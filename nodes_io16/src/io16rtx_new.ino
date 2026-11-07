/* PJON RS485 Client IO16 - ATmega324PB / ATmega1284P */
/* Hardware verificato al tester:
 * - 8 LED: Porta PA (Pin 24..31), catodo comune a GND, livello logico 1 (HIGH).
 * - Timer2 Hardware Interrupt per PWM Software a 625 Hz su PORTA (nessun hardware esterno necessario).
 * - 8 Pulsanti: Porta PB (Pin 7..0: PB7=P1, PB6=P2, PB5=P3, PB4=P4, PB3=P5, PB2=P6, PB1=P7, PB0=P8).
 * - DIP 1..5: Indirizzo PJON (Pin 23, 22, 21, 20, 19).
 * - DIP 6: PC2 (Pin 18) -> Feedback locale ON/OFF (LOW = feedback locale immediato, HIGH = solo remoto).
 * - DIP 7: PD7 (Pin 15) -> Retroilluminazione: Piena (OFF/HIGH) vs PWM Soffusa ~12.5% (ON/LOW).
 * - DIP 8: PD5 (Pin 13) -> Retroilluminazione Standby Abilitata (ON/LOW) vs Disabilitata (OFF/HIGH).
 * - LED RX diagnostico: PD4 (Pin 12).
 * - RS485 DE: PD2 (Pin 10).
 * - I2C LM75: PC0 (SCL / Pin 16), PC1 (SDA / Pin 17).
 */

#define PJON_INCLUDE_ASYNC_ACK true
#define TS_RESPONSE_TIME_OUT 10000
#define TS_BACK_OFF_DEGREE 4
#define TS_MAX_ATTEMPTS   20

#ifndef PJON_IO_WRITE
  #define PJON_IO_WRITE(P, V) digitalWrite(P, V)
  #define PJON_IO_MODE(P, V) pinMode(P, V)
  #define PJON_IO_READ(P) digitalRead(P)
#endif

#include <avr/wdt.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <Wire.h>
#include <EEPROM.h>
#include <PJON.h>
#include <PJONThroughSerial.h>
#include <PinButton.h>

// Disabilita immediatamente il watchdog timer all'avvio (.init3) per prevenire loop di reset infiniti
void wdt_init(void) __attribute__((used, naked, section(".init3")));
void wdt_init(void) {
  MCUSR = 0;
  wdt_disable();
}

// Parametri EEPROM per Retroilluminazione e Modalità LED (elimina dip switch 6, 7, 8)
#define EEPROM_MAGIC 0xA5
#define EEPROM_ADDR_MAGIC 0
#define EEPROM_ADDR_BACKLIGHT 1
#define EEPROM_ADDR_MODE 2

// Configurazioni caricate da EEPROM:
// - config_backlight_pct: 0..100% (default 12%)
// - config_led_mode:
//     0 = Solo Remoto (nessun feedback locale, solo comandi da Gateway/HA)
//     1 = Temporizzato 5s (feedback locale 5s alla pressione)
//     2 = Toggle Bistabile Locale (inverte stato ON/OFF ad ogni pressione)
//     3 = Momentaneo (acceso mentre il tasto è premuto)
uint8_t config_backlight_pct = 12;
uint8_t config_led_mode = 1;

bool led_toggle_state[8] = {false};

void save_eeprom_config() {
  EEPROM.update(EEPROM_ADDR_MAGIC, EEPROM_MAGIC);
  EEPROM.update(EEPROM_ADDR_BACKLIGHT, config_backlight_pct);
  EEPROM.update(EEPROM_ADDR_MODE, config_led_mode);
}

void load_eeprom_config() {
  if (EEPROM.read(EEPROM_ADDR_MAGIC) == EEPROM_MAGIC) {
    config_backlight_pct = EEPROM.read(EEPROM_ADDR_BACKLIGHT);
    if (config_backlight_pct > 100) config_backlight_pct = 12;
    config_led_mode = EEPROM.read(EEPROM_ADDR_MODE);
    if (config_led_mode > 3) config_led_mode = 1;
  } else {
    config_backlight_pct = 12;
    config_led_mode = 1;
    save_eeprom_config();
  }
}

