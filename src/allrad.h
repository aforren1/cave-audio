/*
 * allrad.h — All-Round Ambisonic Decoding for the diffuse layer. Builds the SH->26 bed-decode matrix
 * (same [BWA_CHANNELS][BWA_AMBI_CH] shape + SN3D/ACN convention as the sampling decode in rt.c), but
 * robust on an IRREGULAR array: sampling-decode the ambisonic signal to a dense uniform VIRTUAL layout
 * (a Fibonacci sphere), then VBAP each virtual loudspeaker onto the real array (convex-hull
 * triangulation), then energy-normalise to the sampling decode. On a clustered/lopsided array this
 * keeps the diffuse field even (no loud directions) and improves localization, where the plain
 * sampling decode over-energises dense regions. A pole with no real speaker within ~60° (a
 * floor-less array's nadir) gets an IMAGINARY speaker whose decode share is discarded, so diffuse
 * energy aimed into the hole is dropped rather than smeared onto the bottom ring (IEM AllRADecoder
 * practice). See docs/spatialization.md.
 *
 * Heavy (convex hull + VBAP over a few hundred virtual directions) — a LOAD-TIME control-thread build;
 * the audio thread still just applies the resulting matrix (mix_bed), unchanged.
 */
#ifndef BWA_ALLRAD_H
#define BWA_ALLRAD_H

#include "layout.h"
#include "ambisonics.h"   /* BWA_AMBI_CH */

/* Build the AllRAD decode into `decode` (row s = speaker s, BWA_AMBI_CH SN3D/ACN coefficients).
 * Returns 1 on success, 0 if the array can't be triangulated (caller should keep the sampling decode). */
int allrad_build_decode(const Layout* L, float decode[BWA_CHANNELS][BWA_AMBI_CH]);

#endif /* BWA_ALLRAD_H */
