# Standard Aziendale Telemetria IoT — Autodomain

[![Standard: Autodomain IoT v1.0](https://img.shields.io/badge/Standard-Autodomain%20IoT%20v1.0-blue.svg)]()
[![Platform: Raspberry Pi Hub](https://img.shields.io/badge/Hub-Raspberry%20Pi%203B-red.svg)]()
[![Endpoint: HTTP POST :8088](https://img.shields.io/badge/Endpoint-HTTP%20%3A8088-green.svg)]()

Questo documento definisce lo **standard unico di telemetria e diagnostica sul campo** per tutti i dispositivi dell'ecosistema **Autodomain**:
1. **Termostati Smart ESP32** (CYD 2.8″ TFT & AZ-Touch 2.4″ MOD)
2. **Interfacce Tastiera & Relè IO16** (Nodi su bus RS485 PJON ThroughSerial)
3. **Gateway Ethernet & MQTT ATmega1284P** (Bridge W5500 tra Bus RS485 e Rete IP/MQTT)

---

## 🏛️ Architettura Generale

```
                    ┌─────────────────────────┐
                    │     Server Telemetria   │
                    │   Raspberry Pi (Debian) │
                    │       192.168.1.68      │
                    │   Porta HTTP :8088      │
                    └────────────▲────────────┘
                                 │
           ┌─────────────────────┼─────────────────────┐
           │ HTTP POST           │ HTTP POST           │ HTTP POST / Proxy
┌──────────┴──────────┐ ┌────────┴──────────┐ ┌────────┴──────────┐
│  Termostati Smart   │ │   Gateway Ethernet │ │  Nodi Tastiera   │
│     ESP32 (WiFi)    │ │ MQTT (ATmega1284P)│ │     IO16 RS485   │
│   CYD & AZ-Touch    │ │   + Wiznet W5500  │ │   (via Gateway)   │
└─────────────────────┘ └───────────────────┘ └───────────────────┘
```

- **Server di Raccolta**: Raspberry Pi a `http://192.168.1.68:8088/api/telemetry`
- **Dashboard Web Unificata**: [http://192.168.1.68:8088](http://192.168.1.68:8088)
- **Database**: SQLite WAL mode permanente (`telemetry.db`).
- **Politica di Conservazione**: **Nessun dispositivo viene mai cancellato automaticamente**. Anche se un dispositivo è spento o offline per mesi, rimane censito con lo storico e lo stato `Offline`. Ogni riga dispone di un pulsante dedicato **Elimina** per la rimozione manuale singola.

---

## 📦 1. Specifiche Payload JSON

Tutti i dispositivi inviano pacchetti JSON con metodo **`POST`** all'endpoint:
`http://192.168.1.68:8088/api/telemetry`

Header HTTP richiesti:
```http
POST /api/telemetry HTTP/1.1
Host: 192.168.1.68:8088
Content-Type: application/json
Connection: close
Content-Length: <lunghezza_payload>
```

---

### A. Termostati Smart ESP32 (`thermostat`)

Inviato periodicamente ogni **15 minuti** (e 5 secondi dopo l'avvio).

```json
{
  "device_type": "thermostat",
  "mac": "24:6F:28:B5:0C:98",
  "model": "az_touch",
  "version": "2.5.0",
  "temp": 21.4,
  "target_temp": 22.0,
  "humidity": 55.0,
  "heating": 1,
  "mode": 1,
  "rssi": -62,
  "free_heap": 182400,
  "uptime_s": 84200
}
```

| Campo | Tipo | Descrizione |
| :--- | :---: | :--- |
| `device_type` | `string` | `"thermostat"` |
| `mac` | `string` | Indirizzo MAC WiFi univoco (`XX:XX:XX:XX:XX:XX`) |
| `model` | `string` | `"cyd"` (CYD 2.8″) oppure `"az_touch"` (AZ-Touch 2.4″) |
| `version` | `string` | Versione firmware (es. `"2.5.0"`) |
| `temp` | `float` | Temperatura ambiente rilevata (°C) |
| `target_temp` | `float` | Setpoint impostato (°C) |
| `humidity` | `float` | Umidità relativa (%) |
| `heating` | `int` | `1` se relè caldaia acceso, `0` se spento |
| `mode` | `int` | `0` = Manuale, `1` = Standby, `2` = Crono |
| `rssi` | `int` | Segnale WiFi in dBm |
| `free_heap` | `int` | RAM libera in byte |
| `uptime_s` | `int` | Secondi di funzionamento dall'avvio |

---

### B. Interfacce Tastiera & Relè IO16 (`io16`)

Inviato dal Gateway per conto di ciascun nodo IO16 rilevato sul bus RS485 PJON (oppure via Ethernet se la scheda dispone di modulo di rete).

```json
{
  "device_type": "io16",
  "node_id": 4,
  "gateway_id": "00:08:DC:12:34:56",
  "model": "IO16 Keyboard vA",
  "version": "2.1-beta",
  "bus": 0,
  "inputs": 5,
  "outputs": 1,
  "temp_lm75": 23.2,
  "backlight": 80,
  "packets_rx": 1420,
  "packets_tx": 1418,
  "errors": 2,
  "uptime_s": 95000
}
```

| Campo | Tipo | Descrizione |
| :--- | :---: | :--- |
| `device_type` | `string` | `"io16"` |
| `node_id` | `int` | ID logico del nodo sul bus PJON (`1` .. `63`) |
| `gateway_id` | `string` | MAC address o identificativo del Gateway RS485 Master |
| `model` | `string` | `"IO16 Keyboard vA"` (ATmega1284P) / `"IO16 Keyboard vB"` (ATmega324PB) |
| `version` | `string` | Versione firmware del nodo |
| `bus` | `int` | Indice bus (`0` per UART0, `1` per UART1) |
| `inputs` | `int` | Maschera 8/16 bit stato pulsanti fisici premuti (bit 0..7) |
| `outputs` | `int` | Maschera 8/16 bit stato LED/Relè accesi (bit 0..7) |
| `temp_lm75` | `float` | Temperatura sensore I2C LM75 (°C) |
| `backlight` | `int` | Percentuale retroilluminazione PWM (`0` .. `100%`) |
| `packets_rx` | `int` | Pacchetti PJON ricevuti dal nodo |
| `packets_tx` | `int` | Pacchetti PJON trasmessi con successo |
| `errors` | `int` | Conteggio errori / ritrasmissioni RS485 |

---

### C. Gateway Ethernet & MQTT ATmega1284P (`gateway`)

Inviato periodicamente dal Gateway via Ethernet Wiznet W5500.

```json
{
  "device_type": "gateway",
  "mac": "00:08:DC:12:34:56",
  "model": "ATmega1284P MQTT Gateway",
  "version": "2.1-beta",
  "ip": "192.168.1.150",
  "mqtt_connected": true,
  "mqtt_broker": "192.168.1.68:1883",
  "active_nodes": 6,
  "throughput": "4.2 pkt/s",
  "free_ram": 11420,
  "uptime_s": 128400
}
```

| Campo | Tipo | Descrizione |
| :--- | :---: | :--- |
| `device_type` | `string` | `"gateway"` |
| `mac` | `string` | Indirizzo MAC hardware del modulo W5500 |
| `model` | `string` | `"ATmega1284P MQTT Gateway"` |
| `version` | `string` | Versione firmware del Gateway |
| `ip` | `string` | Indirizzo IP assegnato (DHCP o Statico) |
| `mqtt_connected` | `bool` | `true` se il client MQTT è connesso al broker |
| `mqtt_broker` | `string` | Indirizzo IP e porta del broker Mosquitto |
| `active_nodes` | `int` | Numero totale nodi IO16 attualmente attivi sulla matrice |
| `throughput` | `string` | Frequenza pacchetti o carico bus RS485 |
| `free_ram` | `int` | Memoria SRAM libera in byte (su 16 KB totali) |
| `uptime_s` | `int` | Secondi di attività dall'accensione |

---

## 💻 2. Codice di Esempio di Implementazione

### Esempio per ATmega1284P Gateway (Ethernet W5500)
```cpp
#include <Ethernet.h>
#include <ArduinoJson.h>

const char* telemetry_host = "192.168.1.68";
const int   telemetry_port = 8088;

void send_gateway_telemetry() {
  EthernetClient client;
  if (!client.connect(telemetry_host, telemetry_port)) {
    return;
  }

  StaticJsonDocument<256> doc;
  doc["device_type"] = "gateway";
  doc["mac"] = "00:08:DC:12:34:56";
  doc["model"] = "ATmega1284P MQTT Gateway";
  doc["version"] = "2.1-beta";
  doc["mqtt_connected"] = mqttClient.connected();
  doc["mqtt_broker"] = "192.168.1.68:1883";
  doc["active_nodes"] = get_online_nodes_count();
  doc["free_ram"] = freeRam();
  doc["uptime_s"] = millis() / 1000UL;

  String payload;
  serializeJson(doc, payload);

  client.print("POST /api/telemetry HTTP/1.1\r\n");
  client.printf("Host: %s:%d\r\n", telemetry_host, telemetry_port);
  client.print("Content-Type: application/json\r\n");
  client.print("Connection: close\r\n");
  client.printf("Content-Length: %u\r\n\r\n", payload.length());
  client.print(payload);
  client.stop();
}
```

---

## 📊 3. Struttura della Dashboard Web (`:8088`)

La dashboard presenta tre sezioni indipendenti:
1. **KPI Header Globali**: riepilogo istantaneo di termostati online, nodi IO16 attivi, gateway e totale flotta.
2. **Riquadro Termostati Smart ESP32**: tabella termostati con grafici, temperatura, caldaia, segnale WiFi, storico e tasto Elimina.
3. **Riquadro Interfacce IO16**: tabella nodi PJON con ID (#1..#63), visualizzatore grafico bit a bit degli 8 ingressi e delle 8 uscite, temperatura LM75 e PWM.
4. **Riquadro Gateway MQTT ATmega1284P**: stato connessione al broker MQTT, nodi periferici gestiti, throughput e memoria libera.
