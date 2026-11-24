/* sdcard.c — voir sdcard.h */
#include "sdcard.h"
#include <string.h>
#include "board.h"
#include "spi_bus.h"
#include "xl9555.h"
#include "axp2101.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "sd";

#define POLL_MS        3000      /* lecture du contact SD_DET (I2C, ~0,1 ms) */
#define SETTLE_MS      600       /* carte qui s'enfonce : contacts qui rebondissent */
#define FLUSH_S        60        /* vidage des tampons sur la carte */
#define INFO_S         600       /* espace libre recalculé */
#define SD_KHZ         20000     /* bus partagé avec la radio (8 MHz) et le NFC */
#define MAX_FLUSHERS   4

enum { REQ_EJECT = 1, REQ_MOUNT = 2, REQ_FLUSH = 4 };

static TaskHandle_t s_task;
static SemaphoreHandle_t s_mx;           /* accès aux fichiers de /sd */
static volatile sd_state_t s_state = SD_ABSENT;
static sdmmc_card_t *s_card;
static volatile uint32_t s_req;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static sd_info_t s_info;
static sd_flush_cb_t s_flush[MAX_FLUSHERS];
static int s_nflush;

static void set_state(sd_state_t st)
{
    portENTER_CRITICAL(&s_mux);
    s_state = st;
    s_info.state = st;
    if (st != SD_MOUNTED) { s_info.total = s_info.free = 0; s_info.name[0] = 0; }
    portEXIT_CRITICAL(&s_mux);
}

static void refresh_info(void)
{
    uint64_t tot = 0, fr = 0;
    if (!sd_lock()) return;
    esp_vfs_fat_info(SD_ROOT, &tot, &fr);
    char nm[8] = "";
    if (s_card) { memcpy(nm, s_card->cid.name, sizeof nm - 1); nm[sizeof nm - 1] = 0; }
    sd_unlock();
    portENTER_CRITICAL(&s_mux);
    s_info.total = tot;
    s_info.free = fr;
    memcpy(s_info.name, nm, sizeof nm);
    portEXIT_CRITICAL(&s_mux);
}

static void run_flushers(void)
{
    for (int i = 0; i < s_nflush; i++) if (s_flush[i]) s_flush[i]();
}

static bool try_mount(void);

/* contact SD_DET : niveau bas quand la carte est en place (vu sur la montre) */
static esp_err_t card_present(bool *present)
{
    bool lvl = true;
    esp_err_t e = xl9555_read(XIO_SD_DET, &lvl);
    *present = !lvl;
    return e;
}

/* Une carte déjà là au démarrage est restée dans l'état où l'ESP l'a laissée
 * (l'AXP2101 garde ses rails à travers un redémarrage) : il lui arrive de ne
 * plus répondre. Quelques essais, espacés, avant de la dire illisible. */
static void do_mount(void)
{
    for (int i = 0; i < 3; i++) {
        if (try_mount()) return;
        bool det = false;
        if (card_present(&det) == ESP_OK && !det) { set_state(SD_ABSENT); return; }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    set_state(SD_FAILED);
}

static bool try_mount(void)
{
    spi_peers_get();                         /* rails SD et NFC, CS hauts */
    if (spi_shared_get() != ESP_OK) {
        spi_peers_put();
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));           /* rail tout juste allumé : mise sous tension de la carte */

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = BOARD_SPI_HOST;
    host.max_freq_khz = SD_KHZ;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = BOARD_SPI_HOST;
    slot.gpio_cs = BOARD_SD_CS;
    esp_vfs_fat_mount_config_t mc = {
        .format_if_mount_failed = false,     /* jamais d'effacement sans le demander */
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    esp_err_t e = esp_vfs_fat_sdspi_mount(SD_ROOT, &host, &slot, &mc, &card);
    if (e == ESP_OK) s_card = card;
    xSemaphoreGive(s_mx);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "carte illisible : %s", esp_err_to_name(e));
        spi_peers_hold_cs(BOARD_SD_CS);      /* le pilote a rendu la broche : CS haut */
        spi_shared_put();
        spi_peers_put();
        return false;
    }
    set_state(SD_MOUNTED);
    refresh_info();
    ESP_LOGI(TAG, "carte « %s » montée : %llu Mo, %llu Mo libres", s_info.name,
             (unsigned long long)(s_info.total >> 20), (unsigned long long)(s_info.free >> 20));
    return true;
}

