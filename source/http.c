/*
 * HTTP client for Wii Moonlight.
 * Plain HTTP over libogc net_ sockets; HTTPS over mbedtls TLS
 * with certificate verification disabled (Sunshine uses a
 * self-signed cert that the client must not reject).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <network.h>
#include <arpa/inet.h>

#include <mbedtls/ssl.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/debug.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <poll.h>

#include "http.h"
#include "entropy.h"
#include "errors.h"

/* Provided by main.c */
extern int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen);

static void tls_log_printf(const char *fmt, ...);

static int g_log_level = 0;
static uint32_t g_timeout_s = 10;

/* Client certificate + key, presented during TLS when the server requests
 * one (Sunshine's HTTPS endpoint uses mTLS). Set by client.c after pairing. */
static mbedtls_x509_crt *g_client_cert = NULL;
static mbedtls_pk_context *g_client_key = NULL;

void http_set_client_cert(void *cert, void *key) {
    g_client_cert = (mbedtls_x509_crt*)cert;
    g_client_key = (mbedtls_pk_context*)key;
}

/* TLS contexts are file-scope statics: mbedtls_ssl_context alone is ~33 KB
 * (16 KB in/out buffers), and putting it plus path/req on the http_request
 * stack overflows the Wii's stack (DSI on the quit path). The app is
 * single-threaded, so static storage is safe. */
static mbedtls_ssl_context g_ssl;
static mbedtls_ssl_config g_conf;
static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_ctr_drbg;

int http_init(const char *keyDirectory, int logLevel) {
    (void)keyDirectory;
    g_log_level = logLevel;
    return 0;
}

void http_cleanup(void) {
}

void http_set_timeout_s(uint32_t connection_timeout_in) {
    g_timeout_s = connection_timeout_in;
}

void http_set_log_level(int log_level_in) {
    g_log_level = log_level_in;
}

PHTTP_DATA http_create_data(void) {
    PHTTP_DATA data = (PHTTP_DATA)calloc(1, sizeof(HTTP_DATA));
    if (data != NULL) {
        data->memory = (char*)malloc(1);
        if (data->memory != NULL)
            data->memory[0] = '\0';
        data->size = 0;
    }
    return data;
}

void http_free_data(PHTTP_DATA data) {
    if (data == NULL)
        return;
    if (data->memory != NULL)
        free(data->memory);
    free(data);
}

typedef struct _TLS_IO {
    int sock;
} TLS_IO;

static int wii_tls_send(void *ctx, const unsigned char *buf, size_t len) {
    TLS_IO *io = (TLS_IO*)ctx;
    int ret = net_send(io->sock, buf, (int)len, 0);
    if (ret < 0)
        return MBEDTLS_ERR_NET_SEND_FAILED;
    return ret;
}

static int wii_tls_recv(void *ctx, unsigned char *buf, size_t len) {
    TLS_IO *io = (TLS_IO*)ctx;
    // Wait up to 10s for data so a stalled HTTPS server can't hang us forever.
    struct pollsd p;
    p.socket = io->sock;
    p.events = POLLIN;
    p.revents = 0;
    if (net_poll(&p, 1, 10000) <= 0)
        return MBEDTLS_ERR_SSL_TIMEOUT;
    int ret = net_recv(io->sock, buf, (int)len, 0);
    if (ret < 0)
        return MBEDTLS_ERR_NET_RECV_FAILED;
    return ret; /* 0 = EOF, positive = bytes read */
}

static void parse_url(const char *url, char *host, size_t hostlen, int *port, int *is_https, char *path, size_t pathlen) {
    const char *p = url;

    *is_https = 0;
    *port = 0;

    if (strncmp(p, "https://", 8) == 0) {
        *is_https = 1;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    }

    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');

    if (colon != NULL && (slash == NULL || colon < slash)) {
        size_t hlen = (size_t)(colon - p);
        if (hlen >= hostlen) hlen = hostlen - 1;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
        *port = atoi(colon + 1);
    } else if (slash != NULL) {
        size_t hlen = (size_t)(slash - p);
        if (hlen >= hostlen) hlen = hostlen - 1;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
    } else {
        strncpy(host, p, hostlen - 1);
        host[hostlen - 1] = '\0';
    }

    if (*port == 0)
        *port = *is_https ? 47984 : 47989;

    if (slash != NULL) {
        strncpy(path, slash, pathlen - 1);
        path[pathlen - 1] = '\0';
    } else {
        strcpy(path, "/");
    }
}

