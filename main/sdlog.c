/* sdlog.c — voir sdlog.h */
#include "sdlog.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "sdcard.h"
#include "wclock.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define RING     (32 * 1024)
#define LOG_LINE_MAX 200
#define CHUNK    4096

EXT_RAM_BSS_ATTR static char s_ring[RING];
static uint64_t s_wr, s_rd;                 /* positions absolues : écrit, déjà sur la carte */
static portMUX_TYPE s_rmux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_fmt_mx;          /* tampon de mise en forme partagé */
static vprintf_like_t s_orig;

static void ring_put(const char *p, size_t n)
{
    portENTER_CRITICAL(&s_rmux);
    for (size_t k = 0; k < n; ) {
        size_t at = (size_t)(s_wr % RING), run = RING - at;
        if (run > n - k) run = n - k;
        memcpy(&s_ring[at], p + k, run);
        k += run;
        s_wr += run;
    }
    portEXIT_CRITICAL(&s_rmux);
}

static int hook(const char *fmt, va_list ap)
{
    va_list cp;
    va_copy(cp, ap);
    int r = s_orig(fmt, ap);                /* console USB, comme avant */
    if (!xPortInIsrContext() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
        xSemaphoreTake(s_fmt_mx, pdMS_TO_TICKS(5)) == pdTRUE) {
        static char buf[LOG_LINE_MAX];
        static bool line_start = true;
        int k = 0;
        if (line_start) {                   /* heure locale en tête de ligne */
            time_t t = time(NULL) + wclock_tz_offset_min() * 60;
            struct tm tm;
            gmtime_r(&t, &tm);
            k = (int)strftime(buf, sizeof buf, "%H:%M:%S ", &tm);
        }
        int n = vsnprintf(buf + k, sizeof buf - k, fmt, cp);
        if (n > 0) {
            n += k;
            if (n >= (int)sizeof buf) { n = sizeof buf - 1; buf[n - 1] = '\n'; }   /* ligne tronquée */
            line_start = buf[n - 1] == '\n';
            ring_put(buf, (size_t)n);
        }
        xSemaphoreGive(s_fmt_mx);
    }
    va_end(cp);
    return r;
}

/* tâche « sd », carte montée */
static void flush(void)
{
    EXT_RAM_BSS_ATTR static char chunk[CHUNK];
    char path[48];
    time_t now = time(NULL) + wclock_tz_offset_min() * 60;
    struct tm tm;
    gmtime_r(&now, &tm);
    if (tm.tm_year + 1900 < 2024) snprintf(path, sizeof path, SD_ROOT "/logs/sans-date.log");
    else snprintf(path, sizeof path, SD_ROOT "/logs/%04d-%02d-%02d.log", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

    if (!sd_lock()) return;
    mkdir(SD_ROOT "/logs", 0775);
    FILE *f = NULL;
    for (;;) {
        portENTER_CRITICAL(&s_rmux);
        if (s_wr - s_rd > RING) s_rd = s_wr - RING;              /* trop ancien : déjà écrasé */
        size_t n = (size_t)(s_wr - s_rd), at = (size_t)(s_rd % RING);
        if (n > CHUNK) n = CHUNK;
        if (n > RING - at) n = RING - at;
        memcpy(chunk, &s_ring[at], n);
        uint64_t from = s_rd;
        portEXIT_CRITICAL(&s_rmux);
        if (!n) break;
        if (!f && !(f = fopen(path, "a"))) break;
        if (fwrite(chunk, 1, n, f) != n) break;
        portENTER_CRITICAL(&s_rmux);
        if (s_rd == from) s_rd += n;                             /* sinon déjà avancé (écrasement) */
        portEXIT_CRITICAL(&s_rmux);
    }
    if (f) fclose(f);
    sd_unlock();
}

void sdlog_init(void)
{
    if (s_orig) return;
    s_fmt_mx = xSemaphoreCreateMutex();
    if (!s_fmt_mx) return;
    s_orig = esp_log_set_vprintf(hook);
}

void sdlog_attach(void)
{
    if (s_orig) sd_add_flusher(flush);
}
