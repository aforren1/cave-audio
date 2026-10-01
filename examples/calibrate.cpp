/*
 * calibrate.c — speaker calibration tool for the CAVE array.
 *
 * Plays an exponential sweep out each speaker in turn, records it at an omnidirectional measurement
 * mic, recovers per-speaker delay + sensitivity (measure.c), turns those into layout trims that
 * arrival-align to the farthest speaker and equalize sensitivity (calib.c), and writes them back into
 * cave_layout.json. Run it once, at the listening position, with the mic where the head will be.
 *
 *   calibrate --layout examples/cave_layout.json --out tuned.json --mic 0 0 0 --input 0
 *   calibrate --simulate                     # no hardware: synthesize captures from the layout geometry
 *   calibrate --verify --mic 0 1.448 0       # second pass: sweep THROUGH the trims, report the leftovers
 *   calibrate --zylia --trims --input 0 ... # the ZM-1 as the trim mic (bare --zylia = the position survey)
 *   calibrate --live 7 --zylia --latency 20.6 # live aiming: one speaker's position and off-axis angle
 *   calibrate --live 7 --zylia --ref-speakers 2,10,18,24   # ... the latency from measured speaker positions
 *   calibrate --localize rows.txt --zylia    # speaker positions from the ZM-1's center arrival (no survey)
 *   calibrate --capsule-survey s.json --zylia --mic 0 1.448 0   # the capsule table, swept from the speakers
 *   calibrate --aim-sheet aim.csv            # no audio: the installer's aiming sheet
 *
 * Two capture backends (extracted to calib_capture.cpp, shared with bwa_calib_view's Capture tab):
 *   - ASIO full-duplex (gated on BWA_HAVE_ASIO): 26 outputs + one mic input, sample-aligned — or, with
 *     --zylia, the ZM-1's 19 capsule inputs on the same device (Dante Via; docs/calibration.md). Built
 *     when the ASIO SDK is vendored. NOT verified on hardware here — treat as rig bring-up code (it
 *     mirrors asio_sink.cpp's host: load -> ASIOInit -> ASIOGetChannels -> create in+out buffers -> Start).
 *   - simulate: delays/attenuates the sweep per the layout's speaker->mic distances (+ a deterministic
 *     sensitivity wobble) so the whole measure -> solve -> writeback path runs without the rig.
 *
 * Compiled as C++ only because the ASIO host helpers are C++; the engine pieces it calls are C. Built
 * opt-in: cmake -DBWA_BUILD_CALIBRATE=ON.
 */
extern "C" {
#include "calib/measure.h"
#include "calib/calib.h"
#include "core/layout.h"
#include "calib/zylia.h"
#include "sink/sink.h"          /* BWA_CHANNELS (== BWA_MAX_CHANNELS, the public name used below) */
#include "dsp/sos.h"           /* room-temperature speed of sound: --temp / --c, or the layout's */
}
#include "calib_capture.h" /* sweep constants + the simulate/ASIO capture backends */
#include "mic_track.h"     /* --track: the ZM-1's stand as a tracked rigid body */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <algorithm>
#ifdef BWA_HAVE_ASIO
#define WIN32_LEAN_AND_MEAN
#include <windows.h>       /* Sleep */
#include <conio.h>         /* _kbhit/_getch for --live */
#endif

/* local aliases for the shared sweep geometry (the rest of this file predates the extraction) */
static const double FS         = CAL_FS;
static const double F1         = CAL_F1, F2 = CAL_F2;
static const double BAND_HZ[2] = { CAL_BAND_LO, CAL_BAND_HI };
/* --check-aim's tilt bands (calib.h says why they are not BAND_HZ): passed to measure_response in the
 * --localize loop AND to calib_check_aim / calib_aim_tilt_db, so both sides of the tilt use them */
static const double AIM_BAND_HZ[2] = { CALIB_AIM_MID_HZ, CALIB_AIM_HIGH_HZ };
static const double LIVE_BAND_HZ[2] = { CALIB_LIVE_MID_HZ, CALIB_LIVE_HIGH_HZ };   /* the live meter's (calib.h) */
static const int    NSWEEP     = CAL_NSWEEP;
static const int    CAPLEN     = CAL_CAPLEN;
static const int    IR_LEN     = CAL_IRLEN;

/* Record the c every range was scaled by into the layout that just received them, so a rerun on this
 * file inherits the rig's temperature instead of needing the flag again, and a reader can tell which
 * c the survey assumed. Non-fatal: the measurements themselves are already written. */
static void record_sos(const char* path, double sos) {
    char err[256] = {0};
    if (!calib_write_sos(path, path, sos, err, sizeof err))
        fprintf(stderr, "calibrate: warning: could not record speed of sound (%s)\n", err);
}

/* The position writers (--localize, the --zylia survey) keep the plan the first time they overwrite a
 * speaker's position (layout_json_keep_plan): say what happened, so the installer knows the output
 * file now holds the plan, and that a second survey left it alone. */
static void report_plan(const char* who, int nplan, int n) {
    if (nplan > 0)
        printf("%s: recorded the plan for %d of %d speaker(s): the positions and aims this survey replaced are\n"
               "%*s now plan_position and plan_aim, which live aiming targets and no survey overwrites\n",
               who, nplan, n, (int)strlen(who), "");
    else
        printf("%s: every speaker already carries a plan (plan_position); the plan was left as it was\n", who);
}

/* ---- --track: the ZM-1's stand as a tracked rigid body (docs/calibration.md, "Placing the ZM-1 with
 * the tracker") ----
 * Before the captures of a placement, wait until the measured center sits within --place-tol-mm of the
 * target and has stayed still (placement.h), then take THAT as the mic position. After each capture,
 * the bump check: a run measured across a moved mic is wrong, so it stops. The two DIRECTION modes (the
 * --zylia survey, and live aiming with its position readout on) also hold the gate while the stand
 * turns and stop on a turn past their limit (place_turn_limit_deg): a stand turned about the array
 * center moves no center, and turns every direction they read. The rest read only the center and keep
 * center-only checks. With --track-sim the
 * simulated captures are synthesized at the simulated stand's TRUE center (mic_track_sim_truth), not at
 * the mic variable the tool solves with, so a run that forgot to take the measured center, or took it
 * wrong, is solved at a point the captures did not come from. */
static MicTrack  g_mt;                    /* static: it carries an atomic; main is not reentrant */
static int       g_tracked, g_track_sim;
static float     g_tol_m = PLACE_TOL_DEFAULT_M;
static double    g_place_timeout = 300.0;
static MicPlaced g_placed;
static float     g_taken[3];              /* the center the current placement took */
static float     g_sim_at[3];             /* --track-sim: the true center the next capture comes from */
static float     g_max_move;
static int       g_nchecks, g_nunchecked;
static float     g_turn_limit;            /* this placement's turn limit (deg); 0 = a center-only mode */
static float     g_max_turn, g_last_turn;

static const double CAPTURE_S = CAL_CAPLEN / CAL_FS, LIVE_CAPTURE_S = CAL_LIVE_CAPLEN / CAL_FS;

static void track_sim_sync(void) { if (g_track_sim) mic_track_sim_truth(&g_mt, g_sim_at); }

/* --localize's gate: trilateration needs each position KNOWN, not hit, so the row is only roughly
 * enforced. But not "still anywhere": the stand is still at the PREVIOUS row the moment the gate
 * restarts, and a stillness-only gate opened there after its 1.5 s and recorded that row twice. Rows
 * worth trilaterating from sit far more than this apart. */
#define LOCALIZE_TOL_M 0.10f

/* The loose gate's tolerance: LOCALIZE_TOL_M, or --place-tol-mm if wider. The --room-eq-grid rows use
 * it too, for the same reason: their key is the MEASURED position, so a row only has to be near. */
static float loose_tol_m(void) { return g_tol_m > LOCALIZE_TOL_M ? g_tol_m : LOCALIZE_TOL_M; }

/* Wait for the placement and take the measured center into mic_out. loose_why: a mode that records the
 * MEASURED position (--localize, the --room-eq-grid rows), so the gate is loose (loose_tol_m) and this
 * says why a miss costs nothing; NULL = the strict gate. turn_limit_deg > 0: a direction mode, whose
 * gate also waits for the orientation to hold still and whose bump check stops on a turn.
 * Returns 0, or 1 (timed out). */
static int track_place(const float target[3], const char* loose_why, const char* what, float mic_out[3], float turn_limit_deg) {
    PlaceCfg cfg;
    place_cfg_default(&cfg, loose_why ? loose_tol_m() : g_tol_m);
    if (turn_limit_deg > 0.f) place_cfg_direction(&cfg, turn_limit_deg);
    g_turn_limit = turn_limit_deg > 0.f ? turn_limit_deg : 0.f;
    if (mic_track_place_console(&g_mt, target, &cfg, g_place_timeout, !g_track_sim, what, &g_placed)) return 1;
    memcpy(mic_out, g_placed.center, sizeof g_placed.center);
    memcpy(g_taken, g_placed.center, sizeof g_taken);
    if (loose_why && g_placed.dist_m > g_tol_m)
        printf("placement: %.1f mm from the planned position (the %.0f mm guide): %s needs the position\n"
               "           KNOWN, not hit, so the measured one is recorded\n", g_placed.dist_m * 1e3, g_tol_m * 1e3, loose_why);
    g_max_move = 0.f; g_nchecks = 0; g_nunchecked = 0; g_max_turn = 0.f; g_last_turn = 0.f;
    if (g_turn_limit > 0.f)
        printf("placement: a direction mode: the bump check also stops on a turn past %.2f deg\n", g_turn_limit);
    track_sim_sync();
    return 0;
}

/* The farthest speaker from p: the range a --zylia survey's directions are read at. */
static float farthest_speaker_m(const Layout& L, const float p[3]) {
    float r = 0.f;
    for (uint32_t i = 0; i < L.count; ++i) {
        const float dx = L.speakers[i].pos[0] - p[0], dy = L.speakers[i].pos[1] - p[1], dz = L.speakers[i].pos[2] - p[2];
        const float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d > r) r = d;
    }
    return r;
}

/* The bump check after one capture. Returns 0 (in place, or no pose to judge by), 4 (bumped: the
 * message is printed, and the caller stops without writing anything). */
static int track_after_capture(double capture_s, const char* what, int idx) {
    if (!g_tracked) return 0;
    float moved = 0.f, turned = 0.f, now[3] = { 0.f, 0.f, 0.f };
    const int b = mic_track_bump_console(&g_mt, g_taken, g_placed.q, g_tol_m, g_turn_limit, capture_s, &moved, &turned, now);
    if (b < 0) { ++g_nunchecked; return 0; }
    ++g_nchecks;
    if (moved > g_max_move) g_max_move = moved;
    if (turned > g_max_turn) g_max_turn = turned;
    g_last_turn = turned;
    track_sim_sync();
    if (b == 0) return 0;
    const char* unit = !strcmp(what, "live aiming") ? "reading" : "speaker";
    if (b & PLACE_BUMP_MOVED) {
        fprintf(stderr, "calibrate: BUMP: the ZM-1 moved %.1f mm during the %s, after %s %d (limit %.1f mm, half the\n"
                        "           tolerance). Center taken (%.4f %.4f %.4f), now (%.4f %.4f %.4f). What was measured\n"
                        "           across a moved mic is wrong, so the run stops and writes nothing: re-place the ZM-1\n"
                        "           and run again.\n",
                moved * 1e3, what, unit, idx, 0.5f * g_tol_m * 1e3f,
                g_taken[0], g_taken[1], g_taken[2], now[0], now[1], now[2]);
        if (b & PLACE_BUMP_TURNED)
            fprintf(stderr, "           It also turned %.2f deg (limit %.2f deg).\n", turned, g_turn_limit);
    } else
        fprintf(stderr, "calibrate: BUMP: the ZM-1 turned %.2f deg during the %s, after %s %d (limit %.2f deg), its\n"
                        "           center %.1f mm from where it was taken. This mode turns capsule arrival differences\n"
                        "           into room directions, and every direction measured after the turn is off by it, so\n"
                        "           the run stops and writes nothing: re-place the ZM-1 and run again.\n",
                turned, what, unit, idx, g_turn_limit, moved * 1e3);
    return 4;
}

static void track_report(const char* what) {
    if (!g_tracked) return;
    char turn[96];
    if (g_turn_limit > 0.f)
        snprintf(turn, sizeof turn, ", the largest turn %.2f deg (limit %.2f deg)", g_max_turn, g_turn_limit);
    else    /* shown, not judged: the pressure proxy does not rotate */
        snprintf(turn, sizeof turn, ", the largest turn %.2f deg (unchecked: this mode reads the center)", g_max_turn);
    printf("placement: %s at the measured center (%.4f %.4f %.4f), mount yaw %.1f deg, tilt %.1f deg;\n"
           "           %d bump check(s), the largest move %.1f mm (limit %.1f mm)%s; %d capture(s) had no live pose to check\n",
           what, g_taken[0], g_taken[1], g_taken[2], g_placed.yaw_deg, g_placed.tilt_deg, g_nchecks,
           g_max_move * 1e3, 0.5f * g_tol_m * 1e3f, turn, g_nunchecked);
    /* the script's own record of its twist, so a run that did not stop can show the twist happened */
    if (g_track_sim && mic_track_sim_twisted(&g_mt))
        printf("track-sim: the stand was twisted %.1f deg about the array center (--track-sim-twist)%s\n",
               MIC_SIM_TWIST_DEG, g_turn_limit > 0.f ? "" : "; this mode reads only the center, so it does not check");
}

/* One speaker's capture and measurement, shared by the trim loop and --verify so the two passes
 * cannot measure differently: calib_measure_speaker (calib_capture.h), which calib_view's Capture tab
 * runs too, so the CLI and the GUI cannot either. This wrapper only adds the console messages. */
/* sweep quality (calib.h, docs/calibration.md "Sweep quality"): the window's latency prior, settled
 * once the device is open (the driver's loop is only known then), and the run's re-sweep count */
static CalibWindow g_win;
static int         g_have_win;
static double      g_window_m = -1.0;     /* --window-m: the position margin, < 0 = per mode */
static int         g_nsweeps, g_nspk_measured, g_nresweep_spk;
static std::vector<float> g_bg;           /* each measurement's raw background (CalibMeasInfo.bg_dbfs) */

/* the margin a mode uses: --window-m when given, else `mode_m` (< 0 = per speaker, surveyed or plan) */
static double mode_window_m(double mode_m) { return g_window_m >= 0.0 ? g_window_m : mode_m; }

/* point a pass at the run's window with this mode's margin; agree = the trims' and --verify's rule */
static void pass_quality(CalibPass& c, const float* mic, double mode_m, int agree) {
    c.mic = mic; c.have_win = g_have_win; c.win = g_win; c.win.pos_m = mode_window_m(mode_m); c.agree = agree;
}

static void sweep_report(const char* what) {
    if (!g_nspk_measured) return;
    printf("%s: %d sweep(s) for %d speaker measurement(s), %d of them re-swept (a rejected or disagreeing sweep)\n",
           what, g_nsweeps, g_nspk_measured, g_nresweep_spk);
    /* the room's own level, read off every capture before its sweep arrives: the Session tab records it
     * per step, so a background rising through the day shows */
    if (g_bg.empty()) return;
    std::vector<float> b = g_bg;
    std::sort(b.begin(), b.end());
    const float med = b[b.size() / 2], loud = b.back();
    if (loud <= CALIB_BG_SILENT_DBFS + 1.f)
        printf("background: silent over %d capture(s) (simulate has no noise floor)\n", (int)b.size());
    else
        printf("background: %.1f dBFS median, %.1f dBFS loudest over %d capture(s), read before each sweep arrives\n",
               med, loud, (int)b.size());
}

struct Pass {
    CalibPass     c;
    float         capsule_spread_db;   /* --zylia: max/min capsule level of the last speaker */
    int           in_first;            /* --input: the first capsule's input, for messages */
};

static int measure_speaker(Pass& P, int s, int through, MeasureResult* out, CalibMeasInfo* info = NULL) {
#ifdef BWA_HAVE_ASIO
    if (!P.c.simulate) {
        printf("  speaker %2d: playing sweep%s...\n", s, through ? " through the output stage" : ""); fflush(stdout); }
#endif
    CalibMeasInfo own;
    CalibMeasInfo& mi = info ? *info : own;
    const int r = calib_measure_speaker(&P.c, s, through, out, &mi);
    /* every re-sweep says why, so a noisy room shows in the log as it happens */
    for (const char* q = mi.log; *q; ) {
        const char* e = strchr(q, '\n');
        const int len = e ? (int)(e - q) : (int)strlen(q);
        printf("  speaker %2d: %s%.*s\n", s, r == CALIB_MEAS_UNCLEAN ? "" : "re-swept, ", len, q);
        q += len + (e ? 1 : 0);
    }
    ++g_nspk_measured;
    g_nsweeps += mi.sweeps;
    if (r == CALIB_MEAS_OK) g_bg.push_back(mi.bg_dbfs);
    if (mi.sweeps > (P.c.agree ? 2 : 1)) ++g_nresweep_spk;
    if (r == CALIB_MEAS_UNCLEAN) {
        fprintf(stderr, "calibrate: speaker %d: no clean measurement in %d sweeps (the window and SNR checks and the\n"
                        "           agreement rule, docs/calibration.md \"Sweep quality\"). The run stops and writes nothing:\n"
                        "           quiet the room, check the routing, or widen the window (--window-m) and run again.\n",
                s, mi.sweeps);
        return 0;
    }
    if (r == CALIB_MEAS_TIMEOUT) { fprintf(stderr, "calibrate: capture timed out on speaker %d\n", s); return 0; }
    if (r == CALIB_MEAS_DEAD) {
        fprintf(stderr, "calibrate: speaker %d: ZM-1 capsule %d (input %d) is dead: its level is non-finite or more than\n"
                        "           %.0f dB under the capsules' median (%.3g against %.3g). Its arrival would be the\n"
                        "           peak of noise and throw the center arrival off by milliseconds, so the run\n"
                        "           stops. Check the routing and the capsule with bwa_zylia_probe.\n",
                s, mi.dead, P.in_first + mi.dead, ZYLIA_PROXY_DEAD_DB, mi.dead_level, mi.median_level);
        return 0;
    }
    if (r != CALIB_MEAS_OK) return 0;
    if (P.c.zylia) P.capsule_spread_db = mi.capsule_spread_db;
    return 1;
}

/* The directivity report the trim run and --verify both print: each speaker's bearing off its axis
 * from the mic and from the listening point, the model's loss at each, the direct share and the
 * resulting factor. */
static void print_directivity(const Layout& L, const float mic[3], const MeasureResult* res, const float* corr, int n) {
    printf("directivity: per-speaker bearing off the acoustic axis, and the trim re-aim\n"
           "             (direct = the gated share of each capture's energy; the model ratio acts on it only)\n");
    printf("             spk  at-mic   at-ref   loss@mic(lo/hi dB)  loss@ref(lo/hi dB)  direct  trim corr\n");
    for (int i = 0; i < n; ++i) {
        float th_mic = layout_speaker_off_axis_deg(&L, (uint32_t)i, mic);
        float th_ref = layout_speaker_off_axis_deg(&L, (uint32_t)i, L.ref);
        float lm, hm, lr, hr;
        directivity_lookup(&L.dir, th_mic, &lm, &hm);
        directivity_lookup(&L.dir, th_ref, &lr, &hr);
        printf("             %3d  %5.1f    %5.1f    %+5.1f / %+5.1f        %+5.1f / %+5.1f        %4.2f   %+.2f dB%s\n",
               i, th_mic, th_ref, lm, hm, lr, hr, res[i].direct_frac, 20.0 * log10(corr[i]),
               th_ref > 30.f ? "   <- aimed > 30 deg off the listening point" : "");
    }
    if (fabsf(mic[0] - L.ref[0]) < 1e-3f && fabsf(mic[1] - L.ref[1]) < 1e-3f && fabsf(mic[2] - L.ref[2]) < 1e-3f)
        printf("             mic is at the listening point: every correction is 0 dB by construction\n");
    /* the direct shares in one line: 1.00 everywhere is an anechoic capture, and the model's ratio
     * then acts on the whole level */
    float fs_[BWA_MAX_CHANNELS];
    for (int i = 0; i < n; ++i) {
        float v = res[i].direct_frac; int b = i;
        while (b > 0 && fs_[b-1] > v) { fs_[b] = fs_[b-1]; --b; } fs_[b] = v;
    }
    printf("directivity: direct share of the level band: min %.2f  median %.2f  max %.2f\n",
           fs_[0], (n & 1) ? fs_[n/2] : 0.5f * (fs_[n/2 - 1] + fs_[n/2]), fs_[n - 1]);
}

/* --aim-sheet: no audio device. The sheet and a readiness summary from the engine's own loader. */
static int aim_sheet(const char* layout_path, const Layout& L, const char* csv) {
    const int n = (int)L.count;
    int has_lp = 0;
    unsigned char expl[BWA_MAX_CHANNELS] = { 0 };
    if (!calib_layout_declared(layout_path, n, &has_lp, expl))
        fprintf(stderr, "calibrate: warning: could not re-read %s for its declared fields; treating every aim as default\n", layout_path);
    int nexpl = 0;
    for (int i = 0; i < n; ++i) nexpl += expl[i];
    int nflag = 0; char err[256] = { 0 };
    if (!calib_write_aim_sheet(csv, &L, expl, CALIB_AIM_SHEET_FLAG_DEG, &nflag, err, sizeof err)) {
        fprintf(stderr, "calibrate: %s\n", err); return 1; }
    printf("aim-sheet: %d speakers from %s -> %s\n", n, layout_path, csv);
    { int np = 0; for (int i = 0; i < n; ++i) np += L.speakers[i].has_plan;
      if (np) printf("  positions and aims: the PLAN (plan_position/plan_aim) for %d of %d speakers, as-built for the rest\n", np, n); }
    printf("  listening point: %s (%.3f %.3f %.3f), height %.3f m (%.2f ft) above the floor\n",
           has_lp ? "declared (listening_point_m)" : "NOT declared: the loader uses the array centroid",
           L.ref[0], L.ref[1], L.ref[2], L.ref[1], L.ref[1] / 0.3048);
    if (!has_lp)
        printf("  -> add \"listening_point_m\": [x, y, z] to the layout, or every default aim points at the centroid\n");
    if (L.dir.nband) printf("  directivity model: present (%d bands); the sheet carries the loss at each speaker's aim error\n", (int)L.dir.nband);
    else             printf("  directivity model: none (tools/directivity/clf_to_json.py --into %s); no loss columns\n", layout_path);
    printf("  explicit aims: %d of %d speakers (the rest point at the listening point by default)\n", nexpl, n);
    printf("  angles: bearing clockwise from above, 0 = room-ahead (+z), 90 = room-right (-x); down-tilt + = below level\n");
    for (int s = 0; s < n; ++s) {
        CalibAimRow r;
        calib_aim_row(&L, s, &r);
        if (!(r.off_deg <= CALIB_AIM_SHEET_FLAG_DEG)) {
            printf("  spk %2d: layout aim is %.1f deg off the listening point (%s aim; set bearing %.1f, down-tilt %.1f)",
                   s, r.off_deg, expl[s] ? "explicit" : "default", r.bearing_deg, r.down_tilt_deg);
            if (r.have_loss) printf("; loss there %.1f dB at 2 kHz, %.1f dB at 16 kHz", r.loss_2k_db, r.loss_16k_db);
            printf("   <- OFF AIM\n");
        }
    }
    printf("aim-sheet: %d speaker(s) aimed more than %.0f deg off the listening point\n", nflag, CALIB_AIM_SHEET_FLAG_DEG);
    return nflag ? 3 : 0;
}

/* ---- --ref-speakers: the system latency from several speakers whose positions are trusted
 * (docs/calibration.md, "Distances need the latency") ----
 * --ref tapes ONE center-to-speaker distance. --ref-speakers takes the distance from the layout instead:
 * |position - the array center| for each listed speaker, the center being the tracked measured one or
 * --mic, so the positions must be MEASURED (bwa_speaker_survey --write --fields position on the
 * camera-visible boxes, or --localize), never the plan: a plan position puts its placement error over c
 * straight into the latency. Each speaker gives its own latency, its pooled center arrival (the same
 * zylia_center_arrival --ref reads) minus distance / c; the MEDIAN is the one used, so one bad box cannot
 * move it, and a box off the median by more than REFSPK_FLAG_S is flagged by name.
 *
 * REFSPK_FLAG_S, 0.1 ms (34 mm at c): in simulation a correct run's speakers agree to a few microseconds
 * (the center arrival's plane-wave bias, 0.3 to 0.6 mm, is 1 to 2 us), and on the rig a 1 cm position
 * error is 29 us, so a correct set sits well under it. What it exists for sits well over it: a box whose
 * Dante receive latency is set differently from the rest is off by a whole setting step (0.25 ms or more
 * between the usual 0.25, 0.5, 1, 2 and 5 ms), and a position wrong by 34 mm along the line to the array
 * is a gross survey error, not noise. */
#define REFSPK_MIN        3          /* the median has to outvote one bad box: three or more */
#define REFSPK_FLAG_S     100e-6     /* a speaker this far off the median is flagged (see above) */
#define REFSPK_PLAN_EPS_M 0.00005f   /* 0.05 mm: the writers round positions to 0.1 mm, so equal is equal */
/* A flag stops nothing while the unflagged speakers are a majority: the median stands without the
 * flagged ones. When they are not, no latency is used and nothing is written: the input is suspect, the
 * same meaning as the capsule survey's exit 6 (CSURVEY_EXIT_OUTLIER below). */
#define REFSPK_EXIT_SPLIT 6

/* The listed speakers whose positions look unmeasured: no plan_position at all (no survey ever wrote
 * one), or a plan_position equal to the position (no survey MOVED it; an aim-only --write records the
 * plan too). A warning, not a refusal: a measured position can land on its plan to the 0.1 mm. */
