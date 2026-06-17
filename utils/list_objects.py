#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import requests
import sys

# Mapping des types d'objets BACnet standards
TYPE_MAP = {
    0: "AI",   # Analog Input
    1: "AO",   # Analog Output
    2: "AV",   # Analog Value
    3: "BI",   # Binary Input
    4: "BO",   # Binary Output
    5: "BV",   # Binary Value
    6: "CAL",  # Calendar
    7: "CMD",  # Command
    8: "DEV",  # Device
    9: "EVN",  # Event Enrollment
    10: "FIL", # File
    11: "GRP", # Group
    12: "LP",  # Loop
    13: "MSI", # Multi-state Input
    14: "MSO", # Multi-state Output
    15: "NOT", # Notification Class
    16: "PRG", # Program
    17: "SCH", # Schedule
    18: "AVG", # Trend Log
    19: "MSV"  # Multi-state Value
}

def get_type_str(obj_type):
    return TYPE_MAP.get(obj_type, f"OBJ_{obj_type}")

def decode_status_flags(flags_int):
    """
    Décode l'octet de status_flags BACnet.
    Bit 0 : In Alarm (A)
    Bit 1 : Fault (F)
    Bit 2 : Overridden (O) (Forçage Physique/Local)
    Bit 3 : Out Of Service (S) (Hors Service)
    """
    if flags_int is None:
        return "[- - - -]"
    
    in_alarm = (flags_int & 1) != 0
    fault = (flags_int & 2) != 0
    overridden = (flags_int & 4) != 0
    oos = (flags_int & 8) != 0
    
    a_char = "A" if in_alarm else "-"
    f_char = "F" if fault else "-"
    o_char = "O" if overridden else "-"
    s_char = "S" if oos else "-"
    
    return f"[{a_char} {f_char} {o_char} {s_char}]"

def list_objects(ip, user, password):
    auth = (user, password)
    url = f"http://{ip}/api/objects"
    
    print(f"Connexion à la passerelle http://{ip} ...")
    try:
        r = requests.get(url, auth=auth, timeout=5)
        if r.status_code != 200:
            print(f"ERREUR : Réponse HTTP {r.status_code} de la passerelle.")
            print(r.text)
            return False
    except requests.exceptions.RequestException as e:
        print(f"ERREUR RÉSEAU : Impossible de joindre la passerelle ({e})")
        return False
        
    devices = r.json()
    if not devices:
        print("Aucun périphérique BACnet en cache sur la passerelle.")
        return True
        
    for dev in devices:
        dev_name = dev.get("name", "Inconnu")
        dev_id = dev.get("device_id", "Inconnu")
        dev_vendor = dev.get("vendor", "Inconnu")
        enabled = "ACTIF" if dev.get("enabled", True) else "DÉSACTIVÉ"
        
        print("\n" + "=" * 90)
        print(f" PÉRIPHÉRIQUE : {dev_name} (ID: {dev_id}) | Constructeur: {dev_vendor} | État: {enabled}")
        print("=" * 90)
        
        objects = dev.get("objects", [])
        if not objects:
            print("  Aucun objet en cache pour ce périphérique.")
            continue
            
        # En-tête du tableau
        header = f"{'Objet':<10} | {'Nom':<25} | {'Valeur':<12} | {'Unité':<8} | {'Poll':<5} | {'Flags (A F O S)':<16} | {'Forçage BACnet':<14}"
        print(header)
        print("-" * len(header))
        
        # Tri des objets par type puis par instance pour un affichage propre
        sorted_objects = sorted(objects, key=lambda x: (x.get("type", 0), x.get("inst", 0)))
        
        for obj in sorted_objects:
            obj_type = obj.get("type", 0)
            obj_inst = obj.get("inst", 0)
            obj_name = obj.get("name", "")
            val = obj.get("val")
            unit = obj.get("unit", "")
            poll = "OUI" if obj.get("poll", True) else "NON"
            flags_val = obj.get("status_flags", 0)
            oos_bacnet = "OUI" if obj.get("overridden_bacnet", False) else "NON"
            
            # Formatage de l'identifiant objet (ex: AV:27)
            obj_id_str = f"{get_type_str(obj_type)}:{obj_inst}"
            
            # Formatage de la valeur pour alignement
            if val is None:
                val_str = "None"
            elif isinstance(val, float):
                val_str = f"{val:.4f}".rstrip('0').rstrip('.')
            else:
                val_str = str(val)
                
            flags_str = decode_status_flags(flags_val)
            
            row = f"{obj_id_str:<10} | {obj_name:<25} | {val_str:<12} | {unit:<8} | {poll:<5} | {flags_str:<16} | {oos_bacnet:<14}"
            print(row)
            
    print("\nLégende des Flags : A = Alarme active | F = Défaut | O = Forçage Physique Local (PLC) | S = Hors Service (Out of Service)")
    return True

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description="Affiche l'état des objets BACnet en RAM sous forme de tableau ASCII.")
    parser.add_argument('--ip', default="192.168.1.50", help="Adresse IP de la passerelle (Défaut: 192.168.1.50)")
    parser.add_argument('--user', default="admin", help="Utilisateur Basic Auth (Défaut: admin)")
    parser.add_argument('--pass', dest='passwd', default="admin1234", help="Mot de passe Basic Auth (Défaut: admin1234)")
    
    args = parser.parse_args()
    success = list_objects(args.ip, args.user, args.passwd)
    sys.exit(0 if success else 1)
