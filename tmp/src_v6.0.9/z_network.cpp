#include "z_network.h"
#include "z_ui.h"
#include "z_bacnet.h"
#include "z_mqtt.h"
#include <ArduinoJson.h>
#include <Update.h>
#include <ESPmDNS.h>
#include "esp_wifi.h"
#include <stdarg.h>
#include "nvs_flash.h"

extern AsyncWebSocket ws;
static uint32_t wifi_connect_start = 0;
static bool wifi_fallback_active = false;

typedef struct { char message[256]; } LogMessage;
QueueHandle_t log_queue = NULL;

static void websocket_log_task(void *pvParameters) {
    LogMessage received_log;
    for( ;; ) {
        if (xQueueReceive(log_queue, &received_log, portMAX_DELAY) == pdTRUE) {
            if (ws.count() > 0) ws.textAll(received_log.message);
        }
    }
}

void z_log(int level, const char* tag, const char* format, ...) {
    if (level > sysCfg.log_level) return;
    char lvl_str[4] = "INF";
    if (level == LOG_ERROR) strcpy(lvl_str, "ERR");
    else if (level == LOG_WARN) strcpy(lvl_str, "WRN");
    else if (level == LOG_DEBUG) strcpy(lvl_str, "DBG");
    LogMessage log_msg;
    uint32_t now_ms = millis();
    int core = xPortGetCoreID();
    int prefix_len = snprintf(log_msg.message, sizeof(log_msg.message), "[%lu][%d][%s][%s] ", (unsigned long)now_ms, core, lvl_str, tag);
    va_list arg; va_start(arg, format);
    vsnprintf(log_msg.message + prefix_len, sizeof(log_msg.message) - prefix_len, format, arg);
    va_end(arg);
    printf("%s", log_msg.message); 
    if (log_queue != NULL) xQueueSend(log_queue, &log_msg, 0);
}

void setup_system_mutexes() {
    if (cache_mutex == NULL) {
        cache_mutex = xSemaphoreCreateMutex();
        z_log(LOG_INFO, "SYS", "Cache Mutex Initialized\n");
    }
}

void load_device_objects(uint32_t device_id) {
    if (cache_mutex == NULL) return;
    char ns[16]; snprintf(ns, sizeof(ns), "dev_%lu", (unsigned long)device_id);
    Preferences prefs;
    if (prefs.begin(ns, true)) {
        BACnetPersistenceDev head;
        if (prefs.getBytes("head", &head, sizeof(head)) > 0) {
            if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(1000))) {
                bool exists = false;
                for(auto& d : bacnet_network_cache) if(d.device_id == device_id) exists = true;
                if (!exists) {
                    BACnetDevice dev; dev.device_id = head.device_id; dev.mac_address = head.mac_address; dev.enabled = head.enabled;
                    dev.name = String(head.name); dev.vendor = String(head.vendor); dev.discovery_done = head.discovery_done;
                    dev.disc_step = (DISC_STEP_T)head.disc_step; dev.disc_obj_idx = head.disc_obj_idx; dev.last_seen = millis();
                    for (int p = 0; p * 20 < head.count; p++) {
                        char key[16]; snprintf(key, 16, "p%d", p);
                        BACnetPersistencePage page;
                        if (prefs.getBytes(key, &page, sizeof(page)) > 0) {
                            if (page.device_id != device_id) continue;
                            for (int i = 0; i < 20 && (p * 20 + i) < head.count; i++) {
                                BACnetObject obj; obj.type = page.objects[i].val >> 22; obj.instance = page.objects[i].val & 0x3FFFFF;
                                obj.enabled = page.objects[i].poll; obj.name_published = page.objects[i].name_published;
                                strlcpy(obj.name, page.objects[i].name, sizeof(obj.name)); strlcpy(obj.unit_text, page.objects[i].unit_text, sizeof(obj.unit_text));
                                strlcpy(obj.last_mqtt_name, page.objects[i].name, sizeof(obj.last_mqtt_name)); obj.present_value = 0.0f;
                                dev.objects.push_back(obj);
                            }
                        }
                    }
                    bacnet_network_cache.push_back(dev);
                }
                xSemaphoreGive(cache_mutex);
            }
        }
        prefs.end();
    }
}

