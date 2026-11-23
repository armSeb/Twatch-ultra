/* Ed25519.h — vérification de signature (API rweather) sur l'ed25519 d'orlp */
#pragma once
#include <stddef.h>
#include <stdint.h>

class Ed25519 {
public:
    static bool verify(const uint8_t sig[64], const uint8_t publicKey[32], const void *message, size_t len);
};
