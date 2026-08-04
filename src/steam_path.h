/*
 * steam_path.h — sound pathing (phonon side). Build-only-with-SDK.
 *
 * Routes a source's sound to the listener along INDIRECT paths that bend around occluders / through
 * openings, when the direct line is blocked. Borrows SteamScene's IPLContext + IPLScene, builds its
 * own PATHING simulator over a probe grid (with baked probe-to-probe visibility), and per source
 * publishes an IPLPathEffectParams: a 3-band EQ (the bending loss) + ambisonic shCoeffs (the
 * directions the indirect sound arrives from). rt.c renders that phonon-free (EQ + SH-encode into the
 * existing SH->26 bed decode), so the indirect sound decodes to the 26 speakers from the right
 * directions — e.g. out of an open doorway, not through the wall.
 *
 * Per-source like occlusion; the probe-transform convention (center in the translation column, radius
 * from the basis lengths) is the one learned wiring baked reflections.
 */
#ifndef BWA_STEAM_PATH_H
#define BWA_STEAM_PATH_H

#include "steam_scene.h"
#include "layout.h"
#include <stdint.h>

typedef struct SteamPath SteamPath;

/* Create the pathing sim over `scene`'s geometry: generate a probe grid spanning the layout (+margin),
 * bake the visibility graph, and start a sim thread. `order` is the pathing ambisonic order (1..3).
 * NULL on failure. Call at bwa_start, off the audio thread. */
SteamPath* steam_path_create(SteamScene* scene, RtCore* rt, const Layout* L,
                             uint32_t sample_rate, uint32_t block, uint32_t order);
void       steam_path_start(SteamPath* sp);    /* start the publishing sim thread (engine wires the tap first) */
void       steam_path_destroy(SteamPath* sp);

/* The rt path tap (matches RtPathTap): AUDIO thread, after the voice loop. ud == the SteamPath*.
 * Decodes the summed ambisonic indirect field to the 26-ch bus. */
void       steam_path_tap(void* ud, float* bus, uint32_t n, const float* lp, const float* lq, const float* ambi, uint32_t ambi_ch);

/* Register / move / enable a source for pathing (control thread; shadow-only and non-blocking — the
 * sim thread creates the IPLSource and commits, so these can never race RunPathing). `on`=0 mutes
 * it (publishes a zero path). The handle is the rt source handle, so rt.c can gate the published
 * params on its own generation. */
void       steam_path_set_source(SteamPath* sp, uint32_t handle, const float pos[3], int on);
void       steam_path_set_pos(SteamPath* sp, uint32_t handle, float x, float y, float z);  /* update an enabled source's pos */

/* ---- test seam (exercised by path_test without the sim thread): run one pathing pass for a given
 * listener and read a source's path params directly. eq[3] + sh[(order+1)^2]; returns 1 if a path
 * carries energy (sh[0] > 0), else 0. ---- */
int        steam_path_debug_run_get(SteamPath* sp, const float listener[3], uint32_t handle, float eq[3], float* sh);

#endif /* BWA_STEAM_PATH_H */
