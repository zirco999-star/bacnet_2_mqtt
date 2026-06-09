#include "z_network.h"
#include "z_ui.h"
#include "z_bacnet.h"
#include "z_mqtt.h"
#include "z_nvs.h"
#include <ArduinoJson.h>
#include <Update.h>
#include <ESPmDNS.h>
#include "esp_wifi.h"
#include <stdarg.h>
#include "nvs_flash.h"
#include <ArduinoOTA.h>

extern AsyncWebSocket ws;
static uint32_t wifi_connect_start = 0;
static bool wifi_fallback_active = false;

// Structure to hold log messages in the queue
typedef struct {
    char message[256];
} LogMessage;

QueueHandle_t log_queue = NULL;
SemaphoreHandle_t api_mutex = NULL;
SemaphoreHandle_t ws_mutex = NULL;
PSRAM_Allocator psram_alloc;

static uint32_t last_ws_event = 0;
static uint32_t active_ws_id = 0;

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
             void *arg, uint8_t *data, size_t len) {
    // v6.6.0: Callbacks atomiques pour éviter tout blocage TCP
    if (type == WS_EVT_CONNECT) {
        last_ws_event = millis();
        active_ws_id = client->id();
        Serial.printf("[WEB] WS Session Link: %u\n", active_ws_id);
    } 
    else if (type == WS_EVT_DISCONNECT) {
        if (active_ws_id == client->id()) active_ws_id = 0;
        last_ws_event = millis();
        Serial.printf("[WEB] WS Session Terminated\n");
    }
}

