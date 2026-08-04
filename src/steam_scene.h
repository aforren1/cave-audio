/*
 * steam_scene.h — materials occlusion, off-thread half (M-later, step 2). Phonon-free interface so
 * engine.c includes it unconditionally; the implementation (steam_scene.c) links phonon and is
 * compiled only with BWA_WITH_STEAMAUDIO.
 *
 * Owns an IPLScene (geometry + materials) + an IPLSimulator on a dedicated thread (NOT the bwa_*
 * control thread, NOT the audio thread). The control thread feeds it geometry + per-source
 * occlusion enable/position through a small locked shadow; the sim thread ray-traces occlusion at
 * a low rate and publishes each source's transmittance into the RT mixer via rt_set_occlusion()
 * (a single aligned float the audio thread ramps toward). All phonon objects are owned by the sim
 * thread; the control thread never touches them.
 */
#ifndef BWA_STEAM_SCENE_H
#define BWA_STEAM_SCENE_H

#include "rt.h"
#include <stdint.h>

typedef struct SteamScene SteamScene;

/* Create the scene + simulator + sim thread. rt is used for the listener pose (rt_read_pose) and
 * to publish occlusion (rt_set_occlusion). voice_cap bounds the per-source records. NULL on error. */
SteamScene* steam_scene_create(RtCore* rt, uint32_t sample_rate, uint32_t frame_size, uint32_t voice_cap,
                               int use_embree /* try Embree ray tracing; falls back to default */);

/* Set the occluding geometry with PER-TRIANGLE materials, in room space (RH, meters). verts is
 * nverts*3 floats; tris is ntris*3 vertex indices (CCW winding). nmat materials are given as flat
 * arrays: absorption[nmat*3], scattering[nmat], transmission[nmat*3] (each band/coeff 0..1).
 * tri_material is ntris entries, each an index in [0,nmat) (out-of-range clamps to 0; NULL = all
 * material 0). Replaces any prior mesh. */
void steam_scene_set_mesh_mat(SteamScene* s, const float* verts, int nverts, const int* tris, int ntris,
                              int nmat, const float* absorption, const float* scattering,
                              const float* transmission, const int* tri_material);

/* Drive any STAGED static mesh through to a COMMITTED scene before returning (set_mesh_mat only
 * stages; the sim thread commits within ~one 33 ms tick). Call before create-time ray tracing —
 * probe generation, the reflection/path bakes — so they trace the room that was just set instead
 * of racing the deferred commit (or tracing an empty scene). Control thread; may block ~one sim
 * tick; safe alongside the running sim thread. Dynamic (instanced) movers are not flushed. */
void steam_scene_flush(SteamScene* s);

/* Per-source controls (by bw source handle). Occlusion + directivity are independent features (a
 * source can be directional without being occluded). directivity: weight 0=omni / .5=cardioid /
 * 1=fig-8, power>=1 sharpens; the dipole axis is the source forward (fwd). source_gone clears all
 * features on destroy so the sim tears the IPLSource down and a recycled slot starts clean. */
void steam_scene_set_occlusion  (SteamScene* s, uint32_t handle, int on);
void steam_scene_set_directivity(SteamScene* s, uint32_t handle, float weight, float power);
void steam_scene_set_orientation(SteamScene* s, uint32_t handle, float fx, float fy, float fz);
void steam_scene_set_pos        (SteamScene* s, uint32_t handle, float x, float y, float z);
void steam_scene_source_gone    (SteamScene* s, uint32_t handle);

/* Dynamic (movable) occluders/reflectors. The static mesh above is committed once and its BVH built
 * once; a dynamic mesh is a rigid INSTANCE of its own sub-scene, so moving it is a cheap top-level BVH
 * refit, not a geometry rebuild (Steam Audio's IPLInstancedMesh — the acoustic analogue of a physics
 * collider with a transform). Both the occlusion sim and the borrowing reflection/pathing sims see the
 * change on their next tick. Geometry is in the mover's LOCAL space (meters); place it with the rigid
 * transform. add returns a handle >= 0 (a slot index), or -1 on bad geometry / a full table.
 *
 * The mover carries a single material (absorption[3] / scattering / transmission[3], each 0..1). All
 * three calls are control-thread, per-frame-safe (the sim thread owns every phonon object; the control
 * thread only writes a locked shadow). BAKED reflections/pathing do NOT track these — the bake froze
 * the geometry (docs/materials.md); real-time reflections and occlusion do. */
int  steam_scene_add_dynamic_mesh(SteamScene* s, const float* verts, int nverts, const int* tris, int ntris,
                                  const float absorption[3], float scattering, const float transmission[3]);
/* Move/rotate/scale a dynamic mesh. m16 is a row-major 4x4 local-to-room affine (phonon IPLMatrix4x4
 * order); the engine builds it from a rigid pos+quat. No-op for an unknown/removed handle. */
void steam_scene_set_dynamic_transform(SteamScene* s, int handle, const float m16[16]);
/* Remove a dynamic mesh (the sim tears down its instance + sub-scene). */
void steam_scene_remove_dynamic_mesh(SteamScene* s, int handle);

/* Borrow the underlying phonon objects (void* = opaque IPLContext / IPLScene) so the reflection bed
 * can build a reflections simulator on the SAME committed geometry. Valid for the scene's lifetime;
 * the reflection bed must be destroyed before the scene. Concurrent commits (this scene's sim thread)
 * and reflection/pathing ray-traces are serialized by the scene lock below — take it SHARED around a
 * borrowing sim's RunReflections/RunPathing (the commit side takes it exclusive internally). */
void* steam_scene_ipl_context(SteamScene* s);
void* steam_scene_ipl_scene(SteamScene* s);
int   steam_scene_ipl_scenetype(SteamScene* s);   /* IPLSceneType of the shared scene (DEFAULT/EMBREE); reflect sim must match */

/* Reader lock over the committed scene, for the borrowing sims (steam_reflect / steam_path). A ray
 * trace that reads the shared scene's BVH must hold this SHARED while it runs, so it can't race an
 * iplSceneCommit (a dynamic-mesh move or a mesh swap) on the scene's own sim thread. NULL-safe. */
void steam_scene_ray_lock(SteamScene* s);
void steam_scene_ray_unlock(SteamScene* s);

void steam_scene_destroy(SteamScene* s);   /* stops the sim thread, releases phonon objects */

#endif /* BWA_STEAM_SCENE_H */
