/* xl9555.c — voir xl9555.h. Registres compatibles PCA9555/TCA9555. */
#include "xl9555.h"
#include "board.h"
#include "i2c_bus.h"

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
