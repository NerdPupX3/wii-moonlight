#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <gccore.h>
#include <network.h>
#include <fat.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>
#include <ogc/conf.h>
#include <ogc/mutex.h>
#include <mbedtls/platform.h>
#include "client.h"
#include "errors.h"
#include "ui.h"
#include "video.h"
#include "audio.h"
#include "input.h"
#include "applog.h"
#include "entropy.h"
#include "hosts.h"
#include "menu.h"

// libogc's BSD socket() API (used by moonlight-common-c's streaming core and
// enet) routes through a "soc:" device registered by socInit(). The installed
// libogc build does NOT call this from net_init(), so it must be done
// explicitly before any socket() call.
extern int socInit(void);

// --- LOGGING: everything goes to a log file on the SD card ---
//
// Design: writing to the SD card is slow (tens of milliseconds, more for the
// close+reopen that makes libfat record the file size). The streaming threads
// must never wait for that, or packets get dropped. So:
//   - app_log()/cl_log() format the line and copy it into a memory buffer
//     (g_logbuf) under a short lock. That is all a streaming thread ever does.
//   - A low-priority writer thread wakes up every LOG_WRITER_PERIOD_MS, takes
//     the buffered text, and writes it to the SD card outside that lock.
//   - printf() in this file (control-plane messages on the main thread) and the
//     HOME-exit path flush synchronously, so those lines survive a crash.
// Every line is prefixed with "[seconds.millis]" since boot.
#define LOG_PATH "sd:/apps/wii-moonlight/moonlight.log"
#define LOGBUF_SIZE (64 * 1024)
#define LOG_LINE_MAX 512
#define LOG_WRITER_PERIOD_MS 1000
#define LOG_WRITER_PRIORITY 20      /* below the streaming threads (0x50) and main */

static FILE *g_log = NULL;
static mutex_t g_log_mutex = LWP_MUTEX_NULL;    // protects g_logbuf, g_loglen, g_log_lost
static mutex_t g_file_mutex = LWP_MUTEX_NULL;   // protects g_log and g_logout
static char g_logbuf[LOGBUF_SIZE];
static char g_logout[LOGBUF_SIZE];
static int g_loglen = 0;
static unsigned g_log_lost = 0;                 // lines dropped because the buffer was full
static volatile int g_log_ready = 0;
static lwp_t g_log_thread = LWP_THREAD_NULL;

// Stream resolution: 360p by default. 480p is opt-in, behind a warning screen.
static int g_use_480 = 0;
// Set during HOME-exit teardown: write every line to the SD card immediately,
// so the last line before a crash is on the card.
static volatile int g_sync_every_line = 0;

// Moves the buffered text to the SD card. Slow; never call from a streaming thread
// (except during teardown). libfat only records a file's size when the file is
// CLOSED, so we close and reopen in append mode; fflush alone would leave the
// file looking empty after a crash.
static void log_flush_to_sd(void) {
    if (!g_log_ready)
        return;
    LWP_MutexLock(g_file_mutex);

    LWP_MutexLock(g_log_mutex);
    int n = g_loglen;
    unsigned lost = g_log_lost;
    if (n > 0)
        memcpy(g_logout, g_logbuf, n);
    g_loglen = 0;
    g_log_lost = 0;
    LWP_MutexUnlock(g_log_mutex);

    if (g_log && (n > 0 || lost > 0)) {
        if (n > 0)
            fwrite(g_logout, 1, n, g_log);
        if (lost > 0)
            fprintf(g_log, "[log] %u lines lost: the log buffer was full\n", lost);
        fflush(g_log);
        fclose(g_log);
        g_log = fopen(LOG_PATH, "a");
    }
    LWP_MutexUnlock(g_file_mutex);
}

