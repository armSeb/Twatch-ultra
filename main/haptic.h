/*
 * haptic.h — vibreur : moteur ERM piloté par un DRV2605 (I2C 0x5A, broche
 * EN sur l'expandeur XL9555). Le moteur est commandé en temps réel (mode
 * RTP) : les motifs ON/OFF de power_vibrate() gardent leur rythme exact.
 * Hors vibration, la puce reste en veille (standby, quelques µA).
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

esp_err_t haptic_init(void);
void      haptic_set(bool on);      /* appelable depuis un esp_timer */
