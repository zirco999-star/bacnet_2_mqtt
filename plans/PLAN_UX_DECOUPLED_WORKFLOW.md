# PLAN : Développement UI Découplé via Proxy Python

## 1. Contexte et Problématique
Le firmware actuel stocke l'interface utilisateur dans `src/z_ui.h` sous forme de constante `PROGMEM`. Chaque modification de l'UI (CSS/JS/HTML) nécessite une compilation et un flash OTA, ralentissant le cycle de développement.

## 2. Solution : Workflow "Live Proxy"
Mise en place d'un environnement dans `utils/dev_ui/` permettant d'éditer les fichiers web localement et de les tester instantanément avec les données réelles de la passerelle.

### 2.1 Les Outils (Scripts Python)
1.  **`1_extract_ui.py`** : Extrait l'HTML actuel de `src/z_ui.h` vers `utils/dev_ui/index.html`.
2.  **`2_server_proxy.py`** :
    *   Sert `index.html` sur `http://localhost:8000`.
    *   Redirige les appels `/api/*` vers l'IP réelle de l'ESP32 (`192.168.1.50`).
    *   Contourne les problèmes de CORS.
3.  **`3_inject_ui.py`** : Réinjecte le code de `index.html` dans `src/z_ui.h` une fois le design finalisé.

## 3. Workflow de Travail
1.  **Initialisation** : `cd utils/dev_ui && python3 1_extract_ui.py`.
2.  **Développement** :
    *   Lancer le proxy : `python3 2_server_proxy.py`.
    *   Modifier `index.html` dans un éditeur.
    *   Rafraîchir `http://localhost:8000` pour voir les changements en temps réel avec les vraies données BACnet.
3.  **Finalisation** :
    *   Arrêter le proxy.
    *   Injecter les changements : `python3 3_inject_ui.py`.
4.  **Déploiement** :
    *   Compiler : `./utils/compil.sh`.
    *   Flasher : `./utils/flashOTA.sh`.

## 4. Recommandations de Sécurité et Performance
*   **Validation JS** : Toujours vérifier la syntaxe JS (ex: `let` au lieu de `int` dans les boucles) avant injection.
*   **Taille HTML** : Surveiller l'occupation flash (actuellement ~16% RAM/7% Flash) pour ne pas dépasser les limites du partitionnement.
*   **CORS** : Le proxy Python élimine le besoin de modifier les headers de sécurité sur l'ESP32 en phase de dev.

## 5. Prochaines Étapes
*   [ ] Exécuter l'extraction initiale.
*   [ ] Tester la connexion proxy vers l'ESP32 à l'IP `192.168.1.50`.
*   [ ] Commencer les améliorations esthétiques (Mode Sombre, Radar WiFi optimisé).
