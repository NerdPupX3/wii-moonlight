// Host selection, manual IP entry and pairing screens.
//
// Controls everywhere: point with the Wii Remote (IR) or use the D-pad to move
// the highlight, A to choose. Logging: "[menu] ..." for every choice,
// "[keypad] ..." for submitted/rejected addresses, "[pair] ..." for pairing.

#include <stdio.h>
#include <string.h>

#include <gccore.h>
#include <wiiuse/wpad.h>
#include <network.h>
#include <ogc/lwp_watchdog.h>

#include "menu.h"
#include "ui.h"
#include "discover.h"
#include "hosts.h"
#include "entropy.h"
#include "errors.h"
#include "applog.h"

/* ---------------------------------------------------------------- pointer */

static int g_ptr_x = -1, g_ptr_y = -1;     /* last IR position (for hover) */

/* Reads the IR pointer. Returns 1 if it is on screen. *moved is set when it
 * moved since the last frame, so a resting pointer doesn't fight the D-pad. */
static int pointer_read(int *x, int *y, int *moved)
{
    ir_t ir;
    WPAD_IR(0, &ir);
    *moved = 0;
    if (!ir.valid)
        return 0;
    *x = (int)ir.x;
    *y = (int)ir.y;
    if (*x != g_ptr_x || *y != g_ptr_y) {
        *moved = (g_ptr_x >= 0) && ((*x - g_ptr_x) * (*x - g_ptr_x) + (*y - g_ptr_y) * (*y - g_ptr_y) > 9);
        g_ptr_x = *x;
        g_ptr_y = *y;
    }
    return 1;
}

static void pointer_draw(int x, int y)
{
    ui_rect(x - 2, y - 2, 16, 16, UI_BLACK);
    ui_rect(x, y, 12, 12, UI_WHITE);
    ui_rect(x + 3, y + 3, 6, 6, UI_BLUE);
}

/* Free extra randomness: the accelerometer's noisy low bits and the exact
 * frame timing, mixed in (never counted) while the user is in the menus. */
static void mix_controller_noise(void)
{
    WPADData *d = WPAD_Data(0);
    struct { u64 t; s16 ax, ay, az; f32 ix, iy; } m;
    memset(&m, 0, sizeof(m));
    m.t = gettime();
    if (d) {
        m.ax = d->accel.x; m.ay = d->accel.y; m.az = d->accel.z;
        m.ix = d->ir.x; m.iy = d->ir.y;
    }
    entropy_mix(&m, sizeof(m));
}

/* -------------------------------------------------------------- host list */

typedef struct {
    char name[64];
    char ip[16];
    const char *tag;
    int manual;
} ROW;

#define MAX_ROWS (DISCOVER_MAX_HOSTS + HOSTS_MAX + 1)

static int build_rows(ROW *rows, const SAVED_HOST *saved, int nsaved)
{
    int n = 0;
    for (int i = 0; i < discover_count(); i++) {
        const DISCOVERED_HOST *h = discover_host(i);
        snprintf(rows[n].name, sizeof(rows[n].name), "%s", h->name);
        snprintf(rows[n].ip, sizeof(rows[n].ip), "%s", h->ip);
        rows[n].tag = "found on the network";
        rows[n].manual = 0;
        n++;
    }
    for (int i = 0; i < nsaved; i++) {
        int dup = 0;
        for (int j = 0; j < n; j++)
            if (strcmp(rows[j].ip, saved[i].ip) == 0) dup = 1;
        if (dup) continue;
        snprintf(rows[n].name, sizeof(rows[n].name), "%s", saved[i].name);
        snprintf(rows[n].ip, sizeof(rows[n].ip), "%s", saved[i].ip);
        rows[n].tag = "used before";
        rows[n].manual = 0;
        n++;
    }
    snprintf(rows[n].name, sizeof(rows[n].name), "Enter an IP address...");
    rows[n].ip[0] = 0;
    rows[n].tag = NULL;
    rows[n].manual = 1;
    return n + 1;
}

