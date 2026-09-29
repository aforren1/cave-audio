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

#include "core/layout.h"
#include "core/rt.h"            /* RtDirectVoice (the mode-2 per-voice point taps) */
#include <stdint.h>

typedef struct SteamMonitor SteamMonitor;

/* Build the decoder for the 26-ch monitor. hrtf_path = NULL uses the built-in HRTF, else a SOFA
 * file. block_size is the phonon frameSize — phonon effects process EXACTLY this many samples per
 * apply, so it must equal the device block n passed to steam_monitor_process. max_voices > 0
 * additionally builds a fleet of per-voice IPLBinauralEffects (BWA_PROFILE_BINAURAL mode 2: one
 * true HRTF convolution per point voice, fed by rt_direct_voices); 0 skips the fleet (the sim
 * profiles).
 *
 * `parallax` picks where each virtual speaker is encoded FROM. 0 (BWA_PROFILE_BINAURAL): a fixed
 * direction from Layout.ref, computed once here; that bus carries synthesized-diffuse taps decoded
 * through virtual directions, and the listener's position never enters the decode. 1 (cave_sim and
 * cave_both, the array audition): the direction from the LIVE listener position p to each speaker,
 * re-encoded whenever p moved more than 1 cm and ramped across the block (invariant 4), so walking
 * past a speaker moves its image the way the real room would. At p == ref the two are the same
 * matrix. NULL on failure. */
SteamMonitor* steam_monitor_create(const Layout* L, uint32_t sample_rate, uint32_t block_size,
                                   const char* hrtf_path, uint32_t max_voices, int parallax);

/* True when the per-voice fleet exists (its creation is non-fatal: without it the engine keeps
 * rt in mode 1 and the direct render stays on the shared SH field). */
int steam_monitor_pervoice(const SteamMonitor* m);

/* Encode the planar 26-ch bus to ambisonics, sum the optional direct-binaural field `direct16`
 * (BWA_AMBI_CH planar channels of n, phonon monitor basis — rt_direct_ambi; NULL = none),
 * HRTF-decode to planar stereo out (L at out[0..n), R at out[n..2n)), then convolve each active
 * per-voice point tap (`voices`/`nvoices` from rt_direct_voices; NULL/0 = none) through its own
 * binaural effect and sum. Listener position p (read only in parallax mode), head orientation q
 * (xyzw, room frame — the per-voice room dirs are rotated into the head frame here). */
void steam_monitor_process(SteamMonitor* m, const float* bus26, const float* direct16,
                           const RtDirectVoice* voices, uint32_t nvoices,
                           const float p[3], const float q[4], float* out, uint32_t n);

void steam_monitor_destroy(SteamMonitor* m);

#endif /* BWA_STEAM_DECODE_H */
