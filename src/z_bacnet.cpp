#include "z_bacnet.h"
#include <string.h>
#include <algorithm>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "z_network.h" 
#include "rom/ets_sys.h"
#include "z_mqtt.h"
#include "z_nvs.h"

uint32_t period_poll_count = 0;
BACnet_Stats bacnetStats = {0, 0, 0, 0, 0, 0, 0, false};
std::vector<BACnetDevice> bacnet_network_cache;
QueueHandle_t bacnet_job_queue = NULL;
QueueHandle_t uart_evt_queue = NULL;
QueueHandle_t mstp_rx_queue = NULL;
SemaphoreHandle_t cache_mutex = NULL;

// --- VARIABLES D'ÉTAT MS/TP (v6.3.0) ---
static uint8_t last_apdu[512];
static uint16_t last_apdu_len = 0;
static uint8_t last_sent_invoke_id = 0;
static uint8_t next_invoke_id = 10;
static uint8_t retry_count = 0;
static BACnetJob pending_write_job;

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
static uint8_t current_poll_idx = 0;
static uint8_t current_dev_idx = 0;
static uint32_t last_who_is_time = 0;

static const uint32_t T_TURNAROUND_US = 1050;
static const uint32_t T_NO_TOKEN_US = 500000;

// --- PROTOTYPES INTERNES (v6.3.0) ---
static bool validate_rx_header_crc(const uint8_t *header);
static bool validate_rx_data_crc(const uint8_t *data, size_t len);
static void send_mstp_frame(uint8_t target, uint8_t type, const uint8_t* apdu, uint16_t len);
bool has_bacnet_work();

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
                                z_log(pdLOG_DEBUG, "MSTP", "RX Frame: T=0x%02X, S=%d, D=%d, L=%d\n", frame.type, frame.src, frame.dest, frame.len);
                            }

                            // Auto-Discovery MAC (Cout CPU faible)
                            if (frame.src < 128 && frame.src != sysCfg.ucMacAddress) {
                                if (xSemaphoreTake(cache_mutex, 0)) {
                                    bool k=false; for(auto& d:bacnet_network_cache) if(d.ucMacAddress==frame.src){k=true;break;}
                                    if(!k){
                                        BACnetDevice d; d.ucMacAddress=frame.src; d.ulDeviceId=4194303; d.xEnabled=false; d.xDiscoveryDone=false; d.ucDiscStep=DISC_DEV_ID;
                                        bacnet_network_cache.push_back(d);
                                        z_log(pdLOG_INFO,"BACNET","Found New MAC: %u\n", frame.src);
                                    }
                                    xSemaphoreGive(cache_mutex);
                                }
                            }

                            if (data_len_local > 0 && data_len_local <= 512) { rx_state = RX_DATA; data_idx = 0; } 
                            else { 
                                frame.timestamp_us = last_rx_time_us;
                                xQueueSend(mstp_rx_queue, &frame, 0); 
                                rx_state = RX_IDLE; 
                            }
                        } else { 
                            z_log(pdLOG_DEBUG, "MSTP", "Header CRC Error\n");
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

struct BACnetTag { uint32_t number; bool isContext; uint32_t len; bool isOpening; bool isClosing; };
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

uint16_t build_write_property_value_apdu(uint8_t* buffer, uint8_t invoke_id, uint16_t obj_type, uint32_t obj_instance, uint8_t prop_id, float value) {
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
        case 95: return "no-usUnits"; case 255: return "none";
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
            mstp_state = MSTP_POLL_FOR_MASTER; return;
        }
        if (current_dev_idx < bacnet_network_cache.size()) {
            if (retry_count < sysCfg.max_retries) {
                retry_count++; send_mstp_frame(bacnet_network_cache[current_dev_idx].ucMacAddress, 0x05, last_apdu, last_apdu_len);
                waiting_for_reply = true;
            } else { retry_count = 0; mstp_state = MSTP_DONE_WITH_TOKEN; }
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

void execute_bacnet_work() {
    if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(15))) {
        BACnetJob j; if (xQueueReceive(bacnet_job_queue, &j, 0)) {
            if (j.type == JOB_WHO_IS) { uint8_t b[16]; uint16_t l=0; b[l++]=0x01; b[l++]=0x20; b[l++]=0xFF; b[l++]=0xFF; b[l++]=0x00; b[l++]=0xFF; b[l++]=0x10; b[l++]=0x08; send_mstp_frame(0xFF, 0x06, b, l); }
            else if (j.type == JOB_WRITE_PROP) { uint8_t b[256]; uint16_t al=0; if (j.prop_id == 77) al = build_write_property_name_apdu(b, next_invoke_id++, j.obj_type, j.obj_instance, j.name); else al = build_write_property_value_apdu(b, next_invoke_id++, j.obj_type, j.obj_instance, j.prop_id, j.write_value); pending_write_job = j; waiting_for_reply = true; retry_count = 0; send_mstp_frame(j.target_mac, 0x05, b, al); mstp_state = MSTP_WAIT_FOR_REPLY; }
            frame_count++;
        } else if (!bacnet_network_cache.empty()) {
            if (current_dev_idx >= bacnet_network_cache.size()) current_dev_idx = 0;
            execute_discovery_logic(bacnet_network_cache[current_dev_idx]);
        } xSemaphoreGive(cache_mutex);
    }
}

