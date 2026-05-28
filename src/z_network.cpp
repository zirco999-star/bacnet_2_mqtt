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

// Structure pour la file d'attente des logs
typedef struct {
    char message[256];
} LogMessage;

QueueHandle_t log_queue = NULL;

// Tâche dédiée à l'envoi des logs via WebSocket (Core 0)
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

// Fonction de log thread-safe et non-bloquante
void z_log(const char* format, ...) {
    LogMessage log_msg;
    va_list arg;
    va_start(arg, format);
    vsnprintf(log_msg.message, sizeof(log_msg.message), format, arg);
    va_end(arg);
    
    printf("%s", log_msg.message); 

    if (log_queue != NULL) {
        xQueueSend(log_queue, &log_msg, 0);
    }
}

/**
 * Charge les données d'un équipement BACnet depuis le NVS.
 * Utilise le cache_mutex pour garantir l'intégrité de la RAM.
 */
void load_device_objects(uint32_t device_id) {
    if (cache_mutex == NULL) cache_mutex = xSemaphoreCreateMutex();
    char ns[16]; 
    snprintf(ns, sizeof(ns), "dev_%lu", (unsigned long)device_id);
    Preferences prefs;
    
    if (prefs.begin(ns, true)) {
        // 1. Lire le Header
        BACnetPersistenceDev head;
        if (prefs.getBytes("head", &head, sizeof(head)) > 0) {
            
            if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(1000))) {
                // Vérifier si le device existe déjà
                bool exists = false;
                for(auto& d : bacnet_network_cache) if(d.device_id == device_id) exists = true;
                
                if (!exists) {
                    BACnetDevice dev;
                    dev.device_id = head.device_id;
                    dev.mac_address = head.mac_address;
                    dev.enabled = head.enabled;
                    dev.name = String(head.name);
                    dev.vendor = String(head.vendor);
                    dev.discovery_done = head.discovery_done;
                    dev.disc_step = (DISC_STEP_T)head.disc_step;
                    dev.disc_obj_idx = head.disc_obj_idx;
                    dev.last_seen = millis();

                    // 2. Charger les objets par pages
                    // On boucle tant qu'on trouve des pages "p0", "p1", etc.
                    for (int p = 0; p * 20 < head.count; p++) {
                        char key[16]; snprintf(key, 16, "p%d", p);
                        BACnetPersistencePage page;
                        
                        if (prefs.getBytes(key, &page, sizeof(page)) > 0) {
                            // On vérifie que la page appartient bien au bon device
                            if (page.device_id != device_id) continue;

                            for (int i = 0; i < 20 && (p * 20 + i) < head.count; i++) {
                                BACnetObject obj;
                                obj.type = page.objects[i].val >> 22;
                                obj.instance = page.objects[i].val & 0x3FFFFF;
                                obj.enabled = page.objects[i].poll;
                                obj.name_published = page.objects[i].name_published;
                                
                                // Copie sécurisée des char[] vers String (ou char[])
                                strlcpy(obj.name, page.objects[i].name, sizeof(obj.name));
                                strlcpy(obj.unit_text, page.objects[i].unit_text, sizeof(obj.unit_text));
                                strlcpy(obj.last_mqtt_name, page.objects[i].name, sizeof(obj.last_mqtt_name));
                                
                                obj.present_value = 0.0f;
                                dev.objects.push_back(obj);
                            }
                        }
                    }
                    dev.last_seen = millis();   
                    bacnet_network_cache.push_back(dev);
                    z_log("[NVS] Restored Device %lu (%d objs)\n", (unsigned long)device_id, (int)dev.objects.size());
                
                }
                
                
                
                xSemaphoreGive(cache_mutex);
            }
        }
        prefs.end();
    }
}

/**
 * Sauvegarde la configuration système (Wi-Fi, MQTT, BACnet params).
 */
