/* mc_hist.c — voir mc_hist.h */
#include "mc_hist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "mc_text.h"
#include "sdcard.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "mchist";

#define HIST_PATH     "/mc/hist"
#define HIST_TMP      "/mc/hist.tmp"
#define HIST_MAGIC    0x3248434Du          /* « MCH2 » */
#define HIST_MAGIC_V1 0x48430001u          /* 0.2.0 à 0.2.13 : sans conversation ni état lu */
#define HIST_SAVE_US  (30 * 1000000LL)     /* sauvegarde après 30 s de calme */
#define ARCH_DIR      SD_ROOT "/meshcore"
#define ARCH_MAGIC    0x3141434Du          /* « MCA1 » */
#define ARCH_BLK      8                    /* enregistrements lus d'un coup */
#define QFRAME_MAX    200

typedef struct {
    uint32_t magic;
    mc_msg_t m;
    uint8_t  pad[256 - 4 - sizeof(mc_msg_t)];
} arch_rec_t;
_Static_assert(sizeof(arch_rec_t) == 256, "enregistrement d'archive : 256 octets");

typedef struct { uint32_t magic, n, seq, arch_seq; } hist_hdr_t;

EXT_RAM_BSS_ATTR static mc_msg_t s_ring[MC_HIST_N];
static int s_n, s_head;                    /* head : prochain emplacement */
static uint32_t s_seq;                     /* dernier numéro donné */
static uint32_t s_ver;                     /* change à chaque modification visible */
static uint32_t s_arch_seq;                /* dernier message copié sur la carte */
static SemaphoreHandle_t s_mx;
static int64_t s_dirty_at;                 /* 0 : rien à sauver */
static mc_msg_cb_t s_cb;
static uint8_t s_qframe[QFRAME_MAX];       /* dernière trame mise en file (tâche mesh) */
static int s_qlen;

EXT_RAM_BSS_ATTR static mch_peer_t s_peers[MCH_PEERS_MAX];
static int s_npeers;
static volatile bool s_peers_req = true;

#define AT(i) s_ring[(s_head - 1 - (i) + 2 * MC_HIST_N) % MC_HIST_N]   /* 0 : le plus récent */

static void lock(void) { xSemaphoreTake(s_mx, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mx); }

static void touch(void)                    /* sous le verrou */
{
    s_ver++;
    if (!s_dirty_at) s_dirty_at = esp_timer_get_time();
}

static bool same(const mc_msg_t *e, uint8_t kind, const uint8_t *peer)
{
    return e->kind == kind && memcmp(e->peer, peer, MC_PEER_LEN) == 0;
}

static void notify(const mc_msg_t *m)
{
    mc_msg_cb_t cb = s_cb;
    if (cb) cb(m);
}

/* ------------------------------------------------------------------ anneau */
void mch_frame_queued(const uint8_t *frame, int len)
{
    if (len <= 0 || len > QFRAME_MAX) { s_qlen = 0; return; }
    memcpy(s_qframe, frame, len);
    s_qlen = len;
}

void mch_add(mc_msg_t *m, bool recv)
{
    if (!s_mx) return;
    if (recv && s_qlen > 0) {
        uint8_t kind;
        uint32_t ts;
        m->qkey = mc_fnv32(s_qframe, (size_t)s_qlen);
        if (!m->ts && mc_frame_info(s_qframe, s_qlen, &kind, &ts) && kind == m->kind) m->ts = ts;
    }
    s_qlen = 0;
    if (!m->ts) m->ts = (uint32_t)time(NULL);
    lock();
    m->seq = ++s_seq;
    if (!recv) {                            /* l'app vient d'écrire ici : conversation lue */
        for (int i = 0; i < s_n; i++)
            if (same(&AT(i), m->kind, m->peer)) AT(i).flags |= MC_F_READ;
    }
    s_ring[s_head] = *m;
    s_head = (s_head + 1) % MC_HIST_N;
    if (s_n < MC_HIST_N) s_n++;
    touch();
    unlock();
    notify(m);
}

