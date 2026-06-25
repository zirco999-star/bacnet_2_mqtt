# Guide de Compilation et d'Installation

Ce guide explique comment compiler le micrologiciel BACnet2MQTT pour l'ESP32-S3 et l'installer sur la passerelle matérielle Waveshare.

## 🛠️ Prérequis matériels et logiciels

1. **Matériel ciblé** : Waveshare ESP32-S3-RS485-CAN (avec 8 Mo de PSRAM OPI).
2. **Environnement** : IDE Arduino (v2.x) ou `arduino-cli`.
3. **Dépendances** : Installez le support des cartes ESP32 (version recommandée 2.0.x ou supérieure).

> [!CAUTION]
> **ALERTE SÉCURITÉ PSRAM / GPIO 47** :
> Le GPIO 47 est utilisé par le bus Octal SPI (OPI) de la PSRAM. Ne **JAMAIS** configurer ce GPIO en entrée ou en sortie dans le code sous peine de plantage immédiat et d'instabilité du bus mémoire.

---

## ⚙️ Configuration de la compilation (IDE Arduino)

Pour compiler correctement avec l'IDE Arduino, sélectionnez les paramètres de carte suivants dans le menu **Outils** :

*   **Carte (Board)** : `ESP32S3 Dev Module`
*   **PSRAM** : `OPI PSRAM` (Impératif pour la stabilité)
*   **Flash Mode** : `QIO`
*   **Partition Scheme** : `8MB Flash (3MB APP/1.5MB SPIFFS)` ou supérieur
*   **Upload Speed** : `921600`

---

## 🚀 Compilation et Flash

### Option A : Via les scripts CLI fournis (Recommandé)

Dans le répertoire de développement, utilisez les scripts du dossier `utils/` :

1. **Compiler le firmware** :
   ```bash
   ./utils/Compile.sh
   ```
   Cette commande génère le fichier binaire compressé dans `build/bacnet_2_mqtt.ino.bin`.

2. **Flasher par liaison USB** :
   Branchez la carte en USB et utilisez l'outil standard `esptool.py` ou l'IDE Arduino.

3. **Flasher par OTA (Over-The-Air)** :
   Une fois la passerelle connectée à votre réseau, vous pouvez la mettre à jour à distance :
   ```bash
   ./utils/Flash_OTA.sh [IP_DE_LA_PASSERELLE] [PORT]
   # Exemple par défaut : ./utils/flashOTA.sh 192.168.1.50 3232
   ```

### Option B : Via l'IDE Arduino

1. Ouvrez le fichier principal `bacnet_2_mqtt.ino` situé à la racine du projet.
2. Assurez-vous que le dossier `src/` contenant les fichiers d'en-tête et sources `.h`/`.cpp` est bien présent.
3. Configurez les options de carte comme indiqué ci-dessus.
4. Cliquez sur **Vérifier/Compiler**, puis sur **Téléverser**.

## Structure du code
### Diagramme des fonctions clés
````mermaid
classDiagram
    class BACnetGateway {
        +uint8_t device_address_
        +bool has_token_
        +void handle_mstp_frame(const std::vector<uint8_t> &data)
        +void send_mstp_frame(const std::vector<uint8_t> &data)
        +uint16_t crc16_ms(const uint8_t *data, size_t length)
        +size_t build_mstp_frame(uint8_t frame_type, uint8_t dest_addr, const std::vector<uint8_t> &payload, uint8_t *buffer)
        +bool check_mstp_crc(const std::vector<uint8_t> &data)
    }

    class MQTTHandler {
        +void publish_bacnet_state(uint32_t device_id, uint32_t object_id, float value, bool oos)
        +void handle_mqtt_command(const std::string &topic, const std::string &payload)
    }

    BACnetGateway --> MQTTHandler : Utilise pour publier les états
    MQTTHandler --> BACnetGateway : Appelle pour envoyer des commandes

    style BACnetGateway fill:#ffd0d0,stroke:#333
    style MQTTHandler fill:#d0d0ff,stroke:#333
````
### Explications :

* `BACnetGateway` : Classe principale avec fonctions personnalisées :
  * `handle_mstp_frame` : Parse les trames MS/TP entrantes.
  * `send_mstp_frame` : Construit et envoie des trames MS/TP.
  * `crc16_ms` : Calcule le CRC16 pour les trames MS/TP.
  * `build_mstp_frame` : Construit une trame MS/TP complète (préambule, headers, payload, CRC).
  * `check_mstp_crc` : Vérifie le CRC d'une trame reçue.
* `MQTTHandler` : Gère la conversion entre BACnet et MQTT.
