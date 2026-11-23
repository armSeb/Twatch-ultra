/*
 * base64.hpp — décodage base64 (API de la bibliothèque base64 d'Arduino).
 * Seul usage : la clé d'un canal (16 ou 32 octets) dans un tampon de 32.
 * On n'écrit jamais au-delà de 32 octets ; la longueur renvoyée est la
 * longueur décodée réelle, que l'appelant compare à 16 ou 32.
 */
#pragma once
#include <stdint.h>

#define BASE64_DECODE_MAX 32

static inline int b64_val(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static inline unsigned int decode_base64(const unsigned char *in, unsigned int n, unsigned char *out)
{
    unsigned int k = 0, bits = 0, acc = 0;
    for (unsigned int i = 0; i < n; i++) {
        if (in[i] == '=') break;
        int v = b64_val(in[i]);
        if (v < 0) return 0;                    /* caractère invalide : clé refusée */
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (k < BASE64_DECODE_MAX) out[k] = (unsigned char)(acc >> bits);
            k++;
            acc &= (1u << bits) - 1;
        }
    }
    return k;
}
