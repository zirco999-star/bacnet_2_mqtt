#include "z_mstp.h"
#include <string.h>
#include "driver/gpio.h"

MSTP_Stats mstpStats = {0, 0, 0, 0, 0, 0};
QueueHandle_t mstp_tx_queue = NULL;

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

// --- EMISSION BAS NIVEAU ---
void uart_send_frame(uint8_t *buffer, uint16_t length) {
    uart_write_bytes(RS485_UART_PORT, (const char*)buffer, length);
    uart_wait_tx_done(RS485_UART_PORT, pdMS_TO_TICKS(50));
}

void send_pfm(uint8_t target_mac) {
    uint8_t f[8] = { 0x55, 0xFF, 0x01, target_mac, sysCfg.mac_address, 0x00, 0x00, 0x00 };
    f[7] = calc_mstp_header_crc(&f[2], 5);
    uart_send_frame(f, 8);
}

void send_reply_to_pfm(uint8_t target_mac) {
    uint8_t f[8] = { 0x55, 0xFF, 0x02, target_mac, sysCfg.mac_address, 0x00, 0x00, 0x00 };
    f[7] = calc_mstp_header_crc(&f[2], 5);
    uart_send_frame(f, 8);
}

void send_token(uint8_t target_mac) {
    uint8_t f[8] = { 0x55, 0xFF, 0x00, target_mac, sysCfg.mac_address, 0x00, 0x00, 0x00 };
    f[7] = calc_mstp_header_crc(&f[2], 5);
    uart_send_frame(f, 8);
}

// --- SERVICES APPLICATION (Push to Queue) ---
void send_who_is() {
    if (mstp_tx_queue == NULL) return;
    MSTP_TxFrame tx;
    tx.length = 18;
    tx.expect_reply = false;
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

void send_write_property(uint8_t target_mac, uint16_t obj_type, uint32_t obj_inst, float value) {
    if (mstp_tx_queue == NULL) return;
    MSTP_TxFrame tx;
    tx.expect_reply = true;
    int p = 0;
    tx.buffer[p++] = 0x55; tx.buffer[p++] = 0xFF;
    tx.buffer[p++] = 0x05; tx.buffer[p++] = target_mac; tx.buffer[p++] = sysCfg.mac_address;
    int len_pos = p; p += 2;
    tx.buffer[p++] = 0; 
    int data_start = p;
    tx.buffer[p++] = 0x01; tx.buffer[p++] = 0x04; 
    tx.buffer[p++] = 0x00; tx.buffer[p++] = 0x05; tx.buffer[p++] = 0x01; tx.buffer[p++] = 0x0F; 
    tx.buffer[p++] = 0x0C; 
    uint32_t oid = ((uint32_t)obj_type << 22) | (obj_inst & 0x3FFFFF);
    tx.buffer[p++] = (oid >> 24) & 0xFF; tx.buffer[p++] = (oid >> 16) & 0xFF; tx.buffer[p++] = (oid >> 8) & 0xFF; tx.buffer[p++] = oid & 0xFF;
    tx.buffer[p++] = 0x19; tx.buffer[p++] = 85; 
    tx.buffer[p++] = 0x3E; tx.buffer[p++] = 0x44; 
    uint8_t *v = (uint8_t *)&value;
    tx.buffer[p++] = v[3]; tx.buffer[p++] = v[2]; tx.buffer[p++] = v[1]; tx.buffer[p++] = v[0];
    tx.buffer[p++] = 0x3F; tx.buffer[p++] = 0x49; tx.buffer[p++] = 16; 
    uint16_t dlen = p - data_start;
    tx.buffer[len_pos] = (dlen >> 8) & 0xFF; tx.buffer[len_pos+1] = dlen & 0xFF;
    tx.buffer[7] = calc_mstp_header_crc(&tx.buffer[2], 5);
    uint16_t crc16 = calc_mstp_data_crc(&tx.buffer[data_start], dlen);
    tx.buffer[p++] = crc16 & 0xFF; tx.buffer[p++] = (crc16 >> 8) & 0xFF;
    tx.length = p;
    xQueueSend(mstp_tx_queue, &tx, 0);
}

void mstp_rt_task(void *pvParameters) {
    uint8_t rx_byte;
    uint8_t header[6];
    uint8_t header_idx = 0;
    uint8_t *data_buf = (uint8_t *)malloc(1024);
    uint16_t data_len = 0, data_idx = 0, received_crc16 = 0;
    enum { RX_IDLE, RX_PREAMBLE_55, RX_PREAMBLE_FF, RX_HEADER, RX_DATA, RX_CRC16_L, RX_CRC16_H } state = RX_IDLE;
    uint32_t last_rx_time = millis();
    uint8_t next_station = sysCfg.mac_address;
    uint8_t poll_station = (sysCfg.mac_address + 1) % (sysCfg.max_master + 1);

    for (;;) {
        if (uart_read_bytes(RS485_UART_PORT, &rx_byte, 1, pdMS_TO_TICKS(1)) > 0) {
            last_rx_time = millis(); mstpStats.raw_bytes++;
            switch (state) {
                case RX_IDLE: if (rx_byte == 0x55) state = RX_PREAMBLE_55; break;
                case RX_PREAMBLE_55: if (rx_byte == 0xFF) { state = RX_HEADER; header_idx = 0; } else if (rx_byte != 0x55) state = RX_IDLE; break;
                case RX_HEADER:
                    header[header_idx++] = rx_byte;
                    if (header_idx == 6) {
                        if (calc_mstp_header_crc(header, 5) == header[5]) {
                            mstpStats.frames_received++;
                            uint8_t ftype = header[0], dest = header[1], src = header[2];
                            data_len = (header[3] << 8) | header[4];
                            if (ftype == 0x00) {
                                mstpStats.tokens_seen++;
                                if (dest == sysCfg.mac_address) {
                                    MSTP_TxFrame tx;
                                    if (mstp_tx_queue && xQueueReceive(mstp_tx_queue, &tx, 0) == pdTRUE) {
                                        uart_send_frame(tx.buffer, tx.length);
                                    }
                                    send_token(next_station);
                                }
                            } else if (dest == sysCfg.mac_address || dest == 0xFF) {
                                if (ftype == 0x01 && dest == sysCfg.mac_address) send_reply_to_pfm(src);
                                else if (ftype == 0x02 && dest == sysCfg.mac_address) next_station = src;
                            }
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
            if (millis() - last_rx_time > 1000) {
                last_rx_time = millis();
                poll_station = (poll_station + 1) % (sysCfg.max_master + 1);
                if (poll_station == sysCfg.mac_address) poll_station = (poll_station + 1) % (sysCfg.max_master + 1);
                send_pfm(poll_station);
            }
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
    
    if (mstp_tx_queue == NULL) {
        mstp_tx_queue = xQueueCreate(10, sizeof(MSTP_TxFrame));
    }
    xTaskCreatePinnedToCore(mstp_rt_task, "MSTP_FSM", 4096, NULL, 10, NULL, 1);
}
