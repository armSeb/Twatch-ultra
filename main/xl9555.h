/*
 * xl9555.h — expandeur d'E/S XL9555 (16 broches, I2C 0x20).
 * Sur l'Ultra : alimentation de l'écran et du DRV2605, reset tactile,
 * détection de carte SD, commutateur d'antenne LoRa (voir board.h, XIO_*).
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

esp_err_t xl9555_init(void);                 /* toutes les broches en entrée */
esp_err_t xl9555_output(int pin, bool level); /* passe la broche en sortie */
bool      xl9555_input(int pin);
