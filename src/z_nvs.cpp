/**
 * @file z_nvs.cpp
 * @brief Couche de persistance NVS (Non-Volatile Storage) pour le projet BACnet2MQTT.
 *
 * Ce module gère la sérialisation/désérialisation de toutes les données
 * persistantes du système dans la partition NVS de l'ESP32 :
 *  - **Configuration système** (WiFi, MQTT, BACnet, Home Assistant).
 *  - **Base de données des équipements BACnet** et de leurs objets.
 *  - **Labels d'états** des objets Multi-State (MSI/MSO/MSV).
 *
 * Architecture de stockage NVS :
 *  ┌─────────────────────────────────────────────────────────────────┐
 *  │ Namespace "system"   │ Configuration globale (sysCfg)           │
 *  │ Namespace "registry" │ Liste des Device IDs (chaîne séparée ;) │
 *  │ Namespace "dv_<DID>" │ Entête + pages d'objets par équipement  │
 *  │ Namespace "st_<DID>" │ Labels d'états MSI/MSO/MSV par device   │
 *  └─────────────────────────────────────────────────────────────────┘
 *
 * Pagination des objets :
 *  La limite NVS ESP-IDF est de ~1984 octets par blob. Les objets sont
 *  donc découpés en pages de 20 (20 × 95 octets = 1900 octets < 1984).
 *  Chaque page est stockée sous la clé "p0", "p1", etc.
 *
 * Stratégie de sauvegarde différée (Lazy Save) :
 *  Pour éviter l'usure prématurée de la flash, les modifications d'objets
 *  positionnent un drapeau xDirty sur le device. La sauvegarde effective
 *  est déclenchée ultérieurement par le système (via save_device_objects_locked).
 *
 * Gestion de la concurrence :
 *  - cache_mutex : Protège l'accès au vecteur bacnet_network_cache (RAM).
 *  - nvs_mutex   : Protège les écritures NVS pour éviter les corruptions
 *                  si plusieurs tâches tentent d'écrire simultanément.
 *  Le pattern "copie locale puis écriture" est utilisé dans
 *  save_device_objects_locked pour minimiser le temps de verrouillage
 *  du cache_mutex pendant les opérations NVS (lentes, ~10-50 ms).
 *
 * Dépendances :
 *  - z_nvs.h     : Prototypes publics de ce module.
 *  - z_network.h : Fonction z_log() pour la journalisation.
 *  - z_bacnet.h  : Structures BACnetDevice, BACnetObject, BACnetPersistence*.
 *  - z_mqtt.h    : Accès à ha_dependencies pour les références croisées HA.
 *  - Preferences : Wrapper Arduino ESP32 autour de l'API nvs_flash ESP-IDF.
 */

#include "z_nvs.h"
#include "z_network.h"
#include "z_bacnet.h"
#include "z_mqtt.h"
#include <Preferences.h>
#include <algorithm>

/** @brief Cache RAM des équipements BACnet découverts (déclaré dans z_bacnet.cpp). */
extern std::vector<BACnetDevice> bacnet_network_cache;

/** @brief Mutex de protection du cache RAM (déclaré dans z_bacnet.cpp). */
extern SemaphoreHandle_t cache_mutex;

/**
 * @brief Mutex dédié aux opérations NVS.
 *
 * Sépare la protection des écritures flash de celle du cache RAM.
 * Créé paresseusement (lazy init) au premier appel de load_device_objects
 * ou save_device_objects_locked.
 */
static SemaphoreHandle_t nvs_mutex = NULL;

// ============================================================================
// Chargement des objets d'un équipement depuis la flash
// ============================================================================

/**
 * @brief Restaure un équipement BACnet et tous ses objets depuis la NVS.
 *
 * Lit l'entête du device (BACnetPersistenceDev) puis itère sur les pages
 * d'objets (BACnetPersistencePage, 20 objets/page) pour reconstruire
 * le BACnetDevice complet en RAM.
 *
 * Processus détaillé :
 *  1. Ouverture du namespace NVS "dv_<ulDeviceId>" en lecture seule.
 *  2. Lecture du blob "head" contenant les métadonnées du device.
 *  3. Vérification d'absence de doublon dans bacnet_network_cache.
 *  4. Lecture des pages "p0", "p1", ... contenant les objets sérialisés.
 *  5. Décodage du champ ulVal : bits [31:22] = type, bits [21:0] = instance.
 *  6. Reconstruction des dépendances Home Assistant (cMinRef/cMaxRef).
 *  7. Chargement des labels d'états pour les objets Multi-State.
 *
 * @param ulDeviceId Identifiant unique BACnet du device à restaurer.
 *
 * @note Cette fonction prend cache_mutex avec un timeout de 1000 ms.
 *       Si le mutex n'est pas disponible, le device n'est pas chargé.
 * @note Les objets Multi-State (MSI/MSO/MSV) déclenchent un appel
 *       supplémentaire à load_object_states() pour les labels d'états.
 */
