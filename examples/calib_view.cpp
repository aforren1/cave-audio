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
 *       --tests [filter]                            # run the imgui_test_engine suite (optionally
 *                                                   #   filtered, e.g. --tests viewer) and exit
 *
 * PILOT NOTES (vs the raylib tools): this is the imgui + implot + implot3d stack on the win32+d3d11
 * backend, chosen for imgui_test_engine — `--tests` drives the ACTUAL GUI with fake inputs
 * (type a path, click Load, click tabs), asserts on app state, captures screenshots, and exits with
 * a pass/fail code, so the GUI itself runs under ctest. That loop is what raylib can't do.
 * Test-engine wiring + conventions follow aforren1/lsl-viewer (the house reference for imgui tools).
 *
 * Data comes straight from the engine's own loader (layout.c via bw_core) — the viewer can't drift
 * from what the engine would actually load. IR wavs decode through sound.c for the same reason.
 */
#include "imgui.h"
#include "implot.h"
#include "implot3d.h"
#include "bw_theme.h"      /* lsl-viewer's theme + embedded Roboto (applyTheme / loadEmbeddedFont / uiScaled) */
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "imgui_te_engine.h"
#include "imgui_te_context.h"
#include "imgui_te_ui.h"

extern "C" {                       /* engine internals (C, no extern-C guards of their own) */
#include "layout.h"
#include "sound.h"
#include "zylia_capture.h"         /* ZM-1 ASIO shell + ZpShared (pulls in zylia.h: tdoa/doa) */
}
#include "calib_capture.h"         /* sweep constants + the simulate/ASIO capture backends (Capture tab) */
#include "measure.h"               /* measurement DSP (self-guarded extern "C") */
#include "calib.h"                 /* trims solve + layout writeback (self-guarded extern "C") */

#include <atomic>
#include <thread>

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
    float  eqmagB[BW_CHANNELS][EQ_PTS];                          /* B too: reviewing what calibration WROTE */

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
        if (which) derive(L, V.gainB_db, V.delayB_ms, V.bx, V.by, V.bz, V.eqmagB);
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
static bool g_light;                                             /* theme (dark default; Tools menu toggles) */
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
    char lbl[32];
    if (V.hasA) {
        for (uint32_t i = 0; i < V.A.count; ++i) {
            if (!V.A.speakers[i].eq_len) continue;
            if (!V.eq_all && (int)i != V.sel) continue;
            any = true;
            snprintf(lbl, sizeof lbl, "A %u (%u taps)", i, V.A.speakers[i].eq_len);
            ImPlot::PlotLine(lbl, V.eqfreq, V.eqmagA[i], EQ_PTS);
        }
    }
    if (V.hasB) {                                                /* the filters calibration WROTE live in B */
        for (uint32_t i = 0; i < V.B.count; ++i) {
            if (!V.B.speakers[i].eq_len) continue;
            if (!V.eq_all && (int)i != V.sel) continue;
            any = true;
            snprintf(lbl, sizeof lbl, "B %u (%u taps)", i, V.B.speakers[i].eq_len);
            ImPlot::PlotLine(lbl, V.eqfreq, V.eqmagB[i], EQ_PTS);
        }
    }
    if (!any) ImPlot::Annotation(200, 0, ImVec4(1, 1, 1, 0.6f), ImVec2(0, 0), false,
                                 "no correction eq %s(run calibration with eq enabled)",
                                 V.eq_all ? "in the loaded layout(s) " : "on the selected speaker ");
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

/* ============ Capture tab — run the calibration sweep (bw_calibrate's core flow, in-window) ============
 * A worker thread runs sweep -> measure_response per speaker -> calib_solve -> writeback, using the
 * SAME calib_capture backends and measure/calib DSP as the CLI (simulate today; the ASIO full-duplex
 * path compiles in with the SDK and is exercised at the rig). Publication is row-at-a-time: the
 * worker fills a speaker's result fields, THEN bumps done_count (release); the UI only reads rows
 * below done_count (acquire) — live progress with no torn rows. On success the result loads straight
 * into the Diff view (A = input layout, B = what calibration wrote): capture -> review, one window. */
struct CapJob {
    char  layout[512], out[512], irprefix[512];      /* config (UI writes only while idle) */
    char  ran_layout[512], ran_out[512];             /* paths snapshotted at Run: the worker + the
                                                      * Load-into-Diff button use THESE, so edits made
                                                      * after the run can't change what gets diffed */
    float mic[3];
    int   mic_in;
    bool  simulate, do_room, do_eq, do_irs;
    char  driver[128];

