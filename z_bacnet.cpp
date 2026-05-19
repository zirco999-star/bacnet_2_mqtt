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

// --- MSTP LOW LEVEL ---
static void uart_tx(const uint8_t *buffer, uint16_t length) {
    uart_write_bytes(RS485_UART_PORT, (const char*)buffer, length);
    uart_wait_tx_done(RS485_UART_PORT, pdMS_TO_TICKS(50));
    bacnetStats.ms_msgs_tx++;
}

static void send_token(uint8_t target_mac) {
    uint8_t f[8] = { 0x55, 0xFF, 0x00, target_mac, sysCfg.mac_address, 0, 0, 0 };
    f[7] = calc_header_crc(&f[2], 5);
    uart_tx(f, 8);
}

static void send_reply_to_pfm(uint8_t target_mac) {
    uint8_t f[8] = { 0x55, 0xFF, 0x02, target_mac, sysCfg.mac_address, 0, 0, 0 };
    f[7] = calc_header_crc(&f[2], 5);
    uart_tx(f, 8);
}

static void send_scan_req(uint8_t target_mac, uint32_t device_id, uint8_t index, uint8_t invoke_id) {
    uint8_t apdu[] = { 
        0x01, 0x00, // NPDU
        0x00, 0x05, invoke_id, 0x0C, 
        0x0C, 0x02, 0x3F, 0xFF, 0xFF, 
        0x19, 0x4C, 0x29, index 
    };
    if (device_id != 0x3FFFFF) {
        apdu[8] = (uint8_t)(device_id >> 16); 
        apdu[9] = (uint8_t)(device_id >> 8); 
        apdu[10] = (uint8_t)device_id;
    }
    uint16_t apdu_len = sizeof(apdu);
    uint8_t buffer[64];
    buffer[0]=0x55; buffer[1]=0xFF; buffer[2]=0x05; 
    buffer[3]=target_mac; buffer[4]=sysCfg.mac_address; 
    buffer[5]=(apdu_len>>8)&0xFF; buffer[6]=apdu_len&0xFF;
    buffer[7]=calc_header_crc(&buffer[2], 5);
    memcpy(&buffer[8], apdu, apdu_len);
    uint16_t crc16 = calc_data_crc(&buffer[8], apdu_len);
    buffer[8+apdu_len]=crc16&0xFF; buffer[8+apdu_len+1]=(crc16>>8)&0xFF;
    
    char h[48] = {0};
    for(int i=0; i<15; i++) sprintf(h + strlen(h), "%02X ", buffer[i]);
    z_log("[SCAN] TX Req: %s\n", h);
    
    uart_tx(buffer, 8 + apdu_len + 2);
}

// --- MSTP FSM ---
static void bacnet_task(void *pv) {
    uint8_t rx_byte; uint8_t header[6]; uint8_t header_idx = 0; uint8_t data_buf[512]; 
    uint16_t data_len = 0, data_idx = 0, rx_crc16 = 0;
    enum { IDLE, PREAMBLE_55, PREAMBLE_FF, HEADER, DATA, CRC16_L, CRC16_H } state = IDLE;
    
    uint8_t next_station = 4;
    uint32_t last_rx = millis();
    uint32_t last_heartbeat = 0;
    
    uint32_t target_device_id = 0x3FFFFF;
    uint8_t current_scan_index = 0;
    uint8_t total_objects = 0;
    bool scan_done = false;
    uint8_t current_invoke_id = 240;

    bool holding_token = false;
    uint32_t token_acquired_time = 0;
    bool request_sent_this_token = false;

    z_log("[BACNET] v4.2.24.1 - Verbose Discovery started\n");

    for (;;) {
        if (uart_read_bytes(RS485_UART_PORT, &rx_byte, 1, 0) > 0) {
            last_rx = millis();
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
                                holding_token = true;
                                token_acquired_time = millis();
                                request_sent_this_token = false;
                            } else if (type == 0x01 && dest == sysCfg.mac_address) {
                                vTaskDelay(pdMS_TO_TICKS(5));
                                send_reply_to_pfm(src);
                                next_station = src;
                            } else if (type == 0x06 && dest == sysCfg.mac_address) {
                                z_log("[SCAN] RX Data Type 06 (Len %d)\n", data_len);
                            }
                            if (data_len > 0 && data_len <= 512) { state = DATA; data_idx = 0; } else state = IDLE;
                        } else state = IDLE;
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
                        // Debug Hex
                        char h[48] = {0};
                        for(int i=0; i<data_len && i<32; i++) sprintf(h + strlen(h), "%02X ", data_buf[i]);
                        z_log("[SCAN] RX Data: %s\n", h);

                        for (int i=0; i<data_len-2; i++) {
                            if (data_buf[i] == 0x3E) {
                                if (current_scan_index == 0 && data_buf[i+1] == 0x21) {
                                    total_objects = data_buf[i+2];
                                    z_log("[SCAN] Found %d objects.\n", total_objects);
                                    current_scan_index = 1;
                                } else if (data_buf[i+1] == 0xC4) {
                                    uint16_t ot = (data_buf[i+2] << 2) | (data_buf[i+3] >> 6);
                                    uint32_t oi = ((uint32_t)(data_buf[i+3] & 0x3F) << 16) | (data_buf[i+4] << 8) | data_buf[i+5];
                                    z_log("[SCAN] %d/%d -> Type:%d Inst:%u\n", current_scan_index, total_objects, ot, oi);
                                    if (current_scan_index == 1 && ot == 8) target_device_id = oi;
                                    current_scan_index++;
                                    if (current_scan_index > total_objects) scan_done = true;
                                }
                                break;
                            }
                        }
                    }
                    state = IDLE; break;
            }
        }

        if (holding_token) {
            // Every 20 tokens OR if bus was idle and we just got token back
            if (!scan_done && !request_sent_this_token && (bacnetStats.tokens_seen % 20 == 0)) {
                current_invoke_id++;
                send_scan_req(4, target_device_id, current_scan_index, current_invoke_id);
                request_sent_this_token = true;
            }
            uint32_t wait_limit = request_sent_this_token ? 280 : 5;
            if (millis() - token_acquired_time > wait_limit) {
                send_token(next_station);
                holding_token = false;
                bacnetStats.tokens_seen++;
            }
        }

        if (millis() - last_heartbeat > 10000) {
            last_heartbeat = millis();
            z_log("[STATS] T:%u Progress:%d/%d\n", (unsigned int)bacnetStats.tokens_seen, current_scan_index, total_objects);
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
