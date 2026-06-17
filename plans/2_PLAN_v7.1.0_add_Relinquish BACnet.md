# **Modifications Chirurgicales : Ajout de la commande "Relinquish" (AUTO)**

### **1\. Ajout de la fonction APDU (Dans z\_bacnet.cpp)**

Il nous faut un nouveau constructeur d'APDU capable d'encoder la valeur NULL (Application Tag 0). Ajoute cette fonction juste en dessous de tes autres fonctions build\_write\_property\_....

**AJOUT CHIRURGICAL :**

// AJOUT CHIRURGICAL : Constructeur APDU pour relâcher une priorité (Write NULL)  
// Raison : Indispensable pour effacer une consigne manuelle (Priorité 8\) et rendre la main au programme interne de l'automate.  
uint16\_t build\_write\_property\_relinquish\_apdu(uint8\_t\* buffer, uint8\_t invoke\_id, uint16\_t obj\_type, uint32\_t obj\_instance, uint8\_t prop\_id, uint8\_t priority) {  
    uint16\_t len \= 0;  
    buffer\[len++\] \= 0x01; buffer\[len++\] \= 0x04; buffer\[len++\] \= 0x02; buffer\[len++\] \= 0x03;  
    buffer\[len++\] \= invoke\_id; buffer\[len++\] \= 0x0F; buffer\[len++\] \= 0x0C;  
      
    uint32\_t oid \= ((uint32\_t)obj\_type \<\< 22\) | (obj\_instance & 0x3FFFFF);  
    buffer\[len++\] \= (oid \>\> 24\) & 0xFF; buffer\[len++\] \= (oid \>\> 16\) & 0xFF; buffer\[len++\] \= (oid \>\> 8\) & 0xFF; buffer\[len++\] \= oid & 0xFF;  
      
    buffer\[len++\] \= 0x19; buffer\[len++\] \= prop\_id;   
    buffer\[len++\] \= 0x3E; // Open Tag 3  
      
    // Application Tag 0 (NULL). C'est ce qui indique à l'automate d'effacer la valeur du tableau de priorité.  
    buffer\[len++\] \= 0x00;   
      
    buffer\[len++\] \= 0x3F; // Close Tag 3  
      
    if (priority \> 0 && priority \<= 16\) {  
        buffer\[len++\] \= 0x49; // Tag 4 (Priority)  
        buffer\[len++\] \= priority;  
    }  
      
    return len;  
}

*(Pense à rajouter le prototype uint16\_t build\_write\_property\_relinquish\_apdu(...); dans z\_bacnet.h)*

### **2\. Routage dans le Worker BACnet (Dans z\_bacnet.cpp)**

Dans ta fonction execute\_bacnet\_work(), au niveau du case JOB\_WRITE\_PROP:, on va intercepter la valeur NAN pour appeler notre nouvelle fonction.

**Rechercher dans ton code actuel :**

                } else if (j.prop\_id \== 81\) { // Out\_Of\_Service  
                    bool is\_oos \= (j.write\_value \> 0.5f);  
                    l \= build\_write\_property\_outofservice\_apdu(b, next\_invoke\_id++, j.obj\_type, j.obj\_instance, is\_oos);  
                    z\_log(pdLOG\_INFO, "BACNET", "WRITE obj: %u:%lu (Out\_Of\_Service) \-\> %d\\n", j.obj\_type, (unsigned long)j.obj\_instance, is\_oos);  
                } else { // Present\_Value (85) ou autre numérique  
                    l \= build\_write\_property\_value\_apdu(b, next\_invoke\_id++, j.obj\_type, j.obj\_instance, j.prop\_id, j.write\_value, j.priority);  
                    z\_log(pdLOG\_INFO, "BACNET", "WRITE obj: %u:%lu (Prop %u) \-\> %.2f (Prio: %u)\\n", j.obj\_type, (unsigned long)j.obj\_instance, j.prop\_id, j.write\_value, j.priority);  
                }

**Remplacer par :**

                } else if (j.prop\_id \== 81\) { // Out\_Of\_Service (Prop 81\)  
                    bool is\_oos \= (j.write\_value \> 0.5f);  
                    l \= build\_write\_property\_outofservice\_apdu(b, next\_invoke\_id++, j.obj\_type, j.obj\_instance, is\_oos);  
                    z\_log(pdLOG\_INFO, "BACNET", "WRITE obj: %u:%lu (Out\_Of\_Service) \-\> %d\\n", j.obj\_type, (unsigned long)j.obj\_instance, is\_oos);  
                } else if (isnan(j.write\_value)) { // AJOUT CHIRURGICAL : Si la valeur est NAN, c'est un Relinquish (AUTO)  
                    l \= build\_write\_property\_relinquish\_apdu(b, next\_invoke\_id++, j.obj\_type, j.obj\_instance, j.prop\_id, j.priority);  
                    z\_log(pdLOG\_INFO, "BACNET", "WRITE obj: %u:%lu (Prop %u) \-\> RELINQUISH/AUTO (Prio: %u)\\n", j.obj\_type, (unsigned long)j.obj\_instance, j.prop\_id, j.priority);  
                } else { // Present\_Value (85) ou autre numérique  
                    l \= build\_write\_property\_value\_apdu(b, next\_invoke\_id++, j.obj\_type, j.obj\_instance, j.prop\_id, j.write\_value, j.priority);  
                    z\_log(pdLOG\_INFO, "BACNET", "WRITE obj: %u:%lu (Prop %u) \-\> %.2f (Prio: %u)\\n", j.obj\_type, (unsigned long)j.obj\_instance, j.prop\_id, j.write\_value, j.priority);  
                }

### **3\. Prise en charge API REST (Dans z\_network.cpp)**

Dans ta route /api/writevalue, permet d'accepter "AUTO" comme valeur valide.

**Rechercher :**

            uint8\_t prop \= request-\>getParam("prop", true)-\>value().toInt();  
            float val \= request-\>getParam("val", true)-\>value().toFloat();

**Remplacer par :**

            uint8\_t prop \= request-\>getParam("prop", true)-\>value().toInt();  
              
            // MODIFICATION CHIRURGICALE : Autoriser le mot clé "AUTO" pour générer un NAN  
            String val\_str \= request-\>getParam("val", true)-\>value();  
            float val \= val\_str.equalsIgnoreCase("AUTO") ? NAN : val\_str.toFloat();

### **4\. Prise en charge MQTT (Dans z\_mqtt.cpp)**

Dans ton parseur mqtt\_event\_handler, permet de traiter la commande AUTO.

**Rechercher :**

                                        } else if (job.obj\_type \== 14 || job.obj\_type \== 19\) {  
                                            // Conversion Texte \-\> Index pour Multi-State

**Remplacer par (Juste au dessus du bloc MSI/MSV) :**

                                        } else if (String(payload\_buf).equalsIgnoreCase("AUTO")) {  
                                            // AJOUT CHIRURGICAL : Prise en charge du mot clé AUTO depuis MQTT  
                                            job.write\_value \= NAN;  
                                        } else if (job.obj\_type \== 14 || job.obj\_type \== 19\) {  
                                            // Conversion Texte \-\> Index pour Multi-State

