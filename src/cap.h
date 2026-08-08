/*
 * cap.h — Compensated Amplitude Panning (Menzies & Fazi): correct the LF INTERAURAL component
 * instead of matching the whole velocity vector. NOT a panner — a projection applied ON TOP of the
 * selected panner's gain vector, so it inherits that panner's image and only fixes ITD.
 *
 * Below ~700 Hz the ear localizes by ITD, and ITD depends only on the interaural component of the
 * incident field: the rigid-sphere LF diffraction factor (~1.5a) multiplies every incident wave
 * equally, so it cancels out of the ratio. What is left is ONE scalar constraint on the gains,
 *
 *     rV . e  ==  u_s . e        rV = sum(g_k u_k)/sum(g_k), e = interaural axis, u_s = source dir
 *
 * where the plain dual-band low band instead renormalizes the point solve to an amplitude sum and
 * accepts whatever |rV| < 1 the geometry gives (rt.c, "dual-band low band"). Matching one scalar is
 * SATISFIABLE where matching a 3-vector is not, which is the whole point: any two speakers
 * straddling the target hit it exactly, so the rendered ITD equals a real source's at that bearing.
 *
 * Two consequences worth knowing:
 *   - e is fixed in the HEAD frame while the speakers are not, so re-solving per block against the
 *     current e IS the head-rotation compensation. A source stays put as the listener turns.
 *   - Facing the source (u_s . e == 0, a median-plane source) a symmetric seed already satisfies
 *     the constraint, so CAP is a NO-OP and reduces to whatever panner seeded it.
 *
 * Everything here is ROOM-frame on purpose. The SPCAP/VBAP direction caches key on listener
 * POSITION (panner_cache_stale), so rotating e into room space rather than rotating the speakers
 * into head space means a head that only TURNS invalidates no panner cache — the seated case costs
 * one 3-vector rotate plus `count` dot products per block, and nothing else.
 *
 * The array bounds what is achievable. rV is a CONVEX combination of speaker directions, so no
 * non-negative gain vector can render an ITD more lateral than the most lateral speaker the panner
 * lit: the target is clamped into [min ce, max ce] over that set. On the default grid, viewed from
 * the center, this bites only when the head turns to put the source within ~11 deg of the interaural
 * axis while the nearest speaker sits 15 deg off it — 2 of 24 yaw angles in the dsp_test sweep,
 * worst residual 0.017 against dual-band's 0.404. It is an array-density limit, not a CAP defect,
 * and CAP saturates at the bound rather than diverging.
 *
 * Deliberately NOT implemented: the near-field ILD arm (one first-order filter per image). That
 * needs per-speaker frequency-dependent gain, which would make this a render mode rather than a
 * gain-vector modifier and break the bus seam. The engine's near-field proximity shelf
 * (BWA_NF_RADIUS) and near-listener widening cover adjacent ground. See docs/spatialization.md.
 */
#ifndef BWA_CAP_H
#define BWA_CAP_H

#include "layout.h"

typedef struct {
    float    e[3];                  /* interaural axis (listener's right) in ROOM space */
    float    ce[BWA_CHANNELS];      /* (u_k . e) per speaker, u_k = unit dir from the listener */
    float    cached_lis[3];
    float    cached_q[4];
    uint32_t cached_gen;
    uint32_t count;
    int      valid;
} CapState;

/* Zero a fresh CapState (rt_create's calloc also suffices). The cache thereafter self-invalidates
 * when the listener moves, the head TURNS, or the layout generation changes. */
void cap_reset(CapState* s);

/* Refresh the per-block interaural cache for listener `lis` with head orientation `q` (xyzw, the
 * committed pose). Cheap and idempotent — call it before the per-voice solves; it early-outs when
 * nothing changed. `gen` is the caller's layout generation. */
void cap_block(CapState* s, const Layout* L, const float lis[3], const float q[4], uint32_t gen);

/* Project the LF band onto the correct-ITD hyperplane.
 *
 *   g0       the selected panner's power-normalized gains (v->gtarget), `s->count` entries
 *   u_s      UNIT source direction from the listener, room space
 *   strength 0..1 scaling of the correction. Callers pass (1 - spread): a point source gets the
 *            full ITD correction, an engulfing one gets none — forcing an exact ITD on a
 *            deliberately near-uniform gain vector would pull it back toward one side and undo
 *            the widening.
 *   out      receives the LF gains (v->gtarget_lo). MUST NOT alias g0: the degenerate-fallback arm
 *            re-reads g0 after the solve has already written out, so an aliased call would rescale
 *            the clamped values instead of the seed.
 *
 * Alloc-free, branch-light, O(count). Leaves g0 untouched. */
void cap_gains_lo(const CapState* s, const float* g0, const float u_s[3], float strength, float* out);

#endif /* BWA_CAP_H */
