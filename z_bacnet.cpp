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

struct MSTP_TxFrame { 
    uint8_t buffer[512]; 
    uint16_t length; 
    bool expect_reply;
};
static QueueHandle_t mstp_tx_queue = NULL;

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
    uart_wait_tx_done(RS485_UART_PORT, pdMS_TO_TICKS(100));
    bacnetStats.ms_msgs_tx++;
    
    char hex[48] = {0};
    for(int i=0; i<length && i<15; i++) sprintf(hex + strlen(hex), "%02X ", buffer[i]);
    z_log("[TX] %s\n", hex);
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

static void send_mstp_data(uint8_t target_mac, uint8_t frame_type, const uint8_t* apdu, uint16_t apdu_len) {
    uint8_t buffer[512+10];
    buffer[0] = 0x55; buffer[1] = 0xFF; 
    buffer[2] = frame_type; buffer[3] = target_mac; buffer[4] = sysCfg.mac_address; 
    buffer[5] = (apdu_len >> 8) & 0xFF; buffer[6] = apdu_len & 0xFF;
    buffer[7] = calc_header_crc(&buffer[2], 5);
    memcpy(&buffer[8], apdu, apdu_len);
    uint16_t crc16 = calc_data_crc(&buffer[8], apdu_len);
    buffer[8 + apdu_len] = crc16 & 0xFF;
    buffer[8 + apdu_len + 1] = (crc16 >> 8) & 0xFF;
    uart_tx(buffer, 8 + apdu_len + 2);
}

// --- BACNET APPLICATION LAYER ---

static void send_i_am() {
    uint8_t apdu[32]; uint8_t i = 0;
    apdu[i++] = 0x01; apdu[i++] = 0x00; 
    apdu[i++] = 0x10; apdu[i++] = 0x00; // I-Am
    // Device ID encoding (Tag 12, length 4)
    apdu[i++] = (12 << 4) | 0; apdu[i++] = (OBJ_DEVICE >> 2) & 0xFF; // simplified for v4.2.14
    apdu[i++] = (sysCfg.device_id >> 16) & 0xFF; apdu[i++] = (sysCfg.device_id >> 8) & 0xFF; apdu[i++] = sysCfg.device_id & 0xFF;
    // ... basic I-Am ...
    send_mstp_data(0xFF, 0x06, apdu, i);
}

// --- MSTP FSM ---

