/*
 * measure.h — acoustic measurement DSP for speaker calibration (pure, no I/O, no audio thread).
 *
 * The method is the textbook exponential sine sweep (ESS, Farina): play a known log sweep out one
 * speaker, capture it at an omnidirectional mic, and recover that speaker's impulse response by
 * regularized deconvolution H(f) = Capture·conj(Ref) / (|Ref|^2 + eps). From the IR we read:
 *   - delay_samples : the IR peak position = system latency + speaker->mic time of flight.
 *   - level         : the in-band average |H(f)| = the speaker's broadband sensitivity at the mic.
 *   - band[3]       : low / mid / high average |H(f)| (a coarse response shape; diagnostic only).
 *   - the same two, *_direct, from the IR GATED to the direct sound (measure_direct_gate), plus the
 *     direct-energy fraction: what the speaker's axis governs, with the room's reverberant part out.
 *
 * The calibration tool (examples/calibrate.c) runs this per speaker, then turns the 26 results into
 * per-speaker delay trims (align all arrivals to the farthest) and gain trims (equalize sensitivity,
 * with the layout's speaker->mic distance divided out) written back into cave_layout.json.
 *
 * This file is deliberately I/O-free and hardware-free so the DSP is unit-tested (test/measure_test.c)
 * against a synthetic capture with a known delay + gain + low-pass — the ASIO capture is the only part
 * that needs the rig.
 */
#ifndef BWA_MEASURE_H
#define BWA_MEASURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int   delay_samples;   /* IR peak position (system latency + speaker->mic time of flight) */
    float delay_frac;      /* sub-sample refinement of the peak [-0.5,0.5]; true delay = delay_samples + delay_frac */
    float level;           /* in-band mean |H(f)| — broadband sensitivity (linear, ref-normalized) */
    float band[3];         /* low / mid / high mean |H(f)| (diagnostic; not written to the layout) */
    /* The same means over the DIRECT sound only: the IR windowed from 1 ms before its peak to
     * gate_samples after it (raised-cosine rise over that 1 ms, fall over the gate's last quarter),
     * the gate end per measure_direct_gate from the first reflection this IR shows. level and band
     * above keep their whole-response meaning (the trims are built on them). RESOLUTION: a gate of
     * T seconds smears the spectrum over about 1/T Hz (250 Hz at the 4 ms default), so
     * band_direct[0] and the bottom of the mid band describe a truncated response, not the speaker. */
    float level_direct;
    float band_direct[3];
    float direct_frac;     /* gated / whole-response ENERGY over the level band, in [0, 1]: how much of
                            * `level` is direct sound (1 in an anechoic capture; lower in a live room) */
    int   gate_samples;    /* the gate end, samples after the peak */
    float energy;          /* whole-response ENERGY over the level band (sum of |H|^2): what direct_frac is
                            * a share of, so several captures' shares can be pooled energy-weighted
                            * (zylia_pressure_proxy) */
    /* The capture's own quality (measure_response_ex). The arrival above is the FIRST strong arrival
     * inside the expected-arrival window when one was given (MEASURE_FIRST_*), else the strongest tap
     * over the whole positive-lag IR. */
    int   win_lo, win_hi;  /* the window searched, capture samples [win_lo, win_hi); 0, 0 = none */
    int   peak_any;        /* the strongest tap over the whole positive-lag IR [0, ncap) */
    int   outside;         /* 1 = peak_any lies outside the window: something other than this speaker
                            * was louder than its arrival. Never set without a window. */
    float outside_db;      /* |ir[peak_any]| over |ir[arrival]|, dB (0 when the two are the same tap) */
    float snr_db;          /* |ir[arrival]| over the noise floor (measure_noise_floor), dB; 0 = no region */
    float floor_rms;       /* that floor: the RMS of the loudest MEASURE_NOISE_BLOCK_S block, linear */
    int   noise_n;         /* samples the floor was read from (0 = the regions were empty: snr unknown) */
} MeasureResult;

/* In a room the strongest tap is not always the direct sound: on the rig, three of 26 speakers carried a
 * reflection within 4 dB of an arrival 2.5 to 7 ms ahead of it, and noise flipped which one won. So a
 * windowed search walks back from the strongest tap and takes the earliest one within MEASURE_FIRST_DB of
 * it. The rig's clean speakers had nothing above -35 dB there, and the real early arrivals sat above
 * -5 dB, so the limit has room on both sides. The minimum lead keeps the peak's own rising edge and a
 * two-way speaker's crossover smear (under 0.6 ms, -25 dB on the rig) from reading as an arrival. */
