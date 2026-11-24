/* sdcard.c — voir sdcard.h */
#include "sdcard.h"
#include <stdlib.h>
#include <string.h>
#include "board.h"
#include "spi_bus.h"
#include "xl9555.h"
#include "axp2101.h"
#include "power.h"
#include "display.h"
#include "usb_msc.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "sd";

/* Économies : écran éteint, la tâche se réveille 5 fois moins souvent. Les
 * tampons (journal, archive MeshCore) ne sont écrits que toutes les 10 min,
 * plus tôt si le journal se remplit, et avant un redémarrage, une éjection
 * ou un partage : peu d'écritures, la carte reste au repos. L'espace libre
 * n'est recalculé qu'à la demande. */
#define POLL_ON_MS     3000      /* lecture du contact SD_DET (I2C, ~0,1 ms) */
#define POLL_OFF_MS    15000
#define SETTLE_MS      600       /* carte qui s'enfonce : contacts qui rebondissent */
#define FLUSH_S        600       /* vidage des tampons sur la carte */
#define SD_KHZ         20000     /* bus partagé avec la radio (8 MHz) et le NFC */
#define MAX_FLUSHERS   4

enum { REQ_EJECT = 1, REQ_MOUNT = 2, REQ_FLUSH = 4, REQ_INFO = 8, REQ_SHARE = 16, REQ_UNSHARE = 32 };

static TaskHandle_t s_task;
static SemaphoreHandle_t s_mx;           /* accès aux fichiers de /sd (et à la carte brute) */
static volatile sd_state_t s_state = SD_ABSENT;
static sdmmc_card_t *s_card;
static sdspi_dev_handle_t s_raw_dev = -1;   /* carte ouverte sans FAT (partage USB) */
static volatile uint32_t s_req;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static sd_info_t s_info;
static sd_flush_cb_t s_flush[MAX_FLUSHERS];
static int s_nflush;
static volatile uint32_t s_flushes;      /* vidages faits (sd_flush_wait) */

static void set_state(sd_state_t st)
{
    portENTER_CRITICAL(&s_mux);
    s_state = st;
    s_info.state = st;
    if (st != SD_MOUNTED && st != SD_SHARED) { s_info.total = s_info.free = 0; s_info.name[0] = 0; }
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

/* contact SD_DET : niveau bas quand la carte est en place (vu sur la montre) */
static esp_err_t card_present(bool *present)
{
    bool lvl = true;
    esp_err_t e = xl9555_read(XIO_SD_DET, &lvl);
    *present = !lvl;
    return e;
}

static void host_cfg(sdmmc_host_t *host, sdspi_device_config_t *slot)
{
    *host = (sdmmc_host_t)SDSPI_HOST_DEFAULT();
    host->slot = BOARD_SPI_HOST;
    host->max_freq_khz = SD_KHZ;
    *slot = (sdspi_device_config_t)SDSPI_DEVICE_CONFIG_DEFAULT();
    slot->host_id = BOARD_SPI_HOST;
    slot->gpio_cs = BOARD_SD_CS;
}

/* rails SD et NFC, CS hauts, bus SPI : pris tant que la carte est ouverte */
static bool bus_get(void)
{
    spi_peers_get();
    if (spi_shared_get() != ESP_OK) {
        spi_peers_put();
        return false;
    }
    return true;
}

static void bus_put(void)
{
    spi_peers_hold_cs(BOARD_SD_CS);          /* le pilote a rendu la broche : CS haut */
    spi_shared_put();
    spi_peers_put();
}

static bool try_mount(void)
{
    if (!bus_get()) return false;
    vTaskDelay(pdMS_TO_TICKS(20));           /* rail tout juste allumé : mise sous tension de la carte */

    sdmmc_host_t host;
    sdspi_device_config_t slot;
    host_cfg(&host, &slot);
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
        bus_put();
        return false;
    }
    set_state(SD_MOUNTED);
    refresh_info();
    ESP_LOGI(TAG, "carte « %s » montée : %llu Mo, %llu Mo libres", s_info.name,
             (unsigned long long)(s_info.total >> 20), (unsigned long long)(s_info.free >> 20));
    return true;
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

/* démonte la FAT ; le bus et les rails restent pris (bus_put à part) */
static void fat_unmount(sd_state_t next)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    set_state(next);                         /* plus personne n'entre (sd_lock) */
    esp_vfs_fat_sdcard_unmount(SD_ROOT, s_card);
    s_card = NULL;
    xSemaphoreGive(s_mx);
}

static void do_unmount(sd_state_t next)
{
    if (s_state == SD_MOUNTED) {
        fat_unmount(next);
        bus_put();
        if (next == SD_EJECTED) ESP_LOGI(TAG, "carte éjectée");
        else ESP_LOGI(TAG, "carte retirée");
    } else {
        set_state(next);
    }
}

/* ---------------- partage USB : la carte en secteurs bruts ---------------- */

static esp_err_t raw_read(void *dst, uint32_t lba, uint32_t n)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    esp_err_t e = s_card && s_raw_dev >= 0 ? sdmmc_read_sectors(s_card, dst, lba, n) : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_mx);
    return e;
}

static esp_err_t raw_write(const void *src, uint32_t lba, uint32_t n)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    esp_err_t e = s_card && s_raw_dev >= 0 ? sdmmc_write_sectors(s_card, src, lba, n) : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_mx);
    return e;
}

static void request(uint32_t r);
static void host_ejected(void) { request(REQ_UNSHARE); }

static void raw_close(void)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (s_raw_dev >= 0) sdspi_host_remove_device(s_raw_dev);
    s_raw_dev = -1;
    free(s_card);
    s_card = NULL;
    xSemaphoreGive(s_mx);
}

