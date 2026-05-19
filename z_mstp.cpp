#include "z_mstp.h"
#include <string.h>

MSTP_Stats mstpStats = {0, 0, 0, 0, 0};

// --- ALGORITHMES CRC BACnet MS/TP ---

// CRC8 pour le Header (Polynomial: x^8 + x^7 + x^2 + 1)
uint8_t calc_mstp_header_crc(uint8_t *data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80) crc = (crc << 1) ^ 0x01; // Polynomial simplifié pour MS/TP
            else crc <<= 1;
        }
    }
    return ~crc;
}

// CRC16 pour les Data (CRC-CCITT reversed)
uint16_t calc_mstp_data_crc(uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) crc = (crc >> 1) ^ 0x8408;
            else crc >>= 1;
        }
    }
    return ~crc;
}

// --- MACHINE À ÉTATS DE CAPTURE ---

enum MSTP_State {
    MSTP_IDLE,
    MSTP_PREAMBLE,
    MSTP_HEADER,
    MSTP_DATA,
    MSTP_CRC16
};

void mstp_rt_task(void *pvParameters) {
    uint8_t rx_byte;
    uint8_t header[6]; // Type, Dest, Src, LenH, LenL, CRC8
    uint8_t header_idx = 0;
    uint8_t *data_buf = (uint8_t *)ps_malloc(2048); // Utilisation de la PSRAM pour les gros paquets
    uint16_t data_len = 0;
    uint16_t data_idx = 0;
    uint16_t received_crc16 = 0;
    
    MSTP_State state = MSTP_IDLE;
    
    log_to_web(1, "MSTP: Task démarrée sur Core 1.");

    for (;;) {
        // Lecture octet par octet (plus précis pour une FSM)
        if (uart_read_bytes(RS485_UART_PORT, &rx_byte, 1, pdMS_TO_TICKS(10)) > 0) {
            
            switch (state) {
                case MSTP_IDLE:
                    if (rx_byte == 0x55) state = MSTP_PREAMBLE;
                    break;
                    
                case MSTP_PREAMBLE:
                    if (rx_byte == 0xFF) {
                        state = MSTP_HEADER;
                        header_idx = 0;
                    } else if (rx_byte != 0x55) {
                        state = MSTP_IDLE;
                    }
                    break;
                    
                case MSTP_HEADER:
                    header[header_idx++] = rx_byte;
                    if (header_idx == 6) {
                        // Vérification CRC8
                        uint8_t calc_crc = calc_mstp_header_crc(header, 5);
                        if (calc_crc == header[5]) {
                            mstpStats.frames_received++;
                            uint8_t frame_type = header[0];
                            data_len = (header[3] << 8) | header[4];
                            
                            if (frame_type == 0x00) mstpStats.tokens_seen++;
                            
                            if (data_len > 0 && data_len <= 2000) {
                                state = MSTP_DATA;
                                data_idx = 0;
                            } else {
                                // Trame sans data (Token, PFM, etc.)
                                if (frame_type != 0x00) {
                                    log_to_web(3, "MSTP: Trame Type 0x%02X recue.", frame_type);
                                }
                                state = MSTP_IDLE;
                            }
                        } else {
                            mstpStats.crc_header_errors++;
                            state = MSTP_IDLE;
                        }
                    }
                    break;
                    
                case MSTP_DATA:
                    data_buf[data_idx++] = rx_byte;
                    if (data_idx == data_len) {
                        state = MSTP_CRC16;
                        data_idx = 0;
                    }
                    break;
                    
                case MSTP_CRC16:
                    if (data_idx == 0) received_crc16 = rx_byte;
                    else {
                        received_crc16 |= (rx_byte << 8);
                        // Validation CRC16
                        uint16_t calc_crc = calc_mstp_data_crc(data_buf, data_len);
                        if (calc_crc == received_crc16) {
                            mstpStats.data_frames++;
                            log_to_web(2, "MSTP: Data Frame valide (%d octets) recue.", data_len);
                            // TODO: Envoyer vers Parser
                        } else {
                            mstpStats.crc_data_errors++;
                            log_to_web(0, "MSTP: Erreur CRC16 (Calc: %04X, Recu: %04X)", calc_crc, received_crc16);
                        }
                        state = MSTP_IDLE;
                    }
                    data_idx++;
                    break;
            }
        }
        
        // Log périodique des stats
        static uint32_t last_stats = 0;
        if (millis() - last_stats > 10000) {
            last_stats = millis();
            log_to_web(2, "Bus MS/TP: %u frames, %u tokens, %u data, %u CRC_Err", 
                       mstpStats.frames_received, mstpStats.tokens_seen, mstpStats.data_frames, mstpStats.crc_header_errors);
        }
    }
}

void setup_mstp() {
    const uart_config_t uc = { 
        .baud_rate = 38400, 
        .data_bits = UART_DATA_8_BITS, 
        .parity = UART_PARITY_DISABLE, 
        .stop_bits = UART_STOP_BITS_1, 
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, 
        .source_clk = UART_SCLK_DEFAULT 
    };
    
    // Installation du driver avec queue pour les évènements UART
    uart_driver_install(RS485_UART_PORT, 2048, 2048, 20, &uart_queue, 0);
    uart_param_config(RS485_UART_PORT, &uc);
    
    // Configuration des pins et du mode Half Duplex
    uart_set_pin(RS485_UART_PORT, TX_PIN, RX_PIN, RTS_PIN, UART_PIN_NO_CHANGE);
    uart_set_mode(RS485_UART_PORT, UART_MODE_RS485_HALF_DUPLEX);
    
    // Tâche sur le Core 1 pour ne pas gêner le WiFi sur le Core 0
    xTaskCreatePinnedToCore(mstp_rt_task, "MSTP_FSM", 8192, NULL, 15, NULL, 1);
}