#define MEASURE_FIRST_DB      (-12.0)
#define MEASURE_FIRST_BACK_S  0.020
#define MEASURE_FIRST_MIN_S   0.001

/* The noise-floor regions (measure_response_ex). Both sit on the IR's positive lags, where the
 * linear deconvolution (FFT length >= ncap + nref, so nothing wraps) puts the speaker's response:
 *   before: [0, lo - MEASURE_NOISE_GUARD_S): earlier than this speaker's sound can arrive. The
 *           window's low edge is the earliest physical arrival, and the guard keeps the arrival's
 *           own band-edge ringing out.
 *   after:  [hi + MEASURE_NOISE_SKIP_S, ncap): past the window's high edge plus a room's decay, so
 *           the reflections and the reverberant tail have died. Lags past ncap - nref see only the
 *           sweep's low end, so this region is a narrower band than the first; it is where a
 *           transient late in the capture shows.
 * The exponential sweep's harmonic-distortion products land AHEAD of their arrival: order k by
 * nref ln(k) / ln(f2 / f1) samples (measure_harmonic_lead), 150 ms for the 1.5 s sweep's second
 * harmonic and 50 ms for the 0.5 s live sweep's. An arrival earlier than that puts the product at a
 * negative lag, past ncap in the FFT buffer, where neither region holds it. A later one (the rig's
 * ZM-1 chain is about 65 ms) puts it in the "before" region, so measure_response leaves
 * MEASURE_HARM_GUARD_S either side of orders 2..MEASURE_HARM_MAX of the found arrival out of both
 * regions: a speaker's distortion is not the room's noise. With
 * no window, lo = hi = the found arrival. The floor is the RMS of the LOUDEST block, so one cough
 * in an otherwise quiet capture reads as the cough. */
#define MEASURE_NOISE_GUARD_S 0.002
#define MEASURE_NOISE_SKIP_S  0.30
#define MEASURE_NOISE_BLOCK_S 0.02
#define MEASURE_NOISE_MIN_N   64       /* fewer samples than this in a region: the region is skipped */
#define MEASURE_HARM_MAX      4        /* harmonic orders 2..4 are kept out of the noise regions */
#define MEASURE_HARM_GUARD_S  0.005

/* Room characterization from the captured impulse response — a treatment diagnostic, NOT a model to
 * match (matching would double-count: the engine renders the virtual room AND the real room adds its
 * own). RT60 tells you how live the room is (and the floor on what you can render); the early
 * reflections tell you which nearby surfaces to treat to protect localization.
 *
 * The decay is read per OCTAVE BAND, because treatment is chosen per band: a room that rings at
 * 125 Hz needs bass traps, one that rings at 2 kHz needs absorption on its walls, and one broadband
 * number cannot tell them apart. Every figure is T20 (the line through the decay curve from -5 to
 * -25 dB) scaled to 60 dB, the existing convention.
 *
 * A figure is only as good as the decay above the noise. `range_db` is how far the decay curve falls
 * from its start before the noise floor takes over (the curve's level where the decay crosses the
 * floor), or before the IR ends when the decay is still above the noise there. T20 ends at -25 dB and wants the floor 10 dB under that, so a band with less than
 * MEASURE_RT_MIN_RANGE_DB of range reports rt60 = 0 beside its range: no reading, never a guess.
 *
 * rt60 = 0 with range_db = 0 means there was NOTHING TO MEASURE, which is not the same as too
 * noisy: no decay follows the direct sound in that band (an anechoic capture: what is left after
 * the arrival is more than 30 dB under the band's whole energy, or it dies in under 30 ms or two
 * periods of the bandwidth), a non-finite sample, or a band the sweep does not cover.
 *
 * THE 0 dB REFERENCE IS THE REVERBERANT ONSET, not the direct sound. The bands (and the broadband
 * figure of measure_rt60_ex and measure_room) are filtered backward in time, which moves the
 * direct sound's band energy ahead of the arrival, out of the curve. In a room that leaves T20
 * alone (a backward filter turns an exponential into the same exponential), but range_db is then
 * counted from the reverberation's start and reads 10 log10(1 + D/R) under the ISO figure that
 * counts from the direct sound. Plain measure_rt60 filters nothing for its broadband figure, so
 * that one figure keeps the direct sample. */
