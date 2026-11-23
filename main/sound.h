/*
 * sound.h — bips et flux audio sur le haut-parleur (ampli I2S MAX98357A sur
 * BLDO2 : BCLK 9, WCLK 10, DOUT 11). L'ampli et le périphérique I2S
 * n'existent que le temps du son : rien ne consomme au repos.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Volume général (centre de contrôle) : tous les sons passent par ici et
 * sont multipliés par ce gain, en plus du volume propre à l'app (radio…).
 * pct 0..100 ; muted = mode silencieux (aucun son, l'ampli reste coupé).
 * Appelable de n'importe quelle tâche. */
void sound_set_master(int pct, bool muted);
bool sound_muted(void);            /* silencieux, ou volume à 0 */

/* n bips de freq_hz, on_ms / off_ms ; ignoré si un son est déjà en cours.
 * Non bloquant, appelable depuis la tâche app. */
void sound_beeps(int n, int freq_hz, int on_ms, int off_ms);
bool sound_busy(void);
int  sound_last_error(void);       /* 0 = dernier son joué, sinon esp_err_t */

/* Flux audio continu (une seule tâche écrivain). open : ampli + I2S à `rate`
 * Hz ; write : échantillons mono 16 bits, gain en Q12 (4096 = ×1), bloquant
 * au rythme du son ; mono NULL = silence. Les bips sont ignorés pendant un flux. */
esp_err_t sound_stream_open(int rate);
/* idem, hors volume général et mode silencieux : réservé aux sons émis à la
 * demande pour un appareil tiers (tonalité d'ouverture de relais) */
esp_err_t sound_stream_open_fixed(int rate);
void      sound_stream_write(const int16_t *mono, int n, int gain_q12);
void      sound_stream_close(void);
