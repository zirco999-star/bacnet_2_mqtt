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
- **v3.3** : **Validation Finale.** Succès de la mise à jour OTA à distance et vérification de la persistance de la configuration.

## 🚀 État de la Main
- **Branche main** : Contient la v3.3 stable.
- **IP Fixe** : 192.168.1.50
- **Gateway** : 192.168.1.254

## 🎯 Prochain Objectif : Phase 1 - Transport MS/TP
L'environnement WiFi est désormais bétonné. Le travail en autonomie va se concentrer sur le driver RS485 et la capture de données BACnet.

## 📝 Notes pour l'Agent (Mode Autonome)
1. Toujours utiliser `curl` pour vérifier l'état de l'ESP32 avant et après un flash OTA.
2. Garder le niveau de log à DEBUG (3) pour la Phase 1.
3. Ne jamais modifier la config WiFi (SSID/Pass) sauf demande explicite.

### [13:40] Phase 2.11 : Restauration Professionnelle (v3.3.S)
- Action : Retour intégral au commit 9526cd7 (v3.3 stable).
- Fix Flash : Passage forcé en mode 'DIO' pour correspondre aux spécifications matérielles validées par les logs ESPHome.
- Objectif : Retrouver l'IP 192.168.1.50 et l'UI industrielle.

### [13:50] Phase 3 : Reconnexion Infrastructure (v3.6)
- État : Bootloader validé en mode DIO/PSRAM.
- Action : Réintégration du code réseau v3.3 (WiFi/IP Fixe/Digest).
- Paramètres : Compilation forcée avec les flags de boot v3.5 (Huge App, OPI).
- Objectif : Retrouver l'IP 192.168.1.50.

### [14:45] Phase 4.1 : Pivot Partition OTA (v3.7.2)
- Erreur Identifiée : 'huge_app' ne supporte pas l'OTA (No OTA).
- Solution : Basculement vers 'min_spiffs' (1.9MB APP avec OTA).
- Fix OTA : Ajout d'un délai de 1s avant ESP.restart() pour éviter l'erreur 'Connection reset'.
- Action requise : Un dernier flash USB est nécessaire pour changer la table des partitions.

### [15:10] Phase 4.2 : Correction Sauvegarde & Network (v3.7.3)
- Bug : Popup de sauvegarde vide et absence de reboot (endpoint /save manquant).
- Fix : Restauration de l'endpoint /save dans z_network.cpp et mise en conformité avec le frontend.
- Network : Restauration des valeurs par défaut pour l'IP statique (192.168.1.50) si non configuré.
- Boot : Unification des versions (v3.7.3) dans tous les fichiers et logs série.
- Partition : Maintien du schéma 'min_spiffs' (1.9MB avec OTA).

### [15:55] Phase 4.6 : Restauration Radicale Réseau v3.8.3 (Pivot v3.3)
- État : IP Statique non fonctionnelle dans les versions 3.7.x.
- Action : Restauration de la logique v3.3 (WiFi.config() et NVS par blocs structurés).
- Fix : Correction de la détection de la checkbox 'static_ip' dans l'API.
- Logs : Réactivation des logs de boot détaillés (Mode, SSID, IP).
- Résultat attendu : Retour immédiat à 192.168.1.50 après flash USB.
