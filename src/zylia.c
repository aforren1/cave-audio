/*
 * zylia.c — single-position ZM-1 speaker localization. See zylia.h for the model + accuracy notes.
 */
#include "zylia.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "ambisonics.h"   /* the engine's own xval-pinned ACN/SN3D basis, for the intensity DOA */
#include "fft.h"          /* offline analysis FFT (header-only); NOT for the audio thread */

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
    /* Far field: tau_i = A - (1/c)(m_i . d), m_i = capsule position relative to the center. Fit
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
            for (int k = -1; k <= 1; ++k) {                 /* recompute the neighbors (cheaper than storing the curve) */
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
            if (frac >  0.5) frac =  0.5;                   /* the true peak is between the neighbors */
            if (frac < -0.5) frac = -0.5;
        }                                                   /* boundary peak: keep the integer lag as-is */
        arrival_s[ch] = ((double)best + frac) / fs;
    }
    return 1;
}

/* ==============================================================================================
 * Validation-grade DOA: first-order active intensity in the SH domain. See zylia.h for the model,
 * the band limit, and why this exists alongside the TDOA path.
 * ============================================================================================== */

#define ZY_NFFT 2048        /* analysis frame; 42.7 ms / 23.4 Hz bins at 48 kHz */
#define ZY_REG  1e-6        /* Tikhonov floor, RELATIVE to the best-conditioned order at that bin */

/* Spherical Bessel functions of the first (j) and second (y) kind, orders 0..3, by CLOSED FORM.
 * Not by the upward recurrence: that is unstable for j_n once n > x, and our ka runs down to ~0.02
 * at the bottom of the band — precisely where it would come apart. Four orders is few enough to
 * just write out, and then there is no stability question to argue about. */
static void zy_sph_jy(double x, double j[4], double y[4]) {
    if (x < 1e-4) {                                   /* leading small-argument terms; y_n diverges
                                                       * like x^-(n+1) and the regularized inversion
                                                       * discards those bins anyway. */
        j[0] = 1.0 - x*x/6.0; j[1] = x/3.0; j[2] = x*x/15.0; j[3] = x*x*x/105.0;
        y[0] = y[1] = y[2] = y[3] = -1e12;
        return;
    }
    double s = sin(x), c = cos(x), x2 = x*x, x3 = x2*x, x4 = x3*x;
    j[0] =  s/x;
    j[1] =  s/x2 - c/x;
    j[2] = ( 3.0/x3 - 1.0/x)*s - ( 3.0/x2)*c;
    j[3] = (15.0/x4 - 6.0/x2)*s - (15.0/x3 - 1.0/x)*c;
    y[0] = -c/x;
    y[1] = -c/x2 - s/x;
    y[2] = (-3.0/x3 + 1.0/x)*c - ( 3.0/x2)*s;
    y[3] = (-15.0/x4 + 6.0/x2)*c - (15.0/x3 - 1.0/x)*s;
}

/* Rigid-sphere mode strength for orders 0..3.
 *
 *   b_n(ka) = 4pi i^n ( j_n - (j_n'/h_n') h_n ),   h_n = j_n - i y_n  (2nd kind)
 *
 * The bracket collapses via the Wronskian j_n h_n' - j_n' h_n = -i/x^2, leaving
 *
 *   b_n(ka) = 4pi i^n (-i) / ( (ka)^2 h_n'(ka) )
 *
 * which needs only the DERIVATIVE, so the near-cancellation in the original bracket never happens.
 * i^n(-i) cycles -i, 1, i, -1 for n = 0..3. Derivatives from j_n' = j_{n-1} - ((n+1)/x) j_n with
 * j_{-1} = cos(x)/x, y_{-1} = sin(x)/x. */
