#ifndef Z_BACNET_H
#define Z_BACNET_H

#include "z_config.h"
#include <vector>

// --- STATISTIQUES MS/TP ---
struct BACnet_Stats {
    uint32_t ms_msgs_rx;
    uint32_t ms_msgs_tx;
    uint32_t tokens_seen;
    uint32_t pfm_replies;
    uint32_t errors_crc;
    uint8_t current_index; // Pour UI
    uint8_t total_objects; // Pour UI
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
    OBJ_MULTI_STATE_INPUT = 13,
    OBJ_MULTI_STATE_OUTPUT = 14,
    OBJ_MULTI_STATE_VALUE = 19,
    OBJ_DEVICE = 8
};

// --- BASE DE DONNÉES EN RAM ---
struct BACnetObject {
    uint16_t type;
    uint32_t instance;
    float present_value;
    bool is_commandable;
    bool enabled;
    uint32_t last_update;
    uint16_t units;
    std::vector<String> state_texts;
};

struct BACnetDevice {
    uint8_t mac_address;
    uint32_t device_id;
    std::vector<BACnetObject> objects;
    uint32_t last_seen;
    bool discovery_done; 
};

extern std::vector<BACnetDevice> bacnet_network_cache;

// --- QUEUES INTER-BRIQUES (FreeRTOS) ---
enum BACnetJobType {
    JOB_WHO_IS,
    JOB_READ_PROP,
    JOB_WRITE_PROP,
    JOB_READ_UNITS,
    JOB_CHECK_COMMANDABLE,
    JOB_READ_STATE_TEXT
};

struct BACnetJob {
    BACnetJobType type;     // Le type d'ordre
    uint8_t target_mac;     // Adresse MAC MS/TP cible
    uint16_t obj_type;      // Type d'objet BACnet (ex: 1 = Analog Output)
    uint32_t obj_instance;  // Instance de l'objet (ex: 0)
    uint8_t prop_id;        // Propriété à lire/écrire (ex: 85 = Present_Value)
    float write_value;      // Valeur à écrire (utilisé si type == JOB_WRITE_PROP)
    uint8_t priority;       // Priorité d'écriture BACnet (ex: 16 par défaut)
};

enum MQTTPublishDataType {
    PUB_FLOAT,
    PUB_INT,
    PUB_STRING
};

struct MQTTPublishJob {
    uint32_t device_id;
    uint16_t obj_type;
    uint32_t obj_instance;
    uint8_t prop_id;
    MQTTPublishDataType data_type;
    float value_float;
    int32_t value_int;
    char value_string[32];
};

extern QueueHandle_t mqtt_publish_queue;
bool enqueue_mqtt_publish(MQTTPublishJob pubJob);

extern QueueHandle_t bacnet_job_queue;

// --- API PUBLIQUE BACNET ---
void setup_bacnet_engine();
bool enqueue_bacnet_job(BACnetJob job);

#endif
