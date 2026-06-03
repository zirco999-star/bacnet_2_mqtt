# Plan de Remédiation FSM BACnet MS/TP - Hard Real-Time & Ring Stability (v6.0.4)

## État des Lieux
L'implémentation actuelle v6.0.3 respecte les timings microsecondes mais a perdu la capacité à maintenir l'anneau logique (Ring). L'ESP32 s'auto-envoie le jeton ou ne découvre pas son successeur, ce qui bloque le polling après la première interaction.

## Objectifs
1. Restaurer l'apprentissage dynamique du successeur (`next_station`).
2. Centraliser toutes les émissions via le Gatekeeper `send_mstp_frame` (T_turnaround).
3. Utiliser `micros()` pour TOUTES les mesures de silence.
4. Rendre la découverte de l'anneau (Poll For Master) conforme à l'ASHRAE 135.

## Phase 1 : Variables de Contrôle de l'Anneau
Ajouter au niveau global (statique) :
- `static uint8_t next_station = 127;` (Défaut : nous-mêmes ou broadcast)
- `static uint8_t poll_station = 0;` (Utilisé pour itérer dans POLL_FOR_MASTER)

## Phase 2 : Refactoring de la Réception (Apprentissage)
Dans `MSTP_IDLE` (lors de `ReceivedValidFrame`) :
- **Si Type == 0x01 (PFM) et Dest == Me** :
    - Répondre avec un `Reply To PFM` (0x02).
    - **APPRENTISSAGE** : `next_station = src_mac;` (Celui qui nous cherche devient notre cible de jeton).
- **Si Type == 0x00 (Token) et Dest == Me** :
    - On a le jeton. Basculer en `MSTP_USE_TOKEN`.
    - *Note : Ne plus forcer next_station ici, il doit être stable.*

## Phase 3 : Refactoring de l'Émission (Passage de Jeton)
Dans `MSTP_PASS_TOKEN` :
- Utiliser `send_mstp_frame(next_station, 0x00, NULL, 0);`.
- Si on ne reçoit rien après plusieurs essais (ASHRAE `N_retry_token`), basculer en `MSTP_POLL_FOR_MASTER`.

## Phase 4 : Logic de Poll For Master (Découverte du Voisin)
Dans `MSTP_POLL_FOR_MASTER` :
- Itérer `poll_station = (sysCfg.mac_address + 1) % 128`.
- Envoyer `PFM` (0x01) à `poll_station`.
- Attendre `T_usage_timeout` (30ms).
- Si `Reply To PFM` (0x02) reçu : `next_station = poll_station;` -> Ring Stable.

## Phase 5 : Correction des Timers de Silence
- S'assurer que `last_rx_time_us` est mis à jour à CHAQUE octet reçu dans la boucle UART.
- S'assurer que `uart_tx` attend la fin physique (`uart_wait_tx_done`) avant de réinitialiser le silence.

## Validation
- Compilation v6.0.4.
- Flash OTA.
- Log attendu : `[RING] Learned Successor: X` ou `[RING] Stable`.