// Formats one timestamped line and appends it to the memory buffer. Fast.
static void log_append(const char *fmt, va_list ap) {
    char line[LOG_LINE_MAX];
    if (!g_log_ready)
        return;
    unsigned long long ms = (unsigned long long)ticks_to_millisecs(gettime());
    int n = snprintf(line, sizeof(line), "[%llu.%03llu] ", ms / 1000, ms % 1000);
    int m = vsnprintf(line + n, sizeof(line) - n, fmt, ap);
    if (m < 0)
        m = 0;
    n += m;
    if (n >= (int)sizeof(line)) {       // truncated: keep it one line
        n = sizeof(line) - 1;
        line[n - 1] = '\n';
    }

    LWP_MutexLock(g_log_mutex);
    if (g_loglen + n <= LOGBUF_SIZE) {
        memcpy(g_logbuf + g_loglen, line, n);
        g_loglen += n;
    } else {
        g_log_lost++;
    }
    LWP_MutexUnlock(g_log_mutex);
}

static volatile int g_log_writer_stop = 0;

static void *log_writer_thread(void *arg) {
    (void)arg;
    while (!g_log_writer_stop) {
        usleep(LOG_WRITER_PERIOD_MS * 1000);
        if (g_loglen > 0 || g_log_lost > 0)
            log_flush_to_sd();
    }
    return NULL;
}

// Stops the background writer and writes everything that is buffered. Called
// on the way out, so no thread of ours is left running when the loader takes over.
static void log_shutdown(void) {
    if (!g_log_ready)
        return;
    g_log_writer_stop = 1;
    LWP_JoinThread(g_log_thread, NULL);
    g_log_thread = LWP_THREAD_NULL;
    log_flush_to_sd();
}

static void log_open(void) {
    LWP_MutexInit(&g_log_mutex, false);
    LWP_MutexInit(&g_file_mutex, false);
    g_log = fopen(LOG_PATH, "w");
    if (!g_log) {
        printf("WARNING: can't open log file %s\n", LOG_PATH);
    } else {
        fputs("=== BOOT ===\n", g_log);
        fflush(g_log);
        fclose(g_log);
        g_log = fopen(LOG_PATH, "a");
    }
    g_log_ready = 1;
    LWP_CreateThread(&g_log_thread, log_writer_thread, NULL, NULL, 32 * 1024, LOG_WRITER_PRIORITY);
}

// Control-plane messages (main thread): written to the SD card right away.
static void log_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_append(fmt, args);
    va_end(args);
    log_flush_to_sd();
}

// Writes all buffered lines to the SD card now.
void app_log_sync(void) {
    log_flush_to_sd();
}

// Shared logger for the other source files (audio.c, video.c, enet, ...) and
// the streaming library's own messages. Buffered: safe to call from any thread
// (but NOT from interrupt context).
void app_logv(const char *fmt, va_list ap) {
    log_append(fmt, ap);
    if (g_sync_every_line)
        log_flush_to_sd();
}

void app_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    app_logv(fmt, args);
    va_end(args);
}

// Route all remaining printf() output in this file to the log.
#define printf log_printf

static void log_flush(void) {
    log_flush_to_sd();
}

// mbedtls_hardware_poll (the entropy source) lives in entropy.c.

mbedtls_ms_time_t mbedtls_ms_time(void) {
    return (mbedtls_ms_time_t)ticks_to_millisecs(gettime());
}

// --- CONFIGURATION ---
// The host is chosen at startup (hostmenu.c); the pairing PIN is random.
#define KEY_DIR "sd:/apps/wii-moonlight"

