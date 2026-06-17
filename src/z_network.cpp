/**
 * @file z_network.cpp
 * @brief Implémentation de l'infrastructure réseau, du serveur web et du système de journalisation.
 *
 * @details Ce module constitue le cœur de la couche réseau de la passerelle BACnet2MQTT.
 *          Il orchestre les composants suivants, tous exécutés sur le Cœur 0 de l'ESP32-S3 :
 *
 *          - **WiFi** : Connexion STA avec IP statique ou DHCP, fallback automatique en
 *            Point d'Accès (AP) si la connexion échoue après 30 secondes.
 *          - **Serveur Web Asynchrone** : ESPAsyncWebServer servant une SPA (Single Page Application)
 *            embarquée depuis z_ui.h, avec authentification HTTP Basic sur toutes les routes.
 *          - **API REST** : Ensemble de routes POST/GET pour piloter le système (configuration,
 *            découverte BACnet, lecture/écriture de propriétés, gestion du cache NVS).
 *          - **WebSockets** : Canal de diffusion des logs temps réel vers l'interface web,
 *            alimenté par une file d'attente FreeRTOS depuis n'importe quel cœur.
 *          - **OTA** : Mises à jour firmware Over-The-Air via ArduinoOTA.
 *          - **Sérialisation JSON** : Utilisation d'ArduinoJson v7 avec un allocateur PSRAM
 *            dédié (PSRAM_Allocator) pour éviter la fragmentation de la heap interne.
 *
 *          Architecture de sécurité inter-tâches :
 *          - `api_mutex`   : Protège la sérialisation JSON des routes API contre les accès
 *                            concurrents (une seule requête API lourde à la fois).
 *          - `ws_mutex`    : Protège l'accès au client WebSocket actif lors de l'envoi des logs.
 *          - `cache_mutex` : Protège le vecteur `bacnet_network_cache` partagé entre le
 *                            Core 0 (API/MQTT) et le Core 1 (FSM MS/TP).
 *
 * @note    Ce fichier ne doit PAS contenir de logique BACnet ou MQTT. Ces responsabilités
 *          sont déléguées à z_bacnet.cpp et z_mqtt.cpp respectivement.
 *
 * @see     z_network.h   - Déclarations publiques (prototypes, allocateur PSRAM, mutex).
 * @see     z_config.h    - Structure Config (sysCfg) et constantes globales.
 * @see     z_bacnet.h    - Structures BACnetDevice/BACnetObject et file de jobs BACnet.
 * @see     z_mqtt.h      - Interface MQTT (publication, découverte Home Assistant).
 * @see     z_nvs.h       - Persistance NVS (sauvegarde/chargement configuration et objets).
 * @see     z_ui.h        - Interface utilisateur HTML embarquée (INDEX_HTML, favicons).
 *
 * @version v7.0.7
 * @date    2026
 */

#include "z_network.h"
#include "z_bacnet.h"
#include "z_mqtt.h"
#include "z_nvs.h"
#include <ESPmDNS.h>
#include "esp_wifi.h"
#include <stdarg.h>
#include "nvs_flash.h"

// Déclaration externe de l'objet WebSocket global défini dans le .ino principal.
extern AsyncWebSocket ws;

// Variables statiques pour la gestion de la connexion WiFi.
static uint32_t wifi_connect_start = 0;
static bool wifi_fallback_active = false;

// Structure to hold log messages in the queue
/**
 * @brief Structure pour stocker un message de log dans la file d'attente FreeRTOS.
 * @details Permet de passer des messages de log de n'importe quelle tâche à la tâche de logging WebSocket.
 */
typedef struct {
    char message[256];
} LogMessage;

// ============================================================================
// Variables globales inter-tâches
// ============================================================================

/**
 * File d'attente FreeRTOS pour le transport asynchrone des messages de log.
 * Capacité : 20 messages de 256 octets. Le producteur (z_log) n'attend jamais
 * (xQueueSend avec timeout=0), les messages sont simplement perdus si la file est pleine.
 */
QueueHandle_t log_queue = NULL;

/**
 * Mutex protégeant les routes API qui sérialisent du JSON volumineux.
 * Empêche les collisions entre une requête /api/status et /api/objects simultanées,
 * et sert aussi de "verrou de courtoisie" pour la tâche WebSocket (qui n'envoie pas
 * de logs pendant qu'une réponse API est en cours de construction).
 */
SemaphoreHandle_t api_mutex = NULL;

/**
 * Mutex protégeant l'envoi unitaire de messages vers le client WebSocket.
 * Nécessaire car la tâche de log (Core 0, priorité 2) et le callback onEvent
 * peuvent accéder au client WS depuis des contextes différents.
 */
SemaphoreHandle_t ws_mutex = NULL;

/** Instance globale de l'allocateur PSRAM pour ArduinoJson (déclarée dans z_network.h). */
PSRAM_Allocator psram_alloc;

// Variables statiques pour la gestion de la session WebSocket.
/** Timestamp du dernier événement WS (connexion/déconnexion). Sert de garde temporelle. */
static uint32_t last_ws_event = 0;
/** ID du client WebSocket actuellement connecté. 0 = aucun client actif.
 *  Seul un client est supporté simultanément pour éviter la saturation AsyncTCP. */
static uint32_t active_ws_id = 0;

// ============================================================================
// WebSocket : Gestion des événements et tâche d'envoi des logs
// ============================================================================

/**
 * @brief Callback pour les événements du serveur WebSocket.
 * @details Gère la connexion et la déconnexion des clients. Mémorise l'ID du client actif
 *          pour ne diffuser les logs qu'à une seule session, évitant les crashs lors de rafraîchissements rapides.
 *
 *          Stratégie mono-client (v6.6.0) :
 *          - À chaque nouvelle connexion, on remplace `active_ws_id` par le nouveau client.
 *          - À la déconnexion, on ne remet `active_ws_id` à 0 que si c'est le client actif qui part.
 *          - Les callbacks sont volontairement atomiques (pas de mutex) pour ne jamais bloquer la pile TCP.
 *
 * @param server Pointeur vers le serveur WebSocket.
 * @param client Pointeur vers le client WebSocket concerné.
 * @param type   Type d'événement (WS_EVT_CONNECT, WS_EVT_DISCONNECT, WS_EVT_DATA, etc.).
 * @param arg    Données d'argument spécifiques à l'événement (non utilisé ici).
 * @param data   Pointeur vers les données reçues du client (non utilisé ici).
 * @param len    Longueur des données reçues (non utilisé ici).
 */
void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
             void *arg, uint8_t *data, size_t len) {
    // v6.6.0: Callbacks atomiques pour éviter tout blocage TCP
    if (type == WS_EVT_CONNECT) {
        last_ws_event = millis();
        active_ws_id = client->id();
        printf("[WEB] WS Session Link: %u\n", active_ws_id);
    } 
    else if (type == WS_EVT_DISCONNECT) {
        if (active_ws_id == client->id()) active_ws_id = 0;
        last_ws_event = millis();
        printf("[WEB] WS Session Terminated\n");
    }
}

// Task running on Core 0 to send logs to the web interface via WebSockets
/**
 * @brief Tâche dédiée à l'envoi des logs via WebSocket (s'exécute sur le Cœur 0).
 * @details Cette tâche attend en permanence des messages dans `log_queue`. Lorsqu'un message est reçu,
 *          elle l'envoie de manière sécurisée au client WebSocket actif. Elle inclut des gardes-fous
 *          pour éviter de saturer la pile réseau ou de causer des corruptions de mémoire.
 *
 *          Protections multicouches contre les crashs réseau :
 *          1. **Silence au démarrage** (10s) : Laisse le WiFi et le MQTT s'établir avant tout envoi.
 *          2. **Garde temporelle** (5s) : Après un événement WS (connect/disconnect), les logs
 *             sont jetés pour éviter les collisions lors d'un rafraîchissement de page.
 *          3. **Vérification du mutex API** : Si une route API est en cours de sérialisation JSON,
 *             on jette le log plutôt que de risquer un crash mémoire (erreur 0x30 historique).
 *          4. **Seuil de heap** (50 Ko) : Pas d'envoi si la mémoire libre est trop basse.
 *          5. **Mutex WS** (20ms timeout) : Accès exclusif au client WS avec abandon rapide.
 *          6. **Délai inter-messages** (50ms) : Empêche la saturation du buffer AsyncTCP.
 *
 * @param pvParameters Paramètres de la tâche FreeRTOS (non utilisés).
 */