    std::atomic<int>  state;                         /* 0 idle / 1 running / 2 done / 3 failed */
    std::atomic<int>  done_count;
    std::atomic<bool> cancel;
    std::atomic<int>  n;                             /* speaker count (worker sets; UI reads concurrently) */
    float    arrival_ms[BW_CHANNELS], level[BW_CHANNELS], rt60[BW_CHANNELS];
    uint16_t eqlen[BW_CHANNELS];
    float    gain_db[BW_CHANNELS], trim_ms[BW_CHANNELS];   /* the solve, valid when state == 2 */
    char     msg[256];

    std::thread th;
    bool     th_live;
};
static CapJob J;

static void cap_fail(const char* m) { snprintf(J.msg, sizeof J.msg, "%s", m); J.state.store(3, std::memory_order_release); }

static void cap_worker(void) {
    char err[256];
    Layout L;
    if (!layout_load(J.ran_layout, (uint32_t)CAL_FS, &L, err, sizeof err)) { cap_fail(err); return; }
    const int n = (int)L.count;
    J.n.store(n);
    static float sweep[CAL_NSWEEP], cap[CAL_CAPLEN], irbuf[CAL_IRLEN];   /* one job at a time; off the stack */
    static float eq_taps[(size_t)BW_CHANNELS * BW_EQ_TAPS];
    static uint16_t eq_lens[BW_CHANNELS];
    static MeasureResult res[BW_CHANNELS];
    memset(eq_lens, 0, sizeof eq_lens);
    measure_sweep(sweep, CAL_NSWEEP, CAL_F1, CAL_F2, CAL_FS);

#ifdef BW_HAVE_ASIO
    bool asio_up = false;
    if (!J.simulate) {
        if (calib_asio_open(J.driver[0] ? J.driver : NULL, J.mic_in, sweep, cap) != 0) { cap_fail("ASIO open failed (>=26 outs + the mic input; see console)"); return; }
        asio_up = true;
    }
#else
    if (!J.simulate) { cap_fail("built without the ASIO SDK - simulate only"); return; }
#endif

    const double band[2] = { CAL_BAND_LO, CAL_BAND_HI };
    for (int i = 0; i < n; ++i) {
        if (J.cancel.load(std::memory_order_relaxed)) {
#ifdef BW_HAVE_ASIO
            if (asio_up) calib_asio_close();
#endif
            cap_fail("cancelled");
            return;
        }
        if (J.simulate) calib_sim_capture(i, &L, J.mic, sweep, cap);
#ifdef BW_HAVE_ASIO
        else if (!calib_asio_capture(i)) { calib_asio_close(); cap_fail("capture timed out (speaker not wired? see console)"); return; }
#endif
        measure_response(cap, CAL_CAPLEN, sweep, CAL_NSWEEP, CAL_F1, CAL_F2, CAL_FS, band, &res[i]);
        J.arrival_ms[i] = (float)(((double)res[i].delay_samples + res[i].delay_frac) * 1000.0 / CAL_FS);
        J.level[i]      = res[i].level;
        J.rt60[i]       = 0.f;
        J.eqlen[i]      = 0;
        if (J.do_room || J.do_eq || J.do_irs) {
            RoomResult rr;
            int want_ir = (J.do_eq || J.do_irs);
            measure_room(cap, CAL_CAPLEN, sweep, CAL_NSWEEP, CAL_F1, CAL_F2, CAL_FS, &rr, want_ir ? irbuf : NULL, want_ir ? CAL_IRLEN : 0);
            J.rt60[i] = rr.rt60;
            if (J.do_irs && J.irprefix[0]) { char p[600]; snprintf(p, sizeof p, "%s_%02d.wav", J.irprefix, i); calib_write_wav_f32(p, irbuf, CAL_IRLEN, (int)CAL_FS); }
            if (J.do_eq) {                                       /* gate to before the first reflection, invert */
                int first_refl = rr.er_count ? rr.er_delay[0] : 0;
                if (calib_eq(irbuf, CAL_IRLEN, first_refl, CAL_FS, 256, &eq_taps[(size_t)i * BW_EQ_TAPS])) {
                    eq_lens[i] = 256; J.eqlen[i] = 256;
                }
            }
        }
        J.done_count.store(i + 1, std::memory_order_release);    /* publish the completed row */
    }
#ifdef BW_HAVE_ASIO
    if (asio_up) calib_asio_close();
#endif

    float gdb[BW_CHANNELS], dms[BW_CHANNELS];
    static float pos[BW_CHANNELS][3];                            /* calib_solve wants a packed [3]-stride array */
    for (int i = 0; i < n; ++i) { pos[i][0] = L.speakers[i].pos[0]; pos[i][1] = L.speakers[i].pos[1]; pos[i][2] = L.speakers[i].pos[2]; }
    calib_solve(res, pos, J.mic, n, CAL_FS, gdb, dms);
    memcpy(J.gain_db, gdb, sizeof gdb);
    memcpy(J.trim_ms, dms, sizeof dms);
    if (!calib_write_layout(J.ran_layout, J.ran_out, gdb, dms, n, err, sizeof err)) { cap_fail(err); return; }
    if (J.do_eq && !calib_write_eq(J.ran_out, J.ran_out, eq_taps, eq_lens, n, BW_EQ_TAPS, err, sizeof err)) { cap_fail(err); return; }
    snprintf(J.msg, sizeof J.msg, "wrote %s%s", J.ran_out, J.do_eq ? " (trims + eq)" : " (trims)");
    J.state.store(2, std::memory_order_release);
}

