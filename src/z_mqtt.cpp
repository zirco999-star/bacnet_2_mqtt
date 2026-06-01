#include "z_mqtt.h"
#include "z_bacnet.h"
#include <string.h>
#include <atomic>
#include <ArduinoJson.h>

extern void z_log(const char* format, ...);

static int mqtt_fail_count = 0;
static std::atomic<bool> circuit_breaker_active{false};
static std::atomic<bool> pending_discovery{false};
static std::atomic<uint32_t> target_did{0};
static std::atomic<uint32_t> target_inst{0};
static std::atomic<uint16_t> target_type{0xFFFF};
static bool mqtt_is_connected = false;
static char lwt_topic[128] = {0};

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
                
                // Signal pour la découverte HA
                pending_discovery = true;
                
                // Publication du statut Online
                if (lwt_topic[0] != 0) {
                    esp_mqtt_client_publish(mqtt_client, lwt_topic, "online", 0, 1, 1);
                }
            }
            break;

        case MQTT_EVENT_ERROR:
        case MQTT_EVENT_DISCONNECTED:
            mqtt_is_connected = false;
            if (circuit_breaker_active) break;
            
            if (WiFi.status() != WL_CONNECTED) {
                z_log("[MQTT] Connection dropped due to Wi-Fi loss.\n");
                break;
            }
            
            if (event->error_handle != NULL) {
                if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                    if (event->error_handle->connect_return_code == MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
                        event->error_handle->connect_return_code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED) {
                        z_log("[MQTT] FATAL: Invalid Broker Credentials.\n");
                        mqtt_fail_count = 3;
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
                        uint32_t device_id = t.substring(p1 + 1, p2).toInt();
                        uint8_t target_mac = 0;
                        bool found = false;

                        if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(50))) {
                            for (auto& d : bacnet_network_cache) {
                                if (d.device_id == device_id) { target_mac = d.mac_address; found = true; break; }
                            }
                            xSemaphoreGive(cache_mutex);
                        }

                        if (found) {
                            BACnetJob job;
                            job.type = JOB_WRITE_PROP;
                            job.target_mac = target_mac;
                            job.obj_instance = t.substring(p3 + 1, p4).toInt();
                            job.prop_id = 85;
                            job.write_value = String(payload_buf).toFloat();
                            String type_str = t.substring(p2 + 1, p3);
                            
                            if (type_str == "AO" || type_str == "AV") job.obj_type = (type_str == "AO") ? 1 : 2;
                            else if (type_str == "BO" || type_str == "BV") job.obj_type = (type_str == "BO") ? 4 : 5;
                            else if (type_str == "MSO" || type_str == "MSV") job.obj_type = (type_str == "MSO") ? 14 : 19;
                            else job.type = JOB_WHO_IS;
                            
                            if (job.type == JOB_WRITE_PROP) enqueue_bacnet_job(job);
                        }
                    }
                }
            }
            break;
        default: break;
    }
}