static void websocket_log_task(void *pvParameters) {
    LogMessage received_log;
    // v6.6.0: Silence radio total de 10s au démarrage pour laisser le WiFi et MQTT s'établir
    vTaskDelay(pdMS_TO_TICKS(10000));
    
    for( ;; ) {
        if (xQueueReceive(log_queue, &received_log, portMAX_DELAY) == pdTRUE) {
            // v6.6.0: Pas d'envoi si un événement réseau récent (< 5s) pour éviter collision au refresh
            if (millis() - last_ws_event < 5000) continue;

            // v6.6.0: SI L'API EST OCCUPEE (chargement page/JSON), ON JETTE LE LOG
            // C'est la protection ultime contre le crash 0x30
            if (api_mutex == NULL || uxSemaphoreGetCount(api_mutex) == 0) continue;

            if (active_ws_id > 0 && ESP.getFreeHeap() > 50000) {
                if (xSemaphoreTake(ws_mutex, pdMS_TO_TICKS(20))) {
                    AsyncWebSocketClient* target = ws.client(active_ws_id);
                    if (target && target->status() == WS_CONNECTED) {
                        target->text(received_log.message);
                    } else {
                        active_ws_id = 0;
                    }
                    xSemaphoreGive(ws_mutex);
                }
                // v6.6.0: Délai forcé entre chaque message pour ne pas saturer la pile AsyncTCP
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
    }
}

// ============================================================================
// Système de journalisation unifié (z_log)
// ============================================================================

// Safe logging function that adds messages to the queue without blocking the program
/**
 * @brief Système de journalisation unifié, thread-safe et multi-destination.
 * @details Formate un message et l'envoie à la fois sur le port Série (UART) et
 *          vers une file d'attente pour une transmission asynchrone via WebSocket.
 *          Filtre les messages en fonction du niveau de log global (`sysCfg.log_level`).
 *
 *          Format de sortie UART :  [core][LVL][TAG] message
 *          Format de sortie WS   :  core|[HH:MM:SS,mmm][LVL][TAG] message
 *
 *          Le format WS inclut un timestamp car le moniteur série ESP-IDF en ajoute
 *          automatiquement un, mais pas le navigateur web.
 *
 *          Cette fonction est appelable depuis n'importe quel cœur et n'importe quelle
 *          tâche FreeRTOS, y compris la FSM MS/TP du Core 1.
 *
 * @param level  Niveau de sévérité du log (pdLOG_ERROR=1, pdLOG_WARN=2, pdLOG_INFO=3, pdLOG_DEBUG=4).
 * @param tag    Un court identifiant de module (ex: "WIFI", "MQTT", "BACNET", "API").
 * @param format Chaîne de formatage de type `printf`.
 * @param ...    Arguments variables pour la chaîne de formatage.
 */
void z_log(int level, const char* tag, const char* format, ...) {
    // 1. Filtrage temps réel : on ignore le log si sa priorité est trop faible
    if (level > sysCfg.log_level) return;

    // 2. Détermination de la chaîne de sévérité
    char lvl_str[5] = "INF";
    if (level == pdLOG_ERROR) strcpy(lvl_str, "ERR");
    else if (level == pdLOG_WARN) strcpy(lvl_str, "WRN");
    else if (level == pdLOG_DEBUG) strcpy(lvl_str, "DBG");

    // 3. Calcul du temps
    uint32_t now_ms = millis();
    uint32_t ms = now_ms % 1000;
    uint32_t s = (now_ms / 1000) % 60;
    uint32_t m = (now_ms / 60000) % 60;
    uint32_t h = (now_ms / 3600000); 

    // Création du timestamp formaté (13 caractères + '\0' = 14)
    char timestamp[14];
    snprintf(timestamp, sizeof(timestamp), "%02lu:%02lu:%02lu,%03lu", h, m, s, ms);

    int core = xPortGetCoreID();

    // 4. Formatage de la base commune : "[lvl_str][tag] "
    LogMessage base_msg;
    int prefix_len = snprintf(base_msg.message, sizeof(base_msg.message), "[%s][%s] ", lvl_str, tag);

    // 5. Extraction des arguments variables (vsnprintf) pour le "msg"
    if (prefix_len > 0 && prefix_len < sizeof(base_msg.message)) {
        va_list arg;
        va_start(arg, format);
        vsnprintf(base_msg.message + prefix_len, sizeof(base_msg.message) - prefix_len, format, arg);
        va_end(arg);
    }

    // 6. Sortie physique UART : [core] + base_msg
    // Le moniteur ESP s'occupera d'ajouter son propre timestamp devant
    printf("[%d]%s", core, base_msg.message);

    // 7. Envoi asynchrone sécurisé vers la tâche WebSocket
    if (log_queue != NULL) {
        LogMessage ws_msg;
        
        // Format demandé : core|[timestamp][lvl_str][tag] msg
        snprintf(ws_msg.message, sizeof(ws_msg.message), "%d|[%s]%s", core, timestamp, base_msg.message);
        
        xQueueSend(log_queue, &ws_msg, 0);
    }
}

// ============================================================================
// Configuration des routes du serveur web
// ============================================================================

/**
 * @brief Configure toutes les routes (endpoints) du serveur web asynchrone.
 * @details Enregistre l'ensemble des handlers pour :
 *          - L'interface utilisateur web (SPA depuis PROGMEM/z_ui.h)
 *          - Les favicons avec cache navigateur longue durée (1 an)
 *          - Les API REST de lecture d'état (/api/status, /api/objects)
 *          - Les API de commande BACnet (/api/whois, /api/iam, /api/writevalue, etc.)
 *          - Les API de configuration (/save, /api/save_object, /api/toggle_device)
 *          - Les API de maintenance (/api/reset_cache, /api/factory_reset, /api/reboot)
 *          - Le canal WebSocket (/ws) pour la diffusion des logs
 *
 *          Toutes les routes nécessitant une authentification appellent `is_authenticated()`
 *          en premier. Les routes lourdes (JSON) sont protégées par `api_mutex` et vérifient
 *          un seuil de heap minimum (50 Ko) avant d'allouer.
 *
 * @note    Cette fonction est appelée une seule fois depuis `setup_network_infrastructure()`.
 */
void setup_web_routes() {
    // -------------------------------------------------------------------------
    // WEBSOCKETS & INTERFACE WEB
    // -------------------------------------------------------------------------
    
    // Enregistrement du gestionnaire d'événements WebSocket pour la transmission asynchrone des logs.
    ws.onEvent(onEvent); // v6.5.2: Callback obligatoire pour la thread-safety
    webServer.addHandler(&ws);
    
    // Route principale : Sert l'interface utilisateur web (Single Page Application).
    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!is_authenticated(request)) return;
        request->send_P(200, "text/html", reinterpret_cast<const uint8_t*>(INDEX_HTML), sizeof(INDEX_HTML));
    });

    // Routes statiques pour servir les différentes tailles d'icônes (Favicons).
    // Ces icônes sont mises en cache par le navigateur pendant 1 an (max-age=31536000) pour optimiser les performances.
    webServer.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse *response = request->beginResponse_P(200, "image/x-icon", favicon_ico, favicon_ico_len);
        if (response) {
            response->addHeader("Cache-Control", "public, max-age=31536000");
            request->send(response);
        }
    });

    webServer.on("/favicon-96x96.png", HTTP_GET,  [](AsyncWebServerRequest *request){
        request->send_P(200, "image/png", favicon_96, favicon_96_len);
    });

    webServer.on("/web-app-manifest-192x192.png", HTTP_GET,  [](AsyncWebServerRequest *request){
        request->send_P(200, "image/png", favicon_192, favicon_192_len);
    });

    webServer.on("/apple-touch-icon.png", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse *response = request->beginResponse_P(200, "image/png", favicon_apple, favicon_apple_len);
        if (response) {
            response->addHeader("Cache-Control", "public, max-age=31536000");
            request->send(response);
        }
    });

    // -------------------------------------------------------------------------
    // ROUTES API - LECTURE DES DONNÉES ET ÉTAT DU SYSTÈME
    // -------------------------------------------------------------------------

    /**
     * @route   GET /api/status
     * @brief   Retourne l'état global du système sous forme de JSON.
     * @details Endpoint le plus sollicité par l'interface web (polling périodique pour le dashboard).
     *          Construit un document JSON contenant :
     *          - Informations WiFi (RSSI, IP courante, masque, passerelle)
     *          - Configuration système complète (sysCfg)
     *          - État MQTT (connecté/déconnecté)
     *          - Statistiques MS/TP (ring actif, compteurs trames RX/TX, erreurs CRC)
     *          - Résumé de chaque équipement BACnet (sans le détail des objets, pour la légèreté)
     *          - Métriques système (heap libre en Ko, uptime en secondes)
     *
     *          Protection double-mutex : api_mutex (1s) puis cache_mutex (100ms) pour
     *          l'accès au vecteur d'équipements. En cas de timeout, retourne 503.
     *
     *          Le header "Connection: close" force la fermeture TCP pour libérer les
     *          ressources AsyncTCP immédiatement après l'envoi.
     */
    webServer.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!is_authenticated(request)) return;

        // v6.4.8: Strict Heap Protection
        // Sécurité critique : On empêche la création du gros objet JSON si la RAM libre est trop basse.
        if (ESP.getFreeHeap() < 50000) {
            request->send(503, "text/plain", "Service Unavailable: Low Memory");
            return;
        }

        // v6.4.8: API Serialization
        // Utilisation d'un mutex pour éviter qu'une autre tâche (ex: MS/TP) ne modifie le cache pendant la lecture.
        if (xSemaphoreTake(api_mutex, pdMS_TO_TICKS(1000))) {
            // v6.4.9: Force PSRAM allocation
            JsonDocument doc(&psram_alloc);
            doc["ver"] = configVERSION_GLOBAL;
            doc["rssi"] = WiFi.RSSI();
            doc["cur_ip"] = WiFi.localIP().toString();
            doc["cur_mask"] = WiFi.subnetMask().toString();
            doc["cur_gw"] = WiFi.gatewayIP().toString();
            
            doc["ssid"] = sysCfg.wifi_ssid;
            doc["static"] = sysCfg.static_ip;
            doc["ip"] = sysCfg.local_ip;
            doc["gw"] = sysCfg.gateway;
            doc["mask"] = sysCfg.subnet;
            
            doc["mqs"] = sysCfg.mqtt_server;
            doc["mqu"] = sysCfg.mqtt_user;
            doc["mqpr"] = sysCfg.mqtt_prefix;
            doc["ha_disc"] = sysCfg.ha_discover;
            doc["n_min"] = sysCfg.default_number_min;
            doc["n_max"] = sysCfg.default_number_max;
            doc["n_stp"] = sysCfg.default_number_step;
            
            doc["mqtt"] = is_mqtt_connected();
            doc["heap"] = ESP.getFreeHeap() / 1024;
            doc["uptime"] = millis() / 1000;
            
            doc["mac"] = sysCfg.ucMacAddress;
            doc["mm"] = sysCfg.max_master;
            doc["did"] = sysCfg.ulDeviceId;
            doc["to"] = sysCfg.ulApduTimeout;
            doc["ret"] = sysCfg.max_retries;
            doc["hbeat"] = sysCfg.heartbeat_interval;
            doc["tskip"] = sysCfg.token_skip;
            doc["mif"] = sysCfg.max_info_frames;
            doc["mpi"] = sysCfg.mqtt_poll_interval;
            doc["bpi"] = sysCfg.bacnet_poll_interval;
            doc["adu"] = sysCfg.admin_user;
            doc["lvl"] = sysCfg.log_level;

            doc["mstp_t"] = bacnetStats.xRingActive;
            doc["mstp_cnt"] = bacnetStats.ulTokensSeen;
            doc["mstp_rx"] = bacnetStats.ulMsMsgsRx;
            doc["mstp_tx"] = bacnetStats.ulMsMsgsTx;
            doc["mstp_err"] = bacnetStats.ulErrorsCrc;

            JsonArray devices = doc["devices"].to<JsonArray>();
            if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(100))) {
                for (auto& dev : bacnet_network_cache) {
                    JsonObject d = devices.add<JsonObject>();
                    d["id"] = dev.ulDeviceId;
                    d["step"] = (int)dev.ucDiscStep;
                    d["idx"] = dev.usDiscObjIdx;
                    d["total"] = dev.objects.size();
                    d["enabled"] = dev.xEnabled;
                    d["done"] = dev.xDiscoveryDone;
                    
                    int sel = 0;
                    for(auto& o : dev.objects) if(o.xEnabled) sel++;
                    d["sel"] = sel;
                }
                xSemaphoreGive(cache_mutex);
            }

            String payload;
            serializeJson(doc, payload);
            AsyncWebServerResponse *response = request->beginResponse(200, "application/json", payload);
            if (response) {
                response->addHeader("Connection", "close");
                request->send(response);
            }
            xSemaphoreGive(api_mutex);
        } else {
            request->send(503, "text/plain", "Service Unavailable: API Busy");
        }
    });

    /**
     * @route   GET /api/objects
     * @brief   Retourne l'arborescence complète des équipements et de leurs objets BACnet.
     * @details Endpoint utilisé pour peupler le tableau détaillé dans l'interface web.
     *          Sérialise l'intégralité de `bacnet_network_cache` incluant pour chaque objet :
     *          type, instance, nom, valeur courante, état de polling, unité, limites min/max/step,
     *          références croisées (cMinRef/cMaxRef), status_flags et état OutOfService.
     *
     *          C'est la route la plus coûteuse en mémoire car elle peut générer plusieurs dizaines
     *          de Ko de JSON pour un réseau BACnet conséquent. L'allocation se fait en PSRAM
     *          via `psram_alloc` pour préserver la heap interne SRAM (320 Ko).
     *
     *          Les champs min/max ne sont inclus que s'ils sont définis (différents de NAN) afin
     *          de réduire la taille de la réponse JSON.
     */
    webServer.on("/api/objects", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!is_authenticated(request)) return;

        // v6.4.8: Strict Heap Protection
        if (ESP.getFreeHeap() < 50000) {
            request->send(503, "text/plain", "Service Unavailable: Low Memory");
            return;
        }

        // v6.4.8: API Serialization
        if (xSemaphoreTake(api_mutex, pdMS_TO_TICKS(1000))) {
            // v6.4.9: Force PSRAM allocation
            JsonDocument doc(&psram_alloc); 
            JsonArray controllers = doc.to<JsonArray>();
            
            if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(100))) {
                for (auto& dev : bacnet_network_cache) {
                    JsonObject c = controllers.add<JsonObject>();
                    c["id"] = dev.ulDeviceId;
                    c["device_id"] = dev.ulDeviceId;
                    c["name"] = dev.name; 
                    c["vendor"] = dev.vendor; 
                    c["enabled"] = dev.xEnabled;
                    JsonArray objs_arr = c["objects"].to<JsonArray>();
                    for (auto& o : dev.objects) {
                        JsonObject obj = objs_arr.add<JsonObject>();
                        obj["type"] = o.usType; obj["inst"] = o.ulInstance; obj["name"] = o.cName;
                        obj["val"] = o.fPresentValue; obj["poll"] = o.xEnabled;
                        obj["unit"] = o.cUnitText;
                        if (!isnan(o.fMinValue)) obj["min"] = o.fMinValue;
                        if (!isnan(o.fMaxValue)) obj["max"] = o.fMaxValue;
                        obj["step"] = o.fStepValue;
                        if (strlen(o.cMinRef) > 0) obj["min_ref"] = o.cMinRef;
                        if (strlen(o.cMaxRef) > 0) obj["max_ref"] = o.cMaxRef;
                        obj["status_flags"] = o.ucStatusFlags;
                        obj["outofservice"] = o.isOutOfService();
                        obj["overridden_bacnet"] = o.xOverriddenBacnet;
                    }
                }
                xSemaphoreGive(cache_mutex);
            }
            String payload;
            serializeJson(doc, payload);
            AsyncWebServerResponse *response = request->beginResponse(200, "application/json", payload);
            if (response) {
                response->addHeader("Connection", "close");
                request->send(response);
            }
            xSemaphoreGive(api_mutex);
        } else {
            request->send(503, "text/plain", "Service Unavailable: API Busy");
        }
    });

    // -------------------------------------------------------------------------
    // ROUTES API - COMMANDES BACNET (Jobs asynchrones vers le Core 1)
    // -------------------------------------------------------------------------
    // Ces routes créent des BACnetJob et les enfilent dans `bacnet_job_queue`.
    // Le moteur MS/TP du Core 1 les défile et les exécute lors de la possession
    // du jeton (état MSTP_USE_TOKEN).
    // -------------------------------------------------------------------------

    /**
     * @route   POST /api/whois
     * @brief   Déclenche un broadcast "Who-Is" sur le bus MS/TP.
     * @details Envoie une requête de découverte globale pour identifier tous les
     *          équipements BACnet présents sur le réseau. Les réponses "I-Am" sont
     *          traitées par le Core 1 et ajoutées au `bacnet_network_cache`.
     */
    webServer.on("/api/whois", HTTP_POST, [](AsyncWebServerRequest *request){
        if(!is_authenticated(request)) return;
        BACnetJob job; job.type = JOB_WHO_IS;
        enqueue_bacnet_job(job);
        request->send(200, "text/plain", "WHO-IS ENQUEUED");
    });

    /**
     * @route   POST /api/iam
     * @brief   Force l'envoi d'une réponse "I-Am" de notre propre passerelle.
     * @details Annonce notre présence sur le bus MS/TP. Utile pour que des superviseurs
     *          (YABE, Desigo, etc.) détectent la passerelle sans attendre un Who-Is.
     */
    webServer.on("/api/iam", HTTP_POST, [](AsyncWebServerRequest *request){
        if(!is_authenticated(request)) return;
        BACnetJob job; job.type = JOB_I_AM;
        enqueue_bacnet_job(job);
        request->send(200, "text/plain", "I-AM ENQUEUED");
        z_log(pdLOG_INFO, "API", "I-AM ENQUEUED\n");
    });


    /**
     * @route   POST /api/reload_device
     * @brief   Relance la découverte complète d'un équipement BACnet spécifique.
     * @details Remet l'automate de découverte à l'étape initiale (DISC_DEV_ID) et
     *          sauvegarde l'état en NVS. L'équipement sera redécouvert lors du
     *          prochain cycle de polling.
     * @param   id  (POST) Device ID de l'équipement à redécouvrir.
     */
    webServer.on("/api/reload_device", HTTP_POST, [](AsyncWebServerRequest *request){
        if(!is_authenticated(request)) return;
        if (request->hasParam("id", true)) {
            uint32_t did = request->getParam("id", true)->value().toInt();
            if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(500))) {
                for (auto& dev : bacnet_network_cache) {
                    if (dev.ulDeviceId == did) {
                        dev.xDiscoveryDone = false;
                        dev.ucDiscStep = DISC_DEV_ID;
                        dev.usDiscObjIdx = 0;
                        z_log(pdLOG_INFO, "API", "Reloading device %lu\n", (unsigned long)did);
                        break;
                    }
                }
                xSemaphoreGive(cache_mutex);
                save_device_objects(did);
            }
            request->send(200, "text/plain", "RELOADING");
        } else request->send(400, "text/plain", "Missing id");
    });

    /**
     * @route   POST /api/reload_object
     * @brief   Force la relecture immédiate de la valeur (Present_Value) d'un objet BACnet.
     * @details Crée un job JOB_READ_PROP ciblant la propriété 85 (Present_Value) et
     *          l'enfile pour exécution par le Core 1. Résout l'adresse MAC du device
     *          cible en parcourant le cache.
     * @param   did  (POST) Device ID de l'équipement parent.
     * @param   inst (POST) Instance de l'objet à relire.
     * @param   type (POST) Type d'objet BACnet (enum BACnetObjectType).
     */
    webServer.on("/api/reload_object", HTTP_POST, [](AsyncWebServerRequest *request){
        if(!is_authenticated(request)) return;
        if (request->hasParam("did", true) && request->hasParam("inst", true) && request->hasParam("type", true)) {
            uint32_t did = request->getParam("did", true)->value().toInt();
            uint32_t inst = request->getParam("inst", true)->value().toInt();
            uint16_t type = request->getParam("type", true)->value().toInt();
            
            bool device_found = false;
            BACnetJob job;
            job.type = JOB_READ_PROP; 
            job.obj_type = type;
            job.obj_instance = inst;
            job.prop_id = 85; // Present_Value
            job.array_index = -1; // -1 = Pas de tableau (lecture standard)
            job.target_mac = 255; // Valeur par défaut (Broadcast/Invalide) par sécurité
            
            if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(500))) {
                for (auto& dev : bacnet_network_cache) {
                    if (dev.ulDeviceId == did) {
                        job.target_mac = dev.ucMacAddress;
                        device_found = true;
                        break;
                    }
                }
                xSemaphoreGive(cache_mutex);
            }
            
            if (device_found) {
                enqueue_bacnet_job(job);
                request->send(200, "text/plain", "READ ENQUEUED");
            } else {
                request->send(404, "text/plain", "Device not found");
            }
        } else request->send(400, "text/plain", "Missing params");
    });

    // -------------------------------------------------------------------------
    // ROUTES API - CONFIGURATION ET SAUVEGARDE
    // -------------------------------------------------------------------------

    /**
     * @route   POST /api/save_object
     * @brief   Met à jour les propriétés éditables d'un objet BACnet (nom, unité, polling, limites).
     * @details Endpoint central de l'interface de configuration par objet. Gère :
     *
     *          - **Renommage** : Si le nom change, un WriteProperty(Object_Name, prop_id=77)
     *            est enfilé vers le Core 1 pour propager le nouveau nom vers l'automate BACnet
     *            physique. L'ancien nom est conservé pour le log de traçabilité.
     *
     *          - **Références croisées min/max** : Si la valeur min ou max contient le
     *            caractère ':', elle est interprétée comme une référence vers un autre objet
     *            (format "type:instance"). Le système enregistre une dépendance dans
     *            `ha_dependencies` pour la mise à jour dynamique dans Home Assistant.
     *            Sinon, la valeur est interprétée comme un flottant statique.
     *
     *          - **Contrainte step** : Le pas minimum est contraint à 0.1 pour éviter les
     *            boucles infinies dans les sliders HA.
     *
     *          Après modification, sauvegarde en NVS et déclenche la re-publication HA.
     *
     * @param   did  (POST) Device ID du parent.
     * @param   inst (POST) Instance de l'objet.
     * @param   type (POST) Type d'objet BACnet.
     * @param   name (POST, opt) Nouveau nom de l'objet.
     * @param   unit (POST, opt) Nouveau texte d'unité.
     * @param   poll (POST, opt) "1" pour activer le polling MQTT, "0" pour désactiver.
     * @param   min  (POST, opt) Valeur min ou référence croisée (format "type:instance").
     * @param   max  (POST, opt) Valeur max ou référence croisée.
     * @param   step (POST, opt) Pas d'incrément (minimum 0.1).
     */
    webServer.on("/api/save_object", HTTP_POST, [](AsyncWebServerRequest *request){
        if(!is_authenticated(request)) return;
        if (request->hasParam("did", true) && request->hasParam("inst", true) && request->hasParam("type", true)) {
            uint32_t did = request->getParam("did", true)->value().toInt();
            uint32_t inst = request->getParam("inst", true)->value().toInt();
            uint16_t type = request->getParam("type", true)->value().toInt();
            
            String name = request->hasParam("name", true) ? request->getParam("name", true)->value() : "";
            String unit = request->hasParam("unit", true) ? request->getParam("unit", true)->value() : "";
            bool poll = request->hasParam("poll", true) ? (request->getParam("poll", true)->value() == "1") : true;
            String min_str = request->hasParam("min", true) ? request->getParam("min", true)->value() : "";
            String max_str = request->hasParam("max", true) ? request->getParam("max", true)->value() : "";
            String step_str = request->hasParam("step", true) ? request->getParam("step", true)->value() : "";

            // Tracking du changement de nom pour WriteProperty(Object_Name) vers l'automate
            bool xNameChanged = false;
            uint8_t ucTargetMac = 255;
            char pcOldName[50] = {};

            if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(500))) {
                for (auto& dev : bacnet_network_cache) {
                    if (dev.ulDeviceId == did) {
                        ucTargetMac = dev.ucMacAddress;
                        for (auto& obj : dev.objects) {
                            if (obj.ulInstance == inst && obj.usType == type) {
                                // Détection du changement de nom avant écrasement
                                if (name.length() > 0 && strncmp(obj.cName, name.c_str(), sizeof(obj.cName)) != 0) {
                                    strlcpy(pcOldName, obj.cName, sizeof(pcOldName));
                                    xNameChanged = true;
                                    strlcpy(obj.cName, name.c_str(), sizeof(obj.cName));
                                } else if (name.length() > 0) {
                                    strlcpy(obj.cName, name.c_str(), sizeof(obj.cName));
                                }
                                if (unit.length() > 0) strlcpy(obj.cUnitText, unit.c_str(), sizeof(obj.cUnitText));
                                obj.xEnabled = poll;
                                
                                if (step_str.length() > 0) {
                                    float s = step_str.toFloat();
                                    if (s < 0.1f) s = 0.1f;
                                    obj.fStepValue = s;
                                }
                                
                                if (min_str.length() > 0) {
                                    if (min_str.indexOf(':') != -1) {
                                        strlcpy(obj.cMinRef, min_str.c_str(), sizeof(obj.cMinRef));
                                        String key = String(did) + "_" + min_str;
                                        auto& vec = ha_dependencies[key];
                                        bool found = false;
                                        for (auto& item : vec) { if (item.first == type && item.second == inst) { found = true; break; } }
                                        if (!found) vec.push_back({type, inst});
                                    } else {
                                        obj.cMinRef[0] = '\0';
                                        obj.fMinValue = min_str.toFloat();
                                    }
                                }
                                
                                if (max_str.length() > 0) {
                                    if (max_str.indexOf(':') != -1) {
                                        strlcpy(obj.cMaxRef, max_str.c_str(), sizeof(obj.cMaxRef));
                                        String key = String(did) + "_" + max_str;
                                        auto& vec = ha_dependencies[key];
                                        bool found = false;
                                        for (auto& item : vec) { if (item.first == type && item.second == inst) { found = true; break; } }
                                        if (!found) vec.push_back({type, inst});
                                    } else {
                                        obj.cMaxRef[0] = '\0';
                                        obj.fMaxValue = max_str.toFloat();
                                    }
                                }
                                break;
                            }
                        }
                        break;
                    }
                }
                xSemaphoreGive(cache_mutex); 
            }

            // Si le nom a changé, propager l'écriture vers l'automate BACnet via WriteProperty(Object_Name, prop_id=77)
            if (xNameChanged && ucTargetMac != 255) {
                BACnetJob xNameJob;
                memset(&xNameJob, 0, sizeof(BACnetJob));
                xNameJob.type       = JOB_WRITE_PROP;
                xNameJob.target_mac = ucTargetMac;
                xNameJob.obj_type   = type;
                xNameJob.obj_instance = inst;
                xNameJob.prop_id    = 77; // PROP_OBJECT_NAME
                xNameJob.priority   = 0;  // Non-commandable, pas de priorité BACnet
                strlcpy(xNameJob.name, name.c_str(), sizeof(xNameJob.name));
                enqueue_bacnet_job(xNameJob);
                z_log(pdLOG_INFO, "API", "WriteProperty(Name) ENQUEUED for %u:%lu : '%s' -> '%s'\n",
                      type, (unsigned long)inst, pcOldName, name.c_str());
            }

            save_device_objects(did);
            trigger_ha_discovery(did, inst, type);
            request->send(200, "text/plain", "OK");
        } else request->send(400, "text/plain", "Missing params");
    });

    /**
     * @route   POST /api/toggle_device
     * @brief   Active ou désactive le polling d'un équipement BACnet entier.
     * @details Bascule le flag `xEnabled` de l'équipement. Un équipement désactivé
     *          n'est plus interrogé sur le bus MS/TP, libérant de la bande passante
     *          pour les autres équipements.
     *
     *          Cas particulier (v6.8.7-patch2) : Si l'on réactive un équipement qui
     *          n'a aucun objet en cache, sa découverte est automatiquement relancée
     *          depuis l'étape initiale.
     *
     * @param   id  (POST) Device ID de l'équipement à basculer.
     */
    webServer.on("/api/toggle_device", HTTP_POST, [](AsyncWebServerRequest *request){
        if(!is_authenticated(request)) return;
        if (request->hasParam("id", true)) {
            uint32_t did = request->getParam("id", true)->value().toInt();
            if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(500))) {
                for (auto& dev : bacnet_network_cache) {
                    if (dev.ulDeviceId == did) {
                        dev.xEnabled = !dev.xEnabled;
                        // v6.8.7-patch2: Si on active un device sans objets, on relance la découverte
                        if (dev.xEnabled && dev.objects.empty()) {
                            dev.xDiscoveryDone = false;
                            dev.ucDiscStep = DISC_DEV_ID;
                            dev.usDiscObjIdx = 0;
                            z_log(pdLOG_INFO, "BACNET", "Manual activation: Restarting discovery for Device %lu\n", (unsigned long)did);
                        }
                        break; 
                    }
                }
                xSemaphoreGive(cache_mutex);
                save_device_objects(did);
                trigger_ha_discovery();
            }
            request->send(200, "text/plain", "OK");
        } else request->send(400, "text/plain", "Missing id");
    });

    // =========================================================================
    // Route API pour forcer l'état OutOfService d'un objet (mode "Hack Clim")
    // =========================================================================
    /**
     * @route   POST /api/outofservice
     * @brief   Force ou relâche le flag Out_Of_Service (propriété 96) d'un objet BACnet.
     * @details Implémente le pattern "Hack Clim" : permet de prendre le contrôle
     *          local d'un capteur (typiquement un Analog Input) en le mettant hors-service
     *          puis en écrivant directement sa Present_Value pour simuler une mesure.
     *
     *          Double action :
     *          1. Mise à jour immédiate du flag `ucStatusFlags` dans le cache local RAM
     *             + publication MQTT pour feedback instantané vers Home Assistant.
     *          2. Envoi d'un WriteProperty(Out_Of_Service) vers l'automate BACnet
     *             distant via la file de jobs (priorité 8 = Manuel).
     *
     *          Accepte les valeurs "1", "true", "on" (insensible à la casse) pour activer,
     *          toute autre valeur pour désactiver.
     *
     * @param   did   (POST) Device ID.
     * @param   inst  (POST) Instance de l'objet.
     * @param   state (POST) État souhaité ("1"/"true"/"on" ou "0"/"false"/"off").
     * @param   type  (POST, opt) Type d'objet (défaut : 0 = Analog Input).
     */
    webServer.on("/api/outofservice", HTTP_POST, [](AsyncWebServerRequest *request){
        if(!is_authenticated(request)) return;
        
        if (request->hasParam("did", true) && request->hasParam("inst", true) && request->hasParam("state", true)) {
            uint32_t ulDid = request->getParam("did", true)->value().toInt();
            uint32_t ulInst = request->getParam("inst", true)->value().toInt();
            
            String state_str = request->getParam("state", true)->value();
            bool xState = (state_str == "1" || state_str.equalsIgnoreCase("true") || state_str.equalsIgnoreCase("on"));
            
            uint16_t usType = request->hasParam("type", true) ? request->getParam("type", true)->value().toInt() : 0; 
            
            bool xDeviceFound = false;
            BACnetJob xJob;
            memset(&xJob, 0, sizeof(BACnetJob)); // Purge la mémoire pour éviter les valeurs parasites
            
            xJob.type = JOB_WRITE_PROP; 
            xJob.obj_type = usType;
            xJob.obj_instance = ulInst;
            xJob.prop_id = 81; // PROP_OUT_OF_SERVICE
            xJob.write_value = xState ? 1.0f : 0.0f;
            xJob.priority = 8; // Priorité 8 (Manuel) par défaut pour le débrayage
            xJob.target_mac = 255;
            
            if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(500))) {
                for (auto& dev : bacnet_network_cache) {
                    if (dev.ulDeviceId == ulDid) {
                        xJob.target_mac = dev.ucMacAddress;
                        xDeviceFound = true;
                        
                        // Local out-of-service emulation fallback
                        for (auto& o : dev.objects) {
                            if (o.usType == usType && o.ulInstance == ulInst) {
                                if (xState) {
                                    o.ucStatusFlags |= BACNET_STATUS_OUT_OF_SERVICE;
                                } else {
                                    o.ucStatusFlags &= ~BACNET_STATUS_OUT_OF_SERVICE;
                                }
                                o.ulLastUpdate = millis();
                                publish_mqtt_topic(dev.ulDeviceId, o, 81, false);
                                break;
                            }
                        }
                        break;
                    }
                }
                xSemaphoreGive(cache_mutex);
            }
            
            if (xDeviceFound) {
                enqueue_bacnet_job(xJob);
                z_log(pdLOG_INFO, "API", "OutOfService %s ENQUEUED for %u:%lu\n", xState ? "ON" : "OFF", usType, (unsigned long)ulInst);
                request->send(200, "text/plain", "OUT_OF_SERVICE ENQUEUED");
            } else {
                request->send(404, "text/plain", "Device not found");
            }
        } else {
            request->send(400, "text/plain", "Missing params (requires: did, inst, state)");
        }
    });

    // =========================================================================
    // Route API pour écrire une valeur générique (Present_Value ou autre)
    // =========================================================================
    /**
     * @route   POST /api/writevalue
     * @brief   Écrit une valeur sur une propriété quelconque d'un objet BACnet.
     * @details Endpoint générique de commande. Crée un WriteProperty BACnet et l'enfile
     *          vers le Core 1 pour transmission sur le bus MS/TP.
     *
     *          Cas particulier d'émulation locale : si l'objet cible est un Analog Input
     *          (type=0) en mode Out-Of-Service et que la propriété est Present_Value (85),
     *          la valeur est directement appliquée dans le cache RAM et publiée via MQTT,
     *          sans attendre la confirmation du bus. Cela garantit un retour instantané
     *          pour le pattern "Hack Clim". La vérification de dépendances HA est aussi
     *          déclenchée car la valeur d'un AI peut servir de min/max dynamique.
     *
     * @param   did      (POST) Device ID.
     * @param   type     (POST) Type d'objet BACnet.
     * @param   inst     (POST) Instance de l'objet.
     * @param   prop     (POST) ID de propriété BACnet (ex: 85=Present_Value).
     * @param   val      (POST) Valeur flottante/chaîne à écrire.
     * @param   priority (POST, opt) Priorité BACnet 1-16 (défaut: 0 = pas de priorité).
     */
    webServer.on("/api/writevalue", HTTP_POST, [](AsyncWebServerRequest *request){
        if(!is_authenticated(request)) return;
        if (request->hasParam("did", true) && request->hasParam("type", true) && 
            request->hasParam("inst", true) && request->hasParam("prop", true) && 
            (request->hasParam("val", true) || request->hasParam("name", true))) {
            
            uint32_t ulDid = request->getParam("did", true)->value().toInt();
            uint16_t usType = request->getParam("type", true)->value().toInt();
            uint32_t ulInst = request->getParam("inst", true)->value().toInt();
            uint8_t ucProp = request->getParam("prop", true)->value().toInt();
            
            float fVal = NAN;
            if (request->hasParam("val", true)) {
                String xValStr = request->getParam("val", true)->value();
                fVal = xValStr.equalsIgnoreCase("AUTO") ? NAN : xValStr.toFloat();
            }
            
            uint8_t ucPriority = request->hasParam("priority", true) ? request->getParam("priority", true)->value().toInt() : 0;
            
            bool xDeviceFound = false;
            BACnetJob xJob;
            memset(&xJob, 0, sizeof(BACnetJob)); // Initialisation propre à 0
            
            xJob.type = JOB_WRITE_PROP; 
            xJob.obj_type = usType;
            xJob.obj_instance = ulInst;
            xJob.prop_id = ucProp;
            xJob.write_value = fVal;
            xJob.priority = ucPriority; // L'API transmet la priorité au moteur BACnet
            xJob.target_mac = 255;
            
            if (ucProp == 77 && request->hasParam("name", true)) {
                strlcpy(xJob.name, request->getParam("name", true)->value().c_str(), sizeof(xJob.name));
            } else if (ucProp == 77 && request->hasParam("val", true)) {
                strlcpy(xJob.name, request->getParam("val", true)->value().c_str(), sizeof(xJob.name));
            }
            
            // Recherche de l'adresse MAC cible
            if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(500))) {
                for (auto& dev : bacnet_network_cache) {
                    if (dev.ulDeviceId == ulDid) {
                        xJob.target_mac = dev.ucMacAddress;
                        xDeviceFound = true;
                        
                        // Local out-of-service write emulation
                        for (auto& o : dev.objects) {
                            if (o.usType == usType && o.ulInstance == ulInst) {
                                if (ucProp == 85 && usType == 0 && o.isOutOfService()) {
                                    o.fPresentValue = fVal;
                                    o.ulLastUpdate = millis();
                                    publish_mqtt_topic(dev.ulDeviceId, o, 85, false);
                                    check_ha_dependencies(dev.ulDeviceId, o.usType, o.ulInstance);
                                    z_log(pdLOG_INFO, "API", "Local AI out-of-service emulation write applied: %.2f\n", fVal);
                                }
                                break;
                            }
                        }
                        break;
                    }
                }
                xSemaphoreGive(cache_mutex);
            }
            
            // Envoi dans la file d'attente
            if (xDeviceFound) {
                enqueue_bacnet_job(xJob);
                z_log(pdLOG_INFO, "API", "WriteValue %.2f ENQUEUED for %u:%lu (Prop: %u, Prio: %u)\n", fVal, usType, (unsigned long)ulInst, ucProp, ucPriority);
                request->send(200, "text/plain", "WRITE_VALUE ENQUEUED");
            } else {
                request->send(404, "text/plain", "Device not found");
            }
        } else {
            request->send(400, "text/plain", "Missing params (requires: did, type, inst, prop, val or name)");
        }
    });

    // =========================================================================
    // Route API pour lire n'importe quelle propriété d'un objet (ReadProperty)
    // =========================================================================
    /**
     * @route   POST /api/readproperty
     * @brief   Enfile une requête de lecture d'une propriété quelconque d'un objet BACnet.
     * @details Endpoint de diagnostic générique. Crée un ReadProperty BACnet pour
     *          n'importe quelle combinaison type/instance/propriété/array_index.
     *          La réponse ne revient pas dans la réponse HTTP (asynchrone) : elle est
     *          traitée par le Core 1 et apparaîtra dans les logs et/ou le cache.
     *
     * @param   did   (POST) Device ID.
     * @param   type  (POST) Type d'objet BACnet.
     * @param   inst  (POST) Instance de l'objet.
     * @param   prop  (POST) ID de propriété BACnet.
     * @param   array (POST, opt) Index de tableau (défaut: -1 = pas de tableau).
     */
    webServer.on("/api/readproperty", HTTP_POST, [](AsyncWebServerRequest *request){
        if(!is_authenticated(request)) return;
        
        if (request->hasParam("did", true) && request->hasParam("type", true) && 
            request->hasParam("inst", true) && request->hasParam("prop", true)) {
            
            uint32_t ulDid = request->getParam("did", true)->value().toInt();
            uint16_t usType = request->getParam("type", true)->value().toInt();
            uint32_t ulInst = request->getParam("inst", true)->value().toInt();
            uint8_t ucProp = request->getParam("prop", true)->value().toInt();
            int32_t lArray = request->hasParam("array", true) ? request->getParam("array", true)->value().toInt() : -1;
            
            bool xDeviceFound = false;
            BACnetJob xJob;
            memset(&xJob, 0, sizeof(BACnetJob)); // Initialisation propre à 0
            
            xJob.type = JOB_READ_PROP; 
            xJob.obj_type = usType;
            xJob.obj_instance = ulInst;
            xJob.prop_id = ucProp;
            xJob.array_index = lArray;
            xJob.target_mac = 255;
            
            // Recherche de l'adresse MAC cible
            if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(500))) {
                for (auto& dev : bacnet_network_cache) {
                    if (dev.ulDeviceId == ulDid) {
                        xJob.target_mac = dev.ucMacAddress;
                        xDeviceFound = true;
                        break;
                    }
                }
                xSemaphoreGive(cache_mutex);
            }
            
            // Envoi dans la file d'attente
            if (xDeviceFound) {
                enqueue_bacnet_job(xJob);
                z_log(pdLOG_INFO, "API", "ReadProperty ENQUEUED for %u:%lu (Prop: %u, Array: %d)\n", usType, (unsigned long)ulInst, ucProp, lArray);
                request->send(200, "text/plain", "READ_PROPERTY ENQUEUED");
            } else {
                request->send(404, "text/plain", "Device not found");
            }
        } else {
            request->send(400, "text/plain", "Missing params (requires: did, type, inst, prop)");
        }
    });

    // -------------------------------------------------------------------------
    // ROUTE API - SAUVEGARDE DE LA CONFIGURATION SYSTÈME (NVS)
    // -------------------------------------------------------------------------

    /**
     * @route   POST /save
     * @brief   Sauvegarde les paramètres système en NVS (Flash SPI).
     * @details Point d'entrée unique pour la sauvegarde de tous les onglets de configuration
     *          de l'interface web. Le paramètre `form_type` détermine la section à traiter :
     *
     *          - **"wifi"** : SSID, mot de passe (ignore "******"), IP statique/DHCP,
     *            adresse IP, passerelle, masque de sous-réseau.
     *          - **"mqtt"** : Serveur, utilisateur, mot de passe, préfixe topic, découverte HA.
     *          - **"bac"**  : MAC BACnet, Device ID, Max Master, retries, timeout APDU,
     *            token skip, max info frames, intervalle heartbeat.
     *          - **"poll"** : Intervalles de polling MQTT et BACnet (en secondes).
     *          - **"sec"**  : Identifiants admin (utilisateur/mot de passe), niveau de log.
     *
     *          Optimisation NVS (v6.9.3) : chaque paramètre est comparé à sa valeur courante
     *          avant modification. La NVS n'est écrite que si au moins un paramètre a réellement
     *          changé, évitant l'usure inutile de la Flash SPI et le gel du bus.
     *
     *          Actions post-sauvegarde :
     *          - WiFi modifié → redémarrage programmé (pending_reboot).
     *          - MQTT modifié → reconnexion MQTT sans redémarrage (setup_mqtt()).
     *          - Autres → la nouvelle config est effective immédiatement en RAM.
     *
     *          Les mots de passe masqués (valeur "******") dans le formulaire sont ignorés
     *          pour éviter d'écraser le mot de passe réel stocké en NVS.
     */
    webServer.on("/save", HTTP_POST, [](AsyncWebServerRequest *request){
        if (!is_authenticated(request)) return;
        String ft = "";
        if (request->hasParam("form_type", true)) ft = request->getParam("form_type", true)->value();

        // Variable traquant si un paramètre a réellement été modifié.
        bool changed = false;
        String ip_str = "unknown";
        if (request->client() != nullptr) {
            ip_str = request->client()->remoteIP().toString();
        }

        if (ft == "wifi") {
            // Comparaison SSID
            String ssid = request->getParam("ssid", true)->value();
            if (strcmp(sysCfg.wifi_ssid, ssid.c_str()) != 0) {
                strlcpy(sysCfg.wifi_ssid, ssid.c_str(), 32);
                changed = true;
            }
            
            // Comparaison Mot de passe (on ignore ****** qui est la valeur factice)
            String p = request->getParam("pass", true)->value();
            if (p.length() > 0 && p != "******") {
                if (strcmp(sysCfg.wifi_pass, p.c_str()) != 0) {
                    strlcpy(sysCfg.wifi_pass, p.c_str(), 64);
                    changed = true;
                }
            }
            
            // Mode IP Statique
            bool static_ip = request->hasParam("static_ip", true);
            if (sysCfg.static_ip != static_ip) {
                sysCfg.static_ip = static_ip;
                changed = true;
            }
            
            // Local IP
            String ip = request->getParam("local_ip", true)->value();
            if (strcmp(sysCfg.local_ip, ip.c_str()) != 0) {
                strlcpy(sysCfg.local_ip, ip.c_str(), 16);
                changed = true;
            }
            
            // Passerelle
            String gw = request->getParam("gateway", true)->value();
            if (strcmp(sysCfg.gateway, gw.c_str()) != 0) {
                strlcpy(sysCfg.gateway, gw.c_str(), 16);
                changed = true;
            }
            
            // Masque de sous-réseau
            String subnet = request->getParam("subnet", true)->value();
            if (strcmp(sysCfg.subnet, subnet.c_str()) != 0) {
                strlcpy(sysCfg.subnet, subnet.c_str(), 16);
                changed = true;
            }
        } else if (ft == "mqtt") {
            // Serveur MQTT
            String server = request->getParam("mqh", true)->value();
            if (strcmp(sysCfg.mqtt_server, server.c_str()) != 0) {
                strlcpy(sysCfg.mqtt_server, server.c_str(), 32);
                changed = true;
            }
            
            // Utilisateur MQTT
            String user = request->getParam("mqu", true)->value();
            if (strcmp(sysCfg.mqtt_user, user.c_str()) != 0) {
                strlcpy(sysCfg.mqtt_user, user.c_str(), 32);
                changed = true;
            }
            
            // Mot de passe MQTT
            String p = request->getParam("mqp", true)->value();
            if (p.length() > 0 && p != "******") {
                if (strcmp(sysCfg.mqtt_pass, p.c_str()) != 0) {
                    strlcpy(sysCfg.mqtt_pass, p.c_str(), 32);
                    changed = true;
                }
            }
            
            // Préfixe MQTT
            String prefix = request->getParam("mqpr", true)->value();
            if (strcmp(sysCfg.mqtt_prefix, prefix.c_str()) != 0) {
                strlcpy(sysCfg.mqtt_prefix, prefix.c_str(), 64);
                changed = true;
            }
            
            // Découverte HA
            bool ha_disc = request->hasParam("ha_disc", true);
            if (sysCfg.ha_discover != ha_disc) {
                sysCfg.ha_discover = ha_disc;
                changed = true;
            }
        } else if (ft == "bac") {
            // Adresse MAC BACnet
            uint8_t mac = (uint8_t)request->getParam("mac", true)->value().toInt();
            if (sysCfg.ucMacAddress != mac) { sysCfg.ucMacAddress = mac; changed = true; }

            // Device ID
            uint32_t did = (uint32_t)request->getParam("did", true)->value().toInt();
            if (sysCfg.ulDeviceId != did) { sysCfg.ulDeviceId = did; changed = true; }

            // Max Master
            uint8_t mm = (uint8_t)request->getParam("mm", true)->value().toInt();
            if (sysCfg.max_master != mm) { sysCfg.max_master = mm; changed = true; }

            // Retries
            uint8_t retries = (uint8_t)request->getParam("retries", true)->value().toInt();
            if (sysCfg.max_retries != retries) { sysCfg.max_retries = retries; changed = true; }

            // Timeout APDU
            uint16_t timeout = (uint16_t)request->getParam("timeout", true)->value().toInt();
            if (sysCfg.ulApduTimeout != timeout) { sysCfg.ulApduTimeout = timeout; changed = true; }

            // Token Skip
            uint8_t tskip = (uint8_t)request->getParam("tskip", true)->value().toInt();
            if (sysCfg.token_skip != tskip) { sysCfg.token_skip = tskip; changed = true; }

            // Max Info Frames
            uint8_t mif = (uint8_t)request->getParam("mif", true)->value().toInt();
            if (sysCfg.max_info_frames != mif) { sysCfg.max_info_frames = mif; changed = true; }

            // Heartbeat Interval
            uint32_t hbeat = (uint32_t)request->getParam("hbeat", true)->value().toInt();
            if (sysCfg.heartbeat_interval != hbeat) { sysCfg.heartbeat_interval = hbeat; changed = true; }
        } else if (ft == "poll") {
            // Intervalles de Polling
            uint16_t mpi = (uint16_t)request->getParam("mpi", true)->value().toInt();
            if (sysCfg.mqtt_poll_interval != mpi) { sysCfg.mqtt_poll_interval = mpi; changed = true; }

            uint16_t bpi = (uint16_t)request->getParam("bpi", true)->value().toInt();
            if (sysCfg.bacnet_poll_interval != bpi) { sysCfg.bacnet_poll_interval = bpi; changed = true; }
        } else if (ft == "sec") {
            // Admin Username
            String admin_u = request->getParam("admin_u", true)->value();
            if (strcmp(sysCfg.admin_user, admin_u.c_str()) != 0) {
                strlcpy(sysCfg.admin_user, admin_u.c_str(), 32);
                changed = true;
            }
            
            // Admin Password
            String p = request->getParam("admin_p", true)->value();
            if (p.length() > 0 && p != "******") {
                if (strcmp(sysCfg.admin_pass, p.c_str()) != 0) {
                    strlcpy(sysCfg.admin_pass, p.c_str(), 64);
                    changed = true;
                }
            }
            
            // Log Level
            if (request->hasParam("lvl", true)) {
                uint8_t lvl = (uint8_t)request->getParam("lvl", true)->value().toInt();
                if (sysCfg.log_level != lvl) {
                    sysCfg.log_level = lvl;
                    changed = true;
                }
            }
        }

        // v6.9.3: On n'écrit la Flash SPI que si un paramètre a été modifié
        if (changed) {
            z_log(pdLOG_INFO, "WEB", "POST /save from %s: configuration modified (type: %s), saving NVS\n", ip_str.c_str(), ft.c_str());
            save_configuration();
        } else {
            z_log(pdLOG_INFO, "WEB", "POST /save from %s: configuration unchanged (type: %s), skipping NVS save\n", ip_str.c_str(), ft.c_str());
        }

        request->send(200, "text/plain", "OK");

        // Action post-sauvegarde si modification effective
        // Certaines modifications exigent un redémarrage, d'autres non (ex: relancer la tâche MQTT).
        if (changed) {
            if (ft == "wifi") {
                pending_reboot = true; reboot_timer = millis();
            } else if (ft == "mqtt") {
                setup_mqtt();
            }
        }
    });

    // -------------------------------------------------------------------------
    // ROUTES API - MAINTENANCE ET RÉINITIALISATION
    // -------------------------------------------------------------------------

    /**
     * @route   ANY /api/reset_cache
     * @brief   Efface le cache NVS de tous les équipements BACnet et redémarre.
     * @details Parcourt chaque équipement dans `bacnet_network_cache`, ouvre son
     *          namespace NVS dédié ("dev_<device_id>") et le purge. Efface également
     *          le registre global ("registry") qui liste les Device IDs connus.
     *          Vide ensuite le vecteur en RAM et programme un redémarrage.
     *
     *          Utilise HTTP_ANY pour accepter GET et POST (commodité pour les scripts curl).
     */
    webServer.on("/api/reset_cache", HTTP_ANY, [](AsyncWebServerRequest *request){
        if (!is_authenticated(request)) return;
        if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(1000))) {
            for (auto& dev : bacnet_network_cache) {
                char ns[16]; snprintf(ns, sizeof(ns), "dev_%lu", (unsigned long)dev.ulDeviceId);
                Preferences p; p.begin(ns, false); p.clear(); p.end();
            }
            Preferences reg; reg.begin("registry", false); reg.clear(); reg.end();
            bacnet_network_cache.clear();
            xSemaphoreGive(cache_mutex);
        }
        request->send(200, "text/plain", "Cache cleared. Rebooting...");
        pending_reboot = true; reboot_timer = millis();
    });

    /**
     * @route   ANY /api/factory_reset
     * @brief   Effectue un Factory Reset complet (effacement total de la partition NVS).
     * @details Appelle `nvs_flash_erase()` qui efface TOUTE la partition NVS, incluant
     *          la configuration système, les équipements, et les calibrations.
     *          Après redémarrage, le système repasse en mode AP avec les valeurs par défaut.
     *
     * @warning Cette opération est irréversible. Tous les paramètres et données
     *          persistantes seront perdus.
     */
    webServer.on("/api/factory_reset", HTTP_ANY, [](AsyncWebServerRequest *request){
        if (!is_authenticated(request)) return;
        nvs_flash_erase();
        request->send(200, "text/plain", "Factory reset done. Rebooting...");
        pending_reboot = true; reboot_timer = millis();
    });

    /**
     * @route   ANY /api/reboot
     * @brief   Redémarre la passerelle proprement.
     * @details Programme un redémarrage différé d'environ 1 seconde (via `pending_reboot`
     *          et `reboot_timer`). Le délai permet à la réponse HTTP d'être envoyée
     *          au client avant le reset matériel.
     */
    webServer.on("/api/reboot", HTTP_ANY, [](AsyncWebServerRequest *request){
        if (!is_authenticated(request)) return;
        request->send(200, "text/plain", "Rebooting...");
        pending_reboot = true; reboot_timer = millis();
    });


    /**
     * @route   GET /api/trigger_discovery
     * @brief   Force une redécouverte complète de tous les équipements et leurs objets.
     * @details Route de débogage qui :
     *          1. Remet tous les équipements en état "non-découvert" (DISC_DEV_ID, index 0).
     *          2. Efface les flags de publication des noms et les textes d'unité pour
     *             forcer leur re-lecture depuis les automates BACnet.
     *          3. Déclenche la re-publication de la découverte Home Assistant.
     *
     *          Utile après un changement de configuration sur les automates physiques
     *          ou pour résoudre des incohérences de cache.
     */
    webServer.on("/api/trigger_discovery", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!is_authenticated(request)) return;
        
        if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(1000))) {
            z_log(pdLOG_INFO, "WEB", "Manual Discovery Triggered - Clearing Metadata Cache\n");
            for (auto& dev : bacnet_network_cache) {
                dev.xDiscoveryDone = false;
                dev.ucDiscStep = DISC_DEV_ID;
                dev.usDiscObjIdx = 0;
                for (auto& obj : dev.objects) {
                    obj.xNamePublished = false;
                    memset(obj.cUnitText, 0, sizeof(obj.cUnitText));
                }
            }
            xSemaphoreGive(cache_mutex);
        }

        trigger_ha_discovery();
        request->send(200, "text/plain", "Discovery triggered & cache cleared");
    });
}

