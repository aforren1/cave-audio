/*
 * zylia_test.c — single-position ZM-1 localization recovers a known speaker position (off-hardware).
 *
 * Synthesize the 19 per-capsule arrival times for a speaker at a known position (exact spherical
 * wavefront), then check: (1) zylia_doa recovers the direction within a degree, (2) zylia_localize
 * recovers the full position to sub-mm on exact data, (3) the DOA is latency-independent.
 */
#include "zylia.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++fails; } } while (0)

/* exact arrival time at each capsule for a source at `src`, array centred at `center`. */
static void synth(const float center[3], const double src[3], double latency, double c, double arr[ZYLIA_MICS]) {
    float dirs[ZYLIA_MICS][3]; float R; zylia_geometry(dirs, &R);
    for (int i = 0; i < ZYLIA_MICS; ++i) {
        double mx = center[0] + R*dirs[i][0], my = center[1] + R*dirs[i][1], mz = center[2] + R*dirs[i][2];
        double dx = mx - src[0], dy = my - src[1], dz = mz - src[2];
        arr[i] = sqrt(dx*dx + dy*dy + dz*dz) / c + latency;
    }
}

/* Coherent band-limited field at the 19 capsules for a source at `src`: a sum of tones, delayed
 * EXACTLY per capsule — a tone delays by a phase shift, so there is no interpolation error anywhere
 * in this forward model. It deliberately shares NOTHING with the frequency-domain estimators it
 * feeds: no spherical harmonics, no mode strengths, no FFT convention. See the long note at the
 * intensity-DOA tests for why that independence is the whole point. */
#define FIELD_N 8192
static void synth_field(const float center[3], const double src[3], double c, double fs,
                        float buf[ZYLIA_MICS][FIELD_N], uint32_t n, const double* phase, int nt) {
    float dirs[ZYLIA_MICS][3], R;
    zylia_geometry(dirs, &R);
    for (int ch = 0; ch < ZYLIA_MICS; ++ch) {
        double mx = center[0] + R*dirs[ch][0], my = center[1] + R*dirs[ch][1], mz = center[2] + R*dirs[ch][2];
        double dx = mx - src[0], dy = my - src[1], dz = mz - src[2];
        double tau = sqrt(dx*dx + dy*dy + dz*dz) / c;
        for (uint32_t i = 0; i < n; ++i) {
            double tt = (double)i / fs - tau, v = 0.0;
            for (int j = 0; j < nt; ++j) {
                double fj = 420.0 + (1150.0 - 420.0) * (double)j / (double)(nt - 1);
                v += sin(6.283185307179586 * fj * tt + phase[j]);
            }
            buf[ch][i] = (float)(v / nt);
        }
    }
}

/* arrivals for an EXPLICIT capsule table, so a test can move the array without touching the
 * installed one (the tracked-mount case: the physical geometry and the assumed geometry differ). */
static void synth_caps(const float caps[ZYLIA_MICS][3], const float center[3], const double src[3],
                       double c, double arr[ZYLIA_MICS]) {
    for (int i = 0; i < ZYLIA_MICS; ++i) {
        double mx = center[0] + caps[i][0], my = center[1] + caps[i][1], mz = center[2] + caps[i][2];
        double dx = mx - src[0], dy = my - src[1], dz = mz - src[2];
        arr[i] = sqrt(dx*dx + dy*dy + dz*dz) / c;
    }
}

/* great-circle angle (deg) between two unit vectors. */
static double ang_deg(const float a[3], const double b[3]) {
    double d = (double)a[0]*b[0] + (double)a[1]*b[1] + (double)a[2]*b[2];
    return acos(fmax(-1.0, fmin(1.0, d))) * 180.0 / 3.14159265358979;
}

/* ---- the capsule table is a vertex-up dodecahedron minus its nadir vertex ----
 *
 * Worth being clear about what this can and cannot do. Every OTHER test in this file synthesizes its
 * arrivals from zylia_geometry and then recovers them through zylia_geometry, so a wrong table cancels
 * exactly and they all pass regardless — they pin the algebra, not the array. These checks pin the
 * TABLE, by asserting the structure that makes it a dodecahedron rather than 19 arbitrary points.
 *
 * What still escapes: a PERMUTATION of the table (channel order) and a global ROTATION (which capsule
 * faces the device front) both preserve every property below. Neither is knowable off-hardware — they
 * get pinned at the rig, or measured by the capsule self-survey. See zylia.c.
 */
