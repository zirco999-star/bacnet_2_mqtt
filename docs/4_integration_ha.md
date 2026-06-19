# Intégration dans Home Assistant & Dashboard Lovelace

La passerelle BACnet2MQTT intègre un moteur d'Auto-Découverte (MQTT Discovery) pour mapper instantanément vos objets de génie climatique et automates BACnet dans Home Assistant.

---

## 🎭 Catégories d'Objets et Entités Générées

Pour chaque objet BACnet supervisé, la passerelle crée automatiquement plusieurs entités sous un appareil (Device) correspondant à l'automate BACnet. Les objets sont classés en deux catégories :

### 1. Objets Commandables (Catégorie 1 - Avec Priorité)
Ces objets (ex: `Ventilateur`, `PositionVolet`) disposent d'un tableau de priorité BACnet (Priority Array). Ils génèrent :
*   **Entité de commande principale** : 
    *   `number`, `switch`, ou `select` (selon le type d'objet) : Curseur ou sélecteur de commande.
    *   *Comportement des priorités* : Par défaut, lorsque le mode manuel est désactivé (`OFF`), les écritures s'effectuent à la **priorité 0** (mode standard/auto) afin de ne pas verrouiller l'automate. Si le mode manuel est activé (`ON`), les écritures s'effectuent à la **priorité 8** (mode forçage manuel).
*   **Commutateur de Forçage (Manual Operator)** :
    *   `switch` (ex: `[Nom]_manual_operator`, icône `mdi:hand-pointing-right`) : Permet de basculer l'objet en mode manuel.
        *   Togglé à `ON` : Verrouille la valeur courante à la **priorité 8**.
        *   Togglé à `OFF` : Envoie `AUTO` (Relinquish) à la **priorité 8** pour libérer le verrou et redonner le contrôle au programme interne de l'automate.
*   **Forçage Réseau (Forçage BACnet)** :
    *   `sensor` (ex: `[Nom]_forcage_bacnet`) : Affiche `OUI` si la passerelle contrôle l'objet en priorité 8, sinon `NON`.
*   **Bouton de Libération (Reset)** :
    *   `button` (ex: `[Nom]_reset`) : Bouton à impulsion qui envoie la commande `AUTO` (Relinquish) pour effacer le forçage à la priorité 8 et repasser le commutateur `Manual Operator` à `OFF`.

### 2. Objets de Configuration/Consignes (Catégorie 2 - Sans Priorité)
Ces objets (ex: `ConsigneTempEco`, `TempoVolets`) sont des paramètres d'exploitation sans tableau de priorité. Ils génèrent :
*   **Entité de réglage principale** :
    *   `number`, `switch` ou `select` (selon le type d'objet) : Réglage de la consigne.
    *   *Note d'implémentation* : Contrairement à la catégorie 1, l'écriture s'effectue toujours directement en **priorité 0** (sans envoi d'icône de priorité `0x49` ni écriture en priorité 8).
*   **Pas de bouton Reset ni de switch Manual Operator** : Comme ces objets ne maintiennent pas de tableau de priorité en écriture, ils n'ont pas de bouton de réinitialisation ni de commutateur de mode manuel.

### 3. Diagnostics Communs (status_flags)
Tous les objets (Catégories 1 & 2) génèrent des capteurs de diagnostic pour superviser l'état du matériel BACnet :
*   `sensor` Alarme (ex: `[Nom]_alarme`) : Indique si la sonde est en alarme (`OUI`/`NON`).
*   `sensor` Défaut (ex: `[Nom]_defaut`) : Signalement d'anomalie matérielle (`OUI`/`NON`).
*   `sensor` Forçage Manuel (ex: `[Nom]_forcage_manuel`) : Reflète une intervention physique/locale directe sur le commutateur du tableau électrique ou de l'automate.
*   `sensor` Hors Service (ex: `[Nom]_hors_service`) : Indique si la sonde physique a été désactivée localement (permettant la simulation d'une valeur).

---

## 🎨 Cohérence Visuelle et Icônes

Pour assurer une harmonie visuelle entre les entités Home Assistant et l'interface utilisateur :
*   **Icône pour l'aéraulique (`mdi:fan`)** : Toutes les entités dont le nom contient `ventil` ou `volet` (ex: `Ventilateur`, `VoletAir`, `PoidsVolet`) se voient attribuer automatiquement l'icône de ventilation `mdi:fan`.
*   **Diagnostics de statut** :
    *   Alarme : `mdi:alarm-light`
    *   Défaut : `mdi:alert`
    *   Forçage Manuel (physique) : `mdi:hand-back-right`
    *   Forçage BACnet (réseau) : `mdi:lan-pending`
    *   Hors Service : `mdi:power-plug-off`
    *   Bouton Reset : `mdi:restore`

---

## 📊 Dashboard Lovelace Générique B2M

La passerelle fournit un script de génération interactif et générique pour créer automatiquement un tableau de bord Lovelace complet sous Home Assistant.

### ⚙️ Génération du Dashboard
Exécutez le script suivant dans votre environnement de développement :
```bash
python3 utils/update_ha_dashboards.py
```
Le script vous demandera :
1. L'URL de votre instance Home Assistant.
2. Votre jeton d'accès à longue durée (Long-Lived Access Token).
3. L'adresse IP de votre passerelle BACnet2MQTT.

Il interroge ensuite la passerelle (ou l'instance HA) pour découvrir les devices et générer la configuration Lovelace associée.

### 📐 Structure du Dashboard
*   **Onglet Accueil** :
    *   **Informations de la passerelle** : Version, uptime, niveau de signal Wi-Fi RSSI, température CPU, état du bus MS/TP et nombre de devices vus.
    *   **Actionneur de redémarrage** : Un bouton pour rebooter la passerelle intégrant un message de confirmation natif Lovelace (OUI/NON) pour éviter les fausses manipulations.
    *   **Status global** : Une carte recensant le nombre d'alarmes, défauts matériels ou forçages réseau actifs sur l'ensemble du parc.
*   **Onglets Équipements** : Crée un onglet par automate BACnet détecté (ex: `ECB_203`).
*   **Blocs par Types d'Objets** : Les entités de chaque appareil sont regroupées dans des blocs horizontaux occupant toute la largeur, classés par type d'objet BACnet : `AI` (Entrées Analogiques), `AO` (Sorties Analogiques), `AV` (Valeurs Analogiques), `BI`/`BO`/`BV` (Binaires) et `MSV` (Multi-États).
*   **Grille à 2 Colonnes** : À l'intérieur de ces blocs, les objets sont disposés sur 2 colonnes sous forme de cartes d'entités contenant leurs commandes (valeur, mode manuel, resets) et attributs de diagnostic.

---

## 🤖 Exemples de Scénarios d'Automatisation (YAML HA)

### Exemple 1 : Consigne de vitesse forcie temporairement (Catégorie 1)
```yaml
alias: "Régulation Vitesse Ventilation Bureau"
description: "Force la vitesse en journée et repasse en auto la nuit"
trigger:
  - platform: time
    at: "08:00:00"
    id: "matin"
  - platform: time
    at: "18:00:00"
    id: "soir"
action:
  - choose:
      - conditions:
          - condition: trigger
            id: "matin"
        sequence:
          # Force le ventilateur à 80% (Priorité 8)
          - service: number.set_value
            target:
              entity_id: number.mon_device_ventilateur
            data:
              value: 80
      - conditions:
          - condition: trigger
            id: "soir"
        sequence:
          # Libère le contrôle (Relinquish) en pressant le bouton Reset
          - service: button.press
            target:
              entity_id: button.mon_device_ventilateur_reset
```

### Exemple 2 : Écritures Directes via MQTT
*   **Actionneur (Catégorie 1 - Forçage à la priorité 8)** :
    *   Topic: `bacnet/[DeviceID]/AO/[Instance]/set` (ou `AV`)
    *   Payload: `22.5`
*   **Libération de l'actionneur (Retour en AUTO)** :
    *   Topic: `bacnet/[DeviceID]/AO/[Instance]/set`
    *   Payload: `AUTO`
*   **Consigne/Paramètre (Catégorie 2 - Écriture directe priorité 0)** :
    *   Topic: `bacnet/[DeviceID]/AV/[Instance]/set`
    *   Payload: `19.0`

---

## 💻 Environnement de Test & Matériel validé

Toutes les fonctionnalités d'intégration, d'Auto-Découverte et de dashboard ont été testées et validées avec le matériel suivant :
*   **Passerelle** : [Waveshare ESP32-S3-RS485-CAN](https://www.waveshare.com/esp32-s3-rs485-can.htm) (avec PSRAM active).
*   **Automate de test cible** : Automate programmable **Distech Controls ECB-203** (Régulation d'UTA verticale), connecté sur le segment RS-485 BACnet MS/TP.
*   **Broker MQTT** : Mosquitto exécuté localement.
