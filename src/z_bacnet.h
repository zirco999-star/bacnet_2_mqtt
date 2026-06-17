/**
 * @file z_bacnet.h
 * @brief Déclarations du moteur BACnet MS/TP et de la base de données d'objets.
 *
 * Ce fichier définit l'ensemble des structures de données, énumérations,
 * constantes de timing et prototypes nécessaires au fonctionnement du
 * protocole BACnet MS/TP (ASHRAE 135) sur le gateway BACnet2MQTT.
 *
 * @details
 * Organisation logique du fichier :
 *   1. Masques de bits pour les Status_Flags BACnet (Propriété 111).
 *   2. Structures de persistance NVS (format binaire compact, pragma pack).
 *   3. Énumération complète des types d'objets BACnet (ASHRAE 135 §12.1).
 *   4. Machine à états de découverte (DISC_STEP_T).
 *   5. Base de données en RAM (BACnetDevice / BACnetObject).
 *   6. Constantes de timing ASHRAE 135.
 *   7. Structure de trame MS/TP et FSM du nœud maître.
 *   8. File de travaux (BACnetJob) et prototypes de l'API publique.
 *
 * @note Architecture multi-cœur :
 *   La FSM MS/TP (handle_mstp_*) tourne exclusivement sur le Core 1.
 *   L'accès à bacnet_network_cache depuis le Core 0 (web/MQTT) est
 *   protégé par cache_mutex (SemaphoreHandle_t).
 *
 * @see z_bacnet.cpp  Implémentation de la FSM et du moteur BACnet.
 * @see z_nvs.h       Persistance des devices/objets en NVS.
 */
#ifndef Z_BACNET_H
#define Z_BACNET_H

#include "z_config.h"
#include <vector>

// ============================================================================
// MASQUES DE BITS POUR STATUS_FLAGS (ASHRAE 135, Propriété 111)
// Chaque objet BACnet expose 4 drapeaux d'état encodés sur les bits 0-3.
// Ces masques permettent un test rapide via opérateur bitwise AND.
// ============================================================================
#define BACNET_STATUS_IN_ALARM         0x01 ///< Bit 0 : Alarme active sur l'objet.
#define BACNET_STATUS_FAULT            0x02 ///< Bit 1 : Défaut matériel ou sonde déconnectée.
#define BACNET_STATUS_OVERRIDDEN       0x04 ///< Bit 2 : Forçage local physique (bypass opérateur).
#define BACNET_STATUS_OUT_OF_SERVICE   0x08 ///< Bit 3 : Objet hors-service (mode maintenance/hack).

// ============================================================================
// STRUCTURES DE PERSISTANCE BINAIRE (NVS)
// Format compact avec pragma pack(1) pour éliminer le padding.
// Utilisées par save_device_objects() / load_device_objects() dans z_nvs.cpp.
// ============================================================================

/**
 * @brief Structure de persistance d'un objet BACnet en NVS (v1 Legacy).
 *
 * @details Format binaire compact (95 octets) pour stocker les propriétés
 * essentielles d'un objet BACnet dans la NVS de l'ESP32. Le champ ulVal
 * encode le type et l'instance dans un seul uint32_t :
 *   - Bits [31:22] : Type d'objet (10 bits, max 1023)
 *   - Bits [21:0]  : Instance (22 bits, max 4194303)
 *
 * @note Le champ state_texts (Multi-State) n'est PAS stocké ici car sa
 *       taille est variable. Il est persisté séparément via
 *       save_object_states() / load_object_states().
 *
 * @warning La taille de cette structure est critique : 20 objets × 95 octets
 *          = 1900 octets, ce qui respecte la limite NVS de 1984 octets par blob.
 */
#pragma pack(push, 1)
struct BACnetPersistenceObj {
    uint32_t ulVal;         ///< Encodage combiné [TYPE:10 bits][INSTANCE:22 bits].
    char cName[32];        ///< Nom convivial de l'objet (Object_Name, Prop 77).
    char cUnitText[12];    ///< Texte d'unité personnalisé (ex: "°C", "kWh").
    bool xEnabled;         ///< true si l'objet est activé pour le polling/MQTT.
    bool xNamePublished;   ///< true si le nom a déjà été publié en MQTT.
    bool xIsCommandable;   ///< true si l'objet possède une Priority_Array (Prop 87).
    uint16_t usUnits;       ///< Code d'unité BACnet (ASHRAE 135, Prop 117).
    uint8_t ucExpectedStatesCount; ///< Nombre d'états attendus (Multi-State, Prop 74).
    uint8_t ucStatusFlags;     ///< v7.0.0 : 4 bits de Status_Flags [InAlarm|Fault|Overridden|OutOfService].
    float fMinValue;      ///< v6.4.3 : Valeur minimum (Prop 69, Min_Pres_Value).
    float fMaxValue;      ///< v6.4.3 : Valeur maximum (Prop 65, Max_Pres_Value).
    char cLastHaComponent[16]; ///< v6.3.6 : Dernier composant HA publié (sensor/select/switch) pour nettoyage des entités fantômes.
    float fStepValue;     ///< v6.9.0 : Pas de réglage pour les entités HA 'number'.
    char cMinRef[6];      ///< v6.9.0 : Référence croisée vers un autre objet pour le min dynamique.
    char cMaxRef[6];      ///< v6.9.0 : Référence croisée vers un autre objet pour le max dynamique.
}; // Total : 95 octets

