/* cap.c — see cap.h. */
#include "cap.h"
#include "frame.h"     /* frame_qrot + BWA_ROOM_RIGHT: the interaural axis under the room convention */

#include <math.h>
#include <string.h>

void cap_reset(CapState* s) { if (s) s->valid = 0; }

void cap_block(CapState* s, const Layout* L, const float lis[3], const float q[4], uint32_t gen) {
    const uint32_t N = L->count;
    if (s->valid && s->cached_gen == gen && s->count == N &&
        memcmp(s->cached_lis, lis, sizeof s->cached_lis) == 0 &&
        memcmp(s->cached_q,   q,   sizeof s->cached_q)   == 0) return;

    /* interaural axis in ROOM space. Rotating THIS by the head pose (rather than rotating the
     * speakers into head space) is what keeps the SPCAP/VBAP position-keyed caches warm through a
     * pure head turn. A degenerate pose falls back to the identity axis rather than collapsing e to
     * zero, which would make every a_k identical and silently disable the correction. */
    float n[4] = { q[0], q[1], q[2], q[3] };
    float qn = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2] + n[3]*n[3]);
    if (qn < 1e-6f) { n[0] = n[1] = n[2] = 0.f; n[3] = 1.f; }
    else { float inv = 1.f/qn; n[0] *= inv; n[1] *= inv; n[2] *= inv; n[3] *= inv; }
    frame_qrot(n, BWA_ROOM_RIGHT, s->e);

    for (uint32_t k = 0; k < N; ++k) {
        float u[3];
        unit_dir(lis, L->speakers[k].pos, u);            /* degenerate -> (0,0,1) */
        s->ce[k] = u[0]*s->e[0] + u[1]*s->e[1] + u[2]*s->e[2];
    }
    memcpy(s->cached_lis, lis, sizeof s->cached_lis);
    memcpy(s->cached_q,   q,   sizeof s->cached_q);
    s->cached_gen = gen; s->count = N; s->valid = 1;
}

