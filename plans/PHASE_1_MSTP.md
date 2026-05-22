# Plan Détaillé : Phase 1 - Transport MS/TP & Capture

## 🎯 Objectif
Transformer l'ESP32 en un sniffer BACnet MS/TP intelligent capable de valider et d'extraire les trames de données brutes du bus RS485.

## 🛠 Sous-Tâches Techniques

### 1. Fondations Algorithmiques (CRC)
- Implémentation du **CRC8** (pour l'en-tête de trame).
- Implémentation du **CRC16-CCITT** (pour le corps des données).
- *Vérification* : Tests unitaires sur des trames connues.

### 2. Driver Matériel (RS485/UART1)
- Configuration fine de l'UART1 (RX:18, TX:17, RTS:21).
- Réglage du Baudrate (Défaut: 38400, Auto-sensing optionnel).
- Gestion du mode **Half-Duplex** via le pin RTS.

### 3. Machine à États de Capture (FSM)
Développement d'une tâche FreeRTOS dédiée sur le Core 1 :
- **État SEARCH** : Recherche du préambule `0x55 0xFF`.
- **État HEADER** : Capture des 6 octets suivants (Type, Dest, Src, Len, CRC8).
- **État DATA** : Si `Len > 0`, capture des données + CRC16.
- **État VALIDATE** : Vérification des CRC et mise en queue pour le parser.

### 4. Monitoring & Debug (Web Console)
- Affichage du trafic temps réel sur le Dashboard.
- Statistiques de bus : Trames/sec, Taux d'erreurs CRC, Token Rotation Time.

## 🚀 Critères de Succès
1. Voir passer les trames `Token` et `Poll For Master` dans la console.
2. Capturer une trame de type `BACnet-Data` valide (CRC OK).
3. Ne pas impacter la stabilité WiFi pendant la capture intensive.

## 📅 Prochaines Actions (Autonome)
- [ ] Création du fichier `z_mstp_utils.cpp` pour les calculs CRC.
- [ ] Mise à jour de `z_mstp.cpp` avec la machine à états.
- [ ] Flash OTA v3.3 pour tester la réception.
