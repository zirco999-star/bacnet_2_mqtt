#ifndef Z_BACNET_H
#define Z_BACNET_H

#include "z_config.h"
#include <vector>

// --- STRUCTURE DE PERSISTANCE BINAIRE (Max 4KB pour NVS) ---
struct BACnetPersistenceObj {
    uint32_t val; // [TYPE:6][INSTANCE:25][UNUSED:1]
    char name[24];
    bool poll;
};

struct BACnetPersistenceDev {
    uint32_t device_id;
    bool enabled;
    char name[32];
    char vendor[32];
    uint8_t count;
    BACnetPersistenceObj objects[100]; // Max 100 points
};

// --- STATISTIQUES MS/TP ---
struct BACnet_Stats {
    uint32_t ms_msgs_rx;
    uint32_t ms_msgs_tx;
    uint32_t tokens_seen;
    uint32_t pfm_replies;
    uint32_t errors_crc;
    uint8_t current_index; 
    uint8_t total_objects; 
};
extern BACnet_Stats bacnetStats;

// --- TYPES D'OBJETS BACNET ---
enum BACnetObjectType {
    OBJ_ANALOG_INPUT = 0, OBJ_ANALOG_OUTPUT = 1, OBJ_ANALOG_VALUE = 2,
    OBJ_BINARY_INPUT = 3, OBJ_BINARY_OUTPUT = 4, OBJ_BINARY_VALUE = 5,
    OBJ_MULTI_STATE_INPUT = 13, OBJ_MULTI_STATE_OUTPUT = 14, OBJ_MULTI_STATE_VALUE = 19,
    OBJ_DEVICE = 8
};

// --- BASE DE DONNÉES EN RAM ---
struct BACnetObject {
    uint16_t type;
    uint32_t instance;
    String name;
    float present_value;
    bool is_commandable;
    bool enabled;
    uint32_t last_update;
    uint16_t units;
    String unit_text;
    std::vector<String> state_texts;
};

struct BACnetDevice {
    uint8_t mac_address;
    uint32_t device_id;
    String name;
    String vendor;
    String version;
    bool enabled;
    std::vector<BACnetObject> objects;
    uint32_t last_seen;
    bool discovery_done; 
};

extern std::vector<BACnetDevice> bacnet_network_cache;
extern SemaphoreHandle_t cache_mutex;

enum BACnetJobType { JOB_WHO_IS, JOB_READ_PROP, JOB_WRITE_PROP, JOB_READ_UNITS, JOB_CHECK_COMMANDABLE, JOB_READ_STATE_TEXT };
struct BACnetJob {
    BACnetJobType type;
    uint8_t target_mac;
    uint16_t obj_type;
    uint32_t obj_instance;
    uint8_t prop_id;
    float write_value;
    uint8_t priority;
};

struct MQTTPublishJob {
    uint32_t device_id;
    uint16_t obj_type;
    uint32_t obj_instance;
    uint8_t prop_id;
    char value_string[32];
};

extern QueueHandle_t mqtt_publish_queue;
extern QueueHandle_t bacnet_job_queue;

void setup_bacnet_engine();
bool enqueue_bacnet_job(BACnetJob job);
bool enqueue_mqtt_publish(MQTTPublishJob pubJob);

#endif
