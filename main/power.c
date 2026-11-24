#include "power.h"
#include <string.h>
#include "nvs.h"
#include "esp_system.h"
#include "axp2101.h"
#include "xl9555.h"
#include "haptic.h"
#include "display.h"
#include "board.h"
#include "motion.h"
#include "app_events.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "power";

#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_lock_cpu, s_lock_nosleep;
#endif
static int s_cnt_active, s_cnt_nosleep;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static int s_percent = -1;
static bool s_vbus, s_charging;

/* ------------------------------------------------------------------ */
/* Verrous PM                                                          */
/* ------------------------------------------------------------------ */
void power_hold_active(bool hold)
{
    portENTER_CRITICAL(&s_mux);
    bool acquire = hold && (s_cnt_active++ == 0);
    bool release = !hold && s_cnt_active > 0 && (--s_cnt_active == 0);
    portEXIT_CRITICAL(&s_mux);
#if CONFIG_PM_ENABLE
    if (acquire) { esp_pm_lock_acquire(s_lock_cpu); power_hold_nosleep(true); }
    if (release) { esp_pm_lock_release(s_lock_cpu); power_hold_nosleep(false); }
#else
    (void)acquire; (void)release;
#endif
}

/* Plafond du CPU. Avec un maximum de 240 MHz, le mode « APB max » (exigé par
 * le Wi-Fi, l'I2S…) tourne lui aussi à 240 MHz : la radio en marche garde le
 * CPU à fond même écran éteint. Plafonné à 160, ce mode descend à 80 MHz. */
static int s_max_mhz = POWER_CPU_MAX_MHZ;
static int s_cnt_floor;            /* demandes de plancher à POWER_CPU_FLOOR_MHZ */

static void pm_apply(void)
{
#if CONFIG_PM_ENABLE
    portENTER_CRITICAL(&s_mux);
    esp_pm_config_t pm = {
        .max_freq_mhz = s_max_mhz,
        .min_freq_mhz = s_cnt_floor ? POWER_CPU_FLOOR_MHZ : POWER_CPU_MIN_MHZ,
        .light_sleep_enable = true,
    };
    portEXIT_CRITICAL(&s_mux);
    esp_err_t err = esp_pm_configure(&pm);
    if (err != ESP_OK) ESP_LOGE(TAG, "esp_pm_configure(%d-%d): %s", pm.min_freq_mhz, pm.max_freq_mhz,
                                esp_err_to_name(err));
#endif
}

void power_set_cpu_max(int mhz)
{
    s_max_mhz = mhz > 0 ? mhz : POWER_CPU_MAX_MHZ;
    pm_apply();
}

/* Plancher de fréquence (appli MeshCore connectée) : le CPU ne descend
 * plus sous 80 MHz au repos, pour répondre sans délai de remontée. */
void power_hold_cpu_floor(bool hold)
{
    portENTER_CRITICAL(&s_mux);
    bool change = hold ? (s_cnt_floor++ == 0) : (s_cnt_floor > 0 && --s_cnt_floor == 0);
    portEXIT_CRITICAL(&s_mux);
    if (change) pm_apply();
}

void power_hold_nosleep(bool hold)
{
    portENTER_CRITICAL(&s_mux);
    bool acquire = hold && (s_cnt_nosleep++ == 0);
    bool release = !hold && s_cnt_nosleep > 0 && (--s_cnt_nosleep == 0);
    portEXIT_CRITICAL(&s_mux);
#if CONFIG_PM_ENABLE
    if (acquire) esp_pm_lock_acquire(s_lock_nosleep);
    if (release) esp_pm_lock_release(s_lock_nosleep);
#else
    (void)acquire; (void)release;
#endif
}

