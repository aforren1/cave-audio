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
#include "clicker_track.h"         /* the capsule survey's tracked clicker: measured clap positions */
#include "calib_session.h"         /* the Session tab: the rig-day steps as one guided, resumable session */
#include <cJSON.h>                 /* the Placement panel peeks at a survey's frame and offset */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include <d3d11.h>
#include <commdlg.h>       /* GetOpenFileNameA: the native file picker (comdlg32) */
#include <math.h>
#include <stdarg.h>
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
 * it took. The poller runs TWO gates on the same poses: `g` reads the center only (the Capture tab's
 * trims and verify), `gd` also holds while the stand turns (the Aim tab's position readout, which turns
 * capsule arrival differences into a room direction). A run waits on the one its mode needs, and only a
 * direction run's bump check stops on a turn. The panel itself only reads snapshots. Unverified against
 * live Motive. */
struct PlaceSnap {
    bool      have_pose;
    float     q[4];
    PlaceGate g;                         /* the gate after the last poll: state, center, mean, delta */
    PlaceGate gd;                        /* the same poses through the direction gate (place_cfg_direction) */
    bool      sim_twisted;               /* simulate: the script's own record that it twisted the stand */
    float     yaw_deg, tilt_deg;
    bool      have_truth; float truth[3];/* simulate: the stand's true center */
    unsigned long long seq;              /* polls published */
};

struct PlaceJob {
    /* config: the UI writes these only while disconnected */
    char  body[64], server[64], multicast[64], survey[512];
    bool  sim, sim_bump, sim_twist;
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
#define PL_SIM_BUMP_AFTER 3              /* the simulated knock (and twist): after the third capture */

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
    /* A body-frame survey that carries its own mount offset wins, as the panel says: that is what
     * bwa_calibrate --capsule-survey --track writes. The panel always has an offset mode selected, and
     * mic_track_open refuses a second offset, so without this the survey the runbook hands the Aim tab
     * could never connect. */
    if (PL.survey[0]) {
        FILE* sf = fopen(PL.survey, "rb");
        if (sf) {
            static char sbuf[65536];                     /* one poller start at a time */
            const size_t sn = fread(sbuf, 1, sizeof sbuf - 1, sf);
            fclose(sf);
            sbuf[sn] = 0;
            cJSON* sj = cJSON_Parse(sbuf);
            const cJSON* fr = cJSON_GetObjectItemCaseSensitive(sj, "frame");
            if (sj && cJSON_IsString(fr) && !strcmp(fr->valuestring, "body") &&
                cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(sj, "mount_offset_m")))
                mc.have_offset = mc.offset_ring = 0;
            cJSON_Delete(sj);
        }
    }
    mc.sim = PL.sim ? MIC_SIM_SCRIPT : MIC_SIM_OFF;
    mc.sim_bump_after = (PL.sim && PL.sim_bump) ? PL_SIM_BUMP_AFTER : 0;
    mc.sim_twist_after = (PL.sim && PL.sim_twist) ? PL_SIM_BUMP_AFTER : 0;
    char e[400] = { 0 };
    if (mic_track_open(&PL.mt, &mc, e, sizeof e) != 0) {
        snprintf(PL.msg, sizeof PL.msg, "%s", e);
        PL.state.store(3, std::memory_order_release);
        return;
    }
    mic_track_describe(&PL.mt, PL.desc, sizeof PL.desc);
    PL.body_frame = PL.mt.body_frame != 0;
    PlaceCfg cfg, cfgd;
    float tgt[3], tol;
    { std::lock_guard<std::mutex> lk(PL.mu); memcpy(tgt, PL.target, sizeof tgt); tol = PL.tol_mm; memset(&PL.d, 0, sizeof PL.d); }
    /* the direction gate's stillness term at the floor: the poller does not know the Aim tab's speaker,
     * and at the 10 mm default every live readout's limit IS the floor (placement.h) */
    auto set_cfg = [&]() {
        place_cfg_default(&cfg, tol * 1e-3f);
        cfgd = cfg;
        place_cfg_direction(&cfgd, PLACE_TURN_MIN_DEG);
    };
    set_cfg();
    static PlaceGate g, gd;                              /* static: two 6 KB rings; one poller thread at a time */
    place_gate_init(&g, &cfg);
    place_gate_init(&gd, &cfgd);
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
            place_gate_reset(&gd);
        }
        if (ntol != tol) { tol = ntol; set_cfg(); place_gate_init(&g, &cfg); place_gate_init(&gd, &cfgd); }
        MicPose ps;
        const bool have = mic_track_read(&PL.mt, &ps) != 0;
        const double now = mic_track_now(&PL.mt);
        place_gate_update_q(&g, now, have ? ps.center : NULL, have ? ps.q : NULL, tgt);
        place_gate_update_q(&gd, now, have ? ps.center : NULL, have ? ps.q : NULL, tgt);
        static PlaceSnap s;                              /* static: it carries both gates */
        memset(&s, 0, sizeof s);
        s.have_pose = have;
        if (have) {
            memcpy(s.q, ps.q, sizeof s.q);
            place_mount_angles(ps.q, &s.yaw_deg, &s.tilt_deg);
        }
        s.g = g;
        s.gd = gd;
        s.have_truth = mic_track_sim_truth(&PL.mt, s.truth) != 0;
        s.sim_twisted = mic_track_sim_twisted(&PL.mt) != 0;
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

/* What a run took from the panel: the center, the delta it used, and whether it moved or turned. */
struct PlaceTake {
    bool  used;                          /* this run took its center from the tracker */
    float center[3], target[3], delta_mm[3], dist_mm;
    float tol_m;
    float q[4];                          /* the orientation taken (the window mean when there is one) */
    float turn_limit_deg;                /* > 0: a direction run, whose bump check also stops on a turn */
    bool  bumped; float bump_mm; int bump_after;
    bool  turned; float turn_deg;        /* the bump was a turn (turned), and its size */
    float last_turn_deg, max_turn_deg;   /* the turn since the take, read after every capture */
    bool  have_truth; float truth[3];    /* simulate: the truth at the take (the tests read it) */
    bool  sim_twisted;                   /* simulate: the script twisted the stand (its own record) */
};

/* In a run's worker: wait for the gate (dir: the direction gate, which also holds while the stand
 * turns), then take the window mean. 0 = taken; 1 = canceled; 2 = lost the tracker; 3 = timed out. */
static int pl_take(PlaceTake* tk, const std::atomic<bool>& cancel, double timeout_s, float q_out[4], bool dir) {
    const auto t0 = std::chrono::steady_clock::now();
    static PlaceSnap s;                                  /* static: two gates; one run worker at a time */
    for (;;) {
        if (cancel.load(std::memory_order_relaxed)) return 1;
        if (!pl_live()) return 2;
        s = pl_snap();
        const PlaceGate& g = dir ? s.gd : s.g;
        if (g.state == PLACE_OK) {
            float tgt[3];
            { std::lock_guard<std::mutex> lk(PL.mu); memcpy(tgt, PL.target, sizeof tgt); tk->tol_m = PL.tol_mm * 1e-3f; }
            memcpy(tk->center, g.mean, sizeof tk->center);
            memcpy(tk->target, tgt, sizeof tk->target);
            for (int a = 0; a < 3; ++a) tk->delta_mm[a] = (g.mean[a] - tgt[a]) * 1e3f;
            tk->dist_mm = g.mean_dist_m * 1e3f;
            tk->have_truth = s.have_truth;
            memcpy(tk->truth, s.truth, sizeof tk->truth);
            memcpy(tk->q, g.have_q ? g.qmean : s.q, sizeof tk->q);   /* one pose carries Motive's jitter */
            memcpy(q_out, tk->q, 4 * sizeof(float));
            tk->used = true; tk->bumped = false; tk->bump_mm = 0.f; tk->bump_after = -1;
            tk->turn_limit_deg = 0.f; tk->turned = false; tk->turn_deg = 0.f;
            tk->last_turn_deg = tk->max_turn_deg = 0.f; tk->sim_twisted = s.sim_twisted;
            return 0;
        }
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > timeout_s) return 3;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

/* In a run's worker, after capture `idx`: note it, wait for a fresh pose, compare. Returns 1 when the
 * mic moved past half the tolerance, or, in a direction run (tk->turn_limit_deg > 0), turned past its
 * limit (tk records which); 0 otherwise, including no fresh pose. The turn since the take is recorded in
 * every run, but only a direction run judges it. The simulated truth for the NEXT capture comes back in
 * truth_out (when simulating). */
static int pl_after_capture(PlaceTake* tk, int idx, double capture_s, float truth_out[3]) {
    unsigned long long s0;
    { std::lock_guard<std::mutex> lk(PL.mu); s0 = PL.d.seq; }
    mic_track_note_capture(&PL.mt, capture_s);
    static PlaceSnap s;                                  /* static: two gates; one run worker at a time */
    for (int tries = 0; tries < 100 && pl_live(); ++tries) {   /* up to about a second */
        s = pl_snap();
        if (s.seq >= s0 + 2) {
            if (s.have_truth && truth_out) memcpy(truth_out, s.truth, 3 * sizeof(float));
            tk->sim_twisted = s.sim_twisted;
            if (!s.have_pose) return 0;
            float moved = 0.f, turned = 0.f;
            if (place_turn_deg(tk->q, s.q, &turned)) {
                tk->last_turn_deg = turned;
                if (turned > tk->max_turn_deg) tk->max_turn_deg = turned;
            }
            const int b = place_bump_pose(tk->center, tk->q, s.g.center, s.q, tk->tol_m, tk->turn_limit_deg, &moved, &turned);
            if (b > 0) {
                tk->bumped = true; tk->bump_mm = moved * 1e3f; tk->bump_after = idx;
                tk->turned = !(b & PLACE_BUMP_MOVED); tk->turn_deg = turned;
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
                  bwTip("knock the simulated stand 15 mm after the third capture of a run");
                  ImGui::SameLine(); ImGui::Checkbox("twist mid-run##pl", &PL.sim_twist);
                  bwTip("turn the simulated stand 2 deg about the array center after the third capture: no center "
                        "moves, so only the Aim tab's position readout (a direction) stops on it"); }
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
            place_move_words(s.g.delta, PLACE_MOVE_DEAD_STAND_M, words, sizeof words);   /* live aiming's words too */
            ImGui::Text("%s", words);
            ImGui::Text("dx %+.1f  dy %+.1f  dz %+.1f mm  (center minus target)", s.g.delta[0] * 1e3f, s.g.delta[1] * 1e3f, s.g.delta[2] * 1e3f);
            ImGui::Text("center (%.3f %.3f %.3f)  spread %.1f mm", s.g.center[0], s.g.center[1], s.g.center[2], s.g.spread_m * 1e3f);
            ImGui::Text("mount yaw %.1f deg, tilt %.1f deg%s", s.yaw_deg, s.tilt_deg, PL.body_frame ? ", capsule table follows the stand" : "");
            if (s.g.state == PLACE_SETTLING) ImGui::TextDisabled("held %.1f of %.1f s", s.g.held_s, s.g.cfg.hold_s);
            /* the Aim tab's position readout waits on the direction gate: say so when only it holds */
            if (PL.body_frame && s.gd.have_q)
                ImGui::TextDisabled("orientation spread %.2f deg (limit %.2f)%s", s.gd.turn_spread_deg, s.gd.cfg.still_deg,
                                    s.g.state == PLACE_OK && s.gd.state != PLACE_OK ? ": the Aim position readout waits" : "");
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
    if (tk.bumped && tk.turned)
        ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f), "BUMP: the ZM-1 turned %.2f deg after capture %d (limit %.2f deg), "
                           "its center in place: every direction after it is off by the turn. Re-place it and run again",
                           tk.turn_deg, tk.bump_after, tk.turn_limit_deg);
    else if (tk.bumped)
        ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f), "BUMP: the ZM-1 moved %.1f mm after capture %d (limit %.1f mm): "
                           "re-place it and run again", tk.bump_mm, tk.bump_after, tk.tol_m * 0.5e3f);
}

/* ============ Capture tab — run the calibration sweep (bwa_calibrate's core flow, in-window) ============
 * A worker thread runs sweep -> measure per speaker -> calib_solve -> writeback (the trims), or the
 * same sweep THROUGH the layout's output stage -> calib_verify_residuals (the verify pass, nothing
 * written). Every speaker goes through calib_measure_speaker, the CLI's own capture-and-measure, with
 * the omni or the ZM-1 (19 capsules, cross-correlated arrivals, the pressure proxy), so the tab and
 * bwa_calibrate cannot measure differently. Simulate runs hardware-free; the ASIO full-duplex path
 * compiles in with the SDK and is unverified at the rig. Publication is row-at-a-time: the
 * worker fills a speaker's result fields, THEN bumps done_count (release); the UI only reads rows
 * below done_count (acquire) — live progress with no torn rows. On success the result loads straight
 * into the Diff view (A = input layout, B = what calibration wrote): capture -> review, one window. */
