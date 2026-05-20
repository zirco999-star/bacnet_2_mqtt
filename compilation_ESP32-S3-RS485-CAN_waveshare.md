# Guide de Compilation "Gold Standard" - Waveshare ESP32-S3-RS485-CAN (v2026)

Ce document définit la méthode unique et certifiée pour compiler le projet `bacnet_2_mqtt`. Tout assistant IA ou ingénieur doit suivre ces règles pour garantir un binaire stable et compatible OTA.

## 1. Spécifications Matérielles
*   **MCU** : ESP32-S3-WROOM-1 (N16R8)
*   **Flash** : 16MB (Interface Quad SPI)
*   **PSRAM** : 8MB OPI (Octal SPI)
*   **UART** : Port 1 pour RS485 (Pins 17/18/21)

## 2. Configuration du Build (arduino-cli)
Le FQBN exact doit inclure les options de mémoire et le schéma de partitionnement pour ESP32 Core 3.x :

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=cdc,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,DebugLevel=debug,PSRAM=opi,LoopCore=1,EventsCore=1,EraseFlash=all,JTAGAdapter=builtin,ZigbeeMode=default ...
```

### Paramètres Critiques :
1.  **PSRAM=opi** : Requis pour la RAM de 8MB. Sans cela, crash immédiat du MMU au boot.
2.  **FlashMode=qio** : Mode haute performance (Quad SPI) natif de la carte Waveshare.
3.  **PartitionScheme=app3M_fat9M_16MB** : Équivalent à l'ancien `default_16MB`. Définit les offsets système certifiés :
    *   **NVS** : 0x9000 (Taille 0x5000)
    *   **OTADATA** : 0xE000 (Taille 0x2000)
    *   **APP0** : 0x10000 (Taille 3MB)

## 3. Méthode de Fusion (Merged Binary 16MB)
Pour un flash complet via web (esphome.io), générer un binaire de 16MB incluant le bootloader et la table de partitions.

```bash
esptool --chip esp32s3 merge_bin -o GOLD_MASTER_FINAL.bin \
  --flash_mode qio --flash_size 16MB --flash_freq 80m --pad-to-size 16MB \
  0x0 build/tmp_bacnet.ino.bootloader.bin \
  0x8000 build/tmp_bacnet.ino.partitions.bin \
  0xe000 /root/.arduino15/packages/esp32/hardware/esp32/3.0.0/tools/partitions/boot_app0.bin \
  0x10000 build/tmp_bacnet.ino.bin
```

## 4. Procédure de Flashage (Anti-Bootloop)
1.  **Erase Full** : `esptool erase_flash` obligatoire pour nettoyer le NVS corrompu.
2.  **Download Mode** : Maintenir BOOT, reset court, relâcher BOOT.
3.  **Write 0x0** : Flasher le binaire fusionné à l'offset 0.
