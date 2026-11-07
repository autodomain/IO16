#include "ha_discovery.h"
#include <PubSubClient.h>
#include <Ethernet.h>
#include "config.h"
#include "web_server.h"
#include "lm75_sensor.h"
#include "node_diagnostics.h"
#include "ntp_client.h"

extern PubSubClient client;

// Buffer per topic di discovery
static char tbuf[96];

void ha_discovery_publish_node(uint8_t node_id) {
    if (!config.ha_discovery || !client.connected()) return;

    // 0. Stato di Diagnostica Nodo (binary_sensor con device_class connectivity)
    snprintf_P(tbuf, sizeof(tbuf), PSTR("homeassistant/binary_sensor/%s_n%u/status/config"), config.mqtt_client_id, node_id);
    char j_node_status[320];
    snprintf_P(j_node_status, sizeof(j_node_status),
        PSTR("{\"name\":\"Stato Tastiera %u\",\"stat_t\":\"%s/node/%u/status\",\"pl_on\":\"online\",\"pl_off\":\"offline\","
        "\"dev_cla\":\"connectivity\",\"uniq_id\":\"%s_n%u_status\",\"ic\":\"mdi:network-outline\","
        "\"dev\":{\"ids\":[\"%s_node_%u\"],\"name\":\"%s Tastiera %u\",\"mf\":\"PJON RS485\",\"via_device\":\"%s\"}}"),
        node_id, config.mqtt_client_id, node_id,
        config.mqtt_client_id, node_id,
        config.mqtt_client_id, node_id, config.gateway_name, node_id, config.mqtt_client_id
    );
    client.publish(tbuf, j_node_status, true);
    delay(15);

    // 1. Tasti 1..8 (Click Singolo: binary_sensor su btn/1..8 con off_delay per trigger)
    for (uint8_t b = 1; b <= 8; b++) {
        snprintf_P(tbuf, sizeof(tbuf), PSTR("homeassistant/binary_sensor/%s_n%u/btn_%u/config"), config.mqtt_client_id, node_id, b);

        char json[320];
        snprintf_P(json, sizeof(json),
            PSTR("{\"name\":\"Tasto %u\",\"stat_t\":\"%s/node/%u/btn/%u\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\","
            "\"off_dly\":1,\"uniq_id\":\"%s_n%u_b%u\",\"ic\":\"mdi:gesture-tap-button\","
            "\"dev\":{\"ids\":[\"%s_node_%u\"],\"name\":\"%s Tastiera %u\",\"mf\":\"PJON RS485\",\"via_device\":\"%s\"}}"),
            b, config.mqtt_client_id, node_id, b,
            config.mqtt_client_id, node_id, b,
            config.mqtt_client_id, node_id, config.gateway_name, node_id, config.mqtt_client_id
        );

        client.publish(tbuf, json, true);
        delay(15);
    }

    // 1b. Tasti 1..8 Pressione Lunga (binary_sensor su btn/9..16)
    for (uint8_t b = 1; b <= 8; b++) {
        snprintf_P(tbuf, sizeof(tbuf), PSTR("homeassistant/binary_sensor/%s_n%u/btn_long_%u/config"), config.mqtt_client_id, node_id, b);

        char json[320];
        snprintf_P(json, sizeof(json),
            PSTR("{\"name\":\"Tasto %u (Pressione Lunga)\",\"stat_t\":\"%s/node/%u/btn/%u\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\","
            "\"off_dly\":1,\"uniq_id\":\"%s_n%u_bl%u\",\"ic\":\"mdi:gesture-tap-hold\","
            "\"dev\":{\"ids\":[\"%s_node_%u\"],\"name\":\"%s Tastiera %u\",\"mf\":\"PJON RS485\",\"via_device\":\"%s\"}}"),
            b, config.mqtt_client_id, node_id, b + 8,
            config.mqtt_client_id, node_id, b,
            config.mqtt_client_id, node_id, config.gateway_name, node_id, config.mqtt_client_id
        );

        client.publish(tbuf, json, true);
        delay(15);
    }

    // 1c. Tasti 1..8 Doppio Click (binary_sensor su btn/17..24)
    for (uint8_t b = 1; b <= 8; b++) {
        snprintf_P(tbuf, sizeof(tbuf), PSTR("homeassistant/binary_sensor/%s_n%u/btn_dbl_%u/config"), config.mqtt_client_id, node_id, b);

        char json[320];
        snprintf_P(json, sizeof(json),
            PSTR("{\"name\":\"Tasto %u (Doppio Click)\",\"stat_t\":\"%s/node/%u/btn/%u\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\","
            "\"off_dly\":1,\"uniq_id\":\"%s_n%u_bd%u\",\"ic\":\"mdi:gesture-double-tap\","
            "\"dev\":{\"ids\":[\"%s_node_%u\"],\"name\":\"%s Tastiera %u\",\"mf\":\"PJON RS485\",\"via_device\":\"%s\"}}"),
            b, config.mqtt_client_id, node_id, b + 16,
            config.mqtt_client_id, node_id, b,
            config.mqtt_client_id, node_id, config.gateway_name, node_id, config.mqtt_client_id
        );

        client.publish(tbuf, json, true);
        delay(15);
    }

    // 2. LED 1..8 per questo nodo (light switchabile)
    for (uint8_t l = 1; l <= 8; l++) {
        snprintf_P(tbuf, sizeof(tbuf), PSTR("homeassistant/light/%s_n%u/led_%u/config"), config.mqtt_client_id, node_id, l);

        char json[320];
        snprintf_P(json, sizeof(json),
            PSTR("{\"name\":\"LED %u\",\"stat_t\":\"%s/node/%u/led/%u/state\",\"cmd_t\":\"%s/node/%u/led/%u/set\","
            "\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"uniq_id\":\"%s_n%u_l%u\",\"ic\":\"mdi:led-on\","
            "\"dev\":{\"ids\":[\"%s_node_%u\"],\"name\":\"%s Tastiera %u\",\"mf\":\"PJON RS485\",\"via_device\":\"%s\"}}"),
            l, config.mqtt_client_id, node_id, l, config.mqtt_client_id, node_id, l,
            config.mqtt_client_id, node_id, l,
            config.mqtt_client_id, node_id, config.gateway_name, node_id, config.mqtt_client_id
        );

        client.publish(tbuf, json, true);
        delay(15);
    }

    // 2b. Master: Tutti i LED (light switchabile su led 0)
    snprintf_P(tbuf, sizeof(tbuf), PSTR("homeassistant/light/%s_n%u/led_all/config"), config.mqtt_client_id, node_id);
    char j_led_all[320];
    snprintf_P(j_led_all, sizeof(j_led_all),
        PSTR("{\"name\":\"Tutti i LED\",\"stat_t\":\"%s/node/%u/led/0/state\",\"cmd_t\":\"%s/node/%u/led/0/set\","
        "\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"uniq_id\":\"%s_n%u_l0\",\"ic\":\"mdi:lightbulb-group\","
        "\"dev\":{\"ids\":[\"%s_node_%u\"],\"name\":\"%s Tastiera %u\",\"mf\":\"PJON RS485\",\"via_device\":\"%s\"}}"),
        config.mqtt_client_id, node_id, config.mqtt_client_id, node_id,
        config.mqtt_client_id, node_id,
        config.mqtt_client_id, node_id, config.gateway_name, node_id, config.mqtt_client_id
    );
    client.publish(tbuf, j_led_all, true);
    delay(15);

    // 3. Sensore di Temperatura LM75 per questo nodo (se equipaggiato)
    snprintf_P(tbuf, sizeof(tbuf), PSTR("homeassistant/sensor/%s_n%u/temp/config"), config.mqtt_client_id, node_id);
    char j_node_temp[320];
    snprintf_P(j_node_temp, sizeof(j_node_temp),
        PSTR("{\"name\":\"Temperatura Tastiera %u\",\"stat_t\":\"%s/node/%u/temperature\",\"unit_of_meas\":\"°C\","
        "\"dev_cla\":\"temperature\",\"state_cla\":\"measurement\",\"uniq_id\":\"%s_n%u_temp\","
        "\"dev\":{\"ids\":[\"%s_node_%u\"],\"name\":\"%s Tastiera %u\",\"mf\":\"PJON RS485\",\"via_device\":\"%s\"}}"),
        node_id, config.mqtt_client_id, node_id,
        config.mqtt_client_id, node_id,
        config.mqtt_client_id, node_id, config.gateway_name, node_id, config.mqtt_client_id
    );
    client.publish(tbuf, j_node_temp, true);
    delay(15);

    // 4. Sensore Qualità Segnale RS485
    snprintf_P(tbuf, sizeof(tbuf), PSTR("homeassistant/sensor/%s_n%u/quality/config"), config.mqtt_client_id, node_id);
    char j_node_qual[320];
    snprintf_P(j_node_qual, sizeof(j_node_qual),
        PSTR("{\"name\":\"Qualit\\u00e0 Link Tastiera %u\",\"stat_t\":\"%s/node/%u/quality\",\"unit_of_meas\":\"%%\","
        "\"dev_cla\":\"signal_strength\",\"entity_category\":\"diagnostic\",\"uniq_id\":\"%s_n%u_quality\",\"ic\":\"mdi:signal\","
        "\"dev\":{\"ids\":[\"%s_node_%u\"],\"name\":\"%s Tastiera %u\",\"mf\":\"PJON RS485\",\"via_device\":\"%s\"}}"),
        node_id, config.mqtt_client_id, node_id,
        config.mqtt_client_id, node_id,
        config.mqtt_client_id, node_id, config.gateway_name, node_id, config.mqtt_client_id
    );
    client.publish(tbuf, j_node_qual, true);
    delay(15);

    // 5. Switch Modalita Notte per questo nodo
    snprintf_P(tbuf, sizeof(tbuf), PSTR("homeassistant/switch/%s_n%u/night_mode/config"), config.mqtt_client_id, node_id);
    char j_node_nm[320];
    snprintf_P(j_node_nm, sizeof(j_node_nm),
        PSTR("{\"name\":\"Modalit\\u00e0 Notte\",\"stat_t\":\"%s/node/%u/night_mode/state\",\"cmd_t\":\"%s/node/%u/night_mode/set\","
        "\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"uniq_id\":\"%s_n%u_nm\",\"ic\":\"mdi:weather-night\","
        "\"dev\":{\"ids\":[\"%s_node_%u\"],\"name\":\"%s Tastiera %u\",\"mf\":\"PJON RS485\",\"via_device\":\"%s\"}}"),
        config.mqtt_client_id, node_id, config.mqtt_client_id, node_id,
        config.mqtt_client_id, node_id,
        config.mqtt_client_id, node_id, config.gateway_name, node_id, config.mqtt_client_id
    );
    client.publish(tbuf, j_node_nm, true);
    delay(15);

    // 6. Switch Modalita Pulizia per questo nodo
    snprintf_P(tbuf, sizeof(tbuf), PSTR("homeassistant/switch/%s_n%u/clean_mode/config"), config.mqtt_client_id, node_id);
    char j_node_cm[320];
    snprintf_P(j_node_cm, sizeof(j_node_cm),
        PSTR("{\"name\":\"Modalit\\u00e0 Pulizia (60s)\",\"stat_t\":\"%s/node/%u/clean_mode/state\",\"cmd_t\":\"%s/node/%u/clean_mode/set\","
        "\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"uniq_id\":\"%s_n%u_cm\",\"ic\":\"mdi:spray-bottle\","
        "\"dev\":{\"ids\":[\"%s_node_%u\"],\"name\":\"%s Tastiera %u\",\"mf\":\"PJON RS485\",\"via_device\":\"%s\"}}"),
        config.mqtt_client_id, node_id, config.mqtt_client_id, node_id,
        config.mqtt_client_id, node_id,
        config.mqtt_client_id, node_id, config.gateway_name, node_id, config.mqtt_client_id
    );
    client.publish(tbuf, j_node_cm, true);
    delay(15);
}