// ============================================================================
// Initialisation de l'infrastructure réseau
// ============================================================================

/**
 * @brief Initialise toute l'infrastructure réseau de la passerelle.
 * @details Fonction d'initialisation appelée une seule fois depuis `setup()` dans le .ino principal.
 *          Séquence d'initialisation :
 *
 *          1. **Mutex** : Création des 3 sémaphores (cache, API, WS) en premier pour
 *             éviter les conditions de course lors du chargement NVS.
 *
 *          2. **File de logs** : Création de la queue FreeRTOS (20 messages × 256 octets)
 *             et lancement de la tâche `websocket_log_task` sur le Core 0 (8 Ko de pile,
 *             priorité 2). La tâche n'est créée qu'une seule fois grâce à un flag statique.
 *
 *          3. **Configuration NVS** : Chargement de sysCfg depuis la Flash SPI.
 *
 *          4. **WiFi** :
 *             - Si les identifiants sont vides → Mode AP ("ZIRCON-GW-CONFIG" / "admin1234").
 *             - Sinon → Mode STA avec désactivation du Power Saving WiFi (WIFI_PS_NONE)
 *               pour garantir la latence réseau, et configuration IP statique si activée.
 *
 *          5. **Routes web** : Enregistrement de toutes les routes via `setup_web_routes()`.
 *
 *          6. **OTA** : Configuration des callbacks ArduinoOTA (Start, End, Error).
 *
 *          7. **Démarrage des services** : webServer.begin() et ArduinoOTA.begin().
 *
 * @note    La désactivation du Power Saving WiFi (WIFI_PS_NONE) est critique pour
 *          la réactivité des WebSockets et de l'API REST.
 */
