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
void load_device_objects(uint32_t device_id) {
    if (nvs_mutex == NULL) nvs_mutex = xSemaphoreCreateMutex();
    char ns[16]; 
    snprintf(ns, sizeof(ns), "dv_%lu", (unsigned long)device_id); // Namespace standard
    Preferences prefs;
    
    if (prefs.begin(ns, true)) {
        BACnetPersistenceDev head;
        if (prefs.getBytes("head", &head, sizeof(head)) == sizeof(head)) {
            
            if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(1000))) {
                bool exists = false;
                for(auto& d : bacnet_network_cache) if(d.device_id == device_id) exists = true;
                
                if (!exists) {
                    BACnetDevice dev;
                    dev.device_id = head.device_id;
                    dev.mac_address = head.mac_address;
                    dev.enabled = head.enabled;
                    dev.name = String(head.name);
                    dev.vendor = String(head.vendor);
                    dev.discovery_done = head.discovery_done;
                    dev.disc_step = (DISC_STEP_T)head.disc_step;
                    dev.disc_obj_idx = head.disc_obj_idx;
                    dev.last_seen = millis();

                    for (int p = 0; p * 20 < head.count; p++) {
                        char key[16]; snprintf(key, 16, "p%d", p);
                        BACnetPersistencePage page;
                        memset(&page, 0, sizeof(page));
                        
                        if (prefs.getBytes(key, &page, sizeof(page)) == sizeof(page)) {
                            if (page.device_id != device_id) continue;

                            for (int i = 0; i < 20 && (p * 20 + i) < head.count; i++) {
                                BACnetObject obj;
                                obj.type = page.objects[i].val >> 22;
                                obj.instance = page.objects[i].val & 0x3FFFFF;
                                obj.enabled = page.objects[i].poll;
                                obj.name_published = page.objects[i].name_published;
                                obj.is_commandable = page.objects[i].is_commandable;
                                obj.units = page.objects[i].units;
                                obj.expected_states_count = page.objects[i].states_count;
                                obj.min_value = page.objects[i].min_value;
                                obj.max_value = page.objects[i].max_value;
                                
                                strlcpy(obj.name, page.objects[i].name, sizeof(obj.name));
                                strlcpy(obj.unit_text, page.objects[i].unit_text, sizeof(obj.unit_text));
                                strlcpy(obj.last_mqtt_name, page.objects[i].name, sizeof(obj.last_mqtt_name));
                                strlcpy(obj.last_ha_component, page.objects[i].last_ha_component, sizeof(obj.last_ha_component));
                                
                                // v6.6.1: Charger les labels des états MSI/MSO/MSV
                                if (obj.type == OBJ_MULTI_STATE_INPUT || obj.type == OBJ_MULTI_STATE_OUTPUT || obj.type == OBJ_MULTI_STATE_VALUE) {
                                    load_object_states(device_id, obj.type, obj.instance, obj.state_texts);
                                }

                                obj.present_value = 0.0f;
                                dev.objects.push_back(obj);
                            }
                        }
                    }
                    dev.last_seen = millis();   
                    bacnet_network_cache.push_back(dev);
                    z_log(pdLOG_INFO, "NVS", "[NVS] Restored Device %lu (%d objs)\n", (unsigned long)device_id, (int)dev.objects.size());
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
    strlcpy(sysCfg.cWifiSsid, configDEFAULT_SSID, 32);
    strlcpy(sysCfg.cWifiPass, "", 64);
    sysCfg.xStaticIp = false;
    strlcpy(sysCfg.cLocalIp, configDEFAULT_STATIC_IP, 16);
    strlcpy(sysCfg.cGateway, configDEFAULT_GATEWAY, 16);
    strlcpy(sysCfg.cSubnet, configDEFAULT_SUBNET, 16);
    sysCfg.ucMacAddress = configDEFAULT_MAC_ADDRESS;
    sysCfg.ucMaxMaster = configDEFAULT_MAX_MASTER;
    sysCfg.ulDeviceId = configDEFAULT_DEVICE_ID;
    sysCfg.usApduTimeout = configDEFAULT_APDU_TIMEOUT;
    sysCfg.ucMaxRetries = configDEFAULT_MAX_RETRIES;
    strlcpy(sysCfg.cMqttServer, configDEFAULT_MQTT_SERVER, 32);
    sysCfg.usMqttPort = 1883;
    strlcpy(sysCfg.cMqttUser, "", 32);
    strlcpy(sysCfg.cMqttPass, "", 32);
    strlcpy(sysCfg.cMqttPrefix, "bacnet", 64);
    sysCfg.ulHeartbeatInterval = configDEFAULT_HEARTBEAT_INTERVAL;
    sysCfg.usMqttPollInterval = configDEFAULT_MQTT_POLL;
    sysCfg.usBacnetPollInterval = configDEFAULT_BACNET_POLL;
    sysCfg.ucTokenSkip = configDEFAULT_TOKEN_SKIP;
    sysCfg.ucLogLevel = pdLOG_INFO;
    sysCfg.ucMaxInfoFrames = configDEFAULT_MAX_INFO_FRAMES;
    sysCfg.xHaDiscover = configDEFAULT_HA_DISCOVER;
    sysCfg.fDefaultNumberMin = configDEFAULT_NUM_MIN;
    sysCfg.fDefaultNumberMax = configDEFAULT_NUM_MAX;
    sysCfg.fDefaultNumberStep = configDEFAULT_NUM_STEP;
    strlcpy(sysCfg.cAdminUser, "admin", 32);
    strlcpy(sysCfg.cAdminPass, "admin1234", 64);

    Preferences prefs;
    if (prefs.begin("system", true)) {
        if (prefs.isKey("ssid")) prefs.getString("ssid", sysCfg.cWifiSsid, 32);
        if (prefs.isKey("pass")) prefs.getString("pass", sysCfg.cWifiPass, 64);
        if (prefs.isKey("static")) sysCfg.xStaticIp = prefs.getBool("static", false);
        if (prefs.isKey("ip")) prefs.getString("ip", sysCfg.cLocalIp, 16);
        if (prefs.isKey("gw")) prefs.getString("gw", sysCfg.cGateway, 16);
        if (prefs.isKey("sn")) prefs.getString("sn", sysCfg.cSubnet, 16);
        
        if (prefs.isKey("mac")) sysCfg.ucMacAddress = prefs.getUChar("mac", configDEFAULT_MAC_ADDRESS);
        if (prefs.isKey("mm")) sysCfg.ucMaxMaster = prefs.getUChar("mm", configDEFAULT_MAX_MASTER);
        if (prefs.isKey("did")) sysCfg.ulDeviceId = prefs.getUInt("did", configDEFAULT_DEVICE_ID);
        if (prefs.isKey("to")) sysCfg.usApduTimeout = prefs.getUShort("to", configDEFAULT_APDU_TIMEOUT);
        if (prefs.isKey("ret")) sysCfg.ucMaxRetries = prefs.getUChar("ret", configDEFAULT_MAX_RETRIES);
        
        if (prefs.isKey("mqh")) prefs.getString("mqh", sysCfg.cMqttServer, 32);
        if (prefs.isKey("mprt")) sysCfg.usMqttPort = prefs.getUInt("mprt", 1883);
        if (prefs.isKey("mqu")) prefs.getString("mqu", sysCfg.cMqttUser, 32);
        if (prefs.isKey("mqp")) prefs.getString("mqp", sysCfg.cMqttPass, 32);
        if (prefs.isKey("mqpr")) prefs.getString("mqpr", sysCfg.cMqttPrefix, 64);
        
        if (prefs.isKey("hbeat")) sysCfg.ulHeartbeatInterval = prefs.getUInt("hbeat", configDEFAULT_HEARTBEAT_INTERVAL);
        if (prefs.isKey("mpi")) sysCfg.usMqttPollInterval = prefs.getUShort("mpi", configDEFAULT_MQTT_POLL);
        if (prefs.isKey("bpi")) sysCfg.usBacnetPollInterval = prefs.getUShort("bpi", configDEFAULT_BACNET_POLL);
        if (prefs.isKey("tskip")) sysCfg.ucTokenSkip = prefs.getUChar("tskip", configDEFAULT_TOKEN_SKIP);
        if (prefs.isKey("mif")) sysCfg.ucMaxInfoFrames = prefs.getUChar("mif", configDEFAULT_MAX_INFO_FRAMES);
        
        if (prefs.isKey("adu")) prefs.getString("adu", sysCfg.cAdminUser, 32);
        if (prefs.isKey("adp")) prefs.getString("adp", sysCfg.cAdminPass, 64);
        if (prefs.isKey("lvl")) sysCfg.ucLogLevel = prefs.getUChar("lvl", pdLOG_INFO);
        if (prefs.isKey("ha_disc")) sysCfg.xHaDiscover = prefs.getBool("ha_disc", configDEFAULT_HA_DISCOVER);
        if (prefs.isKey("n_min")) sysCfg.fDefaultNumberMin = prefs.getFloat("n_min", configDEFAULT_NUM_MIN);
        if (prefs.isKey("n_max")) sysCfg.fDefaultNumberMax = prefs.getFloat("n_max", configDEFAULT_NUM_MAX);
        if (prefs.isKey("n_stp")) sysCfg.fDefaultNumberStep = prefs.getFloat("n_stp", configDEFAULT_NUM_STEP);
        
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
        prefs.putString("ssid", sysCfg.cWifiSsid);
        prefs.putString("pass", sysCfg.cWifiPass);
        prefs.putBool("static", sysCfg.xStaticIp);
        prefs.putString("ip", sysCfg.cLocalIp);
        prefs.putString("gw", sysCfg.cGateway);
        prefs.putString("sn", sysCfg.cSubnet);
        
        prefs.putUChar("mac", sysCfg.ucMacAddress);
        prefs.putUChar("mm", sysCfg.ucMaxMaster);
        prefs.putUInt("did", sysCfg.ulDeviceId);
        prefs.putUShort("to", sysCfg.usApduTimeout);
        prefs.putUChar("ret", sysCfg.ucMaxRetries);
        
        prefs.putString("mqh", sysCfg.cMqttServer);
        prefs.putUInt("mprt", sysCfg.usMqttPort);
        prefs.putString("mqu", sysCfg.cMqttUser);
        prefs.putString("mqp", sysCfg.cMqttPass);
        prefs.putString("mqpr", sysCfg.cMqttPrefix);
        
        prefs.putUInt("hbeat", sysCfg.ulHeartbeatInterval);
        prefs.putUShort("mpi", sysCfg.usMqttPollInterval);
        prefs.putUShort("bpi", sysCfg.usBacnetPollInterval);
        prefs.putUChar("tskip", sysCfg.ucTokenSkip);
        prefs.putUChar("mif", sysCfg.ucMaxInfoFrames);
        
        prefs.putString("adu", sysCfg.cAdminUser);
        prefs.putString("adp", sysCfg.cAdminPass);
        prefs.putUChar("lvl", sysCfg.ucLogLevel);
        prefs.putBool("ha_disc", sysCfg.xHaDiscover);
        prefs.putFloat("n_min", sysCfg.fDefaultNumberMin);
        prefs.putFloat("n_max", sysCfg.fDefaultNumberMax);
        prefs.putFloat("n_stp", sysCfg.fDefaultNumberStep);
        
        prefs.end();
    }
    z_log(pdLOG_INFO, "NVS", "[NVS] Configuration System Saved\n");
}

/**
 * Sauvegarde interne d'un device (doit être appelé avec cache_mutex déjà verrouillé).
 */
void save_device_objects_locked(uint32_t device_id) {
    if (nvs_mutex == NULL) nvs_mutex = xSemaphoreCreateMutex();
    if (xSemaphoreTake(nvs_mutex, pdMS_TO_TICKS(1000))) {
        for (auto& dev : bacnet_network_cache) {
            if (dev.device_id == device_id) {
                char ns[16]; 
                snprintf(ns, sizeof(ns), "dv_%lu", (unsigned long)device_id); // Namespace standard
                Preferences prefs;
                
                if (prefs.begin(ns, false)) {
                    prefs.clear(); 

                    BACnetPersistenceDev head;
                    memset(&head, 0, sizeof(head));
                    head.device_id = dev.device_id;
                    head.mac_address = dev.mac_address;
                    head.enabled = dev.enabled;
                    head.count = (uint16_t)dev.objects.size();
                    head.discovery_done = dev.discovery_done;
                    head.disc_step = (uint8_t)dev.disc_step;
                    head.disc_obj_idx = dev.disc_obj_idx;
                    strlcpy(head.name, dev.name.c_str(), 32);
                    strlcpy(head.vendor, dev.vendor.c_str(), 32);
                    
                    prefs.putBytes("head", &head, sizeof(head));

                    for (int p = 0; p * 20 < (int)dev.objects.size(); p++) {
                        BACnetPersistencePage page;
                        memset(&page, 0, sizeof(page));
                        page.device_id = device_id;
                        page.page_index = p;

                        for (int i = 0; i < 20 && (p * 20 + i) < (int)dev.objects.size(); i++) {
                            auto& o = dev.objects[p * 20 + i];
                            page.objects[i].val = ((uint32_t)o.type << 22) | (o.instance & 0x3FFFFF);
                            page.objects[i].poll = o.enabled;
                            page.objects[i].name_published = o.name_published;
                            page.objects[i].is_commandable = o.is_commandable;
                            page.objects[i].units = o.units;
                            page.objects[i].states_count = (uint8_t)std::min((int)o.expected_states_count, 255);
                            page.objects[i].min_value = o.min_value;
                            page.objects[i].max_value = o.max_value;
                            strlcpy(page.objects[i].name, o.name, 32);
                            strlcpy(page.objects[i].unit_text, o.unit_text, 12);
                            strlcpy(page.objects[i].last_ha_component, o.last_ha_component, 16);
                        }
                        char key[16]; snprintf(key, 16, "p%d", p);
                        prefs.putBytes(key, &page, sizeof(page));
                    }
                    prefs.end();

                    Preferences reg;
                    if (reg.begin("registry", false)) {
                        String list = reg.getString("dev_list", "");
                        if (list.indexOf(String(device_id)) == -1) {
                            list += (list.length() > 0 ? ";" : "") + String(device_id);
                            reg.putString("dev_list", list);
                        }
                        reg.end();
                    }
                    z_log(pdLOG_INFO, "NVS", "[NVS] SUCCESS: Saved Device %lu\n", (unsigned long)device_id);
                }
                break; 
            }
        }
        xSemaphoreGive(nvs_mutex);
    }
}

/**
 * Fonction publique de sauvegarde d'un device (gère le verrouillage).
 */
void save_device_objects(uint32_t device_id) {
    if (cache_mutex == NULL) return;
    if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(1000))) {
        save_device_objects_locked(device_id);
        xSemaphoreGive(cache_mutex);
    }
}

