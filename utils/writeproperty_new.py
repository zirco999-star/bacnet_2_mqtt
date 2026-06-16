#!/usr/bin/env python3
import argparse
import requests
import sys

def write_property(ip, user, password, did, obj_type, inst, prop, val, priority):
    auth = (user, password)
    base_url = f"http://{ip}"
    
    # Sécurisation normative BACnet : empêcher l'écriture sur une propriété en lecture seule
    if prop == 111:
        print("ERREUR : La propriété Status_Flags (111) est en lecture seule selon la norme ASHRAE 135.")
        return False

    # 1. Traitement spécifique pour Out_Of_Service (Propriété 81, BOOLEAN)
    if prop == 81:
        # Conversion flexible du texte en état booléen 1 ou 0
        is_active = str(val).lower() in ['1', 'true', 'on', 'active']
        payload = {
            "did": did,
            "inst": inst,
            "type": obj_type,
            "state": "1" if is_active else "0"
        }
        # Utilisation de ta route dédiée pour forcer le bon type de donnée (BOOLEAN)
        url = f"{base_url}/api/outofservice"
        
    # 2. Traitement spécifique pour Object_Name (Propriété 77, CharacterString)
    elif prop == 77:
        payload = {
            "did": did,
            "type": obj_type,
            "inst": inst,
            "prop": prop,
            "name": str(val) # On garantit l'envoi d'une chaîne pour la RAM de l'ESP32
        }
        url = f"{base_url}/api/writevalue"

    # 3. Traitement standard : Present_Value (85), Units (117), Relinquish_Default (104), etc.
    else:
        payload = {
            "did": did,
            "type": obj_type,
            "inst": inst,
            "prop": prop,
            "val": val
        }
        
        # Injection du Context Tag 4 (Priority_Array) UNIQUEMENT sur la Present_Value
        if priority > 0 and prop == 85:
            payload["priority"] = priority
            
        url = f"{base_url}/api/writevalue"

    print(f"Envoi de la requête vers {url} | Payload : {payload}")
    
    try:
        r = requests.post(url, auth=auth, data=payload, timeout=5)
        if r.status_code == 200:
            print(f"SUCCÈS : {r.text}")
            return True
        else:
            print(f"ERREUR : Rejet de la passerelle - HTTP {r.status_code}")
            print(f"Détail : {r.text}")
            return False
    except requests.exceptions.RequestException as e:
        print(f"ERREUR RÉSEAU : Impossible de joindre l'ESP32 ({e})")
        return False

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description="Outil d'injection BACnet via passerelle ESP32-S3")
    parser.add_argument('--ip', default="192.168.1.50", help="Adresse IP de la passerelle")
    parser.add_argument('--user', default="admin", help="Basic Auth User")
    parser.add_argument('--pass', dest='passwd', default="admin1234", help="Basic Auth Password")
    parser.add_argument('--did', type=int, required=True, help="Device ID cible (ex: 364004)")
    parser.add_argument('--type', type=int, required=True, help="Type d'objet (ex: 0=AI, 4=BO, 19=MSV)")
    parser.add_argument('--inst', type=int, required=True, help="Instance d'objet")
    parser.add_argument('--prop', type=int, default=85, help="ID Propriété (77=Name, 81=OOS, 85=Value, 117=Units)")
    
    # MODIFICATION CRITIQUE : type=str pour supporter les chaînes de caractères (noms)
    parser.add_argument('--val', type=str, required=True, help="Valeur à écrire (texte, entier ou flottant)")
    parser.add_argument('--priority', type=int, default=0, help="Priorité BACnet (1-16, 0 = aucune, 8 = Manuel)")
    
    args = parser.parse_args()
    
    success = write_property(args.ip, args.user, args.passwd, args.did, args.type, args.inst, args.prop, args.val, args.priority)
    sys.exit(0 if success else 1)