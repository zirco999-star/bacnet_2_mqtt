# Plan de Remédiation BACnet2MQTT v5.8.6

## 1. Analyse des Causes Racines
- **NPCI Invalide :** La v5.8.1 utilise `0x01 0x20` pour les requêtes confirmées locales sans fournir de DNET, ce qui est hors-norme.
- **Défaut de Parsing :** Le passage d'un parsing NPDU dynamique (v5.7.9) à un parsing statique (v5.8.1) empêche le traitement des trames routées ou segmentées.
- **Timing & Type 7 :** L'automate Distech MAC 4 est lent et peut envoyer des `Reply Postponed` (Type 7) qui ne sont pas gérés, provoquant des timeouts et des pertes de synchronisation.
- **Crash NVS :** L'effacement massif des namespaces NVS dans une tâche asynchrone (WebServer) interfère probablement avec le polling BACnet ou sature la pile.

## 2. Actions Techniques

### A. Conformité Protocolaire (src/z_bacnet.cpp)
- [ ] **Correction APDU ReadProperty :** Utiliser NPCI `0x01 0x04` pour les requêtes locales.
- [ ] **Restauration du Parsing NPDU Dynamique :** Ré-implémenter la détection de la longueur du NPDU (DNET/SNET/HopCount) avant d'accéder à l'APDU.
- [ ] **Gestion du Type 7 (Reply Postponed) :** Dans l'état `MSTP_WAIT_FOR_REPLY`, si une trame de type 7 est reçue, passer immédiatement à `MSTP_DONE_WITH_TOKEN` sans erreur.
- [ ] **Timer Silence :** Réinitialiser `timer_silence` *après* l'envoi effectif de la trame UART.

### B. Optimisation Discovery & Polling
- [ ] **Filtrage d'Objets :** Activer par défaut les objets AI, AV, BI, BV, MSI, MSV lors de la découverte.
- [ ] **Réduction du Bruit NVS :** Utiliser `taskYIELD()` entre chaque écriture d'objet et supprimer les logs `[NVS] Saved` répétitifs.

### C. Stabilisation Système (src/z_network.cpp)
- [ ] **Sécurisation Clear Cache :** Ajouter des logs d'étape pour identifier l'instruction de crash. Vérifier la validité de la chaîne `dev_list` avant le split.
- [ ] **Gestion du Reboot :** S'assurer que la réponse HTTP est envoyée *avant* toute opération bloquante ou critique.

## 3. Étapes d'Exécution
1. Mise à jour de `z_config.h` vers `v5.8.6`.
2. Application des correctifs protocolaire dans `z_bacnet.cpp`.
3. Sécurisation de l'API de reset dans `z_network.cpp`.
4. Compilation et Flash OTA.
5. Validation via logs Heartbeat et MQTT Discovery.

## 4. Validation attendue
- Heartbeat montrant une activité stable (RX/TX équilibrés).
- Découverte des 98 objets du Distech MAC 4 réussie.
- Persistance NVS fonctionnelle après redémarrage.
