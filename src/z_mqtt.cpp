#include "z_mqtt.h"
#include "z_bacnet.h"
#include <string.h>
#include <atomic>

extern void z_log(const char* format, ...);

static int mqtt_conn_fail_count = 0;
static std::atomic<bool> mqtt_circuit_open{false};
static bool mqtt_is_connected = false;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            z_log("[MQTT] Connected to Broker.\n");
            mqtt_conn_fail_count = 0; 
            mqtt_circuit_open = false;
            mqtt_is_connected = true;
            {
                char sub_topic[128];
                snprintf(sub_topic, sizeof(sub_topic), "%s/+/+/+/set", sysCfg.mqtt_prefix);
                esp_mqtt_client_subscribe(mqtt_client, sub_topic, 0);
            }
            break;

        case MQTT_EVENT_DISCONNECTED:
            mqtt_is_connected = false;
            // Si le Wi-Fi est là mais qu'on perd le MQTT, on compte les échecs
            if (!mqtt_circuit_open && WiFi.status() == WL_CONNECTED) {
                mqtt_conn_fail_count++;
                z_log("[MQTT] Disconnected (%d/3)\n", mqtt_conn_fail_count);
                if (mqtt_conn_fail_count >= 3) {
                    z_log("[MQTT] CIRCUIT BREAKER TRIPPED: Signaling shutdown to main task.\n");
                    mqtt_circuit_open = true;
                }
            }
            break;

        case MQTT_EVENT_ERROR:
            mqtt_is_connected = false;
            if (WiFi.status() == WL_CONNECTED) {
                z_log("[MQTT] Protocol/Network Error\n");
                if (!mqtt_circuit_open) {
                    mqtt_conn_fail_count++;
                    if (mqtt_conn_fail_count >= 3) {
                        z_log("[MQTT] CIRCUIT BREAKER TRIPPED (Error).\n");
                        mqtt_circuit_open = true;
                    }
                }
            }
            break;

        case MQTT_EVENT_DATA:
            // ... (logique de parsing des messages entrants identique) ...
            {
                char topic_buf[128];
                if (event->topic_len < sizeof(topic_buf)) {
                    memcpy(topic_buf, event->topic, event->topic_len);
                    topic_buf[event->topic_len] = '\0';
                    char payload_buf[32];
                    int plen = event->data_len < 31 ? event->data_len : 31;
                    memcpy(payload_buf, event->data, plen);
                    payload_buf[plen] = '\0';
                    
                    String t = String(topic_buf);
                    int p1 = t.indexOf('/');
                    int p2 = t.indexOf('/', p1 + 1);
                    int p3 = t.indexOf('/', p2 + 1);
                    int p4 = t.indexOf('/', p3 + 1);
                    if (p1 > 0 && p2 > 0 && p3 > 0 && p4 > 0) {
                        BACnetJob job;
                        job.type = JOB_WRITE_PROP;
                        job.target_mac = t.substring(p1 + 1, p2).toInt();
                        job.obj_instance = t.substring(p3 + 1, p4).toInt();
                        job.prop_id = 85;
                        job.write_value = String(payload_buf).toFloat();
                        String type_str = t.substring(p2 + 1, p3);
                        if (type_str == "analog_output") job.obj_type = OBJ_ANALOG_OUTPUT;
                        else if (type_str == "analog_value") job.obj_type = OBJ_ANALOG_VALUE;
                        else if (type_str == "binary_output") job.obj_type = OBJ_BINARY_OUTPUT;
                        else if (type_str == "binary_value") job.obj_type = OBJ_BINARY_VALUE;
                        else job.type = JOB_WHO_IS;
                        
                        if (job.type == JOB_WRITE_PROP) enqueue_bacnet_job(job);
                    }
                }
            }
            break;
        default: break;
    }
}

void setup_mqtt() {
    // 1. Tear-down complet et propre selon les standards Espressif
    if (mqtt_client != NULL) {
        z_log("[MQTT] Stopping and Destroying previous client instance...\n");
        esp_mqtt_client_stop(mqtt_client);
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
    }

    // 2. Réinitialisation des états du Circuit Breaker
    mqtt_conn_fail_count = 0;
    mqtt_circuit_open = false;

    if (strlen(sysCfg.mqtt_server) == 0) return;
    
    static char mqtt_uri[128]; 
    if (strlen(sysCfg.mqtt_user) > 0) {
        snprintf(mqtt_uri, sizeof(mqtt_uri), "mqtt://%s:%s@%s:%d", sysCfg.mqtt_user, sysCfg.mqtt_pass, sysCfg.mqtt_server, sysCfg.mqtt_port);
    } else {
        snprintf(mqtt_uri, sizeof(mqtt_uri), "mqtt://%s:%d", sysCfg.mqtt_server, sysCfg.mqtt_port);
    }
    
    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = mqtt_uri;
    
    // 3. Désactivation de la reconnexion automatique du driver pour laisser le contrôle à l'appli
    mqtt_cfg.network.disable_auto_reconnect = true;
    mqtt_cfg.network.reconnect_timeout_ms = 10000; // 10s entre les tentatives manuelles si besoin

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (mqtt_client) {
        esp_mqtt_client_register_event(mqtt_client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
        esp_mqtt_client_start(mqtt_client);
        z_log("[MQTT] Client initialized (v5.6 - Official Best Practices)\n");
    }
}

bool is_mqtt_connected() { return mqtt_is_connected; }

void handle_mqtt() {
    // Exécution différée de la destruction (Deferred Processing) sur le Core 0
    if (mqtt_circuit_open && mqtt_client != NULL) {
        z_log("[MQTT] ASYNC SHUTDOWN: Stopping and Destroying client to release Core 0 resources.\n");
        esp_mqtt_client_stop(mqtt_client);
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL; // Évite les dangling pointers
        return;
    }

    // Traitement normal des publications si le circuit est fermé
    if (mqtt_publish_queue != NULL && mqtt_client != NULL && !mqtt_circuit_open) {
        MQTTPublishJob pubJob;
        while (xQueueReceive(mqtt_publish_queue, &pubJob, 0) == pdTRUE) {
            char topic[128];
            const char* t_str = "unknown";
            switch(pubJob.obj_type) {
                case OBJ_ANALOG_INPUT: t_str = "analog_input"; break;
                case OBJ_ANALOG_OUTPUT: t_str = "analog_output"; break;
                case OBJ_ANALOG_VALUE: t_str = "analog_value"; break;
                case OBJ_BINARY_INPUT: t_str = "binary_input"; break;
                case OBJ_BINARY_OUTPUT: t_str = "binary_output"; break;
                case OBJ_BINARY_VALUE: t_str = "binary_value"; break;
                case OBJ_MULTI_STATE_INPUT: t_str = "multi_state_input"; break;
                case OBJ_MULTI_STATE_OUTPUT: t_str = "multi_state_output"; break;
                case OBJ_MULTI_STATE_VALUE: t_str = "multi_state_value"; break;
            }
            snprintf(topic, sizeof(topic), "%s/%lu/%s/%lu/state", sysCfg.mqtt_prefix, (unsigned long)pubJob.device_id, t_str, (unsigned long)pubJob.obj_instance);
            esp_mqtt_client_publish(mqtt_client, topic, pubJob.value_string, 0, 1, 0);
        }
    }
}
