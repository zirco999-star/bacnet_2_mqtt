#include "z_network.h"
#include "z_ui.h"
#include "z_bacnet.h"
#include "z_mqtt.h"
#include <ArduinoJson.h>
#include <Update.h>
#include <ESPmDNS.h>
#include "esp_wifi.h"
#include <stdarg.h>
#include "nvs_flash.h"

extern AsyncWebSocket ws;
static uint32_t wifi_connect_start = 0;
static bool wifi_fallback_active = false;

// Structure statique pour la queue (256 octets par log)
typedef struct {
    char message[256];
} LogMessage;

QueueHandle_t log_queue = NULL;

// Tâche Gatekeeper pour les WebSockets (Core 0)
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

// FIX v5.6: Envoi via Queue pour ne pas bloquer le Core 1
void z_log(const char* format, ...) {
    LogMessage log_msg;
    va_list arg;
    va_start(arg, format);
    vsnprintf(log_msg.message, sizeof(log_msg.message), format, arg);
    va_end(arg);
    
    printf("%s", log_msg.message); 

    if (log_queue != NULL) {
        // Timeout 0 pour garantir que le Core 1 ne bloque jamais
        xQueueSend(log_queue, &log_msg, 0);
    }
}

void load_device_objects(uint32_t device_id) {
    if (cache_mutex == NULL) cache_mutex = xSemaphoreCreateMutex();
    char ns[16]; snprintf(ns, sizeof(ns), "dev_%lu", (unsigned long)device_id);
    Preferences prefs;
    z_log("[NVS] Restore Attempt: %s\n", ns);
    
    if (prefs.begin(ns, true)) {
        // FIX v5.6.1: Utilisation de malloc standard (plus sûr au boot que ps_malloc)
        BACnetPersistenceDev* data = (BACnetPersistenceDev*)malloc(sizeof(BACnetPersistenceDev));
        if (data == NULL) { z_log("[NVS] ERROR: Malloc failed\n"); prefs.end(); return; }
        
        bool loaded = false;

        if (prefs.isKey("blob_v2")) {
            if (prefs.getBytes("blob_v2", data, sizeof(BACnetPersistenceDev)) > 0) {
                loaded = true;
            }
        } 
        else if (prefs.isKey("blob")) {
            // Migration: On alloue aussi l'ancienne structure sur le heap
            BACnetPersistenceDev_v1* old = (BACnetPersistenceDev_v1*)malloc(sizeof(BACnetPersistenceDev_v1));
            if (old != NULL) {
                if (prefs.getBytes("blob", old, sizeof(BACnetPersistenceDev_v1)) > 0) {
                    z_log("[NVS] MIGRATING %lu to v2...\n", (unsigned long)device_id);
                    memset(data, 0, sizeof(BACnetPersistenceDev));
                    data->device_id = old->device_id;
                    data->mac_address = old->mac_address;
                    data->enabled = old->enabled;
                    strlcpy(data->name, old->name, 32);
                    strlcpy(data->vendor, old->vendor, 32);
                    data->count = old->count;
                    data->discovery_done = old->discovery_done;
                    for (int i = 0; i < old->count; i++) {
                        data->objects[i].val = old->objects[i].val;
                        data->objects[i].poll = old->objects[i].poll;
                        strlcpy(data->objects[i].name, old->objects[i].name, 20);
                        String u = get_unit_text(old->objects[i].units);
                        strlcpy(data->objects[i].unit_text, u.c_str(), 11);
                    }
                    prefs.end(); prefs.begin(ns, false);
                    prefs.putBytes("blob_v2", data, sizeof(BACnetPersistenceDev));
                    prefs.remove("blob");
                    loaded = true;
                }
                free(old);
            }
        }

        if (loaded) {
            if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(100))) {
                bool found = false;
                for(auto& d : bacnet_network_cache) if(d.device_id == device_id) found = true;
                if (!found) {
                    BACnetDevice dev;
                    dev.device_id = data->device_id;
                    dev.mac_address = data->mac_address;
                    dev.enabled = data->enabled;
                    dev.name = String(data->name);
                    dev.vendor = String(data->vendor);
                    dev.discovery_done = data->discovery_done;
                    dev.disc_step = (DISC_STEP_T)data->disc_step;
                    dev.disc_obj_idx = data->disc_obj_idx;

                    for (int i = 0; i < data->count; i++) {
                        BACnetObject obj;
                        obj.type = data->objects[i].val >> 22;
                        obj.instance = data->objects[i].val & 0x3FFFFF;
                        obj.name = String(data->objects[i].name);
                        obj.enabled = data->objects[i].poll;
                        obj.unit_text = String(data->objects[i].unit_text);
                        obj.present_value = 0.0f;
                        obj.last_update = 0;
                        dev.objects.push_back(obj);
                    }
                    dev.last_seen = millis();
                    bacnet_network_cache.push_back(dev);
                    z_log("[NVS] SUCCESS: Loaded %lu (%d objs, ScanDone:%d, Step:%d, Idx:%d)\n", 
                        (unsigned long)device_id, (int)dev.objects.size(), dev.discovery_done, (int)dev.disc_step, dev.disc_obj_idx);
                }
                xSemaphoreGive(cache_mutex);
            }
        }
        free(data);
        prefs.end();
    }
}

