#include "z_mstp.h"
#include <string.h>
#include "driver/gpio.h"

MSTP_Stats mstpStats = {0, 0, 0, 0, 0, 0};

// --- ALGORITHMES CRC BACNET ---
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
        crc &= 0xFFFF;
    }
    return (~crc) & 0xFFFF;
}

void mstp_rt_task(void *pvParameters) {
    uint8_t rx_byte;
    uint8_t header[6];
    uint8_t header_idx = 0;
    uint8_t *data_buf = (uint8_t *)malloc(1024);
    uint16_t data_len = 0, data_idx = 0, received_crc16 = 0;
    enum { RX_IDLE, RX_PREAMBLE_55, RX_PREAMBLE_FF, RX_HEADER, RX_DATA, RX_CRC16_L, RX_CRC16_H } state = RX_IDLE;
    
    Serial.println("[MSTP] SNIFFER Task Started");

    for (;;) {
        if (uart_read_bytes(RS485_UART_PORT, &rx_byte, 1, pdMS_TO_TICKS(1)) > 0) {
            mstpStats.raw_bytes++;
            switch (state) {
                case RX_IDLE: if (rx_byte == 0x55) state = RX_PREAMBLE_55; break;
                case RX_PREAMBLE_55: if (rx_byte == 0xFF) { state = RX_HEADER; header_idx = 0; } else if (rx_byte != 0x55) state = RX_IDLE; break;
                case RX_HEADER:
                    header[header_idx++] = rx_byte;
                    if (header_idx == 6) {
                        if (calc_mstp_header_crc(header, 5) == header[5]) {
                            mstpStats.frames_received++;
                            data_len = (header[3] << 8) | header[4];
                            if (header[0] == 0x00) mstpStats.tokens_seen++;
                            if (data_len > 0 && data_len <= 1000) { state = RX_DATA; data_idx = 0; } else state = RX_IDLE;
                        } else { mstpStats.crc_header_errors++; state = RX_IDLE; }
                    }
                    break;
                case RX_DATA: data_buf[data_idx++] = rx_byte; if (data_idx == data_len) state = RX_CRC16_L; break;
                case RX_CRC16_L: received_crc16 = rx_byte; state = RX_CRC16_H; break;
                case RX_CRC16_H:
                    received_crc16 |= (rx_byte << 8);
                    if (calc_mstp_data_crc(data_buf, data_len) == received_crc16) mstpStats.data_frames++;
                    else mstpStats.crc_data_errors++;
                    state = RX_IDLE;
                    break;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

void setup_mstp() {
    const uart_config_t uc = { .baud_rate = 38400, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_APB };
    uart_driver_install(RS485_UART_PORT, 2048, 2048, 20, &uart_queue, 0);
    uart_param_config(RS485_UART_PORT, &uc);
    uart_set_pin(RS485_UART_PORT, TX_PIN, RX_PIN, RTS_PIN, UART_PIN_NO_CHANGE);
    uart_set_mode(RS485_UART_PORT, UART_MODE_RS485_HALF_DUPLEX);
    xTaskCreatePinnedToCore(mstp_rt_task, "MSTP_FSM", 4096, NULL, 10, NULL, 1);
}
