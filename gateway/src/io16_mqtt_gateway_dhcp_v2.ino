/* PJON MAX485 serial modules RS485 communication Gateway with Home Assistant
 * Auto-Discovery */

// Include async ack defining PJON_INCLUDE_ASYNC_ACK before including PJON.h
#define PJON_INCLUDE_ASYNC_ACK true
/* Set 10 milliseconds as the maximum timeframe between
   transmission and synchronous acknowledgement response. */
#define TS_RESPONSE_TIME_OUT 10000
/* Set the back-off exponential degree (default 4) */
#define TS_BACK_OFF_DEGREE 4

/* Set the maximum sending attempts (default 20) */
#define TS_MAX_ATTEMPTS 20

// Compatibilità pin-IO PJON per MCU AVR (MegaCore / MightyCore)
// Scrittura pin PJON con pulizia del buffer RX al rilascio di DE/RE: il
// transceiver RS485 genera un byte spurio quando DE torna basso, e PJON lo
// leggerebbe come ACK.
void pjon_io_write(uint8_t pin, uint8_t val);
#define PJON_IO_WRITE(P, V) pjon_io_write(P, V)
#define PJON_RXFLUSH_US 400
#define PJON_IO_MODE(P, V) pinMode(P, V)
#define PJON_IO_READ(P) digitalRead(P)

#include "config.h"
#include "ha_discovery.h"
#include "lm75_sensor.h"
#include "node_diagnostics.h"
#include "ntp_client.h"
#include "ota_flasher.h"
#include "web_server.h"
#include <Ethernet.h>
#include <PJON.h>
#include <PJONThroughSerial.h>
#include <PubSubClient.h>
#include <SPI.h>

// Due interfacce bus PJON ThroughSerial su RS485 (U2 e U3)
PJON<ThroughSerial> bus1;
PJON<ThroughSerial> bus2;

// 5 milliseconds maximum expected latency, higher if necessary
uint32_t latency = 5000;

// PIN HARDWARE BOARD
#if defined(__AVR_ATmega1284__) || defined(__AVR_ATmega1284P__)
const int eth_cs_pin = 2;
extern const int rs485_enable_pin_1 = 12; // U2 su Serial  (UART0)
extern const int rs485_enable_pin_2 = 13; // U3 su Serial1 (UART1)
const int led_activity = 14;
const int led_mqtt = 15;
#else
const int eth_cs_pin = 10;
extern const int rs485_enable_pin_1 = 2;
extern const int rs485_enable_pin_2 = 3;
const int led_activity = 8;
const int led_mqtt = 7;
#endif

// ---- Analizzatore di traffico software (registra byte RX/TX e fronti DE) ----
#define TR_MAX 110
bool tr_on = false;
uint8_t tr_n = 0;
unsigned long tr_t[TR_MAX];
uint8_t tr_d[TR_MAX], tr_b[TR_MAX]; // d: 0=TX 1=RX 2=RX scartato 3=DE
void trace_ev(uint8_t d, uint8_t b) {
  if (tr_on && tr_n < TR_MAX) {
    tr_t[tr_n] = micros();
    tr_d[tr_n] = d;
    tr_b[tr_n] = b;
    tr_n++;
  }
}
class TraceSerial : public Stream {
public:
  HardwareSerial *h;
  TraceSerial(HardwareSerial *s) : h(s) {}
  int available() override { return h->available(); }
  int read() override {
    int v = h->read();
    if (v >= 0)
      trace_ev(1, (uint8_t)v);
    return v;
  }
  int peek() override { return h->peek(); }
  void flush() override { h->flush(); }
  size_t write(uint8_t b) override {
    trace_ev(0, b);
    return h->write(b);
  }
  using Print::write;
};
TraceSerial traceSerial1(&Serial);

void pjon_io_write(uint8_t pin, uint8_t val) {
  digitalWrite(pin, val);
  if (pin == rs485_enable_pin_1)
    trace_ev(3, val);
  if (val == LOW) {
    if (pin == rs485_enable_pin_1) {
      delayMicroseconds(PJON_RXFLUSH_US);
      while (Serial.available()) {
        int v = Serial.read();
        trace_ev(2, (uint8_t)v);
      }
    } else if (pin == rs485_enable_pin_2) {
      delayMicroseconds(PJON_RXFLUSH_US);
      while (Serial1.available())
        Serial1.read();
    }
  }
}

