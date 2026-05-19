#ifndef Z_BACNET_H
#define Z_BACNET_H

#include "z_config.h"

struct BACnet_Stats {
    uint32_t ms_msgs_rx;
    uint32_t ms_msgs_tx;
    uint32_t tokens_seen;
    uint32_t pfm_replies;
    uint32_t errors_crc;
};

extern BACnet_Stats bacnetStats;

void setup_bacnet_engine();
void send_bacnet_who_is();
void send_bacnet_i_am();
#endif
