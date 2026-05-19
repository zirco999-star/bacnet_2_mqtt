#include "z_bacnet.h"
#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"

BACnet_Stats bacnetStats = {0, 0, 0, 0, 0};
QueueHandle_t mstp_tx_queue = NULL;

struct MSTP_TxFrame { uint8_t buffer[512]; uint16_t length; };

uint8_t calc_header_crc(uint8_t *data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        uint16_t crc16 = crc ^ (crc << 1) ^ (crc << 2) ^ (crc << 3) ^ (crc << 4) ^ (crc << 5) ^ (crc << 6) ^ (crc << 7);
        crc = (crc16 & 0xfe) ^ ((crc16 >> 8) & 1);
    }
    return (~crc) & 0xFF;
}

void uart_tx(uint8_t *buffer, uint16_t length) {
    uart_write_bytes(RS485_UART_PORT, (const char*)buffer, length);
    uart_wait_tx_done(RS485_UART_PORT, pdMS_TO_TICKS(10)); 
}

void bacnet_task(void *pv) {
    uint8_t rx_byte;
    uint8_t header[6];
    uint8_t header_idx = 0;
    enum { IDLE, HEADER } state = IDLE;
    uint8_t next_station = sysCfg.mac_address;

    for (;;) {
        if (uart_read_bytes(RS485_UART_PORT, &rx_byte, 1, pdMS_TO_TICKS(1)) > 0) {
            switch (state) {
                case IDLE: if (rx_byte == 0xFF) { state = HEADER; header_idx = 0; } break;
                case HEADER:
                    header[header_idx++] = rx_byte;
                    if (header_idx == 6) {
                        if (calc_header_crc(header, 5) == header[5]) {
                            uint8_t type = header[0], dest = header[1], src = header[2];
                            if (type == 0x00 && dest == sysCfg.mac_address) { // Token
                                MSTP_TxFrame tx;
                                if (xQueueReceive(mstp_tx_queue, &tx, 0) == pdTRUE) uart_tx(tx.buffer, tx.length);
                                uint8_t f[8] = { 0x55, 0xFF, 0x00, next_station, sysCfg.mac_address, 0, 0, 0 };
                                f[7] = calc_header_crc(&f[2], 5);
                                uart_tx(f, 8);
                                bacnetStats.tokens_seen++;
                            } else if (type == 0x01 && dest == sysCfg.mac_address) { // PFM
                                uint8_t f[8] = { 0x55, 0xFF, 0x02, src, sysCfg.mac_address, 0, 0, 0 };
                                f[7] = calc_header_crc(&f[2], 5);
                                uart_tx(f, 8);
                                bacnetStats.pfm_replies++;
                            } else if (type == 0x02 && dest == sysCfg.mac_address) next_station = src;
                        }
                        state = IDLE;
                    }
                    break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void setup_bacnet_engine() {
    gpio_set_direction((gpio_num_t)47, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)47, 1); 
    const uart_config_t uc = { .baud_rate = 38400, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_APB };
    uart_driver_install(RS485_UART_PORT, 1024, 1024, 20, NULL, 0);
    uart_param_config(RS485_UART_PORT, &uc);
    uart_set_pin(RS485_UART_PORT, TX_PIN, RX_PIN, RTS_PIN, UART_PIN_NO_CHANGE);
    uart_set_mode(RS485_UART_PORT, UART_MODE_RS485_HALF_DUPLEX);
    
    mstp_tx_queue = xQueueCreate(10, sizeof(MSTP_TxFrame));
    xTaskCreatePinnedToCore(bacnet_task, "BACnet", 4096, NULL, 10, NULL, 1);
}

void send_bacnet_who_is() {}
void send_bacnet_i_am() {}
