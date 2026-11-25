/*
 * mc_region.c — liste des régions et région de chaque canal (voir mc_region.h).
 *
 * NVS, espace « mcreg » : « r » = noms séparés par '\n' ; « c » = suites de
 * {canal (6 octets), longueur, nom}. Quelques centaines d'octets en PSRAM.
 */
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "nvs.h"
#include "mbedtls/sha256.h"
#include "mc_region.h"

#define CHAN_MAX 16

typedef struct {
    uint8_t peer[MC_PEER_LEN];
    char    name[MC_REGION_NAME];
} chan_reg_t;

EXT_RAM_BSS_ATTR static char s_reg[MC_REGION_MAX][MC_REGION_NAME];
EXT_RAM_BSS_ATTR static chan_reg_t s_chan[CHAN_MAX];
static int s_nreg, s_nchan;
static SemaphoreHandle_t s_mx;
static StaticSemaphore_t s_mx_buf;
static bool s_loaded;

static void lock(void)
{
    if (!s_mx) s_mx = xSemaphoreCreateMutexStatic(&s_mx_buf);   /* premier appel : tâche app, avant la mesh */
    xSemaphoreTake(s_mx, portMAX_DELAY);
}
static void unlock(void) { xSemaphoreGive(s_mx); }

/* nom nettoyé dans out ; false : refusé */
static bool clean(const char *in, char out[MC_REGION_NAME])
{
    if (!in) return false;
    while (*in == ' ' || *in == '#') in++;
    size_t n = strlen(in);
    while (n && (in[n - 1] == ' ' || in[n - 1] == '\n')) n--;
    if (!n || n >= MC_REGION_NAME || in[0] == '$' || in[0] == '*') return false;
    for (size_t i = 0; i < n; i++) if (in[i] == ',' || in[i] == '\n' || (uint8_t)in[i] < ' ') return false;
    memcpy(out, in, n);
    out[n] = 0;
    return true;
}

static int find_reg(const char *name)
{
    for (int i = 0; i < s_nreg; i++) if (!strcmp(s_reg[i], name)) return i;
    return -1;
}

static int find_chan(const uint8_t peer[MC_PEER_LEN])
{
    for (int i = 0; i < s_nchan; i++) if (!memcmp(s_chan[i].peer, peer, MC_PEER_LEN)) return i;
    return -1;
}

