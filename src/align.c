/*
 * align.c — per-channel gain trim + integer-sample delay line. One power-of-two ring per
 * channel sized to max_delay+1, a single shared write index. Trivial DSP, but not optional
 * for a real array (docs/spatialization.md). In-place on the planar 26-ch bus.
 */
#include "align.h"

#include <stdlib.h>
#include <string.h>

struct Aligner {
    uint32_t channels;
    uint32_t len;                  /* ring length, power of two >= max_delay + 1 */
    uint32_t mask;                 /* len - 1 */
    uint32_t w;                    /* write index */
    float    gain[BW_CHANNELS];
    uint32_t delay[BW_CHANNELS];
    float*   buf;                  /* channels * len */
};

static uint32_t pow2_ge(uint32_t x) { uint32_t p = 1; while (p < x) p <<= 1; return p; }

Aligner* align_create(uint32_t channels, const Layout* L) {
    if (channels == 0 || channels > BW_CHANNELS || !L) return NULL;
    Aligner* a = (Aligner*)calloc(1, sizeof *a);
    if (!a) return NULL;
    a->channels = channels;
    a->len  = pow2_ge(L->max_delay_samples + 1);   /* +1 so delay==max still has a slot */
    a->mask = a->len - 1;
    a->w    = 0;
    for (uint32_t k = 0; k < channels; ++k) {
        a->gain[k]  = L->speakers[k].gain_lin;
        a->delay[k] = L->speakers[k].delay_samples;
    }
    a->buf = (float*)calloc((size_t)channels * a->len, sizeof(float));
    if (!a->buf) { free(a); return NULL; }
    return a;
}

void align_destroy(Aligner* a) {
    if (a) { free(a->buf); free(a); }
}

void align_process(Aligner* a, float* bus, uint32_t n) {
    const uint32_t len = a->len, mask = a->mask, C = a->channels;
    const uint32_t w = a->w;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t wi = (w + i) & mask;
        for (uint32_t k = 0; k < C; ++k)               /* write the trimmed current sample */
            a->buf[(size_t)k * len + wi] = bus[(size_t)k * n + i] * a->gain[k];
        for (uint32_t k = 0; k < C; ++k) {             /* read it back delayed (delay < len) */
            const uint32_t ri = (wi - a->delay[k]) & mask;
            bus[(size_t)k * n + i] = a->buf[(size_t)k * len + ri];
        }
    }
    a->w = (w + n) & mask;
}
