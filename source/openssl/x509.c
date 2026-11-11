#include "x509.h"
#include <stdlib.h>

X509 *d2i_X509(X509 **a, const unsigned char **in, long len) {
    X509 *cert = calloc(1, sizeof(X509));
    mbedtls_x509_crt_init(cert);
    if (mbedtls_x509_crt_parse_der(cert, *in, len) != 0) { mbedtls_x509_crt_free(cert); free(cert); return NULL; }
    return cert;
}
void X509_free(X509 *a) { if (a) { mbedtls_x509_crt_free(a); free(a); } }
EVP_PKEY *X509_get_pubkey(X509 *x) { return &x->pk; }
void EVP_PKEY_free(EVP_PKEY *pkey) { }
void X509_get0_signature(const ASN1_BIT_STRING **psig, void *palgo, X509 *cert) {
    static ASN1_BIT_STRING asn;
    asn.length = cert->MBEDTLS_PRIVATE(sig).len; asn.data = cert->MBEDTLS_PRIVATE(sig).p; *psig = &asn;
}
