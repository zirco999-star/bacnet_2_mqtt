#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import requests
import sys

# Mapping des types d'objets BACnet
TYPE_MAP = {
    "ai": 0, "ao": 1, "av": 2,
    "bi": 3, "bo": 4, "bv": 5,
    "cal": 6, "cmd": 7, "dev": 8,
    "evn": 9, "fil": 10, "grp": 11,
    "lp": 12, "msi": 13, "mso": 14,
    "not": 15, "prg": 16, "sch": 17,
    "avg": 18, "msv": 19
}

# Mapping des propriétés BACnet courantes
PROP_MAP = {
    "present_value": 85,
    "presentvalue": 85,
    "present value": 85,
    "out_of_service": 81,
    "outofservice": 81,
    "out of service": 81,
    "object_name": 77,
    "objectname": 77,
    "object name": 77,
    "status_flags": 111,
    "statusflags": 111,
    "status flags": 111
}

def resolve_type(type_input):
    """Résout le type d'objet en entier BACnet (supporte chaine ou int)"""
    if isinstance(type_input, int):
        return type_input
    
    val_str = str(type_input).strip().lower()
    if val_str in TYPE_MAP:
        return TYPE_MAP[val_str]
    
    try:
        return int(type_input)
    except ValueError:
        raise argparse.ArgumentTypeError(f"Type d'objet invalide : '{type_input}'. Utilisez un entier ou un code standard (ex: AI, AV, BO, MSV).")

def resolve_prop(prop_input):
    """Résout la propriété BACnet en entier ID (supporte chaine ou int)"""
    val_str = str(prop_input).strip().lower().replace('_', ' ').replace('-', ' ')
    if val_str in PROP_MAP:
        return PROP_MAP[val_str]
    
    try:
        return int(prop_input)
    except ValueError:
        raise argparse.ArgumentTypeError(f"Propriété invalide : '{prop_input}'. Utilisez un entier ou un nom connu (ex: Present_Value, Out_Of_Service).")

def write_property(ip, user, password, did, obj_type, inst, prop, val, priority):
    auth = (user, password)
    base_url = f"http://{ip}"
    
    # Sécurisation normative BACnet : Status_Flags (111) est en lecture seule
    if prop == 111:
        print("ERREUR : La propriété Status_Flags (111) est en lecture seule selon la norme ASHRAE 135.")
        return False

    # 1. Traitement pour Out_Of_Service (Propriété 81)
    if prop == 81:
        # Conversion du texte en état booléen 1 ou 0
        is_active = str(val).lower() in ['1', 'true', 'on', 'active', 'oui', 'yes']
        payload = {
            "did": did,
            "inst": inst,
            "type": obj_type,
            "state": "1" if is_active else "0"
        }
        url = f"{base_url}/api/outofservice"
        
    # 2. Traitement pour Object_Name (Propriété 77)
    elif prop == 77:
        payload = {
            "did": did,
            "type": obj_type,
            "inst": inst,
            "prop": prop,
            "val": str(val),
            "name": str(val) # name est obligatoire pour la mise à jour de la chaîne, val pour satisfaire les requis API
        }
        url = f"{base_url}/api/writevalue"

    # 3. Traitement standard (Present_Value (85) ou autre)
    else:
        payload = {
            "did": did,
            "type": obj_type,
            "inst": inst,
            "prop": prop,
            "val": val
        }
        
        # Injection du paramètre de priorité uniquement si défini (>0) et sur la Present_Value (85)
        if priority > 0 and prop == 85:
            payload["priority"] = priority
            
        url = f"{base_url}/api/writevalue"

    print(f"Envoi de la requête vers : {url}")
    print(f"Payload : {payload}")
    
    try:
        r = requests.post(url, auth=auth, data=payload, timeout=5)
        if r.status_code == 200:
            print(f"SUCCÈS : {r.text}")
            return True
        else:
            print(f"ERREUR : Rejet de la passerelle - Code HTTP {r.status_code}")
            print(f"Détails : {r.text}")
            return False
    except requests.exceptions.RequestException as e:
        print(f"ERREUR RÉSEAU : Impossible de joindre la passerelle ({e})")
        return False

if __name__ == '__main__':
    epilog_text = """
Exemples d'utilisation :
  1. Écrire la Present_Value de AV:35 à 12.5 sans priorité :
     python3 write_property.py --did 364004 --type AV --inst 35 --val 12.5

  2. Forcer la Present_Value de AO:1 à 22.0 avec la priorité manuelle 8 :
     python3 write_property.py --did 364004 --type AO --inst 1 --val 22.0 --priority 8

  3. Mettre l'objet AI:1 hors service (Out_Of_Service = True) :
     python3 write_property.py --did 364004 --type AI --inst 1 --prop Out_Of_Service --val True

  4. Libérer le forçage manuel (Relinquish / AUTO) en priorité 8 :
     python3 write_property.py --did 364004 --type AO --inst 1 --val AUTO --priority 8

AVERTISSEMENT SÉCURITÉ / CONFORMITÉ NORMATIVE :
  L'utilisation d'une priorité d'écriture BACnet inférieure ou égale à 10 (telle que la priorité 8 par défaut
  de contournement manuel) écrit dans la table des priorités persistante de l'automate cible.
  L'automate ignorera son propre programme interne pour cet objet jusqu'à ce que la priorité soit expressément
  libérée (Relinquish). C'est la responsabilité exclusive de l'utilisateur de restituer le contrôle
  au programme de l'automate en envoyant la commande de libération avec la valeur "AUTO" (voir exemple 4).
"""

    parser = argparse.ArgumentParser(
        description="Outil d'écriture de propriétés d'objets BACnet via l'API REST de la passerelle.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=epilog_text
    )
    
    parser.add_argument('--ip', default="192.168.1.50", help="Adresse IP de la passerelle (Défaut: 192.168.1.50)")
    parser.add_argument('--user', default="admin", help="Utilisateur Basic Auth (Défaut: admin)")
    parser.add_argument('--pass', dest='passwd', default="admin1234", help="Mot de passe Basic Auth (Défaut: admin1234)")
    
    parser.add_argument('--did', type=int, required=True, help="Device ID BACnet cible (ex: 364004)")
    parser.add_argument('--type', type=resolve_type, required=True, help="Type d'objet (ex: AI, AO, AV, BI, BO, BV, MSV ou entier)")
    parser.add_argument('--inst', type=int, required=True, help="Numéro d'instance de l'objet (ex: 1)")
    parser.add_argument('--prop', type=resolve_prop, default=85, help="Propriété BACnet à écrire (ex: Present_Value, Out_Of_Service ou entier ID, Défaut: 85)")
    parser.add_argument('--val', type=str, required=True, help="Valeur à écrire (nombre, booléen, texte, ou 'AUTO' pour relinquish)")
    parser.add_argument('--priority', type=int, default=0, choices=range(0, 17), help="Priorité d'écriture BACnet (1 à 16. 0 = pas de priorité. Défaut: 0)")
    
    args = parser.parse_args()
    
    success = write_property(
        args.ip, args.user, args.passwd,
        args.did, args.type, args.inst, args.prop, args.val, args.priority
    )
    sys.exit(0 if success else 1)
