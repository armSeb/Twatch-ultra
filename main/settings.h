#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t brightness;       /* 10..255 */
    uint8_t screen_timeout_s; /* 5..60 */
    bool    vibrate;
    bool    ble_enabled;      /* false = mode avion (light sleep réel) */
    int16_t tz_offset_min;    /* fuseau + DST */
    uint8_t watchface;        /* 0 = classique, 1 = numérique, 2 = pub (carré) */
    bool    tap_wake;         /* double tap sur l'écran éteint = réveil */
    bool    notif_wake;       /* false = notif : vibre seulement, bouton pour voir */
    uint8_t lang;             /* 0 = français, 1 = anglais (i18n.h) */
    bool    dnd;              /* ne pas déranger : ni vibration ni allumage (sauf appels) */
    bool    step_count;       /* compteur de pas (BMA423) */
    bool    wrist_wake;       /* lever le poignet = allumer l'écran (BMA423) */
    uint8_t wrist_sens;       /* 0 basse, 1 moyenne, 2 haute (geste Bosch seul) */
    bool    glance;           /* poignet : écran tamisé et bref, toucher = réveil complet */
    uint8_t glance_pct;       /* luminosité du coup d'œil, % */
    uint8_t glance_s;         /* durée du coup d'œil, s */
    bool    meshcore;         /* radio LoRa MeshCore (app compagnon iPhone) */
    uint8_t wifi_pwr;         /* puissance Wi-Fi : 0 basse … 3 maximale (wifi_sta.c) */
    bool    lora_ext_ant;     /* antenne LoRa externe (USB-C) au lieu de l'intégrée */
    uint8_t volume;           /* volume général 0..100 (sound.c), en plus de celui des apps */
    bool    silent;           /* mode silencieux : aucun son (la vibration reste) */
    bool    wifi_on;          /* Wi-Fi global (centre de contrôle, réglages) */
    uint8_t wifi_net;         /* réseau choisi : 0 = automatique, k = réseau k (1..WRX_NETS) */
    bool    wifi_lowbatt;     /* coupe le Wi-Fi global sous 30 % de batterie */
    bool    wifi_noble;       /* Bluetooth suspendu tant que le Wi-Fi est allumé (débit) */
    bool    wifi_ap;          /* Wi-Fi global en point d'accès (montre en hotspot) au lieu de client */
    bool    mesh_tcp;         /* serveur TCP MeshCore (port 5000) quand le Wi-Fi est connecté */
    bool    web_on;           /* serveur web (réglages, mise à jour) */
    bool    web_sd;           /* fichiers de la carte SD sur le serveur web */
    uint8_t mesh_tries;       /* MeshCore, message privé : tentatives 1..5 (comme l'app iPhone) */
    uint8_t kb_mode;          /* clavier : 0 complet, 1 T9 (KB_MODE_*) */
    uint8_t kb_layout;        /* clavier complet : 0 AZERTY, 1 QWERTY */
    bool    gps_on;           /* GNSS allumé (gnss.c) ; coupé par défaut */
    bool    rec_hq;           /* magnétophone : 32 kHz (sinon 16 kHz, voix) */
} settings_t;

void        settings_load(void);
void        settings_save(void);
settings_t *settings_get(void);
