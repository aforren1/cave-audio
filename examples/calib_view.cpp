/*
 * calib_view.cpp — calibration report viewer (the Dear ImGui pilot: win32 + d3d11).
 *
 * bwa_calibrate writes trims/positions/EQ into cave_layout.json and (--save-irs) per-speaker IR wavs;
 * this views all of it BEFORE you trust it: the array in 3D, gain/delay trims as bars, correction-EQ
 * magnitude curves, the retained IRs, and — the workhorse — a layout DIFF (load the pre-calibration
 * layout as A and the written-back one as B) so a swapped channel, a bad mic placement, or a bogus
 * localize solve shows up as an absurd delta instead of an evening of confusion at the rig.
 *
 *   bwa_calib_view [layoutA.json] [layoutB.json]     # B optional: diff mode (the Aim tab aims A's speakers)
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
 * Data comes straight from the engine's own loader (layout.c via bwa_core) — the viewer can't drift
 * from what the engine would actually load. IR wavs decode through sound.c for the same reason.
 */
#include "imgui.h"
#include "implot.h"
#include "implot3d.h"
#include "bwa_theme.h"      /* lsl-viewer's theme + embedded Roboto (applyTheme / loadEmbeddedFont / uiScaled) */
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "imgui_te_engine.h"
#include "imgui_te_context.h"
#include "imgui_te_ui.h"

extern "C" {                       /* engine internals (C, no extern-C guards of their own) */
#include "core/layout.h"
#include "core/sound.h"
#include "dsp/sos.h"                   /* room-temperature speed of sound (the capsule survey scales with it) */
#include "zylia_capture.h"         /* ZM-1 ASIO shell + ZpShared (pulls in zylia.h: tdoa/doa) */
}
#include "calib_capture.h"         /* sweep constants + the simulate/ASIO capture backends (Capture tab) */
#include "calib/measure.h"               /* measurement DSP (self-guarded extern "C") */
#include "calib/calib.h"                 /* trims solve + layout writeback (self-guarded extern "C") */
#include "mic_track.h"             /* the Placement panel: the ZM-1's stand as a tracked rigid body */

#include <atomic>
#include <chrono>
#include <mutex>
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
#define IR_FS  48000u              /* bwa_calibrate writes IRs at the engine rate */

struct View {
    char   pathA[512], pathB[512], irprefix[512];
    Layout A, B;
    bool   hasA, hasB;
    char   status[512];

    /* derived, refreshed on load (not per frame) */
    float  gainA_db[BWA_MAX_CHANNELS], delayA_ms[BWA_MAX_CHANNELS];
    float  gainB_db[BWA_MAX_CHANNELS], delayB_ms[BWA_MAX_CHANNELS];
    float  ax[BWA_MAX_CHANNELS], ay[BWA_MAX_CHANNELS], az[BWA_MAX_CHANNELS];    /* 3D plot coords (room x, z, y-up) */
    float  bx[BWA_MAX_CHANNELS], by[BWA_MAX_CHANNELS], bz[BWA_MAX_CHANNELS];
    float  dpos_mm[BWA_MAX_CHANNELS];
    float  eqfreq[EQ_PTS];
    float  eqmagA[BWA_MAX_CHANNELS][EQ_PTS];                          /* dB; only valid where eq_len > 0 */
    float  eqmagB[BWA_MAX_CHANNELS][EQ_PTS];                          /* B too: reviewing what calibration WROTE */

    SoundData ir[BWA_MAX_CHANNELS];
    bool      hasIR[BWA_MAX_CHANNELS];
    float     ir_ms[BWA_MAX_CHANNELS];                                /* peak time of each loaded IR */
    int       ir_n;

    int    sel;                                                  /* selected speaker */
    bool   eq_all;                                               /* overlay every eq curve */
    bool   diffable;                                             /* A and B have the SAME speaker count —
                                                                  * comparing different arrays is meaningless */
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
    V.diffable = V.hasA && V.hasB && V.A.count == V.B.count;     /* per-speaker deltas need one array */
    if (!V.diffable) return;
    for (uint32_t i = 0; i < V.A.count; ++i) {
        float dx = V.A.speakers[i].pos[0] - V.B.speakers[i].pos[0];
        float dy = V.A.speakers[i].pos[1] - V.B.speakers[i].pos[1];
        float dz = V.A.speakers[i].pos[2] - V.B.speakers[i].pos[2];
        V.dpos_mm[i] = sqrtf(dx * dx + dy * dy + dz * dz) * 1000.0f;
    }
}

/* Room-temperature speed of sound, inherited from layout A's reference.speed_of_sound_mps
 * (bwa_calibrate records it there — docs/calibration.md, "Air temperature"). The ZM-1 capsule survey
 * solves geometry from arrival TIMES, so the solved radius scales directly with c: 1% of c is 0.5 mm
 * on the 49 mm array, the same order as the capsule error that tab exists to measure. The synthetic
 * clap generator reads the SAME value on purpose — if the two disagreed, simulate mode would report
 * a residual that is really just a temperature mismatch. */
static double g_room_sos = BWA_SOS_REF_MPS;

static void load_layout(int which) {                             /* 0 = A, 1 = B */
    Layout* L    = which ? &V.B : &V.A;
    bool*   has  = which ? &V.hasB : &V.hasA;
    char*   path = which ? V.pathB : V.pathA;
    char err[256];
    *has = layout_load(path, IR_FS, L, err, sizeof err);
    /* A is the rig's layout (the survey autofills clap positions from it), so it also carries the
     * rig's speed of sound. Absent from the file = the 20 C reference, which is what the tab
     * defaulted to before this field existed. */
    if (*has && !which && !calib_read_sos(path, &g_room_sos)) g_room_sos = BWA_SOS_REF_MPS;
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
    uint32_t n = V.hasA ? V.A.count : BWA_MAX_CHANNELS;
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
    /* NoPan: every axis auto-fits, and a re-fit overwrites the pan translation on the next
       frame, so the view snaps back mid-drag. Rotate + zoom still read naturally. */
    if (!ImPlot3D::BeginPlot("##array", ImGui::GetContentRegionAvail(), ImPlot3DFlags_NoPan)) return;
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
        static float seg[3][2 * BWA_MAX_CHANNELS];                    /* A->B delta segments (same array only) */
        if (V.diffable) {
        for (uint32_t i = 0; i < V.B.count; ++i) {
            seg[0][2*i] = V.ax[i]; seg[0][2*i+1] = V.bx[i];
            seg[1][2*i] = V.ay[i]; seg[1][2*i+1] = V.by[i];
            seg[2][2*i] = V.az[i]; seg[2][2*i+1] = V.bz[i];
        }
        ImPlot3D::PlotLine("A->B", seg[0], seg[1], seg[2], (int)(2 * V.B.count),
                           ImPlot3DSpec(ImPlot3DProp_Flags, ImPlot3DLineFlags_Segments));
        }
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
    bwTip("draw every speaker's correction curve at once - an outlier (bad capture, dead driver) pops out");
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
                            "(bwa_calibrate --save-irs <prefix> writes <prefix>_NN.wav)", V.sel);
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
    if (!V.hasA || !V.hasB) { ImGui::TextDisabled("load a second layout as B to diff (e.g. the file bwa_calibrate wrote)"); return; }
    if (!V.diffable) {                                           /* different arrays: a per-speaker diff is meaningless */
        ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f),
                           "A has %u speakers, B has %u - these are different arrays, not a before/after.",
                           V.A.count, V.B.count);
        ImGui::TextDisabled("load two layouts of the same array (calibration never changes the speaker count)");
        return;
    }
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

/* ============ Placement panel: the tracked ZM-1, shared by the Capture and Aim tabs ============
 * The same stand bwa_calibrate --track follows (mic_track.cpp over placement.c), on its own poller
 * thread: the thread opens NatNet (which can block on the handshake, so the GUI never does), polls
 * the pose at about 100 Hz, runs the gate, and publishes a snapshot under a mutex. A run (Capture or
 * Aim) started while it is live waits in ITS worker for the gate, takes the measured center instead
 * of the typed field, and after every capture notes it (the simulated bump is keyed on the count) and
 * compares a FRESH center (two polls newer than the note, ordering rather than timing) against the one
 * it took. The panel itself only reads snapshots. Unverified against live Motive. */
struct PlaceSnap {
    bool      have_pose;
    float     q[4];
    PlaceGate g;                         /* the gate after the last poll: state, center, mean, delta */
    float     yaw_deg, tilt_deg;
    bool      have_truth; float truth[3];/* simulate: the stand's true center */
    unsigned long long seq;              /* polls published */
};

struct PlaceJob {
    /* config: the UI writes these only while disconnected */
    char  body[64], server[64], multicast[64], survey[512];
    bool  sim, sim_bump;
    int   offset_mode;                   /* 0 = the x,y,z field, 1 = ring */
    float offset[3];
    /* live settings: the UI writes, the poller reads, under mu */
    float tol_mm;
    float target[3]; bool target_set; char target_for[512];
    std::atomic<int>  state;             /* 0 disconnected / 1 connecting / 2 live / 3 failed */
    std::atomic<bool> stop;
    char  msg[400], desc[800];
    bool  body_frame;                    /* valid while live */
    std::thread th; bool th_live;
    std::mutex mu;
    PlaceSnap d;
    MicTrack  mt;                        /* the poller owns it; runs only note captures and re-aim */
};
static PlaceJob PL;
#define PL_SIM_BUMP_AFTER 3              /* the simulated knock: after the third capture */

static bool pl_live(void) { return PL.state.load(std::memory_order_acquire) == 2; }
static PlaceSnap pl_snap(void) { std::lock_guard<std::mutex> lk(PL.mu); return PL.d; }

