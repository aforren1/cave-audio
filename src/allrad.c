/*
 * allrad.c — see allrad.h. Three stages, all at load time:
 *   1. a dense uniform VIRTUAL layout (Fibonacci sphere) + its sampling decoder D_virt;
 *   2. VBAP each virtual loudspeaker onto the real array via the array's convex-hull triangulation;
 *   3. accumulate decode = G * D_virt and energy-normalise to the sampling decode's diffuse level.
 * The virtual layer is uniform so the sampling decoder is well-behaved there; VBAP absorbs the real
 * array's irregularity. (Standard AllRAD, Zotter & Frank 2012, with a Fibonacci virtual layer in
 * place of a t-design and a brute-force hull — fine for 26 points.)
 */
#include "allrad.h"

#include <math.h>
#include <string.h>

#define ALLRAD_M       240     /* virtual loudspeakers (Fibonacci sphere) */
#define ALLRAD_MAXTRI  1024    /* hull-triangle cap; overflow (extreme coplanarity) -> fall back to SAD */

static float dot3(const float a[3], const float b[3]) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
static void  cross3(const float a[3], const float b[3], float o[3]) {
    o[0] = a[1]*b[2] - a[2]*b[1]; o[1] = a[2]*b[0] - a[0]*b[2]; o[2] = a[0]*b[1] - a[1]*b[0];
}
static float triple(const float a[3], const float b[3], const float c[3]) {  /* a . (b x c) */
    float bc[3]; cross3(b, c, bc); return dot3(a, bc);
}
static void fib_dir(int i, int M, float d[3]) {
    float y = 1.f - 2.f * ((float)i + 0.5f) / (float)M;
    float r = sqrtf(fmaxf(0.f, 1.f - y * y)), th = (float)i * 2.39996323f;
    d[0] = r * cosf(th); d[1] = y; d[2] = r * sinf(th);
}
/* room (x=right,y=up,z=back) -> ambisonic axes (x=front,y=left,z=up): (-z,-x,y) — matches build_bed_decode */
static void room_to_ambi(const float d[3], float a[3]) { a[0] = -d[2]; a[1] = -d[0]; a[2] = d[1]; }

