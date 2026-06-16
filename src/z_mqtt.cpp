#include "z_mqtt.h"
#include "z_bacnet.h"
#include <string.h>
#include <atomic>
#include <ArduinoJson.h>

#include "z_network.h"
#include "z_nvs.h"

uint32_t period_mqtt_pub_count = 0;
uint32_t period_mqtt_obj_count = 0;
uint32_t period_mqtt_b2m_count = 0;
uint32_t period_mqtt_tele_count = 0;

static int mqtt_fail_count = 0;
static std::atomic<bool> circuit_breaker_active{false};
static std::atomic<bool> pending_discovery{false};
static std::atomic<uint32_t> target_did{0};
static std::atomic<uint32_t> target_inst{0};
static std::atomic<uint16_t> target_type{0xFFFF};
static bool mqtt_is_connected = false;
static bool force_full_discovery = false; 
static std::atomic<bool> mqtt_pending_connected_log{false};
static std::atomic<bool> mqtt_pending_disconnected_log{false};
static std::atomic<bool> mqtt_pending_auth_error_log{false};
static std::atomic<bool> mqtt_pending_breaker_log{false};
static std::atomic<bool> mqtt_pending_wifi_loss_log{false};
static char lwt_topic[128] = {0};

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            mqtt_pending_connected_log = true;
            mqtt_fail_count = 0; 
            circuit_breaker_active = false;
            mqtt_is_connected = true;
            {
                char sub_topic[128];
                snprintf(sub_topic, sizeof(sub_topic), "%s/+/+/+/set", sysCfg.mqtt_prefix);
                esp_mqtt_client_subscribe(mqtt_client, sub_topic, 0);
                
                // Signal pour la découverte HA complète suite à reconnexion
                force_full_discovery = true; 
                pending_discovery = true;
                
                // Publication du statut Online
                if (lwt_topic[0] != 0) {
                    esp_mqtt_client_publish(mqtt_client, lwt_topic, "online", 0, 1, 1);
                    period_mqtt_tele_count++;
                }
            }
            break;

        case MQTT_EVENT_ERROR:
        case MQTT_EVENT_DISCONNECTED:
            mqtt_is_connected = false;
            if (circuit_breaker_active) break;

            if (WiFi.status() != WL_CONNECTED) {
                mqtt_pending_wifi_loss_log = true;
                break;
            }

            if (event->error_handle != NULL) {
                if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                    if (event->error_handle->connect_return_code == MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
                        event->error_handle->connect_return_code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED) {
                        mqtt_pending_auth_error_log = true;
                        mqtt_fail_count = 3;
                    }
                }
            }

            mqtt_fail_count++;
            mqtt_pending_disconnected_log = true;

            if (mqtt_fail_count >= 3) {
                mqtt_pending_breaker_log = true;
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
                        uint32_t ulDeviceId = t.substring(p1 + 1, p2).toInt();
                        uint8_t target_mac = 0;
                        bool found = false;

                        if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(100))) {
                            for (auto& d : bacnet_network_cache) {
                                if (d.ulDeviceId == ulDeviceId) { 
                                    target_mac = d.ucMacAddress; 
                                    found = true; 
                                    
                                    BACnetJob job;
                                    job.type = JOB_WRITE_PROP;
                                    job.target_mac = target_mac;
                                    job.obj_instance = t.substring(p3 + 1, p4).toInt();
                                    job.prop_id = 85;
                                    String type_str = t.substring(p2 + 1, p3);
                                    
                                    if (type_str == "AO" || type_str == "AV") job.obj_type = (type_str == "AO") ? 1 : 2;
                                    else if (type_str == "BO" || type_str == "BV") job.obj_type = (type_str == "BO") ? 4 : 5;
                                    else if (type_str == "MSO" || type_str == "MSV") job.obj_type = (type_str == "MSO") ? 14 : 19;
                                    else { job.type = JOB_WHO_IS; found = false; }

                                    if (found) {
                                        if (job.obj_type == 14 || job.obj_type == 19) {
                                            // Conversion Texte -> Index pour Multi-State
                                            bool text_found = false;
                                            for (auto& o : d.objects) {
                                                if (o.usType == job.obj_type && o.ulInstance == job.obj_instance) {
                                                    for (size_t i = 0; i < o.state_texts.size(); i++) {
                                                        if (o.state_texts[i] == String(payload_buf)) {
                                                            job.write_value = (float)(i + 1);
                                                            text_found = true;
                                                            break;
                                                        }
                                                    }
                                                    break;
                                                }
                                            }
                                            if (!text_found) job.write_value = String(payload_buf).toFloat();
                                        } else {
                                            job.write_value = String(payload_buf).toFloat();
                                        }
                                        enqueue_bacnet_job(job);
                                    }
                                    break; 
                                }
                            }
                            xSemaphoreGive(cache_mutex);
                        }
                    }
                }
            }
            break;
        default: break;
    }
}