#define MEASURE_RT_BANDS         8
#define MEASURE_RT_MIN_RANGE_DB  35.0
/* the octave-band centers, Hz (ISO 3382 / IEC 61260 nominal): 63 ... 8000 */
extern const float measure_rt_band_hz[MEASURE_RT_BANDS];

typedef struct {
    float rt60;            /* broadband reverberation time (s), 0 = no reading (range_db says why). Read
                            * over the valid band [f_lo, f_hi] when one is given (measure_room,
                            * measure_rt60_ex), not over the raw IR: a sweep's deconvolution raises noise
                            * 3 dB per octave, so the part above f_hi was most of the floor. */
    float range_db;        /* the broadband decay's usable range, dB (capped at 99 = not noise-limited) */
    /* The same two per octave band. A band whose edges (fc / sqrt 2, fc sqrt 2) leave the valid
     * measurement band reads 0 and 0: with the tools' 20 Hz to 20 kHz sweep that band is
     * [40 Hz, 10 kHz], which holds 63 Hz to 4 kHz and leaves 8 kHz out (its top edge is 11.3 kHz). */
    float band_rt60[MEASURE_RT_BANDS];
    float band_range_db[MEASURE_RT_BANDS];
    int   er_count;        /* number of early reflections found (<= 8) */
    int   er_delay[8];     /* each reflection's delay in samples AFTER the direct arrival */
    float er_level[8];     /* each reflection's level relative to the direct (linear) */
} RoomResult;

/* Whether measure_room reads the octave band at fc: both edges (fc / sqrt 2, fc sqrt 2) inside the
 * valid band [f_lo, f_hi], and the top edge under 0.45 fs (fs <= 0 skips that limit). For a sweep from
 * f1 to f2 the valid band is [2 f1, f2 / 2]. measure_room applies this rule, and a report lists exactly
 * these bands: a covered band with no reading at all (0 s, 0 dB) must still show, or it reads as a band
 * the sweep never covered. */
int measure_rt_band_covered(double fc, double f_lo, double f_hi, double fs);

/* The room's figures over several speakers' RoomResults. A MEDIAN over the speakers with a reading
 * (rt60 > 0), so one speaker in a corner, or one capture with a door slam in it, does not move the
 * room's figure, and a speaker with no reading is not a reading of 0 s. An even count takes the mean of
 * the middle two. A band or broadband no speaker read is 0 s with a count of 0. */
typedef struct {
    int   speakers;                          /* rows summarized (n) */
    float rt60;                              /* broadband median (s), 0 = no speaker gave a reading */
    int   count;                             /* speakers with a broadband reading */
    float band_rt60[MEASURE_RT_BANDS];       /* the same per octave band */
    int   band_count[MEASURE_RT_BANDS];
} RoomSummary;
void measure_room_summary(const RoomResult* rows, int n, RoomSummary* out);

/* Fill out[n] with an exponential sine sweep from f1 to f2 Hz at fs, with short raised-cosine fades
 * at both ends (no click). This is the signal to play out a speaker; the SAME array is the reference
 * passed to measure_response. n is arbitrary (a power of two is not required). */
void measure_sweep(float* out, int n, double f1, double f2, double fs);

/* The calibration's direct-sound gate policy, ONE implementation (calib_eq, calib_room_eq and
 * measure_response's direct fields all use it): end the gate 4 samples before the first reflection
 * when one is known (`first_refl` > 8 samples after the direct peak), else a 4 ms default (long
 * enough to hold a speaker's own response, short enough to exclude most rooms). Returns samples. */
int measure_direct_gate(int first_refl, double fs);

/* Recover one speaker's response: deconvolve `capture` (the mic recording, ncap samples) against
 * `ref` (the played sweep, nref samples) and analyze the IR. `band_hz[2]` are the low|mid and mid|high
 * crossover frequencies (e.g. {300, 3000}); the measurement band is [f1*2, f2/2] derived from the
 * sweep extent passed as f1/f2. Returns 1 on success, 0 on allocation failure. Allocates internally
 * (control thread / offline only) — never call on the audio thread. */
int measure_response(const float* capture, int ncap, const float* ref, int nref,
                     double f1, double f2, double fs, const double band_hz[2], MeasureResult* out);

/* measure_response, plus a window of the impulse response on the ABSOLUTE capture time axis: win[i]
 * = ir[*win_start + i] for i < win_len, *win_start = the arrival peak - `pre` (clamped at 0), zeros
 * past the IR's end. Several captures' windows then share one time axis, which is what a
 * cross-correlation of them needs (zylia_ir_tdoa). win NULL = measure_response. */
