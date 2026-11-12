// Input for the stream: the Wii Remote and Nunchuk act as one PlayStation-style
// controller on the host. Sunshine chooses that emulation because the client
// reports motion sensors and a touchpad (see Sunshine's "automatic selection").
//
// Wii Remote and Nunchuk -> PlayStation controller:
//   A                  -> Cross            B -> Circle
//   1 / 2              -> Square / Triangle
//   D-pad              -> D-pad
//   +                  -> Options          -  -> Select (Back; see below)
//   HOME               -> PlayStation button
//   Nunchuk stick      -> left stick
//   Z / C              -> L1 / R1
//   Chords (decided from the buttons held right now, highest priority first):
//     + and C          -> mouse: A = left click, B = right click (no gamepad buttons)
//     1 and 2          -> right stick click
//     B and Z          -> left stick click
//     A and B          -> R2 (right trigger, full)
//     Z and C          -> L2 (left trigger, full)
//   Remote gyro        -> not sent: MotionPlus is kept off so the Nunchuk works (see input_start)
//   Remote accelerometer -> accelerometer motion events
//   IR pointer         -> touchpad (touch down / move / up at the pointer position)
//                         and the desktop cursor (absolute mouse position)
//
// Select: Sunshine's default "Map Back/Select to Touchpad Click" turns Back into the
// touchpad button. Uncheck that option in Sunshine to send Select as Select.
//
// + and - held together leave the stream.
//
// The touchpad and cursor only work when the Remote points at the sensor bar.
// Logging: connection state, the controller arrival result, touch-support status,
// and a stats line every 5 seconds, including the raw MotionPlus values (the gyro
// scale is a guess until calibrated from a log).

#include <stdio.h>
#include <string.h>
#include <math.h>

#include <gccore.h>
#include <wiiuse/wpad.h>
#include <ogc/lwp_watchdog.h>

#include "Limelight.h"
#include "input.h"
#include "applog.h"

/* Crash debugging: for the first frames of a stream, every step is written to the
 * SD card immediately, so the log shows exactly which step a crash happens in. */
#define TRACE_FRAMES 0   /* set to e.g. 60 to trace the first frames again */
static int g_trace_left = TRACE_FRAMES;
#define TRACE(label) do { if (g_trace_left > 0) { app_log("[input] trace %s\n", label); app_log_sync(); } } while (0)

/* DIAGNOSTIC (crash hunt): 1 = report the controller exactly like the last build
 * that did not crash (Nintendo type, gyro only) and send no accelerometer or
 * touch events. 0 = PlayStation type with accel + gyro + touchpad. */
#define DIAG_OLD_ARRIVAL 0

#define STICK_DEADZONE 0.12f
#define KEEPALIVE_MS 250           /* resend an unchanged gamepad state this often */
#define STATS_INTERVAL_MS 5000
#define GYRO_SCALE 0.1f            /* deg/s per raw MotionPlus unit: a guess, verify in the log */
#define GYRO_DEADZONE_DEG 2.0f
#define ACCEL_CHANGE_MS2 0.05f     /* resend accelerometer when it changes by this much */
#define GRAVITY_MS2 9.80665f
#define TOUCH_MOVE_EPS 0.002f      /* normalized distance that counts as movement */

static int g_started = 0;
static int g_combo_prev = 0;
static int g_mouse_mode = 0;      /* toggled by pressing A and C together */
static int g_ac_prev = 0;
static int g_screen_w = 640, g_screen_h = 480;
static int g_touch_ok = -1;        /* did the host accept touch events? (-1 = not tried) */

/* Last gamepad state sent, to send only changes. */
static int g_last_flags = -1, g_last_lt = -1, g_last_rt = -1;
static short g_last_lsx = 0, g_last_lsy = 0;
static uint64_t g_last_pad_send_ms = 0;

static int g_mp_zero_set = 0;
static int g_mp_zero[3];
static float g_last_gyro[3];
static float g_last_accel[3];

/* Touchpad state (from the IR pointer). */
static int g_touch_active = 0;
static float g_touch_x = 0.0f, g_touch_y = 0.0f;
static int g_last_ir_valid = -1;