static void refspk_plan_warnings(const Layout& L, const int* spk, int n, const char* tag) {
    for (int k = 0; k < n; ++k) {
        const Speaker& s = L.speakers[spk[k]];
        if (!s.has_plan) {
            printf("%s: WARNING --ref-speakers %d carries no plan_position, so no survey ever wrote its position: it is\n"
                   "%*s the plan or a typed value, and its placement error over c lands in its latency (1 cm is 29 us)\n",
                   tag, spk[k], (int)strlen(tag) + 1, "");
            continue;
        }
        float d2 = 0.f;
        for (int a = 0; a < 3; ++a) d2 += (s.pos[a] - s.plan_pos[a]) * (s.pos[a] - s.plan_pos[a]);
        if (d2 <= REFSPK_PLAN_EPS_M * REFSPK_PLAN_EPS_M)
            printf("%s: WARNING --ref-speakers %d: its plan_position equals its position, so it was never measured\n"
                   "%*s (bwa_speaker_survey --write --fields aim records the plan too). If it is the plan, its placement\n"
                   "%*s error over c lands in its latency (1 cm is 29 us).\n",
                   tag, spk[k], (int)strlen(tag) + 1, "", (int)strlen(tag) + 1, "");
    }
}

static double median_of(double* v, int n) {   /* sorts v in place */
    std::sort(v, v + n);
    return (n & 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/* carr_s[k]: speaker spk[k]'s pooled center arrival (s). Prints the table, the median, the spread and the
 * flags; *lat_s = the median. Returns 0, or REFSPK_EXIT_SPLIT when the unflagged speakers are not a
 * majority (nothing to use). */
static int refspk_latency(const Layout& L, const int* spk, int n, const double* carr_s, const float center[3],
                          const char* center_what, double C, const char* tag, double* lat_s) {
    double lat[BWA_MAX_CHANNELS], srt[BWA_MAX_CHANNELS], dist[BWA_MAX_CHANNELS];
    for (int k = 0; k < n; ++k) {
        const float* p = L.speakers[spk[k]].pos;
        const double dx = p[0] - center[0], dy = p[1] - center[1], dz = p[2] - center[2];
        dist[k] = sqrt(dx * dx + dy * dy + dz * dz);
        lat[k] = carr_s[k] - dist[k] / C;
        srt[k] = lat[k];
    }
    const double med = median_of(srt, n);
    const int w = (int)strlen(tag) + 1;
    printf("%s: system latency from --ref-speakers: each speaker's center arrival minus its layout distance from the\n"
           "%*s array center over c, the center (%.4f %.4f %.4f) from %s; flagged over %.0f us\n"
           "%*s (%.0f mm at c) off the median:\n",
           tag, w, "", center[0], center[1], center[2], center_what, REFSPK_FLAG_S * 1e6, w, "", REFSPK_FLAG_S * C * 1e3);
    int nflag = 0;
    double lo = 0, hi = 0, glo = 0, ghi = 0;
    int ng = 0;
    for (int k = 0; k < n; ++k) {
        const double dev = lat[k] - med;
        const int flag = fabs(dev) > REFSPK_FLAG_S;
        nflag += flag;
        if (k == 0 || lat[k] < lo) lo = lat[k];
        if (k == 0 || lat[k] > hi) hi = lat[k];
        if (!flag) { if (!ng || lat[k] < glo) glo = lat[k]; if (!ng || lat[k] > ghi) ghi = lat[k]; ++ng; }
        printf("  spk %2d: %.3f m, center arrival %.4f ms, latency %.4f ms, %+7.1f us from the median%s\n", spk[k], dist[k],
               carr_s[k] * 1e3, lat[k] * 1e3, dev * 1e6, flag ? "   FLAGGED" : "");
    }
    if (2 * (n - nflag) <= n) {
        fprintf(stderr, "calibrate: --ref-speakers: %d of the %d speakers are more than %.0f us off the median, so no majority\n"
                        "           agrees on one latency and none is used; nothing written. Check their Dante latency\n"
                        "           settings and their positions, or list other speakers.\n",
                nflag, n, REFSPK_FLAG_S * 1e6);
        return REFSPK_EXIT_SPLIT;
    }
    *lat_s = med;
    printf("%s: latency %.4f ms (%.4f m at c), the median of %d; spread %.1f us (max - min)", tag, med * 1e3, med * C, n,
           (hi - lo) * 1e6);
    if (nflag) printf(", %.1f us without the flagged", (ghi - glo) * 1e6);
    printf("; --latency %.4f carries it to the next run\n", med * C);
    for (int k = 0; k < n; ++k) {
        const double dev = lat[k] - med;
        if (fabs(dev) <= REFSPK_FLAG_S) continue;
        printf("%s: WARNING speaker %d arrives %.1f us %s than its layout distance says (%.0f mm at c), over the %.0f us\n"
               "%*s limit: its Dante latency setting differs from the others', or its layout position is off along the\n"
               "%*s line to the array. The median stands without it; anything this run measures from speaker %d carries it.\n",
               tag, spk[k], fabs(dev) * 1e6, dev > 0 ? "later" : "earlier", fabs(dev) * C * 1e3, REFSPK_FLAG_S * 1e6,
               w, "", w, "", spk[k]);
    }
    return 0;
}

/* ---- --live N --zylia: live aiming (docs/calibration.md, "Live aiming") ----
 * Sweep ONE speaker over and over with the short live sweep, capture the 19 capsules, and print one
 * line per sweep: where the box is against its PLAN (zylia_live_position against plan_position, or
 * the layout's position when the file carries no plan) with the move in installer words
 * (place_move_words), and how far its axis is off the direction to the mic (the direct-sound tilt, as
 * a peak meter and as an estimated MAGNITUDE; one mic position never says which way the box points).
 * Keys on the rig: r stores this reading as the on-axis reference, p resets the peak, any other key
 * stops. The simulated box starts at the AS-BUILT position and aim (position/aim) plus --sim-move:
 * the box is where the last survey put it, and the plan is where it should go. */
struct LiveArgs {
    const Layout* L;
    int     spk;
    float   mic[3];
    double  sos;
    int     simulate;
    double  known_latency_m;    /* --latency, < 0 = none */
    int     ref_spk; double ref_dist;   /* --ref (latency from a taped distance), ref_spk < 0 = none */
    const int* ref_spks; int nref_spks; /* --ref-speakers (latency from trusted positions), 0 = none */
    const char* center_what;            /* where mic came from, for the --ref-speakers report */
    int     aim_ref_spk;        /* --aim-ref: sweep this speaker first, its tilt is the 0 deg reference */
    int     have_aim_ref_db; float aim_ref_db;   /* --aim-ref-db: a stored reference */
    int     sweeps;             /* 0 = until a key (the rig); simulate defaults to the step count or 4 */
    float   sim_move[3];        /* --sim-move */
    float   sim_screen_db;      /* --sim-screen */
    const float* sim_steps; int nsteps;   /* --sim-aim-steps */
    float*  cap19;              /* [19][CAL_CAPLEN], the ASIO shell's rows */
    int     in_first;
    int     have_survey;
    const float* sim_at;        /* --track-sim: the true center the simulated readings come from */
    int     pos_ok;             /* 0 = tracked with no body-frame survey: no position readout */
};

static void live_true_aim(const Layout* L, int s, float deg, float out[3]) {
    calib_sim_rotate(L->speakers[s].aim, deg, out);
}

/* live aiming's window (NULL until there is a latency to time it from): the run's prior, or the
 * latency --ref measured, with the PLAN margin around the as-built and the plan (calib_live_read) */
static CalibWindow g_live_win;
static int         g_live_have_win;

static int live_sweep_one(const LiveArgs& A, int spk, float aim_err_deg, const float* lsweep, CalibLiveReading* r) {
    float tp[3], ta[3];
    CalibSimOpts o; memset(&o, 0, sizeof o);
    o.on_axis = 1; o.screen_db = A.sim_screen_db;
    if (A.simulate && spk == A.spk) {                    /* only the live speaker is being moved and turned */
        for (int a = 0; a < 3; ++a) tp[a] = A.L->speakers[spk].pos[a] + A.sim_move[a];
        live_true_aim(A.L, spk, aim_err_deg, ta);
        o.true_pos = tp; o.true_aim = ta;
    }
    if (!calib_live_read(spk, A.L, A.sim_at ? A.sim_at : A.mic, A.sos, A.simulate, &o, lsweep, A.cap19, r,
                         g_live_have_win ? &g_live_win : NULL, A.mic)) {
        fprintf(stderr, "\ncalibrate: capture timed out on speaker %d\n", spk); return 0;
    }
    if (!r->ok)
        printf("  speaker %d: ZM-1 capsule %d (input %d) is dead or silent; reading skipped (bwa_zylia_probe)\n",
               spk, r->dead, A.in_first + r->dead);
    return 1;
}

/* live aiming's latency prior: the measured one (--latency, --ref, the simulator's own) when there is
 * one, else the run's (the driver's loop, or none) */
static void live_window(int lat_known, double latency_s, int simulate) {
    g_live_have_win = g_have_win;
    g_live_win = g_win;
    if (lat_known) {
        g_live_have_win = 1;
        g_live_win.lat_s = latency_s;
        g_live_win.lat_early_s = g_live_win.lat_late_s = simulate ? 0.0 : CALIB_WIN_LAT_KNOWN_S;
    }
    g_live_win.pos_m = mode_window_m(CALIB_WIN_PLAN_M);   /* the box is being moved: never the surveyed margin */
}

/* "24.6 deg [19.4-29.7]", "on axis (under 16 deg)", ">= 90 deg" or "n/a" */
static void fmt_aim(const CalibAimAngle& a, int have, char* buf, size_t cap) {
    if (!have || !a.ok) snprintf(buf, cap, "n/a");
    else if (a.beyond)  snprintf(buf, cap, ">= %.0f deg", a.max_deg);
    else if (a.on_axis) snprintf(buf, cap, "on axis (under %.0f deg)", a.hi_deg);
    else                snprintf(buf, cap, "%.1f deg [%.1f-%.1f]", a.angle_deg, a.lo_deg, a.hi_deg);
}

static int live_zylia(const LiveArgs& A) {
    const Layout& L = *A.L;
    const int s = A.spk;
    const double C = A.sos;
    static float lsweep[CAL_LIVE_NSWEEP];
    measure_sweep(lsweep, CAL_LIVE_NSWEEP, F1, F2, FS);
    static float curve[CALIB_AIM_CURVE_N];
    const int have_model = L.dir.nband > 0;
    if (have_model) calib_aim_curve(&L.dir, LIVE_BAND_HZ, F2, curve);
    float tilt0_file = 0.f;
    const int have_file = have_model && calib_on_axis_tilt_db(&L.dir, LIVE_BAND_HZ, F2, &tilt0_file);
    /* the target is the PLAN: plan_position / plan_aim when the file carries them, else the layout's
     * position and aim (a record with no plan is its own plan). The engine renders from the as-built
     * position; the installer works toward the plan. */
    const int has_plan = L.speakers[s].has_plan;
    const float* tp = layout_plan_pos(&L, (uint32_t)s);       /* the target */
    const float* ta = layout_plan_aim(&L, (uint32_t)s);
    const float* bp = L.speakers[s].pos;                      /* as-built: the simulated box starts here */
    const float plan_deg = directivity_off_axis_deg(tp, ta, A.mic);
    const double dx = tp[0] - A.mic[0], dy = tp[1] - A.mic[1], dz = tp[2] - A.mic[2];

    printf("live: speaker %d against its %s, ZM-1 at (%.3f %.3f %.3f), target (%.3f %.3f %.3f), %.3f m away%s\n",
           s, has_plan ? "PLAN (plan_position, plan_aim)" : "layout position and aim (the file carries no plan)",
           A.mic[0], A.mic[1], A.mic[2], tp[0], tp[1], tp[2], sqrt(dx * dx + dy * dy + dz * dz),
           A.simulate ? "  [SIMULATE]" : "");
    if (has_plan) {   /* where the last survey put it against the plan: what this session is here to fix */
        const float e[3] = { bp[0] - tp[0], bp[1] - tp[1], bp[2] - tp[2] };
        char words[160];
        place_move_words(e, PLACE_MOVE_DEAD_BOX_M, words, sizeof words);
        const float* aa = L.speakers[s].aim;
        float c = aa[0] * ta[0] + aa[1] * ta[1] + aa[2] * ta[2];
        c = c > 1.f ? 1.f : (c < -1.f ? -1.f : c);
        printf("live: as-built (the layout's position and aim) is %.1f mm from the plan (%s), its aim %.1f deg\n"
               "      off the plan aim\n", sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]) * 1e3f, words,
               acosf(c) * (180.f / 3.14159265f));
    }
    printf("live: %.2f s sweep + %.2f s tail per reading; tilt = direct-sound %.0f Hz up against %.0f-%.0f Hz\n",
           CAL_LIVE_NSWEEP / FS, CAL_LIVE_NTAIL / FS, LIVE_BAND_HZ[1], LIVE_BAND_HZ[0], LIVE_BAND_HZ[1]);
    printf("live: the %s aim is %.1f deg off the mic. The measured angle is a MAGNITUDE: one mic position\n"
           "      cannot say which way the box points. Turn the box until the tilt peaks.\n",
           has_plan ? "plan's" : "layout's", plan_deg);
    if (!have_model)
        printf("live: no directivity model in the layout: the peak meter works, the angle estimate does not\n"
               "      (tools/directivity/clf_to_json.py --into <layout>)\n");
    else if (have_file)
        printf("live: 0 deg tilt from the file's on_axis_db: %+.2f dB (a reference speaker replaces it)\n", tilt0_file);
    else
        printf("live: the model has no on_axis_db, so the angle needs a reference (--aim-ref, --aim-ref-db, or r)\n");
    if (!A.pos_ok)
        printf("live: tracked with no body-frame survey: the POSITION readout is off. It turns capsule arrival\n"
               "      differences into a room direction, which needs the array's orientation; the tilt meter does not.\n");
    else if (!A.simulate && !A.have_survey)
        printf("live: no --survey: the built-in capsule table. The channel order and the ZM-1's yaw are unpinned,\n"
               "      and a yaw error rotates the measured direction (docs/calibration.md, capsule self-survey).\n");

    /* the latency: --ref (one taped distance) beats --latency; simulate knows its own */
    double latency = 0.0; int lat_known = 0;
    if (A.known_latency_m >= 0.0) { latency = A.known_latency_m / C; lat_known = 1; }
    else if (A.simulate)          { latency = CAL_SIM_LATENCY_SAMPLES / FS; lat_known = 1; }
    live_window(lat_known, latency, A.simulate);
    CalibLiveReading rd;
    if (A.ref_spk >= 0) {
        if (!live_sweep_one(A, A.ref_spk, 0.f, lsweep, &rd)) return 1;
        if (!rd.ok) { fprintf(stderr, "calibrate: --ref speaker %d gave no reading\n", A.ref_spk); return 1; }
        latency = zylia_center_arrival(rd.arr, C) - A.ref_dist / C; lat_known = 1;
        live_window(lat_known, latency, 0);                  /* a measured loop now times the readings */
        printf("live: system latency from --ref %d @ %.3f m: %.3f ms (%.3f m at c)%s\n", A.ref_spk, A.ref_dist,
               latency * 1e3, latency * C, A.ref_spk == s ? "; the live speaker's distance now reads the tape" : "");
    } else if (A.nref_spks > 0) {
        /* one live sweep per listed speaker, before any reading: each one's pooled center arrival */
        double carr[BWA_MAX_CHANNELS];
        for (int k = 0; k < A.nref_spks; ++k) {
            if (!live_sweep_one(A, A.ref_spks[k], 0.f, lsweep, &rd)) return 1;
            if (!rd.ok) { fprintf(stderr, "calibrate: --ref-speakers speaker %d gave no reading\n", A.ref_spks[k]); return 1; }
            carr[k] = zylia_center_arrival(rd.arr, C);
        }
        double lr = 0.0;
        if (const int rc = refspk_latency(L, A.ref_spks, A.nref_spks, carr, A.mic, A.center_what, C, "live", &lr)) return rc;
        if (A.known_latency_m >= 0.0)
            printf("live: --ref-speakers solves %.4f m against --latency %.4f m (%+.1f mm); using --ref-speakers\n",
                   lr * C, A.known_latency_m, (lr * C - A.known_latency_m) * 1e3);
        latency = lr; lat_known = 1;
        live_window(lat_known, latency, 0);                  /* a measured loop now times the readings */
    } else if (A.known_latency_m >= 0.0)
        printf("live: system latency from --latency: %.3f ms (%.3f m at c)\n", latency * 1e3, A.known_latency_m);
    else if (A.simulate)
        printf("live: system latency: the simulator's own %d samples\n", CAL_SIM_LATENCY_SAMPLES);
    else
        printf("live: no --latency, --ref or --ref-speakers: direction only, no distance or position (--ref <spk> <m>\n"
               "      needs one tape, --ref-speakers <list> measured positions)\n");

    int have_ref = 0; float tilt_ref = 0.f;
    if (A.have_aim_ref_db) { have_ref = 1; tilt_ref = A.aim_ref_db; printf("live: on-axis reference %+.2f dB (--aim-ref-db)\n", tilt_ref); }
    if (A.aim_ref_spk >= 0) {
        if (!live_sweep_one(A, A.aim_ref_spk, 0.f, lsweep, &rd)) return 1;
        if (!rd.ok || !rd.have_tilt) { fprintf(stderr, "calibrate: --aim-ref speaker %d gave no tilt\n", A.aim_ref_spk); return 1; }
        if (rd.quality != CALIB_SWEEP_OK) {
            fprintf(stderr, "calibrate: --aim-ref speaker %d: the reference sweep is not clean (%s); run again\n", A.aim_ref_spk, rd.why);
            return 1;
        }
        have_ref = 1; tilt_ref = rd.tilt_db;
        printf("live: on-axis reference from speaker %d: tilt %+.2f dB (reuse it with --aim-ref-db %.2f)\n",
               A.aim_ref_spk, tilt_ref, tilt_ref);
    }
#ifdef BWA_HAVE_ASIO
    if (!A.simulate) printf("live: keys: r = store this reading as the on-axis reference, p = reset the peak, any other key stops\n");
#endif

    const int nsw = A.sweeps > 0 ? A.sweeps : (A.simulate ? (A.nsteps > 0 ? A.nsteps : 4) : (1 << 30));
    CalibPeakHold pk; calib_peak_reset(&pk);
    int npk_at = 0; float peak_true_deg = 0.f;
    const auto t0 = std::chrono::steady_clock::now();    /* wall time: the rig's update rate is the point */
    int done = 0;
    for (int t = 0; t < nsw; ++t) {
        const float aim_err = A.nsteps > 0 ? A.sim_steps[t < A.nsteps ? t : A.nsteps - 1] : 0.f;
        float true_deg = 0.f;                              /* simulate: the true axis against the mic */
        if (A.simulate) {
            float tra[3]; live_true_aim(&L, s, aim_err, tra);
            const float tq[3] = { bp[0] + A.sim_move[0], bp[1] + A.sim_move[1], bp[2] + A.sim_move[2] };
            true_deg = directivity_off_axis_deg(tq, tra, A.mic);
        }
        if (!live_sweep_one(A, s, aim_err, lsweep, &rd)) return 1;
        ++done;
        const int bump = track_after_capture(LIVE_CAPTURE_S, "live aiming", t + 1);
        if (bump) return bump;
        if (!rd.ok) continue;
        ZyliaLivePos lp;
        memset(&lp, 0, sizeof lp);
        if (A.pos_ok) zylia_live_position(rd.arr, A.mic, lat_known, latency, C, tp, &lp);
        char line[1400]; size_t k = 0;
        k += snprintf(line + k, sizeof line - k, "  #%-3d", t + 1);
        if (lp.ok && lp.have_distance) {
            /* the deltas are measured minus the target; the words are the move that undoes them */
            const float e[3] = { lp.delta_mm[0] * 1e-3f, lp.delta_mm[1] * 1e-3f, lp.delta_mm[2] * 1e-3f };
            char words[160];
            place_move_words(e, PLACE_MOVE_DEAD_BOX_M, words, sizeof words);
            k += snprintf(line + k, sizeof line - k, " pos %+6.1f %+6.1f %+6.1f mm (|d| %5.1f)  dir %.2f deg  dist %.3f m (%+.1f mm) | %s",
                          lp.delta_mm[0], lp.delta_mm[1], lp.delta_mm[2], lp.delta_norm_mm, lp.dir_err_deg, lp.dist_m, lp.dist_err_mm,
                          words);
        } else if (lp.ok)
            k += snprintf(line + k, sizeof line - k, " dir %.2f deg off the %s (no distance)", lp.dir_err_deg, has_plan ? "plan" : "layout");
        else if (!A.pos_ok)
            k += snprintf(line + k, sizeof line - k, " position: n/a (no body-frame survey)");
        else
            k += snprintf(line + k, sizeof line - k, " position: DOA solve failed");
        if (rd.have_tilt) {
            /* the peak holds only clean readings, and only once two in a row agree (calib.h) */
            const int clean = rd.quality == CALIB_SWEEP_OK;
            const float below = calib_peak_update(&pk, rd.tilt_db, clean, CALIB_LIVE_TILT_TOL_DB);
            if (pk.peak_index && pk.peak_index == pk.n) { npk_at = t + 1; peak_true_deg = true_deg; }
            if (pk.peak_index)
                k += snprintf(line + k, sizeof line - k, " | tilt %+.2f dB  peak %+.2f  below %.2f dB", rd.tilt_db, pk.peak_db, below);
            else
                k += snprintf(line + k, sizeof line - k, " | tilt %+.2f dB  peak -- (two clean readings that agree set it)", rd.tilt_db);
            if (!clean) k += snprintf(line + k, sizeof line - k, " | NOT HELD: %s", rd.why);
            char ba[64] = "", bb[64] = "";
            CalibAimAngle ar, af;
            if (have_model && have_ref)  { calib_aim_invert(curve, rd.tilt_db - tilt_ref, CALIB_LIVE_TILT_TOL_DB, &ar); fmt_aim(ar, 1, ba, sizeof ba); }
            if (have_model && have_file) { calib_aim_invert(curve, rd.tilt_db - tilt0_file, CALIB_LIVE_TILT_TOL_DB, &af); fmt_aim(af, 1, bb, sizeof bb); }
            if (ba[0] || bb[0])
                k += snprintf(line + k, sizeof line - k, " | off-axis%s%s%s%s%s", ba[0] ? " ref " : "", ba,
                              ba[0] && bb[0] ? "," : "", bb[0] ? " file " : "", bb);
        } else
            k += snprintf(line + k, sizeof line - k, " | tilt: no direct-sound level in a band");
        k += snprintf(line + k, sizeof line - k, " | %s %.1f deg", has_plan ? "plan" : "layout", plan_deg);
        if (A.simulate) k += snprintf(line + k, sizeof line - k, " | true %.1f deg", true_deg);
        if (g_tracked && g_turn_limit > 0.f) k += snprintf(line + k, sizeof line - k, " | turned %.2f deg", g_last_turn);
        printf("%s\n", line);
        fflush(stdout);
#ifdef BWA_HAVE_ASIO
        if (!A.simulate && _kbhit()) {
            const int ch = _getch();
            if ((ch == 'r' || ch == 'R') && rd.have_tilt) {
                have_ref = 1; tilt_ref = rd.tilt_db;
                printf("live: on-axis reference stored: tilt %+.2f dB (reuse it with --aim-ref-db %.2f)\n", tilt_ref, tilt_ref);
            } else if (ch == 'p' || ch == 'P') {
                calib_peak_reset(&pk);
                printf("live: peak reset\n");
            } else break;
        }
#endif
    }
    const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (pk.nrejected)
        printf("live: %d reading(s) not held by the peak (outside the window or under the SNR floor)\n", pk.nrejected);
    if (pk.peak_index) {
        printf("live: peak tilt %+.2f dB at reading %d", pk.peak_db, npk_at);
        if (A.simulate) printf(", where the true axis was %.1f deg off the mic", peak_true_deg);
        printf("\n");
    }
    if (done > 0)
        printf("live: %d reading(s) in %.1f s, %.2f per second%s\n", done, el, el > 0.0 ? done / el : 0.0,
               A.simulate ? " (simulate: the simulator's cost, not the rig's rate)" : "");
    return 0;
}

/* ---- --capsule-survey out.json --zylia: the capsule table from the speakers (docs/calibration.md,
 * "The speaker-sweep capsule survey") ----
 * zylia_survey wants source positions relative to the array center and per-capsule arrivals. The clap
 * survey gets both by hand; here the sources are the speakers, at the layout's as-built positions, and
 * the arrivals come from one sweep each, timed by cross-correlating the capsules' impulse responses
 * (calib_measure_speaker -> calib_measure_zylia_rows -> zylia_ir_tdoa), the path every ZM-1 sweep
 * takes. Three solves:
 *   1. the table at the given center (the tracked center, or --mic);
 *   2. the array CENTER, from the absolute center arrivals the survey itself throws away: with that
 *      table installed, zylia_center_arrival per speaker is a range plus the system latency, and
 *      calib_trilaterate solves the center and the latency from them, the speakers standing in for
 *      --localize's mic rows;
 *   3. the table again, at that center.
 * Why 2 is a measurement and the survey's own origin is not: zylia_survey subtracts each observation's
 * mean, so a translation of the whole capsule cloud is invisible to it, and it pins the origin by
 * re-centering on the fitted sphere, which puts the sphere's center exactly at the point the source
 * positions were given relative to. Read as a "measured center" it would hand back the tracked center
 * bit for bit. The center arrivals carry the translation the means removed.
 * What 2 is measured against: the speaker positions. If those came from --localize --zylia --track with
 * the same mount offset and the stand never turned, an offset error shifted them by exactly the error
 * it shifts the tracked center by, and the two agree whatever the offset is. So the comparison checks
 * the offset only as far as the positions do not share it: a stand turned between rows, or positions
 * from another source.
 *
 * Leave-one-out (zylia_survey_loo) on the solve: the residual absorbs most of ONE speaker whose position
 * is off (CLAUDE.md, "a survey's residual is a weak detector"), so each speaker is scored against the
 * table the rest solve to. That sees a position error ACROSS the line of sight; one ALONG it moves no
 * direction and is the range check's (CSURVEY_RANGE_FLAG_MM): each speaker's range residual from solve 2
 * against the median residual. A box whose Dante latency differs from the rest lands there too: it delays
 * every capsule alike, which no direction shows. A flag of either kind refuses the write
 * (CSURVEY_EXIT_OUTLIER). --drop-outliers drops the
 * worst flagged speaker and re-runs all three solves without it, so it is out of the center's
 * trilateration too, then checks again. A speaker flagged in THAT check stops the run: see
 * CSURVEY_MAX_DROPS below. */
