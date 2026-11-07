#include "ota_flasher.h"

#ifndef PJON_IO_WRITE
  #define PJON_IO_WRITE(P, V) digitalWrite(P, V)
  #define PJON_IO_MODE(P, V) pinMode(P, V)
  #define PJON_IO_READ(P) digitalRead(P)
#endif

#include <PJON.h>
#include <PJONThroughSerial.h>

extern PJON<ThroughSerial> bus1;
extern PJON<ThroughSerial> bus2;
extern const int rs485_enable_pin_1;
extern const int rs485_enable_pin_2;

// Costanti protocollo STK500 v1
#define STK_OK              0x10
#define STK_FAILED          0x11
#define STK_UNKNOWN         0x12
#define STK_NODEVICE        0x13
#define STK_INSYNC          0x14
#define STK_NOSYNC          0x15

#define STK_GET_SYNC        0x30
#define STK_ENTER_PROGMODE  0x50
#define STK_LEAVE_PROGMODE  0x51
#define STK_LOAD_ADDRESS    0x55
#define STK_PROG_PAGE       0x64
#define CRC_EOP             0x20

static HardwareSerial *ota_serial = &Serial;
static uint8_t ota_de_pin = 12;
static bool ota_active = false;
static char ota_detected_mcu[16] = "unknown";

const char* ota_get_target_mcu() {
    return ota_detected_mcu;
}

bool ota_is_active() {
    return ota_active;
}

void ota_abort() {
    ota_active = false;
    digitalWrite(ota_de_pin, LOW);
}

static void rs485_tx(const uint8_t *data, size_t len) {
    while (ota_serial->available()) ota_serial->read();
    digitalWrite(ota_de_pin, HIGH);
    delayMicroseconds(50);
    ota_serial->write(data, len);
    ota_serial->flush();
    digitalWrite(ota_de_pin, LOW);
}

static bool rs485_expect_sync(uint16_t timeout_ms = 400, char *err_detail = NULL, size_t err_len = 0) {
    unsigned long t0 = millis();
    uint8_t rx_hist[8];
    uint8_t rx_cnt = 0;

    while (millis() - t0 < timeout_ms) {
        if (ota_serial->available()) {
            uint8_t b = (uint8_t)ota_serial->read();
            if (rx_cnt < sizeof(rx_hist)) rx_hist[rx_cnt++] = b;

            // STK_OK (0x10) e' la conferma finale inviata da Optiboot al termine di ogni comando
            if (b == STK_OK) {
                return true;
            }
        }
    }

    if (err_detail && err_len > 0) {
        int n = snprintf(err_detail, err_len, "timeout (rx %u: ", rx_cnt);
        for (uint8_t i = 0; i < rx_cnt && n < (int)err_len - 6; i++) {
            n += snprintf(err_detail + n, err_len - n, "%02X ", rx_hist[i]);
        }
        if (n < (int)err_len - 1) snprintf(err_detail + n, err_len - n, ")");
    }
    return false;
}