int measure_response_win(const float* capture, int ncap, const float* ref, int nref,
                         double f1, double f2, double fs, const double band_hz[2], MeasureResult* out,
                         float* win, int win_len, int pre, int* win_start);

/* measure_response_win with an EXPECTED-ARRIVAL window: the arrival peak is searched in
 * [exp_lo, exp_hi) only (clamped to the capture), and out->outside says whether a stronger tap sits
 * anywhere else in [0, ncap). exp_hi <= exp_lo = no window, exactly measure_response_win. The noise
 * floor and snr_db are filled either way. Inside the window the arrival is the EARLIEST tap within
 * MEASURE_FIRST_DB of the window's strongest (the walk back above), so on an unflagged capture it is the
 * whole-IR peak unless a near-equal tap leads it: that lead is the case the walk back exists for, a
 * reflection outshouting the direct sound. It differs from the whole-IR search only there, or on a
 * capture the window also flags. */
/* measure_response_ex with the arrival search narrowed to [pick_lo, pick_hi) and no walk back: the strongest
 * tap there. exp_lo/exp_hi still set the quality fields. The ZM-1 uses it so all 19 capsules read the same
 * physical arrival: decided per capsule, a capsule in the sphere's shadow picks a reflection its neighbors
 * do not, and the windows the cross-correlation compares no longer overlap. */
int measure_response_pick(const float* capture, int ncap, const float* ref, int nref,
                          double f1, double f2, double fs, const double band_hz[2],
                          int exp_lo, int exp_hi, int pick_lo, int pick_hi, MeasureResult* out,
                          float* win, int win_len, int pre, int* win_start);

/* measure_response_pick with the positive lags ending at lag_hi (0 = ncap): the peak search, the outside
 * flag and the noise floor read [0, lag_hi) only. For a capture that holds a SECOND sweep a known gap
 * later (live aiming's reference, calib_live_read): its arrival sits at a later lag of this same IR and is
 * not this speaker's interferer. */
int measure_response_lim(const float* capture, int ncap, const float* ref, int nref,
                         double f1, double f2, double fs, const double band_hz[2],
                         int exp_lo, int exp_hi, int pick_lo, int pick_hi, int lag_hi, MeasureResult* out,
                         float* win, int win_len, int pre, int* win_start);

int measure_response_ex(const float* capture, int ncap, const float* ref, int nref,
                        double f1, double f2, double fs, const double band_hz[2],
                        int exp_lo, int exp_hi, MeasureResult* out,
                        float* win, int win_len, int pre, int* win_start);

/* Samples by which an exponential sweep's order-k harmonic product leads its arrival (0 on bad input). */
double measure_harmonic_lead(int nref, int k, double f1, double f2);

/* The noise floor of an IR (the regions above, with no harmonic exclusion): the RMS of the loudest MEASURE_NOISE_BLOCK_S block in
 * [0, lo - guard) and [hi + skip, ncap). *n receives the samples looked at (0 = no region). Pure. */
double measure_noise_floor(const float* ir, int ncap, int lo, int hi, double fs, int* n);

/* Room report: deconvolve as measure_response, then characterize the room from the IR (Schroeder RT60
 * + early reflections). The bands and the broadband figure are limited to [2 f1, f2 / 2], and each
 * decay is read only over the lags the capture still covered its band at. If `ir_out` is non-NULL, the deconvolved impulse response from the direct
 * arrival onward is copied into it (up to `ir_cap` samples) — the room-preview convolution kernel, so
 * one capture serves trims, this report, AND a future headphone room simulator. Returns 1 / 0. */
/* The deconvolved IR around its strongest tap, with `pre` samples ahead of it (zero-padded at the start), so
 * out[pre] is that tap. measure_room's kernel starts AT the tap, which hides the question a survey needs
 * answered in a room: is an earlier, weaker arrival sitting ahead of the peak the tool picked? Returns 0 on
 * bad input. */
int measure_ir_around(const float* capture, int ncap, const float* ref, int nref,
                      double f1, double f2, double fs, int pre, float* out, int cap);

int measure_room(const float* capture, int ncap, const float* ref, int nref,
                 double f1, double f2, double fs, RoomResult* out, float* ir_out, int ir_cap);