#define CSURVEY_MIN_SPK        4      /* zylia_survey's own floor */
#define CSURVEY_REC_SPK        6      /* what zylia.h recommends */
#define CSURVEY_CENTER_MIN_SPK 5      /* calib_trilaterate's floor, for the acoustic center */
#define CSURVEY_OFFSET_WARN_MM 5.f    /* acoustic against tracked: past this, the offset or the positions are wrong */
#define CSURVEY_RESID_WARN_US  5.f    /* the clap survey's "sub-microsecond is clean, tens is bad" */
/* The range check's flag: a speaker's range residual this far off the median one. Leave-one-out sees a
 * position error ACROSS the line of sight (a direction); an error ALONG it barely moves any direction (a
 * 40 mm radial move held out at 0.02 us against the 3 us floor), but it moves the range, which the center's
 * trilateration reads, and a 35 mm one put the acoustic center 4.9 mm off. A correct run's residuals sit
 * within +/-0.2 mm of the median in simulation (the center arrival's plane-wave bias, 0.3 to 0.6 mm at
 * most), so 10 mm is 50 times that and still a quarter of the radial error the ctest injects. Not 5 mm: a
 * bad speaker bends the least-squares center, and with a 35 mm one in the solve the GOOD speakers' residuals
 * reached 4.6 mm off the median, which 10 mm keeps clear behind an outlier up to about 75 mm (a worse one
 * still flags first, and --drop-outliers drops it). A 10 mm range error moves the center about 1.4 mm, past
 * the 1 mm the clean center lands within. */
#define CSURVEY_RANGE_FLAG_MM  10.0
/* Leave-one-out flagged a speaker and nothing was written. Not 3: --verify's and --check's "flagged" means
 * a measured result to read, while this one means the survey's INPUT is suspect. Not 5: bwa_validate's
 * track self-check, and the Session tab maps every tool's codes through one table. */
#define CSURVEY_EXIT_OUTLIER   6
struct CSurveyArgs {
    const Layout* L;
    const int*    spk;
    int           nspk;
    float         mic[3];        /* tracked: the target; untracked: --mic, the center to start from */
    double        sos;
    int           simulate;
    const float*  sweep;
    float*        cap;
    float*        cap19;
    int           in_first;
    const char*   out_path;
    int           have_sim_center;   /* untracked simulate: the array's TRUE center (--sim-center), else --mic */
    float         sim_center[3];
    double        known_latency_m;   /* --latency (c * tau, m), < 0 = solve it here */
    int           drop_outliers;     /* --drop-outliers: drop the speaker leave-one-out flags, solve again */
};

/* the speakers spk[0..n) seen from `center`: 0 ok, else rc_fail with the reason printed */
static int csurvey_check(const CSurveyArgs& A, const int* spk, int n, const float center[3], const char* at, int rc_fail) {
    static float src[ZYLIA_SURVEY_MAX][3];
    for (int k = 0; k < n; ++k) {
        const float* p = A.L->speakers[spk[k]].pos;
        for (int a = 0; a < 3; ++a) src[k][a] = p[a] - center[a];
        const float d = sqrtf(src[k][0] * src[k][0] + src[k][1] * src[k][1] + src[k][2] * src[k][2]);
        if (!(d >= 0.2f)) {
            fprintf(stderr, "calibrate: --capsule-survey: speaker %d is %.0f mm from %s (%.3f %.3f %.3f); every source\n"
                            "           must be 0.2 m or more from the array\n", spk[k], d * 1e3f, at, center[0], center[1], center[2]);
            return rc_fail;
        }
    }
    const double sp = zylia_survey_spread(src, n);
    if (!(sp >= ZYLIA_SURVEY_MIN_SPREAD)) {
        fprintf(stderr, "calibrate: --capsule-survey: the %d speakers' directions from %s have a spread of %.3f, under\n"
                        "           the survey's %.2f floor (1 = all round, 0 = one plane through the array). Speakers in a\n"
                        "           ring around the array leave the capsules' heights unconstrained: add speakers above\n"
                        "           and below it.\n", n, at, sp < 0.0 ? 0.0 : sp, ZYLIA_SURVEY_MIN_SPREAD);
        return rc_fail;
    }
    return 0;
}

/* speaker indices as a --speakers list ("0-14,16-25"), ascending runs collapsed */
static void fmt_speakers(const int* spk, int n, char* out, size_t cap) {
    size_t k = 0;
    out[0] = 0;
    for (int i = 0; i < n && k < cap; ) {
        int j = i;
        while (j + 1 < n && spk[j + 1] == spk[j] + 1) ++j;
        const int w = j > i ? snprintf(out + k, cap - k, "%s%d-%d", i ? "," : "", spk[i], spk[j])
                            : snprintf(out + k, cap - k, "%s%d", i ? "," : "", spk[i]);
        if (w < 0) break;
        k += (size_t)w;
        i = j + 1;
    }
}

/* One pass of the survey's three solves over a subset of the swept speakers (use[0..n), indices into the
 * per-sweep arrays), so a dropped speaker is out of every one of them, the center's trilateration
 * included. */
struct CSolve {
    float  caps[ZYLIA_MICS][3];
    float  resid, radius, spread;
    float  cen[3];                 /* the acoustic center, else the center the solve started from */
    double lat_m, fit_rms, dil;
    int    have_ac, lat_given, lat_trusted;
    /* the range check (CSURVEY_RANGE_FLAG_MM), per speaker of the solve, index k into use[] */
    int    rchecked;               /* 1 = it ran (an acoustic center, and two speakers of redundancy) */
    double rmed_mm;                /* the median range residual: what every speaker shares */
    double rdev_mm[ZYLIA_SURVEY_MAX];   /* each one's residual against that median */
    int    rflag[ZYLIA_SURVEY_MAX];
    int    nrflag;
};
static int csurvey_solve(const CSurveyArgs& A, const float center[3], const float (*spos_all)[3],
                         const double (*arr_all)[ZYLIA_MICS], const int* use, int n, CSolve* S) {
    static float  spos[ZYLIA_SURVEY_MAX][3], src[ZYLIA_SURVEY_MAX][3];
    static double arr[ZYLIA_SURVEY_MAX][ZYLIA_MICS], rng[ZYLIA_SURVEY_MAX];
    const double C = A.sos;
    for (int k = 0; k < n; ++k) {
        memcpy(spos[k], spos_all[use[k]], sizeof spos[k]);
        memcpy(arr[k], arr_all[use[k]], sizeof arr[k]);
    }
    memset(S, 0, sizeof *S);
    /* 1. the table at the given center */
    for (int k = 0; k < n; ++k) for (int a = 0; a < 3; ++a) src[k][a] = spos[k][a] - center[a];
    if (!zylia_survey(src, (const double (*)[ZYLIA_MICS])arr, n, C, S->caps, &S->resid, &S->radius, &S->spread)) {
        fprintf(stderr, "calibrate: --capsule-survey: the solve failed (spread %.3f); nothing written\n", S->spread);
        return 0;
    }
    /* 2. the center, from the center arrivals against the speaker positions */
    zylia_set_capsules(S->caps);
    for (int k = 0; k < n; ++k) rng[k] = C * zylia_center_arrival(arr[k], C);
    memcpy(S->cen, center, sizeof S->cen);
    /* With --latency the center is three unknowns. Without it calib_trilaterate solves four, and on a dome
     * around the listening point (every speaker at about one distance, all above the floor) the latency
     * and the center's radial coordinate trade off: in simulation 3.5 mm of position error moved the
     * latency 41 mm while the center held to 0.7 mm. The dilution says how far to trust it. */
    S->lat_given = A.known_latency_m >= 0.0;
    S->dil = -1.0;
    if (S->lat_given) {
        S->lat_m = A.known_latency_m;
        S->have_ac = n >= 4 && calib_locate_known_latency(rng, spos, n, S->lat_m, S->cen);
    } else {
        S->have_ac = n >= CSURVEY_CENTER_MIN_SPK && calib_trilaterate(rng, spos, n, S->cen, &S->lat_m)
                  && calib_trilaterate_refine(rng, spos, n, S->cen, &S->lat_m);
        if (S->have_ac) S->dil = calib_latency_dilution(spos, n, S->cen);
    }
    S->lat_trusted = S->lat_given || (S->dil > 0.0 && S->dil <= 3.0);
    if (!S->have_ac) { memcpy(S->cen, center, sizeof S->cen); return 1; }
    /* The range check: each speaker's range residual against the MEDIAN residual, so an error every
     * speaker shares (a latency that is off, given or poorly pinned) moves the median and flags nobody.
     * Identifying one bad range takes two speakers more than the unknowns (3 with the latency given, 4
     * without): one more only detects that something is off, not which. */
    static double e_mm[ZYLIA_SURVEY_MAX], srt[ZYLIA_SURVEY_MAX];
    const int nunk = S->lat_given ? 3 : 4;
    for (int k = 0; k < n; ++k) {
        const double dx = spos[k][0] - S->cen[0], dy = spos[k][1] - S->cen[1], dz = spos[k][2] - S->cen[2];
        const double e = sqrt(dx * dx + dy * dy + dz * dz) + S->lat_m - rng[k];
        S->fit_rms += e * e;
        e_mm[k] = e * 1e3;
        srt[k] = e_mm[k];
    }
    S->fit_rms = sqrt(S->fit_rms / n);
    S->rchecked = n - nunk >= 2;
    S->rmed_mm = median_of(srt, n);
    if (S->rchecked)
        printf("capsule survey: per speaker, its range from the acoustic center against its position, and that residual\n"
               "                against the median one (mm; flagged over %.0f mm off the median):\n", CSURVEY_RANGE_FLAG_MM);
    else
        printf("capsule survey: per speaker, its range from the acoustic center against its position (mm; the range check\n"
               "                needs %d speakers or more and has %d, so none is flagged):\n", nunk + 2, n);
    for (int k = 0; k < n; ++k) {
        const double dx = spos[k][0] - S->cen[0], dy = spos[k][1] - S->cen[1], dz = spos[k][2] - S->cen[2];
        S->rdev_mm[k] = e_mm[k] - S->rmed_mm;
        S->rflag[k] = S->rchecked && fabs(S->rdev_mm[k]) > CSURVEY_RANGE_FLAG_MM;
        S->nrflag += S->rflag[k];
        printf("  spk %2d: %.3f m, %+6.2f mm, %+6.2f from the median%s\n", A.spk[use[k]], sqrt(dx * dx + dy * dy + dz * dz),
               e_mm[k], S->rdev_mm[k], S->rflag[k] ? "   RANGE FLAGGED: its position or its arrival is off along the line" : "");
    }
    if (S->rchecked)
        printf("capsule survey: the median range residual is %+.2f mm: an error every speaker shares (the latency), which\n"
               "                flags nobody\n", S->rmed_mm);
    if (S->lat_given && fabs(S->rmed_mm) > CSURVEY_RANGE_FLAG_MM)
        printf("capsule survey: WARNING every speaker's range, --latency taken off, reads about %.1f mm %s than its layout\n"
               "                distance: --latency is about that much (%.0f us) too %s, and the acoustic center carries part of it\n",
               fabs(S->rmed_mm), S->rmed_mm > 0 ? "shorter" : "longer", fabs(S->rmed_mm) / C * 1e3,
               S->rmed_mm > 0 ? "large" : "small");
    /* 3. the table again, at the acoustic center */
    for (int k = 0; k < n; ++k) for (int a = 0; a < 3; ++a) src[k][a] = spos[k][a] - S->cen[a];
    if (!zylia_survey(src, (const double (*)[ZYLIA_MICS])arr, n, C, S->caps, &S->resid, &S->radius, &S->spread)) {
        fprintf(stderr, "calibrate: --capsule-survey: the solve at the acoustic center failed; nothing written\n");
        return 0;
    }
    return 1;
}

/* Leave-one-out over the solve S's speakers, at the center S solved the table at. One line a speaker.
 * Returns the flag count, or -1 when there are too few speakers to leave one out. score[k] (k indexes
 * use) is how far a flagged speaker is past the floor (held out / ZYLIA_LOO_FLOOR_US), 0 for the rest:
 * the range check's flags are scored the same way, so the worst of either kind is the one dropped. */
static int csurvey_loo(const CSurveyArgs& A, const CSolve& S, const float (*spos_all)[3],
                       const double (*arr_all)[ZYLIA_MICS], const int* use, int n, double* score) {
    static float  src[ZYLIA_SURVEY_MAX][3];
    static double arr[ZYLIA_SURVEY_MAX][ZYLIA_MICS];
    static ZyliaLooObs lo[ZYLIA_SURVEY_MAX];
    for (int k = 0; k < n; ++k) {
        for (int a = 0; a < 3; ++a) src[k][a] = spos_all[use[k]][a] - S.cen[a];
        memcpy(arr[k], arr_all[use[k]], sizeof arr[k]);
    }
    for (int k = 0; k < n; ++k) score[k] = 0.0;
    const int nf = zylia_survey_loo(src, (const double (*)[ZYLIA_MICS])arr, n, A.sos, lo);
    if (nf < 0) {
        printf("capsule survey: leave-one-out needs 5 speakers or more and has %d: no speaker is checked against the rest\n", n);
        return -1;
    }
    printf("capsule survey: leave-one-out, each speaker against the table the other %d solve to (flagged over %.0f us\n"
           "                and %.0f x the rest's own residual):\n", n - 1, ZYLIA_LOO_FLOOR_US, ZYLIA_LOO_RATIO);
    for (int k = 0; k < n; ++k) {
        const ZyliaLooObs& o = lo[k];
        if (!o.ok) {
            if (o.spread < ZYLIA_SURVEY_MIN_SPREAD)
                printf("  spk %2d: unchecked: without it the rest have a spread of %.3f, under the %.2f floor\n",
                       A.spk[use[k]], o.spread < 0.f ? 0.f : o.spread, ZYLIA_SURVEY_MIN_SPREAD);
            else
                printf("  spk %2d: unchecked: the rest do not solve without it\n", A.spk[use[k]]);
            continue;
        }
        printf("  spk %2d: held out %6.2f us, the rest %.2f us%s\n", A.spk[use[k]], o.heldout_us, o.resid_us,
               o.flagged ? "   FLAGGED: it does not fit the rest" : "");
        if (o.flagged) score[k] = o.heldout_us / ZYLIA_LOO_FLOOR_US;   /* how far past its floor */
    }
    if (nf > 0) {
        char list[256] = "";
        size_t m = 0;
        for (int k = 0; k < n; ++k)
            if (lo[k].ok && lo[k].flagged && m < sizeof list)
                m += (size_t)snprintf(list + m, sizeof list - m, "%s%d (%.2f us)", m ? ", " : "", A.spk[use[k]], lo[k].heldout_us);
        printf("capsule survey: leave-one-out flagged speaker(s) %s\n", list);
    }
    return nf;
}

/* --drop-outliers drops ONE speaker, the worst flagged, and solves again. A speaker flagged in that second
 * check stops the run rather than getting a round of its own. Leave-one-out's premise is one bad speaker
 * against a consistent rest; a second one says more than one position is off, so the rest is no longer
 * known to be consistent, and each further automatic drop picks among speakers the tool cannot tell from a
 * wrong rest. The positions all come from one layout, so two bad ones point at the layout (a stale or
 * partial survey) more than at two speakers, and --speakers is the explicit way to exclude them. Only the
 * worst is dropped, never every flagged one: a bad speaker bends the table the others are scored against,
 * so a good one can flag beside it and clear once it is gone (zylia.h: drop the worst, look again). */
#define CSURVEY_MAX_DROPS 1

static void print_caps(const char* tag, const float c[ZYLIA_MICS][3]) {
    for (int i = 0; i < ZYLIA_MICS; ++i) printf("%s %2d (%+.6f %+.6f %+.6f)\n", tag, i, c[i][0], c[i][1], c[i][2]);
}

static int capsule_survey(const CSurveyArgs& A) {
    const Layout& L = *A.L;
    const double C = A.sos;
    const int K = A.nspk;
    static float spos[ZYLIA_SURVEY_MAX][3];
    static double arr[ZYLIA_SURVEY_MAX][ZYLIA_MICS];
    if (K < CSURVEY_MIN_SPK) {
        fprintf(stderr, "calibrate: --capsule-survey: %d speaker(s); the survey needs %d or more, and %d or more for\n"
                        "           the acoustic center\n", K, CSURVEY_MIN_SPK, CSURVEY_CENTER_MIN_SPK);
        return 2;
    }
    for (int k = 0; k < K; ++k) memcpy(spos[k], L.speakers[A.spk[k]].pos, sizeof spos[k]);
    int unsurveyed = 0;
    for (int k = 0; k < K; ++k) unsurveyed += !L.speakers[A.spk[k]].has_plan;
    printf("capsule survey: %d speaker(s), their as-built positions (`position`) taken as the known sources%s\n", K,
           A.simulate ? "  [SIMULATE]" : "");
    if (unsurveyed)
        printf("capsule survey: %d of them carry no plan_position, so no survey ever wrote their position: it is the\n"
               "                plan, or a typed value. The survey is wrong in proportion to the positions (1 cm at\n"
               "                2 m is 0.3 deg of the array's orientation). Run --localize --zylia first.\n", unsurveyed);
    else
        printf("capsule survey: every one carries a plan, so a survey wrote its position (--localize, or the --zylia\n"
               "                position survey)\n");
    if (K < CSURVEY_REC_SPK)
        printf("capsule survey: WARNING %d speakers: the survey asks for %d or more, spread high and low\n", K, CSURVEY_REC_SPK);

    float center[3] = { A.mic[0], A.mic[1], A.mic[2] };
    if (const int rc = csurvey_check(A, A.spk, K, center, g_tracked ? "the target" : "--mic", 2)) return rc;
    if (g_tracked) {
        /* a direction mode: the table is read in room axes at the take's orientation */
        float far_m = 0.f;
        for (int k = 0; k < K; ++k) {
            const float dx = spos[k][0] - center[0], dy = spos[k][1] - center[1], dz = spos[k][2] - center[2];
            const float d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d > far_m) far_m = d;
        }
        if (track_place(A.mic, NULL, "capsule survey", center, place_turn_limit_deg(g_tol_m, far_m))) return 1;
        if (const int rc = csurvey_check(A, A.spk, K, center, "the measured center", 1)) return rc;
    }
    if (A.simulate) {   /* the truth, from the simulator's own code; nothing below reads it */
        float q[4], tr[ZYLIA_MICS][3];
        const int hq = g_track_sim && mic_track_sim_orientation(&g_mt, q);
        if (hq)
            printf("track-sim: true orientation at the take, q = (%+.6f %+.6f %+.6f %+.6f) (xyzw); true mount offset\n"
                   "           (%.4f %.4f %.4f) m, body axes\n", q[0], q[1], q[2], q[3],
                   g_mt.sim_true_off[0], g_mt.sim_true_off[1], g_mt.sim_true_off[2]);
        calib_sim_zm1_room(hq ? q : NULL, tr);
        if (!g_track_sim) {
            const float* tc = A.have_sim_center ? A.sim_center : center;
            printf("sim-truth: the simulated ZM-1's center (%.4f %.4f %.4f)%s\n", tc[0], tc[1], tc[2],
                   A.have_sim_center ? " (--sim-center; --mic is the taped guess)" : " (--mic)");
        }
        printf("sim-truth: the simulated ZM-1's capsules about its center, room axes, at the take (radius %.1f mm,\n"
               "           turned %.0f deg in its mount)\n", CALIB_SIM_ZM1_RADIUS_M * 1e3f, CALIB_SIM_ZM1_MOUNT_YAW_DEG);
        print_caps("sim-truth: cap", tr);
    }

    static Pass P;                                             /* static: it carries the capsule table */
    P.c.L = &L; P.c.sos = C; P.c.simulate = A.simulate; P.c.zylia = 1;
    /* where the simulated array really is: the stand's TRUE center, or --sim-center, else --mic */
    P.c.sim_at = g_track_sim ? g_sim_at : (A.have_sim_center ? A.sim_center : center);
    P.c.sweep = A.sweep; P.c.cap = A.cap; P.c.cap19 = A.cap19;
    P.c.band_hz = NULL;
    P.in_first = A.in_first;
    /* the window from the taped (or tracked) center, with the PLAN margin: the center is a guess here */
    pass_quality(P.c, center, CALIB_WIN_PLAN_M, 0);
    int nfall = 0;
    for (int k = 0; k < K; ++k) {
        if (A.simulate) {   /* the physical array as it is turned right now, never the table a solve reads */
            float q[4];
            calib_sim_zm1_room(g_track_sim && mic_track_sim_orientation(&g_mt, q) ? q : NULL, P.c.caps);
        }
        MeasureResult r;
        CalibMeasInfo mi;
        if (!measure_speaker(P, A.spk[k], 0, &r, &mi)) return 1;
        if (const int bump = track_after_capture(CAPTURE_S * mi.sweeps, "capsule survey", A.spk[k])) return bump;
        nfall += !mi.refined;
        memcpy(arr[k], mi.arrival_s, sizeof arr[k]);
    }
    sweep_report("capsule survey");
    if (nfall)
        printf("capsule survey: WARNING the capsule cross-correlation fell back to each capsule's own peak for %d\n"
               "                speaker(s); those arrivals carry up to a degree of timing bias\n", nfall);

    /* the three solves, leave-one-out on the result, and with --drop-outliers one drop and a second pass */
    static int use[ZYLIA_SURVEY_MAX];
    int n = K;
    for (int k = 0; k < K; ++k) use[k] = k;
    static int dropped[ZYLIA_SURVEY_MAX];
    int ndropped = 0;
    static CSolve S;
    for (;;) {
        if (!csurvey_solve(A, center, (const float (*)[3])spos, (const double (*)[ZYLIA_MICS])arr, use, n, &S)) return 1;
        /* the two checks: leave-one-out (a direction error) and the range check (a distance error, in
         * csurvey_solve). A speaker either one flags counts; the worst is the one furthest past its own
         * threshold, so a speaker both flag is scored by the worse of the two. */
        static double lscore[ZYLIA_SURVEY_MAX];
        const int nl = csurvey_loo(A, S, (const float (*)[3])spos, (const double (*)[ZYLIA_MICS])arr, use, n, lscore);
        int nf = 0, worst = -1;
        double wscore = 0.0;
        char why[256] = "";
        size_t wm = 0;
        for (int k = 0; k < n; ++k) {
            const int lf = nl > 0 && lscore[k] > 0.0, rf = S.rflag[k];
            if (!lf && !rf) continue;
            ++nf;
            double sc = lf ? lscore[k] : 0.0;
            if (rf && fabs(S.rdev_mm[k]) / CSURVEY_RANGE_FLAG_MM > sc) sc = fabs(S.rdev_mm[k]) / CSURVEY_RANGE_FLAG_MM;
            if (worst < 0 || sc > wscore) { worst = k; wscore = sc; }
            if (wm < sizeof why)
                wm += (size_t)snprintf(why + wm, sizeof why - wm, "%s%d (%s)", wm ? ", " : "", A.spk[use[k]],
                                       lf && rf ? "leave-one-out and range" : (lf ? "leave-one-out" : "range"));
        }
        if (S.nrflag) {
            char rl[256] = "";
            size_t m = 0;
            for (int k = 0; k < n; ++k)
                if (S.rflag[k] && m < sizeof rl)
                    m += (size_t)snprintf(rl + m, sizeof rl - m, "%s%d (%+.1f mm)", m ? ", " : "", A.spk[use[k]], S.rdev_mm[k]);
            printf("capsule survey: the range check flagged speaker(s) %s\n", rl);
        }
        if (nf == 0) break;
        static int kept[ZYLIA_SURVEY_MAX], flg[ZYLIA_SURVEY_MAX];
        char klist[512], dlist[160];
        const int bad = A.spk[use[worst]];
        int nk = 0;
        for (int k = 0; k < n; ++k) if (k != worst) kept[nk++] = A.spk[use[k]];
        if (!A.drop_outliers) {
            fmt_speakers(kept, nk, klist, sizeof klist);
            fprintf(stderr, "calibrate: --capsule-survey: %d speaker(s) flagged, %s; the worst speaker %d: it does not\n"
                            "           fit the rest (leave-one-out: its direction; range: its distance), so its position or\n"
                            "           its capture is off. Nothing written. Check speaker %d's position (re-run --localize\n"
                            "           --zylia) and its Dante latency, or re-run with --speakers %s, or pass --drop-outliers\n"
                            "           to drop it and solve again.\n",
                    nf, why, bad, bad, klist);
            return CSURVEY_EXIT_OUTLIER;
        }
        if (ndropped >= CSURVEY_MAX_DROPS) {
            for (int k = 0; k < ndropped; ++k) flg[k] = A.spk[dropped[k]];
            fmt_speakers(flg, ndropped, dlist, sizeof dlist);
            fmt_speakers(kept, nk, klist, sizeof klist);
            fprintf(stderr, "calibrate: --capsule-survey: --drop-outliers dropped speaker %s, and the checks now flag\n"
                            "           speaker %s too.\n"
                            "           A second outlier means more than one position is off, so the rest is no longer known to\n"
                            "           be consistent and the run stops: nothing written. Check the layout's positions (re-run\n"
                            "           --localize --zylia), or choose the set yourself with --speakers, for example\n"
                            "           --speakers %s\n", dlist, why, klist);
            return CSURVEY_EXIT_OUTLIER;
        }
        /* the rest must still be a survey: the solver's floor, and a spread from the center the solve starts at */
        if (nk < CSURVEY_MIN_SPK) {
            fprintf(stderr, "calibrate: --capsule-survey: --drop-outliers: dropping speaker %d leaves %d speakers, under the\n"
                            "           survey's %d: nothing written\n", bad, nk, CSURVEY_MIN_SPK);
            return CSURVEY_EXIT_OUTLIER;
        }
        if (csurvey_check(A, kept, nk, center, g_tracked ? "the measured center" : "--mic", 1)) {
            fprintf(stderr, "calibrate: --capsule-survey: --drop-outliers: that is without speaker %d, so it cannot be\n"
                            "           dropped: nothing written\n", bad);
            return CSURVEY_EXIT_OUTLIER;
        }
        printf("capsule survey: --drop-outliers: dropped speaker %d; solving again over the other %d, the acoustic center\n"
               "                included\n", bad, nk);
        dropped[ndropped++] = use[worst];
        for (int k = worst; k + 1 < n; ++k) use[k] = use[k + 1];
        --n;
    }
    if (ndropped) {
        static int dl[ZYLIA_SURVEY_MAX];
        char dlist[160];
        for (int k = 0; k < ndropped; ++k) dl[k] = A.spk[dropped[k]];
        fmt_speakers(dl, ndropped, dlist, sizeof dlist);
        printf("capsule survey: the survey leaves out speaker %s (--drop-outliers); %d speakers remain\n", dlist, n);
        if (n < CSURVEY_REC_SPK)
            printf("capsule survey: WARNING %d speakers: the survey asks for %d or more, spread high and low\n", n, CSURVEY_REC_SPK);
    }
    const float (&caps)[ZYLIA_MICS][3] = S.caps;
    const float resid = S.resid, radius = S.radius, spread = S.spread;
    const float* cen = S.cen;
    const int have_ac = S.have_ac;
    const double lat_m = S.lat_m, dil = S.dil;
    printf("capsule survey: residual %.2f us, radius %.2f mm, spread %.2f (the floor is %.2f), %d speakers\n",
           resid, radius * 1e3f, spread, ZYLIA_SURVEY_MIN_SPREAD, n);
    if (resid > CSURVEY_RESID_WARN_US)
        printf("capsule survey: WARNING a residual over %.0f us: positions that are off, a center far from the real one,\n"
               "                or arrivals the free-field model does not explain (the rigid sphere, reflections)\n",
               CSURVEY_RESID_WARN_US);
    if (!(radius > 0.040f && radius < 0.060f))
        printf("capsule survey: WARNING a radius of %.1f mm, not about 49: something upstream is badly wrong\n", radius * 1e3f);
    const float ex = cen[0] - center[0], ey = cen[1] - center[1], ez = cen[2] - center[2];
    const float dc = sqrtf(ex * ex + ey * ey + ez * ez);
    if (have_ac) {
        printf("capsule survey: acoustic center (%.4f %.4f %.4f), %+.1f %+.1f %+.1f mm (|d| %.1f mm) from %s;\n"
               "                fit RMS %.2f mm over %d speakers\n", cen[0], cen[1], cen[2], ex * 1e3f, ey * 1e3f, ez * 1e3f,
               dc * 1e3f, g_tracked ? "the measured center" : "--mic", S.fit_rms * 1e3, n);
        if (S.lat_given)
            printf("capsule survey: system latency from --latency, %.3f ms (%.3f m at c): --latency %.3f for live aiming and the --zylia survey\n",
                   lat_m / C * 1e3, lat_m, lat_m);
        else if (S.lat_trusted)
            printf("capsule survey: system latency %.3f ms (%.3f m at c), dilution %.1f: --latency %.3f for live aiming and the --zylia survey\n",
                   lat_m / C * 1e3, lat_m, dil, lat_m);
        else
            printf("capsule survey: WARNING the system latency (%.3f m) is poorly determined from these speakers (dilution %.1f,\n"
                   "                over 3): they sit at about one distance on one side. The center above stands, and so does the\n"
                   "                range check (each speaker against the median, which takes the shared error out), but the\n"
                   "                latency does not: run --localize first and pass its latency as --latency\n",
                   lat_m, dil);
    } else
        printf("capsule survey: no acoustic center (%s): the table stays at %s\n",
               S.lat_given ? (n < 4 ? "it needs 4 speakers or more" : "the solve is degenerate")
                           : (n < CSURVEY_CENTER_MIN_SPK ? "it needs 5 speakers or more" : "the trilateration is degenerate"),
               g_tracked ? "the measured center" : "--mic");

    int rc = 0;
    char err[256] = { 0 };
    if (g_tracked) {
        /* BODY frame: caps_body = R^T caps_room at the take's orientation; offset = R^T (center - p),
         * with p = the measured center - R offset_in_use */
        float R[9], body[ZYLIA_MICS][3];
        zylia_quat_to_matrix(g_placed.q, R);
        zylia_capsules_rotate(caps, R, 1, body);
        const float* ou = g_mt.offset;
        float oa[3];
        for (int a = 0; a < 3; ++a) oa[a] = ou[a] + R[0 * 3 + a] * ex + R[1 * 3 + a] * ey + R[2 * 3 + a] * ez;
        const float dx = oa[0] - ou[0], dy = oa[1] - ou[1], dz = oa[2] - ou[2];
        const float dof = sqrtf(dx * dx + dy * dy + dz * dz);
        if (have_ac) {
            printf("capsule survey: mount offset, body axes: in use (%.4f %.4f %.4f) m, acoustic (%.4f %.4f %.4f) m:\n"
                   "                %.1f mm apart\n", ou[0], ou[1], ou[2], oa[0], oa[1], oa[2], dof * 1e3f);
            if (dof * 1e3f > CSURVEY_OFFSET_WARN_MM)
                printf("capsule survey: WARNING the acoustic center and the tracked one disagree by %.1f mm (over %.0f): either\n"
                       "                the mount offset in use is wrong, or the speaker positions are. The survey keeps the\n"
                       "                ACOUSTIC offset, which matches the positions every direction is compared against.\n",
                       dof * 1e3f, CSURVEY_OFFSET_WARN_MM);
        }
        ZyliaMount m;
        memset(&m, 0, sizeof m);
        m.body_frame = 1; m.have_offset = 1;
        memcpy(m.offset_m, have_ac ? oa : ou, sizeof m.offset_m);
        if (!zylia_survey_save(A.out_path, body, resid, radius, spread, n, &m, err, sizeof err)) {
            fprintf(stderr, "calibrate: --capsule-survey: %s\n", err); return 1; }
        printf("capsule survey: wrote a BODY-FRAME survey to %s (the %s mount offset); it follows the stand from here on\n",
               A.out_path, have_ac ? "acoustic" : "tracked");
    } else {
        if (!zylia_survey_save(A.out_path, caps, resid, radius, spread, n, NULL, err, sizeof err)) {
            fprintf(stderr, "calibrate: --capsule-survey: %s\n", err); return 1; }
        printf("capsule survey: wrote a ROOM-AXES survey to %s: it holds for this mounting only. Give the runs that use\n"
               "                it the array center as --mic: (%.4f %.4f %.4f)\n", A.out_path, cen[0], cen[1], cen[2]);
    }
    /* the file as the direction modes will read it: through the loader, re-aimed the way mic_track
     * re-aims a body-frame table at a placement (here the take's orientation) */
    ZyliaMount lm;
    if (!zylia_survey_load(A.out_path, &lm, err, sizeof err)) { fprintf(stderr, "calibrate: reload: %s\n", err); return 1; }
    float rt[ZYLIA_MICS][3];
    zylia_capsules(rt);
    if (lm.body_frame) {
        float R[9];
        zylia_quat_to_matrix(g_placed.q, R);
        zylia_capsules_rotate(rt, R, 0, rt);
    }
    printf("capsule survey: the saved file, reloaded%s, room axes:\n", lm.body_frame ? " and re-aimed at the take's orientation" : "");
    print_caps("reload: cap", rt);
    if (lm.body_frame && lm.have_offset) {
        float R[9], c2[3];
        zylia_quat_to_matrix(g_placed.q, R);
        for (int a = 0; a < 3; ++a) {   /* p + R offset_saved, p = the measured center - R offset_in_use */
            c2[a] = center[a];
            for (int b = 0; b < 3; ++b) c2[a] += R[a * 3 + b] * (lm.offset_m[b] - g_mt.offset[b]);
        }
        printf("reload: center from the saved offset at the take: (%.4f %.4f %.4f)\n", c2[0], c2[1], c2[2]);
    }
    track_report("the capsule survey measured");
    return rc;
}