void load_configuration() {
    // ... default init ...
    strlcpy(sysCfg.wifi_ssid, "", 32);
    strlcpy(sysCfg.wifi_pass, "", 64);
    sysCfg.static_ip = true; 
    strlcpy(sysCfg.local_ip, DEFAULT_STATIC_IP, 16);
    strlcpy(sysCfg.gateway, DEFAULT_GATEWAY, 16);
    strlcpy(sysCfg.subnet, DEFAULT_SUBNET, 16);
    sysCfg.mac_address = DEFAULT_MAC_ADDRESS; 
    sysCfg.max_master = DEFAULT_MAX_MASTER; 
    sysCfg.device_id = DEFAULT_DEVICE_ID;
    sysCfg.apdu_timeout = DEFAULT_APDU_TIMEOUT; 
    sysCfg.max_retries = DEFAULT_MAX_RETRIES;
    strlcpy(sysCfg.mqtt_server, DEFAULT_MQTT_SERVER, 32);
    sysCfg.mqtt_port = 1883;
    strlcpy(sysCfg.mqtt_user, "", 32);
    strlcpy(sysCfg.mqtt_pass, "", 32);
    strlcpy(sysCfg.admin_user, "admin", 32);
    strlcpy(sysCfg.admin_pass, "admin1234", 64);

    Preferences prefs;
    if (prefs.begin("system", false)) {
        if (!prefs.isKey("init")) {
            prefs.putBool("init", true);
            z_log("[NVS] Bootstrap: Created 'system' namespace\n");
        }
        if (prefs.isKey("ssid")) prefs.getString("ssid", sysCfg.wifi_ssid, 32);
        if (prefs.isKey("pass")) prefs.getString("pass", sysCfg.wifi_pass, 64);
        if (prefs.isKey("static")) sysCfg.static_ip = prefs.getBool("static", true);
        if (prefs.isKey("ip")) prefs.getString("ip", sysCfg.local_ip, 16);
        if (prefs.isKey("gw")) prefs.getString("gw", sysCfg.gateway, 16);
        if (prefs.isKey("sn")) prefs.getString("sn", sysCfg.subnet, 16);
        if (prefs.isKey("mqh")) prefs.getString("mqh", sysCfg.mqtt_server, 32);
        if (prefs.isKey("mqu")) prefs.getString("mqu", sysCfg.mqtt_user, 32);
        if (prefs.isKey("mqp")) prefs.getString("mqp", sysCfg.mqtt_pass, 32);
        if (prefs.isKey("mqpr")) prefs.getString("mqpr", sysCfg.mqtt_prefix, 64);
        if (prefs.isKey("mac")) sysCfg.mac_address = prefs.getUChar("mac", 1);
        if (prefs.isKey("mm")) sysCfg.max_master = prefs.getUChar("mm", 127);
        if (prefs.isKey("did")) sysCfg.device_id = prefs.getUInt("did", 123);
        if (prefs.isKey("to")) sysCfg.apdu_timeout = prefs.getUShort("to", 500);
        if (prefs.isKey("ret")) sysCfg.max_retries = prefs.getUChar("ret", 3);
        sysCfg.heartbeat_interval = prefs.getUInt("hbeat", DEFAULT_HEARBEAT_INTERVAL);
        sysCfg.token_skip = prefs.getUChar("tskip", DEFAULT_TOKEN_SKIP);
        sysCfg.mqtt_poll_interval = prefs.getUShort("mpi", DEFAULT_MQTT_POLL);
        prefs.end();
        z_log("[NVS] Core Config Loaded (v5.6.8)\n");
    }

    // Chargement de la liste des devices distants via namespace 'registry'
    Preferences reg;
    String dev_list = "";
    if (reg.begin("registry", true)) {
        if (reg.isKey("dev_list")) {
            dev_list = reg.getString("dev_list", "");
            z_log("[NVS] Registry found: %s\n", dev_list.c_str());
        } else { z_log("[NVS] Registry empty (dev_list missing)\n"); }
        reg.end();
    }

    if (dev_list.length() > 0) {
        int start = 0;
        int end = dev_list.indexOf(';');
        while (end != -1) {
            uint32_t id = dev_list.substring(start, end).toInt();
            if (id > 0) load_device_objects(id);
            start = end + 1;
            end = dev_list.indexOf(';', start);
        }
        uint32_t last_id = dev_list.substring(start).toInt();
        if (last_id > 0) load_device_objects(last_id);
    }
}

