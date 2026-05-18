# ZIRCON1UM - BACnet-MS/TP_2_MQTT

## Description
Ce projet est une passerelle souveraine haute performance permettant de transcoder le trafic **BACnet MS/TP** (RS485) vers des topics **MQTT** via WiFi. Il est optimisé pour la carte **Waveshare ESP32-S3-R8**.

## Architecture
- **App Name** : BACnetMSTP2MQTT
- **Core Engine** : by Z1rc0n1um
- **CORE 1** : FSM MS/TP Temps Réel.
- **CORE 0** : WiFi, WebServer, MQTT, OTA Web.

## Installation
1. Flash initial via USB (merged.bin).
2. Configuration via AP `BACnetMSTP2MQTT_SETUP`.
3. Mises à jour futures via l'onglet System (OTA Web).