void mch_frame_synced(const uint8_t *frame, int len)
{
    if (!s_mx || len <= 0) return;
    uint32_t k = mc_fnv32(frame, (size_t)len);
    bool hit = false;
    lock();
    for (int i = s_n - 1; i >= 0; i--) {    /* la file se vide du plus ancien au plus récent */
        mc_msg_t *e = &AT(i);
        if (e->qkey == k && !(e->flags & MC_F_APP)) {
            e->flags |= MC_F_APP | MC_F_READ;
            touch();
            hit = true;
            break;
        }
    }
    unlock();
    if (hit) notify(NULL);
}

bool mch_has_qkey(uint32_t qkey)
{
    bool hit = false;
    lock();
    for (int i = 0; i < s_n && !hit; i++) hit = AT(i).qkey == qkey;
    unlock();
    return hit;
}

bool mch_import(const mc_msg_t *m)
{
    lock();
    /* déjà repris de l'ancien historique (même texte, même conversation) */
    for (int i = 0; i < s_n; i++) {
        const mc_msg_t *e = &AT(i);
        if ((e->flags & MC_F_LEGACY) && e->kind == m->kind && !strcmp(e->from, m->from) &&
            !strcmp(e->text, m->text)) {
            unlock();
            return false;
        }
    }
    s_ring[s_head] = *m;
    s_ring[s_head].seq = ++s_seq;
    s_head = (s_head + 1) % MC_HIST_N;
    if (s_n < MC_HIST_N) s_n++;
    touch();
    unlock();
    return true;
}

int mc_history_count(void) { return s_n; }
uint32_t mc_history_last_seq(void) { return s_ver; }
void mc_set_msg_cb(mc_msg_cb_t cb) { s_cb = cb; }

bool mc_history_get(int i, mc_msg_t *out)
{
    if (!s_mx) return false;
    lock();
    bool ok = i >= 0 && i < s_n;
    if (ok) *out = AT(i);
    unlock();
    return ok;
}

/* ------------------------------------------------------------------ FAT interne */
static void sanitize(mc_msg_t *m)
{
    m->from[MC_NAME_MAX - 1] = 0;           /* fichier abîmé : chaînes bornées */
    m->text[MC_TEXT_MAX] = 0;
    if (m->kind > MC_CONV_CHANNEL) m->kind = MC_CONV_CONTACT;
}

void mch_save_now(void)
{
    EXT_RAM_BSS_ATTR static mc_msg_t snap[MC_HIST_N];   /* tâche mesh seule */
    if (!s_mx) return;
    hist_hdr_t h = { HIST_MAGIC, 0, 0, 0 };
    lock();
    h.n = (uint32_t)s_n;
    h.seq = s_seq;
    h.arch_seq = s_arch_seq;
    for (int i = 0; i < s_n; i++) snap[i] = AT(s_n - 1 - i);     /* du plus ancien au plus récent */
    s_dirty_at = 0;
    unlock();
    FILE *f = fopen(HIST_TMP, "wb");
    bool ok = f && fwrite(&h, sizeof h, 1, f) == 1 && fwrite(snap, sizeof(mc_msg_t), h.n, f) == h.n;
    if (f && fclose(f) != 0) ok = false;
    if (ok) {
        remove(HIST_PATH);                  /* FAT : rename ne remplace pas */
        ok = rename(HIST_TMP, HIST_PATH) == 0;
    }
    if (!ok) ESP_LOGW(TAG, "historique non sauvegardé");
}

void mch_save_if_due(int64_t now)
{
    if (s_dirty_at && now - s_dirty_at >= HIST_SAVE_US) mch_save_now();
}

/* message de l'historique 0.2.0 à 0.2.13 */
typedef struct {
    uint32_t seq, ts;
    bool     channel;
    int8_t   snr_q4;
    uint8_t  hops;
    char     from[MC_NAME_MAX];
    char     text[MC_TEXT_MAX + 1];
} msg_v1_t;

/* identifiant provisoire tiré du nom, remplacé par le vrai dès que la liste
 * des contacts et canaux est connue (mch_peers_set) */
