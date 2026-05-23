#include "z_bacnet.h"
#include <string.h>
#include <algorithm>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "z_network.h" 
#include "rom/ets_sys.h"

extern void z_log(const char* format, ...);
extern void save_device_objects(uint32_t device_id);

BACnet_Stats bacnetStats = {0, 0, 0, 0, 0, 0, 0};
std::vector<BACnetDevice> bacnet_network_cache;
QueueHandle_t bacnet_job_queue = NULL;
QueueHandle_t mqtt_publish_queue = NULL;
QueueHandle_t uart_evt_queue = NULL;
SemaphoreHandle_t cache_mutex = NULL;

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

static void uart_tx(const uint8_t *buffer, uint16_t length) {
    uart_write_bytes(RS485_UART_PORT, (const char*)buffer, length);
}

static void send_mstp_frame(uint8_t target_mac, uint8_t type, const uint8_t* apdu, uint16_t len) {
    uint8_t buffer[512+10];
    buffer[0]=0x55; buffer[1]=0xFF; buffer[2]=type; buffer[3]=target_mac; buffer[4]=sysCfg.mac_address;
    buffer[5]=(len>>8)&0xFF; buffer[6]=len&0xFF;
    buffer[7]=calc_header_crc(&buffer[2], 5);
    if (len > 0) {
        memcpy(&buffer[8], apdu, len);
        uint16_t crc16 = calc_data_crc(&buffer[8], len);
        buffer[8+len]=crc16&0xFF; buffer[8+len+1]=(crc16>>8)&0xFF;
        uart_tx(buffer, 8+len+2);
    } else uart_tx(buffer, 8);
    bacnetStats.ms_msgs_tx++;
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

static String get_unit_text(uint16_t units) {
    switch(units) {
        case 62: return "°C"; case 63: return "°F";
        case 98: return "%"; case 95: return "kWh";
        case 19: return "W"; case 47: return "V";
        case 3: return "A"; case 79: return "Pa";
        case 20: return "Wh"; case 17: return "kW";
        case 82: return "l/s"; case 84: return "m³/h";
        case 255: return "No Unit";
        default: return "Unit " + String(units);
    }
}

static void bacnet_task(void *pv) {
    uint8_t rx_byte; uint8_t header[6]; uint8_t header_idx=0; uint8_t data_buf[512]; 
    uint16_t data_len=0, data_idx=0, rx_crc16=0;
    enum { IDLE, PREAMBLE_55, PREAMBLE_FF, HEADER, DATA, CRC16_L, CRC16_H, MSTP_WAIT_TX_DONE } state = IDLE;
    uint32_t last_rx_time = millis(); 
    bool has_token = false, scan_done = false, waiting_for_reply = false;
    uint32_t token_acquired_time = 0, last_req_time = 0;
    uint8_t current_scan_index = 0, current_invoke_id = 10, current_poll_idx = 0;
    uint32_t heartbeat_timer = 0;
    uint8_t next_station = (sysCfg.mac_address + 1) % 128;
    enum { DISC_DEV_ID, DISC_DEV_NAME, DISC_DEV_VENDOR, DISC_LIST, DISC_NAME, DISC_UNITS, DISC_VALUE } disc_step = DISC_DEV_ID;
    uint8_t disc_obj_ptr = 0;
    uart_event_t event;

    z_log("[BACNET] Engine v4.7.18 - Stable Polling\n");

    for (;;) {
        if (xQueueReceive(uart_evt_queue, (void *)&event, 0) == pdTRUE) {
            if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) uart_flush_input(RS485_UART_PORT);
        }
        if (millis() - heartbeat_timer > 10000) {
            z_log("[BACNET] Heartbeat - Tokens: %lu, RX: %lu, TX: %lu\n", bacnetStats.tokens_seen, bacnetStats.ms_msgs_rx, bacnetStats.ms_msgs_tx);
            heartbeat_timer = millis();
        }

        if (state == MSTP_WAIT_TX_DONE) {
            if (uart_wait_tx_done(RS485_UART_PORT, 0) == ESP_OK) {
                if (waiting_for_reply) { last_req_time = millis(); state = IDLE; }
                else {
                    if (has_token) {
                        uint8_t f[8] = { 0x55, 0xFF, 0x00, next_station, sysCfg.mac_address, 0, 0, 0 };
                        f[7] = calc_header_crc(&f[2], 5); uart_tx(f, 8); has_token = false;
                    }
                    state = IDLE;
                }
            }
        }

        while (uart_read_bytes(RS485_UART_PORT, &rx_byte, 1, 0) > 0) {
            last_rx_time = millis();
            switch (state) {
                case IDLE: if (rx_byte == 0x55) { state = PREAMBLE_55; memset(data_buf, 0, sizeof(data_buf)); } break;
                case PREAMBLE_55: if (rx_byte == 0xFF) { state = HEADER; header_idx = 0; } else state = IDLE; break;
                case HEADER:
                    header[header_idx++] = rx_byte;
                    if (header_idx == 6) {
                        if (calc_header_crc(header, 5) == header[5]) {
                            bacnetStats.ms_msgs_rx++;
                            uint8_t type = header[0], dest = header[1], src_mac = header[2];
                            data_len = (header[3] << 8) | header[4];

                            if (type == 0x00 && dest == sysCfg.mac_address) { 
                                has_token = true; token_acquired_time = millis(); bacnetStats.tokens_seen++; next_station = (src_mac + 1) % 128; 
                            } else if (type == 0x01 && dest == sysCfg.mac_address) {
                                uint8_t f[8] = { 0x55, 0xFF, 0x02, src_mac, sysCfg.mac_address, 0, 0, 0 };
                                f[7] = calc_header_crc(&f[2], 5); uart_tx(f, 8); state = MSTP_WAIT_TX_DONE;
                            }

                            if (src_mac < 128 && src_mac != sysCfg.mac_address) {
                                if (xSemaphoreTake(cache_mutex, 0)) {
                                    bool known = false;
                                    for(auto& d : bacnet_network_cache) if(d.mac_address == src_mac) known = true;
                                    if(!known) {
                                        z_log("[BACNET] Found MAC %u\n", src_mac);
                                        BACnetDevice d; d.mac_address = src_mac; d.device_id = 4194303; d.enabled = true;
                                        bacnet_network_cache.push_back(d);
                                    }
                                    xSemaphoreGive(cache_mutex);
                                }
                            }
                            if (data_len > 0 && data_len <= 512) { state = DATA; data_idx = 0; } else if(state != MSTP_WAIT_TX_DONE) state = IDLE;
                        } else state = IDLE;
                    }
                    break;
                case DATA: data_buf[data_idx++] = rx_byte; if (data_idx == data_len) state = CRC16_L; break;
                case CRC16_L: rx_crc16 = rx_byte; state = CRC16_H; break;
                case CRC16_H: 
                    rx_crc16 |= (rx_byte << 8);
                    if (calc_data_crc(data_buf, data_len) == rx_crc16) {
                        uint16_t pos = 0;
                        if (data_len > 2 && data_buf[0] == 0x01) { // NPDU
                            pos = (data_buf[1] & 0x20) ? 10 : 2; 
                            if (pos < data_len) {
                                uint8_t pdu_type = data_buf[pos] & 0xF0;
                                if (pdu_type == 0x50 || pdu_type == 0x40 || pdu_type == 0x20) { // Error, Reject, SimpleAck
                                    waiting_for_reply = false;
                                    z_log("[BACNET] Received Error/Reject/Ack (Step:%d)\n", (int)disc_step);
                                    if (xSemaphoreTake(cache_mutex, 0)) {
                                        if (!bacnet_network_cache.empty()) {
                                            if (!scan_done) {
                                                if (disc_step == DISC_DEV_ID) disc_step = DISC_DEV_NAME;
                                                else if (disc_step == DISC_DEV_NAME) disc_step = DISC_DEV_VENDOR;
                                                else if (disc_step == DISC_DEV_VENDOR) disc_step = DISC_LIST;
                                                else if (disc_step == DISC_LIST && current_scan_index > 0) current_scan_index++;
                                                else if (disc_step == DISC_NAME) disc_step = DISC_UNITS;
                                                else if (disc_step == DISC_UNITS) disc_step = DISC_VALUE;
                                                else if (disc_step == DISC_VALUE) { disc_obj_ptr++; disc_step = DISC_NAME; }
                                                if (disc_obj_ptr >= bacnet_network_cache[0].objects.size()) {
                                                    if (disc_step != DISC_LIST && disc_step != DISC_DEV_ID && disc_step != DISC_DEV_NAME && disc_step != DISC_DEV_VENDOR && disc_step != DISC_UNITS) { scan_done = true; z_log("[BACNET] Scan Finalized (on error)\n"); }
                                                }
                                            } else {
                                                if (current_poll_idx < bacnet_network_cache[0].objects.size()) {
                                                    bacnet_network_cache[0].objects[current_poll_idx].enabled = false;
                                                    z_log("[BACNET] Disabled Obj T%u I%lu\n", bacnet_network_cache[0].objects[current_poll_idx].type, (unsigned long)bacnet_network_cache[0].objects[current_poll_idx].instance);
                                                }
                                            }
                                        }
                                        xSemaphoreGive(cache_mutex);
                                    }
                                } else if (pos + 3 < data_len && data_buf[pos] == 0x30) { // Complex Ack
                                    waiting_for_reply = false;
                                    pos += 3; BACnetTag t;
                                    while (pos < data_len && decode_next_tag(data_buf, &pos, data_len, &t)) {
                                        if (t.isOpening && t.number == 3) {
                                            if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(50))) {
                                                if (!bacnet_network_cache.empty()) {
                                                    BACnetTag val_tag;
                                                    if (decode_next_tag(data_buf, &pos, data_len, &val_tag)) {
                                                        if (disc_step == DISC_DEV_ID && val_tag.number == 12) {
                                                            uint32_t oid = (data_buf[pos]<<24)|(data_buf[pos+1]<<16)|(data_buf[pos+2]<<8)|data_buf[pos+3];
                                                            bacnet_network_cache[0].device_id = (oid & 0x3FFFFF);
                                                            z_log("[BACNET] Device ID Updated: %lu\n", (unsigned long)bacnet_network_cache[0].device_id);
                                                            disc_step = DISC_DEV_NAME;
                                                        } else if (disc_step == DISC_DEV_NAME && val_tag.number == 7) {
                                                            uint8_t enc = data_buf[pos]; char n[33]; n[0] = '\0';
                                                            if (enc == 0x00) { uint16_t slen = std::min((int)val_tag.len - 1, 32); if (slen > 0) memcpy(n, &data_buf[pos+1], slen); n[slen]='\0'; }
                                                            else if (enc == 0x04) { int slen = 0; for(int i=2; i < val_tag.len && slen < 32; i+=2) n[slen++] = data_buf[pos+i]; n[slen]='\0'; }
                                                            bacnet_network_cache[0].name = String(n);
                                                            z_log("[BACNET] Device Name: %s\n", n);
                                                            disc_step = DISC_DEV_VENDOR;
                                                        } else if (disc_step == DISC_DEV_VENDOR && val_tag.number == 7) {
                                                            uint8_t enc = data_buf[pos]; char n[33]; n[0] = '\0';
                                                            if (enc == 0x00) { uint16_t slen = std::min((int)val_tag.len - 1, 32); if (slen > 0) memcpy(n, &data_buf[pos+1], slen); n[slen]='\0'; }
                                                            else if (enc == 0x04) { int slen = 0; for(int i=2; i < val_tag.len && slen < 32; i+=2) n[slen++] = data_buf[pos+i]; n[slen]='\0'; }
                                                            bacnet_network_cache[0].vendor = String(n);
                                                            z_log("[BACNET] Device Vendor: %s\n", n);
                                                            disc_step = DISC_LIST;
                                                        } else if (disc_step == DISC_LIST) {
                                                            if (current_scan_index == 0) {
                                                                uint16_t count = data_buf[pos];
                                                                z_log("[BACNET] Device has %u objects\n", count);
                                                                bacnet_network_cache[0].objects.clear();
                                                                for(int i=0; i<count; i++) { 
                                                                    BACnetObject obj; 
                                                                    obj.enabled = false; 
                                                                    obj.type = 65535; 
                                                                    obj.instance = 0; 
                                                                    obj.name = "Unknown";
                                                                    bacnet_network_cache[0].objects.push_back(obj); 
                                                                }
                                                                current_scan_index = 1;
                                                            } else if (current_scan_index > 0 && current_scan_index <= bacnet_network_cache[0].objects.size() && val_tag.number == 12 && val_tag.len == 4) {
                                                                uint32_t oid = (data_buf[pos]<<24)|(data_buf[pos+1]<<16)|(data_buf[pos+2]<<8)|data_buf[pos+3];
                                                                if (oid != 0) {
                                                                    uint16_t o_type = (oid>>22);
                                                                    if (o_type < 1024) {
                                                                        auto& o = bacnet_network_cache[0].objects[current_scan_index-1];
                                                                        o.type = o_type;
                                                                        o.instance = (oid&0x3FFFFF);
                                                                        // Enable only if not filtered
                                                                        if (o_type != 8 && o_type != 10 && o_type != 16) {
                                                                            o.enabled = true;
                                                                        }
                                                                        z_log("[BACNET] Found Obj %u/%u: T%u I%lu (%s)\n", current_scan_index, (uint16_t)bacnet_network_cache[0].objects.size(), o_type, (unsigned long)o.instance, o.enabled ? "Enabled" : "Filtered");
                                                                    }
                                                                }
                                                                    current_scan_index++;
                                                                    if (current_scan_index > bacnet_network_cache[0].objects.size()) { disc_step = DISC_NAME; disc_obj_ptr = 0; }
                                                                }
                                                            } else if (disc_obj_ptr < bacnet_network_cache[0].objects.size()) {
                                                            auto& o = bacnet_network_cache[0].objects[disc_obj_ptr];
                                                            if (disc_step == DISC_NAME && val_tag.number == 7) { 
                                                                uint8_t enc = data_buf[pos]; char n[33]; n[0] = '\0';
                                                                if (enc == 0x00) {
                                                                    uint16_t slen = std::min((int)val_tag.len - 1, 32);
                                                                    if (slen > 0) memcpy(n, &data_buf[pos+1], slen); n[slen]='\0';
                                                                } else if (enc == 0x04) {
                                                                    int slen = 0; for(int i=2; i < val_tag.len && slen < 32; i+=2) n[slen++] = data_buf[pos+i]; n[slen]='\0';
                                                                }
                                                                o.name = String(n); 
                                                                if (o.type <= 2) disc_step = DISC_UNITS; // AI, AO, AV
                                                                else disc_step = DISC_VALUE;
                                                                z_log("[BACNET] Point %lu Name: %s\n", (unsigned long)o.instance, n);
                                                            } else if (disc_step == DISC_UNITS && val_tag.number == 9) { // Enumerated
                                                                uint32_t v = 0; for(int i=0; i<val_tag.len; i++) v = (v << 8) | data_buf[pos+i];
                                                                o.units = (uint16_t)v;
                                                                o.unit_text = get_unit_text(v);
                                                                z_log("[BACNET] Point %lu Units: %s\n", (unsigned long)o.instance, o.unit_text.c_str());
                                                                disc_step = DISC_VALUE;
                                                            } else { 
                                                                disc_obj_ptr++; disc_step = DISC_NAME; 
                                                                if (disc_obj_ptr >= bacnet_network_cache[0].objects.size()) { scan_done = true; z_log("[BACNET] Scan Complete\n"); }
                                                            }
                                                        } else {
                                                            // Success ReadProperty
                                                            if (val_tag.number == 4 && val_tag.len == 4) { // Real
                                                                float v; memcpy(&v, &data_buf[pos], 4);
                                                                // Swap endianness if needed (BACnet is Big Endian)
                                                                uint32_t tmp; memcpy(&tmp, &v, 4);
                                                                tmp = __builtin_bswap32(tmp);
                                                                memcpy(&v, &tmp, 4);
                                                                if (current_poll_idx < bacnet_network_cache[0].objects.size()) {
                                                                    auto& o = bacnet_network_cache[0].objects[current_poll_idx];
                                                                    o.present_value = v; o.last_update = millis();
                                                                    z_log("[BACNET] Polled T%u I%lu: %.2f\n", o.type, (unsigned long)o.instance, v);
                                                                    // TODO: MQTT Publish here
                                                                }
                                                            } else if (val_tag.number == 2) { // Unsigned
                                                                uint32_t v = 0; for(int i=0; i<val_tag.len; i++) v = (v << 8) | data_buf[pos+i];
                                                                if (current_poll_idx < bacnet_network_cache[0].objects.size()) {
                                                                    auto& o = bacnet_network_cache[0].objects[current_poll_idx];
                                                                    o.present_value = (float)v; o.last_update = millis();
                                                                    z_log("[BACNET] Polled T%u I%lu: %lu\n", o.type, (unsigned long)o.instance, (unsigned long)v);
                                                                }
                                                            } else if (val_tag.number == 9) { // Enumerated
                                                                uint32_t v = 0; for(int i=0; i<val_tag.len; i++) v = (v << 8) | data_buf[pos+i];
                                                                if (current_poll_idx < bacnet_network_cache[0].objects.size()) {
                                                                    auto& o = bacnet_network_cache[0].objects[current_poll_idx];
                                                                    o.present_value = (float)v; o.last_update = millis();
                                                                    z_log("[BACNET] Polled T%u I%lu: Enum(%lu)\n", o.type, (unsigned long)o.instance, (unsigned long)v);
                                                                }
                                                            }
                                                        }
                                                        pos += val_tag.len;
                                                    }
                                                }
                                                xSemaphoreGive(cache_mutex);
                                            }
                                        } else pos += t.len;
                                    }
                                }
                            }
                        }
                    }
                    state = IDLE; break;
            }
        }

        if (has_token && state == IDLE && !waiting_for_reply) {
            bool should_req = scan_done ? (bacnetStats.tokens_seen % 15 == 0) : (bacnetStats.tokens_seen % 10 == 0);
            if (should_req) {
                if (xSemaphoreTake(cache_mutex, 0)) {
                    if (!bacnet_network_cache.empty()) {
                        uint8_t apdu[64]; uint16_t apdu_len = 0;
                        if (!scan_done) {
                            if (disc_step == DISC_DEV_ID) apdu_len = build_read_property_apdu(apdu, current_invoke_id++, 8, 4194303, 75, -1);
                            else if (disc_step == DISC_DEV_NAME) apdu_len = build_read_property_apdu(apdu, current_invoke_id++, 8, bacnet_network_cache[0].device_id, 77, -1);
                            else if (disc_step == DISC_DEV_VENDOR) apdu_len = build_read_property_apdu(apdu, current_invoke_id++, 8, bacnet_network_cache[0].device_id, 121, -1);
                            else if (disc_step == DISC_LIST) apdu_len = build_read_property_apdu(apdu, current_invoke_id++, 8, bacnet_network_cache[0].device_id, 76, current_scan_index);
                            else {
                                while(disc_obj_ptr < bacnet_network_cache[0].objects.size() && (bacnet_network_cache[0].objects[disc_obj_ptr].type == 8 || !bacnet_network_cache[0].objects[disc_obj_ptr].enabled)) {
                                    disc_obj_ptr++; disc_step = DISC_NAME;
                                }
                                if (disc_obj_ptr < bacnet_network_cache[0].objects.size()) {
                                    auto& o = bacnet_network_cache[0].objects[disc_obj_ptr];
                                    uint8_t pid = 77; // Object_Name
                                    if (disc_step == DISC_UNITS) pid = 117; // Engineering_Units
                                    else if (disc_step == DISC_VALUE) pid = 85; // Present_Value
                                    apdu_len = build_read_property_apdu(apdu, current_invoke_id++, o.type, o.instance, pid, -1);
                                } else { scan_done = true; z_log("[BACNET] Scan Finished (all filtered)\n"); }
                            }
                        } else {
                            size_t count = bacnet_network_cache[0].objects.size();
                            for(size_t i=0; i<count; i++) {
                                current_poll_idx = (current_poll_idx + 1) % count;
                                auto& o = bacnet_network_cache[0].objects[current_poll_idx];
                                if (o.enabled && o.type != 8) {
                                    apdu_len = build_read_property_apdu(apdu, current_invoke_id++, o.type, o.instance, 85, -1);
                                    break;
                                }
                            }
                        }
                        if (apdu_len > 0) {
                            send_mstp_frame(bacnet_network_cache[0].mac_address, 0x05, apdu, apdu_len);
                            waiting_for_reply = true;
                        }
                    }
                    xSemaphoreGive(cache_mutex);
                }
            }

            uint32_t limit = waiting_for_reply ? 500 : 5;
            if (millis() - token_acquired_time > limit) {
                if (waiting_for_reply && (millis() - last_req_time > 1500)) { 
                    waiting_for_reply = false; z_log("[BACNET] Reply Timeout\n"); 
                    // Progression forcée au timeout
                    if (xSemaphoreTake(cache_mutex, 0)) {
                        if (!bacnet_network_cache.empty() && !scan_done) {
                            if (disc_step == DISC_DEV_ID) disc_step = DISC_DEV_NAME;
                            else if (disc_step == DISC_DEV_NAME) disc_step = DISC_DEV_VENDOR;
                            else if (disc_step == DISC_DEV_VENDOR) disc_step = DISC_LIST;
                            else if (disc_step == DISC_LIST && current_scan_index > 0) current_scan_index++;
                            else if (disc_step == DISC_NAME) disc_step = DISC_UNITS;
                            else if (disc_step == DISC_UNITS) disc_step = DISC_VALUE;
                            else if (disc_step == DISC_VALUE) { disc_obj_ptr++; disc_step = DISC_NAME; }
                            if (!bacnet_network_cache.empty() && disc_obj_ptr >= bacnet_network_cache[0].objects.size()) {
                                if (disc_step != DISC_LIST && disc_step != DISC_DEV_ID && disc_step != DISC_DEV_NAME && disc_step != DISC_DEV_VENDOR && disc_step != DISC_UNITS) { scan_done = true; z_log("[BACNET] Scan Finalized (on timeout)\n"); }
                            }
                        }
                        xSemaphoreGive(cache_mutex);
                    }
                }
                if (!waiting_for_reply && has_token) {
                    uint8_t f[8] = { 0x55, 0xFF, 0x00, next_station, sysCfg.mac_address, 0, 0, 0 };
                    f[7] = calc_header_crc(&f[2], 5); uart_tx(f, 8); has_token = false; state = MSTP_WAIT_TX_DONE;
                }
            }
        }
        
        if (millis() - last_rx_time > 3000) { 
            last_rx_time = millis(); 
            uint8_t f[8]={0x55,0xFF,0x01, (uint8_t)((sysCfg.mac_address+1)%128), sysCfg.mac_address,0,0,0}; 
            f[7]=calc_header_crc(&f[2],5); uart_tx(f,8); state = MSTP_WAIT_TX_DONE;
            z_log("[BACNET] Token Reg (Bus Silent)\n");
        }
        vTaskDelay(1);
    }
}

