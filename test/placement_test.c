/*
 * placement_test.c - the tracked ZM-1's placement math (src/calib/placement.c): the acoustic center
 * from a pose plus the mount offset against hand-computed rotations, the marker-ring offset fit
 * (uneven spacing, noise, and the collinear and off-plane refusals), the mount angles, the default
 * thresholds, the stillness spread, the gate holding until the stand has settled (and dropping on a
 * jolt), the tolerance edges, accept-on-stillness, the bump check, and the refusal of every
 * non-finite or absurd input. The orientation half: the turn between two poses (sign-safe, small
 * angles exact), the per-mode turn limits, the orientation-aware bump (a twist about the center that
 * place_bump cannot see), and the gate's orientation term (jitter passes, a turn about the center
 * holds it shut). The tools on top (bwa_calibrate --track, calib_view's Placement panel)
 * are covered end to end by the calibrate_track_* ctests and calib_view --tests placement.
 */
#include "calib/placement.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", (msg)); ++fails; } } while (0)
#define NEAR(a, b, tol) (fabs((double)(a) - (double)(b)) <= (tol))

static const float S45 = 0.70710678f;

static int near3(const float a[3], double x, double y, double z, double tol) {
    return NEAR(a[0], x, tol) && NEAR(a[1], y, tol) && NEAR(a[2], z, tol);
}

/* center = p + R(q) . offset, against rotations worked by hand */
static void test_center(void) {
    float c[3];
    const float id[4] = { 0, 0, 0, 1 };
    const float p[3] = { 1.f, 2.f, 3.f }, o[3] = { 0.1f, 0.2f, 0.3f };
    CHECK(place_center(p, id, o, c) && near3(c, 1.1, 2.2, 3.3, 1e-6), "identity: center = p + offset");
    CHECK(place_center(p, id, NULL, c) && near3(c, 1, 2, 3, 1e-6), "NULL offset = the body origin");

    /* +90 deg about +y: body +z -> room +x, body +x -> room -z */
    const float qy[4] = { 0, S45, 0, S45 }, p1[3] = { 0.f, 1.f, 0.f };
    const float oz[3] = { 0, 0, 0.1f }, ox[3] = { 0.1f, 0, 0 };
    CHECK(place_center(p1, qy, oz, c) && near3(c, 0.1, 1.0, 0.0, 1e-6), "yaw +90: body +z offset lands on room +x");
    CHECK(place_center(p1, qy, ox, c) && near3(c, 0.0, 1.0, -0.1, 1e-6), "yaw +90: body +x offset lands on room -z");

    /* +90 deg about +x: body +y -> room +z (a stand tipped onto its face) */
    const float qx[4] = { S45, 0, 0, S45 }, oy[3] = { 0, 0.12f, 0 };
    CHECK(place_center(p1, qx, oy, c) && near3(c, 0.0, 1.0, 0.12, 1e-6), "pitch +90: body +y offset lands on room +z");

    /* 180 deg about +z: (x, y, z) -> (-x, -y, z) */
    const float qz[4] = { 0, 0, 1, 0 }, o3[3] = { 0.05f, 0.1f, 0.02f }, p0[3] = { 0, 0, 0 };
    CHECK(place_center(p0, qz, o3, c) && near3(c, -0.05, -0.1, 0.02, 1e-6), "roll 180: x and y flip, z stays");

    /* a slightly non-unit quaternion is normalized, not trusted */
    const float qs[4] = { 0, 1.5f * S45, 0, 1.5f * S45 };
    CHECK(place_center(p1, qs, oz, c) && near3(c, 0.1, 1.0, 0.0, 1e-6), "a 1.5x quaternion is normalized first");

    /* composed: yaw 30 deg about +y then offset (0, -0.12, 0.05): the stand's arm points down and
     * forward. R_y(30) (0, -0.12, 0.05) = (0.05 sin 30, -0.12, 0.05 cos 30) = (0.025, -0.12, 0.0433) */
    const float h = 0.2588190f, ch = 0.9659258f;                 /* sin 15, cos 15 */
    const float q30[4] = { 0, h, 0, ch }, parm[3] = { 0.2f, 1.6f, -0.1f }, oarm[3] = { 0.f, -0.12f, 0.05f };
    CHECK(place_center(parm, q30, oarm, c) && near3(c, 0.225, 1.48, -0.1 + 0.0433013, 1e-5), "yaw 30 + a down-and-forward arm");
}

static void test_refusals(void) {
    float c[3] = { 7, 7, 7 };
    const float id[4] = { 0, 0, 0, 1 }, p[3] = { 0, 1, 0 }, o[3] = { 0, 0.1f, 0 };
    const float qnan[4] = { 0, NAN, 0, 1 }, qinf[4] = { 0, 0, INFINITY, 1 };
    const float qsmall[4] = { 0, 0, 0, 0.1f }, qbig[4] = { 0, 0, 0, 3.f }, qzero[4] = { 0, 0, 0, 0 };
    const float pnan[3] = { 0, NAN, 0 }, pbig[3] = { 3e30f, 0, 0 }, pedge[3] = { 1500.f, 0, 0 };
    const float obig[3] = { 50.f, 0, 0 }, onan[3] = { 0, 0, NAN };
    CHECK(!place_center(p, qnan, o, c),   "NaN quaternion refused");
    CHECK(!place_center(p, qinf, o, c),   "Inf quaternion refused");
    CHECK(!place_center(p, qsmall, o, c), "a 0.1-norm quaternion is a corrupt frame, not identity");
    CHECK(!place_center(p, qzero, o, c),  "a zero quaternion refused");
    CHECK(!place_center(p, qbig, o, c),   "a 3-norm quaternion refused");
    CHECK(!place_center(pnan, id, o, c),  "NaN position refused");
    CHECK(!place_center(pbig, id, o, c),  "finite-but-absurd position refused");
    CHECK(!place_center(pedge, id, o, c), "a position past PLACE_MAX_COORD_M refused");
    CHECK(!place_center(p, id, obig, c),  "a 50 m mount offset refused");
    CHECK(!place_center(p, id, onan, c),  "NaN mount offset refused");
    CHECK(c[0] == 7 && c[1] == 7 && c[2] == 7, "a refused center leaves out untouched");
    float y = 5, t = 5;
    CHECK(!place_mount_angles(qnan, &y, &t) && y == 5 && t == 5, "mount angles refuse NaN");
}