static void mqtt_gatekeeper_task(void *pv) {
    z_log(pdLOG_INFO, "MQTT", "Gatekeeper Task Operational.\n");
    uint32_t last_status_pub = 0;
    uint32_t last_global_disc = 0;
    uint32_t last_single_disc = 0;

    while (1) {
        uint32_t now = millis();

        // Gestion du Lazy Save NVS (déporté sur Core 0 pour éviter de geler la FSM MS/TP sur Core 1)
        std::vector<uint32_t> dirty_dids;
        if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(100))) {
            for (auto& dev : bacnet_network_cache) {
                if (dev.xDirty && (now - dev.ulLastDirtyTime > 2000)) {
                    dirty_dids.push_back(dev.ulDeviceId);
                    dev.xDirty = false; // Reset to avoid double trigger
                }
            }
            xSemaphoreGive(cache_mutex);
        }

        for (uint32_t did : dirty_dids) {
            save_device_objects_locked(did);
        }

        // 1. Logs d'événements déportés
        if (mqtt_pending_connected_log.exchange(false)) z_log(pdLOG_INFO, "MQTT", "Connected to Broker.\n");
        if (mqtt_pending_disconnected_log.exchange(false)) z_log(pdLOG_WARN, "MQTT", "Disconnected from Broker (%d/3).\n", mqtt_fail_count);
        if (mqtt_pending_auth_error_log.exchange(false)) z_log(pdLOG_ERROR, "MQTT", "FATAL: Invalid Broker Credentials.\n");
        if (mqtt_pending_wifi_loss_log.exchange(false)) z_log(pdLOG_WARN, "MQTT", "Connection dropped due to Wi-Fi loss.\n");
        if (mqtt_pending_breaker_log.exchange(false)) z_log(pdLOG_ERROR, "MQTT", "CIRCUIT BREAKER: Halting MQTT connection attempts.\n");

        if (mqtt_is_connected && !circuit_breaker_active) {
            
            // 2. Déclenchement Découverte HA (Synchrone v6.8.15)
            if (pending_discovery.load(std::memory_order_acquire)) {
                uint32_t t_did = target_did.load();
                bool is_global = (t_did == 0 || force_full_discovery);
                bool can_start = is_global ? (now - last_global_disc > 30000) : (now - last_single_disc > 5000);

                if (can_start) {
                    if (is_global) last_global_disc = now; else last_single_disc = now;
                    publish_ha_autodiscovery(target_did.load(), target_inst.load(), target_type.load());
                    pending_discovery.store(false, std::memory_order_release);
                    force_full_discovery = false;
                }
            }

            // 3. Traitement de la queue de publication
            MQTTPublishJob pubJob;
            while (xQueueReceive(mqtt_publish_queue, &pubJob, 0) == pdTRUE) {
                char topic[128];
                const char* t_str = "OBJ";
                switch(pubJob.obj_type) {
                    case OBJ_ANALOG_INPUT:       t_str = "AI"; break;
                    case OBJ_ANALOG_OUTPUT:      t_str = "AO"; break;
                    case OBJ_ANALOG_VALUE:       t_str = "AV"; break;
                    case OBJ_BINARY_INPUT:       t_str = "BI"; break;
                    case OBJ_BINARY_OUTPUT:      t_str = "BO"; break;
                    case OBJ_BINARY_VALUE:       t_str = "BV"; break;
                    case OBJ_MULTI_STATE_INPUT:  t_str = "MSI"; break;
                    case OBJ_MULTI_STATE_OUTPUT: t_str = "MSO"; break;
                    case OBJ_MULTI_STATE_VALUE:  t_str = "MSV"; break;
                }
                const char* subtopic = (pubJob.prop_id == 77) ? "name" : "state";
                snprintf(topic, sizeof(topic), "%s/%lu/%s/%lu/%s", sysCfg.mqtt_prefix, (unsigned long)pubJob.ulDeviceId, t_str, (unsigned long)pubJob.obj_instance, subtopic);
                
                if (esp_mqtt_client_publish(mqtt_client, topic, pubJob.value_string, 0, 1, pubJob.retain) < 0) {
                    z_log(pdLOG_WARN, "MQTT", "Publish failed. Queue Full or Client error.\n");
                    vTaskDelay(pdMS_TO_TICKS(50));
                    break;
                } else {
                    period_mqtt_pub_count++;
                    period_mqtt_obj_count++;
                    z_log(pdLOG_DEBUG, "MQTT", "Published: %s = %s\n", topic, pubJob.value_string);
                }
                vTaskDelay(pdMS_TO_TICKS(5));
            }

            // 4. Status Gateway périodique
            if (millis() - last_status_pub > (sysCfg.mqtt_poll_interval * 1000)) {
                last_status_pub = millis();
                auto pub_b2m = [&](const char* key, String val) {
                    char t[128]; snprintf(t, sizeof(t), "%s/B2M/%s/state", sysCfg.mqtt_prefix, key);
                    esp_mqtt_client_publish(mqtt_client, t, val.c_str(), 0, 1, 0);
                    period_mqtt_pub_count++;
                    period_mqtt_b2m_count++;
                    z_log(pdLOG_DEBUG, "MQTT", "Published: %s = %s\n", t, val.c_str());
                };
                pub_b2m("ver", configVERSION_GLOBAL);
                pub_b2m("rssi", String(WiFi.RSSI()));
                pub_b2m("heap", String(ESP.getFreeHeap() / 1024));
                pub_b2m("min_heap", String(ESP.getMinFreeHeap() / 1024));
                pub_b2m("uptime", String(millis() / 1000));

                size_t n_dev = 0;
                if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(100))) {
                    n_dev = bacnet_network_cache.size();
                    xSemaphoreGive(cache_mutex);
                }
                pub_b2m("nb_dev", String(n_dev));

                // Température interne ESP32-S3
                pub_b2m("temp", String(temperatureRead(), 1));

                // Santé du réseau MS/TP (basé sur le mouvement des jetons)
                static uint32_t last_token_count = 0;
                bool mstp_active = (bacnetStats.ulTokensSeen != last_token_count);
                last_token_count = bacnetStats.ulTokensSeen;
                pub_b2m("mstp", mstp_active ? "ON" : "OFF");

                z_log(pdLOG_INFO, "MQTT", "Gateway Status published (Messages: %lu, Devices: %zu)\n", (unsigned long)period_mqtt_pub_count, n_dev);
                z_log(pdLOG_INFO, "MQTT", "Published topics : %s/+ : %lu msg, %s/B2M : %lu msg, tele/%s : %lu msg\n", 
                      sysCfg.mqtt_prefix, (unsigned long)period_mqtt_obj_count, 
                      sysCfg.mqtt_prefix, (unsigned long)period_mqtt_b2m_count,
                      sysCfg.mqtt_prefix, (unsigned long)period_mqtt_tele_count);

                period_mqtt_pub_count = 0;
                period_mqtt_obj_count = 0;
                period_mqtt_b2m_count = 0;
                period_mqtt_tele_count = 0;
                }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

