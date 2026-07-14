/*
 * spcap.h — Speaker-Placement Correction Amplitude Panning: a smooth, all-speaker, power-conserving
 * sweet-spot panner for the FIXED-observer case (docs/spatialization.md). Same gain-vector interface
 * as dbap.c, so it drops in behind the bus seam as a selectable alternative (bwa_set_panner). The
 * placement correction (1/local-density) de-biases an uneven array so a source isn't pulled toward a
 * cluster of speakers. The cached state (speaker directions + per-speaker correction) is recomputed
 * only when the listener or layout changes, so the per-voice solve stays alloc/lock-free like DBAP.
 */
#ifndef BWA_SPCAP_H
#define BWA_SPCAP_H

#include "layout.h"

typedef struct {
    float    c[BWA_CHANNELS];          /* placement correction (1 / local density) per speaker */
    float    sdir[BWA_CHANNELS][3];    /* unit speaker directions from the cached listener */
    float    cached_lis[3];           /* listener the cache was built for */
    uint32_t cached_gen;              /* layout generation the cache was built for */
    int      valid;
} SpcapState;

/* Zero a fresh SpcapState (call once before first use; rt_create's calloc also suffices). The cache
 * thereafter self-invalidates when the listener moves or the layout generation `gen` changes. */
void spcap_reset(SpcapState* s);

/* Per-speaker target gains for a source at `src` heard by a fixed listener at `lis`, scaled by
 * `user_gain`. Direction-based + placement-corrected + constant-power, with the same source->listener
 * distance attenuation as DBAP. `gen` is the caller's layout generation — the cached placement
 * correction is rebuilt when it changes (or the listener moves). Writes L->count gains into out[]
 * (caller provides >= BWA_CHANNELS). SPCAP assumes a FIXED observer; with a moving (tracked) listener
 * the correction rebuilds every block — use DBAP for a moving observer. */
void spcap_gains(SpcapState* s, const float src[3], const float lis[3], const Layout* L,
                 uint32_t gen, float user_gain, float* out);

#endif /* BWA_SPCAP_H */
