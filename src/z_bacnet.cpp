#include "z_bacnet.h"
#include <string.h>
#include <algorithm>
#include "driver/uart.h"
#include "z_network.h" 
#include "z_mqtt.h"
#include "z_nvs.h"

uint32_t period_poll_count = 0;
BACnet_Stats bacnetStats = {0, 0, 0, 0, 0, 0, 0, false};
std::vector<BACnetDevice> bacnet_network_cache;
std::map<String, std::vector<std::pair<uint32_t, uint32_t>>> ha_dependencies;
QueueHandle_t bacnet_job_queue = NULL;
QueueHandle_t mac_discovery_queue = NULL; // Axe 4: Pour auto-discovery MAC sans mutex
QueueHandle_t uart_evt_queue = NULL;
QueueHandle_t mstp_rx_queue = NULL;
SemaphoreHandle_t cache_mutex = NULL;

enum StorageType { 
    STORE_NONE, 
    STORE_DEV_ID, 
    STORE_DEV_NAME, 
    STORE_DEV_VENDOR,
    STORE_DEV_UINT, 
    STORE_OBJ_OID, 
    STORE_OBJ_NAME, 
    STORE_OBJ_UNITS, 
    STORE_OBJ_REAL, 
    STORE_OBJ_STATES,
    STORE_OBJ_STATUS_FLAGS,
    STORE_OBJ_VALUE 
};

struct DiscoveryStepConfig {
    uint16_t prop;
    int32_t idx;
    StorageType storage;
};

static const DiscoveryStepConfig DISCOVERY_STEPS[] = {
    {75, -1, STORE_DEV_ID},     // DISC_DEV_ID
    {77, -1, STORE_DEV_NAME},   // DISC_DEV_NAME
    {121,-1, STORE_DEV_VENDOR}, // DISC_DEV_VENDOR
    {62, -1, STORE_DEV_UINT},   // DISC_DEV_MAX_APDU
    {11, -1, STORE_DEV_UINT},   // DISC_DEV_TIMEOUT
    {73, -1, STORE_DEV_UINT},   // DISC_DEV_RETRIES
    {76,  0, STORE_DEV_UINT},   // DISC_OBJ_COUNT
    {76, -2, STORE_OBJ_OID},    // DISC_OBJ_OID
    {77, -1, STORE_OBJ_NAME},   // DISC_OBJ_NAME
    {117,-1, STORE_OBJ_UNITS},  // DISC_OBJ_UNITS
    {69, -1, STORE_OBJ_REAL},   // DISC_OBJ_MIN
    {65, -1, STORE_OBJ_REAL},   // DISC_OBJ_MAX
    {110, 0, STORE_OBJ_STATES}, // DISC_OBJ_STATES
    {87, -1, STORE_NONE},       // DISC_OBJ_COMMANDABLE
    {111, -1, STORE_OBJ_STATUS_FLAGS}, // DISC_OBJ_STATUS_FLAGS
    {85, -1, STORE_OBJ_VALUE}   // DISC_OBJ_VALUE
};


// --- VARIABLES D'ÉTAT MS/TP (v6.3.0) ---
static uint8_t last_apdu[512];
static uint16_t last_apdu_len = 0;
static uint8_t last_sent_invoke_id = 0;
static uint8_t next_invoke_id = 10;
static uint8_t retry_count = 0;
static BACnetJob pending_write_job;
static BACnetJob xPendingReadJob;
static bool xReadJobPending = false;

static MSTP_MASTER_STATE mstp_state = MSTP_INITIALIZE;
static uint32_t timer_silence_us = 0;
static uint32_t last_rx_time_us = 0;
static uint8_t next_station = 127;
static uint8_t poll_station = 0;
static bool ring_stable = false;
static uint8_t frame_type = 0, dest_mac = 0, src_mac = 0;
static uint16_t data_len = 0;
static uint8_t data_buf[512 + 2];
static bool ReceivedValidFrame = false;
static bool waiting_for_reply = false;
static uint8_t frame_count = 0;
static uint8_t token_skip_count = 0;
static uint16_t current_poll_idx = 0;
static uint8_t current_dev_idx = 0;
static std::vector<BACnetObject> FSM_old_objects;
static uint32_t last_who_is_time = 0;

static const uint32_t T_TURNAROUND_US = 1050;
static const uint32_t T_NO_TOKEN_US = 500000;

// --- PROTOTYPES INTERNES (v6.3.0) ---
static bool validate_rx_header_crc(const uint8_t *header);
static bool validate_rx_data_crc(const uint8_t *data, size_t len);
static uint8_t calc_header_crc(uint8_t *data, size_t len);
static uint16_t calc_data_crc(uint8_t *data, size_t len);
static void send_mstp_frame(uint8_t target, uint8_t type, const uint8_t* apdu, uint16_t len);
bool has_bacnet_work();
void check_ha_dependencies(uint32_t did, uint16_t type, uint32_t inst);

// --- TÂCHE DE RÉCEPTION DÉDIÉE (Priorité Critique) ---
static void mstp_rx_task(void *pv) {
    uint8_t rx_byte; 
    uint8_t header[6]; uint8_t header_idx=0;
    uint16_t data_len_local=0, data_idx=0;
    uint8_t data_buf_local[512+2];
    enum RX_STATE_INTERNAL { RX_IDLE, RX_PREAMBLE_55, RX_HEADER, RX_DATA, RX_CRC16_L, RX_CRC16_H };
    RX_STATE_INTERNAL rx_state = RX_IDLE;
    MSTP_Frame frame;

    z_log(pdLOG_INFO,"MSTP","RX Task Started (Priority 20)\n");

    for (;;) {
        // Lecture bloquante pour économiser le CPU si pas de trafic
        if (uart_read_bytes(RS485_UART_PORT, &rx_byte, 1, portMAX_DELAY) > 0) {
            last_rx_time_us = (uint32_t)esp_timer_get_time();
            switch (rx_state) {
                case RX_IDLE: 
                    if (rx_byte == 0x55) { rx_state = RX_PREAMBLE_55; } 
                    break;
                case RX_PREAMBLE_55: 
                    if (rx_byte == 0xFF) { rx_state = RX_HEADER; header_idx = 0; } 
                    else rx_state = RX_IDLE; 
                    break;
                case RX_HEADER:
                    header[header_idx++] = rx_byte;
                    if (header_idx == 6) {
                        if (validate_rx_header_crc(header)) {
                            frame.type = header[0]; frame.dest = header[1]; frame.src = header[2];
                            data_len_local = (header[3] << 8) | header[4];
                            frame.len = data_len_local;
                            
                            if (frame.type != 0x00 && frame.type != 0x01) {
                                // v6.9.3: Garde de niveau pour éviter de charger le code z_log en Flash si DEBUG désactivé
                                if (sysCfg.log_level >= pdLOG_DEBUG) {
                                    z_log(pdLOG_DEBUG, "MSTP", "RX Frame: T=0x%02X, S=%d, D=%d, L=%d\n", frame.type, frame.src, frame.dest, frame.len);
                                }
                            }

                            // Auto-Discovery MAC (Axe 4: via Queue pour éviter inversion de priorité)
                            if (frame.src < 128 && frame.src != sysCfg.ucMacAddress) {
                                xQueueSend(mac_discovery_queue, &frame.src, 0);
                            }

                            if (data_len_local > 0 && data_len_local <= 512) { rx_state = RX_DATA; data_idx = 0; } 
                            else { 
                                frame.timestamp_us = last_rx_time_us;
                                xQueueSend(mstp_rx_queue, &frame, 0); 
                                rx_state = RX_IDLE; 
                            }
                        } else { 
                            // v6.9.3: Garde de niveau pour éviter de charger le code z_log en Flash si DEBUG désactivé
                            if (sysCfg.log_level >= pdLOG_DEBUG) {
                                z_log(pdLOG_DEBUG, "MSTP", "Header CRC Error\n");
                            }
                            bacnetStats.ulErrorsCrc++; rx_state = RX_IDLE; 
                        }
                    }
                    break;
                case RX_DATA: 
                    data_buf_local[data_idx++] = rx_byte; 
                    if (data_idx == data_len_local) rx_state = RX_CRC16_L; 
                    break;
                case RX_CRC16_L: 
                    data_buf_local[data_len_local] = rx_byte; 
                    rx_state = RX_CRC16_H; 
                    break;
                case RX_CRC16_H: 
                    data_buf_local[data_len_local+1] = rx_byte;
                    if (validate_rx_data_crc(data_buf_local, data_len_local + 2)) { 
                        memcpy(frame.data, data_buf_local, data_len_local);
                        frame.timestamp_us = last_rx_time_us;
                        xQueueSend(mstp_rx_queue, &frame, 0);
                    } else bacnetStats.ulErrorsCrc++;
                    rx_state = RX_IDLE;
                    break;
            }
        }
    }
}

static uint8_t calc_header_crc(uint8_t *data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        uint16_t crc16 = crc ^ (crc << 1) ^ (crc << 2) ^ (crc << 3) ^ (crc << 4) ^ (crc << 5) ^ (crc << 6) ^ (crc << 7);
        crc = (crc16 & 0xfe) ^ ((crc16 >> 8) & 1);
    }
    return (~crc) & 0xFF;
}

static uint16_t calc_data_crc(uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        uint8_t crc_low = (crc & 0xff) ^ data[i];
        crc = (crc >> 8) ^ (crc_low << 8) ^ (crc_low << 3) ^ (crc_low << 12) ^ (crc_low >> 4) ^ (crc_low & 0x0f) ^ ((crc_low & 0x0f) << 7);
    }
    return (~crc) & 0xFFFF;
}

