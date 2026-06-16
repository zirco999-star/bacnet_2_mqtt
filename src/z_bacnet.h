#ifndef Z_BACNET_H
#define Z_BACNET_H

#include "z_config.h"
#include <vector>

// --- MASQUES DE BITS POUR STATUS_FLAGS (ASHRAE 135) ---
#define BACNET_STATUS_IN_ALARM         0x01 // Bit 0 : Alarme active
#define BACNET_STATUS_FAULT            0x02 // Bit 1 : Défaut matériel/sonde
#define BACNET_STATUS_OVERRIDDEN       0x04 // Bit 2 : Forçage local physique
#define BACNET_STATUS_OUT_OF_SERVICE   0x08 // Bit 3 : Hors-service / Mode Hack

// --- STRUCTURE DE PERSISTANCE BINAIRE (v1 Legacy) ---
#pragma pack(push, 1)
struct BACnetPersistenceObj {
    uint32_t ulVal;         // [TYPE:10][INSTANCE:22]
    char cName[32];        // friendly name
    char cUnitText[12];
    bool xEnabled;
    bool xNamePublished;
    bool xIsCommandable;  
    uint16_t usUnits;       
    uint8_t ucExpectedStatesCount;
    uint8_t ucStatusFlags;     // v7.0.0: Stocke les 4 bits de Status_Flags [InAlarm:Bit0][Fault:Bit1][Overridden:Bit2][OutOfService:Bit3]
    float fMinValue;      // v6.4.3: Prop 69
    float fMaxValue;      // v6.4.3: Prop 65
    char cLastHaComponent[16]; // v6.3.6: For ghost entity cleanup (sensor/select/switch)
    float fStepValue;     // v6.9.0
    char cMinRef[6];      // v6.9.0
    char cMaxRef[6];      // v6.9.0
}; // Total: 95 bytes

struct BACnetPersistenceDev {
    uint32_t ulDeviceId;
    uint8_t ucMacAddress;
    bool xEnabled;
    char cName[32];
    char cVendor[32];
    uint16_t usCount;       // uint16_t for > 255 objects safety
    bool xDiscoveryDone;
    uint16_t usDiscObjIdx;
    uint8_t ucDiscStep;
    // Nouveaux champs v6.8.3
    uint16_t usMaxApduLengthAccepted; // Prop 62
    uint32_t ulApduTimeout;           // Prop 11
    uint8_t ucNumberOfApduRetries;    // Prop 73
    bool xSupportsRpm;                // Fallback flag
};

// Structure de transport pour les pages (pour respecter la limite 1984 octets)
struct BACnetPersistencePage {
    uint32_t ulDeviceId;
    uint16_t page_index;
    BACnetPersistenceObj objects[20]; // 20 * 95 = 1900 octets ==> OK pour NVS (< 1984)
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
    DISC_DEV_MAX_APDU,
    DISC_DEV_TIMEOUT,
    DISC_DEV_RETRIES,
    DISC_OBJ_COUNT,
    DISC_OBJ_OID,
    DISC_OBJ_NAME,
    DISC_OBJ_UNITS,
    DISC_OBJ_MIN,         // Prop 69
    DISC_OBJ_MAX,         // Prop 65
    DISC_OBJ_STATES,
    DISC_OBJ_COMMANDABLE, // Propriété 87 (Priority_Array)
    DISC_OBJ_STATUS_FLAGS, //propriété 111 : lu avant la valeur pour ne pas remonter les valeurs d'un objet en defaut.
    DISC_OBJ_VALUE
};

// --- STATISTIQUES MS/TP ---
struct BACnet_Stats {
    uint32_t ulMsMsgsRx;
    uint32_t ulMsMsgsTx;
    uint32_t ulTokensSeen;
    uint32_t ulPfmReplies;
    uint32_t ulErrorsCrc;
    uint8_t ucCurrentIndex; 
    uint8_t ucTotalObjects; 
    bool xRingActive;
};
extern BACnet_Stats bacnetStats;

