/*
 * board.h — LilyGo T-Watch Ultra : brochage et constantes matérielles
 *
 * Sources : LilyGoLib (docs/hardware/lilygo-t-watch-ultra.md,
 * variants/lilygo_twatch_ultra/pins_arduino.h, src/LilyGoWatchUltra.cpp),
 * schéma « T-Watch Ultra V1.0 SCH 25-07-24 ».
 */
#pragma once

#include "driver/gpio.h"

/* ---------- Écran AMOLED CO5300 410x502 (QSPI) ---------- */
#define BOARD_LCD_HOST          SPI2_HOST
#define BOARD_LCD_CS            GPIO_NUM_41
#define BOARD_LCD_SCLK          GPIO_NUM_40
#define BOARD_LCD_D0            GPIO_NUM_38
#define BOARD_LCD_D1            GPIO_NUM_39
#define BOARD_LCD_D2            GPIO_NUM_42
#define BOARD_LCD_D3            GPIO_NUM_45
#define BOARD_LCD_RST           GPIO_NUM_37
#define BOARD_LCD_TE            GPIO_NUM_6
#define BOARD_LCD_W             410
#define BOARD_LCD_H             502
/* la dalle est centrée dans la GRAM du CO5300 (colonnes 22 à 431) */
#define BOARD_LCD_GAP_X         22
#define BOARD_LCD_GAP_Y         0
/* 80 MHz : valeur de LilyGoLib (4 lignes → 40 Mo/s, une image pleine en ~10 ms) */
#define BOARD_LCD_PCLK_HZ       (40 * 1000 * 1000)   /* 80 MHz : barres de couleur (MCLite), via la matrice GPIO */
/* Rayon des coins de la dalle (px, estimé : à mesurer sur la montre). Le
 * contenu utile reste dans le rectangle inset de X à gauche/droite et de Y en
 * haut/bas : son coin (X, Y) est à √((R−X)² + (R−Y)²) ≈ 80 px du centre de
 * l'arrondi, donc à l'intérieur (< R). Zone utile 360 × 442. */
#define BOARD_LCD_CORNER_R      84
#define BOARD_SAFE_INSET_X      25
#define BOARD_SAFE_INSET_Y      30

/* ---------- Bus I2C unique ---------- */
#define BOARD_I2C_SDA           GPIO_NUM_3
#define BOARD_I2C_SCL           GPIO_NUM_2
#define BOARD_I2C_HZ            400000

#define BOARD_CST9217_ADDR      0x1A
#define BOARD_XL9555_ADDR       0x20
#define BOARD_BHI260_ADDR       0x28
#define BOARD_AXP2101_ADDR      0x34
#define BOARD_PCF85063_ADDR     0x51
#define BOARD_DRV2605_ADDR      0x5A

/* ---------- Interruptions (toutes actives basses sauf le BHI260AP) ---------- */
#define BOARD_RTC_INT           GPIO_NUM_1
#define BOARD_NFC_INT           GPIO_NUM_5
#define BOARD_PMU_INT           GPIO_NUM_7
#define BOARD_SENSOR_INT        GPIO_NUM_8    /* active haute */
#define BOARD_TOUCH_INT         GPIO_NUM_12
#define BOARD_LORA_IRQ          GPIO_NUM_14   /* DIO1, active haute */
#define BOARD_BOOT_BTN          GPIO_NUM_0    /* bouton BOOT, actif bas */

/* ---------- Expandeur XL9555 (numéros de broche 0..15) ---------- */
#define XIO_DRV_EN              6     /* alimentation DRV2605 */
#define XIO_DISP_EN             7     /* alimentation de l'écran */
#define XIO_TOUCH_RST           8
#define XIO_SD_DET              10    /* carte présente = 1 */
#define XIO_LORA_RF_SW          11    /* commutateur d'antenne LoRa */

/* ---------- Bus SPI partagé : LoRa, NFC, carte SD ---------- */
#define BOARD_SPI_HOST          SPI3_HOST
#define BOARD_SPI_MOSI          GPIO_NUM_34
#define BOARD_SPI_MISO          GPIO_NUM_33
#define BOARD_SPI_SCK           GPIO_NUM_35
#define BOARD_SD_CS             GPIO_NUM_21
#define BOARD_NFC_CS            GPIO_NUM_4
#define BOARD_LORA_CS           GPIO_NUM_36
#define BOARD_LORA_RST          GPIO_NUM_47
#define BOARD_LORA_BUSY         GPIO_NUM_48

/* ---------- Audio ---------- */
#define BOARD_I2S_BCLK          GPIO_NUM_9
#define BOARD_I2S_WCLK          GPIO_NUM_10
#define BOARD_I2S_DOUT          GPIO_NUM_11
#define BOARD_MIC_CLK           GPIO_NUM_17
#define BOARD_MIC_DAT           GPIO_NUM_18

/* ---------- GNSS u-blox MIA-M10Q ---------- */
#define BOARD_GPS_TX            GPIO_NUM_43   /* ESP → GNSS */
#define BOARD_GPS_RX            GPIO_NUM_44   /* GNSS → ESP */
#define BOARD_GPS_PPS           GPIO_NUM_13

/* ---------- Gestion d'énergie ---------- */
/* Écran allumé et interaction : 240 MHz. Au repos, 40 MHz (= quartz,
 * minimum accepté par esp_pm sur ESP32-S3) ; le light sleep automatique prend
 * le relais entre deux événements, BLE compris, grâce au quartz 32 kHz. */
#define POWER_CPU_MAX_MHZ       240
#define POWER_CPU_MIN_MHZ       40

/* Batterie d'origine T-Watch Ultra */
#define BATTERY_CAPACITY_MAH    1100
/* courant de charge : ≤ 0,5 C et sous 500 mA (échauffement de l'AXP2101) */
#define BATTERY_CHARGE_MA       400
