/*
 * calib_view.cpp — calibration report viewer (the Dear ImGui pilot: win32 + d3d11).
 *
 * bw_calibrate writes trims/positions/EQ into cave_layout.json and (--save-irs) per-speaker IR wavs;
 * this views all of it BEFORE you trust it: the array in 3D, gain/delay trims as bars, correction-EQ
 * magnitude curves, the retained IRs, and — the workhorse — a layout DIFF (load the pre-calibration
 * layout as A and the written-back one as B) so a swapped channel, a bad mic placement, or a bogus
 * localize solve shows up as an absurd delta instead of an evening of confusion at the rig.
 *
 *   bw_calib_view [layoutA.json] [layoutB.json]     # B optional: diff mode
 *       --irs <prefix>                              # preload <prefix>_NN.wav IR kernels
 *       --selftest                                  # run the imgui_test_engine suite and exit
 *
 * PILOT NOTES (vs the raylib tools): this is the imgui + implot + implot3d stack on the win32+d3d11
 * backend, chosen for imgui_test_engine — `--selftest` drives the ACTUAL GUI with fake inputs
 * (type a path, click Load, click tabs), asserts on app state, captures screenshots, and exits with
 * a pass/fail code, so the GUI itself runs under ctest. That loop is what raylib can't do.
 *
 * Data comes straight from the engine's own loader (layout.c via bw_core) — the viewer can't drift
 * from what the engine would actually load. IR wavs decode through sound.c for the same reason.
 */
#include "imgui.h"
#include "implot.h"
#include "implot3d.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "imgui_te_engine.h"
#include "imgui_te_context.h"
#include "imgui_te_ui.h"

extern "C" {                       /* engine internals (C, no extern-C guards of their own) */
#include "layout.h"
#include "sound.h"
}

#include <d3d11.h>
#include <commdlg.h>       /* GetOpenFileNameA: the native file picker (comdlg32) */
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef Yield
#undef Yield               /* winbase.h defines Yield() as a macro; it shadows ImGuiTestContext::Yield */
#endif

/* ============================== data model ============================== */

#define EQ_PTS 240                 /* magnitude-curve resolution (log 20 Hz .. 20 kHz) */
#define IR_FS  48000u              /* bw_calibrate writes IRs at the engine rate */

struct View {
    char   pathA[512], pathB[512], irprefix[512];
    Layout A, B;
    bool   hasA, hasB;
    char   status[512];

    /* derived, refreshed on load (not per frame) */
    float  gainA_db[BW_CHANNELS], delayA_ms[BW_CHANNELS];
    float  gainB_db[BW_CHANNELS], delayB_ms[BW_CHANNELS];
    float  ax[BW_CHANNELS], ay[BW_CHANNELS], az[BW_CHANNELS];    /* 3D plot coords (room x, z, y-up) */
    float  bx[BW_CHANNELS], by[BW_CHANNELS], bz[BW_CHANNELS];
    float  dpos_mm[BW_CHANNELS];
    float  eqfreq[EQ_PTS];
    float  eqmagA[BW_CHANNELS][EQ_PTS];                          /* dB; only valid where eq_len > 0 */

    SoundData ir[BW_CHANNELS];
    bool      hasIR[BW_CHANNELS];
    float     ir_ms[BW_CHANNELS];                                /* peak time of each loaded IR */
    int       ir_n;

    int    sel;                                                  /* selected speaker */
    bool   eq_all;                                               /* overlay every eq curve */
};
static View V;

static float lin_to_db(float g)  { return (g > 1e-9f) ? 20.0f * log10f(g) : -120.0f; }

/* |H(f)| of a FIR, evaluated directly at EQ_PTS log-spaced frequencies (512 taps x 240 pts is
 * trivial and only runs on load — no FFT machinery needed for a display curve). */
