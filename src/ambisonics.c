/* ambisonics.c — 3rd-order real SH encode (ACN/SN3D), max-rE weights, SH rotation. See ambisonics.h. */
#include "ambisonics.h"

#include <math.h>

/* SN3D-normalized real spherical harmonics in Cartesian form (unit dir; x=front, y=left, z=up).
 * Constants: sqrt(3)=1.7320508, sqrt(3)/2=0.8660254, sqrt(15)=3.8729833, sqrt(15)/2=1.9364917,
 *            sqrt(5/8)=0.7905694, sqrt(3/8)=0.6123724. */
void ambi_encode_sn3d(const float dir[3], float y[BWA_AMBI_CH]) {
    const float x = dir[0], yy = dir[1], z = dir[2];

    y[0]  = 1.0f;                                            /* l=0          */

    y[1]  = yy;                                              /* l=1, m=-1  y */
    y[2]  = z;                                               /*      m= 0  z */
    y[3]  = x;                                               /*      m=+1  x */

    y[4]  = 1.7320508f * x * yy;                             /* l=2, m=-2    */
    y[5]  = 1.7320508f * yy * z;                             /*      m=-1    */
    y[6]  = 0.5f * (3.0f * z * z - 1.0f);                    /*      m= 0    */
    y[7]  = 1.7320508f * x * z;                              /*      m=+1    */
    y[8]  = 0.8660254f * (x * x - yy * yy);                  /*      m=+2    */

    y[9]  = 0.7905694f * yy * (3.0f * x * x - yy * yy);      /* l=3, m=-3    */
    y[10] = 3.8729833f * x * yy * z;                         /*      m=-2    */
    y[11] = 0.6123724f * yy * (5.0f * z * z - 1.0f);         /*      m=-1    */
    y[12] = 0.5f * z * (5.0f * z * z - 3.0f);                /*      m= 0    */
    y[13] = 0.6123724f * x * (5.0f * z * z - 1.0f);          /*      m=+1    */
    y[14] = 1.9364917f * z * (x * x - yy * yy);              /*      m=+2    */
    y[15] = 0.7905694f * x * (x * x - 3.0f * yy * yy);       /*      m=+3    */
}

/* Sampling (projection) SH->speaker decode over a layout (see ambisonics.h). Room convention: ambi
 * front = room +z, left = room +x, up = room +y (via room_to_ambi). Directions are from the array
 * centroid (L->ref), not the origin (which canonically sits on the floor). Byte-identical to the
 * former rt.c build_bed_decode_sad — the divisions and the (2l+1)*y*invL order are preserved.
 * (fdn.c's former local copy divided by count instead of multiplying by invL, so its
 * degenerate-array fallback can differ from pre-hoist output by 1 ULP.) */
void ambi_sad_decode(const Layout* L, uint32_t count, float dec[BWA_CHANNELS][BWA_AMBI_CH]) {
    const float invL = 1.0f / (float)count;
    for (uint32_t s = 0; s < count; ++s) {
        float p[3] = { L->speakers[s].pos[0] - L->ref[0],
                       L->speakers[s].pos[1] - L->ref[1],
                       L->speakers[s].pos[2] - L->ref[2] };
        float len = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
        float dr[3];
        if (len < 1e-6f) { dr[0] = 0.f; dr[1] = 0.f; dr[2] = 1.f; }            /* degenerate: face room front */
        else { dr[0] = p[0]/len; dr[1] = p[1]/len; dr[2] = p[2]/len; }
        float ad[3]; room_to_ambi(dr, ad);                                     /* (z,x,y): ambi front=+z */
        float y[BWA_AMBI_CH];
        ambi_encode_sn3d(ad, y);
        for (int k = 0; k < BWA_AMBI_CH; ++k) {
            int l = (int)floorf(sqrtf((float)k));                              /* ACN order of channel k */
            dec[s][k] = (float)(2*l + 1) * y[k] * invL;
        }
    }
}

/* max-rE weights: w_l = P_l(r) with r the largest zero of P_{order+1}, energy-renormalized (header).
 * The roots are exact: P_2 -> 1/sqrt(3), P_3 -> sqrt(3/5), P_4 -> sqrt((15 + 2*sqrt(30))/35). */