// --- BASE DE DONNÉES EN RAM ---
struct BACnetObject {
    uint16_t usType = 65535;
    uint32_t ulInstance = 0;
    char cName[50] = "Unknown";
    float fPresentValue = NAN;
    bool xEnabled = false;
    bool xNamePublished = false;
    char cLastMqttName[50] = "";
    uint32_t ulLastUpdate = 0;
    uint16_t ucExpectedStatesCount = 0;
    uint8_t ucStatusFlags = 0;
    uint16_t usUnits = 95;
    char cUnitText[20] = "";
    float fMinValue = NAN;
    float fMaxValue = NAN;
    float fStepValue = 1.0f;
    char cMinRef[6] = "";
    char cMaxRef[6] = "";
    bool xDiscoveryDone = false;
    bool xIsCommandable = false; // Prop 87
    char cLastHaComponent[16] = ""; // Pour nettoyer les doublons si on change sensor -> select
    // state_texts reste dynamique en RAM, mais n'est pas persistant en NVS 
    std::vector<String> state_texts;
    
    // --- ENCAPSULATION DES ETATS (HELPERS) ---
    inline bool isInAlarm() const { return (ucStatusFlags & BACNET_STATUS_IN_ALARM) != 0; }
    inline bool isFault() const { return (ucStatusFlags & BACNET_STATUS_FAULT) != 0; }
    inline bool isOverridden() const { return (ucStatusFlags & BACNET_STATUS_OVERRIDDEN) != 0; }
    inline bool isOutOfService() const { return (ucStatusFlags & BACNET_STATUS_OUT_OF_SERVICE) != 0; }

};

struct BACnetDevice {
    uint8_t ucMacAddress = 0;
    uint32_t ulDeviceId = 4194303;
    String name = "";
    String vendor = "";
    String version = "";
    bool xEnabled = true;

    // Nouveaux paramètres réseau dynamiques (v6.8.3)
    uint16_t usMaxApduLengthAccepted = 480;
    uint32_t ulApduTimeout = 3000;
    uint8_t ucNumberOfApduRetries = 3;
    bool xSupportsRpm = true;

    std::vector<BACnetObject> objects;
    uint32_t last_seen = 0;
    bool xDiscoveryDone = false;
    DISC_STEP_T ucDiscStep = DISC_DEV_ID;
    uint16_t usDiscObjIdx = 0;
    bool xReloadSingle = false;
    bool xRecoveryMode = false;
    bool xDirty = false;           // v6.8.8: Drapeau pour sauvegarde différée
    uint32_t ulLastDirtyTime = 0;  // v6.8.8: Timestamp du dernier changement

    BACnetDevice() {}
    };

#include <map>
extern std::vector<BACnetDevice> bacnet_network_cache;
extern SemaphoreHandle_t cache_mutex;
extern std::map<String, std::vector<std::pair<uint32_t, uint32_t>>> ha_dependencies;

extern uint32_t period_poll_count;
extern uint32_t period_mqtt_pub_count;

// --- CONSTANTES DE TEMPS ASHRAE 135 (v6.3.0) ---
const uint32_t T_REPLY_TIMEOUT_US = 280000; // 280 ms (Support ECB-203 lent)
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
enum BACnetJobType { JOB_WHO_IS, JOB_I_AM, JOB_WRITE_PROP, JOB_READ_PROP };
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
String get_unit_text(uint16_t usUnits);

uint16_t build_read_property_multiple_apdu(uint8_t* buffer, uint8_t invoke_id, std::vector<BACnetObject*>& objects, uint8_t property_id);
uint16_t build_read_property_apdu(uint8_t* buffer, uint8_t invoke_id, uint16_t obj_type, uint32_t obj_instance, uint8_t property_id, int32_t array_index);
uint16_t build_write_property_name_apdu(uint8_t* buffer, uint8_t invoke_id, uint16_t obj_type, uint32_t obj_instance, const char* new_name);
uint16_t build_write_property_value_apdu(uint8_t* buffer, uint8_t invoke_id, uint16_t obj_type, uint32_t obj_instance, uint8_t prop_id, float value, uint8_t ucPriority);
uint16_t build_write_property_outofservice_apdu(uint8_t* buffer, uint8_t invoke_id, uint16_t obj_type, uint32_t obj_instance, bool out_of_service);
uint16_t build_i_am_apdu(uint8_t* buffer, uint32_t device_instance, uint16_t max_apdu, uint16_t vendor_id);

#endif