static void legacy_peer(uint8_t kind, const char *name, uint8_t out[MC_PEER_LEN])
{
    uint32_t a = mc_fnv32(name, strlen(name)), b = a * 2654435761u + kind;
    memcpy(out, &a, 4);
    memcpy(out + 4, &b, 2);
}

/* reprise de l'ancien historique : messages tenus pour lus (ils avaient été
 * vus dans l'ancienne app) */
static bool load_v1(FILE *f, uint32_t n)
{
    static msg_v1_t o;
    int k = 0;
    while (k < (int)n && k < MC_HIST_N && fread(&o, sizeof o, 1, f) == 1) {
        mc_msg_t *m = &s_ring[k];
        memset(m, 0, sizeof *m);
        m->seq = (uint32_t)k + 1;
        m->ts = o.ts;
        m->kind = o.channel ? MC_CONV_CHANNEL : MC_CONV_CONTACT;
        m->flags = MC_F_READ | MC_F_LEGACY;
        m->snr_q4 = o.snr_q4;
        m->hops = o.hops;
        o.from[MC_NAME_MAX - 1] = 0;
        o.text[MC_TEXT_MAX] = 0;
        memcpy(m->from, o.from, sizeof m->from);
        memcpy(m->text, o.text, sizeof m->text);
        legacy_peer(m->kind, m->from, m->peer);
        k++;
    }
    s_n = k;
    s_head = k % MC_HIST_N;
    s_seq = (uint32_t)k;
    s_arch_seq = 0;
    if (k) s_dirty_at = esp_timer_get_time();        /* réécrit au nouveau format */
    ESP_LOGI(TAG, "%d message(s) repris de l'ancien historique", k);
    return true;
}

static bool load_from(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    hist_hdr_t h;
    if (fread(&h, 8, 1, f) == 1 && h.magic == HIST_MAGIC_V1 && h.n <= 4096) {
        fseek(f, 8, SEEK_SET);
        bool ok = load_v1(f, h.n);
        fclose(f);
        return ok;
    }
    fseek(f, 0, SEEK_SET);
    bool ok = fread(&h, sizeof h, 1, f) == 1 && h.magic == HIST_MAGIC && h.n <= MC_HIST_N;
    if (ok) {
        int n = 0;
        uint32_t top = h.seq;
        while (n < (int)h.n && fread(&s_ring[n], sizeof(mc_msg_t), 1, f) == 1) {
            sanitize(&s_ring[n]);
            if (s_ring[n].seq > top) top = s_ring[n].seq;
            n++;
        }
        s_n = n;
        s_head = n % MC_HIST_N;
        s_seq = top;
        s_arch_seq = h.arch_seq <= top ? h.arch_seq : top;
        ESP_LOGI(TAG, "%d message(s) dans l'historique", n);
    }
    fclose(f);
    return ok;
}

void mch_load(void)
{
    lock();
    if (!load_from(HIST_PATH) && load_from(HIST_TMP)) rename(HIST_TMP, HIST_PATH);  /* coupure pendant la sauvegarde */
    unlock();
}

/* ------------------------------------------------------------------ carte SD */
static void arch_path(char *b, size_t n, uint8_t kind, const uint8_t *p)
{
    snprintf(b, n, ARCH_DIR "/%c-%02x%02x%02x%02x%02x%02x.bin", kind == MC_CONV_CHANNEL ? 'g' : 'c',
             p[0], p[1], p[2], p[3], p[4], p[5]);
}

