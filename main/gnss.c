/* gnss.c — voir gnss.h */
#include "gnss.h"
#include <string.h>
#include <time.h>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "app_events.h"
#include "axp2101.h"
#include "board.h"
#include "power.h"
#include "settings.h"
#include "task_psram.h"
#include "wclock.h"

static const char *TAG = "gnss";

#define GNSS_UART       UART_NUM_1
#define RX_BUF          1024            /* tampon du pilote (RAM interne) */
#define WIN_PERIOD_US   (30 * 1000000LL)
#define WIN_MAX_US      (3 * 1000000LL)
#define PROBE_US        (1500 * 1000LL)
#define TSYNC_PERIOD_US (6 * 3600 * 1000000LL)

static const int BAUDS[] = { 38400, 9600, 115200 };   /* 38 400 : défaut du M10 */

static SemaphoreHandle_t s_mtx;
static portMUX_TYPE s_task_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;             /* remis à NULL sous s_task_mux juste avant la fin */
static volatile bool s_run, s_live;
static bool s_on;

/* tâche gnss seule */
EXT_RAM_BSS_ATTR static nmea_t s_p;
/* copie publiée (s_mtx) */
EXT_RAM_BSS_ATTR static gnss_info_t s_pub;
static int64_t s_on_us, s_fix_us, s_ttff_us;
static bool s_seen;

/* UBX-CFG-VALSET (couche RAM) : sorties NMEA inutiles coupées (GLL, VTG) */
static void ubx_send_cfg(void)
{
    static const uint32_t KEYS_OFF[] = {
        0x209100ca,     /* CFG-MSGOUT-NMEA_ID_GLL_UART1 */
        0x209100b1,     /* CFG-MSGOUT-NMEA_ID_VTG_UART1 */
    };
    uint8_t m[6 + 4 + 5 * 2 + 2];
    int n = 0;
    m[n++] = 0xB5; m[n++] = 0x62; m[n++] = 0x06; m[n++] = 0x8A;
    uint16_t len = 4 + 5 * (sizeof KEYS_OFF / sizeof KEYS_OFF[0]);
    m[n++] = len & 0xFF; m[n++] = len >> 8;
    m[n++] = 0; m[n++] = 0x01; m[n++] = 0; m[n++] = 0;    /* version, couche RAM */
    for (size_t i = 0; i < sizeof KEYS_OFF / sizeof KEYS_OFF[0]; i++) {
        for (int b = 0; b < 4; b++) m[n++] = (uint8_t)(KEYS_OFF[i] >> (8 * b));
        m[n++] = 0;
    }
    uint8_t a = 0, b = 0;
    for (int i = 2; i < n; i++) { a += m[i]; b += a; }
    m[n++] = a; m[n++] = b;
    uart_write_bytes(GNSS_UART, m, n);
}

/* âges calculés à la lecture : la tâche dort entre deux fenêtres */
static int64_t s_pub_fix_us, s_pub_ttff_us;

static void publish(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_pub.on = true;
    s_pub.detected = s_seen;
    s_pub_fix_us = s_fix_us;
    s_pub_ttff_us = s_ttff_us;
    s_pub.st = s_p.st;
    xSemaphoreGive(s_mtx);
}

/* sous s_mtx */
static void ages(gnss_info_t *o)
{
    int64_t now = esp_timer_get_time();
    o->on_s = o->on ? (uint32_t)((now - s_on_us) / 1000000) : 0;
    o->ttff_s = o->on && s_pub_ttff_us ? (int32_t)((s_pub_ttff_us - s_on_us) / 1000000) : -1;
    o->fix_age_s = o->on && s_pub_fix_us ? (uint32_t)((now - s_pub_fix_us) / 1000000) : UINT32_MAX;
}

