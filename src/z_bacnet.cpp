#include "z_bacnet.h"
#include <string.h>
#include <algorithm>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "z_network.h" 
#include "rom/ets_sys.h"
#include "z_mqtt.h"
#include "z_nvs.h"


BACnet_Stats bacnetStats = {0, 0, 0, 0, 0, 0, 0};
std::vector<BACnetDevice> bacnet_network_cache;
QueueHandle_t bacnet_job_queue = NULL;
QueueHandle_t uart_evt_queue = NULL;
SemaphoreHandle_t cache_mutex = NULL;

static uint8_t last_apdu[512];
static uint16_t last_apdu_len = 0;
static uint8_t last_sent_invoke_id = 0;
static uint8_t next_invoke_id = 10;
static uint8_t retry_count = 0;
static BACnetJob pending_write_job;

// --- UTILS CRC ---
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

static bool validate_rx_header_crc(const uint8_t *header_and_crc) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < 6; i++) { 
        crc ^= header_and_crc[i];
        uint16_t crc16 = crc ^ (crc << 1) ^ (crc << 2) ^ (crc << 3) ^ (crc << 4) ^ (crc << 5) ^ (crc << 6) ^ (crc << 7);
        crc = (crc16 & 0xfe) ^ ((crc16 >> 8) & 1);
    }
    return (crc == 0x55);
}

static bool validate_rx_data_crc(const uint8_t *data_and_crc, size_t total_len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < total_len; i++) {
        uint8_t crc_low = (crc & 0xff) ^ data_and_crc[i];
        crc = (crc >> 8) ^ (crc_low << 8) ^ (crc_low << 3) ^ (crc_low << 12) ^ (crc_low >> 4) ^ (crc_low & 0x0f) ^ ((crc_low & 0x0f) << 7);
        crc &= 0xFFFF;
    }
    return (crc == 0xF0B8);
}

// --- ASN.1 DECODING ---
struct BACnetTag { uint32_t number; bool isContext; uint32_t len; bool isOpening; bool isClosing; uint8_t tag_raw; };
static bool decode_next_tag(const uint8_t *data, uint16_t *pos, uint16_t max_len, BACnetTag *tag) {
    if (*pos >= max_len) return false;
    uint8_t b = data[(*pos)++];
    tag->tag_raw = b; tag->number = b >> 4; tag->isContext = (b & 0x08) != 0;
    uint8_t lvt = b & 0x07;
    if (tag->number == 0x0F) tag->number = data[(*pos)++];
    tag->isOpening = (lvt == 6); tag->isClosing = (lvt == 7);
    if (lvt <= 4) tag->len = lvt;
    else if (lvt == 5) {
        tag->len = data[(*pos)++];
        if (tag->len == 254) { tag->len = (data[*pos] << 8) | data[*pos+1]; *pos += 2; }
        else if (tag->len == 255) { tag->len = ((uint32_t)data[*pos] << 24) | ((uint32_t)data[*pos+1] << 16) | ((uint32_t)data[*pos+2] << 8) | data[*pos+3]; *pos += 4; }
    } else tag->len = 0;
    return true;
}

// --- UART & MS/TP FRAMING ---
void bacnet_abort_current_transaction() {
    last_sent_invoke_id = 0xFF;
    // waiting_for_reply = false; // Ne pas toucher à l'état de la FSM, juste l'ID
    z_log(LOG_WARN,"BACNET","Transaction Aborted (Manual Override)\n");
}

static void uart_tx(const uint8_t *buffer, uint16_t length) {
    uart_write_bytes(RS485_UART_PORT, (const char*)buffer, length);
}

static void send_mstp_frame(uint8_t target_mac, uint8_t type, const uint8_t* apdu, uint16_t len) {
    if (apdu != NULL && len > 0) {
        memcpy(last_apdu, apdu, len);
        last_apdu_len = len;
        // L'Invoke ID est à l'index 4 de l'APDU car il y a 4 octets de NPDU (01 04 02 73)
        if (len > 4) last_sent_invoke_id = apdu[4];
        else last_sent_invoke_id = 0xFF; 
    }
    uint8_t buffer[512+10];
    buffer[0]=0x55; buffer[1]=0xFF; buffer[2]=type; buffer[3]=target_mac; buffer[4]=sysCfg.mac_address;
    buffer[5]=(len>>8)&0xFF; buffer[6]=len&0xFF;
    buffer[7]=calc_header_crc(&buffer[2], 5);
    if (type == 0x05 || type == 0x06) bacnetStats.ms_msgs_tx++;
    if (len > 0) {
        memcpy(&buffer[8], apdu, len);
        uint16_t crc16 = calc_data_crc(&buffer[8], len);
        buffer[8+len]=crc16&0xFF; buffer[8+len+1]=(crc16>>8)&0xFF;
        uart_tx(buffer, 8+len+2);
    } else { uart_tx(buffer, 8); }
}

uint16_t build_read_property_apdu(uint8_t* buffer, uint8_t invoke_id, uint16_t obj_type, uint32_t obj_instance, uint8_t property_id, int32_t array_index) {
    uint16_t len = 0;
    buffer[len++] = 0x01; buffer[len++] = 0x04; buffer[len++] = 0x02; buffer[len++] = 0x73;
    buffer[len++] = invoke_id; buffer[len++] = 0x0C; buffer[len++] = 0x0C;
    uint32_t oid = ((uint32_t)obj_type << 22) | (obj_instance & 0x3FFFFF);
    buffer[len++] = (oid >> 24) & 0xFF; buffer[len++] = (oid >> 16) & 0xFF; buffer[len++] = (oid >> 8) & 0xFF; buffer[len++] = oid & 0xFF;
    buffer[len++] = 0x19; buffer[len++] = property_id;
    if (array_index >= 0) { 
        if (array_index <= 255) {
            buffer[len++] = 0x29; 
            buffer[len++] = (uint8_t)array_index;
        } else {
            buffer[len++] = 0x2A; 
            buffer[len++] = (array_index >> 8) & 0xFF;
            buffer[len++] = array_index & 0xFF;
        }
    }
    return len;
}

uint16_t build_write_property_name_apdu(uint8_t* buffer, uint8_t invoke_id, uint16_t obj_type, uint32_t obj_instance, const char* new_name) {
    if (new_name == NULL) return 0;
    uint16_t len = 0;
    buffer[len++] = 0x01; buffer[len++] = 0x04; buffer[len++] = 0x02; buffer[len++] = 0x03;
    buffer[len++] = invoke_id; buffer[len++] = 0x0F; buffer[len++] = 0x0C;
    uint32_t oid = ((uint32_t)obj_type << 22) | (obj_instance & 0x3FFFFF);
    buffer[len++] = (oid >> 24) & 0xFF; buffer[len++] = (oid >> 16) & 0xFF; buffer[len++] = (oid >> 8) & 0xFF; buffer[len++] = oid & 0xFF;
    buffer[len++] = 0x19; buffer[len++] = 0x4D; 
    buffer[len++] = 0x3E; 
    uint32_t str_len = strlen(new_name);
    uint32_t payload_len = str_len + 1;
    if (payload_len <= 4) buffer[len++] = 0x70 | (uint8_t)payload_len;
    else { buffer[len++] = 0x75; buffer[len++] = (uint8_t)payload_len; }
    buffer[len++] = 0x00; 
    memcpy(&buffer[len], new_name, str_len);
    len += str_len;
    buffer[len++] = 0x3F;
    return len;
}