static void zy_mode_strength(double ka, double br[4], double bi[4]) {
    double j[4], y[4];
    zy_sph_jy(ka, j, y);
    double x = (ka < 1e-6) ? 1e-6 : ka;
    double jm1 = cos(x)/x, ym1 = sin(x)/x;            /* order -1, to seed the derivative recurrence */
    const double numr[4] = { 0.0, 1.0, 0.0, -1.0 };   /* 4pi factored out below */
    const double numi[4] = { -1.0, 0.0, 1.0, 0.0 };
    for (int nn = 0; nn < 4; ++nn) {
        double jp = (nn == 0 ? jm1 : j[nn-1]) - ((double)nn + 1.0)/x * j[nn];
        double yp = (nn == 0 ? ym1 : y[nn-1]) - ((double)nn + 1.0)/x * y[nn];
        double dr = x*x*jp, di = -x*x*yp;             /* (ka)^2 * h_n'  =  x^2 (j_n' - i y_n') */
        double den = dr*dr + di*di;
        if (den < 1e-300) { br[nn] = bi[nn] = 0.0; continue; }
        double nr = 4.0*M_PI*numr[nn], ni = 4.0*M_PI*numi[nn];
        br[nn] =  ( nr*dr + ni*di) / den;             /* num / den, complex */
        bi[nn] = -( ni*dr - nr*di) / den;             /* ...CONJUGATED -> h^(1). See below. */
    }
    /* WHY THE CONJUGATE, i.e. why h^(1) and not the h^(2) the formula above is written in.
     *
     * b_n's convention has to match the transform's. fft.h's forward direction (dir = +1) carries the
     * twiddle e^{+i2*pi*nk/N}, so a DELAY of tau comes back as e^{+i*w*tau} — that is the e^{-iwt}
     * time convention, whose outgoing wave is h^(1), not the h^(2) the Wronskian form above assumes.
     * Since j and y are real, h^(1) = conj(h^(2)) and the whole mode strength simply conjugates.
     *
     * Get this backwards and NOTHING looks broken: |b_n| is identical, so levels, diffuseness and the
     * conditioning are all unchanged, and the DOA comes back exactly 180 deg out. Exactly 180, because
     * the residual factor is cos(2(theta_0 - theta_1)) and at low ka consecutive degrees sit 90 deg
     * apart (b_n ~ i^n x real), which lands the cosine on -1. A confident, clean, backwards answer.
     *
     * This is the same class of bug as the steam_decode DC-polarity incident (see CLAUDE.md): it
     * cannot be caught by synthesizing test input from the model being inverted, because the error
     * cancels. zylia_test.c pins it with a pure geometric-delay forward model and a cross-check
     * against the TDOA estimator instead. Do not "simplify" that test into an SH round-trip. */
}

/* Least-squares projector from the INCLUDED capsule pressures onto ACN/SN3D channels 0..nsh-1:
 * M = (Y^T Y + ridge)^-1 Y^T, with Y[i][k] = Y_k(capsule i) and an excluded capsule contributing a
 * zero row (so its column of M comes back zero — but callers should skip it outright rather than
 * lean on that, since 0 * NaN is NaN and a broken capsule is exactly where NaNs come from).
 *
 * Quadrature weighting would assume a uniform sampling; the ZM-1 is a dodecahedron MINUS its nadir,
 * so it is not uniform and least squares is the honest form. Geometry-only, so it is built once per
 * call and reused across every bin. Reads zylia_geometry, which follows an installed capsule SURVEY
 * — so a surveyed array feeds these estimators its measured channel order and orientation for free.
 *
 * The ridge is what makes order 3 usable at all: 16 unknowns from 19 capsules on a solid that is no
 * spherical design is a tight, ill-conditioned fit, and the ridge trades a little bias for not
 * amplifying capsule noise into the high-order channels. It is scaled by the normal matrix's own
 * trace, so it is scale-free, and it is negligible at order 1.
 * Returns 1 on success, 0 if too few capsules remain for this order or the solve is singular. */
static int zy_build_projector(int nsh, const unsigned char* exclude,
                              double M[BWA_AMBI_CH][ZYLIA_MICS], int* ncap_out) {
    if (nsh < 1 || nsh > BWA_AMBI_CH) return 0;
    float dirs[ZYLIA_MICS][3], R;
    zylia_geometry(dirs, &R);
    double Y[ZYLIA_MICS][BWA_AMBI_CH];
    int ncap = 0;
    for (int i = 0; i < ZYLIA_MICS; ++i) {
        int drop = (exclude && exclude[i]);
        /* room (+x right, +y up, -z front) -> ambisonic (x front, y left, z up) */
        float a[3] = { -dirs[i][2], -dirs[i][0], dirs[i][1] };
        float sh[BWA_AMBI_CH];
        ambi_encode_sn3d(a, sh);
        for (int k = 0; k < nsh; ++k) Y[i][k] = drop ? 0.0 : (double)sh[k];
        if (!drop) ++ncap;
    }
    if (ncap_out) *ncap_out = ncap;
    if (ncap < nsh + 2) return 0;                     /* refuse to fit a direction out of nothing */

    double G[BWA_AMBI_CH * BWA_AMBI_CH], tr = 0.0;
    for (int r = 0; r < nsh; ++r)
        for (int cc = 0; cc < nsh; ++cc) {
            double s = 0.0;
            for (int i = 0; i < ZYLIA_MICS; ++i) s += Y[i][r] * Y[i][cc];
            G[r*nsh + cc] = s;
            if (r == cc) tr += s;
        }
    for (int r = 0; r < nsh; ++r) G[r*nsh + r] += 1e-6 * tr / (double)nsh;

    double Ginv[BWA_AMBI_CH][BWA_AMBI_CH];
    for (int k = 0; k < nsh; ++k) {                   /* invert column by column (solve_lin eats A) */
        double A[BWA_AMBI_CH * BWA_AMBI_CH], b[BWA_AMBI_CH] = { 0 }, xs[BWA_AMBI_CH] = { 0 };
        memcpy(A, G, sizeof(double) * (size_t)nsh * (size_t)nsh);
        b[k] = 1.0;
        if (!solve_lin(nsh, A, b, xs)) return 0;
        for (int r = 0; r < nsh; ++r) Ginv[r][k] = xs[r];
    }
    for (int k = 0; k < nsh; ++k)
        for (int i = 0; i < ZYLIA_MICS; ++i) {
            double s = 0.0;
            for (int r = 0; r < nsh; ++r) s += Ginv[k][r] * Y[i][r];
            M[k][i] = s;
        }
    return 1;
}

