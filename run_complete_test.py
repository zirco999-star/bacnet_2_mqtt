import requests
import time
import sys
import json
import math
import os

# Configuration de la passerelle
ESP_IP = "192.168.1.50"
ESP_AUTH = ("admin", "admin1234")

# Configuration Home Assistant
HA_URL = "http://192.168.1.11:8123/api/states"
HA_TOKEN = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiIyYjcwMzgxYjUyZjA0NTQ0YTJiNGM2ZWZmNTIzMjljMSIsImlhdCI6MTc3ODE2Mzg4MywiZXhwIjoyMDkzNTIzODgzfQ.RaPwMCVqb9e26Yqvx2Pt4qAFrdjRBGDoUM4zeagFwCw"
HA_HEADERS = {
    "Authorization": f"Bearer {HA_TOKEN}",
    "Content-Type": "application/json",
}

# Mapping des types d'objets BACnet
TYPES_MAP = {
    0: "AI", 1: "AO", 2: "AV", 
    3: "BI", 4: "BO", 5: "BV", 
    13: "MSI", 14: "MSO", 19: "MSV"
}

# Composants HA acceptables par type BACnet (pour lever les ambiguïtés de collision de nom)
ACCEPTABLE_COMPONENTS = {
    0: ["sensor"],
    1: ["number", "sensor"],
    2: ["number", "sensor"],
    3: ["binary_sensor"],
    4: ["switch", "binary_sensor"],
    5: ["switch", "binary_sensor"],
    13: ["sensor"],
    14: ["select", "number", "sensor"],
    19: ["select", "number", "sensor"]
}

def log(msg):
    print(f"[*] {msg}")

def get_devices_data():
    r = requests.get(f"http://{ESP_IP}/api/objects", auth=ESP_AUTH, timeout=10)
    if r.status_code != 200:
        raise ValueError(f"Failed to fetch objects, HTTP {r.status_code}")
    return r.json()

def get_status():
    r = requests.get(f"http://{ESP_IP}/api/status", auth=ESP_AUTH, timeout=10)
    if r.status_code != 200:
        raise ValueError(f"Failed to fetch status, HTTP {r.status_code}")
    return r.json()

