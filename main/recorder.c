/*
 * recorder.c — voir recorder.h.
 *
 * Le micro PDM ne peut passer que par I2S0 (seul port PDM RX de
 * l'ESP32-S3, avec le filtre PDM→PCM matériel) : le haut-parleur utilise
 * I2S1 (sound.c), si bien que bips et notifications restent possibles
 * pendant un enregistrement.
 *
 * Mémoire : réserve d'échantillons en PSRAM (512 Ko) ; en RAM interne,
 * seulement les tampons DMA I2S (≈ 6 Ko) et un tampon d'écriture de 8 Ko
 * (le pilote SD ne fait du DMA qu'en RAM interne ; depuis la PSRAM il
 * écrirait secteur par secteur), le temps de l'enregistrement.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include "recorder.h"
#include "sdcard.h"
#include "sound.h"
#include "power.h"
#include "board.h"
#include "task_psram.h"
#include "driver/i2s_pdm.h"
#include "driver/gpio.h"
#include "esp_vfs_fat.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"

#define TAG "rec"

#define HDR_BYTES     512                    /* en-tête WAV : données alignées sur un secteur */
#define RING_SAMPLES  (256 * 1024)           /* 512 Ko en PSRAM : 8 s à 32 kHz */
#define RING_MASK     (RING_SAMPLES - 1)
#define IO_SAMPLES    4096                   /* tampon DMA d'écriture/lecture : 8 Ko… */
#define IO_MIN        1024                   /* …ou moins si la RAM interne manque (Wi-Fi en point d'accès) */
#define CHUNK_SAMPLES (32 * 1024)            /* écriture par 64 Ko */
#define HDR_EVERY_US  (30 * 1000000LL)       /* en-tête + fsync toutes les 30 s */
#define RD_SAMPLES    512                    /* bloc lu sur l'I2S */
#define HP_HZ         80.0f                  /* coupe le continu et le grondement */
#define WARMUP_MS     150                    /* démarrage du micro et du filtre PDM */
#define SD_MARGIN     (2u * 1024 * 1024)     /* place laissée libre sur la carte */
#define MAX_BYTES     0xF0000000u            /* sous la limite FAT32 (4 Go) */

/* gain automatique : crêtes ramenées vers -1 dBFS (le limiteur doux
 * arrondit les rares dépassements), montée de 6 dB/s après 0,5 s de calme,
 * descente immédiate ; de 0 à +48 dB (0.2.62 : -6 dBFS et +36 dB au plus,
 * trop faible d'après Seb) */
#define AGC_LIM       29000.0f
#define AGC_LOW       14500.0f
#define AGC_MAX       251.0f
#define AGC_UP_DBS    6.0f
#define AGC_HOLD_US   500000LL

/* ---- état partagé ---- */
static volatile rec_state_t s_state;
static volatile rec_err_t s_err;
static volatile bool s_stop;                 /* demande d'arrêt (UI ou erreur) */
static volatile bool s_cap_done;             /* tâche micro terminée */
static volatile uint32_t s_wr, s_rd;         /* compteurs d'échantillons (réserve) */
static volatile uint32_t s_drops;
static volatile uint8_t s_level, s_gain_db;
static volatile int s_mic_stage;             /* 1 canal, 2 mode PDM, 3 démarrage, 4 lecture */
static volatile esp_err_t s_mic_err;
static uint32_t s_rate;
static int16_t *s_ring;
static uint8_t *s_io;                        /* ≤ 8 Ko DMA, enregistrement OU lecture */
static uint32_t s_io_n;                      /* sa taille, en échantillons */
static int s_fd = -1;                        /* fichier enregistré, utilisé sous sd_lock() */
static volatile uint32_t s_data;             /* octets de données écrits */
static uint32_t s_limit;                     /* octets de données permis */
static char s_name[40];
static char s_path[64];
static bool s_closer_set;

