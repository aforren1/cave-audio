/*
 * ui_text.h — crisp HUD text for the raylib tools (playground, layout_tool).
 *
 * Two things made the text hard to read: (1) raylib's default font is a 10 px bitmap, so drawing it at
 * 15-20 px bilinear-upscales a tiny atlas (blurry glyphs); (2) without FLAG_WINDOW_HIGHDPI the window's
 * framebuffer is at logical size and the OS upscales the whole thing on a scaled display (the
 * "not pixel-aligned" look). Fix both: enable HIGHDPI at window create so raylib renders at the native
 * pixel density, then rasterize a real TTF atlas at the DPI-scaled size and draw with DrawTextEx in
 * logical units — raylib maps the high-res atlas ~1:1 onto physical pixels, so glyphs land crisp.
 *
 * Usage:  SetConfigFlags(FLAG_WINDOW_HIGHDPI);   // BEFORE InitWindow
 *         InitWindow(...);
 *         ui_text_init();                         // AFTER (needs the GL context)
 *         ...
 *         ui_text("hello", 12, 12, 18, WHITE);    // in place of DrawText(...)
 *
 * Windows-only paths (these tools are Windows-only anyway); falls back to the default font if neither
 * TTF is present, in which case HIGHDPI alone still sharpens things.
 */
#ifndef BW_UI_TEXT_H
#define BW_UI_TEXT_H

#include "raylib.h"

static Font g_ui_font;
static int  g_ui_ready = 0;     /* a real TTF atlas loaded (else fall back to DrawText) */

static void ui_text_init(void) {
    Vector2 s = GetWindowScaleDPI();
    float dpi = (s.y > 0.5f) ? s.y : 1.0f;
    const int atlas = (int)(48.0f * dpi);          /* one atlas, ~3x the largest HUD size, so every */
    const char* fonts[] = {                        /* size we draw is a crisp downscale from it      */
        "C:/Windows/Fonts/consola.ttf",            /* Consolas: monospace, jitter-free readouts */
        "C:/Windows/Fonts/segoeui.ttf",            /* Segoe UI: proportional fallback */
    };
    for (int i = 0; i < 2; ++i) {
        if (!FileExists(fonts[i])) continue;
        g_ui_font = LoadFontEx(fonts[i], atlas, NULL, 0);
        if (g_ui_font.glyphCount > 0 && g_ui_font.texture.id != 0) {
            SetTextureFilter(g_ui_font.texture, TEXTURE_FILTER_BILINEAR);
            g_ui_ready = 1;
            return;
        }
    }
    g_ui_font = GetFontDefault();                  /* no TTF: HIGHDPI still helps */
}

static void ui_text(const char* s, int x, int y, int size, Color c) {
    if (!g_ui_ready) { DrawText(s, x, y, size, c); return; }
    DrawTextEx(g_ui_font, s, (Vector2){ (float)x, (float)y }, (float)size, 0.0f, c);
}

#endif /* BW_UI_TEXT_H */
