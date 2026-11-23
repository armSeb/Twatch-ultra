/* haptic.c — voir haptic.h. Séquence d'initialisation : fiche TI (ERM en
 * boucle ouverte), comme le pilote Adafruit/SensorLib. */
#include "haptic.h"
#include "board.h"
#include "i2c_bus.h"
#include "xl9555.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define REG_STATUS     0x00
#define REG_MODE       0x01   /* b6 standby, b2:0 mode (5 = temps réel) */
#define REG_RTP        0x02
#define REG_LIBRARY    0x03
#define REG_WAVESEQ1   0x04
#define REG_OVERDRIVE  0x0D
#define REG_SUSTAINP   0x0E
#define REG_SUSTAINN   0x0F
#define REG_BRAKE      0x10
#define REG_AUDIOMAX   0x13
#define REG_FEEDBACK   0x1A   /* b7 : 0 = ERM */
#define REG_CONTROL3   0x1D   /* b5 : ERM en boucle ouverte */

#define MODE_RTP       0x05
#define MODE_STANDBY   0x40
#define RTP_LEVEL      0x7F   /* pleine amplitude, format signé */

static const char *TAG = "haptic";
static i2c_master_dev_handle_t s_dev;

esp_err_t haptic_init(void)
{
    xl9555_output(XIO_DRV_EN, true);
    vTaskDelay(pdMS_TO_TICKS(2));
    esp_err_t e = i2c_bus_add(BOARD_DRV2605_ADDR, 0, &s_dev);
    if (e != ESP_OK) { ESP_LOGW(TAG, "DRV2605 absent"); return e; }
    i2c_reg_write8(s_dev, REG_MODE, 0x00);           /* sortie de veille */
    i2c_reg_write8(s_dev, REG_RTP, 0x00);
    i2c_reg_write8(s_dev, REG_WAVESEQ1, 1);
    i2c_reg_write8(s_dev, REG_WAVESEQ1 + 1, 0);
    i2c_reg_write8(s_dev, REG_OVERDRIVE, 0);
    i2c_reg_write8(s_dev, REG_SUSTAINP, 0);
    i2c_reg_write8(s_dev, REG_SUSTAINN, 0);
    i2c_reg_write8(s_dev, REG_BRAKE, 0);
    i2c_reg_write8(s_dev, REG_AUDIOMAX, 0x64);
    i2c_reg_write8(s_dev, REG_LIBRARY, 1);
    i2c_reg_update8(s_dev, REG_FEEDBACK, 0x80, 0x00);
    i2c_reg_update8(s_dev, REG_CONTROL3, 0x20, 0x20);
    i2c_reg_write8(s_dev, REG_MODE, MODE_STANDBY);
    return ESP_OK;
}

void haptic_set(bool on)
{
    if (!s_dev) return;
    if (on) {
        i2c_reg_write8(s_dev, REG_RTP, RTP_LEVEL);
        i2c_reg_write8(s_dev, REG_MODE, MODE_RTP);
    } else {
        i2c_reg_write8(s_dev, REG_RTP, 0x00);
        i2c_reg_write8(s_dev, REG_MODE, MODE_STANDBY);
    }
}
