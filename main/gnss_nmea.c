/* gnss_nmea.c — voir gnss_nmea.h */
#include "gnss_nmea.h"
#include <stdlib.h>
#include <string.h>

/* groupes GSV/GSA (deux lettres après le $) ; id système NMEA 4.10 = index + 1 */
enum { T_GP = 0, T_GL, T_GA, T_GB, T_GQ, T_GI, T_GN, T_N };

#define MAXF 24

static int talker_of(const char *s)
{
    if (s[0] == 'G') {
        switch (s[1]) {
        case 'P': return T_GP;
        case 'L': return T_GL;
        case 'A': return T_GA;
        case 'B': return T_GB;
        case 'Q': return T_GQ;
        case 'I': return T_GI;
        case 'N': return T_GN;
        }
    }
    if (s[0] == 'B' && s[1] == 'D') return T_GB;
    if (s[0] == 'Q' && s[1] == 'Z') return T_GQ;
    return -1;
}

static uint8_t sys_of(int talker, int prn)
{
    switch (talker) {
    case T_GL: return GNSS_GLONASS;
    case T_GA: return GNSS_GALILEO;
    case T_GB: return GNSS_BEIDOU;
    case T_GQ: return GNSS_QZSS;
    case T_GI: return GNSS_NAVIC;
    default:
        if (prn >= 33 && prn <= 64) return GNSS_SBAS;
        if (prn >= 193 && prn <= 202) return GNSS_QZSS;
        if (prn >= 65 && prn <= 96) return GNSS_GLONASS;
        return GNSS_GPS;
    }
}

const char *gnss_sys_name(int sys)
{
    static const char *const N[GNSS_SYS_N] = { "GPS", "Galileo", "GLONASS", "BeiDou", "QZSS", "NavIC", "SBAS" };
    return sys >= 0 && sys < GNSS_SYS_N ? N[sys] : "?";
}

int64_t gnss_days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

void nmea_init(nmea_t *p)
{
    memset(p, 0, sizeof *p);
    p->st.course_deg = -1;
}

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* entier d'un champ ; def si vide ou invalide */
static int fint(const char *s, int def)
{
    if (!s || !*s) return def;
    char *e;
    long v = strtol(s, &e, 10);
    if (*e && *e != '.') return def;
    if (v > 100000 || v < -100000) return def;
    return (int)v;
}

static bool fnum(const char *s, double *out)
{
    if (!s || !*s) return false;
    char *e;
    double v = strtod(s, &e);
    if (*e) return false;
    *out = v;
    return true;
}

/* ddmm.mmmm / dddmm.mmmm + hémisphère → degrés */
static bool fcoord(const char *v, const char *h, double lim, double *out)
{
    double x;
    if (!fnum(v, &x) || x < 0 || !h || !h[0] || h[1]) return false;
    double deg = (double)(int)(x / 100.0);
    double min = x - deg * 100.0;
    if (min >= 60.0) return false;
    deg += min / 60.0;
    if (h[0] == 'S' || h[0] == 'W') deg = -deg;
    else if (h[0] != 'N' && h[0] != 'E') return false;
    if (deg > lim || deg < -lim) return false;
    *out = deg;
    return true;
}

/* hhmmss(.ss) → secondes du jour */
static bool ftime(const char *s, int *sec)
{
    if (!s || strlen(s) < 6) return false;
    for (int i = 0; i < 6; i++) if (s[i] < '0' || s[i] > '9') return false;
    int hh = (s[0] - '0') * 10 + s[1] - '0', mm = (s[2] - '0') * 10 + s[3] - '0', ss = (s[4] - '0') * 10 + s[5] - '0';
    if (hh > 23 || mm > 59 || ss > 60) return false;
    *sec = hh * 3600 + mm * 60 + ss;
    return true;
}

static void recount(gnss_state_t *st, const nmea_t *p)
{
    memset(st->view, 0, sizeof st->view);
    memset(st->used, 0, sizeof st->used);
    for (int i = 0; i < st->nsat; i++) {
        gnss_sat_t *s = &st->sat[i];
        s->used = false;
        for (int k = 0; k < p->nused; k++)
            if (p->used[k].prn == s->prn && p->used[k].sysid == s->talker + 1) { s->used = true; break; }
        st->view[s->sys]++;
        if (s->used) st->used[s->sys]++;
    }
}