void execute_discovery_logic(BACnetDevice &dev) {
    uint8_t a[64]; uint16_t al = 0;
    if (!dev.xDiscoveryDone) {
        if (dev.ucDiscStep == DISC_DEV_ID) al = build_read_property_apdu(a, next_invoke_id++, 8, 4194303, 75, -1);
        else if (dev.ucDiscStep == DISC_DEV_NAME) al = build_read_property_apdu(a, next_invoke_id++, 8, dev.ulDeviceId, 77, -1);
        else if (dev.ucDiscStep == DISC_DEV_VENDOR) al = build_read_property_apdu(a, next_invoke_id++, 8, dev.ulDeviceId, 121, -1);
        else if (dev.ucDiscStep == DISC_OBJ_COUNT) al = build_read_property_apdu(a, next_invoke_id++, 8, dev.ulDeviceId, 76, 0);
        else if (dev.usDiscObjIdx < dev.objects.size()) {
            auto& o = dev.objects[dev.usDiscObjIdx];
            if (dev.ucDiscStep == DISC_OBJ_OID) al = build_read_property_apdu(a, next_invoke_id++, 8, dev.ulDeviceId, 76, dev.usDiscObjIdx + 1);
            else if (dev.ucDiscStep == DISC_OBJ_NAME) al = build_read_property_apdu(a, next_invoke_id++, o.usType, o.ulInstance, 77, -1);
            else if (dev.ucDiscStep == DISC_OBJ_UNITS) al = build_read_property_apdu(a, next_invoke_id++, o.usType, o.ulInstance, 117, -1);
            else if (dev.ucDiscStep == DISC_OBJ_MIN) al = build_read_property_apdu(a, next_invoke_id++, o.usType, o.ulInstance, 69, -1);
            else if (dev.ucDiscStep == DISC_OBJ_MAX) al = build_read_property_apdu(a, next_invoke_id++, o.usType, o.ulInstance, 65, -1);
            else if (dev.ucDiscStep == DISC_OBJ_STATES) { if (o.ucExpectedStatesCount == 0) al = build_read_property_apdu(a, next_invoke_id++, o.usType, o.ulInstance, 110, 0); else if (o.state_texts.empty()) al = build_read_property_apdu(a, next_invoke_id++, o.usType, o.ulInstance, 110, -1); else { dev.ucDiscStep = DISC_OBJ_COMMANDABLE; al = build_read_property_apdu(a, next_invoke_id++, o.usType, o.ulInstance, 87, -1); } }
            else if (dev.ucDiscStep == DISC_OBJ_COMMANDABLE) al = build_read_property_apdu(a, next_invoke_id++, o.usType, o.ulInstance, 87, -1);
            else if (dev.ucDiscStep == DISC_OBJ_VALUE) al = build_read_property_apdu(a, next_invoke_id++, o.usType, o.ulInstance, 85, -1);
        } else { dev.xDiscoveryDone = true; save_device_objects_locked(dev.ulDeviceId); trigger_ha_discovery(dev.ulDeviceId, 0xFFFFFFFF, 0xFFFF); return; }
    } else { execute_polling_logic(dev); return; }
    if (al > 0) { retry_count = 0; waiting_for_reply = true; send_mstp_frame(dev.ucMacAddress, 0x05, a, al); frame_count++; mstp_state = MSTP_WAIT_FOR_REPLY; }
    else { current_dev_idx = (current_dev_idx + 1) % bacnet_network_cache.size(); mstp_state = MSTP_DONE_WITH_TOKEN; }
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

void process_incoming_frame(MSTP_Frame &frame) {
    if (frame.dest != 0xFF && frame.dest != sysCfg.ucMacAddress) return;
    if (frame.type == 0x02) { next_station = frame.src; ring_stable = true; return; }
    if (frame.type != 0x05 && frame.type != 0x06) return;
    
    bacnetStats.ulMsMsgsRx++; // Increment RX stats for Data Frames

    if (frame.len < 2 || frame.data[0] != 0x01) return;
    uint8_t ctrl = frame.data[1]; uint16_t pos = 2;
    if ((ctrl & 0x80) != 0) return;
    if ((ctrl & 0x20) != 0) { if (pos + 2 >= frame.len) return; pos += 3 + frame.data[pos+2]; }
    if ((ctrl & 0x08) != 0) { if (pos + 2 >= frame.len) return; pos += 3 + frame.data[pos+2]; }
    if ((ctrl & 0x20) != 0) pos += 1;
    if (pos >= frame.len) return;
    uint8_t *apdu = &frame.data[pos]; uint16_t al = frame.len - pos;
    uint8_t type = apdu[0] & 0xF0;

    if (apdu[0] == 0x10 && apdu[1] == 0x00) { // I-Am
        if (al >= 6 && apdu[2] == 0xC4) {
            uint32_t ulDeviceId = ((apdu[3]<<24)|(apdu[4]<<16)|(apdu[5]<<8)|apdu[6]) & 0x3FFFFF;
            if (xSemaphoreTake(cache_mutex, 0)) {
                bool exists = false; for (auto& d : bacnet_network_cache) { if (d.ulDeviceId == ulDeviceId) { exists = true; d.last_seen = millis(); break; } }
                if (!exists) {
                    BACnetDevice nd; nd.ulDeviceId = ulDeviceId; nd.ucMacAddress = frame.src; nd.xEnabled = false; nd.xDiscoveryDone = false; nd.last_seen = millis(); nd.ucDiscStep = DISC_DEV_ID;
                    bacnet_network_cache.push_back(nd); z_log(pdLOG_INFO,"BACNET", "New Device: ID %lu, MAC %d\n", ulDeviceId, frame.src);
                } xSemaphoreGive(cache_mutex);
            }
        } return;
    }
    if (frame.dest != sysCfg.ucMacAddress) return;
    if (type == 0x20) { // Simple-ACK
        z_log(pdLOG_DEBUG, "BACNET", "Simple-ACK from MAC %d (Success)\n", frame.src);
        if (xSemaphoreTake(cache_mutex, 0)) {
            for(auto& d : bacnet_network_cache) if (d.ucMacAddress == frame.src) {
                for(auto& o : d.objects) if (o.usType == pending_write_job.obj_type && o.ulInstance == pending_write_job.obj_instance) {
                    if (pending_write_job.prop_id == 85) { o.fPresentValue = pending_write_job.write_value; publish_mqtt_topic(d.ulDeviceId, o, 85, false); }
                    else if (pending_write_job.prop_id == 77) publish_mqtt_topic(d.ulDeviceId, o, 77, true);
                    o.ulLastUpdate = millis(); break;
                } break;
            } xSemaphoreGive(cache_mutex);
        } current_dev_idx = (current_dev_idx + 1) % bacnet_network_cache.size();
    } else if (type == 0x30) { // Complex-ACK
        uint16_t ap = 3; BACnetTag t; uint8_t pid = 0xFF;
        z_log(pdLOG_DEBUG, "BACNET", "Complex-ACK from MAC %d (Invoke %d)\n", frame.src, apdu[1]);
        while (ap < al && decode_next_tag(apdu, &ap, al, &t)) {
            if (t.number == 1) { pid = 0; for(int i=0; i<t.len; i++) pid = (pid<<8)|apdu[ap+i]; ap += t.len; }
            else if (t.isOpening && t.number == 3) {
                if (xSemaphoreTake(cache_mutex, 0)) {
                    if (current_dev_idx < bacnet_network_cache.size()) {
                        auto& dev = bacnet_network_cache[current_dev_idx]; BACnetTag vt;
                        while (ap < al && decode_next_tag(apdu, &ap, al, &vt)) {
                            if (vt.isClosing && vt.number == 3) break;
                            if (!dev.xDiscoveryDone) {
                                if (dev.ucDiscStep == DISC_DEV_ID) { dev.ulDeviceId = (((apdu[ap]<<24)|(apdu[ap+1]<<16)|(apdu[ap+2]<<8)|apdu[ap+3]) & 0x3FFFFF); z_log(pdLOG_INFO, "BACNET", "Device ID: %lu\n", (unsigned long)dev.ulDeviceId); dev.ucDiscStep = DISC_DEV_NAME; }
                                else if (dev.ucDiscStep == DISC_DEV_NAME) { char n[33]; uint8_t enc = apdu[ap]; int sl=0; if(enc==0){sl=std::min((int)vt.len-1,32); memcpy(n,&apdu[ap+1],sl);} else {for(int i=2;i<vt.len&&sl<32;i+=2) n[sl++]=apdu[ap+i];} n[sl]=0; dev.name=String(n); z_log(pdLOG_INFO, "BACNET", "Device Name: %s\n", n); dev.ucDiscStep=DISC_DEV_VENDOR; }
                                else if (dev.ucDiscStep == DISC_DEV_VENDOR) { char n[33]; uint8_t enc = apdu[ap]; int sl=0; if(enc==0){sl=std::min((int)vt.len-1,32); memcpy(n,&apdu[ap+1],sl);} else {for(int i=2;i<vt.len&&sl<32;i+=2) n[sl++]=apdu[ap+i];} n[sl]=0; dev.vendor=String(n); z_log(pdLOG_INFO, "BACNET", "Device Vendor: %s\n", n); dev.ucDiscStep=DISC_OBJ_COUNT; }
                                else if (dev.ucDiscStep == DISC_OBJ_COUNT) { 
                                    uint32_t c=0; for(int i=0;i<vt.len;i++) c=(c<<8)|apdu[ap+i]; 
                                    z_log(pdLOG_INFO, "BACNET", "Device Object Count: %lu\n", (unsigned long)c); 
                                    dev.objects.clear(); 
                                    dev.objects.reserve(c); 
                                    for(int i=0;i<c;i++){ BACnetObject o; o.usType=65535; dev.objects.push_back(o); } 
                                    dev.usDiscObjIdx=0; 
                                    dev.ucDiscStep=DISC_OBJ_OID; 
                                    
                                    // v6.3.8: Point d'arrêt passif SI désactivé
                                    if (!dev.xEnabled) {
                                        z_log(pdLOG_INFO, "BACNET", "Passive Discovery: MAC %d paused after Phase 1. Waiting for manual ON.\n", dev.ucMacAddress);
                                        dev.xDiscoveryDone = true;
                                        save_device_objects_locked(dev.ulDeviceId);
                                        xSemaphoreGive(cache_mutex);
                                        return; 
                                    }
                                }
                                else if (dev.usDiscObjIdx < dev.objects.size()) {
                                    auto& o = dev.objects[dev.usDiscObjIdx];
                                    if (dev.ucDiscStep == DISC_OBJ_OID) { uint32_t oid = (apdu[ap]<<24)|(apdu[ap+1]<<16)|(apdu[ap+2]<<8)|apdu[ap+3]; o.usType=oid>>22; o.ulInstance=oid&0x3FFFFF; z_log(pdLOG_INFO, "BACNET", "Obj %u OID: T%u I%lu\n", dev.usDiscObjIdx+1, o.usType, (unsigned long)o.ulInstance); dev.ucDiscStep=DISC_OBJ_NAME; }
                                    else if (dev.ucDiscStep == DISC_OBJ_NAME) { 
                                        char n[33]; uint8_t enc=apdu[ap]; int sl=0; if(enc==0){sl=std::min((int)vt.len-1,32); memcpy(n,&apdu[ap+1],sl);} else {for(int i=2;i<vt.len&&sl<32;i+=2) n[sl++]=apdu[ap+i];} n[sl]=0; 
                                        String ns = String(n); ns.trim();
                                        if (ns.length() == 0 || ns.equalsIgnoreCase("Unknown") || ns.equalsIgnoreCase("Untitled")) {
                                            const char* ts = (o.usType == 0) ? "AI" : (o.usType == 1) ? "AO" : (o.usType == 2) ? "AV" : 
                                                            (o.usType == 3) ? "BI" : (o.usType == 4) ? "BO" : (o.usType == 5) ? "BV" : 
                                                            (o.usType == 13) ? "MSI" : (o.usType == 14) ? "MSO" : (o.usType == 19) ? "MSV" : "OBJ";
                                            snprintf(o.cName, sizeof(o.cName), "%s-%lu", ts, (unsigned long)o.ulInstance);
                                            z_log(pdLOG_INFO, "BACNET", "Obj %u: Using Fallback Name %s\n", dev.usDiscObjIdx+1, o.cName);
                                        } else { strlcpy(o.cName, n, sizeof(o.cName)); }
                                        z_log(pdLOG_INFO, "BACNET", "Obj %u Name: %s\n", dev.usDiscObjIdx+1, o.cName);
                                        
                                        if(o.usType<=2||o.usType==23||o.usType==24||o.usType==46) dev.ucDiscStep=DISC_OBJ_UNITS; 
                                        else if(o.usType==13||o.usType==14||o.usType==19){ o.state_texts.clear(); o.ucExpectedStatesCount=0; dev.ucDiscStep=DISC_OBJ_STATES; } 
                                        else dev.ucDiscStep=DISC_OBJ_COMMANDABLE; 
                                    }
                                    else if (dev.ucDiscStep == DISC_OBJ_UNITS) { 
                                        uint32_t u=0; for(int i=0;i<vt.len;i++) u=(u<<8)|apdu[ap+i]; 
                                        o.usUnits=u; String us=get_unit_text(u); strlcpy(o.cUnitText,us.c_str(),sizeof(o.cUnitText)); 
                                        z_log(pdLOG_INFO, "BACNET", "Obj %u Units: %s\n", dev.usDiscObjIdx+1, o.cUnitText); 
                                        dev.ucDiscStep = DISC_OBJ_MIN; 
                                    }
                                    else if (dev.ucDiscStep == DISC_OBJ_MIN) {
                                        if (vt.number == 4) { // REAL
                                            float v; uint32_t tm; memcpy(&tm, &apdu[ap], 4); tm = __builtin_bswap32(tm); memcpy(&v, &tm, 4);
                                            o.fMinValue = v; z_log(pdLOG_INFO, "BACNET", "Obj %u Min: %.2f\n", dev.usDiscObjIdx+1, v);
                                        }
                                        dev.ucDiscStep = DISC_OBJ_MAX;
                                    }
                                    else if (dev.ucDiscStep == DISC_OBJ_MAX) {
                                        if (vt.number == 4) { // REAL
                                            float v; uint32_t tm; memcpy(&tm, &apdu[ap], 4); tm = __builtin_bswap32(tm); memcpy(&v, &tm, 4);
                                            o.fMaxValue = v; z_log(pdLOG_INFO, "BACNET", "Obj %u Max: %.2f\n", dev.usDiscObjIdx+1, v);
                                        }
                                        if(o.usType==13||o.usType==14||o.usType==19){ o.state_texts.clear(); o.ucExpectedStatesCount=0; dev.ucDiscStep=DISC_OBJ_STATES; } 
                                        else dev.ucDiscStep=DISC_OBJ_COMMANDABLE;
                                    }
                                    else if (dev.ucDiscStep == DISC_OBJ_STATES) { 
                                        if(vt.number==2){ uint32_t c=0; for(int i=0;i<vt.len;i++) c=(c<<8)|apdu[ap+i]; o.ucExpectedStatesCount=c; z_log(pdLOG_INFO, "BACNET", "Obj %u State Count: %lu\n", dev.usDiscObjIdx+1, (unsigned long)c); if(c==0) dev.ucDiscStep=DISC_OBJ_VALUE; } 
                                        else if(vt.number==7){ char n[33]; uint8_t enc=apdu[ap]; int sl=0; if(enc==0){sl=std::min((int)vt.len-1,32); memcpy(n,&apdu[ap+1],sl);} else {for(int i=2;i<vt.len&&sl<32;i+=2) n[sl++]=apdu[ap+i];} n[sl]=0; o.state_texts.push_back(String(n)); z_log(pdLOG_INFO, "BACNET", "Obj %u State %zu: %s\n", dev.usDiscObjIdx+1, o.state_texts.size(), n); 
                                        if(o.state_texts.size()>=o.ucExpectedStatesCount) {
                                            dev.ucDiscStep=DISC_OBJ_COMMANDABLE;
                                            // v6.6.1: Persistance immédiate des labels pour cet objet
                                            save_object_states(dev.ulDeviceId, o.usType, o.ulInstance, o.state_texts);
                                        } 
                                        }
                                    }
                                    else if (dev.ucDiscStep == DISC_OBJ_COMMANDABLE) { o.xIsCommandable=true; z_log(pdLOG_DEBUG, "BACNET", "Obj %u is commandable\n", dev.usDiscObjIdx+1); if(o.xEnabled||dev.xReloadSingle) dev.ucDiscStep=DISC_OBJ_VALUE; else { if(!dev.xReloadSingle) dev.usDiscObjIdx++; dev.ucDiscStep=DISC_OBJ_OID; if(dev.xReloadSingle){dev.xDiscoveryDone=true; dev.xReloadSingle=false;} } ap=al; break; }
                                    else if (dev.ucDiscStep == DISC_OBJ_VALUE) { 
                                        if(vt.number==4){ float v; uint32_t tm; memcpy(&tm,&apdu[ap],4); tm=__builtin_bswap32(tm); memcpy(&v,&tm,4); o.fPresentValue=v; } 
                                        else { uint32_t v=0; for(int i=0;i<vt.len;i++) v=(v<<8)|apdu[ap+i]; o.fPresentValue=v; } 
                                        o.ulLastUpdate=millis(); z_log(pdLOG_INFO, "BACNET", "Obj %u Value: %.2f\n", dev.usDiscObjIdx+1, o.fPresentValue);
                                        bool stop=dev.xReloadSingle||dev.xRecoveryMode; 
                                        if(!stop) dev.usDiscObjIdx++; 
                                        dev.ucDiscStep=DISC_OBJ_OID; 
                                        
                                        if(dev.usDiscObjIdx%50==0||dev.usDiscObjIdx>=dev.objects.size()) save_device_objects_locked(dev.ulDeviceId); 
                                        
                                        if(stop){
                                            dev.xDiscoveryDone=true; dev.xReloadSingle=false; dev.xRecoveryMode=false; 
                                            save_device_objects_locked(dev.ulDeviceId); 
                                            if(o.xEnabled) trigger_ha_discovery(dev.ulDeviceId, o.ulInstance, o.usType);
                                        } else if(dev.usDiscObjIdx>=dev.objects.size()){
                                            dev.xDiscoveryDone=true; 
                                            save_device_objects_locked(dev.ulDeviceId); 
                                            publish_all_names();
                                            trigger_ha_discovery(dev.ulDeviceId, 0xFFFFFFFF, 0xFFFF); 
                                        } 
                                    }
                                }
                            } else {
                                if (apdu[2] == 0x0E) {
                                    // Parse ReadPropertyMultiple ACK
                                    uint32_t current_oid = 0;
                                    while (ap < al) {
                                        BACnetTag t_rpm;
                                        if (!decode_next_tag(apdu, &ap, al, &t_rpm)) break;
                                        if (t_rpm.isOpening || t_rpm.isClosing) continue;
                                        
                                        if (t_rpm.number == 0 && t_rpm.len >= 4) { // ObjectIdentifier
                                            current_oid = (apdu[ap]<<24) | (apdu[ap+1]<<16) | (apdu[ap+2]<<8) | apdu[ap+3];
                                        } 
                                        else if (t_rpm.number == 4 || t_rpm.number == 2 || t_rpm.number == 9) { // Value inside Context 4
                                            float v = 0.0f;
                                            if (t_rpm.number == 4 && t_rpm.len == 4) {
                                                uint32_t tm; memcpy(&tm, &apdu[ap], 4); tm = __builtin_bswap32(tm); memcpy(&v, &tm, 4);
                                            } else {
                                                uint32_t iv = 0; for(int i=0; i<t_rpm.len; i++) iv = (iv<<8) | apdu[ap+i];
                                                v = (float)iv;
                                            }
                                            if (current_oid > 0) {
                                                uint16_t t_type = current_oid >> 22;
                                                uint32_t t_inst = current_oid & 0x3FFFFF;
                                                for (auto& o : dev.objects) {
                                                    if (o.usType == t_type && o.ulInstance == t_inst) {
                                                        o.fPresentValue = v;
                                                        o.ulLastUpdate = millis();
                                                        if (o.xEnabled) publish_mqtt_topic(dev.ulDeviceId, o, 85, false);
                                                        break;
                                                    }
                                                }
                                                current_oid = 0; // Reset for next
                                            }
                                        }
                                        ap += t_rpm.len;
                                    }
                                } else {
                                    // Parse single ReadProperty ACK
                                    if(vt.number==4){ float v; uint32_t tm; memcpy(&tm,&apdu[ap],4); tm=__builtin_bswap32(tm); memcpy(&v,&tm,4); if(current_poll_idx<dev.objects.size()){ auto& o=dev.objects[current_poll_idx]; o.fPresentValue=v; o.ulLastUpdate=millis(); if(o.xEnabled) publish_mqtt_topic(dev.ulDeviceId, o, 85, false); } }
                                    else if(vt.number==2||vt.number==9){ uint32_t v=0; for(int i=0;i<vt.len;i++) v=(v<<8)|apdu[ap+i]; if(current_poll_idx<dev.objects.size()){ auto& o=dev.objects[current_poll_idx]; o.fPresentValue=v; o.ulLastUpdate=millis(); if(o.xEnabled) publish_mqtt_topic(dev.ulDeviceId, o, 85, false); } }
                                    ap += vt.len;
                                }
                                continue;
                            } ap += vt.len;
                        }
                    } xSemaphoreGive(cache_mutex);
                }
            } else ap += t.len;
        } current_dev_idx = (current_dev_idx + 1) % bacnet_network_cache.size();
    } else if (type >= 0x40) {
        if (sysCfg.log_level >= pdLOG_DEBUG) {
            z_log(pdLOG_DEBUG, "BACNET", "Received PDU type 0x%02X (Error/Abort/Reject)\n", type);
        }
        if (current_dev_idx < bacnet_network_cache.size()) {

            auto& dev = bacnet_network_cache[current_dev_idx];
            if (!dev.xDiscoveryDone && dev.usDiscObjIdx < dev.objects.size()) {
                auto& o = dev.objects[dev.usDiscObjIdx];
                if (dev.ucDiscStep == DISC_OBJ_MIN) { dev.ucDiscStep = DISC_OBJ_MAX; }
                else if (dev.ucDiscStep == DISC_OBJ_MAX) {
                    if(o.usType==13||o.usType==14||o.usType==19){ o.state_texts.clear(); o.ucExpectedStatesCount=0; dev.ucDiscStep=DISC_OBJ_STATES; } 
                    else dev.ucDiscStep=DISC_OBJ_COMMANDABLE;
                }
                else if (dev.ucDiscStep == DISC_OBJ_COMMANDABLE) { 
                    o.xIsCommandable = (o.usType==13||o.usType==14||o.usType==19||o.usType<=5);
                    if(o.xEnabled||dev.xReloadSingle) dev.ucDiscStep=DISC_OBJ_VALUE; else { if(!dev.xReloadSingle) dev.usDiscObjIdx++; dev.ucDiscStep=DISC_OBJ_OID; if(dev.xReloadSingle){dev.xDiscoveryDone=true; dev.xReloadSingle=false;} }
                } else { if (retry_count < sysCfg.max_retries) { retry_count++; send_mstp_frame(dev.ucMacAddress, 0x05, last_apdu, last_apdu_len); waiting_for_reply = true; } else { retry_count = 0; if(!dev.xDiscoveryDone){dev.usDiscObjIdx++; dev.ucDiscStep=DISC_OBJ_OID;} current_dev_idx = (current_dev_idx + 1) % bacnet_network_cache.size(); } }
            } else { if (retry_count < sysCfg.max_retries) { retry_count++; send_mstp_frame(dev.ucMacAddress, 0x05, last_apdu, last_apdu_len); waiting_for_reply = true; } else { retry_count = 0; if(!dev.xDiscoveryDone){dev.usDiscObjIdx++; dev.ucDiscStep=DISC_OBJ_OID;} current_dev_idx = (current_dev_idx + 1) % bacnet_network_cache.size(); } }
        }
    }
}

static void bacnet_task(void *pv) {
    MSTP_Frame frame;
    z_log(pdLOG_INFO,"BACNET","Master FSM Task Started (Priority 15)\n");

    for (;;) {
        timer_silence_us = (uint32_t)(esp_timer_get_time() - last_rx_time_us);

        // --- TÂCHES PÉRIODIQUES (Découverte / Santé) ---
        uint32_t who_is_interval = (bacnet_network_cache.empty()) ? 10000 : 300000;
        if (millis() - last_who_is_time > who_is_interval) {
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
            z_log(pdLOG_INFO,"BACNET","Heartbeat - Tokens:%lu, RX:%lu, TX:%lu (State:%d, Cache:%u, Enabled:%lu)\n", 
                  bacnetStats.ulTokensSeen, bacnetStats.ulMsMsgsRx, bacnetStats.ulMsMsgsTx, (int)mstp_state, cache_size, enabled_count);
            z_log(pdLOG_INFO,"BACNET","Polling - objects polled : %lu\n", (unsigned long)period_poll_count);
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
        ReceivedValidFrame = (xQueueReceive(mstp_rx_queue, &frame, 0) == pdTRUE);
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

        if (mstp_state == MSTP_IDLE && !ReceivedValidFrame) vTaskDelay(1);
        else taskYIELD();
    }
}

void setup_bacnet_engine() {
    bacnet_job_queue = xQueueCreate(20, sizeof(BACnetJob));
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