void load_configuration() {
    strlcpy(sysCfg.wifi_ssid, "", 32); strlcpy(sysCfg.wifi_pass, "", 64); sysCfg.static_ip = true; 
    strlcpy(sysCfg.local_ip, DEFAULT_STATIC_IP, 16); strlcpy(sysCfg.gateway, DEFAULT_GATEWAY, 16); strlcpy(sysCfg.subnet, DEFAULT_SUBNET, 16);
    sysCfg.mac_address = DEFAULT_MAC_ADDRESS; sysCfg.max_master = DEFAULT_MAX_MASTER; sysCfg.device_id = DEFAULT_DEVICE_ID;
    sysCfg.apdu_timeout = DEFAULT_APDU_TIMEOUT; sysCfg.max_retries = DEFAULT_MAX_RETRIES;
    strlcpy(sysCfg.mqtt_server, DEFAULT_MQTT_SERVER, 32); sysCfg.mqtt_port = 1883;
    strlcpy(sysCfg.mqtt_user, "", 32); strlcpy(sysCfg.mqtt_pass, "", 32); strlcpy(sysCfg.mqtt_prefix, "bacnet", 64);
    sysCfg.heartbeat_interval = DEFAULT_HEARBEAT_INTERVAL; sysCfg.token_skip = DEFAULT_TOKEN_SKIP;
    sysCfg.mqtt_poll_interval = DEFAULT_MQTT_POLL; sysCfg.bacnet_poll_interval = DEFAULT_BACNET_POLL;
    sysCfg.log_level = LOG_INFO; sysCfg.max_info_frames = DEFAULT_MAX_INFO_FRAMES;
    strlcpy(sysCfg.admin_user, "admin", 32); strlcpy(sysCfg.admin_pass, "admin1234", 64);
    Preferences prefs;
    if (prefs.begin("system", true)) {
        if (prefs.isKey("ssid")) prefs.getString("ssid", sysCfg.wifi_ssid, 32);
        if (prefs.isKey("pass")) prefs.getString("pass", sysCfg.wifi_pass, 64);
        if (prefs.isKey("static")) sysCfg.static_ip = prefs.getBool("static", true);
        if (prefs.isKey("ip")) prefs.getString("ip", sysCfg.local_ip, 16);
        if (prefs.isKey("gw")) prefs.getString("gw", sysCfg.gateway, 16);
        if (prefs.isKey("sn")) prefs.getString("sn", sysCfg.subnet, 16);
        if (prefs.isKey("mqh")) prefs.getString("mqh", sysCfg.mqtt_server, 32);
        if (prefs.isKey("mqu")) prefs.getString("mqu", sysCfg.mqtt_user, 32);
        if (prefs.isKey("mqp")) prefs.getString("mqp", sysCfg.mqtt_pass, 32);
        if (prefs.isKey("mqpr")) prefs.getString("mqpr", sysCfg.mqtt_prefix, 64);
        if (prefs.isKey("mac")) sysCfg.mac_address = prefs.getUChar("mac", 1);
        if (prefs.isKey("mm")) sysCfg.max_master = prefs.getUChar("mm", 127);
        if (prefs.isKey("did")) sysCfg.device_id = prefs.getUInt("did", 123);
        if (prefs.isKey("to")) sysCfg.apdu_timeout = prefs.getUShort("to", 500);
        if (prefs.isKey("ret")) sysCfg.max_retries = prefs.getUChar("ret", 3);
        if (prefs.isKey("bpi")) sysCfg.bacnet_poll_interval = prefs.getUShort("bpi", DEFAULT_BACNET_POLL);
        if (prefs.isKey("hbeat")) sysCfg.heartbeat_interval = prefs.getUInt("hbeat", DEFAULT_HEARBEAT_INTERVAL);
        if (prefs.isKey("tskip")) sysCfg.token_skip = prefs.getUChar("tskip", DEFAULT_TOKEN_SKIP);
        if (prefs.isKey("mpi")) sysCfg.mqtt_poll_interval = prefs.getUShort("mpi", DEFAULT_MQTT_POLL);
        if (prefs.isKey("mif")) sysCfg.max_info_frames = prefs.getUChar("mif", DEFAULT_MAX_INFO_FRAMES);
        if (prefs.isKey("adu")) prefs.getString("adu", sysCfg.admin_user, 32);
        if (prefs.isKey("adp")) prefs.getString("adp", sysCfg.admin_pass, 64);
        if (prefs.isKey("lvl")) sysCfg.log_level = prefs.getUChar("lvl", LOG_INFO);
        prefs.end();
    }
    Preferences reg; String dev_list = "";
    if (reg.begin("registry", true)) { dev_list = reg.getString("dev_list", ""); reg.end(); }
    if (dev_list.length() > 0) {
        int start = 0; int end = dev_list.indexOf(';');
        while (start < dev_list.length()) {
            String sid = (end == -1) ? dev_list.substring(start) : dev_list.substring(start, end);
            sid.trim(); if (sid.length() > 0) load_device_objects(sid.toInt());
            if (end == -1) break; start = end + 1; end = dev_list.indexOf(';', start);
        }
    }
}

