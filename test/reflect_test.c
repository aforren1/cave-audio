/*
 * reflect_test.c — directional reflection-bed regression test (with-SDK only).
 *
 * Protects the property that the hybrid reverb bed renders DIRECTIONAL early reflections (not the
 * old omni-only workaround): with the listener placed off-center — close to the +X wall of a box —
 * the reflections off that near wall must decode louder into the +X-facing speakers than the -X
 * ones. This exercises the full bw_audio path (phonon ambisonic convolution -> custom 26-speaker
 * decode -> bus sum) and depends on the vendored phonon's alignment patch (multichannel reflections
 * crash without it). It drives steam_reflect_tap directly with a known aux send, so it needs no
 * device, sound asset, or voice machinery.
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

int main(void) {
    const uint32_t SR = 48000, BLK = 256;
    RtCore* rt = rt_create(64, 64, SR, BWA_CHANNELS);
    if (!rt) { printf("FAIL: rt_create\n"); return 1; }
    Layout L = layout_default();
    rt_set_layout(rt, &L);

    /* Promote the off-center listener pose BEFORE the reflection sim starts, so its accumulating
     * energy field is built for (2,1.5,0) from the first tick (never the centered default). 1 m from
     * the +X wall, 5 m from -X, on the array's ear plane (floor origin: ears at y=1.5). */
    float p[3] = { 2.0f, 1.5f, 0.0f }, q[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float* bus = (float*)calloc((size_t)BWA_CHANNELS * BLK, sizeof(float));
    bwa_timestamp ts; memset(&ts, 0, sizeof ts);
    rt_set_listener(rt, p, q);
    rt_commit(rt);
    rt_render(rt, bus, BLK, &ts);     /* drain the commit -> active pose = (2,0,0); published for rt_read_pose */

    SteamScene* scene = steam_scene_create(rt, SR, BLK, 64, 0);
    if (!scene) { printf("FAIL: steam_scene_create\n"); free(bus); rt_destroy(rt); return 1; }

    /* a reflective box room [-3,3] x [-2,2] x [-3,3] (8 verts, 12 tris) */
    float verts[] = { -3,-2,-3,  3,-2,-3,  3,2,-3,  -3,2,-3,  -3,-2,3,  3,-2,3,  3,2,3,  -3,2,3 };
    int   tris[]  = { 0,1,2, 0,2,3,  4,6,5, 4,7,6,  0,4,7, 0,7,3,  1,2,6, 1,6,5,  3,2,6, 3,6,7,  0,1,5, 0,5,4 };
    float absorption[3] = { 0.1f, 0.1f, 0.1f }, scattering[1] = { 0.5f }, transmission[3] = { 0.05f, 0.05f, 0.05f };
    steam_scene_set_mesh_mat(scene, verts, 8, tris, 12, 1, absorption, scattering, transmission, NULL);

    SteamReflect* refl = steam_reflect_create(scene, rt, &L, SR, BLK, /*order*/1, /*ir*/0.5f, /*rays*/4096, /*bounces*/16, /*wet*/1.0f, /*bake*/0);
    if (!refl) { printf("FAIL: steam_reflect_create\n"); steam_scene_destroy(scene); free(bus); rt_destroy(rt); return 1; }

    Sleep(600);                       /* let the off-thread sim accumulate + publish the (2,0,0) IR */

    /* drive the bed with a periodic impulse; accumulate per-speaker reverb energy */
    double e[BWA_CHANNELS]; for (int s = 0; s < BWA_CHANNELS; ++s) e[s] = 0.0;
    float aux[256];
    for (int blk = 0; blk < 80; ++blk) {
        for (uint32_t i = 0; i < BLK; ++i) aux[i] = (i == 0) ? 1.0f : 0.0f;
        memset(bus, 0, (size_t)BWA_CHANNELS * BLK * sizeof(float));
        steam_reflect_tap(refl, bus, BLK, p, q, aux);
        for (int s = 0; s < BWA_CHANNELS; ++s)
            for (uint32_t i = 0; i < BLK; ++i) { float v = bus[(size_t)s * BLK + i]; e[s] += (double)v * v; }
    }

    /* split energy by speaker face: +X (pos.x > 0.5) vs -X (pos.x < -0.5) */
    double ePlusX = 0.0, eMinusX = 0.0; int nPlus = 0, nMinus = 0;
    for (int s = 0; s < BWA_CHANNELS; ++s) {
        if (L.speakers[s].pos[0] >  0.5f) { ePlusX  += e[s]; ++nPlus;  }
        else if (L.speakers[s].pos[0] < -0.5f) { eMinusX += e[s]; ++nMinus; }
    }
    double ratio = ePlusX / (eMinusX + 1e-12);
    printf("listener near +X wall:  +X-face energy=%.6g (%d spk)   -X-face energy=%.6g (%d spk)   ratio=%.2f\n",
           ePlusX, nPlus, eMinusX, nMinus, ratio);

    /* directional: the near (+X) wall's reflections must dominate the far (-X) wall's. A
     * non-directional (omni) bed would split ~evenly (ratio ~1). Require a clear margin. */
    int pass = (ePlusX > 0.0) && (ratio > 1.3);
    printf("%s\n", pass ? "PASS: reflection bed is directional (+X-biased reverb for an +X-near listener)"
                        : "FAIL: reverb not directional (regressed to omni? or phonon alignment patch missing?)");

    steam_reflect_destroy(refl);
    steam_scene_destroy(scene);
    rt_destroy(rt);
    free(bus);
    return pass ? 0 : 1;
}
