// Entropy for the Wii: Starlet round-trip timing jitter -> SHA-256 -> CTR_DRBG.
// See entropy.h for the idea. Logging: the self-test result, the seeding time,
// and any failure, all as "[entropy] ..." lines in moonlight.log.

#include <string.h>
#include <math.h>

#include <gccore.h>
#include <ogc/es.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/mutex.h>
#include <network.h>

#include <mbedtls/sha256.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>

#include "entropy.h"
#include "applog.h"

#define SELFTEST_SAMPLES 1024
#define MIN_SAMPLES_PER_BLOCK 512     /* floor: 2 samples per output bit */
#define MAX_SAMPLES_PER_BLOCK 8192    /* ceiling, used if the jitter is poor */
#define BLOCK_BITS 256                /* one SHA-256 output */

static int g_samples_per_block = MAX_SAMPLES_PER_BLOCK;
static int g_ready = 0;
static mutex_t g_lock = LWP_MUTEX_NULL;
static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_drbg;

/* One timing sample: the length of a round trip to IOS on the Starlet.
 * ES_GetDeviceID is a cheap, harmless request (it reads the console ID). */
static u32 jitter_sample(void)
{
    u32 id;
    u64 t0 = gettime();
    ES_GetDeviceID(&id);
    u64 t1 = gettime();
    return (u32)(t1 - t0);
}

/* Fills out[0..31] with SHA-256 over g_samples_per_block timing samples. Each
 * sample's full value and the running timebase are hashed, so nothing is lost
 * by throwing away the high bits ourselves. */
static void jitter_block(unsigned char out[32])
{
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    for (int i = 0; i < g_samples_per_block; i++) {
        u32 d = jitter_sample();
        u64 now = gettime();
        mbedtls_sha256_update(&sha, (const unsigned char *)&d, sizeof(d));
        mbedtls_sha256_update(&sha, (const unsigned char *)&now, sizeof(now));
    }
    mbedtls_sha256_finish(&sha, out);
    mbedtls_sha256_free(&sha);
}

/* mbedtls entropy source (also used directly by http.c's TLS setup). */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    (void)data;
    size_t done = 0;
    unsigned char block[32];
    while (done < len) {
        jitter_block(block);
        size_t n = len - done < sizeof(block) ? len - done : sizeof(block);
        memcpy(output + done, block, n);
        done += n;
    }
    memset(block, 0, sizeof(block));
    *olen = len;
    return 0;
}

/* Measures the jitter and picks how many samples to hash per 256-bit block.
 * Estimate: Shannon entropy of the lowest 8 bits of the round-trip time.
 * Credit: only half of it (a safety margin), so the block gets at least 256
 * bits by that conservative estimate. */
static void jitter_selftest(void)
{
    static u16 hist[256];
    memset(hist, 0, sizeof(hist));
    u32 mn = 0xFFFFFFFF, mx = 0, prev = 0;
    int repeat = 0, max_repeat = 0, distinct = 0;
    u64 t0 = gettime();
    for (int i = 0; i < SELFTEST_SAMPLES; i++) {
        u32 d = jitter_sample();
        if (d < mn) mn = d;
        if (d > mx) mx = d;
        if (i > 0 && d == prev) {
            repeat++;
            if (repeat > max_repeat) max_repeat = repeat;
        } else {
            repeat = 0;
        }
        prev = d;
        if (hist[d & 0xFF]++ == 0)
            distinct++;
    }
    u64 t1 = gettime();

    double h = 0.0;
    for (int i = 0; i < 256; i++) {
        if (hist[i]) {
            double p = (double)hist[i] / SELFTEST_SAMPLES;
            h -= p * log2(p);
        }
    }
    double credit = h / 2.0;              /* bits counted per sample */
    if (credit < 0.03125) credit = 0.03125;
    int spb = (int)ceil(BLOCK_BITS / credit);
    if (spb < MIN_SAMPLES_PER_BLOCK) spb = MIN_SAMPLES_PER_BLOCK;
    if (spb > MAX_SAMPLES_PER_BLOCK) spb = MAX_SAMPLES_PER_BLOCK;
    g_samples_per_block = spb;

    int us = (int)ticks_to_microsecs(t1 - t0);
    app_log("[entropy] self-test: %d IOS round trips in %d ms, %u..%u ticks, %d distinct low bytes, "
            "longest repeat %d, low-byte entropy %.2f bits/sample (crediting %.2f) -> %d samples per 256-bit block\n",
            SELFTEST_SAMPLES, us / 1000, (unsigned)mn, (unsigned)mx, distinct, max_repeat, h, credit, spb);
    if (h < 1.0)
        app_log("[entropy] WARNING: very little timing jitter measured; using the maximum sample count\n");
}

int entropy_init(void)
{
    if (g_ready)
        return 0;
    if (g_lock == LWP_MUTEX_NULL)
        LWP_MutexInit(&g_lock, false);

    jitter_selftest();

    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_drbg);
    int r = mbedtls_entropy_add_source(&g_entropy, mbedtls_hardware_poll, NULL, 32,
                                       MBEDTLS_ENTROPY_SOURCE_STRONG);
    if (r != 0) {
        app_log("[entropy] ERROR: add_source failed -0x%04x\n", -r);
        return -1;
    }

    /* Uncounted extras: unique or changing, but not secret. They only make two
     * consoles that happen to collect identical jitter end up different. */
    struct {
        u32 device_id;
        u32 ip;
        u64 now;
    } extra;
    extra.device_id = 0;
    ES_GetDeviceID(&extra.device_id);
    extra.ip = net_gethostip();
    extra.now = gettime();
    mbedtls_entropy_update_manual(&g_entropy, (const unsigned char *)&extra, sizeof(extra));

    static const char pers[] = "wii-moonlight entropy v2";
    u64 t0 = gettime();
    r = mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy,
                              (const unsigned char *)pers, sizeof(pers) - 1);
    u64 t1 = gettime();
    if (r != 0) {
        app_log("[entropy] ERROR: ctr_drbg_seed failed -0x%04x\n", -r);
        return -1;
    }
    /* Pull fresh jitter into the generator every 64 requests. */
    mbedtls_ctr_drbg_set_reseed_interval(&g_drbg, 64);
    g_ready = 1;
    app_log("[entropy] generator seeded in %d ms (CTR_DRBG/AES-256, reseed every 64 requests)\n",
            (int)ticks_to_millisecs(t1 - t0));
    return 0;
}

int entropy_random(void *ctx, unsigned char *out, size_t len)
{
    (void)ctx;
    if (!g_ready && entropy_init() != 0)
        return -1;
    LWP_MutexLock(g_lock);
    int r = 0;
    while (len > 0 && r == 0) {
        size_t n = len > MBEDTLS_CTR_DRBG_MAX_REQUEST ? MBEDTLS_CTR_DRBG_MAX_REQUEST : len;
        r = mbedtls_ctr_drbg_random(&g_drbg, out, n);
        out += n;
        len -= n;
    }
    LWP_MutexUnlock(g_lock);
    if (r != 0)
        app_log("[entropy] ERROR: random failed -0x%04x\n", -r);
    return r;
}

void entropy_mix(const void *data, size_t len)
{
    if (!g_ready || len == 0)
        return;
    LWP_MutexLock(g_lock);
    mbedtls_ctr_drbg_update(&g_drbg, (const unsigned char *)data, len);
    LWP_MutexUnlock(g_lock);
}
