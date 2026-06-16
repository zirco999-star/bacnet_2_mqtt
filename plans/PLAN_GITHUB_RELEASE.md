# Plan : Préparation de la Release GitHub (Branche `github-release`)

Ce plan détaille la procédure pour assainir le code source et préparer une version publique sur la branche `github-release`, en ne conservant que les fichiers nécessaires à la compilation et en supprimant les informations personnelles.

## 1. Analyse et Filtrage des Fichiers
Nous ne conserverons que les fichiers essentiels à la compilation avec `arduino-cli` ou l'IDE Arduino.

### Fichiers à CONSERVER :
- `bacnet_2_mqtt.ino` : Point d'entrée principal.
- `src/z_bacnet.cpp` & `z_bacnet.h` : Logique BACnet.
- `src/z_config.h` : Configuration (à assainir).
- `src/z_mqtt.cpp` & `z_mqtt.h` : Logique MQTT.
- `src/z_network.cpp` & `z_network.h` : Logique WiFi/Web.
- `src/z_nvs.cpp` & `z_nvs.h` : Gestion de la mémoire non-volatile.
- `src/z_ui.h` : Interface utilisateur compressée.
- `partitions.csv` : Table des partitions ESP32.
- `sketch.yaml` : Configuration de compilation.
- `LICENSE` : Licence du projet.
- `README.md` : Documentation pour GitHub.

### Fichiers à EXCLURE :
- Dossiers : `build/`, `utils/`, `plans/`, `extract/`, `tmp/`, `.vscode/`, `.theia/`.
- Fichiers de suivi : `TRACE.md`, `GEMINI.md`, `MEMORY.md`, `CONVENTION_CODAGE.md`.
- Scripts utilitaires : `*.py`, `*.sh`.

## 2. Procédure d'Exécution

### Étape 0 : Archivage du plan
1. Sauvegarder ce plan sous `/home/dev/bacnet_2_mqtt/plans/PLAN_GITHUB_RELEASE.md`.

### Étape 1 : Préparation de la branche
1. Basculer sur la branche `github-release` : `git checkout github-release`.
2. Identifier et supprimer les fichiers et dossiers non essentiels (ceux listés en section 1.1) via `git rm -r`. Cela ne supprimera les fichiers que sur cette branche.
3. Créer un fichier `.gitignore` minimal pour le futur.

### Étape 2 : Assainissement du code
1. **Configuration (`src/z_config.h`)** :
   - `DEFAULT_SSID` -> `"DEFAULT_SSID"`
   - `DEFAULT_STATIC_IP` -> `"192.168.1.50"`
   - `DEFAULT_GATEWAY` -> `"192.168.1.1"`
   - `DEFAULT_MQTT_SERVER` -> `"192.168.1.10"`
2. **Commentaires internes** :
   - Parcourir les fichiers `.cpp`, `.h` et `.ino`.
   - Supprimer les commentaires contenant des mentions spécifiques au déploiement local, des noms de serveurs internes ou des notes de debug privées (ex: mentions de "Freebox", IPs spécifiques, ou notes de l'agent Gemini non pertinentes pour le public).

### Étape 3 : Validation et Commit
1. Vérifier la compilation via `arduino-cli`.
2. Créer un commit de release : `git commit -m "Public release v6.8.6"`.

## 3. Vérification
- Tentative de compilation locale sur la branche `github-release`.
- Inspection visuelle de `z_config.h` pour s'assurer qu'aucun secret n'est resté.
