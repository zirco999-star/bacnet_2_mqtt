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

// FIX v4.5.22: Utilisation de printf() pour un log brut identique à Serial.print
void z_log(const char* format, ...) {
    char loc_buf[256];
    va_list arg;
    va_start(arg, format);
    vsnprintf(loc_buf, sizeof(loc_buf), format, arg);
    va_end(arg);
    
    printf("%s", loc_buf); // Standard C Print (Raw output to Serial)
    
    if (WiFi.status() == WL_CONNECTED && !is_ap_mode && ws.count() > 0) {
        ws.textAll(loc_buf);
    }
}

void load_device_objects(uint32_t device_id) {
    char ns[16]; snprintf(ns, sizeof(ns), "dev_%lu", (unsigned long)device_id);
    Preferences prefs;
    if (prefs.begin(ns, false)) {
        if (prefs.isKey("blob")) {
            BACnetPersistenceDev data;
            if (prefs.getBytes("blob", &data, sizeof(data)) > 0) {
                bool found = false;
                for(auto& d : bacnet_network_cache) if(d.device_id == device_id) found = true;
                if (!found) {
                    BACnetDevice dev;
                    dev.device_id = data.device_id;
                    dev.enabled = data.enabled;
                    dev.name = String(data.name);
                    dev.vendor = String(data.vendor);
                    for (int i = 0; i < data.count; i++) {
                        BACnetObject obj;
                        obj.type = data.objects[i].val >> 25;
                        obj.instance = data.objects[i].val & 0x1FFFFFF;
                        obj.name = String(data.objects[i].name);
                        obj.enabled = data.objects[i].poll;
                        obj.present_value = 0;
                        obj.last_update = 0;
                        dev.objects.push_back(obj);
                    }
                    dev.discovery_done = true;
                    dev.last_seen = millis();
                    bacnet_network_cache.push_back(dev);
                    z_log("[NVS] Blob restored for %lu\n", (unsigned long)device_id);
                }
            }
        }
        prefs.end();
    }
}

void load_configuration() {
    strlcpy(sysCfg.wifi_ssid, "", 32);
    strlcpy(sysCfg.wifi_pass, "", 64);
    sysCfg.static_ip = true; 
    strlcpy(sysCfg.local_ip, "192.168.1.50", 16);
    strlcpy(sysCfg.gateway, "192.168.1.254", 16);
    strlcpy(sysCfg.subnet, "255.255.255.0", 16);
    sysCfg.mac_address = 1; sysCfg.max_master = 127; sysCfg.device_id = 1234;
    sysCfg.apdu_timeout = 1000; sysCfg.max_retries = 3;
    strlcpy(sysCfg.mqtt_server, "192.168.1.11", 32);
    sysCfg.mqtt_port = 1883;
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
        if (prefs.isKey("mac")) sysCfg.mac_address = prefs.getUChar("mac", 1);
        if (prefs.isKey("mm")) sysCfg.max_master = prefs.getUChar("mm", 127);
        if (prefs.isKey("did")) sysCfg.device_id = prefs.getUInt("did", 1234);
        if (prefs.isKey("to")) sysCfg.apdu_timeout = prefs.getUShort("to", 1000);
        if (prefs.isKey("ret")) sysCfg.max_retries = prefs.getUChar("ret", 3);
        prefs.end();
        z_log("[NVS] System config loaded\n");
    }
    load_device_objects(sysCfg.device_id);
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
                    data.objects[i].val = ((uint32_t)dev.objects[i].type << 25) | (dev.objects[i].instance & 0x1FFFFFF);
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
        String response; serializeJson(doc, response);
        request->send(200, "application/json", response);
    });
webServer.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["ver"] = VERSION_GLOBAL;
    doc["rssi"] = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
    doc["ip"] = is_ap_mode ? "192.168.4.1" : WiFi.localIP().toString();
    doc["mqtt"] = is_mqtt_connected();
    doc["heap"] = ESP.getFreeHeap() / 1024;    doc["mac_id"] = sysCfg.mac_address;
    doc["mstp_t"] = bacnetStats.tokens_seen;
    doc["ssid"] = sysCfg.wifi_ssid;
    doc["static"] = sysCfg.static_ip;
    doc["gw"] = sysCfg.gateway; doc["sn"] = sysCfg.subnet;
    doc["mqh"] = sysCfg.mqtt_server;
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

webServer.on("/api/iam", HTTP_POST, [](AsyncWebServerRequest *request) {
    if(!is_authenticated(request)) return;
    request->send(200, "text/plain", "I-AM ENQUEUED");
});

webServer.on("/api/reset_cache", HTTP_POST, [](AsyncWebServerRequest *request) {
    if(!is_authenticated(request)) return;
    z_log("[NVS] Manual cache reset requested\n");
    for (auto& dev : bacnet_network_cache) {
        char ns[16]; snprintf(ns, sizeof(ns), "dev_%lu", (unsigned long)dev.device_id);
        Preferences prefs;
        if (prefs.begin(ns, false)) { prefs.clear(); prefs.end(); }
    }
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

        if(request->hasParam("mac", true)) sysCfg.mac_address = request->getParam("mac", true)->value().toInt();
        if(request->hasParam("mm", true)) sysCfg.max_master = request->getParam("mm", true)->value().toInt();
        if(request->hasParam("did", true)) sysCfg.device_id = request->getParam("did", true)->value().toInt();
        if(request->hasParam("timeout", true)) sysCfg.apdu_timeout = request->getParam("timeout", true)->value().toInt();
        if(request->hasParam("retries", true)) sysCfg.max_retries = request->getParam("retries", true)->value().toInt();

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
