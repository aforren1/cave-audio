/*
 * epad.c — see epad.h. The build, all at load time:
 *   1. Y: the N3D encode matrix of the speaker directions (EPAD's energy property holds in the
 *      orthonormal basis; SN3D rows rescale by sqrt(2l+1));
 *   2. eigensolve A = YYᵀ (16×16 symmetric, cyclic Jacobi in double) and form the truncated
 *      inverse square root B = QΛ^(-1/2)Qᵀ — eigenvalues below 1e-6·λmax are DROPPED, which is
 *      the rank truncation of Zotter/Pomberger/Noisternig's SVD formulation (D = c·VUᵀ equals
 *      c·Yᵀ(YYᵀ)^(-1/2) on the kept subspace);
 *   3. D = Yᵀ·B, rescaled back to SN3D input and energy-normalised to the sampling decode's
 *      diffuse level (Σ D²/(2l+1) = BWA_AMBI_CH/N, the same metric allrad.c matches).
 */
#include "epad.h"

#include <math.h>
#include <string.h>

/* room (+z fwd, +y up, -x right) -> ambisonic axes (x=front,y=left,z=up): (z,x,y) — matches build_bed_decode */
static void room_to_ambi(const float d[3], float a[3]) { a[0] = d[2]; a[1] = d[0]; a[2] = d[1]; }

/* Cyclic Jacobi eigensolve of a symmetric n×n matrix in place: A -> diag(eigenvalues), V -> the
 * eigenvectors (columns). Classic textbook form, double precision; n = 16 converges in a few sweeps. */
static void jacobi_eig(double A[BWA_AMBI_CH][BWA_AMBI_CH], double V[BWA_AMBI_CH][BWA_AMBI_CH], int n) {
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) V[i][j] = (i == j) ? 1.0 : 0.0;
    for (int sweep = 0; sweep < 64; ++sweep) {
        double off = 0.0;
        for (int p = 0; p < n - 1; ++p)
            for (int q = p + 1; q < n; ++q) off += A[p][q] * A[p][q];
        if (off < 1e-24) return;
        for (int p = 0; p < n - 1; ++p)
            for (int q = p + 1; q < n; ++q) {
                if (fabs(A[p][q]) < 1e-30) continue;
                double theta = (A[q][q] - A[p][p]) / (2.0 * A[p][q]);
                double t = (theta >= 0.0 ? 1.0 : -1.0) / (fabs(theta) + sqrt(theta * theta + 1.0));
                double cth = 1.0 / sqrt(t * t + 1.0), sth = t * cth;
                for (int k = 0; k < n; ++k) {
                    double akp = A[k][p], akq = A[k][q];
                    A[k][p] = cth * akp - sth * akq;
                    A[k][q] = sth * akp + cth * akq;
                }
                for (int k = 0; k < n; ++k) {
                    double apk = A[p][k], aqk = A[q][k];
                    A[p][k] = cth * apk - sth * aqk;
                    A[q][k] = sth * apk + cth * aqk;
                }
                for (int k = 0; k < n; ++k) {
                    double vkp = V[k][p], vkq = V[k][q];
                    V[k][p] = cth * vkp - sth * vkq;
                    V[k][q] = sth * vkp + cth * vkq;
                }
            }
    }
}

int epad_build_decode(const Layout* L, float decode[BWA_CHANNELS][BWA_AMBI_CH]) {
    const uint32_t N = L->count;
    if (N < 4 || N > BWA_CHANNELS) return 0;

    /* per-degree N3D<->SN3D scale */
    double n3d[BWA_AMBI_CH];
    for (int k = 0; k < BWA_AMBI_CH; ++k) {
        int l = (int)floorf(sqrtf((float)k));
        n3d[k] = sqrt((double)(2 * l + 1));
    }

    /* Y: N3D encode of the speaker directions from the layout's nominal listening point */
    double Y[BWA_AMBI_CH][BWA_CHANNELS];
    for (uint32_t s = 0; s < N; ++s) {
        float p[3] = { L->speakers[s].pos[0] - L->ref[0],
                       L->speakers[s].pos[1] - L->ref[1],
                       L->speakers[s].pos[2] - L->ref[2] };
        float len = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
        float d[3];
        if (len < 1e-6f) { d[0] = 0.f; d[1] = 0.f; d[2] = 1.f; }             /* degenerate: face front */
        else { float inv = 1.f/len; d[0] = p[0]*inv; d[1] = p[1]*inv; d[2] = p[2]*inv; }
        float a3[3]; room_to_ambi(d, a3);
        float y[BWA_AMBI_CH]; ambi_encode_sn3d(a3, y);
        for (int k = 0; k < BWA_AMBI_CH; ++k) Y[k][s] = (double)y[k] * n3d[k];
    }

    /* A = YYᵀ -> Q Λ Qᵀ, then B = Q Λ^(-1/2) Qᵀ over the kept eigenvalues */
    double A[BWA_AMBI_CH][BWA_AMBI_CH], Q[BWA_AMBI_CH][BWA_AMBI_CH];
    for (int i = 0; i < BWA_AMBI_CH; ++i)
        for (int j = 0; j < BWA_AMBI_CH; ++j) {
            double acc = 0.0;
            for (uint32_t s = 0; s < N; ++s) acc += Y[i][s] * Y[j][s];
            A[i][j] = acc;
        }
    jacobi_eig(A, Q, BWA_AMBI_CH);
    double lmax = 0.0;
    for (int i = 0; i < BWA_AMBI_CH; ++i) if (A[i][i] > lmax) lmax = A[i][i];
    if (lmax <= 0.0) return 0;
    double isq[BWA_AMBI_CH];
    for (int i = 0; i < BWA_AMBI_CH; ++i)                    /* truncate: drop unreproducible components */
        isq[i] = (A[i][i] > 1e-6 * lmax) ? 1.0 / sqrt(A[i][i]) : 0.0;
    double B[BWA_AMBI_CH][BWA_AMBI_CH];
    for (int i = 0; i < BWA_AMBI_CH; ++i)
        for (int j = 0; j < BWA_AMBI_CH; ++j) {
            double acc = 0.0;
            for (int k = 0; k < BWA_AMBI_CH; ++k) acc += Q[i][k] * isq[k] * Q[j][k];
            B[i][j] = acc;
        }

    /* D = Yᵀ·B back to SN3D input, then energy-normalise to the sampling decode's diffuse level */
    memset(decode, 0, sizeof(float) * BWA_CHANNELS * BWA_AMBI_CH);
    double e = 0.0;
    for (uint32_t s = 0; s < N; ++s)
        for (int k = 0; k < BWA_AMBI_CH; ++k) {
            double acc = 0.0;
            for (int i = 0; i < BWA_AMBI_CH; ++i) acc += Y[i][s] * B[i][k];
            acc *= n3d[k];                                   /* accept SN3D signals: D_SN3D = D_N3D·diag(√(2l+1)) */
            decode[s][k] = (float)acc;
            e += acc * acc / (n3d[k] * n3d[k]);              /* the allrad.c diffuse metric: Σ D²/(2l+1) */
        }
    if (e <= 0.0) return 0;
    float scale = (float)sqrt(((double)BWA_AMBI_CH / (double)N) / e);
    for (uint32_t s = 0; s < N; ++s)
        for (int k = 0; k < BWA_AMBI_CH; ++k) decode[s][k] *= scale;

    return 1;
}