void save_configuration() {
    Preferences prefs;
    if (prefs.begin("system", false)) {
        prefs.putString("ssid", sysCfg.wifi_ssid);
        prefs.putString("pass", sysCfg.wifi_pass);
        prefs.putBool("static", sysCfg.static_ip);
        prefs.putString("ip", sysCfg.local_ip);
        prefs.putString("gw", sysCfg.gateway);
        prefs.putString("sn", sysCfg.subnet);
        prefs.putUChar("mac", sysCfg.mac_address);
        prefs.putUChar("mm", sysCfg.max_master);
        prefs.putUInt("did", sysCfg.device_id);
        prefs.putUShort("to", sysCfg.apdu_timeout);
        prefs.putUChar("ret", sysCfg.max_retries);
        prefs.putUInt("hbeat", sysCfg.heartbeat_interval);
        prefs.putUChar("tskip", sysCfg.token_skip);
        prefs.putString("mqh", sysCfg.mqtt_server);
        prefs.putString("mqu", sysCfg.mqtt_user);
        prefs.putString("mqp", sysCfg.mqtt_pass);
        prefs.putString("mqpr", sysCfg.mqtt_prefix);
        prefs.putUShort("mpi", sysCfg.mqtt_poll_interval);
        prefs.end();
        z_log("[NVS] Configuration saved\n");
    }
}

void save_device_objects(uint32_t device_id) {
    char ns[16]; snprintf(ns, sizeof(ns), "dev_%lu", (unsigned long)device_id);
    Preferences prefs;
    for (auto& dev : bacnet_network_cache) {
        if (dev.device_id == device_id) {
            if (prefs.begin(ns, false)) {
                // FIX v5.6.1: Utilisation de malloc standard (plus sûr que ps_malloc sur boot)
                BACnetPersistenceDev* data = (BACnetPersistenceDev*)malloc(sizeof(BACnetPersistenceDev));
                if (data != NULL) {
                    memset(data, 0, sizeof(BACnetPersistenceDev));
                    data->device_id = dev.device_id;
                    data->mac_address = dev.mac_address;
                    data->enabled = dev.enabled;
                    data->discovery_done = dev.discovery_done;
                    data->disc_step = (uint8_t)dev.disc_step;
                    data->disc_obj_idx = dev.disc_obj_idx;
                    strlcpy(data->name, dev.name.c_str(), 32);
                    strlcpy(data->vendor, dev.vendor.c_str(), 32);
                    data->count = (uint8_t)std::min((int)dev.objects.size(), 100);
                    for (int i = 0; i < data->count; i++) {
                        data->objects[i].val = ((uint32_t)dev.objects[i].type << 22) | (dev.objects[i].instance & 0x3FFFFF);
                        strlcpy(data->objects[i].name, dev.objects[i].name.c_str(), 20);
                        strlcpy(data->objects[i].unit_text, dev.objects[i].unit_text.c_str(), 11);
                        data->objects[i].poll = dev.objects[i].enabled;
                    }
                    prefs.putBytes("blob_v2", data, sizeof(BACnetPersistenceDev));
                    free(data);
                }
                prefs.end();
                
                // Enregistrement ROBUSTE dans le registre global
                Preferences reg;
                if (reg.begin("registry", false)) {
                    String list = reg.getString("dev_list", "");
                    String id_str = String(device_id);
                    // Check avec délimiteurs pour éviter les faux positifs (ex: 123 dans 1123)
                    String check_list = ";" + list + ";";
                    if (check_list.indexOf(";" + id_str + ";") == -1) {
                        if (list.length() > 0) list += ";";
                        list += id_str;
                        reg.putString("dev_list", list);
                        z_log("[NVS] Registered device %lu in registry\n", (unsigned long)device_id);
                    }
                    reg.end();
                }
                z_log("[NVS] SUCCESS: Saved %lu to v2\n", (unsigned long)device_id);
            }
            break;
        }
    }
}

