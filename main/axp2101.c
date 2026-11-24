/* axp2101.c — voir axp2101.h */
#include "axp2101.h"
#include "board.h"
#include "i2c_bus.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "axp2101";

#define REG_STATUS1        0x00
#define REG_STATUS2        0x01
#define REG_COMMON_CFG     0x10
#define REG_FUEL_RESET     0x17
#define REG_MODULE_EN      0x18   /* b3 jauge, b2 pile RTC, b1 charge, b0 chien de garde */
#define REG_LOWBAT_WARN    0x1A
#define REG_VOFF           0x24
#define REG_PWRON_TIME     0x27   /* b1:0 durée d'allumage, b3:2 durée de coupure */
#define REG_ADC_EN         0x30
#define REG_ADC_VBAT_H     0x34
#define REG_ADC_VBUS_H     0x38
#define REG_ADC_VSYS_H     0x3A
#define REG_ADC_TDIE_H     0x3C
#define REG_INTEN1         0x40
#define REG_INTSTS1        0x48
#define REG_IPRECHG        0x61
#define REG_ICC            0x62
#define REG_ITERM          0x63
#define REG_CV             0x64
#define REG_CHGLED         0x69
#define REG_BTN_CHG_V      0x6A
#define REG_DC_ONOFF       0x80
#define REG_LDO_ONOFF0     0x90
#define REG_LDO_ONOFF1     0x91   /* b0 DLDO2 */
#define REG_LDO_VOL0       0x92   /* ALDO1 ; +1 par LDO, dans l'ordre des bits de 0x90 */
#define REG_BAT_PARAMS     0xA1
#define REG_GAUGE_CTRL     0xA2
#define REG_PERCENT        0xA4

static i2c_master_dev_handle_t s_dev;

/* rail → (bit dans 0x90, registre de tension, mV) */
static const struct { uint8_t bit, vreg; uint16_t mv; } RAIL[AXP_RAIL_COUNT] = {
    [AXP_RAIL_SD]      = { 0, 0x92, 3300 },
    [AXP_RAIL_DISPLAY] = { 1, 0x93, 3300 },
    [AXP_RAIL_LORA]    = { 2, 0x94, 3300 },
    [AXP_RAIL_SENSOR]  = { 3, 0x95, 1800 },
    [AXP_RAIL_GPS]     = { 4, 0x96, 3300 },
    [AXP_RAIL_SPEAKER] = { 5, 0x97, 3300 },
    [AXP_RAIL_NFC]     = { 7, 0x99, 3300 },
};

/* Paramètres de la cellule 1100 mAh pour la jauge interne (modèle de batterie
 * fourni par LilyGo, LilyGoLib/src/LilyGoWatchUltra.cpp, MIT). */
static const uint8_t BAT_PARAMS[128] = {
    0x01, 0xf5, 0x40, 0x00, 0x1b, 0x1e, 0x28, 0x0f, 0x0c, 0x1e, 0x32, 0x02, 0x14, 0x05, 0x0a, 0x04,
    0x74, 0xfc, 0xf4, 0x0d, 0x43, 0x10, 0x52, 0xfb, 0xa6, 0x01, 0xea, 0x04, 0x64, 0x06, 0x52, 0x06,
    0x18, 0x0a, 0xe7, 0x0f, 0x9f, 0x0f, 0x51, 0x09, 0xf7, 0x0e, 0x89, 0x0e, 0x71, 0x04, 0x58, 0x04,
    0x43, 0x09, 0x32, 0x0e, 0x1c, 0x0e, 0x14, 0x09, 0x04, 0x0d, 0xe9, 0x0d, 0xde, 0x03, 0xc8, 0x03,
    0xb3, 0x08, 0x9d, 0x0d, 0x79, 0x0d, 0x3a, 0x07, 0xf5, 0x9e, 0x56, 0x47, 0x36, 0x20, 0x24, 0x17,
    0xc5, 0x98, 0x7e, 0x66, 0x4e, 0x44, 0x38, 0x1a, 0x12, 0x0a, 0xf6, 0x00, 0x00, 0xf6, 0x00, 0xf6,
    0x00, 0xfb, 0x00, 0x00, 0xfb, 0x00, 0x00, 0xfb, 0x00, 0x00, 0xf6, 0x00, 0x00, 0xf6, 0x00, 0xf6,
    0x00, 0xfb, 0x00, 0x00, 0xfb, 0x00, 0x00, 0xfb, 0x00, 0x00, 0xf6, 0x00, 0x00, 0xf6, 0x00, 0xf6,
};
#define BAT_PARAMS_VER 1

