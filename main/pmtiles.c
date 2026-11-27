/* pmtiles.c — voir pmtiles.h */
#include "pmtiles.h"
#include <string.h>

static uint64_t le64(const uint8_t *b)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = v << 8 | b[i];
    return v;
}

static int32_t le32s(const uint8_t *b)
{
    return (int32_t)((uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24);
}

/* ------------------------------------------------------------------ numéro de tuile */
/* niveaux plus larges d'abord, puis courbe de Hilbert dans le niveau */
uint64_t pm_tile_id(int z, uint32_t x, uint32_t y)
{
    uint64_t acc = (((uint64_t)1 << (2 * z)) - 1) / 3;
    uint64_t n = (uint64_t)1 << z, d = 0, rx, ry, tx = x, ty = y;
    for (uint64_t s = n / 2; s > 0; s /= 2) {
        rx = (tx & s) > 0;
        ry = (ty & s) > 0;
        d += s * s * ((3 * rx) ^ ry);
        if (ry == 0) {
            if (rx == 1) {
                tx = n - 1 - tx;
                ty = n - 1 - ty;
            }
            uint64_t t = tx;
            tx = ty;
            ty = t;
        }
    }
    return acc + d;
}

/* ------------------------------------------------------------------ répertoires */
static bool varint(const uint8_t **p, const uint8_t *end, uint64_t *v)
{
    uint64_t r = 0;
    for (int sh = 0; sh < 64; sh += 7) {
        if (*p >= end) return false;
        uint8_t b = *(*p)++;
        r |= (uint64_t)(b & 0x7F) << sh;
        if (!(b & 0x80)) { *v = r; return true; }
    }
    return false;
}

/* gzip → données brutes ; *out alloué (à libérer) */
static int gunzip(pm_t *p, const uint8_t *in, size_t n, uint8_t **out, size_t *out_n)
{
    if (n < 18 || in[0] != 0x1F || in[1] != 0x8B || in[2] != 8) return PM_E_FORMAT;
    uint8_t fl = in[3];
    size_t pos = 10;
    if (fl & 4) {                                  /* FEXTRA */
        if (pos + 2 > n) return PM_E_FORMAT;
        pos += 2 + (size_t)(in[pos] | in[pos + 1] << 8);
    }
    for (int f = 8; f <= 16; f += 8) {             /* FNAME, FCOMMENT */
        if (!(fl & f)) continue;
        while (pos < n && in[pos]) pos++;
        pos++;
    }
    if (fl & 2) pos += 2;                          /* FHCRC */
    if (pos + 8 > n) return PM_E_FORMAT;
    uint32_t isize = (uint32_t)in[n - 4] | (uint32_t)in[n - 3] << 8 | (uint32_t)in[n - 2] << 16 | (uint32_t)in[n - 1] << 24;
    if (isize == 0 || isize > PM_DIR_MAX) return PM_E_FORMAT;
    uint8_t *o = p->alloc(isize);
    if (!o) return PM_E_MEMORY;
    if (p->inflate(in + pos, n - pos - 8, o, isize) != (long)isize) {
        p->free(o);
        return PM_E_FORMAT;
    }
    *out = o;
    *out_n = isize;
    return PM_OK;
}

static int parse_dir(pm_t *p, const uint8_t *b, size_t n, pm_dir_t *d)
{
    const uint8_t *q = b, *end = b + n;
    uint64_t cnt, v;
    if (!varint(&q, end, &cnt) || cnt == 0 || cnt > n) return PM_E_FORMAT;   /* au moins un octet par entrée */
    pm_entry_t *e = p->alloc((size_t)cnt * sizeof *e);
    if (!e) return PM_E_MEMORY;
    uint64_t id = 0;
    for (uint64_t i = 0; i < cnt; i++) {
        if (!varint(&q, end, &v)) goto bad;
        id += v;
        e[i].tile_id = id;
    }
    for (uint64_t i = 0; i < cnt; i++) {
        if (!varint(&q, end, &v) || v > UINT32_MAX) goto bad;
        e[i].run = (uint32_t)v;
    }
    for (uint64_t i = 0; i < cnt; i++) {
        if (!varint(&q, end, &v) || v > UINT32_MAX) goto bad;
        e[i].length = (uint32_t)v;
    }
    for (uint64_t i = 0; i < cnt; i++) {
        if (!varint(&q, end, &v)) goto bad;
        if (v == 0 && i > 0) e[i].offset = e[i - 1].offset + e[i - 1].length;
        else if (v == 0) goto bad;
        else e[i].offset = v - 1;
    }
    d->e = e;
    d->n = (uint32_t)cnt;
    return PM_OK;
bad:
    p->free(e);
    return PM_E_FORMAT;
}

/* lit, décompresse et décode le répertoire [off, off + len) */
static int load_dir(pm_t *p, uint64_t off, uint64_t len, pm_dir_t *d)
{
    if (len == 0 || len > PM_DIR_MAX) return PM_E_FORMAT;
    uint8_t *raw = p->alloc((size_t)len);
    if (!raw) return PM_E_MEMORY;
    if (p->read(p->io, off, raw, (size_t)len)) {
        p->free(raw);
        return PM_E_IO;
    }
    int r;
    if (p->internal_comp == PM_COMP_GZIP) {
        uint8_t *u;
        size_t un;
        r = gunzip(p, raw, (size_t)len, &u, &un);
        p->free(raw);
        if (r) return r;
        r = parse_dir(p, u, un, d);
        p->free(u);
    } else {
        r = parse_dir(p, raw, (size_t)len, d);
        p->free(raw);
    }
    return r;
}

/* dernière entrée de début <= id, si elle couvre id ou mène à une feuille */
static const pm_entry_t *lookup(const pm_dir_t *d, uint64_t id)
{
    int64_t lo = 0, hi = (int64_t)d->n - 1;
    while (lo <= hi) {
        int64_t k = (lo + hi) / 2;
        if (d->e[k].tile_id < id) lo = k + 1;
        else if (d->e[k].tile_id > id) hi = k - 1;
        else return &d->e[k];
    }
    if (hi < 0) return NULL;
    const pm_entry_t *e = &d->e[hi];
    if (e->run == 0) return e;
    return id - e->tile_id < e->run ? e : NULL;
}

/* ------------------------------------------------------------------ API */
int pm_open(pm_t *p)
{
    uint8_t h[127];
    memset(&p->root, 0, sizeof p->root);
    memset(p->leaf, 0, sizeof p->leaf);
    if (p->read(p->io, 0, h, sizeof h)) return PM_E_IO;
    if (memcmp(h, "PMTiles", 7) || h[7] != 3) return PM_E_FORMAT;
    p->root_off = le64(h + 8);
    p->root_len = le64(h + 16);
    p->leaf_off = le64(h + 40);
    p->leaf_len = le64(h + 48);
    p->data_off = le64(h + 56);
    p->data_len = le64(h + 64);
    p->internal_comp = h[97];
    p->tile_comp = h[98];
    p->tile_type = h[99];
    p->min_zoom = h[100];
    p->max_zoom = h[101];
    p->min_lon_e7 = le32s(h + 102);
    p->min_lat_e7 = le32s(h + 106);
    p->max_lon_e7 = le32s(h + 110);
    p->max_lat_e7 = le32s(h + 114);
    p->center_zoom = h[118];
    p->center_lon_e7 = le32s(h + 119);
    p->center_lat_e7 = le32s(h + 123);
    if (p->internal_comp != PM_COMP_GZIP && p->internal_comp != PM_COMP_NONE && p->internal_comp != 0)
        return PM_E_UNSUPPORTED;
    if (p->min_zoom > p->max_zoom || p->max_zoom > 30) return PM_E_FORMAT;
    return load_dir(p, p->root_off, p->root_len, &p->root);
}

void pm_close(pm_t *p)
{
    if (p->root.e) p->free(p->root.e);
    for (int i = 0; i < PM_LEAVES; i++)
        if (p->leaf[i].d.e) p->free(p->leaf[i].d.e);
    memset(&p->root, 0, sizeof p->root);
    memset(p->leaf, 0, sizeof p->leaf);
}

static int get_leaf(pm_t *p, uint64_t off, uint32_t len, pm_dir_t **out)
{
    int victim = 0;
    for (int i = 0; i < PM_LEAVES; i++) {
        if (p->leaf[i].d.e && p->leaf[i].off == off && p->leaf[i].len == len) {
            p->leaf[i].used = ++p->clock;
            *out = &p->leaf[i].d;
            return PM_OK;
        }
        if (!p->leaf[i].d.e) victim = i;
        else if (p->leaf[victim].d.e && p->leaf[i].used < p->leaf[victim].used) victim = i;
    }
    pm_dir_t d;
    int r = load_dir(p, p->leaf_off + off, len, &d);
    if (r) return r;
    if (p->leaf[victim].d.e) p->free(p->leaf[victim].d.e);
    p->leaf[victim].off = off;
    p->leaf[victim].len = len;
    p->leaf[victim].used = ++p->clock;
    p->leaf[victim].d = d;
    *out = &p->leaf[victim].d;
    return PM_OK;
}

int pm_find(pm_t *p, int z, uint32_t x, uint32_t y, uint64_t *off, uint32_t *len)
{
    if (!p->root.e || z < 0 || z > 30 || (z < 31 && (x >> z || y >> z))) return 0;
    uint64_t id = pm_tile_id(z, x, y);
    const pm_dir_t *d = &p->root;
    for (int depth = 0; depth < 4; depth++) {        /* racine, puis feuilles (3 niveaux au plus) */
        const pm_entry_t *e = lookup(d, id);
        if (!e) return 0;
        if (e->run > 0) {
            *off = p->data_off + e->offset;
            *len = e->length;
            return 1;
        }
        pm_dir_t *leaf;
        int r = get_leaf(p, e->offset, e->length, &leaf);
        if (r) return r;
        d = leaf;
    }
    return PM_E_FORMAT;
}
