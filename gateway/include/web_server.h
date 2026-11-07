#pragma once
#include <Arduino.h>
#include <Ethernet.h>
#include "config.h"

extern EthernetServer httpServer;

// Contatori di diagnostica
extern unsigned long pjon_packets_tx;
extern unsigned long pjon_packets_rx;
extern unsigned long pjon_packets_err;
extern unsigned long mqtt_messages_tx;
extern unsigned long mqtt_messages_rx;

// Ultimi dati MQTT ricevuti/trasmessi
#define MQTT_LOG_DEPTH 5
struct MqttLogItem {
  char time[9];       // Orario formato HH:MM:SS
  char topic[44];
  char payload[24];
};

extern char last_mqtt_rx_topic[48];
extern char last_mqtt_rx_payload[32];
extern unsigned long last_mqtt_rx_time;
extern char last_mqtt_tx_topic[48];
extern char last_mqtt_tx_payload[32];

extern MqttLogItem mqtt_rx_history[MQTT_LOG_DEPTH];
extern uint8_t mqtt_rx_history_count;
extern MqttLogItem mqtt_tx_history[MQTT_LOG_DEPTH];
extern uint8_t mqtt_tx_history_count;

void web_server_log_rx(const char *topic, const uint8_t *payload, unsigned int length);
void web_server_log_tx(const char *topic, const char *payload);

void web_server_init();
void web_server_handle();

