- **Prochaine Étape** : Test de charge avec plusieurs automates physiques et validation de la persistence NVS sur le long terme.

## État au 28 Mai 2026 (Refonte complète des Settings - v5.7.12) - DÉPLOYÉ
- **Version actuelle** : v5.7.12
- **Succès Technologiques** :
    - **Refonte UI Settings** : Organisation granulaire des paramètres en 5 sections (Network, MQTT, BACnet, Polling, Security).
    - **Persistance Admin** : Les identifiants d'accès à l'interface (User/Pass) sont désormais persistés dans le NVS et configurables via l'onglet Security.
    - **Dashboard Enrichi** : Affichage de la Gateway et du Mask dans la carte Network du Dashboard pour un diagnostic rapide.
    - **Corrections Ergonomiques** : Suppression de la colonne `BACnet Idx` (jugée encombrante sur mobile) et renommage explicite de l'action de vidage de cache (`Clear BACNET Cache`).
    - **MQTT User Fix** : Correction de la remontée du nom d'utilisateur MQTT dans l'interface de configuration.

## État au 28 Mai 2026 (Refonte UI & Logs Dual-Core - v5.7.7) - DÉPLOYÉ
- **Version actuelle** : v5.7.7
- **Succès Technologiques** :
    - **Logs Dual-Core (Architecture Segmentée)** : Refonte de la fonction `z_log` pour préfixer chaque message par l'ID du cœur (`0|` pour Core 0 / `1|` pour Core 1). Cette innovation permet un diagnostic séparé et en temps réel des couches système (WiFi/MQTT) et protocolaire (BACnet FSM).
    - **API Status Enrichie** : Extension de `/api/status` pour fournir des métadonnées réseau complètes (Mask, GW), statistiques MS/TP (RX/TX/Tokens) et l'état d'avancement de la découverte par automate (Progress Bar data).
    - **Refonte UI Mobile-Friendly (v5.7.7)** : Passage à une interface ultra-compacte optimisée pour iPhone. Utilisation d'accordéons (`<details>`) pour les réglages et d'un tableau compressé pour les objets BACnet.
    - **Dashboard Temps Réel** : Visualisation immédiate de la santé du réseau (Heap, RSSI, MS/TP Traffic) et barre de progression dynamique de la découverte par automate.
    - **Gestion Unitaire des Objets** : Ajout d'endpoints API dédiés (`/api/save_object`, `/api/reload_object`, `/api/reload_device`, `/api/delete_device`) permettant des actions granulaires sans perturber le reste du réseau.
    - **Validation OTA** : Compilation et Flash réussis. Système opérationnel avec monitoring dual-core actif.

- **Prochaine Étape** : Compilation finale et Flash OTA pour validation sur site.

## État au 28 Mai 2026 (Mise à jour Majeure - v5.7.2)
- **Version actuelle** : v5.7.2 (Robustness & Memory Safety)
- **Succès Technologiques** :
    - **Anti-Fragmentation (Zero-String Strategy)** : Suppression massive de la classe `String` dans les structures d'objets au profit de `char[]`. Cette approche élimine les risques de fragmentation du tas lors du traitement de centaines d'objets BACnet.
    - **Gestion par Blocs (Pagination NVS/RAM)** : Implémentation d'une logique de traitement et de sauvegarde par blocs de 20 objets (`BACnetPersistencePage`). Cette segmentation garantit la stabilité lors de l'écriture en NVS (respect de la limite de 1984 octets) et fluidifie l'allocation mémoire.
    - **Self-Healing NVS** : Routine de démarrage avec détection de corruption et auto-formatage de la partition NVS pour garantir un boot sécurisé.
    - **Circuit Breaker MQTT** : Disjoncteur logiciel protégeant le Core 0. Suspension des tentatives après 3 échecs pour éviter la saturation LwIP.
    - **Support Multi-State & Unités** : Décodage des `State_Texts` et traduction automatique des unités ASHRAE 135 (°C, Pa, kW...) sur les topics MQTT.
    - **Pilotage Bidirectionnel** : Support des écritures MQTT (topics `/set`) pour les valeurs et les noms.
    - **UI v3.9 (UX Refresh)** : Onglets persistants (Sticky Tabs), Echo Radar Wi-Fi et édition en direct du polling/naming.
- **Prochaine Étape** : Finaliser l'export EDE avec métadonnées et stabiliser la lecture itérative des textes d'états pour les équipements très lents.

## Historique des Incidents Résolus
- [v5.7.2] NVS Header Mismatch -> Correction via la nouvelle structure `BACnetPersistenceDev` alignée.
- [v5.7.2] MQTT Storm on Boot -> Publication des noms déléguée au Core 0 avec pacing.
- [v5.7.1] MSV Null States -> Initialisation sécurisée du vecteur `state_texts` en cas de lecture échouée.

