#include "z_mqtt.h"
#include "z_bacnet.h"
#include <string.h>
#include <atomic>
#include <ArduinoJson.h>

#include "z_network.h"

uint32_t period_mqtt_pub_count = 0;

static int mqtt_fail_count = 0;
static std::atomic<bool> circuit_breaker_active{false};
static std::atomic<bool> pending_discovery{false};
static std::atomic<uint32_t> target_did{0};
static std::atomic<uint32_t> target_inst{0};
static std::atomic<uint16_t> target_type{0xFFFF};
static bool mqtt_is_connected = false;
static bool force_full_discovery = false; 
static char lwt_topic[128] = {0};

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            z_log(pdLOG_INFO, "MQTT", "Connected to Broker.\n");
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
                }
            }
            break;

        case MQTT_EVENT_ERROR:
        case MQTT_EVENT_DISCONNECTED:
            mqtt_is_connected = false;
            if (circuit_breaker_active) break;
            
            if (WiFi.status() != WL_CONNECTED) {
                z_log(pdLOG_WARN, "MQTT", "Connection dropped due to Wi-Fi loss.\n");
                break;
            }
            
            if (event->error_handle != NULL) {
                if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                    if (event->error_handle->connect_return_code == MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
                        event->error_handle->connect_return_code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED) {
                        z_log(pdLOG_ERROR, "MQTT", "FATAL: Invalid Broker Credentials.\n");
                        mqtt_fail_count = 3;
                    }
                }
            }
            
            mqtt_fail_count++;
            z_log(pdLOG_WARN, "MQTT", "Disconnected from Broker (%d/3).\n", mqtt_fail_count);
            
            if (mqtt_fail_count >= 3) {
                z_log(pdLOG_ERROR, "MQTT", "CIRCUIT BREAKER: Halting MQTT connection attempts.\n");
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
        if (mqtt_is_connected && !circuit_breaker_active) {
            // 1. Traitement de l'Auto-Discovery (Atomique pour éviter les loops de triggers)
            if (pending_discovery.load(std::memory_order_acquire)) {
                uint32_t t_did = target_did.load();
                uint32_t t_inst = target_inst.load();
                uint16_t t_type = target_type.load();
                bool is_global = (t_did == 0 || force_full_discovery);
                
                uint32_t now = millis();
                bool can_pub = false;
                if (is_global) {
                    if (now - last_global_disc > 30000) { can_pub = true; last_global_disc = now; }
                } else {
                    if (now - last_single_disc > 5000) { can_pub = true; last_single_disc = now; }
                }

                if (can_pub) {
                    pending_discovery.store(false, std::memory_order_release);
                    if (force_full_discovery) {
                        t_did = 0; 
                        t_inst = 0xFFFFFFFF;
                        force_full_discovery = false;
                    }
                    publish_ha_autodiscovery(t_did, t_inst, t_type);
                } else {
                    // On ne reset PAS pending_discovery, on attendra la prochaine boucle (non-dropping)
                    // On logue une seule fois pour éviter le flood de logs
                    static uint32_t last_log = 0;
                    if (now - last_log > 5000) {
                        z_log(pdLOG_DEBUG, "MQTT", "HA Discovery deferred (throttle active)\n");
                        last_log = now;
                    }
                }
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
                snprintf(topic, sizeof(topic), "%s/%lu/%s/%lu/%s", sysCfg.mqtt_prefix, (unsigned long)pubJob.ulDeviceId, t_str, (unsigned long)pubJob.obj_instance, subtopic);
                
                if (esp_mqtt_client_publish(mqtt_client, topic, pubJob.value_string, 0, 1, pubJob.retain) < 0) {
                    z_log(pdLOG_WARN, "MQTT", "Publish failed. Queue Full or Client error.\n");
                    vTaskDelay(pdMS_TO_TICKS(50));
                    break; 
                } else {
                    period_mqtt_pub_count++;
                    z_log(pdLOG_DEBUG, "MQTT", "Published: %s = %s\n", topic, pubJob.value_string);
                }
                vTaskDelay(pdMS_TO_TICKS(5)); 
            }

            // 3. Status Gateway périodique
            if (millis() - last_status_pub > (sysCfg.mqtt_poll_interval * 1000)) {
                last_status_pub = millis();
                auto pub_b2m = [&](const char* key, String val) {
                    char t[128]; snprintf(t, sizeof(t), "%s/B2M/%s/state", sysCfg.mqtt_prefix, key);
                    esp_mqtt_client_publish(mqtt_client, t, val.c_str(), 0, 1, 0);
                    period_mqtt_pub_count++;
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
                
                z_log(pdLOG_INFO, "MQTT", "Gateway Status published (Uptime: %lu s, Devices: %zu)\n", (unsigned long)(millis() / 1000), n_dev);
                z_log(pdLOG_INFO, "MQTT", "Published topics : %s/+, %s/B2M, tele/%s - Total messages : %lu\n", sysCfg.mqtt_prefix, sysCfg.mqtt_prefix, sysCfg.mqtt_prefix, (unsigned long)period_mqtt_pub_count);
                
                period_mqtt_pub_count = 0;
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
            // Migration vers Core 0 (Priorité 5) pour libérer le Core 1 pour le temps-réel BACnet
            xTaskCreatePinnedToCore(mqtt_gatekeeper_task, "MQTT_GK", 8192, NULL, 5, NULL, 0);
            task_created = true;
        }
    }
}

bool is_mqtt_connected() { return mqtt_is_connected; }

void trigger_ha_discovery(uint32_t did, uint32_t inst, uint16_t type) {
    if (!sysCfg.ha_discover) return;
    
    // Stratégie de Coalescence (v6.4.2)
    // Si une demande est déjà en attente :
    if (pending_discovery.load()) {
        if (target_did != did) {
            // Changement de device -> On passe en global pour tout republier proprement
            target_did = 0;
            target_inst = 0xFFFFFFFF;
        } else if (target_inst != inst || target_type != type) {
            // Même device mais objet différent -> On passe en mode "tout le device"
            target_inst = 0xFFFFFFFF;
            target_type = 0xFFFF;
        }
    } else {
        // Nouvelle demande fraîche
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

    // Détermination si c'est une requête ciblée
    bool is_single_object = (t_did != 0 && t_inst != 0xFFFFFFFF);
    
    if (!is_single_object) {
        z_log(pdLOG_INFO, "MQTT", "Starting HA Auto-Discovery%s...\n", (t_did != 0) ? " (Single Device)" : "");
    }

    // Étape 0 : Discovery de la Gateway elle-même (Diagnostics)
    // On ne publie les capteurs de diagnostic globaux que si la découverte n'est pas limitée à un seul objet précis
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
            z_log(pdLOG_DEBUG, "MQTT", "HA Discovery: %s\n", uniq);
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

                    // Filtrage ciblé (v6.4.2 Param-based)
                    if (t_did != 0) {
                        bool match_dev = (dev.ulDeviceId == t_did);
                        if (!match_dev) { xSemaphoreGive(cache_mutex); continue; }
                        
                        if (t_inst != 0xFFFFFFFF && obj.ulInstance != t_inst) { xSemaphoreGive(cache_mutex); continue; }
                        if (t_type != 0xFFFF && obj.usType != t_type) { xSemaphoreGive(cache_mutex); continue; }
                    }

                    // Ne pas publier si la découverte du device n'est pas terminée (Metadata manquante)
                    // Sauf si on est en train de forcer une suppression (obj.xEnabled == false OU dev.xEnabled == false)
                    if (!dev.xDiscoveryDone && obj.xEnabled && dev.xEnabled) {
                        xSemaphoreGive(cache_mutex);
                        continue;
                    }

                    if (obj.usType != 65535) {
                        const char* t_str = "OBJ";
                        const char* ha_component = "sensor";
                        bool is_command = obj.xIsCommandable; // Utilisation de la détection Prop 87
                
                        switch(obj.usType) {
                            case OBJ_ANALOG_INPUT: t_str = "AI"; ha_component = "sensor"; break;
                            case OBJ_BINARY_INPUT: t_str = "BI"; ha_component = "binary_sensor"; break;
                            case OBJ_BINARY_OUTPUT: 
                            case OBJ_BINARY_VALUE: 
                                t_str = (obj.usType == OBJ_BINARY_OUTPUT) ? "BO" : "BV"; 
                                ha_component = (is_command || obj.usType == OBJ_BINARY_OUTPUT) ? "switch" : "binary_sensor"; 
                                break;
                            case OBJ_ANALOG_OUTPUT:
                            case OBJ_ANALOG_VALUE: 
                                t_str = (obj.usType == OBJ_ANALOG_OUTPUT) ? "AO" : "AV"; 
                                ha_component = (is_command || obj.usType == OBJ_ANALOG_OUTPUT) ? "number" : "sensor"; 
                                break;
                            case OBJ_MULTI_STATE_INPUT: t_str = "MSI"; ha_component = "sensor"; break;
                            case OBJ_MULTI_STATE_OUTPUT:
                            case OBJ_MULTI_STATE_VALUE: 
                                t_str = (obj.usType == OBJ_MULTI_STATE_OUTPUT) ? "MSO" : "MSV"; 
                                // Fallback MSV: si on a des textes, on force select. Sinon, number pour rester visible.
                                if (obj.state_texts.empty()) {
                                    ha_component = (is_command || obj.usType == OBJ_MULTI_STATE_OUTPUT) ? "number" : "sensor";
                                } else {
                                    ha_component = "select";
                                }
                                break;
                        }

                        char uniq_id[64];
                        snprintf(uniq_id, sizeof(uniq_id), "bacnet_%lu_%s_%lu", (unsigned long)dev.ulDeviceId, t_str, (unsigned long)obj.ulInstance);
                        snprintf(final_topic, sizeof(final_topic), "homeassistant/%s/%s/config", ha_component, uniq_id);

                        // --- NETTOYAGE DES DOUBLONS (v6.4.0 Optimized) ---
                        // On ne supprime que si on a une trace d'un composant précédent DIFFÉRENT
                        if (strlen(obj.cLastHaComponent) > 0 && strcmp(obj.cLastHaComponent, ha_component) != 0) {
                            char old_topic[128];
                            snprintf(old_topic, sizeof(old_topic), "homeassistant/%s/%s/config", obj.cLastHaComponent, uniq_id);
                            esp_mqtt_client_publish(mqtt_client, old_topic, "", 0, 1, 1); 
                            vTaskDelay(pdMS_TO_TICKS(50)); // Petit délai entre unpublish et publish
                        }
                        strlcpy(obj.cLastHaComponent, ha_component, sizeof(obj.cLastHaComponent));

                        bool is_single_request = (t_did != 0);

                        if (dev.xEnabled && obj.xEnabled && strcmp(obj.cName, "Unknown") != 0) {
                            JsonDocument doc; 
                            char base_topic[128];
                            snprintf(base_topic, sizeof(base_topic), "%s/%lu/%s/%lu", sysCfg.mqtt_prefix, (unsigned long)dev.ulDeviceId, t_str, (unsigned long)obj.ulInstance);
                            
                            doc["~"] = String(base_topic);
                            doc["uniq_id"] = String(uniq_id);
                            doc["name"] = String(obj.cName); 
                            doc["stat_t"] = "~/state";
                            doc["avty_t"] = String(lwt_topic);
                            doc["pl_avail"] = "online";
                            doc["pl_not_avail"] = "offline";

                            if (strcmp(ha_component, "sensor") != 0 && strcmp(ha_component, "binary_sensor") != 0) {
                                doc["cmd_t"] = "~/set";
                            }

                            if (obj.usType == OBJ_BINARY_INPUT || obj.usType == OBJ_BINARY_OUTPUT || obj.usType == OBJ_BINARY_VALUE) {
                                doc["pl_on"] = "1.00"; doc["pl_off"] = "0.00";
                            }

                            if (strcmp(ha_component, "number") == 0) {
                                doc["min"] = isnan(obj.fMinValue) ? sysCfg.default_number_min : obj.fMinValue;
                                doc["max"] = isnan(obj.fMaxValue) ? sysCfg.default_number_max : obj.fMaxValue;
                                doc["step"] = sysCfg.default_number_step;
                            }

                            // --- GESTION DES UNITÉS (v6.3.4) ---
                            if (obj.usType == OBJ_ANALOG_INPUT || obj.usType == OBJ_ANALOG_VALUE || obj.usType == OBJ_ANALOG_OUTPUT) {
                                String unit = String(obj.cUnitText);
                                if (unit == "Unknown" || unit.length() == 0 || unit == "none") {
                                    unit = get_unit_text(obj.usUnits);
                                }
                                
                                if (unit != "no-usUnits" && unit.length() > 0) {
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

                            bool can_publish = true;
                            if (obj.usType == OBJ_MULTI_STATE_INPUT || obj.usType == OBJ_MULTI_STATE_OUTPUT || obj.usType == OBJ_MULTI_STATE_VALUE) {
                                if (!obj.state_texts.empty()) {
                                    JsonArray opts = doc["options"].to<JsonArray>();
                                    for (size_t i = 0; i < obj.state_texts.size(); i++) {
                                        opts.add(String(obj.state_texts[i]));
                                    }
                                    if (strcmp(ha_component, "sensor") == 0) doc["dev_cla"] = "enum";
                                } else {
                                    // Pas de textes: on publie en tant que nombre brut pour le moment
                                    if (strcmp(ha_component, "sensor") == 0) doc["icon"] = "mdi:numeric";
                                }
                            }

                            if (can_publish) {
                                JsonObject device = doc["dev"].to<JsonObject>();
                                JsonArray ids = device["ids"].to<JsonArray>();
                                char dev_id_str[32]; 
                                snprintf(dev_id_str, sizeof(dev_id_str), "bacnet_dev_%lu", (unsigned long)dev.ulDeviceId);
                                ids.add(String(dev_id_str));
                                device["name"] = dev.name.length() > 0 ? String(dev.name) : String(dev_id_str);
                                device["mf"] = dev.cVendor.length() > 0 ? String(dev.cVendor) : "BACnet Manufacturer";
                                device["sw"] = configVERSION_GLOBAL;

                                serializeJson(doc, final_payload);
                                should_publish = true;
                            }
                        } else if (is_single_request || !dev.xEnabled || !obj.xEnabled) {
                            // On efface l'entité de HA si l'appareil ou l'objet est désactivé
                            final_payload = ""; 
                            should_publish = true;
                        }
                    }
                }
                
                // On rend le Mutex ICI, tant qu'on est sûr de l'avoir pris
                // et AVANT de faire le réseau et le délai.
                xSemaphoreGive(cache_mutex);
            } 
            // --- FIN ZONE CRITIQUE ---

            // --- EXECUTION RESEAU ET TEMPORISATION (HORS MUTEX) ---
            if (should_publish) { 
                // Utilisation de esp_mqtt_client_publish avec QoS 1 pour garantir la délivrance et le traitement synchrone
                esp_mqtt_client_publish(mqtt_client, final_topic, final_payload.c_str(), final_payload.length(), 1, 1);
                vTaskDelay(pdMS_TO_TICKS(150)); // Augmenté à 150ms pour laisser respirer HA
            }
        }
    }
    
    // Le log de fin est placé tout à la fin de la fonction
    z_log(pdLOG_INFO, "MQTT", "HA Auto-Discovery payload sent.\n");
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
        if ((obj.usType == OBJ_MULTI_STATE_INPUT || obj.usType == OBJ_MULTI_STATE_OUTPUT || obj.usType == OBJ_MULTI_STATE_VALUE) && !obj.state_texts.empty()) {
            int idx = (int)obj.fPresentValue - 1;
            if (idx >= 0 && idx < (int)obj.state_texts.size()) {
                strlcpy(pub.value_string, obj.state_texts[idx].c_str(), sizeof(pub.value_string));
            } else {
                snprintf(pub.value_string, sizeof(pub.value_string), "%.0f", obj.fPresentValue);
            }
        } else {
            snprintf(pub.value_string, sizeof(pub.value_string), "%.2f", obj.fPresentValue);
        }
    } else return;

    enqueue_mqtt_publish(pub);
}

