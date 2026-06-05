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
BACnet_Stats bacnetStats = {0, 0, 0, 0, 0, 0, 0};
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

    z_log(LOG_INFO,"MSTP","RX Task Started (Priority 20)\n");

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
                                z_log(LOG_DEBUG, "MSTP", "RX Frame: T=0x%02X, S=%d, D=%d, L=%d\n", frame.type, frame.src, frame.dest, frame.len);
                            }

                            // Auto-Discovery MAC (Cout CPU faible)
                            if (frame.src < 128 && frame.src != sysCfg.mac_address) {
                                if (xSemaphoreTake(cache_mutex, 0)) {
                                    bool k=false; for(auto& d:bacnet_network_cache) if(d.mac_address==frame.src){k=true;break;}
                                    if(!k){
                                        BACnetDevice d; d.mac_address=frame.src; d.device_id=4194303; d.enabled=false; d.discovery_done=false; d.disc_step=DISC_DEV_ID;
                                        bacnet_network_cache.push_back(d);
                                        z_log(LOG_INFO,"BACNET","Found New MAC: %u\n", frame.src);
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
                            z_log(LOG_DEBUG, "MSTP", "Header CRC Error\n");
                            bacnetStats.errors_crc++; rx_state = RX_IDLE; 
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
                    } else bacnetStats.errors_crc++;
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
    uint8_t b[512+10]; b[0]=0x55; b[1]=0xFF; b[2]=type; b[3]=target; b[4]=sysCfg.mac_address;
    b[5]=(len>>8)&0xFF; b[6]=len&0xFF; b[7]=calc_header_crc(&b[2], 5);
    while ((micros() - last_rx_time_us) < T_TURNAROUND_US) { }
    if (len > 0) {
        memcpy(&b[8], apdu, len); uint16_t c = calc_data_crc(&b[8], len);
        b[8+len]=c&0xFF; b[8+len+1]=(c>>8)&0xFF; uart_tx(b, 8+len+2);
    } else uart_tx(b, 8);
    if (type == 0x05 || type == 0x06) bacnetStats.ms_msgs_tx++;
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

String get_unit_text(uint16_t units) {
    switch(units) {
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
        default: return "Unit " + String(units);
    }
}


static MSTP_MASTER_STATE last_mstp_log_state = MSTP_INITIALIZE;
static void log_mstp_state_change(MSTP_MASTER_STATE new_state) {
    if (new_state != last_mstp_log_state) {
        // z_log(LOG_DEBUG, "MSTP", "State: %d -> %d\n", (int)last_mstp_log_state, (int)new_state);
        last_mstp_log_state = new_state;
    }
}

void handle_mstp_idle() {
    uint32_t tnt = T_NO_TOKEN_US + (sysCfg.mac_address * 10000);
    if (ReceivedValidFrame) {
        if (frame_type == 0x00 && dest_mac == sysCfg.mac_address) { 
            bacnetStats.tokens_seen++; frame_count = 0; mstp_state = MSTP_USE_TOKEN; 
        }
        else if (frame_type == 0x01 && dest_mac == sysCfg.mac_address) {
            send_mstp_frame(src_mac, 0x02, NULL, 0); next_station = src_mac; ring_stable = true;
        } else if (dest_mac == sysCfg.mac_address && frame_type >= 0x05) {
            z_log(LOG_DEBUG, "MSTP", "Data for us from %d (Type 0x%02X)\n", src_mac, frame_type);
            mstp_state = MSTP_ANSWER_DATA_REQUEST;
        } else if (dest_mac == 0xFF || dest_mac == sysCfg.mac_address) {
            MSTP_Frame f = { frame_type, dest_mac, src_mac, data_len, {0}, (uint32_t)esp_timer_get_time() };
            memcpy(f.data, data_buf, data_len); process_incoming_frame(f);
        }
    } else if (timer_silence_us >= tnt) {
        z_log(LOG_INFO, "MSTP", "Silence Timeout (%lu us). Lost Token? Claiming.\n", timer_silence_us);
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
        MSTP_Frame f = { frame_type, dest_mac, src_mac, data_len, {0}, (uint32_t)esp_timer_get_time() };
        memcpy(f.data, data_buf, data_len); process_incoming_frame(f);
        waiting_for_reply = false; mstp_state = MSTP_DONE_WITH_TOKEN;
    } else if (timer_silence_us >= T_REPLY_TIMEOUT_US) {
        waiting_for_reply = false;
        if (!ring_stable) {
            poll_station = (poll_station + 1) % 128; if (poll_station == sysCfg.mac_address) poll_station = (poll_station + 1) % 128;
            mstp_state = MSTP_POLL_FOR_MASTER; return;
        }
        if (current_dev_idx < bacnet_network_cache.size()) {
            if (retry_count < sysCfg.max_retries) {
                retry_count++; send_mstp_frame(bacnet_network_cache[current_dev_idx].mac_address, 0x05, last_apdu, last_apdu_len);
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
            if (!d.discovery_done) { w = true; break; }
            for (const auto& o : d.objects) if (o.enabled && (o.last_update == 0 || (millis()-o.last_update)>=(sysCfg.bacnet_poll_interval*1000))) { w = true; break; }
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
    if (!dev.discovery_done) {
        if (dev.disc_step == DISC_DEV_ID) al = build_read_property_apdu(a, next_invoke_id++, 8, 4194303, 75, -1);
        else if (dev.disc_step == DISC_DEV_NAME) al = build_read_property_apdu(a, next_invoke_id++, 8, dev.device_id, 77, -1);
        else if (dev.disc_step == DISC_DEV_VENDOR) al = build_read_property_apdu(a, next_invoke_id++, 8, dev.device_id, 121, -1);
        else if (dev.disc_step == DISC_OBJ_COUNT) al = build_read_property_apdu(a, next_invoke_id++, 8, dev.device_id, 76, 0);
        else if (dev.disc_obj_idx < dev.objects.size()) {
            auto& o = dev.objects[dev.disc_obj_idx];
            if (dev.disc_step == DISC_OBJ_OID) al = build_read_property_apdu(a, next_invoke_id++, 8, dev.device_id, 76, dev.disc_obj_idx + 1);
            else if (dev.disc_step == DISC_OBJ_NAME) al = build_read_property_apdu(a, next_invoke_id++, o.type, o.instance, 77, -1);
            else if (dev.disc_step == DISC_OBJ_UNITS) al = build_read_property_apdu(a, next_invoke_id++, o.type, o.instance, 117, -1);
            else if (dev.disc_step == DISC_OBJ_MIN) al = build_read_property_apdu(a, next_invoke_id++, o.type, o.instance, 69, -1);
            else if (dev.disc_step == DISC_OBJ_MAX) al = build_read_property_apdu(a, next_invoke_id++, o.type, o.instance, 65, -1);
            else if (dev.disc_step == DISC_OBJ_STATES) { if (o.expected_states_count == 0) al = build_read_property_apdu(a, next_invoke_id++, o.type, o.instance, 110, 0); else if (o.state_texts.empty()) al = build_read_property_apdu(a, next_invoke_id++, o.type, o.instance, 110, -1); else { dev.disc_step = DISC_OBJ_COMMANDABLE; al = build_read_property_apdu(a, next_invoke_id++, o.type, o.instance, 87, -1); } }
            else if (dev.disc_step == DISC_OBJ_COMMANDABLE) al = build_read_property_apdu(a, next_invoke_id++, o.type, o.instance, 87, -1);
            else if (dev.disc_step == DISC_OBJ_VALUE) al = build_read_property_apdu(a, next_invoke_id++, o.type, o.instance, 85, -1);
        } else { dev.discovery_done = true; save_device_objects_locked(dev.device_id); trigger_ha_discovery(dev.device_id, 0xFFFFFFFF, 0xFFFF); return; }
    } else { execute_polling_logic(dev); return; }
    if (al > 0) { retry_count = 0; waiting_for_reply = true; send_mstp_frame(dev.mac_address, 0x05, a, al); frame_count++; mstp_state = MSTP_WAIT_FOR_REPLY; }
    else { current_dev_idx = (current_dev_idx + 1) % bacnet_network_cache.size(); mstp_state = MSTP_DONE_WITH_TOKEN; }
}

void execute_polling_logic(BACnetDevice &dev) {
    uint8_t a[64]; uint16_t al = 0; size_t c = dev.objects.size();
    if (c > 0) { size_t s = 0; while (s < c) { current_poll_idx = (current_poll_idx + 1) % c; auto& o = dev.objects[current_poll_idx]; if (o.enabled && o.type != 8 && o.type != 65535 && (o.last_update == 0 || (millis()-o.last_update)>(sysCfg.bacnet_poll_interval*1000))) { al = build_read_property_apdu(a, next_invoke_id++, o.type, o.instance, 85, -1); period_poll_count++; break; } s++; } }
    if (al > 0) { retry_count = 0; waiting_for_reply = true; send_mstp_frame(dev.mac_address, 0x05, a, al); frame_count++; mstp_state = MSTP_WAIT_FOR_REPLY; }
    else { current_dev_idx = (current_dev_idx + 1) % bacnet_network_cache.size(); mstp_state = MSTP_DONE_WITH_TOKEN; }
}

void process_incoming_frame(MSTP_Frame &frame) {
    if (frame.dest != 0xFF && frame.dest != sysCfg.mac_address) return;
    if (frame.type == 0x02) { next_station = frame.src; ring_stable = true; return; }
    if (frame.type != 0x05 && frame.type != 0x06) return;
    
    bacnetStats.ms_msgs_rx++; // Increment RX stats for Data Frames

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
            uint32_t device_id = ((apdu[3]<<24)|(apdu[4]<<16)|(apdu[5]<<8)|apdu[6]) & 0x3FFFFF;
            if (xSemaphoreTake(cache_mutex, 0)) {
                bool exists = false; for (auto& d : bacnet_network_cache) { if (d.device_id == device_id) { exists = true; d.last_seen = millis(); break; } }
                if (!exists) {
                    BACnetDevice nd; nd.device_id = device_id; nd.mac_address = frame.src; nd.enabled = false; nd.discovery_done = false; nd.last_seen = millis(); nd.disc_step = DISC_DEV_ID;
                    bacnet_network_cache.push_back(nd); z_log(LOG_INFO,"BACNET", "New Device: ID %lu, MAC %d\n", device_id, frame.src);
                } xSemaphoreGive(cache_mutex);
            }
        } return;
    }
    if (frame.dest != sysCfg.mac_address) return;
    if (type == 0x20) { // Simple-ACK
        z_log(LOG_DEBUG, "BACNET", "Simple-ACK from MAC %d (Success)\n", frame.src);
        if (xSemaphoreTake(cache_mutex, 0)) {
            for(auto& d : bacnet_network_cache) if (d.mac_address == frame.src) {
                for(auto& o : d.objects) if (o.type == pending_write_job.obj_type && o.instance == pending_write_job.obj_instance) {
                    if (pending_write_job.prop_id == 85) { o.present_value = pending_write_job.write_value; publish_mqtt_topic(d.device_id, o, 85, false); }
                    else if (pending_write_job.prop_id == 77) publish_mqtt_topic(d.device_id, o, 77, true);
                    o.last_update = millis(); break;
                } break;
            } xSemaphoreGive(cache_mutex);
        } current_dev_idx = (current_dev_idx + 1) % bacnet_network_cache.size();
    } else if (type == 0x30) { // Complex-ACK
        uint16_t ap = 3; BACnetTag t; uint8_t pid = 0xFF;
        z_log(LOG_DEBUG, "BACNET", "Complex-ACK from MAC %d (Invoke %d)\n", frame.src, apdu[1]);
        while (ap < al && decode_next_tag(apdu, &ap, al, &t)) {
            if (t.number == 1) { pid = 0; for(int i=0; i<t.len; i++) pid = (pid<<8)|apdu[ap+i]; ap += t.len; }
            else if (t.isOpening && t.number == 3) {
                if (xSemaphoreTake(cache_mutex, 0)) {
                    if (current_dev_idx < bacnet_network_cache.size()) {
                        auto& dev = bacnet_network_cache[current_dev_idx]; BACnetTag vt;
                        while (ap < al && decode_next_tag(apdu, &ap, al, &vt)) {
                            if (vt.isClosing && vt.number == 3) break;
                            if (!dev.discovery_done) {
                                if (dev.disc_step == DISC_DEV_ID) { dev.device_id = (((apdu[ap]<<24)|(apdu[ap+1]<<16)|(apdu[ap+2]<<8)|apdu[ap+3]) & 0x3FFFFF); z_log(LOG_INFO, "BACNET", "Device ID: %lu\n", (unsigned long)dev.device_id); dev.disc_step = DISC_DEV_NAME; }
                                else if (dev.disc_step == DISC_DEV_NAME) { char n[33]; uint8_t enc = apdu[ap]; int sl=0; if(enc==0){sl=std::min((int)vt.len-1,32); memcpy(n,&apdu[ap+1],sl);} else {for(int i=2;i<vt.len&&sl<32;i+=2) n[sl++]=apdu[ap+i];} n[sl]=0; dev.name=String(n); z_log(LOG_INFO, "BACNET", "Device Name: %s\n", n); dev.disc_step=DISC_DEV_VENDOR; }
                                else if (dev.disc_step == DISC_DEV_VENDOR) { char n[33]; uint8_t enc = apdu[ap]; int sl=0; if(enc==0){sl=std::min((int)vt.len-1,32); memcpy(n,&apdu[ap+1],sl);} else {for(int i=2;i<vt.len&&sl<32;i+=2) n[sl++]=apdu[ap+i];} n[sl]=0; dev.vendor=String(n); z_log(LOG_INFO, "BACNET", "Device Vendor: %s\n", n); dev.disc_step=DISC_OBJ_COUNT; }
                                else if (dev.disc_step == DISC_OBJ_COUNT) { 
                                    uint32_t c=0; for(int i=0;i<vt.len;i++) c=(c<<8)|apdu[ap+i]; 
                                    z_log(LOG_INFO, "BACNET", "Device Object Count: %lu\n", (unsigned long)c); 
                                    dev.objects.clear(); 
                                    dev.objects.reserve(c); 
                                    for(int i=0;i<c;i++){ BACnetObject o; o.type=65535; dev.objects.push_back(o); } 
                                    dev.disc_obj_idx=0; 
                                    dev.disc_step=DISC_OBJ_OID; 
                                    
                                    // v6.3.8: Point d'arrêt passif SI désactivé
                                    if (!dev.enabled) {
                                        z_log(LOG_INFO, "BACNET", "Passive Discovery: MAC %d paused after Phase 1. Waiting for manual ON.\n", dev.mac_address);
                                        dev.discovery_done = true;
                                        save_device_objects_locked(dev.device_id);
                                        xSemaphoreGive(cache_mutex);
                                        return; 
                                    }
                                }
                                else if (dev.disc_obj_idx < dev.objects.size()) {
                                    auto& o = dev.objects[dev.disc_obj_idx];
                                    if (dev.disc_step == DISC_OBJ_OID) { uint32_t oid = (apdu[ap]<<24)|(apdu[ap+1]<<16)|(apdu[ap+2]<<8)|apdu[ap+3]; o.type=oid>>22; o.instance=oid&0x3FFFFF; z_log(LOG_INFO, "BACNET", "Obj %u OID: T%u I%lu\n", dev.disc_obj_idx+1, o.type, (unsigned long)o.instance); dev.disc_step=DISC_OBJ_NAME; }
                                    else if (dev.disc_step == DISC_OBJ_NAME) { 
                                        char n[33]; uint8_t enc=apdu[ap]; int sl=0; if(enc==0){sl=std::min((int)vt.len-1,32); memcpy(n,&apdu[ap+1],sl);} else {for(int i=2;i<vt.len&&sl<32;i+=2) n[sl++]=apdu[ap+i];} n[sl]=0; 
                                        String ns = String(n); ns.trim();
                                        if (ns.length() == 0 || ns.equalsIgnoreCase("Unknown") || ns.equalsIgnoreCase("Untitled")) {
                                            const char* ts = (o.type == 0) ? "AI" : (o.type == 1) ? "AO" : (o.type == 2) ? "AV" : 
                                                            (o.type == 3) ? "BI" : (o.type == 4) ? "BO" : (o.type == 5) ? "BV" : 
                                                            (o.type == 13) ? "MSI" : (o.type == 14) ? "MSO" : (o.type == 19) ? "MSV" : "OBJ";
                                            snprintf(o.name, sizeof(o.name), "%s-%lu", ts, (unsigned long)o.instance);
                                            z_log(LOG_INFO, "BACNET", "Obj %u: Using Fallback Name %s\n", dev.disc_obj_idx+1, o.name);
                                        } else { strlcpy(o.name, n, sizeof(o.name)); }
                                        z_log(LOG_INFO, "BACNET", "Obj %u Name: %s\n", dev.disc_obj_idx+1, o.name);
                                        
                                        if(o.type<=2||o.type==23||o.type==24||o.type==46) dev.disc_step=DISC_OBJ_UNITS; 
                                        else if(o.type==13||o.type==14||o.type==19){ o.state_texts.clear(); o.expected_states_count=0; dev.disc_step=DISC_OBJ_STATES; } 
                                        else dev.disc_step=DISC_OBJ_COMMANDABLE; 
                                    }
                                    else if (dev.disc_step == DISC_OBJ_UNITS) { 
                                        uint32_t u=0; for(int i=0;i<vt.len;i++) u=(u<<8)|apdu[ap+i]; 
                                        o.units=u; String us=get_unit_text(u); strlcpy(o.unit_text,us.c_str(),sizeof(o.unit_text)); 
                                        z_log(LOG_INFO, "BACNET", "Obj %u Units: %s\n", dev.disc_obj_idx+1, o.unit_text); 
                                        dev.disc_step = DISC_OBJ_MIN; 
                                    }
                                    else if (dev.disc_step == DISC_OBJ_MIN) {
                                        if (vt.number == 4) { // REAL
                                            float v; uint32_t tm; memcpy(&tm, &apdu[ap], 4); tm = __builtin_bswap32(tm); memcpy(&v, &tm, 4);
                                            o.min_value = v; z_log(LOG_INFO, "BACNET", "Obj %u Min: %.2f\n", dev.disc_obj_idx+1, v);
                                        }
                                        dev.disc_step = DISC_OBJ_MAX;
                                    }
                                    else if (dev.disc_step == DISC_OBJ_MAX) {
                                        if (vt.number == 4) { // REAL
                                            float v; uint32_t tm; memcpy(&tm, &apdu[ap], 4); tm = __builtin_bswap32(tm); memcpy(&v, &tm, 4);
                                            o.max_value = v; z_log(LOG_INFO, "BACNET", "Obj %u Max: %.2f\n", dev.disc_obj_idx+1, v);
                                        }
                                        if(o.type==13||o.type==14||o.type==19){ o.state_texts.clear(); o.expected_states_count=0; dev.disc_step=DISC_OBJ_STATES; } 
                                        else dev.disc_step=DISC_OBJ_COMMANDABLE;
                                    }
                                    else if (dev.disc_step == DISC_OBJ_STATES) { 
                                        if(vt.number==2){ uint32_t c=0; for(int i=0;i<vt.len;i++) c=(c<<8)|apdu[ap+i]; o.expected_states_count=c; z_log(LOG_INFO, "BACNET", "Obj %u State Count: %lu\n", dev.disc_obj_idx+1, (unsigned long)c); if(c==0) dev.disc_step=DISC_OBJ_VALUE; } 
                                        else if(vt.number==7){ char n[33]; uint8_t enc=apdu[ap]; int sl=0; if(enc==0){sl=std::min((int)vt.len-1,32); memcpy(n,&apdu[ap+1],sl);} else {for(int i=2;i<vt.len&&sl<32;i+=2) n[sl++]=apdu[ap+i];} n[sl]=0; o.state_texts.push_back(String(n)); z_log(LOG_INFO, "BACNET", "Obj %u State %zu: %s\n", dev.disc_obj_idx+1, o.state_texts.size(), n); if(o.state_texts.size()>=o.expected_states_count) dev.disc_step=DISC_OBJ_COMMANDABLE; } 
                                    }
                                    else if (dev.disc_step == DISC_OBJ_COMMANDABLE) { o.is_commandable=true; z_log(LOG_DEBUG, "BACNET", "Obj %u is commandable\n", dev.disc_obj_idx+1); if(o.enabled||dev.reload_single) dev.disc_step=DISC_OBJ_VALUE; else { if(!dev.reload_single) dev.disc_obj_idx++; dev.disc_step=DISC_OBJ_OID; if(dev.reload_single){dev.discovery_done=true; dev.reload_single=false;} } ap=al; break; }
                                    else if (dev.disc_step == DISC_OBJ_VALUE) { 
                                        if(vt.number==4){ float v; uint32_t tm; memcpy(&tm,&apdu[ap],4); tm=__builtin_bswap32(tm); memcpy(&v,&tm,4); o.present_value=v; } 
                                        else { uint32_t v=0; for(int i=0;i<vt.len;i++) v=(v<<8)|apdu[ap+i]; o.present_value=v; } 
                                        o.last_update=millis(); z_log(LOG_INFO, "BACNET", "Obj %u Value: %.2f\n", dev.disc_obj_idx+1, o.present_value);
                                        bool stop=dev.reload_single||dev.recovery_mode; 
                                        if(!stop) dev.disc_obj_idx++; 
                                        dev.disc_step=DISC_OBJ_OID; 
                                        
                                        if(dev.disc_obj_idx%50==0||dev.disc_obj_idx>=dev.objects.size()) save_device_objects_locked(dev.device_id); 
                                        
                                        if(stop){
                                            dev.discovery_done=true; dev.reload_single=false; dev.recovery_mode=false; 
                                            save_device_objects_locked(dev.device_id); 
                                            if(o.enabled) trigger_ha_discovery(dev.device_id, o.instance, o.type);
                                        } else if(dev.disc_obj_idx>=dev.objects.size()){
                                            dev.discovery_done=true; 
                                            save_device_objects_locked(dev.device_id); 
                                            publish_all_names();
                                            trigger_ha_discovery(dev.device_id, 0xFFFFFFFF, 0xFFFF); 
                                        } 
                                    }
                                }
                            } else {
                                if(vt.number==4){ float v; uint32_t tm; memcpy(&tm,&apdu[ap],4); tm=__builtin_bswap32(tm); memcpy(&v,&tm,4); if(current_poll_idx<dev.objects.size()){ auto& o=dev.objects[current_poll_idx]; o.present_value=v; o.last_update=millis(); if(o.enabled) publish_mqtt_topic(dev.device_id, o, 85, false); } }
                                else if(vt.number==2||vt.number==9){ uint32_t v=0; for(int i=0;i<vt.len;i++) v=(v<<8)|apdu[ap+i]; if(current_poll_idx<dev.objects.size()){ auto& o=dev.objects[current_poll_idx]; o.present_value=v; o.last_update=millis(); if(o.enabled) publish_mqtt_topic(dev.device_id, o, 85, false); } }
                            } ap += vt.len;
                        }
                    } xSemaphoreGive(cache_mutex);
                }
            } else ap += t.len;
        } current_dev_idx = (current_dev_idx + 1) % bacnet_network_cache.size();
    } else if (type >= 0x40) {
        if (sysCfg.log_level >= LOG_DEBUG) {
            z_log(LOG_DEBUG, "BACNET", "Received PDU type 0x%02X (Error/Abort/Reject)\n", type);
        }
        if (current_dev_idx < bacnet_network_cache.size()) {

            auto& dev = bacnet_network_cache[current_dev_idx];
            if (!dev.discovery_done && dev.disc_obj_idx < dev.objects.size()) {
                auto& o = dev.objects[dev.disc_obj_idx];
                if (dev.disc_step == DISC_OBJ_MIN) { dev.disc_step = DISC_OBJ_MAX; }
                else if (dev.disc_step == DISC_OBJ_MAX) {
                    if(o.type==13||o.type==14||o.type==19){ o.state_texts.clear(); o.expected_states_count=0; dev.disc_step=DISC_OBJ_STATES; } 
                    else dev.disc_step=DISC_OBJ_COMMANDABLE;
                }
                else if (dev.disc_step == DISC_OBJ_COMMANDABLE) { 
                    o.is_commandable = (o.type==13||o.type==14||o.type==19||o.type<=5);
                    if(o.enabled||dev.reload_single) dev.disc_step=DISC_OBJ_VALUE; else { if(!dev.reload_single) dev.disc_obj_idx++; dev.disc_step=DISC_OBJ_OID; if(dev.reload_single){dev.discovery_done=true; dev.reload_single=false;} }
                } else { if (retry_count < sysCfg.max_retries) { retry_count++; send_mstp_frame(dev.mac_address, 0x05, last_apdu, last_apdu_len); waiting_for_reply = true; } else { retry_count = 0; if(!dev.discovery_done){dev.disc_obj_idx++; dev.disc_step=DISC_OBJ_OID;} current_dev_idx = (current_dev_idx + 1) % bacnet_network_cache.size(); } }
            } else { if (retry_count < sysCfg.max_retries) { retry_count++; send_mstp_frame(dev.mac_address, 0x05, last_apdu, last_apdu_len); waiting_for_reply = true; } else { retry_count = 0; if(!dev.discovery_done){dev.disc_obj_idx++; dev.disc_step=DISC_OBJ_OID;} current_dev_idx = (current_dev_idx + 1) % bacnet_network_cache.size(); } }
        }
    }
}

static void bacnet_task(void *pv) {
    MSTP_Frame frame;
    z_log(LOG_INFO,"BACNET","Master FSM Task Started (Priority 15)\n");

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
                        for(auto& o : d.objects) if(o.enabled) enabled_count++;
                    }
                }
                xSemaphoreGive(cache_mutex);
            }
            z_log(LOG_INFO,"BACNET","Heartbeat - Tokens:%lu, RX:%lu, TX:%lu (State:%d, Cache:%u, Enabled:%lu)\n", 
                  bacnetStats.tokens_seen, bacnetStats.ms_msgs_rx, bacnetStats.ms_msgs_tx, (int)mstp_state, cache_size, enabled_count);
            z_log(LOG_INFO,"BACNET","Polling - objects polled : %lu\n", (unsigned long)period_poll_count);
            period_poll_count = 0;
            heartbeat_timer = millis();
        }

        static uint32_t recovery_timer = 0;
        if (millis() - recovery_timer > 60000) { // Check every 60s
            if (xSemaphoreTake(cache_mutex, 0)) {
                for (auto& dev : bacnet_network_cache) {
                    if (dev.discovery_done) {
                        for (size_t i = 0; i < dev.objects.size(); i++) {
                            auto& o = dev.objects[i];
                            if ((o.type == 13 || o.type == 14 || o.type == 19) && o.enabled && o.state_texts.empty()) {
                                dev.discovery_done = false;
                                dev.disc_obj_idx = i;
                                dev.disc_step = DISC_OBJ_STATES;
                                dev.recovery_mode = true;
                                z_log(LOG_INFO, "BACNET", "Recovery: Fetching missing states for Obj %zu (MAC %d)\n", i+1, dev.mac_address);
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
                next_station = (sysCfg.mac_address + 1) % 128;
                poll_station = (sysCfg.mac_address + 1) % 128;
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
    
    z_log(LOG_INFO,"BACNET","BACnet Multi-Task Engine Initialized\n");
}

bool enqueue_bacnet_job(BACnetJob job) { if (bacnet_job_queue == NULL) return false; return xQueueSend(bacnet_job_queue, &job, 0) == pdTRUE; }

void publish_all_names() {
    for (auto& dev : bacnet_network_cache) {
        for (auto& obj : dev.objects) {
            if (obj.type == 65535 || obj.enabled == false) continue;
            if (!obj.name_published || (strcmp(obj.name, obj.last_mqtt_name) != 0)) {
                publish_mqtt_topic(dev.device_id, obj, 77, true);
                obj.name_published = true; strlcpy(obj.last_mqtt_name, obj.name, sizeof(obj.last_mqtt_name));
            }
        }
    }
}
