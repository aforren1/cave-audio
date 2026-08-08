/*
 * valid_test.c — the phantom-localization harness, off-hardware.
 *
 * Two jobs. First, pin the machinery: the statistics are exact on known inputs, the grid builder
 * produces the directions it claims, and a cell scored at the sweet spot comes back sane (if it does
 * not, nothing below means anything). Second, RUN the shootout and report it — three panners, solved
 * either at the listener (tracked) or at the sweet spot (fixed), over a listener set that separates
 * horizontal displacement from HEIGHT.
 *
 * Height is its own axis on purpose. Off-center-in-the-plane and off-center-in-height are different
 * failure modes: this array's speakers sit mostly above ear level, and the layout's alignment delays
 * time-align arrivals at ONE reference point, so a seated or tall listener is mis-aligned in a way a
 * horizontal step does not reproduce. Pooling the two would average away both.
 *
 * Note what is asserted and what is only reported. Which panner wins is a RESULT, not a contract —
 * asserting it would bake today's answer into a regression test and quietly convert a measurement
 * into an assumption. So the assertions cover the machinery and the geometrically necessary
 * relationships; the comparison itself is printed for a human to read.
 */
#include "valid.h"
#include "bw_audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++fails; } } while (0)

#define NAZ   12
#define NEL    3
#define NTGT  (NAZ * NEL)
#define NPAN   3
#define NCAP   2048u          /* one analysis frame: the synthetic field is clean, so more buys
                               * nothing but wall-clock, and this sweep is ~1500 cells */

static const char* pan_name(int p) {
    return p == BWA_PAN_DBAP ? "DBAP" : (p == BWA_PAN_SPCAP ? "SPCAP" : "VBAP");
}

/* median miss over the targets of one (panner, listener) block; fills `out` with the usable cells
 * and returns how many there were. */
static int block_misses(const ValidCell* cells, int npan, int nlis, int ntgt,
                        int p, int li, double* out) {
    int w = 0;
    for (int t = 0; t < ntgt; ++t) {
        const ValidCell* c = &cells[((size_t)p * nlis + li) * ntgt + t];
        if (c->ok) out[w++] = (double)c->miss_deg;
    }
    return w;
}

static void test_stats(void) {
    /* exact on a known input: median of 1..9 is 5, of 1..8 is 4.5 */
    double odd[9]  = { 3, 1, 4, 1, 5, 9, 2, 6, 8 };        /* sorted: 1 1 2 3 4 5 6 8 9 */
    double even[8] = { 8, 2, 6, 4, 1, 3, 7, 5 };           /* sorted: 1..8 -> 4.5 */
    CHECK(fabs(valid_median(odd, 9) - 4.0) < 1e-12, "median of an odd-length sample");
    CHECK(fabs(valid_median(even, 8) - 4.5) < 1e-12, "median of an even-length sample");

    /* a paired contrast is the median of the DIFFERENCES, not the difference of the medians —
     * the whole point of matched cells. Build a case where those two disagree. */
    double a[5] = {  1.0, 2.0, 3.0, 4.0, 5.0 };            /* median 3 */
    double b[5] = { 10.0, 1.0, 2.0, 3.0, 4.0 };            /* median 3 -> medians differ by 0... */
    double md = 0.0, lo = 0.0, hi = 0.0;                   /* ...but diffs are +9 -1 -1 -1 -1 -> -1 */
    CHECK(valid_contrast(a, b, 5, 500, 12345u, &md, &lo, &hi), "contrast runs");
    CHECK(fabs(md + 1.0) < 1e-12, "paired contrast is the median of differences");
    CHECK(fabs(valid_median(b, 5) - valid_median(a, 5)) < 1e-12,
          "...on a case where the difference of medians is 0 and would have said nothing");
    CHECK(lo <= md && md <= hi, "the bootstrap interval brackets its own estimate");

    /* a constant sample has a degenerate interval — the bootstrap must not invent width */
    double flat[6] = { 7, 7, 7, 7, 7, 7 };
    CHECK(valid_bootstrap_ci(flat, 6, 500, 999u, &lo, &hi) && fabs(hi - lo) < 1e-12,
          "a constant sample yields a zero-width interval");
    printf("[stats        ] median/contrast/bootstrap exact\n");
}

static void test_grid(void) {
    float elev[3] = { -25.0f, 0.0f, 25.0f };
    static float tg[NTGT][3];
    int n = valid_target_grid(NAZ, elev, NEL, tg, NTGT);
    CHECK(n == NTGT, "grid writes naz*nel directions");
    int unit = 1, elok = 1;
    for (int i = 0; i < n; ++i) {
        double m = sqrt((double)tg[i][0]*tg[i][0] + (double)tg[i][1]*tg[i][1] + (double)tg[i][2]*tg[i][2]);
        if (fabs(m - 1.0) > 1e-5) unit = 0;
        double el = asin(tg[i][1] > 1.0f ? 1.0 : tg[i][1]) * 180.0 / 3.14159265358979;
        if (fabs(el - elev[i / NAZ]) > 0.01) elok = 0;
    }
    CHECK(unit, "grid directions are unit vectors");
    CHECK(elok, "each row sits at its requested elevation");
    /* first azimuth is straight ahead: room front is -z */
    CHECK(fabs(tg[NAZ][0]) < 1e-5 && fabs(tg[NAZ][1]) < 1e-5 && tg[NAZ][2] < -0.99f,
          "azimuth 0 at elevation 0 points at room front (-z)");
    CHECK(!valid_target_grid(NAZ, elev, NEL, tg, NTGT - 1), "an undersized buffer is refused");
    printf("[grid         ] %d directions, %d azimuths x %d elevations\n", n, NAZ, NEL);
}

/* Spearman rank correlation (Pearson on ranks, ties averaged). Rank-based on purpose: the question
 * is whether the proxy ORDERS directions the way the measurement does, not whether it predicts
 * degrees, and localization error is heavy-tailed enough that a raw Pearson would be led by outliers. */
static void rankify(const double* v, int n, double* r) {
    int* idx = (int*)malloc(sizeof(int) * (size_t)n);
    if (!idx) return;
    for (int i = 0; i < n; ++i) idx[i] = i;
    for (int i = 1; i < n; ++i) {                      /* insertion sort; n is a few hundred */
        int k = idx[i], j = i - 1;
        while (j >= 0 && v[idx[j]] > v[k]) { idx[j+1] = idx[j]; --j; }
        idx[j+1] = k;
    }
    int i = 0;
    while (i < n) {
        int j = i;
        while (j + 1 < n && v[idx[j+1]] == v[idx[i]]) ++j;
        double avg = 0.5 * ((double)i + (double)j) + 1.0;
        for (int k = i; k <= j; ++k) r[idx[k]] = avg;
        i = j + 1;
    }
    free(idx);
}
static double spearman(const double* a, const double* b, int n) {
    if (n < 3) return 0.0;
    double* ra = (double*)malloc(sizeof(double) * (size_t)n);
    double* rb = (double*)malloc(sizeof(double) * (size_t)n);
    if (!ra || !rb) { free(ra); free(rb); return 0.0; }
    rankify(a, n, ra); rankify(b, n, rb);
    double ma = 0, mb = 0;
    for (int i = 0; i < n; ++i) { ma += ra[i]; mb += rb[i]; }
    ma /= n; mb /= n;
    double sab = 0, saa = 0, sbb = 0;
    for (int i = 0; i < n; ++i) {
        double x = ra[i] - ma, y = rb[i] - mb;
        sab += x*y; saa += x*x; sbb += y*y;
    }
    double d = sqrt(saa * sbb);
    free(ra); free(rb);
    return (d > 1e-12) ? sab / d : 0.0;
}

/* Propagate speaker feeds to the 19 capsules the LONG way — an explicit per-sample sum over
 * speakers with a linear-interpolated fractional delay and 1/r — as an independent check on
 * valid_simulate's analytic collapse to one complex number per (capsule, tone). The hardware path
 * emits exactly these feeds into a real room; if the two disagree, the offline sweep is not
 * measuring what the rig would. */
