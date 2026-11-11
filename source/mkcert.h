#ifndef MKCERT_H
#define MKCERT_H

typedef struct {
    char *cert;
    char *key;
} CERT_KEY_PAIR;

CERT_KEY_PAIR mkcert_generate();
void mkcert_save(const char *cert_path, const char *p12_path, const char *key_path, CERT_KEY_PAIR cert);
void mkcert_free(CERT_KEY_PAIR cert);

#endif
