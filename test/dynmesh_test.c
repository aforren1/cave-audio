/*
 * dynmesh_test.c — dynamic (instanced) occluder + shared-scene-lock regression (with-SDK only).
 *
 * Proves two things at once:
 *   1. Path B: a movable occluder added at runtime via steam_scene_add_dynamic_mesh actually enters
 *      the ray-traced scene, and moving it with steam_scene_set_dynamic_transform changes what it
 *      blocks — occlusion goes clear -> blocked -> clear as a wall slides between a source and the
 *      listener and back, and stays clear after removal.
 *   2. Blocker 1: a steam_reflect bed runs its reflection sim on the SAME borrowed scene throughout,
 *      so its RunReflections reads the scene concurrently with the occlusion sim's dynamic-mesh
 *      commits. If the shared scene lock were missing/wrong this deadlocks or crashes; passing (and
 *      shutting down cleanly) pins the lock.
 *
 * Occlusion is a geometric visibility test (deterministic), published per source via rt_set_direct and
 * read back with rt_get_occlusion — no device, sound asset, or voice machinery needed.
 */
#include "rt.h"
#include "layout.h"
#include "steam_scene.h"
#include "steam_reflect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* row-major 4x4 pure translation (identity rotation), phonon IPLMatrix4x4 order (translation column) */
static void trans_m16(float m[16], float x, float y, float z) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.f;
    m[3] = x; m[7] = y; m[11] = z;
}

/* wait for the 30 Hz occlusion sim to re-trace + publish, then read the source's occlusion factor */
static float settle_occ(RtCore* rt, uint32_t handle) {
    Sleep(250);
    return rt_get_occlusion(rt, handle);
}

int main(void) {
    const uint32_t SR = 48000, BLK = 256, H = 5 /* the occluded source's handle (idx 5) */;
    RtCore* rt = rt_create(64, 64, SR, BWA_CHANNELS);
    if (!rt) { printf("FAIL: rt_create\n"); return 1; }
    Layout L = layout_default();
    rt_set_layout(rt, &L);

    /* listener at the centre ear plane, source 3 m to +X on the same line */
    float lp[3] = { 0.0f, 1.5f, 0.0f }, lq[4] = { 0, 0, 0, 1 };
    rt_set_listener(rt, lp, lq); rt_commit(rt);
    float* bus = (float*)calloc((size_t)BWA_CHANNELS * BLK, sizeof(float));
    bwa_timestamp ts; memset(&ts, 0, sizeof ts);
    rt_render(rt, bus, BLK, &ts);                 /* promote the pose (published for rt_read_pose) */

    SteamScene* scene = steam_scene_create(rt, SR, BLK, 64, 0);
    if (!scene) { printf("FAIL: steam_scene_create\n"); free(bus); rt_destroy(rt); return 1; }

    /* a reflection bed borrowing the same scene: its sim thread reads the scene while we mutate it */
    SteamReflect* refl = steam_reflect_create(scene, rt, &L, SR, BLK, 1, 0.5f, 2048, 8, 1.0f, 0);
    if (!refl) { printf("FAIL: steam_reflect_create\n"); steam_scene_destroy(scene); free(bus); rt_destroy(rt); return 1; }

    /* occlude a source 3 m to +X of the listener */
    steam_scene_set_pos(scene, H, 3.0f, 1.5f, 0.0f);
    steam_scene_set_occlusion(scene, H, 1);

    /* a 4x4 movable wall in the YZ plane (local), so a translation in x slides it across the +X line */
    float wv[] = { 0,-2,-2,  0,2,-2,  0,2,2,  0,-2,2 };
    int   wt[] = { 0,1,2,  0,2,3 };
    float absorp[3] = { 0.1f, 0.1f, 0.1f }, scat = 0.5f, trans[3] = { 0.02f, 0.02f, 0.02f };
    int wall = steam_scene_add_dynamic_mesh(scene, wv, 4, wt, 2, absorp, scat, trans);
    if (wall < 0) { printf("FAIL: add_dynamic_mesh returned %d\n", wall); goto fail; }

    float m[16];
    /* (a) wall parked far behind the listener (x = -5): the +X line of sight is clear */
    trans_m16(m, -5.0f, 1.5f, 0.0f); steam_scene_set_dynamic_transform(scene, wall, m);
    float occ_clear0 = settle_occ(rt, H);

    /* (b) slide the wall between listener (x=0) and source (x=3): the line is blocked */
    trans_m16(m, 1.5f, 1.5f, 0.0f); steam_scene_set_dynamic_transform(scene, wall, m);
    float occ_blocked = settle_occ(rt, H);

    /* (c) slide it back out of the way: clear again */
    trans_m16(m, -5.0f, 1.5f, 0.0f); steam_scene_set_dynamic_transform(scene, wall, m);
    float occ_clear1 = settle_occ(rt, H);

    /* (d) remove it entirely: still clear (scene commits empty) */
    steam_scene_remove_dynamic_mesh(scene, wall);
    float occ_removed = settle_occ(rt, H);

    printf("occlusion: far=%.3f  between=%.3f  far-again=%.3f  removed=%.3f\n",
           occ_clear0, occ_blocked, occ_clear1, occ_removed);

    int pass = (occ_clear0 > 0.9f)              /* unblocked when the wall is elsewhere */
            && (occ_blocked < 0.5f)             /* the moved wall blocks the direct path */
            && (occ_clear1  > 0.9f)             /* moving it away restores line of sight */
            && (occ_removed > 0.9f);            /* removal leaves the path clear */
    printf("%s\n", pass ? "PASS: dynamic occluder blocks when moved between, clears when moved away/removed; "
                          "reflect sim ran concurrently (shared scene lock holds)"
                        : "FAIL: dynamic occluder did not track its transform (Path B) or the sim stalled");

    steam_reflect_destroy(refl);
    steam_scene_destroy(scene);
    rt_destroy(rt);
    free(bus);
    return pass ? 0 : 1;

fail:
    steam_reflect_destroy(refl);
    steam_scene_destroy(scene);
    rt_destroy(rt);
    free(bus);
    return 1;
}
