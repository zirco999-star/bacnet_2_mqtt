#include "z_network.h"

void load_configuration() {
    if (!preferences.begin("sys", false)) return;
    if(preferences.getBytesLength("cfg") == sizeof(Config)) preferences.getBytes("cfg", &sysCfg, sizeof(Config));
    preferences.end();
}

void save_configuration() {
    preferences.begin("sys", false);
    preferences.putBytes("cfg", &sysCfg, sizeof(Config));
    preferences.end();
}

bool is_authenticated(AsyncWebServerRequest *request) {
    if (!request->authenticate(sysCfg.admin_user, sysCfg.admin_pass)) {
        request->requestAuthentication();
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
        Serial.println("Mode AP: 192.168.4.1");
    } else {
        is_ap_mode = false;
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(WIFI_PS_NONE);
        WiFi.setMinSecurity(WIFI_AUTH_WPA_PSK);
        if (sysCfg.static_ip) {
            IPAddress ip, gw, sn;
            if (ip.fromString(sysCfg.local_ip) && gw.fromString(sysCfg.gateway) && sn.fromString(sysCfg.subnet)) WiFi.config(ip, gw, sn);
        }
        WiFi.begin(sysCfg.wifi_ssid, sysCfg.wifi_pass);
        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) { delay(500); Serial.print("."); }
        if(WiFi.status() != WL_CONNECTED) {
            is_ap_mode = true;
            WiFi.mode(WIFI_AP);
            WiFi.softAP("ZIRCON1UM_RECOVERY", "admin1234");
        }
    }

    ArduinoOTA.begin();
    ws.onEvent([](AsyncWebSocket *s, AsyncWebSocketClient *c, AwsEventType t, void *a, uint8_t *d, size_t l) {
        if (t == WS_EVT_CONNECT) {
            for(int i=0; i<backlog_count; i++) c->text(log_backlog[(backlog_index - backlog_count + i + BACKLOG_SIZE) % BACKLOG_SIZE]);
        }
    });
    webServer.addHandler(&ws);
    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *r){ if(is_authenticated(r)) r->send_P(200, "text/html", INDEX_HTML); });
    webServer.on("/save", HTTP_POST, [](AsyncWebServerRequest *r){
        if(is_authenticated(r)) {
            if(r->hasParam("ssid", true)) strncpy(sysCfg.wifi_ssid, r->getParam("ssid", true)->value().c_str(), 31);
            if(r->hasParam("pass", true)) strncpy(sysCfg.wifi_pass, r->getParam("pass", true)->value().c_str(), 63);
            sysCfg.static_ip = r->hasParam("static_ip", true);
            if(r->hasParam("local_ip", true)) strncpy(sysCfg.local_ip, r->getParam("local_ip", true)->value().c_str(), 15);
            save_configuration();
            r->send(200, "text/plain", "OK. Rebooting...");
            pending_reboot = true; reboot_timer = millis();
        }
    });
    webServer.begin();
}

void handle_network() {
    ArduinoOTA.handle();
    if (pending_reboot && (millis() - reboot_timer > 2000)) ESP.restart();
    if (!is_ap_mode && !mqttClient.connected()) {
        static uint32_t last_mq = 0;
        if (millis() - last_mq > 10000) {
            last_mq = millis();
            mqttClient.connect("Zirconium_Node", sysCfg.mqtt_user, sysCfg.mqtt_pass);
        }
    } else if (mqttClient.connected()) mqttClient.loop();
    static uint32_t ls = 0;
    if(millis() - ls > 10000) {
        ls = millis();
        if (WiFi.status() == WL_CONNECTED) log_to_web(2, "Heap: %d KB | RSSI: %d dBm", (int)ESP.getFreeHeap() / 1024, (int)WiFi.RSSI());
    }
}
