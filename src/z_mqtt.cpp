/**
 * @file z_mqtt.cpp
 * @brief Module MQTT complet : connexion broker, Gatekeeper, publication BACnet,
 *        Auto-Discovery Home Assistant, réception des commandes /set et Circuit Breaker.
 *
 * ## Rôle
 * Ce fichier implémente toute la couche MQTT de la passerelle BACnet2MQTT :
 * - Connexion/reconnexion au broker MQTT via ESP-IDF mqtt_client (TCP, QoS 1)
 * - Pattern Circuit Breaker : après 3 échecs consécutifs, arrêt total des tentatives
 *   pour éviter de surcharger le broker ou le réseau
 * - Tâche Gatekeeper FreeRTOS dédiée sur Core 0 (priorité 5) qui consomme la queue
 *   de publication, publie les statuts Gateway et orchestre la HA Discovery
 * - Auto-Discovery Home Assistant conforme (payloads JSON retained sur homeassistant/...)
 * - Réception des commandes /set et /outofservice/set pour piloter les écritures BACnet
 * - Publication LWT (Last Will and Testament) pour signaler online/offline à HA
 * - Lazy Save NVS : la sauvegarde des devices modifiés est différée et exécutée
 *   sur Core 0 pour ne pas bloquer la FSM MS/TP temps réel sur Core 1
 *
 * ## Architecture
 * - Le handler d'événements mqtt_event_handler() s'exécute dans le contexte de la
 *   tâche ESP-MQTT (Core 0). Il ne fait que positionner des flags atomiques et traiter
 *   les messages reçus (MQTT_EVENT_DATA).
 * - La tâche mqtt_gatekeeper_task() boucle toutes les 10ms et assure :
 *   1. Les logs différés (flags atomiques → z_log)
 *   2. La HA Discovery (coalescée pour éviter les rafales)
 *   3. La consommation de la queue de publication (MQTTPublishJob)
 *   4. La télémétrie périodique de la Gateway (version, RSSI, heap, uptime, etc.)
 *   5. Le Lazy Save NVS des devices dirty
 * - Les producteurs (BACnet polling, API web) enfilent des MQTTPublishJob via
 *   enqueue_mqtt_publish() depuis n'importe quel core.
 *
 * ## Dépendances
 * - z_mqtt.h   : Déclaration de MQTTPublishJob et prototypes publics
 * - z_config.h : Structure Config (sysCfg), handle mqtt_client, constantes globales
 * - z_bacnet.h : Structures BACnetDevice/BACnetObject, cache réseau, mutex, énumérations
 * - z_network.h : z_log() (logging centralisé vers WebSocket + Serial)
 * - z_nvs.h    : save_device_objects_locked() (persistance NVS paginée)
 * - ArduinoJson : Sérialisation des payloads HA Discovery
 *
 * ## Hiérarchie des topics MQTT
 * ```
 * {prefix}/{deviceId}/{typeAbbr}/{objInstance}/state     ← valeurs BACnet
 * {prefix}/{deviceId}/{typeAbbr}/{objInstance}/name      ← noms d'objets (retained)
 * {prefix}/{deviceId}/{typeAbbr}/{objInstance}/set       ← commandes écriture
 * {prefix}/{deviceId}/{typeAbbr}/{objInstance}/outofservice       ← état OoS
 * {prefix}/{deviceId}/{typeAbbr}/{objInstance}/outofservice/set   ← commande OoS
 * {prefix}/B2M/{key}/state                              ← télémétrie Gateway
 * tele/{prefix}/LWT                                     ← Last Will and Testament
 * homeassistant/{component}/{uniq_id}/config             ← HA Auto-Discovery
 * ```
 *
 * @note Matériel cible : Waveshare ESP32-S3-RS485-CAN (8MB Flash, 8MB OPI PSRAM)
 * @note La tâche Gatekeeper utilise 16KB de pile pour supporter les payloads JSON
 *       volumineux de la HA Discovery (sérialisation ArduinoJson sur la pile).
 */
#include "z_mqtt.h"
#include "z_bacnet.h"
#include <string.h>
#include <atomic>
#include <ArduinoJson.h>

#include "z_network.h"
#include "z_nvs.h"

/* ─────────────────────────────────────────────────────────────────────────────
 *  COMPTEURS DE TÉLÉMÉTRIE
 *  Réinitialisés à chaque cycle de publication périodique (mqtt_poll_interval).
 *  Permettent d'afficher dans les logs le débit de messages par période.
 * ───────────────────────────────────────────────────────────────────────────── */
uint32_t period_mqtt_pub_count = 0;     ///< Total messages publiés toutes catégories
uint32_t period_mqtt_obj_count = 0;     ///< Messages de valeurs BACnet (state/name)
uint32_t period_mqtt_b2m_count = 0;     ///< Messages de télémétrie Gateway (B2M)
uint32_t period_mqtt_tele_count = 0;    ///< Messages de télémétrie système (LWT, etc.)

/* ─────────────────────────────────────────────────────────────────────────────
 *  CIRCUIT BREAKER ET FLAGS D'ÉTAT
 *  Le Circuit Breaker protège le système contre les boucles de reconnexion
 *  infinies (broker injoignable, credentials invalides).
 *  Après 3 échecs consécutifs, le client MQTT est détruit et aucune
 *  tentative n'est faite jusqu'au prochain redémarrage ou reconfiguration.
 *
 *  Les flags atomiques pending_*_log permettent de déporter les appels z_log()
 *  hors du contexte du handler d'événements ESP-MQTT (qui tourne dans une
 *  tâche système à pile réduite). Le Gatekeeper les consomme à chaque cycle.
 * ───────────────────────────────────────────────────────────────────────────── */
static int mqtt_fail_count = 0;                              ///< Compteur d'échecs consécutifs (0..3)
static std::atomic<bool> circuit_breaker_active{false};      ///< Vrai si le disjoncteur est déclenché
static std::atomic<bool> pending_discovery{false};           ///< Demande de HA Discovery en attente
static std::atomic<uint32_t> target_did{0};                  ///< Device ID cible pour discovery (0 = global)
static std::atomic<uint32_t> target_inst{0};                 ///< Instance cible (0xFFFFFFFF = toutes)
static std::atomic<uint16_t> target_type{0xFFFF};            ///< Type cible (0xFFFF = tous)
static bool mqtt_is_connected = false;                       ///< État de connexion au broker
static bool force_full_discovery = false;                    ///< Force une discovery complète à la reconnexion
static std::atomic<bool> mqtt_pending_connected_log{false};     ///< Flag différé : log "Connected"
static std::atomic<bool> mqtt_pending_disconnected_log{false};  ///< Flag différé : log "Disconnected"
static std::atomic<bool> mqtt_pending_auth_error_log{false};    ///< Flag différé : log "Auth Error"
static std::atomic<bool> mqtt_pending_breaker_log{false};       ///< Flag différé : log "Circuit Breaker"
static std::atomic<bool> mqtt_pending_wifi_loss_log{false};     ///< Flag différé : log "WiFi Loss"
static char lwt_topic[128] = {0};   ///< Topic LWT construit au setup (tele/{prefix}/LWT)

