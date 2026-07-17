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
 *
 * Two capture backends (extracted to calib_capture.cpp, shared with bwa_calib_view's Capture tab):
 *   - ASIO full-duplex (gated on BWA_HAVE_ASIO): 26 outputs + one mic input, sample-aligned. Built when
 *     the ASIO SDK is vendored. NOT verified on hardware here — treat as rig bring-up code (it mirrors
 *     asio_sink.cpp's host: load -> ASIOInit -> ASIOGetChannels -> create in+out buffers -> Start).
 *   - simulate: delays/attenuates the sweep per the layout's speaker->mic distances (+ a deterministic
 *     sensitivity wobble) so the whole measure -> solve -> writeback path runs without the rig.
 *
 * Compiled as C++ only because the ASIO host helpers are C++; the engine pieces it calls are C. Built
 * opt-in: cmake -DBWA_BUILD_CALIBRATE=ON.
 */
extern "C" {
#include "measure.h"
#include "calib.h"
#include "layout.h"
#include "zylia.h"
#include "sink.h"          /* BWA_CHANNELS */
}
#include "calib_capture.h" /* sweep constants + the simulate/ASIO capture backends */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef BWA_HAVE_ASIO
#define WIN32_LEAN_AND_MEAN
#include <windows.h>       /* Sleep */
#include <conio.h>         /* _kbhit/_getch for --live */
#endif

/* local aliases for the shared sweep geometry (the rest of this file predates the extraction) */
static const double FS         = CAL_FS;
static const double F1         = CAL_F1, F2 = CAL_F2;
static const double BAND_HZ[2] = { CAL_BAND_LO, CAL_BAND_HI };
static const int    NSWEEP     = CAL_NSWEEP;
static const int    CAPLEN     = CAL_CAPLEN;
static const int    IR_LEN     = CAL_IRLEN;

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
    int   mic_in = 0, simulate = 0, room = 0, check = 0, live_speaker = -1, eq = 0, room_eq = 0, rq_grid = 0, zylia = 0;
    double known_latency = -1.0;
    const char* ir_prefix = NULL;
    const char* localize_file = NULL;
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
        else if (!strcmp(argv[i],"--latency") && i+1<argc) known_latency = atof(argv[++i]); /* c*tau meters, for --live absolute distance */
        else if (!strcmp(argv[i],"--eq"))                 eq          = 1;   /* per-speaker direct-sound correction FIR */
        else if (!strcmp(argv[i],"--room-eq"))            eq = room_eq = 1;  /* + room correction AT THE MIC POINT (static listener only) */
        else if (!strcmp(argv[i],"--room-eq-grid"))       rq_grid     = 1;   /* accumulate LF modal cuts at THIS mic position into room_eq_grid (tracked room EQ) */
        else if (!strcmp(argv[i],"--zylia"))              zylia       = 1;   /* single-position localization with the ZM-1 */
        else if (!strcmp(argv[i],"--mic") && i+3<argc) { mic[0]=(float)atof(argv[++i]); mic[1]=(float)atof(argv[++i]); mic[2]=(float)atof(argv[++i]); }
        else { fprintf(stderr, "usage: calibrate [--layout f] [--out f] [--mic x y z] [--input ch] [--driver name] [--list-drivers] [--simulate] [--room] [--eq | --room-eq | --room-eq-grid] [--zylia] [--save-irs prefix] [--localize positions.txt] [--check] [--live N] [--latency m]\n"); return 2; }
    }
    if (room_eq && rq_grid) { fprintf(stderr, "calibrate: --room-eq and --room-eq-grid are mutually exclusive (one scheme per layout)\n"); return 2; }
    if (room_eq)
        printf("calibrate: --room-eq corrects the ROOM at the mic position — valid only for a STATIC\n"
               "           listener seated there (SPCAP/VBAP deployments); a roaming listener wants plain --eq.\n"
               "           Place the mic at the listening position, ear height.\n");
    if (rq_grid)
        printf("calibrate: --room-eq-grid measures this mic position's LF modal cuts and merges them into\n"
               "           the layout's room_eq_grid — one run per mic placement, --mic x y z IS the grid\n"
               "           key (a rerun within 5 cm replaces that entry). Cover the working area (ear\n"
               "           height, ~0.5-1 m spacing); the engine interpolates between positions live.\n");
    if (!out_path) out_path = layout_path;                    /* in-place by default */

    char err[256] = {0};
    Layout L;
    if (!layout_load(layout_path, (uint32_t)FS, &L, err, sizeof err)) {
        fprintf(stderr, "calibrate: %s\n", err); return 1;
    }
    const int n = (int)L.count;
    printf("calibrate: %d speakers from %s; mic at (%.2f %.2f %.2f)%s\n",
           n, layout_path, mic[0], mic[1], mic[2], simulate ? "  [SIMULATE]" : "");

    float* sweep = (float*)malloc((size_t)NSWEEP * sizeof(float));
    float* cap   = (float*)malloc((size_t)CAPLEN * sizeof(float));
    MeasureResult* res = (MeasureResult*)calloc((size_t)n, sizeof(MeasureResult));
    if (!sweep || !cap || !res) { fprintf(stderr, "calibrate: out of memory\n"); return 1; }
    measure_sweep(sweep, NSWEEP, F1, F2, FS);
    double rt60_sum = 0.0; int rt60_n = 0;                     /* room-report aggregate */

