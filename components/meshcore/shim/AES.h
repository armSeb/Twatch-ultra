/* AES.h — AES-128 en blocs (API rweather) sur mbedTLS */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "mbedtls/aes.h"

class AES128 {
public:
    AES128() { mbedtls_aes_init(&enc); mbedtls_aes_init(&dec); }
    ~AES128();
    bool setKey(const uint8_t *key, size_t len);
    void encryptBlock(uint8_t *out, const uint8_t *in);
    void decryptBlock(uint8_t *out, const uint8_t *in);
private:
    mbedtls_aes_context enc, dec;
};