uint8_t get_standby_duty() {
  if (config_backlight_pct == 0) return 0;
  uint16_t d = ((uint16_t)config_backlight_pct * 16 + 50) / 100;
  if (d < 1) d = 1;
  if (d > 16) d = 16;
  return (uint8_t)d;
}

// Pin LED RX e Gateway
const int led_rx = 12;
const int gw_address = 254;
const unsigned long led_auto_off_interval = 5000; // 5 secondi per autospegnimento locale

// Duty cycle per ciascun LED (0..16): 0 = spento, 2 = soffuso (~12.5%), 16 = pieno 100%
volatile uint8_t led_duty[8] = {0};

// ISR Timer2: Generatore PWM hardware a 625 Hz su PORTA (PA0..PA7)
ISR(TIMER2_COMPA_vect) {
  static uint8_t pwm_phase = 0;
  pwm_phase = (pwm_phase + 1) & 0x0F; // 0..15

  uint8_t out_port = 0;
  for (uint8_t i = 0; i < 8; i++) {
    if (led_duty[i] > pwm_phase) {
      out_port |= (1 << i);
    }
  }
  PORTA = out_port;
}

void init_timer2_pwm() {
  // Configurazione Timer2 in modalità CTC, Prescaler 64
  TCCR2A = (1 << WGM21);
  TCCR2B = (1 << CS22);
  OCR2A = 24;              // 16MHz / (64 * 25) = 10,000 Hz interruzioni -> 625 Hz frequenza PWM
  TIMSK2 |= (1 << OCIE2A); // Abilita interrupt Compare Match A
}

// Configurazione pulsanti: debounce 20ms, singleClickDelay 150ms, longClickDelay 450ms
const MultiButtonConfig btn_config = { 20, 150, 450 };

PinButton buttons[8] = {
  PinButton(7, &btn_config), // P1 (PB7)
  PinButton(6, &btn_config), // P2 (PB6)
  PinButton(5, &btn_config), // P3 (PB5)
  PinButton(4, &btn_config), // P4 (PB4)
  PinButton(3, &btn_config), // P5 (PB3)
  PinButton(2, &btn_config), // P6 (PB2)
  PinButton(1, &btn_config), // P7 (PB1)
  PinButton(0, &btn_config)  // P8 (PB0)
};

int id = 0;
unsigned long led_timer[8] = {0};
bool led_remote_state[8] = {false};

unsigned long last_pjon_rx_time = 0;
unsigned long last_diag_blink = 0;

PJON<ThroughSerial> bus;

// Sensore di Temperatura I2C LM75
static uint8_t lm75_addr = 0x48;
static bool lm75_present = false;
static float lm75_temperature = 0.0f;
static float last_reported_temp = -999.0f;
static unsigned long last_lm75_read = 0;
static unsigned long last_temp_report = 0;

static bool lm75_read_internal() {
  Wire.beginTransmission(lm75_addr);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) {
    lm75_present = false;
    return false;
  }
  if (Wire.requestFrom(lm75_addr, (uint8_t)2) == 2) {
    int8_t msb = Wire.read();
    uint8_t lsb = Wire.read();
    int16_t raw = ((int16_t)msb << 8) | lsb;
    lm75_temperature = (float)(raw >> 5) * 0.125f;
    lm75_present = true;
    return true;
  }
  return false;
}

void lm75_init() {
  lm75_present = false;
  // Test preliminare: attiva pull-up interni sui pin I2C (PC0=SCL / Pin 16, PC1=SDA / Pin 17)
  pinMode(16, INPUT_PULLUP);
  pinMode(17, INPUT_PULLUP);
  delayMicroseconds(50);
  if (digitalRead(16) == LOW || digitalRead(17) == LOW) {
    // Linee I2C assenti o a massa: nessun sensore LM75
    return;
  }

  Wire.begin();
#if defined(WIRE_TIMEOUT)
  Wire.setWireTimeout(10000, true);
#endif
  for (uint8_t a = 0x48; a <= 0x4F; a++) {
    wdt_reset();
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      lm75_addr = a;
      lm75_present = true;
      break;
    }
  }
  if (lm75_present) {
    lm75_read_internal();
  }
}