static unsigned g_pad_sent = 0, g_touch_sent = 0, g_mouse_sent = 0, g_buttons_sent = 0;
static unsigned g_ir_frames = 0, g_ir_valid_frames = 0;

/* Raw button diagnostics (chord bug hunt): every change of the held-button mask
 * is logged (up to BTN_LOG_LIMIT per stream), and the stats line counts changes.
 * A chord that never fires, or a button that flickers on/off every report (for
 * example Nunchuk buttons read through MotionPlus pass-through), shows up here. */
#define BTN_LOG_LIMIT 400
static u32 g_last_held = 0;
static unsigned g_btn_logged = 0, g_btn_changes = 0;
static int g_last_exp_type = -1;

/* Switches an active MotionPlus (attachment type 5) off so the Nunchuk behind it
 * is read again. Two calls are needed the first time: libogc's first call only
 * probes (and enables); once it knows a MotionPlus is present, the next call with
 * 0 really disables it. So: retry once a second while type 5 is seen, up to
 * MP_OFF_MAX_TRIES per stream, and log every attempt. */
#define EXP_TYPE_MOTION_PLUS 5
#define MP_OFF_RETRY_MS 1000
#define MP_OFF_MAX_TRIES 6
static uint64_t g_mp_off_ms = 0;
static int g_mp_off_tries = 0;

static void motionplus_keep_off(int exp_type, uint64_t now)
{
    if (exp_type != EXP_TYPE_MOTION_PLUS || g_mp_off_tries >= MP_OFF_MAX_TRIES)
        return;
    if (g_mp_off_tries > 0 && now - g_mp_off_ms < MP_OFF_RETRY_MS)
        return;
    g_mp_off_ms = now;
    g_mp_off_tries++;
    int r = WPAD_SetMotionPlus(0, 0);
    app_log("[input] MotionPlus is active (Nunchuk hidden): disable attempt %d/%d returned %d\n",
            g_mp_off_tries, MP_OFF_MAX_TRIES, r);
}
static uint64_t g_last_stats_ms = 0;

static uint64_t now_ms(void)
{
    return (uint64_t)ticks_to_millisecs(gettime());
}

void input_start(int screenW, int screenH)
{
    g_screen_w = screenW;
    g_screen_h = screenH;

    WPAD_SetDataFormat(0, WPAD_FMT_BTNS_ACC_IR);
    WPAD_SetVRes(0, screenW, screenH);
    /* MotionPlus stays OFF, and input_start does NOT call WPAD_SetMotionPlus.
     *
     * libogc can only run a MotionPlus in its exclusive mode: the attachment
     * then reports as type 5 and a Nunchuk plugged into it is not read at all
     * (no stick, C, Z, or Nunchuk chords such as the A+C mouse toggle).
     *
     * Calling WPAD_SetMotionPlus(0, 0) here made it worse: libogc's first call
     * only probes for a MotionPlus, and its probe callback then calls
     * wiiuse_set_motion_plus(wm, 1) whatever was asked (wiiuse/motion_plus.c,
     * wiiuse_probe_motion_plus_check2). The log showed it: attachment type 1
     * (Nunchuk) at stream start, then 0, then 5 about 0.7 s after the "disable".
     * Left alone, an inactive MotionPlus passes the Nunchuk straight through.
     * If it is ever found active, motionplus_keep_off() below switches it off. */

#if DIAG_OLD_ARRIVAL
    int flags = A_FLAG | B_FLAG | X_FLAG | Y_FLAG | UP_FLAG | DOWN_FLAG | LEFT_FLAG |
                RIGHT_FLAG | LB_FLAG | RB_FLAG | PLAY_FLAG | BACK_FLAG | LS_CLK_FLAG;
    uint16_t caps = LI_CCAP_GYRO;
    uint8_t type = LI_CTYPE_NINTENDO;
#else
    int flags = A_FLAG | B_FLAG | X_FLAG | Y_FLAG | UP_FLAG | DOWN_FLAG | LEFT_FLAG |
                RIGHT_FLAG | LB_FLAG | RB_FLAG | PLAY_FLAG | BACK_FLAG | LS_CLK_FLAG |
                SPECIAL_FLAG | TOUCHPAD_FLAG;
    uint16_t caps = LI_CCAP_ACCEL | LI_CCAP_GYRO | LI_CCAP_TOUCHPAD;
    uint8_t type = LI_CTYPE_PS;
#endif
    int r = LiSendControllerArrivalEvent(0, 1, type, flags, caps);

    g_started = 1;
    g_mouse_mode = 0;
    g_ac_prev = 0;
    g_trace_left = TRACE_FRAMES;
    g_last_flags = -1;
    g_mp_zero_set = 0;
    g_touch_active = 0;
    g_last_ir_valid = -1;
    g_touch_ok = -1;
    g_last_stats_ms = now_ms();
    g_pad_sent = g_touch_sent = g_mouse_sent = g_buttons_sent = 0;
    g_ir_frames = g_ir_valid_frames = 0;
    g_last_held = 0;
    g_last_exp_type = -1;
    g_mp_off_tries = 0;
    g_btn_logged = g_btn_changes = 0;
    app_log("[input] start: screen %dx%d, MotionPlus left off, arrival type=%u caps=0x%x "
            "returned %d%s\n", screenW, screenH, (unsigned)type, (unsigned)caps, r,
            DIAG_OLD_ARRIVAL ? " [DIAG: old arrival, no accel/touch sends]" : "");
}

