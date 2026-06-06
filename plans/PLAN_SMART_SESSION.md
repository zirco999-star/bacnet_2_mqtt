# Plan: Gestion intelligente des sessions concurrentes (v6.5.4)

## Objectif
Permettre la connexion de plusieurs clients distincts (ex: PC + Smartphone) tout en empêchant l'empilement de sessions "fantômes" provenant du même appareil lors d'un rafraîchissement (problème typique du "slide refresh" sur iPhone).

## Analyse technique
Actuellement, v6.5.3 ferme TOUT dès qu'un nouveau client arrive. Pour être plus fin, nous devons identifier l'appelant via son adresse IP (`client->remoteIP()`).

## Changements proposés

### 1. Politique de nettoyage par IP (`src/z_network.cpp`)
Dans le callback `onEvent` (cas `WS_EVT_CONNECT`) :
- Parcourir la liste des clients existants.
- Si un client possède la **même adresse IP** que le nouveau arrivant, fermer l'ancienne session.
- Cela libère les buffers TCP de l'ancienne page avant d'activer la nouvelle, tout en laissant les autres IP (autres appareils) connectées.

### 2. Sécurisation du parcours de liste
L'itération sera protégée par le mutex `ws_mutex`.

### 3. Ré-activation progressive des services
- Restaurer les logs INFO pour le suivi des connexions (via une queue pour éviter la réentrance).
- Maintenir le header `Connection: close` pour les APIs HTTP.

## Étapes d'exécution
1.  **Commit v6.5.3** : Sécuriser l'état actuel.
2.  **Refactor `onEvent`** : Implémenter la détection par IP.
3.  **Versioning** : Passage en **v6.5.4**.
4.  **Déploiement** : Compilation et flash OTA.
