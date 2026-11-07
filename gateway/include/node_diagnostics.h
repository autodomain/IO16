#pragma once
#include <Arduino.h>

#define MAX_DIAG_NODES 63

struct RemoteNodeInfo {
    bool active;             // Nodo configurato o rilevato
    bool online;             // Stato online/offline
    uint8_t bus_id;          // Bus RS485 (1 o 2)
    unsigned long last_seen; // Millis dell'ultimo pacchetto
    uint16_t packets_rx;     // Pacchetti ricevuti
    uint16_t ping_fails;     // Ping consecutivi falliti
    float temp;              // Ultima temperatura letta dal nodo (°C)
    bool has_temp;           // True se ha inviato una lettura di temperatura valida
    uint8_t attempts;        // Tentativi necessari per l'ultimo invio riuscito (1..20)
    uint8_t quality;         // Qualita' percentuale calcolata (0..100%)
    uint16_t fails_total;    // Conteggio storico fallimenti / pacchetti persi
    uint16_t packets_tx_ok;  // Invii riusciti con successo verso questo nodo
    uint8_t backlight_pct;   // Percentuale luminosità standby (0..100%)
    uint8_t led_mode;        // Modalità accensione LED (0..3)
    bool has_config;         // True se la configurazione è stata letta dal nodo
};

extern RemoteNodeInfo remote_nodes[MAX_DIAG_NODES + 1];

void node_diag_init();
void node_diag_mark_seen(uint8_t node_id, uint8_t bus_id = 1);
void node_diag_set_temp(uint8_t node_id, float temp_c);
void node_diag_record_attempts(uint8_t node_id, uint8_t attempts, bool success);
void node_diag_loop();
void node_diag_force_ping_all();
void node_diag_start_discovery_scan();
bool node_diag_is_scanning();
void node_diag_clear_all();
void pjon_send_ping(uint8_t addr);
void pjon_send_temp_req(uint8_t addr);
void pjon_send_config_req(uint8_t addr);
void pjon_send_config_set(uint8_t addr, uint8_t backlight_pct, uint8_t led_mode);