void ha_discovery_publish_all() {
    if (!config.ha_discovery || !client.connected()) return;

    // 1. Gateway Status (binary_sensor connectivity)
    snprintf_P(tbuf, sizeof(tbuf), PSTR("homeassistant/binary_sensor/%s/status/config"), config.mqtt_client_id);
    char j_status[280];
    snprintf_P(j_status, sizeof(j_status),
        PSTR("{\"name\":\"Stato Connessione\",\"stat_t\":\"%s/status\",\"dev_cla\":\"connectivity\","
        "\"uniq_id\":\"%s_status\",\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"PJON RS485\",\"mdl\":\"ATmega128 Gateway\",\"sw\":\"2.0\"}}"),
        config.mqtt_client_id, config.mqtt_client_id, config.mqtt_client_id, config.gateway_name
    );
    client.publish(tbuf, j_status, true);
    delay(20);

    // 2. IP Sensor
    snprintf_P(tbuf, sizeof(tbuf), PSTR("homeassistant/sensor/%s/ip/config"), config.mqtt_client_id);
    char j_ip[240];
    snprintf_P(j_ip, sizeof(j_ip),
        PSTR("{\"name\":\"Indirizzo IP\",\"stat_t\":\"%s/telemetry\",\"val_tpl\":\"{{ value_json.ip }}\","
        "\"uniq_id\":\"%s_ip\",\"ic\":\"mdi:ip-network\",\"dev\":{\"ids\":[\"%s\"]}}"),
        config.mqtt_client_id, config.mqtt_client_id, config.mqtt_client_id
    );
    client.publish(tbuf, j_ip, true);
    delay(20);

    // 3. PJON RX Sensor
    snprintf_P(tbuf, sizeof(tbuf), PSTR("homeassistant/sensor/%s/pjon_rx/config"), config.mqtt_client_id);
    char j_rx[240];
    snprintf_P(j_rx, sizeof(j_rx),
        PSTR("{\"name\":\"PJON Pacchetti RX\",\"stat_t\":\"%s/telemetry\",\"val_tpl\":\"{{ value_json.pjon_rx }}\","
        "\"uniq_id\":\"%s_pjon_rx\",\"ic\":\"mdi:counter\",\"dev\":{\"ids\":[\"%s\"]}}"),
        config.mqtt_client_id, config.mqtt_client_id, config.mqtt_client_id
    );
    client.publish(tbuf, j_rx, true);
    delay(20);

    // 4. PJON TX Sensor
    snprintf_P(tbuf, sizeof(tbuf), PSTR("homeassistant/sensor/%s/pjon_tx/config"), config.mqtt_client_id);
    char j_tx[240];
    snprintf_P(j_tx, sizeof(j_tx),
        PSTR("{\"name\":\"PJON Pacchetti TX\",\"stat_t\":\"%s/telemetry\",\"val_tpl\":\"{{ value_json.pjon_tx }}\","
        "\"uniq_id\":\"%s_pjon_tx\",\"ic\":\"mdi:counter\",\"dev\":{\"ids\":[\"%s\"]}}"),
        config.mqtt_client_id, config.mqtt_client_id, config.mqtt_client_id
    );
    client.publish(tbuf, j_tx, true);
    delay(20);

    // 5. Termometro LM75 Gateway (Sensore di Temperatura Home Assistant)
    snprintf_P(tbuf, sizeof(tbuf), PSTR("homeassistant/sensor/%s/temp/config"), config.mqtt_client_id);
    char j_temp[280];
    snprintf_P(j_temp, sizeof(j_temp),
        PSTR("{\"name\":\"Temperatura Gateway\",\"stat_t\":\"%s/temperature\",\"unit_of_meas\":\"°C\","
        "\"dev_cla\":\"temperature\",\"state_cla\":\"measurement\",\"uniq_id\":\"%s_temp\","
        "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"PJON RS485\",\"mdl\":\"ATmega128 Gateway\",\"sw\":\"2.0\"}}"),
        config.mqtt_client_id, config.mqtt_client_id, config.mqtt_client_id, config.gateway_name
    );
    client.publish(tbuf, j_temp, true);
    delay(20);

    // 6. Sensore Salute Bus RS485
    snprintf_P(tbuf, sizeof(tbuf), PSTR("homeassistant/sensor/%s/bus_health/config"), config.mqtt_client_id);
    char j_health[280];
    snprintf_P(j_health, sizeof(j_health),
        PSTR("{\"name\":\"Salute Bus RS485\",\"stat_t\":\"%s/bus_health\",\"unit_of_meas\":\"%%\","
        "\"dev_cla\":\"signal_strength\",\"entity_category\":\"diagnostic\",\"uniq_id\":\"%s_bus_health\","
        "\"ic\":\"mdi:transit-connection-variant\",\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"PJON RS485\"}}"),
        config.mqtt_client_id, config.mqtt_client_id, config.mqtt_client_id, config.gateway_name
    );
    client.publish(tbuf, j_health, true);
    delay(20);

    // 7. Sensore Data e Ora Gateway (NTP)
    snprintf_P(tbuf, sizeof(tbuf), PSTR("homeassistant/sensor/%s/time/config"), config.mqtt_client_id);
    char j_time[280];
    snprintf_P(j_time, sizeof(j_time),
        PSTR("{\"name\":\"Data e Ora Gateway\",\"stat_t\":\"%s/time\",\"entity_category\":\"diagnostic\","
        "\"uniq_id\":\"%s_time\",\"ic\":\"mdi:clock-check-outline\",\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"PJON RS485\"}}"),
        config.mqtt_client_id, config.mqtt_client_id, config.mqtt_client_id, config.gateway_name
    );
    client.publish(tbuf, j_time, true);
    delay(20);

    // 8. Master Switch Modalita Notte (Tutte le Tastiere)
    snprintf_P(tbuf, sizeof(tbuf), PSTR("homeassistant/switch/%s/night_mode/config"), config.mqtt_client_id);
    char j_nm_all[280];
    snprintf_P(j_nm_all, sizeof(j_nm_all),
        PSTR("{\"name\":\"Modalit\\u00e0 Notte (Tutte le Tastiere)\",\"stat_t\":\"%s/night_mode/state\",\"cmd_t\":\"%s/night_mode/set\","
        "\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"uniq_id\":\"%s_night_mode_all\",\"ic\":\"mdi:weather-night\","
        "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"PJON RS485\"}}"),
        config.mqtt_client_id, config.mqtt_client_id, config.mqtt_client_id, config.mqtt_client_id, config.gateway_name
    );
    client.publish(tbuf, j_nm_all, true);
    delay(20);

    // 9. Registra tutti i nodi client registrati e memorizzati in EEPROM (fino a 63 nodi)
    for (uint8_t n = 1; n <= 63; n++) {
        if (remote_nodes[n].active) {
            ha_discovery_publish_node(n);
        }
    }
}

