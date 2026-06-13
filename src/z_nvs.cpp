#include "z_nvs.h"
#include "z_network.h"
#include "z_bacnet.h"
#include "z_mqtt.h"
#include <Preferences.h>
#include <algorithm>

extern std::vector<BACnetDevice> bacnet_network_cache;
extern SemaphoreHandle_t cache_mutex;

static SemaphoreHandle_t nvs_mutex = NULL;

/**
 * Charge les objets d'un équipement depuis la mémoire flash (NVS).
 * Utilise cache_mutex pour éviter les accès concurrents.
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

                    // Nouveaux champs (avec fallback si non présents/valides)
                    dev.usMaxApduLengthAccepted = head.usMaxApduLengthAccepted > 0 ? head.usMaxApduLengthAccepted : 480;
                    dev.ulApduTimeout = head.ulApduTimeout > 0 ? head.ulApduTimeout : 3000;
                    dev.ucNumberOfApduRetries = head.ucNumberOfApduRetries;
                    dev.xSupportsRpm = head.xSupportsRpm;

                    dev.last_seen = millis();

                    for (int p = 0; p * 20 < head.usCount; p++) {
                        char key[16]; snprintf(key, 16, "p%d", p);
                        BACnetPersistencePage page;
                        memset(&page, 0, sizeof(page));
                        
                        if (prefs.getBytes(key, &page, sizeof(page)) == sizeof(page)) {
                            if (page.ulDeviceId != ulDeviceId) continue;

                            for (int i = 0; i < 20 && (p * 20 + i) < head.usCount; i++) {
                                BACnetObject obj;
                                obj.usType = page.objects[i].ulVal >> 22;
                                obj.ulInstance = page.objects[i].ulVal & 0x3FFFFF;
                                obj.xEnabled = page.objects[i].xEnabled;
                                obj.xNamePublished = page.objects[i].xNamePublished;
                                obj.xIsCommandable = page.objects[i].xIsCommandable;
                                obj.usUnits = page.objects[i].usUnits;
                                obj.ucExpectedStatesCount = page.objects[i].ucExpectedStatesCount;
                                obj.fMinValue = page.objects[i].fMinValue;
                                obj.fMaxValue = page.objects[i].fMaxValue;
                                
                                strlcpy(obj.cName, page.objects[i].cName, sizeof(obj.cName));
                                strlcpy(obj.cUnitText, page.objects[i].cUnitText, sizeof(obj.cUnitText));
                                strlcpy(obj.cLastMqttName, page.objects[i].cName, sizeof(obj.cLastMqttName));
                                strlcpy(obj.cLastHaComponent, page.objects[i].cLastHaComponent, sizeof(obj.cLastHaComponent));
                                
                                // v6.6.1: Charger les labels des états MSI/MSO/MSV
                                if (obj.usType == OBJ_MULTI_STATE_INPUT || obj.usType == OBJ_MULTI_STATE_OUTPUT || obj.usType == OBJ_MULTI_STATE_VALUE) {
                                    load_object_states(ulDeviceId, obj.usType, obj.ulInstance, obj.state_texts);
                                }

                                obj.fPresentValue = 0.0f;
                                dev.objects.push_back(obj);
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

/**
 * Charge la configuration générale (WiFi, MQTT, BACnet) depuis NVS.
 */