struct CapJob {
    char  layout[512], out[512], irprefix[512];      /* config (UI writes only while idle) */
    char  ran_layout[512], ran_out[512];             /* paths snapshotted at Run: the worker + the
                                                      * Load-into-Diff button use THESE, so edits made
                                                      * after the run can't change what gets diffed */
    float mic[3];
    int   mic_in;                                    /* the omni's input, or the ZM-1's FIRST capsule input */
    bool  simulate, do_room, do_eq, do_irs;
    bool  zylia;                                     /* the mic: false = one omni input, true = the ZM-1 */
    int   pass;                                      /* 0 = trims (writes the layout), 1 = verify (writes nothing) */
    bool  sim_room;                                  /* simulate inside the shoebox room (--sim-room 0.3) */
    char  survey[512];                               /* the ZM-1: an optional ROOM-AXES capsule survey */
    char  driver[128];
    /* snapshotted at Run, for the worker and for reading the results after the fields change */
    bool  ran_zylia, ran_room; int ran_pass; char ran_survey[512];
    int   table_src;                                 /* the ZM-1 run's capsule table: 0 built-in, 1 the tab's
                                                      * survey, 2 the panel's room-axes survey, 3 the panel's
                                                      * body-frame survey re-aimed by the pose */
    float run_ref_off_m;                             /* the run's mic against the layout's listening point */
    float spread_db[BWA_MAX_CHANNELS];               /* the ZM-1: capsule level spread per speaker */
    float v_arr_us[BWA_MAX_CHANNELS], v_lvl_db[BWA_MAX_CHANNELS];   /* verify residuals, valid when state == 2 */
    int   v_flag[BWA_MAX_CHANNELS];
    CalibVerifySummary vs;
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
    int      sweeps[BWA_MAX_CHANNELS];                /* sweeps each speaker took (2 = clean and agreeing) */
    float    snr_db[BWA_MAX_CHANNELS];                /* the accepted sweep's IR SNR (sweep quality, calib.h) */
    char     win_desc[300];                           /* the run's arrival window, as calib_window_prior says it */
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
/* The Session tab's subprocess (below). ASIO allows one driver per process, and a driver is usually one
 * client at a time, so while a session step runs a tool, no tab here may open the device, and a step
 * waits for every tab to let go of it. */
static bool ses_busy(void);

static void cap_fail(const char* m) { snprintf(J.msg, sizeof J.msg, "%s", m); J.state.store(3, std::memory_order_release); }

/* What a Capture run changes in process-wide state, put back on every way out: the ASIO shell, the
 * simulated room, and the installed capsule table. A ZM-1 run installs the table its CLI twin would
 * have (bwa_calibrate is one process per run and never has to put it back); this window keeps the
 * Zylia and Aim tabs, which read the same table, so it restores what was there. zylia.c has no "is a
 * survey installed" query: an installed table equal to the built-in one restores as the built-in,
 * which is the same table. */
struct CapScope {
    float saved[ZYLIA_MICS][3];
    bool  had = false, armed = false, room = false, asio = false;
    void save_table() {
        zylia_capsules(saved);
        zylia_set_capsules(NULL);
        float b[ZYLIA_MICS][3];
        zylia_capsules(b);
        had = memcmp(saved, b, sizeof b) != 0;
        armed = true;                                            /* the built-in table is installed now */
    }
    void done() {
#ifdef BWA_HAVE_ASIO
        if (asio) calib_asio_close();
#endif
        if (room) calib_sim_set_room(0.f);                       /* do not leak the room into the Aim tab */
        if (armed) zylia_set_capsules(had ? saved : NULL);
        asio = room = armed = false;
    }
    ~CapScope() { done(); }
};

static void cap_worker(void) {
    char err[256];
    static Layout L;          /* never a stack local (layout.h); one capture job at a time */
    if (!layout_load(J.ran_layout, (uint32_t)CAL_FS, &L, err, sizeof err)) { cap_fail(err); return; }
    const int n = (int)L.count;
    J.n.store(n);
    const bool zy = J.ran_zylia, verify = J.ran_pass == 1;
    /* the speed of sound the CLI would use: the layout's recorded one, else the 20 C reference. The
     * simulator generates at it, and the ZM-1's center arrival and the verify residuals divide by it. */
    double sos = BWA_SOS_REF_MPS;
    { double v; if (calib_read_sos(J.ran_layout, &v)) sos = v; }
    static float sweep[CAL_NSWEEP], cap[CAL_CAPLEN], irbuf[CAL_IRLEN];   /* one job at a time; off the stack */
    static float play[CAL_CAPLEN], tmp[CAL_CAPLEN];
    static float cap19[(size_t)ZYLIA_MICS * CAL_CAPLEN];         /* the ZM-1's rows, the ASIO shell's stride */
    static float eq_taps[(size_t)BWA_MAX_CHANNELS * BWA_EQ_TAPS];
    static uint16_t eq_lens[BWA_MAX_CHANNELS];
    static MeasureResult res[BWA_MAX_CHANNELS];
    memset(eq_lens, 0, sizeof eq_lens);
    measure_sweep(sweep, CAL_NSWEEP, CAL_F1, CAL_F2, CAL_FS);
    /* verify on the rig plays the sweep through the stage, longer than the sweep by the stage's delay
     * and FIR; the recording stays CAL_CAPLEN, so the tail must still hold the latency and the decay */
    const int nplay = CAL_NSWEEP + calib_stage_pad(&L);
    if (verify && nplay > CAL_CAPLEN - CAL_NTAIL / 2) {
        snprintf(err, sizeof err, "verify: the layout's delays (%.1f ms at most) leave the %.2f s capture tail too short",
                 1e3 * L.max_delay_samples / CAL_FS, CAL_NTAIL / CAL_FS);
        cap_fail(err); return;
    }
    const bool do_eq = J.do_eq && !zy && !verify;                /* the CLI refuses --eq with the ZM-1 */
    const bool do_room = J.do_room && !verify, do_irs = J.do_irs && !verify;

    CapScope sc;
    /* the Placement panel was live at Run: wait for its gate, then the measured center IS the mic */
    J.tk.used = false;
    float q[4] = { 0.f, 0.f, 0.f, 1.f };
    if (J.ran_tracked) {
        J.phase.store(1);
        const int r = pl_take(&J.tk, J.cancel, 300.0, q, false);   /* trims and verify read only the center */
        J.phase.store(0);
        if (r) {
            cap_fail(r == 1 ? "canceled" : r == 2 ? "the tracker went away before the placement settled"
                                                  : "placement timed out (300 s): the ZM-1 never sat in tolerance and still");
            return;
        }
        memcpy(J.mic_run, J.tk.center, sizeof J.mic_run);
    }

    /* The ZM-1's capsule table, as bwa_calibrate would have it. Tracked: the Placement panel's survey
     * under mic_track's rules (a body-frame table re-aimed by the pose the gate accepted, a room-axes
     * one installed for its channel order and geometry, none = the built-in table). Untracked: the
     * tab's survey, room axes only (a body-frame table cannot be re-aimed with no pose), else the
     * built-in table. The trims read only the center, so none of this is required: it moves the
     * center arrival's tilt correction by about 15 us at most. */
    J.table_src = 0;
    if (zy) {
        sc.save_table();
        ZyliaMount m;
        char e[192] = { 0 };
        if (J.ran_tracked) {
            if (mic_track_aim_capsules(&PL.mt, q)) J.table_src = 3;
            else if (PL.survey[0]) {
                if (!zylia_survey_load(PL.survey, &m, e, sizeof e)) {
                    snprintf(err, sizeof err, "the Placement panel's survey: %s", e); cap_fail(err); return; }
                J.table_src = 2;
            }
        } else if (J.ran_survey[0]) {
            if (!zylia_survey_load(J.ran_survey, &m, e, sizeof e)) {
                snprintf(err, sizeof err, "survey: %s", e); cap_fail(err); return; }
            if (m.body_frame) {
                cap_fail("the survey is BODY-FRAME (a tracked mount): with no tracker it cannot be re-aimed. Connect "
                         "the Placement panel with it, or use a room-axes survey taken at the current mounting");
                return;
            }
            J.table_src = 1;
        }
    }
    if (J.simulate) { calib_sim_set_room(J.ran_room ? 0.3f : 0.f); sc.room = true; }

#ifdef BWA_HAVE_ASIO
    if (!J.simulate) {
        const char* drv = J.driver[0] ? J.driver : NULL;
        if ((zy ? calib_asio_open_multi(drv, J.mic_in, ZYLIA_MICS, n, sweep, cap19)
                : calib_asio_open(drv, J.mic_in, n, sweep, cap)) != 0) {
            char m[128];
            if (zy) snprintf(m, sizeof m, "ASIO open failed (>=%d outs + 19 ZM-1 inputs from %d; see console)", n, J.mic_in);
            else    snprintf(m, sizeof m, "ASIO open failed (>=%d outs + the mic input; see console)", n);
            cap_fail(m); return;
        }
        sc.asio = true;
    }
#else
    if (!J.simulate) { cap_fail("built without the ASIO SDK - simulate only"); return; }
#endif

    float sim_at[3];                                             /* simulate + a tracked stand: its TRUE center */
    const bool sim_truth = J.tk.used && J.tk.have_truth && J.simulate;
    if (sim_truth) memcpy(sim_at, J.tk.truth, sizeof sim_at);
    static CalibPass P;                                          /* static: it carries the capsule table */
    memset(&P, 0, sizeof P);
    P.L = &L; P.sim_at = sim_truth ? sim_at : J.mic_run; P.sos = sos; P.simulate = J.simulate; P.zylia = zy;
    P.sweep = sweep; P.cap = cap; P.cap19 = cap19;
    zylia_capsules(P.caps);                                      /* the table settled above */
    P.play = play; P.nplay = nplay; P.tmp = tmp;
    /* sweep quality, the CLI's own rule (calib_measure_speaker): the expected-arrival window from the
     * run's mic, per speaker surveyed or at its plan; a trim or verify value counts once two sweeps agree */
    P.mic = J.mic_run;
    P.have_win = calib_window_prior(J.simulate, -1.0, -1.0, &P.win, J.win_desc, sizeof J.win_desc);
    P.agree = 1;
    for (int i = 0; i < n; ++i) {
        if (J.cancel.load(std::memory_order_relaxed)) { cap_fail("canceled"); return; }
        CalibMeasInfo mi;
        const int r = calib_measure_speaker(&P, i, verify ? 1 : 0, &res[i], &mi);
        J.sweeps[i] = mi.sweeps;
        J.snr_db[i] = mi.snr_db;
        if (mi.log[0]) printf("calib_view: speaker %d re-swept:\n%s", i, mi.log);
        if (r == CALIB_MEAS_UNCLEAN) {
            char last[200] = "";
            const char* q = mi.log; const char* ln = q;               /* the last reason line */
            for (const char* c = q; *c; ++c) if (c[0] == '\n' && c[1]) ln = c + 1;
            snprintf(last, sizeof last, "%.*s", (int)strcspn(ln, "\n"), ln);
            snprintf(err, sizeof err, "speaker %d: no clean measurement in %d sweeps (%s). Nothing written", i, mi.sweeps, last);
            cap_fail(err); return;
        }
        if (r == CALIB_MEAS_TIMEOUT) { cap_fail("capture timed out (speaker not wired? see console)"); return; }
        if (r == CALIB_MEAS_DEAD) {
            snprintf(err, sizeof err, "speaker %d: ZM-1 capsule %d (input %d) is dead (more than %.0f dB under the capsules' "
                     "median): check the routing with bwa_zylia_probe. Nothing written", i, mi.dead, J.mic_in + mi.dead, ZYLIA_PROXY_DEAD_DB);
            cap_fail(err); return;
        }
        if (r != CALIB_MEAS_OK) { snprintf(err, sizeof err, "measurement failed on speaker %d", i); cap_fail(err); return; }
        J.arrival_ms[i] = (float)(((double)res[i].delay_samples + res[i].delay_frac) * 1000.0 / CAL_FS);
        J.level[i]      = res[i].level;
        J.spread_db[i]  = zy ? mi.capsule_spread_db : 0.f;
        J.rt60[i]       = 0.f;
        J.eqlen[i]      = 0;
        if (do_room || do_eq || do_irs) {                        /* the ZM-1: `cap` is the capsules' mean */
            RoomResult rr;
            int want_ir = (do_eq || do_irs);
            measure_room(cap, CAL_CAPLEN, sweep, CAL_NSWEEP, CAL_F1, CAL_F2, CAL_FS, &rr, want_ir ? irbuf : NULL, want_ir ? CAL_IRLEN : 0);
            J.rt60[i] = rr.rt60;
            if (do_irs && J.irprefix[0]) { char p[600]; snprintf(p, sizeof p, "%s_%02d.wav", J.irprefix, i); calib_write_wav_f32(p, irbuf, CAL_IRLEN, (int)CAL_FS); }
            if (do_eq) {                                         /* gate to before the first reflection, invert */
                int first_refl = rr.er_count ? rr.er_delay[0] : 0;
                if (calib_eq(irbuf, CAL_IRLEN, first_refl, CAL_FS, 256, &eq_taps[(size_t)i * BWA_EQ_TAPS])) {
                    eq_lens[i] = 256; J.eqlen[i] = 256;
                }
            }
        }
        /* the bump check: a trim set measured across a moved mic is wrong, so the run stops */
        if (J.tk.used && pl_after_capture(&J.tk, i, CAL_CAPLEN / CAL_FS, sim_truth ? sim_at : NULL)) {
            char m[200];
            snprintf(m, sizeof m, "BUMP: the ZM-1 moved %.1f mm after speaker %d (limit %.1f mm): nothing written",
                     J.tk.bump_mm, i, J.tk.tol_m * 0.5e3f);
            cap_fail(m);
            return;
        }
        J.done_count.store(i + 1, std::memory_order_release);    /* publish the completed row */
    }
    sc.done();                                                   /* the device and the table, before the solve */

    float gdb[BWA_MAX_CHANNELS], dms[BWA_MAX_CHANNELS];
    static float pos[BWA_MAX_CHANNELS][3];                            /* calib_solve wants a packed [3]-stride array */
    for (int i = 0; i < n; ++i) { pos[i][0] = L.speakers[i].pos[0]; pos[i][1] = L.speakers[i].pos[1]; pos[i][2] = L.speakers[i].pos[2]; }
    {   /* the trims align arrivals at the mic; the engine assumes the listening point (the CLI's 5 cm warning) */
        const float ex = J.mic_run[0] - L.ref[0], ey = J.mic_run[1] - L.ref[1], ez = J.mic_run[2] - L.ref[2];
        J.run_ref_off_m = sqrtf(ex * ex + ey * ey + ez * ez);
    }
    /* the directivity re-aim, when the layout carries a model AND the user set the mic: the same
     * factor and the same rule as the CLI (calib_directivity_corr, only with --mic), so the two tools
     * cannot write different trims from one capture */
    static float corr[BWA_MAX_CHANNELS];
    const float* cp = (J.ran_mic_set && calib_directivity_corr(&L, J.mic_run, 2.0 * CAL_F1, 0.5 * CAL_F2, res, corr))
                    ? corr : NULL;
    J.corr_applied = cp != NULL;
    if (verify) {
        /* align_pt = the mic: verify runs from the trim run's placement, where the trims made every
         * arrival equal (calib.h), exactly as bwa_calibrate --verify passes it */
        CalibVerifySummary vs;
        if (calib_verify_residuals(res, pos, J.mic_run, J.mic_run, n, CAL_FS, sos, cp, J.v_arr_us, J.v_lvl_db, J.v_flag, &vs) < 0) {
            cap_fail("verify: bad input to the residuals"); return; }
        J.vs = vs;
        snprintf(J.msg, sizeof J.msg, "verify: %d of %d speaker(s) flagged (arrival beyond +/-%.0f us, level beyond +/-%.1f dB); nothing written",
                 vs.nflag, n, CALIB_VERIFY_ARRIVAL_US, CALIB_VERIFY_LEVEL_DB);
        J.state.store(2, std::memory_order_release);
        return;
    }
    calib_solve_corr(res, pos, J.mic_run, n, CAL_FS, cp, gdb, dms);
    memcpy(J.gain_db, gdb, sizeof gdb);
    memcpy(J.trim_ms, dms, sizeof dms);
    if (!calib_write_layout(J.ran_layout, J.ran_out, gdb, dms, n, err, sizeof err)) { cap_fail(err); return; }
    if (do_eq && !calib_write_eq(J.ran_out, J.ran_out, eq_taps, eq_lens, n, BWA_EQ_TAPS, err, sizeof err)) { cap_fail(err); return; }
    snprintf(J.msg, sizeof J.msg, "wrote %s%s%s", J.ran_out, do_eq ? " (trims + eq)" : " (trims)",
             zy ? ", the ZM-1's 19 capsules pooled" : "");
    J.state.store(2, std::memory_order_release);
}

static void tab_capture(void) {
    int  st      = J.state.load(std::memory_order_acquire);
    bool running = (st == 1);
    if (!J.layout[0] && V.hasA) snprintf(J.layout, sizeof J.layout, "%s", V.pathA);   /* sensible defaults */
    if (!J.out[0]) snprintf(J.out, sizeof J.out, "calibrated.json");

    ImGui::BeginDisabled(running);
    ImGui::RadioButton("trims##cpass", &J.pass, 0);
    bwTip("sweep the raw outputs, solve delay and gain trims, write them to the layout out (bwa_calibrate --trims)");
    ImGui::SameLine(); ImGui::RadioButton("verify##cpass", &J.pass, 1);
    bwTip("play every speaker THROUGH the layout's own output stage and flag what is off: arrival beyond +/-100 us, "
          "level beyond +/-1 dB. Writes nothing (bwa_calibrate --verify). Run it from the trim run's placement");
    ImGui::SameLine(0, uiScaled(24)); ImGui::TextDisabled("mic:");
    ImGui::SameLine(); if (ImGui::RadioButton("omni##cmic", !J.zylia)) J.zylia = false;
    bwTip("one omnidirectional measurement mic on one input");
    ImGui::SameLine(); if (ImGui::RadioButton("ZM-1##cmic", J.zylia)) J.zylia = true;
    bwTip("the Zylia ZM-1 on 19 consecutive inputs: each capsule deconvolved, the arrivals refined by cross-correlation, "
          "pooled into one omni-like reading (the power mean, the arrival at the array center). bwa_calibrate --zylia");
    ImGui::SetNextItemWidth(-uiScaled(240));
    ImGui::InputText("##cl", J.layout, sizeof J.layout);
    bwTip(J.pass == 1 ? "the layout to verify: the trims it carries are what plays (verify the file a trim run wrote)"
                      : "the surveyed layout the sweep reads speaker geometry from - becomes the A side of the diff");
    ImGui::SameLine(); if (ImGui::Button("...##cpl") && pick_file(J.layout, sizeof J.layout,
                          "layout json (*.json)\0*.json\0all files (*.*)\0*.*\0")) {}
    ImGui::SameLine(); ImGui::TextUnformatted(J.pass == 1 ? "layout to verify" : "layout in");
    ImGui::BeginDisabled(J.pass == 1);
    ImGui::SetNextItemWidth(-uiScaled(240));
    ImGui::InputText("##co", J.out, sizeof J.out);
    bwTip("where the calibrated layout is written (trims + optional eq) - becomes the B side of the diff");
    ImGui::SameLine(); ImGui::TextUnformatted(J.pass == 1 ? "layout out (verify writes nothing)" : "layout out (trims written here)");
    ImGui::EndDisabled();
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
    bwTip(J.zylia ? "the ZM-1's array CENTER in room space - the solve time-aligns and level-matches arrivals at this point. "
                    "Defaults to the layout's listening point, which is where it belongs (the trims align at the mic)"
                  : "omni mic position in room space - the solve time-aligns and level-matches arrivals at this point. "
                    "Defaults to the layout's listening point; set it to where the mic really is");
    if (J.mic_set && J.have_ref) {
        ImGui::SameLine();
        if (ImGui::SmallButton("use listening point")) { memcpy(J.mic, J.mic_ref, sizeof J.mic); J.mic_set = false; }
        bwTip("put the mic field back on the layout's listening point (the directivity re-aim turns off again)");
    }
    ImGui::SameLine(0, uiScaled(16)); ImGui::Checkbox("simulate", &J.simulate);
    bwTip("no hardware: synthesize each sweep capture (1/r + a per-speaker sensitivity wobble; the ZM-1 as 19 "
          "free-field capsules at their own positions) and run the identical measure->solve->writeback pipeline");
    if (J.simulate) { ImGui::SameLine(); ImGui::Checkbox("room##cap", &J.sim_room);
                      bwTip("put the simulated shoebox room around the array (bwa_calibrate --sim-room 0.3)"); }
    ImGui::BeginDisabled(J.pass == 1);                           /* verify writes nothing and keeps nothing */
    ImGui::SameLine(); ImGui::Checkbox("room report", &J.do_room);
    bwTip("Schroeder RT60 + early reflections per speaker - tells you how live the room is. "
          "Treat the room if it's too live; don't copy the RT60 into your engine reverb. "
          "With the ZM-1 it reads the capsules' mean: trust it below about 2 kHz");
    ImGui::BeginDisabled(J.zylia);
    ImGui::SameLine(); ImGui::Checkbox("eq", &J.do_eq);
    ImGui::EndDisabled();
    bwTip(J.zylia ? "refused with the ZM-1, like bwa_calibrate: a correction FIR inverts ONE impulse response, and no "
                    "capsule on the sphere stands for the pressure at its center above about 2 kHz. Use an omni for eq"
                  : "per-speaker correction FIR inverted from the direct-sound window - flattens the SPEAKER, "
                    "not the room (a moving listener can't be room-EQ'd from one point)");
    ImGui::SameLine(); ImGui::Checkbox("save IRs", &J.do_irs);
    bwTip("keep each speaker's impulse response as <prefix>_NN.wav - feeds the IRs tab, and one "
          "capture then serves trims, the room report, and future analysis. With the ZM-1: the capsules' mean");
    if (J.do_irs) {
        ImGui::SameLine(); ImGui::SetNextItemWidth(uiScaled(140));
        ImGui::InputTextWithHint("##cirp", "ir prefix", J.irprefix, sizeof J.irprefix);
    }
    ImGui::EndDisabled();
    if (J.zylia) {
        ImGui::BeginDisabled(pl_live());                         /* tracked: the Placement panel's survey rules */
        ImGui::SetNextItemWidth(-uiScaled(300));
        ImGui::InputTextWithHint("##csurvey", "capsule survey (optional, room axes)", J.survey, sizeof J.survey);
        bwTip("a ROOM-AXES capsule survey (Zylia tab -> Capsule survey) pins the channel order and the orientation. "
              "Optional: the power mean ignores both, and the center arrival moves by about 15 us at most. Empty = the "
              "built-in table. While the Placement panel tracks, its survey applies instead");
        ImGui::SameLine(); if (ImGui::Button("...##csv")) pick_file(J.survey, sizeof J.survey, "survey json (*.json)\0*.json\0all files (*.*)\0*.*\0");
        ImGui::EndDisabled();
        if (pl_live()) { ImGui::SameLine(); ImGui::TextDisabled("tracking: the Placement panel's survey applies"); }
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
        bwTip("ASIO driver name; empty = first driver with an output per speaker + the mic input(s)");
        driver_pick("##cdrvpick", J.driver, sizeof J.driver);
        ImGui::SameLine(); ImGui::SetNextItemWidth(uiScaled(90));
        ImGui::InputInt(J.zylia ? "first ZM-1 input##cap" : "mic input ch", &J.mic_in);
        bwTip(J.zylia ? "the driver input carrying the ZM-1's first capsule; the 19 capsules are consecutive from here "
                        "(Dante Via on the same device as the outputs)"
                      : "the driver INPUT channel the measurement mic is plugged into (0-based)");
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
        ImGui::BeginDisabled(aim_running() || ses_busy());
        const bool run_clicked = ImGui::Button(J.pass == 1 ? "Run verify" : "Run calibration");
        ImGui::EndDisabled();
        if (run_clicked) {
            if (J.th_live) { J.th.join(); J.th_live = false; }   /* reap the previous run */
            snprintf(J.ran_layout, sizeof J.ran_layout, "%s", J.layout);   /* snapshot: this run's paths */
            snprintf(J.ran_out,    sizeof J.ran_out,    "%s", J.out);
            snprintf(J.ran_survey, sizeof J.ran_survey, "%s", J.survey);
            J.ran_zylia = J.zylia; J.ran_pass = J.pass; J.ran_room = J.simulate && J.sim_room;
            J.ran_mic_set = J.mic_set;
            memcpy(J.mic_run, J.mic, sizeof J.mic_run);
            J.ran_tracked = pl_live();                            /* tracking: the measured center replaces the field */
            if (J.ran_tracked) J.ran_mic_set = true;              /* ...and it is a real position (the re-aim may use it) */
            J.cancel.store(false); J.done_count.store(0); J.n.store(0); J.msg[0] = 0;
            J.state.store(1, std::memory_order_release);
            J.th = std::thread(cap_worker); J.th_live = true;
        }
        bwTip(J.pass == 1 ? "sweep every speaker through the layout's output stage -> measure -> residuals, on a worker "
                            "thread; nothing is written"
                          : "sweep every speaker -> measure -> solve trims -> write the layout, on a worker "
                            "thread; the table fills in live as speakers finish");
    } else if (ImGui::Button("Cancel")) J.cancel.store(true);

    if (st == 0) {
        ImGui::TextDisabled(J.pass == 1 ? "plays every speaker through the layout's trims and flags what is off - writes nothing"
                                        : "sweeps every speaker, solves the trims, writes the layout - then diff it right here");
        return;
    }
    const bool vrun = J.ran_pass == 1;

    if (st == 1 && J.phase.load() == 1)
        ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.35f, 1.f), "waiting for the placement gate (the Placement panel above)");
    if (st != 1) place_take_line(J.tk);                      /* the worker is done writing it */
    int dc = J.done_count.load(std::memory_order_acquire);
    int jn = J.n.load();
    int n  = jn > 0 ? jn : BWA_MAX_CHANNELS;
    char ov[64]; snprintf(ov, sizeof ov, "%d / %d speakers", dc, n);
    ImGui::ProgressBar((float)dc / (float)n, ImVec2(-1, 0), ov);
    const ImVec4 red(1.0f, 0.42f, 0.42f, 1.0f), amber(0.95f, 0.8f, 0.35f, 1.f);
    if (st == 3) ImGui::TextColored(red, "FAILED: %s", J.msg);
    if (st == 2) {
        ImGui::TextColored(vrun && J.vs.nflag ? red : ImVec4(0.45f, 0.85f, 0.5f, 1.0f), "%s", J.msg);
        if (vrun) {
            ImGui::Text("arrival spread %.1f us, level spread %.2f dB over %d live speaker(s), median removed",
                        J.vs.arrival_spread_us, J.vs.level_spread_db, J.vs.nlive);
        } else {
            ImGui::SameLine(0, uiScaled(16));
            if (ImGui::Button("Load into Diff (A=input, B=result)")) {
                snprintf(V.pathA, sizeof V.pathA, "%s", J.ran_layout); load_layout(0);   /* the RUN's paths, not the */
                snprintf(V.pathB, sizeof V.pathB, "%s", J.ran_out);    load_layout(1);   /* possibly-edited fields  */
            }
            bwTip("review what calibration wrote (Diff/Trims/EQ tabs) BEFORE trusting it - a swapped "
                  "channel or bad mic placement shows up as an absurd delta");
            ImGui::SameLine();
            if (ImGui::Button("Verify this result")) {
                snprintf(J.layout, sizeof J.layout, "%s", J.ran_out);
                J.pass = 1;
            }
            bwTip("select verify with the layout just written: then Run verify from the SAME placement, right away");
        }
        if (J.run_ref_off_m > 0.05f)
            ImGui::TextColored(amber, vrun ? "the mic is %.2f m from the listening point: this checks the STATIC stage, what a "
                                             "listener at the mic hears before the engine tracks them"
                                           : "the mic is %.2f m from the listening point: delay_ms aligns arrivals at the MIC, the "
                                             "engine assumes the listening point. Re-run with the mic there", J.run_ref_off_m);
        if (J.ran_zylia)
            ImGui::TextDisabled("ZM-1: power mean over 19 capsules, arrival at the array center; capsule table: %s",
                                J.table_src == 3 ? "the panel's body-frame survey, re-aimed by the pose" :
                                J.table_src == 2 ? "the panel's room-axes survey" :
                                J.table_src == 1 ? "the tab's survey" : "built-in");
    }
    if (st != 0 && J.win_desc[0]) {
        ImGui::TextDisabled("arrival window: %s", J.win_desc);
        bwTip("sweep quality (docs/calibration.md): the arrival is searched only where this speaker's sound can arrive;\n"
              "a sweep with a stronger tap elsewhere, or an IR too close to its noise floor (CALIB_SWEEP_MIN_SNR_DB), is\n"
              "re-swept, and a value counts once two sweeps agree within a sample and 0.2 dB. The thresholds are provisional.");
    }
    const int ncol = 8 + (J.ran_zylia ? 1 : 0);                  /* spk, arrival, level, [spread], sweeps, snr, three per pass */
    if (ImGui::BeginTable("capt", ncol, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("spk"); ImGui::TableSetupColumn("arrival (ms)"); ImGui::TableSetupColumn("level");
        if (J.ran_zylia) ImGui::TableSetupColumn("capsule spread (dB)");
        ImGui::TableSetupColumn("sweeps"); ImGui::TableSetupColumn("IR SNR (dB)");
        if (vrun) {
            ImGui::TableSetupColumn("arrival resid (us)"); ImGui::TableSetupColumn("level resid (dB)"); ImGui::TableSetupColumn("flag");
        } else {
            ImGui::TableSetupColumn("rt60 (s)"); ImGui::TableSetupColumn("gain trim (dB)"); ImGui::TableSetupColumn("delay trim (ms)");
        }
        ImGui::TableSetupScrollFreeze(0, 1); ImGui::TableHeadersRow();
        for (int i = 0; i < dc; ++i) {                           /* only published rows */
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%2d%s", i, J.eqlen[i] ? " eq" : "");
            ImGui::TableNextColumn(); ImGui::Text("%8.3f", J.arrival_ms[i]);
            ImGui::TableNextColumn(); ImGui::Text("%7.4f", J.level[i]);
            if (J.ran_zylia) { ImGui::TableNextColumn(); ImGui::Text("%5.1f", J.spread_db[i]); }
            ImGui::TableNextColumn();                            /* 2 is the minimum: two that agree */
            if (J.sweeps[i] > 2) ImGui::TextColored(amber, "%d re-swept", J.sweeps[i]); else ImGui::Text("%d", J.sweeps[i]);
            ImGui::TableNextColumn(); ImGui::Text("%5.1f", J.snr_db[i]);
            if (vrun) {
                const int f = st == 2 ? J.v_flag[i] : 0;
                ImGui::TableNextColumn();
                if (st != 2) ImGui::TextDisabled("...");
                else ImGui::TextColored((f & CALIB_VERIFY_FLAG_ARRIVAL) ? red : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%+8.1f", J.v_arr_us[i]);
                ImGui::TableNextColumn();
                if (st != 2) ImGui::TextDisabled("...");
                else ImGui::TextColored((f & CALIB_VERIFY_FLAG_LEVEL) ? red : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%+6.2f", J.v_lvl_db[i]);
                ImGui::TableNextColumn();
                if (f & CALIB_VERIFY_FLAG_DEAD) ImGui::TextColored(red, "DEAD");
                else if (f) ImGui::TextColored(red, "%s%s", (f & CALIB_VERIFY_FLAG_ARRIVAL) ? "ARRIVAL " : "",
                                               (f & CALIB_VERIFY_FLAG_LEVEL) ? "LEVEL" : "");
                else ImGui::TextDisabled(st == 2 ? "ok" : "...");
            } else {
                ImGui::TableNextColumn(); if (J.rt60[i] > 0.f) ImGui::Text("%5.2f", J.rt60[i]); else ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); if (st == 2) ImGui::Text("%+6.2f", J.gain_db[i]); else ImGui::TextDisabled("...");
                ImGui::TableNextColumn(); if (st == 2) ImGui::Text("%7.3f", J.trim_ms[i]); else ImGui::TextDisabled("...");
            }
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

/* ---- which transient is the clap? (capsule survey) ----
 * The capture trips on ANY transient, so three layers decide whether one is banked:
 *
 * ARM, THEN ACCEPT. A transient banks only inside an armed window, and only the FIRST one in it; one
 * outside it, or a second inside it, is refused. With the tracked clicker the window opens when the
 * clicker reads still at a spot (clicker_latest: 300 ms within 4 mm) and closes when it moves or after
 * ZY_ARM_WINDOW_S; a new one needs the clicker to leave the spot by ZY_ARM_LEAVE_M first. Typed, the
 * window opens on "Arm for the next clap", after an optional lead-in (time to walk to the spot, so
 * footsteps on the way are refused, not banked). 2 s is the hold-then-click rhythm with a margin
 * (the simulated clicker clicks 0.3 s after it arms), and every second of it is exposure: a stray
 * transient landing in it first is banked. The window is judged at the clap's ONSET (the capture's
 * stamp, zy_onset), so a hand that moves right after its click does not close the window under it,
 * however late the UI notices the snapshot.
 *
 * DIRECTION, once a provisional survey exists. From ZY_DIR_MIN_OBS banked claps on (the count the
 * Solve button needs; 6 x 19 arrivals against 57 capsule coordinates), every banked set is solved
 * provisionally, with the claps leave-one-out flags left out, and each new clap's DOA against that
 * table (zylia_doa_caps) must lie within ZY_DIR_TOL_DEG of the clap position's direction from the
 * center. 10 deg is about 5x the worst error a genuine clap carries: a typed position 5 cm off at
 * 1.5 m is 2 deg, the clicker's 5 mm is 0.15 deg, zylia_doa's near-field bias stays under 1 deg, and a
 * provisional table from 6 claps with 1 us of timing noise moves a DOA well under 1 deg. A wrong tip
 * offset (11.6 cm, about 3.3 deg at 2 m) still passes, which is the residual's and leave-one-out's
 * business, not this gate's. An interferer within 10 deg of the clicker (35 cm at 2 m) passes too.
 * A clap refused for its direction hands the window back, so the real click after it can still bank.
 *
 * LEAVE-ONE-OUT on the final solve (zylia_survey_loo): it flags a clap that does not fit the rest and
 * offers to drop it. That is the backstop for an interferer that got past both, for example one timed
 * like a click before the provisional survey existed. */
#define ZY_ARM_WINDOW_S 2.0
#define ZY_ARM_LEAVE_M  0.05f
#define ZY_DIR_MIN_OBS  6
#define ZY_DIR_TOL_DEG  10.0f
enum { ZY_REF_ARM, ZY_REF_SECOND, ZY_REF_DIR, ZY_REF_TAKE, ZY_REF_OTHER, ZY_REF_N };   /* why a clap was refused */
enum { ZY_BANK_REFUSED = 0, ZY_BANK_OK = 1, ZY_BANK_DIR = 2 };

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
    /* per banked clap: the array center it was taken against (the tracked stand's, else the typed one),
     * and in simulate the clap's TRUE position (the tests compare against it) */
    float     surv_center_used[ZYLIA_SURVEY_MAX][3];
    bool      surv_center_tracked[ZYLIA_SURVEY_MAX];
    float     surv_truth[ZYLIA_SURVEY_MAX][3];
    bool      surv_has_truth[ZYLIA_SURVEY_MAX];
    float     surv_take_spread[ZYLIA_SURVEY_MAX];        /* the clicker's tip spread over its window (m), else -1 */
    int       surv_refused;                              /* claps heard but not banked, and why (the last) */
    char      surv_why[256];
    bool      surv_have_q0; float surv_q0[4];            /* the tracked stand's orientation at the first clap */

    /* ---- the tracked clicker (clicker_track.h): the clap position MEASURED at the clap ---- */
    int       clk_mode;                                  /* 0 = typed / speaker, 1 = the tracked clicker */
    char      ck_body[64], ck_server[64], ck_mc[64];
    float     ck_tip[3];                                 /* pivot -> tip, the clicker's body axes (m) */
    bool      ck_sim, ck_sim_ring;
    /* a clap heard, waiting for the clicker poses that cover its stillness window */
    bool      pend_on;
    double    pend_t_det, pend_t_on;
    double    pend_arr[ZYLIA_MICS];
    bool      pend_has_truth; float pend_truth[3];
    bool      sim_click_pending; float sim_click_truth[3];   /* the simulated clicker fired the clap in Z.sim */
    bool      sim_pub_on; double sim_pub_at;                 /* ...and the UI notices it at sim_pub_at (ZY_SIM_HITCH_S) */
    bool      sim_click_interf;                              /* ...and it was the script's interferer, not a click */
    bool      pend_interf;
    bool      surv_interf[ZYLIA_SURVEY_MAX];                 /* simulate: the banked clap was an interferer (tests) */
    int       ck_sim_if_set;                                 /* simulate: 0 none, 1 the refused pair, 2 one that slips */

    /* ---- arm, then accept ---- */
    bool      arm_valid, arm_taken, arm_need_move, arm_still;
    double    arm_t, arm_end;                                /* the window, zy_now */
    double    arm_taken_t;                                   /* the onset of the transient it took */
    float     arm_tip[3];                                    /* the clicker's tip when it armed */
    float     arm_lead_s;                                    /* typed: the window opens this long after Arm */
    int       arm_mode;                                      /* the clk_mode the state belongs to */
    int       ref_kind[ZY_REF_N];                            /* refused claps by kind */
    /* ---- the onset each clap is judged at (zy_onset) ---- */
    bool      onset_force_est;                               /* tests: ignore the stamp, use the old estimate */
    int       onset_last_src;                                /* ZP_ONSET_* the last clap was judged at (NONE = estimate) */
    float     onset_last_est_ms;                             /* the last clap: the estimate minus the stamp (ms) */
    int       onset_n_stamped, onset_n_est;                  /* claps judged at a stamp / at the estimate */
    float     notice_min_ms, notice_max_ms;                  /* stamped claps: the frame that saw them minus the stamp */
    /* ---- the direction check: a provisional survey of what is banked ---- */
    bool      prov_ok; int prov_n, prov_out;                 /* from prov_n claps; prov_out left out by leave-one-out */
    float     prov_caps[ZYLIA_MICS][3]; float prov_spread, prov_resid;
    float     dir_worst_ok_deg, dir_refused_deg;             /* the largest disagreement banked; the last refused */
    /* ---- leave-one-out on the final solve ---- */
    int       loo_n;                                         /* claps checked, 0 = not run */
    int       loo_nflag;
    ZyliaLooObs loo[ZYLIA_SURVEY_MAX];
};
static ZyState Z;
static ClickerTrack CK;
#define ZY_CK_SIM_MOVING_LEG 4          /* the simulated clicker waves through one click on its way to the fifth spot */

/* The survey's clock: the clicker's session clock while the clicker gives the positions (the wall clock live,
 * the script's virtual clock simulated, so the arm, the onset and the poses agree), else the wall clock. Both
 * live clocks are os_monotonic_ns, the base the capture stamps onsets on. */
static double zy_now(void) { return Z.clk_mode ? clicker_now(&CK) : clicker_clock_s(); }

/* simulate: the UI notices a scripted clap this long AFTER the capture would have published it (its post-roll
 * past the onset). A UI hitch, made deterministic: the stamped onset must not care, and the old estimate (the
 * noticing frame less the post-roll) lands this far after the click, past CLICKER_GUARD_S, onto the hand's
 * next move. 80 ms is a stall a busy Windows desktop produces without trying. */
#define ZY_SIM_HITCH_S 0.080

/* Guarded read: a hand-edited or malformed layout must never land 0 here (it would divide by zero
 * in the clap synthesis below). */
static double zy_c(void) { return g_room_sos > 0.0 ? g_room_sos : BWA_SOS_REF_MPS; }

static float zy_az(const float d[3]) { return atan2f(d[0], -d[2]) * 57.29578f; }
static float zy_el(const float d[3]) { return asinf(d[1] > 1.f ? 1.f : (d[1] < -1.f ? -1.f : d[1])) * 57.29578f; }

#define ZY_SIM_DIST 2.0      /* how far a synthetic clap happens from the array center (m) */

/* a clap-like Gaussian click sampled at each capsule's exact fractional arrival time (the synthesis
 * the zylia unit test validates), landed in the shared block exactly like the ASIO side would. `rel` is
 * the clap's TRUE position relative to the TRUE array center (m). */
static void zy_sim_synth(ZpShared* sh, const float rel[3]) {
    const double C = zy_c(), FS = sh->rate, SIGMA = 1.0e-4;     /* == zy_solve's c */
    unsigned int rng = (unsigned int)(sh->seq * 2654435761u + 12345u);
    const double src[3] = { rel[0], rel[1], rel[2] };
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
    /* the onset stamp, in the field the capture callback fills: the session's clock at the clap (the
     * clicker script's virtual clock when it fired this, else the wall clock) */
    sh->onset_s = zy_now();
    sh->onset_src = ZP_ONSET_SIM;
}
static void zy_sim_publish(ZpShared* sh) { sh->blocks++; sh->seq++; }   /* same publish the ASIO side does */
static void zy_sim_clap_at(ZpShared* sh, const float rel[3]) { zy_sim_synth(sh, rel); zy_sim_publish(sh); }
static void zy_sim_clap(ZpShared* sh, const float dir[3]) {
    const float rel[3] = { dir[0] * (float)ZY_SIM_DIST, dir[1] * (float)ZY_SIM_DIST, dir[2] * (float)ZY_SIM_DIST };
    zy_sim_clap_at(sh, rel);
}

/* simulate: where the array REALLY is. The simulated stand's own truth while the Placement panel tracks
 * it (mic_track's script, not placement.c), else the typed center, which simulate takes as exact. */
static void zy_true_center(float c[3]) {
    if (pl_live() && PL.sim) {
        static PlaceSnap s;                                      /* static: two gates */
        s = pl_snap();
        if (s.have_truth) { memcpy(c, s.truth, 3 * sizeof(float)); return; }
    }
    memcpy(c, Z.surv_center, 3 * sizeof(float));
}

static bool zy_clicker_live(void) { return Z.clk_mode && clicker_state(&CK) == CLICKER_LIVE; }

static void zy_refuse(const char* why, int kind = ZY_REF_OTHER) {
    ++Z.surv_refused;
    if (kind >= 0 && kind < ZY_REF_N) ++Z.ref_kind[kind];
    snprintf(Z.surv_why, sizeof Z.surv_why, "clap refused: %s", why);
    fprintf(stderr, "capsule survey: %s\n", Z.surv_why);
}

/* ---- arm, then accept (see ZY_ARM_WINDOW_S) ---- */
static void zy_arm_open(double t_open) {
    Z.arm_valid = true;
    Z.arm_t = t_open;
    Z.arm_end = t_open + ZY_ARM_WINDOW_S;
    Z.arm_taken = false;
}
static void zy_arm_reset(void) { Z.arm_valid = Z.arm_taken = Z.arm_need_move = false; }
static bool zy_arm_open_now(double now) { return Z.arm_valid && now >= Z.arm_t && now < Z.arm_end; }

/* Once per frame. The tracked clicker arms itself: still at a spot opens the window, moving closes it.
 * After a window that TOOK a clap the clicker must also leave the spot (ZY_ARM_LEAVE_M) before the next
 * one opens, so a hand that relaxes after its click cannot re-arm the same spot; after one that took
 * nothing, moving at all re-arms. */
static void zy_arm_update(void) {
    if (Z.arm_mode != Z.clk_mode) { zy_arm_reset(); Z.arm_mode = Z.clk_mode; }
    if (!Z.clk_mode) return;                                     /* typed: Arm opens it, the clock closes it */
    const double now = zy_now();
    float tip[3] = { 0.f, 0.f, 0.f }, sp = -1.f;
    double age = 1e9;
    const bool have = zy_clicker_live() && clicker_latest(&CK, tip, &age, &sp) && age < CLICKER_STALE_S;
    Z.arm_still = have && sp >= 0.f && sp <= CLICKER_STILL_M;
    if (!Z.arm_still && Z.arm_valid && Z.arm_end > now) Z.arm_end = now;   /* moving closes the window, taken or not */
    if (Z.arm_need_move) {
        /* the clap is admitted when the UI sees it, which can be after the window closed, so the leave test
         * also runs while still: a clicker already at its next spot when its last clap was noticed has left */
        const float dx = tip[0] - Z.arm_tip[0], dy = tip[1] - Z.arm_tip[1], dz = tip[2] - Z.arm_tip[2];
        const bool left = have && dx * dx + dy * dy + dz * dz > ZY_ARM_LEAVE_M * ZY_ARM_LEAVE_M;
        if ((!Z.arm_still && !Z.arm_taken) || left) Z.arm_need_move = false;
    }
    if (Z.arm_still && !Z.arm_need_move && !zy_arm_open_now(now)) {
        zy_arm_open(now);
        memcpy(Z.arm_tip, tip, sizeof Z.arm_tip);
        Z.arm_need_move = true;
    }
}

/* A transient with this onset: admitted (the window's first: -1), or the ZY_REF_* it is refused for,
 * with why. */
static int zy_arm_admit(double t_on, char* why, size_t cap) {
    if (!Z.arm_valid || t_on < Z.arm_t || t_on >= Z.arm_end) {
        if (Z.clk_mode) {
            if (!zy_clicker_live())     snprintf(why, cap, "not armed: the clicker is not tracking");
            else if (!Z.arm_still)      snprintf(why, cap, "not armed: the clicker is moving. Hold it still at a spot to arm");
            else if (Z.arm_need_move)   snprintf(why, cap, "not armed: this spot's window is used up or ran out (%.0f s). "
                                                           "Move the clicker to the next spot and hold it still", ZY_ARM_WINDOW_S);
            else                        snprintf(why, cap, "not armed: hold the clicker still at a spot to arm");
        } else {
            if (!Z.arm_valid)           snprintf(why, cap, "not armed: press 'Arm for the next clap' before you clap");
            else if (t_on < Z.arm_t)    snprintf(why, cap, "not armed yet: the window opens %.1f s after this", Z.arm_t - t_on);
            else                        snprintf(why, cap, "not armed: the window closed %.1f s before it (it is open %.0f s)",
                                                 t_on - Z.arm_end, ZY_ARM_WINDOW_S);
        }
        return ZY_REF_ARM;
    }
    if (Z.arm_taken) {
        snprintf(why, cap, "a second transient %.0f ms after the one this armed window took: only the first counts",
                 (t_on - Z.arm_taken_t) * 1e3);
        return ZY_REF_SECOND;
    }
    Z.arm_taken = true;
    Z.arm_taken_t = t_on;
    if (Z.clk_mode) Z.arm_need_move = true;                      /* taken: the clicker must leave this spot first */
    return -1;
}

/* Typed: open the window, after the lead-in. */
static void zy_arm_typed(void) { zy_arm_open(zy_now() + (Z.arm_lead_s > 0.f ? Z.arm_lead_s : 0.f)); }

/* The provisional survey the direction check uses: every banked clap, less those leave-one-out flags. */
static void zy_provisional(void) {
    Z.prov_ok = false; Z.prov_n = Z.prov_out = 0;
    if (Z.surv_n < ZY_DIR_MIN_OBS) return;
    static ZyliaLooObs lo[ZYLIA_SURVEY_MAX];
    static float  src[ZYLIA_SURVEY_MAX][3];
    static double arr[ZYLIA_SURVEY_MAX][ZYLIA_MICS];
    const int nf = zylia_survey_loo(Z.surv_src, Z.surv_arr, Z.surv_n, zy_c(), lo);
    int n = 0;
    for (int k = 0; k < Z.surv_n; ++k) {
        if (nf > 0 && lo[k].flagged) { ++Z.prov_out; continue; }
        memcpy(src[n], Z.surv_src[k], sizeof src[n]);
        memcpy(arr[n], Z.surv_arr[k], sizeof arr[n]);
        ++n;
    }
    Z.prov_spread = 0.f;
    if (n < ZY_DIR_MIN_OBS) return;
    float radius = 0.f;
    if (zylia_survey(src, arr, n, zy_c(), Z.prov_caps, &Z.prov_resid, &radius, &Z.prov_spread)) { Z.prov_ok = true; Z.prov_n = n; }
}

/* Bank one observation: a clap at src_abs (room coordinates) and its 19 arrivals. The array center it
 * is taken against is the Placement panel's MEASURED center while the ZM-1's stand is tracked (the
 * direction gate's window mean, so the stand must be still in position AND orientation), else the typed
 * field. A survey is in room axes for ONE orientation of the array, so a stand that has turned since the
 * first clap is refused too. Returns false (with the reason) when the clap is not banked. */
static int zy_bank(const float src_abs[3], const double arr[ZYLIA_MICS], const float* truth, float take_spread,
                   bool interf = false) {
    char m[256];
    if (Z.surv_n >= ZYLIA_SURVEY_MAX) { snprintf(m, sizeof m, "the survey is full (%d claps)", ZYLIA_SURVEY_MAX); zy_refuse(m); return ZY_BANK_REFUSED; }
    float center[3];
    const bool tracked = pl_live();
    if (tracked) {
        static PlaceSnap s;                                      /* static: two gates */
        s = pl_snap();
        if (!s.have_pose) { zy_refuse("the Placement panel tracks the ZM-1 but has no pose for it (occluded?)"); return ZY_BANK_REFUSED; }
        if (s.gd.state < PLACE_OFF_TARGET) {
            snprintf(m, sizeof m, "the ZM-1's stand is %s: wait until the Placement panel reads it still", place_gate_reason(&s.gd));
            zy_refuse(m); return ZY_BANK_REFUSED;
        }
        if (s.gd.have_q) {
            float deg = 0.f;
            if (Z.surv_have_q0 && place_turn_deg(Z.surv_q0, s.gd.qmean, &deg) && deg > PLACE_TURN_MIN_DEG) {
                snprintf(m, sizeof m, "the ZM-1's stand turned %.2f deg since the first clap (limit %.2f): a survey holds ONE "
                                      "orientation. Clear and clap again", deg, PLACE_TURN_MIN_DEG);
                zy_refuse(m); return ZY_BANK_REFUSED;
            }
        }
        memcpy(center, s.gd.mean, sizeof center);
        if (s.gd.have_q && !Z.surv_have_q0) { memcpy(Z.surv_q0, s.gd.qmean, sizeof Z.surv_q0); Z.surv_have_q0 = true; }
    } else memcpy(center, Z.surv_center, sizeof center);
    float rel[3], d2 = 0.f;
    for (int a = 0; a < 3; ++a) { rel[a] = src_abs[a] - center[a]; d2 += rel[a] * rel[a]; }
    if (!(d2 >= 0.2f * 0.2f && d2 < 100.f * 100.f)) {           /* zylia_survey refuses the whole set over one */
        snprintf(m, sizeof m, "the clap is %.2f m from the array center: it must be 0.2 m or more", sqrtf(d2));
        zy_refuse(m); return ZY_BANK_REFUSED;
    }
    if (Z.prov_ok) {                                             /* the direction check (ZY_DIR_TOL_DEG) */
        float dir[3];
        const float n = sqrtf(d2);
        if (!zylia_doa_caps(Z.prov_caps, arr, dir)) {
            zy_refuse("no direction against the provisional survey", ZY_REF_DIR); return ZY_BANK_DIR;
        }
        float dot = (dir[0] * rel[0] + dir[1] * rel[1] + dir[2] * rel[2]) / n;
        dot = dot > 1.f ? 1.f : (dot < -1.f ? -1.f : dot);
        const float deg = acosf(dot) * 57.29578f;
        if (deg > ZY_DIR_TOL_DEG) {
            const float rd[3] = { rel[0] / n, rel[1] / n, rel[2] / n };
            snprintf(m, sizeof m, "it came from az %+.0f el %+.0f, %.1f deg off the %s (az %+.0f el %+.0f; limit %.0f deg): "
                                  "another sound, not the clap", zy_az(dir), zy_el(dir), deg,
                     Z.clk_mode ? "clicker's tip" : "typed position", zy_az(rd), zy_el(rd), ZY_DIR_TOL_DEG);
            Z.dir_refused_deg = deg;
            zy_refuse(m, ZY_REF_DIR); return ZY_BANK_DIR;
        }
        if (deg > Z.dir_worst_ok_deg) Z.dir_worst_ok_deg = deg;
    }
    const int k = Z.surv_n++;
    memcpy(Z.surv_src[k], rel, sizeof rel);
    memcpy(Z.surv_arr[k], arr, sizeof Z.surv_arr[k]);
    memcpy(Z.surv_center_used[k], center, sizeof center);
    Z.surv_center_tracked[k] = tracked;
    Z.surv_has_truth[k] = truth != NULL;
    if (truth) memcpy(Z.surv_truth[k], truth, sizeof Z.surv_truth[k]);
    Z.surv_take_spread[k] = take_spread;
    Z.surv_interf[k] = interf;
    Z.surv_solved = false;                                       /* new data: the old solve is stale */
    Z.loo_n = 0;
    zy_provisional();
    return ZY_BANK_OK;
}

/* A clap waiting on the clicker: bank it at the clicker's still tip once the poses covering its window
 * are in (clicker_take), refuse it when it was moving or not tracked. force = decide now (a newer clap
 * has arrived). */
static void zy_resolve(bool force) {
    if (!Z.pend_on) return;
    ClickerTake tk;
    const int r = clicker_take(&CK, Z.pend_t_on, &tk);
    if (r == CLICKER_TAKE_PENDING && !force && clicker_now(&CK) - Z.pend_t_det < CLICKER_WAIT_S) return;
    Z.pend_on = false;
    if (r == CLICKER_TAKE_OK) {
        if (zy_bank(tk.tip, Z.pend_arr, Z.pend_has_truth ? Z.pend_truth : NULL, tk.spread_m, Z.pend_interf) == ZY_BANK_DIR &&
            Z.arm_taken && Z.arm_taken_t == Z.pend_t_on)
            Z.arm_taken = false;                                 /* not the clap: the window stays open for it */
    } else zy_refuse(tk.why, ZY_REF_TAKE);
}

/* The onset a clap is judged at. The capture's stamp (zylia_capture.h, "THE ONSET STAMP"), when there is
 * one and it is usable: on a consistent clock it lies before the frame that saw it and, at worst, a few
 * seconds before (a dragged window stalls the UI). Otherwise the old ESTIMATE: this frame less the shell's
 * post-roll (the snapshot is published ZP_SNAP_N - ZP_SNAP_PRE samples after its trigger block), late by
 * an ASIO block plus a UI frame plus any hitch. Simulated snapshots are stamped too and come the same way. */
#define ZY_ONSET_MAX_AGE_S 5.0
static double zy_onset(const ZpShared* sh, double onset_s, int onset_src, double t_det) {
    const double est = t_det - (double)(ZP_SNAP_N - ZP_SNAP_PRE) / sh->rate;
    const bool usable = onset_src != ZP_ONSET_NONE && onset_s <= t_det && onset_s > t_det - ZY_ONSET_MAX_AGE_S;
    if (usable) Z.onset_last_est_ms = (float)((est - onset_s) * 1e3);
    if (!usable || Z.onset_force_est) {
        Z.onset_last_src = ZP_ONSET_NONE;
        ++Z.onset_n_est;
        return est;
    }
    const float notice_ms = (float)((t_det - onset_s) * 1e3);
    if (!Z.onset_n_stamped || notice_ms < Z.notice_min_ms) Z.notice_min_ms = notice_ms;
    if (!Z.onset_n_stamped || notice_ms > Z.notice_max_ms) Z.notice_max_ms = notice_ms;
    Z.onset_last_src = onset_src;
    ++Z.onset_n_stamped;
    return onset_s;
}
static const char* zy_onset_name(int src) {
    switch (src) {
    case ZP_ONSET_DRIVER:   return "stamped: the driver's systemTime";
    case ZP_ONSET_CALLBACK: return "stamped: the capture callback's clock";
    case ZP_ONSET_SIM:      return "stamped: the simulated clock";
    default:                return "ESTIMATED: the frame that saw it, less the post-roll";
    }
}

static void zy_process(ZpShared* sh, float dt) {
    zy_arm_update();
    if (Z.simulate && !Z.live) {
        if (Z.sim_walk && !(zy_clicker_live() && CK.cfg.sim)) {   /* the simulated clicker fires its own */
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
        const double onset_s = sh->onset_s;                      /* written with snap[], before seq */
        const int onset_src = sh->onset_src;
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

            /* the same clap, banked as a survey observation. The arrivals are already in hand: the
             * only extra thing a survey needs is WHERE it came from, which the operator has typed, or
             * which the tracked clicker measures once its poses around the clap are in. */
            if (Z.surv_on) {
                if (Z.clk_mode) zy_resolve(true);                /* a clap still waiting: decide it now (a direction
                                                                  * refusal hands its window back) */
                /* the onset: the snapshot's stamp (zy_onset). The arm and the clicker's window judge the
                 * onset, never the detection. */
                const double t_det = zy_now();
                const double t_on = zy_onset(sh, onset_s, onset_src, t_det);
                char why[256];
                const int rk = zy_arm_admit(t_on, why, sizeof why);
                if (rk >= 0) zy_refuse(why, rk);
                else if (Z.clk_mode) {
                    Z.pend_t_det = t_det;
                    Z.pend_t_on = t_on;
                    memcpy(Z.pend_arr, arr, sizeof arr);
                    Z.pend_has_truth = Z.sim_click_pending;
                    memcpy(Z.pend_truth, Z.sim_click_truth, sizeof Z.pend_truth);
                    Z.pend_interf = Z.sim_click_pending && Z.sim_click_interf;
                    Z.pend_on = true;
                    zy_resolve(false);
                } else if (zy_bank(Z.surv_clap, arr, NULL, -1.f) == ZY_BANK_DIR)
                    Z.arm_taken = false;                         /* not the clap: the window stays open for it */
            }
        } else ++Z.rejects;                                      /* not transient enough / degenerate solve */
        Z.sim_click_pending = false;
        Z.sim_click_interf = false;
    }
    if (Z.pend_on) zy_resolve(false);
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
        /* which clap does not fit the rest? (zylia_survey_loo) */
        Z.loo_nflag = zylia_survey_loo(Z.surv_src, Z.surv_arr, Z.surv_n, zy_c(), Z.loo);
        Z.loo_n = Z.loo_nflag >= 0 ? Z.surv_n : 0;
        if (Z.loo_nflag < 0) Z.loo_nflag = 0;
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

/* Drop every banked clap and the solve built on them. */
static void zy_clear(void) {
    Z.surv_n = 0; Z.surv_solved = false; Z.surv_msg[0] = 0;
    Z.surv_refused = 0; Z.surv_why[0] = 0;
    Z.surv_have_q0 = false;
    Z.pend_on = false; Z.sim_click_pending = false; Z.sim_click_interf = false;
    memset(Z.ref_kind, 0, sizeof Z.ref_kind);
    Z.onset_last_src = ZP_ONSET_NONE; Z.onset_last_est_ms = 0.f;
    Z.onset_n_stamped = Z.onset_n_est = 0;
    Z.notice_min_ms = Z.notice_max_ms = 0.f;
    zy_arm_reset();
    Z.prov_ok = false; Z.prov_n = Z.prov_out = 0;
    Z.dir_worst_ok_deg = Z.dir_refused_deg = 0.f;
    Z.loo_n = Z.loo_nflag = 0;
}

/* Drop banked clap k (leave-one-out flagged it), then solve again. */
static void zy_drop(int k) {
    if (k < 0 || k >= Z.surv_n) return;
    const int tail = Z.surv_n - k - 1;
    memmove(Z.surv_src[k], Z.surv_src[k + 1], (size_t)tail * sizeof Z.surv_src[0]);
    memmove(Z.surv_arr[k], Z.surv_arr[k + 1], (size_t)tail * sizeof Z.surv_arr[0]);
    memmove(Z.surv_center_used[k], Z.surv_center_used[k + 1], (size_t)tail * sizeof Z.surv_center_used[0]);
    memmove(&Z.surv_center_tracked[k], &Z.surv_center_tracked[k + 1], (size_t)tail * sizeof Z.surv_center_tracked[0]);
    memmove(Z.surv_truth[k], Z.surv_truth[k + 1], (size_t)tail * sizeof Z.surv_truth[0]);
    memmove(&Z.surv_has_truth[k], &Z.surv_has_truth[k + 1], (size_t)tail * sizeof Z.surv_has_truth[0]);
    memmove(&Z.surv_take_spread[k], &Z.surv_take_spread[k + 1], (size_t)tail * sizeof Z.surv_take_spread[0]);
    memmove(&Z.surv_interf[k], &Z.surv_interf[k + 1], (size_t)tail * sizeof Z.surv_interf[0]);
    --Z.surv_n;
    Z.surv_solved = false;
    Z.loo_n = Z.loo_nflag = 0;
    zy_provisional();
    zy_solve();
}

/* Drive a whole survey off synthetic claps, end to end through the real path (sim clap -> snapshot ->
 * tdoa -> observation). Hardware-free proof the flow is wired, and what the UI test drives. */
static void zy_sim_survey(void) {
    const int N = 14;
    Z.sim_walk = false;
    Z.surv_on  = true;
    zy_clear();
    Z.surv_installed = false;
    zylia_set_capsules(NULL);                                    /* recover from a clean slate */
    zylia_geometry(Z.dirs, &Z.R);
    Z.surv_center[0] = Z.surv_center[1] = Z.surv_center[2] = 0.0f;
    float tc[3];
    zy_true_center(tc);                                          /* the typed origin, or the tracked stand's truth */
    for (int k = 0; k < N; ++k) {
        double yy = 1.0 - 2.0 * ((double)k + 0.5) / (double)N;   /* Fibonacci: spread, and crucially not
                                                                  * coplanar — it includes high and low */
        double rr = sqrt(fmax(0.0, 1.0 - yy * yy)), th = 2.399963229728653 * (double)k;
        Z.truth[0] = (float)(rr * cos(th)); Z.truth[1] = (float)yy; Z.truth[2] = (float)(rr * sin(th));
        for (int a = 0; a < 3; ++a) Z.surv_clap[a] = tc[a] + (float)ZY_SIM_DIST * Z.truth[a];
        zy_arm_open(zy_now());                                   /* armed for this clap, as the Arm button does */
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
    /* the simulated clicker's virtual clock: one step per frame, before anything reads the clicker, so its
     * poses up to this frame's time are all in before the script may fire a click at it (clicker_track.h) */
    if (CK.cfg.sim) clicker_sim_advance(&CK, ImGui::GetIO().DeltaTime);

    /* -------- source controls -------- */
    if (ImGui::Checkbox("simulate claps", &Z.simulate) && Z.simulate) {
        memset((void*)&Z.sim, 0, sizeof Z.sim);
        Z.sim.nch = ZYLIA_MICS; Z.sim.rate = 48000.0; Z.sim.title = "simulate";
        Z.last_seq = 0; Z.sim_t = 1.0f;
        Z.sim_pub_on = false;
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
    if (!Z.live) { ImGui::BeginDisabled(ses_busy());
                   const bool open_zm1 = ImGui::Button("Open ZM-1");
                   ImGui::EndDisabled();
                   if (open_zm1) { Z.live = zylia_capture_open(Z.driver[0] ? Z.driver : NULL, 48000.0);
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
    /* simulate: the scripted clicker asks for a click; the clap is synthesized from its TRUE tip against the
     * TRUE array center, so a tool that banked anything else lands off the truth */
    if (zy_clicker_live() && CK.cfg.sim && sh == &Z.sim && !Z.sim_pub_on) {
        float tt[3];
        const int ev = clicker_sim_poll_click(&CK, tt);
        if (ev != CLICKER_SIM_EV_NONE) {                         /* a click, or an interferer from elsewhere */
            float c[3], rel[3];
            zy_true_center(c);
            for (int a = 0; a < 3; ++a) rel[a] = tt[a] - c[a];
            zy_sim_synth(&Z.sim, rel);                           /* stamped now, at the script's clock... */
            Z.sim_pub_on = true;                                 /* ...and seen only after the post-roll and a hitch */
            Z.sim_pub_at = Z.sim.onset_s + (double)(ZP_SNAP_N - ZP_SNAP_PRE) / Z.sim.rate + ZY_SIM_HITCH_S;
            Z.sim_click_pending = true;
            Z.sim_click_interf = ev == CLICKER_SIM_EV_INTERF;
            memcpy(Z.sim_click_truth, tt, sizeof tt);
        }
    }
    if (Z.sim_pub_on && (zy_now() >= Z.sim_pub_at || !(zy_clicker_live() && CK.cfg.sim))) {
        Z.sim_pub_on = false;
        zy_sim_publish(&Z.sim);
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
        ImGui::SameLine();
        const int tb = sh->ts_base;
        ImGui::TextDisabled("| block %d, input latency %d%s, systemTime %s", sh->block, sh->in_lat,
                            sh->in_lat ? "" : " (none given)",
                            tb == SINK_TS_HOST ? "on QPC" : tb == SINK_TS_TGT ? "on timeGetTime (unused)"
                            : tb == SINK_TS_UNKNOWN ? "on an unknown base (unused)" : "absent");
        if (tb == SINK_TS_HOST && sh->ts_lag_ms >= 0.f) { ImGui::SameLine(); ImGui::TextDisabled("(callback +%.2f ms after it)", sh->ts_lag_ms); }
        bwTip("what the onset stamp is built from (zylia_capture.h). On QPC the driver's systemTime IS the host clock "
              "and the stamp uses it; on timeGetTime or an unknown base, or absent, the stamp is the host clock read at "
              "callback entry. Record these on rig day (docs/hardware-validation.md)");
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

        /* the array center: the tracked stand's measured one when the Placement panel is live */
        placement_panel(V.hasA ? V.A.ref : NULL, V.hasA ? V.pathA : NULL,
                        J.state.load(std::memory_order_acquire) == 1 || aim_running());
        if (pl_live()) {
            static PlaceSnap ps;                                 /* static: two gates */
            ps = pl_snap();
            const bool still = ps.have_pose && ps.gd.state >= PLACE_OFF_TARGET;
            ImGui::TextColored(still ? ImVec4(0.45f, 0.85f, 0.55f, 1.0f) : ImVec4(0.95f, 0.8f, 0.35f, 1.0f),
                               "array center: TRACKED, (%.4f %.4f %.4f) from the Placement panel%s",
                               ps.gd.mean[0], ps.gd.mean[1], ps.gd.mean[2],
                               still ? "" : ps.have_pose ? "; the stand is not still, so claps are refused" : "; no pose");
            bwTip("while the Placement panel tracks the ZM-1's stand, every clap is taken against its measured center "
                  "(the window mean, still in position and orientation), and the typed field is ignored");
        } else {
            ImGui::SetNextItemWidth(uiScaled(220));
            ImGui::DragFloat3("array center", Z.surv_center, 0.01f, -20.0f, 20.0f, "%.3f");
            bwTip("where the ZM-1 sits, room coords (m). A tape measure is plenty: at 2.5 m a 5 cm error "
                  "tilts a clap direction by ~1 deg, which is already at the timing noise floor. Track the "
                  "stand in the Placement panel above and its measured center replaces this");
        }

        /* where each clap happened: typed (or a speaker), or measured by the tracked clicker */
        ImGui::TextUnformatted("clap positions:");
        ImGui::SameLine(); ImGui::RadioButton("typed##zsrc", &Z.clk_mode, 0);
        bwTip("you say where the next clap happens: a tape-measured position, or a speaker of layout A");
        ImGui::SameLine(); ImGui::RadioButton("tracked clicker##zsrc", &Z.clk_mode, 1);
        bwTip("a hand clicker carrying an OptiTrack rigid body: each clap's position is its tip, measured. Hold it "
              "still, then click");
        if (Z.clk_mode) {
            ImGui::PushID("clicker");
            const int cs = clicker_state(&CK);
            if (CK.th_live && (cs == CLICKER_OFF || cs == CLICKER_FAILED)) clicker_stop(&CK);   /* the thread ended */
            const bool ck_on = cs == CLICKER_CONNECTING || cs == CLICKER_LIVE;
            if (!Z.ck_mc[0]) snprintf(Z.ck_mc, sizeof Z.ck_mc, "239.255.42.99");
            if (!Z.ck_server[0] && PL.server[0]) snprintf(Z.ck_server, sizeof Z.ck_server, "%s", PL.server);
            ImGui::BeginDisabled(ck_on);
            ImGui::SetNextItemWidth(uiScaled(110));
            ImGui::InputTextWithHint("##ckbody", "clicker body", Z.ck_body, sizeof Z.ck_body);
            bwTip("the clicker's rigid body: its streaming id, or its name (a name needs the server)");
            ImGui::SameLine(); ImGui::SetNextItemWidth(uiScaled(120));
            ImGui::InputTextWithHint("##cksrv", "Motive IP", Z.ck_server, sizeof Z.ck_server);
            bwTip("Motive's host (numeric IPv4): for a name, and for the bitstream version. The stand and the clicker "
                  "are two bodies on the same multicast stream; each tracker listens to it on its own");
            ImGui::SameLine(); ImGui::SetNextItemWidth(uiScaled(110));
            ImGui::InputText("##ckmc", Z.ck_mc, sizeof Z.ck_mc);
            bwTip("NatNet multicast group");
            ImGui::SameLine(); ImGui::Checkbox("simulate##ck", &Z.ck_sim);
            bwTip("no Motive: a scripted clicker walks to 10 spots high and low around the array, holds still at each "
                  "and clicks; once on the way it clicks while waving, which must be refused. Needs 'simulate claps'");
            if (Z.ck_sim) {
                ImGui::SameLine(); ImGui::Checkbox("coplanar ring##ck", &Z.ck_sim_ring);
                bwTip("the scripted clicker clicks 8 times in a ring at the array's height instead: the coplanar set "
                      "the survey refuses");
                bool ifr = Z.ck_sim_if_set == 1;
                ImGui::SameLine();
                if (ImGui::Checkbox("interferers##ck", &ifr)) Z.ck_sim_if_set = ifr ? 1 : 0;
                bwTip("two sounds from 60 deg away while the clicker holds still: one right after the second click (a second "
                      "transient in its window), one timed like the eighth click (the direction check refuses it). Both "
                      "must be refused and the survey must come out as if they never happened");
            }
            ImGui::SetNextItemWidth(uiScaled(220));
            ImGui::InputFloat3("tip offset (m)##ck", Z.ck_tip, "%.3f");
            bwTip("from the rigid body's pivot to where the click is made, in the BODY's axes (m). 0 = you moved the "
                  "pivot to the tip in Motive. A wrong tip moves every clap by a different amount, because the clicker "
                  "is held at a different angle each time; the residual shows it");
            if (Z.ck_sim) {
                ImGui::SameLine();
                if (ImGui::SmallButton("the simulated tip##ck")) clicker_sim_tip(Z.ck_tip);
                bwTip("fill in the simulated clicker's true tip offset");
            }
            ImGui::EndDisabled();
            if (!ck_on) {
                if (ImGui::Button("Connect##ck")) {
                    ClickerCfg cc;
                    memset(&cc, 0, sizeof cc);
                    snprintf(cc.body, sizeof cc.body, "%s", Z.ck_body);
                    snprintf(cc.server, sizeof cc.server, "%s", Z.ck_server);
                    snprintf(cc.multicast, sizeof cc.multicast, "%s", Z.ck_mc);
                    memcpy(cc.tip_m, Z.ck_tip, sizeof cc.tip_m);
                    cc.sim = Z.ck_sim ? (Z.ck_sim_ring ? CLICKER_SIM_RING : CLICKER_SIM_SPREAD) : CLICKER_SIM_OFF;
                    cc.sim_moving_leg = Z.ck_sim_ring ? 0 : ZY_CK_SIM_MOVING_LEG;
                    if (Z.ck_sim_if_set == 1) {                  /* refused: by the window, then by direction */
                        cc.sim_nif = 2;
                        cc.sim_if[0] = { 1, CLICKER_SIM_IF_AFTER };
                        cc.sim_if[1] = { 7, CLICKER_SIM_IF_BEFORE };
                    } else if (Z.ck_sim_if_set == 2) {           /* slips: like a click, before a provisional survey */
                        cc.sim_nif = 1;
                        cc.sim_if[0] = { 2, CLICKER_SIM_IF_BEFORE };
                    }
                    if (Z.ck_sim) {
                        zy_true_center(cc.sim_anchor);           /* the script's spots sit around the TRUE center */
                        zylia_set_capsules(NULL);                /* the synthesis is the built-in table */
                        zylia_geometry(Z.dirs, &Z.R);
                        Z.surv_installed = false;
                        Z.sim_walk = false;
                    }
                    clicker_start(&CK, &cc);
                }
            } else if (ImGui::Button("Disconnect##ck")) CK.stop.store(true);   /* never join here: the receive can
                                                                             * hold the thread for 200 ms; the
                                                                             * check above joins once it ends */
            ImGui::SameLine();
            if (cs == CLICKER_CONNECTING) ImGui::TextDisabled("connecting...");
            else if (cs == CLICKER_FAILED) ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f), "FAILED: %s", CK.msg);
            else if (cs == CLICKER_OFF) ImGui::TextDisabled("not tracking: a clap now is refused");
            else {
                float tip[3], sp = -1.f;
                double age = 0.0;
                if (clicker_latest(&CK, tip, &age, &sp) && age < CLICKER_STALE_S) {
                    const bool still = sp >= 0.f && sp <= CLICKER_STILL_M;
                    ImGui::TextColored(still ? ImVec4(0.45f, 0.85f, 0.55f, 1.0f) : ImVec4(0.95f, 0.8f, 0.35f, 1.0f),
                                       "tip (%.3f %.3f %.3f)  %s", tip[0], tip[1], tip[2],
                                       sp < 0.f ? "(filling)" : still ? "STILL: click now" : "moving");
                    if (sp >= 0.f) { ImGui::SameLine(); ImGui::TextDisabled("spread %.1f mm over %.0f ms", sp * 1e3f, CLICKER_WINDOW_S * 1e3); }
                } else ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f), "no live pose (occluded, wrong body, or not streaming)");
                if (CK.cfg.sim) {
                    const int left = clicker_sim_left(&CK);
                    ImGui::SameLine();
                    ImGui::TextDisabled("| SIMULATED, %d click%s to go%s", left, left == 1 ? "" : "s",
                                        Z.simulate && !Z.live ? "" : " (turn on 'simulate claps')");
                }
            }
            ImGui::PopID();
        } else {
            if (V.hasA) {                                        /* clap AT a surveyed speaker: it is a
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
        }

        ImGui::Checkbox("record claps", &Z.surv_on);
        bwTip(Z.clk_mode ? "every accepted clap is banked at the clicker's tip, if the clicker held still before it"
                         : "every accepted clap is banked as an observation at the position above");
        ImGui::SameLine(); ImGui::Text("|  %d clap%s banked, %d refused%s", Z.surv_n, Z.surv_n == 1 ? "" : "s", Z.surv_refused,
                                       Z.pend_on ? ", one waiting for the clicker" : "");
        ImGui::SameLine();
        if (ImGui::Button("Clear")) zy_clear();

        /* arm, then accept: which transient is the clap (ZY_ARM_WINDOW_S) */
        {
            const double now = zy_now();
            const ImVec4 go(0.45f, 0.85f, 0.55f, 1.0f), wait(0.95f, 0.8f, 0.35f, 1.0f);
            if (Z.clk_mode) ImGui::SameLine();
            else {
                if (ImGui::Button("Arm for the next clap")) zy_arm_typed();
                bwTip("only the FIRST transient inside the window that opens now (after the lead-in) is banked; one outside "
                      "it, or a second inside it, is refused. Arm once per clap");
                ImGui::SameLine(); ImGui::SetNextItemWidth(uiScaled(90));
                ImGui::DragFloat("lead-in (s)##zarm", &Z.arm_lead_s, 0.1f, 0.0f, 30.0f, "%.1f");
                bwTip("the window opens this long after Arm: time to walk to the spot, so your footsteps are refused "
                      "rather than banked");
                ImGui::SameLine();
            }
            if (zy_arm_open_now(now) && !Z.arm_taken)
                ImGui::TextColored(go, "ARMED: %s now (%.1f s left)", Z.clk_mode ? "click" : "clap", Z.arm_end - now);
            else if (Z.arm_valid && now < Z.arm_t)
                ImGui::TextColored(wait, "arming in %.1f s", Z.arm_t - now);
            else if (Z.clk_mode)
                ImGui::TextColored(wait, "%s", !zy_clicker_live() ? "not armed: the clicker is not tracking"
                                               : Z.arm_need_move ? "not armed: move to the next spot and hold the clicker still"
                                               : "not armed: hold the clicker still at a spot");
            else ImGui::TextColored(wait, "not armed%s", Z.arm_valid && Z.arm_taken ? ": the window took its clap" : "");
            bwTip("a transient banks only while armed, and only the first one in the window. The tracked clicker arms "
                  "itself when it holds still at a spot and disarms when it moves");
        }
        if (Z.surv_n > 0) {
            const int k = Z.surv_n - 1;
            float d = 0.f;
            for (int a = 0; a < 3; ++a) d += Z.surv_src[k][a] * Z.surv_src[k][a];
            ImGui::TextDisabled("last clap (%.3f %.3f %.3f), %.2f m from the %s center", Z.surv_src[k][0] + Z.surv_center_used[k][0],
                                Z.surv_src[k][1] + Z.surv_center_used[k][1], Z.surv_src[k][2] + Z.surv_center_used[k][2], sqrtf(d),
                                Z.surv_center_tracked[k] ? "tracked" : "typed");
            if (Z.surv_take_spread[k] >= 0.f) { ImGui::SameLine(); ImGui::TextDisabled("; the clicker held still to %.1f mm", Z.surv_take_spread[k] * 1e3f); }
            ImGui::SameLine();
            ImGui::TextDisabled("| onset %s", Z.onset_last_src == ZP_ONSET_NONE ? "ESTIMATED" : "stamped");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("the last transient was judged at the onset %s.\n"
                                  "The estimate would have put it %+.1f ms from the stamp; the frames that saw stamped "
                                  "claps came %.0f to %.0f ms after them (%d stamped, %d estimated)",
                                  zy_onset_name(Z.onset_last_src), Z.onset_last_est_ms, Z.notice_min_ms, Z.notice_max_ms,
                                  Z.onset_n_stamped, Z.onset_n_est);
        }
        if (Z.surv_why[0]) ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.35f, 1.0f), "%s", Z.surv_why);

        if (Z.simulate && !Z.live && !Z.clk_mode) {
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
            /* Every clap taken against the TRACKED stand: save in the stand's BODY frame, so the survey
             * follows the stand through every later move (bwa_calibrate --track, bwa_validate --track,
             * the Aim tab) instead of being right for this one orientation only. The claps were made
             * against center = p + R.offset with the panel's offset, so that offset is the mount's. */
            bool all_tracked = Z.surv_n > 0 && Z.surv_have_q0;
            for (int k = 0; k < Z.surv_n && all_tracked; ++k) all_tracked = Z.surv_center_tracked[k];
            ZyliaMount mount;
            memset(&mount, 0, sizeof mount);
            float body[ZYLIA_MICS][3];
            if (all_tracked) {
                float R[9];
                zylia_quat_to_matrix(Z.surv_q0, R);
                zylia_capsules_rotate(Z.surv_caps, R, 1, body);            /* room -> body: R^T */
                mount.body_frame = 1;
                mount.have_offset = 1;
                memcpy(mount.offset_m, PL.mt.offset, sizeof mount.offset_m);
            }
            if (zylia_survey_save(p, all_tracked ? body : Z.surv_caps, Z.surv_resid, Z.surv_radius, Z.surv_spread,
                                  Z.surv_n, all_tracked ? &mount : NULL, e, sizeof e))
                snprintf(Z.surv_msg, sizeof Z.surv_msg, all_tracked
                         ? "saved to %s in the tracked stand's BODY frame (it follows the stand from here on)"
                         : "saved to %s in room axes (right for THIS mounting only; track the stand to save a body-frame survey)", p);
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
            ZyliaMount m;
            memset(&m, 0, sizeof m);
            if (zylia_survey_load(p, &m, e, sizeof e)) {
                bool ok = true;
                if (m.body_frame) {
                    /* body axes: only meaningful re-aimed at the stand's CURRENT pose */
                    static PlaceSnap s;                                     /* static: two gates */
                    s = pl_live() ? pl_snap() : PlaceSnap{};
                    if (s.have_pose) {
                        float body[ZYLIA_MICS][3], room[ZYLIA_MICS][3], R[9];
                        zylia_capsules(body);
                        zylia_quat_to_matrix(s.q, R);
                        zylia_capsules_rotate(body, R, 0, room);
                        zylia_set_capsules(room);
                    } else {
                        zylia_set_capsules(NULL);
                        ok = false;
                        snprintf(Z.surv_msg, sizeof Z.surv_msg, "%s is a BODY-FRAME survey: connect the Placement panel "
                                 "to the stand first, so it can be re-aimed at the stand's pose", p);
                    }
                }
                if (ok) {
                    zylia_geometry(Z.dirs, &Z.R);
                    Z.surv_installed = true;
                    snprintf(Z.surv_msg, sizeof Z.surv_msg, m.body_frame
                             ? "loaded %s (body frame, re-aimed at the stand's pose now) - it is driving the DOA now"
                             : "loaded %s - it is driving the DOA now", p);
                }
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
        /* leave-one-out: a clap that does not fit the rest, and the offer to drop it */
        if (Z.surv_solved && Z.loo_n == Z.surv_n && Z.loo_n > 0) {
            int worst = -1, unchecked = 0;
            for (int k = 0; k < Z.loo_n; ++k) {
                if (!Z.loo[k].ok) { ++unchecked; continue; }
                if (worst < 0 || Z.loo[k].heldout_us > Z.loo[worst].heldout_us) worst = k;
            }
            if (!Z.loo_nflag && worst >= 0)
                ImGui::TextDisabled("leave-one-out: every clap fits the rest (worst: clap %d, %.2f us held out against the "
                                    "rest's %.2f us)", worst + 1, Z.loo[worst].heldout_us, Z.loo[worst].resid_us);
            bwTip("each clap is held out of the solve and scored against the geometry the others give. One that misfits "
                  "the rest by more than 5x their own residual (and 3 us) does not fit: an interferer, or a wrong position");
            for (int k = 0; k < Z.loo_n; ++k) {
                if (!Z.loo[k].flagged) continue;
                ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.0f),
                                   "clap %d does not fit the rest: held out, it misfits by %.1f us; without it the residual "
                                   "is %.2f us (now %.2f)", k + 1, Z.loo[k].heldout_us, Z.loo[k].resid_us, Z.surv_resid);
                ImGui::SameLine();
                char lbl[32];
                snprintf(lbl, sizeof lbl, "Drop clap %d", k + 1);
                ImGui::BeginDisabled(Z.surv_n - 1 < ZY_DIR_MIN_OBS);
                const bool drop = ImGui::SmallButton(lbl);
                ImGui::EndDisabled();
                bwTip("remove it and solve again. Two bad claps can hide each other: check again after the drop");
                if (drop) { zy_drop(k); break; }
            }
            if (unchecked)
                ImGui::TextDisabled("%d clap%s cannot be left out: without %s the rest is coplanar, so %s unchecked",
                                    unchecked, unchecked == 1 ? "" : "s", unchecked == 1 ? "it" : "one", unchecked == 1 ? "it is" : "they are");
        }
        if (Z.surv_on) {                                         /* the direction check's state */
            if (Z.prov_ok)
                ImGui::TextDisabled("direction check ON: a provisional survey of %d claps (spread %.2f, residual %.2f us)%s; "
                                    "a clap must come from within %.0f deg of its position (worst so far %.1f deg)",
                                    Z.prov_n, Z.prov_spread, Z.prov_resid, Z.prov_out ? ", leaving out claps that do not fit" : "",
                                    ZY_DIR_TOL_DEG, Z.dir_worst_ok_deg);
            else if (Z.surv_n < ZY_DIR_MIN_OBS)
                ImGui::TextDisabled("direction check waits for %d claps (have %d): until then only the arm protects",
                                    ZY_DIR_MIN_OBS, Z.surv_n);
            else
                ImGui::TextDisabled("direction check OFF: the provisional survey does not solve (%d claps fit, spread %.3f)",
                                    Z.surv_n - Z.prov_out, Z.prov_spread);
            bwTip("from the 6th banked clap on, each new clap's arrival direction against a provisional survey of the "
                  "rest must agree with where it was banked. Disagreement means another sound: it is refused");
            if (Z.surv_refused)
                ImGui::TextDisabled("refused: %d not armed, %d second in a window, %d wrong direction, %d clicker, %d other",
                                    Z.ref_kind[ZY_REF_ARM], Z.ref_kind[ZY_REF_SECOND], Z.ref_kind[ZY_REF_DIR],
                                    Z.ref_kind[ZY_REF_TAKE], Z.ref_kind[ZY_REF_OTHER]);
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
 * a second with the live sweep and shows, against layout A's PLAN for it (plan_position / plan_aim, or
 * its position and aim when the file carries no plan): where the box is (zylia_live_position: DOA x the
 * center arrival minus the latency) with the move in room words (place_move_words, the CLI's own),
 * and how far its axis is off the direction to the ZM-1,
 * as a tilt PEAK METER (turn until it peaks: no calibration, and the screen's loss cannot move the
 * peak) and as an estimated MAGNITUDE (the tilt against the 0 deg tilt of a stored reference or of
 * the file's on_axis_db, inverted through the model). One mic position never says which way the box
 * points, and the UI says so. The capture runs on a worker thread through calib_live_read, the SAME
 * reading the CLI takes. Simulate mode synthesizes it with a draggable true position and aim. */
#define AIM_HIST 90

struct AimData {                         /* everything worker and UI share; copied out under the lock */
    float move[3];                       /* simulate: the true position's offset from the AS-BUILT position (m) */
    float turn_deg, tilt_deg;            /* simulate: the true aim, turned about +y then tilted, off the as-built aim */
    float screen_db;                     /* simulate: a screen's HF loss on every path out of the box */
    int   n;                             /* readings published since Start */
    int   dead;                          /* the last reading's dead capsule, -1 = none */
    bool  have_pos; ZyliaLivePos lp;
    bool  have_tilt; float tilt_db, below_db;
    bool  clean;                         /* the last reading passed the window and the SNR floor */
    char  why[200];                      /* ... and why not, when it did not */
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
    bool  have_model, have_file; float tilt0_file, layout_deg;   /* layout_deg: the PLAN aim against the center */
    std::mutex mu;
    AimData d;
};
static AimJob AJ;
static bool aim_running(void) { return AJ.state.load(std::memory_order_acquire) == 1; }
static Layout g_aim_L;                   /* the worker's copy of layout A (never a stack local) */
static char   g_aim_from[512];           /* the layout file g_aim_L came from: what a session reading was against */
static void   ses_aim_accept_ui(void);   /* the Session tab's aim step: accept this reading (below) */
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
        const int r = pl_take(&tk, AJ.stop, 300.0, q, AJ.r_pos_ok);   /* a position readout is a direction mode */
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
        if (AJ.r_pos_ok) {
            mic_track_aim_capsules(&PL.mt, q);
            /* the bump check stops on a turn too, at this speaker's range (placement.h) */
            const float* sp = L->speakers[s].pos;
            const float dx = sp[0] - tk.center[0], dy = sp[1] - tk.center[1], dz = sp[2] - tk.center[2];
            tk.turn_limit_deg = place_turn_limit_deg(tk.tol_m, sqrtf(dx * dx + dy * dy + dz * dz));
            std::lock_guard<std::mutex> lk(AJ.mu); AJ.d.tk = tk;
        }
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
        /* the expected-arrival window (calib.h): the latency the run knows (the simulator's own, or the
         * typed one), else the driver's loop; the PLAN margin, since the box is being moved */
        CalibWindow aw;
        const int have_aw = calib_window_prior(AJ.r_sim, AJ.r_lat_known ? AJ.r_latency_s : -1.0, CALIB_WIN_PLAN_M, &aw, NULL, 0);
        if (!calib_live_read(s, L, sim_truth ? sim_at : AJ.r_center, C, AJ.r_sim, &o, lsweep, cap19, &rd,
                             have_aw ? &aw : NULL, AJ.r_center)) {
#ifdef BWA_HAVE_ASIO
            if (asio_up) calib_asio_close();
#endif
            if (AJ.r_sim) calib_sim_set_room(0.f);
            aim_fail("capture timed out (speaker not wired? see console)");
            return;
        }
        ZyliaLivePos lp;
        const bool have_pos = rd.ok && AJ.r_pos_ok &&
                              zylia_live_position(rd.arr, AJ.r_center, AJ.r_lat_known, AJ.r_latency_s, C, layout_plan_pos(L, (uint32_t)s), &lp);
        ++nread;
        const bool bumped = AJ.r_tracked && pl_after_capture(&tk, nread, CAL_LIVE_CAPLEN / CAL_FS, sim_truth ? sim_at : NULL);
        {
            std::lock_guard<std::mutex> lk(AJ.mu);
            AimData& d = AJ.d;
            d.dead = rd.ok ? -1 : rd.dead;
            d.have_pos = have_pos;
            if (have_pos) d.lp = lp;
            d.have_tilt = rd.ok && rd.have_tilt;
            d.clean = rd.ok && rd.quality == CALIB_SWEEP_OK;
            snprintf(d.why, sizeof d.why, "%s", rd.ok ? rd.why : "");
            if (d.have_tilt) {
                d.tilt_db  = rd.tilt_db;
                /* the peak holds clean readings only, once two in a row agree (calib_peak_update) */
                d.below_db = calib_peak_update(&d.pk, rd.tilt_db, d.clean ? 1 : 0, CALIB_LIVE_TILT_TOL_DB);
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
            if (tk.turned)
                snprintf(m, sizeof m, "BUMP: the ZM-1 turned %.2f deg after reading %d (limit %.2f deg): re-place it and start again",
                         tk.turn_deg, nread, tk.turn_limit_deg);
            else
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
    snprintf(g_aim_from, sizeof g_aim_from, "%s", V.pathA);
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
    AJ.layout_deg = directivity_off_axis_deg(layout_plan_pos(&g_aim_L, (uint32_t)AJ.spk), layout_plan_aim(&g_aim_L, (uint32_t)AJ.spk), AJ.center);
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
    const float* sp = layout_plan_pos(&V.A, (uint32_t)AJ.spk);
    ImGui::TextDisabled("%s (%.2f %.2f %.2f)", V.A.speakers[AJ.spk].has_plan ? "plan at" : "at", sp[0], sp[1], sp[2]);
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
    {   /* what the readout aims at: the plan when the file keeps one (docs/layout-schema.md) */
        const Speaker& ps = V.A.speakers[AJ.spk];
        if (ps.has_plan) {
            const float e[3] = { ps.pos[0] - ps.plan_pos[0], ps.pos[1] - ps.plan_pos[1], ps.pos[2] - ps.plan_pos[2] };
            char words[160];
            place_move_words(e, PLACE_MOVE_DEAD_BOX_M, words, sizeof words);
            float c = ps.aim[0] * ps.plan_aim[0] + ps.aim[1] * ps.plan_aim[1] + ps.aim[2] * ps.plan_aim[2];
            c = c > 1.f ? 1.f : (c < -1.f ? -1.f : c);
            ImGui::Text("against its PLAN (plan_position, plan_aim). As-built is %.0f mm from it (%s), aim %.1f deg off",
                        sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]) * 1e3f, words, acosf(c) * 57.2957795f);
        } else
            ImGui::TextDisabled("against layout A's position and aim: the file carries no plan for this speaker");
    }
    placement_panel(V.A.ref, V.pathA, running || J.state.load(std::memory_order_acquire) == 1);
    if (pl_live() && !running)
        ImGui::TextDisabled("tracking is live: Start waits for the placement gate and takes the measured center%s",
                            PL.body_frame ? "" : "; no body-frame survey, so no position readout (the tilt meter works)");

    const bool capture_busy = J.state.load(std::memory_order_acquire) == 1;   /* one sweep shell, one simulator */
    if (!running) {
        ImGui::BeginDisabled(capture_busy || ses_busy());
        if (ImGui::Button("Start##aim")) aim_start();
        ImGui::EndDisabled();
        bwTip(capture_busy ? "the Capture tab is running a calibration"
                           : ses_busy() ? "a Session step is running a tool, which holds the ASIO device"
                                        : "sweep the speaker over and over (about one reading a second on the rig)");
    } else if (ImGui::Button("Stop##aim")) AJ.stop.store(true);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", st == 3 ? AJ.msg : (running ? "running" : (st == 2 ? "stopped" : "idle")));
    if (st == 3) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f), "FAILED"); }
    ses_aim_accept_ui();

    if (AJ.simulate) {                                          /* the simulated truth, live */
        std::lock_guard<std::mutex> lk(AJ.mu);
        ImGui::SetNextItemWidth(uiScaled(220));
        ImGui::DragFloat3("true offset (m)##aim", AJ.d.move, 0.002f, -0.5f, 0.5f, "%.3f");
        bwTip("where the simulated box really is, relative to its as-built position (the layout's `position`)");
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
        const bool at_peak = d.have_tilt && d.pk.peak_index && d.below_db < 0.1f;
        ImGui::PushFont(NULL, fs0 * 4.0f);
        if (d.have_tilt) ImGui::TextColored(at_peak ? ImVec4(0.45f, 0.9f, 0.5f, 1.f) : ImVec4(0.95f, 0.8f, 0.35f, 1.f),
                                            "%.1f dB", d.below_db);
        else             ImGui::TextDisabled("-- dB");
        ImGui::PopFont();
        ImGui::TextUnformatted(!d.pk.peak_index ? "no peak yet: hold the box still for two readings to set one"
                               : at_peak ? "AT PEAK: the treble is as high as it has been" : "below the peak: turn the box until this reads 0");

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
        if (d.have_tilt && d.pk.peak_index) ImGui::Text("tilt %+.2f dB   peak %+.2f dB (reading %d of %d)", d.tilt_db, d.pk.peak_db, d.pk.peak_index, d.pk.n);
        else if (d.have_tilt) ImGui::Text("tilt %+.2f dB   no peak yet (two clean readings in a row that agree set it)", d.tilt_db);
        bwTip("the peak holds only CLEAN readings (the arrival inside its expected window, the IR well over its noise\n"
              "floor), and only once two in a row agree within the meter's tolerance, so one cough cannot set it");
        if (d.n && !d.clean && d.dead < 0)
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.3f, 1.f), "last reading NOT HELD: %s", d.why);
        if (d.pk.nrejected) ImGui::TextDisabled("%d reading(s) not held since Start", d.pk.nrejected);
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
        ImGui::Text("%s expects:  %.1f deg", g_aim_L.speakers[AJ.r_spk].has_plan ? "plan" : "layout",
                    d.tk.used ? directivity_off_axis_deg(layout_plan_pos(&g_aim_L, (uint32_t)AJ.r_spk),
                                                         layout_plan_aim(&g_aim_L, (uint32_t)AJ.r_spk), d.tk.center)
                              : AJ.layout_deg);
        if (AJ.simulate && d.n) ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.55f, 1.f), "truth (simulate): %.1f deg", d.true_deg);
        ImGui::PopFont();
        ImGui::TextWrapped("The angle is a MAGNITUDE: how far the box's axis is off the line to the ZM-1, never which way "
                           "it points. Near 0 deg the model's curve is flat, so a box within the 'under N deg' bracket "
                           "reads as on axis. Use the peak meter to aim; the angle says how far there is to go.");
        if (!AJ.have_model) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.f), "layout A has no directivity model: the meter works, the angle does not");

        /* the position */
        ImGui::Separator();
        const bool plan = g_aim_L.speakers[AJ.r_spk].has_plan != 0;   /* the run's target: its plan, or the layout's */
        if (d.have_pos && d.lp.have_distance) {
            ImGui::Text("position: %+.0f %+.0f %+.0f mm off the %s (|d| %.0f mm)\ndirection %.2f deg off, distance %.3f m (%+.0f mm)",
                        d.lp.delta_mm[0], d.lp.delta_mm[1], d.lp.delta_mm[2], plan ? "plan" : "layout", d.lp.delta_norm_mm,
                        d.lp.dir_err_deg, d.lp.dist_m, d.lp.dist_err_mm);
            /* the move in room words: the CLI's function, so the tab and the console say the same thing */
            const float e[3] = { d.lp.delta_mm[0] * 1e-3f, d.lp.delta_mm[1] * 1e-3f, d.lp.delta_mm[2] * 1e-3f };
            char words[160];
            place_move_words(e, PLACE_MOVE_DEAD_BOX_M, words, sizeof words);
            ImGui::PushFont(NULL, ImGui::GetStyle().FontSizeBase * 1.3f);
            ImGui::TextUnformatted(words);
            ImGui::PopFont();
        } else if (d.have_pos)
            ImGui::Text("direction %.2f deg off the %s (no latency: no distance)", d.lp.dir_err_deg, plan ? "plan" : "layout");
        else if (AJ.r_tracked && !AJ.r_pos_ok)
            ImGui::TextDisabled("position: off (tracked with no body-frame survey: a direction needs the array's orientation)");
        else
            ImGui::TextDisabled("position: -");
        if (d.tk.used && d.tk.turn_limit_deg > 0.f)    /* a direction run: how far the stand turned since the take */
            ImGui::Text("stand turned %.2f deg since the take (limit %.2f deg)", d.tk.last_turn_deg, d.tk.turn_limit_deg);
    }
    ImGui::EndChild();
    ImGui::SameLine();

    if (ImPlot3D::BeginPlot("##aim3d", ImGui::GetContentRegionAvail(), ImPlot3DFlags_NoPan)) {
        ImPlot3D::SetupAxes("x (m)", "z (m)", "y up (m)", ImPlot3DAxisFlags_AutoFit, ImPlot3DAxisFlags_AutoFit, ImPlot3DAxisFlags_AutoFit);
        const Layout& L = V.A;
        if (V.hasA) ImPlot3D::PlotScatter("speakers", V.ax, V.ay, V.az, (int)L.count,
                                          ImPlot3DSpec(ImPlot3DProp_MarkerSize, 3.0f, ImPlot3DProp_MarkerFillColor, IM_COL32(120, 120, 140, 160)));
        const int s = AJ.spk;
        const bool plan = L.speakers[s].has_plan != 0;
        const float* p = layout_plan_pos(&L, (uint32_t)s); const float* a = layout_plan_aim(&L, (uint32_t)s);
        float lx[2] = { p[0], p[0] + 0.4f * a[0] }, ly[2] = { p[2], p[2] + 0.4f * a[2] }, lz[2] = { p[1], p[1] + 0.4f * a[1] };
        ImPlot3D::PlotScatter(plan ? "plan" : "layout", &lx[0], &ly[0], &lz[0], 1, ImPlot3DSpec(ImPlot3DProp_MarkerSize, 7.0f));
        ImPlot3D::PlotLine(plan ? "plan aim" : "layout aim", lx, ly, lz, 2);
        if (plan) {                                              /* where the last survey put it */
            const float* bp = L.speakers[s].pos; const float* ba = L.speakers[s].aim;
            float bx[2] = { bp[0], bp[0] + 0.4f * ba[0] }, by[2] = { bp[2], bp[2] + 0.4f * ba[2] }, bz[2] = { bp[1], bp[1] + 0.4f * ba[1] };
            ImPlot3D::PlotScatter("as-built", &bx[0], &by[0], &bz[0], 1, ImPlot3DSpec(ImPlot3DProp_MarkerSize, 5.0f));
            ImPlot3D::PlotLine("as-built aim", bx, by, bz, 2);
        }
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

/* ============ Session tab - the rig-day calibration as one guided, resumable session ============
 * calib_session.h is the model: the steps, their file chain, the session file, the runner. This tab
 * draws it, starts each step, and collects a finished one on the UI thread (ses_poll, every frame),
 * so the Session is only ever touched here. Every step but aim runs the tested tool as a subprocess
 * (bwa_speaker_survey, bwa_calibrate, bwa_validate) in the session folder, with its output streamed
 * into the log and its exit code as the result. The aim step is the Aim tab, handed the session's
 * files: live aiming is a person turning a box against a meter, and the CLI's keys read a console a
 * subprocess does not have. Unverified on hardware, all of it. */
static Session   SES;
static SesRunner SR;
static int       g_ses_step = -1;                  /* the step the runner is running */
static bool      g_ses_dirty;                      /* an input changed: save at the end of the frame */
static bool      g_ses_aim_tab;                    /* switch to the Aim tab on the next frame */
static int       g_ses_aim_cur = -1;               /* the speaker the session handed the Aim tab */
static size_t    g_ses_shown;                      /* log lines shown: scroll to the bottom on news */
static char      g_ses_msg[600];
static char      g_ses_note[200];                  /* the note a skip records */
struct SesUi {
    char dir[512], plan[512], driver[128], server[64], multicast[64], body[64], offset[64], temp[32];
    char lrows[512], grows[512], fspk[128], cspk[128], aspk[128], vpos[512], truth[512];
};
static SesUi SU;

static bool ses_busy(void) { return SR.state.load(std::memory_order_acquire) != 0; }

/* a tool beside this exe, or where ctest's environment says */
static std::string find_tool(const char* env, const char* exe) {
    const char* e = getenv(env);
    if (e && *e && GetFileAttributesA(e) != INVALID_FILE_ATTRIBUTES) return ses_abs(e);
    char self[MAX_PATH];
    const DWORD k = GetModuleFileNameA(NULL, self, MAX_PATH);
    if (!k || k >= MAX_PATH) return std::string();
    char* sl = strrchr(self, '\\');
    if (!sl) return std::string();
    sl[1] = 0;
    const std::string p = std::string(self) + exe;
    return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES ? p : std::string();
}
static SesTools ses_tools(void) {
    SesTools T;
    T.calibrate = find_tool("BWA_CALIBRATE_EXE", "bwa_calibrate.exe");
    T.validate  = find_tool("BWA_VALIDATE_EXE", "bwa_validate.exe");
    T.survey    = find_tool("BWA_SPEAKER_SURVEY_EXE", "bwa_speaker_survey.exe");
    return T;
}

static std::string ses_logpath(void) { return ses_join(SES.folder, "session.log"); }
static void ses_say(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_ses_msg, sizeof g_ses_msg, fmt, ap);
    va_end(ap);
}
static void ses_save_now(void) {
    std::string e;
    if (!SES.folder.empty() && !ses_save(SES, &e)) ses_say("%s", e.c_str());
    g_ses_dirty = false;
}

