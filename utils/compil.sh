#!/bin/bash
export PATH=/home/dev/venv/bin:$PATH
echo "Compilation du projet BACnet2MQTT..."
/home/dev/bin/arduino-cli compile \
  --fqbn esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=cdc,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=custom,DebugLevel=debug,PSRAM=opi,LoopCore=1,EventsCore=1,EraseFlash=all,JTAGAdapter=builtin,ZigbeeMode=default \
  --build-property "build.extra_flags=-mfix-esp32-psram-cache-issue -DESP32 -DCORE_DEBUG_LEVEL=4" \
  --build-property "board_build.partitions=partitions.csv" \
  --output-dir /home/dev/bacnet_2_mqtt/build \
  /home/dev/bacnet_2_mqtt/bacnet_2_mqtt.ino
