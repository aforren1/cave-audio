/*
 * ambisonics.h — 3rd-order real spherical harmonics (ACN ordering, SN3D normalization).
 *
 * The front half of the *production* binaural monitor (docs/spatialization.md): treat each of
 * the 26 bus channels as a virtual speaker at its room direction from the listener, encode the
 * 26 feeds into a 16-channel 3rd-order ambisonic bus with these gains (a fixed matrix), then do
 * a single ambisonics→binaural HRTF decode via Steam Audio. This module is the SDK-independent,
 * unit-tested encode; the HRTF decode (stage 2) links Steam Audio (BWA_WITH_STEAMAUDIO) and
 * supersedes the first-cut per-channel pan in binaural.c.
 *
 * Convention: ACN channel order n = l(l+1)+m, SN3D normalization (the AmbiX standard). Ambisonic
 * axes are x=front, y=left, z=up — callers convert room space (x=right, y=up, z=back). At
 * integration this must match the linked phonon build's convention: if it uses N3D, scale ACN n
 * of degree l by sqrt(2l+1); if its axes differ, permute/sign the direction before encoding.
 */
#ifndef BWA_AMBISONICS_H
#define BWA_AMBISONICS_H

#define BWA_AMBI_ORDER 3
#define BWA_AMBI_CH    16    /* (order + 1)^2 */

/* Real SH gains for a unit direction `dir` (ambisonic axes), written to y[16] (ACN/SN3D). */
void ambi_encode_sn3d(const float dir[3], float y[BWA_AMBI_CH]);

/* Per-ACN-channel gain converting the SN3D encode above into the orthonormal real-SH basis Steam
 * Audio (phonon) consumes internally: its iplAmbisonicsDecodeEffect takes no normalization param,
 * and its SH evaluator is math-orthonormal (= N3D / sqrt(4pi)). Factor = sqrt(2l+1)/sqrt(4pi) for a
 * channel of degree l. Confirmed against phonon's hardcoded SH constants (third_party/steam-audio
 * core/src/core/sh/spherical_harmonics.cc: Y00=0.282095=1/sqrt(4pi), Y1=0.488603=sqrt(3)/sqrt(4pi),
 * ...). Multiply ambi_encode_sn3d's output by this elementwise to feed the decode; test_ambi
 * checks the product against those constants. */
extern const float ambi_phonon_scale[BWA_AMBI_CH];

#endif /* BWA_AMBISONICS_H */