void load_device_objects(uint32_t ulDeviceId) {
    if (nvs_mutex == NULL) nvs_mutex = xSemaphoreCreateMutex();
    char ns[16]; 
    snprintf(ns, sizeof(ns), "dv_%lu", (unsigned long)ulDeviceId); // Namespace standard
    Preferences prefs;
    
    if (prefs.begin(ns, true)) {
        BACnetPersistenceDev head;
        if (prefs.getBytes("head", &head, sizeof(head)) == sizeof(head)) {
            
            if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(1000))) {
                bool exists = false;
                for(auto& d : bacnet_network_cache) if(d.ulDeviceId == ulDeviceId) exists = true;
                
                if (!exists) {
                    BACnetDevice dev;
                    dev.ulDeviceId = head.ulDeviceId;
                    dev.ucMacAddress = head.ucMacAddress;
                    dev.xEnabled = head.xEnabled;
                    dev.name = String(head.cName);
                    dev.vendor = String(head.cVendor);
                    dev.xDiscoveryDone = head.xDiscoveryDone;
                    dev.ucDiscStep = (DISC_STEP_T)head.ucDiscStep;
                    dev.usDiscObjIdx = head.usDiscObjIdx;

                    // Restauration des paramètres APDU du device distant.
                    // Fallback sur les valeurs par défaut si le champ n'était pas
                    // encore présent dans une ancienne version du firmware (migration).
                    dev.usMaxApduLengthAccepted = head.usMaxApduLengthAccepted > 0 ? head.usMaxApduLengthAccepted : 480;
                    dev.ulApduTimeout = head.ulApduTimeout > 0 ? head.ulApduTimeout : 3000;
                    dev.ucNumberOfApduRetries = head.ucNumberOfApduRetries;
                    dev.xSupportsRpm = head.xSupportsRpm;

                    dev.last_seen = millis();

                    // Itération sur les pages d'objets (20 objets par page max).
                    // L'index p représente le numéro de page, chaque page couvre
                    // les objets [p*20 .. (p+1)*20-1].
                    for (int p = 0; p * 20 < head.usCount; p++) {
                        char key[16]; snprintf(key, 16, "p%d", p);
                        BACnetPersistencePage page;
                        memset(&page, 0, sizeof(page));
                        
                        if (prefs.getBytes(key, &page, sizeof(page)) == sizeof(page)) {
                            // Vérification d'intégrité : le Device ID dans la page
                            // doit correspondre au device en cours de chargement.
                            if (page.ulDeviceId != ulDeviceId) continue;

                            for (int i = 0; i < 20 && (p * 20 + i) < head.usCount; i++) {
                                BACnetObject obj;
                                // Décodage de l'identifiant BACnet compact :
                                // Bits [31:22] = Type d'objet (10 bits, max 1023)
                                // Bits [21:0]  = Numéro d'instance (22 bits, max 4194303)
                                obj.usType = page.objects[i].ulVal >> 22;
                                obj.ulInstance = page.objects[i].ulVal & 0x3FFFFF;
                                obj.xEnabled = page.objects[i].xEnabled;
                                obj.xNamePublished = page.objects[i].xNamePublished;
                                obj.xIsCommandable = page.objects[i].xIsCommandable;
                                obj.usUnits = page.objects[i].usUnits;
                                obj.ucExpectedStatesCount = page.objects[i].ucExpectedStatesCount;
                                obj.fMinValue = page.objects[i].fMinValue;
                                obj.fMaxValue = page.objects[i].fMaxValue;
                                obj.fStepValue = page.objects[i].fStepValue;
                                
                                // AJOUT CHIRURGICAL : Restauration des Status_Flags depuis la flash
                                obj.ucStatusFlags = page.objects[i].ucStatusFlags;
                                
                                strlcpy(obj.cMinRef, page.objects[i].cMinRef, sizeof(obj.cMinRef));
                                strlcpy(obj.cMaxRef, page.objects[i].cMaxRef, sizeof(obj.cMaxRef));
                                
                                strlcpy(obj.cName, page.objects[i].cName, sizeof(obj.cName));
                                strlcpy(obj.cUnitText, page.objects[i].cUnitText, sizeof(obj.cUnitText));
                                strlcpy(obj.cLastMqttName, page.objects[i].cName, sizeof(obj.cLastMqttName));
                                strlcpy(obj.cLastHaComponent, page.objects[i].cLastHaComponent, sizeof(obj.cLastHaComponent));
                                
                                // v6.6.1: Charger les labels des états MSI/MSO/MSV
                                // Ces labels sont stockés dans un namespace séparé "st_<DID>"
                                // pour ne pas gonfler la taille des pages d'objets.
                                if (obj.usType == OBJ_MULTI_STATE_INPUT || obj.usType == OBJ_MULTI_STATE_OUTPUT || obj.usType == OBJ_MULTI_STATE_VALUE) {
                                    load_object_states(ulDeviceId, obj.usType, obj.ulInstance, obj.state_texts);
                                }

                                // La valeur courante n'est pas persistée ; elle sera
                                // lue depuis le bus BACnet au prochain cycle de polling.
                                // v7.0.12: Initialisation à NAN pour forcer la publication MQTT au premier poll
                                obj.fPresentValue = NAN;
                                dev.objects.push_back(obj);
                                
                                // Reconstruction de la table de dépendances Home Assistant.
                                // cMinRef/cMaxRef contiennent des références croisées vers
                                // d'autres objets dont la valeur sert de borne min/max
                                // pour l'entité HA 'number'. Quand la valeur de l'objet
                                // référencé change, les entités dépendantes sont republier.
                                if (strlen(obj.cMinRef) > 0) {
                                    String key = String(ulDeviceId) + "_" + String(obj.cMinRef);
                                    ha_dependencies[key].push_back({obj.usType, obj.ulInstance});
                                }
                                if (strlen(obj.cMaxRef) > 0) {
                                    String key = String(ulDeviceId) + "_" + String(obj.cMaxRef);
                                    ha_dependencies[key].push_back({obj.usType, obj.ulInstance});
                                }
                            }
                        }
                    }
                    dev.last_seen = millis();   
                    bacnet_network_cache.push_back(dev);
                    z_log(pdLOG_INFO, "NVS", "[NVS] Restored Device %lu (%d objs)\n", (unsigned long)ulDeviceId, (int)dev.objects.size());
                }
                xSemaphoreGive(cache_mutex);
            }
        }
        prefs.end();
    }
}