static void test_angles(void) {
    float y, t;
    const float id[4] = { 0, 0, 0, 1 };
    CHECK(place_mount_angles(id, &y, &t) && NEAR(y, 0, 1e-4) && NEAR(t, 0, 1e-4), "identity: yaw 0, tilt 0");
    /* -90 about +y turns body +z onto room -x, which is room-RIGHT: bearing +90 */
    const float qr[4] = { 0, -S45, 0, S45 };
    CHECK(place_mount_angles(qr, &y, &t) && NEAR(y, 90, 1e-3) && NEAR(t, 0, 1e-3), "body +z to room-right reads yaw +90");
    const float ql[4] = { 0, S45, 0, S45 };
    CHECK(place_mount_angles(ql, &y, &t) && NEAR(y, -90, 1e-3), "body +z to room-left reads yaw -90");
    const float h = 0.2588190f, ch = 0.9659258f;                 /* 30 deg about +x */
    const float qt[4] = { h, 0, 0, ch };
    CHECK(place_mount_angles(qt, &y, &t) && NEAR(t, 30, 1e-3), "30 deg about +x tilts the stand 30");
}

static void test_cfg(void) {
    PlaceCfg c;
    place_cfg_default(&c, 0.010f);
    CHECK(NEAR(c.tol_m, 0.010, 1e-7) && NEAR(c.still_m, 0.002, 1e-7), "10 mm: still limit clamps to 2 mm");
    CHECK(NEAR(c.window_s, 0.5, 1e-9) && NEAR(c.hold_s, 1.0, 1e-9), "window 0.5 s, hold 1 s");
    place_cfg_default(&c, 0.004f);
    CHECK(NEAR(c.still_m, 0.001, 1e-7), "4 mm: still limit is a quarter, 1 mm");
    place_cfg_default(&c, 0.001f);
    CHECK(NEAR(c.still_m, 0.0005, 1e-7), "1 mm: still limit floors at 0.5 mm");
    place_cfg_default(&c, NAN);
    CHECK(NEAR(c.tol_m, 0.010, 1e-7), "NaN tolerance = the default");
    place_cfg_default(&c, 3e30f);
    CHECK(NEAR(c.tol_m, PLACE_TOL_MAX_M, 1e-7), "absurd tolerance clamps to 1 m");
    place_cfg_default(&c, 0.f);
    CHECK(c.tol_m == 0.f && NEAR(c.still_m, 0.002, 1e-7), "tolerance 0 = accept on stillness, 2 mm spread");
}

/* run a stationary stand at `at` for `secs` at `hz`, return the state at the end */
static PlaceState run_static(PlaceGate* g, const float at[3], const float tgt[3], double t0, double secs, double hz) {
    PlaceState s = PLACE_NO_POSE;
    const int n = (int)(secs * hz);
    for (int i = 0; i <= n; ++i) s = place_gate_update(g, t0 + i / hz, at, tgt);
    return s;
}

static void test_stillness(void) {
    PlaceGate g;
    place_gate_init(&g, NULL);
    const float tgt[3] = { 0.f, 1.448f, 0.f };
    /* +/-0.3 mm of jitter around a point: still */
    for (int i = 0; i <= 100; ++i) {
        const float c[3] = { tgt[0] + ((i & 1) ? 0.0003f : -0.0003f), tgt[1], tgt[2] };
        place_gate_update(&g, i * 0.01, c, tgt);
    }
    CHECK(NEAR(g.spread_m, 0.0003, 2e-5), "alternating +/-0.3 mm: spread 0.3 mm from the mean");
    CHECK(g.state == PLACE_SETTLING || g.state == PLACE_OK, "0.3 mm of jitter reads still");
    CHECK(NEAR(g.mean[0], 0.0, 1e-5), "the window mean sits on the jitter's center");
    /* +/-3 mm: moving */
    place_gate_init(&g, NULL);
    for (int i = 0; i <= 100; ++i) {
        const float c[3] = { tgt[0] + ((i & 1) ? 0.003f : -0.003f), tgt[1], tgt[2] };
        place_gate_update(&g, i * 0.01, c, tgt);
    }
    CHECK(NEAR(g.spread_m, 0.003, 1e-4) && g.state == PLACE_MOVING, "+/-3 mm reads moving");
    /* the window must be FULL before anything is still: two samples 10 ms apart are not stillness */
    place_gate_init(&g, NULL);
    place_gate_update(&g, 0.0, tgt, tgt);
    CHECK(place_gate_update(&g, 0.01, tgt, tgt) == PLACE_MOVING, "a window that is not full yet reads moving");
    /* a 1 kHz poll must still fill the window (the ring decimates, it does not overflow) */
    place_gate_init(&g, NULL);
    PlaceState s = run_static(&g, tgt, tgt, 0.0, 2.0, 1000.0);
    CHECK(s == PLACE_OK && g.span_s >= 0.45, "a 1 kHz poll still spans the window and opens the gate");
    /* the window forgets: 0.6 s after a jolt the spread is back down */
    place_gate_init(&g, NULL);
    run_static(&g, tgt, tgt, 0.0, 1.0, 100.0);
    const float jolt[3] = { tgt[0] + 0.004f, tgt[1], tgt[2] };
    place_gate_update(&g, 1.01, jolt, tgt);
    CHECK(g.state == PLACE_MOVING, "one sample 4 mm off breaks the stillness");
    run_static(&g, tgt, tgt, 1.02, 0.6, 100.0);
    CHECK(g.spread_m < 1e-6f, "0.6 s later the jolt has left the window");
}