static void ses_ui_from(void) {
    const SesInputs& I = SES.in;
    snprintf(SU.plan, sizeof SU.plan, "%s", I.plan.c_str());
    snprintf(SU.driver, sizeof SU.driver, "%s", I.driver.c_str());
    snprintf(SU.server, sizeof SU.server, "%s", I.nn_server.c_str());
    snprintf(SU.multicast, sizeof SU.multicast, "%s", I.nn_multicast.c_str());
    snprintf(SU.body, sizeof SU.body, "%s", I.body.c_str());
    snprintf(SU.offset, sizeof SU.offset, "%s", I.mount_offset.c_str());
    snprintf(SU.temp, sizeof SU.temp, "%s", I.temp.c_str());
    snprintf(SU.lrows, sizeof SU.lrows, "%s", I.localize_rows.c_str());
    snprintf(SU.grows, sizeof SU.grows, "%s", I.grid_rows.c_str());
    snprintf(SU.fspk, sizeof SU.fspk, "%s", I.frame_speakers.c_str());
    snprintf(SU.cspk, sizeof SU.cspk, "%s", I.capsule_speakers.c_str());
    snprintf(SU.aspk, sizeof SU.aspk, "%s", I.aim_speakers.c_str());
    snprintf(SU.vpos, sizeof SU.vpos, "%s", I.validate_positions.c_str());
    snprintf(SU.truth, sizeof SU.truth, "%s", I.sim_truth.c_str());
}
static void ses_ui_to(void) {
    SesInputs& I = SES.in;
    I.plan = SU.plan; I.driver = SU.driver; I.nn_server = SU.server; I.nn_multicast = SU.multicast;
    I.body = SU.body; I.mount_offset = SU.offset; I.temp = SU.temp;
    I.localize_rows = SU.lrows; I.grid_rows = SU.grows; I.frame_speakers = SU.fspk;
    I.capsule_speakers = SU.cspk; I.aim_speakers = SU.aspk; I.validate_positions = SU.vpos; I.sim_truth = SU.truth;
}

