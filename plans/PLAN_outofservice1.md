# **Modifications Chirurgicales pour src/z\_bacnet.cpp**

Voici les modifications exactes à intégrer dans ton code, basées **exclusivement** sur tes propres fonctions.

### **1\. Modification dans handle\_error\_pdu (Cas d'erreur ou Rejet)**

Dans ton switch (dev.ucDiscStep) qui gère l'avancement de la FSM sur erreur/rejet, on intercale l'étape DISC\_OBJ\_STATUS\_FLAGS entre COMMANDABLE et VALUE.

**Dans ton code actuel :**

            case DISC\_OBJ\_COMMANDABLE:  
                o.xIsCommandable \= (o.usType==13||o.usType==14||o.usType==19||o.usType\<=5);  
                if(o.xEnabled || dev.xReloadSingle) dev.ucDiscStep \= DISC\_OBJ\_VALUE;  
                else { if(\!dev.xReloadSingle) dev.usDiscObjIdx++; dev.ucDiscStep \= DISC\_OBJ\_OID; }  
                break;  
            case DISC\_OBJ\_VALUE:

**La modification à appliquer :**

            case DISC\_OBJ\_COMMANDABLE:  
                o.xIsCommandable \= (o.usType==13||o.usType==14||o.usType==19||o.usType\<=5);  
                  
                // MODIFICATION : Si l'objet est activé, on passe à l'étape STATUS\_FLAGS au lieu de VALUE  
                if(o.xEnabled || dev.xReloadSingle) dev.ucDiscStep \= DISC\_OBJ\_STATUS\_FLAGS;  
                else { if(\!dev.xReloadSingle) dev.usDiscObjIdx++; dev.ucDiscStep \= DISC\_OBJ\_OID; }  
                break;

            // AJOUT : Nouvelle étape pour faire la transition de Status\_Flags vers Value sur un rejet  
            case DISC\_OBJ\_STATUS\_FLAGS:  
                dev.ucDiscStep \= DISC\_OBJ\_VALUE;  
                break;

            case DISC\_OBJ\_VALUE:

### **2\. Protection dans execute\_discovery\_logic (Bypass si défaut)**

Dans ta fonction qui construit la trame, on bloque l'envoi de la requête de lecture si la sonde est marquée en panne.