/* The stand walks in from 8 cm off over 2 s (smoothstep) and settles 4 mm off. The gate must stay
 * shut until the stand has been still for the window AND in tolerance for the hold, open once, and
 * stay open; a later 6 mm jolt must shut it again for at least the hold. */
static void test_gate_settle(void) {
    PlaceGate g;
    place_gate_init(&g, NULL);
    const float tgt[3] = { 0.f, 1.448f, 0.f };
    const float start[3] = { 0.06f, 0.03f, -0.05f }, end[3] = { 0.003f, -0.002f, 0.0017f };
    double first_ok = -1.0; int reopened_late = 1, flapped = 0, mean_ok = 0;
    const double dt = 0.01;
    for (int i = 0; i <= 600; ++i) {
        const double t = i * dt;
        double u = t / 2.0; if (u > 1.0) u = 1.0;
        const double s = u * u * (3.0 - 2.0 * u);
        float c[3];
        for (int a = 0; a < 3; ++a) c[a] = tgt[a] + (float)(start[a] + (end[a] - start[a]) * s);
        const PlaceState st = place_gate_update(&g, t, c, tgt);
        if (st == PLACE_OK && first_ok < 0.0) {
            first_ok = t;
            mean_ok = near3(g.mean, end[0], tgt[1] + end[1], end[2], 1e-4);
        }
        if (first_ok >= 0.0 && st != PLACE_OK) flapped = 1;
    }
    printf("placement: gate opened at t = %.2f s (motion ends at 2.00 s)\n", first_ok);
    CHECK(first_ok >= 2.0 + PLACE_HOLD_S, "the gate stays shut until the stand has stopped AND held");
    CHECK(first_ok > 0.0 && first_ok < 3.8, "the gate does open once the stand has settled");
    CHECK(!flapped, "once open, a still stand keeps the gate open");
    CHECK(mean_ok, "the accepted mean is the settled center, not the approach");
    CHECK(NEAR(g.mean_dist_m, 0.0039, 1e-4), "the settled stand is about 4 mm off the target");

    /* the jolt: 6 mm for one sample */
    const double tj = 6.01;
    const float j[3] = { tgt[0] + end[0] + 0.006f, tgt[1] + end[1], tgt[2] + end[2] };
    const float settled[3] = { tgt[0] + end[0], tgt[1] + end[1], tgt[2] + end[2] };
    CHECK(place_gate_update(&g, tj, j, tgt) == PLACE_MOVING, "a 6 mm jolt shuts the gate");
    double reopen = -1.0;
    for (int i = 1; i <= 300; ++i) {
        const double t = tj + i * dt;
        if (place_gate_update(&g, t, settled, tgt) == PLACE_OK && reopen < 0.0) reopen = t;
    }
    reopened_late = reopen >= tj + PLACE_WINDOW_S + PLACE_HOLD_S - 0.02;
    CHECK(reopen > 0.0 && reopened_late, "after a jolt the gate waits a full window plus the hold again");
}

static void test_tolerance_edges(void) {
    PlaceGate g;
    PlaceCfg cfg;
    place_cfg_default(&cfg, 0.010f);
    const float tgt[3] = { 0.5f, 1.2f, -0.3f };
    const float in[3]  = { tgt[0] + 0.0099f, tgt[1], tgt[2] };
    const float out[3] = { tgt[0] + 0.0101f, tgt[1], tgt[2] };
    place_gate_init(&g, &cfg);
    CHECK(run_static(&g, in, tgt, 0.0, 3.0, 100.0) == PLACE_OK, "9.9 mm off a 10 mm tolerance opens");
    place_gate_init(&g, &cfg);
    CHECK(run_static(&g, out, tgt, 0.0, 10.0, 100.0) == PLACE_OFF_TARGET, "10.1 mm off stays off target, forever");
    CHECK(g.held_s == 0.0, "off target accrues no hold");
    /* diagonal: 6 mm on each axis is 10.4 mm in total */
    const float diag[3] = { tgt[0] + 0.006f, tgt[1] + 0.006f, tgt[2] + 0.006f };
    place_gate_init(&g, &cfg);
    CHECK(run_static(&g, diag, tgt, 0.0, 3.0, 100.0) == PLACE_OFF_TARGET, "the tolerance is on the total, not per axis");
    CHECK(NEAR(g.delta[0], 0.006, 1e-6) && NEAR(g.delta[2], 0.006, 1e-6), "the delta is center minus target, per axis");
    /* accept on stillness: half a meter off, tolerance off */
    PlaceCfg loose;
    place_cfg_default(&loose, 0.f);
    const float far_[3] = { tgt[0] + 0.5f, tgt[1], tgt[2] };
    place_gate_init(&g, &loose);
    CHECK(run_static(&g, far_, tgt, 0.0, 3.0, 100.0) == PLACE_OK, "tolerance 0 accepts on stillness alone");
    CHECK(NEAR(g.mean_dist_m, 0.5, 1e-5), "... and still reports how far off it is");
}

