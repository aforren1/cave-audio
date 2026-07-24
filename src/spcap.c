/*
 * spcap.c — Speaker-Placement Correction Amplitude Panning. For a source we weight every speaker by
 * a smooth lobe ((1+cos)/2)^focus pointed at the source's bearing *from the listener*, scaled by a
 * per-speaker placement correction (1/local-density) so a cluster of speakers doesn't pull the image
 * toward it; then normalise for constant power and apply the source->listener distance attenuation.
 * The lobe is graceful (never identically zero across the sphere, so a source over a layout hole
 * fades rather than cutting out). focus / density-exponent are tuning knobs for the real array.
 * See docs/spatialization.md. SPCAP assumes a fixed observer; for a moving listener use DBAP.
 */
#include "spcap.h"

#include <math.h>

#define SPCAP_FOCUS    12.0f   /* lobe sharpness ((1+cos)/2)^focus; higher = tighter image, fewer speakers */
#define SPCAP_DENSITY   2.0f   /* density-kernel exponent for the placement correction */

void spcap_reset(SpcapState* s) { if (s) s->valid = 0; }

/* Rebuild the listener-relative speaker directions and the placement correction. O(N^2), but only
 * runs when the listener (or layout) actually changes — once, for a fixed observer. */
static void recompute(SpcapState* s, const float lis[3], const Layout* L, uint32_t gen) {
    const uint32_t N = L->count;
    for (uint32_t k = 0; k < N; ++k) {
        const float* p = L->speakers[k].pos;
        float dx = p[0] - lis[0], dy = p[1] - lis[1], dz = p[2] - lis[2];
        float len = sqrtf(dx * dx + dy * dy + dz * dz);
        if (len > 1e-6f) { float inv = 1.f / len; s->sdir[k][0] = dx * inv; s->sdir[k][1] = dy * inv; s->sdir[k][2] = dz * inv; }
        else             { s->sdir[k][0] = 0.f; s->sdir[k][1] = 0.f; s->sdir[k][2] = 0.f; }
    }
    for (uint32_t k = 0; k < N; ++k) {
        float dens = 0.f;
        for (uint32_t j = 0; j < N; ++j) {
            float d = s->sdir[k][0] * s->sdir[j][0] + s->sdir[k][1] * s->sdir[j][1] + s->sdir[k][2] * s->sdir[j][2];
            if (d > 0.f) dens += powf(d, SPCAP_DENSITY);     /* count near (front-hemisphere) neighbours */
        }
        s->c[k] = (dens > 0.f) ? 1.f / dens : 1.f;           /* dense -> down-weighted */
    }
    s->cached_lis[0] = lis[0]; s->cached_lis[1] = lis[1]; s->cached_lis[2] = lis[2];
    s->cached_gen = gen; s->valid = 1;
}

void spcap_gains(SpcapState* s, const float src[3], const float lis[3], const Layout* L,
                 uint32_t gen, float user_gain, float* out) {
    const uint32_t N = L->count;
    if (panner_cache_stale(s->valid, s->cached_gen, s->cached_lis, gen, lis))   /* layout changed or listener moved */
        recompute(s, lis, L, gen);

    /* source bearing from the listener */
    float sx = src[0] - lis[0], sy = src[1] - lis[1], sz = src[2] - lis[2];
    float ds = sqrtf(sx * sx + sy * sy + sz * sz);
    float dx = 0.f, dy = 0.f, dz = 0.f;                      /* source at listener -> cos 0 -> uniform-ish */
    if (ds > 1e-6f) { float inv = 1.f / ds; dx = sx * inv; dy = sy * inv; dz = sz * inv; }

    float sumr = 0.f;
    for (uint32_t k = 0; k < N; ++k) {
        float cosang = dx * s->sdir[k][0] + dy * s->sdir[k][1] + dz * s->sdir[k][2];
        float lobe = 0.5f + 0.5f * cosang;                  /* [0,1] for unit vectors, peaks toward the source */
        float raw = s->c[k] * powf(lobe, SPCAP_FOCUS);
        out[k] = raw;
        sumr += raw;
    }
    float invs = (sumr > 0.f) ? 1.f / sumr : 0.f;            /* g_i^2 = raw_i / sum -> constant power */

    float atten = atten_curve(ds, L->atten_ref_m, L->atten_rolloff, L->atten_min_lin);   /* source->listener (as DBAP) */

    float g0 = user_gain * atten;
    for (uint32_t k = 0; k < N; ++k) out[k] = g0 * sqrtf(out[k] * invs);
}
