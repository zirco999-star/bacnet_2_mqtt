#ifndef Z_MSTP_H
#define Z_MSTP_H

#include "z_config.h"
#include "z_logger.h"
#include <driver/uart.h>

void setup_mstp();
void mstp_rt_task(void *pvParameters);
uint8_t calc_header_crc_impl(uint8_t *data, size_t len);

#endif