/* "x,y,z" with every component finite and within +-lim; 1 = parsed */
static int parse_xyz(const char* s, float lim, float out[3]) {
    const char* q = s;
    for (int a = 0; a < 3; ++a) {
        char* end = NULL;
        const double v = strtod(q, &end);
        if (end == q || !(v >= -lim && v <= lim)) return 0;
        out[a] = (float)v;
        q = end;
        if (a < 2) { if (*q != ',') return 0; ++q; }
    }
    return *q == 0;
}

/* --speakers "0,3,5-9": distinct indices below n, in the order given; returns the count, -1 on a
 * malformed list (why in err) */
static int parse_speakers(const char* s, int n, int* out, int cap, char* err, size_t errcap) {
    unsigned char seen[BWA_MAX_CHANNELS] = { 0 };
    int k = 0;
    const char* q = s;
    while (*q) {
        char* end = NULL;
        const long a = strtol(q, &end, 10);
        if (end == q) { snprintf(err, errcap, "'%s' is not a list like 0,3,5-9", s); return -1; }
        long b = a;
        q = end;
        if (*q == '-') {
            ++q;
            b = strtol(q, &end, 10);
            if (end == q) { snprintf(err, errcap, "'%s' is not a list like 0,3,5-9", s); return -1; }
            q = end;
        }
        if (a < 0 || b < a || b >= n) { snprintf(err, errcap, "'%s': speakers run 0..%d", s, n - 1); return -1; }
        for (long v = a; v <= b; ++v) {
            if (seen[v]) { snprintf(err, errcap, "'%s' names speaker %ld twice", s, v); return -1; }
            if (k >= cap) { snprintf(err, errcap, "'%s': more than %d speakers", s, cap); return -1; }
            seen[v] = 1; out[k++] = (int)v;
        }
        if (*q == ',') ++q;
        else if (*q) { snprintf(err, errcap, "'%s': unexpected '%c'", s, *q); return -1; }
    }
    return k;
}

/* read mic positions ("x y z" per line) for --localize; returns the count (<= maxK) */
static int read_positions(const char* path, float (*out)[3], int maxK) {
    FILE* f = fopen(path, "r"); if (!f) return 0;
    int k = 0;
    while (k < maxK && fscanf(f, "%f %f %f", &out[k][0], &out[k][1], &out[k][2]) == 3) ++k;
    fclose(f); return k;
}

