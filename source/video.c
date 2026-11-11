/*
 * Wii Moonlight — H.264 video decode path.
 *
 * Receives Annex-B H.264 decode units from the moonlight-common-c streaming core
 * and decodes them with FFmpeg's libavcodec (the MPlayer CE / WiiMC fork).
 * Decoded YUV420p frames are converted to RGB565 with libswscale and pushed
 * onto a small ring buffer; the UI thread pulls them with video_frame_get()
 * and blits them via GX.
 *
 * We run the GENERIC C DSP (HAVE_PAIRED=0). The WiiMC paired-single path was
 * never enabled in a shipped build (their config.h shipped HAVE_PAIRED=0), so
 * its ps_* and GQR code has never run on real Broadway hardware — enabling it here
 * crashed during avcodec_open2. Start with the portable C decoder and only
 * revisit the paired path once a working baseline exists.
 */

#include <gccore.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <malloc.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <ogc/lwp_watchdog.h>

#include "Limelight.h"
#include "video.h"
#include "applog.h"

#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

/* video.log is no longer written: the decoder thread must never touch the SD
 * card (see video_log below). Everything goes to moonlight.log instead. */

/* Number of decoded frames kept in flight (decoder thread -> UI thread). */
/* Display frame ring. The decoder writes slot w only when w+1 != read, so a
 * frame the UI is drawing (the one just before the read index) is never
 * overwritten. Capacity is VIDEO_NUM_BUFFERS-1 frames: 3 slots hold 2 frames,
 * which absorbs the bursts of frames that arrive together after a loss recovery
 * (2 slots held only 1, so the second frame of a burst was dropped). */
#define VIDEO_NUM_BUFFERS 3

/* Decoder-thread debug lines. These used to be written straight to video.log
 * with fopen/fclose from the decoder thread. On hardware the decoder thread
 * got stuck inside those SD card writes (video.log stops mid-line at "submit 2"),
 * so no frame after the first was ever decoded: no picture, endless "Video
 * decode unit queue overflow". Now they go to the in-memory app_log buffer,
 * which the low-priority log writer thread saves to moonlight.log. */
static void video_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    app_logv(fmt, ap);
    va_end(ap);
}

/* ---- decoder state ---- */
/* Counters for the periodic [video] stats line in moonlight.log. */
#define VIDEO_STATS_INTERVAL_MS 5000
static unsigned v_units = 0, v_idr = 0, v_frames = 0, v_decode_err = 0, v_queue_drops = 0;
static u64 v_display_ticks = 0, v_display_max = 0;
static unsigned v_display_n = 0;
static u64 v_decode_ticks = 0, v_convert_ticks = 0;
static u64 v_last_stats_ms = 0;

static void video_stats_reset(void)
{
    v_display_ticks = v_display_max = 0;
    v_display_n = 0;
    v_units = v_idr = v_frames = v_decode_err = v_queue_drops = 0;
    v_decode_ticks = v_convert_ticks = 0;
    v_last_stats_ms = (u64)ticks_to_millisecs(gettime());
}

static void video_stats_maybe_log(void)
{
    u64 now = (u64)ticks_to_millisecs(gettime());
    if (now - v_last_stats_ms < VIDEO_STATS_INTERVAL_MS)
        return;
    v_last_stats_ms = now;
    unsigned frames = v_frames ? v_frames : 1;
    app_log("[video] stats: display avg=%llu max=%llu ms (n=%u) | units=%u idr=%u decoded=%u decode_err=%u queue_drops=%u "
            "avg_decode=%llu ms avg_convert=%llu ms\n",
            (unsigned long long)(v_display_n ? ticks_to_millisecs(v_display_ticks) / v_display_n : 0),
            (unsigned long long)ticks_to_millisecs(v_display_max),
            v_display_n,
            v_units, v_idr, v_frames, v_decode_err, v_queue_drops,
            (unsigned long long)(ticks_to_millisecs(v_decode_ticks) / frames),
            (unsigned long long)(ticks_to_millisecs(v_convert_ticks) / frames));
}

/* Called by the stream loop (main.c) with the ticks spent drawing one UI frame. */
void video_note_display(u64 ticks)
{
    v_display_ticks += ticks;
    if (ticks > v_display_max)
        v_display_max = ticks;
    v_display_n++;
}

static AVCodec *g_codec = NULL;
static AVCodecContext *g_ctx = NULL;
static AVFrame *g_frame = NULL;
static uint8_t *g_bitstream = NULL;
static int g_bitstream_cap = 0;
static struct SwsContext *g_sws = NULL;