#ifdef BWA_HAVE_ASIO
    int asio_up = 0;
    if (!simulate) { if (calib_asio_open(driver, mic_in, n, sweep, cap) != 0) return 1; asio_up = 1; }
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
        for (int k = 0; k < K; ++k) {
            if (!simulate) { printf("  -> place the mic at (%.2f %.2f %.2f) and press Enter...", micpos[k][0], micpos[k][1], micpos[k][2]); fflush(stdout); getchar(); }
            for (int s = 0; s < n; ++s) {
                if (simulate) calib_sim_capture(s, &L, micpos[k], sweep, cap);
#ifdef BWA_HAVE_ASIO
                else if (!calib_asio_capture(s)) { fprintf(stderr, "calibrate: capture timed out (spk %d, pos %d)\n", s, k); calib_asio_close(); return 1; }
#endif
                MeasureResult r; measure_response(cap, CAPLEN, sweep, NSWEEP, F1, F2, FS, BAND_HZ, &r);
                range[(size_t)s * K + k] = ((double)r.delay_samples + r.delay_frac) * 343.0 / FS; /* c*delay, meters (sub-sample) */
            }
        }
#ifdef BWA_HAVE_ASIO
        if (asio_up) calib_asio_close();
#endif
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
         * (device mix-up, clocking); tens of ms points at an unexpected buffer (the DVS latency
         * setting). Median over speakers — per-speaker latency should agree, it is one system. */
        { long il = 0, ol = 0;
          if (!simulate && calib_asio_latencies(&il, &ol)) {
              double lats[BWA_CHANNELS]; int nl = 0;
              for (int s = 0; s < n; ++s) if (latv[s] >= 0.0) lats[nl++] = latv[s];
              if (nl > 0) {
                  for (int a = 1; a < nl; ++a) { double v = lats[a]; int b = a;   /* tiny insertion sort */
                      while (b > 0 && lats[b-1] > v) { lats[b] = lats[b-1]; --b; } lats[b] = v; }
                  double med_m = (nl & 1) ? lats[nl/2] : 0.5 * (lats[nl/2 - 1] + lats[nl/2]);
                  double drv_m = 343.0 * (double)(il + ol) / FS;
                  double resid_ms = (med_m - drv_m) / 343.0 * 1e3;
                  printf("localize: solved system latency %.3f m (%.2f ms) vs driver digital loop %.3f m (%.2f ms) -> residual %+.2f ms\n",
                         med_m, med_m / 343.0 * 1e3, drv_m, drv_m / 343.0 * 1e3, resid_ms);
                  if (resid_ms < -0.5)
                      printf("  WARNING: solved latency is BELOW the driver's own digital loop — physically impossible;\n"
                             "           check the device/clocking (wrong driver? sample-rate mismatch?)\n");
                  else if (resid_ms > 20.0)
                      printf("  WARNING: residual is unexpectedly large for DAC/ADC + analog — check the DVS latency\n"
                             "           setting / an extra buffer in the loop\n");
              }
          } }
