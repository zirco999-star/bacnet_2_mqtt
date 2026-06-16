#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
Script de sauvegarde NVS BACnet2MQTT.
Sauvegarde la configuration système (sysCfg) et la totalité du cache devices/objets
en interrogeant les routes API de l'ESP32.

Usage:
  python3 nvs_backup.py
  python3 nvs_backup.py --ip 192.168.1.50 --user admin --pass admin1234
  python3 nvs_backup.py --output /chemin/vers/backup.json
"""

import argparse
import json
import sys
import time
import os
from datetime import datetime
import requests

# Codes couleur ANSI
class C:
    OK    = '\033[92m'
    WARN  = '\033[93m'
    FAIL  = '\033[91m'
    BLUE  = '\033[94m'
    BOLD  = '\033[1m'
    DIM   = '\033[2m'
    END   = '\033[0m'

def ok(msg):   print(f"  {C.OK}[OK]{C.END}   {msg}")
def warn(msg): print(f"  {C.WARN}[WARN]{C.END} {msg}")
def fail(msg): print(f"  {C.FAIL}[FAIL]{C.END} {msg}")
def info(msg): print(f"  {C.BLUE}[INFO]{C.END} {msg}")


def fetch_status(base_url, auth, timeout):
    """Récupère la configuration système depuis /api/status."""
    r = requests.get(f"{base_url}/api/status", auth=auth, timeout=timeout)
    r.raise_for_status()
    return r.json()


def fetch_objects(base_url, auth, timeout):
    """Récupère la totalité du cache devices/objets depuis /api/objects."""
    r = requests.get(f"{base_url}/api/objects", auth=auth, timeout=timeout)
    r.raise_for_status()
    return r.json()


def build_config_snapshot(status: dict) -> dict:
    """
    Extrait les champs de configuration persistante de /api/status.
    On exclut les métriques live (heap, uptime, rssi, mqtt_connected, etc.)
    qui ne font pas partie de la NVS.
    """
    CONFIG_KEYS = {
        # WiFi
        "ssid":    "ssid",
        "static":  "static_ip",
        "ip":      "local_ip",
        "gw":      "gateway",
        "mask":    "subnet",
        # MQTT
        "mqs":     "mqtt_server",
        "mqu":     "mqtt_user",
        "mqpr":    "mqtt_prefix",
        "ha_disc": "ha_discover",
        # Polling
        "mpi":     "mqtt_poll_interval",
        "bpi":     "bacnet_poll_interval",
        # BACnet
        "mac":     "mac_address",
        "mm":      "max_master",
        "did":     "device_id",
        "to":      "apdu_timeout",
        "ret":     "max_retries",
        "hbeat":   "heartbeat_interval",
        "tskip":   "token_skip",
        "mif":     "max_info_frames",
        # Security (hors mot de passe)
        "adu":     "admin_user",
        "lvl":     "log_level",
        # Defaults
        "n_min":   "default_number_min",
        "n_max":   "default_number_max",
        "n_stp":   "default_number_step",
    }
    snapshot = {}
    for api_key, label in CONFIG_KEYS.items():
        if api_key in status:
            snapshot[label] = status[api_key]
    return snapshot


def run_backup(args):
    base_url = f"http://{args.ip}"
    auth = (args.user, args.passwd)

    print(f"\n{C.BOLD}{'='*55}{C.END}")
    print(f"{C.BOLD}   SAUVEGARDE NVS BACnet2MQTT — {args.ip}{C.END}")
    print(f"{C.BOLD}{'='*55}{C.END}\n")

    # --- Étape 1 : Configuration ---
    print(f"{C.BLUE}[1/2] Récupération de la configuration système (/api/status)...{C.END}")
    try:
        status = fetch_status(base_url, auth, args.timeout)
        config_snapshot = build_config_snapshot(status)
        firmware_ver = status.get("ver", "?")
        ok(f"Firmware : {firmware_ver}")
        ok(f"{len(config_snapshot)} paramètres de configuration extraits")
    except requests.RequestException as e:
        fail(f"Impossible de joindre /api/status : {e}")
        sys.exit(1)

    # --- Étape 2 : Devices & objets ---
    print(f"\n{C.BLUE}[2/2] Récupération du cache devices/objets (/api/objects)...{C.END}")
    try:
        devices = fetch_objects(base_url, auth, args.timeout)
        total_objects = sum(len(d.get("objects", [])) for d in devices)
        ok(f"{len(devices)} device(s) — {total_objects} objet(s) au total")
        for d in devices:
            objs = d.get("objects", [])
            enabled = sum(1 for o in objs if o.get("poll"))
            info(f"  Device {d.get('device_id')} ({d.get('name','?')}) : {len(objs)} objets, {enabled} en polling")
    except requests.RequestException as e:
        fail(f"Impossible de joindre /api/objects : {e}")
        sys.exit(1)

    # --- Construction du backup ---
    backup = {
        "_meta": {
            "tool":         "nvs_backup.py",
            "version":      "1.0.0",
            "created_at":   datetime.now().isoformat(timespec='seconds'),
            "esp_ip":       args.ip,
            "firmware_ver": firmware_ver,
        },
        "config":  config_snapshot,
        "devices": devices,
    }

    # --- Écriture du fichier ---
    if args.output:
        outpath = args.output
    else:
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        outpath = os.path.join(
            os.path.dirname(os.path.abspath(__file__)),
            f"nvs_backup_{ts}.json"
        )

    with open(outpath, "w", encoding="utf-8") as f:
        json.dump(backup, f, indent=2, ensure_ascii=False)

    print(f"\n{C.OK}{C.BOLD}✔  Sauvegarde complète !{C.END}")
    print(f"   Fichier : {C.BOLD}{outpath}{C.END}")
    print(f"   Config  : {len(config_snapshot)} paramètres")
    print(f"   Devices : {len(devices)} ({total_objects} objets)\n")
    return outpath


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Sauvegarde NVS BACnet2MQTT (config + devices/objets)")
    parser.add_argument("--ip",      default="192.168.1.50", help="Adresse IP de l'ESP32 (défaut: 192.168.1.50)")
    parser.add_argument("--user",    default="admin",        help="Basic Auth user (défaut: admin)")
    parser.add_argument("--pass",    dest="passwd", default="admin1234", help="Basic Auth password (défaut: admin1234)")
    parser.add_argument("--output",  default=None,           help="Chemin du fichier de sortie (auto si omis)")
    parser.add_argument("--timeout", type=int, default=10,   help="Timeout HTTP en secondes (défaut: 10)")
    args = parser.parse_args()
    run_backup(args)