/* Schroeder RT60 (broadband and per octave band) + early reflections directly from an impulse
 * response (pure; `direct_idx` is the direct-arrival sample). Each decay curve is cut where the
 * decay meets the IR's noise floor, with the noise's energy subtracted and the cut-off part of the
 * decay added back, so a long noisy tail does not read as reverberation. Every band whose top edge
 * is under 0.45 fs is read (all eight at 48 kHz). Allocates internally (two buffers a call): control thread or offline only. Used by
 * measure_room and unit-tested on synthetic IRs. */
void measure_rt60(const float* ir, int nir, int direct_idx, double fs, RoomResult* out);

/* measure_rt60 for an IR that is only valid over [f_lo, f_hi] Hz (a sweep's measurement band): an
 * octave band that does not lie wholly inside reads 0, and the broadband figure is read through a
 * band-pass over [f_lo, f_hi] (a side is left open at f_lo = 0 or f_hi >= 0.45 fs). */
void measure_rt60_ex(const float* ir, int nir, int direct_idx, double fs, double f_lo, double f_hi, RoomResult* out);

/* The band filter the decay analysis uses, exposed so its response can be tested against the
 * Butterworth formula: a 6th-order high-pass at f_hp into a 6th-order low-pass at f_lp, x in place,
 * a side at 0 left out. The report runs it backward in time (the caller reverses x). */
void measure_rt_bandpass(double* x, int n, double f_hp, double f_lp, double fs);

/* Design a per-speaker correction FIR from a measured impulse response. The IR is GATED to the direct
 * sound — `gate_len` samples from `direct`, chosen to end before the first reflection — so the filter
 * corrects the SPEAKER's own magnitude response, NOT the room (a moving listener can't be room-EQ'd
 * from one point; that's the same trap as matching RT60). The gated magnitude is inverted in-band
 * (regularized; boosts capped at `max_boost_db`, cuts at `max_cut_db`, so deep nulls aren't fought)
 * and realized as a minimum-phase FIR (no added latency or pre-ring), normalized toward unity in-band
 * so the scalar gain trim still owns overall level. taps[ntaps] receives the kernel. Returns 1 / 0.
 * Pure + allocates internally — offline/control-thread only. Unit-tested on a synthetic colored IR. */
int measure_correction(const float* ir, int nir, int direct, int gate_len,
                       double f1, double f2, double fs, double max_boost_db, double max_cut_db,
                       int ntaps, float* taps);

/* ---- STATIC-LISTENER room correction (docs/calibration.md). Only valid when the listener sits at
 * the measurement point (the fixed-observer SPCAP/VBAP deployments) — a moving listener keeps the
 * speaker-only measure_correction above. ---- */

/* Room-correction FIR via a FREQUENCY-DEPENDENT WINDOW: each frequency's magnitude is estimated from
 * the IR windowed to `cycles`/f seconds, clamped to [gate_len .. max_win_s]. At HF the window shrinks
 * to the direct-sound gate (== measure_correction's quasi-anechoic view, and the ~1/cycles-octave
 * resolution is the broad-stroke smoothing that survives head sway); at LF it grows to include the
 * room. Same inversion policy as measure_correction (regularized, boost/cut-capped, min-phase, unity
 * outside [f1, f2]) — pass f1 at the LF split so the modal band below is left to measure_room_cuts,
 * never corrected twice. Returns 1 / 0. Offline only. */
int measure_correction_room(const float* ir, int nir, int direct, int gate_len,
                            double cycles, double max_win_s,
                            double f1, double f2, double fs, double max_boost_db, double max_cut_db,
                            int ntaps, float* taps);

/* One parametric peaking section (RBJ biquad parameters, rate-independent). */
typedef struct { float fc, gain_db, q; } MeasureEqSection;

/* LF modal CUTS from a long-window magnitude estimate: find peaks in [f_lo, f_hi] that stand above a
 * ±octave smoothed baseline by >= ~3 dB (room modes are minimum-phase there, so a magnitude cut also
 * fixes the ringing), fit fc/Q from the peak's half-prominence width, and emit cut-only sections
 * (gain_db < 0, depth capped at max_cut_db). NEVER emits boosts — dips are position-dependent
 * interference and must not be fought. Returns the section count (0 = flat / nothing to cut), or -1
 * on failure. Offline only. */
int measure_room_cuts(const float* ir, int nir, int direct, double fs,
                      double f_lo, double f_hi, double max_cut_db,
                      int max_sections, MeasureEqSection* out);

#ifdef __cplusplus
}
#endif

#endif /* BWA_MEASURE_H */