void setup_network_infrastructure() {
    // 1. Initialize core system mutexes first to avoid race conditions during NVS load
    if (cache_mutex == NULL) {
        cache_mutex = xSemaphoreCreateMutex();
    }
    if (api_mutex == NULL) {
        api_mutex = xSemaphoreCreateMutex();
    }
    if (ws_mutex == NULL) {
        ws_mutex = xSemaphoreCreateMutex();
    }

    log_queue = xQueueCreate(20, sizeof(LogMessage));
    // La tâche est créée une seule fois pour éviter les duplications lors des reconnexions.
    static bool log_task_created = false;
    if (log_queue != NULL && !log_task_created) {
        xTaskCreatePinnedToCore(websocket_log_task, "WS_Log", 8192, NULL, 2, NULL, 0);
        log_task_created = true;
    }
    // 2. Load Settings (now safe because cache_mutex exists)
    load_configuration();

    WiFi.persistent(false); WiFi.disconnect(true);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Démarrage en mode Point d'Accès si les identifiants WiFi sont incomplets.
    if (strlen(sysCfg.wifi_ssid) == 0 || strlen(sysCfg.wifi_pass) == 0) {
        z_log(pdLOG_INFO, "WIFI", "Credentials missing. Starting Access Point: ZIRCON-GW-CONFIG\n");
        is_ap_mode = true; WiFi.mode(WIFI_AP);
        WiFi.softAP("ZIRCON-GW-CONFIG", "admin1234");
    } else {
        is_ap_mode = false; WiFi.mode(WIFI_STA);
        esp_wifi_set_ps(WIFI_PS_NONE);
        if (sysCfg.static_ip) {
            IPAddress ip, gw, sn;
            if (ip.fromString(sysCfg.local_ip) && gw.fromString(sysCfg.gateway) && sn.fromString(sysCfg.subnet)) {
                WiFi.config(ip, gw, sn);
            }
        }
            WiFi.begin(sysCfg.wifi_ssid, sysCfg.wifi_pass);
        wifi_connect_start = millis();
    }

    setup_web_routes();

    // Configuration des callbacks pour les mises à jour OTA (Over-The-Air).
    ArduinoOTA.onStart([]() { z_log(pdLOG_INFO, "OTA", "Start\n"); });
    ArduinoOTA.onEnd([]() { z_log(pdLOG_INFO, "OTA", "End\n"); });
    ArduinoOTA.onError([](ota_error_t error) { z_log(pdLOG_ERROR,"OTA", "Error[%u]\n", error); });
    ArduinoOTA.begin();

    webServer.begin();
    // Log déplacé vers system_task (Core 0) pour cohérence visuelle
}

