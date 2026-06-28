/*
 * ambisonics.h — 3rd-order real spherical harmonics (ACN ordering, SN3D normalization).
 *
 * The front half of the *production* binaural monitor (docs/spatialization.md): treat each of
 * the 26 bus channels as a virtual speaker at its room direction from the listener, encode the
 * 26 feeds into a 16-channel 3rd-order ambisonic bus with these gains (a fixed matrix), then do
 * a single ambisonics→binaural HRTF decode via Steam Audio. This module is the SDK-independent,
 * unit-tested encode; the HRTF decode (stage 2) links Steam Audio (BWAUDIO_WITH_STEAMAUDIO) and
 * supersedes the first-cut per-channel pan in binaural.c.
 *
 * Convention: ACN channel order n = l(l+1)+m, SN3D normalization (the AmbiX standard). Ambisonic
 * axes are x=front, y=left, z=up — callers convert room space (x=right, y=up, z=back). At
 * integration this must match the linked phonon build's convention: if it uses N3D, scale ACN n
 * of degree l by sqrt(2l+1); if its axes differ, permute/sign the direction before encoding.
 */
#ifndef BW_AMBISONICS_H
#define BW_AMBISONICS_H

#define BW_AMBI_ORDER 3
#define BW_AMBI_CH    16    /* (order + 1)^2 */

/* Real SH gains for a unit direction `dir` (ambisonic axes), written to y[16] (ACN/SN3D). */
void ambi_encode_sn3d(const float dir[3], float y[BW_AMBI_CH]);

#endif /* BW_AMBISONICS_H */
