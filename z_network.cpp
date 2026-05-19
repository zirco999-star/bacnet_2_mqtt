#include "z_network.h"
#include <Update.h>
#include <ArduinoJson.h>
#include "esp_wifi.h" 

static uint32_t connection_start_ms = 0;
static bool connection_active = false;
static int last_wifi_status = -1;

void load_configuration() {
    memset(&sysCfg, 0, sizeof(Config));
    // Defaults forcés
    strncpy(sysCfg.wifi_ssid, "", 31);
    strncpy(sysCfg.wifi_pass, "", 63);
    sysCfg.static_ip = false;
    strncpy(sysCfg.local_ip, "192.168.1.50", 15);
    strncpy(sysCfg.gateway, "192.168.1.254", 15);
    strncpy(sysCfg.subnet, "255.255.255.0", 15);
    strncpy(sysCfg.mqtt_server, "", 31);
    sysCfg.mqtt_port = 1883;
    sysCfg.mac_address = 1;
    sysCfg.target_mac = 4;
    sysCfg.max_master = 127;
    sysCfg.log_level = 3; 
    strncpy(sysCfg.admin_user, "admin", 31);
    strncpy(sysCfg.admin_pass, "admin1234", 63);

    if (preferences.begin("sys", true)) {
        if(preferences.getBytesLength("cfg") == sizeof(Config)) {
            preferences.getBytes("cfg", &sysCfg, sizeof(Config));
            
            // ATOMIC WIPE: Détection de corruption (caractères non imprimables dans le pass)
            bool corrupt = false;
            for(int i=0; i<(int)strlen(sysCfg.wifi_pass); i++) {
                if(sysCfg.wifi_pass[i] < 32 || sysCfg.wifi_pass[i] > 126) { corrupt = true; break; }
            }
            if(corrupt) {
                Serial.println("!!! CONFIG CORROMPUE DÉTECTÉE - RAZ NVS !!!");
                preferences.end();
                preferences.begin("sys", false);
                preferences.clear();
                preferences.end();
                load_configuration(); // Rechargement des defaults
                return;
            }
        }
        preferences.end();
    }
}

void save_configuration() {
    if (preferences.begin("sys", false)) {
        // Sécurisation avant sauvegarde
        sysCfg.wifi_ssid[31] = '\0';
        sysCfg.wifi_pass[63] = '\0';
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
    // 1. KILL AUTO-CONNECT IMMEDIAT (Antidote CCMP Replay)
    WiFi.persistent(false);
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    
    load_configuration();
    
    esp_log_level_set("wifi", ESP_LOG_DEBUG);
    esp_log_level_set("esp_netif", ESP_LOG_DEBUG);

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
        
        log_to_web(1, "WiFi: Lancement connexion (v3.1)...");
        log_to_web(3, "DEBUG: SSID=[%s] PASS=[%s]", sysCfg.wifi_ssid, sysCfg.wifi_pass);
        
        WiFi.begin(sysCfg.wifi_ssid, sysCfg.wifi_pass);
        connection_start_ms = millis();
        connection_active = true;
    }

    ArduinoOTA.begin();

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

    webServer.on("/update", HTTP_POST, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        request->send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
        if(!Update.hasError()) { pending_reboot = true; reboot_timer = millis(); }
    }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
        if (!index) Update.begin(UPDATE_SIZE_UNKNOWN);
        if (!Update.hasError()) Update.write(data, len);
        if (final) Update.end(true);
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
            log_to_web(1, "WiFi Connecté ! IP: %s", WiFi.localIP().toString().c_str());
            connection_active = false;
        } else if (millis() - connection_start_ms > 45000) {
            log_to_web(0, "WiFi Timeout. Mode RECOVERY.");
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
