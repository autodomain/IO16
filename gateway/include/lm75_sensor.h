#pragma once
#include <Arduino.h>

void lm75_init();
void lm75_loop();
bool lm75_is_available();
float lm75_get_temp();

