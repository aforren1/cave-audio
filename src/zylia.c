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
