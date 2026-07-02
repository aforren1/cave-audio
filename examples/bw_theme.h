/*
 * bw_theme.h — UI theme + embedded font for the bwaudio imgui tools (calib_view, future station
 * tabs). Ported from aforren1/lsl-viewer's theme.hpp (the house reference for imgui tooling):
 * mostly-grayscale chrome with electric-blue + purple as the ONLY accents, light + dark variants,
 * and embedded Roboto-Regular (Apache-2.0) so text is identical everywhere with no asset files.
 * applyTheme() is safe to re-call at runtime (light/dark toggle, scale change) — it resets the
 * style from scratch each time, so FontScaleMain/ScaleAllSizes never compound.
 */
#pragma once

#include "imgui.h"
#if defined(__has_include) && __has_include("implot.h")   /* the layout tool has no plots; theme it anyway */
#define BW_THEME_HAVE_IMPLOT 1
#include "implot.h"
#endif
#include "roboto_font.h"   /* embedded Roboto-Regular (Apache-2.0) */

/* User-facing UI scale. On win32 set it from ImGui_ImplWin32_GetDpiScaleForHwnd once at startup
 * (the dx11/win32 backend does not feed a framebuffer scale the way SDL_GPU does, so DPI rides
 * FontScaleMain here); imgui 1.92's dynamic atlas re-rasterizes crisply at the scaled size. */
inline float g_uiScale = 1.0f;

/* Scale a hard-coded pixel literal (panel widths, fixed item widths). Do NOT wrap values already
 * derived from text metrics or style fields — those are scaled once already and would compound. */
inline float uiScaled(float px) { return px * g_uiScale; }

