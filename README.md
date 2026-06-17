# BACnet MS/TP to MQTT Gateway (ESP32-S3)
[![Version](https://img.shields.io/badge/version-7.1.12-blue.svg)](https://github.com/zirco999-star/bacnet_2_mqtt)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

**Tags:** #home-assistant #mqtt #esp32 #bacnet #mstp #hacf #automation #industrial-iot

---

## 🇫🇷 Français

Une passerelle bidirectionnelle autonome BACnet MS/TP vers MQTT (ESP32-S3) pour l'interfaçage des réseaux de terrain **BACnet MS/TP** (RS-485) avec un broker **MQTT**, incluant une intégration automatique dans **Home Assistant**.

### 🛠️ Environnement de Développement
- **Matériel** : [Waveshare ESP32-S3-RS485-CAN](https://www.waveshare.com/esp32-s3-rs485-can.htm) (8 Mo d'OPI PSRAM indispensables pour la stabilité).
- **Cibles Terrain** : Automates **Distech ECB-203** (Régulation UTA verticale).

### 🚀 Évolutions Majeures v7.1.12
- **Classification et Routage de Priorité Stricte** :
  - Détection automatique de la commandabilité (présence de la `Priority_Array` via la propriété 87). Si la lecture renvoie une erreur (comme `UNKNOWN_PROPERTY`), l'objet est identifié comme non-commandable (`xIsCommandable = false`).
  - Les requêtes d'écriture `WriteProperty` sur les objets commandables (AO, BO, MSO, AV/BV/MSV de consigne) injectent le Context Tag 4 (priorité 8) et gèrent le relâchement via la commande `AUTO` (Relinquish).
  - Les requêtes d'écriture sur les objets non-commandables (AI, BI, MSI, paramètres systèmes, configurations) sont routées directement **sans tag de priorité** (`0x49` absent de l'APDU) pour éviter les rejets et erreurs de l'automate.
  - Suppression dynamique des entités boutons de Reset superflues dans Home Assistant pour tous les objets identifiés comme non-commandables.

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

### 🚀 Major v7.1.12 Features
- **Strict Priority & Commandability Routing**:
  - Automatic commandability discovery (detecting `Priority_Array` via property 87). Objects returning an error (such as `UNKNOWN_PROPERTY`) are flagged as non-commandable (`xIsCommandable = false`).
  - Writes to commandable objects (AO, BO, MSO, writable AV/BV/MSV setpoints) use Context Tag 4 (Priority 8) and support clearing overrides via `AUTO` (Relinquish).
  - Writes to non-commandable objects (AI, BI, MSI, configurations, loop constants) are sent directly **without a priority tag** (no `0x49` byte in the APDU) to prevent controller PDU errors.
  - Automatically cleans up and deletes obsolete Reset button entities in Home Assistant for objects discovered as non-commandable.

### 📚 Documentation
Please refer to the files in the [docs/](file:///home/dev/bacnet_2_mqtt/docs/) folder:
1. [Compilation & Installation](file:///home/dev/bacnet_2_mqtt/docs/1_compilation_installation.md)
2. [Gateway Configuration](file:///home/dev/bacnet_2_mqtt/docs/2_configuration.md)
3. [API REST & MQTT Topics Reference](file:///home/dev/bacnet_2_mqtt/docs/3_api_mqtt.md)
4. [Home Assistant Integration & Blueprints](file:///home/dev/bacnet_2_mqtt/docs/4_integration_ha.md)
