/* maptiles.c — voir maptiles.h */
#include "maptiles.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "png_tile.h"
#include "sdcard.h"
#include "task_psram.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include "miniz.h"

static const char *TAG = "tiles";

#define DIR_TILES   SD_ROOT "/tiles"
#define NSLOT_MAX   20                 /* 2,5 Mo de PSRAM */
#define NSLOT_MIN   12                 /* 3 × 3 tuiles visibles et un peu d'avance */
#define FILE_MAX    (1024 * 1024)      /* tuile plus grosse : refusée */
#define PX_BYTES    (MT_TILE * MT_TILE * 2)
#define MT_PS          (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define PS_RESERVE  (1536 * 1024)      /* PSRAM laissée au reste de la montre */

enum { S_FREE, S_QUEUED, S_LOADING, S_READY, S_MISSING };

typedef struct {
    int8_t   z;
    uint8_t  st;
    bool     approx;
    uint8_t  pin;                      /* lue par la tâche (agrandissement) : pas évincée */
    int32_t  x, y;
    uint16_t prio;                     /* ordre de mt_want() dans la vue */
    uint32_t used;                     /* dernière vue qui l'a voulue */
    uint16_t *px;
    lv_image_dsc_t dsc;
} slot_t;

static slot_t *s_slot;
static int s_nslot;
static SemaphoreHandle_t s_mx, s_done;
static TaskHandle_t s_task;
static volatile bool s_quit, s_scan_req, s_dirty;
static volatile mt_state_t s_state;
static volatile uint32_t s_mask;
static double s_cmx, s_cmy;
static bool s_has_center;
static uint32_t s_frame;
static uint16_t s_prio;

/* tâche « tiles » seule */
static uint8_t *s_file;
static size_t s_file_cap;
static uint16_t *s_tmp;                /* tuile d'un niveau plus large, avant agrandissement */
static tinfl_decompressor *s_inf;
static png_tile_ctx_t s_png;

static void lock(void) { xSemaphoreTake(s_mx, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mx); }

/* ------------------------------------------------------------------ décodage */
static long inflate_rom(const uint8_t *in, size_t n, uint8_t *out, size_t out_len, void *ctx)
{
    tinfl_init(s_inf);
    size_t in_n = n, out_n = out_len;
    tinfl_status st = tinfl_decompress(s_inf, in, &in_n, out, out, &out_n,
                                       TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    /* DONE, ou sortie pleine (adler32 et fin du flux ignorés) */
    if (st < 0 && out_n != out_len) return -1;
    return (long)out_n;
}

static void *ps_realloc(void *p, size_t n) { return heap_caps_realloc(p, n, MT_PS); }

/* lit et décode z/x/y dans out ; false : absente ou illisible */
static bool read_tile(int z, int32_t x, int32_t y, uint16_t *out)
{
    char path[48];
    snprintf(path, sizeof path, DIR_TILES "/%d/%ld/%ld.png", z, (long)x, (long)y);
    if (!sd_lock()) {
        s_state = MT_NO_SD;
        return false;
    }
    size_t n = 0;
    bool ok = false;
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size <= FILE_MAX) {
            n = (size_t)st.st_size;
            if (s_file_cap < n) {
                uint8_t *p = heap_caps_realloc(s_file, n, MT_PS);
                if (p) { s_file = p; s_file_cap = n; }
            }
            ok = s_file_cap >= n && read(fd, s_file, n) == (ssize_t)n;
        }
        close(fd);
    }
    sd_unlock();
    if (!ok) return false;
    int r = png_tile_decode(&s_png, s_file, n, out);
    if (r != PNG_OK) ESP_LOGW(TAG, "%s : PNG illisible (%d)", path, r);
    return r == PNG_OK;
}

/* agrandit le carré de la tuile src (niveau z − k) qui couvre (x, y) */
static void upscale(const uint16_t *src, uint16_t *dst, int k, int32_t x, int32_t y)
{
    int n = MT_TILE >> k, m = (1 << k) - 1;
    int ox = (int)(x & m) * n, oy = (int)(y & m) * n;
    for (int dy = 0; dy < MT_TILE; dy++) {
        const uint16_t *row = src + (oy + (dy >> k)) * MT_TILE + ox;
        uint16_t *o = dst + dy * MT_TILE;
        for (int dx = 0; dx < MT_TILE; dx++) o[dx] = row[dx >> k];
    }
}

static slot_t *find(int z, int32_t x, int32_t y)      /* sous le verrou */
{
    for (int i = 0; i < s_nslot; i++) {
        slot_t *s = &s_slot[i];
        if (s->st != S_FREE && s->z == z && s->x == x && s->y == y) return s;
    }
    return NULL;
}

