### Rapport d'audit technique : Implémentation FSM MS/TP sur ESP32-S3

#### 1\. Analyse des Écarts de la Machine à États Finis (FSM)

##### Analyse de la conformité normative

En vertu des spécifications ASHRAE 135 détaillées dans "Fundamentals of BACnet", le protocole MS/TP (Master-Slave/Token-Passing) est défini comme un réseau local à passage de jeton "low-cost", conçu pour une implémentation sur microcontrôleurs sans matériel dédié. La robustesse de ce LAN repose intégralement sur une gestion déterministe du cycle de possession du jeton. L'analyse du fichier source bacnet\_2\_mqtt\_202605260904.txt révèle une architecture FSM incomplète qui compromet la synchronisation du bus et l'intégrité du réseau.

##### Identification des lacunes fonctionnelles

L'absence de trois états critiques engendre les risques systémiques suivants :

* **NO\_TOKEN :**  Cet état est le pilier de l'entrée sur le bus. Sans lui, le nœud est incapable de gérer le timeout Tno\_token ou d'incrémenter les compteurs nécessaires à l'auto-établissement du réseau (cycle "Sole Master"). Le dispositif reste un auditeur passif incapable de revendiquer le droit d'émission en cas de silence du bus.  
* **DONE\_WITH\_TOKEN :**  Cet état intervient immédiatement après l'envoi des trames de données. Son absence entraîne une gestion imprécise des compteurs de trames (Nmax\_info\_frames) et risque de provoquer une rétention abusive du jeton, violant les contraintes temporelles inter-nœuds.  
* **ANSWER\_DATA\_REQUEST :**  Indispensable pour la couche liaison de données, cet état permet de traiter les requêtes entrantes de type "Reply Postponed". Son omission rend l'équipement invisible pour toute opération de lecture de propriété par un tiers, brisant la sémantique client-serveur du protocole.

##### Tableau comparatif des états : Implémentation vs Norme ASHRAE 135

État Normatif (ASHRAE 135),Statut Actuel,Référence Normative,Impact Technique  
IDLE,Présent,Clause 9.2.1,-  
WAIT\_FOR\_PROPORTIONAL,Présent,Clause 9.2.2,-  
USE\_TOKEN,Présent,Clause 9.2.3,-  
WAIT\_FOR\_REPLY,Présent,Clause 9.2.4,-  
DONE\_WITH\_TOKEN,Manquant,Clause 9.2.5,Instabilité du passage de relais  
PASS\_TOKEN,Présent,Clause 9.2.6,-  
NO\_TOKEN,Manquant,Clause 9.2.7,Échec d'initialisation (Tno\_token)  
ANSWER\_DATA\_REQUEST,Manquant,Clause 9.2.8,Incapacité de réponse (DS-RP-B)  
SAW\_FRAME,Présent,Clause 9.2.9,-

#### 2\. Remédiation et Optimisation pour l'Architecture ESP32-S3

##### Élimination des délais bloquants

L'utilisation de vTaskDelay ou de boucles d'attente actives est proscrite au sein de la boucle FSM. Le respect du timing inter-octet et des timeouts de réception (Tframe\_abort) nécessite une approche non-bloquante. La remédiation impose l'utilisation des composants asynchrones genie\_timer (réf: genie\_timer.h), permettant de déclencher des transitions d'état basées sur des deltas de ticks système sans suspendre la tâche de communication.

##### Résolution du conflit d'API UART et intégrité des données

Sur l'ESP32-S3, l'accès concurrent aux registres de l'UART par plusieurs tâches applicatives est une source majeure de corruption de trames.

1. **Synchronisation :**  L'utilisation de Mutex FreeRTOS est obligatoire pour l'accès aux fonctions uart\_write\_bytes.  
2. **Prévention des pertes :**  Pour éviter les débordements de tampon (Buffer Overrun) lors de l'analyse des en-têtes (Header), l'implémentation doit utiliser un tampon circulaire (RingBuffer) ou une file d'attente (Queue) FreeRTOS pour découpler la réception matérielle de l'analyse logique de la FSM.

#### 3\. Spécification des 9 États Normatifs ASHRAE 135

##### Architecture de la FSM

