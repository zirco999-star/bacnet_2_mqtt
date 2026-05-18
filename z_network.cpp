#include "z_network.h"
#include <Update.h>
#include <ArduinoJson.h>

void load_configuration() {
    if (!preferences.begin("sys", false)) return;
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

bool is_authenticated(AsyncWebServerRequest *request) {
    if (!request->authenticate(sysCfg.admin_user, sysCfg.admin_pass)) {
        request->requestAuthentication("ZIRCON1UM Console");
        return false;
    }
    return true;
}

void setup_network_infrastructure() {
    load_configuration();
    
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(500);

    if (strlen(sysCfg.wifi_ssid) == 0) {
        is_ap_mode = true;
        WiFi.mode(WIFI_AP);
        WiFi.softAP("ZIRCON1UM_SETUP", "admin1234");
        log_to_web(1, "Mode AP Setup: 192.168.4.1");
    } else {
        is_ap_mode = false;
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(WIFI_PS_NONE);
        WiFi.setMinSecurity(WIFI_AUTH_WPA_PSK);
        WiFi.setAutoReconnect(true);

        if (sysCfg.static_ip) {
            IPAddress ip, gw, sn;
            if (ip.fromString(sysCfg.local_ip) && gw.fromString(sysCfg.gateway) && sn.fromString(sysCfg.subnet)) {
                WiFi.config(ip, gw, sn);
            }
        }
        
        WiFi.begin(sysCfg.wifi_ssid, sysCfg.wifi_pass);
        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) { 
            delay(500); 
            Serial.print("."); 
        }
        
        if(WiFi.status() == WL_CONNECTED) {
            log_to_web(1, "Connected! IP: %s (Signal: %d dBm)", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
        } else {
            is_ap_mode = true;
            WiFi.mode(WIFI_AP);
            WiFi.softAP("ZIRCON1UM_RECOVERY", "admin1234");
            log_to_web(0, "WiFi failed, Recovery AP active");
        }
    }

    if(strlen(sysCfg.mqtt_server) > 0) {
        mqttClient.setServer(sysCfg.mqtt_server, sysCfg.mqtt_port);
    }

    ArduinoOTA.begin();

    // ROUTES API (ArduinoJson v7 syntax)
    webServer.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        JsonDocument doc;
        doc["rssi"] = WiFi.RSSI();
        doc["ip"] = WiFi.localIP().toString();
        doc["mqtt"] = mqttClient.connected();
        doc["heap"] = ESP.getFreeHeap() / 1024;
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });

    webServer.on("/api/config", HTTP_GET, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        JsonDocument doc;
        doc["ssid"] = sysCfg.wifi_ssid;
        doc["static_ip"] = sysCfg.static_ip;
        doc["local_ip"] = sysCfg.local_ip;
        doc["gateway"] = sysCfg.gateway;
        doc["subnet"] = sysCfg.subnet;
        doc["mac"] = sysCfg.mac_address;
        doc["target"] = sysCfg.target_mac;
        doc["max_m"] = sysCfg.max_master;
        doc["mq_host"] = sysCfg.mqtt_server;
        doc["mq_port"] = sysCfg.mqtt_port;
        doc["mq_pref"] = sysCfg.mqtt_prefix;
        doc["log_lvl"] = sysCfg.log_level;
        doc["bridge"] = tcp_bridge_active;
        doc["adm_user"] = sysCfg.admin_user;
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });

    webServer.on("/save", HTTP_POST, [](AsyncWebServerRequest *request){
        if(!is_authenticated(request)) return;
        
        if(request->hasParam("ssid", true)) strncpy(sysCfg.wifi_ssid, request->getParam("ssid", true)->value().c_str(), 31);
        if(request->hasParam("pass", true)) strncpy(sysCfg.wifi_pass, request->getParam("pass", true)->value().c_str(), 63);
        
        sysCfg.static_ip = request->hasParam("static_ip", true);
        if(request->hasParam("local_ip", true)) strncpy(sysCfg.local_ip, request->getParam("local_ip", true)->value().c_str(), 15);
        if(request->hasParam("gateway", true))  strncpy(sysCfg.gateway, request->getParam("gateway", true)->value().c_str(), 15);
        if(request->hasParam("subnet", true))   strncpy(sysCfg.subnet, request->getParam("subnet", true)->value().c_str(), 15);

        if(request->hasParam("mac", true)) sysCfg.mac_address = request->getParam("mac", true)->value().toInt();
        if(request->hasParam("target", true)) sysCfg.target_mac = request->getParam("target", true)->value().toInt();
        if(request->hasParam("max_m", true)) sysCfg.max_master = request->getParam("max_m", true)->value().toInt();

        if(request->hasParam("mq_host", true)) strncpy(sysCfg.mqtt_server, request->getParam("mq_host", true)->value().c_str(), 31);
        if(request->hasParam("mq_port", true)) sysCfg.mqtt_port = request->getParam("mq_port", true)->value().toInt();
        if(request->hasParam("mq_pref", true)) strncpy(sysCfg.mqtt_prefix, request->getParam("mq_pref", true)->value().c_str(), 63);
        
        if(request->hasParam("log_lvl", true)) sysCfg.log_level = request->getParam("log_lvl", true)->value().toInt();
        if(request->hasParam("adm_user", true)) strncpy(sysCfg.admin_user, request->getParam("adm_user", true)->value().c_str(), 31);
        if(request->hasParam("adm_pass", true)) strncpy(sysCfg.admin_pass, request->getParam("adm_pass", true)->value().c_str(), 63);

        save_configuration();
        request->send(200, "text/plain", "Configuration Saved. Rebooting...");
        pending_reboot = true; 
        reboot_timer = millis();
    });

    webServer.on("/reboot", HTTP_GET, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        request->send(200, "text/plain", "Rebooting...");
        pending_reboot = true;
        reboot_timer = millis();
    });

    // ROUTE UPDATE OTA WEB
    webServer.on("/update", HTTP_POST, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        bool error = Update.hasError();
        request->send(200, "text/plain", error ? "FAIL" : "OK");
        if(!error) { pending_reboot = true; reboot_timer = millis(); }
    }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
        if (!index) {
            log_to_web(1, "OTA: Start %s", filename.c_str());
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
        }
        if (!Update.hasError()) {
            if (Update.write(data, len) != len) Update.printError(Serial);
        }
        if (final) {
            if (Update.end(true)) log_to_web(1, "OTA: Success. %u bytes", index + len);
            else Update.printError(Serial);
        }
    });

    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        if(is_authenticated(request)) {
            request->send_P(200, "text/html", INDEX_HTML);
        }
    });

    webServer.addHandler(&ws);
    webServer.begin();
}

void handle_network() {
    ArduinoOTA.handle();
    
    if (pending_reboot && (millis() - reboot_timer > 2000)) {
        ESP.restart();
    }

    if (!is_ap_mode && !mqttClient.connected()) {
        static uint32_t last_mq = 0;
        if (millis() - last_mq > 10000) {
            last_mq = millis();
            mqttClient.connect("Zirconium_Node", sysCfg.mqtt_user, sysCfg.mqtt_pass);
        }
    } else if (mqttClient.connected()) {
        mqttClient.loop();
    }

    static uint32_t ls = 0;
    if(millis() - ls > 10000) {
        ls = millis();
        if (WiFi.status() == WL_CONNECTED) {
            log_to_web(2, "IP: %s | Signal %d dBm | Heap: %d KB", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI(), (int)ESP.getFreeHeap() / 1024);
        }
    }
}