// ============================================================================
// Chargement de la configuration système
// ============================================================================

/**
 * @brief Charge la configuration complète du système depuis la NVS.
 *
 * Initialise d'abord la structure sysCfg avec les valeurs par défaut
 * (définies dans z_config.h), puis surcharge chaque champ avec la valeur
 * sauvegardée en NVS si elle existe (pattern isKey → get).
 *
 * Après le chargement de la configuration, lit le registre des devices
 * ("registry" / "dev_list") et restaure chaque équipement via
 * load_device_objects().
 *
 * Format du registre des devices :
 *  Chaîne de Device IDs séparés par des points-virgules.
 *  Exemple : "123001;456002;789003"
 *
 * @note Les clés NVS sont volontairement courtes (2-4 caractères) pour
 *       respecter la limite de 15 caractères des clés NVS ESP-IDF et
 *       minimiser l'espace consommé dans la partition.
 */
void load_configuration() {
    // --- Initialisation des valeurs par défaut ---
    // Ces valeurs sont utilisées si aucune configuration n'a jamais été
    // sauvegardée (premier démarrage) ou si une clé spécifique est absente.
    strlcpy(sysCfg.wifi_ssid, DEFAULT_SSID, 32);
    strlcpy(sysCfg.wifi_pass, "", 64);
    sysCfg.static_ip = false;
    strlcpy(sysCfg.local_ip, DEFAULT_STATIC_IP, 16);
    strlcpy(sysCfg.gateway, DEFAULT_GATEWAY, 16);
    strlcpy(sysCfg.subnet, DEFAULT_SUBNET, 16);
    sysCfg.ucMacAddress = DEFAULT_MAC_ADDRESS;
    sysCfg.max_master = DEFAULT_MAX_MASTER;
    sysCfg.ulDeviceId = DEFAULT_DEVICE_ID;
    sysCfg.ulApduTimeout = DEFAULT_APDU_TIMEOUT;
    sysCfg.max_retries = DEFAULT_MAX_RETRIES;
    strlcpy(sysCfg.mqtt_server, DEFAULT_MQTT_SERVER, 32);
    sysCfg.mqtt_port = 1883;
    strlcpy(sysCfg.mqtt_user, "", 32);
    strlcpy(sysCfg.mqtt_pass, "", 32);
    strlcpy(sysCfg.mqtt_prefix, "bacnet", 64);
    sysCfg.heartbeat_interval = DEFAULT_HEARBEAT_INTERVAL;
    sysCfg.mqtt_poll_interval = DEFAULT_MQTT_POLL;
    sysCfg.bacnet_poll_interval = DEFAULT_BACNET_POLL;
    sysCfg.token_skip = DEFAULT_TOKEN_SKIP;
    sysCfg.log_level = pdLOG_INFO;
    sysCfg.max_info_frames = DEFAULT_MAX_INFO_FRAMES;
    sysCfg.ha_discover = DEFAULT_HA_DISCOVER;
    sysCfg.default_number_min = DEFAULT_NUM_MIN;
    sysCfg.default_number_max = DEFAULT_NUM_MAX;
    sysCfg.default_number_step = DEFAULT_NUM_STEP;
    strlcpy(sysCfg.admin_user, "admin", 32);
    strlcpy(sysCfg.admin_pass, "admin1234", 64);

    // --- Surcharge depuis la NVS (namespace "system", lecture seule) ---
    Preferences prefs;
    if (prefs.begin("system", true)) {
        // Paramètres WiFi
        if (prefs.isKey("ssid")) prefs.getString("ssid", sysCfg.wifi_ssid, 32);
        if (prefs.isKey("pass")) prefs.getString("pass", sysCfg.wifi_pass, 64);
        if (prefs.isKey("static")) sysCfg.static_ip = prefs.getBool("static", false);
        if (prefs.isKey("ip")) prefs.getString("ip", sysCfg.local_ip, 16);
        if (prefs.isKey("gw")) prefs.getString("gw", sysCfg.gateway, 16);
        if (prefs.isKey("sn")) prefs.getString("sn", sysCfg.subnet, 16);
        
        // Paramètres BACnet MS/TP
        if (prefs.isKey("mac")) sysCfg.ucMacAddress = prefs.getUChar("mac", DEFAULT_MAC_ADDRESS);
        if (prefs.isKey("mm")) sysCfg.max_master = prefs.getUChar("mm", DEFAULT_MAX_MASTER);
        if (prefs.isKey("did")) sysCfg.ulDeviceId = prefs.getUInt("did", DEFAULT_DEVICE_ID);
        if (prefs.isKey("to")) sysCfg.ulApduTimeout = prefs.getUShort("to", DEFAULT_APDU_TIMEOUT);
        if (prefs.isKey("ret")) sysCfg.max_retries = prefs.getUChar("ret", DEFAULT_MAX_RETRIES);
        
        // Paramètres MQTT
        if (prefs.isKey("mqh")) prefs.getString("mqh", sysCfg.mqtt_server, 32);
        if (prefs.isKey("mprt")) sysCfg.mqtt_port = prefs.getUInt("mprt", 1883);
        if (prefs.isKey("mqu")) prefs.getString("mqu", sysCfg.mqtt_user, 32);
        if (prefs.isKey("mqp")) prefs.getString("mqp", sysCfg.mqtt_pass, 32);
        if (prefs.isKey("mqpr")) prefs.getString("mqpr", sysCfg.mqtt_prefix, 64);
        
        // Paramètres de timing et polling
        if (prefs.isKey("hbeat")) sysCfg.heartbeat_interval = prefs.getUInt("hbeat", DEFAULT_HEARBEAT_INTERVAL);
        if (prefs.isKey("mpi")) sysCfg.mqtt_poll_interval = prefs.getUShort("mpi", DEFAULT_MQTT_POLL);
        if (prefs.isKey("bpi")) sysCfg.bacnet_poll_interval = prefs.getUShort("bpi", DEFAULT_BACNET_POLL);
        if (prefs.isKey("tskip")) sysCfg.token_skip = prefs.getUChar("tskip", DEFAULT_TOKEN_SKIP);
        if (prefs.isKey("mif")) sysCfg.max_info_frames = prefs.getUChar("mif", DEFAULT_MAX_INFO_FRAMES);
        
        // Paramètres d'administration et logs
        if (prefs.isKey("adu")) prefs.getString("adu", sysCfg.admin_user, 32);
        if (prefs.isKey("adp")) prefs.getString("adp", sysCfg.admin_pass, 64);
        if (prefs.isKey("lvl")) sysCfg.log_level = prefs.getUChar("lvl", pdLOG_INFO);

        // Paramètres Home Assistant
        if (prefs.isKey("ha_disc")) sysCfg.ha_discover = prefs.getBool("ha_disc", DEFAULT_HA_DISCOVER);
        if (prefs.isKey("n_min")) sysCfg.default_number_min = prefs.getFloat("n_min", DEFAULT_NUM_MIN);
        if (prefs.isKey("n_max")) sysCfg.default_number_max = prefs.getFloat("n_max", DEFAULT_NUM_MAX);
        if (prefs.isKey("n_stp")) sysCfg.default_number_step = prefs.getFloat("n_stp", DEFAULT_NUM_STEP);
        
        prefs.end();
    }
    z_log(pdLOG_INFO, "NVS", "[NVS] Configuration Loaded\n");

    // --- Restauration de tous les équipements BACnet enregistrés ---
    // Le registre "dev_list" contient une chaîne de Device IDs séparés
    // par des ';'. On parse cette chaîne et on restaure chaque device.
    Preferences reg;
    String dev_list = "";
    if (reg.begin("registry", true)) {
        dev_list = reg.getString("dev_list", "");
        reg.end();
    }

    if (dev_list.length() > 0) {
        // Parsing manuel de la chaîne "DID1;DID2;DID3" sans strtok
        // (strtok n'est pas thread-safe et modifie la chaîne source).
        int start = 0;
        int end = dev_list.indexOf(';');
        while (start < dev_list.length()) {
            String sid = (end == -1) ? dev_list.substring(start) : dev_list.substring(start, end);
            sid.trim();
            if (sid.length() > 0) {
                load_device_objects(sid.toInt());
            }
            if (end == -1) break;
            start = end + 1;
            end = dev_list.indexOf(';', start);
        }
    }
}

