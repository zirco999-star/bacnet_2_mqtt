# Plan : Environnement de Développement UI Découplé (v5.9.2)

## Objectif
Permettre l'édition rapide de l'interface utilisateur sans cycle de compilation/flash (15s vs 5min).

## Architecture de l'environnement `utils/dev_ui`
1. **Source de vérité** : `utils/dev_ui/index.html` devient le fichier de travail principal.
2. **Serveur de simulation (`simulator.py`)** : 
   - Serveur Python HTTP (port 8000).
   - Simule les endpoints API `/api/status`, `/api/objects`, `/api/save`.
   - Simule les flux WebSocket `/ws-logs`.
3. **Script de synchronisation (`sync_ui.py`)** :
   - Automate la conversion de `index.html` vers le format `PROGMEM` dans `src/z_ui.h`.
   - Utilise des délimiteurs `rawliteral` pour éviter l'escaping complexe.

## Étapes de mise en œuvre

### Étape 1 : Consolidation du Simulateur
- [ ] Mettre à jour `utils/dev_ui/simulator.py` pour refléter les données de la v5.9.2.
- [ ] Ajouter la gestion des requêtes POST pour simuler la configuration MQTT/IP.

### Étape 2 : Création du script de synchronisation
- [ ] Créer `utils/dev_ui/sync_ui.py`.
- [ ] Fonctionnalité : Lire `index.html`, l'envelopper dans `const char INDEX_HTML[] PROGMEM = R"rawliteral(...)rawliteral";`, et l'injecter proprement dans `src/z_ui.h`.

### Étape 3 : Workflow de développement
1. Lancer le simulateur : `python3 utils/dev_ui/simulator.py`.
2. Ouvrir `http://localhost:8000` dans le navigateur.
3. Éditer `utils/dev_ui/index.html` (changements visibles instantanément au refresh).
4. Une fois satisfait, exécuter `python3 utils/dev_ui/sync_ui.py` pour préparer le firmware final.
5. Compiler une seule fois avec `./utils/compil.sh`.

## Recommandations Expert (Analyse locale)
- **C-Style loops** : Attention aux boucles `for` dans le JS. Toujours utiliser `let i` au lieu de `int i` (fixé en v5.9.2).
- **Responsive design** : Maintenir le support mobile via les Media Queries CSS déjà présentes.
- **WebSocket Mock** : Le simulateur doit fournir un endpoint `/ws-logs` minimal pour éviter les erreurs JS en console.

## Prochaines étapes
1. Création du script `sync_ui.py`.
2. Mise à jour de `simulator.py`.
3. Documentation du mode YOLO pour l'UI.