uint16_t build_write_property_value_apdu(uint8_t* buffer, uint8_t invoke_id, uint16_t obj_type, uint32_t obj_instance, uint8_t prop_id, float value) {
    uint16_t len = 0;
    buffer[len++] = 0x01; buffer[len++] = 0x04; 
    buffer[len++] = 0x02; buffer[len++] = 0x03;
    buffer[len++] = invoke_id; 
    buffer[len++] = 0x0F; // WriteProperty
    
    buffer[len++] = 0x0C;
    uint32_t oid = ((uint32_t)obj_type << 22) | (obj_instance & 0x3FFFFF);
    buffer[len++] = (oid >> 24) & 0xFF; buffer[len++] = (oid >> 16) & 0xFF; 
    buffer[len++] = (oid >> 8) & 0xFF; buffer[len++] = oid & 0xFF;
    
    buffer[len++] = 0x19; buffer[len++] = prop_id;
    buffer[len++] = 0x3E; // Opening Tag 3
    
    if (obj_type == 13 || obj_type == 14 || obj_type == 19) {
        uint32_t v = (uint32_t)value;
        if (v <= 255) { buffer[len++] = 0x21; buffer[len++] = (uint8_t)v; }
        else if (v <= 65535) { buffer[len++] = 0x22; buffer[len++] = (v >> 8) & 0xFF; buffer[len++] = v & 0xFF; }
        else { 
            buffer[len++] = 0x24; 
            buffer[len++] = (v >> 24) & 0xFF; buffer[len++] = (v >> 16) & 0xFF; 
            buffer[len++] = (v >> 8) & 0xFF; buffer[len++] = v & 0xFF; 
        }
    } else if (obj_type == 3 || obj_type == 4 || obj_type == 5) {
        buffer[len++] = 0x91; buffer[len++] = (value > 0.5f) ? 1 : 0;
    } else {
        buffer[len++] = 0x44;
        uint32_t tmp; float fv = value;
        memcpy(&tmp, &fv, 4);
        tmp = __builtin_bswap32(tmp);
        memcpy(&buffer[len], &tmp, 4);
        len += 4;
    }
    
    buffer[len++] = 0x3F; // Closing Tag 3
    return len;
}

String get_unit_text(uint16_t units) {
    switch(units) {
        // Température
        case 62: return "°C";
        case 63: return "°K";
        case 64: return "°F";
        // Énergie
        case 19: return "kWh";
        case 18: return "Wh";
        case 146: return "MWh";
        case 17: return "kJ";
        case 16: return "J";
        case 126: return "MJ";
        case 20: return "BTU";
        // Puissance
        case 48: return "kW";
        case 47: return "W";
        case 49: return "MW";
        // Électrique
        case 5: return "V";
        case 124: return "mV";
        case 3: return "A";
        case 2: return "mA";
        case 4: return "Ohm";
        case 8: return "VA";
        case 9: return "kVA";
        // Pression
        case 53: return "Pa";
        case 54: return "kPa";
        case 55: return "bar";
        case 56: return "psi";
        // Débit
        case 87: return "L/s";
        case 88: return "L/min";
        case 136: return "L/h";
        case 85: return "m³/s";
        case 135: return "m³/h";
        case 84: return "cfm";
        // Humidité / Concentration / Pourcentage
        case 29: return "%RH";
        case 98: return "%";
        case 96: return "ppm";
        // Temps
        case 73: return "s";
        case 72: return "min";
        case 71: return "h";
        case 159: return "ms";
        // Volume
        case 82: return "L";
        case 80: return "m³";
        // Divers
        case 95: return "no-units"; 
        case 255: return "none";
        default: return "Unit " + String(units);
    }
}

