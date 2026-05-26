import os, re

filepath = 'src/z_bacnet.cpp'
with open(filepath, 'r') as f:
    content = f.read()

# 1. New Generic Function
func = """
uint16_t build_read_property_apdu(uint8_t* buffer, uint8_t invoke_id, uint16_t obj_type, uint32_t obj_instance, uint8_t property_id, int32_t array_index) {
    uint16_t len = 0;
    buffer[len++] = 0x01;
    buffer[len++] = 0x04;
    buffer[len++] = 0x02;
    buffer[len++] = 0x73;
    buffer[len++] = invoke_id;
    buffer[len++] = 0x0C;
    buffer[len++] = 0x0C;
    uint32_t oid = ((uint32_t)obj_type << 22) | (obj_instance & 0x3FFFFF);
    buffer[len++] = (oid >> 24) & 0xFF;
    buffer[len++] = (oid >> 16) & 0xFF;
    buffer[len++] = (oid >> 8) & 0xFF;
    buffer[len++] = oid & 0xFF;
    buffer[len++] = 0x19;
    buffer[len++] = property_id;
    if (array_index >= 0) {
        if (array_index <= 255) {
            buffer[len++] = 0x29;
            buffer[len++] = (uint8_t)array_index;
        } else {
            buffer[len++] = 0x2A;
            buffer[len++] = (array_index >> 8) & 0xFF;
            buffer[len++] = array_index & 0xFF;
        }
    }
    return len;
}
"""
if 'build_read_property_apdu' not in content:
    content = content.replace('static void bacnet_task(void *pv) {', func + '\\nstatic void bacnet_task(void *pv) {')

# 2. Passive Discovery
passive_logic = """                        bacnetStats.ms_msgs_rx++;
                        if (header[2] != sysCfg.mac_address && header[2] < 128) {
                            bool known = false;
                            for(auto& d : bacnet_network_cache) if(d.mac_address == header[2]) known = true;
                            if(!known) {
                                z_log("[BACNET] Passive Discovery: Found MAC %u\\n", header[2]);
                                BACnetDevice d; d.mac_address = header[2]; d.device_id = 4194303; d.discovery_done = false;
                                d.enabled = true; d.name = "Unknown"; d.vendor = "Detected";
                                bacnet_network_cache.push_back(d);
                            }
                        }"""
if 'Passive Discovery' not in content:
    content = content.replace('bacnetStats.ms_msgs_rx++;', passive_logic)

# 3. DISC_LIST replacement
old_list = r'if \(disc_step == DISC_LIST\) \{.*?send_mstp_frame\(4, 0x05, apdu, sizeof\(apdu\)\);'
new_list = """if (disc_step == DISC_LIST) {
                            uint8_t apdu[32];
                            uint16_t apdu_len = build_read_property_apdu(apdu, current_invoke_id, 8, (target_device_id == 0x3FFFFF ? 4194303 : target_device_id), 76, current_scan_index);
                            z_log("[BACNET] Query MAC 4 (Index %u)\\n", current_scan_index);
                            send_mstp_frame(4, 0x05, apdu, apdu_len);"""
content = re.sub(old_list, new_list, content, flags=re.DOTALL)

# 4. Polling replacement
old_poll = r'if \(o\.enabled && bacnet_network_cache\[0\]\.enabled && o\.type != 8\) \{.*?send_mstp_frame\(4, 0x05, apdu, sizeof\(apdu\)\);'
new_poll = """if (o.enabled && bacnet_network_cache[0].enabled && o.type != 8) {
                            uint8_t apdu[32];
                            uint16_t apdu_len = build_read_property_apdu(apdu, current_invoke_id, o.type, o.instance, 0x55, -1);
                            send_mstp_frame(bacnet_network_cache[0].mac_address, 0x05, apdu, apdu_len);"""
content = re.sub(old_poll, new_poll, content, flags=re.DOTALL)

# 5. Version
content = content.replace('Engine v4.5.32 Initialized', 'Engine v4.7.10 Initialized')

with open(filepath, 'w') as f:
    f.write(content)