/* lecture */
static volatile bool s_play, s_play_stop, s_play_pause;
static volatile int s_play_seek;             /* saut demandé, en secondes */
static int s_pfd = -1;                       /* fichier lu, sous sd_lock() */
static char s_ppath[64];
static uint32_t s_prate, s_pdata_off, s_pdata_len;
static volatile uint32_t s_poff;             /* octets lus dans les données */

/* ------------------------------------------------------------------ */
static void put_le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put_le16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }

/* RIFF/WAVE : fmt (PCM 16 bits mono), JUNK de bourrage (ignoré par les
 * lecteurs), puis data à l'octet 512 */
static void wav_header(uint8_t *h, uint32_t rate, uint32_t data)
{
    memset(h, 0, HDR_BYTES);
    memcpy(h, "RIFF", 4);
    put_le32(h + 4, HDR_BYTES - 8 + data);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    put_le32(h + 16, 16);
    put_le16(h + 20, 1);                     /* PCM */
    put_le16(h + 22, 1);                     /* mono */
    put_le32(h + 24, rate);
    put_le32(h + 28, rate * 2);
    put_le16(h + 32, 2);
    put_le16(h + 34, 16);
    memcpy(h + 36, "JUNK", 4);
    put_le32(h + 40, HDR_BYTES - 44 - 8);
    memcpy(h + HDR_BYTES - 8, "data", 4);
    put_le32(h + HDR_BYTES - 4, data);
}

/* sous sd_lock() : en-tête à jour et fichier validé sur la carte */
static void header_sync_locked(void)
{
    if (s_fd < 0) return;
    uint8_t *h = s_io;                       /* tampon libre entre deux écritures */
    wav_header(h, s_rate, s_data);
    if (lseek(s_fd, 0, SEEK_SET) == 0) {
        if (write(s_fd, h, HDR_BYTES) != HDR_BYTES) ESP_LOGW(TAG, "en-tête non écrit");
    }
    lseek(s_fd, HDR_BYTES + s_data, SEEK_SET);
    fsync(s_fd);
}

/* appelé par la tâche sd, verrou tenu, avant un démontage (éjection,
 * retrait, partage USB, mise au repos) : on referme proprement */
static void closer(void)
{
    if (s_fd >= 0) {
        header_sync_locked();
        close(s_fd);
        s_fd = -1;
        ESP_LOGW(TAG, "carte démontée pendant l'enregistrement");
    }
    if (s_pfd >= 0) { close(s_pfd); s_pfd = -1; }
}

/* tampon DMA interne : 8 Ko, sinon 4 puis 2 Ko (le Wi-Fi en point d'accès
 * occupe beaucoup de RAM interne) ; un tampon plus petit fait seulement
 * plus d'appels au pilote SD */
