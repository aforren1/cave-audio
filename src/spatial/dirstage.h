/*
 * dirstage.h: a per-channel slewed broadband gain plus RBJ high shelf, in place on a planar bus.
 * The DSP half of align.c's tracked directivity COMPENSATION (layout.h Directivity): relative,
 * loss(ref) - loss(listener), clamped to +/-6 dB broadband and +/-10 dB treble; rt.c's
 * directivity_track computes the targets. It is a two-band approximation of the model: a broadband
 * gain carrying the low half-band loss and a high shelf at the model's split carrying the difference
 * to the high one. The array-sim audition deliberately does NOT use it: it simulates the room's loss
 * at the model's full resolution (binaural/arraysim.h), so the comp's two-band residual stays
 * audible on headphones instead of cancelling against a matching approximation.
 *
 * The state is a plain struct the CALLER owns (a member of Aligner): init on the control thread,
 * everything else on the audio thread, no allocation anywhere. While every channel sits at identity
 * the stage does nothing to the samples, so a caller that never leaves identity is bit-identical to
 * a caller without the stage.
 */
#ifndef BWA_DIRSTAGE_H
#define BWA_DIRSTAGE_H

#include "core/layout.h"        /* BWA_CHANNELS */

#include <stdint.h>

typedef struct {
    uint32_t channels;
    int      have;                                    /* init succeeded: the calls below do something */
    int      live;                                    /* latched while any channel is off identity */
    float    cw0, alpha;                              /* shelf prototype at the split (Q 0.7071, S = 1) */
    float    slew;                                    /* max dB change per sample */
    float    gcur[BWA_CHANNELS], gtgt[BWA_CHANNELS];  /* broadband, dB */
    float    scur[BWA_CHANNELS], stgt[BWA_CHANNELS];  /* shelf, dB */
    float    sco[BWA_CHANNELS];                       /* the shelf gain co[] was built for */
    float    co[BWA_CHANNELS][5];
    float    x1[BWA_CHANNELS], x2[BWA_CHANNELS], y1[BWA_CHANNELS], y2[BWA_CHANNELS];
} DirStage;

/* Zero the state and precompute the shelf prototype at `split_hz` (pulled under 0.45 x the rate, so
 * a split at or past Nyquist keeps the broadband half rather than dropping the stage). `slew_db_s`
 * is the glide rate of both gains. Returns s->have: 0 for a non-positive split, a zero rate or a
 * channel count past BWA_CHANNELS, after which every other call is a no-op. Control thread. */
int  dirstage_init(DirStage* s, uint32_t channels, float split_hz, uint32_t sample_rate, float slew_db_s);

/* Set the per-channel targets (dB; NULL = 0 for that half). Each is NaN-cleansed to 0 and clamped
 * to +/-gain_max_db (broadband) and +/-shelf_max_db (shelf): a poisoned target would otherwise NaN
 * the coefficients and the ramp for the rest of the session. Latches `live` when any target is off
 * identity; identity itself is reached by gliding back. Audio thread (or before it starts). */
void dirstage_targets(DirStage* s, const float* gain_db, const float* shelf_db,
                      float gain_max_db, float shelf_max_db);

/* Readback of the current (slewed) gains; either pointer may be NULL. */
void dirstage_state(const DirStage* s, float* gain_db, float* shelf_db);

/* Apply one block in place: `bus` is planar, channel stride n. Per channel, a linear gain ramp from
 * the block's start gain to its end gain, then the shelf (coefficients rebuilt only when its gain
 * moved). Clears `live` once every channel has landed exactly on identity. Audio thread. */
void dirstage_process(DirStage* s, float* bus, uint32_t n);

#endif /* BWA_DIRSTAGE_H */