static void tab_capture(void) {
    int  st      = J.state.load(std::memory_order_acquire);
    bool running = (st == 1);
    if (!J.layout[0] && V.hasA) snprintf(J.layout, sizeof J.layout, "%s", V.pathA);   /* sensible defaults */
    if (!J.out[0]) snprintf(J.out, sizeof J.out, "calibrated.json");

    ImGui::BeginDisabled(running);
    ImGui::SetNextItemWidth(-uiScaled(240));
    ImGui::InputText("##cl", J.layout, sizeof J.layout);
    ImGui::SameLine(); if (ImGui::Button("...##cpl") && pick_file(J.layout, sizeof J.layout,
                          "layout json (*.json)\0*.json\0all files (*.*)\0*.*\0")) {}
    ImGui::SameLine(); ImGui::TextUnformatted("layout in");
    ImGui::SetNextItemWidth(-uiScaled(240));
    ImGui::InputText("##co", J.out, sizeof J.out);
    ImGui::SameLine(); ImGui::TextUnformatted("layout out (trims written here)");
    ImGui::SetNextItemWidth(uiScaled(220));
    ImGui::InputFloat3("mic (m)", J.mic, "%.2f");
    ImGui::SameLine(0, uiScaled(16)); ImGui::Checkbox("simulate", &J.simulate);
    ImGui::SameLine(); ImGui::Checkbox("room report", &J.do_room);
    ImGui::SameLine(); ImGui::Checkbox("eq", &J.do_eq);
    ImGui::SameLine(); ImGui::Checkbox("save IRs", &J.do_irs);
    if (J.do_irs) {
        ImGui::SameLine(); ImGui::SetNextItemWidth(uiScaled(140));
        ImGui::InputTextWithHint("##cirp", "ir prefix", J.irprefix, sizeof J.irprefix);
    }
#ifdef BW_HAVE_ASIO
    if (!J.simulate) {
        ImGui::SetNextItemWidth(uiScaled(160));
        ImGui::InputTextWithHint("##cdrv", "ASIO driver (auto)", J.driver, sizeof J.driver);
        ImGui::SameLine(); ImGui::SetNextItemWidth(uiScaled(90));
        ImGui::InputInt("mic input ch", &J.mic_in);
        if (J.mic_in < 0) J.mic_in = 0;                          /* channelNum -1 would reach the driver */
    }
#else
    if (!J.simulate) ImGui::TextDisabled("(built without the ASIO SDK: simulate only)");
#endif
    ImGui::EndDisabled();

    if (!running) {
        if (ImGui::Button("Run calibration")) {
            if (J.th_live) { J.th.join(); J.th_live = false; }   /* reap the previous run */
            snprintf(J.ran_layout, sizeof J.ran_layout, "%s", J.layout);   /* snapshot: this run's paths */
            snprintf(J.ran_out,    sizeof J.ran_out,    "%s", J.out);
            J.cancel.store(false); J.done_count.store(0); J.n.store(0); J.msg[0] = 0;
            J.state.store(1, std::memory_order_release);
            J.th = std::thread(cap_worker); J.th_live = true;
        }
    } else if (ImGui::Button("Cancel")) J.cancel.store(true);

    if (st == 0) { ImGui::TextDisabled("sweeps every speaker, solves the trims, writes the layout - then diff it right here"); return; }

    int dc = J.done_count.load(std::memory_order_acquire);
    int jn = J.n.load();
    int n  = jn > 0 ? jn : BW_CHANNELS;
    char ov[64]; snprintf(ov, sizeof ov, "%d / %d speakers", dc, n);
    ImGui::ProgressBar((float)dc / (float)n, ImVec2(-1, 0), ov);
    if (st == 3) ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f), "FAILED: %s", J.msg);
    if (st == 2) {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1.0f), "%s", J.msg);
        ImGui::SameLine(0, uiScaled(16));
        if (ImGui::Button("Load into Diff (A=input, B=result)")) {
            snprintf(V.pathA, sizeof V.pathA, "%s", J.ran_layout); load_layout(0);   /* the RUN's paths, not the */
            snprintf(V.pathB, sizeof V.pathB, "%s", J.ran_out);    load_layout(1);   /* possibly-edited fields  */
        }
    }
    if (ImGui::BeginTable("capt", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("spk"); ImGui::TableSetupColumn("arrival (ms)"); ImGui::TableSetupColumn("level");
        ImGui::TableSetupColumn("rt60 (s)"); ImGui::TableSetupColumn("gain trim (dB)"); ImGui::TableSetupColumn("delay trim (ms)");
        ImGui::TableSetupScrollFreeze(0, 1); ImGui::TableHeadersRow();
        for (int i = 0; i < dc; ++i) {                           /* only published rows */
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%2d%s", i, J.eqlen[i] ? " eq" : "");
            ImGui::TableNextColumn(); ImGui::Text("%8.3f", J.arrival_ms[i]);
            ImGui::TableNextColumn(); ImGui::Text("%7.4f", J.level[i]);
            ImGui::TableNextColumn(); if (J.rt60[i] > 0.f) ImGui::Text("%5.2f", J.rt60[i]); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (st == 2) ImGui::Text("%+6.2f", J.gain_db[i]); else ImGui::TextDisabled("...");
            ImGui::TableNextColumn(); if (st == 2) ImGui::Text("%7.3f", J.trim_ms[i]); else ImGui::TextDisabled("...");
        }
        ImGui::EndTable();
    }
}

