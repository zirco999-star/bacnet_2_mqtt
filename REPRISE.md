# 🚀 PROMPT DE REPRISE : Projet BACnet2MQTT (v7.1.14)

**État du Projet :** Classification de commandabilité ASHRAE 135 finalisée. Restauration de la modificabilité des consignes secondaires et des configurations sur le dashboard Home Assistant validée ✅. Icônes de ventilation et volets configurées en `mdi:fan` ✅.

**Fichiers Clés :**
- `GEMINI.md` : Directives prioritaires et spécificités techniques (ESP-IDF v5).
- `TRACE.md` : Journal de suivi exhaustif de toutes les versions de référence et des déploiements.
- `MEMORY.md` : Contexte réseau et checkpoints matériels.

## 🛠 Actions de Début de Session (OBLIGATOIRE)
1. **Initialisation** : Lis le fichier `GEMINI.md` local pour charger les pins, les cores et les règles d'autonomie.
2. **Consultation de l'Historique** : Examine la dernière entrée de `TRACE.md` pour comprendre les améliorations apportées en `v7.1.14` (Icônes personnalisées mdi:fan).
3. **Validation** : Lance une compilation de test via `./utils/compil.sh` pour confirmer la propreté de l'environnement de développement.

## 📍 Points Clés à retenir (Session v7.1.14)
1. **Classification des Objets (ASHRAE 135)** :
   - **Catégorie 1** : Objets commandables (AO, BO, MSO, AV/BV/MSV de régulation active). Ils possèdent une `Priority_Array` (Prop 87) et nécessitent le Context Tag 4 (priorité 8) pour être écrits. Ils disposent d'un bouton de reset Home Assistant (`button.ecb_203_<name>_reset`) envoyant `AUTO` (Relinquish).
   - **Catégorie 2** : Objets écrivables mais non-commandables (consignes finales, offsets, limites, changeover). Ils ne possèdent pas de `Priority_Array`. Les écritures doivent être émises **sans tag de priorité** (priorité 0, pas d'octet `0x49` dans l'APDU) pour éviter les rejets de l'automate. Leurs boutons de reset sont automatiquement masqués.
   - **Catégorie 3** : Objets en lecture seule (AI, BI, MSI).
2. **Dashboard Lovelace & Icônes** :
   - Localisation : `/home/dev/homeassistant/.storage/lovelace.ecb_203`.
   - Les objets avec "ventil" ou "volet" dans leur nom reçoivent automatiquement l'icône `"mdi:fan"` de l'auto-découverte pour correspondre au design du dashboard.
3. **Autogestion du Cache et des Fantômes** :
   - `src/z_mqtt.cpp` publie dynamiquement un payload vide sur les topics autodiscovery HA obsolètes lors d'une découverte si le composant a changé pour éviter les entités fantômes.

---
*Fin du Prompt de Reprise.*
