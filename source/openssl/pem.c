#include "pem.h"
#include <stdlib.h>
#include <string.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>

extern int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen);

/* mbedtls_pk_parse_key() requires a non-NULL RNG for RSA blinding. */
static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_drbg;
static int g_rng_ready = 0;

static int pem_rng(void *p_rng, unsigned char *output, size_t len) {
    (void)p_rng;
    if (!g_rng_ready) {
        mbedtls_entropy_init(&g_entropy);
        mbedtls_ctr_drbg_init(&g_drbg);
        mbedtls_entropy_add_source(&g_entropy, mbedtls_hardware_poll, NULL, 32, MBEDTLS_ENTROPY_SOURCE_STRONG);
        mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy, NULL, 0);
        g_rng_ready = 1;
    }
    return mbedtls_ctr_drbg_random(&g_drbg, output, len);
}

BIO *BIO_new_mem_buf(const void *buf, int len) {
    BIO *bio = calloc(1, sizeof(BIO));
    bio->buf = malloc(len + 1); memcpy(bio->buf, buf, len); bio->buf[len] = 0; bio->len = len;
    return bio;
}
void BIO_free(BIO *a) { if (a) { if (a->buf) free(a->buf); free(a); } }

EVP_PKEY *PEM_read_bio_PrivateKey(BIO *bp, EVP_PKEY **x, void *cb, void *u) {
    EVP_PKEY *pkey = calloc(1, sizeof(EVP_PKEY)); mbedtls_pk_init(pkey);
    if (mbedtls_pk_parse_key(pkey, (const unsigned char*)bp->buf, bp->len + 1, NULL, 0, pem_rng, NULL) != 0) { mbedtls_pk_free(pkey); free(pkey); return NULL; }
    if (x) *x = pkey;
    return pkey;
}
X509 *PEM_read_X509(FILE *fp, X509 **x, void *cb, void *u) {
    char buf[8192]; size_t len = fread(buf, 1, sizeof(buf)-1, fp); buf[len] = 0;
    X509 *cert = calloc(1, sizeof(X509)); mbedtls_x509_crt_init(cert);
    if (mbedtls_x509_crt_parse(cert, (unsigned char*)buf, len + 1) != 0) { mbedtls_x509_crt_free(cert); free(cert); return NULL; }
    if (x) *x = cert;
    return cert;
}
EVP_PKEY *PEM_read_PrivateKey(FILE *fp, EVP_PKEY **x, void *cb, void *u) {
    char buf[8192]; size_t len = fread(buf, 1, sizeof(buf)-1, fp); buf[len] = 0;
    EVP_PKEY *pkey = calloc(1, sizeof(EVP_PKEY)); mbedtls_pk_init(pkey);
    if (mbedtls_pk_parse_key(pkey, (unsigned char*)buf, len + 1, NULL, 0, pem_rng, NULL) != 0) { mbedtls_pk_free(pkey); free(pkey); return NULL; }
    if (x) *x = pkey;
    return pkey;
}
BIO *BIO_new(void *type) { return calloc(1, sizeof(BIO)); }
void *BIO_s_mem(void) { return NULL; }
int BIO_puts(BIO *bp, const char *buf) {
    bp->len = strlen(buf); bp->buf = malloc(bp->len + 1); strcpy(bp->buf, buf); return bp->len;
}
X509 *PEM_read_bio_X509(BIO *bp, X509 **x, void *cb, void *u) {
    X509 *cert = calloc(1, sizeof(X509)); mbedtls_x509_crt_init(cert);
    if (mbedtls_x509_crt_parse(cert, (unsigned char*)bp->buf, bp->len + 1) != 0) { mbedtls_x509_crt_free(cert); free(cert); return NULL; }
    if (x) *x = cert;
    return cert;
}