QueueHandle_t mqtt_publish_queue = NULL;

bool enqueue_mqtt_publish(MQTTPublishJob pubJob) { if (mqtt_publish_queue == NULL) return false; return xQueueSend(mqtt_publish_queue, &pubJob, 0) == pdTRUE; }

void init_mqtt_queue() {
    if (mqtt_publish_queue == NULL) {
        mqtt_publish_queue = xQueueCreate(100, sizeof(MQTTPublishJob));
        z_log(pdLOG_INFO, "MQTT", "Queue Initialized\n");
    }
}

void setup_mqtt() {
    init_mqtt_queue();
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
            // Migration vers Core 0 (Priorité 5). Pile augmentée à 16KB pour HA Discovery complexe.
            xTaskCreatePinnedToCore(mqtt_gatekeeper_task, "MQTT_GK", 16384, NULL, 5, NULL, 0);
            task_created = true;
        }
    }
}

bool is_mqtt_connected() { return mqtt_is_connected; }

void trigger_ha_discovery(uint32_t did, uint32_t inst, uint16_t type) {
    if (!sysCfg.ha_discover) return;
    
    // Stratégie de Coalescence (v6.4.2)
    if (pending_discovery.load()) {
        if (target_did != did) {
            target_did = 0;
            target_inst = 0xFFFFFFFF;
        } else if (target_inst != inst || target_type != type) {
            target_inst = 0xFFFFFFFF;
            target_type = 0xFFFF;
        }
    } else {
        target_did = did;
        target_inst = inst;
        target_type = type;
    }
    
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
        z_log(pdLOG_ERROR, "MQTT", "Circuit Breaker Active. Client destroyed.\n");
    }
}