/**
 * @brief Structure de persistance d'un device BACnet en NVS.
 *
 * @details Stocke les métadonnées d'un device découvert sur le bus MS/TP :
 * identification, état de découverte, et paramètres réseau dynamiques
 * (négociés via les propriétés APDU du device distant).
 *
 * @note Le champ xSupportsRpm sert de flag de fallback : si un device ne
 *       supporte pas ReadPropertyMultiple (RPM), le moteur bascule
 *       automatiquement en ReadProperty unitaire (plus lent).
 */
struct BACnetPersistenceDev {
    uint32_t ulDeviceId;    ///< Identifiant BACnet du device (Prop 75).
    uint8_t ucMacAddress;   ///< Adresse MAC MS/TP du device (0-127).
    bool xEnabled;          ///< true si le device est activé pour le polling.
    char cName[32];         ///< Nom du device (Object_Name, Prop 77).
    char cVendor[32];       ///< Nom du fabricant (Vendor_Name, Prop 121).
    uint16_t usCount;       ///< Nombre d'objets du device. uint16_t pour supporter > 255.
    bool xDiscoveryDone;    ///< true si la découverte complète est terminée.
    uint16_t usDiscObjIdx;  ///< Index de l'objet en cours de découverte.
    uint8_t ucDiscStep;     ///< Étape courante de la machine de découverte (DISC_STEP_T).
    // Champs ajoutés en v6.8.3 pour la négociation APDU par device
    uint16_t usMaxApduLengthAccepted; ///< Prop 62 : Taille max d'APDU acceptée par le device distant.
    uint32_t ulApduTimeout;           ///< Prop 11 : Timeout APDU du device distant (ms).
    uint8_t ucNumberOfApduRetries;    ///< Prop 73 : Nombre de retransmissions APDU du device distant.
    bool xSupportsRpm;                ///< Flag de fallback : false => utiliser ReadProperty unitaire.
};

/**
 * @brief Page de transport pour la persistance NVS des objets.
 *
 * @details La NVS ESP32 limite chaque blob à ~1984 octets. Cette structure
 * regroupe 20 objets (20 × 95 = 1900 octets) par page pour respecter cette
 * contrainte. Plusieurs pages sont nécessaires pour les devices ayant > 20 objets.
 */
struct BACnetPersistencePage {
    uint32_t ulDeviceId;               ///< Identifiant du device propriétaire des objets.
    uint16_t page_index;               ///< Index de la page (0, 1, 2...).
    BACnetPersistenceObj objects[20];   ///< Tableau de 20 objets compacts (1900 octets).
};
#pragma pack(pop)

// ============================================================================
// TYPES D'OBJETS BACNET (ASHRAE 135, §12.1)
// Énumération complète des types d'objets standard, de 0 (Analog Input) à
// 62 (Audit Reporter). Les valeurs numériques correspondent aux codes
// officiels de la norme.
// ============================================================================