static void test_geometry(void) {
    float dirs[ZYLIA_MICS][3]; float R;
    zylia_geometry(dirs, &R);

    CHECK(R > 0.03f && R < 0.07f, "radius is a ~100 mm sphere");

    int unit = 1;
    for (int i = 0; i < ZYLIA_MICS; ++i) {
        double m = sqrt((double)dirs[i][0]*dirs[i][0] + (double)dirs[i][1]*dirs[i][1] + (double)dirs[i][2]*dirs[i][2]);
        if (fabs(m - 1.0) > 1e-5) unit = 0;
    }
    CHECK(unit, "all 19 capsule directions are unit vectors");

    /* elevation rings: 1 @ +90, 3 @ +48.19, 6 @ +19.47, 6 @ -19.47, 3 @ -48.19 (the absent 20th
     * dodecahedral vertex is the 1 @ -90). Any other split is not this solid. */
    const double EL_A = asin(sqrt(5.0) / 3.0) * 180.0 / 3.14159265358979;   /* 48.1897 */
    const double EL_B = asin(1.0 / 3.0)       * 180.0 / 3.14159265358979;   /* 19.4712 */
    const double ring[5] = { 90.0, EL_A, EL_B, -EL_B, -EL_A };
    const int    want[5] = {    1,    3,    6,     6,     3 };
    int got[5] = {0}, stray = 0;
    for (int i = 0; i < ZYLIA_MICS; ++i) {
        double el = asin(dirs[i][1] > 1.0 ? 1.0 : (dirs[i][1] < -1.0 ? -1.0 : dirs[i][1])) * 180.0 / 3.14159265358979;
        int hit = 0;
        for (int r = 0; r < 5; ++r) if (fabs(el - ring[r]) < 0.05) { ++got[r]; hit = 1; break; }
        if (!hit) ++stray;
    }
    CHECK(!stray, "every capsule lands on a dodecahedral elevation ring");
    int rings_ok = 1;
    for (int r = 0; r < 5; ++r) if (got[r] != want[r]) rings_ok = 0;
    CHECK(rings_ok, "ring populations are 1 / 3 / 6 / 6 / 3");
    printf("[geometry    ] R=%.3f m  rings %d/%d/%d/%d/%d @ el %+.2f %+.2f %+.2f %+.2f %+.2f\n",
           R, got[0], got[1], got[2], got[3], got[4], ring[0], ring[1], ring[2], ring[3], ring[4]);

    /* A dodecahedron's vertices are antipodally symmetric. Drop the nadir and EVERY capsule still has
     * its opposite in the table except the zenith, which is left unpaired. Two consequences, and the
     * second is the sharp one: the 18 paired vectors cancel, so the whole table sums to the zenith. */
    int unpaired = 0;
    for (int i = 0; i < ZYLIA_MICS; ++i) {
        int found = 0;
        for (int j = 0; j < ZYLIA_MICS; ++j) {
            double dot = (double)dirs[i][0]*dirs[j][0] + (double)dirs[i][1]*dirs[j][1] + (double)dirs[i][2]*dirs[j][2];
            if (dot < -0.9999) { found = 1; break; }
        }
        if (!found) ++unpaired;
    }
    CHECK(unpaired == 1, "exactly one capsule (the zenith) has no antipode — the nadir is the missing vertex");

    double sx = 0, sy = 0, sz = 0;
    for (int i = 0; i < ZYLIA_MICS; ++i) { sx += dirs[i][0]; sy += dirs[i][1]; sz += dirs[i][2]; }
    CHECK(fabs(sx) < 1e-4 && fabs(sy - 1.0) < 1e-4 && fabs(sz) < 1e-4,
          "the table sums to the zenith (0,1,0): the other 18 cancel in antipodal pairs");

    /* adjacent dodecahedral vertices subtend arccos(sqrt5/3) = 41.81 deg; nothing may be closer
     * (a duplicated or collapsed capsule would show up here and nowhere else). */
    double closest = 180.0;
    for (int i = 0; i < ZYLIA_MICS; ++i)
        for (int j = i + 1; j < ZYLIA_MICS; ++j) {
            double dot = (double)dirs[i][0]*dirs[j][0] + (double)dirs[i][1]*dirs[j][1] + (double)dirs[i][2]*dirs[j][2];
            double deg = acos(fmax(-1.0, fmin(1.0, dot))) * 180.0 / 3.14159265358979;
            if (deg < closest) closest = deg;
        }
    printf("[geometry    ] closest capsule pair %.3f deg (dodecahedral edge = 41.810)\n", closest);
    CHECK(fabs(closest - 41.8103) < 0.01, "closest pair is the dodecahedral edge angle, 41.81 deg");
}

/* ---- capsule self-survey ----
 *
 * This is the test the others can't be. Build a "real" array that differs from the built-in table in
 * exactly the ways a real ZM-1 on a real stand does — its channels PERMUTED (node i is not ASIO input
 * i), the whole thing ROTATED (nobody knows which capsule faces front, and nobody aimed the stand),
 * and each capsule nudged half a millimetre — then synthesize claps off the EXACT spherical wavefront
 * and check the survey recovers it. Both of those corruptions preserve every structural invariant
 * test_geometry() checks, and both are invisible to every other test in this file, because they all
 * synthesize and recover through the same table. Here they are the thing under test.
 *
 * The last check is the one that matters: the same arrivals, decoded against the BUILT-IN table, must
 * come out badly wrong. If they didn't, the survey wouldn't be buying anything.
 */
