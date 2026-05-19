#include "z_network.h"
#include <Update.h>
#include <ArduinoJson.h>
#include "esp_wifi.h" 
#include "esp_netif.h"
#include "esp_event.h"

// Variables de statut bas niveau
static bool idf_wifi_started = false;
static bool idf_got_ip = false;
static int idf_reason = 0;

// Handler d'événements natif ESP-IDF
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* disconnected = (wifi_event_sta_disconnected_t*) event_data;
        idf_reason = disconnected->reason;
        idf_got_ip = false;
        esp_wifi_connect(); // Auto-reconnect natif
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        idf_got_ip = true;
    }
}

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
    
    // 1. INITIALISATION PURE ESP-IDF (Methodologie ESPHome)
    // On arrête tout ce qui pourrait être en cours (NVS, cache)
    esp_wifi_stop();
    esp_wifi_deinit();
    delay(200);

    if (strlen(sysCfg.wifi_ssid) == 0) {
        is_ap_mode = true;
        WiFi.mode(WIFI_AP);
        WiFi.softAP("BACnetMSTP2MQTT_SETUP", "admin1234");
        log_to_web(1, "Mode AP: 192.168.4.1");
    } else {
        is_ap_mode = false;
        
        // Initialisation Netif et Event Loop (Arduino le fait déjà, mais on s'assure de l'interface STA)
        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta_netif == NULL) sta_netif = esp_netif_create_default_wifi_sta();

        // Config IP Statique via Netif
        if (sysCfg.static_ip) {
            esp_netif_dhcpc_stop(sta_netif);
            esp_netif_ip_info_t ip_info;
            IPAddress ip, gw, sn;
            ip.fromString(sysCfg.local_ip);
            gw.fromString(sysCfg.gateway);
            sn.fromString(sysCfg.subnet);
            ip_info.ip.addr = ip;
            ip_info.gw.addr = gw;
            ip_info.netmask.addr = sn;
            esp_netif_set_ip_info(sta_netif, &ip_info);
            log_to_web(1, "IDF: IP Statique %s appliquée.", sysCfg.local_ip);
        }

        // Init WiFi avec config par défaut
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_wifi_init(&cfg);
        
        // --- FORCE RAM STORAGE (Élimine CCMP Replay) ---
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        
        // Handlers natifs
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

        wifi_config_t wifi_config = {};
        strncpy((char*)wifi_config.sta.ssid, sysCfg.wifi_ssid, 32);
        strncpy((char*)wifi_config.sta.password, sysCfg.wifi_pass, 64);
        
        // Paramètres PMF ESPHome "Gold Standard"
        wifi_config.sta.pmf_cfg.capable = true;
        wifi_config.sta.pmf_cfg.required = false;
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        
        // Désactivation Power Save (Antenne full power)
        esp_wifi_set_ps(WIFI_PS_NONE);
        
        log_to_web(1, "IDF: Start WiFi Driver pour [%s]...", sysCfg.wifi_ssid);
        esp_wifi_start();
        idf_wifi_started = true;
        
        // Attente de l'IP (30s)
        uint32_t start = millis();
        while (!idf_got_ip && millis() - start < 30000) {
            delay(500);
            Serial.print(".");
        }

        if (idf_got_ip) {
            log_to_web(1, "IDF: WiFi Link UP ! IP: %s", WiFi.localIP().toString().c_str());
        } else {
            log_to_web(0, "IDF: Echec (Reason: %d). Mode RECOVERY.", idf_reason);
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

    webServer.on("/reboot", HTTP_GET, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        request->send(200, "text/plain", "Reboot...");
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