static void propagate(const Layout* L, const float* feeds, const float mic[3], double c, double fs,
                      float* cap, uint32_t n) {
    float capd[ZYLIA_MICS][3], R;
    zylia_geometry(capd, &R);
    for (int j = 0; j < ZYLIA_MICS; ++j) {
        double mx = mic[0] + R*capd[j][0], my = mic[1] + R*capd[j][1], mz = mic[2] + R*capd[j][2];
        for (uint32_t t = 0; t < n; ++t) cap[(size_t)j*n + t] = 0.0f;
        for (uint32_t i = 0; i < L->count; ++i) {
            double dx = mx - L->speakers[i].pos[0];
            double dy = my - L->speakers[i].pos[1];
            double dz = mz - L->speakers[i].pos[2];
            double r = sqrt(dx*dx + dy*dy + dz*dz);
            if (r < 0.05) r = 0.05;
            double dl = r / c * fs;
            uint32_t d0 = (uint32_t)dl;
            double fr = dl - (double)d0;
            for (uint32_t t = d0 + 1; t < n; ++t) {
                double a = feeds[(size_t)i*n + (t - d0)];
                double b = feeds[(size_t)i*n + (t - d0 - 1)];
                cap[(size_t)j*n + t] += (float)(((1.0 - fr)*a + fr*b) / r);
            }
        }
    }
}

/* The seam the rig enters through: valid_speaker_feeds -> (play/record) -> valid_score must land on
 * the same answer as the offline valid_cell. Here the "play/record" is the explicit propagation
 * above, so this pins the two paths against each other without any hardware. */
static void test_feed_path(const Layout* L, double FS, double C) {
    enum { PRE = 1024, NN = 8192 };
    float* feeds = (float*)malloc(sizeof(float) * (size_t)BWA_CHANNELS * (NN + PRE));
    float* wide  = (float*)malloc(sizeof(float) * (size_t)ZYLIA_MICS  * (NN + PRE));
    float* cap   = (float*)malloc(sizeof(float) * (size_t)ZYLIA_MICS  * NN);
    if (!feeds || !wide || !cap) { printf("FAIL: out of memory\n"); ++fails; return; }

    const int pans[3] = { BWA_PAN_DBAP, BWA_PAN_SPCAP, BWA_PAN_VBAP };
    float mic[3] = { 0.5f, 1.6f, -0.3f };
    float src[3] = { L->ref[0] + 1.4f*0.6f, L->ref[1] + 1.4f*0.3f, L->ref[2] - 1.4f*0.74f };
    double worst = 0.0;
    for (int p = 0; p < 3; ++p) {
        CHECK(valid_speaker_feeds(L, pans[p], NULL, mic, src, FS, feeds, NN + PRE), "feeds build");
        propagate(L, feeds, mic, C, FS, wide, NN + PRE);
        /* drop the pre-roll: until the farthest speaker has arrived the field is incomplete, and
         * that transient is an artifact of starting the model, not something the rig would see */
        for (int j = 0; j < ZYLIA_MICS; ++j)
            memcpy(cap + (size_t)j*NN, wide + (size_t)j*(NN+PRE) + PRE, sizeof(float)*NN);

        ValidCell viaFeeds, viaSim;
        CHECK(valid_score(L, pans[p], NULL, 1, mic, src, cap, NN, FS, C, NULL, &viaFeeds), "score from a capture");
        CHECK(valid_cell(L, pans[p], NULL, 1, mic, src, FS, C, NN, &viaSim), "score from the simulation");
        if (viaFeeds.ok && viaSim.ok) {
            double d = (double)viaFeeds.measured[0]*viaSim.measured[0]
                     + (double)viaFeeds.measured[1]*viaSim.measured[1]
                     + (double)viaFeeds.measured[2]*viaSim.measured[2];
            double deg = acos(fmax(-1.0, fmin(1.0, d))) * 180.0 / 3.14159265358979;
            if (deg > worst) worst = deg;
            CHECK(deg < 2.0, "the feed path and the analytic path agree on the direction");
        } else {
            CHECK(0, "both paths resolve the cell");
        }
    }
    printf("[feed path    ] worst feeds-vs-analytic disagreement %.2f deg\n", worst);

    /* THE TWO ARMS NOW USE DIFFERENT PROPAGATION MODELS, and the physical-versus-phantom table
     * SUBTRACTS them, so any systematic difference between the models would land in that contrast
     * wearing the phantom's name. The phantom arm propagates real engine feeds with a cubic
     * fractional tap (a feed is no longer a scaled copy of one stimulus, so the exact phase-domain
     * model cannot apply); the reference arm keeps the exact model. Pin the pair: drive one speaker
     * alone, propagate its feed the explicit way, and require the answer to match the analytic
     * reference cell it will be differenced against. */
    {
        double worst_ref = 0.0, worst_comb = 0.0;
        int n = 0;
        for (uint32_t sp = 0; sp < L->count; sp += 5) {
            ValidCell viaFeeds, viaField;
            float tgt[3] = { L->speakers[sp].pos[0], L->speakers[sp].pos[1], L->speakers[sp].pos[2] };
            if (!valid_reference_feeds(L, (int)sp, FS, feeds, NN + PRE)) { CHECK(0, "reference feeds"); continue; }
            propagate(L, feeds, mic, C, FS, wide, NN + PRE);
            for (int j = 0; j < ZYLIA_MICS; ++j)
                memcpy(cap + (size_t)j*NN, wide + (size_t)j*(NN+PRE) + PRE, sizeof(float)*NN);
            if (!valid_score(L, 0, NULL, 0, mic, tgt, cap, NN, FS, C, NULL, &viaFeeds) ||
                !valid_reference_cell(L, (int)sp, mic, FS, C, NN, &viaField)) { CHECK(0, "reference cells"); continue; }
            if (!viaFeeds.ok || !viaField.ok) { CHECK(0, "both reference paths resolve"); continue; }
            double d = (double)viaFeeds.measured[0]*viaField.measured[0]
                     + (double)viaFeeds.measured[1]*viaField.measured[1]
                     + (double)viaFeeds.measured[2]*viaField.measured[2];
            double deg = acos(fmax(-1.0, fmin(1.0, d))) * 180.0 / 3.14159265358979;
            if (deg > worst_ref) worst_ref = deg;
            if (viaFeeds.comb_ok && viaField.comb_ok) {
                double dk = fabs((double)viaFeeds.comb_db - viaField.comb_db);
                if (dk > worst_comb) worst_comb = dk;
            }
            ++n;
        }
        printf("[feed path    ] reference arm, explicit propagation vs the analytic field: worst\n"
               "                %.2f deg, %.2f dB comb over %d speakers\n", worst_ref, worst_comb, n);
        CHECK(n >= 4, "the reference-arm model check runs on several speakers");
        CHECK(worst_ref < 1.0 && worst_comb < 0.5,
              "the two arms' propagation models agree, so the matched contrast between them is real");
    }
    free(feeds); free(wide); free(cap);
}

/* ---- the engine-rendered phantom arm ------------------------------------------------------------
 *
 * The phantom arm no longer scales a stimulus by a panner solve: it drives a real engine core and
 * takes the speaker bus, which is the only way the shipping knobs (dual-band, CAP, the spread modes,
 * decorrelation, the hole floor, tracked alignment) can reach a measurement at all. Two things have
 * to hold for that reroute to be trustworthy, and they are opposite claims:
 *
 *   1. With every knob OFF it must reproduce the old feeds. That is what says the numbers this
 *      harness has already reported are still the same numbers.
 *   2. With a knob ON it must move a measured number. Otherwise the sweep is decorative.
 *
 * Both are below. The second is reported per knob and asserted only where the physics is unambiguous.
 */

/* layout_default() is a cube GRID with unity trims and NO delays, so its arrivals are not
 * time-aligned anywhere — dref runs from 1.5 m to 2.6 m. A calibrated install is not like that, and
 * the difference matters for tracked alignment, which RE-REFERENCES an existing alignment rather than
 * creating one. This is the same grid with the per-speaker delay a calibration would have written:
 * arrivals coincide at the layout reference. */
