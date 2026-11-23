/*
 * emoji.c — voir emoji.h.
 *
 * Pack (tools/gen_emoji.py) : en-tête, table d'entrées triées par séquence
 * de points de code, séquences, images RGB565A8 en deflate brut.
 */
#include "emoji.h"
#include <string.h>
#include <stdint.h>
#include "fonts/fonts.h"

#ifdef ESP_PLATFORM
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "miniz.h"          /* tinfl de la ROM */
static const char *TAG = "emoji";
#define EMJ_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
/* aperçu PC : pack lu depuis un fichier, zlib du système */
#include <stdio.h>
#include <stdlib.h>
#include <zlib.h>
#define ESP_LOGI(t, ...) ((void)0)
#define ESP_LOGW(t, ...) ((void)0)
#define EMJ_ALLOC(n) malloc(n)
#endif

#define PUA_BASE   0xF0000u
#define MAX_SEQ    10          /* plus longue séquence du pack (famille ZWJ) */
#define CACHE_MAX  192         /* images décodées gardées par taille */
#define BAD_CP     0xFFFFFFFFu /* UTF-8 invalide (coupure d'un snprintf…) : ignoré */

typedef struct __attribute__((packed)) {
    char     magic[4];
    uint16_t count;
    uint8_t  nsizes, version;
    uint8_t  sizes[4];
    uint32_t off_seq, off_img;
} pack_hdr_t;

static const uint8_t *s_pack;
static const pack_hdr_t *s_hdr;
static int s_count, s_ent_sz;
static size_t s_size;

/* ------------------------------------------------------------------ */
/* Pack                                                                 */
/* ------------------------------------------------------------------ */
static inline uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static const uint8_t *entry(int i) { return s_pack + sizeof(pack_hdr_t) + (size_t)i * s_ent_sz; }

/* compare la séquence de l'entrée i à seq[0..n) */
static int cmp_entry(int i, const uint32_t *seq, int n)
{
    const uint8_t *e = entry(i);
    const uint8_t *sp = s_pack + s_hdr->off_seq + rd32(e);
    int m = e[4];
    for (int k = 0; k < m && k < n; k++) {
        uint32_t c = rd32(sp + 4 * k);
        if (c != seq[k]) return c < seq[k] ? -1 : 1;
    }
    return m == n ? 0 : (m < n ? -1 : 1);
}

static int find(const uint32_t *seq, int n)
{
    int lo = 0, hi = s_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = cmp_entry(mid, seq, n);
        if (c == 0) return mid;
        if (c < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

bool emoji_init(void)
{
    const uint8_t *p = NULL;
#ifdef ESP_PLATFORM
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "emoji");
    esp_partition_mmap_handle_t h;
    if (!part || esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, (const void **)&p, &h) != ESP_OK) {
        ESP_LOGW(TAG, "partition emoji absente");
        return false;
    }
#else
    const char *fn = getenv("EMOJI_BIN");
    FILE *f = fn ? fopen(fn, "rb") : NULL;
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = sz > 0 ? malloc(sz) : NULL;
    if (!buf || fread(buf, 1, sz, f) != (size_t)sz) { fclose(f); free(buf); return false; }
    fclose(f);
    p = buf;
#endif
#ifdef ESP_PLATFORM
    size_t size = part->size;
#else
    size_t size = (size_t)sz;
#endif
    const pack_hdr_t *h0 = (const pack_hdr_t *)p;
    /* le pack est à nous, mais une partition effacée ou tronquée ne doit pas
     * faire lire hors de la projection */
    size_t ent_end = sizeof(pack_hdr_t) + (size_t)h0->count * (8 + 8 * 2);
    if (size < sizeof *h0 || memcmp(h0->magic, "EMJ1", 4) || h0->nsizes != 2 || h0->sizes[0] != 20 ||
        h0->sizes[1] != 28 || ent_end > h0->off_seq || h0->off_seq > h0->off_img || h0->off_img > size) {
        ESP_LOGW(TAG, "pack emoji invalide");
        return false;
    }
    s_pack = p;
    s_size = size;
    s_hdr = h0;
    s_ent_sz = 8 + 8 * h0->nsizes;
    s_count = h0->count;
    ESP_LOGI(TAG, "%d émojis", s_count);
    return true;
}

