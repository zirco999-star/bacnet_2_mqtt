# BACnet MS/TP to MQTT Gateway (ESP32-S3)
[![Version](https://img.shields.io/badge/version-7.1.16-blue.svg)](https://github.com/zirco999-star/bacnet_2_mqtt)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

**Tags:** #home-assistant #mqtt #esp32 #bacnet #mstp #hacf #automation #industrial-iot

---

## 🇫🇷 Français

Une passerelle bidirectionnelle autonome BACnet MS/TP vers MQTT (ESP32-S3) pour l'interfaçage des réseaux de terrain **BACnet MS/TP** (RS-485) avec un broker **MQTT**, incluant une intégration automatique dans **Home Assistant**.

### 🛠️ Environnement de Développement
- **Matériel** : [Waveshare ESP32-S3-RS485-CAN](https://www.waveshare.com/esp32-s3-rs485-can.htm) (8 Mo d'OPI PSRAM indispensables pour la stabilité).
- **Cibles Terrain** : Automates **Distech ECB-203** (Régulation UTA verticale).

### 🚀 Évolutions Majeures v7.1.16
- **Rafraîchissement instantané Out-of-Service (OOS)** : Cascade asynchrone bidirectionnelle lors de la publication d'une valeur et de son statut OOS, garantissant un retour d'état immédiat sous Home Assistant.
- **Commutateur de Forçage "Manual Operator" (Prio 8)** : Ajout d'un switch `Manual Operator` (`switch.[name]_manual_operator`) pour toutes les entités commandables, permettant d'activer à la demande le forçage à la priorité 8, de piloter les setpoints à ce niveau, et d'envoyer un `AUTO` (Relinquish) pour repasser en mode automatique (priorité 0 par défaut).

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

### 🚀 Major v7.1.16 Features
- **Instant Out-of-Service (OOS) Refresh**: Bidirectional async cascading publication for values and OOS status updates, providing immediate visual feedback in Home Assistant.
- **Forced Override "Manual Operator" Switch (Prio 8)**: Introduces a `Manual Operator` switch entity (`switch.[name]_manual_operator`) on all commandable objects, enabling operators to force values at priority 8, write values at that priority, or send `AUTO` (Relinquish) to release the override and return to normal automatic operation (defaulting to priority 0).

### 📚 Documentation
Please refer to the files in the [docs/](file:///home/dev/bacnet_2_mqtt/docs/) folder:
1. [Compilation & Installation](file:///home/dev/bacnet_2_mqtt/docs/1_compilation_installation.md)
2. [Gateway Configuration](file:///home/dev/bacnet_2_mqtt/docs/2_configuration.md)
3. [API REST & MQTT Topics Reference](file:///home/dev/bacnet_2_mqtt/docs/3_api_mqtt.md)
4. [Home Assistant Integration & Blueprints](file:///home/dev/bacnet_2_mqtt/docs/4_integration_ha.md)
