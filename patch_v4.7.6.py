import sys

with open('src/z_bacnet.cpp', 'r') as f:
    content = f.read()

# 1. Update Version
content = content.replace('Engine v4.7.5 - Passive Discovery', 'Engine v4.7.6 - Active Scan')

# 2. Faster Modulo & Send Log
content = content.replace('bacnetStats.tokens_seen % 35 == 0', 'bacnetStats.tokens_seen % 10 == 0')
content = content.replace('if(!bacnet_network_cache.empty()) send_mstp_frame', 'z_log("[BACNET] Sending Scan Request to MAC %u...\\n", (!bacnet_network_cache.empty() ? bacnet_network_cache[0].mac_address : 0)); last_req_time = millis(); if(!bacnet_network_cache.empty()) send_mstp_frame')

with open('src/z_bacnet.cpp', 'w') as f:
    f.write(content)
