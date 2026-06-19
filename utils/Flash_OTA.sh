#!/bin/bash

# Détection dynamique du répertoire racine du projet (en remontant d'un niveau depuis utils/)
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." &> /dev/null && pwd)"
FILE="${PROJECT_DIR}/build/bacnet_2_mqtt.ino.bin"

# Paramètres IP et Port avec tes valeurs par défaut
IP=${1:-192.168.1.50}
PORT=${2:-3232}

# Chemin vers espota.py (par défaut ton chemin, mais surchargeable par variable d'environnement)
ESPOTA_PATH=${ESPOTA_PATH:-"/root/.arduino15/packages/esp32/hardware/esp32/3.3.8/tools/espota.py"}

if [ ! -f "$FILE" ]; then
    echo "Erreur : Fichier $FILE introuvable."
    echo "Veuillez compiler d'abord avec : ./utils/compil.sh"
    exit 1
fi

if [ ! -f "$ESPOTA_PATH" ]; then
    echo "Erreur : L'outil espota.py est introuvable à l'emplacement : $ESPOTA_PATH"
    echo "Astuce : Si votre installation Arduino est différente, définissez la variable d'environnement :"
    echo "export ESPOTA_PATH=/chemin/vers/votre/espota.py"
    exit 1
fi

echo "Déploiement OTA de $FILE sur $IP:$PORT..."

# Lancement du flash
python3 "$ESPOTA_PATH" -i "$IP" -p "$PORT" -f "$FILE"

if [ $? -eq 0 ]; then
    echo "Mise à jour OTA terminée avec succès !"
else
    echo "Échec de la mise à jour OTA."
    exit 1
fi
