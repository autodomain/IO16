#ifndef NTP_CLIENT_H
#define NTP_CLIENT_H

#include <Arduino.h>

void ntp_init();
void ntp_update();
bool ntp_is_synced();
uint32_t ntp_get_epoch();
void ntp_get_time_str(char *buf, size_t max_len);
void ntp_get_datetime_str(char *buf, size_t max_len);

#endif // NTP_CLIENT_H
