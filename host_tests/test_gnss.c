/* Tests PC du lecteur NMEA (main/gnss_nmea.c) : trames d'un u-blox M10
 * (NMEA 4.11), sommes de contrôle, coupures, bruit. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "gnss_nmea.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("ÉCHEC %s:%d : %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* corps sans $ ni *hh → trame complète avec CR LF */
static const char *mk(const char *body)
{
    static char out[160];
    unsigned cs = 0;
    for (const char *c = body; *c; c++) cs ^= (unsigned char)*c;
    snprintf(out, sizeof out, "$%s*%02X\r\n", body, cs);
    return out;
}

static void feed(nmea_t *p, const char *body)
{
    const char *s = mk(body);
    nmea_feed(p, (const uint8_t *)s, strlen(s));
}

static void test_no_fix(void)
{
    nmea_t p;
    nmea_init(&p);
    feed(&p, "GNRMC,,V,,,,,,,,,,N,V");
    feed(&p, "GNGGA,,,,,,0,00,99.99,,,,,,");
    feed(&p, "GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99,1");
    feed(&p, "GPGSV,1,1,00,1");
    CHECK(p.st.sentences == 4);
    CHECK(p.st.fix == 0);
    CHECK(p.st.utc == 0);
    CHECK(p.st.nsat == 0);
}

static void epoch(nmea_t *p)
{
    feed(p, "GNRMC,083559.00,A,4851.12345,N,00221.54321,E,0.5,45.0,261126,,,A,V");
    feed(p, "GNGGA,083559.00,4851.12345,N,00221.54321,E,1,09,0.92,35.4,M,46.9,M,,");
    feed(p, "GNGSA,A,3,05,13,15,18,,,,,,,,,1.65,0.92,1.37,1");
    feed(p, "GNGSA,A,3,07,30,,,,,,,,,,,1.65,0.92,1.37,3");
    feed(p, "GNGSA,A,3,14,,,,,,,,,,,,1.65,0.92,1.37,4");
    feed(p, "GPGSV,2,1,06,05,40,083,46,13,55,160,43,15,20,240,38,18,70,300,44,1");
    feed(p, "GPGSV,2,2,06,24,05,020,,44,30,190,40,1");          /* 44 : SBAS */
    feed(p, "GAGSV,1,1,03,07,45,100,41,30,60,200,39,12,10,330,,7");
    feed(p, "GBGSV,1,1,01,14,35,120,36,1");
    feed(p, "GLGSV,1,1,00,1");
}

static void test_fix(void)
{
    nmea_t p;
    nmea_init(&p);
    epoch(&p);
    epoch(&p);                                      /* GSA validés par la trame suivante */
    feed(&p, "GNRMC,083600.00,A,4851.12345,N,00221.54321,E,0.5,45.0,261126,,,A,V");
    CHECK(p.st.bad == 0);
    CHECK(p.st.fix == 3);
    CHECK(fabs(p.st.lat - (48 + 51.12345 / 60)) < 1e-9);
    CHECK(fabs(p.st.lon - (2 + 21.54321 / 60)) < 1e-9);
    CHECK(p.st.alt_ok && fabs(p.st.alt_m - 35.4f) < 0.01f);
    CHECK(p.st.sats_used == 9);
    CHECK(fabs(p.st.hdop - 0.92f) < 0.001f);
    CHECK(fabs(p.st.speed_kmh - 0.926f) < 0.001f);
    /* 2026-11-26 08:36:00 UTC */
    CHECK(p.st.utc == 1795682160);
    CHECK(p.st.nsat == 10);
    CHECK(p.st.view[GNSS_GPS] == 5 && p.st.view[GNSS_SBAS] == 1);
    CHECK(p.st.view[GNSS_GALILEO] == 3 && p.st.view[GNSS_BEIDOU] == 1);
    CHECK(p.st.used[GNSS_GPS] == 4 && p.st.used[GNSS_GALILEO] == 2 && p.st.used[GNSS_BEIDOU] == 1);
    CHECK(p.st.used[GNSS_SBAS] == 0 && p.st.used[GNSS_GLONASS] == 0);
    int snr24 = -1;
    for (int i = 0; i < p.st.nsat; i++) if (p.st.sat[i].sys == GNSS_GPS && p.st.sat[i].prn == 24) snr24 = p.st.sat[i].snr;
    CHECK(snr24 == 0);
}

static void test_hemispheres_and_loss(void)
{
    nmea_t p;
    nmea_init(&p);
    feed(&p, "GNGGA,120000.00,3352.00000,S,15112.60000,W,1,05,1.5,10.0,M,0,M,,");
    CHECK(p.st.fix == 3);
    CHECK(fabs(p.st.lat + (33 + 52.0 / 60)) < 1e-9 && fabs(p.st.lon + (151 + 12.6 / 60)) < 1e-9);
    feed(&p, "GNGGA,120001.00,,,,,0,00,99.99,,,,,,");
    CHECK(p.st.fix == 0);
    /* coordonnées absurdes : ignorées */
    feed(&p, "GNGGA,120002.00,9500.00000,N,00000.00000,E,1,05,1.5,10.0,M,0,M,,");
    CHECK(p.st.lat < 0);
}

static void test_garbage(void)
{
    nmea_t p;
    nmea_init(&p);
    const char *bad = "$GNGGA,120000.00,4851.1,N,00221.5,E,1,05,1.5,10.0,M,0,M,,*00\r\n";
    nmea_feed(&p, (const uint8_t *)bad, strlen(bad));
    CHECK(p.st.sentences == 0 && p.st.bad == 1);
    /* trame coupée (octets perdus en veille), puis une bonne */
    const char *cut = "$GPGSV,2,1,06,05,40,08";
    nmea_feed(&p, (const uint8_t *)cut, strlen(cut));
    feed(&p, "GPGSV,2,2,06,24,05,020,,44,30,190,40,1");       /* 2/2 sans 1/2 : ignorée */
    CHECK(p.st.nsat == 0);
    uint8_t noise[300];
    for (int i = 0; i < (int)sizeof noise; i++) noise[i] = (uint8_t)(i * 37 + 11);
    nmea_feed(&p, noise, sizeof noise);
    char longline[200];
    memset(longline, 'A', sizeof longline);
    longline[0] = '$';
    nmea_feed(&p, (const uint8_t *)longline, sizeof longline);
    feed(&p, "GNGGA,120000.00,4851.10000,N,00221.50000,E,1,05,1.5,10.0,M,0,M,,");
    CHECK(p.st.fix == 3);
    /* UBX binaire mêlé au flux (réponse ACK) */
    const uint8_t ack[] = { 0xB5, 0x62, 0x05, 0x01, 0x02, 0x00, 0x06, 0x8A, 0x98, 0xC1 };
    nmea_feed(&p, ack, sizeof ack);
    feed(&p, "GNRMC,083559.00,A,4851.12345,N,00221.54321,E,0.5,45.0,261126,,,A,V");
    CHECK(p.st.utc != 0);
}

static void test_days(void)
{
    CHECK(gnss_days_from_civil(1970, 1, 1) == 0);
    CHECK(gnss_days_from_civil(2000, 3, 1) == 11017);
    CHECK(gnss_days_from_civil(2026, 11, 26) == 20783);
}

int main(void)
{
    test_no_fix();
    test_fix();
    test_hemispheres_and_loss();
    test_garbage();
    test_days();
    printf("GNSS : %s\n", fails ? "ÉCHEC" : "OK");
    return fails ? 1 : 0;
}
