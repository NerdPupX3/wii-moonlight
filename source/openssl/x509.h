#ifndef OPENSSL_X509_H
#define OPENSSL_X509_H
#include "evp.h"

typedef struct { int length; unsigned char *data; } ASN1_BIT_STRING;

X509 *d2i_X509(X509 **a, const unsigned char **in, long len);
void X509_free(X509 *a);
EVP_PKEY *X509_get_pubkey(X509 *x);
void EVP_PKEY_free(EVP_PKEY *pkey);
void X509_get0_signature(const ASN1_BIT_STRING **psig, void *palgo, X509 *cert);

#endif
