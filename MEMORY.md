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

## Protocole de Découverte et Activation (MANDATOIRE)
- **Règle d'or :** AUCUNE activation automatique (Device ou Objet).
- **Cycle de Vie :**
    1. **Découverte Auto (MAC ID) :** Identification MAC.
    2. **Phase 1 (Automatique) :** Lecture exclusive de l'ID Device, Nom, Vendor et Nombre d'objets.
    3. **POINT D'ARRÊT :** Si l'automate est `enabled == false`, le scan s'arrête là (pas de lecture d'objets).
    4. **Phase 2 (Action Utilisateur) :** L'utilisateur active l'automate via l'UI. Le moteur reprend la lecture des métadonnées des objets (OID, Name, Units, Value).
    5. **Phase 3 (Action Utilisateur) :** Les objets découverts sont créés avec `enabled = false`. L'utilisateur doit activer manuellement chaque objet désiré pour lancer le Polling et la diffusion MQTT/HA.
- **Protection Bus :** Ce flux empêche la saturation du bus RS-485 lors de l'ajout de nouveaux équipements.

## NVS Persistence Logic
- **Standard Namespace :** Utiliser exclusivement le préfixe `dv_` (ex: `dv_364004`). Ne plus utiliser de versioning dans le namespace (`dv3_`, `dv4_`).
- **Discovery State:** Always persist disc_step and disc_obj_idx in BACnetPersistenceDev to allow resumption of discovery after reboot.
- **Multi-Device Support:** The engine must handle multiple devices via rotation. No hardcoding index 0.
- **Safe Boot:** Use standard malloc for NVS blobs during boot phases.

## Lessons Learned & Best Practices
- **Prop 87 (Commandable):** The most reliable way to identify writable points is reading Prop 87 (Priority_Array). However, to avoid blocking slow automates (like ECB-203 which returns Code 145 - Busy), a "Best Effort" approach with immediate fallback to type-based deduction (v6.0.5) is mandatory.
- **Polling Loop Robustness:** The polling scanner must always increment even on error/timeout. Objects with `last_update = 0` must be treated as high priority to ensure immediate state sync after boot.
- **Brace Integrity:** In the complex MS/TP FSM, ensure that the cache mutex scope correctly wraps all logic accessing `bacnet_network_cache` to avoid race conditions or accessibility errors of local variables like `apdu_len`.

## Future Optimizations (Planned)
- **Burst Mode (Max_Info_Frames):** To maximize throughput, the FSM will be updated to send multiple frames per token cycle. This requires strict implementation of "SendAnotherFrame" (looping back to `USE_TOKEN`) and "NothingToSend" (immediate `PASS_TOKEN`).