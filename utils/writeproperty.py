#!/usr/bin/env python3
import argparse
import requests
import sys

def write_property(ip, user, password, did, obj_type, inst, prop, val, priority):
    auth = (user, password)
    base_url = f"http://{ip}"
    
    # 1. Détecter si on écrit l'Out of Service (96)
    if prop == 96:
        # Route outofservice dédiée
        payload = {
            "did": did,
            "inst": inst,
            "type": obj_type,
            "state": "1" if val > 0.5 else "0"
        }
        r = requests.post(f"{base_url}/api/outofservice", auth=auth, data=payload, timeout=5)
    else:
        # Route writevalue standard
        payload = {
            "did": did,
            "type": obj_type,
            "inst": inst,
            "prop": prop,
            "val": val
        }
        if priority > 0:
            payload["priority"] = priority
            
        r = requests.post(f"{base_url}/api/writevalue", auth=auth, data=payload, timeout=5)
        
    if r.status_code == 200:
        print(f"SUCCÈS : {r.text}")
        return True
    else:
        print(f"ERREUR : HTTP {r.status_code}")
        print(r.text)
        return False

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description="Écrire une propriété BACnet via l'API REST de la passerelle")
    parser.add_argument('--ip', default="192.168.1.50", help="Adresse IP de la passerelle")
    parser.add_argument('--user', default="admin", help="Basic Auth User")
    parser.add_argument('--pass', dest='passwd', default="admin1234", help="Basic Auth Password")
    parser.add_argument('--did', type=int, required=True, help="Device ID cible")
    parser.add_argument('--type', type=int, required=True, help="Type d'objet (ex: 0=AI, 2=AV)")
    parser.add_argument('--inst', type=int, required=True, help="Instance d'objet")
    parser.add_argument('--prop', type=int, default=85, help="Propriété à écrire (défaut: 85 = Present_Value)")
    parser.add_argument('--val', type=float, required=True, help="Valeur à écrire")
    parser.add_argument('--priority', type=int, default=0, help="Priorité d'écriture BACnet (1-16, 0 = aucune)")
    
    args = parser.parse_args()
    success = write_property(args.ip, args.user, args.passwd, args.did, args.type, args.inst, args.prop, args.val, args.priority)
    sys.exit(0 if success else 1)
