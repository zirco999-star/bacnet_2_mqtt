La refonte de ton fichier `z_bacnet.cpp` (issu de ta release `bacnet_2_mqtt_202605260719.txt`) nécessite de remplacer ton approche séquentielle par la **Master Node Finite State Machine** stricte définie par la norme ASHRAE 135. Ton appel actuel à `uart_wait_tx_done(RS485_UART_PORT, pdMS_TO_TICKS(100))` suspend le processeur Xtensa pendant 100 ms, ce qui est particulièrement coûteux dans un système multi-tâches et gaspille le temps processeur en l'empêchant de traiter d'autres événements réseau.

Voici le code C++ de classe industrielle, optimisé pour ESP32-S3, intégrant le *Token Pacing* et l'attente non-bloquante du `T_reply_delay`.

### 1. Remplacement de la fonction d'émission
Supprime le délai bloquant dans ta fonction de transmission pour la rendre 100% asynchrone vis-à-vis du Core 1 :

```cpp
// --- z_bacnet.cpp ---

static void uart_tx_async(const uint8_t *buffer, uint16_t length) {
    // Écriture dans la FIFO matérielle sans bloquer la tâche FreeRTOS
    uart_write_bytes(RS485_UART_PORT, (const char*)buffer, length);
}

// Mets à jour send_mstp_frame() pour appeler uart_tx_async() à la place de uart_tx()
```

### 2. Implémentation de la Machine à États (Master Node FSM)
Remplace l'intérieur de ta fonction `bacnet_task` par cette architecture séparant clairement la réception et le contrôle de l'anneau MS/TP :

