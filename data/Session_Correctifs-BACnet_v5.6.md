### Résumé Technique de Session : Correctifs BACnet et État du Projet v5.6

#### 1\. Métadonnées de la Session et Contexte de Reprise

Ce document consigne les évolutions critiques de la pile logicielle BACnet MS/TP développée pour l'ESP32-S3. Il sert de base de référence pour la transition vers la phase de stabilisation v5.6.| Paramètre | Valeur || \------ | \------ || **ID de Session** | 2e337db4-9ed2-47bf-9599-cd95cf5aa47e || **Horodatage Initial** | 2026-05-23 01:34:20 UTC || **Dernière Synchronisation** | 2026-05-26 19:36:34 UTC || **Cible Matérielle** | Waveshare ESP32-S3-RS485-CAN || **Statut du Projet** | v5.6 \- Optimisation NVS & Discovery |

#### 2\. Diagnostic et Correction du Bug de Timeout

L'utilisateur a rapporté que la modification du champ "timeout" dans l'UI n'affectait pas les performances réelles du bus. L'audit a révélé une rupture dans la chaîne de propagation des données.

* **Défaut de Binding UI :**  Dans  **z\_ui.h** , la fonction loadConfig() ne synchronisait pas l'élément DOM in-timeout avec la réponse de l'API /api/status. Le champ restait graphiquement figé à sa valeur par défaut.  
* **Logique FSM :**  Bien que sysCfg.apdu\_timeout soit correctement mis à jour en RAM via /save, l'automate MS/TP dans  **z\_bacnet.cpp**  n'utilisait pas systématiquement cette variable pour cadencer l'état MSTP\_AWAIT\_REPLY.  
* **Remédiation :**  
* Correction de la liaison bidirectionnelle (Binding) dans le formulaire BACnet.  
* Utilisation forcée de sysCfg.apdu\_timeout dans la FSM pour les requêtes  *Data-Expecting-Reply* .  
* Application d'une fenêtre de garde de  **280ms**  (basée sur les tests physiques) pour accommoder la latence du contrôleur Distech ECB\_203.

#### 3\. Optimisation de la Persistance NVS et Masquage OID

Afin de résoudre les problèmes de latence et de prévenir le "Deadlock NVS" observé lors des phases de scan intensif, le système de stockage des Object Identifiers (OID) a été refondu.**Mécanisme de compaction binaire :**  Le système utilise désormais un schéma de masquage sur 32 bits pour chaque OID :

* **Type d'objet :**  10 bits de poids fort ((uint32\_t)obj\_type \<\< 22).  
* **Instance :**  22 bits de poids faible (Masque 0x3FFFFF).**Justification Technique :**  Ce format compact permet d'utiliser la bibliothèque  **Preferences.h**  avec la méthode putBytes (stockage en mode Blob binaire) au lieu d'appels répétitifs à putUInt. Ce passage au stockage groupé génère un  **gain de performance de l'ordre de 100x** , garantissant que l'écriture en NVS ne bloque pas la FSM temps réel s'exécutant sur le Core 1\.

#### 4\. Refonte du Parsing ComplexACK (Isolation OID)

La réception de trames complexes lors du "Turbo Discovery" présentait des risques de collision de données. La logique de décodage dans  **z\_bacnet.cpp**  a été sécurisée.

* **Verrouillage du Tag 3 :**  Un mécanisme de verrou ( **lock** ) a été implémenté spécifiquement sur le  **Tag 3 (inside\_property\_value)**  lors de l'analyse ASN.1.  
* **Objectif :**  Isoler le segment contenant la propriété Object\_List. Cette isolation est critique pour extraire proprement les listes d'objets volumineuses sans risque de débordement de tampon ou d'erreur d'interprétation lors des scans parallèles.  
* **Compatibilité Legacy :**  Intégration du patch APDU 01 04 02 73 forçant les paramètres SA=1 et MaxSegs=7 pour garantir la réponse des automates Distech anciens.

#### 5\. Nouvelles Fonctions de Forge Protocolaire (v5.6)

Pour renforcer l'autonomie de la passerelle vis-à-vis des bibliothèques externes, trois fonctions de forgeage de trames natives ont été ajoutées :| Fonction | Type de Trame | Rôle Technique || \------ | \------ | \------ || **Who-Is** | Broadcast | Implémentation du NPDU global pour la découverte de dispositifs sur l'ensemble du segment MS/TP. || **I-Am** | Unicast / Broadcast | Annonce de la gateway et réponse aux requêtes Who-Is des outils tiers (type YABE). || **WriteProperty** | Unicast | Forgeage des trames APDU de commande pour le pilotage des points (Setpoints). |

#### 6\. Audit de l'Automate Distech ECB\_203 vs EDE

Le scan physique du bus a permis de caractériser précisément le comportement du contrôleur Distech ECB\_203, révélant des écarts majeurs avec le fichier EDE théorique :

1. **Incohérence de Schéma :**  De nombreux objets présents sur le bus ne sont pas répertoriés dans l'EDE, validant la nécessité du mode "Turbo Discovery".  
2. **Analyse de Latence :**  Le temps de réponse moyen du contrôleur est de  **240ms** .  
3. **Ajustement FSM :**  Ce délai impose une gestion stricte du  $T\_{reply\\\_delay}$ . La FSM a été configurée pour rester en écoute active pendant 280ms avant de déclarer un timeout, évitant ainsi le cycle de re-transmission inutile qui surchargeait le bus.

#### 7\. Plan de Remédiation v5.6 et Prochaines Étapes

Les actions prioritaires pour la suite de la Phase 2 (Étape 2\) sont définies comme suit :

*   **Fix IP Gateway**  : Dans  **z\_network.cpp** , forcer sysCfg.gateway à 192.168.1.254 pour stopper l'effet domino des reboots MQTT liés à la Freebox.  
*   **Circuit Breaker MQTT**  : Stabiliser la logique de déconnexion après 3 échecs pour préserver les ressources CPU.  
*   **Décodage Multi-State**  : Finaliser le parsing des textes d'état pour les types d'objets  **MSI** ,  **MSO**  et  **MSV** .  
*   **CSV Dynamique**  : Déploiement du téléchargement direct des objets découverts au format CSV depuis l'interface web.  
*   **Procédure de Flash**  : Rappel strict de l'exécution de ./utils/compil.sh avant tout lancement de flashOTA.sh.

**Notification obligatoire :**  La clôture de cette session doit être confirmée via Alfr3D\_Notifier pour valider l'intégrité des correctifs v5.6.  