void ambi_max_re_weights(int order, float w[BWA_AMBI_CH]) {
    if (order < 1) order = 1; else if (order > 3) order = 3;
    static const double rr[3] = { 0.5773502691896258, 0.7745966692414834, 0.8611363115940526 };
    const double r = rr[order - 1];
    const double pl[4] = { 1.0, r, 0.5 * (3.0*r*r - 1.0), 0.5 * (5.0*r*r*r - 3.0*r) };
    double e = 0.0, n = 0.0;
    for (int l = 0; l <= order; ++l) { e += (2*l + 1) * pl[l] * pl[l]; n += 2*l + 1; }
    const double g = sqrt(n / e);                       /* diffuse-energy match: fair A/B levels */
    for (int k = 0; k < BWA_AMBI_CH; ++k) {
        const int l = (int)floorf(sqrtf((float)k));
        w[k] = (l <= order) ? (float)(g * pl[l]) : 1.0f;
    }
}

/* ---- SH rotation (Ivanic & Ruedenberg 1996, with the published errata) ----
 * Block l is built from block l-1 and the l=1 matrix via the P/U/V/W recursion. Centered indices
 * throughout: block l is (2l+1)x(2l+1), entry (m', m) at [(m'+l)*(2l+1) + (m+l)]. */

/* r1 is the l=1 block (SH order m = -1,0,+1); prev is block l-1. P(i, a, b) of the recursion. */
static float rot_P(const float r1[9], const float* prev, int l, int i, int a, int b) {
    const int d = 2*l - 1;                              /* prev block dimension */
    const float* pr = prev + (a + l - 1) * d;           /* prev row a (centered) */
    if (b ==  l) return r1[(i+1)*3 + 2] * pr[d - 1] - r1[(i+1)*3 + 0] * pr[0];
    if (b == -l) return r1[(i+1)*3 + 2] * pr[0]     + r1[(i+1)*3 + 0] * pr[d - 1];
    return r1[(i+1)*3 + 1] * pr[b + l - 1];
}

void ambi_rot_matrix(const float R[3][3], float M[BWA_SH_ROT_N]) {
    /* l=1: the SH dipoles are (y, z, x), so the block is R with rows/cols permuted. This seed makes
     * M the FIELD rotation: M * encode(d) == encode(R * d) (ambi_test pins it for random rotations). */
    static const int c3[3] = { 1, 2, 0 };               /* SH index -> cartesian component */
    float* m1 = M;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            m1[i*3 + j] = R[c3[i]][c3[j]];

    const float* prev = m1;
    int off = 9;
    for (int l = 2; l <= 3; ++l) {
        float* out = M + off;
        const int dim = 2*l + 1;
        for (int a = -l; a <= l; ++a) {
            for (int b = -l; b <= l; ++b) {
                const int aa = a < 0 ? -a : a;
                const float den = (b == l || b == -l) ? (float)((2*l) * (2*l - 1))
                                                      : (float)((l + b) * (l - b));
                float acc = 0.f;
                const float u = sqrtf((float)((l + a) * (l - a)) / den);
                if (u != 0.f) acc += u * rot_P(m1, prev, l, 0, a, b);
                {   /* v * V: v = 0.5*sqrt((1+d(a,0))(l+|a|-1)(l+|a|)/den) * (1 - 2*d(a,0)) */
                    const float v = 0.5f * sqrtf((float)((1 + (a == 0)) * (l + aa - 1) * (l + aa)) / den)
                                  * (a == 0 ? -1.f : 1.f);
                    float V;
                    if      (a == 0)  V = rot_P(m1, prev, l, 1, 1, b) + rot_P(m1, prev, l, -1, -1, b);
                    else if (a == 1)  V = 1.41421356f * rot_P(m1, prev, l, 1, 0, b);
                    else if (a >  1)  V = rot_P(m1, prev, l, 1, a - 1, b) - rot_P(m1, prev, l, -1, -a + 1, b);
                    else if (a == -1) V = 1.41421356f * rot_P(m1, prev, l, -1, 0, b);
                    else              V = rot_P(m1, prev, l, 1, a + 1, b) + rot_P(m1, prev, l, -1, -a - 1, b);
                    acc += v * V;
                }
                if (a != 0) {   /* w * W: w = -0.5*sqrt((l-|a|-1)(l-|a|)/den); zero at |a| >= l-1 */
                    const float w = -0.5f * sqrtf((float)((l - aa - 1) * (l - aa)) / den);
                    if (w != 0.f) {
                        const float W = (a > 0)
                            ? rot_P(m1, prev, l, 1, a + 1, b) + rot_P(m1, prev, l, -1, -a - 1, b)
                            : rot_P(m1, prev, l, 1, a - 1, b) - rot_P(m1, prev, l, -1, -a + 1, b);
                        acc += w * W;
                    }
                }
                out[(a + l) * dim + (b + l)] = acc;
            }
        }
        prev = out;
        off += dim * dim;
    }
}