// ============================================================================
// Sauvegarde de la configuration système
// ============================================================================

/**
 * @brief Sauvegarde la totalité de la configuration système dans la NVS.
 *
 * Écrit tous les champs de la structure sysCfg dans le namespace "system".
 * Appelée typiquement depuis l'interface Web après modification des
 * paramètres par l'utilisateur.
 *
 * @note Chaque appel provoque de multiples écritures flash. Cette fonction
 *       ne doit pas être appelée en boucle rapide (risque d'usure flash).
 * @note Les clés NVS sont limitées à 15 caractères (contrainte ESP-IDF).
 */
void save_configuration() {
    Preferences prefs;
    if (prefs.begin("system", false)) {
        // Paramètres WiFi
        prefs.putString("ssid", sysCfg.wifi_ssid);
        prefs.putString("pass", sysCfg.wifi_pass);
        prefs.putBool("static", sysCfg.static_ip);
        prefs.putString("ip", sysCfg.local_ip);
        prefs.putString("gw", sysCfg.gateway);
        prefs.putString("sn", sysCfg.subnet);
        
        // Paramètres BACnet MS/TP
        prefs.putUChar("mac", sysCfg.ucMacAddress);
        prefs.putUChar("mm", sysCfg.max_master);
        prefs.putUInt("did", sysCfg.ulDeviceId);
        prefs.putUShort("to", sysCfg.ulApduTimeout);
        prefs.putUChar("ret", sysCfg.max_retries);
        
        // Paramètres MQTT
        prefs.putString("mqh", sysCfg.mqtt_server);
        prefs.putUInt("mprt", sysCfg.mqtt_port);
        prefs.putString("mqu", sysCfg.mqtt_user);
        prefs.putString("mqp", sysCfg.mqtt_pass);
        prefs.putString("mqpr", sysCfg.mqtt_prefix);
        
        // Paramètres de timing et polling
        prefs.putUInt("hbeat", sysCfg.heartbeat_interval);
        prefs.putUShort("mpi", sysCfg.mqtt_poll_interval);
        prefs.putUShort("bpi", sysCfg.bacnet_poll_interval);
        prefs.putUChar("tskip", sysCfg.token_skip);
        prefs.putUChar("mif", sysCfg.max_info_frames);
        
        // Paramètres d'administration et Home Assistant
        prefs.putString("adu", sysCfg.admin_user);
        prefs.putString("adp", sysCfg.admin_pass);
        prefs.putUChar("lvl", sysCfg.log_level);
        prefs.putBool("ha_disc", sysCfg.ha_discover);
        prefs.putFloat("n_min", sysCfg.default_number_min);
        prefs.putFloat("n_max", sysCfg.default_number_max);
        prefs.putFloat("n_stp", sysCfg.default_number_step);
        
        prefs.end();
    }
    z_log(pdLOG_INFO, "NVS", "[NVS] Configuration System Saved\n");
}