static Layout make_aligned_grid(double fs, double c) {
    Layout L = layout_default();
    double d[BWA_CHANNELS], dmax = 0.0;
    for (uint32_t k = 0; k < L.count; ++k) {
        double dx = L.speakers[k].pos[0] - L.ref[0];
        double dy = L.speakers[k].pos[1] - L.ref[1];
        double dz = L.speakers[k].pos[2] - L.ref[2];
        d[k] = sqrt(dx*dx + dy*dy + dz*dz);
        if (d[k] > dmax) dmax = d[k];
    }
    uint32_t mx = 0;
    for (uint32_t k = 0; k < L.count; ++k) {
        L.speakers[k].delay_samples = (uint32_t)((dmax - d[k]) / c * fs + 0.5);
        if (L.speakers[k].delay_samples > mx) mx = L.speakers[k].delay_samples;
    }
    L.max_delay_samples = mx;
    return L;
}

/* A BARREL: 8 perimeter positions x 3 heights, no top or bottom cap — the CAVE array's real shape,
 * open at both poles, and the only geometry on which the hole-aware spread floor does anything. The
 * same construction dsp_test and rt_feature_test use, so the three agree on what a hole is. */
static Layout make_barrel(void) {
    Layout L;
    memset(&L, 0, sizeof L);
    const float rad = 1.5f, ys[3] = { 0.5f, 1.5f, 2.5f };
    uint32_t k = 0;
    for (int ri = 0; ri < 3; ++ri)
        for (int a = 0; a < 8; ++a, ++k) {
            const float th = (float)a * 0.785398163f;
            L.speakers[k].pos[0] = rad * cosf(th);
            L.speakers[k].pos[1] = ys[ri];
            L.speakers[k].pos[2] = rad * sinf(th);
            L.speakers[k].gain_lin = 1.f;
        }
    L.count = k;
    layout_compute_ref(&L);
    L.rolloff_r     = 0.7f;
    L.spcap_focus   = layout_derive_spcap_focus(&L);
    L.spcap_density = BWA_SPCAP_DENSITY_DEFAULT;
    L.atten_ref_m   = 1.f;
    L.atten_rolloff = 1.f;
    L.atten_min_lin = 0.01f;
    return L;
}

/* CLAIM 1: with every knob off, the engine render reproduces the pre-engine feed builder.
 *
 * Not bit-identical, and it cannot be. The engine multiplies a float stimulus sample by a float gain
 * and then by align.c's float trim; the direct builder folds gain and trim into one double and
 * multiplies once. Same panner solve, same trim, same integer delay, different rounding — so the
 * disagreement is float epsilon on the product, not a difference in what is rendered. Anything
 * larger means something in the render path is no longer a plain gain. */
static void test_engine_vs_direct(const Layout* L, double FS) {
    enum { FN = 4096 };
    const int pans[3] = { BWA_PAN_DBAP, BWA_PAN_SPCAP, BWA_PAN_VBAP };
    float* fe = (float*)malloc(sizeof(float) * (size_t)BWA_CHANNELS * FN);
    float* fd = (float*)malloc(sizeof(float) * (size_t)BWA_CHANNELS * FN);
    if (!fe || !fd) { printf("FAIL: out of memory\n"); ++fails; free(fe); free(fd); return; }
    float mic[3] = { 0.55f, 1.5f, -0.35f };
    float src[3] = { L->ref[0] + 1.4f*0.6f, L->ref[1] + 1.4f*0.3f, L->ref[2] - 1.4f*0.74f };
    double worst = 0.0;
    for (int p = 0; p < 3; ++p) {
        CHECK(valid_speaker_feeds(L, pans[p], NULL, mic, src, FS, fe, FN), "engine feeds build");
        CHECK(valid_speaker_feeds_direct(L, pans[p], mic, src, FS, 0.f, 0.f, fd, FN),
              "direct feeds build");
        double peak = 0.0, diff = 0.0;
        for (size_t i = 0; i < (size_t)L->count * FN; ++i) {
            double a = fabs((double)fd[i]);
            double e = fabs((double)fe[i] - (double)fd[i]);
            if (a > peak) peak = a;
            if (e > diff) diff = e;
        }
        double rel = (peak > 0.0) ? diff / peak : 1.0;
        if (rel > worst) worst = rel;
        printf("[engine feeds ] %-5s peak %.5f, worst |engine - direct| %.3e  (%.2e of peak)\n",
               pan_name(pans[p]), peak, diff, rel);
    }
    CHECK(worst < 1e-5, "the engine render reproduces the pre-engine feeds to float epsilon");
    free(fe); free(fd);
}

/* Render one cell and hand back its three numbers, so the knob checks below read as a table rather
 * than as thirty lines of boilerplate. Returns 0 if either estimator refused. */
static int cell_of(const Layout* L, int panner, const ValidRender* r, const float mic[3],
                   const float src[3], double FS, double C, uint32_t n,
                   double* miss, double* comb, double* psi) {
    ValidCell c;
    if (!valid_cell(L, panner, r, 1, mic, src, FS, C, n, &c) || !c.ok || !c.comb_ok) return 0;
    if (miss) *miss = c.miss_deg;
    if (comb) *comb = c.comb_db;
    if (psi)  *psi  = c.diffuseness;
    return 1;
}

/* The property that makes any of this quotable: the same inputs give the same numbers.
 *
 * Asserted twice over, because the two ways it can break are different. The engine core is CACHED
 * across cells, so a cell must not depend on which cell ran before it (filter state, the alignment
 * ring, the tracked-alignment glide). And a fresh process must agree with a warm one, which is what
 * valid_engine_release exercises. */
static void test_engine_determinism(const Layout* L, double FS, double C) {
    const uint32_t NC = 8192u;
    float mic[3]   = { 0.6f, 1.5f, -0.4f };
    float src[3]   = { L->ref[0] + 0.9f, L->ref[1] + 0.3f, L->ref[2] - 1.0f };
    float other[3] = { L->ref[0] - 1.1f, L->ref[1] - 0.2f, L->ref[2] + 0.6f };
    ValidRender r;
    valid_render_init(&r);
    r.dual_band = 1; r.tracked_align = 1; r.decorrelation = 1; r.spread = 0.4f;

    ValidCell a, b, c, d;
    CHECK(valid_cell(L, BWA_PAN_DBAP,  &r, 1, mic, src,   FS, C, NC, &a), "determinism cell a");
    CHECK(valid_cell(L, BWA_PAN_SPCAP, &r, 1, mic, other, FS, C, NC, &c), "an unrelated cell between");
    CHECK(valid_cell(L, BWA_PAN_DBAP,  &r, 1, mic, src,   FS, C, NC, &b), "determinism cell b");
    CHECK(a.miss_deg == b.miss_deg && a.comb_db == b.comb_db && a.diffuseness == b.diffuseness,
          "a cell does not depend on which cell ran before it (the engine core is cached)");
    printf("[determinism  ] warm cache: miss %.6f/%.6f  comb %.6f/%.6f\n",
           a.miss_deg, b.miss_deg, a.comb_db, b.comb_db);

    valid_engine_release();                         /* the cold-start path a fresh process takes */
    CHECK(valid_cell(L, BWA_PAN_DBAP, &r, 1, mic, src, FS, C, NC, &d), "determinism cell d");
    CHECK(a.miss_deg == d.miss_deg && a.comb_db == d.comb_db && a.diffuseness == d.diffuseness,
          "...and a cold engine renders the same cell as a warm one");
    printf("[determinism  ] cold start: miss %.6f  comb %.6f\n", d.miss_deg, d.comb_db);
}

/* CLAIM 2: each knob moves a measured number, and where the physics says WHICH WAY, assert it.
 *
 * Read the tracked-alignment rows against TWO layouts, because how much the knob buys depends on the
 * calibration under it and that is easy to miss with one. It re-references an existing alignment onto
 * the live head. On a layout whose per-speaker delays already equalize arrival at the reference
 * (make_aligned_grid), moving that reference onto an off-center listener restores coincidence there
 * and the comb falls all the way to the stimulus floor. On the built-in grid, whose trims are unity
 * and whose delays are ZERO, the comp still equalizes what geometry it can and the comb falls too,
 * but only part way — there was never an alignment there to move. Both are measured; only the
 * calibrated case is asserted, because only there is the size of the drop forced by the physics. */