static void eq_magnitude(const float* taps, int n, float fs, float* out_db) {
    for (int k = 0; k < EQ_PTS; ++k) {
        float f = 20.0f * powf(1000.0f, (float)k / (EQ_PTS - 1));       /* 20 Hz .. 20 kHz */
        float w = 2.0f * 3.14159265f * f / fs, re = 0.f, im = 0.f;
        for (int i = 0; i < n; ++i) { re += taps[i] * cosf(w * i); im -= taps[i] * sinf(w * i); }
        out_db[k] = lin_to_db(sqrtf(re * re + im * im));
    }
}

static void derive(Layout* L, float* gdb, float* dms, float* x, float* y, float* z, float eqmag[][EQ_PTS]) {
    for (uint32_t i = 0; i < L->count; ++i) {
        gdb[i] = lin_to_db(L->speakers[i].gain_lin);
        dms[i] = (float)L->speakers[i].delay_samples * 1000.0f / (float)IR_FS;
        x[i] = L->speakers[i].pos[0];                            /* implot3d: Z is up; feed room (x, z, y) */
        y[i] = L->speakers[i].pos[2];
        z[i] = L->speakers[i].pos[1];
        if (eqmag && L->speakers[i].eq_len)
            eq_magnitude(L->speakers[i].eq, L->speakers[i].eq_len, (float)IR_FS, eqmag[i]);
    }
}

static void refresh_diff(void) {
    if (!V.hasA || !V.hasB) return;
    for (uint32_t i = 0; i < V.A.count; ++i) {
        float dx = V.A.speakers[i].pos[0] - V.B.speakers[i].pos[0];
        float dy = V.A.speakers[i].pos[1] - V.B.speakers[i].pos[1];
        float dz = V.A.speakers[i].pos[2] - V.B.speakers[i].pos[2];
        V.dpos_mm[i] = sqrtf(dx * dx + dy * dy + dz * dz) * 1000.0f;
    }
}

static void load_layout(int which) {                             /* 0 = A, 1 = B */
    Layout* L    = which ? &V.B : &V.A;
    bool*   has  = which ? &V.hasB : &V.hasA;
    char*   path = which ? V.pathB : V.pathA;
    char err[256];
    *has = layout_load(path, IR_FS, L, err, sizeof err);
    if (*has) {
        int neq = 0; for (uint32_t i = 0; i < L->count; ++i) if (L->speakers[i].eq_len) ++neq;
        snprintf(V.status, sizeof V.status, "%c: %u speakers loaded from %s (%d with eq)",
                 which ? 'B' : 'A', L->count, path, neq);
        if (which) derive(L, V.gainB_db, V.delayB_ms, V.bx, V.by, V.bz, NULL);
        else       derive(L, V.gainA_db, V.delayA_ms, V.ax, V.ay, V.az, V.eqmagA);
        refresh_diff();
    } else snprintf(V.status, sizeof V.status, "%c: %s", which ? 'B' : 'A', err);
}

static void load_irs(void) {
    char err[256], p[600];
    V.ir_n = 0;
    uint32_t n = V.hasA ? V.A.count : BW_CHANNELS;
    for (uint32_t i = 0; i < n; ++i) {
        if (V.hasIR[i]) { sound_unload(&V.ir[i]); V.hasIR[i] = false; }
        snprintf(p, sizeof p, "%s_%02u.wav", V.irprefix, i);
        V.hasIR[i] = sound_load(p, IR_FS, &V.ir[i], err, sizeof err);
        if (V.hasIR[i]) {
            ++V.ir_n;
            uint32_t pk = 0; float pv = 0.f;                     /* direct-arrival marker = |peak| */
            for (uint32_t s = 0; s < V.ir[i].frames; ++s) { float a = fabsf(V.ir[i].pcm[s]); if (a > pv) { pv = a; pk = s; } }
            V.ir_ms[i] = (float)pk * 1000.0f / (float)IR_FS;
        }
    }
    snprintf(V.status, sizeof V.status, "IRs: loaded %d of %u (%s_NN.wav)", V.ir_n, n, V.irprefix);
}

/* ============================== UI ============================== */