void setup_bacnet_engine() {
    cache_mutex = xSemaphoreCreateMutex();
    bacnet_job_queue = xQueueCreate(10, sizeof(BACnetJob));
    mqtt_publish_queue = xQueueCreate(30, sizeof(MQTTPublishJob));
    const uart_config_t uc = { .baud_rate = 38400, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .rx_flow_ctrl_thresh = 122, .source_clk = UART_SCLK_APB };
    uart_driver_install(RS485_UART_PORT, 2048, 2048, 20, &uart_evt_queue, 0);
    uart_param_config(RS485_UART_PORT, &uc);
    uart_set_pin(RS485_UART_PORT, TX_PIN, RX_PIN, RTS_PIN, UART_PIN_NO_CHANGE);
    uart_set_mode(RS485_UART_PORT, UART_MODE_RS485_HALF_DUPLEX);
    xTaskCreatePinnedToCore(bacnet_task, "BACnet", 16384, NULL, 15, NULL, 1);
    z_log("[BACNET] Engine Initialized\n");
}
bool enqueue_bacnet_job(BACnetJob job) { if (bacnet_job_queue == NULL) return false; return xQueueSend(bacnet_job_queue, &job, 0) == pdTRUE; }
bool enqueue_mqtt_publish(MQTTPublishJob pubJob) { if (mqtt_publish_queue == NULL) return false; return xQueueSend(mqtt_publish_queue, &pubJob, 0) == pdTRUE; }
