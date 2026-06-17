# Intégration dans Home Assistant

La passerelle BACnet2MQTT intègre un moteur d'Auto-Découverte (MQTT Discovery) pour mapper instantanément vos objets de génie climatique dans Home Assistant.

---

## 🎭 Entités générées par Objet BACnet

Pour chaque objet BACnet surveillé, la passerelle crée automatiquement plusieurs entités regroupées sous un même appareil (Device) correspondant à l'automate BACnet :

1.  **Entité Principale** :
    *   `sensor` (ex: `TempFinale1`) : Pour les entrées analogiques (AI).
    *   `number` (ex: `OffsetFinal1`) : Curseur de réglage pour les sorties et valeurs analogiques (AO/AV).
    *   `switch` (ex: `DemandeChaud1`) : Bouton à bascule pour les objets binaires (BI/BO/BV).
    *   `select` (ex: `ModeConfortEco`) : Menu déroulant pour les objets multi-états (MSI/MSO/MSV).
2.  **Diagnostics de Statut (status_flags)** :
    *   `sensor` Alarme (ex: `TempFinale1 Alarme`) : Indique si la sonde est en alarme (`OUI`/`NON`).
    *   `sensor` Défaut (ex: `TempFinale1 Défaut`) : Signalement d'anomalie matérielle (`OUI`/`NON`).
    *   `sensor` Forçage Manuel (ex: `TempFinale1 Forçage Manuel`) : Reflète une intervention physique **physique/locale** sur le commutateur du tableau électrique ou de l'automate.
    *   `sensor` Hors Service (ex: `TempFinale1 Hors Service`) : Indique si la sonde physique a été désactivée.
3.  **Forçage Réseau Passerelle** :
    *   `sensor` Forçage BACnet (ex: `TempFinale1 Forçage BACnet`) : Passe à `OUI` lorsque la passerelle a écrit activement une valeur sur le réseau en priorité 8.
4.  **Libération du Forçage (Reset)** :
    *   `button` Reset (ex: `TempFinale1 Reset`) : Permet de libérer immédiatement le forçage réseau (Relinquish). Appuyer sur ce bouton envoie la commande `"AUTO"` pour rendre le contrôle au programme automatique de l'automate.

---

## 🎛️ Logique de fonctionnement des forçages

```
[HA UI (Number / Switch)] ──(Écriture valeur)──> [Écriture Priorité 8 (Forçage BACnet = OUI)]
                                                               │
[HA UI (Button Reset)]    ──(Envoi "AUTO")     ──> [Relinquish Priorité 8 (Forçage BACnet = NON)]
```

*   **Forçage Manuel** (lu depuis le PLC) : Indique un commutateur physique local activé par un technicien dans l'armoire électrique.
*   **Forçage BACnet** (géré par la passerelle) : Indique que la passerelle a pris la main en priorité 8 (par exemple suite à une consigne envoyée par Home Assistant).
*   **Bouton Reset** : Toujours utiliser le bouton `Reset` pour libérer la consigne et permettre à l'automate de reprendre sa régulation autonome.

---

## 🤖 Exemples de Scénarios d'Automatisation (YAML HA)

### Exemple 1 : Consigne de chauffe forcée par calendrier et libérée ensuite

Cet exemple force la consigne de température à 21°C à 8h00, puis libère le contrôle (Relinquish) à 17h00 pour laisser l'automate appliquer son mode éco de nuit.

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
              entity_id: number.consignefinale1
            data:
              value: 21
      - conditions:
          - condition: trigger
            id: "soir"
        sequence:
          # Libère le contrôle réseau (Relinquish) en pressant le bouton Reset
          - service: button.press
            target:
              entity_id: button.consignefinale1_reset
```

### Exemple 2 : Commande directe via MQTT (Node-RED ou scripts externes)

Pour modifier la valeur directement depuis le protocole MQTT sans passer par l'UI de Home Assistant :

*   **Actionneur (Forçage 20.5°C)** :
    *   Topic: `bacnet/364004/AV/35/set`
    *   Payload: `20.5`
*   **Libération (Relinquish / Retour mode auto)** :
    *   Topic: `bacnet/364004/AV/35/set`
    *   Payload: `AUTO`

### Exemple 3 : Commande directe via API REST (CURL)

*   **Activer un forçage manuel (priorité 8) à 22°C** :
    ```bash
    curl -u admin:admin1234 -X POST -d "did=364004&type=2&inst=35&prop=85&val=22&priority=8" http://192.168.1.50/api/writevalue
    ```

*   **Restituer le contrôle à l'automate (Relinquish en priorité 8)** :
    ```bash
    curl -u admin:admin1234 -X POST -d "did=364004&type=2&inst=35&prop=85&val=AUTO&priority=8" http://192.168.1.50/api/writevalue
    ```
