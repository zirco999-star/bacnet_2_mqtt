#ifndef Z_CONFIG_H
#define Z_CONFIG_H

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <atomic>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <PubSubClient.h>

// Pins
#define RX_PIN 18
#define TX_PIN 17
#define RTS_PIN 21
#define RS485_UART_PORT UART_NUM_1
#define TCP_PORT 6638
#define WEB_PORT 80

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
    char admin_pass[64] = "admin1234"; // Changé de zirconium à admin1234 pour cohérence
};

extern Config sysCfg;
extern Preferences preferences;
extern AsyncWebServer webServer;
extern AsyncWebSocket ws;
extern PubSubClient mqttClient;
extern WiFiClient mqttWifiClient;
extern QueueHandle_t uart_queue;
extern std::atomic<int> mstp_current_state;
extern volatile bool tcp_bridge_active;
extern bool is_ap_mode;
extern bool pending_reboot;
extern uint32_t reboot_timer;

#endif
