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
    /* per-speaker correction FIR (optional). Applied to each channel BEFORE the gain+delay. */
    int      any_eq;               /* any channel has a correction filter */
    uint16_t eq_len[BW_CHANNELS];  /* taps per channel (0 = bypass) */
    float*   eq;                   /* channels * BW_EQ_TAPS, the kernels */
    uint32_t eqlen, eqmask, eqw;   /* per-channel input-history ring (power of two >= BW_EQ_TAPS) */
    float*   eqhist;               /* channels * eqlen */
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
        a->gain[k]   = L->speakers[k].gain_lin;
        a->delay[k]  = L->speakers[k].delay_samples;
        a->eq_len[k] = L->speakers[k].eq_len > BW_EQ_TAPS ? BW_EQ_TAPS : L->speakers[k].eq_len;
        if (a->eq_len[k]) a->any_eq = 1;
    }
    a->buf = (float*)calloc((size_t)channels * a->len, sizeof(float));
    if (!a->buf) { free(a); return NULL; }
    if (a->any_eq) {                               /* only pay the memory + DSP if a filter exists */
        a->eqlen  = pow2_ge(BW_EQ_TAPS);
        a->eqmask = a->eqlen - 1;
        a->eq     = (float*)calloc((size_t)channels * BW_EQ_TAPS, sizeof(float));
        a->eqhist = (float*)calloc((size_t)channels * a->eqlen, sizeof(float));
        if (!a->eq || !a->eqhist) { free(a->buf); free(a->eq); free(a->eqhist); free(a); return NULL; }
        for (uint32_t k = 0; k < channels; ++k)
            for (uint32_t t = 0; t < a->eq_len[k]; ++t)
                a->eq[(size_t)k * BW_EQ_TAPS + t] = L->speakers[k].eq[t];
    }
    return a;
}

void align_destroy(Aligner* a) {
    if (a) { free(a->buf); free(a->eq); free(a->eqhist); free(a); }
}

void align_process(Aligner* a, float* bus, uint32_t n) {
    if (a->any_eq) {                               /* per-speaker correction FIR, in place on the bus */
        const uint32_t elen = a->eqlen, emask = a->eqmask, C = a->channels, ew = a->eqw;
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t wi = (ew + i) & emask;
            for (uint32_t k = 0; k < C; ++k) a->eqhist[(size_t)k * elen + wi] = bus[(size_t)k * n + i];
            for (uint32_t k = 0; k < C; ++k) {
                const uint16_t T = a->eq_len[k];
                if (!T) continue;
                const float* kn = &a->eq[(size_t)k * BW_EQ_TAPS];
                const float* hh = &a->eqhist[(size_t)k * elen];
                float y = 0.f;
                for (uint16_t t = 0; t < T; ++t) y += kn[t] * hh[(wi - t) & emask];
                bus[(size_t)k * n + i] = y;
            }
        }
        a->eqw = (ew + n) & emask;
    }
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