// ============================================================================
// Marquage d'un device pour sauvegarde différée (Lazy Save)
// ============================================================================

/**
 * @brief Marque un équipement comme 'sale' pour déclencher une sauvegarde différée (Lazy Save).
 *
 * Au lieu de sauvegarder immédiatement en flash (opération lente et usante),
 * on positionne le drapeau xDirty et le timestamp ulLastDirtyTime. Le système
 * vérifie périodiquement ces drapeaux et appelle save_device_objects_locked()
 * quand suffisamment de temps s'est écoulé depuis la dernière modification.
 *
 * @param ulDeviceId Identifiant BACnet unique de l'équipement à marquer.
 *
 * @note Le timeout du mutex est court (100 ms) car cette fonction est
 *       appelée fréquemment depuis le contexte de polling BACnet.
 */
void save_device_objects(uint32_t ulDeviceId) {
    if (cache_mutex == NULL) return;
    if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(100))) {
        for (auto& dev : bacnet_network_cache) {
            if (dev.ulDeviceId == ulDeviceId) {
                dev.xDirty = true;
                dev.ulLastDirtyTime = millis();
                break;
            }
        }
        xSemaphoreGive(cache_mutex);
    }
}

// ============================================================================
// Sauvegarde effective d'un device en flash (appelée par le système)
// ============================================================================

