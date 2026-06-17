/A# **Modifications Chirurgicales pour src/z\_mqtt.cpp**

Voici les **5 modifications ciblées** à intégrer dans ton code d'origine pour activer le décodage des commandes de hack, publier l'état de contournement de tes capteurs et enregistrer les entités complémentaires dans Home Assistant.

### **1\. Souscription au topic OutOfService spécifique**

Dans mqtt\_event\_handler, à l'intérieur du cas MQTT\_EVENT\_CONNECTED, il faut souscrire au nouveau topic dédié aux Analog Inputs (AI).

**Rechercher dans ton code :**

                char sub\_topic\[128\];  
                snprintf(sub\_topic, sizeof(sub\_topic), "%s/+/+/+/set", sysCfg.mqtt\_prefix);  
                esp\_mqtt\_client\_subscribe(mqtt\_client, sub\_topic, 0);  
                  
                // Signal pour la découverte HA complète suite à reconnexion

**Remplacer par :**

                char sub\_topic\[128\];  
                snprintf(sub\_topic, sizeof(sub\_topic), "%s/+/+/+/set", sysCfg.mqtt\_prefix);  
                esp\_mqtt\_client\_subscribe(mqtt\_client, sub\_topic, 0);  
                  
                // AJOUT CHIRURGICAL : Souscription aux commandes de contournement OutOfService (AI uniquement)  
                // Raison : Permet à l'ESP32 d'intercepter les demandes de débrayage (ON/OFF) envoyées par l'IHM HA.  
                char sub\_topic\_oos\[128\];  
                snprintf(sub\_topic\_oos, sizeof(sub\_topic\_oos), "%s/+/+/+/outofservice/set", sysCfg.mqtt\_prefix);  
                esp\_mqtt\_client\_subscribe(mqtt\_client, sub\_topic\_oos, 0);  
                  
                // Signal pour la découverte HA complète suite à reconnexion

### **2\. Décodage de la commande de hack (/outofservice/set) et ajout du type AI**

Dans mqtt\_event\_handler, à l'intérieur du cas MQTT\_EVENT\_DATA, nous devons :

* Déterminer si la commande s'adresse à prop\_id \= 96 (Out of Service) ou 85 (Present Value).  
* Autoriser les requêtes sur le type "AI" (Analog Input).  
* Adapter le décodage de la charge utile (payload\_buf) si l'action vise la propriété 96\.