/* ------------------------------------------------------------------ */
/* IRQ AXP2101 : niveau bas sur GPIO7. L'ISR masque la ligne et poste  */
/* un événement ; la tâche app lit/acquitte via I2C puis démasque.     */
/* Niveau (et non front) : aucune IRQ perdue pendant un light sleep.   */
/* ------------------------------------------------------------------ */
static void IRAM_ATTR axp_isr(void *arg)
{
    gpio_intr_disable(BOARD_PMU_INT);
    BaseType_t hp = pdFALSE;
    app_event_t ev = { .type = EV_AXP_IRQ };
    app_post_from_isr(&ev, &hp);
    if (hp) portYIELD_FROM_ISR();
}

/* ------------------------------------------------------------------ */
/* Période en cours (sur batterie, ou charge) et part du temps écran.   */
/* L'AXP2101 ne mesure pas le courant batterie : la consommation        */
/* moyenne est estimée d'après la baisse du % de sa jauge (1 % =        */
/* 11 mAh), donc seulement après quelques heures.                       */
/* ------------------------------------------------------------------ */
/* Temps passé en light sleep (rappel de sortie du sommeil, tâche idle,
 * interruptions masquées : un simple cumul). Avec le temps écran éteint,
 * il dit si la montre dort vraiment quand elle ne fait rien. */
static volatile int64_t  s_sleep_us;
static volatile uint32_t s_sleep_n;
static int64_t  s_sleep_us0;
static uint32_t s_sleep_n0;

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
static esp_err_t IRAM_ATTR sleep_exit_cb(int64_t slept_us, void *arg)
{
    if (slept_us > 0) {
        s_sleep_us += slept_us;
        s_sleep_n++;
    }
    return ESP_OK;
}
#endif

static int64_t s_period_start_us;
static int     s_pct_start = -1;
static bool    s_seg_on;
static int64_t s_seg_us, s_on_us, s_off_us;

static void period_restart(void)
{
    s_period_start_us = s_seg_us = esp_timer_get_time();
    s_on_us = s_off_us = 0;
    s_pct_start = s_percent;
    s_sleep_us0 = s_sleep_us;
    s_sleep_n0 = s_sleep_n;
}

void power_screen_segment(bool on)
{
    int64_t now = esp_timer_get_time();
    if (s_seg_on) s_on_us += now - s_seg_us; else s_off_us += now - s_seg_us;
    s_seg_us = now;
    s_seg_on = on;
}

static float screen_pct_now(void)
{
    int64_t now = esp_timer_get_time();
    int64_t on = s_on_us, off = s_off_us;
    if (s_seg_on) on += now - s_seg_us; else off += now - s_seg_us;
    return on + off > 0 ? (float)(100.0 * on / (double)(on + off)) : 0;
}

/* ------------------------------------------------------------------ */
/* Historique de décharge (NVS) : survit à une coupure batterie vide.   */
/* Instantané toutes les 10 min sur batterie ; au démarrage suivant (ou */
/* au branchement USB), il devient la « décharge précédente ».          */
/* ------------------------------------------------------------------ */
#define HIST_MAGIC     0x42484954u
#define HIST_PERIOD_US (10LL * 60 * 1000000)

typedef struct {
    uint32_t magic;
    power_hist_t h;
} bhist_t;

static power_hist_t s_last;
static bool s_last_valid;
static int64_t s_hist_last_us;

static bool hist_get(nvs_handle_t h, const char *k, bhist_t *b)
{
    size_t n = sizeof(*b);
    return nvs_get_blob(h, k, b, &n) == ESP_OK && n == sizeof(*b) && b->magic == HIST_MAGIC;
}

/* « bcur » (décharge en cours) → « blast » (précédente) */
static void hist_promote(uint8_t end_reason)
{
    nvs_handle_t h;
    if (nvs_open("tw", NVS_READWRITE, &h) != ESP_OK) return;
    bhist_t b;
    if (hist_get(h, "bcur", &b)) {
        if (b.h.end == POWER_HIST_RUNNING) b.h.end = end_reason;
        if (b.h.since_s >= 1800) nvs_set_blob(h, "blast", &b, sizeof(b));
        nvs_erase_key(h, "bcur");
        nvs_commit(h);
    }
    if (hist_get(h, "blast", &b)) { s_last = b.h; s_last_valid = true; }
    nvs_close(h);
}