static bool show_imgui_demo, show_implot_demo, show_te_ui;
static ImGuiTestEngine* g_te;
static HWND g_hwnd;

/* Native open dialog. OFN_NOCHANGEDIR is load-bearing: this tool (like the rest of the repo's tools)
 * resolves relative paths against the CWD, and GetOpenFileName silently changes it by default. The
 * typed InputText stays alongside — it is the path the test engine drives (a native modal can't be). */
static bool pick_file(char* buf, size_t cap, const char* filter) {
    char tmp[512] = "";
    OPENFILENAMEA ofn = { sizeof ofn };
    ofn.hwndOwner   = g_hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile   = tmp;
    ofn.nMaxFile    = sizeof tmp;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&ofn)) return false;
    snprintf(buf, cap, "%s", tmp);
    return true;
}

/* picking any one "<prefix>_NN.wav" IR selects the whole set: strip the suffix back to the prefix */
static void wav_to_prefix(char* p) {
    size_t n = strlen(p);
    if (n > 4 && !_stricmp(p + n - 4, ".wav")) {
        size_t cut = n - 4;
        if (cut >= 3 && p[cut - 3] == '_' && p[cut - 2] >= '0' && p[cut - 2] <= '9'
                                          && p[cut - 1] >= '0' && p[cut - 1] <= '9') cut -= 3;
        p[cut] = 0;
    }
}

static void tab_array(void) {
    if (!ImPlot3D::BeginPlot("##array", ImGui::GetContentRegionAvail())) return;
    ImPlot3D::SetupAxes("x (m)", "z (m)", "y up (m)",            /* auto-fit: implot3d defaults to 0..1 */
                        ImPlot3DAxisFlags_AutoFit, ImPlot3DAxisFlags_AutoFit, ImPlot3DAxisFlags_AutoFit);
    if (V.hasA) {
        ImPlot3D::PlotScatter("A", V.ax, V.ay, V.az, (int)V.A.count);
        char lbl[8];
        for (uint32_t i = 0; i < V.A.count; ++i) {               /* index labels: the wiring check */
            snprintf(lbl, sizeof lbl, "%u", i);
            ImPlot3D::PlotText(lbl, V.ax[i], V.ay[i], V.az[i] + 0.12f);
        }
        ImPlot3D::PlotScatter("sel", &V.ax[V.sel], &V.ay[V.sel], &V.az[V.sel], 1,
                              ImPlot3DSpec(ImPlot3DProp_MarkerSize, 7.0f));
    }
    if (V.hasB) {
        ImPlot3D::PlotScatter("B", V.bx, V.by, V.bz, (int)V.B.count);
        static float seg[3][2 * BW_CHANNELS];                    /* A->B delta segments */
        for (uint32_t i = 0; i < V.B.count; ++i) {
            seg[0][2*i] = V.ax[i]; seg[0][2*i+1] = V.bx[i];
            seg[1][2*i] = V.ay[i]; seg[1][2*i+1] = V.by[i];
            seg[2][2*i] = V.az[i]; seg[2][2*i+1] = V.bz[i];
        }
        ImPlot3D::PlotLine("A->B", seg[0], seg[1], seg[2], (int)(2 * V.B.count),
                           ImPlot3DSpec(ImPlot3DProp_Flags, ImPlot3DLineFlags_Segments));
    }
    ImPlot3D::EndPlot();
}

static void tab_trims(void) {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 half  = ImVec2(avail.x, avail.y * 0.5f - 4);
    if (ImPlot::BeginPlot("gain trim (dB)", half)) {                 /* values plot at x = i + shift */
        ImPlot::SetupAxes("speaker", "dB");
        if (V.hasA) ImPlot::PlotBars("A", V.gainA_db, (int)V.A.count, V.hasB ? 0.35 : 0.6, V.hasB ? -0.19 : 0.0);
        if (V.hasB) ImPlot::PlotBars("B", V.gainB_db, (int)V.B.count, 0.35, 0.19);
        ImPlot::EndPlot();
    }
    if (ImPlot::BeginPlot("delay trim (ms)", half)) {
        ImPlot::SetupAxes("speaker", "ms");
        if (V.hasA) ImPlot::PlotBars("A", V.delayA_ms, (int)V.A.count, V.hasB ? 0.35 : 0.6, V.hasB ? -0.19 : 0.0);
        if (V.hasB) ImPlot::PlotBars("B", V.delayB_ms, (int)V.B.count, 0.35, 0.19);
        ImPlot::EndPlot();
    }
}