void save_configuration() {
    Preferences prefs;
    if (prefs.begin("system", false)) {
        prefs.putString("ssid", sysCfg.wifi_ssid); prefs.putString("pass", sysCfg.wifi_pass);
        prefs.putBool("static", sysCfg.static_ip); prefs.putString("ip", sysCfg.local_ip);
        prefs.putString("gw", sysCfg.gateway); prefs.putString("sn", sysCfg.subnet);
        prefs.putUChar("mac", sysCfg.mac_address); prefs.putUChar("mm", sysCfg.max_master);
        prefs.putUInt("did", sysCfg.device_id); prefs.putUShort("to", sysCfg.apdu_timeout);
        prefs.putUChar("ret", sysCfg.max_retries); prefs.putUInt("hbeat", sysCfg.heartbeat_interval);
        prefs.putUChar("tskip", sysCfg.token_skip); prefs.putString("mqh", sysCfg.mqtt_server);
        prefs.putString("mqu", sysCfg.mqtt_user); prefs.putString("mqp", sysCfg.mqtt_pass);
        prefs.putString("mqpr", sysCfg.mqtt_prefix); prefs.putUShort("mpi", sysCfg.mqtt_poll_interval);
        prefs.putUShort("bpi", sysCfg.bacnet_poll_interval); prefs.putUChar("mif", sysCfg.max_info_frames);
        prefs.putString("adu", sysCfg.admin_user); prefs.putString("adp", sysCfg.admin_pass);
        prefs.putUChar("lvl", sysCfg.log_level); prefs.end();
    }
}

void save_device_objects_locked(uint32_t device_id) {
    char ns[16]; snprintf(ns, sizeof(ns), "dev_%lu", (unsigned long)device_id);
    Preferences prefs;
    for (auto& dev : bacnet_network_cache) {
        if (dev.device_id == device_id) {
            if (prefs.begin(ns, false)) {
                BACnetPersistenceDev head; memset(&head, 0, sizeof(head));
                head.device_id = dev.device_id; head.mac_address = dev.mac_address; head.enabled = dev.enabled;
                head.discovery_done = dev.discovery_done; head.disc_step = (uint8_t)dev.disc_step; head.disc_obj_idx = dev.disc_obj_idx;
                head.total_slots = dev.total_slots;
                strlcpy(head.name, dev.name.c_str(), 32); strlcpy(head.vendor, dev.vendor.c_str(), 32);
                head.count = (uint8_t)std::min((int)dev.objects.size(), 100);
                prefs.putBytes("head", &head, sizeof(head));
                for (int p = 0; p * 20 < head.count; p++) {
                    BACnetPersistencePage page; memset(&page, 0, sizeof(page));
                    page.device_id = dev.device_id; page.page_index = p;
                    for (int i = 0; i < 20 && (p * 20 + i) < head.count; i++) {
                        auto& o = dev.objects[p * 20 + i];
                        page.objects[i].val = ((uint32_t)o.type << 22) | (o.instance & 0x3FFFFF);
                        strlcpy(page.objects[i].name, o.name, 32); strlcpy(page.objects[i].unit_text, o.unit_text, 12);
                        page.objects[i].poll = o.enabled; page.objects[i].name_published = o.name_published;
                    }
                    char key[16]; snprintf(key, 16, "p%d", p);
                    prefs.putBytes(key, &page, sizeof(page));
                }
                prefs.end();
                Preferences reg; if (reg.begin("registry", false)) {
                    String list = reg.getString("dev_list", "");
                    if (list.indexOf(String(device_id)) == -1) { list += (list.length() > 0 ? ";" : "") + String(device_id); reg.putString("dev_list", list); }
                    reg.end();
                }
                z_log(LOG_INFO, "NVS", "Saved Dev %lu (%u objs)\n", (unsigned long)device_id, head.count);
            }
            break; 
        }
    }
}

void save_device_objects(uint32_t device_id) { if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(1000))) { save_device_objects_locked(device_id); xSemaphoreGive(cache_mutex); } }

