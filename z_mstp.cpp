#include "z_mstp.h"
#include <string.h>
#include "driver/gpio.h"

MSTP_Stats mstpStats = {0, 0, 0, 0, 0, 0};
QueueHandle_t mstp_tx_queue = NULL;

// --- CRC BACNET ---
uint8_t calc_mstp_header_crc(uint8_t *data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        uint16_t crc16 = crc ^ (crc << 1) ^ (crc << 2) ^ (crc << 3) ^ (crc << 4) ^ (crc << 5) ^ (crc << 6) ^ (crc << 7);
        crc = (crc16 & 0xfe) ^ ((crc16 >> 8) & 1);
    }
    return (~crc) & 0xFF;
}

uint16_t calc_mstp_data_crc(uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        uint8_t crc_low = (crc & 0xff) ^ data[i];
        crc = (crc >> 8) ^ (crc_low << 8) ^ (crc_low << 3) ^ (crc_low << 12) ^ (crc_low >> 4) ^ (crc_low & 0x0f) ^ ((crc_low & 0x0f) << 7);
    }
    return (~crc) & 0xFFFF;
}

void uart_send_frame(uint8_t *buffer, uint16_t length) {
    uart_write_bytes(RS485_UART_PORT, (const char*)buffer, length);
    uart_wait_tx_done(RS485_UART_PORT, pdMS_TO_TICKS(20));
}

void send_token(uint8_t target_mac) {
    uint8_t f[8] = { 0x55, 0xFF, 0x00, target_mac, sysCfg.mac_address, 0x00, 0x00, 0x00 };
    f[7] = calc_mstp_header_crc(&f[2], 5);
    uart_send_frame(f, 8);
}

void send_reply_to_pfm(uint8_t target_mac) {
    uint8_t f[8] = { 0x55, 0xFF, 0x02, target_mac, sysCfg.mac_address, 0x00, 0x00, 0x00 };
    f[7] = calc_mstp_header_crc(&f[2], 5);
    uart_send_frame(f, 8);
}

void send_who_is() {
    if (mstp_tx_queue == NULL) return;
    MSTP_TxFrame tx;
    tx.length = 18;
    tx.buffer[0] = 0x55; tx.buffer[1] = 0xFF; tx.buffer[2] = 0x06; 
    tx.buffer[3] = 0xFF; tx.buffer[4] = sysCfg.mac_address; 
    tx.buffer[5] = 0x00; tx.buffer[6] = 0x08; 
    tx.buffer[7] = calc_mstp_header_crc(&tx.buffer[2], 5);
    uint8_t *p = &tx.buffer[8];
    *p++ = 0x01; *p++ = 0x20; *p++ = 0xFF; *p++ = 0xFF; *p++ = 0x00; *p++ = 0xFF; *p++ = 0x10; *p++ = 0x08;
    uint16_t crc16 = calc_mstp_data_crc(&tx.buffer[8], 8);
    tx.buffer[16] = crc16 & 0xFF; tx.buffer[17] = (crc16 >> 8) & 0xFF;
    xQueueSend(mstp_tx_queue, &tx, 0);
}

void send_i_am() {
    if (mstp_tx_queue == NULL) return;
    MSTP_TxFrame tx;
    uint8_t apdu[] = { 
        0x10, 0x00,                     // Unconfirmed-Req, I-Am
        0xC4, 0x02, 0x00, 0x04, 0xD2,   // Device ID Object: 1234
        0x21, 0x04, 0x00,               // Max APDU: 1024
        0x21, 0x01,                     // Segmentation: None
        0x21, 0x01                      // Vendor ID: 1
    };
    uint16_t apdu_len = sizeof(apdu);
    tx.length = 8 + apdu_len + 2;
    tx.buffer[0] = 0x55; tx.buffer[1] = 0xFF; tx.buffer[2] = 0x06; 
    tx.buffer[3] = 0xFF; tx.buffer[4] = sysCfg.mac_address; 
    tx.buffer[5] = (apdu_len >> 8); tx.buffer[6] = (apdu_len & 0xFF);
    tx.buffer[7] = calc_mstp_header_crc(&tx.buffer[2], 5);
    memcpy(&tx.buffer[8], apdu, apdu_len);
    uint16_t crc16 = calc_mstp_data_crc(apdu, apdu_len);
    tx.buffer[8+apdu_len] = crc16 & 0xFF;
    tx.buffer[8+apdu_len+1] = (crc16 >> 8) & 0xFF;
    xQueueSend(mstp_tx_queue, &tx, 0);
}

