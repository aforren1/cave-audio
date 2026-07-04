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
#include "hull.h"

#include <math.h>
#include <string.h>

#define ALLRAD_M       240     /* virtual loudspeakers (Fibonacci sphere) */
#define ALLRAD_MAXTRI  1024    /* hull-triangle cap; overflow (extreme coplanarity) -> fall back to SAD */

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

    /* real loudspeaker directions from the layout's nominal listening point (the array centroid —
     * world-locked, like the sampling decode; the room origin canonically sits on the floor) */
    float r[BW_CHANNELS][3];
    for (uint32_t s = 0; s < N; ++s) {
        float p[3] = { L->speakers[s].pos[0] - L->ref[0],
                       L->speakers[s].pos[1] - L->ref[1],
                       L->speakers[s].pos[2] - L->ref[2] };
        float len = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
        if (len < 1e-6f) { r[s][0] = 1.f; r[s][1] = 0.f; r[s][2] = 0.f; }
        else { float inv = 1.f/len; r[s][0] = p[0]*inv; r[s][1] = p[1]*inv; r[s][2] = p[2]*inv; }
    }

    /* convex-hull triangulation of the unit speaker directions around the origin */
    int tri[ALLRAD_MAXTRI][3]; float tdet[ALLRAD_MAXTRI];
    int ntri = hull_triangulate(r, N, tri, tdet, ALLRAD_MAXTRI);
    if (ntri == 0) return 0;                                   /* degenerate / overflow -> caller keeps SAD */

    memset(decode, 0, sizeof(float) * BW_CHANNELS * BW_AMBI_CH);

    /* accumulate decode = G * D_virt: VBAP each virtual loudspeaker onto the real array, scaled by its
     * sampling-decoder row (2l+1)*Y_SN3D(virt)/M */
    for (int v = 0; v < ALLRAD_M; ++v) {
        float d[3]; fib_dir(v, ALLRAD_M, d);
        int spk[3]; float bg[3];
        if (!hull_vbap(d, r, tri, tdet, ntri, spk, bg)) continue;   /* VBAP this virtual loudspeaker */

        float a3[3]; room_to_ambi(d, a3);
        float y[BW_AMBI_CH]; ambi_encode_sn3d(a3, y);
        float row[BW_AMBI_CH];
        for (int kk = 0; kk < BW_AMBI_CH; ++kk) {
            int l = (int)floorf(sqrtf((float)kk));
            row[kk] = (float)(2*l + 1) * y[kk] / (float)ALLRAD_M;   /* D_virt row */
        }
        for (int q = 0; q < 3; ++q)
            for (int kk = 0; kk < BW_AMBI_CH; ++kk) decode[spk[q]][kk] += bg[q] * row[kk];
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
