/* hole.c — see hole.h. */
#include "hole.h"

#include <math.h>

#define HOLE_HALF_PI 1.5707963267948966f

/* The knee is clamped before it is used. Both bounds are guards, not tuning:
 *  - below ~10 degrees a "hole" the size of the knee is inside the panner's own phantom accuracy,
 *    and a near-zero knee would let float noise in the direction cache raise a floor on a
 *    fully-covered array;
 *  - above 60 degrees the span (pi/2 - knee) collapses and the ramp turns into a step. An array
 *    that sparse has no point-source rendering left to protect anyway. */
#define HOLE_KNEE_MIN 0.17453293f    /* 10 degrees */
#define HOLE_KNEE_MAX 1.04719755f    /* 60 degrees */

void hole_reset(HoleState* s) { if (s) s->valid = 0; }

void hole_block(HoleState* s, const Layout* L, const float lis[3], uint32_t gen) {
    const uint32_t N = L->count;
    /* two tiers: the knee follows the LAYOUT (generation), the directions follow the LISTENER. A
     * walking listener therefore never re-runs the O(N^2) spacing measurement. */
    const int layout_changed = (!s->valid || s->cached_gen != gen || s->count != N);
    if (!layout_changed && !panner_cache_stale(s->valid, s->cached_gen, s->cached_lis, gen, lis))
        return;

    if (layout_changed) {
        float knee = layout_mean_speaker_spacing(L);
        if (!(knee > HOLE_KNEE_MIN)) knee = HOLE_KNEE_MIN;      /* also catches a degenerate 0 / NaN */
        else if (knee > HOLE_KNEE_MAX) knee = HOLE_KNEE_MAX;
        s->knee     = knee;
        s->inv_span = 1.f / (HOLE_HALF_PI - knee);
    }
    for (uint32_t k = 0; k < N; ++k)
        unit_dir(lis, L->speakers[k].pos, s->sdir[k]);           /* degenerate -> (0,0,1) */

    s->cached_lis[0] = lis[0]; s->cached_lis[1] = lis[1]; s->cached_lis[2] = lis[2];
    s->cached_gen = gen; s->count = N; s->valid = 1;
}

float hole_floor(const HoleState* s, const float u_s[3]) {
    const uint32_t N = s->count;
    if (!s->valid || N == 0) return 0.f;

    float best = -2.f;                                           /* largest cos = smallest angle */
    for (uint32_t k = 0; k < N; ++k) {
        const float d = u_s[0]*s->sdir[k][0] + u_s[1]*s->sdir[k][1] + u_s[2]*s->sdir[k][2];
        if (d > best) best = d;
    }
    if (best >  1.f) best =  1.f;                                /* acosf of 1+eps is NaN */
    if (best < -1.f) best = -1.f;

    const float f = (acosf(best) - s->knee) * s->inv_span;
    if (f <= 0.f) return 0.f;
    return f < 1.f ? f : 1.f;
}
