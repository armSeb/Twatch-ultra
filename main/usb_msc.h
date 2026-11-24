/*
 * usb_msc.h — la carte SD vue comme une clé USB par le PC.
 *
 * Le port USB-C sert d'habitude à la console série (USB Serial/JTAG) ;
 * pendant le partage, le PHY USB est confié au contrôleur OTG qui présente
 * la carte en stockage de masse. À l'arrêt, le PHY revient à la console.
 *
 * Le pilote ne connaît que des secteurs : la tâche « sd » lui donne la carte
 * démontée côté montre (pas de FAT ouverte des deux côtés à la fois).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t sectors;                                   /* nombre de secteurs de 512 octets */
    esp_err_t (*read)(void *dst, uint32_t lba, uint32_t n);
    esp_err_t (*write)(const void *src, uint32_t lba, uint32_t n);
    void (*ejected)(void);      /* le PC a éjecté le disque (tâche USB) */
} usb_msc_media_t;

esp_err_t usb_msc_start(const usb_msc_media_t *m);
void      usb_msc_stop(void);    /* attend la fin de la tâche USB */
bool      usb_msc_active(void);
bool      usb_msc_host_seen(void);   /* le PC a configuré le périphérique */
