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

// Task running on Core 0 to send logs to the web interface via WebSockets
static void websocket_log_task(void *pvParameters) {
    LogMessage received_log;
    for( ;; ) {
        if (xQueueReceive(log_queue, &received_log, portMAX_DELAY) == pdTRUE) {
            if (ws.count() > 0) {
                ws.textAll(received_log.message);
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

    LogMessage log_msg;
    uint32_t now_ms = millis();
    int core = xPortGetCoreID();

    // 3. Formatage industriel du préfixe de log : [Timestamp][Core][Niveau][Tag]
    int prefix_len = snprintf(log_msg.message, sizeof(log_msg.message), "[%lu][%d][%s][%s] ", (unsigned long)now_ms, core, lvl_str, tag);

    // 4. Extraction des arguments variables (vsnprintf)
    va_list arg;
    va_start(arg, format);
    vsnprintf(log_msg.message + prefix_len, sizeof(log_msg.message) - prefix_len, format, arg);
    va_end(arg);

    // 5. Sortie physique UART
    printf("%s", log_msg.message);

    // 6. Envoi asynchrone sécurisé vers la tâche WebSocket (Core 0)
    if (log_queue != NULL) {
        LogMessage ws_msg;
        snprintf(ws_msg.message, sizeof(ws_msg.message), "%d|%s", core, log_msg.message);
        xQueueSend(log_queue, &ws_msg, 0);
    }
}



void setup_network_infrastructure() {
    // 1. Initialize core system mutexes first to avoid race conditions during NVS load
    if (cache_mutex == NULL) {
        cache_mutex = xSemaphoreCreateMutex();
    }

    log_queue = xQueueCreate(20, sizeof(LogMessage));
    if (log_queue != NULL) {
        xTaskCreatePinnedToCore(websocket_log_task, "WS_Log", 4096, NULL, 2, NULL, 0);
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

    webServer.addHandler(&ws);
    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!is_authenticated(request)) return;
        request->send_P(200, "text/html", INDEX_HTML);
    });

    webServer.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!is_authenticated(request)) return;
        JsonDocument doc;
        doc["ver"] = VERSION_GLOBAL;
        doc["rssi"] = WiFi.RSSI();
        doc["cur_ip"] = WiFi.localIP().toString();
        doc["cur_mask"] = WiFi.subnetMask().toString();
        doc["cur_gw"] = WiFi.gatewayIP().toString();
        doc["mqs"] = sysCfg.mqtt_server;
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
        doc["ha_disc"] = sysCfg.ha_discover;

        doc["mstp_t"] = (bacnetStats.tokens_seen > 0);
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

        String out; serializeJson(doc, out);
        request->send(200, "application/json", out);
    });

    webServer.on("/api/objects", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!is_authenticated(request)) return;
        JsonDocument* doc_ptr = new JsonDocument(); JsonDocument& doc = *doc_ptr; JsonArray controllers = doc.to<JsonArray>();
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
        String response; serializeJson(doc, response);
        request->send(200, "application/json", response);
        delete doc_ptr;
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
            
            BACnetJob job;
            job.type = JOB_READ_PROP;
            job.obj_type = type;
            job.obj_instance = inst;
            job.prop_id = 85; // Present_Value
            
            if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(500))) {
                for (auto& dev : bacnet_network_cache) {
                    if (dev.device_id == did) {
                        job.target_mac = dev.mac_address;
                        break;
                    }
                }
                xSemaphoreGive(cache_mutex);
            }
            enqueue_bacnet_job(job);
            request->send(200, "text/plain", "READ ENQUEUED");
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
                for (auto& dev : bacnet_network_cache) {
                    if (dev.device_id == did) {
                        for (auto& obj : dev.objects) {
                            if (obj.instance == inst && obj.type == type) {
                                if (name.length() > 0) strlcpy(obj.name, name.c_str(), sizeof(obj.name));
                                if (unit.length() > 0) strlcpy(obj.unit_text, unit.c_str(), sizeof(obj.unit_text));
                                obj.enabled = poll;
                                break; // Objet trouvé et modifié
                            }
                        }
                        break; // Device trouvé et modifié
                    }
                }
                // LIBÉRATION IMMÉDIATE DU MUTEX
                // Le Core 1 (MS/TP) peut à nouveau répondre aux requêtes BACnet !
                xSemaphoreGive(cache_mutex); 
            }

            // -------------------------------------------------------------
            // PHASE 2 : Écriture Flash NVS (Lente, ~100 ms)
            // -------------------------------------------------------------
            // La fonction save_device_objects va re-verrouiller le mutex
            // de manière optimisée par "paquets" ou pages, laissant au Core 1
            // le temps de s'intercaler si le bus RS-485 est actif.
            save_device_objects(did);

            // -------------------------------------------------------------
            // PHASE 3 : Signalement au Gatekeeper MQTT (Asynchrone)
            // -------------------------------------------------------------
            trigger_ha_discovery(did, inst, type);
            
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
                // Si l'équipement vient d'être réactivé, il faut que le Core 0 
                // publie ses entités vers Home Assistant via le drapeau atomique
                trigger_ha_discovery();

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
            if (request->getParam("pass", true)->value() != "******")
                strlcpy(sysCfg.wifi_pass, request->getParam("pass", true)->value().c_str(), 64);
            sysCfg.static_ip = request->hasParam("static_ip", true);
            strlcpy(sysCfg.local_ip, request->getParam("local_ip", true)->value().c_str(), 16);
            strlcpy(sysCfg.gateway, request->getParam("gateway", true)->value().c_str(), 16);
            strlcpy(sysCfg.subnet, request->getParam("subnet", true)->value().c_str(), 16);
        } else if (ft == "mqtt") {
            strlcpy(sysCfg.mqtt_server, request->getParam("mqh", true)->value().c_str(), 32);
            strlcpy(sysCfg.mqtt_user, request->getParam("mqu", true)->value().c_str(), 32);
            if (request->getParam("mqp", true)->value() != "******")
                strlcpy(sysCfg.mqtt_pass, request->getParam("mqp", true)->value().c_str(), 32);
            strlcpy(sysCfg.mqtt_prefix, request->getParam("mqpr", true)->value().c_str(), 64);
            sysCfg.ha_discover = request->hasParam("ha_disc", true);
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
            if (request->getParam("admin_p", true)->value() != "******")
                strlcpy(sysCfg.admin_pass, request->getParam("admin_p", true)->value().c_str(), 64);
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
        if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(1000))) {
            for (auto& dev : bacnet_network_cache) {
                char ns[16]; snprintf(ns, sizeof(ns), "dev_%lu", (unsigned long)dev.device_id);
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
        trigger_ha_discovery();
        request->send(200, "text/plain", "Discovery triggered");
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
