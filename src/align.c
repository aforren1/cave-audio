/*
 * align.c — per-channel gain trim + integer-sample delay line. One power-of-two ring per
 * channel, a single shared write index. Trivial DSP, but not optional for a real array
 * (docs/spatialization.md). In-place on the planar 26-ch bus.
 * Optionally also applies the per-speaker correction FIR and the LF room_eq modal cuts
 * (static-listener room correction; docs/calibration.md) before the gain+delay, and the
 * opt-in tracked-listener alignment (align_tracked_targets), which adds a slewed FRACTIONAL
 * delay + gain per channel so the array's time coherence follows the tracked head instead of
 * the layout's fixed reference. The ring carries headroom for that whether or not it is used.
 */
#include "align.h"
#include "biquad.h"       /* shared RBJ cookbook (also used by rt.c's transmission/pathing EQ) */
#include "bits.h"         /* bwa_pow2_ge */

#include <math.h>
#include <stdlib.h>
#include <string.h>

struct Aligner {
    uint32_t channels;
    uint32_t len;                  /* ring length, power of two >= max_delay + lc_max + 2 */
    uint32_t mask;                 /* len - 1 */
    uint32_t w;                    /* write index */
    float    gain[BWA_CHANNELS];
    uint32_t delay[BWA_CHANNELS];
    float*   buf;                  /* channels * len */
    /* per-speaker correction FIR (optional). Applied to each channel BEFORE the gain+delay. */
    int      any_eq;               /* any channel has a correction filter */
    uint16_t eq_len[BWA_CHANNELS];  /* taps per channel (0 = bypass) */
    float*   eq;                   /* channels * BWA_EQ_TAPS, the kernels */
    uint32_t eqlen, eqmask, eqw;   /* per-channel input-history ring (power of two >= BWA_EQ_TAPS) */
    float*   eqhist;               /* channels * eqlen */
    /* LF room_eq modal cuts (optional): per-channel RBJ peaking biquads, coefficients derived from
     * the layout's fc/gain/Q at the engine rate. Static state — fixed arrays, no allocation. */
    int      any_rq;
    uint8_t  rq_n[BWA_CHANNELS];
    float    rq_co[BWA_CHANNELS][BWA_ROOM_EQ_MAX][5];   /* b0 b1 b2 a1 a2 (a0-normalized) */
    float    rq_x1[BWA_CHANNELS][BWA_ROOM_EQ_MAX], rq_x2[BWA_CHANNELS][BWA_ROOM_EQ_MAX];
    float    rq_y1[BWA_CHANNELS][BWA_ROOM_EQ_MAX], rq_y2[BWA_CHANNELS][BWA_ROOM_EQ_MAX];
    /* Tracked room EQ (layouts with a room_eq_grid): the SAME rq cascade, but the section gains are
     * live — rt.c interpolates targets from the grid at the listener position (align_room_eq_targets)
     * and process slews each section's gain toward its target (RQ_SLEW_DB_S), rebuilding that section's
     * coefficients from the precomputed cw0/alpha (fixed fc/Q ladder — only the depth moves, so the
     * trig stays out of the audio thread; same trick as rt.c's transmission EQ). Sections settled at
     * 0 dB are skipped (a cold restart of a near-identity biquad is inaudible). */
    int      rq_dyn;
    float    rq_cw0[BWA_CHANNELS][BWA_ROOM_EQ_MAX];     /* cos(w0) per ladder section */
    float    rq_alpha[BWA_CHANNELS][BWA_ROOM_EQ_MAX];   /* sin(w0)/(2Q) per ladder section */
    float    rq_gcur[BWA_CHANNELS][BWA_ROOM_EQ_MAX];    /* current section gain (dB, slewed) */
    float    rq_gtgt[BWA_CHANNELS][BWA_ROOM_EQ_MAX];    /* target section gain (dB) */
    float    rq_slew;                                 /* max dB change per sample (RQ_SLEW_DB_S / rate) */
    /* Tracked listener alignment (align_tracked_targets; off by default). An EXTRA per-channel delay
     * and gain on top of the layout's fixed trims, re-referencing the array's time alignment onto the
     * tracked listener. lc_live latches on the moment any channel leaves identity and clears only when
     * every channel has landed back EXACTLY on identity — while it is 0 the delay read stays the
     * original integer tap, so the default path is bit-identical. */
    int      lc_live;
    uint32_t lc_max;                                  /* comp delay ceiling (frames) = the ring headroom */
    double   lc_rate;                                 /* max delay change per frame (slew / rate) */
    float    lc_grate;                                /* max gain change per frame (LC_GAIN_SLEW_S / rate) */
    float    inv_rate;                                /* 1 / sample_rate */
    /* the accumulating tap position is DOUBLE on purpose: it is a small increment added to a value
     * that can reach ~1100 frames, and in float the increment sinks toward the ulp there — a slow
     * slew would quantize, and at the extreme stall outright */
    double   lc_dcur[BWA_CHANNELS];
    float    lc_dtgt[BWA_CHANNELS];                   /* extra delay, frames (>= 0) */
    float    lc_gcur[BWA_CHANNELS], lc_gtgt[BWA_CHANNELS];   /* extra gain, linear */
};

