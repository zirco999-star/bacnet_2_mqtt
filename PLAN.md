# Plan d'implémentation : Passerelle BACnet MS/TP vers MQTT

Ce document détaille les étapes nécessaires pour transformer le transceiver actuel en une passerelle applicative fonctionnelle.

## Phase 0 : Stabilité Réseau & Infrastructure (CRITIQUE)
*Indispensable pour garantir l'accès à distance (OTA) et la stabilité sur les réseaux Freebox.*

1.  **Correction du Bug de Configuration** :
    *   Mettre à jour la route `/save` pour capturer `static_ip`, `local_ip`, `gateway`, et `subnet`.
    *   Harmoniser l'interface HTML (`index.html`) pour inclure ces champs.
2.  **Optimisation Wi-Fi (Fix Timeout/Handshake)** :
    *   Forcer `WiFi.setSleep(WIFI_PS_NONE)` pour éviter les déconnexions intempestives.
    *   Améliorer la robustesse de la connexion initiale (disconnect/mode/begin).
3.  **Sécurisation du Reboot** :
    *   Remplacer le `ESP.restart()` immédiat dans le thread async par un flag global traité dans la `loop()`.
4.  **Validation OTA** :
    *   Vérifier que le mécanisme d'Update actuel est pleinement fonctionnel pour permettre le déploiement en rack.

## Phase 1 : Fiabilisation du Transport MS/TP
*Capture et validation des données.*

1.  **Implémentation du CRC16 (Data CRC)** :
    *   Ajouter la fonction `calc_data_crc(uint8_t *data, size_t len)` (Polynôme ASHRAE 135).
2.  **Extension de la FSM (Core 1)** :
    *   Gérer les types de trames `BACnetDataExpectReply` (0x05) et `BACnetDataNoReply` (0x06).
    *   Validation CRC16 et extraction du Payload.

## Phase 2 : Analyse NPDU / APDU (Minimaliste)
1.  **Parseur NPDU** : Routage et identification des destinataires.
2.  **Parseur APDU** : Décodage des services `ReadProperty` (0x0C).

## Phase 3 : Mapping et Publication MQTT
1.  **Décodeur ASN.1** : Conversion des types BACnet (Real, Binary, etc.) en JSON.
2.  **Publication MQTT** : Envoi automatique des changements de valeurs sur le broker.

## Phase 4 : Interface Utilisateur et Diagnostics
1.  **Stats Bus** : Affichage temps réel des erreurs CRC et du trafic.
2.  **Logs Distants** : Envoi des diagnostics via MQTT.