/** @brief Énumération des types d'objets BACnet standard (ASHRAE 135). */
enum BACnetObjectType {
    OBJ_ANALOG_INPUT = 0,
    OBJ_ANALOG_OUTPUT = 1,
    OBJ_ANALOG_VALUE = 2,
    OBJ_BINARY_INPUT = 3,
    OBJ_BINARY_OUTPUT = 4,
    OBJ_BINARY_VALUE = 5,
    OBJ_CALENDAR = 6,
    OBJ_COMMAND = 7,
    OBJ_DEVICE = 8,
    OBJ_EVENT_ENROLLMENT = 9,
    OBJ_FILE = 10,
    OBJ_GROUP = 11,
    OBJ_LOOP = 12,
    OBJ_MULTI_STATE_INPUT = 13,
    OBJ_MULTI_STATE_OUTPUT = 14,
    OBJ_NOTIFICATION_CLASS = 15,
    OBJ_PROGRAM = 16,
    OBJ_SCHEDULE = 17,
    OBJ_AVERAGING = 18,
    OBJ_MULTI_STATE_VALUE = 19,
    OBJ_TREND_LOG = 20,
    OBJ_LIFE_SAFETY_POINT = 21,
    OBJ_LIFE_SAFETY_ZONE = 22,
    OBJ_ACCUMULATOR = 23,
    OBJ_PULSE_CONVERTER = 24,
    OBJ_EVENT_LOG = 25,
    OBJ_GLOBAL_GROUP = 26,
    OBJ_TREND_LOG_MULTIPLE = 27,
    OBJ_LOAD_CONTROL = 28,
    OBJ_STRUCTURED_VIEW = 29,
    OBJ_ACCESS_DOOR = 30,
    OBJ_ACCESS_CREDENTIAL = 32,
    OBJ_ACCESS_POINT = 33,
    OBJ_ACCESS_RIGHTS = 34,
    OBJ_ACCESS_USER = 35,
    OBJ_ACCESS_ZONE = 36,
    OBJ_CREDENTIAL_DATA_INPUT = 37,
    OBJ_NETWORK_SECURITY = 38,
    OBJ_BITSTRING_VALUE = 39,
    OBJ_CHARACTERSTRING_VALUE = 40,
    OBJ_DATE_PATTERN_VALUE = 41,
    OBJ_DATE_VALUE = 42,
    OBJ_DATETIME_PATTERN_VALUE = 43,
    OBJ_DATETIME_VALUE = 44,
    OBJ_INTEGER_VALUE = 45,
    OBJ_LARGE_ANALOG_VALUE = 46,
    OBJ_OCTETSTRING_VALUE = 47,
    OBJ_POSITIVE_INTEGER_VALUE = 48,
    OBJ_TIME_PATTERN_VALUE = 49,
    OBJ_TIME_VALUE = 50,
    OBJ_NOTIFICATION_FORWARDER = 51,
    OBJ_ALERT_ENROLLMENT = 52,
    OBJ_CHANNEL = 53,
    OBJ_LIGHTING_OUTPUT = 54,
    OBJ_BINARY_LIGHTING_OUTPUT = 55,
    OBJ_NETWORK_PORT = 56,
    OBJ_TIMER = 57,
    OBJ_ELEVATOR_GROUP = 58,
    OBJ_LIFT = 59,
    OBJ_ESCALATOR = 60,
    OBJ_AUDIT_LOG = 61,
    OBJ_AUDIT_REPORTER = 62
};

// ============================================================================
// ÉTATS DE LA MACHINE DE DÉCOUVERTE
// Séquence ordonnée des étapes pour découvrir un device et ses objets.
// Progression : propriétés du device → liste d'objets → propriétés de chaque objet.
// ============================================================================

/**
 * @brief Étapes de la machine à états de découverte d'un device BACnet.
 *
 * @details La découverte s'effectue en deux phases :
 *   1. **Phase Device** (DISC_DEV_*) : Lecture des propriétés du device
 *      (nom, vendeur, paramètres APDU).
 *   2. **Phase Objets** (DISC_OBJ_*) : Pour chaque objet du device, lecture
 *      séquentielle de ses propriétés (nom, unités, min/max, états, valeur).
 *
 * @note L'ordre des étapes est critique : Status_Flags (Prop 111) est lu
 *       AVANT la valeur pour ne pas remonter les valeurs d'un objet en défaut.
 */
enum DISC_STEP_T {
    DISC_DEV_ID = 0,      ///< Lecture du Device_Identifier (Prop 75).
    DISC_DEV_NAME,         ///< Lecture du Object_Name (Prop 77).
    DISC_DEV_VENDOR,       ///< Lecture du Vendor_Name (Prop 121).
    DISC_DEV_MAX_APDU,     ///< Lecture du Max_APDU_Length_Accepted (Prop 62).
    DISC_DEV_TIMEOUT,      ///< Lecture de l'APDU_Timeout (Prop 11).
    DISC_DEV_RETRIES,      ///< Lecture du Number_Of_APDU_Retries (Prop 73).
    DISC_OBJ_COUNT,        ///< Lecture du Object_List (Prop 76) pour compter les objets.
    DISC_OBJ_OID,          ///< Lecture de l'Object_Identifier de chaque objet.
    DISC_OBJ_NAME,         ///< Lecture du Object_Name de chaque objet (Prop 77).
    DISC_OBJ_UNITS,        ///< Lecture des Units de chaque objet (Prop 117).
    DISC_OBJ_MIN,          ///< Lecture du Min_Pres_Value (Prop 69).
    DISC_OBJ_MAX,          ///< Lecture du Max_Pres_Value (Prop 65).
    DISC_OBJ_STATES,       ///< Lecture des State_Text pour les Multi-State (Prop 110).
    DISC_OBJ_COMMANDABLE,  ///< Test de présence de la Priority_Array (Prop 87).
    DISC_OBJ_STATUS_FLAGS, ///< Lecture des Status_Flags (Prop 111) — AVANT la valeur.
    DISC_OBJ_VALUE         ///< Lecture de la Present_Value (Prop 85).
};

