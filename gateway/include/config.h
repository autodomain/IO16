#pragma once
#include <Arduino.h>
#include <EEPROM.h>

#define CONFIG_MAGIC 0x47575459 // "GWTY"
#define CONFIG_VERSION 2

struct GatewayConfig {
    uint32_t magic;
    uint8_t version;
    uint8_t dhcp;            // 1 = DHCP, 0 = Static
    uint8_t ip[4];
    uint8_t gateway[4];
    uint8_t subnet[4];
    uint8_t dns[4];
    uint8_t mac[6];
    char mqtt_server[32];
    uint16_t mqtt_port;
    char mqtt_user[20];
    char mqtt_pass[20];
    char mqtt_client_id[24]; // ID univoco per MQTT e Home Assistant
    char gateway_name[32];   // Nome descrittivo es. "PJON Gateway Piano Terra"
    char mqtt_prefix[20];
    uint8_t pjon_id;
    uint32_t pjon_baud;
    uint8_t ha_discovery;    // 1 = Auto-Discovery attivo
    uint8_t ha_nodes_count;  // Nodi tastiere client pre-registrati (es. 2)
};

extern GatewayConfig config;

void config_load_defaults();
void config_load();
void config_save();