// --- Streaming connection listener (logs progress to the backend log) ---
// --- Host feedback callbacks. The Wii has no rumble motor or LED to drive yet,
// so these only log what the host asked for. They must exist (see above). ---
static void cl_rumble(uint16_t controllerNumber, uint16_t low, uint16_t high) {
    app_log("[host] rumble request: controller %u low=%u high=%u (ignored)\n",
            (unsigned)controllerNumber, (unsigned)low, (unsigned)high);
}
static void cl_rumble_triggers(uint16_t controllerNumber, uint16_t left, uint16_t right) {
    app_log("[host] trigger rumble: controller %u left=%u right=%u (ignored)\n",
            (unsigned)controllerNumber, (unsigned)left, (unsigned)right);
}
static void cl_set_motion_state(uint16_t controllerNumber, uint8_t motionType, uint16_t reportRateHz) {
    app_log("[host] motion state request: controller %u type %u rate %u Hz\n",
            (unsigned)controllerNumber, (unsigned)motionType, (unsigned)reportRateHz);
}
static void cl_set_led(uint16_t controllerNumber, uint8_t r, uint8_t g, uint8_t b) {
    app_log("[host] LED request: controller %u rgb=(%u,%u,%u) (ignored)\n",
            (unsigned)controllerNumber, (unsigned)r, (unsigned)g, (unsigned)b);
}
static void cl_set_hdr(bool enabled) {
    app_log("[host] HDR request: %d (ignored)\n", (int)enabled);
}
static void cl_set_adaptive_triggers(uint16_t controllerNumber, uint8_t eventFlags, uint8_t typeLeft,
                                     uint8_t typeRight, uint8_t *left, uint8_t *right) {
    (void)left; (void)right;
    app_log("[host] adaptive triggers request: controller %u flags=%u (ignored)\n",
            (unsigned)controllerNumber, (unsigned)eventFlags);
    (void)typeLeft; (void)typeRight;
}
static void cl_connection_status(int status) {
    app_log("[host] connection status: %d\n", status);
}

static void cl_log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    app_logv(format, args);
    va_end(args);
}
static void cl_stage_starting(int stage) { printf("  [%s] ...\n", LiGetStageName(stage)); }
static void cl_stage_complete(int stage) { printf("  [%s] done\n", LiGetStageName(stage)); }
static void cl_stage_failed(int stage, int code) { printf("  [%s] FAILED (code %d)\n", LiGetStageName(stage), code); }
static void cl_started(void) { printf("\nSTREAM CONNECTED. Host is streaming.\n"); }
static void cl_terminated(int code) { printf("\nSTREAM ENDED (code %d).\n", code); }

// --- Menu rendering helpers ---
// Room kept free at the right of the top bar for render_top_bar_right.
#define TOP_BAR_RIGHT_W 120

void render_top_bar(const char *title) {
    ui_fill(UI_BLACK);
    ui_rect(0, 0, ui_screen_width(), TOP_BAR_H, UI_BLUE);
    ui_text_clip(UI_MARGIN, 12, title, UI_WHITE, 2,
                 ui_screen_width() - 2 * UI_MARGIN - TOP_BAR_RIGHT_W);
}

void render_top_bar_right(const char *text) {
    // Vertically centred on the title's capital letters.
    ui_text(ui_screen_width() - UI_MARGIN - ui_text_width(text, 1), 20, text, UI_WHITE, 1);
}

static void draw_line(int x, int y, const char *s, int n, unsigned int color) {
    char line[256];
    if (n > (int)sizeof(line) - 1)
        n = (int)sizeof(line) - 1;
    memcpy(line, s, n);
    line[n] = '\0';
    ui_text(x, y, line, color, 1);
}

