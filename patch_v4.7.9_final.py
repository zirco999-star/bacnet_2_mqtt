import sys

with open('src/z_bacnet.cpp', 'r') as f:
    lines = f.readlines()

new_lines = []
inserted_func = False

for line in lines:
    # 1. Update Version
    line = line.replace('Engine v4.7.8 - Throttled Scan', 'Engine v4.7.9 - Normative APDU')

    # 2. Insert the generic build function before bacnet_task
    if 'static void bacnet_task(void *pv)' in line and not inserted_func:
        func = """
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
        new_lines.append(func)
        inserted_func = True

    # 3. Replace dynamic request blocks
    if 'if (disc_step == DISC_LIST) {' in line:
        new_lines.append(line)
        new_lines.append('                            uint8_t apdu[32];\n')
        new_lines.append('                            uint16_t apdu_len = build_read_property_apdu(apdu, current_invoke_id, 8, (target_device_id == 0x3FFFFF ? 4194303 : target_device_id), 76, current_scan_index);\n')
        new_lines.append('                            z_log("[BACNET] Scanning MAC %u (Index %u)...\\n", (!bacnet_network_cache.empty() ? bacnet_network_cache[0].mac_address : 4), current_scan_index);\n')
        new_lines.append('                            send_mstp_frame((!bacnet_network_cache.empty() ? bacnet_network_cache[0].mac_address : 4), 0x05, apdu, apdu_len);\n')
        skip_next = True # We need to skip the old hardcoded block
        continue
        
    # Simplified: for other blocks, I will do a more surgical replacement in the next turn if needed
    # but let's try to fix the main discovery first.
    
    new_lines.append(line)

with open('src/z_bacnet.cpp', 'w') as f:
    f.writelines(new_lines)
