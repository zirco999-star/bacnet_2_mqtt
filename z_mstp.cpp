#include "z_mstp.h"

uint8_t calc_header_crc_impl(uint8_t *data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        uint16_t crc16 = crc ^ (crc << 1) ^ (crc << 2) ^ (crc << 3) ^ (crc << 4) ^ (crc << 5) ^ (crc << 6) ^ (crc << 7);
        crc = (crc16 & 0xfe) ^ ((crc16 >> 8) & 1);
    }
    return (~crc) & 0xFF;
}

void mstp_rt_task(void *pvParameters) {
    uart_event_t event; uint8_t data[8];
    for (;;) {
        if (xQueueReceive(uart_queue, (void *)&event, pdMS_TO_TICKS(500))) {
            if (event.type == UART_DATA) {
                int len = uart_read_bytes(RS485_UART_PORT, data, 8, 0);
                if (len == 8 && data[0] == 0x55 && data[1] == 0xFF) {
                    if (calc_header_crc_impl(&data[2], 5) == data[7] && data[3] == sysCfg.mac_address) {
                        if (data[2] == 0x00) {
                            uint8_t next = (sysCfg.mac_address + 1) % 128;
                            uint8_t t[8] = { 0x55, 0xFF, 0x00, next, sysCfg.mac_address, 0x00, 0x00, 0x00 };
                            t[7] = calc_header_crc_impl(&t[2], 5);
                            uart_write_bytes(RS485_UART_PORT, (const char*)t, 8);
                        }
                    }
                }
            }
        }
    }
}

void setup_mstp() {
    const uart_config_t uc = { .baud_rate = 38400, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT };
    uart_driver_install(RS485_UART_PORT, 2048, 2048, 20, &uart_queue, 0);
    uart_param_config(RS485_UART_PORT, &uc);
    uart_set_pin(RS485_UART_PORT, TX_PIN, RX_PIN, RTS_PIN, UART_PIN_NO_CHANGE);
    uart_set_mode(RS485_UART_PORT, UART_MODE_RS485_HALF_DUPLEX);
    xTaskCreatePinnedToCore(mstp_rt_task, "MSTP", 8192, NULL, 10, NULL, 1);
}
