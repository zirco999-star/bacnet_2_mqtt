/******************************************************************************
*  PROJET BACnetMSTP2MQTT - by Z1rc0n1um
*  Version 3.1 - ATOMIC WIPE & UI REFINEMENT (Eye Toggle)
*****************************************************************************/
#include "z_config.h"
#include "z_logger.h"
#include "z_network.h"
#include "z_mstp.h"

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
    // KILL AUTO-CONNECT IMMEDIAT (Antidote CCMP Replay)
    WiFi.persistent(false);
    WiFi.disconnect(true, true);
    
    Serial.begin(115200);
    delay(1000); 
    
    Serial.println("\n\n#########################################");
    Serial.println("# BACnetMSTP2MQTT v3.2 Starting...      #");
    Serial.println("#########################################");
    
    init_log_system();
    setup_network_infrastructure();
    setup_mstp();
    
    log_to_web(1, "BACnetMSTP2MQTT v3.2 pret.");
}

void loop() {
    handle_network();
    ws.cleanupClients();
    vTaskDelay(pdMS_TO_TICKS(10));
}