int host_menu(const char *keydir, char *ip_out, char *name_out)
{
    SAVED_HOST saved[HOSTS_MAX];
    int nsaved = hosts_load(keydir, saved, HOSTS_MAX);
    ROW rows[MAX_ROWS];
    int sel = 0, top = 0;

    discover_begin();
    app_log("[menu] host list opened (%d saved)\n", nsaved);

    while (1) {
        ui_begin_frame();
        WPAD_ScanPads();
        u32 down = WPAD_ButtonsDown(0);
        discover_poll();
        mix_controller_noise();

        int n = build_rows(rows, saved, nsaved);
        int list_y = CONTENT_Y;
        int visible = LIST_VISIBLE(3 * WRAP_LINE_H);   /* room for the hint */
        if (visible < 3) visible = 3;
        if (sel >= n) sel = n - 1;

        int px, py, moved;
        int ptr = pointer_read(&px, &py, &moved);
        if (ptr && moved && py >= list_y - 5) {
            int r = top + (py - (list_y - 5)) / LIST_ITEM_H;
            if (r < n && r < top + visible) sel = r;
        }
        if ((down & WPAD_BUTTON_DOWN) && sel < n - 1) sel++;
        if ((down & WPAD_BUTTON_UP) && sel > 0) sel--;
        if (sel < top) top = sel;
        if (sel >= top + visible) top = sel - visible + 1;

        if (down & WPAD_BUTTON_HOME) {
            ui_end_frame();
            discover_end();
            app_log("[menu] host list: HOME, exiting\n");
            return 0;
        }
        if (down & WPAD_BUTTON_2) {
            app_log("[menu] host list: searching again\n");
            discover_begin();
        }
        if (down & WPAD_BUTTON_A) {
            ui_end_frame();
            if (rows[sel].manual) {
                char ip[16];
                app_log("[menu] host list: manual IP entry\n");
                if (ip_keypad(ip, sizeof(ip))) {
                    discover_end();
                    snprintf(ip_out, 16, "%s", ip);
                    snprintf(name_out, 64, "%s", ip);
                    return 1;
                }
                continue;
            }
            discover_end();
            snprintf(ip_out, 16, "%s", rows[sel].ip);
            snprintf(name_out, 64, "%s", rows[sel].name);
            app_log("[menu] host chosen: \"%s\" at %s (%s)\n", name_out, ip_out, rows[sel].tag);
            return 1;
        }

        /* --- draw --- */
        render_top_bar("Choose your PC");
        if (discover_searching())
            render_top_bar_right("Searching...");

        int maxw = ui_screen_width() - 2 * UI_MARGIN;
        int y = list_y;
        for (int i = top; i < n && i < top + visible; i++) {
            int s = (i == sel);
            if (s) ui_rect(0, y - 5, ui_screen_width(), LIST_ITEM_H, UI_LIGHTBLU);
            if (rows[i].manual) {
                ui_text(UI_MARGIN, y + 10, rows[i].name, s ? UI_BLACK : UI_WHITE, 1);
            } else {
                ui_text_clip(UI_MARGIN, y, rows[i].name, s ? UI_BLACK : UI_WHITE, 1, maxw);
                /* Second line: address and where it came from. The address
                 * is left out when the name already is the address. */
                char sub[96];
                if (strcmp(rows[i].name, rows[i].ip) == 0)
                    snprintf(sub, sizeof(sub), "%s", rows[i].tag);
                else
                    snprintf(sub, sizeof(sub), "%s  -  %s", rows[i].ip, rows[i].tag);
                ui_text_clip(UI_MARGIN + 16, y + 20, sub, s ? UI_DARKGREY : UI_GREY, 1, maxw - 16);
            }
            y += LIST_ITEM_H;
        }
        if (top + visible < n)
            ui_text(UI_MARGIN, y, "...", UI_GREY, 1);

        if (discover_count() == 0 && !discover_searching())
            draw_wrapped(UI_MARGIN, ui_screen_height() - FOOTER_H - 8 - 3 * WRAP_LINE_H,
                         "No PC found automatically. Check that Sunshine is running, or enter the IP address.",
                         UI_GREY, 0);

        render_footer("[A] choose    [2] search again    [HOME] exit");
        if (ptr) pointer_draw(px, py);
        ui_end_frame();
    }
}

/* ------------------------------------------------------------ IP keypad */

typedef struct { int x, y, w, h; const char *label; char code; } KEY;

