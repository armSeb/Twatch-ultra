/*
 * gnss_nmea.h — lecture des trames NMEA 0183 (4.10/4.11) du GNSS u-blox
 * MIA-M10Q : position, vitesse, précision, satellites par constellation.
 *
 * Pur C, sans matériel (testé sur PC : host_tests/test_gnss.c). On pousse
 * les octets reçus de l'UART avec nmea_feed() ; chaque trame dont la somme de
 * contrôle est bonne met à jour l'état. Trames lues : RMC, GGA, GSA, GSV.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* constellations (ordre d'affichage) */
enum {
    GNSS_GPS = 0,
    GNSS_GALILEO,
    GNSS_GLONASS,
    GNSS_BEIDOU,
    GNSS_QZSS,
    GNSS_NAVIC,
    GNSS_SBAS,
    GNSS_SYS_N,
};

#define GNSS_SAT_MAX 64

typedef struct {
    uint8_t  sys;        /* GNSS_* */
    uint8_t  prn;        /* numéro NMEA du satellite */
    int8_t   elev;       /* degrés, -1 inconnu */
    uint16_t az;         /* degrés, 0xFFFF inconnu */
    uint8_t  snr;        /* dB-Hz, 0 = pas suivi */
    bool     used;       /* entre dans la solution */
    uint8_t  talker;     /* interne : groupe GSV qui l'a décrit */
} gnss_sat_t;

typedef struct {
    /* position (valable si fix >= 2) */
    uint8_t  fix;        /* 0 aucune, 2 = 2D, 3 = 3D */
    uint8_t  quality;    /* GGA : 0 aucune, 1 autonome, 2 différentielle (SBAS)… */
    double   lat, lon;   /* degrés */
    float    alt_m;      /* altitude au-dessus du niveau moyen de la mer */
    bool     alt_ok;
    float    speed_kmh;
    float    course_deg; /* -1 inconnu */
    float    hdop, pdop, vdop;   /* 0 inconnu */
    uint8_t  sats_used;  /* GGA */
    /* heure UTC (RMC valide) */
    int64_t  utc;        /* secondes Unix, 0 inconnue */
    /* satellites vus (GSV) */
    uint8_t  nsat;
    gnss_sat_t sat[GNSS_SAT_MAX];
    uint8_t  view[GNSS_SYS_N];   /* vus, par constellation */
    uint8_t  used[GNSS_SYS_N];   /* utilisés, par constellation */
    /* compteurs */
    uint32_t sentences;  /* trames valides */
    uint32_t bad;        /* sommes de contrôle fausses, trames trop longues */
    uint32_t epochs;     /* une par RMC */
} gnss_state_t;

typedef struct {
    gnss_state_t st;
    /* privé */
    char     line[96];
    uint8_t  len;
    bool     in;
    bool     gsa_open;                   /* bloc de GSA en cours */
    uint8_t  gsa_fix;
    uint8_t  nused, nused_new;
    struct { uint8_t sysid, prn; } used[48], used_new[48];
    /* GSV en cours, par groupe (talker) */
    struct {
        uint8_t total, next, n;
        gnss_sat_t s[24];
    } gsv[8];
} nmea_t;

void nmea_init(nmea_t *p);
/* octets bruts de l'UART ; renvoie le nombre de trames valides lues */
int  nmea_feed(nmea_t *p, const uint8_t *buf, size_t n);
/* une trame complète, sans fin de ligne ($…*hh) ; true si valide et lue */
bool nmea_line(nmea_t *p, const char *line);
/* nom court d'une constellation */
const char *gnss_sys_name(int sys);
/* jours depuis 1970-01-01 (calendrier grégorien) */
int64_t gnss_days_from_civil(int y, unsigned m, unsigned d);