static bool raw_open(void)
{
    sdmmc_host_t host;
    sdspi_device_config_t slot;
    host_cfg(&host, &slot);
    sdmmc_card_t *card = calloc(1, sizeof *card);
    if (!card) return false;
    sdspi_dev_handle_t dev = -1;
    esp_err_t e = sdspi_host_init_device(&slot, &dev);
    if (e == ESP_OK) {
        host.slot = dev;
        e = sdmmc_card_init(&host, card);
    }
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "partage : carte muette (%s)", esp_err_to_name(e));
        if (dev >= 0) sdspi_host_remove_device(dev);
        free(card);
        return false;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    s_card = card;
    s_raw_dev = dev;
    xSemaphoreGive(s_mx);
    return true;
}

static void do_share(void)
{
    if (!power_vbus()) return;
    if (s_state == SD_MOUNTED) {
        run_flushers();                      /* dernières lignes avant de céder la carte */
        fat_unmount(SD_SHARED);
    } else if (!bus_get()) {
        return;
    } else {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    set_state(SD_SHARED);
    bool ok = raw_open();
    if (ok) {
        const usb_msc_media_t m = {
            .sectors = (uint32_t)s_card->csd.capacity,
            .read = raw_read, .write = raw_write, .ejected = host_ejected,
        };
        ok = usb_msc_start(&m) == ESP_OK;
        if (!ok) raw_close();
    }
    if (!ok) {
        bus_put();
        do_mount();                          /* la montre reprend la carte comme avant */
        return;
    }
    char nm[8] = "";
    memcpy(nm, s_card->cid.name, sizeof nm - 1);
    portENTER_CRITICAL(&s_mux);
    s_info.total = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
    s_info.free = 0;
    memcpy(s_info.name, nm, sizeof nm);
    portEXIT_CRITICAL(&s_mux);
}

/* fin du partage ; remount : la carte est toujours là */
static void do_unshare(bool remount)
{
    usb_msc_stop();
    raw_close();
    bus_put();
    ESP_LOGI(TAG, "partage USB terminé");
    if (remount) do_mount();
    else set_state(SD_ABSENT);
}

/* ---------------- tâche ---------------- */

static void sd_task(void *arg)
{
    int64_t next_flush = 0;
    for (;;) {
        bool screen = display_is_on();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(screen || s_state == SD_SHARED ? POLL_ON_MS : POLL_OFF_MS));
        portENTER_CRITICAL(&s_mux);
        uint32_t req = s_req;
        s_req = 0;
        portEXIT_CRITICAL(&s_mux);

        bool det = false;
        if (card_present(&det) != ESP_OK) {                     /* I2C occupé : on verra au tour suivant */
            if (req) {                                           /* demande gardée pour le tour suivant */
                portENTER_CRITICAL(&s_mux);
                s_req |= req;
                portEXIT_CRITICAL(&s_mux);
            }
            continue;
        }
        int64_t now = esp_timer_get_time();

        if (s_state == SD_SHARED) {
            if (!det) do_unshare(false);                         /* arrachée pendant le partage */
            else if ((req & REQ_UNSHARE) || !power_vbus()) do_unshare(true);
            next_flush = now + 5 * 1000000LL;
            continue;
        }
        if (!det) {                                              /* arrachée (ou jamais là) */
            if (s_state != SD_ABSENT) do_unmount(SD_ABSENT);
            continue;
        }
        if (s_state == SD_ABSENT) {                              /* insertion : on laisse les contacts se poser */
            vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));
            if (card_present(&det) != ESP_OK || !det) continue;
            do_mount();
            next_flush = now + 5 * 1000000LL;                    /* journaux du démarrage, vite */
            continue;
        }
        if (req & REQ_SHARE) {
            do_share();
            continue;
        }
        if ((req & REQ_MOUNT) && (s_state == SD_EJECTED || s_state == SD_FAILED)) {
            do_mount();
            next_flush = now + 5 * 1000000LL;
            continue;
        }
        if (s_state != SD_MOUNTED) continue;
        if ((req & (REQ_EJECT | REQ_FLUSH)) || now >= next_flush) {
            run_flushers();
            s_flushes++;
            next_flush = now + FLUSH_S * 1000000LL;
        }
        if (req & REQ_EJECT) {
            do_unmount(SD_EJECTED);
            continue;
        }
        if (req & REQ_INFO) refresh_info();
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

void sd_refresh_info(void) { if (s_state == SD_MOUNTED) request(REQ_INFO); }

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

bool sd_can_share(void)
{
    sd_state_t st = s_state;
    return power_vbus() && (st == SD_MOUNTED || st == SD_EJECTED || st == SD_FAILED);
}

esp_err_t sd_share(bool on)
{
    if (on) {
        if (!sd_can_share()) return ESP_ERR_INVALID_STATE;
        request(REQ_SHARE);
    } else {
        if (s_state != SD_SHARED) return ESP_ERR_INVALID_STATE;
        request(REQ_UNSHARE);
    }
    return ESP_OK;
}

bool sd_share_host_seen(void) { return s_state == SD_SHARED && usb_msc_host_seen(); }

void sd_flush_soon(void) { request(REQ_FLUSH); }

void sd_flush_wait(int ms)
{
    if (s_state != SD_MOUNTED || xTaskGetCurrentTaskHandle() == s_task) return;
    uint32_t n = s_flushes;
    request(REQ_FLUSH);
    for (int t = 0; t < ms && s_flushes == n && s_state == SD_MOUNTED; t += 20) vTaskDelay(pdMS_TO_TICKS(20));
}

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
