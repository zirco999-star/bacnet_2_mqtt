#include "z_bacnet.h"
#include <string.h>
#include <algorithm>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "z_network.h" 
#include "rom/ets_sys.h"
#include "z_mqtt.h"

extern void z_log(int level, const char* tag, const char* format, ...);
extern void save_device_objects(uint32_t device_id);
extern void save_device_objects_locked(uint32_t device_id);

BACnet_Stats bacnetStats = {0, 0, 0, 0, 0, 0, 0};
std::vector<BACnetDevice> bacnet_network_cache;
QueueHandle_t bacnet_job_queue = NULL;
QueueHandle_t mqtt_publish_queue = NULL;
QueueHandle_t uart_evt_queue = NULL;
SemaphoreHandle_t cache_mutex = NULL;

static uint8_t last_apdu[512];
static uint16_t last_apdu_len = 0;
static uint8_t last_sent_invoke_id = 0;
static uint8_t next_invoke_id = 10;
static uint8_t retry_count = 0;

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
        crc &= 0xFFFF;
    }
    return (~crc) & 0xFFFF;
}

static bool validate_rx_header_crc(const uint8_t *header_and_crc) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < 6; i++) { 
        crc ^= header_and_crc[i];
        uint16_t crc16 = crc ^ (crc << 1) ^ (crc << 2) ^ (crc << 3) ^ (crc << 4) ^ (crc << 5) ^ (crc << 6) ^ (crc << 7);
        crc = (crc16 & 0xfe) ^ ((crc16 >> 8) & 1);
    }
    return (crc == 0x55);
}

static bool validate_rx_data_crc(const uint8_t *data_and_crc, size_t total_len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < total_len; i++) {
        uint8_t crc_low = (crc & 0xff) ^ data_and_crc[i];
        crc = (crc >> 8) ^ (crc_low << 8) ^ (crc_low << 3) ^ (crc_low << 12) ^ (crc_low >> 4) ^ (crc_low & 0x0f) ^ ((crc_low & 0x0f) << 7);
        crc &= 0xFFFF;
    }
    return (crc == 0xF0B8);
}

struct BACnetTag { uint32_t number; bool isContext; uint32_t len; bool isOpening; bool isClosing; uint8_t tag_raw; };
static bool decode_next_tag(const uint8_t *data, uint16_t *pos, uint16_t max_len, BACnetTag *tag) {
    if (*pos >= max_len) return false;
    uint8_t b = data[(*pos)++];
    tag->tag_raw = b; tag->number = b >> 4; tag->isContext = (b & 0x08) != 0;
    uint8_t lvt = b & 0x07;
    if (tag->number == 0x0F) tag->number = data[(*pos)++];
    tag->isOpening = (lvt == 6); tag->isClosing = (lvt == 7);
    if (lvt <= 4) tag->len = lvt;
    else if (lvt == 5) {
        tag->len = data[(*pos)++];
        if (tag->len == 254) { tag->len = (data[*pos] << 8) | data[*pos+1]; *pos += 2; }
        else if (tag->len == 255) { tag->len = ((uint32_t)data[*pos] << 24) | ((uint32_t)data[*pos+1] << 16) | ((uint32_t)data[*pos+2] << 8) | data[*pos+3]; *pos += 4; }
    } else tag->len = 0;
    return true;
}

void bacnet_abort_current_transaction() { last_sent_invoke_id = 0xFF; z_log(LOG_WARN, "BACNET", "Transaction Aborted\n"); }
static void uart_tx(const uint8_t *buffer, uint16_t length) { uart_write_bytes(RS485_UART_PORT, (const char*)buffer, length); bacnetStats.ms_msgs_tx++; }

static void send_mstp_frame(uint8_t target_mac, uint8_t type, const uint8_t* apdu, uint16_t len) {
    if (apdu != NULL && len > 0) {
        memcpy(last_apdu, apdu, len); last_apdu_len = len;
        if (len > 4) last_sent_invoke_id = apdu[4]; else last_sent_invoke_id = 0xFF; 
    }
    uint8_t buffer[512+10];
    buffer[0]=0x55; buffer[1]=0xFF; buffer[2]=type; buffer[3]=target_mac; buffer[4]=sysCfg.mac_address;
    buffer[5]=(len>>8)&0xFF; buffer[6]=len&0xFF; buffer[7]=calc_header_crc(&buffer[2], 5);
    if (len > 0) {
        memcpy(&buffer[8], apdu, len); uint16_t crc16 = calc_data_crc(&buffer[8], len);
        buffer[8+len]=crc16&0xFF; buffer[8+len+1]=(crc16>>8)&0xFF; uart_tx(buffer, 8+len+2);
    } else { uart_tx(buffer, 8); }
}

