/* spi_bus.c — voir spi_bus.h */
#include "spi_bus.h"
#include "board.h"
#include "axp2101.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"

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
/* Le NFC (DLDO1) est alimenté tant que quelqu'un utilise le bus. La carte SD
 * (ALDO1) l'est quand elle sert (s_sd_users) et, avec la radio en marche,
 * tant qu'on n'a pas vérifié que le SX1262 répond carte coupée : au repos
 * certaines cartes tirent des dizaines de mA (vu sur la montre). */
static int s_peers, s_sd_users;
static bool s_sd_on;
static bool s_sd_bus_need = true;       /* la radio a besoin du rail SD (pas encore vérifié) */
static bool s_sd_bad;                   /* vérification ratée : rail SD gardé jusqu'au redémarrage */
static void (*s_runner)(void);          /* radio : lance la vérification dans sa tâche */

void spi_peers_hold_cs(int gpio)
{
    gpio_num_t g = (gpio_num_t)gpio;
    gpio_set_level(g, 1);
    gpio_config_t o = { .pin_bit_mask = 1ULL << g, .mode = GPIO_MODE_OUTPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&o);
    gpio_set_level(g, 1);
    gpio_sleep_sel_dis(g);                      /* tenu haut en light sleep aussi */
}

/* CS d'une puce sans alimentation : haute impédance (ne la nourrit pas) */
static void cs_release(gpio_num_t g)
{
    gpio_reset_pin(g);
    gpio_set_pull_mode(g, GPIO_FLOATING);
    gpio_set_direction(g, GPIO_MODE_DISABLE);
    gpio_sleep_sel_en(g);
}

/* sous verrou */
static void sd_rail_apply(void)
{
    bool on = s_sd_users > 0 || (s_peers > 0 && s_sd_bus_need);
    if (on == s_sd_on) return;
    s_sd_on = on;
    if (on) {
        axp_rail(AXP_RAIL_SD, true);
        spi_peers_hold_cs(BOARD_SD_CS);
    } else {
        cs_release(BOARD_SD_CS);
        axp_rail(AXP_RAIL_SD, false);
    }
}

void spi_peers_get(void)
{
    lock();
    if (s_peers++ == 0) {
        axp_rail(AXP_RAIL_NFC, true);
        spi_peers_hold_cs(BOARD_NFC_CS);
    }
    sd_rail_apply();
    xSemaphoreGive(s_mux);
}

void spi_peers_put(void)
{
    lock();
    if (s_peers > 0 && --s_peers == 0) {
        cs_release(BOARD_NFC_CS);
        axp_rail(AXP_RAIL_NFC, false);
    }
    sd_rail_apply();
    xSemaphoreGive(s_mux);
}

void spi_sd_get(void)
{
    lock();
    s_sd_users++;
    sd_rail_apply();
    xSemaphoreGive(s_mux);
}

void spi_sd_put(void)
{
    lock();
    if (s_sd_users > 0) s_sd_users--;
    sd_rail_apply();
    xSemaphoreGive(s_mux);
}

void spi_set_sd_off_runner(void (*run)(void)) { s_runner = run; }

bool spi_sd_powered(void) { return s_sd_on; }

void spi_sd_try_off(void)
{
    void (*run)(void) = s_runner;
    if (run && !s_sd_bad && s_sd_bus_need) run();
}

void spi_sd_try_off_run(bool (*check)(void))
{
    lock();
    bool go = !s_sd_bad && s_sd_bus_need && s_peers > 0 && s_sd_users == 0 && s_sd_on;
    if (go) {
        s_sd_bus_need = false;
        sd_rail_apply();
    }
    xSemaphoreGive(s_mux);
    if (!go) return;
    vTaskDelay(pdMS_TO_TICKS(300));             /* le condensateur de la carte se vide */
    bool ok = check();
    lock();
    if (s_sd_on) {                              /* carte rallumée entre-temps : rien de prouvé */
        s_sd_bus_need = true;
    } else if (ok) {
        ESP_LOGI("spi", "carte SD hors tension, la radio répond : rail SD coupé au repos");
    } else {
        ESP_LOGW("spi", "radio muette carte SD hors tension : rail SD gardé allumé");
        s_sd_bad = true;
        s_sd_bus_need = true;
    }
    sd_rail_apply();
    xSemaphoreGive(s_mux);
}

void spi_sd_bus_failed(void)
{
    lock();
    if (!s_sd_bad) ESP_LOGW("spi", "radio muette carte SD coupée : rail SD rallumé jusqu'au redémarrage");
    s_sd_bad = true;
    s_sd_bus_need = true;
    sd_rail_apply();
    xSemaphoreGive(s_mux);
}
