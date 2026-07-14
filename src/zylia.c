/*
 * zylia.c — single-position ZM-1 speaker localization. See zylia.h for the model + accuracy notes.
 */
#include "zylia.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#define ZYLIA_RADIUS_M 0.049f   /* ZM-1 shell is ~100 mm across. Only zylia_localize's near-field solve
                                 * reads this — zylia_doa normalizes the fitted gradient, so the radius
                                 * cancels out of the DIRECTION entirely (49 vs 50 mm changes nothing). */

/* An installed capsule survey (zylia_set_capsules). Overrides the built-in table everywhere. These
 * tools are single-threaded; the survey is set once at load and read by the solves. */
static float g_caps[ZYLIA_MICS][3];
static int   g_have_survey = 0;

void zylia_geometry(float dirs[ZYLIA_MICS][3], float* radius_m) {
    /* The ZM-1's 19 capsules are the 20 vertices of a regular DODECAHEDRON, vertex-up, minus the nadir
     * vertex. That is the whole geometry, and it self-checks: the rings come out 1 / 3 / 6 / 6 / 3 by
     * elevation (the missing 20th would be the 1 at -90), each ring's azimuths are exactly its opposite
     * ring's rotated 180 deg, and the elevations are the dodecahedral values below. It reproduces the
     * node table published in Zylia's documentation (see docs/calibration.md for the source) to that
     * table's 1-degree rounding — but we build it from the closed forms, so there is no rounding here.
     *
     * Frame: the engine's room convention (+X right, +Y up, -Z front); azimuth measured from -Z toward
     * +X, elevation = asin(y). That is exactly what calib_view's zy_az/zy_el invert.
     *
     * TWO THINGS THIS TABLE CANNOT TELL YOU, both of which must be pinned on the rig:
     *   - CHANNEL ORDER: node i here is not necessarily ASIO input i. A permutation still yields a
     *     confident direction, just the wrong one. bwa_zylia_probe resolves it (tap a capsule, see
     *     which channel jumps).
     *   - AZIMUTH REFERENCE: nothing published says which capsule faces the device's front, so an
     *     unknown yaw offset rotates every DOA by a constant. Clap from a known direction in
     *     calib_view's Zylia tab; the discrepancy IS the offset.
     * Both fall out for free if you run the capsule self-survey instead (docs/calibration.md).
     */
    const double D2R = 3.14159265358979323846 / 180.0;
    const double A = asin(sqrt(5.0) / 3.0) / D2R;    /* 48.1897 deg — the +-48 rings                 */
    const double B = asin(1.0 / 3.0)       / D2R;    /* 19.4712 deg — the +-19 rings                 */
    const double Q = atan(sqrt(3.0 / 5.0)) / D2R;    /* 37.7612 deg — generates every ring's azimuth */

    /* {azimuth, elevation} in degrees, in the published node order. */
    const double node[ZYLIA_MICS][2] = {
        {       0.0,  90.0 },                                                  /* 0     zenith   */
        {       0.0,     A }, {     120.0,     A }, {    -120.0,     A },       /* 1-3   +48 ring */
        { -(120.0-Q),    B }, {        -Q,     B }, {         Q,     B },       /* 4-9   +19 ring */
        {   120.0-Q,     B }, {   120.0+Q,     B }, { -(120.0+Q),    B },
        { -(180.0-Q),   -B }, { -(60.0+Q),    -B }, { -(60.0-Q),    -B },       /* 10-15 -19 ring */
        {    60.0-Q,    -B }, {   60.0+Q,     -B }, {   180.0-Q,    -B },
        {     180.0,    -A }, {     -60.0,    -A }, {      60.0,    -A },       /* 16-18 -48 ring */
    };

    for (int i = 0; i < ZYLIA_MICS; ++i) {
        double az = node[i][0] * D2R, el = node[i][1] * D2R, ce = cos(el);
        dirs[i][0] = (float)( ce * sin(az));
        dirs[i][1] = (float)( sin(el));
        dirs[i][2] = (float)(-ce * cos(az));
    }
    if (radius_m) *radius_m = ZYLIA_RADIUS_M;

    if (g_have_survey) {                       /* a survey overrides the table: hand back its directions */
        double rsum = 0.0;
        for (int i = 0; i < ZYLIA_MICS; ++i) {
            double m = sqrt((double)g_caps[i][0]*g_caps[i][0] + (double)g_caps[i][1]*g_caps[i][1] +
                            (double)g_caps[i][2]*g_caps[i][2]);
            rsum += m;
            if (m < 1e-9) m = 1e-9;
            dirs[i][0] = (float)(g_caps[i][0] / m);
            dirs[i][1] = (float)(g_caps[i][1] / m);
            dirs[i][2] = (float)(g_caps[i][2] / m);
        }
        if (radius_m) *radius_m = (float)(rsum / ZYLIA_MICS);
    }
}

