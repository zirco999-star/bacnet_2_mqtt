# Intégration dans Home Assistant

La passerelle BACnet2MQTT intègre un moteur d'Auto-Découverte (MQTT Discovery) pour mapper instantanément vos objets de génie climatique dans Home Assistant.

---

## 🎭 Catégories d'Objets et Entités Générées

Pour chaque objet BACnet supervisé, la passerelle crée automatiquement plusieurs entités regroupées sous un même appareil (Device) correspondant à l'automate BACnet. Les objets sont classés en deux catégories logiques :

### 1. Objets Commandables (Catégorie 1 - Avec Priorité)
Ces objets (ex: `Ventilateur`, `VoletAir1`, `VoletAir2`, `VoletAir3`) disposent d'un tableau de priorité BACnet (Priority Array). Ils génèrent :
*   **Entité de commande principale** : 
    *   `number` (ex: `Ventilateur`) : Curseur de commande en écriture (publiée à la priorité 8 de forçage BACnet).
    *   `switch` ou `select` : Selon le type d'objet (binaire ou multi-états).
*   **Forçage Réseau (Forçage BACnet)** :
    *   `sensor` (ex: `Ventilateur Forçage BACnet`) : Affiche `OUI` si la passerelle contrôle l'objet en priorité 8, sinon `NON`.
*   **Bouton de Libération (Reset)** :
    *   `button` (ex: `Ventilateur Reset`) : Envoie la commande `AUTO` (Relinquish) pour effacer le forçage à la priorité 8 et rendre le contrôle à l'automate.

### 2. Objets de Configuration/Consignes (Catégorie 2 - Sans Priorité)
Ces objets (ex: `ConsigneFinale1`, `ConsigneTempEcoETE`, `TempoVolet`, `HighOffsetLimit`) sont des paramètres d'exploitation sans tableau de priorité. Ils génèrent :
*   **Entité de réglage principale** :
    *   `number` (ex: `ConsigneFinale1`) : Curseur de réglage.
    *   `switch` ou `select` : Selon le type d'objet.
    *   *Note d'implémentation* : Contrairement à la catégorie 1, l'écriture sur ces entités s'effectue directement en priorité 0 (sans envoi d'icône de priorité `0x49` ni écriture en priorité 8).
*   **Pas de bouton Reset** : Comme ces objets ne maintiennent pas de tableau de priorité en écriture, ils n'ont pas besoin et ne possèdent pas de bouton de réinitialisation (`button.*_reset`).

### 3. Diagnostics Communs (status_flags)
Tous les objets (Catégories 1 & 2) génèrent des capteurs de diagnostic pour superviser l'état du matériel BACnet :
*   `sensor` Alarme (ex: `TempFinale1 Alarme`) : Indique si la sonde est en alarme (`OUI`/`NON`).
*   `sensor` Défaut (ex: `TempFinale1 Défaut`) : Signalement d'anomalie matérielle (`OUI`/`NON`).
*   `sensor` Forçage Manuel (ex: `TempFinale1 Forçage Manuel`) : Reflète une intervention physique/locale directe sur le commutateur du tableau électrique ou de l'automate.
*   `sensor` Hors Service (ex: `TempFinale1 Hors Service`) : Indique si la sonde physique a été désactivée (permettant la simulation d'une valeur).

---

## 🎨 Cohérence Visuelle et Icônes
Pour assurer une harmonie visuelle entre les entités Home Assistant et l'interface utilisateur :
*   **Icône par défaut pour l'aéraulique (`mdi:fan`)** : Toutes les entités dont le nom contient `ventil` ou `volet` (insensible à la casse, ex: `Ventilateur`, `VoletAir1`, `PoidsVolet1`, `TempoVolet`) se voient attribuer automatiquement l'icône de ventilation `mdi:fan`, cohérente avec l'icône de l'onglet du Dashboard.
*   **Diagnostics de statut** :
    *   Alarme : `mdi:alarm-light`
    *   Défaut : `mdi:alert`
    *   Forçage Manuel (physique) : `mdi:hand-back-right`
    *   Forçage BACnet (réseau) : `mdi:lan-pending`
    *   Hors Service : `mdi:power-plug-off`
    *   Bouton Reset : `mdi:restore`

---

## 🎛️ Scripts Globaux de Contrôle
Pour faciliter l'exploitation globale de l'installation, deux scripts Home Assistant sont configurés sur le Dashboard :
1.  **Réinitialisation Globale des Forçages** (`script.ecb_203_global_reset`) : Libère d'un coup tous les forçages réseau (priorité 8) actifs sur l'ensemble des entités commandables.
2.  **Annulation Globale du Mode Simulation** (`script.ecb_203_global_oos_cancel`) : Remet en service toutes les sondes configurées en Hors Service (OOS) en désactivant le flag de simulation.

---

## 📊 Dashboard Lovelace ECB-203
Un tableau de bord Lovelace complet et moderne est généré pour l'exploitation de l'automate ECB-203. Il applique les règles d'affichage suivantes :
*   **Mode Panel Responsive** : La vue "Zones" utilise le type `panel` occupant tout l'espace d'affichage disponible du navigateur.
*   **Layout en 3 Colonnes** : Les cartes de zones sont réparties de façon homogène sur une grille à 3 colonnes.
*   **Alignement Global / Système** : La section système au bas du dashboard est divisée en 3 colonnes alignées ("Régimes & Ventilation", "Paramètres Système", "Réinitialisation (AUTO)") pour combler l'espace horizontal de manière esthétique.
*   **Rapprochement des Entités** : L'espacement entre le libellé d'entité et son contrôle a été réduit pour conserver une lecture compacte et claire.

---

## 🤖 Exemples de Scénarios d'Automatisation (YAML HA)

### Exemple 1 : Consigne de chauffe forcée par calendrier et libérée ensuite (Catégorie 1)
```yaml
alias: "Régulation Chauffage Bureaux"
description: "Force la consigne la journée et libère le contrôle la nuit"
trigger:
  - platform: time
    at: "08:00:00"
    id: "matin"
  - platform: time
    at: "17:00:00"
    id: "soir"
action:
  - choose:
      - conditions:
          - condition: trigger
            id: "matin"
        sequence:
          # Force la consigne à 21°C (Écrit en priorité 8)
          - service: number.set_value
            target:
              entity_id: number.ecb_203_ventilateur
            data:
              value: 21
      - conditions:
          - condition: trigger
            id: "soir"
        sequence:
          # Libère le contrôle réseau (Relinquish) en pressant le bouton Reset
          - service: button.press
            target:
              entity_id: button.ecb_203_ventilateur_reset
```

### Exemple 2 : Modification directe via MQTT
*   **Actionneur (Catégorie 1 - Forçage à la priorité 8)** :
    *   Topic: `bacnet/364004/AV/35/set`
    *   Payload: `20.5`
*   **Libération de l'actionneur (Retour mode auto)** :
    *   Topic: `bacnet/364004/AV/35/set`
    *   Payload: `AUTO`
*   **Consigne/Paramètre (Catégorie 2 - Écriture directe priorité 0)** :
    *   Topic: `bacnet/364004/AV/8/set`
    *   Payload: `19.0`
