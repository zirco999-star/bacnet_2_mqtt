#include "z_network.h"
#include "z_ui.h"
#include "z_bacnet.h"
#include <ArduinoJson.h>
#include <Update.h>
#include <ESPmDNS.h>
#include "esp_wifi.h"
#include <stdarg.h>

extern AsyncWebSocket ws;

void z_log(const char* format, ...) {
    char loc_buf[256];
    va_list arg;
    va_start(arg, format);
    vsnprintf(loc_buf, sizeof(loc_buf), format, arg);
    va_end(arg);
    Serial.print(loc_buf);
    // Protection anti-crash WebSocket (uniquement hors mode AP et si client présent)
    if (!is_ap_mode && ws.count() > 0) {
        ws.textAll(loc_buf);
    }
}

void load_configuration() {
    // 1. Force Defaults (Success v4.2.28 state)
    strlcpy(sysCfg.wifi_ssid, "", 32);
    strlcpy(sysCfg.wifi_pass, "", 64);
    sysCfg.static_ip = true; 
    strlcpy(sysCfg.local_ip, "192.168.1.50", 16);
    strlcpy(sysCfg.gateway, "192.168.1.254", 16);
    strlcpy(sysCfg.subnet, "255.255.255.0", 16);
    sysCfg.mac_address = 1;
    sysCfg.max_master = 127;
    sysCfg.device_id = 1234;
    sysCfg.apdu_timeout = 1000;
    sysCfg.max_retries = 3;
    strlcpy(sysCfg.mqtt_server, "192.168.1.11", 32);
    sysCfg.mqtt_port = 1883;
    strlcpy(sysCfg.mqtt_user, "", 32);
    strlcpy(sysCfg.mqtt_pass, "", 32);
    strlcpy(sysCfg.mqtt_prefix, "bacnet", 64);
    sysCfg.log_level = 1;
    strlcpy(sysCfg.admin_user, "admin", 32);
    strlcpy(sysCfg.admin_pass, "admin1234", 64);

    // 2. Load from NVS namespace 'system' (Read-Write mode to allow creation)
    if (preferences.begin("system", false)) { 
        if (preferences.isKey("ssid")) preferences.getString("ssid", sysCfg.wifi_ssid, 32);
        if (preferences.isKey("pass")) preferences.getString("pass", sysCfg.wifi_pass, 64);
        if (preferences.isKey("static")) sysCfg.static_ip = preferences.getBool("static", true);
        if (preferences.isKey("ip")) preferences.getString("ip", sysCfg.local_ip, 16);
        if (preferences.isKey("gw")) preferences.getString("gw", sysCfg.gateway, 16);
        if (preferences.isKey("sn")) preferences.getString("sn", sysCfg.subnet, 16);
        if (preferences.isKey("mac")) sysCfg.mac_address = preferences.getUChar("mac", 1);
        if (preferences.isKey("mm")) sysCfg.max_master = preferences.getUChar("mm", 127);
        if (preferences.isKey("did")) sysCfg.device_id = preferences.getUInt("did", 1234);
        if (preferences.isKey("mqh")) preferences.getString("mqh", sysCfg.mqtt_server, 32);
        if (preferences.isKey("mqp")) sysCfg.mqtt_port = preferences.getUShort("mqp", 1883);
        preferences.end();
        z_log("[WIFI] Configuration loaded from 'system' (RW Init)\n");
    } else {
        z_log("[WIFI] ERROR: Could not open 'system' namespace in NVS!\n");
    }
}

void save_configuration() {
    if (preferences.begin("system", false)) {
        preferences.putString("ssid", sysCfg.wifi_ssid);
        preferences.putString("pass", sysCfg.wifi_pass);
        preferences.putBool("static", sysCfg.static_ip);
        preferences.putString("ip", sysCfg.local_ip);
        preferences.putString("gw", sysCfg.gateway);
        preferences.putString("sn", sysCfg.subnet);
        preferences.putUChar("mac", sysCfg.mac_address);
        preferences.putUChar("mm", sysCfg.max_master);
        preferences.putUInt("did", sysCfg.device_id);
        preferences.putString("mqh", sysCfg.mqtt_server);
        preferences.putUShort("mqp", sysCfg.mqtt_port);
        preferences.end();
        z_log("[WIFI] Configuration saved to 'system'\n");
    }
}

