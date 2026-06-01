#ifndef Z_CONFIG_H
#define Z_CONFIG_H

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <atomic>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoOTA.h>
#include <mqtt_client.h>

// DEFAULT CONFIGURATION
#define VERSION_GLOBAL "v5.8.7"
#define DEFAULT_SSID    "Freebox-A4297A"
#define DEFAULT_STATIC_IP "192.168.1.50"
#define DEFAULT_GATEWAY "192.168.1.254"
#define DEFAULT_SUBNET "255.255.255.0"
#define DEFAULT_MAX_MASTER 5
#define DEFAULT_DEVICE_ID 123
#define DEFAULT_MAC_ADDRESS 1
#define DEFAULT_APDU_TIMEOUT 300
#define DEFAULT_MAX_RETRIES 3
#define DEFAULT_BACNET_POLL 30
#define DEFAULT_MQTT_SERVER "192.168.1.11"
#define DEFAULT_MQTT_POLL 30

#define DEFAULT_MAX_INFO_FRAMES 3
#define DEFAULT_HEARBEAT_INTERVAL 50000
#define DEFAULT_TOKEN_SKIP  0

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
    uint16_t bacnet_poll_interval;
    char mqtt_server[32];
    uint16_t mqtt_port;
    char mqtt_user[32];
    char mqtt_pass[32];
    char mqtt_prefix[64];
    uint16_t mqtt_poll_interval;
    uint8_t log_level;
    char admin_user[32];
    char admin_pass[64];
    uint32_t heartbeat_interval;
    uint8_t token_skip;
    uint8_t max_info_frames;
};
extern Config sysCfg;
extern AsyncWebServer webServer;
extern AsyncWebSocket ws;
extern esp_mqtt_client_handle_t mqtt_client;
extern bool is_ap_mode;
extern bool pending_reboot;
extern uint32_t reboot_timer;

#endif
