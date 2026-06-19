# Référence de l'API REST et des Topics MQTT

La passerelle communique de manière transparente et bidirectionnelle via une API REST HTTP et des messages MQTT.

---

## 🌐 API REST (HTTP JSON)

Toutes les requêtes de modification ou lecture nécessitent une authentification HTTP Basic Auth (Identifiants par défaut : `admin` / `admin1234`).

### 1. GET `/api/status`
*   **Description** : Retourne l'état général de la passerelle.
*   **Format de réponse** :
    ```json
    {
      "uptime": 12450,
      "heap": 214580,
      "devices": 1,
      "tasks": { "mstp": "RUNNING", "mqtt": "CONNECTED" }
    }
    ```

### 2. GET `/api/objects`
*   **Description** : Récupère la liste complète des objets BACnet découverts et stockés en mémoire vive (RAM).
*   **Format de réponse** : Un tableau de périphériques contenant leurs objets respectifs :
    ```json
    [
      {
        "device_id": 364004,
        "name": "ECB_203",
        "vendor": "Distech Controls",
        "objects": [
          {
            "type": 2,
            "inst": 27,
            "name": "TempFinale1",
            "val": 25.9,
            "unit": "°C",
            "poll": true,
            "status_flags": 0,
            "outofservice": false,
            "overridden_bacnet": false
          }
        ]
      }
    ]
    ```

### 3. POST `/api/writevalue`
*   **Description** : Envoie une commande d'écriture d'une propriété d'objet vers le bus BACnet MS/TP.
*   **Paramètres (POST)** :
    *   `did` : ID de l'automate BACnet (ex: `364004`).
    *   `type` : Type d'objet (ex: `0=AI`, `1=AO`, `2=AV`, `3=BI`, `4=BO`, `5=BV`, `19=MSV`).
    *   `inst` : Numéro d'instance de l'objet.
    *   `prop` : Propriété BACnet (ex: `85` = Present_Value, `77` = Object_Name).
    *   `val` : Valeur numérique à écrire. Pour libérer la priorité (Relinquish), transmettez le mot-clé `"AUTO"`.
    *   `priority` : Priorité BACnet (Optionnel, `1-16`). La priorité `8` est réservée aux forçages manuels.
    *   `name` : (Optionnel, uniquement si `prop=77`) Chaîne de caractères représentant le nouveau nom.

### 4. POST `/api/outofservice`
*   **Description** : Force ou relâche l'état Out of Service (débrayage local de la sonde physique).
*   **Paramètres (POST)** :
    *   `did` : ID de l'automate BACnet.
    *   `type` : Type d'objet.
    *   `inst` : Instance.
    *   `state` : `"1"`, `"true"`, ou `"on"` pour activer ; `"0"`, `"false"`, ou `"off"` pour désactiver.

---

## 📡 Topics MQTT (Bidirectionnel)

Le préfixe par défaut est `bacnet`.

### 1. Publication d'État (Passerelle → Broker)
Chaque objet publie son état JSON sur le topic suivant dès qu'une variation est détectée ou lors du rafraîchissement périodique :
*   **Topic** : `bacnet/[DeviceID]/[Type]/[Instance]/state`
*   **Payload JSON** :
    ```json
    {
      "val": 25.9,
      "alarm": false,
      "fault": false,
      "overridden": false,
      "oos": false,
      "overridden_bacnet": false
    }
    ```
    *Note : Les objets binaires (BI/BO/BV) publient leur état sous forme d'entier formaté (`"0"` ou `"1"` sans décimales) pour assurer la conformité avec Home Assistant.*

### 2. Publication du Nom (Passerelle → Broker)
*   **Topic** : `bacnet/[DeviceID]/[Type]/[Instance]/name`
*   **Payload** : Nom texte de l'objet (ex: `TempFinale1`).

### 3. Commandes d'Écriture (Broker → Passerelle)
Pour modifier la Present_Value d'un objet (mode standard à priorité 0 ou mode manuel/forcé à priorité 8 selon le statut `manual_op`) :
*   **Topic** : `bacnet/[DeviceID]/[Type]/[Instance]/set`
*   **Payload** : La valeur numérique souhaitée (ex: `22.5`).
*   **Comportement des priorités** :
    *   Si le mode manuel est désactivé (`OFF` / `overridden_bacnet` est `false`) : L'écriture s'effectue en **priorité 0** (sans priorité). La valeur n'est pas verrouillée et le programme interne de l'automate peut l'écraser.
    *   Si le mode manuel est activé (`ON` / `overridden_bacnet` est `true`) : L'écriture s'effectue en **priorité 8** (mode forçage manuel). La valeur est verrouillée sur l'actionneur.
*   **Libération (Relinquish)** : Envoyez le mot-clé `"AUTO"` pour effacer le forçage manuel à la priorité 8, remettre le mode automatique (priorité 0) et restituer le contrôle au programme local de l'automate.

### 4. Commande de Mode Manuel "Manual Operator" (Broker → Passerelle)
Pour forcer manuellement le contrôle ou relâcher un objet commandable (AV, AO, BV, BO, MSV, MSO) :
*   **Topic** : `bacnet/[DeviceID]/[Type]/[Instance]/manual_op/set`
*   **Payload** : 
    *   `"ON"` : Force la valeur actuelle du cache en **priorité 8** et verrouille l'écriture en mode manuel (toutes les écritures suivantes sur `/set` se feront également en priorité 8).
    *   `"OFF"` : Envoie un `AUTO` (Relinquish/NULL) à la **priorité 8** pour libérer le contrôle et repasse l'objet en mode automatique (priorité 0 pour les écritures futures).

### 5. Commandes OutOfService (Broker → Passerelle)
Pour simuler une sonde en mettant l'objet hors-service :
*   **Topic** : `bacnet/[DeviceID]/AI/[Instance]/outofservice/set`
*   **Payload** : `"ON"` pour activer le mode hors-service, `"OFF"` pour repasser en mode automatique.
