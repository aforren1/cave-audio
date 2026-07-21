/*
 * valid_test.c — the phantom-localization harness, off-hardware.
 *
 * Two jobs. First, pin the machinery: the statistics are exact on known inputs, the grid builder
 * produces the directions it claims, and a cell scored at the sweet spot comes back sane (if it does
 * not, nothing below means anything). Second, RUN the shootout and report it — three panners, solved
 * either at the listener (tracked) or at the sweet spot (fixed), over a listener set that separates
 * horizontal displacement from HEIGHT.
 *
 * Height is its own axis on purpose. Off-centre-in-the-plane and off-centre-in-height are different
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
        CHECK(valid_speaker_feeds(L, pans[p], mic, src, FS, feeds, NN + PRE), "feeds build");
        propagate(L, feeds, mic, C, FS, wide, NN + PRE);
        /* drop the pre-roll: until the farthest speaker has arrived the field is incomplete, and
         * that transient is an artifact of starting the model, not something the rig would see */
        for (int j = 0; j < ZYLIA_MICS; ++j)
            memcpy(cap + (size_t)j*NN, wide + (size_t)j*(NN+PRE) + PRE, sizeof(float)*NN);

        ValidCell viaFeeds, viaSim;
        CHECK(valid_score(L, pans[p], 1, mic, src, cap, NN, FS, C, NULL, &viaFeeds), "score from a capture");
        CHECK(valid_cell(L, pans[p], 1, mic, src, FS, C, NN, &viaSim), "score from the simulation");
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
    free(feeds); free(wide); free(cap);
}

int main(void) {
    test_stats();
    test_grid();

    const double FS = 48000.0, C = 343.0;
    Layout L = layout_default();
    printf("[layout       ] %u speakers, sweet spot (%.2f, %.2f, %.2f)\n",
           L.count, L.ref[0], L.ref[1], L.ref[2]);
    test_feed_path(&L, FS, C);

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

    int nt = valid_run(&L, panners, NPAN, 1, lispos, NLIS, tg, NTGT, RADIUS, FS, C, NCAP, tracked);
    int nf = valid_run(&L, panners, NPAN, 0, lispos, NLIS, tg, NTGT, RADIUS, FS, C, NCAP, fixed);
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
     * capsules. Correlating the two says whether the cheap thing the optimizer maximises actually
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
                    if (!valid_re_proxy(&L, panners[p], lispos[li], lispos[li], src, &re, &sp)) continue;
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
