/*
 * apps.c — registre des apps (ordre du lanceur).
 * Ajouter une app : écrire app_xxx.c avec un `const app_t app_xxx`, puis
 * l'ajouter ici. Les entrées système (Lecture, Énergie, Réglages) sont
 * ajoutées par le lanceur lui-même (ui.c).
 */
#include "app.h"

extern const app_t app_chrono;
extern const app_t app_timer;
extern const app_t app_lamp;
extern const app_t app_steps;
extern const app_t app_puca;
extern const app_t app_demo;
extern const app_t app_flight;
extern const app_t app_mouse;
extern const app_t app_wifi;
extern const app_t app_webrx;
extern const app_t app_tone;
extern const app_t app_radio;
extern const app_t app_mesh;

const app_t *const g_apps[] = {
    &app_mesh,
    &app_chrono,
    &app_timer,
    &app_steps,
    &app_puca,
    &app_demo,
    &app_flight,
    &app_lamp,
    &app_mouse,
    &app_wifi,
    &app_radio,
    &app_webrx,
    &app_tone,
};
const int g_app_count = sizeof(g_apps) / sizeof(g_apps[0]);