static void test_engine_knobs(double FS, double C) {
    const uint32_t NC = 8192u;
    const Layout LG = layout_default();
    const Layout LA = make_aligned_grid(FS, C);
    const float  off[3] = { 0.7f, 1.5f, -0.2f };        /* an off-center listener: where it can matter */
    float src[3] = { LA.ref[0] + 1.4f*0.6f, LA.ref[1] + 1.4f*0.2f, LA.ref[2] - 1.4f*0.77f };

    printf("\n  tracked alignment, DBAP, tracked solve (the listener IS the solve point)\n");
    printf("  %-26s %10s %10s %10s %10s\n", "layout / listener", "miss off", "miss on", "comb off", "comb on");
    ValidRender base, ta;
    valid_render_init(&base);
    valid_render_init(&ta); ta.tracked_align = 1;

    double m0, m1, k0, k1;
    /* the calibrated case: delays that align at the reference, listener elsewhere */
    if (cell_of(&LA, BWA_PAN_DBAP, &base, off, src, FS, C, NC, &m0, &k0, NULL) &&
        cell_of(&LA, BWA_PAN_DBAP, &ta,   off, src, FS, C, NC, &m1, &k1, NULL)) {
        printf("  %-26s %10.2f %10.2f %10.2f %10.2f\n", "aligned grid / off-center", m0, m1, k0, k1);
        CHECK(k1 < k0 - 0.2,
              "tracked alignment LOWERS comb depth off-center on a time-aligned array");
    } else CHECK(0, "the aligned-grid tracked-alignment cells resolve");

    /* the control: at the layout reference the comp is identity, so nothing may move */
    double r0, r1, c0, c1;
    if (cell_of(&LA, BWA_PAN_DBAP, &base, LA.ref, src, FS, C, NC, &r0, &c0, NULL) &&
        cell_of(&LA, BWA_PAN_DBAP, &ta,   LA.ref, src, FS, C, NC, &r1, &c1, NULL)) {
        printf("  %-26s %10.2f %10.2f %10.2f %10.2f\n", "aligned grid / at the ref", r0, r1, c0, c1);
        /* A TIGHT null on purpose. At the reference the compensation is bitwise identity, so these
         * two cells must agree to rounding, not merely to a tolerance. The loose 0.5 deg / 0.15 dB
         * this used to carry was wide enough to pass while measuring a bug: the OFF cell ran right
         * after an ON cell on the cached core and opened its capture window while the aligner was
         * still gliding home, reading 3.38 against a true 3.21. ve_settle now waits out that glide
         * (g_ve.lc_dirty), and this bound is what stops the leak coming back unnoticed. */
        CHECK(fabs(c1 - c0) < 0.01 && fabs(r1 - r0) < 0.02,
              "...and is inert at the reference the trims were computed for (an exact null)");
    } else CHECK(0, "the at-reference tracked-alignment cells resolve");

    /* the built-in grid: nothing to re-reference, so the drop is partial. Reported, not asserted as a
     * size — the point is that how much this knob buys is a property of the CALIBRATION under it. */
    double g0, g1, gc0, gc1;
    if (cell_of(&LG, BWA_PAN_DBAP, &base, off, src, FS, C, NC, &g0, &gc0, NULL) &&
        cell_of(&LG, BWA_PAN_DBAP, &ta,   off, src, FS, C, NC, &g1, &gc1, NULL)) {
        printf("  %-26s %10.2f %10.2f %10.2f %10.2f   <- no layout delays to re-reference\n",
               "built-in grid / off-center", g0, g1, gc0, gc1);
        CHECK(fabs(g1 - g0) > 0.5 || fabs(gc1 - gc0) > 0.1,
              "tracked alignment still reaches the render on an un-delayed layout");
    } else CHECK(0, "the built-in-grid tracked-alignment cells resolve");

    /* ---- the hole-aware spread floor, on the one geometry that has holes ---- */
    {
        const Layout LB = make_barrel();
        const float lis[3] = { 0.25f, 1.4f, -0.15f };    /* seated ear height, slightly off the axis */
        /* Straight down FROM THE LISTENER: the barrel's open nadir, where the hull closes the hole
         * with a triangle of distant speakers and the render is a split image rather than a phantom.
         * The bearing has to be taken from the LISTENER, not from the layout reference — hole.c
         * derives its gap against the tracked listener, and a bearing measured from the reference
         * lands inside the knee here and floors at exactly 0. */
        float low[3] = { lis[0], lis[1] - 1.0f, lis[2] };
        /* Swept, not toggled, and that is the finding. `strength` scales a floor the geometry
         * derives, and this barrel's nadir gap (59 deg against a 33.7 deg knee) derives only 0.45 at
         * strength 1.0. Both measured numbers move the way the feature claims — comb down, the image
         * more diffuse — but at 1.0 the diffuseness change is smaller than the scatter between
         * neighboring bearings, so only the comb column resolves it. At 2.0 (the clamp, an
         * exaggerated width) both do. Asserted accordingly: comb at every strength, diffuseness only
         * where the floor is decisive. If you A/B this knob on the rig, read the comb column. */
        const float STR[2] = { 1.0f, 2.0f };
        ValidRender hoff;
        valid_render_init(&hoff);
        double hm0, hk0, hp0;
        if (cell_of(&LB, BWA_PAN_VBAP, &hoff, lis, low, FS, C, NC, &hm0, &hk0, &hp0)) {
            printf("\n  hole-aware spread floor, barrel (24 spk, open poles), source at the listener's nadir\n");
            printf("  %-26s %10s %10s %12s\n", "VBAP, tracked solve", "miss deg", "comb dB", "diffuseness");
            printf("  %-26s %10.2f %10.2f %12.3f\n", "floor off", hm0, hk0, hp0);
            double hp_last = hp0;
            for (int i = 0; i < 2; ++i) {
                ValidRender hon;
                valid_render_init(&hon); hon.hole_spread = STR[i];
                double hm1, hk1, hp1;
                char row[32];
                if (!cell_of(&LB, BWA_PAN_VBAP, &hon, lis, low, FS, C, NC, &hm1, &hk1, &hp1)) {
                    CHECK(0, "the barrel hole-spread cells resolve"); continue;
                }
                snprintf(row, sizeof row, "strength %.1f", (double)STR[i]);
                printf("  %-26s %10.2f %10.2f %12.3f\n", row, hm1, hk1, hp1);
                CHECK(hk1 < hk0, "the hole floor lowers comb depth (fewer near-equal coherent copies)");
                hp_last = hp1;
            }
            CHECK(hp_last > hp0,
                  "...and at a decisive floor it also reads as a more diffuse image, which is the claim");
        } else CHECK(0, "the barrel hole-spread reference cell resolves");

        /* the negative control the feature's own design promises: a source aimed at a speaker's own
         * bearing is inside the knee, so the floor is 0 and the render must be untouched */
        float atspk[3] = { LB.speakers[8].pos[0], LB.speakers[8].pos[1], LB.speakers[8].pos[2] };
        ValidRender hfull;
        valid_render_init(&hfull); hfull.hole_spread = 2.0f;
        double am0, am1, ak0, ak1;
        if (cell_of(&LB, BWA_PAN_VBAP, &hoff,  lis, atspk, FS, C, NC, &am0, &ak0, NULL) &&
            cell_of(&LB, BWA_PAN_VBAP, &hfull, lis, atspk, FS, C, NC, &am1, &ak1, NULL))
            CHECK(am0 == am1 && ak0 == ak1,
                  "the hole floor is exactly inert at a covered bearing (0 floor, identical render)");
    }

    /* ---- every remaining knob: does it reach the feeds, and what does it move? ---- */
    {
        enum { FN = 4096 };
        float* f0 = (float*)malloc(sizeof(float) * (size_t)BWA_CHANNELS * FN);
        float* f1 = (float*)malloc(sizeof(float) * (size_t)BWA_CHANNELS * FN);
        if (!f0 || !f1) { printf("FAIL: out of memory\n"); ++fails; free(f0); free(f1); return; }
        /* a WIDE source: the spread modes and decorrelation act on the wide part, so with a point
         * source they have nothing to do and a "no effect" reading would mean nothing */
        float near_src[3] = { off[0] + 0.45f, off[1] + 0.1f, off[2] - 0.2f };
        ValidRender b2;
        valid_render_init(&b2); b2.spread = 0.5f;
        struct { const char* name; ValidRender r; const float* src; } K[6];
        int nk = 0;
        for (int i = 0; i < 6; ++i) { K[i].r = b2; K[i].src = src; K[i].name = ""; }
        K[nk].name = "dual-band";       K[nk].r.dual_band = 1;                          ++nk;
        K[nk].name = "dual+CAP";        K[nk].r.dual_band = 1; K[nk].r.cap = 1;         ++nk;
        K[nk].name = "spread MDAP";     K[nk].r.spread_mode = 1;                        ++nk;
        K[nk].name = "spread spectral"; K[nk].r.spread_mode = 2;                        ++nk;
        K[nk].name = "decorrelation";   K[nk].r.decorrelation = 1;                      ++nk;
        /* 2 m, not 1. The floor is 1 - dist/radius, and at this source distance a 1 m radius derives
         * 0.498, which the source's own 0.5 width already covers — so the knob would measure nothing
         * and read exactly like a broken knob. Worth knowing before A/Bing it on the rig. */
        K[nk].name = "near spread";     K[nk].r.near_spread = 2.0f; K[nk].src = near_src; ++nk;

        printf("\n  the remaining knobs, DBAP on the built-in grid, source width 0.5\n");
        printf("  %-18s %9s %9s %9s %9s %9s %9s\n", "knob",
               "miss off", "miss on", "comb off", "comb on", "psi off", "psi on");
        for (int i = 0; i < nk; ++i) {
            double q0, q1, w0, w1, p0, p1;
            ValidRender ref = b2;
            if (!cell_of(&LG, BWA_PAN_DBAP, &ref,    off, K[i].src, FS, C, NC, &q0, &w0, &p0) ||
                !cell_of(&LG, BWA_PAN_DBAP, &K[i].r, off, K[i].src, FS, C, NC, &q1, &w1, &p1)) {
                CHECK(0, "the knob cells resolve"); continue;
            }
            printf("  %-18s %9.2f %9.2f %9.2f %9.2f %9.3f %9.3f\n",
                   K[i].name, q0, q1, w0, w1, p0, p1);
            /* The claim asserted is REACHABILITY, not a direction: which way dual-band or a spread
             * mode moves a single-point intensity vector is a result about this array, and asserting
             * it would bake today's answer into a regression test. What must hold is that the knob
             * changes the speaker feeds at all — a flag that swept nothing was the whole reason for
             * routing this arm through the engine. */
            CHECK(valid_speaker_feeds(&LG, BWA_PAN_DBAP, &ref,    off, K[i].src, FS, f0, FN) &&
                  valid_speaker_feeds(&LG, BWA_PAN_DBAP, &K[i].r, off, K[i].src, FS, f1, FN),
                  "knob feeds build");
            CHECK(memcmp(f0, f1, sizeof(float) * (size_t)LG.count * FN) != 0,
                  "the knob reaches the speaker feeds");
        }
        free(f0); free(f1);
    }
    printf("\n");
}