void load_configuration() {
    // Initialisation des valeurs par défaut
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

    Preferences prefs;
    if (prefs.begin("system", true)) {
        if (prefs.isKey("ssid")) prefs.getString("ssid", sysCfg.wifi_ssid, 32);
        if (prefs.isKey("pass")) prefs.getString("pass", sysCfg.wifi_pass, 64);
        if (prefs.isKey("static")) sysCfg.static_ip = prefs.getBool("static", false);
        if (prefs.isKey("ip")) prefs.getString("ip", sysCfg.local_ip, 16);
        if (prefs.isKey("gw")) prefs.getString("gw", sysCfg.gateway, 16);
        if (prefs.isKey("sn")) prefs.getString("sn", sysCfg.subnet, 16);
        
        if (prefs.isKey("mac")) sysCfg.ucMacAddress = prefs.getUChar("mac", DEFAULT_MAC_ADDRESS);
        if (prefs.isKey("mm")) sysCfg.max_master = prefs.getUChar("mm", DEFAULT_MAX_MASTER);
        if (prefs.isKey("did")) sysCfg.ulDeviceId = prefs.getUInt("did", DEFAULT_DEVICE_ID);
        if (prefs.isKey("to")) sysCfg.ulApduTimeout = prefs.getUShort("to", DEFAULT_APDU_TIMEOUT);
        if (prefs.isKey("ret")) sysCfg.max_retries = prefs.getUChar("ret", DEFAULT_MAX_RETRIES);
        
        if (prefs.isKey("mqh")) prefs.getString("mqh", sysCfg.mqtt_server, 32);
        if (prefs.isKey("mprt")) sysCfg.mqtt_port = prefs.getUInt("mprt", 1883);
        if (prefs.isKey("mqu")) prefs.getString("mqu", sysCfg.mqtt_user, 32);
        if (prefs.isKey("mqp")) prefs.getString("mqp", sysCfg.mqtt_pass, 32);
        if (prefs.isKey("mqpr")) prefs.getString("mqpr", sysCfg.mqtt_prefix, 64);
        
        if (prefs.isKey("hbeat")) sysCfg.heartbeat_interval = prefs.getUInt("hbeat", DEFAULT_HEARBEAT_INTERVAL);
        if (prefs.isKey("mpi")) sysCfg.mqtt_poll_interval = prefs.getUShort("mpi", DEFAULT_MQTT_POLL);
        if (prefs.isKey("bpi")) sysCfg.bacnet_poll_interval = prefs.getUShort("bpi", DEFAULT_BACNET_POLL);
        if (prefs.isKey("tskip")) sysCfg.token_skip = prefs.getUChar("tskip", DEFAULT_TOKEN_SKIP);
        if (prefs.isKey("mif")) sysCfg.max_info_frames = prefs.getUChar("mif", DEFAULT_MAX_INFO_FRAMES);
        
        if (prefs.isKey("adu")) prefs.getString("adu", sysCfg.admin_user, 32);
        if (prefs.isKey("adp")) prefs.getString("adp", sysCfg.admin_pass, 64);
        if (prefs.isKey("lvl")) sysCfg.log_level = prefs.getUChar("lvl", pdLOG_INFO);
        if (prefs.isKey("ha_disc")) sysCfg.ha_discover = prefs.getBool("ha_disc", DEFAULT_HA_DISCOVER);
        if (prefs.isKey("n_min")) sysCfg.default_number_min = prefs.getFloat("n_min", DEFAULT_NUM_MIN);
        if (prefs.isKey("n_max")) sysCfg.default_number_max = prefs.getFloat("n_max", DEFAULT_NUM_MAX);
        if (prefs.isKey("n_stp")) sysCfg.default_number_step = prefs.getFloat("n_stp", DEFAULT_NUM_STEP);
        
        prefs.end();
    }
    z_log(pdLOG_INFO, "NVS", "[NVS] Configuration Loaded\n");

    // Chargement de la liste des devices enregistrés
    Preferences reg;
    String dev_list = "";
    if (reg.begin("registry", true)) {
        dev_list = reg.getString("dev_list", "");
        reg.end();
    }

    if (dev_list.length() > 0) {
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

/**
 * Sauvegarde la configuration système en NVS.
 */
void save_configuration() {
    Preferences prefs;
    if (prefs.begin("system", false)) {
        prefs.putString("ssid", sysCfg.wifi_ssid);
        prefs.putString("pass", sysCfg.wifi_pass);
        prefs.putBool("static", sysCfg.static_ip);
        prefs.putString("ip", sysCfg.local_ip);
        prefs.putString("gw", sysCfg.gateway);
        prefs.putString("sn", sysCfg.subnet);
        
        prefs.putUChar("mac", sysCfg.ucMacAddress);
        prefs.putUChar("mm", sysCfg.max_master);
        prefs.putUInt("did", sysCfg.ulDeviceId);
        prefs.putUShort("to", sysCfg.ulApduTimeout);
        prefs.putUChar("ret", sysCfg.max_retries);
        
        prefs.putString("mqh", sysCfg.mqtt_server);
        prefs.putUInt("mprt", sysCfg.mqtt_port);
        prefs.putString("mqu", sysCfg.mqtt_user);
        prefs.putString("mqp", sysCfg.mqtt_pass);
        prefs.putString("mqpr", sysCfg.mqtt_prefix);
        
        prefs.putUInt("hbeat", sysCfg.heartbeat_interval);
        prefs.putUShort("mpi", sysCfg.mqtt_poll_interval);
        prefs.putUShort("bpi", sysCfg.bacnet_poll_interval);
        prefs.putUChar("tskip", sysCfg.token_skip);
        prefs.putUChar("mif", sysCfg.max_info_frames);
        
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

/**
 * Marque un équipement comme 'sale' pour déclencher une sauvegarde différée (Lazy Save).
 * @param ulDeviceId Identifiant de l'équipement.
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

/**
 * Sauvegarde interne d'un device (doit être appelé avec cache_mutex déjà verrouillé).
 */
void save_device_objects_locked(uint32_t ulDeviceId) {
    if (nvs_mutex == NULL) nvs_mutex = xSemaphoreCreateMutex();
    if (xSemaphoreTake(nvs_mutex, pdMS_TO_TICKS(1000))) {
        for (auto& dev : bacnet_network_cache) {
            if (dev.ulDeviceId == ulDeviceId) {
                // v6.8.8: Reset du flag dirty avant l'écriture
                dev.xDirty = false;

                char ns[16];
                snprintf(ns, sizeof(ns), "dv_%lu", (unsigned long)ulDeviceId); // Namespace standard
                Preferences prefs;

                if (prefs.begin(ns, false)) {
                    prefs.clear();

                    BACnetPersistenceDev head;
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
                    // v6.8.3
                    head.usMaxApduLengthAccepted = dev.usMaxApduLengthAccepted;
                    head.ulApduTimeout = dev.ulApduTimeout;
                    head.ucNumberOfApduRetries = dev.ucNumberOfApduRetries;
                    head.xSupportsRpm = dev.xSupportsRpm;

                    prefs.putBytes("head", &head, sizeof(head));

                    for (int p = 0; p * 20 < (int)dev.objects.size(); p++) {
                        BACnetPersistencePage page;
                        memset(&page, 0, sizeof(page));
                        page.ulDeviceId = ulDeviceId;
                        page.page_index = (uint16_t)p;

                        for (int i = 0; i < 20 && (p * 20 + i) < (int)dev.objects.size(); i++) {
                            auto& o = dev.objects[p * 20 + i];
                            page.objects[i].ulVal = ((uint32_t)o.usType << 22) | (o.ulInstance & 0x3FFFFF);
                            page.objects[i].xEnabled = o.xEnabled;
                            page.objects[i].xNamePublished = o.xNamePublished;
                            page.objects[i].xIsCommandable = o.xIsCommandable;
                            page.objects[i].usUnits = o.usUnits;
                            page.objects[i].ucExpectedStatesCount = (uint8_t)std::min((int)o.ucExpectedStatesCount, 255);
                            page.objects[i].fMinValue = o.fMinValue;
                            page.objects[i].fMaxValue = o.fMaxValue;
                            strlcpy(page.objects[i].cName, o.cName, 32);
                            strlcpy(page.objects[i].cUnitText, o.cUnitText, 12);
                            strlcpy(page.objects[i].cLastHaComponent, o.cLastHaComponent, 16);
                        }
                        char key[16]; snprintf(key, 16, "p%d", p);
                        prefs.putBytes(key, &page, sizeof(page));
                    }
                    prefs.end();

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
                break;
            }
        }
        xSemaphoreGive(nvs_mutex);
    }
}

/**
 * Sauvegarde les labels des états (MSI/MSO/MSV) dans un namespace dédié.
 */
void save_object_states(uint32_t ulDeviceId, uint16_t type, uint32_t ulInstance, const std::vector<String>& states) {
    if (states.empty()) return;
    
    char ns[16]; snprintf(ns, sizeof(ns), "st_%lu", (unsigned long)ulDeviceId);
    char key[16]; snprintf(key, sizeof(key), "o%u_%lu", type, (unsigned long)ulInstance);
    
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
 * Charge les labels des états depuis le namespace dédié.
 */
void load_object_states(uint32_t ulDeviceId, uint16_t type, uint32_t ulInstance, std::vector<String>& states) {
    char ns[16]; snprintf(ns, sizeof(ns), "st_%lu", (unsigned long)ulDeviceId);
    char key[16]; snprintf(key, sizeof(key), "o%u_%lu", type, (unsigned long)ulInstance);
    
    Preferences prefs;
    if (prefs.begin(ns, true)) {
        String combined = prefs.getString(key, "");
        if (combined.length() > 0) {
            states.clear();
            int start = 0;
            int end = combined.indexOf('|');
            while (end != -1) {
                states.push_back(combined.substring(start, end));
                start = end + 1;
                end = combined.indexOf('|', start);
            }
            states.push_back(combined.substring(start));
        }
        prefs.end();
        }
        }


