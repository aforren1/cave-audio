/*
 * measure.h — acoustic measurement DSP for speaker calibration (pure, no I/O, no audio thread).
 *
 * The method is the textbook exponential sine sweep (ESS, Farina): play a known log sweep out one
 * speaker, capture it at an omnidirectional mic, and recover that speaker's impulse response by
 * regularized deconvolution H(f) = Capture·conj(Ref) / (|Ref|^2 + eps). From the IR we read:
 *   - delay_samples : the IR peak position = system latency + speaker->mic time of flight.
 *   - level         : the in-band average |H(f)| = the speaker's broadband sensitivity at the mic.
 *   - band[3]       : low / mid / high average |H(f)| (a coarse response shape; diagnostic only).
 *
 * The calibration tool (examples/calibrate.c) runs this per speaker, then turns the 26 results into
 * per-speaker delay trims (align all arrivals to the farthest) and gain trims (equalize sensitivity,
 * with the layout's speaker->mic distance divided out) written back into cave_layout.json.
 *
 * This file is deliberately I/O-free and hardware-free so the DSP is unit-tested (test/measure_test.c)
 * against a synthetic capture with a known delay + gain + low-pass — the ASIO capture is the only part
 * that needs the rig.
 */
#ifndef BW_MEASURE_H
#define BW_MEASURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int   delay_samples;   /* IR peak position (system latency + speaker->mic time of flight) */
    float level;           /* in-band mean |H(f)| — broadband sensitivity (linear, ref-normalized) */
    float band[3];         /* low / mid / high mean |H(f)| (diagnostic; not written to the layout) */
} MeasureResult;

/* Fill out[n] with an exponential sine sweep from f1 to f2 Hz at fs, with short raised-cosine fades
 * at both ends (no click). This is the signal to play out a speaker; the SAME array is the reference
 * passed to measure_response. n is arbitrary (a power of two is not required). */
void measure_sweep(float* out, int n, double f1, double f2, double fs);

/* Recover one speaker's response: deconvolve `capture` (the mic recording, ncap samples) against
 * `ref` (the played sweep, nref samples) and analyze the IR. `band_hz[2]` are the low|mid and mid|high
 * crossover frequencies (e.g. {300, 3000}); the measurement band is [f1*2, f2/2] derived from the
 * sweep extent passed as f1/f2. Returns 1 on success, 0 on allocation failure. Allocates internally
 * (control thread / offline only) — never call on the audio thread. */
int measure_response(const float* capture, int ncap, const float* ref, int nref,
                     double f1, double f2, double fs, const double band_hz[2], MeasureResult* out);

#ifdef __cplusplus
}
#endif

#endif /* BW_MEASURE_H */