/* tâche « sd » : copie sur la carte les messages pas encore archivés */
static void arch_flush(void)
{
    EXT_RAM_BSS_ATTR static arch_rec_t rec[MC_HIST_N];
    int n = 0;
    lock();
    for (int i = s_n - 1; i >= 0; i--) {
        if (AT(i).seq <= s_arch_seq) continue;
        memset(&rec[n], 0, sizeof rec[n]);
        rec[n].magic = ARCH_MAGIC;
        rec[n].m = AT(i);
        n++;
    }
    unlock();
    if (!n || !sd_lock()) return;
    mkdir(ARCH_DIR, 0775);
    uint32_t done = 0;
    char path[64];
    for (int i = 0; i < n; i++) {
        arch_path(path, sizeof path, rec[i].m.kind, rec[i].m.peer);
        FILE *f = fopen(path, "ab");
        bool ok = f && fwrite(&rec[i], sizeof rec[i], 1, f) == 1;
        if (f && fclose(f) != 0) ok = false;
        if (!ok) { ESP_LOGW(TAG, "archive SD : écriture ratée"); break; }
        done = rec[i].m.seq;
    }
    sd_unlock();
    /* pas de sauvegarde exprès pour ce numéro : il part avec la prochaine. Un
     * redémarrage entre-temps recopie quelques messages, ignorés à la lecture */
    lock();
    if (done > s_arch_seq) s_arch_seq = done;
    unlock();
}

/* tâche app : messages de seq < before, du plus récent au plus ancien */
static int arch_read(uint8_t kind, const uint8_t *peer, uint32_t before, mc_msg_t *out, int max)
{
    EXT_RAM_BSS_ATTR static arch_rec_t blk[ARCH_BLK];
    /* reprise là où la lecture précédente s'est arrêtée (bouton « plus anciens ») */
    static uint8_t c_kind, c_peer[MC_PEER_LEN];
    static uint32_t c_before;
    static long c_pos = -1;

    if (max <= 0 || !sd_lock()) return 0;
    char path[64];
    arch_path(path, sizeof path, kind, peer);
    FILE *f = fopen(path, "rb");
    int got = 0;
    if (f) {
        long end = -1;
        if (fseek(f, 0, SEEK_END) == 0) end = ftell(f) / (long)sizeof(arch_rec_t);
        if (c_pos >= 0 && c_pos <= end && c_kind == kind && c_before == before &&
            !memcmp(c_peer, peer, MC_PEER_LEN))
            end = c_pos;
        uint32_t lim = before;
        while (end > 0 && got < max) {
            long start = end > ARCH_BLK ? end - ARCH_BLK : 0;
            size_t cnt = (size_t)(end - start);
            if (fseek(f, start * (long)sizeof(arch_rec_t), SEEK_SET) != 0 ||
                fread(blk, sizeof(arch_rec_t), cnt, f) != cnt) break;
            long j = (long)cnt - 1;
            for (; j >= 0 && got < max; j--) {
                mc_msg_t *m = &blk[j].m;
                if (blk[j].magic != ARCH_MAGIC || !same(m, kind, peer) || m->seq >= lim) continue;
                sanitize(m);
                m->flags |= MC_F_READ;
                out[got++] = *m;
                lim = m->seq;
            }
            end = start + j + 1;            /* au besoin, on repart du prochain non lu */
            if (got >= max) break;
        }
        fclose(f);
        c_kind = kind;
        memcpy(c_peer, peer, MC_PEER_LEN);
        c_before = lim;
        c_pos = end;
    }
    sd_unlock();
    return got;
}

/* ------------------------------------------------------------------ conversations */
bool mch_peers_wanted(void) { return s_peers_req; }

void mch_peers_set(const mch_peer_t *p, int n)
{
    if (!s_mx) return;
    if (n > MCH_PEERS_MAX) n = MCH_PEERS_MAX;
    lock();
    memcpy(s_peers, p, (size_t)n * sizeof *p);
    s_npeers = n;
    /* messages repris de l'ancien historique : rattachés à leur contact ou
     * canal par le nom */
    for (int i = 0; i < s_n; i++) {
        mc_msg_t *e = &AT(i);
        if (!(e->flags & MC_F_LEGACY)) continue;
        for (int k = 0; k < n; k++) {
            if (p[k].kind != e->kind || strcmp(p[k].name, e->from)) continue;
            memcpy(e->peer, p[k].peer, MC_PEER_LEN);
            e->flags &= ~MC_F_LEGACY;
            touch();
            break;
        }
    }
    s_peers_req = false;
    s_ver++;
    unlock();
    notify(NULL);
}

void mc_conv_refresh(void) { s_peers_req = true; }

