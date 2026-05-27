# TRACE - BACnet2MQTT (v2026)

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
