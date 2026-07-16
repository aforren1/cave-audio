/*
 * epad.h — Energy-Preserving Ambisonic Decoding (Zotter, Pomberger & Noisternig 2012, Acta
 * Acustica 98) for the diffuse layer. Same [BWA_CHANNELS][BWA_AMBI_CH] shape + SN3D/ACN
 * convention as the sampling/AllRAD decodes: the decode is the POLAR FACTOR of the transposed
 * encode matrix, D = c·Yᵀ(YYᵀ)^(-1/2), which makes the decoded ENERGY of a panned plane wave
 * constant over direction on an IRREGULAR array — the sampling decode over-energises dense
 * speaker regions, and mode-matching (pinv) blows up on lopsided ones. Rank-deficient
 * directions (a coplanar survey's missing axis) truncate out of the inverse square root, so
 * unreproducible field components are dropped rather than amplified.
 *
 * A 16×16 symmetric eigensolve (cyclic Jacobi, double) — a LOAD-TIME control-thread build;
 * the audio thread just applies the matrix (mix_bed, unchanged). Selected with
 * bwa_desc.bed_decoder = BWA_DECODE_EPAD. See docs/spatialization.md.
 */
#ifndef BWA_EPAD_H
#define BWA_EPAD_H

#include "layout.h"
#include "ambisonics.h"   /* BWA_AMBI_CH */

/* Build the EPAD decode into `decode` (row s = speaker s, BWA_AMBI_CH SN3D/ACN coefficients),
 * energy-normalised to the sampling decode's diffuse level (level-fair decoder swap, as
 * allrad.c). Returns 1 on success, 0 on a degenerate array (caller keeps the sampling decode). */
int epad_build_decode(const Layout* L, float decode[BWA_CHANNELS][BWA_AMBI_CH]);

#endif /* BWA_EPAD_H */