static bool validate_rx_header_crc(const uint8_t *header) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < 6; i++) { 
        crc ^= header[i];
        uint16_t crc16 = crc ^ (crc << 1) ^ (crc << 2) ^ (crc << 3) ^ (crc << 4) ^ (crc << 5) ^ (crc << 6) ^ (crc << 7);
        crc = (crc16 & 0xfe) ^ ((crc16 >> 8) & 1);
    }
    return (crc == 0x55);
}

static bool validate_rx_data_crc(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        uint8_t crc_low = (crc & 0xff) ^ data[i];
        crc = (crc >> 8) ^ (crc_low << 8) ^ (crc_low << 3) ^ (crc_low << 12) ^ (crc_low >> 4) ^ (crc_low & 0x0f) ^ ((crc_low & 0x0f) << 7);
    }
    return (crc == 0xF0B8);
}

struct BACnetTag { 
    uint32_t number; 
    bool isContext; 
    uint32_t len; 
    bool isOpening; 
    bool isClosing; 
};

static bool decode_next_tag(const uint8_t *data, uint16_t *pos, uint16_t max, BACnetTag *tag) {
    if (*pos >= max) return false;
    uint8_t b = data[(*pos)++];
    tag->number = b >> 4; tag->isContext = (b & 0x08) != 0;
    uint8_t lvt = b & 0x07;
    if (tag->number == 0x0F) tag->number = data[(*pos)++];
    tag->isOpening = (lvt == 6); tag->isClosing = (lvt == 7);
    if (lvt <= 4) tag->len = lvt;
    else if (lvt == 5) {
        tag->len = data[(*pos)++];
        if (tag->len == 254) { tag->len = (data[*pos] << 8) | data[*pos+1]; *pos += 2; }
        else if (tag->len == 255) { tag->len = ((uint32_t)data[*pos]<<24)|((uint32_t)data[*pos+1]<<16)|((uint32_t)data[*pos+2]<<8)|data[*pos+3]; *pos += 4; }
    } else tag->len = 0;
    return true;
}

/**
 * @brief Décode un flottant BACnet (REAL) sur 4 octets.
 */
static float decode_bacnet_real(const uint8_t *buf) {
    uint32_t tm;
    memcpy(&tm, buf, 4);
    tm = __builtin_bswap32(tm);
    float v;
    memcpy(&v, &tm, 4);
    return v;
}

/**
 * @brief Décode un entier non signé BACnet de longueur variable.
 */
static uint32_t decode_bacnet_unsigned(const uint8_t *buf, uint32_t len) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < len; i++) {
        v = (v << 8) | buf[i];
    }
    return v;
}

/**
 * @brief Décode un Object Identifier BACnet (Type + Instance).
 */
static void decode_bacnet_object_id(const uint8_t *buf, uint16_t *type, uint32_t *instance) {
    uint32_t oid = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) | ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
    if (type) *type = (uint16_t)(oid >> 22);
    if (instance) *instance = oid & 0x3FFFFF;
}

/**
 * @brief Décode une chaîne de caractères BACnet selon son encodage.
 */
static int decode_bacnet_string(const uint8_t *buf, uint32_t len, char *out, size_t max_out) {
    if (len == 0 || out == NULL) return 0;
    uint8_t encoding = buf[0];
    int sl = 0;
    if (encoding == 0) { // UTF-8 / ASCII
        sl = std::min((int)len - 1, (int)max_out - 1);
        memcpy(out, &buf[1], sl);
    } else { // UCS-2 / UTF-16 (Simplifié)
        for (int i = 2; i < (int)len && sl < (int)max_out - 1; i += 2) {
            out[sl++] = buf[i];
        }
    }
    out[sl] = 0;
    return sl;
}

/**
 * @brief Gère la réponse I-Am d'un appareil BACnet.
 */
static void handle_i_am_response(uint8_t src_mac, const uint8_t *apdu, uint16_t al) {
    if (al >= 6 && apdu[2] == 0xC4) {
        uint32_t ulDeviceId;
        decode_bacnet_object_id(&apdu[3], NULL, &ulDeviceId);
        if (xSemaphoreTake(cache_mutex, 0)) {
            bool exists = false;
            for (auto& d : bacnet_network_cache) {
                if (d.ulDeviceId == ulDeviceId) {
                    exists = true;
                    d.last_seen = millis();
                    break;
                }
            }
            if (!exists) {
                BACnetDevice nd;
                nd.ulDeviceId = ulDeviceId;
                nd.ucMacAddress = src_mac;
                nd.xEnabled = false;
                nd.xDiscoveryDone = false;
                nd.last_seen = millis();
                nd.ucDiscStep = DISC_DEV_ID;
                bacnet_network_cache.push_back(nd);
                z_log(pdLOG_INFO, "BACNET", "New Device: ID %lu, MAC %d\n", ulDeviceId, src_mac);
            }
            xSemaphoreGive(cache_mutex);
        }
    }
}

/**
 * @brief Gère un acquittement simple (Simple-ACK).
 */
static void handle_simple_ack(uint8_t src_mac) {
    z_log(pdLOG_DEBUG, "BACNET", "Simple-ACK from MAC %d (Success)\n", src_mac);
    if (xSemaphoreTake(cache_mutex, 0)) {
        for (auto& d : bacnet_network_cache) {
            if (d.ucMacAddress == src_mac) {
                for (auto& o : d.objects) {
                    if (o.usType == pending_write_job.obj_type && o.ulInstance == pending_write_job.obj_instance) {
                        if (pending_write_job.prop_id == 85) {
                            o.fPresentValue = pending_write_job.write_value;
                            publish_mqtt_topic(d.ulDeviceId, o, 85, false);
                            check_ha_dependencies(d.ulDeviceId, o.usType, o.ulInstance);
                        } else if (pending_write_job.prop_id == 77) {
                            publish_mqtt_topic(d.ulDeviceId, o, 77, true);
                        } else if (pending_write_job.prop_id == 81) {
                            bool xIsOos = (pending_write_job.write_value > 0.5f);
                            if (xIsOos) {
                                o.ucStatusFlags |= BACNET_STATUS_OUT_OF_SERVICE;
                            } else {
                                o.ucStatusFlags &= ~BACNET_STATUS_OUT_OF_SERVICE;
                            }
                            publish_mqtt_topic(d.ulDeviceId, o, 81, false);
                        }
                        o.ulLastUpdate = millis();
                        break;
                    }
                }
                break;
            }
        }
        xSemaphoreGive(cache_mutex);
    }
    current_dev_idx = (current_dev_idx + 1) % bacnet_network_cache.size();
}

/**
 * @brief Gère les erreurs réseau (Error, Abort, Reject).
 */
static void handle_error_pdu(BACnetDevice &dev, uint8_t type) {
    if (sysCfg.log_level >= pdLOG_DEBUG) {
        z_log(pdLOG_DEBUG, "BACNET", "Received PDU type 0x%02X from MAC %d (Step %d)\n", type, dev.ucMacAddress, (int)dev.ucDiscStep);
    }
    
    // Si on est en phase de découverte (soit au niveau device, soit au niveau objet valide)
    if (!dev.xDiscoveryDone && (dev.ucDiscStep < DISC_OBJ_OID || dev.usDiscObjIdx < dev.objects.size())) {
        auto& o = dev.objects[dev.usDiscObjIdx];
        retry_count = 0;
        
        switch (dev.ucDiscStep) {
            case DISC_OBJ_NAME:    dev.ucDiscStep = DISC_OBJ_UNITS; break;
            case DISC_OBJ_UNITS:   dev.ucDiscStep = DISC_OBJ_MIN; break;
            case DISC_OBJ_MIN:     dev.ucDiscStep = DISC_OBJ_MAX; break;
            case DISC_OBJ_MAX:
                if(o.usType==13||o.usType==14||o.usType==19) dev.ucDiscStep = DISC_OBJ_STATES;
                else dev.ucDiscStep = DISC_OBJ_COMMANDABLE;
                break;
            case DISC_OBJ_STATES:  dev.ucDiscStep = DISC_OBJ_COMMANDABLE; break;
            case DISC_OBJ_COMMANDABLE:
                o.xIsCommandable = (o.usType==13||o.usType==14||o.usType==19||o.usType<=5);
                if(o.xEnabled || dev.xReloadSingle) dev.ucDiscStep = DISC_OBJ_STATUS_FLAGS;
                else { if(!dev.xReloadSingle) dev.usDiscObjIdx++; dev.ucDiscStep = DISC_OBJ_OID; }
                break;
            case DISC_OBJ_STATUS_FLAGS:
                dev.ucDiscStep = DISC_OBJ_VALUE;
                break;
            case DISC_OBJ_VALUE:
                if(!dev.xReloadSingle) dev.usDiscObjIdx++;
                dev.ucDiscStep = DISC_OBJ_OID;
                break;
            default:
                dev.ucDiscStep = (DISC_STEP_T)((int)dev.ucDiscStep + 1);
                break;
        }

        if (dev.xReloadSingle && dev.ucDiscStep == DISC_OBJ_OID) {
            dev.xDiscoveryDone = true;
            dev.xReloadSingle = false;
        }
    } else {
        waiting_for_reply = false;
        if (xReadJobPending) {
            xReadJobPending = false;
        }
        if (dev.xSupportsRpm) {
            z_log(pdLOG_WARN, "BACNET", "MAC %d RPM rejected/timeout (PDU 0x%02X). Fallback to single read.\n", dev.ucMacAddress, type);
            dev.xSupportsRpm = false;
        } else {
            if (dev.xDiscoveryDone && current_poll_idx < dev.objects.size()) {
                auto& o = dev.objects[current_poll_idx];
                o.ulLastUpdate = millis();
                if (isnan(o.fPresentValue)) {
                    o.fPresentValue = 0.0f; // Default fallback to break HA Catch-22
                    if (o.xEnabled) publish_mqtt_topic(dev.ulDeviceId, o, 85, false);
                }
            }
        }
    }
}

/**
 * @brief Gère la partie Découverte d'un Complex-ACK.
 */
