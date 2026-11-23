/*
 * sx1262.h — pilote minimal de la radio LoRa SX1262 (SPI brut, sans RadioLib).
 *
 * Une tâche « lora » (cœur 0) possède la puce : elle attend l'interruption
 * DIO1 (réception ou fin d'émission, aussi source de réveil du light sleep)
 * ou une demande d'émission, et revient toujours en réception. Les paquets
 * reçus sont passés à un rappel, appelé depuis cette tâche.
 *
 * Coupé (sx_stop) : rail ALDO3 éteint, broches en haute impédance, la radio
 * ne consomme rien.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define SX_MAX_PKT 255

typedef struct {
    uint32_t freq_hz;      /* 150–960 MHz */
    uint32_t bw_hz;        /* 7800 … 500000 (valeurs LoRa standard) */
    uint8_t  sf;           /* 5–12 */
    uint8_t  cr;           /* 5–8 : 4/5 … 4/8 */
    int8_t   power_dbm;    /* −9 … +22 */
    uint8_t  sync_word;    /* 0x12 privé (MeshCore), 0x34 public (LoRaWAN) */
    uint16_t preamble;     /* symboles */
} sx_cfg_t;

typedef void (*sx_rx_cb_t)(const uint8_t *buf, size_t len, int16_t rssi_dbm, int8_t snr_q4);

esp_err_t sx_start(const sx_cfg_t *cfg, sx_rx_cb_t cb);   /* allume, configure, réception */
void      sx_stop(void);
bool      sx_running(void);
esp_err_t sx_set_config(const sx_cfg_t *cfg);              /* à chaud */
/* met le paquet en file d'émission (copié) ; l'émission attend la fin d'une
 * réception en cours. false : radio arrêtée ou file pleine. */
bool      sx_send(const uint8_t *buf, size_t len);
/* true : un préambule ou un en-tête a été vu, le paquet n'est pas fini */
bool      sx_rx_active(void);
/* true : la file d'émission est vide et aucune émission n'est en cours */
bool      sx_tx_idle(void);
/* durée d'émission (ms) d'un paquet de len octets avec la config courante */
uint32_t  sx_airtime_ms(size_t len);
/* bruit de fond mesuré entre deux paquets (dBm), INT16_MIN si inconnu */
int16_t   sx_noise_floor(void);

/* compteurs (diagnostic) */
typedef struct { uint32_t rx_ok, rx_crc, tx_ok, tx_fail; } sx_stats_t;
void      sx_get_stats(sx_stats_t *out);
