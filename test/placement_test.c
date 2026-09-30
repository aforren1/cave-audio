/*
 * placement_test.c - the tracked ZM-1's placement math (src/calib/placement.c): the acoustic center
 * from a pose plus the mount offset against hand-computed rotations, the marker-ring offset fit
 * (uneven spacing, noise, and the collinear and off-plane refusals), the mount angles, the default
 * thresholds, the stillness spread, the gate holding until the stand has settled (and dropping on a
 * jolt), the tolerance edges, accept-on-stillness, the bump check, and the refusal of every
 * non-finite or absurd input. The tools on top (bwa_calibrate --track, calib_view's Placement panel)
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

int main(void) {
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
    CHECK(strcmp(place_state_name(PLACE_OK), "OK") == 0 && strcmp(place_state_name((PlaceState)99), "no pose") == 0,
          "state names");
    if (fails) { printf("placement_test: %d FAILURES\n", fails); return 1; }
    printf("placement_test OK (center under rotation + offset, ring fit, refusals, mount angles, defaults, stillness, "
           "gate settle + jolt, tolerance edges, NaN gate, bump)\n");
    return 0;
}