void zylia_set_capsules(const float caps_m[ZYLIA_MICS][3]) {
    if (!caps_m) { g_have_survey = 0; return; }
    memcpy(g_caps, caps_m, sizeof g_caps);
    g_have_survey = 1;
}

void zylia_capsules(float caps_m[ZYLIA_MICS][3]) {
    if (!caps_m) return;
    if (g_have_survey) { memcpy(caps_m, g_caps, sizeof g_caps); return; }
    float dirs[ZYLIA_MICS][3], R;
    zylia_geometry(dirs, &R);                  /* no survey installed, so this is the built-in table */
    for (int i = 0; i < ZYLIA_MICS; ++i) {
        caps_m[i][0] = R * dirs[i][0];
        caps_m[i][1] = R * dirs[i][1];
        caps_m[i][2] = R * dirs[i][2];
    }
}

/* Gaussian elimination with partial pivoting on a row-major n x n system A*x = b (n <= 4). 0 if singular. */
static int solve_lin(int n, double* A, double* b, double* x) {
    for (int c = 0; c < n; ++c) {
        int piv = c;
        for (int r = c + 1; r < n; ++r) if (fabs(A[r*n+c]) > fabs(A[piv*n+c])) piv = r;
        if (fabs(A[piv*n+c]) < 1e-15) return 0;
        if (piv != c) {
            for (int j = 0; j < n; ++j) { double t = A[c*n+j]; A[c*n+j] = A[piv*n+j]; A[piv*n+j] = t; }
            double t = b[c]; b[c] = b[piv]; b[piv] = t;
        }
        for (int r = c + 1; r < n; ++r) {
            double f = A[r*n+c] / A[c*n+c];
            for (int j = c; j < n; ++j) A[r*n+j] -= f * A[c*n+j];
            b[r] -= f * b[c];
        }
    }
    for (int r = n - 1; r >= 0; --r) {
        double s = b[r];
        for (int j = r + 1; j < n; ++j) s -= A[r*n+j] * x[j];
        x[r] = s / A[r*n+r];
    }
    return 1;
}

int zylia_doa(const double arrival_s[ZYLIA_MICS], float dir_out[3]) {
    if (!arrival_s || !dir_out) return 0;
    float caps[ZYLIA_MICS][3]; zylia_capsules(caps);
    /* Far field: tau_i = A - (1/c)(m_i . d), m_i = capsule position relative to the centre. Fit
     * tau_i = A + b.m_i (4 unknowns) by least squares; b = -d/c points AWAY from the source, so
     * d = -normalize(b). Latency folds into A and cancels. Fitting against POSITIONS rather than
     * (radius x unit direction) is what lets a SURVEYED array — capsules not exactly on the nominal
     * sphere — be handled exactly, and it makes the radius irrelevant to the direction. */
    double M[16] = {0}, rhs[4] = {0};
    for (int i = 0; i < ZYLIA_MICS; ++i) {
        double row[4] = { 1.0, caps[i][0], caps[i][1], caps[i][2] };
        for (int a = 0; a < 4; ++a) {
            for (int cc = 0; cc < 4; ++cc) M[a*4+cc] += row[a] * row[cc];
            rhs[a] += row[a] * arrival_s[i];
        }
    }
    double x[4];
    if (!solve_lin(4, M, rhs, x)) return 0;
    double nb = sqrt(x[1]*x[1] + x[2]*x[2] + x[3]*x[3]);
    if (nb < 1e-12) return 0;
    dir_out[0] = (float)(-x[1]/nb);
    dir_out[1] = (float)(-x[2]/nb);
    dir_out[2] = (float)(-x[3]/nb);
    return 1;
}

