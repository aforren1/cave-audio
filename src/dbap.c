/*
 * dbap.c — listener-relative DBAP. For each speaker we combine (a) DBAP proximity to the
 * source, blurred by r to control spread, with (b) a directional weight toward the
 * source's bearing *from the listener* so off-centre listeners still get a correct
 * distribution; then normalise for constant power and scale by user gain and the
 * source->listener distance attenuation. The exponents/r/curve are tuning knobs to dial
 * against the real array (docs/spatialization.md). This is the M4 first cut.
 */
#include "dbap.h"

#include <math.h>

static float dist2(const float a[3], const float b[3]) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

void dbap_gains(const float src[3], const float lis[3], const Layout* L,
                float user_gain, float* out) {
    const uint32_t N = L->count;
    const float r2 = L->rolloff_r * L->rolloff_r;
    const float a  = 2.0f;                              /* DBAP rolloff exponent: gain ~ 1/d^a */

    /* source bearing from the listener */
    float sx = src[0] - lis[0], sy = src[1] - lis[1], sz = src[2] - lis[2];
    float ds = sqrtf(sx * sx + sy * sy + sz * sz);
    int   have_dir = ds > 1e-6f;
    float sdx = 0.f, sdy = 0.f, sdz = 0.f;
    if (have_dir) { float inv = 1.f / ds; sdx = sx * inv; sdy = sy * inv; sdz = sz * inv; }

    float norm2 = 0.f;
    for (uint32_t k = 0; k < N; ++k) {
        const float* spk = L->speakers[k].pos;
        float dk = sqrtf(dist2(src, spk) + r2);         /* blurred source->speaker distance */
        float g  = 1.f / powf(dk, a);                   /* proximity weight */
        if (have_dir) {
            float kx = spk[0] - lis[0], ky = spk[1] - lis[1], kz = spk[2] - lis[2];
            float kl = sqrtf(kx * kx + ky * ky + kz * kz);
            float cosang = (kl > 1e-6f) ? (sdx * kx + sdy * ky + sdz * kz) / kl : 1.f;
            if (cosang < -1.f) cosang = -1.f; else if (cosang > 1.f) cosang = 1.f;
            g *= 0.5f + 0.5f * cosang;                  /* toward the source's bearing, never < 0 */
        }
        out[k] = g;
        norm2 += g * g;
    }

    float scale = (norm2 > 0.f) ? 1.f / sqrtf(norm2) : 0.f;   /* constant power */

    float atten = atten_curve(ds, L->atten_ref_m, L->atten_rolloff, L->atten_min_lin);   /* source->listener distance attenuation */

    float s = user_gain * atten * scale;
    for (uint32_t k = 0; k < N; ++k) out[k] *= s;
}
