Notice Technique : Constantes C++ pour Pile BACnet MS/TP (Standard ASHRAE 135)

Cette notice technique définit les standards de codage, les constantes protocolaires et les directives d'implémentation pour la pile BACnet MS/TP. En tant qu'architecte, l'objectif est de garantir la portabilité, le déterminisme et la sécurité mémoire sur des cibles microcontrôleurs variées.

1. Guide de Style et Types de Données Fondamentaux

L'implémentation suit une convention de nommage compositionnelle stricte, facilitant la lecture du code sans ambiguïté sur la portée ou le type de donnée.

1.1 Conventions de Nommage (Préfixes)

Les préfixes se cumulent (ex: p + u + c = puc pour un pointeur sur un octet non-signé).

Préfixe	Type de donnée associé	Nuance technique
c	char	Doit être qualifié (signed/unsigned) sauf pour l'ASCII ou les pointeurs de chaînes.
s	int16_t (short)	Entier 16 bits signé.
l	int32_t (long)	Entier 32 bits signé.
x	BaseType_t	Type le plus efficace pour l'architecture (ou structures/handles).
u	unsigned	Ajouté pour les types non-signés (ex: us, ul, uc).
p	pointer	Ajouté pour les pointeurs (ex: px, puc).

1.2 Macros et Localisation

Les macros sont en majuscules et obligatoirement préfixées par leur lieu de définition :

* port : Spécifique à la couche portable (ex: portMAX_DELAY).
* task : Défini dans task.h (ex: taskENTER_CRITICAL()).
* pd : Définitions générales du projet (projdefs.h) (ex: pdTRUE).
* config : Configuration système (FreeRTOSConfig.h) (ex: configTOTAL_HEAP_SIZE).
* err : Codes d'erreurs (ex: errQUEUE_FULL).

1.3 Types Portables et Architecture

* TickType_t : Type utilisé pour le décompte du temps système. Sur une architecture 32 bits, il doit impérativement être configuré en uint32_t (configUSE_16_BIT_TICKS à 0). L'usage du 16 bits sur architecture 32 bits n'a aucune justification technique et limite sévèrement la période de blocage maximale.
* BaseType_t : Type le plus performant pour l'unité de traitement (32 bits sur un processeur 32 bits). À privilégier pour les booléens (pdTRUE/pdFALSE) et les codes de retour.


--------------------------------------------------------------------------------


2. Tableaux de Correspondance : Couche MAC MS/TP (Frame Types)

Les types de trames MS/TP sont définis comme des constantes unsigned char (uc). Les valeurs sont exprimées en littéraux non-signés.

Valeur (Hex)	Constante C++	Description
0x00U	ucMSTP_FRAME_TYPE_TOKEN	Jeton pour le droit de parole
0x01U	ucMSTP_FRAME_TYPE_POLL_FOR_MASTER	Recherche d'un nouveau maître
0x02U	ucMSTP_FRAME_TYPE_REPLY_TO_POLL_FOR_MASTER	Réponse à la recherche de maître
0x03U	ucMSTP_FRAME_TYPE_TEST_REQUEST	Requête de test de liaison
0x04U	ucMSTP_FRAME_TYPE_TEST_RESPONSE	Réponse au test de liaison
0x05U	ucMSTP_FRAME_TYPE_BACNET_DATA_EXPECTING_REPLY	Données avec réponse attendue
0x06U	ucMSTP_FRAME_TYPE_BACNET_DATA_NOT_EXPECTING_REPLY	Données sans réponse attendue
0x07U	ucMSTP_FRAME_TYPE_REPLY_POSTPONED	Réponse différée


--------------------------------------------------------------------------------


3. Masques Binaires du NPDU Control Octet

L'octet de contrôle du Network Layer Protocol Data Unit (NPDU) régit le routage. Les masques utilisent le préfixe pd.

/* Masques pour le décodage de l'octet de contrôle NPDU (Alignement binaire) */
#define pdNPDU_MASK_PROTOCOL_VERSION    ( 1 << 7 ) /* 0x80 : Version du protocole */
#define pdNPDU_MASK_DEST_SPECIFIER       ( 1 << 5 ) /* 0x20 : Présence DNET, DADR, Hop Count */
#define pdNPDU_MASK_SRC_SPECIFIER        ( 1 << 3 ) /* 0x08 : Présence SNET, SADR */
#define pdNPDU_MASK_EXPECTING_REPLY      ( 1 << 2 ) /* 0x04 : Réponse attendue par le protocole */
#define pdNPDU_MASK_PRIORITY             ( 0x03 )   /* Bits 1-0 : Priorité (00: Normal, 11: Life Safety) */