/* Live-transient TDOA (see zylia.h). Plain time-domain cross-correlation on a short window around
 * the onset: at n~4k, L=256, max_lag~32 that is ~19 x 65 x 256 MACs — microseconds, no FFT needed.
 * A clap is broadband, so the correlation has ONE sharp peak (no carrier-cycle ambiguity). */
#define TDOA_WIN 256           /* correlation window (samples): ~5 ms at 48 kHz — direct sound only;
                                 * the first room reflection lands ms later and stays outside it */

int zylia_tdoa(const float* const x[ZYLIA_MICS], uint32_t n, double fs, uint32_t max_lag,
               double arrival_s[ZYLIA_MICS]) {
    if (!x || !arrival_s || fs <= 0.0 || max_lag < 1 || max_lag > 4096) return 0;

    /* reference = the channel with the strongest peak (closest capsule: earliest + loudest). */
    int ref = 0; float pk = 0.f; uint32_t pki = 0;
    for (int ch = 0; ch < ZYLIA_MICS; ++ch) {
        if (!x[ch]) return 0;
        for (uint32_t i = 0; i < n; ++i) {
            float a = fabsf(x[ch][i]);
            if (a > pk) { pk = a; ref = ch; pki = i; }
        }
    }
    double sos = 0.0;                                   /* transient sanity: peak must stand off the RMS */
    for (uint32_t i = 0; i < n; ++i) sos += (double)x[ref][i] * x[ref][i];
    double rms = sqrt(sos / (n > 0 ? n : 1));
    if (pk < 6.0 * rms || pk < 1e-6f) return 0;         /* steady noise / silence: no transient to time */

    /* onset on the reference: first sample reaching 20% of the peak (leading edge — robust to the
     * tail and to reflections after it). Window starts just before it. */
    uint32_t onset = pki;
    for (uint32_t i = 0; i < n; ++i) if (fabsf(x[ref][i]) >= 0.2f * pk) { onset = i; break; }
    /* Correlation window [s0, s0+L) with lags in [-max_lag, +max_lag]. It must satisfy both
     * s0 >= max_lag (negative lags stay in bounds) AND s0 + L + max_lag < n (positive lags do too).
     * Clamp L to the largest value that fits and bail if there is no room — computed underflow-safe
     * (uint32), since a large max_lag (the localize refine) can exceed TDOA_WIN. */
    uint32_t L  = TDOA_WIN;
    uint32_t s0 = (onset > max_lag + 8) ? onset - max_lag - 8 : 0;    /* pre-roll: capture earlier arrivals */
    if (n < 2 * max_lag + 64) return 0;                               /* too little context to correlate */
    if (s0 < max_lag) s0 = max_lag;                                   /* need s0-lag >= 0 for negative lags */
    if (s0 + max_lag + 1 >= n) return 0;                              /* no room for even a 1-sample window */
    uint32_t Lmax = n - s0 - max_lag - 1;                             /* largest L keeping s0+L+max_lag < n */
    if (L > Lmax) L = Lmax;
    if (L < 32) return 0;

    for (int ch = 0; ch < ZYLIA_MICS; ++ch) {
        if (ch == ref) { arrival_s[ch] = 0.0; continue; }
        /* r(lag) = sum_i ref[s0+i] * ch[s0+i+lag]; a channel DELAYED by d peaks at lag = +d. */
        int    best = 0; double rbest = -1e300, rm1 = 0, rp1 = 0;
        double r_at[3];                                     /* r at best-1 / best / best+1 for the parabola */
        for (int lag = -(int)max_lag; lag <= (int)max_lag; ++lag) {
            double r = 0.0;
            const float* a = x[ref] + s0;
            const float* b = x[ch]  + s0 + lag;
            for (uint32_t i = 0; i < L; ++i) r += (double)a[i] * b[i];
            if (r > rbest) { rbest = r; best = lag; }
        }
        double frac = 0.0;
        if (best > -(int)max_lag && best < (int)max_lag) {  /* interior peak: parabolic sub-sample refine */
            for (int k = -1; k <= 1; ++k) {                 /* recompute the neighbours (cheaper than storing the curve) */
                int lag = best + k;
                double r = 0.0;
                const float* a = x[ref] + s0;
                const float* b = x[ch]  + s0 + lag;
                for (uint32_t i = 0; i < L; ++i) r += (double)a[i] * b[i];
                r_at[k + 1] = r;
            }
            rm1 = r_at[0]; rp1 = r_at[2];
            double denom = rm1 - 2.0 * r_at[1] + rp1;
            frac = (fabs(denom) > 1e-12) ? 0.5 * (rm1 - rp1) / denom : 0.0;
            if (frac >  0.5) frac =  0.5;                   /* the true peak is between the neighbours */
            if (frac < -0.5) frac = -0.5;
        }                                                   /* boundary peak: keep the integer lag as-is */
        arrival_s[ch] = ((double)best + frac) / fs;
    }
    return 1;
}

