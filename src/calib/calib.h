/*
 * calib.h — turn per-speaker acoustic measurements into layout trims, and write them back into
 * cave_layout.json. The measurement DSP is in measure.h; the full-duplex ASIO capture that feeds it
 * is the calibration tool (examples/calibrate.c). This part is pure + file I/O only (no audio thread,
 * no ASIO), so it is unit-tested (test/calib_test.c).
 */
#ifndef BWA_CALIB_H
#define BWA_CALIB_H

#include "calib/measure.h"
#include "core/layout.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Solve per-speaker trims from the measurements + geometry:
 *   delay_ms[i] aligns every speaker's arrival to the FARTHEST (the latest measured delay -> 0 trim,
 *               nearer speakers delayed to match). The common system latency cancels in the difference.
 *   gain_db[i]  equalizes SENSITIVITY: the measured level is back-projected to a reference by the
 *               speaker->mic distance (level*dist, factoring out 1/r), then trimmed cut-only (<= 0 dB,
 *               relative to the least-sensitive speaker) so nothing is boosted into clipping.
 * `pos[i]` and `mic` are room-space meters; a near-silent speaker (level ~ 0, e.g. unplugged) gets
 * 0 dB and is excluded from the reference. fs is the sample rate. */
void calib_solve(const MeasureResult* m, const float (*pos)[3], const float mic[3], int n, double fs,
                 float* gain_db, float* delay_ms);

/* calib_solve with a per-speaker sensitivity correction: `corr[i]` (linear amplitude, > 0) multiplies
 * speaker i's measured level before the equalization; NULL = calib_solve. The calibration tool derives
 * it from the layout's directivity model (calib_directivity_corr: loss(listening-point bearing) /
 * loss(mic bearing), acting on the capture's direct share only), so a trim measured off a speaker's
 * axis is re-aimed to what the listening point hears of that speaker,
 * the way the 1/r term is already divided out. With the mic AT the listening point every factor is 1
 * and the two solves agree exactly. A non-finite or non-positive factor reads as 1. */
void calib_solve_corr(const MeasureResult* m, const float (*pos)[3], const float mic[3], int n, double fs,
                      const float* corr, float* gain_db, float* delay_ms);

/* The `corr` for calib_solve_corr from the layout's directivity model. Per speaker, the model's
 * free-field ratio r = D(ref) / D(mic): the amplitude loss toward the listening point over the loss
 * toward the mic, each averaged uniformly over [f_lo, f_hi] the way measure.c's level is (pass 2*f1
 * and f2/2 of the sweep). But `level` is the WHOLE response, and in a live room part of it is
 * reverberant energy that does not follow the speaker's axis, so applying r to all of it would
 * over-correct. With f = m[i].direct_frac (the gated share of the level band's energy):
 *
 *     corr = sqrt(f * r^2 + (1 - f)),   computed as sqrt(1 + f * (r^2 - 1))
 *
 * which is r in an anechoic capture (f = 1), 1 in a purely diffuse one (f = 0), and in between
 * otherwise; the second spelling makes a mic AT the listening point (r == 1 exactly, same bearing)
 * give exactly 1 whatever f is. `m` may be NULL: every f reads as 1 (the free-field factor). A
 * non-finite f also reads as 1. Fills corr[count] and returns 1 when the layout carries a model;
 * returns 0 and leaves corr alone when it does not. The ONE implementation of this factor, so the
 * CLI and the GUI cannot write different trims. */
int calib_directivity_corr(const Layout* L, const float mic[3], double f_lo, double f_hi,
                           const MeasureResult* m, float* corr);

/* ---- aim check (bwa_calibrate --check-aim; docs/calibration.md) --------------------------------
 * The directivity model multiplies each speaker's AIM, and the aim is the one input nothing else
 * verifies: a mount pointed at the floor, or a layout `aim` typed wrong, is 2 to 4 dB of treble at
 * the listening point on a waveguide monitor. The --localize captures already hold what is needed:
 * every speaker swept from K known mic positions, each yielding a mid and high band level of the
 * DIRECT sound (measure.c's band_direct[1]/[2]: the IR gated before the first reflection, because
 * the reverberant part does not follow the axis, and left in, it dilutes the bearing dependence and
 * the fit reads errors too small). Their ratio, the TILT, changes with the bearing off the axis the
 * way the model says, and the speaker's own response and the mic cancel out of it as a per-speaker
 * constant. So: predict the tilt at each position for a candidate aim, remove the mean, and the
 * residual RMS scores the aim. A grid search around the layout's aim finds the best one.
 *
 * The bands are CALIB_AIM_MID_HZ..CALIB_AIM_HIGH_HZ (mid) and CALIB_AIM_HIGH_HZ..f2 (high), not the
 * level bands' 300 Hz / 3 kHz: a gate of a few ms smears the spectrum over 1/gate (250 Hz at 4 ms,
 * 500 Hz behind a 2 ms floor bounce), and the gate length changes with the mic position, so a mid
 * band starting at 300 Hz would carry a position-dependent window error into exactly the quantity
 * the fit compares across positions. From 1 kHz the band sits at least two resolution cells above
 * that. The cost is contrast: on the Genelec 4410A model the 45 deg tilt is 2.8 dB instead of 3.8.
 * The measurement (the band_hz passed to measure_response) and the model (calib_aim_tilt_db's
 * band_hz) MUST use the same bands; the CLI passes these constants to both.
 *
 * What it can resolve: an aim error of the order of the bearing SPREAD the positions give it, so
 * with positions spanning 20 to 30 deg of bearing, roughly 10 deg. What it cannot: the balloon
 * shape (far too little coverage, and gated HF in a live room is noisy at the dB level), which is
 * why it fits ONE direction and refuses below a minimum spread rather than reporting a confident
 * wrong answer. Diagnostic only; nothing writes the fitted aim back. */
