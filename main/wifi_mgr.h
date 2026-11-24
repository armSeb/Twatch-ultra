/*
 * wifi_mgr.h — Wi-Fi de la montre, géré en un seul endroit.
 *
 * Le pilote (mode station) n'existe que tant que quelqu'un le tient :
 *   - l'utilisateur (interrupteur du centre de contrôle et des réglages,
 *     retenu en NVS) ;
 *   - une app qui en a besoin (WebRX, Radio) : si le Wi-Fi est déjà là, elle
 *     utilise la connexion telle quelle ; sinon elle l'allume et le rend en
 *     partant (il ne s'éteint que si plus personne ne le tient) ;
 *   - l'analyseur Wi-Fi, qui n'a besoin que de la radio (pas de connexion).
 *
 * Une tâche « wifi » (pile en PSRAM) cherche les réseaux connus (page web de
 * la montre, jusqu'à WRX_NETS), s'y connecte dans l'ordre de préférence (ou au
 * seul réseau choisi dans les réglages), se reconnecte après une coupure et
 * espace ses essais en cas d'échec. Chaque changement d'état est posté à la
 * tâche app (EV_WIFI) : centre de contrôle, réglages, services réseau
 * (serveur web, serveur TCP MeshCore).
 *
 * Bluetooth : les deux radios cohabitent (coexistence logicielle, économie
 * du modem Wi-Fi). Si le pilote Wi-Fi ne trouve pas assez de RAM interne à
 * côté du Bluetooth, seule une app qui l'accepte (WIFI_ACQ_PAUSE_BLE) le
 * suspend le temps de son écoute ; l'interrupteur global ne coupe jamais le
 * Bluetooth (échec « mémoire insuffisante »).
 *
 * wifi_acquire / wifi_release / wifi_user_* / wifi_load_cfg : tâche app
 * uniquement. Les autres fonctions : n'importe quelle tâche.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    WIFI_OFF = 0,        /* pilote arrêté */
    WIFI_IDLE,           /* radio prête, aucune connexion demandée (analyseur) */
    WIFI_CONNECTING,     /* recherche / connexion en cours */
    WIFI_CONNECTED,      /* adresse IP obtenue */
    WIFI_FAILED,         /* dernier essai raté ; nouvel essai programmé */
} wifi_state_t;

/* qui tient le Wi-Fi (masque) */
enum {
    WIFI_HOLD_USER  = 1u << 0,
    WIFI_HOLD_WEBRX = 1u << 1,
    WIFI_HOLD_RADIO = 1u << 2,
    WIFI_HOLD_SCAN  = 1u << 3,       /* radio seule, sans connexion */
};
#define WIFI_HOLD_CONNECT (WIFI_HOLD_USER | WIFI_HOLD_WEBRX | WIFI_HOLD_RADIO)

/* options de wifi_acquire */
#define WIFI_ACQ_PAUSE_BLE 0x01      /* peut suspendre le Bluetooth faute de RAM interne */
#define WIFI_ACQ_PERF      0x02      /* pas d'économie du modem (flux continu) */

/* demandes de faible latence (pas d'économie du modem), voir wifi_perf() */
enum { WIFI_PERF_MCTCP = 1u << 0, WIFI_PERF_WEB = 1u << 1 };

/* codes EV_WIFI (champ a) */
enum { WIFI_EV_STATE = 0, WIFI_EV_RELOAD = 1 };

typedef struct {
    wifi_state_t state;
    const char  *msg;            /* étape ou cause d'échec, N_() ; NULL si aucune */
    char         detail[40];     /* complément (réseau, « raison 15 ») */
    char         ssid[33];       /* réseau connecté (ou essayé) */
    char         ip[16];         /* "" si pas d'adresse */
    int          rssi;           /* dBm, 0 = inconnu */
    uint32_t     gen;            /* +1 à chaque connexion réussie */
    uint32_t     holders;        /* WIFI_HOLD_* */
    bool         ble_paused;     /* Bluetooth suspendu faute de RAM */
    bool         no_mem;         /* le pilote n'a pas pu démarrer (RAM) */
    int64_t      retry_at_us;    /* FAILED : prochain essai (esp_timer), 0 = aucun prévu */
} wifi_status_t;

void      wifi_mgr_init(void);                       /* au démarrage (tâche app) */
esp_err_t wifi_acquire(uint32_t holder, uint32_t flags);
/* Bluetooth suspendu pour le débit (option wifi_noble, mise à jour web) :
 * réévalue la règle. Tâche app. */
void      wifi_ble_policy(void);
void      wifi_web_ota(bool on);        /* une image arrive par le serveur web */
void      wifi_release(uint32_t holder);
bool      wifi_held(uint32_t holder);
/* interrupteur global : retenu dans les réglages ; false = refus (RAM…) */
bool      wifi_user_set(bool on);
bool      wifi_user_on(void);
/* tâche app : relit les réseaux connus (NVS) et le réseau choisi, puis se
 * reconnecte si besoin. Ailleurs : wifi_request_reload() */
void      wifi_load_cfg(void);
void      wifi_request_reload(void);
/* batterie (tâche app, à chaque mesure) : coupe le Wi-Fi global quand elle
 * passe sous 30 % hors USB (réglage « Coupure sous 30 % ») ; true = coupé */
bool      wifi_battery_check(int pct, bool vbus);
/* avant le point d'accès de la mise à jour : tout est arrêté et plus rien ne
 * redémarre (la session finit par un redémarrage) */
void      wifi_mgr_shutdown(void);

void      wifi_get_status(wifi_status_t *out);
bool      wifi_is_up(void);
uint32_t  wifi_gen(void);
bool      wifi_lost(uint32_t gen);                   /* plus connecté depuis la connexion gen */
int       wifi_rssi(void);
bool      wifi_ip(char out[16]);
void      wifi_retry(void);                          /* nouvel essai tout de suite */
void      wifi_refresh(void);                        /* relit le signal (écran Wi-Fi affiché) */
void      wifi_apply_power(void);                    /* réglage « puissance Wi-Fi » */
int8_t    wifi_power_qdbm(void);                     /* même réglage, en quarts de dBm */
/* tâche app : réseau choisi (0 = automatique, k = réseau k), retenu */
void      wifi_set_pick(uint8_t pick);
bool      wifi_ble_paused(void);
/* Bluetooth démarré ou arrêté (tâche app) : pas de light sleep tant que les
 * deux radios tournent (balises Wi-Fi manquées sinon) */
void      wifi_ble_changed(void);
void      wifi_perf(uint32_t who, bool on);          /* WIFI_PERF_* */

/* analyseur (WIFI_HOLD_SCAN, tâche app) : radio coupée entre deux relèves
 * quand il est seul à tenir le Wi-Fi ; sans effet sinon */
void      wifi_scan_radio(bool on);

/* tâches réseau des apps : attend la connexion en relayant les étapes à cb ;
 * false si l'essai échoue (cb appelé avec fail = true) ou si *quit passe à
 * true. Un échec précédent relance un essai tout de suite. */
typedef void (*wifi_status_cb)(bool fail, const char *msg, const char *detail);
bool      wifi_wait_up(volatile bool *quit, wifi_status_cb cb);
