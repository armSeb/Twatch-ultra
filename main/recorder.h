/*
 * recorder.h — magnétophone : micro PDM (CLK 17, DATA 18, sur I2S0) vers
 * fichiers WAV sur la carte SD (/sd/REC), et relecture sur le haut-parleur.
 *
 * Format : WAV PCM 16 bits mono, 32 kHz (qualité haute, par défaut) ou
 * 16 kHz (voix, deux fois moins de place). Pas de compression : aucun
 * calcul, donc CPU au plus bas, et un fichier lisible partout.
 *
 * L'enregistrement tourne dans deux tâches, indépendantes de l'écran :
 *   - « rec » lit le micro (DMA I2S), filtre le continu, règle le niveau
 *     (gain automatique) et remplit une réserve en PSRAM (≈ 8 s) ;
 *   - « recw » vide la réserve sur la carte par blocs de 64 Ko, alignés sur
 *     les secteurs (en-tête WAV de 512 octets), et met l'en-tête à jour
 *     toutes les 30 s : une coupure ne perd que les dernières secondes.
 * Le light sleep est suspendu le temps de l'enregistrement (l'horloge I2S
 * doit tourner) ; le CPU reste au plus bas.
 *
 * Fonctions appelables depuis la tâche app (UI). L'état se lit par copie.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define REC_DIR "/sd/REC"

typedef enum {
    REC_IDLE,
    REC_RECORDING,
    REC_STOPPING,       /* fin d'écriture en cours */
} rec_state_t;

typedef enum {
    REC_OK,
    REC_ERR_NO_SD,      /* pas de carte montée */
    REC_ERR_FULL,       /* carte pleine : enregistrement arrêté et gardé */
    REC_ERR_SD,         /* carte retirée ou erreur d'écriture : gardé jusque-là */
    REC_ERR_MIC,        /* micro (I2S) indisponible */
    REC_ERR_MEM,        /* mémoire */
    REC_ERR_BUSY,       /* son en cours (radio…) */
} rec_err_t;

typedef struct {
    rec_state_t state;
    rec_err_t   err;            /* dernière erreur (REC_OK après un arrêt normal) */
    uint32_t    rate;           /* Hz */
    uint32_t    seconds;        /* durée enregistrée */
    uint8_t     level;          /* niveau de crête 0..100 (après gain) */
    uint8_t     gain_db;        /* gain automatique appliqué */
    uint32_t    left_s;         /* place restante sur la carte, en secondes */
    char        name[40];       /* fichier en cours (ou dernier), sans dossier */
} rec_status_t;

esp_err_t rec_start(bool hq);   /* hq : 32 kHz, sinon 16 kHz */
void      rec_stop(void);       /* non bloquant : l'état passe par REC_STOPPING */
void      rec_get(rec_status_t *out);
bool      rec_active(void);     /* enregistrement ou fin d'écriture en cours */
/* secondes d'enregistrement que tient la carte à ce débit (0 : pas de carte) */
uint32_t  rec_capacity_s(bool hq);

/* ---- fichiers ---- */
typedef struct {
    char     name[40];          /* nom dans REC_DIR */
    uint32_t bytes;
    uint32_t seconds;           /* d'après la taille et l'en-tête */
    uint32_t rate;
} rec_file_t;

/* liste les enregistrements, plus récents d'abord ; renvoie le nombre
 * (au plus max), -1 si la carte est absente */
int       rec_list(rec_file_t *out, int max);
esp_err_t rec_delete(const char *name);

/* ---- relecture (haut-parleur, volume général) ---- */
esp_err_t rec_play(const char *name);
void      rec_play_stop(void);
void      rec_play_pause(bool pause);
bool      rec_playing(void);    /* tâche de lecture en marche (même en pause) */
bool      rec_paused(void);
/* position et durée de la lecture en cours, en secondes */
void      rec_play_pos(uint32_t *pos_s, uint32_t *len_s);
/* saut relatif (±s) pendant la lecture */
void      rec_play_seek(int delta_s);
