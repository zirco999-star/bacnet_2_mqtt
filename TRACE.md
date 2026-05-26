# TRACE - BACnet2MQTT (v2026)

## État au 26 Mai 2026 (Fin de soirée)
- **Version actuelle** : v4.7.45 (NVS Unit Persistence)
- **Environnement** : Core 3.3.8, ESP32-S3 (FSM 9-États + NVS Units)
- **Succès Technologiques** : 
    - **Persistance NVS des Unités** : Extension de la structure `BACnetPersistenceObj` pour inclure le champ `units`. Les unités d'ingénierie sont désormais sauvegardées en NVS et restaurées instantanément au boot.
    - **Initialisation RAM Propre** : Correction de `load_device_objects` pour initialiser `present_value` à `0.0f` et recalculer `unit_text` à partir du code unité stocké.
    - **Restauration de la Sauvegarde Automatique** : Réactivation des appels à `save_device_objects()` à la fin du cycle de découverte (scan nominal, timeout ou erreur).
    - **Correctif de Syntaxe** : Nettoyage intégral de `z_bacnet.cpp` suite à des erreurs de concaténation de chaînes lors du refactoring précédent.

## Prochaine Étape
- **Mapping MQTT Final** : Raccorder la file de publication MQTT pour diffuser les données avec leurs unités.

## Historique des Incidents Résolus
- [v4.7.35] Discovery Hang -> Absence de logs et de progression forcée au timeout.
- [v4.7.42] CRC Storage Bug -> Indexation incorrecte des octets CRC en réception.
- [v4.7.45] Unit Persistence Loss -> Champ `units` manquant dans les structures NVS.