int main(int argc, char** argv) {
    /* A pipe makes stdout fully buffered, so a caller streaming this tool's output (calib_view's Session
     * tab) would see nothing until 4 KB piled up or the run ended: the live placement line included. */
    setvbuf(stdout, NULL, _IONBF, 0);
    const char* layout_path = "examples/cave_layout.json";
    const char* out_path    = NULL;
    const char* driver      = NULL;                       /* --driver; NULL = auto-pick */
    float mic[3] = { 0.f, 0.f, 0.f };
    int   mic_set = 0;                                    /* --mic given: the directivity re-aim needs a real bearing */
    int   mic_in = 0, simulate = 0, room = 0, check = 0, live_speaker = -1, eq = 0, room_eq = 0, rq_grid = 0, zylia = 0;
    int   no_dir = 0;                                     /* --ignore-directivity: trims as measured, no model re-aim */
    int   check_aim = 0;                                  /* --check-aim: fit each speaker's aim from the --localize tilts */
    int   trims = 0, verify = 0;                          /* --trims (the default mode, spelled out; with --zylia it
                                                           * selects trims over the position survey) / --verify */
    const char* aim_csv = NULL;                           /* --aim-sheet: no audio, the installer's sheet */
    double sim_aim_err = 0.0;                             /* --sim-aim-error: simulate only, a known aim error to recover */
    double sim_room = 0.0;                                /* --sim-room [absorption]: simulate only, a room around the array */
    int    sweeps = 0;                                    /* --sweeps: a bounded --live (tests, scripts); 0 = until a key */
    int    aim_ref_spk = -1;                              /* --aim-ref: the speaker known to point at the mic */
    int    have_aim_ref_db = 0; float aim_ref_db = 0.f;   /* --aim-ref-db: a stored on-axis reference tilt */
    float  sim_move[3] = { 0.f, 0.f, 0.f };               /* --sim-move: the live speaker's true offset */
    int    sim_move_set = 0;
    float  sim_steps[64]; int nsteps = 0;                 /* --sim-aim-steps: its true aim error, per reading */
    float  sim_screen = 0.f;                              /* --sim-screen: a screen's HF loss on every speaker's path */
    double known_latency = -1.0;
    int    ref_spk = -1; double ref_dist = 0.0;               /* --ref: one tape-measured distance -> latency */
    const char* ref_spk_list = NULL;                          /* --ref-speakers 2,6,18: measured positions -> latency */
    int    sim_lat_spk[8]; double sim_lat_s[8]; int nsim_lat = 0;   /* --sim-speaker-latency spk,ms (repeatable) */
    const char* survey_path = NULL;                           /* --survey: pinned ZM-1 channel order + orientation */
    const char* ir_prefix = NULL;
    const char* localize_file = NULL;
    const char* rq_rows_file = NULL;                          /* --room-eq-grid rows.txt: one grid position per row */
    const char* track_body = NULL;                           /* --track: the ZM-1 stand's rigid body (id or name) */
    const char* nn_server = NULL;                             /* --natnet-server: Motive's host */
    const char* nn_multicast = "239.255.42.99";               /* --natnet-multicast */
    int    track_sim = 0, track_sim_bump = 0;                 /* --track-sim, --track-sim-bump N */
    int    track_sim_twist = 0;                               /* --track-sim-twist N */
    int    mount_ring = 0, mount_off_set = 0;                 /* --mount-offset ring | x,y,z */
    float  mount_off[3] = { 0.f, 0.f, 0.f };
    const char* csurvey_out = NULL;                           /* --capsule-survey out.json: the table from the speakers */
    const char* sim_truth_path = NULL;                        /* --sim-truth f.json: where the simulated speakers stand */
    const char* spk_list = NULL;                              /* --speakers 0,3,5-9: its source set */
    int    drop_outliers = 0;                                 /* --drop-outliers: drop what leave-one-out flags */
    int    track_sim_off_set = 0;                             /* --track-sim-offset x,y,z: the stand's TRUE offset */
    float  track_sim_off[3] = { 0.f, 0.f, 0.f };
    int    sim_center_set = 0;                                /* --sim-center x,y,z: the untracked ZM-1's TRUE center */
    int    intf_caps[CALIB_SIM_INTF_MAX]; int intf_n = 0;     /* --sim-interferer: the captures it lands on */
    CalibSimInterferer intf;                                  /* ... and what it is */
    memset(&intf, 0, sizeof intf);
    float  sim_center[3] = { 0.f, 0.f, 0.f };
    int    tol_set = 0, timeout_set = 0;
    /* Room-temperature speed of sound. Every acoustic RANGE below is c * delay, so a 2% error in c
     * is a 2% systematic in every surveyed position (8 cm at 4 m) — the dominant error term in the
     * survey, well above the 7 mm timing resolution. Precedence: --temp/--c, else the layout's
     * reference.speed_of_sound_mps, else the 20 C reference. See sos.h. */
    double sos = BWA_SOS_REF_MPS;
    int    sos_set = 0;                    /* an explicit flag beats the file */
    int    n_temp = 0, n_c = 0;            /* --temp and --c are alternatives, not a pair */
    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i],"--layout") && i+1<argc) layout_path = argv[++i];
        else if (!strcmp(argv[i],"--out")    && i+1<argc) out_path    = argv[++i];
        else if (!strcmp(argv[i],"--driver") && i+1<argc) driver      = argv[++i];
        else if (!strcmp(argv[i],"--list-drivers"))       return calib_asio_list();   /* names for --driver */
        else if (!strcmp(argv[i],"--input")  && i+1<argc) mic_in      = atoi(argv[++i]);
        else if (!strcmp(argv[i],"--simulate"))           simulate    = 1;
        else if (!strcmp(argv[i],"--room"))               room        = 1;   /* RT60 + early reflections report */
        else if (!strcmp(argv[i],"--save-irs") && i+1<argc) ir_prefix = argv[++i];  /* dump per-speaker IR WAVs */
        else if (!strcmp(argv[i],"--localize") && i+1<argc) localize_file = argv[++i]; /* mic-positions file -> solve speaker positions */
        else if (!strcmp(argv[i],"--check"))              check       = 1;   /* flag speakers nudged from the stored layout */
        else if (!strcmp(argv[i],"--live") && i+1<argc)   live_speaker= atoi(argv[++i]); /* live distance readout for one speaker */
        else if (!strcmp(argv[i],"--latency") && i+1<argc) known_latency = atof(argv[++i]); /* c*tau meters: --live absolute distance, --zylia distances */
        else if (!strcmp(argv[i],"--ref") && i+2<argc)    { ref_spk = atoi(argv[++i]); ref_dist = atof(argv[++i]); } /* --zylia: tape-measured center->speaker distance -> solves the latency */
        else if (!strcmp(argv[i],"--ref-speakers") && i+1<argc) ref_spk_list = argv[++i];   /* --zylia: measured positions -> the latency, their median */
        else if (!strcmp(argv[i],"--sim-speaker-latency") && i+1<argc) {   /* "3,0.5": speaker 3 plays 0.5 ms late */
            const char* q = argv[++i];
            char* end = NULL;
            const long sp = strtol(q, &end, 10);
            double ms = 0.0;
            int ok = end != q && *end == ',' && sp >= 0 && sp < BWA_MAX_CHANNELS;
            if (ok) { q = end + 1; ms = strtod(q, &end); ok = end != q && *end == 0 && ms >= -20.0 && ms <= 20.0 && ms != 0.0; }
            if (!ok) {
                fprintf(stderr, "calibrate: --sim-speaker-latency wants spk,ms with ms in [-20, 20] and not 0 (got %s)\n", argv[i]); return 2; }
            if (nsim_lat >= 8) { fprintf(stderr, "calibrate: --sim-speaker-latency: at most 8\n"); return 2; }
            for (int k = 0; k < nsim_lat; ++k)
                if (sim_lat_spk[k] == (int)sp) { fprintf(stderr, "calibrate: --sim-speaker-latency names speaker %ld twice\n", sp); return 2; }
            sim_lat_spk[nsim_lat] = (int)sp; sim_lat_s[nsim_lat] = ms * 1e-3; ++nsim_lat;
        }
        else if (!strcmp(argv[i],"--survey") && i+1<argc)  survey_path = argv[++i]; /* --zylia: capsule self-survey (room axes) */
        else if (!strcmp(argv[i],"--eq"))                 eq          = 1;   /* per-speaker direct-sound correction FIR */
        else if (!strcmp(argv[i],"--room-eq"))            eq = room_eq = 1;  /* + room correction AT THE MIC POINT (static listener only) */
        else if (!strcmp(argv[i],"--room-eq-grid")) {     /* accumulate LF modal cuts into room_eq_grid (tracked room EQ) */
            rq_grid = 1;                                  /* bare: THIS mic position (--mic); with a file: its rows */
            if (i+1 < argc && argv[i+1][0] != '-') rq_rows_file = argv[++i];
        }
        else if (!strcmp(argv[i],"--zylia"))              zylia       = 1;   /* the mic is the ZM-1: bare = the position survey */
        else if (!strcmp(argv[i],"--trims"))              trims       = 1;   /* the trim mode (default); --zylia --trims = trims from the ZM-1 */
        else if (!strcmp(argv[i],"--verify"))             verify      = 1;   /* second pass: sweep THROUGH the trims, report what is left */
        else if (!strcmp(argv[i],"--aim-sheet") && i+1<argc) aim_csv = argv[++i];   /* installer's aiming sheet (CSV); no audio */
        else if (!strcmp(argv[i],"--ignore-directivity")) no_dir      = 1;   /* trims: skip the layout's directivity re-aim */
        else if (!strcmp(argv[i],"--check-aim"))          check_aim   = 1;   /* with --localize: fit each speaker's aim from its off-axis tilt */
        else if (!strcmp(argv[i],"--sim-aim-error") && i+1<argc) sim_aim_err = atof(argv[++i]);   /* --simulate: rotate the true aims by this */
        else if (!strcmp(argv[i],"--sweeps") && i+1<argc) {
            sweeps = atoi(argv[++i]);
            if (sweeps < 1) { fprintf(stderr, "calibrate: --sweeps wants a count >= 1 (got %s)\n", argv[i]); return 2; }
        }
        else if (!strcmp(argv[i],"--aim-ref") && i+1<argc)    aim_ref_spk = atoi(argv[++i]);
        else if (!strcmp(argv[i],"--aim-ref-db") && i+1<argc) {
            char* end = NULL; double v = strtod(argv[++i], &end);
            if (end == argv[i] || *end || !(v > -40.0 && v < 40.0)) {
                fprintf(stderr, "calibrate: --aim-ref-db wants a tilt in dB (got %s)\n", argv[i]); return 2; }
            aim_ref_db = (float)v; have_aim_ref_db = 1;
        }
        else if (!strcmp(argv[i],"--sim-move") && i+3<argc) {
            for (int a = 0; a < 3; ++a) sim_move[a] = (float)atof(argv[++i]);
            sim_move_set = 1;
        }
        else if (!strcmp(argv[i],"--sim-screen") && i+1<argc) {
            sim_screen = (float)atof(argv[++i]);
            if (!(sim_screen > 0.f && sim_screen < 40.f)) { fprintf(stderr, "calibrate: --sim-screen wants a loss in dB in (0, 40) (got %s)\n", argv[i]); return 2; }
        }
        else if (!strcmp(argv[i],"--sim-aim-steps") && i+1<argc) {   /* "25,12,0,12,25" */
            const char* q = argv[++i];
            while (*q && nsteps < 64) {
                char* end = NULL; double v = strtod(q, &end);
                if (end == q || !(v >= 0.0 && v <= 180.0)) {
                    fprintf(stderr, "calibrate: --sim-aim-steps wants degrees in [0, 180], comma-separated (got %s)\n", argv[i]); return 2; }
                sim_steps[nsteps++] = (float)v;
                q = end;
                if (*q == ',') ++q; else if (*q) { fprintf(stderr, "calibrate: --sim-aim-steps: unexpected '%c'\n", *q); return 2; }
            }
        }
        else if (!strcmp(argv[i],"--sim-room")) {         /* --simulate: a shoebox room; optional absorption */
            sim_room = 0.3;                               /* a moderately damped room: RT60 ~0.35 s on the 26-grid */
            if (i+1 < argc) {
                char* end = NULL;
                double v = strtod(argv[i+1], &end);
                if (end != argv[i+1] && *end == 0) {
                    if (!(v > 0.0 && v <= 1.0)) { fprintf(stderr, "calibrate: --sim-room absorption must be in (0, 1] (got %s)\n", argv[i+1]); return 2; }
                    sim_room = v; ++i;
                }
            }
        }
        else if (!strcmp(argv[i],"--track") && i+1<argc)           track_body = argv[++i];
        else if (!strcmp(argv[i],"--natnet-server") && i+1<argc)   nn_server = argv[++i];
        else if (!strcmp(argv[i],"--natnet-multicast") && i+1<argc) nn_multicast = argv[++i];
        else if (!strcmp(argv[i],"--track-sim"))                   track_sim = 1;
        else if (!strcmp(argv[i],"--track-sim-bump") && i+1<argc) {
            track_sim_bump = atoi(argv[++i]);
            if (track_sim_bump < 1) { fprintf(stderr, "calibrate: --track-sim-bump wants a capture count >= 1 (got %s)\n", argv[i]); return 2; }
        }
        else if (!strcmp(argv[i],"--track-sim-twist") && i+1<argc) {
            track_sim_twist = atoi(argv[++i]);
            if (track_sim_twist < 1) { fprintf(stderr, "calibrate: --track-sim-twist wants a capture count >= 1 (got %s)\n", argv[i]); return 2; }
        }
        else if (!strcmp(argv[i],"--place-tol-mm") && i+1<argc) {
            char* end = NULL; double v = strtod(argv[++i], &end);
            if (end == argv[i] || *end || !(v >= 1.0 && v <= 500.0)) {
                fprintf(stderr, "calibrate: --place-tol-mm wants a tolerance in mm, 1 to 500 (got %s)\n", argv[i]); return 2; }
            g_tol_m = (float)(v * 1e-3); tol_set = 1;
        }
        else if (!strcmp(argv[i],"--place-timeout") && i+1<argc) {
            char* end = NULL; double v = strtod(argv[++i], &end);
            if (end == argv[i] || *end || !(v >= 1.0 && v <= 86400.0)) {
                fprintf(stderr, "calibrate: --place-timeout wants seconds, 1 to 86400 (got %s)\n", argv[i]); return 2; }
            g_place_timeout = v; timeout_set = 1;
        }
        else if (!strcmp(argv[i],"--mount-offset") && i+1<argc) {   /* "ring" or "x,y,z" (body axes, m) */
            const char* s = argv[++i];
            if (!strcmp(s, "ring")) mount_ring = 1;
            else {
                char* end = NULL; const char* q = s;
                for (int a = 0; a < 3; ++a) {
                    double v = strtod(q, &end);
                    if (end == q || !(v >= -PLACE_MAX_OFFSET_M && v <= PLACE_MAX_OFFSET_M)) {
                        fprintf(stderr, "calibrate: --mount-offset wants ring or x,y,z in meters, body axes (got %s)\n", s); return 2; }
                    mount_off[a] = (float)v;
                    q = end;
                    if (a < 2) { if (*q != ',') { fprintf(stderr, "calibrate: --mount-offset wants ring or x,y,z (got %s)\n", s); return 2; } ++q; }
                }
                if (*q) { fprintf(stderr, "calibrate: --mount-offset wants ring or x,y,z (got %s)\n", s); return 2; }
            }
            mount_off_set = 1;
        }
        else if (!strcmp(argv[i],"--capsule-survey") && i+1<argc) csurvey_out = argv[++i];
        else if (!strcmp(argv[i],"--speakers") && i+1<argc)       spk_list = argv[++i];
        else if (!strcmp(argv[i],"--drop-outliers"))              drop_outliers = 1;
        else if (!strcmp(argv[i],"--track-sim-offset") && i+1<argc) {   /* "x,y,z", body axes, m */
            if (!parse_xyz(argv[++i], PLACE_MAX_OFFSET_M, track_sim_off)) {
                fprintf(stderr, "calibrate: --track-sim-offset wants x,y,z in meters, body axes (got %s)\n", argv[i]); return 2; }
            track_sim_off_set = 1;
        }
        else if (!strcmp(argv[i],"--sim-truth") && i+1<argc) sim_truth_path = argv[++i];
        else if (!strcmp(argv[i],"--sim-interferer") && i+1<argc) {   /* "3,9:0.9:12:noise" */
            char e[200];
            intf_n = calib_sim_parse_interferer(argv[++i], intf_caps, CALIB_SIM_INTF_MAX, &intf, e, sizeof e);
            if (!intf_n) { fprintf(stderr, "calibrate: --sim-interferer %s\n", e); return 2; }
        }
        else if (!strcmp(argv[i],"--window-m") && i+1<argc) {
            char* end = NULL; double v = strtod(argv[++i], &end);
            if (end == argv[i] || *end || !(v >= 0.0 && v <= 10.0)) {
                fprintf(stderr, "calibrate: --window-m wants the window's position margin in meters, 0 to 10 (got %s)\n", argv[i]); return 2; }
            g_window_m = v;
        }
        else if (!strcmp(argv[i],"--sim-center") && i+1<argc) {   /* "x,y,z", room meters */
            if (!parse_xyz(argv[++i], 100.f, sim_center)) {
                fprintf(stderr, "calibrate: --sim-center wants x,y,z in room meters (got %s)\n", argv[i]); return 2; }
            sim_center_set = 1;
        }
        else if (!strcmp(argv[i],"--mic") && i+3<argc) { mic[0]=(float)atof(argv[++i]); mic[1]=(float)atof(argv[++i]); mic[2]=(float)atof(argv[++i]); mic_set = 1; }
        else if (!strcmp(argv[i],"--temp") && i+1<argc) {   /* "22.8", "22.8C", "73F" */
            if (!sos_parse_temp(argv[++i], &sos)) {
                fprintf(stderr, "calibrate: --temp '%s' is not a plausible room temperature "
                                "(bare number or C suffix = Celsius, F suffix = Fahrenheit)\n", argv[i]); return 2; }
            sos_set = 1; ++n_temp;
        }
        else if (!strcmp(argv[i],"--c") && i+1<argc) {      /* someone who measured c directly */
            if (!sos_parse_mps(argv[++i], &sos)) {
                fprintf(stderr, "calibrate: --c '%s' is not a plausible speed of sound (%.0f..%.0f m/s)\n",
                        argv[i], BWA_SOS_MIN_MPS, BWA_SOS_MAX_MPS); return 2; }
            sos_set = 1; ++n_c;
        }
        else if (!strcmp(argv[i],"--temp") || !strcmp(argv[i],"--c")) {   /* present but no value */
            fprintf(stderr, "calibrate: %s needs a value\n", argv[i]); return 2;
        }
        else { fprintf(stderr, "usage: calibrate [--layout f] [--out f] [--mic x y z] [--input ch] [--driver name] [--list-drivers] [--simulate] [--trims | --verify] [--room] [--eq | --room-eq | --room-eq-grid [rows.txt]] [--zylia] [--survey f] [--ref spk dist_m | --ref-speakers list] [--save-irs prefix] [--localize positions.txt] [--check] [--live N] [--latency m] [--temp T[C|F] | --c mps] [--ignore-directivity] [--check-aim] [--sim-aim-error deg] [--sim-room [absorption]] [--aim-sheet out.csv] [--sweeps N] [--aim-ref spk | --aim-ref-db dB] [--sim-move dx dy dz] [--sim-aim-steps a,b,...] [--sim-screen dB] [--track id|name [--natnet-server ip] [--natnet-multicast g] | --track-sim [--track-sim-bump N] [--track-sim-twist N]] [--mount-offset x,y,z | ring] [--place-tol-mm mm] [--place-timeout s] [--capsule-survey out.json [--speakers list] [--drop-outliers] [--sim-center x,y,z]] [--track-sim-offset x,y,z] [--sim-truth f.json] [--window-m m] [--sim-interferer captures[:t_s[:dB[:click|noise|sweep]]]] [--sim-speaker-latency spk,ms]\n"
                               "  --zylia: the mic is a ZM-1; --input is the FIRST of its 19 consecutive capture channels,\n"
                               "  --mic is the array center. Alone it is the single-placement position survey: distances\n"
                               "  need --latency (loopback, m at c) or --ref <spk> <m> (one tape-measured center->speaker\n"
                               "  distance) or --ref-speakers <list>.\n"
                               "  --ref-speakers 2,6,18-19 (the --zylia survey and --live N --zylia): the latency from 3 or more\n"
                               "  speakers whose layout positions are MEASURED (bwa_speaker_survey --write --fields position, or\n"
                               "  --localize), never planned: a plan position puts its placement error over c in the latency\n"
                               "  (1 cm is 29 us; the optical one inherits --baffle-offset-m, 1 mm is 3 us). Each one's distance\n"
                               "  is |position - the array center| (the measured center tracked, else --mic, which the --zylia\n"
                               "  survey requires); its latency is its center arrival minus that over c. The median is used;\n"
                               "  one more than %.0f us off it is FLAGGED (a different Dante latency, or a wrong position), and\n"
                               "  with no unflagged majority nothing is used (exit 6). A layout speaker with no plan_position,\n"
                               "  or one equal to its position, is warned about: nothing ever measured it.\n"
                               "  --sim-speaker-latency spk,ms (simulate, repeatable): that speaker plays ms later than the rest.\n"
                               "  --zylia --trims measures the trims with it, --zylia --verify the second pass,\n"
                               "  both through a pressure proxy (the power mean over the capsules, the center arrival).\n"
                               "  --verify: sweep every speaker THROUGH the layout's output stage (gain, delay, eq, room_eq)\n"
                               "  and report the leftover arrival and level per speaker. Writes nothing; exit 3 if flagged.\n"
                               "  --aim-sheet out.csv: no audio. The aim each speaker should have toward the listening point,\n"
                               "  as a bearing and a down-tilt, plus a readiness summary. Exit 3 if a layout aim is > 20 deg off.\n"
                               "  --temp: room air temperature; every surveyed range scales with it (2%% of c = 8 cm at\n"
                               "  4 m). Recorded into the layout's reference.speed_of_sound_mps, so set it once per rig.\n"
                               "  --ignore-directivity: with a layout that carries a directivity model, the trims are\n"
                               "  re-aimed from the mic's bearing off each speaker to the listening point's; this skips it.\n"
                               "  --check-aim: with --localize and a directivity model, fit each speaker's acoustic axis from\n"
                               "  how its high/mid tilt changes across the mic positions, and report it against the layout's\n"
                               "  aim. Diagnostic only. --sim-aim-error <deg> gives --simulate a known error to recover.\n"
                               "  --sim-room [absorption]: --simulate inside a shoebox room around the array (image sources\n"
                               "  to order 2, each carrying the directivity model at its own departure angle, plus a late\n"
                               "  tail); absorption 0.3 by default. Exercises the direct-sound gate and its dilution.\n"
                               "  --room-eq-grid rows.txt: one grid position per \"x y z\" row (at most 16), stepped through like\n"
                               "  --localize (tracked: the loose gate, and the MEASURED position is the key; untracked: Enter),\n"
                               "  with the bump check per row, then ONE room_eq_grid written. Bare, it takes --mic.\n"
                               "  --live N --zylia: live aiming for speaker N with the ZM-1 (--mic = the array center, default\n"
                               "  the listening point): one line per sweep with its position against the PLAN (plan_position,\n"
                               "  else the layout's position) and the move in room words (needs --latency or --ref for the\n"
                               "  distance), and its off-axis angle as a tilt peak meter plus an\n"
                               "  estimated MAGNITUDE (never a direction). --aim-ref S takes speaker S's tilt as the on-axis\n"
                               "  reference, --aim-ref-db a stored one; the model's on_axis_db is the fallback. Keys: r stores\n"
                               "  the reference, p resets the peak, any other key stops. --sweeps N bounds any --live run.\n"
                               "  --sim-move / --sim-aim-steps (simulate only): the live speaker's true offset (m) and its\n"
                               "  true aim error (deg off the as-built aim) per reading, both from the as-built position and aim. --sim-screen dB: a screen's HF loss\n"
                               "  (a shelf above 4 kHz) on every speaker's path, which a reference absorbs and the file cannot.\n"
                               "  --track <id|name>: the ZM-1's stand is a tracked rigid body (Motive's frame is the room frame).\n"
                               "  Before the captures the tool shows a live line (the measured center, the target, dx/dy/dz and\n"
                               "  |d| in mm, HOLD or OK) and waits until the center is within --place-tol-mm (default 10) of the\n"
                               "  target and still (2 mm for 0.5 s, then held 1 s); that center IS the mic position. After every\n"
                               "  capture it checks for a bump (a move past half the tolerance) and stops with exit 4. The\n"
                               "  --zylia survey and the --live position readout, which read room DIRECTIONS, also wait for the\n"
                               "  stand's orientation to hold still and stop on a turn past their limit (0.3 deg or more). Targets:\n"
                               "  trims, --verify, --live: --mic, else the layout's listening point; the --zylia survey and\n"
                               "  bare --room-eq-grid: --mic (required); --localize and --room-eq-grid rows.txt: each row, loosely\n"
                               "  (100 mm, or --place-tol-mm if wider, so the gate cannot open at the previous row), and the\n"
                               "  MEASURED position is recorded, which is what trilateration and the grid key need. The mount offset (body origin to the array center, body axes) comes from a body-frame\n"
                               "  --survey, or --mount-offset x,y,z, or --mount-offset ring (fit the circle through the body's\n"
                               "  markers from Motive's model definition, needs --natnet-server; it assumes the marker ring sits\n"
                               "  at the array center's height), else 0. Only the modes that turn capsule arrival differences into\n"
                               "  a room DIRECTION need a body-frame survey: the --zylia survey, and the --live position readout.\n"
                               "  --natnet-server is REQUIRED to track by name. On the rig a key takes the current reading anyway,\n"
                               "  with a warning; --place-timeout s (default 300) aborts the wait. Unverified against live Motive.\n"
                               "  --track-sim (simulate only): a scripted stand walks in from 8 cm off the target, settles about\n"
                               "  5 mm off it, and the captures come from its TRUE center; --track-sim-bump N knocks it 15 mm\n"
                               "  after the Nth capture; --track-sim-twist N turns it 2 deg about the array center after the\n"
                               "  Nth capture, which moves no center. --track-sim-offset x,y,z: the stand's TRUE mount offset\n"
                               "  (body axes) when it differs from the one the tool is given, so a wrong offset can be caught.\n"
                               "  --localize rows.txt --zylia: --localize with the ZM-1. Each range is the arrival at the array\n"
                               "  CENTER (zylia_center_arrival over the 19 capsules), which does not change when the array\n"
                               "  turns: no capsule survey and no orientation needed. --check-aim reads the pooled tilt.\n"
                               "  --capsule-survey out.json --zylia: the capsule survey from the speakers. One placement (--mic\n"
                               "  is the center, or the tracked target), every speaker in --speakers (default all) swept, their\n"
                               "  layout POSITIONS taken as the known sources: trust them (--localize --zylia first). It solves\n"
                               "  the table (zylia_survey), the array center from the center arrivals (the latency too), and\n"
                               "  re-solves there. Untracked it writes a ROOM-AXES survey; tracked, a BODY-FRAME one whose mount\n"
                               "  offset is the acoustic center's. Refused below 4 speakers or a coplanar set (spread < 0.05).\n"
                               "  Leave-one-out scores each speaker against the table the rest solve to (a direction error);\n"
                               "  the range check scores its range from the acoustic center against the median speaker's (a\n"
                               "  distance error, flagged over %.0f mm, which leave-one-out barely sees). A flagged speaker (its\n"
                               "  position or its capture is off) refuses the write with exit 6. --drop-outliers drops the\n"
                               "  worst flagged speaker and solves again, the center included; a second one stops it.\n"
                               "  --capsule-survey takes no --ref or --ref-speakers: it already trusts every swept position\n"
                               "  and solves the latency with the center (--latency, from --localize, pins it).\n"
                               "  --sim-center x,y,z (untracked --capsule-survey --simulate): where the simulated ZM-1 really\n"
                               "  is, so --mic is a taped guess the acoustic center has to correct.\n"
                               "  --sim-truth f.json (simulate): the speakers really stand where f.json says (position, aim),\n"
                               "  not where --layout says: a rehearsal whose as-built differs from its plan.\n"
                               "  Sweep quality (every mode that sweeps): the arrival is searched only in a window around\n"
                               "  the latency plus the layout distance over c; a capture whose strongest tap lies outside it,\n"
                               "  or whose IR peak is under %.0f dB over its noise floor, is re-swept with the reason printed,\n"
                               "  and the trims and --verify count a speaker only once two sweeps agree (%.0f sample, %.1f dB).\n"
                               "  --window-m m: the window's position margin (default %.2f m for a surveyed speaker, %.2f m at\n"
                               "  its plan and in the modes that find a position). --sim-interferer captures[:t_s[:dB[:kind]]]\n"
                               "  (simulate): a click, a noise burst or a stray sweep from 1.5 m off on those 1-based captures.\n",
                               REFSPK_FLAG_S * 1e6, CSURVEY_RANGE_FLAG_MM, CALIB_SWEEP_MIN_SNR_DB, CALIB_SWEEP_AGREE_SAMPLES, CALIB_SWEEP_AGREE_DB,
                               CALIB_WIN_SURVEYED_M, CALIB_WIN_PLAN_M); return 2; }
    }
    if (n_temp && n_c) {
        fprintf(stderr, "calibrate: --temp and --c set the same thing; pass one\n"); return 2; }
    if (room_eq && rq_grid) { fprintf(stderr, "calibrate: --room-eq and --room-eq-grid are mutually exclusive (one scheme per layout)\n"); return 2; }
    if (zylia && check) {
        fprintf(stderr, "calibrate: --zylia runs the position survey, --trims, --verify, --live, --localize or --capsule-survey;\n"
                        "           drop --check\n"); return 2; }
    if (csurvey_out) {   /* the capsule survey is its own mode: one placement, the table, nothing else */
        if (!zylia) {
            fprintf(stderr, "calibrate: --capsule-survey measures the ZM-1's capsules; pass --zylia\n"); return 2; }
        if (trims || verify || localize_file || live_speaker >= 0 || rq_grid || eq || room || ir_prefix || check_aim) {
            fprintf(stderr, "calibrate: --capsule-survey is its own mode; drop --trims/--verify/--localize/--live/--room-eq-grid/\n"
                            "           --eq/--room-eq/--room/--save-irs/--check-aim\n"); return 2; }
        if (survey_path) {
            fprintf(stderr, "calibrate: --capsule-survey measures the table, so it takes no --survey (to start the tracked\n"
                            "           center from an old survey's offset, pass that offset as --mount-offset x,y,z)\n"); return 2; }
        if (ref_spk >= 0 || ref_spk_list) {
            fprintf(stderr, "calibrate: --capsule-survey takes the latency as --latency (from --localize), not --ref or\n"
                            "           --ref-speakers: it already trusts every swept speaker's position and solves the latency\n"
                            "           with the center, and a latency read at the assumed center would tie the acoustic\n"
                            "           center to the one it checks\n"); return 2; }
        if (!mic_set) {
            fprintf(stderr, "calibrate: --capsule-survey needs --mic x y z: the array center%s\n",
                    (track_body || track_sim) ? " the stand is placed at" : " (a taped guess; the run measures it)"); return 2; }
    }
    if (spk_list && !csurvey_out) {
        fprintf(stderr, "calibrate: --speakers picks the --capsule-survey's speakers\n"); return 2; }
    if (ref_spk_list && ref_spk >= 0) {
        fprintf(stderr, "calibrate: --ref and --ref-speakers both set the latency; pass one\n"); return 2; }
    if (nsim_lat && !simulate) {
        fprintf(stderr, "calibrate: --sim-speaker-latency is a --simulate knob: a real box's Dante latency is set in Dante\n"
                        "           Controller\n"); return 2; }
    if (drop_outliers && !csurvey_out) {
        fprintf(stderr, "calibrate: --drop-outliers drops the speakers the --capsule-survey's leave-one-out flags\n"); return 2; }
    if (track_sim_off_set && !track_sim) {
        fprintf(stderr, "calibrate: --track-sim-offset is the SIMULATED stand's true offset; pass --track-sim\n"); return 2; }
    if (intf_n && !simulate) {
        fprintf(stderr, "calibrate: --sim-interferer is a --simulate knob: a real room brings its own\n"); return 2; }
    if (intf_n && aim_csv) {
        fprintf(stderr, "calibrate: --aim-sheet sweeps nothing, so nothing can interfere with it\n"); return 2; }
    if (sim_truth_path && !simulate) {
        fprintf(stderr, "calibrate: --sim-truth is a --simulate knob: a real rig's speakers stand where they stand\n"); return 2; }
    if (sim_truth_path && (live_speaker >= 0 || aim_csv)) {
        fprintf(stderr, "calibrate: --sim-truth is for the sweeping modes; --live moves its own box (--sim-move) and\n"
                        "           --aim-sheet sweeps nothing\n"); return 2; }
    if (sim_center_set && !(simulate && csurvey_out && !track_body && !track_sim)) {
        fprintf(stderr, "calibrate: --sim-center is where the simulated ZM-1 really is in an untracked --capsule-survey\n"
                        "           --simulate (--mic is then the taped guess); tracked, the stand says where it is\n"); return 2; }
    if (live_speaker >= 0 && (localize_file || check || eq || rq_grid || room || ir_prefix || check_aim)) {
        fprintf(stderr, "calibrate: --live is its own mode; drop --localize/--check/--eq/--room-eq/--room-eq-grid/--room/\n"
                        "           --save-irs/--check-aim\n"); return 2; }
    const int live_zy = zylia && live_speaker >= 0;
    if ((aim_ref_spk >= 0 || have_aim_ref_db) && !live_zy) {
        fprintf(stderr, "calibrate: --aim-ref/--aim-ref-db are for --live N --zylia\n"); return 2; }
    if (aim_ref_spk >= 0 && have_aim_ref_db) {
        fprintf(stderr, "calibrate: --aim-ref and --aim-ref-db both set the on-axis reference; pass one\n"); return 2; }
    if ((sim_move_set || nsteps || sim_screen > 0.f) && !(live_zy && simulate)) {
        fprintf(stderr, "calibrate: --sim-move/--sim-aim-steps/--sim-screen are --live N --zylia --simulate knobs\n"); return 2; }
    if (sweeps && live_speaker < 0) {
        fprintf(stderr, "calibrate: --sweeps bounds a --live run\n"); return 2; }
    if (live_zy && sim_aim_err != 0.0) {
        fprintf(stderr, "calibrate: --live --zylia takes its aim error per reading from --sim-aim-steps\n"); return 2; }
    if (aim_csv && (trims || verify || zylia || localize_file || check || live_speaker >= 0 || eq || rq_grid || room || ir_prefix || simulate)) {
        fprintf(stderr, "calibrate: --aim-sheet needs no audio and runs alone (just --layout)\n"); return 2; }
    if (trims && verify) {
        fprintf(stderr, "calibrate: --trims and --verify are separate passes: write the trims, then verify them\n"); return 2; }
    if ((trims || verify) && (localize_file || check || live_speaker >= 0)) {
        fprintf(stderr, "calibrate: --trims/--verify are their own modes; drop --localize/--check/--live\n"); return 2; }
    if (verify && (eq || rq_grid || room || ir_prefix)) {
        fprintf(stderr, "calibrate: --verify checks what the layout already carries and writes nothing; drop\n"
                        "           --eq/--room-eq/--room-eq-grid/--room/--save-irs\n"); return 2; }
    if (zylia && !trims && !verify && !rq_rows_file && !localize_file && !csurvey_out && (eq || rq_grid || room || ir_prefix)) {
        fprintf(stderr, "calibrate: bare --zylia is the position survey, which takes no --eq/--room-eq/--room-eq-grid/\n"
                        "           --room/--save-irs; add --trims to measure trims (and these) with the ZM-1\n"); return 2; }
    if (zylia && trims && eq) {   /* --room-eq sets eq too */
        fprintf(stderr, "calibrate: --eq and --room-eq with the ZM-1 are refused. Both invert ONE impulse response into a\n"
                        "           filter, and the ZM-1 has none that stands for the pressure at its center: above\n"
                        "           about 2 kHz every capsule is colored by the sphere's own shadow, differently for\n"
                        "           each direction, and the filter would bake that coloring in. The trims use a power\n"
                        "           mean over the 19 capsules, which is a level, not a response. --room-eq-grid (30 to\n"
                        "           200 Hz, where the sphere is acoustically transparent) works; for --eq use an omni.\n"); return 2; }
    /* --track: which modes place the mic, and what each one needs */
    g_tracked = track_body != NULL || track_sim;
    g_track_sim = track_sim;
    /* the grid rows, --localize and the capsule survey are modes of their own */
    const int zylia_survey_mode = zylia && !trims && !verify && live_speaker < 0 && !rq_rows_file && !localize_file && !csurvey_out;
    if (ref_spk_list && !csurvey_out && !(live_zy || zylia_survey_mode)) {
        fprintf(stderr, "calibrate: --ref-speakers sets the latency for the --zylia position survey and --live N --zylia\n"
                        "           (--localize solves its own)\n"); return 2; }
    if (track_body && track_sim) {
        fprintf(stderr, "calibrate: --track and --track-sim are alternatives: a real stand or the simulated one\n"); return 2; }
    if (track_sim && !simulate) {
        fprintf(stderr, "calibrate: --track-sim is a --simulate knob: a simulated stand in front of a real array places nothing\n"); return 2; }
    if ((track_sim_bump || track_sim_twist) && !track_sim) {
        fprintf(stderr, "calibrate: --track-sim-bump/--track-sim-twist act on the SIMULATED stand; pass --track-sim\n"); return 2; }
    if (!g_tracked && (tol_set || timeout_set || mount_off_set || nn_server)) {
        fprintf(stderr, "calibrate: --place-tol-mm/--place-timeout/--mount-offset/--natnet-server are --track options\n"); return 2; }
    if (g_tracked && (check || (live_speaker >= 0 && !zylia) || aim_csv)) {
        fprintf(stderr, "calibrate: --track places the mic for the trims, --verify, --localize, --room-eq-grid, the --zylia\n"
                        "           survey, --capsule-survey and --live N --zylia; --check, the omni --live and --aim-sheet\n"
                        "           take no placement\n"); return 2; }
    if (g_tracked && (zylia_survey_mode || (rq_grid && !rq_rows_file)) && !mic_set) {
        fprintf(stderr, "calibrate: --track with %s needs --mic x y z: the target the ZM-1 is placed at\n",
                rq_grid ? "--room-eq-grid (or a rows file: --room-eq-grid rows.txt)" : "the --zylia position survey"); return 2; }
    if (rq_rows_file && mic_set) {
        fprintf(stderr, "calibrate: --room-eq-grid rows.txt takes its positions from the rows; drop --mic\n"); return 2; }
    if (rq_rows_file && (eq || room || ir_prefix || verify)) {
        fprintf(stderr, "calibrate: --room-eq-grid rows.txt measures the grid and nothing else (one placement per row):\n"
                        "           drop --eq/--room/--save-irs/--verify\n"); return 2; }
    if (check_aim && !localize_file) {
        fprintf(stderr, "calibrate: --check-aim rides the --localize captures; pass --localize positions.txt\n"); return 2; }
    if (sim_aim_err != 0.0 && !simulate) {
        fprintf(stderr, "calibrate: --sim-aim-error is a --simulate knob (a real rig has its own aim errors)\n"); return 2; }
    if (sim_aim_err != 0.0) calib_sim_set_aim_error((float)sim_aim_err);
    if (sim_room != 0.0 && !simulate) {
        fprintf(stderr, "calibrate: --sim-room is a --simulate knob (a real rig brings its own room)\n"); return 2; }
    if (sim_room != 0.0) calib_sim_set_room((float)sim_room);
    if (room_eq)
        printf("calibrate: --room-eq corrects the ROOM at the mic position - valid only for a STATIC\n"
               "           listener seated there (SPCAP/VBAP deployments); a roaming listener wants plain --eq.\n"
               "           Place the mic at the listening position, ear height.\n");
    if (rq_grid && !rq_rows_file)
        printf("calibrate: --room-eq-grid measures this mic position's LF modal cuts and merges them into\n"
               "           the layout's room_eq_grid - one run per mic placement, --mic x y z IS the grid\n"
               "           key (a rerun within 5 cm replaces that entry). Cover the working area (ear\n"
               "           height, ~0.5-1 m spacing); the engine interpolates between positions live.\n"
               "           A rows file steps through every placement in one run: --room-eq-grid rows.txt.\n");
    if (!out_path) out_path = layout_path;                    /* in-place by default */

    char err[256] = {0};
    static Layout L;                                          /* never a stack local (layout.h); main is not reentrant */
    if (!layout_load(layout_path, (uint32_t)FS, &L, err, sizeof err)) {
        fprintf(stderr, "calibrate: %s\n", err); return 1;
    }
    const int n = (int)L.count;
    if (aim_csv) return aim_sheet(layout_path, L, aim_csv);   /* no device, no sweep, nothing written to the layout */
    /* --sim-truth: the simulated rig's speakers stand where this file says, and the run is told --layout.
     * Every capture is synthesized from the truth; nothing the run solves or writes reads it. */
    static Layout ST;                                         /* never a stack local (layout.h) */
    if (sim_truth_path) {
        if (!layout_load(sim_truth_path, (uint32_t)FS, &ST, err, sizeof err)) {
            fprintf(stderr, "calibrate: --sim-truth: %s\n", err); return 1; }
        if (ST.count != L.count) {
            fprintf(stderr, "calibrate: --sim-truth %s has %u speakers and %s has %u: one array, two descriptions\n",
                    sim_truth_path, ST.count, layout_path, L.count); return 2; }
        float worst = 0.f; int wk = 0;
        for (uint32_t k = 0; k < L.count; ++k) {
            const float* a = ST.speakers[k].pos; const float* b = L.speakers[k].pos;
            const float d = sqrtf((a[0]-b[0])*(a[0]-b[0]) + (a[1]-b[1])*(a[1]-b[1]) + (a[2]-b[2])*(a[2]-b[2]));
            if (d > worst) { worst = d; wk = (int)k; }
        }
        calib_sim_set_truth(&ST);
        printf("calibrate: simulate: the speakers really stand where %s says (--sim-truth), the worst %.1f mm\n"
               "           from %s's positions (speaker %d)\n", sim_truth_path, worst * 1e3f, layout_path, wk);
    }
    /* --room-eq-grid rows.txt: every row one grid position, stepped through like --localize's, and every
     * row's MEASURED position its key. Refused before any sweep when the grid cannot hold them. */
    static float rq_rows[65][3];                              /* static: main is not reentrant */
    int nrows = 0;
    if (rq_rows_file) {
        nrows = read_positions(rq_rows_file, rq_rows, 65);    /* one past the cap, so "too many" is visible */
        if (nrows < 1) {
            fprintf(stderr, "calibrate: --room-eq-grid %s: no \"x y z\" rows\n", rq_rows_file); return 2; }
        if (nrows > (int)BWA_RQ_GRID_MAX) {
            fprintf(stderr, "calibrate: --room-eq-grid %s: %s%d rows, and room_eq_grid holds %d positions\n"
                            "           (BWA_RQ_GRID_MAX). Split the plan, or thin it.\n",
                    rq_rows_file, nrows > 64 ? "over " : "", nrows > 64 ? 64 : nrows, (int)BWA_RQ_GRID_MAX); return 2; }
        /* rows closer than the writer's replace radius overwrite each other; tracked, each measured key
         * can also land a whole gate tolerance off its row, toward the next one */
        const float min_sep = CALIB_RQ_GRID_REPLACE_M + (g_tracked ? 2.f * loose_tol_m() : 0.f);
        for (int a = 0; a < nrows; ++a)
            for (int b = a + 1; b < nrows; ++b) {
                const float ex = rq_rows[a][0] - rq_rows[b][0], ey = rq_rows[a][1] - rq_rows[b][1], ez = rq_rows[a][2] - rq_rows[b][2];
                const float d = sqrtf(ex * ex + ey * ey + ez * ez);
                if (!(d >= min_sep)) {
                    fprintf(stderr, "calibrate: --room-eq-grid %s: rows %d and %d are %.0f mm apart; keep rows at least %.0f mm\n"
                                    "           apart (the %.0f mm replace radius%s)\n", rq_rows_file, a + 1, b + 1, d * 1e3f,
                            min_sep * 1e3f, CALIB_RQ_GRID_REPLACE_M * 1e3f,
                            g_tracked ? ", plus twice the gate tolerance each measured key may land off its row" : "");
                    return 2;
                }
            }
        /* the existing grid: an entry within the replace radius of a row is replaced, the rest stay */
        int kept = 0;
        for (int p = 0; p < (int)L.rq_grid.npos; ++p) {
            int hit = 0;
            for (int r = 0; r < nrows && !hit; ++r) {
                const float ex = L.rq_grid.pos[p][0] - rq_rows[r][0], ey = L.rq_grid.pos[p][1] - rq_rows[r][1], ez = L.rq_grid.pos[p][2] - rq_rows[r][2];
                hit = ex * ex + ey * ey + ez * ez < CALIB_RQ_GRID_REPLACE_M * CALIB_RQ_GRID_REPLACE_M;
            }
            kept += !hit;
        }
        if (kept + nrows > (int)BWA_RQ_GRID_MAX) {
            fprintf(stderr, "calibrate: --room-eq-grid %s: the layout's grid already holds %d position(s), %d of them away from\n"
                            "           every row, and %d rows would make %d; room_eq_grid holds %d (BWA_RQ_GRID_MAX)\n",
                    rq_rows_file, (int)L.rq_grid.npos, kept, nrows, kept + nrows, (int)BWA_RQ_GRID_MAX); return 2; }
        printf("calibrate: --room-eq-grid %s: %d row(s), each one placement; every row's %s position is its\n"
               "           grid key, and the grid is written once, after the last row (the trims are not touched)\n",
               rq_rows_file, nrows, g_tracked ? "MEASURED" : "typed");
    }
    /* live aiming: the ZM-1 sits at the listening point unless --mic says otherwise */
    if (live_zy && !mic_set) memcpy(mic, L.ref, sizeof mic);
    /* --track: mic is the TARGET until the placement measures it. The trims, --verify and live aiming aim
     * at the listening point unless --mic says otherwise; the measured center is a real position, so the
     * directivity re-aim below may use it (mic_set). */
    if (g_tracked && !localize_file) {
        if (!mic_set) memcpy(mic, L.ref, sizeof mic);
        mic_set = 1;
    }
    /* No explicit flag: inherit the rig's own c from the layout it was surveyed with. Falls back to
     * the 20 C reference, which is also what the synthetic-capture path assumes (calib_capture.cpp),
     * so --simulate stays bit-identical unless you deliberately ask for another temperature. */
    const char* sos_src = "default";
    if (sos_set)                                sos_src = "--temp/--c";
    else if (calib_read_sos(layout_path, &sos)) sos_src = "layout";
    if (nrows)
        printf("calibrate: %d speakers from %s; mic at each of %d row(s)%s%s\n", n, layout_path, nrows,
               g_tracked ? " (--track measures it)" : "", simulate ? "  [SIMULATE]" : "");
    else
        printf("calibrate: %d speakers from %s; mic %s (%.2f %.2f %.2f)%s\n",
               n, layout_path, g_tracked ? "TARGET (--track measures the mic)" : "at", mic[0], mic[1], mic[2],
               simulate ? "  [SIMULATE]" : "");
    printf("           speed of sound %.1f m/s (%s)\n", sos, sos_src);
    if (simulate) {
        char rd[256];
        calib_sim_room_describe(&L, rd, sizeof rd);
        printf("           simulated room: %s\n", rd);
    }
    /* Speaker directivity model (layout `directivity` + per-speaker `aim`; docs/calibration.md).
     * The measured level of each speaker carries its off-axis loss toward the MIC; the trims should
     * describe what the LISTENING POINT hears, which is the loss toward ref. The factor is
     * calib_directivity_corr's: D(ref)/D(mic) averaged the way measure.c's level is, diluted by each
     * capture's reverberant share, so it is computed AFTER the sweeps (it needs direct_frac). A mic at
     * the listening point leaves every trim exactly as measured. The report is the rig-day check that
     * the speakers are aimed where the layout says. */
    int want_corr = 0;
    if (live_zy || nrows || csurvey_out || zylia_survey_mode) {
        /* no trims here: live aiming feeds the model to its angle estimate, the grid rows write only the grid,
         * the capsule survey writes only its table, the --zylia position survey writes only positions */
    } else if (L.dir.nband && !no_dir && !mic_set) {
        /* the default mic (0,0,0) is the floor origin: a 1/r term nobody minds much, but a bearing
         * off every speaker's axis that would put several dB of fictitious correction in the trims */
        printf("directivity: model present but no --mic given, so the mic's bearing off each speaker is\n"
               "             unknown; the trims stay as measured. Pass --mic x y z (the listening point is\n"
               "             (%.2f %.2f %.2f)) to re-aim them.\n", L.ref[0], L.ref[1], L.ref[2]);
    } else if (L.dir.nband && !no_dir) {
        want_corr = 1;
        printf("directivity: %d-band model, split %.0f Hz; the %s re-aimed to the listening point\n"
               "             after the sweeps (the per-speaker table follows them)\n", (int)L.dir.nband, L.dir.split_hz,
               verify ? "verify levels are" : "trims are");
    } else if (L.dir.nband) {
        printf("directivity: model present, --ignore-directivity given: trims stay as measured at the mic\n");
    }
    if (ref_spk >= 0 && !live_zy && !zylia_survey_mode) {
        /* it used to be ignored silently, so a run that LOOKED latency-calibrated was not */
        fprintf(stderr, "calibrate: --ref sets the latency for --live N --zylia and the --zylia position survey only\n"); return 2;
    }
    if (ref_spk >= 0 && (ref_spk >= n || ref_dist < 0.2)) {
        fprintf(stderr, "calibrate: --ref wants a speaker 0..%d and a distance >= 0.2 m (got %d, %.3f)\n",
                n - 1, ref_spk, ref_dist); return 2;
    }
    /* --ref-speakers: three or more distinct speakers, a center to measure their distances from, and never
     * the box live aiming is moving (its position is the one in question) */
    static int rs_spk[BWA_MAX_CHANNELS];
    int rs_n = 0;
    if (ref_spk_list) {
        char e[160] = { 0 };
        rs_n = parse_speakers(ref_spk_list, n, rs_spk, BWA_MAX_CHANNELS, e, sizeof e);
        if (rs_n < 0) { fprintf(stderr, "calibrate: --ref-speakers %s\n", e); return 2; }
        if (rs_n < REFSPK_MIN) {
            fprintf(stderr, "calibrate: --ref-speakers names %d speaker(s); a median needs %d or more to outvote one bad box\n",
                    rs_n, REFSPK_MIN); return 2; }
        if (zylia_survey_mode && !mic_set) {
            fprintf(stderr, "calibrate: --ref-speakers measures each distance from the array center: pass --mic x y z (or\n"
                            "           --track), since the latency inherits every millimeter of error in it\n"); return 2; }
        for (int k = 0; k < rs_n; ++k)
            if (live_zy && rs_spk[k] == live_speaker) {
                fprintf(stderr, "calibrate: --ref-speakers names speaker %d, the one live aiming moves: its position is the\n"
                                "           one in question, so it cannot vouch for the latency\n", live_speaker); return 2; }
    }
    if (nsim_lat) {
        for (int k = 0; k < nsim_lat; ++k) {
            if (sim_lat_spk[k] >= n) {
                fprintf(stderr, "calibrate: --sim-speaker-latency speaker %d: speakers run 0..%d\n", sim_lat_spk[k], n - 1); return 2; }
            calib_sim_set_speaker_latency(sim_lat_spk[k], sim_lat_s[k]);
            printf("calibrate: simulate: speaker %d plays %+.3f ms later than the rest (--sim-speaker-latency), a\n"
                   "           different Dante latency setting\n", sim_lat_spk[k], sim_lat_s[k] * 1e3);
        }
    }
    if (survey_path && !g_tracked) {   /* pins the ZM-1's channel order + orientation; installs via zylia_set_capsules */
        ZyliaMount mount; char serr[192] = {0};
        if (!zylia_survey_load(survey_path, &mount, serr, sizeof serr)) {
            fprintf(stderr, "calibrate: survey: %s\n", serr); return 1; }
        if (mount.body_frame) {
            fprintf(stderr, "calibrate: survey %s is BODY-FRAME (tracked mount): with no tracker the table cannot be\n"
                            "           re-aimed. Pass --track <body> (the stand's rigid body), or use a room-axes survey\n"
                            "           taken at the CURRENT mounting (calib_view -> Zylia -> Capsule survey).\n",
                    survey_path);
            return 1;
        }
        printf("calibrate: capsule survey %s installed (channel order + orientation pinned)\n", survey_path);
    }
    if (g_tracked) {   /* the survey (if any) loads through the tracker, which knows what the mode needs */
        MicTrackCfg mc;
        memset(&mc, 0, sizeof mc);
        mc.body = track_body; mc.server = nn_server; mc.multicast = nn_multicast;
        mc.survey_path = survey_path;
        mc.need_body_frame = zylia_survey_mode;               /* the one mode that turns DOAs into room directions */
        mc.have_offset = mount_off_set && !mount_ring;
        memcpy(mc.offset_m, mount_off, sizeof mc.offset_m);
        mc.offset_ring = mount_ring;
        mc.sim = track_sim ? MIC_SIM_SCRIPT : MIC_SIM_OFF;
        mc.sim_bump_after = track_sim_bump;
        mc.sim_twist_after = track_sim_twist;
        mc.sim_have_true_offset = track_sim_off_set;
        memcpy(mc.sim_true_offset_m, track_sim_off, sizeof mc.sim_true_offset_m);
        mc.virtual_clock = 1;                                 /* simulated time: no waiting on the wall clock */
        char e[400] = { 0 };
        const int rc = mic_track_open(&g_mt, &mc, e, sizeof e);
        if (rc) {
            fprintf(stderr, "calibrate: %s\n", e);
            if (zylia_survey_mode && rc == 2)
                fprintf(stderr, "           (the --zylia survey turns capsule arrival differences into room directions, so a\n"
                                "           tracked run needs the array's orientation: a BODY-FRAME survey. The trims, --verify\n"
                                "           and the live tilt meter only need the center: --mount-offset.)\n");
            return rc;
        }
        std::atexit([]() { mic_track_close(&g_mt); });
        char d[800];
        mic_track_describe(&g_mt, d, sizeof d);
        for (char* line = strtok(d, "\n"); line; line = strtok(NULL, "\n")) printf("track: %s\n", line);
        if (track_sim)
            printf("track: SIMULATED stand (--track-sim), tolerance %.1f mm, bump limit %.1f mm%s%s\n", g_tol_m * 1e3f,
                   0.5f * g_tol_m * 1e3f, track_sim_bump ? ", knocked 15 mm mid-run (--track-sim-bump)" : "",
                   track_sim_twist ? ", twisted 2 deg about the center mid-run (--track-sim-twist)" : "");
        if (track_sim_off_set)
            printf("track-sim: the stand's TRUE mount offset is (%.4f %.4f %.4f) m, body axes (--track-sim-offset), not the\n"
                   "           offset above: every center the tool takes is off by the difference\n",
                   track_sim_off[0], track_sim_off[1], track_sim_off[2]);
        else
            printf("track: rigid body '%s', tolerance %.1f mm, bump limit %.1f mm (unverified against live Motive)\n",
                   track_body, g_tol_m * 1e3f, 0.5f * g_tol_m * 1e3f);
        if (survey_path)
            printf("calibrate: capsule survey %s installed\n", survey_path);
    }

    /* --capsule-survey: the speaker set and its geometry from the target, refused before the device opens */
    static int cs_spk[BWA_MAX_CHANNELS];
    int cs_n = 0;
    static CSurveyArgs CS;                                    /* static: main is not reentrant */
    if (csurvey_out) {
        if (spk_list) {
            char e[160] = { 0 };
            cs_n = parse_speakers(spk_list, n, cs_spk, BWA_MAX_CHANNELS, e, sizeof e);
            if (cs_n < 0) { fprintf(stderr, "calibrate: --speakers %s\n", e); return 2; }
        } else
            for (int s = 0; s < n; ++s) cs_spk[cs_n++] = s;
        CS.L = &L; CS.spk = cs_spk; CS.nspk = cs_n; memcpy(CS.mic, mic, sizeof CS.mic); CS.sos = sos;
        CS.simulate = simulate; CS.in_first = mic_in; CS.out_path = csurvey_out;
        CS.have_sim_center = sim_center_set; memcpy(CS.sim_center, sim_center, sizeof CS.sim_center);
        CS.known_latency_m = known_latency;
        CS.drop_outliers = drop_outliers;
        if (cs_n < CSURVEY_MIN_SPK) {
            fprintf(stderr, "calibrate: --capsule-survey: %d speaker(s); the survey needs %d or more, and %d or more for\n"
                            "           the acoustic center\n", cs_n, CSURVEY_MIN_SPK, CSURVEY_CENTER_MIN_SPK);
            return 2;
        }
        if (const int rc = csurvey_check(CS, CS.spk, CS.nspk, mic, g_tracked ? "the target" : "--mic", 2)) return rc;
    }

    float* sweep = (float*)malloc((size_t)NSWEEP * sizeof(float));
    float* cap   = (float*)malloc((size_t)CAPLEN * sizeof(float));
    MeasureResult* res = (MeasureResult*)calloc((size_t)n, sizeof(MeasureResult));
    if (!sweep || !cap || !res) { fprintf(stderr, "calibrate: out of memory\n"); return 1; }
    measure_sweep(sweep, NSWEEP, F1, F2, FS);
    double rt60_sum = 0.0; int rt60_n = 0;                     /* room-report aggregate */

    float* cap19 = NULL;                                       /* --zylia: [19][CAL_CAPLEN], one row per capsule */
    if (zylia) {
        cap19 = (float*)malloc((size_t)ZYLIA_MICS * CAPLEN * sizeof(float));
        if (!cap19) { fprintf(stderr, "calibrate: out of memory\n"); return 1; }
    }
    /* --verify on the rig plays the sweep through the output stage, longer than the sweep by the
     * stage's delay and FIR; the recording stays CAL_CAPLEN, so the tail must still hold the
     * latency (about 60 ms through Dante Via) and the room decay after it */
    const int nplay = NSWEEP + calib_stage_pad(&L);
    if (verify && nplay > CAPLEN - CAL_NTAIL / 2) {
        fprintf(stderr, "calibrate: --verify: the layout's delays (%.1f ms at most) leave the %.2f s capture tail too\n"
                        "           short; the stage adds %d samples to the sweep\n",
                1e3 * L.max_delay_samples / FS, CAL_NTAIL / FS, nplay - NSWEEP);
        return 1;
    }

