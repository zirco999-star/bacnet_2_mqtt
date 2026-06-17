# **Modifications Chirurgicales pour src/z\_nvs.cpp**

Voici les **2 modifications ciblées** à intégrer dans ton code d'origine pour assurer la sauvegarde et la restauration de la propriété ucStatusFlags (qui contient l'état Out\_Of\_Service) en mémoire flash (NVS).

### **1\. Restauration depuis la NVS au démarrage**

Dans la fonction load\_device\_objects, au moment où tu copies les données de la structure page.objects\[i\] vers ton objet en RAM obj.

**Rechercher dans ton code (vers la ligne 53\) :**

                                obj.fMinValue \= page.objects\[i\].fMinValue;  
                                obj.fMaxValue \= page.objects\[i\].fMaxValue;  
                                obj.fStepValue \= page.objects\[i\].fStepValue;  
                                strlcpy(obj.cMinRef, page.objects\[i\].cMinRef, sizeof(obj.cMinRef));  
                                strlcpy(obj.cMaxRef, page.objects\[i\].cMaxRef, sizeof(obj.cMaxRef));

**Remplacer par :**

                                obj.fMinValue \= page.objects\[i\].fMinValue;  
                                obj.fMaxValue \= page.objects\[i\].fMaxValue;  
                                obj.fStepValue \= page.objects\[i\].fStepValue;  
                                  
                                // AJOUT CHIRURGICAL : Restauration des Status\_Flags depuis la flash  
                                // Raison : Permet de retrouver l'état OutOfService (hack actif ou non) et les alarmes même après un redémarrage de la passerelle.  
                                obj.ucStatusFlags \= page.objects\[i\].ucStatusFlags;  
                                  
                                strlcpy(obj.cMinRef, page.objects\[i\].cMinRef, sizeof(obj.cMinRef));  
                                strlcpy(obj.cMaxRef, page.objects\[i\].cMaxRef, sizeof(obj.cMaxRef));

### **2\. Sauvegarde vers la NVS (Lazy Save)**

Dans la fonction save\_device\_objects\_locked, au moment où tu copies les données de ton objet en RAM o vers la structure persistante page.objects\[i\].

**Rechercher dans ton code (vers la ligne 203\) :**

                    page.objects\[i\].fMinValue \= o.fMinValue;  
                    page.objects\[i\].fMaxValue \= o.fMaxValue;  
                    page.objects\[i\].fStepValue \= o.fStepValue;  
                    strlcpy(page.objects\[i\].cMinRef, o.cMinRef, sizeof(page.objects\[i\].cMinRef));  
                    strlcpy(page.objects\[i\].cMaxRef, o.cMaxRef, sizeof(page.objects\[i\].cMaxRef));

**Remplacer par :**

                    page.objects\[i\].fMinValue \= o.fMinValue;  
                    page.objects\[i\].fMaxValue \= o.fMaxValue;  
                    page.objects\[i\].fStepValue \= o.fStepValue;  
                      
                    // AJOUT CHIRURGICAL : Sauvegarde des Status\_Flags en mémoire non volatile  
                    // Raison : Persister l'état du hack pour éviter de perdre l'isolation d'une sonde en cas de reboot ou de plantage.  
                    page.objects\[i\].ucStatusFlags \= o.ucStatusFlags;  
                      
                    strlcpy(page.objects\[i\].cMinRef, o.cMinRef, sizeof(page.objects\[i\].cMinRef));  
                    strlcpy(page.objects\[i\].cMaxRef, o.cMaxRef, sizeof(page.objects\[i\].cMaxRef));  