/* Apply the light or dark theme to ImGui + ImPlot, plus shared style polish. */
inline void applyTheme(bool light) {
    if (light) ImGui::StyleColorsLight(); else ImGui::StyleColorsDark();
#ifdef BW_THEME_HAVE_IMPLOT
    if (light) ImPlot::StyleColorsLight(); else ImPlot::StyleColorsDark();
#endif
    ImGuiStyle& s = ImGui::GetStyle();

    /* grayscale neutrals; blue + purple are the only accents (checkmark, hover, active/pressed,
     * selected-tab overline, text selection) so the chrome stays quiet and the highlights read. */
    auto rgb = [](int r, int g, int b, float a = 1.0f) {
        return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
    };
    auto A = [](ImVec4 v, float a) { v.w = a; return v; };
    ImVec4* c = s.Colors;
    if (light) {
        const ImVec4 blue = rgb(0x1B, 0x33, 0xE6), purp = rgb(0x7B, 0x22, 0xA8);
        c[ImGuiCol_Text]                 = rgb(0x1E, 0x1E, 0x22);
        c[ImGuiCol_TextDisabled]         = rgb(0x77, 0x77, 0x7F);
        c[ImGuiCol_WindowBg]             = rgb(0xF4, 0xF4, 0xF6);
        c[ImGuiCol_ChildBg]              = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_PopupBg]              = rgb(0xFF, 0xFF, 0xFF, 0.98f);
        c[ImGuiCol_Border]               = rgb(0x1E, 0x1E, 0x22, 0.55f);
        c[ImGuiCol_FrameBg]              = rgb(0xE7, 0xE7, 0xEA);
        c[ImGuiCol_FrameBgHovered]       = rgb(0xDB, 0xDB, 0xDF);
        c[ImGuiCol_FrameBgActive]        = rgb(0xCF, 0xCF, 0xD4);
        c[ImGuiCol_TitleBg]              = rgb(0xDA, 0xDA, 0xDF);
        c[ImGuiCol_TitleBgActive]        = rgb(0xD6, 0xD6, 0xDB);
        c[ImGuiCol_MenuBarBg]            = rgb(0xEC, 0xEC, 0xEF);
        c[ImGuiCol_Header]               = A(blue, 0.18f);
        c[ImGuiCol_HeaderHovered]        = A(blue, 0.32f);
        c[ImGuiCol_HeaderActive]         = A(purp, 0.42f);
        c[ImGuiCol_Button]               = rgb(0xDD, 0xDD, 0xE2);
        c[ImGuiCol_ButtonHovered]        = A(blue, 0.32f);
        c[ImGuiCol_ButtonActive]         = A(purp, 0.55f);
        c[ImGuiCol_CheckMark]            = blue;
        c[ImGuiCol_SliderGrab]           = rgb(0xA6, 0xA6, 0xAE);
        c[ImGuiCol_SliderGrabActive]     = purp;
        c[ImGuiCol_Tab]                  = rgb(0xDD, 0xDD, 0xE2);
        c[ImGuiCol_TabHovered]           = A(blue, 0.32f);
        c[ImGuiCol_TabSelected]          = rgb(0xC2, 0xCE, 0xF6);
        c[ImGuiCol_TabSelectedOverline]  = purp;
        c[ImGuiCol_TabDimmed]            = rgb(0xE6, 0xE6, 0xE9);
        c[ImGuiCol_TabDimmedSelected]    = rgb(0xD4, 0xD4, 0xD9);
        c[ImGuiCol_Separator]            = rgb(0x1E, 0x1E, 0x22, 0.55f);
        c[ImGuiCol_SeparatorHovered]     = A(blue, 0.55f);
        c[ImGuiCol_ResizeGrip]           = rgb(0x1E, 0x1E, 0x22, 0.12f);
        c[ImGuiCol_ResizeGripHovered]    = A(blue, 0.45f);
        c[ImGuiCol_ResizeGripActive]     = A(purp, 0.80f);
        c[ImGuiCol_ScrollbarBg]          = rgb(0xEC, 0xEC, 0xEF);
        c[ImGuiCol_ScrollbarGrab]        = rgb(0xC4, 0xC4, 0xCA);
        c[ImGuiCol_ScrollbarGrabHovered] = rgb(0xB2, 0xB2, 0xBA);
        c[ImGuiCol_ScrollbarGrabActive]  = A(blue, 0.70f);
        c[ImGuiCol_TextSelectedBg]       = A(blue, 0.25f);
        c[ImGuiCol_NavCursor]            = blue;
        c[ImGuiCol_PlotLines]            = blue;
        c[ImGuiCol_PlotHistogram]        = purp;
    } else {
        /* accents lifted in lightness for dark-gray contrast, same royal/violet hue */
        const ImVec4 blue = rgb(0x3D, 0x50, 0xEE), purp = rgb(0x9E, 0x3A, 0xD0);
        c[ImGuiCol_Text]                 = rgb(0xE8, 0xE8, 0xEC);
        c[ImGuiCol_TextDisabled]         = rgb(0x86, 0x86, 0x8E);
        c[ImGuiCol_WindowBg]             = rgb(0x18, 0x18, 0x1B);
        c[ImGuiCol_ChildBg]              = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_PopupBg]              = rgb(0x20, 0x20, 0x24, 0.98f);
        c[ImGuiCol_Border]               = rgb(0x6A, 0x6A, 0x7C, 0.95f);
        c[ImGuiCol_FrameBg]              = rgb(0x28, 0x28, 0x2D);
        c[ImGuiCol_FrameBgHovered]       = rgb(0x33, 0x33, 0x39);
        c[ImGuiCol_FrameBgActive]        = rgb(0x3D, 0x3D, 0x44);
        c[ImGuiCol_TitleBg]              = rgb(0x1C, 0x1C, 0x20);
        c[ImGuiCol_TitleBgActive]        = rgb(0x2A, 0x2A, 0x30);
        c[ImGuiCol_MenuBarBg]            = rgb(0x16, 0x16, 0x19);
        c[ImGuiCol_Header]               = A(blue, 0.26f);
        c[ImGuiCol_HeaderHovered]        = A(blue, 0.45f);
        c[ImGuiCol_HeaderActive]         = A(purp, 0.55f);
        c[ImGuiCol_Button]               = rgb(0x34, 0x34, 0x3A);
        c[ImGuiCol_ButtonHovered]        = A(blue, 0.45f);
        c[ImGuiCol_ButtonActive]         = A(purp, 0.80f);
        c[ImGuiCol_CheckMark]            = blue;
        c[ImGuiCol_SliderGrab]           = rgb(0x57, 0x57, 0x60);
        c[ImGuiCol_SliderGrabActive]     = purp;
        c[ImGuiCol_Tab]                  = rgb(0x22, 0x22, 0x27);
        c[ImGuiCol_TabHovered]           = A(blue, 0.45f);
        c[ImGuiCol_TabSelected]          = rgb(0x33, 0x40, 0x7E);
        c[ImGuiCol_TabSelectedOverline]  = purp;
        c[ImGuiCol_TabDimmed]            = rgb(0x18, 0x18, 0x1B);
        c[ImGuiCol_TabDimmedSelected]    = rgb(0x28, 0x28, 0x2E);
        c[ImGuiCol_Separator]            = rgb(0x6A, 0x6A, 0x7C, 0.95f);
        c[ImGuiCol_SeparatorHovered]     = A(blue, 0.60f);
        c[ImGuiCol_ResizeGrip]           = rgb(0xFF, 0xFF, 0xFF, 0.10f);
        c[ImGuiCol_ResizeGripHovered]    = A(blue, 0.55f);
        c[ImGuiCol_ResizeGripActive]     = A(purp, 0.85f);
        c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab]        = rgb(0x3A, 0x3A, 0x42);
        c[ImGuiCol_ScrollbarGrabHovered] = rgb(0x4A, 0x4A, 0x54);
        c[ImGuiCol_ScrollbarGrabActive]  = A(blue, 0.75f);
        c[ImGuiCol_TextSelectedBg]       = A(blue, 0.35f);
        c[ImGuiCol_NavCursor]            = blue;
        c[ImGuiCol_PlotLines]            = blue;
        c[ImGuiCol_PlotHistogram]        = purp;
    }
