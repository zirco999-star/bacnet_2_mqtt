/**
 * @file z_network.h
 * @brief Infrastructure réseau : WiFi, serveur web, OTA, logging et allocateur PSRAM.
 *
 * Ce fichier déclare l'allocateur mémoire PSRAM pour ArduinoJson, les
 * prototypes de l'infrastructure réseau (WiFi, serveur web, OTA), le
 * système de logging unifié, et les mutex de protection des ressources web.
 *
 * @details
 * Composants gérés par ce module (tous sur Core 0) :
 *   - **WiFi** : Connexion station ou mode Access Point de secours.
 *   - **Serveur web** : API REST asynchrone + interface embarquée (z_ui.h).
 *   - **WebSocket** : Flux de logs temps réel vers l'interface web.
 *   - **OTA** : Mise à jour firmware Over-The-Air via ArduinoOTA.
 *   - **Logging** : Système z_log() unifié avec filtrage par niveau.
 *
 * Gestion mémoire :
 *   L'allocateur PSRAM_Allocator redirige toutes les allocations JSON
 *   vers la PSRAM externe (8 Mo OPI) via heap_caps_malloc(MALLOC_CAP_SPIRAM).
 *   Ceci est critique pour les gros payloads JSON (export de la base BACnet,
 *   auto-discovery HA) qui dépasseraient la SRAM interne (~320 Ko).
 *
 * @warning Le GPIO 47 est utilisé par le bus Octal SPI de la PSRAM.
 *          Ne JAMAIS le reconfigurer en entrée ou sortie.
 *
 * @see z_config.h  Pour les paramètres réseau (WiFi, port web).
 * @see z_ui.h      Pour le contenu HTML/JS/CSS de l'interface embarquée.
 */
#ifndef Z_NETWORK_H
#define Z_NETWORK_H

#include "z_config.h"
#include "z_ui.h"
#include <ArduinoOTA.h>
#include <ArduinoJson.h>

// ============================================================================
// ALLOCATEUR PSRAM POUR ARDUINOJSON (v6.4.9)
// Redirige toutes les allocations JSON vers la PSRAM externe (8 Mo OPI)
// pour éviter la saturation de la SRAM interne lors des gros payloads.
// ============================================================================

/**
 * @brief Allocateur mémoire personnalisé pour ArduinoJson utilisant la PSRAM.
 *
 * @details Surcharge les 3 méthodes d'allocation d'ArduinoJson::Allocator
 * pour utiliser heap_caps_malloc/realloc/free avec le flag MALLOC_CAP_SPIRAM.
 * Le flag MALLOC_CAP_8BIT garantit un alignement octet compatible avec
 * toutes les structures JSON.
 *
 * @note Instance globale : psram_alloc (déclarée dans z_network.cpp).
 *       Utilisation typique : JsonDocument doc(&psram_alloc);
 */
struct PSRAM_Allocator : ArduinoJson::Allocator {
    void* allocate(size_t size) override {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    void deallocate(void* ptr) override {
        heap_caps_free(ptr);
    }
    void* reallocate(void* ptr, size_t new_size) override {
        return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
};

/** @brief Instance globale de l'allocateur PSRAM pour ArduinoJson. */
extern PSRAM_Allocator psram_alloc;

// ============================================================================
// PROTOTYPES DE L'INFRASTRUCTURE RÉSEAU
// ============================================================================

/**
 * @brief Initialise l'ensemble de l'infrastructure réseau.
 * @details Configure dans l'ordre : WiFi (station ou AP), serveur web
 *          (routes API + UI), WebSocket, OTA, et mutex de protection.
 *          Appelée une seule fois depuis setup() dans bacnet_2_mqtt.ino.
 */
void setup_network_infrastructure();

/**
 * @brief Boucle de traitement réseau : OTA, reconnexion WiFi, heartbeat.
 * @details Appelée périodiquement depuis la boucle principale (Core 0).
 */
void handle_network();

/** @brief Mutex de protection des handlers API REST.
 *  @details Empêche les accès concurrents aux routes web depuis
 *  plusieurs requêtes HTTP simultanées (serveur asynchrone). */
extern SemaphoreHandle_t api_mutex;

/** @brief Mutex de protection des envois WebSocket.
 *  @details Sérialise les écritures WebSocket pour éviter les corruptions
 *  quand z_log() et les handlers web écrivent simultanément. */
extern SemaphoreHandle_t ws_mutex;

// ============================================================================
// SYSTÈME DE LOGGING UNIFIÉ
// ============================================================================

/**
 * @brief Fonction de logging centralisée avec filtrage par niveau.
 *
 * @param level  Niveau de log (pdLOG_ERROR, pdLOG_WARN, pdLOG_INFO, pdLOG_DEBUG).
 * @param tag    Tag identifiant le module source (ex: "MSTP", "MQTT", "NVS").
 * @param format Chaîne de format printf.
 * @param ...    Arguments variadiques pour le formatage.
 *
 * @details Les messages sont envoyés simultanément vers :
 *   - Le port série (Serial) pour le débogage local.
 *   - Le WebSocket (ws) pour l'affichage en temps réel dans l'UI web.
 *   Le niveau de filtrage est configurable via sysCfg.log_level.
 */
void z_log(int level, const char* tag, const char* format, ...);

/**
 * @brief Vérifie l'authentification HTTP Basic d'une requête web.
 * @param request Pointeur vers la requête HTTP entrante.
 * @return true si la requête est authentifiée (credentials valides ou auth désactivée).
 */
bool is_authenticated(AsyncWebServerRequest *request);

#endif
