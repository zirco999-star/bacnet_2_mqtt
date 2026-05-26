#ifndef Z_BACNET_H
#define Z_BACNET_H

#include "z_config.h"
#include <vector>

// --- STRUCTURE DE PERSISTANCE BINAIRE (Max 4KB pour NVS) ---
struct BACnetPersistenceObj {
    uint32_t val; // [TYPE:6][INSTANCE:25][UNUSED:1]
    char name[24];
    bool poll;
    uint16_t units;
};

struct BACnetPersistenceDev {
    uint32_t device_id;
    uint8_t mac_address;
    bool enabled;
    char name[32];
    char vendor[32];
    uint8_t count;
    bool discovery_done;
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

// --- TYPES D'OBJETS BACNET (ASHRAE 135) ---
enum BACnetObjectType {
    OBJ_ANALOG_INPUT = 0,
    OBJ_ANALOG_OUTPUT = 1,
    OBJ_ANALOG_VALUE = 2,
    OBJ_BINARY_INPUT = 3,
    OBJ_BINARY_OUTPUT = 4,
    OBJ_BINARY_VALUE = 5,
    OBJ_CALENDAR = 6,
    OBJ_COMMAND = 7,
    OBJ_DEVICE = 8,
    OBJ_EVENT_ENROLLMENT = 9,
    OBJ_FILE = 10,
    OBJ_GROUP = 11,
    OBJ_LOOP = 12,
    OBJ_MULTI_STATE_INPUT = 13,
    OBJ_MULTI_STATE_OUTPUT = 14,
    OBJ_NOTIFICATION_CLASS = 15,
    OBJ_PROGRAM = 16,
    OBJ_SCHEDULE = 17,
    OBJ_AVERAGING = 18,
    OBJ_MULTI_STATE_VALUE = 19,
    OBJ_TREND_LOG = 20,
    OBJ_LIFE_SAFETY_POINT = 21,
    OBJ_LIFE_SAFETY_ZONE = 22,
    OBJ_ACCUMULATOR = 23,
    OBJ_PULSE_CONVERTER = 24,
    OBJ_EVENT_LOG = 25,
    OBJ_GLOBAL_GROUP = 26,
    OBJ_TREND_LOG_MULTIPLE = 27,
    OBJ_LOAD_CONTROL = 28,
    OBJ_STRUCTURED_VIEW = 29,
    OBJ_ACCESS_DOOR = 30,
    OBJ_ACCESS_CREDENTIAL = 32,
    OBJ_ACCESS_POINT = 33,
    OBJ_ACCESS_RIGHTS = 34,
    OBJ_ACCESS_USER = 35,
    OBJ_ACCESS_ZONE = 36,
    OBJ_CREDENTIAL_DATA_INPUT = 37,
    OBJ_NETWORK_SECURITY = 38,
    OBJ_BITSTRING_VALUE = 39,
    OBJ_CHARACTERSTRING_VALUE = 40,
    OBJ_DATE_PATTERN_VALUE = 41,
    OBJ_DATE_VALUE = 42,
    OBJ_DATETIME_PATTERN_VALUE = 43,
    OBJ_DATETIME_VALUE = 44,
    OBJ_INTEGER_VALUE = 45,
    OBJ_LARGE_ANALOG_VALUE = 46,
    OBJ_OCTETSTRING_VALUE = 47,
    OBJ_POSITIVE_INTEGER_VALUE = 48,
    OBJ_TIME_PATTERN_VALUE = 49,
    OBJ_TIME_VALUE = 50,
    OBJ_NOTIFICATION_FORWARDER = 51,
    OBJ_ALERT_ENROLLMENT = 52,
    OBJ_CHANNEL = 53,
    OBJ_LIGHTING_OUTPUT = 54,
    OBJ_BINARY_LIGHTING_OUTPUT = 55,
    OBJ_NETWORK_PORT = 56,
    OBJ_TIMER = 57,
    OBJ_ELEVATOR_GROUP = 58,
    OBJ_LIFT = 59,
    OBJ_ESCALATOR = 60,
    OBJ_AUDIT_LOG = 61,
    OBJ_AUDIT_REPORTER = 62
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
String get_unit_text(uint16_t units);

#endif
