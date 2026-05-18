# ZIRCON1UM - Passerelle BACnet MS/TP vers MQTT (ESP32-S3)

## Description
Ce projet est une passerelle souveraine haute performance permettant de transcoder le trafic **BACnet MS/TP** (RS485) vers des topics **MQTT** via WiFi. Il est optimisé pour la carte **Waveshare ESP32-S3-R8** avec gestion de la PSRAM en mode OPI.

## Architecture Core
Le micrologiciel exploite l'architecture asymétrique de l'ESP32-S3 :
- **CORE 1 (APP_CPU)** : Dédié exclusivement à la machine à états (FSM) MS/TP en temps réel. Priorité maximale pour garantir les timings critiques du bus (T_usage_timeout, CRC).
- **CORE 0 (PRO_CPU)** : Gère les services réseau (WiFi, WebServer, MQTT, OTA) et le monitoring système.

## Fonctionnalités
- [x] **WiFi STA/AP** : Initialisation robuste avec gestion des timeouts (optimisé Freebox).
- [x] **Admin Portal** : Interface SPA moderne (Tailwind CSS) intégrée en PROGMEM.
- [x] **Real-time Logs** : Flux de diagnostic via WebSockets.
- [x] **Update OTA** : Mise à jour sans fil activée.
- [ ] **BACnet MS/TP** : Capture des trames de données et validation CRC16 (Phase 1).
- [ ] **Gateway MQTT** : Mapping automatique des objets BACnet en JSON.

## Environnement de Développement
- **Framework** : Arduino (via Arduino IDE pour la gestion spécifique de la PSRAM OPI).
- **Cœur** : ESP32 Arduino Core v3.0.0.
- **Outils** : `arduino-cli` (compilation autonome), Git (versioning).

## Configuration Réseau
Par défaut, si aucun SSID n'est configuré, l'appareil crée un point d'accès :
- **SSID** : `ZIRCON1UM_SETUP`
- **Pass** : `admin1234`
- **IP Portal** : `192.168.4.1`

## Structure des Fichiers
- `bacnet_2_mqtt.ino` : Code principal et logique Core 0.
- `index_html.h` : Interface utilisateur déportée (protection pré-processeur).
- `PLAN.md` : Feuille de route du développement.
- `TRACE.md` : Historique des tâches et modifications.

## Installation
1. Configurer l'Arduino IDE pour la carte **ESP32S3 Dev Module**.
2. **Réglages critiques** :
   - Flash Mode : **OPI 80MHz**
   - PSRAM : **OPI PSRAM**
   - Partition Scheme : **16M Flash (3MB APP / 9MB FATFS)**
3. Installer les bibliothèques `AsyncTCP`, `ESPAsyncWebServer`, `ArduinoJson`, `PubSubClient`.

---
*Développé avec Gemini CLI - Mode YOLO*
