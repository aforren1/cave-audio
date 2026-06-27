/*
 * binaural.c — head-oriented 26->stereo monitor. Per channel we find the speaker's bearing
 * relative to the listener, project it onto the head's "right" axis to get a lateral
 * component in [-1,1], and constant-power pan to L/R. Rotating the head rotates the right
 * axis, so the stereo image turns with the head. (W/X ambisonic encode + two cardioids.)
 */
#include "binaural.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

struct Monitor {
    uint32_t channels;
    uint32_t sample_rate;
    float    spk[BW_CHANNELS][3];           /* speaker world positions */
    float    gL[BW_CHANNELS], gR[BW_CHANNELS];
    float    last_p[3], last_q[4];
    int      have;
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

/* world-space direction of the head's local +x ("right"), from quaternion q (xyzw). The
 * rotation formula assumes a unit quaternion, so normalize first; a zero/degenerate q
 * (e.g. an unset pose) falls back to identity (facing forward). */
static void head_right(const float q[4], float r[3]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float n2 = x * x + y * y + z * z + w * w;
    if (n2 > 1e-12f) { float inv = 1.f / sqrtf(n2); x *= inv; y *= inv; z *= inv; w *= inv; }
    else { x = y = z = 0.f; w = 1.f; }
    r[0] = 1.f - 2.f * (y * y + z * z);
    r[1] = 2.f * (x * y + z * w);
    r[2] = 2.f * (x * z - y * w);
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
    float* L = out;
    float* R = out + (size_t)n;                     /* planar 2-ch out */
    memset(out, 0, sizeof(float) * (size_t)n * 2);
    for (uint32_t k = 0; k < m->channels; ++k) {
        const float* src = bus + (size_t)k * n;
        const float gl = m->gL[k], gr = m->gR[k];
        for (uint32_t i = 0; i < n; ++i) { L[i] += gl * src[i]; R[i] += gr * src[i]; }
    }
    /* Debug monitor: summing 26 virtual speakers into 2 channels is not level-calibrated
     * (the production Steam Audio decode normalizes). Clamp to keep the device in range. */
    for (uint32_t i = 0; i < n; ++i) {
        if      (L[i] >  1.f) L[i] =  1.f; else if (L[i] < -1.f) L[i] = -1.f;
        if      (R[i] >  1.f) R[i] =  1.f; else if (R[i] < -1.f) R[i] = -1.f;
    }
}
