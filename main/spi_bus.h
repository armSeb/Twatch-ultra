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

/* Voisins du bus : la carte SD (ALDO1) et le NFC (DLDO1). Hors tension,
 * leurs diodes de protection tirent MISO, MOSI et SCK vers la masse et plus
 * personne ne répond sur le bus. Le premier utilisateur (radio ou carte SD)
 * les alimente, CS tenus hauts (désélectionnés) ; le dernier les coupe. */
void spi_peers_get(void);
void spi_peers_put(void);
/* reprend un CS de voisin rendu par son pilote (carte SD démontée) : haut */
void spi_peers_hold_cs(int gpio);