/* ============ Zylia tab — ZM-1 bring-up + live clap DOA (the calibration-station seed) ============
 * The capture shell (zylia_capture.cpp) streams the 19 capsules and publishes transient snapshots;
 * this tab runs the SAME zylia_tdoa -> zylia_doa the speaker survey uses and draws the arrival on
 * the capsule sphere. Simulate mode synthesizes claps from a (walking) known direction through the
 * identical snapshot -> tdoa -> doa path — the hardware-free pipeline check, and what the test
 * drives ("Clap now" is deterministic: it uses the current truth direction). */
#define ZY_HIST 12

struct ZyState {
    ZpShared* live;                        /* non-NULL while the ASIO capture is open */
    ZpShared  sim;                         /* simulate-mode block (UI thread only) */
    bool      simulate, sim_walk;
    float     sim_t, sim_az;
    float     truth[3];                    /* last sim clap's true direction */
    long      last_seq;
    int       claps, rejects;
    struct { float dir[3]; float age; int valid; } hist[ZY_HIST];
    int       hist_n;
    float     last_dir[3];                 /* newest estimate (test hook) */
    int       last_valid;
    char      driver[128];
    float     dirs[ZYLIA_MICS][3]; float R;
    bool      geom_init;
};
static ZyState Z;

static float zy_az(const float d[3]) { return atan2f(d[0], -d[2]) * 57.29578f; }
static float zy_el(const float d[3]) { return asinf(d[1] > 1.f ? 1.f : (d[1] < -1.f ? -1.f : d[1])) * 57.29578f; }

/* a clap-like Gaussian click sampled at each capsule's exact fractional arrival time (the synthesis
 * the zylia unit test validates), landed in the shared block exactly like the ASIO side would */
static void zy_sim_clap(ZpShared* sh, const float dir[3]) {
    const double C = 343.0, FS = sh->rate, SIGMA = 1.0e-4, DIST = 2.0;
    unsigned int rng = (unsigned int)(sh->seq * 2654435761u + 12345u);
    double src[3] = { dir[0] * DIST, dir[1] * DIST, dir[2] * DIST };
    for (int ch = 0; ch < ZYLIA_MICS; ++ch) {
        double mx = Z.R * Z.dirs[ch][0] - src[0], my = Z.R * Z.dirs[ch][1] - src[1], mz = Z.R * Z.dirs[ch][2] - src[2];
        double t0 = 0.020 + sqrt(mx * mx + my * my + mz * mz) / C;
        for (int i = 0; i < ZP_SNAP_N; ++i) {
            double td = (double)i / FS - t0;
            double s  = exp(-0.5 * (td / SIGMA) * (td / SIGMA));
            rng = rng * 1664525u + 1013904223u;
            double nz = ((double)(int)(rng >> 9) / (double)(1 << 22) - 1.0) * 1e-3;
            sh->snap[ch][i] = (float)(0.7 * s + nz);
        }
        sh->rms[ch] = 0.25f;                                     /* kick the meters; drawing decays them */
    }
    sh->blocks++; sh->seq++;                                     /* same publish the ASIO side does */
}

