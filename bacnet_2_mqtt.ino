/******************************************************************************
*  PROJET BACnetMSTP2MQTT - by Z1rc0n1um
*  Version 2.6 - FULL NATIVE ESP-IDF TCP/IP STACK
*****************************************************************************/
#include "z_config.h"
#include "z_logger.h"
#include "z_network.h"
#include "z_mstp.h"

// Définitions des variables globales
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
    
    Serial.println("\n\n#########################################");
    Serial.println("# BACnetMSTP2MQTT v2.6 Starting...      #");
    Serial.println("#########################################");
    
    init_log_system();
    setup_network_infrastructure();
    setup_mstp();
    
    log_to_web(1, "BACnetMSTP2MQTT v2.6 pret (Native IDF Stack).");
}

void loop() {
    handle_network();
    ws.cleanupClients();
    vTaskDelay(pdMS_TO_TICKS(10));
}