// Callback function header
void callback(char *topic, byte *payload, unsigned int length);

EthernetClient ethClient;
PubSubClient client(ethClient);

unsigned long last_reconnect_attempt = 0;
unsigned long last_telemetry_time = 0;
uint64_t known_nodes_mask = 0; // Maschera bit 64-bit per nodi fino a 63

// ---- Contatori di debug (esposti in /api/status -> "dbg") ----
unsigned long dbg_err_lost = 0, dbg_err_full = 0, dbg_err_other = 0;
unsigned long dbg_send_fail = 0, dbg_btn_rx = 0, dbg_k_rx = 0;
unsigned long dbg_rx_b1 = 0, dbg_rx_b2 = 0;
unsigned long dbg_loop_max_us = 0, dbg_loop_last_us = 0;
unsigned long dbg_sec_max[6] = {0, 0, 0,
                                0, 0, 0}; // web, lm75, mqtt, bus1+2, diag
#define DBG_SEC(i, t)                                                          \
  do {                                                                         \
    unsigned long _n = micros();                                               \
    unsigned long _d = _n - (t);                                               \
    if (_d > dbg_sec_max[i])                                                   \
      dbg_sec_max[i] = _d;                                                     \
    (t) = _n;                                                                  \
  } while (0)
uint8_t dbg_last_err_code = 0;
uint16_t dbg_last_err_data = 0;

struct TrackedPacket {
  uint8_t target;
  uint8_t prev_attempts;
  bool active;
};

static void service_bus_with_tracking(PJON<ThroughSerial> &b, uint8_t bus_id) {
  TrackedPacket tr[PJON_MAX_PACKETS];
  for (uint8_t i = 0; i < PJON_MAX_PACKETS; i++) {
    if (b.packets[i].state != 0) {
      tr[i].target = (uint8_t)b.packets[i].content[0];
      tr[i].prev_attempts = b.packets[i].attempts;
      tr[i].active = true;
    } else {
      tr[i].active = false;
    }
  }

  b.update();

  for (uint8_t i = 0; i < PJON_MAX_PACKETS; i++) {
    if (tr[i].active) {
      uint8_t tgt = tr[i].target;
      if (b.packets[i].state == 0) {
        // Consegnato con successo (ACK ricevuto)
        uint8_t att = tr[i].prev_attempts + 1;
        node_diag_record_attempts(tgt, att, true);
      } else if (b.packets[i].attempts > 1) {
        node_diag_record_attempts(tgt, b.packets[i].attempts, true);
      }
    }
  }

  b.receive();
}

// Serve i due bus PJON (ricezione + ACK + invio con analisi tentativi)
void pjon_service() {
  service_bus_with_tracking(bus1, 1);
  service_bus_with_tracking(bus2, 2);
}

// Test hardware: trasmette 4 byte 0x55 sul bus indicato e conta quanti ne
// rientrano in RX (eco). 0 = ricevitore escluso durante TX, 4 = eco presente.
// Ritorna anche i byte "spuri" presenti in RX prima della trasmissione.
void pjon_echo_test(uint8_t bus_n, uint8_t *pre, uint8_t *echo) {
  HardwareSerial &hs = (bus_n == 2) ? Serial1 : Serial;
  int de = (bus_n == 2) ? rs485_enable_pin_2 : rs485_enable_pin_1;
  uint8_t n = 0;
  while (hs.available()) {
    hs.read();
    if (n < 255)
      n++;
  }
  *pre = n;
  digitalWrite(de, HIGH);
  delay(2);
  for (uint8_t i = 0; i < 4; i++)
    hs.write((uint8_t)0x55);
  hs.flush();
  delay(2);
  digitalWrite(de, LOW);
  delay(5);
  n = 0;
  while (hs.available()) {
    hs.read();
    if (n < 255)
      n++;
  }
  *echo = n;
}

void pjon_send_test(uint8_t addr, uint8_t led, uint8_t stat) {
  char m[2];
  m[0] = led;
  m[1] = stat;
  uint8_t b = (addr <= MAX_DIAG_NODES) ? remote_nodes[addr].bus_id : 0;
  if (b == 1) {
    if (bus1.send(addr, m, 2) == PJON_FAIL)
      dbg_send_fail++;
  } else if (b == 2) {
    if (bus2.send(addr, m, 2) == PJON_FAIL)
      dbg_send_fail++;
  } else {
    // Se non ancora contattato, prova su entrambi i bus
    if (bus1.send(addr, m, 2) == PJON_FAIL)
      dbg_send_fail++;
    if (bus2.send(addr, m, 2) == PJON_FAIL)
      dbg_send_fail++;
  }
  pjon_packets_tx++;
  digitalWrite(led_activity, HIGH);
  delay(10);
  digitalWrite(led_activity, LOW);
}

