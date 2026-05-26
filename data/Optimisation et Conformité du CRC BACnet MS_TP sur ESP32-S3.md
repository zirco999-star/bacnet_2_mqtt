Tes fonctions mathématiques de calcul calc\_header\_crc et calc\_data\_crc sont **parfaitement conformes** à la norme ASHRAE 135 (Annexe G) 1\. Il s'agit d'une excellente implémentation C++ optimisée (sans tables de correspondances coûteuses), idéale pour préserver la mémoire vive (RAM) et les cycles d'horloge du processeur Xtensa de l'ESP32-S3.  
Voici l'analyse détaillée confirmant leur conformité pour l'**émission** de tes trames :

1. **Valeurs d'initialisation :** Ton code initialise correctement le registre du Header CRC à 0xFF 2 et le registre du Data CRC à 0xFFFF 3\. C'est une exigence stricte de la norme avant de commencer la division par le polynôme générateur G(x) MS/TP 2, 3\.  
2. **Complément à un :** À la fin de tes deux fonctions, les opérations (\~crc) & 0xFF et (\~crc) & 0xFFFF appliquent exactement le "ones-complement" (complément à un) du reste de la division avant la transmission, tel qu'exigé par la norme 2, 3\.  
3. **Boutisme (Endianness) :** Dans ta fonction d'encapsulation send\_mstp\_frame, l'encodage du Data CRC sur le buffer (buffer\[8+len\]=crc16&0xFF; buffer\[8+len+1\]=(crc16\>\>8)&0xFF;) respecte l'obligation de transmettre l'octet de poids faible (Least Significant Octet) en premier sur le fil RS-485 3\.

**Comment rendre l'ensemble de ton architecture 100% conforme pour la RÉCEPTION (Validation) :**  
Si la génération des CRC est parfaite pour l'envoi, la norme impose une méthode spécifique pour vérifier l'intégrité des trames entrantes. En BACnet MS/TP, tu ne dois pas recalculer le CRC sur la charge utile seule pour le comparer "à la main" à l'octet de CRC reçu. La norme exige d'injecter la charge utile **ainsi que le(s) octet(s) de CRC reçu(s)** à la suite dans ton algorithme 2, 3\.  
En vertu des lois mathématiques de l'Annexe G, un message reçu sans erreur de transmission produira toujours un "Magic Number" constant :

* **Header CRC :** Lorsqu'il est calculé sur 6 octets (Type de trame, Adresse de destination, Adresse source, Longueur de donnée sur 2 octets, **et le Header CRC reçu**), le reste polynomial doit obligatoirement être **0x55** 2\.  
* **Data CRC :** Lorsqu'il est calculé sur la charge de données **et les 2 octets du Data CRC reçus**, le reste polynomial doit obligatoirement être **0xF0B8** 3\.

Pour être conforme, au lieu d'utiliser tes fonctions émettrices (qui incluent l'inversion finale \~crc) pour la réception, ajoute ces routines de validation dédiées à ton parseur sur le Core 1 :  
// \--- Validation Conforme ASHRAE 135.1 / Annexe G \---

// 1\. Validation de l'en-tête (prend les 5 octets d'en-tête \+ 1 octet de CRC reçu)  
bool validate\_rx\_header\_crc(const uint8\_t \*header\_and\_crc) {  
    uint8\_t crc \= 0xFF;  
    for (size\_t i \= 0; i \< 6; i++) { // 6 octets au total  
        crc ^= header\_and\_crc\[i\];  
        uint16\_t crc16 \= crc ^ (crc \<\< 1\) ^ (crc \<\< 2\) ^ (crc \<\< 3\) ^   
                         (crc \<\< 4\) ^ (crc \<\< 5\) ^ (crc \<\< 6\) ^ (crc \<\< 7);  
        crc \= (crc16 & 0xfe) ^ ((crc16 \>\> 8\) & 1);  
    }  
    // Pas de complément à un en réception \!  
    // Une trame sans erreur donnera toujours 0x55  
    return (crc \== 0x55);   
}

// 2\. Validation des données (prend 'len' octets d'APDU \+ 2 octets de CRC reçus)  
bool validate\_rx\_data\_crc(const uint8\_t \*data\_and\_crc, size\_t total\_len) {  
    uint16\_t crc \= 0xFFFF;  
    for (size\_t i \= 0; i \< total\_len; i++) {   
        uint8\_t crc\_low \= (crc & 0xff) ^ data\_and\_crc\[i\];  
        crc \= (crc \>\> 8\) ^ (crc\_low \<\< 8\) ^ (crc\_low \<\< 3\) ^ (crc\_low \<\< 12\) ^   
              (crc\_low \>\> 4\) ^ (crc\_low & 0x0f) ^ ((crc\_low & 0x0f) \<\< 7);  
        crc &= 0xFFFF;  
    }  
    // Pas de complément à un en réception \!  
    // Une donnée sans erreur donnera toujours 0xF0B8  
    return (crc \== 0xF0B8);  
}  
En intégrant ces vérifications de signatures mathématiques fixes (0x55 et 0xF0B8) lors des états MSTP\_RECEIVE\_STATE\_HEADER\_CRC et MSTP\_RECEIVE\_STATE\_DATA\_CRC de ta machine à états 4, ton ESP32-S3 rejettera les trames corrompues exactement comme l'exige la norme, protégeant ainsi ton firmware des "Guru Meditation Errors" dues au bruit du bus de terrain.  