// ============================================================================
// STATISTIQUES MS/TP
// Compteurs de fonctionnement du bus, accessibles via l'API web et les logs.
// ============================================================================

/**
 * @brief Compteurs statistiques du bus BACnet MS/TP.
 *
 * @details Permet de surveiller la santé du bus depuis l'interface web :
 * nombre de messages échangés, tokens vus, erreurs CRC, et progression
 * du polling. Remis à zéro à chaque redémarrage.
 */
struct BACnet_Stats {
    uint32_t ulMsMsgsRx;      ///< Nombre total de messages MS/TP reçus.
    uint32_t ulMsMsgsTx;      ///< Nombre total de messages MS/TP transmis.
    uint32_t ulTokensSeen;    ///< Nombre de passages de jeton observés.
    uint32_t ulPfmReplies;    ///< Nombre de réponses Poll-For-Master reçues.
    uint32_t ulErrorsCrc;     ///< Nombre d'erreurs CRC détectées (trames corrompues).
    uint8_t ucCurrentIndex;   ///< Index de l'objet en cours de polling.
    uint8_t ucTotalObjects;   ///< Nombre total d'objets activés pour le polling.
    bool xRingActive;         ///< true si le ring MS/TP est actif (jeton circule).
};
extern BACnet_Stats bacnetStats;

// ============================================================================
// BASE DE DONNÉES EN RAM
// Représentation vivante des devices et objets BACnet découverts.
// Protégée par cache_mutex pour l'accès inter-cœurs.
// ============================================================================

/**
 * @brief Représentation en RAM d'un objet BACnet.
 *
 * @details Contient toutes les propriétés d'un objet nécessaires au
 * fonctionnement du gateway : valeur courante, métadonnées d'unité,
 * état de découverte, configuration HA, et drapeaux de statut.
 *
 * @note Les valeurs par défaut (usType=65535, fPresentValue=NAN, etc.)
 *       permettent de détecter les objets non encore découverts.
 * @note Le champ state_texts (vector<String>) est dynamique en RAM
 *       et persisté séparément via save_object_states() / load_object_states().
 */
struct BACnetObject {
    uint16_t usType = 65535;              ///< Type d'objet BACnet (0xFFFF = non initialisé).
    uint32_t ulInstance = 0;              ///< Numéro d'instance de l'objet.
    char cName[50] = "Unknown";           ///< Nom convivial (Object_Name, Prop 77).
    float fPresentValue = NAN;            ///< Valeur courante (Present_Value, Prop 85). NAN = pas encore lue.
    bool xEnabled = false;                ///< true si l'objet est activé pour le polling et la publication MQTT.
    bool xNamePublished = false;          ///< true si le nom a été publié au moins une fois en MQTT.
    char cLastMqttName[50] = "";          ///< Dernier nom utilisé pour la publication MQTT (détection de renommage).
    uint32_t ulLastUpdate = 0;            ///< Timestamp (millis) de la dernière mise à jour de la valeur.
    uint16_t ucExpectedStatesCount = 0;   ///< Nombre d'états pour les objets Multi-State (Prop 74).
    uint8_t ucStatusFlags = 0;            ///< Drapeaux d'état BACnet (4 bits, Prop 111).
    uint16_t usUnits = 95;                ///< Code d'unité BACnet (95 = "no-units", Prop 117).
    char cUnitText[20] = "";              ///< Texte d'unité résolu (ex: "°C", "kWh").
    float fMinValue = NAN;                ///< Valeur minimum (Prop 69). NAN = non définie.
    float fMaxValue = NAN;                ///< Valeur maximum (Prop 65). NAN = non définie.
    float fStepValue = 1.0f;              ///< Pas de réglage pour HA 'number' (Prop custom).
    char cMinRef[6] = "";                 ///< Référence croisée min dynamique (ex: "AI:3").
    char cMaxRef[6] = "";                 ///< Référence croisée max dynamique (ex: "AO:1").
    bool xDiscoveryDone = false;          ///< true si toutes les propriétés ont été lues.
    bool xIsCommandable = false;          ///< true si l'objet possède une Priority_Array (Prop 87).
    char cLastHaComponent[16] = "";       ///< Dernier composant HA publié (sensor/select/switch) pour nettoyage des entités fantômes lors d'un changement de type.
    /// Textes des états pour les objets Multi-State (Prop 110).
    /// Dynamique en RAM, persisté séparément en NVS via save_object_states().
    std::vector<String> state_texts;
    
