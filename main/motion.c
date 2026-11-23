/* motion.c — voir motion.h. Capteur intelligent Bosch BHI260AP. */
#include "motion.h"
#include <string.h>
#include <time.h>
#include "bhy2.h"
#include "bhy2_parse.h"
#include "board.h"
#include "i2c_bus.h"
#include "axp2101.h"
#include "settings.h"
#include "wclock.h"
#include "app_events.h"
#include "ble.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "motion";

extern const uint8_t bhi_fw_start[] asm("_binary_BHI260AP_fw_start");
extern const uint8_t bhi_fw_end[]   asm("_binary_BHI260AP_fw_end");

#define SAVE_PERIOD_US (15LL * 60 * 1000000)
#define READ_CACHE_US  (2LL * 1000000)
#define I2C_FAST_HZ    1000000      /* chargement du firmware (~100 Ko) en ~1 s */
#define RW_LEN         240          /* morceaux I2C (pilote Bosch) */

static i2c_master_dev_handle_t s_i2c;
static struct bhy2_dev s_dev;
static bool s_booted;                  /* firmware chargé, capteur prêt */
static bool s_steps, s_wrist;          /* fonctions actuellement actives */
static bool s_isr;
static uint8_t s_gesture_id;           /* capteur virtuel du geste (selon le firmware) */
static volatile bool s_gesture_seen;
EXT_RAM_BSS_ATTR static uint8_t s_fifo[2048];

/* pas */
static uint32_t s_day;                 /* AAAAMMJJ local du compteur, 0 = inconnu */
static uint32_t s_base;                /* pas du jour hors compteur du capteur */
static uint32_t s_hist[MOTION_HIST_DAYS];
static uint32_t s_saved = UINT32_MAX;  /* total du jour à la dernière sauvegarde */
static int64_t  s_save_us;
static uint32_t s_cache; static int64_t s_cache_us = -1;
static uint32_t s_sensor;              /* dernière valeur du compteur du capteur */
static uint32_t s_sensor_zero;         /* valeur du compteur au dernier « zéro » logiciel */

/* accéléromètre brut (apps) */
static bool s_raw;
static struct bhy2_data_xyz s_acc;
static bool s_acc_new;

/* ------------------------------------------------------------------ */
/* bus pour le pilote Bosch                                            */
/* ------------------------------------------------------------------ */
static int8_t bus_read(uint8_t reg, uint8_t *d, uint32_t len, void *p)
{
    return i2c_master_transmit_receive(s_i2c, &reg, 1, d, len, 200) == ESP_OK ? 0 : -1;
}

static int8_t bus_write(uint8_t reg, const uint8_t *d, uint32_t len, void *p)
{
    i2c_master_transmit_multi_buffer_info_t b[2] = {
        { .write_buffer = &reg, .buffer_size = 1 },
        { .write_buffer = (uint8_t *)d, .buffer_size = len },
    };
    return i2c_master_multi_buffer_transmit(s_i2c, b, 2, 200) == ESP_OK ? 0 : -1;
}

static void bus_delay(uint32_t us, void *p)
{
    if (us >= 1000) vTaskDelay(pdMS_TO_TICKS((us + 999) / 1000));
    else esp_rom_delay_us(us);
}