/**
 * @brief Sérialise et écrit un équipement BACnet complet dans la NVS.
 *
 * Cette fonction effectue la sauvegarde réelle en flash. Elle utilise un
 * pattern "copie-puis-écriture" en deux phases :
 *
 *  **Phase 1 — Copie locale (sous cache_mutex)** :
 *   - Copie les métadonnées du device dans une structure BACnetPersistenceDev.
 *   - Effectue une copie profonde (deep copy) du vecteur d'objets.
 *   - Réinitialise le drapeau xDirty à false.
 *   - Durée : ~1 ms (opérations mémoire uniquement).
 *
 *  **Phase 2 — Écriture NVS (sous nvs_mutex, sans cache_mutex)** :
 *   - Efface le namespace "dv_<DID>" puis réécrit l'entête et les pages.
 *   - Met à jour le registre "dev_list" si le device n'y figure pas encore.
 *   - Durée : ~10-50 ms selon la taille et l'état de la partition NVS.
 *
 * @param ulDeviceId Identifiant BACnet unique de l'équipement à sauvegarder.
 *
 * @note Le nom "locked" est historique — la fonction gère elle-même ses mutex.
 *       Le cache_mutex n'est PAS pré-verrouillé par l'appelant.
 * @warning prefs.clear() efface TOUT le namespace avant réécriture. C'est
 *          nécessaire pour éviter les pages orphelines si le nombre d'objets
 *          a diminué depuis la dernière sauvegarde.
 */