static uint8_t *io_alloc(void)
{
    size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    for (uint32_t n = IO_SAMPLES; n >= IO_MIN; n /= 2) {
        if (n > IO_MIN && big < n * 2 + 6144) continue;   /* laisser de quoi ouvrir le micro */
        uint8_t *p = heap_caps_malloc(n * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (p) { s_io_n = n; return p; }
    }
    return NULL;
}

static void fail(rec_err_t e)
{
    if (s_err == REC_OK) s_err = e;
    s_stop = true;
}

/* ------------------------------------------------------------------ */
/* Tâche micro                                                         */
/* ------------------------------------------------------------------ */
static void mic_pins_idle(void)
{
    const gpio_num_t pins[] = { BOARD_MIC_CLK, BOARD_MIC_DAT };
    for (int i = 0; i < 2; i++) {
        gpio_reset_pin(pins[i]);
        gpio_set_pull_mode(pins[i], GPIO_FLOATING);
        gpio_set_direction(pins[i], GPIO_MODE_DISABLE);
    }
    gpio_set_pull_mode(BOARD_MIC_CLK, GPIO_PULLDOWN_ONLY);   /* comme power.c au repos */
}

static inline int16_t soft_clip(float v)
{
    const float knee = 24000.0f, room = 32767.0f - knee;
    float a = fabsf(v);
    if (a > knee) {
        float over = a - knee;
        a = knee + over * room / (over + room);
    }
    return (int16_t)(v < 0 ? -a : a);
}

static void mic_task(void *arg)
{
    i2s_chan_handle_t rx = NULL;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = 4;                      /* 4 Ko de RAM DMA : 64 ms à 32 kHz */
    i2s_pdm_rx_config_t pc = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(s_rate),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = { .clk = BOARD_MIC_CLK, .din = BOARD_MIC_DAT },
    };
    /* horloge PDM ≈ 2 Mbit/s dans les deux cas (plage nominale des micros
     * MEMS) : 32 kHz × 64, ou 16 kHz × 128 */
    pc.clk_cfg.dn_sample_mode = s_rate >= 32000 ? I2S_PDM_DSR_8S : I2S_PDM_DSR_16S;
    /* ouverture : jusqu'à 3 essais (RAM DMA interne momentanément prise
     * par le Wi-Fi ou le Bluetooth…) ; l'étape et le code d'erreur sont
     * gardés pour l'écran et le journal */
    esp_err_t e = ESP_FAIL;
    for (int t = 0; t < 3 && !s_stop; t++) {
        if (t) vTaskDelay(pdMS_TO_TICKS(150));
        s_mic_stage = 1;
        cc.dma_frame_num = RD_SAMPLES >> t;   /* 4, 2 puis 1 Ko de RAM DMA */
        e = i2s_new_channel(&cc, NULL, &rx);
        if (e == ESP_OK) { s_mic_stage = 2; e = i2s_channel_init_pdm_rx_mode(rx, &pc); }
        if (e == ESP_OK) { s_mic_stage = 3; e = i2s_channel_enable(rx); }
        if (e == ESP_OK) break;
        ESP_LOGE(TAG, "micro indisponible (étape %d, essai %d) : %s ; RAM DMA libre %u o, plus grand bloc %u o",
                 s_mic_stage, t + 1, esp_err_to_name(e),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        if (rx) i2s_del_channel(rx);
        rx = NULL;
    }
    if (!rx) {
        s_mic_err = e;
        fail(REC_ERR_MIC);
    }

    int16_t buf[RD_SAMPLES];
    const float a = expf(-2.0f * (float)M_PI * HP_HZ / s_rate);
    const float up = powf(10.0f, AGC_UP_DBS * RD_SAMPLES / s_rate / 20.0f);
    float x1 = 0, y1 = 0, g = 32.0f;          /* +30 dB d'entrée : la parole y est */
    int64_t calm_since = 0, warm_end = esp_timer_get_time() + WARMUP_MS * 1000;
    int64_t log_t = esp_timer_get_time();
    float raw_peak_log = 0;
    int read_fail = 0;

    while (rx && !s_stop) {
        size_t got = 0;
        esp_err_t re = i2s_channel_read(rx, buf, sizeof buf, &got, pdMS_TO_TICKS(500));
        if (re != ESP_OK || got == 0) {
            ESP_LOGW(TAG, "lecture micro : %s", esp_err_to_name(re));
            if (++read_fail < 4) continue;   /* 2 s sans données : abandon */
            s_mic_stage = 4;
            s_mic_err = re != ESP_OK ? re : ESP_ERR_TIMEOUT;
            fail(REC_ERR_MIC);
            break;
        }
        read_fail = 0;
        int n = got / 2;
        /* passe-haut du 1er ordre, crête du bloc */
        float pk = 0;
        for (int i = 0; i < n; i++) {
            float x = buf[i];
            float y = a * (y1 + x - x1);
            x1 = x;
            y1 = y;
            float ay = fabsf(y);
            if (ay > pk) pk = ay;
            buf[i] = (int16_t)(y > 32767.f ? 32767.f : y < -32768.f ? -32768.f : y);
        }
        int64_t now = esp_timer_get_time();
        if (now < warm_end) continue;        /* mise en route : jeté */
        if (pk > raw_peak_log) raw_peak_log = pk;

        /* gain automatique, interpolé sur le bloc (pas de « zip ») */
        float g0 = g;
        if (pk * g > AGC_LIM) {
            g = AGC_LIM / pk;
            calm_since = now;
        } else if (pk * g < AGC_LOW && now - calm_since > AGC_HOLD_US) {
            g *= up;
        }
        if (g > AGC_MAX) g = AGC_MAX;
        if (g < 1.0f) g = 1.0f;
        float step = (g - g0) / n, gi = g0, opk = 0;
        for (int i = 0; i < n; i++) {
            gi += step;
            float v = buf[i] * gi;
            buf[i] = soft_clip(v);
            float av = fabsf(v);
            if (av > opk) opk = av;
        }
        /* niveau affiché sur 60 dB */
        float db = opk > 1 ? 20.0f * log10f(opk / 32768.0f) : -60.0f;
        int lv = (int)((db + 60.0f) * 100.0f / 60.0f);
        s_level = lv < 0 ? 0 : lv > 100 ? 100 : lv;
        s_gain_db = (uint8_t)(20.0f * log10f(g) + 0.5f);

        /* dans la réserve ; pleine (carte trop lente) : bloc perdu */
        uint32_t wr = s_wr;
        if (RING_SAMPLES - (wr - s_rd) < (uint32_t)n) { s_drops += n; continue; }
        uint32_t p = wr & RING_MASK, k = RING_SAMPLES - p;
        if (k > (uint32_t)n) k = n;
        memcpy(s_ring + p, buf, k * 2);
        if ((uint32_t)n > k) memcpy(s_ring, buf + k, (n - k) * 2);
        s_wr = wr + n;

        if (now - log_t > 10000000LL) {
            ESP_LOGI(TAG, "%lu s, crête brute %.0f, gain %u dB, pertes %lu",
                     (unsigned long)(s_wr / s_rate), raw_peak_log, s_gain_db, (unsigned long)s_drops);
            log_t = now;
            raw_peak_log = 0;
        }
    }
    if (rx) {
        i2s_channel_disable(rx);
        i2s_del_channel(rx);
    }
    mic_pins_idle();
    s_level = 0;
    s_cap_done = true;
    task_psram_exit();
}

/* ------------------------------------------------------------------ */
/* Tâche d'écriture (pile interne : accès FAT)                         */
/* ------------------------------------------------------------------ */
/* écrit n échantillons de la réserve (n pair de secteurs sauf le dernier) */
static bool write_samples(uint32_t n)
{
    if (!sd_lock()) { fail(REC_ERR_SD); return false; }
    bool ok = s_fd >= 0;
    while (ok && n) {
        uint32_t k = n < s_io_n ? n : s_io_n;
        uint32_t p = s_rd & RING_MASK, k1 = RING_SAMPLES - p;
        if (k1 > k) k1 = k;
        memcpy(s_io, s_ring + p, k1 * 2);
        if (k > k1) memcpy(s_io + k1 * 2, s_ring, (k - k1) * 2);
        int w = write(s_fd, s_io, k * 2);
        if (w != (int)(k * 2)) {
            ESP_LOGE(TAG, "écriture : %d (errno %d)", w, errno);
            ok = false;
            break;
        }
        s_data += k * 2;
        s_rd += k;
        n -= k;
    }
    sd_unlock();
    if (!ok) fail(REC_ERR_SD);
    return ok;
}

static void writer_task(void *arg)
{
    int64_t last_hdr = esp_timer_get_time();
    for (;;) {
        uint32_t avail = s_wr - s_rd;
        bool done = s_cap_done;
        if (s_err == REC_ERR_SD) {                          /* carte perdue : on jette */
            s_rd = s_wr;
            if (done) break;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        uint32_t room = (s_limit - s_data) / 2;
        if (avail >= CHUNK_SAMPLES || (done && avail)) {
            uint32_t n = avail > CHUNK_SAMPLES ? CHUNK_SAMPLES : avail;
            if (!done) n &= ~255u;                           /* secteurs entiers */
            if (n > room) { n = room; fail(REC_ERR_FULL); }
            if (n) write_samples(n);
            if (s_err == REC_ERR_FULL) s_rd = s_wr;
        } else if (done) {
            break;
        } else {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        if (esp_timer_get_time() - last_hdr > HDR_EVERY_US && s_fd >= 0) {
            if (sd_lock()) { header_sync_locked(); sd_unlock(); }
            last_hdr = esp_timer_get_time();
        }
    }
    if (sd_lock()) {
        if (s_fd >= 0) {
            header_sync_locked();
            close(s_fd);
            s_fd = -1;
        }
        sd_unlock();
    }
    ESP_LOGI(TAG, "fin : %s, %lu octets, pertes %lu, erreur %d", s_name,
             (unsigned long)s_data, (unsigned long)s_drops, s_err);
    heap_caps_free(s_ring);
    heap_caps_free(s_io);
    s_ring = NULL;
    s_io = NULL;
    power_hold_nosleep(false);
    sd_refresh_info();
    s_state = REC_IDLE;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
static void make_name(void)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_year + 1900 >= 2024)
        strftime(s_name, sizeof s_name, "%Y%m%d-%H%M%S.WAV", &tm);
    else                                     /* heure inconnue */
        snprintf(s_name, sizeof s_name, "REC-%08lu.WAV", (unsigned long)(esp_timer_get_time() / 1000000));
    snprintf(s_path, sizeof s_path, REC_DIR "/%s", s_name);
}

uint32_t rec_capacity_s(bool hq)
{
    sd_info_t i;
    sd_get_info(&i);
    if (i.state != SD_MOUNTED || i.free <= SD_MARGIN) return 0;
    uint64_t b = i.free - SD_MARGIN;
    if (b > MAX_BYTES) b = MAX_BYTES;
    return (uint32_t)(b / ((hq ? 32000 : 16000) * 2));
}

bool rec_active(void) { return s_state != REC_IDLE; }

esp_err_t rec_start(bool hq)
{
    if (s_state != REC_IDLE) return ESP_ERR_INVALID_STATE;
    if (s_play) {
        rec_play_stop();
        for (int i = 0; i < 100 && s_play; i++) vTaskDelay(pdMS_TO_TICKS(20));
        if (s_play) { s_err = REC_ERR_BUSY; return ESP_ERR_INVALID_STATE; }
    }
    s_err = REC_OK;
    s_rate = hq ? 32000 : 16000;
    s_ring = heap_caps_malloc(RING_SAMPLES * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_io = io_alloc();
    if (!s_ring || !s_io) {
        heap_caps_free(s_ring);
        heap_caps_free(s_io);
        s_ring = NULL;
        s_io = NULL;
        s_err = REC_ERR_MEM;
        return ESP_ERR_NO_MEM;
    }
    if (!s_closer_set) { sd_add_closer(closer); s_closer_set = true; }

    if (!sd_lock()) {
        heap_caps_free(s_ring);
        heap_caps_free(s_io);
        s_ring = NULL;
        s_io = NULL;
        s_err = REC_ERR_NO_SD;
        return ESP_ERR_NOT_FOUND;
    }
    mkdir(REC_DIR, 0775);
    make_name();
    uint64_t tot = 0, fr = 0;
    esp_vfs_fat_info(SD_ROOT, &tot, &fr);
    s_limit = fr > SD_MARGIN + HDR_BYTES ? (fr - SD_MARGIN - HDR_BYTES > MAX_BYTES ? MAX_BYTES
                                                                      : (uint32_t)(fr - SD_MARGIN - HDR_BYTES))
                                         : 0;
    s_limit &= ~511u;
    s_data = 0;
    s_fd = s_limit ? open(s_path, O_WRONLY | O_CREAT | O_TRUNC, 0664) : -1;
    if (s_fd >= 0) {
        wav_header(s_io, s_rate, 0);
        if (write(s_fd, s_io, HDR_BYTES) != HDR_BYTES) { close(s_fd); unlink(s_path); s_fd = -1; }
    }
    sd_unlock();
    if (s_fd < 0) {
        heap_caps_free(s_ring);
        heap_caps_free(s_io);
        s_ring = NULL;
        s_io = NULL;
        s_err = s_limit ? REC_ERR_SD : REC_ERR_FULL;
        return ESP_FAIL;
    }

    s_wr = s_rd = s_drops = 0;
    s_mic_stage = 0;
    s_mic_err = ESP_OK;
    s_level = 0;
    s_stop = false;
    s_cap_done = false;
    s_state = REC_RECORDING;
    power_hold_nosleep(true);                /* horloge I2S : pas de light sleep */
    if (xTaskCreatePinnedToCore(writer_task, "recw", 4608, NULL, 4, NULL, tskNO_AFFINITY) != pdPASS) {
        s_stop = true;
        s_cap_done = true;
        s_err = REC_ERR_MEM;
        if (sd_lock()) { if (s_fd >= 0) { close(s_fd); s_fd = -1; unlink(s_path); } sd_unlock(); }
        heap_caps_free(s_ring);
        heap_caps_free(s_io);
        s_ring = NULL;
        s_io = NULL;
        power_hold_nosleep(false);
        s_state = REC_IDLE;
        return ESP_ERR_NO_MEM;
    }
    if (task_psram(mic_task, "rec", 4096, NULL, 7, NULL, tskNO_AFFINITY) != pdPASS) {
        s_err = REC_ERR_MEM;
        s_stop = true;
        s_cap_done = true;                   /* la tâche d'écriture referme le fichier */
    }
    ESP_LOGI(TAG, "enregistrement %s, %lu Hz, %lu Mo permis", s_name,
             (unsigned long)s_rate, (unsigned long)(s_limit >> 20));
    return ESP_OK;
}

void rec_stop(void)
{
    if (s_state == REC_RECORDING) {
        s_state = REC_STOPPING;
        s_stop = true;
    }
}

void rec_get(rec_status_t *o)
{
    o->state = s_state;
    o->err = s_err;
    o->rate = s_rate;
    o->seconds = s_rate ? s_wr / s_rate : 0;
    o->level = s_level;
    o->gain_db = s_gain_db;
    uint32_t d = s_data;
    o->left_s = s_rate && s_limit > d ? (s_limit - d) / (2 * s_rate) : 0;
    strlcpy(o->name, s_name, sizeof o->name);
    o->mic_stage = s_mic_stage;
    o->mic_err = s_mic_err;
    if (s_state == REC_RECORDING && s_stop) o->state = REC_STOPPING;
}

/* ------------------------------------------------------------------ */
/* Fichiers                                                             */
/* ------------------------------------------------------------------ */
static bool is_wav(const char *n)
{
    size_t l = strlen(n);
    return l > 4 && !strcasecmp(n + l - 4, ".wav");
}

/* cherche fmt et data dans l'en-tête lu (h, n octets) */
static bool parse_wav(const uint8_t *h, int n, uint32_t *rate, uint32_t *off, uint32_t *len)
{
    if (n < 44 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return false;
    bool fmt_ok = false;
    for (int p = 12; p + 8 <= n;) {
        uint32_t sz = le32(h + p + 4);
        if (!memcmp(h + p, "fmt ", 4) && p + 24 <= n) {
            fmt_ok = le16(h + p + 8) == 1 && le16(h + p + 10) == 1 && le16(h + p + 22) == 16;
            *rate = le32(h + p + 12);
        } else if (!memcmp(h + p, "data", 4)) {
            *off = p + 8;
            *len = sz;
            return fmt_ok && *rate >= 8000 && *rate <= 48000;
        }
        if (sz > (uint32_t)n) return false;
        p += 8 + ((sz + 1) & ~1u);
    }
    return false;
}

static int cmp_desc(const void *a, const void *b)
{
    return strcmp(((const rec_file_t *)b)->name, ((const rec_file_t *)a)->name);
}

int rec_list(rec_file_t *out, int max)
{
    if (!sd_lock()) return -1;
    int n = 0;
    DIR *d = opendir(REC_DIR);
    if (d) {
        struct dirent *e;
        while (n < max && (e = readdir(d))) {
            if (e->d_type == DT_DIR || !is_wav(e->d_name) || strlen(e->d_name) >= sizeof out->name) continue;
            strlcpy(out[n].name, e->d_name, sizeof out[n].name);
            n++;
        }
        closedir(d);
    }
    qsort(out, n, sizeof *out, cmp_desc);
    /* taille et débit : 64 premiers octets de chaque fichier */
    char path[64];
    uint8_t h[64];
    for (int i = 0; i < n; i++) {
        snprintf(path, sizeof path, REC_DIR "/%s", out[i].name);
        out[i].bytes = out[i].seconds = out[i].rate = 0;
        struct stat st;
        if (stat(path, &st) == 0) out[i].bytes = st.st_size;
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        int r = read(fd, h, sizeof h);
        close(fd);
        uint32_t rate = 0, off = 0, len = 0;
        if (r >= 44 && !memcmp(h, "RIFF", 4)) {
            rate = le32(h + 24);
            /* nos fichiers : data à 512 ; la taille du fichier fait foi
             * (en-tête éventuellement en retard après une coupure) */
            if (parse_wav(h, r, &rate, &off, &len) || (rate >= 8000 && rate <= 48000)) {
                if (!off) off = HDR_BYTES;
                out[i].rate = rate;
                out[i].seconds = out[i].bytes > off ? (out[i].bytes - off) / (2 * rate) : 0;
            }
        }
    }
    sd_unlock();
    return n;
}

static bool safe_name(const char *n)
{
    return n && *n && !strchr(n, '/') && !strchr(n, '\\') && strcmp(n, ".") && strcmp(n, "..") && strlen(n) < 40;
}

esp_err_t rec_delete(const char *name)
{
    if (!safe_name(name)) return ESP_ERR_INVALID_ARG;
    char path[64];
    snprintf(path, sizeof path, REC_DIR "/%s", name);
    if (s_state != REC_IDLE && !strcmp(path, s_path)) return ESP_ERR_INVALID_STATE;
    if (s_play && !strcmp(path, s_ppath)) {
        rec_play_stop();
        for (int i = 0; i < 100 && s_play; i++) vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!sd_lock()) return ESP_ERR_NOT_FOUND;
    int r = unlink(path);
    sd_unlock();
    sd_refresh_info();
    return r == 0 ? ESP_OK : ESP_FAIL;
}

/* ------------------------------------------------------------------ */
/* Lecture                                                              */
/* ------------------------------------------------------------------ */
static void play_task(void *arg)
{
    bool open_stream = false;
    while (!s_play_stop) {
        if (s_play_pause) {
            if (open_stream) { sound_stream_close(); open_stream = false; }   /* ampli coupé */
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        int sk = s_play_seek;
        if (sk) {
            s_play_seek = 0;
            int64_t o = (int64_t)s_poff + (int64_t)sk * s_prate * 2;
            if (o < 0) o = 0;
            if (o > s_pdata_len) o = s_pdata_len;
            s_poff = (uint32_t)o & ~1u;
        }
        if (!open_stream) {
            if (sound_stream_open(s_prate) != ESP_OK) break;
            open_stream = true;
        }
        if (s_poff >= s_pdata_len) break;
        uint32_t want = s_pdata_len - s_poff;
        if (want > s_io_n * 2) want = s_io_n * 2;
        int got = -1;
        if (sd_lock()) {
            if (s_pfd < 0) s_pfd = open(s_ppath, O_RDONLY);   /* refermé par la mise au repos */
            if (s_pfd >= 0 && lseek(s_pfd, s_pdata_off + s_poff, SEEK_SET) >= 0)
                got = read(s_pfd, s_io, want);
            sd_unlock();
        }
        if (got <= 0) break;
        got &= ~1;
        s_poff += got;
        sound_stream_write((const int16_t *)s_io, got / 2, 4096);
    }
    if (open_stream) sound_stream_close();
    if (sd_lock()) {
        if (s_pfd >= 0) { close(s_pfd); s_pfd = -1; }
        sd_unlock();
    }
    heap_caps_free(s_io);
    s_io = NULL;
    s_play = false;
    vTaskDelete(NULL);
}

esp_err_t rec_play(const char *name)
{
    if (!safe_name(name) || s_state != REC_IDLE) return ESP_ERR_INVALID_STATE;
    if (s_play) {
        rec_play_stop();
        for (int i = 0; i < 100 && s_play; i++) vTaskDelay(pdMS_TO_TICKS(20));
        if (s_play) return ESP_ERR_INVALID_STATE;
    }
    if (!s_closer_set) { sd_add_closer(closer); s_closer_set = true; }
    snprintf(s_ppath, sizeof s_ppath, REC_DIR "/%s", name);
    s_io = io_alloc();
    if (!s_io) return ESP_ERR_NO_MEM;
    if (!sd_lock()) { heap_caps_free(s_io); s_io = NULL; return ESP_ERR_NOT_FOUND; }
    bool ok = false;
    s_pfd = open(s_ppath, O_RDONLY);
    if (s_pfd >= 0) {
        int r = read(s_pfd, s_io, HDR_BYTES);
        struct stat st;
        uint32_t size = fstat(s_pfd, &st) == 0 ? st.st_size : 0;
        ok = r > 0 && parse_wav(s_io, r, &s_prate, &s_pdata_off, &s_pdata_len);
        if (ok) {
            /* en-tête en retard (coupure) : la taille du fichier fait foi */
            uint32_t real = size > s_pdata_off ? size - s_pdata_off : 0;
            if (s_pdata_len == 0 || s_pdata_len > real) s_pdata_len = real;
            s_pdata_len &= ~1u;
        }
        if (!ok) { close(s_pfd); s_pfd = -1; }
    }
    sd_unlock();
    if (!ok) { heap_caps_free(s_io); s_io = NULL; return ESP_ERR_NOT_SUPPORTED; }
    s_poff = 0;
    s_play_seek = 0;
    s_play_pause = false;
    s_play_stop = false;
    s_play = true;
    if (xTaskCreatePinnedToCore(play_task, "recplay", 4096, NULL, 6, NULL, tskNO_AFFINITY) != pdPASS) {
        if (sd_lock()) { if (s_pfd >= 0) { close(s_pfd); s_pfd = -1; } sd_unlock(); }
        heap_caps_free(s_io);
        s_io = NULL;
        s_play = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void rec_play_stop(void) { if (s_play) s_play_stop = true; }
void rec_play_pause(bool p) { s_play_pause = p; }
bool rec_playing(void) { return s_play; }
bool rec_paused(void) { return s_play && s_play_pause; }
void rec_play_seek(int d) { if (s_play) s_play_seek += d; }

void rec_play_pos(uint32_t *pos, uint32_t *len)
{
    uint32_t r = s_prate ? s_prate * 2 : 1;
    *pos = s_poff / r;
    *len = s_pdata_len / r;
}
