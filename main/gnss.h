/*
 * gnss.h — GNSS u-blox MIA-M10Q (GPS, Galileo, BeiDou, QZSS, SBAS ;
 * GLONASS selon la configuration du module).
 *
 * Coupé (réglage par défaut) : rail BLDO1 éteint, broches en haute
 * impédance, aucune tâche : rien ne consomme. La sauvegarde (VRTC, toujours
 * alimentée) garde l'almanach et l'heure : le fix suivant revient vite.
 *
 * Allumé : une tâche lit les trames NMEA (UART1, 38 400 bauds). Pour ne pas
 * empêcher le light sleep, la lecture se fait par fenêtres : toutes les 30 s
 * l'ESP32 reste éveillé le temps de deux trames (≈ 2 s). L'écran GPS ouvert
 * (gnss_live) la lecture est continue.
 *
 * Fonctions gnss_set/gnss_init : tâche app seulement (NVS, rail AXP).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "gnss_nmea.h"

typedef struct {
    bool     on;           /* alimenté */
    bool     detected;     /* le module répond (trames valides) */
    uint32_t on_s;         /* secondes depuis l'allumage */
    int32_t  ttff_s;       /* délai du premier fix depuis l'allumage, -1 pas encore */
    uint32_t fix_age_s;    /* âge du dernier fix lu, UINT32_MAX aucun */
    gnss_state_t st;
} gnss_info_t;

void gnss_init(void);              /* au démarrage : applique le réglage */
void gnss_set(bool on);            /* réglage mémorisé + alimentation */
bool gnss_is_on(void);
void gnss_live(bool live);         /* écran GPS affiché : lecture continue */
void gnss_get(gnss_info_t *out);   /* copie (≈ 800 o : prévoir un tampon statique) */
/* dernière position, si le fix a moins de max_age_s secondes */
bool gnss_position(double *lat, double *lon, float *alt_m, bool *alt_ok, uint32_t max_age_s);
