/* dirstage.c: see dirstage.h. Moved out of align.c unchanged, so align's arithmetic is the same
 * sequence of float operations it always was. */
#include "spatial/dirstage.h"
#include "dsp/biquad.h"

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
        float* x = &bus[(size_t)k * n];
        if (g0 != 0.f || g1 != 0.f) {              /* broadband: linear ramp across the block */
            const float l0 = powf(10.f, g0 / 20.f), l1 = powf(10.f, g1 / 20.f);
            const float dl = (l1 - l0) / (float)n;
            float l = l0;
            for (uint32_t i = 0; i < n; ++i) { x[i] *= l; l += dl; }
        }
        if (s0 != 0.f || s1 != 0.f) {              /* shelf: rebuild when its gain moved, then DF-I */
            if (s1 != s->sco[k]) {
                bwa_biquad_rbj(BWA_BIQUAD_HIGHSHELF, (double)s->cw0, (double)s->alpha,
                               pow(10.0, (double)s1 / 40.0), s->co[k]);
                s->sco[k] = s1;
            }
            const float* co = s->co[k];
            float x1 = s->x1[k], x2 = s->x2[k], y1 = s->y1[k], y2 = s->y2[k];
            for (uint32_t i = 0; i < n; ++i) {
                float in = x[i];
                float y  = co[0]*in + co[1]*x1 + co[2]*x2 - co[3]*y1 - co[4]*y2;
                x2 = x1; x1 = in; y2 = y1; y1 = y; x[i] = y;
            }
            s->x1[k] = x1; s->x2[k] = x2; s->y1[k] = y1; s->y2[k] = y2;
        } else {
            s->x1[k] = s->x2[k] = s->y1[k] = s->y2[k] = 0.f;   /* flat: idle state */
        }
        s->gcur[k] = g1;
        s->scur[k] = s1;
        if (g1 != 0.f || s1 != 0.f || gt != 0.f || st != 0.f) settled = 0;
    }
    if (settled) s->live = 0;                      /* landed on identity: resume the fast path */
}
