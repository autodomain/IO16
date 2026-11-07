#include "config.h"
#include <string.h>

GatewayConfig config;

void config_load_defaults() {
    config.magic = CONFIG_MAGIC;
    config.version = CONFIG_VERSION;
    config.dhcp = 1; // Default DHCP attivo
    
    // IP di fallback (se statico)
    config.ip[0] = 192; config.ip[1] = 168; config.ip[2] = 1; config.ip[3] = 100;
    config.gateway[0] = 192; config.gateway[1] = 168; config.gateway[2] = 1; config.gateway[3] = 1;
    config.subnet[0] = 255; config.subnet[1] = 255; config.subnet[2] = 255; config.subnet[3] = 0;
    config.dns[0] = 192; config.dns[1] = 168; config.dns[2] = 1; config.dns[3] = 1;
    
    // MAC address di default
    config.mac[0] = 0xAA; config.mac[1] = 0xED; config.mac[2] = 0xAB;
    config.mac[3] = 0xFE; config.mac[4] = 0xAA; config.mac[5] = 0xF2;
    
    // ID Univoco calcolato con MAC per evitare conflitti tra Gateway multipli
    snprintf(config.mqtt_client_id, sizeof(config.mqtt_client_id), "pjon_gw_%02x%02x", config.mac[4], config.mac[5]);
    snprintf(config.gateway_name, sizeof(config.gateway_name), "PJON Gateway %02X%02X", config.mac[4], config.mac[5]);

    // MQTT impostazioni originali
    strncpy(config.mqtt_server, "192.168.1.250", sizeof(config.mqtt_server) - 1);
    config.mqtt_server[sizeof(config.mqtt_server) - 1] = '\0';
    config.mqtt_port = 1883;
    strncpy(config.mqtt_user, "admin", sizeof(config.mqtt_user) - 1);
    config.mqtt_user[sizeof(config.mqtt_user) - 1] = '\0';
    strncpy(config.mqtt_pass, "admin", sizeof(config.mqtt_pass) - 1);
    config.mqtt_pass[sizeof(config.mqtt_pass) - 1] = '\0';
    strncpy(config.mqtt_prefix, "pjon", sizeof(config.mqtt_prefix) - 1);
    config.mqtt_prefix[sizeof(config.mqtt_prefix) - 1] = '\0';
    
    // PJON impostazioni originali
    config.pjon_id = 254;
    config.pjon_baud = 38400;

    // Home Assistant Auto-Discovery
    config.ha_discovery = 1;
    config.ha_nodes_count = 2; // Pre-registra nodi 1 e 2 (espandibile)
}

void config_load() {
    EEPROM.get(0, config);
    if (config.magic != CONFIG_MAGIC || config.version != CONFIG_VERSION) {
        config_load_defaults();
        config_save();
    }
}

void config_save() {
    EEPROM.put(0, config);
}
