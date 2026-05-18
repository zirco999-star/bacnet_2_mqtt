/******************************************************************************
*  PROJET ZIRCON1UM - BACnet MS/TP transceiver
*  Version 1.9 - STABLE WiFi & External UI
*****************************************************************************/
#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Update.h>
#include <ArduinoOTA.h>
#include <stdarg.h>
#include <atomic>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "index_html.h" // UI externe pour eviter les bugs de pre-processeur Arduino

// ============================================================================
// PARAMÈTRES MATÉRIELS
// ============================================================================
#define RX_PIN 18
#define TX_PIN 17
#define RTS_PIN 21
#define RS485_UART_PORT UART_NUM_1
#define TCP_PORT 6638
#define WEB_PORT 80

// ============================================================================
// CONFIGURATION NVS
// ============================================================================
Preferences preferences;
struct Config {
    char wifi_ssid[32] = "";
    char wifi_pass[64] = "";
    bool static_ip = false;
    char local_ip[16] = "192.168.1.50";
    char gateway[16] = "192.168.1.254";
    char subnet[16] = "255.255.255.0";
    char mqtt_server[32] = "192.168.1.11";
    uint16_t mqtt_port = 1883;
    char mqtt_user[32] = "";
    char mqtt_pass[32] = "";
    char mqtt_prefix[64] = "bacnet";
    uint8_t mac_address = 1;
    uint8_t target_mac = 4;
    uint8_t max_master = 127;
    uint32_t polling_interval = 5000;
    uint8_t log_level = 2;
    char admin_user[32] = "admin";
    char admin_pass[64] = "zirconium";
} sysCfg;

// Globales
QueueHandle_t uart_queue;
std::atomic<int> mstp_current_state(0);
volatile bool tcp_bridge_active = false;
bool is_ap_mode = false;
bool pending_reboot = false;
uint32_t reboot_timer = 0;

WiFiServer tcpServer(TCP_PORT);
WiFiClient tcpClient;
AsyncWebServer webServer(WEB_PORT);
AsyncWebSocket ws("/ws-logs");
WiFiClient mqttWifiClient;
PubSubClient mqttClient(mqttWifiClient);

// ============================================================================
// SYSTEME DE LOGS
// ============================================================================
#define BACKLOG_SIZE 100
String log_backlog[BACKLOG_SIZE];
int backlog_index = 0;
int backlog_count = 0;
SemaphoreHandle_t log_mutex;

void init_log_system() { log_mutex = xSemaphoreCreateMutex(); }
void log_to_web(uint8_t level, const char* format, ...) {
    if (level > sysCfg.log_level) return;
    char fmsg[256];
    va_list args; va_start(args, format); vsnprintf(fmsg, sizeof(fmsg), format, args); va_end(args);
    String lvl = (level == 0) ? "ERR" : (level == 1) ? "WRN" : (level == 3) ? "DBG" : "INF";
    String line = "[" + lvl + "] " + String(fmsg);
    Serial.println(line);
    if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        log_backlog[backlog_index] = line;
        backlog_index = (backlog_index + 1) % BACKLOG_SIZE;
        if (backlog_count < BACKLOG_SIZE) backlog_count++;
        xSemaphoreGive(log_mutex);
    }
    ws.textAll(line);
}

// ============================================================================
// BACNET STUB
// ============================================================================
void mstp_rt_task(void *pvParameters) {
    uart_event_t event;
    for (;;) {
        if (xQueueReceive(uart_queue, (void *)&event, pdMS_TO_TICKS(500))) {
            if (event.type == UART_DATA) {
                uint8_t dummy[128];
                uart_read_bytes(RS485_UART_PORT, dummy, 128, 0);
            }
        }
    }
}

// ============================================================================
// RÉSEAU & INITIALISATION
// ============================================================================
void load_configuration() {
    if (!preferences.begin("sys", false)) return;
    if(preferences.getBytesLength("cfg") == sizeof(Config)) preferences.getBytes("cfg", &sysCfg, sizeof(Config));
    preferences.end();
}
void save_configuration() {
    preferences.begin("sys", false);
    preferences.putBytes("cfg", &sysCfg, sizeof(Config));
    preferences.end();
}

