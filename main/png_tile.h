/*
 * png_tile.h — tuile de carte PNG → image RGB565 de 256 × 256.
 *
 * Pur C, sans matériel (testé sur PC : host_tests/test_png.c). Lit les PNG
 * des tuiles OpenStreetMap et des outils qui les rangent sur la carte SD :
 * palette (1, 2, 4, 8 bits), gris, RGB, avec ou sans alpha, 8 ou 16 bits,
 * non entrelacés. Une tuile de 256 × 256 est copiée telle quelle, une de
 * 512 × 512 (écran « retina ») réduite de moitié (moyenne 2 × 2). La
 * transparence est posée sur le fond des cartes OSM (PNG_TILE_BG).
 *
 * La décompression (zlib) est fournie par l'appelant : tinfl de la ROM de
 * l'ESP32-S3 sur la montre, zlib sur PC.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define PNG_TILE      256
#define PNG_TILE_BG   0xF2EFE9          /* fond des tuiles OSM, 0xRRGGBB */

/* décompresse un flux zlib complet ; renvoie le nombre d'octets écrits
 * (doit valoir out_len), < 0 en cas d'erreur */
typedef long (*png_inflate_fn)(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len, void *ctx);

typedef struct {
    png_inflate_fn inflate;
    void          *inflate_ctx;
    /* tampon des lignes décompressées, agrandi au besoin (realloc) */
    void *(*realloc_fn)(void *p, size_t n);
    uint8_t       *raw;
    size_t         raw_cap;
} png_tile_ctx_t;

enum {
    PNG_OK = 0,
    PNG_E_FORMAT = -1,      /* pas un PNG, ou abîmé */
    PNG_E_UNSUPPORTED = -2, /* entrelacé, taille autre que 256 ou 512 */
    PNG_E_MEMORY = -3,
    PNG_E_INFLATE = -4,
};

/* png est modifié (les blocs IDAT sont regroupés sur place). out : 256 × 256
 * pixels RGB565 natifs (R en bits de poids fort). */
int png_tile_decode(png_tile_ctx_t *c, uint8_t *png, size_t len, uint16_t *out);