static void pl_worker(void) {
    MicTrackCfg mc;
    memset(&mc, 0, sizeof mc);
    mc.body = PL.body; mc.server = PL.server[0] ? PL.server : NULL; mc.multicast = PL.multicast[0] ? PL.multicast : NULL;
    mc.survey_path = PL.survey[0] ? PL.survey : NULL;
    mc.have_offset = PL.offset_mode == 0;
    memcpy(mc.offset_m, PL.offset, sizeof mc.offset_m);
    mc.offset_ring = PL.offset_mode == 1;
    mc.sim = PL.sim ? MIC_SIM_SCRIPT : MIC_SIM_OFF;
    mc.sim_bump_after = (PL.sim && PL.sim_bump) ? PL_SIM_BUMP_AFTER : 0;
    char e[400] = { 0 };
    if (mic_track_open(&PL.mt, &mc, e, sizeof e) != 0) {
        snprintf(PL.msg, sizeof PL.msg, "%s", e);
        PL.state.store(3, std::memory_order_release);
        return;
    }
    mic_track_describe(&PL.mt, PL.desc, sizeof PL.desc);
    PL.body_frame = PL.mt.body_frame != 0;
    PlaceCfg cfg;
    float tgt[3], tol;
    { std::lock_guard<std::mutex> lk(PL.mu); memcpy(tgt, PL.target, sizeof tgt); tol = PL.tol_mm; memset(&PL.d, 0, sizeof PL.d); }
    place_cfg_default(&cfg, tol * 1e-3f);
    PlaceGate g;
    place_gate_init(&g, &cfg);
    mic_track_sim_target(&PL.mt, tgt);
    PL.msg[0] = 0;
    PL.state.store(2, std::memory_order_release);
    while (!PL.stop.load(std::memory_order_relaxed)) {
        float nt[3], ntol;
        { std::lock_guard<std::mutex> lk(PL.mu); memcpy(nt, PL.target, sizeof nt); ntol = PL.tol_mm; }
        if (memcmp(nt, tgt, sizeof tgt) != 0) {              /* a new target: the simulated stand walks there */
            memcpy(tgt, nt, sizeof tgt);
            mic_track_sim_target(&PL.mt, tgt);
            place_gate_reset(&g);
        }
        if (ntol != tol) { tol = ntol; place_cfg_default(&cfg, tol * 1e-3f); place_gate_init(&g, &cfg); }
        MicPose ps;
        const bool have = mic_track_read(&PL.mt, &ps) != 0;
        place_gate_update(&g, mic_track_now(&PL.mt), have ? ps.center : NULL, tgt);
        PlaceSnap s;
        memset(&s, 0, sizeof s);
        s.have_pose = have;
        if (have) {
            memcpy(s.q, ps.q, sizeof s.q);
            place_mount_angles(ps.q, &s.yaw_deg, &s.tilt_deg);
        }
        s.g = g;
        s.have_truth = mic_track_sim_truth(&PL.mt, s.truth) != 0;
        {
            std::lock_guard<std::mutex> lk(PL.mu);
            s.seq = PL.d.seq + 1;
            PL.d = s;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    mic_track_close(&PL.mt);
    PL.state.store(0, std::memory_order_release);
}

static void pl_connect(void) {
    if (PL.th_live) { PL.th.join(); PL.th_live = false; }
    PL.stop.store(false);
    PL.msg[0] = 0; PL.desc[0] = 0;
    PL.state.store(1, std::memory_order_release);
    PL.th = std::thread(pl_worker); PL.th_live = true;
}

/* What a run took from the panel: the center, the delta it used, and whether it moved. */
struct PlaceTake {
    bool  used;                          /* this run took its center from the tracker */
    float center[3], target[3], delta_mm[3], dist_mm;
    float tol_m;
    bool  bumped; float bump_mm; int bump_after;
    bool  have_truth; float truth[3];    /* simulate: the truth at the take (the tests read it) */
};

/* In a run's worker: wait for the gate, then take the window mean. 0 = taken; 1 = canceled; 2 = lost
 * the tracker; 3 = timed out. */
static int pl_take(PlaceTake* tk, const std::atomic<bool>& cancel, double timeout_s, float q_out[4]) {
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        if (cancel.load(std::memory_order_relaxed)) return 1;
        if (!pl_live()) return 2;
        PlaceSnap s = pl_snap();
        if (s.g.state == PLACE_OK) {
            float tgt[3];
            { std::lock_guard<std::mutex> lk(PL.mu); memcpy(tgt, PL.target, sizeof tgt); tk->tol_m = PL.tol_mm * 1e-3f; }
            memcpy(tk->center, s.g.mean, sizeof tk->center);
            memcpy(tk->target, tgt, sizeof tk->target);
            for (int a = 0; a < 3; ++a) tk->delta_mm[a] = (s.g.mean[a] - tgt[a]) * 1e3f;
            tk->dist_mm = s.g.mean_dist_m * 1e3f;
            tk->have_truth = s.have_truth;
            memcpy(tk->truth, s.truth, sizeof tk->truth);
            memcpy(q_out, s.q, 4 * sizeof(float));
            tk->used = true; tk->bumped = false; tk->bump_mm = 0.f; tk->bump_after = -1;
            return 0;
        }
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > timeout_s) return 3;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

/* In a run's worker, after capture `idx`: note it, wait for a fresh center, compare. Returns 1 when
 * the mic moved past half the tolerance (tk records it); 0 otherwise, including no fresh pose. The
 * simulated truth for the NEXT capture comes back in truth_out (when simulating). */
static int pl_after_capture(PlaceTake* tk, int idx, double capture_s, float truth_out[3]) {
    unsigned long long s0;
    { std::lock_guard<std::mutex> lk(PL.mu); s0 = PL.d.seq; }
    mic_track_note_capture(&PL.mt, capture_s);
    for (int tries = 0; tries < 100 && pl_live(); ++tries) {   /* up to about a second */
        PlaceSnap s = pl_snap();
        if (s.seq >= s0 + 2) {
            if (s.have_truth && truth_out) memcpy(truth_out, s.truth, 3 * sizeof(float));
            if (!s.have_pose) return 0;
            float moved = 0.f;
            if (place_bump(tk->center, s.g.center, tk->tol_m, &moved) == 1) {
                tk->bumped = true; tk->bump_mm = moved * 1e3f; tk->bump_after = idx;
                return 1;
            }
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return 0;
}

/* The panel. `ref` is the tab's layout listening point (NULL = none), `ref_for` names that layout so
 * the target follows it until the user edits the field. */
static void placement_panel(const float* ref, const char* ref_for, bool run_busy) {
    if (!ImGui::CollapsingHeader("Placement (tracked ZM-1)")) return;
    ImGui::PushID("placement");
    const int st = PL.state.load(std::memory_order_acquire);
    if (PL.th_live && (st == 0 || st == 3)) { PL.th.join(); PL.th_live = false; }   /* the thread has ended */
    if (PL.tol_mm <= 0.f) PL.tol_mm = PLACE_TOL_DEFAULT_M * 1e3f;
    if (!PL.multicast[0]) snprintf(PL.multicast, sizeof PL.multicast, "239.255.42.99");
    if (ref && ref_for && strcmp(PL.target_for, ref_for) != 0) {
        snprintf(PL.target_for, sizeof PL.target_for, "%s", ref_for);
        if (!PL.target_set) { std::lock_guard<std::mutex> lk(PL.mu); memcpy(PL.target, ref, sizeof PL.target); }
    }
    const bool connected = st == 1 || st == 2;

    ImGui::BeginDisabled(connected);
    ImGui::SetNextItemWidth(uiScaled(110));
    ImGui::InputTextWithHint("##plbody", "rigid body", PL.body, sizeof PL.body);
    bwTip("the stand's rigid body: its streaming id, or its name (a name needs the server)");
    ImGui::SameLine(); ImGui::SetNextItemWidth(uiScaled(120));
    ImGui::InputTextWithHint("##plsrv", "Motive IP", PL.server, sizeof PL.server);
    bwTip("Motive's host (numeric IPv4). Needed to track by name and for the ring offset");
    ImGui::SameLine(); ImGui::SetNextItemWidth(uiScaled(110));
    ImGui::InputText("##plmc", PL.multicast, sizeof PL.multicast);
    bwTip("NatNet multicast group");
    ImGui::SameLine(); ImGui::Checkbox("simulate##pl", &PL.sim);
    bwTip("no Motive: a scripted stand walks in from 8 cm off the target over 2 s and settles about 5 mm off it");
    if (PL.sim) { ImGui::SameLine(); ImGui::Checkbox("bump mid-run##pl", &PL.sim_bump);
                  bwTip("knock the simulated stand 15 mm after the third capture of a run"); }
    ImGui::SetNextItemWidth(-uiScaled(300));
    ImGui::InputTextWithHint("##plsurvey", "survey (optional)", PL.survey, sizeof PL.survey);
    bwTip("a capsule survey. BODY-FRAME: it carries the mount offset and follows the stand (the Aim tab's position "
          "readout needs one). Room axes: channel order and geometry only. Trims and the tilt meter need no survey");
    ImGui::SameLine(); if (ImGui::Button("...##plsv")) pick_file(PL.survey, sizeof PL.survey, "survey json (*.json)\0*.json\0all files (*.*)\0*.*\0");
    ImGui::SameLine(); ImGui::RadioButton("offset##plo", &PL.offset_mode, 0);
    ImGui::SameLine(); ImGui::SetNextItemWidth(uiScaled(170));
    ImGui::BeginDisabled(PL.offset_mode != 0);
    ImGui::InputFloat3("##ploff", PL.offset, "%.3f");
    ImGui::EndDisabled();
    bwTip("body origin to the array center, in the rigid body's axes (m). 0 = you moved Motive's pivot to the "
          "array center. A body-frame survey's own offset wins");
    ImGui::SameLine(); ImGui::RadioButton("ring##plo", &PL.offset_mode, 1);
    bwTip("fit the circle through the body's markers (Motive's model definition, needs the server): for markers in "
          "a ring around the housing's equator. It assumes the ring sits at the array center's height");
    ImGui::EndDisabled();

    if (!connected) { if (ImGui::Button("Connect##pl")) pl_connect(); }
    else {
        ImGui::BeginDisabled(run_busy);
        if (ImGui::Button("Disconnect##pl")) PL.stop.store(true);
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (st == 1) ImGui::TextDisabled("connecting...");
    else if (st == 3) ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f), "FAILED: %s", PL.msg);
    else if (st == 0) ImGui::TextDisabled("not tracking: runs use the typed position");
    else ImGui::TextDisabled("%s", PL.sim ? "tracking the SIMULATED stand" : "tracking (unverified against live Motive)");

    ImGui::SetNextItemWidth(uiScaled(220));
    float tol = PL.tol_mm;
    if (ImGui::SliderFloat("tolerance (mm)##pl", &tol, 1.f, 50.f, "%.0f")) { std::lock_guard<std::mutex> lk(PL.mu); PL.tol_mm = tol; }
    bwTip("how close the center must sit to the target: about 10 mm for trims and verify (1 cm is up to 29 us of "
          "arrival), a guide only for localize positions. A bump is a move past half of it");
    float tg[3];
    { std::lock_guard<std::mutex> lk(PL.mu); memcpy(tg, PL.target, sizeof tg); }
    ImGui::SetNextItemWidth(uiScaled(220));
    if (ImGui::InputFloat3("target (m)##pl", tg, "%.3f")) {
        std::lock_guard<std::mutex> lk(PL.mu); memcpy(PL.target, tg, sizeof tg); PL.target_set = true;
    }
    bwTip("where the ZM-1's center should be, room coordinates. Defaults to the layout's listening point");
    if (PL.target_set && ref) {
        ImGui::SameLine();
        if (ImGui::SmallButton("use listening point##pl")) {
            std::lock_guard<std::mutex> lk(PL.mu); memcpy(PL.target, ref, sizeof PL.target); PL.target_set = false;
        }
    }
    if (st == 2) {
        const PlaceSnap s = pl_snap();
        const bool ok = s.g.state == PLACE_OK;
        const ImVec4 col = ok ? ImVec4(0.45f, 0.9f, 0.5f, 1.f) : (s.have_pose ? ImVec4(0.95f, 0.8f, 0.35f, 1.f) : ImVec4(1.f, 0.42f, 0.42f, 1.f));
        const float fs0 = ImGui::GetStyle().FontSizeBase;
        ImGui::BeginGroup();
        ImGui::PushFont(NULL, fs0 * 3.0f);
        if (s.have_pose) ImGui::TextColored(col, "%.1f mm", s.g.dist_m * 1e3f);
        else             ImGui::TextColored(col, "no pose");
        ImGui::PopFont();
        ImGui::TextColored(col, "%s", ok ? "OK: in tolerance and still" : (s.have_pose ? place_state_name(s.g.state) : "occluded, wrong id, or not streaming"));
        ImGui::EndGroup();
        ImGui::SameLine(0, uiScaled(24));
        /* the compass: seen from above, screen up = the front (+z), screen right = room-right (-x) */
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float R = uiScaled(34);
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const ImVec2 c(p0.x + R + 2, p0.y + R + 2);
            dl->AddCircle(c, R, IM_COL32(120, 120, 140, 255), 32, 1.5f);
            dl->AddText(ImVec2(c.x - uiScaled(14), p0.y - 2), IM_COL32(160, 160, 170, 255), "front");
            if (s.have_pose) {
                const float mx = -s.g.delta[0], mz = -s.g.delta[2];          /* the move = target - center */
                const float h = sqrtf(mx * mx + mz * mz);
                if (h > 5e-4f) {
                    const float ux = -mx / h, uy = -mz / h;                   /* screen: right = -x, up = +z */
                    const float L = R * (h > 0.02f ? 0.9f : 0.35f + 0.55f * h / 0.02f);
                    const ImVec2 tip(c.x + ux * L, c.y + uy * L);
                    dl->AddLine(c, tip, ImGui::GetColorU32(col), 3.f);
                    const ImVec2 nrm(-uy, ux);
                    dl->AddTriangleFilled(tip, ImVec2(tip.x - ux * 9 + nrm.x * 6, tip.y - uy * 9 + nrm.y * 6),
                                          ImVec2(tip.x - ux * 9 - nrm.x * 6, tip.y - uy * 9 - nrm.y * 6), ImGui::GetColorU32(col));
                }
                /* the height bar: up or down */
                const float my = -s.g.delta[1];
                const float bx = c.x + R + uiScaled(18);
                dl->AddLine(ImVec2(bx, c.y - R), ImVec2(bx, c.y + R), IM_COL32(120, 120, 140, 255), 1.5f);
                if (fabsf(my) > 5e-4f) {
                    const float L = R * (fabsf(my) > 0.02f ? 0.9f : 0.35f + 0.55f * fabsf(my) / 0.02f);
                    const float sy = my > 0.f ? -1.f : 1.f;
                    dl->AddLine(ImVec2(bx, c.y), ImVec2(bx, c.y + sy * L), ImGui::GetColorU32(col), 3.f);
                    dl->AddTriangleFilled(ImVec2(bx, c.y + sy * L), ImVec2(bx - 6, c.y + sy * (L - 9)), ImVec2(bx + 6, c.y + sy * (L - 9)),
                                          ImGui::GetColorU32(col));
                }
            }
            ImGui::Dummy(ImVec2(2 * R + uiScaled(30), 2 * R + 4));
        }
        ImGui::SameLine(0, uiScaled(16));
        ImGui::BeginGroup();
        if (s.have_pose) {
            char words[128];
            mic_track_move_words(s.g.delta, words, sizeof words);
            ImGui::Text("%s", words);
            ImGui::Text("dx %+.1f  dy %+.1f  dz %+.1f mm  (center minus target)", s.g.delta[0] * 1e3f, s.g.delta[1] * 1e3f, s.g.delta[2] * 1e3f);
            ImGui::Text("center (%.3f %.3f %.3f)  spread %.1f mm", s.g.center[0], s.g.center[1], s.g.center[2], s.g.spread_m * 1e3f);
            ImGui::Text("mount yaw %.1f deg, tilt %.1f deg%s", s.yaw_deg, s.tilt_deg, PL.body_frame ? ", capsule table follows the stand" : "");
            if (s.g.state == PLACE_SETTLING) ImGui::TextDisabled("held %.1f of %.1f s", s.g.held_s, s.g.cfg.hold_s);
        }
        ImGui::EndGroup();
        if (PL.desc[0]) ImGui::TextDisabled("%s", PL.desc);
    }
    ImGui::PopID();
    ImGui::Separator();
}

/* the line a tab shows about what its last run took */
static void place_take_line(const PlaceTake& tk) {
    if (!tk.used) return;
    ImGui::Text("tracked: the run took the center (%.4f %.4f %.4f), %+.1f %+.1f %+.1f mm from the target (%.1f mm)",
                tk.center[0], tk.center[1], tk.center[2], tk.delta_mm[0], tk.delta_mm[1], tk.delta_mm[2], tk.dist_mm);
    if (tk.bumped)
        ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f), "BUMP: the ZM-1 moved %.1f mm after capture %d (limit %.1f mm): "
                           "re-place it and run again", tk.bump_mm, tk.bump_after, tk.tol_m * 0.5e3f);
}

/* ============ Capture tab — run the calibration sweep (bwa_calibrate's core flow, in-window) ============
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
    /* The directivity re-aim needs the mic's REAL position, the bwa_calibrate --mic rule: until the
     * user sets the field it follows the loaded layout's listening point, and the correction stays
     * off (a guessed bearing puts fictitious dB into the trims). mic_for is the layout path the
     * default was taken from; dir_note says which case the UI line shows. */
    bool  mic_set;                                   /* the user edited the mic field */
    bool  ran_mic_set;                               /* ... snapshotted at Run, for the worker */
    char  mic_for[512];
    bool  have_ref, have_model;                      /* what the layout at mic_for carries */
    float mic_ref[3];
    int   dir_note;                                  /* 0 no model, 1 model + mic unset (off), 2 model + mic set (on) */
    bool  corr_applied;                              /* the last run re-aimed the trims (valid when state == 2) */
    bool  ran_tracked;                               /* the Placement panel was live at Run: take its center */
    float mic_run[3];                                /* the worker's mic: the field at Run, or the tracked center */
    PlaceTake tk;                                    /* what the run took (read by the UI once state != 1) */
    std::atomic<int> phase;                          /* 1 = waiting for the placement gate */
    unsigned mic_scope_id;                           /* ImGui ID scope of the mic field (the test drives it) */

    std::atomic<int>  state;                         /* 0 idle / 1 running / 2 done / 3 failed */
    std::atomic<int>  done_count;
    std::atomic<bool> cancel;
    std::atomic<int>  n;                             /* speaker count (worker sets; UI reads concurrently) */
    float    arrival_ms[BWA_MAX_CHANNELS], level[BWA_MAX_CHANNELS], rt60[BWA_MAX_CHANNELS];
    uint16_t eqlen[BWA_MAX_CHANNELS];
    float    gain_db[BWA_MAX_CHANNELS], trim_ms[BWA_MAX_CHANNELS];   /* the solve, valid when state == 2 */
    char     msg[256];

    std::thread th;
    bool     th_live;
};
static CapJob J;

