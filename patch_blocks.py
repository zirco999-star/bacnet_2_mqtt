import re
with open('src/z_bacnet.cpp', 'r') as f: content = f.read()
# Remplacement DISC_LIST
content = re.sub(r'if \(disc_step == DISC_LIST\) \{.*?send_mstp_frame\(4, 0x05, apdu, sizeof\(apdu\)\);', 
    'if (disc_step == DISC_LIST) {\\n                            uint8_t apdu[32]; uint16_t apdu_len = build_read_property_apdu(apdu, current_invoke_id, 8, 4194303, 76, current_scan_index);\\n                            z_log("[BACNET] Query MAC 4 (Index %u)\\n", current_scan_index);\\n                            send_mstp_frame(4, 0x05, apdu, apdu_len);', content, flags=re.DOTALL)
# Remplacement Polling
content = re.sub(r'if \(o\.enabled \&\& bacnet_network_cache\[0\]\.enabled \&\& o\.type != 8\) \{.*?send_mstp_frame\(4, 0x05, apdu, sizeof\(apdu\)\);',
    'if (o.enabled && bacnet_network_cache[0].enabled && o.type != 8) {\\n                            uint8_t apdu[32]; uint16_t apdu_len = build_read_property_apdu(apdu, current_invoke_id, o.type, o.instance, 0x55, -1);\\n                            send_mstp_frame(bacnet_network_cache[0].mac_address, 0x05, apdu, apdu_len);', content, flags=re.DOTALL)
with open('src/z_bacnet.cpp', 'w') as f: f.write(content)
