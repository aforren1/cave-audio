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

#include "layout.h"        /* Layout + BWA_CHANNELS for ambi_sad_decode */

#include <math.h>

#define BWA_AMBI_ORDER 3
#define BWA_AMBI_CH    16    /* (order + 1)^2 */

/* room axes (+z fwd, +y up, -x right) -> ambisonic axes (x=front, y=left, z=up): the (z,x,y)
 * permutation, defined once as an index table so forward and inverse cannot diverge. Gather:
 * a[i] = d[BWA_ROOM2AMBI[i]]; the inverse scatters back. `d` must already be a unit room direction.
 * Consumers: the bed/AllRAD/EPAD/FDN decode builds (forward), the parametric bed's DOA unpack
 * (inverse), and bed_rot_ambi's rotation conjugation (rt.c, raw table). NOTE: the monitor paths
 * (ambi_encode_phonon below) use a DELIBERATELY different (phonon net-AmbiX, negated: front=-z,
 * left=-x) convention — do NOT route them through here; merging mirrors the HRTF field. */
static const int BWA_ROOM2AMBI[3] = { 2, 0, 1 };
static inline void room_to_ambi(const float d[3], float a[3]) {
    for (int i = 0; i < 3; ++i) a[i] = d[BWA_ROOM2AMBI[i]];
}
static inline void ambi_to_room(const float a[3], float d[3]) {
    for (int i = 0; i < 3; ++i) d[BWA_ROOM2AMBI[i]] = a[i];
}

/* i-th of M points on a Fibonacci sphere (golden-angle spiral), room axes, FLOAT precision. Shared by
 * allrad.c (virtual layer) and fdn.c (line directions). zylia.c uses a DOUBLE variant on purpose (it
 * feeds a Gauss-Newton solver pinned to sub-degree accuracy) — keep that one; this must stay float so
 * the AllRAD build stays byte-identical (xval pins it). */
static inline void fib_sphere_dir(int i, int M, float d[3]) {
    float y = 1.f - 2.f * ((float)i + 0.5f) / (float)M;
    float r = sqrtf(fmaxf(0.f, 1.f - y * y)), th = (float)i * 2.39996323f;
    d[0] = r * cosf(th); d[1] = y; d[2] = r * sinf(th);
}

/* Real SH gains for a unit direction `dir` (ambisonic axes), written to y[16] (ACN/SN3D). */
void ambi_encode_sn3d(const float dir[3], float y[BWA_AMBI_CH]);

/* Sampling (projection) ambisonic decode over a layout: each speaker's direction from L->ref is
 * SH-sampled (room->ambi via room_to_ambi) and scaled by (2l+1)/count. This is the degenerate-array
 * fallback both the bed decode (rt.c) and the FDN line render (fdn.c) share — assumes a roughly
 * uniform speaker distribution; AllRAD/EPAD supersede it on irregular arrays. Writes `count` rows. */
void ambi_sad_decode(const Layout* L, uint32_t count, float dec[BWA_CHANNELS][BWA_AMBI_CH]);

/* max-rE decode weights (Zotter & Frank 2012) for content of `order` (1..3): per-ACN-channel gains
 * w[k] = gamma * P_l(r), where r is the largest zero of P_{order+1} and gamma renormalizes so a
 * DIFFUSE field keeps its energy (gamma^2 * sum (2l+1) P_l(r)^2 = sum (2l+1)). Scaling the SH signal
 * by these before a decode D is the max-rE decode D*diag(w): it tapers the high orders, which
 * suppresses the decode's sidelobes and lengthens the energy vector — better localization AWAY from
 * the sweet spot, at a slightly wider main lobe. Channels beyond the order are written as 1. */
void ambi_max_re_weights(int order, float w[BWA_AMBI_CH]);

/* Full 3-axis SH rotation (Ivanic & Ruedenberg 1996, the standard real-SH recursion): build the
 * packed block-diagonal matrix M (l = 1..3 blocks: 3x3 + 5x5 + 7x7 = 83 floats, row-major, centered
 * indices; l = 0 is always identity) for the 3x3 direction rotation R (ambi axes, field convention:
 * applying M to an encoded plane wave from d yields the encode of R*d). Normalization-agnostic:
 * rotation mixes only within one degree l, and SN3D/N3D differ by a per-l scale. */
/* MONITOR-basis encode: room direction -> the orthonormal real SH phonon decodes, in the phonon
 * net-AmbiX axis convention (front=-z_room, left=-x_room, up=+y_room). This is the DELIBERATELY
 * different convention the NOTE above warns about, as one shared function: steam_decode.c's
 * virtual-speaker matrix and rt.c's direct-binaural bus (the BWA_PROFILE_BINAURAL per-voice encode)
 * both use it, so their contributions sum in one consistent field. binaural.c's no-SDK fallback
 * decodes the same basis. Do NOT route through room_to_ambi/ambi_encode_sn3d directly for monitor
 * content — a convention mismatch mirrors the HRTF field (see steam_decode.c CONVENTION 2b). */
void ambi_encode_phonon(const float room_dir[3], float y[BWA_AMBI_CH]);
/* Per-channel diagonal taking an engine-canonical ACN/SN3D FIELD into that same monitor basis
 * ((-1)^|m| axis flip x the orthonormal rescale) — the beds' SH->SH pass in the direct-binaural
 * render. See ambisonics.c for the derivation. */
extern const float ambi_canon_to_phonon[BWA_AMBI_CH];

#define BWA_SH_ROT_N 83
void ambi_rot_matrix(const float R[3][3], float M[BWA_SH_ROT_N]);
/* Apply M to an ACN SH vector of nch channels (4/9/16; blocks past nch untouched). out != sh. */
void ambi_rot_apply(const float M[BWA_SH_ROT_N], const float* sh, int nch, float* out);

/* Per-ACN-channel gain converting the SN3D encode above into the orthonormal real-SH basis Steam
 * Audio (phonon) consumes internally: its iplAmbisonicsDecodeEffect takes no normalization param,
 * and its SH evaluator is math-orthonormal (= N3D / sqrt(4pi)). Factor = sqrt(2l+1)/sqrt(4pi) for a
 * channel of degree l. Confirmed against phonon's hardcoded SH constants (third_party/steam-audio
 * core/src/core/sh/spherical_harmonics.cc: Y00=0.282095=1/sqrt(4pi), Y1=0.488603=sqrt(3)/sqrt(4pi),
 * ...). Multiply ambi_encode_sn3d's output by this elementwise to feed the decode; test_ambi
 * checks the product against those constants. */
extern const float ambi_phonon_scale[BWA_AMBI_CH];

#endif /* BWA_AMBISONICS_H */
