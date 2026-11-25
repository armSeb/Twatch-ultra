/* xl9555.c — voir xl9555.h. Registres compatibles PCA9555/TCA9555. */
#include "xl9555.h"
#include "board.h"
#include "i2c_bus.h"
#include "esp_log.h"

#define REG_IN0   0x00
#define REG_OUT0  0x02
#define REG_CFG0  0x06    /* 1 = entrée */

static i2c_master_dev_handle_t s_dev;
static uint16_t s_out, s_cfg = 0xFFFF;

static esp_err_t write16(uint8_t reg, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    return i2c_reg_write(s_dev, reg, b, 2);
}

esp_err_t xl9555_init(void)
{
    esp_err_t e = i2c_bus_add(BOARD_XL9555_ADDR, 0, &s_dev);
    if (e != ESP_OK) return e;
    uint8_t b[2];
    if (i2c_reg_read(s_dev, REG_OUT0, b, 2) == ESP_OK) s_out = b[0] | (b[1] << 8);
    if (i2c_reg_read(s_dev, REG_CFG0, b, 2) == ESP_OK) s_cfg = b[0] | (b[1] << 8);
    ESP_LOGI("xl9555", "sorties 0x%04x, config 0x%04x", s_out, s_cfg);
    /* Le XL9555 garde registres de sortie et de direction tant que la
     * batterie l'alimente : un firmware précédent (celui d'usine, une
     * version de test) a pu laisser n'importe quoi. Tout est donc réécrit,
     * broche par broche, d'après le schéma V1.0 :
     *  - P12 SD_DET, seule entrée : le contact de détection la met à la
     *    masse carte insérée. Laissée en sortie haute, c'était un
     *    court-circuit de ~40 mA (0.2.50) ;
     *  - P06 M_EN, P07 VCI_EN, P10 TP_RST, P13 LORA_SEL : sorties, au niveau
     *    déjà présent (pas de coupure de l'écran ni du tactile ici), chaque
     *    pilote fixe ensuite le sien ;
     *  - P00-P05, P11, P14-P17, reliées à rien : sorties au niveau haut (une
     *    entrée en l'air flotte à mi-tension et fait consommer son étage). */
    const uint16_t unused = 0x003F | (1u << 9) | 0xF000;
    const uint16_t cfg = 1u << XIO_SD_DET;
    if (s_cfg != cfg) ESP_LOGW("xl9555", "directions 0x%04x trouvées, remises à 0x%04x", s_cfg, cfg);
    s_out |= unused;
    if (write16(REG_OUT0, s_out) == ESP_OK && write16(REG_CFG0, cfg) == ESP_OK) s_cfg = cfg;
    return ESP_OK;
}

esp_err_t xl9555_output(int pin, bool level)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    uint16_t m = 1u << pin;
    s_out = level ? (s_out | m) : (s_out & ~m);
    esp_err_t e = write16(REG_OUT0, s_out);         /* niveau d'abord : pas de glitch */
    if (e == ESP_OK && (s_cfg & m)) { s_cfg &= ~m; e = write16(REG_CFG0, s_cfg); }
    return e;
}

bool xl9555_input(int pin)
{
    uint8_t b[2];
    if (!s_dev || i2c_reg_read(s_dev, REG_IN0, b, 2) != ESP_OK) return false;
    return ((b[0] | (b[1] << 8)) >> pin) & 1;
}

esp_err_t xl9555_read(int pin, bool *level)
{
    uint8_t b[2];
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    esp_err_t e = i2c_reg_read(s_dev, REG_IN0, b, 2);
    if (e == ESP_OK) *level = ((b[0] | (b[1] << 8)) >> pin) & 1;
    return e;
}
