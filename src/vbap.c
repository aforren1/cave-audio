/* vbap.c — see vbap.h. */
#include "vbap.h"
#include "hull.h"
#include "dbap.h"      /* fallback for a non-triangulable array / source-at-listener */

#include <math.h>

void vbap_reset(VbapState* s) { if (s) s->valid = 0; }

/* Rebuild the listener-relative speaker directions + their hull triangulation. Runs only when the
 * listener or layout changes — once, for a fixed observer. */
static void recompute(VbapState* s, const float lis[3], const Layout* L, uint32_t gen) {
    const uint32_t N = L->count;
    for (uint32_t k = 0; k < N; ++k) {
        const float* p = L->speakers[k].pos;
        float dx = p[0] - lis[0], dy = p[1] - lis[1], dz = p[2] - lis[2];
        float len = sqrtf(dx * dx + dy * dy + dz * dz);
        if (len > 1e-6f) { float inv = 1.f / len; s->sdir[k][0] = dx * inv; s->sdir[k][1] = dy * inv; s->sdir[k][2] = dz * inv; }
        else             { s->sdir[k][0] = 0.f; s->sdir[k][1] = 0.f; s->sdir[k][2] = 1.f; }
    }
    s->ntri = hull_triangulate(s->sdir, N, s->tri, s->det, VBAP_MAXTRI);   /* 0 -> DBAP fallback in gains() */
    s->cached_lis[0] = lis[0]; s->cached_lis[1] = lis[1]; s->cached_lis[2] = lis[2];
    s->cached_gen = gen; s->valid = 1;
}

void vbap_gains(VbapState* s, const float src[3], const float lis[3], const Layout* L,
                uint32_t gen, float user_gain, float* out) {
    const uint32_t N = L->count;
    if (!s->valid || s->cached_gen != gen ||
        fabsf(s->cached_lis[0] - lis[0]) > 1e-4f ||
        fabsf(s->cached_lis[1] - lis[1]) > 1e-4f ||
        fabsf(s->cached_lis[2] - lis[2]) > 1e-4f)
        recompute(s, lis, L, gen);

    float sx = src[0] - lis[0], sy = src[1] - lis[1], sz = src[2] - lis[2];
    float ds = sqrtf(sx * sx + sy * sy + sz * sz);
    int spk[3]; float g[3];
    if (s->ntri == 0 || ds <= 1e-6f) { dbap_gains(src, lis, L, user_gain, out); return; }  /* degenerate / at listener */
    float inv = 1.f / ds, d[3] = { sx * inv, sy * inv, sz * inv };
    if (!hull_vbap(d, s->sdir, s->tri, s->det, s->ntri, spk, g)) { dbap_gains(src, lis, L, user_gain, out); return; }

    for (uint32_t k = 0; k < N; ++k) out[k] = 0.f;             /* VBAP: only the triangle's speakers sound */

    float atten = 1.f;                                         /* source->listener distance attenuation (as DBAP) */
    if (L->atten_ref_m > 0.f) {
        float dd = (ds > L->atten_ref_m) ? ds : L->atten_ref_m;
        atten = powf(L->atten_ref_m / dd, L->atten_rolloff);
        if (atten < L->atten_min_lin) atten = L->atten_min_lin;
        if (atten > 1.f) atten = 1.f;
    }
    float gg = user_gain * atten;
    for (int q = 0; q < 3; ++q) out[spk[q]] = gg * g[q];
}