static void tab_eq(void) {
    ImGui::Checkbox("overlay all speakers", &V.eq_all);
    if (!ImPlot::BeginPlot("##eq", ImGui::GetContentRegionAvail())) return;
    ImPlot::SetupAxes("Hz", "dB");
    ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
    ImPlot::SetupAxesLimits(20, 20000, -12, 12, ImPlotCond_Once);
    bool any = false;
    if (V.hasA) {
        char lbl[24];
        for (uint32_t i = 0; i < V.A.count; ++i) {
            if (!V.A.speakers[i].eq_len) continue;
            if (!V.eq_all && (int)i != V.sel) continue;
            any = true;
            snprintf(lbl, sizeof lbl, "spk %u (%u taps)", i, V.A.speakers[i].eq_len);
            ImPlot::PlotLine(lbl, V.eqfreq, V.eqmagA[i], EQ_PTS);
        }
    }
    if (!any) ImPlot::Annotation(200, 0, ImVec4(1, 1, 1, 0.6f), ImVec2(0, 0), false,
                                 "no correction eq %s(run bw_calibrate --eq)",
                                 V.eq_all ? "in this layout " : "on the selected speaker ");
    ImPlot::EndPlot();
}

static void tab_irs(void) {
    if (!V.hasIR[V.sel]) {
        ImGui::TextDisabled("no IR loaded for speaker %d - set the --save-irs prefix and Load IRs "
                            "(bw_calibrate --save-irs <prefix> writes <prefix>_NN.wav)", V.sel);
        return;
    }
    static float xs[120000]; static uint32_t xs_n;               /* shared ms axis, built lazily */
    SoundData* s = &V.ir[V.sel];
    uint32_t n = s->frames; if (n > 120000) n = 120000;
    if (xs_n < n) { for (uint32_t i = 0; i < n; ++i) xs[i] = (float)i * 1000.0f / (float)IR_FS; xs_n = n; }
    char title[64]; snprintf(title, sizeof title, "impulse response, speaker %d##ir", V.sel);
    if (!ImPlot::BeginPlot(title, ImGui::GetContentRegionAvail())) return;
    ImPlot::SetupAxes("ms", "amp");
    ImPlot::PlotLine("ir", xs, s->pcm, (int)n);
    double pk = V.ir_ms[V.sel];
    ImPlot::TagX(pk, ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "direct %.1f ms", pk);
    ImPlot::EndPlot();
}

