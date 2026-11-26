/* Tests PC du décodeur de tuiles (main/png_tile.c) : chaque format de PNG
 * fabriqué par gen_png.py comparé à l'image attendue, puis des fichiers
 * tronqués ou abîmés (aucun débordement : ASan). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "png_tile.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("ÉCHEC %s:%d : %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static long zinf(const uint8_t *in, size_t n, uint8_t *out, size_t out_len, void *ctx)
{
    (void)ctx;
    uLongf ol = out_len;
    int r = uncompress(out, &ol, in, n);
    return r == Z_OK || r == Z_BUF_ERROR ? (long)ol : -1;
}

static void *rl(void *p, size_t n) { return realloc(p, n); }

static uint8_t *load(const char *dir, const char *name, const char *ext, size_t *len)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.%s", dir, name, ext);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("absent : %s\n", path); fails++; return NULL; }
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*len);
    if (fread(b, 1, *len, f) != *len) { free(b); b = NULL; }
    fclose(f);
    return b;
}

static int close565(uint16_t a, uint16_t b)
{
    int d0 = abs((a >> 11) - (b >> 11)), d1 = abs(((a >> 5) & 63) - ((b >> 5) & 63)), d2 = abs((a & 31) - (b & 31));
    return d0 <= 1 && d1 <= 1 && d2 <= 1;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "png";
    static const char *NAMES[] = { "rgba8", "rgb8", "gray8", "graya8", "pal8", "pal4", "pal1", "bw1",
                                   "rgba512", "palt8", "gray16" };
    png_tile_ctx_t c = { .inflate = zinf, .realloc_fn = rl };
    static uint16_t out[PNG_TILE * PNG_TILE];
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) {
        size_t n, en;
        uint8_t *png = load(dir, NAMES[i], "png", &n);
        uint8_t *exp = load(dir, NAMES[i], "565", &en);
        if (!png || !exp) { free(png); free(exp); continue; }
        CHECK(en == sizeof out);
        int r = png_tile_decode(&c, png, n, out);
        if (r != PNG_OK) { printf("ÉCHEC %s : code %d\n", NAMES[i], r); fails++; }
        int bad = 0;
        for (int k = 0; k < PNG_TILE * PNG_TILE && r == PNG_OK; k++) {
            uint16_t e = (uint16_t)(exp[2 * k] | exp[2 * k + 1] << 8);
            if (!close565(out[k], e)) {
                if (!bad) printf("ÉCHEC %s : pixel %d,%d %04X attendu %04X\n", NAMES[i], k % 256, k / 256, out[k], e);
                bad++;
            }
        }
        if (bad) fails++;
        /* abîmé : tronqué partout, octets changés au hasard */
        srand(1);
        uint8_t *orig = load(dir, NAMES[i], "png", &n);
        for (size_t cut = 0; cut < n; cut += 1 + n / 97) {
            uint8_t *t = malloc(cut ? cut : 1);
            memcpy(t, orig, cut);
            png_tile_decode(&c, t, cut, out);
            free(t);
        }
        for (int k = 0; k < 300; k++) {
            uint8_t *t = malloc(n);
            memcpy(t, orig, n);
            for (int j = 0; j < 4; j++) t[rand() % n] = (uint8_t)rand();
            png_tile_decode(&c, t, n, out);
            free(t);
        }
        free(orig);
        free(png);
        free(exp);
    }
    /* pas un PNG */
    uint8_t junk[64] = "hello";
    CHECK(png_tile_decode(&c, junk, sizeof junk, out) == PNG_E_FORMAT);
    free(c.raw);
    if (fails) { printf("png : %d échec(s)\n", fails); return 1; }
    printf("png : OK\n");
    return 0;
}
