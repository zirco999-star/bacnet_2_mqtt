# Suivi du Projet : BACnetMSTP2MQTT

## 📋 Historique des Versions et Stabilisation

### 🛡 Phase 0 : Infrastructure (Terminée)
- **v2.3.0 - v2.3.5** : Lutte contre les erreurs WiFi "Reason 202" et les crashs NVS. Introduction du rebranding et de l'UI Industrielle.
- **v2.4 - v2.5** : Tentative de clonage de la stack WiFi ESPHome (PMF fix, Power Save fix).
- **v2.6** : Échec de l'init native IDF (Bootloop).
- **v2.7** : Introduction du Scan-First et BSSID Lock. Premier succès partiel du scan.
- **v2.8** : **Percée Technique.** Désactivation de la persistance WiFi (Cold Boot) pour tuer le CCMP Replay.
- **v2.9** : Passage en mode **Asynchrone**. Le WiFi ne bloque plus le démarrage des services.
- **v3.0** : Activation du mode **DEBUG** total et protection NULL Pointer sur le Logger.
- **v3.1** : **Atomic Wipe.** Nettoyage automatique du NVS en cas de corruption du mot de passe. Ajout du bouton 👁️ sur l'UI.
- **v3.2** : **Validation Finale.** Succès de la mise à jour OTA à distance et vérification de la persistance de la configuration.

## 🚀 État de la Main
- **Branche main** : Contient la v3.2 stable.
- **IP Fixe** : 192.168.1.50
- **Gateway** : 192.168.1.254

## 🎯 Prochain Objectif : Phase 1 - Transport MS/TP
L'environnement WiFi est désormais bétonné. Le travail en autonomie va se concentrer sur le driver RS485 et la capture de données BACnet.

## 📝 Notes pour l'Agent (Mode Autonome)
1. Toujours utiliser `curl` pour vérifier l'état de l'ESP32 avant et après un flash OTA.
2. Garder le niveau de log à DEBUG (3) pour la Phase 1.
3. Ne jamais modifier la config WiFi (SSID/Pass) sauf demande explicite.
