/*
 * reflect_test.c — directional reflection-bed regression test (with-SDK only).
 *
 * Protects the property that the hybrid reverb bed renders DIRECTIONAL early reflections (not the
 * old omni-only workaround): with the listener placed off-center — close to the +X wall of a box —
 * the reflections off that near wall must decode louder into the +X-facing speakers than the -X
 * ones. This exercises the full bw_audio path (phonon ambisonic convolution -> custom 26-speaker
 * decode -> bus sum) and depends on the vendored phonon's alignment patch (multichannel reflections
 * crash without it). It drives steam_reflect_tap directly with a known aux send, so it needs no
 * device, sound asset, or voice machinery. It also pins the wet-gain ramp (invariant 4): a gain change
 * must ramp across one block, never step at the block boundary.
 */
#include "core/rt.h"
#include "core/layout.h"
#include "acoustics/steam_scene.h"
#include "acoustics/steam_reflect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os/os.h"

/* One block of deterministic noise through the tap onto a cleared bus; e4 gets the bus energy in
 * each quarter of a `blk`-frame block (all four 0 when the tap rendered nothing). */
static void tap_noise(SteamReflect* r, float* bus, uint32_t n, uint32_t blk, const float* p, const float* q,
                      float* aux, double e4[4]) {
    static uint32_t lcg = 12345u;
    for (uint32_t i = 0; i < n; ++i) {
        lcg = lcg * 1664525u + 1013904223u;
        aux[i] = ((float)(lcg >> 8) / 16777216.f - 0.5f) * 0.5f;
    }
    memset(bus, 0, (size_t)BWA_DEFAULT_GRID * blk * sizeof(float));
    steam_reflect_tap(r, bus, n, p, q, aux);
    for (int k = 0; k < 4; ++k) e4[k] = 0.0;
    for (int s = 0; s < BWA_DEFAULT_GRID; ++s)
        for (uint32_t i = 0; i < blk; ++i) { const float v = bus[(size_t)s * blk + i]; e4[i / (blk / 4)] += (double)v * v; }
}

int main(void) {
    const uint32_t SR = 48000, BLK = 256;
    RtCore* rt = rt_create(64, 64, SR, BWA_DEFAULT_GRID);
    if (!rt) { printf("FAIL: rt_create\n"); return 1; }
    static Layout L; layout_default(&L);
    rt_set_layout(rt, &L);

    /* Promote the off-center listener pose BEFORE the reflection sim starts, so its accumulating
     * energy field is built for (2,1.5,0) from the first tick (never the centered default). 1 m from
     * the +X wall, 5 m from -X, on the array's ear plane (floor origin: ears at y=1.5). */
    float p[3] = { 2.0f, 1.5f, 0.0f }, q[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float* bus = (float*)calloc((size_t)BWA_DEFAULT_GRID * BLK, sizeof(float));
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

    os_sleep_ms(600);                       /* let the off-thread sim accumulate + publish the (2,0,0) IR */

    /* drive the bed with a periodic impulse; accumulate per-speaker reverb energy */
    double e[BWA_DEFAULT_GRID]; for (int s = 0; s < BWA_DEFAULT_GRID; ++s) e[s] = 0.0;
    float aux[256];
    for (int blk = 0; blk < 80; ++blk) {
        for (uint32_t i = 0; i < BLK; ++i) aux[i] = (i == 0) ? 1.0f : 0.0f;
        memset(bus, 0, (size_t)BWA_DEFAULT_GRID * BLK * sizeof(float));
        steam_reflect_tap(refl, bus, BLK, p, q, aux);
        for (int s = 0; s < BWA_DEFAULT_GRID; ++s)
            for (uint32_t i = 0; i < BLK; ++i) { float v = bus[(size_t)s * BLK + i]; e[s] += (double)v * v; }
    }

    /* split energy by speaker face: +X (pos.x > 0.5) vs -X (pos.x < -0.5) */
    double ePlusX = 0.0, eMinusX = 0.0; int nPlus = 0, nMinus = 0;
    for (int s = 0; s < BWA_DEFAULT_GRID; ++s) {
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

    /* Wet-gain ramp (invariant 4). Drive the bed with steady noise so the field is roughly stationary
     * across one block, then compare the first and last QUARTER of the block where the gain changes.
     * A 1 -> 0 ramp leaves ~0.77 of the gain-squared in the first quarter and ~0.02 in the last (37x);
     * a step leaves an all-zero block. 0 -> 1 is the mirror image, which a step fails by leaving the
     * quarters about equal. The 4x bar absorbs the field's own quarter-to-quarter fluctuation. No sim
     * timing: the directional check above already needed a published IR, and each ratio compares
     * quarters of ONE block. */
    double e4[4];
    for (int b = 0; b < 40; ++b) tap_noise(refl, bus, BLK, BLK, p, q, aux, e4);   /* settle at gain 1 */
    printf("ramp: steady   quarters %.4g %.4g %.4g %.4g\n", e4[0], e4[1], e4[2], e4[3]);

    steam_reflect_set_gain(refl, 0.f);
    tap_noise(refl, bus, BLK, BLK, p, q, aux, e4);
    printf("ramp: 1 -> 0   quarters %.4g %.4g %.4g %.4g\n", e4[0], e4[1], e4[2], e4[3]);
    int ramp_ok = (e4[0] > 0.0) && (e4[0] > 4.0 * e4[3]);
    printf("%s\n", ramp_ok ? "PASS: wet gain ramps down across the block"
                           : "FAIL: wet gain stepped down (first quarter not louder than the last)");

    tap_noise(refl, bus, BLK, BLK, p, q, aux, e4);
    const double after = e4[0] + e4[1] + e4[2] + e4[3];
    int land_ok = (after == 0.0);
    printf("%s (energy %.4g)\n", land_ok ? "PASS: ramp landed on 0" : "FAIL: ramp did not land on 0", after);

    steam_reflect_set_gain(refl, 1.f);
    tap_noise(refl, bus, BLK, BLK, p, q, aux, e4);
    printf("ramp: 0 -> 1   quarters %.4g %.4g %.4g %.4g\n", e4[0], e4[1], e4[2], e4[3]);
    int up_ok = (e4[3] > 0.0) && (e4[3] > 4.0 * e4[0]);
    printf("%s\n", up_ok ? "PASS: wet gain ramps up across the block"
                         : "FAIL: wet gain stepped up (last quarter not louder than the first)");

    /* an off-spec block renders dry, so the bed's applied gain there was 0: the next block must ramp
     * in from silence, not restart the field at full level */
    for (int b = 0; b < 4; ++b) tap_noise(refl, bus, BLK, BLK, p, q, aux, e4);
    tap_noise(refl, bus, BLK / 2, BLK, p, q, aux, e4);
    tap_noise(refl, bus, BLK, BLK, p, q, aux, e4);
    printf("ramp: resume   quarters %.4g %.4g %.4g %.4g\n", e4[0], e4[1], e4[2], e4[3]);
    int resume_ok = (e4[3] > 0.0) && (e4[3] > 4.0 * e4[0]);
    printf("%s\n", resume_ok ? "PASS: bed ramps back in after an off-spec (dry) block"
                             : "FAIL: bed restarted at full level after an off-spec (dry) block");
    pass = pass && ramp_ok && land_ok && up_ok && resume_ok;

    steam_reflect_destroy(refl);
    steam_scene_destroy(scene);
    rt_destroy(rt);
    free(bus);
    return pass ? 0 : 1;
}
