/* dirstage.c: see dirstage.h. */
#include "spatial/dirstage.h"
#include "dsp/biquad.h"
#include "dsp/vec.h"          /* vec_ramp_scale, vec_biquad8 */

#include <math.h>
#include <string.h>

int dirstage_init(DirStage* s, uint32_t channels, float split_hz, uint32_t sample_rate, float slew_db_s) {
    if (!s) return 0;
    memset(s, 0, sizeof *s);
    if (channels == 0 || channels > BWA_CHANNELS || sample_rate == 0) return 0;
    double fc = (double)split_hz;
    if (fc > 0.45 * (double)sample_rate) fc = 0.45 * (double)sample_rate;
    if (!(fc > 0.0)) return 0;
    const double w0 = 2.0 * M_PI * fc / (double)sample_rate;
    s->channels = channels;
    s->have  = 1;
    s->cw0   = (float)cos(w0);
    s->alpha = (float)(sin(w0) / (2.0 * 0.70710678));
    s->slew  = slew_db_s / (float)sample_rate;
    for (uint32_t k = 0; k < channels; ++k) s->sco[k] = -1e9f;   /* no coefficients built yet */
    return 1;
}

void dirstage_targets(DirStage* s, const float* gain_db, const float* shelf_db,
                      float gain_max_db, float shelf_max_db) {
    if (!s || !s->have) return;
    int live = 0;
    for (uint32_t k = 0; k < s->channels; ++k) {
        float g  = gain_db  ? gain_db[k]  : 0.f;
        float sh = shelf_db ? shelf_db[k] : 0.f;
        /* written so a NaN lands on 0 (every NaN compare is false) */
        if (g != g)   g  = 0.f;
        if (sh != sh) sh = 0.f;
        if (g  < -gain_max_db)  g  = -gain_max_db;
        if (g  >  gain_max_db)  g  =  gain_max_db;
        if (sh < -shelf_max_db) sh = -shelf_max_db;
        if (sh >  shelf_max_db) sh =  shelf_max_db;
        s->gtgt[k] = g;
        s->stgt[k] = sh;
        if (g != 0.f || sh != 0.f) live = 1;
    }
    if (live) s->live = 1;   /* identity is reached by slewing back (dirstage_process clears it) */
}

void dirstage_state(const DirStage* s, float* gain_db, float* shelf_db) {
    if (!s) return;
    for (uint32_t k = 0; k < s->channels; ++k) {
        if (gain_db)  gain_db[k]  = s->gcur[k];
        if (shelf_db) shelf_db[k] = s->scur[k];
    }
}

void dirstage_process(DirStage* s, float* bus, uint32_t n) {
    if (!s || !s->live || n == 0) return;
    const float step = s->slew * (float)n;
    int settled = 1;
    uint32_t shk[BWA_CHANNELS], nsh = 0;           /* the channels whose shelf runs this block */
    for (uint32_t k = 0; k < s->channels; ++k) {
        float g0 = s->gcur[k], g1 = g0;
        const float gt = s->gtgt[k];
        if      (g1 < gt - step) g1 += step;
        else if (g1 > gt + step) g1 -= step;
        else                     g1  = gt;
        float s0 = s->scur[k], s1 = s0;
        const float st = s->stgt[k];
        if      (s1 < st - step) s1 += step;
        else if (s1 > st + step) s1 -= step;
        else                     s1  = st;
        if (g0 != 0.f || g1 != 0.f) {              /* broadband: linear ramp across the block */
            const float l0 = powf(10.f, g0 / 20.f), l1 = powf(10.f, g1 / 20.f);
            vec_ramp_scale(&bus[(size_t)k * n], n, l0, (l1 - l0) / (float)n);
        }
        if (s0 != 0.f || s1 != 0.f) {              /* shelf: rebuild when its gain moved; run below */
            if (s1 != s->sco[k]) {
                bwa_biquad_rbj(BWA_BIQUAD_HIGHSHELF, (double)s->cw0, (double)s->alpha,
                               pow(10.0, (double)s1 / 40.0), s->co[k]);
                s->sco[k] = s1;
            }
            shk[nsh++] = k;
        } else {
            s->x1[k] = s->x2[k] = s->y1[k] = s->y2[k] = 0.f;   /* flat: idle state */
        }
        s->gcur[k] = g1;
        s->scur[k] = s1;
        if (g1 != 0.f || s1 != 0.f || gt != 0.f || st != 0.f) settled = 0;
    }
    /* The shelves, eight channels per pass: one channel's biquad is latency-bound, eight interleaved
     * are eight independent chains (vec_biquad8). A short last group pads with identity lanes. Each
     * channel's shelf still runs after its own gain ramp, as it always did. */
    for (uint32_t q0 = 0; q0 < nsh; q0 += 8) {
        const uint32_t nl = (nsh - q0 < 8) ? nsh - q0 : 8;
        float c[5][8], sv[4][8];
        for (uint32_t l = 0; l < 8; ++l) {
            if (l < nl) {
                const uint32_t k = shk[q0 + l];
                for (int t = 0; t < 5; ++t) c[t][l] = s->co[k][t];
                sv[0][l] = s->x1[k]; sv[1][l] = s->x2[k]; sv[2][l] = s->y1[k]; sv[3][l] = s->y2[k];
            } else {
                c[0][l] = 1.f; c[1][l] = c[2][l] = c[3][l] = c[4][l] = 0.f;
                sv[0][l] = sv[1][l] = sv[2][l] = sv[3][l] = 0.f;
            }
        }
        for (uint32_t c0 = 0; c0 < n; c0 += DIRSTAGE_CHUNK) {
            const uint32_t cn = (n - c0 < DIRSTAGE_CHUNK) ? n - c0 : DIRSTAGE_CHUNK;
            float* v = s->lanes;
            for (uint32_t l = 0; l < 8; ++l) {
                if (l < nl) { const float* x = &bus[(size_t)shk[q0 + l] * n + c0]; for (uint32_t i = 0; i < cn; ++i) v[8*i + l] = x[i]; }
                else        { for (uint32_t i = 0; i < cn; ++i) v[8*i + l] = 0.f; }
            }
            vec_biquad8(v, cn, (const float (*)[8])c, sv);
            for (uint32_t l = 0; l < nl; ++l) { float* x = &bus[(size_t)shk[q0 + l] * n + c0]; for (uint32_t i = 0; i < cn; ++i) x[i] = v[8*i + l]; }
        }
        for (uint32_t l = 0; l < nl; ++l) {
            const uint32_t k = shk[q0 + l];
            s->x1[k] = sv[0][l]; s->x2[k] = sv[1][l]; s->y1[k] = sv[2][l]; s->y2[k] = sv[3][l];
        }
    }
    if (settled) s->live = 0;                      /* landed on identity: resume the fast path */
}
