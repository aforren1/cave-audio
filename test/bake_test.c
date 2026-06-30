/*
 * bake_test.c — baked reflection-bed check (with-SDK only).
 *
 * Same setup as reflect_test, but baking ON: a grid of probes covers the listening zone and the
 * listener-centric reverb is PRECOMPUTED at create; the sim thread then looks it up instead of
 * ray-tracing. Verifies the bake ran and produced a usable, DIRECTIONAL reverb out of baked data —
 * the +X-near listener must still get +X-biased reverb, not silence and not omni.
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
    RtCore* rt = rt_create(64, 64, SR, BW_CHANNELS);
    if (!rt) { printf("FAIL: rt_create\n"); return 1; }
    Layout L = layout_default();
    rt_set_layout(rt, &L);

    float p[3] = { 2.0f, 0.0f, 0.0f }, q[4] = { 0.0f, 0.0f, 0.0f, 1.0f };   /* 1 m from the +X wall */
    float* bus = (float*)calloc((size_t)BW_CHANNELS * BLK, sizeof(float));
    BwTimestamp ts; memset(&ts, 0, sizeof ts);
    rt_set_listener(rt, p, q); rt_commit(rt); rt_render(rt, bus, BLK, &ts);

    SteamScene* scene = steam_scene_create(rt, SR, BLK, 64);
    if (!scene) { printf("FAIL: steam_scene_create\n"); free(bus); rt_destroy(rt); return 1; }

    float verts[] = { -3,-2,-3,  3,-2,-3,  3,2,-3,  -3,2,-3,  -3,-2,3,  3,-2,3,  3,2,3,  -3,2,3 };
    int   tris[]  = { 0,1,2, 0,2,3,  4,6,5, 4,7,6,  0,4,7, 0,7,3,  1,2,6, 1,6,5,  3,2,6, 3,6,7,  0,1,5, 0,5,4 };
    float absorption[3] = { 0.1f, 0.1f, 0.1f }, transmission[3] = { 0.05f, 0.05f, 0.05f };
    steam_scene_set_mesh(scene, verts, 8, tris, 12, absorption, 0.5f, transmission);

    printf("baking reverb at a probe grid (ray-traces every probe once)...\n"); fflush(stdout);
    SteamReflect* refl = steam_reflect_create(scene, rt, &L, SR, BLK, /*order*/1, /*ir*/0.4f, /*rays*/2048, /*bounces*/8, /*wet*/1.0f, /*bake*/1);
    if (!refl) { printf("FAIL: steam_reflect_create (baked)\n"); steam_scene_destroy(scene); free(bus); rt_destroy(rt); return 1; }

    Sleep(600);                       /* let the sim thread look up + publish the baked reverb */

    double e[BW_CHANNELS]; for (int s = 0; s < BW_CHANNELS; ++s) e[s] = 0.0;
    float aux[256];
    for (int blk = 0; blk < 80; ++blk) {
        for (uint32_t i = 0; i < BLK; ++i) aux[i] = (i == 0) ? 1.0f : 0.0f;
        memset(bus, 0, (size_t)BW_CHANNELS * BLK * sizeof(float));
        steam_reflect_tap(refl, bus, BLK, p, q, aux);
        for (int s = 0; s < BW_CHANNELS; ++s)
            for (uint32_t i = 0; i < BLK; ++i) { float v = bus[(size_t)s * BLK + i]; e[s] += (double)v * v; }
    }

    double ePlusX = 0.0, eMinusX = 0.0; int nPlus = 0, nMinus = 0;
    for (int s = 0; s < BW_CHANNELS; ++s) {
        if (L.speakers[s].pos[0] >  0.5f) { ePlusX  += e[s]; ++nPlus;  }
        else if (L.speakers[s].pos[0] < -0.5f) { eMinusX += e[s]; ++nMinus; }
    }
    double ratio = ePlusX / (eMinusX + 1e-12);
    printf("baked, listener near +X wall:  +X-face energy=%.6g (%d spk)   -X-face energy=%.6g (%d spk)   ratio=%.2f\n",
           ePlusX, nPlus, eMinusX, nMinus, ratio);

    int pass = (ePlusX > 1e-9) && (ratio > 1.3);   /* non-silent + directional, from baked data */
    printf("%s\n", pass ? "PASS: baked reflection bed is non-silent and directional"
                        : "FAIL: baked reverb silent or non-directional");

    steam_reflect_destroy(refl);
    steam_scene_destroy(scene);
    rt_destroy(rt);
    free(bus);
    return pass ? 0 : 1;
}