// --- BACNET TASK FSM ---
static void bacnet_task(void *pv) {
    uint8_t rx_byte; uint8_t header[6]; uint8_t header_idx=0; uint8_t data_buf[512+2]; 
    uint16_t data_len=0, data_idx=0;
    enum RX_STATE { RX_IDLE, RX_PREAMBLE_55, RX_HEADER, RX_DATA, RX_CRC16_L, RX_CRC16_H };
    RX_STATE rx_state = RX_IDLE;
    MSTP_MASTER_STATE mstp_state = MSTP_INITIALIZE;
    static unsigned frame_count = 0;
    uint32_t last_rx_time = millis(); uint32_t timer_silence = millis();
    uint8_t token_skip_count = 0; bool waiting_for_reply = false, ReceivedValidFrame = false;
    uint8_t frame_type = 0, dest_mac = 0, src_mac = 0;
    uint8_t current_poll_idx = 0, current_dev_idx = 0;
    uint32_t heartbeat_timer = 0;
    static uint32_t last_who_is_time = 0;
    static uint8_t next_station;
    static uint8_t poll_station;

    uart_event_t event;

    z_log(LOG_INFO,"BACNET","Engine %s - MSTP Enabled\n", VERSION_GLOBAL);

    for (;;) {
        if (xQueueReceive(uart_evt_queue, (void *)&event, 0) == pdTRUE) {
            if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) uart_flush_input(RS485_UART_PORT);
        }

        // --- DÉCOUVERTE AUTOMATIQUE ---
        // Envoi toutes les 10s si cache vide, sinon toutes les 5 min
        uint32_t who_is_interval = (bacnet_network_cache.empty()) ? 10000 : 300000;
        if (millis() - last_who_is_time > who_is_interval) {
            BACnetJob job; job.type = JOB_WHO_IS; job.target_mac = 0xFF;
            enqueue_bacnet_job(job);
            last_who_is_time = millis();
        }

        if (millis() - heartbeat_timer > sysCfg.heartbeat_interval) {
         
            z_log(LOG_INFO,"BACNET","Heartbeat - Tokens: %lu, RX: %lu, TX: %lu (State:%d, Cache:%u)\n", bacnetStats.tokens_seen, bacnetStats.ms_msgs_rx, bacnetStats.ms_msgs_tx, (int)mstp_state, (uint32_t)bacnet_network_cache.size());
            heartbeat_timer = millis();
        }

        ReceivedValidFrame = false;
        while (uart_read_bytes(RS485_UART_PORT, &rx_byte, 1, 0) > 0) {
            last_rx_time = millis();
            switch (rx_state) {
                case RX_IDLE: if (rx_byte == 0x55) { rx_state = RX_PREAMBLE_55; memset(data_buf, 0, sizeof(data_buf)); } break;
                case RX_PREAMBLE_55: if (rx_byte == 0xFF) { rx_state = RX_HEADER; header_idx = 0; } else rx_state = RX_IDLE; break;
                case RX_HEADER:
                    header[header_idx++] = rx_byte;
                    if (header_idx == 6) {
                        if (validate_rx_header_crc(header)) {
                            frame_type = header[0]; dest_mac = header[1]; src_mac = header[2];
                            data_len = (header[3] << 8) | header[4];
                            if (src_mac < 128 && src_mac != sysCfg.mac_address) {
                                if (xSemaphoreTake(cache_mutex, 0)) {
                                    bool known = false;
                                    for(auto& d : bacnet_network_cache) { 
                                        if(d.mac_address == src_mac) { 
                                            known = true; 
                                            break; 
                                        } 
                                    }
                                    if(!known) {
                                        z_log(LOG_INFO, "BACNET","Found New MAC: %u\n", src_mac);
                                        BACnetDevice d; d.mac_address = src_mac; d.device_id = 4194303; d.enabled = false; d.discovery_done = false;
                                        d.name = ""; d.vendor = ""; d.disc_step = DISC_DEV_ID; d.disc_obj_idx = 0;
                                        bacnet_network_cache.push_back(d);
                                    }
                                    xSemaphoreGive(cache_mutex);
                                }
                            }
                            if (data_len > 0 && data_len <= 512) { rx_state = RX_DATA; data_idx = 0; } 
                            else { ReceivedValidFrame = true; rx_state = RX_IDLE; }
                        } else { bacnetStats.errors_crc++; rx_state = RX_IDLE; }
                    }
                    break;
                case RX_DATA: data_buf[data_idx++] = rx_byte; if (data_idx == data_len) rx_state = RX_CRC16_L; break;
                case RX_CRC16_L: data_buf[data_len] = rx_byte; rx_state = RX_CRC16_H; break;
                case RX_CRC16_H: 
                    data_buf[data_len+1] = rx_byte;
                    if (validate_rx_data_crc(data_buf, data_len + 2)) { 
                        ReceivedValidFrame = true; 
                        if (frame_type == 0x05 || frame_type == 0x06) bacnetStats.ms_msgs_rx++;
                        if (frame_type == 0x00 && dest_mac == sysCfg.mac_address) {
                            delayMicroseconds(1050); 
                        }
                    }
                    else { bacnetStats.errors_crc++; }
                    rx_state = RX_IDLE;
                    break;
            }
        }

        uint32_t Tno_token = 500 + (sysCfg.mac_address * 10);
        switch (mstp_state) {
            case MSTP_INITIALIZE: 
                token_skip_count = 0; 
                next_station = sysCfg.mac_address;
                poll_station = sysCfg.mac_address;
                mstp_state = MSTP_IDLE; 
                break;
            case MSTP_IDLE:
                if (ReceivedValidFrame) {
                    if (frame_type == 0x00 && dest_mac == sysCfg.mac_address) { 
                        bacnetStats.tokens_seen++; 
                        next_station = (src_mac + 1) % (sysCfg.max_master + 1); 
                        frame_count = 0;
                        mstp_state = MSTP_USE_TOKEN; 
                    }
                    else if (frame_type == 0x01 && dest_mac == sysCfg.mac_address) { uint8_t f[8] = { 0x55, 0xFF, 0x02, src_mac, sysCfg.mac_address, 0, 0, 0 }; f[7] = calc_header_crc(&f[2], 5); uart_tx(f, 8); mstp_state = MSTP_IDLE; }
                    else if (dest_mac == sysCfg.mac_address && frame_type >= 0x05) { mstp_state = MSTP_ANSWER_DATA_REQUEST; }
                    else if (dest_mac == 0xFF && (frame_type == 0x05 || frame_type == 0x06)) {
                        // --- PARSING NPDU DYNAMIQUE POUR BROADCAST ---
                        if (data_len >= 2 && data_buf[0] == 0x01) {
                            uint8_t npdu_control = data_buf[1];
                            uint16_t pos = 2;
                            if ((npdu_control & 0x80) == 0) {
                                if ((npdu_control & 0x20) != 0) { if (pos + 2 < data_len) { uint8_t dlen = data_buf[pos + 2]; pos += 3 + dlen; } }
                                if ((npdu_control & 0x08) != 0) { if (pos + 2 < data_len) { uint8_t slen = data_buf[pos + 2]; pos += 3 + slen; } }
                                if ((npdu_control & 0x20) != 0) pos += 1;
                                if (pos < data_len) {
                                    uint8_t *apdu = &data_buf[pos];
                                    uint16_t apdu_len = data_len - pos;
                                    if (apdu[0] == 0x10 && apdu[1] == 0x00) { // I-Am
                                        if (apdu_len >= 6 && apdu[2] == 0xC4) {
                                            uint32_t oid = (apdu[3] << 24) | (apdu[4] << 16) | (apdu[5] << 8) | apdu[6];
                                            uint32_t device_id = oid & 0x3FFFFF;
                                            if (xSemaphoreTake(cache_mutex, 0)) {
                                                bool exists = false;
                                                for (auto& dev : bacnet_network_cache) { if (dev.device_id == device_id) { exists = true; dev.last_seen = millis(); break; } }
                                                if (!exists) {
                                                    BACnetDevice new_dev; new_dev.device_id = device_id; new_dev.mac_address = src_mac;
                                                    new_dev.enabled = false; new_dev.discovery_done = false; new_dev.last_seen = millis();
                                                    new_dev.disc_step = DISC_DEV_ID; new_dev.disc_obj_idx = 0;
                                                    bacnet_network_cache.push_back(new_dev);
                                                    z_log(LOG_INFO,"BACNET","Discover New Device (Broadcast): ID %lu, MAC %d\n", device_id, src_mac);
                                                }
                                                xSemaphoreGive(cache_mutex);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                } else if (millis() - last_rx_time > Tno_token) { mstp_state = MSTP_NO_TOKEN; }
                break;
            case MSTP_NO_TOKEN:  mstp_state = MSTP_POLL_FOR_MASTER; timer_silence = millis(); break;
            case MSTP_POLL_FOR_MASTER:
                if (millis() - timer_silence > 50) { last_rx_time = millis(); uint8_t f[8]={0x55,0xFF,0x01, (uint8_t)((sysCfg.mac_address+1)%128), sysCfg.mac_address,0,0,0}; f[7]=calc_header_crc(&f[2],5); uart_tx(f,8); bacnetStats.tokens_seen++; mstp_state = MSTP_IDLE; }
                break;
            case MSTP_USE_TOKEN:
                if (frame_count == 0) token_skip_count++;
                if (frame_count > 0 || token_skip_count >= sysCfg.token_skip) { 
                    if (xSemaphoreTake(cache_mutex, 0)) {
                        BACnetJob current_job;
                        bool has_job = (xQueueReceive(bacnet_job_queue, &current_job, 0) == pdTRUE);
                        
                        if (has_job && current_job.type == JOB_WHO_IS) {
                            uint8_t buffer[16]; uint16_t len = 0;
                            buffer[len++] = 0x01; // Version
                            buffer[len++] = 0x20; // Control (Bit 5 = DNET présent)
                            buffer[len++] = 0xFF; // DNET MSB (65535 = Broadcast)
                            buffer[len++] = 0xFF; // DNET LSB
                            buffer[len++] = 0x00; // DLEN = 0 (Broadcast)
                            buffer[len++] = 0xFF; // Hop Count (255)
                            buffer[len++] = 0x10; // Unconfirmed-Request-PDU
                            buffer[len++] = 0x08; // Service Choice: Who-Is
                            send_mstp_frame(0xFF, 0x06, buffer, len);
                            token_skip_count = 0;
                            frame_count++;
                            if (frame_count < sysCfg.max_info_frames) mstp_state = MSTP_USE_TOKEN;
                            else mstp_state = MSTP_DONE_WITH_TOKEN;
                        } else if (has_job && current_job.type == JOB_WRITE_PROP) {
                            uint8_t buffer[256];
                            uint16_t apdu_len = 0;
                            if (current_job.prop_id == 77) {
                                apdu_len = build_write_property_name_apdu(buffer, next_invoke_id++, current_job.obj_type, current_job.obj_instance, current_job.name);
                            } else {
                                apdu_len = build_write_property_value_apdu(buffer, next_invoke_id++, current_job.obj_type, current_job.obj_instance, current_job.prop_id, current_job.write_value);
                            }
                            pending_write_job = current_job;
                            token_skip_count = 0; retry_count=0; waiting_for_reply = true;
                            send_mstp_frame(current_job.target_mac, 0x05, buffer, apdu_len);
                            frame_count++;
                            mstp_state = MSTP_WAIT_FOR_REPLY;
                        } else if (!bacnet_network_cache.empty()) {
                            uint8_t apdu[64]; uint16_t apdu_len = 0;
                            if (current_dev_idx >= bacnet_network_cache.size()) current_dev_idx = 0;
                            auto& dev = bacnet_network_cache[current_dev_idx];
                            
                            if (dev.enabled || (!dev.discovery_done && dev.disc_step <= DISC_OBJ_COUNT)) {
                                if (!dev.discovery_done) {
                                    // --- DISCOVERY LOGIC ---
                                    if (dev.disc_step == DISC_DEV_ID) apdu_len = build_read_property_apdu(apdu, next_invoke_id++, 8, 4194303, 75, -1);
                                    else if (dev.disc_step == DISC_DEV_NAME) apdu_len = build_read_property_apdu(apdu, next_invoke_id++, 8, dev.device_id, 77, -1);
                                    else if (dev.disc_step == DISC_DEV_VENDOR) apdu_len = build_read_property_apdu(apdu, next_invoke_id++, 8, dev.device_id, 121, -1);
                                    else if (dev.disc_step == DISC_OBJ_COUNT) apdu_len = build_read_property_apdu(apdu, next_invoke_id++, 8, dev.device_id, 76, 0);
                                    else if (dev.enabled && dev.disc_obj_idx < dev.objects.size()) {
                                        auto& o = dev.objects[dev.disc_obj_idx];
                                        if (dev.disc_step == DISC_OBJ_OID) apdu_len = build_read_property_apdu(apdu, next_invoke_id++, 8, dev.device_id, 76, dev.disc_obj_idx + 1);
                                        else if (dev.disc_step == DISC_OBJ_NAME) apdu_len = build_read_property_apdu(apdu, next_invoke_id++, o.type, o.instance, 77, -1);
                                        else if (dev.disc_step == DISC_OBJ_UNITS) apdu_len = build_read_property_apdu(apdu, next_invoke_id++, o.type, o.instance, 117, -1);
                                        else if (dev.disc_step == DISC_OBJ_STATES) {
                                            if (o.expected_states_count == 0) {
                                                apdu_len = build_read_property_apdu(apdu, next_invoke_id++, o.type, o.instance, 110, 0);
                                            }
                                            else if (o.state_texts.empty()) {
                                                apdu_len = build_read_property_apdu(apdu, next_invoke_id++, o.type, o.instance, 110, -1);
                                            }
                                            else { 
                                                dev.disc_step = DISC_OBJ_VALUE; 
                                                apdu_len = build_read_property_apdu(apdu, next_invoke_id++, o.type, o.instance, 85, -1); 
                                            }
                                        }
                                        else if (dev.disc_step == DISC_OBJ_VALUE) apdu_len = build_read_property_apdu(apdu, next_invoke_id++, o.type, o.instance, 85, -1);
                                    }
                                    else { 
                                        dev.discovery_done = true; save_device_objects_locked(dev.device_id); 
                                        z_log(LOG_INFO, "BACNET", "Discovery Successfully Finalized for ID:%lu.\n", dev.device_id);
                                        
                                        // Synchronisation HA complète (inclut le nettoyage des objets non-activés)
                                        trigger_ha_discovery(dev.device_id, 0xFFFFFFFF, 0xFFFF);
                                        
                                        for (auto& obj : dev.objects) {
                                            if (obj.type == 65535) continue;
                                            if (!obj.name_published || strcmp(obj.name, obj.last_mqtt_name) != 0) {
                                                publish_mqtt_topic(dev.device_id, obj, 77, true);
                                                obj.name_published = true;
                                                strlcpy(obj.last_mqtt_name, obj.name, sizeof(obj.last_mqtt_name));
                                                z_log(LOG_INFO, "BACNET", "Sync MQTT Name OK : %s\n", obj.name);
                                            }
                                        }
                                        // On reste dans USE_TOKEN pour peut être commencer le polling
                                        mstp_state = MSTP_USE_TOKEN;
                                        xSemaphoreGive(cache_mutex);
                                        break;
                                    }
                                } else {
                                    // --- METADATA RECOVERY CHECK ---
                                    bool recovery_triggered = false;
                                    for(size_t i=0; i<dev.objects.size(); i++) {
                                        auto& o = dev.objects[i];
                                        if (o.enabled && (o.type == 13 || o.type == 14 || o.type == 19) && o.state_texts.empty()) {
                                            z_log(LOG_WARN, "BACNET", "Object Metadata Missing for Obj T%u I%lu - Triggering Recovery\n", o.type, (unsigned long)o.instance);
                                            dev.discovery_done = false;
                                            dev.recovery_mode = true;
                                            dev.disc_step = DISC_OBJ_STATES;
                                            dev.disc_obj_idx = i;
                                            recovery_triggered = true;
                                            break;
                                        }
                                    }
                                    if (recovery_triggered) { xSemaphoreGive(cache_mutex); break; }

                                    // --- POLLING LOGIC (Optimisée pour plusieurs frames) ---
                                    size_t count = dev.objects.size();
                                    if (count > 0) {
                                        size_t scanned = 0;
                                        while (scanned < count) {
                                            current_poll_idx = (current_poll_idx + 1) % count;
                                            auto& o = dev.objects[current_poll_idx];
                                            if (o.enabled && o.type != 8 && o.type != 65535 && (millis() - o.last_update > (sysCfg.bacnet_poll_interval * 1000))) { 
                                                z_log(LOG_DEBUG,"BACNET", "Polling Obj %d/%d (T%u I%lu)\n", current_poll_idx + 1, count, o.type, (unsigned long)o.instance);
                                                apdu_len = build_read_property_apdu(apdu, next_invoke_id++, o.type, o.instance, 85, -1); 
                                                break;
                                            }
                                            scanned++;
                                        }
                                    }
                                }
                            }

                            if (apdu_len > 0) { 
                                token_skip_count = 0; 
                                retry_count=0; 
                                waiting_for_reply = true; 
                                send_mstp_frame(dev.mac_address, 0x05, apdu, apdu_len); 
                                frame_count++;
                                mstp_state = MSTP_WAIT_FOR_REPLY; 
                            }
                            else { 
                                // Rien à faire pour ce device, on passe au suivant
                                current_dev_idx = (current_dev_idx + 1) % bacnet_network_cache.size();
                                // On libère le jeton car on n'a plus rien d'urgent à envoyer
                                frame_count = sysCfg.max_info_frames;
                                mstp_state = MSTP_DONE_WITH_TOKEN; 
                            }
                        } else { 
                            frame_count = sysCfg.max_info_frames;
                            mstp_state = MSTP_DONE_WITH_TOKEN; 
                        }
                        xSemaphoreGive(cache_mutex);
                    } else { 
                        // Mutex busy, on réessaiera au prochain tour de loop()
                    }
                } else { 
                    mstp_state = MSTP_DONE_WITH_TOKEN; 
                }
                break;
            case MSTP_WAIT_FOR_REPLY:
                if (ReceivedValidFrame) {
                    waiting_for_reply = false;
                    
                    // --- PARSING NPDU DYNAMIQUE ---
                    if (data_len < 2 || data_buf[0] != 0x01) { mstp_state = MSTP_DONE_WITH_TOKEN; break; }
                    uint8_t npdu_control = data_buf[1];
                    uint16_t pos = 2;
                    if ((npdu_control & 0x80) != 0) { mstp_state = MSTP_DONE_WITH_TOKEN; break; } // Message réseau pur
                    if ((npdu_control & 0x20) != 0) { if (pos + 2 >= data_len) break; uint8_t dlen = data_buf[pos + 2]; pos += 3 + dlen; } // DNET
                    if ((npdu_control & 0x08) != 0) { if (pos + 2 >= data_len) break; uint8_t slen = data_buf[pos + 2]; pos += 3 + slen; } // SNET
                    if ((npdu_control & 0x20) != 0) pos += 1; // Hop Count
                    if (pos >= data_len) { mstp_state = MSTP_DONE_WITH_TOKEN; break; }

                    uint8_t *apdu = &data_buf[pos];
                    uint16_t apdu_len = data_len - pos;
                    uint8_t pdu_type = apdu[0] & 0xF0;

                    // --- DETECTION I-AM (Découverte) ---
                    if (apdu[0] == 0x10 && apdu[1] == 0x00) { // Unconfirmed Request, Service 0 (I-Am)
                        if (apdu_len >= 6 && apdu[2] == 0xC4) { // Tag 12 (OID), Len 4
                            uint32_t oid = (apdu[3] << 24) | (apdu[4] << 16) | (apdu[5] << 8) | apdu[6];
                            uint32_t device_id = oid & 0x3FFFFF;
                            if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(100))) {
                                bool exists = false;
                                for (auto& dev : bacnet_network_cache) { if (dev.device_id == device_id) { exists = true; dev.last_seen = millis(); break; } }
                                if (!exists) {
                                    BACnetDevice new_dev; new_dev.device_id = device_id; new_dev.mac_address = src_mac;
                                    new_dev.enabled = false; new_dev.discovery_done = false; new_dev.last_seen = millis();
                                    bacnet_network_cache.push_back(new_dev);
                                    z_log(LOG_INFO,"BACNET", "New Device Discovered: ID %lu, MAC %d\n", device_id, src_mac);
                                }
                                xSemaphoreGive(cache_mutex);
                            }
                        }
                        mstp_state = MSTP_DONE_WITH_TOKEN; break;
                    }

                    // --- TRAITEMENT RÉPONSES AUX REQUÊTES ---
                    if (dest_mac == sysCfg.mac_address) {
                        if (pdu_type == 0x20) { // Simple-ACK (Succès WriteProperty)
                            z_log(LOG_INFO,"BACNET","WriteProperty SUCCESS (Invoke ID %d)\n", apdu[1]);
                            
                            // --- REFRESH OPTIMISTE ---
                            if (apdu[1] == last_sent_invoke_id && pending_write_job.type == JOB_WRITE_PROP) {
                                if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(100))) {
                                    for(auto& d : bacnet_network_cache) {
                                        if (d.mac_address == src_mac) {
                                            for(auto& o : d.objects) {
                                                if (o.type == pending_write_job.obj_type && o.instance == pending_write_job.obj_instance) {
                                                    if (pending_write_job.prop_id == 85) {
                                                        o.present_value = pending_write_job.write_value;
                                                        o.last_update = millis();
                                                        z_log(LOG_INFO,"BACNET","Optimistic update for Obj T%u I%lu: %.2f\n", o.type, (unsigned long)o.instance, o.present_value);
                                                    } else if (pending_write_job.prop_id == 77) {
                                                        strlcpy(o.name, pending_write_job.name, sizeof(o.name));
                                                        z_log(LOG_INFO,"BACNET","Optimistic update for Obj T%u I%lu: %.2f\n", o.type, (unsigned long)o.instance, o.present_value);
                                                    }
                                                    publish_mqtt_topic(d.device_id, o, pending_write_job.prop_id, true);
                                                    break;
                                                }
                                            }
                                            break;
                                        }
                                    }
                                    xSemaphoreGive(cache_mutex);
                                }
                            }
                            
                            last_sent_invoke_id = 0xFF; 
                            current_dev_idx = (current_dev_idx + 1) % bacnet_network_cache.size();
                        } 
                        else if (pdu_type == 0x30) { // Complex-ACK (Succès ReadProperty)
                            uint8_t invoke_id = apdu[1];
                            if (invoke_id != last_sent_invoke_id) {
                                z_log(LOG_WARN,"BACNET","Complex-ACK ignored (InvokeID mismatch: %d != %d)\n", invoke_id, last_sent_invoke_id);
                                waiting_for_reply = true; break;
                            }
                            uint16_t apdu_pos = 3; BACnetTag t;
                            uint8_t last_prop_id = 0xFF;
                            while (apdu_pos < apdu_len && decode_next_tag(apdu, &apdu_pos, apdu_len, &t)) {
                                if (t.number == 1) { // Property Identifier
                                    last_prop_id = 0;
                                    for(int i=0; i<t.len; i++) last_prop_id = (last_prop_id << 8) | apdu[apdu_pos+i];
                                    apdu_pos += t.len;
                                    // log volontairement retiré pour réduire le bruit lors du polling
                                }
                                else if (t.isOpening && t.number == 3) {
                                    // On boucle tant qu'on ne trouve pas le tag de fermeture correspondant (Closing 3)
                                    // ou qu'on n'a pas atteint la fin de l'APDU
                                    if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(50))) {
                                        if (current_dev_idx < bacnet_network_cache.size()) {
                                            auto& dev = bacnet_network_cache[current_dev_idx];
                                            BACnetTag val_tag;
                                            while (apdu_pos < apdu_len && decode_next_tag(apdu, &apdu_pos, apdu_len, &val_tag)) {
                                                if (val_tag.isClosing && val_tag.number == 3) break; 
                                                
                                                if (last_prop_id == 28) {
                                                    char desc[65];
                                                    uint8_t enc = apdu[apdu_pos];
                                                    int slen = 0;
                                                    if (enc == 0) {
                                                        slen = std::min((int)val_tag.len - 1, 64);
                                                        memcpy(desc, &apdu[apdu_pos+1], slen);
                                                    } else {
                                                        for(int i=2; i<val_tag.len && slen<64; i+=2) desc[slen++] = apdu[apdu_pos+i];
                                                    }
                                                    desc[slen] = '\0';
                                                    z_log(LOG_DEBUG,"BACNET","Property 28 (Description) Data: %s\n", desc);
                                                }

                                                if (!dev.discovery_done) {
                                                    // --- DISCOVERY PROCESSING ---
                                                    if (dev.disc_step == DISC_DEV_ID) { dev.device_id = (((apdu[apdu_pos]<<24)|(apdu[apdu_pos+1]<<16)|(apdu[apdu_pos+2]<<8)|apdu[apdu_pos+3]) & 0x3FFFFF); z_log(LOG_INFO,"BACNET","Device ID: %lu\n", (unsigned long)dev.device_id); dev.disc_step = DISC_DEV_NAME; }
                                                    else if (dev.disc_step == DISC_DEV_NAME) { char n[33]; uint8_t enc = apdu[apdu_pos]; if (enc == 0) { uint16_t slen = std::min((int)val_tag.len - 1, 32); memcpy(n, &apdu[apdu_pos+1], slen); n[slen]=0; } else { int slen=0; for(int i=2; i<val_tag.len && slen<32; i+=2) n[slen++]=apdu[apdu_pos+i]; n[slen]=0; } dev.name = String(n); z_log(LOG_INFO,"BACNET","Device name: %s\n", n); dev.disc_step = DISC_DEV_VENDOR; }
                                                    else if (dev.disc_step == DISC_DEV_VENDOR) { char n[33]; uint8_t enc = apdu[apdu_pos]; if (enc == 0) { uint16_t slen = std::min((int)val_tag.len - 1, 32); memcpy(n, &apdu[apdu_pos+1], slen); n[slen]=0; } else { int slen=0; for(int i=2; i<val_tag.len && slen<32; i+=2) n[slen++]=apdu[apdu_pos+i]; n[slen]=0; } dev.vendor = String(n); z_log(LOG_INFO,"BACNET","Device Vendor: %s\n", n); dev.disc_step = DISC_OBJ_COUNT; }
                                                    else if (dev.disc_step == DISC_OBJ_COUNT) {
                                                        uint32_t count = 0; for(int i=0; i<val_tag.len; i++) count = (count << 8) | apdu[apdu_pos+i];
                                                        z_log(LOG_INFO,"BACNET","Device Object Count: %lu\n", (unsigned long)count);
                                                        dev.objects.clear(); dev.objects.reserve(count);
                                                        for(int i=0; i<count; i++) { 
                                                            BACnetObject o; 
                                                            strlcpy(o.name, "Unknown", sizeof(o.name)); 
                                                            o.enabled=false; // Désactivé par défaut pour éviter l'inondation réseau
                                                            o.type=65535; 
                                                            dev.objects.push_back(o); 
                                                        }
                                                        dev.disc_obj_idx = 0; dev.disc_step = DISC_OBJ_OID;
                                                    } else if (dev.disc_obj_idx < dev.objects.size()) {
                                                        auto& o = dev.objects[dev.disc_obj_idx];
                                                        if (dev.disc_step == DISC_OBJ_OID) { 
                                                            uint32_t oid = (apdu[apdu_pos]<<24)|(apdu[apdu_pos+1]<<16)|(apdu[apdu_pos+2]<<8)|apdu[apdu_pos+3]; 
                                                            o.type = oid >> 22; o.instance = oid & 0x3FFFFF; 
                                                            
                                                            // Tous les objets sont désactivés par défaut. L'utilisateur choisira.
                                                            o.enabled = false; 

                                                            z_log(LOG_INFO,"BACNET","Obj %u (BACnet Idx:%d) OID: T%u I%lu (Poll:NO - Waiting for User)\n", 
                                                                dev.disc_obj_idx+1, dev.disc_obj_idx+1, o.type, (unsigned long)o.instance); 
                                                            dev.disc_step = DISC_OBJ_NAME; 
                                                        }
                                                        else if (dev.disc_step == DISC_OBJ_NAME) { 
                                                            char n[33]; uint8_t enc = apdu[apdu_pos]; 
                                                            if (enc == 0) { uint16_t slen = std::min((int)val_tag.len - 1, 32); memcpy(n, &apdu[apdu_pos+1], slen); n[slen]=0; } 
                                                            else { int slen=0; for(int i=2; i<val_tag.len && slen<32; i+=2) n[slen++]=apdu[apdu_pos+i]; n[slen]=0; } 
                                                            
                                                            String ns = String(n); ns.trim();
                                                            if (ns.length() == 0 || ns.equalsIgnoreCase("Unknown") || ns.equalsIgnoreCase("Untitled")) {
                                                                String typeStr = (o.type == 0) ? "AI" : (o.type == 1) ? "AO" : (o.type == 2) ? "AV" : 
                                                                                (o.type == 3) ? "BI" : (o.type == 4) ? "BO" : (o.type == 5) ? "BV" : 
                                                                                (o.type == 13) ? "MSI" : (o.type == 14) ? "MSO" : (o.type == 19) ? "MSV" : "OBJ";
                                                                snprintf(o.name, sizeof(o.name), "%s:%lu", typeStr.c_str(), (unsigned long)o.instance);
                                                            } else {
                                                                strlcpy(o.name, n, sizeof(o.name));
                                                                z_log(LOG_INFO,"BACNET","Obj %u Name: %s\n", dev.disc_obj_idx+1, o.name); 
                                                            }
                                                            
                                                            if(o.type <= 2 || o.type == 23 || o.type == 24 || o.type == 46) dev.disc_step = DISC_OBJ_UNITS; 
                                                            else if(o.type == 13 || o.type == 14 || o.type == 19) { o.state_texts.clear(); o.expected_states_count = 0; dev.disc_step = DISC_OBJ_STATES; }
                                                            else if(o.enabled) dev.disc_step = DISC_OBJ_VALUE;
                                                            else { dev.disc_obj_idx++; dev.disc_step = DISC_OBJ_OID; }
                                                        }
                                                        else if (dev.disc_step == DISC_OBJ_UNITS) { 
                                                            uint32_t u=0; for(int i=0; i<val_tag.len; i++) u = (u << 8) | apdu[apdu_pos+i]; 
                                                            o.units = u; 
                                                            String unitStr = get_unit_text(u);
                                                            if (unitStr.length() > 0) strlcpy(o.unit_text, unitStr.c_str(), sizeof(o.unit_text)); 
                                                            else strlcpy(o.unit_text, "Unknown", sizeof(o.unit_text)); 
                                                            z_log(LOG_INFO,"BACNET","Obj %u Units: %s\n", dev.disc_obj_idx+1, o.unit_text); 
                                                            if(o.type == 13 || o.type == 14 || o.type == 19) { o.state_texts.clear(); o.expected_states_count = 0; dev.disc_step = DISC_OBJ_STATES; }
                                                            else dev.disc_step = DISC_OBJ_VALUE; 
                                                        }
                                                        else if (dev.disc_step == DISC_OBJ_STATES) {
                                                            if (val_tag.number == 2) { // Unsigned Integer (Count)
                                                                uint32_t count = 0; for(int i=0; i<val_tag.len; i++) count = (count << 8) | apdu[apdu_pos+i];
                                                                o.expected_states_count = (uint16_t)count;
                                                                z_log(LOG_INFO,"BACNET","Obj %u States Count: %u\n", dev.disc_obj_idx+1, o.expected_states_count);
                                                                if (o.expected_states_count == 0) dev.disc_step = DISC_OBJ_VALUE;
                                                            } 
                                                            else if (val_tag.number == 7) { // CharacterString
                                                                char n[33]; uint8_t enc = apdu[apdu_pos]; 
                                                                if (enc == 0) { uint16_t slen = std::min((int)val_tag.len - 1, 32); memcpy(n, &apdu[apdu_pos+1], slen); n[slen]=0; } 
                                                                else { int slen=0; for(int i=2; i<val_tag.len && slen<32; i+=2) n[slen++]=apdu[apdu_pos+i]; n[slen]=0; }
                                                                
                                                                if (o.state_texts.size() < o.expected_states_count) {
                                                                    o.state_texts.push_back(String(n));
                                                                    z_log(LOG_INFO,"BACNET","Obj %u State %u/%u: %s\n", dev.disc_obj_idx+1, (uint32_t)o.state_texts.size(), (uint32_t)o.expected_states_count, n);
                                                                    if (o.state_texts.size() >= o.expected_states_count) dev.disc_step = DISC_OBJ_VALUE;
                                                                }
                                                            }
                                                        }
                                                        else if (dev.disc_step == DISC_OBJ_VALUE) {
                                                            if (val_tag.number == 4) { 
                                                                float v; uint32_t tmp; memcpy(&tmp, &apdu[apdu_pos], 4); 
                                                                tmp = __builtin_bswap32(tmp); memcpy(&v, &tmp, 4); o.present_value = v; 
                                                            } else { 
                                                                uint32_t v=0; for(int i=0; i<val_tag.len; i++) v = (v << 8) | apdu[apdu_pos+i]; 
                                                                o.present_value = (float)v; 
                                                            }
                                                            o.last_update = millis(); 
                                                            z_log(LOG_INFO,"BACNET","Obj %u Value: %.2f\n", dev.disc_obj_idx+1, o.present_value);
                                                            
                                                            bool stop_now = dev.reload_single || dev.recovery_mode;
                                                            if (!dev.recovery_mode) dev.disc_obj_idx++; 
                                                            dev.disc_step = DISC_OBJ_OID;

                                                            if (dev.disc_obj_idx % 10 == 0 || dev.disc_obj_idx >= dev.objects.size()) {
                                                                save_device_objects_locked(dev.device_id);
                                                            }

                                                            if (stop_now) { 
                                                                dev.discovery_done = true; 
                                                                dev.reload_single = false; 
                                                                dev.recovery_mode = false;
                                                                save_device_objects_locked(dev.device_id); 
                                                                if (o.enabled) trigger_ha_discovery(dev.device_id, o.instance, o.type);
                                                            }
                                                            else if (dev.disc_obj_idx >= dev.objects.size()) { 
                                                                dev.discovery_done = true; 
                                                                save_device_objects_locked(dev.device_id); 
                                                                publish_all_names(); 
                                                            }
                                                        }
                                                    }
                                                } else {
                                                    // --- POLLING PROCESSING ---
                                                    if (val_tag.number == 4) { 
                                                        float v; uint32_t tmp; memcpy(&tmp, &apdu[apdu_pos], 4); tmp = __builtin_bswap32(tmp); memcpy(&v, &tmp, 4); 
                                                        if(current_poll_idx < dev.objects.size()){ 
                                                            auto& o=dev.objects[current_poll_idx]; 
                                                            z_log(LOG_INFO,"BACNET","Poll Result Obj %d: %.2f\n", current_poll_idx + 1, v);
                                                            o.present_value=v; o.last_update=millis(); 
                                                            if (o.enabled) publish_mqtt_topic(dev.device_id, o, 85, false); 
                                                        } 
                                                    }
                                                    else if (val_tag.number == 2 || val_tag.number == 9) { 
                                                        uint32_t v=0; for(int i=0; i<val_tag.len; i++) v=(v<<8)|apdu[apdu_pos+i]; 
                                                        if(current_poll_idx < dev.objects.size()){ 
                                                            auto& o=dev.objects[current_poll_idx]; 
                                                            z_log(LOG_INFO,"BACNET","Poll Result Obj %d: %.2f\n", current_poll_idx + 1, (float)v);
                                                            o.present_value=(float)v; o.last_update=millis(); 
                                                            if (o.enabled) publish_mqtt_topic(dev.device_id, o, 85, false); 
                                                        } 
                                                    }
                                                }
                                                apdu_pos += val_tag.len;
                                            }
                                        }
                                        xSemaphoreGive(cache_mutex);
                                    }
                                } else apdu_pos += t.len;
                            }
                            last_sent_invoke_id = 0xFF;
                            // Une fois qu'on a traité une réponse complexe valide, on passe au device suivant
                            current_dev_idx = (current_dev_idx + 1) % bacnet_network_cache.size();
                        }
                        else if (pdu_type == 0x50 || pdu_type == 0x60 || pdu_type == 0x70 || pdu_type == 0x40) { 
                            uint8_t invoke_id = apdu[1];

                            // 1. Vérification que cette erreur concerne notre requête en cours
                            if (invoke_id == last_sent_invoke_id) {
                                z_log(LOG_ERROR,"BACNET","TSM: received Error/Reject/Abort 0x%02X, InvokeID: %d\n", pdu_type, invoke_id);

                                // 2. Logique de Retry
                                if (retry_count < sysCfg.max_retries) {
                                    retry_count++;
                                    z_log(LOG_WARN,"BACNET","Retry %d/%d for InvokeID %d\n", retry_count, sysCfg.max_retries, invoke_id);
                                    
                                    if (current_dev_idx < bacnet_network_cache.size()) {
                                        auto& dev = bacnet_network_cache[current_dev_idx];
                                        send_mstp_frame(dev.mac_address, 0x05, last_apdu, last_apdu_len);
                                    }
                                    
                                    timer_silence = millis();
                                    waiting_for_reply = true;
                                    break; 
                                } else {
                                    z_log(LOG_INFO,"BACNET","Maximum attempts reached, object rejected.");
                                    z_log(LOG_DEBUG,"BACNET","InvokeID: %d aborted.\n",invoke_id);
                                    // 3. Épuisement des retries : incrémentation sécurisée pour s'évader de la boucle
                                    if (current_dev_idx < bacnet_network_cache.size()) {
                                        auto& dev = bacnet_network_cache[current_dev_idx];
                                        if (!dev.discovery_done) {
                                            z_log(LOG_WARN,"BACNET","Skipping faulty object at Index %d\n", dev.disc_obj_idx);
                                            dev.disc_obj_idx++; 
                                            dev.disc_step = DISC_OBJ_OID;
                                            if (dev.disc_obj_idx >= dev.objects.size()) {
                                                dev.discovery_done = true;
                                                save_device_objects_locked(dev.device_id);
                                                publish_all_names();
                                            }
                                        }
                                        current_dev_idx = (current_dev_idx + 1) % bacnet_network_cache.size();
                                    }
                                    retry_count = 0;
                                    last_sent_invoke_id = 0xFF;
                                }
                            } else { 
                                z_log(LOG_INFO,"BACNET","TSM: Ignored inconsistency frame : %d != %d\n", invoke_id, last_sent_invoke_id);
                                waiting_for_reply = true;
                                break;
                            }
                        }
                    }
                    mstp_state = MSTP_DONE_WITH_TOKEN;
                } else if (millis() - timer_silence > sysCfg.apdu_timeout) { 
 
                    // CORRECTION DU TIMEOUT ET DU SCOPE
                    if (current_dev_idx < bacnet_network_cache.size()) {
                        auto& dev = bacnet_network_cache[current_dev_idx];
                        
                        if (retry_count < sysCfg.max_retries) {
                            retry_count++;
                            z_log(LOG_INFO,"BACNET","Timeout, Retry %d/%d for Obj %d\n", retry_count, sysCfg.max_retries, current_poll_idx);
                            send_mstp_frame(dev.mac_address, 0x05, last_apdu, last_apdu_len); 
                            timer_silence = millis(); 
                            waiting_for_reply = true; // On repart en attente
                            // On ne fait rien d'autre, le break du switch est en bas
                        } else {
                            z_log(LOG_WARN,"BACNET","Max retries reached on timeout. Skipping Obj.\n");
                            // CORRECTION DE L'ERREUR DE SYNTAXE (le "|| ...") supprimé
                            if (!dev.discovery_done) { 
                                dev.disc_obj_idx++; 
                                dev.disc_step = DISC_OBJ_OID;
                            }
                            current_dev_idx = (current_dev_idx + 1) % bacnet_network_cache.size();
                            mstp_state = MSTP_DONE_WITH_TOKEN;
                        }
                    } else {
                        // Sécurité si l'index est hors limites
                        mstp_state = MSTP_DONE_WITH_TOKEN;
                    }
                }
                break;
            

            case MSTP_ANSWER_DATA_REQUEST: z_log(LOG_INFO,"MSTP","Serving request...\n"); mstp_state = MSTP_IDLE; break;
            case MSTP_DONE_WITH_TOKEN: 
                if (frame_count < sysCfg.max_info_frames) {
                    mstp_state = MSTP_USE_TOKEN;
                } else {
                    mstp_state = MSTP_PASS_TOKEN;
                }
                break;
            case MSTP_PASS_TOKEN: { 
                uint8_t f[8] = { 0x55, 0xFF, 0x00, next_station, sysCfg.mac_address, 0, 0, 0 }; 
                f[7] = calc_header_crc(&f[2], 5); uart_tx(f, 8); 
                waiting_for_reply = false; 
                last_rx_time = millis(); 
                mstp_state = MSTP_IDLE; 
                break; 
            }
        }
        if (mstp_state == MSTP_IDLE && !ReceivedValidFrame) vTaskDelay(1);
        else taskYIELD();
    }
}

