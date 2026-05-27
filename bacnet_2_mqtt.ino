#include "src/z_config.h"
#include "src/z_network.h"
#include "src/z_bacnet.h"
#include "src/z_mqtt.h"

extern "C" {
#include "nvs_flash.h"
}

Config sysCfg;
AsyncWebServer webServer(WEB_PORT);
AsyncWebSocket ws("/ws-logs");
esp_mqtt_client_handle_t mqtt_client = NULL;
bool is_ap_mode = false;
bool pending_reboot = false;
uint32_t reboot_timer = 0;

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n\n>>> " + String(VERSION_GLOBAL) + " - MODULAR START <<<");

    // --- CORRECTION : Routine de Self-Healing NVS ---
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND || err == ESP_ERR_NVS_NOT_FOUND) {
        Serial.println("[NVS] Corruption critique ou partition absente. Formatage...");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        Serial.printf("[NVS] ERREUR FATALE : 0x%x\n", err);
    }
    // ------------------------------------------------
    
    setup_network_infrastructure(); // WiFi + OTA + NVS Load
    setup_bacnet_engine();          // RS485 + MS/TP
    //setup_mqtt();                   // Broker connection
    
    Serial.println("[" + String(VERSION_GLOBAL) + "] System Operational.");
}

void loop() {
    handle_network();
    handle_mqtt();
    vTaskDelay(pdMS_TO_TICKS(1));
}
