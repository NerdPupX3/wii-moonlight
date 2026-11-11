#include "sha.h"

void SHA256_Init(SHA256_CTX *c) { mbedtls_sha256_init(&c->ctx); mbedtls_sha256_starts(&c->ctx, 0); }
void SHA256_Update(SHA256_CTX *c, const void *data, size_t len) { mbedtls_sha256_update(&c->ctx, data, len); }
void SHA256_Final(unsigned char *md, SHA256_CTX *c) { mbedtls_sha256_finish(&c->ctx, md); mbedtls_sha256_free(&c->ctx); }
unsigned char *SHA256(const unsigned char *d, size_t n, unsigned char *md) { mbedtls_sha256(d, n, md, 0); return md; }
unsigned char *SHA1(const unsigned char *d, size_t n, unsigned char *md) { mbedtls_sha1(d, n, md); return md; }
