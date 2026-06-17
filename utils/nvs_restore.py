#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
Script de restauration NVS BACnet2MQTT.
Depuis un fichier de sauvegarde généré par nvs_backup.py, permet de restaurer :
  - La configuration complète (wifi, mqtt, bacnet, poll, security)
  - Un device entier (tous ses objets)
  - Un objet individuel (name, unit, poll, min, max, step)

Les mots de passe (wifi, mqtt, admin) ne sont PAS restaurés (non exposés par l'API).

Usage:
  # Restaurer toute la config système
  python3 nvs_restore.py --backup nvs_backup_20260616_160000.json --restore config

  # Restaurer tous les objets d'un device
  python3 nvs_restore.py --backup nvs_backup_20260616_160000.json --restore device --did 364004

  # Restaurer un seul objet
  python3 nvs_restore.py --backup nvs_backup_20260616_160000.json --restore object --did 364004 --type 0 --inst 1005

  # Aperçu sans appliquer (dry-run)
  python3 nvs_restore.py --backup nvs_backup_20260616_160000.json --restore config --dry-run
"""

import argparse
import json
import sys
import time
import requests
from datetime import datetime

# Codes couleur ANSI
class C:
    OK    = '\033[92m'
    WARN  = '\033[93m'
    FAIL  = '\033[91m'
    BLUE  = '\033[94m'
    BOLD  = '\033[1m'
    DIM   = '\033[2m'
    END   = '\033[0m'

MAP_TYPE = {0:"AI", 1:"AO", 2:"AV", 3:"BI", 4:"BO", 5:"BV", 13:"MSI", 14:"MSO", 19:"MSV"}

def ok(msg):   print(f"  {C.OK}[OK]{C.END}   {msg}")
def warn(msg): print(f"  {C.WARN}[WARN]{C.END} {msg}")
def fail(msg): print(f"  {C.FAIL}[FAIL]{C.END} {msg}")
def info(msg): print(f"  {C.BLUE}[INFO]{C.END} {msg}")
def dry(msg):  print(f"  {C.DIM}[DRY]{C.END}  {msg}")


def load_backup(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


# ---------------------------------------------------------------------------
# Helpers de restauration
# ---------------------------------------------------------------------------

def restore_config_section(base_url, auth, timeout, form_type, params, dry_run):
    """Envoie un POST /save avec form_type et les paramètres fournis."""
    payload = {"form_type": form_type, **params}
    if dry_run:
        dry(f"/save [{form_type}] → {params}")
        return True
    try:
        r = requests.post(f"{base_url}/save", data=payload, auth=auth, timeout=timeout)
        if r.status_code == 200:
            ok(f"/save [{form_type}] → OK")
            return True
        else:
            fail(f"/save [{form_type}] → HTTP {r.status_code}: {r.text}")
            return False
    except requests.RequestException as e:
        fail(f"/save [{form_type}] → {e}")
        return False


def restore_object(base_url, auth, timeout, did, obj, dry_run):
    """Envoie un POST /api/save_object pour restaurer les métadonnées d'un objet."""
    type_id  = obj.get("type")
    inst     = obj.get("inst")
    name     = obj.get("name", "")
    unit     = obj.get("unit", "")
    poll     = "1" if obj.get("poll", True) else "0"
    step     = str(obj.get("step", 1.0))
    type_str = MAP_TYPE.get(type_id, f"T{type_id}")

    params = {
        "did":  str(did),
        "type": str(type_id),
        "inst": str(inst),
        "name": name,
        "unit": unit,
        "poll": poll,
        "step": step,
    }

    # min/max numériques
    if "min" in obj and obj["min"] is not None:
        params["min"] = str(obj["min"])
    if "max" in obj and obj["max"] is not None:
        params["max"] = str(obj["max"])

    # min/max référentiels (ex: "0_AV:3")
    if "min_ref" in obj and obj["min_ref"]:
        params["min"] = obj["min_ref"]
    if "max_ref" in obj and obj["max_ref"]:
        params["max"] = obj["max_ref"]

    label = f"{type_str}:{inst} ('{name}')"

    if dry_run:
        dry(f"save_object did={did} {label} poll={poll} unit='{unit}'")
        return True

    try:
        r = requests.post(f"{base_url}/api/save_object", data=params, auth=auth, timeout=timeout)
        if r.status_code == 200:
            ok(f"  {label}")
            return True
        else:
            fail(f"  {label} → HTTP {r.status_code}: {r.text}")
            return False
    except requests.RequestException as e:
        fail(f"  {label} → {e}")
        return False


# ---------------------------------------------------------------------------
# Modes de restauration
# ---------------------------------------------------------------------------

def do_restore_config(backup: dict, base_url, auth, timeout, dry_run):
    cfg = backup.get("config", {})
    if not cfg:
        fail("Aucune config dans le backup.")
        return

    print(f"\n{C.BLUE}Restauration de la configuration système...{C.END}")
    if dry_run:
        warn("Mode DRY-RUN — aucune modification ne sera appliquée.")

    results = []

    # WiFi
    wifi_params = {}
    if "ssid"     in cfg: wifi_params["ssid"]      = cfg["ssid"]
    if "local_ip" in cfg: wifi_params["local_ip"]  = cfg["local_ip"]
    if "gateway"  in cfg: wifi_params["gateway"]   = cfg["gateway"]
    if "subnet"   in cfg: wifi_params["subnet"]    = cfg["subnet"]
    if "static_ip" in cfg: wifi_params["static_ip"] = "on" if cfg["static_ip"] else ""
    wifi_params["pass"] = "******"  # mot de passe non restauré
    results.append(restore_config_section(base_url, auth, timeout, "wifi", wifi_params, dry_run))

    # MQTT
    mqtt_params = {}
    if "mqtt_server" in cfg: mqtt_params["mqh"]   = cfg["mqtt_server"]
    if "mqtt_user"   in cfg: mqtt_params["mqu"]   = cfg["mqtt_user"]
    if "mqtt_prefix" in cfg: mqtt_params["mqpr"]  = cfg["mqtt_prefix"]
    if "ha_discover" in cfg: mqtt_params["ha_disc"] = "on" if cfg["ha_discover"] else ""
    mqtt_params["mqp"] = "******"  # mot de passe non restauré
    results.append(restore_config_section(base_url, auth, timeout, "mqtt", mqtt_params, dry_run))

    # BACnet
    bac_params = {}
    if "mac_address"       in cfg: bac_params["mac"]     = str(cfg["mac_address"])
    if "device_id"         in cfg: bac_params["did"]     = str(cfg["device_id"])
    if "max_master"        in cfg: bac_params["mm"]      = str(cfg["max_master"])
    if "max_retries"       in cfg: bac_params["retries"] = str(cfg["max_retries"])
    if "apdu_timeout"      in cfg: bac_params["timeout"] = str(cfg["apdu_timeout"])
    if "token_skip"        in cfg: bac_params["tskip"]   = str(cfg["token_skip"])
    if "max_info_frames"   in cfg: bac_params["mif"]     = str(cfg["max_info_frames"])
    if "heartbeat_interval" in cfg: bac_params["hbeat"]  = str(cfg["heartbeat_interval"])
    results.append(restore_config_section(base_url, auth, timeout, "bac", bac_params, dry_run))

    # Polling
    poll_params = {}
    if "mqtt_poll_interval"   in cfg: poll_params["mpi"] = str(cfg["mqtt_poll_interval"])
    if "bacnet_poll_interval"  in cfg: poll_params["bpi"] = str(cfg["bacnet_poll_interval"])
    results.append(restore_config_section(base_url, auth, timeout, "poll", poll_params, dry_run))

    # Security (sans mot de passe)
    sec_params = {}
    if "admin_user" in cfg: sec_params["admin_u"] = cfg["admin_user"]
    if "log_level"  in cfg: sec_params["lvl"]     = str(cfg["log_level"])
    sec_params["admin_p"] = "******"  # mot de passe non restauré
    results.append(restore_config_section(base_url, auth, timeout, "sec", sec_params, dry_run))

    passed = sum(results)
    print(f"\n  → {passed}/{len(results)} sections restaurées")
    if not dry_run:
        warn("NOTE : Les mots de passe WiFi/MQTT/Admin ne sont pas restaurés (non exposés par l'API).")
        warn("NOTE : Un redémarrage peut être nécessaire pour appliquer les changements WiFi.")


def do_restore_device(backup: dict, base_url, auth, timeout, did, dry_run):
    devices = backup.get("devices", [])
    target = next((d for d in devices if str(d.get("device_id")) == str(did)), None)
    if not target:
        fail(f"Device {did} introuvable dans le backup.")
        info(f"Devices disponibles : {[d.get('device_id') for d in devices]}")
        sys.exit(1)

    objects = target.get("objects", [])
    name = target.get("name", "?")
    print(f"\n{C.BLUE}Restauration du device {did} ({name}) — {len(objects)} objet(s)...{C.END}")
    if dry_run:
        warn("Mode DRY-RUN — aucune modification ne sera appliquée.")

    passed = failed = 0
    for obj in objects:
        if restore_object(base_url, auth, timeout, did, obj, dry_run):
            passed += 1
        else:
            failed += 1
        if not dry_run:
            time.sleep(0.15)  # pacing pour éviter la saturation AsyncTCP

    print(f"\n  → {passed} OK | {failed} FAIL sur {len(objects)} objets")


def do_restore_object(backup: dict, base_url, auth, timeout, did, type_id, inst, dry_run):
    devices = backup.get("devices", [])
    target_dev = next((d for d in devices if str(d.get("device_id")) == str(did)), None)
    if not target_dev:
        fail(f"Device {did} introuvable dans le backup.")
        info(f"Devices disponibles : {[d.get('device_id') for d in devices]}")
        sys.exit(1)

    objects = target_dev.get("objects", [])
    target_obj = next(
        (o for o in objects if str(o.get("type")) == str(type_id) and str(o.get("inst")) == str(inst)),
        None
    )
    if not target_obj:
        fail(f"Objet type={type_id} inst={inst} introuvable dans le backup pour device {did}.")
        type_str = MAP_TYPE.get(int(type_id), f"T{type_id}")
        available = [f"{MAP_TYPE.get(o['type'], o['type'])}:{o['inst']}" for o in objects]
        info(f"Objets disponibles : {available}")
        sys.exit(1)

    type_str = MAP_TYPE.get(int(type_id), f"T{type_id}")
    print(f"\n{C.BLUE}Restauration de l'objet {type_str}:{inst} sur device {did}...{C.END}")
    if dry_run:
        warn("Mode DRY-RUN — aucune modification ne sera appliquée.")
        print(f"  Données dans le backup :")
        for k, v in target_obj.items():
            print(f"    {k:12s} = {v}")

    if restore_object(base_url, auth, timeout, did, target_obj, dry_run):
        if not dry_run:
            print(f"\n  {C.OK}{C.BOLD}✔  Objet {type_str}:{inst} restauré avec succès.{C.END}")
    else:
        fail(f"Échec de la restauration de {type_str}:{inst}.")
        sys.exit(1)


# ---------------------------------------------------------------------------
# Point d'entrée
# ---------------------------------------------------------------------------

def print_backup_summary(backup: dict):
    meta = backup.get("_meta", {})
    cfg  = backup.get("config", {})
    devs = backup.get("devices", [])
    total_obj = sum(len(d.get("objects", [])) for d in devs)

    print(f"\n{C.BOLD}{'='*55}{C.END}")
    print(f"{C.BOLD}   RESTAURATION NVS BACnet2MQTT{C.END}")
    print(f"{C.BOLD}{'='*55}{C.END}")
    print(f"  Backup créé    : {meta.get('created_at', '?')}")
    print(f"  Source ESP32   : {meta.get('esp_ip', '?')}")
    print(f"  Firmware       : {meta.get('firmware_ver', '?')}")
    print(f"  Config         : {len(cfg)} paramètres")
    print(f"  Devices        : {len(devs)} ({total_obj} objets)")
    for d in devs:
        objs = d.get("objects", [])
        print(f"    • Device {d.get('device_id')} ({d.get('name','?')}) : {len(objs)} objets")
    print()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Restauration NVS BACnet2MQTT depuis un backup JSON")
    parser.add_argument("--backup",  required=True, help="Chemin vers le fichier de backup (.json)")
    parser.add_argument("--restore", required=True, choices=["config", "device", "object"],
                        help="Mode de restauration : config | device | object")
    parser.add_argument("--ip",      default="192.168.1.50", help="Adresse IP de l'ESP32 (défaut: 192.168.1.50)")
    parser.add_argument("--user",    default="admin",        help="Basic Auth user (défaut: admin)")
    parser.add_argument("--pass",    dest="passwd", default="admin1234", help="Basic Auth password")
    parser.add_argument("--timeout", type=int, default=10,   help="Timeout HTTP en secondes (défaut: 10)")
    parser.add_argument("--dry-run", action="store_true",    help="Aperçu sans appliquer les modifications")
    # Pour --restore device / object
    parser.add_argument("--did",     default=None, help="Device ID (requis pour --restore device/object)")
    parser.add_argument("--type",    default=None, dest="obj_type",
                        help="Type d'objet BACnet (0=AI,1=AO,2=AV,3=BI,4=BO,5=BV,13=MSI,14=MSO,19=MSV) — requis pour --restore object")
    parser.add_argument("--inst",    default=None, help="Instance de l'objet — requis pour --restore object")

    args = parser.parse_args()

    # Charger le backup
    try:
        backup = load_backup(args.backup)
    except FileNotFoundError:
        print(f"{C.FAIL}Erreur : fichier backup introuvable : {args.backup}{C.END}")
        sys.exit(1)
    except json.JSONDecodeError as e:
        print(f"{C.FAIL}Erreur : fichier backup invalide (JSON) : {e}{C.END}")
        sys.exit(1)

    print_backup_summary(backup)

    base_url = f"http://{args.ip}"
    auth = (args.user, args.passwd)

    # Vérifier la connectivité avant de commencer
    if not args.dry_run:
        print(f"{C.BLUE}Vérification de la connectivité avec {args.ip}...{C.END}")
        try:
            r = requests.get(f"{base_url}/api/status", auth=auth, timeout=args.timeout)
            r.raise_for_status()
            live_ver = r.json().get("ver", "?")
            ok(f"ESP32 joignable — firmware actuel : {live_ver}")
            backup_ver = backup.get("_meta", {}).get("firmware_ver", "?")
            if live_ver != backup_ver:
                warn(f"Version firmware différente : backup={backup_ver}, live={live_ver}")
        except requests.RequestException as e:
            fail(f"Impossible de joindre l'ESP32 : {e}")
            sys.exit(1)
        print()

    # Dispatcher
    if args.restore == "config":
        do_restore_config(backup, base_url, auth, args.timeout, args.dry_run)

    elif args.restore == "device":
        if not args.did:
            parser.error("--did est requis pour --restore device")
        do_restore_device(backup, base_url, auth, args.timeout, args.did, args.dry_run)

    elif args.restore == "object":
        if not args.did or args.obj_type is None or args.inst is None:
            parser.error("--did, --type et --inst sont requis pour --restore object")
        do_restore_object(backup, base_url, auth, args.timeout,
                          args.did, args.obj_type, args.inst, args.dry_run)

    print()