void setup_network_infrastructure() {
    log_queue = xQueueCreate(20, sizeof(LogMessage));
    if (log_queue != NULL) xTaskCreatePinnedToCore(websocket_log_task, "WS_Log", 4096, NULL, 2, NULL, 0);
    load_configuration();
    WiFi.persistent(false); WiFi.disconnect(true); vTaskDelay(pdMS_TO_TICKS(100));
    if (strlen(sysCfg.wifi_ssid) == 0) { is_ap_mode = true; WiFi.mode(WIFI_AP); WiFi.softAP("ZIRCON-GW-CONFIG", "admin1234"); }
    else {
        is_ap_mode = false; WiFi.mode(WIFI_STA); esp_wifi_set_ps(WIFI_PS_NONE);
        if (sysCfg.static_ip) { IPAddress ip, gw, sn; if (ip.fromString(sysCfg.local_ip) && gw.fromString(sysCfg.gateway) && sn.fromString(sysCfg.subnet)) WiFi.config(ip, gw, sn); }
        WiFi.begin(sysCfg.wifi_ssid, sysCfg.wifi_pass); wifi_connect_start = millis();
    }
    webServer.addHandler(&ws);
    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request) { if(!is_authenticated(request)) return; request->send_P(200, "text/html", INDEX_HTML); });
    webServer.on("/api/objects", HTTP_GET, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        JsonDocument doc; JsonArray controllers = doc.to<JsonArray>();
        if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(1000))) {
            for (auto& dev : bacnet_network_cache) {
                JsonObject c = controllers.add<JsonObject>(); c["device_id"] = dev.device_id; c["name"] = dev.name; c["vendor"] = dev.vendor; c["enabled"] = dev.enabled;
                JsonArray objs_arr = c["objects"].to<JsonArray>();
                for (auto& o : dev.objects) { 
                    JsonObject obj = objs_arr.add<JsonObject>(); obj["type"] = o.type; obj["inst"] = o.instance; obj["name"] = o.name; obj["val"] = o.present_value; obj["poll"] = o.enabled; obj["unit"] = o.unit_text; 
                    if (!o.state_texts.empty()) { JsonArray states = obj["states"].to<JsonArray>(); for(const auto& s : o.state_texts) states.add(s); }
                }
            }
            xSemaphoreGive(cache_mutex);
        }
        String response; response.reserve(65536);
        serializeJson(doc, response); request->send(200, "application/json", response);
    });
    webServer.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        JsonDocument doc; doc["ver"] = VERSION_GLOBAL; doc["rssi"] = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
        doc["ip"] = is_ap_mode ? "192.168.4.1" : WiFi.localIP().toString(); doc["heap"] = ESP.getFreeHeap() / 1024; doc["uptime"] = millis() / 1000;
        doc["mstp_cnt"] = bacnetStats.tokens_seen; doc["mstp_rx"] = bacnetStats.ms_msgs_rx; doc["mstp_tx"] = bacnetStats.ms_msgs_tx;
        JsonArray dev_arr = doc["devices"].to<JsonArray>();
        if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(100))) {
            for (auto& dev : bacnet_network_cache) { JsonObject d = dev_arr.add<JsonObject>(); d["id"] = dev.device_id; d["step"] = (int)dev.disc_step; d["idx"] = dev.disc_obj_idx; d["total"] = dev.total_slots; d["done"] = dev.discovery_done; d["vobjs"] = dev.objects.size(); d["doid"] = dev.debug_oid; }
            xSemaphoreGive(cache_mutex);
        }
        String res; serializeJson(doc, res); request->send(200, "application/json", res);
    });
    webServer.on("/api/reset_cache", HTTP_POST, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        z_log(LOG_INFO, "NVS", "Resetting NVS...\n");
        nvs_flash_erase(); nvs_flash_init();
        request->send(200, "text/plain", "OK"); pending_reboot = true; reboot_timer = millis() + 1000;
    });
    webServer.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
        if(!is_authenticated(request)) return;
        auto check = [&](const char* p, char* t, size_t m) { if(request->hasParam(p, true)) { String v = request->getParam(p, true)->value(); if(v.length() > 0 && v != "******") strlcpy(t, v.c_str(), m); } };
        check("ssid", sysCfg.wifi_ssid, 32); check("pass", sysCfg.wifi_pass, 64); check("ip", sysCfg.local_ip, 16); check("mqh", sysCfg.mqtt_server, 32);
        if(request->hasParam("mac", true)) sysCfg.mac_address = request->getParam("mac", true)->value().toInt();
        if(request->hasParam("mm", true)) sysCfg.max_master = request->getParam("mm", true)->value().toInt();
        if(request->hasParam("did", true)) sysCfg.device_id = request->getParam("did", true)->value().toInt();
        if(request->hasParam("lvl", true)) sysCfg.log_level = request->getParam("lvl", true)->value().toInt();
        save_configuration(); request->send(200, "text/plain", "OK");
        if(request->hasParam("form_type", true) && request->getParam("form_type", true)->value() == "wifi") { pending_reboot = true; reboot_timer = millis() + 1000; }
    });
    webServer.begin(); MDNS.begin("bacnet-gateway"); ArduinoOTA.begin();
}

void handle_network() { ArduinoOTA.handle(); if (pending_reboot && (millis() > reboot_timer)) ESP.restart(); }
bool is_authenticated(AsyncWebServerRequest *request) { if (!request->authenticate(sysCfg.admin_user, sysCfg.admin_pass)) { request->requestAuthentication(); return false; } return true; }
