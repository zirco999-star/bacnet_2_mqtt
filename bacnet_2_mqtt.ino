#include "z_config.h"
#include "z_network.h"
#include "z_mstp.h"
#include "z_logger.h"

Config sysCfg;
Preferences preferences;
AsyncWebServer webServer(WEB_PORT);
AsyncWebSocket ws("/ws-logs");
WiFiClient mqttWifiClient;
PubSubClient mqttClient(mqttWifiClient);
QueueHandle_t uart_queue;
std::atomic<int> mstp_current_state(0);
volatile bool tcp_bridge_active = false;
bool is_ap_mode = false;
bool pending_reboot = false;
uint32_t reboot_timer = 0;

void setup() {
    Serial.begin(115200);
    delay(2000); 
    Serial.println("\n\n>>> " + String(VERSION_GLOBAL) + " - RECONNECTING TO INFRASTRUCTURE <<<");
    
    Serial.println("#########################################");
    Serial.println("# BACnetMSTP2MQTT " + String(VERSION_GLOBAL) + " Starting...      #");
    Serial.println("#########################################");

    setup_network_infrastructure();
    setup_mstp();

    Serial.println("[" + String(VERSION_GLOBAL) + "] pret.");
}

void loop() {
    handle_network();
    vTaskDelay(pdMS_TO_TICKS(1));
}
