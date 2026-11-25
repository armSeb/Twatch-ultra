/*
 * spi_bus.h — bus SPI partagé (SPI3) : LoRa SX1262, NFC, carte SD.
 *
 * Le bus n'existe que tant qu'un pilote l'utilise : le premier qui le prend
 * l'initialise, le dernier qui le rend le libère et remet MOSI/MISO/SCK en
 * haute impédance (les puces du bus peuvent être sans alimentation).
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

esp_err_t spi_shared_get(void);
void      spi_shared_put(void);

/* Voisins du bus : la carte SD (ALDO1) et le NFC (DLDO1). Hors tension,
 * leurs diodes de protection peuvent tirer MISO, MOSI et SCK vers la masse
 * et plus personne ne répond sur le bus. Le premier utilisateur (radio ou
 * carte SD) les alimente, CS tenus hauts (désélectionnés) ; le dernier les
 * coupe. Le rail SD peut être coupé radio en marche si la radio répond
 * encore sans lui (spi_sd_try_off) : une carte au repos tire jusqu'à des
 * dizaines de mA. */
void spi_peers_get(void);
void spi_peers_put(void);
/* reprend un CS de voisin rendu par son pilote (carte SD démontée) : haut */
void spi_peers_hold_cs(int gpio);

/* carte SD en service (montée, écriture) : rail SD allumé, CS haut */
void spi_sd_get(void);
void spi_sd_put(void);
/* Coupure du rail SD radio en marche. La radio enregistre un « lanceur »
 * qui exécute spi_sd_try_off_run(sonde) dans sa propre tâche (personne
 * d'autre ne parle sur le bus pendant l'essai). spi_sd_try_off() demande
 * l'essai (asynchrone, sans effet s'il est déjà fait). Si la sonde échoue,
 * ou si la radio devient muette plus tard (spi_sd_bus_failed), le rail SD
 * reste allumé jusqu'au redémarrage. */
void spi_set_sd_off_runner(void (*run)(void));
void spi_sd_try_off(void);
void spi_sd_try_off_run(bool (*check)(void));
void spi_sd_bus_failed(void);
bool spi_sd_powered(void);
