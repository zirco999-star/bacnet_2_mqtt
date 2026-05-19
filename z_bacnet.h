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
    uint32_t last_update;
};

struct BACnetDevice {
    uint8_t mac_address;
    uint32_t device_id;
    std::vector<BACnetObject> objects;
    uint32_t last_seen;
    bool discovery_done; // True si on a lu son Object_List
};

extern std::vector<BACnetDevice> bacnet_network_cache;

// --- QUEUES INTER-BRIQUES (FreeRTOS) ---
enum BACnetJobType {
    JOB_WHO_IS,
    JOB_READ_PROP,
    JOB_WRITE_PROP
};

struct BACnetJob {
    BACnetJobType type;
    uint8_t target_mac;     // MS/TP MAC (0-127)
    uint16_t obj_type;
    uint32_t obj_instance;
    uint8_t prop_id;        // Par defaut 85 (Present_Value)
    float write_value;      // Valeur a ecrire
    uint8_t priority;       // Priorite BACnet (1-16)
};

extern QueueHandle_t bacnet_job_queue;

// --- API PUBLIQUE BACNET ---
void setup_bacnet_engine();
bool enqueue_bacnet_job(BACnetJob job);

#endif
