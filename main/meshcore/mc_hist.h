/*
 * mc_hist.h — historique des messages de la montre (interne à MeshCore,
 * voir mc_port.h pour l'interface publique).
 *
 *   anneau (PSRAM, MC_HIST_N messages) ── 30 s de calme ──▶ /mc/hist (FAT interne)
 *        │
 *        └── toutes les minutes (tâche « sd ») ──▶ /sd/meshcore/<conv>.bin
 *
 * L'anneau garde les messages récents et leur état lu / non lu ; la carte
 * garde tout, un fichier par conversation, en enregistrements de 256 octets.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "mc_port.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t kind;
    uint8_t peer[MC_PEER_LEN];
    char    name[MC_NAME_MAX];
} mch_peer_t;

void mch_init(void);                    /* avant tout le reste */
void mch_load(void);                    /* /mc/hist (FAT interne montée) */
void mch_save_if_due(int64_t now_us);   /* tâche mesh */
void mch_save_now(void);

/* tâche mesh, depuis MyMesh */
void mch_frame_queued(const uint8_t *frame, int len);   /* trame ajoutée à la file de l'app */
void mch_frame_synced(const uint8_t *frame, int len);   /* trame récupérée par l'app */
/* ajoute un message ; recv : reçu (qkey et horodatage pris dans la dernière
 * trame mise en file), sinon envoyé depuis l'app */
void mch_add(mc_msg_t *m, bool recv);

/* contacts et canaux connus, recopiés par la tâche mesh sur demande */
bool mch_peers_wanted(void);
void mch_peers_set(const mch_peer_t *p, int n);
#define MCH_PEERS_MAX (MAX_CONTACTS + MAX_GROUP_CHANNELS)

#ifdef __cplusplus
}
#endif