/* registered-driver picker: a no-preview dropdown beside a driver text field, filled from
 * calib_capture's registry enumeration (a fresh scan while the combo is open; loads nothing and
 * needs no session slot, so it is safe while a capture streams). Shared by the Capture and Zylia
 * tabs; without the ASIO SDK the enumeration returns 0 and the combo says so. */
static void driver_pick(const char* id, char* buf, size_t cap) {
    ImGui::SameLine();
    if (ImGui::BeginCombo(id, "", ImGuiComboFlags_NoPreview)) {
        char names[32][32];
        int nd = calib_asio_driver_names(names, 32);
        if (nd == 0) ImGui::TextDisabled("(no registered drivers)");
        for (int i = 0; i < nd; ++i)
            if (ImGui::Selectable(names[i], false)) snprintf(buf, cap, "%s", names[i]);
        ImGui::EndCombo();
    }
    bwTip("pick a registered ASIO driver (fills the field; clear the field for auto-pick)");
}

static bool aim_running(void);        /* the Aim tab's worker (below) owns the same shell and simulator */

static void cap_fail(const char* m) { snprintf(J.msg, sizeof J.msg, "%s", m); J.state.store(3, std::memory_order_release); }

static void cap_worker(void) {
    char err[256];
    static Layout L;          /* never a stack local (layout.h); one capture job at a time */
    if (!layout_load(J.ran_layout, (uint32_t)CAL_FS, &L, err, sizeof err)) { cap_fail(err); return; }
    const int n = (int)L.count;
    J.n.store(n);
    static float sweep[CAL_NSWEEP], cap[CAL_CAPLEN], irbuf[CAL_IRLEN];   /* one job at a time; off the stack */
    static float eq_taps[(size_t)BWA_MAX_CHANNELS * BWA_EQ_TAPS];
    static uint16_t eq_lens[BWA_MAX_CHANNELS];
    static MeasureResult res[BWA_MAX_CHANNELS];
    memset(eq_lens, 0, sizeof eq_lens);
    measure_sweep(sweep, CAL_NSWEEP, CAL_F1, CAL_F2, CAL_FS);

    /* the Placement panel was live at Run: wait for its gate, then the measured center IS the mic */
    J.tk.used = false;
    if (J.ran_tracked) {
        J.phase.store(1);
        float q[4];
        const int r = pl_take(&J.tk, J.cancel, 300.0, q);
        J.phase.store(0);
        if (r) {
            cap_fail(r == 1 ? "canceled" : r == 2 ? "the tracker went away before the placement settled"
                                                  : "placement timed out (300 s): the ZM-1 never sat in tolerance and still");
            return;
        }
        memcpy(J.mic_run, J.tk.center, sizeof J.mic_run);
    }

#ifdef BWA_HAVE_ASIO
    bool asio_up = false;
    if (!J.simulate) {
        if (calib_asio_open(J.driver[0] ? J.driver : NULL, J.mic_in, n, sweep, cap) != 0) {
            char m[96]; snprintf(m, sizeof m, "ASIO open failed (>=%d outs + the mic input; see console)", n);
            cap_fail(m); return;
        }
        asio_up = true;
    }
#else
    if (!J.simulate) { cap_fail("built without the ASIO SDK - simulate only"); return; }
#endif

    const double band[2] = { CAL_BAND_LO, CAL_BAND_HI };
    float sim_at[3];                                             /* simulate + a tracked stand: its TRUE center */
    const bool sim_truth = J.tk.used && J.tk.have_truth && J.simulate;
    if (sim_truth) memcpy(sim_at, J.tk.truth, sizeof sim_at);
    for (int i = 0; i < n; ++i) {
        if (J.cancel.load(std::memory_order_relaxed)) {
#ifdef BWA_HAVE_ASIO
            if (asio_up) calib_asio_close();
#endif
            cap_fail("canceled");
            return;
        }
        if (J.simulate) calib_sim_capture(i, &L, sim_truth ? sim_at : J.mic_run, g_room_sos, sweep, cap);
#ifdef BWA_HAVE_ASIO
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
                if (calib_eq(irbuf, CAL_IRLEN, first_refl, CAL_FS, 256, &eq_taps[(size_t)i * BWA_EQ_TAPS])) {
                    eq_lens[i] = 256; J.eqlen[i] = 256;
                }
            }
        }
        /* the bump check: a trim set measured across a moved mic is wrong, so the run stops */
        if (J.tk.used && pl_after_capture(&J.tk, i, CAL_CAPLEN / CAL_FS, sim_truth ? sim_at : NULL)) {
#ifdef BWA_HAVE_ASIO
            if (asio_up) calib_asio_close();
#endif
            char m[200];
            snprintf(m, sizeof m, "BUMP: the ZM-1 moved %.1f mm after speaker %d (limit %.1f mm): nothing written",
                     J.tk.bump_mm, i, J.tk.tol_m * 0.5e3f);
            cap_fail(m);
            return;
        }
        J.done_count.store(i + 1, std::memory_order_release);    /* publish the completed row */
    }
#ifdef BWA_HAVE_ASIO
    if (asio_up) calib_asio_close();
#endif

    float gdb[BWA_MAX_CHANNELS], dms[BWA_MAX_CHANNELS];
    static float pos[BWA_MAX_CHANNELS][3];                            /* calib_solve wants a packed [3]-stride array */
    for (int i = 0; i < n; ++i) { pos[i][0] = L.speakers[i].pos[0]; pos[i][1] = L.speakers[i].pos[1]; pos[i][2] = L.speakers[i].pos[2]; }
    /* the directivity re-aim, when the layout carries a model AND the user set the mic: the same
     * factor and the same rule as the CLI (calib_directivity_corr, only with --mic), so the two tools
     * cannot write different trims from one capture */
    static float corr[BWA_MAX_CHANNELS];
    const float* cp = (J.ran_mic_set && calib_directivity_corr(&L, J.mic_run, 2.0 * CAL_F1, 0.5 * CAL_F2, res, corr))
                    ? corr : NULL;
    J.corr_applied = cp != NULL;
    calib_solve_corr(res, pos, J.mic_run, n, CAL_FS, cp, gdb, dms);
    memcpy(J.gain_db, gdb, sizeof gdb);
    memcpy(J.trim_ms, dms, sizeof dms);
    if (!calib_write_layout(J.ran_layout, J.ran_out, gdb, dms, n, err, sizeof err)) { cap_fail(err); return; }
    if (J.do_eq && !calib_write_eq(J.ran_out, J.ran_out, eq_taps, eq_lens, n, BWA_EQ_TAPS, err, sizeof err)) { cap_fail(err); return; }
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
    bwTip("the surveyed layout the sweep reads speaker geometry from - becomes the A side of the diff");
    ImGui::SameLine(); if (ImGui::Button("...##cpl") && pick_file(J.layout, sizeof J.layout,
                          "layout json (*.json)\0*.json\0all files (*.*)\0*.*\0")) {}
    ImGui::SameLine(); ImGui::TextUnformatted("layout in");
    ImGui::SetNextItemWidth(-uiScaled(240));
    ImGui::InputText("##co", J.out, sizeof J.out);
    bwTip("where the calibrated layout is written (trims + optional eq) - becomes the B side of the diff");
    ImGui::SameLine(); ImGui::TextUnformatted("layout out (trims written here)");
    /* the mic default follows the layout's listening point until the user sets the field. The
     * layout is re-read only when the path changes (a failed read of a half-typed path is fine). */
    if (strcmp(J.mic_for, J.layout) != 0) {
        snprintf(J.mic_for, sizeof J.mic_for, "%s", J.layout);
        static Layout ML;                            /* never a stack local (layout.h); UI thread */
        char merr[256];
        J.have_ref = layout_load(J.layout, (uint32_t)CAL_FS, &ML, merr, sizeof merr);
        J.have_model = J.have_ref && ML.dir.nband > 0;
        if (J.have_ref) memcpy(J.mic_ref, ML.ref, sizeof J.mic_ref);
        if (J.have_ref && !J.mic_set) memcpy(J.mic, ML.ref, sizeof J.mic);
    }
    ImGui::SetNextItemWidth(uiScaled(220));
    J.mic_scope_id = ImGui::GetID("mic (m)");        /* InputFloat3 pushes its label as an ID scope */
    if (ImGui::InputFloat3("mic (m)", J.mic, "%.2f")) J.mic_set = true;
    bwTip("omni mic position in room space - the solve time-aligns and level-matches arrivals at this point. "
          "Defaults to the layout's listening point; set it to where the mic really is");
    if (J.mic_set && J.have_ref) {
        ImGui::SameLine();
        if (ImGui::SmallButton("use listening point")) { memcpy(J.mic, J.mic_ref, sizeof J.mic); J.mic_set = false; }
        bwTip("put the mic field back on the layout's listening point (the directivity re-aim turns off again)");
    }
    ImGui::SameLine(0, uiScaled(16)); ImGui::Checkbox("simulate", &J.simulate);
    bwTip("no hardware: synthesize each sweep capture (1/r + a per-speaker sensitivity wobble) and "
          "run the identical measure->solve->writeback pipeline");
    ImGui::SameLine(); ImGui::Checkbox("room report", &J.do_room);
    bwTip("Schroeder RT60 + early reflections per speaker - tells you how live the room is. "
          "Treat the room if it's too live; don't copy the RT60 into your engine reverb");
    ImGui::SameLine(); ImGui::Checkbox("eq", &J.do_eq);
    bwTip("per-speaker correction FIR inverted from the direct-sound window - flattens the SPEAKER, "
          "not the room (a moving listener can't be room-EQ'd from one point)");
    ImGui::SameLine(); ImGui::Checkbox("save IRs", &J.do_irs);
    bwTip("keep each speaker's impulse response as <prefix>_NN.wav - feeds the IRs tab, and one "
          "capture then serves trims, the room report, and future analysis");
    if (J.do_irs) {
        ImGui::SameLine(); ImGui::SetNextItemWidth(uiScaled(140));
        ImGui::InputTextWithHint("##cirp", "ir prefix", J.irprefix, sizeof J.irprefix);
    }
    J.dir_note = !J.have_model ? 0 : (J.mic_set ? 2 : 1);
    if (J.dir_note == 0)
        ImGui::TextDisabled("directivity: the layout carries no model; the trims are as measured");
    else if (J.dir_note == 1)
        ImGui::TextDisabled("directivity: mic not set, so it sits at the listening point (%.2f %.2f %.2f) and the "
                            "re-aim is OFF; set the mic to turn it on", J.mic_ref[0], J.mic_ref[1], J.mic_ref[2]);
    else
        ImGui::TextUnformatted("directivity: mic set, the re-aim is ON (trims corrected from the mic's bearing "
                               "to the listening point's)");
#ifdef BWA_HAVE_ASIO
    if (!J.simulate) {
        ImGui::SetNextItemWidth(uiScaled(160));
        ImGui::InputTextWithHint("##cdrv", "ASIO driver (auto)", J.driver, sizeof J.driver);
        bwTip("ASIO driver name; empty = first driver with an output per speaker + the mic input");
        driver_pick("##cdrvpick", J.driver, sizeof J.driver);
        ImGui::SameLine(); ImGui::SetNextItemWidth(uiScaled(90));
        ImGui::InputInt("mic input ch", &J.mic_in);
        bwTip("the driver INPUT channel the measurement mic is plugged into (0-based)");
        if (J.mic_in < 0) J.mic_in = 0;                          /* channelNum -1 would reach the driver */
    }
#else
    if (!J.simulate) ImGui::TextDisabled("(built without the ASIO SDK: simulate only)");
#endif
    ImGui::EndDisabled();
    placement_panel(J.have_ref ? J.mic_ref : NULL, J.layout, running || aim_running());
    if (pl_live() && !running)
        ImGui::TextDisabled("tracking is live: a run waits for the placement gate and takes the measured center, not the mic field");

    if (!running) {
        ImGui::BeginDisabled(aim_running());
        const bool run_clicked = ImGui::Button("Run calibration");
        ImGui::EndDisabled();
        if (run_clicked) {
            if (J.th_live) { J.th.join(); J.th_live = false; }   /* reap the previous run */
            snprintf(J.ran_layout, sizeof J.ran_layout, "%s", J.layout);   /* snapshot: this run's paths */
            snprintf(J.ran_out,    sizeof J.ran_out,    "%s", J.out);
            J.ran_mic_set = J.mic_set;
            memcpy(J.mic_run, J.mic, sizeof J.mic_run);
            J.ran_tracked = pl_live();                            /* tracking: the measured center replaces the field */
            if (J.ran_tracked) J.ran_mic_set = true;              /* ...and it is a real position (the re-aim may use it) */
            J.cancel.store(false); J.done_count.store(0); J.n.store(0); J.msg[0] = 0;
            J.state.store(1, std::memory_order_release);
            J.th = std::thread(cap_worker); J.th_live = true;
        }
        bwTip("sweep every speaker -> measure -> solve trims -> write the layout, on a worker "
              "thread; the table fills in live as speakers finish");
    } else if (ImGui::Button("Cancel")) J.cancel.store(true);

    if (st == 0) { ImGui::TextDisabled("sweeps every speaker, solves the trims, writes the layout - then diff it right here"); return; }

    if (st == 1 && J.phase.load() == 1)
        ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.35f, 1.f), "waiting for the placement gate (the Placement panel above)");
    if (st != 1) place_take_line(J.tk);                      /* the worker is done writing it */
    int dc = J.done_count.load(std::memory_order_acquire);
    int jn = J.n.load();
    int n  = jn > 0 ? jn : BWA_MAX_CHANNELS;
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
        bwTip("review what calibration wrote (Diff/Trims/EQ tabs) BEFORE trusting it - a swapped "
              "channel or bad mic placement shows up as an absurd delta");
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
    int       short_ch;                    /* device exposes < 19 inputs: DOA refused (see zy_process) */
    struct { float dir[3]; float age; int valid; } hist[ZY_HIST];
    int       hist_n;
    float     last_dir[3];                 /* newest estimate (test hook) */
    int       last_valid;
    char      driver[128];
    float     dirs[ZYLIA_MICS][3]; float R;
    bool      geom_init;

    /* ---- capsule survey (see zylia.h) ----
     * Each accepted clap contributes one observation: WHERE it happened (relative to the array center)
     * and the 19 arrival times it produced. Solve over >= 6 well-spread ones and the capsule geometry —
     * channel order and mounted orientation included — falls out. */
    bool      surv_on;
    float     surv_center[3];                        /* array center, room coords (tape-measured) */
    float     surv_clap[3];                          /* where the NEXT clap will happen, room coords */
    int       surv_spk;                              /* -1 = manual, else autofill from layout A */
    int       surv_n;
    float     surv_src[ZYLIA_SURVEY_MAX][3];         /* clap positions RELATIVE to the center */
    double    surv_arr[ZYLIA_SURVEY_MAX][ZYLIA_MICS];
    bool      surv_solved, surv_installed;
    float     surv_caps[ZYLIA_MICS][3];
    float     surv_resid, surv_radius, surv_spread;
    char      surv_msg[192];
    char      surv_path[512];
};
static ZyState Z;

