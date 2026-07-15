/*
 * ambi_test.c — the 3rd-order ACN/SN3D spherical-harmonic encode (front half of the production
 * binaural monitor). Checks known SH values at the cardinal directions and basic sanity.
 */
#include "ambisonics.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", (m)); ++fails; } } while (0)
#define EQ(a, b)    (fabsf((a) - (b)) < 1e-5f)

int main(void) {
    float y[BWA_AMBI_CH];
    const float SQ3_2 = 0.8660254f, S58 = 0.7905694f, S38 = 0.6123724f;

    /* +x (front): l=1 picks out x; zonal/sectoral l2,l3 take known values */
    ambi_encode_sn3d((const float[]){ 1, 0, 0 }, y);
    CHECK(EQ(y[0], 1) && EQ(y[3], 1) && EQ(y[1], 0) && EQ(y[2], 0), "+x: W and l=1");
    CHECK(EQ(y[6], -0.5f) && EQ(y[8], SQ3_2), "+x: l=2");
    CHECK(EQ(y[13], -S38) && EQ(y[15], S58) && EQ(y[12], 0), "+x: l=3");

    /* +y (left) */
    ambi_encode_sn3d((const float[]){ 0, 1, 0 }, y);
    CHECK(EQ(y[0], 1) && EQ(y[1], 1) && EQ(y[3], 0) && EQ(y[2], 0), "+y: W and l=1");
    CHECK(EQ(y[8], -SQ3_2) && EQ(y[6], -0.5f), "+y: l=2");
    CHECK(EQ(y[9], -S58) && EQ(y[11], -S38), "+y: l=3");

    /* +z (up): only the zonal (m=0) harmonics are non-zero */
    ambi_encode_sn3d((const float[]){ 0, 0, 1 }, y);
    CHECK(EQ(y[0], 1) && EQ(y[2], 1) && EQ(y[1], 0) && EQ(y[3], 0), "+z: W and l=1");
    CHECK(EQ(y[6], 1) && EQ(y[12], 1) && EQ(y[8], 0), "+z: zonal l=2/l=3");

    /* W is always unity; all harmonics finite for an arbitrary direction */
    ambi_encode_sn3d((const float[]){ 0.5773503f, 0.5773503f, 0.5773503f }, y);
    int finite = 1;
    for (int i = 0; i < BWA_AMBI_CH; ++i) finite &= isfinite(y[i]);
    CHECK(finite && EQ(y[0], 1), "diagonal: finite and W=1");

    /* The product / sectoral harmonics (ACN 4,5,7,8,10,11,14) are EXACTLY ZERO at every cardinal
     * direction, so a sign flip in any of them — the m<0 SH-sign bug class that has bitten this repo
     * — is invisible to the cardinal checks above. Pin their signs+values at an ASYMMETRIC direction
     * (2,1,2)/3 (unit; x!=y so the m=+2 terms don't vanish either). */
    ambi_encode_sn3d((const float[]){ 0.6666667f, 0.3333333f, 0.6666667f }, y);
    CHECK(EQ(y[4],  0.3849002f), "asym: ACN4 (l2 m-2, x*y)");
    CHECK(EQ(y[5],  0.3849002f), "asym: ACN5 (l2 m-1, y*z)");
    CHECK(EQ(y[7],  0.7698004f), "asym: ACN7 (l2 m+1, x*z)");
    CHECK(EQ(y[8],  0.2886751f), "asym: ACN8 (l2 m+2, x^2-y^2)");
    CHECK(EQ(y[10], 0.5737753f), "asym: ACN10 (l3 m-2, x*y*z)");
    CHECK(EQ(y[11], 0.2494813f), "asym: ACN11 (l3 m-1)");
    CHECK(EQ(y[14], 0.4303315f), "asym: ACN14 (l3 m+2)");

    /* phonon interop: SN3D encode * ambi_phonon_scale must reproduce phonon's orthonormal SH
     * (third_party/steam-audio core/src/core/sh/spherical_harmonics.cc hardcoded constants). At
     * AmbiX front (x=1) = phonon Google +x: W=0.282095, ACN3(x)=0.488603, ACN6(l2,m0)=-0.315392,
     * ACN8(l2,m2)=0.546274 — confirms the SN3D->N3D/sqrt(4pi) match the decode needs. */
    ambi_encode_sn3d((const float[]){ 1, 0, 0 }, y);
    CHECK(EQ(y[0] * ambi_phonon_scale[0],  0.282095f), "phonon W = 0.282095");
    CHECK(EQ(y[3] * ambi_phonon_scale[3],  0.488603f), "phonon ACN3 (front) = 0.488603");
    CHECK(EQ(y[6] * ambi_phonon_scale[6], -0.315392f), "phonon ACN6 (l=2,m=0) = -0.315392");
    CHECK(EQ(y[8] * ambi_phonon_scale[8],  0.546274f), "phonon ACN8 (l=2,m=2) = 0.546274");

    /* ---- SH rotation (ambi_rot_matrix / ambi_rot_apply) ----
     * The defining property: rotating the ENCODED field equals encoding the rotated direction —
     * M(R) * Y(d) == Y(R d) — for arbitrary rotations and directions. This pins the whole
     * Ivanic-Ruedenberg recursion (every block, every sign convention) against the encode, which
     * is itself xval-pinned against scipy + phonon. Block orthogonality pins level conservation. */
    {
        uint32_t rng = 12345u;
        #define FRAND() ((float)((rng = rng*1664525u + 1013904223u) >> 8) * (1.0f/16777216.0f))
        static const int off[3] = { 0, 9, 34 };
        int prop_ok = 1, orth_ok = 1;
        for (int t = 0; t < 24; ++t) {
            const float a = (FRAND() - 0.5f) * 6.2831853f;
            const float b = (FRAND() - 0.5f) * 6.2831853f;
            const float c = (FRAND() - 0.5f) * 6.2831853f;
            const float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b), cc = cosf(c), sc = sinf(c);
            /* R = Rz(a) * Ry(b) * Rx(c) in ambi axes — an arbitrary rotation */
            const float rz[3][3] = { { ca, -sa, 0 }, { sa, ca, 0 }, { 0, 0, 1 } };
            const float ry[3][3] = { { cb, 0, sb }, { 0, 1, 0 }, { -sb, 0, cb } };
            const float rx[3][3] = { { 1, 0, 0 }, { 0, cc, -sc }, { 0, sc, cc } };
            float t1[3][3], R[3][3];
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
                float s = 0; for (int k = 0; k < 3; ++k) s += ry[i][k] * rx[k][j]; t1[i][j] = s;
            }
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
                float s = 0; for (int k = 0; k < 3; ++k) s += rz[i][k] * t1[k][j]; R[i][j] = s;
            }
            float M[BWA_SH_ROT_N];
            ambi_rot_matrix(R, M);
            for (int l = 1; l <= 3; ++l) {                     /* M M^T = I per block */
                const int dim = 2*l + 1;
                for (int i = 0; i < dim; ++i) for (int j = 0; j < dim; ++j) {
                    float s = 0;
                    for (int k = 0; k < dim; ++k) s += M[off[l-1] + i*dim + k] * M[off[l-1] + j*dim + k];
                    if (fabsf(s - (i == j ? 1.f : 0.f)) > 1e-4f) orth_ok = 0;
                }
            }
            float d[3] = { FRAND() - 0.5f, FRAND() - 0.5f, FRAND() - 0.5f };
            float dl = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
            if (dl < 0.1f) continue;
            d[0] /= dl; d[1] /= dl; d[2] /= dl;
            float rd[3] = { R[0][0]*d[0] + R[0][1]*d[1] + R[0][2]*d[2],
                            R[1][0]*d[0] + R[1][1]*d[1] + R[1][2]*d[2],
                            R[2][0]*d[0] + R[2][1]*d[1] + R[2][2]*d[2] };
            float yd[BWA_AMBI_CH], yr[BWA_AMBI_CH], ym[BWA_AMBI_CH];
            ambi_encode_sn3d(d, yd);
            ambi_encode_sn3d(rd, yr);
            ambi_rot_apply(M, yd, BWA_AMBI_CH, ym);
            for (int k = 0; k < BWA_AMBI_CH; ++k)
                if (fabsf(ym[k] - yr[k]) > 5e-4f) prop_ok = 0;
        }
        CHECK(orth_ok, "SH rotation blocks are orthogonal (M M^T = I: level conservation)");
        CHECK(prop_ok, "M(R) * encode(d) == encode(R d) for random rotations");
        #undef FRAND
    }

    /* ---- max-rE weights: per-degree constants, diffuse-energy normalization, the taper, and the
     * order-3 values (P_l at the largest zero of P_4 = 0.8611363) ---- */
    {
        const float w_want[4] = { 1.f, 0.8611363f, 0.6123336f, 0.3047470f };
        float w[BWA_AMBI_CH];
        int per_l_ok = 1, taper_ok = 1;
        for (int o = 1; o <= 3; ++o) {
            ambi_max_re_weights(o, w);
            double e = 0, n = 0;
            float wl_prev = w[0];
            for (int l = 0; l <= o; ++l) {
                const float wl = w[l*l];
                for (int k = l*l; k < (l+1)*(l+1); ++k) if (w[k] != wl) per_l_ok = 0;
                if (l > 0 && wl >= wl_prev) taper_ok = 0;
                wl_prev = wl;
                e += (2*l + 1) * (double)wl * wl;
                n += 2*l + 1;
            }
            CHECK(fabs(e - n) / n < 1e-4, "max-rE weights are diffuse-energy-normalized");
        }
        CHECK(per_l_ok, "max-rE weights are constant within each degree");
        CHECK(taper_ok, "max-rE weights taper with degree");
        ambi_max_re_weights(3, w);
        int ratio_ok = 1;
        for (int l = 1; l <= 3; ++l)
            if (fabsf(w[l*l] / w[0] - w_want[l]) > 5e-4f) ratio_ok = 0;
        CHECK(ratio_ok, "order-3 weights are P_l at the largest P_4 zero");
    }

    if (fails) { printf("ambi_test: %d FAILURES\n", fails); return 1; }
    printf("ambi_test OK (3rd-order ACN/SN3D encode, SH rotation, max-rE weights)\n");
    return 0;
}
