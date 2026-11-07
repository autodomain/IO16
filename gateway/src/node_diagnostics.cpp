#include "node_diagnostics.h"
#include <PubSubClient.h>
#include <EEPROM.h>
#include "config.h"

extern PubSubClient client;
extern void pjon_send_ping(uint8_t addr);
extern uint64_t known_nodes_mask;

RemoteNodeInfo remote_nodes[MAX_DIAG_NODES + 1];

#define NODE_STORAGE_MAGIC 0x4E4F4445 // "NODE"
#define NODE_STORAGE_ADDR 256

struct NodeRegistryStorage {
    uint32_t magic;
    uint64_t registered_mask; // 64 bit: bit i = 1 se il nodo i è registrato (1..63)
    uint8_t bus_id[64];       // Bus RS485 (1 o 2)
    uint8_t backlight[64];    // Backlight (0..100)
    uint8_t led_mode[64];     // Modo LED (0..3)
};

static NodeRegistryStorage node_storage;

static void node_storage_write() {
    EEPROM.put(NODE_STORAGE_ADDR, node_storage);
}

void node_diag_save_node(uint8_t node_id) {
    if (node_id < 1 || node_id > MAX_DIAG_NODES) return;
    node_storage.magic = NODE_STORAGE_MAGIC;
    node_storage.registered_mask |= ((uint64_t)1 << node_id);
    node_storage.bus_id[node_id] = remote_nodes[node_id].bus_id ? remote_nodes[node_id].bus_id : 1;
    node_storage.backlight[node_id] = remote_nodes[node_id].backlight_pct;
    node_storage.led_mode[node_id] = remote_nodes[node_id].led_mode;
    node_storage_write();
}

void node_diag_save_all() {
    node_storage.magic = NODE_STORAGE_MAGIC;
    for (uint8_t i = 1; i <= MAX_DIAG_NODES; i++) {
        if (remote_nodes[i].active) {
            node_storage.registered_mask |= ((uint64_t)1 << i);
            node_storage.bus_id[i] = remote_nodes[i].bus_id ? remote_nodes[i].bus_id : 1;
            node_storage.backlight[i] = remote_nodes[i].backlight_pct;
            node_storage.led_mode[i] = remote_nodes[i].led_mode;
        }
    }
    node_storage_write();
}

static unsigned long last_check_time = 0;
static unsigned long last_ping_time = 0;
static bool startup_ping_active = false;
static uint8_t startup_ping_cur = 1;
static unsigned long last_startup_ping = 0;

void node_diag_init() {
    memset(remote_nodes, 0, sizeof(remote_nodes));
    
    // Carica registro interfacce persistenti da EEPROM
    EEPROM.get(NODE_STORAGE_ADDR, node_storage);
    if (node_storage.magic == NODE_STORAGE_MAGIC) {
        for (uint8_t i = 1; i <= MAX_DIAG_NODES; i++) {
            if (node_storage.registered_mask & ((uint64_t)1 << i)) {
                remote_nodes[i].active = true;
                remote_nodes[i].online = false; // Sarà confermato online dal ping di verifica immediato
                remote_nodes[i].bus_id = node_storage.bus_id[i] ? node_storage.bus_id[i] : 1;
                remote_nodes[i].backlight_pct = node_storage.backlight[i] ? node_storage.backlight[i] : 12;
                remote_nodes[i].led_mode = node_storage.led_mode[i];
                remote_nodes[i].has_config = true;
                remote_nodes[i].last_seen = 0;
                known_nodes_mask |= ((uint64_t)1 << i);
            }
        }
    } else {
        // Prima inizializzazione assoluta (nessun nodo salvato)
        memset(&node_storage, 0, sizeof(node_storage));
        node_storage.magic = NODE_STORAGE_MAGIC;
        uint8_t count = config.ha_nodes_count;
        if (count > MAX_DIAG_NODES) count = MAX_DIAG_NODES;
        for (uint8_t i = 1; i <= count; i++) {
            remote_nodes[i].active = true;
            remote_nodes[i].online = false;
            remote_nodes[i].bus_id = 1;
            remote_nodes[i].backlight_pct = 12;
            remote_nodes[i].led_mode = 1;
            remote_nodes[i].has_config = false;
            node_storage.registered_mask |= ((uint64_t)1 << i);
            node_storage.bus_id[i] = 1;
            node_storage.backlight[i] = 12;
            node_storage.led_mode[i] = 1;
            known_nodes_mask |= ((uint64_t)1 << i);
        }
        node_storage_write();
    }

    // Abilita la sequenza di interrogazione istantanea all'avvio
    startup_ping_active = true;
    startup_ping_cur = 1;
    last_startup_ping = 0;
}

static void publish_node_ha_status(uint8_t node_id, bool is_online) {
    if (!client.connected()) return;
    
    char status_topic[56];
    snprintf_P(status_topic, sizeof(status_topic), PSTR("%s/node/%u/status"), config.mqtt_client_id, node_id);
    client.publish(status_topic, is_online ? "online" : "offline", true);
}

