# Project Memory - BACnet2MQTT

## Project Execution Constraints (MANDATORY)
- **Temporary Files Location:** ALL temporary files created during plan execution, debugging, or testing (e.g., Python scripts, C files, Markdown notes) MUST strictly be located in `/home/dev/bacnet_2_mqtt/tmp/`. NEVER create temporary files directly in the `/home/dev/` root directory.

## Abandoned Features
- **EDE Export:** L'export EDE est officiellement abandonné. Ne plus travailler sur le développement de cette fonctionnalité. Le bouton UI "EDE" sera éventuellement redirigé ou supprimé lors de la prochaine refonte UI.

## Multi-State Strategy (MQTT/HA)
- **Transport:** Publication des index entiers (1, 2, 3...) uniquement sur `/state`.
- **Command:** Réception des index entiers sur `/set`.
- **UI (Home Assistant):** Utiliser le composant `select` (pour MSO/MSV) ou `sensor` (pour MSI). Les libellés textuels sont injectés via le payload Discovery (`options` + `value_template` / `command_template`).

## Strict UI Requirements
- **Sticky Tabs:** The navigation tabs must always be sticky (fixed under the logo) to allow sliding between views without scrolling up.
- **Full Parameter Support:** The Settings page MUST allow editing:
    - WiFi: SSID, Password, Static IP, Local IP, Gateway, Subnet.
    - BACnet: Station MAC, Device ID (did), Max Master (mm), Max Retries, APDU Timeout, Token Skip (tskip), Heartbeat Interval (hbeat).
    - MQTT: Broker IP, User, Password, Root Prefix (mqpr).

## NVS Persistence Logic
- **Discovery State:** Always persist disc_step and disc_obj_idx in BACnetPersistenceDev to allow resumption of discovery after reboot.
- **Multi-Device Support:** The engine must handle multiple devices via rotation. No hardcoding index 0.
- **Safe Boot:** Use standard malloc for NVS blobs during boot phases.