#define CALIB_AIM_MID_HZ  1000.0
#define CALIB_AIM_HIGH_HZ 3000.0
typedef struct {
    int   ok;              /* 1 = fitted; 0 = refused (no model, < 3 positions, or < 8 deg of bearing
                            * spread); the layout-aim fields below are still filled when a model exists */
    int   npos;
    float spread_deg;      /* max bearing difference between any two positions: the fit's leverage */
    float rms_layout_db;   /* tilt residual RMS (mean removed) with the layout's aim */
    float rms_fit_db;      /* ... with the fitted aim */
    float aim_fit[3];      /* unit vector; = the layout aim when refused */
    float aim_err_deg;     /* angle between the layout aim and the fit (0 when refused) */
} CalibAimResult;

/* The model's predicted TILT (dB) at `angle_deg` off axis: the high band's mean amplitude loss over
 * [band_hz[1], f2] against the mid band's over [band_hz[0], band_hz[1]], each averaged the way
 * measure.c's band means are. 0 without a model. */
float calib_aim_tilt_db(const Directivity* d, float angle_deg, const double band_hz[2], double f2);

/* Fit speaker `s`'s aim from measured tilts: `tilt_db[k]` = 20 log10(band_direct[2]/band_direct[1])
 * of its capture at `mic[k]`, K positions, `band_hz`/`f2` as passed to measure_response. Uses L->speakers[s].pos
 * (pass a layout carrying the SOLVED positions) and .aim as the starting point. Pure. Returns
 * out->ok. */
int calib_check_aim(const Layout* L, int s, const float (*mic)[3], const float* tilt_db, int K,
                    const double band_hz[2], double f2, CalibAimResult* out);

/* Read the layout JSON at `in_path`, set each speaker's `gain_db` + `delay_ms` (preserving index,
 * position, the dbap block, everything else), and write to `out_path` (may equal in_path). The file's
 * speaker count must equal `n`. Returns 1 on success, 0 with a message in `err`. */
int calib_write_layout(const char* in_path, const char* out_path,
                       const float* gain_db, const float* delay_ms, int n, char* err, size_t errcap);

/* The layout file's recorded speed of sound (reference.speed_of_sound_mps), the room-temperature c
 * every acoustic RANGE in a survey is scaled by. Reads 1 + the value when the file carries a
 * plausible one (see sos.h), 0 otherwise — the caller then falls back to BWA_SOS_REF_MPS. Keeping it
 * in the file means an install sets its temperature once instead of remembering a flag per run, and
 * a collaborator rig at a different temperature carries its own. calib_write_sos records it back
 * (creating the `reference` block if the file predates the field); like every calib_write_*, it
 * re-parses and re-serializes so unknown fields survive. */
int calib_read_sos(const char* path, double* out_mps);
int calib_write_sos(const char* in_path, const char* out_path, double mps, char* err, size_t errcap);

/* Acoustic self-localization: solve one speaker's 3D position from its measured RANGE (= c * delay,
 * in meters, latency INCLUDED) to K known mic positions. The unknown constant system latency is
 * recovered jointly (as a range, c*tau) by linear least squares — so no separate loopback is needed.
 * Needs K >= 5 non-coplanar mic positions (more = more robust). `mic[k]` and `pos_out` are room meters.
 * Returns 1 + pos_out (+ latency_out = recovered c*tau if non-NULL); 0 if underdetermined/singular.
 * This sees speakers OPTICAL trackers can't (the sweep passes through acoustically-transparent screens). */
int calib_trilaterate(const double* range, const float (*mic)[3], int K, float* pos_out, double* latency_out);

/* Write recovered speaker positions back into the layout JSON (sets each speaker's "position" [x,y,z],
 * preserving everything else). `pos[i]` are room meters; the file's speaker count must equal `n`. */
