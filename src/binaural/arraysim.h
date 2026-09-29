/*
 * arraysim.h: the array-sim ROOM stage. The headphone audition of the speaker array (cave_sim, and
 * cave_both's monitor tap) decodes each bus channel as a virtual speaker. On its own that decode
 * models only DIRECTION: every virtual speaker reaches the listener at the same level, at the same
 * time and with the same spectrum, wherever they stand. A real room does not work that way, and the
 * bus the audition plays already carries the layout's gain and delay trims and the tracked
 * compensations, all of which exist to fight distance and off-axis loss. Played without those,
 * the audition hears the fixes and not the problems they fix.
 *
 * So this stage puts the physics back, per channel k and for the live listener position p, on a
 * COPY of the bus that both decoders then read:
 *
 *   1. Directivity, at the model's full resolution (only when the layout carries a model): a cascade
 *      graphic EQ, one peaking biquad per model band, fitted to loss(angle(aim_k, p - pos_k), f).
 *      The section gains come from a precomputed interaction matrix (Liski and Valimaki 2017, "the
 *      accurate cascade graphic equalizer"): built and inverted on the control thread at create,
 *      one mat-vec per retarget on the audio thread. The bandwidth is set in OCTAVES (the cookbook's
 *      BW form), so the sections near Nyquist keep their width, and the fit lands within about
 *      0.8 dB of the table at and between the band centers out to 90 degrees on the Genelec 4410A
 *      model (1.5 dB at 180). Section gains slew at ARRAYSIM_EQ_SLEW_DB_S and saturate at
 *      +/-ARRAYSIM_EQ_MAX_DB.
 *   2. Distance gain r0 / d_k: d_k = |p - pos_k| floored at 0.3 m, r0 = the mean |ref - pos_k| (so
 *      the overall level at the listening point stays about where it was), clamped to
 *      [-24, +18] dB, ramped per sample. ABSOLUTE on purpose: the calibration trims divide 1/r out
 *      and equalize sensitivity only, so a real listener at ref hears the nearer speakers louder.
 *   3. Propagation delay (d_k - C) / c * rate frames: c = the engine's live speed of sound,
 *      C = min_k |ref - pos_k| (the nearest speaker at ref arrives with no added latency beyond the
 *      interpolator's fixed ARRAYSIM_BASE_FRAMES), clamped at 0. A fractional delay line read with a
 *      16-tap Kaiser-windowed sinc (beta 6, 1024 phases, nearest phase): flat within 0.03 dB to
 *      16 kHz at 44.1 and 48 kHz for every fraction. Not the linear tap align.c and the Doppler line
 *      use: that one is a low-pass whose depth follows the fraction (6 dB at 16 kHz half a sample
 *      off), so a walking listener sweeping the fraction would hear the interpolator, not the room.
 *      The price is the kernel's half-length of fixed latency. The tap creeps at a physical closing
 *      speed (ARRAYSIM_CLOSING_MS, about 3 m/s), and the Doppler that creep produces is the real
 *      one, so there is no dead zone.
 *
 * Stage 1 and 2 retarget only when the listener moved more than 1 cm (or c changed); stage 3's
 * creep runs every sample. The first block after create or reset snaps every target (there is
 * nothing to glide from).
 *
 * MEMORY, fixed at create: a delay ring per channel sized for a listener up to ARRAYSIM_EXCURSION_M
 * from ref at 343 m/s (a listener further out, or a slower c, CLAMPS at the ring's ceiling rather
 * than wrapping), the channels x max_block scratch the decoders read (plus 4 x max_block for the
 * EQ's four-channel interleave), and the EQ state (a fixed ~110 KB of arrays in the struct). The
 * ring is 26 x 1024 floats = 104 KB on the default grid at 48 kHz; the engine asks for max_block =
 * BWA_MAX_BLOCK (8192), which makes the scratch the big term, 832 KB at 26 channels.
 * arraysim_bytes reports the whole footprint.
 *
 * Threading: create/destroy/reset on the control thread (reset only while the audio thread is
 * stopped); apply and the readbacks on the audio thread. No allocation, lock or syscall after
 * create. The bus apply reads is never written, which cave_both needs: its bus IS the array's
 * device buffer, and the array must never hear the monitor's room.
 */
#ifndef BWA_ARRAYSIM_H
#define BWA_ARRAYSIM_H

#include "core/layout.h"        /* Layout, Directivity, BWA_CHANNELS */

#include <stddef.h>
#include <stdint.h>

#define ARRAYSIM_EXCURSION_M   4.0f   /* ring sizing: the listener's reach from ref (at 343 m/s) */
#define ARRAYSIM_CLOSING_MS    3.0f   /* delay creep limit: a physical closing speed, m/s */
#define ARRAYSIM_MIN_DIST_M    0.3f   /* distance-gain floor: nobody's ear is inside a cabinet */
#define ARRAYSIM_EQ_SLEW_DB_S 96.0f   /* graphic-EQ section glide: the real room's loss follows the
                                       * head with no lag, so this is 4x the comp's 24 dB/s (the
                                       * audition must not hide the comp's glide under its own) */
#define ARRAYSIM_EQ_MAX_DB    36.0f   /* per-section magnitude cap: a deep rear null saturates */
#define ARRAYSIM_INTERP_TAPS  16      /* the delay tap's windowed-sinc length */
#define ARRAYSIM_BASE_FRAMES  8       /* its fixed latency (half the taps): every channel arrives this
                                       * many frames later than (d_k - C) / c * rate */

typedef struct ArraySim ArraySim;

/* Build the stage for this layout's speakers (positions, aims, ref, and the directivity model when
 * it has one). max_block = the largest n apply will see. NULL on a bad argument or allocation
 * failure. Control thread. */
ArraySim* arraysim_create(const Layout* L, uint32_t sample_rate, uint32_t max_block);
void      arraysim_destroy(ArraySim* s);
/* Forget the audio: clear the rings and the filter state and re-arm the first-block snap. Control
 * thread, audio thread stopped (bwa_start calls it). */
void      arraysim_reset(ArraySim* s);

/* The room-applied copy of the planar `bus` (channels x n) for a listener at `p`, speed of sound
 * `sos_mps` (non-finite or out of range reads as 343). Returns `bus` itself when s is NULL or n is 0
 * or past max_block. Audio thread. */
const float* arraysim_apply(ArraySim* s, const float* bus, const float p[3], float sos_mps, uint32_t n);

/* ---- readbacks (tests, diagnostics); audio thread or with it stopped ---- */
/* The current (crept, ramped) delay in frames, WITHOUT the fixed ARRAYSIM_BASE_FRAMES, and distance
 * gain (linear) per channel; either pointer may be NULL. */
void     arraysim_state(const ArraySim* s, float* delay_frames, float* gain_lin);
/* The graphic EQ's band centers (Hz): returns the section count (0 without a model); hz may be NULL,
 * else BWA_DIR_MAX_BANDS long. */
uint32_t arraysim_eq_bands(const ArraySim* s, float* hz);
/* Channel k's current section gains (dB), arraysim_eq_bands() long. */
void     arraysim_eq_gains(const ArraySim* s, uint32_t k, float* gain_db);
/* The delay ceiling in frames, and the stage's whole heap footprint in bytes. */
uint32_t arraysim_max_delay_frames(const ArraySim* s);
size_t   arraysim_bytes(const ArraySim* s);

#endif /* BWA_ARRAYSIM_H */
