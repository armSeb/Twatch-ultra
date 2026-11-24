/*
 * sdcard.h — carte micro-SD (bus SPI partagé avec la radio LoRa et le NFC).
 *
 * Une tâche « sd » surveille la présence de la carte (contact SD_DET sur le
 * XL9555, dont l'interruption n'est pas câblée : lu toutes les 3 s, 15 s
 * écran éteint), monte
 * la carte en FAT sur /sd à l'insertion et la démonte à l'arrachage.
 * « Éjecter » vide les tampons (journaux, archive MeshCore) puis démonte :
 * la carte reste démontée jusqu'à son retrait, ou jusqu'à sd_mount().
 *
 * Toute lecture ou écriture sur /sd se fait sous sd_lock(), après avoir
 * vérifié sd_mounted() : la carte peut disparaître à tout moment.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define SD_ROOT "/sd"

typedef enum {
    SD_ABSENT,          /* pas de carte */
    SD_MOUNTED,         /* montée sur /sd */
    SD_EJECTED,         /* présente, démontée à la demande */
    SD_FAILED,          /* présente, illisible (pas en FAT, abîmée…) */
    SD_SHARED,          /* prêtée au PC par l'USB (stockage de masse), démontée côté montre */
} sd_state_t;

typedef struct {
    sd_state_t state;
    uint64_t   total, free;     /* octets ; 0 si inconnu */
    char       name[8];         /* nom donné par le fabricant */
} sd_info_t;

void       sd_init(void);
void       sd_get_info(sd_info_t *out);
bool       sd_mounted(void);
esp_err_t  sd_eject(void);      /* tâche app : vide les tampons, démonte */
esp_err_t  sd_mount(void);      /* remonte une carte éjectée */
void       sd_refresh_info(void);   /* recalcule l'espace libre (page Carte SD ouverte) */

/* Partage avec le PC (câble USB branché) : la carte est démontée côté montre
 * et présentée au PC comme une clé USB ; la console USB est coupée pendant
 * ce temps. Le partage cesse sur sd_share(false), quand le PC éjecte le
 * disque, au débranchement du câble ou au retrait de la carte ; la carte est
 * alors remontée. Journaux et messages attendent en mémoire. */
bool       sd_can_share(void);
esp_err_t  sd_share(bool on);
bool       sd_share_host_seen(void);    /* le PC a reconnu le disque */

bool sd_lock(void);             /* false : pas de carte montée (rien n'est pris) */
void sd_unlock(void);

/* Tampons à vider sur la carte : appelés par la tâche « sd » toutes les
 * 10 min et avant un démontage ou un partage, carte montée (sd_lock() non
 * tenu). */
typedef void (*sd_flush_cb_t)(void);
void sd_add_flusher(sd_flush_cb_t cb);
void sd_flush_soon(void);       /* demande un vidage rapide (n'importe quelle tâche) */
void sd_flush_wait(int ms);     /* vidage tout de suite, attendu (avant un redémarrage) */
