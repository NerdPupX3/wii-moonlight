#ifndef OPENSSL_PEM_H
#define OPENSSL_PEM_H
#include "evp.h"
#include <stdio.h>

typedef struct { char *buf; int len; } BIO;

BIO *BIO_new_mem_buf(const void *buf, int len);
void BIO_free(BIO *a);
EVP_PKEY *PEM_read_bio_PrivateKey(BIO *bp, EVP_PKEY **x, void *cb, void *u);
X509 *PEM_read_X509(FILE *fp, X509 **x, void *cb, void *u);
EVP_PKEY *PEM_read_PrivateKey(FILE *fp, EVP_PKEY **x, void *cb, void *u);
BIO *BIO_new(void *type);
void *BIO_s_mem(void);
int BIO_puts(BIO *bp, const char *buf);
X509 *PEM_read_bio_X509(BIO *bp, X509 **x, void *cb, void *u);

#endif