    // --- Accesseurs inline pour les Status_Flags (test bitwise rapide) ---
    /** @brief Vérifie si l'objet est en état d'alarme (Bit 0). */
    inline bool isInAlarm() const { return (ucStatusFlags & BACNET_STATUS_IN_ALARM) != 0; }
    /** @brief Vérifie si l'objet est en défaut matériel (Bit 1). */
    inline bool isFault() const { return (ucStatusFlags & BACNET_STATUS_FAULT) != 0; }
    /** @brief Vérifie si l'objet est en forçage local (Bit 2). */
    inline bool isOverridden() const { return (ucStatusFlags & BACNET_STATUS_OVERRIDDEN) != 0; }
    /** @brief Vérifie si l'objet est hors-service (Bit 3). */
    inline bool isOutOfService() const { return (ucStatusFlags & BACNET_STATUS_OUT_OF_SERVICE) != 0; }

};

/**
 * @brief Représentation en RAM d'un device BACnet découvert sur le bus MS/TP.
 *
 * @details Contient les métadonnées du device, ses paramètres réseau
 * négociés (APDU), l'état de sa découverte, et le vecteur de ses objets.
 *
 * @note Le champ ulDeviceId par défaut (4194303 = 0x3FFFFF) correspond à
 *       la valeur max encodable sur 22 bits, signalant un device non identifié.
 *
 * @note Système de sauvegarde différée (v6.8.8) :
 *       Le drapeau xDirty est positionné à chaque modification d'un objet.
 *       La sauvegarde NVS n'est déclenchée que si ulLastDirtyTime dépasse
 *       un seuil, évitant ainsi les écritures flash trop fréquentes.
 */
struct BACnetDevice {
    uint8_t ucMacAddress = 0;                 ///< Adresse MAC MS/TP du device (0-127).
    uint32_t ulDeviceId = 4194303;            ///< Identifiant BACnet (Prop 75). 0x3FFFFF = non identifié.
    String name = "";                          ///< Nom du device (Object_Name).
    String vendor = "";                        ///< Nom du fabricant (Vendor_Name).
    String version = "";                       ///< Version du firmware du device.
    bool xEnabled = true;                      ///< true si le device est activé pour le polling.

    // Paramètres réseau dynamiques négociés avec le device distant (v6.8.3)
    uint16_t usMaxApduLengthAccepted = 480;   ///< Taille max d'APDU que le device accepte (Prop 62). 480 = MS/TP standard.
    uint32_t ulApduTimeout = 3000;            ///< Timeout APDU du device distant en ms (Prop 11).
    uint8_t ucNumberOfApduRetries = 3;        ///< Nombre de retransmissions du device distant (Prop 73).
    bool xSupportsRpm = true;                  ///< true = supporte ReadPropertyMultiple. false = fallback ReadProperty unitaire.

    std::vector<BACnetObject> objects;         ///< Vecteur dynamique des objets du device.
    uint32_t last_seen = 0;                    ///< Timestamp (millis) du dernier signe de vie.
    bool xDiscoveryDone = false;               ///< true si la découverte complète est terminée.
    DISC_STEP_T ucDiscStep = DISC_DEV_ID;     ///< Étape courante de la machine de découverte.
    uint16_t usDiscObjIdx = 0;                 ///< Index de l'objet en cours de découverte.
    bool xReloadSingle = false;                ///< true si un rechargement unitaire est demandé (UI).
    bool xRecoveryMode = false;                ///< true si le device est en mode récupération après erreur.
    bool xDirty = false;                       ///< v6.8.8 : true si des modifications non sauvegardées existent.
    uint32_t ulLastDirtyTime = 0;              ///< v6.8.8 : Timestamp du dernier changement (pour sauvegarde différée).

    /** @brief Constructeur par défaut. Initialise tous les champs à leurs valeurs par défaut. */
    BACnetDevice() {}
    };

#include <map>

/** @brief Cache réseau : vecteur de tous les devices BACnet découverts.
 *  @warning Accès protégé par cache_mutex (obligatoire depuis Core 0). */
extern std::vector<BACnetDevice> bacnet_network_cache;

/** @brief Mutex de protection du cache réseau pour l'accès inter-cœurs.
 *  @details Core 1 (FSM BACnet) est le producteur principal.
 *           Core 0 (web/MQTT) est consommateur en lecture. */
extern SemaphoreHandle_t cache_mutex;

/**
 * @brief Table de dépendances Home Assistant.
 * @details Associe un unique_id HA à la liste des objets BACnet (paires
 * device_id/instance) qui influencent cette entité. Utilisée pour
 * déclencher une re-publication HA quand un objet référencé change.
 */
extern std::map<String, std::vector<std::pair<uint32_t, uint32_t>>> ha_dependencies;

extern uint32_t period_poll_count;       ///< Compteur de cycles de polling BACnet dans la période courante.
extern uint32_t period_mqtt_pub_count;   ///< Compteur de publications MQTT dans la période courante.

// ============================================================================
// CONSTANTES DE TEMPS ASHRAE 135 (v6.3.0)
// Toutes les valeurs sont en microsecondes pour la précision de la FSM.
// ============================================================================

