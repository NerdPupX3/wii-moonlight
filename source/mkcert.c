#include "mkcert.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/md.h>

#include "entropy.h"
#include "applog.h"

// Creates this Wii's identity: a 2048-bit RSA key and a self-signed certificate.
// Sunshine remembers the certificate when you pair, so whoever holds the private
// key can stream from that PC. All randomness (the RSA primes, the certificate
// serial number, the signature) comes from entropy.c.
CERT_KEY_PAIR mkcert_generate() {
    CERT_KEY_PAIR pair = {NULL, NULL};
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    char *subject = "CN=Wii-Moonlight,O=Nintendo";
    int r;

    pair.cert = malloc(4096); pair.key = malloc(4096);
    if (!pair.cert || !pair.key) {
        app_log("[cert] ERROR: out of memory\n");
        return pair;
    }
    memset(pair.cert, 0, 4096); memset(pair.key, 0, 4096);

    if (entropy_init() != 0) {
        app_log("[cert] ERROR: random generator unavailable, not creating a key\n");
        return pair;
    }

    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);

    u64 t0 = gettime();
    if ((r = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA))) != 0) goto fail;
    if ((r = mbedtls_rsa_gen_key(mbedtls_pk_rsa(key), entropy_random, NULL, 2048, 65537)) != 0) goto fail;
    u64 t1 = gettime();
    app_log("[cert] RSA-2048 key generated in %d ms\n", (int)ticks_to_millisecs(t1 - t0));

    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);

    if ((r = mbedtls_x509write_crt_set_subject_name(&crt, subject)) != 0) goto fail;
    if ((r = mbedtls_x509write_crt_set_issuer_name(&crt, subject)) != 0) goto fail;

    mbedtls_x509write_crt_set_validity(&crt, "20230101000000", "20430101000000");

    /* Random 128-bit serial number, top bit clear so it reads as positive. */
    unsigned char serial[16];
    if ((r = entropy_random(NULL, serial, sizeof(serial))) != 0) goto fail;
    serial[0] &= 0x7F;
    if (serial[0] == 0) serial[0] = 1;
    if ((r = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial))) != 0) goto fail;

    if ((r = mbedtls_x509write_crt_pem(&crt, (unsigned char*)pair.cert, 4096, entropy_random, NULL)) != 0) goto fail;
    if ((r = mbedtls_pk_write_key_pem(&key, (unsigned char*)pair.key, 4096)) != 0) goto fail;
    app_log("[cert] certificate created (SHA-256, valid 2023-2043, random serial)\n");
    goto done;

fail:
    app_log("[cert] ERROR: certificate creation failed -0x%04x\n", -r);
    memset(pair.cert, 0, 4096);
    memset(pair.key, 0, 4096);
done:
    mbedtls_pk_free(&key);
    mbedtls_x509write_crt_free(&crt);
    return pair;
}

void mkcert_save(const char *cert_path, const char *p12_path, const char *key_path, CERT_KEY_PAIR cert) {
    (void)p12_path;
    if (!cert.cert || !cert.key || !cert.cert[0] || !cert.key[0]) {
        app_log("[cert] not saving: generation failed\n");
        return;
    }
    FILE *f = fopen(cert_path, "w"); if (f) { fputs(cert.cert, f); fclose(f); }
    f = fopen(key_path, "w"); if (f) { fputs(cert.key, f); fclose(f); }
    app_log("[cert] saved %s and %s\n", cert_path, key_path);
}

void mkcert_free(CERT_KEY_PAIR cert) {
    if (cert.cert) free(cert.cert);
    if (cert.key) free(cert.key);
}
