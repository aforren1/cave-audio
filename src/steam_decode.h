/*
 * steam_decode.h — production binaural monitor, stage 2: ambisonics → binaural HRTF decode
 * via Steam Audio. Phonon-free interface, so engine.c includes it unconditionally; the
 * implementation (steam_decode.c) is compiled ONLY when the SDK is vendored
 * (BWA_WITH_STEAMAUDIO), so the steam_monitor_* symbols exist only in that build — callers
 * gate the actual calls on BWA_HAVE_STEAMAUDIO.
 *
 * Pipeline: the 26-ch bus is encoded to 3rd-order ambisonics (ambisonics.c, fixed matrix from
 * the speaker directions) and decoded to stereo through Steam Audio's HRTF, with the head
 * orientation applied at the decode. Supersedes the first-cut per-channel pan in binaural.c.
 */
#ifndef BWA_STEAM_DECODE_H
#define BWA_STEAM_DECODE_H

#include "layout.h"
#include <stdint.h>

typedef struct SteamMonitor SteamMonitor;

/* Build the decoder for the 26-ch monitor. hrtf_path = NULL uses the built-in HRTF, else a SOFA
 * file. block_size is the phonon frameSize — phonon effects process EXACTLY this many samples per
 * apply, so it must equal the device block n passed to steam_monitor_process. NULL on failure. */
SteamMonitor* steam_monitor_create(const Layout* L, uint32_t sample_rate, uint32_t block_size,
                                   const char* hrtf_path);

/* Encode the planar 26-ch bus to ambisonics and HRTF-decode to planar stereo out (L at out[0..n),
 * R at out[n..2n)), with listener position p and head orientation q (xyzw, room frame). */
void steam_monitor_process(SteamMonitor* m, const float* bus26, const float p[3], const float q[4],
                           float* out, uint32_t n);

void steam_monitor_destroy(SteamMonitor* m);

#endif /* BWA_STEAM_DECODE_H */