/* ACN channel -> its SH degree l (0,1,1,1,2,2,2,2,2,3,...). */
static int zy_acn_degree(int k) { int l = (int)sqrt((double)k); return (l > 3) ? 3 : l; }

int zylia_intensity_doa(const float* const x[ZYLIA_MICS], uint32_t n, double fs, double c,
                        double f_lo, double f_hi, const unsigned char* exclude,
                        float dir_out[3], float* diffuseness_out) {
    if (!x || !dir_out || fs <= 0.0 || c <= 0.0) return 0;
    if (f_lo <= 0.0 || f_hi <= f_lo || n < (uint32_t)ZY_NFFT) return 0;
    for (int i = 0; i < ZYLIA_MICS; ++i) if (!x[i]) return 0;

    if (f_hi > ZYLIA_FOA_FMAX) f_hi = ZYLIA_FOA_FMAX;   /* above kr~1 the inversion is not trustworthy */
    if (f_hi <= f_lo) return 0;                         /* band lay entirely above it: REFUSE */

    /* Excluded capsules are skipped OUTRIGHT, not merely zero-weighted: a capsule flagged faulty is
     * exactly the one liable to carry NaNs, and 0 * NaN is NaN. */
    int inc[ZYLIA_MICS], ninc = 0;
    for (int i = 0; i < ZYLIA_MICS; ++i) if (!(exclude && exclude[i])) inc[ninc++] = i;

    float dirs[ZYLIA_MICS][3], R;
    zylia_geometry(dirs, &R);
    double M[BWA_AMBI_CH][ZYLIA_MICS];
    if (!zy_build_projector(4, exclude, M, NULL)) return 0;

    const int N = ZY_NFFT, H = ZY_NFFT / 2;
    int k_lo = (int)ceil (f_lo * (double)N / fs);
    int k_hi = (int)floor(f_hi * (double)N / fs);
    if (k_lo < 1) k_lo = 1;
    if (k_hi > N/2 - 1) k_hi = N/2 - 1;
    if (k_hi < k_lo) return 0;

    double* re = (double*)malloc(sizeof(double) * (size_t)ZYLIA_MICS * (size_t)N);
    double* im = (double*)malloc(sizeof(double) * (size_t)ZYLIA_MICS * (size_t)N);
    if (!re || !im) { free(re); free(im); return 0; }

    double Ix = 0.0, Iy = 0.0, Iz = 0.0, Esum = 0.0;
    int frames = 0;
    for (uint32_t off = 0; off + (uint32_t)N <= n; off += (uint32_t)H) {
        for (int e = 0; e < ninc; ++e) {
            int ch = inc[e];
            double* r = re + (size_t)ch * N, *m = im + (size_t)ch * N;
            for (int t = 0; t < N; ++t) {
                double w = 0.5 - 0.5 * cos(2.0 * M_PI * (double)t / (double)(N - 1));   /* Hann */
                r[t] = w * (double)x[ch][off + (uint32_t)t];
                m[t] = 0.0;
            }
            fft(r, m, N, +1);
        }
        for (int k = k_lo; k <= k_hi; ++k) {
            double Pr[4], Pi[4];
            for (int q = 0; q < 4; ++q) {                     /* capsule pressures -> 4 SH channels */
                double sr = 0.0, si = 0.0;
                for (int e = 0; e < ninc; ++e) {
                    int ch = inc[e];
                    sr += M[q][ch] * re[(size_t)ch * N + k];
                    si += M[q][ch] * im[(size_t)ch * N + k];
                }
                Pr[q] = sr; Pi[q] = si;
            }
            double f  = (double)k * fs / (double)N;
            double ka = 2.0 * M_PI * f * (double)R / c;
            double br[4], bi[4];
            zy_mode_strength(ka, br, bi);
            double m0 = br[0]*br[0] + bi[0]*bi[0], m1 = br[1]*br[1] + bi[1]*bi[1];
            double lam = ZY_REG * (m0 > m1 ? m0 : m1);        /* relative -> scale-free */
            double Ar[4], Ai[4];
            for (int q = 0; q < 4; ++q) {                     /* A = P conj(b) / (|b|^2 + lam) */
                int deg = (q == 0) ? 0 : 1;
                double d = br[deg]*br[deg] + bi[deg]*bi[deg] + lam;
                if (d < 1e-300) { Ar[q] = Ai[q] = 0.0; continue; }
                Ar[q] = (Pr[q]*br[deg] + Pi[q]*bi[deg]) / d;
                Ai[q] = (Pi[q]*br[deg] - Pr[q]*bi[deg]) / d;
            }
            /* W = ACN 0; the first-order triple in ambisonic axes is (front, left, up) = ACN (3,1,2).
             * In SN3D those ARE the direction cosines, so Re{conj(W).V} points AT the source. */
            double Wr = Ar[0], Wi = Ai[0];
            double Vr[3] = { Ar[3], Ar[1], Ar[2] }, Vi[3] = { Ai[3], Ai[1], Ai[2] };
            Ix += Wr*Vr[0] + Wi*Vi[0];
            Iy += Wr*Vr[1] + Wi*Vi[1];
            Iz += Wr*Vr[2] + Wi*Vi[2];
            Esum += Wr*Wr + Wi*Wi + Vr[0]*Vr[0] + Vi[0]*Vi[0]
                  + Vr[1]*Vr[1] + Vi[1]*Vi[1] + Vr[2]*Vr[2] + Vi[2]*Vi[2];
        }
        ++frames;
    }
    free(re); free(im);
    if (!frames) return 0;

    double mag = sqrt(Ix*Ix + Iy*Iy + Iz*Iz);
    if (mag < 1e-30) return 0;
    double ax = Ix/mag, ay = Iy/mag, az = Iz/mag;     /* ambisonic -> room */
    dir_out[0] = (float)(-ay);
    dir_out[1] = (float)( az);
    dir_out[2] = (float)(-ax);
    if (diffuseness_out) {
        /* plane wave: |I| = |W|^2 and E = 2|W|^2, so the ratio is 1 and psi lands at 0. */
        double d = 1.0 - mag / (0.5 * Esum + 1e-30);
        *diffuseness_out = (float)(d < 0.0 ? 0.0 : (d > 1.0 ? 1.0 : d));
    }
    return 1;
}

