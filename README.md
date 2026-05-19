# BACnetMSTP2MQTT Gateway
**Version 3.2 - Stable Infrastructure Release**
*By Z1rc0n1um*

High-performance BACnet MS/TP to MQTT gateway for ESP32-S3 (specifically Waveshare ESP32-S3-R8).

## 🚀 État Actuel
- **WiFi** : ✅ STABLE (Correction définitive des erreurs "CCMP Replay" et "Reason 202" via méthodologie ESPHome).
- **OTA** : ✅ FONCTIONNEL (Mise à jour à distance via "/update" validée).
- **UI** : ✅ INDUSTRIELLE (Console temps réel, monitoring de signal, configuration dynamique).
- **Stockage** : ✅ PERSISTANT (NVS Atomique avec détection de corruption).

## 🛠 Matériel Requis
- **MCU** : Waveshare ESP32-S3-R8 (OPI PSRAM indispensable).
- **Flash Mode** : QIO.
- **PSRAM Mode** : OPI.
- **RS485** : Connecté sur les pins RX:18, TX:17, RTS:21.

## 📡 Configuration WiFi (v3.2+)
Le gateway utilise une séquence de démarrage à froid pour garantir la stabilité sur les routeurs modernes (Freebox) :
1. **No Persistence** : Les credentials ne sont plus écrits en flash de manière automatique par la stack pour éviter les désynchronisations de compteurs de sécurité.
2. **RAM Storage** : La pile WiFi utilise exclusivement la RAM pour ses sessions.
3. **PMF Disabled** : Les trames de gestion protégées sont désactivées pour une compatibilité maximale.

## ⚙️ API REST
- "GET /api/status" : État temps réel (RSSI, IP, Heap, MQTT).
- "GET /api/config" : Lecture de la configuration actuelle.
- "POST /save" : Mise à jour de la configuration (reboot requis).
- "POST /update" : Flashage de binaire OTA.

## 🛣 Roadmap
- [x] Phase 0 : Stabilisation Infrastructure (WiFi, OTA, UI).
- [ ] **Phase 1 : Transport MS/TP** (Capture de trames, CRC16, Token tracking).
- [ ] Phase 2 : Parser BACnet (NPDU/APDU).
- [ ] Phase 3 : Publication MQTT.