/* Guarded read: a hand-edited or malformed layout must never land 0 here (it would divide by zero
 * in the clap synthesis below). */
static double zy_c(void) { return g_room_sos > 0.0 ? g_room_sos : BWA_SOS_REF_MPS; }

static float zy_az(const float d[3]) { return atan2f(d[0], -d[2]) * 57.29578f; }
static float zy_el(const float d[3]) { return asinf(d[1] > 1.f ? 1.f : (d[1] < -1.f ? -1.f : d[1])) * 57.29578f; }

#define ZY_SIM_DIST 2.0      /* how far a synthetic clap happens from the array center (m) */

/* a clap-like Gaussian click sampled at each capsule's exact fractional arrival time (the synthesis
 * the zylia unit test validates), landed in the shared block exactly like the ASIO side would */
static void zy_sim_clap(ZpShared* sh, const float dir[3]) {
    const double C = zy_c(), FS = sh->rate, SIGMA = 1.0e-4, DIST = ZY_SIM_DIST;   /* == zy_solve's c */
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
    /* The capture tolerates a device with fewer than 19 inputs (the probe still meters what it has), but
     * the DOA cannot: zylia_tdoa/zylia_doa want all 19, and the capsules the device never filled are
     * still ZEROED snapshot channels. They would sail through the transient check on the reference
     * channel and enter the least-squares fit as perfectly valid arrivals — yielding a direction that is
     * confident, stable, and wrong, with nothing on screen to say so. Refuse instead of guessing. */
    Z.short_ch = (sh->nch < ZYLIA_MICS);

    long seq = sh->seq;                                          /* fresh snapshot -> TDOA -> DOA */
    if (seq != Z.last_seq && !Z.short_ch) {
        Z.last_seq = seq;
        static float snap[ZYLIA_MICS][ZP_SNAP_N];                /* local copy (static: 300 KB off the stack) */
        memcpy(snap, (const void*)sh->snap, sizeof snap);
        const float* ptr[ZYLIA_MICS];
        for (int ch = 0; ch < ZYLIA_MICS; ++ch) ptr[ch] = snap[ch];
        /* 2x the array's max TDOA. Deliberately NOT tracking zy_c(): this is a search bound, and the
         * 2x margin already swallows any room temperature (1% of c moves it by 1%). */
        uint32_t max_lag = (uint32_t)(sh->rate * (2.0 * 0.049 / 343.0) * 2.0) + 4;
        double arr[ZYLIA_MICS]; float dir[3];
        if (zylia_tdoa(ptr, ZP_SNAP_N, sh->rate, max_lag, arr) && zylia_doa(arr, dir)) {
            auto* h = &Z.hist[Z.hist_n++ % ZY_HIST];
            h->dir[0] = dir[0]; h->dir[1] = dir[1]; h->dir[2] = dir[2];
            h->age = 0.0f; h->valid = 1;
            memcpy(Z.last_dir, dir, sizeof Z.last_dir); Z.last_valid = 1;
            ++Z.claps;
            fprintf(stderr, "arrival %d: az %+.1f el %+.1f\n", Z.claps, zy_az(dir), zy_el(dir));

            /* the same clap, banked as a survey observation. The arrivals are already in hand — the
             * only extra thing a survey needs is WHERE it came from, which the operator has told us. */
            if (Z.surv_on && Z.surv_n < ZYLIA_SURVEY_MAX) {
                int k = Z.surv_n++;
                for (int a = 0; a < 3; ++a) Z.surv_src[k][a] = Z.surv_clap[a] - Z.surv_center[a];
                memcpy(Z.surv_arr[k], arr, sizeof arr);
                Z.surv_solved = false;                           /* new data: the old solve is stale */
            }
        } else ++Z.rejects;                                      /* not transient enough / degenerate solve */
    }
    for (int i = 0; i < ZY_HIST; ++i) if (Z.hist[i].valid) Z.hist[i].age += dt;
}

static void zy_solve(void) {
    if (Z.surv_n < 6) {
        snprintf(Z.surv_msg, sizeof Z.surv_msg, "need at least 6 claps (have %d)", Z.surv_n);
        Z.surv_solved = false; return;
    }
    if (zylia_survey(Z.surv_src, Z.surv_arr, Z.surv_n, zy_c(), Z.surv_caps,
                     &Z.surv_resid, &Z.surv_radius, &Z.surv_spread)) {
        Z.surv_solved = true;
        snprintf(Z.surv_msg, sizeof Z.surv_msg,
                 "solved from %d claps: residual %.2f us, radius %.1f mm, spread %.2f (c = %.1f m/s)",
                 Z.surv_n, Z.surv_resid, Z.surv_radius * 1000.0f, Z.surv_spread, zy_c());
    } else {
        Z.surv_solved = false;
        snprintf(Z.surv_msg, sizeof Z.surv_msg,
                 "refused (spread %.2f): the claps are too coplanar or clustered — the capsules' HEIGHTS "
                 "are unconstrained. Clap from above and below, not just a ring.", Z.surv_spread);
    }
}

static void zy_install(void) {
    zylia_set_capsules(Z.surv_caps);
    zylia_geometry(Z.dirs, &Z.R);                                /* follows the override: the sphere now
                                                                  * draws the array we actually measured */
    Z.surv_installed = true;
}

/* Drive a whole survey off synthetic claps, end to end through the real path (sim clap -> snapshot ->
 * tdoa -> observation). Hardware-free proof the flow is wired, and what the UI test drives. */
static void zy_sim_survey(void) {
    const int N = 14;
    Z.sim_walk = false;
    Z.surv_on  = true;
    Z.surv_n   = 0;
    Z.surv_solved = Z.surv_installed = false;
    zylia_set_capsules(NULL);                                    /* recover from a clean slate */
    zylia_geometry(Z.dirs, &Z.R);
    Z.surv_center[0] = Z.surv_center[1] = Z.surv_center[2] = 0.0f;   /* zy_sim_clap centers on the origin */
    for (int k = 0; k < N; ++k) {
        double yy = 1.0 - 2.0 * ((double)k + 0.5) / (double)N;   /* Fibonacci: spread, and crucially not
                                                                  * coplanar — it includes high and low */
        double rr = sqrt(fmax(0.0, 1.0 - yy * yy)), th = 2.399963229728653 * (double)k;
        Z.truth[0] = (float)(rr * cos(th)); Z.truth[1] = (float)yy; Z.truth[2] = (float)(rr * sin(th));
        for (int a = 0; a < 3; ++a) Z.surv_clap[a] = (float)ZY_SIM_DIST * Z.truth[a];
        zy_sim_clap(&Z.sim, Z.truth);
        zy_process(&Z.sim, 0.0f);                                /* consume immediately: 1 clap = 1 observation */
    }
}