uint16_t build_read_property_apdu(uint8_t* buffer, uint8_t invoke_id, uint16_t obj_type, uint32_t obj_instance, uint8_t property_id, int32_t array_index) {
    uint16_t len = 0; buffer[len++] = 0x01; buffer[len++] = 0x04; 
    buffer[len++] = 0x02; buffer[len++] = 0x73; buffer[len++] = invoke_id; buffer[len++] = 0x0C; 
    uint32_t oid = ((uint32_t)obj_type << 22) | (obj_instance & 0x3FFFFF);
    buffer[len++] = 0x0C; 
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
    uint16_t len = 0; buffer[len++] = 0x01; buffer[len++] = 0x04; 
    buffer[len++] = 0x02; buffer[len++] = 0x03; buffer[len++] = invoke_id; buffer[len++] = 0x0F; 
    uint32_t oid = ((uint32_t)obj_type << 22) | (obj_instance & 0x3FFFFF);
    buffer[len++] = 0x0C; 
    buffer[len++] = (oid >> 24) & 0xFF; buffer[len++] = (oid >> 16) & 0xFF; buffer[len++] = (oid >> 8) & 0xFF; buffer[len++] = oid & 0xFF;
    buffer[len++] = 0x19; buffer[len++] = 0x4D; buffer[len++] = 0x3E; 
    uint32_t str_len = strlen(new_name); uint32_t payload_len = str_len + 1;
    if (payload_len <= 4) buffer[len++] = 0x70 | (uint8_t)payload_len;
    else { buffer[len++] = 0x75; buffer[len++] = (uint8_t)payload_len; }
    buffer[len++] = 0x00; memcpy(&buffer[len], new_name, str_len); len += str_len; buffer[len++] = 0x3F;
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

static void bacnet_task(void *pv) {
    uint8_t rx_byte; uint8_t header[6]; uint8_t header_idx=0; uint8_t data_buf[512+2]; uint16_t data_len=0, data_idx=0;
    enum RX_STATE { RX_IDLE, RX_PREAMBLE_55, RX_HEADER, RX_DATA, RX_CRC16_L, RX_CRC16_H };
    RX_STATE rx_state = RX_IDLE; MSTP_MASTER_STATE mstp_state = MSTP_INITIALIZE;
    static unsigned frame_count = 0; uint32_t last_rx_time = millis(); uint32_t timer_silence = millis();
    uint8_t token_skip_count = 0; bool waiting_for_reply = false, ReceivedValidFrame = false;
    uint8_t frame_type = 0, dest_mac = 0, src_mac = 0, target_mac = 0;
    uint8_t current_poll_idx = 0, current_dev_idx = 0; uint32_t heartbeat_timer = 0;
    static uint32_t last_who_is_time = 0; static uint8_t next_station;
    uart_event_t event;

    z_log(LOG_INFO, "BACNET", "Engine %s - TSM Enabled (v5.9.6 Final)\n", VERSION_GLOBAL);

    for (;;) {
        if (xQueueReceive(uart_evt_queue, (void *)&event, 0) == pdTRUE) { if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) uart_flush_input(RS485_UART_PORT); }
        uint32_t who_is_interval = (bacnet_network_cache.empty()) ? 10000 : 300000;
        if (millis() - last_who_is_time > who_is_interval) { BACnetJob job; job.type = JOB_WHO_IS; job.target_mac = 0xFF; enqueue_bacnet_job(job); last_who_is_time = millis(); }
        if (millis() - heartbeat_timer > sysCfg.heartbeat_interval) { z_log(LOG_INFO, "BACNET", "Heartbeat - Tokens: %lu, RX: %lu, TX: %lu\n", bacnetStats.tokens_seen, bacnetStats.ms_msgs_rx, bacnetStats.ms_msgs_tx); heartbeat_timer = millis(); }

        ReceivedValidFrame = false;
        while (uart_read_bytes(RS485_UART_PORT, &rx_byte, 1, 0) > 0) {
            last_rx_time = millis();
            switch (rx_state) {
                case RX_IDLE: if (rx_byte == 0x55) rx_state = RX_PREAMBLE_55; break;
                case RX_PREAMBLE_55: if (rx_byte == 0xFF) { rx_state = RX_HEADER; header_idx = 0; } else rx_state = RX_IDLE; break;
                case RX_HEADER: header[header_idx++] = rx_byte;
                    if (header_idx == 6) {
                        if (validate_rx_header_crc(header)) {
                            bacnetStats.ms_msgs_rx++; frame_type = header[0]; dest_mac = header[1]; src_mac = header[2]; data_len = (header[3] << 8) | header[4];
                            if (src_mac < 128 && src_mac != sysCfg.mac_address) {
                                if (xSemaphoreTake(cache_mutex, 0)) {
                                    bool known = false; for(auto& d : bacnet_network_cache) if(d.mac_address == src_mac) known = true;
                                    if(!known) { z_log(LOG_INFO, "BACNET", "Found MAC %u\n", src_mac); BACnetDevice d; d.mac_address = src_mac; d.device_id = 4194303; d.enabled = true; d.discovery_done = false; d.disc_step = DISC_DEV_ID; d.last_seen = millis(); d.total_slots = 0; d.debug_oid = 0; bacnet_network_cache.push_back(d); }
                                    xSemaphoreGive(cache_mutex);
                                }
                            }
                            if (data_len > 0 && data_len <= 512) { rx_state = RX_DATA; data_idx = 0; } else { ReceivedValidFrame = true; rx_state = RX_IDLE; }
                        } else { bacnetStats.errors_crc++; rx_state = RX_IDLE; }
                    } break;
                case RX_DATA: data_buf[data_idx++] = rx_byte; if (data_idx == data_len) rx_state = RX_CRC16_L; break;
                case RX_CRC16_L: data_buf[data_len] = rx_byte; rx_state = RX_CRC16_H; break;
                case RX_CRC16_H: data_buf[data_len+1] = rx_byte; if (validate_rx_data_crc(data_buf, data_len + 2)) ReceivedValidFrame = true; else bacnetStats.errors_crc++; rx_state = RX_IDLE; break;
            }
        }

        uint32_t Tno_token = 500 + (sysCfg.mac_address * 10);
        switch (mstp_state) {
            case MSTP_INITIALIZE: token_skip_count = 0; next_station = sysCfg.mac_address; mstp_state = MSTP_IDLE; break;
            case MSTP_IDLE:
                if (ReceivedValidFrame) {
                    if (frame_type == 0x00 && dest_mac == sysCfg.mac_address) { bacnetStats.tokens_seen++; next_station = (src_mac + 1) % (sysCfg.max_master + 1); frame_count = 0; mstp_state = MSTP_USE_TOKEN; }
                    else if (frame_type == 0x01 && dest_mac == sysCfg.mac_address) { uint8_t f[8] = { 0x55, 0xFF, 0x02, src_mac, sysCfg.mac_address, 0, 0, 0 }; f[7] = calc_header_crc(&f[2], 5); uart_tx(f, 8); mstp_state = MSTP_IDLE; }
                    else if (dest_mac == sysCfg.mac_address && frame_type >= 0x05) mstp_state = MSTP_ANSWER_DATA_REQUEST;
                } else if (millis() - last_rx_time > Tno_token) mstp_state = MSTP_NO_TOKEN;
                break;
            case MSTP_NO_TOKEN: mstp_state = MSTP_POLL_FOR_MASTER; timer_silence = millis(); break;
            case MSTP_POLL_FOR_MASTER: if (millis() - timer_silence > 50) { last_rx_time = millis(); uint8_t f[8]={0x55,0xFF,0x01, (uint8_t)((sysCfg.mac_address+1)%128), sysCfg.mac_address,0,0,0}; f[7]=calc_header_crc(&f[2],5); uart_tx(f,8); mstp_state = MSTP_IDLE; } break;
            case MSTP_USE_TOKEN:
                token_skip_count++;
                if (token_skip_count >= sysCfg.token_skip) { 
                    BACnetJob current_job; bool has_job = (xQueueReceive(bacnet_job_queue, &current_job, 0) == pdTRUE);
                    uint8_t apdu[256]; uint16_t apdu_len = 0;
                    if (has_job && current_job.type == JOB_WHO_IS) {
                        uint8_t buffer[16]; uint16_t len = 0;
                        buffer[len++] = 0x01; buffer[len++] = 0x20; buffer[len++] = 0xFF; buffer[len++] = 0xFF; buffer[len++] = 0x00; buffer[len++] = 0xFF;
                        buffer[len++] = 0x10; buffer[len++] = 0x08;
                        send_mstp_frame(0xFF, 0x06, buffer, len);
                        timer_silence = millis(); token_skip_count = 0; frame_count++; mstp_state = MSTP_DONE_WITH_TOKEN;
                    } else if (has_job && current_job.type == JOB_WRITE_PROP) {
                        apdu_len = build_write_property_name_apdu(apdu, next_invoke_id++, current_job.obj_type, current_job.obj_instance, current_job.name);
                        token_skip_count = 0; target_mac = current_job.target_mac; waiting_for_reply = true; 
                        send_mstp_frame(target_mac, 0x05, apdu, apdu_len);
                        timer_silence = millis(); frame_count++; mstp_state = MSTP_WAIT_FOR_REPLY;
                    } else {
                        if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(50))) {
                            if (!bacnet_network_cache.empty()) {
                                if (current_dev_idx >= bacnet_network_cache.size()) current_dev_idx = 0;
                                auto& dev = bacnet_network_cache[current_dev_idx]; target_mac = dev.mac_address;
                                if (dev.enabled) {
                                    if (!dev.discovery_done) {
                                        if (dev.disc_step == DISC_DEV_ID) apdu_len = build_read_property_apdu(apdu, next_invoke_id++, 8, 4194303, 75, -1);
                                        else if (dev.disc_step == DISC_DEV_NAME) apdu_len = build_read_property_apdu(apdu, next_invoke_id++, 8, dev.device_id, 77, -1);
                                        else if (dev.disc_step == DISC_DEV_VENDOR) apdu_len = build_read_property_apdu(apdu, next_invoke_id++, 8, dev.device_id, 121, -1);
                                        else if (dev.disc_step == DISC_OBJ_COUNT) apdu_len = build_read_property_apdu(apdu, next_invoke_id++, 8, dev.device_id, 76, 0);
                                        else if (dev.disc_obj_idx < dev.total_slots) {
                                            if (dev.disc_step == DISC_OBJ_OID) apdu_len = build_read_property_apdu(apdu, next_invoke_id++, 8, dev.device_id, 76, dev.disc_obj_idx + 1);
                                            else if (!dev.objects.empty()) {
                                                auto& o = dev.objects.back();
                                                if (dev.disc_step == DISC_OBJ_NAME) apdu_len = build_read_property_apdu(apdu, next_invoke_id++, o.type, o.instance, 77, -1);
                                                else if (dev.disc_step == DISC_OBJ_UNITS) apdu_len = build_read_property_apdu(apdu, next_invoke_id++, o.type, o.instance, 117, -1);
                                                else if (dev.disc_step == DISC_OBJ_VALUE) apdu_len = build_read_property_apdu(apdu, next_invoke_id++, o.type, o.instance, 85, -1);
                                            } else { dev.disc_obj_idx++; dev.disc_step = DISC_OBJ_OID; }
                                        } else { dev.discovery_done = true; save_device_objects_locked(dev.device_id); publish_all_names(); }
                                    } else {
                                        size_t count = dev.objects.size();
                                        if (count > 0) {
                                            for (size_t i = 0; i < count; i++) {
                                                current_poll_idx = (current_poll_idx + 1) % count; auto& o = dev.objects[current_poll_idx];
                                                if (current_poll_idx == 0) current_dev_idx = (current_dev_idx + 1) % bacnet_network_cache.size();
                                                if (o.enabled && o.type != 8) {
                                                    if (o.last_update == 0 || (millis() - o.last_update > (sysCfg.bacnet_poll_interval * 1000))) { 
                                                        apdu_len = build_read_property_apdu(apdu, next_invoke_id++, o.type, o.instance, 85, -1); break; 
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            xSemaphoreGive(cache_mutex);
                        }
                        if (apdu_len > 0) { 
                            token_skip_count = 0; retry_count=0; waiting_for_reply = true; send_mstp_frame(target_mac, 0x05, apdu, apdu_len); 
                            timer_silence = millis(); frame_count++; mstp_state = MSTP_WAIT_FOR_REPLY; 
                        } else mstp_state = MSTP_PASS_TOKEN; 
                    }
                } else mstp_state = MSTP_PASS_TOKEN; 
                break;
            case MSTP_WAIT_FOR_REPLY:
                if (ReceivedValidFrame) {
                    waiting_for_reply = false;
                    if (frame_type == 0x07) { z_log(LOG_INFO, "BACNET", "Reply Postponed from %u\n", src_mac); mstp_state = MSTP_DONE_WITH_TOKEN; break; }
                    if (data_len >= 2 && data_buf[0] == 0x01) {
                        uint16_t pos = 2;
                        if (pos < data_len) {
                            uint8_t *apdu = &data_buf[pos]; uint16_t apdu_len = data_len - pos;
                            uint8_t pdu_type = apdu[0] & 0xF0; uint8_t invoke_id = apdu[1];
                            if (dest_mac == sysCfg.mac_address && pdu_type == 0x30 && invoke_id == last_sent_invoke_id) {
                                uint16_t apdu_pos = 3; BACnetTag t; uint8_t last_prop_id = 0xFF;
                                while (apdu_pos < apdu_len && decode_next_tag(apdu, &apdu_pos, apdu_len, &t)) {
                                    if (t.number == 1) { last_prop_id = 0; for(int i=0; i<t.len; i++) last_prop_id = (last_prop_id << 8) | apdu[apdu_pos+i]; apdu_pos += t.len; }
                                    else if (t.isOpening && t.number == 3) {
                                        if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(50))) {
                                            if (current_dev_idx < bacnet_network_cache.size()) {
                                                auto& dev = bacnet_network_cache[current_dev_idx]; BACnetTag val_tag;
                                                while (apdu_pos < apdu_len && decode_next_tag(apdu, &apdu_pos, apdu_len, &val_tag)) {
                                                    if (val_tag.isClosing && val_tag.number == 3) break; 
                                                    if (!dev.discovery_done) {
                                                        if (dev.disc_step == DISC_DEV_ID) { dev.device_id = (((apdu[apdu_pos]<<24)|(apdu[apdu_pos+1]<<16)|(apdu[apdu_pos+2]<<8)|apdu[apdu_pos+3]) & 0x3FFFFF); dev.disc_step = DISC_DEV_NAME; }
                                                        else if (dev.disc_step == DISC_DEV_NAME) { char n[33]; uint8_t enc = apdu[apdu_pos]; if (enc == 0) { uint16_t slen = std::min((int)val_tag.len - 1, 32); memcpy(n, &apdu[apdu_pos+1], slen); n[slen]=0; } else { snprintf(n,32,"Dev:%lu",(unsigned long)dev.device_id); } dev.name = String(n); dev.disc_step = DISC_DEV_VENDOR; }
                                                        else if (dev.disc_step == DISC_DEV_VENDOR) { char n[33]; uint8_t enc = apdu[apdu_pos]; if (enc == 0) { uint16_t slen = std::min((int)val_tag.len - 1, 32); memcpy(n, &apdu[apdu_pos+1], slen); n[slen]=0; } dev.vendor = String(n); dev.disc_step = DISC_OBJ_COUNT; }
                                                        else if (dev.disc_step == DISC_OBJ_COUNT) { 
                                                            uint32_t count = 0; for(int i=0; i<val_tag.len; i++) count = (count << 8) | apdu[apdu_pos+i]; 
                                                            if (count > 0 && count < 200) { dev.total_slots = count; dev.objects.clear(); dev.disc_obj_idx = 0; dev.disc_step = DISC_OBJ_OID; z_log(LOG_INFO, "BACNET", "Dev %lu: %lu slots\n", dev.device_id, count); } else dev.discovery_done = true;
                                                        }
                                                        else if (dev.disc_obj_idx < dev.total_slots) {
                                                            if (dev.disc_step == DISC_OBJ_OID) { 
                                                                uint32_t oid = 0xFFFFFFFF; if (val_tag.number == 12 && val_tag.len == 4) oid = ((uint32_t)apdu[apdu_pos]<<24)|((uint32_t)apdu[apdu_pos+1]<<16)|((uint32_t)apdu[apdu_pos+2]<<8)|(uint32_t)apdu[apdu_pos+3]; 
                                                                dev.debug_oid = oid;
                                                                if (oid != 0xFFFFFFFF && (oid >> 22) < 1023) {
                                                                    BACnetObject o; o.type = oid >> 22; o.instance = oid & 0x3FFFFF; 
                                                                    if (o.type <= 5 || (o.type >= 13 && o.type <= 19)) o.enabled = true;
                                                                    String ts = (o.type == 0) ? "AI" : (o.type == 1) ? "AO" : (o.type == 2) ? "AV" : (o.type == 3) ? "BI" : (o.type == 4) ? "BO" : (o.type == 5) ? "BV" : (o.type == 13) ? "MSI" : (o.type == 14) ? "MSO" : (o.type == 19) ? "MSV" : "OBJ";
                                                                    snprintf(o.name, 50, "%s:%lu", ts.c_str(), (unsigned long)o.instance);
                                                                    dev.objects.push_back(o); dev.disc_step = DISC_OBJ_NAME;
                                                                } else { dev.disc_obj_idx++; }
                                                            } else if (!dev.objects.empty()) {
                                                                auto& o = dev.objects.back();
                                                                if (dev.disc_step == DISC_OBJ_NAME) { char n[33]; uint8_t enc = apdu[apdu_pos]; if (enc == 0) { uint16_t slen = std::min((int)val_tag.len - 1, 32); memcpy(n, &apdu[apdu_pos+1], slen); n[slen]=0; if(strlen(n)>0) strlcpy(o.name, n, 50); } dev.disc_step = DISC_OBJ_UNITS; }
                                                                else if (dev.disc_step == DISC_OBJ_UNITS) { uint32_t u=0; for(int i=0; i<val_tag.len; i++) u = (u << 8) | apdu[apdu_pos+i]; o.units = u; strlcpy(o.unit_text, get_unit_text(u).c_str(), 20); dev.disc_step = DISC_OBJ_VALUE; }
                                                                else if (dev.disc_step == DISC_OBJ_VALUE) {
                                                                    if (val_tag.number == 4) { float v; uint32_t tmp; memcpy(&tmp, &apdu[apdu_pos], 4); tmp = __builtin_bswap32(tmp); memcpy(&v, &tmp, 4); o.present_value = v; } else { uint32_t v=0; for(int i=0; i<val_tag.len; i++) v = (v << 8) | apdu[apdu_pos+i]; o.present_value = (float)v; }
                                                                    o.last_update = millis(); 
                                                                    dev.disc_obj_idx++; if (dev.disc_obj_idx >= dev.total_slots) { dev.discovery_done = true; save_device_objects_locked(dev.device_id); publish_all_names(); } else dev.disc_step = DISC_OBJ_OID;
                                                                }
                                                            } else { dev.disc_obj_idx++; dev.disc_step = DISC_OBJ_OID; }
                                                        }
                                                    } else {
                                                        if (val_tag.number == 4) { float v; uint32_t tmp; memcpy(&tmp, &apdu[apdu_pos], 4); tmp = __builtin_bswap32(tmp); memcpy(&v, &tmp, 4); if(current_poll_idx < dev.objects.size()){ auto& o=dev.objects[current_poll_idx]; o.present_value=v; o.last_update=millis(); if (o.enabled) publish_mqtt_topic(dev.device_id, o, 85, false); } }
                                                        else if (val_tag.number == 2 || val_tag.number == 9) { uint32_t v=0; for(int i=0; i<val_tag.len; i++) v=(v<<8)|apdu[apdu_pos+i]; if(current_poll_idx < dev.objects.size()){ auto& o=dev.objects[current_poll_idx]; o.present_value=(float)v; o.last_update=millis(); if (o.enabled) publish_mqtt_topic(dev.device_id, o, 85, false); } }
                                                    }
                                                    apdu_pos += val_tag.len;
                                                }
                                            }
                                            xSemaphoreGive(cache_mutex);
                                        }
                                    } else apdu_pos += t.len;
                                }
                            } else if (dest_mac == sysCfg.mac_address && pdu_type == 0x50) {
                                uint8_t err_c = apdu[2], err_code = apdu[3];
                                z_log(LOG_WARN, "BACNET", "Error %u/%u ID %u\n", err_c, err_code, invoke_id);
                                if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(50))) { if (current_dev_idx < bacnet_network_cache.size()) { auto& d = bacnet_network_cache[current_dev_idx]; if (!d.discovery_done) { d.disc_obj_idx++; d.disc_step = DISC_OBJ_OID; if (d.disc_obj_idx >= d.total_slots) d.discovery_done = true; } } xSemaphoreGive(cache_mutex); }
                            } else if (dest_mac == sysCfg.mac_address) {
                                z_log(LOG_DEBUG, "BACNET", "Recv PDU 0x%02X ID %u\n", pdu_type, invoke_id);
                                if (pdu_type == 0x20) z_log(LOG_INFO, "BACNET", "SimpleACK ID %u\n", invoke_id);
                            }
                        }
                    }
                    mstp_state = MSTP_DONE_WITH_TOKEN;
                } else if (millis() - timer_silence > sysCfg.apdu_timeout) {
                    z_log(LOG_WARN, "BACNET", "Reply Timeout station %u\n", target_mac);
                    if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(50))) { if (current_dev_idx < bacnet_network_cache.size()) { auto& d = bacnet_network_cache[current_dev_idx]; if (!d.discovery_done) { retry_count++; if (retry_count > sysCfg.max_retries) { d.disc_obj_idx++; d.disc_step = DISC_OBJ_OID; retry_count = 0; if (d.disc_obj_idx >= d.total_slots) d.discovery_done = true; } } } xSemaphoreGive(cache_mutex); }
                    mstp_state = MSTP_DONE_WITH_TOKEN;
                }
                break;
            case MSTP_ANSWER_DATA_REQUEST: mstp_state = MSTP_IDLE; break;
            case MSTP_DONE_WITH_TOKEN: if (frame_count < sysCfg.max_info_frames) mstp_state = MSTP_USE_TOKEN; else mstp_state = MSTP_PASS_TOKEN; break;
            case MSTP_PASS_TOKEN: { uint8_t f[8] = { 0x55, 0xFF, 0x00, next_station, sysCfg.mac_address, 0, 0, 0 }; f[7] = calc_header_crc(&f[2], 5); uart_tx(f, 8); last_rx_time = millis(); mstp_state = MSTP_IDLE; break; }
        }
        if (mstp_state == MSTP_IDLE && !ReceivedValidFrame) vTaskDelay(1); else taskYIELD();
    }
}

