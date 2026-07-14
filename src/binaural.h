/*
 * binaural.h — the binaural debug monitor: a bus->stereo transform that auditions the
 * actual 26-ch array render on headphones (docs/architecture.md, docs/spatialization.md).
 * It treats each bus channel as a virtual speaker at its surveyed direction relative to the
 * listener, rotated with the head, and decodes to stereo.
 *
 * M5 decode is a head-oriented constant-power L/R pan — equivalent to a 1st-order ambisonic
 * (W/X) encode + two opposed cardioid decoders: cheap, real, and responsive to source
 * position + head orientation. The production path is a higher-order ambisonic encode ->
 * single ambisonics->binaural HRTF decode via Steam Audio (one decode, not 26 convolutions);
 * that slots in here behind the same interface when the SDK is vendored (see roadmap M5).
 */
#ifndef BWA_BINAURAL_H
#define BWA_BINAURAL_H

#include "layout.h"        /* Layout, BWA_CHANNELS */

#include <stdint.h>

typedef struct Monitor Monitor;

Monitor* monitor_create(const Layout* L, uint32_t sample_rate);   /* control thread; allocates */
void     monitor_destroy(Monitor* m);

/* Decode one block of the planar `bus` (channels * nframes) to stereo for a listener at `p`
 * with head orientation quaternion `q` (xyzw). `out` is planar 2-ch: out[i]=L, out[nframes+i]=R.
 * Audio thread; no alloc/lock (recomputes the per-channel pan only when the pose changes). */
void monitor_process(Monitor* m, const float* bus, const float p[3], const float q[4],
                     float* out_stereo, uint32_t nframes);

#endif /* BWA_BINAURAL_H */