/* ------------------------------------------------------------------ */
/* persistance                                                          */
/* ------------------------------------------------------------------ */
static uint32_t today_key(void)
{
    if (!wclock_time_valid()) return 0;
    struct tm t;
    wclock_local_tm(&t);
    return (uint32_t)((t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday);
}

static void load(void)
{
    nvs_handle_t h;
    if (nvs_open("motion", NVS_READONLY, &h) != ESP_OK) return;
    nvs_get_u32(h, "day", &s_day);
    nvs_get_u32(h, "today", &s_base);
    size_t n = sizeof s_hist;
    nvs_get_blob(h, "hist", s_hist, &n);
    nvs_close(h);
    s_saved = s_base;
}

static void fifo_poll(void)
{
    if (s_booted) bhy2_get_and_process_fifo(s_fifo, sizeof s_fifo, &s_dev);
}

/* pas comptés par le capteur depuis le dernier zéro logiciel. Le compteur
 * du BHI260AP ne se remet pas à zéro sans redémarrer le firmware : on garde
 * sa valeur au zéro et on soustrait. */
static uint32_t sensor_steps(void)
{
    if (!s_steps) return 0;
    fifo_poll();
    return s_sensor >= s_sensor_zero ? s_sensor - s_sensor_zero : 0;
}

static void sensor_zero(void) { fifo_poll(); s_sensor_zero = s_sensor; }

static void save(void)
{
    uint32_t total = s_base + sensor_steps();
    nvs_handle_t h;
    if (nvs_open("motion", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "day", s_day);
    nvs_set_u32(h, "today", total);
    nvs_set_blob(h, "hist", s_hist, sizeof s_hist);
    nvs_commit(h);
    nvs_close(h);
    s_saved = total;
    s_save_us = esp_timer_get_time();
}

static void shutdown_save(void)
{
    if (s_steps && s_base + sensor_steps() != s_saved) save();
}

/* jour écoulé : on pousse le total dans l'historique (autant de jours que
 * nécessaire si la montre est restée éteinte) et on repart de zéro */
static void rollover(uint32_t today)
{
    if (s_day == today) return;
    if (s_day == 0) { s_day = today; return; }   /* première date connue */
    uint32_t total = s_base + sensor_steps();
    struct tm a = { .tm_year = (int)(s_day / 10000) - 1900, .tm_mon = (int)(s_day / 100 % 100) - 1,
                    .tm_mday = (int)(s_day % 100), .tm_hour = 12 };
    struct tm b = { .tm_year = (int)(today / 10000) - 1900, .tm_mon = (int)(today / 100 % 100) - 1,
                    .tm_mday = (int)(today % 100), .tm_hour = 12 };
    long days = (long)((mktime(&b) - mktime(&a)) / 86400);
    if (days < 1) days = 1;                      /* heure reculée : on ne garde pas l'ancien jour */
    for (int k = 0; k < days && k < MOTION_HIST_DAYS; k++) {
        memmove(&s_hist[1], &s_hist[0], (MOTION_HIST_DAYS - 1) * sizeof s_hist[0]);
        s_hist[0] = k == 0 ? total : 0;
    }
    if (days >= MOTION_HIST_DAYS + 1) memset(s_hist, 0, sizeof s_hist);
    if (s_steps) sensor_zero();
    s_base = 0;
    s_day = today;
    s_cache_us = -1;
    save();
    ESP_LOGI(TAG, "nouveau jour %lu (hier %lu pas)", (unsigned long)today, (unsigned long)s_hist[0]);
}

/* ------------------------------------------------------------------ */
/* capteur                                                              */
/* ------------------------------------------------------------------ */
static void IRAM_ATTR bhi_isr(void *arg)
{
    gpio_intr_disable(BOARD_SENSOR_INT);
    BaseType_t hp = pdFALSE;
    app_event_t ev = { .type = EV_MOTION };
    app_post_from_isr(&ev, &hp);
    if (hp) portYIELD_FROM_ISR();
}

static void on_steps(const struct bhy2_fifo_parse_data_info *info, void *ref)
{
    if (info->data_size >= 5) s_sensor = BHY2_LE2U32(info->data_ptr);
}

static void on_gesture(const struct bhy2_fifo_parse_data_info *info, void *ref) { s_gesture_seen = true; }

static void on_acc(const struct bhy2_fifo_parse_data_info *info, void *ref)
{
    if (info->data_size >= 7) { bhy2_parse_xyz(info->data_ptr, &s_acc); s_acc_new = true; }
}

static void isr_setup(bool on)
{
    if (on && !s_isr) {
        gpio_config_t io = { .pin_bit_mask = 1ULL << BOARD_SENSOR_INT, .mode = GPIO_MODE_INPUT,
                             .pull_down_en = GPIO_PULLDOWN_ENABLE, .intr_type = GPIO_INTR_HIGH_LEVEL };
        gpio_config(&io);
        gpio_isr_handler_add(BOARD_SENSOR_INT, bhi_isr, NULL);   /* service installé par power_init */
        gpio_wakeup_enable(BOARD_SENSOR_INT, GPIO_INTR_HIGH_LEVEL);
        s_isr = true;
    } else if (!on && s_isr) {
        gpio_wakeup_disable(BOARD_SENSOR_INT);
        gpio_isr_handler_remove(BOARD_SENSOR_INT);
        gpio_intr_disable(BOARD_SENSOR_INT);
        gpio_reset_pin(BOARD_SENSOR_INT);
        gpio_set_direction(BOARD_SENSOR_INT, GPIO_MODE_DISABLE);
        s_isr = false;
    }
}

/* rail ALDO4 coupé : le capteur ne consomme rien */
static void power_off(void)
{
    isr_setup(false);
    if (s_i2c) { i2c_master_bus_rm_device(s_i2c); s_i2c = NULL; }
    axp_rail(AXP_RAIL_SENSOR, false);
    s_booted = false;
    s_steps = s_wrist = false;
}

static bool boot(void)
{
    int64_t t0 = esp_timer_get_time();
    axp_rail(AXP_RAIL_SENSOR, true);
    vTaskDelay(pdMS_TO_TICKS(10));
    if (!s_i2c && i2c_bus_add(BOARD_BHI260_ADDR, I2C_FAST_HZ, &s_i2c) != ESP_OK) {
        s_i2c = NULL;
        ESP_LOGE(TAG, "BHI260AP absent");
        return false;
    }
    int8_t r = bhy2_init(BHY2_I2C_INTERFACE, bus_read, bus_write, bus_delay, RW_LEN, NULL, &s_dev);
    if (r == BHY2_OK) r = bhy2_soft_reset(&s_dev);
    uint8_t id = 0, st = 0;
    if (r == BHY2_OK) r = bhy2_get_product_id(&id, &s_dev);
    if (r != BHY2_OK || id != BHY2_PRODUCT_ID) {
        ESP_LOGE(TAG, "BHI260AP : id 0x%02x (err %d)", id, r);
        return false;
    }
    /* IRQ : niveau haut, push-pull, FIFO de réveil seule (le geste) ; le
     * compteur de pas s'accumule dans la FIFO normale, lue à la demande */
    bhy2_set_host_interrupt_ctrl(BHY2_ICTL_DISABLE_STATUS_FIFO | BHY2_ICTL_DISABLE_DEBUG |
                                 BHY2_ICTL_DISABLE_FIFO_NW, &s_dev);
    bhy2_set_host_intf_ctrl(0, &s_dev);
    bhy2_get_boot_status(&st, &s_dev);
    if (!(st & BHY2_BST_HOST_INTERFACE_READY)) { ESP_LOGE(TAG, "interface hôte non prête"); return false; }
    r = bhy2_upload_firmware_to_ram(bhi_fw_start, (uint32_t)(bhi_fw_end - bhi_fw_start), &s_dev);
    if (r == BHY2_OK) r = bhy2_boot_from_ram(&s_dev);
    uint16_t kv = 0;
    if (r == BHY2_OK) r = bhy2_get_kernel_version(&kv, &s_dev);
    if (r != BHY2_OK || kv == 0) {
        ESP_LOGE(TAG, "firmware BHI260AP non démarré (err %d)", r);
        ble_diag("BHI fw e%d", r);
        return false;
    }
    bhy2_update_virtual_sensor_list(&s_dev);
    /* Ultra : puce retournée de 180° autour de Z (LilyGoLib,
     * TOP_LAYER_BOTTOM_RIGHT_CORNER) */
    struct bhy2_orient_matrix m = { .c = { -1, 0, 0, 0, -1, 0, 0, 0, 1 } };
    bhy2_set_orientation_matrix(BHY2_PHYS_SENSOR_ID_ACCELEROMETER, m, &s_dev);
    bhy2_set_orientation_matrix(BHY2_PHYS_SENSOR_ID_GYROSCOPE, m, &s_dev);

    static const uint8_t GESTURES[] = { BHY2_SENSOR_ID_WRIST_TILT_GESTURE, BHY2_SENSOR_ID_WAKE_GESTURE,
                                        BHY2_SENSOR_ID_GLANCE_GESTURE };
    s_gesture_id = 0;
    for (size_t i = 0; i < sizeof GESTURES && !s_gesture_id; i++)
        if (bhy2_is_sensor_available(GESTURES[i], &s_dev)) s_gesture_id = GESTURES[i];
    bhy2_register_fifo_parse_callback(BHY2_SENSOR_ID_STC, on_steps, NULL, &s_dev);
    bhy2_register_fifo_parse_callback(BHY2_SENSOR_ID_ACC, on_acc, NULL, &s_dev);
    if (s_gesture_id) bhy2_register_fifo_parse_callback(s_gesture_id, on_gesture, NULL, &s_dev);
    bhy2_get_and_process_fifo(s_fifo, sizeof s_fifo, &s_dev);   /* événements de démarrage */
    s_sensor = s_sensor_zero = 0;
    ESP_LOGI(TAG, "BHI260AP prêt en %d ms (noyau %u, geste %u)",
             (int)((esp_timer_get_time() - t0) / 1000), kv, s_gesture_id);
    return true;
}

void motion_apply(void)
{
    settings_t *st = settings_get();
    bool steps = st->step_count, wrist = st->wrist_wake;

    if (s_steps && !steps) {                   /* on fige le total du jour */
        s_base += sensor_steps();
        save();
    }
    if (!steps && !wrist && !s_raw) {
        if (s_booted || s_i2c) ESP_LOGI(TAG, "BHI260AP coupé");
        power_off();
        return;
    }
    if (!s_booted) {
        s_booted = boot();
        if (!s_booted) { power_off(); return; }
    }
    if (steps && !s_steps) sensor_zero();      /* total = s_base + (compteur − zéro) */
    bhy2_set_virt_sensor_cfg(BHY2_SENSOR_ID_STC, steps ? 1.0f : 0.0f, 0, &s_dev);
    if (s_gesture_id) bhy2_set_virt_sensor_cfg(s_gesture_id, wrist ? 1.0f : 0.0f, 0, &s_dev);
    s_steps = steps;
    s_wrist = wrist && s_gesture_id;
    s_cache_us = -1;
    s_gesture_seen = false;
    fifo_poll();
    isr_setup(s_wrist);
    if (s_wrist) gpio_intr_enable(BOARD_SENSOR_INT);
    ESP_LOGI(TAG, "pas %s, poignet %s", steps ? "oui" : "non", s_wrist ? "oui" : "non");
}

esp_err_t motion_init(void)
{
    load();
    esp_register_shutdown_handler(shutdown_save);
    motion_apply();
    return (settings_get()->step_count || settings_get()->wrist_wake) && !s_booted ? ESP_FAIL : ESP_OK;
}

bool motion_on_irq(void)
{
    s_gesture_seen = false;
    fifo_poll();                                /* lit les FIFO, libère la ligne */
    if (s_isr) gpio_intr_enable(BOARD_SENSOR_INT);
    return s_wrist && s_gesture_seen;
}

/* ------------------------------------------------------------------ */
/* Confirmation du geste selon la sensibilité. Le geste Bosch seul     */
/* (haute) allume parfois en bougeant le bras ; on vérifie alors que   */
/* l'écran regarde vers le haut (gravité surtout sur Z) et que la      */
/* montre est immobile un instant ; en basse, une montre posée à plat  */
/* est aussi refusée. Échelle : 1024 = 1 g.                             */
/* ------------------------------------------------------------------ */
typedef struct { int samples, gap_ms, zmin, zmax, still; } conf_t;
static const conf_t CONF[2] = {
    { 3, 220, 785, 1000, 150 },   /* basse : < 38° de l'horizontale, pas à plat (> 12°), immobile */
    { 2, 250, 640, 2048, 260 },   /* moyenne : < 50°, presque immobile */
};

static int iabs(int v) { return v < 0 ? -v : v; }

bool motion_wrist_confirm(int sens)
{
    if (sens >= 2 || !s_booted) return true;
    const conf_t *c = &CONF[sens < 0 ? 0 : sens];
    if (!motion_accel_begin()) return true;
    vTaskDelay(pdMS_TO_TICKS(40));
    bool ok = true;
    int16_t px = 0, py = 0, pz = 0;
    for (int i = 0; i < c->samples && ok; i++) {
        if (i) vTaskDelay(pdMS_TO_TICKS(c->gap_ms));
        int16_t x, y, z;
        if (!motion_accel_read(&x, &y, &z)) break;           /* doute : on allume */
        int az = iabs(z);
        ok = az >= c->zmin && az <= c->zmax;
        if (ok && i && (iabs(x - px) > c->still || iabs(y - py) > c->still || iabs(z - pz) > c->still)) ok = false;
        if (!ok) ESP_LOGI(TAG, "geste refusé (x %d y %d z %d, échantillon %d)", x, y, z, i);
        px = x; py = y; pz = z;
    }
    motion_accel_end();
    return ok;
}

void motion_tick(void)
{
    if (!s_booted) return;
    uint32_t today = today_key();
    if (s_steps && today) rollover(today);
    if (s_steps && esp_timer_get_time() - s_save_us >= SAVE_PERIOD_US) {
        if (s_base + sensor_steps() != s_saved) save();
        else s_save_us = esp_timer_get_time();
    }
}

bool motion_ok(void) { return s_booted; }
bool motion_steps_on(void) { return s_steps; }

uint32_t motion_steps_today(void)
{
    int64_t now = esp_timer_get_time();
    if (s_cache_us >= 0 && now - s_cache_us < READ_CACHE_US) return s_cache;
    uint32_t today = today_key();
    if (s_steps && today) rollover(today);
    s_cache = s_base + sensor_steps();
    s_cache_us = now;
    return s_cache;
}

void motion_history(uint32_t h[MOTION_HIST_DAYS])
{
    uint32_t today = today_key();
    if (s_steps && today) rollover(today);
    memcpy(h, s_hist, sizeof s_hist);
}

/* ------------------------------------------------------------------ */
/* Accéléromètre brut (apps) : capteur virtuel « accéléromètre corrigé » */
/* à 100 Hz (±8 g, 4096 = 1 g), rendu à l'échelle 1024 = 1 g.           */
/* ------------------------------------------------------------------ */
bool motion_accel_begin(void)
{
    if (s_raw) return true;
    s_raw = true;
    if (!s_booted) motion_apply();                         /* allume et charge le capteur */
    if (!s_booted) { s_raw = false; return false; }
    s_acc_new = false;
    bhy2_set_virt_sensor_cfg(BHY2_SENSOR_ID_ACC, 100.0f, 0, &s_dev);
    return true;
}

bool motion_accel_read(int16_t *x, int16_t *y, int16_t *z)
{
    if (!s_raw || !s_booted) return false;
    fifo_poll();
    if (!s_acc_new) return false;
    *x = s_acc.x / 4;
    *y = s_acc.y / 4;
    *z = s_acc.z / 4;
    return true;
}

void motion_accel_end(void)
{
    if (!s_raw) return;
    s_raw = false;
    if (s_booted) bhy2_set_virt_sensor_cfg(BHY2_SENSOR_ID_ACC, 0.0f, 0, &s_dev);
    motion_apply();                                         /* retour au réglage */
}