int emoji_count(void) { return s_count; }

/* ------------------------------------------------------------------ */
/* Texte : séquences → points de code privés                            */
/* ------------------------------------------------------------------ */
static bool is_skin(uint32_t c) { return c >= 0x1F3FB && c <= 0x1F3FF; }
static bool is_invisible(uint32_t c)
{
    return c == 0xFE0F || c == 0xFE0E || c == 0x200D || is_skin(c) || (c >= 0xE0020 && c <= 0xE007F);
}

/* caractères du plan de base affichés en émoji même sans U+FE0F */
static bool bmp_emoji_default(uint32_t c)
{
    static const uint16_t R[][2] = {
        { 0x231A, 0x231B }, { 0x23E9, 0x23EC }, { 0x23F0, 0x23F0 }, { 0x23F3, 0x23F3 }, { 0x25FD, 0x25FE },
        { 0x2614, 0x2615 }, { 0x2648, 0x2653 }, { 0x267F, 0x267F }, { 0x2693, 0x2693 }, { 0x26A1, 0x26A1 },
        { 0x26AA, 0x26AB }, { 0x26BD, 0x26BE }, { 0x26C4, 0x26C5 }, { 0x26CE, 0x26CE }, { 0x26D4, 0x26D4 },
        { 0x26EA, 0x26EA }, { 0x26F2, 0x26F3 }, { 0x26F5, 0x26F5 }, { 0x26FA, 0x26FA }, { 0x26FD, 0x26FD },
        { 0x2705, 0x2705 }, { 0x270A, 0x270B }, { 0x2728, 0x2728 }, { 0x274C, 0x274C }, { 0x274E, 0x274E },
        { 0x2753, 0x2755 }, { 0x2757, 0x2757 }, { 0x2795, 0x2797 }, { 0x27B0, 0x27B0 }, { 0x27BF, 0x27BF },
        { 0x2B1B, 0x2B1C }, { 0x2B50, 0x2B50 }, { 0x2B55, 0x2B55 },
    };
    for (size_t i = 0; i < sizeof R / sizeof R[0]; i++)
        if (c >= R[i][0] && c <= R[i][1]) return true;
    return false;
}

static int utf8_get(const unsigned char *s, uint32_t *cp)
{
    unsigned char b = s[0];
    int len = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 0;
    if (!len) { *cp = BAD_CP; return 1; }
    uint32_t c = len == 1 ? b : len == 2 ? b & 0x1F : len == 3 ? b & 0x0F : b & 0x07;
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) { *cp = BAD_CP; return i; }
        c = (c << 6) | (s[i] & 0x3F);
    }
    /* formes trop longues, surrogates, hors Unicode : rejetés */
    static const uint32_t MIN[5] = { 0, 0, 0x80, 0x800, 0x10000 };
    *cp = (c < MIN[len] || (c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF) ? BAD_CP : c;
    return len;
}

static int utf8_len(uint32_t c) { return c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4; }

static int utf8_put(char *o, uint32_t c)
{
    if (c < 0x80) { o[0] = c; return 1; }
    if (c < 0x800) { o[0] = 0xC0 | (c >> 6); o[1] = 0x80 | (c & 0x3F); return 2; }
    if (c < 0x10000) { o[0] = 0xE0 | (c >> 12); o[1] = 0x80 | ((c >> 6) & 0x3F); o[2] = 0x80 | (c & 0x3F); return 3; }
    o[0] = 0xF0 | (c >> 18); o[1] = 0x80 | ((c >> 12) & 0x3F); o[2] = 0x80 | ((c >> 6) & 0x3F); o[3] = 0x80 | (c & 0x3F);
    return 4;
}

