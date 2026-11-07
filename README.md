# IO16 — Sistema Domotico RS485 PJON & Gateway Ethernet MQTT

Un ecosistema domotico avanzato, robusto ed espandibile basato sul protocollo **PJON (ThroughSerial half-duplex)** su bus bilanciato **RS485** a 38400 baud, interfacciato a reti IP locali e **Home Assistant** tramite un **Gateway Ethernet MQTT** dedicato.

---

## 🌟 Caratteristiche Principali

### 1. Gateway Ethernet & MQTT (`/gateway`)
- **Microcontrollore**: Microchip ATmega1284P (128 KB Flash, 16 KB SRAM, 16 MHz).
- **Connettività Rete**: Modulo Ethernet SPI Wiznet W5500 con supporto sia per **DHCP** sia per **IP Statico** configurabile.
- **Interfaccia Web Reattiva Integrata**:
  - Diagnostica real-time dello stato del sistema, RAM libera e throughput.
  - Matrice grafica dei nodi (da 1 a 63) con stato Online/Offline e qualità link.
  - Tabella interattiva dei dispositivi con indicazione di bus, pacchetti, errori e latenza.
  - Monitor live del traffico MQTT in tempo reale (ultimi messaggi RX e TX).
  - Test rapido dei LED dei nodi RS485 direttamente dall'interfaccia.
  - Configurazione dinamica in EEPROM dei parametri di retroilluminazione e modalità LED (per singolo nodo o broadcast globale).
  - Menu a tendina compatti e campi IP opacizzati/protetti quando il DHCP è attivo.
- **Integrazione Home Assistant**:
  - **MQTT Discovery automatico**: genera automaticamente entità Luce (LED) e Sensori/Eventi (Pulsanti, Pressioni Lunghe, Doppi Click, Temperatura).
- **Interrogazione Ciclica Sensori**:
  - Polling round-robin rilassato della temperatura LM75 dei nodi client (1 richiesta/s, intervallo 60 s, zero carico sul bus).
- **Doppio Bus RS485**:
  - Supporto per due bus RS485 fisicamente separati su UART0 e UART1 hardware.

---

### 2. Nodi Tastiera IO16 (`/nodes_io16`)
- **Microcontrollori Supportati**:
  - **ATmega1284P** (Tastiera IO16 Versione A)
  - **ATmega324PB** (Tastiera IO16 Versione B)
- **Funzionalità Nodo**:
  - Gestione di 8 pulsanti con de-bounce e rilevazione di:
    - Click singolo
    - Pressione lunga (*Long Press*)
    - Doppio click (*Double Click*)
  - Controllo di 8 LED bicolore/monocromatici con modalità operative selezionabili via EEPROM:
    - `0`: Solo Remoto (comandato da MQTT / Home Assistant)
    - `1`: Temporizzato 5s (feedback visivo locale automatico)
    - `2`: Toggle Bistabile (ON/OFF alternato)
    - `3`: Momentaneo (acceso solo durante la pressione)
  - Controllo luminosità retroilluminazione tasti in PWM (0-100%) memorizzata in EEPROM.
  - Lettura temperatura con sensore digitale I2C LM75.
  - **Architettura Silenziosa (Master-Slave)**: i nodi trasmettono solo su evento tasto o se interrogati dal Gateway, garantendo un bus sempre pulito per gli aggiornamenti firmware.

---

### 3. Sistema di Aggiornamento Firmware Web OTA (`/firmware`)
- **Bootloader Optiboot RS485 Personalizzato**:
  - Bootloader compatto (512 parole) conforme a **STK500v1** su UART seriale half-duplex a 38400 baud.
  - Gestione automatica del pin `DIR` (Driver Enable RS485) per commutazione RX/TX senza collisioni.
  - Supporto software watchdog e timeout di sicurezza (1 secondo).
- **Flasher OTA Web Integrato nel Gateway**:
  - Pagina web dedicata (`/ota`) accessibile da qualsiasi browser.
  - **Aggiornamento Singolo**: selezione del nodo specifico e caricamento del file `.hex`.
  - **Aggiornamento Massivo (Batch Mass OTA)**: con un solo click e un file binario universale, il Gateway riprogramma in sequenza automatica tutti i nodi collegati sul bus.
  - **Firmware Universale**: script `create_universal_fw.py` che genera un unico file Intel HEX contenente il codice compilato per entrambi i target MCU (`ATmega1284P` e `ATmega324PB`), selezionato dinamicamente in base alla firma hardware letta dal bootloader.

---

## 📁 Struttura della Repository