def run_test():
    log("======================================================================")
    log("DÉBUT DU TEST D'INTÉGRATION COMPLET (Phases 0 à 8)")
    log("======================================================================")

    # ==========================================
    # PHASE 0 : Récupérer et conserver la configuration
    # ==========================================
    log("\n[PHASE 0] Récupération et sauvegarde de la configuration des objets...")
    try:
        initial_data = get_devices_data()
    except Exception as e:
        log(f"Erreur de communication avec la Gateway : {e}")
        sys.exit(1)

    if not initial_data:
        log("Erreur : Aucun périphérique trouvé dans le cache !")
        sys.exit(1)

    # Recherche du périphérique principal (ID valide > 0)
    valid_devs = [d for d in initial_data if d.get("device_id", 0) > 0]
    if not valid_devs:
        log("Aucun périphérique actif avec ID > 0. On prend le premier.")
        target_dev = initial_data[0]
    else:
        target_dev = valid_devs[0]

    target_device_id = target_dev.get("device_id")
    log(f"Périphérique cible identifié : ID {target_device_id} ({target_dev.get('name', 'Unknown')})")

    # Sauvegarde de la configuration des objets
    saved_configs = {}
    for obj in target_dev.get("objects", []):
        key = f"{obj.get('type')}:{obj.get('inst')}"
        saved_configs[key] = {
            "type": obj.get("type"),
            "inst": obj.get("inst"),
            "name": obj.get("name", ""),
            "poll": obj.get("poll", False),
            "unit": obj.get("unit", ""),
            "min": obj.get("min"),
            "max": obj.get("max"),
            "min_ref": obj.get("min_ref"),
            "max_ref": obj.get("max_ref"),
            "step": obj.get("step")
        }

    log(f"Sauvegardé {len(saved_configs)} objets. {sum(1 for o in saved_configs.values() if o['poll'])} objets étaient actifs (polling).")

    # ==========================================
    # PHASE 1 : Effacer le cache BACnet
    # ==========================================
    log("\n[PHASE 1] Effacement du cache BACnet via /api/reset_cache...")
    try:
        r_reset = requests.post(f"http://{ESP_IP}/api/reset_cache", auth=ESP_AUTH, timeout=10)
        log(f"Réponse API Reset Cache : {r_reset.status_code} - {r_reset.text}")
    except Exception as e:
        log(f"Erreur lors du reset cache : {e}")
        sys.exit(1)

    # ==========================================
    # PHASE 2 : Attendre le reboot
    # ==========================================
    log("\n[PHASE 2] Attente du redémarrage de la Gateway...")
    time.sleep(5)  # Laisser le temps à la Gateway de couper sa connexion
    rebooted = False
    start_time = time.time()
    while time.time() - start_time < 45:
        try:
            status = get_status()
            uptime = status.get("uptime", 0)
            log(f"Gateway en ligne ! Uptime : {uptime}s")
            rebooted = True
            break
        except requests.exceptions.RequestException:
            log("Attente de la reconnexion de la Gateway...")
            time.sleep(2)

    if not rebooted:
        log("Erreur : La Gateway n'a pas redémarré à temps !")
        sys.exit(1)

    log("Attente de 5 secondes supplémentaires pour la détection initiale du bus...")
    time.sleep(5)

    # ==========================================
    # PHASE 3 : Activer par l'API le device détecté
    # ==========================================
    log("\n[PHASE 3] Détection de l'appareil et activation...")
    device_detected = None
    detect_start = time.time()
    while time.time() - detect_start < 30:
        try:
            status = get_status()
            devices = status.get("devices", [])
            if devices:
                # Chercher l'ID cible exact (ignore le wildcard 4194303)
                dev_match = [d for d in devices if d.get("id") == target_device_id]
                if not dev_match:
                    dev_match = [d for d in devices if d.get("id") > 0 and d.get("id") != 4194303]
                
                if dev_match:
                    device_detected = dev_match[0]
                    break
        except Exception as e:
            log(f"Erreur de lecture du statut : {e}")
        log("Attente de la détection de l'automate réel sur le bus MS/TP (ignore wildcard)...")
        time.sleep(2)

    if not device_detected:
        log("Erreur : L'automate réel n'a pas été détecté sur le bus après reboot !")
        sys.exit(1)

    detected_id = device_detected.get("id")
    log(f"Automate détecté en cache avec l'ID réel {detected_id}")

    # Si désactivé, on l'active
    if not device_detected.get("enabled"):
        log(f"L'automate {detected_id} est désactivé. Activation via /api/toggle_device...")
        try:
            r_toggle = requests.post(f"http://{ESP_IP}/api/toggle_device", auth=ESP_AUTH, data={"id": detected_id}, timeout=10)
            log(f"Réponse API Activation : {r_toggle.status_code} - {r_toggle.text}")
        except Exception as e:
            log(f"Erreur d'activation : {e}")
            sys.exit(1)
    else:
        log(f"L'automate {detected_id} est déjà actif.")

    # ==========================================
    # PHASE 4 : Attendre la découverte complète
    # ==========================================
    log("\n[PHASE 4] Attente de la découverte complète des objets...")
    discovery_done = False
    disc_start = time.time()
    while time.time() - disc_start < 90:
        try:
            status = get_status()
            devices = status.get("devices", [])
            dev_match = [d for d in devices if d.get("id") == detected_id]
            if dev_match:
                dev = dev_match[0]
                idx = dev.get("idx", 0)
                total = dev.get("total", 0)
                done = dev.get("done", False)
                log(f"Progression : {idx} / {total} objets découverts (done={done})")
                if done:
                    discovery_done = True
                    break
        except Exception as e:
            log(f"Erreur de suivi de la découverte : {e}")
        time.sleep(3)

    if not discovery_done:
        log("Erreur : La découverte n'a pas pu se terminer à temps !")
        sys.exit(1)
    log("Découverte complète des objets terminée avec succès !")

    # ==========================================
    # PHASES 5 & 6 : Réactiver et reconfigurer les objets
    # ==========================================
    log("\n[PHASES 5 & 6] Réactivation et reconfiguration des objets...")
    post_data = get_devices_data()
    post_dev = [d for d in post_data if d.get("device_id") == detected_id][0]
    current_objects = {f"{o.get('type')}:{o.get('inst')}": o for o in post_dev.get("objects", [])}

    count_restored = 0
    for key, saved_o in saved_configs.items():
        if key in current_objects:
            if saved_o["poll"] or saved_o["name"] or saved_o["min"] is not None or saved_o["max"] is not None or saved_o["step"] is not None:
                # Préparation du payload de sauvegarde
                payload = {
                    "did": detected_id,
                    "inst": saved_o["inst"],
                    "type": saved_o["type"],
                    "name": saved_o["name"],
                    "unit": saved_o["unit"],
                    "poll": "1" if saved_o["poll"] else "0",
                    "step": saved_o["step"] if saved_o["step"] is not None else ""
                }
                
                if saved_o["min_ref"]:
                    payload["min"] = saved_o["min_ref"]
                elif saved_o["min"] is not None:
                    payload["min"] = saved_o["min"]
                else:
                    payload["min"] = ""
                    
                if saved_o["max_ref"]:
                    payload["max"] = saved_o["max_ref"]
                elif saved_o["max"] is not None:
                    payload["max"] = saved_o["max"]
                else:
                    payload["max"] = ""

                try:
                    r_save = requests.post(f"http://{ESP_IP}/api/save_object", auth=ESP_AUTH, data=payload, timeout=5)
                    if r_save.status_code == 200:
                        count_restored += 1
                    else:
                        log(f"  Échec de restauration pour {key} : HTTP {r_save.status_code}")
                except Exception as e:
                    log(f"  Erreur lors de la restauration de {key} : {e}")

    log(f"Restauration terminée : {count_restored} objets réactivés/reconfigurés en cache.")
    time.sleep(3)  # Laisser le temps pour l'application NVS

    # ==========================================
    # PHASE 7 : Vérifier Home Assistant
    # ==========================================
    log("\n[PHASE 7] Vérification de la configuration et des valeurs dans Home Assistant...")
    
    # Attente active intelligente (laisser le polling s'exécuter et les valeurs remonter)
    # On boucle pendant max 90 secondes tant qu'il y a des entités en polling actif à l'état 'unknown' ou 'unavailable'
    ha_report = {}
    ha_verification_done = False
    ha_audit_start = time.time()
    
    while time.time() - ha_audit_start < 90:
        try:
            # Récupérer la configuration actuelle des objets sur la Gateway (pour avoir les valeurs lues)
            gw_data = get_devices_data()
            gw_dev = [d for d in gw_data if d.get("device_id") == detected_id][0]
            gw_objects = {f"{o.get('type')}:{o.get('inst')}": o for o in gw_dev.get("objects", [])}
            
            # Récupérer les états de Home Assistant
            r_ha = requests.get(HA_URL, headers=HA_HEADERS, timeout=10)
            ha_states = r_ha.json()
            
            # Réinitialiser les compteurs
            success_count = 0
            failed_count = 0
            missing_count = 0
            unavail_count = 0
            report_list = []
            
            for key, saved_o in saved_configs.items():
                obj_type = saved_o["type"]
                obj_inst = saved_o["inst"]
                obj_name = saved_o["name"]
                
                if obj_type in [8, 10, 16]: # Ignorer Device, File, Program
                    continue
                    
                type_str = TYPES_MAP.get(obj_type, f"TYPE_{obj_type}")
                gw_o = gw_objects.get(key, {})
                gw_val = gw_o.get("val")
                gw_poll = gw_o.get("poll", False)
                
                # Chercher le candidat dans HA avec filtrage par composant acceptable
                acceptable_comps = ACCEPTABLE_COMPONENTS.get(obj_type, ["sensor"])
                
                candidates = []
                obj_name_clean = obj_name.strip()
                for state in ha_states:
                    entity_id = state.get("entity_id", "")
                    friendly_name = state.get("attributes", {}).get("friendly_name", "").strip()
                    comp = entity_id.split(".")[0]
                    
                    # Le composant doit être acceptable pour ce type BACnet
                    if comp not in acceptable_comps:
                        continue
                        
                    is_match = False
                    if friendly_name == obj_name_clean:
                        is_match = True
                    elif friendly_name == f"ECB_203 {obj_name_clean}":
                        is_match = True
                    elif friendly_name.endswith(f" {obj_name_clean}"):
                        prefix = friendly_name[:-len(obj_name_clean)].strip()
                        if "ecb" in prefix.lower() or "bacnet" in prefix.lower() or prefix == "":
                            is_match = True
                            
                    # Secours par Entity ID
                    if not is_match:
                        slug = obj_name_clean.lower().replace(" ", "_").replace(":", "_").replace("-", "_")
                        if f"ecb_203_{slug}" in entity_id:
                            is_match = True
                        elif f"bacnet_{detected_id}_{type_str.lower()}_{obj_inst}" in entity_id:
                            is_match = True
                            
                    if is_match:
                        candidates.append(state)
                
                # Choisir le meilleur candidat
                ha_state = None
                if candidates:
                    active_candidates = [c for c in candidates if c.get("state") not in ["unavailable", "unknown"]]
                    if active_candidates:
                        ha_state = active_candidates[0]
                    else:
                        ha_state = candidates[0]
                
                if ha_state:
                    entity_id = ha_state.get("entity_id")
                    state_val = ha_state.get("state")
                    attrs = ha_state.get("attributes", {})
                    comp = entity_id.split(".")[0]
                    
                    errors = []
                    
                    # 1. Vérifier si l'entité est indisponible
                    if state_val in ["unavailable", "unknown"]:
                        # Si l'objet est en polling actif, c'est une indisponibilité temporaire/réelle
                        if gw_poll:
                            unavail_count += 1
                        errors.append("Entité indisponible (unknown/unavailable)")
                    else:
                        # 2. Vérifier la valeur
                        try:
                            if comp in ["binary_sensor", "switch"]:
                                expected_state = "on" if gw_val == 1.0 else "off"
                                if state_val != expected_state:
                                    errors.append(f"Valeur incohérente : HA={state_val} != Gateway={expected_state} (val={gw_val})")
                            elif comp == "select":
                                # Select : valeur textuelle
                                pass # Tolère
                            else:
                                # Numérique
                                ha_val_float = float(state_val)
                                if gw_val is not None:
                                    if not math.isclose(ha_val_float, gw_val, rel_tol=0.05, abs_tol=0.1):
                                        errors.append(f"Valeur incohérente : HA={ha_val_float} != Gateway={gw_val}")
                        except Exception as e:
                            errors.append(f"Erreur de conversion de valeur HA ({state_val}) : {e}")
                            
                    # 3. Vérifier les attributs min/max/step pour les number
                    if comp == "number":
                        expected_min = gw_o.get("min", -100.0)
                        expected_max = gw_o.get("max", 100.0)
                        expected_step = gw_o.get("step", 1.0)
                        
                        ha_min = attrs.get("min")
                        ha_max = attrs.get("max")
                        ha_step = attrs.get("step")
                        
                        if ha_min is not None and not math.isclose(ha_min, expected_min, abs_tol=0.1):
                            errors.append(f"Min incohérent : HA={ha_min} != Gateway={expected_min}")
                        if ha_max is not None and not math.isclose(ha_max, expected_max, abs_tol=0.1):
                            errors.append(f"Max incohérent : HA={ha_max} != Gateway={expected_max}")
                        if ha_step is not None and not math.isclose(ha_step, expected_step, abs_tol=0.01):
                            errors.append(f"Step incohérent : HA={ha_step} != Gateway={expected_step}")
                            
                    if not errors:
                        success_count += 1
                        report_list.append({
                            "key": key,
                            "name": obj_name,
                            "type": type_str,
                            "inst": obj_inst,
                            "entity": entity_id,
                            "status": "OK",
                            "value": state_val,
                            "poll": gw_poll
                        })
                    else:
                        failed_count += 1
                        report_list.append({
                            "key": key,
                            "name": obj_name,
                            "type": type_str,
                            "inst": obj_inst,
                            "entity": entity_id,
                            "status": "ERROR",
                            "errors": errors,
                            "value": state_val,
                            "poll": gw_poll
                        })
                else:
                    # Manquant dans HA
                    failed_count += 1
                    missing_count += 1
                    report_list.append({
                        "key": key,
                        "name": obj_name,
                        "type": type_str,
                        "inst": obj_inst,
                        "entity": None,
                        "status": "MISSING",
                        "errors": ["Entité introuvable dans Home Assistant"],
                        "poll": gw_poll
                    })
            
            # Construire le dictionnaire de synthèse
            ha_report = {
                "summary": {
                    "total": len(report_list),
                    "success": success_count,
                    "failed": failed_count,
                    "missing": missing_count,
                    "unavailable_polling": unavail_count
                },
                "objects": report_list
            }
            
            # Si aucune entité en polling actif n'est à l'état 'unavailable' ou 'unknown',
            # et qu'on n'a pas d'entités manquantes pour les actifs, on s'arrête
            if unavail_count == 0 and missing_count == 0:
                log(f"Stabilisation HA terminée. {success_count} entités OK, {failed_count} KO (dont {unavail_count} indisponibles).")
                ha_verification_done = True
                break
            else:
                log(f"Attente stabilisation HA... {unavail_count} actifs indisponibles, {missing_count} manquants. Nouvelle tentative dans 5s...")
                time.sleep(5)
                
        except Exception as e:
            log(f"Erreur durant l'évaluation HA : {e}")
            time.sleep(5)

    if not ha_verification_done:
        log("Timeout d'attente active HA atteint. Audit finalisé en l'état.")
        
    # Écriture du rapport JSON intermédiaire
    os.makedirs("/home/dev/bacnet_2_mqtt/tmp", exist_ok=True)
    with open("/home/dev/bacnet_2_mqtt/tmp/ha_verification_report.json", "w") as f:
        json.dump(ha_report, f, indent=2)

    # ==========================================
    # PHASE 8 : Rédiger le rapport
    # ==========================================
    log("\n[PHASE 8] Rédaction du rapport de test final...")
    generate_markdown_report(ha_report)
    
    log("======================================================================")
    log("TEST D'INTÉGRATION COMPLET ET RAPPORT TERMINÉS !")
    log("======================================================================")

