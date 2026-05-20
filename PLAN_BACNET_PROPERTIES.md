# Plan d'Implémentation : Propriétés BACnet Avancées (Unités, Commandabilité, Textes)

## Objectif
Permettre à la passerelle de lire et stocker les métadonnées BACnet essentielles (`units`, `Priority_Array`, `State_Text`) afin de mapper correctement ces objets vers Home Assistant via MQTT, tout en respectant la norme ASHRAE 135.

## 0. mise en contexte des directives et utilisation des agents.
Toujours lire le fichier de directives GEMINI.md dans le repertoire ou se situe le plan du projet avant de commencer le plan. il faut respecter et appliquer toutes ses consignes a chaque etape du plan
lister les capacités de ses agents avant de commencer, privilégier leur utilisation a chaque fois que c'est possible. utiliser les experts et les notebooks pour obtenir des reponses a des hypothèses.
Tracer toutes les actions effectuer dans le cadre du plan dans le fichier TRACE.md dans le repertoire ou se situe le plan du projet.

## 1. Mise à jour de la structure de données (`z_bacnet.h`)
Il faut enrichir la représentation en RAM des objets pour stocker ces nouvelles métadonnées.

**Modifications dans `BACnetObject` :**
- `uint16_t units;` : Pour stocker l'ID de l'unité (ex: 62 pour °C, 95 pour No-Units). S'applique aux Analog.
- `std::vector<String> state_texts;` : Pour stocker le tableau de textes des Multi-State.
- L'attribut `is_commandable` existe déjà, nous allons le piloter dynamiquement.

**Nouveaux types de requêtes (`BACnetJobType`) :**
- `JOB_READ_UNITS` (Prop 117)
- `JOB_CHECK_COMMANDABLE` (Prop 87 - Priority_Array)
- `JOB_READ_STATE_TEXT` (Prop 74)

## 2. Mise à jour de la Persistance NVS (`z_network.cpp`)
Pour éviter de saturer le bus MS/TP avec ces lectures de métadonnées à chaque redémarrage, elles doivent être sauvegardées en flash.

**Modifications dans `save_cache_to_nvs` et `load_cache_from_nvs` :**
- Sauvegarder la propriété `units`.
- Sauvegarder l'état booléen `is_commandable`.
- (Optionnel/Complexe) Sauvegarder les `state_texts`. Vu la limite de taille des clés NVS, nous pourrions soit les concaténer, soit forcer leur re-lecture au boot pour préserver la RAM/Flash. *Proposition : On sauvegarde uniquement `units` et `is_commandable` en NVS. Les textes Multi-State seront lus en asynchrone au boot si un objet MSx est détecté.*

## 3. Mise à jour de la Machine à États (FSM) BACnet (`z_bacnet.cpp`)
Une fois le scan initial (découverte des 98 objets) terminé, l'ESP32 doit peupler les métadonnées manquantes avant de commencer le polling cyclique continu.

**Logique de file d'attente (Jobs) :**
- Lors de la découverte d'un objet `Analog` (AI, AO, AV) : Mettre en file d'attente un `JOB_READ_UNITS`.
- Lors de la découverte d'un objet `Value` (AV, BV, MSV) : Mettre en file d'attente un `JOB_CHECK_COMMANDABLE`.
- Lors de la découverte d'un objet `Multi-State` (MSI, MSO, MSV) : Mettre en file d'attente un `JOB_READ_STATE_TEXT`.

## 4. Extension du Parseur ASN.1 (`z_bacnet.cpp`)
Le parseur actuel décode bien les requêtes complexes, mais doit être enrichi pour ces cas d'usage spécifiques :

- **Parsing des Enumerated (Unités & Binary)** : Lire la valeur brute (0/1 pour Binary, ID numérique pour Units).
- **Parsing d'un BACnet-Error-PDU** : Intercepter l'erreur `Class 2 (Property)`, `Code 31 (unknown-property)`.
  - *Cas d'usage* : Si on demande la Prop 87 (`Priority_Array`) sur un Analog Value et qu'on reçoit l'Erreur 31, on définit `is_commandable = false`. Si on reçoit un Array, `is_commandable = true`.
- **Parsing d'un Array de CharacterString** : Décoder séquentiellement l'index 0 (taille du tableau), puis boucler pour extraire les strings UTF-8.

## 5. Tests et Validation
1. Compiler et flasher la nouvelle version.
2. Surveiller les logs via WebSockets (`listen_logs_v2.py`) pour vérifier la détection de l'erreur 31 sur les objets non inscriptibles.
3. Vérifier que la structure en mémoire locale contient les bonnes unités (ex: 62) pour les sondes de température.
