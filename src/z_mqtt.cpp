#include "z_mqtt.h"
#include "z_bacnet.h"
#include <string.h>
#include <atomic>

extern void z_log(const char* format, ...);

static int mqtt_fail_count = 0;
static std::atomic<bool> circuit_breaker_active{false};
static bool mqtt_is_connected = false;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            z_log("[MQTT] Connected to Broker.\n");
            mqtt_fail_count = 0; 
            circuit_breaker_active = false;
            mqtt_is_connected = true;
            {
                char sub_topic[128];
                snprintf(sub_topic, sizeof(sub_topic), "%s/+/+/+/set", sysCfg.mqtt_prefix);
                esp_mqtt_client_subscribe(mqtt_client, sub_topic, 0);
            }
            break;

        case MQTT_EVENT_ERROR:
        case MQTT_EVENT_DISCONNECTED:
            mqtt_is_connected = false;
            if (circuit_breaker_active) break;
            
            // Ne pas pénaliser le broker si c'est la couche Wi-Fi qui est tombée
            if (WiFi.status() != WL_CONNECTED) {
                z_log("[MQTT] Connection dropped due to Wi-Fi loss. Waiting for link...\n");
                break;
            }
            
            // Analyse granulaire de l'erreur via l'API esp-mqtt
            if (event->error_handle != NULL) {
                if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                    if (event->error_handle->connect_return_code == MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
                        event->error_handle->connect_return_code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED) {
                        z_log("[MQTT] FATAL: Invalid Broker Credentials.\n");
                        mqtt_fail_count = 3; // On force le disjoncteur immédiatement
                    }
                }
            }
            
            mqtt_fail_count++;
            z_log("[MQTT] Disconnected from Broker (%d/3).\n", mqtt_fail_count);
            
            if (mqtt_fail_count >= 3) {
                z_log("[MQTT] CIRCUIT BREAKER: Halting MQTT connection attempts.\n");
                circuit_breaker_active = true;
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
                        if (type_str == "AO") job.obj_type = OBJ_ANALOG_OUTPUT;
                        else if (type_str == "AV") job.obj_type = OBJ_ANALOG_VALUE;
                        else if (type_str == "BO") job.obj_type = OBJ_BINARY_OUTPUT;
                        else if (type_str == "BV") job.obj_type = OBJ_BINARY_VALUE;
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
    mqtt_fail_count = 0;
    circuit_breaker_active = false;

    if (strlen(sysCfg.mqtt_server) == 0) return;
    
    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.hostname = sysCfg.mqtt_server;
    mqtt_cfg.broker.address.port = sysCfg.mqtt_port;
    mqtt_cfg.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
    
    mqtt_cfg.credentials.username = strlen(sysCfg.mqtt_user) > 0 ? sysCfg.mqtt_user : NULL;
    mqtt_cfg.credentials.authentication.password = strlen(sysCfg.mqtt_pass) > 0 ? sysCfg.mqtt_pass : NULL;

    z_log("[MQTT] Attempting to connect to Broker at %s:%d with credentials '%s' / '%s'\n", sysCfg.mqtt_server, sysCfg.mqtt_port, sysCfg.mqtt_user, sysCfg.mqtt_pass);      

    // 3. Désactivation de la reconnexion automatique du driver pour laisser le contrôle à l'appli
    mqtt_cfg.network.disable_auto_reconnect = true;
    mqtt_cfg.outbox.limit = 15360; // 15KB Outbox for bursts (validated v5.6.6)
    mqtt_cfg.buffer.out_size = 2048;

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (mqtt_client) {
        esp_mqtt_client_register_event(mqtt_client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
        esp_mqtt_client_start(mqtt_client);
        z_log("[MQTT] Client initialized -%s-\n", VERSION_GLOBAL);    
    }
}

bool is_mqtt_connected() { return mqtt_is_connected; }

void handle_mqtt() {
    static bool was_wifi_connected = false;
    bool is_wifi_connected = (WiFi.status() == WL_CONNECTED);

    // 1. Détection du front montant du Wi-Fi (Connexion ou Reconnexion)
    if (is_wifi_connected && !was_wifi_connected) {
        z_log("[MQTT] Wi-Fi Link Up. Starting/Reconnecting MQTT...\n");
        
        if (mqtt_client == NULL && !circuit_breaker_active) {
            setup_mqtt(); 
        } else if (mqtt_client != NULL && !circuit_breaker_active) {
            esp_mqtt_client_reconnect(mqtt_client); 
        }
        was_wifi_connected = true;
    } else if (!is_wifi_connected && was_wifi_connected) {
        z_log("[MQTT] Wi-Fi Link Down. Suspending MQTT traffic.\n");
        was_wifi_connected = false;
    }

    // 2. Exécution différée de la destruction (Deferred Processing) sur le Core 0
    if (circuit_breaker_active && mqtt_client != NULL) {
        esp_mqtt_client_stop(mqtt_client);
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL; // Évite les dangling pointers
        z_log("[MQTT] Client fully destroyed to protect Core 0 LwIP.\n");
        return;
    }

    // 3. Traitement normal des publications si le circuit est fermé et Wi-Fi UP
    if (mqtt_publish_queue != NULL && mqtt_client != NULL && is_wifi_connected && !circuit_breaker_active) {
        MQTTPublishJob pubJob;
        while (xQueueReceive(mqtt_publish_queue, &pubJob, 0) == pdTRUE) {
            char topic[128];
            const char* t_str = "unknown";
            switch(pubJob.obj_type) {
                case OBJ_ANALOG_INPUT: t_str = "AI"; break;
                case OBJ_ANALOG_OUTPUT: t_str = "AO"; break;
                case OBJ_ANALOG_VALUE: t_str = "AV"; break;
                case OBJ_BINARY_INPUT: t_str = "BI"; break;
                case OBJ_BINARY_OUTPUT: t_str = "BO"; break;
                case OBJ_BINARY_VALUE: t_str = "BV"; break;
                case OBJ_MULTI_STATE_INPUT: t_str = "MSI"; break;
                case OBJ_MULTI_STATE_OUTPUT: t_str = "MSO"; break;
                case OBJ_MULTI_STATE_VALUE: t_str = "MSV"; break;
            }
            snprintf(topic, sizeof(topic), "%s/%lu/%s/%lu/state", sysCfg.mqtt_prefix, (unsigned long)pubJob.device_id, t_str, (unsigned long)pubJob.obj_instance);
            
            int msg_id = esp_mqtt_client_enqueue(mqtt_client, topic, pubJob.value_string, 0, 0, 0, true);
            if (msg_id == -2) {
                z_log("[MQTT] WARN: Outbox is full. Message dropped.\n");
            } else {
                z_log("[MQTT] Published: %s -> %s\n", topic, pubJob.value_string);
            }
        }
    }
}
