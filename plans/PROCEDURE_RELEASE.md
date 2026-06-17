# Manuel Opérationnel : Release GitHub & Compilation Publique

Ce document consigne la procédure exacte utilisée pour créer la version v6.8.6 et servira de référence pour toutes les futures releases du projet BACnet2MQTT.

## 1. Préparation de la branche de Release

### Isolation (Branche `github-release`)
La branche `github-release` doit être une version "sanitisée" du code, exempte de données personnelles et de fichiers de développement.

1. **Bascule sécurisée** :
   ```bash
   git stash
   git checkout github-release
   ```

2. **Assainissement de `src/z_config.h`** :
   Remplacer les valeurs `DEFAULT_SSID`, `DEFAULT_WIFI_PASS`, `DEFAULT_STATIC_IP`, etc., par des placeholders génériques (`"YOUR_SSID"`, etc.).

3. **Nettoyage logique** :
   Supprimer de l'index Git (SANS supprimer physiquement sur les autres branches) les dossiers de travail :
   - `utils/`, `plans/`, `extract/`, `tmp/`, `build/`
   - Fichiers : `TRACE.md`, `GEMINI.md`, `MEMORY.md`, scripts `.py` et `.sh`.

4. **README Final** :
   S'assurer que le `README.md` est la version bilingue (FR/EN) incluant :
   - Les tags (#home-assistant, #bacnet, etc.).
   - Les spécifications matérielles (Waveshare ESP32-S3, Distech ECB-203).
   - Le tableau exact des topics MQTT.

## 2. Génération du Binaire Public

Pour garantir un binaire fonctionnel pour la communauté (gestion de la PSRAM OPI), utiliser impérativement la commande de compilation complète extraite de `utils/compil.sh`.

### Commande de compilation (exécutée sur `github-release`) :
```bash
/home/dev/bin/arduino-cli compile \
  --fqbn esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=cdc,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=custom,DebugLevel=debug,PSRAM=opi,LoopCore=1,EventsCore=1,EraseFlash=all,JTAGAdapter=builtin,ZigbeeMode=default \
  --build-property "build.extra_flags=-mfix-esp32-psram-cache-issue -DESP32 -DCORE_DEBUG_LEVEL=4" \
  --build-property "board_build.partitions=partitions.csv" \
  --output-dir ./build_public \
  ./bacnet_2_mqtt.ino
```

### Exportation :
Le fichier à publier est le binaire fusionné : `build_public/bacnet_2_mqtt.ino.merged.bin`.

## 3. Publication sur GitHub

1. **Push Git** :
   ```bash
   git push github github-release --force
   ```
2. **GitHub Release** :
   - Créer un Tag (ex: `v6.8.6`).
   - Attacher le binaire renommé (ex: `BACnet2MQTT_v6.8.6_Waveshare_ESP32S3.bin`).
   - Utiliser la branche `github-release` comme cible.

## 4. Retour au Developpement
```bash
git checkout dev/lastest
git stash pop
```
