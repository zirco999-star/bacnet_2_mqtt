#include "z_network.h"
#include <Update.h>
#include <ArduinoJson.h>
#include "esp_wifi.h" 

// Variables de gestion asynchrone
static uint32_t connection_start_ms = 0;
static bool connection_active = false;
static int last_wifi_status = -1;

void load_configuration() {
    memset(&sysCfg, 0, sizeof(Config));
    strncpy(sysCfg.wifi_ssid, "", 31);
    strncpy(sysCfg.wifi_pass, "", 63);
    sysCfg.static_ip = false;
    strncpy(sysCfg.local_ip, "192.168.1.50", 15);
    strncpy(sysCfg.gateway, "192.168.1.1", 15);
    strncpy(sysCfg.subnet, "255.255.255.0", 15);
    strncpy(sysCfg.mqtt_server, "", 31);
    sysCfg.mqtt_port = 1883;
    strncpy(sysCfg.mqtt_prefix, "bacnet", 63);
    sysCfg.mac_address = 1;
    sysCfg.target_mac = 4;
    sysCfg.max_master = 127;
    sysCfg.polling_interval = 5000;
    sysCfg.log_level = 3; // DEBUG VERBOSE
    strncpy(sysCfg.admin_user, "admin", 31);
    strncpy(sysCfg.admin_pass, "admin1234", 63);

    if (preferences.begin("sys", true)) {
        if(preferences.getBytesLength("cfg") == sizeof(Config)) {
            preferences.getBytes("cfg", &sysCfg, sizeof(Config));
        }
        preferences.end();
    }
}

void save_configuration() {
    if (preferences.begin("sys", false)) {
        preferences.putBytes("cfg", &sysCfg, sizeof(Config));
        preferences.end();
    }
}

bool is_authenticated(AsyncWebServerRequest *request) {
    if (!request->authenticate(sysCfg.admin_user, sysCfg.admin_pass)) {
        request->requestAuthentication("BACnetMSTP2MQTT Console");
        return false;
    }
    return true;
}

void setup_network_infrastructure() {
    load_configuration();
    
    // ACTIVE LE DEBUG IDF POUR LE WIFI
    esp_log_level_set("wifi", ESP_LOG_DEBUG);
    esp_log_level_set("esp_netif", ESP_LOG_DEBUG);

    WiFi.persistent(false);
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    delay(500);

    if (strlen(sysCfg.wifi_ssid) == 0) {
        is_ap_mode = true;
        WiFi.mode(WIFI_AP);
        WiFi.softAP("BACnetMSTP2MQTT_SETUP", "admin1234");
        log_to_web(1, "Mode AP Setup (192.168.4.1)");
    } else {
        is_ap_mode = false;
        WiFi.mode(WIFI_STA);
        
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        esp_wifi_set_ps(WIFI_PS_NONE);
        
        wifi_config_t conf;
        esp_wifi_get_config(WIFI_IF_STA, &conf);
        conf.sta.pmf_cfg.capable = false; 
        conf.sta.pmf_cfg.required = false;
        esp_wifi_set_config(WIFI_IF_STA, &conf);

        if (sysCfg.static_ip) {
            IPAddress ip, gw, sn;
            if (ip.fromString(sysCfg.local_ip) && gw.fromString(sysCfg.gateway) && sn.fromString(sysCfg.subnet)) {
                WiFi.config(ip, gw, sn);
            }
        }
        
        log_to_web(3, "WiFi DEBUG: SSID=%s, Pass=%s", sysCfg.wifi_ssid, sysCfg.wifi_pass);
        log_to_web(1, "WiFi: Lancement connexion asynchrone...");
        WiFi.begin(sysCfg.wifi_ssid, sysCfg.wifi_pass);
        
        connection_start_ms = millis();
        connection_active = true;
    }

    ArduinoOTA.begin();

    // ROUTES API
    webServer.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        JsonDocument doc;
        doc["rssi"] = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
        doc["ip"] = is_ap_mode ? "192.168.4.1" : WiFi.localIP().toString();
        doc["mqtt"] = mqttClient.connected();
        doc["heap"] = ESP.getFreeHeap() / 1024;
        String res; serializeJson(doc, res);
        request->send(200, "application/json", res);
    });

    webServer.on("/api/config", HTTP_GET, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        JsonDocument doc;
        doc["ssid"] = String(sysCfg.wifi_ssid);
        doc["static_ip"] = sysCfg.static_ip;
        doc["local_ip"] = String(sysCfg.local_ip);
        doc["mac"] = sysCfg.mac_address;
        doc["log_lvl"] = sysCfg.log_level;
        String res; serializeJson(doc, res);
        request->send(200, "application/json", res);
    });

    webServer.on("/save", HTTP_POST, [](AsyncWebServerRequest *request){
        if(!is_authenticated(request)) return;
        if(request->hasParam("ssid", true)) strncpy(sysCfg.wifi_ssid, request->getParam("ssid", true)->value().c_str(), 31);
        if(request->hasParam("pass", true)) strncpy(sysCfg.wifi_pass, request->getParam("pass", true)->value().c_str(), 63);
        sysCfg.static_ip = request->hasParam("static_ip", true);
        if(request->hasParam("local_ip", true)) strncpy(sysCfg.local_ip, request->getParam("local_ip", true)->value().c_str(), 15);
        if(request->hasParam("log_lvl", true)) sysCfg.log_level = request->getParam("log_lvl", true)->value().toInt();
        save_configuration();
        request->send(200, "text/plain", "OK. Reboot...");
        pending_reboot = true; reboot_timer = millis();
    });

    webServer.on("/reboot", HTTP_GET, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        request->send(200, "text/plain", "Rebooting...");
        pending_reboot = true; reboot_timer = millis();
    });

    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        if(is_authenticated(request)) request->send_P(200, "text/html", INDEX_HTML);
    });

    webServer.addHandler(&ws);
    webServer.begin();
}

void handle_network() {
    ArduinoOTA.handle();
    if (pending_reboot && (millis() - reboot_timer > 2000)) ESP.restart();

    if (connection_active) {
        int status = WiFi.status();
        if (status == WL_CONNECTED) {
            log_to_web(1, "WiFi Connecté ! IP: %s (Signal: %d dBm)", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
            connection_active = false;
        } else if (millis() - connection_start_ms > 40000) {
            log_to_web(0, "WiFi Timeout (40s). Mode RECOVERY.");
            is_ap_mode = true;
            WiFi.mode(WIFI_AP);
            WiFi.softAP("BACnetMSTP2MQTT_RECOVERY", "admin1234");
            connection_active = false;
        }
        
        if (status != last_wifi_status) {
            last_wifi_status = status;
            log_to_web(3, "WiFi Status: %d", status);
        }
    }

    if (!is_ap_mode && WiFi.status() == WL_CONNECTED && !mqttClient.connected()) {
        static uint32_t last_mq = 0;
        if (millis() - last_mq > 15000) {
            last_mq = millis();
            mqttClient.connect("BACnetNode", sysCfg.mqtt_user, sysCfg.mqtt_pass);
        }
    } else if (mqttClient.connected()) mqttClient.loop();
}
