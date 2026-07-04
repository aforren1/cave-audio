/*
 * zylia.c — single-position ZM-1 speaker localization. See zylia.h for the model + accuracy notes.
 */
#include "zylia.h"

#include <math.h>
#include <string.h>

#define ZYLIA_RADIUS_M 0.049f   /* ZM-1 is ~98 mm diameter */

void zylia_geometry(float dirs[ZYLIA_MICS][3], float* radius_m) {
    /* PLACEHOLDER: 19-point Fibonacci-sphere spread (well-distributed, deterministic). REPLACE this
     * loop's output with the ZM-1 datasheet/surveyed capsule directions before on-hardware use — the
     * localization math is geometry-agnostic, but the DOA is only correct if these match the real array. */
    const double golden = 2.399963229728653;   /* pi*(3 - sqrt 5) */
    for (int i = 0; i < ZYLIA_MICS; ++i) {
        double y  = 1.0 - 2.0 * ((double)i + 0.5) / (double)ZYLIA_MICS;   /* 1 .. -1 */
        double r  = sqrt(fmax(0.0, 1.0 - y * y));
        double th = golden * (double)i;
        dirs[i][0] = (float)(r * cos(th));
        dirs[i][1] = (float)y;
        dirs[i][2] = (float)(r * sin(th));
    }
    if (radius_m) *radius_m = ZYLIA_RADIUS_M;
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
    float dirs[ZYLIA_MICS][3]; float R; zylia_geometry(dirs, &R);
    /* Far field: tau_i = A - (R/c)(dir_i . d). Fit tau_i = A + b.dir_i (4 unknowns) by least squares;
     * b = -(R/c) d points AWAY from the source, so d = -normalize(b). Latency folds into A and cancels. */
    double M[16] = {0}, rhs[4] = {0};
    for (int i = 0; i < ZYLIA_MICS; ++i) {
        double row[4] = { 1.0, dirs[i][0], dirs[i][1], dirs[i][2] };
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

int zylia_localize(const double arrival_s[ZYLIA_MICS], const float center[3],
                   double latency_s, double c, float pos_out[3], float* dist_out) {
    if (!arrival_s || !center || !pos_out || c <= 0.0) return 0;
    float dirs[ZYLIA_MICS][3]; float R; zylia_geometry(dirs, &R);

    double mic[ZYLIA_MICS][3], range[ZYLIA_MICS];
    for (int i = 0; i < ZYLIA_MICS; ++i) {
        mic[i][0] = center[0] + R * dirs[i][0];
        mic[i][1] = center[1] + R * dirs[i][1];
        mic[i][2] = center[2] + R * dirs[i][2];
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