static void mqtt_gatekeeper_task(void *pv) {
    z_log("[MQTT] Gatekeeper Task (Core 1) Operational.\n");
    uint32_t last_status_pub = 0;

    while (1) {
        if (mqtt_is_connected && !circuit_breaker_active) {
            // 1. Traitement de l'Auto-Discovery
            if (pending_discovery.load(std::memory_order_acquire)) {
                publish_ha_autodiscovery();
                target_did = 0; // Reset pour le prochain scan (soit ciblé, soit global)
                pending_discovery.store(false, std::memory_order_release);
            }

            // 2. Traitement de la queue de publication
            MQTTPublishJob pubJob;
            while (xQueueReceive(mqtt_publish_queue, &pubJob, 0) == pdTRUE) {
                char topic[128];
                const char* t_str = "OBJ";
                switch(pubJob.obj_type) {
                    case 0: t_str = "AI"; break;
                    case 1: t_str = "AO"; break;
                    case 2: t_str = "AV"; break;
                    case 3: t_str = "BI"; break;
                    case 4: t_str = "BO"; break;
                    case 5: t_str = "BV"; break;
                    case 13: t_str = "MSI"; break;
                    case 14: t_str = "MSO"; break;
                    case 19: t_str = "MSV"; break;
                }
                const char* subtopic = (pubJob.prop_id == 77) ? "name" : "state";
                snprintf(topic, sizeof(topic), "%s/%lu/%s/%lu/%s", sysCfg.mqtt_prefix, (unsigned long)pubJob.device_id, t_str, (unsigned long)pubJob.obj_instance, subtopic);
                
                if (esp_mqtt_client_enqueue(mqtt_client, topic, pubJob.value_string, 0, 1, pubJob.retain, true) < 0) {
                    z_log("[MQTT] Queue Full. Back-pressure active.\n");
                    vTaskDelay(pdMS_TO_TICKS(50));
                    break; 
                } else {
                    z_log("[MQTT] Published: %s = %s\n", topic, pubJob.value_string);
                }
                vTaskDelay(pdMS_TO_TICKS(5)); // Pacing pour soulager la pile réseau
            }

            // 3. Status Gateway périodique
            if (millis() - last_status_pub > (sysCfg.mqtt_poll_interval * 1000)) {
                last_status_pub = millis();
                auto pub_b2m = [&](const char* key, String val) {
                    char t[128]; snprintf(t, sizeof(t), "%s/B2M/%s/state", sysCfg.mqtt_prefix, key);
                    esp_mqtt_client_enqueue(mqtt_client, t, val.c_str(), 0, 1, 0, true);
                };
                pub_b2m("ver", VERSION_GLOBAL);
                pub_b2m("rssi", String(WiFi.RSSI()));
                pub_b2m("heap", String(ESP.getFreeHeap() / 1024));
                pub_b2m("nb_dev", String(bacnet_network_cache.size()));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void setup_mqtt() {
    if (mqtt_client != NULL) {
        esp_mqtt_client_stop(mqtt_client);
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
    }

    mqtt_fail_count = 0;
    circuit_breaker_active = false;
    if (strlen(sysCfg.mqtt_server) == 0) return;
    
    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.hostname = sysCfg.mqtt_server;
    mqtt_cfg.broker.address.port = sysCfg.mqtt_port;
    mqtt_cfg.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
    mqtt_cfg.credentials.username = strlen(sysCfg.mqtt_user) > 0 ? sysCfg.mqtt_user : NULL;
    mqtt_cfg.credentials.authentication.password = strlen(sysCfg.mqtt_pass) > 0 ? sysCfg.mqtt_pass : NULL;

    // Configuration LWT
    snprintf(lwt_topic, sizeof(lwt_topic), "tele/%s/LWT", sysCfg.mqtt_prefix);
    mqtt_cfg.session.last_will.topic = lwt_topic;
    mqtt_cfg.session.last_will.msg = "offline";
    mqtt_cfg.session.last_will.qos = 1;
    mqtt_cfg.session.last_will.retain = 1;

    mqtt_cfg.network.disable_auto_reconnect = true;
    mqtt_cfg.outbox.limit = 20480; 
    mqtt_cfg.buffer.out_size = 4096;

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (mqtt_client) {
        esp_mqtt_client_register_event(mqtt_client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
        esp_mqtt_client_start(mqtt_client);
        
        static bool task_created = false;
        if (!task_created) {
            // Migration vers Core 0 (Priorité 5) pour libérer le Core 1 pour le temps-réel BACnet
            xTaskCreatePinnedToCore(mqtt_gatekeeper_task, "MQTT_GK", 8192, NULL, 5, NULL, 0);
            task_created = true;
        }
    }
}

bool is_mqtt_connected() { return mqtt_is_connected; }

void trigger_ha_discovery(uint32_t did, uint32_t inst, uint16_t type) {
    target_did = did;
    target_inst = inst;
    target_type = type;
    pending_discovery = true;
}

void handle_mqtt() {
    static bool was_wifi_connected = false;
    bool is_wifi_connected = (WiFi.status() == WL_CONNECTED);

    if (is_wifi_connected && !was_wifi_connected) {
        if (mqtt_client == NULL && !circuit_breaker_active) setup_mqtt(); 
        else if (mqtt_client != NULL && !circuit_breaker_active) esp_mqtt_client_reconnect(mqtt_client); 
        was_wifi_connected = true;
    } else if (!is_wifi_connected && was_wifi_connected) {
        was_wifi_connected = false;
    }

    if (circuit_breaker_active && mqtt_client != NULL) {
        esp_mqtt_client_stop(mqtt_client);
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
        z_log("[MQTT] Circuit Breaker Active. Client destroyed.\n");
    }
}

void publish_ha_autodiscovery() {
    if (!mqtt_is_connected || circuit_breaker_active) return;
    z_log("[MQTT] Starting HA Auto-Discovery...\n");

    // Étape 1 : Obtenir le nombre de devices sans bloquer longtemps
    size_t dev_count = 0;
    if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(500))) {
        dev_count = bacnet_network_cache.size();
        xSemaphoreGive(cache_mutex);
    }

    for (size_t d = 0; d < dev_count; d++) {
        size_t obj_count = 0;
        
        if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(500))) {
            if (d < bacnet_network_cache.size()) obj_count = bacnet_network_cache[d].objects.size();
            xSemaphoreGive(cache_mutex);
        }

        for (size_t o = 0; o < obj_count; o++) {
            // Variables déclarées en dehors de la zone critique
            String final_payload;
            char final_topic[128] = {0};
            bool should_publish = false;

            // --- DEBUT ZONE CRITIQUE (< 1ms) ---
            if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(500))) {
                
                // Vérification de sécurité
                if (d < bacnet_network_cache.size() && o < bacnet_network_cache[d].objects.size()) {
                    auto& dev = bacnet_network_cache[d];
                    auto& obj = dev.objects[o];

                    // Filtrage ciblé (Optimisation v5.8.1)
                    uint32_t t_did = target_did.load();
                    if (t_did != 0) {
                        if (dev.device_id != t_did || obj.instance != target_inst.load() || obj.type != target_type.load()) {
                            xSemaphoreGive(cache_mutex);
                            continue;
                        }
                    }

                    if (obj.type != 65535) {
                        const char* t_str = "OBJ";
                        const char* ha_component = "sensor";
                        bool is_command = false;
                
                        switch(obj.type) {
                            case OBJ_ANALOG_INPUT: t_str = "AI"; ha_component = "sensor"; break;
                            case OBJ_BINARY_INPUT: t_str = "BI"; ha_component = "binary_sensor"; break;
                            case OBJ_BINARY_OUTPUT: 
                            case OBJ_BINARY_VALUE: t_str = (obj.type == OBJ_BINARY_OUTPUT) ? "BO" : "BV"; ha_component = "switch"; is_command = true; break;
                            case OBJ_ANALOG_OUTPUT:
                            case OBJ_ANALOG_VALUE: t_str = (obj.type == OBJ_ANALOG_OUTPUT) ? "AO" : "AV"; ha_component = "number"; is_command = true; break;
                            case OBJ_MULTI_STATE_INPUT: t_str = "MSI"; ha_component = "sensor"; break;
                            case OBJ_MULTI_STATE_OUTPUT:
                            case OBJ_MULTI_STATE_VALUE: t_str = (obj.type == OBJ_MULTI_STATE_OUTPUT) ? "MSO" : "MSV"; ha_component = "select"; is_command = true; break;
                        }

                        char uniq_id[64];
                        snprintf(uniq_id, sizeof(uniq_id), "bacnet_%lu_%s_%lu", (unsigned long)dev.device_id, t_str, (unsigned long)obj.instance);
                        snprintf(final_topic, sizeof(final_topic), "homeassistant/%s/%s/config", ha_component, uniq_id);

                        if (obj.enabled) {
                            JsonDocument doc; 
                            char base_topic[128];
                            snprintf(base_topic, sizeof(base_topic), "%s/%lu/%s/%lu", sysCfg.mqtt_prefix, (unsigned long)dev.device_id, t_str, (unsigned long)obj.instance);
                            
                            doc["~"] = base_topic;
                            doc["uniq_id"] = uniq_id;
                            doc["name"] = obj.name;
                            doc["stat_t"] = "~/state";
                            doc["avty_t"] = lwt_topic;
                            doc["pl_avail"] = "online";
                            doc["pl_not_avail"] = "offline";

                            if (is_command) doc["cmd_t"] = "~/set";

                            if (obj.type == OBJ_BINARY_INPUT || obj.type == OBJ_BINARY_OUTPUT || obj.type == OBJ_BINARY_VALUE) {
                                doc["pl_on"] = "1.00";
                                doc["pl_off"] = "0.00";
                            }

                            if (strcmp(ha_component, "number") == 0) {
                                doc["min"] = -100; doc["max"] = 1000; doc["step"] = 0.1;
                            }

                            if (obj.type == OBJ_ANALOG_INPUT || obj.type == OBJ_ANALOG_VALUE || obj.type == OBJ_ANALOG_OUTPUT) {
                                String unit = String(obj.unit_text);
                                if (unit == "Unknown" || unit.length() == 0) {
                                    unit = get_unit_text(obj.units);
                                }
                                
                                // Nettoyage pour HA
                                if (unit == "no-units" || unit == "none") unit = "";

                                if (unit.length() > 0) {
                                    doc["unit_of_meas"] = unit;
                                    if (unit == "°C" || unit == "°F" || unit == "°K") doc["dev_cla"] = "temperature";
                                    else if (unit == "%" || unit == "%RH") doc["dev_cla"] = "humidity";
                                    else if (unit == "Pa" || unit == "kPa" || unit == "bar" || unit == "psi") doc["dev_cla"] = "pressure";
                                    else if (unit == "kW" || unit == "W" || unit == "MW") doc["dev_cla"] = "power";
                                    else if (unit == "kWh" || unit == "Wh" || unit == "MWh") doc["dev_cla"] = "energy";
                                    else if (unit == "V" || unit == "mV") doc["dev_cla"] = "voltage";
                                    else if (unit == "A" || unit == "mA") doc["dev_cla"] = "current";
                                }
                            }

                            if (obj.type == OBJ_MULTI_STATE_INPUT || obj.type == OBJ_MULTI_STATE_OUTPUT || obj.type == OBJ_MULTI_STATE_VALUE) {
                                JsonArray opts = doc["options"].to<JsonArray>();
                                String jinja_list = "[";
                                for (size_t i = 0; i < obj.state_texts.size(); i++) {
                                    opts.add(obj.state_texts[i]);
                                    jinja_list += "'" + obj.state_texts[i] + "'";
                                    if (i < obj.state_texts.size() - 1) jinja_list += ",";
                                }
                                jinja_list += "]";
                                doc["val_tpl"] = "{% set o = " + jinja_list + " %} {{ o[value|int - 1] if value|int > 0 else 'Unknown' }}";
                                if (is_command) {
                                    doc["cmd_tpl"] = "{% set o = " + jinja_list + " %} {{ o.index(value) + 1 }}";
                                }
                            }

                            JsonObject device = doc["dev"].to<JsonObject>();
                            JsonArray ids = device["ids"].to<JsonArray>();
                            char dev_id_str[32]; 
                            snprintf(dev_id_str, sizeof(dev_id_str), "bacnet_dev_%lu", (unsigned long)dev.device_id);
                            ids.add(dev_id_str);
                            device["name"] = dev.name.length() > 0 ? dev.name : "BACnet Device";
                            device["mf"] = dev.vendor.length() > 0 ? dev.vendor : "BACnet Manufacturer";
                            device["sw"] = VERSION_GLOBAL;

                            serializeJson(doc, final_payload);
                        } else {
                            final_payload = ""; // Suppression HA (Payload vide)
                        }
                        should_publish = true;
                    }
                }
                
                // On rend le Mutex ICI, tant qu'on est sûr de l'avoir pris
                // et AVANT de faire le réseau et le délai.
                xSemaphoreGive(cache_mutex);
            } 
            // --- FIN ZONE CRITIQUE ---

            // --- EXECUTION RESEAU ET TEMPORISATION (HORS MUTEX) ---
            if (should_publish) { 
                // Note : On publie même si le payload est vide (cas de suppression HA)
                esp_mqtt_client_enqueue(mqtt_client, final_topic, final_payload.c_str(), 0, 1, 1, true);
                vTaskDelay(pdMS_TO_TICKS(50)); 
            }
        }
    }
    
    // Le log de fin est placé tout à la fin de la fonction
    z_log("[MQTT] HA Auto-Discovery payload sent.\n");
}

void publish_mqtt_topic(uint32_t device_id, BACnetObject& obj, uint8_t prop_id, bool retain) {
    if (!obj.enabled) return;
    MQTTPublishJob pub;
    pub.device_id = device_id;
    pub.obj_type = obj.type;
    pub.obj_instance = obj.instance;
    pub.prop_id = prop_id;
    pub.retain = retain;

    if (prop_id == 77) {
        if (strlen(obj.name) == 0) return;
        strlcpy(pub.value_string, obj.name, sizeof(pub.value_string));
    } else if (prop_id == 85) {
        snprintf(pub.value_string, sizeof(pub.value_string), "%.2f", obj.present_value);
    } else return;

    enqueue_mqtt_publish(pub);
}
