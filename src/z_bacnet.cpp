#include "z_bacnet.h"
#include <string.h>
#include <algorithm>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "z_network.h" 
#include "rom/ets_sys.h"

extern void z_log(const char* format, ...);
extern void save_device_objects(uint32_t device_id);

BACnet_Stats bacnetStats = {0, 0, 0, 0, 0, 0, 0};
std::vector<BACnetDevice> bacnet_network_cache;
QueueHandle_t bacnet_job_queue = NULL;
QueueHandle_t mqtt_publish_queue = NULL;
QueueHandle_t uart_evt_queue = NULL;
SemaphoreHandle_t cache_mutex = NULL;

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
static void uart_tx(const uint8_t *buffer, uint16_t length) {
    uart_write_bytes(RS485_UART_PORT, (const char*)buffer, length);
    bacnetStats.ms_msgs_tx++;
}

static void send_mstp_frame(uint8_t target_mac, uint8_t type, const uint8_t* apdu, uint16_t len) {
    uint8_t buffer[512+10];
    buffer[0]=0x55; buffer[1]=0xFF; buffer[2]=type; buffer[3]=target_mac; buffer[4]=sysCfg.mac_address;
    buffer[5]=(len>>8)&0xFF; buffer[6]=len&0xFF;
    buffer[7]=calc_header_crc(&buffer[2], 5);
    if (len > 0) {
        memcpy(&buffer[8], apdu, len);
        uint16_t crc16 = calc_data_crc(&buffer[8], len);
        buffer[8+len]=crc16&0xFF; buffer[8+len+1]=(crc16>>8)&0xFF;
        uart_tx(buffer, 8+len+2);
    } else uart_tx(buffer, 8);
}

