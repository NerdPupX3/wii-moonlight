#ifndef OPENSSL_EVP_H
#define OPENSSL_EVP_H

#include <stddef.h>
#include <stdlib.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/aes.h>
#include <mbedtls/cipher.h>
#include <mbedtls/gcm.h>

#define EVP_PKEY mbedtls_pk_context
#define X509 mbedtls_x509_crt

#define EVP_MD_CTX_create EVP_MD_CTX_new
#define EVP_MD_CTX_destroy EVP_MD_CTX_free
#define OPENSSL_malloc malloc

/* GCM ctrl commands (values match OpenSSL) */
#define EVP_CTRL_GCM_SET_IVLEN 0x9
#define EVP_CTRL_GCM_GET_TAG   0x10
#define EVP_CTRL_GCM_SET_TAG   0x11

typedef struct {
    EVP_PKEY *pkey;
    unsigned char hash[64];
    size_t hash_len;
} EVP_MD_CTX;

/* Tag the struct "evp_cipher_ctx_st" so it is compatible with the forward
 * declaration in moonlight-common-c's PlatformCrypto.h, which does
 * "typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;" before including us. */
typedef struct evp_cipher_ctx_st {
    int cipher_id;   /* 0 = uninit, 1 = CBC, 2 = GCM, 3 = ECB */
    int enc;         /* 1 = encrypt, 0 = decrypt */
    int iv_len;      /* IV length: 16 for CBC/ECB, settable for GCM */
    int tag_len;
    unsigned char tag[16];   /* GCM auth tag (staged for GET_TAG / SET_TAG) */

    mbedtls_aes_context aes;               /* ECB path */
    mbedtls_cipher_context_t cipher;       /* CBC path */
    mbedtls_gcm_context gcm;               /* GCM path */
} EVP_CIPHER_CTX;

const void *EVP_sha256(void);
EVP_MD_CTX *EVP_MD_CTX_new(void);
void EVP_MD_CTX_free(EVP_MD_CTX *ctx);
int EVP_DigestVerifyInit(EVP_MD_CTX *ctx, void *pctx, const void *type, void *e, EVP_PKEY *pkey);
int EVP_DigestVerifyUpdate(EVP_MD_CTX *ctx, const void *d, size_t cnt);
int EVP_DigestVerifyFinal(EVP_MD_CTX *ctx, const unsigned char *sig, size_t siglen);
int EVP_DigestSignInit(EVP_MD_CTX *ctx, void *pctx, const void *type, void *e, EVP_PKEY *pkey);
int EVP_DigestSignUpdate(EVP_MD_CTX *ctx, const void *d, size_t cnt);
int EVP_DigestSignFinal(EVP_MD_CTX *ctx, unsigned char *sig, size_t *siglen);

EVP_CIPHER_CTX *EVP_CIPHER_CTX_new(void);
void EVP_CIPHER_CTX_free(EVP_CIPHER_CTX *ctx);
const void *EVP_aes_128_ecb(void);
const void *EVP_aes_128_cbc(void);
const void *EVP_aes_128_gcm(void);
int EVP_EncryptInit(EVP_CIPHER_CTX *ctx, const void *cipher, const unsigned char *key, const unsigned char *iv);
int EVP_DecryptInit(EVP_CIPHER_CTX *ctx, const void *cipher, const unsigned char *key, const unsigned char *iv);
int EVP_EncryptInit_ex(EVP_CIPHER_CTX *ctx, const void *cipher, void *engine, const unsigned char *key, const unsigned char *iv);
int EVP_DecryptInit_ex(EVP_CIPHER_CTX *ctx, const void *cipher, void *engine, const unsigned char *key, const unsigned char *iv);
int EVP_CIPHER_CTX_set_padding(EVP_CIPHER_CTX *ctx, int padding);
int EVP_CIPHER_CTX_ctrl(EVP_CIPHER_CTX *ctx, int cmd, int arg, void *ptr);
int EVP_EncryptUpdate(EVP_CIPHER_CTX *ctx, unsigned char *out, int *outl, const unsigned char *in, int inl);
int EVP_DecryptUpdate(EVP_CIPHER_CTX *ctx, unsigned char *out, int *outl, const unsigned char *in, int inl);
int EVP_EncryptFinal_ex(EVP_CIPHER_CTX *ctx, unsigned char *out, int *outl);
int EVP_DecryptFinal_ex(EVP_CIPHER_CTX *ctx, unsigned char *out, int *outl);

#endif