void pjon_trace_run(uint8_t a, uint8_t l, uint8_t st) {
  tr_n = 0;
  tr_on = true;
  pjon_send_test(a, l, st);
  unsigned long t0 = millis();
  while (millis() - t0 < 250)
    pjon_service();
  tr_on = false;
}

void pjon_send_ping(uint8_t addr) {
  uint8_t b = (addr <= MAX_DIAG_NODES) ? remote_nodes[addr].bus_id : 0;
  // Per nodi già contattati (last_seen > 0) usiamo ACK per misurare i tentativi
  // effettivi Per nodi mai contattati (last_seen == 0) disattiviamo ACK per non
  // saturare la coda in caso di assenza
  bool use_ack = (addr <= MAX_DIAG_NODES && remote_nodes[addr].last_seen > 0);
  uint8_t h1 = use_ack ? (bus1.config | PJON_ACK_REQ_BIT)
                       : (bus1.config & ~PJON_ACK_REQ_BIT);
  uint8_t h2 = use_ack ? (bus2.config | PJON_ACK_REQ_BIT)
                       : (bus2.config & ~PJON_ACK_REQ_BIT);
  if (b == 1) {
    bus1.send(addr, "?", 1, h1);
  } else if (b == 2) {
    bus2.send(addr, "?", 1, h2);
  } else {
    bus1.send(addr, "?", 1, h1);
    bus2.send(addr, "?", 1, h2);
  }
  pjon_packets_tx++;
  digitalWrite(led_activity, HIGH);
  digitalWrite(led_activity, LOW);
}

void pjon_send_temp_req(uint8_t addr) {
  uint8_t b = (addr <= MAX_DIAG_NODES) ? remote_nodes[addr].bus_id : 1;
  uint8_t h = (b == 2 ? bus2.config : bus1.config) & ~PJON_ACK_REQ_BIT;
  if (b == 2)
    bus2.send(addr, "T", 1, h);
  else
    bus1.send(addr, "T", 1, h);
  pjon_packets_tx++;
}

void pjon_send_config_req(uint8_t addr) {
  uint8_t b = (addr <= MAX_DIAG_NODES && remote_nodes[addr].bus_id > 0) ? remote_nodes[addr].bus_id : 1;
  uint8_t h = (b == 2 ? bus2.config : bus1.config) & ~PJON_ACK_REQ_BIT;
  if (b == 2)
    bus2.send(addr, "C", 1, h);
  else
    bus1.send(addr, "C", 1, h);
  pjon_packets_tx++;
}

void pjon_send_config_set(uint8_t addr, uint8_t backlight_pct, uint8_t led_mode) {
  char pkt[3];
  pkt[0] = 'S';
  pkt[1] = (char)backlight_pct;
  pkt[2] = (char)led_mode;
  if (addr == 0) {
    bus1.send(0, pkt, 3, bus1.config & ~PJON_ACK_REQ_BIT);
    bus2.send(0, pkt, 3, bus2.config & ~PJON_ACK_REQ_BIT);
    pjon_packets_tx += 2;
    return;
  }
  uint8_t b = (addr <= MAX_DIAG_NODES && remote_nodes[addr].bus_id > 0) ? remote_nodes[addr].bus_id : 1;
  uint8_t h = (b == 2 ? bus2.config : bus1.config) & ~PJON_ACK_REQ_BIT;
  if (b == 2)
    bus2.send(addr, pkt, 3, h);
  else
    bus1.send(addr, pkt, 3, h);
  pjon_packets_tx++;
}

void pjon_send_night_mode(uint8_t addr, bool enable) {
  char pkt[2] = { 'N', enable ? '1' : '0' };
  if (addr == 0) {
    bus1.send(0, pkt, 2, bus1.config & ~PJON_ACK_REQ_BIT);
    bus2.send(0, pkt, 2, bus2.config & ~PJON_ACK_REQ_BIT);
    pjon_packets_tx += 2;
    return;
  }
  uint8_t b = (addr <= MAX_DIAG_NODES && remote_nodes[addr].bus_id > 0) ? remote_nodes[addr].bus_id : 1;
  uint8_t h = (b == 2 ? bus2.config : bus1.config) & ~PJON_ACK_REQ_BIT;
  if (b == 2)
    bus2.send(addr, pkt, 2, h);
  else
    bus1.send(addr, pkt, 2, h);
  pjon_packets_tx++;
}

