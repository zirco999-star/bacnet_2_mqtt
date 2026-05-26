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
QueueHandle_t log_queue = NULL;
static uint32_t wifi_connect_start = 0;
static bool wifi_fallback_active = false;

void z_log(const char* format, ...) {
    char loc_buf[128];
    va_list arg;
    va_start(arg, format);
    vsnprintf(loc_buf, sizeof(loc_buf), format, arg);
    va_end(arg);
    Serial.print(loc_buf);
    if (log_queue != NULL) {
        char* q_msg = strdup(loc_buf);
        if (q_msg != NULL) {
            if (xQueueSend(log_queue, &q_msg, 0) != pdTRUE) free(q_msg);
        }
    }
}

/**
 * Charge les données d'un équipement BACnet et de ses objets depuis la mémoire non volatile (NVS).
 * @param device_id L'identifiant unique de l'équipement (Device ID) à charger.
 */
void load_device_objects(uint32_t device_id) {
    // Création d'un "namespace" unique dans le NVS pour cet équipement (ex: "dev_1234")
    char ns[16]; snprintf(ns, sizeof(ns), "dev_%lu", (unsigned long)device_id);
    Preferences prefs;
    
    // Ouverture de la partition NVS correspondante
    if (prefs.begin(ns, false)) {
        // Vérification de la présence de l'objet binaire "blob" contenant les données
        if (prefs.isKey("blob")) {
            BACnetPersistenceDev data;
            
            // Lecture du blob binaire directement dans la structure optimisée BACnetPersistenceDev
            if (prefs.getBytes("blob", &data, sizeof(data)) > 0) {
                bool found = false;
                
                // Sécurité : On vérifie que l'équipement n'est pas déjà présent dans le cache RAM
                for(auto& d : bacnet_network_cache) if(d.device_id == device_id) found = true;
                
                if (!found) {
                    // Création et hydratation de la structure RAM dynamique (BACnetDevice)
                    BACnetDevice dev;
                    dev.device_id = data.device_id;
                    dev.enabled = data.enabled;
                    dev.name = String(data.name);
                    dev.vendor = String(data.vendor);
                    
                    // Boucle sur le tableau d'objets (points de données) restaurés
                    for (int i = 0; i < data.count; i++) {
                        BACnetObject obj;
                        // Désérialisation bit à bit : Les 7 bits de poids fort contiennent le type d'objet
                        obj.type = data.objects[i].val >> 25;
                        // Désérialisation : Les 25 bits de poids faible contiennent l'instance de l'objet (Masque 0x1FFFFFF)
                        obj.instance = data.objects[i].val & 0x1FFFFFF;
                        obj.name = String(data.objects[i].name);
                        obj.enabled = data.objects[i].poll;
                        
                        // Initialisation des valeurs dynamiques à 0 (elles seront mises à jour lors du premier polling)
                        obj.present_value = 0;
                        obj.last_update = 0;
                        obj.discovery_done = true;
                        obj.expected_states_count = 0;
                        dev.objects.push_back(obj);
                    }
                    
                    // Indique au moteur MS/TP que la découverte est déjà faite.
                    // L'automate BACnet passera directement en mode "Polling" (Scrutation des valeurs)
                    dev.discovery_done = true;
                    dev.last_seen = millis();
                    
                    // Ajout final au cache réseau (RAM)
                    bacnet_network_cache.push_back(dev);
                    z_log("[NVS] Blob restored for %lu\n", (unsigned long)device_id);
                }
            }
        }
        prefs.end();
    }
}

void load_configuration() {
    Preferences prefs;
    prefs.begin("system", true); // Mode lecture seule
    
    // Chargement depuis le NVS avec fallback sur les valeurs par défaut
    strlcpy(sysCfg.wifi_ssid, prefs.getString("ssid", "").c_str(), 32);
    strlcpy(sysCfg.wifi_pass, prefs.getString("pass", "").c_str(), 64);
    sysCfg.static_ip = prefs.getBool("static", true);
    strlcpy(sysCfg.local_ip, prefs.getString("ip", "192.168.1.50").c_str(), 16);
    
    // CORRECTION : On pointe vers une Gateway valide si le NVS est vierge
    strlcpy(sysCfg.gateway, prefs.getString("gw", "192.168.1.1").c_str(), 16); 
    strlcpy(sysCfg.subnet, prefs.getString("sn", "255.255.255.0").c_str(), 16);
    
    sysCfg.mac_address = prefs.getUChar("mac", 1);
    sysCfg.max_master = prefs.getUChar("mm", 127);
    sysCfg.device_id = prefs.getUInt("did", 1234);
    sysCfg.apdu_timeout = prefs.getUShort("to", 1000);
    sysCfg.max_retries = prefs.getUChar("ret", 3);
    
    strlcpy(sysCfg.mqtt_server, prefs.getString("mqh", "192.168.1.11").c_str(), 32);
    sysCfg.mqtt_port = 1883;
    strlcpy(sysCfg.mqtt_user, prefs.getString("mqu", "").c_str(), 32);
    strlcpy(sysCfg.mqtt_pass, prefs.getString("mqp", "").c_str(), 32);
    strlcpy(sysCfg.mqtt_prefix, prefs.getString("mqpref", "bacnet").c_str(), 64);
    
    strlcpy(sysCfg.admin_user, "admin", 32);
    strlcpy(sysCfg.admin_pass, "admin1234", 64);
    
    prefs.end();
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
        prefs.putString("mqh", sysCfg.mqtt_server);
        prefs.putString("mqu", sysCfg.mqtt_user);
        prefs.putString("mqp", sysCfg.mqtt_pass);
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
                BACnetPersistenceDev data;
                data.device_id = dev.device_id;
                data.enabled = dev.enabled;
                strlcpy(data.name, dev.name.c_str(), 32);
                strlcpy(data.vendor, dev.vendor.c_str(), 32);
                data.count = (uint8_t)std::min((int)dev.objects.size(), 100);
                for (int i = 0; i < data.count; i++) {
                    data.objects[i].val = ((uint32_t)dev.objects[i].type << 22) | (dev.objects[i].instance & 0x3FFFFF);
                    strlcpy(data.objects[i].name, dev.objects[i].name.c_str(), 24);
                    data.objects[i].poll = dev.objects[i].enabled;
                }
                prefs.putBytes("blob", &data, sizeof(data));
                prefs.end();
                z_log("[NVS] Device blob %lu saved\n", (unsigned long)device_id);
            }
            break;
        }
    }
}

