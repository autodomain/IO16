#include "lm75_sensor.h"
#include <Wire.h>

static uint8_t lm75_addr = 0x48;
static bool lm75_present = false;
static float lm75_temperature = 0.0f;
static unsigned long last_temp_read = 0;

static bool lm75_read_internal() {
    Wire.beginTransmission(lm75_addr);
    Wire.write(0x00); // Puntatore al registro temperatura
    if (Wire.endTransmission() != 0) {
        lm75_present = false;
        return false;
    }

    if (Wire.requestFrom(lm75_addr, (uint8_t)2) == 2) {
        int8_t msb = Wire.read();
        uint8_t lsb = Wire.read();
        int16_t raw = ((int16_t)msb << 8) | lsb;
        // Shift a destra di 5 bit per step da 0.125 °C (supporta sia 9-bit LM75 che 11-bit LM75A/B)
        lm75_temperature = (float)(raw >> 5) * 0.125f;
        lm75_present = true;
        return true;
    }
    return false;
}

void lm75_init() {
    Wire.begin();
    
    // Prova indirizzo standard 0x48
    Wire.beginTransmission(0x48);
    if (Wire.endTransmission() == 0) {
        lm75_addr = 0x48;
        lm75_present = true;
    } else {
        // Scansione alternativa da 0x49 a 0x4F
        for (uint8_t a = 0x49; a <= 0x4F; a++) {
            Wire.beginTransmission(a);
            if (Wire.endTransmission() == 0) {
                lm75_addr = a;
                lm75_present = true;
                break;
            }
        }
    }

    if (lm75_present) {
        lm75_read_internal();
    }
}

bool lm75_is_available() {
    return lm75_present;
}

float lm75_get_temp() {
    return lm75_temperature;
}

void lm75_loop() {
    unsigned long now = millis();
    if (now - last_temp_read >= 3000) {
        last_temp_read = now;
        
        if (!lm75_present) {
            Wire.beginTransmission(lm75_addr);
            if (Wire.endTransmission() == 0) {
                lm75_present = true;
            }
        }

        if (lm75_present) {
            lm75_read_internal();
        }
    }
}

