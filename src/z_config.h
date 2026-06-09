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
#define configVERSION_GLOBAL "v6.8.1"

#define configDEFAULT_SSID    "Freebox-A4297A"
#define configDEFAULT_STATIC_IP "192.168.1.50"
#define configDEFAULT_GATEWAY "192.168.1.254"
#define configDEFAULT_SUBNET "255.255.255.0"
#define configDEFAULT_MAX_MASTER 5
#define configDEFAULT_DEVICE_ID 123
#define configDEFAULT_MAC_ADDRESS 1
#define configDEFAULT_APDU_TIMEOUT 300
#define configDEFAULT_MAX_RETRIES 3
#define configDEFAULT_BACNET_POLL 30
#define configDEFAULT_MQTT_SERVER "192.168.1.11"
#define configDEFAULT_MQTT_POLL 30
#define configDEFAULT_HA_DISCOVER pdTRUE

#define configDEFAULT_NUM_MIN -100.0f
#define configDEFAULT_NUM_MAX 100.0f
#define configDEFAULT_NUM_STEP 1.0f

#define configDEFAULT_MAX_INFO_FRAMES 3
#define configDEFAULT_HEARTBEAT_INTERVAL 50000
#define configDEFAULT_TOKEN_SKIP  0

// HARDWARE PIN CONFIGURATION (RS485)
#define configRX_PIN 18
#define configTX_PIN 17
#define configRTS_PIN 21
#define configRS485_UART_PORT UART_NUM_1
#define configWEB_PORT 80

// Project Definitions
#ifndef pdTRUE
#define pdTRUE 1
#endif
#ifndef pdFALSE
#define pdFALSE 0
#endif

// LOG LEVELS (Standard Syslog) - Prefixed with pd for project definitions
#define pdLOG_ERROR 1
#define pdLOG_WARN  2
#define pdLOG_INFO  3
#define pdLOG_DEBUG 4

// Main configuration structure to hold all system settings saved in memory
struct Config {
    char cWifiSsid[32];             // WiFi network name
    char cWifiPass[64];             // WiFi password
    BaseType_t xStaticIp;           // pdTRUE if using a fixed IP address instead of DHCP
    char cLocalIp[16];              // Fixed IP address
    char cGateway[16];              // Router IP address
    char cSubnet[16];               // Network mask
    uint8_t ucMacAddress;           // BACnet MS/TP node MAC address
    uint8_t ucMaxMaster;            // Highest MAC address on the MS/TP network
    uint32_t ulDeviceId;            // Unique BACnet device ID
    uint16_t usApduTimeout;         // Time to wait for a BACnet reply (in milliseconds)
    uint8_t ucMaxRetries;           // Number of times to retry sending a BACnet message
    uint16_t usBacnetPollInterval;  // Time between BACnet data updates
    char cMqttServer[32];           // IP or domain of the MQTT broker
    uint16_t usMqttPort;            // MQTT broker port (usually 1883)
    char cMqttUser[32];             // MQTT login username
    char cMqttPass[32];             // MQTT login password
    char cMqttPrefix[64];           // Base topic for MQTT messages
    uint16_t usMqttPollInterval;    // Time between MQTT status updates
    BaseType_t xHaDiscover;         // Enable Home Assistant auto-discovery
    float fDefaultNumberMin;        // Default MIN for 'number' entities in HA
    float fDefaultNumberMax;        // Default MAX for 'number' entities in HA
    float fDefaultNumberStep;       // Default STEP for 'number' entities in HA
    uint8_t ucLogLevel;             // Amount of detail in system logs
    char cAdminUser[32];            // Web interface admin username
    char cAdminPass[64];            // Web interface admin password
    uint32_t ulHeartbeatInterval;   // Time between internal system health checks
    uint8_t ucTokenSkip;            // Number of MS/TP tokens to pass before sending data
    uint8_t ucMaxInfoFrames;        // Max number of data frames sent per token
};

// Global variables shared across the entire project
extern Config sysCfg;                           // Global settings object
extern AsyncWebServer webServer;                // Global web server object
extern AsyncWebSocket ws;                       // Global WebSocket for live logs
extern esp_mqtt_client_handle_t mqtt_client;    // Global MQTT connection object
extern BaseType_t xIsApMode;                    // pdTRUE if the device is hosting its own WiFi network
extern BaseType_t xPendingReboot;               // pdTRUE if the device is waiting to restart
extern uint32_t ulRebootTimer;                  // Timer used to delay the restart

#endif
