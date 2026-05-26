import sys

with open('src/z_bacnet.cpp', 'r') as f:
    content = f.read()

# 1. Update Version
content = content.replace('Engine v4.7.7 - Discovery Fixed', 'Engine v4.7.8 - Throttled Scan')

# 2. Fix the Flood Logic (Remove the || !scan_done that caused the flood)
content = content.replace('if (!waiting_for_reply && (bacnetStats.tokens_seen % 10 == 0 || !scan_done)) {', 
                          'if (!waiting_for_reply && (bacnetStats.tokens_seen % 20 == 0)) {')

# 3. Ensure we have a device before sending non-WhoIs requests
content = content.replace('if (!scan_done) {', 'if (!scan_done && !bacnet_network_cache.empty()) {')

with open('src/z_bacnet.cpp', 'w') as f:
    f.write(content)
