/*
 * align.c — per-channel gain trim + integer-sample delay line. One power-of-two ring per
 * channel sized to max_delay+1, a single shared write index. Trivial DSP, but not optional
 * for a real array (docs/spatialization.md). In-place on the planar 26-ch bus.
 * Optionally also applies the per-speaker correction FIR and the LF room_eq modal cuts
 * (static-listener room correction; docs/calibration.md) before the gain+delay.
 */
#include "align.h"
#include "biquad.h"       /* shared RBJ cookbook (also used by rt.c's transmission/pathing EQ) */

#include <math.h>
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
    /* LF room_eq modal cuts (optional): per-channel RBJ peaking biquads, coefficients derived from
     * the layout's fc/gain/Q at the engine rate. Static state — fixed arrays, no allocation. */
    int      any_rq;
    uint8_t  rq_n[BW_CHANNELS];
    float    rq_co[BW_CHANNELS][BW_ROOM_EQ_MAX][5];   /* b0 b1 b2 a1 a2 (a0-normalized) */
    float    rq_x1[BW_CHANNELS][BW_ROOM_EQ_MAX], rq_x2[BW_CHANNELS][BW_ROOM_EQ_MAX];
    float    rq_y1[BW_CHANNELS][BW_ROOM_EQ_MAX], rq_y2[BW_CHANNELS][BW_ROOM_EQ_MAX];
};

static uint32_t pow2_ge(uint32_t x) { uint32_t p = 1; while (p < x) p <<= 1; return p; }

Aligner* align_create(uint32_t channels, const Layout* L, uint32_t sample_rate) {
    if (channels == 0 || channels > BW_CHANNELS || !L || sample_rate == 0) return NULL;
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
        uint8_t nrq = L->speakers[k].room_eq_count > BW_ROOM_EQ_MAX ? BW_ROOM_EQ_MAX : L->speakers[k].room_eq_count;
        for (uint8_t s = 0; s < nrq; ++s) {        /* RBJ peaking EQ from the rate-independent params */
            const RoomEqSection* rq = &L->speakers[k].room_eq[s];
            if (!(rq->fc > 0.f) || rq->fc >= 0.5f * (float)sample_rate || !(rq->q > 0.f)) continue;
            uint8_t j = a->rq_n[k];
            bw_biquad_rbj_hz(BW_BIQUAD_PEAK, rq->fc, rq->q, rq->gain_db, (double)sample_rate, a->rq_co[k][j]);
            a->rq_n[k] = j + 1;
        }
        if (a->rq_n[k]) a->any_rq = 1;
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
    if (a->any_rq) {                               /* LF modal cuts: per-channel biquad cascade (DF-I) */
        for (uint32_t k = 0; k < a->channels; ++k) {
            const uint8_t S = a->rq_n[k];
            if (!S) continue;
            float* x = &bus[(size_t)k * n];
            for (uint8_t s = 0; s < S; ++s) {
                const float* co = a->rq_co[k][s];
                float x1 = a->rq_x1[k][s], x2 = a->rq_x2[k][s], y1 = a->rq_y1[k][s], y2 = a->rq_y2[k][s];
                for (uint32_t i = 0; i < n; ++i) {
                    float in = x[i];
                    float y  = co[0]*in + co[1]*x1 + co[2]*x2 - co[3]*y1 - co[4]*y2;
                    x2 = x1; x1 = in; y2 = y1; y1 = y; x[i] = y;
                }
                a->rq_x1[k][s] = x1; a->rq_x2[k][s] = x2; a->rq_y1[k][s] = y1; a->rq_y2[k][s] = y2;
            }
        }
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