static uint8_t rd(uint8_t reg)
{
    uint8_t v = 0;
    i2c_reg_read8(s_dev, reg, &v);
    return v;
}

static void wr(uint8_t reg, uint8_t v) { i2c_reg_write8(s_dev, reg, v); }
static void upd(uint8_t reg, uint8_t mask, uint8_t v) { i2c_reg_update8(s_dev, reg, mask, v); }

static int adc14(uint8_t reg_h, uint8_t hmask)
{
    uint8_t b[2];
    if (i2c_reg_read(s_dev, reg_h, b, 2) != ESP_OK) return 0;
    return ((b[0] & hmask) << 8) | b[1];
}

/* Modèle de batterie : écrit une fois (il survit tant que la batterie reste
 * branchée, et la jauge apprend ensuite d'elle-même). */
static void gauge_params_once(void)
{
    nvs_handle_t h;
    uint8_t ver = 0;
    if (nvs_open("tw", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_get_u8(h, "axp_bp", &ver);
    if (ver != BAT_PARAMS_VER) {
        upd(REG_FUEL_RESET, 0x04, 0x04); upd(REG_FUEL_RESET, 0x04, 0x00);
        upd(REG_GAUGE_CTRL, 0x01, 0x00); upd(REG_GAUGE_CTRL, 0x01, 0x01);
        for (int i = 0; i < 128; i++) wr(REG_BAT_PARAMS, BAT_PARAMS[i]);
        upd(REG_GAUGE_CTRL, 0x01, 0x00);
        upd(REG_GAUGE_CTRL, 0x10, 0x10);              /* paramètres pris dans la ROM écrite */
        upd(REG_FUEL_RESET, 0x04, 0x04); upd(REG_FUEL_RESET, 0x04, 0x00);
        nvs_set_u8(h, "axp_bp", BAT_PARAMS_VER);
        nvs_commit(h);
        ESP_LOGI(TAG, "modèle de batterie écrit dans la jauge");
    }
    nvs_close(h);
}

esp_err_t axp_init(void)
{
    esp_err_t e = i2c_bus_add(BOARD_AXP2101_ADDR, 0, &s_dev);
    if (e != ESP_OK) return e;
    ESP_LOGI(TAG, "AXP2101 id 0x%02x", rd(0x03));

    /* L'AXP garde ses registres tant que la batterie est branchée : tout est
     * réglé explicitement, quel que soit le firmware précédent. */
    for (int r = 0; r < AXP_RAIL_COUNT; r++)
        wr(RAIL[r].vreg, (uint8_t)((RAIL[r].mv - 500) / 100));
    /* au démarrage : écran et capteur seulement, le reste à la demande */
    uint8_t on = (1u << RAIL[AXP_RAIL_DISPLAY].bit) | (1u << RAIL[AXP_RAIL_SENSOR].bit);
    wr(REG_LDO_ONOFF0, on);
    upd(REG_LDO_ONOFF1, 0x01, 0x00);                   /* DLDO2 : relié à rien (schéma V1.0) */
    upd(REG_DC_ONOFF, 0x1E, 0x00);                     /* DCDC2..5 coupés, DCDC1 intact */

    upd(REG_VOFF, 0x07, 0x03);                         /* coupure système à 2,9 V */
    wr(REG_LOWBAT_WARN, (uint8_t)(((15 - 5) << 4) | 5)); /* alerte 15 %, arrêt 5 % */

    /* charge : 4,2 V, 400 mA (≤ 0,5 C, sous 500 mA pour l'échauffement),
     * pré-charge 100 mA, fin de charge 25 mA */
    upd(REG_CV, 0x07, 0x03);
    upd(REG_ICC, 0x1F, BATTERY_CHARGE_MA <= 200 ? (uint8_t)(BATTERY_CHARGE_MA / 25)
                                               : (uint8_t)(8 + (BATTERY_CHARGE_MA - 200) / 100));
    upd(REG_IPRECHG, 0x0F, 4);
    upd(REG_ITERM, 0x1F, 0x11);
    /* pile de la RTC : charge à 3,3 V */
    upd(REG_BTN_CHG_V, 0x07, 7);
    upd(REG_MODULE_EN, 0x0F, 0x0E);                    /* jauge + pile RTC + charge, sans chien de garde */
    /* LED de charge éteinte (contrôle manuel, haute impédance) */
    upd(REG_CHGLED, 0x37, 0x05);

    /* ADC : tension batterie, NTC, VBUS, VSYS, température */
    wr(REG_ADC_EN, 0x1F);

    /* bouton : allumage 1 s, coupure 6 s */
    upd(REG_PWRON_TIME, 0x0F, (1u << 2) | 2);

    gauge_params_once();
    return ESP_OK;
}

esp_err_t axp_rail(axp_rail_t rail, bool on)
{
    if (rail >= AXP_RAIL_COUNT) return ESP_ERR_INVALID_ARG;
    uint8_t m = 1u << RAIL[rail].bit;
    return i2c_reg_update8(s_dev, REG_LDO_ONOFF0, m, on ? m : 0);
}

bool axp_rail_is_on(axp_rail_t rail)
{
    return rail < AXP_RAIL_COUNT && (rd(REG_LDO_ONOFF0) >> RAIL[rail].bit) & 1;
}

esp_err_t axp_irq_enable(uint32_t mask)
{
    esp_err_t err = ESP_OK;
    for (int i = 0; i < 3; i++) {
        err |= i2c_reg_write8(s_dev, REG_INTEN1 + i, (uint8_t)(mask >> (8 * i)));
        err |= i2c_reg_write8(s_dev, REG_INTSTS1 + i, 0xFF);
    }
    return err;
}

uint32_t axp_irq_read_and_clear(void)
{
    uint8_t st[3] = { 0 };
    if (i2c_reg_read(s_dev, REG_INTSTS1, st, 3) != ESP_OK) return 0;
    for (int i = 0; i < 3; i++)
        if (st[i]) i2c_reg_write8(s_dev, REG_INTSTS1 + i, st[i]);   /* W1C */
    return st[0] | ((uint32_t)st[1] << 8) | ((uint32_t)st[2] << 16);
}

bool axp_vbus_present(void) { return (rd(REG_STATUS1) >> 5) & 1; }
bool axp_is_charging(void)  { return (rd(REG_STATUS2) >> 5) == 0x01; }
bool axp_charge_done(void)  { return (rd(REG_STATUS2) & 0x07) == 4; }

int axp_batt_mv(void)
{
    if (!((rd(REG_STATUS1) >> 3) & 1)) return 0;
    return adc14(REG_ADC_VBAT_H, 0x1F);
}

int axp_vbus_mv(void) { return axp_vbus_present() ? adc14(REG_ADC_VBUS_H, 0x3F) : 0; }
int axp_vsys_mv(void) { return adc14(REG_ADC_VSYS_H, 0x3F); }

int axp_percent(void)
{
    if (!((rd(REG_STATUS1) >> 3) & 1)) return -1;
    uint8_t p = rd(REG_PERCENT);
    return p > 100 ? -1 : p;
}

float axp_die_temp_c(void) { return 22.0f + (7274 - adc14(REG_ADC_TDIE_H, 0x3F)) / 20.0f; }

uint8_t axp_reg(uint8_t reg) { return rd(reg); }

void axp_power_off(void)
{
    upd(REG_COMMON_CFG, 0x01, 0x01);
    vTaskDelay(portMAX_DELAY);
}