int zylia_survey(const float src_m[][3], const double (*arrival_s)[ZYLIA_MICS], int nobs, double c,
                 float caps_out[ZYLIA_MICS][3], float* resid_us, float* radius_out, float* spread_out) {
    if (!src_m || !arrival_s || !caps_out || nobs < 4 || c <= 0.0) return 0;

    double d[ZYLIA_SURVEY_MAX][3], D[ZYLIA_SURVEY_MAX];      /* unit direction + range, per observation */
    if (nobs > ZYLIA_SURVEY_MAX) nobs = ZYLIA_SURVEY_MAX;
    for (int k = 0; k < nobs; ++k) {
        D[k] = sqrt((double)src_m[k][0]*src_m[k][0] + (double)src_m[k][1]*src_m[k][1] +
                    (double)src_m[k][2]*src_m[k][2]);
        if (D[k] < 0.2) return 0;                            /* inside/at the array: the model is nonsense */
        d[k][0] = src_m[k][0] / D[k]; d[k][1] = src_m[k][1] / D[k]; d[k][2] = src_m[k][2] / D[k];
    }

    /* A = sum_k d_k d_k^T is the normal matrix, and it is the SAME for all 19 capsules — the model
     * separates, so this is one 3x3 shared by nineteen 3-unknown solves. Its determinant is also the
     * degeneracy test: the d_k are unit, so trace(A) = nobs, and 27*det(A/nobs) is 1 for perfectly
     * isotropic directions and 0 when they are coplanar. Coplanar is the realistic failure — clapping
     * in a horizontal ring around the array leaves the capsules' HEIGHTS unconstrained, and the solve
     * would happily return a flattened array rather than admit it. Refuse instead. */
    double A[9] = {0};
    for (int k = 0; k < nobs; ++k)
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) A[a*3+b] += d[k][a] * d[k][b];
    double s = 1.0 / (double)nobs;
    double det = A[0]*s * (A[4]*s*A[8]*s - A[5]*s*A[7]*s)
               - A[1]*s * (A[3]*s*A[8]*s - A[5]*s*A[6]*s)
               + A[2]*s * (A[3]*s*A[7]*s - A[4]*s*A[6]*s);
    double spread = 27.0 * det;
    if (spread < 0.0) spread = 0.0;
    if (spread_out) *spread_out = (float)spread;
    if (spread < 0.05) return 0;               /* coplanar / clustered: the vertical is unrecoverable */

    /* The linear model is a PLANE wave, but a clap 2.5 m away is a sphere, and across a 49 mm array
     * that curvature is a systematic ~1.4 us — worth 2-3 mm of capsule error if ignored. We know the
     * clap's RANGE (it is the same known spot that gave us the direction), so we can just subtract the
     * curvature off: seed with the plane-wave solve, then iterate
     *     corr[k][i] = ( |src_k - m_i| - (D_k - m_i . d_k) ) / c        (exact minus plane-wave)
     * re-solving the linear system on tau - corr each round. corr depends only weakly on m, so this is
     * a contraction and settles in a few passes. Pass a far-away source and corr -> 0, recovering the
     * pure plane-wave solve. */
    double m[ZYLIA_MICS][3];
    memset(m, 0, sizeof m);
    for (int iter = 0; iter < 8; ++iter) {
        /* Per observation, subtract the mean over the 19 capsules. That mean is exactly the per-k
         * constant (t0_k + D_k/c) under the centering gauge sum_i m_i = 0, so this removes the system
         * latency, the clap's onset, and zylia_tdoa's arbitrary reference channel in one stroke —
         * without ever estimating one of them. The gauge then holds automatically in the solution:
         * summing the 19 normal equations gives A * (sum_i m_i) = 0, and A is non-singular above. */
        double rhs[ZYLIA_MICS][3];
        memset(rhs, 0, sizeof rhs);
        for (int k = 0; k < nobs; ++k) {
            double y[ZYLIA_MICS], mean = 0.0;
            for (int i = 0; i < ZYLIA_MICS; ++i) {
                double ex = (double)src_m[k][0] - m[i][0], ey = (double)src_m[k][1] - m[i][1],
                       ez = (double)src_m[k][2] - m[i][2];
                double exact = sqrt(ex*ex + ey*ey + ez*ez);
                double plane = D[k] - (m[i][0]*d[k][0] + m[i][1]*d[k][1] + m[i][2]*d[k][2]);
                y[i] = arrival_s[k][i] - (exact - plane) / c;      /* iter 0: m = 0, so corr = 0 */
                mean += y[i];
            }
            mean /= (double)ZYLIA_MICS;
            /* y[i] - mean = -(1/c) m_i . d_k   ->   normal equations  A m_i = -c * sum_k (y-mean) d_k */
            for (int i = 0; i < ZYLIA_MICS; ++i)
                for (int a = 0; a < 3; ++a) rhs[i][a] += -c * (y[i] - mean) * d[k][a];
        }
        for (int i = 0; i < ZYLIA_MICS; ++i) {
            double Ai[9], bi[3];
            memcpy(Ai, A, sizeof Ai);
            bi[0] = rhs[i][0]; bi[1] = rhs[i][1]; bi[2] = rhs[i][2];
            if (!solve_lin(3, Ai, bi, m[i])) return 0;
        }

        /* WHERE IS THE ORIGIN? Arrival times fix the capsule cloud's SHAPE but not its position:
         * translating every m_i by w shifts each arrival by -(1/c) w . d_k, one constant per
         * observation — and those are exactly what we fitted out. The translation is a gauge, so we
         * must CHOOSE it, and the one the linear solve lands on (sum_i m_i = 0, the centroid) is the
         * wrong choice. The ZM-1's capsules are not centroid-balanced: a dodecahedron MISSING its nadir
         * vertex sums to the zenith, so the centroid sits R/19 = 2.6 mm ABOVE the sphere centre. Nobody
         * tape-measures to the centroid. Fit the sphere the capsules actually lie on — algebraically,
         * |p-q|^2 = r^2  ->  2 p_i . q - k = |p_i|^2 with k = |q|^2 - r^2, linear in four unknowns —
         * and re-centre on q, giving the physical centre of the shell: the point the operator measured
         * the clap positions from, and what zylia_localize's `center` argument means.
         *
         * This happens INSIDE the loop, not after it: the near-field correction above needs m in the
         * same frame src_m is measured in. Re-centring only at the end would leave the correction
         * computed 2.6 mm off, and it shows up as a residual that will not go away. */
        double S[16] = {0}, sb[4] = {0}, q[4];
        for (int i = 0; i < ZYLIA_MICS; ++i) {
            double row[4] = { 2.0*m[i][0], 2.0*m[i][1], 2.0*m[i][2], -1.0 };
            double rv    = m[i][0]*m[i][0] + m[i][1]*m[i][1] + m[i][2]*m[i][2];
            for (int a = 0; a < 4; ++a) {
                for (int b = 0; b < 4; ++b) S[a*4+b] += row[a] * row[b];
                sb[a] += row[a] * rv;
            }
        }
        if (!solve_lin(4, S, sb, q)) return 0;
        for (int i = 0; i < ZYLIA_MICS; ++i) { m[i][0] -= q[0]; m[i][1] -= q[1]; m[i][2] -= q[2]; }
    }

    double rsum = 0.0;
    for (int i = 0; i < ZYLIA_MICS; ++i) {
        caps_out[i][0] = (float)m[i][0];
        caps_out[i][1] = (float)m[i][1];
        caps_out[i][2] = (float)m[i][2];
        rsum += sqrt(m[i][0]*m[i][0] + m[i][1]*m[i][1] + m[i][2]*m[i][2]);
    }
    if (radius_out) *radius_out = (float)(rsum / ZYLIA_MICS);   /* ~49 mm; a wild value means bad data */

    if (resid_us) {                            /* RMS of what the fitted geometry fails to explain, on the
                                                * EXACT model — this is the number that says "trust it" */
        double sse = 0.0; int cnt = 0;
        for (int k = 0; k < nobs; ++k) {
            double pred[ZYLIA_MICS], mean = 0.0;
            for (int i = 0; i < ZYLIA_MICS; ++i) {
                double ex = (double)src_m[k][0] - m[i][0], ey = (double)src_m[k][1] - m[i][1],
                       ez = (double)src_m[k][2] - m[i][2];
                pred[i] = sqrt(ex*ex + ey*ey + ez*ez) / c - arrival_s[k][i];
                mean += pred[i];
            }
            mean /= (double)ZYLIA_MICS;        /* the unknowable per-k constant, fitted out */
            for (int i = 0; i < ZYLIA_MICS; ++i) { double e = pred[i] - mean; sse += e * e; ++cnt; }
        }
        *resid_us = (float)(sqrt(sse / (cnt ? cnt : 1)) * 1e6);
    }
    return 1;
}

