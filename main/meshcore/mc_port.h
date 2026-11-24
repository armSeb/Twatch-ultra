/*
 * mc_port.h — MeshCore (companion_radio d'origine, v1.18) sur la montre.
 *
 * Une tâche « mesh » (cœur 0) fait tourner le code MeshCore : radio SX1262,
 * contacts, canaux, chiffrement, et le protocole compagnon que parle l'app
 * MeshCore de l'iPhone par le service Bluetooth NUS (ble.c).
 *
 * Messages : MeshCore garde chaque message reçu dans sa file « hors ligne »
 * jusqu'à ce que l'app vienne le chercher. La montre n'y touche jamais :
 * elle a sa propre copie (mc_history_*). Un message lu au poignet arrive
 * donc quand même dans l'app. La file est sauvegardée en flash.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mc_start(void);      /* monte /mc, charge l'identité, allume la radio */
void      mc_stop(void);       /* sauve, coupe la radio (l'état reste en RAM) */
bool      mc_running(void);

/* ---- côté Bluetooth (ble.c, tâche hôte NimBLE) ---- */
#define MC_FRAME_MAX 176
void mc_ble_link(bool up);                      /* l'app s'abonne / se désabonne */
bool mc_ble_rx(const uint8_t *buf, size_t len); /* écriture sur la caractéristique RX */
/* fourni par ble.c : notification sur la caractéristique TX */
bool ble_mc_notify(const uint8_t *buf, size_t len);

/* ---- côté TCP (mc_tcp.c) : app MeshCore en TCP/IP ou meshterm ----
 * Serveur lancé par netsvc.c (réglage, Wi-Fi connecté, MeshCore actif). */
#define MC_TCP_PORT 5000
esp_err_t mc_tcp_start(void);
void      mc_tcp_stop(void);
bool      mc_tcp_running(void);
bool      mc_tcp_client(char ip[16]);           /* une app est branchée (son adresse) */
bool      mc_tcp_send(const uint8_t *buf, size_t len);   /* tâche mesh */
void      mc_tcp_link(bool up);                 /* mc_port.cpp : app branchée / partie */
bool      mc_tcp_rx(const uint8_t *buf, size_t len);     /* mc_port.cpp : trame reçue */

/* ---- historique de la montre ----
 * Un anneau des MC_HIST_N derniers messages (reçus, et envoyés depuis l'app)
 * vit en PSRAM, sauvegardé sur la FAT interne. Avec une carte SD, chaque
 * message y est aussi archivé (un fichier par conversation) : les plus
 * anciens se relisent depuis la carte.
 *
 * Lu / non lu : ouvrir une conversation sur la montre la marque lue. Un
 * message que l'app MeshCore a récupéré (CMD_SYNC_NEXT_MESSAGE) ou une
 * conversation où l'app vient d'écrire passent en lu sur la montre. */
#define MC_HIST_N   128
#define MC_NAME_MAX 32
#define MC_TEXT_MAX 160
#define MC_PEER_LEN 6

enum { MC_CONV_CONTACT = 0, MC_CONV_CHANNEL = 1 };
#define MC_F_READ  0x01           /* lu (montre ou app) */
#define MC_F_OUT   0x02           /* envoyé par nous (depuis l'app) */
#define MC_F_APP   0x04           /* récupéré par l'app */
#define MC_F_LEGACY 0x08          /* repris de l'historique 0.2.13 : conversation retrouvée par son nom */

typedef struct mc_msg_s {
    uint32_t seq;                 /* croissant, jamais réutilisé */
    uint32_t ts;                  /* horodatage de l'expéditeur (UTC) */
    uint32_t qkey;                /* empreinte de la trame donnée à l'app (0 : aucune) */
    uint8_t  kind;                /* MC_CONV_* */
    uint8_t  flags;               /* MC_F_* */
    int8_t   snr_q4;
    uint8_t  hops;                /* 0xFF : direct */
    uint8_t  peer[MC_PEER_LEN];   /* contact : début de sa clé ; canal : empreinte du secret */
    char     from[MC_NAME_MAX];   /* nom du contact ou du canal */
    char     text[MC_TEXT_MAX + 1];   /* UTF-8 ; « nom: texte » sur un canal */
} mc_msg_t;

/* Octet path_len d'un paquet MeshCore : les 2 bits du haut donnent la taille
 * des empreintes de relais (0 : 1 octet, 1 : 2 octets, 2 : 3 octets), les 6
 * du bas le nombre de sauts. Un message à 2 sauts en empreintes de 2 octets
 * porte donc 0x42 (66). 0xFF (direct) est gardé tel quel. Sert aussi à
 * corriger les historiques écrits avant 0.2.27, où l'octet brut était rangé. */
static inline uint8_t mc_hops_of(uint8_t path_len)
{
    return path_len == 0xFF ? 0xFF : (uint8_t)(path_len & 63);
}

int  mc_history_count(void);
bool mc_history_get(int i, mc_msg_t *out);      /* 0 = le plus récent */
uint32_t mc_history_last_seq(void);             /* change à chaque message ou changement lu/non lu */
/* appelé (tâche mesh) à chaque message reçu ; à brancher sur app_post */
typedef void (*mc_msg_cb_t)(const mc_msg_t *m);
void mc_set_msg_cb(mc_msg_cb_t cb);

/* ---- conversations (onglets Contacts et Canaux) ---- */
typedef struct {
    uint8_t  kind;
    uint8_t  peer[MC_PEER_LEN];
    char     name[MC_NAME_MAX];
    uint16_t unread;
    uint32_t last_ts;             /* 0 : aucun message dans l'anneau */
    bool     last_out;            /* le dernier message est de nous */
    char     last[64];            /* aperçu du dernier message */
} mc_conv_t;
/* Liste triée : non lus d'abord, puis ordre alphabétique (émojis et
 * ponctuation de tête ignorés, sans casse ni accents). Contacts : chat et
 * salons ; plus ceux de l'historique supprimés depuis. Renvoie le nombre total
 * (peut dépasser max). La liste des contacts et canaux est recopiée par la
 * tâche mesh après mc_conv_refresh() (rappel mc_set_msg_cb avec NULL). */
int  mc_conv_list(uint8_t kind, mc_conv_t *out, int max);
void mc_conv_refresh(void);
int  mc_unread_total(uint8_t kind);
void mc_conv_mark_read(uint8_t kind, const uint8_t peer[MC_PEER_LEN]);
/* messages d'une conversation, du plus récent au plus ancien, de seq
 * < before (UINT32_MAX : les derniers) : d'abord l'anneau, puis la carte SD */
int  mc_conv_messages(uint8_t kind, const uint8_t peer[MC_PEER_LEN], uint32_t before, mc_msg_t *out, int max);

/* état courant (diagnostic, écran Réglages) */
typedef struct {
    char     name[MC_NAME_MAX];
    float    freq_mhz, bw_khz;
    uint8_t  sf, cr;
    int8_t   tx_dbm;
    int      contacts;
    int      app_queue;           /* messages pas encore récupérés par l'app */
    bool     app_connected;       /* par Bluetooth ou TCP */
    bool     tcp_connected;
} mc_status_t;
void mc_get_status(mc_status_t *out);

#ifdef __cplusplus
}
#endif
