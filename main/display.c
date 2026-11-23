/*
 * display.c — AMOLED CO5300 410x502 (QSPI, esp_lcd) + tactile CST9217 +
 * liaison LVGL 9. Voir display.h.
 *
 * Protocole QSPI du CO5300 (comme SH8601) : un registre s'écrit avec
 * l'instruction 0x02 sur une ligne, puis l'adresse 24 bits 0x00 RR 00 ; les
 * pixels partent avec l'instruction 0x32, adresse 0x002C00, données sur 4
 * lignes. esp_lcd fait exactement cela en « quad_mode » avec 32 bits de
 * commande. La fenêtre d'écriture doit commencer et finir sur des colonnes
 * et lignes paires : les zones invalidées par LVGL sont arrondies.
 */
#include "display.h"
#include "board.h"
#include "power.h"
#include "i2c_bus.h"
#include "xl9555.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "lvgl.h"
#include "src/indev/lv_indev_private.h"   /* seuils de geste */
#include "ui_geom.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "app_events.h"

static const char *TAG = "display";

static esp_lcd_panel_io_handle_t s_io;
static lv_display_t *s_disp;
static lv_indev_t *s_indev;
static i2c_master_dev_handle_t s_touch;
static bool s_on;
static bool s_turbo;           /* verrou CPU_FREQ_MAX tenu */
static bool s_glance;          /* allumé en coup d'œil */
static bool s_glance_touch;    /* toucher reçu en coup d'œil, pas encore traité */
static bool s_swallow;         /* doigt posé pendant le coup d'œil : ignoré jusqu'au relâcher */
static bool s_touch_asleep;    /* CST9217 en veille profonde : il faut un reset pour le réveiller */
static SemaphoreHandle_t s_te_sem;
static int64_t s_slpin_us;     /* dernier SLPIN : la dalle exige 120 ms avant SLPOUT */

/* Écran allumé sans interaction ni animation depuis TURBO_IDLE_MS : le CPU
 * redescend au minimum. Il remonte dès qu'un toucher remet l'inactivité LVGL
 * à zéro. */
#define TURBO_IDLE_MS 1500

/* 2 × 410 × 40 × 2 = 64 Ko de RAM DMA : une bande de 40 lignes part en
 * ~0,8 ms à 80 MHz sur 4 lignes */
#define BUF_LINES 40

/* ------------------------------------------------------------------ */
/* CO5300                                                               */
/* ------------------------------------------------------------------ */
#define QSPI_CMD(r)   ((0x02u << 24) | ((uint32_t)(r) << 8))
#define QSPI_PIXELS   ((0x32u << 24) | (0x2Cu << 8))

static void lcd_cmd(uint8_t reg, const void *data, size_t len)
{
    esp_lcd_panel_io_tx_param(s_io, QSPI_CMD(reg), data, len);
}

static void lcd_cmd1(uint8_t reg, uint8_t v) { lcd_cmd(reg, &v, 1); }

static void lcd_window(int x1, int y1, int x2, int y2)   /* bornes incluses */
{
    x1 += BOARD_LCD_GAP_X; x2 += BOARD_LCD_GAP_X;
    y1 += BOARD_LCD_GAP_Y; y2 += BOARD_LCD_GAP_Y;
    uint8_t c[4] = { x1 >> 8, x1 & 0xFF, x2 >> 8, x2 & 0xFF };
    uint8_t r[4] = { y1 >> 8, y1 & 0xFF, y2 >> 8, y2 & 0xFF };
    lcd_cmd(0x2A, c, 4);
    lcd_cmd(0x2B, r, 4);
}

static void lcd_pixels(int x1, int y1, int x2, int y2, const void *px)
{
    lcd_window(x1, y1, x2, y2);
    esp_lcd_panel_io_tx_color(s_io, QSPI_PIXELS, px, (size_t)(x2 - x1 + 1) * (y2 - y1 + 1) * 2);
}

/* Séquence d'initialisation de LilyGoLib (dalle 2,06" de l'Ultra) */
static void lcd_init_seq(void)
{
    lcd_cmd1(0xFE, 0x00);            /* page de commandes utilisateur */
    lcd_cmd1(0xC4, 0x80);            /* interface SPI : QSPI */
    lcd_cmd1(0x3A, 0x55);            /* RGB565 */
    lcd_cmd1(0x35, 0x00);            /* TE : vblank seulement */
    lcd_cmd1(0x53, 0x20);            /* luminosité pilotée par 0x51 */
    lcd_cmd1(0x63, 0xFF);            /* luminosité HBM max */
    lcd_cmd1(0x51, 0x00);            /* noir tant qu'aucune image n'est peinte */
}

