#!/bin/bash

# Détection du répertoire courant (là où se trouve le script)
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" &> /dev/null && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"

echo "Compilation du projet BACnet2MQTT..."

arduino-cli compile \
  --fqbn esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=cdc,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=custom,DebugLevel=debug,PSRAM=opi,LoopCore=1,EventsCore=1,EraseFlash=all,JTAGAdapter=builtin,ZigbeeMode=default \
  --build-property "build.extra_flags=-mfix-esp32-psram-cache-issue -DESP32 -DCORE_DEBUG_LEVEL=4" \
  --build-property "board_build.partitions=partitions.csv" \
  --output-dir "$BUILD_DIR" \
  "$PROJECT_DIR/bacnet_2_mqtt.ino"

echo "Compilation terminée. Les binaires sont dans : $BUILD_DIR"