static void tab_diff(void) {
    if (!V.hasA || !V.hasB) { ImGui::TextDisabled("load a second layout as B to diff (e.g. the file bw_calibrate wrote)"); return; }
    if (!ImGui::BeginTable("difft", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) return;
    ImGui::TableSetupColumn("spk"); ImGui::TableSetupColumn("dpos (mm)"); ImGui::TableSetupColumn("dgain (dB)");
    ImGui::TableSetupColumn("ddelay (ms)"); ImGui::TableSetupColumn("eq A -> B");
    ImGui::TableSetupScrollFreeze(0, 1); ImGui::TableHeadersRow();
    const ImVec4 bad(1.0f, 0.42f, 0.42f, 1.0f), ok(0.78f, 0.78f, 0.82f, 1.0f);
    for (uint32_t i = 0; i < V.A.count; ++i) {
        float dg = V.gainB_db[i] - V.gainA_db[i], dd = V.delayB_ms[i] - V.delayA_ms[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%2u", i);
        ImGui::TableNextColumn(); ImGui::TextColored(V.dpos_mm[i] > 10.f ? bad : ok, "%8.1f", V.dpos_mm[i]);
        ImGui::TableNextColumn(); ImGui::TextColored(fabsf(dg) > 1.f ? bad : ok, "%+7.2f", dg);
        ImGui::TableNextColumn(); ImGui::TextColored(fabsf(dd) > 0.1f ? bad : ok, "%+7.3f", dd);
        ImGui::TableNextColumn(); ImGui::Text("%u -> %u taps", V.A.speakers[i].eq_len, V.B.speakers[i].eq_len);
    }
    ImGui::EndTable();
}

static void draw_ui(void) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("calib view", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings |
                                     ImGuiWindowFlags_MenuBar);
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Tools")) {
            ImGui::MenuItem("ImGui demo", NULL, &show_imgui_demo);
            ImGui::MenuItem("ImPlot demo", NULL, &show_implot_demo);
            ImGui::MenuItem("Test engine", NULL, &show_te_ui);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    ImGui::BeginChild("side", ImVec2(340, 0), ImGuiChildFlags_Borders);
    const char* JSONF = "layout json (*.json)\0*.json\0all files (*.*)\0*.*\0";
    const char* WAVF  = "IR wav (*.wav)\0*.wav\0all files (*.*)\0*.*\0";
    ImGui::TextUnformatted("layout A (surveyed / before)");
    ImGui::SetNextItemWidth(-104);
    ImGui::InputText("##A", V.pathA, sizeof V.pathA);
    ImGui::SameLine(); if (ImGui::Button("...##pA") && pick_file(V.pathA, sizeof V.pathA, JSONF)) load_layout(0);
    ImGui::SameLine(); if (ImGui::Button("Load A")) load_layout(0);
    ImGui::TextUnformatted("layout B (calibrated / after)");
    ImGui::SetNextItemWidth(-104);
    ImGui::InputText("##B", V.pathB, sizeof V.pathB);
    ImGui::SameLine(); if (ImGui::Button("...##pB") && pick_file(V.pathB, sizeof V.pathB, JSONF)) load_layout(1);
    ImGui::SameLine(); if (ImGui::Button("Load B")) load_layout(1);
    ImGui::TextUnformatted("IR prefix (bw_calibrate --save-irs)");
    ImGui::SetNextItemWidth(-104);
    ImGui::InputText("##IR", V.irprefix, sizeof V.irprefix);
    ImGui::SameLine(); if (ImGui::Button("...##pI") && pick_file(V.irprefix, sizeof V.irprefix, WAVF)) {
        wav_to_prefix(V.irprefix);                               /* any one _NN.wav selects the set */
        load_irs();
    }
    ImGui::SameLine(); if (ImGui::Button("Load IRs")) load_irs();
    ImGui::Separator();
    ImGui::TextWrapped("%s", V.status[0] ? V.status : "load a cave_layout.json to begin");
    ImGui::Separator();
    if (V.hasA && ImGui::BeginListBox("##spk", ImVec2(-1, -1))) {
        char row[96];
        for (uint32_t i = 0; i < V.A.count; ++i) {
            snprintf(row, sizeof row, "spk %2u   %+5.1f dB  %6.2f ms%s%s", i, V.gainA_db[i], V.delayA_ms[i],
                     V.A.speakers[i].eq_len ? "  eq" : "", V.hasIR[i] ? "  ir" : "");
            if (ImGui::Selectable(row, V.sel == (int)i)) V.sel = (int)i;
        }
        ImGui::EndListBox();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("main", ImVec2(0, 0));
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Array")) { tab_array(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Trims")) { tab_trims(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("EQ"))    { tab_eq();    ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("IRs"))   { tab_irs();   ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Diff"))  { tab_diff();  ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::End();

    if (show_imgui_demo)  ImGui::ShowDemoWindow(&show_imgui_demo);
    if (show_implot_demo) ImPlot::ShowDemoWindow(&show_implot_demo);
    if (show_te_ui && g_te) ImGuiTestEngine_ShowTestEngineWindows(g_te, &show_te_ui);
}

/* ============================== selftest (imgui_test_engine) ============================== */

/* Hermetic fixtures written to the cwd: the 3x3x3 boundary grid (like layout_default). Variant B
 * nudges speaker 7 by +10 cm and -1.5 dB and gives speaker 3 an 8-tap eq — known deltas the tests
 * assert on through the REAL UI (typed paths, clicked buttons), not through a parallel code path. */
static const char* FIX_A = "calibview_fix_a.json";
static const char* FIX_B = "calibview_fix_b.json";

static int write_fixture(const char* path, int variant_b) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "{\n  \"schema_version\": 1,\n"
               "  \"units\": { \"position\": \"meters\", \"gain\": \"decibels\", \"delay\": \"milliseconds\" },\n"
               "  \"coordinate_space\": \"room, right-handed\",\n"
               "  \"reference\": { \"alignment\": \"max-distance\", \"speed_of_sound_mps\": 343.0 },\n"
               "  \"dbap\": { \"rolloff_r\": 0.5, \"distance_attenuation\": { \"model\": \"inverse\","
               " \"reference_distance_m\": 1.0, \"rolloff\": 1.0, \"min_gain_db\": -40.0 } },\n"
               "  \"speakers\": [\n");
    int idx = 0;
    for (int zi = -1; zi <= 1; ++zi) for (int yi = -1; yi <= 1; ++yi) for (int xi = -1; xi <= 1; ++xi) {
        if (!zi && !yi && !xi) continue;                          /* 27 - centre = 26 */
        double x = 1.5 * xi, y = 1.5 * yi, z = 1.5 * zi, g = 0.0;
        if (variant_b && idx == 7) { x += 0.10; g = -1.5; }       /* the known deltas */
        fprintf(f, "    { \"index\": %d, \"position\": [%.4f, %.4f, %.4f], \"gain_db\": %.2f, \"delay_ms\": %.3f",
                idx, x, y, z, g, 0.05 * idx);
        if (variant_b && idx == 3)
            fprintf(f, ", \"eq\": [0.9, 0.2, -0.1, 0.05, 0.02, -0.01, 0.005, 0.0]");
        fprintf(f, " }%s\n", idx < 25 ? "," : "");
        ++idx;
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    return 1;
}

static void register_tests(ImGuiTestEngine* e) {
    ImGuiTest* t;

    t = IM_REGISTER_TEST(e, "viewer", "load_a");                 /* type a path, click Load, layout appears */
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("calib view");
        ctx->ItemClick("**/##A");
        ctx->KeyCharsReplaceEnter(FIX_A);
        ctx->ItemClick("**/Load A");
        IM_CHECK(V.hasA);
        IM_CHECK_EQ(V.A.count, 26u);
        IM_CHECK(strstr(V.status, "26 speakers") != NULL);
    };

    t = IM_REGISTER_TEST(e, "viewer", "diff_b");                 /* load B, the known deltas show up */
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("calib view");
        ctx->ItemClick("**/##B");
        ctx->KeyCharsReplaceEnter(FIX_B);
        ctx->ItemClick("**/Load B");
        IM_CHECK(V.hasB);
        IM_CHECK(V.dpos_mm[7] > 95.0f && V.dpos_mm[7] < 105.0f); /* the +10 cm nudge */
        IM_CHECK_EQ(V.B.speakers[3].eq_len, (uint16_t)8);        /* the injected eq */
        ctx->ItemClick("**/Diff");
        ctx->Yield(2);
        ctx->CaptureScreenshotWindow("//calib view");            /* -> output/captures/viewer_diff_b_NNNN.png */
        ctx->ItemClick("**/Array");
        ctx->Yield(2);
        ctx->CaptureScreenshotWindow("//calib view");
    };

    t = IM_REGISTER_TEST(e, "viewer", "tabs");                   /* every tab renders without faulting */
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("calib view");
        const char* tabs[] = { "**/Array", "**/Trims", "**/EQ", "**/IRs", "**/Diff" };
        for (int i = 0; i < 5; ++i) { ctx->ItemClick(tabs[i]); ctx->Yield(2); }
        ctx->ItemClick("**/EQ");                                 /* the checkbox lives inside the EQ tab */
        ctx->Yield(2);
        ctx->ItemClick("**/overlay all speakers");
        IM_CHECK(V.eq_all);
        ctx->Yield(2);
    };
}