## État au 27 Mai 2026 (Fin de journée - v5.6.8)
- **Version actuelle** : v5.6.8 (Smart Sync & MQTT Pro)
- **Succès Technologiques** : 
    - **Naming & Discovery Robustes** : Les objets sont découverts avec polling **OFF** par défaut. Les noms (`Object_Name`) sont publiés sur MQTT (`.../name`) à la découverte, au reboot (via cache NVS) et lors de chaque modification manuelle.
    - **MQTT Dynamic Polling** : Ajout de l'option `poll_interval` (mpi) configurable dans l'UI pour cadencer la diffusion MQTT indépendamment du polling BACnet.
    - **Simplification des Topics** : Passage aux abréviations normatives (`AI`, `AO`, `AV`, `BI`, `BO`, `BV`, `MSI`, `MSO`, `MSV`) pour faciliter l'intégration YAML.
    - **Gateway Status (B2M)** : Publication périodique (60s) de l'état de santé du Waveshare (`heap`, `rssi`, `mstp_cnt`, `nb_dev`).
    - **Smart Save & Stability** : Reboot uniquement pour les changements Wi-Fi. Hot-reload pour MQTT et BACnet. Gestion des gros payloads JSON via bufferisation de corps HTTP asynchrone.
- **Prochaine Étape** : Finaliser la documentation utilisateur pour les nouveaux topics simplifiés.

## Historique des Incidents Résolus
- [v5.6.8] JSON Fragmented POST -> Implémentation du body buffering pour les équipements à grand nombre d'objets.
- [v5.6.8] Unnecessary Reboots -> Distinction des formulaires de sauvegarde pour maintenir l'uptime.

## État au 27 Mai 2026 (Après-midi - v5.6.7)
- **Version actuelle** : v5.6.7 (Iterative Discovery)
- **Succès Technologiques** : 
    - **FSM ASHRAE Strict** : Restauration de la FSM à 9 états (base commit 3713ac1). Ring stable avec ECB_203 (MAC 4).
    - **Découverte Prop 110 (State_Text)** : Implémentation d'une lecture itérative (Index 0 puis 1..N) conforme à l'ASHRAE 135. Récupération réussie des libellés (ex: "Confort", "Eco") pour les objets Multi-State (MSV).
    - **Observabilité MQTT** : Logs de debug ajoutés pour chaque publication (Topic + Valeur), permettant un suivi en temps réel de la diffusion.
- **Prochaine Étape** : Intégrer les textes d'états dans les publications MQTT (Optionnel) et finaliser l'export EDE avec les métadonnées.

## Historique des Incidents Résolus
- [v5.6.7] RX Deafness -> Reboot de l'automate et restauration de la FSM stricte ont rétabli la communication.
- [v5.6.7] Iterative Prop 110 -> Lecture par index prévient les AbortPDU sur MS/TP à 38400 bauds.

## État au 26 Mai 2026 (Nuit - Stabilisation v5.6)
- **Version actuelle** : v5.6 (Official Expert Remediation)
- **Environnement** : Core 1 (BACnet FSM Precision), Core 0 (WiFi/MQTT/UI Deferred)
- **Succès Technologiques** : 
    - **Délégation Asynchrone des Logs** : Implémentation d'une `log_queue` (FreeRTOS) pour les WebSockets. Le Core 1 n'appelle plus de fonctions réseau bloquantes, garantissant le respect du `Tusage_delay`.
    - **Timing MS/TP Normatif** : Intégration du `Tturnaround` (1050us) et suppression des `vTaskDelay(1)` aléatoires au profit d'un yield dynamique. Le bus est d'une stabilité absolue (>1000 jetons sans régénération).
    - **Découverte Dynamique NPCI** : Refonte du parseur NPDU gérant les offsets DNET/SNET/HopCount. Capture réussie des messages `Who-Is` / `I-Am`, permettant la découverte automatique de l'ECB-203 sans scan manuel des MAC.
    - **Circuit Breaker MQTT (Best Practices)** : Désactivation de l'auto-reconnect du driver au profit d'une gestion applicative différée (`esp_mqtt_client_stop` & `esp_mqtt_client_destroy` sur le Core 0). Neutralisation réelle des boucles infinies de reconnexion en cas de broker hors-ligne.
    - **Considération de l'UI** : Toutes les modifications respectent les variables de configuration ajoutées dans le menu SETTINGS de l'interface Web.

## Historique des Incidents Résolus
- [v5.6] Discovery Failure -> Parser NPCI à offset fixe ignorait les I-Am routés.
- [v5.6] Token Regeneration -> Blocage synchrone du Core 1 par les appels WebSockets.
- [v5.6] MQTT Storm -> Boucle de reconnexion infinie saturant LwIP.
- [v4.7.45] Unit Persistence Loss -> Champ `units` manquant dans les structures NVS.
