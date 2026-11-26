/*
 * display.h — AMOLED CO5300 410x502 (esp_lcd, QSPI DMA) + CST9217 + glue LVGL 9.
 *
 * Pas d'esp_lvgl_port volontairement : son timer de tick périodique et sa
 * tâche dédiée réveilleraient le CPU en permanence. Ici le tick LVGL est lu
 * à la demande (esp_timer_get_time) et lv_timer_handler n'est appelé que
 * par la tâche app, uniquement écran allumé.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t display_init(void);          /* LCD + tactile + lv_init + display/indev */
void      display_set_on(bool on);      /* séquence complète veille/réveil */
void      display_panel_brightness(uint8_t level);   /* registre 0x51 brut (power_backlight) */
/* « coup d'œil » (lever le poignet) : luminosité réduite, CPU laissé au
 * minimum (pas de verrou CPU max) ; le premier toucher n'est pas transmis à
 * l'interface, il demande le réveil complet (display_glance_take_touch). */
void      display_set_glance(uint8_t level);   /* allume en coup d'œil */
bool      display_is_glance(void);
void      display_glance_end(void);     /* passe en réveil complet (luminosité, CPU) */
bool      display_glance_take_touch(void); /* true une fois si un toucher a demandé le réveil */
bool      display_is_on(void);
void      display_set_brightness(uint8_t level);
void      display_perf_tick(void);
/* réveil par double tap (écran éteint) */
bool      display_tap_begin(void);      /* sur EV_TOUCH_INT : démarre l'analyse */
int       display_tap_poll(void);       /* 1 = double tap, 0 = continuer, -1 = abandon */
uint32_t  display_tap_sample_ms(void);
/* Affichage direct pour les jeux, sans LVGL : fill() remplit des bandes de
 * lignes (RGB565 natif) envoyées à l'écran au fur et à mesure, pendant que la
 * suivante se prépare. Deux bandes de 26 lignes en RAM DMA, libérées par
 * display_direct_end(). false si l'écran est éteint ou la mémoire manque.
 * Chaque image commence sur le signal TE de la dalle. */
typedef void (*display_strip_fn)(uint16_t *px, int y0, int lines, void *ctx);
bool      display_direct(display_strip_fn fill, void *ctx);
void      display_direct_end(void);
/* Fluidité de la dernière seconde animée */
typedef struct {
    uint16_t fps;          /* images par seconde pendant l'animation */
    uint32_t avg_us;       /* rafraîchissement moyen : mise en page, dessin, envoi */
    uint32_t max_us;
} display_perf_t;
void      display_get_perf(display_perf_t *p);
/* banc de mesure : LVGL dessine sans rien envoyer à la dalle (temps de rendu pur) */
void      display_bench_mode(bool on);
/* Observateur du tactile (tâche app, à chaque lecture LVGL) : reçoit ce qui
 * est transmis à LVGL, après le filtre du coup d'œil. Sert au tirage depuis
 * le bord haut (ui.c), qui doit voir le doigt avant les objets de l'écran. */
typedef void (*display_touch_hook_t)(bool down, int x, int y);
void      display_set_touch_hook(display_touch_hook_t hook);
/* second doigt posé (le CST9217 en suit deux) : position, tâche app */
bool      display_touch_second(int *x, int *y);
void      display_tap_enable(bool en);  /* réglage modifié */      /* CPU max seulement pendant l'interaction */