void setup_bacnet_engine() {
    bacnet_job_queue = xQueueCreate(10, sizeof(BACnetJob)); mqtt_publish_queue = xQueueCreate(100, sizeof(MQTTPublishJob));
    const uart_config_t uc = { .baud_rate = 38400, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .rx_flow_ctrl_thresh = 122, .source_clk = UART_SCLK_APB };
    uart_driver_install(RS485_UART_PORT, 2048, 2048, 20, &uart_evt_queue, 0); uart_param_config(RS485_UART_PORT, &uc);
    uart_set_pin(RS485_UART_PORT, TX_PIN, RX_PIN, RTS_PIN, UART_PIN_NO_CHANGE); uart_set_mode(RS485_UART_PORT, UART_MODE_RS485_HALF_DUPLEX); uart_set_rx_timeout(RS485_UART_PORT, 2); 
    xTaskCreatePinnedToCore(bacnet_task, "BACnet", 16384, NULL, 15, NULL, 1);
}
bool enqueue_bacnet_job(BACnetJob job) { if (bacnet_job_queue == NULL) return false; return xQueueSend(bacnet_job_queue, &job, 0) == pdTRUE; }
bool enqueue_mqtt_publish(MQTTPublishJob pubJob) { if (mqtt_publish_queue == NULL) return false; return xQueueSend(mqtt_publish_queue, &pubJob, 0) == pdTRUE; }
void publish_all_names() { for (auto& dev : bacnet_network_cache) { for (auto& obj : dev.objects) { if (obj.type == 65535 || obj.enabled == false) continue; if (!obj.name_published || (strcmp(obj.name, obj.last_mqtt_name) != 0)) { publish_mqtt_topic(dev.device_id, obj, 77, true); obj.name_published = true; strlcpy(obj.last_mqtt_name, obj.name, 50); } } } }
