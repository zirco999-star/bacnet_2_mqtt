# Journal de Suivi - BACnet2MQTT

## État au 1 Juin 2026 (Refonte Statistiques - v5.8.3) - DÉPLOYÉ
- **Version** : v5.8.3
- **Diagnostic Utile** : Séparation stricte entre signalisation du Ring et trafic de données.
    *   **Tokens** : Nombre de jetons reçus (Santé du Ring).
    *   **TX** : Désormais uniquement le nombre de **requêtes de données** envoyées (ReadProperty).
    *   **RX** : Désormais uniquement le nombre de **réponses de données** valides reçues (Complex-ACK).
- **Interprétation** : Permet de voir instantanément si le polling est efficace (TX ≈ RX) ou s'il y a des timeouts (TX > RX).

## État au 1 Juin 2026 (Correction Unités & HA - v5.8.2) - DÉPLOYÉ
- **Version** : v5.8.2
- **Standardisation Unités** : Alignement des codes d'unités JS (`z_ui.h`) sur le standard ASHRAE 135 et le firmware C++.
*   `62` : `%` -> `°C`
*   `98` : `ppm` -> `%`
*   `95` : `No Units` -> `no-units`
- **Priorité Utilisateur** : La découverte Home Assistant utilise désormais l'unité textuelle modifiée par l'utilisateur (`unit_text`) en priorité, permettant de corriger manuellement les points sans unités.
- **Nettoyage HA** : Les unités "no-units" ou "none" sont converties en chaîne vide pour un affichage propre dans Home Assistant.
- **Device Class** : Extension de la détection automatique de `device_class` dans HA (température, humidité, pression, puissance, énergie, tension, courant).

## État au 1 Juin 2026 (Optimisation HA Discovery - v5.8.1) - DÉPLOYÉ
- **Version** : v5.8.1
- **Filtrage Home Assistant** : Seuls les objets `enabled = true` sont désormais diffusés via MQTT Auto-Discovery.
- **Suppression Automatique** : La désactivation d'un objet dans l'UI envoie un payload vide sur le topic de config HA, provoquant la suppression immédiate de l'entité dans Home Assistant.
- **Discovery Ciblée** : Implémentation de `trigger_ha_discovery(did, inst, type)` permettant de mettre à jour un seul objet instantanément lors d'une modification utilisateur, sans scanner tout le cache.
- **Validation** : Intégration réussie dans `/api/save_object` et `/api/save_objects`.

## État au 29 Mai 2026 (Version de Clôture - v6.0.0) - TERMINÉ
- **Version** : v6.0.0 (PROJET DÉFINITIVEMENT FINALISÉ)
- **Découverte Parfaite** : Capture intégrale des 98 objets du Distech MAC 4 avec décodage CharacterString (UCS-2) fonctionnel.
- **Énumération Multi-State** : Restauration de l'étape `DISC_OBJ_STATES` avec énumération itérative (index par index) pour contourner les limitations du MAC 4.
- **Robustesse NVS & Reboot** : Sécurisation des écritures par blocs et correction du timer de reboot différé.
- **Observabilité** : Ajout de `mstp_err` et `debug_oid` pour un diagnostic matériel précis.
- **Statut Final** : Système ultra-stable, intégration MQTT/Home Assistant validée sur bus RS-485 réel.

## État au 29 Mai 2026 (Finalisation & Robustesse Discovery - v5.9.6) - DÉPLOYÉ
- **Version** : v5.9.6 (PROJET FINALISÉ)
- **Découverte Intégrale** : Succès de l'énumération des 98 objets du Distech MAC 4 (ECB-203). Filtrage robuste des slots vides (0xFFFFFFFF) et capture des noms réels (Object_Name).
- **Stabilité NVS** : Correction du débordement de buffer dans `save_device_objects_locked`. Le compteur d'objets est désormais dynamiquement aligné sur le nombre d'objets valides trouvés.
- **Nuclear Reset** : Implémentation de `/api/reset_cache` utilisant `nvs_flash_erase()` pour une remise à zéro garantie du système sans crash.
- **Optimisation Timings** : Passage du `apdu_timeout` à 1000ms et conformité NPCI `0x01 0x04` pour une interopérabilité maximale avec les automates lents.
- **Polling Précoce** : Activation du polling automatique pour les types Analog et Binary dès la détection de l'OID, assurant une publication MQTT immédiate après découverte.
- **Validation** : Système stable, sans fuite mémoire, opérant sur les deux cœurs de l'ESP32-S3.

## État au 29 Mai 2026 (Version de Production Stable - v5.8.1) - DÉPLOYÉ
- **Version** : v5.8.1 (Finale)
- **Polling Optimisé** : 
    - Le moteur MS/TP scanne désormais toute la liste des objets à chaque jeton pour servir immédiatement les points expirés.
    - Correction du deadlock FSM (token hogging) qui bloquait le ring en cas d'absence de données.
    - Intervalle de polling BACnet et MQTT respecté (testé avec 6 objets @ 15/20s).
- **Stabilité Mutex** : Toutes les routes API Web sont sécurisées et thread-safe.
- **Observabilité** : Logs normalisés horodatés. Mode INFO par défaut, DEBUG disponible pour analyse granulaire du polling.
- **Validation** : Confirmée par monitoring WebSocket et agent Home Assistant.

