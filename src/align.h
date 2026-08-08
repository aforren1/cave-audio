/*
 * align.h — the output alignment stage: per-speaker gain trim + delay line, to align
 * arrival times from unequally-distant CAVE speakers (docs/spatialization.md). align_create
 * allocates on the control thread; align_process runs on the audio thread (no alloc/lock).
 */
#ifndef BWA_ALIGN_H
#define BWA_ALIGN_H

#include "layout.h"

#include <stdint.h>

typedef struct Aligner Aligner;

/* sample_rate derives the room_eq biquad coefficients (the layout stores rate-independent fc/Q). */
Aligner* align_create(uint32_t channels, const Layout* L, uint32_t sample_rate);   /* NULL on alloc failure */
void     align_destroy(Aligner* a);
void     align_process(Aligner* a, float* bus, uint32_t nframes);  /* in place; planar bus */

/* Tracked room EQ (layouts with a room_eq_grid): set the section-gain targets (dB, <= 0) the next
 * align_process blocks slew toward — rt.c interpolates them from the grid at the live listener
 * position. gain_db is [channel][section] over the grid's fc/q ladder. AUDIO thread (the same thread
 * as align_process — plain stores, no atomics needed); no-op for a gridless layout. */
void     align_room_eq_targets(Aligner* a, const float (*gain_db)[BWA_ROOM_EQ_MAX]);

/* ---- tracked listener alignment (bwa_set_tracked_align + _guards; OFF by default) ----------------
 * The layout's delay/gain trims align the array at ONE point (Layout.ref). These three calls add an
 * EXTRA per-channel delay and gain on top, re-referencing that alignment onto the tracked listener:
 * rt.c derives the targets from |speaker - listener| against |speaker - ref| (listener_align_track),
 * align_process slews toward them.
 *
 * Every delay change is a resampling event, so the slew is RATE LIMITED (align_tracked_slew) — a
 * listener who outruns the limit gets stale alignment instead of a pitch-shifted array. While no
 * channel is displaced, align_process runs its original INTEGER delay tap, so the default (off) path
 * is bit-identical to a build without this feature; the fractional (linear-interpolated) tap only
 * engages once something leaves identity, and disengages again the moment everything lands back.
 * All three are AUDIO thread, like align_process. */

/* Aim the comp at `delay_frames` (>= 0, extra frames per channel; rt.c normalizes the minimum to 0 so
 * a listener standing at `ref` is exact identity) and `gain_lin` (per-channel linear trim, clamped to
 * +/-6 dB). Both arrays are `channels` long. Pass NULL for BOTH to aim at identity (feature off) —
 * align then SLEWS back rather than stepping out. Targets past the delay ceiling clamp. */
void     align_tracked_targets(Aligner* a, const float* delay_frames, const float* gain_lin);
/* The rate limit: max delay change in frames per second (<= 0 ignored). Live; re-sent every block. */
void     align_tracked_slew(Aligner* a, float frames_per_s);
/* The comp delay ceiling in frames (the extra ring headroom align_create reserved). rt.c clamps its
 * targets to this, so a listener far outside the array degrades instead of wrapping the ring. */
uint32_t align_tracked_max_frames(const Aligner* a);
/* Readback of the CURRENT (slewed, not target) comp state — diagnostics and tests. Either pointer may
 * be NULL; both are `channels` long. */
void     align_tracked_state(const Aligner* a, float* delay_frames, float* gain_lin);

#endif /* BWA_ALIGN_H */
