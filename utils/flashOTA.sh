#!/bin/bash
export PATH=/home/dev/venv/bin:$PATH
IP=${1:-192.168.1.50}
PORT=${2:-3232}
FILE="/home/dev/bacnet_2_mqtt/build/bacnet_2_mqtt.ino.bin"
if [ ! -f "$FILE" ]; then
    echo "Erreur: Fichier $FILE introuvable. Compilez d'abord avec ./utils/compil.sh"
    exit 1
fi
echo "Déploiement OTA sur $IP:$PORT..."
/home/dev/venv/bin/python3 /root/.arduino15/packages/esp32/hardware/esp32/3.3.8/tools/espota.py -i $IP -p $PORT -f $FILE