/* ---- the SPCAP focus knob, made measurable -----------------------------------------------------
 *
 * Focus is the lobe sharpness in ((1+cos)/2)^focus, and until now it was judgeable only by ear. What
 * it trades is the NUMBER OF SPEAKERS carrying a source, and a phantom's speakers radiate coherent
 * copies of one signal, so the copies interfere and the render combs. No direction estimator can see
 * that; zylia_comb_depth is what reads it.
 *
 * WHERE THE SWEEP HAS POWER, AND WHERE IT DOES NOT. This is the finding, and it is not the obvious
 * one. Two target populations behave completely differently:
 *
 *  - A source AT A SPEAKER'S OWN POSITION. Tighten the lobe far enough and the panner collapses onto
 *    that one real speaker: one copy, nothing to interfere with, and the comb falls all the way to the
 *    physical floor. Loosen it and the whole array joins in. So the sweep runs its full range here and
 *    every step of it is measurable. It is also exactly the population the harness already renders for
 *    the physical reference arm, so the excess over a REAL source is available cell for cell.
 *  - An ARBITRARY grid direction. A tight lobe collapses onto nothing, because the direction falls
 *    BETWEEN speakers: the tightest achievable render is two near-equal copies, and two equal copies
 *    null completely. So comb depth does not fall away at high focus there, and the sweep comes out
 *    weak and non-monotone. Reported below, never asserted.
 *
 * So the assertions live on the speaker-coincident arm, where the physics is unambiguous, and the grid
 * arm is printed for a human to read. That split follows this file's rule: assert the machinery and
 * what geometry forces, report the result.
 *
 * Every number is read as an EXCESS over one speaker driven alone. An absolute comb depth also
 * contains the stimulus's line structure, the analysis, and (on hardware) the room, and only the
 * excess is attributable to panning. */