static short clamp_axis(float v)
{
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (short)(v * 32767.0f);
}

static void send_gamepad(int flags, int lt, int rt, short lsx, short lsy, uint64_t now)
{
    int changed = flags != g_last_flags || lt != g_last_lt || rt != g_last_rt ||
                  lsx != g_last_lsx || lsy != g_last_lsy;
    if (!changed && now - g_last_pad_send_ms < KEEPALIVE_MS)
        return;
    LiSendControllerEvent(flags, (unsigned char)lt, (unsigned char)rt, lsx, lsy, 0, 0);
    g_last_flags = flags; g_last_lt = lt; g_last_rt = rt;
    g_last_lsx = lsx; g_last_lsy = lsy;
    g_last_pad_send_ms = now;
    g_pad_sent++;
}

/* Converts the Nunchuk stick (angle in degrees, magnitude 0..1) to x/y in -1..1. */
static int g_stick_oct = -1;   /* last logged stick direction, -1 = centred */

static void nunchuk_stick(const struct expansion_t *exp, float *x, float *y)
{
    *x = 0.0f;
    *y = 0.0f;
    if (exp->type != WPAD_EXP_NUNCHUK)
        return;
    float mag = exp->nunchuk.js.mag;
    if (mag < STICK_DEADZONE) {
        g_stick_oct = -1;   /* centred */
        return;
    }
    /* libogc gives the stick direction as a compass bearing: 0 degrees is UP,
     * 90 is RIGHT, growing clockwise. So x (right +) is the sine and y (up +,
     * like an Xbox stick) is the cosine; cos/sin the other way round turned
     * up into right and mirrored every other direction. */
    float ang = exp->nunchuk.js.ang * (float)M_PI / 180.0f;
    float scaled = (mag - STICK_DEADZONE) / (1.0f - STICK_DEADZONE);
    if (scaled > 1.0f) scaled = 1.0f;
    *x = scaled * sinf(ang);
    *y = scaled * cosf(ang);

    /* Log each change of direction (8 compass points), to check the mapping. */
    static const char *names[8] = { "up", "up-right", "right", "down-right",
                                    "down", "down-left", "left", "up-left" };
    float deg = exp->nunchuk.js.ang;
    while (deg < 0) deg += 360.0f;
    int oct = ((int)((deg + 22.5f) / 45.0f)) % 8;
    if (oct != g_stick_oct) {
        app_log("[input] nunchuk stick %s (angle %.0f, mag %.2f) -> x=%.2f y=%.2f\n",
                names[oct], deg, mag, *x, *y);
        g_stick_oct = oct;
    }
}