static void zy_process(ZpShared* sh, float dt) {
    if (Z.simulate && !Z.live) {
        if (Z.sim_walk) {
            Z.sim_t += dt;
            if (Z.sim_t >= 1.5f) {
                Z.sim_t = 0.0f; Z.sim_az += 0.44f;               /* ~25 deg steps; elevation sweeps too */
                float el = 0.45f * sinf(0.8f * Z.sim_az);
                Z.truth[0] = cosf(el) * sinf(Z.sim_az); Z.truth[1] = sinf(el); Z.truth[2] = -cosf(el) * cosf(Z.sim_az);
                zy_sim_clap(sh, Z.truth);
            }
        }
        for (int ch = 0; ch < ZYLIA_MICS; ++ch) sh->rms[ch] *= expf(-4.0f * dt);   /* meter decay */
    }
    long seq = sh->seq;                                          /* fresh snapshot -> TDOA -> DOA */
    if (seq != Z.last_seq) {
        Z.last_seq = seq;
        static float snap[ZYLIA_MICS][ZP_SNAP_N];                /* local copy (static: 300 KB off the stack) */
        memcpy(snap, (const void*)sh->snap, sizeof snap);
        const float* ptr[ZYLIA_MICS];
        for (int ch = 0; ch < ZYLIA_MICS; ++ch) ptr[ch] = snap[ch];
        uint32_t max_lag = (uint32_t)(sh->rate * (2.0 * 0.049 / 343.0) * 2.0) + 4;   /* 2x the array's max TDOA */
        double arr[ZYLIA_MICS]; float dir[3];
        if (zylia_tdoa(ptr, ZP_SNAP_N, sh->rate, max_lag, arr) && zylia_doa(arr, dir)) {
            auto* h = &Z.hist[Z.hist_n++ % ZY_HIST];
            h->dir[0] = dir[0]; h->dir[1] = dir[1]; h->dir[2] = dir[2];
            h->age = 0.0f; h->valid = 1;
            memcpy(Z.last_dir, dir, sizeof Z.last_dir); Z.last_valid = 1;
            ++Z.claps;
            fprintf(stderr, "arrival %d: az %+.1f el %+.1f\n", Z.claps, zy_az(dir), zy_el(dir));
        } else ++Z.rejects;                                      /* not transient enough / degenerate solve */
    }
    for (int i = 0; i < ZY_HIST; ++i) if (Z.hist[i].valid) Z.hist[i].age += dt;
}