// Word wrap by measured pixel width (the font is proportional, so counting
// characters can't tell whether a line fits). A word longer than a whole line
// is split wherever it has to be.
int draw_wrapped(int x, int y, const char *s, unsigned int color, int maxw) {
    if (maxw <= 0)
        maxw = ui_screen_width() - UI_MARGIN - x;
    const int space_w = ui_text_width(" ", 1);
    const char *line = NULL;   // start of the current line in s
    int line_n = 0, line_w = 0;
    const char *p = s;

    while (*p) {
        if (*p == '\n') {
            if (line) draw_line(x, y, line, line_n, color);
            y += WRAP_LINE_H;
            line = NULL; line_n = line_w = 0;
            p++;
            continue;
        }
        if (*p == ' ') { p++; continue; }

        int wl = 0;
        while (p[wl] && p[wl] != ' ' && p[wl] != '\n')
            wl++;
        int ww = ui_text_width_n(p, wl, 1);

        if (line && line_w + space_w + ww > maxw) {   // doesn't fit: new line
            draw_line(x, y, line, line_n, color);
            y += WRAP_LINE_H;
            line = NULL; line_n = line_w = 0;
        }
        while (!line && ww > maxw && wl > 1) {        // longer than a whole line
            int k = wl;
            while (k > 1 && ui_text_width_n(p, k, 1) > maxw)
                k--;
            draw_line(x, y, p, k, color);
            y += WRAP_LINE_H;
            p += k; wl -= k;
            ww = ui_text_width_n(p, wl, 1);
        }
        if (line) {
            // Extend the current line; the text between is the original spaces.
            line_n = (int)(p + wl - line);
            line_w += space_w + ww;
        } else {
            line = p; line_n = wl; line_w = ww;
        }
        p += wl;
    }
    if (line) {
        draw_line(x, y, line, line_n, color);
        y += WRAP_LINE_H;
    }
    return y;
}

static void render_message(const char *title, const char *msg) {
    render_top_bar(title);
    draw_wrapped(UI_MARGIN, CONTENT_Y + 8, msg, UI_WHITE, 0);
}

static void render_list(const char *title, char **names, int count, int sel, int top) {
    render_top_bar(title);
    render_top_bar_right(g_use_480 ? "480p" : "360p");

    int item_h = LIST_ITEM_H;
    int maxw = ui_screen_width() - 2 * UI_MARGIN;
    int visible = LIST_VISIBLE(0);
    int end = top + visible;
    if (end > count) end = count;

    int y = CONTENT_Y;
    for (int i = top; i < end; i++) {
        u32 fg = (i == sel) ? UI_BLACK : UI_WHITE;
        if (i == sel)
            ui_rect(0, y - 5, ui_screen_width(), item_h, UI_LIGHTBLU);
        // One-line names are centred in the row; long names wrap onto a
        // second line at a word boundary.
        if (ui_text_width(names[i], 1) <= maxw)
            ui_text(UI_MARGIN, y + 10, names[i], fg, 1);
        else
            draw_wrapped(UI_MARGIN, y, names[i], fg, maxw);
        y += item_h;
    }

    if (end < count)
        ui_text(UI_MARGIN, y, "...", UI_GREY, 1);

    render_footer("[A] launch    [+] resolution    [HOME] exit");
}

// Warning shown before switching to 480p. A = keep 480p, B = back to 360p.
// Returns 1 for 480p, 0 for 360p. Returns to the app list either way.
static int resolution_warning(void) {
    while (1) {
        ui_begin_frame();
        WPAD_ScanPads();
        u32 down = WPAD_ButtonsDown(0);

        if (down & WPAD_BUTTON_A) {
            ui_end_frame();
            return 1;
        }
        if (down & WPAD_BUTTON_B) {
            ui_end_frame();
            return 0;
        }

        render_top_bar("480p warning");
        int y = draw_wrapped(UI_MARGIN, CONTENT_Y + 8,
            "480p needs a stable connection, or the picture will freeze.\n\n"
            "For the best result, connect the Wii with a wired (Ethernet) cable.",
            UI_WHITE, 0);
        draw_wrapped(UI_MARGIN, y + 20,
            "Over Wi-Fi you may get short freezes.",
            UI_GREY, 0);

        render_footer("[A] use 480p    [B] back to 360p");
        ui_end_frame();
    }
}

void render_footer(const char *text) {
    int h = ui_screen_height();
    ui_rect(0, h - FOOTER_H, ui_screen_width(), FOOTER_H, UI_DARKGREY);
    ui_text_clip(UI_MARGIN, h - FOOTER_H + 9, text, UI_RGBA(190, 190, 190, 255), 1,
                 ui_screen_width() - 2 * UI_MARGIN);
}