void save_device_objects_locked(uint32_t ulDeviceId) {
    // === Phase 1 : Copie locale sous cache_mutex ===
    bool found = false;
    BACnetPersistenceDev head;
    std::vector<BACnetObject> local_objects;

    if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(500))) {
        for (auto& dev : bacnet_network_cache) {
            if (dev.ulDeviceId == ulDeviceId) {
                dev.xDirty = false;
                found = true;
                
                memset(&head, 0, sizeof(head));
                head.ulDeviceId = dev.ulDeviceId;
                head.ucMacAddress = dev.ucMacAddress;
                head.xEnabled = dev.xEnabled;
                head.usCount = (uint16_t)dev.objects.size();
                head.xDiscoveryDone = dev.xDiscoveryDone;
                head.ucDiscStep = (uint8_t)dev.ucDiscStep;
                head.usDiscObjIdx = dev.usDiscObjIdx;
                strlcpy(head.cName, dev.name.c_str(), 32);
                strlcpy(head.cVendor, dev.vendor.c_str(), 32);
                head.usMaxApduLengthAccepted = dev.usMaxApduLengthAccepted;
                head.ulApduTimeout = dev.ulApduTimeout;
                head.ucNumberOfApduRetries = dev.ucNumberOfApduRetries;
                head.xSupportsRpm = dev.xSupportsRpm;

                local_objects = dev.objects; // Deep copy
                break;
            }
        }
        xSemaphoreGive(cache_mutex);
    }

    if (!found) return;

    // === Phase 2 : Écriture NVS sous nvs_mutex (cache_mutex libéré) ===
    if (nvs_mutex == NULL) nvs_mutex = xSemaphoreCreateMutex();
    if (xSemaphoreTake(nvs_mutex, pdMS_TO_TICKS(1000))) {
        char ns[16];
        snprintf(ns, sizeof(ns), "dv_%lu", (unsigned long)ulDeviceId); // Namespace standard
        Preferences prefs;

        if (prefs.begin(ns, false)) {
            // Effacement complet du namespace pour éviter les pages orphelines
            // (cas où le nombre d'objets a diminué depuis la dernière sauvegarde).
            prefs.clear();
            prefs.putBytes("head", &head, sizeof(head));

            // Sérialisation des objets par pages de 20.
            // Encodage compact de l'identifiant : (type << 22) | (instance & 0x3FFFFF).
            for (int p = 0; p * 20 < (int)local_objects.size(); p++) {
                BACnetPersistencePage page;
                memset(&page, 0, sizeof(page));
                page.ulDeviceId = ulDeviceId;
                page.page_index = (uint16_t)p;

                for (int i = 0; i < 20 && (p * 20 + i) < (int)local_objects.size(); i++) {
                    auto& o = local_objects[p * 20 + i];
                    page.objects[i].ulVal = ((uint32_t)o.usType << 22) | (o.ulInstance & 0x3FFFFF);
                    page.objects[i].xEnabled = o.xEnabled;
                    page.objects[i].xNamePublished = o.xNamePublished;
                    page.objects[i].xIsCommandable = o.xIsCommandable;
                    page.objects[i].usUnits = o.usUnits;
                    page.objects[i].ucExpectedStatesCount = (uint8_t)std::min((int)o.ucExpectedStatesCount, 255);
                    page.objects[i].fMinValue = o.fMinValue;
                    page.objects[i].fMaxValue = o.fMaxValue;
                    page.objects[i].fStepValue = o.fStepValue;
                    
                    // AJOUT CHIRURGICAL : Sauvegarde des Status_Flags en mémoire non volatile
                    page.objects[i].ucStatusFlags = o.ucStatusFlags;
                    
                    strlcpy(page.objects[i].cMinRef, o.cMinRef, sizeof(page.objects[i].cMinRef));
                    strlcpy(page.objects[i].cMaxRef, o.cMaxRef, sizeof(page.objects[i].cMaxRef));
                    strlcpy(page.objects[i].cName, o.cName, 32);
                    strlcpy(page.objects[i].cUnitText, o.cUnitText, 12);
                    strlcpy(page.objects[i].cLastHaComponent, o.cLastHaComponent, 16);
                }
                char key[16]; snprintf(key, 16, "p%d", p);
                prefs.putBytes(key, &page, sizeof(page));
            }
            prefs.end();

            // Mise à jour du registre global des devices.
            // Ajout du Device ID à la liste "dev_list" s'il n'y figure pas encore.
            // Format : "DID1;DID2;DID3" (recherche par indexOf pour éviter les doublons).
            Preferences reg;
            if (reg.begin("registry", false)) {
                String list = reg.getString("dev_list", "");
                if (list.indexOf(String(ulDeviceId)) == -1) {
                    list += (list.length() > 0 ? ";" : "") + String(ulDeviceId);
                    reg.putString("dev_list", list);
                }
                reg.end();
            }
            z_log(pdLOG_INFO, "NVS", "Device %lu saved to Flash (Lazy Save)\n", (unsigned long)ulDeviceId);
        }
        xSemaphoreGive(nvs_mutex);
    }
}

