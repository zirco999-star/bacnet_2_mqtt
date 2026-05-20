#include "z_mqtt.h"
#include "z_bacnet.h"
#include <string.h>

extern void z_log(const char* format, ...);

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            z_log("[MQTT] Connected to Broker.\n");
            {
                char sub_topic[128];
                snprintf(sub_topic, sizeof(sub_topic), "%s/+/+/+/set", sysCfg.mqtt_prefix);
                esp_mqtt_client_subscribe(mqtt_client, sub_topic, 0);
                z_log("[MQTT] Subscribed to %s\n", sub_topic);
            }
            break;
        case MQTT_EVENT_DISCONNECTED:
            z_log("[MQTT] Disconnected from Broker.\n");
            break;
        case MQTT_EVENT_DATA:
            z_log("[MQTT] Message received on topic: %.*s\n", event->topic_len, event->topic);
            // Parse topic: <prefix>/<mac>/<obj_type>/<instance>/set
            {
                char topic_buf[128];
                if (event->topic_len < sizeof(topic_buf)) {
                    memcpy(topic_buf, event->topic, event->topic_len);
                    topic_buf[event->topic_len] = '\0';
                    
                    char payload_buf[32];
                    int plen = event->data_len < 31 ? event->data_len : 31;
                    memcpy(payload_buf, event->data, plen);
                    payload_buf[plen] = '\0';
                    
                    // Simple parser
                    String t = String(topic_buf);
                    int p1 = t.indexOf('/');
                    int p2 = t.indexOf('/', p1 + 1);
                    int p3 = t.indexOf('/', p2 + 1);
                    int p4 = t.indexOf('/', p3 + 1);
                    if (p1 > 0 && p2 > 0 && p3 > 0 && p4 > 0) {
                        String mac_str = t.substring(p1 + 1, p2);
                        String type_str = t.substring(p2 + 1, p3);
                        String inst_str = t.substring(p3 + 1, p4);
                        
                        BACnetJob job;
                        job.type = JOB_WRITE_PROP;
                        job.target_mac = mac_str.toInt();
                        job.obj_instance = inst_str.toInt();
                        job.prop_id = 85; // Present_Value
                        job.priority = 16;
                        job.write_value = String(payload_buf).toFloat();
                        
                        if (type_str == "analog_output") job.obj_type = OBJ_ANALOG_OUTPUT;
                        else if (type_str == "analog_value") job.obj_type = OBJ_ANALOG_VALUE;
                        else if (type_str == "binary_output") job.obj_type = OBJ_BINARY_OUTPUT;
                        else if (type_str == "binary_value") job.obj_type = OBJ_BINARY_VALUE;
                        else if (type_str == "multi_state_output") job.obj_type = OBJ_MULTI_STATE_OUTPUT;
                        else if (type_str == "multi_state_value") job.obj_type = OBJ_MULTI_STATE_VALUE;
                        else job.type = JOB_WHO_IS; // Ignore
                        
                        if (job.type == JOB_WRITE_PROP) {
                            enqueue_bacnet_job(job);
                            z_log("[MQTT] Enqueued Write MAC %d Obj %d Inst %d Val %.2f\n", job.target_mac, job.obj_type, job.obj_instance, job.write_value);
                        }
                    }
                }
            }
            break;
        default:
            break;
    }
}

void setup_mqtt() {
    if (strlen(sysCfg.mqtt_server) == 0) return;
    
    char uri[128];
    if (strlen(sysCfg.mqtt_user) > 0) {
        snprintf(uri, sizeof(uri), "mqtt://%s:%s@%s:%d", sysCfg.mqtt_user, sysCfg.mqtt_pass, sysCfg.mqtt_server, sysCfg.mqtt_port);
    } else {
        snprintf(uri, sizeof(uri), "mqtt://%s:%d", sysCfg.mqtt_server, sysCfg.mqtt_port);
    }
    
    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = uri;
    
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
    z_log("[MQTT] Client task started.\n");
}

void handle_mqtt() {
    // Process mqtt_publish_queue
    if (mqtt_publish_queue != NULL && mqtt_client != NULL) {
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
            
            char payload[64];
            if (pubJob.data_type == PUB_FLOAT) {
                snprintf(payload, sizeof(payload), "%.2f", pubJob.value_float);
            } else if (pubJob.data_type == PUB_INT) {
                snprintf(payload, sizeof(payload), "%ld", (long)pubJob.value_int);
            } else {
                snprintf(payload, sizeof(payload), "%s", pubJob.value_string);
            }
            
            esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 0);
        }
    }
}
