#include "z_bacnet.h"
#include <string.h>
#include <algorithm>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "z_network.h" 
#include "rom/ets_sys.h"

extern void z_log(const char* format, ...);
extern void save_device_objects(uint32_t device_id);

// --- GLOBALS ---
BACnet_Stats bacnetStats = {0, 0, 0, 0, 0, 0, 0};       
std::vector<BACnetDevice> bacnet_network_cache;         
QueueHandle_t bacnet_job_queue = NULL;                  
QueueHandle_t mqtt_publish_queue = NULL;                
QueueHandle_t uart_evt_queue = NULL;                    
SemaphoreHandle_t cache_mutex = NULL;                   

// --- CRC UTILS ---
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

// --- BACNET ASN.1 TAG DECODER ---
struct BACnetTag { uint32_t number; bool isContext; uint32_t len; bool isOpening; bool isClosing; uint8_t tag_raw; };

static bool decode_next_tag(const uint8_t *data, uint16_t *pos, uint16_t max_len, BACnetTag *tag) {
    if (*pos >= max_len) return false;
    uint8_t b = data[(*pos)++];
    tag->tag_raw = b;
    tag->number = b >> 4;
    tag->isContext = (b & 0x08) != 0;
    uint8_t lvt = b & 0x07;
    if (tag->number == 0x0F) tag->number = data[(*pos)++];
    tag->isOpening = (lvt == 6);
    tag->isClosing = (lvt == 7);
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
    bacnetStats.ms_msgs_tx++;
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
    } else {
        uart_tx(buffer, 8);
    }
}

