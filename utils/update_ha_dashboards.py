#!/usr/bin/env python3
import json
import urllib.request
import os
import re

# File paths for Lovelace dashboard config
STORAGE_DIR = "/home/dev/homeassistant/.storage"
DASHBOARDS_REG = os.path.join(STORAGE_DIR, "lovelace_dashboards")
DASHBOARD_FILE = os.path.join(STORAGE_DIR, "lovelace.b2m")

# Default settings (can be overridden by user prompts or env vars)
DEFAULT_HA_URL = os.environ.get("HA_URL", "http://192.168.1.11:8123")
DEFAULT_HA_TOKEN = os.environ.get("HA_TOKEN", "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiIyYjcwMzgxYjUyZjA0NTQ0YTJiNGM2ZWZmNTIzMjljMSIsImlhdCI6MTc3ODE2Mzg4MywiZXhwIjoyMDkzNTIzODgzfQ.RaPwMCVqb9e26Yqvx2Pt4qAFrdjRBGDoUM4zeagFwCw")
DEFAULT_GATEWAY_IP = os.environ.get("GATEWAY_IP", "192.168.1.50")

def slugify(text):
    text = text.lower()
    text = re.sub(r'[^a-z0-9_]', '_', text)
    text = re.sub(r'_+', '_', text)
    return text.strip('_')

def get_local_config():
    config_file = "/home/dev/bacnet_2_mqtt/config_local.json"
    if os.path.exists(config_file):
        try:
            with open(config_file, 'r', encoding='utf-8') as f:
                return json.load(f)
        except Exception:
            pass
    return {}

def make_request(url, headers=None, data=None, method="GET"):
    req = urllib.request.Request(url, headers=headers or {}, method=method)
    if data:
        req.data = json.dumps(data).encode('utf-8')
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=10) as response:
            return json.loads(response.read().decode('utf-8'))
    except Exception as e:
        print(f"Request to {url} failed: {e}")
        return None

def discover_from_gateway(gateway_ip):
    print(f"Querying B2M Gateway at http://{gateway_ip}/api/objects ...")
    url = f"http://{gateway_ip}/api/objects"
    # Basic auth is configured for admin:admin1234 by default
    import base64
    auth_header = base64.b64encode(b"admin:admin1234").decode('utf-8')
    headers = {"Authorization": f"Basic {auth_header}"}
    res = make_request(url, headers=headers)
    if res and isinstance(res, list):
        for dev in res:
            if "objects" in dev and isinstance(dev["objects"], list):
                dev["objects"] = [obj for obj in dev["objects"] if obj.get("poll") is True]
    return res

def discover_from_ha(ha_url, ha_token):
    print(f"Querying Home Assistant at {ha_url}/api/states ...")
    url = f"{ha_url}/api/states"
    headers = {
        "Authorization": f"Bearer {ha_token}",
        "Content-Type": "application/json"
    }
    states = make_request(url, headers=headers)
    if not states:
        return None

    # Parse devices and entities from states
    devices = {}
    
    # Let's find all B2M entities by looking for the '_forcage_bacnet' suffix
    for s in states:
        entity_id = s['entity_id']
        if entity_id.endswith('_forcage_bacnet'):
            domain, full_name = entity_id.split('.', 1)
            base_name = full_name[:-15]  # Remove '_forcage_bacnet'
            
            # Detect device prefix and object name
            parts = base_name.split('_')
            if len(parts) >= 2 and parts[1].isdigit():
                dev_prefix = f"{parts[0]}_{parts[1]}"
                obj_name = "_".join(parts[2:])
            else:
                dev_prefix = parts[0]
                obj_name = "_".join(parts[1:])
            
            # Skip gateway itself
            if "gateway" in dev_prefix or "b2m" in dev_prefix:
                continue

            if dev_prefix not in devices:
                devices[dev_prefix] = {"name": dev_prefix.upper(), "objects": {}}

            # Determine component type from domain or matching entities
            # Default to AV if number, BV if switch, AI if sensor, etc.
            obj_type = "AV"
            if domain == "sensor":
                obj_type = "AI"
            elif domain == "binary_sensor":
                obj_type = "BI"
            elif domain == "switch":
                obj_type = "BV"
            elif domain == "select":
                obj_type = "MSV"
            
            # Let's see if we can find if it's an output or value
            if obj_type == "AV" and "valve" in obj_name or "fan" in obj_name or "volet" in obj_name or "ventil" in obj_name:
                obj_type = "AO"
            elif obj_type == "BV" and "actionneur" in obj_name or "relais" in obj_name:
                obj_type = "BO"

            # Check if this object is already added
            devices[dev_prefix]["objects"][obj_name] = {
                "name": obj_name.replace("_", " ").title(),
                "type": obj_type,
                "is_commandable": domain in ["number", "switch", "select", "button"]
            }

    # Format like gateway API response for uniform generator processing
    formatted_devices = []
    for dev_prefix, dev_info in devices.items():
        objects_list = []
        for obj_name, obj_info in dev_info["objects"].items():
            # Convert type to numeric code
            type_code = 2 # AV default
            t = obj_info["type"]
            if t == "AI": type_code = 0
            elif t == "AO": type_code = 1
            elif t == "AV": type_code = 2
            elif t == "BI": type_code = 3
            elif t == "BO": type_code = 4
            elif t == "BV": type_code = 5
            elif t == "MSV": type_code = 19
            
            objects_list.append({
                "name": obj_info["name"],
                "type": type_code,
                "inst": 0,  # Instance not strictly needed if we map by slug name
                "xIsCommandable": obj_info["is_commandable"]
            })
        
        formatted_devices.append({
            "name": dev_info["name"],
            "objects": objects_list
        })
    
    return formatted_devices

