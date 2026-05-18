# TRACE - bacnet_2_mqtt

## Objectifs
- [x] Initialiser la structure du projet (Migration vers Arduino IDE).
- [ ] Mettre en place l'infrastructure Core 0 (WiFi, Web, MQTT, Logs) dans le format `.ino`.
- [ ] Implémenter le squelette de la FSM MS/TP (Core 1) : Token et PFM.
- [ ] Implémenter le calcul du CRC16 et la capture des trames de données.
- [ ] Implémenter la passerelle BACnet MS/TP vers MQTT.

## Historique
- **2026-05-18** : Migration du projet depuis PlatformIO vers Arduino IDE en raison des contraintes PSRAM de la Waveshare ESP32-S3-R8. Le workspace est désormais un lien symbolique vers `/mnt/save/bacnet_2_mqtt`.
- **2026-05-18** : Création du fichier `GEMINI.md` et `TRACE.md` avec directives spécifiques pour la lecture/écriture en environnement lié (symlink bypass).
