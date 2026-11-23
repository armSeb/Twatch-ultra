/*
 * test_meshcore.c — briques de sécurité du MeshCore embarqué, sur PC
 * (ASan/UBSan) :
 *  - ed25519 d'orlp (signature des adverts MeshCore) contre des vecteurs
 *    produits par une autre implémentation (Python cryptography) et le
 *    vecteur 1 du RFC 8032 ; signatures et clés aléatoires ou altérées
 *    toujours refusées, sans accès mémoire invalide ;
 *  - décodage base64 des clés de canal : jamais plus de 32 octets écrits.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ed_25519.h"
#include "base64.hpp"
#include "ed25519_vectors.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("ÉCHEC %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void rnd(unsigned char *b, size_t n) { for (size_t i = 0; i < n; i++) b[i] = (unsigned char)rand(); }

static void test_rfc8032(void)
{
    static const unsigned char seed[32] = {
        0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4,
        0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19, 0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60 };
    static const unsigned char pub_ref[32] = {
        0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7, 0xd5, 0x4b, 0xfe, 0xd3, 0xc9, 0x64, 0x07, 0x3a,
        0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6, 0x23, 0x25, 0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a };
    static const unsigned char sig_ref[64] = {
        0xe5, 0x56, 0x43, 0x00, 0xc3, 0x60, 0xac, 0x72, 0x90, 0x86, 0xe2, 0xcc, 0x80, 0x6e, 0x82, 0x8a,
        0x84, 0x87, 0x7f, 0x1e, 0xb8, 0xe5, 0xd9, 0x74, 0xd8, 0x73, 0xe0, 0x65, 0x22, 0x49, 0x01, 0x55,
        0x5f, 0xb8, 0x82, 0x15, 0x90, 0xa3, 0x3b, 0xac, 0xc6, 0x1e, 0x39, 0x70, 0x1c, 0xf9, 0xb4, 0x6b,
        0xd2, 0x5b, 0xf5, 0xf0, 0x59, 0x5b, 0xbe, 0x24, 0x65, 0x51, 0x41, 0x43, 0x8e, 0x7a, 0x10, 0x0b };
    unsigned char pub[32], prv[64], sig[64];
    ed25519_create_keypair(pub, prv, seed);
    CHECK(memcmp(pub, pub_ref, 32) == 0);
    ed25519_sign(sig, (const unsigned char *)"", 0, pub, prv);
    CHECK(memcmp(sig, sig_ref, 64) == 0);
    CHECK(ed25519_verify(sig_ref, (const unsigned char *)"", 0, pub_ref) == 1);
}

static void test_vectors(void)
{
    for (size_t v = 0; v < sizeof VEC / sizeof VEC[0]; v++) {
        const unsigned char *seed = (const unsigned char *)VEC[v].seed, *pubr = (const unsigned char *)VEC[v].pub;
        const unsigned char *sigr = (const unsigned char *)VEC[v].sig, *msg = (const unsigned char *)VEC[v].msg;
        size_t n = (size_t)VEC[v].n;
        unsigned char pub[32], prv[64], sig[64];
        ed25519_create_keypair(pub, prv, seed);
        CHECK(memcmp(pub, pubr, 32) == 0);
        ed25519_sign(sig, msg, n, pub, prv);
        CHECK(memcmp(sig, sigr, 64) == 0);
        CHECK(ed25519_verify(sigr, msg, n, pubr) == 1);
        /* chaque bit de la signature, et un bit du message, altérés : refus */
        for (int b = 0; b < 512; b++) {
            unsigned char s2[64];
            memcpy(s2, sigr, 64);
            s2[b / 8] ^= (unsigned char)(1u << (b % 8));
            CHECK(ed25519_verify(s2, msg, n, pubr) == 0);
        }
        if (n) {
            unsigned char *m2 = malloc(n);
            memcpy(m2, msg, n);
            m2[n / 2] ^= 0x10;
            CHECK(ed25519_verify(sigr, m2, n, pubr) == 0);
            free(m2);
        }
    }
}

/* clés et signatures quelconques venues de la radio : refus, sans plantage */
static void test_random(void)
{
    unsigned char pub[32], sig[64], msg[200];
    int accepted = 0;
    for (int i = 0; i < 5000; i++) {
        rnd(pub, 32); rnd(sig, 64); rnd(msg, sizeof msg);
        if (i & 1) sig[63] &= 0x1F;              /* passe le test de forme de S */
        accepted += ed25519_verify(sig, msg, (size_t)(i % 200), pub);
    }
    CHECK(accepted == 0);
}

static void test_base64(void)
{
    unsigned char out[32 + 16];
    memset(out, 0xAA, sizeof out);
    /* clé du canal Public de MeshCore : 16 octets */
    unsigned n = decode_base64((const unsigned char *)"izOH6cXN6mrJ5e26oRXNcg==", 24, out);
    CHECK(n == 16);
    CHECK(out[0] == 0x8b && out[15] == 0x72);
    /* trop long : longueur réelle renvoyée, rien au-delà de 32 octets */
    char big[200];
    memset(big, 'A', sizeof big);
    memset(out, 0xAA, sizeof out);
    n = decode_base64((const unsigned char *)big, sizeof big, out);
    CHECK(n == 150);
    for (int i = 32; i < (int)sizeof out; i++) CHECK(out[i] == 0xAA);
    /* caractère invalide : refus */
    CHECK(decode_base64((const unsigned char *)"ab$d", 4, out) == 0);
}

int main(void)
{
    srand(1234);
    test_rfc8032();
    test_vectors();
    test_random();
    test_base64();
    if (fails) { printf("meshcore : %d échec(s)\n", fails); return 1; }
    printf("meshcore : OK\n");
    return 0;
}
