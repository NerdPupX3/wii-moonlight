#include <gccore.h>
#include <string.h>
#include <malloc.h>
#include "ui.h"
#include "font.h"

#define UI_FIFO_SIZE (256 * 1024)

static void *ui_fifo = NULL;
static GXRModeObj *ui_rmode = NULL;
static void *ui_fb[2] = {NULL, NULL};
static int ui_fb_index = 0;
static Mtx ui_modelview;

/* Two fonts from font.h: SMALL (17 px) for body text, LARGE (33 px) for titles
 * and big digits. Text is proportional: each glyph has its own advance width. */
typedef struct {
    GXTexObj tex;
    const unsigned char *atlas;
    int atlas_bytes;
    int cell_w, cell_h, cols, atlas_w, atlas_h;
    int base_row;    /* baseline row inside a glyph cell */
    int baseline;    /* baseline distance below the text's y */
    int line_h;      /* height reported by ui_text_height */
    const unsigned short *adv8;   /* advance per glyph, in 1/8 px */
    const signed char *xoff;      /* cell x relative to the pen position */
} UI_FONT;

#define FONT_DEF(P, p) { .atlas = ui_font_##p##_atlas, .atlas_bytes = sizeof(ui_font_##p##_atlas), \
    .cell_w = P##_CELL_W, .cell_h = P##_CELL_H, .cols = P##_COLS, .atlas_w = P##_ATLAS_W, \
    .atlas_h = P##_ATLAS_H, .base_row = P##_BASE_ROW, .baseline = P##_BASELINE, \
    .line_h = P##_LINE_H, .adv8 = ui_font_##p##_adv8, .xoff = ui_font_##p##_xoff }

static UI_FONT ui_fonts[2] = {
    FONT_DEF(UI_FONT_SMALL, small),
    FONT_DEF(UI_FONT_LARGE, large),
};

/* scale 1 = small font; scale 2 = large font; scale 4 = large font drawn 2x. */
static const UI_FONT *font_for(int scale, int *mult) {
    if (scale <= 1) { *mult = 1; return &ui_fonts[0]; }
    *mult = scale / 2;
    return &ui_fonts[1];
}

static int glyph_index(char c) {
    if (c < UI_FONT_FIRST_CHAR || c > UI_FONT_LAST_CHAR)
        c = '?';
    return c - UI_FONT_FIRST_CHAR;
}

