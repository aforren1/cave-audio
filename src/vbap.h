/*
 * vbap.h — Vector Base Amplitude Panning point panner: the 2-3 nearest speakers of the hull triangle
 * containing the source's bearing carry it (constant power), with the same distance attenuation as
 * DBAP. Sharpest localization of the three panners, but direction-based -> a FIXED-observer panner
 * (like SPCAP). The listener-relative triangulation is cached and rebuilt only on listener/layout
 * change, so the per-voice solve is alloc/lock-free. Falls back to DBAP for a non-triangulable array
 * or a source at the listener. See docs/spatialization.md.
 */
#ifndef BWA_VBAP_H
#define BWA_VBAP_H

#include "layout.h"

#define VBAP_MAXTRI 256    /* hull-triangle cap (a generic 26-speaker hull is ~48; overflow -> DBAP fallback) */

typedef struct {
    float    sdir[BWA_CHANNELS][3];    /* listener-relative unit speaker directions */
    int      tri[VBAP_MAXTRI][3];     /* hull triangles of sdir */
    float    det[VBAP_MAXTRI];
    int      ntri;                    /* 0 if the array can't be triangulated within the cap -> DBAP fallback */
    float    cached_lis[3];
    uint32_t cached_gen;
    int      valid;
} VbapState;

/* Zero a fresh VbapState (rt_create's calloc also suffices). Cache self-invalidates on listener move
 * or a layout-generation change. */
void vbap_reset(VbapState* s);

/* Per-speaker target gains for a source at `src` heard by a fixed listener at `lis`, scaled by
 * `user_gain`. VBAP onto the containing hull triangle (constant power) + DBAP-style distance
 * attenuation. `gen` is the caller's layout generation. Writes L->count gains into out[]. */
void vbap_gains(VbapState* s, const float src[3], const float lis[3], const Layout* L,
                uint32_t gen, float user_gain, float* out);

#endif /* BWA_VBAP_H */