/* Presses or releases a mouse button, only when its state changes. */
static int g_mouse_left = 0, g_mouse_right = 0;
static void mouse_button(int button, int want)
{
    int *held = (button == BUTTON_LEFT) ? &g_mouse_left : &g_mouse_right;
    if (want == *held)
        return;
    LiSendMouseButtonEvent(want ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, button);
    *held = want;
    g_buttons_sent++;
}

static void gamepad_state(u32 held, int combo, uint64_t now, const struct expansion_t *exp)
{
    /* Mouse mode toggle: press A and C together. Each press flips the mode.
     * While it is on, A is the left mouse button and B is the right one.
     * The toggle uses A and C, so they are not also sent to the gamepad. */
    int a = (held & WPAD_BUTTON_A) != 0;
    int b = (held & WPAD_BUTTON_B) != 0;
    int c = (held & WPAD_NUNCHUK_BUTTON_C) != 0;
    TRACE("gs: chords start");
    int ac = a && c;
    if (ac && !g_ac_prev) {
        g_mouse_mode = !g_mouse_mode;
        if (!g_mouse_mode) {
            mouse_button(BUTTON_LEFT, 0);
            mouse_button(BUTTON_RIGHT, 0);
        }
        app_log("[input] mouse mode %s (A+C)\n",
                g_mouse_mode ? "ON: A = left click, B = right click" : "OFF: gamepad");
    }
    g_ac_prev = ac;
    if (ac) {
        a = 0;
        c = 0;
    }
    int mouse = g_mouse_mode;
    TRACE("gs: toggle done");

    /* Other chords. Each is decided from the buttons held right now, highest
     * priority first:
     *   1 and 2      -> right stick click
     *   B and Z      -> left stick click
     *   A and B      -> R2 (right trigger)
     *   Z and C      -> L2 (left trigger)
     */
    int one = (held & WPAD_BUTTON_1) != 0;
    int two = (held & WPAD_BUTTON_2) != 0;
    int plus = (held & WPAD_BUTTON_PLUS) != 0;
    int z = (held & WPAD_NUNCHUK_BUTTON_Z) != 0;

    int rs_chord = one && two;
    int ls_chord = b && z;
    int r2 = a && b && !ls_chord;
    int l2 = z && c && !ls_chord;

    TRACE("gs: flags start");
    int flags = 0;
    int lt = l2 ? 255 : 0;
    int rt = r2 ? 255 : 0;

    /* In mouse mode, A and B are mouse buttons, not gamepad buttons. */
    if (!mouse && a && !r2) flags |= A_FLAG;
    if (!mouse && !ls_chord && !r2 && b) flags |= B_FLAG;
    if (!rs_chord && one) flags |= X_FLAG;
    if (!rs_chord && two) flags |= Y_FLAG;
    if (rs_chord) flags |= RS_CLK_FLAG;
    if (ls_chord) flags |= LS_CLK_FLAG;
    if (!ls_chord && !l2 && z) flags |= LB_FLAG;
    if (!l2 && c) flags |= RB_FLAG;

    if (held & WPAD_BUTTON_UP) flags |= UP_FLAG;
    if (held & WPAD_BUTTON_DOWN) flags |= DOWN_FLAG;
    if (held & WPAD_BUTTON_LEFT) flags |= LEFT_FLAG;
    if (held & WPAD_BUTTON_RIGHT) flags |= RIGHT_FLAG;
    if (held & WPAD_BUTTON_HOME) flags |= SPECIAL_FLAG;

    /* + alone is Options; + and - together is the exit combo; - is Select. */
    if (!combo && plus) flags |= PLAY_FLAG;
    if (!combo && (held & WPAD_BUTTON_MINUS)) flags |= BACK_FLAG;

    TRACE("gs: flags done");
    /* Mouse buttons (mouse mode only). */
    mouse_button(BUTTON_LEFT, mouse && a);
    mouse_button(BUTTON_RIGHT, mouse && b && !ls_chord);
    TRACE("gs: mouse buttons done");

    float sx, sy;
    TRACE("gs: stick start");
    nunchuk_stick(exp, &sx, &sy);
    TRACE("gs: stick done");
    send_gamepad(flags, lt, rt, clamp_axis(sx), clamp_axis(sy), now);
    TRACE("gs: pad sent or skipped");

    /* Motion: MotionPlus gyro (deg/s) and the Remote's accelerometer (m/s^2). */
    if (exp->type == WPAD_EXP_NONE && (exp->mp.rx || exp->mp.ry || exp->mp.rz)) {
        int raw[3] = { exp->mp.rx, exp->mp.ry, exp->mp.rz };
        if (!g_mp_zero_set) {
            memcpy(g_mp_zero, raw, sizeof(raw));
            g_mp_zero_set = 1;
        }
        float rate[3];
        for (int i = 0; i < 3; i++) {
            float deg = (float)(raw[i] - g_mp_zero[i]) * GYRO_SCALE;
            rate[i] = (fabsf(deg) < GYRO_DEADZONE_DEG) ? 0.0f : deg;
        }
        if (fabsf(rate[0] - g_last_gyro[0]) > 0.01f || fabsf(rate[1] - g_last_gyro[1]) > 0.01f ||
            fabsf(rate[2] - g_last_gyro[2]) > 0.01f) {
            LiSendControllerMotionEvent(0, LI_MOTION_TYPE_GYRO, rate[0], rate[1], rate[2]);
            memcpy(g_last_gyro, rate, sizeof(rate));
        }
    }

#if !DIAG_OLD_ARRIVAL
    struct gforce_t g;
    TRACE("gforce");
    WPAD_GForce(0, &g);
    TRACE("accel send");
    float accel[3] = { g.x * GRAVITY_MS2, g.y * GRAVITY_MS2, g.z * GRAVITY_MS2 };
    if (fabsf(accel[0] - g_last_accel[0]) > ACCEL_CHANGE_MS2 ||
        fabsf(accel[1] - g_last_accel[1]) > ACCEL_CHANGE_MS2 ||
        fabsf(accel[2] - g_last_accel[2]) > ACCEL_CHANGE_MS2) {
        LiSendControllerMotionEvent(0, LI_MOTION_TYPE_ACCEL, accel[0], accel[1], accel[2]);
        memcpy(g_last_accel, accel, sizeof(accel));
    }
#endif
}