void ambi_rot_apply(const float M[BWA_SH_ROT_N], const float* sh, int nch, float* out) {
    static const int off[3] = { 0, 9, 34 };
    out[0] = sh[0];
    const int maxl = nch >= 16 ? 3 : (nch >= 9 ? 2 : 1);
    for (int l = 1; l <= maxl; ++l) {
        const int dim = 2*l + 1, base = l*l;
        const float* m = M + off[l - 1];
        for (int i = 0; i < dim; ++i) {
            float acc = 0.f;
            for (int j = 0; j < dim; ++j) acc += m[i*dim + j] * sh[base + j];
            out[base + i] = acc;
        }
    }
}

/* sqrt(2l+1)/sqrt(4pi) per ACN channel — SN3D (AmbiX) -> phonon's orthonormal real SH (see header).
 * 1/sqrt(4pi)=0.2820948, sqrt(3)/sqrt(4pi)=0.4886025, sqrt(5)/sqrt(4pi)=0.6307832, sqrt(7)/..=0.7463527. */
const float ambi_phonon_scale[BWA_AMBI_CH] = {
    0.2820948f,                                                                     /* l=0 */
    0.4886025f, 0.4886025f, 0.4886025f,                                             /* l=1 */
    0.6307832f, 0.6307832f, 0.6307832f, 0.6307832f, 0.6307832f,                     /* l=2 */
    0.7463527f, 0.7463527f, 0.7463527f, 0.7463527f, 0.7463527f, 0.7463527f, 0.7463527f  /* l=3 */
};

/* Monitor-basis encode (see header): the phonon net-AmbiX axis map (front=-z_room, left=-x_room,
 * up=+y_room — steam_decode.c CONVENTION 1) followed by the orthonormal rescale. Everything summed
 * into the monitor's ambisonic field (the virtual-speaker matrix AND rt.c's direct-binaural bus)
 * uses this one function, so the two contributions cannot drift apart in convention. */
void ambi_encode_phonon(const float room_dir[3], float y[BWA_AMBI_CH]) {
    float a[3] = { -room_dir[2], -room_dir[0], room_dir[1] };
    ambi_encode_sn3d(a, y);
    for (int k = 0; k < BWA_AMBI_CH; ++k) y[k] *= ambi_phonon_scale[k];
}

/* Diagonal converting an engine-canonical ACN/SN3D FIELD (room_to_ambi axes: front=+z_room,
 * left=+x_room — beds, after rotation) into the phonon monitor basis above. The two axis maps
 * differ by a pi rotation about up, whose real-SH action is diagonal ((-1)^|m| per channel — both
 * cos and sin m-harmonics flip for odd m); compose with the per-degree orthonormal rescale and the
 * whole basis change is one multiply per channel. monitor_test pins this against ambi_encode_phonon
 * over random directions, so the table cannot silently drift from the encode. */
const float ambi_canon_to_phonon[BWA_AMBI_CH] = {
     0.2820948f,                                                                        /* l=0        */
    -0.4886025f,  0.4886025f, -0.4886025f,                                              /* l=1 m -101 */
     0.6307832f, -0.6307832f,  0.6307832f, -0.6307832f,  0.6307832f,                    /* l=2        */
    -0.7463527f,  0.7463527f, -0.7463527f,  0.7463527f, -0.7463527f,  0.7463527f, -0.7463527f  /* l=3 */
};