#endif
        free(latv);
        if (!calib_write_positions(layout_path, out_path, pos, n, err, sizeof err)) { fprintf(stderr, "calibrate: %s\n", err); return 1; }
        printf("localize: wrote %d positions to %s%s\n", n, out_path, failed ? "  (some failed, set to 0)" : "");
        free(range); free(pos); free(res); free(cap); free(sweep);
        return 0;
    }

    /* --- ZM-1 single-position localization: ONE mic placement, 19 capsules -> direction + distance --- */
    if (zylia) {
        float dirs[ZYLIA_MICS][3]; float R; zylia_geometry(dirs, &R);
        const double C = 343.0;
        const double latency = (known_latency >= 0.0) ? known_latency / C : 0.0;   /* c*tau meters -> seconds */
        printf("zylia: array at (%.2f %.2f %.2f), 19 capsules, R=%.3f m%s\n",
               mic[0], mic[1], mic[2], R, simulate ? "  [SIMULATE]" : "");
        float (*pos)[3] = (float(*)[3])malloc((size_t)n * 3 * sizeof(float));
        for (int s = 0; s < n; ++s) {
            double arr[ZYLIA_MICS];
            if (simulate) {                                    /* exact wavefront from the speaker's true pos */
                for (int j = 0; j < ZYLIA_MICS; ++j) {
                    double cx = mic[0]+R*dirs[j][0], cy = mic[1]+R*dirs[j][1], cz = mic[2]+R*dirs[j][2];
                    double dx = cx-L.speakers[s].pos[0], dy = cy-L.speakers[s].pos[1], dz = cz-L.speakers[s].pos[2];
                    arr[j] = sqrt(dx*dx+dy*dy+dz*dz)/C + latency;
                }
            } else {
                /* RIG: play the sweep on speaker s, capture the ZM-1's 19 channels, deconvolve each, take its
                 * sub-sample arrival -> arr[j]. The ZM-1 is a SEPARATE device from the Dante output; its 19
                 * capsules are mutually sample-locked (one ADC), so the DOA is exact regardless of the
                 * cross-device clock — but running two ASIO drivers at once is the unbuilt rig piece. */
                fprintf(stderr, "calibrate: --zylia hardware capture is not wired in this build; use --simulate\n"
                                "  (the ZM-1 is a second 19-ch input device — see docs/calibration.md)\n");
#ifdef BWA_HAVE_ASIO
                if (asio_up) calib_asio_close();
#endif
                free(pos); free(res); free(cap); free(sweep);
                return 1;
            }
            float p[3], dir[3], dist;
            if (!zylia_localize(arr, mic, latency, C, p, &dist)) { p[0]=p[1]=p[2]=0.f; dist=0.f; }
            zylia_doa(arr, dir);
            pos[s][0]=p[0]; pos[s][1]=p[1]; pos[s][2]=p[2];
            printf("  spk %2d: dir=(%+.3f %+.3f %+.3f)  pos=(%+.3f %+.3f %+.3f)  dist=%.3f m\n",
                   s, dir[0],dir[1],dir[2], p[0],p[1],p[2], dist);
        }
        if (!calib_write_positions(layout_path, out_path, pos, n, err, sizeof err)) { fprintf(stderr, "calibrate: %s\n", err); return 1; }
        printf("zylia: wrote %d positions to %s%s\n", n, out_path,
               (known_latency < 0.0) ? "  (no --latency: directions exact, distances uncalibrated)" : "");
        free(pos); free(res); free(cap); free(sweep);
        return 0;
    }

    /* --- drift check: one fast pass from the mic position, flag anything nudged --- */
    if (check) {
        float (*pos)[3] = (float(*)[3])malloc((size_t)n * 3 * sizeof(float));
        for (int s = 0; s < n; ++s) { pos[s][0]=L.speakers[s].pos[0]; pos[s][1]=L.speakers[s].pos[1]; pos[s][2]=L.speakers[s].pos[2]; }
        double* range = (double*)malloc((size_t)n * sizeof(double));
        for (int s = 0; s < n; ++s) {
            if (simulate) calib_sim_capture(s, &L, mic, sweep, cap);
#ifdef BWA_HAVE_ASIO
            else if (!calib_asio_capture(s)) { fprintf(stderr, "calibrate: capture timed out (spk %d)\n", s); calib_asio_close(); return 1; }
#endif
            MeasureResult r; measure_response(cap, CAPLEN, sweep, NSWEEP, F1, F2, FS, BAND_HZ, &r);
            range[s] = ((double)r.delay_samples + r.delay_frac) * 343.0 / FS;
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
              printf("  (driver digital loop = %.3f m of the range below — a lower bound for --latency)\n",
                     343.0 * (double)(il + ol) / FS); }
#endif
        int iters = simulate ? 4 : (1 << 30);
        for (int t = 0; t < iters; ++t) {
            if (simulate) calib_sim_capture(live_speaker, &L, mic, sweep, cap);
#ifdef BWA_HAVE_ASIO
            else {
                if (!calib_asio_capture(live_speaker)) { fprintf(stderr, "\ncalibrate: capture timed out\n"); calib_asio_close(); return 1; }
                if (_kbhit()) { _getch(); break; }
            }
#endif
            MeasureResult r; measure_response(cap, CAPLEN, sweep, NSWEEP, F1, F2, FS, BAND_HZ, &r);
            double range = ((double)r.delay_samples + r.delay_frac) * 343.0 / FS;
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

    const int NTAPS = 256;                                     /* correction-FIR length (<= BWA_EQ_TAPS) */
    float*    eq_taps = NULL; uint16_t* eq_lens = NULL;
    MeasureEqSection* rq_cuts = NULL; int* rq_counts = NULL;   /* --room-eq: LF modal cuts per speaker */
    if (eq) { eq_taps = (float*)calloc((size_t)n * BWA_EQ_TAPS, sizeof(float));
              eq_lens = (uint16_t*)calloc((size_t)n, sizeof(uint16_t)); }
    if (room_eq || rq_grid) { rq_cuts   = (MeasureEqSection*)calloc((size_t)n * BWA_ROOM_EQ_MAX, sizeof(MeasureEqSection));
                              rq_counts = (int*)calloc((size_t)n, sizeof(int)); }
    for (int i = 0; i < n; ++i) {
        if (simulate) {
            calib_sim_capture(i, &L, mic, sweep, cap);
        } else {
#ifdef BWA_HAVE_ASIO
            printf("  speaker %2d: playing sweep...\n", i); fflush(stdout);
            if (!calib_asio_capture(i)) { fprintf(stderr, "calibrate: capture timed out on speaker %d\n", i); calib_asio_close(); return 1; }
#endif
        }
        measure_response(cap, CAPLEN, sweep, NSWEEP, F1, F2, FS, BAND_HZ, &res[i]);
        printf("  speaker %2d: delay=%6d  level=%.4f  bands=[%.3f %.3f %.3f]\n",
               i, res[i].delay_samples, res[i].level, res[i].band[0], res[i].band[1], res[i].band[2]);
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
    calib_solve(res, pos, mic, n, FS, gdb, dms);
    free(pos);

    /* report the spread + write back */
    float gmin=1e9f, gmax=-1e9f, dmax=0.f;
    for (int i = 0; i < n; ++i) { if (gdb[i]<gmin) gmin=gdb[i]; if (gdb[i]>gmax) gmax=gdb[i]; if (dms[i]>dmax) dmax=dms[i]; }
    printf("trims: gain_db in [%.2f, %.2f]  max delay %.3f ms\n", gmin, gmax, dmax);
    for (int i = 0; i < n; ++i) printf("  spk %2d: gain_db=%+.2f  delay_ms=%.3f\n", i, gdb[i], dms[i]);

    if (!calib_write_layout(layout_path, out_path, gdb, dms, n, err, sizeof err)) {
        fprintf(stderr, "calibrate: %s\n", err); return 1;
    }
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

    free(gdb); free(dms); free(res); free(cap); free(sweep);
    return 0;
}