/* IR pointer -> touchpad and desktop cursor. */
static void pointer_state(void)
{
    struct ir_t ir;
    TRACE("ir read");
    WPAD_IR(0, &ir);
    TRACE("ir read done");
    g_ir_frames++;

    if (g_last_ir_valid != ir.valid) {
        app_log("[input] IR pointer %s\n", ir.valid ? "on screen" : "lost (point at the sensor bar)");
        g_last_ir_valid = ir.valid;
    }

    if (ir.valid) {
        g_ir_valid_frames++;
        float nx = ir.x / (float)g_screen_w;
        float ny = ir.y / (float)g_screen_h;
        if (nx < 0.0f) nx = 0.0f;
        if (ny < 0.0f) ny = 0.0f;
        if (nx > 1.0f) nx = 1.0f;
        if (ny > 1.0f) ny = 1.0f;

        /* Desktop cursor: absolute position, works in any app. */
        int x = (int)(nx * g_screen_w), y = (int)(ny * g_screen_h);
        LiSendMousePositionEvent((short)x, (short)y, (short)g_screen_w, (short)g_screen_h);
        g_mouse_sent++;

        /* Touchpad: down when the pointer appears, move while it's on screen. */
        if (DIAG_OLD_ARRIVAL) {
            /* no touch events in the diagnostic build */
        } else if (!g_touch_active) {
            int r = LiSendControllerTouchEvent(0, LI_TOUCH_EVENT_DOWN, 0, nx, ny, 1.0f);
            if (g_touch_ok == -1) {
                g_touch_ok = (r == 0);
                app_log("[input] touchpad events: first touch-down returned %d (%s)\n", r,
                        r == 0 ? "accepted by host" : "NOT supported by this host");
            }
            g_touch_active = 1;
            g_touch_sent++;
        } else if (fabsf(nx - g_touch_x) > TOUCH_MOVE_EPS || fabsf(ny - g_touch_y) > TOUCH_MOVE_EPS) {
            LiSendControllerTouchEvent(0, LI_TOUCH_EVENT_MOVE, 0, nx, ny, 1.0f);
            g_touch_sent++;
        }
        g_touch_x = nx;
        g_touch_y = ny;
    } else if (g_touch_active && !DIAG_OLD_ARRIVAL) {
        LiSendControllerTouchEvent(0, LI_TOUCH_EVENT_UP, 0, g_touch_x, g_touch_y, 0.0f);
        g_touch_active = 0;
        g_touch_sent++;
    }

}

