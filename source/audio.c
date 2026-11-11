// Audio renderer for the Wii: Opus packets -> 16-bit PCM -> audio DMA.
//
// This drives the Wii's audio interface (AI) directly, the way most Wii
// emulators and media players do. There is no mixer in between: the hardware
// plays a block of raw 48 kHz, 16-bit, big-endian stereo samples from main
// memory, and raises an interrupt each time it moves on to the next block.
//
// Pipeline:
//   1. Decoder thread: each Opus packet (5 ms, 240 samples at 48 kHz) is
//      decoded to 16-bit PCM and written straight into the chunk being
//      assembled (CHUNK_BYTES, ~21 ms). When a chunk is full, its cache lines
//      are flushed (the hardware reads main memory, not the CPU cache) and it
//      becomes "ready".
//   2. DMA callback (interrupt context, once per block): queues the oldest
//      ready chunk as the next block with AUDIO_InitDMA(), or a silent chunk
//      if none is ready (an underrun). It copies nothing: interrupt handlers
//      on the Wii don't save the floating-point registers, and memcpy()/
//      memset() may use them.
//   3. Latency: at most MAX_READY chunks wait. Beyond that, incoming audio is
//      dropped, so delay can't build up.
//
// Channel order: the AI plays the two 16-bit samples of each frame as
// (right, left), so the decoder's (left, right) pairs are swapped on write.
//
// History: an earlier version used libogc's ASND mixer. ASND accepted every
// buffer and its voice timer advanced, but no stream audio was ever audible.
//
// Logging: the decoder side logs through app_log(). The callback only updates
// counters, reported in the periodic [audio] stats line.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <gccore.h>
#include <ogc/audio.h>
#include <opus/opus_multistream.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/cache.h>   /* DCFlushRange() */

#include "audio.h"
#include "applog.h"

#define AUDIO_RATE 48000
#define CHUNK_FRAMES 1024                 /* stereo frames per chunk (~21 ms) */
#define CHUNK_BYTES (CHUNK_FRAMES * 4)    /* 4096; must be a multiple of 32 */
#define CHUNK_MS (CHUNK_FRAMES * 1000 / AUDIO_RATE)
#define NUM_CHUNKS 12                     /* ring of chunks */
#define START_CHUNKS 3                    /* ready chunks before playback starts (~64 ms) */
#define MAX_READY 6                       /* latency cap (~128 ms); beyond it, drop audio */
#define STATS_INTERVAL_MS 5000

static OpusMSDecoder *g_dec = NULL;
static int g_channels = 0;
static int g_frame_samples = 0;       /* samples per channel per Opus frame */
static int16_t *g_pcm = NULL;         /* decoder output scratch */

/* Static buffers: part of the program image, so always in MEM1 and aligned. */
static uint8_t g_chunks[NUM_CHUNKS][CHUNK_BYTES] __attribute__((aligned(32)));
static uint8_t g_silence[CHUNK_BYTES] __attribute__((aligned(32)));

static int g_fill = 0;                /* bytes written into the chunk being assembled */
static int g_started = 0;
static volatile int g_running = 0;    /* 0 once stopped: the callback queues silence */


/* Chunk ring indices. g_cwr = chunk being assembled (written only by the
 * decoder thread). g_crd = next ready chunk to play (written only by the DMA
 * callback, and by the decoder thread at start, before the callback runs). */
static volatile int g_cwr = 0;
static volatile int g_crd = 0;

/* Written by the callback (interrupt context); read by the stats line. */
static volatile unsigned g_cb_calls = 0;      /* DMA interrupts */
static volatile unsigned g_cb_chunks = 0;     /* real chunks queued */
static volatile unsigned g_cb_underruns = 0;  /* silent chunks queued (nothing ready) */

/* Decoder-side counters. */
static unsigned g_packets = 0;
static unsigned g_frames = 0;         /* Opus frames written into chunks */
static unsigned g_latency_drops = 0;  /* frames dropped by the latency cap */
static unsigned g_decode_errors = 0;
static int g_peak = 0;                /* largest |sample| since the last stats line */
static uint64_t g_last_stats_ms = 0;

static uint64_t now_ms(void)
{
    return (uint64_t)ticks_to_millisecs(gettime());
}

static int chunks_ready(void)
{
    return (g_cwr - g_crd + NUM_CHUNKS) % NUM_CHUNKS;
}

static void stats_reset(void)
{
    g_packets = g_frames = g_latency_drops = g_decode_errors = 0;
    g_cb_calls = g_cb_chunks = g_cb_underruns = 0;
    g_peak = 0;
    g_last_stats_ms = now_ms();
}

