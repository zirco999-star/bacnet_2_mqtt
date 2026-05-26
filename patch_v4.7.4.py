import sys

with open('src/z_bacnet.cpp', 'r') as f:
    lines = f.readlines()

new_lines = []
for line in lines:
    # 1. Update Version
    line = line.replace('Engine v4.7.3 - Autonomous Discovery', 'Engine v4.7.4 - Token Discovery')

    # 2. Add Token Discovery Logic
    if 'if (!waiting_for_reply && (bacnetStats.tokens_seen % 35 == 0 || !scan_done)) {' in line:
        new_lines.append(line)
        new_lines.append('                if (bacnet_network_cache.empty() && bacnetStats.tokens_seen % 50 == 0) {\n')
        new_lines.append('                    z_log("[BACNET] Cache empty, broadcasting Who-Is...\\n");\n')
        new_lines.append('                    uint8_t payload[] = { 0x01, 0x20, 0xFF, 0xFF, 0x00, 0xFF, 0x10, 0x08 };\n')
        new_lines.append('                    send_mstp_frame(255, 0x06, payload, sizeof(payload));\n')
        new_lines.append('                    waiting_for_reply = false; // No reply expected for 0x06\n')
        new_lines.append('                }\n')
        continue

    # 3. Log token traffic (optional but useful)
    if 'if (header[0] > 1)' in line:
        line = line.replace('if (header[0] > 1)', 'if (header[0] > 1 || (header[0] <= 1 && bacnetStats.tokens_seen % 500 == 0))')

    new_lines.append(line)

with open('src/z_bacnet.cpp', 'w') as f:
    f.writelines(new_lines)
