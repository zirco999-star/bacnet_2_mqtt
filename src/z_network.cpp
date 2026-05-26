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

// FIX v4.5.25: Envoi WebSocket systématique si client connecté
void z_log(const char* format, ...) {
    char loc_buf[256];
    va_list arg;
    va_start(arg, format);
    vsnprintf(loc_buf, sizeof(loc_buf), format, arg);
    va_end(arg);
    printf("%s", loc_buf); 
    if (ws.count() > 0) {
        ws.textAll(loc_buf);
    }
}

void load_device_objects(uint32_t device_id) {
    if (cache_mutex == NULL) cache_mutex = xSemaphoreCreateMutex(); // FIX CRITICAL CRASH
    char ns[16]; snprintf(ns, sizeof(ns), "dev_%lu", (unsigned long)device_id);
    Preferences prefs;
    z_log("[NVS] Attempting restore: %s\n", ns);
    if (prefs.begin(ns, true)) {
        if (prefs.isKey("blob")) {
            BACnetPersistenceDev data;
            if (prefs.getBytes("blob", &data, sizeof(data)) > 0) {
                if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(100))) {
                    bool found = false;
                    for(auto& d : bacnet_network_cache) if(d.device_id == device_id) found = true;
                    if (!found) {
                        BACnetDevice dev;
                        dev.device_id = data.device_id;
                        dev.mac_address = data.mac_address;
                        dev.enabled = data.enabled;
                        dev.name = String(data.name);
                        dev.vendor = String(data.vendor);
                        dev.discovery_done = data.discovery_done; // RESTAURATION ÉTAT
                        for (int i = 0; i < data.count; i++) {
                            BACnetObject obj;
                            obj.type = data.objects[i].val >> 25;
                            obj.instance = data.objects[i].val & 0x1FFFFFF;
                            obj.name = String(data.objects[i].name);
                            obj.enabled = data.objects[i].poll;
                            obj.units = data.objects[i].units;
                            obj.unit_text = get_unit_text(obj.units);
                            obj.present_value = 0.0f;
                            obj.last_update = 0;
                            dev.objects.push_back(obj);
                        }
                        dev.last_seen = millis();
                        bacnet_network_cache.push_back(dev);
                        z_log("[NVS] SUCCESS: Restored %lu (MAC:%u, Done:%d)\n", (unsigned long)device_id, dev.mac_address, dev.discovery_done);
                    }
                    xSemaphoreGive(cache_mutex);
                }
            } else { z_log("[NVS] ERROR: Failed to read blob for %lu\n", (unsigned long)device_id); }
        } else { z_log("[NVS] INFO: No blob key in %s\n", ns); }
        prefs.end();
    } else { z_log("[NVS] INFO: Namespace %s not found\n", ns); }
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
        if (prefs.isKey("mac")) sysCfg.mac_address = prefs.getUChar("mac", 1);
        if (prefs.isKey("mm")) sysCfg.max_master = prefs.getUChar("mm", 127);
        if (prefs.isKey("did")) sysCfg.device_id = prefs.getUInt("did", 123);
        if (prefs.isKey("to")) sysCfg.apdu_timeout = prefs.getUShort("to", 500);
        if (prefs.isKey("ret")) sysCfg.max_retries = prefs.getUChar("ret", 3);
        sysCfg.heartbeat_interval = prefs.getUInt("hbeat", DEFAULT_HEARBEAT_INTERVAL);
        sysCfg.token_skip = prefs.getUChar("tskip", DEFAULT_TOKEN_SKIP);
        prefs.end();
        z_log("[NVS] Core Config Loaded (GW ID:%lu, MAC:%u)\n", (unsigned long)sysCfg.device_id, sysCfg.mac_address);
    }

    // Chargement de la liste des devices distants via namespace 'registry'
    Preferences reg;
    if (reg.begin("registry", true)) {
        if (reg.isKey("dev_list")) {
            String list = reg.getString("dev_list", "");
            z_log("[NVS] Registry found: %s\n", list.c_str());
            int start = 0;
            int end = list.indexOf(';');
            while (end != -1) {
                uint32_t id = list.substring(start, end).toInt();
                if (id > 0) load_device_objects(id);
                start = end + 1;
                end = list.indexOf(';', start);
            }
            uint32_t last_id = list.substring(start).toInt();
            if (last_id > 0) load_device_objects(last_id);
        } else { z_log("[NVS] Registry empty (dev_list missing)\n"); }
        reg.end();
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
                data.mac_address = dev.mac_address;
                data.enabled = dev.enabled;
                data.discovery_done = dev.discovery_done; // SAUVEGARDE ÉTAT
                strlcpy(data.name, dev.name.c_str(), 32);
                strlcpy(data.vendor, dev.vendor.c_str(), 32);
                data.count = (uint8_t)std::min((int)dev.objects.size(), 100);
                for (int i = 0; i < data.count; i++) {
                    data.objects[i].val = ((uint32_t)dev.objects[i].type << 25) | (dev.objects[i].instance & 0x1FFFFFF);
                    strlcpy(data.objects[i].name, dev.objects[i].name.c_str(), 24);
                    data.objects[i].units = dev.objects[i].units;
                    data.objects[i].poll = dev.objects[i].enabled;
                }
                prefs.putBytes("blob", &data, sizeof(data));
                prefs.end();
                
                // Enregistrement dans le registre global
                Preferences reg;
                if (reg.begin("registry", false)) {
                    String list = reg.getString("dev_list", "");
                    char id_str[16]; snprintf(id_str, sizeof(id_str), "%lu", (unsigned long)device_id);
                    if (list.indexOf(id_str) == -1) {
                        if (list.length() > 0) list += ";";
                        list += id_str;
                        reg.putString("dev_list", list);
                        z_log("[NVS] Registered device %lu in registry\n", (unsigned long)device_id);
                    }
                    reg.end();
                }
                z_log("[NVS] Device blob %lu saved\n", (unsigned long)device_id);
            }
            break;
        }
    }
}

void setup_network_infrastructure() {
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
        doc["mm"] = sysCfg.max_master;
        doc["did"] = sysCfg.device_id;
        doc["to"] = sysCfg.apdu_timeout;
        doc["ret"] = sysCfg.max_retries;
        doc["hbeat"] = sysCfg.heartbeat_interval;
        doc["tskip"] = sysCfg.token_skip;
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
        check("mqu", sysCfg.mqtt_user, 32); check("mqp", sysCfg.mqtt_pass, 32);
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
        pending_reboot = true; reboot_timer = millis();
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