static void handle_complex_ack_discovery(BACnetDevice &dev, const uint8_t *apdu, uint16_t al, uint16_t &ap, BACnetTag &vt) {
    const DiscoveryStepConfig &cfg = DISCOVERY_STEPS[dev.ucDiscStep];
    
    switch (cfg.storage) {
        case STORE_DEV_ID:
            decode_bacnet_object_id(&apdu[ap], NULL, &dev.ulDeviceId);
            z_log(pdLOG_INFO, "BACNET", "Device ID: %lu\n", (unsigned long)dev.ulDeviceId);
            dev.ucDiscStep = DISC_DEV_NAME;
            break;

        case STORE_DEV_NAME: {
            char n[64]; decode_bacnet_string(&apdu[ap], vt.len, n, sizeof(n));
            dev.name = String(n);
            z_log(pdLOG_INFO, "BACNET", "Device Name: %s\n", n);
            dev.ucDiscStep = DISC_DEV_VENDOR;
            break;
        }

        case STORE_DEV_VENDOR: {
            char n[64]; decode_bacnet_string(&apdu[ap], vt.len, n, sizeof(n));
            dev.vendor = String(n);
            z_log(pdLOG_INFO, "BACNET", "Device Vendor: %s\n", n);
            dev.ucDiscStep = DISC_DEV_MAX_APDU;
            break;
        }

        case STORE_DEV_UINT: {
            uint32_t val = decode_bacnet_unsigned(&apdu[ap], vt.len);
            if (dev.ucDiscStep == DISC_OBJ_COUNT) {
                if (val > 2000) {
                    z_log(pdLOG_ERROR, "BACNET", "INVALID OBJECT COUNT: %lu (Hex: 0x%08X)\n", (unsigned long)val, (unsigned int)val);
                    val = 0;
                }
                z_log(pdLOG_INFO, "BACNET", "Device Object Count: %lu\n", (unsigned long)val);
                FSM_old_objects = dev.objects;
                dev.objects.clear();
                if (val > 0) {
                    try { dev.objects.resize(val); } 
                    catch (...) { z_log(pdLOG_ERROR, "BACNET", "Alloc Fail\n"); val = 0; }
                }
                dev.usDiscObjIdx = 0;
                dev.ucDiscStep = DISC_OBJ_OID;
                if (val == 0) {
                    dev.xDiscoveryDone = true;
                    dev.xDirty = true; dev.ulLastDirtyTime = millis();
                }
            } else {
                if (dev.ucDiscStep == DISC_DEV_MAX_APDU) { dev.usMaxApduLengthAccepted = (uint16_t)val; dev.ucDiscStep = DISC_DEV_TIMEOUT; }
                else if (dev.ucDiscStep == DISC_DEV_TIMEOUT) { dev.ulApduTimeout = val; dev.ucDiscStep = DISC_DEV_RETRIES; }
                else if (dev.ucDiscStep == DISC_DEV_RETRIES) { dev.ucNumberOfApduRetries = (uint8_t)val; dev.ucDiscStep = DISC_OBJ_COUNT; }
            }
            break;
        }

        case STORE_OBJ_OID: {
            if (dev.usDiscObjIdx < dev.objects.size()) {
                auto& o = dev.objects[dev.usDiscObjIdx];
                decode_bacnet_object_id(&apdu[ap], &o.usType, &o.ulInstance);
                
                // Restauration de la configuration précédente si elle existe
                for (const auto& old_o : FSM_old_objects) {
                    if (old_o.usType == o.usType && old_o.ulInstance == o.ulInstance) {
                        o.xEnabled = old_o.xEnabled;
                        o.fMinValue = old_o.fMinValue;
                        o.fMaxValue = old_o.fMaxValue;
                        o.fStepValue = old_o.fStepValue;
                        strlcpy(o.cMinRef, old_o.cMinRef, sizeof(o.cMinRef));
                        strlcpy(o.cMaxRef, old_o.cMaxRef, sizeof(o.cMaxRef));
                        strlcpy(o.cLastHaComponent, old_o.cLastHaComponent, sizeof(o.cLastHaComponent));
                        break;
                    }
                }
                
                z_log(pdLOG_INFO, "BACNET", "Obj %u OID: T%u I%lu\n", dev.usDiscObjIdx+1, o.usType, (unsigned long)o.ulInstance);
                dev.ucDiscStep = DISC_OBJ_NAME;
            }
            break;
        }

        case STORE_OBJ_NAME: {
            if (dev.usDiscObjIdx < dev.objects.size()) {
                auto& o = dev.objects[dev.usDiscObjIdx];
                char n[64]; decode_bacnet_string(&apdu[ap], vt.len, n, sizeof(n));
                String ns = String(n); ns.trim();
                if (ns.length() == 0 || ns.equalsIgnoreCase("Unknown")) {
                    snprintf(o.cName, sizeof(o.cName), "OBJ-%lu", (unsigned long)o.ulInstance);
                } else strlcpy(o.cName, n, sizeof(o.cName));
                z_log(pdLOG_INFO, "BACNET", "Obj %u Name: %s\n", dev.usDiscObjIdx+1, o.cName);
                if(o.usType<=2||o.usType==23||o.usType==24||o.usType==46) dev.ucDiscStep = DISC_OBJ_UNITS; 
                else if(o.usType==13||o.usType==14||o.usType==19){ o.state_texts.clear(); o.ucExpectedStatesCount=0; dev.ucDiscStep = DISC_OBJ_STATES; } 
                else dev.ucDiscStep = DISC_OBJ_COMMANDABLE; 
            }
            break;
        }

        case STORE_OBJ_UNITS: {
            if (dev.usDiscObjIdx < dev.objects.size()) {
                auto& o = dev.objects[dev.usDiscObjIdx];
                o.usUnits = (uint16_t)decode_bacnet_unsigned(&apdu[ap], vt.len);
                String us = get_unit_text(o.usUnits); strlcpy(o.cUnitText, us.c_str(), sizeof(o.cUnitText)); 
                dev.ucDiscStep = DISC_OBJ_MIN; 
            }
            break;
        }

        case STORE_OBJ_REAL: {
            if (dev.usDiscObjIdx < dev.objects.size()) {
                auto& o = dev.objects[dev.usDiscObjIdx]; float v = decode_bacnet_real(&apdu[ap]);
                if (dev.ucDiscStep == DISC_OBJ_MIN) { o.fMinValue = v; dev.ucDiscStep = DISC_OBJ_MAX; }
                else { 
                    o.fMaxValue = v; 
                    if(o.usType==13||o.usType==14||o.usType==19) dev.ucDiscStep = DISC_OBJ_STATES; 
                    else dev.ucDiscStep = DISC_OBJ_COMMANDABLE;
                }
            }
            break;
        }

        case STORE_OBJ_STATES: {
            if (dev.usDiscObjIdx < dev.objects.size()) {
                auto& o = dev.objects[dev.usDiscObjIdx];
                if (vt.number == 2) {
                    o.ucExpectedStatesCount = (uint16_t)decode_bacnet_unsigned(&apdu[ap], vt.len);
                    z_log(pdLOG_DEBUG, "BACNET", "Obj %u Expecting %u states\n", dev.usDiscObjIdx+1, o.ucExpectedStatesCount);
                    if (o.ucExpectedStatesCount == 0) dev.ucDiscStep = DISC_OBJ_COMMANDABLE;
                } else if (vt.number == 7 || (!vt.isContext && vt.number == 7)) {
                    char n[64]; decode_bacnet_string(&apdu[ap], vt.len, n, sizeof(n));
                    o.state_texts.push_back(String(n));
                    z_log(pdLOG_DEBUG, "BACNET", "Obj %u State %zu: %s (len %u)\n", dev.usDiscObjIdx+1, o.state_texts.size(), n, vt.len);
                    if (o.state_texts.size() >= o.ucExpectedStatesCount) {
                        dev.ucDiscStep = DISC_OBJ_COMMANDABLE;
                        save_object_states(dev.ulDeviceId, o.usType, o.ulInstance, o.state_texts);
                    } 
                }
            }
            break;
        }

        case STORE_OBJ_VALUE: {
            if (dev.usDiscObjIdx < dev.objects.size()) {
                auto& o = dev.objects[dev.usDiscObjIdx];
                o.fPresentValue = (vt.number == 4) ? decode_bacnet_real(&apdu[ap]) : (float)decode_bacnet_unsigned(&apdu[ap], vt.len);
                o.ulLastUpdate = millis();
                bool stop = dev.xReloadSingle || dev.xRecoveryMode; 
                if(!stop) dev.usDiscObjIdx++; 
                dev.ucDiscStep = DISC_OBJ_OID; 
                if(dev.usDiscObjIdx%50==0||dev.usDiscObjIdx>=dev.objects.size()) { dev.xDirty = true; dev.ulLastDirtyTime = millis(); }
                if(stop || dev.usDiscObjIdx>=dev.objects.size()){
                    dev.xDiscoveryDone = true; dev.xReloadSingle = false; dev.xRecoveryMode = false; 
                    dev.xDirty = true; dev.ulLastDirtyTime = millis(); 
                    if (dev.usDiscObjIdx>=dev.objects.size()) { publish_all_names(); trigger_ha_discovery(dev.ulDeviceId, 0xFFFFFFFF, 0xFFFF); }
                }
            }
            break;
        }

        case STORE_OBJ_STATUS_FLAGS: {
            if (dev.usDiscObjIdx < dev.objects.size()) {
                auto& o = dev.objects[dev.usDiscObjIdx];
                if (vt.number == 8 && vt.len >= 2) {
                    uint8_t ucFlags = 0;
                    uint8_t ucFirstByte = apdu[ap + 1];
                    if (ucFirstByte & 0x80) ucFlags |= BACNET_STATUS_IN_ALARM;       // Bit 0
                    if (ucFirstByte & 0x40) ucFlags |= BACNET_STATUS_FAULT;          // Bit 1
                    if (ucFirstByte & 0x20) ucFlags |= BACNET_STATUS_OVERRIDDEN;     // Bit 2
                    if (ucFirstByte & 0x10) ucFlags |= BACNET_STATUS_OUT_OF_SERVICE; // Bit 3
                    o.ucStatusFlags = ucFlags;
                }
                dev.ucDiscStep = DISC_OBJ_VALUE;
            }
            break;
        }

        case STORE_NONE:
            if (dev.ucDiscStep == DISC_OBJ_COMMANDABLE) {
                if (dev.usDiscObjIdx < dev.objects.size()) {
                    auto& o = dev.objects[dev.usDiscObjIdx]; o.xIsCommandable = true;
                    if(o.xEnabled||dev.xReloadSingle) dev.ucDiscStep = DISC_OBJ_STATUS_FLAGS;
                    else { if(!dev.xReloadSingle) dev.usDiscObjIdx++; dev.ucDiscStep = DISC_OBJ_OID; if(dev.xReloadSingle){dev.xDiscoveryDone=true; dev.xReloadSingle=false;} }
                }
                ap = al; 
            }
            break;
    }
}