void pjon_send_clean_mode(uint8_t addr, bool enable) {
  char pkt[2] = { 'K', enable ? '1' : '0' };
  if (addr == 0) {
    bus1.send(0, pkt, 2, bus1.config & ~PJON_ACK_REQ_BIT);
    bus2.send(0, pkt, 2, bus2.config & ~PJON_ACK_REQ_BIT);
    pjon_packets_tx += 2;
    return;
  }
  uint8_t b = (addr <= MAX_DIAG_NODES && remote_nodes[addr].bus_id > 0) ? remote_nodes[addr].bus_id : 1;
  uint8_t h = (b == 2 ? bus2.config : bus1.config) & ~PJON_ACK_REQ_BIT;
  if (b == 2)
    bus2.send(addr, pkt, 2, h);
  else
    bus1.send(addr, pkt, 2, h);
  pjon_packets_tx++;
}

bool mqtt_publish_tracked(const char *t, const char *p, bool retain = false) {
  web_server_log_tx(t, p);
  mqtt_messages_tx++;
  return client.publish(t, p, retain);
}

void reconnect_non_blocking() {
  unsigned long now = millis();
  if (now - last_reconnect_attempt > 5000) {
    last_reconnect_attempt = now;

    // Topic di stato LWT per Home Assistant
    char status_topic[48];
    snprintf(status_topic, sizeof(status_topic), "%s/status",
             config.mqtt_client_id);

    // Connessione con Last Will and Testament ("offline" se cade la
    // connessione)
    if (client.connect(config.mqtt_client_id, config.mqtt_user,
                       config.mqtt_pass, status_topic, 1, true, "offline")) {
      digitalWrite(led_mqtt, HIGH);

      // Pubblica online con retain
      mqtt_publish_tracked(status_topic, "online", true);

      // Annuncio legacy
      mqtt_publish_tracked("announces", "hello world");

      // Sottoscrizione comandi legacy
      client.subscribe("keypad");
      client.subscribe("intopic");
      client.subscribe("home/pm_gate/set/#");

      // Sottoscrizione comandi Home Assistant per questo gateway
      // Topic: <gateway_id>/node/+/led/+/set, night_mode, clean_mode
      char cmd_topic[56];
      snprintf(cmd_topic, sizeof(cmd_topic), "%s/node/+/led/+/set", config.mqtt_client_id);
      client.subscribe(cmd_topic);
      snprintf(cmd_topic, sizeof(cmd_topic), "%s/node/+/night_mode/set", config.mqtt_client_id);
      client.subscribe(cmd_topic);
      snprintf(cmd_topic, sizeof(cmd_topic), "%s/node/+/clean_mode/set", config.mqtt_client_id);
      client.subscribe(cmd_topic);
      snprintf(cmd_topic, sizeof(cmd_topic), "%s/night_mode/set", config.mqtt_client_id);
      client.subscribe(cmd_topic);
      client.subscribe("homeassistant/status");

      // Pubblica Auto-Discovery Home Assistant
      if (config.ha_discovery) {
        ha_discovery_publish_all();
      }
    } else {
      digitalWrite(led_mqtt, LOW);
    }
  }
}

