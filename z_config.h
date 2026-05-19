#ifndef Z_CONFIG_H
#define Z_CONFIG_H

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <atomic>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>

#define VERSION_GLOBAL "v4.2.9"

#define RX_PIN 18
#define TX_PIN 17
#define RTS_PIN 21
#define RS485_UART_PORT UART_NUM_1
#define WEB_PORT 80

struct Config {
    char wifi_ssid[32];
    char wifi_pass[64];
    bool static_ip;
    char local_ip[16];
    char gateway[16];
    char subnet[16];
    uint8_t mac_address;
    uint8_t max_master;
    uint32_t device_id;
    uint16_t apdu_timeout;
    uint8_t max_retries;
    char mqtt_server[32];
    uint16_t mqtt_port;
    char mqtt_user[32];
    char mqtt_pass[32];
    char mqtt_prefix[64];
    uint8_t log_level; 
    char admin_user[32];
    char admin_pass[64];
};

extern Config sysCfg;
extern Preferences preferences;
extern AsyncWebServer webServer;
extern AsyncWebSocket ws;
extern PubSubClient mqttClient;
extern WiFiClient mqttWifiClient;
extern bool is_ap_mode;
extern bool pending_reboot;
extern uint32_t reboot_timer;

#endif