size_t emoji_text(const char *in, char *out, size_t cap)
{
    const unsigned char *s = (const unsigned char *)in;
    size_t o = 0;
    if (!cap) return 0;
    while (*s) {
        uint32_t c;
        int l = utf8_get(s, &c);
        uint32_t put = c;
        int used = l;
        bool keycap = (c == '#' || c == '*' || (c >= '0' && c <= '9')) &&
                      (!strncmp((const char *)s + 1, "\xEF\xB8\x8F\xE2\x83\xA3", 6) ||   /* FE0F 20E3 */
                       !strncmp((const char *)s + 1, "\xE2\x83\xA3", 3));                  /* 20E3 */
        if (s_count && (c >= 0xA9 || keycap)) {
            /* séquence normalisée (sans sélecteurs ni tons) et octets consommés après chaque élément */
            uint32_t seq[MAX_SEQ];
            int end[MAX_SEQ], n = 0, pos = 0;
            bool fe0f_after_first = false;
            while (s[pos] && n < MAX_SEQ) {
                uint32_t d;
                int dl = utf8_get(s + pos, &d);
                if (n > 0 && is_skin(d)) { pos += dl; end[n - 1] = pos; continue; }
                if (d == 0xFE0F || d == 0xFE0E) {
                    if (n == 1 && d == 0xFE0F) fe0f_after_first = true;
                    pos += dl;
                    if (n) end[n - 1] = pos;
                    continue;
                }
                /* une séquence ne continue qu'après un ZWJ, un keycap, un drapeau ou une étiquette */
                if (n > 0) {
                    uint32_t prev = seq[n - 1];
                    bool cont = prev == 0x200D || d == 0x200D || d == 0x20E3 || (d >= 0xE0020 && d <= 0xE007F) ||
                                (prev >= 0x1F1E6 && prev <= 0x1F1FF && d >= 0x1F1E6 && d <= 0x1F1FF && n == 1);
                    if (!cont) break;
                }
                seq[n] = d;
                pos += dl;
                end[n++] = pos;
            }
            for (int k = n; k > 0; k--) {
                if (seq[k - 1] == 0x200D) continue;              /* ne finit pas sur un ZWJ */
                int idx = find(seq, k);
                if (idx < 0) continue;
                if (k == 1 && c < 0x1F000 && !fe0f_after_first && !bmp_emoji_default(c)) break;   /* présentation texte */
                if (k == 1 && c < 0x80) break;                   /* chiffre seul : reste du texte */
                put = PUA_BASE + idx;
                used = end[k - 1];
                break;
            }
        }
        s += used;
        if (put == c && (c == BAD_CP || is_invisible(c))) continue;
        int pl = utf8_len(put);
        if (o + pl >= cap) break;
        o += utf8_put(out + o, put);
    }
    out[o] = 0;
    return o;
}

void emoji_label_set_text(lv_obj_t *label, const char *utf8)
{
    static char buf[1024];
    emoji_text(utf8, buf, sizeof buf);
    lv_label_set_text(label, buf);
}

/* ------------------------------------------------------------------ */
/* Polices                                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t si;                 /* rang de la taille dans le pack */
    uint8_t px;
    int8_t  ofs_y;
    uint16_t n;
    uint16_t idx[CACHE_MAX];
    lv_image_dsc_t *img[CACHE_MAX];
    uint8_t *buf[CACHE_MAX];
} efont_t;