// Callback ricezione messaggi da Broker MQTT
void callback(char *topic, byte *payload, unsigned int length) {
  mqtt_messages_rx++;
  web_server_log_rx(topic, payload, length);

  // 0. Home Assistant Birth Message (se HA si riavvia, ri-annuncia tutti i nodi e configurazioni)
  if (strcmp(topic, "homeassistant/status") == 0) {
    if (length >= 6 && strncasecmp((char *)payload, "online", 6) == 0) {
      if (config.ha_discovery) {
        ha_discovery_publish_all();
      }
    }
    return;
  }

  // 1. Comando legacy: L<led_num>,<address>,<led_stat> (es: L1,1,1)
  if (length >= 6 && (char)payload[0] == 'L') {
    byte led_num = (char)payload[1] - '0';
    byte address = (char)payload[3] - '0';
    byte led_stat = (char)payload[5] - '0';

    pjon_send_test(address, led_num, led_stat);
    return;
  }

  // 1b. Master Night Mode: <gw_id>/night_mode/set
  char master_nm_topic[48];
  snprintf(master_nm_topic, sizeof(master_nm_topic), "%s/night_mode/set", config.mqtt_client_id);
  if (strcmp(topic, master_nm_topic) == 0) {
    bool is_on = (length > 0 && ((char)payload[0] == '1' || strncasecmp((char *)payload, "ON", 2) == 0));
    master_night_mode = is_on;
    pjon_send_night_mode(0, is_on);
    for (uint8_t i = 1; i <= MAX_DIAG_NODES; i++) {
      if (remote_nodes[i].active) remote_nodes[i].night_mode = is_on;
    }
    char st_topic[48];
    snprintf(st_topic, sizeof(st_topic), "%s/night_mode/state", config.mqtt_client_id);
    client.publish(st_topic, is_on ? "ON" : "OFF", true);
    return;
  }

  // 2. Comando Home Assistant: <gw_id>/node/<node_id>/...
  char prefix_check[40];
  snprintf(prefix_check, sizeof(prefix_check), "%s/node/",
           config.mqtt_client_id);
  if (strncmp(topic, prefix_check, strlen(prefix_check)) == 0) {
    const char *p = topic + strlen(prefix_check);
    int node_id = atoi(p);
    if (node_id > 0 && node_id <= MAX_DIAG_NODES) {
      // 2a. Comando LED: /led/<id>/set
      const char *p_led = strstr(p, "/led/");
      if (p_led) {
        int led_id = atoi(p_led + 5);

        uint8_t stat = 0;
        if (length > 0 && ((char)payload[0] == '1' ||
                           strncasecmp((char *)payload, "ON", 2) == 0)) {
          stat = 1;
        }

        pjon_send_test((uint8_t)node_id, (uint8_t)led_id, stat);

        // Pubblica lo stato di ritorno verso Home Assistant
        char state_topic[56];
        snprintf(state_topic, sizeof(state_topic), "%s/node/%u/led/%u/state",
                 config.mqtt_client_id, node_id, led_id);
        client.publish(state_topic, stat ? "ON" : "OFF", true);
        mqtt_messages_tx++;
        return;
      }

      // 2b. Comando Night Mode: /night_mode/set
      const char *p_nm = strstr(p, "/night_mode/set");
      if (p_nm) {
        bool is_on = (length > 0 && ((char)payload[0] == '1' || strncasecmp((char *)payload, "ON", 2) == 0));
        remote_nodes[node_id].night_mode = is_on;
        pjon_send_night_mode(node_id, is_on);
        char state_topic[56];
        snprintf(state_topic, sizeof(state_topic), "%s/node/%u/night_mode/state", config.mqtt_client_id, node_id);
        client.publish(state_topic, is_on ? "ON" : "OFF", true);
        mqtt_messages_tx++;
        return;
      }

      // 2c. Comando Clean Mode: /clean_mode/set
      const char *p_cm = strstr(p, "/clean_mode/set");
      if (p_cm) {
        bool is_on = (length > 0 && ((char)payload[0] == '1' || strncasecmp((char *)payload, "ON", 2) == 0));
        remote_nodes[node_id].clean_mode = is_on;
        pjon_send_clean_mode(node_id, is_on);
        char state_topic[56];
        snprintf(state_topic, sizeof(state_topic), "%s/node/%u/clean_mode/state", config.mqtt_client_id, node_id);
        client.publish(state_topic, is_on ? "ON" : "OFF", true);
        mqtt_messages_tx++;
        return;
      }
    }
  }
}

