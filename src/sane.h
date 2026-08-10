/*
 * sane.h — input sanitizing for values crossing the ABI into audio-thread state. Header-only
 * (static inline) so it links into any translation unit with no separate object and no layering
 * pull-in (only <math.h>/<stdbool.h>). Not part of the public ABI.
 *
 * WHY THIS EXISTS, and it is not line count: `isfinite()` is a finiteness test, NOT a range check.
 * `isfinite(3e38)` is TRUE, so a guard that only rejects non-finite still admits a value that
 * OVERFLOWS to Inf the moment it scales the bus (test_fuzz_api seed 12648430 found exactly that on
 * master gain — see the BWA_MAX_GAIN trap in CLAUDE.md). Writing the check as `bwa_finite_clamp`
 * makes the range a REQUIRED argument, so "I checked finite and forgot the magnitude" stops being
 * expressible. That is the whole point: the signature carries the invariant.
 *
 * REJECT versus CLAMP is a real semantic difference, so both spellings exist and neither is the
 * default. Rejecting leaves the previous value in place (a bad set is a no-op); clamping installs a
 * bounded value. They are NOT interchangeable: `rt_source_set_gain` rejects a negative gain, which
 * keeps the voice at its old level, where clamping to 0 would silently MUTE it. Convert a call site
 * deliberately, one at a time, and preserve whichever behavior it documents today.
 */
#ifndef BWA_SANE_H
#define BWA_SANE_H

#include <math.h>
#include <stdbool.h>

/* Accept-and-clamp. Returns false when v is non-finite (the caller returns and leaves state
 * untouched); otherwise clamps *v into [lo,hi] and returns true. The clamp runs AFTER the
 * finiteness test on purpose: a two-sided `v < lo ? lo : (v > hi ? hi : v)` PASSES NaN, because
 * every NaN comparison is false, so ordering these the other way silently reintroduces the bug. */
static inline bool bwa_finite_clamp(float* v, float lo, float hi) {
    if (!isfinite(*v)) return false;
    if (*v < lo) *v = lo;
    if (*v > hi) *v = hi;
    return true;
}

/* Reject-only, for a scalar with no natural range (an offset, a coordinate). Prefer
 * bwa_finite_clamp wherever a bound exists — this one cannot catch finite-but-absurd. */
static inline bool bwa_finite(float v) { return isfinite(v) != 0; }

/* All three components finite: the position/direction triple guard, spelled once. */
static inline bool bwa_finite3(const float* v) {
    return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
}

/* Make q (xyzw) a UNIT quaternion in place. Returns false when any component is non-finite, and the
 * caller drops the update — every existing site keeps its previous pose, which is also what a
 * dropped tracker frame means. A finite but DEGENERATE (near zero-length) q is not an error: it
 * becomes identity, so a caller who never set an orientation still faces room-ahead.
 *
 * Finite is not enough here. Every consumer assumes a unit quaternion (frame_qrot says so outright),
 * so a finite but large one — an uninitialized or un-normalized caller pose, components around 1e6 —
 * overflows the rotation math and poisons the render exactly as a NaN would. Normalizing once here
 * means no consumer has to. The 1e-6f floor and this whole body were copy-pasted at three sites (the
 * tracker pose, rt_set_listener, source orientation), and one of them was missing the normalize
 * entirely. Keep this the ONLY copy. */
static inline bool bwa_quat_unit(float* q) {
    if (!(isfinite(q[0]) && isfinite(q[1]) && isfinite(q[2]) && isfinite(q[3]))) return false;
    const float n = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (n > 1e-6f) {                       /* n cannot be NaN: every component is finite above */
        const float inv = 1.f / n;
        q[0] *= inv; q[1] *= inv; q[2] *= inv; q[3] *= inv;
    } else {
        q[0] = q[1] = q[2] = 0.f; q[3] = 1.f;
    }
    return true;
}

#endif /* BWA_SANE_H */