#ifdef BWA_HAVE_ASIO
    int asio_up = 0;
    if (!simulate) {
        if (zylia) {
            if (calib_asio_open_multi(driver, mic_in, ZYLIA_MICS, n, sweep, cap19) != 0) return 1;
        } else if (calib_asio_open(driver, mic_in, n, sweep, cap) != 0) return 1;
        asio_up = 1;
    }
#else
    if (!simulate) { fprintf(stderr, "calibrate: built without ASIO; re-run with --simulate or build the ASIO backend\n"); return 1; }
#endif
    /* the expected-arrival window's latency prior: after the open, which is when the driver's loop is
     * known. --latency is c * tau in meters. Live aiming's --ref is measured inside the mode, which
     * settles its own. */
    {
        char wd[300];
        g_have_win = calib_window_prior(simulate, known_latency >= 0.0 ? known_latency / sos : -1.0, -1.0, &g_win, wd, sizeof wd);
        printf("calibrate: arrival window: %s%s\n", wd, g_window_m >= 0.0 ? " (--window-m sets the distance margin)" : "");
    }
    if (intf_n) {
        calib_sim_set_interferer(intf_caps, intf_n, &intf);
        printf("calibrate: simulate: an interferer (%s, %.1f dB at the mic, %.2f s into the capture) on capture(s)",
               intf.kind == CALIB_SIM_INTF_NOISE ? "a 250 ms noise burst" : intf.kind == CALIB_SIM_INTF_SWEEP ? "a stray sweep" : "a 2 ms click",
               intf.level_db, intf.t_s);
        for (int k = 0; k < intf_n; ++k) printf("%s%d", k ? "," : " ", intf_caps[k]);
        printf(" (--sim-interferer)\n");
    }

    /* --- self-localization: capture at K known mic positions, trilaterate each speaker --- */
    if (localize_file) {
        float micpos[64][3];
        int K = read_positions(localize_file, micpos, 64);
        if (K < 5) {
            fprintf(stderr, "calibrate: --localize needs >= 5 non-coplanar mic positions in %s (got %d)\n", localize_file, K);
#ifdef BWA_HAVE_ASIO
            if (asio_up) calib_asio_close();
#endif
            return 1;
        }
        printf("localize: %d mic positions, %d speakers each\n", K, n);
        double* range = (double*)malloc((size_t)n * K * sizeof(double));
        float*  tilt  = (float*)calloc((size_t)n * K, sizeof(float));   /* --check-aim: high/mid tilt per capture */
        float*  tiltw = (float*)calloc((size_t)n * K, sizeof(float));   /* ... the same from the WHOLE response (dilution) */
        if (check_aim && !L.dir.nband)
            printf("check-aim: the layout carries no directivity model, so there is nothing to check the tilts against\n"
                   "           (tools/directivity/clf_to_json.py --into %s)\n", layout_path);
        float planned[64][3];
        memcpy(planned, micpos, sizeof planned);
        /* --zylia: the ZM-1 is the mic. Each range is the arrival at the array CENTER from the 19 capsules
         * (calib_measure_speaker's pressure proxy: zylia_center_arrival), not one capsule's. One capsule
         * on the 49 mm sphere is early or late by up to R/c (143 us, 49 mm) depending on where the speaker
         * is, and that bias moves with the speaker, so the trilateration cannot fit it out as latency.
         * The center arrival does not change when the array turns, so no survey is needed. */
        static Pass LP;                                    /* static: it carries the capsule table */
        /* both mics through measure_speaker, so every capture gets the window and the re-sweep. The
         * window is read from the layout's position (the plan, before any survey) with the PLAN margin:
         * this mode finds the positions, so it must not assume them */
        LP.c.L = &L; LP.c.sos = sos; LP.c.simulate = simulate; LP.c.zylia = zylia;
        LP.c.sweep = sweep; LP.c.cap = cap; LP.c.cap19 = cap19;
        LP.c.band_hz = AIM_BAND_HZ;                        /* the delay and --check-aim's tilt are read */
        LP.in_first = mic_in;
        if (zylia) {
            printf("localize: the ZM-1: each range is the arrival at the array center (the 19 capsules' mean,\n"
                   "          tilt-corrected), which does not change when the array turns; --mic rows are the center\n");
            if (!survey_path)
                printf("localize: no --survey: the built-in table's channel order. A wrong order moves each center\n"
                       "          arrival by up to 15 us (5 mm): check it with bwa_zylia_probe first.\n");
            if (simulate)
                printf("localize: simulate: the captures come from the simulated physical ZM-1 (calib_sim_zm1_room),\n"
                       "          turned %s, never from the table the solve reads\n",
                       g_track_sim ? "the way the scripted stand turns it" : "the untracked simulation's fixed way");
        }
        for (int k = 0; k < K; ++k) {
            if (g_tracked) {
                /* the row is the PLAN; the tracker measures where the mic actually stands, and that
                 * is what the trilateration gets */
                char what[64]; snprintf(what, sizeof what, "localize position %d/%d", k + 1, K);
                if (track_place(planned[k], "trilateration", what, micpos[k], 0.f)) {
#ifdef BWA_HAVE_ASIO
                    if (asio_up) calib_asio_close();
#endif
                    return 1;
                }
            } else if (!simulate) { printf("  -> place the mic at (%.2f %.2f %.2f) and press Enter...", micpos[k][0], micpos[k][1], micpos[k][2]); fflush(stdout); getchar(); }
            for (int s = 0; s < n; ++s) {
                MeasureResult r;
                if (zylia && simulate) {   /* the physical array as it is turned right now, at the stand's TRUE center */
                    float q[4];
                    calib_sim_zm1_room(g_track_sim && mic_track_sim_orientation(&g_mt, q) ? q : NULL, LP.c.caps);
                }
                LP.c.sim_at = g_track_sim ? g_sim_at : micpos[k];
                pass_quality(LP.c, micpos[k], CALIB_WIN_PLAN_M, 0);
                CalibMeasInfo zmi;
                if (!measure_speaker(LP, s, 0, &r, &zmi)) {
                    fprintf(stderr, "calibrate: localize stopped at speaker %d, position %d\n", s, k + 1);
#ifdef BWA_HAVE_ASIO
                    if (asio_up) calib_asio_close();
#endif
                    return 1;
                }
                if (const int bump = track_after_capture(CAPTURE_S * zmi.sweeps, "localize placement", s)) {
#ifdef BWA_HAVE_ASIO
                    if (asio_up) calib_asio_close();
#endif
                    return bump;
                }
                /* AIM_BAND_HZ, not BAND_HZ (LP.c.band_hz): only the delay and the tilt are read here, and
                 * the tilt is the DIRECT sound's, over the bands calib_check_aim predicts it on */
                range[(size_t)s * K + k] = ((double)r.delay_samples + r.delay_frac) * sos / FS; /* c*delay, meters (sub-sample) */
                tilt[(size_t)s * K + k] = (r.band_direct[1] > 1e-9f && r.band_direct[2] > 1e-9f)
                                        ? (float)(20.0 * log10((double)r.band_direct[2] / (double)r.band_direct[1])) : 0.f;
                tiltw[(size_t)s * K + k] = (r.band[1] > 1e-9f && r.band[2] > 1e-9f)
                                         ? (float)(20.0 * log10((double)r.band[2] / (double)r.band[1])) : 0.f;
            }
        }
#ifdef BWA_HAVE_ASIO
        if (asio_up) calib_asio_close();
#endif
        sweep_report("localize");
        if (g_tracked) {
            printf("localize: the MEASURED mic positions the trilateration uses (planned in brackets):\n");
            for (int k = 0; k < K; ++k)
                printf("  pos %2d: (%+.4f %+.4f %+.4f)  [%+.3f %+.3f %+.3f]\n", k + 1, micpos[k][0], micpos[k][1], micpos[k][2],
                       planned[k][0], planned[k][1], planned[k][2]);
        }
        float (*pos)[3] = (float(*)[3])malloc((size_t)n * 3 * sizeof(float));
        double* latv = (double*)malloc((size_t)n * sizeof(double));
        int failed = 0;
        for (int s = 0; s < n; ++s) {
            double lat = 0;
            latv[s] = -1.0;
            if (!calib_trilaterate(&range[(size_t)s * K], micpos, K, pos[s], &lat)) {
                /* keep what the file already says rather than write the floor origin over it: (0,0,0)
                 * is a confident-looking position the engine would then render from */
                fprintf(stderr, "  spk %2d: trilateration failed (degenerate mic positions?); its position is left as it was\n", s);
                memcpy(pos[s], L.speakers[s].pos, sizeof pos[s]); ++failed;
            } else {
                printf("  spk %2d: pos=(%+.3f %+.3f %+.3f)  [system latency %.3f m]\n", s, pos[s][0], pos[s][1], pos[s][2], lat);
                latv[s] = lat;
            }
        }
#ifdef BWA_HAVE_ASIO
        /* Cross-check the solved latency against the driver's own numbers: the solve recovers the
         * FULL loop (digital buffers + DAC/ADC + analog), the driver reports the digital half, so
         * solved-minus-driver must be positive. Negative is physically impossible (device mix-up,
         * clocking). How large it may be depends on the mic (calib_latency_check): a few ms for an
         * omni, where tens of ms point at an unexpected buffer, and with the ZM-1 the Dante Via leg the
         * driver never reports, tens of ms on a CORRECT run, up to the arrival window's own allowance.
         * Median over speakers — per-speaker latency should agree, it is one system. */
        { long il = 0, ol = 0;
          if (!simulate && calib_asio_latencies(&il, &ol)) {
              double lats[BWA_MAX_CHANNELS]; int nl = 0;
              for (int s = 0; s < n; ++s) if (latv[s] >= 0.0) lats[nl++] = latv[s];
              if (nl > 0) {
                  for (int a = 1; a < nl; ++a) { double v = lats[a]; int b = a;   /* tiny insertion sort */
                      while (b > 0 && lats[b-1] > v) { lats[b] = lats[b-1]; --b; } lats[b] = v; }
                  double med_m = (nl & 1) ? lats[nl/2] : 0.5 * (lats[nl/2 - 1] + lats[nl/2]);
                  double drv_m = sos * (double)(il + ol) / FS;
                  double resid_s = 0.0;
                  const int lc = calib_latency_check(zylia, med_m / sos, drv_m / sos, &resid_s);
                  printf("localize: solved system latency %.3f m (%.2f ms) vs driver digital loop %.3f m (%.2f ms) -> residual %+.2f ms\n",
                         med_m, med_m / sos * 1e3, drv_m, drv_m / sos * 1e3, resid_s * 1e3);
                  if (lc == CALIB_LAT_IMPOSSIBLE)
                      printf("  WARNING: solved latency is BELOW the driver's own digital loop - physically impossible;\n"
                             "           check the device/clocking (wrong driver? sample-rate mismatch?)\n");
                  else if (lc == CALIB_LAT_WARN && zylia)
                      printf("  WARNING: residual is over %.0f ms, more than the Dante Via leg the ZM-1 adds and more than\n"
                             "           the arrival window allows past the driver's loop - check the Via latency setting,\n"
                             "           an extra buffer, or pass --latency\n", calib_latency_bound_s(1) * 1e3);
                  else if (lc == CALIB_LAT_WARN)
                      printf("  WARNING: residual is over %.0f ms, unexpectedly large for DAC/ADC + analog - check the\n"
                             "           Dante latency setting / an extra buffer in the loop\n", calib_latency_bound_s(0) * 1e3);
                  else if (zylia)
                      printf("  (the ZM-1's Dante Via leg is in this residual: the driver does not report it; up to %.0f ms is expected)\n",
                             calib_latency_bound_s(1) * 1e3);
              }
          } }
#endif
        if (check_aim && L.dir.nband) {
            /* The aim check (calib_check_aim): each speaker's measured high/mid tilt across the mic
             * positions against the model's prediction for its aim, mean removed; a grid search
             * around the layout aim finds the direction that explains the tilts best. Bearings use
             * the SOLVED positions where the solve succeeded. Then the residual every speaker shares
             * as a function of bearing: a slope there is a loss the model does not know (the screens)
             * or a model that does not fit these boxes, and it is not an aim error. */
            static Layout LA;                                /* never a stack local (layout.h) */
            LA = L;
            for (int s = 0; s < n; ++s) if (latv[s] >= 0.0) memcpy(LA.speakers[s].pos, pos[s], sizeof pos[s]);
            printf("check-aim: fitted acoustic axis per speaker (tilt = direct-sound high/mid level, %.0f-%.0f Hz\n"
                   "           against %.0f Hz up, dB; %d positions)\n", AIM_BAND_HZ[0], AIM_BAND_HZ[1], AIM_BAND_HZ[1], K);
            printf("           spk  spread  rms@layout  rms@fit   aim error  fitted aim                whole-resp\n");
            double sxx = 0, sxy = 0, sx = 0, sy = 0; int nn = 0;
            int flagged = 0;
            /* the gated fit's error and the whole-response fit's, per fitted speaker: the gap is the
             * reverberant dilution the gate removes (docs/calibration.md) */
            float err_g[BWA_MAX_CHANNELS], err_w[BWA_MAX_CHANNELS]; int nerr = 0;
            for (int s = 0; s < n; ++s) {
                CalibAimResult ar;
                calib_check_aim(&LA, s, micpos, &tilt[(size_t)s * K], K, AIM_BAND_HZ, F2, &ar);
                if (!ar.ok) {
                    printf("           %3d  %5.1f    %5.2f       -         -        (refused: %s)\n", s, ar.spread_deg, ar.rms_layout_db,
                           K < 3 ? "fewer than 3 positions" : "under 8 deg of bearing spread");
                    continue;
                }
                int flag = ar.aim_err_deg > 15.f && ar.rms_layout_db - ar.rms_fit_db > 0.5f;
                flagged += flag;
                CalibAimResult aw;
                calib_check_aim(&LA, s, micpos, &tiltw[(size_t)s * K], K, AIM_BAND_HZ, F2, &aw);
                printf("           %3d  %5.1f    %5.2f     %5.2f     %5.1f deg  (%+.3f %+.3f %+.3f)  %5.1f deg%s\n", s, ar.spread_deg,
                       ar.rms_layout_db, ar.rms_fit_db, ar.aim_err_deg, ar.aim_fit[0], ar.aim_fit[1], ar.aim_fit[2],
                       aw.ok ? aw.aim_err_deg : 0.f, flag ? "   <- AIM: check the mount, or the layout's aim" : "");
                if (aw.ok) { err_g[nerr] = ar.aim_err_deg; err_w[nerr] = aw.aim_err_deg; ++nerr; }
                /* the shared residual: layout-aim tilt residual (mean removed) against bearing */
                double m = 0; float rk[64];
                for (int k = 0; k < K; ++k) {
                    float th = layout_speaker_off_axis_deg(&LA, (uint32_t)s, micpos[k]);
                    rk[k] = tilt[(size_t)s * K + k] - calib_aim_tilt_db(&L.dir, th, AIM_BAND_HZ, F2);
                    m += rk[k];
                }
                m /= K;
                for (int k = 0; k < K; ++k) {
                    double x = layout_speaker_off_axis_deg(&LA, (uint32_t)s, micpos[k]), y = rk[k] - m;
                    sx += x; sy += y; sxx += x * x; sxy += x * y; ++nn;
                }
            }
            if (nn > 2) {
                double den = nn * sxx - sx * sx;
                double slope = den > 1e-9 ? (nn * sxy - sx * sy) / den : 0.0;
                printf("check-aim: shared residual vs bearing: %+.2f dB per 10 deg over %d captures", slope * 10.0, nn);
                printf(fabs(slope * 10.0) > 0.5 ? "  <- a loss the model does not carry (screens?), not an aim error\n" : "  (model fits)\n");
            }
            if (nerr > 0) {
                /* medians (insertion sort; nerr <= the speaker count) */
                for (int a = 1; a < nerr; ++a) {
                    float vg = err_g[a], vw = err_w[a]; int b = a;
                    while (b > 0 && err_g[b-1] > vg) { err_g[b] = err_g[b-1]; --b; } err_g[b] = vg;
                    b = a;
                    while (b > 0 && err_w[b-1] > vw) { err_w[b] = err_w[b-1]; --b; } err_w[b] = vw;
                }
                const float mg = (nerr & 1) ? err_g[nerr/2] : 0.5f * (err_g[nerr/2 - 1] + err_g[nerr/2]);
                const float mw = (nerr & 1) ? err_w[nerr/2] : 0.5f * (err_w[nerr/2 - 1] + err_w[nerr/2]);
                printf("check-aim: median aim error: gated %.1f deg, whole-response %.1f deg%s\n", mg, mw,
                       mw < mg - 1.f ? ": the whole response under-reads it (its reverberant share dilutes the tilt)"
                                     : " (no dilution to speak of: an anechoic capture, or a dead room)");
            }
            printf("check-aim: %d speaker(s) flagged. Diagnostic only: nothing is written back.\n", flagged);
        }
        free(tilt);
        free(tiltw);
        free(latv);
        int nplan = 0;
        if (!calib_write_positions(layout_path, out_path, pos, n, &nplan, err, sizeof err)) { fprintf(stderr, "calibrate: %s\n", err); return 1; }
        record_sos(out_path, sos);
        printf("localize: wrote %d positions to %s%s\n", n, out_path, failed ? "  (the failed ones left as they were)" : "");
        report_plan("localize", nplan, n);
        free(range); free(pos); free(res); free(cap); free(sweep);
        return 0;
    }

    /* --- the capsule survey from the speakers: one placement, the table (and the center) --- */
    if (csurvey_out) {
        CS.sweep = sweep; CS.cap = cap; CS.cap19 = cap19;
        const int rc = capsule_survey(CS);
#ifdef BWA_HAVE_ASIO
        if (asio_up) calib_asio_close();
#endif
        free(cap19); free(res); free(cap); free(sweep);
        return rc;
    }

    /* --- live aiming with the ZM-1: one speaker, one line per sweep --- */
    if (live_zy) {
        if (live_speaker >= n || aim_ref_spk >= n) {
            fprintf(stderr, "calibrate: --live %d / --aim-ref %d out of range (0..%d)\n", live_speaker, aim_ref_spk, n - 1);
#ifdef BWA_HAVE_ASIO
            if (asio_up) calib_asio_close();
#endif
            return 1;
        }
        /* live aiming is a direction mode only while its position readout is on (a body-frame survey);
         * the tilt meter reads the center. The range is the one speaker's. */
        const float* lsp = L.speakers[live_speaker].pos;
        const float lrange = sqrtf((lsp[0] - mic[0]) * (lsp[0] - mic[0]) + (lsp[1] - mic[1]) * (lsp[1] - mic[1]) +
                                   (lsp[2] - mic[2]) * (lsp[2] - mic[2]));
        if (g_tracked && track_place(mic, NULL, "live aiming", mic, g_mt.body_frame ? place_turn_limit_deg(g_tol_m, lrange) : 0.f)) {
#ifdef BWA_HAVE_ASIO
            if (asio_up) calib_asio_close();
#endif
            return 1;
        }
        static LiveArgs A;                                     /* static: main is not reentrant */
        A.L = &L; A.spk = live_speaker; memcpy(A.mic, mic, sizeof A.mic); A.sos = sos; A.simulate = simulate;
        A.known_latency_m = known_latency; A.ref_spk = ref_spk; A.ref_dist = ref_dist;
        A.ref_spks = rs_spk; A.nref_spks = rs_n;
        A.center_what = g_tracked ? "the measured center" : (mic_set ? "--mic" : "the listening point (no --mic)");
        if (rs_n) refspk_plan_warnings(L, rs_spk, rs_n, "live");
        A.aim_ref_spk = aim_ref_spk; A.have_aim_ref_db = have_aim_ref_db; A.aim_ref_db = aim_ref_db;
        A.sweeps = sweeps; memcpy(A.sim_move, sim_move, sizeof A.sim_move); A.sim_screen_db = sim_screen;
        A.sim_steps = sim_steps; A.nsteps = nsteps;
        A.cap19 = cap19; A.in_first = mic_in; A.have_survey = survey_path != NULL;
        A.sim_at = g_track_sim ? g_sim_at : NULL;
        A.pos_ok = !g_tracked || g_mt.body_frame;             /* a tracked position readout needs the orientation */
#ifdef BWA_HAVE_ASIO
        if (!simulate && known_latency < 0.0 && ref_spk < 0 && !rs_n) {
            long il = 0, ol = 0;
            if (calib_asio_latencies(&il, &ol))
                printf("  (driver digital loop = %.3f m: a lower bound for --latency; the ZM-1 chain adds about 60 ms)\n",
                       sos * (double)(il + ol) / FS);
        }
#endif
        const int rc = live_zylia(A);
        track_report("live aiming ran");
#ifdef BWA_HAVE_ASIO
        if (asio_up) calib_asio_close();
#endif
        free(cap19); free(res); free(cap); free(sweep);
        return rc;
    }

    /* --- ZM-1 single-position localization: ONE mic placement, 19 capsules -> direction + distance --- */
    if (zylia && !trims && !verify && !nrows) {
        /* tracked: the measured center is the array center, and the body-frame table is re-aimed for
         * however the stand is turned, BEFORE the capsule positions are read below */
        /* a direction mode: the range is the farthest speaker, whose position a turn moves the most */
        if (g_tracked && track_place(mic, NULL, "zylia survey", mic, place_turn_limit_deg(g_tol_m, farthest_speaker_m(L, mic)))) {
#ifdef BWA_HAVE_ASIO
            if (asio_up) calib_asio_close();
#endif
            return 1;
        }
        float caps[ZYLIA_MICS][3]; zylia_capsules(caps);       /* the installed survey if any, else built-in */
        const double C = sos;
        printf("zylia: array at (%.2f %.2f %.2f), %d capsules%s\n",
               mic[0], mic[1], mic[2], ZYLIA_MICS, simulate ? "  [SIMULATE]" : "");
        if (!simulate && !survey_path)
            printf("zylia: no --survey: trusting the BUILT-IN capsule table. Channel order and the device's\n"
                   "       orientation in the room are then unpinned - a yaw error rotates every recovered\n"
                   "       position (docs/calibration.md, \"The capsule self-survey\").\n");

        /* capture first, solve after: --ref needs the reference speaker's arrivals before any distance */
        double* arr = (double*)malloc((size_t)n * ZYLIA_MICS * sizeof(double));
        if (!arr) { fprintf(stderr, "calibrate: out of memory\n"); return 1; }
        for (int s = 0; s < n; ++s) {
            double* row = arr + (size_t)s * ZYLIA_MICS;
            if (simulate) {                                    /* exact wavefront from the speaker's true pos */
                /* the simulator's own latency, the one every other simulated mode carries (or exactly
                 * --latency), plus this box's own extra (--sim-speaker-latency) */
                double lat0 = ((known_latency >= 0.0) ? known_latency / C : CAL_SIM_LATENCY_SAMPLES / FS)
                            + calib_sim_speaker_latency(s);
                const float* at = g_track_sim ? g_sim_at : mic;    /* --track-sim: the stand's TRUE center */
                for (int j = 0; j < ZYLIA_MICS; ++j) {
                    double cx = at[0]+caps[j][0], cy = at[1]+caps[j][1], cz = at[2]+caps[j][2];
                    const float* sp = sim_truth_path ? ST.speakers[s].pos : L.speakers[s].pos;
                    double dx = cx-sp[0], dy = cy-sp[1], dz = cz-sp[2];
                    row[j] = sqrt(dx*dx+dy*dy+dz*dz)/C + lat0;
                }
            }
#ifdef BWA_HAVE_ASIO
            else {
                /* RIG: sweep speaker s, record all 19 capsules in lockstep (one device, one clock:
                 * the Dante Via route), deconvolve each, take its sub-sample arrival. */
                /* the window from the layout's position with the PLAN margin (this mode finds the position)
                 * plus the capsules' radius; a rejected sweep is re-swept, the reason printed */
                int wlo = 0, whi = 0;
                if (g_have_win) {
                    CalibWindow w = g_win;
                    w.pos_m = mode_window_m(CALIB_WIN_PLAN_M) + CALIB_WIN_ZM1_M;
                    if (!calib_arrival_window(&w, L.speakers[s].pos, mic, sos, FS, &wlo, &whi)) wlo = whi = 0;
                }
                MeasureResult rz[ZYLIA_MICS], pq;
                int okz[ZYLIA_MICS] = { 0 }, clean = 0;
                for (int k = 0; k < CALIB_SWEEP_MAX_TRIES && !clean; ++k) {
                    printf("  speaker %2d: playing sweep...\n", s); fflush(stdout);
                    if (!calib_asio_capture(s)) {
                        fprintf(stderr, "calibrate: capture timed out on speaker %d\n", s);
                        calib_asio_close(); free(cap19); free(arr); free(res); free(cap); free(sweep);
                        return 1;
                    }
                    if (!calib_measure_zylia_rows(cap19, CAPLEN, CAPLEN, sweep, NSWEEP, BAND_HZ, wlo, whi, rz, okz))
                        printf("  speaker %2d: capsule cross-correlation did not run; using each capsule's own peak\n", s);
                    memset(&pq, 0, sizeof pq);
                    calib_pool_quality(rz, &pq);
                    const int qc = calib_sweep_check(&pq, CALIB_SWEEP_MIN_SNR_DB);
                    if (qc == CALIB_SWEEP_OK) { clean = 1; break; }
                    char why[240];
                    calib_sweep_why(&pq, qc, FS, why, sizeof why);
                    printf("  speaker %2d: re-swept, sweep %d: %s\n", s, k + 1, why);
                }
                if (!clean) {
                    fprintf(stderr, "calibrate: speaker %d: no clean sweep in %d; the run stops and writes nothing\n", s, CALIB_SWEEP_MAX_TRIES);
                    calib_asio_close(); free(cap19); free(arr); free(res); free(cap); free(sweep);
                    return 1;
                }
                for (int j = 0; j < ZYLIA_MICS; ++j) row[j] = ((double)rz[j].delay_samples + rz[j].delay_frac) / FS;
            }
#endif
            if (const int bump = track_after_capture(CAPTURE_S, "position survey", s)) {
#ifdef BWA_HAVE_ASIO
                if (asio_up) calib_asio_close();
#endif
                free(cap19); free(arr); free(res); free(cap); free(sweep);
                return bump;
            }
        }
#ifdef BWA_HAVE_ASIO
        if (asio_up) { calib_asio_close(); asio_up = 0; }
#endif
        free(cap19); cap19 = NULL;

        /* latency: --ref (one tape-measured distance) beats --latency (a loopback measurement).
         * Directions never need it; distances are c*(arrival - latency), so without either the
         * hardware distances would carry the full system latency radially. */
        double latency = (known_latency >= 0.0) ? known_latency / C : (simulate ? CAL_SIM_LATENCY_SAMPLES / FS : 0.0);
        int lat_known = simulate || (known_latency >= 0.0);    /* simulate arrivals carry its own (or exactly --latency) */
        if (ref_spk >= 0) {
            /* the ref speaker's arrival at the array CENTER (the capsules' mean arrival, corrected
             * for the wavefront's tilt across the sphere), minus its taped flight time */
            double lat_ref = zylia_center_arrival(arr + (size_t)ref_spk * ZYLIA_MICS, C) - ref_dist / C;
            if (known_latency >= 0.0)
                printf("zylia: --ref solves latency %.3f m vs --latency %.3f m (delta %+.1f cm) - using --ref\n",
                       lat_ref * C, known_latency, (lat_ref * C - known_latency) * 100.0);
            latency = lat_ref; lat_known = 1;
            printf("zylia: system latency from --ref %d @ %.3f m: %.2f ms (%.3f m at c)\n",
                   ref_spk, ref_dist, latency * 1e3, latency * C);
            if (!simulate && latency <= 0.0)
                printf("  WARNING: solved latency is not positive - wrong --ref speaker or distance?\n");
        } else if (rs_n) {
            /* several speakers whose positions are measured: each one's center arrival minus its layout
             * distance from the array center, the median used */
            refspk_plan_warnings(L, rs_spk, rs_n, "zylia");
            double carr[BWA_MAX_CHANNELS], lr = 0.0;
            for (int k = 0; k < rs_n; ++k) carr[k] = zylia_center_arrival(arr + (size_t)rs_spk[k] * ZYLIA_MICS, C);
            if (const int rc = refspk_latency(L, rs_spk, rs_n, carr, mic, g_tracked ? "the measured center" : "--mic", C,
                                              "zylia", &lr)) {
                free(arr); free(res); free(cap); free(sweep);
                return rc;
            }
            if (known_latency >= 0.0)
                printf("zylia: --ref-speakers solves %.4f m against --latency %.4f m (%+.1f mm); using --ref-speakers\n",
                       lr * C, known_latency, (lr * C - known_latency) * 1e3);
            latency = lr; lat_known = 1;
            if (!simulate && latency <= 0.0)
                printf("  WARNING: solved latency is not positive - wrong --ref-speakers, or --mic?\n");
        } else if (simulate && known_latency < 0.0)
            printf("zylia: system latency: the simulator's own %d samples\n", CAL_SIM_LATENCY_SAMPLES);
#ifdef BWA_HAVE_ASIO
        /* The same check as --localize, for the ZM-1 (calib_latency_check): the driver's digital loop
         * is inside every arrival, so a solved latency below it is physically impossible, and the
         * Dante Via leg legitimately adds tens of ms the driver does not report (a clap loopback on
         * the rig measured ~60 ms, ~20 m at c, so the residual is HUGE and still correct), up to the
         * arrival window's allowance. */
        { long il = 0, ol = 0;
          if (!simulate && lat_known && calib_asio_latencies(&il, &ol)) {
              double drv_m = sos * (double)(il + ol) / FS;
              double resid_s = 0.0;
              const int lc = calib_latency_check(1, latency, drv_m / sos, &resid_s);
              printf("zylia: system latency %.3f m vs driver digital loop %.3f m -> residual %+.2f ms\n",
                     latency * C, drv_m, resid_s * 1e3);
              if (lc == CALIB_LAT_IMPOSSIBLE)
                  printf("  WARNING: below the driver's own digital loop - physically impossible; check the\n"
                         "           device/clocking, the --ref distance, or the --latency value\n");
              else if (lc == CALIB_LAT_WARN)
                  printf("  WARNING: residual is over %.0f ms, more than the Dante Via leg the ZM-1 adds and more\n"
                         "           than the arrival window allows - check the Via latency setting, the --ref distance,\n"
                         "           or the --latency value\n", calib_latency_bound_s(1) * 1e3);
          } }
#endif

        float (*pos)[3] = (float(*)[3])malloc((size_t)n * 3 * sizeof(float));
        for (int s = 0; s < n; ++s) {
            const double* row = arr + (size_t)s * ZYLIA_MICS;
            float p[3], dir[3], dist;
            if (!zylia_localize(row, mic, latency, C, p, &dist)) { p[0]=p[1]=p[2]=0.f; dist=0.f; }
            zylia_doa(row, dir);
            pos[s][0]=p[0]; pos[s][1]=p[1]; pos[s][2]=p[2];
            printf("  spk %2d: dir=(%+.3f %+.3f %+.3f)  pos=(%+.3f %+.3f %+.3f)  dist=%.3f m%s\n",
                   s, dir[0],dir[1],dir[2], p[0],p[1],p[2], dist,
                   (s == ref_spk) ? "  [--ref: should read the taped distance]"
                                  : (std::find(rs_spk, rs_spk + rs_n, s) != rs_spk + rs_n ? "  [--ref-speakers]" : ""));
        }
        if (!lat_known) {
            fprintf(stderr, "zylia: NOT writing positions - no --latency/--ref/--ref-speakers, so every distance above carries\n"
                            "       the full system latency radially (directions are exact). Tape ONE speaker's\n"
                            "       distance from the array center and re-run with --ref <spk> <m>, or measure a\n"
                            "       loopback and pass --latency <m>.\n");
            free(arr); free(pos); free(res); free(cap); free(sweep);
            return 1;
        }
        track_report("the position survey measured");
        int nplan = 0;
        if (!calib_write_positions(layout_path, out_path, pos, n, &nplan, err, sizeof err)) { fprintf(stderr, "calibrate: %s\n", err); return 1; }
        record_sos(out_path, sos);
        printf("zylia: wrote %d positions to %s\n", n, out_path);
        report_plan("zylia", nplan, n);
        free(arr); free(pos); free(res); free(cap); free(sweep);
        return 0;
    }

    /* --- drift check: one fast pass from the mic position, flag anything nudged --- */
    if (check) {
        float (*pos)[3] = (float(*)[3])malloc((size_t)n * 3 * sizeof(float));
        for (int s = 0; s < n; ++s) { pos[s][0]=L.speakers[s].pos[0]; pos[s][1]=L.speakers[s].pos[1]; pos[s][2]=L.speakers[s].pos[2]; }
        double* range = (double*)malloc((size_t)n * sizeof(double));
        static Pass CP;                                        /* static: main is not reentrant */
        CP.c.L = &L; CP.c.sos = sos; CP.c.simulate = simulate; CP.c.zylia = 0; CP.c.sim_at = mic;
        CP.c.sweep = sweep; CP.c.cap = cap; CP.c.band_hz = NULL; CP.in_first = mic_in;
        pass_quality(CP.c, mic, CALIB_WIN_PLAN_M, 0);          /* the plan margin: it is looking for moved boxes */
        for (int s = 0; s < n; ++s) {
            MeasureResult r;
            if (!measure_speaker(CP, s, 0, &r)) {
#ifdef BWA_HAVE_ASIO
                if (asio_up) calib_asio_close();
#endif
                return 1;
            }
            range[s] = ((double)r.delay_samples + r.delay_frac) * sos / FS;
        }
#ifdef BWA_HAVE_ASIO
        if (asio_up) calib_asio_close();
#endif
        float* dev = (float*)malloc((size_t)n * sizeof(float));
        calib_check_drift(range, pos, mic, n, dev);
        const float tol = 0.02f; int flagged = 0;
        printf("drift check (mic at %.2f %.2f %.2f, tolerance %.0f mm):\n", mic[0], mic[1], mic[2], tol * 1000.f);
        for (int s = 0; s < n; ++s) {
            printf("  spk %2d: %+6.1f mm%s\n", s, dev[s] * 1000.0, fabs(dev[s]) > tol ? "   <-- MOVED" : "");
            if (fabs(dev[s]) > tol) ++flagged;
        }
        printf("drift: %d speaker(s) beyond %.0f mm\n", flagged, tol * 1000.f);
        free(pos); free(range); free(dev); free(res); free(cap); free(sweep);
        return flagged ? 3 : 0;
    }

    /* --- live: repeatedly measure one speaker's distance while you position it --- */
    if (live_speaker >= 0) {
        if (live_speaker >= n) { fprintf(stderr, "calibrate: --live %d out of range (0..%d)\n", live_speaker, n - 1); return 1; }
        float* tp = L.speakers[live_speaker].pos;
        double dx = tp[0]-mic[0], dy = tp[1]-mic[1], dz = tp[2]-mic[2];
        double target = sqrt(dx*dx + dy*dy + dz*dz);
        printf("live: speaker %d, target %.3f m from the mic (%.2f %.2f %.2f). Move it; press a key to stop.\n",
               live_speaker, target, mic[0], mic[1], mic[2]);
#ifdef BWA_HAVE_ASIO
        /* no --latency given: the driver's digital loop is a hard LOWER bound for it — a useful
         * starting value (the true system latency adds DAC/ADC + analog on top; --localize solves
         * it exactly, and prints this same comparison). */
        { long il = 0, ol = 0;
          if (known_latency < 0.0 && !simulate && calib_asio_latencies(&il, &ol))
              printf("  (driver digital loop = %.3f m of the range below - a lower bound for --latency)\n",
                     sos * (double)(il + ol) / FS); }
#endif
        int iters = sweeps > 0 ? sweeps : (simulate ? 4 : (1 << 30));
        for (int t = 0; t < iters; ++t) {
            if (simulate) calib_sim_capture(live_speaker, &L, mic, sos, sweep, cap);
#ifdef BWA_HAVE_ASIO
            else {
                if (!calib_asio_capture(live_speaker)) { fprintf(stderr, "\ncalibrate: capture timed out\n"); calib_asio_close(); return 1; }
                if (_kbhit()) { _getch(); break; }
            }
#endif
            int wlo = 0, whi = 0;
            if (g_have_win) {
                CalibWindow w = g_win;
                w.pos_m = mode_window_m(CALIB_WIN_PLAN_M);         /* the box is being moved */
                if (!calib_arrival_window(&w, tp, mic, sos, FS, &wlo, &whi)) wlo = whi = 0;
            }
            MeasureResult r; measure_response_ex(cap, CAPLEN, sweep, NSWEEP, F1, F2, FS, BAND_HZ, wlo, whi, &r, NULL, 0, 0, NULL);
            const int qc = calib_sweep_check(&r, CALIB_SWEEP_MIN_SNR_DB);
            if (qc != CALIB_SWEEP_OK) {
                char why[240];
                calib_sweep_why(&r, qc, FS, why, sizeof why);
                printf("\r  reading skipped: %s\n", why);
                continue;
            }
            double range = ((double)r.delay_samples + r.delay_frac) * sos / FS;
            if (known_latency >= 0.0)
                printf("\r  distance %.3f m   target %.3f   delta %+.1f cm        ", range - known_latency, target, (range - known_latency - target) * 100.0);
            else
                printf("\r  range %.3f m (distance + latency; pass --latency m for absolute)        ", range);
            fflush(stdout);
        }
        printf("\n");
#ifdef BWA_HAVE_ASIO
        if (asio_up) calib_asio_close();
#endif
        free(res); free(cap); free(sweep);
        return 0;
    }

    /* tracked: the measured center becomes the mic, and a body-frame table is re-aimed, BEFORE the
     * capsule positions are read into P below */
    if (g_tracked && !nrows && track_place(mic, NULL, verify ? "verify" : (rq_grid ? "room-eq-grid" : "trims"), mic, 0.f)) {
#ifdef BWA_HAVE_ASIO
        if (asio_up) calib_asio_close();
#endif
        return 1;
    }
    /* the trim loop and --verify capture through one helper (measure_speaker), omni or ZM-1 */
    static Pass P;                                             /* static: main is not reentrant */
    P.c.L = &L; P.c.sos = sos; P.c.simulate = simulate; P.c.zylia = zylia;
    P.c.sim_at = g_track_sim ? g_sim_at : mic;                 /* --track-sim: the stand's TRUE center */
    P.c.sweep = sweep; P.c.cap = cap; P.c.cap19 = cap19;
    zylia_capsules(P.c.caps);                                  /* the installed survey, else the built-in table */
    P.c.nplay = nplay;
    P.in_first = mic_in;
    P.c.play = (float*)calloc((size_t)CAPLEN, sizeof(float));
    P.c.tmp  = (float*)calloc((size_t)CAPLEN, sizeof(float));
    /* the window per speaker (surveyed or at its plan); a trim or a verify value counts only once two
     * sweeps agree. The grid rows read only the LF modal cuts, so one clean sweep does. mic is the
     * variable the rows and the placement update, so the window follows them. */
    pass_quality(P.c, mic, -1.0, nrows ? 0 : 1);
    if (!P.c.play || !P.c.tmp) { fprintf(stderr, "calibrate: out of memory\n"); return 1; }
    if (zylia) {
        printf("zylia: the ZM-1 is the %s mic, array center at (%.2f %.2f %.2f): level, bands and direct share are\n"
               "       the power mean over its %d capsules, the delay is the arrival at the center\n",
               verify ? "verify" : (nrows ? "grid" : "trim"), mic[0], mic[1], mic[2], ZYLIA_MICS);
        if (!survey_path)
            printf("zylia: no --survey: the built-in capsule table. The power mean does not care about channel\n"
                   "       order; the center arrival's tilt correction does, by at most about 15 us.\n");
        if (room || ir_prefix || rq_grid)
            printf("zylia: --room/--save-irs/--room-eq-grid read the MEAN of the 19 capsule captures: the center\n"
                   "       pressure below about 2 kHz, a direction-dependent beam above it. --room-eq-grid only uses\n"
                   "       30 to 200 Hz; the RT60, the reflection levels and saved IRs above 2 kHz carry the sphere.\n");
    }

    /* --- --room-eq-grid rows.txt: one placement per row, ONE grid written after the last --- */
    if (nrows) {
        static float keys[BWA_RQ_GRID_MAX][3];                 /* every row's grid key */
        MeasureEqSection* rcuts = (MeasureEqSection*)calloc((size_t)nrows * n * BWA_ROOM_EQ_MAX, sizeof(MeasureEqSection));
        int* rcounts = (int*)calloc((size_t)nrows * n, sizeof(int));
        if (!rcuts || !rcounts) { fprintf(stderr, "calibrate: out of memory\n"); return 1; }
        int rc = 0;
        for (int k = 0; k < nrows && !rc; ++k) {
            char what[64]; snprintf(what, sizeof what, "room-eq-grid row %d/%d", k + 1, nrows);
            if (g_tracked) {
                /* the row is the PLAN; the key is where the stand measures, as --localize records it */
                if (track_place(rq_rows[k], "the grid key", what, mic, 0.f)) { rc = 1; break; }
            } else {
                memcpy(mic, rq_rows[k], sizeof mic);
                if (!simulate) { printf("  -> place the mic at (%.2f %.2f %.2f) and press Enter...", mic[0], mic[1], mic[2]); fflush(stdout); getchar(); }
            }
            memcpy(keys[k], mic, sizeof keys[k]);              /* P.c.sim_at points at mic (or g_sim_at) */
            printf("%s: grid key (%.4f %.4f %.4f)%s\n", what, mic[0], mic[1], mic[2],
                   g_tracked ? ", the measured center" : ", the row");
            for (int i = 0; i < n && !rc; ++i) {
                CalibMeasInfo gmi;
                if (!measure_speaker(P, i, 0, &res[i], &gmi)) { rc = 1; break; }
                if ((rc = track_after_capture(CAPTURE_S * gmi.sweeps, what, i)) != 0) break;
                RoomResult rr; static float irbuf[IR_LEN];
                measure_room(cap, CAPLEN, sweep, NSWEEP, F1, F2, FS, &rr, irbuf, IR_LEN);
                MeasureEqSection* c = &rcuts[((size_t)k * n + i) * BWA_ROOM_EQ_MAX];
                const int nc = measure_room_cuts(irbuf, IR_LEN, 0, FS, 30.0, 200.0, 12.0, BWA_ROOM_EQ_MAX, c);
                rcounts[(size_t)k * n + i] = nc < 0 ? 0 : nc;
                printf("  speaker %2d: %d LF modal cut(s)", i, rcounts[(size_t)k * n + i]);
                for (int t = 0; t < rcounts[(size_t)k * n + i]; ++t) printf("  [%.0f Hz %.1f dB Q%.1f]", c[t].fc, c[t].gain_db, c[t].q);
                printf("\n");
            }
            if (!rc) track_report(what);
        }
        sweep_report("room-eq-grid");
#ifdef BWA_HAVE_ASIO
        if (asio_up) { calib_asio_close(); asio_up = 0; }
#endif
        if (!rc) {
            if (!calib_write_room_eq_grid_n(layout_path, out_path, nrows, (const float (*)[3])keys, rcuts, rcounts, n,
                                            BWA_ROOM_EQ_MAX, err, sizeof err)) {
                fprintf(stderr, "calibrate: room-eq-grid writeback: %s\n", err); rc = 1;
            } else {
                printf("calibrate: merged %d position(s) into room_eq_grid in %s:\n", nrows, out_path);
                for (int k = 0; k < nrows; ++k)
                    printf("  row %2d: key (%.3f %.3f %.3f), planned (%.3f %.3f %.3f)\n", k + 1, keys[k][0], keys[k][1], keys[k][2],
                           rq_rows[k][0], rq_rows[k][1], rq_rows[k][2]);
            }
        } else
            fprintf(stderr, "calibrate: the room-eq-grid run stopped at a row; nothing was written\n");
        free(rcuts); free(rcounts);
        free(P.c.play); free(P.c.tmp); free(cap19); free(res); free(cap); free(sweep);
        return rc;
    }

    /* --- the second pass: every speaker THROUGH the layout's output stage, residuals, nothing written --- */
    if (verify) {
        printf("verify: sweeping each speaker through the layout's own output stage (gain_db, delay_ms, eq,\n"
               "        room_eq; %s), deconvolved against the raw sweep\n",
               simulate ? "simulate stages the raw capture, the same thing for a linear stage"
                        : "the staged sweep is what plays");
        if (L.rq_grid.npos)
            printf("verify: room_eq_grid plays at its flat start: the engine interpolates it from the tracked\n"
                   "        listener, so this pass does not check it\n");
        if (fabsf(mic[0] - L.ref[0]) > 0.05f || fabsf(mic[1] - L.ref[1]) > 0.05f || fabsf(mic[2] - L.ref[2]) > 0.05f)
            printf("verify: the mic is not at the listening point (%.2f %.2f %.2f). The listener-tracked stages\n"
                   "        (tracked alignment, directivity comp) are identity only there, so this checks the STATIC\n"
                   "        stage: what a listener at the mic would hear before the engine tracks them.\n",
                   L.ref[0], L.ref[1], L.ref[2]);
        for (int i = 0; i < n; ++i) {
            CalibMeasInfo vmi;
            if (!measure_speaker(P, i, 1, &res[i], &vmi)) {
#ifdef BWA_HAVE_ASIO
                if (asio_up) calib_asio_close();
#endif
                fprintf(stderr, "calibrate: verify capture failed on speaker %d\n", i); return 1;
            }
            if (const int bump = track_after_capture(CAPTURE_S * vmi.sweeps, "verify", i)) {
#ifdef BWA_HAVE_ASIO
                if (asio_up) calib_asio_close();
#endif
                return bump;
            }
            if (zylia) printf("  speaker %2d: delay=%9.2f  level=%.4f  capsule level spread %.1f dB\n",
                              i, res[i].delay_samples + res[i].delay_frac, res[i].level, P.capsule_spread_db);
            else       printf("  speaker %2d: delay=%9.2f  level=%.4f\n", i, res[i].delay_samples + res[i].delay_frac, res[i].level);
        }
#ifdef BWA_HAVE_ASIO
        if (asio_up) { calib_asio_close(); asio_up = 0; }
#endif
        float (*vpos)[3] = (float(*)[3])malloc((size_t)n * 3 * sizeof(float));
        float* corr = (float*)calloc((size_t)n, sizeof(float));
        float* aus  = (float*)calloc((size_t)n, sizeof(float));
        float* ldb  = (float*)calloc((size_t)n, sizeof(float));
        int*   flg  = (int*)calloc((size_t)n, sizeof(int));
        if (!vpos || !corr || !aus || !ldb || !flg) { fprintf(stderr, "calibrate: out of memory\n"); return 1; }
        for (int i = 0; i < n; ++i) memcpy(vpos[i], L.speakers[i].pos, sizeof vpos[i]);
        if (want_corr) {
            calib_directivity_corr(&L, mic, 2.0 * F1, 0.5 * F2, res, corr);   /* the trim run's own factor */
            print_directivity(L, mic, res, corr, n);
        }
        CalibVerifySummary vs;
        /* align_pt = the mic: verify runs from the trim run's placement, where the trims made every
         * arrival equal (calib.h) */
        calib_verify_residuals(res, vpos, mic, mic, n, FS, sos, want_corr ? corr : NULL, aus, ldb, flg, &vs);
        sweep_report("verify");
        printf("verify: residuals, median removed (arrival: measured minus the aligned arrival; level: x distance%s)\n",
               want_corr ? " x the directivity re-aim" : "");
        printf("        spk  arrival(us)  level(dB)\n");
        for (int i = 0; i < n; ++i) {
            if (flg[i] & CALIB_VERIFY_FLAG_DEAD) { printf("        %3d        -          -      <- DEAD (no signal)\n", i); continue; }
            printf("        %3d  %+9.1f   %+7.2f%s%s\n", i, aus[i], ldb[i],
                   (flg[i] & CALIB_VERIFY_FLAG_ARRIVAL) ? "   <- ARRIVAL" : "",
                   (flg[i] & CALIB_VERIFY_FLAG_LEVEL)   ? "   <- LEVEL"   : "");
        }
        printf("verify: arrival spread %.1f us (flag beyond +/-%.0f us), level spread %.2f dB (flag beyond +/-%.1f dB)\n",
               vs.arrival_spread_us, CALIB_VERIFY_ARRIVAL_US, vs.level_spread_db, CALIB_VERIFY_LEVEL_DB);
        printf("verify: residuals against the mic at (%.4f %.4f %.4f)%s\n", mic[0], mic[1], mic[2],
               g_tracked ? ", the measured center" : "");
        track_report("verify measured");
        printf("verify: %d speaker(s) flagged. Nothing is written.\n", vs.nflag);
        free(vpos); free(corr); free(aus); free(ldb); free(flg);
        free(P.c.play); free(P.c.tmp); free(cap19); free(res); free(cap); free(sweep);
        return vs.nflag ? 3 : 0;
    }

    const int NTAPS = 256;                                     /* correction-FIR length (<= BWA_EQ_TAPS) */
    float*    eq_taps = NULL; uint16_t* eq_lens = NULL;
    MeasureEqSection* rq_cuts = NULL; int* rq_counts = NULL;   /* --room-eq: LF modal cuts per speaker */
    if (eq) { eq_taps = (float*)calloc((size_t)n * BWA_EQ_TAPS, sizeof(float));
              eq_lens = (uint16_t*)calloc((size_t)n, sizeof(uint16_t)); }
    if (room_eq || rq_grid) { rq_cuts   = (MeasureEqSection*)calloc((size_t)n * BWA_ROOM_EQ_MAX, sizeof(MeasureEqSection));
                              rq_counts = (int*)calloc((size_t)n, sizeof(int)); }
    for (int i = 0; i < n; ++i) {
        CalibMeasInfo tmi;
        if (!measure_speaker(P, i, 0, &res[i], &tmi)) {
#ifdef BWA_HAVE_ASIO
            if (asio_up) calib_asio_close();
#endif
            return 1;
        }
        if (const int bump = track_after_capture(CAPTURE_S * tmi.sweeps, rq_grid ? "room-eq-grid run" : "trim run", i)) {
#ifdef BWA_HAVE_ASIO
            if (asio_up) calib_asio_close();
#endif
            return bump;
        }
        printf("  speaker %2d: delay=%6d  level=%.4f  bands=[%.3f %.3f %.3f]",
               i, res[i].delay_samples, res[i].level, res[i].band[0], res[i].band[1], res[i].band[2]);
        if (zylia) printf("  capsule level spread %.1f dB", P.capsule_spread_db);
        printf("  snr=%.1f dB  sweeps=%d\n", res[i].snr_db, tmi.sweeps);
        if (room || ir_prefix || eq || rq_grid) {              /* room report + retained IR kernels + EQ */
            RoomResult rr; static float irbuf[IR_LEN];
            int want_ir = (ir_prefix || eq || rq_grid);
            measure_room(cap, CAPLEN, sweep, NSWEEP, F1, F2, FS, &rr, want_ir ? irbuf : NULL, want_ir ? IR_LEN : 0);
            if (room) {
                printf("            RT60=%.3f s  early reflections:", rr.rt60);
                for (int e = 0; e < rr.er_count; ++e) printf(" %.1fms/%.2f", rr.er_delay[e] * 1000.0 / FS, rr.er_level[e]);
                printf("%s\n", rr.er_count ? "" : " (none)");
                if (rr.rt60 > 0.f) { rt60_sum += rr.rt60; ++rt60_n; }
            }
            if (ir_prefix) { char p[512]; snprintf(p, sizeof p, "%s_%02d.wav", ir_prefix, i); calib_write_wav_f32(p, irbuf, IR_LEN, (int)FS); }
            if (room_eq) {   /* room correction at the mic point: FD-window FIR + LF modal cuts */
                int first_refl = rr.er_count ? rr.er_delay[0] : 0;
                int nc = calib_room_eq(irbuf, IR_LEN, first_refl, FS, NTAPS,
                                       &eq_taps[(size_t)i * BWA_EQ_TAPS],
                                       &rq_cuts[(size_t)i * BWA_ROOM_EQ_MAX], BWA_ROOM_EQ_MAX);
                if (nc >= 0) { eq_lens[i] = (uint16_t)NTAPS; rq_counts[i] = nc; }
                printf("            room-eq: %d-tap FIR (200 Hz up), %d LF modal cut(s)", eq_lens[i], nc < 0 ? 0 : nc);
                for (int s = 0; s < rq_counts[i]; ++s)
                    printf("  [%.0f Hz %.1f dB Q%.1f]", rq_cuts[(size_t)i*BWA_ROOM_EQ_MAX+s].fc,
                           rq_cuts[(size_t)i*BWA_ROOM_EQ_MAX+s].gain_db, rq_cuts[(size_t)i*BWA_ROOM_EQ_MAX+s].q);
                printf("\n");
            } else if (eq) {   /* gate to before the first reflection -> invert the speaker's direct response */
                int first_refl = rr.er_count ? rr.er_delay[0] : 0;
                if (calib_eq(irbuf, IR_LEN, first_refl, FS, NTAPS, &eq_taps[(size_t)i * BWA_EQ_TAPS]))
                    eq_lens[i] = (uint16_t)NTAPS;
                printf("            eq: %d-tap correction (gate %s)\n", eq_lens[i],
                       first_refl ? "to first reflection" : "default 4 ms");
            }
            if (rq_grid) {     /* tracked room EQ: this position's LF modal cuts (same 30-200 Hz band +
                                * 12 dB depth cap as --room-eq's cut half; the merge across positions
                                * happens in the writeback) */
                int nc = measure_room_cuts(irbuf, IR_LEN, 0, FS, 30.0, 200.0, 12.0,
                                           BWA_ROOM_EQ_MAX, &rq_cuts[(size_t)i * BWA_ROOM_EQ_MAX]);
                rq_counts[i] = nc < 0 ? 0 : nc;
                printf("            room-eq-grid: %d LF modal cut(s) at this position", rq_counts[i]);
                for (int s = 0; s < rq_counts[i]; ++s)
                    printf("  [%.0f Hz %.1f dB Q%.1f]", rq_cuts[(size_t)i*BWA_ROOM_EQ_MAX+s].fc,
                           rq_cuts[(size_t)i*BWA_ROOM_EQ_MAX+s].gain_db, rq_cuts[(size_t)i*BWA_ROOM_EQ_MAX+s].q);
                printf("\n");
            }
        }
    }
    if (room && rt60_n) printf("room: mean RT60 ~ %.3f s (the floor on renderable reverb; treat the room to lower it)\n", rt60_sum / rt60_n);
    sweep_report("trims");
