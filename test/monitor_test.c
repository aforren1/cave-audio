/*
 * monitor_test.c — M5 verification of the binaural monitor DSP (no device/audio thread):
 *   - a source on a right-side speaker is louder in the right ear (and vice-versa);
 *   - a source on the median plane (x≈0) is balanced L≈R;
 *   - rotating the head 180° flips left/right;
 *   - the decode is finite and roughly energy-preserving;
 *   - the direct-binaural field (BWA_PROFILE_BINAURAL, cardioid fallback decode): laterality,
 *     the 180° flip, median balance, and the cardioid level.
 */
#include "binaural.h"
#include "layout.h"
#include "ambisonics.h"

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
    monitor_process(m, bus, NULL, p, q, out, N);
    monitor_process(m, bus, NULL, p, q, out, N);
}

/* drive the DIRECT field with a constant-amplitude plane wave from room direction `dir` (the
 * per-channel SH coefficients rt.c's direct solve would produce for a unit signal), silent bus. */
static float dfield[BWA_AMBI_CH * N];
static void decode_direct(Monitor* m, const float dir[3], const float q[4]) {
    const float p[3] = { 0, 1.5f, 0 };
    float y[BWA_AMBI_CH];
    ambi_encode_phonon(dir, y);
    memset(bus, 0, sizeof bus);
    for (int k = 0; k < BWA_AMBI_CH; ++k)
        for (int i = 0; i < N; ++i) dfield[(size_t)k * N + i] = y[k];
    monitor_process(m, bus, dfield, p, q, out, N);
    monitor_process(m, bus, dfield, p, q, out, N);
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

    /* 5. direct-binaural field (the BWA_PROFILE_BINAURAL fallback decode): a plane wave from the
     * listener's right (-x room) favors the right ear (the cardioid there reads 0.5*(1+1) = 1, the
     * opposed one 0), flips on a 180° turn, and balances dead ahead at the 0.5 cardioid level. */
    const float dRight[3] = { -1.f, 0.f, 0.f }, dAhead[3] = { 0.f, 0.f, 1.f };
    decode_direct(m, dRight, ident);
    CHECK(e_right() > e_left() * 1.2, "direct field from the right favors the right ear");
    decode_direct(m, dRight, yaw180);
    CHECK(e_left() > e_right() * 1.2, "direct field: a 180° turn flips the ears");
    decode_direct(m, dAhead, ident);
    CHECK(fabs(e_left() - e_right()) < 0.01 * (e_left() + e_right()) + 1e-6, "direct field ahead is balanced");
    CHECK(fabs(e_left() - (double)N * 0.5) < 0.02 * (double)N, "direct field ahead at the cardioid level");

    /* 6. the bed pass-through diagonal (ambi_canon_to_phonon): a canon-basis encode times the
     * diagonal must equal the monitor-basis encode for ANY direction — pins the (-1)^|m| x
     * orthonormal-rescale table against the shared encode, so it cannot silently drift. */
    {
        const float dirs[4][3] = { { 1, 0, 0 }, { 0, 0, 1 },
                                   { 0.5773503f, 0.5773503f, 0.5773503f }, { -0.7f, 0.14f, 0.7f } };
        double maxerr = 0.0;
        for (int t = 0; t < 4; ++t) {
            float dr[3] = { dirs[t][0], dirs[t][1], dirs[t][2] };
            const float len = sqrtf(dr[0]*dr[0] + dr[1]*dr[1] + dr[2]*dr[2]);
            dr[0] /= len; dr[1] /= len; dr[2] /= len;
            float a[3]; room_to_ambi(dr, a);
            float yc[BWA_AMBI_CH], yp[BWA_AMBI_CH];
            ambi_encode_sn3d(a, yc);
            ambi_encode_phonon(dr, yp);
            for (int k = 0; k < BWA_AMBI_CH; ++k) {
                const double e = fabs((double)yc[k] * ambi_canon_to_phonon[k] - yp[k]);
                if (e > maxerr) maxerr = e;
            }
        }
        CHECK(maxerr < 1e-5, "canon->phonon diagonal matches the monitor-basis encode");
    }

    monitor_destroy(m);
    if (fails) { printf("monitor_test: %d FAILURES\n", fails); return 1; }
    printf("monitor_test OK (L/R directionality, median balance, head-rotation flip, direct-field decode, "
           "canon->phonon diagonal)\n");
    return 0;
}
