#!/usr/bin/env python3
import argparse
import requests
import time
import sys

def read_property(ip, user, password, did, obj_type, inst, prop):
    auth = (user, password)
    base_url = f"http://{ip}"
    
    # 1. Obtenir le timestamp d'update de l'objet avant la lecture
    r = requests.get(f"{base_url}/api/objects", auth=auth, timeout=5)
    if r.status_code != 200:
        print(f"Erreur: Impossible de lire le cache initial (HTTP {r.status_code})")
        sys.exit(1)
        
    last_update_before = 0
    devices = r.json()
    for d in devices:
        if d.get("device_id") == did:
            for o in d.get("objects", []):
                if o.get("type") == obj_type and o.get("inst") == inst:
                    last_update_before = o.get("last_update", 0)
                    break
    
    # 2. Envoyer la demande asynchrone de lecture de propriété
    payload = {"did": did, "type": obj_type, "inst": inst, "prop": prop}
    r_req = requests.post(f"{base_url}/api/readproperty", auth=auth, data=payload, timeout=5)
    if r_req.status_code != 200:
        print(f"Erreur lors de la soumission du ReadProperty (HTTP {r_req.status_code})")
        print(r_req.text)
        sys.exit(1)
        
    # 3. Attendre la réponse en surveillant le cache (polling)
    # L'automate distant peut mettre jusqu'à 300ms à répondre sur MS/TP
    timeout = 4.0
    start_time = time.time()
    while time.time() - start_time < timeout:
        time.sleep(0.2)
        r = requests.get(f"{base_url}/api/objects", auth=auth, timeout=5)
        if r.status_code == 200:
            for d in r.json():
                if d.get("device_id") == did:
                    for o in d.get("objects", []):
                        if o.get("type") == obj_type and o.get("inst") == inst:
                            last_update = o.get("last_update", 0)
                            # Si le timestamp de mise à jour a changé, la valeur a été lue du bus
                            if last_update > last_update_before or (last_update_before == 0 and last_update > 0):
                                if prop == 85:
                                    return o.get("val")
                                elif prop == 111:
                                    return o.get("status_flags", o.get("ucStatusFlags", 0))
                                else:
                                    return o.get("val") # Fallback
    
    # Si timeout, renvoyer la valeur actuellement en cache
    print("Warning: Timeout de lecture MS/TP, retour de la valeur en cache.")
    for d in devices:
        if d.get("device_id") == did:
            for o in d.get("objects", []):
                if o.get("type") == obj_type and o.get("inst") == inst:
                    if prop == 111:
                        return o.get("status_flags", o.get("ucStatusFlags", 0))
                    return o.get("val")
    return None

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description="Lire une propriété BACnet via l'API REST de la passerelle")
    parser.add_argument('--ip', default="192.168.1.50", help="Adresse IP de la passerelle")
    parser.add_argument('--user', default="admin", help="Basic Auth User")
    parser.add_argument('--pass', dest='passwd', default="admin1234", help="Basic Auth Password")
    parser.add_argument('--did', type=int, required=True, help="Device ID cible")
    parser.add_argument('--type', type=int, required=True, help="Type d'objet (ex: 0=AI, 2=AV)")
    parser.add_argument('--inst', type=int, required=True, help="Instance d'objet")
    parser.add_argument('--prop', type=int, default=85, help="Propriété à lire (défaut: 85 = Present_Value)")
    
    args = parser.parse_args()
    val = read_property(args.ip, args.user, args.passwd, args.did, args.type, args.inst, args.prop)
    if val is not None:
        print(f"VALEUR LUE : {val}")
        sys.exit(0)
    else:
        print("Erreur: Impossible d'obtenir la valeur de la propriété.")
        sys.exit(1)