void check_ha_dependencies(uint32_t did, uint16_t type, uint32_t inst) {
    String t_str = "";
    if (type == 0) t_str = "AI";
    else if (type == 1) t_str = "AO";
    else if (type == 2) t_str = "AV";
    if (t_str.length() == 0) return;
    
    String key = String(did) + "_" + t_str + ":" + String(inst);
    auto it = ha_dependencies.find(key);
    if (it != ha_dependencies.end()) {
        for (auto& dep : it->second) trigger_ha_discovery(did, dep.second, dep.first);
    }
}

/**
 * @brief Gère la partie Polling d'un Complex-ACK.
 */
static void handle_complex_ack_polling(BACnetDevice &dev, const uint8_t *apdu, uint16_t al, uint16_t &ap, BACnetTag &vt) {
    if (apdu[2] == 0x0E) { // ReadPropertyMultiple
        uint32_t current_oid = 0;
        while (ap < al) {
            BACnetTag t_rpm;
            if (!decode_next_tag(apdu, &ap, al, &t_rpm)) break;
            if (t_rpm.isOpening || t_rpm.isClosing) continue;
            
            if (t_rpm.number == 0 && t_rpm.len >= 4) { // ObjectIdentifier
                decode_bacnet_object_id(&apdu[ap], NULL, &current_oid);
            } 
            else if (t_rpm.number == 4 || t_rpm.number == 2 || t_rpm.number == 9) { // Value inside Context 4
                float v = 0.0f;
                if (t_rpm.number == 4) v = decode_bacnet_real(&apdu[ap]);
                else v = (float)decode_bacnet_unsigned(&apdu[ap], t_rpm.len);
                
                if (current_oid > 0) {
                    uint16_t t_type = current_oid >> 22;
                    uint32_t t_inst = current_oid & 0x3FFFFF;
                    for (auto& o : dev.objects) {
                        if (o.usType == t_type && o.ulInstance == t_inst) {
                            if (!(o.usType == 0 && o.isOutOfService())) {
                                if (o.fPresentValue != v) {
                                    o.fPresentValue = v;
                                    o.ulLastUpdate = millis();
                                    if (o.xEnabled) publish_mqtt_topic(dev.ulDeviceId, o, 85, false);
                                    check_ha_dependencies(dev.ulDeviceId, o.usType, o.ulInstance);
                                } else {
                                    o.ulLastUpdate = millis();
                                }
                            } else {
                                o.ulLastUpdate = millis();
                            }
                            break;
                        }
                    }
                    current_oid = 0; // Reset for next
                }
            }
            else if (t_rpm.number == 5) { // Error
                if (current_oid > 0) {
                    uint16_t t_type = current_oid >> 22;
                    uint32_t t_inst = current_oid & 0x3FFFFF;
                    z_log(pdLOG_WARN, "BACNET", "RPM returned Error (Context 5) for Object Type %d Inst %d. Skipped.", t_type, t_inst);
                    for (auto& o : dev.objects) {
                        if (o.usType == t_type && o.ulInstance == t_inst) {
                            o.ulLastUpdate = millis(); // Mark as read to avoid infinite loop
                            if (isnan(o.fPresentValue)) {
                                o.fPresentValue = 0.0f; // Default fallback to break HA Catch-22
                                if (o.xEnabled) publish_mqtt_topic(dev.ulDeviceId, o, 85, false);
                            }
                            break;
                        }
                    }
                    current_oid = 0;
                }
            }
            ap += t_rpm.len;
        }
    } else {
        // Parse single ReadProperty ACK
        if (xReadJobPending) {
            xReadJobPending = false;
            float v = 0.0f;
            if (vt.number == 4) v = decode_bacnet_real(&apdu[ap]);
            else v = (float)decode_bacnet_unsigned(&apdu[ap], vt.len);
            
            for (auto& o : dev.objects) {
                if (o.usType == xPendingReadJob.obj_type && o.ulInstance == xPendingReadJob.obj_instance) {
                    if (xPendingReadJob.prop_id == 85) {
                        if (!(o.usType == 0 && o.isOutOfService())) {
                            if (o.fPresentValue != v) {
                                o.fPresentValue = v; o.ulLastUpdate = millis();
                                if (o.xEnabled) publish_mqtt_topic(dev.ulDeviceId, o, 85, false);
                                check_ha_dependencies(dev.ulDeviceId, o.usType, o.ulInstance);
                            } else o.ulLastUpdate = millis();
                        } else {
                            o.ulLastUpdate = millis();
                        }
                    } else if (xPendingReadJob.prop_id == 111) {
                        o.ucStatusFlags = (uint8_t)v;
                        o.ulLastUpdate = millis();
                        if (o.xEnabled) publish_mqtt_topic(dev.ulDeviceId, o, 81, false);
                    }
                    break;
                }
            }
        } else {
            if (vt.number == 4) {
                float v = decode_bacnet_real(&apdu[ap]);
                if (current_poll_idx < dev.objects.size()) {
                    auto& o = dev.objects[current_poll_idx]; 
                    if (!(o.usType == 0 && o.isOutOfService())) {
                        if (o.fPresentValue != v) {
                            o.fPresentValue = v; o.ulLastUpdate = millis();
                            if (o.xEnabled) publish_mqtt_topic(dev.ulDeviceId, o, 85, false);
                            check_ha_dependencies(dev.ulDeviceId, o.usType, o.ulInstance);
                        } else o.ulLastUpdate = millis();
                    } else {
                        o.ulLastUpdate = millis();
                    }
                }
            } else if (vt.number == 2 || vt.number == 9) {
                uint32_t v = decode_bacnet_unsigned(&apdu[ap], vt.len);
                if (current_poll_idx < dev.objects.size()) {
                    auto& o = dev.objects[current_poll_idx]; 
                    if (!(o.usType == 0 && o.isOutOfService())) {
                        if (o.fPresentValue != (float)v) {
                            o.fPresentValue = (float)v; o.ulLastUpdate = millis();
                            if (o.xEnabled) publish_mqtt_topic(dev.ulDeviceId, o, 85, false);
                            check_ha_dependencies(dev.ulDeviceId, o.usType, o.ulInstance);
                        } else o.ulLastUpdate = millis();
                    } else {
                        o.ulLastUpdate = millis();
                    }
                }
            }
        }
        ap += vt.len;
    }
}

static void uart_tx(const uint8_t *buf, uint16_t len) {
    uart_write_bytes(RS485_UART_PORT, (const char*)buf, len);
    uart_wait_tx_done(RS485_UART_PORT, pdMS_TO_TICKS(50));
    last_rx_time_us = (uint32_t)esp_timer_get_time();
}

static void send_mstp_frame(uint8_t target, uint8_t type, const uint8_t* apdu, uint16_t len) {
    if (apdu && len > 4) last_sent_invoke_id = apdu[4]; else last_sent_invoke_id = 0xFF;
    if (apdu && len > 0) { memcpy(last_apdu, apdu, len); last_apdu_len = len; }
    uint8_t b[512+10]; b[0]=0x55; b[1]=0xFF; b[2]=type; b[3]=target; b[4]=sysCfg.ucMacAddress;
    b[5]=(len>>8)&0xFF; b[6]=len&0xFF; b[7]=calc_header_crc(&b[2], 5);
    while ((micros() - last_rx_time_us) < T_TURNAROUND_US) { }
    if (len > 0) {
        memcpy(&b[8], apdu, len); uint16_t c = calc_data_crc(&b[8], len);
        b[8+len]=c&0xFF; b[8+len+1]=(c>>8)&0xFF; uart_tx(b, 8+len+2);
    } else uart_tx(b, 8);
    if (type == 0x05 || type == 0x06) bacnetStats.ulMsMsgsTx++;
}

uint16_t build_read_property_multiple_apdu(uint8_t* buffer, uint8_t invoke_id, std::vector<BACnetObject*>& objects, uint8_t property_id) {
    uint16_t len = 0;
    buffer[len++] = 0x01; // Confirmed-REQ
    buffer[len++] = 0x04; // Max Seg / Max Resp
    buffer[len++] = invoke_id;
    buffer[len++] = 0x0E; // Service Choice 14 (ReadPropertyMultiple)
    
    for (auto* obj : objects) {
        // [0] ObjectIdentifier
        buffer[len++] = 0x0C;
        uint32_t oid = ((uint32_t)obj->usType << 22) | (obj->ulInstance & 0x3FFFFF);
        buffer[len++] = (oid >> 24) & 0xFF;
        buffer[len++] = (oid >> 16) & 0xFF;
        buffer[len++] = (oid >> 8) & 0xFF;
        buffer[len++] = oid & 0xFF;
        
        // [1] List of Property References
        buffer[len++] = 0x1E; // Open Tag 1
        buffer[len++] = 0x09; // Context 0, Length 1 (PropertyIdentifier)
        buffer[len++] = property_id;
        buffer[len++] = 0x1F; // Close Tag 1
    }
    return len;
}