// Callback comune ricezione pacchetti da entrambi i bus RS485 PJON (Client ->
// Gateway)
void handle_pjon_packet(uint8_t bus_id, uint8_t *payload, uint16_t length,
                        const PJON_Packet_Info &packet_info) {
  pjon_packets_rx++;
  if (bus_id == 1)
    dbg_rx_b1++;
  else
    dbg_rx_b2++;
  digitalWrite(led_activity, HIGH);

  uint8_t node_id = packet_info.tx.id;
  node_diag_mark_seen(node_id, bus_id);

  // Se e' un pacchetto Keepalive / Heartbeat puro di diagnostica
  if (length == 1 && (payload[0] == 'K' || payload[0] == '?')) {
    dbg_k_rx++;
    digitalWrite(led_activity, LOW);
    return;
  }

  // Risposta Modalita Notte ('N') o Modalita Pulizia ('K')
  if (length >= 2 && payload[0] == 'N') {
    if (node_id >= 1 && node_id <= MAX_DIAG_NODES) {
      remote_nodes[node_id].night_mode = (payload[1] == '1');
      char st_topic[56];
      snprintf(st_topic, sizeof(st_topic), "%s/node/%u/night_mode/state", config.mqtt_client_id, node_id);
      client.publish(st_topic, remote_nodes[node_id].night_mode ? "ON" : "OFF", true);
    }
    digitalWrite(led_activity, LOW);
    return;
  }

  if (length >= 2 && payload[0] == 'K') {
    if (node_id >= 1 && node_id <= MAX_DIAG_NODES) {
      remote_nodes[node_id].clean_mode = (payload[1] == '1');
      char st_topic[56];
      snprintf(st_topic, sizeof(st_topic), "%s/node/%u/clean_mode/state", config.mqtt_client_id, node_id);
      client.publish(st_topic, remote_nodes[node_id].clean_mode ? "ON" : "OFF", true);
    }
    digitalWrite(led_activity, LOW);
    return;
  }

  // Risposta configurazione (C = risposta query, S = risposta salvataggio EEPROM)
  if (length >= 3 && (payload[0] == 'C' || payload[0] == 'S')) {
    if (node_id >= 1 && node_id <= MAX_DIAG_NODES) {
      remote_nodes[node_id].backlight_pct = payload[1];
      remote_nodes[node_id].led_mode = payload[2];
      remote_nodes[node_id].has_config = true;
    }
    digitalWrite(led_activity, LOW);
    return;
  }

  // Pacchetto Temperatura inviato da nodo remoto (es. "T23.5", "23.5", "TN/A", "N/A")
  bool is_na = (length >= 3 && memcmp(payload, "N/A", 3) == 0) ||
               (length >= 4 && memcmp(payload, "TN/A", 4) == 0);
  if (is_na) {
    remote_nodes[node_id].has_temp = true; // Segnato come gestito (assente) per non richiederlo in loop
    remote_nodes[node_id].temp = 0.0f;
    digitalWrite(led_activity, LOW);
    return;
  }

  bool is_temp = (length >= 2 && payload[0] == 'T') ||
                 (length >= 3 && (isdigit(payload[0]) || payload[0] == ' ') && memchr(payload, '.', length));
  if (is_temp) {
    char t_str[12];
    const char *src = (const char *)payload;
    uint8_t rem_len = length;
    if (payload[0] == 'T') {
      src++;
      rem_len--;
    }
    while (rem_len > 0 && *src == ' ') {
      src++;
      rem_len--;
    }
    while (rem_len > 0 && (src[rem_len - 1] == '\0' || src[rem_len - 1] == '\r' || src[rem_len - 1] == '\n' || src[rem_len - 1] == ' ')) {
      rem_len--;
    }
    if (rem_len > sizeof(t_str) - 1) rem_len = sizeof(t_str) - 1;
    memcpy(t_str, src, rem_len);
    t_str[rem_len] = '\0';

    float t_val = atof(t_str);
    node_diag_set_temp(node_id, t_val);

    // 1. Pubblicazione Home Assistant: <gw_id>/node/<node_id>/temperature
    char ha_t_topic[56];
    snprintf_P(ha_t_topic, sizeof(ha_t_topic), PSTR("%s/node/%u/temperature"),
               config.mqtt_client_id, node_id);
    mqtt_publish_tracked(ha_t_topic, t_str);

    // 2. Pubblicazione Legacy: <prefix>/<node_id>/temperature
    char leg_t[48];
    snprintf_P(leg_t, sizeof(leg_t), PSTR("%s/%u/temperature"),
               config.mqtt_prefix, node_id);
    client.publish(leg_t, t_str);

    digitalWrite(led_activity, LOW);
    return;
  }

  // Tasto premuto dal client (payload 'A'..'H')
  if (length >= 1) {
    dbg_btn_rx++;
    uint8_t button_num =
        (payload[0] >= 65 && payload[0] <= 90) ? (payload[0] - 64) : payload[0];

    // 1. Pubblicazione Legacy (scene/<node_id>/<button_num> = "On")
    char leg_topic[32];
    snprintf_P(leg_topic, sizeof(leg_topic), PSTR("%s/%u/%u"),
               config.mqtt_prefix, node_id, button_num);
    mqtt_publish_tracked(leg_topic, "On");

    // 2. Pubblicazione Home Assistant
    // (<gateway_id>/node/<node_id>/btn/<button_num> = "ON")
    char ha_topic[48];
    snprintf_P(ha_topic, sizeof(ha_topic), PSTR("%s/node/%u/btn/%u"),
               config.mqtt_client_id, node_id, button_num);
    mqtt_publish_tracked(ha_topic, "ON");

    // 3. Auto-Discovery automatica al primo click se nodo non ancora registrato
    // (fino a 63 nodi)
    if (config.ha_discovery && node_id > 0 && node_id <= 63) {
      if (!(known_nodes_mask & ((uint64_t)1 << node_id))) {
        known_nodes_mask |= ((uint64_t)1 << node_id);
        ha_discovery_publish_node(node_id);
      }
    }
  }

  delay(5);
  digitalWrite(led_activity, LOW);
}

