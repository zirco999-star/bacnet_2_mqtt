#!/usr/bin/env python3
"""
@file       mstp_simulator.py
@brief      Simulateur MS/TP basique pour tester la Gateway sans matériel.
@details    Crée une paire de ports virtuels PTY et répond aux trames MS/TP.
@author     Gemini CLI
@date       2026/05/11
"""

import os
import pty
import serial
import time
import threading

class MSTPSimulator:
    def __init__(self, mac_address=2):
        self.mac_address = mac_address
        self.master_fd, self.slave_fd = pty.openpty()
        self.port_name = os.ttyname(self.slave_fd)
        print(f"[*] Port virtuel MS/TP créé : {self.port_name}")
        self.running = False
        self.thread = None

    def start(self):
        self.running = True
        self.thread = threading.Thread(target=self._loop)
        self.thread.start()
        print(f"[*] Simulateur MS/TP démarré sur l'adresse MAC {self.mac_address}")

    def stop(self):
        self.running = False
        if self.thread:
            self.thread.join()
        os.close(self.master_fd)
        os.close(self.slave_fd)
        print("[*] Simulateur MS/TP arrêté.")

    def _crc16_ms(self, data):
        crc = 0xFFFF
        for byte in data:
            crc ^= byte
            for _ in range(8):
                if crc & 0x0001:
                    crc = (crc >> 1) ^ 0x8005
                else:
                    crc >>= 1
        return crc

    def _loop(self):
        # Utiliser un objet série sur le port master pour la lecture facile
        ser = serial.Serial(os.ttyname(self.master_fd), 38400, timeout=0.1)
        buffer = bytearray()
        
        while self.running:
            if ser.in_waiting > 0:
                buffer.extend(ser.read(ser.in_waiting))
                
                # Recherche du préfixe 55 FF
                while len(buffer) >= 8:
                    if buffer[0] == 0x55 and buffer[1] == 0xFF:
                        frame_type = buffer[2]
                        dest_mac = buffer[3]
                        src_mac = buffer[4]
                        length = buffer[5]
                        
                        total_len = 8 + length + (2 if length > 0 else 0)
                        if len(buffer) >= total_len:
                            # Frame complète
                            print(f"[MS/TP] Trame reçue: Type={frame_type}, Dest={dest_mac}, Src={src_mac}, Len={length}")
                            
                            # Si on reçoit un Token (Type 0x00) pour notre MAC, on répond
                            if frame_type == 0x00 and dest_mac == self.mac_address:
                                print("[MS/TP] Jeton reçu. Simulation d'un retour de jeton...")
                                # Réponse simple: on passe le jeton au prochain (ex: MAC 0)
                                token_frame = bytes([0x55, 0xFF, 0x00, 0x00, self.mac_address, 0x00])
                                crc = self._crc16_ms(token_frame)
                                ser.write(token_frame + crc.to_bytes(2, 'big'))
                                
                            # Si on reçoit un BACnet Data Expecting Reply (0x05)
                            elif frame_type == 0x05 and (dest_mac == self.mac_address or dest_mac == 255):
                                print("[MS/TP] Données BACnet reçues.")
                                # Simulation de l'envoi d'un I-Am si c'est un broadcast
                                
                            buffer = buffer[total_len:]
                        else:
                            break # Attente du reste de la trame
                    else:
                        buffer.pop(0) # Avancer jusqu'au préfixe
            else:
                time.sleep(0.01)

if __name__ == "__main__":
    sim = MSTPSimulator()
    try:
        sim.start()
        print("[*] En attente de requêtes. Appuyez sur Ctrl+C pour quitter.")
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        sim.stop()