// ============================================================================
// Gestion des labels d'états Multi-State (MSI/MSO/MSV)
// ============================================================================

/**
 * @brief Sauvegarde les labels d'états d'un objet Multi-State en NVS.
 *
 * Les labels sont concaténés en une seule chaîne séparée par des '|'
 * (ex: "Off|Low|Medium|High") pour minimiser le nombre de clés NVS.
 * Stockés dans un namespace dédié "st_<DID>" pour ne pas impacter
 * la taille des pages d'objets dans "dv_<DID>".
 *
 * @param ulDeviceId Identifiant BACnet du device propriétaire.
 * @param type       Type d'objet BACnet (OBJ_MULTI_STATE_INPUT, _OUTPUT, _VALUE).
 * @param ulInstance Numéro d'instance de l'objet.
 * @param states     Vecteur de labels (index 0 = état 1, conforme BACnet).
 *
 * @note Clé NVS : "o<type>_<instance>" (ex: "o13_42" pour MSI instance 42).
 */
void save_object_states(uint32_t ulDeviceId, uint16_t type, uint32_t ulInstance, const std::vector<String>& states) {
    if (states.empty()) return;
    
    char ns[16]; snprintf(ns, sizeof(ns), "st_%lu", (unsigned long)ulDeviceId);
    char key[16]; snprintf(key, sizeof(key), "o%u_%lu", type, (unsigned long)ulInstance);
    
    // Concaténation des labels avec '|' comme séparateur.
    // Ce format permet de stocker un nombre variable de labels dans
    // une seule clé NVS de type String.
    String combined = "";
    for (size_t i = 0; i < states.size(); i++) {
        combined += states[i];
        if (i < states.size() - 1) combined += "|";
    }
    
    Preferences prefs;
    if (prefs.begin(ns, false)) {
        prefs.putString(key, combined);
        prefs.end();
        z_log(pdLOG_DEBUG, "NVS", "[NVS] Saved States for %u:%lu (%d labels)\n", type, ulInstance, (int)states.size());
    }
}

/**
 * @brief Charge les labels d'états d'un objet Multi-State depuis la NVS.
 *
 * Inverse de save_object_states() : lit la chaîne concaténée et la
 * découpe sur le séparateur '|' pour reconstruire le vecteur de labels.
 *
 * @param ulDeviceId Identifiant BACnet du device propriétaire.
 * @param type       Type d'objet BACnet (OBJ_MULTI_STATE_INPUT, _OUTPUT, _VALUE).
 * @param ulInstance Numéro d'instance de l'objet.
 * @param[out] states Vecteur de labels à remplir (vidé puis reconstruit).
 *
 * @note Si la clé n'existe pas en NVS, le vecteur states n'est pas modifié
 *       (conserve ses valeurs par défaut, typiquement vide).
 */
void load_object_states(uint32_t ulDeviceId, uint16_t type, uint32_t ulInstance, std::vector<String>& states) {
    char ns[16]; snprintf(ns, sizeof(ns), "st_%lu", (unsigned long)ulDeviceId);
    char key[16]; snprintf(key, sizeof(key), "o%u_%lu", type, (unsigned long)ulInstance);
    
    Preferences prefs;
    if (prefs.begin(ns, true)) {
        String combined = prefs.getString(key, "");
        if (combined.length() > 0) {
            states.clear();
            // Parsing manuel de la chaîne "label1|label2|label3".
            // On ne peut pas utiliser strtok car String Arduino n'expose
            // pas de c_str() mutable de manière fiable.
            int start = 0;
            int end = combined.indexOf('|');
            while (end != -1) {
                states.push_back(combined.substring(start, end));
                start = end + 1;
                end = combined.indexOf('|', start);
            }
            // Dernier élément après le dernier '|' (ou unique élément sans '|').
            states.push_back(combined.substring(start));
        }
        prefs.end();
        }
        }