static void do_unmount(sd_state_t next)
{
    if (s_state == SD_MOUNTED) {
        xSemaphoreTake(s_mx, portMAX_DELAY);
        set_state(next);                     /* plus personne n'entre (sd_lock) */
        esp_vfs_fat_sdcard_unmount(SD_ROOT, s_card);
        s_card = NULL;
        xSemaphoreGive(s_mx);
        spi_peers_hold_cs(BOARD_SD_CS);
        spi_shared_put();
        spi_peers_put();
        if (next == SD_EJECTED) ESP_LOGI(TAG, "carte éjectée");
        else ESP_LOGI(TAG, "carte retirée");
    } else {
        set_state(next);
    }
}

static void sd_task(void *arg)
{
    int64_t next_flush = 0, next_info = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POLL_MS));
        portENTER_CRITICAL(&s_mux);
        uint32_t req = s_req;
        s_req = 0;
        portEXIT_CRITICAL(&s_mux);

        bool det = false;
        if (card_present(&det) != ESP_OK) continue;   /* I2C occupé : on verra au tour suivant */
        int64_t now = esp_timer_get_time();

        if (!det) {                                              /* arrachée (ou jamais là) */
            if (s_state != SD_ABSENT) do_unmount(SD_ABSENT);
            continue;
        }
        if (s_state == SD_ABSENT) {                              /* insertion : on laisse les contacts se poser */
            vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));
            if (card_present(&det) != ESP_OK || !det) continue;
            do_mount();
            next_flush = now + 5 * 1000000LL;                    /* journaux du démarrage, vite */
            next_info = now + INFO_S * 1000000LL;
            continue;
        }
        if ((req & REQ_MOUNT) && (s_state == SD_EJECTED || s_state == SD_FAILED)) {
            do_mount();
            next_flush = now + 5 * 1000000LL;
            next_info = now + INFO_S * 1000000LL;
            continue;
        }
        if (s_state != SD_MOUNTED) continue;
        if ((req & (REQ_EJECT | REQ_FLUSH)) || now >= next_flush) {
            run_flushers();
            next_flush = now + FLUSH_S * 1000000LL;
        }
        if (req & REQ_EJECT) {
            do_unmount(SD_EJECTED);
            continue;
        }
        if (now >= next_info) {
            refresh_info();
            next_info = now + INFO_S * 1000000LL;
        }
    }
}

static void request(uint32_t r)
{
    portENTER_CRITICAL(&s_mux);
    s_req |= r;
    portEXIT_CRITICAL(&s_mux);
    if (s_task) xTaskNotifyGive(s_task);
}

void sd_init(void)
{
    if (s_task) return;
    /* avant la radio : personne n'a encore les rails du bus. Carte remise
     * à zéro par une vraie coupure d'alimentation (voir do_mount) */
    axp_rail(AXP_RAIL_SD, false);
    vTaskDelay(pdMS_TO_TICKS(100));
    s_mx = xSemaphoreCreateMutex();
    /* pile interne : la tâche écrit aussi pendant qu'une écriture en flash
     * (OTA, FAT interne) coupe l'accès à la PSRAM */
    if (!s_mx || xTaskCreatePinnedToCore(sd_task, "sd", 4608, NULL, 2, &s_task, 0) != pdPASS)
        ESP_LOGE(TAG, "tâche sd : mémoire insuffisante");
    else
        xTaskNotifyGive(s_task);                                 /* premier coup d'œil tout de suite */
}

void sd_get_info(sd_info_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_info;
    portEXIT_CRITICAL(&s_mux);
}

bool sd_mounted(void) { return s_state == SD_MOUNTED; }

esp_err_t sd_eject(void)
{
    if (s_state != SD_MOUNTED) return ESP_ERR_INVALID_STATE;
    request(REQ_EJECT);
    return ESP_OK;
}

esp_err_t sd_mount(void)
{
    if (s_state != SD_EJECTED && s_state != SD_FAILED) return ESP_ERR_INVALID_STATE;
    request(REQ_MOUNT);
    return ESP_OK;
}

void sd_flush_soon(void) { request(REQ_FLUSH); }

bool sd_lock(void)
{
    if (!s_mx || s_state != SD_MOUNTED) return false;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (s_state != SD_MOUNTED) { xSemaphoreGive(s_mx); return false; }
    return true;
}

void sd_unlock(void) { xSemaphoreGive(s_mx); }

void sd_add_flusher(sd_flush_cb_t cb)
{
    if (s_nflush < MAX_FLUSHERS) s_flush[s_nflush++] = cb;
}