static int open_connection(const char *host, int port) {
    int sock = net_socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        tls_log_printf("[http] net_socket() failed\n");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);

    /* Parse the host as a literal IP first (no DNS round-trip for IPs). */
    if (inet_aton(host, &addr.sin_addr) == 0) {
        struct hostent *he = net_gethostbyname(host);
        if (he == NULL || he->h_addr_list == NULL) {
            tls_log_printf("[http] DNS lookup failed for '%s'\n", host);
            net_close(sock);
            return -1;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(struct in_addr));
    }

    if (net_connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        tls_log_printf("[http] connect to %s:%d failed (server unreachable?)\n", host, port);
        net_close(sock);
        return -1;
    }
    tls_log_printf("[http] connected to %s:%d\n", host, port);

    return sock;
}

#define TLS_LOG_PATH "sd:/apps/wii-moonlight/tls.log"

static FILE *g_tls_log = NULL;

static void tls_log_open(void) {
    if (g_tls_log == NULL)
        g_tls_log = fopen(TLS_LOG_PATH, "w");
}

/* mbedtls debug callback: the full trace goes to the log file only. */
static void tls_debug(void *ctx, int level, const char *file, int line, const char *str) {
    (void)ctx; (void)level; (void)file; (void)line;
    tls_log_open();
    if (g_tls_log != NULL) {
        fputs(str, g_tls_log);
        fflush(g_tls_log);
    }
}

/* Summary lines: echo to console AND append to the log file. */
static void tls_log_printf(const char *fmt, ...) {
    va_list ap;
    char buf[1024];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s", buf);
    tls_log_open();
    if (g_tls_log != NULL) {
        fputs(buf, g_tls_log);
        fflush(g_tls_log);
    }
}

