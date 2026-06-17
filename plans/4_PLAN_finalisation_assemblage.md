# **Modifications Chirurgicales BACnet (Gestion Priorité)**

### **1\. Modification du constructeur build\_write\_property\_value\_apdu**

Dans ton code, mets à jour la signature de la fonction pour accepter uint8\_t priority et ajoute l'encodage du Tag 4 à la fin.

**Remplacer l'ancienne fonction par :**

uint16\_t build\_write\_property\_value\_apdu(uint8\_t\* buffer, uint8\_t invoke\_id, uint16\_t obj\_type, uint32\_t obj\_instance, uint8\_t prop\_id, float value, uint8\_t priority) {  
    uint16\_t len \= 0;  
    buffer\[len++\] \= 0x01; buffer\[len++\] \= 0x04; buffer\[len++\] \= 0x02; buffer\[len++\] \= 0x03;  
    buffer\[len++\] \= invoke\_id; buffer\[len++\] \= 0x0F; buffer\[len++\] \= 0x0C;  
    uint32\_t oid \= ((uint32\_t)obj\_type \<\< 22\) | (obj\_instance & 0x3FFFFF);  
    buffer\[len++\] \= (oid \>\> 24\) & 0xFF; buffer\[len++\] \= (oid \>\> 16\) & 0xFF; buffer\[len++\] \= (oid \>\> 8\) & 0xFF; buffer\[len++\] \= oid & 0xFF;  
    buffer\[len++\] \= 0x19; buffer\[len++\] \= prop\_id; buffer\[len++\] \= 0x3E;  
      
    if (obj\_type \== 13 || obj\_type \== 14 || obj\_type \== 19\) {  
        uint32\_t v \= (uint32\_t)value;  
        if (v \<= 255\) { buffer\[len++\] \= 0x21; buffer\[len++\] \= (uint8\_t)v; }  
        else if (v \<= 65535\) { buffer\[len++\] \= 0x22; buffer\[len++\] \= (v \>\> 8\) & 0xFF; buffer\[len++\] \= v & 0xFF; }  
        else { buffer\[len++\] \= 0x24; buffer\[len++\] \= (v \>\> 24\) & 0xFF; buffer\[len++\] \= (v \>\> 16\) & 0xFF; buffer\[len++\] \= (v \>\> 8\) & 0xFF; buffer\[len++\] \= v & 0xFF; }  
    } else if (obj\_type \== 3 || obj\_type \== 4 || obj\_type \== 5\) {  
        buffer\[len++\] \= 0x91; buffer\[len++\] \= (value \> 0.5f) ? 1 : 0;  
    } else {  
        buffer\[len++\] \= 0x44; uint32\_t tmp; float fv \= value; memcpy(\&tmp, \&fv, 4); tmp \= \_\_builtin\_bswap32(tmp);  
        memcpy(\&buffer\[len\], \&tmp, 4); len \+= 4;  
    }  
      
    buffer\[len++\] \= 0x3F; // Close Tag 3  
      
    // AJOUT CHIRURGICAL : Si une priorité est définie (de 1 à 16), on ajoute le Tag contextuel 4\.  
    if (priority \> 0 && priority \<= 16\) {  
        buffer\[len++\] \= 0x49; // Tag 4 (Priority), Length 1 byte  
        buffer\[len++\] \= priority;  
    }  
      
    return len;  
}

### **2\. Modification de l'appel dans execute\_bacnet\_work()**

Dans ta tâche principale qui dépile les jobs, mets à jour l'appel pour passer la priorité à ta nouvelle fonction.

**Rechercher :**

                } else { // Present\_Value (85) ou autre numérique  
                    l \= build\_write\_property\_value\_apdu(b, next\_invoke\_id++, j.obj\_type, j.obj\_instance, j.prop\_id, j.write\_value);  
                    z\_log(pdLOG\_INFO, "BACNET", "WRITE obj: %u:%lu (Prop %u) \-\> %.2f\\n", j.obj\_type, (unsigned long)j.obj\_instance, j.prop\_id, j.write\_value);  
                }

**Remplacer par :**

                } else { // Present\_Value (85) ou autre numérique  
                    // AJOUT CHIRURGICAL : Transfert de j.priority dans la chaîne de construction  
                    l \= build\_write\_property\_value\_apdu(b, next\_invoke\_id++, j.obj\_type, j.obj\_instance, j.prop\_id, j.write\_value, j.priority);  
                    z\_log(pdLOG\_INFO, "BACNET", "WRITE obj: %u:%lu (Prop %u) \-\> %.2f (Prio: %u)\\n", j.obj\_type, (unsigned long)j.obj\_instance, j.prop\_id, j.write\_value, j.priority);  
                }  
