#include "z_network.h"
#include <Update.h>
#include <ArduinoJson.h>
#include "esp_wifi.h" 

// ESPHome-style Event Queue
enum WiFiTaskEvent { EVENT_NONE, EVENT_CONNECTED, EVENT_DISCONNECTED };
volatile WiFiTaskEvent pending_event = EVENT_NONE;
volatile int last_err = 0;

void WiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        pending_event = EVENT_CONNECTED;
    } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        pending_event = EVENT_DISCONNECTED;
        last_err = info.wifi_sta_disconnected.reason;
    }
}

void load_configuration() {
    memset(&sysCfg, 0, sizeof(Config));
    // Valeurs par défaut
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
    sysCfg.log_level = 2;
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
    
    // 1. INITIALISATION STYLE ESPHOME (Direct IDF)
    WiFi.onEvent(WiFiEvent);
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    delay(500);

    if (strlen(sysCfg.wifi_ssid) == 0) {
        is_ap_mode = true;
        WiFi.mode(WIFI_AP);
        WiFi.softAP("BACnetMSTP2MQTT_SETUP", "admin1234");
        log_to_web(1, "Mode AP: 192.168.4.1");
    } else {
        is_ap_mode = false;
        WiFi.mode(WIFI_STA);
        
        // --- CONFIGURATION CRITIQUE ESPHOME ---
        esp_wifi_set_storage(WIFI_STORAGE_RAM); // Ne pas toucher au NVS pour les compteurs CCMP
        esp_wifi_set_ps(WIFI_PS_NONE);          // Pas de dodo radio
        
        wifi_config_t conf;
        esp_wifi_get_config(WIFI_IF_STA, &conf);
        
        // Paramètres de sécurité "Gold Standard"
        conf.sta.pmf_cfg.capable = true;
        conf.sta.pmf_cfg.required = false;
        conf.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        
        // Scan complet avant connexion
        conf.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        conf.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        
        esp_wifi_set_config(WIFI_IF_STA, &conf);
        // --------------------------------------

        if (sysCfg.static_ip) {
            IPAddress ip, gw, sn;
            if (ip.fromString(sysCfg.local_ip) && gw.fromString(sysCfg.gateway) && sn.fromString(sysCfg.subnet)) {
                WiFi.config(ip, gw, sn);
            }
        }
        
        log_to_web(1, "WiFi: Connexion a [%s]...", sysCfg.wifi_ssid);
        WiFi.begin(sysCfg.wifi_ssid, sysCfg.wifi_pass);
        
        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 25000) { 
            delay(500);
            Serial.print(".");
        }
        
        if(WiFi.status() == WL_CONNECTED) {
            log_to_web(1, "WiFi Connecté ! Signal: %d dBm", (int)WiFi.RSSI());
        } else {
            log_to_web(0, "WiFi Echec (Reason: %d). Mode RECOVERY.", last_err);
            is_ap_mode = true;
            WiFi.mode(WIFI_AP);
            WiFi.softAP("BACnetMSTP2MQTT_RECOVERY", "admin1234");
        }
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
        doc["gateway"] = String(sysCfg.gateway);
        doc["subnet"] = String(sysCfg.subnet);
        doc["mac"] = sysCfg.mac_address;
        doc["target"] = sysCfg.target_mac;
        doc["max_m"] = sysCfg.max_master;
        doc["mq_host"] = String(sysCfg.mqtt_server);
        doc["mq_port"] = sysCfg.mqtt_port;
        doc["mq_pref"] = String(sysCfg.mqtt_prefix);
        doc["log_lvl"] = sysCfg.log_level;
        doc["adm_user"] = String(sysCfg.admin_user);
        String res; serializeJson(doc, res);
        request->send(200, "application/json", res);
    });

    webServer.on("/save", HTTP_POST, [](AsyncWebServerRequest *request){
        if(!is_authenticated(request)) return;
        if(request->hasParam("ssid", true)) strncpy(sysCfg.wifi_ssid, request->getParam("ssid", true)->value().c_str(), 31);
        if(request->hasParam("pass", true)) strncpy(sysCfg.wifi_pass, request->getParam("pass", true)->value().c_str(), 63);
        sysCfg.static_ip = request->hasParam("static_ip", true);
        if(request->hasParam("local_ip", true)) strncpy(sysCfg.local_ip, request->getParam("local_ip", true)->value().c_str(), 15);
        if(request->hasParam("gateway", true)) strncpy(sysCfg.gateway, request->getParam("gateway", true)->value().c_str(), 15);
        if(request->hasParam("subnet", true)) strncpy(sysCfg.subnet, request->getParam("subnet", true)->value().c_str(), 15);
        if(request->hasParam("mac", true)) sysCfg.mac_address = request->getParam("mac", true)->value().toInt();
        if(request->hasParam("target", true)) sysCfg.target_mac = request->getParam("target", true)->value().toInt();
        if(request->hasParam("mq_host", true)) strncpy(sysCfg.mqtt_server, request->getParam("mq_host", true)->value().c_str(), 31);
        if(request->hasParam("mq_port", true)) sysCfg.mqtt_port = request->getParam("mq_port", true)->value().toInt();
        if(request->hasParam("log_lvl", true)) sysCfg.log_level = request->getParam("log_lvl", true)->value().toInt();
        save_configuration();
        request->send(200, "text/plain", "OK. Reboot...");
        pending_reboot = true; reboot_timer = millis();
    });

    webServer.on("/update", HTTP_POST, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        request->send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
        if(!Update.hasError()) { pending_reboot = true; reboot_timer = millis(); }
    }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
        if (!index) {
            log_to_web(1, "OTA: Start %s", filename.c_str());
            Update.begin(UPDATE_SIZE_UNKNOWN);
        }
        if (!Update.hasError()) Update.write(data, len);
        if (final) {
            if (Update.end(true)) log_to_web(1, "OTA: Success.");
        }
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

    // Déport de la gestion d'événements WiFi (ESPHome Style)
    if (pending_event == EVENT_CONNECTED) {
        log_to_web(1, "Event: Connecté à l'IP %s", WiFi.localIP().toString().c_str());
        pending_event = EVENT_NONE;
    } else if (pending_event == EVENT_DISCONNECTED) {
        log_to_web(0, "Event: Déconnecté (Raison: %d)", last_err);
        pending_event = EVENT_NONE;
    }

    if (!is_ap_mode && WiFi.status() == WL_CONNECTED && !mqttClient.connected()) {
        static uint32_t last_mq = 0;
        if (millis() - last_mq > 15000) {
            last_mq = millis();
            mqttClient.connect("BACnetNode", sysCfg.mqtt_user, sysCfg.mqtt_pass);
        }
    } else if (mqttClient.connected()) mqttClient.loop();
    
    static uint32_t ls = 0;
    if(millis() - ls > 10000) {
        ls = millis();
        if (WiFi.status() == WL_CONNECTED) log_to_web(2, "Signal %d dBm | Heap: %d KB", (int)WiFi.RSSI(), (int)ESP.getFreeHeap() / 1024);
    }
}
