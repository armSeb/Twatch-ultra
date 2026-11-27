/* Tests PC du lecteur PMTiles (main/pmtiles.c) : fichiers écrits par le
 * paquet Python pmtiles (gen_pmtiles.py), racine seule et avec feuilles,
 * tuiles identiques regroupées ; puis répertoires abîmés (ASan). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "pmtiles.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("ÉCHEC %s:%d : %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

typedef struct { uint8_t *b; size_t n; } mem_t;

static int rd(void *ctx, uint64_t off, void *buf, size_t n)
{
    mem_t *m = ctx;
    if (off > m->n || n > m->n - off) return -1;
    memcpy(buf, m->b + off, n);
    return 0;
}

static long raw_inflate(const uint8_t *in, size_t n, uint8_t *out, size_t out_len)
{
    z_stream s = { 0 };
    if (inflateInit2(&s, -15) != Z_OK) return -1;
    s.next_in = (Bytef *)in; s.avail_in = (uInt)n;
    s.next_out = out; s.avail_out = (uInt)out_len;
    int r = inflate(&s, Z_FINISH);
    long got = (long)(out_len - s.avail_out);
    inflateEnd(&s);
    return r == Z_STREAM_END || r == Z_BUF_ERROR || r == Z_OK ? got : -1;
}

static mem_t load(const char *path)
{
    mem_t m = { 0 };
    FILE *f = fopen(path, "rb");
    if (!f) { printf("absent : %s\n", path); fails++; return m; }
    fseek(f, 0, SEEK_END);
    m.n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    m.b = malloc(m.n);
    if (fread(m.b, 1, m.n, f) != m.n) m.n = 0;
    fclose(f);
    return m;
}

static void run(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.pmtiles", dir, name);
    mem_t m = load(path);
    if (!m.b) return;
    pm_t p = { .read = rd, .inflate = raw_inflate, .alloc = malloc, .free = free, .io = &m };
    int r = pm_open(&p);
    CHECK(r == PM_OK);
    CHECK(p.tile_type == PM_TYPE_PNG && p.center_lat_e7 == 488566000);
    snprintf(path, sizeof path, "%s/%s.txt", dir, name);
    FILE *f = fopen(path, "r");
    int z, n = 0, bad = 0;
    unsigned x, y;
    long long eoff;
    unsigned elen;
    while (f && fscanf(f, "%d %u %u %lld %u", &z, &x, &y, &eoff, &elen) == 5) {
        uint64_t off = 0;
        uint32_t len = 0;
        int k = pm_find(&p, z, x, y, &off, &len);
        bool ok = eoff < 0 ? k == 0 : (k == 1 && len == elen && !memcmp(m.b + off, m.b + eoff, elen));
        if (!ok && bad++ < 5) printf("ÉCHEC %s %d/%u/%u : %d off %llu len %u, attendu %lld %u\n", name, z, x, y, k,
                                     (unsigned long long)off, len, eoff, elen);
        n++;
    }
    if (f) fclose(f);
    if (bad) fails++;
    printf("pmtiles %s : %d tuiles vérifiées, racine %u entrées\n", name, n, p.root.n);
    pm_close(&p);
    /* répertoires abîmés : octets changés au hasard, aucun accès hors bornes */
    srand(2);
    for (int i = 0; i < 400; i++) {
        mem_t c = { malloc(m.n), m.n };
        memcpy(c.b, m.b, m.n);
        size_t span = m.n < 20000 ? m.n : 20000;   /* en-tête, racine, feuilles du début */
        for (int j = 0; j < 3; j++) c.b[rand() % span] = (uint8_t)rand();
        pm_t q = { .read = rd, .inflate = raw_inflate, .alloc = malloc, .free = free, .io = &c };
        if (pm_open(&q) == PM_OK) {
            uint64_t off; uint32_t len;
            for (int t = 0; t < 50; t++) {
                int zz = rand() % 12;
                pm_find(&q, zz, (uint32_t)rand() & ((1u << zz) - 1), (uint32_t)rand() & ((1u << zz) - 1), &off, &len);
            }
        }
        pm_close(&q);
        free(c.b);
    }
    free(m.b);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    /* numéros de tuile de la spécification PMTiles */
    CHECK(pm_tile_id(0, 0, 0) == 0);
    CHECK(pm_tile_id(1, 0, 0) == 1);
    CHECK(pm_tile_id(1, 0, 1) == 2);
    CHECK(pm_tile_id(1, 1, 1) == 3);
    CHECK(pm_tile_id(1, 1, 0) == 4);
    CHECK(pm_tile_id(2, 0, 0) == 5);
    CHECK(pm_tile_id(20, 0, 0) == 366503875925ULL);
    run(dir, "small");
    run(dir, "big");
    if (fails) { printf("pmtiles : %d échec(s)\n", fails); return 1; }
    printf("pmtiles : OK\n");
    return 0;
}
