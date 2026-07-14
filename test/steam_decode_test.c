/*
 * steam_decode_test.c — the production HRTF monitor decode runs and spatializes (with-SDK only).
 *
 * The 26->stereo Steam Audio HRTF decode (steam_decode.c) was the one phonon path with no test: it was
 * built + linked but exercised by no assertion. This closes that gap, mirroring monitor_test's checks
 * (which cover the simple-pan fallback in binaural.c) for the phonon path: build the monitor, drive one
 * hard-side speaker, and assert the decode (a) runs and produces finite, audible stereo and (b)
 * preserves laterality — right speaker -> right ear, left -> left, and a 180-degree head turn flips it.
 * It does NOT judge HRTF *quality* (that stays the by-ear check); it proves the decode actually works.
 */
#include "steam_decode.h"
#include "layout.h"
#include "sink.h"          /* BWA_CHANNELS */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 1024u            /* device block == phonon frameSize */

static float* bus;         /* BWA_CHANNELS * N */
static float* out;         /* 2 * N (L at [0,N), R at [N,2N)) */
static int    fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", (msg)); ++fails; } } while (0)

static double e_left(void)  { double e = 0; for (uint32_t i = 0; i < N; ++i) e += fabs(out[i]);     return e; }
static double e_right(void) { double e = 0; for (uint32_t i = 0; i < N; ++i) e += fabs(out[N + i]); return e; }

static void decode_channel(SteamMonitor* m, int ch, const float q[4]) {
    const float p[3] = { 0, 1.5f, 0 };   /* the default grid's ear point (floor origin) */
    memset(bus, 0, sizeof(float) * (size_t)BWA_CHANNELS * N);
    for (uint32_t i = 0; i < N; ++i) bus[(size_t)ch * N + i] = 1.0f;
    /* phonon crossfades an orientation CHANGE across one block (AmbisonicsRotateEffect keeps the
     * previous rotation and interpolates), so the first block after a new q is a smear of old and
     * new. Render twice and measure the settled block. (The old head convention masked this: its
     * identity equalled phonon's initial internal rotation, so block 1 happened to be pure.) */
    steam_monitor_process(m, bus, p, q, out, N);
    steam_monitor_process(m, bus, p, q, out, N);
}

int main(void) {
    Layout L = layout_default();
    bus = (float*)calloc((size_t)BWA_CHANNELS * N, sizeof(float));
    out = (float*)calloc((size_t)2 * N, sizeof(float));
    if (!bus || !out) { printf("FAIL: alloc\n"); return 1; }

    SteamMonitor* m = steam_monitor_create(&L, 48000, N, NULL);   /* built-in HRTF */
    CHECK(m != NULL, "steam_monitor_create (built-in HRTF)");
    if (!m) { printf("steam_decode_test: %d FAILURES\n", fails); return 1; }

    int right = -1, left = -1;     /* pure-lateral speakers (y,z ~ 0) so the HRTF L/R isn't diluted by elevation */
    for (int k = 0; k < (int)BWA_CHANNELS; ++k) {     /* identity faces +z, so the listener's right is -x */
        float x = L.speakers[k].pos[0], y = L.speakers[k].pos[1], z = L.speakers[k].pos[2];
        if (fabsf(y - 1.5f) > 0.01f || fabsf(z) > 0.01f) continue;   /* lateral = at ear height */
        if (x < -1.0f) right = k;
        if (x >  1.0f) left  = k;
    }
    CHECK(right >= 0 && left >= 0, "default layout has pure-lateral right (-x) and left (+x) speakers");

    const float ident[4]  = { 0, 0, 0, 1 };          /* head facing forward (+z) */
    const float yaw180[4] = { 0, 1, 0, 0 };          /* head turned 180 about +y */

    /* 1. the decode runs + is finite + audible */
    decode_channel(m, right, ident);
    CHECK(isfinite(e_left()) && isfinite(e_right()) && (e_left() + e_right()) > 1e-6,
          "HRTF decode runs: finite + audible stereo");

    /* 2. laterality: right speaker -> right ear, left -> left */
    double rL = e_left(), rR = e_right();
    printf("right speaker: L=%.4g R=%.4g\n", rL, rR);
    CHECK(rR > rL * 1.1, "right speaker favors the right ear");
    decode_channel(m, left, ident);
    printf("left speaker:  L=%.4g R=%.4g\n", e_left(), e_right());
    CHECK(e_left() > e_right() * 1.1, "left speaker favors the left ear");

    /* 3. a 180-degree head turn flips the right speaker to the left ear */
    decode_channel(m, right, yaw180);
    printf("right + 180:   L=%.4g R=%.4g\n", e_left(), e_right());
    CHECK(e_left() > e_right() * 1.1, "head turned 180: the right speaker now favors the left ear");

    steam_monitor_destroy(m);
    free(bus); free(out);
    if (fails) { printf("steam_decode_test: %d FAILURES\n", fails); return 1; }
    printf("steam_decode_test OK (HRTF decode runs + preserves laterality)\n");
    return 0;
}
