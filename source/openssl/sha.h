#ifndef OPENSSL_SHA_H
#define OPENSSL_SHA_H
#include <stddef.h>
#include <mbedtls/sha256.h>
#include <mbedtls/sha1.h>

#define SHA256_DIGEST_LENGTH 32
typedef struct { mbedtls_sha256_context ctx; } SHA256_CTX;

void SHA256_Init(SHA256_CTX *c);
void SHA256_Update(SHA256_CTX *c, const void *data, size_t len);
void SHA256_Final(unsigned char *md, SHA256_CTX *c);
unsigned char *SHA256(const unsigned char *d, size_t n, unsigned char *md);
unsigned char *SHA1(const unsigned char *d, size_t n, unsigned char *md);

#endif