void mstp_rt_task(void *pvParameters) {
    uint8_t rx_byte;
    uint8_t header[6];
    uint8_t header_idx = 0;
    uint8_t *data_buf = (uint8_t *)malloc(1024);
    uint16_t data_len = 0, data_idx = 0;
    enum { RX_IDLE, RX_HEADER, RX_DATA, RX_CRC16_L, RX_CRC16_H } state = RX_IDLE;
    uint8_t next_station = sysCfg.mac_address;

    for (;;) {
        if (uart_read_bytes(RS485_UART_PORT, &rx_byte, 1, pdMS_TO_TICKS(1)) > 0) {
            mstpStats.raw_bytes++;
            switch (state) {
                case RX_IDLE: 
                    if (rx_byte == 0x55) break; 
                    if (rx_byte == 0xFF) { state = RX_HEADER; header_idx = 0; }
                    break;
                case RX_HEADER:
                    header[header_idx++] = rx_byte;
                    if (header_idx == 6) {
                        if (calc_mstp_header_crc(header, 5) == header[5]) {
                            mstpStats.frames_received++;
                            uint8_t ftype = header[0], dest = header[1], src = header[2];
                            data_len = (header[3] << 8) | header[4];
                            
                            if (ftype == 0x00) { // Token
                                mstpStats.tokens_seen++;
                                if (dest == sysCfg.mac_address) {
                                    MSTP_TxFrame tx;
                                    if (mstp_tx_queue && xQueueReceive(mstp_tx_queue, &tx, 0) == pdTRUE) {
                                        uart_send_frame(tx.buffer, tx.length);
                                    }
                                    send_token(next_station);
                                }
                            }
                            else if (ftype == 0x01 && dest == sysCfg.mac_address) {
                                send_reply_to_pfm(src);
                            }
                            else if (ftype == 0x02 && dest == sysCfg.mac_address) {
                                next_station = src;
                            }
                            
                            if (data_len > 0 && data_len <= 1000) { state = RX_DATA; data_idx = 0; } else state = RX_IDLE;
                        } else { mstpStats.crc_header_errors++; state = RX_IDLE; }
                    }
                    break;
                case RX_DATA: data_buf[data_idx++] = rx_byte; if (data_idx == data_len) state = RX_CRC16_L; break;
                case RX_CRC16_L: state = RX_CRC16_H; break;
                case RX_CRC16_H: mstpStats.data_frames++; state = RX_IDLE; break;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

void setup_mstp() {
    gpio_reset_pin(GPIO_NUM_47);
    gpio_set_direction(GPIO_NUM_47, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_47, 1); 
    const uart_config_t uc = { .baud_rate = 38400, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_APB };
    uart_driver_install(RS485_UART_PORT, 2048, 2048, 20, &uart_queue, 0);
    uart_param_config(RS485_UART_PORT, &uc);
    uart_set_pin(RS485_UART_PORT, TX_PIN, RX_PIN, RTS_PIN, UART_PIN_NO_CHANGE);
    uart_set_mode(RS485_UART_PORT, UART_MODE_RS485_HALF_DUPLEX);
    
    if (mstp_tx_queue == NULL) mstp_tx_queue = xQueueCreate(10, sizeof(MSTP_TxFrame));
    xTaskCreatePinnedToCore(mstp_rt_task, "MSTP_FSM", 4096, NULL, 10, NULL, 1);
    
    vTaskDelay(pdMS_TO_TICKS(5000));
    send_i_am();
}