void setup_network_infrastructure() {
    load_configuration();
    WiFi.persistent(false);
    WiFi.disconnect(true);
    vTaskDelay(pdMS_TO_TICKS(100));

    if (strlen(sysCfg.wifi_ssid) == 0) {
        is_ap_mode = true;
        WiFi.mode(WIFI_AP);
        WiFi.softAP("ZIRCON-GW-CONFIG", "admin1234");
        z_log("[WIFI] Mode AP: ZIRCON-GW-CONFIG (192.168.4.1)\n");
    } else {
        is_ap_mode = false;
        WiFi.mode(WIFI_STA);
        if (sysCfg.static_ip) {
            IPAddress ip, gw, sn;
            if (ip.fromString(sysCfg.local_ip) && gw.fromString(sysCfg.gateway) && sn.fromString(sysCfg.subnet)) {
                WiFi.config(ip, gw, sn);
                z_log("[WIFI] Static IP: %s\n", sysCfg.local_ip);
            }
        }
        WiFi.begin(sysCfg.wifi_ssid, sysCfg.wifi_pass);
        z_log("[WIFI] Connecting to %s...\n", sysCfg.wifi_ssid);
    }

    webServer.addHandler(&ws);
    
    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        request->send_P(200, "text/html", INDEX_HTML);
    });

    webServer.on("/api/objects", HTTP_GET, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();
        if (bacnet_network_cache.size() > 0) {
            for (auto& o : bacnet_network_cache[0].objects) {
                JsonObject obj = arr.add<JsonObject>();
                obj["type"] = o.type;
                obj["inst"] = o.instance;
                obj["val"] = o.present_value;
                obj["en"] = o.enabled;
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
        doc["mqtt"] = (mqtt_client != NULL);
        doc["heap"] = ESP.getFreeHeap() / 1024;
        doc["mac_id"] = sysCfg.mac_address;
        doc["mstp_t"] = bacnetStats.tokens_seen;
        String res; serializeJson(doc, res);
        request->send(200, "application/json", res);
    });

    webServer.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        if(request->hasParam("ssid", true)) strlcpy(sysCfg.wifi_ssid, request->getParam("ssid", true)->value().c_str(), 32);
        if(request->hasParam("pass", true)) strlcpy(sysCfg.wifi_pass, request->getParam("pass", true)->value().c_str(), 64);
        if(request->hasParam("static_ip", true)) sysCfg.static_ip = true; else sysCfg.static_ip = false;
        if(request->hasParam("local_ip", true)) strlcpy(sysCfg.local_ip, request->getParam("local_ip", true)->value().c_str(), 16);
        if(request->hasParam("gateway", true)) strlcpy(sysCfg.gateway, request->getParam("gateway", true)->value().c_str(), 16);
        if(request->hasParam("subnet", true)) strlcpy(sysCfg.subnet, request->getParam("subnet", true)->value().c_str(), 16);
        if(request->hasParam("mac", true)) sysCfg.mac_address = request->getParam("mac", true)->value().toInt();
        if(request->hasParam("mm", true)) sysCfg.max_master = request->getParam("mm", true)->value().toInt();
        if(request->hasParam("did", true)) sysCfg.device_id = request->getParam("did", true)->value().toInt();
        if(request->hasParam("mqh", true)) strlcpy(sysCfg.mqtt_server, request->getParam("mqh", true)->value().c_str(), 32);
        save_configuration();
        request->send(200, "text/plain", "OK");
        pending_reboot = true; reboot_timer = millis();
    });

    webServer.on("/reboot", HTTP_GET, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        request->send(200, "text/plain", "Rebooting...");
        pending_reboot = true; reboot_timer = millis();
    });

    webServer.on("/api/reset_cache", HTTP_POST, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        preferences.begin("bac_cache", false); preferences.clear(); preferences.end();
        request->send(200, "text/plain", "OK");
        pending_reboot = true; reboot_timer = millis();
    });

    webServer.begin();
    MDNS.begin("bacnet-gateway");
    ArduinoOTA.begin();
}

void handle_network() {
    ArduinoOTA.handle();
    if (pending_reboot && (millis() - reboot_timer > 2000)) ESP.restart();
}

bool is_authenticated(AsyncWebServerRequest *request) {
    if (!request->authenticate(sysCfg.admin_user, sysCfg.admin_pass)) {
        request->requestAuthentication();
        return false;
    }
    return true;
}
