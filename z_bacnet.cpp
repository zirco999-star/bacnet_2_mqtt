#include "z_bacnet.h"
#include <string.h>
#include <algorithm>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "z_network.h" 
#include "rom/ets_sys.h"

extern void z_log(const char* format, ...);

// --- GLOBALS ---
BACnet_Stats bacnetStats = {0, 0, 0, 0, 0, 0, 0};
std::vector<BACnetDevice> bacnet_network_cache;
QueueHandle_t bacnet_job_queue = NULL;
QueueHandle_t mqtt_publish_queue = NULL;
QueueHandle_t uart_evt_queue = NULL;

// --- NVS PERSISTENCE (Multi-Device, Thread-Safe) ---
static void save_cache_to_nvs() {
    for (auto& dev : bacnet_network_cache) {
        char ns[16]; snprintf(ns, sizeof(ns), "dev_%lu", (unsigned long)dev.device_id);
        Preferences prefs;
        if (prefs.begin(ns, false)) {
            BACnetPersistenceDev data;
            data.device_id = dev.device_id;
            data.enabled = dev.enabled;
            strlcpy(data.name, dev.name.c_str(), 32);
            strlcpy(data.vendor, dev.vendor.c_str(), 32);
            data.count = (uint8_t)std::min((int)dev.objects.size(), 100);
            for (int i = 0; i < data.count; i++) {
                data.objects[i].val = ((uint32_t)dev.objects[i].type << 25) | (dev.objects[i].instance & 0x1FFFFFF);
                strlcpy(data.objects[i].name, dev.objects[i].name.c_str(), 24);
                data.objects[i].poll = dev.objects[i].enabled;
            }
            prefs.putBytes("blob", &data, sizeof(data));
            prefs.end();
        }
    }
}

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