int allrad_build_decode(const Layout* L, float decode[BW_CHANNELS][BW_AMBI_CH]) {
    const uint32_t N = L->count;
    if (N < 4 || N > BW_CHANNELS) return 0;

    /* real loudspeaker directions from the room origin (world-locked, like the sampling decode) */
    float r[BW_CHANNELS][3];
    for (uint32_t s = 0; s < N; ++s) {
        const float* p = L->speakers[s].pos;
        float len = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
        if (len < 1e-6f) { r[s][0] = 1.f; r[s][1] = 0.f; r[s][2] = 0.f; }
        else { float inv = 1.f/len; r[s][0] = p[0]*inv; r[s][1] = p[1]*inv; r[s][2] = p[2]*inv; }
    }

    /* convex-hull faces (triangles) of the unit directions around the origin: a triple is a face if
     * every other point lies on the inner side of its (outward-oriented) plane. O(N^4), fine for N<=26. */
    int tri[ALLRAD_MAXTRI][3]; float tdet[ALLRAD_MAXTRI]; int ntri = 0;
    for (uint32_t i = 0; i < N; ++i)
    for (uint32_t j = i + 1; j < N; ++j)
    for (uint32_t k = j + 1; k < N; ++k) {
        float e1[3] = { r[j][0]-r[i][0], r[j][1]-r[i][1], r[j][2]-r[i][2] };
        float e2[3] = { r[k][0]-r[i][0], r[k][1]-r[i][1], r[k][2]-r[i][2] };
        float nrm[3]; cross3(e1, e2, nrm);
        float nl = sqrtf(dot3(nrm, nrm));
        if (nl < 1e-9f) continue;                              /* colinear triple */
        nrm[0]/=nl; nrm[1]/=nl; nrm[2]/=nl;
        float off = dot3(nrm, r[i]);
        if (off < 0.f) { nrm[0]=-nrm[0]; nrm[1]=-nrm[1]; nrm[2]=-nrm[2]; off=-off; }  /* orient outward */
        int face = 1;
        for (uint32_t m = 0; m < N; ++m) if (dot3(nrm, r[m]) > off + 1e-5f) { face = 0; break; }
        if (face) {
            if (ntri >= ALLRAD_MAXTRI) return 0;        /* hull too large (degenerate array): caller keeps SAD */
            tri[ntri][0]=(int)i; tri[ntri][1]=(int)j; tri[ntri][2]=(int)k;
            tdet[ntri] = triple(r[i], r[j], r[k]);
            ++ntri;
        }
    }
    if (ntri == 0) return 0;                                   /* not triangulable (e.g. coplanar array) */

    memset(decode, 0, sizeof(float) * BW_CHANNELS * BW_AMBI_CH);

    /* accumulate decode = G * D_virt: VBAP each virtual loudspeaker onto the real array, scaled by its
     * sampling-decoder row (2l+1)*Y_SN3D(virt)/M */
    for (int v = 0; v < ALLRAD_M; ++v) {
        float d[3]; fib_dir(v, ALLRAD_M, d);

        /* find the hull triangle containing direction d (largest min barycentric/VBAP gain) */
        int bt = -1; float bmin = -2.f, bg[3] = { 0, 0, 0 };
        for (int t = 0; t < ntri; ++t) {
            if (fabsf(tdet[t]) < 1e-9f) continue;
            const float *a = r[tri[t][0]], *b = r[tri[t][1]], *c = r[tri[t][2]];
            float inv = 1.f / tdet[t];
            float g0 = triple(d, b, c) * inv;                  /* Cramer's rule: g = [a b c]^-1 d */
            float g1 = triple(a, d, c) * inv;
            float g2 = triple(a, b, d) * inv;
            float mn = g0 < g1 ? (g0 < g2 ? g0 : g2) : (g1 < g2 ? g1 : g2);
            if (mn > bmin) { bmin = mn; bt = t; bg[0]=g0; bg[1]=g1; bg[2]=g2; }
        }
        if (bt < 0) continue;
        /* clamp + L2-normalise the VBAP gains (constant power per virtual source) */
        for (int q = 0; q < 3; ++q) if (bg[q] < 0.f) bg[q] = 0.f;
        float gn = sqrtf(bg[0]*bg[0] + bg[1]*bg[1] + bg[2]*bg[2]);
        if (gn < 1e-9f) continue;
        bg[0]/=gn; bg[1]/=gn; bg[2]/=gn;

        float a3[3]; room_to_ambi(d, a3);
        float y[BW_AMBI_CH]; ambi_encode_sn3d(a3, y);
        float row[BW_AMBI_CH];
        for (int kk = 0; kk < BW_AMBI_CH; ++kk) {
            int l = (int)floorf(sqrtf((float)kk));
            row[kk] = (float)(2*l + 1) * y[kk] / (float)ALLRAD_M;   /* D_virt row */
        }
        for (int q = 0; q < 3; ++q) {
            int sp = tri[bt][q];
            for (int kk = 0; kk < BW_AMBI_CH; ++kk) decode[sp][kk] += bg[q] * row[kk];
        }
    }

    /* energy-normalise to the sampling decode's diffuse level (so swapping decoders keeps the bed
     * loudness): diffuse energy = sum_{s,k} D[s][k]^2 / (2l+1); the sampling decode's is 84/(4pi*N). */
    double e_all = 0.0;
    for (uint32_t s = 0; s < N; ++s)
        for (int kk = 0; kk < BW_AMBI_CH; ++kk) {
            int l = (int)floorf(sqrtf((float)kk));
            e_all += (double)decode[s][kk] * decode[s][kk] / (double)(2*l + 1);
        }
    if (e_all <= 0.0) return 0;
    /* sampling decode's diffuse energy under this metric: sum_{s,k} ((2l+1)Y/N)^2/(2l+1) = sum_l(2l+1)/N
     * = BW_AMBI_CH/N  (since sum_m Y_lm^SN3D(dir)^2 = 1, summed over speakers ~ N/(2l+1) each channel). */
    double e_sad = (double)BW_AMBI_CH / (double)N;
    float scale = (float)sqrt(e_sad / e_all);
    for (uint32_t s = 0; s < N; ++s)
        for (int kk = 0; kk < BW_AMBI_CH; ++kk) decode[s][kk] *= scale;

    return 1;
}