int message_choice(const char *title, const char *msg, const char *footer) {
    while (1) {
        ui_begin_frame();
        WPAD_ScanPads();
        u32 down = WPAD_ButtonsDown(0);
        int c = (down & WPAD_BUTTON_A) ? 'A' : (down & WPAD_BUTTON_B) ? 'B' :
                (down & WPAD_BUTTON_HOME) ? 'H' : 0;
        render_message(title, msg);
        render_footer(footer ? footer : "[A] OK    [B] back    [HOME] exit");
        ui_end_frame();
        if (c)
            return c;
    }
}

static void error_screen(const char *title, const char *msg) {
    while (1) {
        ui_begin_frame();
        WPAD_ScanPads();
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) {
            ui_end_frame();
            return;
        }
        render_message(title, msg);
        render_footer("[HOME] exit");
        ui_end_frame();
    }
}

// --- Leaving the app cleanly ---
//
// Returning to the Homebrew Channel used to hang on a red/black garbage
// screen. The cause: the app left while the streaming library's threads
// (video, audio, control, input) were still running and still had network
// requests open with IOS. While libogc shuts the system down on exit, it
// switches to those threads, and they use hardware that is being turned off.
// Now the stream is stopped properly first (LiStopConnection), and the
// controllers, network and video are shut down before handing back.

// 1 while the streaming library has threads running.
static volatile int g_stream_active = 0;
static volatile int g_stop_done = 0;

static void *stop_connection_thread(void *arg) {
    (void)arg;
    LiStopConnection();
    g_stop_done = 1;
    return NULL;
}

// Runs LiStopConnection on a helper thread and waits at most timeout_ms, so a
// hang inside it can't freeze the exit. Returns 1 if the stream stopped.
static int stop_connection_timed(int timeout_ms) {
    static u8 stack[64 * 1024] __attribute__((aligned(32)));
    lwp_t t = LWP_THREAD_NULL;
    g_stop_done = 0;
    app_log("[exit] stopping the stream (LiStopConnection)...\n");
    u64 t0 = gettime();
    if (LWP_CreateThread(&t, stop_connection_thread, NULL, stack, sizeof(stack), 0x50) != 0) {
        app_log("[exit] ERROR: can't create the stop thread; leaving the stream running\n");
        return 0;
    }
    while (!g_stop_done && ticks_to_millisecs(gettime() - t0) < (u64)timeout_ms)
        usleep(10 * 1000);
    unsigned ms = (unsigned)ticks_to_millisecs(gettime() - t0);
    if (!g_stop_done) {
        app_log("[exit] WARNING: LiStopConnection still running after %u ms; giving up on it\n", ms);
        return 0;
    }
    LWP_JoinThread(t, NULL);
    g_stream_active = 0;
    app_log("[exit] stream stopped in %u ms\n", ms);
    return 1;
}

