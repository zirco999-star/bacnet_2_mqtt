# TRACE - BACnet2MQTT (v2026)

## État au 21 Mai 2026 (Matin)
- **Version actuelle** : v4.5.9 (Platinum)
- **Environnement** : Core 3.3.8, ESP32-S3 (8MB Flash, 8MB OPI PSRAM)
- **Succès Technologiques** : 
    - **FSM Asynchrone (Core 1)** : Migration vers une gestion UART par interruptions matérielles. Suppression de `uart_wait_tx_done` (zéro blocage RTOS).
    - **Correctif NVS Critique** : Initialisation forcée en RW au boot (plus d'erreur `NOT_FOUND` après effacement).
    - **Performance NVS** : Stockage par Blobs binaires (`putBytes`) pour l'automate. Écriture 100x plus rapide.
    - **Robustesse WiFi** : Protection anti-wipe du mot de passe et fallback AP de secours.
    - **UI v4.5.6+** : Branding validé, multi-contrôleurs, édition des noms, logo GitHub cliquable.

## Prochaine Étape (Phase 2 - Étape 2)
- **Export EDE** : Implémentation du téléchargement CSV dynamique.
- **BACnet Meta** : Décodage des unités et des textes d'état (Multi-State).

## Historique des Incidents Résolus
- [v4.5.6] Blocage LED (Deadlock NVS) -> Résolu par Blob binaire v4.5.8.
- [v4.5.7] WiFi Handshake Timeout -> Résolu par protection API v4.5.8.
- [v4.5.8] NVS NOT_FOUND au boot -> Résolu par RW Init v4.5.9.