static void bacnet_task(void *pv) {
    uint8_t rx_byte; uint8_t header[6]; uint8_t header_idx=0; uint8_t data_buf[512]; 
    uint16_t data_len=0, data_idx=0, rx_crc16=0;
    
    enum { IDLE, PREAMBLE_55, PREAMBLE_FF, HEADER, DATA, CRC16_L, CRC16_H, MSTP_WAIT_TX_DONE } state = IDLE;
    uint32_t last_rx_time = millis(); 
    uint32_t who_is_timer = 0;    
    bool has_token = false, scan_done = false, waiting_for_reply = false;
    uint32_t token_acquired_time = 0, last_req_time = 0;
    uint32_t target_device_id = 0x3FFFFF;
    uint8_t total_objects = 0, current_scan_index = 0, current_invoke_id = 10, current_poll_idx = 0;
    uint32_t heartbeat_timer = 0;
    uint8_t next_station = (sysCfg.mac_address + 1) % 128;
    
    enum { DISC_LIST, DISC_DEV_NAME, DISC_DEV_VENDOR, DISC_NAME, DISC_VALUE, DISC_UNITS, DISC_STATE_COUNT, DISC_STATE_TEXT, DISC_BINARY_ACTIVE, DISC_BINARY_INACTIVE } disc_step = DISC_LIST;
    uint8_t disc_obj_ptr = 0;
    uint16_t disc_state_idx = 0;
    uint8_t discovery_interleave_cnt = 0; 
    uint8_t frames_sent_this_token = 0;
    const uint8_t N_max_info_frames = 3; 

    uart_event_t event;
    
    for (;;) {
        if (xQueueReceive(uart_evt_queue, (void *)&event, 0) == pdTRUE) {
            if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) uart_flush_input(RS485_UART_PORT);
        }

        uint32_t now = millis();
        if (now - heartbeat_timer > 10000) {
            z_log("[BACNET] Heartbeat - Tokens: %lu, RX: %lu, TX: %lu\n", bacnetStats.tokens_seen, bacnetStats.ms_msgs_rx, bacnetStats.ms_msgs_tx);
            heartbeat_timer = now;
        }

        if (!scan_done && bacnet_network_cache.empty() && (now - who_is_timer > 10000)) {
            BACnetJob wj; wj.type = JOB_WHO_IS;
            if (enqueue_bacnet_job(wj)) {
                z_log("[BACNET] Enqueued automatic Who-Is (Cache Empty)\n");
                who_is_timer = now;
            }
        }

        // --- MASTER LOGIC ---
        if (has_token && state == IDLE) {
            uint32_t now = millis();
            bool can_send = !waiting_for_reply && (frames_sent_this_token < N_max_info_frames);
            
            // Respect Tusage_delay (15ms max between reply and next request)
            if (can_send && (now - last_rx_time < 15)) {
                BACnetJob job;
                bool req_sent = false;
                
                if (xQueueReceive(bacnet_job_queue, &job, 0) == pdTRUE) {
                    if (job.type == JOB_WHO_IS) {
                        uint8_t payload[] = { 0x01, 0x20, 0xFF, 0xFF, 0x00, 0xFF, 0x10, 0x08 };
                        send_mstp_frame(0xFF, 0x06, payload, 8); 
                        waiting_for_reply = false;
                    } else if (job.type == JOB_WRITE_PROP) {
                        uint8_t apdu[] = { 0x01, 0x00, 0x00, 0x0F, current_invoke_id++, 0x01, 0x0C, 
                            (uint8_t)((job.obj_type>>2)&0xFF), (uint8_t)((job.obj_type<<6)|(job.obj_instance>>16)), (uint8_t)(job.obj_instance>>8), (uint8_t)job.obj_instance,
                            0x19, 0x55, 0x3E, 0x44, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x49, job.priority };
                        union { uint32_t i; float f; } u; u.f = job.write_value;
                        apdu[15] = (u.i >> 24) & 0xFF; apdu[16] = (u.i >> 16) & 0xFF; apdu[17] = (u.i >> 8) & 0xFF; apdu[18] = u.i & 0xFF;
                        send_mstp_frame(job.target_mac, 0x05, apdu, sizeof(apdu));
                        waiting_for_reply = true;
                    }
                    req_sent = true;
                } 
                else if (now % 5 == 0 || !scan_done) {
                    if (xSemaphoreTake(cache_mutex, 0)) { // Non-blocking take
                        if (!scan_done) {
                            if (disc_step == DISC_LIST) {
                                uint8_t apdu[] = { 0x01, 0x00, 0x00, 0x05, current_invoke_id++, 0x0C, 0x0C, 0x02, 0x00, 0x00, 0x00, 0x19, 0x4C, 0x29, current_scan_index };
                                if (target_device_id != 0x3FFFFF) { apdu[8]=(target_device_id>>16)&0xFF; apdu[9]=(target_device_id>>8)&0xFF; apdu[10]=target_device_id&0xFF; }
                                if (!bacnet_network_cache.empty()) send_mstp_frame(bacnet_network_cache[0].mac_address, 0x05, apdu, sizeof(apdu));
                            } else if (disc_step == DISC_DEV_NAME || disc_step == DISC_DEV_VENDOR) {
                                uint8_t pid = (disc_step == DISC_DEV_NAME) ? 77 : 121;
                                uint8_t apdu[] = { 0x01, 0x00, 0x00, 0x05, current_invoke_id++, 0x0C, 0x0C, 0x02, 0x00, 0x00, 0x00, 0x19, pid };
                                if (target_device_id != 0x3FFFFF) { apdu[8]=(target_device_id>>16)&0xFF; apdu[9]=(target_device_id>>8)&0xFF; apdu[10]=target_device_id&0xFF; }
                                if (!bacnet_network_cache.empty()) send_mstp_frame(bacnet_network_cache[0].mac_address, 0x05, apdu, sizeof(apdu));
                            } else if (!bacnet_network_cache.empty()) {
                                auto& o = bacnet_network_cache[0].objects[disc_obj_ptr];
                                uint8_t pid = 85; uint16_t aid = 0xFFFF; bool has_aid = false;
                                if (disc_step == DISC_NAME) pid = 77;
                                else if (disc_step == DISC_UNITS) pid = 117;
                                else if (disc_step == DISC_STATE_COUNT) { pid = 110; aid = 0; has_aid = true; }
                                else if (disc_step == DISC_STATE_TEXT) { pid = 110; aid = disc_state_idx; has_aid = true; }
                                else if (disc_step == DISC_BINARY_ACTIVE) pid = 4;
                                else if (disc_step == DISC_BINARY_INACTIVE) pid = 46;
                                uint8_t apdu[24]; uint8_t len = 0;
                                apdu[len++] = 0x01; apdu[len++] = 0x00; apdu[len++] = 0x00; apdu[len++] = 0x05; apdu[len++] = current_invoke_id++;
                                apdu[len++] = 0x0C; apdu[len++] = 0x0C;
                                apdu[len++] = (uint8_t)((o.type>>2)&0xFF); apdu[len++] = (uint8_t)((o.type<<6)|(o.instance>>16));
                                apdu[len++] = (uint8_t)(o.instance>>8); apdu[len++] = (uint8_t)o.instance;
                                apdu[len++] = 0x19; apdu[len++] = pid;
                                if (has_aid) { apdu[len++] = 0x29; apdu[len++] = (uint8_t)aid; }
                                send_mstp_frame(bacnet_network_cache[0].mac_address, 0x05, apdu, len);
                            }
                            waiting_for_reply = true; req_sent = true;
                        } else if (!bacnet_network_cache.empty() && discovery_interleave_cnt < 2) {
                            current_poll_idx = (current_poll_idx + 1) % bacnet_network_cache[0].objects.size();
                            auto& o = bacnet_network_cache[0].objects[current_poll_idx];
                            if (o.enabled && bacnet_network_cache[0].enabled && o.type != 8) {
                                uint8_t apdu[] = { 0x01, 0x00, 0x00, 0x05, current_invoke_id++, 0x0C, 0x0C, (uint8_t)((o.type>>2)&0xFF), (uint8_t)((o.type<<6)|(o.instance>>16)), (uint8_t)(o.instance>>8), (uint8_t)o.instance, 0x19, 0x55 };
                                send_mstp_frame(bacnet_network_cache[0].mac_address, 0x05, apdu, sizeof(apdu));
                                waiting_for_reply = true; req_sent = true;
                                discovery_interleave_cnt++;
                            }
                        } else { discovery_interleave_cnt = 0; }
                        xSemaphoreGive(cache_mutex);
                    }
                }
                
                if (req_sent) { frames_sent_this_token++; state = MSTP_WAIT_TX_DONE; }
                else { frames_sent_this_token = N_max_info_frames; }
            }
            
            uint32_t limit = waiting_for_reply ? (sysCfg.apdu_timeout + 100) : 10;
            if (now - token_acquired_time > limit) {
                if (waiting_for_reply && (now - last_req_time > sysCfg.apdu_timeout)) waiting_for_reply = false;
                if (!waiting_for_reply) {
                    uint8_t f[8] = { 0x55, 0xFF, 0x00, next_station, sysCfg.mac_address, 0, 0, 0 };
                    f[7] = calc_header_crc(&f[2], 5); uart_tx(f, 8);
                    has_token = false; frames_sent_this_token = 0;
                    state = MSTP_WAIT_TX_DONE;
                }
            }
        }

        // --- RECEIVE LOGIC ---
        while (uart_read_bytes(RS485_UART_PORT, &rx_byte, 1, 0) > 0) {
            last_rx_time = millis();
            switch (state) {
                case IDLE: if (rx_byte == 0x55) state = PREAMBLE_55; break;
                case PREAMBLE_55: if (rx_byte == 0xFF) { state = HEADER; header_idx = 0; } else state = IDLE; break;
                case HEADER:
                    header[header_idx++] = rx_byte;
                    if (header_idx == 6) {
                        if (calc_header_crc(header, 5) == header[5]) {
                            bacnetStats.ms_msgs_rx++;
                            uint8_t type = header[0], dest = header[1], src_mac = header[2];
                            data_len = (header[3] << 8) | header[4];
                            if (type == 0x00 && dest == sysCfg.mac_address) { has_token = true; token_acquired_time = millis(); bacnetStats.tokens_seen++; next_station = src_mac; }
                            else if (type == 0x01 && dest == sysCfg.mac_address) { 
                                vTaskDelay(pdMS_TO_TICKS(15));
                                uint8_t f[8] = { 0x55, 0xFF, 0x02, src_mac, sysCfg.mac_address, 0, 0, 0 };
                                f[7] = calc_header_crc(&f[2], 5); uart_tx(f, 8);
                                state = MSTP_WAIT_TX_DONE;
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
                        last_rx_time = millis(); 
                        uint16_t pos = 0;
                        if (data_len > 2 && data_buf[0] == 0x01) { 
                            uint8_t control = data_buf[1];
                            pos = 2;
                            if (control & 0x20) { pos += 2; uint8_t dlen = data_buf[pos++]; pos += dlen; pos++; } 
                            if (control & 0x40) { pos += 2; uint8_t slen = data_buf[pos++]; pos += slen; }

                            if ((control & 0x80) == 0) { // Contains APDU
                                uint8_t apdu_type = data_buf[pos] & 0xF0;
                                // z_log("[BACNET] RX Data: SRC=%d, APDU=%02X\n", header[4], data_buf[pos]);

                                if (apdu_type == 0x10) { // Unconfirmed-Request (I-Am)
                                pos++;
                                if (data_buf[pos] == 0x00) { // I-Am Service
                                    pos++; BACnetTag t;
                                    if (decode_next_tag(data_buf, &pos, data_len, &t) && t.number == 12) {
                                        uint32_t di = ((uint32_t)(data_buf[pos] & 0x3F) << 16) | (data_buf[pos+1] << 8) | data_buf[pos+2];
                                        if (xSemaphoreTake(cache_mutex, 0)) {
                                            bool found = false;
                                            for(auto& d : bacnet_network_cache) if(d.device_id == di) found = true;
                                            if (!found) {
                                                BACnetDevice dev; dev.device_id = di; dev.mac_address = header[4]; 
                                                dev.enabled = true; dev.discovery_done = false; dev.name = "Device_" + String(di);
                                                bacnet_network_cache.push_back(dev);
                                                target_device_id = di;
                                                z_log("[BACNET] I-Am from %lu (MAC: %d)\n", (unsigned long)di, dev.mac_address);
                                            }
                                            xSemaphoreGive(cache_mutex);
                                        }
                                    }
                                }
                            } else if (apdu_type == 0x50 || apdu_type == 0x60 || apdu_type == 0x70) {
                                if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(50))) {
                                    if (!scan_done) {
                                        if (disc_step == DISC_DEV_NAME) disc_step = DISC_DEV_VENDOR;
                                        else if (disc_step == DISC_DEV_VENDOR) { disc_step = DISC_NAME; disc_obj_ptr = 0; }
                                        else if (disc_step == DISC_NAME) disc_step = DISC_VALUE;
                                        else if (disc_step == DISC_VALUE) { disc_obj_ptr++; disc_step = DISC_NAME; }
                                        else if (disc_step == DISC_UNITS || disc_step == DISC_BINARY_ACTIVE || disc_step == DISC_STATE_COUNT) { disc_obj_ptr++; disc_step = DISC_NAME; }
                                        if (!bacnet_network_cache.empty() && disc_obj_ptr >= bacnet_network_cache[0].objects.size()) { scan_done = true; save_device_objects(target_device_id); }
                                    }
                                    xSemaphoreGive(cache_mutex);
                                }
                            } else if (pos + 3 < data_len && data_buf[pos] == 0x30) { // Complex-ACK
                                pos += 3; BACnetTag t;
                                while (decode_next_tag(data_buf, &pos, data_len, &t)) {
                                    if (t.isOpening && t.number == 3) {
                                        if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(50))) {
                                            BACnetTag val_tag;
                                            if (decode_next_tag(data_buf, &pos, data_len, &val_tag)) {
                                                if (!scan_done) {
                                                    if (disc_step == DISC_LIST) {
                                                        if (current_scan_index == 0) {
                                                            if (val_tag.number == 2) total_objects = data_buf[pos];
                                                            bacnetStats.total_objects = total_objects;
                                                            current_scan_index = 1; bacnetStats.current_index = 1;
                                                            BACnetDevice d; d.mac_address = header[2]; d.device_id = 0; d.discovery_done = false; d.enabled = true;
                                                            d.name = "Unknown Device"; d.vendor = "Unknown Vendor";
                                                            bacnet_network_cache.push_back(d);
                                                        } else if (!bacnet_network_cache.empty()) {
                                                            if (val_tag.number == 12) {
                                                                uint16_t ot = (data_buf[pos] << 2) | (data_buf[pos+1] >> 6);
                                                                uint32_t oi = ((uint32_t)(data_buf[pos+1] & 0x3F) << 16) | (data_buf[pos+2] << 8) | data_buf[pos+3];
                                                                if (current_scan_index == 1) { target_device_id = oi; bacnet_network_cache[0].device_id = oi; }
                                                                BACnetObject obj; obj.type = ot; obj.instance = oi; obj.present_value = 0; obj.last_update = 0; obj.enabled = true;
                                                                obj.name = "Point_" + String(oi); obj.discovery_done = false; obj.expected_states_count = 0;
                                                                bacnet_network_cache[0].objects.push_back(obj);
                                                                current_scan_index++; 
                                                                if (current_scan_index > total_objects) disc_step = DISC_DEV_NAME;
                                                            }
                                                        }
                                                    } else if (disc_step == DISC_DEV_NAME || disc_step == DISC_DEV_VENDOR) {
                                                        if (val_tag.number == 7) {
                                                            char n[65]; uint16_t slen = std::min((int)val_tag.len - 1, 64);
                                                            memcpy(n, &data_buf[pos+1], slen); n[slen] = '\0';
                                                            if (disc_step == DISC_DEV_NAME) { bacnet_network_cache[0].name = String(n); disc_step = DISC_DEV_VENDOR; }
                                                            else { bacnet_network_cache[0].vendor = String(n); disc_step = DISC_NAME; disc_obj_ptr = 0; }
                                                        }
                                                    } else if (!bacnet_network_cache.empty() && disc_obj_ptr < bacnet_network_cache[0].objects.size()) {
                                                        auto& o = bacnet_network_cache[0].objects[disc_obj_ptr];
                                                        if (disc_step == DISC_NAME && val_tag.number == 7) {
                                                            char n[33]; uint16_t slen = std::min((int)val_tag.len - 1, 32);
                                                            memcpy(n, &data_buf[pos+1], slen); n[slen] = '\0';
                                                            o.name = String(n); disc_step = DISC_VALUE;
                                                        } else if (disc_step == DISC_VALUE) {
                                                            if (val_tag.number == 4) { union { uint32_t i; float f; } u; u.i = (data_buf[pos]<<24)|(data_buf[pos+1]<<16)|(data_buf[pos+2]<<8)|(data_buf[pos+3]); o.present_value = u.f; }
                                                            else if (val_tag.number == 9 || val_tag.number == 2) o.present_value = (float)data_buf[pos];
                                                            o.last_update = millis();
                                                            if (o.type <= 2) disc_step = DISC_UNITS;
                                                            else if (o.type >= 3 && o.type <= 5) disc_step = DISC_BINARY_ACTIVE;
                                                            else if (o.type == 13 || o.type == 14 || o.type == 19) disc_step = DISC_STATE_COUNT;
                                                            else { o.discovery_done = true; disc_obj_ptr++; disc_step = DISC_NAME; }
                                                        } else if (disc_step == DISC_UNITS && val_tag.number == 2) {
                                                            o.units = (val_tag.len == 1) ? data_buf[pos] : (data_buf[pos]<<8)|data_buf[pos+1];
                                                            o.discovery_done = true; disc_obj_ptr++; disc_step = DISC_NAME;
                                                        } else if (disc_step == DISC_BINARY_ACTIVE && val_tag.number == 7) {
                                                            char n[33]; uint16_t slen = std::min((int)val_tag.len - 1, 32); memcpy(n, &data_buf[pos+1], slen); n[slen] = '\0';
                                                            o.state_texts.push_back(String(n)); disc_step = DISC_BINARY_INACTIVE;
                                                        } else if (disc_step == DISC_BINARY_INACTIVE && val_tag.number == 7) {
                                                            char n[33]; uint16_t slen = std::min((int)val_tag.len - 1, 32); memcpy(n, &data_buf[pos+1], slen); n[slen] = '\0';
                                                            o.state_texts.insert(o.state_texts.begin(), String(n));
                                                            o.discovery_done = true; disc_obj_ptr++; disc_step = DISC_NAME;
                                                        } else if (disc_step == DISC_STATE_COUNT && val_tag.number == 2) {
                                                            o.expected_states_count = data_buf[pos];
                                                            if (o.expected_states_count > 0) { disc_step = DISC_STATE_TEXT; disc_state_idx = 1; }
                                                            else { o.discovery_done = true; disc_obj_ptr++; disc_step = DISC_NAME; }
                                                        } else if (disc_step == DISC_STATE_TEXT && val_tag.number == 7) {
                                                            char n[33]; uint16_t slen = std::min((int)val_tag.len - 1, 32); memcpy(n, &data_buf[pos+1], slen); n[slen] = '\0';
                                                            o.state_texts.push_back(String(n));
                                                            if (o.state_texts.size() >= o.expected_states_count) { o.discovery_done = true; disc_obj_ptr++; disc_step = DISC_NAME; }
                                                            else disc_state_idx++;
                                                        }
                                                        if (disc_obj_ptr >= bacnet_network_cache[0].objects.size()) { scan_done = true; save_device_objects(target_device_id); z_log("[BACNET] Discovery Finalized.\n"); }
                                                    } else { // Polling
                                                        float val = 0;
                                                        if (val_tag.number == 4) { union { uint32_t i; float f; } u; u.i = (data_buf[pos]<<24)|(data_buf[pos+1]<<16)|(data_buf[pos+2]<<8)|(data_buf[pos+3]); val = u.f; }
                                                        else if (val_tag.number == 9 || val_tag.number == 2) val = (float)data_buf[pos];
                                                        if (current_poll_idx < bacnet_network_cache[0].objects.size()) {
                                                            auto& o = bacnet_network_cache[0].objects[current_poll_idx];
                                                            o.present_value = val; o.last_update = millis();
                                                            MQTTPublishJob pubJob; pubJob.device_id = bacnet_network_cache[0].device_id; pubJob.obj_type = o.type; pubJob.obj_instance = o.instance;
                                                            int v_idx = (int)val;
                                                            if (!o.state_texts.empty() && v_idx >= 0 && v_idx < (int)o.state_texts.size()) snprintf(pubJob.value_string, sizeof(pubJob.value_string), "%s", o.state_texts[v_idx].c_str());
                                                            else snprintf(pubJob.value_string, sizeof(pubJob.value_string), "%.2f", val);
                                                            enqueue_mqtt_publish(pubJob);
                                                        }
                                                    }
                                                }
                                            }
                                            xSemaphoreGive(cache_mutex);
                                        }
                                    }
                                    pos += t.len;
                                }
                            }
                            } // End of if ((control & 0x80) == 0)
                        }
                    }
                    state = IDLE; break;
                default: break;
            }
        }

        if (state == MSTP_WAIT_TX_DONE) {
            if (uart_wait_tx_done(RS485_UART_PORT, 0) == ESP_OK) {
                if (waiting_for_reply) state = IDLE;
                else {
                    if (has_token && frames_sent_this_token >= N_max_info_frames) {
                        uint8_t f[8] = { 0x55, 0xFF, 0x00, next_station, sysCfg.mac_address, 0, 0, 0 };
                        f[7] = calc_header_crc(&f[2], 5); uart_tx(f, 8);
                        has_token = false; frames_sent_this_token = 0;
                    }
                    state = IDLE;
                }
            }
        }

        if (millis() - last_rx_time > 5000) { 
            last_rx_time = millis(); 
            uint8_t f[8]={0x55,0xFF,0x01,next_station,sysCfg.mac_address,0,0,0}; f[7]=calc_header_crc(&f[2],5); 
            uart_tx(f,8); state = MSTP_WAIT_TX_DONE;
            z_log("[BACNET] Token Regeneration\n");
        }
        vTaskDelay(1);
    }
}

void setup_bacnet_engine() {
    cache_mutex = xSemaphoreCreateMutex();
    bacnet_job_queue = xQueueCreate(20, sizeof(BACnetJob));
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
