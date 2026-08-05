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
#include "measure.h"
#include "calib.h"
#include "layout.h"
#include "zylia.h"
#include "sink.h"          /* BWA_CHANNELS */
#include "sos.h"           /* room-temperature speed of sound: --temp / --c, or the layout's */
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

/* Record the c every range was scaled by into the layout that just received them, so a rerun on this
 * file inherits the rig's temperature instead of needing the flag again, and a reader can tell which
 * c the survey assumed. Non-fatal: the measurements themselves are already written. */
static void record_sos(const char* path, double sos) {
    char err[256] = {0};
    if (!calib_write_sos(path, path, sos, err, sizeof err))
        fprintf(stderr, "calibrate: warning: could not record speed of sound (%s)\n", err);
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
    int   mic_in = 0, simulate = 0, room = 0, check = 0, live_speaker = -1, eq = 0, room_eq = 0, rq_grid = 0, zylia = 0;
    double known_latency = -1.0;
    int    ref_spk = -1; double ref_dist = 0.0;               /* --ref: one tape-measured distance -> latency */
    const char* survey_path = NULL;                           /* --survey: pinned ZM-1 channel order + orientation */
    const char* ir_prefix = NULL;
    const char* localize_file = NULL;
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
        else if (!strcmp(argv[i],"--zylia"))              zylia       = 1;   /* single-position localization with the ZM-1 */
        else if (!strcmp(argv[i],"--mic") && i+3<argc) { mic[0]=(float)atof(argv[++i]); mic[1]=(float)atof(argv[++i]); mic[2]=(float)atof(argv[++i]); }
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
        else { fprintf(stderr, "usage: calibrate [--layout f] [--out f] [--mic x y z] [--input ch] [--driver name] [--list-drivers] [--simulate] [--room] [--eq | --room-eq | --room-eq-grid] [--zylia] [--survey f] [--ref spk dist_m] [--save-irs prefix] [--localize positions.txt] [--check] [--live N] [--latency m] [--temp T[C|F] | --c mps]\n"
                               "  --zylia: ZM-1 single-placement localization; --input is the FIRST of its 19 consecutive\n"
                               "  capture channels, --mic is the array center. Distances need --latency (loopback, m at c)\n"
                               "  or --ref <spk> <m> (one tape-measured center->speaker distance).\n"
                               "  --temp: room air temperature; every surveyed range scales with it (2%% of c = 8 cm at\n"
                               "  4 m). Recorded into the layout's reference.speed_of_sound_mps, so set it once per rig.\n"); return 2; }
    }
    if (n_temp && n_c) {
        fprintf(stderr, "calibrate: --temp and --c set the same thing; pass one\n"); return 2; }
    if (room_eq && rq_grid) { fprintf(stderr, "calibrate: --room-eq and --room-eq-grid are mutually exclusive (one scheme per layout)\n"); return 2; }
    if (zylia && (localize_file || check || live_speaker >= 0)) {
        fprintf(stderr, "calibrate: --zylia is its own mode (19-input capture); drop --localize/--check/--live\n"); return 2; }
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
    /* No explicit flag: inherit the rig's own c from the layout it was surveyed with. Falls back to
     * the 20 C reference, which is also what the synthetic-capture path assumes (calib_capture.cpp),
     * so --simulate stays bit-identical unless you deliberately ask for another temperature. */
    const char* sos_src = "default";
    if (sos_set)                                sos_src = "--temp/--c";
    else if (calib_read_sos(layout_path, &sos)) sos_src = "layout";
    printf("calibrate: %d speakers from %s; mic at (%.2f %.2f %.2f)%s\n",
           n, layout_path, mic[0], mic[1], mic[2], simulate ? "  [SIMULATE]" : "");
    printf("           speed of sound %.1f m/s (%s)\n", sos, sos_src);
    if (ref_spk >= 0 && (ref_spk >= n || ref_dist < 0.2)) {
        fprintf(stderr, "calibrate: --ref wants a speaker 0..%d and a distance >= 0.2 m (got %d, %.3f)\n",
                n - 1, ref_spk, ref_dist); return 2;
    }
    if (survey_path) {   /* pins the ZM-1's channel order + orientation; installs via zylia_set_capsules */
        ZyliaMount mount; char serr[192] = {0};
        if (!zylia_survey_load(survey_path, &mount, serr, sizeof serr)) {
            fprintf(stderr, "calibrate: survey: %s\n", serr); return 1; }
        if (mount.body_frame) {
            fprintf(stderr, "calibrate: survey %s is BODY-FRAME (tracked mount); this tool has no tracker, so\n"
                            "           it cannot re-aim the table. Use a room-axes survey taken at the CURRENT\n"
                            "           mounting (calib_view -> Zylia -> Capsule survey), or bwa_validate --track.\n",
                    survey_path);
            return 1;
        }
        printf("calibrate: capsule survey %s installed (channel order + orientation pinned)\n", survey_path);
    }

    float* sweep = (float*)malloc((size_t)NSWEEP * sizeof(float));
    float* cap   = (float*)malloc((size_t)CAPLEN * sizeof(float));
    MeasureResult* res = (MeasureResult*)calloc((size_t)n, sizeof(MeasureResult));
    if (!sweep || !cap || !res) { fprintf(stderr, "calibrate: out of memory\n"); return 1; }
    measure_sweep(sweep, NSWEEP, F1, F2, FS);
    double rt60_sum = 0.0; int rt60_n = 0;                     /* room-report aggregate */