def generate_dashboard(devices):
    print("Generating generic Lovelace dashboard JSON...")
    
    # 1. Accueil Tab Gateway State Markdown Card
    md_content = """### 🚨 États Généraux des Objets

* **Alarme Active** : {% if states.sensor | selectattr('entity_id', 'search', '^sensor\\..*_.*_alarme$') | selectattr('state', 'eq', 'OUI') | list | count > 0 %}🔴 OUI (Alarme active sur le bus){% else %}🟢 NON{% endif %}
* **Défaut Détecté** : {% if states.sensor | selectattr('entity_id', 'search', '^sensor\\..*_.*_defaut$') | selectattr('state', 'eq', 'OUI') | list | count > 0 %}⚠️ OUI (Défaut matériel détecté){% else %}🟢 NON{% endif %}
* **Forçage Physique (PLC)** : {% if states.sensor | selectattr('entity_id', 'search', '^sensor\\..*_.*_forcage_manuel$') | selectattr('state', 'eq', 'OUI') | list | count > 0 %}🟠 OUI (Interrupteur PLC commuté){% else %}🟢 NON{% endif %}
* **Forçage BACnet (Réseau)** : {% if states.sensor | selectattr('entity_id', 'search', '^sensor\\..*_.*_forcage_bacnet$') | selectattr('state', 'eq', 'OUI') | list | count > 0 %}🔵 OUI (Contrôle forcé via HA/MQTT){% else %}🟢 NON (Automatique){% endif %}
* **Hors Service (OOS)** : {% if states.sensor | selectattr('entity_id', 'search', '^sensor\\..*_.*_hors_service$') | selectattr('state', 'eq', 'OUI') | list | count > 0 %}⚫ OUI (Des sondes sont simulées){% else %}🟢 NON{% endif %}
"""

    views = [
        # --- VIEW 1 : ACCUEIL ---
        {
            "title": "Accueil",
            "path": "accueil",
            "icon": "mdi:home",
            "cards": [
                {
                    "type": "horizontal-stack",
                    "cards": [
                        {
                            "type": "entities",
                            "title": "Passerelle BACnet2MQTT",
                            "entities": [
                                {"entity": "sensor.bacnet2mqtt_gateway_gateway_version", "name": "Version Globale"},
                                {"entity": "sensor.bacnet2mqtt_gateway_gateway_uptime", "name": "Temps d'activité"},
                                {"entity": "sensor.bacnet2mqtt_gateway_gateway_wifi_rssi", "name": "Signal Wi-Fi RSSI"},
                                {"entity": "sensor.bacnet2mqtt_gateway_gateway_chip_temp", "name": "Température CPU ESP32-S3"},
                                {"entity": "binary_sensor.bacnet2mqtt_gateway_gateway_ms_tp_network", "name": "État du Bus MS/TP"},
                                {"entity": "sensor.bacnet2mqtt_gateway_gateway_devices_count", "name": "Nombre d'automates vus"}
                            ]
                        },
                        {
                            "type": "button",
                            "name": "Redémarrer la Passerelle",
                            "icon": "mdi:restart",
                            "show_state": False,
                            "tap_action": {
                                "action": "call-service",
                                "service": "button.press",
                                "target": {
                                    "entity_id": "button.b2m_reboot_gateway"
                                },
                                "confirmation": {
                                    "text": "Voulez-vous vraiment redémarrer la passerelle BACnet2MQTT ?"
                                }
                            }
                        }
                    ]
                },
                {
                    "type": "markdown",
                    "title": "🚨 Status : Alertes & Forçages",
                    "content": md_content
                }
            ]
        }
    ]

    TYPE_MAP = {
        0: "AI",
        1: "AO",
        2: "AV",
        3: "BI",
        4: "BO",
        5: "BV",
        19: "MSV"
    }

    TYPE_LABELS = {
        "AI": "Entrées Analogiques (AI)",
        "AO": "Sorties Analogiques (AO)",
        "AV": "Valeurs Analogiques (AV)",
        "BI": "Entrées Binaires (BI)",
        "BO": "Sorties Binaires (BO)",
        "BV": "Valeurs Binaires (BV)",
        "MSV": "Valeurs Multi-États (MSV)"
    }

    # --- DEVICE VIEWS ---
    for dev in devices:
        dev_name = dev.get("name", "Unknown Device")
        dev_prefix = slugify(dev_name)
        
        # Group objects by BACnet type abbreviation
        grouped_objects = {}
        for obj in dev.get("objects", []):
            type_code = obj.get("type", 2)
            type_abbr = TYPE_MAP.get(type_code, "AV")
            if type_abbr not in grouped_objects:
                grouped_objects[type_abbr] = []
            grouped_objects[type_abbr].append(obj)
            
        device_cards = []
        
        # For each object type present, create a full-width block
        for type_abbr in ["AI", "AO", "AV", "BI", "BO", "BV", "MSV"]:
            if type_abbr not in grouped_objects:
                continue
                
            objects = grouped_objects[type_abbr]
            grid_cards = []
            
            for obj in objects:
                obj_name = obj.get("name", "Unknown Object")
                obj_slug = slugify(obj_name)
                is_commandable = obj.get("xIsCommandable", False) or type_abbr in ["AO", "BO", "MSO"]
                
                # Determine HA components and suffixes based on object type
                entities = []
                
                if type_abbr == "AI":
                    entities.append({"entity": f"sensor.{dev_prefix}_{obj_slug}", "name": "Valeur"})
                    entities.append({"entity": f"switch.{dev_prefix}_{obj_slug}_out_of_service", "name": "Simuler Sonde (OOS)"})
                    entities.append({"entity": f"number.{dev_prefix}_{obj_slug}_forcing_value", "name": "Val Simul."})
                    entities.append({"entity": f"sensor.{dev_prefix}_{obj_slug}_forcage_bacnet", "name": "Forçage Réseau"})
                    entities.append({"entity": f"sensor.{dev_prefix}_{obj_slug}_alarme", "name": "Alarme"})
                    entities.append({"entity": f"sensor.{dev_prefix}_{obj_slug}_defaut", "name": "Défaut"})
                elif type_abbr == "BI":
                    entities.append({"entity": f"binary_sensor.{dev_prefix}_{obj_slug}", "name": "Valeur"})
                elif type_abbr in ["AO", "AV", "MSV"]:
                    domain = "select" if type_abbr == "MSV" else "number"
                    entities.append({"entity": f"{domain}.{dev_prefix}_{obj_slug}", "name": "Commande"})
                    if is_commandable:
                        entities.append({"entity": f"switch.{dev_prefix}_{obj_slug}_manual_operator", "name": "Activer Mode Manuel"})
                        entities.append({"entity": f"button.{dev_prefix}_{obj_slug}_reset", "name": "Libérer (AUTO)"})
                        entities.append({"entity": f"sensor.{dev_prefix}_{obj_slug}_forcage_bacnet", "name": "Forçage Réseau"})
                elif type_abbr in ["BO", "BV"]:
                    entities.append({"entity": f"switch.{dev_prefix}_{obj_slug}", "name": "Commande"})
                    if is_commandable:
                        entities.append({"entity": f"switch.{dev_prefix}_{obj_slug}_manual_operator", "name": "Activer Mode Manuel"})
                        entities.append({"entity": f"button.{dev_prefix}_{obj_slug}_reset", "name": "Libérer (AUTO)"})
                        entities.append({"entity": f"sensor.{dev_prefix}_{obj_slug}_forcage_bacnet", "name": "Forçage Réseau"})
                
                grid_cards.append({
                    "type": "entities",
                    "title": obj_name,
                    "entities": entities
                })
                
            # Create a 2-column grid representing this BACnet type block
            device_cards.append({
                "type": "grid",
                "columns": 2,
                "square": False,
                "title": TYPE_LABELS.get(type_abbr, f"Objets {type_abbr}"),
                "cards": grid_cards
            })

        views.append({
            "title": dev_name,
            "path": dev_prefix,
            "icon": "mdi:lan-connect",
            "type": "panel",
            "panel": True,
            "cards": [
                {
                    "type": "vertical-stack",
                    "cards": device_cards
                }
            ]
        })

    return {
        "version": 1,
        "minor_version": 1,
        "key": "lovelace.b2m",
        "data": {
            "config": {
                "title": "B2M Gateway",
                "views": views
            }
        }
    }