static void gsa_close(nmea_t *p)
{
    if (!p->gsa_open) return;
    p->gsa_open = false;
    memcpy(p->used, p->used_new, sizeof p->used);
    p->nused = p->nused_new;
    if (p->st.quality == 0 || p->gsa_fix < 2) p->st.fix = 0;
    else p->st.fix = p->gsa_fix;
    recount(&p->st, p);
}

static void on_rmc(nmea_t *p, char **f, int nf)
{
    if (nf < 10) return;
    p->st.epochs++;
    if (f[2][0] != 'A') return;
    double lat, lon;
    if (fcoord(f[3], f[4], 90, &lat) && fcoord(f[5], f[6], 180, &lon)) { p->st.lat = lat; p->st.lon = lon; }
    double v;
    if (fnum(f[7], &v) && v >= 0 && v < 2000) p->st.speed_kmh = (float)(v * 1.852);
    p->st.course_deg = fnum(f[8], &v) && v >= 0 && v <= 360 ? (float)v : -1;
    int sec;
    const char *d = f[9];
    if (ftime(f[1], &sec) && strlen(d) == 6) {
        int dd = fint((char[]){ d[0], d[1], 0 }, -1), mo = fint((char[]){ d[2], d[3], 0 }, -1),
            yy = fint((char[]){ d[4], d[5], 0 }, -1);
        if (dd >= 1 && dd <= 31 && mo >= 1 && mo <= 12 && yy >= 0)
            p->st.utc = gnss_days_from_civil(2000 + yy, (unsigned)mo, (unsigned)dd) * 86400 + sec;
    }
}

static void on_gga(nmea_t *p, char **f, int nf)
{
    if (nf < 10) return;
    int q = fint(f[6], 0);
    p->st.quality = (uint8_t)(q < 0 ? 0 : q > 9 ? 9 : q);
    int n = fint(f[7], 0);
    p->st.sats_used = (uint8_t)(n < 0 ? 0 : n > 99 ? 99 : n);
    double v;
    if (fnum(f[8], &v) && v > 0 && v < 100) p->st.hdop = (float)v;
    if (q == 0) { p->st.fix = 0; return; }
    double lat, lon;
    if (fcoord(f[2], f[3], 90, &lat) && fcoord(f[4], f[5], 180, &lon)) { p->st.lat = lat; p->st.lon = lon; }
    p->st.alt_ok = fnum(f[9], &v) && v > -1000 && v < 100000;
    if (p->st.alt_ok) p->st.alt_m = (float)v;
    /* pas de GSA (sortie coupée) : 3D si l'altitude est donnée */
    if (p->st.fix < 2 && p->gsa_fix == 0) p->st.fix = p->st.alt_ok ? 3 : 2;
}

static void on_gsa(nmea_t *p, int talker, char **f, int nf)
{
    if (nf < 18) return;
    if (!p->gsa_open) {
        p->gsa_open = true;
        p->nused_new = 0;
        p->gsa_fix = 0;
    }
    int ft = fint(f[2], 1);
    if (ft > p->gsa_fix && ft <= 3) p->gsa_fix = (uint8_t)ft;
    int sysid = nf >= 19 ? fint(f[18], 0) : 0;          /* NMEA 4.10 */
    double v;
    if (fnum(f[15], &v) && v > 0 && v < 100) p->st.pdop = (float)v;
    if (fnum(f[16], &v) && v > 0 && v < 100) p->st.hdop = (float)v;
    if (fnum(f[17], &v) && v > 0 && v < 100) p->st.vdop = (float)v;
    for (int i = 3; i < 15; i++) {
        int prn = fint(f[i], 0);
        if (prn <= 0 || prn > 255) continue;
        int sid = sysid;
        if (sid < 1 || sid > 6) {
            if (talker >= T_GP && talker <= T_GI) sid = talker + 1;
            else sid = prn >= 65 && prn <= 96 ? 2 : 1;  /* NMEA 4.0 : numéros de GLONASS */
        }
        if (p->nused_new < sizeof p->used_new / sizeof p->used_new[0]) {
            p->used_new[p->nused_new].sysid = (uint8_t)sid;
            p->used_new[p->nused_new].prn = (uint8_t)prn;
            p->nused_new++;
        }
    }
}