static void ses_open(bool make) {
    if (ses_busy()) { ses_say("a step is running: cancel it first"); return; }
    const std::string dir = ses_abs(SU.dir);
    if (dir.empty()) { ses_say("type the session folder"); return; }
    std::string e;
    Session N;
    if (ses_load(&N, dir, &e)) {
        if (make) { ses_say("%s already holds a session: Open it", dir.c_str()); return; }
        SES = N;
        ses_ui_from();
        ses_log_reload(&SR, ses_logpath());
        ses_say("opened %s", dir.c_str());
        ses_log(&SR, ses_logpath(), "=== " + ses_now() + ": session opened");
        return;
    }
    if (!make) { ses_say("%s", e.c_str()); return; }
    if (!CreateDirectoryA(dir.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        ses_say("cannot make the folder %s", dir.c_str()); return; }
    const bool sim = SES.simulate;
    SES = Session();                     /* the inputs typed so far carry over into the new session */
    SES.folder = dir;
    SES.created = ses_now();
    SES.simulate = sim;
    ses_ui_to();
    ses_save_now();
    ses_log_reload(&SR, ses_logpath());
    ses_log(&SR, ses_logpath(), "=== " + ses_now() + ": session created in " + dir);
    ses_say("made a new session in %s", dir.c_str());
}

/* who holds the audio device in this window, if anyone */
static const char* ses_device_holder(void) {
    if (J.state.load(std::memory_order_acquire) == 1) return "the Capture tab is running";
    if (aim_running()) return "the Aim tab is running: Stop it";
    if (Z.live) return "the Zylia tab has the ZM-1 open: Close it";
    return NULL;
}

static float ses_sos(const std::string& layout) {
    double c = BWA_SOS_REF_MPS;
    if (!calib_read_sos(layout.c_str(), &c)) c = BWA_SOS_REF_MPS;
    return (float)c;
}

/* hand the Aim tab one of the session's speakers: layout A = as_built.json, the capsule survey and the
 * latency from the capsule step, and (tracked) the Placement panel set up from the session's inputs */
static void ses_aim_hand(int spk) {
    const std::string ab = ses_join(SES.folder, "as_built.json"), cs = ses_join(SES.folder, "capsules.json");
    snprintf(V.pathA, sizeof V.pathA, "%s", ab.c_str());
    load_layout(0);
    if (!V.hasA) { ses_say("aim: as_built.json does not load: %s", V.status); return; }
    AJ.spk = spk;
    AJ.simulate = SES.simulate;
    AJ.center_set = false;
    AJ.center_for[0] = 0;                /* the center follows the new layout A's listening point */
    /* The latency: the localize run's median, ahead of the capsule survey's. Each localize solve sees the
     * speaker from five spread rows; the survey trilaterates one placement against speakers that sit at
     * nearly one distance (a dome around the listening point), where its linear solve barely constrains
     * the latency: 3.5 mm of position error moved it 41 mm in simulation while the center held. */
    const SesStep& C = SES.step[SES_CAPSULES];
    const SesStep& Lz = SES.step[SES_LOCALIZE];
    const double lat_m = Lz.have_latency ? Lz.latency_m : C.latency_m;
    if (Lz.have_latency || C.have_latency) { AJ.latency_ms = (float)(lat_m / ses_sos(ab) * 1e3); AJ.latency_set = true; }
    const bool trk = SES.simulate || !SES.in.body.empty();
    if (!trk && C.have_center) { memcpy(AJ.center, C.center, sizeof AJ.center); AJ.center_set = true; }
    if (trk) {
        const bool want_sim = SES.simulate;
        const bool same = pl_live() && PL.sim == want_sim && !strcmp(PL.survey, cs.c_str()) &&
                          (want_sim || (!strcmp(PL.body, SES.in.body.c_str()) && !strcmp(PL.server, SES.in.nn_server.c_str())));
        if (!same) {
            if (PL.state.load() == 1 || PL.state.load() == 2) PL.stop.store(true);
            if (PL.th_live) { PL.th.join(); PL.th_live = false; }
            snprintf(PL.body, sizeof PL.body, "%s", SES.in.body.c_str());
            snprintf(PL.server, sizeof PL.server, "%s", SES.in.nn_server.c_str());
            if (!SES.in.nn_multicast.empty()) snprintf(PL.multicast, sizeof PL.multicast, "%s", SES.in.nn_multicast.c_str());
            snprintf(PL.survey, sizeof PL.survey, "%s", cs.c_str());   /* body-frame: it carries the mount offset */
            PL.sim = want_sim; PL.sim_bump = PL.sim_twist = false;
            PL.offset_mode = 0; PL.offset[0] = PL.offset[1] = PL.offset[2] = 0.f;
            { std::lock_guard<std::mutex> lk(PL.mu); memcpy(PL.target, V.A.ref, sizeof PL.target); PL.target_set = false; }
            snprintf(PL.target_for, sizeof PL.target_for, "%s", V.pathA);
            pl_connect();
        }
    }
    g_ses_aim_cur = spk;
    g_ses_aim_tab = true;
    ses_say("aim: speaker %d is in the Aim tab%s: Start, turn the box, then Accept for the session", spk,
            trk ? " with the Placement panel tracking" : "");
}

/* the aim list as typed (every frame: no file read); the speaker count is checked when the step runs */
static std::vector<int> ses_aim_list(void) {
    int v[BWA_MAX_CHANNELS];
    const int k = ses_parse_speakers(SES.in.aim_speakers, BWA_MAX_CHANNELS, v, BWA_MAX_CHANNELS);
    return k > 0 ? std::vector<int>(v, v + k) : std::vector<int>();
}

static void ses_run_step(int k) {
    g_ses_msg[0] = 0;
    if (SES.folder.empty()) { ses_say("open a session first"); return; }
    if (ses_busy()) { ses_say("%s is running: wait for it, or Cancel it", ses_step_id(g_ses_step)); return; }
    const char* holder = ses_device_holder();
    const std::string blocked = ses_blocked(SES, k);
    if (!blocked.empty() || holder) {
        const std::string why = holder ? holder : blocked;
        ses_say("%s: refused: %s", ses_step_id(k), why.c_str());
        ses_log(&SR, ses_logpath(), "=== " + ses_now() + ": " + ses_step_id(k) + " refused: " + why);
        return;
    }
    if (k == SES_AIM) {
        const std::vector<int> spk = ses_aim_list();
        if (spk.empty()) { ses_say("aim: refused: the aim speakers are not a list like 3,7 or 0-4"); return; }
        float ref[3];
        int cnt = 0;
        if (!ses_layout_ref(ses_join(SES.folder, "as_built.json"), ref, &cnt)) { ses_say("aim: refused: as_built.json does not load"); return; }
        for (int v : spk)
            if (v >= cnt) { ses_say("aim: refused: speaker %d is not in as_built.json (%d speakers)", v, cnt); return; }
        /* already aiming: hand over the next speaker without a reading, and keep the accepted ones */
        if (SES.step[SES_AIM].status == SES_RUNNING) {
            for (int v : spk) {
                bool have = false;
                for (const SesAimRec& x : SES.step[SES_AIM].aim) have |= x.spk == v;
                if (!have) { ses_aim_hand(v); return; }
            }
        }
        std::vector<SesFile> cons;
        cons.push_back({ "localize", "as_built.json", ses_hash_file(ses_join(SES.folder, "as_built.json")), SES.step[SES_LOCALIZE].run });
        cons.push_back({ "capsules", "capsules.json", ses_hash_file(ses_join(SES.folder, "capsules.json")), SES.step[SES_CAPSULES].run });
        ses_begin(&SES, SES_AIM, "the Aim tab, in this window", cons);
        ses_save_now();
        ses_log(&SR, ses_logpath(), "=== " + ses_now() + ": aim run " + std::to_string(SES.step[SES_AIM].run) +
                                    ": the Aim tab, speakers " + SES.in.aim_speakers);
        ses_aim_hand(spk[0]);
        return;
    }
    std::string cmd, why;
    std::vector<SesFile> cons;
    if (!ses_command(SES, k, ses_tools(), &cmd, &cons, &why)) {
        ses_say("%s: refused: %s", ses_step_id(k), why.c_str());
        ses_log(&SR, ses_logpath(), std::string("=== ") + ses_now() + ": " + ses_step_id(k) + " refused: " + why);
        return;
    }
    /* the step's old file goes first: a run that fails must not leave the last run's file looking current */
    if (ses_step_artifact(k)[0]) DeleteFileA(ses_join(SES.folder, ses_step_artifact(k)).c_str());
    ses_begin(&SES, k, cmd, cons);
    ses_save_now();
    ses_log(&SR, ses_logpath(), "=== " + ses_now() + ": " + ses_step_id(k) + " run " + std::to_string(SES.step[k].run) + ": " + cmd);
    if (!ses_run_start(&SR, cmd, SES.folder, ses_logpath(), &why)) {
        ses_finish(&SES, k, 1, std::string(), false);
        SES.step[k].note = why;
        ses_save_now();
        ses_say("%s: did not start: %s", ses_step_id(k), why.c_str());
        return;
    }
    g_ses_step = k;
    ses_say("%s: running", ses_step_id(k));
}

/* every frame, whatever tab is showing: collect a finished step on the UI thread */
static void ses_poll(void) {
    if (SR.state.load(std::memory_order_acquire) != 2) return;
    ses_run_join(&SR);
    const int k = g_ses_step;
    g_ses_step = -1;
    if (k < 0) return;
    std::string out;
    { std::lock_guard<std::mutex> lk(SR.mu); out = SR.output; }
    const bool canceled = SR.canceled.load();
    ses_finish(&SES, k, SR.exit_code, out, canceled);
    const SesStep& P = SES.step[k];
    char b[700];
    snprintf(b, sizeof b, "=== %s: %s %s (exit %d%s%s)%s%s", ses_now().c_str(), ses_step_id(k), ses_status_name(P.status),
             P.exit_code, P.note.empty() ? "" : ": ", P.note.c_str(), P.summary.empty() ? "" : ": ", P.summary.c_str());
    ses_log(&SR, ses_logpath(), b);
    /* the room's background this step read, against the last step that read one */
    { const std::string bl = ses_background_line(SES, k); if (!bl.empty()) ses_log(&SR, ses_logpath(), "=== " + bl); }
    ses_save_now();
    ses_say("%s %s%s%s", ses_step_id(k), ses_status_name(P.status), P.note.empty() ? "" : ": ", P.note.c_str());
}

/* the Aim tab's half of the aim step: accept the current reading for the session */
static void ses_aim_accept_ui(void) {
    SesStep& P = SES.step[SES_AIM];
    if (SES.folder.empty() || P.status != SES_RUNNING) return;
    const std::vector<int> list = ses_aim_list();
    bool listed = false;
    for (int v : list) listed |= v == AJ.r_spk;
    AimData d;
    { std::lock_guard<std::mutex> lk(AJ.mu); d = AJ.d; }
    const std::string ab = ses_join(SES.folder, "as_built.json");
    const bool against = !_stricmp(ses_abs(g_aim_from).c_str(), ab.c_str());   /* the run read the session's file */
    const bool ok = listed && against && d.n > 0 && d.have_tilt;
    ImGui::SameLine(0, uiScaled(24));
    ImGui::BeginDisabled(!ok);
    const bool acc = ImGui::Button("Accept for the session##aim");
    ImGui::EndDisabled();
    bwTip(!against ? "this run did not read the session's as_built.json: start it from the Session tab's aim step"
          : !listed ? "this speaker is not in the session's aim list"
          : "record this reading as the speaker's aim in the session (the Session tab's aim step)");
    if (!acc) return;
    SesAimRec r;
    r.spk = AJ.r_spk;
    r.below_db = d.below_db;
    char a[64] = "-";
    if (d.have_ref) aim_fmt(d.a_ref, a, sizeof a);
    else if (AJ.have_file) aim_fmt(d.a_file, a, sizeof a);
    r.angle = a;
    if (d.have_pos && d.lp.have_distance) {
        r.have_pos = true; r.pos_mm = d.lp.delta_norm_mm;
        const float e[3] = { d.lp.delta_mm[0] * 1e-3f, d.lp.delta_mm[1] * 1e-3f, d.lp.delta_mm[2] * 1e-3f };
        char w[160];
        place_move_words(e, PLACE_MOVE_DEAD_BOX_M, w, sizeof w);
        r.words = w;
    }
    r.when = ses_now();
    bool replaced = false;
    for (SesAimRec& x : P.aim) if (x.spk == r.spk) { x = r; replaced = true; }
    if (!replaced) P.aim.push_back(r);
    char b[400];
    snprintf(b, sizeof b, "aim: speaker %d accepted: %.2f dB below the peak, off axis %s%s%s", r.spk, r.below_db, r.angle.c_str(),
             r.have_pos ? ", " : "", r.have_pos ? (std::to_string((int)lroundf(r.pos_mm)) + " mm from the plan (" + r.words + ")").c_str() : "");
    ses_log(&SR, ses_logpath(), b);
    int done = 0;
    for (int v : list) for (const SesAimRec& x : P.aim) done += x.spk == v;
    if (done == (int)list.size()) {
        P.status = SES_PASSED;
        P.finished = ses_now();
        P.summary = std::to_string(done) + " speaker(s) aimed and accepted";
        ses_log(&SR, ses_logpath(), "=== " + ses_now() + ": aim passed: " + P.summary);
        ses_say("aim passed: every listed speaker has an accepted reading");
    } else {
        for (int v : list) {
            bool have = false;
            for (const SesAimRec& x : P.aim) have |= x.spk == v;
            if (!have) { ses_say("aim: speaker %d accepted; next, speaker %d (Session tab)", r.spk, v); break; }
        }
    }
    ses_save_now();
}

static void ses_input(const char* label, const char* hint, char* buf, size_t cap, float w, const char* tip) {
    ImGui::SetNextItemWidth(w);
    if (ImGui::InputTextWithHint(label, hint, buf, cap)) { ses_ui_to(); g_ses_dirty = true; }
    bwTip(tip);
}

static void tab_session(void) {
    const ImVec4 red(1.0f, 0.42f, 0.42f, 1.0f), amber(0.95f, 0.8f, 0.35f, 1.f), green(0.45f, 0.85f, 0.5f, 1.0f);
    const bool busy = ses_busy();
    ImGui::TextColored(amber, "Unverified on hardware: every step here has run only in simulate, never against the rig.");
    ImGui::BeginDisabled(busy);
    ImGui::SetNextItemWidth(-uiScaled(250));
    ImGui::InputTextWithHint("##sesdir", "session folder (one per rig day)", SU.dir, sizeof SU.dir);
    bwTip("the folder that holds session.json, session.log and every file the steps write. The plan is never written");
    ImGui::SameLine(); if (ImGui::Button("New##ses")) ses_open(true);
    bwTip("make the folder and a new session in it, with the inputs below");
    ImGui::SameLine(); if (ImGui::Button("Open##ses")) ses_open(false);
    bwTip("reopen a session: its inputs, every step's record and the log come back");
    ImGui::EndDisabled();
    if (g_ses_msg[0]) ImGui::TextWrapped("%s", g_ses_msg);
    if (SES.folder.empty()) {
        ImGui::TextDisabled("Make a new session folder, or open one. The steps run the runbook's Stage 2 in order, each\n"
                            "writing a new file in the folder and handing it to the next.");
        return;
    }
    ImGui::TextDisabled("session %s, created %s", SES.folder.c_str(), SES.created.c_str());

    /* -------- inputs -------- */
    ImGui::BeginDisabled(busy);
    if (ImGui::Checkbox("simulate##ses", &SES.simulate)) g_ses_dirty = true;
    bwTip("no hardware: every tool runs with --simulate, the tracked ones with --track-sim, and the Aim tab and the "
          "Placement panel in their simulate modes. The whole session rehearses on this machine");
    if (ImGui::CollapsingHeader("Inputs##ses", ImGuiTreeNodeFlags_DefaultOpen)) {
        const float w_long = -uiScaled(160), w_mid = uiScaled(160), w_short = uiScaled(90);
        ses_input("plan layout##ses", "the Stage 1 plan (read, never written)", SU.plan, sizeof SU.plan, w_long,
                  "the plan layout: the frame check and the localize run read it; the localize run writes as_built.json");
        ses_input("localize rows##ses", "x y z per line, 5 or more, off one plane", SU.lrows, sizeof SU.lrows, w_long,
                  "the ZM-1 placements for --localize; tracked, each is a plan and the measured center is recorded");
        ses_input("grid rows##ses", "x y z per line, up to 16, 250 mm apart", SU.grows, sizeof SU.grows, w_long,
                  "the room-EQ grid placements (--room-eq-grid rows.txt)");
        ses_input("validate placements##ses", "x y z per line (empty = the tool's default envelope)", SU.vpos, sizeof SU.vpos, w_long,
                  "bwa_validate --positions; empty runs its default listener envelope");
        ses_input("##sesdrv", "ASIO driver (auto)", SU.driver, sizeof SU.driver, w_mid, "the ASIO driver every step opens; empty = auto");
        ImGui::SameLine(); ImGui::SetNextItemWidth(w_short);
        if (ImGui::InputInt("first ZM-1 input##ses", &SES.in.input_first)) { if (SES.in.input_first < 0) SES.in.input_first = 0; g_ses_dirty = true; }
        bwTip("the driver input carrying the ZM-1's first capsule (--input, --mic-in)");
        ImGui::SameLine(); ses_input("temperature##ses", "e.g. 22.5C", SU.temp, sizeof SU.temp, w_short,
                                     "the room's air temperature, passed as --temp to every bwa_calibrate step; empty = the layout's");
        ses_input("##sesbody", "stand body (empty = untracked)", SU.body, sizeof SU.body, w_mid,
                  "the ZM-1 stand's rigid body: its streaming id or name. Empty: untracked, and the tools ask for Enter between rows");
        ImGui::SameLine(); ses_input("##sessrv", "Motive IP", SU.server, sizeof SU.server, w_mid, "Motive's host: the frame check, and tracking by name");
        ImGui::SameLine(); ses_input("##sesmc", "multicast", SU.multicast, sizeof SU.multicast, w_short + uiScaled(20), "the NatNet multicast group");
        ImGui::SameLine(); ses_input("mount offset##ses", "ring or x,y,z", SU.offset, sizeof SU.offset, w_mid,
                                     "body origin to the array center (--mount-offset) for localize and the capsule survey; "
                                     "after that the body-frame survey carries it");
        ImGui::SetNextItemWidth(w_short);
        if (ImGui::InputDouble("baffle offset (m)##ses", &SES.in.baffle_offset_m, 0.0, 0.0, "%.4f")) {
            if (!(fabs(SES.in.baffle_offset_m) <= SES_BAFFLE_MAX_M)) SES.in.baffle_offset_m = 0.0;
            g_ses_dirty = true;
        }
        bwTip("the frame check's --baffle-offset-m: layout positions are acoustic centers, and the speaker bodies' markers "
              "sit on the baffle this far in front of them. After localize, run bwa_speaker_survey on this session's "
              "as_built.json and enter its \"suggested --baffle-offset-m\", then run the frame step again. A change makes "
              "the frame step stale");
        ImGui::SameLine(); ses_input("frame speakers##ses", "simulate: e.g. 0,4,8", SU.fspk, sizeof SU.fspk, w_mid,
                  "simulate only: which speakers the simulated cameras see (--sim-speakers); empty = the tool's four");
        ImGui::SameLine(); ses_input("capsule speakers##ses", "e.g. 0-9 (empty = all)", SU.cspk, sizeof SU.cspk, w_mid,
                                     "the capsule survey's speakers (--speakers): 6 or more, high and low");
        ImGui::SameLine(); ses_input("aim speakers##ses", "e.g. 3,7", SU.aspk, sizeof SU.aspk, w_mid,
                                     "the speakers to aim live (the hidden ones); empty = none, so skip the step");
        ImGui::SetNextItemWidth(w_short);
        if (ImGui::InputInt("validate azimuths##ses", &SES.in.validate_azimuths)) {
            if (SES.in.validate_azimuths < 3) SES.in.validate_azimuths = 3;
            if (SES.in.validate_azimuths > 36) SES.in.validate_azimuths = 36;
            g_ses_dirty = true;
        }
        bwTip("bwa_validate --azimuths: 12 on the rig; 3 makes a quick rehearsal");
        ImGui::SameLine();
        if (ImGui::Checkbox("validate reference arm##ses", &SES.in.validate_reference)) g_ses_dirty = true;
        bwTip("off = bwa_validate --no-reference (faster, and no real-source floor under the phantom misses)");
        ImGui::SameLine(); ImGui::SetNextItemWidth(w_short);
        if (ImGui::InputInt("place timeout (s)##ses", &SES.in.place_timeout_s)) {
            if (SES.in.place_timeout_s < 1) SES.in.place_timeout_s = 1;
            g_ses_dirty = true;
        }
        bwTip("how long a tracked step waits for the placement gate (--place-timeout); the gate opens by itself");
        if (SES.simulate)
            ses_input("simulated truth##ses", "a layout: where the speakers really stand (optional)", SU.truth, sizeof SU.truth, w_long,
                      "simulate only (bwa_calibrate --sim-truth): the speakers stand here, not at the plan, so the "
                      "rehearsal has an as-built to find");
    }
    ImGui::EndDisabled();

    /* -------- the steps -------- */
    const std::vector<int> aim_list = ses_aim_list();
    /* one line per step, so the whole table and the controls under it stay on screen; the full record
     * (times, exit code, the reason, the command) is the row's tooltip */
    if (ImGui::BeginTable("sessteps", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("step", ImGuiTableColumnFlags_WidthFixed, uiScaled(175));
        ImGui::TableSetupColumn("status", ImGuiTableColumnFlags_WidthFixed, uiScaled(110));
        ImGui::TableSetupColumn("files", ImGuiTableColumnFlags_WidthFixed, uiScaled(230));
        ImGui::TableSetupColumn("result");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, uiScaled(130));
        ImGui::TableHeadersRow();
        for (int k = 0; k < SES_NSTEPS; ++k) {
            const SesStep& P = SES.step[k];
            const std::string stale = ses_stale(SES, k);
            const std::string blocked = (P.status == SES_PASSED && stale.empty()) || P.status == SES_RUNNING ||
                                        P.status == SES_SKIPPED ? std::string() : ses_blocked(SES, k);
            std::string tip = std::string(ses_step_title(k)) + ": " + ses_status_name(P.status);
            if (!P.started.empty()) tip += "\nstarted " + P.started + (P.finished.empty() ? "" : ", finished " + P.finished);
            if (P.status == SES_PASSED || P.status == SES_FAILED)
                tip += "\nexit " + std::to_string(P.exit_code) + " (" + ses_exit_meaning(k, P.exit_code) + ")";
            if (!P.note.empty()) tip += "\n" + P.note;
            if (!P.summary.empty()) tip += "\n" + P.summary;
            if (!stale.empty()) tip += "\nSTALE: " + stale;
            if (!blocked.empty()) tip += "\nblocked: " + blocked;
            if (!P.command.empty()) tip += "\n" + P.command;
            ImGui::TableNextRow();
            ImGui::PushID(k);
            ImGui::TableNextColumn(); ImGui::Text("%d  %s", k + 1, ses_step_title(k));
            ImGui::TableNextColumn();
            const ImVec4 col = !stale.empty() ? amber : P.status == SES_PASSED ? green : P.status == SES_FAILED ? red
                             : P.status == SES_RUNNING ? amber : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            if (P.status == SES_RUNNING && k == SES_AIM) {
                int done = 0;
                for (int v : aim_list) for (const SesAimRec& x : P.aim) done += x.spk == v;
                ImGui::TextColored(col, "aiming %d/%d", done, (int)aim_list.size());
            } else
                ImGui::TextColored(col, "%s%s", ses_status_name(P.status), !stale.empty() ? ", STALE" : (P.status == SES_RUNNING ? "..." : ""));
            ImGui::SetItemTooltip("%s", tip.c_str());
            ImGui::TableNextColumn();
            {
                std::string f;
                for (const SesFile& u : P.consumed) {
                    const size_t sl = u.path.find_last_of("\\/");
                    if (u.what == "baffle_offset_m") { f += (f.empty() ? "" : ", ") + ("baffle " + u.path + " m"); continue; }
                    f += (f.empty() ? "" : ", ") + (sl == std::string::npos ? u.path : u.path.substr(sl + 1));
                }
                std::string o;
                for (const SesFile& u : P.produced) o += (o.empty() ? "" : ", ") + u.path;
                if (k == SES_VERIFY && P.status == SES_PASSED) o = "verified";
                if (!f.empty() || !o.empty()) ImGui::Text("%s -> %s", f.empty() ? "-" : f.c_str(), o.empty() ? "-" : o.c_str());
                else ImGui::TextDisabled("-> %s", ses_step_artifact(k)[0] ? ses_step_artifact(k) : (k == SES_VERIFY ? "verified" : "readings"));
                ImGui::SetItemTooltip("%s", tip.c_str());
            }
            ImGui::TableNextColumn();
            if (!stale.empty())        ImGui::TextColored(amber, "STALE: %s", stale.c_str());
            else if (!blocked.empty()) ImGui::TextColored(P.status == SES_FAILED ? red : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
                                                          "blocked: %s", blocked.c_str());
            else if (P.status == SES_FAILED) ImGui::TextColored(red, "exit %d: %s", P.exit_code, P.note.c_str());
            else if (!P.summary.empty()) ImGui::TextUnformatted(P.summary.c_str());
            else if (!P.note.empty())    ImGui::TextUnformatted(P.note.c_str());
            else                         ImGui::TextDisabled("-");
            ImGui::SetItemTooltip("%s", tip.c_str());
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(busy);
            char id[48];
            snprintf(id, sizeof id, "Run##ses_%s", ses_step_id(k));
            if (ImGui::SmallButton(id)) ses_run_step(k);
            bwTip(k == SES_AIM ? "hand the first listed speaker to the Aim tab with the session's files"
                               : "run the tool for this step, in the session folder (refused, with the reason, when blocked)");
            ImGui::SameLine();
            snprintf(id, sizeof id, "Skip##ses_%s", ses_step_id(k));
            if (ImGui::SmallButton(id)) {
                if (!g_ses_note[0]) ses_say("%s: a skip needs a note: type why in the note field below", ses_step_id(k));
                else {
                    ses_skip(&SES, k, g_ses_note);
                    ses_log(&SR, ses_logpath(), "=== " + ses_now() + ": " + ses_step_id(k) + " skipped: " + g_ses_note);
                    g_ses_note[0] = 0;
                    ses_save_now();
                }
            }
            bwTip("mark the step skipped with the note below (for example: the frame was checked with Motive's own tools)");
            ImGui::SameLine();
            snprintf(id, sizeof id, "Reset##ses_%s", ses_step_id(k));
            if (ImGui::SmallButton(id)) {
                ses_reset(&SES, k);
                ses_log(&SR, ses_logpath(), "=== " + ses_now() + ": " + ses_step_id(k) + " reset to pending");
                ses_save_now();
            }
            bwTip("put the step back to pending; anything that used its file reads stale");
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(busy);
    ImGui::SetNextItemWidth(-uiScaled(250));
    ImGui::InputTextWithHint("##sesnote", "note for a skip (why)", g_ses_note, sizeof g_ses_note);
    ImGui::SameLine();
    if (ImGui::Button("Run next##ses")) {
        int nk = -1;
        for (int k = 0; k < SES_NSTEPS && nk < 0; ++k) {
            const SesStep& P = SES.step[k];
            const bool current = (P.status == SES_PASSED || P.status == SES_SKIPPED) && ses_stale(SES, k).empty();
            if (!current && P.status != SES_RUNNING) nk = k;
        }
        if (nk < 0) ses_say("every step has passed or was skipped");
        else ses_run_step(nk);
    }
    bwTip("run the first step that has not passed (or is stale)");
    ImGui::EndDisabled();

    /* -------- the file chain -------- */
    {
        static const int chain[] = { SES_LOCALIZE, SES_CAPSULES, SES_TRIMS, SES_VERIFY, SES_GRID, SES_VALIDATE };
        const size_t sl = SES.in.plan.find_last_of("\\/");
        ImGui::TextDisabled("chain:");
        ImGui::SameLine(); ImGui::Text("%s (plan, read only)", SES.in.plan.empty() ? "-" : SES.in.plan.substr(sl == std::string::npos ? 0 : sl + 1).c_str());
        for (int c : chain) {
            const SesStep& P = SES.step[c];
            const bool cur = P.status == SES_PASSED && ses_stale(SES, c).empty();
            ImGui::SameLine(); ImGui::TextDisabled("->");
            ImGui::SameLine();
            ImGui::TextColored(cur ? green : (P.status == SES_FAILED ? red : (P.status == SES_PASSED ? amber : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled))),
                               "%s", c == SES_VERIFY ? "verified" : ses_step_artifact(c));
        }
    }

    /* -------- the aim step's speakers -------- */
    if (!aim_list.empty() && ImGui::CollapsingHeader("Live aiming (the aim step)##ses", ImGuiTreeNodeFlags_DefaultOpen)) {
        const SesStep& P = SES.step[SES_AIM];
        for (int v : aim_list) {
            ImGui::PushID(v + 1000);
            const SesAimRec* r = NULL;
            for (const SesAimRec& x : P.aim) if (x.spk == v) r = &x;
            ImGui::BeginDisabled(P.status != SES_RUNNING || busy);
            char lb[48];
            snprintf(lb, sizeof lb, "Aim speaker %d##ses", v);
            if (ImGui::SmallButton(lb)) ses_aim_hand(v);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (r) ImGui::Text("accepted %s: %.2f dB below the peak, off axis %s%s%s", r->when.c_str() + (r->when.size() > 11 ? 11 : 0),
                               r->below_db, r->angle.c_str(), r->have_pos ? ", " : "",
                               r->have_pos ? (std::to_string((int)lroundf(r->pos_mm)) + " mm from the plan, " + r->words).c_str() : "");
            else ImGui::TextDisabled(v == g_ses_aim_cur && P.status == SES_RUNNING ? "in the Aim tab now" : "not yet");
            ImGui::PopID();
        }
        if (P.status != SES_RUNNING && P.status != SES_PASSED)
            ImGui::TextDisabled("Run the aim step to hand these to the Aim tab one at a time.");
        ImGui::TextDisabled("If a box moves, run localize again: everything after it reads stale.");
    }

    /* -------- the running step -------- */
    if (busy) {
        ImGui::TextColored(amber, "%s is running", ses_step_id(g_ses_step));
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel##ses")) ses_run_cancel(&SR);
        bwTip("stop the tool now (it is killed; on the rig the device closes with it). The step reads failed");
        ImGui::SameLine();
        if (ImGui::SmallButton("Send Enter##ses")) ses_run_enter(&SR);
        bwTip("untracked runs on the rig stop at each row with 'press Enter': place the ZM-1, then send it");
    }

    /* -------- the log -------- */
    ImGui::BeginChild("seslog", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard<std::mutex> lk(SR.mu);
        ImGuiListClipper clip;
        clip.Begin((int)SR.lines.size());
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                const std::string& l = SR.lines[(size_t)i];
                if (!l.compare(0, 4, "=== ")) ImGui::TextColored(amber, "%s", l.c_str());
                else ImGui::TextUnformatted(l.c_str());
            }
        if (!SR.partial.empty()) ImGui::TextColored(green, "%s", SR.partial.c_str());
        if (SR.lines.size() != g_ses_shown || !SR.partial.empty()) { ImGui::SetScrollHereY(1.0f); g_ses_shown = SR.lines.size(); }
    }
    ImGui::EndChild();
    if (g_ses_dirty && !ImGui::IsAnyItemActive()) ses_save_now();
}

static void draw_ui(void) {
    ses_poll();                                                  /* a finished session step, whatever tab shows */
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
        if (ImGui::BeginTabItem("Session")) { tab_session(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Array")) { tab_array(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Trims")) { tab_trims(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("EQ"))    { tab_eq();    ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("IRs"))     { tab_irs();     ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Diff"))    { tab_diff();    ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Capture")) { tab_capture(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Zylia"))   { tab_zylia();   ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Aim", NULL, g_ses_aim_tab ? ImGuiTabItemFlags_SetSelected : 0)) { tab_aim(); ImGui::EndTabItem(); }
        g_ses_aim_tab = false;                                   /* the session handed it a speaker: show it once */
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
static const char* FIX_P = "calibview_fix_p.json";   /* E plus a plan for speaker 3, 60 mm toward +x of its position */
/* The small arrays the ZM-1 Capture tests measure. A ZM-1 trim or verify run deconvolves 19 capsules per
 * sweep and takes two agreeing sweeps per speaker, so those tests cost their speaker count: about 1 s a
 * speaker anechoic, 3 s in the room. Their checks compare EVERY speaker (against the simulator's truth or
 * the CLI) and none needs 26, so they run on three antipodal pairs of the same grid: a face, an edge and
 * a corner pair (1.5, 2.12 and 2.60 m out, so the delay trims are not all equal), the centroid at the
 * origin like FIX_A's, and the grid's own bounding box, so the simulated room is the same shoebox. */
static const char* FIX_S  = "calibview_fix_s.json";  /* FIX_A's grid, SMALL_N speakers of it */
static const char* FIX_DS = "calibview_fix_ds.json"; /* FIX_D's model and listening point on FIX_S's speakers */
#define SMALL_N 6
static const int SMALL_GRID[SMALL_N][3] = { { 1, 0, 0 }, { 0, 1, 1 }, { 1, 1, -1 }, { -1, 0, 0 }, { 0, -1, -1 }, { -1, -1, 1 } };

static int write_fixture(const char* path, int variant_b, bool few = false) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "{\n  \"schema_version\": 1,\n"
               "  \"units\": { \"position\": \"meters\", \"gain\": \"decibels\", \"delay\": \"milliseconds\" },\n"
               "  \"coordinate_space\": \"room, right-handed, +y up, +z forward; origin on the floor\",\n"
               "  \"reference\": { \"alignment\": \"max-distance\", \"speed_of_sound_mps\": 343.0 },\n"
               "  \"dbap\": { \"rolloff_r\": 0.5, \"distance_attenuation\": { \"model\": \"inverse\","
               " \"reference_distance_m\": 1.0, \"rolloff\": 1.0, \"min_gain_db\": -40.0 } },\n"
               "  \"speakers\": [\n");
    int grid[26][3], n = 0;
    if (few) { memcpy(grid, SMALL_GRID, sizeof SMALL_GRID); n = SMALL_N; }
    else
        for (int zi = -1; zi <= 1; ++zi) for (int yi = -1; yi <= 1; ++yi) for (int xi = -1; xi <= 1; ++xi) {
            if (!zi && !yi && !xi) continue;                      /* 27 - center = 26 */
            grid[n][0] = xi; grid[n][1] = yi; grid[n][2] = zi; ++n;
        }
    for (int idx = 0; idx < n; ++idx) {
        double x = 1.5 * grid[idx][0], y = 1.5 * grid[idx][1], z = 1.5 * grid[idx][2], g = 0.0;
        if (variant_b == 1 && idx == 7) { x += 0.10; g = -1.5; }  /* the known deltas */
        fprintf(f, "    { \"index\": %d, \"position\": [%.4f, %.4f, %.4f], \"gain_db\": %.2f, \"delay_ms\": %.3f",
                idx, x, y, z, g, 0.05 * idx);
        if (variant_b == 1 && idx == 3)
            fprintf(f, ", \"eq\": [0.9, 0.2, -0.1, 0.05, 0.02, -0.01, 0.005, 0.0]");
        if (variant_b == 4 && idx == 3)                           /* the plan: the as-built box sits 60 mm toward -x of it */
            fprintf(f, ", \"plan_position\": [%.4f, %.4f, %.4f]", x + 0.06, y, z);
        fprintf(f, " }%s\n", idx < n - 1 ? "," : "");
    }
    if (variant_b == 3 || variant_b == 4) {
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

/* The tracked-clicker tests' helpers. The script runs on its own virtual clock, stepped once per frame and
 * never more than CLICKER_SIM_MAX_STEP_S (clicker_track.h), so what it does does not depend on load; the
 * waits here are only timeouts, on the WALL clock. */
static void ck_wait_state(ImGuiTestContext* ctx, int want, double secs) {
    const double t0 = clicker_clock_s();
    while (clicker_state(&CK) != want && clicker_clock_s() - t0 < secs) ctx->Yield();
}
/* One simulated-clicker survey through the Zylia tab: the clicker named "clicker" (resolved through the
 * script's model definitions, with a decoy stand body ahead of it in every frame), the tip offset
 * already in Z.ck_tip, a cleared survey, every scripted click, then disconnect. */
static void ck_run_survey(ImGuiTestContext* ctx, bool ring) {
    ctx->ItemClick("**/tracked clicker##zsrc");
    ctx->Yield(2);
    if (clicker_state(&CK) == CLICKER_LIVE) { ctx->ItemClick("**/Disconnect##ck"); ck_wait_state(ctx, CLICKER_OFF, 5.0); }
    ctx->ItemClick("**/##ckbody");   ctx->KeyCharsReplaceEnter("clicker");
    ctx->ItemCheck("**/simulate##ck");
    if (ring) ctx->ItemCheck("**/coplanar ring##ck"); else ctx->ItemUncheck("**/coplanar ring##ck");
    ctx->ItemClick("**/Clear");
    /* Connect stops the walking simulated claps, but they run on the frames' DeltaTime, and one landing in
     * the frames between "record claps" and Connect was refused ("not tracking") under load */
    Z.sim_walk = false;
    ctx->ItemCheck("**/record claps");
    ctx->ItemClick("**/Connect##ck");
    ck_wait_state(ctx, CLICKER_LIVE, 10.0);
    IM_CHECK_EQ(clicker_state(&CK), CLICKER_LIVE);
    const double t0 = clicker_clock_s(), v0 = clicker_now(&CK);
    while ((clicker_sim_left(&CK) > 0 || Z.pend_on || Z.sim_pub_on) && clicker_clock_s() - t0 < 90.0) ctx->Yield();
    printf("clicker test: the script took %.1f s of its clock in %.1f s of wall time\n", clicker_now(&CK) - v0,
           clicker_clock_s() - t0);
    IM_CHECK_EQ(clicker_sim_left(&CK), 0);
    IM_CHECK(!Z.pend_on);
    IM_CHECK(!Z.sim_pub_on);
    ctx->ItemClick("**/Disconnect##ck");
    ck_wait_state(ctx, CLICKER_OFF, 5.0);
}
/* the worst distance of a recovered capsule from the built-in table the claps were synthesized from (m) */
static float ck_geom_err(void) {
    float ref[ZYLIA_MICS][3], R, worst = 0.f;
    zylia_set_capsules(NULL);
    zylia_geometry(ref, &R);
    for (int i = 0; i < ZYLIA_MICS; ++i) {
        const float tr[3] = { R * ref[i][0], R * ref[i][1], R * ref[i][2] };
        const float d = dist3(Z.surv_caps[i], tr);
        if (d > worst) worst = d;
    }
    return worst;
}
static float g_ck_good_resid = -1.f, g_ck_good_geom = -1.f;   /* the right-tip run, for the wrong-tip one */
/* the worst distance of a banked clap from its TRUE position (m), and how many banked claps were the script's
 * interferer (the last one's index in *last) */
static float ck_banked_vs_truth(int* ninterf, int* last) {
    float worst = 0.f;
    *ninterf = 0; *last = -1;
    for (int k = 0; k < Z.surv_n; ++k) {
        if (Z.surv_interf[k]) { ++*ninterf; *last = k; continue; }
        float at[3];
        for (int a = 0; a < 3; ++a) at[a] = Z.surv_src[k][a] + Z.surv_center_used[k][a];
        const float d = dist3(at, Z.surv_truth[k]);
        if (d > worst) worst = d;
    }
    return worst;
}
/* typed-arm test: a simulated clap from direction d at ZY_SIM_DIST, and the typed clap position at direction t */
static void ta_unit(const float d[3], float o[3]) {
    const float n = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int a = 0; a < 3; ++a) o[a] = d[a] / n;
}
static void ta_aim(const float truth_dir[3], const float typed_dir[3]) {
    float t[3], p[3];
    ta_unit(truth_dir, t); ta_unit(typed_dir, p);
    memcpy(Z.truth, t, sizeof t);
    for (int a = 0; a < 3; ++a) Z.surv_clap[a] = Z.surv_center[a] + (float)ZY_SIM_DIST * p[a];
}
/* Scroll the tab until `ref` is on screen: a clipped item cannot be found by its label, and the capsule survey
 * runs past the bottom of the window with the clicker and the Placement panel open. */
static void zy_reach(ImGuiTestContext* ctx, const char* ref) {
    if (ctx->ItemExists(ref)) return;
    ImGuiWindow* w = ctx->WindowInfo("//calib view/main").Window;
    if (!w) return;
    ctx->ScrollToTop(w->ID);
    for (int i = 0; i < 60 && !ctx->ItemExists(ref) && w->Scroll.y < w->ScrollMax.y; ++i)
        ctx->ScrollToY(w->ID, ImMin(w->Scroll.y + 80.f, w->ScrollMax.y));
}
static void ta_wait(ImGuiTestContext* ctx, double secs) {
    const double t0 = clicker_clock_s();
    while (clicker_clock_s() - t0 < secs) ctx->Yield();
}

/* the Capture tests' helpers: click Run (trims or verify) and wait for the worker on the wall clock */
static void cap_run(ImGuiTestContext* ctx, const char* button, double secs) {
    ctx->ItemClick(button);
    const double t0 = ImGui::GetTime();
    while (J.state.load() == 1 && ImGui::GetTime() - t0 < secs) ctx->Yield();
}

/* The ZM-1 trims a correct run must write in simulate, from the truth the simulator generates at and
 * not from anything the run measured: the gain trim cuts each speaker's simulated sensitivity down to
 * the least sensitive one (calib_solve divides the 1/r out), and the delay trim aligns every arrival
 * to the farthest speaker's at the mic, to the whole sample the trims round to. Prints and returns the
 * worst errors (dB, us) over the layout at `path`. */
static void zy_truth_errors(const char* path, const float mic[3], float* worst_db, float* worst_us) {
    static Layout TL;                                            /* never a stack local (layout.h) */
    char e[256];
    *worst_db = *worst_us = 1e9f;
    if (!layout_load(path, (uint32_t)CAL_FS, &TL, e, sizeof e)) return;
    double c = BWA_SOS_REF_MPS;
    { double v; if (calib_read_sos(path, &v)) c = v; }
    const int n = (int)TL.count;
    double smin = 1e30, dmax = 0.0;
    for (int i = 0; i < n; ++i) {
        if (calib_sim_sensitivity(i) < smin) smin = calib_sim_sensitivity(i);
        const double d = dist3(TL.speakers[i].pos, mic);
        if (d > dmax) dmax = d;
    }
    *worst_db = *worst_us = 0.f;
    int wg = 0, wd = 0;
    for (int i = 0; i < n; ++i) {
        const double g = 20.0 * log10(smin / calib_sim_sensitivity(i));
        const double ms = (dmax - dist3(TL.speakers[i].pos, mic)) / c * 1e3;
        const float eg = (float)fabs(J.gain_db[i] - g), ed = (float)fabs(J.trim_ms[i] - ms) * 1e3f;
        if (eg > *worst_db) { *worst_db = eg; wg = i; }
        if (ed > *worst_us) { *worst_us = ed; wd = i; }
    }
    printf("capture test: ZM-1 trims against the simulator's truth: worst gain %.3f dB (speaker %d), worst delay %.1f us (speaker %d)\n",
           *worst_db, wg, *worst_us, wd);
}

/* bwa_calibrate for the CLI cross-check: where ctest says it is (BWA_CALIBRATE_EXE, set when both
 * tools are built), else beside this exe. false = not built. */
static bool find_calibrate(char* buf, size_t cap) {
    const char* env = getenv("BWA_CALIBRATE_EXE");
    if (env && *env && GetFileAttributesA(env) != INVALID_FILE_ATTRIBUTES) { snprintf(buf, cap, "%s", env); return true; }
    char self[MAX_PATH];
    const DWORD k = GetModuleFileNameA(NULL, self, MAX_PATH);
    if (!k || k >= MAX_PATH) return false;
    char* sl = strrchr(self, '\\');
    if (!sl) return false;
    sl[1] = 0;
    snprintf(buf, cap, "%sbwa_calibrate.exe", self);
    return GetFileAttributesA(buf) != INVALID_FILE_ATTRIBUTES;
}

/* start `cmdline` with its stdout and stderr in `log`; NULL on failure */
static HANDLE spawn_logged(const char* cmdline, const char* log) {
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE f = CreateFileA(log, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = f; si.hStdError = f; si.hStdInput = NULL;
    PROCESS_INFORMATION pi;
    char cl[2048];
    snprintf(cl, sizeof cl, "%s", cmdline);
    const BOOL ok = CreateProcessA(NULL, cl, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(f);
    if (!ok) return NULL;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

/* The Capture tab's mode radios persist across tests, so a test that dies mid-way (a watchdog abort
 * under a loaded ctest -j) leaves "verify" or "ZM-1" selected and every later bare "Run calibration"
 * test then fails looking for a button that is labeled "Run verify". Each omni trim test selects its
 * own mode instead of trusting the last test to have put it back. */
static void cap_omni_trims(ImGuiTestContext* ctx) {
    ctx->ItemClick("**/trims##cpass");
    ctx->ItemClick("**/omni##cmic");
    ctx->Yield(1);
}

/* ---- the Session tests' fixtures and helpers ----
 * A 10-speaker array (two rings and two low boxes, so no plane holds it) around a listening point at
 * ear height. The PLAN is where the boxes were meant to go; the TRUTH (bwa_calibrate --sim-truth) is
 * where the simulated rig put them: five of them 72 to 100 mm off the plan. A session that measured the
 * plan back, or trimmed against it, lands off the truth. */
static const char* SES_PLAN  = "calibview_ses_plan.json";
static const char* SES_TRUTH = "calibview_ses_truth.json";
/* failure_blocks needs a localize that passes and nothing after it measures, so it takes the plan's first
 * SES_NSPK_FEW speakers (the upper ring and one mid box): localize sweeps every speaker from every row,
 * and on all 10 it was most of that test's time */
static const char* SES_PLAN_FEW  = "calibview_ses_plan_few.json";
static const char* SES_TRUTH_FEW = "calibview_ses_truth_few.json";
#define SES_NSPK_FEW 5
static const char* SES_ROWS  = "calibview_ses_rows.txt";
static const char* SES_GRIDR = "calibview_ses_grid.txt";
static const char* SES_VPOS  = "calibview_ses_vpos.txt";
static const char* SES_FULL  = "calibview_session_full";
static const char* SES_BLOCK = "calibview_session_block";
#define SES_NSPK 10
static const float SES_PLAN_POS[SES_NSPK][3] = {
    {  1.34f, 2.60f,  1.34f }, {  1.34f, 2.60f, -1.34f }, { -1.34f, 2.60f, -1.34f }, { -1.34f, 2.60f,  1.34f },
    {  0.00f, 1.45f,  2.20f }, {  2.20f, 1.45f,  0.00f }, {  0.00f, 1.45f, -2.20f }, { -2.20f, 1.45f,  0.00f },
    {  1.56f, 0.30f,  0.90f }, { -1.56f, 0.30f, -0.90f } };
static const float SES_MOVE[SES_NSPK][3] = {
    { 0.f, 0.f, 0.f }, { 0.06f, 0.f, -0.05f }, { 0.f, 0.f, 0.f }, { -0.07f, 0.f, 0.05f }, { 0.f, 0.f, 0.10f },
    { 0.f, 0.f, 0.f }, { 0.05f, 0.f, -0.06f }, { 0.f, 0.f, 0.f }, { 0.06f, -0.04f, 0.f }, { 0.f, 0.f, 0.f } };

static int ses_write_layout(const char* path, int truth, int n = SES_NSPK) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "{\n  \"schema_version\": 1,\n  \"listening_point_m\": [0.0, 1.45, 0.0],\n  \"speakers\": [\n");
    for (int i = 0; i < n; ++i) {
        float p[3];
        for (int a = 0; a < 3; ++a) p[a] = SES_PLAN_POS[i][a] + (truth ? SES_MOVE[i][a] : 0.f);
        fprintf(f, "    { \"index\": %d, \"position\": [%.4f, %.4f, %.4f] }%s\n", i, p[0], p[1], p[2], i < n - 1 ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    return 1;
}
static int ses_write_text(const char* path, const char* txt) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    fputs(txt, f);
    fclose(f);
    return 1;
}
static void ses_write_fixtures(void) {
    ses_write_layout(SES_PLAN, 0);
    ses_write_layout(SES_TRUTH, 1);
    ses_write_layout(SES_PLAN_FEW, 0, SES_NSPK_FEW);
    ses_write_layout(SES_TRUTH_FEW, 1, SES_NSPK_FEW);
    ses_write_text(SES_ROWS, "0.6 1.2 0.4\n-0.6 1.6 0.5\n0.5 1.8 -0.6\n-0.5 1.0 -0.5\n0.0 1.45 0.0\n");
    ses_write_text(SES_GRIDR, "0.4 1.45 0.4\n-0.4 1.45 -0.4\n");
    ses_write_text(SES_VPOS, "0 1.45 0 center\n");
}
/* a fresh folder: the files a session writes, then the folder */
static void ses_clear_dir(const char* dir) {
    static const char* const files[] = { "session.json", "session.json.tmp", "session.log", "frame_check.csv", "as_built.json",
                                         "capsules.json", "trims.json", "grid.json", "validate.csv" };
    for (const char* f : files) DeleteFileA(ses_join(ses_abs(dir), f).c_str());
    RemoveDirectoryA(dir);
}
/* wait for the running step to be collected (ses_poll runs every frame) */
static void ses_ui_wait(ImGuiTestContext* ctx, double secs) {
    const double t0 = ImGui::GetTime();
    while ((ses_busy() || g_ses_step >= 0) && ImGui::GetTime() - t0 < secs) ctx->Yield();
    ctx->Yield(2);
}
static void ses_ui_run(ImGuiTestContext* ctx, int k, double secs) {
    char id[64];
    snprintf(id, sizeof id, "**/Run##ses_%s", ses_step_id(k));
    const double t0 = ImGui::GetTime();
    ctx->ItemClick(id);
    ses_ui_wait(ctx, secs);
    printf("session test: %-8s %s (exit %d) in %.1f s%s%s\n", ses_step_id(k), ses_status_name(SES.step[k].status),
           SES.step[k].exit_code, ImGui::GetTime() - t0, SES.step[k].summary.empty() ? "" : ": ", SES.step[k].summary.c_str());
}
static void ses_ui_type(ImGuiTestContext* ctx, const char* item, const char* text) {
    ctx->ItemClick(item);
    ctx->KeyCharsReplaceEnter(text);
}
/* a new session in `dir` through the tab, simulated, with the fixtures as its inputs */
static void ses_ui_new(ImGuiTestContext* ctx, const char* dir, const char* capsule_speakers, const char* aim_speakers,
                       const char* plan = SES_PLAN, const char* truth = SES_TRUTH) {
    ctx->SetRef("calib view");
    ctx->ItemClick("**/Session");
    ctx->Yield(2);
    IM_CHECK(!ses_busy());
    SES = Session();                     /* nothing carries over from an earlier test */
    memset(&SU, 0, sizeof SU);
    g_ses_msg[0] = 0;
    ses_clear_dir(dir);
    ses_ui_type(ctx, "**/##sesdir", dir);
    ctx->ItemClick("**/New##ses");
    ctx->Yield(2);
    IM_CHECK(!SES.folder.empty());
    ctx->ItemCheck("**/simulate##ses");
    ses_ui_type(ctx, "**/plan layout##ses", plan);
    ses_ui_type(ctx, "**/localize rows##ses", SES_ROWS);
    ses_ui_type(ctx, "**/grid rows##ses", SES_GRIDR);
    ses_ui_type(ctx, "**/validate placements##ses", SES_VPOS);
    ses_ui_type(ctx, "**/mount offset##ses", "0.02,-0.11,0.04");
    ses_ui_type(ctx, "**/baffle offset (m)##ses", "0.06");
    if (capsule_speakers) ses_ui_type(ctx, "**/capsule speakers##ses", capsule_speakers);
    if (aim_speakers) ses_ui_type(ctx, "**/aim speakers##ses", aim_speakers);
    ctx->ItemInputValue("**/validate azimuths##ses", 3);
    ses_ui_type(ctx, "**/simulated truth##ses", truth);
    ctx->Yield(2);
}
static float ses_dist(const float a[3], const float b[3]) { return dist3(a, b); }
/* a step's consumed record for `what`, or NULL */
static const SesFile* ses_used(int k, const char* what) {
    for (const SesFile& f : SES.step[k].consumed) if (f.what == what) return &f;
    return NULL;
}
/* the chain link: step k consumed step j's file as j produced it, in j's current run */
static bool ses_link(int k, int j) {
    const SesFile* u = ses_used(k, ses_step_id(j));
    const SesStep& Q = SES.step[j];
    return u && !Q.produced.empty() && u->hash == Q.produced[0].hash && u->run == Q.run && u->path == ses_step_artifact(j);
}
static bool ses_same_files(const std::vector<SesFile>& a, const std::vector<SesFile>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].what != b[i].what || a[i].path != b[i].path || a[i].hash != b[i].hash || a[i].run != b[i].run) return false;
    return true;
}
/* two sessions equal in everything the file carries; *why names the first difference */
static bool ses_same(const Session& A, const Session& B, std::string* why) {
    const SesInputs &I = A.in, &J2 = B.in;
    if (A.simulate != B.simulate || A.next_run != B.next_run || A.created != B.created) { *why = "simulate/next_run/created"; return false; }
    if (I.plan != J2.plan || I.localize_rows != J2.localize_rows || I.grid_rows != J2.grid_rows || I.mount_offset != J2.mount_offset ||
        I.aim_speakers != J2.aim_speakers || I.capsule_speakers != J2.capsule_speakers || I.validate_positions != J2.validate_positions ||
        I.validate_azimuths != J2.validate_azimuths || I.sim_truth != J2.sim_truth || I.input_first != J2.input_first ||
        I.baffle_offset_m != J2.baffle_offset_m ||
        I.validate_reference != J2.validate_reference || I.place_timeout_s != J2.place_timeout_s) { *why = "inputs"; return false; }
    for (int k = 0; k < SES_NSTEPS; ++k) {
        const SesStep &P = A.step[k], &Q = B.step[k];
        *why = ses_step_id(k);
        if (P.status != Q.status || P.run != Q.run || P.exit_code != Q.exit_code || P.started != Q.started ||
            P.finished != Q.finished || P.note != Q.note || P.command != Q.command || P.summary != Q.summary) return false;
        if (!ses_same_files(P.consumed, Q.consumed) || !ses_same_files(P.produced, Q.produced)) return false;
        if (P.have_latency != Q.have_latency || fabs(P.latency_m - Q.latency_m) > 1e-9) return false;
        if (P.aim.size() != Q.aim.size()) return false;
        for (size_t i = 0; i < P.aim.size(); ++i)
            if (P.aim[i].spk != Q.aim[i].spk || P.aim[i].angle != Q.aim[i].angle || P.aim[i].when != Q.aim[i].when ||
                fabsf(P.aim[i].below_db - Q.aim[i].below_db) > 1e-4f) return false;
    }
    why->clear();
    return true;
}
static bool ses_log_has(const char* needle) {
    std::lock_guard<std::mutex> lk(SR.mu);
    for (const std::string& l : SR.lines) if (l.find(needle) != std::string::npos) return true;
    return false;
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
        cap_omni_trims(ctx);
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
        ctx->ItemClick("**/##cl");   ctx->KeyCharsReplaceEnter(FIX_DS);
        ctx->ItemClick("**/##co");   ctx->KeyCharsReplaceEnter("calibview_cap_out_d.json");
        ctx->ItemCheck("**/simulate");
        ctx->Yield(2);
        IM_CHECK(J.have_model);
        IM_CHECK(!J.mic_set);
        IM_CHECK_LT(fabsf(J.mic[0] - 0.2f), 1e-4f);               /* the layout's listening point, */
        IM_CHECK_LT(fabsf(J.mic[1] - 0.3f), 1e-4f);               /* not (0, 0, 0)                 */
        IM_CHECK_LT(fabsf(J.mic[2] + 0.1f), 1e-4f);
        IM_CHECK_EQ(J.dir_note, 1);
        cap_omni_trims(ctx);
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
        cap_omni_trims(ctx);
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

    /* ---- the ZM-1 as the Capture tab's mic (bwa_calibrate --zylia --trims / --verify) ----
     * An anechoic simulated trim run, held against the truth the simulator generates at: the 19
     * capsules pooled into the center's pressure and arrival land every gain within 0.05 dB and every
     * delay within a sample of it. One capsule standing in for the pool sits up to 49 mm off the center
     * (193 us and 0.28 dB at worst on FIX_S, measured by breaking the pool on purpose), and the omni path
     * measures no capsule spread. */
    t = IM_REGISTER_TEST(e, "capture", "zylia_trims");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("calib view");
        ctx->ItemClick("**/Capture");
        ctx->Yield(2);
        IM_CHECK_EQ(PL.state.load(), 0);                          /* no tracker: the typed mic is the mic */
        J.mic_set = false;                                        /* earlier tests share J */
        J.survey[0] = 0;
        ctx->ItemClick("**/trims##cpass");
        ctx->ItemClick("**/ZM-1##cmic");
        ctx->ItemClick("**/##cl");   ctx->KeyCharsReplaceEnter(FIX_S);
        ctx->ItemClick("**/##co");   ctx->KeyCharsReplaceEnter("calibview_zy_out.json");
        ctx->ItemCheck("**/simulate");
        ctx->ItemUncheck("**/room##cap");
        cap_run(ctx, "**/Run calibration", 120.0);
        printf("capture test: ZM-1 trims: state %d, %s\n", J.state.load(), J.msg);
        IM_CHECK_EQ(J.state.load(), 2);
        IM_CHECK_EQ(J.done_count.load(), SMALL_N);
        IM_CHECK(J.ran_zylia);
        IM_CHECK_EQ(J.table_src, 0);                              /* no survey: the built-in table */
        float smin = 1e9f;
        for (int i = 0; i < SMALL_N; ++i) if (J.spread_db[i] < smin) smin = J.spread_db[i];
        IM_CHECK_GT(smin, 0.1f);                                  /* 19 capsules measured, not one input */
        float wdb, wus;
        zy_truth_errors(FIX_S, J.mic_run, &wdb, &wus);
        IM_CHECK_LT(wdb, 0.05f);
        IM_CHECK_LT(wus, 1e6f / (float)CAL_FS + 1.f);             /* one sample: the trims are whole samples */
        ctx->ItemClick("**/Load into Diff (A=input, B=result)");
        IM_CHECK(V.hasB);
        ctx->CaptureScreenshotWindow("//calib view");
    };

    /* The tab's ZM-1 trims ARE the CLI's: the same simulated setup through bwa_calibrate --zylia --trims
     * (the room on, a directivity model, a mic off the listening point, so the re-aim and the direct
     * shares are in play) writes the same gain and delay on every speaker. A tab that measured with the
     * wrong mic, the wrong capsule table, room or c does not match: on FIX_DS the omni in place of the
     * pooled ZM-1 moved 5 of 6 gains (up to 0.06 dB) and 2 delays, and an absorption of 0.25 in place of
     * 0.3 moved 5 gains by up to 0.54 dB. bwa_calibrate is not built: the cross-check cannot run,
     * and the test SAYS so rather than pass in silence; the truth test above still runs. */
    t = IM_REGISTER_TEST(e, "capture", "zylia_matches_cli");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        char exe[MAX_PATH];
        if (!find_calibrate(exe, sizeof exe)) {
            printf("capture test: zylia_matches_cli NOT RUN: bwa_calibrate is not built (-DBWA_BUILD_CALIBRATE=ON)\n");
            return;
        }
        const float mic[3] = { 0.5f, 0.4f, 0.3f };
        remove("calibview_zy_cli.json"); remove("calibview_zy_gui.json");
        char cl[1400];
        snprintf(cl, sizeof cl, "\"%s\" --layout %s --out calibview_zy_cli.json --simulate --sim-room 0.3 "
                                "--mic 0.5 0.4 0.3 --zylia --trims", exe, FIX_DS);
        HANDLE ph = spawn_logged(cl, "calibview_zy_cli.log");      /* runs beside the tab's own run */
        IM_CHECK(ph != NULL);
        if (!ph) return;
        ctx->SetRef("calib view");
        ctx->ItemClick("**/Capture");
        ctx->Yield(2);
        IM_CHECK_EQ(PL.state.load(), 0);
        J.mic_set = false; J.survey[0] = 0;
        ctx->ItemClick("**/trims##cpass");
        ctx->ItemClick("**/ZM-1##cmic");
        ctx->ItemClick("**/##cl");   ctx->KeyCharsReplaceEnter(FIX_DS);
        ctx->ItemClick("**/##co");   ctx->KeyCharsReplaceEnter("calibview_zy_gui.json");
        ctx->ItemCheck("**/simulate");
        ctx->ItemCheck("**/room##cap");
        for (int a = 0; a < 3; ++a) ctx->ItemInputValue(ctx->GetIDByInt(a, J.mic_scope_id), mic[a]);
        ctx->Yield(2);
        IM_CHECK(J.mic_set);
        cap_run(ctx, "**/Run calibration", 240.0);
        printf("capture test: ZM-1 trims in the room: state %d, %s\n", J.state.load(), J.msg);
        IM_CHECK_EQ(J.state.load(), 2);
        IM_CHECK(J.ran_zylia && J.ran_room);
        IM_CHECK(J.corr_applied);                                 /* a model and a set mic: re-aimed, like --mic */
        IM_CHECK_LT(dist3(J.mic_run, mic), 1e-6f);
        const double t0 = ImGui::GetTime();
        while (WaitForSingleObject(ph, 0) == WAIT_TIMEOUT && ImGui::GetTime() - t0 < 300.0) ctx->Yield();
        DWORD code = 99;
        GetExitCodeProcess(ph, &code);
        CloseHandle(ph);
        printf("capture test: bwa_calibrate exited %lu (calibview_zy_cli.log)\n", (unsigned long)code);
        IM_CHECK_EQ((int)code, 0);
        static Layout LC, LG;                                     /* never stack locals (layout.h) */
        char e1[256], e2[256];
        IM_CHECK(layout_load("calibview_zy_cli.json", (uint32_t)CAL_FS, &LC, e1, sizeof e1));
        IM_CHECK(layout_load("calibview_zy_gui.json", (uint32_t)CAL_FS, &LG, e2, sizeof e2));
        IM_CHECK_EQ(LC.count, (uint32_t)SMALL_N);
        IM_CHECK_EQ(LG.count, (uint32_t)SMALL_N);
        int nd = 0, ng = 0;
        float wg = 0.f, gspan = 0.f;
        for (uint32_t i = 0; i < LC.count && i < LG.count; ++i) {
            const float g = fabsf(lin_to_db(LC.speakers[i].gain_lin) - lin_to_db(LG.speakers[i].gain_lin));
            if (g > wg) wg = g;
            if (g > 0.005f) ++ng;
            if (LC.speakers[i].delay_samples != LG.speakers[i].delay_samples) ++nd;
            const float gd = fabsf(lin_to_db(LG.speakers[i].gain_lin));
            if (gd > gspan) gspan = gd;
        }
        printf("capture test: tab vs bwa_calibrate: %d gain(s) differ (worst %.3f dB), %d delay(s) differ; trims span %.2f dB\n",
               ng, wg, nd, gspan);
        IM_CHECK_EQ(ng, 0);
        IM_CHECK_EQ(nd, 0);
        IM_CHECK_GT(gspan, 0.5f);                                 /* the trims are not all zero */
        ctx->CaptureScreenshotWindow("//calib view");
    };

    /* Verify, through the tab: the ZM-1 plays a fresh trim set back through the engine's output stage
     * and flags nothing; one speaker's delay_ms corrupted by +0.3 ms flags exactly that speaker, on
     * ARRIVAL, at about +300 us, with the ZM-1 and with the omni. */
    t = IM_REGISTER_TEST(e, "capture", "verify_flags_delay");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const int BAD = 5;
        ctx->SetRef("calib view");
        ctx->ItemClick("**/Capture");
        ctx->Yield(2);
        IM_CHECK_EQ(PL.state.load(), 0);
        J.mic_set = false; J.survey[0] = 0;
        ctx->ItemClick("**/trims##cpass");
        ctx->ItemClick("**/ZM-1##cmic");
        ctx->ItemClick("**/##cl");   ctx->KeyCharsReplaceEnter(FIX_S);
        ctx->ItemClick("**/##co");   ctx->KeyCharsReplaceEnter("calibview_zyv_trims.json");
        ctx->ItemCheck("**/simulate");
        ctx->ItemUncheck("**/room##cap");
        cap_run(ctx, "**/Run calibration", 120.0);
        IM_CHECK_EQ(J.state.load(), 2);
        /* the corrupted copy: the same trims, speaker BAD's delay 0.3 ms late */
        static float gdb[BWA_MAX_CHANNELS], dms[BWA_MAX_CHANNELS];
        memcpy(gdb, J.gain_db, sizeof gdb); memcpy(dms, J.trim_ms, sizeof dms);
        dms[BAD] += 0.3f;
        char err[256] = { 0 };
        IM_CHECK(calib_write_layout("calibview_zyv_trims.json", "calibview_zyv_bad.json", gdb, dms, SMALL_N, err, sizeof err));

        ctx->ItemClick("**/Verify this result");                  /* the written file, the verify pass */
        ctx->Yield(2);
        IM_CHECK_EQ(J.pass, 1);
        IM_CHECK_STR_EQ(J.layout, "calibview_zyv_trims.json");
        cap_run(ctx, "**/Run verify", 120.0);
        printf("capture test: ZM-1 verify, clean: state %d, %s; arrival spread %.1f us, level spread %.3f dB\n",
               J.state.load(), J.msg, J.vs.arrival_spread_us, J.vs.level_spread_db);
        IM_CHECK_EQ(J.state.load(), 2);
        IM_CHECK(J.ran_zylia && J.ran_pass == 1);
        float smin = 1e9f;
        for (int i = 0; i < SMALL_N; ++i) if (J.spread_db[i] < smin) smin = J.spread_db[i];
        IM_CHECK_GT(smin, 0.1f);                                  /* the verify pass measured 19 capsules too */
        IM_CHECK_EQ(J.vs.nlive, SMALL_N);
        IM_CHECK_EQ(J.vs.nflag, 0);
        IM_CHECK_LT(J.vs.arrival_spread_us, 50.f);
        IM_CHECK_LT(J.vs.level_spread_db, 0.2f);

        ctx->ItemClick("**/##cl");   ctx->KeyCharsReplaceEnter("calibview_zyv_bad.json");
        cap_run(ctx, "**/Run verify", 120.0);
        printf("capture test: ZM-1 verify, speaker %d +0.3 ms: %s; its residual %+.1f us\n", BAD, J.msg, J.v_arr_us[BAD]);
        IM_CHECK_EQ(J.state.load(), 2);
        IM_CHECK_EQ(J.vs.nflag, 1);
        IM_CHECK(J.v_flag[BAD] & CALIB_VERIFY_FLAG_ARRIVAL);
        IM_CHECK(!(J.v_flag[BAD] & CALIB_VERIFY_FLAG_LEVEL));
        IM_CHECK(J.v_arr_us[BAD] > 250.f && J.v_arr_us[BAD] < 350.f);
        ctx->CaptureScreenshotWindow("//calib view");

        ctx->ItemClick("**/omni##cmic");                          /* the same file, the omni */
        cap_run(ctx, "**/Run verify", 120.0);
        printf("capture test: omni verify, speaker %d +0.3 ms: %s; its residual %+.1f us\n", BAD, J.msg, J.v_arr_us[BAD]);
        IM_CHECK_EQ(J.state.load(), 2);
        IM_CHECK(!J.ran_zylia);
        IM_CHECK_EQ(J.vs.nflag, 1);
        IM_CHECK(J.v_flag[BAD] & CALIB_VERIFY_FLAG_ARRIVAL);
        IM_CHECK(J.v_arr_us[BAD] > 250.f && J.v_arr_us[BAD] < 350.f);
        ctx->ItemClick("**/trims##cpass");                        /* leave the tab the way later tests expect */
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

    /* live aiming reads the PLAN: FIX_P gives speaker 3 a plan_position 60 mm toward +x of its position,
     * and the simulated box stands at the position (the as-built) with no offset. Against the plan the
     * box reads 60 mm toward -x; against its position, the bug this guards, it would read 0. The
     * number is typed here from the fixture, never read back from the tab. */
    t = IM_REGISTER_TEST(e, "aim", "sim_plan");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("calib view");
        ctx->ItemClick("**/##A");
        ctx->KeyCharsReplaceEnter(FIX_P);
        ctx->ItemClick("**/Load A");
        IM_CHECK(V.hasA && V.A.speakers[3].has_plan && fabsf(V.A.speakers[3].plan_pos[0] + 1.44f) < 1e-4f);
        ctx->ItemClick("**/Aim");
        ctx->Yield(2);
        ctx->ItemInputValue("**/speaker##aim", 3);
        ctx->ItemCheck("**/simulate##aim");
        { std::lock_guard<std::mutex> lk(AJ.mu); AJ.d.move[0] = AJ.d.move[1] = AJ.d.move[2] = 0.f; AJ.d.turn_deg = AJ.d.tilt_deg = 0.f; }
        ctx->Yield(2);
        ctx->ItemClick("**/Start##aim");
        int n0; { std::lock_guard<std::mutex> lk(AJ.mu); n0 = AJ.d.n; }
        const double t0 = ImGui::GetTime();
        for (;;) {
            int n; { std::lock_guard<std::mutex> lk(AJ.mu); n = AJ.d.n; }
            if (n >= n0 + 2 || AJ.state.load() != 1 || ImGui::GetTime() - t0 > 60.0) break;
            ctx->Yield();
        }
        AimData d; { std::lock_guard<std::mutex> lk(AJ.mu); d = AJ.d; }
        printf("aim test: plan: read %+.1f %+.1f %+.1f mm against the plan (want -60 0 0)\n", d.lp.delta_mm[0], d.lp.delta_mm[1], d.lp.delta_mm[2]);
        IM_CHECK(d.have_pos && d.lp.have_distance);
        IM_CHECK_LT(fabsf(d.lp.delta_mm[0] + 60.f), 8.f);          /* the box sits 60 mm toward -x of its plan */
        IM_CHECK_LT(fabsf(d.lp.delta_mm[1]) + fabsf(d.lp.delta_mm[2]), 10.f);
        ctx->ItemClick("**/Stop##aim");
        const double t1 = ImGui::GetTime();
        while (AJ.state.load() == 1 && ImGui::GetTime() - t1 < 30.0) ctx->Yield();
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
        cap_omni_trims(ctx);
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
        cap_omni_trims(ctx);
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

    /* A tracked ZM-1 run takes its capsule table from the Placement panel, by mic_track's rules: a
     * BODY-FRAME survey there is re-aimed by the pose the gate accepted, and the tab's own survey field
     * is ignored (here it names a file that does not exist, so a run that read it would fail). The run
     * solves at the measured center, lands on the simulator's truth, and puts the table back the way
     * it found it, so the Zylia and Aim tabs never inherit a run's table. */
    t = IM_REGISTER_TEST(e, "placement", "zylia_survey_rules");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        static const char* SURV = "calibview_body_survey.json";
        float caps[ZYLIA_MICS][3];
        zylia_set_capsules(NULL);
        zylia_capsules(caps);                                     /* the built-in table, as a body-frame survey */
        ZyliaMount m;
        memset(&m, 0, sizeof m);
        m.body_frame = 1;                                         /* no offset of its own: the panel's field (0) */
        char e[256] = { 0 };
        IM_CHECK(zylia_survey_save(SURV, caps, 0.5f, 0.049f, 0.9f, 14, &m, e, sizeof e));
        ctx->SetRef("calib view");
        ctx->ItemClick("**/Capture");
        ctx->Yield(2);
        ctx->ItemOpen("**/Placement (tracked ZM-1)");
        ctx->Yield(2);
        if (PL.state.load() == 2) { ctx->ItemClick("**/Disconnect##pl"); pl_wait_state(ctx, 0, 10.0); }
        IM_CHECK_EQ(PL.state.load(), 0);
        ctx->ItemCheck("**/simulate##pl");
        ctx->ItemUncheck("**/bump mid-run##pl");
        ctx->ItemClick("**/##plsurvey"); ctx->KeyCharsReplaceEnter(SURV);
        ctx->ItemClick("**/Connect##pl");
        pl_wait_state(ctx, 2, 10.0);
        IM_CHECK_EQ(PL.state.load(), 2);
        IM_CHECK(PL.body_frame);
        J.mic_set = false;
        snprintf(J.survey, sizeof J.survey, "calibview_no_such_survey.json");
        ctx->ItemClick("**/trims##cpass");
        ctx->ItemClick("**/ZM-1##cmic");
        ctx->ItemClick("**/##cl");   ctx->KeyCharsReplaceEnter(FIX_S);
        ctx->ItemClick("**/##co");   ctx->KeyCharsReplaceEnter("calibview_zy_track.json");
        ctx->ItemCheck("**/simulate");
        ctx->ItemUncheck("**/room##cap");
        float before[ZYLIA_MICS][3], after[ZYLIA_MICS][3];
        zylia_capsules(before);
        cap_run(ctx, "**/Run calibration", 180.0);
        zylia_capsules(after);
        printf("placement test: tracked ZM-1 run: state %d, %s; capsule table source %d\n", J.state.load(), J.msg, J.table_src);
        IM_CHECK_EQ(J.state.load(), 2);
        IM_CHECK(J.tk.used);
        IM_CHECK_EQ(J.table_src, 3);                              /* the panel's body-frame table, re-aimed */
        IM_CHECK_LT(dist3(J.mic_run, J.tk.truth), 0.001f);
        IM_CHECK(memcmp(before, after, sizeof before) == 0);      /* put back */
        float wdb, wus;
        zy_truth_errors(FIX_S, J.tk.truth, &wdb, &wus);
        IM_CHECK_LT(wdb, 0.05f);
        IM_CHECK_LT(wus, 1e6f / (float)CAL_FS + 5.f);             /* a sample, plus the 1 mm the take may miss by */
        ctx->CaptureScreenshotWindow("//calib view");
        ctx->ItemClick("**/Disconnect##pl");
        pl_wait_state(ctx, 0, 10.0);
        IM_CHECK_EQ(PL.state.load(), 0);
        PL.survey[0] = 0; J.survey[0] = 0;                        /* leave no survey behind */
        zylia_set_capsules(NULL);
        ctx->ItemClick("**/omni##cmic");
    };

    /* The stand TWISTED 2 deg about the array center after the third capture (mic_track's script: no
     * center moves). The Aim tab's position readout turns capsule arrival differences into a direction,
     * so it stops on the twist; a ZM-1 trim run reads only the center, so the same twist must NOT stop
     * it. Each half gets its own connection, because the twist is keyed on the connection's capture
     * count. That the twist happened in the run that did not stop comes from the script's own record
     * (sim_twisted), never from placement.c. */
    t = IM_REGISTER_TEST(e, "placement", "twist");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        static const char* SURV = "calibview_body_survey_twist.json";
        float caps[ZYLIA_MICS][3];
        zylia_set_capsules(NULL);
        zylia_capsules(caps);                                     /* the built-in table, as a body-frame survey */
        ZyliaMount m;
        memset(&m, 0, sizeof m);
        m.body_frame = 1;
        char e[256] = { 0 };
        IM_CHECK(zylia_survey_save(SURV, caps, 0.5f, 0.049f, 0.9f, 14, &m, e, sizeof e));
        ctx->SetRef("calib view");
        ctx->ItemClick("**/Capture");
        ctx->Yield(2);
        ctx->ItemOpen("**/Placement (tracked ZM-1)");
        ctx->Yield(2);
        if (PL.state.load() == 2) { ctx->ItemClick("**/Disconnect##pl"); pl_wait_state(ctx, 0, 10.0); }
        IM_CHECK_EQ(PL.state.load(), 0);
        ctx->ItemCheck("**/simulate##pl");
        ctx->ItemUncheck("**/bump mid-run##pl");
        ctx->ItemCheck("**/twist mid-run##pl");
        ctx->ItemClick("**/##plsurvey"); ctx->KeyCharsReplaceEnter(SURV);
        ctx->ItemClick("**/Connect##pl");
        pl_wait_state(ctx, 2, 10.0);
        IM_CHECK_EQ(PL.state.load(), 2);
        IM_CHECK(PL.body_frame);

        /* 1. the Aim tab with its position readout on: the twist stops the run */
        ctx->ItemClick("**/##A");
        ctx->KeyCharsReplaceEnter(FIX_A);
        ctx->ItemClick("**/Load A");
        ctx->ItemClick("**/Aim");
        ctx->Yield(2);
        ctx->ItemCheck("**/simulate##aim");
        ctx->ItemInputValue("**/speaker##aim", 3);
        ctx->ItemClick("**/Start##aim");
        double t0 = ImGui::GetTime();
        while (AJ.state.load() == 1 && ImGui::GetTime() - t0 < 120.0) ctx->Yield();
        AimData d; { std::lock_guard<std::mutex> lk(AJ.mu); d = AJ.d; }
        printf("placement test: twisted Aim run: state %d, %s; turned %.2f deg (limit %.2f), moved %.2f mm, after reading %d\n",
               AJ.state.load(), AJ.msg, d.tk.turn_deg, d.tk.turn_limit_deg, d.tk.bump_mm, d.tk.bump_after);
        IM_CHECK_EQ(AJ.state.load(), 3);                          /* stopped */
        IM_CHECK(d.tk.used && d.tk.turn_limit_deg >= PLACE_TURN_MIN_DEG);   /* a direction run */
        IM_CHECK(d.have_pos);                                     /* ...whose position readout was on */
        IM_CHECK(d.tk.bumped && d.tk.turned);                     /* stopped by the TURN */
        IM_CHECK_EQ(d.tk.bump_after, PL_SIM_BUMP_AFTER);          /* readings count from 1: right after the third */
        IM_CHECK_LT(fabsf(d.tk.turn_deg - MIC_SIM_TWIST_DEG), 0.1f);
        IM_CHECK_LT(d.tk.bump_mm, 1.f);                           /* the center stayed put */
        IM_CHECK(d.tk.sim_twisted);
        IM_CHECK(strstr(AJ.msg, "turned") != NULL);
        ctx->CaptureScreenshotWindow("//calib view");

        /* 2. a fresh connection (the capture count restarts), a ZM-1 trim run: the same twist must not stop it */
        ctx->ItemClick("**/Capture");
        ctx->Yield(2);
        ctx->ItemClick("**/Disconnect##pl");
        pl_wait_state(ctx, 0, 10.0);
        IM_CHECK_EQ(PL.state.load(), 0);
        ctx->ItemClick("**/Connect##pl");
        pl_wait_state(ctx, 2, 10.0);
        IM_CHECK_EQ(PL.state.load(), 2);
        J.mic_set = false;
        ctx->ItemClick("**/trims##cpass");
        ctx->ItemClick("**/ZM-1##cmic");
        ctx->ItemClick("**/##cl");   ctx->KeyCharsReplaceEnter(FIX_S);
        ctx->ItemClick("**/##co");   ctx->KeyCharsReplaceEnter("calibview_twist_trims.json");
        ctx->ItemCheck("**/simulate");
        ctx->ItemUncheck("**/room##cap");
        cap_run(ctx, "**/Run calibration", 180.0);
        printf("placement test: twisted trim run: state %d, %s; largest turn %.2f deg, twisted %d\n",
               J.state.load(), J.msg, J.tk.max_turn_deg, (int)J.tk.sim_twisted);
        IM_CHECK_EQ(J.state.load(), 2);                           /* finished */
        IM_CHECK(J.tk.used && J.tk.turn_limit_deg == 0.f && !J.tk.bumped);   /* center only, not stopped */
        IM_CHECK(J.tk.sim_twisted);                               /* the script did twist it mid-run */
        IM_CHECK_GT(J.tk.max_turn_deg, 1.5f);                     /* ...and the stand read turned */
        ctx->CaptureScreenshotWindow("//calib view");
        ctx->ItemClick("**/Disconnect##pl");
        pl_wait_state(ctx, 0, 10.0);
        IM_CHECK_EQ(PL.state.load(), 0);
        ctx->ItemUncheck("**/twist mid-run##pl");
        PL.survey[0] = 0;                                         /* leave no survey behind */
        zylia_set_capsules(NULL);
        ctx->ItemClick("**/omni##cmic");
    };

    /* ---- the tracked clicker (clicker_track.h), simulated: a scripted clicker walks to 10 spots high and low
     * around the array, holds still at each and clicks, and once clicks while waving. Each clap is synthesized
     * from the script's TRUE tip against the TRUE array center, both from code apart from what is under test
     * (the script's own quaternions and tip offset, mic_track's own stand truth), so a tool that banked the
     * typed field, the pivot, the wrong body, or the typed center lands off the truth. */
    /* the right tip, with the stand tracked: every banked clap is the clicker's TRUE tip, measured against
     * the stand's MEASURED center (not either typed field), the waved click is refused, and the survey
     * recovers the table the claps were synthesized from */
    t = IM_REGISTER_TEST(e, "zylia", "clicker_survey");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("calib view");
        ctx->ItemClick("**/Zylia");
        ctx->Yield(2);
        ctx->ItemCheck("**/simulate claps");
        ctx->ItemOpen("**/Capsule survey");
        ctx->Yield(2);
        /* typed values the run must NOT use: far from the stand and from every spot the clicker visits */
        Z.surv_center[0] = 1.0f; Z.surv_center[1] = 0.5f; Z.surv_center[2] = 1.0f;
        Z.surv_clap[0] = 9.0f; Z.surv_clap[1] = 9.0f; Z.surv_clap[2] = 9.0f;
        ctx->ItemClick("**/typed##zsrc");
        ctx->Yield(2);
        ctx->ItemOpen("**/Placement (tracked ZM-1)");
        ctx->Yield(2);
        if (PL.state.load() == 2) { ctx->ItemClick("**/Disconnect##pl"); pl_wait_state(ctx, 0, 10.0); }
        IM_CHECK_EQ(PL.state.load(), 0);
        ctx->ItemCheck("**/simulate##pl");
        ctx->ItemUncheck("**/bump mid-run##pl");
        ctx->ItemUncheck("**/twist mid-run##pl");
        ctx->ItemClick("**/Connect##pl");
        pl_wait_state(ctx, 2, 10.0);
        IM_CHECK_EQ(PL.state.load(), 2);
        double t0 = clicker_clock_s();
        while (pl_snap().gd.state < PLACE_OFF_TARGET && clicker_clock_s() - t0 < 20.0) ctx->Yield();
        IM_CHECK_GE((int)pl_snap().gd.state, (int)PLACE_OFF_TARGET);   /* the stand has settled */

        ctx->ItemClick("**/tracked clicker##zsrc");
        ctx->Yield(2);
        ctx->ItemCheck("**/simulate##ck");                         /* the button shows in simulate only */
        ctx->ItemClick("**/the simulated tip##ck");
        ctx->Yield(1);
        IM_CHECK_EQ(Z.ck_tip[2], CLICKER_SIM_TIP_Z);
        ck_run_survey(ctx, false);
        static PlaceSnap s;
        s = pl_snap();
        IM_CHECK(s.have_truth);
        float worst_tip = 0.f, worst_center = 0.f;
        for (int k = 0; k < Z.surv_n; ++k) {
            float at[3];
            for (int a = 0; a < 3; ++a) at[a] = Z.surv_src[k][a] + Z.surv_center_used[k][a];
            const float dt = dist3(at, Z.surv_truth[k]), dc = dist3(Z.surv_center_used[k], s.truth);
            if (dt > worst_tip) worst_tip = dt;
            if (dc > worst_center) worst_center = dc;
            IM_CHECK_GT(dist3(at, Z.surv_clap), 1.0f);             /* not the typed clap position */
            IM_CHECK_GT(dist3(Z.surv_center_used[k], Z.surv_center), 0.5f);   /* nor the typed center */
        }
        printf("clicker test: right tip, tracked stand: %d banked, %d refused (%s); worst clap %.2f mm off the true tip, "
               "worst center %.2f mm off the stand's truth\n", Z.surv_n, Z.surv_refused, Z.surv_why, worst_tip * 1e3f, worst_center * 1e3f);
        IM_CHECK_EQ(Z.surv_n, 10);                                 /* every still click banked */
        IM_CHECK_EQ(Z.surv_refused, 1);                            /* the waved one refused... */
        IM_CHECK(strstr(Z.surv_why, "moving") != NULL);           /* ...for moving: the arm closes while it moves */
        IM_CHECK_EQ(Z.ref_kind[ZY_REF_ARM], 1);
        IM_CHECK_LT(worst_tip, 0.002f);                            /* the clicker's TRUE tip */
        IM_CHECK_LT(worst_center, 0.001f);                         /* against the stand's TRUE center */
        for (int k = 0; k < Z.surv_n; ++k) IM_CHECK(Z.surv_has_truth[k] && Z.surv_center_tracked[k]);

        ctx->ItemClick("**/Solve");
        ctx->Yield(2);
        g_ck_good_resid = Z.surv_resid;
        g_ck_good_geom = ck_geom_err();
        printf("clicker test: right tip: solved %d, residual %.3f us, radius %.2f mm, spread %.2f, worst capsule %.3f mm\n",
               (int)Z.surv_solved, Z.surv_resid, Z.surv_radius * 1e3f, Z.surv_spread, g_ck_good_geom * 1e3f);
        IM_CHECK(Z.surv_solved);
        IM_CHECK_LT(Z.surv_resid, 1.0f);
        IM_CHECK_LT(g_ck_good_geom, 0.0005f);                      /* the table the claps came from */
        {   /* Save writes the BODY frame (every clap was tracked). Reloaded and re-aimed at the stand's
             * pose it must give back the room-axes truth, the built-in table the claps came from. A save
             * that rotated by R instead of R^T lands 2 x 30 deg of yaw away. */
            static const char* BF = "calibview_clicker_body.json";
            remove(BF);
            snprintf(Z.surv_path, sizeof Z.surv_path, "%s", BF);
            ctx->ItemClick("**/Save");
            ctx->Yield(2);
            IM_CHECK(strstr(Z.surv_msg, "BODY frame") != NULL);
            ZyliaMount m; char le[160] = { 0 };
            IM_CHECK(zylia_survey_load(BF, &m, le, sizeof le));
            IM_CHECK(m.body_frame && m.have_offset);
            float body[ZYLIA_MICS][3], room[ZYLIA_MICS][3], ref[ZYLIA_MICS][3], Rr, R[9], worst = 0.f;
            zylia_capsules(body);                                  /* what the load installed: body axes */
            zylia_quat_to_matrix(Z.surv_q0, R);
            zylia_capsules_rotate(body, R, 0, room);
            zylia_set_capsules(NULL);
            zylia_geometry(ref, &Rr);
            for (int i = 0; i < ZYLIA_MICS; ++i) {
                const float tr[3] = { Rr * ref[i][0], Rr * ref[i][1], Rr * ref[i][2] };
                const float d = dist3(room[i], tr);
                if (d > worst) worst = d;
            }
            printf("clicker test: body-frame save re-aimed at the stand's pose: worst capsule %.3f mm off the truth\n", worst * 1e3f);
            IM_CHECK_LT(worst, 0.0005f);
            remove(BF);
            Z.surv_path[0] = 0;
        }
        ctx->CaptureScreenshotWindow("//calib view");
        ctx->ItemClick("**/Disconnect##pl");                       /* the next two take the typed center */
        pl_wait_state(ctx, 0, 10.0);
        IM_CHECK_EQ(PL.state.load(), 0);
        zylia_set_capsules(NULL);
    };

    /* the tip offset left at 0 (the pivot taken as the tip) on a clicker whose tip is 11.6 cm out: every
     * clap is off by that much, in a different direction at each spot because the clicker is held at a
     * different angle, so the survey's residual and geometry both get worse, by a margin */
    t = IM_REGISTER_TEST(e, "zylia", "clicker_wrong_tip");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("calib view");
        ctx->ItemClick("**/Zylia");
        ctx->Yield(2);
        ctx->ItemCheck("**/simulate claps");
        ctx->ItemOpen("**/Capsule survey");
        ctx->Yield(2);
        IM_CHECK_EQ(PL.state.load(), 0);                           /* the typed center: simulate takes it as true */
        ctx->ItemClick("**/tracked clicker##zsrc");
        ctx->Yield(2);
        Z.ck_tip[0] = Z.ck_tip[1] = Z.ck_tip[2] = 0.f;             /* the operator forgot the tip */
        ck_run_survey(ctx, false);
        float best_tip = 1e9f;
        for (int k = 0; k < Z.surv_n; ++k) {
            float at[3];
            for (int a = 0; a < 3; ++a) at[a] = Z.surv_src[k][a] + Z.surv_center_used[k][a];
            const float d = dist3(at, Z.surv_truth[k]);
            if (d < best_tip) best_tip = d;
        }
        ctx->ItemClick("**/Solve");
        ctx->Yield(2);
        const float geom = ck_geom_err();
        printf("clicker test: tip ignored: %d banked, every clap at least %.1f mm off its true tip; solved %d, residual %.3f us "
               "(right tip %.3f), worst capsule %.3f mm (right tip %.3f)\n", Z.surv_n, best_tip * 1e3f, (int)Z.surv_solved,
               Z.surv_resid, g_ck_good_resid, geom * 1e3f, g_ck_good_geom * 1e3f);
        IM_CHECK_EQ(Z.surv_n, 10);                                 /* the pivot held still too: nothing refused but the wave */
        IM_CHECK_GT(best_tip, 0.05f);                              /* the banked positions are the pivot's */
        IM_CHECK(g_ck_good_resid >= 0.f);                          /* clicker_survey ran first */
        IM_CHECK(Z.surv_solved);
        IM_CHECK_GT(Z.surv_resid, 3.0f * g_ck_good_resid + 1.0f);  /* the residual says so... */
        IM_CHECK_GT(geom, 5.0f * g_ck_good_geom + 0.001f);         /* ...and the geometry is off */
        ctx->CaptureScreenshotWindow("//calib view");
        zylia_set_capsules(NULL);
    };

    /* the clicker in a ring at the array's height: coplanar, so the survey refuses rather than hand back a
     * flattened array */
    t = IM_REGISTER_TEST(e, "zylia", "clicker_coplanar");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("calib view");
        ctx->ItemClick("**/Zylia");
        ctx->Yield(2);
        ctx->ItemCheck("**/simulate claps");
        ctx->ItemOpen("**/Capsule survey");
        ctx->Yield(2);
        ctx->ItemClick("**/tracked clicker##zsrc");
        ctx->Yield(2);
        ctx->ItemCheck("**/simulate##ck");                         /* the button shows in simulate only */
        ctx->ItemClick("**/the simulated tip##ck");
        ck_run_survey(ctx, true);
        ctx->ItemClick("**/Solve");
        ctx->Yield(2);
        printf("clicker test: coplanar ring: %d banked, %d refused; solved %d, spread %.4f\n", Z.surv_n, Z.surv_refused,
               (int)Z.surv_solved, Z.surv_spread);
        IM_CHECK(!Z.surv_solved);                                  /* refused */
        IM_CHECK_LT(Z.surv_spread, 0.05f);                         /* for the spread */
        IM_CHECK(strstr(Z.surv_msg, "refused") != NULL);
        IM_CHECK_EQ(Z.surv_n, 8);                                  /* every ring click banked */
        IM_CHECK_EQ(Z.surv_refused, 0);
        ctx->CaptureScreenshotWindow("//calib view");
        ctx->ItemUncheck("**/coplanar ring##ck");                  /* leave the tab as the other tests expect */
        ctx->ItemClick("**/typed##zsrc");
        zylia_set_capsules(NULL);
    };

    /* ---- which transient is the clap? (ZY_ARM_WINDOW_S) ----
     * Typed: arm, then clap. A clap with no arm, a second one in the window, one before the lead-in ends and
     * one after the window closes are refused; once 6 claps are banked, a clap whose arrivals come from 60 deg
     * away from its typed position is refused for its direction and hands the window back, so the real clap
     * after it still banks. Deterministic: "Clap now" synthesizes in the frame it is clicked. */
    t = IM_REGISTER_TEST(e, "zylia", "typed_arm");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("calib view");
        ctx->ItemClick("**/Zylia");
        ctx->Yield(2);
        ctx->ItemCheck("**/simulate claps");
        ctx->ItemUncheck("**/walk");
        ctx->ItemOpen("**/Capsule survey");
        ctx->Yield(2);
        IM_CHECK_EQ(PL.state.load(), 0);                           /* the typed center: simulate takes it as true */
        ctx->ItemClick("**/typed##zsrc");
        ctx->Yield(2);
        ctx->ItemClick("**/Clear");
        ctx->ItemCheck("**/record claps");
        zylia_set_capsules(NULL);                                  /* the synthesis is the built-in table */
        zylia_geometry(Z.dirs, &Z.R);
        Z.surv_installed = false;
        Z.arm_lead_s = 0.f;
        Z.surv_center[0] = 0.2f; Z.surv_center[1] = 1.1f; Z.surv_center[2] = -0.3f;
        static const float D[8][3] = { { 0.6f, 0.3f, -0.7f }, { -0.5f, -0.4f, -0.6f }, { 0.9f, -0.2f, 0.3f },
                                       { -0.2f, 0.8f, 0.4f }, { -0.7f, 0.1f, 0.7f }, { 0.3f, -0.8f, 0.5f },
                                       { 0.1f, 0.2f, 0.95f }, { -0.9f, 0.4f, -0.1f } };
        ta_aim(D[0], D[0]);
        ctx->ItemClick("**/Clap now");                             /* not armed */
        ctx->Yield(2);
        IM_CHECK_EQ(Z.surv_n, 0);
        IM_CHECK_EQ(Z.ref_kind[ZY_REF_ARM], 1);
        ctx->ItemClick("**/Arm for the next clap");
        ctx->ItemClick("**/Clap now");                             /* the window's first */
        ctx->Yield(2);
        IM_CHECK_EQ(Z.surv_n, 1);
        ctx->ItemClick("**/Clap now");                             /* its second */
        ctx->Yield(2);
        IM_CHECK_EQ(Z.surv_n, 1);
        IM_CHECK_EQ(Z.ref_kind[ZY_REF_SECOND], 1);
        Z.arm_lead_s = 0.5f;                                       /* a lead-in: refused until it ends */
        ta_aim(D[1], D[1]);
        ctx->ItemClick("**/Arm for the next clap");
        ctx->ItemClick("**/Clap now");
        ctx->Yield(2);
        IM_CHECK_EQ(Z.surv_n, 1);
        IM_CHECK_EQ(Z.ref_kind[ZY_REF_ARM], 2);
        IM_CHECK(strstr(Z.surv_why, "not armed yet") != NULL);
        ta_wait(ctx, 0.6);
        ctx->ItemClick("**/Clap now");
        ctx->Yield(2);
        IM_CHECK_EQ(Z.surv_n, 2);
        Z.arm_lead_s = 0.f;
        ta_aim(D[2], D[2]);                                        /* the window runs out */
        ctx->ItemClick("**/Arm for the next clap");
        ta_wait(ctx, ZY_ARM_WINDOW_S + 0.1);
        ctx->ItemClick("**/Clap now");
        ctx->Yield(2);
        IM_CHECK_EQ(Z.surv_n, 2);
        IM_CHECK_EQ(Z.ref_kind[ZY_REF_ARM], 3);
        IM_CHECK(strstr(Z.surv_why, "closed") != NULL);
        for (int k = 2; k < 8; ++k) {                              /* to 8 banked: the direction check is on */
            ta_aim(D[k], D[k]);
            ctx->ItemClick("**/Arm for the next clap");
            ctx->ItemClick("**/Clap now");
            ctx->Yield(2);
        }
        IM_CHECK_EQ(Z.surv_n, 8);
        IM_CHECK(Z.prov_ok);
        /* typed at one spot, the sound from 60 deg away: refused for its direction */
        const float typed[3] = { 0.5f, 0.2f, -0.8f };
        const float c60 = 0.5f, s60 = 0.8660254f;
        float other[3];
        ta_unit(typed, other);
        const float ox = other[0], oz = other[2];
        other[0] = c60 * ox + s60 * oz; other[2] = -s60 * ox + c60 * oz;
        ta_aim(other, typed);
        ctx->ItemClick("**/Arm for the next clap");
        ctx->ItemClick("**/Clap now");
        ctx->Yield(2);
        printf("typed arm test: %d banked; refused %d not armed, %d second, %d direction (that one %.1f deg off); "
               "worst banked %.2f deg off its typed direction\n", Z.surv_n, Z.ref_kind[ZY_REF_ARM], Z.ref_kind[ZY_REF_SECOND],
               Z.ref_kind[ZY_REF_DIR], Z.dir_refused_deg, Z.dir_worst_ok_deg);
        IM_CHECK_EQ(Z.surv_n, 8);
        IM_CHECK_EQ(Z.ref_kind[ZY_REF_DIR], 1);
        IM_CHECK_GT(Z.dir_refused_deg, 50.f);
        IM_CHECK_LT(Z.dir_worst_ok_deg, 0.25f * ZY_DIR_TOL_DEG);
        ta_aim(typed, typed);                                      /* the real clap: the window is still open for it */
        ctx->ItemClick("**/Clap now");
        ctx->Yield(2);
        IM_CHECK_EQ(Z.surv_n, 9);
        IM_CHECK_EQ(Z.ref_kind[ZY_REF_SECOND], 1);
        ctx->ItemClick("**/Clear");
        ctx->ItemUncheck("**/record claps");
        zylia_set_capsules(NULL);
    };

    /* the tracked clicker with interferers from 60 deg away (clicker_track.h), synthesized through the same
     * snapshot path from the script's own truth, while the clicker holds still at a spot. One lands right
     * after the second click (a second transient in that window); one is timed like the eighth click, after
     * the provisional survey exists (refused for its direction, and the real click after it banks). The
     * survey must come out as a clean one does: the script's 10 still clicks banked, and the table the claps
     * were synthesized from within 0.2 mm. That bound is the clean run's own: a clean typed-center survey
     * lands about 0.03 mm off the table, and this test used to run one first and demand 0.2 mm against it,
     * which cost a whole scripted survey and could not see a solve that is off the table in both runs. One
     * banked interferer puts the table about 10 mm off (clicker_interferer_slip). */
    t = IM_REGISTER_TEST(e, "zylia", "clicker_interferer");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("calib view");
        ctx->ItemClick("**/Zylia");
        ctx->Yield(2);
        ctx->ItemCheck("**/simulate claps");
        ctx->ItemOpen("**/Capsule survey");
        ctx->Yield(2);
        IM_CHECK_EQ(PL.state.load(), 0);                           /* the typed center: simulate takes it as true */
        ctx->ItemClick("**/tracked clicker##zsrc");
        ctx->Yield(2);
        ctx->ItemCheck("**/simulate##ck");
        ctx->ItemClick("**/the simulated tip##ck");
        ctx->ItemCheck("**/interferers##ck");
        IM_CHECK_EQ(Z.ck_sim_if_set, 1);
        ck_run_survey(ctx, false);
        int nif = 0, last = -1;
        const float worst_tip = ck_banked_vs_truth(&nif, &last);
        printf("clicker interferer test: %d banked (want 10), %d interferers among them; refused %d not armed, %d second, "
               "%d direction (%.1f deg off), %d clicker; worst banked clap %.2f mm off its true tip, worst direction "
               "%.2f deg\n", Z.surv_n, nif, Z.ref_kind[ZY_REF_ARM], Z.ref_kind[ZY_REF_SECOND], Z.ref_kind[ZY_REF_DIR],
               Z.dir_refused_deg, Z.ref_kind[ZY_REF_TAKE], worst_tip * 1e3f, Z.dir_worst_ok_deg);
        IM_CHECK_EQ(Z.surv_n, 10);                                 /* every still click banked... */
        IM_CHECK_EQ(nif, 0);                                       /* ...and no interferer */
        IM_CHECK_LT(worst_tip, 0.002f);
        IM_CHECK_EQ(Z.ref_kind[ZY_REF_SECOND], 1);                 /* the one after the second click */
        IM_CHECK_EQ(Z.ref_kind[ZY_REF_DIR], 1);                    /* the one timed like the eighth */
        IM_CHECK_EQ(Z.ref_kind[ZY_REF_ARM], 1);                    /* the waved click */
        IM_CHECK_EQ(Z.surv_refused, 3);
        IM_CHECK_GT(Z.dir_refused_deg, 30.f);
        IM_CHECK_LT(Z.dir_worst_ok_deg, 0.25f * ZY_DIR_TOL_DEG);   /* genuine clicks sit far inside the tolerance */
        zy_reach(ctx, "**/Solve");
        ctx->ItemClick("**/Solve");
        ctx->Yield(2);
        IM_CHECK(Z.surv_solved);
        const float geom = ck_geom_err();
        printf("clicker interferer test: solved, residual %.3f us, worst capsule %.3f mm off the truth; leave-one-out flags %d\n",
               Z.surv_resid, geom * 1e3f, Z.loo_nflag);
        IM_CHECK_LT(geom, 0.0002f);                                /* a clean survey's geometry */
        IM_CHECK_EQ(Z.loo_nflag, 0);
        ctx->CaptureScreenshotWindow("//calib view");
        ctx->ItemUncheck("**/interferers##ck");
        zylia_set_capsules(NULL);
    };

    /* an interferer that slips past both: timed exactly like the third click, before any provisional survey,
     * so it banks at the clicker's still tip and the real click after it is refused as the window's second.
     * The provisional survey leaves it out once there is one, leave-one-out flags it on Solve, and dropping
     * it restores the geometry */
    t = IM_REGISTER_TEST(e, "zylia", "clicker_interferer_slip");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("calib view");
        ctx->ItemClick("**/Zylia");
        ctx->Yield(2);
        ctx->ItemCheck("**/simulate claps");
        ctx->ItemOpen("**/Capsule survey");
        ctx->Yield(2);
        IM_CHECK_EQ(PL.state.load(), 0);
        ctx->ItemClick("**/tracked clicker##zsrc");
        ctx->Yield(2);
        ctx->ItemCheck("**/simulate##ck");
        ctx->ItemClick("**/the simulated tip##ck");
        Z.ck_sim_if_set = 2;                                       /* not a checkbox: the slip is a test case */
        ck_run_survey(ctx, false);
        Z.ck_sim_if_set = 0;
        int nif = 0, bad = -1;
        const float worst_tip = ck_banked_vs_truth(&nif, &bad);
        printf("clicker slip test: %d banked, %d interferers among them (clap %d); refused %d not armed, %d second, "
               "%d direction; the provisional survey leaves out %d; worst click %.2f mm off its true tip\n", Z.surv_n, nif,
               bad + 1, Z.ref_kind[ZY_REF_ARM], Z.ref_kind[ZY_REF_SECOND], Z.ref_kind[ZY_REF_DIR], Z.prov_out, worst_tip * 1e3f);
        IM_CHECK_EQ(Z.surv_n, 10);
        IM_CHECK_EQ(nif, 1);                                       /* it slipped past the arm */
        IM_CHECK_EQ(Z.ref_kind[ZY_REF_SECOND], 1);                 /* the real click after it */
        IM_CHECK_EQ(Z.ref_kind[ZY_REF_DIR], 0);                    /* nothing to check it against yet */
        IM_CHECK_EQ(Z.prov_out, 1);                                /* the provisional survey leaves it out */
        IM_CHECK(Z.prov_ok);
        zy_reach(ctx, "**/Solve");
        ctx->ItemClick("**/Solve");
        ctx->Yield(2);
        IM_CHECK(Z.surv_solved);
        const float resid_with = Z.surv_resid, geom_with = ck_geom_err();
        IM_CHECK(bad >= 0);
        if (bad < 0) return;
        printf("clicker slip test: with it, residual %.3f us, worst capsule %.3f mm; leave-one-out flags %d, clap %d held "
               "out %.1f us against the rest's %.3f us\n", resid_with, geom_with * 1e3f, Z.loo_nflag, bad + 1,
               Z.loo[bad].heldout_us, Z.loo[bad].resid_us);
        IM_CHECK_EQ(Z.loo_nflag, 1);
        IM_CHECK(Z.loo[bad].flagged);
        char lbl[64];
        snprintf(lbl, sizeof lbl, "**/Drop clap %d", bad + 1);
        zy_reach(ctx, lbl);
        ctx->ItemClick(lbl);
        ctx->Yield(2);
        const float geom = ck_geom_err();
        nif = 0;
        for (int k = 0; k < Z.surv_n; ++k) nif += Z.surv_interf[k];
        printf("clicker slip test: dropped: %d claps, residual %.3f us, worst capsule %.3f mm (%.3f mm with it), flags %d\n",
               Z.surv_n, Z.surv_resid, geom * 1e3f, geom_with * 1e3f, Z.loo_nflag);
        IM_CHECK_EQ(Z.surv_n, 9);
        IM_CHECK_EQ(nif, 0);
        IM_CHECK(Z.surv_solved);
        IM_CHECK_LT(geom, 0.0005f);                                /* the table the clicks came from */
        IM_CHECK_LT(5.0f * geom, geom_with);                       /* ...and it was not, with the interferer */
        IM_CHECK_EQ(Z.loo_nflag, 0);
        ctx->CaptureScreenshotWindow("//calib view");
        zylia_set_capsules(NULL);
    };

    /* ---- the onset stamp (zylia_capture.h, "THE ONSET STAMP") ----
     * The simulated session publishes every scripted clap ZY_SIM_HITCH_S (80 ms) later than the capture
     * would, as a UI hitch does. Judged at its stamp, every still click banks at its true tip; judged at the
     * old estimate (the noticing frame less the post-roll), the onset lands 80 ms after the click, where the
     * hand's next walk has already closed the arm: every click that is followed by a move is refused. The
     * last one is not, because the script leaves the clicker still at its last spot, so the estimate run
     * may bank that one, and it must bank it at the true tip. */
    /* the capture callback's onset arithmetic and its driver-stamp classifier, which only the rig runs live:
     * fed the three clock bases a driver can stamp on (zylia_capture.h, "THE ONSET STAMP") */
    t = IM_REGISTER_TEST(e, "zylia", "onset_stamp_math");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        IM_UNUSED(ctx);
        const double FS = 48000.0;
        /* a block's first frame is in_lat frames old at its switch; none given = one block */
        IM_CHECK_LT(fabs(zp_onset_s(10.0, 100, 256, 0, FS) - (10.0 - 156.0 / FS)), 1e-9);
        IM_CHECK_LT(fabs(zp_onset_s(10.0, 100, 256, 512, FS) - (10.0 - 412.0 / FS)), 1e-9);
        IM_CHECK_LT(fabs(zp_onset_s(10.0, 255, 256, 256, FS) - (10.0 - 1.0 / FS)), 1e-9);   /* the last frame */
        /* the stamps: blocks of 256 at 48 kHz, the callback 80 to 400 us after the switch, timeGetTime 27.3 ms
         * behind QPC (this box's offset) at a 1 ms tick */
        const int64_t BLK = (int64_t)(256.0 / FS * 1e9), OFF = 27300000, T0 = 40000000000000LL;
        int got[3] = { -1, -1, -1 }, early = -1;
        int64_t lag = 0;
        for (int base = 0; base < 3; ++base) {                     /* 0: QPC stamps, 1: timeGetTime stamps, 2: 1 s off */
            SinkTsClass c;
            sink_ts_init(&c, 188);
            for (int b = 0; b < 400; ++b) {
                const int64_t sw = T0 + b * BLK;                     /* the switch, on QPC */
                const int64_t host = sw + 80000 + (int64_t)((b * 7919) % 320) * 1000;
                const int64_t tgt = ((host - OFF) / 1000000) * 1000000;            /* ms since boot, read at entry */
                const int64_t sys = base == 0 ? sw : base == 1 ? ((sw - OFF) / 1000000) * 1000000 : sw - 1000000000;
                const int r = sink_ts_note(&c, host, tgt, sys);
                if (b == 5 && base == 0) early = r;
                got[base] = r;
            }
            if (base == 0) lag = sink_ts_lag_ns(&c);
        }
        printf("onset stamp math: base QPC -> %d, timeGetTime -> %d, 1 s off -> %d (HOST %d, TGT %d, UNKNOWN %d); "
               "after 6 blocks %d; QPC dispatch floor %.3f ms\n", got[0], got[1], got[2], SINK_TS_HOST, SINK_TS_TGT,
               SINK_TS_UNKNOWN, early, (double)lag * 1e-6);
        IM_CHECK_EQ(early, SINK_TS_UNKNOWN);                       /* too few blocks to name a base */
        IM_CHECK_EQ(got[0], SINK_TS_HOST);
        IM_CHECK_EQ(got[1], SINK_TS_TGT);                          /* 27 ms off QPC: NOT taken for the host clock */
        IM_CHECK_EQ(got[2], SINK_TS_UNKNOWN);
        IM_CHECK(lag >= 80000 && lag < 400000);                    /* the floor of the callback's delay */
    };
    t = IM_REGISTER_TEST(e, "zylia", "clicker_late_notice");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("calib view");
        ctx->ItemClick("**/Zylia");
        ctx->Yield(2);
        ctx->ItemCheck("**/simulate claps");
        ctx->ItemOpen("**/Capsule survey");
        ctx->Yield(2);
        IM_CHECK_EQ(PL.state.load(), 0);                           /* the typed center: simulate takes it as true */
        ctx->ItemClick("**/tracked clicker##zsrc");
        ctx->Yield(2);
        ctx->ItemCheck("**/simulate##ck");
        ctx->ItemClick("**/the simulated tip##ck");
        Z.onset_force_est = false;
        ck_run_survey(ctx, false);
        int nif = 0, last = -1;
        const float worst_tip = ck_banked_vs_truth(&nif, &last);
        const float post_ms = (float)((ZP_SNAP_N - ZP_SNAP_PRE) / Z.sim.rate * 1e3);
        printf("clicker late-notice test, stamped: %d banked, %d refused (%d not armed, %d clicker); %d judged at a stamp, "
               "%d at the estimate; seen %.1f to %.1f ms after the stamp (post-roll %.1f + hitch %.0f); the estimate sat "
               "%+.1f ms from the stamp; worst clap %.2f mm off its true tip\n", Z.surv_n, Z.surv_refused,
               Z.ref_kind[ZY_REF_ARM], Z.ref_kind[ZY_REF_TAKE], Z.onset_n_stamped, Z.onset_n_est, Z.notice_min_ms,
               Z.notice_max_ms, post_ms, ZY_SIM_HITCH_S * 1e3, Z.onset_last_est_ms, worst_tip * 1e3f);
        IM_CHECK_EQ(Z.onset_n_est, 0);                             /* every clap carried a usable stamp */
        IM_CHECK_EQ(Z.onset_n_stamped, 11);                        /* 10 still clicks and the waved one */
        IM_CHECK_GE(Z.notice_min_ms, post_ms + (float)ZY_SIM_HITCH_S * 1e3f - 0.5f);   /* the hitch was there */
        IM_CHECK_LT(Z.notice_max_ms, post_ms + (float)ZY_SIM_HITCH_S * 1e3f + (float)CLICKER_SIM_MAX_STEP_S * 1e3f + 0.5f);
        IM_CHECK_GE(Z.onset_last_est_ms, (float)ZY_SIM_HITCH_S * 1e3f - 0.5f);         /* the estimate was that late */
        IM_CHECK_EQ(Z.surv_n, 10);                                 /* every still click banked... */
        IM_CHECK_EQ(nif, 0);
        IM_CHECK_LT(worst_tip, 0.002f);                            /* ...at its true tip */
        IM_CHECK_EQ(Z.surv_refused, 1);                            /* and only the waved one refused */
        IM_CHECK_EQ(Z.ref_kind[ZY_REF_ARM], 1);
        ctx->CaptureScreenshotWindow("//calib view");
        zylia_set_capsules(NULL);
    };
    t = IM_REGISTER_TEST(e, "zylia", "clicker_late_notice_estimate");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("calib view");
        ctx->ItemClick("**/Zylia");
        ctx->Yield(2);
        ctx->ItemCheck("**/simulate claps");
        ctx->ItemOpen("**/Capsule survey");
        ctx->Yield(2);
        IM_CHECK_EQ(PL.state.load(), 0);
        ctx->ItemClick("**/tracked clicker##zsrc");
        ctx->Yield(2);
        ctx->ItemCheck("**/simulate##ck");
        ctx->ItemClick("**/the simulated tip##ck");
        Z.onset_force_est = true;                                  /* the stamp ignored: the old behavior */
        ck_run_survey(ctx, false);
        Z.onset_force_est = false;                                 /* before any check can return */
        int nif = 0, last = -1;
        const float worst_tip = ck_banked_vs_truth(&nif, &last);
        printf("clicker late-notice test, ESTIMATED: %d banked (the stamped run banks 10), %d refused (%d not armed, "
               "%d clicker; last: %s); %d judged at the estimate, %d at a stamp; worst banked clap %.2f mm off its true tip\n",
               Z.surv_n, Z.surv_refused, Z.ref_kind[ZY_REF_ARM], Z.ref_kind[ZY_REF_TAKE], Z.surv_why, Z.onset_n_est,
               Z.onset_n_stamped, worst_tip * 1e3f);
        IM_CHECK_EQ(Z.onset_n_est, 11);
        IM_CHECK_EQ(Z.onset_n_stamped, 0);
        IM_CHECK_LE(Z.surv_n, 1);                                  /* the clicks followed by a move are lost */
        IM_CHECK_GE(Z.surv_refused, 10);
        IM_CHECK_LT(worst_tip, 0.002f);                            /* and nothing banked off its tip */
        zylia_set_capsules(NULL);
    };

    /* ---- the Session tab (calib_view_session runs these; calib_view excludes them) ----
     * A whole simulated rig day through the tab: every step in order, each a subprocess of the real tool
     * except aim, which is the Aim tab. The checks hold the file chain against the simulator's TRUTH,
     * which differs from the plan: as_built.json must land on the truth and not on the plan, trims.json
     * must carry as_built's positions and gains a solve against them gets right (and a solve against the
     * plan's could not, by a margin this test also demands), and the plan must come out byte for byte. */
    t = IM_REGISTER_TEST(e, "session", "full_sim");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ses_write_fixtures();
        const std::string plan_hash = ses_hash_file(ses_abs(SES_PLAN));
        IM_CHECK(!plan_hash.empty());
        ses_ui_new(ctx, SES_FULL, NULL, "4");
        IM_CHECK(SES.simulate);
        IM_CHECK_STR_EQ(SES.in.aim_speakers.c_str(), "4");
        const double t0 = ImGui::GetTime();
        ses_ui_run(ctx, SES_FRAME, 60.0);
        ses_ui_run(ctx, SES_LOCALIZE, 180.0);
        ses_ui_run(ctx, SES_CAPSULES, 120.0);
        IM_CHECK_EQ(SES.step[SES_FRAME].status, SES_PASSED);
        IM_CHECK_EQ(SES.step[SES_LOCALIZE].status, SES_PASSED);
        IM_CHECK_EQ(SES.step[SES_CAPSULES].status, SES_PASSED);
        IM_CHECK(SES.step[SES_CAPSULES].have_latency);

        /* aim: the session hands speaker 4 to the Aim tab with the session's files, the simulated stand on
         * the Placement panel; two readings, accept, stop */
        ctx->ItemClick("**/Run##ses_aim");
        ctx->Yield(2);
        IM_CHECK_EQ(SES.step[SES_AIM].status, SES_RUNNING);
        IM_CHECK_EQ(AJ.spk, 4);
        IM_CHECK(AJ.simulate && AJ.latency_set);
        IM_CHECK(!_stricmp(V.pathA, ses_join(SES.folder, "as_built.json").c_str()));
        ctx->ItemClick("**/Aim");
        ctx->Yield(2);
        { std::lock_guard<std::mutex> lk(AJ.mu);                 /* the simulated box where the layout says */
          AJ.d.move[0] = AJ.d.move[1] = AJ.d.move[2] = 0.f; AJ.d.turn_deg = AJ.d.tilt_deg = AJ.d.screen_db = 0.f; }
        { const double tw = ImGui::GetTime(); while (!pl_live() && ImGui::GetTime() - tw < 10.0) ctx->Yield(); }
        if (!pl_live()) printf("session test: the Placement panel is not live: state %d, %s\n", PL.state.load(), PL.msg);
        IM_CHECK(pl_live());
        IM_CHECK(PL.sim && PL.body_frame);                       /* the session's capsules.json, body frame */
        ctx->ItemClick("**/Start##aim");
        {
            const double tw = ImGui::GetTime();
            for (;;) {
                int n; { std::lock_guard<std::mutex> lk(AJ.mu); n = AJ.d.n; }
                if (n >= 2 || AJ.state.load() == 3 || ImGui::GetTime() - tw > 60.0) break;
                ctx->Yield();
            }
        }
        IM_CHECK_EQ(AJ.state.load(), 1);
        ctx->ItemClick("**/Accept for the session##aim");
        ctx->ItemClick("**/Stop##aim");
        { const double tw = ImGui::GetTime(); while (AJ.state.load() == 1 && ImGui::GetTime() - tw < 30.0) ctx->Yield(); }
        PL.stop.store(true);                                     /* the panel was the aim step's; let it go */
        IM_CHECK_EQ(SES.step[SES_AIM].status, SES_PASSED);
        IM_CHECK_EQ((int)SES.step[SES_AIM].aim.size(), 1);
        if (!SES.step[SES_AIM].aim.empty()) {
            const SesAimRec& r = SES.step[SES_AIM].aim[0];
            printf("session test: aim accepted speaker %d: %.2f dB below the peak, off axis %s, %s%.1f mm from the plan\n",
                   r.spk, r.below_db, r.angle.c_str(), r.have_pos ? "" : "(no position) ", r.pos_mm);
            IM_CHECK_EQ(r.spk, 4);
            /* the box stands at the TRUTH, 100 mm toward +z of its plan: the readout must say so */
            IM_CHECK(r.have_pos && r.pos_mm > 80.f && r.pos_mm < 120.f);
        }
        ctx->ItemClick("**/Session");
        ctx->Yield(2);

        ses_ui_run(ctx, SES_TRIMS, 120.0);
        ses_ui_run(ctx, SES_VERIFY, 120.0);
        ses_ui_run(ctx, SES_GRID, 180.0);
        ses_ui_run(ctx, SES_VALIDATE, 180.0);
        printf("session test: the whole simulated session took %.1f s\n", ImGui::GetTime() - t0);
        for (int k = 0; k < SES_NSTEPS; ++k) {
            IM_CHECK_EQ(SES.step[k].status, SES_PASSED);
            IM_CHECK(ses_stale(SES, k).empty());
        }

        /* the chain: each step read the file the step before it wrote, in that step's current run */
        IM_CHECK(SES.step[SES_FRAME].command.find("--baffle-offset-m 0.0600") != std::string::npos);   /* the input, passed */
        IM_CHECK(ses_used(SES_FRAME, "baffle_offset_m") && ses_used(SES_FRAME, "baffle_offset_m")->path == "0.0600");
        IM_CHECK(SES.step[SES_LOCALIZE].command.find("--layout \"" + ses_abs(SES_PLAN) + "\" --out as_built.json") != std::string::npos);
        IM_CHECK(SES.step[SES_LOCALIZE].command.find("--sim-truth") != std::string::npos);
        IM_CHECK(ses_link(SES_CAPSULES, SES_LOCALIZE));
        IM_CHECK(ses_link(SES_TRIMS, SES_LOCALIZE));
        IM_CHECK(ses_link(SES_TRIMS, SES_CAPSULES));
        IM_CHECK(ses_link(SES_VERIFY, SES_TRIMS));
        IM_CHECK(ses_link(SES_GRID, SES_TRIMS));
        IM_CHECK(ses_link(SES_VALIDATE, SES_GRID));
        IM_CHECK(ses_link(SES_VALIDATE, SES_CAPSULES));
        IM_CHECK(ses_link(SES_AIM, SES_LOCALIZE));
        IM_CHECK(SES.step[SES_TRIMS].command.find("--layout as_built.json --out trims.json") != std::string::npos);
        IM_CHECK(SES.step[SES_VERIFY].command.find("--layout trims.json") != std::string::npos);
        IM_CHECK(SES.step[SES_GRID].command.find("--layout trims.json --out grid.json") != std::string::npos);
        IM_CHECK(SES.step[SES_VALIDATE].command.find("--layout grid.json") != std::string::npos);

        /* the background: every step that sweeps or captures recorded the one its tool read (silent in
         * simulate), and the trend line flags a step that got louder than the last one */
        for (int k : { SES_LOCALIZE, SES_CAPSULES, SES_TRIMS, SES_VERIFY, SES_GRID, SES_VALIDATE })
            IM_CHECK(SES.step[k].have_bg && SES.step[k].bg_dbfs <= SES_BG_SILENT_DBFS);
        {
            Session T;
            T.step[SES_TRIMS].have_bg = true;  T.step[SES_TRIMS].bg_dbfs = -70.0;
            T.step[SES_VERIFY].have_bg = true; T.step[SES_VERIFY].bg_dbfs = -68.0;
            const std::string flat = ses_background_line(T, SES_VERIFY);
            IM_CHECK(flat.find("+2.0 dB against trims") != std::string::npos && flat.find("RISING") == std::string::npos);
            T.step[SES_VERIFY].bg_dbfs = -55.0;
            IM_CHECK(ses_background_line(T, SES_VERIFY).find("RISING") != std::string::npos);
            IM_CHECK(ses_background_line(T, SES_GRID).empty());
        }

        /* the plan, byte for byte; the files exist */
        IM_CHECK(ses_hash_file(ses_abs(SES_PLAN)) == plan_hash);
        for (int k = 0; k < SES_NSTEPS; ++k)
            if (ses_step_artifact(k)[0]) IM_CHECK(ses_file_exists(ses_join(SES.folder, ses_step_artifact(k))));

        static Layout LP, LT, LA, LR, LG;                         /* never stack locals (layout.h) */
        char err[256];
        IM_CHECK(layout_load(SES_PLAN, 48000, &LP, err, sizeof err));
        IM_CHECK(layout_load(SES_TRUTH, 48000, &LT, err, sizeof err));
        IM_CHECK(layout_load(ses_join(SES.folder, "as_built.json").c_str(), 48000, &LA, err, sizeof err));
        IM_CHECK(layout_load(ses_join(SES.folder, "trims.json").c_str(), 48000, &LR, err, sizeof err));
        IM_CHECK(layout_load(ses_join(SES.folder, "grid.json").c_str(), 48000, &LG, err, sizeof err));
        IM_CHECK(LA.count == SES_NSPK && LR.count == SES_NSPK && LG.count == SES_NSPK);
        /* as_built: on the truth, off the plan where the truth is, and the plan kept beside it */
        float worst_truth = 0.f, least_moved = 1e9f;
        for (int i = 0; i < SES_NSPK; ++i) {
            const float dt = ses_dist(LA.speakers[i].pos, LT.speakers[i].pos);
            if (dt > worst_truth) worst_truth = dt;
            const float mv = sqrtf(SES_MOVE[i][0] * SES_MOVE[i][0] + SES_MOVE[i][1] * SES_MOVE[i][1] + SES_MOVE[i][2] * SES_MOVE[i][2]);
            if (mv > 0.f) { const float dp = ses_dist(LA.speakers[i].pos, LP.speakers[i].pos); if (dp < least_moved) least_moved = dp; }
            IM_CHECK(LA.speakers[i].has_plan && ses_dist(LA.speakers[i].plan_pos, LP.speakers[i].pos) < 1e-4f);
            IM_CHECK_LT(ses_dist(LR.speakers[i].pos, LA.speakers[i].pos), 1e-4f);   /* trims carries as_built */
            IM_CHECK_LT(ses_dist(LG.speakers[i].pos, LR.speakers[i].pos), 1e-4f);
            IM_CHECK_EQ(LG.speakers[i].delay_samples, LR.speakers[i].delay_samples);
        }
        printf("session test: as_built is %.1f mm from the truth at worst; the moved speakers %.1f mm from the plan at least\n",
               worst_truth * 1e3f, least_moved * 1e3f);
        IM_CHECK_LT(worst_truth, 0.010f);
        IM_CHECK_GT(least_moved, 0.060f);
        IM_CHECK_EQ((int)LG.rq_grid.npos, 2);

        /* trims: each gain against the simulator's own sensitivities (positions right, the 1/r divides
         * out exactly). And the margin: the same solve against the PLAN's positions would miss by more */
        double smin = 1e30;
        for (int i = 0; i < SES_NSPK; ++i) if (calib_sim_sensitivity(i) < smin) smin = calib_sim_sensitivity(i);
        const float c[3] = { 0.f, 1.45f, 0.f };
        double sref_plan = 1e30, sp[SES_NSPK];
        for (int i = 0; i < SES_NSPK; ++i) {
            sp[i] = calib_sim_sensitivity(i) * ses_dist(LP.speakers[i].pos, c) / ses_dist(LT.speakers[i].pos, c);
            if (sp[i] < sref_plan) sref_plan = sp[i];
        }
        float worst_db = 0.f, plan_db = 0.f;
        for (int i = 0; i < SES_NSPK; ++i) {
            const double want = 20.0 * log10(smin / calib_sim_sensitivity(i));
            const float got = lin_to_db(LR.speakers[i].gain_lin);
            if (fabsf(got - (float)want) > worst_db) worst_db = fabsf(got - (float)want);
            const double from_plan = 20.0 * log10(sref_plan / sp[i]);
            if (fabs(from_plan - want) > plan_db) plan_db = (float)fabs(from_plan - want);
        }
        printf("session test: trims against the truth: worst gain %.3f dB; a solve against the plan would miss by %.2f dB\n",
               worst_db, plan_db);
        IM_CHECK_LT(worst_db, 0.1f);
        IM_CHECK_GT(plan_db, 0.25f);                              /* the check above has the power to see the plan */

        /* one log: every step's command and its result */
        for (int k = 0; k < SES_NSTEPS; ++k) {
            char h[64];
            snprintf(h, sizeof h, ": %s run ", ses_step_id(k));
            IM_CHECK(ses_log_has(h));
        }
        IM_CHECK(ses_log_has("aim: speaker 4 accepted"));
        IM_CHECK(ses_log_has("verify: 0 speaker(s) flagged"));
        ctx->CaptureScreenshotWindow("//calib view");
    };

    /* Resume: the full session's record survives a reopen exactly (save, forget everything in memory,
     * Open the folder). Then localize runs again, and everything downstream of it reads STALE, by name,
     * and a stale prerequisite refuses the step after it. */
    t = IM_REGISTER_TEST(e, "session", "resume");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("calib view");
        ctx->ItemClick("**/Session");
        ctx->Yield(2);
        Session before;
        IM_CHECK(ses_load(&before, ses_abs(SES_FULL), NULL));    /* needs session/full_sim's folder */
        for (int k = 0; k < SES_NSTEPS; ++k) IM_CHECK_EQ(before.step[k].status, SES_PASSED);
        /* open it through the tab (this process may not have run full_sim), save it, and take that as
         * the state to come back to */
        ses_ui_type(ctx, "**/##sesdir", SES_FULL);
        ctx->ItemClick("**/Open##ses");
        ctx->Yield(2);
        IM_CHECK(!SES.folder.empty());
        for (int k = 0; k < SES_NSTEPS; ++k) IM_CHECK_EQ(SES.step[k].status, SES_PASSED);
        std::string e2;
        IM_CHECK(ses_save(SES, &e2));
        before = SES;
        SES = Session();                                         /* forget it all */
        memset(&SU, 0, sizeof SU);
        { std::lock_guard<std::mutex> lk(SR.mu); SR.lines.clear(); }
        ctx->Yield(2);
        IM_CHECK(SES.folder.empty());
        ses_ui_type(ctx, "**/##sesdir", SES_FULL);
        ctx->ItemClick("**/Open##ses");
        ctx->Yield(2);
        std::string why;
        IM_CHECK_STR_EQ(SES.folder.c_str(), before.folder.c_str());
        const bool same = ses_same(before, SES, &why);
        if (!same) printf("session test: resume differs at %s\n", why.c_str());
        IM_CHECK(same);
        IM_CHECK_STR_EQ(SU.plan, before.in.plan.c_str());         /* the fields show the reopened inputs */
        IM_CHECK(ses_log_has(": validate run "));                 /* the log came back too */
        for (int k = 0; k < SES_NSTEPS; ++k) IM_CHECK(ses_stale(SES, k).empty());

        ses_ui_run(ctx, SES_LOCALIZE, 180.0);
        IM_CHECK_EQ(SES.step[SES_LOCALIZE].status, SES_PASSED);
        IM_CHECK_GT(SES.step[SES_LOCALIZE].run, before.step[SES_LOCALIZE].run);
        IM_CHECK(ses_stale(SES, SES_FRAME).empty());              /* upstream of localize: untouched */
        IM_CHECK(ses_stale(SES, SES_LOCALIZE).empty());
        for (int k = SES_CAPSULES; k < SES_NSTEPS; ++k) {
            const std::string st = ses_stale(SES, k);
            printf("session test: after localize ran again, %-8s STALE: %s\n", ses_step_id(k), st.c_str());
            IM_CHECK(!st.empty());
            IM_CHECK_EQ(SES.step[k].status, SES_PASSED);          /* its record stays, marked */
        }
        IM_CHECK(ses_stale(SES, SES_CAPSULES).find("localize ran again") != std::string::npos);
        IM_CHECK(ses_stale(SES, SES_TRIMS).find("localize ran again") != std::string::npos);
        IM_CHECK(ses_stale(SES, SES_VERIFY).find("trims is stale") != std::string::npos);
        /* a stale prerequisite refuses what comes after it */
        const int trims_run = SES.step[SES_TRIMS].run;
        ctx->ItemClick("**/Run##ses_trims");
        ctx->Yield(2);
        IM_CHECK(!ses_busy());
        IM_CHECK_EQ(SES.step[SES_TRIMS].run, trims_run);
        IM_CHECK(strstr(g_ses_msg, "trims: refused: capsules is stale") != NULL);
        /* the staleness is the files', so it survives a reopen */
        SES = Session();
        ctx->ItemClick("**/Open##ses");
        ctx->Yield(2);
        IM_CHECK(!ses_stale(SES, SES_CAPSULES).empty());
        ctx->CaptureScreenshotWindow("//calib view");
    };

    /* No UI: exit 0 is not a pass when the step's file is missing, and a canceled run is a failure
     * whatever its exit code. Then the file appears and the same exit 0 passes, hashed. */
    t = IM_REGISTER_TEST(e, "session", "finish_rules");
    t->TestFunc = [](ImGuiTestContext*) {
        Session S;
        S.folder = ses_abs("calibview_session_rules");
        ses_clear_dir("calibview_session_rules");
        CreateDirectoryA(S.folder.c_str(), NULL);
        ses_begin(&S, SES_TRIMS, "test", std::vector<SesFile>());
        ses_finish(&S, SES_TRIMS, 0, "trims: gain_db in [-1.00, 0.00]  max delay 0.1 ms\n", false);
        IM_CHECK_EQ(S.step[SES_TRIMS].status, SES_FAILED);
        IM_CHECK(S.step[SES_TRIMS].note.find("trims.json was not written") != std::string::npos);
        ses_begin(&S, SES_TRIMS, "test", std::vector<SesFile>());
        IM_CHECK(ses_write_text(ses_join(S.folder, "trims.json").c_str(), "{}\n"));
        ses_finish(&S, SES_TRIMS, 0, "", true);
        IM_CHECK_EQ(S.step[SES_TRIMS].status, SES_FAILED);      /* canceled */
        ses_finish(&S, SES_TRIMS, 3, "", false);
        IM_CHECK_EQ(S.step[SES_TRIMS].status, SES_FAILED);      /* the tool's own check failed */
        ses_finish(&S, SES_TRIMS, 0, "", false);
        IM_CHECK_EQ(S.step[SES_TRIMS].status, SES_PASSED);
        IM_CHECK(S.step[SES_TRIMS].produced.size() == 1 && S.step[SES_TRIMS].produced[0].hash == ses_hash_file(ses_join(S.folder, "trims.json")));
        /* the localize summary reads the residual the ZM-1 way: the Dante Via leg's tens of ms are a
         * correct run, past the arrival window's allowance is LARGE, below the driver's loop NEGATIVE */
        const char* const lines[3] = { "-> residual +61.20 ms\n", "-> residual +190.00 ms\n", "-> residual -2.00 ms\n" };
        const char* const want[3]  = { NULL, "(LARGE", "(NEGATIVE" };
        for (int i = 0; i < 3; ++i) {
            ses_begin(&S, SES_LOCALIZE, "test", std::vector<SesFile>());
            ses_finish(&S, SES_LOCALIZE, 0, lines[i], false);
            printf("session test: localize summary for \"%.*s\": %s\n", (int)strlen(lines[i]) - 1, lines[i], S.step[SES_LOCALIZE].summary.c_str());
            IM_CHECK(S.step[SES_LOCALIZE].summary.find("residual") != std::string::npos);
            if (want[i]) IM_CHECK(S.step[SES_LOCALIZE].summary.find(want[i]) != std::string::npos);
            else IM_CHECK(S.step[SES_LOCALIZE].summary.find('(') == std::string::npos);
        }
        ses_clear_dir("calibview_session_rules");
    };

    /* The baffle offset: typed in the tab, passed to the frame check, kept in session.json, and a change
     * to it makes a passed frame step STALE (which then blocks localize), the way a changed input file
     * does. Typing the value it ran with makes it current again. No tool runs: the frame record is made
     * by hand, so this is the model and the field, not the survey. */
    t = IM_REGISTER_TEST(e, "session", "baffle_offset");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        static const char* dir = "calibview_session_baffle";
        ses_write_fixtures();
        ses_ui_new(ctx, dir, NULL, NULL);
        IM_CHECK_EQ(SES.in.baffle_offset_m, 0.06);                /* typed through the field */
        SesTools T;
        T.survey = "bwa_speaker_survey.exe";                      /* never run here */
        std::string cmd, why;
        std::vector<SesFile> cons;
        IM_CHECK(ses_command(SES, SES_FRAME, T, &cmd, &cons, &why));
        IM_CHECK(cmd.find(" --baffle-offset-m 0.0600") != std::string::npos);
        ses_begin(&SES, SES_FRAME, cmd, cons);
        IM_CHECK(ses_write_text(ses_join(SES.folder, "frame_check.csv").c_str(), "index\n"));
        ses_finish(&SES, SES_FRAME, 0, "summary: matched 4\n", false);
        IM_CHECK_EQ(SES.step[SES_FRAME].status, SES_PASSED);
        IM_CHECK(ses_stale(SES, SES_FRAME).empty());
        IM_CHECK(ses_blocked(SES, SES_LOCALIZE).empty());
        ses_save_now();

        ses_ui_type(ctx, "**/baffle offset (m)##ses", "0.0725");   /* the measured depth comes in */
        ctx->Yield(2);
        IM_CHECK_EQ(SES.in.baffle_offset_m, 0.0725);
        const std::string st = ses_stale(SES, SES_FRAME);
        printf("session test: after the baffle offset changed, frame STALE: %s\n", st.c_str());
        IM_CHECK(st.find("baffle_offset_m input changed") != std::string::npos && st.find("0.0600 then, 0.0725 now") != std::string::npos);
        IM_CHECK(ses_blocked(SES, SES_LOCALIZE).find("frame is stale") != std::string::npos);
        IM_CHECK(ses_command(SES, SES_FRAME, T, &cmd, &cons, &why) && cmd.find(" --baffle-offset-m 0.0725") != std::string::npos);

        /* session.json carries it, and the staleness comes back with a reopen */
        ses_save_now();
        Session R;
        IM_CHECK(ses_load(&R, SES.folder, NULL));
        IM_CHECK_EQ(R.in.baffle_offset_m, 0.0725);
        IM_CHECK(!ses_stale(R, SES_FRAME).empty());
        std::string d;
        IM_CHECK(ses_same(SES, R, &d));

        ses_ui_type(ctx, "**/baffle offset (m)##ses", "0.06");     /* back to what the step ran with */
        ctx->Yield(2);
        IM_CHECK(ses_stale(SES, SES_FRAME).empty());
        ses_clear_dir(dir);
    };

    /* A failure blocks what depends on it: a capsule survey of 3 speakers is refused by the tool (exit
     * 2), and aim, trims, verify, grid and validate then refuse to run, each with the reason, and nothing
     * starts. The frame check is skipped with a note on the way, which localize accepts. */
    t = IM_REGISTER_TEST(e, "session", "failure_blocks");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ses_write_fixtures();
        ses_ui_new(ctx, SES_BLOCK, "0,2,4", "4", SES_PLAN_FEW, SES_TRUTH_FEW);
        ses_ui_type(ctx, "**/##sesnote", "checked with Motive's own tools");
        ctx->ItemClick("**/Skip##ses_frame");
        ctx->Yield(2);
        IM_CHECK_EQ(SES.step[SES_FRAME].status, SES_SKIPPED);
        IM_CHECK_STR_EQ(SES.step[SES_FRAME].note.c_str(), "checked with Motive's own tools");
        IM_CHECK(ses_blocked(SES, SES_LOCALIZE).empty());        /* a skip with a note satisfies the order */
        ses_ui_run(ctx, SES_LOCALIZE, 180.0);
        IM_CHECK_EQ(SES.step[SES_LOCALIZE].status, SES_PASSED);
        ses_ui_run(ctx, SES_CAPSULES, 60.0);
        IM_CHECK_EQ(SES.step[SES_CAPSULES].status, SES_FAILED);
        IM_CHECK_EQ(SES.step[SES_CAPSULES].exit_code, 2);
        IM_CHECK(SES.step[SES_CAPSULES].produced.empty());
        IM_CHECK(!ses_file_exists(ses_join(SES.folder, "capsules.json")));
        IM_CHECK(ses_log_has("3 speaker(s); the survey needs 4 or more"));   /* the tool's own reason, in the log */
        for (int k = SES_AIM; k < SES_NSTEPS; ++k) {
            const std::string b = ses_blocked(SES, k);
            printf("session test: after the capsule survey failed, %-8s blocked: %s\n", ses_step_id(k), b.c_str());
            IM_CHECK(!b.empty());
        }
        IM_CHECK(ses_blocked(SES, SES_AIM).find("capsules failed") != std::string::npos);
        IM_CHECK(ses_blocked(SES, SES_TRIMS).find("capsules failed") != std::string::npos);
        IM_CHECK(ses_blocked(SES, SES_VERIFY).find("trims") != std::string::npos);
        IM_CHECK(ses_blocked(SES, SES_VALIDATE).find("trims") != std::string::npos);
        /* through the UI: the button refuses, says why, starts nothing and writes nothing */
        for (int k : { SES_AIM, SES_TRIMS, SES_VALIDATE }) {
            char id[64];
            snprintf(id, sizeof id, "**/Run##ses_%s", ses_step_id(k));
            ctx->ItemClick(id);
            ctx->Yield(2);
            IM_CHECK(!ses_busy());
            IM_CHECK_EQ(SES.step[k].status, SES_PENDING);
            IM_CHECK_EQ(SES.step[k].run, 0);
            char want[64];
            snprintf(want, sizeof want, "%s: refused: ", ses_step_id(k));
            IM_CHECK(strstr(g_ses_msg, want) == g_ses_msg);
            if (k != SES_VALIDATE) IM_CHECK(strstr(g_ses_msg, "capsules failed") != NULL);
        }
        IM_CHECK(!ses_file_exists(ses_join(SES.folder, "trims.json")));
        IM_CHECK(ses_log_has(": trims refused: "));
        ctx->CaptureScreenshotWindow("//calib view");
    };

    t = IM_REGISTER_TEST(e, "viewer", "tabs");                   /* every tab renders without faulting */
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("calib view");
        const char* tabs[] = { "**/Session", "**/Array", "**/Trims", "**/EQ", "**/IRs", "**/Diff", "**/Capture", "**/Zylia", "**/Aim" };
        for (int i = 0; i < 9; ++i) { ctx->ItemClick(tabs[i]); ctx->Yield(2); }
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
                   "  calibration station: the Session tab (the rig-day steps in order, one folder, one log,\n"
                   "  resumable), array 3D view, trims, EQ curves, IRs, a layout diff (A vs B), the Capture\n"
                   "  tab (sweep->measure->solve->writeback), the Zylia DOA tab, and the Aim tab (live aiming\n"
                   "  of one of A's speakers with the ZM-1).\n");
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
        if (!write_fixture(FIX_A, 0) || !write_fixture(FIX_B, 1) || !write_fixture(FIX_D, 2) || !write_fixture(FIX_E, 3) || !write_fixture(FIX_P, 4) ||
            !write_fixture(FIX_S, 0, true) || !write_fixture(FIX_DS, 2, true)) {
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
    /* the capture tests run whole simulated sweeps (a ZM-1 verify is two agreeing sweeps x 19
     * deconvolutions per speaker); alone the slowest takes about 25 s, but under ctest -j beside the
     * other suites one passed the default 60 s kill and the abort cascaded into the tests after it. The
     * per-test wall times print at the end ([time]); read those rather than add timing asserts. */
    teio.ConfigWatchdogWarning     = 120.0f;
    teio.ConfigWatchdogKillTest    = 600.0f;
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
        /* wall time per test, slowest first: the suite's budget is the sum of these, and a test that
         * grows shows here before the ctest timeout or the watchdog finds it */
        ImVector<ImGuiTest*> tl;
        ImGuiTestEngine_GetTestList(g_te, &tl);
        std::vector<ImGuiTest*> ran;
        for (ImGuiTest* t : tl) if (t->Output.Status != ImGuiTestStatus_Unknown && t->Output.EndTime >= t->Output.StartTime) ran.push_back(t);
        std::sort(ran.begin(), ran.end(), [](const ImGuiTest* a, const ImGuiTest* b) {
            return a->Output.EndTime - a->Output.StartTime > b->Output.EndTime - b->Output.StartTime; });
        double total = 0.0;
        for (const ImGuiTest* t : ran) {
            const double s = (double)(t->Output.EndTime - t->Output.StartTime) * 1e-6;
            total += s;
            printf("[time] %7.1f s  %s/%s%s\n", s, t->Category, t->Name, t->Output.Status == ImGuiTestStatus_Success ? "" : "  (FAILED)");
        }
        printf("[time] %7.1f s  total\n", total);
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
    if (ses_busy()) {                                            /* a session step still running: stop it, say so */
        const int k = g_ses_step;
        ses_run_cancel(&SR);
        while (SR.state.load() == 1) Sleep(10);
        ses_run_join(&SR);
        if (k >= 0) {
            ses_finish(&SES, k, 1, std::string(), true);
            SES.step[k].note = "interrupted: the window closed while it ran";
            std::string e;
            ses_save(SES, &e);
        }
    }
    if (J.th_live) { J.cancel.store(true); J.th.join(); }        /* reap a still-running capture job */
    if (AJ.th_live) { AJ.stop.store(true); AJ.th.join(); }       /* ... and a live aiming run */
    if (PL.th_live) { PL.stop.store(true); PL.th.join(); }       /* ... and the placement poller */
    clicker_stop(&CK);                                           /* ... and the clicker's */
    return rc;
}
