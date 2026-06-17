Il faut s’appuyer sur la propriété `Status_Flags` (ID 111\) pour généraliser la surveillance est la méthode la plus puissante et la plus économe en bande passante. La norme ASHRAE 135 stipule d'ailleurs que cette propriété regroupe les 4 indicateurs vitaux d'un objet : `{IN_ALARM, FAULT, OVERRIDDEN, OUT_OF_SERVICE}`.  
En regroupant ces 4 drapeaux dans ton unique payload de statut (ton JSON en RAM), tu vas permettre à Home Assistant de mettre à jour **5 entités simultanément (1 capteur \+ 4 capteurs binaires) avec un seul message MQTT sur le réseau** \! C'est une optimisation redoutable pour soulager la pile TCP de ton ESP32-S3.  
Voici comment implémenter cette généralisation de bout en bout :  
**1\. Le Piège Bas Niveau : L'Endianness du** `BIT STRING` **BACnet**  
Avant de modifier le JSON, il y a un détail critique sur le bit-shifting. En ASN.1 BACnet (Application Tag 8), les chaînes de bits sont encodées de gauche à droite. Le tout premier bit (`IN_ALARM`) est placé sur le bit de poids fort (MSB) du premier octet de données. Si ton parser C++ lit l'octet brut, les masques hexadécimaux sont :

* `IN_ALARM` : `0x80` (Bit 7\)  
* `FAULT` : `0x40` (Bit 6\)  
* `OVERRIDDEN` : `0x20` (Bit 5\)  
* `OUT_OF_SERVICE` : `0x10` (Bit 4\)

*(Note : Si ton parseur inverse déjà la structure pour la stocker dans un uint8\_t normalisé, les masques seront 0x01, 0x02, 0x04 et 0x08. Assure-toi de vérifier ce point dans ta mémoire RAM).*  
**2\. Mise à jour de l'API de publication d'État (Le Payload MQTT)**  
Tu utilises ArduinoJson 7 alloué en PSRAM. Dans ta fonction de publication d'état (`publish_mqtt_topic`), tu vas simplement ajouter ces 4 booléens au document JSON avant de l'expédier.  
// Création du document JSON en PSRAM  
JsonDocument doc;

// Tes données existantes  
doc\["val"\] \= obj.present\_value;

// \--- AJOUT DU RADAR GLOBAL STATUS\_FLAGS \---  
// Remplacement direct par des booléens natifs pour optimiser le JSON  
doc\["alarm"\]      \= (obj.status\_flags & 0x80) \!= 0;   
doc\["fault"\]      \= (obj.status\_flags & 0x40) \!= 0;  
doc\["overridden"\] \= (obj.status\_flags & 0x20) \!= 0;  
doc\["oos"\]        \= (obj.status\_flags & 0x10) \!= 0;

char buffer\[4\];  
serializeJson(doc, buffer);

// Envoi asynchrone sur "tele/B2M/364004/AI\_1/state"  
enqueue\_mqtt\_publish(topic, buffer, retain);

