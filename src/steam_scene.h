/*
 * steam_scene.h — materials occlusion, off-thread half (M-later, step 2). Phonon-free interface so
 * engine.c includes it unconditionally; the implementation (steam_scene.c) links phonon and is
 * compiled only with BWAUDIO_WITH_STEAMAUDIO.
 *
 * Owns an IPLScene (geometry + materials) + an IPLSimulator on a dedicated thread (NOT the bw_*
 * control thread, NOT the audio thread). The control thread feeds it geometry + per-source
 * occlusion enable/position through a small locked shadow; the sim thread ray-traces occlusion at
 * a low rate and publishes each source's transmittance into the RT mixer via rt_set_occlusion()
 * (a single aligned float the audio thread ramps toward). All phonon objects are owned by the sim
 * thread; the control thread never touches them.
 */
#ifndef BW_STEAM_SCENE_H
#define BW_STEAM_SCENE_H

#include "rt.h"
#include <stdint.h>

typedef struct SteamScene SteamScene;

/* Create the scene + simulator + sim thread. rt is used for the listener pose (rt_read_pose) and
 * to publish occlusion (rt_set_occlusion). voice_cap bounds the per-source records. NULL on error. */
SteamScene* steam_scene_create(RtCore* rt, uint32_t sample_rate, uint32_t frame_size, uint32_t voice_cap);

/* Set the (single-material) occluding geometry, in room space (RH, metres). verts is nverts*3
 * floats; tris is ntris*3 vertex indices (CCW winding). Replaces any previous mesh. */
void steam_scene_set_mesh(SteamScene* s, const float* verts, int nverts, const int* tris, int ntris,
                          const float absorption[3], float scattering, const float transmission[3]);

/* Per-source controls (by bw source handle). Occlusion + directivity are independent features (a
 * source can be directional without being occluded). directivity: weight 0=omni / .5=cardioid /
 * 1=fig-8, power>=1 sharpens; the dipole axis is the source forward (fwd). source_gone clears all
 * features on destroy so the sim tears the IPLSource down and a recycled slot starts clean. */
void steam_scene_set_occlusion  (SteamScene* s, uint32_t handle, int on);
void steam_scene_set_directivity(SteamScene* s, uint32_t handle, float weight, float power);
void steam_scene_set_orientation(SteamScene* s, uint32_t handle, float fx, float fy, float fz);
void steam_scene_set_pos        (SteamScene* s, uint32_t handle, float x, float y, float z);
void steam_scene_source_gone    (SteamScene* s, uint32_t handle);

void steam_scene_destroy(SteamScene* s);   /* stops the sim thread, releases phonon objects */

#endif /* BW_STEAM_SCENE_H */
