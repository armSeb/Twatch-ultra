/*
 * tusb_config.h — TinyUSB pour la montre : périphérique USB pleine vitesse,
 * une seule classe, le stockage de masse (carte SD partagée avec le PC).
 *
 * Mode « esclave » du contrôleur (sans DMA) : le processeur recopie les
 * paquets lui-même, ce qui permet de garder tous les tampons de TinyUSB en
 * PSRAM ; le débit reste limité par la carte (SPI 20 MHz), pas par là.
 */
#pragma once
#include "sdkconfig.h"
#include "esp_attr.h"

#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU                OPT_MCU_ESP32S3
#endif
#define CFG_TUSB_OS                 OPT_OS_FREERTOS
#define CFG_TUSB_OS_INC_PATH        freertos/
#define CFG_TUSB_DEBUG              0

#define CFG_TUD_ENABLED             1
#define CFG_TUD_MAX_SPEED           OPT_MODE_FULL_SPEED
#define CFG_TUD_DWC2_DMA_ENABLE     0
#define CFG_TUD_MEM_SECTION         EXT_RAM_BSS_ATTR
#define CFG_TUD_MEM_ALIGN           __attribute__((aligned(4)))
#define CFG_TUSB_MEM_ALIGN          __attribute__((aligned(4)))
#define CFG_TUD_MEM_DCACHE_ENABLE   0
#define CFG_TUD_ENDPOINT0_SIZE      64

#define CFG_TUD_CDC                 0
#define CFG_TUD_MSC                 1
#define CFG_TUD_HID                 0
#define CFG_TUD_MIDI                0
#define CFG_TUD_VENDOR              0
#define CFG_TUD_MSC_EP_BUFSIZE      4096    /* 8 secteurs par échange avec la carte */