static int tls_connect(int sock, const char *host,
                       mbedtls_ssl_context *ssl, mbedtls_ssl_config *conf,
                       mbedtls_entropy_context *entropy, mbedtls_ctr_drbg_context *ctr_drbg,
                       TLS_IO *io) {
    int ret;

    mbedtls_ssl_init(ssl);
    mbedtls_ssl_config_init(conf);
    mbedtls_entropy_init(entropy);
    mbedtls_ctr_drbg_init(ctr_drbg);

    /* Randomness comes from the shared generator in entropy.c (seeded once from
     * IOS timing jitter). The entropy/ctr_drbg contexts stay initialized only so
     * the existing cleanup code can free them. Before, each connection seeded
     * its own generator, which now would mean re-sampling jitter every time. */
    (void)entropy; (void)ctr_drbg;

    if ((ret = mbedtls_ssl_config_defaults(conf, MBEDTLS_SSL_IS_CLIENT,
                                           MBEDTLS_SSL_TRANSPORT_STREAM,
                                           MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
        tls_log_printf("[http] TLS: config_defaults failed -0x%04x\n", -ret);
        return -1;
    }

    mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(conf, entropy_random, NULL);
    if (g_client_cert != NULL && g_client_key != NULL) {
        if ((ret = mbedtls_ssl_conf_own_cert(conf, g_client_cert, g_client_key)) != 0) {
            tls_log_printf("[http] TLS: own_cert failed -0x%04x\n", -ret);
            return -1;
        }
    }
    /* Force TLS 1.2: TLS 1.3 needs PSA crypto, whose DRBG can't get enough
     * entropy from the Wii's weak timer source (psa_crypto_init -> -0x0094). */
    mbedtls_ssl_conf_min_tls_version(conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_dbg(conf, tls_debug, NULL);
    mbedtls_debug_set_threshold(3);

    if ((ret = mbedtls_ssl_setup(ssl, conf)) != 0) {
        tls_log_printf("[http] TLS: ssl_setup failed -0x%04x\n", -ret);
        return -1;
    }

    mbedtls_ssl_set_hostname(ssl, host);

    io->sock = sock;
    mbedtls_ssl_set_bio(ssl, io, wii_tls_send, wii_tls_recv, NULL);

    if ((ret = mbedtls_ssl_handshake(ssl)) != 0) {
        tls_log_printf("[http] TLS: handshake failed -0x%04x\n", -ret);
        return -1;
    }

    tls_log_printf("[http] TLS: handshake OK (%s)\n", mbedtls_ssl_get_version(ssl));
    return 0;
}

/* Decode an HTTP/1.1 chunked body in place. Returns decoded length. */
static size_t dechunk(const char *body, size_t bodylen, char *out, size_t outcap) {
    size_t o = 0;
    const char *p = body;
    const char *end = body + bodylen;

    while (p < end) {
        const char *eol = strstr(p, "\r\n");
        if (eol == NULL)
            break;

        unsigned int chunk = 0;
        for (const char *q = p; q < eol; q++) {
            char c = *q;
            if (c >= '0' && c <= '9') chunk = chunk * 16 + (unsigned int)(c - '0');
            else if (c >= 'a' && c <= 'f') chunk = chunk * 16 + (unsigned int)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') chunk = chunk * 16 + (unsigned int)(c - 'A' + 10);
            else break; /* chunk extensions follow a ';' */
        }

        p = eol + 2;
        if (chunk == 0)
            break;

        if (o + chunk >= outcap)
            chunk = (unsigned int)(outcap - o - 1);
        memcpy(out + o, p, chunk);
        o += chunk;
        p += chunk + 2; /* data + trailing CRLF */
    }

    out[o] = '\0';
    return o;
}

int http_request(const char *url, PHTTP_DATA data) {
    char host[256];
    /* The getservercert pairing URL carries the hex-encoded client cert
     * (~2.3 KB hex for a 2048-bit key), so the path can be ~2.7 KB total. */
    char path[4096];
    int port;
    int is_https;

    if (data == NULL)
        return -1;

    parse_url(url, host, sizeof(host), &port, &is_https, path, sizeof(path));

    printf("[http] -> %s://%s:%d%s\n", is_https ? "https" : "http", host, port, is_https ? " (TLS)" : "");

    int sock = open_connection(host, port);
    if (sock < 0)
        return -1;

    TLS_IO io;
    int tls_up = 0;

    if (is_https) {
        if (tls_connect(sock, host, &g_ssl, &g_conf, &g_entropy, &g_ctr_drbg, &io) != 0) {
            net_close(sock);
            return -1;
        }
        tls_up = 1;
    }

    char req[8192];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Connection: close\r\n"
        "User-Agent: Moonlight-Wii\r\n"
        "\r\n",
        path, host, port);

    int ret = -1;
    char *buf = NULL;
    size_t cap = 16384;
    size_t total = 0;

    if (is_https) {
        if (mbedtls_ssl_write(&g_ssl, (const unsigned char*)req, req_len) != req_len) {
            tls_log_printf("[http] TLS write failed\n");
            goto done;
        }
    } else {
        if (net_send(sock, req, req_len, 0) != req_len) {
            printf("[http] send failed\n");
            goto done;
        }
    }

    buf = (char*)malloc(cap);
    if (buf == NULL)
        goto done;

    for (;;) {
        if (total + 4096 > cap) {
            cap *= 2;
            char *nb = (char*)realloc(buf, cap);
            if (nb == NULL)
                goto done;
            buf = nb;
        }

        int n;
        if (is_https) {
            n = mbedtls_ssl_read(&g_ssl, (unsigned char*)(buf + total), cap - total - 1);
            if (n < 0)
                tls_log_printf("[http] TLS read failed -0x%04x\n", -n);
        } else {
            n = net_recv(sock, buf + total, (int)(cap - total - 1), 0);
        }

        if (n <= 0)
            break;
        total += (size_t)n;
    }
    buf[total] = '\0';

    char *body = strstr(buf, "\r\n\r\n");
    if (body == NULL) {
        printf("[http] no HTTP response body (got %u bytes)\n", (unsigned)total);
        goto done;
    }
    body += 4;

    size_t bodylen = strlen(body);
    char *te = strstr(buf, "Transfer-Encoding:");
    int is_chunked = 0;
    if (te != NULL && te < body)
        is_chunked = strstr(te, "chunked") != NULL;

    if (is_chunked) {
        data->memory = (char*)malloc(bodylen + 1);
        if (data->memory == NULL)
            goto done;
        data->size = dechunk(body, bodylen, data->memory, bodylen + 1);
    } else {
        data->memory = (char*)malloc(bodylen + 1);
        if (data->memory == NULL)
            goto done;
        memcpy(data->memory, body, bodylen + 1);
        data->size = bodylen;
    }

    ret = 0;

done:
    free(buf);
    if (tls_up) {
        // Send a proper TLS close_notify before dropping the TCP connection.
        // This is polite shutdown; without it the server sees an abrupt close,
        // which is one thing that can leave Sunshine's HTTPS server wedged for
        // the next run (the "handshake hangs on the second session" problem).
        mbedtls_ssl_close_notify(&g_ssl);
        mbedtls_ssl_free(&g_ssl);
        mbedtls_ssl_config_free(&g_conf);
        mbedtls_entropy_free(&g_entropy);
        mbedtls_ctr_drbg_free(&g_ctr_drbg);
    }
    net_close(sock);
    return ret;
}