static void zy_err(char* err, int cap, const char* msg) {
    if (err && cap > 0) { snprintf(err, (size_t)cap, "%s", msg); }
}

int zylia_survey_save(const char* path, const float caps_m[ZYLIA_MICS][3],
                      float resid_us, float radius_m, float spread, int nobs, char* err, int errcap) {
    if (!path || !caps_m) { zy_err(err, errcap, "zylia_survey_save: null argument"); return 0; }
    cJSON* root = cJSON_CreateObject();
    if (!root) { zy_err(err, errcap, "zylia_survey_save: out of memory"); return 0; }

    cJSON_AddStringToObject(root, "_comment",
        "ZM-1 capsule survey. Positions are metres, relative to the array centre, in ROOM axes "
        "(+X right, +Y up, -Z front), indexed BY ASIO INPUT CHANNEL. This file therefore encodes the "
        "channel order and the array's mounted orientation as well as the geometry. It is specific to "
        "one physical ZM-1 on one mount: re-survey if either changes.");
    cJSON_AddNumberToObject(root, "residual_us", resid_us);
    cJSON_AddNumberToObject(root, "radius_m",    radius_m);
    cJSON_AddNumberToObject(root, "spread",      spread);
    cJSON_AddNumberToObject(root, "observations", nobs);

    cJSON* arr = cJSON_AddArrayToObject(root, "capsules");
    if (!arr) { cJSON_Delete(root); zy_err(err, errcap, "zylia_survey_save: out of memory"); return 0; }
    for (int i = 0; i < ZYLIA_MICS; ++i) {
        cJSON* p = cJSON_CreateArray();
        for (int a = 0; a < 3; ++a) cJSON_AddItemToArray(p, cJSON_CreateNumber(caps_m[i][a]));
        cJSON_AddItemToArray(arr, p);
    }

    char* text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text) { zy_err(err, errcap, "zylia_survey_save: serialize failed"); return 0; }

    FILE* f = fopen(path, "wb");
    if (!f) { free(text); zy_err(err, errcap, "zylia_survey_save: cannot open file for writing"); return 0; }
    size_t n = strlen(text);
    size_t w = fwrite(text, 1, n, f);
    fclose(f);
    free(text);
    if (w != n) { zy_err(err, errcap, "zylia_survey_save: short write"); return 0; }
    return 1;
}