/** @brief Timeout de réponse après une requête Data-Expecting-Reply.
 *  @details Fixé à 280ms (au lieu des 255ms standard) pour supporter
 *  l'automate ECB-203 qui répond en ~240ms. Cf. leçon apprise YABE. */
const uint32_t T_REPLY_TIMEOUT_US = 280000;

/** @brief Timeout d'utilisation du jeton (T_usage_timeout ASHRAE 135).
 *  @details Temps max autorisé entre la réception du jeton et l'émission
 *  de la première trame de données. Au-delà, le jeton est passé. */
const uint32_t T_USAGE_TIMEOUT_US = 15000;

/** @brief Timeout d'abandon de trame (Frame Abort).
 *  @details Si une trame incomplète est détectée (octets reçus puis silence),
 *  ce timeout déclenche l'abandon et le retour à l'état RX_IDLE. */
const uint32_t T_FRAME_ABORT_US   = 50000;

// ============================================================================
// STRUCTURE DE TRANSPORT MS/TP
// ============================================================================

/**
 * @brief Trame MS/TP décodée (en-tête + données APDU).
 *
 * @details Utilisée comme structure intermédiaire entre la réception UART
 * brute et le traitement applicatif BACnet. Le buffer data[512] est
 * dimensionné pour contenir une APDU MS/TP complète.
 */
struct MSTP_Frame {
    uint8_t type;          ///< Type de trame MS/TP (Token, PFM, BACnet Data, etc.).
    uint8_t dest;          ///< Adresse MAC de destination.
    uint8_t src;           ///< Adresse MAC source.
    uint16_t len;          ///< Longueur des données APDU (0 pour Token/PFM).
    uint8_t data[512];     ///< Buffer de données APDU.
    uint32_t timestamp_us; ///< Timestamp de réception (µs) pour le calcul des timeouts.
};

// ============================================================================
// PROTOTYPES — MODULES FSM MS/TP (v6.3.0)
// Chaque état de la FSM maître est implémenté dans une fonction dédiée.
// Toutes s'exécutent exclusivement sur Core 1.
// ============================================================================

/** @brief Gère l'état IDLE : attente de trame ou expiration de timeout. */
void handle_mstp_idle();
/** @brief Gère l'état USE_TOKEN : émission des trames de données. */
void handle_mstp_use_token();
/** @brief Gère l'état WAIT_FOR_REPLY : attente de réponse après requête DER. */
void handle_mstp_wait_for_reply();
/** @brief Gère l'état PASS_TOKEN : transmission du jeton au successeur. */
void handle_mstp_pass_token();
/** @brief Gère l'état POLL_FOR_MASTER : sondage des adresses MAC inconnues. */
void handle_mstp_poll_for_master();

/**
 * @brief Traite une trame MS/TP reçue et décodée.
 * @param frame Référence vers la trame décodée à traiter.
 */
void process_incoming_frame(MSTP_Frame &frame);

/** @brief Exécute le prochain travail BACnet en file d'attente. */
void execute_bacnet_work();

/**
 * @brief Exécute la logique de découverte séquentielle pour un device.
 * @param dev Référence vers le device en cours de découverte.
 */
void execute_discovery_logic(BACnetDevice &dev);

/**
 * @brief Exécute la logique de polling cyclique pour un device.
 * @param dev Référence vers le device à interroger.
 */
void execute_polling_logic(BACnetDevice &dev);

/**
 * @brief Vérifie s'il reste du travail BACnet à effectuer.
 * @return true s'il y a des jobs en file ou des devices à découvrir/poller.
 */
bool has_bacnet_work();

// ============================================================================
// ÉTATS FSM MS/TP (ASHRAE 135, §9.5)
// ============================================================================

/**
 * @brief États de la machine de réception de trames MS/TP.
 * @details Décode le flux UART octet par octet :
 *   IDLE → PREAMBLE (0x55/0xFF) → HEADER → HEADER_CRC → DATA → DATA_CRC.
 */
enum RX_STATE { 
    RX_IDLE,        ///< Attente du premier octet de préambule (0x55).
    RX_PREAMBLE,    ///< Préambule 0x55 reçu, attente du 0xFF.
    RX_HEADER,      ///< Réception des 6 octets d'en-tête (Type, Dest, Src, Len).
    RX_HEADER_CRC,  ///< Vérification du CRC8 de l'en-tête.
    RX_DATA,        ///< Réception des données APDU.
    RX_DATA_CRC     ///< Vérification du CRC16 des données.
};

