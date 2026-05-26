import sys

with open('src/z_bacnet.cpp', 'r') as f:
    content = f.read()

# 1. New Generic Function
func_code = """
uint16_t build_read_property_apdu(uint8_t* buffer, uint8_t invoke_id, uint16_t obj_type, uint32_t obj_instance, uint8_t property_id, int32_t array_index) {
    uint16_t len = 0;
    buffer[len++] = 0x01; // NPCI Version
    buffer[len++] = 0x04; // NPCI Expecting Reply
    buffer[len++] = 0x02; // APDU Confirmed Request + Segmented Response Accepted
    buffer[len++] = 0x73; // Max Segments > 64, Max APDU = 102 bytes
    buffer[len++] = invoke_id;
    buffer[len++] = 0x0C; // Service Choice: ReadProperty
    buffer[len++] = 0x0C; // Context Tag 0 (Object ID), Length 4
    uint32_t oid = ((uint32_t)obj_type << 22) | (obj_instance & 0x3FFFFF);
    buffer[len++] = (oid >> 24) & 0xFF;
    buffer[len++] = (oid >> 16) & 0xFF;
    buffer[len++] = (oid >> 8) & 0xFF;
    buffer[len++] = oid & 0xFF;
    buffer[len++] = 0x19; // Context Tag 1 (Property ID), Length 1
    buffer[len++] = property_id;
    if (array_index >= 0) {
        if (array_index <= 255) {
            buffer[len++] = 0x29; // Context Tag 2 (Array Index), Length 1
            buffer[len++] = (uint8_t)array_index;
        } else {
            buffer[len++] = 0x2A; // Context Tag 2, Length 2
            buffer[len++] = (array_index >> 8) & 0xFF;
            buffer[len++] = array_index & 0xFF;
        }
    }
    return len;
}
"""

content = content.replace('static void bacnet_task(void *pv) {', func_code + '\nstatic void bacnet_task(void *pv) {')
content = content.replace('Engine v4.5.32 - Expert Async FSM', 'Engine v4.7.9 - Normative APDU')

# 2. Fix the Scan loop (Discovery)
scan_block_old = """                        if (disc_step == DISC_LIST) {
                            uint8_t apdu[] = { 0x01, 0x00, 0x00, 0x05, current_invoke_id, 0x0C, 0x0C, 0x02, 0x00, 0x00, 0x00, 0x19, 0x4C, 0x29, current_scan_index };
                            if (target_device_id != 0x3FFFFF) { apdu[8]=(target_device_id>>16)&0xFF; apdu[9]=(target_device_id>>8)&0xFF; apdu[10]=target_device_id&0xFF; }
                            if(!bacnet_network_cache.empty()) send_mstp_frame(bacnet_network_cache[0].mac_address, 0x05, apdu, sizeof(apdu));
                        } else if (!bacnet_network_cache.empty()) {
                            if(bacnet_network_cache.empty()) { state = IDLE; continue; } auto& o = bacnet_network_cache[0].objects[disc_obj_ptr];
                            uint8_t pid = (disc_step == DISC_NAME) ? 77 : 85;
                            uint8_t apdu[] = { 0x01, 0x00, 0x00, 0x05, current_invoke_id, 0x0C, 0x0C, (uint8_t)((o.type>>2)&0xFF), (uint8_t)((o.type<<6)|(o.instance>>16)), (uint8_t)(o.instance>>8), (uint8_t)o.instance, 0x19, pid };
                            if(!bacnet_network_cache.empty()) send_mstp_frame(bacnet_network_cache[0].mac_address, 0x05, apdu, sizeof(apdu));
                        }"""

scan_block_new = """                        uint8_t apdu[32]; uint16_t apdu_len = 0;
                        uint8_t target_mac = (!bacnet_network_cache.empty() ? bacnet_network_cache[0].mac_address : 4);
                        if (disc_step == DISC_LIST) {
                            apdu_len = build_read_property_apdu(apdu, current_invoke_id, 8, (target_device_id == 0x3FFFFF ? 4194303 : target_device_id), 76, current_scan_index);
                        } else if (!bacnet_network_cache.empty()) {
                            auto& o = bacnet_network_cache[0].objects[disc_obj_ptr];
                            apdu_len = build_read_property_apdu(apdu, current_invoke_id, o.type, o.instance, (disc_step == DISC_NAME ? 77 : 85), -1);
                        }
                        if (apdu_len > 0) {
                            z_log("[BACNET] Query MAC %u (Step %d)\\n", target_mac, disc_step);
                            send_mstp_frame(target_mac, 0x05, apdu, apdu_len);
                        }"""

content = content.replace(scan_block_old, scan_block_new)

# 3. Fix the Poll loop (Normal operation)
poll_block_old = """                        current_poll_idx = (current_poll_idx + 1) % bacnet_network_cache[0].objects.size();
                        if(bacnet_network_cache.empty()) { state = IDLE; continue; } auto& o = bacnet_network_cache[0].objects[current_poll_idx];
                        if (o.enabled && bacnet_network_cache[0].enabled && o.type != 8) {
                            uint8_t apdu[] = { 0x01, 0x00, 0x00, 0x05, current_invoke_id, 0x0C, 0x0C, (uint8_t)((o.type>>2)&0xFF), (uint8_t)((o.type<<6)|(o.instance>>16)), (uint8_t)(o.instance>>8), (uint8_t)o.instance, 0x19, 0x55 };
                            if(!bacnet_network_cache.empty()) send_mstp_frame(bacnet_network_cache[0].mac_address, 0x05, apdu, sizeof(apdu));
                            req_sent = true; waiting_for_reply = true;
                        }"""

poll_block_new = """                        current_poll_idx = (current_poll_idx + 1) % bacnet_network_cache[0].objects.size();
                        auto& o = bacnet_network_cache[0].objects[current_poll_idx];
                        if (o.enabled && bacnet_network_cache[0].enabled && o.type != 8) {
                            uint8_t apdu[32];
                            uint16_t apdu_len = build_read_property_apdu(apdu, current_invoke_id, o.type, o.instance, 85, -1);
                            send_mstp_frame(bacnet_network_cache[0].mac_address, 0x05, apdu, apdu_len);
                            req_sent = true; waiting_for_reply = true;
                        }"""

content = content.replace(poll_block_old, poll_block_new)

with open('src/z_bacnet.cpp', 'w') as f:
    f.write(content)
