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
 *   calibrate --zylia --trims --input 26 ... # the ZM-1 as the trim mic (bare --zylia = the position survey)
 *   calibrate --live 7 --zylia --latency 20.6 # live aiming: one speaker's position and off-axis angle
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

/* ---- --track: the ZM-1's stand as a tracked rigid body (docs/calibration.md, "Placing the ZM-1 with
 * the tracker") ----
 * Before the captures of a placement, wait until the measured center sits within --place-tol-mm of the
 * target and has stayed still (placement.h), then take THAT as the mic position. After each capture,
 * the bump check: a run measured across a moved mic is wrong, so it stops. With --track-sim the
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

static const double CAPTURE_S = CAL_CAPLEN / CAL_FS, LIVE_CAPTURE_S = CAL_LIVE_CAPLEN / CAL_FS;

static void track_sim_sync(void) { if (g_track_sim) mic_track_sim_truth(&g_mt, g_sim_at); }

/* --localize's gate: trilateration needs each position KNOWN, not hit, so the row is only roughly
 * enforced. But not "still anywhere": the stand is still at the PREVIOUS row the moment the gate
 * restarts, and a stillness-only gate opened there after its 1.5 s and recorded that row twice. Rows
 * worth trilaterating from sit far more than this apart. */
#define LOCALIZE_TOL_M 0.10f

/* Wait for the placement and take the measured center into mic_out. accept_still: --localize, where
 * the tolerance is loose (LOCALIZE_TOL_M, or --place-tol-mm if wider). Returns 0, or 1 (timed out). */
static int track_place(const float target[3], int accept_still, const char* what, float mic_out[3]) {
    PlaceCfg cfg;
    place_cfg_default(&cfg, accept_still ? (g_tol_m > LOCALIZE_TOL_M ? g_tol_m : LOCALIZE_TOL_M) : g_tol_m);
    if (mic_track_place_console(&g_mt, target, &cfg, g_place_timeout, !g_track_sim, what, &g_placed)) return 1;
    memcpy(mic_out, g_placed.center, sizeof g_placed.center);
    memcpy(g_taken, g_placed.center, sizeof g_taken);
    if (accept_still && g_placed.dist_m > g_tol_m)
        printf("placement: %.1f mm from the planned position (the %.0f mm guide): trilateration needs the position\n"
               "           KNOWN, not hit, so the measured one is recorded\n", g_placed.dist_m * 1e3, g_tol_m * 1e3);
    g_max_move = 0.f; g_nchecks = 0; g_nunchecked = 0;
    track_sim_sync();
    return 0;
}

/* The bump check after one capture. Returns 0 (in place, or no pose to judge by), 4 (bumped: the
 * message is printed, and the caller stops without writing anything). */
static int track_after_capture(double capture_s, const char* what, int idx) {
    if (!g_tracked) return 0;
    float moved = 0.f, now[3] = { 0.f, 0.f, 0.f };
    const int b = mic_track_bump_console(&g_mt, g_taken, g_tol_m, capture_s, &moved, now);
    if (b < 0) { ++g_nunchecked; return 0; }
    ++g_nchecks;
    if (moved > g_max_move) g_max_move = moved;
    track_sim_sync();
    if (b == 0) return 0;
    fprintf(stderr, "calibrate: BUMP: the ZM-1 moved %.1f mm during the %s, after %s %d (limit %.1f mm, half the\n"
                    "           tolerance). Center taken (%.4f %.4f %.4f), now (%.4f %.4f %.4f). What was measured\n"
                    "           across a moved mic is wrong, so the run stops and writes nothing: re-place the ZM-1\n"
                    "           and run again.\n",
            moved * 1e3, what, !strcmp(what, "live aiming") ? "reading" : "speaker", idx, 0.5f * g_tol_m * 1e3f,
            g_taken[0], g_taken[1], g_taken[2], now[0], now[1], now[2]);
    return 4;
}

static void track_report(const char* what) {
    if (!g_tracked) return;
    printf("placement: %s at the measured center (%.4f %.4f %.4f), mount yaw %.1f deg, tilt %.1f deg;\n"
           "           %d bump check(s), the largest move %.1f mm (limit %.1f mm); %d capture(s) had no live pose to check\n",
           what, g_taken[0], g_taken[1], g_taken[2], g_placed.yaw_deg, g_placed.tilt_deg, g_nchecks,
           g_max_move * 1e3, 0.5f * g_tol_m * 1e3f, g_nunchecked);
}

/* One speaker's capture and measurement, shared by the trim loop and --verify so the two passes
 * cannot measure differently. The mic is the omni (one capture, `cap`) or, with --zylia, the ZM-1:
 * 19 capsule captures in `cap19`, each deconvolved on its own and pooled by zylia_pressure_proxy,
 * and `cap` then receives their MEAN, which is the center pressure below about 2 kHz and serves the
 * IR consumers that only read low frequencies (--room-eq-grid) or that the docs caveat (--room,
 * --save-irs). `through` = play through the layout's output stage (--verify): on the rig the sweep
 * itself is staged and played (calib_asio_capture_signal); in simulate the RAW capture is staged
 * instead, which is the same thing because the stage is linear and time-invariant and the
 * simulator synthesizes the sweep analytically rather than playing a buffer. Either way the
 * measurement deconvolves against the RAW sweep, so it carries the trims. */