static int cmp_name(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

static void save(void)                  /* sous verrou */
{
    EXT_RAM_BSS_ATTR static char buf[MC_REGION_MAX * MC_REGION_NAME + CHAN_MAX * (MC_PEER_LEN + MC_REGION_NAME + 1)];
    nvs_handle_t h;
    if (nvs_open("mcreg", NVS_READWRITE, &h) != ESP_OK) return;
    size_t k = 0;
    for (int i = 0; i < s_nreg; i++) {
        size_t n = strlen(s_reg[i]);
        memcpy(buf + k, s_reg[i], n);
        k += n;
        buf[k++] = '\n';
    }
    nvs_set_blob(h, "r", buf, k);
    k = 0;
    for (int i = 0; i < s_nchan; i++) {
        size_t n = strlen(s_chan[i].name);
        memcpy(buf + k, s_chan[i].peer, MC_PEER_LEN);
        k += MC_PEER_LEN;
        buf[k++] = (char)n;
        memcpy(buf + k, s_chan[i].name, n);
        k += n;
    }
    nvs_set_blob(h, "c", buf, k);
    nvs_commit(h);
    nvs_close(h);
}

void mc_region_init(void)
{
    lock();
    if (s_loaded) { unlock(); return; }
    s_loaded = true;
    EXT_RAM_BSS_ATTR static char buf[MC_REGION_MAX * MC_REGION_NAME + CHAN_MAX * (MC_PEER_LEN + MC_REGION_NAME + 1) + 1];
    nvs_handle_t h;
    if (nvs_open("mcreg", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof buf - 1;
        if (nvs_get_blob(h, "r", buf, &len) == ESP_OK) {
            buf[len] = 0;
            char *save_p = NULL, nm[MC_REGION_NAME];
            for (char *t = strtok_r(buf, "\n", &save_p); t && s_nreg < MC_REGION_MAX; t = strtok_r(NULL, "\n", &save_p))
                if (clean(t, nm) && find_reg(nm) < 0) memcpy(s_reg[s_nreg++], nm, sizeof nm);
        }
        len = sizeof buf - 1;
        if (nvs_get_blob(h, "c", buf, &len) == ESP_OK) {
            size_t k = 0;
            while (k + MC_PEER_LEN + 1 <= len && s_nchan < CHAN_MAX) {
                uint8_t n = (uint8_t)buf[k + MC_PEER_LEN];
                if (k + MC_PEER_LEN + 1 + n > len || n >= MC_REGION_NAME) break;
                chan_reg_t *c = &s_chan[s_nchan];
                memcpy(c->peer, buf + k, MC_PEER_LEN);
                memcpy(c->name, buf + k + MC_PEER_LEN + 1, n);
                c->name[n] = 0;
                k += MC_PEER_LEN + 1 + n;
                if (find_reg(c->name) >= 0) s_nchan++;      /* région retirée entre-temps : ignorée */
            }
        }
        nvs_close(h);
    }
    unlock();
}

int mc_region_list(char out[][MC_REGION_NAME], int max)
{
    mc_region_init();
    lock();
    int n = s_nreg < max ? s_nreg : max;
    memcpy(out, s_reg, (size_t)n * MC_REGION_NAME);
    unlock();
    return n;
}

bool mc_region_add(const char *name, bool *added)
{
    char nm[MC_REGION_NAME];
    if (added) *added = false;
    if (!clean(name, nm)) return false;
    mc_region_init();
    lock();
    bool ok = true;
    if (find_reg(nm) < 0) {
        if (s_nreg >= MC_REGION_MAX) ok = false;
        else {
            memcpy(s_reg[s_nreg++], nm, sizeof nm);
            qsort(s_reg, (size_t)s_nreg, MC_REGION_NAME, cmp_name);
            save();
            if (added) *added = true;
        }
    }
    unlock();
    return ok;
}

void mc_region_remove(const char *name)
{
    mc_region_init();
    lock();
    int i = find_reg(name);
    if (i >= 0) {
        memmove(s_reg[i], s_reg[i + 1], (size_t)(s_nreg - i - 1) * MC_REGION_NAME);
        s_nreg--;
        for (int c = 0; c < s_nchan;) {
            if (!strcmp(s_chan[c].name, name)) s_chan[c] = s_chan[--s_nchan];
            else c++;
        }
        save();
    }
    unlock();
}

bool mc_region_of(const uint8_t peer[MC_PEER_LEN], char out[MC_REGION_NAME])
{
    mc_region_init();
    lock();
    int i = find_chan(peer);
    if (i >= 0) memcpy(out, s_chan[i].name, MC_REGION_NAME);
    unlock();
    return i >= 0;
}

void mc_region_set(const uint8_t peer[MC_PEER_LEN], const char *name)
{
    mc_region_init();
    lock();
    int i = find_chan(peer);
    if (!name || find_reg(name) < 0) {
        if (i >= 0) { s_chan[i] = s_chan[--s_nchan]; save(); }
    } else {
        if (i < 0 && s_nchan < CHAN_MAX) i = s_nchan++;
        if (i >= 0) {
            memcpy(s_chan[i].peer, peer, MC_PEER_LEN);
            strlcpy(s_chan[i].name, name, MC_REGION_NAME);
            save();
        }
    }
    unlock();
}

bool mc_region_key_of(const uint8_t peer[MC_PEER_LEN], uint8_t key[16])
{
    char nm[MC_REGION_NAME + 1];
    nm[0] = '#';
    if (!mc_region_of(peer, nm + 1)) return false;
    uint8_t h[32];
    if (mbedtls_sha256((const unsigned char *)nm, strlen(nm), h, 0) != 0) return false;
    memcpy(key, h, 16);
    return true;
}