void cap_gains_lo(const CapState* s, const float* g0, const float u_s[3], float strength, float* out) {
    const uint32_t N = s->count;
    const float cs = u_s[0]*s->e[0] + u_s[1]*s->e[1] + u_s[2]*s->e[2];   /* the real source's interaural cosine */

    /* Blend the ITD target from the source's own bearing toward the SEED's bearing as `strength`
     * falls. strength 1 = exact ITD; strength 0 makes the target the seed's own rV.e, so a'g0 lands
     * at zero to float rounding and the solve is a no-op down to that (that is the engulfing-source
     * case: an exact ITD on a near-uniform gain vector would pull it to one side and undo the
     * widening). Exactly zero in exact arithmetic; ~1e-16 in floats, which is why the test asserts
     * agreement with plain dual-band to a tolerance rather than bit-identity. */
    double sg = 0.0, sce = 0.0, gp = 0.0;
    for (uint32_t k = 0; k < N; ++k) {
        sg  += g0[k];
        sce += (double)g0[k] * s->ce[k];
        gp  += (double)g0[k] * g0[k];
    }
    if (sg <= 1e-9 || gp <= 0.0) {                        /* silent voice: nothing to correct */
        for (uint32_t k = 0; k < N; ++k) out[k] = g0[k];
        return;
    }
    const double rv0 = sce / sg;                          /* the seed's own interaural component */
    double cs_eff = (double)strength * cs + (1.0 - (double)strength) * rv0;

    /* Clamp the target into the range the SEED'S OWN speakers can actually reach. rV.e is a convex
     * combination of their ce with non-negative weights, so a target outside [min ce, max ce] has no
     * feasible solution and the iteration below would drive every gain to zero chasing it. Clamping
     * degrades to "as lateral as this speaker set goes" instead. This is NOT a rare corner: on the
     * default grid from the room CENTER it binds at 2 of 24 yaw angles, whenever the head turns to
     * put the source within ~11 deg of the interaural axis while the nearest speaker sits 15 deg
     * off it. Being off-center or near a wall only widens the region. */
    double ce_lo = 1.0, ce_hi = -1.0;
    for (uint32_t k = 0; k < N; ++k) {
        if (g0[k] <= 0.f) continue;
        if (s->ce[k] < ce_lo) ce_lo = s->ce[k];
        if (s->ce[k] > ce_hi) ce_hi = s->ce[k];
    }
    if (ce_lo > ce_hi) { for (uint32_t k = 0; k < N; ++k) out[k] = g0[k]; return; }
    if (cs_eff < ce_lo) cs_eff = ce_lo; else if (cs_eff > ce_hi) cs_eff = ce_hi;

    /* a_k = (u_k.e) - target: the ITD error speaker k contributes per unit gain, so the constraint
     * a'g == 0 IS rV.e == target. Minimizing the g0-WEIGHTED ||g - g0||^2 under it has the closed
     * form g = g0 - lam*W*a with W = diag(g0), which collapses to the multiplicative tilt
     *
     *     g_k = g0_k * (1 - lam*a_k),   lam = (a'g0) / (sum_k a_k^2 g0_k)
     *
     * and that weighting is the point: a speaker the seed left silent stays EXACTLY silent, so CAP
     * never recruits a distant speaker to buy an ITD (which would hold the constraint while
     * wrecking the LF/HF agreement the dual-band split rests on) and VBAP stays sparse. */
    /* Active-set iteration: solve, clamp g >= 0, re-solve on the survivors. Each clamping pass
     * strictly shrinks the active set, so N passes is a hard bound and the loop always terminates;
     * in practice a point source converges in 2-4. The clamping is not a numerical wart but the
     * physics: an exact strong ITD is incompatible with radiating from the far side, so a lateral
     * source really does have to shut its contralateral speakers off in the low band. That is CAP
     * sharpening the bass toward the source, and it is the audible difference from dual-band. */
    for (uint32_t k = 0; k < N; ++k) out[k] = g0[k];
    for (uint32_t pass = 0; pass <= N; ++pass) {
        double num = 0.0, den = 0.0;
        for (uint32_t k = 0; k < N; ++k) {
            if (out[k] <= 0.f) continue;                  /* clamped or never active: out of the solve */
            const double a = (double)s->ce[k] - cs_eff;
            num += a * out[k];
            den += a * a * out[k];
        }
        if (den < 1e-12 || num == 0.0) break;             /* already satisfied, or no lever to pull */
        const double lam = num / den;
        int clamped = 0;
        for (uint32_t k = 0; k < N; ++k) {
            if (out[k] <= 0.f) { out[k] = 0.f; continue; }
            const double a = (double)s->ce[k] - cs_eff;
            double g = (double)out[k] * (1.0 - lam * a);
            if (g <= 0.0) { g = 0.0; clamped = 1; }
            out[k] = (float)g;
        }
        if (!clamped) break;                              /* exact in one pass unless the clamp bit */
    }

    /* Same level convention as the plain dual-band low band (rt.c, "dual-band low band"): the LF
     * amplitude sum matches the HF power magnitude ||g0||_2, so distance attenuation survives into
     * the bass. A UNIFORM scale cannot disturb the constraint — rV is a ratio. */
    double gs = 0.0;
    for (uint32_t k = 0; k < N; ++k) gs += out[k];
    if (gs > 1e-9) {
        const float sc = (float)(sqrt(gp) / gs);
        for (uint32_t k = 0; k < N; ++k) out[k] *= sc;
    } else {
        /* Pathological: the clamp cascade zeroed everything. Fall back to the PLAIN dual-band low
         * band, not to raw g0 — g0 is POWER-normalized, so handing it back would make the LF sum
         * `sum g0` instead of `||g0||_2` and put this one voice's bass up to sqrt(active) hot,
         * contradicting the level convention two lines above. `sg` and `gp` are already g0's. */
        const float sc = (float)(sqrt(gp) / sg);
        for (uint32_t k = 0; k < N; ++k) out[k] = g0[k] * sc;
    }
}
