#ifndef Z_MSTP_H
#define Z_MSTP_H

#include "z_config.h"
#include "z_logger.h"
#include <driver/uart.h>

// Statistiques du bus
struct MSTP_Stats {
    uint32_t frames_received;
    uint32_t crc_header_errors;
    uint32_t crc_data_errors;
    uint32_t data_frames;
    uint32_t tokens_seen;
};

extern MSTP_Stats mstpStats;

void setup_mstp();
void mstp_rt_task(void *pvParameters);

// Utilitaires CRC
uint8_t calc_mstp_header_crc(uint8_t *data, size_t len);
uint16_t calc_mstp_data_crc(uint8_t *data, size_t len);

#endif
