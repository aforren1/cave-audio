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
#ifndef BW_ISM_H
#define BW_ISM_H

#define ISM_FACES  6      /* -x, +x, -y, +y, -z, +z (the bw_scene_set_box face order) */
#define ISM_IMAGES 6      /* one first-order image per face */

/* The shoebox, in room space. Floor-based like bw_scene_set_box: x/z centred on the origin, y from
 * 0 (the floor) to `h`. absorb[f][b] is face f's energy absorption in band b (low/mid/high, 0..1);
 * the pressure reflection coefficient is sqrt(1 - absorb). */
typedef struct {
    float w, h, d;
    float absorb[ISM_FACES][3];
    int   valid;                    /* 0 until a room is set (bw_scene_set_box) */
} IsmRoom;

/* One first-order image: the mirrored source position + that face's per-band reflection coefficient
 * (pressure, 0..1). The path length (hence delay and distance attenuation) is |pos - listener|,
 * which rt.c derives at render time — that is what makes the reflection move with the listener. */
typedef struct {
    float pos[3];
    float refl[3];                  /* per-band pressure reflection coefficient = sqrt(1 - absorb) */
} IsmImage;

/* Mirror `src` across each of the room's six faces. Writes ISM_IMAGES images; returns the count
 * (0 if the room is invalid or the source is outside it — an outside source has no valid mirror
 * geometry, so the caller renders it dry). Pure: no allocation, safe on the audio thread. */
int ism_images(const IsmRoom* r, const float src[3], IsmImage* out);

#endif /* BW_ISM_H */