void receiver_bus1(uint8_t *payload, uint16_t length,
                   const PJON_Packet_Info &packet_info) {
  handle_pjon_packet(1, payload, length, packet_info);
}

void receiver_bus2(uint8_t *payload, uint16_t length,
                   const PJON_Packet_Info &packet_info) {
  handle_pjon_packet(2, payload, length, packet_info);
}

// Gestore errori PJON
void error_handler(uint8_t code, uint16_t data, void *custom_pointer) {
  pjon_packets_err++;
  dbg_last_err_code = code;
  dbg_last_err_data = data;
  if (code == PJON_CONNECTION_LOST) {
    dbg_err_lost++;
    if (data < PJON_MAX_PACKETS) {
      uint8_t tgt = (uint8_t)bus1.packets[data].content[0];
      if (tgt == 0)
        tgt = (uint8_t)bus2.packets[data].content[0];
      if (tgt >= 1 && tgt <= MAX_DIAG_NODES) {
        node_diag_record_attempts(tgt, 20, false);
      }
    }
  } else if (code == PJON_PACKETS_BUFFER_FULL)
    dbg_err_full++;
  else
    dbg_err_other++;
  if (code == PJON_CONNECTION_LOST) {
    digitalWrite(led_activity, LOW);
  } else {
    digitalWrite(led_activity, HIGH);
  }
}

#include <avr/wdt.h>

// Disabilita immediatamente il watchdog timer all'avvio (.init3) per prevenire
// loop di reset infiniti
void wdt_init(void) __attribute__((naked)) __attribute__((section(".init3")));
void wdt_init(void) {
  MCUSR = 0;
  wdt_disable();
}

