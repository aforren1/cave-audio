/*
 * hpeq.h — headphone correction EQ: parse an AutoEq-style parametric EQ file into an RBJ
 * biquad cascade, and apply it to the headphone renders' final stereo with a ramped dry/wet
 * mix (the click-free A/B toggle and load swap). The headphone-side analog of the per-speaker
 * align stage: it corrects the TRANSDUCER, not the render, so engine.c runs it on every
 * headphone profile's device-bound stereo (binaural, cave_sim, and cave_both's monitor tap).
 *
 * The format is AutoEq's ParametricEQ.txt (github.com/jaakkopasanen/AutoEq — measured,
 * target-compensated corrections for thousands of headphone models):
 *   Preamp: -6.4 dB
 *   Filter 1: ON PK Fc 105 Hz Gain -4.6 dB Q 0.70
 * with PK (peaking) / LSC (low shelf) / HSC (high shelf) — exactly biquad.h's RBJ types.
 * The Preamp line is load-bearing, not garnish: corrections BOOST dips, and without the
 * headroom the output clamp eats transients.
 */
#ifndef BWA_HPEQ_H
#define BWA_HPEQ_H

#include <stddef.h>
#include <stdint.h>

#define BWA_HPEQ_MAX_SEC 24     /* AutoEq emits ~10 filters; a file beyond this is rejected */

typedef struct {
    float sec[BWA_HPEQ_MAX_SEC][5];   /* RBJ biquads, biquad.h layout: b0 b1 b2 a1 a2 */
    int   nsec;
    float preamp;                     /* linear pre-gain (the Preamp line; 1 = none) */
} HpEqDesign;

/* Audio-thread runtime: DF-I histories per section per channel + the applied dry/wet mix. */
typedef struct {
    float x1[2][BWA_HPEQ_MAX_SEC], x2[2][BWA_HPEQ_MAX_SEC];
    float y1[2][BWA_HPEQ_MAX_SEC], y2[2][BWA_HPEQ_MAX_SEC];
    float mix;                        /* ramped toward the block target by hpeq_apply */
} HpEqState;

/* Parse `path` at `sample_rate` into `design`. Lenient where AutoEq variants differ (extra
 * whitespace, LS/HS for LSC/HSC, a missing Q defaults to 0.707, OFF filters and unknown lines
 * are skipped) and loud where it matters (unreadable file, a malformed Filter line, cascade
 * overflow, or a file yielding NO filters all fail: 0 + err). A filter at/above Nyquist is
 * skipped, not fatal (AutoEq tops out ~20 kHz; only sub-48k rates could hit it).
 * Control thread — file I/O. */
int  hpeq_parse(const char* path, uint32_t sample_rate, HpEqDesign* design,
                char* err, size_t errcap);

void hpeq_state_reset(HpEqState* st);

/* Apply the cascade to planar stereo (L at s[0..n), R at s[n..2n)), the dry/wet mix ramped
 * linearly from st->mix to `mix_tgt` across the block (st->mix lands exactly — invariant 4).
 * mix 0 = bypass, 1 = corrected (preamp included). Audio thread; no alloc/lock. */
void hpeq_apply(const HpEqDesign* d, HpEqState* st, float* stereo, uint32_t n, float mix_tgt);

#endif /* BWA_HPEQ_H */
