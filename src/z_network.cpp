/**
 * @file z_network.cpp
 * @brief Implémentation de l'infrastructure réseau, du serveur web et du système de journalisation.
 * 
 * @details Ce module gère la connexion WiFi (STA avec fallback AP), le serveur web asynchrone,
 *          les API REST, les mises à jour OTA, et un système de logging asynchrone et thread-safe
 *          via WebSockets. Il s'exécute principalement sur le Cœur 0.
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

// File d'attente et sémaphores pour la communication inter-tâches et la protection des ressources.
QueueHandle_t log_queue = NULL;
SemaphoreHandle_t api_mutex = NULL;
SemaphoreHandle_t ws_mutex = NULL;
PSRAM_Allocator psram_alloc;

// Variables statiques pour la gestion de la session WebSocket.
static uint32_t last_ws_event = 0;
static uint32_t active_ws_id = 0;

/**
 * @brief Callback pour les événements du serveur WebSocket.
 * @details Gère la connexion et la déconnexion des clients. Mémorise l'ID du client actif
 *          pour ne diffuser les logs qu'à une seule session, évitant les crashs lors de rafraîchissements rapides.
 * @param server Pointeur vers le serveur WebSocket.
 * @param client Pointeur vers le client WebSocket concerné.
 * @param type Type d'événement (connexion, déconnexion, données, etc.).
 * @param arg Données d'argument (non utilisé).
 * @param data Pointeur vers les données reçues (non utilisé).
 * @param len Longueur des données reçues (non utilisé).
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
 * @param pvParameters Paramètres de la tâche (non utilisés).
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

// Safe logging function that adds messages to the queue without blocking the program
/**
 * @brief Système de journalisation unifié, thread-safe et multi-destination.
 * @details Formate un message et l'envoie à la fois sur le port Série (UART) et
 *          vers une file d'attente pour une transmission asynchrone via WebSocket.
 *          Filtre les messages en fonction du niveau de log global (`sysCfg.log_level`).
 * @param level Niveau de sévérité du log (pdLOG_ERROR, pdLOG_WARN, pdLOG_INFO, pdLOG_DEBUG).
 * @param tag Un court identifiant de module (ex: "WIFI", "MQTT", "BACNET").
 * @param format Chaîne de formatage de type `printf`.
 * @param ... Arguments variables pour la chaîne de formatage.
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


/**
 * @brief Configure toutes les routes (endpoints) du serveur web asynchrone.
 */