void setup_bacnet_engine() {
    bacnet_job_queue = xQueueCreate(10, sizeof(BACnetJob));
    const uart_config_t uc = { .baud_rate = 38400, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .rx_flow_ctrl_thresh = 122, .source_clk = UART_SCLK_APB };
    uart_driver_install(RS485_UART_PORT, 2048, 2048, 20, &uart_evt_queue, 0);
    uart_param_config(RS485_UART_PORT, &uc);
    uart_set_pin(RS485_UART_PORT, TX_PIN, RX_PIN, RTS_PIN, UART_PIN_NO_CHANGE);
    uart_set_mode(RS485_UART_PORT, UART_MODE_RS485_HALF_DUPLEX);
    uart_set_rx_timeout(RS485_UART_PORT, 2); 
    xTaskCreatePinnedToCore(bacnet_task, "BACnet", 16384, NULL, 15, NULL, 1);
    z_log(LOG_INFO,"BACNET","BACnet Engine Initialized\n");
}
bool enqueue_bacnet_job(BACnetJob job) { if (bacnet_job_queue == NULL) return false; return xQueueSend(bacnet_job_queue, &job, 0) == pdTRUE; }

// Helper pour identifier si une erreur est transitoire (mérite un retry)
bool is_transient_error(uint8_t pdu_type, uint8_t reason) {
    // Si c'est un Abort ou un Error
    if (pdu_type == 0x70 || pdu_type == 0x50) {
        // Exemples de raisons récupérables : Device Busy (2)
        return (reason == 0x02); 
    }
    return false;
}

// Extraction de la raison depuis l'APDU (exemple basé sur la structure standard)
uint8_t get_error_reason(uint8_t* apdu) {
    // Dans un Abort/Error, le InvokeID est à apdu[1], la raison à apdu[2]
    return apdu[2];
}

void publish_all_names() {
    // Note : Cette fonction est appelée par bacnet_task qui détient déjà le mutex
    for (auto& dev : bacnet_network_cache) {
        for (auto& obj : dev.objects) {
            // On saute les objets non valides ou désactivés
            if (obj.type == 65535 || obj.enabled == false) continue;

            // --- LOGIQUE DE SYNCHRO MQTT ---
            if (!obj.name_published || (strcmp(obj.name, obj.last_mqtt_name) != 0)) {
                publish_mqtt_topic(dev.device_id, obj, 77, true);
                obj.name_published = true;
                strlcpy(obj.last_mqtt_name, obj.name, sizeof(obj.last_mqtt_name));
                z_log(LOG_INFO,"BACNET","Obj Sync MQTT Name OK : %s\n", obj.name);
            }
        }
    }
}
