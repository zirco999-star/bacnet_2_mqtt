import os

trace_path = '/mnt/save/bacnet_2_mqtt/TRACE.md'
with open(trace_path, 'a', encoding='utf-8') as f:
    f.write('''
## [v6.8.5] - 2026-06-09
### Ajout
- **Phase 1** : Refactorisation structurelle complète selon `CONVENTION_CODAGE.md` (`uc`, `ul`, `x`, `pd`, etc.).
- **Phase 2** : Intégration de la persistance NVS étendue (champs réseau dynamiques : Max_APDU, Timeout, Retries).
- **Phase 3** : Implémentation du moteur de Polling par Lot (ReadPropertyMultiple - RPM) pour optimiser les performances du bus.
- **Phase 4** : Découverte dynamique BACnet (lecture adaptative des limites réseaux distantes via la FSM MS/TP).
- Mise à jour stricte de `GEMINI.md` pour imposer les normes de codage et le workflow de compilation.
''')