/* ═══════════════════════════════════════════════════════════════════════════════
 *  HANDLER D'ÉVÉNEMENTS MQTT (ESP-IDF)
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Callback d'événements du client MQTT ESP-IDF.
 *
 * Exécuté dans le contexte de la tâche interne esp-mqtt (Core 0, pile ~4KB).
 * Pour cette raison, toute opération lourde (log, JSON, NVS) est interdite ici.
 * Seuls des flags atomiques sont positionnés pour traitement différé par le Gatekeeper.
 *
 * Gère trois types d'événements :
 * - MQTT_EVENT_CONNECTED : souscription aux topics /set et /outofservice/set,
 *   publication du statut "online" sur le topic LWT, déclenchement de la
 *   HA Discovery complète.
 * - MQTT_EVENT_DISCONNECTED / MQTT_EVENT_ERROR : incrémentation du compteur
 *   d'échecs, détection des erreurs d'authentification (déclenchement immédiat
 *   du Circuit Breaker), gestion de la perte WiFi.
 * - MQTT_EVENT_DATA : parsing des messages reçus sur les topics /set et
 *   /outofservice/set pour créer des BACnetJob d'écriture.
 *
 * @param handler_args  Non utilisé (NULL).
 * @param base          Base d'événement ESP-IDF (MQTT).
 * @param event_id      ID de l'événement (esp_mqtt_event_id_t).
 * @param event_data    Pointeur vers la structure esp_mqtt_event_t.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            mqtt_pending_connected_log = true;
            mqtt_fail_count = 0; 
            circuit_breaker_active = false;
            mqtt_is_connected = true;
            {
                /*
                 * Souscription aux commandes d'écriture de Present_Value.
                 * Format du topic : {prefix}/{deviceId}/{typeAbbr}/{objInstance}/set
                 * Le wildcard '+' capture chaque segment variable.
                 */
                char sub_topic[128];
                snprintf(sub_topic, sizeof(sub_topic), "%s/+/+/+/set", sysCfg.mqtt_prefix);
                esp_mqtt_client_subscribe(mqtt_client, sub_topic, 0);
                
                /*
                 * Souscription aux commandes de contournement OutOfService (AI uniquement).
                 * Permet à Home Assistant de forcer/libérer une sonde via un switch dédié.
                 * Format : {prefix}/{deviceId}/AI/{instance}/outofservice/set
                 */
                char sub_topic_oos[128];
                snprintf(sub_topic_oos, sizeof(sub_topic_oos), "%s/+/+/+/outofservice/set", sysCfg.mqtt_prefix);
                esp_mqtt_client_subscribe(mqtt_client, sub_topic_oos, 0);
                
                /*
                 * Après reconnexion, on force une discovery HA complète pour
                 * resynchroniser toutes les entités (le broker a pu perdre
                 * les messages retained si redémarré entre-temps).
                 */
                force_full_discovery = true; 
                pending_discovery = true;
                
                /* Publication du statut "online" sur le topic LWT (retained, QoS 1).
                 * Le broker publie automatiquement "offline" si la connexion est perdue. */
                if (lwt_topic[0] != 0) {
                    esp_mqtt_client_publish(mqtt_client, lwt_topic, "online", 0, 1, 1);
                    z_log(pdLOG_INFO, "MQTT", "Published: %s = %s\n", lwt_topic, "online");
                    period_mqtt_tele_count++;
                }
            }
            break;

        case MQTT_EVENT_ERROR:
        case MQTT_EVENT_DISCONNECTED:
            mqtt_is_connected = false;
            /* Si le Circuit Breaker est déjà actif, on ignore les événements
             * supplémentaires pour éviter des logs redondants. */
            if (circuit_breaker_active) break;

            /* Distinction entre perte WiFi et erreur MQTT pure.
             * En cas de perte WiFi, on ne compte pas comme échec MQTT
             * car la reconnexion sera gérée par handle_mqtt(). */
            if (WiFi.status() != WL_CONNECTED) {
                mqtt_pending_wifi_loss_log = true;
                break;
            }

            /* Détection d'erreur d'authentification : si le broker refuse
             * les credentials, on saute directement à 3 échecs pour
             * déclencher le Circuit Breaker immédiatement (pas de retry inutile). */
            if (event->error_handle != NULL) {
                if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                    if (event->error_handle->connect_return_code == MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
                        event->error_handle->connect_return_code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED) {
                        mqtt_pending_auth_error_log = true;
                        mqtt_fail_count = 3;
                    }
                }
            }

            mqtt_fail_count++;
            mqtt_pending_disconnected_log = true;

            /* Seuil du Circuit Breaker : 3 échecs consécutifs.
             * Au-delà, le client sera détruit par handle_mqtt(). */
            if (mqtt_fail_count >= 3) {
                mqtt_pending_breaker_log = true;
                circuit_breaker_active = true;
            }
            break;

        /**
         * Traitement des messages reçus sur les topics de commande.
         *
         * Parsing du topic pour extraire deviceId, type, instance et action :
         *   {prefix}/{deviceId}/{typeAbbr}/{objInstance}/set
         *   {prefix}/{deviceId}/{typeAbbr}/{objInstance}/outofservice/set
         *
         * Le parsing utilise indexOf('/') séquentiel sur la String du topic.
         * Positions : p1=après prefix, p2=après deviceId, p3=après type, p4=après instance.
         *
         * Pour les Multi-State (MSO, MSV), le payload texte est converti en index
         * numérique BACnet (1-based) via les state_texts de l'objet en cache.
         */
        case MQTT_EVENT_DATA:
            {
                char topic_buf[128];
                if (event->topic_len < sizeof(topic_buf)) {
                    memcpy(topic_buf, event->topic, event->topic_len);
                    topic_buf[event->topic_len] = '\0';
                    char payload_buf[32];
                    int plen = event->data_len < 31 ? event->data_len : 31;
                    memcpy(payload_buf, event->data, plen);
                    payload_buf[plen] = '\0';
                    
                    String t = String(topic_buf);
                    int p1 = t.indexOf('/');
                    int p2 = t.indexOf('/', p1 + 1);
                    int p3 = t.indexOf('/', p2 + 1);
                    int p4 = t.indexOf('/', p3 + 1);
                    if (p1 > 0 && p2 > 0 && p3 > 0 && p4 > 0) {
                        uint32_t ulDeviceId = t.substring(p1 + 1, p2).toInt();
                        uint8_t target_mac = 0;
                        bool found = false;

                        /* Section critique : accès au cache BACnet pour :
                         * 1. Résoudre le deviceId en adresse MAC MS/TP
                         * 2. Construire le BACnetJob d'écriture
                         * 3. Pour les MSO/MSV, convertir le texte en index numérique
                         * Timeout court (100ms) pour ne pas bloquer les autres tâches. */
                        if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(100))) {
                            for (auto& d : bacnet_network_cache) {
                                if (d.ulDeviceId == ulDeviceId) { 
                                    target_mac = d.ucMacAddress; 
                                    found = true; 
                                    
                                    BACnetJob job;
                                    job.type = JOB_WRITE_PROP;
                                    job.target_mac = target_mac;
                                    job.obj_instance = t.substring(p3 + 1, p4).toInt();
                                    job.priority = 8; // v7.0.18: Priorité 8 par défaut pour toutes les écritures MQTT

                                    
                                    /* Distinction entre écriture Present_Value (prop 85) et
                                     * commande OutOfService (prop 81) selon la structure du topic.
                                     * Un 5e segment "outofservice" avant "/set" indique une commande OoS. */
                                    int p5 = t.indexOf('/', p4 + 1);
                                    if (p5 > 0 && t.substring(p4 + 1, p5) == "outofservice") {
                                        job.prop_id = 81;
                                    } else {
                                        job.prop_id = 85;
                                    }
                                    
                                    /* Décodage de l'abréviation du type d'objet BACnet.
                                     * Mapping : AI=0, AO=1, AV=2, BI=3, BO=4, BV=5, MSI=13, MSO=14, MSV=19 */
                                    String type_str = t.substring(p2 + 1, p3);
                                    
                                    if (type_str == "AO" || type_str == "AV") job.obj_type = (type_str == "AO") ? 1 : 2;
                                    else if (type_str == "BO" || type_str == "BV") job.obj_type = (type_str == "BO") ? 4 : 5;
                                    else if (type_str == "MSO" || type_str == "MSV") job.obj_type = (type_str == "MSO") ? 14 : 19;
                                    else if (type_str == "AI") job.obj_type = 0;
                                    else { job.type = JOB_WHO_IS; found = false; }

                                    // Déterminer la priorité selon la commandabilité réelle dans le cache
                                    if (found) {
                                        job.priority = 0; // Pas de priorité par défaut (ex: non commandable)
                                        if (job.prop_id == 85) {
                                            for (auto& o : d.objects) {
                                                if (o.usType == job.obj_type && o.ulInstance == job.obj_instance) {
                                                    if (o.xIsCommandable) {
                                                        job.priority = 8; // Priorité 8 si commandable
                                                    }
                                                    break;
                                                }
                                            }
                                        }
                                    }

                                    if (found) {
                                        bool xProceed = true;
                                        
                                        /* Option A (Rejet Strict) : Pour les objets d'entrée (AI=0, BI=3, MSI=13),
                                         * l'écriture Present_Value (85) n'est autorisée que si OutOfService (81) est ON.
                                         * Sinon, on rejette la commande et on republie l'ancienne valeur du cache. */
                                        if (job.prop_id == 85 && (job.obj_type == 0 || job.obj_type == 3 || job.obj_type == 13)) {
                                            bool xIsOos = false;
                                            for (auto& o : d.objects) {
                                                if (o.usType == job.obj_type && o.ulInstance == job.obj_instance) {
                                                    xIsOos = o.isOutOfService();
                                                    if (!xIsOos) {
                                                        xProceed = false;
                                                        z_log(pdLOG_WARN, "MQTT", "Write to Input %u:%lu ignored (OutOfService is OFF)\n", 
                                                              (unsigned int)job.obj_type, (unsigned long)job.obj_instance);
                                                        // Republier l'ancienne valeur pour forcer HA à se resynchroniser
                                                        publish_mqtt_topic(d.ulDeviceId, o, 85, false);
                                                    }
                                                    break;
                                                }
                                            }
                                        }

                                        if (xProceed) {
                                            if (job.prop_id == 81) {
                                                /* Commande OutOfService : payload "ON"/"OFF".
                                                 * Mise à jour locale immédiate du statusFlags en cache
                                                 * + publication MQTT du nouvel état pour feedback HA instantané. */
                                                bool xState = (String(payload_buf) == "ON");
                                                job.write_value = xState ? 1.0f : 0.0f;
                                                
                                                /* Émulation locale : on met à jour le bit OutOfService
                                                 * dans le cache avant même l'écriture BACnet, pour que
                                                 * l'entité HA reflète immédiatement le changement. */
                                                for (auto& o : d.objects) {
                                                    if (o.usType == job.obj_type && o.ulInstance == job.obj_instance) {
                                                        if (xState) {
                                                            o.ucStatusFlags |= BACNET_STATUS_OUT_OF_SERVICE;
                                                        } else {
                                                            o.ucStatusFlags &= ~BACNET_STATUS_OUT_OF_SERVICE;
                                                        }
                                                        o.ulLastUpdate = millis();
                                                        publish_mqtt_topic(d.ulDeviceId, o, 81, false);
                                                        break;
                                                    }
                                                }
                                            } else if (String(payload_buf).equalsIgnoreCase("AUTO")) {
                                                // AJOUT CHIRURGICAL : Prise en charge du mot clé AUTO depuis MQTT (Relinquish)
                                                job.write_value = NAN;
                                            } else if (job.obj_type == 14 || job.obj_type == 19) {
                                                /* Multi-State Output/Value : conversion texte → index.
                                                 * BACnet utilise des index 1-based pour les états.
                                                 * Si le texte ne correspond à aucun state_text connu,
                                                 * on tente un fallback en conversion numérique directe. */
                                                bool text_found = false;
                                                for (auto& o : d.objects) {
                                                    if (o.usType == job.obj_type && o.ulInstance == job.obj_instance) {
                                                        for (size_t i = 0; i < o.state_texts.size(); i++) {
                                                            if (o.state_texts[i] == String(payload_buf)) {
                                                                job.write_value = (float)(i + 1);
                                                                text_found = true;
                                                                break;
                                                            }
                                                        }
                                                        break;
                                                    }
                                                }
                                                if (!text_found) job.write_value = String(payload_buf).toFloat();
                                            } else {
                                                /* Types analogiques et binaires : conversion directe float. */
                                                job.write_value = String(payload_buf).toFloat();
                                            }
                                            enqueue_bacnet_job(job);
                                        }
                                    }
                                    break; 
                                }
                            }
                            xSemaphoreGive(cache_mutex);
                        }
                    }
                }
            }
            break;
        default: break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════════
 *  TÂCHE GATEKEEPER MQTT (FreeRTOS)
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Tâche FreeRTOS dédiée à l'orchestration de toutes les opérations MQTT.
 *
 * Épinglée sur Core 0 (priorité 5), cette tâche est le point central de
 * sérialisation des publications MQTT. Elle assure cinq responsabilités :
 *
 * 1. **Lazy Save NVS** : Parcours des devices marqués "dirty" dans le cache.
 *    Si un device a été modifié il y a plus de 2 secondes, sa sauvegarde NVS
 *    est déclenchée ici (Core 0) plutôt que depuis la FSM MS/TP (Core 1)
 *    pour éviter de bloquer le temps réel BACnet.
 *
 * 2. **Logs différés** : Consommation des flags atomiques positionnés par le
 *    handler d'événements. Garantit que z_log() est appelé dans un contexte
 *    avec suffisamment de pile (16KB vs ~4KB du handler ESP-MQTT).
 *
 * 3. **HA Discovery** : Déclenchement de publish_ha_autodiscovery() avec
 *    rate-limiting (30s global, 5s par device) et coalescence des demandes.
 *
 * 4. **Queue de publication** : Consommation non-bloquante de mqtt_publish_queue.
 *    Chaque MQTTPublishJob est traduit en topic MQTT et publié. Un délai de 5ms
 *    entre chaque publication évite de saturer l'outbox ESP-MQTT.
 *
 * 5. **Télémétrie Gateway** : Publication périodique (mqtt_poll_interval) des
 *    métriques système (version, RSSI, heap, température, état MS/TP, etc.)
 *    sur les topics {prefix}/B2M/{key}/state.
 *
 * @param pv  Paramètre utilisateur (non utilisé).
 */