/* ============================== win32 + d3d11 shell ============================== */

static ID3D11Device*           g_dev;
static ID3D11DeviceContext*    g_ctx;
static IDXGISwapChain*         g_swap;
static ID3D11RenderTargetView* g_rtv;

static void create_rtv(void) {
    ID3D11Texture2D* back = NULL;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) { g_dev->CreateRenderTargetView(back, NULL, &g_rtv); back->Release(); }
}
static bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;           /* RGBA: the capture func memcpys rows out */
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL lvl;
    const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    if (D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want, 2,
            D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &lvl, &g_ctx) != S_OK &&
        D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, want, 2,   /* CI/RDP fallback */
            D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &lvl, &g_ctx) != S_OK)
        return false;
    create_rtv();
    return true;
}
static void destroy_device(void) {
    if (g_rtv)  { g_rtv->Release();  g_rtv = NULL; }
    if (g_swap) { g_swap->Release(); g_swap = NULL; }
    if (g_ctx)  { g_ctx->Release();  g_ctx = NULL; }
    if (g_dev)  { g_dev->Release();  g_dev = NULL; }
}

/* test-engine screenshot hook: copy the backbuffer through a staging texture (screenshots land in
 * output/captures/ next to the cwd). Backbuffer is RGBA8 to match what the capture tool expects. */