#define RQ_SLEW_DB_S 24.0f   /* tracked-room-EQ gain slew: a full 12 dB cut lands in ~0.5 s — fast
                              * enough to track a walking listener, far too slow to zipper (the cuts
                              * live below 200 Hz, where a 0.13 dB/block step at 256/48k is inaudible) */

/* Tracked-listener-alignment sizing. LC_RADIUS_M is the listener excursion from Layout.ref the comp
 * delay line reserves headroom for; the worst case is a listener who moves that far TOWARD one speaker
 * and that far AWAY from another, hence the 2x. LC_SOS is a SIZING constant only (the live
 * rt_set_speed_of_sound scales the targets rt.c computes) — a much slower sound speed just clamps at
 * the ceiling. 4 m at 343 m/s and 48 kHz reserves 1120 frames per channel, which rounds the ring up
 * one or two powers of two: ~200 KB for 26 channels, paid once whether or not the feature is used. */
#define LC_RADIUS_M     4.0f
#define LC_SOS          343.0f
/* The 1/r level re-reference is clamped to +/-6 dB. A listener standing on top of a speaker asks for
 * an unbounded cut on it and an unbounded BOOST on the far side (which the limiter would then eat);
 * saturate instead. */
#define LC_GAIN_MIN     0.5f
#define LC_GAIN_MAX     2.0f
#define LC_GAIN_SLEW_S  4.0f    /* max linear-gain change per second: the full 0.5 -> 2.0 span in ~0.4 s.
                                 * Only the dead-zone crossings move it, and one 5 cm crossing is worth
                                 * well under 0.5 dB, so this only ever bounds a teleport. */

