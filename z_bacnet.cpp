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

struct BACnetTag { uint32_t number; bool isContext; uint32_t len; bool isOpening; bool isClosing; uint8_t tag_raw; };
static bool decode_next_tag(const uint8_t *data, uint16_t *pos, uint16_t max_len, BACnetTag *tag) {
    if (*pos >= max_len) return false;
    uint8_t b = data[(*pos)++];
    tag->tag_raw = b;
    tag->number = b >> 4;
    tag->isContext = (b & 0x08) != 0;
    uint8_t lvt = b & 0x07;
    if (tag->number == 0x0F) {
        if (*pos >= max_len) return false;
        tag->number = data[(*pos)++];
    }
    tag->isOpening = (lvt == 6);
    tag->isClosing = (lvt == 7);
    if (lvt <= 4) tag->len = lvt;
    else if (lvt == 5) {
        if (*pos >= max_len) return false;
        tag->len = data[(*pos)++];
        if (tag->len == 254) { 
            if (*pos + 1 >= max_len) return false;
            tag->len = (data[*pos] << 8) | data[*pos+1]; *pos += 2; 
        }
        else if (tag->len == 255) { 
            if (*pos + 3 >= max_len) return false;
            tag->len = ((uint32_t)data[*pos] << 24) | ((uint32_t)data[*pos+1] << 16) | ((uint32_t)data[*pos+2] << 8) | data[*pos+3]; *pos += 4; 
        }
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
    uint8_t next_station = 4; uint32_t last_rx_time = millis(); 
    bool has_token = false, scan_done = false, waiting_for_reply = false;
    uint32_t token_acquired_time = 0, last_req_time = 0;
    uint32_t target_device_id = 0x3FFFFF;
    uint8_t total_objects = 0, current_scan_index = 0, current_invoke_id = 10, current_poll_idx = 0;
    uint32_t heartbeat_timer = 0;
    
    enum { DISC_LIST, DISC_NAME, DISC_VALUE, DISC_UNITS } disc_step = DISC_LIST;
    uint8_t disc_obj_ptr = 0;

    uart_event_t event;
    z_log("[BACNET] Engine v4.5.29 - Deep Diagnostic Mode\n");

    for (;;) {
        if (xQueueReceive(uart_evt_queue, (void *)&event, 0) == pdTRUE) {
            if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) {
                z_log("[BACNET] UART Overflow/Full Event detected\n");
                uart_flush_input(RS485_UART_PORT);
            }
        }

        if (millis() - heartbeat_timer > 10000) {
            z_log("[BACNET] Task Heartbeat (Tokens: %lu, RX: %lu, TX: %lu)\n", 
                bacnetStats.tokens_seen, bacnetStats.ms_msgs_rx, bacnetStats.ms_msgs_tx);
            heartbeat_timer = millis();
        }

        if (has_token && state == IDLE && !waiting_for_reply) {
            BACnetJob job;
            if (xQueueReceive(bacnet_job_queue, &job, 0) == pdTRUE) {
                if (job.type == JOB_WHO_IS) {
                    if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(100))) {
                        scan_done = false; current_scan_index = 0; disc_step = DISC_LIST;
                        bacnet_network_cache.clear();
                        xSemaphoreGive(cache_mutex);
                    }
                    z_log("[BACNET] Manual Scan Triggered (Who-Is)\n");
                } else if (job.type == JOB_WRITE_PROP) {
                    uint8_t apdu[] = { 0x01, 0x00, 0x00, 0x0F, current_invoke_id++, 0x01, 0x0C, 
                        (uint8_t)((job.obj_type>>2)&0xFF), (uint8_t)((job.obj_type<<6)|(job.obj_instance>>16)), (uint8_t)(job.obj_instance>>8), (uint8_t)job.obj_instance,
                        0x19, 0x55, 0x3E, 0x44, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x49, job.priority };
                    union { uint32_t i; float f; } u; u.f = job.write_value;
                    apdu[15] = (u.i >> 24) & 0xFF; apdu[16] = (u.i >> 16) & 0xFF; apdu[17] = (u.i >> 8) & 0xFF; apdu[18] = u.i & 0xFF;
                    send_mstp_frame(job.target_mac, 0x05, apdu, sizeof(apdu));
                    state = MSTP_WAIT_TX_DONE;
                }
            }
        }

        if (state == MSTP_WAIT_TX_DONE) {
            if (uart_wait_tx_done(RS485_UART_PORT, 0) == ESP_OK) {
                last_req_time = millis();
                state = IDLE;
            }
        }

        while (uart_read_bytes(RS485_UART_PORT, &rx_byte, 1, 0) > 0) {
            last_rx_time = millis();
            switch (state) {
                case IDLE: 
                    if (rx_byte == 0x55) state = PREAMBLE_55; 
                    break;
                case PREAMBLE_55: 
                    if (rx_byte == 0xFF) { state = HEADER; header_idx = 0; } 
                    else if (rx_byte == 0x55) state = PREAMBLE_55;
                    else state = IDLE; 
                    break;
                case HEADER:
                    header[header_idx++] = rx_byte;
                    if (header_idx == 6) {
                        if (calc_header_crc(header, 5) == header[5]) {
                            bacnetStats.ms_msgs_rx++;
                            uint8_t type = header[0], dest = header[1], src = header[2];
                            data_len = (header[3] << 8) | header[4];
                            
                            if (type == 0x00 && dest == sysCfg.mac_address) { 
                                has_token = true; token_acquired_time = millis(); 
                                // z_log("[BACNET] Token Received from MAC %d\n", src);
                            }
                            else if (type == 0x01 && dest == sysCfg.mac_address) { 
                                vTaskDelay(pdMS_TO_TICKS(2)); 
                                uint8_t f[8] = { 0x55, 0xFF, 0x02, src, sysCfg.mac_address, 0, 0, 0 };
                                f[7] = calc_header_crc(&f[2], 5); uart_tx(f, 8);
                                state = MSTP_WAIT_TX_DONE;
                                z_log("[BACNET] Poll-For-Master reply sent to MAC %d\n", src);
                            }
                            
                            if (data_len > 0 && data_len <= 512) { state = DATA; data_idx = 0; } 
                            else if(state != MSTP_WAIT_TX_DONE) state = IDLE;
                        } else { 
                            bacnetStats.errors_crc++; 
                            // z_log("[BACNET] Header CRC Error\n");
                            state = IDLE; 
                        }
                    }
                    break;
                case DATA: 
                    data_buf[data_idx++] = rx_byte; 
                    if (data_idx == data_len) state = CRC16_L; 
                    break;
                case CRC16_L: rx_crc16 = rx_byte; state = CRC16_H; break;
                case CRC16_H: 
                    rx_crc16 |= (rx_byte << 8);
                    if (calc_data_crc(data_buf, data_len) == rx_crc16) {
                        uint16_t pos = 0;
                        if (data_len > 2 && data_buf[0] == 0x01) { // Valid NPDU
                            pos += (data_buf[1] & 0x20) ? 2 : 2; 
                            if (pos + 3 < data_len && data_buf[pos] == 0x30) { // Complex Ack
                                pos += 3;
                                BACnetTag t;
                                while (decode_next_tag(data_buf, &pos, data_len, &t)) {
                                    if (t.isOpening && t.number == 3) {
                                        if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(50))) {
                                            if (!scan_done) {
                                                if (disc_step == DISC_LIST) {
                                                    if (current_scan_index == 0) {
                                                        total_objects = data_buf[pos+1]; bacnetStats.total_objects = total_objects;
                                                        current_scan_index = 1; bacnetStats.current_index = 1;
                                                        z_log("[BACNET] Discovery: Controller MAC %d has %d objects\n", header[2], total_objects);
                                                        BACnetDevice d; d.mac_address = header[2]; d.device_id = 0; d.discovery_done = false; d.enabled = true;
                                                        d.name = "ECB-203"; d.vendor = "Distech Controls";
                                                        bacnet_network_cache.push_back(d);
                                                    } else if (!bacnet_network_cache.empty()) {
                                                        uint16_t ot = (data_buf[pos+1] << 2) | (data_buf[pos+2] >> 6);
                                                        uint32_t oi = ((uint32_t)(data_buf[pos+2] & 0x3F) << 16) | (data_buf[pos+3] << 8) | data_buf[pos+4];
                                                        if (current_scan_index == 1) { target_device_id = oi; bacnet_network_cache[0].device_id = oi; }
                                                        BACnetObject obj; obj.type = ot; obj.instance = oi; obj.present_value = 0; obj.last_update = 0; obj.enabled = true;
                                                        obj.name = "Point_" + String(oi); 
                                                        bacnet_network_cache[0].objects.push_back(obj);
                                                        current_scan_index++; bacnetStats.current_index = current_scan_index;
                                                        if (current_scan_index > total_objects) { disc_step = DISC_NAME; disc_obj_ptr = 0; }
                                                    }
                                                } else if (!bacnet_network_cache.empty() && disc_obj_ptr < bacnet_network_cache[0].objects.size()) {
                                                    auto& o = bacnet_network_cache[0].objects[disc_obj_ptr];
                                                    BACnetTag val_tag;
                                                    if (decode_next_tag(data_buf, &pos, data_len, &val_tag)) {
                                                        if (disc_step == DISC_NAME && val_tag.number == 7) {
                                                            uint16_t slen = std::min((int)val_tag.len - 1, 32);
                                                            char n[33]; memcpy(n, &data_buf[pos+1], slen); n[slen] = '\0';
                                                            o.name = String(n); disc_step = DISC_VALUE;
                                                            z_log("[BACNET]  - %d:%lu Name: %s\n", o.type, (unsigned long)o.instance, n);
                                                        } else if (disc_step == DISC_VALUE && val_tag.number == 4) {
                                                            union { uint32_t i; float f; } u;
                                                            u.i = (data_buf[pos]<<24)|(data_buf[pos+1]<<16)|(data_buf[pos+2]<<8)|data_buf[pos+3];
                                                            o.present_value = u.f; o.last_update = millis();
                                                            disc_obj_ptr++; disc_step = DISC_NAME;
                                                            z_log("[BACNET]  - %d:%lu Value: %.2f\n", o.type, (unsigned long)o.instance, u.f);
                                                            if (disc_obj_ptr >= bacnet_network_cache[0].objects.size()) {
                                                                scan_done = true; save_device_objects(target_device_id);
                                                                z_log("[BACNET] Enhanced Discovery Complete.\n");
                                                            }
                                                        } else { disc_obj_ptr++; disc_step = DISC_NAME; }
                                                    }
                                                }
                                            } else if (!bacnet_network_cache.empty()) {
                                                BACnetTag val_tag;
                                                if (decode_next_tag(data_buf, &pos, data_len, &val_tag) && val_tag.number == 4) {
                                                    union { uint32_t i; float f; } u;
                                                    u.i = (data_buf[pos]<<24)|(data_buf[pos+1]<<16)|(data_buf[pos+2]<<8)|data_buf[pos+3];
                                                    if (current_poll_idx < bacnet_network_cache[0].objects.size()) {
                                                        bacnet_network_cache[0].objects[current_poll_idx].present_value = u.f;
                                                        bacnet_network_cache[0].objects[current_poll_idx].last_update = millis();
                                                    }
                                                }
                                            }
                                            xSemaphoreGive(cache_mutex);
                                        }
                                    }
                                    pos += t.len;
                                }
                            }
                        }
                    } else {
                        bacnetStats.errors_crc++;
                        // z_log("[BACNET] Data CRC Error\n");
                    }
                    state = IDLE; 
                    break;
            }
        }

        if (has_token && state == IDLE) {
            if (!waiting_for_reply && (bacnetStats.tokens_seen % 35 == 0)) {
                current_invoke_id++;
                bool req_sent = false;
                if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(20))) {
                    if (!scan_done) {
                        if (disc_step == DISC_LIST) {
                            uint8_t apdu[] = { 0x01, 0x00, 0x00, 0x05, current_invoke_id, 0x0C, 0x0C, 0x02, 0x00, 0x00, 0x00, 0x19, 0x4C, 0x29, current_scan_index };
                            if (target_device_id != 0x3FFFFF) { apdu[8]=(target_device_id>>16)&0xFF; apdu[9]=(target_device_id>>8)&0xFF; apdu[10]=target_device_id&0xFF; }
                            send_mstp_frame(4, 0x05, apdu, sizeof(apdu));
                            req_sent = true;
                        } else if (!bacnet_network_cache.empty()) {
                            auto& o = bacnet_network_cache[0].objects[disc_obj_ptr];
                            uint8_t pid = (disc_step == DISC_NAME) ? 77 : 85;
                            uint8_t apdu[] = { 0x01, 0x00, 0x00, 0x05, current_invoke_id, 0x0C, 0x0C, (uint8_t)((o.type>>2)&0xFF), (uint8_t)((o.type<<6)|(o.instance>>16)), (uint8_t)(o.instance>>8), (uint8_t)o.instance, 0x19, pid };
                            send_mstp_frame(4, 0x05, apdu, sizeof(apdu));
                            req_sent = true;
                        }
                    } else if (!bacnet_network_cache.empty()) {
                        current_poll_idx = (current_poll_idx + 1) % bacnet_network_cache[0].objects.size();
                        auto& o = bacnet_network_cache[0].objects[current_poll_idx];
                        if (o.enabled && bacnet_network_cache[0].enabled && o.type != 8) {
                            uint8_t apdu[] = { 0x01, 0x00, 0x00, 0x05, current_invoke_id, 0x0C, 0x0C, (uint8_t)((o.type>>2)&0xFF), (uint8_t)((o.type<<6)|(o.instance>>16)), (uint8_t)(o.instance>>8), (uint8_t)o.instance, 0x19, 0x55 };
                            send_mstp_frame(4, 0x05, apdu, sizeof(apdu));
                            req_sent = true;
                        }
                    }
                    xSemaphoreGive(cache_mutex);
                }
                if (req_sent) {
                    waiting_for_reply = true; 
                    state = MSTP_WAIT_TX_DONE;
                }
                token_acquired_time = millis();
            }
            
            uint32_t limit = waiting_for_reply ? 500 : 10;
            if (millis() - token_acquired_time > limit) {
                if (waiting_for_reply && (millis() - last_req_time > 1200)) {
                    waiting_for_reply = false;
                    z_log("[BACNET] Timeout waiting for reply\n");
                }
                if (!waiting_for_reply) {
                    uint8_t f[8] = { 0x55, 0xFF, 0x00, next_station, sysCfg.mac_address, 0, 0, 0 };
                    f[7] = calc_header_crc(&f[2], 5); uart_tx(f, 8);
                    has_token = false; bacnetStats.tokens_seen++;
                    state = MSTP_WAIT_TX_DONE;
                }
            }
        }
        
        if (millis() - last_rx_time > 3000) { 
            last_rx_time = millis(); 
            uint8_t f[8]={0x55,0xFF,0x01,4,sysCfg.mac_address,0,0,0}; f[7]=calc_header_crc(&f[2],5); 
            uart_tx(f,8); state = MSTP_WAIT_TX_DONE;
            z_log("[BACNET] No activity. Regenerating Token to MAC 4\n");
        }
        vTaskDelay(1);
    }
}

