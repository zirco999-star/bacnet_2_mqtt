import os

filepath = 'src/z_bacnet.cpp'
with open(filepath, 'r') as f:
    content = f.read()

# 1. Insertion de la fonction build_read_property_apdu
if 'uint16_t build_read_property_apdu' not in content:
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
    content = content.replace('static void bacnet_task(void *pv) {', func + '\nstatic void bacnet_task(void *pv) {')

# 2. Mise à jour des logs et versions
content = content.replace('z_log("[BACNET] Engine v4.5.32 Initialized\\n");', 'z_log("[BACNET] Engine v4.7.10 Initialized\\n");')

# 3. Remplacement du bloc DISC_LIST (Ligne par ligne pour éviter les erreurs de matching)
old_list_apdu = 'uint8_t apdu[] = { 0x01, 0x00, 0x00, 0x05, current_invoke_id, 0x0C, 0x0C, 0x02, 0x00, 0x00, 0x00, 0x19, 0x4C, 0x29, current_scan_index };'
new_list_logic = """
                            uint8_t apdu[32];
                            uint16_t apdu_len = build_read_property_apdu(apdu, current_invoke_id, 8, (target_device_id == 0x3FFFFF ? 4194303 : target_device_id), 76, current_scan_index);
                            z_log("[BACNET] Query MAC %u (Index %u)\\n", (!bacnet_network_cache.empty() ? bacnet_network_cache[0].mac_address : 4), current_scan_index);
                            send_mstp_frame((!bacnet_network_cache.empty() ? bacnet_network_cache[0].mac_address : 4), 0x05, apdu, apdu_len);
"""
# On cherche la ligne de l'ancien apdu et on la remplace ainsi que les lignes suivantes de calcul de target_device_id et l'envoi.
import re
pattern = r'uint8_t apdu\[\] = \{ 0x01, 0x00, 0x00, 0x05, current_invoke_id, 0x0C, 0x0C, 0x02, 0x00, 0x00, 0x00, 0x19, 0x4C, 0x29, current_scan_index \};.*?send_mstp_frame\(4, 0x05, apdu, sizeof\(apdu\)\);'
content = re.sub(pattern, new_list_logic.strip(), content, flags=re.DOTALL)

# 4. Remplacement du bloc de polling
old_poll_apdu = r'uint8_t apdu\[\] = \{ 0x01, 0x00, 0x00, 0x05, current_invoke_id, 0x0C, 0x0C, \(uint8_t\)\(\(o\.type>>2\)&0xFF\), .*?send_mstp_frame\(4, 0x05, apdu, sizeof\(apdu\)\);'
new_poll_logic = """
                            uint8_t apdu[32];
                            uint16_t apdu_len = build_read_property_apdu(apdu, current_invoke_id, o.type, o.instance, 0x55, -1);
                            send_mstp_frame(bacnet_network_cache[0].mac_address, 0x05, apdu, apdu_len);
"""
content = re.sub(old_poll_apdu, new_poll_logic.strip(), content, flags=re.DOTALL)

# 5. Restauration de la découverte passive
passive_logic = """
                        bacnetStats.ms_msgs_rx++;
                        if (header[2] != sysCfg.mac_address && header[2] < 128) {
                            bool known = false;
                            for(auto& d : bacnet_network_cache) if(d.mac_address == header[2]) known = true;
                            if(!known) {
                                z_log("[BACNET] Passive Discovery: Found MAC %u\\n", header[2]);
                                BACnetDevice d; d.mac_address = header[2]; d.device_id = 4194303; d.discovery_done = false;
                                d.enabled = true; d.name = "Unknown"; d.vendor = "Detected";
                                bacnet_network_cache.push_back(d);
                            }
                        }
"""
if 'Passive Discovery' not in content:
    content = content.replace('bacnetStats.ms_msgs_rx++;', passive_logic.strip())

with open(filepath, 'w') as f:
    f.write(content)