static void hist_snapshot(uint8_t end)
{
    int64_t now = esp_timer_get_time();
    s_hist_last_us = now;
    uint32_t since = (uint32_t)((now - s_period_start_us) / 1000000);
    if (since < 600 || s_pct_start < 0) return;
    bhist_t b = {
        .magic = HIST_MAGIC,
        .h = {
            .since_s = since, .vbat_mv = (uint16_t)axp_batt_mv(),
            .pct_start = (uint8_t)s_pct_start, .pct_end = (uint8_t)(s_percent < 0 ? 0 : s_percent),
            .screen_pct = (uint8_t)(screen_pct_now() + 0.5f), .end = end,
        },
    };
    nvs_handle_t h;
    if (nvs_open("tw", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "bcur", &b, sizeof(b));
        nvs_commit(h);
        nvs_close(h);
    }
}

bool power_last_discharge(power_hist_t *out)
{
    if (s_last_valid) *out = s_last;
    return s_last_valid;
}

/* Câble USB branché : pas de light sleep. La console USB Serial/JTAG se
 * coupe à chaque sommeil (journal haché, port qui disparaît) ; sur
 * chargeur, la consommation n'importe pas. */
static bool s_usb_hold;
static void usb_hold_update(void)
{
    if (s_vbus == s_usb_hold) return;
    s_usb_hold = s_vbus;
    power_hold_nosleep(s_vbus);
}

uint32_t power_handle_irq(void)
{
    uint32_t m = axp_irq_read_and_clear();
    if (m & (AXP_IRQ_VBUS_CONNECT | AXP_IRQ_VBUS_REMOVED | AXP_IRQ_CHG_START | AXP_IRQ_CHG_DONE)) {
        bool was_vbus = s_vbus;
        s_vbus = axp_vbus_present();
        s_charging = axp_is_charging();
        if (!was_vbus && s_vbus) {
            /* fin d'une période sur batterie : dernier instantané, archivé */
            hist_snapshot(POWER_HIST_CHARGED);
            hist_promote(POWER_HIST_CHARGED);
            period_restart();
        }
        if (was_vbus && !s_vbus) period_restart();   /* début d'une période sur batterie */
        usb_hold_update();
    }
    gpio_intr_enable(BOARD_PMU_INT);
    return m;
}

/* ------------------------------------------------------------------ */
/* Vibreur : petit lecteur de motif sur esp_timer                       */
/* ------------------------------------------------------------------ */
const uint16_t VIB_SHORT[] = { 40, 0 };
const uint16_t VIB_NOTIF[] = { 90, 110, 90, 0 };
const uint16_t VIB_CALL[]  = { 450, 300, 450, 300, 450, 0 };

static esp_timer_handle_t s_vib_timer;
static const uint16_t *s_vib_pat;
static int s_vib_idx;

static void vib_step(void *arg)
{
    uint16_t d = s_vib_pat ? s_vib_pat[s_vib_idx] : 0;
    if (d == 0) {
        haptic_set(false);
        s_vib_pat = NULL;
        return;
    }
    haptic_set(!(s_vib_idx & 1));
    s_vib_idx++;
    esp_timer_start_once(s_vib_timer, (uint64_t)d * 1000);
}

void power_vibrate(const uint16_t *pattern)
{
    /* appelé depuis la tâche app uniquement ; le DRV2605 tient le rythme
     * tout seul entre deux commandes : le light sleep reste permis */
    esp_timer_stop(s_vib_timer);
    s_vib_pat = pattern;
    s_vib_idx = 0;
    vib_step(NULL);
}

/* ------------------------------------------------------------------ */
/* Luminosité : registre de l'AMOLED (pas de rétroéclairage)            */
/* ------------------------------------------------------------------ */
void power_backlight(uint8_t level) { display_panel_brightness(level); }

