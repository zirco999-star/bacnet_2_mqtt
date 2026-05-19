# Suivi Technique et Directives : BACnet2MQTT (v2026)

## Architecture Materielle et Logicielle
- Materiel : Waveshare ESP32-S3-RS485-CAN (8MB Flash, 8MB OPI PSRAM).
- Pins Critiques : TX=17, RX=18, RTS/DE=21.
- Alerte Securite PSRAM : Le GPIO 47 est utilise par le bus Octal SPI de la PSRAM. Ne JAMAIS le configurer en entree ou sortie (risque de crash bus memoire).
- Core Strategy : Core 1 dedie a la FSM BACnet MS/TP temps reel. Core 0 pour WiFi, Web, MQTT et OTA.

## Workflow de Developpement (Strict)

### 1. Edition des Sources
Le repertoire /home/dev/bacnet_2_mqtt etant un lien symbolique vers /mnt/save, l'agent doit utiliser la methode de copie :
1. Editer le fichier dans le workspace local (ex: /home/dev/z_bacnet_edit.cpp).
2. Copier vers la cible : cp /home/dev/z_bacnet_edit.cpp /home/dev/bacnet_2_mqtt/z_bacnet.cpp.

### 2. Compilation et Deploiement OTA
Utiliser systematiquement l'environnement virtuel et la chaine de commande unique pour garantir la coherence :
- Commande : export PATH=/home/dev/venv/bin:$PATH && arduino-cli compile ... && python3 espota.py ... && python3 listen_logs_v2.py.

### 3. Suivi du Projet (TRACE.md)
Chaque session ou etape majeure validee (ex: Ring stable, Discovery reussie) doit faire l'objet d'une mise a jour du fichier TRACE.md a la racine du projet.
- Format : [Date/Heure] Phase X.Y : Titre - Actions - Resultat.
- Note : Conserver l'historique des versions pour permettre les retours arriere.

### 4. Gestion Git
- Commiter chaque version de reference (ex: v4.2.14 pour le Ring stable).
- Message de commit explicite : vX.Y.Z: Description technique courte.

## Specificites BACnet MS/TP (Lecons apprises)
- Timing YABE : L'automate ECB-203 est lent (~240ms). Toujours maintenir une attente non-bloquante de 280ms apres une requete Data-Expecting-Reply.
- Auto-RTS : Utiliser UART_MODE_RS485_HALF_DUPLEX natif sur le port UART_NUM_1.
- Polling : Ne pas depasser 1 requete tous les 20 jetons pour garantir la fluidite du bus.