/**
 * Sauvegarde les labels des états (MSI/MSO/MSV) dans un namespace dédié.
 */
void save_object_states(uint32_t device_id, uint16_t type, uint32_t instance, const std::vector<String>& states) {
    if (states.empty()) return;
    
    char ns[16]; snprintf(ns, sizeof(ns), "st_%lu", (unsigned long)device_id);
    char key[16]; snprintf(key, sizeof(key), "o%u_%lu", type, (unsigned long)instance);
    
    String combined = "";
    for (size_t i = 0; i < states.size(); i++) {
        combined += states[i];
        if (i < states.size() - 1) combined += "|";
    }
    
    Preferences prefs;
    if (prefs.begin(ns, false)) {
        prefs.putString(key, combined);
        prefs.end();
        z_log(pdLOG_DEBUG, "NVS", "[NVS] Saved States for %u:%lu (%d labels)\n", type, instance, (int)states.size());
    }
}

/**
 * Charge les labels des états depuis le namespace dédié.
 */
void load_object_states(uint32_t device_id, uint16_t type, uint32_t instance, std::vector<String>& states) {
    char ns[16]; snprintf(ns, sizeof(ns), "st_%lu", (unsigned long)device_id);
    char key[16]; snprintf(key, sizeof(key), "o%u_%lu", type, (unsigned long)instance);
    
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