static void test_comb_sweep(const Layout* L, double FS, double C) {
    const uint32_t NC = 8192u;                    /* zylia_comb_depth's frame; NCAP is too short */
    enum { NF = 5, NSPK = BWA_CHANNELS, NDIR = 12 };
    const float FOCUS[NF] = { 2.0f, 8.0f, 16.0f, 32.0f, 64.0f };
    float mic[3] = { 0.55f, 1.5f, -0.35f };       /* off the sweet spot: the case that matters */

    /* ---- arm 1: sources at speaker positions, against the same speaker driven alone ---- */
    static double comb[NF][NSPK], refc[NSPK], qall[NF * NSPK];
    int n = 0, nq = 0;
    float worst_q = 1.0f;
    for (uint32_t sp = 0; sp < L->count; ++sp) {
        ValidCell rc;
        if (!valid_reference_cell(L, (int)sp, mic, FS, C, NC, &rc) || !rc.comb_ok) continue;
        CHECK(rc.render.focus == 0.f && rc.render.density == 0.f, "a reference cell carries no panner tuning");
        float src[3] = { L->speakers[sp].pos[0], L->speakers[sp].pos[1], L->speakers[sp].pos[2] };
        double got[NF];
        int all = 1;
        for (int f = 0; f < NF; ++f) {
            ValidCell c;
            ValidRender rf; valid_render_init(&rf); rf.focus = FOCUS[f];
            if (!valid_cell(L, BWA_PAN_SPCAP, &rf, 1, mic, src, FS, C, NC, &c) || !c.comb_ok) {
                all = 0; break;
            }
            CHECK(c.render.focus == FOCUS[f], "the cell records the focus it rendered at");
            CHECK(c.comb_q >= 0.f && c.comb_q <= 1.f, "the comb quality is a 0..1 figure");
            qall[nq++] = c.comb_q;
            if (c.comb_q < worst_q) worst_q = c.comb_q;
            got[f] = c.comb_db;
        }
        if (!all) continue;
        refc[n] = rc.comb_db;
        for (int f = 0; f < NF; ++f) comb[f][n] = got[f];
        ++n;
    }
    CHECK(n >= 8, "most speakers yield a full focus sweep");
    if (n < 8) return;

    double floor_db = valid_median(refc, n);
    printf("\n  SPCAP focus sweep on SPEAKER-COINCIDENT sources, listener (%.2f, %.2f, %.2f)\n",
           mic[0], mic[1], mic[2]);
    printf("  %8s %10s %14s %22s\n", "focus", "comb dB", "over real", "matched-cell CI");
    for (int f = 0; f < NF; ++f) {
        double md = 0, lo = 0, hi = 0;
        CHECK(valid_contrast(refc, comb[f], n, 2000, 909u, &md, &lo, &hi), "the excess contrast runs");
        CHECK(lo <= md && md <= hi, "the bootstrap interval brackets its own estimate");
        printf("  %8.1f %10.2f %14.2f   [%+6.2f, %+6.2f]%s\n",
               FOCUS[f], valid_median(comb[f], n), md, lo, hi, (lo > 0.0) ? " *" : "");
    }
    printf("  one speaker driven alone: %.2f dB over %d speakers   (* = excess excludes zero)\n",
           floor_db, n);
    /* Quality is a per-cell diagnostic, not a contract: a handful of directions genuinely put the 19
     * capsules at more scattered points of the interference field, and the mean over them is still
     * the answer. So the TYPICAL cell has to be well determined, and the worst is only reported. */
    printf("  comb quality: median %.2f, worst %.2f over %d cells\n",
           valid_median(qall, nq), worst_q, nq);
    CHECK(valid_median(qall, nq) > 0.5, "a typical cell's comb depth is well determined");

    /* The physics, asserted as an ordering rather than as dB. A source sitting on a speaker is the one
     * case where a tight enough lobe really is a single coherent copy, so the sweep has to run from
     * "the whole array interferes" down to "the floor". */
    for (int f = 1; f < NF; ++f)
        CHECK(valid_median(comb[f], n) < valid_median(comb[f-1], n),
              "a tighter SPCAP focus combs less on a speaker-coincident source");
    CHECK(valid_median(comb[0], n) > floor_db + 3.0,
          "a loose lobe combs well above the physical floor");
    CHECK(valid_median(comb[NF-1], n) < floor_db + 1.5,
          "...and a tight enough lobe collapses onto the real speaker and stops combing");

    {   /* the matched-cell contrast, the same paired arithmetic every other claim here uses */
        double md = 0, lo = 0, hi = 0;
        CHECK(valid_contrast(comb[0], comb[NF-1], n, 2000, 555u, &md, &lo, &hi), "focus contrast runs");
        printf("  matched-cell contrast (focus %.0f - focus %.0f): %+.2f dB  CI [%+.2f, %+.2f]\n",
               FOCUS[NF-1], FOCUS[0], md, lo, hi);
        CHECK(hi < 0.0, "two well-separated focus values measure differently, interval excluding zero");
    }

    /* ---- arm 2: arbitrary grid directions, reported only ---- */
    {
        static float dir[NDIR][3];
        static double gc[NF][NDIR], gm[NF][NDIR];
        float el0 = 0.0f;
        valid_target_grid(NDIR, &el0, 1, dir, NDIR);
        int ng = 0;
        for (int t = 0; t < NDIR; ++t) {
            float src[3] = { L->ref[0] + 1.4f*dir[t][0], L->ref[1] + 1.4f*dir[t][1],
                             L->ref[2] + 1.4f*dir[t][2] };
            double c1[NF], m1[NF];
            int all = 1;
            for (int f = 0; f < NF; ++f) {
                ValidCell c;
                ValidRender rf; valid_render_init(&rf); rf.focus = FOCUS[f];
                if (!valid_cell(L, BWA_PAN_SPCAP, &rf, 1, mic, src, FS, C, NC, &c) ||
                    !c.comb_ok || !c.ok) { all = 0; break; }
                c1[f] = c.comb_db; m1[f] = c.miss_deg;
            }
            if (!all) continue;
            for (int f = 0; f < NF; ++f) { gc[f][ng] = c1[f]; gm[f][ng] = m1[f]; }
            ++ng;
        }
        CHECK(ng >= NDIR / 2, "most grid directions sweep");
        printf("  same sweep on ARBITRARY grid directions (no speaker to collapse onto)\n");
        printf("  %8s %10s %14s %10s\n", "focus", "comb dB", "over real", "miss deg");
        for (int f = 0; f < NF && ng; ++f)
            printf("  %8.1f %10.2f %14.2f %10.1f\n", FOCUS[f], valid_median(gc[f], ng),
                   valid_median(gc[f], ng) - floor_db, valid_median(gm[f], ng));
        printf("  Weak and non-monotone by construction: between speakers the tightest render is two\n"
               "  near-equal copies, and two equal copies null completely. Do not read focus off this.\n");
    }

    /* ---- the <= 0 sentinel, and inertness where focus has no meaning ---- */
    {
        float src[3] = { L->ref[0] + 0.9f, L->ref[1] + 0.3f, L->ref[2] - 1.0f };
        ValidCell a, b;
        ValidRender ra, rb;
        valid_render_init(&ra);
        valid_render_init(&rb); rb.focus = L->spcap_focus; rb.density = L->spcap_density;
        CHECK(valid_cell(L, BWA_PAN_SPCAP, &ra, 1, mic, src, FS, C, NC, &a) &&
              valid_cell(L, BWA_PAN_SPCAP, &rb, 1, mic, src, FS, C, NC, &b),
              "sentinel cells render");
        CHECK(a.render.focus == L->spcap_focus && a.render.density == L->spcap_density,
              "focus/density <= 0 resolves to the LAYOUT's own values, not a stand-in default");
        CHECK(a.comb_db == b.comb_db && a.miss_deg == b.miss_deg,
              "...and renders identically to passing them explicitly");

        /* DBAP has no lobe, so focus must not reach it at all. If this ever fails, focus has leaked
         * somewhere it does not belong. */
        ValidCell d1, d2;
        ValidRender rlo, rhi;
        valid_render_init(&rlo); rlo.focus =  2.0f;
        valid_render_init(&rhi); rhi.focus = 64.0f;
        CHECK(valid_cell(L, BWA_PAN_DBAP, &rlo, 1, mic, src, FS, C, NC, &d1) &&
              valid_cell(L, BWA_PAN_DBAP, &rhi, 1, mic, src, FS, C, NC, &d2),
              "DBAP cells render at two focus values");
        CHECK(d1.comb_db == d2.comb_db && d1.miss_deg == d2.miss_deg, "focus is inert under DBAP");

        /* ...and the FEEDS path honors it too, so the hardware arm sweeps the same knob the offline
         * arm does. Without this the rig would silently measure the derived default forever. */
        enum { FN = 4096 };
        float* f1 = (float*)malloc(sizeof(float) * (size_t)BWA_CHANNELS * FN);
        float* f2 = (float*)malloc(sizeof(float) * (size_t)BWA_CHANNELS * FN);
        if (f1 && f2) {
            CHECK(valid_speaker_feeds(L, BWA_PAN_SPCAP, &rlo, mic, src, FS, f1, FN) &&
                  valid_speaker_feeds(L, BWA_PAN_SPCAP, &rhi, mic, src, FS, f2, FN),
                  "feeds build at two focus values");
            CHECK(memcmp(f1, f2, sizeof(float) * (size_t)L->count * FN) != 0,
                  "the hardware feed path sweeps focus too, not just the simulated field");
        }
        free(f1); free(f2);

        /* the rE proxy takes the tuning as well, so the optimizer comparison is made at the tuning
         * that will ship rather than at whatever the geometry implies */
        float re1 = 0.f, re2 = 0.f, sp1 = 0.f, sp2 = 0.f;
        CHECK(valid_re_proxy(L, BWA_PAN_SPCAP, mic, mic, src,  2.0f, 0.f, &re1, &sp1) &&
              valid_re_proxy(L, BWA_PAN_SPCAP, mic, mic, src, 64.0f, 0.f, &re2, &sp2),
              "the rE proxy runs at two focus values");
        CHECK(sp1 != sp2, "the rE proxy follows focus too");
    }
    printf("\n");
}