uint16_t build_read_property_apdu(uint8_t* buffer, uint8_t invoke_id, uint16_t obj_type, uint32_t obj_instance, uint8_t property_id, int32_t array_index) {
    uint16_t len = 0;
    buffer[len++] = 0x01; buffer[len++] = 0x04; buffer[len++] = 0x02; buffer[len++] = 0x73;
    buffer[len++] = invoke_id; buffer[len++] = 0x0C; buffer[len++] = 0x0C;
    uint32_t oid = ((uint32_t)obj_type << 22) | (obj_instance & 0x3FFFFF);
    buffer[len++] = (oid >> 24) & 0xFF; buffer[len++] = (oid >> 16) & 0xFF; buffer[len++] = (oid >> 8) & 0xFF; buffer[len++] = oid & 0xFF;
    buffer[len++] = 0x19; buffer[len++] = property_id;
    if (array_index >= 0) { 
        if (array_index <= 255) { buffer[len++] = 0x29; buffer[len++] = (uint8_t)array_index; }
        else { buffer[len++] = 0x2A; buffer[len++] = (array_index >> 8) & 0xFF; buffer[len++] = array_index & 0xFF; }
    }
    return len;
}

uint16_t build_write_property_name_apdu(uint8_t* buffer, uint8_t invoke_id, uint16_t obj_type, uint32_t obj_instance, const char* new_name) {
    if (new_name == NULL) return 0;
    uint16_t len = 0;
    buffer[len++] = 0x01; buffer[len++] = 0x04; buffer[len++] = 0x02; buffer[len++] = 0x03;
    buffer[len++] = invoke_id; buffer[len++] = 0x0F; buffer[len++] = 0x0C;
    uint32_t oid = ((uint32_t)obj_type << 22) | (obj_instance & 0x3FFFFF);
    buffer[len++] = (oid >> 24) & 0xFF; buffer[len++] = (oid >> 16) & 0xFF; buffer[len++] = (oid >> 8) & 0xFF; buffer[len++] = oid & 0xFF;
    buffer[len++] = 0x19; buffer[len++] = 0x4D; buffer[len++] = 0x3E; 
    uint32_t str_len = strlen(new_name); uint32_t payload_len = str_len + 1;
    if (payload_len <= 4) buffer[len++] = 0x70 | (uint8_t)payload_len;
    else { buffer[len++] = 0x75; buffer[len++] = (uint8_t)payload_len; }
    buffer[len++] = 0x00; memcpy(&buffer[len], new_name, str_len); len += str_len;
    buffer[len++] = 0x3F;
    return len;
}

uint16_t build_write_property_value_apdu(uint8_t* buffer, uint8_t invoke_id, uint16_t obj_type, uint32_t obj_instance, uint8_t prop_id, float value, uint8_t ucPriority) {
    uint16_t len = 0;
    buffer[len++] = 0x01; buffer[len++] = 0x04; buffer[len++] = 0x02; buffer[len++] = 0x03;
    buffer[len++] = invoke_id; buffer[len++] = 0x0F; buffer[len++] = 0x0C;
    uint32_t oid = ((uint32_t)obj_type << 22) | (obj_instance & 0x3FFFFF);
    buffer[len++] = (oid >> 24) & 0xFF; buffer[len++] = (oid >> 16) & 0xFF; buffer[len++] = (oid >> 8) & 0xFF; buffer[len++] = oid & 0xFF;
    buffer[len++] = 0x19; buffer[len++] = prop_id; buffer[len++] = 0x3E;
    if (obj_type == 13 || obj_type == 14 || obj_type == 19) {
        uint32_t v = (uint32_t)value;
        if (v <= 255) { buffer[len++] = 0x21; buffer[len++] = (uint8_t)v; }
        else if (v <= 65535) { buffer[len++] = 0x22; buffer[len++] = (v >> 8) & 0xFF; buffer[len++] = v & 0xFF; }
        else { buffer[len++] = 0x24; buffer[len++] = (v >> 24) & 0xFF; buffer[len++] = (v >> 16) & 0xFF; buffer[len++] = (v >> 8) & 0xFF; buffer[len++] = v & 0xFF; }
    } else if (obj_type == 3 || obj_type == 4 || obj_type == 5) {
        buffer[len++] = 0x91; buffer[len++] = (value > 0.5f) ? 1 : 0;
    } else {
        buffer[len++] = 0x44; uint32_t tmp; float fv = value; memcpy(&tmp, &fv, 4); tmp = __builtin_bswap32(tmp);
        memcpy(&buffer[len], &tmp, 4); len += 4;
    }
    buffer[len++] = 0x3F;
    if (ucPriority > 0 && ucPriority <= 16) {
        buffer[len++] = 0x49;
        buffer[len++] = ucPriority;
    }
    return len;
}

// AJOUT : Constructeur d'APDU pour écrire un booléen sur Out_Of_Service (96)
uint16_t build_write_property_outofservice_apdu(uint8_t* buffer, uint8_t invoke_id, uint16_t obj_type, uint32_t obj_instance, bool out_of_service) {
    uint16_t len = 0;
    buffer[len++] = 0x01; buffer[len++] = 0x04; buffer[len++] = 0x02; buffer[len++] = 0x03;
    buffer[len++] = invoke_id; buffer[len++] = 0x0F; buffer[len++] = 0x0C;
    
    uint32_t oid = ((uint32_t)obj_type << 22) | (obj_instance & 0x3FFFFF);
    buffer[len++] = (oid >> 24) & 0xFF; buffer[len++] = (oid >> 16) & 0xFF; buffer[len++] = (oid >> 8) & 0xFF; buffer[len++] = oid & 0xFF;
    
    buffer[len++] = 0x19; buffer[len++] = 96; // 96 = PROP_OUT_OF_SERVICE
    buffer[len++] = 0x3E; // Open Tag 3
    
    // Tag BACnet Boolean (Tag Number 1). 0x11 = True, 0x10 = False.
    buffer[len++] = out_of_service ? 0x11 : 0x10; 
    
    buffer[len++] = 0x3F; // Close Tag 3
    return len;
}


uint16_t build_i_am_apdu(uint8_t* buffer, uint32_t device_instance, uint16_t max_apdu, uint16_t vendor_id) {
    uint16_t len = 0;
    buffer[len++] = 0x10; // PDU Type: Unconfirmed-Request
    buffer[len++] = 0x00; // Service Choice: I-Am
    buffer[len++] = 0xC4; // Tag 12, Len 4 (ObjectIdentifier)
    uint32_t oid = (8 << 22) | (device_instance & 0x3FFFFF);
    buffer[len++] = (oid >> 24) & 0xFF; buffer[len++] = (oid >> 16) & 0xFF;
    buffer[len++] = (oid >> 8) & 0xFF;  buffer[len++] = oid & 0xFF;
    if (max_apdu <= 255) { buffer[len++] = 0x21; buffer[len++] = (uint8_t)max_apdu; }
    else { buffer[len++] = 0x22; buffer[len++] = (max_apdu >> 8) & 0xFF; buffer[len++] = max_apdu & 0xFF; }
    buffer[len++] = 0x91; buffer[len++] = 0x03; // Tag 9 (Enumerated), Value 3 (segmented-none)
    if (vendor_id <= 255) { buffer[len++] = 0x21; buffer[len++] = (uint8_t)vendor_id; }
    else { buffer[len++] = 0x22; buffer[len++] = (vendor_id >> 8) & 0xFF; buffer[len++] = vendor_id & 0xFF; }
    return len;
}

String get_unit_text(uint16_t usUnits) {
    switch(usUnits) {
        case 62: return "°C"; case 63: return "°K"; case 64: return "°F";
        case 19: return "kWh"; case 18: return "Wh"; case 146: return "MWh"; case 17: return "kJ"; case 16: return "J"; case 126: return "MJ"; case 20: return "BTU";
        case 48: return "kW"; case 47: return "W"; case 49: return "MW";
        case 5: return "V"; case 124: return "mV"; case 3: return "A"; case 2: return "mA"; case 4: return "Ohm"; case 8: return "VA"; case 9: return "kVA";
        case 53: return "Pa"; case 54: return "kPa"; case 55: return "bar"; case 56: return "psi";
        case 87: return "L/s"; case 88: return "L/min"; case 136: return "L/h"; case 85: return "m³/s"; case 135: return "m³/h"; case 84: return "cfm";
        case 29: return "%RH"; case 98: return "%"; case 96: return "ppm";
        case 73: return "s"; case 72: return "min"; case 71: return "h"; case 159: return "ms";
        case 82: return "L"; case 80: return "m³";
        case 95: return "no-units"; case 255: return "none";
        default: return "Unit " + String(usUnits);
    }
}


static MSTP_MASTER_STATE last_mstp_log_state = MSTP_INITIALIZE;
static void log_mstp_state_change(MSTP_MASTER_STATE new_state) {
    if (new_state != last_mstp_log_state) {
        // z_log(pdLOG_DEBUG, "MSTP", "State: %d -> %d\n", (int)last_mstp_log_state, (int)new_state);
        last_mstp_log_state = new_state;
    }
}