bool ota_start(uint8_t node_id, uint8_t bus_id, char *err_msg, size_t err_len) {
    if (node_id == 0 || node_id > 63) {
        snprintf(err_msg, err_len, "ID nodo non valido: %u", node_id);
        return false;
    }

    if (bus_id == 2) {
        ota_serial = &Serial1;
        ota_de_pin = (uint8_t)rs485_enable_pin_2;
    } else {
        ota_serial = &Serial;
        ota_de_pin = (uint8_t)rs485_enable_pin_1;
    }

    // 1. Invia UN SOLO pacchetto PJON 'U' al nodo in modalita bloccante per ordinargli di riavviarsi in Optiboot RS485
    PJON<ThroughSerial> &pjon_bus = (bus_id == 2) ? bus2 : bus1;
    pjon_bus.set_acknowledge(false);
    pjon_bus.send_packet_blocking(node_id, "U", 1);

    // Attendi 200ms che il nodo completi il watchdog reset (15ms) e avvii Optiboot (65ms crystal startup)
    delay(200);

    ota_active = true;

    // Svuota buffer seriale
    while (ota_serial->available()) ota_serial->read();

    // 2. Handshake STK500 GET_SYNC
    const uint8_t cmd_sync[2] = { STK_GET_SYNC, CRC_EOP };
    bool synced = false;
    char diag[32] = "";
    for (uint8_t attempt = 0; attempt < 15; attempt++) {
        rs485_tx(cmd_sync, 2);
        if (rs485_expect_sync(200, diag, sizeof(diag))) {
            synced = true;
            break;
        }
        delay(30);
    }

    if (!synced) {
        ota_abort();
        snprintf(err_msg, err_len, "Handshake STK500 fallito nodo %u: %s", node_id, diag);
        return false;
    }

    delay(20);

    // 3. Entra in modalita programmazione STK500
    const uint8_t cmd_prog[2] = { STK_ENTER_PROGMODE, CRC_EOP };
    rs485_tx(cmd_prog, 2);
    if (!rs485_expect_sync(400, diag, sizeof(diag))) {
        ota_abort();
        snprintf(err_msg, err_len, "Fallito STK_ENTER_PROGMODE: %s", diag);
        return false;
    }

    delay(20);

    // 4. Lettura firma microcontrollore STK_READ_SIGN ('u' = 0x75 0x20)
    strcpy(ota_detected_mcu, "unknown");
    while (ota_serial->available()) ota_serial->read();
    const uint8_t cmd_sign[2] = { 0x75, 0x20 };
    rs485_tx(cmd_sign, 2);
    unsigned long st0 = millis();
    uint8_t sign_bytes[8];
    uint8_t sign_n = 0;
    while (millis() - st0 < 250 && sign_n < 5) {
        if (ota_serial->available()) {
            sign_bytes[sign_n++] = ota_serial->read();
        }
    }
    for (uint8_t i = 0; i + 2 < sign_n; i++) {
        if (sign_bytes[i] == 0x1E) {
            if (sign_bytes[i+1] == 0x97) strcpy(ota_detected_mcu, "atmega1284p");
            else if (sign_bytes[i+1] == 0x95) strcpy(ota_detected_mcu, "atmega324pb");
            break;
        }
    }

    return true;
}

bool ota_write_page(uint16_t word_addr, const uint8_t *page_data, uint16_t page_len, char *err_msg, size_t err_len) {
    if (!ota_active) {
        snprintf(err_msg, err_len, "Sessione OTA non attiva");
        return false;
    }

    char diag[32] = "";

    // 1. STK_LOAD_ADDRESS (l'indirizzo STK500 e' espresso in WORD a 16 bit)
    uint8_t cmd_addr[4] = {
        STK_LOAD_ADDRESS,
        (uint8_t)(word_addr & 0xFF),
        (uint8_t)((word_addr >> 8) & 0xFF),
        CRC_EOP
    };
    bool addr_ok = false;
    for (uint8_t attempt = 0; attempt < 3; attempt++) {
        rs485_tx(cmd_addr, 4);
        if (rs485_expect_sync(300, diag, sizeof(diag))) {
            addr_ok = true;
            break;
        }
        delay(15);
    }
    if (!addr_ok) {
        snprintf(err_msg, err_len, "Errore LOAD_ADDRESS a word 0x%04X: %s", word_addr, diag);
        return false;
    }

    delay(10);

    // 2. STK_PROG_PAGE (scrive la pagina di Flash)
    while (ota_serial->available()) ota_serial->read();
    digitalWrite(ota_de_pin, HIGH);
    delayMicroseconds(50);
    uint8_t hdr[4] = {
        STK_PROG_PAGE,
        (uint8_t)((page_len >> 8) & 0xFF),
        (uint8_t)(page_len & 0xFF),
        'F'
    };
    ota_serial->write(hdr, 4);
    ota_serial->write(page_data, page_len);
    uint8_t eop = CRC_EOP;
    ota_serial->write(&eop, 1);
    ota_serial->flush();
    digitalWrite(ota_de_pin, LOW);

    if (!rs485_expect_sync(1000, diag, sizeof(diag))) {
        snprintf(err_msg, err_len, "Errore PROG_PAGE a word 0x%04X: %s", word_addr, diag);
        return false;
    }

    return true;
}

bool ota_end(char *err_msg, size_t err_len) {
    if (!ota_active) return true;

    // STK_LEAVE_PROGMODE
    const uint8_t cmd_leave[2] = { STK_LEAVE_PROGMODE, CRC_EOP };
    rs485_tx(cmd_leave, 2);
    rs485_expect_sync(300);

    ota_active = false;
    digitalWrite(ota_de_pin, LOW);
    return true;
}

