/*
 * monitor_test.c — M5 verification of the binaural monitor DSP (no device/audio thread):
 *   - a source on a right-side speaker is louder in the right ear (and vice-versa);
 *   - a source on the median plane (x≈0) is balanced L≈R;
 *   - rotating the head 180° flips left/right;
 *   - the decode is finite and roughly energy-preserving.
 */
#include "binaural.h"
#include "layout.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define CH   BWA_CHANNELS
#define N    64
#define RATE 48000u

static float bus[CH * N];
static float out[2 * N];

static double e_left(void)  { double e = 0; for (int i = 0; i < N; ++i) e += fabs(out[i]);     return e; }
static double e_right(void) { double e = 0; for (int i = 0; i < N; ++i) e += fabs(out[N + i]); return e; }

/* drive a constant 1.0 on exactly one bus channel, decode, with listener at origin + pose q.
 * The monitor ramps its pan gains one block toward a NEW pose (invariant 4 — no zipper as the head
 * turns), so render twice and measure the settled block when the pose changed from the last call. */
static void decode_channel(Monitor* m, int ch, const float q[4]) {
    const float p[3] = { 0, 1.5f, 0 };   /* the default grid's ear point (floor origin) */
    memset(bus, 0, sizeof bus);
    for (int i = 0; i < N; ++i) bus[(size_t)ch * N + i] = 1.0f;
    monitor_process(m, bus, p, q, out, N);
    monitor_process(m, bus, p, q, out, N);
}

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", (msg)); ++fails; } } while (0)

int main(void) {
    Layout L = layout_default();
    Monitor* m = monitor_create(&L, RATE);
    CHECK(m != NULL, "monitor_create");
    if (!m) return 1;

    /* find a right (-x), left (+x), and median-plane (x≈0) speaker. Room convention:
     * identity faces +z with +y up (right-handed), so the listener's right is -x. */
    int right = -1, left = -1, center = -1;
    for (int k = 0; k < CH; ++k) {
        float x = L.speakers[k].pos[0];
        if (x < -1.0f) right  = k;
        if (x >  1.0f) left   = k;
        if (fabsf(x) < 0.01f) center = k;
    }
    CHECK(right >= 0 && left >= 0 && center >= 0, "default layout has right/left/median speakers");

    const float ident[4]   = { 0, 0, 0, 1 };       /* head facing forward (+z) */
    const float yaw180[4]  = { 0, 1, 0, 0 };       /* head turned 180° about +y */

    /* 1. right speaker -> right ear louder; left speaker -> left ear louder */
    decode_channel(m, right, ident);
    CHECK(e_right() > e_left() * 1.2, "right speaker is louder in the right ear");
    double rL = e_left(), rR = e_right();
    decode_channel(m, left, ident);
    CHECK(e_left() > e_right() * 1.2, "left speaker is louder in the left ear");

    /* 2. median-plane speaker -> balanced, at the constant-power level (each ear = sqrt(0.5)) */
    decode_channel(m, center, ident);
    CHECK(fabs(e_left() - e_right()) < 0.01 * (e_left() + e_right()) + 1e-6, "median speaker is balanced L≈R");
    CHECK(fabs(e_left() - (double)N * 0.70710678) < 0.02 * (double)N, "median decode at the constant-power level");

    /* 3. rotating the head 180° flips the right speaker to the left ear */
    decode_channel(m, right, yaw180);
    CHECK(e_left() > e_right() * 1.2, "head turned 180°: the right speaker now favors the left ear");
    CHECK(fabs(e_left()  - rR) < 1e-4 && fabs(e_right() - rL) < 1e-4, "180° turn swaps L/R (to float tolerance)");

    /* 4. finite + energy-bearing */
    decode_channel(m, right, ident);
    CHECK(isfinite(e_left()) && isfinite(e_right()) && (e_left() + e_right()) > 0.0, "output finite and audible");

    monitor_destroy(m);
    if (fails) { printf("monitor_test: %d FAILURES\n", fails); return 1; }
    printf("monitor_test OK (L/R directionality, median balance, head-rotation flip)\n");
    return 0;
}