// Registered with atexit(): runs on every way out of main().
static void app_shutdown(void) {
    g_sync_every_line = 1;
    app_log("[exit] shutting down (stream threads %s)\n",
            g_stream_active ? "STILL RUNNING" : "stopped");
    // (Audio DMA was already stopped by the quit path / the library's ar_stop.)
    // Hide whatever is left in the framebuffer while the loader takes over.
    VIDEO_SetBlack(TRUE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    WPAD_Shutdown();
    app_log("[exit] controllers off\n");
    // Only safe once no thread has a network request in flight.
    if (!g_stream_active) {
        net_deinit();
        app_log("[exit] network off\n");
    } else {
        app_log("[exit] network left on: stream threads may still be using it\n");
    }
    app_log("[exit] returning to the loader\n");
    log_shutdown();
    if (g_log) {
        fclose(g_log);
        g_log = NULL;
    }
}

// --- Stream session: launch, stream, and wait for exit ---
static void stream_session(SERVER_DATA *server, STREAM_CONFIGURATION *config, int appid) {
    CONNECTION_LISTENER_CALLBACKS cl;
    LiInitializeConnectionCallbacks(&cl);
    cl.stageStarting = cl_stage_starting;
    cl.stageComplete = cl_stage_complete;
    cl.stageFailed = cl_stage_failed;
    cl.connectionStarted = cl_started;
    cl.connectionTerminated = cl_terminated;
    cl.logMessage = cl_log;
    // The library calls these when the host sends feedback, WITHOUT checking for
    // NULL. A NULL one jumps to address 0 (the crash). Every one must be set.
    cl.rumble = cl_rumble;
    cl.rumbleTriggers = cl_rumble_triggers;
    cl.setMotionEventState = cl_set_motion_state;
    cl.setControllerLED = cl_set_led;
    cl.setHdrMode = cl_set_hdr;
    cl.setAdaptiveTriggers = cl_set_adaptive_triggers;
    cl.connectionStatusUpdate = cl_connection_status;

    printf("Launching app %d at %dx%d@%d (%d kbps)\n",
           appid, config->width, config->height, config->fps, config->bitrate);

    int r = gs_start_app(server, config, appid, false, false, 0);
    if (r != GS_OK) {
        printf("Launch failed: %s\n", gs_error ? gs_error : "unknown error");
        error_screen("Launch failed", gs_error ? gs_error : "unknown error");
        return;
    }

    // Render "starting" once before the blocking handshake.
    ui_begin_frame();
    render_message("Streaming", "Starting stream (RTSP handshake)...");
    ui_end_frame();

    DECODER_RENDERER_CALLBACKS vc;
    video_init_callbacks(&vc);
    AUDIO_RENDERER_CALLBACKS ac;
    audio_init_callbacks(&ac);
    int sr = LiStartConnection(&server->serverInfo, config, &cl,
                               &vc, &ac, NULL, 0, NULL, 0);
    int streaming = (sr == 0);
    if (streaming) {
        g_stream_active = 1;
        input_start(ui_screen_width(), ui_screen_height());
    }
    if (!streaming)
        printf("LiStartConnection failed: %d\n", sr);

    /* Sync-to-display: hold the last drawn frame and re-show it on VSyncs where
     * no new frame is ready, instead of flashing the status message. */
    u16 *last_rgb = NULL;
    int last_vw = 0, last_vh = 0, last_tw = 0, last_th = 0;

    while (1) {
        ui_begin_frame();
        WPAD_ScanPads();
        // + and - together leave the stream (see input.c for the mapping).
        if (input_poll(streaming) == INPUT_QUIT) {
            // Leaving: stop our side of the stream, then ask Sunshine to quit
            // the app. Every line is written to the SD card right away, so if
            // anything here still crashes, the log shows exactly where.
            // (LiStopConnection once crashed joining the input thread; that was
            // before the thread-stack fix, which was corrupting memory.)
            g_sync_every_line = 1;
            printf("Quit pressed: stopping the stream\n");
            // Stop the audio hardware first. Otherwise its sound interrupt keeps
            // calling our code after the app has returned to the loader (crash).
            audio_shutdown();
            ui_end_frame();
            if (streaming)
                stop_connection_timed(8000);
            if (r == GS_OK)
                gs_quit_app(server);
            printf("gs_quit_app returned\n");
            return;
        }

        u16 *rgb = NULL;
        int vw = 0, vh = 0, tw = 0, th = 0;
        if (streaming && video_frame_get(&rgb, &vw, &vh, &tw, &th)) {
            last_rgb = rgb; last_vw = vw; last_vh = vh; last_tw = tw; last_th = th;
        }
        if (last_rgb) {
            u64 d0 = gettick();
            ui_draw_rgb565(last_rgb, last_vw, last_vh, last_tw, last_th);
            video_note_display(gettick() - d0);
        } else {
            render_message("Streaming", "Host is streaming. [+] [-] to exit.");
        }
        ui_end_frame();
    }
}

int main(int argc, char **argv) {
    // Set up video + a HIDDEN console (client.c/http.c use the real printf,
    // which needs an initialized console to not crash; it is never displayed).
    VIDEO_Init();
    WPAD_Init();
    CONF_Init();
    {
        GXRModeObj *rm = VIDEO_GetPreferredMode(NULL);
        void *con_fb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rm));
        console_init(con_fb, 0, 0, rm->fbWidth, rm->xfbHeight, rm->fbWidth * VI_DISPLAY_PIX_SZ);
    }
    ui_init();

    // Show an initial frame immediately: ui_init() points the video at ui_fb[0],
    // which is uninitialized memory. Until the first GX_CopyDisp below, that
    // garbage is what's on screen — and SD mount + DHCP (net_init) can take
    // several seconds, which is exactly the "striped green/magenta" flash. So
    // render one proper frame right away.
    ui_begin_frame();
    render_message("Wii Moonlight", "Initializing...");
    ui_end_frame();

    printf("Mounting SD/USB...\n");
    if (!fatInitDefault())
        printf("WARNING: SD/USB mount failed.\n");
    log_open();
    atexit(app_shutdown);

    printf("Initializing network...\n");
    if (net_init() != 0) {
        printf("Network init failed!\n");
        error_screen("Network error", "Network init failed.");
        return 0;
    }
    printf("Network OK.\n");
    if (socInit() != 0) {
        printf("WARNING: socInit() failed (already registered?).\n");
    }

    /* Randomness first: the certificate, pairing secrets, PIN and TLS use it. */
    entropy_init();

    /* The Wii Remote pointer works in the menus too. */
    WPAD_SetDataFormat(0, WPAD_FMT_BTNS_ACC_IR);
    WPAD_SetVRes(0, ui_screen_width(), ui_screen_height());

    /* This Wii's identity (certificate). Made once; replaced once if it was
     * made by the old clock-seeded generator. RSA key generation is slow. */
    if (gs_identity_needs_generation(KEY_DIR)) {
        ui_begin_frame();
        render_message("First-time setup",
            "Creating this Wii's security key. This takes a minute or two; please wait.");
        ui_end_frame();
    }
    if (gs_ensure_identity(KEY_DIR) != GS_OK) {
        error_screen("Setup failed", gs_error ? gs_error : "Could not create the security key.");
        return 0;
    }

    char host_ip[16], host_name[64];
    SERVER_DATA server;
    PAPP_LIST applist = NULL;

    /* Pick a host, connect, pair if needed, fetch its apps. Any failure goes
     * back to the host list. */
    while (1) {
        if (!host_menu(KEY_DIR, host_ip, host_name))
            return 0;

        memset(&server, 0, sizeof(server));
        printf("Connecting to \"%s\" at %s...\n", host_name, host_ip);
        {
            char msg[128];
            snprintf(msg, sizeof(msg), "Connecting to %s (%s)...", host_name, host_ip);
            ui_begin_frame();
            render_message("Connecting", msg);
            ui_end_frame();
        }
        if (gs_init(&server, host_ip, 0, KEY_DIR, 0, true) != GS_OK) {
            printf("gs_init failed: %s\n", gs_error ? gs_error : "unknown error");
            char msg[256];
            snprintf(msg, sizeof(msg), "Could not reach Sunshine at %s: %s", host_ip,
                     gs_error ? gs_error : "unknown error");
            if (message_choice("Connection failed", msg, "[B] back to the PC list  [HOME] exit") == 'H')
                return 0;
            continue;
        }
        hosts_remember(KEY_DIR, host_ip, host_name);

        if (!server.paired) {
            printf("Not paired with %s, pairing...\n", host_ip);
            if (!pair_screen(&server, host_name))
                continue;
        }

        printf("Fetching app list...\n");
        applist = NULL;
        if (gs_applist(&server, &applist) != GS_OK) {
            printf("Failed to fetch app list: %s\n", gs_error ? gs_error : "unknown error");
            /* Most often: the Wii was removed from Sunshine's device list, so
             * our "paired" marker is stale. Offer to pair again. */
            int c = message_choice("Can't get the app list",
                "Sunshine refused the request. If you removed this Wii from Sunshine's "
                "device list, it needs to be paired again.",
                "[A] pair again  [B] back to the PC list");
            if (c == 'H')
                return 0;
            if (c != 'A')
                continue;
            gs_forget_pairing(&server);
            if (!pair_screen(&server, host_name))
                continue;
            applist = NULL;
            if (gs_applist(&server, &applist) != GS_OK) {
                error_screen("App list failed", gs_error ? gs_error : "unknown error");
                return 0;
            }
        }
        break;
    }

    // Flatten the linked list into arrays.
    int count = 0;
    for (PAPP_LIST p = applist; p != NULL; p = p->next)
        count++;
    app_log("[menu] app list: %d apps\n", count);
    for (PAPP_LIST p = applist; p != NULL; p = p->next)
        app_log("[menu]   id=%d \"%s\"\n", p->id, p->name ? p->name : "(unnamed)");
    if (count == 0) {
        error_screen("No apps", "No apps found. Add some in the Sunshine web UI.");
        return 0;
    }
    int *ids = (int*)malloc(sizeof(int) * count);
    char **names = (char**)malloc(sizeof(char*) * count);
    int i = 0;
    for (PAPP_LIST p = applist; p != NULL; p = p->next, i++) {
        ids[i] = p->id;
        names[i] = p->name ? p->name : "(unnamed)";
    }

    int sel = 0, top = 0;

    while (1) {
        ui_begin_frame();
        WPAD_ScanPads();
        u32 down = WPAD_ButtonsDown(0);

        if (down & WPAD_BUTTON_HOME) {
            ui_end_frame();
            break;
        }
        if (down & WPAD_BUTTON_DOWN && sel < count - 1)
            sel++;
        if (down & WPAD_BUTTON_UP && sel > 0)
            sel--;
        if (down & WPAD_BUTTON_PLUS) {
            app_log("[menu] 480p option opened (currently %s)\n", g_use_480 ? "480p" : "360p");
            g_use_480 = resolution_warning();
            app_log("[menu] stream resolution set to %s\n", g_use_480 ? "480p" : "360p");
        }

        int visible = LIST_VISIBLE(0);
        if (sel < top) top = sel;
        if (sel >= top + visible) top = sel - visible + 1;

        if (down & WPAD_BUTTON_A) {
            // Build the stream config, matching the Wii's TV aspect ratio.
            STREAM_CONFIGURATION config;
            memset(&config, 0, sizeof(config));
            u8 aspect = (u8)CONF_GetAspectRatio();
            config.fps = 30;
            config.bitrate = 1500;
            if (aspect == CONF_ASPECT_16_9) {
                config.width = g_use_480 ? 854 : 640;
                config.height = g_use_480 ? 480 : 360;
            } else {
                config.width = g_use_480 ? 640 : 480;
                config.height = g_use_480 ? 480 : 360;
            }
            printf("TV aspect: %s -> %dx%d\n",
                   aspect == CONF_ASPECT_16_9 ? "16:9" : "4:3", config.width, config.height);
            config.packetSize = 1024;
            config.streamingRemotely = STREAM_CFG_AUTO;
            config.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
            config.supportedVideoFormats = VIDEO_FORMAT_H264;
            config.clientRefreshRateX100 = config.fps * 100;

            int appid = ids[sel];
            app_log("[menu] launching \"%s\" (id %d) at %dx%d, %d fps, %d kbps\n",
                    names[sel], appid, config.width, config.height, config.fps, config.bitrate);
            ui_end_frame();
            stream_session(&server, &config, appid);
            app_log("[session] stream_session returned, exiting app\n");
            log_flush();
            break;  // exit the app after a stream session
        }

        render_list(host_name, names, count, sel, top);
        ui_end_frame();
    }

    return 0;
}
