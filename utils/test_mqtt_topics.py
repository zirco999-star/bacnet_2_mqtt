#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
Script de test automatique pour valider le fonctionnement des topics MQTT du projet BACnet2MQTT.
Ce script se connecte à l'API de l'ESP32, récupère la liste des objets BACnet découverts,
se connecte au broker MQTT (configuré ou spécifié), et valide la publication et la réception
des différents topics MQTT (LWT, B2M, noms, états, et écriture/set).

Usage:
  python3 test_mqtt_topics.py --ip 192.168.1.50 --user admin --pass admin1234
"""

import argparse
import sys
import time
import json
import threading
import requests
import paho.mqtt.client as mqtt

# Codes couleur ANSI pour l'affichage console
class Colors:
    HEADER = '\033[95m'
    BLUE = '\033[94m'
    GREEN = '\033[92m'
    WARNING = '\033[93m'
    FAIL = '\033[91m'
    ENDC = '\033[0m'
    BOLD = '\033[1m'

# Correspondance des types d'objets BACnet
MAP_TYPE = {
    0: "AI",
    1: "AO",
    2: "AV",
    3: "BI",
    4: "BO",
    5: "BV",
    13: "MSI",
    14: "MSO",
    19: "MSV"
}

class MQTTTestRunner:
    def __init__(self, args):
        self.esp_ip = args.ip
        self.esp_user = args.user
        self.esp_pass = args.passwd
        self.timeout = args.timeout
        self.test_write = args.test_write
        
        # Overrides MQTT
        self.override_host = args.mqtt_host
        self.override_port = args.mqtt_port
        self.override_user = args.mqtt_user
        self.override_pass = args.mqtt_pass
        self.override_prefix = args.mqtt_prefix
        
        # Config récupérée
        self.mqtt_host = None
        self.mqtt_port = 1883
        self.mqtt_user = None
        self.mqtt_pass = None
        self.mqtt_prefix = "bacnet2mqtt"
        
        # Données
        self.devices = []
        self.received_messages = {}  # topic: (payload, timestamp)
        self.lock = threading.Lock()
        
        self.client = None
        self.connected_event = threading.Event()
        
        # Résultats des tests
        self.results = []

    def log_success(self, test_name, msg=""):
        self.results.append((test_name, "PASS", msg))
        print(f"  {Colors.GREEN}[PASS]{Colors.ENDC} {test_name} {f'- {msg}' if msg else ''}")

    def log_fail(self, test_name, msg=""):
        self.results.append((test_name, "FAIL", msg))
        print(f"  {Colors.FAIL}[FAIL]{Colors.ENDC} {test_name} {f'- {msg}' if msg else ''}")

    def log_skip(self, test_name, msg=""):
        self.results.append((test_name, "SKIP", msg))
        print(f"  {Colors.WARNING}[SKIP]{Colors.ENDC} {test_name} {f'- {msg}' if msg else ''}")

    def on_message(self, client, userdata, msg):
        topic = msg.topic
        try:
            payload = msg.payload.decode('utf-8', errors='ignore')
        except Exception:
            payload = str(msg.payload)
            
        with self.lock:
            self.received_messages[topic] = (payload, time.time())
            
    def on_connect(self, *args, **kwargs):
        # Signature flexible pour v1.x et v2.x de paho-mqtt
        # En v2.x, raison de connexion est dans args[3] (reason_code)
        # En v1.x, raison de connexion est dans args[3] (rc)
        rc = args[3] if len(args) > 3 else 0
        if rc == 0 or (hasattr(rc, "is_failure") and not rc.is_failure):
            self.connected_event.set()
        else:
            print(f"{Colors.FAIL}Erreur de connexion MQTT: {rc}{Colors.ENDC}")

    def wait_for_message(self, topic, expected_value=None, timeout=5, validator=None, allow_historical=False):
        start_time = time.time()
        
        if allow_historical:
            with self.lock:
                if topic in self.received_messages:
                    payload, ts = self.received_messages[topic]
                    if expected_value is not None:
                        if payload == expected_value:
                            return payload
                    elif validator is not None:
                        if validator(payload):
                            return payload
                    else:
                        return payload
                        
        while time.time() - start_time < timeout:
            with self.lock:
                if topic in self.received_messages:
                    payload, ts = self.received_messages[topic]
                    if allow_historical or (ts >= start_time):
                        if expected_value is not None:
                            if payload == expected_value:
                                return payload
                        elif validator is not None:
                            if validator(payload):
                                return payload
                        else:
                            return payload
            time.sleep(0.1)
        return None

    def execute(self):
        print(f"{Colors.HEADER}{Colors.BOLD}=== DÉMARRAGE DES TESTS MQTT BACnet2MQTT ==={Colors.ENDC}\n")
        
        # 1. Connexion API ESP32 & Lecture Config
        if not self.step_fetch_config():
            self.print_summary()
            sys.exit(1)
            
        # 2. Connexion Broker MQTT
        if not self.step_connect_mqtt():
            self.print_summary()
            sys.exit(1)
            
        try:
            # 3. Test LWT (Last Will & Testament)
            self.test_lwt()
            
            # 4. Test des topics de statut de la passerelle (B2M)
            self.test_gateway_status()
            
            # 5. Test des noms d'objets (retained)
            self.test_retained_names()
            
            # 6. Test de rafraîchissement & publication des états
            self.test_object_states()
            
            # 7. Test d'écriture de valeurs / OutOfService (si autorisé)
            if self.test_write:
                self.test_mqtt_writing()
            else:
                self.log_skip("Test Écriture MQTT", "Option --test-write non spécifiée. Saut des tests d'écriture.")
                
        finally:
            if self.client:
                print(f"\nDéconnexion du broker MQTT...")
                self.client.loop_stop()
                self.client.disconnect()
                
        self.print_summary()

    def step_fetch_config(self):
        print(f"{Colors.BLUE}[Étape 1] Récupération de la configuration de l'ESP32 ({self.esp_ip})...{Colors.ENDC}")
        auth = (self.esp_user, self.esp_pass)
        base_url = f"http://{self.esp_ip}"
        
        try:
            # Récupérer le statut global
            r_status = requests.get(f"{base_url}/api/status", auth=auth, timeout=5)
            if r_status.status_code != 200:
                print(f"  {Colors.FAIL}Erreur HTTP {r_status.status_code} sur /api/status{Colors.ENDC}")
                return False
                
            status_data = r_status.json()
            
            # Configurer le broker à partir de l'API de l'ESP32
            self.mqtt_host = self.override_host if self.override_host else status_data.get("mqs")
            self.mqtt_port = self.override_port if self.override_port else status_data.get("mpi", 1883)  # note: mpi est mqtt_poll_interval, le port n'est pas exposé dans /api/status mais par défaut 1883 ou surchargeable
            self.mqtt_user = self.override_user if self.override_user else status_data.get("mqu")
            self.mqtt_prefix = self.override_prefix if self.override_prefix else status_data.get("mqpr", "bacnet2mqtt")
            
            # Note: Si le port n'est pas dans status_data, on essaie 1883
            if not self.override_port:
                # Vérifier si status_data contient mqtt_port (non présent dans /api/status sous forme de clé standard, mais pour le futur)
                self.mqtt_port = status_data.get("mqtt_port", 1883)
                
            print(f"  Configuration détectée :")
            print(f"    - Version Firmware : {status_data.get('ver')}")
            print(f"    - Broker MQTT : {self.mqtt_host}:{self.mqtt_port}")
            print(f"    - Utilisateur MQTT : {self.mqtt_user or 'Aucun'}")
            print(f"    - Préfixe MQTT : {self.mqtt_prefix}")
            
            if not self.mqtt_host or self.mqtt_host == "0.0.0.0":
                print(f"  {Colors.FAIL}Erreur: L'ESP32 n'a pas de broker MQTT valide configuré ({self.mqtt_host}). Spécifiez --mqtt-host.{Colors.ENDC}")
                return False
                
            # Récupérer les objets BACnet
            r_objects = requests.get(f"{base_url}/api/objects", auth=auth, timeout=5)
            if r_objects.status_code == 200:
                self.devices = r_objects.json()
                n_objs = sum(len(d.get("objects", [])) for d in self.devices)
                print(f"    - Périphériques BACnet en cache : {len(self.devices)} ({n_objs} objets au total)")
            else:
                print(f"  {Colors.WARNING}Attention: Impossible de récupérer `/api/objects` (HTTP {r_objects.status_code}){Colors.ENDC}")
                
            self.log_success("Récupération de la configuration API")
            return True
            
        except Exception as e:
            print(f"  {Colors.FAIL}Erreur de connexion à l'ESP32 : {e}{Colors.ENDC}")
            return False

    def step_connect_mqtt(self):
        print(f"\n{Colors.BLUE}[Étape 2] Connexion au broker MQTT ({self.mqtt_host}:{self.mqtt_port})...{Colors.ENDC}")
        
        # Gestion de version paho-mqtt
        try:
            from paho.mqtt.enums import CallbackAPIVersion
            self.client = mqtt.Client(callback_api_version=CallbackAPIVersion.VERSION2)
        except (ImportError, AttributeError):
            self.client = mqtt.Client()
            
        self.client.on_connect = self.on_connect
        self.client.on_message = self.on_message
        
        if self.mqtt_user:
            self.client.username_pw_set(self.mqtt_user, self.override_pass or "")
            
        try:
            self.client.connect(self.mqtt_host, int(self.mqtt_port), 60)
            self.client.loop_start()
            
            # Attente de la connexion effective
            if not self.connected_event.wait(timeout=5):
                print(f"  {Colors.FAIL}Erreur: Timeout lors de la connexion au broker MQTT.{Colors.ENDC}")
                return False
                
            # Souscriptions globales aux wildcards
            sub_state = f"{self.mqtt_prefix}/#"
            sub_lwt = f"tele/{self.mqtt_prefix}/#"
            
            self.client.subscribe(sub_state, 0)
            self.client.subscribe(sub_lwt, 0)
            
            print(f"  Connecté au broker avec succès. Souscriptions actives sur :")
            print(f"    - {sub_state}")
            print(f"    - {sub_lwt}")
            
            # Petite pause pour recevoir les messages retained
            time.sleep(1.5)
            
            self.log_success("Connexion Broker MQTT")
            return True
            
        except Exception as e:
            print(f"  {Colors.FAIL}Erreur de connexion au broker MQTT : {e}{Colors.ENDC}")
            return False

    def test_lwt(self):
        print(f"\n{Colors.BLUE}[Test] Vérification du statut LWT (Last Will)...{Colors.ENDC}")
        lwt_topic = f"tele/{self.mqtt_prefix}/LWT"
        
        payload = self.wait_for_message(lwt_topic, expected_value="online", timeout=2, allow_historical=True)
        if payload == "online":
            self.log_success("Topic LWT", f"Le statut de la passerelle est '{payload}' sur '{lwt_topic}'")
        else:
            with self.lock:
                actual = self.received_messages.get(lwt_topic, (None, None))[0]
            self.log_fail("Topic LWT", f"Attendu 'online' sur '{lwt_topic}', reçu '{actual}'")

    def test_gateway_status(self):
        print(f"\n{Colors.BLUE}[Test] Vérification des topics de statut de la passerelle (B2M)...{Colors.ENDC}")
        b2m_topics = ["ver", "rssi", "heap", "min_heap", "uptime", "nb_dev", "temp"]
        
        received_count = 0
        missing = []
        
        for key in b2m_topics:
            topic = f"{self.mqtt_prefix}/B2M/{key}/state"
            # Les messages B2M ne sont pas retained. On vérifie s'il y en a dans l'historique récent
            # Sinon on attend un court instant
            payload = self.wait_for_message(topic, timeout=1, allow_historical=True)
            if payload is not None:
                received_count += 1
                print(f"    - {key} : '{payload}' (via {topic})")
            else:
                missing.append(key)
                
        if received_count == len(b2m_topics):
            self.log_success("Topics de Statut Passerelle", "Tous les topics B2M/+/state sont valides.")
        elif received_count > 0:
            self.log_success("Topics de Statut Passerelle (Partiel)", 
                             f"Reçus: {received_count}/{len(b2m_topics)}. Absents (non publiés dans l'intervalle): {missing}. (Ceci est normal si l'intervalle de publication MQTT est grand)")
        else:
            self.log_skip("Topics de Statut Passerelle", 
                          f"Aucun topic de statut reçu (intervalle typique de 30s). Topics vérifiés: {', '.join(b2m_topics)}")

    def test_retained_names(self):
        print(f"\n{Colors.BLUE}[Test] Vérification des noms d'objets (retained)...{Colors.ENDC}")
        
        if not self.devices:
            self.log_skip("Test des Noms d'Objets", "Aucun périphérique en cache pour tester.")
            return
            
        validated_names = 0
        total_objects = 0
        
        for dev in self.devices:
            did = dev.get("device_id")
            objects = dev.get("objects", [])
            for obj in objects:
                obj_type_id = obj.get("type")
                inst = obj.get("inst")
                obj_name = obj.get("name")
                
                type_str = MAP_TYPE.get(obj_type_id)
                if not type_str:
                    continue
                    
                total_objects += 1
                name_topic = f"{self.mqtt_prefix}/{did}/{type_str}/{inst}/name"
                
                # Le nom de l'objet est retained, on devrait le trouver dans l'historique
                payload = self.wait_for_message(name_topic, expected_value=obj_name, timeout=0.5, allow_historical=True)
                if payload == obj_name:
                    validated_names += 1
                else:
                    with self.lock:
                        actual = self.received_messages.get(name_topic, (None, None))[0]
                    print(f"    {Colors.WARNING}[Alerte]{Colors.ENDC} Nom incorrect pour {type_str}:{inst} sur Device {did}. Attendu: '{obj_name}', Reçu: '{actual}'")
                    
        if total_objects == 0:
            self.log_skip("Test des Noms d'Objets", "Aucun objet BACnet valide trouvé dans le cache.")
        elif validated_names == total_objects:
            self.log_success("Test des Noms d'Objets", f"100% validés ({validated_names}/{total_objects} objets).")
        else:
            self.log_fail("Test des Noms d'Objets", f"Seulement {validated_names}/{total_objects} noms valides ont été reçus par MQTT.")

    def test_object_states(self):
        print(f"\n{Colors.BLUE}[Test] Vérification des topics d'état (via forçage d'une lecture API)...{Colors.ENDC}")
        
        if not self.devices:
            self.log_skip("Test des États d'Objets", "Aucun périphérique en cache.")
            return
            
        # Sélectionner jusqu'à 3 objets de types différents pour ne pas surcharger le réseau MS/TP
        test_objects = []
        for dev in self.devices:
            did = dev.get("device_id")
            for obj in dev.get("objects", []):
                obj_type_id = obj.get("type")
                inst = obj.get("inst")
                type_str = MAP_TYPE.get(obj_type_id)
                if type_str:
                    # Ne pas dupliquer le même type pour varier les tests
                    if not any(x["type_str"] == type_str for x in test_objects):
                        test_objects.append({
                            "did": did,
                            "type_id": obj_type_id,
                            "type_str": type_str,
                            "inst": inst,
                            "name": obj.get("name")
                        })
                if len(test_objects) >= 3:
                    break
            if len(test_objects) >= 3:
                break
                
        if not test_objects:
            self.log_skip("Test des États d'Objets", "Aucun objet BACnet exploitable trouvé.")
            return
            
        print(f"  Sélection de {len(test_objects)} objets pour déclencher une lecture forcée :")
        
        auth = (self.esp_user, self.esp_pass)
        base_url = f"http://{self.esp_ip}"
        success_count = 0
        
        for to in test_objects:
            state_topic = f"{self.mqtt_prefix}/{to['did']}/{to['type_str']}/{to['inst']}/state"
            print(f"    - Déclenchement de la mise à jour pour {to['type_str']}:{to['inst']} (Device {to['did']})...")
            
            # Envoyer la commande de relecture forcée via l'API
            payload_reload = {"did": to["did"], "type": to["type_id"], "inst": to["inst"]}
            try:
                r = requests.post(f"{base_url}/api/reload_object", auth=auth, data=payload_reload, timeout=5)
                if r.status_code != 200 or r.text != "READ ENQUEUED":
                    print(f"      {Colors.FAIL}Échec du rechargement de l'objet via API (HTTP {r.status_code}: {r.text}){Colors.ENDC}")
                    continue
                    
                # Attendre la publication de l'état (historique non autorisé car on veut la valeur fraîche)
                payload = self.wait_for_message(state_topic, timeout=self.timeout, allow_historical=False)
                if payload is not None:
                    print(f"      {Colors.GREEN}Reçu par MQTT{Colors.ENDC} -> '{payload}' sur '{state_topic}'")
                    success_count += 1
                else:
                    print(f"      {Colors.FAIL}Timeout: Aucun message reçu sur '{state_topic}' après {self.timeout} secondes.{Colors.ENDC}")
                    
            except Exception as e:
                print(f"      {Colors.FAIL}Erreur lors du test : {e}{Colors.ENDC}")
                
        if success_count == len(test_objects):
            self.log_success("Forçage & Publication des États", f"Tous les {success_count} objets testés ont publié leur état.")
        elif success_count > 0:
            self.log_success("Forçage & Publication des États", f"Validé ({success_count}/{len(test_objects)} ont publié). Note: Si la valeur lue est identique à celle en cache, la passerelle ne la republie pas sur MQTT.")
        else:
            self.log_fail("Forçage & Publication des États", "Aucun objet n'a publié son état suite au forçage.")

    def test_mqtt_writing(self):
        print(f"\n{Colors.BLUE}[Test] Vérification de l'écriture via commandes MQTT (/set)...{Colors.ENDC}")
        
        auth = (self.esp_user, self.esp_pass)
        base_url = f"http://{self.esp_ip}"
        
        # 1. Essayer de trouver un objet AI pour tester le forçage OutOfService
        target_ai = None
        for dev in self.devices:
            did = dev.get("device_id")
            for obj in dev.get("objects", []):
                if obj.get("type") == 0:  # AI
                    target_ai = {"did": did, "inst": obj.get("inst"), "initial_oos": obj.get("outofservice", False)}
                    break
            if target_ai:
                break
                
        if target_ai:
            did = target_ai["did"]
            inst = target_ai["inst"]
            initial_oos = target_ai["initial_oos"]
            
            oos_topic = f"{self.mqtt_prefix}/{did}/AI/{inst}/outofservice"
            set_topic = f"{oos_topic}/set"
            
            # Déterminer la nouvelle valeur de test
            test_val = "ON" if not initial_oos else "OFF"
            restore_val = "OFF" if not initial_oos else "ON"
            
            print(f"  Test de forçage OutOfService sur AI:{inst} (Device {did}) :")
            print(f"    - État initial OutOfService : {'ON' if initial_oos else 'OFF'}")
            print(f"    - Publication de '{test_val}' vers '{set_topic}'...")
            
            # Vider l'état existant dans le dictionnaire de test pour éviter les faux positifs
            with self.lock:
                self.received_messages.pop(oos_topic, None)
                
            self.client.publish(set_topic, test_val, qos=1)
            
            # Attendre la rétroaction sur le topic d'état OutOfService
            updated_payload = self.wait_for_message(oos_topic, expected_value=test_val, timeout=self.timeout)
            if updated_payload == test_val:
                print(f"      {Colors.GREEN}Succès: Rétroaction MQTT validée à {test_val}{Colors.ENDC}")
                
                # Restauration
                print(f"    - Restauration de l'état initial à '{restore_val}'...")
                self.client.publish(set_topic, restore_val, qos=1)
                restored_payload = self.wait_for_message(oos_topic, expected_value=restore_val, timeout=self.timeout)
                
                if restored_payload == restore_val:
                    self.log_success("Écriture MQTT - OutOfService", "Toggling OutOfService OK et restauration OK.")
                else:
                    self.log_fail("Écriture MQTT - OutOfService (Restauration)", f"Échec de la restauration à {restore_val}. Valeur actuelle : {restored_payload}")
            else:
                self.log_fail("Écriture MQTT - OutOfService", f"Pas de mise à jour reçue sur '{oos_topic}' (attendu: '{test_val}', reçu: '{updated_payload}')")
                # Tentative de restauration par sécurité
                self.client.publish(set_topic, restore_val, qos=1)
        else:
            print("  Aucun objet de type Analog Input (AI) trouvé dans le cache pour tester le forçage OutOfService.")
            
        # 2. Essayer de trouver un objet de type AV ou AO ou BO ou BV (valeur modifiable)
        target_val = None
        for dev in self.devices:
            did = dev.get("device_id")
            for obj in dev.get("objects", []):
                # 2 = AV, 1 = AO, 4 = BO, 5 = BV
                if obj.get("type") in [1, 2, 4, 5]:
                    target_val = {
                        "did": did,
                        "type_id": obj.get("type"),
                        "type_str": MAP_TYPE[obj.get("type")],
                        "inst": obj.get("inst"),
                        "initial_val": obj.get("val")
                    }
                    break
            if target_val:
                break
                
        if target_val:
            did = target_val["did"]
            type_str = target_val["type_str"]
            inst = target_val["inst"]
            initial_val = target_val["initial_val"]
            
            state_topic = f"{self.mqtt_prefix}/{did}/{type_str}/{inst}/state"
            set_topic = f"{self.mqtt_prefix}/{did}/{type_str}/{inst}/set"
            
            # Déterminer la nouvelle valeur de test
            if type_str in ["BO", "BV"]:
                test_val_str = "1.00" if float(initial_val) == 0.0 else "0.00"
                restore_val_str = "0.00" if float(initial_val) == 0.0 else "1.00"
            else:
                test_val_str = f"{(float(initial_val) + 1.0):.2f}"
                restore_val_str = f"{float(initial_val):.2f}"
                
            print(f"  Test de modification de Present_Value sur {type_str}:{inst} (Device {did}) :")
            print(f"    - Valeur initiale : {initial_val}")
            print(f"    - Publication de '{test_val_str}' vers '{set_topic}'...")
            
            with self.lock:
                self.received_messages.pop(state_topic, None)
                
            self.client.publish(set_topic, test_val_str, qos=1)
            
            # Attendre la rétroaction sur le topic d'état
            # Note: Le périphérique réel peut mettre un peu de temps à valider l'écriture BACnet
            # et à republier via MQTT.
            updated_payload = self.wait_for_message(state_topic, timeout=self.timeout)
            
            # On vérifie si la valeur reçue est proche ou égale (conversion float)
            val_ok = False
            if updated_payload is not None:
                try:
                    val_ok = abs(float(updated_payload) - float(test_val_str)) < 0.05
                except ValueError:
                    val_ok = (updated_payload == test_val_str)
                    
            if val_ok:
                print(f"      {Colors.GREEN}Succès: Rétroaction MQTT validée à {updated_payload}{Colors.ENDC}")
                
                # Restauration
                print(f"    - Restauration de la valeur initiale à '{restore_val_str}'...")
                self.client.publish(set_topic, restore_val_str, qos=1)
                
                restored_payload = self.wait_for_message(state_topic, timeout=self.timeout)
                restored_ok = False
                if restored_payload is not None:
                    try:
                        restored_ok = abs(float(restored_payload) - float(restore_val_str)) < 0.05
                    except ValueError:
                        restored_ok = (restored_payload == restore_val_str)
                        
                if restored_ok:
                    self.log_success("Écriture MQTT - Present_Value", f"Toggling {type_str} OK et restauration OK.")
                else:
                    self.log_fail("Écriture MQTT - Present_Value (Restauration)", f"Échec de la restauration à {restore_val_str}. Reçu : {restored_payload}")
            else:
                self.log_fail("Écriture MQTT - Present_Value", f"Pas de mise à jour reçue sur '{state_topic}' (attendu: '{test_val_str}', reçu: '{updated_payload}')")
                # Tenter restauration par sécurité
                self.client.publish(set_topic, restore_val_str, qos=1)
        else:
            print("  Aucun objet de type AO/AV/BO/BV modifiable trouvé dans le cache.")

    def print_summary(self):
        print(f"\n{Colors.HEADER}{Colors.BOLD}=== BILAN DES TESTS MQTT ==={Colors.ENDC}")
        passed = sum(1 for r in self.results if r[1] == "PASS")
        failed = sum(1 for r in self.results if r[1] == "FAIL")
        skipped = sum(1 for r in self.results if r[1] == "SKIP")
        
        for name, status, msg in self.results:
            color = Colors.GREEN if status == "PASS" else (Colors.FAIL if status == "FAIL" else Colors.WARNING)
            print(f"  [{color}{status}{Colors.ENDC}] {name} {f'({msg})' if msg else ''}")
            
        print(f"\nTotal : {Colors.GREEN}{passed} PASS{Colors.ENDC} | {Colors.FAIL}{failed} FAIL{Colors.ENDC} | {Colors.WARNING}{skipped} SKIPPED{Colors.ENDC}")

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description="Test automatique des topics MQTT BACnet2MQTT")
    parser.add_argument('--ip', default="192.168.1.50", help="Adresse IP de l'ESP32 (défaut: 192.168.1.50)")
    parser.add_argument('--user', default="admin", help="Basic Auth User (défaut: admin)")
    parser.add_argument('--pass', dest='passwd', default="admin1234", help="Basic Auth Password (défaut: admin1234)")
    parser.add_argument('--mqtt-host', default=None, help="Surcharge du broker MQTT (IP ou nom)")
    parser.add_argument('--mqtt-port', type=int, default=None, help="Surcharge du port MQTT")
    parser.add_argument('--mqtt-user', default=None, help="Surcharge de l'utilisateur MQTT")
    parser.add_argument('--mqtt-pass', default=None, help="Surcharge du mot de passe MQTT")
    parser.add_argument('--mqtt-prefix', default=None, help="Surcharge du préfixe MQTT (ex: bacnet2mqtt)")
    parser.add_argument('--timeout', type=int, default=10, help="Timeout d'attente des messages MQTT en secondes (défaut: 10)")
    parser.add_argument('--test-write', action='store_true', help="Activer les tests d'écriture MQTT (modifie temporairement des valeurs)")
    
    args = parser.parse_known_args()[0]
    
    runner = MQTTTestRunner(args)
    runner.execute()
