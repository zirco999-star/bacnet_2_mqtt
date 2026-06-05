#ifndef Z_BACNET_H
#define Z_BACNET_H

#include "z_config.h"
#include <vector>

// --- STRUCTURE DE PERSISTANCE BINAIRE (v1 Legacy) ---
#pragma pack(push, 1)
struct BACnetPersistenceObj {
    uint32_t val;         // [TYPE:10][INSTANCE:22]
    char name[32];        // friendly name
    char unit_text[12];
    bool poll;
    bool name_published;
    bool is_commandable;  
    uint16_t units;       
    uint8_t states_count; 
    float min_value;      // v6.4.3: Prop 69
    float max_value;      // v6.4.3: Prop 65
    char last_ha_component[16]; // v6.3.6: For ghost entity cleanup (sensor/select/switch)
}; // Total: 4 + 32 + 12 + 3 + 2 + 1 + 4 + 4 + 16 = 78 bytes

struct BACnetPersistenceDev {
    uint32_t device_id;
    uint8_t mac_address;
    bool enabled;
    char name[32];
    char vendor[32];
    uint16_t count;       // uint16_t for > 255 objects safety
    bool discovery_done;
    uint16_t disc_obj_idx;
    uint8_t disc_step;
};

// Structure de transport pour les pages (pour respecter la limite 1984 octets)
struct BACnetPersistencePage {
    uint32_t device_id;
    uint16_t page_index;
    BACnetPersistenceObj objects[20]; // 20 * 48 = 960 octets ==> OK pour NVS
};
#pragma pack(pop)

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

// --- ÉTATS DE DÉCOUVERTE ---
enum DISC_STEP_T {
    DISC_DEV_ID = 0,
    DISC_DEV_NAME,
    DISC_DEV_VENDOR,
    DISC_OBJ_COUNT,
    DISC_OBJ_OID,
    DISC_OBJ_NAME,
    DISC_OBJ_UNITS,
    DISC_OBJ_MIN,         // Prop 69
    DISC_OBJ_MAX,         // Prop 65
    DISC_OBJ_STATES,
    DISC_OBJ_COMMANDABLE, // Propriété 87 (Priority_Array)
    DISC_OBJ_VALUE
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

// --- BASE DE DONNÉES EN RAM ---
struct BACnetObject {
    uint16_t type = 65535;
    uint32_t instance = 0;
    char name[50] = "Unknown";
    float present_value = 0.0f;
    bool enabled = false;
    bool name_published = false;
    char last_mqtt_name[50] = "";
    uint32_t last_update = 0;
    uint16_t expected_states_count = 0;
    uint16_t units = 95;
    char unit_text[20] = "";
    float min_value = NAN;
    float max_value = NAN;
    bool discovery_done = false;
    bool is_commandable = false; // Prop 87
    char last_ha_component[16] = ""; // Pour nettoyer les doublons si on change sensor -> select
    // state_texts reste dynamique en RAM, mais n'est pas persisté (ou alors avec une logique à part)
    std::vector<String> state_texts; 
};

struct BACnetDevice {
    uint8_t mac_address = 0;
    uint32_t device_id = 4194303;
    String name = "";
    String vendor = "";
    String version = "";
    bool enabled = true;
    std::vector<BACnetObject> objects;
    uint32_t last_seen = 0;
    bool discovery_done = false;
    DISC_STEP_T disc_step = DISC_DEV_ID;
    uint16_t disc_obj_idx = 0;
    bool reload_single = false;
    bool recovery_mode = false;

    BACnetDevice() {} 
};

extern std::vector<BACnetDevice> bacnet_network_cache;
extern SemaphoreHandle_t cache_mutex;

extern uint32_t period_poll_count;
extern uint32_t period_mqtt_pub_count;

// --- CONSTANTES DE TEMPS ASHRAE 135 (v6.3.0) ---
const uint32_t T_REPLY_TIMEOUT_US = 265000; // 265 ms (ASHRAE: 255-300ms)
const uint32_t T_USAGE_TIMEOUT_US = 15000;  // 15 ms (T_usage_timeout)
const uint32_t T_FRAME_ABORT_US   = 50000;  // 50 ms pour abandonner une trame incomplète

// --- STRUCTURE DE TRANSPORT MS/TP ---
struct MSTP_Frame {
    uint8_t type;
    uint8_t dest;
    uint8_t src;
    uint16_t len;
    uint8_t data[512]; // APDU
    uint32_t timestamp_us;
};

// --- PROTOTYPES REFACTORISATION MODULAIRE (v6.3.0) ---
void handle_mstp_idle();
void handle_mstp_use_token();
void handle_mstp_wait_for_reply();
void handle_mstp_pass_token();
void handle_mstp_poll_for_master();
void process_incoming_frame(MSTP_Frame &frame);
void execute_bacnet_work();
void execute_discovery_logic(BACnetDevice &dev);
void execute_polling_logic(BACnetDevice &dev);
bool has_bacnet_work();

// --- ÉTATS FSM MS/TP (ASHRAE 135) ---
enum RX_STATE { 
    RX_IDLE, 
    RX_PREAMBLE, 
    RX_HEADER, 
    RX_HEADER_CRC, 
    RX_DATA, 
    RX_DATA_CRC 
};

enum MSTP_MASTER_STATE {
    MSTP_INITIALIZE,
    MSTP_IDLE,
    MSTP_USE_TOKEN,
    MSTP_WAIT_FOR_REPLY,
    MSTP_DONE_WITH_TOKEN,
    MSTP_PASS_TOKEN,
    MSTP_NO_TOKEN,
    MSTP_POLL_FOR_MASTER,
    MSTP_ANSWER_DATA_REQUEST
};
enum BACnetJobType { JOB_WHO_IS, JOB_I_AM, JOB_WRITE_PROP };
struct BACnetJob {
    BACnetJobType type;
    uint8_t target_mac;
    uint16_t obj_type;
    uint32_t obj_instance;
    uint8_t prop_id;
    uint16_t array_index; // Gardé au cas où, mais n'est plus utilisé pour State_Text
    float write_value;
    uint8_t priority;
    char name[50]; 
};

extern QueueHandle_t bacnet_job_queue;

void setup_bacnet_engine();
void bacnet_abort_current_transaction();
bool enqueue_bacnet_job(BACnetJob job);
void publish_all_names();
String get_unit_text(uint16_t units);

uint16_t build_read_property_apdu(uint8_t* buffer, uint8_t invoke_id, uint16_t obj_type, uint32_t obj_instance, uint8_t property_id, int32_t array_index);
uint16_t build_write_property_name_apdu(uint8_t* buffer, uint8_t invoke_id, uint16_t obj_type, uint32_t obj_instance, const char* new_name);
uint16_t build_write_property_value_apdu(uint8_t* buffer, uint8_t invoke_id, uint16_t obj_type, uint32_t obj_instance, uint8_t prop_id, float value);

#endif