/* ------------------------------------------------------------------ */
/* Broches des périphériques non alimentés : haute impédance, ni entrée */
/* ni sortie. Une E/S tenue à 3,3 V vers une puce sans alimentation la  */
/* nourrit par ses diodes de protection (courant perdu, puce dans un    */
/* état indéfini). Chaque pilote reprend ses broches quand il allume    */
/* son rail.                                                            */
/* ------------------------------------------------------------------ */
static void idle_gpio_setup(void)
{
    const gpio_num_t pins[] = {
        BOARD_SPI_MOSI, BOARD_SPI_MISO, BOARD_SPI_SCK,          /* bus SPI : LoRa, NFC, SD */
        BOARD_SD_CS, BOARD_NFC_CS, BOARD_NFC_INT,
        BOARD_LORA_CS, BOARD_LORA_RST, BOARD_LORA_BUSY, BOARD_LORA_IRQ,
        BOARD_GPS_TX, BOARD_GPS_RX, BOARD_GPS_PPS,              /* GNSS (BLDO1 coupé) */
        BOARD_MIC_CLK, BOARD_MIC_DAT,                           /* micro PDM, inutilisé */
        BOARD_I2S_BCLK, BOARD_I2S_WCLK, BOARD_I2S_DOUT,         /* ampli (BLDO2 coupé) */
        BOARD_RTC_INT,                                          /* alarme RTC, inutilisée */
    };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        gpio_reset_pin(pins[i]);
        gpio_set_pull_mode(pins[i], GPIO_FLOATING);
        gpio_set_direction(pins[i], GPIO_MODE_DISABLE);
    }
    /* Le micro PDM est sur le 3,3 V permanent (schéma) : son entrée
     * d'horloge flottante ferait fuir son étage d'entrée. Tirée au niveau
     * bas, aussi pendant le light sleep. */
    gpio_set_pull_mode(BOARD_MIC_CLK, GPIO_PULLDOWN_ONLY);
    gpio_sleep_set_direction(BOARD_MIC_CLK, GPIO_MODE_INPUT);
    gpio_sleep_set_pull_mode(BOARD_MIC_CLK, GPIO_PULLDOWN_ONLY);
}

/* ------------------------------------------------------------------ */
esp_err_t power_init(void)
{
    idle_gpio_setup();

    ESP_ERROR_CHECK(axp_init());
    if (xl9555_init() != ESP_OK) ESP_LOGE(TAG, "XL9555 absent : écran et vibreur sans alimentation");
    xl9555_output(XIO_LORA_RF_SW, false);    /* 0 = antenne LoRa intégrée, 1 = externe (vérifié sur la montre) */

    esp_timer_create_args_t ta = { .callback = vib_step, .name = "vib" };
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_vib_timer));
    haptic_init();

    s_vbus = axp_vbus_present();
    s_charging = axp_is_charging();
    s_percent = axp_percent();
    period_restart();
    {
        /* décharge précédente : coupure (batterie vide / bouton) ou redémarrage */
        esp_reset_reason_t rr = esp_reset_reason();
        hist_promote((rr == ESP_RST_POWERON || rr == ESP_RST_BROWNOUT) ? POWER_HIST_EMPTY
                                                                       : POWER_HIST_REBOOT);
    }

    axp_irq_enable(AXP_IRQ_VBUS_CONNECT | AXP_IRQ_VBUS_REMOVED | AXP_IRQ_CHG_START |
                   AXP_IRQ_CHG_DONE | AXP_IRQ_PEK_SHORT | AXP_IRQ_PEK_LONG |
                   AXP_IRQ_SOC_WARN1 | AXP_IRQ_SOC_WARN2);

    /* Après un esp_restart (fin d'OTA), la matrice GPIO garde la config de
     * l'ancien firmware : interruptions TE (60 Hz), tactile, capteur encore
     * armées. On repart d'un état neutre avant d'installer le service. */
    for (int i = 0; i < GPIO_NUM_MAX; i++) {
        if (!GPIO_IS_VALID_GPIO(i)) continue;
        gpio_intr_disable(i);
        gpio_set_intr_type(i, GPIO_INTR_DISABLE);
    }

    /* GPIO7 : IRQ niveau bas + source de réveil du light sleep */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_PMU_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_LOW_LEVEL,
    };
    gpio_config(&io);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BOARD_PMU_INT, axp_isr, NULL);
    gpio_wakeup_enable(BOARD_PMU_INT, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();

    /* BHI260AP : coupé, ou compteur de pas / lever de poignet (réglages) */
    if (motion_init() != ESP_OK) ESP_LOGW(TAG, "BHI260AP indisponible");

    /* PM : DFS + light sleep automatique. Sur ESP32-S3, avec l'horloge lente
     * sur le quartz 32 kHz, le contrôleur BLE dort entre deux événements de
     * connexion : le light sleep reste possible iPhone connecté. */
#if CONFIG_PM_ENABLE
    ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "active", &s_lock_cpu));
    ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "nosleep", &s_lock_nosleep));
    power_set_cpu_max(0);
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    esp_pm_sleep_cbs_register_config_t cbs = { .exit_cb = sleep_exit_cb };
    if (esp_pm_light_sleep_register_cbs(&cbs) != ESP_OK) ESP_LOGW(TAG, "mesure du light sleep indisponible");
