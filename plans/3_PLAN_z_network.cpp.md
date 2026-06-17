# **Modifications Chirurgicales pour src/z\_network.cpp**

Voici les **2 modifications ciblées** à intégrer dans ton code d'origine pour exposer les points d'entrée HTTP (API REST) permettant de piloter le mode Out\_Of\_Service et d'injecter des valeurs arbitraires avec gestion de priorité BACnet.

### **1\. Ajout de la route API /api/outofservice**

Dans la fonction setup\_web\_routes(), juste après la route API existante /api/toggle\_device et avant la route /save, il faut insérer notre premier endpoint.

**Remplacer par :**

    // Route API pour activer ou désactiver un équipement entier.  
    webServer.on("/api/toggle\_device", HTTP\_POST, \[\](AsyncWebServerRequest \*request){  
        if(\!is\_authenticated(request)) return;  
        if (request-\>hasParam("id", true)) {  
            // \[ ... code interne existant ... \]  
            request-\>send(200, "text/plain", "OK");  
        } else request-\>send(400, "text/plain", "Missing id");  
    });

    // \=========================================================================  
    // AJOUT CHIRURGICAL 1/2 : Route API pour forcer l'état OutOfService d'un objet (Hack Clim)  
    // Paramètres attendus : did (Device ID), inst (Instance ID), state (1/0, true/false, ON/OFF)  
    // \=========================================================================  
    webServer.on("/api/outofservice", HTTP\_POST, \[\](AsyncWebServerRequest \*request){  
        if(\!is\_authenticated(request)) return;  
          
        if (request-\>hasParam("did", true) && request-\>hasParam("inst", true) && request-\>hasParam("state", true)) {  
            uint32\_t did \= request-\>getParam("did", true)-\>value().toInt();  
            uint32\_t inst \= request-\>getParam("inst", true)-\>value().toInt();  
              
            String state\_str \= request-\>getParam("state", true)-\>value();  
            bool state \= (state\_str \== "1" || state\_str.equalsIgnoreCase("true") || state\_str.equalsIgnoreCase("on"));  
              
            uint16\_t type \= request-\>hasParam("type", true) ? request-\>getParam("type", true)-\>value().toInt() : 0;   
              
            bool device\_found \= false;  
            BACnetJob job;  
            memset(\&job, 0, sizeof(BACnetJob)); // Purge la mémoire pour éviter les valeurs parasites  
              
            job.type \= JOB\_WRITE\_PROP;   
            job.obj\_type \= type;  
            job.obj\_instance \= inst;  
            job.prop\_id \= 96; // PROP\_OUT\_OF\_SERVICE  
            job.write\_value \= state ? 1.0f : 0.0f;  
            job.priority \= 8; // Priorité 8 (Manuel) par défaut pour le débrayage  
            job.target\_mac \= 255;  
              
            if (xSemaphoreTake(cache\_mutex, pdMS\_TO\_TICKS(500))) {  
                for (auto& dev : bacnet\_network\_cache) {  
                    if (dev.ulDeviceId \== did) {  
                        job.target\_mac \= dev.ucMacAddress;  
                        device\_found \= true;  
                        break;  
                    }  
                }  
                xSemaphoreGive(cache\_mutex);  
            }  
              
            if (device\_found) {  
                enqueue\_bacnet\_job(job);  
                z\_log(pdLOG\_INFO, "API", "OutOfService %s ENQUEUED for %u:%lu\\n", state ? "ON" : "OFF", type, (unsigned long)inst);  
                request-\>send(200, "text/plain", "OUT\_OF\_SERVICE ENQUEUED");  
            } else {  
                request-\>send(404, "text/plain", "Device not found");  
            }  
        } else {  
            request-\>send(400, "text/plain", "Missing params (requires: did, inst, state)");  
        }  
    });

### **2\. Ajout de la route API /api/writevalue avec paramètre priority**

Juste en dessous de /api/outofservice, on ajoute la route d'écriture générique qui extrait le paramètre optionnel priority et l'affecte au job BACnet.

**À insérer immédiatement à la suite de l'ajout précédent :**

    // \=========================================================================  
    // AJOUT CHIRURGICAL 2/2 : Route API pour écrire une valeur générique (Present\_Value ou autre)  
    // Paramètres attendus : did, type, inst, prop, val, \[priority\] (Optionnel, ex: 8\)  
    // \=========================================================================  
    webServer.on("/api/writevalue", HTTP\_POST, \[\](AsyncWebServerRequest \*request){  
        if(\!is\_authenticated(request)) return;  
          
        if (request-\>hasParam("did", true) && request-\>hasParam("type", true) &&   
            request-\>hasParam("inst", true) && request-\>hasParam("prop", true) &&   
            request-\>hasParam("val", true)) {  
              
            uint32\_t did \= request-\>getParam("did", true)-\>value().toInt();  
            uint16\_t type \= request-\>getParam("type", true)-\>value().toInt();  
            uint32\_t inst \= request-\>getParam("inst", true)-\>value().toInt();  
            uint8\_t prop \= request-\>getParam("prop", true)-\>value().toInt();  
            float val \= request-\>getParam("val", true)-\>value().toFloat();  
              
            // MODIFICATION CHIRURGICALE : Extraction de la priorité optionnelle (0 si non fournie)  
            uint8\_t priority \= request-\>hasParam("priority", true) ? request-\>getParam("priority", true)-\>value().toInt() : 0;  
              
            bool device\_found \= false;  
            BACnetJob job;  
            memset(\&job, 0, sizeof(BACnetJob)); // Initialisation propre à 0  
              
            job.type \= JOB\_WRITE\_PROP;   
            job.obj\_type \= type;  
            job.obj\_instance \= inst;  
            job.prop\_id \= prop;  
            job.write\_value \= val;  
            job.priority \= priority; // L'API transmet la priorité au moteur BACnet  
            job.target\_mac \= 255;  
              
            // Recherche de l'adresse MAC cible  
            if (xSemaphoreTake(cache\_mutex, pdMS\_TO\_TICKS(500))) {  
                for (auto& dev : bacnet\_network\_cache) {  
                    if (dev.ulDeviceId \== did) {  
                        job.target\_mac \= dev.ucMacAddress;  
                        device\_found \= true;  
                        break;  
                    }  
                }  
                xSemaphoreGive(cache\_mutex);  
            }  
              
            // Envoi dans la file d'attente  
            if (device\_found) {  
                enqueue\_bacnet\_job(job);  
                z\_log(pdLOG\_INFO, "API", "WriteValue %.2f ENQUEUED for %u:%lu (Prop: %u, Prio: %u)\\n", val, type, (unsigned long)inst, prop, priority);  
                request-\>send(200, "text/plain", "WRITE\_VALUE ENQUEUED");  
            } else {  
                request-\>send(404, "text/plain", "Device not found");  
            }  
        } else {  
            request-\>send(400, "text/plain", "Missing params (requires: did, type, inst, prop, val)");  
        }  
    });

    // Route API principale pour la sauvegarde des paramètres système (NVS).  
