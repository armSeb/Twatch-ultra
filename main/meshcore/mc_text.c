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