typedef struct {
    mc_conv_t c;
    char key[24];
} item_t;

static int item_cmp(const void *a, const void *b)
{
    const item_t *x = a, *y = b;
    if ((x->c.unread > 0) != (y->c.unread > 0)) return x->c.unread > 0 ? -1 : 1;
    int r = strcmp(x->key, y->key);
    return r ? r : strcmp(x->c.name, y->c.name);
}

int mc_conv_list(uint8_t kind, mc_conv_t *out, int max)
{
    EXT_RAM_BSS_ATTR static item_t w[MCH_PEERS_MAX + MC_HIST_N];    /* tâche app seule */
    if (!s_mx) return 0;
    int n = 0;
    lock();
    for (int i = 0; i < s_npeers; i++) {
        if (s_peers[i].kind != kind) continue;
        memset(&w[n], 0, sizeof w[n]);
        w[n].c.kind = kind;
        memcpy(w[n].c.peer, s_peers[i].peer, MC_PEER_LEN);
        memcpy(w[n].c.name, s_peers[i].name, MC_NAME_MAX);
        n++;
    }
    for (int i = 0; i < s_n; i++) {          /* du plus récent au plus ancien */
        const mc_msg_t *e = &AT(i);
        if (e->kind != kind) continue;
        int k = 0;
        while (k < n && memcmp(w[k].c.peer, e->peer, MC_PEER_LEN)) k++;
        if (k == n) {                        /* contact supprimé depuis : on garde la conversation */
            memset(&w[n], 0, sizeof w[n]);
            w[n].c.kind = kind;
            memcpy(w[n].c.peer, e->peer, MC_PEER_LEN);
            memcpy(w[n].c.name, e->from, MC_NAME_MAX);
            n++;
        }
        mc_conv_t *c = &w[k].c;
        if (!(e->flags & (MC_F_READ | MC_F_OUT)) && c->unread < UINT16_MAX) c->unread++;
        if (!c->last_ts) {
            c->last_ts = e->ts ? e->ts : 1;
            c->last_out = e->flags & MC_F_OUT;
            mc_copy_utf8(c->last, sizeof c->last, e->text, SIZE_MAX);
        }
    }
    unlock();
    for (int i = 0; i < n; i++) mc_sort_key(w[i].c.name, w[i].key, sizeof w[i].key);
    qsort(w, (size_t)n, sizeof w[0], item_cmp);
    for (int i = 0; i < n && i < max; i++) out[i] = w[i].c;
    return n;
}

int mc_unread_total(uint8_t kind)
{
    if (!s_mx) return 0;
    int u = 0;
    lock();
    for (int i = 0; i < s_n; i++) {
        const mc_msg_t *e = &AT(i);
        if (e->kind == kind && !(e->flags & (MC_F_READ | MC_F_OUT))) u++;
    }
    unlock();
    return u;
}

void mc_conv_mark_read(uint8_t kind, const uint8_t peer[MC_PEER_LEN])
{
    if (!s_mx) return;
    lock();
    bool changed = false;
    for (int i = 0; i < s_n; i++) {
        mc_msg_t *e = &AT(i);
        if (same(e, kind, peer) && !(e->flags & MC_F_READ)) { e->flags |= MC_F_READ; changed = true; }
    }
    if (changed) touch();
    unlock();
}

int mc_conv_messages(uint8_t kind, const uint8_t peer[MC_PEER_LEN], uint32_t before, mc_msg_t *out, int max)
{
    if (!s_mx || max <= 0) return 0;
    int got = 0;
    uint32_t lim = before;
    lock();
    for (int i = 0; i < s_n && got < max; i++) {
        const mc_msg_t *e = &AT(i);
        if (same(e, kind, peer) && e->seq < lim) { out[got++] = *e; lim = e->seq; }
    }
    unlock();
    if (got < max) got += arch_read(kind, peer, lim, out + got, max - got);
    return got;
}

void mch_init(void)
{
    if (s_mx) return;
    s_mx = xSemaphoreCreateMutex();
    if (s_mx) sd_add_flusher(arch_flush);
}
