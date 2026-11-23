/*
 * spi_bus.h — bus SPI partagé (SPI3) : LoRa SX1262, NFC, carte SD.
 *
 * Le bus n'existe que tant qu'un pilote l'utilise : le premier qui le prend
 * l'initialise, le dernier qui le rend le libère et remet MOSI/MISO/SCK en
 * haute impédance (les puces du bus peuvent être sans alimentation).
 */
#pragma once
#include "esp_err.h"

esp_err_t spi_shared_get(void);
void      spi_shared_put(void);
