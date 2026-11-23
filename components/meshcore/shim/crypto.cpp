/* crypto.cpp — SHA-256 / HMAC et AES-128 sur mbedTLS, vérification Ed25519 */
#include <string.h>
#include "SHA256.h"
#include "AES.h"
#include "Ed25519.h"
extern "C" {
#include "ed_25519.h"
}

/* ---------------------------------------------------------------- SHA-256 */
void SHA256::reset()
{
    if (init) mbedtls_sha256_free(&ctx);
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    init = true;
}
void SHA256::update(const void *data, size_t len) { mbedtls_sha256_update(&ctx, (const uint8_t *)data, len); }
void SHA256::finalize(void *hash, size_t len)
{
    uint8_t h[32];
    mbedtls_sha256_finish(&ctx, h);
    memcpy(hash, h, len < 32 ? len : 32);
    reset();
}

/* HMAC (RFC 2104) comme rweather : resetHMAC(clé) puis update(), puis
 * finalizeHMAC(même clé) */
static void hmac_pad(const void *key, size_t keyLen, uint8_t pad, uint8_t out[64])
{
    uint8_t k[32];
    const uint8_t *kp = (const uint8_t *)key;
    if (keyLen > 64) {
        mbedtls_sha256(kp, keyLen, k, 0);
        kp = k;
        keyLen = 32;
    }
    memset(out, pad, 64);
    for (size_t i = 0; i < keyLen; i++) out[i] ^= kp[i];
}
void SHA256::resetHMAC(const void *key, size_t keyLen)
{
    reset();
    uint8_t b[64];
    hmac_pad(key, keyLen, 0x36, b);
    update(b, 64);
}
void SHA256::finalizeHMAC(const void *key, size_t keyLen, void *hash, size_t hashLen)
{
    uint8_t inner[32], b[64];
    mbedtls_sha256_finish(&ctx, inner);
    reset();
    hmac_pad(key, keyLen, 0x5c, b);
    update(b, 64);
    update(inner, 32);
    finalize(hash, hashLen);
}

/* ---------------------------------------------------------------- AES-128 */
AES128::~AES128() { mbedtls_aes_free(&enc); mbedtls_aes_free(&dec); }
bool AES128::setKey(const uint8_t *key, size_t len)
{
    if (len != 16) return false;
    return mbedtls_aes_setkey_enc(&enc, key, 128) == 0 && mbedtls_aes_setkey_dec(&dec, key, 128) == 0;
}
void AES128::encryptBlock(uint8_t *out, const uint8_t *in) { mbedtls_aes_crypt_ecb(&enc, MBEDTLS_AES_ENCRYPT, in, out); }
void AES128::decryptBlock(uint8_t *out, const uint8_t *in) { mbedtls_aes_crypt_ecb(&dec, MBEDTLS_AES_DECRYPT, in, out); }

/* ---------------------------------------------------------------- Ed25519 */
bool Ed25519::verify(const uint8_t sig[64], const uint8_t publicKey[32], const void *message, size_t len)
{
    return ed25519_verify(sig, (const unsigned char *)message, len, publicKey) == 1;
}
