/*
 * sdlog.h — journal de la montre sur la carte SD.
 *
 * Chaque ligne du journal ESP-IDF (ESP_LOGx) part toujours sur la console
 * USB, et une copie horodatée va dans un tampon circulaire de 64 Ko en
 * PSRAM. La tâche « sd » le vide toutes les 10 min (plus tôt s'il est à
 * moitié plein, et avant un redémarrage) dans /sd/logs/AAAA-MM-JJ.log (un
 * fichier par jour). Sans carte, le tampon garde les 64 derniers Ko : ils
 * sont écrits dès qu'une carte est insérée.
 */
#pragma once

void sdlog_init(void);      /* le plus tôt possible : capture le démarrage */
void sdlog_attach(void);    /* après sd_init() : branche le vidage sur la carte */