int calib_write_positions(const char* in_path, const char* out_path, const float (*pos)[3], int n, char* err, size_t errcap);

/* Per-speaker correction FIR with the calibration gate policy: `ir` starts at the direct arrival;
 * `first_refl` is the samples to the first reflection (0/unknown -> a default ~4 ms window). Gates so
 * the filter corrects the SPEAKER (direct sound), not the room, then inverts (see measure_correction).
 * Writes ntaps into `taps`. Returns 1 / 0. */
int calib_eq(const float* ir, int nir, int first_refl, double fs, int ntaps, float* taps);

/* Write per-speaker correction FIRs into the layout JSON as each speaker's "eq" array (replacing any
 * prior), preserving everything else. `taps` is n * max_taps row-major; `lens[i]` taps for speaker i
 * (0 = remove its eq). The file's speaker count must equal `n`. Returns 1 / 0. */
int calib_write_eq(const char* in_path, const char* out_path, const float* taps, const uint16_t* lens,
                   int n, int max_taps, char* err, size_t errcap);

/* STATIC-LISTENER room correction (docs/calibration.md): only valid when the listener sits at the
 * measurement point (the fixed-observer SPCAP/VBAP deployments). Produces BOTH halves from one IR:
 *   taps — a correction FIR from a frequency-dependent window (direct-gated at HF == the speaker EQ
 *          above; growing to include the room toward LF), covering 200 Hz up, boost-capped at +3 dB;
 *   cuts — LF modal peaking CUTS (30..200 Hz), where modes are minimum-phase and correctable.
 * The 200 Hz split means nothing is corrected twice. Returns the cut count (0 = flat), -1 on failure. */
int calib_room_eq(const float* ir, int nir, int first_refl, double fs, int ntaps, float* taps,
                  MeasureEqSection* cuts, int max_cuts);

/* Write per-speaker LF modal cuts into the layout JSON as each speaker's "room_eq" array of
 * { fc, gain_db, q } objects (replacing any prior; count 0 = remove). `cuts` is n * max_sections
 * row-major with `counts[i]` used per speaker. Returns 1 / 0. */
int calib_write_room_eq(const char* in_path, const char* out_path,
                        const MeasureEqSection* cuts, const int* counts, int n,
                        int max_sections, char* err, size_t errcap);

/* TRACKED room EQ (docs/calibration.md): merge ONE speaker's modal cuts measured at `npos` mic
 * positions into the congruent ladder the layout's room_eq_grid needs. Room modes don't move with
 * the mic — only their measured depth does — so per-position fcs within `tol_rel` (e.g. 0.08) are
 * the SAME mode: one output section per cluster, fc/q = the member medians, and per position that
 * position's measured depth (0 dB where it didn't see the mode). Keeps the deepest `max_out`
 * clusters. `cuts` is npos * max_in row-major with counts[p] used; writes fc/q (max_out) +
 * gain_db (npos * max_out row-major, <= 0). Returns the ladder size (0 = flat everywhere). */
int calib_room_grid_merge(const MeasureEqSection* cuts, const int* counts, int npos, int max_in,
                          double tol_rel, int max_out, float* fc, float* q, float* gain_db);

/* Merge THIS mic position's per-speaker modal cuts into the layout JSON's "room_eq_grid": existing
 * grid entries are read back (their sections re-treated as that position's cuts), an entry within
 * 5 cm of `mic` is replaced (else appended, up to BWA_RQ_GRID_MAX), every speaker's ladder is
 * re-merged across all positions (calib_room_grid_merge), and the congruent grid is rewritten —
 * so one bwa_calibrate run per mic placement accumulates the grid. Removes any static per-speaker
 * "room_eq" (the schemes are mutually exclusive). `cuts` is n * max_sections row-major with
 * counts[i] used per speaker. Returns 1 / 0. */
int calib_write_room_eq_grid(const char* in_path, const char* out_path, const float mic[3],
                             const MeasureEqSection* cuts, const int* counts, int n,
                             int max_sections, char* err, size_t errcap);

/* Drift check: given measured ranges (c*delay, meters, latency included) from ONE mic at `mic` to n
 * speakers at their STORED positions `pos`, report each speaker's RADIAL deviation (meters) from where
 * it should be. The unknown common latency is removed as the MEDIAN residual (robust to a few moved
 * speakers), so a speaker bumped toward/away from the mic shows up as a non-zero deviation — one
 * fast single-position pass flags anything nudged. (Purely tangential moves don't change the range;
 * a full re-survey is calib_trilaterate.) */
void calib_check_drift(const double* range, const float (*pos)[3], const float mic[3], int n, float* deviation_m);

#ifdef __cplusplus
}
#endif

#endif /* BWA_CALIB_H */