static void test_survey(void) {
    const double C = 343.0;
    enum { NOBS = 14 };

    /* the "real" array: table -> permute channels -> rotate -> perturb */
    float truth[ZYLIA_MICS][3];
    {
        float dirs[ZYLIA_MICS][3], R;
        zylia_set_capsules(NULL);                       /* build truth off the pristine table */
        zylia_geometry(dirs, &R);
        const double yaw = 0.7, pit = 0.3;              /* an arbitrary, un-aimed stand */
        const double cy = cos(yaw), sy = sin(yaw), cp = cos(pit), sp = sin(pit);
        unsigned rng = 4242u;
        for (int ch = 0; ch < ZYLIA_MICS; ++ch) {
            int node = (ch * 7 + 3) % ZYLIA_MICS;       /* gcd(7,19)=1 -> a genuine permutation */
            double x = R * dirs[node][0], y = R * dirs[node][1], z = R * dirs[node][2];
            double x1 =  cy * x + sy * z, z1 = -sy * x + cy * z;              /* yaw   */
            double y2 =  cp * y - sp * z1, z2 = sp * y + cp * z1;             /* pitch */
            double v[3] = { x1, y2, z2 };
            for (int a = 0; a < 3; ++a) {               /* +-0.5 mm of build tolerance */
                rng = rng * 1664525u + 1013904223u;
                v[a] += ((double)(int)(rng >> 9) / (double)(1 << 22) - 1.0) * 0.0005;
                truth[ch][a] = (float)v[a];
            }
        }
    }

    /* claps from NOBS spread positions (Fibonacci sphere at 2.5 m — deliberately including high and
     * low, which is what the solve needs and what a careless operator clapping in a ring would not
     * give it) */
    float src_m[NOBS][3];
    double arr[NOBS][ZYLIA_MICS];
    const double golden = 2.399963229728653, DIST = 2.5;
    for (int k = 0; k < NOBS; ++k) {
        double yy = 1.0 - 2.0 * ((double)k + 0.5) / (double)NOBS;
        double rr = sqrt(fmax(0.0, 1.0 - yy * yy)), th = golden * (double)k;
        src_m[k][0] = (float)(DIST * rr * cos(th));
        src_m[k][1] = (float)(DIST * yy);
        src_m[k][2] = (float)(DIST * rr * sin(th));

        /* EXACT spherical wavefront (the survey's linear seed assumes a plane one, and iterates the
         * difference away) + a per-clap offset standing in for latency / onset / tdoa's reference
         * channel. Both must wash out. */
        double t0 = 0.001 * (double)k + 0.0033;
        for (int i = 0; i < ZYLIA_MICS; ++i) {
            double dx = truth[i][0]-src_m[k][0], dy = truth[i][1]-src_m[k][1], dz = truth[i][2]-src_m[k][2];
            arr[k][i] = sqrt(dx*dx + dy*dy + dz*dz) / C + t0;
        }
    }

    float caps[ZYLIA_MICS][3], resid_us = 0, radius = 0, spread = 0;
    int ok = zylia_survey(src_m, arr, NOBS, C, caps, &resid_us, &radius, &spread);
    CHECK(ok, "survey solves from 14 spread claps");

    double worst = 0.0;
    for (int i = 0; i < ZYLIA_MICS; ++i) {
        double e = sqrt((double)(caps[i][0]-truth[i][0])*(caps[i][0]-truth[i][0]) +
                        (double)(caps[i][1]-truth[i][1])*(caps[i][1]-truth[i][1]) +
                        (double)(caps[i][2]-truth[i][2])*(caps[i][2]-truth[i][2]));
        if (e > worst) worst = e;
    }
    printf("[survey      ] worst capsule err %.3f mm  resid %.3f us  R=%.1f mm  spread %.2f\n",
           worst * 1000.0, resid_us, radius * 1000.0, spread);
    /* The residual is the real proof: 5 ns says the recovered geometry explains the arrivals to well
     * under a thousandth of a sample, i.e. the cloud's SHAPE is exact. The absolute positions carry a
     * little more error than that, and necessarily so: a translation of the whole cloud is invisible to
     * arrival times (see zylia.c), so the origin is pinned by assuming the capsules lie on a sphere —
     * and this truth array deliberately does NOT, having been perturbed by +-0.5 mm of build tolerance.
     * That non-sphericity is the entire budget for the sub-0.2 mm centre error below. It costs nothing
     * where it counts: DOA is translation-invariant, which is why it comes out exact. */
    CHECK(resid_us < 0.02f, "residual ~5 ns: the recovered shape explains the arrivals exactly");
    CHECK(worst < 3e-4, "every capsule within 0.3 mm — through a permutation, a rotation, and 0.5 mm of slop");
    CHECK(radius > 0.045f && radius < 0.053f, "recovered radius lands on the ~49 mm shell");
    CHECK(spread > 0.5f, "Fibonacci clap directions are well spread");

    /* the recovered array is the real one -> DOA works. The built-in table is NOT -> DOA is garbage.
     * Same arrivals both times; the only difference is which geometry decodes them. */
    const double D = 3.0;
    float probe[3] = { 0.48f, 0.60f, -0.64f };
    double m = sqrt((double)probe[0]*probe[0] + (double)probe[1]*probe[1] + (double)probe[2]*probe[2]);
    probe[0] /= (float)m; probe[1] /= (float)m; probe[2] /= (float)m;
    double src[3] = { D*probe[0], D*probe[1], D*probe[2] }, a2[ZYLIA_MICS];
    for (int i = 0; i < ZYLIA_MICS; ++i) {
        double dx = truth[i][0]-src[0], dy = truth[i][1]-src[1], dz = truth[i][2]-src[2];
        a2[i] = sqrt(dx*dx + dy*dy + dz*dz) / C + 0.0071;
    }
    float d_surv[3], d_table[3];
    zylia_set_capsules(caps);  CHECK(zylia_doa(a2, d_surv),  "DOA against the surveyed array");
    zylia_set_capsules(NULL);  CHECK(zylia_doa(a2, d_table), "DOA against the built-in table");

    double dot_s = d_surv[0]*probe[0]  + d_surv[1]*probe[1]  + d_surv[2]*probe[2];
    double dot_t = d_table[0]*probe[0] + d_table[1]*probe[1] + d_table[2]*probe[2];
    double deg_s = acos(fmax(-1.0, fmin(1.0, dot_s))) * 180.0 / 3.14159265358979;
    double deg_t = acos(fmax(-1.0, fmin(1.0, dot_t))) * 180.0 / 3.14159265358979;
    printf("[survey      ] DOA err: surveyed %.2f deg   built-in table %.1f deg\n", deg_s, deg_t);
    CHECK(deg_s < 1.0,  "surveyed geometry decodes the clap correctly");
    CHECK(deg_t > 10.0, "the built-in table does NOT — the permutation/rotation is what the survey buys");

    /* a per-observation constant is unknowable and must be irrelevant (it is why no sample-sync, no
     * known latency, and tdoa's arbitrary reference channel are all fine) */
    {
        double shifted[NOBS][ZYLIA_MICS];
        for (int k = 0; k < NOBS; ++k)
            for (int i = 0; i < ZYLIA_MICS; ++i) shifted[k][i] = arr[k][i] + 0.017 * (k + 1) - 0.004;
        float c2[ZYLIA_MICS][3];
        CHECK(zylia_survey(src_m, shifted, NOBS, C, c2, NULL, NULL, NULL), "survey solves on shifted arrivals");
        double w = 0.0;
        for (int i = 0; i < ZYLIA_MICS; ++i)
            for (int a = 0; a < 3; ++a) w = fmax(w, fabs((double)c2[i][a] - caps[i][a]));
        CHECK(w < 1e-9, "per-observation offsets (latency / onset / reference channel) cancel exactly");
    }

    /* the operator trap: claps in a horizontal ring are coplanar, the capsules' heights are then
     * unconstrained, and a solve that "succeeded" would be a flattened array. Refuse, don't guess. */
    {
        float flat[8][3]; double fa[8][ZYLIA_MICS]; float c3[ZYLIA_MICS][3]; float sp = -1.0f;
        for (int k = 0; k < 8; ++k) {
            double th = 6.2831853 * k / 8.0;
            flat[k][0] = (float)(2.5 * cos(th)); flat[k][1] = 0.0f; flat[k][2] = (float)(2.5 * sin(th));
            for (int i = 0; i < ZYLIA_MICS; ++i) fa[k][i] = arr[0][i];   /* contents irrelevant: it must refuse */
        }
        CHECK(!zylia_survey(flat, fa, 8, C, c3, NULL, NULL, &sp), "coplanar clap directions are refused");
        CHECK(sp >= 0.0f && sp < 0.05f, "...and the spread metric says why");
    }

    /* persistence: a survey is worthless if it doesn't survive a restart. Round-trip it and check the
     * SOLVE follows — reading the file back must install the geometry, not merely parse it. */
    {
        const char* path = "zylia_survey_test.json";
        char e[128] = {0};
        CHECK(zylia_survey_save(path, caps, resid_us, radius, spread, NOBS, NULL, e, sizeof e), "survey saves");

        zylia_set_capsules(NULL);                        /* wipe it, so a no-op load would be caught */
        CHECK(zylia_survey_load(path, NULL, e, sizeof e), "survey loads");

        float back[ZYLIA_MICS][3];
        zylia_capsules(back);
        double w = 0.0;
        for (int i = 0; i < ZYLIA_MICS; ++i)
            for (int a = 0; a < 3; ++a) w = fmax(w, fabs((double)back[i][a] - caps[i][a]));
        CHECK(w < 1e-6, "round-tripped capsules match to float precision");

        float d3[3];
        CHECK(zylia_doa(a2, d3), "DOA after load");
        double dot = d3[0]*probe[0] + d3[1]*probe[1] + d3[2]*probe[2];
        CHECK(acos(fmax(-1.0, fmin(1.0, dot))) * 180.0 / 3.14159265358979 < 1.0,
              "the LOADED survey drives the solve — persistence reaches zylia_doa, not just the parser");

        /* a mm-vs-m slip would put capsules 49 metres out and quietly destroy every direction */
        {
            float bad[ZYLIA_MICS][3];
            for (int i = 0; i < ZYLIA_MICS; ++i)
                for (int a = 0; a < 3; ++a) bad[i][a] = caps[i][a] * 1000.0f;
            CHECK(zylia_survey_save(path, bad, 0, 49, 1, NOBS, NULL, e, sizeof e), "save (bad units)");
            CHECK(!zylia_survey_load(path, NULL, e, sizeof e), "a mm-vs-m unit slip is rejected on load");
        }
        remove(path);
    }

    zylia_set_capsules(NULL);                            /* leave the global default installed */
}

