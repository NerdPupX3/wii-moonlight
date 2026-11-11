#include "aes.h"

int AES_set_encrypt_key(const unsigned char *userKey, const int bits, AES_KEY *key) {
    mbedtls_aes_init(&key->ctx);
    return mbedtls_aes_setkey_enc(&key->ctx, userKey, bits);
}

int AES_set_decrypt_key(const unsigned char *userKey, const int bits, AES_KEY *key) {
    mbedtls_aes_init(&key->ctx);
    return mbedtls_aes_setkey_dec(&key->ctx, userKey, bits);
}

void AES_encrypt(const unsigned char *in, unsigned char *out, const AES_KEY *key) {
    mbedtls_aes_crypt_ecb((mbedtls_aes_context*)&key->ctx, MBEDTLS_AES_ENCRYPT, in, out);
}

void AES_decrypt(const unsigned char *in, unsigned char *out, const AES_KEY *key) {
    mbedtls_aes_crypt_ecb((mbedtls_aes_context*)&key->ctx, MBEDTLS_AES_DECRYPT, in, out);
}

void AES_cbc_encrypt(const unsigned char *in, unsigned char *out, size_t length, const AES_KEY *key, unsigned char *ivec, const int enc) {
    // mbedTLS modifies the IV in place, exactly like OpenSSL's CBC mode does.
    if (enc == AES_ENCRYPT) {
        mbedtls_aes_crypt_cbc((mbedtls_aes_context*)&key->ctx, MBEDTLS_AES_ENCRYPT, length, ivec, in, out);
    } else {
        mbedtls_aes_crypt_cbc((mbedtls_aes_context*)&key->ctx, MBEDTLS_AES_DECRYPT, length, ivec, in, out);
    }
}
