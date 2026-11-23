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

/* ---- copie montre des messages reçus ---- */
#define MC_NAME_MAX 32
#define MC_TEXT_MAX 160
typedef struct {
    uint32_t seq;                 /* croissant, jamais réutilisé */
    uint32_t ts;                  /* horodatage de l'expéditeur (UTC) */
    bool     channel;             /* message de canal */
    int8_t   snr_q4;
    uint8_t  hops;                /* 0xFF : direct */
    char     from[MC_NAME_MAX];   /* contact, ou nom du canal */
    char     text[MC_TEXT_MAX + 1];   /* UTF-8 ; « nom: texte » sur un canal */
} mc_msg_t;

int  mc_history_count(void);
bool mc_history_get(int i, mc_msg_t *out);      /* 0 = le plus récent */
uint32_t mc_history_last_seq(void);
/* appelé (tâche mesh) à chaque message reçu ; à brancher sur app_post */
typedef void (*mc_msg_cb_t)(const mc_msg_t *m);
void mc_set_msg_cb(mc_msg_cb_t cb);

/* état courant (diagnostic, écran Réglages) */
typedef struct {
    char     name[MC_NAME_MAX];
    float    freq_mhz, bw_khz;
    uint8_t  sf, cr;
    int8_t   tx_dbm;
    int      contacts;
    int      app_queue;           /* messages pas encore récupérés par l'app */
    bool     app_connected;
} mc_status_t;
void mc_get_status(mc_status_t *out);

#ifdef __cplusplus
}
#endif
