#include "z_network.h"
#include "z_ui.h"
#include "z_bacnet.h"
#include <ArduinoJson.h>
#include <Update.h>
#include <ESPmDNS.h>
#include "esp_wifi.h"

void load_configuration() {
    // Defaults Industriels
    strlcpy(sysCfg.wifi_ssid, "", 32);
    strlcpy(sysCfg.wifi_pass, "", 64);
    sysCfg.static_ip = false;
    strlcpy(sysCfg.local_ip, "192.168.1.50", 16);
    strlcpy(sysCfg.gateway, "192.168.1.254", 16);
    strlcpy(sysCfg.subnet, "255.255.255.0", 16);
    sysCfg.mac_address = 1;
    sysCfg.max_master = 3;
    sysCfg.device_id = 4;
    sysCfg.apdu_timeout = 1000;
    sysCfg.max_retries = 3;
    strlcpy(sysCfg.mqtt_server, "192.168.1.11", 32);
    sysCfg.mqtt_port = 1883;
    strlcpy(sysCfg.mqtt_user, "", 32);
    strlcpy(sysCfg.mqtt_pass, "", 32);
    strlcpy(sysCfg.mqtt_prefix, "bacnet", 64);
    sysCfg.log_level = 3;
    strlcpy(sysCfg.admin_user, "admin", 32);
    strlcpy(sysCfg.admin_pass, "admin1234", 64);

    preferences.begin("sys", true);
    if (preferences.isKey("ssid")) preferences.getString("ssid", sysCfg.wifi_ssid, 32);
    if (preferences.isKey("pass")) preferences.getString("pass", sysCfg.wifi_pass, 64);
    if (preferences.isKey("static")) sysCfg.static_ip = preferences.getBool("static", false);
    if (preferences.isKey("ip")) preferences.getString("ip", sysCfg.local_ip, 16);
    if (preferences.isKey("gw")) preferences.getString("gw", sysCfg.gateway, 16);
    if (preferences.isKey("sn")) preferences.getString("sn", sysCfg.subnet, 16);
    if (preferences.isKey("mac")) sysCfg.mac_address = preferences.getUChar("mac", 1);
    if (preferences.isKey("mm")) sysCfg.max_master = preferences.getUChar("mm", 127);
    if (preferences.isKey("did")) sysCfg.device_id = preferences.getUInt("did", 1234);
    if (preferences.isKey("to")) sysCfg.apdu_timeout = preferences.getUShort("to", 1000);
    if (preferences.isKey("ret")) sysCfg.max_retries = preferences.getUChar("ret", 3);
    if (preferences.isKey("mqh")) preferences.getString("mqh", sysCfg.mqtt_server, 32);
    if (preferences.isKey("mqp")) sysCfg.mqtt_port = preferences.getUShort("mqp", 1883);
    if (preferences.isKey("mqu")) preferences.getString("mqu", sysCfg.mqtt_user, 32);
    if (preferences.isKey("mqpa")) preferences.getString("mqpa", sysCfg.mqtt_pass, 32);
    if (preferences.isKey("mqpr")) preferences.getString("mqpr", sysCfg.mqtt_prefix, 64);
    if (preferences.isKey("au")) preferences.getString("au", sysCfg.admin_user, 32);
    if (preferences.isKey("ap")) preferences.getString("ap", sysCfg.admin_pass, 64);
    preferences.end();
}

void save_configuration() {
    preferences.begin("sys", false);
    preferences.putString("ssid", sysCfg.wifi_ssid);
    preferences.putString("pass", sysCfg.wifi_pass);
    preferences.putBool("static", sysCfg.static_ip);
    preferences.putString("ip", sysCfg.local_ip);
    preferences.putString("gw", sysCfg.gateway);
    preferences.putString("sn", sysCfg.subnet);
    preferences.putUChar("mac", sysCfg.mac_address);
    preferences.putUChar("mm", sysCfg.max_master);
    preferences.putUInt("did", sysCfg.device_id);
    preferences.putUShort("to", sysCfg.apdu_timeout);
    preferences.putUChar("ret", sysCfg.max_retries);
    preferences.putString("mqh", sysCfg.mqtt_server);
    preferences.putUShort("mqp", sysCfg.mqtt_port);
    preferences.putString("mqu", sysCfg.mqtt_user);
    preferences.putString("mqpa", sysCfg.mqtt_pass);
    preferences.putString("mqpr", sysCfg.mqtt_prefix);
    preferences.putString("au", sysCfg.admin_user);
    preferences.putString("ap", sysCfg.admin_pass);
    preferences.end();
}