/* 0 : absente, 1 : exacte, 2 : agrandie */
static int load(int z, int32_t x, int32_t y, uint16_t *out)
{
    uint32_t mask = s_mask;
    if ((mask >> z) & 1 && read_tile(z, x, y, out)) return 1;
    for (int k = 1; k <= MT_UPSCALE && z - k >= 0; k++) {
        int zz = z - k;
        if (!((mask >> zz) & 1)) continue;
        int32_t px = x >> k, py = y >> k;
        lock();
        slot_t *s = find(zz, px, py);
        if (s && (s->st != S_READY || s->approx)) s = NULL;
        if (s) s->pin++;
        unlock();
        if (s) {                                   /* déjà en cache : immédiat */
            upscale(s->px, out, k, x, y);
            lock();
            s->pin--;
            unlock();
            return 2;
        }
        if (read_tile(zz, px, py, s_tmp)) {
            upscale(s_tmp, out, k, x, y);
            return 2;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ niveaux */
static bool num_name(const char *s, long *v)
{
    if (!*s) return false;
    long n = 0;
    for (; *s; s++) {
        if (*s == '.') break;                      /* « 12.png » */
        if (*s < '0' || *s > '9' || n > 100000000) return false;
        n = n * 10 + (*s - '0');
    }
    *v = n;
    return true;
}

/* plus petit et plus grand nom numérique d'un dossier ; false : aucun */
static bool dir_range(const char *path, long *lo, long *hi)
{
    DIR *d = opendir(path);
    if (!d) return false;
    bool any = false;
    struct dirent *e;
    while ((e = readdir(d))) {
        long v;
        if (!num_name(e->d_name, &v)) continue;
        if (!any || v < *lo) *lo = v;
        if (!any || v > *hi) *hi = v;
        any = true;
    }
    closedir(d);
    return any;
}

static void scan(void)
{
    if (!sd_lock()) {
        s_state = MT_NO_SD;
        return;
    }
    uint32_t mask = 0;
    DIR *d = opendir(DIR_TILES);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            long v;
            if (e->d_type == DT_DIR && num_name(e->d_name, &v) && v <= MT_ZMAX) mask |= 1u << v;
        }
        closedir(d);
    }
    /* centre de la zone couverte au niveau le plus large */
    bool has = false;
    double cx = 0, cy = 0;
    if (mask) {
        int z = __builtin_ctz(mask);
        char p[64];
        long x0, x1, y0, y1;
        snprintf(p, sizeof p, DIR_TILES "/%d", z);
        if (dir_range(p, &x0, &x1)) {
            long xm = (x0 + x1) / 2;
            /* dossier présent le plus proche du milieu */
            for (long dx = 0; dx <= (x1 - x0) / 2 + 1 && !has; dx++) {
                for (int sgn = -1; sgn <= 1 && !has; sgn += 2) {
                    snprintf(p, sizeof p, DIR_TILES "/%d/%ld", z, xm + sgn * dx);
                    if (dir_range(p, &y0, &y1)) {
                        double n = (double)(1u << z);
                        cx = ((double)(xm + sgn * dx) + 0.5) / n;
                        cy = ((double)(y0 + y1) / 2 + 0.5) / n;
                        has = true;
                    }
                }
            }
        }
    }
    sd_unlock();
    lock();
    s_mask = mask;
    s_cmx = cx;
    s_cmy = cy;
    s_has_center = has;
    /* tuiles marquées absentes : peut-être présentes sur cette carte */
    for (int i = 0; i < s_nslot; i++)
        if (s_slot[i].st == S_MISSING) s_slot[i].st = S_FREE;
    unlock();
    s_state = mask ? MT_READY : MT_NO_TILES;
    ESP_LOGI(TAG, "niveaux présents : 0x%06lx", (unsigned long)mask);
}

/* ------------------------------------------------------------------ tâche */
static void tiles_task(void *arg)
{
    while (!s_quit) {
        if (s_scan_req) {
            s_scan_req = false;
            scan();
            s_dirty = true;
        }
        lock();
        slot_t *best = NULL;
        for (int i = 0; i < s_nslot; i++) {
            slot_t *s = &s_slot[i];
            if (s->st == S_QUEUED && (!best || s->prio < best->prio)) best = s;
        }
        int z = 0;
        int32_t x = 0, y = 0;
        if (best) {
            best->st = S_LOADING;
            z = best->z; x = best->x; y = best->y;
        }
        unlock();
        if (!best) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }
        int r = s_state == MT_READY ? load(z, x, y, best->px) : 0;
        lock();
        best->st = r ? S_READY : S_MISSING;
        best->approx = r == 2;
        unlock();
        s_dirty = true;
    }
    xSemaphoreGive(s_done);
    task_psram_exit();
}

/* ------------------------------------------------------------------ API */
static void free_all(void)
{
    for (int i = 0; i < s_nslot; i++) heap_caps_free(s_slot[i].px);
    heap_caps_free(s_slot);
    heap_caps_free(s_tmp);
    heap_caps_free(s_inf);
    heap_caps_free(s_file);
    heap_caps_free(s_png.raw);
    s_slot = NULL;
    s_nslot = 0;
    s_tmp = NULL;
    s_inf = NULL;
    s_file = NULL;
    s_file_cap = 0;
    memset(&s_png, 0, sizeof s_png);
}