void ha_publish_telemetry() {
    if (!client.connected()) return;

    char tele_topic[48];
    snprintf(tele_topic, sizeof(tele_topic), "%s/telemetry", config.mqtt_client_id);

    IPAddress ip = Ethernet.localIP();
    uint8_t health = get_rs485_bus_health_pct();
    char time_str[24];
    ntp_get_datetime_str(time_str, sizeof(time_str));

    char tele_json[180];
    if (lm75_is_available()) {
        char temp_str[10];
        dtostrf(lm75_get_temp(), 4, 1, temp_str);
        snprintf(tele_json, sizeof(tele_json),
            "{\"ip\":\"%u.%u.%u.%u\",\"pjon_rx\":%lu,\"pjon_tx\":%lu,\"pjon_err\":%lu,\"health\":%u,\"uptime\":%lu,\"temp\":%s}",
            ip[0], ip[1], ip[2], ip[3], pjon_packets_rx, pjon_packets_tx, pjon_packets_err, health, millis() / 1000, temp_str
        );
    } else {
        snprintf(tele_json, sizeof(tele_json),
            "{\"ip\":\"%u.%u.%u.%u\",\"pjon_rx\":%lu,\"pjon_tx\":%lu,\"pjon_err\":%lu,\"health\":%u,\"uptime\":%lu}",
            ip[0], ip[1], ip[2], ip[3], pjon_packets_rx, pjon_packets_tx, pjon_packets_err, health, millis() / 1000
        );
    }

    client.publish(tele_topic, tele_json);

    // Pubblica anche i sensori dedicati diretti per HA
    char h_topic[48];
    snprintf(h_topic, sizeof(h_topic), "%s/bus_health", config.mqtt_client_id);
    char h_str[8];
    snprintf(h_str, sizeof(h_str), "%u", health);
    client.publish(h_topic, h_str, true);

    char tm_topic[48];
    snprintf(tm_topic, sizeof(tm_topic), "%s/time", config.mqtt_client_id);
    client.publish(tm_topic, time_str, true);
}
