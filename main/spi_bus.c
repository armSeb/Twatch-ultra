/* spi_bus.c — voir spi_bus.h */
#include "spi_bus.h"
#include "board.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_mux;
static int s_users;

static void lock(void)
{
    static portMUX_TYPE m = portMUX_INITIALIZER_UNLOCKED;
    if (!s_mux) {
        SemaphoreHandle_t n = xSemaphoreCreateMutex();
        portENTER_CRITICAL(&m);
        if (!s_mux) { s_mux = n; n = NULL; }
        portEXIT_CRITICAL(&m);
        if (n) vSemaphoreDelete(n);
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
}

esp_err_t spi_shared_get(void)
{
    lock();
    esp_err_t e = ESP_OK;
    if (s_users == 0) {
        spi_bus_config_t bus = {
            .mosi_io_num = BOARD_SPI_MOSI,
            .miso_io_num = BOARD_SPI_MISO,
            .sclk_io_num = BOARD_SPI_SCK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 4096,
        };
        e = spi_bus_initialize(BOARD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    }
    if (e == ESP_OK) s_users++;
    xSemaphoreGive(s_mux);
    return e;
}

void spi_shared_put(void)
{
    lock();
    if (s_users > 0 && --s_users == 0) {
        spi_bus_free(BOARD_SPI_HOST);
        const gpio_num_t p[] = { BOARD_SPI_MOSI, BOARD_SPI_MISO, BOARD_SPI_SCK };
        for (int i = 0; i < 3; i++) {
            gpio_reset_pin(p[i]);
            gpio_set_pull_mode(p[i], GPIO_FLOATING);
            gpio_set_direction(p[i], GPIO_MODE_DISABLE);
        }
    }
    xSemaphoreGive(s_mux);
}