void handle_mstp_idle() {
    uint32_t tnt = T_NO_TOKEN_US + (sysCfg.ucMacAddress * 10000);
    if (ReceivedValidFrame) {
        // v6.4.4: Tout trafic valide d'un tiers indique que le bus est vivant
        if (src_mac != sysCfg.ucMacAddress) bacnetStats.xRingActive = true;

        if (frame_type == 0x00 && dest_mac == sysCfg.ucMacAddress) { 
            bacnetStats.ulTokensSeen++; frame_count = 0; mstp_state = MSTP_USE_TOKEN; 
        }
        else if (frame_type == 0x01 && dest_mac == sysCfg.ucMacAddress) {
            send_mstp_frame(src_mac, 0x02, NULL, 0); next_station = src_mac; ring_stable = true;
        } else if (dest_mac == sysCfg.ucMacAddress && frame_type >= 0x05) {
            z_log(pdLOG_DEBUG, "MSTP", "Data for us from %d (Type 0x%02X)\n", src_mac, frame_type);
            mstp_state = MSTP_ANSWER_DATA_REQUEST;
        } else if (dest_mac == 0xFF || dest_mac == sysCfg.ucMacAddress) {
            MSTP_Frame f = { frame_type, dest_mac, src_mac, data_len, {0}, (uint32_t)esp_timer_get_time() };
            memcpy(f.data, data_buf, data_len); process_incoming_frame(f);
        }
    } else if (timer_silence_us >= tnt) {
        // v6.4.4: Silence Timeout = Perte de communication réelle
        if (bacnetStats.xRingActive) {
            z_log(pdLOG_INFO, "MSTP", "Silence Timeout (%lu us). Lost Token? Claiming.\n", timer_silence_us);
            bacnetStats.xRingActive = false;
        }
        // uart_flush_input(RS485_UART_PORT); // RETIRÉ: Provoque un deadlock de uart_read_bytes dans mstp_rx_task
        mstp_state = MSTP_NO_TOKEN;
    }
}

void handle_mstp_use_token() {
    if (frame_count == 0 && !has_bacnet_work()) { 
        mstp_state = MSTP_DONE_WITH_TOKEN; return; 
    }
    if (frame_count == 0) token_skip_count++;
    if (frame_count > 0 || (uxQueueMessagesWaiting(bacnet_job_queue) > 0) || token_skip_count >= sysCfg.token_skip) {
        execute_bacnet_work(); token_skip_count = 0;
    } else mstp_state = MSTP_DONE_WITH_TOKEN;
}

void handle_mstp_wait_for_reply() {
    if (ReceivedValidFrame) {
        // v6.4.4: Réponse reçue = communication active
        bacnetStats.xRingActive = true;

        MSTP_Frame f = { frame_type, dest_mac, src_mac, data_len, {0}, (uint32_t)esp_timer_get_time() };
        memcpy(f.data, data_buf, data_len); process_incoming_frame(f);
        waiting_for_reply = false; mstp_state = MSTP_DONE_WITH_TOKEN;
    } else if (timer_silence_us >= T_REPLY_TIMEOUT_US) {
        waiting_for_reply = false;
        if (!ring_stable) {
            poll_station = (poll_station + 1) % 128; if (poll_station == sysCfg.ucMacAddress) poll_station = (poll_station + 1) % 128;
            poll_station = (poll_station + 1) % (sysCfg.max_master + 1); if (poll_station == sysCfg.ucMacAddress) poll_station = (poll_station + 1) % (sysCfg.max_master + 1);
            mstp_state = MSTP_POLL_FOR_MASTER; return;
        }
        if (current_dev_idx < bacnet_network_cache.size()) {
            if (retry_count < sysCfg.max_retries) {
                retry_count++; send_mstp_frame(bacnet_network_cache[current_dev_idx].ucMacAddress, 0x05, last_apdu, last_apdu_len);
                waiting_for_reply = true;
            } else { 
                retry_count = 0; 
                // v6.8.18: Si on était en découverte, on force le passage à la suite pour éviter de rester bloqué
                auto& dev = bacnet_network_cache[current_dev_idx];
                if (!dev.xDiscoveryDone) {
                    z_log(pdLOG_WARN, "BACNET", "MAC %d Timeout on step %d, skipping...\n", dev.ucMacAddress, (int)dev.ucDiscStep);
                    handle_error_pdu(dev, 0); // 0 = Timeout sentinel
                }
                mstp_state = MSTP_DONE_WITH_TOKEN; 
            }
        } else mstp_state = MSTP_DONE_WITH_TOKEN;
    }
}

void handle_mstp_pass_token() {
    send_mstp_frame(next_station, 0x00, NULL, 0); waiting_for_reply = false; mstp_state = MSTP_IDLE;
}

void handle_mstp_poll_for_master() {
    if (timer_silence_us >= 50000) {
        send_mstp_frame(poll_station, 0x01, NULL, 0); waiting_for_reply = true; mstp_state = MSTP_WAIT_FOR_REPLY;
    }
}

bool has_bacnet_work() {
    if (uxQueueMessagesWaiting(bacnet_job_queue) > 0) return true;
    bool w = false; if (xSemaphoreTake(cache_mutex, 0)) {
        for (const auto& d : bacnet_network_cache) {
            if (!d.xDiscoveryDone) { w = true; break; }
            for (const auto& o : d.objects) if (o.xEnabled && (o.ulLastUpdate == 0 || (millis()-o.ulLastUpdate)>=(sysCfg.bacnet_poll_interval*1000))) { w = true; break; }
            if (w) break;
        } xSemaphoreGive(cache_mutex);
    } return w;
}

/**
 * @brief Exécute les tâches BACnet en attente ou la logique de découverte.
 * @details Priorise les jobs (Who-Is, I-Am, Write) sur la découverte automatique.
 */
void execute_bacnet_work() {
    BACnetJob j;

    if (!xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(15))) {
        mstp_state = MSTP_DONE_WITH_TOKEN; // Release token cycle if cache is locked
        return;
    }

    if (xQueueReceive(bacnet_job_queue, &j, 0)) {
        uint8_t b[256]; 
        uint16_t l = 0;

        switch (j.type) {
            case JOB_WHO_IS:
                // NPDU Broadcast (Global) + Who-Is Unconstrained (0x10 0x08)
                b[l++] = 0x01; b[l++] = 0x20; b[l++] = 0xFF; b[l++] = 0xFF; b[l++] = 0x00; b[l++] = 0xFF;
                b[l++] = 0x10; b[l++] = 0x08;
                send_mstp_frame(0xFF, 0x06, b, l);
                z_log(pdLOG_INFO, "BACNET", "WHO-IS send\n");
                break;

            case JOB_I_AM:
                // NPDU Broadcast (Global)
                b[l++] = 0x01; b[l++] = 0x20; b[l++] = 0xFF; b[l++] = 0xFF; b[l++] = 0x00; b[l++] = 0xFF;
                // APDU I-Am (Service 0x00, PDU 0x10)
                l += build_i_am_apdu(&b[l], sysCfg.ulDeviceId, 480, 0); // segmentation 0 = non-supported
                send_mstp_frame(0xFF, 0x06, b, l);
                z_log(pdLOG_INFO, "BACNET", "I-AM send (Device %lu)\n", (unsigned long)sysCfg.ulDeviceId);
                break;

            case JOB_WRITE_PROP:
                if (j.prop_id == 77) { // Object_Name
                    l = build_write_property_name_apdu(b, next_invoke_id++, j.obj_type, j.obj_instance, j.name);
                    z_log(pdLOG_INFO, "BACNET", "WRITE obj: %u:%lu (Name) -> %s\n", j.obj_type, (unsigned long)j.obj_instance, j.name);
                } else if (j.prop_id == 81) { // Out_Of_Service
                    bool xIsOos = (j.write_value > 0.5f);
                    l = build_write_property_outofservice_apdu(b, next_invoke_id++, j.obj_type, j.obj_instance, xIsOos);
                    z_log(pdLOG_DEBUG, "BACNET", "APDU hex : ");
                    z_log(pdLOG_INFO, "BACNET", "WRITE obj: %u:%lu (Out_Of_Service) -> %d\n", j.obj_type, (unsigned long)j.obj_instance, xIsOos);
                } else { // Present_Value (85) ou autre numérique
                    l = build_write_property_value_apdu(b, next_invoke_id++, j.obj_type, j.obj_instance, j.prop_id, j.write_value, j.priority);
                    z_log(pdLOG_INFO, "BACNET", "WRITE obj: %u:%lu (Prop %u) -> %.2f (Prio: %u)\n", j.obj_type, (unsigned long)j.obj_instance, j.prop_id, j.write_value, j.priority);
                }
                
                // Préparation du suivi de réponse pour la FSM MS/TP
                pending_write_job = j;
                waiting_for_reply = true;
                retry_count = 0;
                send_mstp_frame(j.target_mac, 0x05, b, l);
                z_log(pdLOG_INFO, "BACNET", "WRITE obj: %u:%lu (Prio: %u) = Hex: %02X %02X %02X %02X %02X %02X %02X %02X\n", j.obj_type, (unsigned long)j.obj_instance, j.priority, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
                mstp_state = MSTP_WAIT_FOR_REPLY;
                break;

            case JOB_READ_PROP:
                // Création et émission d'une trame ReadProperty (Service 0x0C)
                l = build_read_property_apdu(b, next_invoke_id++, j.obj_type, j.obj_instance, j.prop_id, j.array_index);
                z_log(pdLOG_INFO, "BACNET", "READ obj: %u:%lu (Prop %u) MAC %d\n", j.obj_type, (unsigned long)j.obj_instance, j.prop_id, j.target_mac);
                
                xPendingReadJob = j;
                xReadJobPending = true;
                
                waiting_for_reply = true;
                retry_count = 0;
                send_mstp_frame(j.target_mac, 0x05, b, l);
                z_log(pdLOG_INFO, "BACNET", "READ obj: %u:%lu (Prio: %u) = Hex: %02X %02X %02X %02X %02X %02X %02X %02X\n", j.obj_type, (unsigned long)j.obj_instance, j.priority, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);

                mstp_state = MSTP_WAIT_FOR_REPLY;
                break;

            default:
                z_log(pdLOG_WARN, "BACNET", "Unknown Job Type: %d\n", (int)j.type);
                break;
        }
        frame_count++;
    } else if (!bacnet_network_cache.empty()) {
        // Logique de découverte automatique si aucun job prioritaire
        if (current_dev_idx >= bacnet_network_cache.size()) current_dev_idx = 0;
        execute_discovery_logic(bacnet_network_cache[current_dev_idx]);
    }

    xSemaphoreGive(cache_mutex);
}