static void test_nan_gate(void) {
    PlaceGate g;
    place_gate_init(&g, NULL);
    const float tgt[3] = { 0.f, 1.448f, 0.f };
    CHECK(run_static(&g, tgt, tgt, 0.0, 2.0, 100.0) == PLACE_OK, "a settled stand opens the gate");
    const float cnan[3] = { NAN, 1.448f, 0.f }, cbig[3] = { 0.f, 2e30f, 0.f }, tnan[3] = { 0.f, NAN, 0.f };
    CHECK(place_gate_update(&g, 2.02, cnan, tgt) == PLACE_NO_POSE, "a NaN center is no pose");
    CHECK(g.n == 0 && g.ok_run == 0, "... and drops the history and the hold");
    CHECK(place_gate_update(&g, 2.03, tgt, tgt) == PLACE_MOVING, "the next good pose starts a fresh window");
    run_static(&g, tgt, tgt, 2.04, 2.0, 100.0);
    CHECK(place_gate_update(&g, 4.10, cbig, tgt) == PLACE_NO_POSE, "a finite-but-absurd center is no pose");
    run_static(&g, tgt, tgt, 4.11, 2.0, 100.0);
    CHECK(place_gate_update(&g, 6.2, tgt, tnan) == PLACE_NO_POSE, "a NaN target is no pose");
    run_static(&g, tgt, tgt, 6.21, 2.0, 100.0);
    CHECK(place_gate_update(&g, NAN, tgt, tgt) == PLACE_NO_POSE, "a NaN time is no pose");
    CHECK(place_gate_update(&g, 1.0, NULL, tgt) == PLACE_NO_POSE, "a NULL center is no pose");
    /* a clock that steps backward is not stillness */
    place_gate_init(&g, NULL);
    run_static(&g, tgt, tgt, 10.0, 2.0, 100.0);
    CHECK(g.state == PLACE_OK, "settled");
    CHECK(place_gate_update(&g, 5.0, tgt, tgt) == PLACE_MOVING, "a backward time step drops the window");
    for (int i = 0; i < 40; ++i) CHECK(g.state != PLACE_NO_POSE, "state stays defined");
}

static void test_bump(void) {
    const float taken[3] = { 0.f, 1.448f, 0.f };
    float m = -1.f;
    const float a[3] = { 0.0049f, 1.448f, 0.f }, b[3] = { 0.0051f, 1.448f, 0.f };
    const float c[3] = { 0.003f, 1.452f, 0.f };                   /* 5.0 mm diagonal */
    CHECK(place_bump(taken, a, 0.010f, &m) == 0 && NEAR(m, 0.0049, 1e-6), "4.9 mm on a 10 mm tolerance: in place");
    CHECK(place_bump(taken, b, 0.010f, &m) == 1 && NEAR(m, 0.0051, 1e-6), "5.1 mm on a 10 mm tolerance: bumped");
    CHECK(place_bump(taken, c, 0.0099f, &m) == 1, "the bump is on the total distance");
    CHECK(place_bump(taken, taken, 0.010f, &m) == 0 && m == 0.f, "no move, no bump");
    const float n[3] = { NAN, 0, 0 }, big[3] = { 0, 0, -5e30f };
    CHECK(place_bump(taken, n, 0.010f, NULL) == -1, "a NaN center cannot be judged");
    CHECK(place_bump(big, taken, 0.010f, NULL) == -1, "an absurd taken center cannot be judged");
    CHECK(place_bump(taken, a, 0.f, NULL) == -1, "tolerance 0 cannot be judged");
    CHECK(place_bump(taken, a, NAN, NULL) == -1, "a NaN tolerance cannot be judged");
}

/* ---- the orientation half ----
 * The test builds its quaternions from axis-angle and a product written out here, never through
 * placement.c, so a turn it reads back is checked against a rotation it made itself. */