static const lv_image_dsc_t *decode(efont_t *f, int idx)
{
    for (int i = 0; i < f->n; i++)
        if (f->idx[i] == idx) return f->img[i];
    if (f->n >= CACHE_MAX) {                       /* plein : on repart de zéro */
        lv_image_cache_drop(NULL);
        for (int i = 0; i < f->n; i++) { lv_free(f->img[i]); free(f->buf[i]); }
        f->n = 0;
    }
    const uint8_t *e = entry(idx);
    uint32_t off = rd32(e + 8 + 8 * f->si), len = rd32(e + 12 + 8 * f->si);
    size_t raw = (size_t)f->px * f->px * 3;
    if ((size_t)s_hdr->off_img + off + len > s_size) return NULL;
    uint8_t *px = EMJ_ALLOC(raw);
    lv_image_dsc_t *d = lv_malloc_zeroed(sizeof *d);
    if (!px || !d) { free(px); lv_free(d); return NULL; }
    const uint8_t *src = s_pack + s_hdr->off_img + off;
#ifdef ESP_PLATFORM
    /* tinfl_decompress_mem_to_mem() pose son décompresseur (~11 Ko) sur la
     * pile : débordement de la tâche « app » au premier émoji. Un seul,
     * alloué une fois en PSRAM (LVGL rend depuis une seule tâche). */
    static tinfl_decompressor *inf;
    if (!inf) inf = heap_caps_malloc(sizeof *inf, MALLOC_CAP_SPIRAM);
    size_t got = 0;
    if (inf) {
        tinfl_init(inf);
        size_t in_n = len, out_n = raw;
        tinfl_status st = tinfl_decompress(inf, src, &in_n, px, px, &out_n,
                                           TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        if (st == TINFL_STATUS_DONE) got = out_n;
    }
#else
    z_stream z = { 0 };
    inflateInit2(&z, -15);
    z.next_in = (Bytef *)src; z.avail_in = len; z.next_out = px; z.avail_out = raw;
    inflate(&z, Z_FINISH);
    size_t got = z.total_out;
    inflateEnd(&z);
#endif
    if (got != raw) { free(px); lv_free(d); return NULL; }
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_RGB565A8;
    d->header.w = f->px;
    d->header.h = f->px;
    d->header.stride = f->px * 2;
    d->data_size = raw;
    d->data = px;
    f->idx[f->n] = idx;
    f->img[f->n] = d;
    f->buf[f->n] = px;
    f->n++;
    return d;
}

static bool get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *g, uint32_t cp, uint32_t next)
{
    efont_t *f = (efont_t *)font->dsc;
    if (cp < PUA_BASE || cp >= PUA_BASE + (uint32_t)s_count) return false;
    const lv_image_dsc_t *img = decode(f, cp - PUA_BASE);
    if (!img) return false;
    g->is_placeholder = 0;
    g->adv_w = f->px + 2;
    g->box_w = f->px;
    g->box_h = f->px;
    g->ofs_x = 1;
    g->ofs_y = f->ofs_y;
    g->format = LV_FONT_GLYPH_FORMAT_IMAGE;
    g->gid.src = img;
    return true;
}

static const void *get_glyph_bitmap(lv_font_glyph_dsc_t *g, lv_draw_buf_t *draw_buf)
{
    return g->gid.src;
}

static efont_t s_f20 = { .si = 0, .px = 20 }, s_f28 = { .si = 1, .px = 28 };

#define EFONT(var, dscp, lh)                                                         \
    lv_font_t var = {                                                                \
        .get_glyph_dsc = get_glyph_dsc, .get_glyph_bitmap = get_glyph_bitmap,       \
        .line_height = lh, .base_line = 0, .subpx = LV_FONT_SUBPX_NONE, .dsc = dscp, \
    }
EFONT(emoji_font_20, &s_f20, 20);
EFONT(emoji_font_28, &s_f28, 28);

/* centre l'émoji sur la ligne de la police qui l'appelle en secours */
__attribute__((constructor)) static void emoji_metrics(void)
{
    s_f20.ofs_y = (font_small.line_height - font_small.base_line) - 20 - (font_small.line_height - 20) / 2;
    s_f28.ofs_y = (font_text.line_height - font_text.base_line) - 28 - (font_text.line_height - 28) / 2;
}