/**
 * @brief États de la FSM du nœud maître MS/TP (ASHRAE 135, §9.5.7).
 *
 * @details Implémente le cycle de vie complet du jeton :
 *   INITIALIZE → IDLE → (USE_TOKEN → WAIT_FOR_REPLY → DONE_WITH_TOKEN →
 *   PASS_TOKEN → POLL_FOR_MASTER) → IDLE.
 *
 * @note MSTP_NO_TOKEN est un état transitoire activé quand le jeton
 *       n'a pas été vu depuis T_no_token (ASHRAE 135, §9.5.7.1).
 */
enum MSTP_MASTER_STATE {
    MSTP_INITIALIZE,         ///< Initialisation du nœud (reset compteurs, attente du bus).
    MSTP_IDLE,               ///< Attente de trame ou de jeton.
    MSTP_USE_TOKEN,          ///< Utilisation du jeton pour émettre des données.
    MSTP_WAIT_FOR_REPLY,     ///< Attente de réponse après requête Data-Expecting-Reply.
    MSTP_DONE_WITH_TOKEN,    ///< Fin d'utilisation du jeton (vérification quota).
    MSTP_PASS_TOKEN,         ///< Passage du jeton au successeur.
    MSTP_NO_TOKEN,           ///< Jeton non vu depuis T_no_token (tentative de récupération).
    MSTP_POLL_FOR_MASTER,    ///< Sondage d'adresses MAC pour découvrir de nouveaux nœuds.
    MSTP_ANSWER_DATA_REQUEST ///< Réponse à une requête de données entrante.
};

/**
 * @brief Types de travaux BACnet pouvant être mis en file d'attente.
 * @details Soumis depuis Core 0 (UI/MQTT) vers Core 1 (FSM) via bacnet_job_queue.
 */
enum BACnetJobType { JOB_WHO_IS, JOB_I_AM, JOB_WRITE_PROP, JOB_READ_PROP };

/**
 * @brief Structure décrivant un travail BACnet à exécuter.
 *
 * @details Transportée via la FreeRTOS Queue bacnet_job_queue.
 * Créée par l'UI web ou le module MQTT (Core 0), consommée par
 * la FSM BACnet (Core 1) dans execute_bacnet_work().
 */
struct BACnetJob {
    BACnetJobType type;      ///< Type de travail (WHO_IS, I_AM, WRITE, READ).
    uint8_t target_mac;      ///< Adresse MAC MS/TP du device cible.
    uint16_t obj_type;       ///< Type d'objet BACnet cible.
    uint32_t obj_instance;   ///< Instance de l'objet BACnet cible.
    uint8_t prop_id;         ///< Identifiant de la propriété BACnet à lire/écrire.
    int32_t array_index;     ///< Index de tableau (conservé mais inutilisé pour State_Text).
    float write_value;       ///< Valeur à écrire (pour JOB_WRITE_PROP).
    uint8_t priority;        ///< Priorité d'écriture BACnet (1-16, cf. Priority_Array).
    char name[50];           ///< Nom de l'objet (pour les logs et le débogage).
};

/** @brief File d'attente FreeRTOS pour les travaux BACnet inter-cœurs.
 *  @details Producteur : Core 0 (UI/MQTT). Consommateur : Core 1 (FSM). */
extern QueueHandle_t bacnet_job_queue;

// ============================================================================
// API PUBLIQUE DU MOTEUR BACNET
// ============================================================================

/**
 * @brief Initialise le moteur BACnet MS/TP : UART, FSM, tâche FreeRTOS Core 1.
 * @details Appelée une seule fois depuis setup() dans bacnet_2_mqtt.ino.
 */
void setup_bacnet_engine();

/**
 * @brief Abandonne la transaction APDU en cours et retourne à MSTP_IDLE.
 * @details Utilisée en cas de timeout ou d'erreur de communication.
 */
void bacnet_abort_current_transaction();

/**
 * @brief Ajoute un travail dans la file d'attente BACnet.
 * @param job Structure BACnetJob décrivant le travail à effectuer.
 * @return true si le job a été ajouté avec succès, false si la file est pleine.
 */
bool enqueue_bacnet_job(BACnetJob job);

/** @brief Force la re-publication de tous les noms d'objets en MQTT. */
void publish_all_names();

/**
 * @brief Convertit un code d'unité BACnet en texte lisible.
 * @param usUnits Code d'unité ASHRAE 135 (Prop 117).
 * @return Chaîne de caractères correspondant à l'unité (ex: "°C", "kPa").
 */
String get_unit_text(uint16_t usUnits);

/**
 * @brief Vérifie et déclenche les dépendances HA pour un objet donné.
 * @param did    Identifiant du device BACnet.
 * @param type   Type de l'objet BACnet.
 * @param inst   Instance de l'objet BACnet.
 */
void check_ha_dependencies(uint32_t did, uint16_t type, uint32_t inst);

// ============================================================================
// CONSTRUCTEURS D'APDU (Encodage ASN.1/BER conforme ASHRAE 135)
// Ces fonctions construisent les PDU BACnet dans un buffer fourni par
// l'appelant et retournent la longueur totale de l'APDU encodée.
// ============================================================================