static void stats_maybe_log(uint64_t now)
{
    if (now - g_last_stats_ms < STATS_INTERVAL_MS)
        return;
    g_last_stats_ms = now;
    app_log("[input] stats: btn_changes=%u held=0x%08x exp=%d mode=%s pad_sent=%u touch_events=%u cursor_sent=%u clicks=%u "
            "ir_valid=%u/%u touch_host=%d mp_zero=(%d,%d,%d) gyro_deg_s=(%.1f,%.1f,%.1f) "
            "accel_ms2=(%.1f,%.1f,%.1f)\n",
            g_btn_changes, (unsigned)g_last_held, g_last_exp_type,
            g_mouse_mode ? "MOUSE" : "GAMEPAD", g_pad_sent, g_touch_sent, g_mouse_sent, g_buttons_sent,
            g_ir_valid_frames, g_ir_frames, g_touch_ok,
            g_mp_zero[0], g_mp_zero[1], g_mp_zero[2],
            g_last_gyro[0], g_last_gyro[1], g_last_gyro[2],
            g_last_accel[0], g_last_accel[1], g_last_accel[2]);
    g_ir_frames = g_ir_valid_frames = 0;
    g_btn_changes = 0;
}

/* Logs the raw held mask whenever it changes. Remote buttons are the low 16 bits
 * (A=0x0008 B=0x0004 1=0x0002 2=0x0001 +=0x1000 -=0x0010 HOME=0x0080), Nunchuk
 * buttons are shifted up 16 (Z=0x00010000 C=0x00020000). exp is the attachment
 * type libogc reports (0 none, 1 Nunchuk, 5/6/7 MotionPlus modes). */
static void buttons_log(u32 held, const struct expansion_t *exp)
{
    if (held == g_last_held)
        return;
    g_btn_changes++;
    if (g_btn_logged < BTN_LOG_LIMIT) {
        g_btn_logged++;
        app_log("[input] buttons 0x%08x (was 0x%08x) exp=%d%s\n", (unsigned)held,
                (unsigned)g_last_held, exp->type,
                g_btn_logged == BTN_LOG_LIMIT ? " [button log limit reached]" : "");
    }
    g_last_held = held;
}

int input_poll(int streaming)
{
    u32 held = WPAD_ButtonsHeld(0);
    uint64_t now = now_ms();

    int combo = (held & WPAD_BUTTON_PLUS) && (held & WPAD_BUTTON_MINUS);
    int quit = combo && !g_combo_prev;
    g_combo_prev = combo;
    if (quit) {
        app_log("[input] + and - pressed together: leaving the stream\n");
        return INPUT_QUIT;
    }

    if (!streaming || !g_started)
        return INPUT_NONE;

    struct expansion_t exp;
    memset(&exp, 0, sizeof(exp));
    WPAD_Expansion(0, &exp);

    buttons_log(held, &exp);
    if (exp.type != g_last_exp_type) {
        app_log("[input] attachment type changed: %d -> %d (0 none, 1 Nunchuk, 2 Classic, "
                "5 MotionPlus active)\n", g_last_exp_type, exp.type);
        g_last_exp_type = exp.type;
    }
    motionplus_keep_off(exp.type, now);
    TRACE("gamepad_state");
    gamepad_state(held, combo, now, &exp);
    TRACE("pointer_state");
    pointer_state();
    TRACE("stats");
    stats_maybe_log(now);
    TRACE("done");
    if (g_trace_left > 0)
        g_trace_left--;
    return INPUT_NONE;
}