/**
 * @brief Types de stockage pour les propriétés découvertes.
 */

/**
 * @brief Structure de configuration pour une étape de découverte BACnet.
 */

/**
 * @brief Table de correspondance entre les étapes de la FSM et les propriétés BACnet.
 */

/**
 * @brief Exécute la logique de découverte pour un appareil spécifique.
 * @details Utilise une approche Data-Driven via DISCOVERY_STEPS pour minimiser le code.
 */
void execute_discovery_logic(BACnetDevice &dev) {
    if (dev.xDiscoveryDone) {
        execute_polling_logic(dev);
        return;
    }

    // v6.8.8: Si le device n'est pas activé, on s'arrête APRÈS avoir récupéré les infos de base
    if (!dev.xEnabled && dev.ucDiscStep >= DISC_OBJ_OID) {
        // On met en pause la découverte. Elle reprendra dès que xEnabled passera à true.
        return;
    }

    uint8_t a[64];
    uint16_t al = 0;

    // 1. Paramètres par défaut basés sur l'étape actuelle
    uint16_t type = 8;               // Type Device par défaut
    uint32_t inst = dev.ulDeviceId;  // Instance du device cible
    const DiscoveryStepConfig &cfg = DISCOVERY_STEPS[dev.ucDiscStep];
    uint16_t prop = cfg.prop;
    int32_t index = cfg.idx;

    // 2. Logique de transition et de construction (v6.8.8: Switch-Case pour plus de clarté)
    switch (dev.ucDiscStep) {
        case DISC_DEV_ID:
            inst = 4194303; // Wildcard
            break;

        case DISC_DEV_NAME:
        case DISC_DEV_VENDOR:
        case DISC_DEV_MAX_APDU:
        case DISC_DEV_TIMEOUT:
        case DISC_DEV_RETRIES:
        case DISC_OBJ_COUNT:
            // Rien de spécial, utilise les params par défaut du Header (prop/idx)
            break;

        default:
            // Phases de découverte d'objets individuels
            if (dev.ucDiscStep >= DISC_OBJ_OID) {
                if (dev.objects.empty() && dev.xEnabled) {
                    dev.ucDiscStep = DISC_DEV_ID;
                    return;
                }

                if (dev.usDiscObjIdx >= dev.objects.size()) {
                    dev.xDiscoveryDone = true;
                    dev.xDirty = true; dev.ulLastDirtyTime = millis();
                    trigger_ha_discovery(dev.ulDeviceId, 0xFFFFFFFF, 0xFFFF);
                    return;
                }

                auto& o = dev.objects[dev.usDiscObjIdx];
                if (dev.ucDiscStep == DISC_OBJ_OID) {
                    index = dev.usDiscObjIdx + 1; // Index 1-based
                } else {
                    type = o.usType;
                    inst = o.ulInstance;

                    // AJOUT : Bypass de sécurité. Si on doit lire la valeur mais que la sonde est en panne (Fault=1)
                    if (dev.ucDiscStep == DISC_OBJ_VALUE && o.isFault()) {
                        o.fPresentValue = NAN; // On invalide la valeur
                        if(!dev.xReloadSingle) dev.usDiscObjIdx++;
                        dev.ucDiscStep = DISC_OBJ_OID;
                        prop = 0;
                        break;
                    }

                    if (dev.ucDiscStep == DISC_OBJ_STATES) {
                        if (o.ucExpectedStatesCount > 0 && o.state_texts.empty()) index = -1;
                        else if (!o.state_texts.empty()) {
                            dev.ucDiscStep = DISC_OBJ_COMMANDABLE;
                            prop = DISCOVERY_STEPS[dev.ucDiscStep].prop;
                            index = DISCOVERY_STEPS[dev.ucDiscStep].idx;
                        }
                    }
                }
            }
            break;
    }

    // 3. Construction de l'APDU ReadProperty
    if (prop > 0) {
        al = build_read_property_apdu(a, next_invoke_id++, type, inst, prop, index);
    }

    // 4. Gestion de la transmission
    if (al > 0) {
        retry_count = 0;
        waiting_for_reply = true;
        send_mstp_frame(dev.ucMacAddress, 0x05, a, al);
        frame_count++;
        mstp_state = MSTP_WAIT_FOR_REPLY;
    } else {
        current_dev_idx = (current_dev_idx + 1) % bacnet_network_cache.size();
        mstp_state = MSTP_DONE_WITH_TOKEN;
    }
}

void execute_polling_logic(BACnetDevice &dev) {
    uint8_t a[512]; uint16_t al = 0; size_t c = dev.objects.size();
    std::vector<BACnetObject*> batch;
    
    if (c > 0 && dev.xSupportsRpm) {
        size_t s = 0;
        // Collect up to max APDU limit objects for this device
        while (s < c && batch.size() < 21) { 
            current_poll_idx = (current_poll_idx + 1) % c; 
            auto& o = dev.objects[current_poll_idx]; 
            if (o.xEnabled && o.usType != 8 && o.usType != 65535 && (o.ulLastUpdate == 0 || (millis()-o.ulLastUpdate)>(sysCfg.bacnet_poll_interval*1000))) { 
                batch.push_back(&o);
            } 
            s++; 
        }
        if (!batch.empty()) {
            al = build_read_property_multiple_apdu(a, next_invoke_id++, batch, 85);
            period_poll_count += batch.size();
        }
    } else if (c > 0) {
        // Fallback to single read
        size_t s = 0; 
        while (s < c) { 
            current_poll_idx = (current_poll_idx + 1) % c; 
            auto& o = dev.objects[current_poll_idx]; 
            if (o.xEnabled && o.usType != 8 && o.usType != 65535 && (o.ulLastUpdate == 0 || (millis()-o.ulLastUpdate)>(sysCfg.bacnet_poll_interval*1000))) { 
                al = build_read_property_apdu(a, next_invoke_id++, o.usType, o.ulInstance, 85, -1); 
                period_poll_count++; 
                break; 
            } 
            s++; 
        }
    }
    
    if (al > 0) { 
        retry_count = 0; 
        waiting_for_reply = true; 
        send_mstp_frame(dev.ucMacAddress, 0x05, a, al); 
        frame_count++; 
        mstp_state = MSTP_WAIT_FOR_REPLY; 
    } else { 
        current_dev_idx = (current_dev_idx + 1) % bacnet_network_cache.size(); 
        mstp_state = MSTP_DONE_WITH_TOKEN; 
    }
}

/**
 * @brief Traite une trame MS/TP entrante et délègue selon le type de PDU.
 */
void process_incoming_frame(MSTP_Frame &frame) {
    if (frame.dest != 0xFF && frame.dest != sysCfg.ucMacAddress) return;
    if (frame.type == 0x02) { next_station = frame.src; ring_stable = true; return; }
    if (frame.type != 0x05 && frame.type != 0x06) return;
    
    bacnetStats.ulMsMsgsRx++; 

    if (frame.len < 2 || frame.data[0] != 0x01) return;
    uint8_t ctrl = frame.data[1]; uint16_t pos = 2;
    if ((ctrl & 0x80) != 0) return;
    if ((ctrl & 0x20) != 0) { if (pos + 2 >= frame.len) return; pos += 3 + frame.data[pos+2]; }
    if ((ctrl & 0x08) != 0) { if (pos + 2 >= frame.len) return; pos += 3 + frame.data[pos+2]; }
    if ((ctrl & 0x20) != 0) pos += 1;
    if (pos >= frame.len) return;

    uint8_t *apdu = &frame.data[pos]; 
    uint16_t al = frame.len - pos;
    uint8_t pdu_type = apdu[0] & 0xF0;

    // 1. Unconfirmed-Request (I-Am)
    if (apdu[0] == 0x10 && apdu[1] == 0x00) {
        handle_i_am_response(frame.src, apdu, al);
        return;
    }

    if (frame.dest != sysCfg.ucMacAddress) return;

    // 2. Traitement selon le type de PDU
    switch (pdu_type) {
        case 0x20: // Simple-ACK
            handle_simple_ack(frame.src);
            break;

        case 0x30: { // Complex-ACK
            uint16_t ap = 3; BACnetTag t; uint8_t pid = 0xFF;
            z_log(pdLOG_DEBUG, "BACNET", "Complex-ACK from MAC %d (Invoke %d)\n", frame.src, apdu[1]);
            while (ap < al && decode_next_tag(apdu, &ap, al, &t)) {
                if (t.number == 1) { 
                    pid = 0; for(int i=0; i<t.len; i++) pid = (pid<<8)|apdu[ap+i]; 
                    ap += t.len; 
                }
                else if (t.isOpening && t.number == 3) {
                    if (xSemaphoreTake(cache_mutex, 0)) {
                        if (current_dev_idx < bacnet_network_cache.size()) {
                            auto& dev = bacnet_network_cache[current_dev_idx];
                            BACnetTag vt;
                            
                            while (ap < al && decode_next_tag(apdu, &ap, al, &vt)) {
                                if (vt.isClosing && vt.number == 3) break;
                                if (!dev.xDiscoveryDone) {
                                    DISC_STEP_T step_before = dev.ucDiscStep;
                                    handle_complex_ack_discovery(dev, apdu, al, ap, vt);
                                    ap += vt.len; // v6.8.10: Avancer IMPÉRATIVEMENT le pointeur après traitement
                                    if (dev.ucDiscStep != step_before) break; 
                                }
                                else {
                                    handle_complex_ack_polling(dev, apdu, al, ap, vt);
                                    // handle_complex_ack_polling gère déjà l'incrément de ap en interne
                                }
                            }
                        }
                        xSemaphoreGive(cache_mutex);
                    }
                } else ap += t.len;
            }
            current_dev_idx = (current_dev_idx + 1) % bacnet_network_cache.size();
            break;
        }

        case 0x50: // Error
        case 0x60: // Reject
        case 0x70: // Abort
            {
                char hex_str[64] = "";
                int pos_str = 0;
                for (int i = 0; i < al && i < 20; i++) {
                    pos_str += snprintf(hex_str + pos_str, sizeof(hex_str) - pos_str, "%02X ", apdu[i]);
                }
                z_log(pdLOG_WARN, "BACNET", "PDU 0x%02X from MAC %d, hex: %s\n", pdu_type, frame.src, hex_str);
            }
            if (xSemaphoreTake(cache_mutex, 0)) {
                if (current_dev_idx < bacnet_network_cache.size()) {
                    handle_error_pdu(bacnet_network_cache[current_dev_idx], pdu_type);
                }
                xSemaphoreGive(cache_mutex);
            }
            break;

        default:
            break;
    }
}

