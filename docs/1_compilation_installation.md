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
   ./utils/compil.sh
   ```
   Cette commande génère le fichier binaire compressé dans `build/bacnet_2_mqtt.ino.bin`.

2. **Flasher par liaison USB** :
   Branchez la carte en USB et utilisez l'outil standard `esptool.py` ou l'IDE Arduino.

3. **Flasher par OTA (Over-The-Air)** :
   Une fois la passerelle connectée à votre réseau, vous pouvez la mettre à jour à distance :
   ```bash
   ./utils/flashOTA.sh [IP_DE_LA_PASSERELLE] [PORT]
   # Exemple par défaut : ./utils/flashOTA.sh 192.168.1.50 3232
   ```

### Option B : Via l'IDE Arduino

1. Ouvrez le fichier principal `bacnet_2_mqtt.ino` situé à la racine du projet.
2. Assurez-vous que le dossier `src/` contenant les fichiers d'en-tête et sources `.h`/`.cpp` est bien présent.
3. Configurez les options de carte comme indiqué ci-dessus.
4. Cliquez sur **Vérifier/Compiler**, puis sur **Téléverser**.