**Dans ton code actuel :**

                auto& o \= dev.objects\[dev.usDiscObjIdx\];  
                if (dev.ucDiscStep \== DISC\_OBJ\_OID) {  
                    index \= dev.usDiscObjIdx \+ 1; // Index 1-based  
                } else {  
                    type \= o.usType;  
                    inst \= o.ulInstance;  
                    if (dev.ucDiscStep \== DISC\_OBJ\_STATES) {

**La modification à appliquer :**

                auto& o \= dev.objects\[dev.usDiscObjIdx\];  
                if (dev.ucDiscStep \== DISC\_OBJ\_OID) {  
                    index \= dev.usDiscObjIdx \+ 1; // Index 1-based  
                } else {  
                    type \= o.usType;  
                    inst \= o.ulInstance;

                    // AJOUT : Bypass de sécurité. Si on doit lire la valeur mais que la sonde est en panne (Fault=1)  
                    if (dev.ucDiscStep \== DISC\_OBJ\_VALUE && o.isFault()) {  
                        o.fPresentValue \= NAN; // On invalide la valeur (Not A Number)  
                          
                        // On force l'avancement local de la FSM  
                        if(\!dev.xReloadSingle) dev.usDiscObjIdx++;   
                        dev.ucDiscStep \= DISC\_OBJ\_OID;  
                          
                        // On met prop à 0 pour by-passer build\_read\_property\_apdu  
                        // Le break permet de tomber dans le "else" et relâcher le jeton proprement  
                        prop \= 0;   
                        break;   
                    }

                    if (dev.ucDiscStep \== DISC\_OBJ\_STATES) {

### **3\. Mise à jour du StorageType et du tableau DISCOVERY\_STEPS**

**La modification à appliquer :**

enum StorageType {   
    STORE\_NONE,   
    STORE\_DEV\_ID,   
    STORE\_DEV\_NAME,   
    STORE\_DEV\_VENDOR,  
    STORE\_DEV\_UINT,   
    STORE\_OBJ\_OID,   
    STORE\_OBJ\_NAME,   
    STORE\_OBJ\_UNITS,   
    STORE\_OBJ\_REAL,   
    STORE\_OBJ\_STATES,  
    STORE\_OBJ\_STATUS\_FLAGS, // AJOUT : Type de stockage dédié aux drapeaux (Prop 111\)  
    STORE\_OBJ\_VALUE   
};

struct DiscoveryStepConfig {  
    uint16\_t prop;  
    int32\_t idx;  
    StorageType storage;  
};

static const DiscoveryStepConfig DISCOVERY\_STEPS\[\] \= {  
    // ... tes premières étapes (inchangées) ...  
    {65, \-1, STORE\_OBJ\_REAL},   // DISC\_OBJ\_MAX  
    {110, 0, STORE\_OBJ\_STATES}, // DISC\_OBJ\_STATES  
    {87, \-1, STORE\_NONE},       // DISC\_OBJ\_COMMANDABLE  
      
    // AJOUT : Correspondance exacte pour la nouvelle étape DISC\_OBJ\_STATUS\_FLAGS  
    // Propriété BACnet 111, Index BACNET\_ARRAY\_ALL (-1), routé vers STORE\_OBJ\_STATUS\_FLAGS  
    {111, \-1, STORE\_OBJ\_STATUS\_FLAGS},   
      
    {85, \-1, STORE\_OBJ\_VALUE}   // DISC\_OBJ\_VALUE  
};

### **4\. Décodage et Assignation dans handle\_complex\_ack\_discovery**

Dans ta fonction handle\_complex\_ack\_discovery, tu dois modifier la transition de STORE\_NONE et ajouter le bloc de décodage pour STORE\_OBJ\_STATUS\_FLAGS.

**Dans ton code actuel (le bloc STORE\_NONE) :**

        case STORE\_NONE:  
            if (dev.ucDiscStep \== DISC\_OBJ\_COMMANDABLE) {  
                if (dev.usDiscObjIdx \< dev.objects.size()) {  
                    auto& o \= dev.objects\[dev.usDiscObjIdx\]; o.xIsCommandable \= true;  
                    if(o.xEnabled||dev.xReloadSingle) dev.ucDiscStep \= DISC\_OBJ\_VALUE;  
                    else { if(\!dev.xReloadSingle) dev.usDiscObjIdx++; dev.ucDiscStep \= DISC\_OBJ\_OID; if(dev.xReloadSingle){dev.xDiscoveryDone=true; dev.xReloadSingle=false;} }  
                }  
                ap \= al;   
            }  
            break;

**La modification à appliquer (sur STORE\_NONE et ajout de STATUS\_FLAGS) :**

        // AJOUT : Nouveau bloc pour décoder le BitString (Tag 8\) des Status\_Flags  
        case STORE\_OBJ\_STATUS\_FLAGS: {  
            if (dev.usDiscObjIdx \< dev.objects.size()) {  
                auto& o \= dev.objects\[dev.usDiscObjIdx\];  
                  
                // En BACnet, le tag BitString (vt.number \== 8\) commence toujours par un octet de "unused\_bits" (apdu\[ap\]).  
                // Les drapeaux eux-mêmes se trouvent dans le premier octet de données : apdu\[ap \+ 1\].  
                if (vt.number \== 8 && vt.len \>= 2\) {  
                    uint8\_t flags \= 0;  
                    uint8\_t first\_byte \= apdu\[ap \+ 1\];   
                      
                    if (first\_byte & 0x80) flags |= BACNET\_STATUS\_IN\_ALARM;       // Bit 0  
                    if (first\_byte & 0x40) flags |= BACNET\_STATUS\_FAULT;          // Bit 1  
                    if (first\_byte & 0x20) flags |= BACNET\_STATUS\_OVERRIDDEN;     // Bit 2  
                    if (first\_byte & 0x10) flags |= BACNET\_STATUS\_OUT\_OF\_SERVICE; // Bit 3  
                      
                    o.ucStatusFlags \= flags;  
                }  
                  
                // Avancement de la FSM : On passe enfin à la valeur  
                dev.ucDiscStep \= DISC\_OBJ\_VALUE;   
            }  
            break;  
        }

        case STORE\_NONE:  
            if (dev.ucDiscStep \== DISC\_OBJ\_COMMANDABLE) {  
                if (dev.usDiscObjIdx \< dev.objects.size()) {  
                    auto& o \= dev.objects\[dev.usDiscObjIdx\]; o.xIsCommandable \= true;  
                      
                    // MODIFICATION : Transition vers DISC\_OBJ\_STATUS\_FLAGS au lieu de DISC\_OBJ\_VALUE  
                    if(o.xEnabled||dev.xReloadSingle) dev.ucDiscStep \= DISC\_OBJ\_STATUS\_FLAGS;  
                    else { if(\!dev.xReloadSingle) dev.usDiscObjIdx++; dev.ucDiscStep \= DISC\_OBJ\_OID; if(dev.xReloadSingle){dev.xDiscoveryDone=true; dev.xReloadSingle=false;} }  
                }  
                ap \= al;   
            }  
            break;

### **5\. Ta fonction d'écriture APDU pour Out\_Of\_Service et intégration dans les Jobs**

Ajoute la fonction build\_write\_property\_outofservice\_apdu avec les autres constructeurs d'APDU :

// AJOUT : Constructeur d'APDU pour écrire un booléen sur Out\_Of\_Service (96)  
uint16\_t build\_write\_property\_outofservice\_apdu(uint8\_t\* buffer, uint8\_t invoke\_id, uint16\_t obj\_type, uint32\_t obj\_instance, bool out\_of\_service) {  
    uint16\_t len \= 0;  
    buffer\[len++\] \= 0x01; buffer\[len++\] \= 0x04; buffer\[len++\] \= 0x02; buffer\[len++\] \= 0x03;  
    buffer\[len++\] \= invoke\_id; buffer\[len++\] \= 0x0F; buffer\[len++\] \= 0x0C;  
      
    uint32\_t oid \= ((uint32\_t)obj\_type \<\< 22\) | (obj\_instance & 0x3FFFFF);  
    buffer\[len++\] \= (oid \>\> 24\) & 0xFF; buffer\[len++\] \= (oid \>\> 16\) & 0xFF; buffer\[len++\] \= (oid \>\> 8\) & 0xFF; buffer\[len++\] \= oid & 0xFF;  
      
    buffer\[len++\] \= 0x19; buffer\[len++\] \= 96; // 96 \= PROP\_OUT\_OF\_SERVICE  
    buffer\[len++\] \= 0x3E; // Open Tag 3  
      
    // Tag BACnet Boolean (Tag Number 1). 0x11 \= True, 0x10 \= False.  
    buffer\[len++\] \= out\_of\_service ? 0x11 : 0x10;   
      
    buffer\[len++\] \= 0x3F; // Close Tag 3  
    return len;  
}

Et modifie le routage des jobs dans execute\_bacnet\_work() (cas JOB\_WRITE\_PROP) pour détecter la propriété 96 :

**Dans ton code actuel :**

            case JOB\_WRITE\_PROP:  
                if (j.prop\_id \== 77\) { // Object\_Name  
                    l \= build\_write\_property\_name\_apdu(b, next\_invoke\_id++, j.obj\_type, j.obj\_instance, j.name);  
                    z\_log(pdLOG\_INFO, "BACNET", "WRITE obj: %u:%lu (Name) \-\> %s\\n", j.obj\_type, (unsigned long)j.obj\_instance, j.name);  
                } else { // Present\_Value (85) ou autre numérique  
                    l \= build\_write\_property\_value\_apdu(b, next\_invoke\_id++, j.obj\_type, j.obj\_instance, j.prop\_id, j.write\_value);  
                    z\_log(pdLOG\_INFO, "BACNET", "WRITE obj: %u:%lu (Prop %u) \-\> %.2f\\n", j.obj\_type, (unsigned long)j.obj\_instance, j.prop\_id, j.write\_value);  
                }

**La modification à appliquer :**

            case JOB\_WRITE\_PROP:  
                if (j.prop\_id \== 77\) { // Object\_Name  
                    l \= build\_write\_property\_name\_apdu(b, next\_invoke\_id++, j.obj\_type, j.obj\_instance, j.name);  
                    z\_log(pdLOG\_INFO, "BACNET", "WRITE obj: %u:%lu (Name) \-\> %s\\n", j.obj\_type, (unsigned long)j.obj\_instance, j.name);  
                } else if (j.prop\_id \== 96\) { // AJOUT : Out\_Of\_Service  
                    bool is\_oos \= (j.write\_value \> 0.5f);  
                    l \= build\_write\_property\_outofservice\_apdu(b, next\_invoke\_id++, j.obj\_type, j.obj\_instance, is\_oos);  
                    z\_log(pdLOG\_INFO, "BACNET", "WRITE obj: %u:%lu (Out\_Of\_Service) \-\> %d\\n", j.obj\_type, (unsigned long)j.obj\_instance, is\_oos);  
                } else { // Present\_Value (85) ou autre numérique  
                    l \= build\_write\_property\_value\_apdu(b, next\_invoke\_id++, j.obj\_type, j.obj\_instance, j.prop\_id, j.write\_value);  
                    z\_log(pdLOG\_INFO, "BACNET", "WRITE obj: %u:%lu (Prop %u) \-\> %.2f\\n", j.obj\_type, (unsigned long)j.obj\_instance, j.prop\_id, j.write\_value);  
                }  
