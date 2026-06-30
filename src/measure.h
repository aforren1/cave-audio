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
    float delay_frac;      /* sub-sample refinement of the peak [-0.5,0.5]; true delay = delay_samples + delay_frac */
    float level;           /* in-band mean |H(f)| — broadband sensitivity (linear, ref-normalized) */
    float band[3];         /* low / mid / high mean |H(f)| (diagnostic; not written to the layout) */
} MeasureResult;

/* Room characterization from the captured impulse response — a treatment diagnostic, NOT a model to
 * match (matching would double-count: the engine renders the virtual room AND the real room adds its
 * own). RT60 tells you how live the room is (and the floor on what you can render); the early
 * reflections tell you which nearby surfaces to treat to protect localization. */
typedef struct {
    float rt60;            /* broadband reverberation time (s), 0 if the decay never reaches the fit range */
    int   er_count;        /* number of early reflections found (<= 8) */
    int   er_delay[8];     /* each reflection's delay in samples AFTER the direct arrival */
    float er_level[8];     /* each reflection's level relative to the direct (linear) */
} RoomResult;

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

/* Room report: deconvolve as measure_response, then characterize the room from the IR (Schroeder RT60
 * + early reflections). If `ir_out` is non-NULL, the deconvolved impulse response from the direct
 * arrival onward is copied into it (up to `ir_cap` samples) — the room-preview convolution kernel, so
 * one capture serves trims, this report, AND a future headphone room simulator. Returns 1 / 0. */
int measure_room(const float* capture, int ncap, const float* ref, int nref,
                 double f1, double f2, double fs, RoomResult* out, float* ir_out, int ir_cap);

/* Schroeder RT60 + early reflections directly from an impulse response (pure; `direct_idx` is the
 * direct-arrival sample). Used by measure_room and unit-tested on synthetic IRs. */
void measure_rt60(const float* ir, int nir, int direct_idx, double fs, RoomResult* out);

#ifdef __cplusplus
}
#endif

#endif /* BW_MEASURE_H */