void publish_ha_autodiscovery(uint32_t t_did, uint32_t t_inst, uint16_t t_type) {
    if (!mqtt_is_connected || circuit_breaker_active || !sysCfg.ha_discover) return;

    bool is_single_object = (t_did != 0 && t_inst != 0xFFFFFFFF);
    
    if (!is_single_object) {
        z_log(pdLOG_INFO, "MQTT", "Starting HA Auto-Discovery%s...\n", (t_did != 0) ? " (Single Device)" : "");
    }

    // Étape 0 : Discovery de la Gateway
    if (!is_single_object && t_did == 0) {
        char base_b2m[128];
        snprintf(base_b2m, sizeof(base_b2m), "%s/B2M", sysCfg.mqtt_prefix);
        
        auto pub_gw_sensor = [&](const char* key, const char* name, const char* dev_cla, const char* unit, const char* icon = NULL, bool is_binary = false) {
            JsonDocument doc;
            char topic[128], uniq[64];
            snprintf(uniq, sizeof(uniq), "b2m_gw_%s", key);
            snprintf(topic, sizeof(topic), "homeassistant/%s/%s/config", is_binary ? "binary_sensor" : "sensor", uniq);
            
            doc["name"] = name;
            doc["uniq_id"] = uniq;
            char stat_t[128]; snprintf(stat_t, sizeof(stat_t), "%s/%s/state", base_b2m, key);
            doc["stat_t"] = stat_t;
            doc["avty_t"] = lwt_topic;
            
            if (dev_cla) doc["dev_cla"] = dev_cla;
            if (unit) doc["unit_of_meas"] = unit;
            if (icon) doc["icon"] = icon;
            if (is_binary) {
                doc["pl_on"] = "ON";
                doc["pl_off"] = "OFF";
            }
            
            JsonObject device = doc["dev"].to<JsonObject>();
            JsonArray ids = device["ids"].to<JsonArray>();
            ids.add("b2m_gateway");
            device["name"] = "BACnet2MQTT Gateway";
            device["mf"] = "Custom";
            device["mdl"] = "ESP32-S3";
            device["sw"] = configVERSION_GLOBAL;

            String payload;
            serializeJson(doc, payload);
            esp_mqtt_client_publish(mqtt_client, topic, payload.c_str(), 0, 1, 1);
        };

        pub_gw_sensor("ver", "Gateway Version", NULL, NULL, "mdi:information-outline");
        pub_gw_sensor("uptime", "Gateway Uptime", "duration", "s", "mdi:timer-outline");
        pub_gw_sensor("rssi", "Gateway WiFi RSSI", "signal_strength", "dBm");
        pub_gw_sensor("heap", "Gateway Free Heap", "data_size", "KB", "mdi:memory");
        pub_gw_sensor("min_heap", "Gateway Min Heap", "data_size", "KB", "mdi:memory");
        pub_gw_sensor("temp", "Gateway Chip Temp", "temperature", "°C");
        pub_gw_sensor("nb_dev", "Gateway Devices Count", NULL, "dev", "mdi:counter");
        pub_gw_sensor("mstp", "Gateway MS/TP Network", "connectivity", NULL, NULL, true);

        vTaskDelay(pdMS_TO_TICKS(100)); 
    }

    // Étape 1 : Parcours sécurisé (Séquentiel par Mutex court)
    size_t nb_devices = 0;
    if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(1000))) {
        nb_devices = bacnet_network_cache.size();
        xSemaphoreGive(cache_mutex);
    }

    int total_published = 0;

    for (size_t d_idx = 0; d_idx < nb_devices; d_idx++) {
        size_t nb_objects = 0;
        uint32_t current_did = 0;
        String dev_name, dev_vendor;
        bool dev_enabled = false, dev_discovery_done = false;

        // Snapshot du device
        if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(200))) {
            if (d_idx < bacnet_network_cache.size()) {
                auto& dev = bacnet_network_cache[d_idx];
                if (t_did != 0 && dev.ulDeviceId != t_did) { xSemaphoreGive(cache_mutex); continue; }
                current_did = dev.ulDeviceId;
                dev_name = dev.name;
                dev_vendor = dev.vendor;
                dev_enabled = dev.xEnabled;
                dev_discovery_done = dev.xDiscoveryDone;
                nb_objects = dev.objects.size();
            }
            xSemaphoreGive(cache_mutex);
        } else continue;

        if (current_did == 0) continue;

        for (size_t o_idx = 0; o_idx < nb_objects; o_idx++) {
            // Snapshot de l'objet
            uint16_t obj_type = 65535;
            uint32_t obj_inst = 0;
            char obj_name[64] = {0};
            char obj_unit_text[32] = {0};
            uint16_t obj_units = 0;
            float obj_min = NAN, obj_max = NAN, obj_step = 0.1f;
            char obj_min_ref[6] = {0}, obj_max_ref[6] = {0};
            bool obj_enabled = false, obj_commandable = false;
            std::vector<String> obj_states;

            if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(200))) {
                if (d_idx < bacnet_network_cache.size() && o_idx < bacnet_network_cache[d_idx].objects.size()) {
                    auto& obj = bacnet_network_cache[d_idx].objects[o_idx];
                    if (is_single_object && (obj.ulInstance != t_inst || obj.usType != t_type)) { xSemaphoreGive(cache_mutex); continue; }
                    obj_type = obj.usType;
                    obj_inst = obj.ulInstance;
                    strlcpy(obj_name, obj.cName, sizeof(obj_name));
                    strlcpy(obj_unit_text, obj.cUnitText, sizeof(obj_unit_text));
                    obj_units = obj.usUnits;
                    obj_min = obj.fMinValue;
                    obj_max = obj.fMaxValue;
                    obj_step = obj.fStepValue;
                    strlcpy(obj_min_ref, obj.cMinRef, sizeof(obj_min_ref));
                    strlcpy(obj_max_ref, obj.cMaxRef, sizeof(obj_max_ref));
                    obj_enabled = obj.xEnabled;
                    obj_commandable = obj.xIsCommandable;
                    obj_states = obj.state_texts; 
                }
                xSemaphoreGive(cache_mutex);
            } else continue;

            if (obj_type == 65535) continue;

            const char* t_str = "OBJ";
            const char* ha_component = "sensor";

            switch(obj_type) {
                case OBJ_ANALOG_INPUT:       t_str = "AI"; ha_component = "sensor"; break;
                case OBJ_ANALOG_OUTPUT:      t_str = "AO"; ha_component = (obj_commandable) ? "number" : "sensor"; break;
                case OBJ_ANALOG_VALUE:       t_str = "AV"; ha_component = (obj_commandable) ? "number" : "sensor"; break;
                case OBJ_BINARY_INPUT:       t_str = "BI"; ha_component = "binary_sensor"; break;
                case OBJ_BINARY_OUTPUT:      t_str = "BO"; ha_component = (obj_commandable) ? "switch" : "binary_sensor"; break;
                case OBJ_BINARY_VALUE:       t_str = "BV"; ha_component = (obj_commandable) ? "switch" : "binary_sensor"; break;
                case OBJ_MULTI_STATE_INPUT:  t_str = "MSI"; ha_component = "sensor"; break;
                case OBJ_MULTI_STATE_OUTPUT: t_str = "MSO"; ha_component = (obj_states.empty()) ? ((obj_commandable) ? "number" : "sensor") : "select"; break;
                case OBJ_MULTI_STATE_VALUE:  t_str = "MSV"; ha_component = (obj_states.empty()) ? ((obj_commandable) ? "number" : "sensor") : "select"; break;
            }

            char uniq_id[64];
            snprintf(uniq_id, sizeof(uniq_id), "bacnet_%lu_%s_%lu", (unsigned long)current_did, t_str, (unsigned long)obj_inst);
            char topic[128];
            snprintf(topic, sizeof(topic), "homeassistant/%s/%s/config", ha_component, uniq_id);

            if (dev_enabled && obj_enabled && strcmp(obj_name, "Unknown") != 0 && (dev_discovery_done || is_single_object)) {
                JsonDocument doc; 
                char base_topic[128];
                snprintf(base_topic, sizeof(base_topic), "%s/%lu/%s/%lu", sysCfg.mqtt_prefix, (unsigned long)current_did, t_str, (unsigned long)obj_inst);
                
                doc["~"] = String(base_topic);
                doc["uniq_id"] = String(uniq_id);
                doc["name"] = String(obj_name); 
                doc["stat_t"] = "~/state";
                doc["avty_t"] = String(lwt_topic);
                doc["pl_avail"] = "online";
                doc["pl_not_avail"] = "offline";

                if (strcmp(ha_component, "sensor") != 0 && strcmp(ha_component, "binary_sensor") != 0) {
                    doc["cmd_t"] = "~/set";
                }

                if (strcmp(ha_component, "binary_sensor") == 0 || strcmp(ha_component, "switch") == 0) {
                    doc["pl_on"] = "1.00"; doc["pl_off"] = "0.00";
                }

                if (strcmp(ha_component, "number") == 0) {
                    float final_min = isnan(obj_min) ? sysCfg.default_number_min : obj_min;
                    float final_max = isnan(obj_max) ? sysCfg.default_number_max : obj_max;
                    
                    if (strlen(obj_min_ref) > 0) {
                        uint16_t ref_type = 65535; uint32_t ref_inst = 0;
                        if (strncmp(obj_min_ref, "AI:", 3) == 0) { ref_type = 0; ref_inst = atoi(obj_min_ref + 3); }
                        else if (strncmp(obj_min_ref, "AV:", 3) == 0) { ref_type = 2; ref_inst = atoi(obj_min_ref + 3); }
                        if (ref_type != 65535) {
                            if (d_idx < bacnet_network_cache.size()) {
                                for (auto& ro : bacnet_network_cache[d_idx].objects) {
                                    if (ro.usType == ref_type && ro.ulInstance == ref_inst) {
                                        final_min = ro.fPresentValue; break;
                                    }
                                }
                            }
                        }
                    }
                    if (strlen(obj_max_ref) > 0) {
                        uint16_t ref_type = 65535; uint32_t ref_inst = 0;
                        if (strncmp(obj_max_ref, "AI:", 3) == 0) { ref_type = 0; ref_inst = atoi(obj_max_ref + 3); }
                        else if (strncmp(obj_max_ref, "AV:", 3) == 0) { ref_type = 2; ref_inst = atoi(obj_max_ref + 3); }
                        if (ref_type != 65535) {
                            if (d_idx < bacnet_network_cache.size()) {
                                for (auto& ro : bacnet_network_cache[d_idx].objects) {
                                    if (ro.usType == ref_type && ro.ulInstance == ref_inst) {
                                        final_max = ro.fPresentValue; break;
                                    }
                                }
                            }
                        }
                    }
                    doc["min"] = final_min;
                    doc["max"] = final_max;
                    doc["step"] = obj_step;
                }

                if (obj_type <= 2) { // AI, AO, AV
                    String unit = String(obj_unit_text);
                    if (unit == "Unknown" || unit.length() == 0 || unit == "none") unit = get_unit_text(obj_units);
                    if (unit != "no-units" && unit.length() > 0) {
                        if (unit == "%RH") unit = "%";
                        doc["unit_of_meas"] = unit;
                        if (unit == "°C" || unit == "°F") doc["dev_cla"] = "temperature";
                        else if (unit == "%") doc["dev_cla"] = "humidity";
                        else if (unit == "kW" || unit == "W") doc["dev_cla"] = "power";
                        else if (unit == "kWh") doc["dev_cla"] = "energy";
                    }
                }

                if (strcmp(ha_component, "select") == 0) {
                    JsonArray opts = doc["options"].to<JsonArray>();
                    for (auto& s : obj_states) opts.add(s);
                }

                JsonObject device = doc["dev"].to<JsonObject>();
                JsonArray ids = device["ids"].to<JsonArray>();
                char dev_id_str[32]; snprintf(dev_id_str, sizeof(dev_id_str), "bacnet_dev_%lu", (unsigned long)current_did);
                ids.add(String(dev_id_str));
                device["name"] = dev_name.length() > 0 ? String(dev_name) : String(dev_id_str);
                device["mf"] = dev_vendor.length() > 0 ? String(dev_vendor) : "BACnet Manufacturer";
                device["sw"] = configVERSION_GLOBAL;

                String payload; serializeJson(doc, payload);
                if (esp_mqtt_client_publish(mqtt_client, topic, payload.c_str(), payload.length(), 1, 1) < 0) {
                    z_log(pdLOG_WARN, "MQTT", "Discovery publish failed for %s. Outbox full?\n", uniq_id);
                    vTaskDelay(pdMS_TO_TICKS(100)); // Pause si erreur
                } else {
                    total_published++;
                }
                
            } else if ((!dev_enabled || !obj_enabled) && dev_discovery_done) {
                esp_mqtt_client_publish(mqtt_client, topic, "", 0, 1, 1);
            }

            if (dev_discovery_done) {
                if (total_published % 10 == 0 && total_published > 0) {
                    z_log(pdLOG_INFO, "MQTT", "Discovery progress: %d objects sent...\n", total_published);
                }
                vTaskDelay(pdMS_TO_TICKS(100)); // Pacing stable
            }
        }
    }
    z_log(pdLOG_INFO, "MQTT", "HA Auto-Discovery payload sent (%d objects).\n", total_published);
}