void load_configuration() {
    // Initialisation des valeurs par défaut
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
    strlcpy(sysCfg.mqtt_prefix, "bacnet", 64);
    sysCfg.heartbeat_interval = DEFAULT_HEARBEAT_INTERVAL;
    sysCfg.token_skip = DEFAULT_TOKEN_SKIP;
    sysCfg.mqtt_poll_interval = DEFAULT_MQTT_POLL;
    sysCfg.bacnet_poll_interval = DEFAULT_BACNET_POLL;


    Preferences prefs;
    if (prefs.begin("system", true)) {
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
        if (prefs.isKey("bpi")) sysCfg.bacnet_poll_interval = prefs.getUShort("bpi", DEFAULT_BACNET_POLL);
        if (prefs.isKey("hbeat")) sysCfg.heartbeat_interval = prefs.getUInt("hbeat", DEFAULT_HEARBEAT_INTERVAL);
        if (prefs.isKey("tskip")) sysCfg.token_skip = prefs.getUChar("tskip", DEFAULT_TOKEN_SKIP);
        if (prefs.isKey("mpi")) sysCfg.mqtt_poll_interval = prefs.getUShort("mpi", DEFAULT_MQTT_POLL);
        prefs.end();
        z_log("[NVS] Configuration Loaded\n");
    }

    // Chargement de la liste des devices
    Preferences reg;
    String dev_list = "";
    if (reg.begin("registry", true)) {
        dev_list = reg.getString("dev_list", "");
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
        prefs.putUShort("bpi", sysCfg.bacnet_poll_interval);
        prefs.end();
        z_log("[NVS] Configuration System Saved\n");
    }
}

/**
 * Version thread-safe (sans verrouillage interne) de la sauvegarde d'un équipement.
 * Doit être appelée alors que cache_mutex est déjà détenu.
 */
void save_device_objects_locked(uint32_t device_id) {
    char ns[16]; 
    snprintf(ns, sizeof(ns), "dev_%lu", (unsigned long)device_id);
    Preferences prefs;
    
    for (auto& dev : bacnet_network_cache) {
        if (dev.device_id == device_id) {
            if (prefs.begin(ns, false)) {
                // 1. Sauvegarde Header (Statique, plus de malloc)
                BACnetPersistenceDev head;
                memset(&head, 0, sizeof(head));
                head.device_id = dev.device_id;
                head.mac_address = dev.mac_address;
                head.enabled = dev.enabled;
                head.discovery_done = dev.discovery_done;
                head.disc_step = (uint8_t)dev.disc_step;
                head.disc_obj_idx = dev.disc_obj_idx;
                strlcpy(head.name, dev.name.c_str(), 32);
                strlcpy(head.vendor, dev.vendor.c_str(), 32);
                head.count = (uint8_t)std::min((int)dev.objects.size(), 100);
                
                prefs.putBytes("head", &head, sizeof(head));

                // 2. Sauvegarde des objets par pages de 20 (Pagination)
                // Chaque page fait ~960 octets, bien en dessous des 1984 octets NVS
                for (int p = 0; p * 20 < head.count; p++) {
                    BACnetPersistencePage page;
                    memset(&page, 0, sizeof(page));
                    page.device_id = dev.device_id;
                    page.page_index = p;

                    for (int i = 0; i < 20 && (p * 20 + i) < head.count; i++) {
                        auto& o = dev.objects[p * 20 + i];
                        page.objects[i].val = ((uint32_t)o.type << 22) | (o.instance & 0x3FFFFF);
                        strlcpy(page.objects[i].name, o.name, 32); // Utilisation char[]
                        strlcpy(page.objects[i].unit_text, o.unit_text, 12);
                        page.objects[i].poll = o.enabled;
                        page.objects[i].name_published = o.name_published;

                        // Publication MQTT conditionnelle
                        if (o.enabled && !o.name_published) {
                            MQTTPublishJob pub;
                            pub.device_id = dev.device_id; 
                            pub.obj_type = o.type;
                            pub.obj_instance = o.instance; 
                            pub.prop_id = 77; 
                            strlcpy(pub.value_string, o.name, sizeof(pub.value_string));
                            pub.retain = true;
                            enqueue_mqtt_publish(pub);
                            
                            o.name_published = true;
                            strlcpy(o.last_mqtt_name, o.name, 50);
                            z_log("[MQTT] Enqueue publish: %s\n", o.name);
                        }
                    }
                    
                    char key[16]; snprintf(key, 16, "p%d", p);
                    prefs.putBytes(key, &page, sizeof(page));
                }
                prefs.end();

                // 3. Mise à jour du registre global (inchangé)
                Preferences reg;
                if (reg.begin("registry", false)) {
                    String list = reg.getString("dev_list", "");
                    if (list.indexOf(String(device_id)) == -1) {
                        list += (list.length() > 0 ? ";" : "") + String(device_id);
                        reg.putString("dev_list", list);
                    }
                    reg.end();
                }
                z_log("[NVS] SUCCESS: Saved Device %lu\n", (unsigned long)device_id);
            }
            break; 
        }
    }
}