void setup_network_infrastructure() {
    log_queue = xQueueCreate(20, sizeof(char*));
    load_configuration();
    WiFi.persistent(false); WiFi.disconnect(true);
    vTaskDelay(pdMS_TO_TICKS(100));

    if (strlen(sysCfg.wifi_ssid) == 0) {
        is_ap_mode = true; WiFi.mode(WIFI_AP);
        WiFi.softAP("BACNET-GW-CONFIG", "admin1234");
        z_log("[WIFI] Mode AP: BACNET-GW-CONFIG\n");
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
                }
            }
            xSemaphoreGive(cache_mutex);
        }
        String response; serializeJson(doc, response);
        request->send(200, "application/json", response);
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
        doc["mqu"] = sysCfg.mqtt_user;
        doc["mqp_set"] = (strlen(sysCfg.mqtt_pass) > 0);
        doc["mm"] = sysCfg.max_master;
        doc["did"] = sysCfg.device_id;
        doc["to"] = sysCfg.apdu_timeout;
        doc["ret"] = sysCfg.max_retries;
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
        z_log("[NVS] Manual cache reset requested\n");
        for (auto& dev : bacnet_network_cache) {
            char ns[16]; snprintf(ns, sizeof(ns), "dev_%lu", (unsigned long)dev.device_id);
            Preferences prefs;
            if (prefs.begin(ns, false)) { prefs.clear(); prefs.end(); }
        }
        char ns[16]; snprintf(ns, sizeof(ns), "dev_%lu", (unsigned long)sysCfg.device_id);
        Preferences prefs;
        if (prefs.begin(ns, false)) { prefs.clear(); prefs.end(); }
        
        bacnet_network_cache.clear();
        request->send(200, "text/plain", "CACHE CLEARED");
        pending_reboot = true; reboot_timer = millis();
    });

    webServer.on("/api/save_objects", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        if(!is_authenticated(request)) return;
        JsonDocument doc;
        if (!deserializeJson(doc, data, len)) {
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
                                if (o.containsKey("name")) obj.name = o["name"].as<String>();
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
        if(request->hasParam("static_ip", true)) sysCfg.static_ip = true; 
        else if(request->hasParam("form_type", true) && request->getParam("form_type", true)->value() == "wifi") sysCfg.static_ip = false;

        if(request->hasParam("mqu", true)) {
            strlcpy(sysCfg.mqtt_user, request->getParam("mqu", true)->value().c_str(), 32);
        }
        if(request->hasParam("mqp", true)) {
            String v = request->getParam("mqp", true)->value();
            if (v != "******") strlcpy(sysCfg.mqtt_pass, v.c_str(), 32);
        }

        if(request->hasParam("mac", true)) sysCfg.mac_address = request->getParam("mac", true)->value().toInt();
        if(request->hasParam("mm", true)) sysCfg.max_master = request->getParam("mm", true)->value().toInt();
        if(request->hasParam("timeout", true)) sysCfg.apdu_timeout = request->getParam("timeout", true)->value().toInt();
        if(request->hasParam("retries", true)) sysCfg.max_retries = request->getParam("retries", true)->value().toInt();

        save_configuration();
        request->send(200, "text/plain", "OK");
        pending_reboot = true; reboot_timer = millis();
    });

    webServer.on("/api/download_ede", HTTP_GET, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        String csv = "keyname;device obj.-instance;object-name;object-type;object-instance;description;unit-code\n";
        if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(100))) {
            for (auto& dev : bacnet_network_cache) {
                for (auto& o : dev.objects) {
                    char line[256];
                    snprintf(line, sizeof(line), "OBJ:%u:%lu;%lu;%s;%u;%lu;%u\n", 
                        o.type, (unsigned long)o.instance, (unsigned long)dev.device_id, o.name.c_str(), o.type, (unsigned long)o.instance, o.units);
                    csv += line;
                }
            }
            xSemaphoreGive(cache_mutex);
        }
        request->send(200, "text/csv", csv);
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
    if (log_queue != NULL) {
        char* msg;
        while (xQueueReceive(log_queue, &msg, 0) == pdTRUE) {
            if (ws.count() > 0) ws.textAll(msg);
            free(msg);
        }
    }
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
