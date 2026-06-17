/**
 * @file z_config.h
 * @brief Configuration centrale du projet BACnet2MQTT.
 *
 * Ce fichier constitue le point de référence unique pour l'ensemble des
 * paramètres du système : réseau WiFi, protocole BACnet MS/TP, MQTT,
 * Home Assistant, matériel RS-485 et serveur web embarqué.
 *
 * @details
 * - Toutes les valeurs par défaut sont définies ici via des macros DEFAULT_*.
 *   Elles servent de valeurs de repli si la NVS (Non-Volatile Storage) est vide
 *   ou corrompue.
 * - La structure Config est partagée globalement via la variable sysCfg.
 *   Elle est chargée depuis la NVS au démarrage (z_nvs.cpp) et peut être
 *   modifiée à chaud via l'interface web (z_network.cpp).
 * - Les broches matérielles RS-485 sont spécifiques au Waveshare ESP32-S3-RS485-CAN.
 *   ATTENTION : Le GPIO 47 est réservé au bus Octal SPI de la PSRAM et ne doit
 *   JAMAIS être reconfiguré.
 *
 * @note Architecture multi-cœur :
 *   - Core 0 : WiFi, serveur web (webServer/ws), MQTT, OTA.
 *   - Core 1 : FSM BACnet MS/TP temps réel (z_bacnet.cpp).
 *
 * @see z_nvs.h  Pour le chargement/sauvegarde de la configuration.
 * @see z_bacnet.h Pour les constantes et structures du protocole BACnet.
 */
#ifndef Z_CONFIG_H
#define Z_CONFIG_H

// Bibliothèques requises par l'ensemble du projet
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <atomic>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoOTA.h>
#include <mqtt_client.h>

// ============================================================================
// CONFIGURATION SYSTÈME PAR DÉFAUT
// Ces valeurs sont utilisées comme repli si la NVS est vide ou corrompue.
// ============================================================================

/** @brief Version globale du firmware, incrémentée avant chaque commit Git. */
#define configVERSION_GLOBAL "v7.1.12"

/** @name Configuration WiFi par défaut */
///@{
#define DEFAULT_SSID    "Freebox-A4297A"
#define DEFAULT_STATIC_IP "192.168.1.50"
#define DEFAULT_GATEWAY "192.168.1.254"
#define DEFAULT_SUBNET "255.255.255.0"
///@}

/** @name Configuration BACnet MS/TP par défaut
 *  @details
 *  - MAX_MASTER : Plus haute adresse MAC active sur le bus MS/TP. Réduire
 *    cette valeur accélère le Poll-For-Master (moins d'adresses à sonder).
 *  - DEVICE_ID : Identifiant BACnet unique du gateway (Prop 75).
 *  - MAC_ADDRESS : Adresse MAC locale sur le segment MS/TP (0-127).
 *  - APDU_TIMEOUT : Délai d'attente d'une réponse BACnet (ms). Augmenté
 *    pour les automates lents (ex: ECB-203 ~240ms).
 */
///@{
#define DEFAULT_MAX_MASTER 5
#define DEFAULT_DEVICE_ID 123
#define DEFAULT_MAC_ADDRESS 1
#define DEFAULT_APDU_TIMEOUT 300
#define DEFAULT_MAX_RETRIES 3
#define DEFAULT_BACNET_POLL 30
///@}

/** @name Configuration MQTT par défaut */
///@{
#define DEFAULT_MQTT_SERVER "192.168.1.11"
#define DEFAULT_MQTT_POLL 30
#define DEFAULT_HA_DISCOVER true
///@}

/** @name Plages par défaut pour les entités Home Assistant de type 'number'
 *  @details Ces valeurs sont utilisées lors de l'auto-discovery HA quand
 *  l'objet BACnet ne fournit pas de Min/Max/Step via ses propriétés 65/69.
 */
///@{
#define DEFAULT_NUM_MIN -100.0f
#define DEFAULT_NUM_MAX 100.0f
#define DEFAULT_NUM_STEP 1.0f
///@}

/** @name Paramètres avancés MS/TP
 *  @details
 *  - MAX_INFO_FRAMES : Nombre max de trames de données émises par rotation
 *    de jeton (ASHRAE 135 §9.5.7.2). Limiter à 3 pour ne pas monopoliser le bus.
 *  - HEARTBEAT_INTERVAL : Intervalle (ms) de vérification de santé interne.
 *  - TOKEN_SKIP : Nombre de rotations de jeton à ignorer avant d'émettre
 *    une requête (régulation de débit, cf. leçon : 1 req / 20 jetons max).
 */
///@{
#define DEFAULT_MAX_INFO_FRAMES 3
#define DEFAULT_HEARBEAT_INTERVAL 50000
#define DEFAULT_TOKEN_SKIP  0
///@}

// ============================================================================
// CONFIGURATION MATÉRIELLE RS-485
// Spécifique au Waveshare ESP32-S3-RS485-CAN.
// ATTENTION : GPIO 47 est réservé par le bus Octal SPI PSRAM — NE PAS TOUCHER.
// ============================================================================

