/*
 * axp2101.h — pilote minimal du PMIC AXP2101 (I2C brut, sans bibliothèque).
 * Registres vérifiés contre XPowersLib / SensorLib (lewisxhe, MIT).
 *
 * Rails de la T-Watch Ultra :
 *   DCDC1 3,3 V ESP32-S3 (jamais coupé)      ALDO1 3,3 V carte SD
 *   ALDO2 3,3 V écran                        ALDO3 3,3 V LoRa
 *   ALDO4 1,8 V BHI260AP                     BLDO1 3,3 V GNSS
 *   BLDO2 3,3 V ampli MAX98357A              DLDO1 3,3 V NFC
 *   VRTC (LDO1) sauvegarde du GNSS, toujours allumé
 *   VBACKUP : charge de la pile de sauvegarde de la RTC
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    AXP_RAIL_SD = 0,      /* ALDO1 */
    AXP_RAIL_DISPLAY,     /* ALDO2 */
    AXP_RAIL_LORA,        /* ALDO3 */
    AXP_RAIL_SENSOR,      /* ALDO4 */
    AXP_RAIL_GPS,         /* BLDO1 */
    AXP_RAIL_SPEAKER,     /* BLDO2 */
    AXP_RAIL_NFC,         /* DLDO1 */
    AXP_RAIL_COUNT,
} axp_rail_t;

/* Sources d'IRQ (bit n = registre 0x48 + n/8, bit n%8) */
#define AXP_IRQ_SOC_WARN2       (1u << 7)    /* niveau d'alerte 2 (5 %) */
#define AXP_IRQ_SOC_WARN1       (1u << 6)    /* niveau d'alerte 1 (15 %) */
#define AXP_IRQ_PEK_LONG        (1u << 10)
#define AXP_IRQ_PEK_SHORT       (1u << 11)
#define AXP_IRQ_VBUS_REMOVED    (1u << 14)
#define AXP_IRQ_VBUS_CONNECT    (1u << 15)
#define AXP_IRQ_CHG_START       (1u << 19)
#define AXP_IRQ_CHG_DONE        (1u << 20)

esp_err_t axp_init(void);                 /* rails, charge, ADC, jauge, bouton */
esp_err_t axp_rail(axp_rail_t rail, bool on);
bool      axp_rail_is_on(axp_rail_t rail);
esp_err_t axp_irq_enable(uint32_t mask);  /* active exactement ce masque */
uint32_t  axp_irq_read_and_clear(void);
bool      axp_vbus_present(void);
bool      axp_is_charging(void);
bool      axp_charge_done(void);
void      axp_charge_enable(bool on);     /* coupée : le câble alimente la montre, batterie au repos */
int       axp_batt_mv(void);              /* 0 si batterie absente */
int       axp_vbus_mv(void);
int       axp_vsys_mv(void);
int       axp_percent(void);              /* jauge interne, -1 si inconnue */
float     axp_die_temp_c(void);
uint8_t   axp_reg(uint8_t reg);           /* lecture brute (diagnostic) */
void      axp_power_off(void);            /* coupure complète (comme 6 s sur le bouton) */