void setup_bacnet_engine() {
    cache_mutex = xSemaphoreCreateMutex();
    bacnet_job_queue = xQueueCreate(10, sizeof(BACnetJob));
    mqtt_publish_queue = xQueueCreate(30, sizeof(MQTTPublishJob));
    
    const uart_config_t uc = { 
        .baud_rate = 38400, 
        .data_bits = UART_DATA_8_BITS, 
        .parity = UART_PARITY_DISABLE, 
        .stop_bits = UART_STOP_BITS_1, 
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, 
        .rx_flow_ctrl_thresh = 122, 
        .source_clk = UART_SCLK_APB 
    };
    
    esp_err_t err = uart_driver_install(RS485_UART_PORT, 2048, 2048, 20, &uart_evt_queue, 0);
    if (err != ESP_OK) z_log("[BACNET] UART Driver Install Fail: 0x%x\n", err);
    
    err = uart_param_config(RS485_UART_PORT, &uc);
    if (err != ESP_OK) z_log("[BACNET] UART Param Config Fail: 0x%x\n", err);
    
    err = uart_set_pin(RS485_UART_PORT, TX_PIN, RX_PIN, RTS_PIN, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) z_log("[BACNET] UART Set Pin Fail: 0x%x\n", err);
    
    err = uart_set_mode(RS485_UART_PORT, UART_MODE_RS485_HALF_DUPLEX);
    if (err != ESP_OK) z_log("[BACNET] UART Set Mode Fail: 0x%x\n", err);
    
    xTaskCreatePinnedToCore(bacnet_task, "BACnet", 16384, NULL, 15, NULL, 1);
    z_log("[BACNET] Engine v4.5.29 Initialized on Core 1\n");
}

bool enqueue_bacnet_job(BACnetJob job) {
    if (bacnet_job_queue == NULL) return false;
    return xQueueSend(bacnet_job_queue, &job, 0) == pdTRUE;
}

bool enqueue_mqtt_publish(MQTTPublishJob pubJob) {
    if (mqtt_publish_queue == NULL) return false;
    return xQueueSend(mqtt_publish_queue, &pubJob, 0) == pdTRUE;
}
