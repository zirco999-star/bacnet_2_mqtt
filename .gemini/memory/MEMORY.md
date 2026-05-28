# 🧠 Mémoire de Travail (Privée) - Intégration BACnet

## 🛠 Commande de Reprise
Pour reprendre le travail après une interruption, lire ce fichier et le fichier `TRACE.md` associé.
## 📍 Dernier Point de Travail (Checkpoint)
- **Action** : Réorganisation du projet BACnet2MQTT (Sources dans `src/`, Plans dans `plans/`, Utils dans `utils/`).
- **Statut** : Compilation validée ✅ (v4.5.33).
- **Prochaine Étape** : Implémenter le fix `UART_TX_DONE` dans `src/z_bacnet.cpp` et corriger la Gateway dans `src/z_network.cpp`.

## 🚀 Workflow de Build (Obligatoire)
- **Compilation** : `./utils/compil.sh`
- **Flash OTA** : `./utils/flashOTA.sh`

## 🌐 Environnement Réseau
...
- **Détails** : Voir [LOCAL_NETWORK.md](./LOCAL_NETWORK.md) pour les adresses IP, Gateway et DNS de l'utilisateur.

## ⚠️ Notes de Vigilance
- Toujours vérifier que le port TCP simulé est libre avant de lancer les tests.
- S'assurer que les trames simulées respectent strictement le préambule MS/TP (0x55 0xFF).
