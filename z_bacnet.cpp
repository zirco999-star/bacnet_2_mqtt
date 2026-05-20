#include "z_bacnet.h"
#include <string.h>
#include <algorithm>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "z_network.h" 
#include "rom/ets_sys.h"

extern void z_log(const char* format, ...);

// --- GLOBALS ---
BACnet_Stats bacnetStats = {0, 0, 0, 0, 0};
std::vector<BACnetDevice> bacnet_network_cache;
QueueHandle_t bacnet_job_queue = NULL;

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

// --- ASN.1 MASTER PARSER (v4.2.28) ---
struct BACnetTag {
    uint32_t number;
    bool isContext;
    uint32_t len;
    bool isOpening;
    bool isClosing;
};

static bool decode_next_tag(const uint8_t *data, uint16_t *pos, uint16_t max_len, BACnetTag *tag) {
    if (*pos >= max_len) return false;
    uint8_t b = data[(*pos)++];
    tag->number = b >> 4;
    tag->isContext = (b & 0x08) != 0;
    uint8_t lvt = b & 0x07;
    
    // 1. Extended Tag ? (Clause 20.2.1.2)
    if (tag->number == 0x0F) tag->number = data[(*pos)++];
    
    // 2. Opening / Closing ? (Clause 20.2.1.4)
    tag->isOpening = (lvt == 6);
    tag->isClosing = (lvt == 7);
    
    // 3. Extended Length ? (Clause 20.2.1.3)
    if (lvt <= 4) tag->len = lvt;
    else if (lvt == 5) {
        tag->len = data[(*pos)++];
        if (tag->len == 254) { 
            tag->len = (data[*pos] << 8) | data[*pos+1]; 
            *pos += 2; 
        } else if (tag->len == 255) { 
            tag->len = ((uint32_t)data[*pos] << 24) | ((uint32_t)data[*pos+1] << 16) | ((uint32_t)data[*pos+2] << 8) | data[*pos+3]; 
            *pos += 4; 
        }
    } else tag->len = 0; // Opening/Closing has no data length in tag
    
    return true;
}

// --- MSTP LOW LEVEL ---
static void uart_tx(const uint8_t *buffer, uint16_t length) {
    uart_write_bytes(RS485_UART_PORT, (const char*)buffer, length);
    uart_wait_tx_done(RS485_UART_PORT, pdMS_TO_TICKS(100));
    bacnetStats.ms_msgs_tx++;
}

static void send_token(uint8_t target_mac) {
    uint8_t f[8] = { 0x55, 0xFF, 0x00, target_mac, sysCfg.mac_address, 0, 0, 0 };
    f[7] = calc_header_crc(&f[2], 5);
    uart_tx(f, 8);
}

static void send_pfm(uint8_t target_mac) {
    uint8_t f[8] = { 0x55, 0xFF, 0x01, target_mac, sysCfg.mac_address, 0, 0, 0 };
    f[7] = calc_header_crc(&f[2], 5);
    uart_tx(f, 8);
}

static void send_reply_to_pfm(uint8_t target_mac) {
    uint8_t f[8] = { 0x55, 0xFF, 0x02, target_mac, sysCfg.mac_address, 0, 0, 0 };
    f[7] = calc_header_crc(&f[2], 5);
    uart_tx(f, 8);
}

static void send_scan_req(uint8_t target_mac, uint32_t device_id, uint8_t index, uint8_t invoke_id) {
    uint8_t apdu[] = { 0x01, 0x00, 0x00, 0x05, invoke_id, 0x0C, 0x0C, 0x02, 0x3F, 0xFF, 0xFF, 0x19, 0x4C, 0x29, index };
    if (device_id != 0x3FFFFF) { 
        apdu[8]=(uint8_t)(device_id>>16); 
        apdu[9]=(uint8_t)(device_id>>8); 
        apdu[10]=(uint8_t)device_id; 
    }
    uint16_t apdu_len = sizeof(apdu);
    uint8_t buffer[64];
    buffer[0]=0x55; buffer[1]=0xFF; buffer[2]=0x05; buffer[3]=target_mac; buffer[4]=sysCfg.mac_address; 
    buffer[5]=(apdu_len>>8)&0xFF; buffer[6]=apdu_len&0xFF; buffer[7]=calc_header_crc(&buffer[2], 5);
    memcpy(&buffer[8], apdu, apdu_len);
    uint16_t crc16 = calc_data_crc(&buffer[8], apdu_len);
    buffer[8+apdu_len]=crc16&0xFF; buffer[8+apdu_len+1]=(crc16>>8)&0xFF;
    uart_tx(buffer, 8+apdu_len+2);
}