/* Codes: digits and '.', 'D' = delete, 'C' = cancel, 'K' = OK. */
static int keypad_layout(KEY *k)
{
    static const char *labels[] = { "1","2","3","4","5","6","7","8","9",".","0","Del" };
    static const char codes[] = "123456789.0D";
    const int kw = 104, kh = 46, gap = 8;
    int x0 = (ui_screen_width() - (3 * kw + 2 * gap)) / 2;
    int y0 = TOP_BAR_H + 66;
    int n = 0;
    for (int i = 0; i < 12; i++) {
        k[n].x = x0 + (i % 3) * (kw + gap);
        k[n].y = y0 + (i / 3) * (kh + gap);
        k[n].w = kw; k[n].h = kh;
        k[n].label = labels[i];
        k[n].code = codes[i];
        n++;
    }
    int wide = (3 * kw + 2 * gap - gap) / 2;
    int yb = y0 + 4 * (kh + gap);
    k[n] = (KEY){ x0, yb, wide, kh, "Cancel", 'C' }; n++;
    k[n] = (KEY){ x0 + wide + gap, yb, wide, kh, "OK", 'K' }; n++;
    return n;
}

/* Moves the highlight to the nearest key in a direction (dx, dy). */
static int keypad_move(const KEY *k, int n, int cur, int dx, int dy)
{
    int cx = k[cur].x + k[cur].w / 2, cy = k[cur].y + k[cur].h / 2;
    int best = cur, best_d = 1 << 30;
    for (int i = 0; i < n; i++) {
        if (i == cur) continue;
        int ox = k[i].x + k[i].w / 2 - cx, oy = k[i].y + k[i].h / 2 - cy;
        int along = dx ? ox * dx : oy * dy;
        int across = dx ? (oy < 0 ? -oy : oy) : (ox < 0 ? -ox : ox);
        if (along <= 0) continue;
        int d = along + 2 * across;
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

/* Strict dotted-quad check: four numbers 0-255, no empty parts. */
static int ip_valid(const char *s)
{
    int parts = 0, val = -1, digits = 0;
    for (const char *p = s;; p++) {
        if (*p >= '0' && *p <= '9') {
            val = (val < 0 ? 0 : val) * 10 + (*p - '0');
            if (++digits > 3 || val > 255) return 0;
        } else if (*p == '.' || *p == 0) {
            if (digits == 0) return 0;
            parts++;
            val = -1; digits = 0;
            if (*p == 0) break;
        } else {
            return 0;
        }
    }
    return parts == 4 && strcmp(s, "0.0.0.0") != 0 && strcmp(s, "255.255.255.255") != 0;
}

int ip_keypad(char *ip_out, int outsz)
{
    KEY keys[14];
    int nk = keypad_layout(keys);
    char text[16];
    int sel = 0;
    const char *error = NULL;
    u64 error_until = 0;

    /* Start from this Wii's own network (first three numbers): the PC is
     * almost always on the same one. */
    u32 me = net_gethostip();
    if (me)
        snprintf(text, sizeof(text), "%u.%u.%u.", (me >> 24) & 255, (me >> 16) & 255, (me >> 8) & 255);
    else
        text[0] = 0;
    app_log("[keypad] opened, prefilled \"%s\"\n", text);

    while (1) {
        ui_begin_frame();
        WPAD_ScanPads();
        u32 down = WPAD_ButtonsDown(0);
        mix_controller_noise();

        int px, py, moved;
        int ptr = pointer_read(&px, &py, &moved);
        if (ptr && moved)
            for (int i = 0; i < nk; i++)
                if (px >= keys[i].x && px < keys[i].x + keys[i].w &&
                    py >= keys[i].y && py < keys[i].y + keys[i].h) sel = i;
        if (down & WPAD_BUTTON_LEFT)  sel = keypad_move(keys, nk, sel, -1, 0);
        if (down & WPAD_BUTTON_RIGHT) sel = keypad_move(keys, nk, sel, 1, 0);
        if (down & WPAD_BUTTON_UP)    sel = keypad_move(keys, nk, sel, 0, -1);
        if (down & WPAD_BUTTON_DOWN)  sel = keypad_move(keys, nk, sel, 0, 1);

        char press = 0;
        if (down & WPAD_BUTTON_A) press = keys[sel].code;
        if (down & WPAD_BUTTON_B) press = 'D';
        if (down & WPAD_BUTTON_PLUS) press = 'K';
        if (down & WPAD_BUTTON_HOME) press = 'C';

        int len = (int)strlen(text);
        if (press == 'D' && len > 0) {
            text[len - 1] = 0;
        } else if ((press == '.' || (press >= '0' && press <= '9')) && len < 15) {
            text[len] = press;
            text[len + 1] = 0;
        } else if (press == 'C') {
            ui_end_frame();
            app_log("[keypad] cancelled\n");
            return 0;
        } else if (press == 'K') {
            if (ip_valid(text)) {
                ui_end_frame();
                snprintf(ip_out, outsz, "%s", text);
                app_log("[keypad] address entered: %s\n", ip_out);
                return 1;
            }
            app_log("[keypad] rejected \"%s\" (not a valid IPv4 address)\n", text);
            error = "That's not a valid address. Example: 192.168.1.20";
            error_until = ticks_to_millisecs(gettime()) + 2500;
        }

        /* --- draw --- */
        render_top_bar("Enter the PC's IP address");
        /* The entry field, as wide as the keypad below it. */
        int fw = 3 * 104 + 2 * 8;
        int fx = (ui_screen_width() - fw) / 2;
        int fy = TOP_BAR_H + 10;
        ui_rect(fx - 2, fy - 2, fw + 4, 48, UI_WHITE);
        ui_rect(fx, fy, fw, 44, UI_BLACK);
        char shown[20];
        snprintf(shown, sizeof(shown), "%s_", text);
        ui_text(fx + 10, fy + 6, shown, UI_WHITE, 2);

        for (int i = 0; i < nk; i++) {
            const KEY *k = &keys[i];
            u32 bg = (i == sel) ? UI_LIGHTBLU : (k->code == 'K' ? UI_GREEN : (k->code == 'C' ? UI_RED : UI_DARKGREY));
            ui_rect(k->x, k->y, k->w, k->h, bg);
            int sc = 2;
            int tx = k->x + (k->w - ui_text_width(k->label, sc)) / 2;
            int ty = k->y + (k->h - ui_text_height(sc)) / 2;
            ui_text(tx, ty, k->label, (i == sel) ? UI_BLACK : UI_WHITE, sc);
        }

        if (error && ticks_to_millisecs(gettime()) < error_until)
            ui_text_clip(UI_MARGIN, ui_screen_height() - FOOTER_H - 26, error, UI_RED, 1,
                         ui_screen_width() - 2 * UI_MARGIN);

        render_footer("[A] press    [B] delete    [+] OK    [HOME] cancel");
        if (ptr) pointer_draw(px, py);
        ui_end_frame();
    }
}

/* ---------------------------------------------------------------- pairing */

int pair_screen(PSERVER_DATA server, const char *hostname)
{
    while (1) {
        /* A fresh random 4-digit PIN each attempt. 60000 is a multiple of
         * 10000, so rejecting values >= 60000 keeps every PIN equally likely. */
        unsigned short r;
        do {
            if (entropy_random(NULL, (unsigned char *)&r, sizeof(r)) != 0) r = 0;
        } while (r >= 60000);
        char pin[8];
        snprintf(pin, sizeof(pin), "%04u", (unsigned)(r % 10000));

        char title[96];
        snprintf(title, sizeof(title), "Pair with %s", hostname);
        ui_begin_frame();
        render_top_bar(title);
        int y = draw_wrapped(UI_MARGIN, CONTENT_Y,
            "On your PC, open Sunshine's web page (https://localhost:47990), "
            "go to the PIN page, and enter this PIN:", UI_WHITE, 0);
        int pw = ui_text_width(pin, 4);
        ui_rect((ui_screen_width() - pw) / 2 - 16, y + 12, pw + 32, ui_text_height(4) + 24, UI_DARKGREY);
        ui_text((ui_screen_width() - pw) / 2, y + 24, pin, UI_YELLOW, 4);
        draw_wrapped(UI_MARGIN, y + 48 + ui_text_height(4),
            "Waiting for Sunshine... This screen continues by itself once the PIN is entered.",
            UI_GREY, 0);
        ui_end_frame();

        /* gs_pair blocks until the PIN is entered in Sunshine (or it fails).
         * The PIN itself is not logged. */
        app_log("[pair] waiting for the PIN to be entered on host \"%s\" (%s)\n",
                hostname, server->serverInfo.address);
        u64 t0 = gettime();
        int ret = gs_pair(server, pin);
        int secs = (int)(ticks_to_millisecs(gettime() - t0) / 1000);
        if (ret == GS_OK) {
            app_log("[pair] paired with \"%s\" after %d s\n", hostname, secs);
            return 1;
        }
        app_log("[pair] FAILED after %d s: %s (code %d)\n", secs, gs_error ? gs_error : "unknown", ret);
        char msg[256];
        snprintf(msg, sizeof(msg), "Pairing failed: %s", gs_error ? gs_error : "unknown error");
        int c = message_choice("Pairing failed", msg, "[A] try again  [B] back to the PC list");
        if (c != 'A')
            return 0;
    }
}
