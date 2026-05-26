# TRACE - BACnet2MQTT (v2026)

## État au 23 Mai 2026 (Nuit)
- **Version actuelle** : v4.6.5 (Core 1 FSM Fixed)
- **Environnement** : Core 3.3.8, ESP32-S3 (8MB Flash, 8MB OPI PSRAM)
- **Succès Technologiques** : 
    - **Correction Compilation FSM** : Ré-inclusion de `driver/uart.h` et alignement des types `BACnet_Stats`. Utilisation de `UART_SCLK_DEFAULT`.
    - **Support I-Am (APDU 0x10)** : Décodage ASN.1 des Unconfirmed-Requests ajouté. Le cache se peuple désormais automatiquement suite à un Who-Is.
    - **NPCI Dynamique** : Correction du parsing NPDU pour gérer les sauts d'en-tête variables (Local vs Global Broadcast).
    - **Priorité Job Queue** : Les requêtes manuelles (Who-Is) passent avant le polling pour éviter les collisions.
    - **Allègement MQTT** : `setup_mqtt()` temporairement désactivé pour isoler les tests BACnet.

## Prochaine Étape (Phase 2 - Étape 2)
- **Export EDE** : Implémentation du téléchargement CSV dynamique.
- **BACnet Meta** : Décodage des unités et des textes d'état (Multi-State).

## Historique des Incidents Résolus
- [v4.6.1] Timeout UI Bug -> Synchronisation JS/NVS.
- [v4.6.2] Cleanup Identity -> Retrait de sysCfg.device_id.
- [v4.6.3] Discovery Deadlock -> Support I-Am et correction NPCI.
- [v4.6.5] Compile Error -> Correction des includes UART et des conflits de types suite à la refonte de la FSM.
