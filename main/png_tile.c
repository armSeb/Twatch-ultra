/* png_tile.c — voir png_tile.h */
#include "png_tile.h"
#include <stdbool.h>
#include <string.h>

#define BG_R ((PNG_TILE_BG >> 16) & 0xFF)
#define BG_G ((PNG_TILE_BG >> 8) & 0xFF)
#define BG_B (PNG_TILE_BG & 0xFF)

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static inline uint16_t rgb565(unsigned r, unsigned g, unsigned b)
{
    return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}

static inline unsigned blend(unsigned c, unsigned a, unsigned bg)
{
    return (c * a + bg * (255 - a) + 127) / 255;
}

typedef struct {
    uint32_t w, h;
    uint8_t  depth, ctype;
    uint8_t  bpp;               /* bits par pixel */
    uint32_t rowbytes;
    uint8_t  pal[256][4];       /* RGBA */
    bool     trns;              /* gris ou RGB : couleur transparente */
    uint16_t key[3];
} hdr_t;

/* ---------------------------------------------------------------- filtres */
static inline uint8_t paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return (uint8_t)a;
    return (uint8_t)(pb <= pc ? b : c);
}

/* lignes sur place : [filtre][rowbytes] × h */
static bool unfilter(uint8_t *raw, uint32_t h, uint32_t rb, unsigned fb)
{
    const uint8_t *prev = NULL;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *r = raw + (size_t)y * (rb + 1);
        uint8_t f = r[0];
        uint8_t *p = r + 1;
        switch (f) {
        case 0: break;
        case 1:
            for (uint32_t i = fb; i < rb; i++) p[i] += p[i - fb];
            break;
        case 2:
            if (prev) for (uint32_t i = 0; i < rb; i++) p[i] += prev[i];
            break;
        case 3:
            for (uint32_t i = 0; i < rb; i++) {
                unsigned a = i >= fb ? p[i - fb] : 0, b = prev ? prev[i] : 0;
                p[i] += (uint8_t)((a + b) >> 1);
            }
            break;
        case 4:
            for (uint32_t i = 0; i < rb; i++) {
                int a = i >= fb ? p[i - fb] : 0, b = prev ? prev[i] : 0, c = (prev && i >= fb) ? prev[i - fb] : 0;
                p[i] += paeth(a, b, c);
            }
            break;
        default:
            return false;
        }
        prev = p;
    }
    return true;
}

/* ---------------------------------------------------------------- pixels */
/* pixel x de la ligne p (sans l'octet de filtre) → RGB posé sur le fond */
static void pixel(const hdr_t *h, const uint8_t *p, uint32_t x, unsigned *r, unsigned *g, unsigned *b)
{
    unsigned a = 255, v;
    switch (h->ctype) {
    case 3: {                                           /* palette */
        unsigned i;
        if (h->depth == 8) i = p[x];
        else {
            unsigned per = 8 / h->depth, sh = 8 - h->depth * (x % per + 1);
            i = (p[x / per] >> sh) & ((1u << h->depth) - 1);
        }
        *r = h->pal[i][0]; *g = h->pal[i][1]; *b = h->pal[i][2]; a = h->pal[i][3];
        break;
    }
    case 0:                                             /* gris */
        if (h->depth == 16) {
            v = p[2 * x];
            if (h->trns && (uint16_t)(p[2 * x] << 8 | p[2 * x + 1]) == h->key[0]) a = 0;
        } else if (h->depth == 8) {
            v = p[x];
            if (h->trns && v == h->key[0]) a = 0;
        } else {
            unsigned per = 8 / h->depth, sh = 8 - h->depth * (x % per + 1), m = (1u << h->depth) - 1;
            unsigned s = (p[x / per] >> sh) & m;
            if (h->trns && s == h->key[0]) a = 0;
            v = s * 255 / m;
        }
        *r = *g = *b = v;
        break;
    case 4:                                             /* gris + alpha */
        if (h->depth == 16) { v = p[4 * x]; a = p[4 * x + 2]; }
        else { v = p[2 * x]; a = p[2 * x + 1]; }
        *r = *g = *b = v;
        break;
    case 2:                                             /* RGB */
        if (h->depth == 16) {
            const uint8_t *q = p + 6 * x;
            *r = q[0]; *g = q[2]; *b = q[4];
            if (h->trns && (uint16_t)(q[0] << 8 | q[1]) == h->key[0] && (uint16_t)(q[2] << 8 | q[3]) == h->key[1] &&
                (uint16_t)(q[4] << 8 | q[5]) == h->key[2]) a = 0;
        } else {
            const uint8_t *q = p + 3 * x;
            *r = q[0]; *g = q[1]; *b = q[2];
            if (h->trns && q[0] == h->key[0] && q[1] == h->key[1] && q[2] == h->key[2]) a = 0;
        }
        break;
    default:                                            /* 6 : RGBA */
        if (h->depth == 16) { const uint8_t *q = p + 8 * x; *r = q[0]; *g = q[2]; *b = q[4]; a = q[6]; }
        else { const uint8_t *q = p + 4 * x; *r = q[0]; *g = q[1]; *b = q[2]; a = q[3]; }
        break;
    }
    if (a != 255) {
        *r = blend(*r, a, BG_R);
        *g = blend(*g, a, BG_G);
        *b = blend(*b, a, BG_B);
    }
}

