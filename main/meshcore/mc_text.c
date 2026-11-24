/* mc_text.c — voir mc_text.h */
#include "mc_text.h"
#include <string.h>
#include "mc_port.h"

void mc_copy_utf8(char *dst, size_t cap, const char *src, size_t len)
{
    if (!cap) return;
    size_t lim = len < cap - 1 ? len : cap - 1, n = 0;
    while (n < lim && src[n]) n++;
    /* dernier caractère incomplet (coupé par la limite) : retiré */
    size_t k = n;
    while (k > 0 && ((uint8_t)src[k - 1] & 0xC0) == 0x80) k--;
    if (k > 0 && (uint8_t)src[k - 1] >= 0xC0) {
        uint8_t lead = (uint8_t)src[k - 1];
        size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
        if (n - (k - 1) < need) n = k - 1;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

uint32_t mc_fnv32(const void *p, size_t n)
{
    const uint8_t *b = p;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h ? h : 1;
}

void mc_channel_peer(const uint8_t *secret, size_t len, uint8_t out[6])
{
    uint32_t a = mc_fnv32(secret, len), b = mc_fnv32(&a, 4) ^ mc_fnv32(secret, len / 2);
    memcpy(out, &a, 4);
    memcpy(out + 4, &b, 2);
}

/* U+00C0..U+00FF → lettre de base (0 : garder tel quel) */
static const char LATIN1[64] =
    "aaaaaaaceeeeiiii" "dnooooo\0ouuuuy\0s"
    "aaaaaaaceeeeiiii" "dnooooo\0ouuuuy\0y";

void mc_sort_key(const char *name, char *key, size_t cap)
{
    if (!cap) return;
    const uint8_t *s = (const uint8_t *)name;
    /* tête : tout ce qui n'est ni lettre ni chiffre (émojis, #, espaces…) */
    const uint8_t *p = s;
    while (*p && !((*p < 0x80 && ((*p | 0x20) >= 'a' && (*p | 0x20) <= 'z')) ||
                   (*p >= '0' && *p <= '9') || *p == 0xC3 || *p == 0xC5)) p++;
    if (!*p) p = s;
    size_t k = 0;
    while (*p && k + 1 < cap) {
        uint8_t c = *p;
        if (c < 0x80) { key[k++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c); p++; continue; }
        if (c == 0xC3 && p[1] >= 0x80 && p[1] <= 0xBF) {
            char b = LATIN1[p[1] - 0x80];
            if (b) { key[k++] = b; p += 2; continue; }
        }
        if (c == 0xC5 && (p[1] == 0x92 || p[1] == 0x93)) {          /* Œ œ */
            key[k++] = 'o';
            if (k + 1 < cap) key[k++] = 'e';
            p += 2;
            continue;
        }
        key[k++] = (char)c;                                          /* autre : octet brut, après z */
        p++;
    }
    key[k] = 0;
}

/* codes de MyMesh.cpp */
#define R_CONTACT_V1 7
#define R_CHANNEL_V1 8
#define R_CONTACT_V3 16
#define R_CHANNEL_V3 17

bool mc_frame_info(const uint8_t *f, int len, uint8_t *kind, uint32_t *ts)
{
    int at;
    if (len < 1) return false;
    switch (f[0]) {
    case R_CONTACT_V1: *kind = MC_CONV_CONTACT; at = 1 + 6 + 2; break;
    case R_CONTACT_V3: *kind = MC_CONV_CONTACT; at = 4 + 6 + 2; break;
    case R_CHANNEL_V1: *kind = MC_CONV_CHANNEL; at = 1 + 1 + 2; break;
    case R_CHANNEL_V3: *kind = MC_CONV_CHANNEL; at = 4 + 1 + 2; break;
    default: return false;
    }
    if (len < at + 4) return false;
    memcpy(ts, f + at, 4);
    return true;
}

const char *mc_split_sender(const char *text, char *sender, size_t cap)
{
    if (cap) sender[0] = 0;
    const char *c = strstr(text, ": ");
    if (!c || c == text || (size_t)(c - text) >= MC_NAME_MAX) return text;
    mc_copy_utf8(sender, cap, text, (size_t)(c - text));
    return c + 2;
}

/* ------------------------------------------------------------------ archive JSON */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* chaîne JSON échappée dans d[*k..lim) ; s'arrête avant un caractère qui ne
 * tient pas (jamais d'échappement ni de caractère UTF-8 coupé) */
static void put_str(char *d, size_t *k, size_t lim, const char *s)
{
    while (*s) {
        uint8_t c = (uint8_t)*s;
        char e[8];
        size_t n = 2, adv = 1;
        if (c == '"' || c == '\\') { e[0] = '\\'; e[1] = (char)c; }
        else if (c == '\n') memcpy(e, "\\n", 2);
        else if (c == '\t') memcpy(e, "\\t", 2);
        else if (c == '\r') memcpy(e, "\\r", 2);
        else if (c < 0x20 || c == 0x7F) n = (size_t)snprintf(e, sizeof e, "\\u%04x", c);
        else if (c < 0x80) { e[0] = (char)c; n = 1; }
        else {
            size_t L = c >= 0xF8 ? 0 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
            for (size_t i = 1; i < L; i++)
                if (((uint8_t)s[i] & 0xC0) != 0x80) { L = 0; break; }
            if (L) { memcpy(e, s, L); n = adv = L; }
            else n = (size_t)snprintf(e, sizeof e, "\\ufffd");   /* UTF-8 invalide */
        }
        if (*k + n > lim) return;
        memcpy(d + *k, e, n);
        *k += n;
        s += adv;
    }
}

static void put_raw(char *d, size_t *k, size_t lim, const char *s)
{
    size_t n = strlen(s);
    if (*k + n > lim) n = lim - *k;
    memcpy(d + *k, s, n);
    *k += n;
}

void mc_arch_encode(const mc_msg_t *m, int32_t tz_min, char rec[MC_ARCH_REC])
{
    const size_t lim = MC_ARCH_REC - 32;    /* place gardée pour la fin de ligne */
    size_t k = 0;
    char b[128];
    time_t t = (time_t)m->ts + (time_t)tz_min * 60;
    struct tm tm;
    gmtime_r(&t, &tm);
    char when[24];
    strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tm);
    snprintf(b, sizeof b, "{\"seq\":%lu,\"ts\":%lu,\"time\":\"%s\",\"dir\":\"%s\",\"conv\":\"",
             (unsigned long)m->seq, (unsigned long)m->ts, when, (m->flags & MC_F_OUT) ? "out" : "in");
    put_raw(rec, &k, lim, b);
    put_str(rec, &k, lim - 160, m->from);
    const char *txt = m->text;
    char sender[MC_NAME_MAX];
    sender[0] = 0;
    if (m->kind == MC_CONV_CHANNEL && !(m->flags & MC_F_OUT)) txt = mc_split_sender(m->text, sender, sizeof sender);
    if (sender[0]) {
        put_raw(rec, &k, lim, "\",\"sender\":\"");
        put_str(rec, &k, lim - 120, sender);
    }
    put_raw(rec, &k, lim, "\",\"text\":\"");
    put_str(rec, &k, lim - 2, txt);
    int q = m->snr_q4, a = q < 0 ? -q : q;
    snprintf(b, sizeof b, "\",\"snr\":%s%d.%02d,\"hops\":%u}", q < 0 ? "-" : "", a / 4, (a % 4) * 25,
             m->hops == 0xFF ? 0u : (unsigned)m->hops);
    size_t end = MC_ARCH_REC - 1;
    put_raw(rec, &k, end, b);
    memset(rec + k, ' ', end - k);
    rec[end] = '\n';
}

/* valeur de "key": dans la ligne (après le premier '{') */
static const char *find_key(const char *rec, size_t len, const char *key)
{
    size_t kl = strlen(key);
    for (size_t i = 0; i + kl + 3 <= len; i++) {
        if (rec[i] == '"' && !memcmp(rec + i + 1, key, kl) && rec[i + 1 + kl] == '"' && rec[i + 2 + kl] == ':')
            return rec + i + 3 + kl;
        if (rec[i] == '"' && i > 0) {                   /* saute l'intérieur des chaînes */
            for (i++; i < len && rec[i] != '"'; i++) if (rec[i] == '\\') i++;
        }
    }
    return NULL;
}

static void put_utf8(char *d, size_t cap, size_t *k, uint32_t cp)
{
    char u[4];
    size_t n;
    if (cp < 0x80) { u[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { u[0] = (char)(0xC0 | cp >> 6); u[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) {
        u[0] = (char)(0xE0 | cp >> 12); u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[2] = (char)(0x80 | (cp & 0x3F)); n = 3;
    } else {
        u[0] = (char)(0xF0 | cp >> 18); u[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        u[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[3] = (char)(0x80 | (cp & 0x3F)); n = 4;
    }
    if (*k + n >= cap) return;
    memcpy(d + *k, u, n);
    *k += n;
}

static unsigned hex4(const char *p, const char *end, bool *ok)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        if (p + i >= end) { *ok = false; return 0; }
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else { *ok = false; return 0; }
    }
    return v;
}

/* chaîne JSON à p (qui pointe sur '"') ; false si mal formée */
static bool get_str(const char *p, const char *end, char *out, size_t cap)
{
    size_t k = 0;
    if (!cap || !p || p >= end || *p != '"') return false;
    for (p++; p < end && *p != '"'; p++) {
        if (*p != '\\') {
            if (k + 1 < cap) out[k++] = *p;
            continue;
        }
        if (++p >= end) return false;
        switch (*p) {
        case 'n': if (k + 1 < cap) out[k++] = '\n'; break;
        case 't': if (k + 1 < cap) out[k++] = '\t'; break;
        case 'r': if (k + 1 < cap) out[k++] = '\r'; break;
        case 'b': case 'f': break;
        case 'u': {
            bool ok = true;
            uint32_t cp = hex4(p + 1, end, &ok);
            if (!ok) return false;
            p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && p + 6 < end && p[1] == '\\' && p[2] == 'u') {
                uint32_t lo = hex4(p + 3, end, &ok);
                if (ok && lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
            }
            if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;
            if (cp) put_utf8(out, cap, &k, cp);
            break;
        }
        default: if (k + 1 < cap) out[k++] = *p; break;     /* \" \\ \/ */
        }
    }
    if (p >= end) return false;
    out[k] = 0;
    char tmp[MC_TEXT_MAX + 1];                              /* tronqué au milieu d'un caractère ? */
    mc_copy_utf8(tmp, sizeof tmp < cap ? sizeof tmp : cap, out, SIZE_MAX);
    if (strlen(tmp) != k) memcpy(out, tmp, strlen(tmp) + 1);
    return true;
}

static bool get_num(const char *rec, size_t len, const char *key, double *v)
{
    const char *p = find_key(rec, len, key);
    if (!p) return false;
    char b[24];
    size_t n = 0;
    while (p + n < rec + len && n < sizeof b - 1 && strchr("-+.0123456789eE", p[n])) { b[n] = p[n]; n++; }
    if (!n) return false;
    b[n] = 0;
    char *e;
    *v = strtod(b, &e);
    return e != b;
}

bool mc_arch_decode(const char *rec, size_t len, mc_msg_t *m)
{
    if (!len || rec[0] != '{') return false;
    const char *end = rec + len;
    double seq, ts, snr = 0, hops = 0;
    if (!get_num(rec, len, "seq", &seq) || !get_num(rec, len, "ts", &ts) || seq < 1 || seq > 4294967295.0 ||
        ts < 0 || ts > 4294967295.0)
        return false;
    get_num(rec, len, "snr", &snr);
    get_num(rec, len, "hops", &hops);
    char text[MC_TEXT_MAX + 1], sender[MC_NAME_MAX];
    if (!get_str(find_key(rec, len, "conv"), end, m->from, sizeof m->from) ||
        !get_str(find_key(rec, len, "text"), end, text, sizeof text))
        return false;
    if (!get_str(find_key(rec, len, "sender"), end, sender, sizeof sender)) sender[0] = 0;
    char dir[8];
    if (!get_str(find_key(rec, len, "dir"), end, dir, sizeof dir)) dir[0] = 0;
    m->seq = (uint32_t)seq;
    m->ts = (uint32_t)ts;
    m->qkey = 0;
    m->flags = (uint8_t)(MC_F_READ | (!strcmp(dir, "out") ? MC_F_OUT : 0));
    if (snr < -32) snr = -32;
    if (snr > 31.75) snr = 31.75;
    m->snr_q4 = (int8_t)(snr * 4 + (snr < 0 ? -0.5 : 0.5));
    m->hops = (hops >= 1 && hops <= 254) ? (uint8_t)hops : 0xFF;
    if (sender[0]) {
        size_t sl = strlen(sender);
        if (sl + 2 < sizeof m->text) {
            memcpy(m->text, sender, sl);
            memcpy(m->text + sl, ": ", 2);
            mc_copy_utf8(m->text + sl + 2, sizeof m->text - sl - 2, text, SIZE_MAX);
        } else {
            mc_copy_utf8(m->text, sizeof m->text, text, SIZE_MAX);
        }
    } else {
        mc_copy_utf8(m->text, sizeof m->text, text, SIZE_MAX);
    }
    return true;
}