/**
 * Version publique de la sauvegarde d'un équipement avec gestion du mutex.
 */
void save_device_objects(uint32_t device_id) {
    if (cache_mutex == NULL) return;
    if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(1000))) {
        save_device_objects_locked(device_id);
        xSemaphoreGive(cache_mutex);
    }
}

void setup_network_infrastructure() {
    log_queue = xQueueCreate(20, sizeof(LogMessage));
    if (log_queue != NULL) {
        xTaskCreatePinnedToCore(websocket_log_task, "WS_Log", 4096, NULL, 2, NULL, 0);
    }

    // Chargement Config
    strlcpy(sysCfg.admin_user, "admin", 32);
    strlcpy(sysCfg.admin_pass, "admin1234", 64);
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
        if(!is_authenticated(request)) return;
        request->send_P(200, "text/html", INDEX_HTML);
    });

    webServer.on("/api/objects", HTTP_GET, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        JsonDocument* doc_ptr = new JsonDocument();
        if (doc_ptr == NULL) { request->send(500, "text/plain", "OOM"); return; }
        JsonDocument& doc = *doc_ptr;
        JsonArray controllers = doc.to<JsonArray>();

        if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(1000))) {
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

    webServer.on("/reboot", HTTP_GET, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        request->send(200, "text/plain", "REBOOTING...");
        pending_reboot = true; reboot_timer = millis();
    });

    webServer.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        JsonDocument* doc_ptr = new JsonDocument();
        if (doc_ptr == NULL) { request->send(500, "text/plain", "OOM"); return; }
        JsonDocument& doc = *doc_ptr;
        doc["ver"] = VERSION_GLOBAL;
        doc["rssi"] = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
        doc["ip"] = is_ap_mode ? "192.168.4.1" : WiFi.localIP().toString();
        doc["mqtt"] = is_mqtt_connected();
        doc["heap"] = ESP.getFreeHeap() / 1024;
        doc["mac_id"] = sysCfg.mac_address;
        doc["mstp_t"] = (bacnetStats.tokens_seen > 0 || bacnetStats.ms_msgs_rx > 0); 
        doc["mstp_cnt"] = bacnetStats.tokens_seen;
        doc["ssid"] = sysCfg.wifi_ssid;
        doc["static"] = sysCfg.static_ip;
        doc["gw"] = sysCfg.gateway; doc["sn"] = sysCfg.subnet;
        doc["mqh"] = sysCfg.mqtt_server; doc["mqpr"] = sysCfg.mqtt_prefix;
        doc["mm"] = sysCfg.max_master; doc["did"] = sysCfg.device_id;
        doc["to"] = sysCfg.apdu_timeout; doc["ret"] = sysCfg.max_retries;
        doc["hbeat"] = sysCfg.heartbeat_interval; doc["tskip"] = sysCfg.token_skip;
        doc["mpi"] = sysCfg.mqtt_poll_interval;
        String res; serializeJson(doc, res);
        request->send(200, "application/json", res);
        delete doc_ptr;
    });

    webServer.on("/api/whois", HTTP_POST, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        BACnetJob job; job.type = JOB_WHO_IS;
        enqueue_bacnet_job(job);
        request->send(200, "text/plain", "WHO-IS ENQUEUED");
    });
    webServer.on("/api/reset_cache", HTTP_POST, [](AsyncWebServerRequest *request) {
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
        z_log("[NVS] BACnet Global Cache Cleared\n");
        pending_reboot = true; reboot_timer = millis();
    });


    webServer.on("/api/save_objects", HTTP_POST, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        request->_tempObject = NULL; 
    }, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        if(!is_authenticated(request)) return;
        if (index == 0) {
            if (total > 65536) { request->send(413, "text/plain", "Too Large"); return; }
            request->_tempObject = malloc(total + 1);
            if (!request->_tempObject) { request->send(500, "text/plain", "OOM"); return; }
        }
        if (request->_tempObject) memcpy((uint8_t*)request->_tempObject + index, data, len);

        if (index + len == total && request->_tempObject) {
            ((uint8_t*)request->_tempObject)[total] = '\0';
            JsonDocument* doc_ptr = new JsonDocument();
            if (doc_ptr != NULL) {
                JsonDocument& doc = *doc_ptr;
                DeserializationError err = deserializeJson(doc, (const char*)request->_tempObject);
                if (!err) {
                    uint32_t device_id = doc["device_id"];
                    JsonArray objects = doc["objects"];
                    if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(2000))) {
                        for (auto& dev : bacnet_network_cache) {
                            if (dev.device_id == device_id) {
                                if (doc.containsKey("enabled")) dev.enabled = doc["enabled"].as<bool>();
                                for (JsonObject o : objects) {
                                    uint32_t inst = o["inst"]; uint16_t type = o["type"];
                                    for (auto& obj : dev.objects) {
                                        if (obj.instance == inst && obj.type == type) {
                                            if (o.containsKey("name")) {
                                                char new_name[50];
                                                // CORRECTION : .as<const char*>() et non .as<String>()
                                                strlcpy(new_name, o["name"].as<const char*>(), sizeof(new_name));

                                                if (strcmp(obj.name, new_name) != 0) {
                                                    // CORRECTION : Copie directe de new_name (char[]), pas de .c_str()
                                                    strlcpy(obj.name, new_name, sizeof(obj.name));
                                                    
                                                    MQTTPublishJob pub;
                                                    pub.device_id = dev.device_id; 
                                                    pub.obj_type = obj.type;
                                                    pub.obj_instance = obj.instance; 
                                                    pub.prop_id = 77; 
                                                    
                                                    // CORRECTION : Pas de .c_str() sur new_name
                                                    strlcpy(pub.value_string, new_name, sizeof(pub.value_string));
                                                    pub.retain = true;
                                                    enqueue_mqtt_publish(pub);
                                                    
                                                    BACnetJob job; 
                                                    job.type = JOB_WRITE_PROP; 
                                                    job.target_mac = dev.mac_address;
                                                    job.obj_type = obj.type; 
                                                    job.obj_instance = obj.instance;
                                                    job.prop_id = 77; 
                                                    job.name = String(new_name);
                                                    enqueue_bacnet_job(job);
                                                }
                                            }
                                            if (o.containsKey("unit")) strlcpy(obj.unit_text, o["unit"].as<const char*>(), sizeof(obj.unit_text));
                                            if (o.containsKey("poll")) obj.enabled = o["poll"].as<bool>();
                                            break;
                                        }
                                    }
                                }
                                save_device_objects_locked(device_id);
                                break;
                            }
                        }
                        xSemaphoreGive(cache_mutex);
                        request->send(200, "text/plain", "OK");
                    } else request->send(503, "text/plain", "Busy");
                } else request->send(400, "text/plain", "Invalid JSON");
                delete doc_ptr;
            } else request->send(500, "text/plain", "OOM_JSON");
            free(request->_tempObject); request->_tempObject = NULL;
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
            if (ft == "wifi") { pending_reboot = true; reboot_timer = millis(); }
            else if (ft == "mqtt") setup_mqtt(); 
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
    z_log("[WIFI] System Infrastructure Operational.\n");
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