static void tab_zylia(void) {
    if (!Z.geom_init) {                                          /* lazy init: geometry + a fixed first truth */
        zylia_geometry(Z.dirs, &Z.R);
        Z.truth[0] = 0.62f; Z.truth[1] = 0.27f; Z.truth[2] = -0.74f;
        float m = sqrtf(Z.truth[0]*Z.truth[0] + Z.truth[1]*Z.truth[1] + Z.truth[2]*Z.truth[2]);
        Z.truth[0] /= m; Z.truth[1] /= m; Z.truth[2] /= m;
        Z.sim_walk = true;
        Z.geom_init = true;
    }

    /* -------- source controls -------- */
    if (ImGui::Checkbox("simulate claps", &Z.simulate) && Z.simulate) {
        memset((void*)&Z.sim, 0, sizeof Z.sim);
        Z.sim.nch = ZYLIA_MICS; Z.sim.rate = 48000.0; Z.sim.title = "simulate";
        Z.last_seq = 0; Z.sim_t = 1.0f;
    }
    if (Z.simulate && !Z.live) {
        ImGui::SameLine(); ImGui::Checkbox("walk", &Z.sim_walk);
        ImGui::SameLine(); if (ImGui::Button("Clap now")) zy_sim_clap(&Z.sim, Z.truth);
    }
#ifdef BW_HAVE_ASIO
    ImGui::SameLine(0, uiScaled(24));
    ImGui::SetNextItemWidth(uiScaled(160));
    ImGui::InputTextWithHint("##zydrv", "ASIO driver (auto)", Z.driver, sizeof Z.driver);
    ImGui::SameLine();
    if (!Z.live) { if (ImGui::Button("Open ZM-1")) { Z.live = zylia_capture_open(Z.driver[0] ? Z.driver : NULL, 48000.0);
                                                     if (Z.live) { Z.simulate = false; Z.last_seq = Z.live->seq; } } }
    else if (ImGui::Button("Close ZM-1")) { zylia_capture_close(); Z.live = NULL;
                                            Z.last_seq = Z.sim.seq; }   /* re-baseline: else the sim block's older
                                                                         * seq reprocesses a stale snapshot once */
#else
    ImGui::SameLine(0, uiScaled(24));
    ImGui::TextDisabled("(built without the ASIO SDK: simulate only)");
#endif
    ZpShared* sh = Z.live ? Z.live : (Z.simulate ? &Z.sim : NULL);
    if (!sh) {
        ImGui::TextDisabled("open the ZM-1 (or enable simulate) — clap anywhere around the array and a dot\n"
                            "appears on the capsule sphere where the clap came from (mapping + geometry check)");
        return;
    }
    zy_process(sh, ImGui::GetIO().DeltaTime);

    if (Z.last_valid)
        ImGui::Text("%s   %d ch @ %.0f Hz   blocks %ld   claps %d (rejected %d)   last: az %+.1f  el %+.1f",
                    sh->title ? sh->title : "?", sh->nch, sh->rate, sh->blocks, Z.claps, Z.rejects,
                    zy_az(Z.last_dir), zy_el(Z.last_dir));
    else
        ImGui::Text("%s   %d ch @ %.0f Hz   blocks %ld   waiting for a clap...",
                    sh->title ? sh->title : "?", sh->nch, sh->rate, sh->blocks);

    /* -------- capsule meters (left) + DOA sphere (right) -------- */
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (ImPlot::BeginPlot("##zymeters", ImVec2(uiScaled(240), avail.y))) {
        static float db[ZYLIA_MICS];
        for (int ch = 0; ch < ZYLIA_MICS; ++ch) {
            float r = sh->rms[ch];
            db[ch] = (r > 1e-6f) ? 20.0f * log10f(r) : -120.0f;
        }
        ImPlot::SetupAxes("capsule", "dBFS");
        ImPlot::SetupAxesLimits(-1, ZYLIA_MICS, -80, 0, ImPlotCond_Always);
        ImPlot::PlotBars("##rms", db, ZYLIA_MICS, 0.8);
        ImPlot::EndPlot();
    }
    ImGui::SameLine();
    if (ImPlot3D::BeginPlot("##zysphere", ImGui::GetContentRegionAvail(), ImPlot3DFlags_NoLegend)) {
        ImPlot3D::SetupAxes("x", "z", "y up");
        ImPlot3D::SetupAxesLimits(-1.4, 1.4, -1.4, 1.4, -1.4, 1.4, ImPlot3DCond_Once);
        const ImU32 wire = IM_COL32(110, 110, 128, 120);
        static float cx[33], cy[33], cz[33];
        for (int ring = 0; ring < 3; ++ring) {                   /* wire sphere: three great circles */
            for (int i = 0; i <= 32; ++i) {
                float a = (float)i / 32.0f * 6.2831853f, c = cosf(a), s = sinf(a);
                if      (ring == 0) { cx[i] = c;   cy[i] = s;   cz[i] = 0.f; }
                else if (ring == 1) { cx[i] = c;   cy[i] = 0.f; cz[i] = s;   }
                else                { cx[i] = 0.f; cy[i] = c;   cz[i] = s;   }
            }
            char id[8]; snprintf(id, sizeof id, "##w%d", ring);
            ImPlot3D::PlotLine(id, cx, cy, cz, 33, ImPlot3DSpec(ImPlot3DProp_LineColor, wire));
        }
        /* capsules: room (x, z, y-up) mapping, sized + lit by their live meter */
        static float px[ZYLIA_MICS], py[ZYLIA_MICS], pz[ZYLIA_MICS], psz[ZYLIA_MICS];
        static ImU32 pcol[ZYLIA_MICS];
        for (int ch = 0; ch < ZYLIA_MICS; ++ch) {
            px[ch] = Z.dirs[ch][0]; py[ch] = Z.dirs[ch][2]; pz[ch] = Z.dirs[ch][1];
            float r  = sh->rms[ch];
            float t  = ((r > 1e-6f) ? (20.0f * log10f(r) + 80.0f) / 80.0f : 0.0f);
            if (t < 0) t = 0; if (t > 1) t = 1;
            psz[ch]  = 3.0f + 6.0f * t;
            pcol[ch] = IM_COL32(70 + (int)(60 * t), 100 + (int)(155 * t), 130 + (int)(40 * t), 255);
        }
        ImPlot3D::PlotScatter("##caps", px, py, pz, ZYLIA_MICS,
                              ImPlot3DSpec(ImPlot3DProp_MarkerSizes, psz, ImPlot3DProp_MarkerFillColors, pcol));
        if (Z.simulate && !Z.live) {                             /* truth marker: the dot must land here */
            float tx = 1.18f * Z.truth[0], ty = 1.18f * Z.truth[2], tz = 1.18f * Z.truth[1];
            ImPlot3D::PlotScatter("##truth", &tx, &ty, &tz, 1,
                                  ImPlot3DSpec(ImPlot3DProp_MarkerSize, 9.0f,
                                               ImPlot3DProp_MarkerFillColor, IM_COL32(240, 240, 120, 90)));
        }
        for (int i = 0; i < ZY_HIST; ++i) {                      /* arrival dots, fading; the newest gets a ray */
            if (!Z.hist[i].valid || Z.hist[i].age > 6.0f) continue;
            float a = 1.0f - Z.hist[i].age / 6.0f;
            float hx = 1.15f * Z.hist[i].dir[0], hy = 1.15f * Z.hist[i].dir[2], hz = 1.15f * Z.hist[i].dir[1];
            char id[8]; snprintf(id, sizeof id, "##h%d", i);
            ImPlot3D::PlotScatter(id, &hx, &hy, &hz, 1,
                                  ImPlot3DSpec(ImPlot3DProp_MarkerSize, 4.0f + 3.0f * a,
                                               ImPlot3DProp_MarkerFillColor, IM_COL32(245, 120, 80, 60 + (int)(195 * a))));
            if (Z.hist[i].age < 1.5f) {
                float lx[2] = { 0, hx }, ly[2] = { 0, hy }, lz[2] = { 0, hz };
                ImPlot3D::PlotLine(id, lx, ly, lz, 2, ImPlot3DSpec(ImPlot3DProp_LineColor, IM_COL32(245, 140, 90, 200)));
            }
        }
        ImPlot3D::EndPlot();
    }
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
            if (ImGui::MenuItem("Light theme", NULL, &g_light)) applyTheme(g_light);
            ImGui::Separator();
            ImGui::MenuItem("ImGui demo", NULL, &show_imgui_demo);
            ImGui::MenuItem("ImPlot demo", NULL, &show_implot_demo);
            ImGui::MenuItem("Test engine", NULL, &show_te_ui);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    ImGui::BeginChild("side", ImVec2(uiScaled(340), 0), ImGuiChildFlags_Borders);
    const char* JSONF = "layout json (*.json)\0*.json\0all files (*.*)\0*.*\0";
    const char* WAVF  = "IR wav (*.wav)\0*.wav\0all files (*.*)\0*.*\0";
    ImGui::TextUnformatted("layout A (surveyed / before)");
    ImGui::SetNextItemWidth(-uiScaled(104));
    ImGui::InputText("##A", V.pathA, sizeof V.pathA);
    ImGui::SameLine(); if (ImGui::Button("...##pA") && pick_file(V.pathA, sizeof V.pathA, JSONF)) load_layout(0);
    ImGui::SameLine(); if (ImGui::Button("Load A")) load_layout(0);
    ImGui::TextUnformatted("layout B (calibrated / after)");
    ImGui::SetNextItemWidth(-uiScaled(104));
    ImGui::InputText("##B", V.pathB, sizeof V.pathB);
    ImGui::SameLine(); if (ImGui::Button("...##pB") && pick_file(V.pathB, sizeof V.pathB, JSONF)) load_layout(1);
    ImGui::SameLine(); if (ImGui::Button("Load B")) load_layout(1);
    ImGui::TextUnformatted("IR prefix (bw_calibrate --save-irs)");
    ImGui::SetNextItemWidth(-uiScaled(104));
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
        if (ImGui::BeginTabItem("IRs"))     { tab_irs();     ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Diff"))    { tab_diff();    ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Capture")) { tab_capture(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Zylia"))   { tab_zylia();   ImGui::EndTabItem(); }
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

    /* pure-logic checks ride the same suite (lsl-viewer's pattern: the test engine is the app's
     * whole harness, not just its UI driver) — no UI touched, still filterable via --tests logic. */
    t = IM_REGISTER_TEST(e, "logic", "wav_prefix");
    t->TestFunc = [](ImGuiTestContext*) {
        char a[64] = "caps/irs_07.wav"; wav_to_prefix(a); IM_CHECK_STR_EQ(a, "caps/irs");
        char b[64] = "plain.wav";       wav_to_prefix(b); IM_CHECK_STR_EQ(b, "plain");
        char c[64] = "a_7.wav";         wav_to_prefix(c); IM_CHECK_STR_EQ(c, "a_7");   /* one digit: not _NN */
        char d[64] = "noext";           wav_to_prefix(d); IM_CHECK_STR_EQ(d, "noext");
    };

    t = IM_REGISTER_TEST(e, "logic", "eq_magnitude");            /* unit FIR is 0 dB flat; 0.5 is -6 dB */
    t->TestFunc = [](ImGuiTestContext*) {
        float taps[2] = { 1.0f, 0.0f }, mag[EQ_PTS];
        eq_magnitude(taps, 2, 48000.0f, mag);
        for (int k = 0; k < EQ_PTS; ++k) IM_CHECK_LT(fabsf(mag[k]), 0.01f);
        taps[0] = 0.5f;
        eq_magnitude(taps, 2, 48000.0f, mag);
        for (int k = 0; k < EQ_PTS; ++k) IM_CHECK_LT(fabsf(mag[k] + 6.0206f), 0.01f);
    };

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

    t = IM_REGISTER_TEST(e, "capture", "simulate_run");          /* the whole calibration: sweep -> solve -> writeback -> diff */
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("calib view");
        ctx->ItemClick("**/Capture");
        ctx->Yield(2);
        ctx->ItemClick("**/##cl");   ctx->KeyCharsReplaceEnter(FIX_A);
        ctx->ItemClick("**/##co");   ctx->KeyCharsReplaceEnter("calibview_cap_out.json");
        ctx->ItemCheck("**/simulate");
        ctx->ItemClick("**/Run calibration");
        double t0 = ImGui::GetTime();                            /* wall-clock bound, not frames: unthrottled */
        while (J.state.load() == 1 && ImGui::GetTime() - t0 < 60.0) ctx->Yield();   /* frames can be sub-ms  */
        IM_CHECK_EQ(J.state.load(), 2);
        IM_CHECK_EQ(J.done_count.load(), 26);
        ctx->ItemClick("**/Load into Diff (A=input, B=result)");
        IM_CHECK(V.hasA);
        IM_CHECK(V.hasB);
        int trimmed = 0;                                          /* the sim sensitivity wobble must show up */
        for (int i = 0; i < 26; ++i) if (fabsf(V.gainB_db[i] - V.gainA_db[i]) > 0.1f) ++trimmed;
        IM_CHECK_GT(trimmed, 5);
        ctx->ItemClick("**/Diff");
        ctx->Yield(2);
        ctx->CaptureScreenshotWindow("//calib view");
    };

    t = IM_REGISTER_TEST(e, "zylia", "sim_doa");                 /* whole live-DOA pipeline through the real UI */
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("calib view");
        ctx->ItemClick("**/Zylia");
        ctx->Yield(2);
        ctx->ItemClick("**/simulate claps");
        ctx->ItemUncheck("**/walk");                             /* freeze truth so Clap now is deterministic */
        ctx->ItemClick("**/Clap now");
        ctx->Yield(4);                                           /* snapshot -> tdoa -> doa happens in-frame */
        IM_CHECK(Z.last_valid);
        double dot = (double)Z.last_dir[0] * Z.truth[0] + (double)Z.last_dir[1] * Z.truth[1] + (double)Z.last_dir[2] * Z.truth[2];
        double deg = acos(dot > 1.0 ? 1.0 : (dot < -1.0 ? -1.0 : dot)) * 57.29578;
        IM_CHECK_LT(deg, 2.0);                                   /* recovered direction lands on the truth ring */
        ctx->CaptureScreenshotWindow("//calib view");
    };

    t = IM_REGISTER_TEST(e, "viewer", "tabs");                   /* every tab renders without faulting */
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("calib view");
        const char* tabs[] = { "**/Array", "**/Trims", "**/EQ", "**/IRs", "**/Diff", "**/Capture", "**/Zylia" };
        for (int i = 0; i < 7; ++i) { ctx->ItemClick(tabs[i]); ctx->Yield(2); }
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
    char filter[64] = "";
    int  npos = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--tests") || !strcmp(argv[i], "--selftest")) {   /* --tests [filter], lsl-viewer style */
            selftest = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') snprintf(filter, sizeof filter, "%s", argv[++i]);
        }
        else if (!strcmp(argv[i], "--irs") && i + 1 < argc) snprintf(V.irprefix, sizeof V.irprefix, "%s", argv[++i]);
        else if (argv[i][0] != '-' && npos < 2) {
            if (npos++ == 0) snprintf(V.pathA, sizeof V.pathA, "%s", argv[i]);
            else             snprintf(V.pathB, sizeof V.pathB, "%s", argv[i]);
        }
        else { fprintf(stderr, "usage: calib_view [layoutA.json] [layoutB.json] [--irs prefix] [--tests [filter]]\n"); return 2; }
    }
    for (int k = 0; k < EQ_PTS; ++k) V.eqfreq[k] = 20.0f * powf(1000.0f, (float)k / (EQ_PTS - 1));
    J.simulate = true;                                            /* hardware capture is the rig-day opt-out */
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
    wc.hIcon = wc.hIconSm = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));   /* the .rc icon (title bar/taskbar) */
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
    g_uiScale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);         /* DPI rides FontScaleMain (bw_theme.h) */
    loadEmbeddedFont(io);                                         /* embedded Roboto, crisp via the 1.92 atlas */
    applyTheme(g_light);
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
    if (selftest) ImGuiTestEngine_QueueTests(g_te, ImGuiTestGroup_Tests, filter[0] ? filter : NULL,
                                             ImGuiTestRunFlags_RunFromCommandLine);

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
        ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];  /* clear matches the theme */
        const float clear[4] = { bg.x, bg.y, bg.z, 1.0f };
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
        printf("[tests] %d/%d passed\n", sum.CountSuccess, sum.CountTested);
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
    if (Z.live) zylia_capture_close();
    if (J.th_live) { J.cancel.store(true); J.th.join(); }        /* reap a still-running capture job */
    return rc;
}
