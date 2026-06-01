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

// Tâche Système pour le Core 0 (WiFi, MQTT, OTA)
void system_task(void *pvParameters) {
    z_log(LOG_INFO, "SYS", "System Task started on Core %d\n", xPortGetCoreID());
    for(;;) {
        handle_network();
        handle_mqtt();
        vTaskDelay(pdMS_TO_TICKS(10)); // Pacing pour laisser du temps au stack WiFi
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n\n>>> " + String(VERSION_GLOBAL) + " - DUAL CORE MODE <<<");

    // Initialisation prioritaire des Mutex (pour le NVS)
    setup_system_mutexes();

    // --- Routine de Self-Healing NVS ---
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND || err == ESP_ERR_NVS_NOT_FOUND) {
        Serial.println("[NVS] Corruption critique ou partition absente. Formatage...");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    
    // Initialisation infrastructure
    setup_network_infrastructure(); // Prépare WiFi + WebServer
    setup_bacnet_engine();          // Démarre BACnet sur Core 1
    
    // Création de la tâche Système sur Core 0
    xTaskCreatePinnedToCore(system_task, "SystemTask", 8192, NULL, 5, NULL, 0);

    Serial.println("[" + String(VERSION_GLOBAL) + "] System Operational.");
}

void loop() {
    // La loop tourne sur Core 1 par défaut.
    // On la laisse vide pour ne pas interférer avec la tâche BACnet (Core 1).
    vTaskDelete(NULL); // Supprime la tâche loop pour libérer des ressources
}