static void on_gsv(nmea_t *p, int talker, char **f, int nf)
{
    if (nf < 4 || talker < 0 || talker >= T_GN) return;
    int total = fint(f[1], 0), num = fint(f[2], 0);
    if (total < 1 || total > 9 || num < 1 || num > total) return;
    __typeof__(p->gsv[0]) *g = &p->gsv[talker];
    if (num == 1) { g->n = 0; g->total = (uint8_t)total; g->next = 1; }
    if (num != g->next || total != g->total) { g->next = 0; return; }   /* trou : on attend la suivante 1 */
    g->next++;
    for (int i = 4; i + 3 < nf; i += 4) {      /* un champ « signal » (4.10) peut suivre */
        int prn = fint(f[i], 0);
        if (prn <= 0 || prn > 255 || g->n >= sizeof g->s / sizeof g->s[0]) continue;
        gnss_sat_t *s = &g->s[g->n];
        bool dup = false;
        for (int k = 0; k < g->n; k++) if (g->s[k].prn == prn) dup = true;
        if (dup) continue;
        memset(s, 0, sizeof *s);
        s->prn = (uint8_t)prn;
        s->talker = (uint8_t)talker;
        s->sys = sys_of(talker, prn);
        int el = fint(f[i + 1], -1), az = fint(f[i + 2], -1), cn = fint(f[i + 3], 0);
        s->elev = (int8_t)(el >= 0 && el <= 90 ? el : -1);
        s->az = (uint16_t)(az >= 0 && az < 360 ? az : 0xFFFF);
        s->snr = (uint8_t)(cn > 0 && cn < 100 ? cn : 0);
        g->n++;
    }
    if (num != total) return;
    /* groupe complet : remplace les satellites de ce groupe */
    gnss_state_t *st = &p->st;
    int w = 0;
    for (int i = 0; i < st->nsat; i++)
        if (st->sat[i].talker != talker) st->sat[w++] = st->sat[i];
    for (int k = 0; k < g->n && w < GNSS_SAT_MAX; k++) st->sat[w++] = g->s[k];
    st->nsat = (uint8_t)w;
    g->next = 0;
    recount(st, p);
}

bool nmea_line(nmea_t *p, const char *line)
{
    size_t n = strlen(line);
    if (n < 9 || n >= sizeof p->line || line[0] != '$') { p->st.bad++; return false; }
    const char *star = strrchr(line, '*');
    if (!star || line + n - star != 3) { p->st.bad++; return false; }
    int h = hexv(star[1]), l = hexv(star[2]);
    uint8_t cs = 0;
    for (const char *c = line + 1; c < star; c++) cs ^= (uint8_t)*c;
    if (h < 0 || l < 0 || cs != (uint8_t)(h * 16 + l)) { p->st.bad++; return false; }

    char buf[sizeof p->line];
    size_t body = (size_t)(star - line - 1);
    memcpy(buf, line + 1, body);
    buf[body] = 0;
    char *f[MAXF];
    int nf = 0;
    char *s = buf;
    f[nf++] = s;
    for (; *s; s++) {
        if (*s == ',') {
            *s = 0;
            if (nf < MAXF) f[nf++] = s + 1;
        }
    }
    for (int i = nf; i < MAXF; i++) f[i] = s;          /* champs absents : chaîne vide */
    p->st.sentences++;
    if (strlen(f[0]) != 5) return true;
    int talker = talker_of(f[0]);
    const char *type = f[0] + 2;
    bool gsa = !strcmp(type, "GSA");
    if (!gsa) gsa_close(p);
    if (!strcmp(type, "RMC")) on_rmc(p, f, nf);
    else if (!strcmp(type, "GGA")) on_gga(p, f, nf);
    else if (gsa) on_gsa(p, talker, f, nf);
    else if (!strcmp(type, "GSV")) on_gsv(p, talker, f, nf);
    return true;
}

int nmea_feed(nmea_t *p, const uint8_t *buf, size_t n)
{
    int ok = 0;
    for (size_t i = 0; i < n; i++) {
        char c = (char)buf[i];
        if (c == '$') { p->in = true; p->len = 0; }
        if (!p->in) continue;
        if (c == '\r' || c == '\n') {
            p->line[p->len] = 0;
            p->in = false;
            if (p->len) ok += nmea_line(p, p->line);
            continue;
        }
        if (c < 0x20 || c > 0x7E || p->len >= sizeof p->line - 1) {   /* bruit, trame trop longue */
            p->in = false;
            p->st.bad++;
            continue;
        }
        p->line[p->len++] = c;
    }
    return ok;
}