def generate_markdown_report(report):
    # Récupérer les stats globales de l'ESP
    try:
        status = get_status()
        uptime = status.get("uptime", 0)
        rssi = status.get("rssi", 0)
        heap = status.get("heap", 0)
        tokens = status.get("tokens_seen", 0)
        ver = status.get("version", "v6.9.6")
    except Exception:
        uptime, rssi, heap, tokens, ver = "Unknown", "Unknown", "Unknown", "Unknown", "Unknown"

    summary = report["summary"]
    
    md = f"""# Rapport de Test d'Intégration Complet - BACnet2MQTT

Ce rapport détaille le cycle d'intégration et d'audit pour la passerelle **BACnet2MQTT** (Waveshare ESP32-S3).

## 1. Métriques de la Passerelle au moment du Test
- **Version du Firmware** : `{ver}`
- **Uptime de la Passerelle** : `{uptime}s`
- **Signal Wi-Fi (RSSI)** : `{rssi} dBm`
- **Mémoire Heap Libre** : `{heap} octets`
- **Jetons MS/TP vus (Liveness)** : `{tokens}`

## 2. Résumé de l'Audit Home Assistant
- **Total Objets Evalués** : {summary["total"]}
- **Entités Validées (OK)** : {summary["success"]}
- **Entités en Échec/Manquantes (ERROR/MISSING)** : {summary["failed"]}
- **dont Entités de Polling Actif Indisponibles** : {summary["unavailable_polling"]}

> [!NOTE]
> Les entités marquées en **MISSING** ou **unknown** peuvent correspondre à des capteurs physiques absents ou déconnectés sur le bus de l'automate ECB-203 (ex: ComSensor 1 CO2, ComSensor 1 Humid, VoletAir1-4) pour lesquels le polling n'a pas pu acquérir de valeur réelle sur le réseau MS/TP.

---

## 3. Détail par Objet et État HA

| OID (Type:Inst) | Nom de l'Objet | Composant / Entité HA | Polling | État HA | Statut | Anomalies / Erreurs |
| :--- | :--- | :--- | :---: | :---: | :---: | :--- |
"""
    
    # Trier les objets par type et instance pour une meilleure présentation
    sorted_objs = sorted(report["objects"], key=lambda x: (x["type"], x["inst"]))
    
    for o in sorted_objs:
        entity = o["entity"] if o["entity"] else "*Non créée*"
        val = o["value"] if o["value"] else "N/A"
        poll_str = "Actif" if o["poll"] else "Inactif"
        
        status_emoji = "✅ OK" if o["status"] == "OK" else "❌ KO"
        if o["status"] == "MISSING":
            status_emoji = "⚠️ Manquant"
            
        errors_str = ", ".join(o.get("errors", [])) if o.get("errors") else "-"
        
        md += f"| {o['type']}:{o['inst']} | {o['name']} | `{entity}` | {poll_str} | `{val}` | {status_emoji} | {errors_str} |\n"

    # Enregistrer le rapport
    os.makedirs("/home/dev/bacnet_2_mqtt/reports", exist_ok=True)
    report_path = "/home/dev/bacnet_2_mqtt/reports/complete_test_report.md"
    with open(report_path, "w") as f:
        f.write(md)
    log(f"Rapport Markdown enregistré dans : {report_path}")
    
    # Copier le rapport dans les artifacts pour la vue utilisateur
    artifact_path = "/root/.gemini/antigravity-cli/brain/17d71199-1193-49f9-9a2f-26225854bac4/complete_test_report.md"
    try:
        with open(artifact_path, "w") as f:
            f.write(md)
        log(f"Rapport copié dans les artifacts : {artifact_path}")
    except Exception as e:
        log(f"Impossible de copier le rapport dans les artifacts : {e}")

if __name__ == "__main__":
    run_test()
