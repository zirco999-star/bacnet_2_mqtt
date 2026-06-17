/**
 * @file z_mqtt.h
 * @brief Interface du module MQTT et intégration Home Assistant.
 *
 * Ce fichier déclare les structures, la file de publication et les
 * prototypes nécessaires à la communication MQTT du gateway BACnet2MQTT.
 *
 * @details
 * Architecture de publication :
 *   1. Le moteur BACnet (Core 1) ou l'UI web (Core 0) crée un MQTTPublishJob.
 *   2. Le job est enfilé dans mqtt_publish_queue (FreeRTOS Queue inter-cœurs).
 *   3. La tâche MQTT (Core 0) dépile et publie via le client ESP-IDF.
 *
 * Intégration Home Assistant :
 *   - L'auto-discovery publie des topics de configuration sur
 *     homeassistant/<component>/<unique_id>/config au format JSON.
 *   - Le type de composant HA (sensor, select, switch, number) est déterminé
 *     automatiquement selon le type d'objet BACnet et ses propriétés.
 *   - unpublish_ha_discovery() envoie un payload vide pour supprimer
 *     les entités fantômes lors d'un changement de type ou de préfixe.
 *
 * @note L'authentification MQTT (user/pass) est optionnelle et configurée
 *       dans sysCfg (z_config.h).
 *
 * @see z_bacnet.h  Pour la structure BACnetObject utilisée dans les publications.
 * @see z_config.h  Pour les paramètres MQTT (serveur, port, préfixe, etc.).
 */
#ifndef Z_MQTT_H
#define Z_MQTT_H
#include "z_config.h"
#include "z_bacnet.h" // Pour avoir accès à la struct BACnetObject

/**
 * @brief Structure décrivant un travail de publication MQTT.
 *
 * @details Transportée via la FreeRTOS Queue mqtt_publish_queue.
 * Permet de découpler la production (Core 1 / BACnet) de la consommation
 * (Core 0 / client MQTT ESP-IDF) pour éviter les blocages inter-cœurs.
 */
struct MQTTPublishJob {
    uint32_t ulDeviceId;       ///< Identifiant du device BACnet source.
    uint16_t obj_type;         ///< Type d'objet BACnet (pour construire le topic).
    uint32_t obj_instance;     ///< Instance de l'objet (pour construire le topic).
    uint8_t prop_id;           ///< Identifiant de la propriété publiée (85=Present_Value, etc.).
    char value_string[128];    ///< Valeur sérialisée en JSON ou chaîne pour le payload MQTT.
    bool retain;               ///< true pour publier avec le flag MQTT RETAIN (persistance broker).
};

/** @brief File d'attente FreeRTOS pour les publications MQTT inter-cœurs.
 *  @details Producteur : Core 1 (polling BACnet) ou Core 0 (UI).
 *           Consommateur : tâche MQTT sur Core 0. */
extern QueueHandle_t mqtt_publish_queue;

/**
 * @brief Initialise le client MQTT ESP-IDF et établit la connexion au broker.
 * @details Appelée depuis setup() après la connexion WiFi.
 */
void setup_mqtt();

/**
 * @brief Boucle de traitement MQTT : dépile les jobs et gère les reconnexions.
 * @details Appelée périodiquement depuis la boucle principale (Core 0).
 */
void handle_mqtt();

/**
 * @brief Vérifie si le client MQTT est actuellement connecté au broker.
 * @return true si la connexion est établie et opérationnelle.
 */
bool is_mqtt_connected();

/**
 * @brief Crée la file d'attente FreeRTOS pour les publications MQTT.
 * @details Doit être appelée avant setup_mqtt() et tout enqueue_mqtt_publish().
 */
void init_mqtt_queue();

/**
 * @brief Ajoute un travail de publication dans la file MQTT.
 * @param pubJob Structure MQTTPublishJob décrivant la publication à effectuer.
 * @return true si le job a été ajouté avec succès, false si la file est pleine.
 */
bool enqueue_mqtt_publish(MQTTPublishJob pubJob);

// ============================================================================
// PUBLICATION CENTRALISÉE ET AUTO-DISCOVERY HOME ASSISTANT
// ============================================================================

/**
 * @brief Publie la valeur d'une propriété BACnet sur le topic MQTT correspondant.
 * @param ulDeviceId Identifiant du device BACnet propriétaire.
 * @param obj        Référence vers l'objet BACnet à publier.
 * @param prop_id    Identifiant de la propriété publiée.
 * @param retain     true pour activer le flag RETAIN sur le message.
 */
void publish_mqtt_topic(uint32_t ulDeviceId, BACnetObject& obj, uint8_t prop_id, bool retain);

/**
 * @brief Publie les configurations d'auto-discovery Home Assistant.
 * @param did  Identifiant du device (0 = tous les devices).
 * @param inst Instance de l'objet (0xFFFFFFFF = tous les objets).
 * @param type Type d'objet (0xFFFF = tous les types).
 * @details Publie sur homeassistant/<component>/<unique_id>/config avec
 *          un payload JSON conforme au protocole MQTT Discovery de HA.
 */
void publish_ha_autodiscovery(uint32_t did = 0, uint32_t inst = 0xFFFFFFFF, uint16_t type = 0xFFFF);

/**
 * @brief Supprime les entités Home Assistant en publiant un payload vide.
 * @param did        Identifiant du device (0 = tous).
 * @param inst       Instance de l'objet (0xFFFFFFFF = tous).
 * @param type       Type d'objet (0xFFFF = tous).
 * @param old_prefix Ancien préfixe MQTT (NULL = utiliser le préfixe courant).
 * @details Utilisée pour nettoyer les entités fantômes lors d'un changement
 *          de composant HA (sensor → select) ou de préfixe MQTT.
 */
void unpublish_ha_discovery(uint32_t did = 0, uint32_t inst = 0xFFFFFFFF, uint16_t type = 0xFFFF, const char* old_prefix = NULL);

/**
 * @brief Déclenche une re-publication sélective de l'auto-discovery HA.
 * @param did  Identifiant du device (0 = tous).
 * @param inst Instance de l'objet (0xFFFFFFFF = tous).
 * @param type Type d'objet (0xFFFF = tous).
 * @details Combine unpublish + publish pour garantir la cohérence des entités.
 */
void trigger_ha_discovery(uint32_t did = 0, uint32_t inst = 0xFFFFFFFF, uint16_t type = 0xFFFF);

#endif
