#include "evp.h"
#include <stdlib.h>
#include <string.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

extern int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen);

/* ===================== Digest / signature (pairing) ===================== */

const void *EVP_sha256(void) { return (void*)MBEDTLS_MD_SHA256; }
EVP_MD_CTX *EVP_MD_CTX_new(void) { return calloc(1, sizeof(EVP_MD_CTX)); }
void EVP_MD_CTX_free(EVP_MD_CTX *ctx) { free(ctx); }

int EVP_DigestVerifyInit(EVP_MD_CTX *ctx, void *pctx, const void *type, void *e, EVP_PKEY *pkey) {
    ctx->pkey = pkey; ctx->hash_len = 0; return 1;
}
int EVP_DigestVerifyUpdate(EVP_MD_CTX *ctx, const void *d, size_t cnt) {
    if (ctx->hash_len + cnt > sizeof(ctx->hash)) return 0;
    memcpy(ctx->hash + ctx->hash_len, d, cnt); ctx->hash_len += cnt; return 1;
}
int EVP_DigestVerifyFinal(EVP_MD_CTX *ctx, const unsigned char *sig, size_t siglen) {
    /* OpenSSL hashes the accumulated message internally; mbedtls_pk_verify
     * expects the digest, so compute SHA-256 over the raw message first. */
    unsigned char digest[32];
    mbedtls_sha256(ctx->hash, ctx->hash_len, digest, 0);
    return (mbedtls_pk_verify(ctx->pkey, MBEDTLS_MD_SHA256, digest, sizeof(digest), sig, siglen) == 0) ? 1 : 0;
}

int EVP_DigestSignInit(EVP_MD_CTX *ctx, void *pctx, const void *type, void *e, EVP_PKEY *pkey) {
    ctx->pkey = pkey; ctx->hash_len = 0; return 1;
}
int EVP_DigestSignUpdate(EVP_MD_CTX *ctx, const void *d, size_t cnt) {
    if (ctx->hash_len + cnt > sizeof(ctx->hash)) return 0;
    memcpy(ctx->hash + ctx->hash_len, d, cnt); ctx->hash_len += cnt; return 1;
}
int EVP_DigestSignFinal(EVP_MD_CTX *ctx, unsigned char *sig, size_t *siglen) {
    unsigned char digest[32];
    mbedtls_sha256(ctx->hash, ctx->hash_len, digest, 0);

    mbedtls_entropy_context entropy; mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_init(&entropy); mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_entropy_add_source(&entropy, mbedtls_hardware_poll, NULL, 32, MBEDTLS_ENTROPY_SOURCE_STRONG);
    mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, NULL, 0);

    int ret;
    if (sig == NULL) {
        *siglen = mbedtls_pk_get_len(ctx->pkey);
        ret = 0;
    } else {
        size_t buf_size = *siglen;
        ret = mbedtls_pk_sign(ctx->pkey, MBEDTLS_MD_SHA256, digest, sizeof(digest),
                              sig, buf_size, siglen, mbedtls_ctr_drbg_random, &ctr_drbg);
    }
    mbedtls_entropy_free(&entropy); mbedtls_ctr_drbg_free(&ctr_drbg);
    return (ret == 0) ? 1 : 0;
}

/* ===================== Cipher context (CBC / GCM / ECB) ===================== */

static int cipher_type(const void *cipher) {
    if (cipher == (const void*)2) return 2;  /* GCM */
    if (cipher == (const void*)3) return 3;  /* ECB */
    if (cipher == (const void*)1) return 1;  /* CBC */
    return 1; /* default CBC */
}

EVP_CIPHER_CTX *EVP_CIPHER_CTX_new(void) {
    EVP_CIPHER_CTX *ctx = calloc(1, sizeof(EVP_CIPHER_CTX));
    if (ctx) {
        mbedtls_aes_init(&ctx->aes);
        mbedtls_cipher_init(&ctx->cipher);
        mbedtls_gcm_init(&ctx->gcm);
        ctx->iv_len = 16;
    }
    return ctx;
}

void EVP_CIPHER_CTX_free(EVP_CIPHER_CTX *ctx) {
    if (!ctx) return;
    mbedtls_aes_free(&ctx->aes);
    mbedtls_cipher_free(&ctx->cipher);
    mbedtls_gcm_free(&ctx->gcm);
    free(ctx);
}