static void time_sync(int64_t now)
{
    static int64_t last;
    if (s_p.st.fix < 2 || !s_p.st.utc) return;
    if (last && now - last < TSYNC_PERIOD_US) return;
    last = now;
    int64_t d = s_p.st.utc - (int64_t)time(NULL);
    if (d >= -2 && d <= 2) return;
    /* même chemin que l'heure donnée par l'iPhone (heure locale, fuseau gardé) */
    app_event_t ev = { .type = EV_TIME_SYNC, .a = INT32_MIN,
                       .t = s_p.st.utc + (int64_t)wclock_tz_offset_min() * 60 };
    app_post(&ev);
    ESP_LOGI(TAG, "heure GNSS : écart de %lld s corrigé", (long long)d);
}

static void gnss_task(void *arg)
{
    uint8_t buf[128];
    int bi = 0;
    bool cfg = false, held = false;
    int64_t probe = esp_timer_get_time(), next_win = 0, win_end = 0;
    uint32_t win_epochs = 0, epochs = 0;
    while (s_run) {
        int64_t now = esp_timer_get_time();
        bool awake = s_live || now < win_end;
        /* éveillé : lecture fine ; sinon on dort jusqu'à la prochaine fenêtre */
        int64_t wait_us = awake ? 100000 : next_win - now;
        if (wait_us < 10000) wait_us = 10000;
        int n = uart_read_bytes(GNSS_UART, buf, sizeof buf, pdMS_TO_TICKS(wait_us / 1000));
        now = esp_timer_get_time();
        if (n > 0 && nmea_feed(&s_p, buf, n) > 0) {
            if (!s_seen) ESP_LOGI(TAG, "module détecté à %d bauds", BAUDS[bi]);
            s_seen = true;
            if (s_p.st.epochs != epochs) {
                epochs = s_p.st.epochs;
                if (s_p.st.fix >= 2) {
                    s_fix_us = now;
                    if (!s_ttff_us) {
                        s_ttff_us = now;
                        ESP_LOGI(TAG, "premier fix en %lld s, %u satellites",
                                 (long long)((now - s_on_us) / 1000000), s_p.st.sats_used);
                    }
                    time_sync(now);
                }
            }
            publish();
        }
        if (!s_seen && now - probe > PROBE_US) {            /* pas de trame : autre vitesse */
            bi = (bi + 1) % (int)(sizeof BAUDS / sizeof BAUDS[0]);
            uart_set_baudrate(GNSS_UART, BAUDS[bi]);
            uart_flush_input(GNSS_UART);
            probe = now;
        }
        if (s_seen && !cfg) { ubx_send_cfg(); cfg = true; }

        /* fenêtres de lecture écran GPS fermé */
        if (!s_live && now >= next_win) {
            next_win = now + WIN_PERIOD_US;
            win_end = now + WIN_MAX_US;
            win_epochs = s_p.st.epochs;
            uart_flush_input(GNSS_UART);                    /* restes d'avant le sommeil */
        } else if (now < win_end && s_seen && s_p.st.epochs >= win_epochs + 2) {
            win_end = 0;                                    /* deux époques lues : fini */
        }
        /* module muet : on le cherche éveillé 20 s, puis par fenêtres */
        bool want = s_live || now < win_end || (!s_seen && now - s_on_us < 20 * 1000000LL);
        if (want != held) { power_hold_nosleep(want); held = want; }
    }
    if (held) power_hold_nosleep(false);
    portENTER_CRITICAL(&s_task_mux);
    s_task = NULL;
    portEXIT_CRITICAL(&s_task_mux);
    task_psram_exit();
}

/* sort la tâche d'une attente (jusqu'à 30 s entre deux fenêtres) */
static void kick(void)
{
    portENTER_CRITICAL(&s_task_mux);
    if (s_task) xTaskAbortDelay(s_task);
    portEXIT_CRITICAL(&s_task_mux);
}

static void pins_idle(void)
{
    const gpio_num_t pins[] = { BOARD_GPS_TX, BOARD_GPS_RX };
    for (size_t i = 0; i < sizeof pins / sizeof pins[0]; i++) {
        gpio_sleep_sel_en(pins[i]);
        gpio_reset_pin(pins[i]);
        gpio_set_pull_mode(pins[i], GPIO_FLOATING);
        gpio_set_direction(pins[i], GPIO_MODE_DISABLE);
    }
}

