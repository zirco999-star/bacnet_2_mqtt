/**
 * @file bacnet_2_mqtt.ino
 * @brief Point d'entrée principal du firmware BACnet2MQTT pour ESP32-S3.
 *
 * Ce fichier orchestre le démarrage du système dual-core :
 *  - **Core 0** : Tâche système (WiFi, serveur Web, MQTT, OTA).
 *  - **Core 1** : Moteur BACnet MS/TP temps réel (FSM ASHRAE 135).
 *
 * Séquence de démarrage :
 *  1. Initialisation NVS avec auto-réparation en cas de corruption.
 *  2. Initialisation de l'infrastructure réseau (WiFi + WebServer).
 *  3. Démarrage du moteur BACnet sur le Core 1 (via setup_bacnet_engine).
 *  4. Création de la tâche système sur le Core 0.
 *  5. Suppression de la boucle Arduino (loop) pour libérer le Core 1.
 *
 * @note Le loop() Arduino natif tourne sur le Core 1. Il est supprimé
 *       via vTaskDelete(NULL) pour ne pas interférer avec la tâche
 *       BACnet MS/TP qui a des contraintes temps réel strictes.
 *
 * Dépendances :
 *  - z_config.h   : Configuration globale, structure Config, constantes.
 *  - z_network.h  : WiFi, serveur Web, WebSocket, OTA, fonction z_log().
 *  - z_bacnet.h   : Moteur MS/TP, FSM, structures BACnetDevice/Object.
 *  - z_mqtt.h     : Client MQTT, publication Home Assistant.
 *  - nvs_flash.h  : API ESP-IDF pour la flash NVS (partition non-volatile).
 *
 * Matériel cible : Waveshare ESP32-S3-RS485-CAN
 *  - 8 Mo Flash, 8 Mo OPI PSRAM
 *  - RS485 : TX=17, RX=18, RTS/DE=21
 *  - GPIO 47 INTERDIT (bus PSRAM OPI)
 */

#include "src/z_config.h"
#include "src/z_network.h"
#include "src/z_bacnet.h"
#include "src/z_mqtt.h"

extern "C" {
#include "nvs_flash.h"
}

// ============================================================================
// Variables globales du système
// ============================================================================

/** @brief Configuration système persistante (WiFi, MQTT, BACnet, HA). */
Config sysCfg;

/** @brief Serveur HTTP asynchrone pour l'interface Web d'administration. */
AsyncWebServer webServer(WEB_PORT);

/**
 * @brief WebSocket pour le streaming temps réel des logs vers le navigateur.
 * Point d'accès : ws://<IP>/ws-logs
 */
AsyncWebSocket ws("/ws-logs");

/** @brief Handle du client MQTT ESP-IDF. NULL tant que la connexion n'est pas établie. */
esp_mqtt_client_handle_t mqtt_client = NULL;

/** @brief Vrai si le dispositif fonctionne en mode Point d'Accès (AP) faute de WiFi configuré. */
bool is_ap_mode = false;

/** @brief Drapeau de redémarrage différé (utilisé après sauvegarde de configuration). */
bool pending_reboot = false;

/** @brief Timestamp (millis) de la demande de redémarrage, pour temporiser avant le reboot effectif. */
uint32_t reboot_timer = 0;

// ============================================================================
// Tâche système — Core 0
// ============================================================================

/**
 * @brief Tâche FreeRTOS dédiée aux services réseau (WiFi, MQTT, OTA).
 *
 * Épinglée sur le Core 0 pour isoler complètement le trafic réseau
 * de la FSM BACnet MS/TP (Core 1). La boucle principale gère :
 *  - handle_network() : Reconnexion WiFi, OTA, reboot différé, serveur Web.
 *  - handle_mqtt()    : Reconnexion MQTT, publication périodique des valeurs.
 *
 * @param pvParameters Non utilisé (convention FreeRTOS).
 *
 * @note Le délai de 10 ms (vTaskDelay) laisse le temps au stack WiFi/LwIP
 *       de traiter ses événements internes sans bloquer le scheduler.
 */
void system_task(void *pvParameters) {
    z_log(pdLOG_INFO, "SYS", "System Task started on Core %d\n", xPortGetCoreID());
    z_log(pdLOG_INFO, "WEB", "Web Server started\n");
    for(;;) {
        handle_network();
        handle_mqtt();
        vTaskDelay(pdMS_TO_TICKS(10)); // Small delay to give time to the WiFi stack
    }
}

// ============================================================================
// setup() — Séquence d'initialisation Arduino
// ============================================================================

/**
 * @brief Initialisation du système au démarrage (exécuté une seule fois sur Core 1).
 *
 * Étapes critiques :
 *  1. **NVS Self-Healing** : Si la partition NVS est corrompue (pages pleines,
 *     version incompatible, ou partition absente), elle est automatiquement
 *     effacée puis réinitialisée. Cela garantit un démarrage propre même
 *     après un flash partiel ou une coupure de courant pendant une écriture.
 *  2. **Infrastructure réseau** : WiFi (STA ou AP), serveur Web, WebSocket,
 *     chargement de la configuration depuis NVS (sysCfg).
 *  3. **Moteur BACnet** : Initialisation UART RS485, démarrage de la tâche
 *     FSM MS/TP épinglée sur le Core 1.
 *  4. **Tâche système** : Création de la tâche réseau sur le Core 0 avec
 *     une pile de 8 Ko et une priorité de 5 (inférieure à la tâche BACnet).
 */
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n\n>>> " + String(configVERSION_GLOBAL) + " - DUAL CORE MODE <<<");

    // --- NVS (Non-Volatile Storage) Self-Healing Routine ---
    // Détecte 3 types de corruption : pages pleines, changement de version du
    // format NVS, ou partition manquante. Dans tous les cas, on efface et
    // on réinitialise pour garantir un état propre.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND || err == ESP_ERR_NVS_NOT_FOUND) {
        Serial.println("[NVS] Critical corruption or missing partition. Formatting...");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    
    // Infrastructure Initialization
    setup_network_infrastructure(); // Prepares WiFi + WebServer
    setup_bacnet_engine();          // Starts BACnet on Core 1

    // Création de la tâche système sur le Core 0.
    // Priorité 5 : suffisante pour les services réseau, inférieure à la
    // tâche BACnet MS/TP qui nécessite un ordonnancement temps réel.
    xTaskCreatePinnedToCore(system_task, "SystemTask", 8192, NULL, 5, NULL, 0);

    Serial.println("[" + String(configVERSION_GLOBAL) + "] System Operational.");
}

// ============================================================================
// loop() — Boucle Arduino (neutralisée)
// ============================================================================

/**
 * @brief Boucle Arduino standard — immédiatement supprimée.
 *
 * La boucle Arduino par défaut s'exécute sur le Core 1. Comme ce core est
 * entièrement dédié à la FSM BACnet MS/TP temps réel, on supprime la tâche
 * loop() via vTaskDelete(NULL) pour :
 *  - Libérer les ~8 Ko de pile alloués à la tâche loopTask.
 *  - Éviter toute interférence de scheduling avec la tâche BACnet.
 *
 * @warning Ne jamais ajouter de code avant vTaskDelete(NULL) ici. Tout code
 *          périodique doit être placé dans system_task (Core 0) ou dans la
 *          tâche BACnet (Core 1).
 */
void loop() {
    // The default Arduino loop runs on Core 1.
    // We leave it empty and delete it so it does not interfere with the BACnet task (Core 1).
    vTaskDelete(NULL); // Delete the loop task to free up resources
}