// Task running on Core 0 to send logs to the web interface via WebSockets
static void websocket_log_task(void *pvParameters) {
    LogMessage received_log;
    // v6.6.0: Silence radio total de 10s au démarrage pour laisser le WiFi et MQTT s'établir
    vTaskDelay(pdMS_TO_TICKS(10000));
    
    for( ;; ) {
        if (xQueueReceive(log_queue, &received_log, portMAX_DELAY) == pdTRUE) {
            // v6.6.0: Pas d'envoi si un événement réseau récent (< 5s) pour éviter collision au refresh
            if (millis() - last_ws_event < 5000) continue;

            // v6.6.0: SI L'API EST OCCUPEE (chargement page/JSON), ON JETTE LE LOG
            // C'est la protection ultime contre le crash 0x30
            if (api_mutex == NULL || uxSemaphoreGetCount(api_mutex) == 0) continue;

            if (active_ws_id > 0 && ESP.getFreeHeap() > 50000) {
                if (xSemaphoreTake(ws_mutex, pdMS_TO_TICKS(20))) {
                    AsyncWebSocketClient* target = ws.client(active_ws_id);
                    if (target && target->status() == WS_CONNECTED) {
                        target->text(received_log.message);
                    } else {
                        active_ws_id = 0;
                    }
                    xSemaphoreGive(ws_mutex);
                }
                // v6.6.0: Délai forcé entre chaque message pour ne pas saturer la pile AsyncTCP
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
    }
}

// Safe logging function that adds messages to the queue without blocking the program
void z_log(int level, const char* tag, const char* format, ...) {
    // 1. Filtrage temps réel : on ignore le log si sa priorité est trop faible
    if (level > sysCfg.log_level) return;

    // 2. Détermination de la chaîne de sévérité
    char lvl_str[5] = "INF";
    if (level == LOG_ERROR) strcpy(lvl_str, "ERR");
    else if (level == LOG_WARN) strcpy(lvl_str, "WRN");
    else if (level == LOG_DEBUG) strcpy(lvl_str, "DBG");

    // 3. Calcul du temps
    uint32_t now_ms = millis();
    uint32_t ms = now_ms % 1000;
    uint32_t s = (now_ms / 1000) % 60;
    uint32_t m = (now_ms / 60000) % 60;
    uint32_t h = (now_ms / 3600000); 

    // Création du timestamp formaté (13 caractères + '\0' = 14)
    char timestamp[14];
    snprintf(timestamp, sizeof(timestamp), "%02lu:%02lu:%02lu,%03lu", h, m, s, ms);

    int core = xPortGetCoreID();

    // 4. Formatage de la base commune : "[lvl_str][tag] "
    LogMessage base_msg;
    int prefix_len = snprintf(base_msg.message, sizeof(base_msg.message), "[%s][%s] ", lvl_str, tag);

    // 5. Extraction des arguments variables (vsnprintf) pour le "msg"
    if (prefix_len > 0 && prefix_len < sizeof(base_msg.message)) {
        va_list arg;
        va_start(arg, format);
        vsnprintf(base_msg.message + prefix_len, sizeof(base_msg.message) - prefix_len, format, arg);
        va_end(arg);
    }

    // 6. Sortie physique UART : [core] + base_msg
    // Le moniteur ESP s'occupera d'ajouter son propre timestamp devant
    printf("[%d]%s", core, base_msg.message);

    // 7. Envoi asynchrone sécurisé vers la tâche WebSocket
    if (log_queue != NULL) {
        LogMessage ws_msg;
        
        // Format demandé : core|[timestamp][lvl_str][tag] msg
        snprintf(ws_msg.message, sizeof(ws_msg.message), "%d|[%s]%s", core, timestamp, base_msg.message);
        
        xQueueSend(log_queue, &ws_msg, 0);
    }
}



void setup_network_infrastructure() {
    // 1. Initialize core system mutexes first to avoid race conditions during NVS load
    if (cache_mutex == NULL) {
        cache_mutex = xSemaphoreCreateMutex();
    }
    if (api_mutex == NULL) {
        api_mutex = xSemaphoreCreateMutex();
    }
    if (ws_mutex == NULL) {
        ws_mutex = xSemaphoreCreateMutex();
    }

    log_queue = xQueueCreate(20, sizeof(LogMessage));
    if (log_queue != NULL) {
        xTaskCreatePinnedToCore(websocket_log_task, "WS_Log", 8192, NULL, 2, NULL, 0);
    }

    // 2. Load Settings (now safe because cache_mutex exists)
    load_configuration();

    WiFi.persistent(false); WiFi.disconnect(true);
    vTaskDelay(pdMS_TO_TICKS(100));

    if (strlen(sysCfg.wifi_ssid) == 0) {
        is_ap_mode = true; WiFi.mode(WIFI_AP);
        WiFi.softAP("ZIRCON-GW-CONFIG", "admin1234");
    } else {
        is_ap_mode = false; WiFi.mode(WIFI_STA);
        esp_wifi_set_ps(WIFI_PS_NONE);
        if (sysCfg.static_ip) {
            IPAddress ip, gw, sn;
            if (ip.fromString(sysCfg.local_ip) && gw.fromString(sysCfg.gateway) && sn.fromString(sysCfg.subnet)) {
                WiFi.config(ip, gw, sn);
            }
        }
            WiFi.begin(sysCfg.wifi_ssid, sysCfg.wifi_pass);
        wifi_connect_start = millis();
    }

    ws.onEvent(onEvent); // v6.5.2: Callback obligatoire pour la thread-safety
    webServer.addHandler(&ws);
    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!is_authenticated(request)) return;
        request->send_P(200, "text/html", INDEX_HTML);
    });

    /* v6.5.2: Désactivation temporaire pour isoler la cause du crash parallel sockets
    webServer.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncWebServerResponse *response = request->beginResponse_P(200, "image/x-icon", favicon_ico, favicon_ico_len);
        if (response) {
            response->addHeader("Cache-Control", "public, max-age=31536000");
            request->send(response);
        }
    });

    webServer.on("/apple-touch-icon.png", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncWebServerResponse *response = request->beginResponse_P(200, "image/png", favicon_apple, favicon_apple_len);
        if (response) {
            response->addHeader("Cache-Control", "public, max-age=31536000");
            request->send(response);
        }
    });
    */

    webServer.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!is_authenticated(request)) return;

        // v6.4.8: Strict Heap Protection
        if (ESP.getFreeHeap() < 50000) {
            request->send(503, "text/plain", "Service Unavailable: Low Memory");
            return;
        }

        // v6.4.8: API Serialization
        if (xSemaphoreTake(api_mutex, pdMS_TO_TICKS(1000))) {
            AsyncResponseStream *stream = request->beginResponseStream("application/json");
            if (!stream) { xSemaphoreGive(api_mutex); return; }
            stream->addHeader("Connection", "close");

            // v6.4.9: Force PSRAM allocation
            JsonDocument doc(&psram_alloc);
            doc["ver"] = configVERSION_GLOBAL;
            doc["rssi"] = WiFi.RSSI();
            doc["cur_ip"] = WiFi.localIP().toString();
            doc["cur_mask"] = WiFi.subnetMask().toString();
            doc["cur_gw"] = WiFi.gatewayIP().toString();
            
            doc["ssid"] = sysCfg.wifi_ssid;
            doc["static"] = sysCfg.static_ip;
            doc["ip"] = sysCfg.local_ip;
            doc["gw"] = sysCfg.gateway;
            doc["mask"] = sysCfg.subnet;
            
            doc["mqs"] = sysCfg.mqtt_server;
            doc["mqu"] = sysCfg.mqtt_user;
            doc["mqpr"] = sysCfg.mqtt_prefix;
            doc["ha_disc"] = sysCfg.ha_discover;
            doc["n_min"] = sysCfg.default_number_min;
            doc["n_max"] = sysCfg.default_number_max;
            doc["n_stp"] = sysCfg.default_number_step;
            
            doc["mqtt"] = is_mqtt_connected();
            doc["heap"] = ESP.getFreeHeap() / 1024;
            doc["uptime"] = millis() / 1000;
            
            doc["mac"] = sysCfg.mac_address;
            doc["mm"] = sysCfg.max_master;
            doc["did"] = sysCfg.device_id;
            doc["to"] = sysCfg.apdu_timeout;
            doc["ret"] = sysCfg.max_retries;
            doc["hbeat"] = sysCfg.heartbeat_interval;
            doc["tskip"] = sysCfg.token_skip;
            doc["mif"] = sysCfg.max_info_frames;
            doc["mpi"] = sysCfg.mqtt_poll_interval;
            doc["bpi"] = sysCfg.bacnet_poll_interval;
            doc["adu"] = sysCfg.admin_user;
            doc["lvl"] = sysCfg.log_level;

            doc["mstp_t"] = bacnetStats.ring_active;
            doc["mstp_cnt"] = bacnetStats.tokens_seen;
            doc["mstp_rx"] = bacnetStats.ms_msgs_rx;
            doc["mstp_tx"] = bacnetStats.ms_msgs_tx;
            doc["mstp_err"] = bacnetStats.errors_crc;

            JsonArray devices = doc["devices"].to<JsonArray>();
            if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(100))) {
                for (auto& dev : bacnet_network_cache) {
                    JsonObject d = devices.add<JsonObject>();
                    d["id"] = dev.device_id;
                    d["step"] = (int)dev.disc_step;
                    d["idx"] = dev.disc_obj_idx;
                    d["total"] = dev.objects.size();
                    d["enabled"] = dev.enabled;
                    d["done"] = dev.discovery_done;
                    
                    int sel = 0;
                    for(auto& o : dev.objects) if(o.enabled) sel++;
                    d["sel"] = sel;
                }
                xSemaphoreGive(cache_mutex);
            }

            serializeJson(doc, *stream);
            request->send(stream);
            xSemaphoreGive(api_mutex);
        } else {
            request->send(503, "text/plain", "Service Unavailable: API Busy");
        }
    });

    webServer.on("/api/objects", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!is_authenticated(request)) return;

        // v6.4.8: Strict Heap Protection
        if (ESP.getFreeHeap() < 50000) {
            request->send(503, "text/plain", "Service Unavailable: Low Memory");
            return;
        }

        // v6.4.8: API Serialization
        if (xSemaphoreTake(api_mutex, pdMS_TO_TICKS(1000))) {
            AsyncResponseStream *stream = request->beginResponseStream("application/json");
            if (!stream) { xSemaphoreGive(api_mutex); return; }
            stream->addHeader("Connection", "close");

            // v6.4.9: Force PSRAM allocation
            JsonDocument doc(&psram_alloc); 
            JsonArray controllers = doc.to<JsonArray>();
            
            if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(100))) {
                for (auto& dev : bacnet_network_cache) {
                    JsonObject c = controllers.add<JsonObject>();
                    c["device_id"] = dev.device_id;
                    c["name"] = dev.name; c["vendor"] = dev.vendor; c["enabled"] = dev.enabled;
                    JsonArray objs_arr = c["objects"].to<JsonArray>();
                    for (auto& o : dev.objects) {
                        JsonObject obj = objs_arr.add<JsonObject>();
                        obj["type"] = o.type; obj["inst"] = o.instance; obj["name"] = o.name;
                        obj["val"] = o.present_value; obj["poll"] = o.enabled;
                        obj["unit"] = o.unit_text;
                    }
                }
                xSemaphoreGive(cache_mutex);
            }
            serializeJson(doc, *stream);
            request->send(stream);
            xSemaphoreGive(api_mutex);
        } else {
            request->send(503, "text/plain", "Service Unavailable: API Busy");
        }
    });

    webServer.on("/api/whois", HTTP_POST, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        BACnetJob job; job.type = JOB_WHO_IS;
        enqueue_bacnet_job(job);
        request->send(200, "text/plain", "WHO-IS ENQUEUED");
    });

    webServer.on("/api/iam", HTTP_POST, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        BACnetJob job; job.type = JOB_I_AM;
        enqueue_bacnet_job(job);
        request->send(200, "text/plain", "I-AM ENQUEUED");
    });

    webServer.on("/api/reload_device", HTTP_POST, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        if (request->hasParam("id", true)) {
            uint32_t did = request->getParam("id", true)->value().toInt();
            if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(500))) {
                for (auto& dev : bacnet_network_cache) {
                    if (dev.device_id == did) {
                        dev.discovery_done = false;
                        dev.disc_step = DISC_DEV_ID;
                        dev.objects.clear();
                        z_log(LOG_INFO, "API", "Reloading device %lu\n", (unsigned long)did);
                        break;
                    }
                }
                xSemaphoreGive(cache_mutex);
            }
            request->send(200, "text/plain", "RELOADING");
        } else request->send(400, "text/plain", "Missing id");
    });

    webServer.on("/api/reload_object", HTTP_POST, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        if (request->hasParam("did", true) && request->hasParam("inst", true) && request->hasParam("type", true)) {
            uint32_t did = request->getParam("did", true)->value().toInt();
            uint32_t inst = request->getParam("inst", true)->value().toInt();
            uint16_t type = request->getParam("type", true)->value().toInt();
            
            if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(500))) {
                for (auto& dev : bacnet_network_cache) {
                    if (dev.device_id == did) {
                        for (size_t i = 0; i < dev.objects.size(); i++) {
                            if (dev.objects[i].instance == inst && dev.objects[i].type == type) {
                                dev.reload_single = true;
                                dev.disc_obj_idx = i;
                                dev.disc_step = DISC_OBJ_NAME; // On veut rafraîchir toutes les métadonnées (Nom, Unités)
                                dev.discovery_done = false;
                                break;
                            }
                        }
                        break;
                    }
                }
                xSemaphoreGive(cache_mutex);
            }
            request->send(200, "text/plain", "READ ENQUEUED VIA FSM");
        } else request->send(400, "text/plain", "Missing params");
    });

    webServer.on("/api/save_object", HTTP_POST, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
    
        if (request->hasParam("did", true) && request->hasParam("inst", true) && request->hasParam("type", true)) {
            uint32_t did = request->getParam("did", true)->value().toInt();
            uint32_t inst = request->getParam("inst", true)->value().toInt();
            uint16_t type = request->getParam("type", true)->value().toInt();
            
            String name = request->hasParam("name", true) ? request->getParam("name", true)->value() : "";
            String unit = request->hasParam("unit", true) ? request->getParam("unit", true)->value() : "";
            bool poll = request->hasParam("poll", true) ? (request->getParam("poll", true)->value() == "1") : true;

            // -------------------------------------------------------------
            // PHASE 1 : Verrouillage de la RAM (Ultra-rapide, < 1 ms)
            // -------------------------------------------------------------
            if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(500))) {
                uint8_t target_mac = 0;
                for (auto& dev : bacnet_network_cache) {
                    if (dev.device_id == did) {
                        target_mac = dev.mac_address;
                        for (auto& obj : dev.objects) {
                            if (obj.instance == inst && obj.type == type) {
                                // Si le nom a changé, on met à jour localement ET sur l'automate
                                if (name.length() > 0 && strcmp(obj.name, name.c_str()) != 0) {
                                    strlcpy(obj.name, name.c_str(), sizeof(obj.name));
                                    BACnetJob job;
                                    job.type = JOB_WRITE_PROP;
                                    job.target_mac = target_mac;
                                    job.obj_type = type;
                                    job.obj_instance = inst;
                                    job.prop_id = 77; // Object_Name
                                    strlcpy(job.name, name.c_str(), sizeof(job.name));
                                    enqueue_bacnet_job(job);
                                    z_log(LOG_INFO, "WEB", "Enqueuing WriteProperty (Name) for Obj T%u I%lu\n", type, (unsigned long)inst);
                                    // Publication MQTT du topic 'name' (pour historique / scripts custom)
                                    publish_mqtt_topic(did, obj, 77, true);
                                    // REQUIS : Déclencher HA Discovery pour mettre à jour l'entité dans Home Assistant
                                    trigger_ha_discovery(did, inst, type);
                                }
                                
                                // L'unité est TOUJOURS gérée localement (RAM + NVS) pour permettre l'override utilisateur
                                if (unit.length() > 0 && strcmp(obj.unit_text, unit.c_str()) != 0) {
                                    strlcpy(obj.unit_text, unit.c_str(), sizeof(obj.unit_text));
                                    z_log(LOG_INFO, "WEB", "Local Unit Override: Obj T%u I%lu -> %s (NVS only)\n", type, (unsigned long)inst, unit.c_str());
                                    // Pas de WriteProperty pour l'unité, mais publication MQTT pour HA
                                    trigger_ha_discovery(did, inst, type); 
                                }

                                if (obj.enabled != poll) {
                                    obj.enabled = poll;
                                    // Si on active/désactive l'objet, on force la synchro HA
                                    trigger_ha_discovery(did, inst, type);
                                    
                                    // Si on l'active, on le force aussi à recharger sa valeur immédiatement
                                    if (poll) {
                                        dev.reload_single = true;
                                        // On réinitialise l'étape pour forcer la relecture de la prop 87 et 85
                                        // Cela permettra à HA de savoir si c'est un sensor ou un switch
                                        dev.disc_step = DISC_OBJ_COMMANDABLE; 
                                        // On trouve l'index de cet objet
                                        for (size_t i = 0; i < dev.objects.size(); i++) {
                                            if (dev.objects[i].instance == inst && dev.objects[i].type == type) {
                                                dev.disc_obj_idx = i;
                                                break;
                                            }
                                        }
                                        dev.discovery_done = false; 
                                    }
                                }
                                break; 
                            }
                        }
                        break; 
                    }
                }
                xSemaphoreGive(cache_mutex); 
            }

            // -------------------------------------------------------------
            // PHASE 2 : Écriture Flash NVS (Lente, ~100 ms)
            // -------------------------------------------------------------
            // La fonction save_device_objects va re-verrouiller le mutex
            // de manière optimisée par "paquets" ou pages, laissant au Core 1
            // le temps de s'intercaler si le bus RS-485 est actif.
            save_device_objects(did);

            request->send(200, "text/plain", "OK");
        } else {
            request->send(400, "text/plain", "Missing params");
        }
    });

    webServer.on("/api/toggle_device", HTTP_POST, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;

        if (request->hasParam("id", true)) {
            uint32_t did = request->getParam("id", true)->value().toInt();
            
            // -------------------------------------------------------------
            // PHASE 1 : Verrouillage de la RAM (Ultra-rapide, < 1 ms)
            // -------------------------------------------------------------
            if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(500))) {
                for (auto& dev : bacnet_network_cache) {
                    if (dev.device_id == did) {
                        dev.enabled = !dev.enabled; // Bascule de l'état
                        
                        // v6.3.8: Reprise de la Phase 2 si l'automate est activé manuellement
                        if (dev.enabled && dev.discovery_done && dev.disc_step == DISC_OBJ_OID) {
                            z_log(LOG_INFO, "WEB", "User Activation: Resuming discovery for MAC %d (Phase 2)\n", dev.mac_address);
                            dev.discovery_done = false;
                        }
                        break; 
                    }
                }
                // LIBÉRATION IMMÉDIATE DU MUTEX
                // Le Core 1 (MS/TP) peut à nouveau manipuler le jeton réseau
                xSemaphoreGive(cache_mutex);
                
                // -------------------------------------------------------------
                // PHASE 2 : Écriture Flash NVS asynchrone par rapport au Core 1
                // -------------------------------------------------------------
                // La fonction wrapper gérera ses propres verrous par petites pages
                save_device_objects(did);

                // -------------------------------------------------------------
                // PHASE 3 : Redéclenchement du Gatekeeper MQTT
                // -------------------------------------------------------------
                // On passe le did pour que le discovery cible cet appareil (unpublish si désactivé)
                trigger_ha_discovery(did);

            } else {
                z_log(LOG_ERROR, "SYS", "Timeout Mutex : Impossible de basculer l'équipement en RAM !");
            }

            request->send(200, "text/plain", "OK");
        } else {
            request->send(400, "text/plain", "Missing id");
        }
    });

    webServer.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!is_authenticated(request)) return;
        String ft = "";
        if (request->hasParam("form_type", true)) ft = request->getParam("form_type", true)->value();

        if (ft == "wifi") {
            strlcpy(sysCfg.wifi_ssid, request->getParam("ssid", true)->value().c_str(), 32);
            String p_val = request->getParam("pass", true)->value();
            if (p_val.length() > 0 && p_val != "******") {
                strlcpy(sysCfg.wifi_pass, p_val.c_str(), 64);
            }
            
            sysCfg.static_ip = request->hasParam("static_ip", true);
            String l_ip = request->getParam("local_ip", true)->value();
            String l_gw = request->getParam("gateway", true)->value();
            String l_sn = request->getParam("subnet", true)->value();
            
            if (sysCfg.static_ip) {
                IPAddress tmp;
                if (!tmp.fromString(l_ip) || !tmp.fromString(l_gw) || !tmp.fromString(l_sn)) {
                    request->send(400, "text/plain", "Invalid IP/GW/Subnet format.");
                    return;
                }
            }
            
            strlcpy(sysCfg.local_ip, l_ip.c_str(), 16);
            strlcpy(sysCfg.gateway, l_gw.c_str(), 16);
            strlcpy(sysCfg.subnet, l_sn.c_str(), 16);
        } else if (ft == "mqtt") {
            char old_pr[64]; strlcpy(old_pr, sysCfg.mqtt_prefix, 64);
            strlcpy(sysCfg.mqtt_server, request->getParam("mqh", true)->value().c_str(), 32);
            strlcpy(sysCfg.mqtt_user, request->getParam("mqu", true)->value().c_str(), 32);
            String mqp_val = request->getParam("mqp", true)->value();
            if (mqp_val.length() > 0 && mqp_val != "******") {
                strlcpy(sysCfg.mqtt_pass, mqp_val.c_str(), 32);
            }
            strlcpy(sysCfg.mqtt_prefix, request->getParam("mqpr", true)->value().c_str(), 64);
            bool old_ha_discover = sysCfg.ha_discover;
            sysCfg.ha_discover = request->hasParam("ha_disc", true);
            
            if (request->hasParam("n_min", true)) sysCfg.default_number_min = request->getParam("n_min", true)->value().toFloat();
            if (request->hasParam("n_max", true)) sysCfg.default_number_max = request->getParam("n_max", true)->value().toFloat();
            if (request->hasParam("n_stp", true)) sysCfg.default_number_step = request->getParam("n_stp", true)->value().toFloat();

            // Si HA Discovery vient d'être désactivé, on supprime tout de HA
            if (old_ha_discover && !sysCfg.ha_discover) {
                unpublish_ha_discovery(0, 0xFFFFFFFF, 0xFFFF, sysCfg.mqtt_prefix);
                z_log(LOG_INFO, "MQTT", "HA Discovery desactive. Nettoyage MQTT envoye.\n");
            }

            // Si le préfixe change et que HA est TOUJOURS actif, on nettoie d'abord l'ancien prefixe côté HA
            if (strcmp(old_pr, sysCfg.mqtt_prefix) != 0 && sysCfg.ha_discover) {
                unpublish_ha_discovery(0, 0xFFFFFFFF, 0xFFFF, old_pr);
            }
        } else if (ft == "bac") {
            sysCfg.mac_address = request->getParam("mac", true)->value().toInt();
            sysCfg.device_id = request->getParam("did", true)->value().toInt();
            sysCfg.max_master = request->getParam("mm", true)->value().toInt();
            sysCfg.max_retries = request->getParam("retries", true)->value().toInt();
            sysCfg.apdu_timeout = request->getParam("timeout", true)->value().toInt();
            sysCfg.token_skip = request->getParam("tskip", true)->value().toInt();
            sysCfg.max_info_frames = request->getParam("mif", true)->value().toInt();
            sysCfg.heartbeat_interval = request->getParam("hbeat", true)->value().toInt();
        } else if (ft == "poll") {
            sysCfg.mqtt_poll_interval = request->getParam("mpi", true)->value().toInt();
            sysCfg.bacnet_poll_interval = request->getParam("bpi", true)->value().toInt();
        } else if (ft == "sec") {
            strlcpy(sysCfg.admin_user, request->getParam("admin_u", true)->value().c_str(), 32);
            String ad_p = request->getParam("admin_p", true)->value();
            if (ad_p.length() > 0 && ad_p != "******") {
                strlcpy(sysCfg.admin_pass, ad_p.c_str(), 64);
            }
            if (request->hasParam("lvl", true)) {
                sysCfg.log_level = request->getParam("lvl", true)->value().toInt();
            }
        }

        save_configuration();
        request->send(200, "text/plain", "OK");
        
        if (ft == "wifi") {
            pending_reboot = true; reboot_timer = millis();
        } else if (ft == "mqtt") {
            setup_mqtt();
        }
    });

    webServer.on("/api/reset_cache", HTTP_ANY, [](AsyncWebServerRequest *request) {
        if (!is_authenticated(request)) return;
        // 1. Nettoyage préventif MQTT (avant de perdre les pointeurs)
        unpublish_ha_discovery(); 
        
        if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(1000))) {
            for (auto& dev : bacnet_network_cache) {
                char ns[16]; snprintf(ns, sizeof(ns), "dv_%lu", (unsigned long)dev.device_id); // Correction namespace v6.4.1
                Preferences p; p.begin(ns, false); p.clear(); p.end();
            }
            Preferences reg; reg.begin("registry", false); reg.clear(); reg.end();
            bacnet_network_cache.clear();
            xSemaphoreGive(cache_mutex);
        }
        request->send(200, "text/plain", "Cache cleared. Rebooting...");
        pending_reboot = true; reboot_timer = millis();
    });

    webServer.on("/api/factory_reset", HTTP_ANY, [](AsyncWebServerRequest *request) {
        if (!is_authenticated(request)) return;
        nvs_flash_erase();
        request->send(200, "text/plain", "Factory reset done. Rebooting...");
        pending_reboot = true; reboot_timer = millis();
    });

    webServer.on("/api/reboot", HTTP_ANY, [](AsyncWebServerRequest *request) {
        if (!is_authenticated(request)) return;
        request->send(200, "text/plain", "Rebooting...");
        pending_reboot = true; reboot_timer = millis();
    });

    webServer.on("/api/trigger_discovery", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!is_authenticated(request)) return;
        
        if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(1000))) {
            z_log(LOG_INFO, "WEB", "Manual Discovery Triggered - Clearing Metadata Cache\n");
            for (auto& dev : bacnet_network_cache) {
                dev.discovery_done = false;
                dev.disc_step = DISC_DEV_ID;
                dev.disc_obj_idx = 0;
                for (auto& obj : dev.objects) {
                    obj.name_published = false;
                    memset(obj.unit_text, 0, sizeof(obj.unit_text));
                }
            }
            xSemaphoreGive(cache_mutex);
        }

        trigger_ha_discovery();
        request->send(200, "text/plain", "Discovery triggered & cache cleared");
    });

    ArduinoOTA.onStart([]() { z_log(LOG_INFO, "OTA", "Start\n"); });
    ArduinoOTA.onEnd([]() { z_log(LOG_INFO, "OTA", "End\n"); });
    ArduinoOTA.onError([](ota_error_t error) { z_log(LOG_ERROR,"OTA", "Error[%u]\n", error); });
    ArduinoOTA.begin();

    webServer.begin();
    z_log(LOG_INFO, "WEB", "Web Server started\n");
}

void handle_network() {
    ArduinoOTA.handle();
    if (pending_reboot && (millis() - reboot_timer > 1000)) ESP.restart();

    if (!is_ap_mode && WiFi.status() != WL_CONNECTED && (millis() - wifi_connect_start > 30000)) {
        if (!wifi_fallback_active) {
            z_log(LOG_ERROR, "WIFI", "WiFi Connection failed. Fallback to AP Mode.\n");
            WiFi.mode(WIFI_AP);
            WiFi.softAP("ZIRCON-GW-FALLBACK", "admin1234");
            wifi_fallback_active = true;
        }
    }
}

bool is_authenticated(AsyncWebServerRequest *request) {
    if (!request->authenticate(sysCfg.admin_user, sysCfg.admin_pass)) {
        request->requestAuthentication();
        return false;
    }
    return true;
}