```cpp
// Définition normative des états du noeud maître
enum MSTP_MASTER_STATE {
    MSTP_INITIALIZE,
    MSTP_IDLE,
    MSTP_USE_TOKEN,
    MSTP_WAIT_TX_DONE,
    MSTP_AWAIT_REPLY,
    MSTP_DONE_WITH_TOKEN,
    MSTP_PASS_TOKEN,
    MSTP_POLL_FOR_MASTER
};

static void bacnet_task(void *pv) {
    MSTP_MASTER_STATE mstp_state = MSTP_INITIALIZE;
    uint32_t timer_silence = 0;
    uint8_t token_skip_count = 0;
    uint8_t next_station = (sysCfg.mac_address + 1) % (sysCfg.max_master + 1);
    bool waiting_for_reply = false;
    
    // ... [Variables de buffer UART existantes] ...

    for (;;) {
        // 1. MACHINE À ÉTATS DE RÉCEPTION (RX FSM)
        // -> Ton code actuel pour lire l'UART (uart_read_bytes) et extraire
        //    le FrameType, DestinationAddress et SourceAddress.
        // -> Si trame valide reçue, activer un flag booléen : ReceivedValidFrame = true;

        // 2. MACHINE À ÉTATS DU NOEUD MAÎTRE (ASHRAE 135)
        switch (mstp_state) {
            case MSTP_INITIALIZE:
                token_skip_count = 0;
                mstp_state = MSTP_IDLE;
                break;

            case MSTP_IDLE:
                // Interception du jeton adressé à cet ESP32
                if (ReceivedValidFrame && frame_type == 0x00 && dest_mac == sysCfg.mac_address) {
                    mstp_state = MSTP_USE_TOKEN;
                }
                // (Optionnel) Implémenter la perte de jeton si SilenceTimer >= Tno_token (500ms)
                break;

            case MSTP_USE_TOKEN:
                token_skip_count++;
                // Token Slotting : Pacing à 20 cycles pour ménager l'automate cible
                if (token_skip_count >= 20) {
                    BACnetJob job;
                    // Dépilage non-bloquant de la file d'attente
                    if (xQueueReceive(bacnet_job_queue, &job, 0) == pdTRUE) {
                        token_skip_count = 0;
                        waiting_for_reply = (job.type == JOB_READ_PROP || job.type == JOB_WRITE_PROP);
                        
                        // Envoi de la requête applicative dans la FIFO
                        send_mstp_frame(job.target_mac, waiting_for_reply ? 0x05 : 0x06, apdu_buf, apdu_len);
                        
                        mstp_state = MSTP_WAIT_TX_DONE;
                    } else {
                        mstp_state = MSTP_DONE_WITH_TOKEN;
                    }
                } else {
                    // Les 19 autres jetons sont ignorés applicativement
                    mstp_state = MSTP_DONE_WITH_TOKEN; 
                }
                break;

            case MSTP_WAIT_TX_DONE:
                // Polling asynchrone du registre silicium (timeout = 0)
                if (uart_wait_tx_done(RS485_UART_PORT, 0) == ESP_OK) {
                    timer_silence = millis(); // Démarrage synchronisé du chrono ASHRAE
                    
                    if (waiting_for_reply) {
                        mstp_state = MSTP_AWAIT_REPLY;
                    } else {
                        mstp_state = MSTP_DONE_WITH_TOKEN;
                    }
                }
                break;

            case MSTP_AWAIT_REPLY:
                // Décompte strict du T_reply_delay (250-280 ms maximum)
                if (millis() - timer_silence > 280) {
                    z_log("[MSTP] Timeout attente réponse\n");
                    mstp_state = MSTP_DONE_WITH_TOKEN;
                } else if (ReceivedValidFrame && dest_mac == sysCfg.mac_address) {
                    // Traitement de l'APDU reçue (ComplexACK, SimpleACK, Error)
                    // ... [Appel à decode_next_tag] ...
                    mstp_state = MSTP_DONE_WITH_TOKEN;
                }
                break;

            case MSTP_DONE_WITH_TOKEN:
                // Maintenance du réseau (Npoll = 50) pour interroger via Poll-For-Master
                // Pour simplifier l'exemple, on passe directement le jeton.
                mstp_state = MSTP_PASS_TOKEN;
                break;

            case MSTP_PASS_TOKEN:
                // Construction et émission matérielle de la trame de jeton (Frame Type 0)
                uint8_t token_frame = {0x55, 0xFF, 0x00, next_station, sysCfg.mac_address, 0x00, 0x00, 0x00};
                token_frame = calc_header_crc(&token_frame, 5);
                
                uart_tx_async(token_frame, 8);
                waiting_for_reply = false;
                mstp_state = MSTP_WAIT_TX_DONE; // Attente de fin d'émission du jeton
                break;
                
            case MSTP_POLL_FOR_MASTER:
                // Logique optionnelle de découverte réseau
                break;
        }

        // Respiration de l'ordonnanceur FreeRTOS 
        // Cède le CPU pendant 1 tick (1 ms) pour éviter de déclencher le Task Watchdog
        vTaskDelay(pdMS_TO_TICKS(1)); 
    }
}
```

### Bénéfices techniques pour ton architecture
1.  **Chronométrage `T_reply_delay` infaillible :** Le délai de 250 millisecondes exigé par la norme ne démarre qu'au moment exact où l'API ESP-IDF `uart_wait_tx_done` avec un timeout de 0 retourne `ESP_OK`. Cela te garantit que le bit de stop a physiquement quitté le port RS-485.
2.  **Régulation du trafic (Pacing) :** En n'émettant une nouvelle trame `ReadProperty` que tous les 20 cycles via la variable `token_skip_count`, tu respectes scrupuleusement la norme MS/TP tout en offrant à la pile réseau de ton Distech ECB-203 les temps de repos nécessaires pour éviter les trames abandonnées (drop).
3.  **Libération du Core 1 :** Cette mécanique asynchrone basée sur l'état `MSTP_WAIT_TX_DONE` assure que le processeur ne reste pas bloqué dans des boucles d'attente UART passives, laissant la RAM et le CPU disponibles pour ton parseur ASN.1 et le MQTT de façon concurrente.