# BACnet MS/TP to MQTT Gateway (ESP32-S3)
[![Version](https://img.shields.io/badge/version-7.1.13-blue.svg)](https://github.com/zirco999-star/bacnet_2_mqtt)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

**Tags:** #home-assistant #mqtt #esp32 #bacnet #mstp #hacf #automation #industrial-iot

---

## 🇫🇷 Français

Une passerelle bidirectionnelle autonome BACnet MS/TP vers MQTT (ESP32-S3) pour l'interfaçage des réseaux de terrain **BACnet MS/TP** (RS-485) avec un broker **MQTT**, incluant une intégration automatique dans **Home Assistant**.

### 🛠️ Environnement de Développement
- **Matériel** : [Waveshare ESP32-S3-RS485-CAN](https://www.waveshare.com/esp32-s3-rs485-can.htm) (8 Mo d'OPI PSRAM indispensables pour la stabilité).
- **Cibles Terrain** : Automates **Distech ECB-203** (Régulation UTA verticale).

### 🚀 Évolutions Majeures v7.1.13
- **Restauration de la Modificabilité des Consignes Écrivables non-commandables** :
  - Restauration de l'exposition en tant qu'entités modifiables (`number`/`switch`/`select`) pour tous les objets écrivables non-commandables (Catégorie 2 : Consignes finales, offsets, limites et changeover).
  - Les écritures sur ces objets sont routées **sans tag de priorité** (priorité 0, pas d'octet `0x49` injecté dans l'APDU), évitant les erreurs protocolaires sur l'automate tout en préservant leur contrôle depuis le Dashboard.
  - Masquage automatique des boutons de Reset uniquement pour ces objets (la réinitialisation se fait directement en modifiant la valeur).

### 📚 Documentation Détaillée
Consultez les guides du dossier [docs/](file:///home/dev/bacnet_2_mqtt/docs/) :
1. [docs/1_compilation_installation.md](file:///home/dev/bacnet_2_mqtt/docs/1_compilation_installation.md) : Guide de compilation et flash.
2. [docs/2_configuration.md](file:///home/dev/bacnet_2_mqtt/docs/2_configuration.md) : Séquence d'installation et paramètres de configuration.
3. [docs/3_api_mqtt.md](file:///home/dev/bacnet_2_mqtt/docs/3_api_mqtt.md) : Référence complète de l'API REST et des topics MQTT.
4. [docs/4_integration_ha.md](file:///home/dev/bacnet_2_mqtt/docs/4_integration_ha.md) : Intégration Home Assistant, entités générées et exemples d'automatisations.

---

## 🇺🇸 English

A standalone bidirectional BACnet MS/TP to MQTT gateway (ESP32-S3) for interfacing **BACnet MS/TP** (RS-485) field networks with an **MQTT** broker, featuring automatic **Home Assistant** integration.

### 🛠️ Hardware Requirements
- **Hardware**: [Waveshare ESP32-S3-RS485-CAN](https://www.waveshare.com/esp32-s3-rs485-can.htm) (8 MB OPI PSRAM is mandatory).
- **Field Target**: **Distech ECB-203** Controllers (Vertical AHU installation).

### 🚀 Major v7.1.13 Features
- **Restore Writability of Non-Commandable Config Variables** :
  - Restored modifiable entities (`number`/`switch`/`select`) for all non-commandable writable objects (Category 2: final setpoints, offsets, limits, and changeover).
  - Writes to these entities are routed **without a priority tag** (priority 0, no `0x49` byte in the APDU) to prevent controller errors while retaining full control from the Lovelace UI.
  - Hides the Reset button entity only for these non-commandable objects.

### 📚 Documentation
Please refer to the files in the [docs/](file:///home/dev/bacnet_2_mqtt/docs/) folder:
1. [Compilation & Installation](file:///home/dev/bacnet_2_mqtt/docs/1_compilation_installation.md)
2. [Gateway Configuration](file:///home/dev/bacnet_2_mqtt/docs/2_configuration.md)
3. [API REST & MQTT Topics Reference](file:///home/dev/bacnet_2_mqtt/docs/3_api_mqtt.md)
4. [Home Assistant Integration & Blueprints](file:///home/dev/bacnet_2_mqtt/docs/4_integration_ha.md)
