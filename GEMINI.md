# Suivi Technique et Directives : BACnet2MQTT (v2026)

## Architecture du Projet
- `/bacnet_2_mqtt.ino` : Point d'entrée principal (doit rester à la racine).
- `src/` : Fichiers sources `.cpp` et `.h`.
- `utils/` : Scripts de compilation (`compil.sh`) et de flash (`flashOTA.sh`).
- `plans/` : Documents de conception et plans de remédiation.
- `extract/` : Logs, extractions et exports de données.

## Architecture Materielle et Logicielle
- Materiel : Waveshare ESP32-S3-RS485-CAN (8MB Flash, 8MB OPI PSRAM).
- Pins Critiques : TX=17, RX=18, RTS/DE=21.
- Alerte Securite PSRAM : Le GPIO 47 est utilise par le bus Octal SPI de la PSRAM. Ne JAMAIS le configurer en entree ou sortie (risque de crash bus memoire).
- Core Strategy : Core 1 dedie a la FSM BACnet MS/TP temps reel. Core 0 pour WiFi, Web, MQTT et OTA.

## Expertise Protocolaire BACnet (Obligatoire)
- Expert Referent : NotebookLM fe92515b-ad88-4bfd-af35-89b9349d1f11.
- Instruction : Cet expert contient la documentation officielle et les captures de trafic de reference. Il DOIT etre systematiquement sollicite avant tout codage de logique MS/TP (Parsing ASN.1, FSM, Timings, Segmentation) pour garantir le respect des bonnes pratiques et de la norme ASHRAE 135.

## Workflow de Developpement (Strict)

### 1. Edition des Sources
Le repertoire /home/dev/bacnet_2_mqtt etant un lien symbolique vers /mnt/save, l'agent doit utiliser la methode de copie :
1. Editer le fichier dans le workspace local (ex: /home/dev/z_bacnet_edit.cpp).
2. Copier vers la cible : cp /home/dev/z_bacnet_edit.cpp /home/dev/bacnet_2_mqtt/z_bacnet.cpp.
3. **Respect des Normes** : Toute modification doit imperativement suivre les regles definies dans `CONVENTION_CODAGE.md` (Prefixes de types, macros preferees, gestion memoire Heap_4). Une verification systematique est exigee avant chaque commit. L'utilisation des préfixes compositionnels (uc, us, ul, x, p) est obligatoire pour toutes les nouvelles variables et la refactorisation du code existant.

### 2. Compilation et Deploiement OTA
Utiliser impérativement les scripts du dossier `utils` :
- Compilation : `./utils/compil.sh`
- Flash OTA : `./utils/flashOTA.sh [IP] [PORT]` (Défaut: 192.168.1.50:3232)

### 3. Suivi du Projet (TRACE.md)
Chaque session ou etape majeure validee (ex: Ring stable, Discovery reussie) doit faire l'objet d'une mise a jour du fichier TRACE.md a la racine du projet.

### 4. Gestion Git
- Mettre à jour la version globale via `#define configVERSION_GLOBAL "vX.Y.Z"` dans `src/z_config.h` avant de compiler et de commiter.
- Commiter chaque version de reference (ex: v4.2.14 pour le Ring stable).
- Message de commit explicite : vX.Y.Z: Description technique courte.

## Specificites BACnet MS/TP (Lecons apprises)
- **Calcul ASHRAE Device ID** : Utiliser impérativement la formule `(did * 1000) + mac` pour l'affichage et l'identification (ex: DID=123, MAC=1 => 123001).
- **Deadlock UART (ESP-IDF v5)** : L'événement `UART_TX_DONE` est absent. Utiliser `uart_wait_tx_done(RS485_UART_PORT, 0) == ESP_OK` pour le polling matériel.
- **FSM Transition** : Après transmission, si `waiting_for_reply` est vrai, basculer impérativement en `MSTP_AWAIT_REPLY` (pas `IDLE`) pour respecter le `T_reply_delay` ASHRAE 135.
- **Timing YABE** : L'automate ECB-203 est lent (~240ms). Toujours maintenir une attente non-bloquante de 280ms apres une requete Data-Expecting-Reply.
- **Auto-RTS** : Utiliser UART_MODE_RS485_HALF_DUPLEX natif sur le port UART_NUM_1.
- **Polling** : Ne pas depasser 1 requete tous les 20 jetons pour garantir la fluidite du bus.

## Workflow de Developpement UI (Proxy)
Pour modifier l'interface sans recompiler le firmware :
1. **Édition** : Modifier `utils/dev_ui/index.html`.
2. **Test Live** : Lancer `python3 utils/dev_ui/2_server_proxy.py`. Accéder via `http://localhost:8000`.
   - Le proxy redirige les API vers le matériel réel (`192.168.1.50`) avec Auth auto.
   - Les WebSockets sont routés dynamiquement vers l'IP du matériel.
3. **Injection** : Une fois l'UI validée, lancer `python3 utils/dev_ui/3_inject_ui.py` pour mettre à jour `src/z_ui.h`.
4. **Finalisation** : Compiler et flasher une seule fois à la fin.
