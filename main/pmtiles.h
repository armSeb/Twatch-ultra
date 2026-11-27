/*
 * pmtiles.h — lecture d'un fichier PMTiles v3 (toutes les tuiles d'une carte
 * dans un seul fichier, https://github.com/protomaps/PMTiles).
 *
 * Pur C, sans matériel (testé sur PC : host_tests/test_pmtiles.c, fichiers
 * écrits par le paquet Python pmtiles). Les répertoires (index) sont
 * décompressés en mémoire : la racine à l'ouverture, les feuilles à la
 * demande, les PM_LEAVES dernières gardées. Trouver une tuile ne demande
 * alors aucune lecture, ou une seule feuille de temps en temps.
 *
 * Entrées / sorties, décompression (deflate brut) et mémoire sont fournies
 * par l'appelant.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PM_LEAVES   4
#define PM_DIR_MAX  (4u << 20)          /* répertoire décompressé : 4 Mo au plus */

/* lit n octets à off ; 0 si tout a été lu */
typedef int  (*pm_read_fn)(void *ctx, uint64_t off, void *buf, size_t n);
/* deflate brut (sans en-tête zlib) ; renvoie les octets écrits, < 0 erreur */
typedef long (*pm_inflate_fn)(const uint8_t *in, size_t n, uint8_t *out, size_t out_len);

typedef struct {
    uint64_t tile_id, offset;
    uint32_t length, run;
} pm_entry_t;

typedef struct {
    pm_entry_t *e;
    uint32_t    n;
} pm_dir_t;

typedef struct {
    /* fournis par l'appelant */
    pm_read_fn    read;
    pm_inflate_fn inflate;
    void *(*alloc)(size_t n);
    void  (*free)(void *p);
    void         *io;                   /* contexte de read (fichier ouvert) */
    /* en-tête */
    uint64_t root_off, root_len, leaf_off, leaf_len, data_off, data_len;
    uint8_t  internal_comp, tile_comp, tile_type, min_zoom, max_zoom, center_zoom;
    int32_t  min_lon_e7, min_lat_e7, max_lon_e7, max_lat_e7, center_lon_e7, center_lat_e7;
    /* privé */
    pm_dir_t root;
    struct { uint64_t off; uint32_t len, used; pm_dir_t d; } leaf[PM_LEAVES];
    uint32_t clock;
} pm_t;

enum {
    PM_OK = 0,
    PM_E_IO = -1,
    PM_E_FORMAT = -2,          /* pas un PMTiles v3, ou abîmé */
    PM_E_UNSUPPORTED = -3,     /* compression des répertoires inconnue (brotli, zstd) */
    PM_E_MEMORY = -4,
};

#define PM_TYPE_PNG   2
#define PM_COMP_NONE  1
#define PM_COMP_GZIP  2

/* lit l'en-tête et le répertoire racine (read, inflate, alloc, free, io
 * remplis avant) */
int      pm_open(pm_t *p);
void     pm_close(pm_t *p);
/* position de la tuile dans le fichier : 1 trouvée, 0 absente, < 0 erreur */
int      pm_find(pm_t *p, int z, uint32_t x, uint32_t y, uint64_t *off, uint32_t *len);
uint64_t pm_tile_id(int z, uint32_t x, uint32_t y);