/* ---------------------------------------------------------------- lecture */
int png_tile_decode(png_tile_ctx_t *c, uint8_t *png, size_t len, uint16_t *out)
{
    static const uint8_t SIG[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    if (len < 8 + 25 || memcmp(png, SIG, 8)) return PNG_E_FORMAT;
    hdr_t h;
    memset(&h, 0, sizeof h);
    for (int i = 0; i < 256; i++) h.pal[i][3] = 255;
    bool ihdr = false, plte = false, end = false;
    size_t pos = 8, zlen = 0;
    uint8_t *z = NULL;                  /* IDAT regroupés au début du premier */
    while (!end) {
        if (len - pos < 12) return PNG_E_FORMAT;
        uint32_t n = be32(png + pos);
        const uint8_t *t = png + pos + 4;
        uint8_t *d = png + pos + 8;
        if (n > len - pos - 12) return PNG_E_FORMAT;
        if (!memcmp(t, "IHDR", 4)) {
            if (n != 13 || ihdr) return PNG_E_FORMAT;
            h.w = be32(d);
            h.h = be32(d + 4);
            h.depth = d[8];
            h.ctype = d[9];
            if (d[10] || d[11]) return PNG_E_FORMAT;
            if (d[12]) return PNG_E_UNSUPPORTED;        /* entrelacé */
            static const uint8_t CH[7] = { 1, 0, 3, 1, 2, 0, 4 };
            if (h.ctype > 6 || !CH[h.ctype]) return PNG_E_FORMAT;
            bool ok;
            switch (h.depth) {
            case 1: case 2: case 4: ok = h.ctype == 0 || h.ctype == 3; break;
            case 8: ok = true; break;
            case 16: ok = h.ctype != 3; break;
            default: ok = false; break;
            }
            if (!ok) return PNG_E_FORMAT;
            if (!((h.w == 256 && h.h == 256) || (h.w == 512 && h.h == 512))) return PNG_E_UNSUPPORTED;
            h.bpp = (uint8_t)(CH[h.ctype] * h.depth);
            h.rowbytes = (h.w * h.bpp + 7) / 8;
            ihdr = true;
        } else if (!ihdr) {
            return PNG_E_FORMAT;
        } else if (!memcmp(t, "PLTE", 4)) {
            if (n % 3 || n > 768 || n == 0) return PNG_E_FORMAT;
            for (uint32_t i = 0; i < n / 3; i++) memcpy(h.pal[i], d + 3 * i, 3);
            plte = true;
        } else if (!memcmp(t, "tRNS", 4)) {
            if (h.ctype == 3) {
                for (uint32_t i = 0; i < n && i < 256; i++) h.pal[i][3] = d[i];
            } else if (h.ctype == 0 && n >= 2) {
                h.key[0] = (uint16_t)(d[0] << 8 | d[1]);
                h.trns = true;
            } else if (h.ctype == 2 && n >= 6) {
                for (int i = 0; i < 3; i++) h.key[i] = (uint16_t)(d[2 * i] << 8 | d[2 * i + 1]);
                h.trns = true;
            }
        } else if (!memcmp(t, "IDAT", 4)) {
            if (!z) z = d;
            memmove(z + zlen, d, n);        /* z + zlen <= d : on ne fait que tasser */
            zlen += n;
        } else if (!memcmp(t, "IEND", 4)) {
            end = true;
        } else if (!(t[0] & 0x20)) {
            return PNG_E_UNSUPPORTED;       /* bloc critique inconnu */
        }
        pos += 12 + (size_t)n;
    }
    if (!ihdr || !z || !zlen || (h.ctype == 3 && !plte)) return PNG_E_FORMAT;
    /* 8 bits : gris ou RGB à transparence, la clé tient sur un octet */
    if (h.depth == 8 && h.trns) for (int i = 0; i < 3; i++) h.key[i] &= 0xFF;

    size_t need = (size_t)h.h * (h.rowbytes + 1);
    if (c->raw_cap < need) {
        uint8_t *p = c->realloc_fn(c->raw, need);
        if (!p) return PNG_E_MEMORY;
        c->raw = p;
        c->raw_cap = need;
    }
    if (c->inflate(z, zlen, c->raw, need, c->inflate_ctx) != (long)need) return PNG_E_INFLATE;
    unsigned fb = h.bpp >= 8 ? h.bpp / 8 : 1;
    if (!unfilter(c->raw, h.h, h.rowbytes, fb)) return PNG_E_FORMAT;

    const size_t stride = h.rowbytes + 1;
    if (h.w == PNG_TILE) {
        if (h.ctype == 3 && h.depth == 8) {         /* tuiles OSM : palette 8 bits */
            uint16_t lut[256];
            for (int i = 0; i < 256; i++) {
                unsigned a = h.pal[i][3];
                lut[i] = rgb565(blend(h.pal[i][0], a, BG_R), blend(h.pal[i][1], a, BG_G), blend(h.pal[i][2], a, BG_B));
            }
            for (int y = 0; y < PNG_TILE; y++) {
                const uint8_t *p = c->raw + (size_t)y * stride + 1;
                uint16_t *o = out + y * PNG_TILE;
                for (int x = 0; x < PNG_TILE; x++) o[x] = lut[p[x]];
            }
        } else {
            for (int y = 0; y < PNG_TILE; y++) {
                const uint8_t *p = c->raw + (size_t)y * stride + 1;
                uint16_t *o = out + y * PNG_TILE;
                for (int x = 0; x < PNG_TILE; x++) {
                    unsigned r, g, b;
                    pixel(&h, p, (uint32_t)x, &r, &g, &b);
                    o[x] = rgb565(r, g, b);
                }
            }
        }
    } else {                                        /* 512 : moyenne 2 × 2 */
        for (int y = 0; y < PNG_TILE; y++) {
            const uint8_t *p0 = c->raw + (size_t)(2 * y) * stride + 1, *p1 = p0 + stride;
            uint16_t *o = out + y * PNG_TILE;
            for (int x = 0; x < PNG_TILE; x++) {
                unsigned sr = 0, sg = 0, sb = 0, r, g, b;
                for (int k = 0; k < 4; k++) {
                    pixel(&h, k < 2 ? p0 : p1, (uint32_t)(2 * x + (k & 1)), &r, &g, &b);
                    sr += r; sg += g; sb += b;
                }
                o[x] = rgb565((sr + 2) / 4, (sg + 2) / 4, (sb + 2) / 4);
            }
        }
    }
    return PNG_OK;
}