#ifdef BWA_HAVE_ASIO
    if (asio_up) calib_asio_close();
#endif

    float* gdb = (float*)malloc((size_t)n * sizeof(float));
    float* dms = (float*)malloc((size_t)n * sizeof(float));
    /* pack positions contiguously: the Speaker struct has gain/delay between pos[] entries, so it is
     * not a float[n][3] — calib_solve needs a packed [3]-stride array. */
    float (*pos)[3] = (float(*)[3])malloc((size_t)n * 3 * sizeof(float));
    for (int i = 0; i < n; ++i) { pos[i][0]=L.speakers[i].pos[0]; pos[i][1]=L.speakers[i].pos[1]; pos[i][2]=L.speakers[i].pos[2]; }
    float* corr = NULL;
    if (want_corr) {
        corr = (float*)calloc((size_t)n, sizeof(float));
        calib_directivity_corr(&L, mic, 2.0 * F1, 0.5 * F2, res, corr);   /* the one implementation (calib.c) */
        print_directivity(L, mic, res, corr, n);
    }
    calib_solve_corr(res, pos, mic, n, FS, corr, gdb, dms);   /* corr NULL = the plain solve */
    free(pos); free(corr);

    /* report the spread + write back */
    float gmin=1e9f, gmax=-1e9f, dmax=0.f;
    for (int i = 0; i < n; ++i) { if (gdb[i]<gmin) gmin=gdb[i]; if (gdb[i]>gmax) gmax=gdb[i]; if (dms[i]>dmax) dmax=dms[i]; }
    printf("trims: gain_db in [%.2f, %.2f]  max delay %.3f ms\n", gmin, gmax, dmax);
    for (int i = 0; i < n; ++i) printf("  spk %2d: gain_db=%+.2f  delay_ms=%.3f\n", i, gdb[i], dms[i]);
    printf("trims: arrivals aligned at the mic (%.4f %.4f %.4f)%s\n", mic[0], mic[1], mic[2],
           g_tracked ? ", the measured center" : "");
    track_report(rq_grid ? "room-eq-grid measured" : "trims measured");

    {   /* The delays equalize arrival AT THE MIC (calib_solve), but the engine treats the trims as
         * aligned at Layout.ref: tracked alignment re-references them from ref onto the listener and
         * is identity only there. The gains are re-aimed to ref (the directivity correction); the
         * delays are not, since that would trust the surveyed geometry over the measurement. So the
         * one consistent placement is the mic AT the listening point: say so when it is not. */
        const float ex = mic[0] - L.ref[0], ey = mic[1] - L.ref[1], ez = mic[2] - L.ref[2];
        const float off = sqrtf(ex * ex + ey * ey + ez * ez);
        if (off > 0.05f)
            printf("trims: WARNING the mic is %.2f m from the layout's listening point (%.3f %.3f %.3f).\n"
                   "       delay_ms aligns arrivals at the MIC; the engine assumes they align at the\n"
                   "       listening point (tracked alignment is identity only there). Re-run with the\n"
                   "       mic%s at the listening point for trims that match the engine.\n",
                   off, L.ref[0], L.ref[1], L.ref[2], zylia ? " (the ZM-1's center)" : "");
    }

    if (!calib_write_layout(layout_path, out_path, gdb, dms, n, err, sizeof err)) {
        fprintf(stderr, "calibrate: %s\n", err); return 1;
    }
    record_sos(out_path, sos);
    printf("calibrate: wrote %s\n", out_path);

    if (eq) {   /* write the correction filters into the file the trims just wrote */
        if (!calib_write_eq(out_path, out_path, eq_taps, eq_lens, n, BWA_EQ_TAPS, err, sizeof err))
            fprintf(stderr, "calibrate: eq writeback: %s\n", err);
        else printf("calibrate: wrote per-speaker correction filters to %s\n", out_path);
        free(eq_taps); free(eq_lens);
    }
    if (room_eq) {   /* the LF modal cuts ride the same file (align.c renders them as biquads) */
        if (!calib_write_room_eq(out_path, out_path, rq_cuts, rq_counts, n, BWA_ROOM_EQ_MAX, err, sizeof err))
            fprintf(stderr, "calibrate: room-eq writeback: %s\n", err);
        else printf("calibrate: wrote LF modal cuts (room_eq) to %s\n", out_path);
    }
    if (rq_grid) {   /* merge this mic position into room_eq_grid (replace-within-5cm or append) */
        if (!calib_write_room_eq_grid(out_path, out_path, mic, rq_cuts, rq_counts, n, BWA_ROOM_EQ_MAX, err, sizeof err))
            fprintf(stderr, "calibrate: room-eq-grid writeback: %s\n", err);
        else printf("calibrate: merged position (%.2f %.2f %.2f) into room_eq_grid in %s\n",
                    mic[0], mic[1], mic[2], out_path);
    }
    if (room_eq || rq_grid) { free(rq_cuts); free(rq_counts); }

    free(gdb); free(dms); free(res); free(cap); free(sweep); free(cap19); free(P.c.play); free(P.c.tmp);
    return 0;
}