/**
 * @brief Construit une APDU ReadPropertyMultiple (RPM).
 * @param buffer     Buffer de sortie pour l'APDU encodée.
 * @param invoke_id  Identifiant d'invocation (corrélation requête/réponse).
 * @param objects    Vecteur de pointeurs vers les objets à lire.
 * @param property_id Identifiant de la propriété à lire sur chaque objet.
 * @return Longueur de l'APDU encodée en octets.
 */
uint16_t build_read_property_multiple_apdu(uint8_t* buffer, uint8_t invoke_id, std::vector<BACnetObject*>& objects, uint8_t property_id);

/**
 * @brief Construit une APDU ReadProperty unitaire.
 * @param buffer       Buffer de sortie pour l'APDU encodée.
 * @param invoke_id    Identifiant d'invocation.
 * @param obj_type     Type de l'objet cible.
 * @param obj_instance Instance de l'objet cible.
 * @param property_id  Identifiant de la propriété à lire.
 * @param array_index  Index de tableau (-1 = pas d'index).
 * @return Longueur de l'APDU encodée en octets.
 */
uint16_t build_read_property_apdu(uint8_t* buffer, uint8_t invoke_id, uint16_t obj_type, uint32_t obj_instance, uint8_t property_id, int32_t array_index);

/**
 * @brief Construit une APDU WriteProperty pour modifier le Object_Name (Prop 77).
 * @param buffer       Buffer de sortie pour l'APDU encodée.
 * @param invoke_id    Identifiant d'invocation.
 * @param obj_type     Type de l'objet cible.
 * @param obj_instance Instance de l'objet cible.
 * @param new_name     Nouveau nom à écrire (chaîne C).
 * @return Longueur de l'APDU encodée en octets.
 */
uint16_t build_write_property_name_apdu(uint8_t* buffer, uint8_t invoke_id, uint16_t obj_type, uint32_t obj_instance, const char* new_name);

/**
 * @brief Construit une APDU WriteProperty pour modifier la Present_Value (Prop 85).
 * @param buffer       Buffer de sortie pour l'APDU encodée.
 * @param invoke_id    Identifiant d'invocation.
 * @param obj_type     Type de l'objet cible.
 * @param obj_instance Instance de l'objet cible.
 * @param prop_id      Identifiant de la propriété à écrire.
 * @param value        Valeur flottante à écrire.
 * @param ucPriority   Priorité BACnet d'écriture (1-16).
 * @return Longueur de l'APDU encodée en octets.
 */
uint16_t build_write_property_value_apdu(uint8_t* buffer, uint8_t invoke_id, uint16_t obj_type, uint32_t obj_instance, uint8_t prop_id, float value, uint8_t ucPriority);

/**
 * @brief Construit une APDU WriteProperty pour libérer une priorité (Write NULL).
 * @param pucBuffer    Buffer de sortie pour l'APDU encodée.
 * @param ucInvokeId   Identifiant d'invocation.
 * @param usObjType    Type de l'objet cible.
 * @param ulObjInstance Instance de l'objet cible.
 * @param ucPropId     Identifiant de la propriété à relâcher.
 * @param ucPriority   Priorité BACnet à effacer (1-16).
 * @return Longueur de l'APDU encodée en octets.
 */
uint16_t build_write_property_relinquish_apdu(uint8_t* pucBuffer, uint8_t ucInvokeId, uint16_t usObjType, uint32_t ulObjInstance, uint8_t ucPropId, uint8_t ucPriority);

/**
 * @brief Construit une APDU WriteProperty pour modifier Out_Of_Service (Prop 81).
 * @param buffer         Buffer de sortie pour l'APDU encodée.
 * @param invoke_id      Identifiant d'invocation.
 * @param obj_type       Type de l'objet cible.
 * @param obj_instance   Instance de l'objet cible.
 * @param out_of_service true pour mettre l'objet hors-service, false pour le remettre en service.
 * @return Longueur de l'APDU encodée en octets.
 */
uint16_t build_write_property_outofservice_apdu(uint8_t* buffer, uint8_t invoke_id, uint16_t obj_type, uint32_t obj_instance, bool out_of_service);

/**
 * @brief Construit une APDU I-Am (annonce de présence BACnet).
 * @param buffer          Buffer de sortie pour l'APDU encodée.
 * @param device_instance Identifiant du device à annoncer.
 * @param max_apdu        Taille max d'APDU supportée.
 * @param vendor_id       Identifiant du fabricant BACnet.
 * @return Longueur de l'APDU encodée en octets.
 */
uint16_t build_i_am_apdu(uint8_t* buffer, uint32_t device_instance, uint16_t max_apdu, uint16_t vendor_id);

#endif