const void *EVP_aes_128_ecb(void) { return (const void*)3; }
const void *EVP_aes_128_cbc(void) { return (const void*)1; }
const void *EVP_aes_128_gcm(void) { return (const void*)2; }

int EVP_EncryptInit(EVP_CIPHER_CTX *ctx, const void *cipher, const unsigned char *key, const unsigned char *iv) {
    return EVP_EncryptInit_ex(ctx, cipher, NULL, key, iv);
}
int EVP_DecryptInit(EVP_CIPHER_CTX *ctx, const void *cipher, const unsigned char *key, const unsigned char *iv) {
    return EVP_DecryptInit_ex(ctx, cipher, NULL, key, iv);
}

int EVP_EncryptInit_ex(EVP_CIPHER_CTX *ctx, const void *cipher, void *engine, const unsigned char *key, const unsigned char *iv) {
    (void)engine;
    if (cipher != NULL) {
        int type = cipher_type(cipher);
        ctx->cipher_id = type;
        ctx->enc = 1;
        if (type == 2) { /* GCM */
            mbedtls_gcm_init(&ctx->gcm);
            ctx->iv_len = 12; /* overridden by EVP_CTRL_GCM_SET_IVLEN */
        } else if (type == 3) { /* ECB */
            mbedtls_aes_init(&ctx->aes);
        } else { /* CBC */
            mbedtls_cipher_init(&ctx->cipher);
            if (mbedtls_cipher_setup(&ctx->cipher,
                    mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_CBC)) != 0) {
                return 0;
            }
            ctx->iv_len = 16;
        }
    }
    if (key != NULL) {
        if (ctx->cipher_id == 2) {
            if (mbedtls_gcm_setkey(&ctx->gcm, MBEDTLS_CIPHER_ID_AES, key, 128) != 0) return 0;
        } else if (ctx->cipher_id == 3) {
            if (mbedtls_aes_setkey_enc(&ctx->aes, key, 128) != 0) return 0;
        } else {
            if (mbedtls_cipher_setkey(&ctx->cipher, key, 128, MBEDTLS_ENCRYPT) != 0) return 0;
        }
    }
    if (iv != NULL) {
        if (ctx->cipher_id == 2) {
            if (mbedtls_gcm_starts(&ctx->gcm, MBEDTLS_GCM_ENCRYPT, iv, ctx->iv_len) != 0) return 0;
        } else if (ctx->cipher_id == 1) {
            if (mbedtls_cipher_set_iv(&ctx->cipher, iv, ctx->iv_len) != 0) return 0;
        }
        /* ECB: no IV */
    }
    return 1;
}

int EVP_DecryptInit_ex(EVP_CIPHER_CTX *ctx, const void *cipher, void *engine, const unsigned char *key, const unsigned char *iv) {
    (void)engine;
    if (cipher != NULL) {
        int type = cipher_type(cipher);
        ctx->cipher_id = type;
        ctx->enc = 0;
        if (type == 2) { /* GCM */
            mbedtls_gcm_init(&ctx->gcm);
            ctx->iv_len = 12;
        } else if (type == 3) { /* ECB */
            mbedtls_aes_init(&ctx->aes);
        } else { /* CBC */
            mbedtls_cipher_init(&ctx->cipher);
            if (mbedtls_cipher_setup(&ctx->cipher,
                    mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_CBC)) != 0) {
                return 0;
            }
            ctx->iv_len = 16;
        }
    }
    if (key != NULL) {
        if (ctx->cipher_id == 2) {
            if (mbedtls_gcm_setkey(&ctx->gcm, MBEDTLS_CIPHER_ID_AES, key, 128) != 0) return 0;
        } else if (ctx->cipher_id == 3) {
            if (mbedtls_aes_setkey_dec(&ctx->aes, key, 128) != 0) return 0;
        } else {
            if (mbedtls_cipher_setkey(&ctx->cipher, key, 128, MBEDTLS_DECRYPT) != 0) return 0;
        }
    }
    if (iv != NULL) {
        if (ctx->cipher_id == 2) {
            if (mbedtls_gcm_starts(&ctx->gcm, MBEDTLS_GCM_DECRYPT, iv, ctx->iv_len) != 0) return 0;
        } else if (ctx->cipher_id == 1) {
            if (mbedtls_cipher_set_iv(&ctx->cipher, iv, ctx->iv_len) != 0) return 0;
        }
    }
    return 1;
}

