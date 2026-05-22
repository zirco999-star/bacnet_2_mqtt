# Plan de Refonte de l'Interface Utilisateur (UI v4.4.0+)

## Objectif
Créer une interface web "Produit Fini", moderne, responsive et intuitive pour superviser la passerelle BACnet.

## 1. Architecture & Design (Look & Feel)
- **Style Industriel Sombre** : Conserver la palette actuelle (#0b1120, #1e293b, #f59e0b) mais améliorer l'espacement et la hiérarchie.
- **Design Adaptatif** : Utilisation de Flexbox/Grid pour une lisibilité parfaite sur smartphone (mobile-first).
- **Zéro Dépendance** : CSS pur (pas de Bootstrap/Tailwind externe) pour garantir le fonctionnement en mode AP.

## 2. Fonctionnalités de Gestion des Objets
- **Tableau Dynamique** :
    - Remplissage par API JSON `/api/objects`.
    - Colonnes : Activation (Checkbox), Nom (Éditable), Type, Valeur actuelle.
- **Persistance** : 
    - Le flag `enabled` (sauvegardé en NVS) contrôle si l'objet est pollé par le Core 1.
    - Édition des noms : Enregistrement dans une nouvelle structure NVS (ou concaténation dans la structure actuelle).
- **Contrôle Global** :
    - Switch global "MQTT Enable" en header pour activer/désactiver le pont sans redémarrage.

## 3. Maintenance Système
- **Bouton RESET CACHE** : 
    - Action sécurisée par `confirm()` JavaScript.
    - endpoint `/api/reset_cache` (POST).
    - Redémarrage automatique du système.
- **Export EDE (CSV)** : 
    - Bouton "Export" téléchargeant la liste des objets découverts au format EDE.
    - endpoint `/api/export_ede` (GET).

## 4. Roadmap de Développement
1. **Implémentation Backend** : Finaliser l'API JSON (Vérifier la mémoire stack de `JsonDocument`).
2. **Implémentation Frontend** : Remplacer `z_ui.h` par le nouveau template HTML/JS.
3. **Tests UI** : Valider l'édition des noms et la réactivité du tableau sur mobile.
4. **Validation Finale** : Test de l'export EDE avec un outil comme YABE pour vérifier la structure CSV.
