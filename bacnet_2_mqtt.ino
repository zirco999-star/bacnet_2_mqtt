#include "z_config.h"
#include "z_network.h"
#include "z_bacnet.h"
#include "z_mqtt.h"

Config sysCfg;
Preferences preferences;
AsyncWebServer webServer(WEB_PORT);
AsyncWebSocket ws("/ws-logs");
WiFiClient mqttWifiClient;
PubSubClient mqttClient(mqttWifiClient);
bool is_ap_mode = false;
bool pending_reboot = false;
uint32_t reboot_timer = 0;

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n\n>>> " + String(VERSION_GLOBAL) + " - MODULAR START <<<");
    
    setup_network_infrastructure(); // WiFi + OTA + NVS
    setup_bacnet_engine();          // RS485 + MS/TP
    setup_mqtt();                   // Broker connection
    
    Serial.println("[" + String(VERSION_GLOBAL) + "] System Operational.");
}

void loop() {
    handle_network();
    handle_mqtt();
    vTaskDelay(pdMS_TO_TICKS(1));
}
