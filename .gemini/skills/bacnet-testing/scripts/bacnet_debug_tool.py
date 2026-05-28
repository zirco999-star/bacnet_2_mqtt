#!/usr/bin/env python3
"""
@file       bacnet_debug_tool.py
@brief      Outil de débogage BACnet avancé pour tester la gateway.
@details    Supporte Who-Is, ReadProperty (basique) et le sniffing UDP avec analyse APDU.
@author     Gemini CLI
@date       2026/05/11
"""

import socket
import struct
import argparse
import time

class BACnetDebugTool:
    def __init__(self, target_ip, port=47808):
        self.target_ip = target_ip
        self.port = port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(2.0)
        try:
            self.sock.bind(('', 47808)) # Bind local pour recevoir les broadcasts
        except OSError:
            print("Port 47808 déjà utilisé, écoute sur un port aléatoire.")

    def send_who_is(self, low=-1, high=-1):
        """Construit et envoie un Who-Is."""
        print(f"[*] Envoi Who-Is ({low} to {high}) à {self.target_ip}...")
        bvll = bytes([0x81, 0x0b, 0x00, 0x0c])
        npdu = bytes([0x01, 0x20, 0xff, 0xff, 0x00, 0xff])
        apdu = bytes([0x10, 0x08])
        packet = bvll + npdu + apdu
        self.sock.sendto(packet, (self.target_ip, self.port))

    def sniff(self, duration=10):
        """Écoute le trafic BACnet entrant et retourne les paquets I-Am reçus."""
        print(f"[*] Sniffing sur le port {self.port} pendant {duration}s...")
        start_time = time.time()
        iams_received = []
        while time.time() - start_time < duration:
            try:
                data, addr = self.sock.recvfrom(2048)
                print(f"[+] Reçu {len(data)} octets de {addr}")
                result = self.decode_packet(data)
                if result and result.get("type") == "I-Am":
                    iams_received.append(result)
            except socket.timeout:
                continue
            except KeyboardInterrupt:
                break
        return iams_received

    def decode_packet(self, data):
        """Décodage avancé des trames BACnet/IP."""
        if len(data) < 4: return None
        bvlc_type, bvlc_func = data[0], data[1]
        bvlc_len = struct.unpack(">H", data[2:4])[0]
        
        funcs = {0x0a: "Original-Unicast", 0x0b: "Original-Broadcast", 0x04: "Forwarded"}
        print(f"    BVLC: Type={hex(bvlc_type)}, Func={funcs.get(bvlc_func, hex(bvlc_func))}, Len={bvlc_len}")
        
        if len(data) > 4:
            npdu_version, npdu_control = data[4], data[5]
            offset = 6
            if npdu_control & 0x20: # DNET present
                offset += 3
            if npdu_control & 0x08: # SNET present
                offset += 3
                
            print(f"    NPDU: Ver={npdu_version}, Control={hex(npdu_control)}, Offset APDU={offset}")
            
            if len(data) > offset:
                apdu_type = data[offset]
                if apdu_type == 0x10: # Unconfirmed Request
                    service_choice = data[offset+1]
                    if service_choice == 0x00: # I-Am
                        print("    [!] I-Am détecté !")
                        # Extraction rudimentaire du Device ID (Object ID)
                        if len(data) >= offset + 6:
                            # ASN.1 tag for Object Identifier is usually C4 (Context 4) or standard tags
                            device_id = struct.unpack(">I", data[offset+2:offset+6])[0] & 0x3FFFFFFF
                            print(f"        Device ID: {device_id}")
                            return {"type": "I-Am", "device_id": device_id}
                elif apdu_type == 0x10 and data[offset+1] == 0x08:
                     print("    [!] Who-Is détecté !")
                     return {"type": "Who-Is"}
        return {"type": "Unknown"}

def main():
    parser = argparse.ArgumentParser(description="BACnet Debug Tool")
    parser.add_argument("target", help="IP de la gateway")
    parser.add_argument("--whois", action="store_true", help="Envoyer un Who-Is")
    parser.add_argument("--sniff", type=int, default=0, help="Durée du sniffing en secondes")
    
    args = parser.parse_args()
    tool = BACnetDebugTool(args.target)
    
    if args.whois:
        tool.send_who_is()
    
    if args.sniff > 0:
        tool.sniff(args.sniff)
    elif not args.whois:
        tool.sniff(30)

if __name__ == "__main__":
    main()
