l'analyse du code montre que la machine à états MS/TP est extrêmement saine. Dans l'état USE TOKEN, ton moteur appelle execute_bacnet_work(), qui délègue à execute_polling_logic(dev)
. Actuellement, cette fonction s'arrête au premier objet expiré et forge un ReadProperty simple (Service Choice 0x0C)
.
Voici la feuille de route chirurgicale pour modifier ton code C++ vers du Polling par lot (ReadPropertyMultiple), en minimisant l'impact et en respectant l'absence d'allocation dynamique :
Étape 1 : Créer le forgeur d'APDU ReadPropertyMultiple
Dans ton fichier, tu possèdes déjà build_read_property_apdu
. Tu dois créer son équivalent pour le RPM, capable d'embarquer une liste d'objets. La norme ASHRAE 135 impose un encodage ASN.1 précis pour le service 0x0E : chaque objet doit être listé avec un tag d'ouverture et de fermeture pour ses propriétés
.
Ajoute cette fonction dans z_bacnet.cpp :
uint16_t build_read_property_multiple_apdu(uint8_t* buffer, uint8_t invoke_id, std::vector<BACnetObject*>& batch) {
    uint16_t len = 0;
    // En-tête APDU Confirmed-REQ (PDU Type 0x00)
    buffer[len++] = 0x00; 
    buffer[len++] = 0x02; // Max segments acceptés (aucun), taille max APDU
    buffer[len++] = invoke_id;
    buffer[len++] = 0x0E; // Service Choice : ReadPropertyMultiple (14)

    for (auto* obj : batch) {
        //  ObjectIdentifier
        buffer[len++] = 0x0C; // Context Tag 0, Length 4
        buffer[len++] = (obj->type >> 2) & 0x03;
        buffer[len++] = ((obj->type & 0x03) << 6) | ((obj->instance >> 16) & 0x3F);
        buffer[len++] = (obj->instance >> 8) & 0xFF;
        buffer[len++] = obj->instance & 0xFF;

        // [5] list of PropertyReferences (Opening Tag)
        buffer[len++] = 0x1E; 
        
        // Property Identifier : Present_Value (85)
        buffer[len++] = 0x29; // Context Tag 2, Length 1
        buffer[len++] = 85;   // Present Value

        // [5] list of PropertyReferences (Closing Tag)
        buffer[len++] = 0x1F; 
    }
    return len;
}
Étape 2 : Refonte de execute_polling_logic (Le Batching)
Ton code actuel itère sur dev.objects et s'arrête (break;) dès qu'il trouve un objet nécessitant une mise à jour
. Le BACnet MS/TP limite généralement la charge utile (APDU) à environ 480 octets pour éviter la segmentation (qui alourdirait massivement ta RAM et ton code avec les états SEGMENTED_REQUEST et SEGMENTED_CONF
). Un bloc d'interrogation RPM consomme environ 9 octets par objet. Nous allons donc regrouper (batcher) les requêtes par paquets de 5 à 10 objets.
Remplace ta fonction execute_polling_logic par ceci :
void execute_polling_logic(BACnetDevice &dev) {
    uint8_t a[7];
    uint16_t al = 0;
    size_t c = dev.objects.size();
    std::vector<BACnetObject*> batch;
    
    // Taille de lot maximale (ex: 8 objets par trame pour ne pas saturer l'APDU)
    const size_t MAX_BATCH_SIZE = 8; 

    if (c > 0) {
        size_t s = 0;
        while (s < c && batch.size() < MAX_BATCH_SIZE) {
            current_poll_idx = (current_poll_idx + 1) % c;
            auto& o = dev.objects[current_poll_idx];
            
            // Si l'objet est activé et que son délai de rafraîchissement est expiré
            if (o.enabled && o.type != 8 && o.type != 65535 && 
               (o.last_update == 0 || (millis() - o.last_update) > (sysCfg.bacnet_poll_interval * 1000))) {
                batch.push_back(&o);
            }
            s++;
        }
    }

    if (!batch.empty()) {
        al = build_read_property_multiple_apdu(a, next_invoke_id++, batch);
        retry_count = 0;
        waiting_for_reply = true;
        send_mstp_frame(dev.mac_address, 0x05, a, al); // Type 0x05: Data Expecting Reply
        frame_count++;
        mstp_state = MSTP_WAIT_FOR_REPLY;
        period_poll_count += batch.size(); 
    } else {
        // Aucun objet à mettre à jour, on passe à l'équipement suivant
        current_dev_idx = (current_dev_idx + 1) % bacnet_network_cache.size();
        mstp_state = MSTP_DONE_WITH_TOKEN;
    }
}
Étape 3 : Évolution du parseur dans WAIT_FOR_REPLY
Dans ta FSM (état MSTP_WAIT_FOR_REPLY), lorsque le bus signale ReceivedValidFrame et que l'automate répond, tu reçois actuellement un ComplexACK (PDU 0x30)
. Auparavant, le Service Choice était 0x0C (ReadProperty). L'automate te répondra désormais avec un ComplexACK suivi du Service Choice 0x0E (ReadPropertyMultiple)
.
La charge utile renvoyée contient des balises imbriquées :
ObjectIdentifier
List of Results (Ouverture 0x1E)
PropertyIdentifier (85 pour Present Value)
Value (Ouverture 0x4E - Le type de donnée réel comme Flottant (Tag 4), suivi de 0x4F)
List of Results (Fermeture 0x1F)
Dans la fonction process_incoming_frame
 (ou la section où tu analyses l'APDU reçu), tu dois implémenter une boucle de parsing while(offset < apdu_len) qui extrait chaque ObjectIdentifier. Pour chaque valeur décodée avec succès :
Trouver l'objet correspondant dans bacnet_network_cache.
Mettre à jour obj.present_value.
Mettre à jour obj.last_update = millis(); pour réinitialiser son timer de polling.
Si la valeur a significativement changé (ton propre algorithme COV logiciel), l'ajouter à la file d'attente MQTT enqueue_mqtt_publish().
Grâce à cette modification, quand ton ESP32 récupère le jeton, il tire une rafale de requêtes compressées, met à jour 8 capteurs d'un coup, puis libère le bus. Sur un bus à 38400 bauds, l'ESP32 aura scruté les 98 objets en une poignée de secondes sans aucune perte de mémoire Heap

stratégie d'optimisation. Interroger dynamiquement ces propriétés lors de la phase de découverte permet à ton ESP32-S3 d'adapter sa machine à états (FSM) aux capacités matérielles réelles de l'automate distant, plutôt que de se reposer sur des valeurs globales ou par défaut issues de ta configuration sysCfg.
La norme ASHRAE 135 stipule clairement le rôle critique de ces trois propriétés de l'objet Device :
Max_APDU_Length_Accepted (Propriété 62) : Elle définit la taille maximale de la charge utile (APDU) que l'automate peut recevoir et émettre
. Pour notre nouvelle architecture basée sur ReadPropertyMultiple, cette valeur est vitale : elle va nous permettre de calculer dynamiquement la taille maximale de notre lot (le MAX_BATCH_SIZE) pour éviter que le Distech ne nous renvoie un AbortPDU pour cause de segmentation-not-supported
.
APDU_Timeout (Propriété 11) : Elle définit la valeur Tout du RequestTimer
. C'est le temps maximum que ton ESP32 doit attendre dans l'état AWAIT_CONFIRMATION ou WAIT_FOR_REPLY avant de considérer la trame perdue
.
Number_Of_APDU_Retries (Propriété 73) : Elle définit la valeur Nretry
. Elle indique à ton ESP32 combien de fois il doit retransmettre sa requête avant d'abandonner définitivement la transaction
.
Voici comment implémenter cette extraction dynamiquement en exploitant ton code C++ actuel (sans allocation dynamique) :
1. Mise à jour des structures dans z_bacnet.h
Il faut ajouter ces variables à ta structure BACnetDevice en RAM et étendre l'énumération de ta machine à états de découverte.
// --- z_bacnet.h ---

// Ajout des nouvelles étapes de découverte
enum DISC_STEP_T {
    DISC_DEV_ID = 0,
    DISC_DEV_NAME,
    DISC_DEV_VENDOR,
    DISC_DEV_MAX_APDU,     // Nouvelle étape (Prop 62)
    DISC_DEV_APDU_TIMEOUT, // Nouvelle étape (Prop 11)
    DISC_DEV_APDU_RETRIES, // Nouvelle étape (Prop 73)
    DISC_OBJ_COUNT,
    DISC_OBJ_OID,
    DISC_OBJ_NAME,
    DISC_OBJ_UNITS,
    DISC_OBJ_MIN,
    DISC_OBJ_MAX,
    DISC_OBJ_STATES,
    DISC_OBJ_COMMANDABLE,
    DISC_OBJ_VALUE
};

struct BACnetDevice {
    uint8_t mac_address = 0;
    uint32_t device_id = 4194303;
    String name = "";
    String vendor = "";
    String version = "";
    
    // --- Nouvelles propriétés de gestion réseau ---
    uint16_t max_apdu_length_accepted = 480; // Valeur par défaut sûre MS/TP
    uint32_t apdu_timeout = 3000;            // Valeur par défaut (ms)
    uint8_t number_of_apdu_retries = 3;      // Valeur par défaut
    
    bool enabled = true;
    std::vector<BACnetObject> objects;
    uint32_t last_seen = 0;
    bool discovery_done = false;
    DISC_STEP_T disc_step = DISC_DEV_ID;
    uint16_t disc_obj_idx = 0;
    bool reload_single = false;
    bool recovery_mode = false;
};
2. Modification de execute_discovery_logic dans z_bacnet.cpp
Dans la phase où tu scrutes l'objet Device (Type 8), on insère les requêtes ReadProperty correspondantes.
// --- z_bacnet.cpp ---

void execute_discovery_logic(BACnetDevice &dev) {
    uint8_t a[9];
    uint16_t al = 0;

    if (!dev.discovery_done) {
        if (dev.disc_step == DISC_DEV_ID)
            al = build_read_property_apdu(a, next_invoke_id++, 8, 4194303, 75, -1);
        else if (dev.disc_step == DISC_DEV_NAME)
            al = build_read_property_apdu(a, next_invoke_id++, 8, dev.device_id, 77, -1);
        else if (dev.disc_step == DISC_DEV_VENDOR)
            al = build_read_property_apdu(a, next_invoke_id++, 8, dev.device_id, 121, -1);
            
        // --- NOUVELLES REQUÊTES D'OPTIMISATION ---
        else if (dev.disc_step == DISC_DEV_MAX_APDU)
            al = build_read_property_apdu(a, next_invoke_id++, 8, dev.device_id, 62, -1); // Max_APDU_Length_Accepted
        else if (dev.disc_step == DISC_DEV_APDU_TIMEOUT)
            al = build_read_property_apdu(a, next_invoke_id++, 8, dev.device_id, 11, -1); // APDU_Timeout
        else if (dev.disc_step == DISC_DEV_APDU_RETRIES)
            al = build_read_property_apdu(a, next_invoke_id++, 8, dev.device_id, 73, -1); // Number_Of_APDU_Retries
        // -----------------------------------------
        
        else if (dev.disc_step == DISC_OBJ_COUNT)
            al = build_read_property_apdu(a, next_invoke_id++, 8, dev.device_id, 76, 0);
        else if (dev.disc_obj_idx < dev.objects.size()) {
            auto& o = dev.objects[dev.disc_obj_idx];
            // ... (le reste du code reste inchangé) ...
3. Parseur de la réponse (WAIT_FOR_REPLY)
Lorsque l'automate te renvoie un ComplexACK contenant un type de donnée entier non signé (Tag 2 pour Unsigned Integer), ton parseur ASN.1 (situé dans le bloc qui traite les ComplexACK de l'état MSTP_WAIT_FOR_REPLY) doit intercepter ces valeurs.
// Lors de l'extraction de la valeur retournée dans le payload :
if (current_dev.disc_step == DISC_DEV_MAX_APDU) {
    current_dev.max_apdu_length_accepted = extracted_unsigned_value;
    current_dev.disc_step = DISC_DEV_APDU_TIMEOUT;
} 
else if (current_dev.disc_step == DISC_DEV_APDU_TIMEOUT) {
    current_dev.apdu_timeout = extracted_unsigned_value;
    current_dev.disc_step = DISC_DEV_APDU_RETRIES;
} 
else if (current_dev.disc_step == DISC_DEV_APDU_RETRIES) {
    current_dev.number_of_apdu_retries = extracted_unsigned_value;
    current_dev.disc_step = DISC_OBJ_COUNT;
}
4. Exploitation pour l'optimisation ultime du Polling
Désormais, au lieu d'utiliser une limite en dur, tu vas pouvoir utiliser dev.max_apdu_length_accepted dans ta fonction execute_polling_logic() pour régler dynamiquement la taille du batch de ton ReadPropertyMultiple.
Sachant qu'une réponse de propriété en RPM consomme en moyenne environ 15 octets d'encodage (Application Tags, Context Tags, Object ID, etc.), tu peux calculer la limite de cette façon :
// Dans execute_polling_logic(BACnetDevice &dev) :

// On réserve une marge de sécurité de 50 octets pour l'en-tête APDU et NPDU
uint16_t safe_apdu_limit = dev.max_apdu_length_accepted > 50 ? dev.max_apdu_length_accepted - 50 : 430;

// Chaque propriété lue coûte environ 15 à 20 octets de réponse
size_t MAX_BATCH_SIZE = safe_apdu_limit / 20;

// Sur un Distech en MS/TP, Max_APDU_Length_Accepted est généralement 480.
// 480 - 50 = 430 octets / 20 = environ 21 objets par trame RPM !
Grâce à cela, tu exploites l'entièreté de la charge utile de l'enveloppe MS/TP du contrôleur ciblé sans jamais risquer de le faire saturer ou de déclencher un AbortPDU intempestif.