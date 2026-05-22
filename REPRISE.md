# 🚀 PROMPT DE REPRISE : Projet BACnet2MQTT (v4.5.33)

**État du Projet :** Réorganisation terminée, structure propre déployée. Compilation validée ✅.
**Fichiers Clés :**
- `GEMINI.md` : Directives prioritaires et spécificités techniques (ESP-IDF v5).
- `MEMORY.md` : Dernier point de passage et contexte réseau utilisateur.

## 🛠 Actions de Début de Session (OBLIGATOIRE)
1. **Initialisation** : Lis le fichier `GEMINI.md` local pour charger les pins, les cores et les règles d'autonomie.
2. **Contextualisation** : Lis `MEMORY.md` pour récupérer les paramètres réseau (Gateway, DNS) et le dernier checkpoint.
3. **Validation** : Lance une compilation de test via `./utils/compil.sh` pour confirmer que l'environnement est prêt.

## 📍 Tâches Prioritaires (Next Steps)
1. **Fix Gateway IP** : Dans `src/z_network.cpp`, s'assurer que `sysCfg.gateway` est bien `192.168.1.254` (Freebox) pour arrêter les reboots MQTT (Domino Effect).
2. **Fix Deadlock FSM** : Dans `src/z_bacnet.cpp`, remplacer les écoutes `UART_TX_DONE` par le polling `uart_wait_tx_done(RS485_UART_PORT, 0)`.
3. **Transition FSM** : S'assurer que le passage se fait vers `MSTP_AWAIT_REPLY` après TX si une réponse est attendue.
4. **Who-Is Global** : Implémenter la trame Broadcast 0x06 avec NPDU global.

## 📜 Rappel du Workflow Strict
- **Compilation** : `./utils/compil.sh`
- **Flash OTA** : `./utils/flashOTA.sh`
- **Journalisation** : Mettre à jour `TRACE.md` après chaque succès technique.
- **WhatsApp** : Prévenir l'utilisateur au début et à la fin de chaque étape majeure.

---
*Fin du Prompt de Reprise.*
