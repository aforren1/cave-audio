/*
 * hole.h — hole-aware spread floor: how wide a source has to be BEFORE the array can render it.
 *
 * The CAVE array is a barrel, open at both poles (docs/spatialization.md, "Array holes"). A source
 * aimed into a pole has no speaker anywhere near it, so the panner's hull closes the hole with big
 * triangles of distant speakers: the rendered DIRECTION stays about right, but the energy is carried
 * by speakers up to 113 degrees apart. That is a split image, not a phantom.
 *
 * Imaginary pole speakers were tried and REJECTED (they made rE direction error worse; the table is
 * in spatialization.md). This is the other fix: a source with no speaker near it is genuinely not a
 * point, so stop asking the array to pretend. Floor the voice's effective SPREAD by how deep into
 * the hole it sits, and the existing spread machinery renders an honest wide source instead.
 *
 * The measurement is one angle: `gap`, from the source bearing to the NEAREST speaker bearing, both
 * seen from the live listener. The mapping to a spread floor is
 *
 *     floor = clamp( (gap - knee) / (pi/2 - knee), 0, 1 )
 *
 * with `knee` = the array's own mean nearest-neighbor speaker spacing (layout_mean_speaker_spacing,
 * the same geometry SPCAP derives its lobe width from). Both ends carry meaning:
 *   - Below the knee the floor is exactly 0. On a well-covered array NO direction is further than
 *     one inter-speaker spacing from a speaker AS SEEN FROM THE REFERENCE, so the floor is 0 there.
 *     Note the asymmetry: `knee` is measured from Layout.ref (it is an array property, and keeping
 *     the O(N^2) spacing measurement off the per-block path is why) while `gap` is measured from the
 *     LIVE listener. Angular gaps stretch as the listener leaves the middle, so a hole-free array can
 *     still derive a floor off-center: on the default 26 grid the worst gap runs 27.5 deg at center,
 *     39.7 deg at 0.7 m out and 61 deg at a corner, against a 37.5 deg knee. That is the feature
 *     working, not misfiring, because from a corner those bearings genuinely have no speaker near
 *     them. It is NOT "inert on a surrounding array" without qualification, and the docs used to say
 *     so. It only wakes up where coverage actually stops, for the listener who is actually there.
 *   - At gap == 90 degrees the floor is 1 (fully wide): no speaker within a hemisphere of the
 *     source means nothing about the render is point-like, and diffuse is the honest answer.
 *   - The slope self-scales. A sparse array has a wider knee, so less angular room between "covered"
 *     and "fully wide", so it widens faster - which is what a sparse array needs.
 *
 * Spread is normalized the way rt.c's solve_spread uses it (1 == a 90 degree half-angle), so the
 * floor reads directly as "the honest half-width of this image, as a fraction of a quarter-turn".
 *
 * Cost: the per-listener part (unit speaker directions + the knee) is CACHED and self-invalidates on
 * a listener move or a layout generation change, exactly like spcap.c/vbap.c/cap.c. The per-voice
 * part is `count` dot products plus one acosf. Alloc-free and lock-free: audio thread safe.
 */
#ifndef BWA_HOLE_H
#define BWA_HOLE_H

#include "layout.h"

typedef struct {
    float    sdir[BWA_CHANNELS][3];   /* unit speaker directions from the cached listener */
    float    knee;                    /* radians: gap below which the floor is exactly 0 */
    float    inv_span;                /* 1 / (pi/2 - knee) */
    float    cached_lis[3];
    uint32_t cached_gen;
    uint32_t count;
    int      valid;
} HoleState;

/* Zero a fresh HoleState (rt_create's calloc also suffices). The cache thereafter self-invalidates
 * when the listener moves or the layout generation changes. */
void hole_reset(HoleState* s);

/* Refresh the per-listener cache. Cheap and idempotent — call it before the per-voice queries; it
 * early-outs when nothing changed. `gen` is the caller's layout generation. The knee is re-derived
 * only on a generation change (it is a property of the ARRAY, measured from the layout's reference
 * point, so a walking listener never moves it); the directions rebuild on any listener move. */
void hole_block(HoleState* s, const Layout* L, const float lis[3], uint32_t gen);

/* The spread floor in [0, 1] for a UNIT source direction `u_s` (listener-relative, room space).
 * 0 when a speaker is within one inter-speaker spacing of the bearing. Alloc-free, O(count). */
float hole_floor(const HoleState* s, const float u_s[3]);

#endif /* BWA_HOLE_H */