static void mqtt_gatekeeper_task(void *pv) {
    z_log(pdLOG_INFO, "MQTT", "Gatekeeper Task Operational.\n");
    uint32_t last_status_pub = 0;     ///< Timestamp de la dernière publication de statut Gateway
    uint32_t last_global_disc = 0;    ///< Timestamp de la dernière HA Discovery globale
    uint32_t last_single_disc = 0;    ///< Timestamp de la dernière HA Discovery ciblée

    while (1) {
        uint32_t now = millis();

        /* ── 0. Lazy Save NVS ──────────────────────────────────────────────
         * Les devices modifiés (xDirty=true) sont sauvegardés en NVS
         * avec un délai de 2 secondes (anti-rebond) pour regrouper les
         * modifications rapides consécutives (ex: discovery de 20 objets).
         * Le snapshot des DIDs à sauvegarder est pris sous mutex court,
         * puis la sauvegarde NVS (lente, ~50ms par page) est faite hors mutex. */
        std::vector<uint32_t> dirty_dids;
        if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(100))) {
            for (auto& dev : bacnet_network_cache) {
                if (dev.xDirty && (now - dev.ulLastDirtyTime > 2000)) {
                    dirty_dids.push_back(dev.ulDeviceId);
                    dev.xDirty = false; // Reset to avoid double trigger
                }
            }
            xSemaphoreGive(cache_mutex);
        }

        for (uint32_t did : dirty_dids) {
            save_device_objects_locked(did);
        }

        /* ── 1. Logs d'événements déportés ─────────────────────────────────
         * exchange(false) lit et reset atomiquement le flag en une opération. */
        if (mqtt_pending_connected_log.exchange(false)) z_log(pdLOG_INFO, "MQTT", "Connected to Broker.\n");
        if (mqtt_pending_disconnected_log.exchange(false)) z_log(pdLOG_WARN, "MQTT", "Disconnected from Broker (%d/3).\n", mqtt_fail_count);
        if (mqtt_pending_auth_error_log.exchange(false)) z_log(pdLOG_ERROR, "MQTT", "FATAL: Invalid Broker Credentials.\n");
        if (mqtt_pending_wifi_loss_log.exchange(false)) z_log(pdLOG_WARN, "MQTT", "Connection dropped due to Wi-Fi loss.\n");
        if (mqtt_pending_breaker_log.exchange(false)) z_log(pdLOG_ERROR, "MQTT", "CIRCUIT BREAKER: Halting MQTT connection attempts.\n");

        if (mqtt_is_connected && !circuit_breaker_active) {
            
            /* ── 2. Déclenchement Découverte HA (Synchrone v6.8.15) ────────
             * Rate-limiting : 30s entre deux discoveries globales, 5s entre
             * deux discoveries ciblées (un seul device/objet).
             * La coalescence (trigger_ha_discovery) fusionne les demandes
             * rapides en une seule discovery plus large. */
            if (pending_discovery.load(std::memory_order_acquire)) {
                uint32_t t_did = target_did.load();
                bool is_global = (t_did == 0 || force_full_discovery);
                bool can_start = is_global ? (now - last_global_disc > 30000) : (now - last_single_disc > 5000);

                if (can_start) {
                    if (is_global) last_global_disc = now; else last_single_disc = now;
                    publish_ha_autodiscovery(target_did.load(), target_inst.load(), target_type.load());
                    pending_discovery.store(false, std::memory_order_release);
                    force_full_discovery = false;
                }
            }

            /* ── 3. Consommation de la queue de publication ─────────────────
             * Boucle non-bloquante (timeout 0) qui vide la queue.
             * Chaque message est traduit en topic MQTT via le mapping
             * type → abréviation (AI, AO, AV, BI, BO, BV, MSI, MSO, MSV).
             * La propriété détermine le sous-topic : 85→state, 77→name, 81→outofservice.
             * En cas d'échec de publication (outbox plein), on pause 50ms
             * et on sort de la boucle pour laisser l'outbox se vider. */
            MQTTPublishJob pubJob;
            while (xQueueReceive(mqtt_publish_queue, &pubJob, 0) == pdTRUE) {
                char topic[128];
                const char* t_str = "OBJ";
                switch(pubJob.obj_type) {
                    case OBJ_ANALOG_INPUT:       t_str = "AI"; break;
                    case OBJ_ANALOG_OUTPUT:      t_str = "AO"; break;
                    case OBJ_ANALOG_VALUE:       t_str = "AV"; break;
                    case OBJ_BINARY_INPUT:       t_str = "BI"; break;
                    case OBJ_BINARY_OUTPUT:      t_str = "BO"; break;
                    case OBJ_BINARY_VALUE:       t_str = "BV"; break;
                    case OBJ_MULTI_STATE_INPUT:  t_str = "MSI"; break;
                    case OBJ_MULTI_STATE_OUTPUT: t_str = "MSO"; break;
                    case OBJ_MULTI_STATE_VALUE:  t_str = "MSV"; break;
                }
                const char* subtopic = "state";
                if (pubJob.prop_id == 77) subtopic = "name";
                else if (pubJob.prop_id == 81) subtopic = "outofservice";
                snprintf(topic, sizeof(topic), "%s/%lu/%s/%lu/%s", sysCfg.mqtt_prefix, (unsigned long)pubJob.ulDeviceId, t_str, (unsigned long)pubJob.obj_instance, subtopic);
                
                if (esp_mqtt_client_publish(mqtt_client, topic, pubJob.value_string, 0, 1, pubJob.retain) < 0) {
                    z_log(pdLOG_WARN, "MQTT", "Publish failed (outbox full). Putting back to queue front...\n");
                    xQueueSendToFront(mqtt_publish_queue, &pubJob, 0);
                    vTaskDelay(pdMS_TO_TICKS(100));
                    break;
                } else {
                    period_mqtt_pub_count++;
                    period_mqtt_obj_count++;
                    z_log(pdLOG_DEBUG, "MQTT", "Published: %s = %s\n", topic, pubJob.value_string);
                }
                /* Délai inter-message de 5ms pour éviter de saturer le buffer
                 * d'envoi TCP et permettre au stack WiFi de traiter les ACK. */
                vTaskDelay(pdMS_TO_TICKS(5));
            }

            /* ── 4. Télémétrie Gateway périodique ──────────────────────────
             * Publie les métriques système sur {prefix}/B2M/{key}/state
             * à intervalle configurable (sysCfg.mqtt_poll_interval en secondes).
             * Lambda pub_b2m encapsule le pattern répétitif de publication. */
            if (millis() - last_status_pub > (sysCfg.mqtt_poll_interval * 1000)) {
                last_status_pub = millis();
                auto pub_b2m = [&](const char* key, String val) {
                    char t[128]; snprintf(t, sizeof(t), "%s/B2M/%s/state", sysCfg.mqtt_prefix, key);
                    esp_mqtt_client_publish(mqtt_client, t, val.c_str(), 0, 1, 0);
                    period_mqtt_pub_count++;
                    period_mqtt_b2m_count++;
                    z_log(pdLOG_DEBUG, "MQTT", "Poll-Published: %s = %s\n", t, val.c_str());
                };
                pub_b2m("ver", configVERSION_GLOBAL);
                pub_b2m("rssi", String(WiFi.RSSI()));
                pub_b2m("heap", String(ESP.getFreeHeap() / 1024));
                pub_b2m("min_heap", String(ESP.getMinFreeHeap() / 1024));
                pub_b2m("uptime", String(millis() / 1000));

                size_t n_dev = 0;
                if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(100))) {
                    n_dev = bacnet_network_cache.size();
                    xSemaphoreGive(cache_mutex);
                }
                pub_b2m("nb_dev", String(n_dev));

                /* Température interne du SoC ESP32-S3 (capteur intégré). */
                pub_b2m("temp", String(temperatureRead(), 1));

                /* Détection de l'activité du réseau MS/TP : on compare le compteur
                 * de jetons vus entre deux cycles. Si le compteur a changé, le bus
                 * est actif. Publié comme binary_sensor "ON"/"OFF" dans HA. */
                static uint32_t last_token_count = 0;
                bool mstp_active = (bacnetStats.ulTokensSeen != last_token_count);
                last_token_count = bacnetStats.ulTokensSeen;
                pub_b2m("mstp", mstp_active ? "ON" : "OFF");

                z_log(pdLOG_INFO, "MQTT", "Gateway Status published (Messages: %lu, Devices: %zu)\n", (unsigned long)period_mqtt_pub_count, n_dev);
                z_log(pdLOG_INFO, "MQTT", "Published topics : %s/+ : %lu msg, %s/B2M : %lu msg, tele/%s : %lu msg\n", 
                      sysCfg.mqtt_prefix, (unsigned long)period_mqtt_obj_count, 
                      sysCfg.mqtt_prefix, (unsigned long)period_mqtt_b2m_count,
                      sysCfg.mqtt_prefix, (unsigned long)period_mqtt_tele_count);

                /* Remise à zéro des compteurs de période. */
                period_mqtt_pub_count = 0;
                period_mqtt_obj_count = 0;
                period_mqtt_b2m_count = 0;
                period_mqtt_tele_count = 0;
                }
        }
        /* Cadence de la boucle principale : 10ms (100 Hz).
         * Compromis entre réactivité de publication et charge CPU. */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ═══════════════════════════════════════════════════════════════════════════════
 *  QUEUE DE PUBLICATION MQTT
 * ═══════════════════════════════════════════════════════════════════════════════ */

/// Handle global de la queue FreeRTOS pour les jobs de publication MQTT
QueueHandle_t mqtt_publish_queue = NULL;

/**
 * @brief Enfile un job de publication MQTT dans la queue inter-tâches.
 *
 * Appelable depuis n'importe quel core/tâche (thread-safe via FreeRTOS queue).
 * L'envoi est non-bloquant (timeout 0) : si la queue est pleine, le message
 * est silencieusement abandonné (retour false).
 *
 * @param pubJob  Structure MQTTPublishJob contenant deviceId, type, instance,
 *                propriété, valeur formatée et flag retain.
 * @return true si le job a été enfilé, false si la queue est pleine ou non initialisée.
 */
bool enqueue_mqtt_publish(MQTTPublishJob pubJob) { if (mqtt_publish_queue == NULL) return false; return xQueueSend(mqtt_publish_queue, &pubJob, 0) == pdTRUE; }

/**
 * @brief Initialise la queue FreeRTOS de publication MQTT.
 *
 * Crée une queue de 100 éléments MQTTPublishJob (~8.4KB avec 84 octets/job).
 * Appelée une seule fois au démarrage via setup_mqtt().
 * La profondeur de 100 permet d'absorber les rafales de polling BACnet
 * sans perte, tout en restant dans les limites mémoire de l'ESP32-S3.
 */
