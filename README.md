# BACnet MS/TP to MQTT Gateway (ESP32-S3)
[![Version](https://img.shields.io/badge/version-7.1.7-blue.svg)](https://github.com/zirco999-star/bacnet_2_mqtt)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

**Tags:** #home-assistant #mqtt #esp32 #bacnet #mstp #hacf #automation #industrial-iot

---

## 🇫🇷 Français

Une passerelle bidirectionnelle autonome BACnet MS/TP vers MQTT (ESP32-S3) pour l'interfaçage des réseaux de terrain **BACnet MS/TP** (RS-485) avec un broker **MQTT**, incluant une intégration automatique dans **Home Assistant**.

### 🛠️ Environnement de Développement
- **Matériel** : [Waveshare ESP32-S3-RS485-CAN](https://www.waveshare.com/esp32-s3-rs485-can.htm) (8 Mo d'OPI PSRAM indispensables pour la stabilité).
- **Cibles Terrain** : Automates **Distech ECB-203** (Régulation UTA verticale).

### 🚀 Évolutions Majeures v7.1.7
- **Séparation des Forçages** : Distinction stricte entre le forçage manuel physique (bit `Overridden` lu sur la propriété 111 de l'automate) et le forçage réseau émis par la passerelle en priorité 8 (`overridden_bacnet` géré par la passerelle).
- **Bouton de Libération (Reset)** : Génération automatique d'un bouton de Reset dans Home Assistant pour chaque objet commandable permettant d'envoyer la commande de libération de priorité (`AUTO`/Relinquish).
- **Correction Payloads Binaires** : Suppression des décimales pour les objets binaires (BI/BO/BV) publiés sur MQTT (`"0"` / `"1"` au lieu de `"0.00"` / `"1.00"`), évitant les états `unknown` dans HA.
- **Outils Publics Communauté** : Nouveaux scripts python pour interroger la passerelle sous forme de tableau ASCII (`list_objects.py`) et écrire des propriétés de manière flexible (`write_property.py`) avec avertissements normatifs sur la priorité d'écriture.

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

### 🚀 Major v7.1.7 Features
- **Separated Overrides**: Strict distinction between physical local override (read from BACnet property 111 `Overridden` bit) and network gateway override (`overridden_bacnet` virtual state managed on priority 8).
- **Relinquish Button (Reset)**: Automatic generation of a Reset button in Home Assistant for each commandable object, sending the `"AUTO"` keyword to clear priority 8 control.
- **Binary Format Fix**: Removed decimals from binary telemetry payloads (BI/BO/BV) on MQTT (`"0"` or `"1"` instead of `"0.00"` or `"1.00"`), fixing `unknown` states in HA.
- **CLI Python Tools**: New public scripts in `utils/` to query all objects in a clean ASCII table (`list_objects.py`) and write property values flexibly (`write_property.py`) with standard compliance warnings.

### 📚 Documentation
Please refer to the files in the [docs/](file:///home/dev/bacnet_2_mqtt/docs/) folder:
1. [Compilation & Installation](file:///home/dev/bacnet_2_mqtt/docs/1_compilation_installation.md)
2. [Gateway Configuration](file:///home/dev/bacnet_2_mqtt/docs/2_configuration.md)
3. [API REST & MQTT Topics Reference](file:///home/dev/bacnet_2_mqtt/docs/3_api_mqtt.md)
4. [Home Assistant Integration & Blueprints](file:///home/dev/bacnet_2_mqtt/docs/4_integration_ha.md)