void unpublish_ha_discovery(uint32_t t_did, uint32_t t_inst, uint16_t t_type, const char* old_prefix) {
    if (!mqtt_is_connected || circuit_breaker_active) return;
    const char* prefix = old_prefix ? old_prefix : sysCfg.mqtt_prefix;
    z_log(pdLOG_INFO, "MQTT", "Cleaning up HA Discovery (Prefix: %s)...\n", prefix);

    // 1. Diagnostics Gateway
    auto unpub_gw = [&](const char* key, bool is_binary = false) {
        char topic[128], uniq[64];
        snprintf(uniq, sizeof(uniq), "b2m_gw_%s", key);
        snprintf(topic, sizeof(topic), "homeassistant/%s/%s/config", is_binary ? "binary_sensor" : "sensor", uniq);
        esp_mqtt_client_publish(mqtt_client, topic, "", 0, 1, 1);
    };
    if (t_did == 0) {
        unpub_gw("ver"); unpub_gw("uptime"); unpub_gw("rssi"); unpub_gw("heap");
        unpub_gw("min_heap"); unpub_gw("temp"); unpub_gw("nb_dev"); unpub_gw("mstp", true);
    }

    // 2. Objets BACnet
    if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(500))) {
        for (auto& dev : bacnet_network_cache) {
            if (t_did != 0 && dev.ulDeviceId != t_did) continue;
            for (auto& obj : dev.objects) {
                if (t_inst != 0xFFFFFFFF && obj.ulInstance != t_inst) continue;
                if (t_type != 0xFFFF && obj.usType != t_type) continue;

                const char* t_str = "OBJ";
                switch(obj.usType) {
                    case 0: t_str="AI"; break;
                    case 1: t_str="AO"; break;
                    case 2: t_str="AV"; break;
                    case 3: t_str="BI"; break;
                    case 4: t_str="BO"; break;
                    case 5: t_str="BV"; break;
                    case 13: t_str="MSI"; break;
                    case 14: t_str="MSO"; break;
                    case 19: t_str="MSV"; break;
                }
                char uniq_id[64];
                snprintf(uniq_id, sizeof(uniq_id), "bacnet_%lu_%s_%lu", (unsigned long)dev.ulDeviceId, t_str, (unsigned long)obj.ulInstance);
                
                // Balayage de TOUS les composants possibles pour cet ID unique (v6.4.1 Hardened)
                const char* components[] = {"sensor", "binary_sensor", "switch", "number", "select"};
                for (const char* comp : components) {
                    char topic[128];
                    snprintf(topic, sizeof(topic), "homeassistant/%s/%s/config", comp, uniq_id);
                    esp_mqtt_client_publish(mqtt_client, topic, "", 0, 1, 1);
                }
                vTaskDelay(pdMS_TO_TICKS(10)); // Petit pacing pour le cleanup
            }
        }
        xSemaphoreGive(cache_mutex);
    }
}