#ifdef IMGUI_HAS_DOCK                     /* lsl-viewer runs the docking branch; these tools don't (yet) */
    c[ImGuiCol_DockingPreview] = A(c[ImGuiCol_CheckMark], light ? 0.45f : 0.60f);
    c[ImGuiCol_DockingEmptyBg] = c[ImGuiCol_WindowBg];
#endif

    s.WindowRounding    = 5.0f; s.ChildRounding = 4.0f; s.FrameRounding = 4.0f;
    s.PopupRounding     = 4.0f; s.GrabRounding  = 3.0f; s.TabRounding   = 4.0f;
    s.ScrollbarRounding = 9.0f;
    s.WindowBorderSize  = 2.0f; s.FrameBorderSize = 0.0f;
    s.ChildBorderSize   = 1.0f;
    s.WindowPadding     = ImVec2(8, 8);
    s.FramePadding      = ImVec2(7, 4);
    s.ItemSpacing       = ImVec2(8, 5);
    s.AntiAliasedLinesUseTex = true;

    /* run last on the freshly-reset base values so re-calling applyTheme() is idempotent */
    s.FontScaleMain = g_uiScale;
    s.ScaleAllSizes(g_uiScale);
}

/* Embedded Roboto at its natural logical size; scale rides style.FontScaleMain (see applyTheme). */
inline void loadEmbeddedFont(ImGuiIO& io) {
    ImFontConfig cfg; cfg.OversampleH = 2; cfg.OversampleV = 1;
    io.Fonts->AddFontFromMemoryCompressedTTF(
        RobotoRegular_compressed_data, RobotoRegular_compressed_size, 15.0f, &cfg);
}
