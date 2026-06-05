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
                                
                                obj.present_value = 0.0f;
                                dev.objects.push_back(obj);
                            }
                        }
                    }
                    dev.last_seen = millis();   
                    bacnet_network_cache.push_back(dev);
                    z_log(LOG_INFO, "NVS", "[NVS] Restored Device %lu (%d objs)\n", (unsigned long)device_id, (int)dev.objects.size());
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
    sysCfg.mac_address = DEFAULT_MAC_ADDRESS;
    sysCfg.max_master = DEFAULT_MAX_MASTER;
    sysCfg.device_id = DEFAULT_DEVICE_ID;
    sysCfg.apdu_timeout = DEFAULT_APDU_TIMEOUT;
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
    sysCfg.log_level = LOG_INFO;
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
        
        if (prefs.isKey("mac")) sysCfg.mac_address = prefs.getUChar("mac", DEFAULT_MAC_ADDRESS);
        if (prefs.isKey("mm")) sysCfg.max_master = prefs.getUChar("mm", DEFAULT_MAX_MASTER);
        if (prefs.isKey("did")) sysCfg.device_id = prefs.getUInt("did", DEFAULT_DEVICE_ID);
        if (prefs.isKey("to")) sysCfg.apdu_timeout = prefs.getUShort("to", DEFAULT_APDU_TIMEOUT);
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
        if (prefs.isKey("lvl")) sysCfg.log_level = prefs.getUChar("lvl", LOG_INFO);
        if (prefs.isKey("ha_disc")) sysCfg.ha_discover = prefs.getBool("ha_disc", DEFAULT_HA_DISCOVER);
        if (prefs.isKey("n_min")) sysCfg.default_number_min = prefs.getFloat("n_min", DEFAULT_NUM_MIN);
        if (prefs.isKey("n_max")) sysCfg.default_number_max = prefs.getFloat("n_max", DEFAULT_NUM_MAX);
        if (prefs.isKey("n_stp")) sysCfg.default_number_step = prefs.getFloat("n_stp", DEFAULT_NUM_STEP);
        
        prefs.end();
    }
    z_log(LOG_INFO, "NVS", "[NVS] Configuration Loaded\n");

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
        
        prefs.putUChar("mac", sysCfg.mac_address);
        prefs.putUChar("mm", sysCfg.max_master);
        prefs.putUInt("did", sysCfg.device_id);
        prefs.putUShort("to", sysCfg.apdu_timeout);
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
    z_log(LOG_INFO, "NVS", "[NVS] Configuration System Saved\n");
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
                    z_log(LOG_INFO, "NVS", "[NVS] SUCCESS: Saved Device %lu\n", (unsigned long)device_id);
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