// --- MSTP FSM ---
static void bacnet_task(void *pv) {
    uint8_t rx_byte; uint8_t header[6]; uint8_t header_idx=0; uint8_t data_buf[512]; 
    uint16_t data_len=0, data_idx=0, rx_crc16=0;
    enum { IDLE, PREAMBLE_55, PREAMBLE_FF, HEADER, DATA, CRC16_L, CRC16_H } state = IDLE;
    
    uint8_t next_station = 4; 
    uint32_t last_rx_time = millis();
    uint32_t last_heartbeat = 0;
    bool has_token = false, scan_done = false, waiting_for_reply = false;
    uint32_t token_acquired_time = 0;
    
    uint32_t target_device_id = 0x3FFFFF;
    uint8_t total_objects = 0, current_scan_index = 0, current_invoke_id = 150;

    z_log("[BACNET] v4.2.28 - Master Discovery starting...\n");
    vTaskDelay(pdMS_TO_TICKS(2000));

    for (;;) {
        // --- 1. RX Logic ---
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
                            if (type == 0x00 && dest == sysCfg.mac_address) { 
                                has_token = true; 
                                token_acquired_time = millis(); 
                            }
                            else if (type == 0x01 && dest == sysCfg.mac_address) { 
                                vTaskDelay(pdMS_TO_TICKS(2)); 
                                send_reply_to_pfm(src); 
                            }
                            if (data_len > 0 && data_len <= 512) { state = DATA; data_idx = 0; } else state = IDLE;
                        } else state = IDLE;
                    }
                    break;
                case DATA: data_buf[data_idx++] = rx_byte; if (data_idx == data_len) state = CRC16_L; break;
                case CRC16_L: rx_crc16 = rx_byte; state = CRC16_H; break;
                case CRC16_H: 
                    rx_crc16 |= (rx_byte << 8);
                    if (calc_data_crc(data_buf, data_len) == rx_crc16) {
                        uint16_t pos = 2; // Skip NPDU
                        if (data_buf[pos] == 0x30) { // Complex ACK
                            pos += 3; // Skip 30 XX 0C
                            BACnetTag t;
                            while (decode_next_tag(data_buf, &pos, data_len, &t)) {
                                if (t.isOpening && t.number == 3) {
                                    // VALUE FIELD FOUND (The Context 3 of ReadProperty-ACK)
                                    if (current_scan_index == 0) {
                                        // Case index 0: Expecting Unsigned (Tag 0x21 or App Tag 2)
                                        uint8_t app_tag = data_buf[pos++];
                                        if ((app_tag & 0xF0) == 0x20) {
                                            total_objects = data_buf[pos++];
                                            z_log("[SCAN] Target confirmed: %d objects.\n", total_objects);
                                            current_scan_index = 1;
                                        }
                                    } else {
                                        // Case index N: Expecting ObjectIdentifier (Tag 0xC4 or App Tag 12)
                                        uint8_t app_tag = data_buf[pos++];
                                        if (app_tag == 0xC4) {
                                            uint16_t ot = (data_buf[pos] << 2) | (data_buf[pos+1] >> 6);
                                            uint32_t oi = ((uint32_t)(data_buf[pos+1] & 0x3F) << 16) | (data_buf[pos+2] << 8) | data_buf[pos+3];
                                            z_log("[SCAN] Found %d/%d -> Type:%d Inst:%u\n", current_scan_index, total_objects, (int)ot, (unsigned int)oi);
                                            if (current_scan_index == 1 && ot == 8) target_device_id = oi;
                                            current_scan_index++;
                                            if (current_scan_index > total_objects) {
                                                z_log("[SCAN] ALL OBJECTS DISCOVERED.\n");
                                                scan_done = true;
                                            }
                                        }
                                    }
                                    waiting_for_reply = false; 
                                    break; 
                                }
                                // Skip tag payload if not the one we want
                                pos += t.len;
                            }
                        }
                    }
                    state = IDLE; break;
            }
        }

        // --- 2. Token Logic ---
        if (has_token) {
            // Poll ECB every 50 tokens to keep things smooth
            if (!scan_done && !waiting_for_reply && (bacnetStats.tokens_seen % 50 == 0)) {
                current_invoke_id++;
                send_scan_req(4, target_device_id, current_scan_index, current_invoke_id);
                waiting_for_reply = true;
                token_acquired_time = millis(); // Refresh start time for the wait
            }

            // The YABE Hold: wait 280ms for reply, or pass token immediately (5ms)
            uint32_t wait_limit = (waiting_for_reply) ? 350 : 5;
            if (millis() - token_acquired_time > wait_limit) {
                vTaskDelay(pdMS_TO_TICKS(2));
                send_token(next_station);
                has_token = false;
                bacnetStats.tokens_seen++;
            }
        }

        // Recovery if bus goes silent
        if (millis() - last_rx_time > 1000) { last_rx_time = millis(); send_pfm(4); }

        if (millis() - last_heartbeat > 10000) {
            last_heartbeat = millis();
            z_log("[STATS] T:%u Ring:OK Scan:%d/%d\n", (unsigned int)bacnetStats.tokens_seen, current_scan_index, total_objects);
        }
        vTaskDelay(1);
    }
}

void setup_bacnet_engine() {
    const uart_config_t uc = { .baud_rate = 38400, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .rx_flow_ctrl_thresh = 122, .source_clk = UART_SCLK_APB };
    uart_driver_install(RS485_UART_PORT, 2048, 2048, 20, NULL, 0);
    uart_param_config(RS485_UART_PORT, &uc);
    uart_set_pin(RS485_UART_PORT, TX_PIN, RX_PIN, RTS_PIN, UART_PIN_NO_CHANGE);
    uart_set_mode(RS485_UART_PORT, UART_MODE_RS485_HALF_DUPLEX);
    bacnet_job_queue = xQueueCreate(100, sizeof(BACnetJob));
    xTaskCreatePinnedToCore(bacnet_task, "BACnet", 8192, NULL, 15, NULL, 1);
}
bool enqueue_bacnet_job(BACnetJob job) { return true; }