--------------------------------------------------------------------------------


4. Types d'APDU et Codes de Services

Les fonctions de traitement des services doivent retourner un BaseType_t pour une efficacité maximale au niveau du registre processeur.

Exemple de prototype C++ : BaseType_t xBACnetServiceHandler( uint8_t *pucPayload, BaseType_t xPayloadLen );

4.1 Types d'APDU

Code	Constante C++	Description
0	xAPDU_TYPE_CONFIRMED_REQUEST	Requête avec confirmation requise
1	xAPDU_TYPE_UNCONFIRMED_REQUEST	Requête sans confirmation
2	xAPDU_TYPE_SIMPLE_ACK	Acquittement sans données
3	xAPDU_TYPE_COMPLEX_ACK	Acquittement avec données
4	xAPDU_TYPE_SEGMENT_ACK	Acquittement de segment
5	xAPDU_TYPE_ERROR	Réponse d'erreur protocolaire
6	xAPDU_TYPE_REJECT	Rejet de l'APDU
7	xAPDU_TYPE_ABORT	Abandon de la transaction

4.2 Codes de Services

Services Confirmés | Constante C++ | Description | | :--- | :--- | | xSERVICE_CONF_READ_PROPERTY | Lecture d'une propriété d'objet | | xSERVICE_CONF_WRITE_PROPERTY | Écriture d'une propriété d'objet | | xSERVICE_CONF_DEVICE_COMM_CONTROL | Contrôle de communication distant |

Services Non-Confirmés | Constante C++ | Description | | :--- | :--- | | xSERVICE_UNCONF_I_AM | Annonce de présence du dispositif | | xSERVICE_UNCONF_WHO_IS | Recherche de dispositifs sur le réseau |


--------------------------------------------------------------------------------


5. Tags d'Application ASN.1 Primitifs

Ces tags identifient le type de donnée encodé dans les flux APDU.

Tag	Constante C++	Type de donnée
0	ucASN1_TAG_NULL	Null
1	ucASN1_TAG_BOOLEAN	Booléen
2	ucASN1_TAG_UNSIGNED_INT	Entier non-signé
3	ucASN1_TAG_SIGNED_INT	Entier signé
4	ucASN1_TAG_REAL	Réel (Float)
5	ucASN1_TAG_DOUBLE	Double précision
6	ucASN1_TAG_OCTET_STRING	Chaîne d'octets
7	ucASN1_TAG_CHARACTER_STRING	Chaîne de caractères
8	ucASN1_TAG_BIT_STRING	Chaîne de bits
9	ucASN1_TAG_ENUMERATED	Énuméré
10	ucASN1_TAG_DATE	Date
11	ucASN1_TAG_TIME	Heure
12	ucASN1_TAG_OBJECT_IDENTIFIER	Identifiant d'objet BACnet


--------------------------------------------------------------------------------


6. Recommandations d'Implémentation Mémoire

6.1 Gestion Dynamique et Déterminisme

L'usage de pvPortMalloc() et vPortFree() est obligatoire. Les fonctions malloc() standards de la bibliothèque C ne sont pas adaptées pour une pile protocolaire temps réel car :

* Elles ne sont pas déterministes (temps d'exécution variable).
* Elles ne sont généralement pas thread-safe.
* Elles ne permettent pas de prévenir la fragmentation de manière efficace sur des systèmes à ressources limitées.

6.2 Choix du Tas (Heap_4)

L'implémentation doit utiliser le gestionnaire Heap_4. Contrairement à Heap_2, Heap_4 utilise un algorithme de coalescence qui fusionne les blocs libres adjacents pour former de plus grands blocs. Cette caractéristique est vitale pour BACnet MS/TP où la taille des buffers APDU varie constamment entre les lectures de propriétés et les annonces "I-Am".

6.3 Fiabilité et Diagnostic

* configASSERT() : Il est impératif de définir cette macro pour valider les paramètres critiques (ex: pointeurs non-nuls, plages de valeurs des tags). Cela permet d'identifier immédiatement les débordements ou les corruptions de pile lors du développement.
* Restriction ISR : L'usage de printf() ou sprintf() est strictement interdit dans les routines d'interruption (ISR) de l'UART MS/TP. Ces fonctions consomment énormément de pile (stack), ne sont pas réentrantes et bloquent les interruptions trop longtemps. En cas de besoin de debug, utilisez printf-stdarg.c ou des mécanismes de transfert différé via queues.
