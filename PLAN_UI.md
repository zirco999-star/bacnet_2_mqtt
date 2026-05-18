# Plan d'implémentation : Phase 0.1 - Enrichissement UI & Persistance

## Objectif
Améliorer l'expérience utilisateur et la fiabilité de la configuration en assurant une remontée d'information précise et une persistance visuelle des réglages.

## 1. Backend (Logiciel ESP32)
*   **API de Configuration (`/api/config`)** :
    *   Créer un endpoint JSON renvoyant la structure `sysCfg` actuelle (masquage du mot de passe pour la sécurité).
    *   Permettre au frontend de pré-remplir les champs (SSID, IP Statique, etc.).
*   **API de Statut (`/api/status`)** :
    *   Créer un endpoint JSON (ou message WS structuré) pour le RSSI, l'IP réelle, le Heap et l'Uptime.
*   **Route `/save` enrichie** :
    *   Réintégrer la capture des champs `gateway` et `subnet`.
    *   Ajouter une validation de format IP avant sauvegarde.

## 2. Frontend (Interface Web v2.2)
*   **Chargement Dynamique** :
    *   Au chargement de la page, appeler `/api/config` pour remplir le formulaire.
*   **Correction du Dashboard** :
    *   Remplacer l'extraction par Regex (fragile) par la consommation du JSON `/api/status`.
*   **Formulaire Complet** :
    *   Restaurer les champs : Passerelle, Masque, Préfixe MQTT, et Intervalle de Polling.
    *   Ajouter un indicateur visuel de "Sauvegarde en cours".

## 3. Robustesse WiFi
*   **Validation de l'IP Statique** :
    *   S'assurer que si `static_ip` est activé mais que les champs sont vides, le système retombe proprement en DHCP au lieu de 0.0.0.0.

## Prochaines étapes (après validation) :
1.  Mise à jour de `z_config.h` (si champs manquants).
2.  Mise à jour de `z_network.cpp` (API JSON + Route Save).
3.  Mise à jour de `z_ui.h` (Formulaire complet + Logic JS).