Aligner* align_create(uint32_t channels, const Layout* L, uint32_t sample_rate) {
    if (channels == 0 || channels > BWA_CHANNELS || !L || sample_rate == 0) return NULL;
    Aligner* a = (Aligner*)calloc(1, sizeof *a);
    if (!a) return NULL;
    a->channels = channels;
    a->inv_rate = 1.f / (float)sample_rate;
    /* Tracked-listener headroom, reserved unconditionally: enlarging a power-of-two delay ring cannot
     * change the output (every read is (w - delay) & mask with delay < len), so the off path stays
     * bit-identical and the toggle can stay live. +2 keeps both linear-interpolation taps in-ring. */
    a->lc_max = (uint32_t)(2.f * LC_RADIUS_M / LC_SOS * (float)sample_rate + 0.5f);
    a->len  = bwa_pow2_ge(L->max_delay_samples + a->lc_max + 2);   /* +2: delay==max plus the second
                                                                    * interpolation tap still have slots */
    a->mask = a->len - 1;
    a->w    = 0;
    a->lc_rate  = 1.0;                                 /* until rt.c sets one; never used while !lc_live */
    a->lc_grate = LC_GAIN_SLEW_S * a->inv_rate;
    for (uint32_t k = 0; k < channels; ++k) { a->lc_gcur[k] = 1.f; a->lc_gtgt[k] = 1.f; }
    for (uint32_t k = 0; k < channels; ++k) {
        a->gain[k]   = L->speakers[k].gain_lin;
        a->delay[k]  = L->speakers[k].delay_samples;
        a->eq_len[k] = L->speakers[k].eq_len > BWA_EQ_TAPS ? BWA_EQ_TAPS : L->speakers[k].eq_len;
        if (a->eq_len[k]) a->any_eq = 1;
        uint8_t nrq = L->speakers[k].room_eq_count > BWA_ROOM_EQ_MAX ? BWA_ROOM_EQ_MAX : L->speakers[k].room_eq_count;
        for (uint8_t s = 0; s < nrq; ++s) {        /* RBJ peaking EQ from the rate-independent params */
            const RoomEqSection* rq = &L->speakers[k].room_eq[s];
            if (!(rq->fc > 0.f) || rq->fc >= 0.5f * (float)sample_rate || !(rq->q > 0.f)) continue;
            uint8_t j = a->rq_n[k];
            bwa_biquad_rbj_hz(BWA_BIQUAD_PEAK, rq->fc, rq->q, rq->gain_db, (double)sample_rate, a->rq_co[k][j]);
            a->rq_n[k] = j + 1;
        }
        if (a->rq_n[k]) a->any_rq = 1;
    }
    if (L->rq_grid.npos) {                         /* tracked room EQ: seed the ladder, gains start flat
                                                    * (0 dB) and slew toward rt.c's interpolated targets
                                                    * (mutually exclusive with room_eq — layout.c enforces) */
        a->rq_dyn  = 1;
        a->any_rq  = 1;
        a->rq_slew = RQ_SLEW_DB_S / (float)sample_rate;
        for (uint32_t k = 0; k < channels; ++k) {
            uint8_t m = L->rq_grid.nsec[k] > BWA_ROOM_EQ_MAX ? BWA_ROOM_EQ_MAX : L->rq_grid.nsec[k];
            for (uint8_t s = 0; s < m; ++s) {
                double fcv = (double)L->rq_grid.fc[k][s], qv = (double)L->rq_grid.q[k][s];
                /* same guard the static path has at its bwa_biquad_rbj_hz call above (layout.c bounds
                 * these, but a q of 0/NaN here would be Inf/NaN alpha -> NaN coefficients forever).
                 * No compaction — rt.c interpolates depths BY LADDER INDEX — so a bad section
                 * becomes unity (alpha 0) instead of being skipped. */
                if (!(fcv > 0.0) || fcv >= 0.5 * (double)sample_rate || !(qv > 0.0)) {
                    a->rq_cw0[k][s] = 1.f; a->rq_alpha[k][s] = 0.f;
                    continue;
                }
                double w0 = 2.0 * M_PI * fcv / (double)sample_rate;
                a->rq_cw0[k][s]   = (float)cos(w0);
                a->rq_alpha[k][s] = (float)(sin(w0) / (2.0 * qv));
            }
            a->rq_n[k] = m;
        }
    }
    a->buf = (float*)calloc((size_t)channels * a->len, sizeof(float));
    if (!a->buf) { free(a); return NULL; }
    if (a->any_eq) {                               /* only pay the memory + DSP if a filter exists */
        a->eqlen  = bwa_pow2_ge(BWA_EQ_TAPS);
        a->eqmask = a->eqlen - 1;
        a->eq     = (float*)calloc((size_t)channels * BWA_EQ_TAPS, sizeof(float));
        a->eqhist = (float*)calloc((size_t)channels * a->eqlen, sizeof(float));
        if (!a->eq || !a->eqhist) { free(a->buf); free(a->eq); free(a->eqhist); free(a); return NULL; }
        for (uint32_t k = 0; k < channels; ++k)
            for (uint32_t t = 0; t < a->eq_len[k]; ++t)
                a->eq[(size_t)k * BWA_EQ_TAPS + t] = L->speakers[k].eq[t];
    }
    return a;
}