static void qaa(double ax, double ay, double az, double deg, double q[4]) {
    const double n = sqrt(ax * ax + ay * ay + az * az), h = deg * 3.14159265358979323846 / 360.0;
    q[0] = ax / n * sin(h); q[1] = ay / n * sin(h); q[2] = az / n * sin(h); q[3] = cos(h);
}
static void qmulx(const double a[4], const double b[4], double o[4]) {          /* xyzw, o = a b */
    o[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    o[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    o[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    o[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
}
static void qf(const double q[4], float o[4]) { for (int a = 0; a < 4; ++a) o[a] = (float)q[a]; }
/* the stand the simulator uses: yawed 30 deg about +y, then 1.5 deg off level about x */
static void stand_q(double q[4]) {
    double qy[4], qx[4];
    qaa(0, 1, 0, 30.0, qy); qaa(1, 0, 0, 1.5, qx);
    qmulx(qy, qx, q);
}
/* the stand turned `deg` about a room axis (pre-multiplied: a rotation in the room frame) */
static void stand_turned(double ax, double ay, double az, double deg, float out[4]) {
    double b[4], t[4], r[4];
    stand_q(b); qaa(ax, ay, az, deg, t); qmulx(t, b, r); qf(r, out);
}

static void test_turn(void) {
    float base[4], d = -1.f;
    double b[4]; stand_q(b); qf(b, base);
    CHECK(place_turn_deg(base, base, &d) && NEAR(d, 0.0, 1e-3), "no turn reads 0");
    /* q and -q are one rotation: without the |w| the angle reads 360 */
    const float neg[4] = { -base[0], -base[1], -base[2], -base[3] };
    CHECK(place_turn_deg(base, neg, &d) && NEAR(d, 0.0, 1e-3), "q and -q: no turn (sign-safe)");
    float t2[4];
    stand_turned(0, 1, 0, 2.0, t2);
    CHECK(place_turn_deg(base, t2, &d) && NEAR(d, 2.0, 1e-3), "2 deg about room vertical reads 2 deg");
    const float t2n[4] = { -t2[0], -t2[1], -t2[2], -t2[3] };
    CHECK(place_turn_deg(base, t2n, &d) && NEAR(d, 2.0, 1e-3), "... and against the negated quaternion");
    CHECK(place_turn_deg(t2, base, &d) && NEAR(d, 2.0, 1e-3), "... and in the other order");
    stand_turned(0.3, -0.5, 0.8, 7.5, t2);
    CHECK(place_turn_deg(base, t2, &d) && NEAR(d, 7.5, 1e-3), "7.5 deg about an arbitrary axis");
    stand_turned(1, 0, 0, 0.01, t2);
    CHECK(place_turn_deg(base, t2, &d) && NEAR(d, 0.01, 2e-3), "a hundredth of a degree survives (no acos of ~1)");
    stand_turned(0, 0, 1, 180.0, t2);
    CHECK(place_turn_deg(base, t2, &d) && NEAR(d, 180.0, 1e-2), "a half turn reads 180");
    const float big[4] = { 1.5f * base[0], 1.5f * base[1], 1.5f * base[2], 1.5f * base[3] };
    CHECK(place_turn_deg(base, big, &d) && NEAR(d, 0.0, 1e-3), "a 1.5x quaternion is normalized first");
    /* refusals: the same rules as place_center's q */
    const float qnan[4] = { 0, NAN, 0, 1 }, qinf[4] = { INFINITY, 0, 0, 1 }, qzero[4] = { 0, 0, 0, 0 };
    const float qsmall[4] = { 0, 0, 0, 0.1f }, qbig[4] = { 0, 0, 0, 3.f };
    d = 7.f;
    CHECK(!place_turn_deg(base, qnan, &d) && !place_turn_deg(qinf, base, &d) && !place_turn_deg(base, qzero, &d) &&
          !place_turn_deg(qsmall, base, &d) && !place_turn_deg(base, qbig, &d) && !place_turn_deg(NULL, base, &d),
          "a non-finite, zero, short, long or missing quaternion is refused");
    CHECK(d == 7.f, "a refused turn leaves out untouched");
}

static void test_turn_limits(void) {
    /* bwa_validate: 20 mm at the 1.4 m source radius is atan(10 / 1400) = 0.409 deg */
    CHECK(NEAR(place_turn_limit_deg(0.020f, 1.4f), 0.40925, 1e-4), "validate: 0.41 deg");
    /* the --zylia survey at 2.5 m wants 0.115 deg, under what one Motive frame resolves: the floor */
    CHECK(NEAR(place_turn_limit_deg(0.010f, 2.5f), PLACE_TURN_MIN_DEG, 1e-7), "survey at 2.5 m: the floor");
    CHECK(NEAR(place_turn_limit_deg(0.050f, 1.4f), 1.0230, 1e-3), "a wide tolerance widens it");
    CHECK(NEAR(place_turn_limit_deg(1.0f, 0.05f), PLACE_TURN_MAX_DEG, 1e-7), "capped: never off by size");
    CHECK(NEAR(place_turn_limit_deg(NAN, 1.4f), PLACE_TURN_MIN_DEG, 1e-7) &&
          NEAR(place_turn_limit_deg(0.02f, NAN), PLACE_TURN_MIN_DEG, 1e-7) &&
          NEAR(place_turn_limit_deg(0.02f, 0.f), PLACE_TURN_MIN_DEG, 1e-7) &&
          NEAR(place_turn_limit_deg(0.02f, 3e30f), PLACE_TURN_MIN_DEG, 1e-7) &&
          NEAR(place_turn_limit_deg(-1.f, 1.4f), PLACE_TURN_MIN_DEG, 1e-7), "a broken input gives the strictest limit");
    PlaceCfg c;
    place_cfg_default(&c, 0.020f);
    CHECK(c.still_deg == 0.f, "the default gate is center only");
    place_cfg_direction(&c, place_turn_limit_deg(0.020f, 1.4f));
    CHECK(NEAR(c.still_deg, PLACE_TURN_MIN_DEG, 1e-7), "validate's stillness term floors at 0.3 deg");
    place_cfg_direction(&c, 2.0f);
    CHECK(NEAR(c.still_deg, 1.0, 1e-7), "a 2 deg limit stills at half of it");
    place_cfg_direction(&c, NAN);
    CHECK(NEAR(c.still_deg, PLACE_TURN_MIN_DEG, 1e-7), "a NaN limit stills at the floor");
    /* a hand-filled config gets the same floor */
    PlaceGate g;
    c.still_deg = 0.01f;
    place_gate_init(&g, &c);
    CHECK(NEAR(g.cfg.still_deg, PLACE_TURN_MIN_DEG, 1e-7), "a hand-filled 0.01 deg is floored");
    c.still_deg = NAN;
    place_gate_init(&g, &c);
    CHECK(g.cfg.still_deg == 0.f, "a NaN still_deg is center only, never a NaN comparison");
}

static void test_bump_pose(void) {
    const float c0[3] = { 0.f, 1.448f, 0.f };
    float q0[4], qa[4], qb[4], m = -1.f, tr = -1.f;
    double b[4]; stand_q(b); qf(b, q0);
    stand_turned(0, 1, 0, 0.29, qa);
    stand_turned(0, 1, 0, 0.31, qb);
    const float lim = PLACE_TURN_MIN_DEG;
    CHECK(place_bump_pose(c0, q0, c0, qa, 0.010f, lim, &m, &tr) == 0 && NEAR(tr, 0.29, 2e-3), "0.29 deg on a 0.3 limit: in place");
    CHECK(place_bump_pose(c0, q0, c0, qb, 0.010f, lim, &m, &tr) == PLACE_BUMP_TURNED && NEAR(tr, 0.31, 2e-3),
          "0.31 deg on a 0.3 limit: TURNED, with the center where it was");
    CHECK(m == 0.f, "... and the center did not move");
    /* the twist the simulator uses: 2 deg about the array center. A center-only check cannot see it. */
    float tw[4];
    stand_turned(0, 1, 0, 2.0, tw);
    CHECK(place_bump(c0, c0, 0.010f, NULL) == 0, "a turn about the center: place_bump sees nothing");
    CHECK(place_bump_pose(c0, q0, c0, tw, 0.010f, 0.f, NULL, NULL) == 0, "limit 0 is center only: the twist passes");
    CHECK(place_bump_pose(c0, q0, c0, tw, 0.010f, lim, NULL, &tr) == PLACE_BUMP_TURNED && NEAR(tr, 2.0, 2e-3),
          "a direction check catches the twist");
    const float c6[3] = { 0.006f, 1.448f, 0.f };
    CHECK(place_bump_pose(c0, q0, c6, q0, 0.010f, lim, &m, NULL) == PLACE_BUMP_MOVED, "a 6 mm move: MOVED only");
    CHECK(place_bump_pose(c0, q0, c6, tw, 0.010f, lim, NULL, NULL) == (PLACE_BUMP_MOVED | PLACE_BUMP_TURNED), "both");
    const float qnan[4] = { NAN, 0, 0, 1 };
    CHECK(place_bump_pose(c0, q0, c0, qnan, 0.010f, lim, NULL, NULL) == -1, "a NaN q with a limit cannot be judged");
    CHECK(place_bump_pose(c0, q0, c0, qnan, 0.010f, 0.f, NULL, NULL) == 0, "... and is ignored center only");
    CHECK(place_bump_pose(c0, q0, c6, qnan, 0.010f, lim, NULL, NULL) == PLACE_BUMP_MOVED, "a definite move stands without a q");
    CHECK(place_bump_pose(c0, q0, c0, tw, 0.010f, NAN, NULL, NULL) == -1, "a NaN limit is not center only");
    CHECK(place_bump_pose(c0, q0, c0, tw, 0.010f, 1e30f, NULL, NULL) == 0, "a huge limit caps at 5 deg: 2 deg passes");
    stand_turned(0, 1, 0, 6.0, tw);
    CHECK(place_bump_pose(c0, q0, c0, tw, 0.010f, 1e30f, NULL, NULL) == PLACE_BUMP_TURNED, "... and 6 deg does not");
}

/* the gate's orientation term: a stand still in position but turning about its center */
static void test_gate_turn(void) {
    const float tgt[3] = { 0.f, 1.448f, 0.f };
    PlaceCfg cc, cd;
    place_cfg_default(&cc, 0.010f);
    cd = cc;
    place_cfg_direction(&cd, PLACE_TURN_MIN_DEG);
    PlaceGate gc, gd;
    /* 1. still, with Motive-sized jitter (+/-0.15 deg about two axes) and the quaternion's sign flipping
     *    every other frame: both gates open, and the mean is the stand */
    place_gate_init(&gc, &cc); place_gate_init(&gd, &cd);
    for (int i = 0; i <= 300; ++i) {
        float q[4], qx[4];
        stand_turned(0, 1, 0, 0.15 * sin(0.9 * i), q);
        double t[4], r[4], b[4];
        for (int a = 0; a < 4; ++a) b[a] = q[a];
        qaa(1, 0, 0, 0.15 * cos(1.3 * i), t); qmulx(t, b, r); qf(r, qx);
        if (i & 1) for (int a = 0; a < 4; ++a) qx[a] = -qx[a];
        place_gate_update_q(&gc, i * 0.01, tgt, qx, tgt);
        place_gate_update_q(&gd, i * 0.01, tgt, qx, tgt);
    }
    float base[4], d = 99.f;
    double b[4]; stand_q(b); qf(b, base);
    CHECK(gc.state == PLACE_OK && gd.state == PLACE_OK, "a still stand with jitter opens both gates");
    CHECK(gd.have_q && gd.turn_spread_deg < 0.25f, "the jitter's spread is under the floor");
    CHECK(place_turn_deg(gd.qmean, base, &d) && d < 0.05f, "the window mean is the stand, signs flipping or not");
    printf("placement: jittered stand: spread %.3f deg, mean %.4f deg off the truth\n", gd.turn_spread_deg, d);

    /* 2. the center fixed, the stand turning 2 deg/s about it (a 1 deg sweep across the window, 0.5 deg
     *    of spread): center only opens, a direction gate never */
    place_gate_init(&gc, &cc); place_gate_init(&gd, &cd);
    int ever_d = 0, saw_turning = 0;
    for (int i = 0; i <= 500; ++i) {
        float q[4];
        stand_turned(0, 1, 0, 0.02 * i, q);
        place_gate_update_q(&gc, i * 0.01, tgt, q, tgt);
        if (place_gate_update_q(&gd, i * 0.01, tgt, q, tgt) == PLACE_OK) ever_d = 1;
        if (gd.state == PLACE_MOVING && gd.turning && !strcmp(place_gate_reason(&gd), "turning")) saw_turning = 1;
    }
    CHECK(gc.state == PLACE_OK, "center only: a stand turning about its center opens the gate");
    CHECK(!ever_d && gd.state == PLACE_MOVING, "a direction gate stays shut while the stand turns");
    CHECK(saw_turning, "... and says why: turning");
    CHECK(!strcmp(place_gate_reason(&gc), "OK"), "the center gate's reason is its state");

    /* 3. open, then a 0.5 deg step: shut, and the full window plus the hold again before it reopens */
    place_gate_init(&gd, &cd);
    float q0[4], q1[4];
    stand_turned(0, 1, 0, 0.0, q0);
    stand_turned(0, 1, 0, 0.5, q1);
    for (int i = 0; i <= 200; ++i) place_gate_update_q(&gd, i * 0.01, tgt, q0, tgt);
    CHECK(gd.state == PLACE_OK, "settled");
    CHECK(place_gate_update_q(&gd, 2.01, tgt, q1, tgt) == PLACE_MOVING && gd.turning, "a 0.5 deg step shuts it");
    double reopen = -1.0;
    for (int i = 2; i <= 300; ++i)
        if (place_gate_update_q(&gd, 2.0 + i * 0.01, tgt, q1, tgt) == PLACE_OK && reopen < 0.0) reopen = 2.0 + i * 0.01;
    CHECK(reopen >= 2.01 + PLACE_WINDOW_S + PLACE_HOLD_S - 0.02, "after a turn the gate waits a window plus the hold");
    CHECK(place_turn_deg(gd.qmean, q1, &d) && d < 1e-3f, "and then takes the new orientation");

    /* 4. a direction gate cannot judge a pose with no orientation; a center gate does not need one */
    const float qnan[4] = { 0, NAN, 0, 1 };
    CHECK(place_gate_update_q(&gd, 9.0, tgt, NULL, tgt) == PLACE_NO_POSE && gd.n == 0, "direction: no q is no pose");
    CHECK(place_gate_update_q(&gd, 9.1, tgt, qnan, tgt) == PLACE_NO_POSE, "direction: a NaN q is no pose");
    place_gate_init(&gc, &cc);
    CHECK(run_static(&gc, tgt, tgt, 0.0, 2.0, 100.0) == PLACE_OK && !gc.have_q, "center only: no q, opens, no qmean");
    /* a window with one q-less sample reports no mean rather than a mean of part of it */
    place_gate_update_q(&gc, 2.01, tgt, q0, tgt);
    CHECK(gc.state == PLACE_OK && !gc.have_q, "a partly oriented window has no mean");
}

/* markers on a ring of radius r about `c`, in the plane with normal n (unit), at these angles */
static void ring_markers(const double c[3], const double n[3], double r, const double* deg, int k, float out[][3]) {
    double t[3] = { 1, 0, 0 };
    if (fabs(n[0]) > 0.9) { t[0] = 0; t[1] = 1; }
    double u[3] = { n[1]*t[2] - n[2]*t[1], n[2]*t[0] - n[0]*t[2], n[0]*t[1] - n[1]*t[0] };
    const double un = sqrt(u[0]*u[0] + u[1]*u[1] + u[2]*u[2]);
    for (int a = 0; a < 3; ++a) u[a] /= un;
    const double v[3] = { n[1]*u[2] - n[2]*u[1], n[2]*u[0] - n[0]*u[2], n[0]*u[1] - n[1]*u[0] };
    for (int i = 0; i < k; ++i) {
        const double a = deg[i] * 3.14159265358979323846 / 180.0;
        for (int d = 0; d < 3; ++d) out[i][d] = (float)(c[d] + r * (cos(a) * u[d] + sin(a) * v[d]));
    }
}

static void test_ring(void) {
    PlaceRing r;
    float mk[16][3];
    /* the user's case: 0/90/180/225 deg on a 60 mm ring. The centroid sits 0.19 r = 11.5 mm off the
     * axis; the offset must be the CIRCLE's center, not the centroid. Here the pivot (the frame
     * origin) is the centroid, as Motive puts it by default. */
    const double deg4[4] = { 0, 90, 180, 225 };
    const double up[3] = { 0, 1, 0 }, zero[3] = { 0, 0, 0 };
    ring_markers(zero, up, 0.060, deg4, 4, mk);
    float cen[3] = { 0, 0, 0 };
    for (int i = 0; i < 4; ++i) for (int a = 0; a < 3; ++a) cen[a] += mk[i][a] / 4.f;
    for (int i = 0; i < 4; ++i) for (int a = 0; a < 3; ++a) mk[i][a] -= cen[a];   /* pivot = centroid */
    CHECK(place_ring_fit((const float(*)[3])mk, 4, &r), "the uneven 4-marker ring fits");
    CHECK(near3(r.center, -cen[0], -cen[1], -cen[2], 1e-6), "the offset is the circle's center, NOT the centroid");
    CHECK(NEAR(r.centroid_to_center_m, 0.011481, 5e-5), "centroid to center 11.5 mm on the user's ring");
    CHECK(NEAR(r.radius_m, 0.060, 1e-6) && r.plane_rms_m < 1e-6f && r.circle_rms_m < 1e-6f, "radius 60 mm, exact residuals");
    CHECK(NEAR(r.normal[1], 1.0, 1e-6), "a horizontal ring's normal is +y");
    printf("placement: uneven ring: centroid %.1f mm from the center (60 mm ring, 0/90/180/225 deg)\n",
           r.centroid_to_center_m * 1e3);

    /* tilted, off-origin, three markers only (always an exact circle) */
    const double c3[3] = { 0.02, -0.11, 0.035 }, n3[3] = { 0.2, 0.9591663, -0.2 };
    const double deg3[3] = { 10, 130, 200 };
    ring_markers(c3, n3, 0.045, deg3, 3, mk);
    CHECK(place_ring_fit((const float(*)[3])mk, 3, &r) && near3(r.center, c3[0], c3[1], c3[2], 1e-6), "3 markers on a tilted ring");

    /* noise: 0.3 mm on every coordinate of 6 uneven markers, deterministic */
    const double deg6[6] = { 0, 40, 95, 170, 230, 300 }, c6[3] = { 0.01, 0.0, -0.02 };
    ring_markers(c6, up, 0.060, deg6, 6, mk);
    for (int i = 0; i < 6; ++i) for (int a = 0; a < 3; ++a) mk[i][a] += 0.0003f * (float)sin(1.7 * i + 2.3 * a + 0.4);
    CHECK(place_ring_fit((const float(*)[3])mk, 6, &r), "a noisy ring fits");
    CHECK(near3(r.center, c6[0], c6[1], c6[2], 0.0006), "noise of 0.3 mm moves the center under 0.6 mm");
    CHECK(r.plane_rms_m > 0.f && r.plane_rms_m < 0.0005f && r.circle_rms_m < 0.0005f, "noise shows in the residuals, small");

    /* refusals */
    const float line[4][3] = { { 0, 0, 0 }, { 0.02f, 0, 0 }, { 0.05f, 0, 0 }, { 0.08f, 0.0002f, 0 } };
    CHECK(!place_ring_fit(line, 4, &r) && strstr(r.why, "line") != NULL, "collinear markers are refused");
    CHECK(!place_ring_fit(line, 2, &r), "two markers are refused");
    /* off-plane: a crown, alternate markers 10 mm up and down (a third harmonic no tilt can absorb) */
    ring_markers(zero, up, 0.060, deg6, 6, mk);
    for (int i = 0; i < 6; ++i) mk[i][1] += (i & 1) ? -0.010f : 0.010f;
    CHECK(!place_ring_fit((const float(*)[3])mk, 6, &r) && strstr(r.why, "plane") != NULL, "markers off one plane are refused");
    /* in the plane but not a circle: a 60 x 20 mm ellipse */
    for (int i = 0; i < 6; ++i) {
        const double a = deg6[i] * 3.14159265358979323846 / 180.0;
        mk[i][0] = (float)(0.060 * cos(a)); mk[i][1] = 0.f; mk[i][2] = (float)(0.020 * sin(a));
    }
    CHECK(!place_ring_fit((const float(*)[3])mk, 6, &r) && strstr(r.why, "circle") != NULL, "an ellipse is refused");
    const float bad[3][3] = { { 0, 0, 0 }, { NAN, 0, 0 }, { 0, 0, 0.05f } };
    CHECK(!place_ring_fit(bad, 3, &r), "a NaN marker is refused");
    CHECK(!place_ring_fit(NULL, 3, &r), "NULL markers are refused");
}

/* place_move_words: err = where it IS minus where it SHOULD be, and the words are the opposite move.
 * The expectations are spelled from the room frame by hand (room-right is -x, up is +y, the front wall
 * is +z), never derived from BWA_ROOM_*, so a sign flip in the function cannot cancel out here. */
static void test_move_words(void) {
    char b[160];
    const float dead = PLACE_MOVE_DEAD_BOX_M;                /* 5 mm */
    struct { float e[3]; const char* want; } one[] = {
        { { +0.012f, 0, 0 }, "move 12 mm toward room-right" },  /* sits 12 mm toward +x = room-LEFT of the plan */
        { { -0.012f, 0, 0 }, "move 12 mm toward room-left" },
        { { 0, +0.020f, 0 }, "move 20 mm down" },               /* too high */
        { { 0, -0.020f, 0 }, "move 20 mm up" },
        { { 0, 0, +0.030f }, "move 30 mm toward the back wall" }, /* too far toward the front */
        { { 0, 0, -0.030f }, "move 30 mm toward the front wall" },
    };
    for (int i = 0; i < 6; ++i) {
        const int n = place_move_words(one[i].e, dead, b, sizeof b);
        if (n != 1 || strcmp(b, one[i].want) != 0) printf("  move words %d: got '%s', want '%s'\n", i, b, one[i].want);
        CHECK(n == 1 && strcmp(b, one[i].want) == 0, "move words: each of the six directions alone");
    }
    /* the task's own example, axis order right/left, up/down, front/back */
    const float ex[3] = { -0.012f, +0.004f, -0.030f };
    CHECK(place_move_words(ex, 0.001f, b, sizeof b) == 3 &&
          strcmp(b, "move 12 mm toward room-left, 4 mm down, 30 mm toward the front wall") == 0, "move words: all three axes");
    if (strcmp(b, "move 12 mm toward room-left, 4 mm down, 30 mm toward the front wall") != 0) printf("  got '%s'\n", b);
    /* the dead band: the 4 mm axis drops out at 5 mm, an axis exactly at the band still speaks */
    CHECK(place_move_words(ex, dead, b, sizeof b) == 2 &&
          strcmp(b, "move 12 mm toward room-left, 30 mm toward the front wall") == 0, "move words: the dead band drops a small axis");
    const float at[3] = { 0.005f, 0.f, 0.f };
    CHECK(place_move_words(at, dead, b, sizeof b) == 1 && strcmp(b, "move 5 mm toward room-right") == 0,
          "move words: an axis at the dead band is named");
    const float small[3] = { 0.0049f, -0.003f, 0.001f };
    CHECK(place_move_words(small, dead, b, sizeof b) == 0 && strcmp(b, "no move: every axis within 5 mm") == 0,
          "move words: everything inside the dead band says so");
    /* the Placement panel's fine band: under half a millimeter never prints as 0 mm */
    const float tiny[3] = { 0.0004f, 0.f, 0.f };
    CHECK(place_move_words(tiny, 0.f, b, sizeof b) == 0, "move words: a move that rounds to 0 mm is not named");
    /* refusals and truncation */
    const float bad[3] = { NAN, 0.f, 0.f };
    CHECK(place_move_words(bad, dead, b, sizeof b) == -1 && strcmp(b, "n/a") == 0, "move words: NaN is refused");
    char s8[8];
    place_move_words(ex, 0.001f, s8, sizeof s8);
    CHECK(strlen(s8) == 7, "move words: a short buffer truncates and stays terminated");
}

int main(void) {
    test_move_words();
    test_center();
    test_ring();
    test_refusals();
    test_angles();
    test_cfg();
    test_stillness();
    test_gate_settle();
    test_tolerance_edges();
    test_nan_gate();
    test_bump();
    test_turn();
    test_turn_limits();
    test_bump_pose();
    test_gate_turn();
    CHECK(strcmp(place_state_name(PLACE_OK), "OK") == 0 && strcmp(place_state_name((PlaceState)99), "no pose") == 0,
          "state names");
    if (fails) { printf("placement_test: %d FAILURES\n", fails); return 1; }
    printf("placement_test OK (center under rotation + offset, ring fit, refusals, mount angles, defaults, stillness, "
           "gate settle + jolt, tolerance edges, NaN gate, bump, turn angle, turn limits, orientation bump, "
           "orientation gate, move words)\n");
    return 0;
}