int zylia_survey_load(const char* path, char* err, int errcap) {
    if (!path) { zy_err(err, errcap, "zylia_survey_load: null path"); return 0; }
    FILE* f = fopen(path, "rb");
    if (!f) { zy_err(err, errcap, "zylia_survey_load: cannot open file"); return 0; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > (1 << 20)) { fclose(f); zy_err(err, errcap, "zylia_survey_load: bad file size"); return 0; }
    char* text = (char*)malloc((size_t)len + 1);
    if (!text) { fclose(f); zy_err(err, errcap, "zylia_survey_load: out of memory"); return 0; }
    size_t got = fread(text, 1, (size_t)len, f);
    fclose(f);
    text[got] = 0;

    int ok = 0;
    float caps[ZYLIA_MICS][3];
    cJSON* root = cJSON_Parse(text);
    free(text);
    if (!root) { zy_err(err, errcap, "zylia_survey_load: JSON parse error"); return 0; }

    cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "capsules");
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) != ZYLIA_MICS) {
        zy_err(err, errcap, "zylia_survey_load: 'capsules' must be an array of 19");
        goto done;
    }
    for (int i = 0; i < ZYLIA_MICS; ++i) {
        cJSON* p = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsArray(p) || cJSON_GetArraySize(p) != 3) {
            zy_err(err, errcap, "zylia_survey_load: each capsule must be [x, y, z]");
            goto done;
        }
        for (int a = 0; a < 3; ++a) {
            cJSON* v = cJSON_GetArrayItem(p, a);
            if (!cJSON_IsNumber(v)) { zy_err(err, errcap, "zylia_survey_load: non-numeric capsule component"); goto done; }
            caps[i][a] = (float)v->valuedouble;
        }
        /* A capsule metres from the array centre means a unit slip (mm vs m) or a corrupt file, and it
         * would silently wreck every DOA. Cheap to check, so check. */
        double m = sqrt((double)caps[i][0]*caps[i][0] + (double)caps[i][1]*caps[i][1] +
                        (double)caps[i][2]*caps[i][2]);
        if (m < 0.005 || m > 0.5) {
            zy_err(err, errcap, "zylia_survey_load: capsule is not on a ~50 mm shell (units wrong?)");
            goto done;
        }
    }
    zylia_set_capsules(caps);
    ok = 1;
