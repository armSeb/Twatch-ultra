/*
 * boot_app.h — app à rouvrir après un redémarrage logiciel volontaire
 * (fin d'une session de réglages sur la page web de la montre).
 *
 * Index dans g_apps gardé en RAM RTC : posé juste avant esp_restart(), lu une
 * seule fois au démarrage. Un plantage ou une coupure l'efface.
 */
#pragma once

void boot_app_init(void);        /* tout début d'app_main */
void boot_app_set(int idx);      /* -1 = aucune */
int  boot_app_get(void);         /* -1 = aucune */
