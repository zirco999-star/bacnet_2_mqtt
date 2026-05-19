# bacnet_2_mqtt

## Architecture
- **Framework** : Arduino IDE.
- **Hardware Target** : Waveshare ESP32-S3-R8 (RS485-CAN).
- **Core Strategy** :
  - **CORE 1 (APP_CPU)** : FSM BACnet MS/TP temps réel (priorité 9) - Squelette (Token/PFM) opérationnel.
  - **CORE 0 (PRO_CPU)** : WiFi, Web Server (Tailwind), MQTT, OTA, TCP Bridge.
- **Frontend** : SPA avec Tailwind CSS (CDN).
- **Security** : Basic Auth (NVS) + RT Logs via circular buffer/WebSockets.

## Environnement technique
- **Compilateur** : Arduino IDE (nécessaire pour la bonne gestion de la PSRAM en mode OPI avec cette carte).
- **Outils activés** : filesystem, shell.

## Standards de Développement
- C++17.
- Logique thread-safe via Mutex (SemaphoreHandle_t).
- Compilation : Manuelle via Arduino IDE par l'utilisateur sur Windows (dossier partagé).

## Directives de Déploiement et d'Édition (LXC -> Windows)
- Le projet est localisé dans `/home/dev/bacnet_2_mqtt` (qui est un symlink vers `/mnt/save/bacnet_2_mqtt`).
- **ATTENTION :** En raison du lien symbolique externe, les outils natifs de fichiers (write_file, replace, list_directory) sont inopérants directement sur la cible. 
- **Méthode d'édition :** L'agent doit écrire le fichier dans un dossier temporaire (ex: `/home/dev/tmp.ino`), puis utiliser `run_shell_command` avec `cp` ou `mv` pour le transférer dans `/home/dev/bacnet_2_mqtt/`. De même pour la lecture, `cp` vers le workspace puis lecture standard.

## 🛠️ Directives de Développement ESP32 (Rigueur 2026)
1. **Isolation par Briques** : Chaque module (WiFi, BACnet, MQTT, UI) doit résider dans son propre fichier .cpp/.h.
2. **Sanctuarisation Réseau** : Ne jamais modifier 'z_network.cpp' ou 'z_config.h' (section WiFi/OTA) lors des travaux sur les couches applicatives, sauf instruction explicite.
3. **Protocole de Flash** : Prioriser systématiquement l'OTA via 'espota.py'. Le flash USB est un dernier recours.
4. **Persistance NVS** : Utiliser des clés individuelles explicites pour chaque paramètre pour éviter les ruptures de compatibilité.
5. **UI Industrielle** : Toujours pré-remplir les champs avec les valeurs par défaut 'sysCfg' dans l'interface.

## ⚡ Flash Autonome (Method v2026)
Pour flasher le binaire sans intervention manuelle :
```bash
curl -u admin:admin1234 -F "file=@/home/dev/bacnet_2_mqtt/build/bacnet_2_mqtt.ino.bin" http://192.168.1.50/update
```

## 🚀 Workflow Autonome Complet
### 1. Compilation
```bash
export PATH=/home/dev/venv/bin:$PATH && /home/dev/bin/arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashMode=qio,FlashSize=8M,PartitionScheme=min_spiffs,PSRAM=opi /home/dev/bacnet_2_mqtt/bacnet_2_mqtt.ino --output-dir /home/dev/bacnet_2_mqtt/build/
```
### 2. Flash OTA
```bash
curl -u admin:admin1234 -F "file=@/home/dev/bacnet_2_mqtt/build/bacnet_2_mqtt.ino.bin" http://192.168.1.50/update
```