struct Pass {
    const Layout* L;
    const float*  mic;
    const float*  sim_at;              /* --track-sim: synthesize here (the true center), not at mic */
    double        sos;
    int           simulate, zylia;
    const float*  sweep;
    float*        cap;                 /* CAL_CAPLEN */
    float*        cap19;               /* ZYLIA_MICS x CAL_CAPLEN, --zylia only */
    float         caps[ZYLIA_MICS][3]; /* capsule positions relative to the center */
    float*        play;                /* --verify on the rig: the staged sweep */
    int           nplay;
    float*        tmp;                 /* CAL_CAPLEN scratch */
    float         capsule_spread_db;   /* --zylia: max/min capsule level of the last speaker */
    int           in_first;            /* --input: the first capsule's input, for messages */
};

static int measure_speaker(Pass& P, int s, int through, MeasureResult* out) {
    const int nrow = P.zylia ? ZYLIA_MICS : 1;
    float* rows = P.zylia ? P.cap19 : P.cap;
    if (P.simulate) {
        const float* at = P.sim_at ? P.sim_at : P.mic;
        if (P.zylia) calib_sim_capture_zylia(s, P.L, at, P.caps, ZYLIA_MICS, P.sos, P.sweep, P.cap19);
        else         calib_sim_capture(s, P.L, at, P.sos, P.sweep, P.cap);
        if (through && !calib_stage_rows(P.L, s, rows, nrow, CAPLEN, P.tmp)) return 0;
    }
#ifdef BWA_HAVE_ASIO
    else {
        printf("  speaker %2d: playing sweep%s...\n", s, through ? " through the output stage" : ""); fflush(stdout);
        int ok;
        if (through) {
            if (!calib_stage_signal(P.L, s, P.sweep, NSWEEP, P.play, P.nplay)) return 0;
            ok = calib_asio_capture_signal(s, P.play, P.nplay);
        } else ok = calib_asio_capture(s);
        if (!ok) { fprintf(stderr, "calibrate: capture timed out on speaker %d\n", s); return 0; }
    }
#endif
    if (!P.zylia) return measure_response(P.cap, CAPLEN, P.sweep, NSWEEP, F1, F2, FS, BAND_HZ, out);
    MeasureResult rj[ZYLIA_MICS];
    int okj[ZYLIA_MICS] = { 0 };
    calib_measure_zylia_rows(P.cap19, CAPLEN, CAPLEN, P.sweep, NSWEEP, BAND_HZ, rj, okj);   /* 19 deconvolutions, threaded,
                                                                                            * arrivals refined by cross-correlation */
    float lmin = 1e30f, lmax = 0.f;
    for (int j = 0; j < ZYLIA_MICS; ++j) {
        if (!okj[j]) return 0;
        if (rj[j].level < lmin) lmin = rj[j].level;
        if (rj[j].level > lmax) lmax = rj[j].level;
    }
    P.capsule_spread_db = (lmin > 0.f) ? (float)(20.0 * log10((double)lmax / (double)lmin)) : 99.f;
    for (int i = 0; i < CAPLEN; ++i) {
        double acc = 0.0;
        for (int j = 0; j < ZYLIA_MICS; ++j) acc += P.cap19[(size_t)j * CAPLEN + i];
        P.cap[i] = (float)(acc / ZYLIA_MICS);
    }
    int dead = -1;
    const int pr = zylia_pressure_proxy(rj, FS, P.sos, out, &dead);
    if (pr < 0) {
        double lv[ZYLIA_MICS];
        for (int j = 0; j < ZYLIA_MICS; ++j) {             /* sorted copy for the median (insertion sort) */
            double v = std::isfinite(rj[j].level) ? rj[j].level : 0.0; int b = j;
            while (b > 0 && lv[b-1] > v) { lv[b] = lv[b-1]; --b; } lv[b] = v;
        }
        fprintf(stderr, "calibrate: speaker %d: ZM-1 capsule %d (input %d) is dead: its level is non-finite or more than\n"
                        "           %.0f dB under the capsules' median (%.3g against %.3g). Its arrival would be the\n"
                        "           peak of noise and throw the center arrival off by milliseconds, so the run\n"
                        "           stops. Check the routing and the capsule with bwa_zylia_probe.\n",
                s, dead, P.in_first + dead, ZYLIA_PROXY_DEAD_DB, dead >= 0 ? (double)rj[dead].level : 0.0, lv[ZYLIA_MICS / 2]);
        return 0;
    }
    return pr;
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

/* ---- --live N --zylia: live aiming (docs/calibration.md, "Live aiming") ----
 * Sweep ONE speaker over and over with the short live sweep, capture the 19 capsules, and print one
 * line per sweep: where the box is against the layout (zylia_live_position), and how far its axis
 * is off the direction to the mic (the direct-sound tilt, as a peak meter and as an estimated
 * MAGNITUDE; one mic position never says which way the box points). Keys on the rig: r stores this
 * reading as the on-axis reference, p resets the peak, any other key stops. */
struct LiveArgs {
    const Layout* L;
    int     spk;
    float   mic[3];
    double  sos;
    int     simulate;
    double  known_latency_m;    /* --latency, < 0 = none */
    int     ref_spk; double ref_dist;   /* --ref (latency from a taped distance), ref_spk < 0 = none */
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

static int live_sweep_one(const LiveArgs& A, int spk, float aim_err_deg, const float* lsweep, CalibLiveReading* r) {
    float tp[3], ta[3];
    CalibSimOpts o; memset(&o, 0, sizeof o);
    o.on_axis = 1; o.screen_db = A.sim_screen_db;
    if (A.simulate && spk == A.spk) {                    /* only the live speaker is being moved and turned */
        for (int a = 0; a < 3; ++a) tp[a] = A.L->speakers[spk].pos[a] + A.sim_move[a];
        live_true_aim(A.L, spk, aim_err_deg, ta);
        o.true_pos = tp; o.true_aim = ta;
    }
    if (!calib_live_read(spk, A.L, A.sim_at ? A.sim_at : A.mic, A.sos, A.simulate, &o, lsweep, A.cap19, r)) {
        fprintf(stderr, "\ncalibrate: capture timed out on speaker %d\n", spk); return 0;
    }
    if (!r->ok)
        printf("  speaker %d: ZM-1 capsule %d (input %d) is dead or silent; reading skipped (bwa_zylia_probe)\n",
               spk, r->dead, A.in_first + r->dead);
    return 1;
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
    const float* tp = L.speakers[s].pos;
    const float layout_deg = layout_speaker_off_axis_deg(&L, (uint32_t)s, A.mic);
    const double dx = tp[0] - A.mic[0], dy = tp[1] - A.mic[1], dz = tp[2] - A.mic[2];

    printf("live: speaker %d, ZM-1 at (%.3f %.3f %.3f), layout position (%.3f %.3f %.3f), %.3f m away%s\n",
           s, A.mic[0], A.mic[1], A.mic[2], tp[0], tp[1], tp[2], sqrt(dx * dx + dy * dy + dz * dz),
           A.simulate ? "  [SIMULATE]" : "");
    printf("live: %.2f s sweep + %.2f s tail per reading; tilt = direct-sound %.0f Hz up against %.0f-%.0f Hz\n",
           CAL_LIVE_NSWEEP / FS, CAL_LIVE_NTAIL / FS, LIVE_BAND_HZ[1], LIVE_BAND_HZ[0], LIVE_BAND_HZ[1]);
    printf("live: the layout's aim is %.1f deg off the mic. The measured angle is a MAGNITUDE: one mic position\n"
           "      cannot say which way the box points. Turn the box until the tilt peaks.\n", layout_deg);
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
    CalibLiveReading rd;
    if (A.ref_spk >= 0) {
        if (!live_sweep_one(A, A.ref_spk, 0.f, lsweep, &rd)) return 1;
        if (!rd.ok) { fprintf(stderr, "calibrate: --ref speaker %d gave no reading\n", A.ref_spk); return 1; }
        latency = zylia_center_arrival(rd.arr, C) - A.ref_dist / C; lat_known = 1;
        printf("live: system latency from --ref %d @ %.3f m: %.3f ms (%.3f m at c)%s\n", A.ref_spk, A.ref_dist,
               latency * 1e3, latency * C, A.ref_spk == s ? "; the live speaker's distance now reads the tape" : "");
    } else if (A.known_latency_m >= 0.0)
        printf("live: system latency from --latency: %.3f ms (%.3f m at c)\n", latency * 1e3, A.known_latency_m);
    else if (A.simulate)
        printf("live: system latency: the simulator's own %d samples\n", CAL_SIM_LATENCY_SAMPLES);
    else
        printf("live: no --latency or --ref: direction only, no distance or position (--ref <spk> <m> needs one tape)\n");

    int have_ref = 0; float tilt_ref = 0.f;
    if (A.have_aim_ref_db) { have_ref = 1; tilt_ref = A.aim_ref_db; printf("live: on-axis reference %+.2f dB (--aim-ref-db)\n", tilt_ref); }
    if (A.aim_ref_spk >= 0) {
        if (!live_sweep_one(A, A.aim_ref_spk, 0.f, lsweep, &rd)) return 1;
        if (!rd.ok || !rd.have_tilt) { fprintf(stderr, "calibrate: --aim-ref speaker %d gave no tilt\n", A.aim_ref_spk); return 1; }
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
            float ta[3]; live_true_aim(&L, s, aim_err, ta);
            const float tq[3] = { tp[0] + A.sim_move[0], tp[1] + A.sim_move[1], tp[2] + A.sim_move[2] };
            true_deg = directivity_off_axis_deg(tq, ta, A.mic);
        }
        if (!live_sweep_one(A, s, aim_err, lsweep, &rd)) return 1;
        ++done;
        const int bump = track_after_capture(LIVE_CAPTURE_S, "live aiming", t + 1);
        if (bump) return bump;
        if (!rd.ok) continue;
        ZyliaLivePos lp;
        memset(&lp, 0, sizeof lp);
        if (A.pos_ok) zylia_live_position(rd.arr, A.mic, lat_known, latency, C, tp, &lp);
        char line[640]; size_t k = 0;
        k += snprintf(line + k, sizeof line - k, "  #%-3d", t + 1);
        if (lp.ok && lp.have_distance)
            k += snprintf(line + k, sizeof line - k, " pos %+6.1f %+6.1f %+6.1f mm (|d| %5.1f)  dir %.2f deg  dist %.3f m (%+.1f mm)",
                          lp.delta_mm[0], lp.delta_mm[1], lp.delta_mm[2], lp.delta_norm_mm, lp.dir_err_deg, lp.dist_m, lp.dist_err_mm);
        else if (lp.ok)
            k += snprintf(line + k, sizeof line - k, " dir %.2f deg off the layout (no distance)", lp.dir_err_deg);
        else if (!A.pos_ok)
            k += snprintf(line + k, sizeof line - k, " position: n/a (no body-frame survey)");
        else
            k += snprintf(line + k, sizeof line - k, " position: DOA solve failed");
        if (rd.have_tilt) {
            const float below = calib_peak_update(&pk, rd.tilt_db);
            if (pk.peak_index == pk.n) { npk_at = t + 1; peak_true_deg = true_deg; }
            k += snprintf(line + k, sizeof line - k, " | tilt %+.2f dB  peak %+.2f  below %.2f dB", rd.tilt_db, pk.peak_db, below);
            char ba[64] = "", bb[64] = "";
            CalibAimAngle ar, af;
            if (have_model && have_ref)  { calib_aim_invert(curve, rd.tilt_db - tilt_ref, CALIB_LIVE_TILT_TOL_DB, &ar); fmt_aim(ar, 1, ba, sizeof ba); }
            if (have_model && have_file) { calib_aim_invert(curve, rd.tilt_db - tilt0_file, CALIB_LIVE_TILT_TOL_DB, &af); fmt_aim(af, 1, bb, sizeof bb); }
            if (ba[0] || bb[0])
                k += snprintf(line + k, sizeof line - k, " | off-axis%s%s%s%s%s", ba[0] ? " ref " : "", ba,
                              ba[0] && bb[0] ? "," : "", bb[0] ? " file " : "", bb);
        } else
            k += snprintf(line + k, sizeof line - k, " | tilt: no direct-sound level in a band");
        k += snprintf(line + k, sizeof line - k, " | layout %.1f deg", layout_deg);
        if (A.simulate) k += snprintf(line + k, sizeof line - k, " | true %.1f deg", true_deg);
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

/* read mic positions ("x y z" per line) for --localize; returns the count (<= maxK) */
static int read_positions(const char* path, float (*out)[3], int maxK) {
    FILE* f = fopen(path, "r"); if (!f) return 0;
    int k = 0;
    while (k < maxK && fscanf(f, "%f %f %f", &out[k][0], &out[k][1], &out[k][2]) == 3) ++k;
    fclose(f); return k;
}

int main(int argc, char** argv) {
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
    const char* survey_path = NULL;                           /* --survey: pinned ZM-1 channel order + orientation */
    const char* ir_prefix = NULL;
    const char* localize_file = NULL;
    const char* track_body = NULL;                            /* --track: the ZM-1 stand's rigid body (id or name) */
    const char* nn_server = NULL;                             /* --natnet-server: Motive's host */
    const char* nn_multicast = "239.255.42.99";               /* --natnet-multicast */
    int    track_sim = 0, track_sim_bump = 0;                 /* --track-sim, --track-sim-bump N */
    int    mount_ring = 0, mount_off_set = 0;                 /* --mount-offset ring | x,y,z */
    float  mount_off[3] = { 0.f, 0.f, 0.f };
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
        else if (!strcmp(argv[i],"--survey") && i+1<argc)  survey_path = argv[++i]; /* --zylia: capsule self-survey (room axes) */
        else if (!strcmp(argv[i],"--eq"))                 eq          = 1;   /* per-speaker direct-sound correction FIR */
        else if (!strcmp(argv[i],"--room-eq"))            eq = room_eq = 1;  /* + room correction AT THE MIC POINT (static listener only) */
        else if (!strcmp(argv[i],"--room-eq-grid"))       rq_grid     = 1;   /* accumulate LF modal cuts at THIS mic position into room_eq_grid (tracked room EQ) */
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
        else { fprintf(stderr, "usage: calibrate [--layout f] [--out f] [--mic x y z] [--input ch] [--driver name] [--list-drivers] [--simulate] [--trims | --verify] [--room] [--eq | --room-eq | --room-eq-grid] [--zylia] [--survey f] [--ref spk dist_m] [--save-irs prefix] [--localize positions.txt] [--check] [--live N] [--latency m] [--temp T[C|F] | --c mps] [--ignore-directivity] [--check-aim] [--sim-aim-error deg] [--sim-room [absorption]] [--aim-sheet out.csv] [--sweeps N] [--aim-ref spk | --aim-ref-db dB] [--sim-move dx dy dz] [--sim-aim-steps a,b,...] [--sim-screen dB] [--track id|name [--natnet-server ip] [--natnet-multicast g] | --track-sim [--track-sim-bump N]] [--mount-offset x,y,z | ring] [--place-tol-mm mm] [--place-timeout s]\n"
                               "  --zylia: the mic is a ZM-1; --input is the FIRST of its 19 consecutive capture channels,\n"
                               "  --mic is the array center. Alone it is the single-placement position survey: distances\n"
                               "  need --latency (loopback, m at c) or --ref <spk> <m> (one tape-measured center->speaker\n"
                               "  distance). --zylia --trims measures the trims with it, --zylia --verify the second pass,\n"
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
                               "  --live N --zylia: live aiming for speaker N with the ZM-1 (--mic = the array center, default\n"
                               "  the listening point): one line per sweep with its position against the layout (needs\n"
                               "  --latency or --ref for the distance) and its off-axis angle as a tilt peak meter plus an\n"
                               "  estimated MAGNITUDE (never a direction). --aim-ref S takes speaker S's tilt as the on-axis\n"
                               "  reference, --aim-ref-db a stored one; the model's on_axis_db is the fallback. Keys: r stores\n"
                               "  the reference, p resets the peak, any other key stops. --sweeps N bounds any --live run.\n"
                               "  --sim-move / --sim-aim-steps (simulate only): the live speaker's true offset (m) and its\n"
                               "  true aim error (deg off the layout aim) per reading. --sim-screen dB: a screen's HF loss\n"
                               "  (a shelf above 4 kHz) on every speaker's path, which a reference absorbs and the file cannot.\n"
                               "  --track <id|name>: the ZM-1's stand is a tracked rigid body (Motive's frame is the room frame).\n"
                               "  Before the captures the tool shows a live line (the measured center, the target, dx/dy/dz and\n"
                               "  |d| in mm, HOLD or OK) and waits until the center is within --place-tol-mm (default 10) of the\n"
                               "  target and still (2 mm for 0.5 s, then held 1 s); that center IS the mic position. After every\n"
                               "  capture it checks for a bump (a move past half the tolerance) and stops with exit 4. Targets:\n"
                               "  trims, --verify, --live: --mic, else the layout's listening point; the --zylia survey and\n"
                               "  --room-eq-grid: --mic (required); --localize: each row, loosely (100 mm, or --place-tol-mm if\n"
                               "  wider, so the gate cannot open at the previous row), and the MEASURED position is recorded,\n"
                               "  which is what trilateration needs. The mount offset (body origin to the array center, body axes) comes from a body-frame\n"
                               "  --survey, or --mount-offset x,y,z, or --mount-offset ring (fit the circle through the body's\n"
                               "  markers from Motive's model definition, needs --natnet-server; it assumes the marker ring sits\n"
                               "  at the array center's height), else 0. Only the modes that turn capsule arrival differences into\n"
                               "  a room DIRECTION need a body-frame survey: the --zylia survey, and the --live position readout.\n"
                               "  --natnet-server is REQUIRED to track by name. On the rig a key takes the current reading anyway,\n"
                               "  with a warning; --place-timeout s (default 300) aborts the wait. Unverified against live Motive.\n"
                               "  --track-sim (simulate only): a scripted stand walks in from 8 cm off the target, settles about\n"
                               "  5 mm off it, and the captures come from its TRUE center; --track-sim-bump N knocks it 15 mm\n"
                               "  after the Nth capture.\n"); return 2; }
    }
    if (n_temp && n_c) {
        fprintf(stderr, "calibrate: --temp and --c set the same thing; pass one\n"); return 2; }
    if (room_eq && rq_grid) { fprintf(stderr, "calibrate: --room-eq and --room-eq-grid are mutually exclusive (one scheme per layout)\n"); return 2; }
    if (zylia && (localize_file || check)) {
        fprintf(stderr, "calibrate: --zylia runs the position survey, --trims, --verify or --live; drop --localize/--check\n"); return 2; }
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
    if (zylia && !trims && !verify && (eq || rq_grid || room || ir_prefix)) {
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
    const int zylia_survey_mode = zylia && !trims && !verify && live_speaker < 0;
    if (track_body && track_sim) {
        fprintf(stderr, "calibrate: --track and --track-sim are alternatives: a real stand or the simulated one\n"); return 2; }
    if (track_sim && !simulate) {
        fprintf(stderr, "calibrate: --track-sim is a --simulate knob: a simulated stand in front of a real array places nothing\n"); return 2; }
    if (track_sim_bump && !track_sim) {
        fprintf(stderr, "calibrate: --track-sim-bump knocks the SIMULATED stand; pass --track-sim\n"); return 2; }
    if (!g_tracked && (tol_set || timeout_set || mount_off_set || nn_server)) {
        fprintf(stderr, "calibrate: --place-tol-mm/--place-timeout/--mount-offset/--natnet-server are --track options\n"); return 2; }
    if (g_tracked && (check || (live_speaker >= 0 && !zylia) || aim_csv)) {
        fprintf(stderr, "calibrate: --track places the mic for the trims, --verify, --localize, --room-eq-grid, the --zylia\n"
                        "           survey and --live N --zylia; --check, the omni --live and --aim-sheet take no placement\n"); return 2; }
    if (g_tracked && (zylia_survey_mode || rq_grid) && !mic_set) {
        fprintf(stderr, "calibrate: --track with %s needs --mic x y z: the target the ZM-1 is placed at\n",
                rq_grid ? "--room-eq-grid" : "the --zylia position survey"); return 2; }
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
    if (rq_grid)
        printf("calibrate: --room-eq-grid measures this mic position's LF modal cuts and merges them into\n"
               "           the layout's room_eq_grid - one run per mic placement, --mic x y z IS the grid\n"
               "           key (a rerun within 5 cm replaces that entry). Cover the working area (ear\n"
               "           height, ~0.5-1 m spacing); the engine interpolates between positions live.\n");
    if (!out_path) out_path = layout_path;                    /* in-place by default */

    char err[256] = {0};
    static Layout L;                                          /* never a stack local (layout.h); main is not reentrant */
    if (!layout_load(layout_path, (uint32_t)FS, &L, err, sizeof err)) {
        fprintf(stderr, "calibrate: %s\n", err); return 1;
    }
    const int n = (int)L.count;
    if (aim_csv) return aim_sheet(layout_path, L, aim_csv);   /* no device, no sweep, nothing written to the layout */
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
    if (live_zy) {
        /* no trims here: the model feeds the angle estimate instead (printed by the live mode) */
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
    if (ref_spk >= 0 && (ref_spk >= n || ref_dist < 0.2)) {
        fprintf(stderr, "calibrate: --ref wants a speaker 0..%d and a distance >= 0.2 m (got %d, %.3f)\n",
                n - 1, ref_spk, ref_dist); return 2;
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
            printf("track: SIMULATED stand (--track-sim), tolerance %.1f mm, bump limit %.1f mm%s\n", g_tol_m * 1e3f,
                   0.5f * g_tol_m * 1e3f, track_sim_bump ? ", knocked 15 mm mid-run (--track-sim-bump)" : "");
        else
            printf("track: rigid body '%s', tolerance %.1f mm, bump limit %.1f mm (unverified against live Motive)\n",
                   track_body, g_tol_m * 1e3f, 0.5f * g_tol_m * 1e3f);
        if (survey_path)
            printf("calibrate: capsule survey %s installed\n", survey_path);
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
        for (int k = 0; k < K; ++k) {
            if (g_tracked) {
                /* the row is the PLAN; the tracker measures where the mic actually stands, and that
                 * is what the trilateration gets */
                char what[64]; snprintf(what, sizeof what, "localize position %d/%d", k + 1, K);
                if (track_place(planned[k], 1, what, micpos[k])) {
#ifdef BWA_HAVE_ASIO
                    if (asio_up) calib_asio_close();
#endif
                    return 1;
                }
            } else if (!simulate) { printf("  -> place the mic at (%.2f %.2f %.2f) and press Enter...", micpos[k][0], micpos[k][1], micpos[k][2]); fflush(stdout); getchar(); }
            for (int s = 0; s < n; ++s) {
                if (simulate) calib_sim_capture(s, &L, g_track_sim ? g_sim_at : micpos[k], sos, sweep, cap);
#ifdef BWA_HAVE_ASIO
                else if (!calib_asio_capture(s)) { fprintf(stderr, "calibrate: capture timed out (spk %d, pos %d)\n", s, k); calib_asio_close(); return 1; }
#endif
                if (const int bump = track_after_capture(CAPTURE_S, "localize placement", s)) {
#ifdef BWA_HAVE_ASIO
                    if (asio_up) calib_asio_close();
#endif
                    return bump;
                }
                /* AIM_BAND_HZ, not BAND_HZ: only the delay and the tilt are read here, and the tilt is
                 * the DIRECT sound's, over the bands calib_check_aim predicts it on */
                MeasureResult r; measure_response(cap, CAPLEN, sweep, NSWEEP, F1, F2, FS, AIM_BAND_HZ, &r);
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
                fprintf(stderr, "  spk %2d: trilateration failed (degenerate mic positions?)\n", s);
                pos[s][0] = pos[s][1] = pos[s][2] = 0.f; ++failed;
            } else {
                printf("  spk %2d: pos=(%+.3f %+.3f %+.3f)  [system latency %.3f m]\n", s, pos[s][0], pos[s][1], pos[s][2], lat);
                latv[s] = lat;
            }
        }
#ifdef BWA_HAVE_ASIO
        /* Cross-check the solved latency against the driver's own numbers: the solve recovers the
         * FULL loop (digital buffers + DAC/ADC + analog), the driver reports the digital half, so
         * solved-minus-driver must be a small positive residual. Negative is physically impossible
         * (device mix-up, clocking); tens of ms points at an unexpected buffer (the Dante latency
         * setting). Median over speakers — per-speaker latency should agree, it is one system. */
        { long il = 0, ol = 0;
          if (!simulate && calib_asio_latencies(&il, &ol)) {
              double lats[BWA_MAX_CHANNELS]; int nl = 0;
              for (int s = 0; s < n; ++s) if (latv[s] >= 0.0) lats[nl++] = latv[s];
              if (nl > 0) {
                  for (int a = 1; a < nl; ++a) { double v = lats[a]; int b = a;   /* tiny insertion sort */
                      while (b > 0 && lats[b-1] > v) { lats[b] = lats[b-1]; --b; } lats[b] = v; }
                  double med_m = (nl & 1) ? lats[nl/2] : 0.5 * (lats[nl/2 - 1] + lats[nl/2]);
                  double drv_m = sos * (double)(il + ol) / FS;
                  double resid_ms = (med_m - drv_m) / sos * 1e3;
                  printf("localize: solved system latency %.3f m (%.2f ms) vs driver digital loop %.3f m (%.2f ms) -> residual %+.2f ms\n",
                         med_m, med_m / sos * 1e3, drv_m, drv_m / sos * 1e3, resid_ms);
                  if (resid_ms < -0.5)
                      printf("  WARNING: solved latency is BELOW the driver's own digital loop - physically impossible;\n"
                             "           check the device/clocking (wrong driver? sample-rate mismatch?)\n");
                  else if (resid_ms > 20.0)
                      printf("  WARNING: residual is unexpectedly large for DAC/ADC + analog - check the Dante latency\n"
                             "           setting / an extra buffer in the loop\n");
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
        if (!calib_write_positions(layout_path, out_path, pos, n, err, sizeof err)) { fprintf(stderr, "calibrate: %s\n", err); return 1; }
        record_sos(out_path, sos);
        printf("localize: wrote %d positions to %s%s\n", n, out_path, failed ? "  (some failed, set to 0)" : "");
        free(range); free(pos); free(res); free(cap); free(sweep);
        return 0;
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
        if (g_tracked && track_place(mic, 0, "live aiming", mic)) {
#ifdef BWA_HAVE_ASIO
            if (asio_up) calib_asio_close();
#endif
            return 1;
        }
        static LiveArgs A;                                     /* static: main is not reentrant */
        A.L = &L; A.spk = live_speaker; memcpy(A.mic, mic, sizeof A.mic); A.sos = sos; A.simulate = simulate;
        A.known_latency_m = known_latency; A.ref_spk = ref_spk; A.ref_dist = ref_dist;
        A.aim_ref_spk = aim_ref_spk; A.have_aim_ref_db = have_aim_ref_db; A.aim_ref_db = aim_ref_db;
        A.sweeps = sweeps; memcpy(A.sim_move, sim_move, sizeof A.sim_move); A.sim_screen_db = sim_screen;
        A.sim_steps = sim_steps; A.nsteps = nsteps;
        A.cap19 = cap19; A.in_first = mic_in; A.have_survey = survey_path != NULL;
        A.sim_at = g_track_sim ? g_sim_at : NULL;
        A.pos_ok = !g_tracked || g_mt.body_frame;             /* a tracked position readout needs the orientation */
#ifdef BWA_HAVE_ASIO
        if (!simulate && known_latency < 0.0 && ref_spk < 0) {
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
    if (zylia && !trims && !verify) {
        /* tracked: the measured center is the array center, and the body-frame table is re-aimed for
         * however the stand is turned, BEFORE the capsule positions are read below */
        if (g_tracked && track_place(mic, 0, "zylia survey", mic)) {
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
                double lat0 = (known_latency >= 0.0) ? known_latency / C : 0.0;
                const float* at = g_track_sim ? g_sim_at : mic;    /* --track-sim: the stand's TRUE center */
                for (int j = 0; j < ZYLIA_MICS; ++j) {
                    double cx = at[0]+caps[j][0], cy = at[1]+caps[j][1], cz = at[2]+caps[j][2];
                    double dx = cx-L.speakers[s].pos[0], dy = cy-L.speakers[s].pos[1], dz = cz-L.speakers[s].pos[2];
                    row[j] = sqrt(dx*dx+dy*dy+dz*dz)/C + lat0;
                }
            }
#ifdef BWA_HAVE_ASIO
            else {
                /* RIG: sweep speaker s, record all 19 capsules in lockstep (one device, one clock:
                 * the Dante Via route), deconvolve each, take its sub-sample arrival. */
                printf("  speaker %2d: playing sweep...\n", s); fflush(stdout);
                if (!calib_asio_capture(s)) {
                    fprintf(stderr, "calibrate: capture timed out on speaker %d\n", s);
                    calib_asio_close(); free(cap19); free(arr); free(res); free(cap); free(sweep);
                    return 1;
                }
                MeasureResult rz[ZYLIA_MICS];
                int okz[ZYLIA_MICS] = { 0 };
                if (!calib_measure_zylia_rows(cap19, CAPLEN, CAPLEN, sweep, NSWEEP, BAND_HZ, rz, okz))
                    printf("  speaker %2d: capsule cross-correlation did not run; using each capsule's own peak\n", s);
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
        double latency = (known_latency >= 0.0) ? known_latency / C : 0.0;
        int lat_known = simulate || (known_latency >= 0.0);    /* simulate arrivals carry none (or exactly --latency) */
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
        }
#ifdef BWA_HAVE_ASIO
        /* Same lower-bound check as --localize: the driver's digital loop is inside every arrival, so
         * a solved latency below it is physically impossible. No upper warning here — the ZM-1's USB
         * stack plus the Dante Via legs legitimately add tens of ms the driver does not report (a clap
         * loopback on the rig measured ~60 ms, ~20 m at c, so the residual is HUGE and still correct). */
        { long il = 0, ol = 0;
          if (!simulate && lat_known && calib_asio_latencies(&il, &ol)) {
              double drv_m = sos * (double)(il + ol) / FS;
              double resid_ms = (latency * C - drv_m) / sos * 1e3;
              printf("zylia: system latency %.3f m vs driver digital loop %.3f m -> residual %+.2f ms\n",
                     latency * C, drv_m, resid_ms);
              if (resid_ms < -0.5)
                  printf("  WARNING: below the driver's own digital loop - physically impossible; check the\n"
                         "           device/clocking, the --ref distance, or the --latency value\n");
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
                   (s == ref_spk) ? "  [--ref: should read the taped distance]" : "");
        }
        if (!lat_known) {
            fprintf(stderr, "zylia: NOT writing positions - no --latency/--ref, so every distance above carries\n"
                            "       the full system latency radially (directions are exact). Tape ONE speaker's\n"
                            "       distance from the array center and re-run with --ref <spk> <m>, or measure a\n"
                            "       loopback and pass --latency <m>.\n");
            free(arr); free(pos); free(res); free(cap); free(sweep);
            return 1;
        }
        track_report("the position survey measured");
        if (!calib_write_positions(layout_path, out_path, pos, n, err, sizeof err)) { fprintf(stderr, "calibrate: %s\n", err); return 1; }
        record_sos(out_path, sos);
        printf("zylia: wrote %d positions to %s\n", n, out_path);
        free(arr); free(pos); free(res); free(cap); free(sweep);
        return 0;
    }

    /* --- drift check: one fast pass from the mic position, flag anything nudged --- */
    if (check) {
        float (*pos)[3] = (float(*)[3])malloc((size_t)n * 3 * sizeof(float));
        for (int s = 0; s < n; ++s) { pos[s][0]=L.speakers[s].pos[0]; pos[s][1]=L.speakers[s].pos[1]; pos[s][2]=L.speakers[s].pos[2]; }
        double* range = (double*)malloc((size_t)n * sizeof(double));
        for (int s = 0; s < n; ++s) {
            if (simulate) calib_sim_capture(s, &L, mic, sos, sweep, cap);
#ifdef BWA_HAVE_ASIO
            else if (!calib_asio_capture(s)) { fprintf(stderr, "calibrate: capture timed out (spk %d)\n", s); calib_asio_close(); return 1; }
#endif
            MeasureResult r; measure_response(cap, CAPLEN, sweep, NSWEEP, F1, F2, FS, BAND_HZ, &r);
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
            MeasureResult r; measure_response(cap, CAPLEN, sweep, NSWEEP, F1, F2, FS, BAND_HZ, &r);
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
    if (g_tracked && track_place(mic, 0, verify ? "verify" : (rq_grid ? "room-eq-grid" : "trims"), mic)) {
#ifdef BWA_HAVE_ASIO
        if (asio_up) calib_asio_close();
#endif
        return 1;
    }
    /* the trim loop and --verify capture through one helper (measure_speaker), omni or ZM-1 */
    static Pass P;                                             /* static: main is not reentrant */
    P.L = &L; P.mic = mic; P.sos = sos; P.simulate = simulate; P.zylia = zylia;
    P.sim_at = g_track_sim ? g_sim_at : NULL;
    P.sweep = sweep; P.cap = cap; P.cap19 = cap19;
    zylia_capsules(P.caps);                                    /* the installed survey, else the built-in table */
    P.nplay = nplay;
    P.in_first = mic_in;
    P.play = (float*)calloc((size_t)CAPLEN, sizeof(float));
    P.tmp  = (float*)calloc((size_t)CAPLEN, sizeof(float));
    if (!P.play || !P.tmp) { fprintf(stderr, "calibrate: out of memory\n"); return 1; }
    if (zylia) {
        printf("zylia: the ZM-1 is the %s mic, array center at (%.2f %.2f %.2f): level, bands and direct share are\n"
               "       the power mean over its %d capsules, the delay is the arrival at the center\n",
               verify ? "verify" : "trim", mic[0], mic[1], mic[2], ZYLIA_MICS);
        if (!survey_path)
            printf("zylia: no --survey: the built-in capsule table. The power mean does not care about channel\n"
                   "       order; the center arrival's tilt correction does, by at most about 15 us.\n");
        if (room || ir_prefix || rq_grid)
            printf("zylia: --room/--save-irs/--room-eq-grid read the MEAN of the 19 capsule captures: the center\n"
                   "       pressure below about 2 kHz, a direction-dependent beam above it. --room-eq-grid only uses\n"
                   "       30 to 200 Hz; the RT60, the reflection levels and saved IRs above 2 kHz carry the sphere.\n");
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
            if (!measure_speaker(P, i, 1, &res[i])) {
#ifdef BWA_HAVE_ASIO
                if (asio_up) calib_asio_close();
#endif
                fprintf(stderr, "calibrate: verify capture failed on speaker %d\n", i); return 1;
            }
            if (const int bump = track_after_capture(CAPTURE_S, "verify", i)) {
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
        free(P.play); free(P.tmp); free(cap19); free(res); free(cap); free(sweep);
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
        if (!measure_speaker(P, i, 0, &res[i])) {
#ifdef BWA_HAVE_ASIO
            if (asio_up) calib_asio_close();
#endif
            return 1;
        }
        if (const int bump = track_after_capture(CAPTURE_S, rq_grid ? "room-eq-grid run" : "trim run", i)) {
#ifdef BWA_HAVE_ASIO
            if (asio_up) calib_asio_close();
#endif
            return bump;
        }
        printf("  speaker %2d: delay=%6d  level=%.4f  bands=[%.3f %.3f %.3f]",
               i, res[i].delay_samples, res[i].level, res[i].band[0], res[i].band[1], res[i].band[2]);
        if (zylia) printf("  capsule level spread %.1f dB", P.capsule_spread_db);
        printf("\n");
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

    free(gdb); free(dms); free(res); free(cap); free(sweep); free(cap19); free(P.play); free(P.tmp);
    return 0;
}
