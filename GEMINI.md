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