void node_diag_set_temp(uint8_t node_id, float temp_c) {
    if (node_id < 1 || node_id > MAX_DIAG_NODES) return;
    remote_nodes[node_id].temp = temp_c;
    remote_nodes[node_id].has_temp = true;
}

void node_diag_record_attempts(uint8_t node_id, uint8_t attempts, bool success) {
    if (node_id < 1 || node_id > MAX_DIAG_NODES) return;
    if (attempts == 0) attempts = 1;
    remote_nodes[node_id].attempts = attempts;

    if (!success || attempts >= 20) {
        remote_nodes[node_id].fails_total++;
        remote_nodes[node_id].ping_fails++;
        if (remote_nodes[node_id].ping_fails >= 2) {
            remote_nodes[node_id].quality = 0;
        } else {
            remote_nodes[node_id].quality = (remote_nodes[node_id].quality > 50) ? 30 : 15;
        }
    } else {
        remote_nodes[node_id].packets_tx_ok++;
        remote_nodes[node_id].ping_fails = 0;

        uint8_t target_q;
        if (attempts == 1) target_q = 100;
        else if (attempts == 2) target_q = 80;
        else if (attempts == 3) target_q = 65;
        else if (attempts == 4) target_q = 50;
        else if (attempts <= 6) target_q = 30;
        else target_q = 15;

        // Se il nodo ha fallito in passato (fails_total > 0), NON ripristinare subito a 100%!
        if (remote_nodes[node_id].fails_total > 0) {
            if (remote_nodes[node_id].quality == 0) {
                remote_nodes[node_id].quality = 30;
            } else {
                remote_nodes[node_id].quality = (uint8_t)(((uint16_t)remote_nodes[node_id].quality * 3 + target_q) / 4);
            }
            uint32_t tot = (uint32_t)remote_nodes[node_id].packets_tx_ok + remote_nodes[node_id].fails_total;
            uint8_t stat_cap = (uint8_t)(((uint32_t)remote_nodes[node_id].packets_tx_ok * 100) / tot);
            if (remote_nodes[node_id].quality > stat_cap) {
                remote_nodes[node_id].quality = stat_cap;
            }
        } else {
            // Nodo che non ha MAI fallito
            remote_nodes[node_id].quality = target_q;
        }
    }
}

void node_diag_mark_seen(uint8_t node_id, uint8_t bus_id) {
    if (node_id < 1 || node_id > MAX_DIAG_NODES) return;

    bool was_unregistered = !(node_storage.registered_mask & ((uint64_t)1 << node_id));

    remote_nodes[node_id].active = true;
    remote_nodes[node_id].bus_id = bus_id;
    remote_nodes[node_id].last_seen = millis();
    remote_nodes[node_id].packets_rx++;
    remote_nodes[node_id].ping_fails = 0;
    
    // Se non era ancora registrato in EEPROM, memorizzalo immediatamente in modo permanente!
    if (was_unregistered) {
        node_diag_save_node(node_id);
        known_nodes_mask |= ((uint64_t)1 << node_id);
    }
    
    // Se la qualita' era azzerata (era offline o appena avviato):
    if (remote_nodes[node_id].quality == 0) {
        if (remote_nodes[node_id].fails_total > 0) {
            remote_nodes[node_id].quality = 40; // Recupero cauto per nodi che hanno fallito in passato
        } else {
            remote_nodes[node_id].quality = 100; // Nodo senza errori passati
            remote_nodes[node_id].attempts = 1;
        }
    }

    if (!remote_nodes[node_id].online) {
        remote_nodes[node_id].online = true;
        publish_node_ha_status(node_id, true);
    }

    if (client.connected()) {
        char q_topic[56];
        char q_val[8];
        snprintf_P(q_topic, sizeof(q_topic), PSTR("%s/node/%u/quality"), config.mqtt_client_id, node_id);
        snprintf_P(q_val, sizeof(q_val), PSTR("%u"), remote_nodes[node_id].quality);
        client.publish(q_topic, q_val, true);
    }

    // Se non abbiamo ancora la temperatura di questo nodo, richiedila subito!
    if (!remote_nodes[node_id].has_temp) {
        pjon_send_temp_req(node_id);
    }
    // Se non abbiamo ancora la configurazione (backlight e modo), richiedila subito!
    if (!remote_nodes[node_id].has_config) {
        pjon_send_config_req(node_id);
    }
}

// Discovery Scan: scansione scaglionata di tutti i 63 nodi su entrambi i bus
// senza saturare il bus (un nodo ogni 60 ms), avviata SOLO su richiesta (tasto Web).
static bool is_scanning = false;
static uint8_t scan_cur_node = 1;
static unsigned long last_scan_step = 0;

void node_diag_start_discovery_scan() {
    is_scanning = true;
    scan_cur_node = 1;
    last_scan_step = 0;
}

bool node_diag_is_scanning() {
    return is_scanning;
}