void setup_network_infrastructure() {
    // Initialisation de la Queue de logs (20 messages max)
    log_queue = xQueueCreate(20, sizeof(LogMessage));
    if (log_queue != NULL) {
        xTaskCreatePinnedToCore(websocket_log_task, "WS_Log", 4096, NULL, 2, NULL, 0);
    }

    load_configuration();
    WiFi.persistent(false); WiFi.disconnect(true);
    vTaskDelay(pdMS_TO_TICKS(100));

    if (strlen(sysCfg.wifi_ssid) == 0) {
        is_ap_mode = true; WiFi.mode(WIFI_AP);
        WiFi.softAP("ZIRCON-GW-CONFIG", "admin1234");
        z_log("[WIFI] Mode AP: ZIRCON-GW-CONFIG\n");
    } else {
        is_ap_mode = false; WiFi.mode(WIFI_STA);
        esp_wifi_set_ps(WIFI_PS_NONE);
        if (sysCfg.static_ip) {
            IPAddress ip, gw, sn;
            if (ip.fromString(sysCfg.local_ip) && gw.fromString(sysCfg.gateway) && sn.fromString(sysCfg.subnet)) {
                WiFi.config(ip, gw, sn);
                z_log("[WIFI] Static IP applied: %s\n", sysCfg.local_ip);
            }
        }
        WiFi.begin(sysCfg.wifi_ssid, sysCfg.wifi_pass);
        wifi_connect_start = millis();
        z_log("[WIFI] Connecting to %s...\n", sysCfg.wifi_ssid);
    }

    webServer.addHandler(&ws);
    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        request->send_P(200, "text/html", INDEX_HTML);
    });

    webServer.on("/api/objects", HTTP_GET, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        JsonDocument doc; JsonArray controllers = doc.to<JsonArray>();
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
                    obj["unit"] = o.unit_text; // AJOUT UNITÉ
                }
            }
            xSemaphoreGive(cache_mutex);
        }
        String response; serializeJson(doc, response);
        request->send(200, "application/json", response);
    });

    webServer.on("/reboot", HTTP_GET, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        request->send(200, "text/plain", "REBOOTING...");
        pending_reboot = true; reboot_timer = millis();
    });

    webServer.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        JsonDocument doc;
        doc["ver"] = VERSION_GLOBAL;
        doc["rssi"] = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
        doc["ip"] = is_ap_mode ? "192.168.4.1" : WiFi.localIP().toString();
        doc["mqtt"] = is_mqtt_connected();
        doc["heap"] = ESP.getFreeHeap() / 1024;
        doc["mac_id"] = sysCfg.mac_address;
        // BACNET Badge OK if tokens OR RX packets seen
        doc["mstp_t"] = (bacnetStats.tokens_seen > 0 || bacnetStats.ms_msgs_rx > 0); 
        doc["mstp_cnt"] = bacnetStats.tokens_seen;
        doc["ssid"] = sysCfg.wifi_ssid;
        doc["static"] = sysCfg.static_ip;
        doc["gw"] = sysCfg.gateway; doc["sn"] = sysCfg.subnet;
        doc["mqh"] = sysCfg.mqtt_server;
        doc["mqpr"] = sysCfg.mqtt_prefix;
        doc["mm"] = sysCfg.max_master;
        doc["did"] = sysCfg.device_id;
        doc["to"] = sysCfg.apdu_timeout;
        doc["ret"] = sysCfg.max_retries;
        doc["hbeat"] = sysCfg.heartbeat_interval;
        doc["tskip"] = sysCfg.token_skip;
        doc["mpi"] = sysCfg.mqtt_poll_interval;
        String res; serializeJson(doc, res);
        request->send(200, "application/json", res);
    });

    webServer.on("/api/whois", HTTP_POST, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        BACnetJob job; job.type = JOB_WHO_IS;
        enqueue_bacnet_job(job);
        request->send(200, "text/plain", "WHO-IS ENQUEUED");
    });

    webServer.on("/api/reset_cache", HTTP_POST, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        z_log("[NVS] BACnet Global Cache Reset\n");
        
        // 1. On nettoie les namespaces individuels via le registre
        Preferences reg;
        if (reg.begin("registry", false)) {
            String dev_list = reg.getString("dev_list", "");
            if (dev_list.length() > 0) {
                int start = 0;
                int end = dev_list.indexOf(';');
                while (start < dev_list.length()) {
                    String sid = (end == -1) ? dev_list.substring(start) : dev_list.substring(start, end);
                    if (sid.length() > 0) {
                        char ns[16]; snprintf(ns, sizeof(ns), "dev_%s", sid.c_str());
                        Preferences p; p.begin(ns, false); p.clear(); p.end();
                        z_log("[NVS] Cleared namespace %s\n", ns);
                    }
                    if (end == -1) break;
                    start = end + 1;
                    end = dev_list.indexOf(';', start);
                }
            }
            reg.remove("dev_list");
            reg.end();
        }

        // 2. On vide la RAM
        if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(500))) {
            bacnet_network_cache.clear();
            xSemaphoreGive(cache_mutex);
        }

        request->send(200, "text/plain", "BACNET CACHE CLEARED - REBOOTING");
        pending_reboot = true; reboot_timer = millis();
    });

    webServer.on("/api/save_objects", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        if(!is_authenticated(request)) return;
        
        if (index == 0) {
            // Premier fragment : on alloue le buffer
            request->_tempObject = malloc(total + 1);
            if (!request->_tempObject) { request->send(500, "text/plain", "OOM"); return; }
        }

        if (request->_tempObject) {
            memcpy((uint8_t*)request->_tempObject + index, data, len);
        }

        if (index + len == total) {
            // Dernier fragment : on traite le JSON
            ((uint8_t*)request->_tempObject)[total] = '\0';
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, (const char*)request->_tempObject);
            free(request->_tempObject); request->_tempObject = NULL;

            if (!err) {
                uint32_t device_id = doc["device_id"];
                JsonArray objects = doc["objects"];
                for (auto& dev : bacnet_network_cache) {
                    if (dev.device_id == device_id) {
                        if (doc.containsKey("enabled")) dev.enabled = doc["enabled"].as<bool>();
                        for (JsonObject o : objects) {
                            uint32_t inst = o["inst"];
                            uint16_t type = o["type"];
                            for (auto& obj : dev.objects) {
                                if (obj.instance == inst && obj.type == type) {
                                    // 1. Gestion du NOM
                                    if (o.containsKey("name")) {
                                        String new_name = o["name"].as<String>();
                                        if (new_name != obj.name) {
                                            obj.name = new_name;
                                            // A. Sauvegarde NVS & RAM faite via save_device_objects en fin de boucle
                                            
                                            // B. Publication MQTT immédiate (Object_Name = Prop 77)
                                            MQTTPublishJob pub;
                                            pub.device_id = dev.device_id;
                                            pub.obj_type = obj.type;
                                            pub.obj_instance = obj.instance;
                                            pub.prop_id = 77; 
                                            strlcpy(pub.value_string, new_name.c_str(), sizeof(pub.value_string));
                                            enqueue_mqtt_publish(pub);

                                            // C. Déclenchement asynchrone du WriteProperty (Prop 77) sur le bus
                                            BACnetJob job;
                                            job.type = JOB_WRITE_PROP;
                                            job.target_mac = dev.mac_address;
                                            job.obj_type = obj.type;
                                            job.obj_instance = obj.instance;
                                            job.prop_id = 77;
                                            job.name = new_name;
                                            enqueue_bacnet_job(job);
                                        }
                                    }
                                    // 2. Gestion de l'UNITÉ
                                    if (o.containsKey("unit")) obj.unit_text = o["unit"].as<String>();
                                    // 3. Gestion du POLLING
                                    if (o.containsKey("poll")) obj.enabled = o["poll"].as<bool>();
                                    break;
                                }
                            }
                        }
                        save_device_objects(device_id);
                        break;
                    }
                }
                request->send(200, "text/plain", "OK");
            } else request->send(400, "text/plain", "Invalid JSON");
        }
    });

    webServer.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        auto check = [&](const char* p, char* t, size_t m) {
            if(request->hasParam(p, true)) {
                String v = request->getParam(p, true)->value();
                if(v.length() > 0 && v != "******") strlcpy(t, v.c_str(), m);
            }
        };
        check("ssid", sysCfg.wifi_ssid, 32); check("pass", sysCfg.wifi_pass, 64);
        check("local_ip", sysCfg.local_ip, 16); check("gateway", sysCfg.gateway, 16);
        check("subnet", sysCfg.subnet, 16); check("mqh", sysCfg.mqtt_server, 32);
        check("mqu", sysCfg.mqtt_user, 32); check("mqp", sysCfg.mqtt_pass, 32);
        check("mqpr", sysCfg.mqtt_prefix, 64);
        if(request->hasParam("mpi", true)) sysCfg.mqtt_poll_interval = request->getParam("mpi", true)->value().toInt();
        if(request->hasParam("static_ip", true)) sysCfg.static_ip = true;
 
        else if(request->hasParam("form_type", true) && request->getParam("form_type", true)->value() == "wifi") sysCfg.static_ip = false;

        if(request->hasParam("mac", true)) sysCfg.mac_address = request->getParam("mac", true)->value().toInt();
        if(request->hasParam("mm", true)) sysCfg.max_master = request->getParam("mm", true)->value().toInt();
        if(request->hasParam("did", true)) sysCfg.device_id = request->getParam("did", true)->value().toInt();
        if(request->hasParam("timeout", true)) sysCfg.apdu_timeout = request->getParam("timeout", true)->value().toInt();
        if(request->hasParam("retries", true)) sysCfg.max_retries = request->getParam("retries", true)->value().toInt();
        if(request->hasParam("hbeat", true)) sysCfg.heartbeat_interval = request->getParam("hbeat", true)->value().toInt();
        if(request->hasParam("tskip", true)) sysCfg.token_skip = request->getParam("tskip", true)->value().toInt();

        save_configuration();
        request->send(200, "text/plain", "OK");

        if(request->hasParam("form_type", true)) {
            String ft = request->getParam("form_type", true)->value();
            if (ft == "wifi") {
                pending_reboot = true; reboot_timer = millis();
            } else if (ft == "mqtt") {
                setup_mqtt(); // Hot-reload MQTT
            }
        }
    });

    webServer.on("/api/factory_reset", HTTP_POST, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        nvs_flash_erase(); nvs_flash_init();
        request->send(200, "text/plain", "FACTORY RESET OK");
        ESP.restart();
    });

    webServer.begin();
    MDNS.begin("bacnet-gateway");
    ArduinoOTA.begin();
    z_log("[WIFI] OTA Service Started (Port 3232)\n");
}

void handle_network() {
    ArduinoOTA.handle();
    if (!is_ap_mode && !wifi_fallback_active && WiFi.status() != WL_CONNECTED) {
        if (millis() - wifi_connect_start > 45000) {
            WiFi.mode(WIFI_AP); WiFi.softAP("ZIRCON-RECOVERY", "admin1234");
            wifi_fallback_active = true; is_ap_mode = true;
        }
    }
    if (pending_reboot && (millis() - reboot_timer > 2000)) ESP.restart();
}

bool is_authenticated(AsyncWebServerRequest *request) {
    if (!request->authenticate(sysCfg.admin_user, sysCfg.admin_pass)) {
        request->requestAuthentication(); return false;
    }
    return true;
}