void lm75_loop() {
  unsigned long now = millis();
  // Lettura locale del sensore ogni 10 secondi (la lettura viene poi inviata al Gateway su richiesta 'T')
  if (now - last_lm75_read >= 10000) {
    last_lm75_read = now;
    if (lm75_present) {
      lm75_read_internal();
    }
  }
}

void update_led_states() {
  uint8_t standby_duty = get_standby_duty();
  unsigned long now = millis();

  for (uint8_t i = 0; i < 8; i++) {
    bool active = false;

    if (config_led_mode == 0) {
      // 0: Solo Remoto
      active = led_remote_state[i];
    } else if (config_led_mode == 1) {
      // 1: Temporizzato 5s
      bool local_on = false;
      if (led_timer[i] > 0) {
        if (now - led_timer[i] < led_auto_off_interval) {
          local_on = true;
        } else {
          led_timer[i] = 0;
        }
      }
      active = local_on || led_remote_state[i];
    } else if (config_led_mode == 2) {
      // 2: Toggle Bistabile Locale
      active = led_toggle_state[i] || led_remote_state[i];
    } else if (config_led_mode == 3) {
      // 3: Momentaneo (attivo mentre il pulsante è premuto fisicamente)
      active = (digitalRead(7 - i) == LOW) || led_remote_state[i];
    }

    led_duty[i] = active ? 16 : standby_duty;
  }
}

int read_id() {
  int dev_id = 0;
  // DIP 1..6 per indirizzamento completo 1..63 (DIP 7 e 8 eliminati)
  pinMode(23, INPUT_PULLUP);
  pinMode(22, INPUT_PULLUP);
  pinMode(21, INPUT_PULLUP);
  pinMode(20, INPUT_PULLUP);
  pinMode(19, INPUT_PULLUP);
  pinMode(18, INPUT_PULLUP); // DIP 6 (PC2)

  if (digitalRead(23) == LOW) dev_id += 1;
  if (digitalRead(22) == LOW) dev_id += 2;
  if (digitalRead(21) == LOW) dev_id += 4;
  if (digitalRead(20) == LOW) dev_id += 8;
  if (digitalRead(19) == LOW) dev_id += 16;
  if (digitalRead(18) == LOW) dev_id += 32;

  // Lampeggio diagnostico LED RX pari all'ID impostato
  if (dev_id > 0) {
    for (int i = 1; i <= dev_id; i++) {
      wdt_reset();
      digitalWrite(led_rx, HIGH);
      delay(120);
      digitalWrite(led_rx, LOW);
      delay(250);
    }
  }
  return dev_id;
}

