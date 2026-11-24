/*
 * power.h — politique d'énergie : PM/DFS + light sleep, batterie (jauge de
 * l'AXP2101), luminosité de l'écran, vibreur.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    int   vbat_mv;
    int   vbus_mv;
    bool  vbus;
    bool  charging;
    bool  charge_done;
    int   percent;
    float temp_c;          /* température de l'AXP2101 */
    uint32_t since_s;      /* durée de la période sur batterie (ou de la charge) */
    int   pct_start;       /* % au début de la période */
    float avg_ma;          /* estimé d'après la baisse du %, 0 si trop tôt */
    float screen_pct;      /* part du temps écran allumé */
    /* light sleep, mesuré depuis le début de la période (écran éteint) :
     * part du temps écran éteint passée en sommeil, et réveils par minute */
    bool  sleep_known;     /* false : mesure indisponible ou trop courte */
    float sleep_pct;
    float wakes_per_min;
} power_telemetry_t;

esp_err_t power_init(void);           /* AXP2101, XL9555, vibreur, PM, IRQ, capteur */

/* Verrous PM (comptés). « active » = interaction : CPU max + pas de light sleep. */
void power_hold_active(bool hold);
void power_hold_nosleep(bool hold);
/* plafond CPU (MHz, 80/160/240 ; 0 = défaut) : 160 pendant les apps radio */
void power_set_cpu_max(int mhz);
void power_hold_cpu_floor(bool hold);   /* compteur : CPU >= POWER_CPU_FLOOR_MHZ au repos */

/* à appeler sur EV_AXP_IRQ depuis la tâche app ; retourne le masque d'IRQ */
uint32_t power_handle_irq(void);
/* rafraîchit l'estimation batterie (≈ toutes les 10–60 s) */
void power_update(void);
int  power_percent(void);
bool power_vbus(void);
bool power_charging(void);
void power_get_telemetry(power_telemetry_t *t);

/* appelé par display.c à chaque allumage/extinction (part du temps écran) */
void power_screen_segment(bool on);

/* Décharge précédente (historique NVS) */
enum { POWER_HIST_RUNNING = 0, POWER_HIST_CHARGED = 1, POWER_HIST_EMPTY = 2, POWER_HIST_REBOOT = 3 };
typedef struct {
    uint32_t since_s;
    uint16_t vbat_mv;        /* dernière tension enregistrée */
    uint8_t  pct_start, pct_end;
    uint8_t  screen_pct;
    uint8_t  end;            /* POWER_HIST_* */
} power_hist_t;
bool     power_last_discharge(power_hist_t *out);

/* luminosité de l'écran 0..255 (0 = noir) */
void power_backlight(uint8_t level);

/* vibreur : motif = durées alternées ON/OFF en ms, terminé par 0 */
void power_vibrate(const uint16_t *pattern);
extern const uint16_t VIB_NOTIF[];
extern const uint16_t VIB_CALL[];
extern const uint16_t VIB_SHORT[];
