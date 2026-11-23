#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

/* Bus I2C unique de la T-Watch Ultra (SDA 3, SCL 2) : AXP2101, XL9555,
 * PCF85063A, DRV2605, CST9217, BHI260AP. Driver « i2c_master » d'ESP-IDF 5.x,
 * thread-safe ; il prend lui-même un verrou PM APB pendant les transactions. */
esp_err_t i2c_bus_init(void);
/* hz = 0 : fréquence par défaut (BOARD_I2C_HZ) */
esp_err_t i2c_bus_add(uint8_t addr, uint32_t hz, i2c_master_dev_handle_t *out);

esp_err_t i2c_reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t len);
esp_err_t i2c_reg_write(i2c_master_dev_handle_t dev, uint8_t reg, const uint8_t *buf, size_t len);   /* len ≤ 32 */
esp_err_t i2c_reg_write8(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val);
esp_err_t i2c_reg_read8(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *val);
/* lecture-modification-écriture : reg = (reg & ~mask) | (val & mask) */
esp_err_t i2c_reg_update8(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t mask, uint8_t val);
