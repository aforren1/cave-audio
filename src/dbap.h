/*
 * dbap.h — listener-relative Distance-Based Amplitude Panning gain solve. Pure + audio-
 * thread safe (no alloc/lock). See docs/spatialization.md for the rationale.
 */
#ifndef BW_DBAP_H
#define BW_DBAP_H

#include "layout.h"

/* Per-speaker target gains for a source at `src` heard by a listener at `lis`, scaled by
 * `user_gain`. Constant-power, blurred by layout->rolloff_r, weighted toward the source's
 * bearing from the listener, and attenuated by the source->listener distance. Writes
 * L->count gains into out[] (caller provides >= BW_CHANNELS floats). */
void dbap_gains(const float src[3], const float lis[3], const Layout* L,
                float user_gain, float* out);

#endif /* BW_DBAP_H */