/* ==============================================================================================
 * Per-session signal integrity. See zylia.h for what this catches and why nothing downstream can.
 * ============================================================================================== */

#define ZY_DEAD_RATIO 0.10      /* -20 dB vs the array's median RMS */
#define ZY_HOT_RATIO  10.0      /* +20 dB; the real-world fault this exists for was ~45 dB */
#define ZY_CLIP_LEVEL 0.999
#define ZY_CLIP_FRAC  1e-4      /* fraction of samples pinned before it counts as clipping */
#define ZY_COH_MIN    0.50      /* peak normalized correlation against the array consensus */
#define ZY_COH_LAG    24        /* lag search (samples); the array's own span is ~14 at 48 kHz */
#define ZY_COH_N      16384u    /* cap the coherence window — a fault check, not a measurement */

/* median of n doubles, in place (n = 19 here, so an insertion sort is the right tool). */
static double zy_median(double* v, int n) {
    for (int i = 1; i < n; ++i) {
        double k = v[i]; int j = i - 1;
        while (j >= 0 && v[j] > k) { v[j+1] = v[j]; --j; }
        v[j+1] = k;
    }
    return (n & 1) ? v[n/2] : 0.5 * (v[n/2 - 1] + v[n/2]);
}

int zylia_check_capsules(const float* const x[ZYLIA_MICS], uint32_t n,
                         unsigned char flags_out[ZYLIA_MICS]) {
    if (!x || !flags_out || n < 256u) return -1;
    for (int i = 0; i < ZYLIA_MICS; ++i) if (!x[i]) return -1;

    double rms[ZYLIA_MICS];
    int nanch[ZYLIA_MICS], clipch[ZYLIA_MICS];
    for (int ch = 0; ch < ZYLIA_MICS; ++ch) {
        double s = 0.0; uint32_t clip = 0; int bad = 0;
        for (uint32_t i = 0; i < n; ++i) {
            double v = (double)x[ch][i];
            if (!(v == v) || v > 1e30 || v < -1e30) { bad = 1; break; }   /* NaN / inf */
            s += v * v;
            if (fabs(v) >= ZY_CLIP_LEVEL) ++clip;
        }
        nanch[ch]  = bad;
        rms[ch]    = bad ? 0.0 : sqrt(s / (double)n);
        clipch[ch] = (!bad && clip > 4u && (double)clip > ZY_CLIP_FRAC * (double)n);
    }

    double tmp[ZYLIA_MICS];
    memcpy(tmp, rms, sizeof tmp);
    double med = zy_median(tmp, ZYLIA_MICS);          /* robust: one fault cannot move it */

    for (int ch = 0; ch < ZYLIA_MICS; ++ch) {
        unsigned char f = 0;
        if (nanch[ch])  f |= ZYLIA_CAP_DEAD | ZYLIA_CAP_CLIPPED;   /* not finite = not usable */
        if (clipch[ch]) f |= ZYLIA_CAP_CLIPPED;
        if (med > 0.0 && !nanch[ch]) {
            if (rms[ch] < ZY_DEAD_RATIO * med) f |= ZYLIA_CAP_DEAD;
            if (rms[ch] > ZY_HOT_RATIO  * med) f |= ZYLIA_CAP_HOT;
        }
        flags_out[ch] = f;
    }

    /* Coherence against the array's own per-sample MEDIAN signal — the case where a capsule sits at
     * a perfectly ordinary level while carrying the wrong signal, which no level check can see. The
     * median is the reference precisely because it is robust: a faulty capsule cannot corrupt the
     * baseline it is being judged against. The lag search covers the array's real inter-capsule
     * delay, so a healthy off-axis capsule is not mistaken for an incoherent one. */
    uint32_t nc = (n < ZY_COH_N) ? n : ZY_COH_N;
    if (med > 0.0 && nc > (uint32_t)(2 * ZY_COH_LAG + 256)) {
        double* cons = (double*)malloc(sizeof(double) * nc);
        if (cons) {
            for (uint32_t i = 0; i < nc; ++i) {
                double col[ZYLIA_MICS]; int m = 0;
                for (int ch = 0; ch < ZYLIA_MICS; ++ch)
                    if (!nanch[ch]) col[m++] = (double)x[ch][i];
                cons[i] = m ? zy_median(col, m) : 0.0;
            }
            uint32_t i0 = (uint32_t)ZY_COH_LAG, i1 = nc - (uint32_t)ZY_COH_LAG;
            double ce = 0.0;
            for (uint32_t i = i0; i < i1; ++i) ce += cons[i] * cons[i];
            if (ce > 0.0) {
                for (int ch = 0; ch < ZYLIA_MICS; ++ch) {
                    if (nanch[ch] || (flags_out[ch] & ZYLIA_CAP_DEAD)) continue;  /* already reported */
                    double best = 0.0;
                    for (int lag = -ZY_COH_LAG; lag <= ZY_COH_LAG; ++lag) {
                        double num = 0.0, xe = 0.0;
                        for (uint32_t i = i0; i < i1; ++i) {
                            double a = (double)x[ch][(uint32_t)((int)i + lag)];
                            num += a * cons[i];
                            xe  += a * a;
                        }
                        if (xe <= 0.0) continue;
                        double r = num / sqrt(xe * ce);
                        if (r > best) best = r;
                    }
                    if (best < ZY_COH_MIN) flags_out[ch] |= ZYLIA_CAP_INCOHERENT;
                }
            }
            free(cons);
        }
    }

    int nbad = 0;
    for (int ch = 0; ch < ZYLIA_MICS; ++ch) if (flags_out[ch]) ++nbad;
    return nbad;
}

