# BACnet MS/TP to MQTT Gateway (ESP32-S3)
[![Version](https://img.shields.io/badge/version-6.8.6-blue.svg)](https://github.com/zirco999-star/bacnet_2_mqtt)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

**Tags:** #home-assistant #mqtt #esp32 #bacnet #mstp #hacf #automation #industrial-iot

---

## 🇫🇷 Français

Une Passerelle bidirectionnelle autonome BACNET MSTP vers MQTT (ESP32-S3) pour l'interface des réseaux de terrain **BACnet MS/TP** (RS-485) avec un broker **MQTT**, avec intégration automatique dans **Home Assistant**.

### 🛠️ Environnement de Développement
Ce projet a été développé et intensivement testé dans les conditions réelles suivantes :
- **Matériel** : [Waveshare ESP32-S3-RS485-CAN](https://www.waveshare.com/esp32-s3-rs485-can.htm) (8 Mo d'OPI PSRAM indispensable pour la stabilité du tas).
- **Cible Terrain** : Automates **Distech ECB-203** (Installation YZENTIS UTA verticale).

### 🧠 Points Forts
- **Architecture Dual-Core Réelle** : Core 1 dédié à la FSM MS/TP (ASHRAE 135) pour une précision temporelle stricte ; Core 0 pour le Wi-Fi, MQTT et l'interface Web.
- **Auto-Discovery Home Assistant** : Intégration immédiate des entités via MQTT Discovery (diagnostic gateway + objets BACnet).
- **Gestion Multi-State (MSI/MSO/MSV)** : Traduction bidirectionnelle automatique des `State_Text` (ex: "Confort", "Eco") via une récupération robuste en une seule transaction.
- **Sécurité Mémoire** : Utilisation de la PSRAM via `Heap_4` pour prévenir toute fragmentation lors des scans intensifs.

---

## 🇺🇸 English

A Standalone bidirectional BACNET MSTP to MQTT gateway (ESP32-S3) for interfacing **BACnet MS/TP** (RS-485) field networks with an **MQTT** broker, with automatic integration into **Home Assistant**.

### 🛠️ Development Environment
This project was developed and extensively tested under real-world conditions:
- **Hardware**: [Waveshare ESP32-S3-RS485-CAN](https://www.waveshare.com/esp32-s3-rs485-can.htm) (8 MB OPI PSRAM is mandatory for heap stability).
- **Field Target**: **Distech ECB-203** Controllers (YZENTIS Vertical AHU installation).

### 🧠 Key Highlights
- **True Dual-Core Architecture**: Core 1 dedicated to the MS/TP FSM (ASHRAE 135) for strict timing compliance; Core 0 handles Wi-Fi, MQTT, and the Web UI.
- **Home Assistant Auto-Discovery**: Instant entity integration via MQTT Discovery (gateway diagnostics + BACnet objects).
- **Multi-State Management (MSI/MSO/MSV)** : Automatic bidirectional translation of `State_Text` labels (e.g., "Comfort", "Eco") using robust single-transaction retrieval.
- **Memory Safety**: Extensive PSRAM usage via `Heap_4` to prevent fragmentation during heavy bus scans.

---

## ⚙️ Configuration & Installation

1.  **Sanitize Configuration**: Edit `src/z_config.h` and replace default placeholders:
    ```cpp
    #define DEFAULT_SSID    "YOUR_WIFI_SSID"
    #define DEFAULT_WIFI_PASS "YOUR_WIFI_PASSWORD"
    #define DEFAULT_MQTT_SERVER "192.168.1.10"
    ```
2.  **Compilation Settings**:
    - **Board**: ESP32S3 Dev Module.
    - **Flash Mode**: QIO.
    - **PSRAM**: OPI PSRAM (Mandatory).
    - **Partition Scheme**: 8MB Flash with SPIFFS or higher.

## 📡 MQTT Topics Structure
The gateway uses a granular and structured topic hierarchy:

| Direction | Topic Pattern | Description |
| :--- | :--- | :--- |
| **Telemetry** | `[prefix]/[DeviceID]/[Type]/[Instance]/state` | Current object value (textual for Multi-State) |
| **Command** | `[prefix]/[DeviceID]/[Type]/[Instance]/set` | Write property to BACnet bus |
| **Metadata** | `[prefix]/[DeviceID]/[Type]/[Instance]/name` | BACnet Object_Name property |
| **Diagnostics** | `[prefix]/B2M/[Key]/state` | Gateway health (rssi, heap, temp, mstp_status) |
| **LWT** | `tele/[prefix]/LWT` | Gateway Availability (online/offline) |

*Types: AI, AO, AV, BI, BO, BV, MSI, MSO, MSV*

## 👥 Community & Support
- Join the discussion on [**HACF Forum**](https://forum.hacf.fr/t/hvac-yzentis-distech-ecb-203-passerelle-bacnet-ms-tp-vers-mqtt-sur-esp32-s3/80861).
- Contributions for ASHRAE 135 object decoding are welcome!

*Developed with passion by **Z1rc0n1um** for the open-source community.*