void display_panel_brightness(uint8_t level)
{
    if (s_io) lcd_cmd1(0x51, level);
}

/* TE : front montant au début du blanking vertical (~60 Hz) */
static void IRAM_ATTR te_isr(void *arg)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_te_sem, &hp);
    if (hp) portYIELD_FROM_ISR();
}

static void wait_te(void)
{
    xSemaphoreTake(s_te_sem, 0);                  /* vieux front */
    xSemaphoreTake(s_te_sem, pdMS_TO_TICKS(20));
}

/* ------------------------------------------------------------------ */
/* LVGL                                                                 */
/* ------------------------------------------------------------------ */
static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *ev, void *ctx)
{
    lv_display_t *d = *(lv_display_t **)ctx;
    if (d) lv_display_flush_ready(d);
    return false;
}

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    lv_draw_sw_rgb565_swap(px, lv_area_get_size(a));
    lcd_pixels(a->x1, a->y1, a->x2, a->y2, px);
}

/* fenêtre du CO5300 : x1, y1 pairs, x2, y2 impairs */
static void rounder_cb(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

/* ------------------------------------------------------------------ */
/* Affichage direct (jeux)                                              */
/* ------------------------------------------------------------------ */
#define STRIP_LINES 26     /* 502 = 19 × 26 + 8 : bandes paires */
static uint16_t *s_strip[2];

bool display_direct(display_strip_fn fill, void *ctx)
{
    if (!s_on) return false;
    if (!s_strip[0]) {
        for (int i = 0; i < 2; i++)
            s_strip[i] = heap_caps_malloc(BOARD_LCD_W * STRIP_LINES * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_strip[0] || !s_strip[1]) { display_direct_end(); return false; }
    }
    /* début d'image calé sur le TE : la dalle relit la GRAM derrière nous,
     * pas de déchirure horizontale. La bande k se remplit pendant que k−1
     * part à l'écran (esp_lcd attend la fin de la précédente). */
    wait_te();
    for (int y = 0, k = 0; y < BOARD_LCD_H; y += STRIP_LINES, k ^= 1) {
        int n = BOARD_LCD_H - y < STRIP_LINES ? BOARD_LCD_H - y : STRIP_LINES;
        fill(s_strip[k], y, n, ctx);
        lv_draw_sw_rgb565_swap(s_strip[k], BOARD_LCD_W * n);
        lcd_pixels(0, y, BOARD_LCD_W - 1, y + n - 1, s_strip[k]);
    }
    return true;
}

void display_direct_end(void)
{
    if (!s_strip[0] && !s_strip[1]) return;
    vTaskDelay(pdMS_TO_TICKS(20));                /* dernière bande partie */
    for (int i = 0; i < 2; i++) { heap_caps_free(s_strip[i]); s_strip[i] = NULL; }
}

/* ------------------------------------------------------------------ */
/* CST9217 : registres sur 16 bits, lecture à 0xD000 puis acquittement   */
/* 0xAB. Point n : 5 octets (id/événement, X haut, Y haut, X/Y bas,      */
/* pression), le 1er à l'offset 0, les suivants à 7 + 5 (n − 1).        */
/* ------------------------------------------------------------------ */
#define CST_ACK 0xAB

static bool cst_read(uint16_t *x, uint16_t *y, int *count)
{
    uint8_t reg[2] = { 0xD0, 0x00 };
    uint8_t b[12];
    *count = 0;
    if (i2c_master_transmit_receive(s_touch, reg, 2, b, sizeof b, 50) != ESP_OK) return false;
    uint8_t ack[3] = { 0xD0, 0x00, CST_ACK };
    i2c_master_transmit(s_touch, ack, 3, 50);
    if (b[6] != CST_ACK || b[0] == CST_ACK || b[0] == 0x00) return true;   /* rien de neuf */
    int n = b[5] & 0x7F;
    if (n < 1 || n > 2 || (b[0] & 0x0F) != 0x06) return true;              /* 0x06 = appuyé */
    *x = (uint16_t)((b[1] << 4) | (b[3] >> 4));
    *y = (uint16_t)((b[2] << 4) | (b[3] & 0x0F));
    *count = n;
    return true;
}

static void cst_cmd(uint16_t reg)
{
    uint8_t b[2] = { reg >> 8, reg & 0xFF };
    i2c_master_transmit(s_touch, b, 2, 50);
}

static void touch_reset(void)
{
    xl9555_output(XIO_TOUCH_RST, false);
    vTaskDelay(pdMS_TO_TICKS(5));
    xl9555_output(XIO_TOUCH_RST, true);
    vTaskDelay(pdMS_TO_TICKS(40));
    s_touch_asleep = false;
}

/* écran éteint : actif (balayage lent automatique) si le double tap doit
 * réveiller la montre, sinon veille profonde (D105, réveil par reset) */
static void touch_power(bool screen_on)
{
    if (!s_touch) return;
    if (screen_on) {
        if (s_touch_asleep) touch_reset();
    } else if (!settings_get()->tap_wake && !s_touch_asleep) {
        cst_cmd(0xD101);                 /* mode commande */
        cst_cmd(0xD105);                 /* veille */
        s_touch_asleep = true;
    }
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static int16_t last_x, last_y;
    static bool pressed;
    data->state = LV_INDEV_STATE_RELEASED;
    uint16_t x, y;
    int n;
    /* INT haute = aucun doigt : pas d'I2C pour rien à 30 Hz */
    if ((pressed || gpio_get_level(BOARD_TOUCH_INT) == 0) && cst_read(&x, &y, &n)) {
        if (n > 0 && x < BOARD_LCD_W && y < BOARD_LCD_H) {
            last_x = x;
            last_y = y;
            data->state = LV_INDEV_STATE_PRESSED;
        }
    }
    pressed = data->state == LV_INDEV_STATE_PRESSED;
    /* coup d'œil : le toucher réveille, il n'agit pas sur l'interface */
    if (data->state == LV_INDEV_STATE_PRESSED && s_glance) { s_glance_touch = true; s_swallow = true; }
    if (s_swallow) {
        if (data->state == LV_INDEV_STATE_RELEASED) s_swallow = false;
        data->state = LV_INDEV_STATE_RELEASED;
    }
    data->point.x = last_x;
    data->point.y = last_y;
}

/* ------------------------------------------------------------------ */
/* Réveil par double tap : écran éteint, le CST9217 reste actif ; sa     */
/* ligne INT (GPIO12, active basse) réveille la tâche app, qui           */
/* échantillonne le tactile en I2C pendant < 1 s pour reconnaître deux   */
/* tapes courtes. Interruption en niveau : marche aussi en light sleep.  */
/* ------------------------------------------------------------------ */
#define TAP_SAMPLE_MS   25
#define TAP_MAX_DOWN_MS 350    /* au-delà : appui long, pas une tape */
#define TAP_MAX_GAP_MS  450    /* entre la 1re et la 2e tape */
#define TAP_WINDOW_MS   1200

static bool    s_tap_armed;
static int     s_tap_phantom;    /* INT basse sans toucher : ligne bloquée ? */
static volatile int64_t s_tap_isr_us;
static struct { bool on; bool down; int taps; int64_t t0, down_t, up_t; } s_tap;

static void IRAM_ATTR touch_isr(void *arg)
{
    gpio_intr_disable(BOARD_TOUCH_INT);
    s_tap_isr_us = esp_timer_get_time();
    BaseType_t hp = pdFALSE;
    app_event_t ev = { .type = EV_TOUCH_INT };
    app_post_from_isr(&ev, &hp);
    if (hp) portYIELD_FROM_ISR();
}

static void tap_arm(bool arm)
{
    s_tap_armed = arm;
    if (arm) gpio_intr_enable(BOARD_TOUCH_INT);
    else gpio_intr_disable(BOARD_TOUCH_INT);
}

static bool touch_down_now(void)
{
    uint16_t x, y;
    int n;
    return cst_read(&x, &y, &n) && n > 0;
}

bool display_tap_begin(void)
{
    if (s_on || s_tap.on) return false;
    s_tap.on = true;
    s_tap.down = true;                 /* la ligne INT vient de passer basse */
    s_tap.taps = 0;
    s_tap.t0 = s_tap.down_t = s_tap_isr_us;
    s_tap.up_t = 0;
    return true;
}

int display_tap_poll(void)
{
    if (!s_tap.on) return -1;
    int64_t now = esp_timer_get_time();
    bool d = touch_down_now();
    if (d && !s_tap.down) {                       /* appui */
        if (s_tap.taps && now - s_tap.up_t > TAP_MAX_GAP_MS * 1000LL) s_tap.taps = 0;
        s_tap.down_t = now;
    } else if (!d && s_tap.down) {                /* relâché */
        if (now - s_tap.down_t <= TAP_MAX_DOWN_MS * 1000LL) { s_tap.taps++; s_tap.up_t = now; }
        else s_tap.taps = 0;
    }
    s_tap.down = d;
    int r = 0;
    if (s_tap.taps >= 2) r = 1;
    else if (now - s_tap.t0 > TAP_WINDOW_MS * 1000LL && !d) r = -1;
    else if (now - s_tap.t0 > 3 * TAP_WINDOW_MS * 1000LL) r = -1;   /* doigt posé longtemps */
    if (r) {
        s_tap.on = false;
        if (r == 1 || d) s_tap_phantom = 0;
        else if (gpio_get_level(BOARD_TOUCH_INT) == 0) s_tap_phantom++;
        if (s_tap_phantom >= 3) {
            /* sécurité batterie : ligne INT basse en permanence, on n'arme plus
             * jusqu'au prochain allumage de l'écran */
            ESP_LOGW(TAG, "INT tactile bloquée basse : double tap suspendu");
        } else if (!s_on && settings_get()->tap_wake) {
            tap_arm(true);
        }
    }
    return r;
}

uint32_t display_tap_sample_ms(void) { return TAP_SAMPLE_MS; }

void display_tap_enable(bool en)
{
    if (s_on) return;
    if (en && s_touch_asleep) touch_reset();
    touch_power(false);
    tap_arm(en);
}

/* ------------------------------------------------------------------ */
esp_err_t display_init(void)
{
    /* alimentation de la dalle (ALDO2 déjà allumé par axp_init) puis reset */
    xl9555_output(XIO_DISP_EN, true);
    gpio_config_t rst = { .pin_bit_mask = 1ULL << BOARD_LCD_RST, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&rst);
    gpio_set_level(BOARD_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(BOARD_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));      /* CO5300 : prêt ~120 ms après le reset */

    spi_bus_config_t bus = {
        .data0_io_num = BOARD_LCD_D0,
        .data1_io_num = BOARD_LCD_D1,
        .sclk_io_num = BOARD_LCD_SCLK,
        .data2_io_num = BOARD_LCD_D2,
        .data3_io_num = BOARD_LCD_D3,
        .max_transfer_sz = BOARD_LCD_W * BUF_LINES * 2 + 16,
        .flags = SPICOMMON_BUSFLAG_QUAD,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    lv_init();
    lv_tick_set_cb(tick_cb);
    s_disp = lv_display_create(BOARD_LCD_W, BOARD_LCD_H);

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BOARD_LCD_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = BOARD_LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_trans_done,
        .user_ctx = &s_disp,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags.quad_mode = true,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_HOST, &io_cfg, &s_io));

    /* light sleep : IDF isole toutes les broches (contournement d'un reset
     * sur décharge électrostatique). Celles de la dalle doivent garder leur
     * état, sinon CS et reset flottent et l'écran reste noir. */
    const gpio_num_t keep[] = { BOARD_LCD_CS, BOARD_LCD_SCLK, BOARD_LCD_D0, BOARD_LCD_D1,
                                BOARD_LCD_D2, BOARD_LCD_D3, BOARD_LCD_RST, BOARD_LCD_TE };
    for (size_t i = 0; i < sizeof keep / sizeof keep[0]; i++) gpio_sleep_sel_dis(keep[i]);

    /* comme LilyGoLib : la séquence deux fois (la première peut tomber
     * pendant le démarrage interne de la dalle), puis réveil et allumage */
    for (int k = 0; k < 2; k++) {
        lcd_init_seq();
        lcd_cmd(0x11, NULL, 0);          /* SLPOUT */
        vTaskDelay(pdMS_TO_TICKS(120));
        lcd_cmd(0x29, NULL, 0);          /* DISPON */
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    size_t bsz = BOARD_LCD_W * BUF_LINES * 2;
    void *b1 = heap_caps_malloc(bsz, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *b2 = heap_caps_malloc(bsz, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(b1 && b2);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, b1, b2, bsz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_add_event_cb(s_disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    /* TE (sortie de la dalle, front montant) */
    s_te_sem = xSemaphoreCreateBinary();
    gpio_config_t te = {
        .pin_bit_mask = 1ULL << BOARD_LCD_TE,
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    gpio_config(&te);
    gpio_isr_handler_add(BOARD_LCD_TE, te_isr, NULL);   /* service installé par power_init */
    gpio_intr_disable(BOARD_LCD_TE);

    /* tactile */
    touch_reset();
    if (i2c_bus_add(BOARD_CST9217_ADDR, 0, &s_touch) == ESP_OK) {
        ESP_LOGI(TAG, "CST9217 présent");
    } else {
        ESP_LOGE(TAG, "CST9217 absent");
    }
    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, touch_read_cb);
    lv_indev_set_display(s_indev, s_disp);
    /* seuils LVGL en pixels (défauts 10, 50, 3) : mêmes distances au doigt que sur 240×240 */
    lv_indev_set_scroll_limit(s_indev, S(10));
    s_indev->gesture_limit = S(50);
    s_indev->gesture_min_velocity = S(3);

    /* ligne INT du tactile (active basse) */
    gpio_config_t ti = {
        .pin_bit_mask = 1ULL << BOARD_TOUCH_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_LOW_LEVEL,
    };
    gpio_config(&ti);
    gpio_intr_disable(BOARD_TOUCH_INT);
    gpio_isr_handler_add(BOARD_TOUCH_INT, touch_isr, NULL);
    gpio_wakeup_enable(BOARD_TOUCH_INT, GPIO_INTR_LOW_LEVEL);

    /* l'écran reste éteint (veille) jusqu'au premier display_set_on(true) */
    lcd_cmd(0x28, NULL, 0);              /* DISPOFF */
    lcd_cmd(0x10, NULL, 0);              /* SLPIN */
    s_slpin_us = esp_timer_get_time();
    s_on = false;
    touch_power(false);
    lv_indev_enable(s_indev, false);
    ESP_LOGI(TAG, "init OK");
    return ESP_OK;
}

static void set_on(bool on, bool glance, uint8_t level);

void display_set_on(bool on) { set_on(on, false, 0); }

void display_set_glance(uint8_t level) { set_on(true, true, level); }

bool display_is_glance(void) { return s_on && s_glance; }

bool display_glance_take_touch(void)
{
    bool t = s_glance_touch;
    s_glance_touch = false;
    return t;
}

void display_glance_end(void)
{
    if (!s_on || !s_glance) return;
    s_glance = false;
    power_backlight(settings_get()->brightness);
    if (!s_turbo) { power_hold_active(true); s_turbo = true; }
    lv_display_trigger_activity(s_disp);
}

static void set_on(bool on, bool glance, uint8_t level)
{
    if (on == s_on) return;
    power_screen_segment(on);
    if (on) {
        tap_arm(false);
        s_tap.on = false;
        s_tap_phantom = 0;
        /* Pas de verrou « nosleep » écran allumé : l'AMOLED garde son image
         * dans sa GRAM, le light sleep reste permis entre deux rendus (un
         * transfert SPI en cours tient lui-même un verrou APB). */
        s_glance = glance;
        s_glance_touch = s_swallow = false;
        if (!glance) {
            power_hold_active(true);             /* CPU max pour le premier rendu */
            s_turbo = true;
        }
        /* SLPOUT moins de 120 ms après SLPIN est ignoré : la dalle restait
         * endormie (écran noir) quand le premier allumage suivait l'init */
        int64_t since = esp_timer_get_time() - s_slpin_us;
        if (since < 125000) vTaskDelay(pdMS_TO_TICKS((125000 - since) / 1000 + 1));
        lcd_cmd(0x11, NULL, 0);                  /* SLPOUT */
        touch_power(true);
        lv_indev_reset(s_indev, NULL);
        lv_indev_enable(s_indev, true);
        lv_display_trigger_activity(s_disp);
        vTaskDelay(pdMS_TO_TICKS(10));
        /* on peint une image fraîche AVANT d'allumer la dalle */
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(s_disp);
        lcd_cmd(0x29, NULL, 0);                  /* DISPON */
        power_backlight(glance ? level : settings_get()->brightness);
        gpio_intr_enable(BOARD_LCD_TE);
        s_on = true;
    } else {
        gpio_intr_disable(BOARD_LCD_TE);
        power_backlight(0);
        lcd_cmd(0x28, NULL, 0);                  /* DISPOFF */
        lcd_cmd(0x10, NULL, 0);                  /* SLPIN */
        s_slpin_us = esp_timer_get_time();
        lv_indev_enable(s_indev, false);
        s_on = false;
        touch_power(false);
        s_glance = s_glance_touch = s_swallow = false;
        if (settings_get()->tap_wake) tap_arm(true);
        if (s_turbo) { power_hold_active(false); s_turbo = false; }
    }
}

bool display_is_on(void) { return s_on; }

void display_perf_tick(void)
{
    if (!s_on || s_glance) return;            /* coup d'œil : CPU au minimum */
    bool busy = lv_display_get_inactive_time(s_disp) < TURBO_IDLE_MS
                || lv_anim_count_running() > 0;
    if (busy != s_turbo) {
        power_hold_active(busy);
        s_turbo = busy;
    }
}

void display_set_brightness(uint8_t level)
{
    if (s_on && !s_glance) power_backlight(level);
}