struct BACnetTag { uint32_t number; bool isContext; uint32_t len; bool isOpening; bool isClosing; };
static bool decode_next_tag(const uint8_t *data, uint16_t *pos, uint16_t max_len, BACnetTag *tag) {
    if (*pos >= max_len) return false;
    uint8_t b = data[(*pos)++];
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
    uint8_t next_station = 4; uint32_t last_rx_time = millis(); uint32_t last_heartbeat = 0;
    bool has_token = false, scan_done = false, waiting_for_reply = false;
    uint32_t token_acquired_time = 0, last_req_time = 0;
    uint32_t target_device_id = 0x3FFFFF;
    uint8_t total_objects = 0, current_scan_index = 0, current_invoke_id = 10, current_poll_idx = 0;
    uart_event_t event;

    z_log("[BACNET] Engine v4.5.20 - Platinum Ready\n");

    for (;;) {
        if (xQueueReceive(uart_evt_queue, (void *)&event, 0) == pdTRUE) {
            if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) uart_flush_input(RS485_UART_PORT);
        }

        // FIX: Polling non-bloquant du registre de statut UART (universel Core 3.x)
        if (state == MSTP_WAIT_TX_DONE) {
            if (uart_wait_tx_done(RS485_UART_PORT, 0) == ESP_OK) {
                last_req_time = millis(); // T_reply démarre EXACTEMENT maintenant
                state = IDLE;
            }
        }

        if (uart_read_bytes(RS485_UART_PORT, &rx_byte, 1, 0) > 0) {
            last_rx_time = millis();
            switch (state) {
                case IDLE: if (rx_byte == 0x55) state = PREAMBLE_55; break;
                case PREAMBLE_55: if (rx_byte == 0xFF) { state = HEADER; header_idx = 0; } else state = IDLE; break;
                case HEADER:
                    header[header_idx++] = rx_byte;
                    if (header_idx == 6) {
                        if (calc_header_crc(header, 5) == header[5]) {
                            uint8_t type = header[0], dest = header[1], src = header[2];
                            data_len = (header[3] << 8) | header[4];
                            if (type == 0x00 && dest == sysCfg.mac_address) { has_token = true; token_acquired_time = millis(); }
                            else if (type == 0x01 && dest == sysCfg.mac_address) { vTaskDelay(pdMS_TO_TICKS(2)); 
                                uint8_t f[8] = { 0x55, 0xFF, 0x02, src, sysCfg.mac_address, 0, 0, 0 };
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
                        uint16_t pos = 2;
                        if (data_buf[pos] == 0x30) {
                            pos += 3; BACnetTag t;
                            while (decode_next_tag(data_buf, &pos, data_len, &t)) {
                                if (t.isOpening && t.number == 3) {
                                    if (!scan_done) {
                                        if (current_scan_index == 0) {
                                            total_objects = data_buf[pos+1]; bacnetStats.total_objects = total_objects;
                                            current_scan_index = 1; bacnetStats.current_index = 1;
                                            BACnetDevice d; d.mac_address = 4; d.device_id = 0; d.discovery_done = false; d.enabled = true;
                                            d.name = "ECB-203"; d.vendor = "Distech Controls";
                                            bacnet_network_cache.push_back(d);
                                        } else {
                                            uint16_t ot = (data_buf[pos+1] << 2) | (data_buf[pos+2] >> 6);
                                            uint32_t oi = ((uint32_t)(data_buf[pos+2] & 0x3F) << 16) | (data_buf[pos+3] << 8) | data_buf[pos+4];
                                            if (current_scan_index == 1) { target_device_id = oi; bacnet_network_cache[0].device_id = oi; }
                                            BACnetObject obj; obj.type = ot; obj.instance = oi; obj.present_value = 0; obj.last_update = 0; obj.enabled = true;
                                            obj.name = "Point_" + String(oi); 
                                            bacnet_network_cache[0].objects.push_back(obj);
                                            current_scan_index++; bacnetStats.current_index = current_scan_index;
                                            if (current_scan_index > total_objects) { 
                                                scan_done = true; 
                                                extern void save_device_objects(uint32_t device_id);
                                                save_device_objects(target_device_id); 
                                            }
                                        }
                                    } else {
                                        uint8_t tag = data_buf[pos++];
                                        if (tag == 0x44) {
                                            union { uint32_t i; float f; } u;
                                            u.i = (data_buf[pos]<<24)|(data_buf[pos+1]<<16)|(data_buf[pos+2]<<8)|data_buf[pos+3];
                                            if (current_poll_idx < bacnet_network_cache[0].objects.size()) {
                                                bacnet_network_cache[0].objects[current_poll_idx].present_value = u.f;
                                                bacnet_network_cache[0].objects[current_poll_idx].last_update = millis();
                                            }
                                        }
                                    }
                                    waiting_for_reply = false; break;
                                }
                                pos += t.len;
                            }
                        }
                    }
                    state = IDLE; break;
            }
        }

        if (has_token && state == IDLE) {
            if (!waiting_for_reply && (bacnetStats.tokens_seen % 30 == 0)) {
                current_invoke_id++;
                if (!scan_done) {
                    uint8_t apdu[] = { 0x01, 0x00, 0x00, 0x05, current_invoke_id, 0x0C, 0x0C, 0x02, 0x00, 0x00, 0x00, 0x19, 0x4C, 0x29, current_scan_index };
                    if (target_device_id == 0x3FFFFF) { apdu[8]=0x3F; apdu[9]=0xFF; apdu[10]=0xFF; }
                    else { apdu[8]=(uint8_t)(target_device_id>>16); apdu[9]=(uint8_t)(target_device_id>>8); apdu[10]=(uint8_t)target_device_id; }
                    send_mstp_frame(4, 0x05, apdu, sizeof(apdu));
                    waiting_for_reply = true; state = MSTP_WAIT_TX_DONE;
                } else if (!bacnet_network_cache.empty()) {
                    current_poll_idx = (current_poll_idx + 1) % bacnet_network_cache[0].objects.size();
                    auto& o = bacnet_network_cache[0].objects[current_poll_idx];
                    if (o.enabled && bacnet_network_cache[0].enabled) {
                        uint8_t apdu[] = { 0x01, 0x00, 0x00, 0x05, current_invoke_id, 0x0C, 0x0C, (uint8_t)((o.type>>2)&0xFF), (uint8_t)((o.type<<6)|(o.instance>>16)), (uint8_t)(o.instance>>8), (uint8_t)o.instance, 0x19, 0x55 };
                        send_mstp_frame(4, 0x05, apdu, sizeof(apdu));
                        waiting_for_reply = true; state = MSTP_WAIT_TX_DONE;
                    }
                }
                token_acquired_time = millis();
            }
            uint32_t limit = waiting_for_reply ? 350 : 5;
            if (millis() - token_acquired_time > limit) {
                if (waiting_for_reply && (millis() - last_req_time > 800)) waiting_for_reply = false;
                if (!waiting_for_reply) {
                    uint8_t f[8] = { 0x55, 0xFF, 0x00, next_station, sysCfg.mac_address, 0, 0, 0 };
                    f[7] = calc_header_crc(&f[2], 5); uart_tx(f, 8);
                    has_token = false; bacnetStats.tokens_seen++;
                    state = MSTP_WAIT_TX_DONE;
                }
            }
        }
        if (millis() - last_rx_time > 1000) { 
            last_rx_time = millis(); 
            uint8_t f[8]={0x55,0xFF,0x01,4,sysCfg.mac_address,0,0,0}; f[7]=calc_header_crc(&f[2],5); 
            uart_tx(f,8); state = MSTP_WAIT_TX_DONE;
        }
        vTaskDelay(1);
    }
}

void setup_bacnet_engine() {
    bacnet_job_queue = xQueueCreate(10, sizeof(BACnetJob));
    mqtt_publish_queue = xQueueCreate(30, sizeof(MQTTPublishJob));
    const uart_config_t uc = { .baud_rate = 38400, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .rx_flow_ctrl_thresh = 122, .source_clk = UART_SCLK_APB };
    uart_driver_install(RS485_UART_PORT, 2048, 2048, 20, &uart_evt_queue, 0);
    uart_param_config(RS485_UART_PORT, &uc);
    uart_set_pin(RS485_UART_PORT, TX_PIN, RX_PIN, RTS_PIN, UART_PIN_NO_CHANGE);
    uart_set_mode(RS485_UART_PORT, UART_MODE_RS485_HALF_DUPLEX);
    xTaskCreatePinnedToCore(bacnet_task, "BACnet", 8192, NULL, 15, NULL, 1);
}
bool enqueue_bacnet_job(BACnetJob job) {
    if (bacnet_job_queue == NULL) return false;
    return xQueueSend(bacnet_job_queue, &job, 0) == pdTRUE;
}
bool enqueue_mqtt_publish(MQTTPublishJob pubJob) {
    if (mqtt_publish_queue == NULL) return false;
    return xQueueSend(mqtt_publish_queue, &pubJob, 0) == pdTRUE;
}