**3\. La Découverte MQTT (Auto-Discovery) pour Home Assistant**  
Pour que Home Assistant crée automatiquement ces 4 indicateurs, il faut publier 4 nouvelles configurations de type `binary_sensor`.  
Pour ne pas faire exploser ton *Heap* ni générer un *MQTT Storm*, on va utiliser les abréviations MQTT natives (`~`, `stat_t`, `val_tpl`) et lier ces 4 capteurs au même appareil (Device) via son identifiant. Surtout, **ils s'abonneront tous au même** state\_topic **que la valeur principale**.  
Voici le code C++ pour générer cette découverte dynamique :  
void publish\_status\_flags\_discovery(BACnetDevice& dev, BACnetObject& obj) {  
    char topic\[9\];  
    char base\_topic\[10\];  
    snprintf(base\_topic, sizeof(base\_topic), "%s/%lu/%s\_%lu",   
             sysCfg.mqtt\_prefix, dev.device\_id, get\_obj\_type\_str(obj.type), obj.instance);

    // Les 4 flags à créer  
    const char\* flags\[\] \= {"alarm", "fault", "overridden", "oos"};  
    const char\* names\[\] \= {"Alarme", "Défaut", "Forçage Manuel", "Hors Service"};  
    const char\* classes\[\] \= {"problem", "problem", "update", "connectivity"};   
    // device\_class "problem" affiche visuellement le capteur en rouge en cas de souci

    for (int i \= 0; i \< 4; i++) {  
        JsonDocument doc;  
          
        // Topic de découverte : homeassistant/binary\_sensor/364004\_AI\_1\_fault/config  
        snprintf(topic, sizeof(topic), "homeassistant/binary\_sensor/%lu\_%s\_%lu\_%s/config",   
                 dev.device\_id, get\_obj\_type\_str(obj.type), obj.instance, flags\[i\]);

        // Configuration avec abréviations (Optimisation RAM)  
        doc\["\~"\] \= base\_topic;  
          
        // Le nom affiché sera "Sonde Extérieure Défaut" (HA 2021+ combine Device et Name)  
        doc\["name"\] \= names\[i\];   
          
        // Un ID unique absolu  
        char uniq\_id\[10\];  
        snprintf(uniq\_id, sizeof(uniq\_id), "%lu\_%s\_%lu\_%s", dev.device\_id, get\_obj\_type\_str(obj.type), obj.instance, flags\[i\]);  
        doc\["uniq\_id"\] \= uniq\_id;

        // Le topic d'état commun à toutes les entités \!  
        doc\["stat\_t"\] \= "\~/state"; 

        // Le filtre Jinja2 pour extraire spécifiquement ce booléen du JSON  
        // Convertit le True/False du JSON en ON/OFF exigé par le binary\_sensor HA  
        char val\_tpl\[10\];  
        snprintf(val\_tpl, sizeof(val\_tpl), "{{ 'ON' if value\_json.%s else 'OFF' }}", flags\[i\]);  
        doc\["val\_tpl"\] \= val\_tpl;

        // L'icône dynamique ou la classe  
        if (strlen(classes\[i\]) \> 0\) {  
            doc\["dev\_cla"\] \= classes\[i\];  
        }

        // Lier au Device parent (ex: l'automate ECB-203 ou l'objet lui-même)  
        JsonObject device \= doc\["dev"\].to\<JsonObject\>();  
        char dev\_id\_str\[11\];  
        snprintf(dev\_id\_str, sizeof(dev\_id\_str), "%lu\_%s\_%lu", dev.device\_id, get\_obj\_type\_str(obj.type), obj.instance);  
        device\["ids"\] \= dev\_id\_str;

        char buffer\[12\];  
        serializeJson(doc, buffer);  
          
        // Envoi dans la queue MQTT (le gatekeeper de 5ms encaissera la charge)  
        enqueue\_mqtt\_publish(topic, buffer, true); // retain \= true obligatoire pour le config  
    }  
}

**Le Résultat dans Home Assistant**  
Dès que ton ESP32-S3 va publier cet Auto-Discovery, Home Assistant va regrouper sous chaque point BACnet (ex : *Vanne Mélangeuse* ou *ConsigneFinale*) **la valeur actuelle** (la jauge ou le slider) **ET les 4 capteurs de diagnostic**.

* Si `value_json.fault` passe à `true` (parce que l'automate distant a détecté un capteur coupé, flag `FAULT` \= 1), l'entité "Défaut" passera en **Rouge (Problème)** dans ton Dashboard.  
* Si tu as écrit avec une priorité 8, `value_json.overridden` passera à `true` (flag `OVERRIDDEN` \= 1), et l'icône "Forçage Manuel" s'activera.

L'immense avantage de cette architecture, c'est qu'elle est "Passive". Ton code C++ sur le *Core 1* de l'ESP32 n'a besoin d'envoyer qu'un seul service BACnet `ReadProperty` pour le `Status_Flags` sur l'UART RS485. Le broker MQTT et les moteurs de *Template* Jinja2 de Home Assistant se chargent de tout le travail de séparation en aval \! Ton bus MS/TP reste parfaitement fluide.