void setup() {
  MCUSR = 0;
  wdt_disable();

  // Inizializza LED di stato
  pinMode(led_activity, OUTPUT);
  pinMode(led_mqtt, OUTPUT);
  digitalWrite(led_activity, HIGH);
  digitalWrite(led_mqtt, HIGH);
  delay(1000);
  digitalWrite(led_activity, LOW);
  digitalWrite(led_mqtt, LOW);

  // Carica configurazione da EEPROM (o valori di default)
  config_load();

  // Inizializza porta seriale per PJON
  Serial.begin(config.pjon_baud);

  // Inizializza Ethernet W5500 con pin CS configurato
  Ethernet.init(eth_cs_pin);
  IPAddress static_ip(config.ip[0], config.ip[1], config.ip[2], config.ip[3]);
  IPAddress static_dns(config.dns[0], config.dns[1], config.dns[2],
                       config.dns[3]);
  IPAddress static_gw(config.gateway[0], config.gateway[1], config.gateway[2],
                      config.gateway[3]);
  IPAddress static_sub(config.subnet[0], config.subnet[1], config.subnet[2],
                       config.subnet[3]);

  if (config.dhcp) {
    if (Ethernet.begin(config.mac) == 0) {
      // Fallback a IP statico immediato se DHCP fallisce o non risponde
      Ethernet.begin(config.mac, static_ip, static_dns, static_gw, static_sub);
    }
  } else {
    Ethernet.begin(config.mac, static_ip, static_dns, static_gw, static_sub);
  }

  // Inizializza sincronizzazione oraria NTP via Ethernet
  ntp_init();

  // Segnale LED: doppio lampeggio rapido indica rete pronta
  digitalWrite(led_activity, HIGH);
  delay(150);
  digitalWrite(led_activity, LOW);
  delay(150);
  digitalWrite(led_activity, HIGH);
  delay(150);
  digitalWrite(led_activity, LOW);

  // Avvia il Web Server HTTP sulla porta 80
  web_server_init();

  // Configura client MQTT
  client.setServer(config.mqtt_server, config.mqtt_port);
  client.setCallback(callback);

  // Inizializza diagnostica nodi remoti
  node_diag_init();

  // Inizializza sensore di temperatura I2C LM75
  lm75_init();

  // Inizializza porte seriali per entrambi i bus RS485 (UART0 e UART1)
  Serial.begin(config.pjon_baud);
  Serial1.begin(config.pjon_baud);

  // Inizializza maschera nodi noti con quelli preconfigurati (fino a 63 nodi)
  for (uint8_t n = 1; n <= config.ha_nodes_count && n <= 63; n++) {
    known_nodes_mask |= ((uint64_t)1 << n);
  }

  // Configura Bus 1 PJON ThroughSerial su RS485 (U2 su Serial)
  bus1.set_id(config.pjon_id);
  bus1.strategy.set_serial(&traceSerial1);
  bus1.include_sender_info(true);
  bus1.set_acknowledge(true);
  bus1.strategy.set_enable_RS485_pin(rs485_enable_pin_1);
  bus1.set_error(error_handler);
  bus1.set_receiver(receiver_bus1);
  bus1.set_crc_32(false);
  bus1.begin();

  // Configura Bus 2 PJON ThroughSerial su RS485 (U3 su Serial1)
  bus2.set_id(config.pjon_id);
  bus2.strategy.set_serial(&Serial1);
  bus2.include_sender_info(true);
  bus2.set_acknowledge(true);
  bus2.strategy.set_enable_RS485_pin(rs485_enable_pin_2);
  bus2.set_error(error_handler);
  bus2.set_receiver(receiver_bus2);
  bus2.set_crc_32(false);
  bus2.begin();
}

void loop() {
  unsigned long dbg_t0 = micros();
  unsigned long dbg_ts = dbg_t0;
  // Gestione Web Server HTTP di configurazione
  web_server_handle();
  DBG_SEC(0, dbg_ts);

  // Gestione sensore di temperatura I2C LM75
  lm75_loop();
  DBG_SEC(1, dbg_ts);

  // Gestione sincronizzazione oraria NTP
  ntp_update();

  // Gestione MQTT
  if (!client.connected()) {
    reconnect_non_blocking();
  } else {
    client.loop();

    // Telemetria periodica ogni 60 secondi
    unsigned long now = millis();
    if (now - last_telemetry_time >= 60000) {
      last_telemetry_time = now;
      ha_publish_telemetry();
    }

    // Pubblicazione temperatura LM75 se variata o ogni 30 secondi
    static float last_pub_temp = -999.0f;
    static unsigned long last_temp_pub_time = 0;
    if (lm75_is_available()) {
      float cur_t = lm75_get_temp();
      if (now - last_temp_pub_time >= 30000 ||
          fabs(cur_t - last_pub_temp) >= 0.2f) {
        last_temp_pub_time = now;
        last_pub_temp = cur_t;
        char tstr[10];
        dtostrf(cur_t, 4, 1, tstr);

        char t_topic[48];
        snprintf_P(t_topic, sizeof(t_topic), PSTR("%s/temperature"),
                   config.mqtt_client_id);
        mqtt_publish_tracked(t_topic, tstr);

        char leg_t[48];
        snprintf_P(leg_t, sizeof(leg_t), PSTR("%s/temperature"),
                   config.mqtt_prefix);
        client.publish(leg_t, tstr);
      }
    }
  }

  DBG_SEC(2, dbg_ts);
  // Gestione di entrambi i bus PJON (ricezione e trasmissione asincrona se non
  // in OTA)
  if (!ota_is_active()) {
    bus1.update();
    bus1.receive();
    bus2.update();
    bus2.receive();
  }
  DBG_SEC(3, dbg_ts);

  // Ciclo di auto-diagnostica nodi remoti (heartbeat e ping se non in OTA)
  if (!ota_is_active()) {
    node_diag_loop();
  }
  DBG_SEC(4, dbg_ts);

  dbg_loop_last_us = micros() - dbg_t0;
  if (dbg_loop_last_us > dbg_loop_max_us)
    dbg_loop_max_us = dbg_loop_last_us;
}