void ui_init(void) {
    // VIDEO_Init() is done by the caller (main.c), which needs it first to set
    // up the hidden printf console.
    ui_rmode = VIDEO_GetPreferredMode(NULL);

    ui_fb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(ui_rmode));
    ui_fb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(ui_rmode));

    VIDEO_Configure(ui_rmode);
    VIDEO_SetNextFramebuffer(ui_fb[0]);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (ui_rmode->viTVMode & VI_NON_INTERLACE)
        VIDEO_WaitVSync();

    ui_fifo = memalign(32, UI_FIFO_SIZE);
    memset(ui_fifo, 0, UI_FIFO_SIZE);

    GX_Init(ui_fifo, UI_FIFO_SIZE);

    GXColor clear = {0, 0, 0, 0xFF};
    GX_SetCopyClear(clear, 0x00FFFFFF);
    GX_SetViewport(0, 0, ui_rmode->fbWidth, ui_rmode->efbHeight, 0, 1);
    GX_SetScissor(0, 0, ui_rmode->fbWidth, ui_rmode->efbHeight);
    GX_SetDispCopySrc(0, 0, ui_rmode->fbWidth, ui_rmode->efbHeight);
    GX_SetDispCopyDst(ui_rmode->fbWidth, ui_rmode->xfbHeight);
    GX_SetDispCopyYScale((f32)ui_rmode->xfbHeight / (f32)ui_rmode->efbHeight);
    GX_SetCopyFilter(ui_rmode->aa, ui_rmode->sample_pattern, GX_TRUE, ui_rmode->vfilter);
    GX_SetFieldMode(ui_rmode->field_rendering, ((ui_rmode->viHeight == 2 * ui_rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE));
    if (ui_rmode->aa)
        GX_SetPixelFmt(GX_PF_RGB565_Z16, GX_ZC_LINEAR);
    else
        GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GX_SetDispCopyGamma(GX_GM_1_0);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetZMode(GX_FALSE, GX_LEQUAL, GX_TRUE);
    GX_SetNumChans(1);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);

    /* 2D orthographic projection: (0,0) = top-left, y grows downward. */
    Mtx44 perspective;
    guOrtho(perspective, 0, (f32)ui_rmode->efbHeight, 0, (f32)ui_rmode->fbWidth, 0, 300);
    GX_LoadProjectionMtx(perspective, GX_ORTHOGRAPHIC);

    guMtxIdentity(ui_modelview);
    guMtxTransApply(ui_modelview, ui_modelview, 0.0f, 0.0f, -5.0f);
    GX_LoadPosMtxImm(ui_modelview, GX_PNMTX0);

    /* Vertex format: 2D position + RGBA8 color + 2D float texcoord. */
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

    /* Font textures: IA8 (alpha = glyph coverage, intensity = white), so the
     * anti-aliased edges blend smoothly into whatever is behind the text. The
     * atlas sizes are powers of two, which the GX texture cache requires.
     * LINEAR filtering: at 1:1 it samples texel centres exactly (crisp), and
     * at 2x (the pairing PIN) it smooths instead of showing blocky pixels.
     * Every glyph cell has an empty 1-texel border, so filtering never picks
     * up a neighbouring glyph. */
    for (int i = 0; i < 2; i++) {
        UI_FONT *f = &ui_fonts[i];
        GX_InitTexObj(&f->tex, (void*)f->atlas, f->atlas_w, f->atlas_h,
                      GX_TF_IA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
        GX_InitTexObjLOD(&f->tex, GX_LINEAR, GX_LINEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
        /* The GP reads textures from RAM by DMA, so flush them out of the
         * CPU cache first (real hardware shows garbage otherwise). */
        DCFlushRange((void*)f->atlas, f->atlas_bytes);
    }

    GX_InvalidateTexAll();
}

void ui_begin_frame(void) {
    GX_SetViewport(0, 0, ui_rmode->fbWidth, ui_rmode->efbHeight, 0, 1);
    GX_SetScissor(0, 0, ui_rmode->fbWidth, ui_rmode->efbHeight);
    GX_SetDispCopySrc(0, 0, ui_rmode->fbWidth, ui_rmode->efbHeight);
    GX_SetDispCopyDst(ui_rmode->fbWidth, ui_rmode->xfbHeight);
    GX_SetDispCopyYScale((f32)ui_rmode->xfbHeight / (f32)ui_rmode->efbHeight);
    GX_InvVtxCache();
    GX_InvalidateTexAll();
    GX_LoadPosMtxImm(ui_modelview, GX_PNMTX0);
}

void ui_end_frame(void) {
    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GX_SetAlphaUpdate(GX_TRUE);
    GX_SetColorUpdate(GX_TRUE);
    GX_CopyDisp(ui_fb[ui_fb_index], GX_TRUE);
    GX_DrawDone();
    VIDEO_SetNextFramebuffer(ui_fb[ui_fb_index]);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    ui_fb_index ^= 1;
}

void ui_fill(u32 color) {
    ui_rect(0, 0, ui_screen_width(), ui_screen_height(), color);
}

void ui_rect(int x, int y, int w, int h, u32 color) {
    f32 x2 = (f32)(x + w);
    f32 y2 = (f32)(y + h);

    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);

    // The vertex format always carries a texture coordinate (GX_VA_TEX0 is
    // DIRECT), so even though the TEV ignores it here we MUST still emit it —
    // otherwise the vertex stream is 8 bytes short per vertex and every vertex
    // after the first is read from garbage offsets.
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position2f32((f32)x, (f32)y);   GX_Color1u32(color); GX_TexCoord2f32(0.0f, 0.0f);
    GX_Position2f32(x2, (f32)y);       GX_Color1u32(color); GX_TexCoord2f32(0.0f, 0.0f);
    GX_Position2f32(x2, y2);           GX_Color1u32(color); GX_TexCoord2f32(0.0f, 0.0f);
    GX_Position2f32((f32)x, y2);       GX_Color1u32(color); GX_TexCoord2f32(0.0f, 0.0f);
    GX_End();
}