## État au 29 Mai 2026 (Visibilité Logs & Stabilité Mutex - v5.8.1) - DÉPLOYÉ
- **Version** : v5.8.1
- **Mutex** : Remédiation complète des deadlocks sur les routes API (`/api/objects`, `/api/save_objects`, `/api/delete_device`, `/api/save_object`).
- **Logs** : 
    - Restauration de la visibilité granulaire du polling (DEBUG).
    - Ajout d'un log informatif à chaque fin de cycle complet de polling (INFO).
    - Normalisation du format de log horodaté avec TAG et Core ID.
- **MQTT** : Ajout des logs DEBUG pour les publications de noms et de valeurs.
- **Validation** : Flash OTA réussi sur 192.168.1.50, stabilité confirmée sans crash lors des accès API concurrents.
- **Statut** : Version stable et communicative.

- **Prochaine Étape** : Déploiement OTA et tests sur site.

## État au 29 Mai 2026 (Validation Finale & Compilation - v5.8.0) - TERMINÉ
- **Version** : v5.8.0
- **Validation MQTT** : Audit de la `mqtt_publish_queue` (taille 100) et du pacing (5ms) validé.
- **Home Assistant** : Vérification des templates Jinja2 pour les objets Multi-State (décalage d'index 1-based) validée.
- **Nettoyage UI** : Confirmation de la suppression totale des références à l'EDE dans `z_ui.h`.
- **Intégrité** : Compilation réussie via `arduino-cli`.
- **Statut** : Version finale prête pour déploiement.

## État au 28 Mai 2026 (Optimisation MQTT & Logs - v5.7.19) - DÉPLOYÉ
- **Version** : v5.7.19
- **MQTT** : Ajout d'un pacing de 5ms dans `mqtt_gatekeeper_task` pour éviter la saturation `AsyncTCP`.
- **BACnet** : Restriction des logs `Complex-ACK Property ID` au mode debug (`sysCfg.debug`).
- **Réseau** : Confirmation de la Gateway `192.168.1.254` par défaut.
- **UI** : Bouton EDE absent (déjà supprimé).
- **Statut** : Prêt pour compilation et flash.

## État au 28 Mai 2026 (MQTT Auto-Discovery - v5.7.18) - DÉPLOYÉ
- **Version actuelle** : v5.7.18
- **Succès Technologiques** :
    - **MQTT Auto-Discovery** : Implémentation du payload conforme Home Assistant avec abréviations (`~`, `stat_t`, etc.) pour minimiser l'usage mémoire.
    - **Mapping Multi-State** : Injection dynamique de templates Jinja2 (`value_template` / `command_template`) gérant le décalage d'index BACnet (1-based).
    - **MQTT Gatekeeper** : Création d'une tâche dédiée sur le **Core 1** (Priorité 10) pour isoler le traitement JSON et les publications, protégeant ainsi la réactivité de la pile WiFi sur le Core 0.
    - **Gestion LWT** : Configuration du Last Will and Testament (`tele/%PREFIX%/LWT`) pour un suivi précis de la disponibilité.

## État au 28 Mai 2026 (Stabilisation & Stratégie Multi-State - v5.7.17) - DÉPLOYÉ
- **Version actuelle** : v5.7.17
- **Succès Technologiques** :
    - **Validation Stratégie Multi-State** : Décision de publier uniquement les **index numériques** sur MQTT pour économiser les ressources de l'ESP32. La conversion texte (State_Text) sera gérée par Home Assistant via les `value_template` et `command_template` dans le payload d'Auto-Discovery.
    - **Abandon EDE** : La fonctionnalité d'export EDE (CSV) est officiellement abandonnée pour simplifier le firmware.
    - **Test de Robustesse Découverte** : Validation de la stabilité après 3 cycles de suppression/re-découverte complète du device 364004 (98 objets). Le crash "LoadProhibited" observé ponctuellement ne s'est pas reproduit.
- **Note de Reprise** : La structure des objets en RAM et NVS est saine. Le prochain chantier est le peaufinage du JSON Discovery dans `z_mqtt.cpp`.

## État au 28 Mai 2026 (Fix State_Text One-Shot - v5.7.16) - DÉPLOYÉ
- **Version actuelle** : v5.7.16
- **Succès Technologiques** :
    - **Découverte State_Text "One-Shot"** : Migration vers une lecture complète du tableau `State_Text` (Index -1) en une seule transaction. Résout le problème des états dupliqués (ex: 'Eco' répété) sur les automates Distech.
    - **Parsing Complex-ACK Avancé** : Support des séquences de tags multiples dans une seule réponse ReadProperty, permettant de reconstruire la liste des états instantanément.
    - **Conformité ASHRAE 135** : Utilisation stricte des mécanismes `BACnetARRAY` pour minimiser la latence sur le bus MS/TP.

## État au 28 Mai 2026 (Optimisation Mobile UI - v5.7.13) - DÉPLOYÉ
- **Version actuelle** : v5.7.13
- **Succès Technologiques** :
    - **Optimisation de l'espace Mobile** : Réduction drastique des marges et largeurs de colonnes (Reload, OBJ, VAL, POLL) dans le tableau des objets BACnet.
    - **Gain de lisibilité** : Plus d'espace alloué à la colonne `NAME / UNIT`, facilitant la saisie et la lecture sur smartphone.
    - **Ajustement CSS** : Réduction du padding interne des cellules (`0.5rem` -> `0.4rem 0.2rem`).

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
    - **Dashboard Temps Réel** : Visualisation immédiate de la santé du réseau (Heap, RSSI, MS/TP Traffic) du Waveshare.
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
- **Prochaine Étape** : Intégrer les textes d'états dans les publications MQTT (Optionnel).

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