static void stats_maybe_log(void)
{
    uint64_t now = now_ms();
    if (now - g_last_stats_ms < STATS_INTERVAL_MS)
        return;
    g_last_stats_ms = now;
    app_log("[audio] stats: packets=%u frames=%u latency_drops=%u decode_err=%u | "
            "dma_irqs=%u chunks_played=%u underruns=%u ready=%d (%d ms) peak=%d dma_left=%u\n",
            g_packets, g_frames, g_latency_drops, g_decode_errors,
            (unsigned)g_cb_calls, (unsigned)g_cb_chunks, (unsigned)g_cb_underruns,
            chunks_ready(), chunks_ready() * CHUNK_MS, g_peak,
            (unsigned)AUDIO_GetDMABytesLeft());
    g_peak = 0;
}

/* DMA callback: the hardware has just moved on to the block we queued last
 * time. Queue the block that follows it. Interrupt context: no logging, no
 * locks, no memcpy/memset. */
static void dma_callback(void)
{
    g_cb_calls++;
    if (g_running && chunks_ready() > 0) {
        AUDIO_InitDMA((u32)g_chunks[g_crd], CHUNK_BYTES);
        g_crd = (g_crd + 1) % NUM_CHUNKS;
        g_cb_chunks++;
    } else {
        AUDIO_InitDMA((u32)g_silence, CHUNK_BYTES);
        if (g_running)
            g_cb_underruns++;
    }
}

static int ar_init(int audioConfiguration, const POPUS_MULTISTREAM_CONFIGURATION opusConfig,
                   void *context, int arFlags)
{
    int err = 0;

    (void)context;

    if (opusConfig == NULL || opusConfig->channelCount <= 0 || opusConfig->samplesPerFrame <= 0) {
        app_log("[audio] init FAILED: bad opus config\n");
        return -1;
    }

    app_log("[audio] init: audioConfiguration=0x%x arFlags=0x%x sampleRate=%d channels=%d "
            "streams=%d coupled=%d samplesPerFrame=%d\n",
            audioConfiguration, arFlags, opusConfig->sampleRate, opusConfig->channelCount,
            opusConfig->streams, opusConfig->coupledStreams, opusConfig->samplesPerFrame);

    if (opusConfig->channelCount != 2 || opusConfig->sampleRate != AUDIO_RATE) {
        app_log("[audio] init FAILED: need 48000 Hz stereo (got %d Hz, %d channels)\n",
                opusConfig->sampleRate, opusConfig->channelCount);
        return -1;
    }

    g_channels = opusConfig->channelCount;
    g_frame_samples = opusConfig->samplesPerFrame;

    g_dec = opus_multistream_decoder_create(opusConfig->sampleRate, g_channels,
                                            opusConfig->streams, opusConfig->coupledStreams,
                                            opusConfig->mapping, &err);
    if (g_dec == NULL || err != OPUS_OK) {
        app_log("[audio] init FAILED: opus_multistream_decoder_create error %d\n", err);
        return -1;
    }

    g_pcm = (int16_t *)malloc(sizeof(int16_t) * g_frame_samples * g_channels);
    if (g_pcm == NULL) {
        app_log("[audio] init FAILED: out of memory\n");
        return -1;
    }

    memset(g_chunks, 0, sizeof(g_chunks));
    memset(g_silence, 0, sizeof(g_silence));
    DCFlushRange(g_chunks, sizeof(g_chunks));
    DCFlushRange(g_silence, sizeof(g_silence));

    g_cwr = g_crd = 0;
    g_fill = 0;
    g_started = 0;
    g_running = 0;
    stats_reset();

    AUDIO_Init(NULL);
    AUDIO_StopDMA();
    AUDIO_SetDSPSampleRate(AI_SAMPLERATE_48KHZ);
    AUDIO_RegisterDMACallback(dma_callback);

    app_log("[audio] init OK: direct audio DMA, %d Hz stereo 16-bit BE (R,L order), "
            "chunk=%d bytes (%d ms) at %p, ring=%d chunks, start after %d chunks, "
            "latency cap %d chunks\n",
            AUDIO_RATE, CHUNK_BYTES, CHUNK_MS, (void *)g_chunks, NUM_CHUNKS,
            START_CHUNKS, MAX_READY);
    return 0;
}

static void ar_start(void)
{
    app_log("[audio] start: decoder ready, waiting for first packet\n");
}

static void ar_stop(void)
{
    g_running = 0;
    AUDIO_StopDMA();
    AUDIO_RegisterDMACallback(NULL);
    g_started = 0;
    app_log("[audio] stop: packets=%u frames=%u latency_drops=%u decode_err=%u "
            "dma_irqs=%u chunks_played=%u underruns=%u\n",
            g_packets, g_frames, g_latency_drops, g_decode_errors,
            (unsigned)g_cb_calls, (unsigned)g_cb_chunks, (unsigned)g_cb_underruns);
}

