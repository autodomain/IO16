#ifndef OTA_FLASHER_H
#define OTA_FLASHER_H

#include <Arduino.h>

bool ota_is_active();
const char* ota_get_target_mcu();
bool ota_start(uint8_t node_id, uint8_t bus_id, char *err_msg, size_t err_len);
bool ota_write_page(uint16_t word_addr, const uint8_t *page_data, uint16_t page_len, char *err_msg, size_t err_len);
bool ota_end(char *err_msg, size_t err_len);
void ota_abort();

#endif // OTA_FLASHER_H