1. **IDLE :**  Écoute du bus. Si le timer Tno\_token expire, basculement vers NO\_TOKEN.  
2. **WAIT\_FOR\_PROPORTIONAL :**  Délai de sécurité avant de générer un nouveau jeton.  
3. **USE\_TOKEN :**  Envoi des trames de données (jusqu'à Nmax\_info\_frames).  
4. **WAIT\_FOR\_REPLY :**  Attente d'une réponse après une requête confirmée.  
5. **DONE\_WITH\_TOKEN :**  Nettoyage post-émission et décision de passage de jeton.  
6. **PASS\_TOKEN :**  Envoi de la trame Token à la station suivante (NS).  
7. **NO\_TOKEN :**  Gestion de l'absence d'activité bus après un cycle Tusage\_timeout.  
8. **ANSWER\_DATA\_REQUEST :**  Traitement d'une trame reçue demandant une réponse immédiate.  
9. **SAW\_FRAME :**  Analyse de l'en-tête (Header CRC) pour déterminer la destination.

##### Logique de Transition (Remédiation)

* **NO\_TOKEN :**  
* *Entrante :*  Depuis IDLE si le timer dépasse Tusage\_timeout \+ (N \* Tslot).  
* *Sortante :*  Vers SAW\_FRAME si une activité est détectée ; vers WAIT\_FOR\_PROPORTIONAL pour réclamer le statut de maître unique.  
* **DONE\_WITH\_TOKEN :**  
* *Entrante :*  Depuis USE\_TOKEN après émission ou épuisement du quota Nmax\_info\_frames.  
* *Sortante :*  Vers PASS\_TOKEN inconditionnellement pour maintenir le cycle du bus.  
* **ANSWER\_DATA\_REQUEST :**  
* *Entrante :*  Depuis SAW\_FRAME si une trame valide est adressée au nœud local.  
* *Sortante :*  Vers IDLE uniquement après l'envoi complet de la trame REPLY (ou timeout de préparation).

#### 4\. Implémentation logicielle C++ (Asynchrone)

\#include "driver/uart.h"  
\#include "freertos/FreeRTOS.h"  
\#include "freertos/semphr.h"  
\#include "genie\_timer.h" // Utilisation des timers asynchrones du framework

enum class MstpState {  
    IDLE, WAIT\_FOR\_PROPORTIONAL, USE\_TOKEN, WAIT\_FOR\_REPLY,  
    DONE\_WITH\_TOKEN, PASS\_TOKEN, NO\_TOKEN, ANSWER\_DATA\_REQUEST, SAW\_FRAME  
};

class BacnetMstpNode {  
private:  
    MstpState state \= MstpState::NO\_TOKEN;  
    uart\_port\_t uart\_num;  
    SemaphoreHandle\_t uartMutex;  
    uint8\_t info\_frames\_sent \= 0;  
    const uint8\_t Nmax\_info\_frames \= 10; // Quota normatif

public:  
    BacnetMstpNode(uart\_port\_t port) : uart\_num(port) {  
        uartMutex \= xSemaphoreCreateMutex();  
    }

    void update() {  
        switch (state) {  
            case MstpState::SAW\_FRAME:  
                // Analyse du Header (5 octets) \+ Validation CRC8  
                if (validate\_header\_crc()) {  
                    // Si destination \== Local, transition vers traitement  
                    state \= MstpState::ANSWER\_DATA\_REQUEST;  
                } else {  
                    state \= MstpState::IDLE;  
                }  
                break;

            case MstpState::USE\_TOKEN:  
                /\* Traitement des trames de données sortantes.  
                   Chaque envoi doit respecter le timing inter-octet   
                   et valider le CRC16 de la charge utile. \*/  
                if (info\_frames\_sent \< Nmax\_info\_frames && has\_queued\_data()) {  
                    send\_data\_frame();  
                    info\_frames\_sent++;  
                } else {  
                    state \= MstpState::DONE\_WITH\_TOKEN;  
                }  
                break;

            case MstpState::ANSWER\_DATA\_REQUEST:  
                /\* Préparation de la trame REPLY. L'accès UART est protégé  
                   par Mutex pour éviter les collisions avec les logs ou  
                   d'autres tâches sur l'ESP32-S3. \*/  
                if (xSemaphoreTake(uartMutex, 0\) \== pdTRUE) {  
                    send\_immediate\_reply();   
                    xSemaphoreGive(uartMutex);  
                }  
                state \= MstpState::IDLE;  
                break;

            case MstpState::DONE\_WITH\_TOKEN:  
                info\_frames\_sent \= 0;  
                state \= MstpState::PASS\_TOKEN;  
                break;

            case MstpState::NO\_TOKEN:  
                // Utilisation de genie\_timer pour vérifier le timeout Tno\_token  
                if (genie\_timer\_is\_expired(Tno\_token\_id)) {  
                    state \= MstpState::WAIT\_FOR\_PROPORTIONAL;  
                } else if (bus\_activity\_detected()) {  
                    state \= MstpState::SAW\_FRAME;  
                }  
                break;

            default:  
                // Gestion IDLE et PASS\_TOKEN  
                break;  
        }  
    }

    bool validate\_header\_crc() { /\* Logique CRC8 \*/ return true; }  
    void send\_data\_frame() { /\* uart\_write\_bytes... \*/ }  
    void send\_immediate\_reply() { /\* Envoi prioritaire \*/ }  
    bool has\_queued\_data() { return false; }  
    bool bus\_activity\_detected() { return false; }  
};

#### 5\. Synthèse de Validation et Conformité

##### Vérification BIBBs (BACnet Interoperability Building Blocks)

L'implémentation de l'état ANSWER\_DATA\_REQUEST est le verrou technique permettant d'activer le BIBB  **DS-RP-B**  (Data Sharing \- ReadProperty \- B). Selon la section "The New Idea" de David Fisher, le rôle de "Server" (B) impose que le dispositif réponde aux requêtes de lecture de propriétés. Sans une FSM capable de passer en mode réponse immédiate sur la couche liaison de données, le support applicatif des services de partage de données reste inopérant, car la requête physique ne sera jamais acquittée.

##### Conclusion technique

La remédiation de la FSM est impérative pour garantir :

1. **La stabilité temporelle :**  Prévention des collisions et respect des slots de silence sur le média RS-485.  
2. **L'interopérabilité native :**  Conformité stricte aux 9 états ASHRAE 135, condition sine qua non pour l'obtention du  **BTL Mark** .  
3. **L'évolutivité :**  Support complet des services serveurs requis pour les profils de dispositifs BACnet (B-ASC ou B-AAC).

