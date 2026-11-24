/* spi_bus.c — voir spi_bus.h */
#include "spi_bus.h"
#include "board.h"
#include "axp2101.h"
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
        /* bus en service : ses lignes gardent leur état en light sleep */
        if (e == ESP_OK) {
            gpio_sleep_sel_dis(BOARD_SPI_MOSI);
            gpio_sleep_sel_dis(BOARD_SPI_MISO);
            gpio_sleep_sel_dis(BOARD_SPI_SCK);
        }
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
            gpio_sleep_sel_en(p[i]);
        }
    }
    xSemaphoreGive(s_mux);
}

/* ------------------------------------------------------------------ voisins */
static int s_peers;

void spi_peers_hold_cs(int gpio)
{
    gpio_num_t g = (gpio_num_t)gpio;
    gpio_set_level(g, 1);
    gpio_config_t o = { .pin_bit_mask = 1ULL << g, .mode = GPIO_MODE_OUTPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&o);
    gpio_set_level(g, 1);
    gpio_sleep_sel_dis(g);                      /* tenu haut en light sleep aussi */
}

void spi_peers_get(void)
{
    lock();
    if (s_peers++ == 0) {
        axp_rail(AXP_RAIL_SD, true);
        axp_rail(AXP_RAIL_NFC, true);
        spi_peers_hold_cs(BOARD_SD_CS);
        spi_peers_hold_cs(BOARD_NFC_CS);
    }
    xSemaphoreGive(s_mux);
}

void spi_peers_put(void)
{
    lock();
    if (s_peers > 0 && --s_peers == 0) {
        const gpio_num_t cs[] = { BOARD_SD_CS, BOARD_NFC_CS };
        for (int i = 0; i < 2; i++) {
            gpio_reset_pin(cs[i]);
            gpio_set_pull_mode(cs[i], GPIO_FLOATING);
            gpio_set_direction(cs[i], GPIO_MODE_DISABLE);
            gpio_sleep_sel_en(cs[i]);
        }
        axp_rail(AXP_RAIL_SD, false);
        axp_rail(AXP_RAIL_NFC, false);
    }
    xSemaphoreGive(s_mux);
}