#endif
#else
    ESP_LOGW(TAG, "PM désactivé (build de diagnostic)");
#endif

    power_update();
    ESP_LOGI(TAG, "PM %d-%d MHz, batterie %d %% (%d mV)", POWER_CPU_MIN_MHZ, POWER_CPU_MAX_MHZ,
             s_percent, axp_batt_mv());
    return ESP_OK;
}

void power_update(void)
{
    s_vbus = axp_vbus_present();
    usb_hold_update();
    s_charging = axp_is_charging();
    int p = axp_percent();
    if (p >= 0) s_percent = p;
    if (s_pct_start < 0) s_pct_start = s_percent;
    if (!s_vbus && esp_timer_get_time() - s_hist_last_us >= HIST_PERIOD_US) hist_snapshot(POWER_HIST_RUNNING);
}

int  power_percent(void)  { return s_percent < 0 ? 0 : s_percent; }
bool power_vbus(void)     { return s_vbus; }
bool power_charging(void) { return s_charging; }

void power_get_telemetry(power_telemetry_t *t)
{
    memset(t, 0, sizeof(*t));
    t->vbat_mv = axp_batt_mv();
    t->vbus_mv = axp_vbus_mv();
    t->vbus = axp_vbus_present();
    t->charging = axp_is_charging();
    t->charge_done = axp_charge_done();
    t->percent = power_percent();
    t->temp_c = axp_die_temp_c();
    t->since_s = (uint32_t)((esp_timer_get_time() - s_period_start_us) / 1000000);
    t->pct_start = s_pct_start;
    int drop = s_pct_start - t->percent;
    if (!t->vbus && drop >= 2 && t->since_s >= 3600)
        t->avg_ma = drop * (BATTERY_CAPACITY_MAH / 100.0f) / (t->since_s / 3600.0f);
    t->screen_pct = screen_pct_now();

    /* le sommeil n'a lieu qu'écran éteint : rapporté au temps écran éteint */
    int64_t now = esp_timer_get_time();
    int64_t off = s_off_us + (s_seg_on ? 0 : now - s_seg_us);
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    if (off >= 60LL * 1000000) {
        int64_t slept = s_sleep_us - s_sleep_us0;
        t->sleep_known = true;
        t->sleep_pct = (float)(100.0 * slept / (double)off);
        if (t->sleep_pct > 100.0f) t->sleep_pct = 100.0f;
        t->wakes_per_min = (float)((s_sleep_n - s_sleep_n0) * 60e6 / (double)off);
    }
#else
    (void)off;
#endif
}
