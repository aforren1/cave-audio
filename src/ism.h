/*
 * ism.h — image-source early reflections for a shoebox room: PURE geometry, phonon-free.
 *
 * The complement to fdn.c. The FDN renders the late, diffuse tail; this renders the FIRST-ORDER
 * SPECULAR reflections — the six wall bounces that actually carry room size and source distance.
 * Together they are the classic hybrid (early + late), and with both the engine has a complete
 * acoustics path that needs NO Steam Audio SDK.
 *
 * Order 1 only, by design: each face mirrors the source once, giving 6 images. Higher orders blend
 * into the diffuse field within a few tens of ms, which is exactly what the FDN already does — so
 * they would cost per-voice DSP to reproduce something the tail renders for free.
 *
 * The images are LISTENER-INDEPENDENT (they depend only on the source and the room), which is what
 * lets rt.c render each one as a plain point source through the engine's own listener-relative
 * panner: the reflections then have correct direction AND parallax as the listener walks — the
 * property a shared listener-centric reverb bed (Steam's, or the FDN's) cannot give.
 */
#ifndef BWA_ISM_H
#define BWA_ISM_H

#define ISM_FACES  6      /* -x, +x, -y, +y, -z, +z (the bwa_scene_set_box face order) */
#define ISM_IMAGES 6      /* one first-order image per face */

/* The shoebox, in room space. Floor-based like bwa_scene_set_box: x/z centred on the origin, y from
 * 0 (the floor) to `h`. absorb[f][b] is face f's energy absorption in band b (low/mid/high, 0..1);
 * the pressure reflection coefficient is sqrt(1 - absorb).
 *
 * A face marked press[f] is a PRESSURE-RELEASE boundary (reflection into a much softer medium —
 * the underside of a water surface is the canonical case): its reflection coefficient is ~ -1, so
 * the face NEGATES the coefficient. The inverted image interferes destructively with the direct
 * sound near the boundary — the Lloyd's-mirror comb that makes a near-surface source sound thin.
 *
 * plane_only drops the box for a single horizontal mirror at y = ground_y (bwa_scene_set_ground) —
 * the outdoor case, where the ground bounce is the one early reflection that exists. The plane uses
 * face slot 2 (-y) for its absorption + press flag; w/h/d are unused and there is no inside test. */
typedef struct {
    float w, h, d;
    float absorb[ISM_FACES][3];
    unsigned char press[ISM_FACES]; /* pressure-release face: negate the reflection (polarity flip) */
    int   plane_only;               /* 1 = single mirror plane at ground_y (face slot 2), no box */
    float ground_y;                 /* plane height (room y; plane_only) */
    int   valid;                    /* 0 until a room is set (bwa_scene_set_box / _set_ground) */
} IsmRoom;

/* One first-order image: the mirrored source position + that face's per-band reflection coefficient
 * (pressure, 0..1). The path length (hence delay and distance attenuation) is |pos - listener|,
 * which rt.c derives at render time — that is what makes the reflection move with the listener. */
typedef struct {
    float pos[3];
    float refl[3];                  /* per-band pressure reflection coefficient = sqrt(1 - absorb) */
} IsmImage;

/* Mirror `src` across each of the room's six faces — or, in plane_only mode, across the one ground
 * plane. Writes up to ISM_IMAGES images; returns the count (0 if the room is invalid or the source
 * is outside the box — an outside source has no valid mirror geometry, so the caller renders it
 * dry). The plane mirrors a source on EITHER side (the geometry is symmetric); a listener on the
 * far side of the plane from the source hears an image that has no physical path — that cross-
 * medium case is the client's to gate (disable ISM on sources across the boundary; they are
 * heavily occluded anyway). Pure: no allocation, safe on the audio thread. */
int ism_images(const IsmRoom* r, const float src[3], IsmImage* out);

#endif /* BWA_ISM_H */