void align_destroy(Aligner* a) {
    if (a) { free(a->buf); free(a->eq); free(a->eqhist); free(a); }
}

void align_room_eq_targets(Aligner* a, const float (*gain_db)[BWA_ROOM_EQ_MAX]) {
    if (!a || !a->rq_dyn || !gain_db) return;
    for (uint32_t k = 0; k < a->channels; ++k)
        for (uint8_t s = 0; s < a->rq_n[k]; ++s) {
            float g = gain_db[k][s];
            if (!(g < 0.f))   g = 0.f;             /* the grid is cut-only by schema; clamp defensively.
                                                    * NaN-cleansing form: a NaN target would pass both
                                                    * plain compares and NaN the biquad coefficients
                                                    * for the rest of the session */
            else if (g < -24.f) g = -24.f;
            a->rq_gtgt[k][s] = g;
        }
}

void align_tracked_targets(Aligner* a, const float* delay_frames, const float* gain_lin) {
    if (!a) return;
    int live = 0;
    for (uint32_t k = 0; k < a->channels; ++k) {
        float d = delay_frames ? delay_frames[k] : 0.f;
        float g = gain_lin     ? gain_lin[k]     : 1.f;
        if (!(d > 0.f)) d = 0.f;                       /* also catches NaN: a poisoned target would
                                                        * otherwise index the ring from a NaN cast */
        if (d > (float)a->lc_max) d = (float)a->lc_max;
        if (!(g > LC_GAIN_MIN)) g = LC_GAIN_MIN;
        if (g > LC_GAIN_MAX)    g = LC_GAIN_MAX;
        a->lc_dtgt[k] = d;
        a->lc_gtgt[k] = g;
        if (d != 0.f || g != 1.f) live = 1;
    }
    if (live) a->lc_live = 1;   /* identity is reached by SLEWING back (align_process clears the flag
                                 * once every channel lands), never by clearing it here — dropping the
                                 * fractional tap mid-displacement would be a step of up to lc_max. */
}

void align_tracked_slew(Aligner* a, float frames_per_s) {
    if (!a || !(frames_per_s > 0.f)) return;
    double r = (double)frames_per_s * (double)a->inv_rate;
    if (r > 0.5) r = 0.5;       /* half a frame of delay per frame is already a 50% resampling ratio;
                                 * anything past it is nonsense and would let the read tap chase the
                                 * write pointer through its own history. */
    a->lc_rate = r;
}

uint32_t align_tracked_max_frames(const Aligner* a) { return a ? a->lc_max : 0; }

