# Plan de Développement : BACnet2MQTT v6.7.X

Ce plan détaille les étapes de migration vers le polling par lot (ReadPropertyMultiple) tout en appliquant les normes de codage `CONVENTION_CODAGE.md` et en intégrant la persistance NVS améliorée.

## Phase 1 : Mise en conformité structurelle (Refactorisation aux Normes)
**Objectif** : Aligner le code existant sur les conventions de nommage compositionnelles.

1. **Étape 1.1 : Refactorisation de `z_config.h`**
   - Modifier les membres de `struct Config` (ex: `mac_address` -> `ucMacAddress`, `device_id` -> `ulDeviceId`).
   - Mettre à jour les macros de configuration avec les préfixes appropriés (`config`, `pd`).
   - **Validation** : Compilation réussie.

2. **Étape 1.2 : Refactorisation de `z_bacnet.h/cpp`**
   - Renommer les variables globales et les membres de `BACnetDevice` / `BACnetObject` (ex: `present_value` -> `fPresentValue`).
   - Appliquer les préfixes `uc`, `us`, `ul`, `x`, `p` selon le type.
   - **Validation** : Test de fonctionnement MS/TP Ring (v6.6.1 stable fonctionnelle mais aux nouvelles normes).

3. **Étape 1.3 : Harmonisation `z_mqtt` et `z_network`**
   - Mise en conformité des variables et fonctions.
   - **Validation** : Reconnexion MQTT et WiFi fonctionnelle.

## Phase 2 : Implémentation du Polling par Lot (RPM)
**Objectif** : Optimiser le bus MS/TP en regroupant les requêtes.

1. **Étape 2.1 : Forgeur d'APDU ReadPropertyMultiple**
   - Implémenter `build_read_property_multiple_apdu` dans `z_bacnet.cpp`.
   - Gérer l'encodage ASN.1 (Service 0x0E).
   - **Validation** : Analyse de trame (si possible via simulateur ou log).

2. **Étape 2.2 : Logique de Batching dans `execute_polling_logic`**
   - Remplacer le polling unitaire par une boucle de regroupement (max 8 objets par défaut).
   - Intégrer le calcul dynamique `MAX_BATCH_SIZE` basé sur `max_apdu_length_accepted`.
   - **Validation** : Observation de la réduction du trafic jeton/données sur le bus.

3. **Étape 2.3 : Parseur ComplexACK pour RPM**
   - Mettre à jour `process_incoming_frame` pour itérer sur les résultats multiples.
   - **Validation** : Mise à jour correcte de plusieurs objets en une seule réponse.

## Phase 3 : Persistance NVS et Paramètres Dynamiques
**Objectif** : Sauvegarder les capacités des équipements découverts.

1. **Étape 3.1 : Intégration des structures NVS v6.7.5**
   - Porter `BACnetPersistenceDev` et `BACnetPersistencePage` dans `z_nvs.cpp`.
   - Ajouter le support de `max_apdu`, `apdu_timeout`, `apdu_retries`.
   - **Validation** : Redémarrage de l'ESP32 et restauration correcte du cache réseau.

2. **Étape 3.2 : Découverte dynamique des propriétés réseau**
   - Ajouter les étapes `DISC_DEV_MAX_APDU`, `DISC_DEV_APDU_TIMEOUT`, `DISC_DEV_APDU_RETRIES` dans la FSM de découverte.
   - **Validation** : Vérification dans l'UI ou les logs que les valeurs sont bien extraites du device cible (ex: Distech).

## Phase 4 : Stabilisation et Finalisation
1. **Étape 4.1 : Test de charge**
   - Vérifier la stabilité avec > 50 objets sur le bus.
2. **Étape 4.2 : Documentation**
   - Mise à jour de `TRACE.md` et commit final v6.7.0.
