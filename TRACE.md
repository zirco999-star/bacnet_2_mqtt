# TRACE - bacnet_2_mqtt

## Objectifs
- [x] Initialiser la structure du projet (Migration vers Arduino IDE).
- [x] Mettre en place l'infrastructure Core 0 (WiFi, Web, Logs) - v1.9.
- [x] Stabiliser le WiFi (Fix Reason 15 / Freebox) - v1.9/2.0.
- [x] Refactorisation en architecture modulaire (.h / .cpp) - v2.1.
- [ ] Implémenter le squelette de la FSM MS/TP (Core 1) : Token et PFM.
- [ ] Implémenter le calcul du CRC16 et la capture des trames de données.
- [ ] Implémenter la passerelle BACnet MS/TP vers MQTT.

## Historique
- **2026-05-18** : Migration vers Arduino IDE pour gestion PSRAM OPI.
- **2026-05-18** : v1.4-v1.5 : Tentatives de stabilisation WiFi (PS_NONE, MinSecurity).
- **2026-05-18** : v1.6-1.8 : Debug des erreurs de pré-processeur Arduino (déport UI) et WiFi stack init.
- **2026-05-18** : v2.0 : Suppression du CDN Tailwind pour fixer le blocage du portail AP (Self-contained UI).
- **2026-05-18** : v2.1 : Refonte totale en architecture modulaire professionnelle. Compilation validée via `arduino-cli`. Gestion Git activée.