void setup_network_infrastructure() {
    WiFi.persistent(false);
    load_configuration();
    delay(500);

    if (strlen(sysCfg.wifi_ssid) == 0) {
        is_ap_mode = true;
        WiFi.mode(WIFI_AP);
        WiFi.softAP("ZIRCON-GW-CONFIG", "admin1234");
        Serial.println("[WIFI] Mode AP: 192.168.4.1 (No Config)");
    } else {
        is_ap_mode = false;
        WiFi.mode(WIFI_STA);
        esp_wifi_set_ps(WIFI_PS_NONE);

        if (sysCfg.static_ip) {
            IPAddress ip, gw, sn;
            if (ip.fromString(sysCfg.local_ip) && gw.fromString(sysCfg.gateway) && sn.fromString(sysCfg.subnet)) {
                WiFi.config(ip, gw, sn);
                Serial.printf("[WIFI] STATIC IP: %s\n", sysCfg.local_ip);
            }
        }
        
        Serial.printf("[WIFI] Connecting to %s ", sysCfg.wifi_ssid);
        WiFi.begin(sysCfg.wifi_ssid, sysCfg.wifi_pass);
        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
            delay(500); Serial.print(".");
        }
        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("\n[WIFI] Connected! IP: " + WiFi.localIP().toString());
            MDNS.begin("bacnet-gateway");
            ArduinoOTA.begin();
        } else {
            Serial.println("\n[WIFI] Failed. AP Mode.");
            WiFi.softAP("ZIRCON-GW-CONFIG", "admin1234");
            is_ap_mode = true;
        }
    }

    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        request->send_P(200, "text/html", INDEX_HTML);
    });

    webServer.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        JsonDocument doc;
        doc["ver"] = VERSION_GLOBAL;
        doc["rssi"] = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
        doc["ip"] = is_ap_mode ? "192.168.4.1" : WiFi.localIP().toString();
        doc["mqtt"] = mqttClient.connected();
        doc["heap"] = ESP.getFreeHeap() / 1024;
        doc["mac_id"] = sysCfg.mac_address;
        doc["mstp_t"] = bacnetStats.tokens_seen;
        doc["mstp_p"] = bacnetStats.pfm_replies;
        String res; serializeJson(doc, res);
        request->send(200, "application/json", res);
    });

    webServer.on("/api/config", HTTP_GET, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        JsonDocument doc;
        doc["ssid"] = String(sysCfg.wifi_ssid);
        doc["static_ip"] = sysCfg.static_ip;
        doc["local_ip"] = String(sysCfg.local_ip);
        doc["gateway"] = String(sysCfg.gateway);
        doc["subnet"] = String(sysCfg.subnet);
        doc["mac"] = sysCfg.mac_address;
        doc["mm"] = sysCfg.max_master;
        doc["did"] = sysCfg.device_id;
        doc["to"] = sysCfg.apdu_timeout;
        doc["ret"] = sysCfg.max_retries;
        doc["mqh"] = String(sysCfg.mqtt_server);
        doc["mqp"] = sysCfg.mqtt_port;
        doc["mqu"] = String(sysCfg.mqtt_user);
        doc["mqpr"] = String(sysCfg.mqtt_prefix);
        doc["au"] = String(sysCfg.admin_user);
        String res; serializeJson(doc, res);
        request->send(200, "application/json", res);
    });

    webServer.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        
        // Sauvegarde Différentielle : on ne touche qu'aux champs présents
        if(request->hasParam("ssid", true)) strlcpy(sysCfg.wifi_ssid, request->getParam("ssid", true)->value().c_str(), 32);
        if(request->hasParam("pass", true)) strlcpy(sysCfg.wifi_pass, request->getParam("pass", true)->value().c_str(), 64);
        
        // Flag Static : Présent uniquement dans le form WiFi
        if(request->hasParam("form_type", true) && request->getParam("form_type", true)->value() == "wifi") {
            sysCfg.static_ip = request->hasParam("static_ip", true);
        }
        
        if(request->hasParam("local_ip", true)) strlcpy(sysCfg.local_ip, request->getParam("local_ip", true)->value().c_str(), 16);
        if(request->hasParam("gateway", true)) strlcpy(sysCfg.gateway, request->getParam("gateway", true)->value().c_str(), 16);
        if(request->hasParam("subnet", true)) strlcpy(sysCfg.subnet, request->getParam("subnet", true)->value().c_str(), 16);
        if(request->hasParam("mac", true)) sysCfg.mac_address = request->getParam("mac", true)->value().toInt();
        if(request->hasParam("mm", true)) sysCfg.max_master = request->getParam("mm", true)->value().toInt();
        if(request->hasParam("did", true)) sysCfg.device_id = request->getParam("did", true)->value().toInt();
        if(request->hasParam("to", true)) sysCfg.apdu_timeout = request->getParam("to", true)->value().toInt();
        if(request->hasParam("ret", true)) sysCfg.max_retries = request->getParam("ret", true)->value().toInt();
        if(request->hasParam("mqh", true)) strlcpy(sysCfg.mqtt_server, request->getParam("mqh", true)->value().c_str(), 32);
        if(request->hasParam("mqp", true)) sysCfg.mqtt_port = request->getParam("mqp", true)->value().toInt();
        if(request->hasParam("mqu", true)) strlcpy(sysCfg.mqtt_user, request->getParam("mqu", true)->value().c_str(), 32);
        if(request->hasParam("mqpa", true)) strlcpy(sysCfg.mqtt_pass, request->getParam("mqpa", true)->value().c_str(), 32);
        if(request->hasParam("mqpr", true)) strlcpy(sysCfg.mqtt_prefix, request->getParam("mqpr", true)->value().c_str(), 64);
        
        save_configuration();
        request->send(200, "text/plain", "Configuration sauvegardee. Redemarrage...");
        pending_reboot = true; reboot_timer = millis();
    });

    webServer.on("/update", HTTP_POST, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
        response->addHeader("Connection", "close");
        request->send(response);
        if(!Update.hasError()) { pending_reboot = true; reboot_timer = millis(); }
    }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
        if (!index) Update.begin(UPDATE_SIZE_UNKNOWN);
        if (!Update.hasError()) Update.write(data, len);
        if (final) Update.end(true);
    });

    webServer.addHandler(&ws);
    webServer.begin();
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