uint16_t build_read_property_apdu(uint8_t* buffer, uint8_t invoke_id, uint16_t obj_type, uint32_t obj_instance, uint8_t property_id, int32_t array_index) {
    uint16_t len = 0;
    buffer[len++] = 0x01; buffer[len++] = 0x04; buffer[len++] = 0x02; buffer[len++] = 0x73;
    buffer[len++] = invoke_id; buffer[len++] = 0x0C; buffer[len++] = 0x0C;
    uint32_t oid = ((uint32_t)obj_type << 22) | (obj_instance & 0x3FFFFF);
    buffer[len++] = (oid >> 24) & 0xFF; buffer[len++] = (oid >> 16) & 0xFF; buffer[len++] = (oid >> 8) & 0xFF; buffer[len++] = oid & 0xFF;
    buffer[len++] = 0x19; buffer[len++] = property_id;
    if (array_index >= 0) {
        if (array_index <= 255) { buffer[len++] = 0x29; buffer[len++] = (uint8_t)array_index; }
        else { buffer[len++] = 0x2A; buffer[len++] = (array_index >> 8) & 0xFF; buffer[len++] = array_index & 0xFF; }
    }
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
    enum MSTP_MASTER_STATE { MSTP_INITIALIZE, MSTP_IDLE, MSTP_USE_TOKEN, MSTP_WAIT_TX_DONE, MSTP_AWAIT_REPLY, MSTP_DONE_WITH_TOKEN, MSTP_PASS_TOKEN, MSTP_NO_TOKEN, MSTP_ANSWER_DATA_REQUEST, MSTP_WAIT_FOR_PROPORTIONAL };
    MSTP_MASTER_STATE mstp_state = MSTP_INITIALIZE;
    uint32_t last_rx_time = millis(); uint32_t timer_silence = millis();
    uint8_t token_skip_count = 0; bool waiting_for_reply = false, ReceivedValidFrame = false;
    uint8_t frame_type = 0, dest_mac = 0, src_mac = 0;
    uint8_t current_invoke_id = 10, current_poll_idx = 0;
    uint32_t heartbeat_timer = 0;
    uint8_t next_station = (sysCfg.mac_address + 1) % 128;
    
    enum DISC_STEP_T { 
        DISC_DEV_ID, 
        DISC_DEV_NAME, 
        DISC_DEV_VENDOR, 
        DISC_OBJ_COUNT,   // Index 0 de Object_List
        DISC_OBJ_OID,     // Index N de Object_List
        DISC_OBJ_NAME,    // Property 77
        DISC_OBJ_UNITS,   // Property 117
        DISC_OBJ_VALUE    // Property 85
    } disc_step = DISC_DEV_ID;
    
    uint16_t disc_obj_idx = 0; // Index 0..count-1 dans le vecteur objects
    
    uart_event_t event;

    z_log("[BACNET] Engine v4.7.85 - Comprehensive Discovery & ASHRAE Units\n");

    for (;;) {
        if (xQueueReceive(uart_evt_queue, (void *)&event, 0) == pdTRUE) {
            if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) uart_flush_input(RS485_UART_PORT);
        }
        if (millis() - heartbeat_timer > sysCfg.heartbeat_interval) {
            z_log("[BACNET] Heartbeat - Tokens: %lu, RX: %lu, TX: %lu (State:%d, Cache:%u)\n", bacnetStats.tokens_seen, bacnetStats.ms_msgs_rx, bacnetStats.ms_msgs_tx, (int)mstp_state, (uint32_t)bacnet_network_cache.size());
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
                            bacnetStats.ms_msgs_rx++;
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
                                        z_log("[BACNET] Found New MAC: %u\n", src_mac);
                                        BACnetDevice d; d.mac_address = src_mac; d.device_id = 4194303; d.enabled = true; d.discovery_done = false;
                                        d.name = ""; d.vendor = "";
                                        bacnet_network_cache.push_back(d);
                                        disc_step = DISC_DEV_ID; disc_obj_idx = 0;
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
                    if (validate_rx_data_crc(data_buf, data_len + 2)) { ReceivedValidFrame = true; }
                    else { bacnetStats.errors_crc++; }
                    rx_state = RX_IDLE;
                    break;
            }
        }

        uint32_t Tno_token = 500 + (sysCfg.mac_address * 10);
        switch (mstp_state) {
            case MSTP_INITIALIZE: token_skip_count = 0; mstp_state = MSTP_IDLE; break;
            case MSTP_IDLE:
                if (ReceivedValidFrame) {
                    if (frame_type == 0x00 && dest_mac == sysCfg.mac_address) { bacnetStats.tokens_seen++; next_station = (src_mac + 1) % 128; mstp_state = MSTP_USE_TOKEN; }
                    else if (frame_type == 0x01 && dest_mac == sysCfg.mac_address) { uint8_t f[8] = { 0x55, 0xFF, 0x02, src_mac, sysCfg.mac_address, 0, 0, 0 }; f[7] = calc_header_crc(&f[2], 5); uart_tx(f, 8); mstp_state = MSTP_WAIT_TX_DONE; }
                    else if (dest_mac == sysCfg.mac_address && frame_type >= 0x05) { mstp_state = MSTP_ANSWER_DATA_REQUEST; }
                } else if (millis() - last_rx_time > Tno_token) { mstp_state = MSTP_NO_TOKEN; }
                break;
            case MSTP_NO_TOKEN:  mstp_state = MSTP_WAIT_FOR_PROPORTIONAL; timer_silence = millis(); break;
            case MSTP_WAIT_FOR_PROPORTIONAL:
                if (millis() - timer_silence > 50) { last_rx_time = millis(); uint8_t f[8]={0x55,0xFF,0x01, (uint8_t)((sysCfg.mac_address+1)%128), sysCfg.mac_address,0,0,0}; f[7]=calc_header_crc(&f[2],5); uart_tx(f,8); bacnetStats.tokens_seen++; mstp_state = MSTP_WAIT_TX_DONE; }
                break;
            case MSTP_USE_TOKEN:
                token_skip_count++;
                if (token_skip_count >= sysCfg.token_skip) { 
                    if (xSemaphoreTake(cache_mutex, 0)) {
                        if (!bacnet_network_cache.empty()) {
                            uint8_t apdu[64]; uint16_t apdu_len = 0;
                            auto& dev = bacnet_network_cache[0];
                            if (!dev.discovery_done) {
                                // --- DISCOVERY LOGIC ---
                                if (disc_step == DISC_DEV_ID) apdu_len = build_read_property_apdu(apdu, current_invoke_id++, 8, 4194303, 75, -1);
                                else if (disc_step == DISC_DEV_NAME) apdu_len = build_read_property_apdu(apdu, current_invoke_id++, 8, dev.device_id, 77, -1);
                                else if (disc_step == DISC_DEV_VENDOR) apdu_len = build_read_property_apdu(apdu, current_invoke_id++, 8, dev.device_id, 121, -1);
                                else if (disc_step == DISC_OBJ_COUNT) apdu_len = build_read_property_apdu(apdu, current_invoke_id++, 8, dev.device_id, 76, 0);
                                else if (disc_obj_idx < dev.objects.size()) {
                                    auto& o = dev.objects[disc_obj_idx];
                                    if (disc_step == DISC_OBJ_OID) apdu_len = build_read_property_apdu(apdu, current_invoke_id++, 8, dev.device_id, 76, disc_obj_idx + 1);
                                    else if (disc_step == DISC_OBJ_NAME) apdu_len = build_read_property_apdu(apdu, current_invoke_id++, o.type, o.instance, 77, -1);
                                    else if (disc_step == DISC_OBJ_UNITS) apdu_len = build_read_property_apdu(apdu, current_invoke_id++, o.type, o.instance, 117, -1);
                                    else if (disc_step == DISC_OBJ_VALUE) apdu_len = build_read_property_apdu(apdu, current_invoke_id++, o.type, o.instance, 85, -1);
                                } else { 
                                    dev.discovery_done = true; save_device_objects(dev.device_id); 
                                    z_log("[BACNET] Discovery Successfully Finalized.\n"); 
                                }
                            } else {
                                // --- POLLING LOGIC ---
                                size_t count = dev.objects.size();
                                if (count > 0) {
                                    current_poll_idx = (current_poll_idx + 1) % count;
                                    auto& o = dev.objects[current_poll_idx];
                                    if (o.enabled && o.type != 8 && o.type != 65535) { 
                                        apdu_len = build_read_property_apdu(apdu, current_invoke_id++, o.type, o.instance, 85, -1); 
                                    }
                                }
                            }
                            if (apdu_len > 0) { token_skip_count = 0; waiting_for_reply = true; send_mstp_frame(dev.mac_address, 0x05, apdu, apdu_len); mstp_state = MSTP_AWAIT_REPLY; }
                            else { mstp_state = MSTP_DONE_WITH_TOKEN; }
                        } else { mstp_state = MSTP_DONE_WITH_TOKEN; }
                        xSemaphoreGive(cache_mutex);
                    } else { mstp_state = MSTP_DONE_WITH_TOKEN; }
                } else { mstp_state = MSTP_DONE_WITH_TOKEN; }
                break;
            case MSTP_WAIT_TX_DONE: if (uart_wait_tx_done(RS485_UART_PORT, 0) == ESP_OK) { timer_silence = millis(); if (waiting_for_reply) mstp_state = MSTP_AWAIT_REPLY; else { last_rx_time = millis(); mstp_state = MSTP_IDLE; } } break;
            case MSTP_AWAIT_REPLY:
                if (ReceivedValidFrame && dest_mac == sysCfg.mac_address) {
                    waiting_for_reply = false;
                    uint16_t pos = (data_buf[1] & 0x20) ? 10 : 2; 
                    if (pos < data_len) {
                        uint8_t pdu_type = data_buf[pos] & 0xF0;
                        if (pdu_type == 0x50 || pdu_type == 0x40 || pdu_type == 0x20) { 
                            if (!bacnet_network_cache.empty() && !bacnet_network_cache[0].discovery_done) {
                                z_log("[BACNET] Step %d Error (Normal for some types)\n", (int)disc_step);
                                if (disc_step == DISC_OBJ_VALUE || disc_step == DISC_OBJ_UNITS || (disc_step == DISC_OBJ_NAME && bacnet_network_cache[0].objects[disc_obj_idx].type > 1000)) { 
                                    disc_obj_idx++; disc_step = DISC_OBJ_OID; 
                                    if (disc_obj_idx % 10 == 0) save_device_objects(bacnet_network_cache[0].device_id);
                                } else disc_step = static_cast<DISC_STEP_T>((int)disc_step + 1);
                            }
                        } else if (pos + 3 < data_len && data_buf[pos] == 0x30) {
                            pos += 3; BACnetTag t;
                            while (pos < data_len && decode_next_tag(data_buf, &pos, data_len, &t)) {
                                if (t.isOpening && t.number == 3) {
                                    if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(50))) {
                                        if (!bacnet_network_cache.empty()) {
                                            auto& dev = bacnet_network_cache[0];
                                            BACnetTag val_tag;
                                            if (decode_next_tag(data_buf, &pos, data_len, &val_tag)) {
                                                if (!dev.discovery_done) {
                                                    // --- DISCOVERY PROCESSING ---
                                                    if (disc_step == DISC_DEV_ID) { dev.device_id = (((data_buf[pos]<<24)|(data_buf[pos+1]<<16)|(data_buf[pos+2]<<8)|data_buf[pos+3]) & 0x3FFFFF); z_log("[BACNET] ID: %lu\n", (unsigned long)dev.device_id); disc_step = DISC_DEV_NAME; }
                                                    else if (disc_step == DISC_DEV_NAME) { char n[33]; uint8_t enc = data_buf[pos]; if (enc == 0) { uint16_t slen = std::min((int)val_tag.len - 1, 32); memcpy(n, &data_buf[pos+1], slen); n[slen]=0; } else { int slen=0; for(int i=2; i<val_tag.len && slen<32; i+=2) n[slen++]=data_buf[pos+i]; n[slen]=0; } dev.name = String(n); z_log("[BACNET] Name: %s\n", n); disc_step = DISC_DEV_VENDOR; }
                                                    else if (disc_step == DISC_DEV_VENDOR) { char n[33]; uint8_t enc = data_buf[pos]; if (enc == 0) { uint16_t slen = std::min((int)val_tag.len - 1, 32); memcpy(n, &data_buf[pos+1], slen); n[slen]=0; } else { int slen=0; for(int i=2; i<val_tag.len && slen<32; i+=2) n[slen++]=data_buf[pos+i]; n[slen]=0; } dev.vendor = String(n); z_log("[BACNET] Vendor: %s\n", n); disc_step = DISC_OBJ_COUNT; }
                                                    else if (disc_step == DISC_OBJ_COUNT) {
                                                        uint32_t count = 0; for(int i=0; i<val_tag.len; i++) count = (count << 8) | data_buf[pos+i];
                                                        z_log("[BACNET] Object Count: %lu\n", (unsigned long)count);
                                                        dev.objects.clear(); dev.objects.reserve(count);
                                                        for(int i=0; i<count; i++) { BACnetObject o; o.name="Unknown"; o.enabled=false; o.type=65535; dev.objects.push_back(o); }
                                                        disc_obj_idx = 0; disc_step = DISC_OBJ_OID;
                                                    } else if (disc_obj_idx < dev.objects.size()) {
                                                        auto& o = dev.objects[disc_obj_idx];
                                                        if (disc_step == DISC_OBJ_OID) { 
                                                            uint32_t oid = (data_buf[pos]<<24)|(data_buf[pos+1]<<16)|(data_buf[pos+2]<<8)|data_buf[pos+3]; 
                                                            o.type = oid >> 22; o.instance = oid & 0x3FFFFF; 
                                                            // Logic exhaustive de filtrage par type (ASHRAE 135)
                                                            if((o.type <= 5) || (o.type >= 12 && o.type <= 14) || (o.type == 19) || (o.type == 23 || o.type == 24) || (o.type >= 39 && o.type <= 50) || (o.type == 54 || o.type == 55)) o.enabled = true;
                                                            else o.enabled = false;
                                                            z_log("[BACNET] Obj %u OID: T%u I%lu\n", disc_obj_idx+1, o.type, (unsigned long)o.instance); 
                                                            disc_step = DISC_OBJ_NAME; 
                                                        }
                                                        else if (disc_step == DISC_OBJ_NAME) { 
                                                            char n[33]; uint8_t enc = data_buf[pos]; 
                                                            if (enc == 0) { uint16_t slen = std::min((int)val_tag.len - 1, 32); memcpy(n, &data_buf[pos+1], slen); n[slen]=0; } 
                                                            else { int slen=0; for(int i=2; i<val_tag.len && slen<32; i+=2) n[slen++]=data_buf[pos+i]; n[slen]=0; } 
                                                            o.name = String(n); z_log("[BACNET] Obj %u Name: %s\n", disc_obj_idx+1, n); 
                                                            
                                                            // Détermination de l'étape suivante (Units, Value ou Next Object)
                                                            if(o.type <= 2 || o.type == 23 || o.type == 24 || o.type == 46) disc_step = DISC_OBJ_UNITS; 
                                                            else if(o.enabled) disc_step = DISC_OBJ_VALUE;
                                                            else {
                                                                // Skip units/value for system objects
                                                                disc_obj_idx++; disc_step = DISC_OBJ_OID;
                                                                if (disc_obj_idx % 10 == 0) save_device_objects(dev.device_id);
                                                                if(disc_obj_idx >= dev.objects.size()) { dev.discovery_done=true; save_device_objects(dev.device_id); z_log("[BACNET] Scan Complete\n"); }
                                                            }
                                                        }
                                                        else if (disc_step == DISC_OBJ_UNITS) { 
                                                            uint32_t u=0; for(int i=0; i<val_tag.len; i++) u = (u << 8) | data_buf[pos+i]; 
                                                            o.units = u; 
                                                            o.unit_text = get_unit_text(u); 
                                                            if (o.unit_text == "" || o.unit_text.startsWith("Unit")) {
                                                                z_log("[BACNET] Obj %u Units: %s (Raw:%lu)\n", disc_obj_idx+1, o.unit_text.c_str(), (unsigned long)u);
                                                            } else {
                                                                z_log("[BACNET] Obj %u Units: %s\n", disc_obj_idx+1, o.unit_text.c_str());
                                                            }
                                                            disc_step = DISC_OBJ_VALUE; 
                                                        }
                                                        else if (disc_step == DISC_OBJ_VALUE) {
                                                            if (val_tag.number == 4) { float v; uint32_t tmp; memcpy(&tmp, &data_buf[pos], 4); tmp = __builtin_bswap32(tmp); memcpy(&v, &tmp, 4); o.present_value = v; }
                                                            else { uint32_t v=0; for(int i=0; i<val_tag.len; i++) v = (v << 8) | data_buf[pos+i]; o.present_value = (float)v; }
                                                            o.last_update = millis(); z_log("[BACNET] Obj %u Value: %.2f\n", disc_obj_idx+1, o.present_value);
                                                            disc_obj_idx++; disc_step = DISC_OBJ_OID;
                                                            if (disc_obj_idx % 10 == 0) { save_device_objects(dev.device_id); z_log("[NVS] Incremental Save (%d/%d)\n", disc_obj_idx, (uint32_t)dev.objects.size()); }
                                                            if(disc_obj_idx >= dev.objects.size()) { dev.discovery_done=true; save_device_objects(dev.device_id); z_log("[BACNET] Scan Complete\n"); }
                                                        }
                                                    }
                                                } else {
                                                    // --- POLLING PROCESSING ---
                                                    if (val_tag.number == 4) { float v; uint32_t tmp; memcpy(&tmp, &data_buf[pos], 4); tmp = __builtin_bswap32(tmp); memcpy(&v, &tmp, 4); if(current_poll_idx < dev.objects.size()){ auto& o=dev.objects[current_poll_idx]; o.present_value=v; o.last_update=millis(); z_log("[BACNET] Polled %lu: %.2f\n", (unsigned long)o.instance, v); } }
                                                    else if (val_tag.number == 2 || val_tag.number == 9) { uint32_t v=0; for(int i=0; i<val_tag.len; i++) v=(v<<8)|data_buf[pos+i]; if(current_poll_idx < dev.objects.size()){ auto& o=dev.objects[current_poll_idx]; o.present_value=(float)v; o.last_update=millis(); z_log("[BACNET] Polled %lu: %lu\n", (unsigned long)o.instance, (unsigned long)v); } }
                                                }
                                                pos += val_tag.len;
                                            }
                                        }
                                        xSemaphoreGive(cache_mutex);
                                    }
                                } else pos += t.len;
                            }
                        }
                    }
                    mstp_state = MSTP_DONE_WITH_TOKEN;
                } else if (millis() - timer_silence > 300) { 
                    waiting_for_reply = false;
                    if (!bacnet_network_cache.empty() && !bacnet_network_cache[0].discovery_done) {
                        z_log("[MSTP] Timeout Step %d\n", (int)disc_step);
                        if (disc_step == DISC_OBJ_VALUE || disc_step == DISC_OBJ_UNITS) { 
                            disc_obj_idx++; disc_step = DISC_OBJ_OID; 
                            if (disc_obj_idx % 10 == 0) save_device_objects(bacnet_network_cache[0].device_id);
                        } else disc_step = static_cast<DISC_STEP_T>((int)disc_step + 1);
                        if(disc_obj_idx >= bacnet_network_cache[0].objects.size()) { bacnet_network_cache[0].discovery_done=true; save_device_objects(bacnet_network_cache[0].device_id); }
                    }
                    mstp_state = MSTP_DONE_WITH_TOKEN;
                }
                break;
            case MSTP_ANSWER_DATA_REQUEST: z_log("[MSTP] Serving request...\n"); mstp_state = MSTP_IDLE; break;
            case MSTP_DONE_WITH_TOKEN: mstp_state = MSTP_PASS_TOKEN; break;
            case MSTP_PASS_TOKEN: { 
                uint8_t f[8] = { 0x55, 0xFF, 0x00, next_station, sysCfg.mac_address, 0, 0, 0 }; 
                f[7] = calc_header_crc(&f[2], 5); uart_tx(f, 8); 
                waiting_for_reply = false; 
                last_rx_time = millis(); 
                mstp_state = MSTP_WAIT_TX_DONE; 
                break; 
            }
        }
        vTaskDelay(1);
    }
}