void publish_mqtt_topic(uint32_t ulDeviceId, BACnetObject& obj, uint8_t prop_id, bool retain) {
    if (!obj.xEnabled) return;
    MQTTPublishJob pub;
    pub.ulDeviceId = ulDeviceId;
    pub.obj_type = obj.usType;
    pub.obj_instance = obj.ulInstance;
    pub.prop_id = prop_id;
    pub.retain = retain;

    if (prop_id == 77) {
        if (strlen(obj.cName) == 0) return;
        strlcpy(pub.value_string, obj.cName, sizeof(pub.value_string));
    } else if (prop_id == 85) {
        switch(obj.usType) {
            case OBJ_MULTI_STATE_INPUT:
            case OBJ_MULTI_STATE_OUTPUT:
            case OBJ_MULTI_STATE_VALUE:
                if (!obj.state_texts.empty()) {
                    int idx = (int)obj.fPresentValue - 1;
                    if (idx >= 0 && idx < (int)obj.state_texts.size()) {
                        strlcpy(pub.value_string, obj.state_texts[idx].c_str(), sizeof(pub.value_string));
                    } else snprintf(pub.value_string, sizeof(pub.value_string), "%.0f", obj.fPresentValue);
                } else snprintf(pub.value_string, sizeof(pub.value_string), "%.0f", obj.fPresentValue);
                break;
            default:
                snprintf(pub.value_string, sizeof(pub.value_string), "%.2f", obj.fPresentValue);
                break;
        }
    } else return;

    enqueue_mqtt_publish(pub);
}