**Rechercher dans ton code :**

                        if (xSemaphoreTake(cache\_mutex, pdMS\_TO\_TICKS(100))) {  
                            for (auto& d : bacnet\_network\_cache) {  
                                if (d.ulDeviceId \== ulDeviceId) {   
                                    target\_mac \= d.ucMacAddress;   
                                    found \= true;   
                                      
                                    BACnetJob job;  
                                    job.type \= JOB\_WRITE\_PROP;  
                                    job.target\_mac \= target\_mac;  
                                    job.obj\_instance \= t.substring(p3 \+ 1, p4).toInt();  
                                    job.prop\_id \= 85;  
                                    String type\_str \= t.substring(p2 \+ 1, p3);  
                                      
                                    if (type\_str \== "AO" || type\_str \== "AV") job.obj\_type \= (type\_str \== "AO") ? 1 : 2;  
                                    else if (type\_str \== "BO" || type\_str \== "BV") job.obj\_type \= (type\_str \== "BO") ? 4 : 5;  
                                    else if (type\_str \== "MSO" || type\_str \== "MSV") job.obj\_type \= (type\_str \== "MSO") ? 14 : 19;  
                                    else { job.type \= JOB\_WHO\_IS; found \= false; }

                                    if (found) {  
                                        if (job.obj\_type \== 14 || job.obj\_type \== 19\) {

**Remplacer par :**

                        if (xSemaphoreTake(cache\_mutex, pdMS\_TO\_TICKS(100))) {  
                            for (auto& d : bacnet\_network\_cache) {  
                                if (d.ulDeviceId \== ulDeviceId) {   
                                    target\_mac \= d.ucMacAddress;   
                                    found \= true;   
                                      
                                    BACnetJob job;  
                                    job.type \= JOB\_WRITE\_PROP;  
                                    job.target\_mac \= target\_mac;  
                                    job.obj\_instance \= t.substring(p3 \+ 1, p4).toInt();

                                    // AJOUT CHIRURGICAL : Détermination de l'action de forçage OutOfService  
                                    // Raison : Si un 5ème slash est détecté avant "/set" et cible "outofservice", le job s'adresse à la propriété 96 (PROP\_OUT\_OF\_SERVICE).  
                                    int p5 \= t.indexOf('/', p4 \+ 1);  
                                    if (p5 \> 0 && t.substring(p4 \+ 1, p5) \== "outofservice") {  
                                        job.prop\_id \= 96;  
                                    } else {  
                                        job.prop\_id \= 85;  
                                    }  
                                      
                                    String type\_str \= t.substring(p2 \+ 1, p3);  
                                      
                                    if (type\_str \== "AO" || type\_str \== "AV") job.obj\_type \= (type\_str \== "AO") ? 1 : 2;  
                                    else if (type\_str \== "BO" || type\_str \== "BV") job.obj\_type \= (type\_str \== "BO") ? 4 : 5;  
                                    else if (type\_str \== "MSO" || type\_str \== "MSV") job.obj\_type \= (type\_str \== "MSO") ? 14 : 19;  
                                    // AJOUT CHIRURGICAL : Prise en charge des entrées analogiques (AI \= 0\) en écriture  
                                    // Raison : Nécessaire pour pouvoir envoyer la valeur de consigne (Forcing Value) ou modifier l'OutOfService sur une AI.  
                                    else if (type\_str \== "AI") job.obj\_type \= 0;  
                                    else { job.type \= JOB\_WHO\_IS; found \= false; }

                                    if (found) {  
                                        // MODIFICATION CHIRURGICALE : Décodage de la payload selon la propriété cible  
                                        // Raison : Si c'est l'écriture OutOfService (96), on traduit "ON" et "OFF" en 1.0f ou 0.0f. Sinon, on utilise la logique standard pour la valeur (85).  
                                        if (job.prop\_id \== 96\) {  
                                            job.write\_value \= (String(payload\_buf) \== "ON") ? 1.0f : 0.0f;  
                                        } else if (job.obj\_type \== 14 || job.obj\_type \== 19\) {

### **3\. Routage dynamique des topics de publication dans le Gatekeeper**

Dans mqtt\_gatekeeper\_task, nous devons router la trame vers le topic /outofservice si pubJob.prop\_id \== 96\.

**Rechercher dans ton code :**

                const char\* subtopic \= (pubJob.prop\_id \== 77\) ? "name" : "state";  
                snprintf(topic, sizeof(topic), "%s/%lu/%s/%lu/%s", sysCfg.mqtt\_prefix, (unsigned long)pubJob.ulDeviceId, t\_str, (unsigned long)pubJob.obj\_instance, subtopic);

**Remplacer par :**

                // MODIFICATION CHIRURGICALE : Routage vers le bon sous-topic en fonction du prop\_id  
                // Raison : La propriété 77 publie sur "/name", la propriété 96 (Status Flags / OutOfService) sur "/outofservice", sinon sur "/state".  
                const char\* subtopic \= "state";  
                if (pubJob.prop\_id \== 77\) subtopic \= "name";  
                else if (pubJob.prop\_id \== 96\) subtopic \= "outofservice";

                snprintf(topic, sizeof(topic), "%s/%lu/%s/%lu/%s", sysCfg.mqtt\_prefix, (unsigned long)pubJob.ulDeviceId, t\_str, (unsigned long)pubJob.obj\_instance, subtopic);

### **4\. Déclaration automatique des entités de hack dans Home Assistant**

Dans publish\_ha\_autodiscovery, dès qu'une entité de type AI est configurée avec succès, nous devons pousser immédiatement la configuration du commutateur et du curseur pour cette sonde.

**Rechercher dans ton code :**

                String payload; serializeJson(doc, payload);  
                if (esp\_mqtt\_client\_publish(mqtt\_client, topic, payload.c\_str(), payload.length(), 1, 1\) \< 0\) {  
                    z\_log(pdLOG\_WARN, "MQTT", "Discovery publish failed for %s. Outbox full?\\n", uniq\_id);  
                    vTaskDelay(pdMS\_TO\_TICKS(100)); // Pause si erreur  
                } else {  
                    total\_published++;  
                }  
                  
            } else if ((\!dev\_enabled || \!obj\_enabled) && dev\_discovery\_done) {

**Remplacer par :**

                String payload; serializeJson(doc, payload);  
                if (esp\_mqtt\_client\_publish(mqtt\_client, topic, payload.c\_str(), payload.length(), 1, 1\) \< 0\) {  
                    z\_log(pdLOG\_WARN, "MQTT", "Discovery publish failed for %s. Outbox full?\\n", uniq\_id);  
                    vTaskDelay(pdMS\_TO\_TICKS(100)); // Pause si erreur  
                } else {  
                    total\_published++;  
                }

                // AJOUT CHIRURGICAL : Déclaration des entités de contournement (hack) pour les Analog Inputs (AI)  
                // Raison : Crée automatiquement un switch "Out of Service" et un curseur "Forcing Value" dans HA liés au même appareil physique.  
                if (obj\_type \== OBJ\_ANALOG\_INPUT) {  
                    // \--- 1\. Création de l'entité SWITCH pour "Out of Service" \---  
                    {  
                        JsonDocument sw\_doc;  
                        char sw\_uniq\_id\[128\];  
                        snprintf(sw\_uniq\_id, sizeof(sw\_uniq\_id), "bacnet\_%lu\_AI\_%lu\_outofservice", (unsigned long)current\_did, (unsigned long)obj\_inst);  
                        char sw\_config\_topic\[128\];  
                        snprintf(sw\_config\_topic, sizeof(sw\_config\_topic), "homeassistant/switch/%s/config", sw\_uniq\_id);

                        sw\_doc\["uniq\_id"\] \= String(sw\_uniq\_id);  
                        sw\_doc\["name"\] \= String(obj\_name) \+ " Out of Service";  
                        sw\_doc\["icon"\] \= "mdi:sensor-off";  
                        sw\_doc\["avty\_t"\] \= String(lwt\_topic);  
                        sw\_doc\["pl\_avail"\] \= "online";  
                        sw\_doc\["pl\_not\_avail"\] \= "offline";  
                          
                        char sw\_state\_topic\[128\];  
                        snprintf(sw\_state\_topic, sizeof(sw\_state\_topic), "%s/outofservice", base\_topic);  
                        sw\_doc\["stat\_t"\] \= String(sw\_state\_topic);

                        char sw\_cmd\_topic\[128\];  
                        snprintf(sw\_cmd\_topic, sizeof(sw\_cmd\_topic), "%s/outofservice/set", base\_topic);  
                        sw\_doc\["cmd\_t"\] \= String(sw\_cmd\_topic);

                        sw\_doc\["pl\_on"\] \= "ON";  
                        sw\_doc\["pl\_off"\] \= "OFF";

                        // Raccordement au même Device HA  
                        JsonObject sw\_device \= sw\_doc\["dev"\].to\<JsonObject\>();  
                        JsonArray sw\_ids \= sw\_device\["ids"\].to\<JsonArray\>();  
                        sw\_ids.add(String(dev\_id\_str));  
                        sw\_device\["name"\] \= dev\_name.length() \> 0 ? String(dev\_name) : String(dev\_id\_str);  
                        sw\_device\["mf"\] \= dev\_vendor.length() \> 0 ? String(dev\_vendor) : "BACnet Manufacturer";  
                        sw\_device\["sw"\] \= configVERSION\_GLOBAL;

                        String sw\_payload;  
                        serializeJson(sw\_doc, sw\_payload);  
                        esp\_mqtt\_client\_publish(mqtt\_client, sw\_config\_topic, sw\_payload.c\_str(), sw\_payload.length(), 1, 1);  
                        total\_published++;  
                    }

                    // \--- 2\. Création de l'entité NUMBER pour "Forcing Value" \---  
                    {  
                        JsonDocument num\_doc;  
                        char num\_uniq\_id\[128\];  
                        snprintf(num\_uniq\_id, sizeof(num\_uniq\_id), "bacnet\_%lu\_AI\_%lu\_forcing", (unsigned long)current\_did, (unsigned long)obj\_inst);  
                        char num\_config\_topic\[128\];  
                        snprintf(num\_config\_topic, sizeof(num\_config\_topic), "homeassistant/number/%s/config", num\_uniq\_id);

                        num\_doc\["uniq\_id"\] \= String(num\_uniq\_id);  
                        num\_doc\["name"\] \= String(obj\_name) \+ " Forcing Value";  
                        num\_doc\["icon"\] \= "mdi:thermometer-cog";  
                        num\_doc\["avty\_t"\] \= String(lwt\_topic);  
                        num\_doc\["pl\_avail"\] \= "online";  
                        num\_doc\["pl\_not\_avail"\] \= "offline";  
                          
                        num\_doc\["stat\_t"\] \= "\~/state";   
                        num\_doc\["cmd\_t"\] \= "\~/set";

                        float final\_min \= isnan(obj\_min) ? sysCfg.default\_number\_min : obj\_min;  
                        float final\_max \= isnan(obj\_max) ? sysCfg.default\_number\_max : obj\_max;  
                        num\_doc\["min"\] \= final\_min;  
                        num\_doc\["max"\] \= final\_max;  
                        num\_doc\["step"\] \= obj\_step;

                        String unit \= String(obj\_unit\_text);  
                        if (unit \== "Unknown" || unit.length() \== 0 || unit \== "none") unit \= get\_unit\_text(obj\_units);  
                        if (unit \!= "no-units" && unit.length() \> 0\) {  
                            if (unit \== "%RH") unit \= "%";  
                            num\_doc\["unit\_of\_meas"\] \= unit;  
                        }

                        // Raccordement au même Device HA  
                        JsonObject num\_device \= num\_doc\["dev"\].to\<JsonObject\>();  
                        JsonArray num\_ids \= num\_device\["ids"\].to\<JsonArray\>();  
                        num\_ids.add(String(dev\_id\_str));  
                        num\_device\["name"\] \= dev\_name.length() \> 0 ? String(dev\_name) : String(dev\_id\_str);  
                        num\_device\["mf"\] \= dev\_vendor.length() \> 0 ? String(dev\_vendor) : "BACnet Manufacturer";  
                        num\_device\["sw"\] \= configVERSION\_GLOBAL;

                        String num\_payload;  
                        serializeJson(num\_doc, num\_payload);  
                        esp\_mqtt\_client\_publish(mqtt\_client, num\_config\_topic, num\_payload.c\_str(), num\_payload.length(), 1, 1);  
                        total\_published++;  
                    }  
                }  
                  
            } else if ((\!dev\_enabled || \!obj\_enabled) && dev\_discovery\_done) {

### **5\. Filtrage et synchronisation d'état lors de la publication d'un objet**

Dans publish\_mqtt\_topic, nous devons :

* Bloquer l'émission de la Present\_Value si le hack est actif pour éviter de polluer l'historique HA (les fameux \-1.0°C).  
* Gérer l'émission formatée "ON"/"OFF" pour la propriété 96 (OutOfService).  
* Déclencher l'auto-publication de l'état du switch 96 de façon synchrone quand la valeur d'une AI est actualisée.

**Rechercher dans ton code (Début de fonction) :**

void publish\_mqtt\_topic(uint32\_t ulDeviceId, BACnetObject& obj, uint8\_t prop\_id, bool retain) {  
    if (\!obj.xEnabled) return;  
    MQTTPublishJob pub;

**Remplacer par (Début de fonction) :**

void publish\_mqtt\_topic(uint32\_t ulDeviceId, BACnetObject& obj, uint8\_t prop\_id, bool retain) {  
    if (\!obj.xEnabled) return;

    // MODIFICATION CHIRURGICALE : Blocage de la publication de la Present\_Value physique pour les sondes isolées  
    // Raison : Évite d'envoyer la fausse valeur brute (-1°C) à HA quand l'Out\_Of\_Service est à true.  
    if (prop\_id \== 85 && obj.usType \== OBJ\_ANALOG\_INPUT && obj.isOutOfService()) {  
        return;   
    }

    MQTTPublishJob pub;

**Rechercher dans ton code (Fin de fonction) :**

            default:  
                snprintf(pub.value\_string, sizeof(pub.value\_string), "%.2f", obj.fPresentValue);  
                break;  
        }  
    } else return;

    enqueue\_mqtt\_publish(pub);  
}

**Remplacer par (Fin de fonction) :**

            default:  
                snprintf(pub.value\_string, sizeof(pub.value\_string), "%.2f", obj.fPresentValue);  
                break;  
        }  
    }   
    // MODIFICATION CHIRURGICALE : Formater la payload pour la propriété 96 (OutOfService)  
    // Raison : Traduit l'état de contournement de la RAM en "ON" ou "OFF" conforme pour l'interrupteur HA.  
    else if (prop\_id \== 96\) {  
        strlcpy(pub.value\_string, obj.isOutOfService() ? "ON" : "OFF", sizeof(pub.value\_string));  
    } else return;

    enqueue\_mqtt\_publish(pub);

    // AJOUT CHIRURGICAL : Synchronisation de publication automatique du statut du switch  
    // Raison : Permet de pousser simultanément l'état du switch OutOfService dès que la valeur de la sonde (85) est actualisée.  
    if (prop\_id \== 85 && obj.usType \== OBJ\_ANALOG\_INPUT) {  
        publish\_mqtt\_topic(ulDeviceId, obj, 96, retain);  
    }  
}  