/** @brief Broche de réception UART RS-485. */
#define RX_PIN 18
/** @brief Broche de transmission UART RS-485. */
#define TX_PIN 17
/** @brief Broche Request-To-Send / Driver Enable du transceiver RS-485.
 *  @details Gérée automatiquement par le mode UART_MODE_RS485_HALF_DUPLEX. */
#define RTS_PIN 21
/** @brief Port UART matériel utilisé pour le bus MS/TP (UART1). */
#define RS485_UART_PORT UART_NUM_1
/** @brief Port d'écoute du serveur web embarqué. */
#define WEB_PORT 80

// ============================================================================
// NIVEAUX DE LOG (Convention Syslog)
// Utilisés par z_log() dans z_network.cpp pour filtrer les messages.
// ============================================================================
#define pdLOG_ERROR 1   ///< Erreurs critiques nécessitant une action immédiate.
#define pdLOG_WARN  2   ///< Avertissements (comportement dégradé mais non fatal).
#define pdLOG_INFO  3   ///< Informations de fonctionnement normal.
#define pdLOG_DEBUG 4   ///< Détails techniques pour le débogage.

// ============================================================================
// STRUCTURE DE CONFIGURATION GLOBALE
// ============================================================================

/**
 * @brief Structure principale de configuration du système.
 *
 * Contient tous les paramètres persistants du gateway BACnet2MQTT.
 * Chargée depuis la NVS au démarrage via load_configuration() et
 * sauvegardée via save_configuration() après modification par l'UI web.
 *
 * @note Les tailles des champs char sont dimensionnées pour les cas d'usage
 *       typiques en domotique. mqtt_prefix est plus large (64) pour permettre
 *       des hiérarchies de topics profondes.
 */
struct Config {
    char wifi_ssid[32];             // Nom du réseau WiFi (SSID)
    char wifi_pass[64];             // Mot de passe WiFi
    bool static_ip;                 // true = IP fixe, false = DHCP
    char local_ip[16];              // Adresse IP fixe (si static_ip == true)
    char gateway[16];               // Adresse IP de la passerelle (routeur)
    char subnet[16];                // Masque de sous-réseau
    uint8_t ucMacAddress;            // Adresse MAC MS/TP locale (0-127)
    uint8_t max_master;             // Plus haute adresse MAC sur le bus MS/TP
    uint32_t ulDeviceId;             // Identifiant unique du device BACnet (Prop 75)
    uint16_t ulApduTimeout;          // Timeout APDU en millisecondes
    uint8_t max_retries;            // Nombre de tentatives de retransmission APDU
    uint16_t bacnet_poll_interval;  // Intervalle de polling BACnet (secondes)
    char mqtt_server[32];           // Adresse IP ou nom d'hôte du broker MQTT
    uint16_t mqtt_port;             // Port du broker MQTT (typiquement 1883)
    char mqtt_user[32];             // Identifiant MQTT (authentification)
    char mqtt_pass[32];             // Mot de passe MQTT (authentification)
    char mqtt_prefix[64];           // Préfixe de base pour tous les topics MQTT
    uint16_t mqtt_poll_interval;    // Intervalle de publication MQTT (secondes)
    bool ha_discover;               // Active l'auto-discovery Home Assistant
    float default_number_min;       // Valeur MIN par défaut pour les entités HA 'number'
    float default_number_max;       // Valeur MAX par défaut pour les entités HA 'number'
    float default_number_step;      // Pas (STEP) par défaut pour les entités HA 'number'
    uint8_t log_level;              // Niveau de verbosité des logs (pdLOG_ERROR..pdLOG_DEBUG)
    char admin_user[32];            // Nom d'utilisateur de l'interface web admin
    char admin_pass[64];            // Mot de passe de l'interface web admin
    uint32_t heartbeat_interval;    // Intervalle du heartbeat interne (ms)
    uint8_t token_skip;             // Rotations de jeton à ignorer avant émission
    uint8_t max_info_frames;        // Trames max émises par rotation de jeton
};

// ============================================================================
// VARIABLES GLOBALES PARTAGÉES
// ============================================================================

extern Config sysCfg;                           ///< Instance globale de la configuration système.
extern AsyncWebServer webServer;                ///< Serveur HTTP asynchrone (Core 0).
extern AsyncWebSocket ws;                       ///< WebSocket pour les logs temps réel vers l'UI.
extern esp_mqtt_client_handle_t mqtt_client;    ///< Handle du client MQTT ESP-IDF.
extern bool is_ap_mode;                         ///< true si le device fonctionne en mode Access Point (WiFi non trouvé).
extern bool pending_reboot;                     ///< true si un redémarrage est programmé (ex: après OTA ou changement de config critique).
extern uint32_t reboot_timer;                   ///< Timestamp (millis) de déclenchement du redémarrage différé.

#endif