static void tab_zylia(void) {
    if (!Z.geom_init) {                                          /* lazy init: geometry + a fixed first truth */
        zylia_geometry(Z.dirs, &Z.R);
        Z.truth[0] = 0.62f; Z.truth[1] = 0.27f; Z.truth[2] = -0.74f;
        float m = sqrtf(Z.truth[0]*Z.truth[0] + Z.truth[1]*Z.truth[1] + Z.truth[2]*Z.truth[2]);
        Z.truth[0] /= m; Z.truth[1] /= m; Z.truth[2] /= m;
        Z.sim_walk = true;
        Z.surv_spk = -1;
        snprintf(Z.surv_path, sizeof Z.surv_path, "zylia_capsules.json");
        Z.geom_init = true;
    }

    /* -------- source controls -------- */
    if (ImGui::Checkbox("simulate claps", &Z.simulate) && Z.simulate) {
        memset((void*)&Z.sim, 0, sizeof Z.sim);
        Z.sim.nch = ZYLIA_MICS; Z.sim.rate = 48000.0; Z.sim.title = "simulate";
        Z.last_seq = 0; Z.sim_t = 1.0f;
    }
    bwTip("synthesize claps from a known direction through the IDENTICAL pipeline (snapshot -> "
          "TDOA -> DOA); the ring marks the truth the dot should land on");
    if (Z.simulate && !Z.live) {
        ImGui::SameLine(); ImGui::Checkbox("walk", &Z.sim_walk);
        bwTip("the truth direction walks a circle, one clap per second - off = it holds still");
        ImGui::SameLine(); if (ImGui::Button("Clap now")) zy_sim_clap(&Z.sim, Z.truth);
        bwTip("fire one synthetic clap immediately");
    }
#ifdef BWA_HAVE_ASIO
    ImGui::SameLine(0, uiScaled(24));
    ImGui::SetNextItemWidth(uiScaled(160));
    ImGui::InputTextWithHint("##zydrv", "ASIO driver (auto)", Z.driver, sizeof Z.driver);
    bwTip("ASIO driver name; empty = first driver exposing the ZM-1's 19 inputs");
    driver_pick("##zydrvpick", Z.driver, sizeof Z.driver);
    ImGui::SameLine();
    if (!Z.live) { if (ImGui::Button("Open ZM-1")) { Z.live = zylia_capture_open(Z.driver[0] ? Z.driver : NULL, 48000.0);
                                                     if (Z.live) { Z.simulate = false; Z.last_seq = Z.live->seq; } }
                   bwTip("open the 19-capsule capture; clap anywhere around the array and the dot "
                         "shows where it came from - verifies capsule mapping AND the geometry table"); }
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

    if (Z.short_ch)
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.0f),
                           "%s exposes %d inputs — the ZM-1 needs %d. DOA disabled: the missing capsules "
                           "would enter the fit as silent arrivals and point somewhere confidently wrong.\n"
                           "Meters below are live, so this is still usable to check what IS arriving.",
                           sh->title ? sh->title : "?", sh->nch, ZYLIA_MICS);
    else if (Z.last_valid)
        ImGui::Text("%s   %d ch @ %.0f Hz   blocks %ld   claps %d (rejected %d)   last: az %+.1f  el %+.1f",
                    sh->title ? sh->title : "?", sh->nch, sh->rate, sh->blocks, Z.claps, Z.rejects,
                    zy_az(Z.last_dir), zy_el(Z.last_dir));
    else
        ImGui::Text("%s   %d ch @ %.0f Hz   blocks %ld   waiting for a clap...",
                    sh->title ? sh->title : "?", sh->nch, sh->rate, sh->blocks);

    /* Trigger tuning, live (the defaults are guesses — see ZpShared). The readout is the point: the
     * floor is what the room is actually giving you, the threshold is where a clap has to reach, and
     * you want the second comfortably above the first without sitting so high that a clap misses it. */
    if (Z.live) {
        float floor_db = (sh->nfloor > 1e-6f) ? 20.0f * log10f(sh->nfloor) : -120.0f;
        float trip     = sh->trig_ratio * sh->nfloor;
        if (trip < sh->trig_min) trip = sh->trig_min;
        float trip_db  = (trip > 1e-6f) ? 20.0f * log10f(trip) : -120.0f;
        ImGui::Text("noise floor %+.1f dBFS   ->   trips at %+.1f dBFS", floor_db, trip_db);
        bwTip("clap and watch: if nothing registers, lower the ratio; if the room self-triggers, raise it");
        ImGui::SetNextItemWidth(uiScaled(150));
        ImGui::SliderFloat("x floor", (float*)&sh->trig_ratio, 2.0f, 40.0f, "%.1f");
        bwTip("a block's peak must exceed this MULTIPLE of the noise floor to trip the snapshot");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(uiScaled(150));
        ImGui::SliderFloat("min peak", (float*)&sh->trig_min, 0.0005f, 0.2f, "%.4f", ImGuiSliderFlags_Logarithmic);
        bwTip("...AND exceed this absolute level, so a dead-quiet room can't trigger on its own hiss");
    }

    /* ---- capsule survey ---- */
    if (ImGui::CollapsingHeader("Capsule survey")) {
        ImGui::TextWrapped(
            "The built-in table knows the ZM-1's shape but NOT which ASIO channel feeds which capsule, "
            "nor how the array is turned on its stand. Both give a confident, wrong direction. Clap from "
            "6+ known spots — high and low, not just a ring — and the geometry, the channel order and the "
            "orientation all fall out together. No sweep, no second audio device.");
        ImGui::Spacing();

        ImGui::SetNextItemWidth(uiScaled(220));
        ImGui::DragFloat3("array center", Z.surv_center, 0.01f, -20.0f, 20.0f, "%.3f");
        bwTip("where the ZM-1 sits, room coords (m). A tape measure is plenty: at 2.5 m a 5 cm error "
              "tilts a clap direction by ~1 deg, which is already at the timing noise floor");

        if (V.hasA) {                                            /* clap AT a surveyed speaker: it is a
                                                                  * known position you don't have to measure */
            ImGui::SetNextItemWidth(uiScaled(220));
            char prev[48];
            snprintf(prev, sizeof prev, Z.surv_spk < 0 ? "manual" : "speaker %d", Z.surv_spk);
            if (ImGui::BeginCombo("clap at", prev)) {
                if (ImGui::Selectable("manual", Z.surv_spk < 0)) Z.surv_spk = -1;
                for (uint32_t i = 0; i < V.A.count; ++i) {
                    char lbl[32]; snprintf(lbl, sizeof lbl, "speaker %u", i);
                    if (ImGui::Selectable(lbl, Z.surv_spk == (int)i)) {
                        Z.surv_spk = (int)i;
                        for (int a = 0; a < 3; ++a) Z.surv_clap[a] = V.A.speakers[i].pos[a];
                    }
                }
                ImGui::EndCombo();
            }
            bwTip("stand at a speaker and clap - layout A already knows exactly where it is");
            ImGui::SameLine();
        }
        ImGui::SetNextItemWidth(uiScaled(220));
        if (ImGui::DragFloat3("clap position", Z.surv_clap, 0.01f, -20.0f, 20.0f, "%.3f")) Z.surv_spk = -1;
        bwTip("where the NEXT clap will happen, room coords (m) - set this BEFORE you clap");

        ImGui::Checkbox("record claps", &Z.surv_on);
        bwTip("every accepted clap is banked as an observation at the position above");
        ImGui::SameLine(); ImGui::Text("|  %d clap%s banked", Z.surv_n, Z.surv_n == 1 ? "" : "s");
        ImGui::SameLine();
        if (ImGui::Button("Clear")) { Z.surv_n = 0; Z.surv_solved = false; Z.surv_msg[0] = 0; }

        if (Z.simulate && !Z.live) {
            ImGui::SameLine(0, uiScaled(16));
            if (ImGui::Button("simulate a full survey")) zy_sim_survey();
            bwTip("fire 14 spread synthetic claps through the identical clap->snapshot->TDOA->observation "
                  "path, so you can see what a good survey looks like before doing it for real");
        }

        ImGui::Spacing();
        if (ImGui::Button("Solve")) zy_solve();
        bwTip("recover the 19 capsule positions from the banked claps");
        ImGui::SameLine();
        if (!Z.surv_solved) ImGui::BeginDisabled();
        if (ImGui::Button(Z.surv_installed ? "Installed" : "Install")) zy_install();
        bwTip("use the surveyed geometry for every DOA from now on (the sphere below redraws to match)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(uiScaled(220));
        ImGui::InputTextWithHint("##zsp", "zylia_capsules.json", Z.surv_path, sizeof Z.surv_path);
        ImGui::SameLine();
        if (ImGui::Button("Save")) {
            char e[128] = {0};
            const char* p = Z.surv_path[0] ? Z.surv_path : "zylia_capsules.json";
            if (zylia_survey_save(p, Z.surv_caps, Z.surv_resid, Z.surv_radius, Z.surv_spread, Z.surv_n, NULL,
                                  e, sizeof e))
                snprintf(Z.surv_msg, sizeof Z.surv_msg, "saved to %s", p);
            else
                snprintf(Z.surv_msg, sizeof Z.surv_msg, "%s", e);
        }
        bwTip("write the survey to JSON - it encodes the geometry, the channel order AND the mounted "
              "orientation, so it is specific to this ZM-1 on this stand");
        if (!Z.surv_solved) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Load")) {
            char e[128] = {0};
            const char* p = Z.surv_path[0] ? Z.surv_path : "zylia_capsules.json";
            if (zylia_survey_load(p, NULL, e, sizeof e)) {
                zylia_geometry(Z.dirs, &Z.R);
                Z.surv_installed = true;
                snprintf(Z.surv_msg, sizeof Z.surv_msg, "loaded %s — it is driving the DOA now", p);
            } else snprintf(Z.surv_msg, sizeof Z.surv_msg, "%s", e);
        }
        bwTip("load a previous survey and install it");

        if (Z.surv_msg[0]) {
            /* residual is the "should I believe this?" number: it is what the recovered geometry FAILS
             * to explain, in microseconds. Sub-microsecond is a clean survey; tens of us means bad
             * claps, a wrong array center, or a clap position that was not where you said it was. */
            bool bad = !Z.surv_solved || Z.surv_resid > 20.0f;
            ImGui::TextColored(bad ? ImVec4(0.95f, 0.45f, 0.35f, 1.0f) : ImVec4(0.45f, 0.85f, 0.55f, 1.0f),
                               "%s", Z.surv_msg);
        }
    }

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
    /* NoPan: the view is a fixed unit sphere centered on the mic, so translating it away from
       the origin only breaks the "which way did the sound come from" read. Rotate + zoom only. */
    if (ImPlot3D::BeginPlot("##zysphere", ImGui::GetContentRegionAvail(),
                            ImPlot3DFlags_NoLegend | ImPlot3DFlags_NoPan)) {
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

/* ============ Aim tab — live aiming of ONE speaker with the ZM-1 (bwa_calibrate --live N --zylia) ============
 * The installer turns a box nobody can see (behind a screen, overhead); this tab sweeps it about once
 * a second with the live sweep and shows, against layout A: where the box is (zylia_live_position:
 * DOA x the center arrival minus the latency), and how far its axis is off the direction to the ZM-1,
 * as a tilt PEAK METER (turn until it peaks: no calibration, and the screen's loss cannot move the
 * peak) and as an estimated MAGNITUDE (the tilt against the 0 deg tilt of a stored reference or of
 * the file's on_axis_db, inverted through the model). One mic position never says which way the box
 * points, and the UI says so. The capture runs on a worker thread through calib_live_read, the SAME
 * reading the CLI takes. Simulate mode synthesizes it with a draggable true position and aim. */
#define AIM_HIST 90

struct AimData {                         /* everything worker and UI share; copied out under the lock */
    float move[3];                       /* simulate: the true position's offset from the layout (m) */
    float turn_deg, tilt_deg;            /* simulate: the true aim, turned about +y then tilted, off the layout aim */
    float screen_db;                     /* simulate: a screen's HF loss on every path out of the box */
    int   n;                             /* readings published since Start */
    int   dead;                          /* the last reading's dead capsule, -1 = none */
    bool  have_pos; ZyliaLivePos lp;
    bool  have_tilt; float tilt_db, below_db;
    CalibPeakHold pk;
    bool  have_ref; float tilt_ref;
    CalibAimAngle a_ref, a_file;
    float true_deg;                      /* simulate: the truth's angle off the mic */
    float true_pos[3], true_aim[3];
    float hist[AIM_HIST]; int nhist;     /* tilt, newest last */
    PlaceTake tk;                        /* tracked: what the run took, and a bump */
    bool  waiting;                       /* tracked: waiting for the placement gate */
};

struct AimJob {
    int   spk;
    bool  simulate, room;
    float center[3]; bool center_set; char center_for[512];
    float latency_ms; bool latency_set;  /* the rig's measured loop latency; simulate knows its own */
    char  driver[128]; int in_first;
    std::atomic<int>  state;             /* 0 idle / 1 running / 2 stopped / 3 failed */
    std::atomic<bool> stop;
    char  msg[256];
    std::thread th; bool th_live;
    /* snapshotted at Start for the worker */
    int   r_spk; bool r_sim, r_room; float r_center[3]; double r_latency_s; bool r_lat_known;
    bool  r_tracked, r_pos_ok;          /* the Placement panel was live at Start; a position readout is allowed */
    bool  have_model, have_file; float tilt0_file, layout_deg;
    std::mutex mu;
    AimData d;
};
static AimJob AJ;
static bool aim_running(void) { return AJ.state.load(std::memory_order_acquire) == 1; }
static Layout g_aim_L;                   /* the worker's copy of layout A (never a stack local) */
static float  g_aim_curve[CALIB_AIM_CURVE_N];

/* the true aim of the simulated box: the layout aim turned about +y, then tilted about the
 * perpendicular calib_sim_rotate uses */
static void aim_truth(const Layout* L, int s, float turn_deg, float tilt_deg, float out[3]) {
    const float* a = L->speakers[s].aim;
    const float t = turn_deg * 3.14159265f / 180.f, c = cosf(t), sn = sinf(t);
    const float r[3] = { c * a[0] + sn * a[2], a[1], -sn * a[0] + c * a[2] };
    calib_sim_rotate(r, tilt_deg, out);
}

/* recompute the angle estimates from the current tilt and references (cheap: the curve is built) */
static void aim_estimates(AimData& d) {
    memset(&d.a_ref, 0, sizeof d.a_ref);
    memset(&d.a_file, 0, sizeof d.a_file);
    if (!d.have_tilt || !AJ.have_model) return;
    if (d.have_ref)     calib_aim_invert(g_aim_curve, d.tilt_db - d.tilt_ref, CALIB_LIVE_TILT_TOL_DB, &d.a_ref);
    if (AJ.have_file)   calib_aim_invert(g_aim_curve, d.tilt_db - AJ.tilt0_file, CALIB_LIVE_TILT_TOL_DB, &d.a_file);
}

static void aim_fail(const char* m) { snprintf(AJ.msg, sizeof AJ.msg, "%s", m); AJ.state.store(3, std::memory_order_release); }

static void aim_worker(void) {
    const Layout* L = &g_aim_L;
    const int s = AJ.r_spk;
    static float lsweep[CAL_LIVE_NSWEEP];
    static float cap19[(size_t)ZYLIA_MICS * CAL_CAPLEN];      /* the ASIO shell's row stride */
    measure_sweep(lsweep, CAL_LIVE_NSWEEP, CAL_F1, CAL_F2, CAL_FS);
    if (AJ.r_sim) calib_sim_set_room(AJ.r_room ? 0.3f : 0.f);
#ifdef BWA_HAVE_ASIO
    bool asio_up = false;
    if (!AJ.r_sim) {
        if (calib_asio_open_multi(AJ.driver[0] ? AJ.driver : NULL, AJ.in_first, ZYLIA_MICS, (int)L->count, lsweep, cap19) != 0) {
            aim_fail("ASIO open failed (the layout's outputs + 19 ZM-1 inputs on one device; see console)"); return; }
        asio_up = true;
    }
#else
    if (!AJ.r_sim) { aim_fail("built without the ASIO SDK - simulate only"); return; }
#endif
    const double C = g_room_sos > 0.0 ? g_room_sos : BWA_SOS_REF_MPS;
    /* the Placement panel was live at Start: wait for its gate; the measured center replaces the
     * typed one, and a body-frame table is re-aimed for how the stand is turned */
    PlaceTake tk;
    memset(&tk, 0, sizeof tk);
    float sim_at[3];
    bool sim_truth = false;
    if (AJ.r_tracked) {
        { std::lock_guard<std::mutex> lk(AJ.mu); AJ.d.waiting = true; }
        float q[4];
        const int r = pl_take(&tk, AJ.stop, 300.0, q);
        { std::lock_guard<std::mutex> lk(AJ.mu); AJ.d.waiting = false; AJ.d.tk = tk; }
        if (r) {
#ifdef BWA_HAVE_ASIO
            if (asio_up) calib_asio_close();
#endif
            if (AJ.r_sim) calib_sim_set_room(0.f);
            if (r == 1) { snprintf(AJ.msg, sizeof AJ.msg, "stopped"); AJ.state.store(2, std::memory_order_release); }
            else aim_fail(r == 2 ? "the tracker went away before the placement settled"
                                 : "placement timed out (300 s): the ZM-1 never sat in tolerance and still");
            return;
        }
        memcpy(AJ.r_center, tk.center, sizeof AJ.r_center);
        if (AJ.r_pos_ok) mic_track_aim_capsules(&PL.mt, q);
        sim_truth = AJ.r_sim && tk.have_truth;
        if (sim_truth) memcpy(sim_at, tk.truth, sizeof sim_at);
    }
    int nread = 0;
    while (!AJ.stop.load(std::memory_order_relaxed)) {
        CalibSimOpts o; memset(&o, 0, sizeof o);
        float tp[3], ta[3];
        {
            std::lock_guard<std::mutex> lk(AJ.mu);
            for (int a = 0; a < 3; ++a) tp[a] = L->speakers[s].pos[a] + AJ.d.move[a];
            aim_truth(L, s, AJ.d.turn_deg, AJ.d.tilt_deg, ta);
            o.screen_db = AJ.d.screen_db;
        }
        o.on_axis = 1; o.true_pos = tp; o.true_aim = ta;
        CalibLiveReading rd;
        if (!calib_live_read(s, L, sim_truth ? sim_at : AJ.r_center, C, AJ.r_sim, &o, lsweep, cap19, &rd)) {
#ifdef BWA_HAVE_ASIO
            if (asio_up) calib_asio_close();
#endif
            if (AJ.r_sim) calib_sim_set_room(0.f);
            aim_fail("capture timed out (speaker not wired? see console)");
            return;
        }
        ZyliaLivePos lp;
        const bool have_pos = rd.ok && AJ.r_pos_ok &&
                              zylia_live_position(rd.arr, AJ.r_center, AJ.r_lat_known, AJ.r_latency_s, C, L->speakers[s].pos, &lp);
        ++nread;
        const bool bumped = AJ.r_tracked && pl_after_capture(&tk, nread, CAL_LIVE_CAPLEN / CAL_FS, sim_truth ? sim_at : NULL);
        {
            std::lock_guard<std::mutex> lk(AJ.mu);
            AimData& d = AJ.d;
            d.dead = rd.ok ? -1 : rd.dead;
            d.have_pos = have_pos;
            if (have_pos) d.lp = lp;
            d.have_tilt = rd.ok && rd.have_tilt;
            if (d.have_tilt) {
                d.tilt_db  = rd.tilt_db;
                d.below_db = calib_peak_update(&d.pk, rd.tilt_db);
                if (d.nhist == AIM_HIST) { memmove(d.hist, d.hist + 1, (AIM_HIST - 1) * sizeof d.hist[0]); --d.nhist; }
                d.hist[d.nhist++] = rd.tilt_db;
            }
            memcpy(d.true_pos, tp, sizeof tp); memcpy(d.true_aim, ta, sizeof ta);
            d.true_deg = directivity_off_axis_deg(tp, ta, sim_truth ? sim_at : AJ.r_center);
            aim_estimates(d);
            d.tk = tk;
            ++d.n;
        }
        if (bumped) {                                            /* the position readout is off by the move */
#ifdef BWA_HAVE_ASIO
            if (asio_up) calib_asio_close();
#endif
            if (AJ.r_sim) calib_sim_set_room(0.f);
            char m[200];
            snprintf(m, sizeof m, "BUMP: the ZM-1 moved %.1f mm after reading %d (limit %.1f mm): re-place it and start again",
                     tk.bump_mm, nread, tk.tol_m * 0.5e3f);
            aim_fail(m);
            return;
        }
        if (AJ.r_sim) std::this_thread::sleep_for(std::chrono::milliseconds(20));   /* leave the UI a core */
    }
#ifdef BWA_HAVE_ASIO
    if (asio_up) calib_asio_close();
#endif
    if (AJ.r_sim) calib_sim_set_room(0.f);                       /* do not leak the room into the Capture tab */
    snprintf(AJ.msg, sizeof AJ.msg, "stopped");
    AJ.state.store(2, std::memory_order_release);
}

static void aim_start(void) {
    if (AJ.th_live) { AJ.th.join(); AJ.th_live = false; }
    g_aim_L = V.A;
    AJ.r_spk = AJ.spk; AJ.r_sim = AJ.simulate; AJ.r_room = AJ.room;
    memcpy(AJ.r_center, AJ.center, sizeof AJ.center);
    AJ.r_tracked = pl_live();                                /* the worker takes the measured center */
    AJ.r_pos_ok = !AJ.r_tracked || PL.body_frame;            /* a tracked DOA needs the array's orientation */
    if (AJ.simulate)         { AJ.r_latency_s = CAL_SIM_LATENCY_SAMPLES / CAL_FS; AJ.r_lat_known = true; }
    else if (AJ.latency_set) { AJ.r_latency_s = AJ.latency_ms * 1e-3;            AJ.r_lat_known = true; }
    else                     { AJ.r_latency_s = 0.0;                              AJ.r_lat_known = false; }
    const double band[2] = { CALIB_LIVE_MID_HZ, CALIB_LIVE_HIGH_HZ };   /* the live meter's bands (calib.h) */
    AJ.have_model = g_aim_L.dir.nband > 0;
    if (AJ.have_model) calib_aim_curve(&g_aim_L.dir, band, CAL_F2, g_aim_curve);
    AJ.have_file = AJ.have_model && calib_on_axis_tilt_db(&g_aim_L.dir, band, CAL_F2, &AJ.tilt0_file);
    AJ.layout_deg = layout_speaker_off_axis_deg(&g_aim_L, (uint32_t)AJ.spk, AJ.center);
    {
        std::lock_guard<std::mutex> lk(AJ.mu);
        AJ.d.n = 0; AJ.d.dead = -1; AJ.d.have_pos = AJ.d.have_tilt = false; AJ.d.nhist = 0;
        memset(&AJ.d.tk, 0, sizeof AJ.d.tk); AJ.d.waiting = false;
        calib_peak_reset(&AJ.d.pk);                              /* a new box: a new peak (the reference stays) */
        aim_estimates(AJ.d);
    }
    AJ.msg[0] = 0;
    AJ.stop.store(false);
    AJ.state.store(1, std::memory_order_release);
    AJ.th = std::thread(aim_worker); AJ.th_live = true;
}

static void aim_fmt(const CalibAimAngle& a, char* buf, size_t cap) {
    if (!a.ok)          snprintf(buf, cap, "-");
    else if (a.beyond)  snprintf(buf, cap, ">= %.0f deg", a.max_deg);
    else if (a.on_axis) snprintf(buf, cap, "on axis (under %.0f deg)", a.hi_deg);
    else                snprintf(buf, cap, "%.1f deg  [%.0f to %.0f]", a.angle_deg, a.lo_deg, a.hi_deg);
}

static void tab_aim(void) {
    const int st = AJ.state.load(std::memory_order_acquire);
    const bool running = st == 1;
    if (!V.hasA) {
        ImGui::TextDisabled("load layout A (the rig's layout, with its directivity model) to aim its speakers");
        return;
    }
    /* the ZM-1 sits at layout A's listening point until the user says otherwise */
    if (strcmp(AJ.center_for, V.pathA) != 0) {
        snprintf(AJ.center_for, sizeof AJ.center_for, "%s", V.pathA);
        if (!AJ.center_set) memcpy(AJ.center, V.A.ref, sizeof AJ.center);
    }
    if (AJ.spk < 0) AJ.spk = 0;
    if (AJ.spk >= (int)V.A.count) AJ.spk = (int)V.A.count - 1;

    ImGui::BeginDisabled(running);
    ImGui::SetNextItemWidth(uiScaled(110));
    ImGui::InputInt("speaker##aim", &AJ.spk);
    bwTip("the speaker to aim: layout A's index (the side panel lists them)");
    if (AJ.spk < 0) AJ.spk = 0;
    if (AJ.spk >= (int)V.A.count) AJ.spk = (int)V.A.count - 1;
    ImGui::SameLine();
    const float* sp = V.A.speakers[AJ.spk].pos;
    ImGui::TextDisabled("at (%.2f %.2f %.2f)", sp[0], sp[1], sp[2]);
    ImGui::SameLine(0, uiScaled(16));
    ImGui::Checkbox("simulate##aim", &AJ.simulate);
    bwTip("no hardware: synthesize each reading (the live sweep, the 19 capsules, the directivity model at the "
          "TRUE angle) with a box you can move and turn below");
    if (AJ.simulate) { ImGui::SameLine(); ImGui::Checkbox("room##aim", &AJ.room);
                       bwTip("put the simulated shoebox room around the array (bwa_calibrate --sim-room 0.3)"); }
    ImGui::SetNextItemWidth(uiScaled(220));
    if (ImGui::InputFloat3("ZM-1 center (m)##aim", AJ.center, "%.3f")) AJ.center_set = true;
    bwTip("the array center, room coordinates. Defaults to layout A's listening point (4.75 ft on the rig)");
    if (!AJ.simulate) {
        ImGui::SameLine(0, uiScaled(16));
        ImGui::SetNextItemWidth(uiScaled(100));
        if (ImGui::InputFloat("latency (ms)##aim", &AJ.latency_ms, 0.f, 0.f, "%.3f")) AJ.latency_set = AJ.latency_ms > 0.f;
        bwTip("the measured loop latency (bwa_calibrate --zylia --ref prints it; about 60 ms through Dante Via). "
              "0 = unknown: direction only, no distance. 20 us of error is 6.9 mm of distance");
#ifdef BWA_HAVE_ASIO
        ImGui::SetNextItemWidth(uiScaled(160));
        ImGui::InputTextWithHint("##aimdrv", "ASIO driver (auto)", AJ.driver, sizeof AJ.driver);
        driver_pick("##aimdrvpick", AJ.driver, sizeof AJ.driver);
        ImGui::SameLine(); ImGui::SetNextItemWidth(uiScaled(90));
        ImGui::InputInt("first ZM-1 input##aim", &AJ.in_first);
        bwTip("the driver input carrying the ZM-1's first capsule; the 19 capsules are consecutive from here");
        if (AJ.in_first < 0) AJ.in_first = 0;
#else
        ImGui::TextDisabled("(built without the ASIO SDK: simulate only)");
#endif
    }
    ImGui::EndDisabled();
    placement_panel(V.A.ref, V.pathA, running || J.state.load(std::memory_order_acquire) == 1);
    if (pl_live() && !running)
        ImGui::TextDisabled("tracking is live: Start waits for the placement gate and takes the measured center%s",
                            PL.body_frame ? "" : "; no body-frame survey, so no position readout (the tilt meter works)");

    const bool capture_busy = J.state.load(std::memory_order_acquire) == 1;   /* one sweep shell, one simulator */
    if (!running) {
        ImGui::BeginDisabled(capture_busy);
        if (ImGui::Button("Start##aim")) aim_start();
        ImGui::EndDisabled();
        bwTip(capture_busy ? "the Capture tab is running a calibration" : "sweep the speaker over and over (about one reading a second on the rig)");
    } else if (ImGui::Button("Stop##aim")) AJ.stop.store(true);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", st == 3 ? AJ.msg : (running ? "running" : (st == 2 ? "stopped" : "idle")));
    if (st == 3) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f), "FAILED"); }

    if (AJ.simulate) {                                          /* the simulated truth, live */
        std::lock_guard<std::mutex> lk(AJ.mu);
        ImGui::SetNextItemWidth(uiScaled(220));
        ImGui::DragFloat3("true offset (m)##aim", AJ.d.move, 0.002f, -0.5f, 0.5f, "%.3f");
        bwTip("where the simulated box really is, relative to its layout position");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(uiScaled(90));
        ImGui::DragFloat("turn (deg)##aim", &AJ.d.turn_deg, 0.25f, -90.f, 90.f, "%.1f");
        bwTip("the simulated box's aim, turned about vertical off its layout aim (no effect on a box aimed straight up or down)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(uiScaled(90));
        ImGui::DragFloat("tilt (deg)##aim", &AJ.d.tilt_deg, 0.25f, -90.f, 90.f, "%.1f");
        bwTip("... and tilted off it");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(uiScaled(90));
        ImGui::DragFloat("screen (dB)##aim", &AJ.d.screen_db, 0.05f, 0.f, 6.f, "%.2f");
        bwTip("a screen's high-frequency loss on every path out of the box (a shelf above 4 kHz). The peak and a "
              "stored reference do not care; the file's 0 deg tilt does");
    }

    AimData d;
    { std::lock_guard<std::mutex> lk(AJ.mu); d = AJ.d; }
    if (running && d.waiting)
        ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.35f, 1.f), "waiting for the placement gate (the Placement panel above)");
    place_take_line(d.tk);
    ImGui::Separator();

    /* -------- left: the meter and the numbers; right: the 3D view -------- */
    ImGui::BeginChild("aimmeter", ImVec2(uiScaled(520), 0));
    {
        if (ImGui::Button("Store reference")) {
            std::lock_guard<std::mutex> lk(AJ.mu);
            if (AJ.d.have_tilt) { AJ.d.have_ref = true; AJ.d.tilt_ref = AJ.d.tilt_db; aim_estimates(AJ.d); }
        }
        bwTip("take THIS reading as the on-axis tilt: do it on a speaker known to point at the ZM-1 (one the "
              "optical survey confirmed, or one you just peaked). It absorbs the ZM-1, the gate and the screen");
        ImGui::SameLine();
        if (ImGui::Button("Reset peak")) {
            std::lock_guard<std::mutex> lk(AJ.mu);
            calib_peak_reset(&AJ.d.pk); AJ.d.below_db = 0.f;
        }
        bwTip("forget the held peak (a new box, or the peak came from a bad reading)");
        ImGui::SameLine();
        if (ImGui::Button("Clear reference")) {
            std::lock_guard<std::mutex> lk(AJ.mu);
            AJ.d.have_ref = false; aim_estimates(AJ.d);
        }
        if (d.have_ref) { ImGui::SameLine(); ImGui::TextDisabled("ref %+.2f dB", d.tilt_ref); }

        /* the number read from a ladder: how far below the peak */
        const float fs0 = ImGui::GetStyle().FontSizeBase;
        const bool at_peak = d.have_tilt && d.below_db < 0.1f;
        ImGui::PushFont(NULL, fs0 * 4.0f);
        if (d.have_tilt) ImGui::TextColored(at_peak ? ImVec4(0.45f, 0.9f, 0.5f, 1.f) : ImVec4(0.95f, 0.8f, 0.35f, 1.f),
                                            "%.1f dB", d.below_db);
        else             ImGui::TextDisabled("-- dB");
        ImGui::PopFont();
        ImGui::TextUnformatted(at_peak ? "AT PEAK: the treble is as high as it has been" : "below the peak: turn the box until this reads 0");

        /* the meter: the reading as a bar over [peak - 6 dB, peak + 0.5 dB], the held peak as a line */
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x, h = uiScaled(34);
            dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(40, 42, 50, 255), 3.f);
            if (d.have_tilt && d.pk.peak_index) {
                const float lo = d.pk.peak_db - 6.f, hi = d.pk.peak_db + 0.5f;
                float t = (d.tilt_db - lo) / (hi - lo); t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
                const float tp = (d.pk.peak_db - lo) / (hi - lo);
                dl->AddRectFilled(p0, ImVec2(p0.x + w * t, p0.y + h), at_peak ? IM_COL32(90, 200, 110, 255) : IM_COL32(230, 180, 70, 255), 3.f);
                dl->AddLine(ImVec2(p0.x + w * tp, p0.y - 3.f), ImVec2(p0.x + w * tp, p0.y + h + 3.f), IM_COL32(250, 250, 250, 255), 3.f);
            }
            ImGui::Dummy(ImVec2(w, h + 4.f));
        }
        if (d.have_tilt) ImGui::Text("tilt %+.2f dB   peak %+.2f dB (reading %d of %d)", d.tilt_db, d.pk.peak_db, d.pk.peak_index, d.pk.n);
        else             ImGui::TextDisabled(running ? "waiting for the first reading..." : "Start to measure");
        if (d.dead >= 0) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.f), "capsule %d (input %d) is dead or silent: reading skipped",
                                            d.dead, AJ.in_first + d.dead);
        if (d.nhist > 1 && ImPlot::BeginPlot("##aimhist", ImVec2(-1, uiScaled(110)), ImPlotFlags_NoLegend | ImPlotFlags_NoMenus)) {
            ImPlot::SetupAxes(NULL, "tilt dB", ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_AutoFit);
            ImPlot::SetupAxisLimits(ImAxis_X1, 0, AIM_HIST, ImPlotCond_Always);
            ImPlot::PlotLine("tilt", d.hist, d.nhist);
            ImPlot::EndPlot();
        }

        /* the angle: a magnitude, next to the target */
        ImGui::Separator();
        ImGui::PushFont(NULL, fs0 * 1.6f);
        char a1[64], a2[64];
        aim_fmt(d.a_ref, a1, sizeof a1);
        aim_fmt(d.a_file, a2, sizeof a2);
        ImGui::Text("off axis (ref):  %s", d.have_ref ? a1 : "store a reference");
        ImGui::Text("off axis (file): %s", AJ.have_file ? a2 : "no on_axis_db");
        ImGui::Text("layout expects:  %.1f deg", d.tk.used ? layout_speaker_off_axis_deg(&g_aim_L, (uint32_t)AJ.r_spk, d.tk.center)
                                                           : AJ.layout_deg);
        if (AJ.simulate && d.n) ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.55f, 1.f), "truth (simulate): %.1f deg", d.true_deg);
        ImGui::PopFont();
        ImGui::TextWrapped("The angle is a MAGNITUDE: how far the box's axis is off the line to the ZM-1, never which way "
                           "it points. Near 0 deg the model's curve is flat, so a box within the 'under N deg' bracket "
                           "reads as on axis. Use the peak meter to aim; the angle says how far there is to go.");
        if (!AJ.have_model) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.f), "layout A has no directivity model: the meter works, the angle does not");

        /* the position */
        ImGui::Separator();
        if (d.have_pos && d.lp.have_distance)
            ImGui::Text("position: %+.0f %+.0f %+.0f mm off the layout (|d| %.0f mm)\ndirection %.2f deg off, distance %.3f m (%+.0f mm)",
                        d.lp.delta_mm[0], d.lp.delta_mm[1], d.lp.delta_mm[2], d.lp.delta_norm_mm, d.lp.dir_err_deg, d.lp.dist_m, d.lp.dist_err_mm);
        else if (d.have_pos)
            ImGui::Text("direction %.2f deg off the layout (no latency: no distance)", d.lp.dir_err_deg);
        else if (AJ.r_tracked && !AJ.r_pos_ok)
            ImGui::TextDisabled("position: off (tracked with no body-frame survey: a direction needs the array's orientation)");
        else
            ImGui::TextDisabled("position: -");
    }
    ImGui::EndChild();
    ImGui::SameLine();

    if (ImPlot3D::BeginPlot("##aim3d", ImGui::GetContentRegionAvail(), ImPlot3DFlags_NoPan)) {
        ImPlot3D::SetupAxes("x (m)", "z (m)", "y up (m)", ImPlot3DAxisFlags_AutoFit, ImPlot3DAxisFlags_AutoFit, ImPlot3DAxisFlags_AutoFit);
        const Layout& L = V.A;
        if (V.hasA) ImPlot3D::PlotScatter("speakers", V.ax, V.ay, V.az, (int)L.count,
                                          ImPlot3DSpec(ImPlot3DProp_MarkerSize, 3.0f, ImPlot3DProp_MarkerFillColor, IM_COL32(120, 120, 140, 160)));
        const int s = AJ.spk;
        const float* p = L.speakers[s].pos; const float* a = L.speakers[s].aim;
        float lx[2] = { p[0], p[0] + 0.4f * a[0] }, ly[2] = { p[2], p[2] + 0.4f * a[2] }, lz[2] = { p[1], p[1] + 0.4f * a[1] };
        ImPlot3D::PlotScatter("layout", &lx[0], &ly[0], &lz[0], 1, ImPlot3DSpec(ImPlot3DProp_MarkerSize, 7.0f));
        ImPlot3D::PlotLine("layout aim", lx, ly, lz, 2);
        const float* zc = d.tk.used ? d.tk.center : AJ.center;  /* the center the readings used */
        float cx = zc[0], cy = zc[2], cz = zc[1];
        ImPlot3D::PlotScatter("ZM-1", &cx, &cy, &cz, 1, ImPlot3DSpec(ImPlot3DProp_MarkerSize, 6.0f));
        if (pl_live()) {                                         /* the tracked center, live */
            const PlaceSnap ps = pl_snap();
            if (ps.have_pose) {
                float mx = ps.g.center[0], my = ps.g.center[2], mz = ps.g.center[1];
                ImPlot3D::PlotScatter("ZM-1 tracked", &mx, &my, &mz, 1, ImPlot3DSpec(ImPlot3DProp_MarkerSize, 4.0f));
            }
        }
        if (d.have_pos) {                                        /* the measured direction, and the position on it */
            const float r = d.lp.have_distance ? d.lp.dist_m : d.lp.layout_dist_m;
            float ex[2] = { cx, zc[0] + r * d.lp.dir[0] }, ey[2] = { cy, zc[2] + r * d.lp.dir[2] },
                  ez[2] = { cz, zc[1] + r * d.lp.dir[1] };
            ImPlot3D::PlotLine("measured direction", ex, ey, ez, 2);
            if (d.lp.have_distance) ImPlot3D::PlotScatter("measured", &ex[1], &ey[1], &ez[1], 1, ImPlot3DSpec(ImPlot3DProp_MarkerSize, 6.0f));
        }
        if (AJ.simulate && d.n) {                                /* the truth the estimate should land on */
            float tx[2] = { d.true_pos[0], d.true_pos[0] + 0.4f * d.true_aim[0] }, ty[2] = { d.true_pos[2], d.true_pos[2] + 0.4f * d.true_aim[2] },
                  tz[2] = { d.true_pos[1], d.true_pos[1] + 0.4f * d.true_aim[1] };
            ImPlot3D::PlotScatter("truth", &tx[0], &ty[0], &tz[0], 1, ImPlot3DSpec(ImPlot3DProp_MarkerSize, 5.0f));
            ImPlot3D::PlotLine("true aim", tx, ty, tz, 2);
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
    ImGui::TextUnformatted("IR prefix (bwa_calibrate --save-irs)");
    ImGui::SetNextItemWidth(-uiScaled(104));
    ImGui::InputText("##IR", V.irprefix, sizeof V.irprefix);
    ImGui::SameLine(); if (ImGui::Button("...##pI") && pick_file(V.irprefix, sizeof V.irprefix, WAVF)) {
        wav_to_prefix(V.irprefix);                               /* any one _NN.wav selects the set */
        load_irs();
    }
    ImGui::SameLine(); if (ImGui::Button("Load IRs")) load_irs();
    bwTip("decode <prefix>_00.wav .. one per speaker (bwa_calibrate --save-irs) through the engine's "
          "own sound loader; picking any one _NN.wav selects the whole set");
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
        if (ImGui::BeginTabItem("Aim"))     { tab_aim();     ImGui::EndTabItem(); }
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
static const char* FIX_D = "calibview_fix_d.json";   /* A plus a directivity model and a listening point */
static const char* FIX_E = "calibview_fix_e.json";   /* A plus a listening point and a model with on_axis_db */

static int write_fixture(const char* path, int variant_b) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "{\n  \"schema_version\": 1,\n"
               "  \"units\": { \"position\": \"meters\", \"gain\": \"decibels\", \"delay\": \"milliseconds\" },\n"
               "  \"coordinate_space\": \"room, right-handed, +y up, +z forward; origin on the floor\",\n"
               "  \"reference\": { \"alignment\": \"max-distance\", \"speed_of_sound_mps\": 343.0 },\n"
               "  \"dbap\": { \"rolloff_r\": 0.5, \"distance_attenuation\": { \"model\": \"inverse\","
               " \"reference_distance_m\": 1.0, \"rolloff\": 1.0, \"min_gain_db\": -40.0 } },\n"
               "  \"speakers\": [\n");
    int idx = 0;
    for (int zi = -1; zi <= 1; ++zi) for (int yi = -1; yi <= 1; ++yi) for (int xi = -1; xi <= 1; ++xi) {
        if (!zi && !yi && !xi) continue;                          /* 27 - center = 26 */
        double x = 1.5 * xi, y = 1.5 * yi, z = 1.5 * zi, g = 0.0;
        if (variant_b == 1 && idx == 7) { x += 0.10; g = -1.5; }  /* the known deltas */
        fprintf(f, "    { \"index\": %d, \"position\": [%.4f, %.4f, %.4f], \"gain_db\": %.2f, \"delay_ms\": %.3f",
                idx, x, y, z, g, 0.05 * idx);
        if (variant_b == 1 && idx == 3)
            fprintf(f, ", \"eq\": [0.9, 0.2, -0.1, 0.05, 0.02, -0.01, 0.005, 0.0]");
        fprintf(f, " }%s\n", idx < 25 ? "," : "");
        ++idx;
    }
    if (variant_b == 3) {
        /* loss = -g (1 - cos theta) per band: the flat top a waveguide has near 0 deg */
        const float bands[6] = { 500, 1000, 2000, 4000, 8000, 16000 }, g[6] = { 0.5f, 1.f, 2.f, 4.f, 8.f, 14.f };
        const int ang[8] = { 0, 10, 20, 30, 45, 60, 90, 180 };
        fprintf(f, "  ],\n  \"listening_point_m\": [0.1, 0, 0.1],\n  \"directivity\": { \"bands_hz\": [500, 1000, 2000, 4000, 8000, 16000],\n"
                   "    \"angles_deg\": [0, 10, 20, 30, 45, 60, 90, 180], \"split_hz\": 1000,\n"
                   "    \"on_axis_db\": [80, 80, 80, 80, 80, 80],\n    \"loss_db\": [");
        for (int b = 0; b < 6; ++b) {
            fprintf(f, "%s[", b ? ", " : "");
            for (int a = 0; a < 8; ++a) fprintf(f, "%s%.3f", a ? ", " : "", -g[b] * (1.0 - cos(ang[a] * 3.14159265358979 / 180.0)));
            fprintf(f, "]");
        }
        fprintf(f, "] }\n}\n");
    } else if (variant_b == 2)
        fprintf(f, "  ],\n  \"listening_point_m\": [0.2, 0.3, -0.1],\n"
                   "  \"directivity\": { \"bands_hz\": [250, 4000], \"angles_deg\": [0, 30, 60, 90, 180],\n"
                   "    \"split_hz\": 1000, \"loss_db\": [[0, -1, -2, -3, -6], [0, -4, -10, -16, -25]] }\n}\n");
    else
        fprintf(f, "  ]\n}\n");
    fclose(f);
    return 1;
}