void setup_bacnet_engine() {
    cache_mutex = xSemaphoreCreateMutex();
    bacnet_job_queue = xQueueCreate(10, sizeof(BACnetJob));
    mqtt_publish_queue = xQueueCreate(30, sizeof(MQTTPublishJob));
    const uart_config_t uc = { .baud_rate = 38400, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .rx_flow_ctrl_thresh = 122, .source_clk = UART_SCLK_APB };
    uart_driver_install(RS485_UART_PORT, 2048, 2048, 20, &uart_evt_queue, 0);
    uart_param_config(RS485_UART_PORT, &uc);
    uart_set_pin(RS485_UART_PORT, TX_PIN, RX_PIN, RTS_PIN, UART_PIN_NO_CHANGE);
    uart_set_mode(RS485_UART_PORT, UART_MODE_RS485_HALF_DUPLEX);
    uart_set_rx_timeout(RS485_UART_PORT, 2); 
    xTaskCreatePinnedToCore(bacnet_task, "BACnet", 16384, NULL, 15, NULL, 1);
    z_log("[BACNET] Engine Initialized\n");
}
bool enqueue_bacnet_job(BACnetJob job) { if (bacnet_job_queue == NULL) return false; return xQueueSend(bacnet_job_queue, &job, 0) == pdTRUE; }
bool enqueue_mqtt_publish(MQTTPublishJob pubJob) { if (mqtt_publish_queue == NULL) return false; return xQueueSend(mqtt_publish_queue, &pubJob, 0) == pdTRUE; }