int main(void) {
    test_stats();
    test_grid();

    const double FS = 48000.0, C = 343.0;
    Layout L = layout_default();
    printf("[layout       ] %u speakers, sweet spot (%.2f, %.2f, %.2f)\n",
           L.count, L.ref[0], L.ref[1], L.ref[2]);
    test_feed_path(&L, FS, C);
    test_engine_vs_direct(&L, FS);
    test_engine_determinism(&L, FS, C);
    test_engine_knobs(FS, C);
    test_comb_sweep(&L, FS, C);

    float elev[NEL] = { -25.0f, 0.0f, 25.0f };
    static float tg[NTGT][3];
    valid_target_grid(NAZ, elev, NEL, tg, NTGT);

    /* Listener set. AXIS separates the two ways of being off the calibrated point, because they are
     * not the same problem: 1 = displaced in the plane, 2 = displaced in HEIGHT (a seated or tall
     * listener against an array whose speakers are mostly overhead, and against alignment delays
     * computed for one reference height), 3 = both at once. */
    const float EY = 1.5f;                       /* the calibrated ear height (= L.ref[1]) */
    struct { float p[3]; const char* name; int axis; } LIS[] = {
        {{  0.0f, EY,        0.0f }, "sweet spot     ", 0},
        {{  0.7f, EY,        0.0f }, "right   +0.7 m ", 1},
        {{ -0.7f, EY,        0.0f }, "left    -0.7 m ", 1},
        {{  0.0f, EY,        0.7f }, "back    +0.7 m ", 1},
        {{  0.0f, EY - 0.4f, 0.0f }, "seated  1.1 m  ", 2},
        {{  0.0f, EY + 0.4f, 0.0f }, "tall    1.9 m  ", 2},
        {{  0.6f, EY + 0.4f, 0.5f }, "tall + off-axis", 3},
    };
    const int NLIS = (int)(sizeof LIS / sizeof LIS[0]);
    static float lispos[16][3];
    for (int i = 0; i < NLIS; ++i) memcpy(lispos[i], LIS[i].p, sizeof LIS[i].p);

    const int panners[NPAN] = { BWA_PAN_DBAP, BWA_PAN_SPCAP, BWA_PAN_VBAP };
    const float RADIUS = 1.4f;                   /* sources inside the array, clear of the listeners */

    ValidCell* tracked = (ValidCell*)malloc(sizeof(ValidCell) * NPAN * NLIS * NTGT);
    ValidCell* fixed   = (ValidCell*)malloc(sizeof(ValidCell) * NPAN * NLIS * NTGT);
    if (!tracked || !fixed) { printf("FAIL: out of memory\n"); return 1; }

    int nt = valid_run(&L, panners, NPAN, NULL, 1, lispos, NLIS, tg, NTGT, RADIUS, FS, C, NCAP, tracked);
    int nf = valid_run(&L, panners, NPAN, NULL, 0, lispos, NLIS, tg, NTGT, RADIUS, FS, C, NCAP, fixed);
    CHECK(nt == NPAN * NLIS * NTGT && nf == nt, "the sweep fills every cell");

    int nok = 0;
    for (int i = 0; i < nt; ++i) if (tracked[i].ok) ++nok;
    printf("[sweep        ] %d cells per mode, %d resolved (%.0f%%)\n", nt, nok, 100.0 * nok / nt);
    CHECK(nok > nt * 9 / 10, "the estimator resolves almost every cell");

    /* ---- the shootout, reported ---- */
    printf("\n  median phantom miss (deg), simulated / anechoic — RENDERING term only\n");
    printf("  %-16s %-5s   %-18s %-18s %s\n", "listener", "axis", "tracked solve", "fixed sweet spot", "delta");
    static double ma[NTGT], mb[NTGT];
    for (int p = 0; p < NPAN; ++p) {
        printf("  --- %s ---\n", pan_name(panners[p]));
        for (int li = 0; li < NLIS; ++li) {
            int na = block_misses(tracked, NPAN, NLIS, NTGT, p, li, ma);
            int nb = block_misses(fixed,   NPAN, NLIS, NTGT, p, li, mb);
            if (na < 4 || nb < 4) continue;
            double mta = valid_median(ma, na), mtb = valid_median(mb, nb);
            double lo1, hi1, lo2, hi2;
            valid_bootstrap_ci(ma, na, 2000, 4242u, &lo1, &hi1);
            valid_bootstrap_ci(mb, nb, 2000, 4242u, &lo2, &hi2);
            printf("  %-16s %-5d   %5.1f [%4.1f,%5.1f]  %5.1f [%4.1f,%5.1f]  %+6.1f\n",
                   LIS[li].name, LIS[li].axis, mta, lo1, hi1, mtb, lo2, hi2, mtb - mta);
        }
    }

    /* ---- matched-cell contrasts: tracked vs fixed, paired over the same directions ---- */
    printf("\n  matched-cell contrast (fixed - tracked), median of paired differences\n");
    for (int p = 0; p < NPAN; ++p) {
        for (int li = 0; li < NLIS; ++li) {
            int w = 0;
            for (int t = 0; t < NTGT; ++t) {
                const ValidCell* a = &tracked[((size_t)p * NLIS + li) * NTGT + t];
                const ValidCell* b = &fixed  [((size_t)p * NLIS + li) * NTGT + t];
                if (a->ok && b->ok) { ma[w] = a->miss_deg; mb[w] = b->miss_deg; ++w; }
            }
            if (w < 8) continue;
            double md, lo, hi;
            if (!valid_contrast(ma, mb, w, 2000, 777u, &md, &lo, &hi)) continue;
            printf("  %-6s %-16s  %+6.1f  CI [%+.1f, %+.1f]%s\n",
                   pan_name(panners[p]), LIS[li].name, md, lo, hi,
                   (lo > 0.0 || hi < 0.0) ? "  *" : "");
        }
    }
    printf("  (* = interval excludes zero)\n\n");

    /* ---- IS THE OPTIMIZER CLIMBING THE RIGHT HILL? ----
     *
     * layout_tool optimizes speaker positions against the energy-vector direction error and the Frank
     * spread. Those are GAIN-domain proxies: a weighted sum of speaker directions, blind to what the
     * speakers' outputs do to each other at a point. The harness sums real acoustic pressure at 19
     * capsules. Correlating the two says whether the cheap thing the optimizer maximizes actually
     * tracks the expensive thing a measurement will report.
     *
     * Reported POOLED and WITHIN-POSITION, because that distinction is where the published analysis
     * of this exact question found its answer: a healthy-looking aggregate correlation turned out to
     * be carried entirely by differences BETWEEN elevation bands, with no skill at ranking directions
     * within one. A pooled number alone would have hidden that. Reported, never asserted — this is a
     * property of the array under test, not a contract. */
    {
        static double mm[16*NTGT], pe[16*NTGT], ps[16*NTGT];
        printf("  optimizer proxy vs acoustic measurement (Spearman rho, tracked solve)\n");
        printf("  %-6s %-10s %8s %8s   %s\n", "panner", "scope", "rE dir", "spread", "n");
        for (int p = 0; p < NPAN; ++p) {
            int all = 0;
            double wsum = 0.0, wn = 0.0;
            for (int li = 0; li < NLIS; ++li) {
                int n = 0;
                for (int t = 0; t < NTGT; ++t) {
                    const ValidCell* c = &tracked[((size_t)p * NLIS + li) * NTGT + t];
                    if (!c->ok) continue;
                    float src[3] = { L.ref[0] + RADIUS*tg[t][0],
                                     L.ref[1] + RADIUS*tg[t][1],
                                     L.ref[2] + RADIUS*tg[t][2] };
                    float re = 0.0f, sp = 0.0f;
                    if (!valid_re_proxy(&L, panners[p], lispos[li], lispos[li], src, 0.f, 0.f, &re, &sp)) continue;
                    mm[all + n] = c->miss_deg; pe[all + n] = re; ps[all + n] = sp; ++n;
                }
                if (n >= 8) {                       /* within-position: no between-position structure */
                    wsum += spearman(mm + all, pe + all, n) * n; wn += n;
                }
                all += n;
            }
            if (all >= 8) {
                printf("  %-6s %-10s %8.2f %8.2f   %d\n", pan_name(panners[p]), "pooled",
                       spearman(mm, pe, all), spearman(mm, ps, all), all);
                if (wn > 0)
                    printf("  %-6s %-10s %8.2f %8s   %d\n", "", "within-pos", wsum / wn, "-", (int)wn);
            }
        }
        printf("\n");
    }

    /* ---- the physical reference arm ----
     *
     * Driving one speaker alone is a real point source at a known position, so the estimator should
     * find it very accurately: this is close to the instrument floor, and it is the baseline a
     * phantom miss should be quoted AGAINST rather than as an absolute. Rendering a phantom at that
     * same speaker's position gives a genuine matched-cell contrast — same direction, same room,
     * same placement — which is the published physical-versus-phantom comparison with nothing moved.
     */
    {
        static double refm[BWA_CHANNELS], phm[BWA_CHANNELS];
        int nref = 0;
        float mic[3] = { 0.4f, EY, -0.3f };
        double worst_ref = 0.0;
        for (uint32_t sp = 0; sp < L.count; ++sp) {
            ValidCell rc, pc;
            if (!valid_reference_cell(&L, (int)sp, mic, FS, C, NCAP, &rc) || !rc.ok) continue;
            CHECK(rc.reference == 1 && rc.tgt == (int)sp, "a reference cell is tagged with its speaker");
            if (rc.miss_deg > worst_ref) worst_ref = rc.miss_deg;
            /* the matched phantom: a rendered source AT that speaker's own position */
            float src[3] = { L.speakers[sp].pos[0], L.speakers[sp].pos[1], L.speakers[sp].pos[2] };
            if (!valid_cell(&L, BWA_PAN_DBAP, NULL, 1, mic, src, FS, C, NCAP, &pc) || !pc.ok) continue;
            if (nref < 4)
                printf("[reference    ]   spk %2u: real %.2f  phantom-at-its-position %.2f deg\n",
                       sp, rc.miss_deg, pc.miss_deg);
            refm[nref] = rc.miss_deg; phm[nref] = pc.miss_deg; ++nref;
        }
        CHECK(nref >= 8, "most speakers yield a matched reference/phantom pair");

        double rmed = valid_median(refm, nref), pmed = valid_median(phm, nref);
        double md = 0, lo = 0, hi = 0;
        valid_contrast(refm, phm, nref, 2000, 31337u, &md, &lo, &hi);
        printf("[reference    ] physical %.2f deg (worst %.2f)   phantom %.2f deg   contrast %+.2f [%+.2f,%+.2f]\n",
               rmed, worst_ref, pmed, md, lo, hi);

        /* A single driven speaker must localize far better than anything panned. If it does not,
         * the fault is upstream of the renderer — layout, estimator or geometry — and every phantom
         * number in the run is resting on it. */
        CHECK(rmed < 1.0, "a directly driven speaker lands essentially on its surveyed position");
        CHECK(md > 0.0, "phantoms cost more than the physical source they are matched against");

        /* an out-of-range speaker index is refused rather than read off the end */
        ValidCell bad;
        CHECK(!valid_reference_cell(&L, (int)L.count, mic, FS, C, NCAP, &bad), "bad speaker index refused");
        CHECK(!valid_reference_cell(&L, -1, mic, FS, C, NCAP, &bad), "negative speaker index refused");
    }

    /* ---- stimulus selection, and the anechoic negative control ----
     *
     * Content dependence is a ROOM effect: a sustained tone sets up a standing-wave field and the
     * single-point intensity vector stops pointing at the source. Simulate is anechoic, so every
     * stimulus must localize about equally here. That makes this a CONTROL rather than a result — if
     * a tone ever localizes much worse in simulate, the fault is in the estimator, not in acoustics,
     * and any in-room content finding built on it would be an artifact.
     */
    {
        float mic[3] = { 0.35f, EY, -0.25f };
        float src[3] = { L.ref[0] + 1.4f*0.5f, L.ref[1] + 1.4f*0.2f, L.ref[2] - 1.4f*0.84f };
        double f_lo = 0, f_hi = 0;

        CHECK(valid_set_stimulus(VALID_STIM_BROADBAND, 0), "broadband selects");
        valid_get_stimulus_band(&f_lo, &f_hi);
        CHECK(f_lo > 300.0 && f_hi <= ZYLIA_FOA_FMAX + 1.0, "broadband band is the first-order band");
        ValidCell bb;
        CHECK(valid_cell(&L, BWA_PAN_DBAP, NULL, 1, mic, src, FS, C, 8192u, &bb) && bb.ok, "broadband cell");

        /* a tone above the array's first-order reach is refused WITH its frequency, not measured */
        CHECK(!valid_set_stimulus(VALID_STIM_TONE, 6000.0), "a 6 kHz tone is refused outright");
        CHECK(!valid_set_stimulus(VALID_STIM_TONE, 0.0), "a zero-frequency tone is refused");
        valid_get_stimulus_band(&f_lo, &f_hi);
        CHECK(f_hi <= ZYLIA_FOA_FMAX + 1.0, "a refused tone leaves the previous stimulus in place");

        /* THE CONTROL IS THE PHYSICAL SOURCE, NOT THE PHANTOM. A single driven speaker is one
         * coherent source with nothing to interfere with, so anechoically it must localize the same
         * whatever the content: that is a clean test of the ESTIMATOR across frequency. Asserting
         * the same of a phantom would be wrong, and measuring it here showed why (below). */
        CHECK(valid_set_stimulus(VALID_STIM_BROADBAND, 0), "broadband selects");
        ValidCell rbb;
        CHECK(valid_reference_cell(&L, 0, mic, FS, C, 8192u, &rbb) && rbb.ok, "reference broadband");
        double worst_ref_spread = 0.0, worst_ph = 0.0;
        const double hz[3] = { 500.0, 800.0, 1000.0 };
        for (int i = 0; i < 3; ++i) {
            CHECK(valid_set_stimulus(VALID_STIM_TONE, hz[i]), "an in-band tone selects");
            valid_get_stimulus_band(&f_lo, &f_hi);
            CHECK(f_lo < hz[i] && f_hi > hz[i], "the analysis band brackets the tone");
            ValidCell rc, tc;
            CHECK(valid_reference_cell(&L, 0, mic, FS, C, 8192u, &rc) && rc.ok, "reference tone cell");
            CHECK(valid_cell(&L, BWA_PAN_DBAP, NULL, 1, mic, src, FS, C, 8192u, &tc) && tc.ok, "phantom tone cell");
            double dr = fabs((double)rc.miss_deg - rbb.miss_deg);
            double dp = fabs((double)tc.miss_deg - bb.miss_deg);
            if (dr > worst_ref_spread) worst_ref_spread = dr;
            if (dp > worst_ph) worst_ph = dp;
            printf("[stimulus     ] %6.0f Hz: physical %.2f deg (broadband %.2f)   phantom %.2f deg (broadband %.2f)\n",
                   hz[i], rc.miss_deg, rbb.miss_deg, tc.miss_deg, bb.miss_deg);
        }

        /* The estimator is content-independent on a real source. If this ever fails, a content
         * finding anywhere else in the harness is an artifact of the analysis chain. */
        CHECK(worst_ref_spread < 1.0,
              "ANECHOIC + PHYSICAL source: content-independent, so the estimator is not the cause");

        /* Phantoms are NOT content-independent even anechoically, and that is a real result rather
         * than a fault: a phantom is a coherent multi-speaker sum, so its interference pattern is
         * frequency-dependent in free field. Broadband averages over that; one tone cannot. The
         * published study's anechoic control used a PHYSICAL loudspeaker, which has nothing to
         * interfere with, so it could not see this — the room is an ADDITIONAL mechanism on top,
         * not the only one. Reported, not asserted: the size is a property of the array. */
        printf("[stimulus     ] anechoic content spread: physical %.2f deg, phantom %.2f deg\n",
               worst_ref_spread, worst_ph);
        valid_set_stimulus(VALID_STIM_BROADBAND, 0);      /* restore for anything after */
    }

    /* ---- assertions: machinery and the geometrically necessary, not the result ---- */

    /* At the sweet spot the two modes solve at the SAME point, so they must agree cell for cell.
     * If they don't, the tracked/fixed plumbing is crossed somewhere. */
    for (int p = 0; p < NPAN; ++p) {
        double worst = 0.0;
        for (int t = 0; t < NTGT; ++t) {
            const ValidCell* a = &tracked[((size_t)p * NLIS + 0) * NTGT + t];
            const ValidCell* b = &fixed  [((size_t)p * NLIS + 0) * NTGT + t];
            if (a->ok && b->ok) { double d = fabs(a->miss_deg - b->miss_deg); if (d > worst) worst = d; }
        }
        CHECK(worst < 1e-3, "at the sweet spot, tracked and fixed are the same render");
    }

    /* A phantom rendered and measured at the sweet spot must actually land near its target — this is
     * the end-to-end sanity check on layout, panner solve, propagation model and estimator together. */
    for (int p = 0; p < NPAN; ++p) {
        int n = block_misses(tracked, NPAN, NLIS, NTGT, p, 0, ma);
        double m = valid_median(ma, n);
        printf("[sanity       ] %-5s at the sweet spot: median miss %.1f deg over %d directions\n",
               pan_name(panners[p]), m, n);
        CHECK(n > NTGT / 2 && m < 15.0, "a phantom at the sweet spot lands near its target");
    }

    /* Geometrically necessary: with a FIXED solve, moving the listener cannot improve things — the
     * gains were computed for a point the listener has left, while the true direction moved with
     * them. Asserted as a direction, not a magnitude. */
    for (int p = 0; p < NPAN; ++p) {
        int n0 = block_misses(fixed, NPAN, NLIS, NTGT, p, 0, ma);
        double m0 = valid_median(ma, n0);
        for (int li = 1; li < NLIS; ++li) {
            int n = block_misses(fixed, NPAN, NLIS, NTGT, p, li, mb);
            if (n < 4) continue;
            CHECK(valid_median(mb, n) >= m0 - 1.0,
                  "leaving the sweet spot never improves a fixed-solve render");
        }
    }

    free(tracked); free(fixed);
    printf("%s\n", fails ? "FAIL: phantom validation harness" : "PASS: phantom validation harness");
    return fails ? 1 : 0;
}
