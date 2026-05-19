#include "z_network.h"
#include <Update.h>
#include <ArduinoJson.h>
#include "esp_wifi.h" 

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
    
    // 1. RESET PROPRE (Sans crash v2.6)
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    delay(500);

    if (strlen(sysCfg.wifi_ssid) == 0) {
        is_ap_mode = true;
        WiFi.mode(WIFI_AP);
        WiFi.softAP("BACnetMSTP2MQTT_SETUP", "admin1234");
        log_to_web(1, "Mode AP Setup: 192.168.4.1");
    } else {
        is_ap_mode = false;
        WiFi.mode(WIFI_STA);
        
        // --- CONFIGURATION ESPHOME NATIVE ---
        esp_wifi_set_storage(WIFI_STORAGE_RAM); // Re-sync counters every boot
        esp_wifi_set_ps(WIFI_PS_NONE);          // Pas de dodo radio
        
        wifi_config_t conf;
        esp_wifi_get_config(WIFI_IF_STA, &conf);
        conf.sta.pmf_cfg.capable = true;
        conf.sta.pmf_cfg.required = false;
        esp_wifi_set_config(WIFI_IF_STA, &conf);
        // -------------------------------------

        // 2. SCAN DES RÉSEAUX (La clé du succès)
        log_to_web(1, "WiFi: Scan en cours pour trouver [%s]...", sysCfg.wifi_ssid);
        int n = WiFi.scanNetworks();
        int target_channel = 0;
        uint8_t* target_bssid = NULL;
        
        for (int i = 0; i < n; ++i) {
            if (WiFi.SSID(i) == String(sysCfg.wifi_ssid)) {
                target_channel = WiFi.channel(i);
                target_bssid = WiFi.BSSID(i);
                log_to_web(1, "WiFi: AP trouvé ! Canal: %d, Signal: %d dBm", target_channel, WiFi.RSSI(i));
                break;
            }
        }

        // 3. CONFIGURATION IP STATIQUE (AVANT begin)
        if (sysCfg.static_ip) {
            IPAddress ip, gw, sn;
            if (ip.fromString(sysCfg.local_ip) && gw.fromString(sysCfg.gateway) && sn.fromString(sysCfg.subnet)) {
                WiFi.config(ip, gw, sn);
            }
        }
        
        // 4. CONNEXION CIBLÉE (BSSID + Canal)
        if (target_bssid) {
            log_to_web(1, "WiFi: Connexion ciblée (BSSID Lock)...");
            WiFi.begin(sysCfg.wifi_ssid, sysCfg.wifi_pass, target_channel, target_bssid);
        } else {
            log_to_web(1, "WiFi: AP non vu au scan, tentative aveugle...");
            WiFi.begin(sysCfg.wifi_ssid, sysCfg.wifi_pass);
        }
        
        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) { 
            delay(500);
            Serial.print(".");
        }
        
        if(WiFi.status() == WL_CONNECTED) {
            log_to_web(1, "WiFi Connecté ! IP: %s", WiFi.localIP().toString().c_str());
        } else {
            log_to_web(0, "WiFi Echec (Code: %d). Mode RECOVERY.", (int)WiFi.status());
            is_ap_mode = true;
            WiFi.mode(WIFI_AP);
            WiFi.softAP("BACnetMSTP2MQTT_RECOVERY", "admin1234");
        }
        WiFi.scanDelete(); // Libère la RAM du scan
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
        if (!index) { Update.begin(UPDATE_SIZE_UNKNOWN); }
        if (!Update.hasError()) Update.write(data, len);
        if (final) { Update.end(true); }
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
    if (!is_ap_mode && WiFi.status() == WL_CONNECTED && !mqttClient.connected()) {
        static uint32_t last_mq = 0;
        if (millis() - last_mq > 15000) {
            last_mq = millis();
            mqttClient.connect("BACnetNode", sysCfg.mqtt_user, sysCfg.mqtt_pass);
        }
    } else if (mqttClient.connected()) mqttClient.loop();
}
