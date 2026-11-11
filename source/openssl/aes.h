#ifndef OPENSSL_AES_H
#define OPENSSL_AES_H

#include <stddef.h>
#include <mbedtls/aes.h>

#define AES_ENCRYPT 1
#define AES_DECRYPT 0
#define AES_BLOCK_SIZE 16

typedef struct {
    mbedtls_aes_context ctx;
} AES_KEY;

int AES_set_encrypt_key(const unsigned char *userKey, const int bits, AES_KEY *key);
int AES_set_decrypt_key(const unsigned char *userKey, const int bits, AES_KEY *key);
void AES_encrypt(const unsigned char *in, unsigned char *out, const AES_KEY *key);
void AES_decrypt(const unsigned char *in, unsigned char *out, const AES_KEY *key);
void AES_cbc_encrypt(const unsigned char *in, unsigned char *out, size_t length, const AES_KEY *key, unsigned char *ivec, const int enc);

#endif
