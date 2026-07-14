/*
 * fdn.h — directional FDN reverb bed: a phonon-free late-reverb renderer that registers at the SAME
 * rt bus-tap seam as the Steam Audio reflection bed (input = the mono reflection aux send; voices
 * opt in via bwa_source_set_reflections, with the same per-voice send levels). A 16-line feedback
 * delay network (Householder feedback — a lossless prototype) with per-line 2-band decay filters;
 * every line is assigned a direction on the sphere and rendered as a plane wave through the
 * layout's SH→26 bed decode, and the per-line decay can be SCALED by direction — anisotropic decay,
 * the diagonal direction-domain special case of the Directional FDN (Alary/Politis/Schlecht,
 * JAES 2019). The decay is a DESIGN parameter, not a model of the real room (matching would
 * double-count — docs/calibration.md).
 *
 * fdn_create/fdn_set_* run on the control thread BEFORE the audio thread starts (bwa_start stages
 * them); fdn_tap runs on the audio thread and never allocates or locks.
 */
#ifndef BWA_FDN_H
#define BWA_FDN_H

#include "layout.h"

#include <stdint.h>

typedef struct Fdn Fdn;

/* Build the FDN for this layout + rate: line delays, decay filters at the defaults (1.2 s low /
 * 0.7 s high @ 2 kHz), and the line→26 render matrix (AllRAD bed decode over the real array; the
 * sampling decode as fallback for a non-triangulable one). NULL on allocation failure. */
Fdn* fdn_create(const Layout* L, uint32_t sample_rate, uint32_t channels);
void fdn_destroy(Fdn* f);

/* Decay time (s) below/above `xover_hz`. Values clamp to [0.05, 30]. Before the audio thread runs. */
void fdn_set_decay(Fdn* f, float rt60_low_s, float rt60_high_s, float xover_hz);

/* Anisotropic decay: scale the decay time toward `dir` (room axes, need not be unit) by `factor`
 * ([0.25, 4]; 1 = uniform; < 1 = the field dies FASTER toward dir — an open or treated side). A
 * smooth first-order pattern over the sphere, applied per delay line. Before the audio thread runs. */
void fdn_set_decay_direction(Fdn* f, const float dir[3], float factor);

/* Reverb return level (linear, default 1). Safe live: the tap ramps toward it per block. */
void fdn_set_gain(Fdn* f, float gain);

/* RtBusTap: process the aux send through the FDN and add the decoded 26-ch reverb onto the bus
 * (pre-align, like the Steam bed). `ud` is the Fdn. Audio thread. */
void fdn_tap(void* ud, float* bus, uint32_t n, const float* lp, const float* lq, const float* aux);

#endif /* BWA_FDN_H */