int EVP_CIPHER_CTX_set_padding(EVP_CIPHER_CTX *ctx, int padding) {
    if (ctx->cipher_id == 1) {
        /* CBC only: PKCS7 on, none off. */
        mbedtls_cipher_set_padding_mode(&ctx->cipher,
            padding ? MBEDTLS_PADDING_PKCS7 : MBEDTLS_PADDING_NONE);
    }
    /* ECB pairing path sets padding off; the input is already block-aligned,
     * so it is a no-op there. GCM never pads. */
    return 1;
}

int EVP_CIPHER_CTX_ctrl(EVP_CIPHER_CTX *ctx, int cmd, int arg, void *ptr) {
    if (cmd == EVP_CTRL_GCM_SET_IVLEN) {
        ctx->iv_len = arg;
        return 1;
    }
    if (cmd == EVP_CTRL_GCM_GET_TAG) {
        if (arg < 0 || arg > 16) return 0;
        memcpy(ptr, ctx->tag, arg);
        return 1;
    }
    if (cmd == EVP_CTRL_GCM_SET_TAG) {
        if (arg < 0 || arg > 16) return 0;
        memcpy(ctx->tag, ptr, arg);
        ctx->tag_len = arg;
        return 1;
    }
    return 0;
}

int EVP_EncryptUpdate(EVP_CIPHER_CTX *ctx, unsigned char *out, int *outl, const unsigned char *in, int inl) {
    size_t olen = 0;
    int ret = 0;
    if (ctx->cipher_id == 2) {
        ret = mbedtls_gcm_update(&ctx->gcm, in, inl, out, inl, &olen);
    } else if (ctx->cipher_id == 3) {
        for (int i = 0; i < inl; i += 16)
            mbedtls_aes_crypt_ecb(&ctx->aes, MBEDTLS_AES_ENCRYPT, in + i, out + i);
        olen = inl;
    } else {
        ret = mbedtls_cipher_update(&ctx->cipher, in, inl, out, &olen);
    }
    *outl = (int)olen;
    return (ret == 0) ? 1 : 0;
}

int EVP_DecryptUpdate(EVP_CIPHER_CTX *ctx, unsigned char *out, int *outl, const unsigned char *in, int inl) {
    size_t olen = 0;
    int ret = 0;
    if (ctx->cipher_id == 2) {
        ret = mbedtls_gcm_update(&ctx->gcm, in, inl, out, inl, &olen);
    } else if (ctx->cipher_id == 3) {
        for (int i = 0; i < inl; i += 16)
            mbedtls_aes_crypt_ecb(&ctx->aes, MBEDTLS_AES_DECRYPT, in + i, out + i);
        olen = inl;
    } else {
        ret = mbedtls_cipher_update(&ctx->cipher, in, inl, out, &olen);
    }
    *outl = (int)olen;
    return (ret == 0) ? 1 : 0;
}

int EVP_EncryptFinal_ex(EVP_CIPHER_CTX *ctx, unsigned char *out, int *outl) {
    size_t olen = 0;
    int ret;
    if (ctx->cipher_id == 2) {
        /* GCM: flush (0 bytes) then stage the auth tag for GET_TAG. */
        ret = mbedtls_gcm_finish(&ctx->gcm, NULL, 0, &olen, ctx->tag, 16);
        ctx->tag_len = 16;
        *outl = (int)olen;
        return (ret == 0) ? 1 : 0;
    }
    /* CBC: flush + PKCS7 pad; ECB: flush (no padding). */
    ret = mbedtls_cipher_finish(&ctx->cipher, out, &olen);
    *outl = (int)olen;
    return (ret == 0) ? 1 : 0;
}

int EVP_DecryptFinal_ex(EVP_CIPHER_CTX *ctx, unsigned char *out, int *outl) {
    size_t olen = 0;
    int ret;
    if (ctx->cipher_id == 2) {
        /* GCM: flush then verify the staged tag. */
        ret = mbedtls_gcm_finish(&ctx->gcm, out, 0, &olen, ctx->tag, ctx->tag_len);
        *outl = (int)olen;
        return (ret == 0) ? 1 : 0; /* 0 on tag mismatch */
    }
    ret = mbedtls_cipher_finish(&ctx->cipher, out, &olen);
    *outl = (int)olen;
    return (ret == 0) ? 1 : 0;
}