void node_diag_clear_all() {
    is_scanning = false;
    startup_ping_active = false;
    memset(&node_storage, 0, sizeof(node_storage));
    node_storage.magic = NODE_STORAGE_MAGIC;
    node_storage_write();
    known_nodes_mask = 0;

    for (uint8_t i = 1; i <= MAX_DIAG_NODES; i++) {
        remote_nodes[i].active = false;
        remote_nodes[i].online = false;
        remote_nodes[i].bus_id = 0;
        remote_nodes[i].last_seen = 0;
        remote_nodes[i].packets_rx = 0;
        remote_nodes[i].ping_fails = 0;
        remote_nodes[i].has_temp = false;
        remote_nodes[i].temp = 0.0f;
        remote_nodes[i].attempts = 0;
        remote_nodes[i].quality = 0;
        remote_nodes[i].fails_total = 0;
        remote_nodes[i].packets_tx_ok = 0;
        remote_nodes[i].backlight_pct = 12;
        remote_nodes[i].led_mode = 1;
        remote_nodes[i].has_config = false;
    }
}

void node_diag_force_ping_all() {
    node_diag_start_discovery_scan();
}

static uint8_t ping_mode = 0;      // 0=nessuno, 2=solo nodi silenziosi
static uint8_t ping_idx = 0;
static unsigned long last_stagger = 0;

static void node_diag_ping_step(unsigned long now) {
    if (ping_mode == 0 || now - last_stagger < 30) return;
    last_stagger = now;
    while (ping_idx <= MAX_DIAG_NODES) {
        uint8_t i = ping_idx++;
        if (!remote_nodes[i].active || remote_nodes[i].last_seen == 0) continue;
        if (now - remote_nodes[i].last_seen <= 15000) continue;
        pjon_send_ping(i);
        return;
    }
    ping_mode = 0;
}

bool ota_is_active();

void node_diag_loop() {
    if (ota_is_active()) return;
    unsigned long now = millis();

    // 0. Sequenza di verifica istantanea all'avvio per tutti i nodi registrati in EEPROM
    if (startup_ping_active && (now >= 500) && (now - last_startup_ping >= 40)) {
        last_startup_ping = now;
        bool found = false;
        while (startup_ping_cur <= MAX_DIAG_NODES) {
            uint8_t nid = startup_ping_cur++;
            if (remote_nodes[nid].active) {
                pjon_send_ping(nid);
                found = true;
                break;
            }
        }
        if (!found) {
            startup_ping_active = false;
        }
    }

    // Avanzamento Discovery Scan (un nodo ogni 60 ms)
    if (is_scanning && (now - last_scan_step >= 60)) {
        last_scan_step = now;
        if (scan_cur_node <= MAX_DIAG_NODES) {
            pjon_send_ping(scan_cur_node);
            scan_cur_node++;
        } else {
            is_scanning = false;
        }
    }

    // Controlla timeout ogni 5 secondi
    if (now - last_check_time >= 5000) {
        last_check_time = now;

        for (uint8_t i = 1; i <= MAX_DIAG_NODES; i++) {
            if (!remote_nodes[i].active) continue;

            // Se il nodo non ha mai risposto ed e' passato piu' di 20s dall'avvio
            if (remote_nodes[i].last_seen == 0) {
                if (now > 20000 && remote_nodes[i].online) {
                    remote_nodes[i].online = false;
                    publish_node_ha_status(i, false);
                }
                continue;
            }

            // Se il nodo e' online ma non risponde da piu' di 45 secondi -> OFFLINE
            if (remote_nodes[i].online && (now - remote_nodes[i].last_seen > 45000)) {
                remote_nodes[i].online = false;
                remote_nodes[i].quality = 0;
                publish_node_ha_status(i, false);
            }
        }
    }

    // Polling periodico rilassato della temperatura per tutti i nodi online (ogni 60 secondi, 1 nodo al secondo)
    static unsigned long temp_cycle_start = 0;
    static uint8_t temp_query_node = 0;
    static unsigned long last_node_temp_step = 0;

    // Avvia un nuovo ciclo ogni 60 secondi (o subito all'avvio dopo 5s)
    if (now >= 5000 && now - temp_cycle_start >= 60000UL) {
        temp_cycle_start = now;
        temp_query_node = 1; // Inizia dal primo nodo
    }

    // Avanza di 1 nodo ogni 1000 ms (molto rilassato: zero carico sul bus)
    if (temp_query_node > 0 && (now - last_node_temp_step >= 1000)) {
        last_node_temp_step = now;
        bool found = false;
        while (temp_query_node <= MAX_DIAG_NODES) {
            uint8_t nid = temp_query_node++;
            if (remote_nodes[nid].active && remote_nodes[nid].online) {
                pjon_send_temp_req(nid);
                found = true;
                break;
            }
        }
        if (!found) {
            temp_query_node = 0; // Giro completato per tutti i nodi online
        }
    }

    // Ping periodico ogni 25 secondi ai soli nodi gia' registrati ma silenziosi (> 15s)
    if (!is_scanning && ping_mode == 0 && now - last_ping_time >= 25000) {
        last_ping_time = now;
        ping_mode = 2;
        ping_idx = 1;
    }
    node_diag_ping_step(now);
}
