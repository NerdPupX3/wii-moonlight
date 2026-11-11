#ifndef MENU_H
#define MENU_H

#include "client.h"

// Screen layout shared by every menu. TVs hide ("overscan") a few percent of
// the picture at each edge, so text stays at least UI_MARGIN px from the sides
// and the top/bottom bars are tall enough that their text sits inside the
// visible area. Backgrounds still fill the whole screen.
#define UI_MARGIN   32
#define TOP_BAR_H   56
#define FOOTER_H    44
#define CONTENT_Y   (TOP_BAR_H + 18)
#define WRAP_LINE_H 22
#define LIST_ITEM_H 46
// Rows that fit between the top bar and the footer (minus `reserved` px).
#define LIST_VISIBLE(reserved) \
    ((ui_screen_height() - CONTENT_Y - FOOTER_H - 8 - (reserved)) / LIST_ITEM_H)

// Drawing helpers shared by the screens (defined in main.c).
void render_top_bar(const char *title);
// Small text at the right end of the top bar (status, resolution).
void render_top_bar_right(const char *text);
// Word-wraps s to lines at most maxw px wide (maxw <= 0: up to the right
// margin). '\n' starts a new line. Returns the y below the last line.
int  draw_wrapped(int x, int y, const char *s, unsigned int color, int maxw);
void render_footer(const char *text);

// Shows a message and waits. Returns 'A', 'B' or 'H' (HOME). footer may be NULL.
int  message_choice(const char *title, const char *msg, const char *footer);

// Screens (hostmenu.c).
// Host list: discovered + saved hosts + "enter an IP". Returns 1 with the
// chosen host in ip_out (16 bytes) / name_out (64 bytes), or 0 for HOME.
int  host_menu(const char *keydir, char *ip_out, char *name_out);
// On-screen keypad for an IPv4 address. Returns 1 with a valid address.
int  ip_keypad(char *ip_out, int outsz);
// Shows a random PIN and runs the pairing handshake. Returns 1 when paired.
int  pair_screen(PSERVER_DATA server, const char *hostname);

#endif
