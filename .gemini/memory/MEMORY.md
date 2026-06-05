# 🧠 Mémoire de Travail (Privée) - Intégration BACnet

## 🛠 Commande de Reprise
Pour reprendre le travail après une interruption, lire ce fichier et le fichier `TRACE.md` associé.
## 📍 Dernier Point de Travail (Checkpoint)
- **Action** : Consolidation UI, Terminal à onglets et Découverte Partielle (Lazy Scan).
- **Statut** : v6.0.2 Déployée ✅.
- **Prochaine Étape** : Optimisation du Discovery Home Assistant (regroupement des publications MQTT).

## 🚀 Workflow de Build & Dev (Obligatoire)
- **Développement UI** : Utiliser le proxy (`utils/dev_ui/2_server_proxy.py`) sur le port 8000.
- **Injection UI** : `python3 utils/dev_ui/3_inject_ui.py` (met à jour `src/z_ui.h`).
- **Compilation** : `./utils/compil.sh`
- **Flash OTA** : `./utils/flashOTA.sh`

## 💡 Architecture v6.0.2
- **Lazy Discovery** : Scan auto de l'identité (Step 0-3) même si disabled. Scan objets (Step 4+) différé à l'activation.
- **UI Terminal** : Fusion Core 0/1 dans un seul composant à onglets (WebSocket routing compatible localhost).
- **ASHRAE ID** : Standardisé en `(did * 1000) + mac`.


## 🌐 Environnement Réseau
...
- **Détails** : Voir [LOCAL_NETWORK.md](./LOCAL_NETWORK.md) pour les adresses IP, Gateway et DNS de l'utilisateur.

## ⚠️ Notes de Vigilance
- Toujours vérifier que le port TCP simulé est libre avant de lancer les tests.
- S'assurer que les trames simulées respectent strictement le préambule MS/TP (0x55 0xFF).
