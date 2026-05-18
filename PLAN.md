# Plan d'implémentation : Passerelle BACnet MS/TP vers MQTT

Ce document détaille les étapes nécessaires pour transformer le transceiver actuel en une passerelle applicative fonctionnelle.

## Phase 0 : Stabilité Réseau & Infrastructure (TERMINÉ ✅)
- [x] **Correction WiFi** : Forçage `WIFI_PS_NONE` et `setMinSecurity` pour compatibilité Freebox.
- [x] **Interface Autonome** : Suppression des dépendances CDN pour éviter les hangs sur l'AP.
- [x] **Architecture Modulaire** : Séparation en modules `z_config`, `z_logger`, `z_network`, `z_mstp`, `z_ui`.
- [x] **Gestion de projet** : `arduino-cli` opérationnel pour compilation autonome + Git versioning.

## Phase 1 : Fiabilisation du Transport MS/TP (EN ATTENTE ⏳)
*Objectif : Capture et validation des données binaires.*

1.  **Implémentation du CRC16** :
    *   Ajouter `calc_data_crc` (Polynôme 0x1021) dans `z_mstp.cpp`.
2.  **Extension de la FSM** :
    *   Gestion des trames `0x05` (ExpectReply) et `0x06` (NoReply).
    *   Validation dynamique de la longueur (Header octets 5-6).
    *   Validation CRC16 du Payload.

## Phase 2 : Analyse NPDU / APDU (Minimaliste)
1.  **Parseur NPDU** : Identification des adresses source/destination BACnet.
2.  **Parseur APDU** : Décodage initial du service `ReadProperty`.

## Phase 3 : Mapping et Publication MQTT
1.  **Décodeur ASN.1** : Conversion des types de données BACnet en JSON.
2.  **Moteur de publication** : Envoi sur topic `{prefix}/{id}/{obj_type}/{inst}/val`.

## Phase 4 : Interface Utilisateur et Diagnostics
1.  **Statistiques Web** : Affichage du taux d'erreur CRC et activité bus.
2.  **MQTT Remote Logs** : Supervision à distance.
