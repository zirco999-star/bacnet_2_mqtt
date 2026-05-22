# Plan d'Implémentation : Intégration du client ESP-MQTT (Espressif)

## Objectif
Remplacer la bibliothèque synchrone et limitée `PubSubClient` par le client officiel Espressif `esp-mqtt` (basé sur ESP-IDF). Ce changement apporte une architecture totalement asynchrone (le client tourne dans sa propre tâche FreeRTOS), une meilleure gestion de la mémoire dynamique pour les gros payloads, et un support natif robuste des QoS et des reconnexions.

## 0. mise en contexte des directives et utilisation des agents.
Toujours lire le fichier de directives GEMINI.md dans le repertoire ou se situe le plan du projet avant de commencer le plan. il faut respecter et appliquer toutes ses consignes a chaque etape du plan
lister les capacités de ses agents avant de commencer, privilégier leur utilisation a chaque fois que c'est possible. utiliser les experts et les notebooks pour obtenir des reponses a des hypothèses.
Tracer toutes les actions effectuer dans le cadre du plan dans le fichier TRACE.md dans le repertoire ou se situe le plan du projet.
  

## 1. Nettoyage de l'existant (`z_config.h` & `z_config.cpp`)
- **Suppression** : Retirer `#include <PubSubClient.h>` et l'objet global `PubSubClient mqttClient`.
- **Ajout** : Inclure `#include "mqtt_client.h"` (le header ESP-IDF).
- **Déclaration** : Créer le handle global `esp_mqtt_client_handle_t mqtt_client;`.

## 2. Refonte du module MQTT (`z_mqtt.h` & `z_mqtt.cpp`)
L'API `esp-mqtt` est pilotée par les événements. Nous devons revoir l'initialisation et la boucle principale.

- **Configuration (`setup_mqtt`)** :
  - Créer la configuration `esp_mqtt_client_config_t mqtt_cfg = {};`.
  - Construire l'URI : `mqtt://<user>:<pass>@<server>:<port>`.
  - Initialiser : `mqtt_client = esp_mqtt_client_init(&mqtt_cfg);`.
  - Enregistrer le handler : `esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);`.
  - Démarrer la tâche asynchrone : `esp_mqtt_client_start(mqtt_client);`.

- **Event Handler (`mqtt_event_handler`)** :
  C'est ici que toute la logique réseau se déroulera :
  - `MQTT_EVENT_CONNECTED` : Le client est en ligne. Il doit s'abonner aux topics d'écriture BACnet. Exemple de filtre MQTT : `<prefix>/+/+/+/set`.
  - `MQTT_EVENT_DISCONNECTED` : Logguer la perte de connexion (la reconnexion est gérée nativement par `esp-mqtt`).
  - `MQTT_EVENT_DATA` : Un message est reçu sur un topic souscrit. Le payload contient la nouvelle valeur, et le topic identifie la cible.

- **Boucle Principale (`handle_mqtt`)** :
  - Comme `esp-mqtt` gère le réseau en arrière-plan (task FreeRTOS), l'appel répété dans la boucle `loop()` (`mqttClient.loop()`) doit être supprimé. La fonction `handle_mqtt()` pourra servir à des diagnostics ou être supprimée.

## 3. Lien Bidirectionnel avec le BACnet (`z_bacnet.cpp` <-> MQTT)

### A. BACnet -> MQTT (Publication)
Lorsqu'un objet est lu sur le bus MS/TP (fin d'un `JOB_READ_PROP`), sa valeur est mise à jour en RAM.
- **Action** : Appeler une nouvelle fonction `mqtt_publish_state(device_id, obj_type, instance, value)`.
- **Implémentation** : Cette fonction utilisera `esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 0)` pour envoyer la donnée au broker de manière asynchrone.

### B. MQTT -> BACnet (Écriture)
L'utilisateur change une valeur depuis Home Assistant, qui publie sur un topic `/set`.
- **Action** : L'événement `MQTT_EVENT_DATA` intercepte le message.
- **Logique** : 
  1. Parser le nom du topic pour extraire le MAC cible (ou Device ID), le type d'objet (ex: `analog_value`), et l'instance.
  2. Parser le payload pour extraire le `write_value` (Float ou Enum).
  3. Générer un objet `BACnetJob` de type `JOB_WRITE_PROP`.
  4. L'injecter dans la queue `bacnet_job_queue` avec `enqueue_bacnet_job()`.
  5. Le Core 1 (qui gère la FSM MS/TP) dépilera ce Job et enverra le télégramme BACnet `WriteProperty` dès qu'il aura le jeton.

## 4. Tests et Déploiement
1. Ajuster les dépendances de compilation (`platformio.ini` ou la chaîne CMake) si `/lib/esp-mqtt` doit être forcé par rapport au composant ESP-IDF intégré de base.
2. Vérifier que la compilation de l'interface `esp_mqtt_client_config_t` est compatible avec la version d'ESP-IDF sous-jacente au framework Arduino utilisé.
3. Surveiller les fuites mémoires (Event Handler) lors d'un flux intensif de publications (scan BACnet complet).
