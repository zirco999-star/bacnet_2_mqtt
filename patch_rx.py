import re

with open('src/z_bacnet.cpp', 'r') as f:
    content = f.read()

# 1. Mise à jour version
content = content.replace('Engine v4.7.12 - Throttled Scan', 'Engine v4.7.13 - Ack & Release')

# 2. Ajout de la réinitialisation du flag lors de la réception d'un ComplexAck
old_ack = r'if \(pos \+ 3 < data_len && data_buf\[pos\] == 0x30\) \{ // Complex Ack\s*pos \+= 3; BACnetTag t;'
new_ack = """if (pos + 3 < data_len && data_buf[pos] == 0x30) { // Complex Ack
                                waiting_for_reply = false; // RELEASE TOKEN
                                pos += 3; BACnetTag t;"""
content = re.sub(old_ack, new_ack, content)

# 3. Ajout de la gestion des erreurs (PDU Type 0x50 Error)
old_npdu = r'if \(data_len > 2 && data_buf\[0\] == 0x01\) \{ // NPDU\s*pos = \(data_buf\[1\] & 0x20\) \? 10 : 2;\s*if \(pos \+ 3 < data_len && data_buf\[pos\] == 0x30\) \{ // Complex Ack'

new_npdu = """if (data_len > 2 && data_buf[0] == 0x01) { // NPDU
                            pos = (data_buf[1] & 0x20) ? 10 : 2; 
                            if (pos < data_len && (data_buf[pos] & 0xF0) == 0x50) { // Error PDU
                                waiting_for_reply = false;
                                z_log("[BACNET] Received Error PDU!\\n");
                            } else if (pos + 3 < data_len && data_buf[pos] == 0x30) { // Complex Ack"""
content = re.sub(old_npdu, new_npdu, content)

# 4. Augmenter légèrement la limite de temps de réponse du Distech pour ne pas timeout trop vite
content = content.replace('if (waiting_for_reply && (millis() - last_req_time > 1500)) waiting_for_reply = false;',
                          'if (waiting_for_reply && (millis() - last_req_time > 1500)) { waiting_for_reply = false; z_log("[BACNET] Reply Timeout\\n"); }')

with open('src/z_bacnet.cpp', 'w') as f:
    f.write(content)