def update_dashboards_registry():
    print("Updating core Lovelace dashboards registry...")
    if os.path.exists(DASHBOARDS_REG):
        try:
            with open(DASHBOARDS_REG, 'r', encoding='utf-8') as f:
                reg_data = json.load(f)
            
            items = reg_data.get("data", {}).get("items", [])
            
            # Check if b2m entry already exists or needs to be added
            new_exists = any(item.get("id") == "b2m" for item in items)
            if not new_exists:
                items.append({
                    "id": "b2m",
                    "show_in_sidebar": True,
                    "title": "B2M Gateway",
                    "require_admin": False,
                    "mode": "storage",
                    "url_path": "b2m-gateway",
                    "icon": "mdi:lan"
                })
                
                with open(DASHBOARDS_REG, 'w', encoding='utf-8') as f:
                    json.dump(reg_data, f, indent=2, ensure_ascii=False)
                print("Dashboards registry successfully updated with B2M entry.")
            else:
                print("B2M entry already exists in Lovelace dashboards registry.")
        except Exception as e:
            print(f"Failed to update dashboards registry: {e}")
    else:
        print("Lovelace dashboards registry file not found. Registry update skipped.")

def main():
    print("=========================================================")
    print("   Générateur de Dashboard Home Assistant BACnet2MQTT")
    print("=========================================================\n")
    
    local_cfg = get_local_config()
    
    # Prompt the user for credentials
    ha_url = local_cfg.get("ha_url", DEFAULT_HA_URL)
    ha_token = local_cfg.get("ha_token", DEFAULT_HA_TOKEN)
    gateway_ip = local_cfg.get("gateway_ip", DEFAULT_GATEWAY_IP)
    
    print(f"Configuration chargée depuis local/défaut.")
    u_url = input(f"URL de Home Assistant [{ha_url}] : ").strip() or ha_url
    u_token = input(f"Jeton d'accès (Token) [{ha_token[:10]}...] : ").strip() or ha_token
    u_gw = input(f"IP de la passerelle BACnet2MQTT [{gateway_ip}] : ").strip() or gateway_ip
    
    print()
    devices = None
    
    # Try discovering from Gateway API first
    try:
        devices = discover_from_gateway(u_gw)
    except Exception as e:
        print(f"Impossible de joindre la passerelle : {e}")
        
    # If gateway discovery failed, fallback to Home Assistant States API
    if not devices:
        print("Tentative de repli via l'API Home Assistant...")
        try:
            devices = discover_from_ha(u_url, u_token)
        except Exception as e:
            print(f"Impossible de requêter Home Assistant : {e}")

    if not devices:
        print("\n[ERREUR] Impossible de découvrir vos équipements BACnet.")
        print("Veuillez vous assurer que la passerelle est en ligne ou que vos entités sont visibles sous Home Assistant.")
        return
        
    print(f"\nÉquipements découverts : {len(devices)}")
    for d in devices:
        print(f" - Périphérique : {d.get('name')} ({len(d.get('objects', []))} objets)")
        
    # Generate Lovelace JSON config
    dashboard_json = generate_dashboard(devices)
    
    # Check if .storage path exists and write config
    if os.path.exists(STORAGE_DIR):
        try:
            with open(DASHBOARD_FILE, 'w', encoding='utf-8') as f:
                json.dump(dashboard_json, f, indent=2, ensure_ascii=False)
            print(f"\nFichier dashboard Lovelace écrit à : {DASHBOARD_FILE}")
            
            # Register in Lovelace registry
            update_dashboards_registry()
            
            print("\n[SUCCÈS] Dashboard créé !")
            print("Vous pouvez recharger Home Assistant pour voir l'onglet 'B2M Gateway' dans la barre latérale.")
        except Exception as e:
            print(f"\n[ERREUR] Échec de l'écriture locale du fichier dashboard : {e}")
    else:
        # Print representation if directory doesn't exist
        print("\nRépertoire Lovelace local non trouvé (.storage/).")
        print("Voici le JSON de configuration généré que vous pouvez copier dans votre dashboard brut :")
        print(json.dumps(dashboard_json, indent=2, ensure_ascii=False))

if __name__ == "__main__":
    main()