void setup_web_routes() {
    ws.onEvent(onEvent); // v6.5.2: Callback obligatoire pour la thread-safety
    webServer.addHandler(&ws);
    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!is_authenticated(request)) return;
        request->send_P(200, "text/html", reinterpret_cast<const uint8_t*>(INDEX_HTML), sizeof(INDEX_HTML));
    });

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

    // Route API pour obtenir l'état complet du système en JSON.
    webServer.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!is_authenticated(request)) return;

        // v6.4.8: Strict Heap Protection
        if (ESP.getFreeHeap() < 50000) {
            request->send(503, "text/plain", "Service Unavailable: Low Memory");
            return;
        }

        // v6.4.8: API Serialization
        if (xSemaphoreTake(api_mutex, pdMS_TO_TICKS(1000))) {
            AsyncResponseStream *stream = request->beginResponseStream("application/json");
            if (!stream) { xSemaphoreGive(api_mutex); return; }
            stream->addHeader("Connection", "close");

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
                    d["xEnabled"] = dev.xEnabled;
                    d["done"] = dev.xDiscoveryDone;
                    
                    int sel = 0;
                    for(auto& o : dev.objects) if(o.xEnabled) sel++;
                    d["sel"] = sel;
                }
                xSemaphoreGive(cache_mutex);
            }

            serializeJson(doc, *stream);
            request->send(stream);
            xSemaphoreGive(api_mutex);
        } else {
            request->send(503, "text/plain", "Service Unavailable: API Busy");
        }
    });

    // Route API pour obtenir la liste des objets BACnet découverts.
    webServer.on("/api/objects", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!is_authenticated(request)) return;

        // v6.4.8: Strict Heap Protection
        if (ESP.getFreeHeap() < 50000) {
            request->send(503, "text/plain", "Service Unavailable: Low Memory");
            return;
        }

        // v6.4.8: API Serialization
        if (xSemaphoreTake(api_mutex, pdMS_TO_TICKS(1000))) {
            AsyncResponseStream *stream = request->beginResponseStream("application/json");
            if (!stream) { xSemaphoreGive(api_mutex); return; }
            stream->addHeader("Connection", "close");

            // v6.4.9: Force PSRAM allocation
            JsonDocument doc(&psram_alloc); 
            JsonArray controllers = doc.to<JsonArray>();
            
            if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(100))) {
                for (auto& dev : bacnet_network_cache) {
                    JsonObject c = controllers.add<JsonObject>();
                    c["ulDeviceId"] = dev.ulDeviceId;
                    c["name"] = dev.name; c["cVendor"] = dev.vendor; c["xEnabled"] = dev.xEnabled;
                    JsonArray objs_arr = c["objects"].to<JsonArray>();
                    for (auto& o : dev.objects) {
                        JsonObject obj = objs_arr.add<JsonObject>();
                        obj["type"] = o.usType; obj["inst"] = o.ulInstance; obj["name"] = o.cName;
                        obj["val"] = o.fPresentValue; obj["poll"] = o.xEnabled;
                        obj["unit"] = o.cUnitText;
                    }
                }
                xSemaphoreGive(cache_mutex);
            }
            serializeJson(doc, *stream);
            request->send(stream);
            xSemaphoreGive(api_mutex);
        } else {
            request->send(503, "text/plain", "Service Unavailable: API Busy");
        }
    });

    // Route API pour déclencher une requête de découverte globale "Who-Is".
    webServer.on("/api/whois", HTTP_POST, [](AsyncWebServerRequest *request){
        if(!is_authenticated(request)) return;
        BACnetJob job; job.type = JOB_WHO_IS;
        enqueue_bacnet_job(job);
        request->send(200, "text/plain", "WHO-IS ENQUEUED");
    });

    // Route API pour forcer l'envoi d'une réponse "I-Am".
    webServer.on("/api/iam", HTTP_POST, [](AsyncWebServerRequest *request){
        if(!is_authenticated(request)) return;
        BACnetJob job; job.type = JOB_I_AM;
        enqueue_bacnet_job(job);
        request->send(200, "text/plain", "I-AM ENQUEUED");
        z_log(pdLOG_INFO, "API", "I-AM ENQUEUED\n");
    });


    // Route API pour recharger tous les objets d'un appareil.
    // Route API pour recharger tous les objets d'un appareil.
    webServer.on("/api/reload_device", HTTP_POST, [](AsyncWebServerRequest *request){
        if(!is_authenticated(request)) return;
        if (request->hasParam("id", true)) {
            uint32_t did = request->getParam("id", true)->value().toInt();
            if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(500))) {
                for (auto& dev : bacnet_network_cache) {
                    if (dev.ulDeviceId == did) {
                        dev.xDiscoveryDone = false;
                        dev.ucDiscStep = DISC_DEV_ID;
                        dev.objects.clear();
                        z_log(pdLOG_INFO, "API", "Reloading device %lu\n", (unsigned long)did);
                        break;
                    }
                }
                xSemaphoreGive(cache_mutex);
            }
            request->send(200, "text/plain", "RELOADING");
        } else request->send(400, "text/plain", "Missing id");
    });

    // Route API pour forcer la lecture d'une propriété d'un objet.
    webServer.on("/api/reload_object", HTTP_POST, [](AsyncWebServerRequest *request){
        if(!is_authenticated(request)) return;
        if (request->hasParam("did", true) && request->hasParam("inst", true) && request->hasParam("type", true)) {
            uint32_t did = request->getParam("did", true)->value().toInt();
            uint32_t inst = request->getParam("inst", true)->value().toInt();
            uint16_t type = request->getParam("type", true)->value().toInt();
            
            BACnetJob job;
            job.type = JOB_WRITE_PROP; // On utilise WRITE_PROP avec une valeur bidon ou on ajoute JOB_READ_PROP dans z_bacnet.h si besoin. 
                                       // NOTE: z_bacnet.h n'avait que WHO_IS, I_AM, WRITE_PROP.
            job.obj_type = type;
            job.obj_instance = inst;
            job.prop_id = 85; // Present_Value
            
            if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(500))) {
                for (auto& dev : bacnet_network_cache) {
                    if (dev.ulDeviceId == did) {
                        job.target_mac = dev.ucMacAddress;
                        break;
                    }
                }
                xSemaphoreGive(cache_mutex);
            }
            enqueue_bacnet_job(job);
            request->send(200, "text/plain", "READ ENQUEUED");
        } else request->send(400, "text/plain", "Missing params");
    });

    // Route API pour sauvegarder les modifications d'un objet (nom, unités, polling).
    webServer.on("/api/save_object", HTTP_POST, [](AsyncWebServerRequest *request){
        if(!is_authenticated(request)) return;
        if (request->hasParam("did", true) && request->hasParam("inst", true) && request->hasParam("type", true)) {
            uint32_t did = request->getParam("did", true)->value().toInt();
            uint32_t inst = request->getParam("inst", true)->value().toInt();
            uint16_t type = request->getParam("type", true)->value().toInt();
            
            String name = request->hasParam("name", true) ? request->getParam("name", true)->value() : "";
            String unit = request->hasParam("unit", true) ? request->getParam("unit", true)->value() : "";
            bool poll = request->hasParam("poll", true) ? (request->getParam("poll", true)->value() == "1") : true;

            if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(500))) {
                for (auto& dev : bacnet_network_cache) {
                    if (dev.ulDeviceId == did) {
                        for (auto& obj : dev.objects) {
                            if (obj.ulInstance == inst && obj.usType == type) {
                                if (name.length() > 0) strlcpy(obj.cName, name.c_str(), sizeof(obj.cName));
                                if (unit.length() > 0) strlcpy(obj.cUnitText, unit.c_str(), sizeof(obj.cUnitText));
                                obj.xEnabled = poll;
                                break;
                            }
                        }
                        break;
                    }
                }
                xSemaphoreGive(cache_mutex); 
            }
            save_device_objects(did);
            trigger_ha_discovery(did, inst, type);
            request->send(200, "text/plain", "OK");
        } else request->send(400, "text/plain", "Missing params");
    });

    // Route API pour activer/désactiver un appareil complet.
    webServer.on("/api/toggle_device", HTTP_POST, [](AsyncWebServerRequest *request){
        if(!is_authenticated(request)) return;
        if (request->hasParam("id", true)) {
            uint32_t did = request->getParam("id", true)->value().toInt();
            if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(500))) {
                for (auto& dev : bacnet_network_cache) {
                    if (dev.ulDeviceId == did) {
                        dev.xEnabled = !dev.xEnabled;
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

    // Route API principale pour la sauvegarde des paramètres système (NVS).
    webServer.on("/save", HTTP_POST, [](AsyncWebServerRequest *request){
        if (!is_authenticated(request)) return;
        String ft = "";
        if (request->hasParam("form_type", true)) ft = request->getParam("form_type", true)->value();

        if (ft == "wifi") {
            strlcpy(sysCfg.wifi_ssid, request->getParam("ssid", true)->value().c_str(), 32);
            String p = request->getParam("pass", true)->value();
            if (p.length() > 0 && p != "******") strlcpy(sysCfg.wifi_pass, p.c_str(), 64);
            
            sysCfg.static_ip = request->hasParam("static_ip", true);
            strlcpy(sysCfg.local_ip, request->getParam("local_ip", true)->value().c_str(), 16);
            strlcpy(sysCfg.gateway, request->getParam("gateway", true)->value().c_str(), 16);
            strlcpy(sysCfg.subnet, request->getParam("subnet", true)->value().c_str(), 16);
        } else if (ft == "mqtt") {
            strlcpy(sysCfg.mqtt_server, request->getParam("mqh", true)->value().c_str(), 32);
            strlcpy(sysCfg.mqtt_user, request->getParam("mqu", true)->value().c_str(), 32);
            String p = request->getParam("mqp", true)->value();
            if (p.length() > 0 && p != "******") strlcpy(sysCfg.mqtt_pass, p.c_str(), 32);
            
            strlcpy(sysCfg.mqtt_prefix, request->getParam("mqpr", true)->value().c_str(), 64);
            sysCfg.ha_discover = request->hasParam("ha_disc", true);
        } else if (ft == "bac") {
            sysCfg.ucMacAddress = (uint8_t)request->getParam("mac", true)->value().toInt();
            sysCfg.ulDeviceId = (uint32_t)request->getParam("did", true)->value().toInt();
            sysCfg.max_master = (uint8_t)request->getParam("mm", true)->value().toInt();
            sysCfg.max_retries = (uint8_t)request->getParam("retries", true)->value().toInt();
            sysCfg.ulApduTimeout = (uint16_t)request->getParam("timeout", true)->value().toInt();
            sysCfg.token_skip = (uint8_t)request->getParam("tskip", true)->value().toInt();
            sysCfg.max_info_frames = (uint8_t)request->getParam("mif", true)->value().toInt();
            sysCfg.heartbeat_interval = (uint32_t)request->getParam("hbeat", true)->value().toInt();
        } else if (ft == "poll") {
            sysCfg.mqtt_poll_interval = (uint16_t)request->getParam("mpi", true)->value().toInt();
            sysCfg.bacnet_poll_interval = (uint16_t)request->getParam("bpi", true)->value().toInt();
        } else if (ft == "sec") {
            strlcpy(sysCfg.admin_user, request->getParam("admin_u", true)->value().c_str(), 32);
            String p = request->getParam("admin_p", true)->value();
            if (p.length() > 0 && p != "******") strlcpy(sysCfg.admin_pass, p.c_str(), 64);
            
            if (request->hasParam("lvl", true)) sysCfg.log_level = (uint8_t)request->getParam("lvl", true)->value().toInt();
        }

        save_configuration();
        request->send(200, "text/plain", "OK");
        
        if (ft == "wifi") {
            pending_reboot = true; reboot_timer = millis();
        } else if (ft == "mqtt") {
            setup_mqtt();
        }
    });

    // API pour réinitialiser le cache BACnet (suppression des fichiers Preferences).
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

    // API pour un Factory Reset complet (effacement total de la NVS).
    webServer.on("/api/factory_reset", HTTP_ANY, [](AsyncWebServerRequest *request){
        if (!is_authenticated(request)) return;
        nvs_flash_erase();
        request->send(200, "text/plain", "Factory reset done. Rebooting...");
        pending_reboot = true; reboot_timer = millis();
    });

    // API pour redémarrer la Gateway.
    webServer.on("/api/reboot", HTTP_ANY, [](AsyncWebServerRequest *request){
        if (!is_authenticated(request)) return;
        request->send(200, "text/plain", "Rebooting...");
        pending_reboot = true; reboot_timer = millis();
    });


    // Route API de débogage pour forcer une redécouverte HA.
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

/**
 * @brief Initialise toute l'infrastructure réseau.
 * @details Charge la configuration NVS, démarre le WiFi (STA ou AP), configure le serveur web,
 *          les WebSockets, les points d'API REST, et lance la tâche de journalisation.
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


/**
 * @brief Gère les tâches réseau récurrentes.
 * @details Appelé dans la boucle principale du Cœur 0, cette fonction gère les mises à jour OTA,
 *          les redémarrages différés et le mécanisme de fallback en mode Point d'Accès.
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

/**
 * @brief Vérifie si une requête web est authentifiée.
 * @details Utilise l'authentification HTTP Basic. Si l'utilisateur n'est pas authentifié,
 *          lui envoie une demande d'authentification (code 401).
 * @param request Pointeur vers l'objet de la requête web asynchrone.
 * @return `true` si l'utilisateur est authentifié, `false` sinon.
 */
bool is_authenticated(AsyncWebServerRequest *request) {
    if (!request->authenticate(sysCfg.admin_user, sysCfg.admin_pass)) {
        request->requestAuthentication();
        return false;
    }
    return true;
}
