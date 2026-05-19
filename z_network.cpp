#include "z_network.h"
#include "z_ui.h"
#include "z_mstp.h"
#include <ArduinoJson.h>
#include <Update.h>
#include <ESPmDNS.h>
#include "esp_wifi.h"

static uint32_t connection_start_ms = 0;
static bool connection_active = false;

void load_configuration() {
    memset(&sysCfg, 0, sizeof(Config));
    // Defaults forcés si mémoire vide
    strncpy(sysCfg.wifi_ssid, "", 31);
    strncpy(sysCfg.wifi_pass, "", 63);
    sysCfg.static_ip = false;
    strncpy(sysCfg.local_ip, "192.168.1.50", 15);
    strncpy(sysCfg.gateway, "192.168.1.254", 15);
    strncpy(sysCfg.subnet, "255.255.255.0", 15);
    sysCfg.mac_address = 1;
    sysCfg.target_mac = 4;
    sysCfg.max_master = 127;
    sysCfg.log_level = 3; 
    strncpy(sysCfg.admin_user, "admin", 31);
    strncpy(sysCfg.admin_pass, "admin1234", 63);

    preferences.begin("sys", true);
    if(preferences.getBytesLength("cfg") == sizeof(Config)) {
        preferences.getBytes("cfg", &sysCfg, sizeof(Config));
    }
    preferences.end();
}

void save_configuration() {
    preferences.begin("sys", false);
    preferences.putBytes("cfg", &sysCfg, sizeof(Config));
    preferences.end();
}

void setup_network_infrastructure() {
    WiFi.persistent(false);
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    
    load_configuration();
    
    delay(500);

    if (strlen(sysCfg.wifi_ssid) == 0) {
        is_ap_mode = true;
        WiFi.mode(WIFI_AP);
        WiFi.softAP("ZIRCON-GW-CONFIG", "admin1234");
        Serial.println("[WIFI] Mode AP Setup (192.168.4.1)");
    } else {
        is_ap_mode = false;
        WiFi.mode(WIFI_STA);
        
        esp_wifi_set_ps(WIFI_PS_NONE); // Performance max

        if (sysCfg.static_ip) {
            IPAddress ip, gw, sn;
            if (ip.fromString(sysCfg.local_ip) && gw.fromString(sysCfg.gateway) && sn.fromString(sysCfg.subnet)) {
                WiFi.config(ip, gw, sn);
                Serial.printf("[WIFI] Configuration Statique: %s\n", sysCfg.local_ip);
            }
        }
        
        Serial.printf("[WIFI] Connexion a %s...\n", sysCfg.wifi_ssid);
        WiFi.begin(sysCfg.wifi_ssid, sysCfg.wifi_pass);
        connection_start_ms = millis();
        connection_active = true;
    }

    ArduinoOTA.setHostname("bacnet-gateway");
    ArduinoOTA.begin();

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
        doc["mstp_f"] = mstpStats.frames_received;
        doc["mstp_t"] = mstpStats.tokens_seen;
        doc["mstp_rb"] = mstpStats.raw_bytes;
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
        doc["log_lvl"] = sysCfg.log_level;
        doc["adm_user"] = String(sysCfg.admin_user);
        String res; serializeJson(doc, res);
        request->send(200, "application/json", res);
    });

    webServer.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        if(request->hasParam("ssid", true)) strncpy(sysCfg.wifi_ssid, request->getParam("ssid", true)->value().c_str(), 31);
        if(request->hasParam("pass", true)) strncpy(sysCfg.wifi_pass, request->getParam("pass", true)->value().c_str(), 63);
        sysCfg.static_ip = request->hasParam("static_ip", true);
        if(request->hasParam("local_ip", true)) strncpy(sysCfg.local_ip, request->getParam("local_ip", true)->value().c_str(), 15);
        if(request->hasParam("gateway", true)) strncpy(sysCfg.gateway, request->getParam("gateway", true)->value().c_str(), 15);
        if(request->hasParam("subnet", true)) strncpy(sysCfg.subnet, request->getParam("subnet", true)->value().c_str(), 15);
        if(request->hasParam("mac", true)) sysCfg.mac_address = request->getParam("mac", true)->value().toInt();
        save_configuration();
        request->send(200, "text/plain", "OK. Reboot...");
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

    if (connection_active) {
        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("\n[WIFI] Connecte ! IP: " + WiFi.localIP().toString());
            connection_active = false;
        } else if (millis() - connection_start_ms > 15000) {
            Serial.println("\n[WIFI] Timeout. Mode AP.");
            WiFi.softAP("ZIRCON-GW-CONFIG", "admin1234");
            is_ap_mode = true;
            connection_active = false;
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