void unpublish_ha_discovery(uint32_t t_did, uint32_t t_inst, uint16_t t_type, const char* old_prefix) {
    if (!mqtt_is_connected || circuit_breaker_active) return;
    const char* prefix = old_prefix ? old_prefix : sysCfg.mqtt_prefix;
    z_log(pdLOG_INFO, "MQTT", "Cleaning up HA Discovery (Prefix: %s)...\n", prefix);

    if (t_did == 0) {
        auto unpub_gw = [&](const char* key, bool is_binary = false) {
            char topic[128], uniq[64];
            snprintf(uniq, sizeof(uniq), "b2m_gw_%s", key);
            snprintf(topic, sizeof(topic), "homeassistant/%s/%s/config", is_binary ? "binary_sensor" : "sensor", uniq);
            esp_mqtt_client_publish(mqtt_client, topic, "", 0, 1, 1);
        };
        unpub_gw("ver"); unpub_gw("uptime"); unpub_gw("rssi"); unpub_gw("heap");
        unpub_gw("min_heap"); unpub_gw("temp"); unpub_gw("nb_dev"); unpub_gw("mstp", true);
    }

    if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(500))) {
        for (auto& dev : bacnet_network_cache) {
            if (t_did != 0 && dev.ulDeviceId != t_did) continue;
            for (auto& obj : dev.objects) {
                if (t_inst != 0xFFFFFFFF && obj.ulInstance != t_inst) continue;
                if (t_type != 0xFFFF && obj.usType != t_type) continue;

                const char* t_str = "OBJ";
                switch(obj.usType) {
                    case OBJ_ANALOG_INPUT:       t_str = "AI"; break;
                    case OBJ_ANALOG_OUTPUT:      t_str = "AO"; break;
                    case OBJ_ANALOG_VALUE:       t_str = "AV"; break;
                    case OBJ_BINARY_INPUT:       t_str = "BI"; break;
                    case OBJ_BINARY_OUTPUT:      t_str = "BO"; break;
                    case OBJ_BINARY_VALUE:       t_str = "BV"; break;
                    case OBJ_MULTI_STATE_INPUT:  t_str = "MSI"; break;
                    case OBJ_MULTI_STATE_OUTPUT: t_str = "MSO"; break;
                    case OBJ_MULTI_STATE_VALUE:  t_str = "MSV"; break;
                }
                char uniq_id[64];
                snprintf(uniq_id, sizeof(uniq_id), "bacnet_%lu_%s_%lu", (unsigned long)dev.ulDeviceId, t_str, (unsigned long)obj.ulInstance);
                
                const char* components[] = {"sensor", "binary_sensor", "switch", "number", "select"};
                for (const char* comp : components) {
                    char topic[128];
                    snprintf(topic, sizeof(topic), "homeassistant/%s/%s/config", comp, uniq_id);
                    esp_mqtt_client_publish(mqtt_client, topic, "", 0, 1, 1);
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        xSemaphoreGive(cache_mutex);
    }
}
