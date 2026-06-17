#!/usr/bin/env python3
import argparse
import requests
import time
import sys

def run_hack_scenario(ip, user, password, did, inst, force_val, no_cleanup):
    auth = (user, password)
    base_url = f"http://{ip}"
    
    print("==========================================================")
    print("DÉBUT DU SCÉNARIO DE TEST : HACK DE L'UTA (SÉCURITÉ ÉTÉ)")
    print("==========================================================")
    
    # 1. Lire l'état initial
    print("\n[Étape 1] Récupération du cache initial de la sonde...")
    r = requests.get(f"{base_url}/api/objects", auth=auth, timeout=5)
    if r.status_code != 200:
        print(f"Erreur: Impossible de lire `/api/objects` (HTTP {r.status_code})")
        sys.exit(1)
        
    initial_obj = None
    devices_data = r.json()
    print(f"DEBUG: Retrieved {len(devices_data)} devices.")
    for d in devices_data:
        dev_id = d.get("device_id")
        print(f"DEBUG: Checking Device {dev_id} (type: {type(dev_id)}) vs Target {did} (type: {type(did)})")
        if dev_id == did:
            print(f"DEBUG: Found target device {did}. Checking objects...")
            for o in d.get("objects", []):
                o_type = o.get("type")
                o_inst = o.get("inst")
                if o_type == 0 and o_inst == inst:
                    initial_obj = o
                    print(f"DEBUG: Found target object AI:{inst}")
                    break
                    
    if not initial_obj:
        print(f"Erreur: Sonde AI:{inst} introuvable sur le périphérique {did} en cache.")
        sys.exit(1)
        
    original_val = initial_obj.get("val")
    original_oos = initial_obj.get("outofservice", False)
    original_flags = initial_obj.get("status_flags", initial_obj.get("ucStatusFlags", 0))
    print(f"   Sonde AI:{inst} trouvée.")
    print(f"   Valeur initiale : {original_val} °C (ou unité)")
    print(f"   Out_Of_Service initial : {original_oos} (Flags: 0x{original_flags:02X})")

    try:
        # 2. Activer Out_Of_Service (Débrayage)
        print("\n[Étape 2] Activation d'Out_Of_Service (Isolation de la sonde)...")
        payload_oos = {"did": did, "inst": inst, "type": 0, "state": "1"}
        r_oos = requests.post(f"{base_url}/api/outofservice", auth=auth, data=payload_oos, timeout=5)
        if r_oos.status_code != 200:
            raise ValueError(f"Échec de l'isolation (HTTP {r_oos.status_code})")
        print("   Commande OutOfService enfilée avec succès.")

        # 3. Attendre et vérifier que le status_flag a le bit Out_Of_Service à 1
        print("\n[Étape 3] Vérification du statut de contournement...")
        verified_oos = False
        start_time = time.time()
        # On attend max 5 secondes que le bus MS/TP applique et remonte la mise à jour
        while time.time() - start_time < 5:
            time.sleep(0.5)
            r_check = requests.get(f"{base_url}/api/objects", auth=auth, timeout=5)
            if r_check.status_code == 200:
                for d in r_check.json():
                    if d.get("device_id") == did:
                        for o in d.get("objects", []):
                            if o.get("type") == 0 and o.get("inst") == inst:
                                flags = o.get("status_flags", o.get("ucStatusFlags", 0))
                                # Bit 3 (Out_Of_Service) = 0x08
                                oos_bit = (flags & 0x08) != 0
                                oos_field = o.get("outofservice", False)
                                print(f"   Vérification cache : Out_Of_Service = {oos_field} (Flags: 0x{flags:02X}, Bit OOS: {oos_bit})")
                                if oos_bit or oos_field:
                                    print("   -> OK : La sonde est bien passée en état Out_Of_Service (débrayée) !")
                                    verified_oos = True
                                    break
                if verified_oos:
                    break
                    
        if not verified_oos:
            raise ValueError("Le bit Out_Of_Service de Status_Flags n'est pas passé à 1 !")

        # 4. Écrire la valeur forcée (-1.0 par défaut)
        print(f"\n[Étape 4] Injection de la valeur forcée ({force_val})...")
        payload_write = {"did": did, "type": 0, "inst": inst, "prop": 85, "val": force_val, "priority": 8}
        r_write = requests.post(f"{base_url}/api/writevalue", auth=auth, data=payload_write, timeout=5)
        if r_write.status_code != 200:
            raise ValueError(f"Échec de l'écriture de valeur (HTTP {r_write.status_code})")
        print("   Commande de valeur forcée enfilée avec succès.")

        # Attente d'une minute pour que le hack agisse sur l'UTA
        print("\n[Attente] Pause de 120 secondes pour permettre le déclenchement de l'UTA...")
        for i in range(120, 0, -1):
            sys.stdout.write(f"\r   Temps restant avant vérification et restauration : {i} secondes... ")
            sys.stdout.flush()
            time.sleep(1)
        print("\r   Pause de 120 secondes terminée.                                          ")

        # 5. Attendre et vérifier la valeur en cache
        print("\n[Étape 5] Vérification de la prise en compte de la valeur forcée...")
        verified_val = False
        start_time = time.time()
        while time.time() - start_time < 5:
            time.sleep(0.5)
            r_check = requests.get(f"{base_url}/api/objects", auth=auth, timeout=5)
            if r_check.status_code == 200:
                for d in r_check.json():
                    if d.get("device_id") == did:
                        for o in d.get("objects", []):
                            if o.get("type") == 0 and o.get("inst") == inst:
                                val = o.get("val")
                                print(f"   Vérification cache : Valeur actuelle = {val}")
                                # Comparaison avec marge d'erreur float
                                if val is not None and abs(val - force_val) < 0.05:
                                    print("   -> OK : La valeur forcée a bien été appliquée au capteur débrayé !")
                                    verified_val = True
                                    break
                if verified_val:
                    break
                    
        if not verified_val:
            raise ValueError(f"La Present_Value n'a pas pris la valeur forcée de {force_val} !")

        print("\n==========================================================")
        print("🌟 SCÉNARIO RÉUSSI : Le Hack UTA est totalement validé !")
        print("==========================================================")

    except Exception as e:
        print(f"\n❌ ÉCHEC DU TEST : {e}")
        sys.exit(1)
        
    finally:
        if not no_cleanup:
            print("\n[Nettoyage] Restauration de l'état d'origine du capteur...")
            # 1. Rétablir Out_Of_Service à False
            payload_restore_oos = {"did": did, "inst": inst, "type": 0, "state": "0" if not original_oos else "1"}
            requests.post(f"{base_url}/api/outofservice", auth=auth, data=payload_restore_oos, timeout=5)
            print("   Nettoyage : OutOfService rétabli.")
            time.sleep(1)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description="Tester le scénario du Hack Climatisation (UTA) sur une sonde AI")
    parser.add_argument('--ip', default="192.168.1.50", help="Adresse IP de la passerelle")
    parser.add_argument('--user', default="admin", help="Basic Auth User")
    parser.add_argument('--pass', dest='passwd', default="admin1234", help="Basic Auth Password")
    parser.add_argument('--did', type=int, required=True, help="Device ID cible")
    parser.add_argument('--inst', type=int, required=True, help="Instance d'objet AI cible")
    parser.add_argument('--val', type=float, default=-1.0, help="Valeur de forçage UTA (défaut: -1.0)")
    parser.add_argument('--no-cleanup', action='store_true', help="Ne pas restaurer l'état original après le test")
    
    args = parser.parse_args()
    run_hack_scenario(args.ip, args.user, args.passwd, args.did, args.inst, args.val, args.no_cleanup)