void receiver_function(uint8_t *payload, uint16_t length, const PJON_Packet_Info &packet_info) {
  last_pjon_rx_time = millis();
  digitalWrite(led_rx, HIGH);

  // 1. Keepalive ping dal Gateway
  if (length >= 1 && payload[0] == '?') {
    bus.set_acknowledge(false);
    bus.send(gw_address, "K", 1);
    digitalWrite(led_rx, LOW);
    return;
  }

  // 2. Richiesta lettura temperatura sensore LM75 ('T' o 't')
  if (length >= 1 && (payload[0] == 'T' || payload[0] == 't')) {
    char tbuf[12];
    tbuf[0] = 'T';
    if (lm75_present) {
      dtostrf(lm75_temperature, 4, 1, tbuf + 1);
    } else {
      strcpy(tbuf + 1, "N/A");
    }
    bus.set_acknowledge(false);
    bus.send(gw_address, tbuf, strlen(tbuf));
    digitalWrite(led_rx, LOW);
    return;
  }

  // 3. Comando di Reboot in Bootloader RS485 per aggiornamento OTA ('U' o 'u')
  if (length >= 1 && (payload[0] == 'U' || payload[0] == 'u')) {
    // Accende tutti i LED della pulsantiera e il LED RX fisso prima del reboot
    PORTA = 0xFF;
    digitalWrite(led_rx, HIGH);

    // Scrive la Magic Key 0xB007 in RAM per far avviare il bootloader
    volatile uint16_t *boot_magic = (volatile uint16_t *)(RAMEND - 1);
    *boot_magic = 0xB007;

    cli();
    wdt_enable(WDTO_15MS);
    while (1);
  }

  // 3. Comando Bitmask: 'M' <1 byte maschera 8-bit> per aggiornamento simultaneo di tutti i LED
  if (length >= 2 && payload[0] == 'M') {
    uint8_t mask = payload[1];
    for (uint8_t i = 0; i < 8; i++) {
      led_remote_state[i] = ((mask & (1 << i)) != 0);
      if (!led_remote_state[i]) led_timer[i] = 0;
    }
    update_led_states();
    digitalWrite(led_rx, LOW);
    return;
  }

  // 4. Lettura configurazione Backlight e Modo LED ('C' o 'c')
  if (length >= 1 && (payload[0] == 'C' || payload[0] == 'c')) {
    char c_msg[3];
    c_msg[0] = 'C';
    c_msg[1] = (char)config_backlight_pct;
    c_msg[2] = (char)config_led_mode;
    bus.set_acknowledge(false);
    bus.send(gw_address, c_msg, 3);
    digitalWrite(led_rx, LOW);
    return;
  }

  // 5. Scrittura configurazione ('S' o 's') -> 'S' <backlight_pct> <led_mode>
  if (length >= 3 && (payload[0] == 'S' || payload[0] == 's')) {
    config_backlight_pct = payload[1];
    if (config_backlight_pct > 100) config_backlight_pct = 100;
    config_led_mode = payload[2];
    if (config_led_mode > 3) config_led_mode = 1;
    save_eeprom_config();
    update_led_states();

    char s_msg[3];
    s_msg[0] = 'S';
    s_msg[1] = (char)config_backlight_pct;
    s_msg[2] = (char)config_led_mode;
    bus.set_acknowledge(false);
    bus.send(gw_address, s_msg, 3);
    digitalWrite(led_rx, LOW);
    return;
  }

  // 6. Comando Standard: payload[0] = led ('0'..'8'), payload[1] = stato ('0'..'1')
  if (length >= 2) {
    uint8_t l = payload[0];
    if (l >= '0' && l <= '8') l -= '0';
    uint8_t s = payload[1];
    if (s >= '0' && s <= '1') s -= '0';

    if (l == 0) {
      for (uint8_t i = 0; i < 8; i++) {
        led_remote_state[i] = (s == 1);
        if (s == 0) led_timer[i] = 0;
      }
    } else if (l >= 1 && l <= 8) {
      led_remote_state[l - 1] = (s == 1);
      if (s == 0) led_timer[l - 1] = 0;
    }
    update_led_states();
  }

  delay(5);
  digitalWrite(led_rx, LOW);
}

void error_handler(uint8_t code, uint16_t data, void *custom_pointer) {}

void send_button_event(char btn_code, uint8_t led_idx, bool long_press = false) {
  digitalWrite(led_rx, HIGH);
  bus.set_acknowledge(false);
  char msg[2] = { btn_code, '\0' };
  bus.send(gw_address, msg, 1);

  if (config_led_mode == 1) {
    // Mode 1: Feedback locale temporizzato (5s)
    led_timer[led_idx] = millis();
    if (long_press) {
      led_duty[led_idx] = 0; // Breve stacco visivo per conferma long press
      delay(50);
    }
    led_duty[led_idx] = 16;
  } else if (config_led_mode == 2) {
    // Mode 2: Toggle Bistabile Locale
    led_toggle_state[led_idx] = !led_toggle_state[led_idx];
    led_duty[led_idx] = led_toggle_state[led_idx] ? 16 : get_standby_duty();
  }
  // In Mode 0 (Solo Remoto) non si accende localmente, attende comando da Gateway/HA.
  // In Mode 3 (Momentaneo) la gestione è dinamica su buttons[i].isPressed() in update_led_states().

  digitalWrite(led_rx, LOW);
}