// ============================================================================
// Boucle réseau récurrente (appelée depuis la boucle principale du Core 0)
// ============================================================================

/**
 * @brief Gère les tâches réseau récurrentes.
 * @details Appelé dans la boucle principale du Cœur 0. Responsabilités :
 *
 *          1. **OTA** : Traitement des paquets de mise à jour firmware en attente.
 *
 *          2. **Redémarrage différé** : Si `pending_reboot` est vrai et que 1 seconde
 *             s'est écoulée depuis l'armement du timer, exécute `ESP.restart()`.
 *             Ce délai d'une seconde garantit que la réponse HTTP a eu le temps
 *             d'être envoyée au client avant le reset matériel.
 *
 *          3. **Fallback AP** : Si le WiFi STA n'est pas connecté après 30 secondes,
 *             bascule automatiquement en mode Point d'Accès pour permettre la
 *             reconfiguration via le portail captif ("ZIRCON-GW-CONFIG").
 *             Le flag `wifi_fallback_active` empêche de relancer le softAP en boucle.
 */
void handle_network() {
    ArduinoOTA.handle();
    if (pending_reboot && (millis() - reboot_timer > 1000)) ESP.restart();

    // Si la connexion WiFi échoue pendant plus de 30 secondes, on passe en mode AP.
    if (!is_ap_mode && WiFi.status() != WL_CONNECTED && (millis() - wifi_connect_start > 30000)) {
        if (!wifi_fallback_active) {
            z_log(pdLOG_ERROR, "WIFI", "WiFi Connection failed. Fallback to AP Mode.\n");
            WiFi.mode(WIFI_AP);
            WiFi.softAP("ZIRCON-GW-CONFIG", "admin1234");
            wifi_fallback_active = true;
        }
    }
}

// ============================================================================
// Authentification HTTP Basic
// ============================================================================

/**
 * @brief Vérifie si une requête web est authentifiée via HTTP Basic Auth.
 * @details Compare les identifiants fournis dans l'en-tête Authorization de la requête
 *          avec les valeurs stockées dans `sysCfg.admin_user` et `sysCfg.admin_pass`.
 *          Si l'authentification échoue, envoie automatiquement une réponse 401
 *          (Unauthorized) avec l'en-tête WWW-Authenticate pour déclencher la boîte
 *          de dialogue de connexion du navigateur.
 *
 * @param  request Pointeur vers l'objet de la requête web asynchrone.
 * @return `true` si l'utilisateur est authentifié et la route peut continuer.
 * @return `false` si l'authentification a échoué (la réponse 401 est déjà envoyée).
 */
bool is_authenticated(AsyncWebServerRequest *request) {
    if (!request->authenticate(sysCfg.admin_user, sysCfg.admin_pass)) {
        request->requestAuthentication();
        return false;
    }
    return true;
}