static bool screen_capture(ImGuiID viewport_id, int x, int y, int w, int h, unsigned int* pixels, void* user) {
    (void)viewport_id; (void)user;
    ID3D11Texture2D* back = NULL;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (!back) return false;
    D3D11_TEXTURE2D_DESC d; back->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D* st = NULL;
    bool ok = false;
    if (SUCCEEDED(g_dev->CreateTexture2D(&d, NULL, &st))) {
        g_ctx->CopyResource(st, back);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            for (int row = 0; row < h; ++row) {
                const unsigned int* src = (const unsigned int*)((const unsigned char*)m.pData + (size_t)(y + row) * m.RowPitch) + x;
                unsigned int* dst = pixels + (size_t)row * w;
                for (int c = 0; c < w; ++c) dst[c] = src[c] | 0xFF000000u;   /* force opaque alpha */
            }
            g_ctx->Unmap(st, 0);
            ok = true;
        }
        st->Release();
    }
    back->Release();
    return ok;
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
static LRESULT WINAPI wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return true;
    switch (m) {
    case WM_SIZE:
        if (g_dev && w != SIZE_MINIMIZED) {
            if (g_rtv) { g_rtv->Release(); g_rtv = NULL; }
            g_swap->ResizeBuffers(0, LOWORD(l), HIWORD(l), DXGI_FORMAT_UNKNOWN, 0);
            create_rtv();
        }
        return 0;
    case WM_SYSCOMMAND: if ((w & 0xfff0) == SC_KEYMENU) return 0; break;   /* no ALT menu */
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int main(int argc, char** argv) {
    bool selftest = false;
    int  npos = 0;
    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--selftest"))            selftest = true;
        else if (!strcmp(argv[i], "--irs") && i + 1 < argc) snprintf(V.irprefix, sizeof V.irprefix, "%s", argv[++i]);
        else if (argv[i][0] != '-' && npos < 2)             snprintf(npos++ ? V.pathB : V.pathA, sizeof V.pathA, "%s", argv[i]);
        else { fprintf(stderr, "usage: calib_view [layoutA.json] [layoutB.json] [--irs prefix] [--selftest]\n"); return 2; }
    }
    for (int k = 0; k < EQ_PTS; ++k) V.eqfreq[k] = 20.0f * powf(1000.0f, (float)k / (EQ_PTS - 1));
    if (selftest) {
        if (!write_fixture(FIX_A, 0) || !write_fixture(FIX_B, 1)) { fprintf(stderr, "calib_view: cannot write fixtures in cwd\n"); return 1; }
    } else {
        if (!V.pathA[0]) snprintf(V.pathA, sizeof V.pathA, "cave_layout.json");
        if (V.pathA[0])  load_layout(0);                          /* best effort; status shows any error */
        if (V.pathB[0])  load_layout(1);
        if (V.irprefix[0]) load_irs();
    }

    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc = { sizeof wc, CS_CLASSDC, wnd_proc, 0, 0, GetModuleHandleW(NULL), NULL, NULL, NULL, NULL, L"bw_calib_view", NULL };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"bwaudio - calibration report viewer", WS_OVERLAPPEDWINDOW,
                              100, 100, 1280, 800, NULL, NULL, wc.hInstance, NULL);
    g_hwnd = hwnd;                                                /* file-picker dialog owner */
    if (!create_device(hwnd)) { fprintf(stderr, "calib_view: d3d11 device creation failed\n"); return 1; }
    ShowWindow(hwnd, selftest ? SW_SHOWNOACTIVATE : SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImPlot3D::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL;                                        /* a viewer; don't scatter imgui.ini */
    float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(scale);
    io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/consola.ttf", 15.0f * scale);   /* crisp mono, like ui_text.h */
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    g_te = ImGuiTestEngine_CreateContext();
    ImGuiTestEngineIO& teio = ImGuiTestEngine_GetIO(g_te);
    teio.ConfigVerboseLevel        = ImGuiTestVerboseLevel_Warning;
    teio.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Debug;
    teio.ConfigLogToTTY            = selftest;                    /* ctest: name each test + why it failed */
    teio.ConfigCaptureEnabled      = true;                        /* actually write the screenshots (output/captures/) */
    teio.ConfigRunSpeed            = selftest ? ImGuiTestRunSpeed_Fast : ImGuiTestRunSpeed_Normal;
    teio.ScreenCaptureFunc         = screen_capture;
    ImGuiTestEngine_Start(g_te, ImGui::GetCurrentContext());
    ImGuiTestEngine_InstallDefaultCrashHandler();
    register_tests(g_te);
    if (selftest) ImGuiTestEngine_QueueTests(g_te, ImGuiTestGroup_Tests, NULL, ImGuiTestRunFlags_RunFromCommandLine);

    bool done = false;
    int  frames = 0, drain = 0;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        draw_ui();
        ImGui::Render();
        const float clear[4] = { 0.08f, 0.08f, 0.10f, 1.0f };
        g_ctx->OMSetRenderTargets(1, &g_rtv, NULL);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(selftest ? 0 : 1, 0);                     /* selftest: no vsync throttle */
        ImGuiTestEngine_PostSwap(g_te);                           /* screenshots happen here */

        ++frames;
        if (selftest && frames > 5 && ImGuiTestEngine_IsTestQueueEmpty(g_te) && ++drain > 3) done = true;
    }

    int rc = 0;
    if (selftest) {
        ImGuiTestEngineResultSummary sum;
        ImGuiTestEngine_GetResultSummary(g_te, &sum);
        printf("calib_view selftest: %d/%d passed\n", sum.CountSuccess, sum.CountTested);
        rc = (sum.CountTested == 0 || sum.CountSuccess != sum.CountTested) ? 1 : 0;
    }

    ImGuiTestEngine_Stop(g_te);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImPlot3D::DestroyContext();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    ImGuiTestEngine_DestroyContext(g_te);                         /* after DestroyContext, per the te docs */
    destroy_device();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    for (int i = 0; i < BW_CHANNELS; ++i) if (V.hasIR[i]) sound_unload(&V.ir[i]);
    return rc;
}