int main(void) {
    const double C = 343.0, LAT = 0.0047;       /* arbitrary nonzero system latency */
    const float center[3] = { 0.1f, 1.2f, -0.3f };   /* array placed off-origin in the room */

    test_geometry();
    test_survey();

    struct { double pos[3]; const char* name; } cases[] = {
        {{  2.0,  1.2, -0.3 }, "right  (+X)"},
        {{  0.1,  1.2, -3.0 }, "front  (-Z)"},
        {{ -2.5,  2.0,  1.0 }, "back-left-up"},
        {{  0.1,  4.0, -0.3 }, "overhead (+Y)"},
        {{  1.0, -0.5, -2.0 }, "low diagonal"},
    };

    for (int t = 0; t < 5; ++t) {
        double arr[ZYLIA_MICS];
        synth(center, cases[t].pos, LAT, C, arr);

        double tx = cases[t].pos[0]-center[0], ty = cases[t].pos[1]-center[1], tz = cases[t].pos[2]-center[2];
        double td = sqrt(tx*tx + ty*ty + tz*tz); tx/=td; ty/=td; tz/=td;

        float doa[3];
        int ok = zylia_doa(arr, doa);
        double dot = doa[0]*tx + doa[1]*ty + doa[2]*tz;
        double deg = acos(fmax(-1.0, fmin(1.0, dot))) * 180.0 / 3.14159265358979;

        float pos[3], dist;
        int ok2 = zylia_localize(arr, center, LAT, C, pos, &dist);
        double perr = sqrt((pos[0]-cases[t].pos[0])*(pos[0]-cases[t].pos[0]) +
                           (pos[1]-cases[t].pos[1])*(pos[1]-cases[t].pos[1]) +
                           (pos[2]-cases[t].pos[2])*(pos[2]-cases[t].pos[2]));
        printf("[%-12s] dist=%.2f m  doa_err=%.3f deg  pos_err=%.4f mm\n",
               cases[t].name, dist, deg, perr * 1000.0);
        CHECK(ok  && deg  < 1.0,  "DOA within 1 deg from a single placement");
        CHECK(ok2 && perr < 1e-3, "localize within 1 mm (Gauss-Newton, exact data)");
    }

    /* DOA is latency-independent: add a constant to every arrival -> same direction. */
    {
        double a0[ZYLIA_MICS], a1[ZYLIA_MICS];
        double src[3] = { -1.5, 0.8, -2.2 };
        synth(center, src, 0.0,   C, a0);
        synth(center, src, 0.013, C, a1);   /* +13 ms of latency */
        float d0[3], d1[3];
        zylia_doa(a0, d0); zylia_doa(a1, d1);
        double dot = d0[0]*d1[0] + d0[1]*d1[1] + d0[2]*d1[2];
        CHECK(dot > 0.99999, "DOA unchanged by a constant latency shift");
    }

    /* live-transient path: a clap-like Gaussian click, sampled at each capsule's exact (fractional)
     * arrival time + noise -> zylia_tdoa recovers the arrival differences -> zylia_doa the direction.
     * This is the whole live-DOA pipeline (zylia_probe GUI) minus the ASIO capture. */
    {
        const double FS = 48000.0, SIGMA = 1.0e-4;      /* ~100 us wide: broadband, ~5 samples at 48 kHz */
        enum { N = 4096 };
        static float buf[ZYLIA_MICS][N];
        const float* ptr[ZYLIA_MICS];
        double arr_true[ZYLIA_MICS], arr_est[ZYLIA_MICS];
        unsigned int rng = 777u;

        struct { double pos[3]; const char* name; } tcases[] = {
            {{  2.0,  1.4,  0.5 }, "clap right"},
            {{ -1.0,  0.2, -2.5 }, "clap front-left"},
            {{  0.1,  3.5, -0.4 }, "clap overhead"},
        };
        for (int t = 0; t < 3; ++t) {
            synth(center, tcases[t].pos, 0.0021, C, arr_true);          /* real capture has latency: DOA won't care */
            for (int chn = 0; chn < ZYLIA_MICS; ++chn) {
                double t0 = 0.030 + arr_true[chn];                       /* click lands ~1440 samples in */
                for (int i = 0; i < N; ++i) {
                    double td = (double)i / FS - t0;
                    double s  = exp(-0.5 * (td / SIGMA) * (td / SIGMA)); /* Gaussian click at the exact fractional time */
                    rng = rng * 1664525u + 1013904223u;
                    double nz = ((double)(int)(rng >> 9) / (double)(1 << 22) - 1.0) * 1e-3;   /* ~-60 dB noise */
                    buf[chn][i] = (float)(s + nz);
                }
                ptr[chn] = buf[chn];
            }
            int ok = zylia_tdoa(ptr, N, FS, 32, arr_est);
            float doa[3];
            int ok2 = ok && zylia_doa(arr_est, doa);
            double tx = tcases[t].pos[0]-center[0], ty = tcases[t].pos[1]-center[1], tz = tcases[t].pos[2]-center[2];
            double td = sqrt(tx*tx + ty*ty + tz*tz); tx/=td; ty/=td; tz/=td;
            double dot = ok2 ? doa[0]*tx + doa[1]*ty + doa[2]*tz : -1.0;
            double deg = acos(fmax(-1.0, fmin(1.0, dot))) * 180.0 / 3.14159265358979;
            printf("[%-14s] tdoa->doa err=%.3f deg\n", tcases[t].name, deg);
            CHECK(ok2 && deg < 2.0, "live-transient TDOA -> DOA within 2 deg");
        }

        /* no transient -> refuse (steady noise must not produce a phantom direction) */
        for (int chn = 0; chn < ZYLIA_MICS; ++chn) {
            for (int i = 0; i < N; ++i) {
                rng = rng * 1664525u + 1013904223u;
                buf[chn][i] = (float)(((double)(int)(rng >> 9) / (double)(1 << 22) - 1.0) * 0.1);
            }
            ptr[chn] = buf[chn];
        }
        CHECK(!zylia_tdoa(ptr, N, FS, 32, arr_est), "steady noise (no transient) is rejected");
    }

    /* ---- validation-grade DOA: first-order active intensity (zylia_intensity_doa) ----
     *
     * READ THIS BEFORE ADDING A TEST HERE. The obvious way to test this estimator is to synthesize
     * capsule pressures from the rigid-sphere SH model it inverts. Do not: that model IS the
     * estimator's own assumption, so a conjugated Hankel function, a flipped FFT sign convention or a
     * swapped axis appears on both sides and CANCELS EXACTLY — the same trap the capsule table has
     * (see test_geometry). It would pass while pointing backwards on hardware.
     *
     * So the forward model below is a pure GEOMETRIC DELAY: a band-limited source, sampled at each
     * capsule's exact arrival time. It shares nothing with the estimator — no spherical harmonics, no
     * mode strengths, no FFT convention. It is free-field rather than a scattering sphere, and that
     * mismatch is deliberately harmless: a wrong RADIAL model is one complex scalar per DEGREE, and
     * all three first-order components share a degree, so it can scale or flip the intensity vector
     * but CANNOT rotate it. Which makes this exactly the check worth having — it is blind to the
     * modelling approximation and maximally sensitive to a sign error, the failure that matters.
     */
    {
        const double FS = 48000.0;
        enum { NS = 8192, NT = 24 };                  /* 4 frames of 2048 at 50% hop; 24 tones in band */
        static float ibuf[ZYLIA_MICS][NS];
        const float* iptr[ZYLIA_MICS];

        double phase[NT];
        unsigned int rng = 20260721u;
        for (int j = 0; j < NT; ++j) {
            rng = rng * 1664525u + 1013904223u;
            phase[j] = ((double)(rng >> 8) / (double)(1u << 24)) * 6.283185307179586;
        }

        struct { double pos[3]; const char* name; } icases[] = {
            {{  2.5,  1.2,  0.0 }, "int right"},
            {{ -2.0,  1.2, -1.5 }, "int front-left"},
            {{  0.0,  1.2, -2.5 }, "int front"},
            {{  0.3,  3.0,  0.2 }, "int overhead"},
            {{ -0.5,  1.0,  2.6 }, "int behind"},
        };
        for (int t = 0; t < 5; ++t) {
            double tx = icases[t].pos[0]-center[0], ty = icases[t].pos[1]-center[1], tz = icases[t].pos[2]-center[2];
            double tn = sqrt(tx*tx + ty*ty + tz*tz); tx/=tn; ty/=tn; tz/=tn;

            synth_field(center, icases[t].pos, C, FS, ibuf, NS, phase, NT);
            for (int chn = 0; chn < ZYLIA_MICS; ++chn) iptr[chn] = ibuf[chn];

            float d[3], psi = -1.0f;
            int ok = zylia_intensity_doa(iptr, NS, FS, C, 400.0, 1200.0, NULL, d, &psi);
            double dot = ok ? d[0]*tx + d[1]*ty + d[2]*tz : -1.0;
            double deg = acos(fmax(-1.0, fmin(1.0, dot))) * 180.0 / 3.14159265358979;
            printf("[%-14s] intensity doa err=%.2f deg  diffuseness=%.3f\n", icases[t].name, deg, psi);
            CHECK(ok && deg < 1.5, "active-intensity DOA recovers a coherent source");
            /* Not ~0, and that is the forward model rather than the estimator: diffuseness compares
             * |V| against |W|, which is exactly the ratio a free-field source read through a
             * rigid-sphere inversion gets wrong. Direction is immune to that (one scalar per degree
             * cannot rotate the vector); this is the one output that is not. On a real sphere it
             * sits lower. Kept as a floor/ceiling check, not a precision one. */
            CHECK(ok && psi < 0.35f, "a coherent source reads as low diffuseness");
        }

        /* CROSS-ESTIMATOR AGREEMENT — the load-bearing check. Same click, two estimators that share
         * no physics: TDOA reads arrival ORDER in the time domain, intensity reads a spectral phase
         * relationship. A sign/convention error in the intensity path flips it ~180 deg away from a
         * TDOA answer that cannot be wrong about which side the sound came from. */
        {
            const double SIGMA = 3.0e-4;                 /* wider click: energy down in the 400-1200 band */
            static float cbuf[ZYLIA_MICS][NS];
            const float* cptr[ZYLIA_MICS];
            double arr_true[ZYLIA_MICS], arr_est[ZYLIA_MICS];
            double cpos[3] = { 1.8, 1.1, -1.9 };
            synth(center, cpos, 0.0030, C, arr_true);
            for (int chn = 0; chn < ZYLIA_MICS; ++chn) {
                double t0 = 0.040 + arr_true[chn];
                for (int i = 0; i < NS; ++i) {
                    double td = (double)i / FS - t0;
                    cbuf[chn][i] = (float)exp(-0.5 * (td / SIGMA) * (td / SIGMA));
                }
                cptr[chn] = cbuf[chn];
            }
            float di[3], dt[3];
            int oki = zylia_intensity_doa(cptr, NS, FS, C, 400.0, 1200.0, NULL, di, NULL);
            int okt = zylia_tdoa(cptr, NS, FS, 32, arr_est) && zylia_doa(arr_est, dt);
            double dot = (oki && okt) ? di[0]*dt[0] + di[1]*dt[1] + di[2]*dt[2] : -1.0;
            double deg = acos(fmax(-1.0, fmin(1.0, dot))) * 180.0 / 3.14159265358979;
            printf("[cross-check   ] intensity vs tdoa disagreement = %.2f deg\n", deg);
            CHECK(oki && okt && deg < 1.5, "intensity DOA agrees with the independent TDOA estimator");
        }

        /* A field with no coherent direction must not yield a confident one. Independent noise per
         * capsule is the extreme case: the intensity vector averages to nothing, diffuseness -> 1. */
        {
            for (int chn = 0; chn < ZYLIA_MICS; ++chn) {
                for (int i = 0; i < NS; ++i) {
                    rng = rng * 1664525u + 1013904223u;
                    ibuf[chn][i] = (float)((double)(int)(rng >> 9) / (double)(1 << 22) - 1.0);
                }
                iptr[chn] = ibuf[chn];
            }
            float d[3], psi = -1.0f;
            int ok = zylia_intensity_doa(iptr, NS, FS, C, 400.0, 1200.0, NULL, d, &psi);
            printf("[diffuse       ] incoherent field diffuseness=%.3f\n", ok ? psi : -1.0f);
            CHECK(ok && psi > 0.9f, "an incoherent field reads as highly diffuse");
        }

        /* Band discipline: a request lying entirely above the first-order ceiling is REFUSED, not
         * silently clamped into a band the array cannot resolve (the 6 kHz tone the paper drops). */
        {
            float d[3];
            CHECK(!zylia_intensity_doa(iptr, NS, FS, C, 5000.0, 7000.0, NULL, d, NULL),
                  "a band above the kr~1 ceiling is refused");
            CHECK(!zylia_intensity_doa(iptr, 1024, FS, C, 400.0, 1200.0, NULL, d, NULL),
                  "fewer samples than one analysis frame is refused");
        }
    }

    /* ---- signal integrity + the independent SRP-PHAT cross-check ---- */
    {
        const double FS = 48000.0;
        enum { NS = FIELD_N, NT = 24 };
        static float sbuf[ZYLIA_MICS][NS], wbuf[ZYLIA_MICS][NS];
        const float* sptr[ZYLIA_MICS];
        const float* wptr[ZYLIA_MICS];
        unsigned char flags[ZYLIA_MICS];
        double phase[NT];
        unsigned int rng = 13571113u;
        for (int j = 0; j < NT; ++j) {
            rng = rng * 1664525u + 1013904223u;
            phase[j] = ((double)(rng >> 8) / (double)(1u << 24)) * 6.283185307179586;
        }
        double spos[3] = { 2.2, 1.4, -1.3 }, truth[3];
        {
            double tx = spos[0]-center[0], ty = spos[1]-center[1], tz = spos[2]-center[2];
            double tn = sqrt(tx*tx + ty*ty + tz*tz);
            truth[0] = tx/tn; truth[1] = ty/tn; truth[2] = tz/tn;
        }
        synth_field(center, spos, C, FS, sbuf, NS, phase, NT);
        for (int ch = 0; ch < ZYLIA_MICS; ++ch) sptr[ch] = sbuf[ch];

        double healthy_rms = 0.0;
        for (int i = 0; i < NS; ++i) healthy_rms += (double)sbuf[0][i] * sbuf[0][i];
        healthy_rms = sqrt(healthy_rms / NS);

        int nbad = zylia_check_capsules(sptr, NS, flags);
        printf("[integrity     ] healthy capture: %d faulty capsules\n", nbad);
        CHECK(nbad == 0, "a healthy capture flags nothing (no false positives)");

        /* SRP-PHAT on the same clean field. Steered power vs an intensity phase relationship: two
         * estimators that fail differently, which is the only reason a cross-check is worth anything. */
        {
            float ds[3], di[3];
            int oks = zylia_srp_doa(sptr, NS, FS, C, 400.0, 1200.0, NULL, ds);
            int oki = zylia_intensity_doa(sptr, NS, FS, C, 400.0, 1200.0, NULL, di, NULL);
            double dd[3] = { di[0], di[1], di[2] };
            double es = oks ? ang_deg(ds, truth) : 999.0;
            double ei = oki ? ang_deg(di, truth) : 999.0;
            double dis = (oks && oki) ? ang_deg(ds, dd) : 999.0;
            printf("[srp-phat      ] srp err=%.2f deg  intensity err=%.2f deg  disagreement=%.2f deg\n",
                   es, ei, dis);
            CHECK(oks && es < 4.0, "SRP-PHAT recovers the source (grid-limited, ~2 deg)");
            CHECK(oks && oki && dis < 4.0, "SRP-PHAT and active intensity agree");
        }

        /* THE fault this layer exists for, and the reason a cross-check alone is not enough. One
         * capsule goes hot with broadband self-noise: total array power still looks healthy, every
         * other capsule is fine, and the direction is poisoned anyway — because every SH channel is a
         * weighted sum over ALL capsules. Both estimators are corrupted IDENTICALLY, so they would
         * agree with each other and both be wrong. Only the raw-signal check catches it. */
        {
            const int BADCH = 7;
            /* A realistically QUIET capture, so a 45 dB fault has headroom to actually be 45 dB up
             * instead of just pinning the converter — otherwise this tests clipping, not self-noise.
             * 45 dB is the magnitude of the real documented fault, not a number picked to pass. */
            const double QUIET = 0.005, FAULT_DB = 45.0;
            for (int ch = 0; ch < ZYLIA_MICS; ++ch)
                for (int i = 0; i < NS; ++i) wbuf[ch][i] = (float)(QUIET * (double)sbuf[ch][i]);
            double fault_amp = healthy_rms * QUIET * pow(10.0, FAULT_DB / 20.0);
            for (int i = 0; i < NS; ++i) {
                rng = rng * 1664525u + 1013904223u;
                wbuf[BADCH][i] = (float)(((double)(int)(rng >> 9)/(double)(1<<22) - 1.0) * fault_amp);
            }
            for (int ch = 0; ch < ZYLIA_MICS; ++ch) wptr[ch] = wbuf[ch];

            int nb = zylia_check_capsules(wptr, NS, flags);
            printf("[integrity     ] hot capsule: %d flagged, ch%d flags=0x%02X\n", nb, BADCH, flags[BADCH]);
            CHECK(nb == 1 && (flags[BADCH] & ZYLIA_CAP_HOT), "a hot capsule is flagged, and only it");

            float dbad[3], dfix[3];
            int okb = zylia_intensity_doa(wptr, NS, FS, C, 400.0, 1200.0, NULL,  dbad, NULL);
            int okf = zylia_intensity_doa(wptr, NS, FS, C, 400.0, 1200.0, flags, dfix, NULL);
            double eb = okb ? ang_deg(dbad, truth) : 999.0;
            double ef = okf ? ang_deg(dfix, truth) : 999.0;
            printf("[integrity     ] DOA with the fault=%.1f deg   with it excluded=%.2f deg\n", eb, ef);
            CHECK(okb && eb > 5.0, "the fault really does corrupt the direction (else this proves nothing)");
            CHECK(okf && ef < 2.0, "excluding the flagged capsule restores the direction");

            /* the same exclusion carries into the cross-check estimator */
            float ds[3];
            int oks = zylia_srp_doa(wptr, NS, FS, C, 400.0, 1200.0, flags, ds);
            CHECK(oks && ang_deg(ds, truth) < 4.0, "SRP-PHAT honours the exclusion mask too");
        }

        /* Each remaining fault mode on its own capsule, so no flag is ambiguous. */
        {
            memcpy(wbuf, sbuf, sizeof sbuf);
            for (int i = 0; i < NS; ++i) wbuf[3][i] = 0.0f;                  /* dead: silent */
            for (int ch = 0; ch < ZYLIA_MICS; ++ch) wptr[ch] = wbuf[ch];
            zylia_check_capsules(wptr, NS, flags);
            printf("[integrity     ] dead capsule ch3 flags=0x%02X\n", flags[3]);
            CHECK(flags[3] & ZYLIA_CAP_DEAD, "a silent capsule is flagged dead");

            memcpy(wbuf, sbuf, sizeof sbuf);
            for (int i = 0; i < NS; ++i) {                                   /* clipped, not hot */
                double v = 3.0 * (double)sbuf[11][i];
                wbuf[11][i] = (float)(v > 1.0 ? 1.0 : (v < -1.0 ? -1.0 : v));
            }
            for (int ch = 0; ch < ZYLIA_MICS; ++ch) wptr[ch] = wbuf[ch];
            zylia_check_capsules(wptr, NS, flags);
            printf("[integrity     ] clipped capsule ch11 flags=0x%02X\n", flags[11]);
            CHECK(flags[11] & ZYLIA_CAP_CLIPPED, "a pinned capsule is flagged clipped");

            memcpy(wbuf, sbuf, sizeof sbuf);
            for (int i = 0; i < NS; ++i) {          /* right LEVEL, wrong SIGNAL — the level checks are
                                                     * blind to this one; only coherence sees it */
                rng = rng * 1664525u + 1013904223u;
                wbuf[15][i] = (float)(((double)(int)(rng >> 9)/(double)(1<<22) - 1.0) * healthy_rms * 1.732);
            }
            for (int ch = 0; ch < ZYLIA_MICS; ++ch) wptr[ch] = wbuf[ch];
            zylia_check_capsules(wptr, NS, flags);
            printf("[integrity     ] incoherent capsule ch15 flags=0x%02X\n", flags[15]);
            CHECK((flags[15] & ZYLIA_CAP_INCOHERENT) && !(flags[15] & (ZYLIA_CAP_DEAD|ZYLIA_CAP_HOT)),
                  "a right-level wrong-signal capsule is caught by coherence alone");
        }

        /* Order step-down: strip enough capsules that order 3 no longer fits and SRP must fall back
         * rather than over-fit. It should still answer, just more coarsely. */
        {
            unsigned char ex[ZYLIA_MICS] = { 0 };
            for (int ch = 0; ch < 6; ++ch) ex[ch] = 1;        /* 13 capsules left: order 3 cannot fit */
            float ds[3];
            int oks = zylia_srp_doa(sptr, NS, FS, C, 400.0, 1200.0, ex, ds);
            printf("[srp-phat      ] 13 capsules: ok=%d err=%.2f deg\n", oks, oks ? ang_deg(ds, truth) : -1.0);
            CHECK(oks, "SRP-PHAT steps the order down instead of failing");

            for (int ch = 0; ch < ZYLIA_MICS - 4; ++ch) ex[ch] = 1;   /* 4 left: not even first order */
            CHECK(!zylia_srp_doa(sptr, NS, FS, C, 400.0, 1200.0, ex, ds),
                  "too few capsules is refused, not over-fitted");
            CHECK(!zylia_intensity_doa(sptr, NS, FS, C, 400.0, 1200.0, ex, ds, NULL),
                  "intensity DOA refuses too few capsules as well");
        }
    }

    /* ---- tracked mount: survey once in the stand's frame, then follow the pose ----
     *
     * The scenario this has to get right is the one a validation session creates seven times: the mic
     * is surveyed at one mount pose, then physically MOVED and re-aimed. Channel order survives that;
     * orientation does not. If the stand is a tracked rigid body the rotation is measurable, so the
     * body-frame table plus the live pose should reconstruct the room-axes geometry exactly.
     *
     * The control at the end is the part that makes this a test rather than a demonstration: decoding
     * the moved array against the STALE survey must come back badly wrong. If it didn't, the rotation
     * would be buying nothing and the whole mechanism could be deleted without a failure.
     */
    {
        const double C2 = 343.0;
        float base[ZYLIA_MICS][3];
        zylia_set_capsules(NULL);
        zylia_capsules(base);                       /* the built-in table, array-centred */

        /* two mount poses: A = where it was surveyed, B = where it ended up after a remount */
        float qA[4] = { 0.0f, 0.3827f, 0.0f, 0.9239f };            /* 45 deg yaw */
        float qB[4] = { 0.1830f, 0.3106f, 0.0223f, 0.9321f };      /* yaw + a tilted stand */
        float RA[9], RB[9];
        zylia_quat_to_matrix(qA, RA);
        zylia_quat_to_matrix(qB, RB);

        /* rotation matrices must be orthonormal or nothing downstream is a rotation */
        double orth = 0.0;
        for (int r = 0; r < 3; ++r)
            for (int cc = 0; cc < 3; ++cc) {
                double d = 0.0;
                for (int k = 0; k < 3; ++k) d += (double)RB[r*3+k] * RB[cc*3+k];
                orth += fabs(d - (r == cc ? 1.0 : 0.0));
            }
        CHECK(orth < 1e-5, "quaternion -> rotation matrix is orthonormal");

        /* a quarter turn about +y takes +x to -z, which pins the handedness */
        float qY[4] = { 0.0f, 0.7071068f, 0.0f, 0.7071068f }, RY[9];
        zylia_quat_to_matrix(qY, RY);
        CHECK(fabs(RY[0]) < 1e-5 && fabs(RY[6] + 1.0f) < 1e-5,
              "+90 deg about +y maps +x to -z (right-handed, y up)");

        /* what a survey at pose A recovers (room axes), and its body-frame form */
        float capsA[ZYLIA_MICS][3], capsBody[ZYLIA_MICS][3], capsB[ZYLIA_MICS][3], back[ZYLIA_MICS][3];
        zylia_capsules_rotate(base, RA, 0, capsA);              /* the array as mounted at A */
        zylia_capsules_rotate(capsA, RA, 1, capsBody);          /* room -> body */
        zylia_capsules_rotate(capsBody, RA, 0, back);           /* and back again */
        double worst_rt = 0.0;
        for (int i = 0; i < ZYLIA_MICS; ++i)
            for (int a = 0; a < 3; ++a) {
                double d = fabs(back[i][a] - capsA[i][a]);
                if (d > worst_rt) worst_rt = d;
            }
        CHECK(worst_rt < 1e-6, "room -> body -> room round-trips exactly");

        /* the mic is remounted at pose B: physically, the capsules are the body table under RB */
        zylia_capsules_rotate(capsBody, RB, 0, capsB);

        double src2[3] = { 1.7, 1.9, -2.3 };
        float center2[3] = { 0.2f, 1.1f, -0.4f };
        double arrB[ZYLIA_MICS];
        synth_caps(capsB, center2, src2, C2, arrB);             /* what the moved array really hears */

        double tx = src2[0]-center2[0], ty = src2[1]-center2[1], tz = src2[2]-center2[2];
        double tn = sqrt(tx*tx+ty*ty+tz*tz);
        double truth2[3] = { tx/tn, ty/tn, tz/tn };

        float d_tracked[3], d_stale[3];
        zylia_set_capsules(capsB);                              /* body table x live pose */
        CHECK(zylia_doa(arrB, d_tracked), "tracked-mount DOA solves");
        double e_tracked = ang_deg(d_tracked, truth2);

        zylia_set_capsules(capsA);                              /* the CONTROL: survey-time table */
        CHECK(zylia_doa(arrB, d_stale), "stale-mount DOA solves");
        double e_stale = ang_deg(d_stale, truth2);

        printf("[tracked mount] remounted: tracked err %.3f deg   stale survey err %.1f deg\n",
               e_tracked, e_stale);
        CHECK(e_tracked < 0.5, "the live pose reconstructs the moved array's geometry");
        CHECK(e_stale > 10.0, "...and the stale survey is badly wrong, so the pose is doing the work");

        /* the body-frame survey round-trips through the file, offset and all */
        {
            const char* path = "._zy_mount_test.json";
            char e[128] = { 0 };
            ZyliaMount m = { 1, 1, { 0.011f, -0.204f, 0.003f } }, got;
            CHECK(zylia_survey_save(path, capsBody, 0.4f, 0.049f, 1.0f, 12, &m, e, sizeof e),
                  "body-frame survey saves");
            CHECK(zylia_survey_load(path, &got, e, sizeof e), "body-frame survey loads");
            CHECK(got.body_frame == 1 && got.have_offset == 1, "the mount metadata survives");
            CHECK(fabs(got.offset_m[1] + 0.204f) < 1e-6, "the probed offset survives");
            /* a body-frame file handed to a caller that can't be told is refused, not silently
             * installed as if it were room axes */
            CHECK(!zylia_survey_load(path, NULL, e, sizeof e),
                  "a body-frame survey is refused without a mount out-param");
            remove(path);
        }
        zylia_set_capsules(NULL);
    }

    printf("%s\n", fails ? "FAIL: zylia localization" : "PASS: zylia single-position localization");
    return fails ? 1 : 0;
}