bool mt_open(void)
{
    if (s_task) return true;
    if (!s_mx) s_mx = xSemaphoreCreateMutex();
    if (!s_done) s_done = xSemaphoreCreateBinary();
    s_slot = heap_caps_calloc(NSLOT_MAX, sizeof *s_slot, MT_PS);
    s_tmp = heap_caps_malloc(PX_BYTES, MT_PS);
    s_inf = heap_caps_malloc(sizeof *s_inf, MT_PS);
    if (!s_mx || !s_done || !s_slot || !s_tmp || !s_inf) goto oom;
    for (s_nslot = 0; s_nslot < NSLOT_MAX; s_nslot++) {
        slot_t *s = &s_slot[s_nslot];
        if (heap_caps_get_free_size(MT_PS) < PS_RESERVE + PX_BYTES) break;
        s->px = heap_caps_malloc(PX_BYTES, MT_PS);
        if (!s->px) break;
        s->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        s->dsc.header.w = MT_TILE;
        s->dsc.header.h = MT_TILE;
        s->dsc.header.stride = MT_TILE * 2;
        s->dsc.data_size = PX_BYTES;
        s->dsc.data = (const uint8_t *)s->px;
    }
    if (s_nslot < NSLOT_MIN) goto oom;
    s_png.inflate = inflate_rom;
    s_png.realloc_fn = ps_realloc;
    s_quit = false;
    s_scan_req = true;
    s_dirty = false;
    s_state = MT_SCANNING;
    s_mask = 0;
    s_has_center = false;
    xSemaphoreTake(s_done, 0);
    if (task_psram(tiles_task, "tiles", 4096, NULL, 3, &s_task, tskNO_AFFINITY) != pdPASS) {
        s_task = NULL;
        goto oom;
    }
    ESP_LOGI(TAG, "cache de %d tuiles (%d Ko de PSRAM)", s_nslot, s_nslot * PX_BYTES / 1024);
    return true;
oom:
    ESP_LOGE(TAG, "PSRAM insuffisante pour les tuiles");
    free_all();
    s_state = MT_NO_MEM;
    return false;
}

void mt_close(void)
{
    if (s_task) {
        s_quit = true;
        xTaskNotifyGive(s_task);
        /* une tuile en cours : quelques centaines de ms au plus */
        if (xSemaphoreTake(s_done, pdMS_TO_TICKS(5000)) != pdTRUE) {
            ESP_LOGE(TAG, "tâche bloquée : cache gardé");
            return;                                /* fuite plutôt qu'écriture en mémoire libérée */
        }
        s_task = NULL;
    }
    for (int i = 0; i < s_nslot; i++) lv_image_cache_drop(&s_slot[i].dsc);
    free_all();
}

mt_state_t mt_state(void) { return s_state; }

void mt_rescan(void)
{
    if (!s_task) return;
    s_state = MT_SCANNING;
    s_scan_req = true;
    xTaskNotifyGive(s_task);
}

uint32_t mt_zoom_mask(void) { return s_mask; }

bool mt_center(double *mx, double *my)
{
    if (!s_mx) return false;
    lock();
    bool h = s_has_center;
    *mx = s_cmx;
    *my = s_cmy;
    unlock();
    return h;
}

void mt_begin(void)
{
    if (!s_nslot) return;
    lock();
    s_frame++;
    s_prio = 0;
    for (int i = 0; i < s_nslot; i++)
        if (s_slot[i].st == S_QUEUED) s_slot[i].st = S_FREE;    /* hors de la nouvelle vue */
    unlock();
}

void mt_want(int z, int x, int y)
{
    if (!s_nslot || s_state != MT_READY) return;
    lock();
    slot_t *s = find(z, x, y);
    if (s) {
        s->used = s_frame;
        unlock();
        return;
    }
    /* sinon une place : libre d'abord, puis la moins récemment vue */
    for (int i = 0; i < s_nslot; i++) {
        slot_t *c = &s_slot[i];
        if (c->st == S_LOADING || c->pin || (c->st != S_FREE && c->used == s_frame)) continue;
        if (!s || (c->st == S_FREE && s->st != S_FREE) ||
            ((c->st == S_FREE) == (s->st == S_FREE) && c->used < s->used)) s = c;
    }
    if (!s) { unlock(); return; }
    bool was_ready = s->st == S_READY;
    s->z = (int8_t)z;
    s->x = x;
    s->y = y;
    s->st = S_QUEUED;
    s->approx = false;
    s->prio = s_prio++;
    s->used = s_frame;
    unlock();
    if (was_ready) lv_image_cache_drop(&s->dsc);
    xTaskNotifyGive(s_task);
}

const lv_image_dsc_t *mt_peek(int z, int x, int y, bool *approx)
{
    if (!s_nslot) return NULL;
    lock();
    slot_t *s = find(z, x, y);
    const lv_image_dsc_t *d = s && s->st == S_READY ? &s->dsc : NULL;
    if (d && approx) *approx = s->approx;
    unlock();
    return d;
}

bool mt_missing(int z, int x, int y)
{
    if (!s_nslot) return false;
    lock();
    slot_t *s = find(z, x, y);
    bool m = s && s->st == S_MISSING;
    unlock();
    return m;
}

bool mt_take_dirty(void)
{
    bool d = s_dirty;
    s_dirty = false;
    return d;
}