static void bacnet_task(void *pv) {
    MSTP_Frame frame;
    z_log(pdLOG_INFO,"BACNET","Master FSM Task Started (Priority 15)\n");

    for (;;) {
        uint32_t now = millis();

        // v6.9.7: Lazy Save NVS déporté sur le Core 0 dans la tâche mqtt_gatekeeper_task pour éviter le gel de la FSM MS/TP

        timer_silence_us = (uint32_t)(esp_timer_get_time() - last_rx_time_us);

        // --- TÂCHES PÉRIODIQUES (Découverte / Santé) ---
        uint32_t who_is_interval = (bacnet_network_cache.empty()) ? 2000 : 300000;
        // v6.9.4: Forcer un Who-Is initial 3 secondes après le boot pour réveiller le bus et démarrer la ronde
        bool force_initial_who_is = (last_who_is_time == 0 && millis() > 3000);
        if (force_initial_who_is || (millis() - last_who_is_time > who_is_interval)) {
            BACnetJob job; job.type = JOB_WHO_IS; job.target_mac = 0xFF;
            enqueue_bacnet_job(job);
            last_who_is_time = millis();
        }

        static uint32_t heartbeat_timer = 0;
        if (millis() - heartbeat_timer > sysCfg.heartbeat_interval) {
            uint32_t enabled_count = 0;
            uint32_t cache_size = 0;
            if (xSemaphoreTake(cache_mutex, 0)) {
                cache_size = (uint32_t)bacnet_network_cache.size();
                for(auto& d : bacnet_network_cache) {
                    if (d.objects.size() < 1000) { // Safety check
                        for(auto& o : d.objects) if(o.xEnabled) enabled_count++;
                    }
                }
                xSemaphoreGive(cache_mutex);
            }
            z_log(pdLOG_DEBUG,"BACNET","Heartbeat - Tokens:%lu, RX:%lu, TX:%lu (State:%d, Cache:%u, Enabled:%lu)\n", 
                  bacnetStats.ulTokensSeen, bacnetStats.ulMsMsgsRx, bacnetStats.ulMsMsgsTx, (int)mstp_state, cache_size, enabled_count);
            z_log(pdLOG_DEBUG,"BACNET","Polling - objects polled : %lu\n", (unsigned long)period_poll_count);
            period_poll_count = 0;
            heartbeat_timer = millis();
        }

        static uint32_t recovery_timer = 0;
        if (millis() - recovery_timer > 60000) { // Check every 60s
            if (xSemaphoreTake(cache_mutex, 0)) {
                for (auto& dev : bacnet_network_cache) {
                    if (dev.xDiscoveryDone) {
                        for (size_t i = 0; i < dev.objects.size(); i++) {
                            auto& o = dev.objects[i];
                            if ((o.usType == 13 || o.usType == 14 || o.usType == 19) && o.xEnabled && o.state_texts.empty()) {
                                dev.xDiscoveryDone = false;
                                dev.usDiscObjIdx = i;
                                dev.ucDiscStep = DISC_OBJ_STATES;
                                dev.xRecoveryMode = true;
                                z_log(pdLOG_INFO, "BACNET", "Recovery: Fetching missing states for Obj %zu (MAC %d)\n", i+1, dev.ucMacAddress);
                                break; 
                            }
                        }
                    }
                }
                xSemaphoreGive(cache_mutex);
            }
            recovery_timer = millis();
        }

        // --- CONSOMMATION DE LA QUEUE RX ---
        // v6.9.4: Timeout de 1 tick (1ms) pour bloquer proprement et éviter de saturer le CPU si la queue est vide
        ReceivedValidFrame = (xQueueReceive(mstp_rx_queue, &frame, pdMS_TO_TICKS(1)) == pdTRUE);
        
        // Axe 4: Traitement de l'auto-découverte MAC asynchrone
        uint8_t new_mac;
        while (xQueueReceive(mac_discovery_queue, &new_mac, 0)) {
            if (xSemaphoreTake(cache_mutex, 0)) {
                bool exists = false;
                for(auto& d : bacnet_network_cache) if(d.ucMacAddress == new_mac){ exists = true; break; }
                if(!exists){
                    BACnetDevice d; d.ucMacAddress = new_mac; d.ulDeviceId = 4194303; 
                    d.xEnabled = false; d.xDiscoveryDone = false; d.ucDiscStep = DISC_DEV_ID;
                    bacnet_network_cache.push_back(d);
                    z_log(pdLOG_INFO, "BACNET", "Found New MAC (Async): %u\n", new_mac);
                }
                xSemaphoreGive(cache_mutex);
            }
        }

        if (ReceivedValidFrame) {
            frame_type = frame.type; dest_mac = frame.dest; src_mac = frame.src;
            data_len = frame.len; memcpy(data_buf, frame.data, frame.len);
            // On utilise le timestamp capturé par la tâche RX pour la précision du silence
            last_rx_time_us = frame.timestamp_us;
        }

        // --- ORCHESTRATION DE LA MACHINE À ÉTATS MAÎTRE ---
        switch (mstp_state) {
            case MSTP_INITIALIZE: 
                next_station = (sysCfg.ucMacAddress + 1) % 128;
                poll_station = (sysCfg.ucMacAddress + 1) % 128;
                next_station = (sysCfg.ucMacAddress + 1) % (sysCfg.max_master + 1);
                poll_station = (sysCfg.ucMacAddress + 1) % (sysCfg.max_master + 1);
                ring_stable = false;
                mstp_state = MSTP_IDLE; 
                break;
            case MSTP_IDLE:             handle_mstp_idle(); break;
            case MSTP_USE_TOKEN:        handle_mstp_use_token(); break;
            case MSTP_WAIT_FOR_REPLY:   handle_mstp_wait_for_reply(); break;
            case MSTP_POLL_FOR_MASTER:  handle_mstp_poll_for_master(); break;
            case MSTP_PASS_TOKEN:       handle_mstp_pass_token(); break;
            case MSTP_NO_TOKEN:         mstp_state = MSTP_POLL_FOR_MASTER; break;
            case MSTP_DONE_WITH_TOKEN: 
                if (frame_count < sysCfg.max_info_frames && has_bacnet_work()) mstp_state = MSTP_USE_TOKEN;
                else mstp_state = MSTP_PASS_TOKEN;
                break;
            case MSTP_ANSWER_DATA_REQUEST: mstp_state = MSTP_IDLE; break;
        }

        // Céder le processeur de façon coopérative aux tâches de priorité équivalente (comme mstp_rx_task ou autres sur Core 1)
        taskYIELD();
    }
}

void setup_bacnet_engine() {
    bacnet_job_queue = xQueueCreate(20, sizeof(BACnetJob));
    mac_discovery_queue = xQueueCreate(10, sizeof(uint8_t));
    mstp_rx_queue = xQueueCreate(20, sizeof(MSTP_Frame));
    
    const uart_config_t uc = { 
        .baud_rate = 38400, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE, 
        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, 
        .rx_flow_ctrl_thresh = 122, .source_clk = UART_SCLK_APB 
    };
    uart_driver_install(RS485_UART_PORT, 2048, 2048, 20, &uart_evt_queue, 0);
    uart_param_config(RS485_UART_PORT, &uc);
    uart_set_pin(RS485_UART_PORT, TX_PIN, RX_PIN, RTS_PIN, UART_PIN_NO_CHANGE);
    uart_set_mode(RS485_UART_PORT, UART_MODE_RS485_HALF_DUPLEX);
    uart_set_rx_timeout(RS485_UART_PORT, 2); 

    // Tâche RX : Priorité Critique (20), Core 1
    xTaskCreatePinnedToCore(mstp_rx_task, "MSTP_RX", 8192, NULL, 20, NULL, 1);
    
    // Tâche Master : Priorité Haute (15), Core 1
    xTaskCreatePinnedToCore(bacnet_task, "BACnet_Master", 24576, NULL, 15, NULL, 1);
    
    z_log(pdLOG_INFO,"BACNET","BACnet Multi-Task Engine Initialized\n");
}

bool enqueue_bacnet_job(BACnetJob job) { if (bacnet_job_queue == NULL) return false; return xQueueSend(bacnet_job_queue, &job, 0) == pdTRUE; }

void publish_all_names() {
    for (auto& dev : bacnet_network_cache) {
        for (auto& obj : dev.objects) {
            if (obj.usType == 65535 || obj.xEnabled == false) continue;
            if (!obj.xNamePublished || (strcmp(obj.cName, obj.cLastMqttName) != 0)) {
                publish_mqtt_topic(dev.ulDeviceId, obj, 77, true);
                obj.xNamePublished = true; strlcpy(obj.cLastMqttName, obj.cName, sizeof(obj.cLastMqttName));
            }
        }
    }
}
