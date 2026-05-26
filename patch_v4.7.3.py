import sys

with open('src/z_bacnet.cpp', 'r') as f:
    lines = f.readlines()

new_lines = []
for line in lines:
    # Fix empty cache access (the most critical)
    line = line.replace('bacnet_network_cache[0]', '(!bacnet_network_cache.empty() ? bacnet_network_cache[0] : dummy_dev)')
    
    # Version update
    line = line.replace('Engine v4.5.32 - Expert Async FSM', 'Engine v4.7.3 - Autonomous Discovery')
    
    # Global dummy dev for safety
    if 'std::vector<BACnetDevice> bacnet_network_cache;' in line:
        new_lines.append(line)
        new_lines.append('BACnetDevice dummy_dev;\n')
        continue

    # NPCI & Error Handling
    if 'uint16_t pos = 2;' in line:
        new_lines.append(line)
        new_lines.append('                            if (data_buf[1] & 0x20) { pos += 3 + (data_buf[pos+2]) + 1; }\n')
        new_lines.append('                            if (data_buf[1] & 0x40) { pos += 3 + (data_buf[pos+2]); }\n')
        new_lines.append('                            if (pos >= data_len) { state = IDLE; continue; }\n')
        new_lines.append('                            uint8_t apdu_type = data_buf[pos];\n')
        new_lines.append('                            if ((apdu_type & 0xF0) == 0x50) {\n')
        new_lines.append('                                uint8_t err_code = (pos+5 < data_len) ? data_buf[pos+5] : 0;\n')
        new_lines.append('                                z_log("[BACNET] Error PDU 0x%02X (Code %u) from MAC %u\\n", data_buf[pos+2], err_code, header[2]);\n')
        new_lines.append('                            }\n')
        continue

    # Auto Who-Is logic
    if 'if (millis() - last_rx_time > 5000) {' in line:
        new_lines.append('        if (millis() - last_rx_time > 10000 && bacnet_network_cache.empty()) {\n')
        new_lines.append('            z_log("[BACNET] Auto-Discovery (Cache Empty)...\\n");\n')
        new_lines.append('            uint8_t payload[] = { 0x01, 0x20, 0xFF, 0xFF, 0x00, 0xFF, 0x10, 0x08 };\n')
        new_lines.append('            send_mstp_frame(255, 0x06, payload, sizeof(payload));\n')
        new_lines.append('            last_rx_time = millis();\n')
        new_lines.append('        }\n')
        new_lines.append(line)
        continue

    new_lines.append(line)

with open('src/z_bacnet.cpp', 'w') as f:
    f.writelines(new_lines)
