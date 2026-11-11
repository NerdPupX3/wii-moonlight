#ifndef UI_H
#define UI_H

#include <gccore.h>

/* RGBA8 packed color, matching GX_Color1u32 byte order (0xRRGGBBAA). */
#define UI_RGBA(r, g, b, a) (((u32)(r) << 24) | ((u32)(g) << 16) | ((u32)(b) << 8) | (u32)(a))

#define UI_BLACK    UI_RGBA(0, 0, 0, 255)
#define UI_WHITE    UI_RGBA(255, 255, 255, 255)
#define UI_GREY     UI_RGBA(120, 120, 120, 255)
#define UI_DARKGREY UI_RGBA(40, 40, 40, 255)
#define UI_BLUE     UI_RGBA(40, 90, 180, 255)
#define UI_LIGHTBLU UI_RGBA(90, 140, 230, 255)
#define UI_GREEN    UI_RGBA(60, 180, 90, 255)
#define UI_RED      UI_RGBA(200, 60, 60, 255)
#define UI_YELLOW   UI_RGBA(230, 200, 60, 255)

void ui_init(void);
void ui_begin_frame(void);
void ui_end_frame(void);
void ui_fill(u32 color);
void ui_rect(int x, int y, int w, int h, u32 color);
/* Text: y is the top of the line; capitals start ~2 px (scale 1) below it.
 * scale 1 = 17 px body font, 2 = 33 px title font, 4 = title font at 2x. */
void ui_text(int x, int y, const char *str, u32 color, int scale);
int  ui_text_width(const char *str, int scale);
int  ui_text_width_n(const char *str, int n, int scale);   /* first n chars */
/* Like ui_text, but cuts the text short with "..." if it is wider than maxw. */
void ui_text_clip(int x, int y, const char *str, u32 color, int scale, int maxw);
int  ui_text_height(int scale);
int  ui_screen_width(void);
int  ui_screen_height(void);
void ui_draw_rgb565(const u16 *rgb, int w, int h, int tex_w, int tex_h);

#endif /* UI_H */
