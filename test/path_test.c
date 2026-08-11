/*
 * path_test.c — pathing routes around an occluder (with-SDK only).
 *
 * A wall at x=0 (z in [-3, 0.5]) blocks the straight line between a source at (-2,1.5,-1.5) and a
 * listener at (2,1.5,-1.5) (the ear plane; the probe grid sits at mean speaker height y=1.5); the
 * only way across is around the wall's end past z=0.5. If pathing finds a
 * route, the source's path output carries energy (shCoeffs[0] > 0) — that energy can ONLY have come
 * around the wall, since the direct line is blocked. Proves the pathing sim + visibility bake work.
 */
#include "rt.h"
#include "layout.h"
#include "steam_scene.h"
#include "steam_path.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int main(void) {
    const uint32_t SR = 48000, BLK = 256, ORDER = 1;
    RtCore* rt = rt_create(64, 64, SR, BWA_CHANNELS);
    if (!rt) { printf("FAIL: rt_create\n"); return 1; }
    Layout L = layout_default();
    rt_set_layout(rt, &L);

    SteamScene* scene = steam_scene_create(rt, SR, BLK, 64, 0);
    if (!scene) { printf("FAIL: steam_scene_create\n"); rt_destroy(rt); return 1; }

    /* a wall quad at x=0, spanning y in [-2,2], z in [-3, 0.5] — blocks the direct line at z=-1.5,
     * leaving only the opening past z=0.5. (Two triangles, both windings, so it occludes either way.) */
    float verts[] = { 0,-2,-3,  0,2,-3,  0,2,0.5f,  0,-2,0.5f };
    int   tris[]  = { 0,1,2, 0,2,3,  0,2,1, 0,3,2 };
    float absorption[3] = { 0.2f, 0.2f, 0.2f }, scattering[1] = { 0.5f }, transmission[3] = { 0.0f, 0.0f, 0.0f };
    steam_scene_set_mesh_mat(scene, verts, 4, tris, 4, 1, absorption, scattering, transmission, NULL);

    printf("baking pathing visibility...\n"); fflush(stdout);
    SteamPath* sp = steam_path_create(scene, rt, &L, SR, BLK, ORDER);
    if (!sp) { printf("FAIL: steam_path_create\n"); steam_scene_destroy(scene); rt_destroy(rt); return 1; }

    float src[3] = { -2.0f, 1.5f, -1.5f }, listener[3] = { 2.0f, 1.5f, -1.5f };
    steam_path_set_source(sp, /*handle*/1, src, /*on*/1);

    float eq[3] = {0,0,0}, sh[16] = {0};
    int found = steam_path_debug_run_get(sp, listener, 1, eq, sh);
    printf("path: sh0=%.5f  sh=[%.4f %.4f %.4f %.4f]  eq=[%.3f %.3f %.3f]\n",
           sh[0], sh[0], sh[1], sh[2], sh[3], eq[0], eq[1], eq[2]);

    /* A route exists (energy arrives) AND it is directional (a pure-omni sh would mean no real path).
     * The direct line is blocked, so any energy came around the wall. */
    double dirmag = sqrt((double)sh[1]*sh[1] + (double)sh[2]*sh[2] + (double)sh[3]*sh[3]);
    int pass = found && (sh[0] > 1e-4f) && (dirmag > 1e-5);
    printf("%s\n", pass ? "PASS: pathing routes around the wall (indirect energy + direction)"
                        : "FAIL: no path found around the wall");

    /* Slot reclamation under source churn: a long-running installation creates and destroys pathing
     * sources indefinitely, so a destroyed handle's slot must return to the pool (and its IPLSource
     * release) — claim-forever slots exhaust the 64-slot table for good. Churn well past the table
     * size; each debug_run_get reconciles inline, which is where the sweep runs (no sim thread here).
     * If reclamation regresses, the final source finds no slot and paths nothing. */
    int churn_ok = 1;
    for (uint32_t i = 0; i < 80; ++i) {
        uint32_t h = 1000u + i;                       /* 80 distinct handles > PATH_MAX_SRC (64) */
        steam_path_set_source(sp, h, src, 1);
        steam_path_source_gone(sp, h);
        steam_path_debug_run_get(sp, listener, h, eq, sh);   /* reconcile: sweeps the dead slot */
    }
    steam_path_set_source(sp, /*handle*/2, src, /*on*/1);
    churn_ok = steam_path_debug_run_get(sp, listener, 2, eq, sh) && sh[0] > 1e-4f;
    printf("%s\n", churn_ok ? "PASS: destroyed sources return their pathing slots (churn > table size)"
                            : "FAIL: pathing slot table exhausted under source churn");
    pass = pass && churn_ok;

    steam_path_destroy(sp);
    steam_scene_destroy(scene);
    rt_destroy(rt);
    return pass ? 0 : 1;
}
