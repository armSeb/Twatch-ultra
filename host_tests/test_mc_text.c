/* Tests PC de main/meshcore/mc_text.c : coupe UTF-8, clé de tri, trames. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mc_text.h"
#include "mc_port.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("ÉCHEC %s:%d : %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void t_copy(void)
{
    char d[8];
    mc_copy_utf8(d, sizeof d, "abc", SIZE_MAX);
    CHECK(!strcmp(d, "abc"));
    mc_copy_utf8(d, sizeof d, "abcdefghij", SIZE_MAX);          /* 7 octets au plus */
    CHECK(!strcmp(d, "abcdefg"));
    mc_copy_utf8(d, sizeof d, "abcde\xC3\xA9", SIZE_MAX);       /* « é » tient tout juste */
    CHECK(!strcmp(d, "abcde\xC3\xA9"));
    mc_copy_utf8(d, sizeof d, "abcdef\xC3\xA9", SIZE_MAX);      /* « é » coupé : retiré */
    CHECK(!strcmp(d, "abcdef"));
    mc_copy_utf8(d, sizeof d, "ab\xF0\x9F\x91\x8B" "cd", SIZE_MAX);   /* 👋 entier */
    CHECK(!strcmp(d, "ab\xF0\x9F\x91\x8B" "c"));
    mc_copy_utf8(d, sizeof d, "abcd\xF0\x9F\x91\x8B", SIZE_MAX);      /* 👋 coupé */
    CHECK(!strcmp(d, "abcd"));
    mc_copy_utf8(d, sizeof d, "xy\0zz", 5);                      /* NUL dans la longueur */
    CHECK(!strcmp(d, "xy"));
    mc_copy_utf8(d, sizeof d, "hello", 3);
    CHECK(!strcmp(d, "hel"));
    char one[1];
    mc_copy_utf8(one, 1, "abc", SIZE_MAX);
    CHECK(one[0] == 0);
}

static int cmp(const char *a, const char *b)
{
    char ka[24], kb[24];
    mc_sort_key(a, ka, sizeof ka);
    mc_sort_key(b, kb, sizeof kb);
    return strcmp(ka, kb);
}

static void t_sort(void)
{
    char k[24];
    mc_sort_key("\xF0\x9F\x90\xA2 Tortue", k, sizeof k);        /* 🐢 Tortue */
    CHECK(!strcmp(k, "tortue"));
    mc_sort_key("#Public", k, sizeof k);
    CHECK(!strcmp(k, "public"));
    mc_sort_key("\xC3\x89lodie", k, sizeof k);                   /* Élodie */
    CHECK(!strcmp(k, "elodie"));
    mc_sort_key("\xF0\x9F\x94\xA5", k, sizeof k);                /* que des émojis : gardés */
    CHECK(!strcmp(k, "\xF0\x9F\x94\xA5"));
    CHECK(cmp("alice", "Bob") < 0);
    CHECK(cmp("\xC3\x89mile", "Fred") < 0);                     /* Émile avant Fred */
    CHECK(cmp("zoe", "\xF0\x9F\x9A\x80 Alpha") > 0);              /* 🚀 Alpha avant zoe */
    mc_sort_key("abcdefghijklmnopqrstuvwxyz", k, 8);
    CHECK(strlen(k) == 7);
}

static void t_frames(void)
{
    uint8_t f[32] = { 0 };
    uint8_t kind = 9;
    uint32_t ts = 0, want = 0x6543ABCDu;
    f[0] = 16;                                                   /* contact v3 */
    memcpy(f + 12, &want, 4);
    CHECK(mc_frame_info(f, 20, &kind, &ts) && kind == MC_CONV_CONTACT && ts == want);
    f[0] = 17;                                                   /* canal v3 */
    memcpy(f + 7, &want, 4);
    CHECK(mc_frame_info(f, 20, &kind, &ts) && kind == MC_CONV_CHANNEL && ts == want);
    f[0] = 8;                                                    /* canal v1 */
    memcpy(f + 4, &want, 4);
    CHECK(mc_frame_info(f, 9, &kind, &ts) && kind == MC_CONV_CHANNEL && ts == want);
    f[0] = 7;
    memcpy(f + 9, &want, 4);
    CHECK(mc_frame_info(f, 13, &kind, &ts) && kind == MC_CONV_CONTACT && ts == want);
    CHECK(!mc_frame_info(f, 12, &kind, &ts));                    /* trop court */
    f[0] = 27;                                                   /* données de canal : pas un texte */
    CHECK(!mc_frame_info(f, 20, &kind, &ts));
    CHECK(!mc_frame_info(f, 0, &kind, &ts));
}

static void t_sender(void)
{
    char s[MC_NAME_MAX];
    const char *t = mc_split_sender("Alice: salut", s, sizeof s);
    CHECK(!strcmp(s, "Alice") && !strcmp(t, "salut"));
    t = mc_split_sender("pas de nom", s, sizeof s);
    CHECK(!s[0] && !strcmp(t, "pas de nom"));
    t = mc_split_sender(": vide", s, sizeof s);
    CHECK(!s[0] && !strcmp(t, ": vide"));
    CHECK(mc_fnv32("", 0) != 0);
    uint8_t a[6], b[6], k1[32] = { 1 }, k2[32] = { 2 };
    mc_channel_peer(k1, sizeof k1, a);
    mc_channel_peer(k2, sizeof k2, b);
    CHECK(memcmp(a, b, 6) != 0);
}

int main(void)
{
    t_copy();
    t_sort();
    t_frames();
    t_sender();
    printf(fails ? "mc_text : %d échec(s)\n" : "mc_text : OK\n", fails);
    return fails != 0;
}