#ifdef BWA_HAVE_ASIO
    int asio_up = 0;
    float* cap19 = NULL;                                       /* --zylia: [19][CAL_CAPLEN], one row per capsule */
    if (!simulate) {
        if (zylia) {
            cap19 = (float*)malloc((size_t)ZYLIA_MICS * CAPLEN * sizeof(float));
            if (!cap19) { fprintf(stderr, "calibrate: out of memory\n"); return 1; }
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
        for (int k = 0; k < K; ++k) {
            if (!simulate) { printf("  -> place the mic at (%.2f %.2f %.2f) and press Enter...", micpos[k][0], micpos[k][1], micpos[k][2]); fflush(stdout); getchar(); }
            for (int s = 0; s < n; ++s) {
                if (simulate) calib_sim_capture(s, &L, micpos[k], sos, sweep, cap);
#ifdef BWA_HAVE_ASIO
                else if (!calib_asio_capture(s)) { fprintf(stderr, "calibrate: capture timed out (spk %d, pos %d)\n", s, k); calib_asio_close(); return 1; }
#endif
                MeasureResult r; measure_response(cap, CAPLEN, sweep, NSWEEP, F1, F2, FS, BAND_HZ, &r);
                range[(size_t)s * K + k] = ((double)r.delay_samples + r.delay_frac) * sos / FS; /* c*delay, meters (sub-sample) */
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
         * (device mix-up, clocking); tens of ms points at an unexpected buffer (the Dante latency
         * setting). Median over speakers — per-speaker latency should agree, it is one system. */
        { long il = 0, ol = 0;
          if (!simulate && calib_asio_latencies(&il, &ol)) {
              double lats[BWA_CHANNELS]; int nl = 0;
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
                      printf("  WARNING: solved latency is BELOW the driver's own digital loop — physically impossible;\n"
                             "           check the device/clocking (wrong driver? sample-rate mismatch?)\n");
                  else if (resid_ms > 20.0)
                      printf("  WARNING: residual is unexpectedly large for DAC/ADC + analog — check the Dante latency\n"
                             "           setting / an extra buffer in the loop\n");
              }
          } }
#endif
        free(latv);
        if (!calib_write_positions(layout_path, out_path, pos, n, err, sizeof err)) { fprintf(stderr, "calibrate: %s\n", err); return 1; }
        record_sos(out_path, sos);
        printf("localize: wrote %d positions to %s%s\n", n, out_path, failed ? "  (some failed, set to 0)" : "");
        free(range); free(pos); free(res); free(cap); free(sweep);
        return 0;
    }

    /* --- ZM-1 single-position localization: ONE mic placement, 19 capsules -> direction + distance --- */
    if (zylia) {
        float caps[ZYLIA_MICS][3]; zylia_capsules(caps);       /* the installed survey if any, else built-in */
        const double C = sos;
        printf("zylia: array at (%.2f %.2f %.2f), %d capsules%s\n",
               mic[0], mic[1], mic[2], ZYLIA_MICS, simulate ? "  [SIMULATE]" : "");
        if (!simulate && !survey_path)
            printf("zylia: no --survey: trusting the BUILT-IN capsule table. Channel order and the device's\n"
                   "       orientation in the room are then unpinned — a yaw error rotates every recovered\n"
                   "       position (docs/calibration.md, \"The capsule self-survey\").\n");

        /* capture first, solve after: --ref needs the reference speaker's arrivals before any distance */
        double* arr = (double*)malloc((size_t)n * ZYLIA_MICS * sizeof(double));
        if (!arr) { fprintf(stderr, "calibrate: out of memory\n"); return 1; }
        for (int s = 0; s < n; ++s) {
            double* row = arr + (size_t)s * ZYLIA_MICS;
            if (simulate) {                                    /* exact wavefront from the speaker's true pos */
                double lat0 = (known_latency >= 0.0) ? known_latency / C : 0.0;
                for (int j = 0; j < ZYLIA_MICS; ++j) {
                    double cx = mic[0]+caps[j][0], cy = mic[1]+caps[j][1], cz = mic[2]+caps[j][2];
                    double dx = cx-L.speakers[s].pos[0], dy = cy-L.speakers[s].pos[1], dz = cz-L.speakers[s].pos[2];
                    row[j] = sqrt(dx*dx+dy*dy+dz*dz)/C + lat0;
                }
            }
#ifdef BWA_HAVE_ASIO
            else {
                /* RIG: sweep speaker s, record all 19 capsules in lockstep (one device, one clock —
                 * the Dante Via route), deconvolve each, take its sub-sample arrival. */
                printf("  speaker %2d: playing sweep...\n", s); fflush(stdout);
                if (!calib_asio_capture(s)) {
                    fprintf(stderr, "calibrate: capture timed out on speaker %d\n", s);
                    calib_asio_close(); free(cap19); free(arr); free(res); free(cap); free(sweep);
                    return 1;
                }
                for (int j = 0; j < ZYLIA_MICS; ++j) {
                    MeasureResult r;
                    measure_response(cap19 + (size_t)j * CAPLEN, CAPLEN, sweep, NSWEEP, F1, F2, FS, BAND_HZ, &r);
                    row[j] = ((double)r.delay_samples + r.delay_frac) / FS;
                }
            }
#endif
        }
#ifdef BWA_HAVE_ASIO
        if (asio_up) { calib_asio_close(); asio_up = 0; }
        free(cap19); cap19 = NULL;
#endif

        /* latency: --ref (one tape-measured distance) beats --latency (a loopback measurement).
         * Directions never need it; distances are c*(arrival - latency), so without either the
         * hardware distances would carry the full system latency radially. */
        double latency = (known_latency >= 0.0) ? known_latency / C : 0.0;
        int lat_known = simulate || (known_latency >= 0.0);    /* simulate arrivals carry none (or exactly --latency) */
        if (ref_spk >= 0) {
            const double* rr = arr + (size_t)ref_spk * ZYLIA_MICS;
            float dref[3] = { 0, 0, 0 };                       /* solve failure -> no tilt correction */
            zylia_doa(rr, dref);
            double mean_t = 0.0, mdotd = 0.0;                  /* mean arrival, wavefront-tilt corrected: the
                                                                * capsule centroid is only ~0 (built-in table:
                                                                * ~2.6 mm up), so project it out along the DOA */
            for (int j = 0; j < ZYLIA_MICS; ++j) {
                mean_t += rr[j] / ZYLIA_MICS;
                mdotd  += (caps[j][0]*dref[0] + caps[j][1]*dref[1] + caps[j][2]*dref[2]) / ZYLIA_MICS;
            }
            double lat_ref = mean_t + mdotd / C - ref_dist / C;
            if (known_latency >= 0.0)
                printf("zylia: --ref solves latency %.3f m vs --latency %.3f m (delta %+.1f cm) — using --ref\n",
                       lat_ref * C, known_latency, (lat_ref * C - known_latency) * 100.0);
            latency = lat_ref; lat_known = 1;
            printf("zylia: system latency from --ref %d @ %.3f m: %.2f ms (%.3f m at c)\n",
                   ref_spk, ref_dist, latency * 1e3, latency * C);
            if (!simulate && latency <= 0.0)
                printf("  WARNING: solved latency is not positive — wrong --ref speaker or distance?\n");
        }
#ifdef BWA_HAVE_ASIO
        /* Same lower-bound check as --localize: the driver's digital loop is inside every arrival, so
         * a solved latency below it is physically impossible. No upper warning here — a Dante Via leg
         * on the capsule inputs legitimately adds ~10 ms the driver does not report. */
        { long il = 0, ol = 0;
          if (!simulate && lat_known && calib_asio_latencies(&il, &ol)) {
              double drv_m = sos * (double)(il + ol) / FS;
              double resid_ms = (latency * C - drv_m) / sos * 1e3;
              printf("zylia: system latency %.3f m vs driver digital loop %.3f m -> residual %+.2f ms\n",
                     latency * C, drv_m, resid_ms);
              if (resid_ms < -0.5)
                  printf("  WARNING: below the driver's own digital loop — physically impossible; check the\n"
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
            fprintf(stderr, "zylia: NOT writing positions — no --latency/--ref, so every distance above carries\n"
                            "       the full system latency radially (directions are exact). Tape ONE speaker's\n"
                            "       distance from the array center and re-run with --ref <spk> <m>, or measure a\n"
                            "       loopback and pass --latency <m>.\n");
            free(arr); free(pos); free(res); free(cap); free(sweep);
            return 1;
        }
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
              printf("  (driver digital loop = %.3f m of the range below — a lower bound for --latency)\n",
                     sos * (double)(il + ol) / FS); }
#endif
        int iters = simulate ? 4 : (1 << 30);
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

    const int NTAPS = 256;                                     /* correction-FIR length (<= BWA_EQ_TAPS) */
    float*    eq_taps = NULL; uint16_t* eq_lens = NULL;
    MeasureEqSection* rq_cuts = NULL; int* rq_counts = NULL;   /* --room-eq: LF modal cuts per speaker */
    if (eq) { eq_taps = (float*)calloc((size_t)n * BWA_EQ_TAPS, sizeof(float));
              eq_lens = (uint16_t*)calloc((size_t)n, sizeof(uint16_t)); }
    if (room_eq || rq_grid) { rq_cuts   = (MeasureEqSection*)calloc((size_t)n * BWA_ROOM_EQ_MAX, sizeof(MeasureEqSection));
                              rq_counts = (int*)calloc((size_t)n, sizeof(int)); }
    for (int i = 0; i < n; ++i) {
        if (simulate) {
            calib_sim_capture(i, &L, mic, sos, sweep, cap);
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

    free(gdb); free(dms); free(res); free(cap); free(sweep);
    return 0;
}
