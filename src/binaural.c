/*
 * binaural.c — head-oriented 26->stereo monitor. Per channel we find the speaker's bearing
 * relative to the listener, project it onto the head's "right" axis to get a lateral
 * component in [-1,1], and constant-power pan to L/R. Rotating the head rotates the right
 * axis, so the stereo image turns with the head. (W/X ambisonic encode + two cardioids.)
 */
#include "binaural.h"
#include "frame.h"          /* BW_ROOM_RIGHT + frame_qrot */

#include <math.h>
#include <stdlib.h>
#include <string.h>

struct Monitor {
    uint32_t channels;
    uint32_t sample_rate;
    float    spk[BW_CHANNELS][3];           /* speaker world positions */
    float    gL[BW_CHANNELS], gR[BW_CHANNELS];          /* target pan gains (recomputed on pose change) */
    float    gL_cur[BW_CHANNELS], gR_cur[BW_CHANNELS];  /* applied gains, ramped toward the target (invariant 4) */
    float    last_p[3], last_q[4];
    int      have;
    int      primed;                        /* gL_cur seeded from the first solve (no ramp up from 0) */
};

Monitor* monitor_create(const Layout* L, uint32_t sample_rate) {
    if (!L || L->count == 0 || L->count > BW_CHANNELS) return NULL;
    Monitor* m = (Monitor*)calloc(1, sizeof *m);
    if (!m) return NULL;
    m->channels    = L->count;
    m->sample_rate = sample_rate;
    for (uint32_t k = 0; k < L->count; ++k)
        memcpy(m->spk[k], L->speakers[k].pos, sizeof m->spk[k]);
    return m;
}

void monitor_destroy(Monitor* m) { free(m); }

/* world-space direction of the listener's right ear axis (BW_ROOM_RIGHT rotated by q, xyzw).
 * frame_qrot assumes a unit quaternion, so normalize first; a zero/degenerate q (e.g. an
 * unset pose) falls back to identity (facing BW_ROOM_AHEAD). */
static void head_right(const float q[4], float r[3]) {
    float n[4] = { q[0], q[1], q[2], q[3] };
    float n2 = n[0] * n[0] + n[1] * n[1] + n[2] * n[2] + n[3] * n[3];
    if (n2 > 1e-12f) { float inv = 1.f / sqrtf(n2); n[0] *= inv; n[1] *= inv; n[2] *= inv; n[3] *= inv; }
    else { n[0] = n[1] = n[2] = 0.f; n[3] = 1.f; }
    frame_qrot(n, BW_ROOM_RIGHT, r);
}

static void recompute(Monitor* m, const float p[3], const float q[4]) {
    float hr[3];
    head_right(q, hr);
    for (uint32_t k = 0; k < m->channels; ++k) {
        float dx = m->spk[k][0] - p[0], dy = m->spk[k][1] - p[1], dz = m->spk[k][2] - p[2];
        float len = sqrtf(dx * dx + dy * dy + dz * dz);
        float lateral = (len > 1e-6f) ? (dx * hr[0] + dy * hr[1] + dz * hr[2]) / len : 0.f;
        if (lateral < -1.f) lateral = -1.f; else if (lateral > 1.f) lateral = 1.f;
        m->gL[k] = sqrtf(0.5f * (1.f - lateral));   /* constant power: gL^2 + gR^2 = 1 */
        m->gR[k] = sqrtf(0.5f * (1.f + lateral));
    }
    memcpy(m->last_p, p, sizeof m->last_p);
    memcpy(m->last_q, q, sizeof m->last_q);
    m->have = 1;
}

void monitor_process(Monitor* m, const float* bus, const float p[3], const float q[4],
                     float* out, uint32_t n) {
    if (!m->have || memcmp(m->last_p, p, sizeof m->last_p) || memcmp(m->last_q, q, sizeof m->last_q))
        recompute(m, p, q);
    if (!m->primed) {                               /* seed the applied gains from the first solve (no ramp from 0) */
        memcpy(m->gL_cur, m->gL, sizeof m->gL_cur);
        memcpy(m->gR_cur, m->gR, sizeof m->gR_cur);
        m->primed = 1;
    }
    float* L = out;
    float* R = out + (size_t)n;                     /* planar 2-ch out */
    memset(out, 0, sizeof(float) * (size_t)n * 2);
    const float inv_n = 1.0f / (float)n;
    for (uint32_t k = 0; k < m->channels; ++k) {
        const float* src = bus + (size_t)k * n;
        float gl = m->gL_cur[k], gr = m->gR_cur[k];              /* ramp per block toward the new pan gains: a head */
        const float dgl = (m->gL[k] - gl) * inv_n;              /* turn moves the stereo image smoothly, no zipper */
        const float dgr = (m->gR[k] - gr) * inv_n;
        for (uint32_t i = 0; i < n; ++i) { L[i] += gl * src[i]; R[i] += gr * src[i]; gl += dgl; gr += dgr; }
        m->gL_cur[k] = m->gL[k]; m->gR_cur[k] = m->gR[k];        /* land exactly */
    }
    /* Debug monitor: summing 26 virtual speakers into 2 channels is not level-calibrated
     * (the production Steam Audio decode normalizes). Clamp to keep the device in range. */
    for (uint32_t i = 0; i < n; ++i) {
        if      (L[i] >  1.f) L[i] =  1.f; else if (L[i] < -1.f) L[i] = -1.f;
        if      (R[i] >  1.f) R[i] =  1.f; else if (R[i] < -1.f) R[i] = -1.f;
    }
}
