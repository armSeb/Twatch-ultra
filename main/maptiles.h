/*
 * maptiles.h — tuiles de carte OpenStreetMap lues sur la carte SD.
 *
 * Format des cartes de MCLite et du firmware MeshCore du T-Deck :
 * /sd/tiles/<z>/<x>/<y>.png, tuiles « slippy map » de 256 × 256 (512 × 512
 * accepté, réduit). Les niveaux de zoom présents sont détectés à l'ouverture.
 *
 * Une tâche « tiles » (pile en PSRAM : la carte SD n'est pas la flash) lit et
 * décode les tuiles demandées, la plus prioritaire d'abord, dans un cache
 * d'images RGB565 en PSRAM (≈ 128 Ko par tuile). Une tuile absente est
 * fabriquée en agrandissant un morceau d'une tuile d'un niveau plus large
 * (jusqu'à 4 niveaux au-dessus) : on peut zoomer au-delà des niveaux
 * téléchargés, et un trou dans la carte reste lisible.
 *
 * Tout se fait depuis la tâche app (LVGL), sauf mention contraire.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#define MT_TILE      256
#define MT_ZMAX      20
#define MT_UPSCALE   4                 /* niveaux d'agrandissement au plus */

typedef enum {
    MT_SCANNING,        /* recherche des niveaux sur la carte SD */
    MT_NO_SD,           /* pas de carte montée */
    MT_NO_TILES,        /* carte sans dossier /tiles utilisable */
    MT_READY,
    MT_NO_MEM,          /* PSRAM insuffisante */
} mt_state_t;

bool       mt_open(void);              /* cache + tâche ; false : mémoire */
void       mt_close(void);             /* attend la tâche, libère tout */
mt_state_t mt_state(void);
void       mt_rescan(void);            /* carte remise : relire les niveaux */
/* niveaux présents (bit z) ; zone couverte au niveau le plus large, en
 * coordonnées Mercator normalisées 0..1 (centre de la carte par défaut) */
uint32_t   mt_zoom_mask(void);
bool       mt_center(double *mx, double *my);

/* Nouvelle vue : oublie les demandes en attente qui n'y figurent plus.
 * Puis mt_want() pour chaque tuile visible, la plus importante d'abord. */
void       mt_begin(void);
void       mt_want(int z, int x, int y);
/* image prête (NULL sinon) ; *approx : agrandie d'un niveau plus large */
const lv_image_dsc_t *mt_peek(int z, int x, int y, bool *approx);
/* tuile connue absente (ni elle ni un niveau plus large) */
bool       mt_missing(int z, int x, int y);
/* true une fois après chaque tuile terminée (à redessiner) */
bool       mt_take_dirty(void);