static void ar_cleanup(void)
{
    g_running = 0;
    if (g_dec != NULL) {
        opus_multistream_decoder_destroy(g_dec);
        g_dec = NULL;
    }
    free(g_pcm);
    g_pcm = NULL;
    app_log("[audio] cleanup done\n");
}


/* Writes n decoded stereo frames into the chunk ring. Returns the number of
 * frames dropped by the latency cap. */
static int write_frames(int n)
{
    for (int i = 0; i < n; i++) {
        if (g_fill == 0 && chunks_ready() >= MAX_READY)
            return n - i;   /* too much audio waiting: drop the rest */

        int16_t l = g_pcm[2 * i];
        int16_t r = g_pcm[2 * i + 1];
        int a = l < 0 ? -l : l;
        if (a > g_peak) g_peak = a;

        /* The AI wants (right, left), each 16-bit big-endian. */
        uint8_t *d = g_chunks[g_cwr] + g_fill;
        d[0] = (uint8_t)((uint16_t)r >> 8);
        d[1] = (uint8_t)((uint16_t)r & 0xFF);
        d[2] = (uint8_t)((uint16_t)l >> 8);
        d[3] = (uint8_t)((uint16_t)l & 0xFF);
        g_fill += 4;

        if (g_fill >= CHUNK_BYTES) {
            /* Chunk complete: flush it so the hardware sees it, then publish. */
            DCFlushRange(g_chunks[g_cwr], CHUNK_BYTES);
            g_fill = 0;
            g_cwr = (g_cwr + 1) % NUM_CHUNKS;
        }
    }
    return 0;
}

static void ar_decode_and_play(char *sampleData, int sampleLength)
{
    if (g_dec == NULL || g_pcm == NULL)
        return;

    g_packets++;
    if (g_packets == 1)
        app_log("[audio] first packet received: %d bytes\n", sampleLength);

    /* sampleData == NULL is handled by opus (packet loss concealment). */
    int n = opus_multistream_decode(g_dec, (const unsigned char *)sampleData, sampleLength,
                                    g_pcm, g_frame_samples, 0);
    if (n <= 0) {
        g_decode_errors++;
        if (g_decode_errors <= 10 || g_decode_errors % 100 == 0)
            app_log("[audio] opus decode error %d (count %u, packet %d bytes)\n",
                    n, g_decode_errors, sampleLength);
        stats_maybe_log();
        return;
    }

    if (write_frames(n) > 0) {
        g_latency_drops++;
        if (g_latency_drops <= 10 || g_latency_drops % 100 == 0)
            app_log("[audio] latency cap reached (%d chunks ready), dropped audio (count %u)\n",
                    chunks_ready(), g_latency_drops);
    } else {
        g_frames++;
    }

    if (!g_started && chunks_ready() >= START_CHUNKS) {
        /* Start playback: first chunk now, second queued as the next block.
         * From then on the DMA callback queues one chunk per block. */
        uint8_t *first = g_chunks[g_crd];
        uint8_t *second = g_chunks[(g_crd + 1) % NUM_CHUNKS];
        g_crd = (g_crd + 2) % NUM_CHUNKS;
        g_running = 1;
        AUDIO_InitDMA((u32)first, CHUNK_BYTES);
        AUDIO_StartDMA();
        AUDIO_InitDMA((u32)second, CHUNK_BYTES);
        g_started = 1;
        app_log("[audio] playback started: first chunk %p bytes %02x %02x %02x %02x %02x %02x %02x %02x, "
                "%d chunks still ready\n", (void *)first,
                first[0], first[1], first[2], first[3], first[4], first[5], first[6], first[7],
                chunks_ready());
    }

    stats_maybe_log();
}

/* Called on the way out (HOME exit), before the app returns to the loader. Stops
 * the audio hardware so it no longer raises interrupts into this program. */
void audio_shutdown(void)
{
    g_running = 0;
    AUDIO_StopDMA();
    AUDIO_RegisterDMACallback(NULL);
    app_log("[audio] shutdown: hardware DMA stopped (dma_irqs=%u chunks_played=%u underruns=%u)\n",
            (unsigned)g_cb_calls, (unsigned)g_cb_chunks, (unsigned)g_cb_underruns);
}

void audio_init_callbacks(PAUDIO_RENDERER_CALLBACKS cb)
{
    LiInitializeAudioCallbacks(cb);
    cb->init = ar_init;
    cb->start = ar_start;
    cb->stop = ar_stop;
    cb->cleanup = ar_cleanup;
    cb->decodeAndPlaySample = ar_decode_and_play;
    cb->capabilities = 0;
}
