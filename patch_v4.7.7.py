import sys

with open('src/z_bacnet.cpp', 'r') as f:
    lines = f.readlines()

new_lines = []
for line in lines:
    # 1. Update Version
    line = line.replace('Engine v4.7.6 - Active Scan', 'Engine v4.7.7 - Discovery Fixed')
    
    # 2. Fix the hardcoded '4' and add logging
    if 'send_mstp_frame(4,' in line:
        target = '(!bacnet_network_cache.empty() ? bacnet_network_cache[0].mac_address : 4)'
        log_line = f'                            z_log("[BACNET] Sending Request to MAC %u\\n", {target});\n'
        new_lines.append(log_line)
        line = line.replace('send_mstp_frame(4,', f'send_mstp_frame({target},')
    
    new_lines.append(line)

with open('src/z_bacnet.cpp', 'w') as f:
    f.writelines(new_lines)
