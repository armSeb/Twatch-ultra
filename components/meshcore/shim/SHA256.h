/* SHA256.h — API de la bibliothèque Crypto (rweather) sur mbedTLS */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "mbedtls/sha256.h"

class SHA256 {
public:
    SHA256() { reset(); }
    ~SHA256() { mbedtls_sha256_free(&ctx); }
    void reset();
    void update(const void *data, size_t len);
    void finalize(void *hash, size_t len);
    void resetHMAC(const void *key, size_t keyLen);
    void finalizeHMAC(const void *key, size_t keyLen, void *hash, size_t hashLen);
private:
    mbedtls_sha256_context ctx;
    bool init = false;
};
