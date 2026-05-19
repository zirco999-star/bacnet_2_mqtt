/******************************************************************************
*  PROJET BACnetMSTP2MQTT - by Z1rc0n1um
*  Version 3.4 - SAFE START & DIAGNOSTIC
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
    Serial.begin(115200);
    delay(2000); // Laisser le temps à l'USB JTAG de se stabiliser
    
    Serial.println("\n\n#########################################");
    Serial.println("# BACnetMSTP2MQTT v3.4 Starting...      #");
    Serial.println("#########################################");
    
    // Initialisation sécurisée du WiFi (après Serial)
    WiFi.persistent(false);
    WiFi.mode(WIFI_OFF);
    WiFi.disconnect(true, true);
    
    Serial.println("[BOOT] Initialisation Log system...");
    init_log_system();
    
    Serial.println("[BOOT] Initialisation Network...");
    setup_network_infrastructure();
    
    Serial.println("[BOOT] Initialisation MS/TP...");
    setup_mstp();
    
    log_to_web(1, "BACnetMSTP2MQTT v3.4 pret.");
    Serial.println("[BOOT] System Ready.");
}

void loop() {
    handle_network();
    ws.cleanupClients();
    vTaskDelay(pdMS_TO_TICKS(10));
}
