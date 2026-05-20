# Contrat d'Interface Inter-Cœurs : BACnet (Core 1) ↔ MQTT (Core 0)

## Philosophie
Le Core 1 (BACnet MS/TP) est critique en temps réel. Il ne doit **jamais** attendre une opération réseau TCP/WiFi (MQTT). Les deux cœurs communiqueront exclusivement de manière asynchrone via des files d'attente (Queues) FreeRTOS thread-safe.

---
## 0. mise en contexte des directives et utilisation des agents.
Toujours lire le fichier de directives GEMINI.md dans le repertoire ou se situe le plan du projet avant de commencer le plan. il faut respecter et appliquer toutes ses consignes a chaque etape du plan
lister les capacités de ses agents avant de commencer, privilégier leur utilisation a chaque fois que c'est possible. utiliser les experts et les notebooks pour obtenir des reponses a des hypothèses.
Tracer toutes les actions effectuer dans le cadre du plan dans le fichier TRACE.md dans le repertoire ou se situe le plan du projet.

--- 
## 1. Flux Descendant : Commandes (MQTT -> BACnet)
Lorsqu'un utilisateur modifie une valeur sur Home Assistant, l'événement MQTT arrive sur le Core 0. Il est traduit en "Ordre" et mis en file d'attente pour le Core 1.

### File d'attente : `bacnet_job_queue`
- **Producteur** : Core 0 (MQTT Event Handler)
- **Consommateur** : Core 1 (BACnet FSM, lorsqu'il a le jeton MS/TP)
- **Type FreeRTOS** : `QueueHandle_t` (Taille recommandée : 10)

### Structure de Données : `BACnetJob`
```cpp
enum BACnetJobType {
    JOB_WHO_IS,
    JOB_READ_PROP,
    JOB_WRITE_PROP,
    JOB_READ_UNITS,
    JOB_CHECK_COMMANDABLE,
    JOB_READ_STATE_TEXT
};

struct BACnetJob {
    BACnetJobType type;     // Le type d'ordre
    uint8_t target_mac;     // Adresse MAC MS/TP cible
    uint16_t obj_type;      // Type d'objet BACnet (ex: 1 = Analog Output)
    uint32_t obj_instance;  // Instance de l'objet (ex: 0)
    uint8_t prop_id;        // Propriété à lire/écrire (ex: 85 = Present_Value)
    float write_value;      // Valeur à écrire (utilisé si type == JOB_WRITE_PROP)
    uint8_t priority;       // Priorité d'écriture BACnet (ex: 16 par défaut)
};
```

### Méthode d'Interface (API)
```cpp
// Appelée par le Core 0 pour envoyer un ordre au Core 1
bool enqueue_bacnet_job(BACnetJob job);
```

---

## 2. Flux Montant : Télémétrie & Valeurs (BACnet -> MQTT)
Lorsque le Core 1 lit une nouvelle valeur sur le bus RS485, il met à jour le cache en RAM, puis met un "Ordre de publication" en file d'attente pour que le Core 0 l'envoie au Broker MQTT.

### File d'attente : `mqtt_publish_queue`
- **Producteur** : Core 1 (BACnet FSM, lors de la réception d'un Complex-ACK)
- **Consommateur** : Core 0 (Boucle MQTT ou Tâche dédiée)
- **Type FreeRTOS** : `QueueHandle_t` (Taille recommandée : 30, car les réponses arrivent en rafale)

### Structure de Données : `MQTTPublishJob`
Pour éviter de saturer le RAM avec des chaînes de caractères (Topics JSON) dans le Core 1, le Core 1 envoie des données sémantiques. Le Core 0 s'occupera du formatage du Topic et du Payload.

```cpp
enum MQTTPublishDataType {
    PUB_FLOAT,      // Pour les Analog (AI, AO, AV)
    PUB_INT,        // Pour les Multi-State ou Binary
    PUB_STRING      // Pour les textes (State_Text, etc.)
};

struct MQTTPublishJob {
    uint32_t device_id;     // ID de l'automate source (pour le topic)
    uint16_t obj_type;      // Type d'objet (pour le topic)
    uint32_t obj_instance;  // Instance (pour le topic)
    uint8_t prop_id;        // Propriété lue (ex: 85 = Present_Value)
    
    MQTTPublishDataType data_type;
    float value_float;      // Peuplé si data_type == PUB_FLOAT
    int32_t value_int;      // Peuplé si data_type == PUB_INT
    char value_string[32];  // Peuplé si data_type == PUB_STRING
};
```

### Méthode d'Interface (API)
```cpp
// Appelée par le Core 1 pour demander une publication MQTT au Core 0
bool enqueue_mqtt_publish(MQTTPublishJob pubJob);
```

---

## 3. Comportements Exceptionnels et Règles

1. **Queue Pleine (Commandes)** : Si `bacnet_job_queue` est pleine (le bus BACnet est saturé ou l'automate ne répond pas), le Core 0 rejette la nouvelle commande MQTT et loggue une erreur. L'utilisateur devra réessayer.
2. **Queue Pleine (Télémétrie)** : Si `mqtt_publish_queue` est pleine (le WiFi est lent ou le Broker déconnecté), le Core 1 abandonne la publication (sans bloquer). La valeur est quand même à jour dans le cache en RAM (`bacnet_network_cache`), elle sera récupérée au prochain cycle de polling BACnet.
3. **Formatage Topic MQTT (Responsabilité du Core 0)** :
   Le Core 0 lira le `MQTTPublishJob` et construira le topic ainsi :
   `{prefix}/{device_id}/{obj_type_string}/{obj_instance}/state`
   (ex: `bacnet/1234/analog_value/1/state`).
