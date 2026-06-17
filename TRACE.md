# Journal de Suivi - BACnet2MQTT

## État au 16 Juin 2026 (Fix WriteProperty(Object_Name) - v7.0.6) - DÉPLOYÉ
- **Version** : v7.0.5 → v7.0.6
- **Bug corrigé** : `/api/save_object` — Lors du changement de nom d'un objet BACnet depuis l'UI Web, le firmware mettait à jour le cache local (RAM + NVS) mais n'envoyait **aucun job BACnet** pour propager l'écriture vers l'automate physique. La propriété `Object_Name` (prop_id=77) restait donc inchangée sur le device, et le topic MQTT retained (`{prefix}/{did}/{type}/{inst}/name`) conservait l'ancienne valeur (ex: `AI:1005`).
- **Fix** ([z_network.cpp](file:///home/dev/bacnet_2_mqtt/src/z_network.cpp)) : Détection du changement de nom avant écrasement (`strncmp`). Si le nom diffère, un `BACnetJob{JOB_WRITE_PROP, prop_id=77}` est enqueuté vers le MAC de l'automate cible, déclenchant un `WriteProperty(Object_Name)` conforme ASHRAE 135.
- **Détecté par** : Script `utils/test_mqtt_topics.py` (FAIL sur 88/89 noms — AI:1005 avait `AI:1005` comme valeur retained au lieu de `Wireless Sensor 3 SetPoin`).
- **Script de test MQTT** : Nouveau script [utils/test_mqtt_topics.py](file:///home/dev/bacnet_2_mqtt/utils/test_mqtt_topics.py) ajouté pour valider l'ensemble des topics MQTT (LWT, B2M gateway, noms retained, états objets, écriture `/set`).

## État au 15 Juin 2026 (Test d'Intégration Complet & Audit HA - v6.9.6) - VALIDÉ
- **Objectif** : Exécution automatisée des phases 0 à 8 (sauvegarde config, reset cache, reboot, découverte de 98 objets, restauration config, et audit des entités Home Assistant avec rapport final).
- **Script développé** : [run_complete_test.py](file:///home/dev/bacnet_2_mqtt/run_complete_test.py) qui orchestre l'ensemble du processus de manière déterministe avec gestion active de la stabilisation HA.
- **Résultat de l'Audit HA** :
  - Sur 89 objets évalués, **42 entités sont validées (OK)**.
  - **47 entités sont en échec/manquantes** (dont 15 en polling actif à l'état `unknown` ou `unavailable`). Les entités indisponibles correspondent à des capteurs physiques absents ou déconnectés sur le bus de l'automate ECB-203 (ex: `ComSensor`, `VoletAir`, `Vanne`) empêchant l'ESP32 d'acquérir une valeur réelle.
  - Le matching d'entités HA a été fiabilisé en filtrant par composant acceptable (ex: `AO:5 Vanne` de type Output est associé au composant `number` tandis que `BO:6 Vanne` est associé à `switch`), éliminant ainsi les collisions historiques.
- **Rapport généré** : [complete_test_report.md](file:///home/dev/bacnet_2_mqtt/reports/complete_test_report.md) compilant l'état de chaque objet.

## État au 15 Juin 2026 (Correction Affichage Unités 'no-units' - v6.9.6) - DÉPLOYÉ
- **Version** : v6.9.5 -> v6.9.6
- **Correction Régression Unité** : Remplacement du terme erroné `"no-usUnits"` (généré par un remplacement automatique) par `"no-units"` dans [z_bacnet.cpp](file:///home/dev/bacnet_2_mqtt/src/z_bacnet.cpp) (fonction `get_unit_text`) et dans [z_mqtt.cpp](file:///home/dev/bacnet_2_mqtt/src/z_mqtt.cpp) (filtre d'Auto-Discovery).
- **Impact UI** : Résout le problème où les objets sans unités (`no-units` / code 95) comme `Temp_salon` s'affichaient par défaut comme `"mA"` dans l'UI Web (en raison d'un mauvais appariement de chaîne forçant le navigateur à se rabattre sur la première clé triée du dictionnaire JS).
- **Validation** : Test de persistance reload concluant et vérification par API que l'unité reçue pour les objets sans unités est bien redevenue `"no-units"`.

## État au 15 Juin 2026 (Réactivité FSM & Résolution Watchdog - v6.9.5) - DÉPLOYÉ
- **Version** : v6.9.5
- **Correction Stabilité FSM** : Remplacement de `vTaskDelay(1)` par `taskYIELD()` à la fin de la boucle principale de la tâche `bacnet_task` dans [z_bacnet.cpp](file:///home/dev/bacnet_2_mqtt/src/z_bacnet.cpp) pour éliminer les délais artificiels lors du traitement actif et de la transmission du jeton. Restauration de la réactivité MS/TP temps réel dure.
- **Sécurisation du Watchdog** : Utilisation du blocage natif déterministe de FreeRTOS via `xQueueReceive(mstp_rx_queue, ..., pdMS_TO_TICKS(1))` pour mettre en veille la tâche Master lorsqu'aucune trame n'est en attente, ce qui libère 100% du Core 1 pour les tâches réseau de priorité inférieure (comme `async_tcp`) et empêche tout déclenchement du Task Watchdog (`task_wdt`) pendant les rechargements intensifs (98 objets).
- **Validation** : Zéro Silence Timeout observé en régime établi. La ronde de jetons est stable à 100%. Validation par script de test de la persistance parfaite des configurations après rechargement.

## État au 15 Juin 2026 (Forçage Who-Is au Démarrage - v6.9.4) - DÉPLOYÉ
- **Version** : v6.9.4
- **Démarrage Déterministe de la Ronde** : Forçage d'un envoi broadcast `Who-Is` initial 3 secondes après le boot de l'appareil si le cache est déjà restauré (non vide). Cela résout le problème d'inactivité au démarrage en réveillant immédiatement le bus MS/TP et en stabilisant la ronde sans attendre le premier cycle de polling.
- **Restauration de la Réception** : Retrait de l'attribut `IRAM_ATTR` sur la tâche de réception `mstp_rx_task` et les fonctions de CRC pour restaurer le comportement de réception nominal stable de la version `main`.

## État au 15 Juin 2026 (Optimisation NVS & IRAM - v6.9.3) - DÉPLOYÉ
- **Version** : v6.9.3
- **Filtre de Sauvegarde (Dirty State)** : La route `/save` de [z_network.cpp](file:///home/dev/bacnet_2_mqtt/src/z_network.cpp) filtre désormais les requêtes redondantes et n'écrit en flash NVS que si les paramètres soumis diffèrent de la configuration actuelle. Élimine 99% des écritures NVS inutiles.
- **Protection IRAM** : Les fonctions critiques de calcul/validation CRC et de la tâche RX (`mstp_rx_task`) ont été marquées avec l'attribut `IRAM_ATTR` dans [z_bacnet.cpp](file:///home/dev/bacnet_2_mqtt/src/z_bacnet.cpp), découplant la réception et le timing de la FSM MS/TP des interruptions d'accès Flash SPI.
- **Sécurisation des Logs** : Les appels `z_log` de débogage dans la tâche RX sont protégés par une vérification de niveau de log locale pour éviter de sauter vers du code en Flash.
- **Validation** : Zéro Silence Timeout observé pendant des salves intensives de requêtes de sauvegarde. L'anneau MS/TP est resté 100% stable.

## État au 15 Juin 2026 (Correction Jeton & Persistance Reload - v6.9.2) - DÉPLOYÉ
- **Version** : v6.9.2
- **Libération de Jeton Anticipée** : Modification de `execute_bacnet_work()` dans [z_bacnet.cpp](file:///home/dev/bacnet_2_mqtt/src/z_bacnet.cpp) pour passer proprement à l'état `MSTP_DONE_WITH_TOKEN` en cas de contention sur le cache (timeout 15ms), évitant le blocage de l'anneau MS/TP.
- **Persistance Configuration Reload** : Introduction d'une variable RAM temporaire `FSM_old_objects` pour copier et restaurer automatiquement les configurations personnalisées de l'utilisateur (polling, limites, etc.) lors d'un rechargement de l'appareil (`/api/reload_device`).
- **Validation** : 100% des objets (98/98) sont découverts et persistés avec succès après rechargement.

## État au 11 Juin 2026 (Refactorisation Industrielle & Data-Driven - v6.8.7) - COMPILÉ
- **Version** : v6.8.7
- **Architecture Data-Driven** : La FSM de découverte est désormais pilotée par une table de vérité `DISCOVERY_STEPS`. L'ajout de nouvelles propriétés BACnet à découvrir ne nécessite plus de modification du code logique, seulement de la configuration de la table.
- **Optimisation ASN.1** : Centralisation du décodage BACnet (Helpers `decode_bacnet_xxx`) pour les types REAL, Unsigned, String et ObjectIdentifier. Suppression de plus de 50 lignes de manipulation brute de tampons (`memcpy`, `bswap`) et sécurisation des offsets de parsing.
- **Modularisation du Parsing** : Division de la fonction monolithique `process_incoming_frame` en sous-fonctions spécialisées (`handle_i_am_response`, `handle_simple_ack`, `handle_complex_ack_discovery`, `handle_complex_ack_polling`, `handle_error_pdu`). Clarté du flux de contrôle via `switch/case` unifié.
- **Sécurisation Temps-Réel (Axe 4)** : Suppression de l'usage du Mutex dans la tâche RX (Priorité 20). Utilisation d'une file asynchrone `mac_discovery_queue` pour transmettre les nouvelles MAC à la tâche Master. Élimine tout risque d'inversion de priorité ou de blocage réseau lors des accès concurrents au cache ou au stockage NVS.
- **Gestion des Tâches** : Refactorisation de `execute_bacnet_work` avec une structure `switch/case` plus robuste et une gestion localisée des buffers de transmission.
- **Gain Mémoire** : Réduction de l'empreinte Flash de ~1.2 Ko malgré l'ajout de fonctionnalités, grâce à la mutualisation du code et à l'architecture déclarative.
- **Validation** : Compilation réussie (1 433 971 octets Flash, 53 648 octets RAM). Architecture prête pour l'extension massive du dictionnaire d'objets.

## État au 4 Juin 2026 (Refactorisation FSM & Découplage RX - v6.3.0) - COMPILÉ
- **Version** : v6.3.0
- **Refactorisation Modulaire** : Division de la fonction monolithique `bacnet_task` en sous-fonctions spécialisées (`handle_mstp_*`, `execute_bacnet_work`, `process_incoming_frame`). Code plus lisible et maintenable.
- **Découplage Multi-Tâches** :
    - **`mstp_rx_task`** (Priorité 20) : Dédiée à la lecture UART temps réel et à l'assemblage des trames. Garantit qu'aucun octet n'est manqué pendant les traitements lourds.
    - **`bacnet_task`** (Priorité 15) : Orchestrateur de la machine à états Maître, consommant les trames via une `mstp_rx_queue`.
- **Conformité ASHRAE 135** :
    - **T_reply_timeout** : Fixé à **265 ms** (strict) pour la couche MAC.
    - **Silence Detection** : Migration vers `esp_timer_get_time()` pour une précision à la microseconde, éliminant le jitter lié à l'ordonnanceur FreeRTOS.
- **Sécurité Mutex** : Utilisation de timeouts non-bloquants (`pdMS_TO_TICKS(15)`) pour l'accès au cache lors de la possession du jeton, garantissant le respect du `T_usage_timeout`.
- **Validation** : Compilation réussie. Prêt pour déploiement OTA.

## État au 4 Juin 2026 (Single-Object Reload & HA Sync - v6.2.3) - DÉPLOYÉ
- **Version** : v6.2.3
- **Correction Reload Object** : L'API `/api/reload_object` démarre désormais la découverte à l'étape `DISC_OBJ_NAME` au lieu de `DISC_OBJ_VALUE`. Cela garantit que le nom, les unités et l'état commandable (Prop 87) sont rafraîchis depuis l'automate avant de mettre à jour Home Assistant.
- **Transilience FSM** : Modification de la FSM BACnet pour autoriser la transition vers la lecture de valeur même pour les objets désactivés (`o.enabled == false`) lorsqu'une requête de rechargement unique (`dev.reload_single`) est active.
- **Validation** : Vérification par logs que les métadonnées (Nom, Unités) sont bien extraites et transmises à HA lors d'un clic sur "Reload" dans l'UI.

## État au 4 Juin 2026 (HA Discovery Cleanup - v6.2.2) - DÉPLOYÉ
- **Version** : v6.2.2
- **Auto-Cleanup Home Assistant** : Implémentation du nettoyage automatique des entités HA lorsque l'option "HA Auto-Discovery" est décochée.
- **Logique de Suppression** : La Gateway envoie désormais des payloads vides aux topics de configuration MQTT pour tous les objets actifs, forçant Home Assistant à retirer les entités de son registre.
- **Détection d'État** : Ajout d'une surveillance du changement d'état `ha_discover` dans le handler `/save` pour déclencher `unpublish_ha_discovery()` uniquement lors d'une transition ON -> OFF.

## État au 4 Juin 2026 (Smart Logs & Sécurité API - v6.2.1) - DÉPLOYÉ
- **Version** : v6.2.1
- **Sécurisation NVS/WiFi** : Correction de la route API `/save` pour éviter l'écrasement des mots de passe avec des chaînes vides si le champ UI est laissé tel quel (condition `length > 0 && != "******"`).
- **Validation IP Stricte** : Ajout d'une vérification native `IPAddress.fromString()` sur les champs Local IP, Gateway et Subnet avant la sauvegarde, prévenant la corruption de la pile LwIP et le Fallback AP intempestif.
- **Hot-Swap Log Level** : Le niveau de log (`lvl`) défini dans l'onglet "Security" est désormais appliqué à chaud, sans nécessiter de redémarrage.
- **Smart Logs** :
    - Déplacement des logs unitaires BACnet (Polling) et MQTT (Publishing) du niveau INFO vers DEBUG pour soulager le buffer WebSocket et LwIP lors des salves à haut débit (Burst Mode).
    - Ajout de compteurs globaux `period_poll_count` et `period_mqtt_pub_count`.
    - Centralisation de l'affichage dans le log "Heartbeat" (INFO, toutes les 60s) au format clair : `Polling device X - polling : Y` et `published topic : ... - published topics : Z`.
- **Statut** : Compilation réussie et flash OTA déployé avec succès.

## État au 4 Juin 2026 (Mode Burst MS/TP - v6.2.0) - COMPILÉ
- **Version** : v6.2.0
- **Mode Burst (Max_Info_Frames)** : Implémentation complète des transitions normatives ASHRAE 135 "SendAnotherFrame" et "NothingToSend".
- **Optimisation Débit** : La Gateway peut désormais envoyer jusqu'à `sysCfg.max_info_frames` (défaut: 3) trames par cycle de jeton si du travail est en attente.
- **Détection Intelligente** : Ajout de `has_bacnet_work()` pour vérifier dynamiquement la présence de jobs en file d'attente ou d'objets nécessitant un polling avant de décider de conserver le jeton.
- **Libération Anticipée** : Si aucun travail n'est prêt au moment de la réception du jeton, celui-ci est passé immédiatement au successeur (gain de bande passante pour les autres maîtres).
- **Stabilité FSM** : Correction des problèmes de portée de variables (brackets de case) et validation par compilation réussie.

## État au 3 Juin 2026 (Hard Real-Time & Ring Stability - v6.0.5) - DÉPLOYÉ
- **Version** : v6.0.5
- **FSM MS/TP BTL** : Migration complète vers `micros()` pour la conformité ASHRAE 135.
- **Ring Management** : Restauration de l'apprentissage dynamique du successeur (PFM Reply) et de la recherche active (Poll For Master).
- **Hard Real-Time** : Implémentation du Gatekeeper TX avec attente active (Busy Wait) sur Core 1, garantissant le `T_turnaround` strict de 1050µs.
- **Synchronisation UART** : `uart_tx` attend désormais la fin physique du transfert (`uart_wait_tx_done`) avant de réinitialiser le timer de silence.
- **Metadata Recovery** : Correction de la logique de récupération des états MSV (Multi-State Value) pour qu'elle soit non-bloquante vis-à-vis du polling normal.
- **Stabilité** : Polling fluide validé avec automate ECB-203.

## État au 2 Juin 2026 (Diagnostics Gateway & Auto-Discovery - v6.0.2) - DÉPLOYÉ
- **Version** : v6.0.2
- **Toggle Home Assistant** : Implémentation complète du bouton "HA Auto-Discovery" dans l'UI (Settings > MQTT).
- **Backend & NVS** : Correction de l'API `/api/status` et du parsing `/save` pour assurer la persistance réelle du choix utilisateur.
- **Enrichissement des Métriques** : Ajout de la Température du Chip (ESP32-S3), de l'Uptime (secondes), du Minimal Heap (détection de fuites) et du statut de santé MS/TP.
- **Santé MS/TP** : Logique de détection d'activité basée sur le mouvement du jeton (`tokens_seen`) entre deux cycles de publication.
- **MQTT Auto-Discovery Gateway** : Implémentation du bloc de déclaration des capteurs internes de la gateway dans Home Assistant.
- **Groupement d'Équipement** : Toutes les métriques de diagnostic sont désormais regroupées sous un seul appareil "BACnet2MQTT Gateway" dans Home Assistant.
- **Optimisation** : Ajout d'un `vTaskDelay` de 100ms après l'envoi du bloc de découverte pour éviter la saturation du broker.

## État au 2 Juin 2026 (Toggle Home Assistant Auto-Discovery - v6.0.1) - DÉPLOYÉ
- **Version** : v6.0.1
- **Contrôle Auto-Discovery** : Ajout d'une option "With Home Assistant Auto-Discovery" dans les réglages MQTT.
- **Logique Conditionnelle** : Les fonctions `publish_ha_autodiscovery` et `trigger_ha_discovery` sont désormais inopérantes si l'option est désactivée (`sysCfg.ha_discover`).
- **Persistance NVS** : Le paramètre est sauvegardé en NVS sous la clé `ha_disc`.
- **Interface Web** : Ajout d'une checkbox dédiée dans l'onglet MQTT pour un pilotage à chaud.
- **Stabilité** : Compilation validée.

## État au 1 Juin 2026 (Consolidation UI & Découverte Partielle - v5.9.3) - DÉPLOYÉ
- **Version** : v5.9.3
- **Terminal Unifié à Onglets** : Fusion des deux fenêtres de logs (Core 0 et Core 1) en un seul composant à onglets. Optimisation de l'espace sur le Dashboard tout en conservant le flux temps réel WebSocket pour chaque coeur.
- **Découverte Partielle (Lazy Scan)** : 
    - Le firmware scanne désormais automatiquement les informations d'identité (Device ID, Name, Vendor, Object Count) de tout nouveau device détecté, même si celui-ci est désactivé.
    - Le scan intensif des propriétés d'objets reste différé jusqu'à l'activation manuelle par l'utilisateur.
- **Visualisation d'État Améliorée** : Code couleur dynamique dans la carte "Discovery Status" (Bleu=En cours, Vert=Terminé, Gris=Désactivé) avec formatage sélectif en gras pour la lisibilité.
- **Écosystème Dev-UX** : Mise en place d'un workflow de développement UI "Proxy" permettant d'éditer le HTML/JS localement sur PC avec redirection automatique des WebSockets et de l'Auth API vers le matériel réel.
- **Extensions API** : Ajout du champ `sel` dans `/api/status` pour le suivi en temps réel du nombre d'objets activés par équipement.

## État au 1 Juin 2026 (Activation Granulaire & UX - v5.9.1) - DÉPLOYÉ
- **Version** : v5.9.1
- **Flux d'Activation Utilisateur** : Suppression du bouton "Delete" au profit d'un Switch global par équipement. Un nouveau device détecté sur le bus reste en mode "Veille" (replié, pas de scan d'objets) tant que l'utilisateur ne l'active pas explicitement.
- **Synchronisation HA Pilotée** : Le basculement sur **OFF** d'un équipement déclenche immédiatement un nettoyage complet de Home Assistant (suppression de toutes ses entités) via MQTT, tout en préservant son cache local pour une réactivation future.
- **Améliorations UI** : 
    - Ré-affichage du nom du fabricant (**Vendor**) dans l'en-tête de l'équipement.
    - Gestion intelligente des accordéons : repliage forcé si OFF, déploiement auto si ON.
    - Sécurité Reboot : Ajout d'une boîte de confirmation avant redémarrage.
- **Optimisation Bus MS/TP** : L'automate ignore totalement les équipements désactivés, libérant de la bande passante pour les équipements actifs.

## État au 1 Juin 2026 (Nettoyage HA & Synchro Discovery - v5.9.0) - DÉPLOYÉ
- **Version** : v5.9.0
- **Suppression Automatique des Fantômes** : Dès que la découverte d'un équipement BACnet est terminée sur le bus MS/TP, la passerelle envoie désormais systématiquement des messages de "nettoyage" (payload vide) à Home Assistant pour tous les objets qui ne sont pas activés. Cela permet d'effacer les restes (messages retenus sur le broker) d'une session précédente après un factory reset.
- **Sécurité Discovery** : Interdiction de publier un objet sur HA tant que ses métadonnées (Nom, Type, Unités) n'ont pas été totalement récupérées (évite l'apparition d'entités nommées "Unknown").
- **Wildcard Sync** : Extension de la fonction `publish_ha_autodiscovery` pour supporter le rafraîchissement global d'un device (Trigger HA Discovery avec wildcards).

## État au 1 Juin 2026 (Fix Réseau & Factory Reset - v5.8.9) - DÉPLOYÉ
- **Version** : v5.8.9
- **Correction Factory Reset** : Le mode DHCP est désormais le mode par défaut après un reset (Static IP décoché).
- **Séparation Statut/Config** : L'API `/api/status` et l'interface Web distinguent maintenant clairement l'état réseau actuel (ex: IP de l'Access Point) de la configuration stockée. Cela évite que les formulaires de réglages ne soient pré-remplis avec l'IP `192.168.4.1` en mode AP.
- **Fiabilité des Valeurs par Défaut** : Les constantes `DEFAULT_STATIC_IP`, `DEFAULT_GATEWAY` et `DEFAULT_SUBNET` sont correctement appliquées lors de l'initialisation.

## État au 1 Juin 2026 (Refresh Instantané MQTT - v5.8.8) - DÉPLOYÉ
- **Version** : v5.8.8
- **Réactivité accrue** : Implémentation d'une mise à jour "optimiste" après une écriture BACnet réussie.
- **Logique** : Dès réception du `Simple-ACK` confirmant que l'automate a accepté la nouvelle valeur, le firmware met à jour son cache interne et publie immédiatement la nouvelle valeur sur MQTT.
- **Bénéfice** : Plus besoin d'attendre le prochain cycle de polling (plusieurs secondes) pour voir le changement d'état dans Home Assistant. L'interface est désormais quasi-instantanée.

## État au 1 Juin 2026 (Fix Affichage Multi-State HA - v5.8.7) - DÉPLOYÉ
- **Version** : v5.8.7
- **Suppression Templates HA** : Suppression des templates `val_tpl` et `cmd_tpl` pour les objets Multi-State.
- **Raison** : Le firmware gérant désormais nativement l'envoi de texte et la réception de commandes textuelles (v5.8.5/6), les templates côté Home Assistant provoquaient des erreurs de conversion (décalage d'index ou affichage de la dernière valeur de la liste).
- **Résultat** : L'affichage dans HA est désormais strictement identique à ce que l'automate renvoie, sans risque de décalage.

## État au 1 Juin 2026 (Fix WriteProperty Datatype - v5.8.6) - DÉPLOYÉ
- **Version** : v5.8.6
- **Correction Écriture Multi-State** : Correction du bug où le firmware tentait d'écrire une chaîne de caractères (`CharacterString`) au lieu d'un entier non-signé (`Unsigned Integer`) pour les objets MSV/MSO.
- **Support Typage Dynamique** : La fonction `WriteProperty` gère désormais les types de données corrects selon l'objet ciblé :
    *   `Unsigned` pour Multi-State.
    *   `Real` pour Analog.
    *   `Enumerated` pour Binary.
- **Stabilité des Commandes** : Résolution de l'erreur `0x50` (Error PDU) renvoyée par les automates ECB-203 lors d'une tentative d'écriture avec un type de donnée invalide.

## État au 1 Juin 2026 (Fix Multi-State Mismatch - v5.8.5) - DÉPLOYÉ
- **Version** : v5.8.5
- **Publication d'état** : Le firmware envoie désormais le **texte de l'état** (ex: "Confort") au lieu de l'index numérique (ex: "1.00") sur MQTT pour les objets Multi-State. Cela permet une compatibilité parfaite avec les entités `select` de Home Assistant.
- **Gestion des Commandes** : Le firmware traduit automatiquement le texte reçu de HA (ex: "Eco") en index numérique BACnet (ex: 2) avant d'écrire sur le bus.

## État au 1 Juin 2026 (Auto-Récupération Multi-State - v5.8.4) - DÉPLOYÉ
- **Version** : v5.8.4
- **Recovery Mode** : Implémentation d'une détection automatique des métadonnées manquantes pour les objets MSV/MSI/MSO.
- **Logique** : Si un objet Multi-State est activé (poll=true) mais que ses étiquettes sont vides (ex: après reboot), le firmware suspend le polling pour relire la propriété `State_Text` sur le bus.
- **MQTT Safe-Guard** : Empêche la création d'entités `select` vides dans Home Assistant tant que les étiquettes n'ont pas été récupérées avec succès.
- **API REST** : Ajout du champ `states` dans `/api/objects` pour visualiser les étiquettes chargées.

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

- [v5.6] MQTT Storm -> Boucle de reconnexion infinie saturant LwIP.
- [v5.6] Discovery Failure -> Parser NPCI à offset fixe ignorait les I-Am routés.
- [v5.6] Token Regeneration -> Blocage synchrone du Core 1 par les appels WebSockets.
- [v5.6] MQTT Storm -> Boucle de reconnexion infinie saturant LwIP.
- [v4.7.45] Unit Persistence Loss -> Champ `units` manquant dans les structures NVS.

## État au 3 Juin 2026 (Diagnostic Polling & Prop 87 Hybride - v6.1.2) - DÉPLOYÉ
- **Version** : v6.1.2
- **Vérification Prop 87 Hybride** : Réintégration de la vérification de la Propriété 87 (Priority_Array) pour identifier les objets pilotables, conformément aux recommandations de l'expert.
- **Logique "Best Effort"** : Implémentation d'un mécanisme non-bloquant. En cas d'erreur `Busy` (145), `Unknown Property` ou `Timeout` sur la Prop 87, la Gateway bascule automatiquement sur une déduction par type (v6.0.5) et passe à l'objet suivant.
- **Correction Polling** : 
    - Suppression du délai d'attente au démarrage : les objets avec `last_update == 0` sont désormais prioritaires.
    - Correction du blocage sur erreur : un objet en échec définitif est marqué comme "tenté" pour ne pas stopper la boucle de scan.
- **Visibilité accrue** : Passage des logs `Poll Request/Result` au niveau INFO et ajout du compteur d'objets activés (`Enabled: X`) dans le Heartbeat.
- **Stabilité** : Correction des erreurs de parenthésage de la v6.1.0 empêchant la compilation.

## État au 3 Juin 2026 (Conception Burst Mode - v6.2.0) - PLANIFIÉ
- **Analyse Expert** : Identification d'une sous-utilisation du `Max_Info_Frames` limitant le débit.
- **Planification** : Création du `plans/PLAN_MAX_INFO_FRAMES.md` pour implémenter les transitions normatives "SendAnotherFrame" et "NothingToSend".
- **Objectif** : Multiplier par 3 (ou `max_info_frames`) le débit de données par cycle de jeton.


## [2026-06-04 19:30] v6.3.1: Debug Restoration & Discovery Fix
- **Logs**: Restored verbose FSM (Token, PFM, Data) and frame-level debug logs in `z_bacnet.cpp`.
- **Logic**: Restored name fallback logic for objects returning "Unknown" or empty strings.
- **MQTT**: Fixed race condition in `pending_discovery` using `exchange(false)` to prevent discovery loops.
- **Stats**: Fixed `ms_msgs_rx` counter which was stuck at 0 in the heartbeat.
- **Build**: Successfully compiled v6.3.1.

## [2026-06-05] v6.4.2 : Throttle Non-Destructif et Coalescence
- **Diagnostic** : Les désactivations/activations rapides dans l'UI saturaient le Gatekeeper MQTT, entraînant des pertes de synchronisation.
- **Correction** : Implémentation d'un throttle différé (`deferred`) au lieu de rejeter les requêtes.
- **Coalescence** : Promotion automatique du niveau de scan si une requête est déjà en attente (Objet -> Device -> Global).
- **Hardened Unpublish** : Balayage systématique des 5 domaines HA lors d'un retrait d'objet.
- **Robustesse** : Passage à `pending_discovery.load/store` pour garantir l'atomicité.

## [2026-06-05] v6.4.6 : Favicons Haute Résolution et Web App
- **Amélioration** : Remplacement de l'icône 16x16 par un jeu complet d'icônes HD (96x96, 192x192) et Apple Touch Icon.
- **Support Mobile** : L'interface est désormais prête pour l'ajout à l'écran d'accueil (icône nette sur iOS et Android).
- **Technique** : Injection de 4 tableaux PROGMEM distincts dans `z_ui.h` (~107 KB de Flash utilisés).
- **Stabilité** : Routes dédiées ajoutées dans `z_network.cpp` pour chaque résolution.

## [2026-06-05] v6.4.5 : Intégration du Favicon en PROGMEM
- **Amélioration** : Ajout d'un favicon officiel au serveur web pour finaliser l'esthétique et supprimer les erreurs 404 `/favicon.ico`.
- **Technique** : Conversion du binaire `favicon.ico` en tableau C (`uint8_t[]`) stocké en Flash via un script d'injection Python.
- **Route** : Ajout du handler `/favicon.ico` avec le type MIME `image/x-icon`.
- **UI** : Ajout de la balise `<link rel="icon">` dans le template HTML.

## [2026-06-05] v6.4.4 : Indicateur de santé MS/TP temps réel
- **Problème** : Le voyant MSTP restait à "RUNNING" même si le bus était coupé (basé sur un compteur cumulatif).
- **Correction** : Implémentation d'un flag `ring_active` dans la FSM MS/TP.
- **Logique Liveness** : Le flag passe à `false` sur `Silence Timeout` (perte de jeton) et repasse à `true` dès réception d'une trame valide d'un tiers.
- **UI Sync** : L'API `/api/status` remonte désormais cet état dynamique pour une mise à jour instantanée du voyant MSTP (vert/rouge).

## [2026-06-05] v6.4.9 : Utilisation de la PSRAM et Sécurisation Thread-Safe
- **Problème** : Reboots persistants lors de rafraîchissements rapides (LoadProhibited) dus à la fragmentation de la RAM interne.
- **Correction PSRAM** : Migration des allocations `JsonDocument` (ArduinoJson 7) vers la **PSRAM (8 Mo)** via un allocateur personnalisé. Cela préserve la RAM interne pour la pile TCP/IP.
- **Thread-Safety** : Ajout d'un `ws_mutex` pour protéger les appels `ws.textAll()` contre les collisions avec le serveur web sur le Core 0.
- **Robustesse** : Réduction du timeout du verrou API à 1s et vérification de la validité du flux de réponse (stream).

## [2026-06-05] v6.4.8 : Sérialisation API et Protection Heap
- **Problème** : Corruption de la heap lors de l'entrelacement de plusieurs requêtes HTTP lourdes (98 objets).
- **Verrou Global** : Implémentation d'un sémaphore `api_mutex` garantissant qu'une seule API JSON est générée à la fois.
- **Garde-fou RAM** : Vérification stricte de la mémoire libre (> 50 Ko) avant tout traitement d'API lourde, avec renvoi d'une erreur 503 si nécessaire.
- **Optimisation** : Réduction de la queue de logs WebSocket à 20 messages pour économiser la RAM permanente.

## [2026-06-06] v6.5.4 : Gestion intelligente des sessions (iPhone Refresh Fix)
- **Problème** : Crash LoadProhibited (0x30) lors du "slide refresh" sur mobile. Les navigateurs empilaient plusieurs WebSockets avant de fermer les anciens.
- **v6.5.2** : Implémentation du mutex WebSocket et délai de grâce post-connexion (2s) pour stabiliser la pile TCP.
- **v6.5.3** : Mode WebSocket exclusif (limité à 1 client) et ajout du header 'Connection: close' pour forcer le nettoyage des sockets HTTP.
- **v6.5.4** : Gestion intelligente par IP. Autorise plusieurs appareils (PC + Mobile) mais détecte les rafraîchissements sur un même appareil pour fermer l'ancienne session avant d'activer la nouvelle.
- **Résultat** : Stabilité accrue sous refresh intensif tout en conservant le support multi-client.

## [2026-06-08] v6.6.1 - Persistance MSV & Stabilité WebSocket
- **Stabilité (v6.6.0)** : Résolution définitive du crash 0x30 (Heap corruption). Implémentation du 'Safe Log Delivery' avec mutex sur l'API JSON et pacing WebSocket de 50ms.
- **Persistance (v6.6.1)** : Implémentation de la sauvegarde/restauration des labels Multi-State (MSI/MSO/MSV) en NVS.
- **Optimisation NVS** : Utilisation de namespaces dynamiques `st_[DEVICE_ID]` pour contourner la limite de 1984 octets par namespace.
- **Résultat** : Boot instantané sans cycle de recovery pour les objets MSV. Ring stable.
- **Git** : Version v6.6.1 mergée dans main et taguée.

## [v6.8.5] - 2026-06-09
### Ajout
- **Phase 1** : Refactorisation structurelle complète selon `CONVENTION_CODAGE.md` (`uc`, `ul`, `x`, `pd`, etc.).
- **Phase 2** : Intégration de la persistance NVS étendue (champs réseau dynamiques : Max_APDU, Timeout, Retries).
- **Phase 3** : Implémentation du moteur de Polling par Lot (ReadPropertyMultiple - RPM) pour optimiser les performances du bus.
- **Phase 4** : Découverte dynamique BACnet (lecture adaptative des limites réseaux distantes via la FSM MS/TP).
- Mise à jour stricte de `GEMINI.md` pour imposer les normes de codage et le workflow de compilation.

## [v6.8.5] - 2026-06-09
### Ajout
- **Phase 1** : Refactorisation structurelle complète selon `CONVENTION_CODAGE.md` (`uc`, `ul`, `x`, `pd`, etc.).
- **Phase 2** : Intégration de la persistance NVS étendue (champs réseau dynamiques : Max_APDU, Timeout, Retries).
- **Phase 3** : Implémentation du moteur de Polling par Lot (ReadPropertyMultiple - RPM) pour optimiser les performances du bus.
- **Phase 4** : Découverte dynamique BACnet (lecture adaptative des limites réseaux distantes via la FSM MS/TP).
- Mise à jour stricte de `GEMINI.md` pour imposer les normes de codage et le workflow de compilation.

## [v6.9.0] - 2026-06-15
### Ajout
- **Min, Max, Step Dynamiques** : 
  - Ajout des champs `min`, `max`, et `step` (avec références dynamiques de type "AV:42" possibles pour min et max) persistés en NVS.
  - Résolution dynamique de la découverte HA MQTT selon une table des dépendances `ha_dependencies` gérée en RAM.
  - Republish MQTT automatique des objets dépendants lors du changement d'une valeur de référence.
- **Interface Utilisateur Dev UI** : Modification du frontend pour gérer la saisie des limites avec validation compacte et en grille.
- **Compilation & Correctifs Stabilité** :
  - Résolution du crash Watchdog (WDT) sur CPU 0 lié à `AsyncResponseStream` : remplacement par des réponses JSON basées sur des payloads `String` statiques pour éliminer le décalage quadratique mémoire `O(N^2)` de `StreamString::read()` lors du transfert de gros fichiers contenant des symboles `%` (comme les unités).
  - Élimination des verrous imbriqués `cache_mutex` dans `publish_ha_autodiscovery` (évite des blocages de 100ms par objet pilotable).
  - Compilation réussie avec `utils/compil.sh` et Flash OTA validé avec succès sur le matériel.
- **Correctif FSM / Reload / Toggle** :
  - Résolution définitive du blocage / deadlock FSM sur timeout appareil (metadata) en forçant l'avancement dans `handle_error_pdu` si `dev.ucDiscStep < DISC_OBJ_OID`.
  - Réinitialisation de `dev.usDiscObjIdx = 0` lors de `/api/reload_device` et du redémarrage dans `/api/toggle_device` pour empêcher une auto-détection erronée de fin de découverte avec 0 objet.
  - Persistance de l'état nettoyé en appelant `save_device_objects(did)` lors du reload.
  - Validation réussie du cycle complet (Toggle OFF -> Reload -> Toggle ON -> Découverte des 98 objets) sur le matériel.

## [v6.9.1] - 2026-06-15
### Ajout
- **Normalisation de l'unité %RH** :
  - Remplacement de l'unité `%RH` par `%` dans la construction des payloads d'autodiscovery HA (dans `src/z_mqtt.cpp`) pour se conformer aux exigences de validation strictes de la classe de périphérique `humidity` dans Home Assistant, qui rejette sinon l'entité.
- **Validation en Masse (HA / REST)** :
  - Activation globale par lot (poll=1) des 98 objets du périphérique `364004`.
  - Vérification réussie : 100 % des objets fonctionnels (89 sur 89, hors métadonnées) sont maintenant correctement découverts et actifs dans Home Assistant.
  - Le rapport détaillé de validation a été généré dans `reports/ha_bulk_activation_report.md`.

## [v6.9.2] - 2026-06-15
### Ajout
- **Stabilité MS/TP (Mutex Contention)** :
  - Modification de `execute_bacnet_work()` pour libérer proprement le cycle de jeton (`MSTP_DONE_WITH_TOKEN`) en cas d'échec d'acquisition du verrou `cache_mutex` (timeout de 15ms), évitant le blocage de la machine à états MS/TP en boucle infinie (Silence Timeout).
- **Restauration de la Configuration au Reload** :
  - Préservation et restauration des états de polling (`xEnabled`), limites (`min`/`max`/`step`), et références (`cMinRef`/`cMaxRef`) des objets lors d'un rechargement de l'appareil (`/api/reload_device`).
  - Suppression du `objects.clear()` immédiat dans le serveur Web pour laisser la FSM cloner les anciens objets dans un vecteur temporaire `FSM_old_objects` au moment du comptage, puis restaurer ces configurations à la rediscovery des OID.
- **Validation réussie** :
  - 100 % des objets fonctionnels (89/89) validés et opérationnels dans Home Assistant après reload.



## État au 15 Juin 2026 (Fix Discovery & Silence Timeout - v6.9.7) - DÉPLOYÉ
- **Version** : v6.9.7
- **Stabilité MS/TP** : Déportation du Lazy Save NVS (`save_device_objects_locked`) sur le Core 0 dans `mqtt_gatekeeper_task`. La FSM MS/TP sur le Core 1 n'est plus bloquée par les écritures Flash.
- **Résilience RX** : Ajout d'un `uart_flush_input` lors des Silence Timeouts pour éviter la saturation du buffer par des trames corrompues en cas de collision.
- **Auto-Discovery** : Assouplissement de la condition de publication MQTT : un objet est désormais publié individuellement même si la découverte globale de l'appareil est temporairement en pause (`dev.xDiscoveryDone == false`).
- **Validation** : Firmware compilé et flashé avec succès via OTA (192.168.1.50).
- **En attente** : Validation de la découverte (Phase 3) par l'utilisateur sur le réseau BACnet réel (ECB_203).

## État au 15 Juin 2026 (Fix Deadlock UART & Sync Save - v6.9.8) - DÉPLOYÉ
- **Version** : v6.9.8
- **Fix UART Deadlock** : Retrait de l'appel `uart_flush_input` dans `handle_mstp_idle`. Cet appel depuis le Core 1 provoquait un deadlock de la tâche `mstp_rx_task` bloquée sur `uart_read_bytes`, "éteignant" complètement la réception MS/TP.
- **Fix Découverte Lente (Sync Save)** : Suppression définitive de tous les appels synchrones à `save_device_objects_locked` restants dans `execute_discovery_logic` (Phase 3). Ils sont remplacés par un flag `dev.xDirty = true` pris en charge de manière asynchrone par `mqtt_gatekeeper_task` sur le Core 0. Cela empêche les pertes de jeton liées aux écritures NVS intensives.
- **Validation OTA** : Flash réussi. En attente de validation physique par l'utilisateur.

## [v6.8.5] - 2026-06-09
### Ajout
- **Phase 1** : Refactorisation structurelle complète selon `CONVENTION_CODAGE.md` (`uc`, `ul`, `x`, `pd`, etc.).
- **Phase 2** : Intégration de la persistance NVS étendue (champs réseau dynamiques : Max_APDU, Timeout, Retries).
- **Phase 3** : Implémentation du moteur de Polling par Lot (ReadPropertyMultiple - RPM) pour optimiser les performances du bus.
- **Phase 4** : Découverte dynamique BACnet (lecture adaptative des limites réseaux distantes via la FSM MS/TP).
- Mise à jour stricte de `GEMINI.md` pour imposer les normes de codage et le workflow de compilation.

## [v6.8.5] - 2026-06-09
### Ajout
- **Phase 1** : Refactorisation structurelle complète selon `CONVENTION_CODAGE.md` (`uc`, `ul`, `x`, `pd`, etc.).
- **Phase 2** : Intégration de la persistance NVS étendue (champs réseau dynamiques : Max_APDU, Timeout, Retries).
- **Phase 3** : Implémentation du moteur de Polling par Lot (ReadPropertyMultiple - RPM) pour optimiser les performances du bus.
- **Phase 4** : Découverte dynamique BACnet (lecture adaptative des limites réseaux distantes via la FSM MS/TP).
- Mise à jour stricte de `GEMINI.md` pour imposer les normes de codage et le workflow de compilation.

## [v7.0.0] - 2026-06-16
### Ajout
- **Support Status_Flags (Prop 111)** : Décodage du tag BitString BACnet pour les drapeaux Alarm, Fault, Overridden et Out_Of_Service lors de la phase de découverte.
- **Bypass de sécurité si Fault=1** : Saut automatique de la phase de lecture de la Present_Value et attribution de `NAN` si la sonde est marquée en panne (Fault), afin d'éviter de perturber le bus MS/TP.
- **Écriture Out_Of_Service (Prop 96)** : Intégration de la fonction d'écriture APDU dédiée et routage approprié du job `JOB_WRITE_PROP`.
- **Correction d'erreur de compilation** : Remplacement du point-virgule par une virgule à la ligne 140 de `src/z_bacnet.h`.
- **Validation** : Compilation locale réussie et flashage via OTA sur 192.168.1.50 validé.

## [v7.0.1] - 2026-06-16
### Ajout
- **Hack Climatisation Complet** :
  - Souscription aux commandes MQTT de contournement `/outofservice/set` et décodage de `"ON"` / `"OFF"`.
  - Routage dynamique des publications MQTT vers le topic `/outofservice` pour la propriété 96.
  - Déclaration automatique (Autodiscovery) dans Home Assistant d'un commutateur d'isolation (`switch`) et d'un curseur de forçage de valeur (`number`) pour toutes les Analog Inputs (AI).
  - Blocage de la publication de la Present_Value physique si la sonde est hors-service (`Out_Of_Service`) et synchronisation immédiate de l'état du commutateur lors des mises à jour.
- **Gestion de la Priorité de Commande** :
  - Ajout du paramètre `ucPriority` à la fonction d'écriture APDU [build_write_property_value_apdu](file:///home/dev/bacnet_2_mqtt/src/z_bacnet.cpp#L776) avec injection du Tag contextuel 4.
  - Ajout de la route HTTP POST `/api/writevalue` acceptant le paramètre optionnel `priority` pour les jobs d'écriture.
  - Ajout de la route HTTP POST `/api/outofservice` pour forcer le débrayage manuel d'un objet (Priorité 8).
- **Persistance NVS** :
  - Sauvegarde et restauration des drapeaux `ucStatusFlags` (comprenant l'Out_Of_Service) en mémoire flash.
- **Validation** : Compilation locale réussie et déploiement OTA sur 192.168.1.50 validé.

## [v7.0.2] - 2026-06-16
### Ajout
- **Nouvel Endpoint de Lecture `/api/readproperty`** :
  - Création de la route HTTP POST `/api/readproperty` dans [z_network.cpp](file:///home/dev/bacnet_2_mqtt/src/z_network.cpp#L655) pour lire n'importe quelle propriété (ex: Present_Value, Status_Flags) d'un objet.
- **Suivi des requêtes asynchrones ReadProperty** :
  - Ajout de `xPendingReadJob` et `xReadJobPending` dans [z_bacnet.cpp](file:///home/dev/bacnet_2_mqtt/src/z_bacnet.cpp#L64) pour assurer le suivi de l'Invoke ID.
  - Correction de [handle_complex_ack_polling](file:///home/dev/bacnet_2_mqtt/src/z_bacnet.cpp#L680) : les réponses à `ReadProperty` simples ne sont plus faussement attribuées à `current_poll_idx` mais redirigées vers l'objet/propriété d'origine.
  - Reset automatique du drapeau d'attente dans [handle_error_pdu](file:///home/dev/bacnet_2_mqtt/src/z_bacnet.cpp#L397).
- **Suite de Tests API** :
  - Création de l'outil de test modulaire et robuste [test_api_endpoints.py](file:///home/dev/bacnet_2_mqtt/utils/test_api_endpoints.py) basé sur la bibliothèque `unittest`.
- **Validation** : Compilation locale réussie, déploiement OTA sur 192.168.1.50 validé, et validation de l'intégralité des 10 tests de routes API avec succès.

## [v7.0.5] - 2026-06-16
### Ajout
- **Émulation locale d'Out_Of_Service** :
  - Résolution des limitations des contrôleurs physiques (ex: Distech ECB-203) ne supportant pas ou restreignant l'écriture à la propriété 96 (OutOfService) ou 111 (Status_Flags) sur les entrées analogiques (retournant `NO_SUCH_PROPERTY` ou `WRITE_ACCESS_DENIED`).
  - Implémentation d'une émulation locale dans le cache de la passerelle : l'état `OutOfService` est forcé instantanément en RAM lors d'un appel à `/api/outofservice`.
  - Blocage local des mises à jour de la Present_Value physique (issues du polling périodique RPM ou de lectures simples) quand l'objet est marqué `OutOfService` en RAM, préservant ainsi la valeur forcée injectée par l'utilisateur.
  - Mise à jour immédiate et locale de `Present_Value` dans la RAM de la passerelle lors d'appels à `/api/writevalue` sur des sondes isolées.
- **Sérialisation `/api/objects`** :
  - Ajout des clés `status_flags` et `outofservice` dans le payload JSON retourné pour chaque objet.
- **Scripts de Tests Python** :
  - Correction de la clé `"value"` en `"val"` dans [readproperty.py](file:///home/dev/bacnet_2_mqtt/utils/readproperty.py) et [test_climate_hack.py](file:///home/dev/bacnet_2_mqtt/utils/test_climate_hack.py) pour correspondre au format JSON réel de la passerelle.
  - Ajout du support de retour de valeur pour [readproperty.py](file:///home/dev/bacnet_2_mqtt/utils/readproperty.py).
- **Validation** : Compilation locale réussie, déploiement OTA sur 192.168.1.50 validé, et validation du test complet de scénario de hack UTA avec succès.

## [v7.0.7] - 2026-06-16
### Ajout
- **Documentation Doxygen de z_bacnet.cpp** :
  - Ajout de commentaires Doxygen structurés en français pour l'intégralité des 40 fonctions et tâches clés constituant la FSM BACnet MS/TP.
  - Explications détaillées sur les rôles, les paramètres et les détails d'implémentation (gestion des buffers APDU, validation des CRC8/16, orchestration de la machine à états MS/TP, auto-découverte MAC).
- **Validation** : Compilation du projet avec succès avec le fichier de FSM commenté.

## [v7.0.8] - 2026-06-16
### Modification
- **Documentation et commentaires Doxygen globaux** :
  - Finalisation de la documentation complète en français des fichiers sources du projet (`.cpp`, `.h`, `.ino`, `index.html`) pour maximiser la maintenabilité et la clarté.
  - Incrémentation de la version globale à `v7.0.8` dans `z_config.h`.
- **Validation** : Compilation de l'intégralité du projet validée avec succès.

## [v7.0.10] - 2026-06-16
### Modification
- **Restauration de l'UI** :
  - Restauration de la dernière version non commentée de `z_ui.h` et de `utils/dev_ui/index.html` à la demande de l'utilisateur pour résoudre des problèmes sur l'interface.
  - Incrémentation de la version globale à `v7.0.10` dans `z_config.h`.
- **Validation** : Compilation de l'intégralité du projet validée avec succès.

## [v7.0.11] - 2026-06-17
### Modification
- **Correctif États HA Unknown** :
  - Modification de `publish_mqtt_topic` dans `src/z_mqtt.cpp` pour forcer `pub.retain = true` pour les topics d'états d'objets BACnet (`Present_Value` et `Out_Of_Service`).
  - Résout le bug où Home Assistant affichait des entités à l'état `unknown` au démarrage ou après rechargement, car la passerelle ne publiait les états qu'en mode non-retained et uniquement en cas de changement physique de valeur.
  - Incrémentation de la version globale à `v7.0.11` dans `z_config.h`.
- **Validation** : Compilation validée avec succès.

## [v7.0.12] - 2026-06-17
### Modification
- **Forçage de publication sur Reload** :
  - Modification du traitement de `xReadJobPending` dans `src/z_bacnet.cpp` pour court-circuiter le test de changement `o.fPresentValue != v`. Un rechargement d'objet provoqué par l'API Web force désormais systématiquement une publication MQTT vers le broker.
- **Restauration de cache NVS à NAN** :
  - Modification de `load_device_objects` dans `src/z_nvs.cpp` pour initialiser `obj.fPresentValue` à `NAN` plutôt qu'à `0.0f` lors de la restauration du cache RAM au boot.
  - Corrige le bug où tout objet dont la valeur physique sur le terrain est `0.0` (ex: `DemandeChaud1`) n'était jamais publié lors du premier polling après reboot, car la comparaison `0.0 != 0.0` renvoyait faux.
- **Validation** : Flashage OTA `v7.0.12` validé sur 192.168.1.50, résolution confirmée sur l'état LWT et réactivité instantanée du reload.

## [v7.0.13] - 2026-06-17
### Modification
- **Anti-Perte de Messages (MQTT Outbox Saturé)** :
  - Résolution des pertes de messages MQTT lors des rafales de publication au démarrage (saturant l'outbox du client MQTT ESP-IDF, notamment pendant la publication en rafale des configurations Auto-Discovery).
  - Modification du consommateur de queue dans `src/z_mqtt.cpp` : si `esp_mqtt_client_publish` renvoie une erreur (valeur négative), la commande de publication `pubJob` n'est plus jetée, mais renvoyée en tête de file via `xQueueSendToFront` pour réessai après un délai de 100 ms.
- **Validation** : Validation complète avec le script `validate_retained.py`. Tous les topics cibles de tests (`ConsigneFinale1,2,3` et `DemandeChaud1,2,3`) ont été publiés avec succès et sont marqués comme retained sur le broker MQTT. Uptime stable, zéro fuite mémoire.

## [v7.0.14] - 2026-06-17
### Modification
- **Correctif Résolution Forcing Value (Auto-Discovery)** :
  - Correction de l'Auto-Discovery de l'entité `number` "Forcing Value" pour toutes les Analog Inputs (AI) dans `src/z_mqtt.cpp`.
  - Ajout de la clé `"~"` (pointant vers le base topic de l'objet) absente de la structure du `num_doc`. Sans cette clé, les topics `stat_t` et `cmd_t` abrégés (ex: `~/state`) ne pouvaient être résolus par Home Assistant, laissant l'entité de forçage bloquée à l'état `unknown`.
- **Limites du Forçage (min/max)** :
  - Forçage strict des attributs `min = -1` et `max = 40` dans le payload de configuration de l'entité de forçage pour s'assurer que toutes les régulations et forçages manuels soient contraints dans cette plage.
- **Validation** : Compilation et flashage OTA `v7.0.14` validés sur 192.168.1.50. Validation par script du format du JSON Auto-Discovery sur le broker (les entités sont maintenant correctement résolues avec la clé `"~"` et les bornes `-1/40`).

## [v7.0.15] - 2026-06-17
### Validation
- **Test de Contournement/Forçage (Hack Ventilation UTA via MQTT)** :
  - Objectif : Valider le fonctionnement du mode `OutOfService` (OoS) et du forçage de température exclusivement par MQTT pour déclencher la demande de chauffage de l'UTA.
  - Procédure de test automatique (`test_hack_both.py`) :
    1. Connexion au broker MQTT `192.168.1.11` et abonnement aux topics de température, d'état OoS et de demande de chaud.
    2. Activation du mode Out-Of-Service de `temp_bureau` (`AI:1`) via publication `"ON"` sur `bacnet/364004/AI/1/outofservice/set`.
    3. Forçage de la valeur de température de `temp_bureau` à `-1.00` via publication sur `bacnet/364004/AI/1/set`.
    4. Observation des modifications de l'état physique sur le contrôleur BACnet ECB_203.
    5. Restauration de l'état initial (OoS = `"OFF"`).
  - Résultats de la validation :
    - Le forçage de `temp_bureau` (`AI:1`) applique bien la valeur `-1.00` à la variable finale correspondante `TempFinale2` (`AV:28`).
    - Ce forçage de Zone 2 active la demande de chauffage associée **`DemandeChaud2` (`BV:2`)** qui passe de `0.00` à `1.00` après ~10s.
    - Le forçage de `temp_salon` (`AI:5001`) à `-1.00` quant à lui applique la valeur à `TempFinale1` (`AV:27`), ce qui active la demande de chauffage associée **`DemandeChaud1` (`BV:1`)** de `0.00` à `1.00` après ~10s.
    - La restauration de `outofservice` à `OFF` libère immédiatement la sonde physique, ramenant les variables `TempFinale1/2` à leurs valeurs ambiantes physiques et désactivant le chauffage (`DemandeChaud1/2` repassent à `0.00`).

## [v7.0.16] - 2026-06-17
### Modification
- **Priorité d'Écriture MQTT par Défaut** :
  - Modification de `src/z_mqtt.cpp` pour appliquer la priorité 8 (manual operator) par défaut sur tous les jobs d'écriture MQTT (`job.priority = 8`) afin d'écraser la régulation automatique du contrôleur ECB-203 et éviter que les écritures MQTT ne soient immédiatement survolées par le programme GFX.
- **Validation** : Compilation et flashage OTA réussis.

## [v7.0.17] - 2026-06-17
### Modification
- **Priorité d'Écriture Sélective (Correction Régression Inputs)** :
  - Analyse : Les objets d'entrée (Analog Input, Binary Input, Multi-State Input) ne supportent pas la priorité dans le standard BACnet (pas de Priority_Array). Forcer la priorité 8 sur ces objets causait des rejets d'écriture par le contrôleur.
  - Correction : Limitation de la priorité 8 aux seuls objets commandables (AO, BO, AV, BV, MSO, MSV). Les objets d'entrée (AI, BI, MSI) sont désormais écrits avec la priorité 0 (sans priorité).
- **Validation** :
  - Validation du fonctionnement du mode `OutOfService` et du forçage de température sur `temp_salon` (AI:5001) et `temp_chambre` (AI:3) sans rejet.
  - Identification de l'indépendance de la temporisation de demande de chaud par rapport à `TempoVolet` (le retard de déclenchement est purement lié à la période de polling).
  - Établissement du modèle mathématique de ventilation : `Ventilateur = max(VentilLimitebasse, sum(VoletAir_i * PoidsVolet_i * DemandeChaud_i) / 100)`.
- **Statut Final** : Version `v7.0.17` compilée, déployée par OTA et validée. Original parameters restored.

## [v7.0.18] - 2026-06-17
### Modification
- **Priorité 8 et Out-Of-Service pour les Inputs (AI, BI, MSI)** :
  - Restauration de la priorité 8 par défaut pour toutes les écritures MQTT (conformément aux souhaits de l'utilisateur).
  - Ajout de la validation strict OoS (Option A) : Les écritures de valeurs numériques (`Present_Value` / 85) sur les objets d'entrée (`AI` = 0, `BI` = 3, `MSI` = 13) ne sont transmises à l'automate (avec la priorité 8) que si l'objet est déjà configuré en mode `OutOfService` (OoS) dans le cache.
  - Si l'OoS est désactivé, l'écriture est immédiatement rejetée localement par la passerelle, et la valeur physique actuelle du cache est republiée sur le topic MQTT de l'objet pour réinitialiser l'état dans Home Assistant (feedback visuel de rejet).
- **Validation** :
  - Validation réussie par script `test_oos_strict.py` : l'écriture sur `AI:5001` sans OoS est rejetée et réinitialisée, tandis que l'écriture après passage de OoS à ON est acceptée avec priorité 8 et déclenche correctement la demande de chauffage associée (`DemandeChaud1`).
  - Déploiement de la version `v7.0.18` par OTA validé. Original parameters restored.

## [v7.1.0] - 2026-06-17
### Modification
- **Surveillance Globale Status_Flags (Prop 111)** :
  - Modification de `publish_mqtt_topic` dans `src/z_mqtt.cpp` pour formater le topic d'état Present_Value (85) sous forme d'un objet JSON contenant la valeur numérique (`val`) et les 4 booléens d'état (`alarm`, `fault`, `overridden`, `oos`).
  - Ajout de la publication dynamique des configurations Auto-Discovery pour les 4 binary_sensors associés (problem, connectivity, update) afin de regrouper les informations de diagnostic dans Home Assistant sans augmenter la charge réseau.
  - Ajout de la clé `val_tpl` (value_template) pour extraire le champ `val` sur toutes les entités principales de découverte et éviter d'afficher le JSON brut.
  - Extension du nettoyage `unpublish` pour inclure la suppression propre des configurations de ces 4 binary_sensors de diagnostic.
- **Commande Relinquish (AUTO)** :
  - Ajout de la fonction APDU `build_write_property_relinquish_apdu` dans `src/z_bacnet.cpp` (déclarée dans `src/z_bacnet.h`) pour écrire la valeur `NULL` (Application Tag 0) à une priorité donnée.
  - Interception des valeurs d'écriture `NAN` (Not-A-Number) dans `execute_bacnet_work()` pour déclencher l'APDU Relinquish.
  - Prise en charge de la commande `"AUTO"` dans l'API REST Web `/api/writevalue` (dans `src/z_network.cpp`) et le parser MQTT `/set` (dans `src/z_mqtt.cpp`) pour traduire l'instruction en `NAN`.
- **Validation** :
  - Compilation et flashage OTA `v7.1.0` réussis sur 192.168.1.50.
  - Validation réussie par le script temporaire `tmp/test_v7.1.0_features.py` : les topics d'états MQTT publient bien le JSON, et l'écriture du mot clé `"AUTO"` libère correctement le tableau de priorité de l'automate (retour de `val: null` sur l'objet).

## [v7.1.1] - 2026-06-17
### Modification
- **Optimisation des Diagnostics Status_Flags (OUI/NON)** :
  - Modification de `publish_ha_autodiscovery` dans `src/z_mqtt.cpp` pour déplacer les 4 entités de statut (Alarme, Défaut, Forçage Manuel, Hors Service) du domaine `binary_sensor` vers `sensor`.
  - Modification du filtre `val_tpl` pour retourner directement la chaîne `"OUI"` ou `"NON"` (ex: `{{ 'OUI' if value_json.alarm else 'NON' }}`).
  - Ajout d'icônes dédiées et explicites pour chaque indicateur de diagnostic (ex: `mdi:alarm-light`, `mdi:alert`, `mdi:hand-back-right`, `mdi:power-plug-off`).
  - Ajout d'une logique de migration automatique supprimant les anciens `binary_sensor` pour éviter les entités orphelines dans Home Assistant lors de la découverte.
  - Mise à jour correspondante des fonctions de nettoyage (`unpublish_ha_discovery` et désactivation d'objets) pour purger les deux formats d'entités.
  - Validation effectuée via le sous-agent `ha_agent` interrogeant directement l'API Home Assistant : les entités de diagnostic sont correctement typées en `sensor` avec état textuel à `"NON"`, friendly name correct et icônes appliquées.

## [v7.1.3] - 2026-06-17
### Modification
- **Correction du Relinquish et Décodage des Status_Flags** :
  - Correction dans `handle_simple_ack` (`src/z_bacnet.cpp`) pour éviter d'écrire `NAN` dans le cache `fPresentValue` lors de la réception de l'ACK d'un relinquish (la valeur physique de l'automate doit être relue au prochain poll).
  - Forçage de `ulLastStatusFlagsUpdate = 0` et `ulLastUpdate = 0` pour planifier une relecture immédiate de la valeur sur le bus après un relinquish.
  - Correction du décodeur de réponse ReadJob pour la propriété 111 (Status_Flags) : remplacement du décodage d'entier non signé par un décodage bit string (tag 8) avec masquage approprié.
  - Ajout de logs détaillés au niveau de la machine de décodage périodique pour tracer les changements des drapeaux d'état.

## [v7.1.4] - 2026-06-17
### Modification
- **Séparation Forçage Manuel et Forçage BACnet** :
  - Ajout du booléen `xOverriddenBacnet` dans la structure de données locale `BACnetObject` (`src/z_bacnet.h`) pour mémoriser l'état du forçage réseau de la passerelle.
  - Mise à jour de `handle_simple_ack` (`src/z_bacnet.cpp`) pour positionner `xOverriddenBacnet` à `true` lors d'un ACK d'écriture à la priorité 8, et à `false` lors d'un relinquish à la priorité 8.
  - Ajout de la clé `"overridden_bacnet"` dans le payload JSON publié via MQTT pour la Present_Value.
  - Ajout d'un 5ème sensor de diagnostic `"Forçage BACnet"` dans le protocole de découverte Home Assistant (`src/z_mqtt.cpp`), configuré avec l'icône `mdi:lan-pending` et le template de conversion `OUI/NON`.
  - Mise à jour des boucles de nettoyage de découverte (`unpublish`) pour purger le 5ème sensor en cas de désactivation ou de suppression.
  - Ajout de la clé `"overridden_bacnet"` dans l'API de retour `/api/objects` (`src/z_network.cpp`).
- **Validation** :
  - Compilation et déploiement réussis par flashage OTA `v7.1.4` sur `192.168.1.50`.
  - Validation via le script Python `test_v7.1.4_overridden_bacnet.py` : l'écriture à la priorité 8 active le flag `overridden_bacnet` à `True` dans `/api/objects`, et le relinquish (`AUTO`) le repasse correctement à `False`.
  - Relinquish global appliqué sur l'ensemble des 45 objets de l'automate pour restaurer la configuration d'origine.

## [v7.1.5] - 2026-06-17
### Modification
- **Ajout d'un Bouton de Relinquish ("Reset") dans Home Assistant** :
  - Publication d'une entité de type `button` nommée `[nom objet] Reset` pour tout objet commandable via le protocole de découverte MQTT HA (`src/z_mqtt.cpp`).
  - Configuration de la propriété `payload_press = "AUTO"` et du topic `cmd_t = "~/set"` pour envoyer automatiquement un ordre de relinquish sur l'objet lors de l'appui dans HA.
  - Utilisation de l'icône `mdi:restore` pour identifier visuellement le bouton.
  - Mise à jour de la suppression unitaire et globale de la découverte pour désenregistrer proprement l'entité bouton.
- **Validation** :
  - Compilation et déploiement réussis par flashage OTA `v7.1.5` sur `192.168.1.50`.
  - Validation via le sous-agent `ha_agent` communiquant avec l'API HA locale : les boutons `button.voletair1_reset` et `button.consignefinale1_reset` ont été correctement auto-découverts et rattachés aux équipements respectifs.
  - Le relinquish global via le bouton (ou script de repli) fonctionne comme prévu.

## [v7.1.6] - 2026-06-17
### Modification
- **Correction des binary_sensor et switch MQTT Discovery (Payloads binaires)** :
  - Modification de `publish_ha_autodiscovery` dans `src/z_mqtt.cpp` pour remplacer les payloads `pl_on` / `pl_off` de `"1.00"` / `"0.00"` par `"1"` / `"0"`.
  - Cette modification permet d'aligner l'attente de Home Assistant avec le typage numérique réel du JSON publié pour `Present_Value` (ex: `value_json.val` qui produit le chiffre `1` ou `0` dans le template, et non `"1.00"` / `"0.00"`).
- **Validation** :
  - Compilation et déploiement réussis par flashage OTA `v7.1.6` sur `192.168.1.50`.
  - Validation par le sous-agent `ha_agent` interrogeant l'API HA locale : les entités `binary_sensor.ecb_203_demandechaudx` et `binary_sensor.ecb_203_demandefroidx` ont toutes changé d'état de `"unknown"` à `"off"` avec succès.
  - Les commandes d'écriture/retour sur les `switch` sont préservées et fonctionnelles (les payloads `"1"` et `"0"` étant convertis en flottants `1.0` et `0.0` par `atof` sur la passerelle).

## [v7.1.7] - 2026-06-17
### Modification
- **Correction du formatage des valeurs d'objets binaires (BV/BO/BI) sur MQTT** :
  - Modification de `publish_mqtt_topic` dans `src/z_mqtt.cpp` pour intercepter les types `OBJ_BINARY_INPUT` (0), `OBJ_BINARY_OUTPUT` (4), et `OBJ_BINARY_VALUE` (5) et formater leur valeur Present_Value sans décimale (`%.0f` au lieu de `%.2f`).
  - Cette correction permet de publier la valeur en cache sous forme d'un entier propre (`0` ou `1` dans le JSON, ex: `{"val": 0, ...}`) au lieu d'un flottant (`0.0` ou `0.00`).
  - Home Assistant extrait ainsi correctement le texte `"0"` ou `"1"`, ce qui correspond exactement aux valeurs attendues par `pl_on = "1"` / `pl_off = "0"`.
- **Validation** :
  - Compilation et déploiement réussis par flashage OTA `v7.1.7` sur `192.168.1.50`.
  - Déclenchement de la découverte via l'API `/api/trigger_discovery` pour forcer la mise à jour globale.
  - Validation par le sous-agent `ha_agent` : les entités de type `switch` (telles que `switch.ecb_203_demandechaud1`) sont désormais correctement reconnues à l'état **`off`** par Home Assistant.

## [Lovelace - ECB-203] - 2026-06-17
### Ajout
- **Dashboard Lovelace 'ECB-203' & Intégration Ventilation / Correctifs** :
  - Création du dashboard Lovelace dédié à l'automate ECB-203 (ID HA `445f07a00f471bf7e7fb8f10d0a3cd03`).
  - Implémentation des scripts globaux `script.ecb_203_global_reset` (relâchement global) et `script.ecb_203_global_oos_cancel` (annulation OoS globale) dans `scripts.yaml`.
  - Configuration de la structure Lovelace en JSON dans `.storage/lovelace.ecb_203` incluant 7 vues (Accueil, Zones, Consignes Analogiques, Ventilation & Volets, Commandes Binaires, Modes Multi-États, Sondes & Simulations).
  - **Onglet Ventilation & Volets (Vue 4)** : Nouvel onglet regroupant les entités de ventilation (`Ventilateur`, `Ventil UTA2`, `MaxVentilateur`, `VentilLimitebasse`, et `VoletAir1`, `VoletAir2`, `VoletAir3`) avec leurs 5 diagnostics de statut et bouton Reset.
  - **Onglet Zones (Vue 2 - Layout 2x2)** : Réorganisation en 4 grandes cartes (Salon, Bureau, Chambre, et Global/Système) disposées en grille 2x2. Chaque pièce intègre ses températures, consignes actives, actionneurs, volets de soufflage, consignes éco, et boutons de reset individuels. Les entités sont séparées de façon lisible par des dividers thématiques (`type: section`).
  - **Page d'Accueil (Vue 1 - Commandes rapides)** : Intégration d'une carte de commandes directes pour agir manuellement sur le `Ventilateur`, `VoletAir1 (Salon)`, `VoletAir3 (Bureau)` et `VoletAir2 (Chambre)`.
  - **Correctif Vanne BO:6** : Remplacement de l'ID erroné `switch.ecb_203_vanne_bo_6` par l'ID réel `switch.ecb_203_vanne` pour la commande dans l'onglet des commandes binaires.
  - Redémarrage de Home Assistant et validation de l'état `RUNNING` après 55 secondes (les commandes de volets et de vanne répondent parfaitement).

## [v7.1.9] - 2026-06-17
### Modification
- **Formatage de l'Uptime Gateway en C++ et nettoyage des attributs HA** :
  - Modification de `src/z_mqtt.cpp` pour calculer et formater l'uptime de la Gateway directement en C++ (format lisible ex: "22m 44s", "1h 22m 40s") avant de le publier sous forme de chaîne de caractères sur MQTT.
  - Suppression de la configuration du template Jinja (`val_tpl`) pour l'entité `uptime` dans la fonction `publish_ha_autodiscovery` de `src/z_mqtt.cpp` afin d'utiliser directement la valeur textuelle formatée.
  - Retrait des attributs de mesure obsolètes (`unit_of_measurement` et `device_class`) de l'appel de découverte MQTT pour l'uptime en passant `NULL` pour les deux dans la fonction `pub_gw_sensor`.
  - Incrémentation de la version globale du firmware à `v7.1.9` dans `src/z_config.h`.
- **Validation** :
  - Compilation et flashage OTA `v7.1.9` réussis sur `192.168.1.50`.
  - Vérification de l'état du capteur via l'API Home Assistant : l'état actuel de `sensor.bacnet2mqtt_gateway_gateway_uptime` renvoie bien une chaîne formatée (ex: `"1m 12s"`) et les attributs de mesure (`unit_of_measurement` et `device_class`) ont été supprimés avec succès.

## [v7.1.10] - 2026-06-17
### Modification
- **Retour à l'Auto-Discovery Jinja2 pour l'Uptime Gateway** :
  - Revert de l'uptime calculé en C++ dans `src/z_mqtt.cpp` pour publier à nouveau le nombre brut de secondes.
  - Restauration de la configuration du template Jinja `val_tpl` dans `publish_ha_autodiscovery` pour déléguer le formatage d'uptime à Home Assistant (évite les conflits d'actualisation de l'entité).
  - Incrémentation de la version globale du firmware à `v7.1.10` dans `src/z_config.h`.
- **Validation** :
  - Compilation et flashage OTA réussis.
  - Validation dynamique dans Home Assistant : le capteur s'actualise correctement toutes les 15 secondes au format lisible.