static void bacnet_task(void *pv) {
    uint8_t rx_byte; uint8_t header[6]; uint8_t header_idx = 0; uint8_t data_buf[512]; 
    uint16_t data_len = 0, data_idx = 0, rx_crc16 = 0;
    enum { IDLE, PREAMBLE_55, PREAMBLE_FF, HEADER, DATA, CRC16_L, CRC16_H } state = IDLE;
    
    uint8_t next_station = (sysCfg.mac_address + 1) % 128; 
    uint32_t last_rx_time = millis();
    uint8_t poll_station = (sysCfg.mac_address + 1) % (sysCfg.max_master + 1);
    uint32_t last_heartbeat = 0;
    bool has_token = false;
    uint32_t pfm_sent_time = 0;
    bool awaiting_pfm_reply = false;
    bool ring_stable = false;

    z_log("[BACNET] Engine v4.2.14 started (MAC %d)\n", (int)sysCfg.mac_address);
    vTaskDelay(pdMS_TO_TICKS(1000));

    for (;;) {
        // --- RX LOGIC ---
        if (uart_read_bytes(RS485_UART_PORT, &rx_byte, 1, 0) > 0) {
            last_rx_time = millis();
            
            // PURE SNIFFER for non-preamble junk
            if (state == IDLE && rx_byte != 0x55 && rx_byte != 0xFF && rx_byte != 0x00) {
                // z_log("RX: %02X\n", rx_byte); // too noisy, only log if needed
            }

            switch (state) {
                case IDLE: if (rx_byte == 0x55) state = PREAMBLE_55; break;
                case PREAMBLE_55: if (rx_byte == 0xFF) { state = HEADER; header_idx = 0; } else state = IDLE; break;
                case HEADER:
                    header[header_idx++] = rx_byte;
                    if (header_idx == 6) {
                        if (calc_header_crc(header, 5) == header[5]) {
                            uint8_t type = header[0], dest = header[1], src = header[2];
                            data_len = (header[3] << 8) | header[4];
                            
                            if (type == 0x00) { // Token
                                bacnetStats.tokens_seen++;
                                if (dest == sysCfg.mac_address) {
                                    has_token = true;
                                    z_log("[RING] Token from %d\n", (int)src);
                                    if (src == next_station) ring_stable = true; // Minimal 2-node ring check
                                }
                            } else if (type == 0x01) { // PFM
                                if (dest == sysCfg.mac_address) {
                                    z_log("[RING] PFM from %d (Replying...)\n", (int)src);
                                    vTaskDelay(pdMS_TO_TICKS(2));
                                    send_reply_to_pfm(src);
                                    next_station = src; // Auto-set successor to whoever polls us
                                }
                            } else if (type == 0x02) { // Reply to PFM
                                if (dest == sysCfg.mac_address) {
                                    z_log("[RING] Found Successor: %d\n", (int)src);
                                    next_station = src;
                                    awaiting_pfm_reply = false;
                                    send_token(next_station);
                                    has_token = false;
                                }
                            }
                            if (data_len > 0 && data_len <= 512) { state = DATA; data_idx = 0; } else state = IDLE;
                        } else { 
                            bacnetStats.errors_crc++; state = IDLE; 
                        }
                    }
                    break;
                case DATA: data_buf[data_idx++] = rx_byte; if (data_idx == data_len) state = CRC16_L; break;
                case CRC16_L: rx_crc16 = rx_byte; state = CRC16_H; break;
                case CRC16_H: rx_crc16 |= (rx_byte << 8); state = IDLE; break;
            }
        }

        // --- TX LOGIC (When holding token) ---
        if (has_token) {
            vTaskDelay(pdMS_TO_TICKS(2)); // Tturnaround
            
            // PFM logic: every 50 tokens, try to find someone new
            if (bacnetStats.tokens_seen % 50 == 0 && !awaiting_pfm_reply) {
                poll_station = (poll_station + 1) % (sysCfg.max_master + 1);
                if (poll_station == sysCfg.mac_address) poll_station = (poll_station + 1) % (sysCfg.max_master + 1);
                z_log("[RING] Polling %d...\n", poll_station);
                send_pfm(poll_station);
                pfm_sent_time = millis();
                awaiting_pfm_reply = true;
            } else if (!awaiting_pfm_reply) {
                // Regular token pass
                send_token(next_station);
                has_token = false;
            }
        }

        // Timeout for PFM reply
        if (awaiting_pfm_reply && (millis() - pfm_sent_time > 40)) {
            awaiting_pfm_reply = false;
            send_token(next_station);
            has_token = false;
        }

        // Bus silence fallback: become master if bus is dead
        if (millis() - last_rx_time > 1500) {
            last_rx_time = millis();
            z_log("[BUS] Silence, reclaiming ring...\n");
            send_pfm(poll_station);
            pfm_sent_time = millis();
            awaiting_pfm_reply = true;
            has_token = false;
        }

        // Stats & I-Am
        if (millis() - last_heartbeat > 10000) {
            last_heartbeat = millis();
            z_log("[STATS] T:%u R:%u E:%u Next:%d\n", 
                (unsigned int)bacnetStats.tokens_seen, (unsigned int)bacnetStats.ms_msgs_rx, 
                (unsigned int)bacnetStats.errors_crc, (int)next_station);
            if (ring_stable) send_i_am();
        }
        
        vTaskDelay(1);
    }
}

void setup_bacnet_engine() {
    const uart_config_t uc = { 
        .baud_rate = 38400, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE, 
        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, 
        .rx_flow_ctrl_thresh = 122, .source_clk = UART_SCLK_APB 
    };
    uart_driver_install(RS485_UART_PORT, 2048, 2048, 20, NULL, 0);
    uart_param_config(RS485_UART_PORT, &uc);
    uart_set_pin(RS485_UART_PORT, TX_PIN, RX_PIN, RTS_PIN, UART_PIN_NO_CHANGE);
    uart_set_mode(RS485_UART_PORT, UART_MODE_RS485_HALF_DUPLEX);
    bacnet_job_queue = xQueueCreate(100, sizeof(BACnetJob));
    xTaskCreatePinnedToCore(bacnet_task, "BACnet", 8192, NULL, 15, NULL, 1);
}

bool enqueue_bacnet_job(BACnetJob job) {
    if (bacnet_job_queue == NULL) return false;
    return (xQueueSend(bacnet_job_queue, &job, 0) == pdTRUE);
}
