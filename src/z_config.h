#ifndef Z_CONFIG_H
#define Z_CONFIG_H

// Required libraries
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <atomic>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoOTA.h>
#include <mqtt_client.h>

// DEFAULT SYSTEM CONFIGURATION
#define VERSION_GLOBAL "v6.4.7"
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
#define DEFAULT_HA_DISCOVER true

#define DEFAULT_NUM_MIN -100.0f
#define DEFAULT_NUM_MAX 100.0f
#define DEFAULT_NUM_STEP 1.0f

#define DEFAULT_MAX_INFO_FRAMES 3
#define DEFAULT_HEARBEAT_INTERVAL 50000
#define DEFAULT_TOKEN_SKIP  0

// HARDWARE PIN CONFIGURATION (RS485)
#define RX_PIN 18
#define TX_PIN 17
#define RTS_PIN 21
#define RS485_UART_PORT UART_NUM_1
#define WEB_PORT 80

// LOG LEVELS (Standard Syslog)
#define LOG_ERROR 1
#define LOG_WARN  2
#define LOG_INFO  3
#define LOG_DEBUG 4

// Main configuration structure to hold all system settings saved in memory
struct Config {
    char wifi_ssid[32];             // WiFi network name
    char wifi_pass[64];             // WiFi password
    bool static_ip;                 // True if using a fixed IP address instead of DHCP
    char local_ip[16];              // Fixed IP address
    char gateway[16];               // Router IP address
    char subnet[16];                // Network mask
    uint8_t mac_address;            // BACnet MS/TP node MAC address
    uint8_t max_master;             // Highest MAC address on the MS/TP network
    uint32_t device_id;             // Unique BACnet device ID
    uint16_t apdu_timeout;          // Time to wait for a BACnet reply (in milliseconds)
    uint8_t max_retries;            // Number of times to retry sending a BACnet message
    uint16_t bacnet_poll_interval;  // Time between BACnet data updates
    char mqtt_server[32];           // IP or domain of the MQTT broker
    uint16_t mqtt_port;             // MQTT broker port (usually 1883)
    char mqtt_user[32];             // MQTT login username
    char mqtt_pass[32];             // MQTT login password
    char mqtt_prefix[64];           // Base topic for MQTT messages
    uint16_t mqtt_poll_interval;    // Time between MQTT status updates
    bool ha_discover;               // Enable Home Assistant auto-discovery
    float default_number_min;       // Default MIN for 'number' entities in HA
    float default_number_max;       // Default MAX for 'number' entities in HA
    float default_number_step;      // Default STEP for 'number' entities in HA
    uint8_t log_level;              // Amount of detail in system logs
    char admin_user[32];            // Web interface admin username
    char admin_pass[64];            // Web interface admin password
    uint32_t heartbeat_interval;    // Time between internal system health checks
    uint8_t token_skip;             // Number of MS/TP tokens to pass before sending data
    uint8_t max_info_frames;        // Max number of data frames sent per token
};

// Global variables shared across the entire project
extern Config sysCfg;                           // Global settings object
extern AsyncWebServer webServer;                // Global web server object
extern AsyncWebSocket ws;                       // Global WebSocket for live logs
extern esp_mqtt_client_handle_t mqtt_client;    // Global MQTT connection object
extern bool is_ap_mode;                         // True if the device is hosting its own WiFi network
extern bool pending_reboot;                     // True if the device is waiting to restart
extern uint32_t reboot_timer;                   // Timer used to delay the restart

#endif
