# Suivi du Projet : BACnetMSTP2MQTT

## Historique des Versions et Stabilisation

### Phase 0 : Infrastructure (Terminee)
- v2.3.0 - v3.9.2 : Stabilisation du socle WiFi, NVS, OTA (Partition min_spiffs) et UI Industrielle.

### Phase 1 : Transport MS/TP et Ronde de Jetons
- v4.0.0 - v4.2.6 : Implementation CRC8/16, FSM initiale, essais de pilotage RTS manuel (GPIO 21).
- v4.2.9 - v4.2.10 : Modularisation du code. Apparition du probleme de "surdite" (RX=0).
- v4.2.11 - v4.2.13 : PERCEE TECHNIQUE. Identification du conflit GPIO 47 (OPI PSRAM). Le retrait du GPIO 47 restaure la reception UART.
- v4.2.14 : REFERENCE STABLE. Implementation du mode UART_MODE_RS485_HALF_DUPLEX natif (Auto-RTS). Ronde de jetons 1 <-> 4 parfaitement stable.

### Phase 2 : Decouverte Automatique des Objets
- v4.2.15-Phase 1.5 : Succes de la lecture de l'Object_List (index 0). L'automate ECB-203 (MAC 4) confirme la presence de 98 objets.
- v4.2.15-Phase 2.4 : Tentative de lecture sequentielle bloquante. Echec (blocage du jeton > 20ms, exclusion de la ronde).
- v4.2.24.2 : VERSION ACTUELLE. Implementation de la strategie "YABE Clone" :
    - Polling lent (1 requete tous les 20 jetons).
    - Attente non-bloquante de 280ms pour capturer la reponse lente de l'automate.
    - Enumeration automatique des 98 objets en cours (40/98 decouverts a l'arret de la session).

## Etat de la Main
- Branche main : Contient le commit v4.2.14 (Ring Reference).
- Version active : v4.2.24.2 (Discovery active).
- IP Fixe : 192.168.1.50
- MAC Adresse : 1 (ESP32) <-> 4 (ECB-203).

## Prochain Objectif : Phase 3 - Monitoring et MQTT
1. Finaliser l'enumeration des 98 objets (automatique au prochain boot).
2. Implémenter la boucle de lecture Present_Value (Prop 85) pour tous les objets decouverts.
3. Publier les valeurs sur MQTT (192.168.1.11).

## Notes pour l'Agent (Session Suivante)
1. Timing CRITIQUE : L'automate ECB-203 met ~240ms a repondre. Toujours maintenir l'attente de 280ms sans bloquer la boucle uart_read_bytes.
2. PSRAM Warning : Ne JAMAIS toucher au GPIO 47 (bus memoire Octal SPI).
3. Logs : Utiliser listen_logs_v2.py via le venv pour le monitoring.

### 2026-05-19 - v4.2.25 "Master Discovery"
- **Problème** : Blocage du scan à l'index 62/98. Confusion entre la valeur 0x3E (62) et l'Opening Tag 3 (0x3E).
- **Solution** : Implémentation d'un parser ASN.1 robuste basé sur la consommation TLV (Tag-Length-Value) contextuelle.
- **Détails** : 
  - Ajout de la structure `BACnetTag` et de la fonction `get_tag()`.
  - Gestion des tags étendus et des longueurs étendues.
  - Séquençage strict de l'APDU Complex-ACK.
  - Validation via l'expert NotebookLM fe92515b.
- **Action** : Déploiement OTA et vérification du franchissement de l'index 62.

### Phase 2.25 : Master Discovery - Parser ASN.1 TLV (v4.2.25)
- Action : Implementation du parser TLV robuste pour gerer l ambiguite de l index 62 (0x3E).
- Expertise : Validation de la structure recursive par NotebookLM fe92515b.
- Deploiement : OTA via espota.py.
- Resultat attendu : Enumeration complete des 98 objets sans decalage d octets.

### Phase 2.28 : VICTOIRE - Scan Complet 98/98 (v4.2.28)
- Action : Integration du "Master Parser" TLV ASHRAE 135 conforme.
- Resultat : Franchissement de l index 62 (0x3E) reussi.
- Etat final : 98 objets decouverts et listes dans le cache.
- Stabilite : Ring 1 <-> 4 maintenu pendant tout le scan (attente 280ms non-bloquante).
