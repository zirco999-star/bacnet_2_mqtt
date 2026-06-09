#include "src/z_config.h"
#include "src/z_network.h"
#include "src/z_bacnet.h"
#include "src/z_mqtt.h"

extern "C" {
#include "nvs_flash.h"
}

// Global system variables
Config sysCfg;
AsyncWebServer webServer(WEB_PORT);
AsyncWebSocket ws("/ws-logs");
esp_mqtt_client_handle_t mqtt_client = NULL;
bool is_ap_mode = false;
bool pending_reboot = false;
uint32_t reboot_timer = 0;

// System Task for Core 0 (handles WiFi, MQTT, and OTA updates)
void system_task(void *pvParameters) {
    z_log(LOG_INFO, "SYS", "System Task started on Core %d\n", xPortGetCoreID());
    for(;;) {
        handle_network();
        handle_mqtt();
        vTaskDelay(pdMS_TO_TICKS(10)); // Small delay to give time to the WiFi stack
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n\n>>> " + String(configVERSION_GLOBAL) + " - DUAL CORE MODE <<<");

    // --- NVS (Non-Volatile Storage) Self-Healing Routine ---
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND || err == ESP_ERR_NVS_NOT_FOUND) {
        Serial.println("[NVS] Critical corruption or missing partition. Formatting...");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    
    // Infrastructure Initialization
    setup_network_infrastructure(); // Prepares WiFi + WebServer
    setup_bacnet_engine();          // Starts BACnet on Core 1
    
    // Create the System task on Core 0
    xTaskCreatePinnedToCore(system_task, "SystemTask", 8192, NULL, 5, NULL, 0);

    Serial.println("[" + String(configVERSION_GLOBAL) + "] System Operational.");
}

void loop() {
    // The default Arduino loop runs on Core 1.
    // We leave it empty and delete it so it does not interfere with the BACnet task (Core 1).
    vTaskDelete(NULL); // Delete the loop task to free up resources
}