void ui_text(int x, int y, const char *str, u32 color, int scale) {
    int mult;
    const UI_FONT *f = font_for(scale, &mult);
    int cw = f->cell_w * mult;
    int ch = f->cell_h * mult;
    f32 top = (f32)(y + (f->baseline - f->base_row) * mult);
    int pen8 = x * 8;   /* pen position in 1/8 px, so rounding never accumulates */

    GX_LoadTexObj((GXTexObj*)&f->tex, GX_TEXMAP0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);

    for (; *str; str++) {
        int gi = glyph_index(*str);
        if (*str != ' ') {
            int gx = gi % f->cols;
            int gy = gi / f->cols;
            f32 u0 = (f32)(gx * f->cell_w) / (f32)f->atlas_w;
            f32 v0 = (f32)(gy * f->cell_h) / (f32)f->atlas_h;
            f32 u1 = (f32)((gx + 1) * f->cell_w) / (f32)f->atlas_w;
            f32 v1 = (f32)((gy + 1) * f->cell_h) / (f32)f->atlas_h;

            /* Whole-pixel positions keep 1:1 text sharp. */
            f32 qx = (f32)((pen8 + 4) / 8 + f->xoff[gi] * mult);
            f32 qx2 = qx + (f32)cw;
            f32 qy2 = top + (f32)ch;

            GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
            GX_Position2f32(qx, top);   GX_Color1u32(color); GX_TexCoord2f32(u0, v0);
            GX_Position2f32(qx2, top);  GX_Color1u32(color); GX_TexCoord2f32(u1, v0);
            GX_Position2f32(qx2, qy2);  GX_Color1u32(color); GX_TexCoord2f32(u1, v1);
            GX_Position2f32(qx, qy2);   GX_Color1u32(color); GX_TexCoord2f32(u0, v1);
            GX_End();
        }
        pen8 += f->adv8[gi] * mult;
    }
}

int ui_text_width_n(const char *str, int n, int scale) {
    int mult;
    const UI_FONT *f = font_for(scale, &mult);
    int w8 = 0;
    for (int i = 0; i < n && str[i]; i++)
        w8 += f->adv8[glyph_index(str[i])] * mult;
    return (w8 + 7) / 8;
}

int ui_text_width(const char *str, int scale) {
    return ui_text_width_n(str, (int)strlen(str), scale);
}

int ui_text_height(int scale) {
    int mult;
    const UI_FONT *f = font_for(scale, &mult);
    return f->line_h * mult;
}

void ui_text_clip(int x, int y, const char *str, u32 color, int scale, int maxw) {
    int n = (int)strlen(str);
    if (ui_text_width_n(str, n, scale) <= maxw) {
        ui_text(x, y, str, color, scale);
        return;
    }
    /* Too wide: cut it short and end with "...". */
    char buf[256];
    int dots = ui_text_width("...", scale);
    if (n > (int)sizeof(buf) - 4)
        n = (int)sizeof(buf) - 4;
    while (n > 0 && ui_text_width_n(str, n, scale) + dots > maxw)
        n--;
    while (n > 0 && str[n - 1] == ' ')
        n--;
    memcpy(buf, str, n);
    memcpy(buf + n, "...", 4);
    ui_text(x, y, buf, color, scale);
}

int ui_screen_width(void) {
    return ui_rmode->fbWidth;
}

int ui_screen_height(void) {
    return ui_rmode->efbHeight;
}

/* Draw a decoded RGB565 frame fullscreen. rgb is a power-of-two (tex_w x tex_h)
 * buffer; w/h are the visible video size within it. */
void ui_draw_rgb565(const u16 *rgb, int w, int h, int tex_w, int tex_h) {
    GXTexObj tex;
    GX_InitTexObj(&tex, (void*)rgb, tex_w, tex_h, GX_TF_RGB565,
                  GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjLOD(&tex, GX_LINEAR, GX_LINEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);

    DCFlushRange((void*)rgb, tex_w * tex_h * 2);
    GX_InvalidateTexAll();

    GX_LoadTexObj(&tex, GX_TEXMAP0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);

    f32 u1 = (f32)w / (f32)tex_w;
    f32 v1 = (f32)h / (f32)tex_h;
    f32 sw = (f32)ui_rmode->fbWidth;
    f32 sh = (f32)ui_rmode->efbHeight;

    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position2f32(0.0f, 0.0f);  GX_Color1u32(UI_WHITE); GX_TexCoord2f32(0.0f, 0.0f);
    GX_Position2f32(sw, 0.0f);    GX_Color1u32(UI_WHITE); GX_TexCoord2f32(u1, 0.0f);
    GX_Position2f32(sw, sh);      GX_Color1u32(UI_WHITE); GX_TexCoord2f32(u1, v1);
    GX_Position2f32(0.0f, sh);    GX_Color1u32(UI_WHITE); GX_TexCoord2f32(0.0f, v1);
    GX_End();
}