void init_mqtt_queue() {
    if (mqtt_publish_queue == NULL) {
        mqtt_publish_queue = xQueueCreate(100, sizeof(MQTTPublishJob));
        z_log(pdLOG_INFO, "MQTT", "Queue Initialized\n");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════════
 *  INITIALISATION DU CLIENT MQTT
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Configure et démarre le client MQTT ESP-IDF + la tâche Gatekeeper.
 *
 * Séquence d'initialisation :
 * 1. Initialisation de la queue de publication (si pas déjà fait)
 * 2. Destruction propre du client existant (si reconfiguration à chaud)
 * 3. Construction de la configuration esp_mqtt_client_config_t :
 *    - Broker : hostname + port TCP
 *    - Credentials : username/password optionnels
 *    - LWT : topic "tele/{prefix}/LWT", message "offline", QoS 1, retained
 *    - Auto-reconnexion désactivée (gérée manuellement par handle_mqtt)
 *    - Outbox limité à 20KB, buffer de sortie 4KB
 * 4. Enregistrement du handler d'événements sur ESP_EVENT_ANY_ID
 * 5. Démarrage du client
 * 6. Création unique de la tâche Gatekeeper sur Core 0 (16KB de pile)
 *
 * @note La pile de 16KB est nécessaire car la tâche Gatekeeper sérialise
 *       des payloads JSON ArduinoJson (HA Discovery) qui peuvent être volumineux
 *       (~1.5KB par entité avec toutes les métadonnées device).
 * @note La tâche n'est créée qu'une seule fois (flag statique task_created)
 *       pour survivre aux reconfigurations à chaud sans fuite de tâche.
 */
void setup_mqtt() {
    init_mqtt_queue();
    if (mqtt_client != NULL) {
        esp_mqtt_client_stop(mqtt_client);
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
    }

    mqtt_fail_count = 0;
    circuit_breaker_active = false;
    if (strlen(sysCfg.mqtt_server) == 0) return;
    
    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.hostname = sysCfg.mqtt_server;
    mqtt_cfg.broker.address.port = sysCfg.mqtt_port;
    mqtt_cfg.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
    mqtt_cfg.credentials.username = strlen(sysCfg.mqtt_user) > 0 ? sysCfg.mqtt_user : NULL;
    mqtt_cfg.credentials.authentication.password = strlen(sysCfg.mqtt_pass) > 0 ? sysCfg.mqtt_pass : NULL;

    /* Configuration du Last Will and Testament (LWT).
     * Le broker publiera automatiquement "offline" sur ce topic si le client
     * se déconnecte de manière non-gracieuse (crash, perte réseau).
     * Topic format : tele/{prefix}/LWT */
    snprintf(lwt_topic, sizeof(lwt_topic), "tele/%s/LWT", sysCfg.mqtt_prefix);
    mqtt_cfg.session.last_will.topic = lwt_topic;
    mqtt_cfg.session.last_will.msg = "offline";
    mqtt_cfg.session.last_will.qos = 1;
    mqtt_cfg.session.last_will.retain = 1;

    /* Auto-reconnexion désactivée : la logique de reconnexion est gérée
     * manuellement dans handle_mqtt() pour intégrer le Circuit Breaker
     * et la détection de perte WiFi. */
    mqtt_cfg.network.disable_auto_reconnect = true;
    mqtt_cfg.outbox.limit = 20480;   ///< Limite outbox 20KB (buffer de messages en attente d'envoi)
    mqtt_cfg.buffer.out_size = 4096; ///< Buffer de sérialisation TCP 4KB (suffisant pour HA Discovery)

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (mqtt_client) {
        esp_mqtt_client_register_event(mqtt_client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
        esp_mqtt_client_start(mqtt_client);
        
        static bool task_created = false;
        if (!task_created) {
            /* Core 0 : partagé avec WiFi, Web et OTA.
             * Priorité 5 : en dessous de la FSM MS/TP (Core 1, priorité élevée)
             * mais au-dessus du serveur web (priorité 1). */
            xTaskCreatePinnedToCore(mqtt_gatekeeper_task, "MQTT_GK", 16384, NULL, 5, NULL, 0);
            task_created = true;
        }
    }
}

/**
 * @brief Retourne l'état de connexion MQTT actuel.
 * @return true si le client est connecté au broker.
 */
bool is_mqtt_connected() { return mqtt_is_connected; }

/* ═══════════════════════════════════════════════════════════════════════════════
 *  DÉCLENCHEMENT DE LA HA DISCOVERY
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Demande une publication HA Auto-Discovery avec coalescence.
 *
 * Implémente une stratégie de coalescence (v6.4.2) pour fusionner les
 * demandes de discovery rapides en une seule opération :
 * - Si aucune discovery n'est en attente : enregistre la cible exacte
 * - Si une discovery est déjà en attente pour un autre device :
 *   élargit à une discovery globale (did=0)
 * - Si même device mais objet différent : élargit à tous les objets du device
 *
 * Ce mécanisme évite les rafales de discovery lors de la découverte BACnet
 * (chaque objet découvert déclenche un trigger_ha_discovery individuel).
 *
 * @param did   Device ID cible (0 = tous les devices).
 * @param inst  Instance cible (0xFFFFFFFF = toutes les instances).
 * @param type  Type d'objet cible (0xFFFF = tous les types).
 */
void trigger_ha_discovery(uint32_t did, uint32_t inst, uint16_t type) {
    if (!sysCfg.ha_discover) return;
    
    // Stratégie de Coalescence (v6.4.2)
    if (pending_discovery.load()) {
        if (target_did != did) {
            target_did = 0;
            target_inst = 0xFFFFFFFF;
        } else if (target_inst != inst || target_type != type) {
            target_inst = 0xFFFFFFFF;
            target_type = 0xFFFF;
        }
    } else {
        target_did = did;
        target_inst = inst;
        target_type = type;
    }
    
    pending_discovery = true;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 *  BOUCLE PRINCIPALE MQTT (appelée depuis loop() sur Core 0)
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Gère la reconnexion MQTT et le Circuit Breaker.
 *
 * Appelée périodiquement depuis la boucle principale Arduino (Core 0).
 * Responsabilités :
 * - Détecte la restauration du WiFi et relance la connexion MQTT
 * - Gère la transition WiFi connecté → déconnecté
 * - Détruit le client MQTT si le Circuit Breaker est actif
 *
 * La reconnexion est conditionnée :
 * - Si mqtt_client == NULL : appel à setup_mqtt() (première connexion ou après CB)
 * - Si mqtt_client existe : appel à esp_mqtt_client_reconnect() (reconnexion légère)
 * - Dans les deux cas, le Circuit Breaker bloque la tentative
 */
void handle_mqtt() {
    static bool was_wifi_connected = false;
    bool is_wifi_connected = (WiFi.status() == WL_CONNECTED);

    /* Détection de front montant WiFi : déclenche la (re)connexion MQTT.
     * Le test mqtt_client == NULL distingue le premier setup de la reconnexion. */
    if (is_wifi_connected && !was_wifi_connected) {
        if (mqtt_client == NULL && !circuit_breaker_active) setup_mqtt(); 
        else if (mqtt_client != NULL && !circuit_breaker_active) esp_mqtt_client_reconnect(mqtt_client); 
        was_wifi_connected = true;
    } else if (!is_wifi_connected && was_wifi_connected) {
        was_wifi_connected = false;
    }

    /* Destruction du client MQTT quand le Circuit Breaker est actif.
     * Libère les ressources (socket TCP, tâche interne esp-mqtt, outbox).
     * La recréation nécessitera un redémarrage ou une reconfiguration. */
    if (circuit_breaker_active && mqtt_client != NULL) {
        esp_mqtt_client_stop(mqtt_client);
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
        z_log(pdLOG_ERROR, "MQTT", "Circuit Breaker Active. Client destroyed.\n");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════════
 *  AUTO-DISCOVERY HOME ASSISTANT
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Publie les payloads JSON d'Auto-Discovery pour Home Assistant.
 *
 * Génère et publie les configurations MQTT Discovery conformes au standard
 * Home Assistant sur les topics homeassistant/{component}/{unique_id}/config.
 * Chaque payload JSON contient :
 * - Identifiant unique (uniq_id), nom, topic d'état et de commande
 * - Disponibilité liée au topic LWT
 * - Rattachement au device HA (identifiants, fabricant, version)
 * - Métadonnées spécifiques au composant (unité, classe, min/max/step, options)
 *
 * La fonction opère en deux étapes :
 *
 * **Étape 0 : Discovery de la Gateway** (si discovery globale)
 * Publie 8 entités de monitoring système : version, uptime, RSSI, heap,
 * min_heap, température, nombre de devices, état MS/TP.
 *
 * **Étape 1 : Discovery des objets BACnet**
 * Parcourt chaque device/objet du cache avec un pattern snapshot-sous-mutex :
 * - Prise du mutex courte (~200ms timeout) pour copier les champs nécessaires
 * - Relâchement du mutex avant la sérialisation JSON et la publication MQTT
 * - Ceci évite de bloquer la FSM MS/TP pendant les publications réseau
 *
 * Mapping type BACnet → composant HA :
 * | Type BACnet | Commandable | Composant HA |
 * |-------------|-------------|-------------|
 * | AI          | non         | sensor      |
 * | AO, AV      | oui         | number      |
 * | AO, AV      | non         | sensor      |
 * | BI          | non         | binary_sensor |
 * | BO, BV      | oui         | switch      |
 * | BO, BV      | non         | binary_sensor |
 * | MSO, MSV    | + states    | select      |
 * | MSO, MSV    | commandable | number      |
 * | MSI         | non         | sensor      |
 *
 * Pour les Analog Inputs (AI), deux entités supplémentaires de contournement
 * sont créées :
 * - Un switch "Out of Service" pour isoler la sonde physique
 * - Un number "Forcing Value" pour injecter une valeur de forçage
 *
 * Les références dynamiques min/max (cMinRef, cMaxRef) permettent de lier
 * les bornes d'un number à la Present_Value d'un autre objet BACnet du même
 * device (ex: "AI:3" lie le min à la valeur de l'Analog Input instance 3).
 *
 * @param t_did   Device ID cible (0 = tous les devices).
 * @param t_inst  Instance cible (0xFFFFFFFF = toutes).
 * @param t_type  Type d'objet cible (0xFFFF = tous).
 *
 * @note Le pacing de 100ms entre chaque objet publié évite de saturer
 *       l'outbox ESP-MQTT (limité à 20KB).
 */
void publish_ha_autodiscovery(uint32_t t_did, uint32_t t_inst, uint16_t t_type) {
    if (!mqtt_is_connected || circuit_breaker_active || !sysCfg.ha_discover) return;

    bool is_single_object = (t_did != 0 && t_inst != 0xFFFFFFFF);
    
    if (!is_single_object) {
        z_log(pdLOG_INFO, "MQTT", "Starting HA Auto-Discovery%s...\n", (t_did != 0) ? " (Single Device)" : "");
    }

    /* ── Étape 0 : Discovery des entités Gateway ──────────────────────────
     * Publiée uniquement lors d'une discovery globale (t_did == 0).
     * Chaque entité est rattachée au device HA "b2m_gateway" avec
     * modèle ESP32-S3 et version firmware courante. */
    if (!is_single_object && t_did == 0) {
        char base_b2m[128];
        snprintf(base_b2m, sizeof(base_b2m), "%s/B2M", sysCfg.mqtt_prefix);
        
        /**
         * @brief Lambda de publication d'une entité sensor/binary_sensor Gateway.
         * @param key      Clé du sous-topic (ex: "rssi", "heap").
         * @param name     Nom affiché dans Home Assistant.
         * @param dev_cla  Device class HA (ex: "signal_strength", "temperature"). NULL si aucune.
         * @param unit     Unité de mesure HA. NULL si aucune.
         * @param icon     Icône MDI optionnelle.
         * @param is_binary  true pour binary_sensor (payloads ON/OFF), false pour sensor numérique.
         */
        auto pub_gw_sensor = [&](const char* key, const char* name, const char* dev_cla, const char* unit, const char* icon = NULL, bool is_binary = false) {
            JsonDocument doc;
            char topic[128], uniq[64];
            snprintf(uniq, sizeof(uniq), "b2m_gw_%s", key);
            snprintf(topic, sizeof(topic), "homeassistant/%s/%s/config", is_binary ? "binary_sensor" : "sensor", uniq);
            
            doc["name"] = name;
            doc["uniq_id"] = uniq;
            char stat_t[128]; snprintf(stat_t, sizeof(stat_t), "%s/%s/state", base_b2m, key);
            doc["stat_t"] = stat_t;
            doc["avty_t"] = lwt_topic;
            
            if (dev_cla) doc["dev_cla"] = dev_cla;
            if (unit) doc["unit_of_meas"] = unit;
            if (icon) doc["icon"] = icon;
            if (strcmp(key, "uptime") == 0) {
                doc["val_tpl"] = "{% set s = value | int %}{% set d = s // 86400 %}{% set h = (s % 86400) // 3600 %}{% set m = (s % 3600) // 60 %}{% set sec = s % 60 %}{% if d > 0 %}{{ d }}j {{ h }}h {{ m }}m {{ sec }}s{% elif h > 0 %}{{ h }}h {{ m }}m {{ sec }}s{% else %}{{ m }}m {{ sec }}s{% endif %}";
            }

            if (is_binary) {
                doc["pl_on"] = "ON";
                doc["pl_off"] = "OFF";
            }
            
            JsonObject device = doc["dev"].to<JsonObject>();
            JsonArray ids = device["ids"].to<JsonArray>();
            ids.add("b2m_gateway");
            device["name"] = "BACnet2MQTT Gateway";
            device["mf"] = "Custom";
            device["mdl"] = "ESP32-S3";
            device["sw"] = configVERSION_GLOBAL;

            String payload;
            serializeJson(doc, payload);
            esp_mqtt_client_publish(mqtt_client, topic, payload.c_str(), 0, 1, 1);
        };

        pub_gw_sensor("ver", "Gateway Version", NULL, NULL, "mdi:information-outline");
        pub_gw_sensor("uptime", "Gateway Uptime", NULL, NULL, "mdi:timer-outline");
        pub_gw_sensor("rssi", "Gateway WiFi RSSI", "signal_strength", "dBm");
        pub_gw_sensor("heap", "Gateway Free Heap", "data_size", "KB", "mdi:memory");
        pub_gw_sensor("min_heap", "Gateway Min Heap", "data_size", "KB", "mdi:memory");
        pub_gw_sensor("temp", "Gateway Chip Temp", "temperature", "°C");
        pub_gw_sensor("nb_dev", "Gateway Devices Count", NULL, "dev", "mdi:counter");
        pub_gw_sensor("mstp", "Gateway MS/TP Network", "connectivity", NULL, NULL, true);

        vTaskDelay(pdMS_TO_TICKS(100)); 
    }

    /* ── Étape 1 : Discovery des objets BACnet ────────────────────────────
     * Pattern "snapshot sous mutex court" :
     * 1. On prend le mutex pour lire nb_devices
     * 2. Pour chaque device, on prend le mutex pour snapshot les champs
     * 3. Pour chaque objet, on prend le mutex pour snapshot les champs
     * 4. La sérialisation JSON et la publication se font HORS mutex
     *
     * Ce pattern est crucial car la sérialisation JSON + publication TCP
     * peut prendre plusieurs dizaines de ms, pendant lesquelles la FSM
     * MS/TP (Core 1) doit pouvoir accéder au cache. */
    size_t nb_devices = 0;
    if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(1000))) {
        nb_devices = bacnet_network_cache.size();
        xSemaphoreGive(cache_mutex);
    }

    int total_published = 0;

    for (size_t d_idx = 0; d_idx < nb_devices; d_idx++) {
        size_t nb_objects = 0;
        uint32_t current_did = 0;
        String dev_name, dev_vendor;
        bool dev_enabled = false, dev_discovery_done = false;

        /* Snapshot des métadonnées du device sous mutex court. */
        if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(200))) {
            if (d_idx < bacnet_network_cache.size()) {
                auto& dev = bacnet_network_cache[d_idx];
                if (t_did != 0 && dev.ulDeviceId != t_did) { xSemaphoreGive(cache_mutex); continue; }
                current_did = dev.ulDeviceId;
                dev_name = dev.name;
                dev_vendor = dev.vendor;
                dev_enabled = dev.xEnabled;
                dev_discovery_done = dev.xDiscoveryDone;
                nb_objects = dev.objects.size();
            }
            xSemaphoreGive(cache_mutex);
        } else continue;

        if (current_did == 0) continue;

        for (size_t o_idx = 0; o_idx < nb_objects; o_idx++) {
            /* Snapshot de chaque objet : copie locale de tous les champs
             * nécessaires à la construction du payload JSON.
             * Le vecteur state_texts est copié par valeur (deep copy)
             * pour être utilisable hors mutex. */
            uint16_t obj_type = 65535;
            uint32_t obj_inst = 0;
            char obj_name[64] = {0};
            char obj_unit_text[32] = {0};
            uint16_t obj_units = 0;
            float obj_min = NAN, obj_max = NAN, obj_step = 0.1f;
            char obj_min_ref[6] = {0}, obj_max_ref[6] = {0};
            bool obj_enabled = false, obj_commandable = false;
            std::vector<String> obj_states;

            if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(200))) {
                if (d_idx < bacnet_network_cache.size() && o_idx < bacnet_network_cache[d_idx].objects.size()) {
                    auto& obj = bacnet_network_cache[d_idx].objects[o_idx];
                    if (is_single_object && (obj.ulInstance != t_inst || obj.usType != t_type)) { xSemaphoreGive(cache_mutex); continue; }
                    obj_type = obj.usType;
                    obj_inst = obj.ulInstance;
                    strlcpy(obj_name, obj.cName, sizeof(obj_name));
                    strlcpy(obj_unit_text, obj.cUnitText, sizeof(obj_unit_text));
                    obj_units = obj.usUnits;
                    obj_min = obj.fMinValue;
                    obj_max = obj.fMaxValue;
                    obj_step = obj.fStepValue;
                    strlcpy(obj_min_ref, obj.cMinRef, sizeof(obj_min_ref));
                    strlcpy(obj_max_ref, obj.cMaxRef, sizeof(obj_max_ref));
                    obj_enabled = obj.xEnabled;
                    obj_commandable = obj.xIsCommandable;
                    obj_states = obj.state_texts; 
                }
                xSemaphoreGive(cache_mutex);
            } else continue;

            if (obj_type == 65535) continue;

            /* Mapping du type BACnet vers l'abréviation topic et le composant HA.
             * La commandabilité (propriété 87 Priority_Array) détermine si l'objet
             * est exposé comme entité de contrôle (number/switch/select) ou
             * comme simple capteur (sensor/binary_sensor).
             * Pour les Multi-State avec state_texts, on utilise "select" plutôt que "number". */
            const char* t_str = "OBJ";
            const char* ha_component = "sensor";

            switch(obj_type) {
                case OBJ_ANALOG_INPUT:       t_str = "AI"; ha_component = "sensor"; break;
                case OBJ_ANALOG_OUTPUT:      t_str = "AO"; ha_component = "number"; break;
                case OBJ_ANALOG_VALUE:       t_str = "AV"; ha_component = "number"; break;
                case OBJ_BINARY_INPUT:       t_str = "BI"; ha_component = "binary_sensor"; break;
                case OBJ_BINARY_OUTPUT:      t_str = "BO"; ha_component = "switch"; break;
                case OBJ_BINARY_VALUE:       t_str = "BV"; ha_component = "switch"; break;
                case OBJ_MULTI_STATE_INPUT:  t_str = "MSI"; ha_component = "sensor"; break;
                case OBJ_MULTI_STATE_OUTPUT: t_str = "MSO"; ha_component = (obj_states.empty()) ? "number" : "select"; break;
                case OBJ_MULTI_STATE_VALUE:  t_str = "MSV"; ha_component = (obj_states.empty()) ? "number" : "select"; break;
            }

            /* Construction de l'identifiant unique HA et du topic de configuration.
             * Format uniq_id : bacnet_{deviceId}_{typeAbbr}_{instance}
             * Format topic   : homeassistant/{component}/{uniq_id}/config */
            char uniq_id[64];
            snprintf(uniq_id, sizeof(uniq_id), "bacnet_%lu_%s_%lu", (unsigned long)current_did, t_str, (unsigned long)obj_inst);
            char topic[128];
            snprintf(topic, sizeof(topic), "homeassistant/%s/%s/config", ha_component, uniq_id);

            /* Conditions de publication :
             * - Device et objet doivent être activés (xEnabled)
             * - L'objet doit avoir un nom résolu (pas "Unknown")
             * - La discovery du device doit être terminée (sauf mode single-object) */
            if (dev_enabled && obj_enabled && strcmp(obj_name, "Unknown") != 0 && (dev_discovery_done || is_single_object)) {
                JsonDocument doc; 
                char base_topic[128];
                snprintf(base_topic, sizeof(base_topic), "%s/%lu/%s/%lu", sysCfg.mqtt_prefix, (unsigned long)current_did, t_str, (unsigned long)obj_inst);
                
                /* Le caractère "~" est une convention HA Discovery qui sert de
                 * raccourci pour le base_topic dans les champs stat_t/cmd_t. */
                doc["~"] = String(base_topic);
                doc["uniq_id"] = String(uniq_id);
                doc["name"] = String(obj_name); 

                // Définition de l'icône par défaut pour la ventilation et les volets (mdi:fan)
                String name_lower = String(obj_name);
                name_lower.toLowerCase();
                if (name_lower.indexOf("ventil") >= 0 || name_lower.indexOf("volet") >= 0) {
                    doc["icon"] = "mdi:fan";
                }

                doc["stat_t"] = "~/state";
                doc["val_tpl"] = "{{ value_json.val }}"; // AJOUT CHIRURGICAL : Extraction de la valeur depuis le JSON
                doc["avty_t"] = String(lwt_topic);
                doc["pl_avail"] = "online";
                doc["pl_not_avail"] = "offline";

                /* Le topic de commande n'est ajouté que pour les entités contrôlables
                 * (number, switch, select). Les sensor/binary_sensor sont read-only. */
                if (strcmp(ha_component, "sensor") != 0 && strcmp(ha_component, "binary_sensor") != 0) {
                    doc["cmd_t"] = "~/set";
                }

                /* Payloads binaires : "1.00" et "0.00" correspondent au format
                 * de publication de Present_Value pour les objets binaires BACnet. */
                if (strcmp(ha_component, "binary_sensor") == 0 || strcmp(ha_component, "switch") == 0) {
                    doc["pl_on"] = "1"; doc["pl_off"] = "0";
                }

                /* Configuration spécifique aux entités "number" (AO, AV commandables).
                 * Les bornes min/max proviennent de :
                 * 1. Les propriétés BACnet 69 (Min_Pres_Value) et 65 (Max_Pres_Value)
                 * 2. Les valeurs par défaut de la configuration système (sysCfg)
                 * 3. Les références dynamiques (cMinRef/cMaxRef) vers d'autres objets
                 *    du même device, permettant des bornes qui suivent une valeur live. */
                if (strcmp(ha_component, "number") == 0) {
                    float final_min = isnan(obj_min) ? sysCfg.default_number_min : obj_min;
                    float final_max = isnan(obj_max) ? sysCfg.default_number_max : obj_max;
                    
                    /* Résolution de la référence min dynamique.
                     * Format : "AI:3" ou "AV:12" (type:instance).
                     * On cherche l'objet référencé dans le même device et on utilise
                     * sa Present_Value comme borne minimum du number HA. */
                    if (strlen(obj_min_ref) > 0) {
                        uint16_t ref_type = 65535; uint32_t ref_inst = 0;
                        if (strncmp(obj_min_ref, "AI:", 3) == 0) { ref_type = 0; ref_inst = atoi(obj_min_ref + 3); }
                        else if (strncmp(obj_min_ref, "AV:", 3) == 0) { ref_type = 2; ref_inst = atoi(obj_min_ref + 3); }
                        if (ref_type != 65535) {
                            if (d_idx < bacnet_network_cache.size()) {
                                for (auto& ro : bacnet_network_cache[d_idx].objects) {
                                    if (ro.usType == ref_type && ro.ulInstance == ref_inst) {
                                        final_min = ro.fPresentValue; break;
                                    }
                                }
                            }
                        }
                    }
                    /* Résolution de la référence max dynamique (même logique que min). */
                    if (strlen(obj_max_ref) > 0) {
                        uint16_t ref_type = 65535; uint32_t ref_inst = 0;
                        if (strncmp(obj_max_ref, "AI:", 3) == 0) { ref_type = 0; ref_inst = atoi(obj_max_ref + 3); }
                        else if (strncmp(obj_max_ref, "AV:", 3) == 0) { ref_type = 2; ref_inst = atoi(obj_max_ref + 3); }
                        if (ref_type != 65535) {
                            if (d_idx < bacnet_network_cache.size()) {
                                for (auto& ro : bacnet_network_cache[d_idx].objects) {
                                    if (ro.usType == ref_type && ro.ulInstance == ref_inst) {
                                        final_max = ro.fPresentValue; break;
                                    }
                                }
                            }
                        }
                    }
                    doc["min"] = final_min;
                    doc["max"] = final_max;
                    doc["step"] = obj_step;
                }

                /* Déduction automatique de la device_class HA à partir de l'unité BACnet.
                 * Uniquement pour les types analogiques (AI=0, AO=1, AV=2).
                 * Le texte d'unité BACnet (ex: "degrees-celsius") est d'abord converti
                 * en symbole court via get_unit_text(), puis mappé vers la device_class HA. */
                if (obj_type <= 2) { // AI, AO, AV
                    String unit = String(obj_unit_text);
                    if (unit == "Unknown" || unit.length() == 0 || unit == "none") unit = get_unit_text(obj_units);
                    if (unit != "no-units" && unit.length() > 0) {
                        if (unit == "%RH") unit = "%";
                        doc["unit_of_meas"] = unit;
                        if (unit == "°C" || unit == "°F") doc["dev_cla"] = "temperature";
                        else if (unit == "%") doc["dev_cla"] = "humidity";
                        else if (unit == "kW" || unit == "W") doc["dev_cla"] = "power";
                        else if (unit == "kWh") doc["dev_cla"] = "energy";
                    }
                }

                /* Pour les "select" (MSO/MSV avec state_texts), on ajoute la liste
                 * des options possibles. HA affichera un menu déroulant avec ces textes. */
                if (strcmp(ha_component, "select") == 0) {
                    JsonArray opts = doc["options"].to<JsonArray>();
                    for (auto& s : obj_states) opts.add(s);
                }

                /* Rattachement de l'entité au device HA.
                 * Tous les objets d'un même device BACnet sont regroupés sous un
                 * seul device HA identifié par "bacnet_dev_{deviceId}".
                 * Cela crée une page device dans l'interface HA avec toutes les entités. */
                JsonObject device = doc["dev"].to<JsonObject>();
                JsonArray ids = device["ids"].to<JsonArray>();
                char dev_id_str[32]; snprintf(dev_id_str, sizeof(dev_id_str), "bacnet_dev_%lu", (unsigned long)current_did);
                ids.add(String(dev_id_str));
                device["name"] = dev_name.length() > 0 ? String(dev_name) : String(dev_id_str);
                device["mf"] = dev_vendor.length() > 0 ? String(dev_vendor) : "BACnet Manufacturer";
                device["sw"] = configVERSION_GLOBAL;

                String payload; serializeJson(doc, payload);
                if (esp_mqtt_client_publish(mqtt_client, topic, payload.c_str(), payload.length(), 1, 1) < 0) {
                    z_log(pdLOG_WARN, "MQTT", "Discovery publish failed for %s. Outbox full?\n", uniq_id);
                    vTaskDelay(pdMS_TO_TICKS(100)); // Pause si erreur
                } else {
                    total_published++;
                    
                    // Nettoyage des composants HA fantômes obsolètes (si l'état de commandabilité a changé)
                    if (strcmp(ha_component, "number") == 0) {
                        char alt_topic[128];
                        snprintf(alt_topic, sizeof(alt_topic), "homeassistant/sensor/%s/config", uniq_id);
                        esp_mqtt_client_publish(mqtt_client, alt_topic, "", 0, 1, 1);
                    } else if (strcmp(ha_component, "sensor") == 0) {
                        char alt_topic[128];
                        snprintf(alt_topic, sizeof(alt_topic), "homeassistant/number/%s/config", uniq_id);
                        esp_mqtt_client_publish(mqtt_client, alt_topic, "", 0, 1, 1);
                    } else if (strcmp(ha_component, "switch") == 0) {
                        char alt_topic[128];
                        snprintf(alt_topic, sizeof(alt_topic), "homeassistant/binary_sensor/%s/config", uniq_id);
                        esp_mqtt_client_publish(mqtt_client, alt_topic, "", 0, 1, 1);
                    } else if (strcmp(ha_component, "binary_sensor") == 0) {
                        char alt_topic[128];
                        snprintf(alt_topic, sizeof(alt_topic), "homeassistant/switch/%s/config", uniq_id);
                        esp_mqtt_client_publish(mqtt_client, alt_topic, "", 0, 1, 1);
                    } else if (strcmp(ha_component, "select") == 0) {
                        char alt_topic1[128];
                        char alt_topic2[128];
                        snprintf(alt_topic1, sizeof(alt_topic1), "homeassistant/number/%s/config", uniq_id);
                        snprintf(alt_topic2, sizeof(alt_topic2), "homeassistant/sensor/%s/config", uniq_id);
                        esp_mqtt_client_publish(mqtt_client, alt_topic1, "", 0, 1, 1);
                        esp_mqtt_client_publish(mqtt_client, alt_topic2, "", 0, 1, 1);
                    }
                }

                /* ── Entités de contournement (hack) pour Analog Input ─────────
                 * Les AI BACnet sont normalement en lecture seule. Ces entités
                 * supplémentaires permettent de :
                 * 1. Isoler la sonde physique (switch OutOfService)
                 * 2. Injecter une valeur de forçage (number Forcing Value)
                 * Utile pour le commissionnement et les tests en production. */
                if (obj_type == OBJ_ANALOG_INPUT) {
                    /* --- 1. Switch "Out of Service" ---
                     * Permet d'activer/désactiver le flag OutOfService (propriété 81)
                     * sur l'Analog Input. Quand actif, la valeur physique de la sonde
                     * n'est plus publiée et l'opérateur peut injecter une valeur manuelle. */
                    {
                        JsonDocument sw_doc;
                        char sw_uniq_id[128];
                        snprintf(sw_uniq_id, sizeof(sw_uniq_id), "bacnet_%lu_AI_%lu_outofservice", (unsigned long)current_did, (unsigned long)obj_inst);
                        char sw_config_topic[128];
                        snprintf(sw_config_topic, sizeof(sw_config_topic), "homeassistant/switch/%s/config", sw_uniq_id);

                        sw_doc["uniq_id"] = String(sw_uniq_id);
                        sw_doc["name"] = String(obj_name) + " Out of Service";
                        sw_doc["icon"] = "mdi:sensor-off";
                        sw_doc["avty_t"] = String(lwt_topic);
                        sw_doc["pl_avail"] = "online";
                        sw_doc["pl_not_avail"] = "offline";
                        
                        char sw_state_topic[128];
                        snprintf(sw_state_topic, sizeof(sw_state_topic), "%s/outofservice", base_topic);
                        sw_doc["stat_t"] = String(sw_state_topic);

                        char sw_cmd_topic[128];
                        snprintf(sw_cmd_topic, sizeof(sw_cmd_topic), "%s/outofservice/set", base_topic);
                        sw_doc["cmd_t"] = String(sw_cmd_topic);

                        sw_doc["pl_on"] = "ON";
                        sw_doc["pl_off"] = "OFF";

                        /* Rattachement au même device HA que l'entité principale.
                         * L'entité OoS apparaîtra dans la même page device que le capteur AI. */
                        JsonObject sw_device = sw_doc["dev"].to<JsonObject>();
                        JsonArray sw_ids = sw_device["ids"].to<JsonArray>();
                        sw_ids.add(String(dev_id_str));
                        sw_device["name"] = dev_name.length() > 0 ? String(dev_name) : String(dev_id_str);
                        sw_device["mf"] = dev_vendor.length() > 0 ? String(dev_vendor) : "BACnet Manufacturer";
                        sw_device["sw"] = configVERSION_GLOBAL;

                        String sw_payload;
                        serializeJson(sw_doc, sw_payload);
                        esp_mqtt_client_publish(mqtt_client, sw_config_topic, sw_payload.c_str(), sw_payload.length(), 1, 1);
                        total_published++;
                    }

                    /* --- 2. Number "Forcing Value" ---
                     * Permet d'écrire une valeur numérique sur la Present_Value de l'AI
                     * quand OutOfService est actif. Partage le même topic state/set
                     * que l'entité principale (le "~" pointe vers le même base_topic).
                     * Les bornes min/max et le step sont repris de l'objet BACnet. */
                    {
                        JsonDocument num_doc;
                        char num_uniq_id[128];
                        snprintf(num_uniq_id, sizeof(num_uniq_id), "bacnet_%lu_AI_%lu_forcing", (unsigned long)current_did, (unsigned long)obj_inst);
                        char num_config_topic[128];
                        snprintf(num_config_topic, sizeof(num_config_topic), "homeassistant/number/%s/config", num_uniq_id);

                        // v7.0.14: Définition de ~ (base_topic) requise pour résoudre ~/state et ~/set
                        num_doc["~"] = String(base_topic);
                        num_doc["uniq_id"] = String(num_uniq_id);
                        num_doc["name"] = String(obj_name) + " Forcing Value";
                        num_doc["icon"] = "mdi:thermometer-cog";
                        num_doc["avty_t"] = String(lwt_topic);
                        num_doc["pl_avail"] = "online";
                        num_doc["pl_not_avail"] = "offline";
                        
                        num_doc["stat_t"] = "~/state"; 
                        num_doc["cmd_t"] = "~/set";
                        num_doc["val_tpl"] = "{{ value_json.val }}"; // AJOUT CHIRURGICAL : Extraction pour le forçage

                        // v7.0.14: Forçage des bornes min à -1.0 et max à 40.0 pour les valeurs de forçage
                        num_doc["min"] = -1.0f;
                        num_doc["max"] = 40.0f;
                        num_doc["step"] = obj_step;

                        String unit = String(obj_unit_text);
                        if (unit == "Unknown" || unit.length() == 0 || unit == "none") unit = get_unit_text(obj_units);
                        if (unit != "no-units" && unit.length() > 0) {
                            if (unit == "%RH") unit = "%";
                            num_doc["unit_of_meas"] = unit;
                        }

                        /* Rattachement au même device HA. */
                        JsonObject num_device = num_doc["dev"].to<JsonObject>();
                        JsonArray num_ids = num_device["ids"].to<JsonArray>();
                        num_ids.add(String(dev_id_str));
                        num_device["name"] = dev_name.length() > 0 ? String(dev_name) : String(dev_id_str);
                        num_device["mf"] = dev_vendor.length() > 0 ? String(dev_vendor) : "BACnet Manufacturer";
                        num_device["sw"] = configVERSION_GLOBAL;

                        String num_payload;
                        serializeJson(num_doc, num_payload);
                        esp_mqtt_client_publish(mqtt_client, num_config_topic, num_payload.c_str(), num_payload.length(), 1, 1);
                        total_published++;
                    }
                }

                // AJOUT CHIRURGICAL : Publication du bouton de libération (Reset) pour les objets commandables
                if (obj_commandable) {
                    JsonDocument reset_doc;
                    char reset_uniq_id[128];
                    snprintf(reset_uniq_id, sizeof(reset_uniq_id), "bacnet_%lu_%s_%lu_reset",
                             (unsigned long)current_did, t_str, (unsigned long)obj_inst);
                    char reset_config_topic[128];
                    snprintf(reset_config_topic, sizeof(reset_config_topic), "homeassistant/button/%s/config", reset_uniq_id);

                    reset_doc["~"] = String(base_topic);
                    reset_doc["uniq_id"] = String(reset_uniq_id);
                    reset_doc["name"] = String(obj_name) + " Reset";
                    reset_doc["cmd_t"] = "~/set";
                    reset_doc["payload_press"] = "AUTO";
                    reset_doc["icon"] = "mdi:restore";
                    reset_doc["avty_t"] = String(lwt_topic);
                    reset_doc["pl_avail"] = "online";
                    reset_doc["pl_not_avail"] = "offline";

                    JsonObject reset_device = reset_doc["dev"].to<JsonObject>();
                    JsonArray reset_ids = reset_device["ids"].to<JsonArray>();
                    reset_ids.add(String(dev_id_str));
                    reset_device["name"] = dev_name.length() > 0 ? String(dev_name) : String(dev_id_str);
                    reset_device["mf"] = dev_vendor.length() > 0 ? String(dev_vendor) : "BACnet Manufacturer";
                    reset_device["sw"] = configVERSION_GLOBAL;

                    String reset_payload;
                    serializeJson(reset_doc, reset_payload);
                    esp_mqtt_client_publish(mqtt_client, reset_config_topic, reset_payload.c_str(), reset_payload.length(), 1, 1);
                    total_published++;
                } else {
                    // Si l'objet n'est plus commandable, supprimer son bouton de reset de HA
                    char reset_uniq_id[128];
                    snprintf(reset_uniq_id, sizeof(reset_uniq_id), "bacnet_%lu_%s_%lu_reset",
                             (unsigned long)current_did, t_str, (unsigned long)obj_inst);
                    char reset_config_topic[128];
                    snprintf(reset_config_topic, sizeof(reset_config_topic), "homeassistant/button/%s/config", reset_uniq_id);
                    esp_mqtt_client_publish(mqtt_client, reset_config_topic, "", 0, 1, 1);
                }
                
                // AJOUT CHIRURGICAL : Publication des 5 sensors de status_flags pour cet objet
                const char* flags[] = {"alarm", "fault", "overridden", "oos", "overridden_bacnet"};
                const char* names[] = {"Alarme", "Défaut", "Forçage Manuel", "Hors Service", "Forçage BACnet"};
                const char* icons[] = {"mdi:alarm-light", "mdi:alert", "mdi:hand-back-right", "mdi:power-plug-off", "mdi:lan-pending"};
                
                for (int i = 0; i < 5; i++) {
                    JsonDocument flag_doc;
                    char flag_uniq_id[128];
                    snprintf(flag_uniq_id, sizeof(flag_uniq_id), "bacnet_%lu_%s_%lu_%s",
                             (unsigned long)current_did, t_str, (unsigned long)obj_inst, flags[i]);
                    char flag_config_topic[128];
                    snprintf(flag_config_topic, sizeof(flag_config_topic), "homeassistant/sensor/%s/config", flag_uniq_id);
                    
                    // Migration : Supprimer l'ancienne configuration binary_sensor
                    char flag_old_config_topic[128];
                    snprintf(flag_old_config_topic, sizeof(flag_old_config_topic), "homeassistant/binary_sensor/%s/config", flag_uniq_id);
                    esp_mqtt_client_publish(mqtt_client, flag_old_config_topic, "", 0, 1, 1);
                    
                    flag_doc["~"] = String(base_topic);
                    flag_doc["uniq_id"] = String(flag_uniq_id);
                    flag_doc["name"] = String(obj_name) + " " + names[i];
                    flag_doc["stat_t"] = "~/state";
                    flag_doc["avty_t"] = String(lwt_topic);
                    flag_doc["pl_avail"] = "online";
                    flag_doc["pl_not_avail"] = "offline";
                    flag_doc["icon"] = icons[i];
                    
                    char flag_val_tpl[128];
                    snprintf(flag_val_tpl, sizeof(flag_val_tpl), "{{ 'OUI' if value_json.%s else 'NON' }}", flags[i]);
                    flag_doc["val_tpl"] = String(flag_val_tpl);
                    
                    JsonObject flag_device = flag_doc["dev"].to<JsonObject>();
                    JsonArray flag_ids = flag_device["ids"].to<JsonArray>();
                    flag_ids.add(String(dev_id_str));
                    flag_device["name"] = dev_name.length() > 0 ? String(dev_name) : String(dev_id_str);
                    flag_device["mf"] = dev_vendor.length() > 0 ? String(dev_vendor) : "BACnet Manufacturer";
                    flag_device["sw"] = configVERSION_GLOBAL;
                    
                    String flag_payload;
                    serializeJson(flag_doc, flag_payload);
                    if (esp_mqtt_client_publish(mqtt_client, flag_config_topic, flag_payload.c_str(), flag_payload.length(), 1, 1) < 0) {
                        z_log(pdLOG_WARN, "MQTT", "Discovery publish failed for flag %s. Outbox full?\n", flag_uniq_id);
                        vTaskDelay(pdMS_TO_TICKS(100));
                    } else {
                        total_published++;
                    }
                }
            } else if ((!dev_enabled || !obj_enabled) && dev_discovery_done) {
                /* Suppression de l'entité HA : publication d'un payload vide (retained)
                 * sur le topic de configuration. HA retire automatiquement l'entité. */
                esp_mqtt_client_publish(mqtt_client, topic, "", 0, 1, 1);

                // AJOUT CHIRURGICAL : Supprimer également les 5 sensors de status_flags (et nettoyer les anciens binary_sensors)
                const char* flags[] = {"alarm", "fault", "overridden", "oos", "overridden_bacnet"};
                for (const char* flag : flags) {
                    char flag_topic_bin[128];
                    snprintf(flag_topic_bin, sizeof(flag_topic_bin), "homeassistant/binary_sensor/bacnet_%lu_%s_%lu_%s/config",
                             (unsigned long)current_did, t_str, (unsigned long)obj_inst, flag);
                    esp_mqtt_client_publish(mqtt_client, flag_topic_bin, "", 0, 1, 1);

                    char flag_topic_sen[128];
                    snprintf(flag_topic_sen, sizeof(flag_topic_sen), "homeassistant/sensor/bacnet_%lu_%s_%lu_%s/config",
                             (unsigned long)current_did, t_str, (unsigned long)obj_inst, flag);
                    esp_mqtt_client_publish(mqtt_client, flag_topic_sen, "", 0, 1, 1);
                }

                // AJOUT CHIRURGICAL : Supprimer également le bouton Reset s'il existe
                if (obj_commandable) {
                    char reset_topic[128];
                    snprintf(reset_topic, sizeof(reset_topic), "homeassistant/button/bacnet_%lu_%s_%lu_reset/config",
                             (unsigned long)current_did, t_str, (unsigned long)obj_inst);
                    esp_mqtt_client_publish(mqtt_client, reset_topic, "", 0, 1, 1);
                }
            }

            if (dev_discovery_done) {
                if (total_published % 10 == 0 && total_published > 0) {
                    z_log(pdLOG_INFO, "MQTT", "Discovery progress: %d objects sent...\n", total_published);
                }
                vTaskDelay(pdMS_TO_TICKS(100)); // Pacing stable
            }
        }
    }
    z_log(pdLOG_INFO, "MQTT", "HA Auto-Discovery payload sent (%d objects).\n", total_published);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 *  PUBLICATION DES VALEURS BACNET SUR MQTT
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Prépare et enfile un message de publication MQTT pour un objet BACnet.
 *
 * Formate la valeur de l'objet selon le type et la propriété, puis l'enfile
 * dans mqtt_publish_queue pour traitement asynchrone par le Gatekeeper.
 *
 * Propriétés supportées :
 * - **77 (Object_Name)** : Publie le nom de l'objet sur le sous-topic "name" (retained).
 *   Utilisé pour les tableaux de bord HA et le mapping des topics.
 * - **85 (Present_Value)** : Publie la valeur courante sur le sous-topic "state".
 *   Pour les Multi-State, convertit l'index numérique BACnet (1-based) en texte
 *   via les state_texts si disponibles, sinon publie l'index brut.
 *   Pour les analogiques, formate en "%.2f" (2 décimales).
 *   **Blocage spécial** : si l'AI est OutOfService, la valeur physique n'est PAS
 *   publiée pour éviter d'écraser la valeur de forçage dans HA.
 * - **81 (Out_Of_Service)** : Publie l'état OoS sur le sous-topic "outofservice"
 *   avec payload "ON"/"OFF" pour le switch HA.
 *
 * Synchronisation automatique : après chaque publication de Present_Value (85)
 * d'une AI, le statut OutOfService (81) est automatiquement republié pour
 * garantir la cohérence de l'affichage HA.
 *
 * @param ulDeviceId  Identifiant du device BACnet propriétaire.
 * @param obj         Référence vers l'objet BACnet en cache.
 * @param prop_id     Identifiant de la propriété BACnet à publier (77, 85 ou 81).
 * @param retain      true pour un message MQTT retained (persiste sur le broker).
 */
void publish_mqtt_topic(uint32_t ulDeviceId, BACnetObject& obj, uint8_t prop_id, bool retain) {
    if (!obj.xEnabled) return;

    /* Blocage de la publication de la Present_Value physique pour les sondes
     * en mode OutOfService. Empêche le polling BACnet d'écraser la valeur
     * de forçage injectée par l'opérateur via HA. */
    if (prop_id == 85 && obj.usType == OBJ_ANALOG_INPUT && obj.isOutOfService()) {
        return; 
    }

    MQTTPublishJob pub;
    pub.ulDeviceId = ulDeviceId;
    pub.obj_type = obj.usType;
    pub.obj_instance = obj.ulInstance;
    pub.prop_id = prop_id;
    pub.retain = true; // v7.0.11: Forçage du flag retain à true pour éviter l'état unknown au reload HA

    if (prop_id == 77) {
        /* Propriété Object_Name : copie directe du nom en cache. */
        if (strlen(obj.cName) == 0) return;
        strlcpy(pub.value_string, obj.cName, sizeof(pub.value_string));
    } else if (prop_id == 85) {
        /* Propriété Present_Value : formatage selon le type d'objet et empaquetage JSON */
        char val_str[64] = {0};
        switch(obj.usType) {
            case OBJ_MULTI_STATE_INPUT:
            case OBJ_MULTI_STATE_OUTPUT:
            case OBJ_MULTI_STATE_VALUE:
                if (!obj.state_texts.empty()) {
                    int idx = (int)obj.fPresentValue - 1;
                    if (idx >= 0 && idx < (int)obj.state_texts.size()) {
                        strlcpy(val_str, obj.state_texts[idx].c_str(), sizeof(val_str));
                    } else snprintf(val_str, sizeof(val_str), "%.0f", obj.fPresentValue);
                } else snprintf(val_str, sizeof(val_str), "%.0f", obj.fPresentValue);
                break;
            case OBJ_BINARY_INPUT:
            case OBJ_BINARY_OUTPUT:
            case OBJ_BINARY_VALUE:
                snprintf(val_str, sizeof(val_str), "%.0f", obj.fPresentValue);
                break;
            default:
                snprintf(val_str, sizeof(val_str), "%.2f", obj.fPresentValue);
                break;
        }

        // Création du document JSON en PSRAM
        JsonDocument xJsonDoc;
        
        // Insertion sécurisée du type de valeur (string pour Multi-State avec texte, float sinon)
        if (obj.usType == OBJ_MULTI_STATE_INPUT || obj.usType == OBJ_MULTI_STATE_OUTPUT || obj.usType == OBJ_MULTI_STATE_VALUE) {
            if (!obj.state_texts.empty()) {
                xJsonDoc["val"] = String(val_str);
            } else {
                xJsonDoc["val"] = atof(val_str);
            }
        } else {
            xJsonDoc["val"] = atof(val_str);
        }

        // Extraction et normalisation des status flags depuis le cache (0x01, 0x02, 0x04, 0x08)
        xJsonDoc["alarm"]      = (obj.ucStatusFlags & BACNET_STATUS_IN_ALARM) != 0;
        xJsonDoc["fault"]      = (obj.ucStatusFlags & BACNET_STATUS_FAULT) != 0;
        xJsonDoc["overridden"] = (obj.ucStatusFlags & BACNET_STATUS_OVERRIDDEN) != 0;
        xJsonDoc["oos"]        = (obj.ucStatusFlags & BACNET_STATUS_OUT_OF_SERVICE) != 0;
        xJsonDoc["overridden_bacnet"] = obj.xOverriddenBacnet;

        serializeJson(xJsonDoc, pub.value_string, sizeof(pub.value_string));
    } 
    /* Propriété Out_Of_Service : formatage binaire "ON"/"OFF" pour le switch HA. */
    else if (prop_id == 81) {
        strlcpy(pub.value_string, obj.isOutOfService() ? "ON" : "OFF", sizeof(pub.value_string));
    } else return;

    enqueue_mqtt_publish(pub);

    /* Synchronisation automatique : chaque publication de Present_Value d'une AI
     * est suivie d'une publication de son statut OutOfService pour garantir
     * que le switch HA reflète toujours l'état correct. */
    if (prop_id == 85 && obj.usType == OBJ_ANALOG_INPUT) {
        publish_mqtt_topic(ulDeviceId, obj, 81, retain);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════════
 *  NETTOYAGE DES ENTITÉS HA DISCOVERY
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Supprime les entités HA Discovery en publiant des payloads vides.
 *
 * Utilisée lors de :
 * - Changement de préfixe MQTT (old_prefix permet de nettoyer les anciens topics)
 * - Désactivation d'un device ou d'un objet
 * - Suppression d'un device du cache
 *
 * Pour chaque objet ciblé, un payload vide est publié sur les topics de
 * configuration de TOUS les composants possibles (sensor, binary_sensor,
 * switch, number, select). Cette approche "brute force" garantit qu'aucune
 * entité fantôme ne subsiste, quelle que soit la transition de type
 * (ex: un objet qui passe de sensor à number après découverte de la
 * commandabilité).
 *
 * Si t_did == 0 (cleanup global), les entités Gateway (B2M) sont aussi supprimées.
 *
 * @param t_did       Device ID cible (0 = tous, incluant la Gateway).
 * @param t_inst      Instance cible (0xFFFFFFFF = toutes).
 * @param t_type      Type cible (0xFFFF = tous).
 * @param old_prefix  Préfixe MQTT à utiliser pour le nettoyage (NULL = préfixe courant).
 *
 * @note Le mutex est maintenu pendant tout le parcours car les publications
 *       de payloads vides sont très rapides (pas de sérialisation JSON).
 */
void unpublish_ha_discovery(uint32_t t_did, uint32_t t_inst, uint16_t t_type, const char* old_prefix) {
    if (!mqtt_is_connected || circuit_breaker_active) return;
    const char* prefix = old_prefix ? old_prefix : sysCfg.mqtt_prefix;
    z_log(pdLOG_INFO, "MQTT", "Cleaning up HA Discovery (Prefix: %s)...\n", prefix);

    /* Nettoyage des entités Gateway si cleanup global. */
    if (t_did == 0) {
        auto unpub_gw = [&](const char* key, bool is_binary = false) {
            char topic[128], uniq[64];
            snprintf(uniq, sizeof(uniq), "b2m_gw_%s", key);
            snprintf(topic, sizeof(topic), "homeassistant/%s/%s/config", is_binary ? "binary_sensor" : "sensor", uniq);
            esp_mqtt_client_publish(mqtt_client, topic, "", 0, 1, 1);
        };
        unpub_gw("ver"); unpub_gw("uptime"); unpub_gw("rssi"); unpub_gw("heap");
        unpub_gw("min_heap"); unpub_gw("temp"); unpub_gw("nb_dev"); unpub_gw("mstp", true);
    }

    /* Parcours de tous les objets sous mutex pour publier des payloads vides
     * sur les 5 composants HA possibles par objet. Le délai de 10ms entre
     * chaque objet évite de saturer l'outbox ESP-MQTT. */
    if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(500))) {
        for (auto& dev : bacnet_network_cache) {
            if (t_did != 0 && dev.ulDeviceId != t_did) continue;
            for (auto& obj : dev.objects) {
                if (t_inst != 0xFFFFFFFF && obj.ulInstance != t_inst) continue;
                if (t_type != 0xFFFF && obj.usType != t_type) continue;

                const char* t_str = "OBJ";
                switch(obj.usType) {
                    case OBJ_ANALOG_INPUT:       t_str = "AI"; break;
                    case OBJ_ANALOG_OUTPUT:      t_str = "AO"; break;
                    case OBJ_ANALOG_VALUE:       t_str = "AV"; break;
                    case OBJ_BINARY_INPUT:       t_str = "BI"; break;
                    case OBJ_BINARY_OUTPUT:      t_str = "BO"; break;
                    case OBJ_BINARY_VALUE:       t_str = "BV"; break;
                    case OBJ_MULTI_STATE_INPUT:  t_str = "MSI"; break;
                    case OBJ_MULTI_STATE_OUTPUT: t_str = "MSO"; break;
                    case OBJ_MULTI_STATE_VALUE:  t_str = "MSV"; break;
                }
                char uniq_id[64];
                snprintf(uniq_id, sizeof(uniq_id), "bacnet_%lu_%s_%lu", (unsigned long)dev.ulDeviceId, t_str, (unsigned long)obj.ulInstance);
                
                /* Publication d'un payload vide sur chaque composant HA possible.
                 * C'est la méthode recommandée par HA pour retirer une entité :
                 * un message retained vide sur le topic de config la supprime. */
                const char* components[] = {"sensor", "binary_sensor", "switch", "number", "select"};
                for (const char* comp : components) {
                    char topic[128];
                    snprintf(topic, sizeof(topic), "homeassistant/%s/%s/config", comp, uniq_id);
                    esp_mqtt_client_publish(mqtt_client, topic, "", 0, 1, 1);
                }

                // AJOUT CHIRURGICAL : Supprimer également les 5 sensors de status_flags lors du nettoyage complet (et nettoyer les anciens binary_sensors)
                const char* flags[] = {"alarm", "fault", "overridden", "oos", "overridden_bacnet"};
                for (const char* flag : flags) {
                    char flag_topic_bin[128];
                    snprintf(flag_topic_bin, sizeof(flag_topic_bin), "homeassistant/binary_sensor/bacnet_%lu_%s_%lu_%s/config",
                             (unsigned long)dev.ulDeviceId, t_str, (unsigned long)obj.ulInstance, flag);
                    esp_mqtt_client_publish(mqtt_client, flag_topic_bin, "", 0, 1, 1);

                    char flag_topic_sen[128];
                    snprintf(flag_topic_sen, sizeof(flag_topic_sen), "homeassistant/sensor/bacnet_%lu_%s_%lu_%s/config",
                             (unsigned long)dev.ulDeviceId, t_str, (unsigned long)obj.ulInstance, flag);
                    esp_mqtt_client_publish(mqtt_client, flag_topic_sen, "", 0, 1, 1);
                }
                
                // AJOUT CHIRURGICAL : Supprimer également le bouton Reset s'il existe
                if (obj.xIsCommandable) {
                    char reset_topic[128];
                    snprintf(reset_topic, sizeof(reset_topic), "homeassistant/button/bacnet_%lu_%s_%lu_reset/config",
                             (unsigned long)dev.ulDeviceId, t_str, (unsigned long)obj.ulInstance);
                    esp_mqtt_client_publish(mqtt_client, reset_topic, "", 0, 1, 1);
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        xSemaphoreGive(cache_mutex);
    }
}