/* ---- frame ring buffer (RGB565, padded to power-of-two texture dims) ---- */
static uint16_t *g_frames[VIDEO_NUM_BUFFERS];
static int g_frame_w = 0, g_frame_h = 0;   /* actual video size */
static int g_tex_w = 0, g_tex_h = 0;       /* power-of-two texture size */
static int g_write = 0, g_read = 0;
static mutex_t g_queue_lock = LWP_MUTEX_NULL;

static int next_pow2(int v) {
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}

/* Combined YUV420p -> RGB565 conversion + 4x4 GX tile swizzle in ONE pass.
 * Hand-tuned inner loop: chroma deltas come from a 256-entry LUT (no multiplies),
 * and we iterate only the visible w x h pixels — the power-of-two padding is
 * never touched (the UI's UV sub-rect crops it out anyway). */
static void yuv420p_to_rgb565_tiled(const uint8_t *Yp, const uint8_t *Up, const uint8_t *Vp,
                                    int ys, int us, int vs, int w, int h,
                                    u16 *dst, int tw, int th)
{
    static int lutR[256], lutG1[256], lutG2[256], lutB[256];
    static int luts_done = 0;
    if (!luts_done) {
        for (int i = 0; i < 256; i++) {
            int c = i - 128;
            lutR[i]  = (359 * c) >> 8;
            lutG1[i] = (88 * c) >> 8;
            lutG2[i] = (183 * c) >> 8;
            lutB[i]  = (454 * c) >> 8;
        }
        luts_done = 1;
    }

    int tw4 = tw >> 2;              /* tiles per texture row */
    for (int ty = 0; ty < h >> 2; ty++) {
        int sy = ty << 2;
        for (int tx = 0; tx < w >> 2; tx++) {
            u16 *tile = dst + ((ty * tw4 + tx) << 4);
            int sx = tx << 2;
            int yb = sy * ys;
            int ub = (sy >> 1) * us;
            int vb = (sy >> 1) * vs;
            for (int y = 0; y < 4; y++) {
                const uint8_t *Yrow = Yp + yb + y * ys;
                const uint8_t *Urow = Up + ub + (y >> 1) * us;
                const uint8_t *Vrow = Vp + vb + (y >> 1) * vs;
                u16 *t = tile + (y << 2);
                for (int x = 0; x < 4; x++) {
                    int Y  = Yrow[sx + x];
                    int Cb = Urow[(sx + x) >> 1];
                    int Cr = Vrow[(sx + x) >> 1];
                    int R = Y + lutR[Cr];
                    int G = Y - lutG1[Cb] - lutG2[Cr];
                    int B = Y + lutB[Cb];
                    if (R < 0) R = 0; else if (R > 255) R = 255;
                    if (G < 0) G = 0; else if (G > 255) G = 255;
                    if (B < 0) B = 0; else if (B > 255) B = 255;
                    t[x] = (u16)(((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3));
                }
            }
        }
    }
}

/*
 * The paired-single GQRs (SPR 916-919) control how psq_l/psq_st treat their
 * operands. avcodec_open2 sets them on the MAIN thread, but the paired decode
 * runs on the SEPARATE decoder thread. Reset them after open2 (so the stream
 * threads see clean state) and re-apply them on the decoder thread before each
 * decode. This is the WiiMC pattern split correctly across our two threads.
 */
static void video_reset_gqr(void)
{
    register uint32_t gqr = 0;
    __asm__ volatile (
        "mtspr 916,%0\n\tmtspr 917,%0\n\tmtspr 918,%0\n\tmtspr 919,%0"
        : : "r"(gqr)
    );
}

static void video_setup_gqr(void)
{
    register uint32_t gqr;
    __asm__ volatile (
        "li %0,4\n\toris %0,%0,4\n\tmtspr 916,%0\n\t"
        "li %0,5\n\toris %0,%0,5\n\tmtspr 917,%0\n\t"
        "li %0,6\n\toris %0,%0,6\n\tmtspr 918,%0\n\t"
        "li %0,7\n\toris %0,%0,7\n\tmtspr 919,%0"
        : "=r"(gqr)
    );
}

static int video_setup(int videoFormat, int width, int height, int redrawRate,
                       void *context, int drFlags)
{
    if (videoFormat != VIDEO_FORMAT_H264) {
        video_log("[video] setup: bad format %d\n", videoFormat);
        return -1;
    }

    avcodec_register_all();

    g_codec = avcodec_find_decoder(CODEC_ID_H264);
    if (!g_codec) {
        video_log("[video] setup: H.264 decoder not found\n");
        return -1;
    }

    g_ctx = avcodec_alloc_context3(g_codec);
    if (!g_ctx) {
        video_log("[video] setup: alloc context failed\n");
        return -1;
    }

    g_ctx->width = width;
    g_ctx->height = height;
    g_ctx->pix_fmt = PIX_FMT_YUV420P;
    g_ctx->flags2 |= CODEC_FLAG2_FAST;
    /* Low delay: output each frame as soon as it is decoded instead of holding
     * frames back to reorder B-frames. This is the latency fix. */
    g_ctx->flags |= CODEC_FLAG_LOW_DELAY;
    /* Deblock filter: skipped entirely. Turning it on (AVDISCARD_NONREF) put heavy
     * vertical stripes over the picture on hardware, so keep the full skip. */
    g_ctx->skip_loop_filter = AVDISCARD_ALL;

    if (avcodec_open2(g_ctx, g_codec, NULL) < 0) {
        video_log("[video] setup: avcodec_open2 failed\n");
        return -1;
    }
    video_reset_gqr();   /* open2 set GQR4-7 on the main thread — reset them */

    g_frame = avcodec_alloc_frame();
    if (!g_frame) {
        video_log("[video] setup: alloc frame failed\n");
        return -1;
    }

    g_bitstream_cap = width * height * 3 / 2 + 1024;
    g_bitstream = memalign(32, g_bitstream_cap);
    if (!g_bitstream) {
        video_log("[video] setup: bitstream alloc failed\n");
        return -1;
    }

    g_frame_w = width;
    g_frame_h = height;
    g_tex_w = next_pow2(width);
    g_tex_h = next_pow2(height);
    for (int i = 0; i < VIDEO_NUM_BUFFERS; i++) {
        g_frames[i] = memalign(32, g_tex_w * g_tex_h * 2);
        if (g_frames[i])   /* black padding, in case filtering ever touches it */
            memset(g_frames[i], 0, g_tex_w * g_tex_h * 2);
        if (!g_frames[i]) {
            video_log("[video] setup: frame buffer %d alloc failed\n", i);
            return -1;
        }
    }
    g_write = g_read = 0;

    g_sws = sws_getContext(width, height, PIX_FMT_YUV420P,
                           width, height, PIX_FMT_RGB565,
                           SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!g_sws) {
        video_log("[video] setup: sws_getContext failed\n");
        return -1;
    }

    if (g_queue_lock == LWP_MUTEX_NULL)
        LWP_MutexInit(&g_queue_lock, FALSE);

    video_log("[video] setup: %dx%d@%d ok (generic C)\n", width, height, redrawRate);
    video_stats_reset();
    app_log("[video] setup OK: %dx%d@%d H.264, decoder=generic C, low_delay=on, "
            "deblock=%s, frame buffers=%d x %dx%d (16-bit), sws=%s\n",
            width, height, redrawRate,
            g_ctx->skip_loop_filter == AVDISCARD_ALL ? "off" : "on",
            VIDEO_NUM_BUFFERS, g_tex_w, g_tex_h,
            "unused (custom YUV->RGB565 path)");
    return 0;
}

static void video_start(void) {
    video_log("[video] start\n");
    app_log("[video] start: waiting for the first frame\n");
}

static void video_stop(void) {
    video_log("[video] stop (w=%d r=%d)\n", g_write, g_read);
    app_log("[video] stop: units=%u idr=%u decoded=%u decode_err=%u queue_drops=%u\n",
            v_units, v_idr, v_frames, v_decode_err, v_queue_drops);
}

static void video_cleanup(void) {
    video_log("[video] cleanup\n");
    app_log("[video] cleanup\n");
    if (g_ctx) {
        avcodec_close(g_ctx);
        av_free(g_ctx);   /* libavcodec 54 has no avcodec_free_context */
        g_ctx = NULL;
    }
    if (g_frame) {
        av_free(g_frame);
        g_frame = NULL;
    }
    if (g_sws) {
        sws_freeContext(g_sws);
        g_sws = NULL;
    }
    if (g_bitstream) {
        free(g_bitstream);
        g_bitstream = NULL;
    }
    for (int i = 0; i < VIDEO_NUM_BUFFERS; i++) {
        if (g_frames[i]) {
            free(g_frames[i]);
            g_frames[i] = NULL;
        }
    }
}

static int video_submit_decode_unit(PDECODE_UNIT du)
{
    static int submit_count = 0;

    submit_count++;
    v_units++;
    if (du->frameType == FRAME_TYPE_IDR) {
        v_idr++;
        app_log("[video] IDR frame received (unit %u, %d bytes)\n", v_units, du->fullLength);
    }
    if (submit_count <= 3)
        video_log("[video] submit %d: enter\n", submit_count);

    int length = 0;
    PLENTRY entry = du->bufferList;
    while (entry != NULL) {
        if (length + entry->length > g_bitstream_cap) {
            video_log("[video] bitstream overflow\n");
            return DR_NEED_IDR;
        }
        memcpy(g_bitstream + length, entry->data, entry->length);
        length += entry->length;
        entry = entry->next;
    }

    if (submit_count <= 3) {
        /* One log call per line: app_log timestamps every call. */
        char hex[33];
        int n = 0;
        for (int i = 0; i < length && i < 16; i++)
            n += snprintf(hex + n, sizeof(hex) - n, "%02x", g_bitstream[i]);
        hex[n] = '\0';
        video_log("[video] submit %d: len=%d first16=%s\n", submit_count, length, hex);
    }

    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = g_bitstream;
    pkt.size = length;

    /* Re-apply the paired-single GQRs on THIS (decoder) thread before decode. */
    video_setup_gqr();

    int got = 0;
    u64 t0 = gettick();
    int ret = avcodec_decode_video2(g_ctx, g_frame, &got, &pkt);
    u64 t1 = gettick();
    if ((s64)(t1 - t0) > 0 && (s64)(t1 - t0) < 1000000000LL)
        v_decode_ticks += t1 - t0;
    if (submit_count <= 3) {
        video_log("[video] decode done: ret=%d got=%d\n", ret, got);
        video_log("[video]   decode %d ms\n", (int)ticks_to_millisecs(t1 - t0));
    }
    if (ret < 0) {
        v_decode_err++;
        app_log("[video] decode error %d (count %u) -> requesting IDR\n", ret, v_decode_err);
        video_stats_maybe_log();
        return DR_NEED_IDR;
    }

    if (got) {
        LWP_MutexLock(g_queue_lock);

        int next = (g_write + 1) % VIDEO_NUM_BUFFERS;
        if (next != g_read) {
            u64 t2 = gettick();
            yuv420p_to_rgb565_tiled(g_frame->data[0], g_frame->data[1], g_frame->data[2],
                                    g_frame->linesize[0], g_frame->linesize[1], g_frame->linesize[2],
                                    g_frame_w, g_frame_h,
                                    g_frames[g_write], g_tex_w, g_tex_h);
            u64 t3 = gettick();
            if ((s64)(t3 - t2) > 0 && (s64)(t3 - t2) < 1000000000LL)
                v_convert_ticks += t3 - t2;
            v_frames++;
            if (submit_count <= 3) {
                video_log("[video] sws done\n");
                video_log("[video]   convert %d ms\n", (int)ticks_to_millisecs(t3 - t2));
            }
            g_write = next;
        } else {
            // Display is behind: the frame queue is full, so this frame is dropped.
            v_queue_drops++;
            if (v_queue_drops <= 10 || v_queue_drops % 100 == 0)
                app_log("[video] display queue full, dropped decoded frame (count %u)\n",
                        v_queue_drops);
        }

        LWP_MutexUnlock(g_queue_lock);
    }

    video_stats_maybe_log();
    return DR_OK;
}

/* UI thread pulls the next decoded frame (returns 1 and sets *rgb_out if one is
 * ready, else 0). w/h are the visible video size; tex_w/tex_h are the padded
 * power-of-two texture dimensions. */
int video_frame_get(u16 **rgb_out, int *w_out, int *h_out, int *tex_w_out, int *tex_h_out)
{
    int have = 0;
    LWP_MutexLock(g_queue_lock);
    if (g_write != g_read) {
        *rgb_out = g_frames[g_read];
        /* The converter works in whole 4x4 tiles, so only the first
         * (w & ~3) columns / (h & ~3) rows are ever written. At 854x480 the
         * last 2 columns would be uninitialised memory (the flickering line
         * at the right edge), so report only the part that holds picture. */
        *w_out = g_frame_w & ~3;
        *h_out = g_frame_h & ~3;
        *tex_w_out = g_tex_w;
        *tex_h_out = g_tex_h;
        g_read = (g_read + 1) % VIDEO_NUM_BUFFERS;
        have = 1;
    }
    LWP_MutexUnlock(g_queue_lock);
    return have;
}

void video_init_callbacks(PDECODER_RENDERER_CALLBACKS cb)
{
    LiInitializeVideoCallbacks(cb);
    cb->setup = video_setup;
    cb->start = video_start;
    cb->stop = video_stop;
    cb->cleanup = video_cleanup;
    cb->submitDecodeUnit = video_submit_decode_unit;
    cb->capabilities = 0;
}
