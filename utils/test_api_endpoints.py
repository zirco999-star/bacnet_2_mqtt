import unittest
import requests
import argparse
import sys

# Variables globales configurées par arguments
ESP_IP = "192.168.1.50"
ESP_USER = "admin"
ESP_PASS = "admin1234"
DANGEROUS = False

class TestBACnet2MQTT_API(unittest.TestCase):
    @property
    def base_url(self):
        return f"http://{ESP_IP}"

    @property
    def auth(self):
        return (ESP_USER, ESP_PASS)

    def test_01_auth_missing(self):
        """Test rejet si pas d'authentification"""
        r = requests.get(f"{self.base_url}/api/status", timeout=5)
        self.assertEqual(r.status_code, 401)

    def test_02_auth_invalid(self):
        """Test rejet si mot de passe erroné"""
        r = requests.get(f"{self.base_url}/api/status", auth=(ESP_USER, "wrongpassword"), timeout=5)
        self.assertEqual(r.status_code, 401)

    def test_03_get_status(self):
        """Test de la route GET /api/status"""
        r = requests.get(f"{self.base_url}/api/status", auth=self.auth, timeout=5)
        self.assertEqual(r.status_code, 200)
        data = r.json()
        self.assertIn("uptime", data)
        self.assertIn("heap", data)
        self.assertIn("devices", data)

    def test_04_get_objects(self):
        """Test de la route GET /api/objects"""
        r = requests.get(f"{self.base_url}/api/objects", auth=self.auth, timeout=5)
        self.assertEqual(r.status_code, 200)
        data = r.json()
        self.assertIsInstance(data, list)

    def test_05_get_trigger_discovery(self):
        """Test de la route GET /api/trigger_discovery"""
        r = requests.get(f"{self.base_url}/api/trigger_discovery", auth=self.auth, timeout=5)
        self.assertEqual(r.status_code, 200)

    def test_06_post_whois(self):
        """Test de la route POST /api/whois"""
        r = requests.post(f"{self.base_url}/api/whois", auth=self.auth, timeout=5)
        self.assertEqual(r.status_code, 200)

    def test_07_post_iam(self):
        """Test de la route POST /api/iam"""
        r = requests.post(f"{self.base_url}/api/iam", auth=self.auth, timeout=5)
        self.assertEqual(r.status_code, 200)

    def test_08_object_specific_routes(self):
        """Test des routes spécifiques à un objet réel (ReadProperty, WriteValue, OutOfService, Save/Reload)"""
        # Récupération d'un objet réel du cache
        r = requests.get(f"{self.base_url}/api/objects", auth=self.auth, timeout=5)
        self.assertEqual(r.status_code, 200)
        devices = r.json()
        
        if not devices:
            self.skipTest("Aucun périphérique en cache pour tester les actions spécifiques aux objets.")
            
        target_dev = None
        for d in devices:
            if d.get("objects"):
                target_dev = d
                break
                
        if not target_dev:
            self.skipTest("Aucun objet trouvé dans les périphériques en cache.")
            
        did = target_dev.get("device_id")
        objects = target_dev.get("objects")
        
        # Trouver un objet (priorité à une AI ou une AV)
        target_obj = None
        for o in objects:
            if o.get("type") in [0, 2]: # AI=0, AV=2
                target_obj = o
                break
        if not target_obj:
            target_obj = objects[0]
            
        inst = target_obj.get("inst")
        obj_type = target_obj.get("type")
        
        print(f"\n   [Info] Objet de test sélectionné : Type {obj_type}, Inst {inst} sur Device {did}")
        
        # 1. Test /api/readproperty (Present_Value = 85)
        payload_read = {"did": did, "type": obj_type, "inst": inst, "prop": 85}
        r_read = requests.post(f"{self.base_url}/api/readproperty", auth=self.auth, data=payload_read, timeout=5)
        self.assertEqual(r_read.status_code, 200)
        self.assertEqual(r_read.text, "READ_PROPERTY ENQUEUED")
        
        # 2. Test /api/outofservice (si c'est une AI)
        if obj_type == 0:
            original_oos = target_obj.get("outofservice", False)
            # Toggle OOS
            payload_oos = {"did": did, "inst": inst, "type": obj_type, "state": "1" if not original_oos else "0"}
            r_oos = requests.post(f"{self.base_url}/api/outofservice", auth=self.auth, data=payload_oos, timeout=5)
            self.assertEqual(r_oos.status_code, 200)
            self.assertEqual(r_oos.text, "OUT_OF_SERVICE ENQUEUED")
            
            # Restaurer OOS
            payload_restore = {"did": did, "inst": inst, "type": obj_type, "state": "1" if original_oos else "0"}
            requests.post(f"{self.base_url}/api/outofservice", auth=self.auth, data=payload_restore, timeout=5)

        # 3. Test /api/writevalue (Present_Value avec priorité)
        payload_write = {"did": did, "type": obj_type, "inst": inst, "prop": 85, "val": 22.5, "priority": 8}
        r_write = requests.post(f"{self.base_url}/api/writevalue", auth=self.auth, data=payload_write, timeout=5)
        self.assertEqual(r_write.status_code, 200)
        self.assertEqual(r_write.text, "WRITE_VALUE ENQUEUED")
        
        # 4. Test /api/reload_object
        payload_reload = {"did": did, "type": obj_type, "inst": inst}
        r_reload = requests.post(f"{self.base_url}/api/reload_object", auth=self.auth, data=payload_reload, timeout=5)
        self.assertEqual(r_reload.status_code, 200)
        self.assertEqual(r_reload.text, "READ ENQUEUED")
        
        # 5. Test /api/save_object
        original_poll = target_obj.get("poll", True)
        payload_save = {
            "did": did,
            "inst": inst,
            "type": obj_type,
            "poll": "1" if original_poll else "0",
            "step": str(target_obj.get("step", 1.0))
        }
        r_save = requests.post(f"{self.base_url}/api/save_object", auth=self.auth, data=payload_save, timeout=5)
        self.assertEqual(r_save.status_code, 200)
        self.assertEqual(r_save.text, "OK")

    def test_09_post_reload_device(self):
        """Test de la route POST /api/reload_device"""
        r = requests.get(f"{self.base_url}/api/objects", auth=self.auth, timeout=5)
        self.assertEqual(r.status_code, 200)
        devices = r.json()
        if not devices:
            self.skipTest("Aucun périphérique en cache.")
            
        did = devices[0].get("device_id")
        r_reload = requests.post(f"{self.base_url}/api/reload_device", auth=self.auth, data={"id": did}, timeout=5)
        self.assertEqual(r_reload.status_code, 200)
        self.assertEqual(r_reload.text, "RELOADING")

    def test_10_post_toggle_device(self):
        """Test de la route POST /api/toggle_device"""
        r = requests.get(f"{self.base_url}/api/objects", auth=self.auth, timeout=5)
        self.assertEqual(r.status_code, 200)
        devices = r.json()
        if not devices:
            self.skipTest("Aucun périphérique en cache.")
            
        did = devices[0].get("device_id")
        # Toggle (désactive/réactive)
        r_toggle = requests.post(f"{self.base_url}/api/toggle_device", auth=self.auth, data={"id": did}, timeout=5)
        self.assertEqual(r_toggle.status_code, 200)
        self.assertEqual(r_toggle.text, "OK")
        
        # Restaure l'état d'origine
        requests.post(f"{self.base_url}/api/toggle_device", auth=self.auth, data={"id": did}, timeout=5)

    def test_11_dangerous_routes(self):
        """Test des routes de réinitialisation/reboot (uniquement si --dangerous est actif)"""
        if not DANGEROUS:
            self.skipTest("Tests dangereux désactivés (utilisez --dangerous pour les activer)")
            
        # Test /api/reset_cache
        r_reset = requests.post(f"{self.base_url}/api/reset_cache", auth=self.auth, timeout=5)
        self.assertEqual(r_reset.status_code, 200)
        
        # Test /api/reboot
        r_reboot = requests.post(f"{self.base_url}/api/reboot", auth=self.auth, timeout=5)
        self.assertEqual(r_reboot.status_code, 200)

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--ip', default="192.168.1.50", help="Adresse IP de l'ESP32")
    parser.add_argument('--user', default="admin", help="Basic Auth User")
    parser.add_argument('--pass', dest='passwd', default="admin1234", help="Basic Auth Password")
    parser.add_argument('--dangerous', action='store_true', help="Autoriser les tests de reset et reboot")
    
    # Extraire uniquement nos arguments personnalisés pour ne pas polluer unittest
    args, unknown = parser.parse_known_args()
    
    ESP_IP = args.ip
    ESP_USER = args.user
    ESP_PASS = args.passwd
    DANGEROUS = args.dangerous
    
    # Nettoyer sys.argv pour unittest
    sys.argv = [sys.argv[0]] + unknown
    
    unittest.main()
