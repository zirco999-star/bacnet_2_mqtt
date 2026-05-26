import sys

with open('src/z_bacnet.cpp', 'r') as f:
    lines = f.readlines()

new_lines = []
for line in lines:
    # 1. Update Version
    line = line.replace('Engine v4.7.9 - Normative APDU', 'Engine v4.7.10 - Full Discovery')
    
    # 2. Inject Passive Discovery logic (the missing part!)
    if 'bacnetStats.ms_msgs_rx++;' in line:
        new_lines.append(line)
        new_lines.append('                        if (header[2] != sysCfg.mac_address && header[2] < 128) {\n')
        new_lines.append('                            bool known = false;\n')
        new_lines.append('                            for(auto& d : bacnet_network_cache) if(d.mac_address == header[2]) known = true;\n')
        new_lines.append('                            if(!known) {\n')
        new_lines.append('                                z_log("[BACNET] Passive Discovery: Found MAC %u\\n", header[2]);\n')
        new_lines.append('                                BACnetDevice d; d.mac_address = header[2]; d.device_id = 0; d.discovery_done = false;\n')
        new_lines.append('                                d.enabled = true; d.name = "Unknown"; d.vendor = "Detected";\n')
        new_lines.append('                                bacnet_network_cache.push_back(d);\n')
        new_lines.append('                            }\n')
        new_lines.append('                        }\n')
        continue

    # 3. Secure Who-Is broadcasting
    if 'if (bacnet_network_cache.empty() && bacnetStats.tokens_seen % 50 == 0) {' in line:
        line = line.replace('uint8_t payload[] = { 0x01, 0x00, 0x10, 0x08 };', 
                            'uint8_t payload[] = { 0x01, 0x04, 0x01, 0x00, 0x10, 0x08 }; // Normative Who-Is')
    
    new_lines.append(line)

with open('src/z_bacnet.cpp', 'w') as f:
    f.writelines(new_lines)
