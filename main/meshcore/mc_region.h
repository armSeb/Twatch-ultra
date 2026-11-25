/*
 * mc_region.h — régions MeshCore gérées sur la montre.
 *
 * Une région (« scope ») restreint l'inondation d'un message aux répéteurs
 * qui la connaissent. Son nom s'écrit sans « # » (« bzh », « fr-ouest ») ;
 * sa clé est le SHA-256 de « #nom », 16 premiers octets, comme sur les
 * répéteurs (RegionMap). Les régions privées (« $… ») ne sont pas gérées.
 *
 * Le nœud compagnon ne garde que sa région par défaut ; la liste et le choix
 * par canal vivent dans l'app iPhone, qui ne les transmet pas. La montre a
 * donc les siens, en NVS : une liste de régions (ajoutées à la main ou
 * découvertes auprès des répéteurs) et, pour chaque canal, la région de ses
 * envois. Sans choix : la région par défaut du nœud.
 *
 * Tâche app et tâche mesh (verrou interne).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "mc_port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MC_REGION_MAX     16
#define MC_REGION_NAME    31       /* octets, zéro final compris (comme RegionEntry) */

void mc_region_init(void);         /* lecture NVS (idempotent) */

/* liste des régions, ordre alphabétique ; renvoie leur nombre */
int  mc_region_list(char out[][MC_REGION_NAME], int max);
/* nom nettoyé (espaces, « # » de tête) ; false : vide, privé, trop long,
 * liste pleine. *added : nouvelle (false : déjà là) */
bool mc_region_add(const char *name, bool *added);
void mc_region_remove(const char *name);    /* les canaux qui l'utilisaient reprennent le défaut */

/* région d'un canal ; false : défaut du nœud */
bool mc_region_of(const uint8_t peer[MC_PEER_LEN], char out[MC_REGION_NAME]);
void mc_region_set(const uint8_t peer[MC_PEER_LEN], const char *name);   /* NULL : défaut */
/* tâche mesh : clé de la région du canal ; false : défaut du nœud */
bool mc_region_key_of(const uint8_t peer[MC_PEER_LEN], uint8_t key[16]);

/* découverte (mc_port.cpp) : requête « régions » aux répéteurs joignables
 * en direct (chemin connu du contact, ou chemin de sa dernière annonce
 * entendue) ; les noms reçus entrent dans la liste */
typedef struct {
    bool    running;
    bool    ran;                   /* lancée au moins une fois */
    uint8_t asked, replied;        /* répéteurs interrogés, réponses */
    uint8_t found;                 /* régions nouvelles dans la liste */
    uint8_t candidates;            /* répéteurs dans les contacts */
    uint8_t reachable;             /* dont joignables en direct (interrogés, 12 au plus) */
    char    names[96];             /* régions reçues, « a, b, c » */
} mc_region_disc_t;
bool mc_region_discover(void);     /* false : MeshCore coupé, ou déjà en cours */
void mc_region_disc_status(mc_region_disc_t *out);

#ifdef __cplusplus
}
#endif