/* ==============================================================================================
 * SH-domain steered-response power, PHAT-whitened. The independent cross-check (see zylia.h).
 * ============================================================================================== */

#define ZY_SRP_DIRS 4096        /* Fibonacci sphere: ~3.4 deg spacing, so ~1.7 deg worst-case */

int zylia_srp_doa(const float* const x[ZYLIA_MICS], uint32_t n, double fs, double c,
                  double f_lo, double f_hi, const unsigned char* exclude, float dir_out[3]) {
    if (!x || !dir_out || fs <= 0.0 || c <= 0.0) return 0;
    if (f_lo <= 0.0 || f_hi <= f_lo || n < (uint32_t)ZY_NFFT) return 0;
    for (int i = 0; i < ZYLIA_MICS; ++i) if (!x[i]) return 0;
    if (f_hi > ZYLIA_SH3_FMAX) f_hi = ZYLIA_SH3_FMAX;
    if (f_hi <= f_lo) return 0;

    int inc[ZYLIA_MICS], ninc = 0;
    for (int i = 0; i < ZYLIA_MICS; ++i) if (!(exclude && exclude[i])) inc[ninc++] = i;

    /* Highest order the SURVIVING capsules support. Stepping down rather than over-fitting is the
     * whole point: an under-determined order-3 solve returns a confident direction built of noise. */
    double M[BWA_AMBI_CH][ZYLIA_MICS];
    int order, nsh = 0;
    for (order = 3; order >= 1; --order) {
        nsh = (order + 1) * (order + 1);
        if (zy_build_projector(nsh, exclude, M, NULL)) break;
    }
    if (order < 1) return 0;

    float dirs[ZYLIA_MICS][3], R;
    zylia_geometry(dirs, &R);

    const int N = ZY_NFFT, H = ZY_NFFT / 2;
    int k_lo = (int)ceil (f_lo * (double)N / fs);
    int k_hi = (int)floor(f_hi * (double)N / fs);
    if (k_lo < 1) k_lo = 1;
    if (k_hi > N/2 - 1) k_hi = N/2 - 1;
    if (k_hi < k_lo) return 0;

    float*  Yd = (float*) malloc(sizeof(float)  * (size_t)ZY_SRP_DIRS * BWA_AMBI_CH);
    float*  Dd = (float*) malloc(sizeof(float)  * (size_t)ZY_SRP_DIRS * 3);
    double* P  = (double*)calloc((size_t)ZY_SRP_DIRS, sizeof(double));
    double* re = (double*)malloc(sizeof(double) * (size_t)ZYLIA_MICS * (size_t)N);
    double* im = (double*)malloc(sizeof(double) * (size_t)ZYLIA_MICS * (size_t)N);
    if (!Yd || !Dd || !P || !re || !im) { free(Yd); free(Dd); free(P); free(re); free(im); return 0; }

    for (int d = 0; d < ZY_SRP_DIRS; ++d) {           /* even directions, golden angle */
        /* DOUBLE-precision Fibonacci sphere (deliberately NOT the shared float fib_sphere_dir in
         * ambisonics.h): this grid feeds the SRP-PHAT / Gauss-Newton estimators whose tests pin
         * sub-degree accuracy, so the extra precision is load-bearing. */
        double yy = 1.0 - 2.0 * ((double)d + 0.5) / (double)ZY_SRP_DIRS;
        double rr = sqrt(1.0 - yy*yy < 0.0 ? 0.0 : 1.0 - yy*yy), th = (double)d * 2.399963229728653;
        float rd[3] = { (float)(rr * cos(th)), (float)yy, (float)(rr * sin(th)) };   /* room axes */
        Dd[d*3+0] = rd[0]; Dd[d*3+1] = rd[1]; Dd[d*3+2] = rd[2];
        float a[3] = { -rd[2], -rd[0], rd[1] };
        ambi_encode_sn3d(a, &Yd[(size_t)d * BWA_AMBI_CH]);
    }

    int frames = 0;
    for (uint32_t off = 0; off + (uint32_t)N <= n; off += (uint32_t)H) {
        for (int e = 0; e < ninc; ++e) {
            int ch = inc[e];
            double* r = re + (size_t)ch * N, *m = im + (size_t)ch * N;
            for (int t = 0; t < N; ++t) {
                double w = 0.5 - 0.5 * cos(2.0 * M_PI * (double)t / (double)(N - 1));
                r[t] = w * (double)x[ch][off + (uint32_t)t];
                m[t] = 0.0;
            }
            fft(r, m, N, +1);
        }
        for (int k = k_lo; k <= k_hi; ++k) {
            double f = (double)k * fs / (double)N, ka = 2.0 * M_PI * f * (double)R / c;
            double br[4], bi[4];
            zy_mode_strength(ka, br, bi);
            double mmax = 0.0;
            for (int l = 0; l <= order; ++l) {
                double m = br[l]*br[l] + bi[l]*bi[l];
                if (m > mmax) mmax = m;
            }
            double lam = ZY_REG * mmax;               /* also rolls high orders off where ka can't
                                                       * support them: |b_l|^2 << lam -> suppressed */
            double Ar[BWA_AMBI_CH], Ai[BWA_AMBI_CH], nrm = 0.0;
            for (int q = 0; q < nsh; ++q) {
                double sr = 0.0, si = 0.0;
                for (int e = 0; e < ninc; ++e) {
                    int ch = inc[e];
                    sr += M[q][ch] * re[(size_t)ch * N + k];
                    si += M[q][ch] * im[(size_t)ch * N + k];
                }
                int l = zy_acn_degree(q);
                double d2 = br[l]*br[l] + bi[l]*bi[l] + lam;
                Ar[q] = (sr*br[l] + si*bi[l]) / d2;
                Ai[q] = (si*br[l] - sr*bi[l]) / d2;
                nrm += Ar[q]*Ar[q] + Ai[q]*Ai[q];
            }
            if (nrm < 1e-30) continue;
            double g = 1.0 / sqrt(nrm);               /* PHAT: every bin contributes unit norm, so a
                                                       * loud bin cannot outvote the rest */
            for (int q = 0; q < nsh; ++q) { Ar[q] *= g; Ai[q] *= g; }
            for (int d = 0; d < ZY_SRP_DIRS; ++d) {
                const float* Y = &Yd[(size_t)d * BWA_AMBI_CH];
                double sr = 0.0, si = 0.0;
                for (int q = 0; q < nsh; ++q) { sr += (double)Y[q]*Ar[q]; si += (double)Y[q]*Ai[q]; }
                P[d] += sr*sr + si*si;
            }
        }
        ++frames;
    }

    int bestd = -1;
    double bestp = -1.0;
    for (int d = 0; d < ZY_SRP_DIRS; ++d) if (P[d] > bestp) { bestp = P[d]; bestd = d; }
    if (frames && bestd >= 0) {
        dir_out[0] = Dd[bestd*3+0]; dir_out[1] = Dd[bestd*3+1]; dir_out[2] = Dd[bestd*3+2];
    }
    free(Yd); free(Dd); free(P); free(re); free(im);
    return (frames && bestd >= 0) ? 1 : 0;
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
         * vertex sums to the zenith, so the centroid sits R/19 = 2.6 mm ABOVE the sphere center. Nobody
         * tape-measures to the centroid. Fit the sphere the capsules actually lie on — algebraically,
         * |p-q|^2 = r^2  ->  2 p_i . q - k = |p_i|^2 with k = |q|^2 - r^2, linear in four unknowns —
         * and re-center on q, giving the physical center of the shell: the point the operator measured
         * the clap positions from, and what zylia_localize's `center` argument means.
         *
         * This happens INSIDE the loop, not after it: the near-field correction above needs m in the
         * same frame src_m is measured in. Re-centering only at the end would leave the correction
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

/* ---- tracked mount helpers (see zylia.h) ----------------------------------------------------- */

void zylia_quat_to_matrix(const float q[4], float R[9]) {
    if (!q || !R) return;
    double x = q[0], y = q[1], z = q[2], w = q[3];
    double n = sqrt(x*x + y*y + z*z + w*w);
    if (n < 1e-12) {                                  /* degenerate: hand back identity rather than NaN */
        R[0]=1;R[1]=0;R[2]=0; R[3]=0;R[4]=1;R[5]=0; R[6]=0;R[7]=0;R[8]=1;
        return;
    }
    x/=n; y/=n; z/=n; w/=n;
    R[0] = (float)(1.0 - 2.0*(y*y + z*z));  R[1] = (float)(2.0*(x*y - w*z));        R[2] = (float)(2.0*(x*z + w*y));
    R[3] = (float)(2.0*(x*y + w*z));        R[4] = (float)(1.0 - 2.0*(x*x + z*z));  R[5] = (float)(2.0*(y*z - w*x));
    R[6] = (float)(2.0*(x*z - w*y));        R[7] = (float)(2.0*(y*z + w*x));        R[8] = (float)(1.0 - 2.0*(x*x + y*y));
}

void zylia_capsules_rotate(const float caps_in[ZYLIA_MICS][3], const float R[9], int transpose,
                           float caps_out[ZYLIA_MICS][3]) {
    if (!caps_in || !R || !caps_out) return;
    for (int i = 0; i < ZYLIA_MICS; ++i) {
        float v[3] = { caps_in[i][0], caps_in[i][1], caps_in[i][2] };   /* copy: in may alias out */
        if (transpose) {
            caps_out[i][0] = R[0]*v[0] + R[3]*v[1] + R[6]*v[2];
            caps_out[i][1] = R[1]*v[0] + R[4]*v[1] + R[7]*v[2];
            caps_out[i][2] = R[2]*v[0] + R[5]*v[1] + R[8]*v[2];
        } else {
            caps_out[i][0] = R[0]*v[0] + R[1]*v[1] + R[2]*v[2];
            caps_out[i][1] = R[3]*v[0] + R[4]*v[1] + R[5]*v[2];
            caps_out[i][2] = R[6]*v[0] + R[7]*v[1] + R[8]*v[2];
        }
    }
}

int zylia_survey_save(const char* path, const float caps_m[ZYLIA_MICS][3],
                      float resid_us, float radius_m, float spread, int nobs,
                      const ZyliaMount* mount, char* err, int errcap) {
    if (!path || !caps_m) { zy_err(err, errcap, "zylia_survey_save: null argument"); return 0; }
    cJSON* root = cJSON_CreateObject();
    if (!root) { zy_err(err, errcap, "zylia_survey_save: out of memory"); return 0; }

    /* The prose has to match the frame, because this is the artifact a human opens to decide whether
     * the file is safe to reuse — a body-frame survey that claims to be room axes invites exactly the
     * mistake the frame field exists to prevent. */
    cJSON_AddStringToObject(root, "_comment",
        (mount && mount->body_frame)
        ? "ZM-1 capsule survey. Positions are meters, relative to the array center, in the TRACKED "
          "MOUNT'S BODY AXES (not room axes), indexed BY ASIO INPUT CHANNEL. Rotate by the mount's "
          "live pose before solving: installed as-is, every direction is wrong by whatever the mount "
          "was turned to at survey time. Encodes the channel order and the capsules' placement inside "
          "the mount, NOT the array's orientation in the room — that is what the tracker supplies. "
          "Specific to one physical ZM-1 rigidly coupled to one mount: re-survey if either changes."
        : "ZM-1 capsule survey. Positions are meters, relative to the array center, in ROOM axes "
          "(+X right, +Y up, -Z front), indexed BY ASIO INPUT CHANNEL. This file therefore encodes the "
          "channel order and the array's mounted orientation as well as the geometry. It is specific to "
          "one physical ZM-1 on one mount: re-survey if either changes.");
    cJSON_AddNumberToObject(root, "residual_us", resid_us);
    cJSON_AddNumberToObject(root, "radius_m",    radius_m);
    cJSON_AddNumberToObject(root, "spread",      spread);
    cJSON_AddNumberToObject(root, "observations", nobs);
    if (mount && mount->body_frame) {
        /* Body-frame survey: the capsules are expressed in the TRACKED MOUNT's axes, so this file is
         * no longer tied to one orientation — rotate it by the live pose at each placement. It is
         * still tied to one physical ZM-1 on one mount (channel order + the rigid coupling). */
        cJSON_AddStringToObject(root, "frame", "body");
        if (mount->have_offset) {
            cJSON* o = cJSON_AddArrayToObject(root, "mount_offset_m");
            for (int a = 0; a < 3; ++a) cJSON_AddItemToArray(o, cJSON_CreateNumber(mount->offset_m[a]));
        }
    }

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

int zylia_survey_load(const char* path, ZyliaMount* mount_out, char* err, int errcap) {
    if (mount_out) memset(mount_out, 0, sizeof *mount_out);   /* absent fields => a room-axes survey */
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
        /* A capsule meters from the array center means a unit slip (mm vs m) or a corrupt file, and it
         * would silently wreck every DOA. Cheap to check, so check. */
        double m = sqrt((double)caps[i][0]*caps[i][0] + (double)caps[i][1]*caps[i][1] +
                        (double)caps[i][2]*caps[i][2]);
        if (m < 0.005 || m > 0.5) {
            zy_err(err, errcap, "zylia_survey_load: capsule is not on a ~50 mm shell (units wrong?)");
            goto done;
        }
    }
    {   /* optional tracked-mount metadata; a file without these is the historical room-axes form */
        cJSON* fr = cJSON_GetObjectItemCaseSensitive(root, "frame");
        int body = (cJSON_IsString(fr) && fr->valuestring && !strcmp(fr->valuestring, "body"));
        cJSON* off = cJSON_GetObjectItemCaseSensitive(root, "mount_offset_m");
        if (mount_out) {
            mount_out->body_frame = body;
            if (cJSON_IsArray(off) && cJSON_GetArraySize(off) == 3) {
                int good = 1;
                for (int a = 0; a < 3; ++a) {
                    cJSON* v = cJSON_GetArrayItem(off, a);
                    if (!cJSON_IsNumber(v)) { good = 0; break; }
                    mount_out->offset_m[a] = (float)v->valuedouble;
                }
                mount_out->have_offset = good;
            }
        } else if (body) {
            /* The caller cannot learn this table is body-frame, so it would be installed and solved
             * against as if it were room axes — every direction wrong by the mount's survey-time
             * orientation. Refuse rather than answer confidently. */
            zy_err(err, errcap, "zylia_survey_load: body-frame survey needs a ZyliaMount out-param");
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