void align_tracked_state(const Aligner* a, float* delay_frames, float* gain_lin) {
    if (!a) return;
    for (uint32_t k = 0; k < a->channels; ++k) {
        if (delay_frames) delay_frames[k] = (float)a->lc_dcur[k];
        if (gain_lin)     gain_lin[k]     = a->lc_gcur[k];
    }
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
                const float* kn = &a->eq[(size_t)k * BWA_EQ_TAPS];
                const float* hh = &a->eqhist[(size_t)k * elen];
                float y = 0.f;
                for (uint16_t t = 0; t < T; ++t) y += kn[t] * hh[(wi - t) & emask];
                bus[(size_t)k * n + i] = y;
            }
        }
        a->eqw = (ew + n) & emask;
    }
    if (a->rq_dyn) {                               /* tracked room EQ: slew each section's gain toward its
                                                    * target, rebuilding its coefficients when it moved. The
                                                    * step is bounded per BLOCK (slew * n), so the spectral
                                                    * envelope glides — the biquad state rides through a
                                                    * coefficient nudge without a step (invariant 4). */
        const float step = a->rq_slew * (float)n;
        for (uint32_t k = 0; k < a->channels; ++k)
            for (uint8_t s = 0; s < a->rq_n[k]; ++s) {
                float g = a->rq_gcur[k][s], t = a->rq_gtgt[k][s];
                if (g == t) continue;
                if      (g < t - step) g += step;
                else if (g > t + step) g -= step;
                else                   g  = t;
                a->rq_gcur[k][s] = g;
                bwa_biquad_rbj(BWA_BIQUAD_PEAK, (double)a->rq_cw0[k][s], (double)a->rq_alpha[k][s],
                              pow(10.0, (double)g / 40.0), a->rq_co[k][s]);
            }
    }
    if (a->any_rq) {                               /* LF modal cuts: per-channel biquad cascade (DF-I) */
        for (uint32_t k = 0; k < a->channels; ++k) {
            const uint8_t S = a->rq_n[k];
            if (!S) continue;
            float* x = &bus[(size_t)k * n];
            for (uint8_t s = 0; s < S; ++s) {
                if (a->rq_dyn && a->rq_gcur[k][s] == 0.f && a->rq_gtgt[k][s] == 0.f)
                    continue;                      /* settled flat: identity — skip (cold restart is fine) */
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
    if (!a->lc_live) {                                 /* the default path: integer taps, untouched */
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t wi = (w + i) & mask;
            for (uint32_t k = 0; k < C; ++k)           /* write the trimmed current sample */
                a->buf[(size_t)k * len + wi] = bus[(size_t)k * n + i] * a->gain[k];
            for (uint32_t k = 0; k < C; ++k) {         /* read it back delayed (delay < len) */
                const uint32_t ri = (wi - a->delay[k]) & mask;
                bus[(size_t)k * n + i] = a->buf[(size_t)k * len + ri];
            }
        }
    } else {
        /* Tracked listener alignment engaged: the same ring, read at delay[k] + lc_dcur[k] with a
         * linear-interpolated fractional tap, and the tap CREEPS toward its target per sample. That
         * creep IS the resampling ratio (the same mechanism as the per-voice Doppler line in rt.c), so
         * lc_rate is what bounds the pitch shift; the extra gain rides a per-sample ramp beside it.
         * lc_dcur >= 0 and delay[k] + lc_max + 2 <= len, so both taps stay in the ring. */
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t wi = (w + i) & mask;
            for (uint32_t k = 0; k < C; ++k)
                a->buf[(size_t)k * len + wi] = bus[(size_t)k * n + i] * a->gain[k];
            for (uint32_t k = 0; k < C; ++k) {
                const double   dl = (double)a->delay[k] + a->lc_dcur[k];
                const uint32_t di = (uint32_t)dl;                       /* integer part (dl >= 0) */
                const float    df = (float)(dl - (double)di);           /* fraction in [0, 1) */
                const float* h = &a->buf[(size_t)k * len];
                const float s0 = h[(wi - di)     & mask];               /* di frames ago */
                const float s1 = h[(wi - di - 1) & mask];               /* di+1 frames ago */
                bus[(size_t)k * n + i] = (s0 + (s1 - s0) * df) * a->lc_gcur[k];
                double d = a->lc_dcur[k];                               /* rate-limited glide, per sample */
                const double dt = (double)a->lc_dtgt[k];
                if      (d < dt - a->lc_rate) d += a->lc_rate;
                else if (d > dt + a->lc_rate) d -= a->lc_rate;
                else                          d  = dt;                  /* land exactly */
                a->lc_dcur[k] = d;
                float g = a->lc_gcur[k];
                const float gt = a->lc_gtgt[k];
                if      (g < gt - a->lc_grate) g += a->lc_grate;
                else if (g > gt + a->lc_grate) g -= a->lc_grate;
                else                           g  = gt;
                a->lc_gcur[k] = g;
            }
        }
        int settled = 1;                               /* landed back on identity: resume the fast path */
        for (uint32_t k = 0; k < C; ++k)
            if (a->lc_dcur[k] != 0.0 || a->lc_gcur[k] != 1.f) { settled = 0; break; }
        if (settled) a->lc_live = 0;
    }
    a->w = (w + n) & mask;
}
