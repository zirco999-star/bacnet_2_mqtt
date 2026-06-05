# Plan de Rémédiation : Implémentation du Mode Burst (Max_Info_Frames)

## Problématique
La FSM actuelle n'exploite pas pleinement le paramètre `Max_Info_Frames`. Bien que la structure permette théoriquement d'envoyer plusieurs trames, l'ordonnancement est sous-optimal et ne respecte pas strictement les transitions "SendAnotherFrame" et "NothingToSend" de la norme ASHRAE 135. Cela limite le débit de données sur le bus MS/TP, car la Gateway rend le jeton trop tôt.

## Objectifs
1.  **Augmenter le Throughput** : Permettre l'envoi de jusqu'à `sysCfg.max_info_frames` (défaut: 3) trames de données par possession de jeton.
2.  **Conformité Normative** : Implémenter les transitions explicites "NothingToSend" (libération immédiate du jeton) et "SendAnotherFrame" (boucle interne).
3.  **Optimisation du Polling** : S'assurer que le burst traite des objets différents pour maximiser l'efficacité d'un cycle de jeton.

## Stratégie de Modification (`z_bacnet.cpp`)

### 1. État `MSTP_USE_TOKEN`
- Vérifier la présence de travaux (Queue ou Polling) au tout début.
- Si rien n'est à envoyer : Transition immédiate vers `DONE_WITH_TOKEN` avec `frame_count = max` (Action "NothingToSend").
- Si un travail est trouvé : Incrémenter `frame_count` et passer à `WAIT_FOR_REPLY`.

### 2. État `MSTP_DONE_WITH_TOKEN`
- Transition "SendAnotherFrame" : Si `frame_count < max` ET qu'il reste des travaux en attente (Queue ou objets à polluer), boucler vers `USE_TOKEN`.
- Sinon : Passer le jeton (`MSTP_PASS_TOKEN`).

### 3. Gestion du Polling Circulaire
- Garantir que `current_poll_idx` progresse correctement à chaque itération de la rafale pour ne pas relire le même objet.

## Étapes d'Exécution

### Étape 1 : Analyse des conditions de "Travail en attente"
Définir une fonction ou une macro `has_bacnet_work()` qui vérifie :
- `uxQueueMessagesWaiting(bacnet_job_queue) > 0`
- OU existence d'au moins un objet dans le cache nécessitant un polling (`o.enabled && (o.last_update == 0 || intervalle_écoulé)`).

### Étape 2 : Refactoring de `MSTP_USE_TOKEN`
Appliquer la logique de décision dès l'entrée dans l'état pour éviter les cycles CPU inutiles.

### Étape 3 : Refactoring de `MSTP_DONE_WITH_TOKEN`
Rendre la boucle de burst conditionnelle à la présence réelle de données à envoyer.

### Étape 4 : Validation et Tests
- **Test 1** : Monitorer les logs `Poll Request`. On doit voir des groupes de N requêtes (N = `max_info_frames`) se suivre rapidement entre deux messages "Passing token".
- **Test 2** : Vérifier que le temps de rafraîchissement total de 98 objets diminue significativement.
- **Test 3** : Vérifier la stabilité du Ring (pas de dépassement de `T_usage_timeout`).

## Risques Identifiés
- **Saturation du Bus** : Si `max_info_frames` est trop élevé, les autres nœuds pourraient subir des latences. (Garder la valeur par défaut à 3).
- **Complexité des Mutex** : La boucle de burst doit gérer correctement le `cache_mutex` pour ne pas bloquer les autres tâches pendant trop longtemps.

## Version Cible
`v6.2.0`