void setup_network_infrastructure() {
    load_configuration();
    
    // Reset WiFi
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(500);

    if (strlen(sysCfg.wifi_ssid) == 0) {
        is_ap_mode = true;
        WiFi.mode(WIFI_AP);
        WiFi.softAP("ZIRCON1UM_SETUP", "admin1234");
        Serial.println("Mode AP: 192.168.4.1");
    } else {
        is_ap_mode = false;
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(WIFI_PS_NONE);
        WiFi.setMinSecurity(WIFI_AUTH_WPA_PSK);
        
        if (sysCfg.static_ip) {
            IPAddress ip, gw, sn;
            if (ip.fromString(sysCfg.local_ip) && gw.fromString(sysCfg.gateway) && sn.fromString(sysCfg.subnet)) {
                WiFi.config(ip, gw, sn);
            }
        }
        
        WiFi.begin(sysCfg.wifi_ssid, sysCfg.wifi_pass);
        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) { delay(500); Serial.print("."); }
        
        if(WiFi.status() == WL_CONNECTED) {
            log_to_web(1, "IP: %s | Signal %d dBm", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
        } else {
            is_ap_mode = true;
            WiFi.mode(WIFI_AP);
            WiFi.softAP("ZIRCON1UM_RECOVERY", "admin1234");
        }
    }

    ArduinoOTA.begin();
    ws.onEvent([](AsyncWebSocket *s, AsyncWebSocketClient *c, AwsEventType t, void *a, uint8_t *d, size_t l) {
        if (t == WS_EVT_CONNECT) {
            for(int i=0; i<backlog_count; i++) c->text(log_backlog[(backlog_index - backlog_count + i + BACKLOG_SIZE) % BACKLOG_SIZE]);
        }
    });
    webServer.addHandler(&ws);
    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *r){ if(!r->authenticate(sysCfg.admin_user, sysCfg.admin_pass)) return r->requestAuthentication(); r->send_P(200, "text/html", INDEX_HTML); });
    webServer.on("/save", HTTP_POST, [](AsyncWebServerRequest *r){
        if(!r->authenticate(sysCfg.admin_user, sysCfg.admin_pass)) return r->requestAuthentication();
        if(r->hasParam("ssid", true)) strncpy(sysCfg.wifi_ssid, r->getParam("ssid", true)->value().c_str(), 31);
        if(r->hasParam("pass", true)) strncpy(sysCfg.wifi_pass, r->getParam("pass", true)->value().c_str(), 63);
        sysCfg.static_ip = r->hasParam("static_ip", true);
        if(r->hasParam("local_ip", true)) strncpy(sysCfg.local_ip, r->getParam("local_ip", true)->value().c_str(), 15);
        save_configuration();
        r->send(200, "text/plain", "OK. Rebooting...");
        pending_reboot = true; reboot_timer = millis();
    });
    webServer.begin();
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    init_log_system();
    setup_network_infrastructure();
    
    const uart_config_t uc = { .baud_rate = 38400, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT };
    uart_driver_install(RS485_UART_PORT, 2048, 2048, 20, &uart_queue, 0);
    uart_param_config(RS485_UART_PORT, &uc);
    uart_set_pin(RS485_UART_PORT, TX_PIN, RX_PIN, RTS_PIN, UART_PIN_NO_CHANGE);
    uart_set_mode(RS485_UART_PORT, UART_MODE_RS485_HALF_DUPLEX);
    xTaskCreatePinnedToCore(mstp_rt_task, "MSTP", 8192, NULL, 10, NULL, 1);
}

void loop() {
    ArduinoOTA.handle();
    if (pending_reboot && (millis() - reboot_timer > 2000)) ESP.restart();
    static uint32_t ls = 0;
    if(millis() - ls > 10000) {
        ls = millis();
        if (WiFi.status() == WL_CONNECTED) log_to_web(2, "Heap: %d KB | RSSI: %d dBm", (int)ESP.getFreeHeap() / 1024, (int)WiFi.RSSI());
    }
    vTaskDelay(pdMS_TO_TICKS(10));
}