/* the Placement tests' helpers (TestFunc is a plain function pointer, so no captures) */
static void pl_wait_state(ImGuiTestContext* ctx, int want, double secs) {
    const double t0 = ImGui::GetTime();
    while (PL.state.load() != want && ImGui::GetTime() - t0 < secs) ctx->Yield();
}
static float dist3(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
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

    /* the directivity re-aim follows bwa_calibrate's --mic rule. A layout with a model and a listening
     * point off the array centroid: the mic field defaults to that point and the re-aim stays OFF
     * through a whole run; typing a mic position turns it ON for the next run; a model-free layout
     * shows the no-model note; "use listening point" puts the field back and turns it off. With the
     * worker applying the correction whenever a model exists (the old rule), the first run's
     * corr_applied check went red. */
    t = IM_REGISTER_TEST(e, "capture", "mic_rule");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("calib view");
        ctx->ItemClick("**/Capture");
        ctx->Yield(2);
        J.mic_set = false;                                        /* earlier tests share J */
        ctx->ItemClick("**/##cl");   ctx->KeyCharsReplaceEnter(FIX_D);
        ctx->ItemClick("**/##co");   ctx->KeyCharsReplaceEnter("calibview_cap_out_d.json");
        ctx->ItemCheck("**/simulate");
        ctx->Yield(2);
        IM_CHECK(J.have_model);
        IM_CHECK(!J.mic_set);
        IM_CHECK_LT(fabsf(J.mic[0] - 0.2f), 1e-4f);               /* the layout's listening point, */
        IM_CHECK_LT(fabsf(J.mic[1] - 0.3f), 1e-4f);               /* not (0, 0, 0)                 */
        IM_CHECK_LT(fabsf(J.mic[2] + 0.1f), 1e-4f);
        IM_CHECK_EQ(J.dir_note, 1);
        ctx->ItemClick("**/Run calibration");
        double t0 = ImGui::GetTime();
        while (J.state.load() == 1 && ImGui::GetTime() - t0 < 60.0) ctx->Yield();
        IM_CHECK_EQ(J.state.load(), 2);
        IM_CHECK(!J.corr_applied);                                /* mic never set: trims as measured */

        ctx->ItemInputValue(ctx->GetIDByInt(0, J.mic_scope_id), 1.0f);   /* the mic's x component */
        ctx->Yield(2);
        IM_CHECK(J.mic_set);
        IM_CHECK_LT(fabsf(J.mic[0] - 1.0f), 1e-4f);
        IM_CHECK_EQ(J.dir_note, 2);
        ctx->ItemClick("**/Run calibration");
        t0 = ImGui::GetTime();
        while (J.state.load() == 1 && ImGui::GetTime() - t0 < 60.0) ctx->Yield();
        IM_CHECK_EQ(J.state.load(), 2);
        IM_CHECK(J.corr_applied);                                 /* mic set + a model: re-aimed */

        ctx->ItemClick("**/##cl");   ctx->KeyCharsReplaceEnter(FIX_A);   /* no model in this one */
        ctx->Yield(2);
        IM_CHECK(!J.have_model);
        IM_CHECK_EQ(J.dir_note, 0);
        IM_CHECK_LT(fabsf(J.mic[0] - 1.0f), 1e-4f);               /* a set mic survives a layout change */
        ctx->ItemClick("**/use listening point");
        ctx->Yield(2);
        IM_CHECK(!J.mic_set);
        IM_CHECK_LT(fabsf(J.mic[0]), 1e-4f);                      /* FIX_A's listening point: its centroid */
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

    /* the capsule-survey flow, end to end through the real UI. The synthetic claps are generated FROM
     * the built-in table, so a correctly-wired panel must recover exactly that table back — which only
     * happens if it fed the solver the right clap positions, the right arrivals, and the right channel
     * order. Get any of those wrong and the recovered geometry is visibly not the ZM-1. */
    t = IM_REGISTER_TEST(e, "zylia", "sim_survey");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("calib view");
        ctx->ItemClick("**/Zylia");
        ctx->Yield(2);
        ctx->ItemCheck("**/simulate claps");                     /* Check, not Click: earlier tests share Z */
        ctx->ItemOpen("**/Capsule survey");
        ctx->Yield(2);
        ctx->ItemClick("**/simulate a full survey");
        ctx->Yield(2);
        IM_CHECK_EQ(Z.surv_n, 14);                               /* 14 claps in -> 14 observations banked */

        ctx->ItemClick("**/Solve");
        ctx->Yield(2);
        IM_CHECK(Z.surv_solved);
        IM_CHECK_LT(Z.surv_resid, 1.0f);                         /* sub-us: the fit explains the claps */
        IM_CHECK_GT(Z.surv_radius, 0.045f);
        IM_CHECK_LT(Z.surv_radius, 0.053f);
        IM_CHECK_GT(Z.surv_spread, 0.5f);

        float ref[ZYLIA_MICS][3], R;
        zylia_set_capsules(NULL);
        zylia_geometry(ref, &R);
        for (int i = 0; i < ZYLIA_MICS; ++i)
            for (int a = 0; a < 3; ++a)
                IM_CHECK_LT(fabsf(Z.surv_caps[i][a] - R * ref[i][a]), 0.0005f);

        ctx->ItemClick("**/Install");
        ctx->Yield(2);
        IM_CHECK(Z.surv_installed);
        ctx->CaptureScreenshotWindow("//calib view");
        zylia_set_capsules(NULL);                                /* don't leak the override into other tests */
    };

    /* live aiming, simulate, through the real UI: a box on its layout aim reads at the peak; store it
     * as the reference; turn the box 25 deg away and the meter drops while the estimate grows; turn it
     * back and it peaks again and reads "on axis". The worker publishes a reading per sweep, and the
     * truth is read at the start of each, so every check waits for TWO new readings after a change. */
    t = IM_REGISTER_TEST(e, "aim", "sim_live");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto wait_readings = [&](int more) {
            int n0; { std::lock_guard<std::mutex> lk(AJ.mu); n0 = AJ.d.n; }
            double t0 = ImGui::GetTime();
            for (;;) {
                int n; { std::lock_guard<std::mutex> lk(AJ.mu); n = AJ.d.n; }
                if (n >= n0 + more || AJ.state.load() != 1 || ImGui::GetTime() - t0 > 60.0) break;
                ctx->Yield();
            }
        };
        auto snap = [&]() { AimData d; std::lock_guard<std::mutex> lk(AJ.mu); d = AJ.d; return d; };
        ctx->SetRef("calib view");
        ctx->ItemClick("**/##A");
        ctx->KeyCharsReplaceEnter(FIX_E);
        ctx->ItemClick("**/Load A");
        IM_CHECK(V.hasA && V.A.dir.has_on_axis);
        ctx->ItemClick("**/Aim");
        ctx->Yield(2);
        ctx->ItemInputValue("**/speaker##aim", 3);               /* (-1.5, 0, -1.5): level with the ZM-1, so a turn is the full angle */
        ctx->ItemCheck("**/simulate##aim");
        ctx->Yield(2);
        IM_CHECK_LT(fabsf(AJ.center[0] - 0.1f) + fabsf(AJ.center[1]) + fabsf(AJ.center[2] - 0.1f), 1e-4f);   /* the ZM-1 defaults to the listening point */
        ctx->ItemInputValue("**/turn (deg)##aim", 0.0f);
        ctx->ItemClick("**/Start##aim");
        wait_readings(2);
        AimData d = snap();
        IM_CHECK_EQ(AJ.state.load(), 1);
        IM_CHECK(d.have_tilt && d.below_db < 0.05f);
        IM_CHECK(d.a_file.ok && d.a_file.on_axis);                /* the fixture's flat on-axis response */
        IM_CHECK(d.have_pos && d.lp.delta_norm_mm < 25.f);        /* where the layout says */
        ctx->ItemClick("**/Store reference");
        d = snap();
        IM_CHECK(d.have_ref && d.a_ref.on_axis);

        ctx->ItemInputValue("**/turn (deg)##aim", 25.0f);
        wait_readings(2);
        d = snap();
        printf("aim test: turned 25 deg: truth %.1f, below %.2f dB, ref %.1f deg [%.1f-%.1f]\n",
               d.true_deg, d.below_db, d.a_ref.angle_deg, d.a_ref.lo_deg, d.a_ref.hi_deg);
        IM_CHECK_GT(d.below_db, 0.5f);                            /* the meter drops */
        IM_CHECK(d.a_ref.ok && !d.a_ref.on_axis);                 /* the estimate grows */
        IM_CHECK_LT(fabsf(d.a_ref.angle_deg - d.true_deg), 3.0f);
        ctx->CaptureScreenshotWindow("//calib view");

        ctx->ItemInputValue("**/turn (deg)##aim", 0.0f);
        wait_readings(2);
        d = snap();
        IM_CHECK_LT(d.below_db, 0.05f);                           /* back at the peak */
        IM_CHECK(d.a_ref.ok && d.a_ref.on_axis);                  /* and "on axis" */
        ctx->ItemClick("**/Stop##aim");
        double t0 = ImGui::GetTime();
        while (AJ.state.load() == 1 && ImGui::GetTime() - t0 < 30.0) ctx->Yield();
        IM_CHECK_EQ(AJ.state.load(), 2);
    };

    /* ---- the Placement panel, on the simulated stand (mic_track's MIC_SIM_SCRIPT, wall clock): it walks
     * in from 8.4 cm off the target over 2 s, settles 5.4 mm off, and with "bump mid-run" is knocked
     * 15 mm after the third capture. The truth the checks use is the script's own (PlaceSnap.truth,
     * computed apart from placement.c), never the value under test. */
    /* the gate goes green only once the stand has settled: it reads MOVING on the way in, and OK no
     * sooner than the 2 s approach plus the 1 s hold, with the truth by then at its settled spot */
    t = IM_REGISTER_TEST(e, "placement", "sim_gate");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("calib view");
        ctx->ItemClick("**/Capture");
        ctx->Yield(2);
        ctx->ItemClick("**/##cl");   ctx->KeyCharsReplaceEnter(FIX_A);
        ctx->ItemOpen("**/Placement (tracked ZM-1)");
        ctx->Yield(2);
        ctx->ItemCheck("**/simulate##pl");
        ctx->ItemUncheck("**/bump mid-run##pl");
        ctx->ItemClick("**/Connect##pl");
        const double t0 = ImGui::GetTime();
        pl_wait_state(ctx, 2, 10.0);
        IM_CHECK_EQ(PL.state.load(), 2);
        bool saw_moving = false;
        double t_ok = -1.0;
        PlaceSnap s;
        while (ImGui::GetTime() - t0 < 20.0) {
            s = pl_snap();
            if (s.g.state == PLACE_MOVING) saw_moving = true;
            if (s.g.state == PLACE_OK) { t_ok = ImGui::GetTime() - t0; break; }
            ctx->Yield();
        }
        printf("placement test: gate opened %.2f s after Connect, %.1f mm off the target\n", t_ok, s.g.mean_dist_m * 1e3f);
        IM_CHECK(saw_moving);                                     /* it was red on the way in */
        IM_CHECK_GT(t_ok, 3.0);                                   /* approach 2 s + hold 1 s, no sooner */
        IM_CHECK(s.have_truth);
        float tgt[3]; { std::lock_guard<std::mutex> lk(PL.mu); memcpy(tgt, PL.target, sizeof tgt); }
        const float settled[3] = { tgt[0] + 0.003f, tgt[1] - 0.002f, tgt[2] + 0.004f };
        IM_CHECK_LT(dist3(s.truth, settled), 0.0005f);            /* the truth HAS settled when it goes green */
        IM_CHECK_LT(dist3(s.g.mean, s.truth), 0.001f);            /* and the gate's mean is that truth */
        ctx->CaptureScreenshotWindow("//calib view");
    };

    /* a run started while tracking is live takes the MEASURED center, not the typed field: the trims are
     * solved at the stand's true center, a few mm off the target; the Aim tab does the same, and with
     * no body-frame survey it keeps the tilt meter and refuses the position readout */
    t = IM_REGISTER_TEST(e, "placement", "run_uses_center");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("calib view");
        ctx->ItemClick("**/Capture");
        ctx->Yield(2);
        IM_CHECK_EQ(PL.state.load(), 2);                          /* sim_gate left it connected */
        J.mic_set = false;
        ctx->ItemClick("**/##cl");   ctx->KeyCharsReplaceEnter(FIX_A);
        ctx->ItemClick("**/##co");   ctx->KeyCharsReplaceEnter("calibview_place_out.json");
        ctx->ItemCheck("**/simulate");
        ctx->ItemClick("**/Run calibration");
        double t0 = ImGui::GetTime();
        while (J.state.load() == 1 && ImGui::GetTime() - t0 < 90.0) ctx->Yield();
        IM_CHECK_EQ(J.state.load(), 2);
        IM_CHECK(J.tk.used);
        IM_CHECK(!J.tk.bumped);
        printf("placement test: run took (%.4f %.4f %.4f), %.1f mm off the target, truth %.2f mm away\n",
               J.mic_run[0], J.mic_run[1], J.mic_run[2], J.tk.dist_mm, dist3(J.mic_run, J.tk.truth) * 1e3f);
        IM_CHECK_LT(dist3(J.mic_run, J.tk.truth), 0.001f);        /* solved where the mic stood */
        IM_CHECK_GT(dist3(J.mic_run, J.tk.target), 0.003f);       /* ...which is not the target */
        IM_CHECK_GT(dist3(J.mic_run, J.mic), 0.003f);             /* ...nor the typed field */
        IM_CHECK(J.tk.dist_mm > 3.f && J.tk.dist_mm < 9.f);        /* the delta the run shows */
        ctx->CaptureScreenshotWindow("//calib view");

        /* the Aim tab: layout A, speaker 3, simulate */
        ctx->ItemClick("**/##A");
        ctx->KeyCharsReplaceEnter(FIX_A);
        ctx->ItemClick("**/Load A");
        ctx->ItemClick("**/Aim");
        ctx->Yield(2);
        ctx->ItemCheck("**/simulate##aim");
        ctx->ItemInputValue("**/speaker##aim", 3);
        ctx->ItemClick("**/Start##aim");
        t0 = ImGui::GetTime();
        for (;;) {
            int n; { std::lock_guard<std::mutex> lk(AJ.mu); n = AJ.d.n; }
            if (n >= 1 || AJ.state.load() != 1 || ImGui::GetTime() - t0 > 60.0) break;
            ctx->Yield();
        }
        AimData d; { std::lock_guard<std::mutex> lk(AJ.mu); d = AJ.d; }
        IM_CHECK_EQ(AJ.state.load(), 1);
        IM_CHECK(d.tk.used);
        IM_CHECK_LT(dist3(d.tk.center, d.tk.truth), 0.001f);
        IM_CHECK(d.have_tilt);                                    /* the tilt meter runs */
        IM_CHECK(!d.have_pos);                                    /* no body-frame survey: no position */
        ctx->CaptureScreenshotWindow("//calib view");
        ctx->ItemClick("**/Stop##aim");
        t0 = ImGui::GetTime();
        while (AJ.state.load() == 1 && ImGui::GetTime() - t0 < 30.0) ctx->Yield();
        IM_CHECK_EQ(AJ.state.load(), 2);
    };

    /* the stand knocked 15 mm after the third capture: the run stops, flags it, writes nothing */
    t = IM_REGISTER_TEST(e, "placement", "bump");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("calib view");
        ctx->ItemClick("**/Capture");
        ctx->Yield(2);
        ctx->ItemOpen("**/Placement (tracked ZM-1)");
        ctx->Yield(2);
        if (PL.state.load() == 2) { ctx->ItemClick("**/Disconnect##pl"); pl_wait_state(ctx, 0, 10.0); }
        IM_CHECK_EQ(PL.state.load(), 0);
        ctx->ItemCheck("**/simulate##pl");
        ctx->ItemCheck("**/bump mid-run##pl");
        ctx->ItemClick("**/Connect##pl");
        pl_wait_state(ctx, 2, 10.0);
        IM_CHECK_EQ(PL.state.load(), 2);
        remove("calibview_bump_out.json");
        ctx->ItemClick("**/##co");   ctx->KeyCharsReplaceEnter("calibview_bump_out.json");
        ctx->ItemClick("**/Run calibration");
        const double t0 = ImGui::GetTime();
        while (J.state.load() == 1 && ImGui::GetTime() - t0 < 90.0) ctx->Yield();
        printf("placement test: bump run: state %d, %s\n", J.state.load(), J.msg);
        IM_CHECK_EQ(J.state.load(), 3);
        IM_CHECK(J.tk.bumped);
        IM_CHECK_EQ(J.tk.bump_after, PL_SIM_BUMP_AFTER - 1);      /* noticed right after the third capture */
        IM_CHECK(J.tk.bump_mm > 10.f && J.tk.bump_mm < 20.f);
        IM_CHECK(strstr(J.msg, "BUMP") != NULL);
        FILE* f = fopen("calibview_bump_out.json", "rb");
        IM_CHECK(f == NULL);                                      /* nothing written */
        if (f) fclose(f);
        ctx->CaptureScreenshotWindow("//calib view");
        ctx->ItemClick("**/Disconnect##pl");                      /* leave no tracker behind */
        pl_wait_state(ctx, 0, 10.0);
        ctx->ItemUncheck("**/bump mid-run##pl");
        IM_CHECK_EQ(PL.state.load(), 0);
    };

    t = IM_REGISTER_TEST(e, "viewer", "tabs");                   /* every tab renders without faulting */
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("calib view");
        const char* tabs[] = { "**/Array", "**/Trims", "**/EQ", "**/IRs", "**/Diff", "**/Capture", "**/Zylia", "**/Aim" };
        for (int i = 0; i < 8; ++i) { ctx->ItemClick(tabs[i]); ctx->Yield(2); }
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
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("usage: calib_view [layoutA.json] [layoutB.json] [--irs prefix] [--tests [filter]]\n"
                   "  calibration station: array 3D view, trims, EQ curves, IRs, a layout diff (A vs\n"
                   "  B), the Capture tab (sweep->measure->solve->writeback), the Zylia DOA tab, and the\n"
                   "  Aim tab (live aiming of one of A's speakers with the ZM-1).\n");
            return 0;
        }
        else if (argv[i][0] != '-' && npos < 2) {
            if (npos++ == 0) snprintf(V.pathA, sizeof V.pathA, "%s", argv[i]);
            else             snprintf(V.pathB, sizeof V.pathB, "%s", argv[i]);
        }
        else { fprintf(stderr, "usage: calib_view [layoutA.json] [layoutB.json] [--irs prefix] [--tests [filter]]\n"); return 2; }
    }
    for (int k = 0; k < EQ_PTS; ++k) V.eqfreq[k] = 20.0f * powf(1000.0f, (float)k / (EQ_PTS - 1));
    J.simulate = true;                                            /* hardware capture is the rig-day opt-out */
    if (selftest) {
        if (!write_fixture(FIX_A, 0) || !write_fixture(FIX_B, 1) || !write_fixture(FIX_D, 2) || !write_fixture(FIX_E, 3)) {
            fprintf(stderr, "calib_view: cannot write fixtures in cwd\n"); return 1; }
    } else {
        if (!V.pathA[0]) snprintf(V.pathA, sizeof V.pathA, "cave_layout.json");
        if (V.pathA[0])  load_layout(0);                          /* best effort; status shows any error */
        if (V.pathB[0])  load_layout(1);
        if (V.irprefix[0]) load_irs();
    }

    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc = { sizeof wc, CS_CLASSDC, wnd_proc, 0, 0, GetModuleHandleW(NULL), NULL, NULL, NULL, NULL, L"bwa_calib_view", NULL };
    wc.hIcon = wc.hIconSm = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));   /* the .rc icon (title bar/taskbar) */
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"bw_audio - calibration report viewer", WS_OVERLAPPEDWINDOW,
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
    g_uiScale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);         /* DPI rides FontScaleMain (bwa_theme.h) */
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
    for (int i = 0; i < BWA_MAX_CHANNELS; ++i) if (V.hasIR[i]) sound_unload(&V.ir[i]);
    if (Z.live) zylia_capture_close();
    if (J.th_live) { J.cancel.store(true); J.th.join(); }        /* reap a still-running capture job */
    if (AJ.th_live) { AJ.stop.store(true); AJ.th.join(); }       /* ... and a live aiming run */
    if (PL.th_live) { PL.stop.store(true); PL.th.join(); }       /* ... and the placement poller */
    return rc;
}
