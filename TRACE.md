# Journal de Suivi - BACnet2MQTT

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