void check_rs485_diagnostics() {
  unsigned long now = millis();
  // Se non si ricevono pacchetti o ping dal Gateway per oltre 60s, segnala con doppio flash
  if (last_pjon_rx_time > 0 && (now - last_pjon_rx_time > 60000)) {
    if (now - last_diag_blink >= 3000) {
      last_diag_blink = now;
      digitalWrite(led_rx, HIGH);
      delay(30);
      digitalWrite(led_rx, LOW);
      delay(80);
      digitalWrite(led_rx, HIGH);
      delay(30);
      digitalWrite(led_rx, LOW);
    }
  }
}

void setup() {
  MCUSR = 0;
  wdt_disable();

  pinMode(led_rx, OUTPUT);
  digitalWrite(led_rx, LOW);

  // Inizializza Porta PA (PA0..PA7 = Pin 24..31) come OUTPUT
  DDRA = 0xFF;
  PORTA = 0x00;

  // Carica parametri salvati in EEPROM (Backlight e Modalità LED)
  load_eeprom_config();

  // Attiva Watchdog hardware (timeout 2 secondi) per proteggere l'intero avvio
  wdt_enable(WDTO_2S);

  // Inizializza Timer2 per PWM software ad alta frequenza (625 Hz)
  init_timer2_pwm();

  // --- LAMP TEST ALL'AVVIO: Tutti gli 8 LED a piena luminosità per 1 secondo ---
  for (uint8_t i = 0; i < 8; i++) led_duty[i] = 16;
  for (uint8_t k = 0; k < 10; k++) { delay(100); wdt_reset(); }
  for (uint8_t i = 0; i < 8; i++) led_duty[i] = 0;

  // Lettura ID e lampeggio conteggio su LED RX
  id = read_id();
  bus.include_sender_info(true);
  bus.set_id(id);

  Serial.begin(38400);
  bus.strategy.set_serial(&Serial);
  bus.set_acknowledge(true);
  bus.strategy.set_enable_RS485_pin(10);
  bus.set_receiver(receiver_function);
  bus.set_error(error_handler);
  bus.set_crc_32(false);
  bus.begin();

  // Inizializzazione sensore I2C LM75 (con timeout protetto)
  lm75_init();

  // Non trasmettere pacchetti non sollecitati all'avvio per non disturbare il bus RS485
  // (es. durante aggiornamenti OTA di altri nodi). Il Gateway interrogherà il nodo con il ping '?'.

  last_pjon_rx_time = millis();
}

void loop() {
  // Reset Watchdog ad ogni ciclo
  wdt_reset();

  // Scansione e gestione eventi pulsanti
  for (uint8_t i = 0; i < 8; i++) {
    buttons[i].update();

    if (buttons[i].isSingleClick()) {
      // Click breve: 'A'..'H' (corrisponde a Button 1..8 su Home Assistant)
      send_button_event('A' + i, i, false);
    } else if (buttons[i].isLongClick()) {
      // Long press: 'I'..'P' (corrisponde a Button 9..16 su Home Assistant)
      send_button_event('I' + i, i, true);
    } else if (buttons[i].isDoubleClick()) {
      // Double click: 'Q'..'X' (corrisponde a Button 17..24 su Home Assistant)
      send_button_event('Q' + i, i, false);
    }
  }

  // Gestione bus PJON
  bus.update();
  bus.receive();

  // Aggiornamento dinamico degli stati e duty cycle dei LED
  update_led_states();

  // Loop sensore di temperatura LM75
  lm75_loop();

  // Diagnostica presenza bus RS485
  check_rs485_diagnostics();
}