```
IO16/
├── gateway/                    # Progetto PlatformIO per Gateway MQTT (ATmega1284P)
│   ├── include/                # Header file di configurazione, OTA, WebServer, HA
│   ├── src/                    # Codice sorgente Gateway (io16_mqtt_gateway_dhcp_v2.ino, web_server.cpp, ecc.)
│   ├── tools/                  # Script di test e utility
│   └── platformio.ini          # Configurazione PlatformIO Gateway
│
├── nodes_io16/                 # Progetto PlatformIO per Tastiere IO16
│   ├── bootloader_rs485/       # Sorgenti e binari Optiboot RS485 (ATmega1284P & ATmega324PB)
│   ├── src/                    # Codice sorgente slave (io16rtx_new.ino)
│   ├── create_universal_fw.py  # Script Python unione firmware universale multi-MCU
│   ├── ota_flash_helper.py     # Script helper per test OTA
│   └── platformio.ini          # Configurazione ambienti atmega1284p e atmega324pb
│
├── firmware/                   # Binari compilati pronti all'uso
│   └── firmware_io16_universal.hex
│
├── docs/                       # Documentazione tecnica e manuali
│   ├── MANUALE_UTENTE.md       # Manuale completo d'uso e configurazione
│   └── Manuale_Utente_Gateway_IO16.pdf
│
├── .gitignore                  # Esclusioni build PlatformIO, Python cache e temporanei
└── README.md                   # Questa documentazione
```

---

## 🛠️ Requisiti & Compilazione

### Requisiti Software
- [Visual Studio Code](https://code.visualstudio.com/) con estensione **PlatformIO IDE** (oppure **PlatformIO Core CLI**).
- Python 3.8+ per gli script di build e unione firmware.

### Compilazione del Gateway
```bash
cd gateway
pio run -e atmega1284p
```

### Compilazione dei Nodi Tastiera
```bash
cd nodes_io16
# Per compilare ambiente ATmega1284P
pio run -e atmega1284p

# Per compilare ambiente ATmega324PB
pio run -e atmega324pb

# Per generare il firmware universale per entrambi i target
python create_universal_fw.py
```

---

## 🌐 Connessioni Hardware & Bus RS485

| Segnale RS485 | Descrizione | Note |
|:---:|:---:|:---:|
| **A / D+** | Linea differenziale non invertente | Bus bilanciato twistato |
| **B / D-** | Linea differenziale invertente | Bus bilanciato twistato |
| **GND** | Massa di riferimento comune | Essenziale per evitare loop di massa |
| **VCC (+12V/+24V)** | Alimentazione bus | Regolata localmente sui nodi a 5V/3.3V |

- **Baud Rate**: `38400 baud`, 8 bit dati, nessuna parità, 1 bit di stop (8-N-1).
- **Terminazione**: Resistore di terminazione da 120 Ω presente all'inizio del bus (Gateway) e all'estremità finale del cavo.

---

## 📡 Argomenti MQTT (Topic Reference)

Il Gateway pubblica e sottoscrive i seguenti topic con prefisso configurabile (default `pjon` o `pjon_gw_<id>`):

| Topic | Tipo | Payload | Descrizione |
|---|:---:|---|---|
| `<prefix>/status` | Pub | `online` / `offline` | Stato di connessione del Gateway (con LWT) |
| `<prefix>/temperature` | Pub | `25.4` | Temperatura LM75 integrata sul Gateway (°C) |
| `<prefix>/node/<ID>/status` | Pub | `online` / `offline` | Stato di raggiungibilità del nodo client |
| `<prefix>/node/<ID>/temp` | Pub | `24.1` | Temperatura letta dal nodo client via LM75 |
| `<prefix>/node/<ID>/btn/<1..8>` | Pub | `ON` | Pressione breve pulsante |
| `<prefix>/node/<ID>/btn/<9..16>` | Pub | `LONG` | Pressione prolungata (>1s) |
| `<prefix>/node/<ID>/btn/<17..24>` | Pub | `DOUBLE` | Doppio click |
| `<prefix>/node/<ID>/led/<1..8>/set` | Sub | `ON` / `OFF` | Comando accensione/spegnimento LED |
| `<prefix>/node/<ID>/led/<1..8>` | Pub | `ON` / `OFF` | Stato feedback attuale del LED |

---

## 🏆 Ringraziamenti & Attribuzioni (Credits & Acknowledgements)

Questo progetto utilizza ed è basato sul framework e protocollo di rete open-source **PJON® (Pjon Network Protocol)**:
- **Autore e Ideatore di PJON**: **Giovanni Blu Mitolo** ([@gioblu](https://github.com/gioblu))
- **Repository Ufficiale PJON**: [https://github.com/gioblu/PJON](https://github.com/gioblu/PJON)
- **Sito Web Ufficiale**: [https://www.pjon.org](https://www.pjon.org)
- **Licenza PJON**: Rilasciato sotto licenza [Apache License 2.0](https://www.apache.org/licenses/LICENSE-2.0).

Un ringraziamento speciale a Giovanni Blu Mitolo e a tutta la community di PJON per aver sviluppato un protocollo di comunicazione multi-master, multi-media e fault-tolerant eccezionale, che rende possibili architetture robuste, reattive e scalabili su bus fisici come RS485.

---

## 📜 Licenza
Progetto distribuito per automazione e domotica ad alta affidabilità. Libero per utilizzi personali e commerciali con citazione dell'autore.  
PJON® è un marchio e software protetto da copyright di Giovanni Blu Mitolo.

