#include "ntp_client.h"
#include <Ethernet.h>
#include <EthernetUdp.h>

static EthernetUDP ntp_udp;
static const unsigned int local_port = 8888;
static const unsigned int ntp_port = 123;
static const char ntp_server_host[] = "pool.ntp.org";
static const int NTP_PACKET_SIZE = 48;
static byte packet_buffer[NTP_PACKET_SIZE];

static uint32_t current_epoch = 0;
static unsigned long last_epoch_millis = 0;
static unsigned long last_ntp_sync_attempt = 0;
static bool ntp_synced = false;
static bool ntp_request_pending = false;
static unsigned long ntp_request_time = 0;

// Fuso orario: Europe/Rome (UTC+1 invernale, +2 estivo)
// Funzione di calcolo ora legale automatica per l'Italia (ultima domenica marzo -> ultima domenica ottobre)
static bool is_dst_italy(int year, int month, int day, int hour) {
    if (month < 3 || month > 10) return false;
    if (month > 3 && month < 10) return true;
    int last_sunday = 31 - ((5 * year / 4 + 4) % 7);
    if (month == 3) {
        return (day > last_sunday) || (day == last_sunday && hour >= 2);
    } else {
        return (day < last_sunday) || (day == last_sunday && hour < 3);
    }
}

void ntp_init() {
    ntp_udp.begin(local_port);
    ntp_synced = false;
    ntp_request_pending = false;
    last_ntp_sync_attempt = 0;
}

static void send_ntp_packet() {
    memset(packet_buffer, 0, NTP_PACKET_SIZE);
    // LI = 0, Version = 4, Mode = 3 (Client)
    packet_buffer[0] = 0b00100011;
    packet_buffer[1] = 0;
    packet_buffer[2] = 6;
    packet_buffer[3] = 0xEC;
    // 8 bytes zero per Root Delay & Root Dispersion
    packet_buffer[12] = 49;
    packet_buffer[13] = 0x4E;
    packet_buffer[14] = 49;
    packet_buffer[15] = 52;

    if (ntp_udp.beginPacket(ntp_server_host, ntp_port)) {
        ntp_udp.write(packet_buffer, NTP_PACKET_SIZE);
        ntp_udp.endPacket();
        ntp_request_pending = true;
        ntp_request_time = millis();
    }
}

void ntp_update() {
    unsigned long now = millis();

    // Sincronizzazione periodica ogni 1 ora (o subito all'avvio)
    if (!ntp_request_pending) {
        if (!ntp_synced || (now - last_ntp_sync_attempt >= 3600000UL)) {
            last_ntp_sync_attempt = now;
            send_ntp_packet();
        }
    } else {
        // Attesa risposta pacchetto NTP
        int size = ntp_udp.parsePacket();
        if (size >= NTP_PACKET_SIZE) {
            ntp_udp.read(packet_buffer, NTP_PACKET_SIZE);
            // I secondi sono dal byte 40 al 43
            uint32_t high_word = word(packet_buffer[40], packet_buffer[41]);
            uint32_t low_word = word(packet_buffer[42], packet_buffer[43]);
            uint32_t secs_since_1900 = (high_word << 16) | low_word;

            // Costante 70 anni: 2208988800UL (da 1900 a 1970 UNIX Epoch)
            if (secs_since_1900 > 2208988800UL) {
                current_epoch = secs_since_1900 - 2208988800UL;
                last_epoch_millis = millis();
                ntp_synced = true;
            }
            ntp_request_pending = false;
        } else if (now - ntp_request_time > 3000) {
            // Timeout risposta NTP (3 secondi)
            ntp_request_pending = false;
        }
    }
}

bool ntp_is_synced() {
    return ntp_synced;
}

uint32_t ntp_get_epoch() {
    if (!ntp_synced) return 0;
    return current_epoch + (millis() - last_epoch_millis) / 1000UL;
}

static void epoch_to_datetime(uint32_t epoch, int &year, int &month, int &day, int &hour, int &minute, int &second) {
    second = epoch % 60;
    epoch /= 60;
    minute = epoch % 60;
    epoch /= 60;
    hour = epoch % 24;
    epoch /= 24; // Giorni da 1/1/1970

    // Calcolo anno, mese, giorno
    uint32_t a = (uint32_t)((4 * epoch + 102032) / 146097 + 15);
    uint32_t b = (uint32_t)(epoch + 2442113 + a - (a / 4));
    uint32_t c = (20 * b - 2442) / 7305;
    uint32_t d = b - 365 * c - (c / 4);
    uint32_t e = (uint32_t)((1000 * d + 5204) / 30601);
    day = d - (uint32_t)((30601 * e) / 1000);
    month = (e < 14) ? (e - 1) : (e - 13);
    year = (month > 2) ? (c - 4716) : (c - 4715);
}

void ntp_get_time_str(char *buf, size_t max_len) {
    if (!ntp_synced) {
        // Fallback: visualizza tempo da boot in formato mm:ss o hh:mm:ss
        unsigned long s = millis() / 1000;
        snprintf(buf, max_len, "+%02lu:%02lu:%02lu", s / 3600, (s % 3600) / 60, s % 60);
        return;
    }

    uint32_t epoch = ntp_get_epoch();
    int y, m, d, hh, mm, ss;
    epoch_to_datetime(epoch, y, m, d, hh, mm, ss);

    // Applica ora legale italiana (+1 solare o +2 estiva)
    int offset = is_dst_italy(y, m, d, hh) ? 2 : 1;
    epoch += (offset * 3600UL);
    epoch_to_datetime(epoch, y, m, d, hh, mm, ss);

    snprintf(buf, max_len, "%02d:%02d:%02d", hh, mm, ss);
}

void ntp_get_datetime_str(char *buf, size_t max_len) {
    if (!ntp_synced) {
        snprintf(buf, max_len, "Non sincronizzato");
        return;
    }

    uint32_t epoch = ntp_get_epoch();
    int y, m, d, hh, mm, ss;
    epoch_to_datetime(epoch, y, m, d, hh, mm, ss);

    int offset = is_dst_italy(y, m, d, hh) ? 2 : 1;
    epoch += (offset * 3600UL);
    epoch_to_datetime(epoch, y, m, d, hh, mm, ss);

    snprintf(buf, max_len, "%02d/%02d/%04d %02d:%02d:%02d", d, m, y, hh, mm, ss);
}