done:
    cJSON_Delete(root);
    return ok;
}

int zylia_localize(const double arrival_s[ZYLIA_MICS], const float center[3],
                   double latency_s, double c, float pos_out[3], float* dist_out) {
    if (!arrival_s || !center || !pos_out || c <= 0.0) return 0;
    float caps[ZYLIA_MICS][3]; zylia_capsules(caps);

    double mic[ZYLIA_MICS][3], range[ZYLIA_MICS];
    for (int i = 0; i < ZYLIA_MICS; ++i) {
        mic[i][0] = center[0] + caps[i][0];
        mic[i][1] = center[1] + caps[i][1];
        mic[i][2] = center[2] + caps[i][2];
        range[i]  = c * (arrival_s[i] - latency_s);    /* known-latency absolute range to each capsule */
    }

    /* initial guess: far-field direction x mean range. */
    float d[3];
    if (!zylia_doa(arrival_s, d)) return 0;
    double mean = 0; for (int i = 0; i < ZYLIA_MICS; ++i) mean += range[i];
    mean /= ZYLIA_MICS;
    double dist0 = (mean > 1e-3) ? mean : 1.0;
    double p[3] = { center[0] + d[0]*dist0, center[1] + d[1]*dist0, center[2] + d[2]*dist0 };

    /* Gauss-Newton on residual r_i(p) = |p - mic_i| - range_i; J_i = unit(p - mic_i).
     * (sum J J^T) dp = -(sum J r_i). Mildly ill-conditioned (tiny array) but exact-data convergent. */
    for (int it = 0; it < 60; ++it) {
        double JtJ[9] = {0}, Jtr[3] = {0};
        for (int i = 0; i < ZYLIA_MICS; ++i) {
            double di[3] = { p[0]-mic[i][0], p[1]-mic[i][1], p[2]-mic[i][2] };
            double L = sqrt(di[0]*di[0] + di[1]*di[1] + di[2]*di[2]);
            if (L < 1e-9) L = 1e-9;
            double u[3] = { di[0]/L, di[1]/L, di[2]/L };
            double ri = L - range[i];
            for (int a = 0; a < 3; ++a) {
                for (int b = 0; b < 3; ++b) JtJ[a*3+b] += u[a]*u[b];
                Jtr[a] += u[a]*ri;
            }
        }
        double rhs[3] = { -Jtr[0], -Jtr[1], -Jtr[2] }, dp[3];
        if (!solve_lin(3, JtJ, rhs, dp)) break;
        p[0] += dp[0]; p[1] += dp[1]; p[2] += dp[2];
        if (dp[0]*dp[0] + dp[1]*dp[1] + dp[2]*dp[2] < 1e-18) break;
    }

    pos_out[0] = (float)p[0]; pos_out[1] = (float)p[1]; pos_out[2] = (float)p[2];
    if (dist_out) {
        double dx = p[0]-center[0], dy = p[1]-center[1], dz = p[2]-center[2];
        *dist_out = (float)sqrt(dx*dx + dy*dy + dz*dz);
    }
    return 1;
}
