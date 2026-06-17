#!/usr/bin/env python3
import sys
import requests
import time

# Informations d'authentification par défaut
DEFAULT_IP = "192.168.1.50"
DEFAULT_USER = "admin"
DEFAULT_PASS = "admin1234"
DEFAULT_DID = 364004  # ECB_203

# Types d'objets BACnet commandables (utilisant un tableau de priorité)
WRITEABLE_TYPES = {
    1: "AO",   # Analog Output
    2: "AV",   # Analog Value
    4: "BO",   # Binary Output
    5: "BV",   # Binary Value
    14: "MSO", # Multi-State Output
    19: "MSV"  # Multi-State Value
}

def main():
    ip = DEFAULT_IP
    user = DEFAULT_USER
    password = DEFAULT_PASS
    target_did = DEFAULT_DID
    priority = 8  # Priorité par défaut des forçages manuels de la passerelle

    if len(sys.argv) > 1:
        try:
            target_did = int(sys.argv[1])
        except ValueError:
            print(f"Usage: {sys.argv[0]} [device_id] [priority]")
            sys.exit(1)
            
    if len(sys.argv) > 2:
        try:
            priority = int(sys.argv[2])
        except ValueError:
            print(f"Priorité invalide : {sys.argv[2]}")
            sys.exit(1)

    print(f"--- Relinquish All pour Device ID {target_did} (Priorité : {priority}) ---")
    auth = (user, password)
    
    # 1. Récupérer la liste des objets
    url_objects = f"http://{ip}/api/objects"
    try:
        r = requests.get(url_objects, auth=auth, timeout=5)
        r.raise_for_status()
        data = r.json()
    except Exception as e:
        print(f"Erreur lors de la récupération des objets : {e}")
        sys.exit(1)

    # Trouver l'équipement cible
    device = None
    for d in data:
        if d.get("device_id") == target_did:
            device = d
            break

    if not device:
        print(f"Équipement {target_did} non trouvé dans le cache.")
        sys.exit(1)

    print(f"Équipement trouvé : {device.get('name')} ({device.get('vendor')})")
    
    # 2. Filtrer les objets commandables et activés (polled)
    objects_to_release = []
    for obj in device.get("objects", []):
        if obj.get("poll") and obj.get("type") in WRITEABLE_TYPES:
            objects_to_release.append(obj)

    if not objects_to_release:
        print("Aucun objet commandable et activé (polled) trouvé pour cet équipement.")
        sys.exit(0)

    print(f"Nombre d'objets à libérer : {len(objects_to_release)}")
    
    # 3. Envoyer la commande Relinquish (val=AUTO) pour chaque objet
    url_write = f"http://{ip}/api/writevalue"
    success_count = 0
    
    for obj in objects_to_release:
        obj_type = obj["type"]
        obj_inst = obj["inst"]
        obj_name = obj["name"]
        obj_type_str = WRITEABLE_TYPES[obj_type]
        
        print(f"Libération de {obj_type_str}:{obj_inst} ({obj_name})... ", end="", flush=True)
        
        payload = {
            "did": target_did,
            "type": obj_type,
            "inst": obj_inst,
            "prop": 85,          # Present_Value
            "val": "AUTO",       # Relinquish
            "priority": priority
        }
        
        try:
            r = requests.post(url_write, data=payload, auth=auth, timeout=5)
            if r.status_code == 200:
                print("OK")
                success_count += 1
            else:
                print(f"Erreur {r.status_code} ({r.text})")
        except Exception as e:
            print(f"Échec ({e})")
            
        # Pacing léger pour ne pas surcharger le bus MS/TP
        time.sleep(0.1)

    print(f"\n--- Terminé : {success_count}/{len(objects_to_release)} objets libérés avec succès. ---")

if __name__ == "__main__":
    main()