static void power_on(void)
{
    if (s_on) return;
    if (axp_rail(AXP_RAIL_GPS, true) != ESP_OK) { ESP_LOGE(TAG, "rail BLDO1 : écriture ratée"); return; }
    const uart_config_t uc = {
        .baud_rate = BAUDS[0],
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_XTAL,             /* indépendant de la fréquence CPU (DFS) */
    };
    if (uart_driver_install(GNSS_UART, RX_BUF, 0, 0, NULL, 0) != ESP_OK ||
        uart_param_config(GNSS_UART, &uc) != ESP_OK ||
        uart_set_pin(GNSS_UART, BOARD_GPS_TX, BOARD_GPS_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        ESP_LOGE(TAG, "UART indisponible");
        uart_driver_delete(GNSS_UART);
        axp_rail(AXP_RAIL_GPS, false);
        pins_idle();
        return;
    }
    gpio_set_pull_mode(BOARD_GPS_RX, GPIO_PULLUP_ONLY);
    gpio_sleep_sel_dis(BOARD_GPS_TX);             /* ligne tenue au repos (haut) en light sleep */
    gpio_sleep_sel_dis(BOARD_GPS_RX);
    nmea_init(&s_p);
    s_seen = false;
    s_on_us = esp_timer_get_time();
    s_fix_us = s_ttff_us = 0;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    memset(&s_pub, 0, sizeof s_pub);
    s_pub.on = true;
    s_pub_fix_us = s_pub_ttff_us = 0;
    xSemaphoreGive(s_mtx);
    s_run = true;
    if (task_psram(gnss_task, "gnss", 4096, NULL, 4, &s_task, 1) != pdPASS) {
        s_run = false;
        uart_driver_delete(GNSS_UART);
        axp_rail(AXP_RAIL_GPS, false);
        pins_idle();
        ESP_LOGE(TAG, "tâche impossible");
        return;
    }
    s_on = true;
    ESP_LOGI(TAG, "allumé");
}

static void power_off(void)
{
    if (!s_on) return;
    s_run = false;
    for (int i = 0; i < 200 && s_task; i++) { kick(); vTaskDelay(pdMS_TO_TICKS(10)); }
    if (s_task) {                       /* jamais vu : on garde l'UART plutôt que de planter */
        ESP_LOGE(TAG, "la tâche ne s'arrête pas");
        return;
    }
    uart_driver_delete(GNSS_UART);
    axp_rail(AXP_RAIL_GPS, false);
    pins_idle();
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_pub.on = false;
    s_pub.detected = false;
    xSemaphoreGive(s_mtx);
    s_on = false;
    ESP_LOGI(TAG, "coupé");
}

void gnss_init(void)
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    if (settings_get()->gps_on) power_on();
}

void gnss_set(bool on)
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    settings_t *st = settings_get();
    if (st->gps_on != on) { st->gps_on = on; settings_save(); }
    if (on) power_on();
    else power_off();
}

bool gnss_is_on(void) { return s_on; }

void gnss_live(bool live)
{
    s_live = live;
    kick();
}

void gnss_get(gnss_info_t *out)
{
    if (!s_mtx) { memset(out, 0, sizeof *out); out->ttff_s = -1; out->fix_age_s = UINT32_MAX; return; }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *out = s_pub;
    ages(out);
    xSemaphoreGive(s_mtx);
    if (!out->on) out->detected = false;
}

bool gnss_position(double *lat, double *lon, float *alt_m, bool *alt_ok, uint32_t max_age_s)
{
    if (!s_mtx || !s_on) return false;
    bool ok;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int64_t age = esp_timer_get_time() - s_pub_fix_us;
    ok = s_pub.on && s_pub.st.fix >= 2 && s_pub_fix_us && age <= (int64_t)max_age_s * 1000000;
    if (ok) {
        *lat = s_pub.st.lat;
        *lon = s_pub.st.lon;
        *alt_m = s_pub.st.alt_m;
        *alt_ok = s_pub.st.alt_ok;
    }
    xSemaphoreGive(s_mtx);
    return ok;
}
