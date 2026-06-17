/**
 * @file z_nvs.h
 * @brief Interface de persistance NVS (Non-Volatile Storage) pour la configuration et les données BACnet.
 *
 * Ce fichier déclare les fonctions de lecture/écriture vers la partition NVS
 * de l'ESP32, utilisée pour persister :
 *   - La configuration système (struct Config via Preferences).
 *   - Les devices et objets BACnet découverts (format binaire compact, cf. z_bacnet.h).
 *   - Les textes d'états des objets Multi-State (State_Text, Prop 110).
 *
 * @details
 * Stratégie de persistance :
 *   - **Configuration** : Sérialisée champ par champ via l'API Preferences
 *     (clé/valeur). Rechargée intégralement au démarrage.
 *   - **Devices/Objets** : Stockés en blobs binaires paginés (BACnetPersistencePage,
 *     20 objets × 95 octets = 1900 octets par page, limite NVS ~1984 octets).
 *   - **State_Text** : Persistés séparément car de taille variable (vector<String>).
 *
 * Sauvegarde différée (v6.8.8) :
 *   Pour limiter l'usure de la flash NVS, les sauvegardes d'objets ne sont
 *   déclenchées que lorsque le drapeau xDirty du device est actif ET que
 *   ulLastDirtyTime dépasse un seuil de temporisation.
 *
 * @warning Les écritures NVS sont des opérations bloquantes. Les fonctions
 *          save_* ne doivent PAS être appelées depuis le Core 1 (FSM temps réel)
 *          sans passer par le mécanisme de sauvegarde différée.
 *
 * @see z_config.h   Pour la structure Config sauvegardée/restaurée.
 * @see z_bacnet.h   Pour les structures de persistance binaire (BACnetPersistenceObj/Dev/Page).
 */
#ifndef Z_NVS_H
#define Z_NVS_H

#include <Preferences.h>
#include "z_config.h"

/**
 * @brief Charge la configuration système depuis la NVS dans sysCfg.
 * @details Lit chaque paramètre via l'API Preferences. Les champs absents
 *          sont initialisés avec les valeurs DEFAULT_* de z_config.h.
 *          Appelée une seule fois au démarrage dans setup().
 */
void load_configuration();

/**
 * @brief Sauvegarde la configuration système de sysCfg vers la NVS.
 * @details Écrit chaque paramètre de la structure Config via l'API Preferences.
 *          Appelée après modification depuis l'interface web (API /config).
 */
void save_configuration();

/**
 * @brief Charge les objets d'un device BACnet depuis la NVS vers le cache RAM.
 * @param ulDeviceId Identifiant unique du device BACnet dont les objets sont à charger.
 * @details Lit les pages binaires (BACnetPersistencePage) et reconstruit
 *          le vecteur objects du BACnetDevice correspondant dans bacnet_network_cache.
 *          Prend le cache_mutex en interne pour protéger l'accès.
 */
void load_device_objects(uint32_t ulDeviceId);

/**
 * @brief Sauvegarde les objets d'un device BACnet du cache RAM vers la NVS.
 * @param ulDeviceId Identifiant unique du device BACnet dont les objets sont à sauvegarder.
 * @details Sérialise les objets en pages binaires (BACnetPersistencePage) et
 *          les écrit comme blobs NVS. Prend le cache_mutex en interne.
 *
 * @note Cette fonction acquiert cache_mutex elle-même. Ne pas l'appeler
 *       si le mutex est déjà détenu — utiliser save_device_objects_locked().
 */
void save_device_objects(uint32_t ulDeviceId);

/**
 * @brief Sauvegarde les objets d'un device — version sans acquisition de mutex.
 * @param ulDeviceId Identifiant unique du device BACnet.
 * @details Identique à save_device_objects() mais n'acquiert PAS cache_mutex.
 *          À utiliser UNIQUEMENT quand le mutex est déjà détenu par l'appelant
 *          (ex: depuis un bloc xSemaphoreTake existant).
 *
 * @warning Appeler cette fonction sans détenir cache_mutex provoquera
 *          des corruptions de données inter-cœurs.
 */
void save_device_objects_locked(uint32_t ulDeviceId);

/**
 * @brief Sauvegarde les textes d'états d'un objet Multi-State en NVS.
 * @param ulDeviceId Identifiant du device propriétaire.
 * @param type       Type d'objet BACnet (13=MSI, 14=MSO, 19=MSV).
 * @param ulInstance Instance de l'objet.
 * @param states     Vecteur des textes d'états (State_Text, Prop 110).
 * @details Persistés séparément des objets car de taille variable.
 *          La clé NVS est construite à partir de did/type/instance.
 */
void save_object_states(uint32_t ulDeviceId, uint16_t type, uint32_t ulInstance, const std::vector<String>& states);

/**
 * @brief Charge les textes d'états d'un objet Multi-State depuis la NVS.
 * @param ulDeviceId Identifiant du device propriétaire.
 * @param type       Type d'objet BACnet (13=MSI, 14=MSO, 19=MSV).
 * @param ulInstance Instance de l'objet.
 * @param states     Vecteur de sortie pour les textes d'états chargés.
 * @details Reconstruit le vecteur state_texts de l'objet BACnetObject
 *          à partir du blob NVS.
 */
void load_object_states(uint32_t ulDeviceId, uint16_t type, uint32_t ulInstance, std::vector<String>& states);

#endif
