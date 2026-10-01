/*
 * mic_track.cpp - the tracked ZM-1 (mic_track.h). The math is placement.c's; this is the tracker, the
 * survey, the simulated source and the console loop around it.
 */
#include "mic_track.h"

#include <chrono>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#ifdef _WIN32
#include <conio.h>         /* _kbhit/_getch: a key takes the reading */
#endif

static void set_err(char* err, size_t cap, const char* m) { if (err && cap) snprintf(err, cap, "%s", m); }

static long long wall_ns(void) {
    return (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

/* ---- --mount-offset ring ----
 * The body's marker offsets come from Motive's model definition (natnet_parse_modeldef, in the body
 * frame), and the ring fit is placement.c's. The simulated source writes its own MODELDEF, a body
 * whose markers sit UNEVENLY on a 60 mm ring (0/90/180/225 deg) about the array center at the
 * center's height, with Motive's default pivot, the marker centroid, 11.5 mm off the ring's axis. It
 * goes through the same parser, and the simulated stand keeps the ring's true center as its offset,
 * so a tool that took the centroid (a zero offset) lands 11.5 mm off the truth. */
#define MIC_SIM_BODY_ID 7
#define MIC_SIM_RING_R  0.060
static uint8_t g_mdbuf[65536];
static NatNetBodyDesc g_desc[64];

struct MdW { uint8_t* b; size_t n, cap; };
static void mdw(MdW* w, const void* p, size_t k) { if (w->n + k <= w->cap) memcpy(w->b + w->n, p, k); w->n += k; }
static void mdi(MdW* w, int32_t v) { mdw(w, &v, 4); }
static void mdf(MdW* w, float v)   { mdw(w, &v, 4); }
static void mds(MdW* w, const char* s) { mdw(w, s, strlen(s) + 1); }

/* NatNet 4.1 (size-prefixed descriptions, no rotation offset): one rigid body. true_off gets the
 * ring center relative to the pivot, which is what the tool should recover. */
static int sim_modeldef(uint8_t* buf, size_t cap, float true_off[3]) {
    const double deg[4] = { 0.0, 90.0, 180.0, 225.0 };
    double mk[4][3], cen[3] = { 0.0, 0.0, 0.0 };
    for (int i = 0; i < 4; ++i) {                      /* the ring: body x-z plane, the array center's height */
        const double a = deg[i] * 3.14159265358979323846 / 180.0;
        mk[i][0] = MIC_SIM_RING_R * cos(a); mk[i][1] = 0.0; mk[i][2] = MIC_SIM_RING_R * sin(a);
        for (int k = 0; k < 3; ++k) cen[k] += mk[i][k] / 4.0;
    }
    for (int k = 0; k < 3; ++k) true_off[k] = (float)-cen[k];    /* pivot (centroid) -> ring center */
    static uint8_t body[1024];
    MdW d = { body, 0, sizeof body };
    mds(&d, "zm1_stand");
    mdi(&d, MIC_SIM_BODY_ID); mdi(&d, -1);
    mdf(&d, 0.f); mdf(&d, 0.f); mdf(&d, 0.f);
    mdi(&d, 4);
    for (int i = 0; i < 4; ++i) for (int k = 0; k < 3; ++k) mdf(&d, (float)(mk[i][k] - cen[k]));
    for (int i = 0; i < 4; ++i) mdi(&d, 0);
    for (int i = 0; i < 4; ++i) { char nm[16]; snprintf(nm, sizeof nm, "Marker%d", i + 1); mds(&d, nm); }
    MdW w = { buf, 0, cap };
    mdi(&w, 1);                                        /* one description */
    mdi(&w, 1);                                        /* type 1: rigid body */
    mdi(&w, (int32_t)d.n);
    mdw(&w, body, d.n);
    return (d.n <= sizeof body && w.n <= cap) ? (int)w.n : -1;
}

static int ring_offset(MicTrack* T, const MicTrackCfg* c, char* err, size_t errcap) {
    int major = 4, minor = 1, len = -1;
    char e[192] = { 0 }, m[400];
    long want_id = -1;
    const char* want_name = NULL;
    if (c->sim) {
        len = sim_modeldef(g_mdbuf, sizeof g_mdbuf, T->sim_true_off);
        want_id = MIC_SIM_BODY_ID;
    } else {
        if (!c->server) {
            set_err(err, errcap, "--mount-offset ring reads the body's markers from Motive's model definitions,\n"
                                 "which needs --natnet-server <ip>");
            return 2;
        }
        char* endp = NULL;
        want_id = strtol(c->body ? c->body : "", &endp, 10);
        if (!(c->body && c->body[0] && endp && *endp == 0)) { want_id = -1; want_name = c->body; }
        NatNetConfig nc;
        memset(&nc, 0, sizeof nc);
        nc.multicast = c->multicast ? c->multicast : "239.255.42.99";
        nc.server = c->server;
        nc.data_port = 1511; nc.command_port = 1510;
        NatNetRaw* r = natnet_raw_open(&nc, e, sizeof e);
        if (!r) { snprintf(m, sizeof m, "tracker: %s", e); set_err(err, errcap, m); return 1; }
        natnet_raw_version(r, &major, &minor);
        len = natnet_raw_modeldef(r, g_mdbuf, sizeof g_mdbuf);
        natnet_raw_close(r);
        if (len < 0) {
            snprintf(m, sizeof m, "--mount-offset ring: no model definitions from %s (is Motive streaming?)", c->server);
            set_err(err, errcap, m); return 1;
        }
    }
    bool complete = false;
    const int nd = natnet_parse_modeldef(g_mdbuf, (size_t)(len > 0 ? len : 0), major, minor, g_desc, 64, &complete);
    const NatNetBodyDesc* bd = NULL;
    for (int i = 0; i < nd && !bd; ++i)
        if (want_name ? !strcmp(g_desc[i].name, want_name) : g_desc[i].id == (int32_t)want_id) bd = &g_desc[i];
    if (!bd) {
        snprintf(m, sizeof m, "--mount-offset ring: rigid body '%s' is not in Motive's model definitions (%d bodies%s)",
                 c->sim ? "zm1_stand" : c->body, nd < 0 ? 0 : nd, complete ? "" : ", the walk stopped early");
        set_err(err, errcap, m); return 1;
    }
    if (!place_ring_fit((const float(*)[3])bd->markers, bd->n_stored, &T->ring)) {
        snprintf(m, sizeof m, "--mount-offset ring: %s (%d markers, plane RMS %.1f mm, circle RMS %.1f mm)",
                 T->ring.why, bd->n_stored, T->ring.plane_rms_m * 1e3, T->ring.circle_rms_m * 1e3);
        set_err(err, errcap, m); return 2;
    }
    memcpy(T->offset, T->ring.center, sizeof T->offset);
    T->offset_src = MIC_OFFSET_RING;
    return 0;
}

int mic_track_open(MicTrack* T, const MicTrackCfg* c, char* err, size_t errcap) {
    T->nn = NULL;
    T->sim = c->sim; T->sim_bump_after = c->sim_bump_after; T->sim_twist_after = c->sim_twist_after;
    T->virtual_clock = c->virtual_clock && c->sim;
    T->have_survey = 0; T->body_frame = 0; T->offset_src = MIC_OFFSET_ZERO;
    memset(&T->mount, 0, sizeof T->mount);
    memset(T->offset, 0, sizeof T->offset);
    snprintf(T->body, sizeof T->body, "%s", c->body ? c->body : "");
    T->vt = 0.0; T->t0_ns = wall_ns();
    T->ncap.store(0);
    T->sim_target[0] = 0.f; T->sim_target[1] = 1.448f; T->sim_target[2] = 0.f;
    T->sim_t0 = 0.0;
    T->sim_have_prev = 0; T->sim_ntarget = 0;
    char e[192] = { 0 };

    int off_ok = 1;
    for (int a = 0; a < 3; ++a)
        if (!(c->offset_m[a] >= -PLACE_MAX_OFFSET_M && c->offset_m[a] <= PLACE_MAX_OFFSET_M)) off_ok = 0;   /* NaN fails too */
    if (c->have_offset && !off_ok) {
        set_err(err, errcap, "--mount-offset wants three finite numbers within 10 m (meters, the rigid body's axes)");
        return 2;
    }
    if (c->survey_path) {
        if (!zylia_survey_load(c->survey_path, &T->mount, e, sizeof e)) {
            char m[256]; snprintf(m, sizeof m, "survey: %s", e); set_err(err, errcap, m); return 2;
        }
        T->have_survey = 1;
        T->body_frame = T->mount.body_frame;
    } else if (c->sim && c->sim_builtin_body) {
        /* the self-check stands the built-in table in as the body frame: it is about whether the
         * placement hook is WIRED, not about the loader, and needs no committed fixture */
        zylia_set_capsules(NULL);
        T->mount.body_frame = 1;
        T->body_frame = 1;
    }
    if (c->need_body_frame && !T->body_frame) {
        if (!c->survey_path)
            set_err(err, errcap, "--track here needs --survey <body-frame survey>: the pose gives the mount's "
                                 "orientation, but only a survey knows how the capsules sit inside it");
        else {
            char m[320];
            snprintf(m, sizeof m, "survey %s is in ROOM axes, not the mount's body frame - it is tied to one "
                                  "orientation, so it cannot follow a moving stand. Re-save it with a mount.", c->survey_path);
            set_err(err, errcap, m);
        }
        return 2;
    }
    if (T->body_frame) zylia_capsules(T->caps_body);           /* the loaded table, still in body axes */
    if (T->body_frame && T->mount.have_offset) {
        if (c->have_offset) {
            set_err(err, errcap, "the body-frame survey carries its own mount offset; drop --mount-offset");
            return 2;
        }
        memcpy(T->offset, T->mount.offset_m, sizeof T->offset);
        T->offset_src = MIC_OFFSET_SURVEY;
    } else if (c->have_offset) {
        memcpy(T->offset, c->offset_m, sizeof T->offset);
        T->offset_src = MIC_OFFSET_FLAG;
    }
    memcpy(T->sim_true_off, T->offset, sizeof T->offset);    /* the user's offset is the truth, except... */
    if (c->offset_ring) {
        if (T->offset_src == MIC_OFFSET_SURVEY) {
            set_err(err, errcap, "the body-frame survey carries its own mount offset; drop --mount-offset ring");
            return 2;
        }
        const int rc = ring_offset(T, c, err, errcap);      /* ...the ring's, whose truth the sim knows */
        if (rc) return rc;
    }
    if (c->sim && c->sim_have_true_offset) {                /* ...or a deliberately different one */
        for (int a = 0; a < 3; ++a)
            if (!(c->sim_true_offset_m[a] >= -PLACE_MAX_OFFSET_M && c->sim_true_offset_m[a] <= PLACE_MAX_OFFSET_M)) {
                set_err(err, errcap, "--track-sim-offset wants three finite numbers within 10 m (meters, body axes)");
                return 2;
            }
        memcpy(T->sim_true_off, c->sim_true_offset_m, sizeof T->sim_true_off);
    }

    if (c->sim) return 0;
    if (!c->body || !c->body[0]) { set_err(err, errcap, "--track wants a rigid-body streaming id or name"); return 2; }
    NatNetConfig nc;
    memset(&nc, 0, sizeof nc);
    nc.multicast = c->multicast ? c->multicast : "239.255.42.99";
    nc.server = c->server;
    nc.data_port = 1511; nc.command_port = 1510;
    char* endp = NULL;
    long id = strtol(c->body, &endp, 10);
    if (endp && *endp == 0) nc.rigid_body = (int32_t)id;      /* numeric => streaming id */
    else {
        if (!c->server) {
            set_err(err, errcap, "--track by NAME needs --natnet-server <ip>: the streaming id is resolved from "
                                 "Motive's model definitions. Or pass the id directly.");
            return 2;
        }
        nc.rigid_body_name = c->body;
    }
    T->nn = natnet_open(&nc, e, sizeof e);
    if (!T->nn) { char m[256]; snprintf(m, sizeof m, "tracker: %s", e); set_err(err, errcap, m); return 1; }
    return 0;
}

void mic_track_close(MicTrack* T) {
    if (T->nn) natnet_close(T->nn);
    T->nn = NULL;
}

void mic_track_describe(const MicTrack* T, char* buf, size_t cap) {
    const float* o = T->offset;
    const int nonzero = o[0] != 0.f || o[1] != 0.f || o[2] != 0.f;
    const char* table = T->body_frame ? (T->have_survey ? "body-frame survey: the capsule table follows the stand"
                                                        : "built-in capsule table standing in as the body frame")
                      : (T->have_survey ? "room-axes survey: installed for its channel order and geometry, NOT re-aimed"
                                        : "no survey: the built-in capsule table, NOT re-aimed");
    if (T->offset_src == MIC_OFFSET_RING)
        snprintf(buf, cap, "mount offset (%.4f %.4f %.4f) m in body axes, the center of the %d-marker ring: %.1f mm from\n"
                           "the marker centroid, radius %.1f mm, plane RMS %.2f mm, circle RMS %.2f mm. It assumes the ring\n"
                           "sits at the array center's height (the capsule sphere's equator): a vertical error in the\n"
                           "ring passes straight into the center\n%s",
                 o[0], o[1], o[2], T->ring.n, T->ring.centroid_to_center_m * 1e3, T->ring.radius_m * 1e3,
                 T->ring.plane_rms_m * 1e3, T->ring.circle_rms_m * 1e3, table);
    else if (T->offset_src == MIC_OFFSET_SURVEY)
        snprintf(buf, cap, "mount offset (%.4f %.4f %.4f) m in body axes, from the survey\n%s", o[0], o[1], o[2], table);
    else if (T->offset_src == MIC_OFFSET_FLAG && nonzero)
        snprintf(buf, cap, "mount offset (%.4f %.4f %.4f) m in body axes, from --mount-offset. With no body-frame survey\n"
                           "the center depends on how Motive orients the rigid body (not on which way the ZM-1 faces)\n%s",
                 o[0], o[1], o[2], table);
    else
        snprintf(buf, cap, "mount offset 0: the rigid body's pivot is taken as the array center (move the pivot\n"
                           "there in Motive, or pass --mount-offset x,y,z)\n%s", table);
}

double mic_track_now(MicTrack* T) {
    if (T->virtual_clock) return T->vt;
    return (double)(wall_ns() - T->t0_ns) * 1e-9;
}

void mic_track_advance(MicTrack* T, double s) { if (T->virtual_clock && s > 0.0) T->vt += s; }

void mic_track_note_capture(MicTrack* T, double capture_s) {
    T->ncap.fetch_add(1);
    mic_track_advance(T, capture_s);
}

static void sim_center(MicTrack* T, double t, float c[3]);

void mic_track_sim_target(MicTrack* T, const float target[3]) {
    if (T->sim != MIC_SIM_SCRIPT) return;
    if (T->sim_ntarget++ > 0) {
        /* a retarget after a placement: the stand stays at its last settled spot for a while */
        sim_center(T, mic_track_now(T), T->sim_prev);
        T->sim_have_prev = 1;
    }
    memcpy(T->sim_target, target, sizeof T->sim_target);
    T->sim_t0 = mic_track_now(T);
}

/* ---- the simulated source ----
 * Its truth is built here, from a quaternion and a vector rotation written out on their own
 * (v' = v + 2w (u x v) + 2 u x (u x v)), NOT through placement.c's matrix. The stand pose it
 * reports is p = truth - R(q) . sim_true_off (the user's offset, or the simulated ring's own
 * center), and the tool gets its center back through place_center with the offset IT settled on;
 * a tool that ignored R, rotated the wrong way, or took the ring's centroid lands off the truth. */
static void qmul(const double a[4], const double b[4], double o[4]) {          /* xyzw */
    o[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    o[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    o[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    o[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
}
static void qrotv(const double q[4], const float v[3], double o[3]) {
    const double u[3] = { q[0], q[1], q[2] }, w = q[3];
    const double t[3] = { 2.0 * (u[1]*v[2] - u[2]*v[1]), 2.0 * (u[2]*v[0] - u[0]*v[2]), 2.0 * (u[0]*v[1] - u[1]*v[0]) };
    o[0] = v[0] + w * t[0] + (u[1]*t[2] - u[2]*t[1]);
    o[1] = v[1] + w * t[1] + (u[2]*t[0] - u[0]*t[2]);
    o[2] = v[2] + w * t[2] + (u[0]*t[1] - u[1]*t[0]);
}

int mic_track_sim_twisted(MicTrack* T) {
    return T->sim == MIC_SIM_SCRIPT && T->sim_twist_after > 0 && T->ncap.load() >= T->sim_twist_after;
}

/* the scripted stand's orientation: yawed 30 deg about +y, then 1.5 deg off level about x. In front of
 * that, in the ROOM frame (pre-multiplied), a wobble of MIC_SIM_WOBBLE_DEG about room vertical, and once
 * twisted, MIC_SIM_TWIST_DEG more about the same axis. The stand's pose is derived from its center (see
 * mic_track_read), so every turn here is about the array center and moves no center. */
static void sim_q(MicTrack* T, double t, double q[4]) {
    const double D = 3.14159265358979323846 / 360.0;             /* half-angle per degree */
    if (T->sim == MIC_SIM_FIXED) { q[0] = 0.0; q[1] = sin(45.0 * D); q[2] = 0.0; q[3] = cos(45.0 * D); return; }
    const double qy[4] = { 0.0, sin(30.0 * D), 0.0, cos(30.0 * D) }, qx[4] = { sin(1.5 * D), 0.0, 0.0, cos(1.5 * D) };
    double base[4];
    qmul(qy, qx, base);
    const double turn = MIC_SIM_WOBBLE_DEG * sin(2.0 * 3.14159265358979 * 3.7 * t) +
                        (mic_track_sim_twisted(T) ? MIC_SIM_TWIST_DEG : 0.0);
    const double qt[4] = { 0.0, sin(turn * D), 0.0, cos(turn * D) };
    qmul(qt, base, q);
}

static void sim_center(MicTrack* T, double t, float c[3]) {
    if (T->sim == MIC_SIM_FIXED) {                     /* bwa_validate's constant stand pose */
        const float p[3] = { 0.05f, 0.02f, -0.03f };
        double q[4], r[3];
        sim_q(T, t, q);
        qrotv(q, T->sim_true_off, r);
        for (int a = 0; a < 3; ++a) c[a] = (float)(p[a] + r[a]);
        return;
    }
    static const float START[3] = { 0.06f, 0.03f, -0.05f };    /* 8.4 cm off */
    static const float SETTLE[3] = { 0.003f, -0.002f, 0.004f };/* 5.4 mm off */
    double dt = t - T->sim_t0;
    if (T->sim_have_prev) dt -= MIC_SIM_LINGER_S;
    if (dt < 0.0) {                                    /* still standing at the previous placement */
        memcpy(c, T->sim_prev, sizeof T->sim_prev);
    } else {
        double u = dt / 2.0;
        if (u > 1.0) u = 1.0;
        const double s = u * u * (3.0 - 2.0 * u);
        for (int a = 0; a < 3; ++a) c[a] = T->sim_target[a] + (float)(START[a] + (SETTLE[a] - START[a]) * s);
    }
    c[0] += (float)(0.00015 * sin(2.0 * 3.14159265358979 * 7.3 * t));          /* the tracker's jitter */
    c[2] += (float)(0.00010 * sin(2.0 * 3.14159265358979 * 5.1 * t + 1.0));
    if (T->sim_bump_after > 0 && T->ncap.load() >= T->sim_bump_after) {        /* knocked 15 mm */
        c[0] += 0.8f * MIC_SIM_BUMP_M;
        c[2] -= 0.6f * MIC_SIM_BUMP_M;
    }
}

int mic_track_sim_truth(MicTrack* T, float center[3]) {
    if (!T->sim) return 0;
    sim_center(T, mic_track_now(T), center);
    return 1;
}

int mic_track_sim_orientation(MicTrack* T, float q[4]) {
    if (!T->sim) return 0;
    double d[4];
    sim_q(T, mic_track_now(T), d);
    for (int a = 0; a < 4; ++a) q[a] = (float)d[a];
    return 1;
}

int mic_track_read(MicTrack* T, MicPose* out) {
    if (T->sim) {
        float c[3];
        double q[4], r[3];
        const double t = mic_track_now(T);
        sim_center(T, t, c);
        sim_q(T, t, q);
        qrotv(q, T->sim_true_off, r);
        for (int a = 0; a < 3; ++a) out->p[a] = (float)(c[a] - r[a]);
        for (int a = 0; a < 4; ++a) out->q[a] = (float)q[a];
    } else {
        /* Reading the pose alone is NOT enough. It returns the last PUBLISHED pose forever, and natnet
         * only publishes tracking-valid frames, so an occluded stand or a wrong streaming id would hand
         * back an OLD pose (the previous placement's) and be accepted as this one's measurement. Gate
         * on liveness, which is what natnet_status is for. */
        if (!(natnet_status(T->nn) == NN_STATUS_LIVE && natnet_read_pose(T->nn, out->p, out->q))) return 0;
    }
    return place_center(out->p, out->q, T->offset, out->center);
}

int mic_track_aim_capsules(const MicTrack* T, const float q[4]) {
    if (!T->body_frame) return 0;
    float R[9], caps_room[ZYLIA_MICS][3];
    zylia_quat_to_matrix(q, R);
    zylia_capsules_rotate(T->caps_body, R, 0, caps_room);
    zylia_set_capsules(caps_room);                     /* the array as it is turned RIGHT NOW */
    return 1;
}

/* ---- console ---- */

static void sleep_poll(MicTrack* T) {
    if (T->virtual_clock) mic_track_advance(T, 0.02);          /* 50 Hz of simulated time, no waiting */
    else std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

static int key_hit(void) {
#ifdef _WIN32
    if (_kbhit()) { (void)_getch(); return 1; }
#endif
    return 0;
}

int mic_track_place_console(MicTrack* T, const float target[3], const PlaceCfg* cfg, double timeout_s,
                            int keys, const char* what, MicPlaced* out) {
    PlaceGate g;
    place_gate_init(&g, cfg);
    const int dir = g.cfg.still_deg > 0.f;                    /* a direction mode: the orientation counts */
    mic_track_sim_target(T, target);
    const double t0 = mic_track_now(T);
    double last_print = -1.0;
    MicPose ps; int have = 0;
    char tol[48];
    if (g.cfg.tol_m > 0.f) snprintf(tol, sizeof tol, "%.1f mm", g.cfg.tol_m * 1e3);
    else                   snprintf(tol, sizeof tol, "none (accept on stillness)");
    char turn[64] = "";
    if (dir) snprintf(turn, sizeof turn, " and turning under %.2f deg", g.cfg.still_deg);
    printf("placement: %s: target (%.3f %.3f %.3f), tolerance %s, still within %.1f mm%s for %.1f s, then hold %.1f s\n",
           what, target[0], target[1], target[2], tol, g.cfg.still_m * 1e3, turn, g.cfg.window_s, g.cfg.hold_s);
    if (keys) printf("placement:   press a key to take the current reading anyway\n");
    for (;;) {
        const double t = mic_track_now(T);
        have = mic_track_read(T, &ps);
        const PlaceState st = place_gate_update_q(&g, t, have ? ps.center : NULL, have ? ps.q : NULL, target);
        if (last_print < 0.0 || t - last_print >= 0.1 || st == PLACE_OK) {
            last_print = t;
            if (st == PLACE_NO_POSE)
                printf("\r  no live pose (occluded, wrong id, or Motive not streaming)  HOLD                                                ");
            else {
                /* a direction mode adds the mount's yaw: the one number a turn about the center changes */
                char yaw[24] = "";
                float y = 0.f;
                if (dir && place_mount_angles(ps.q, &y, NULL)) snprintf(yaw, sizeof yaw, "  yaw %+6.1f", y);
                printf("\r  center (%+.3f %+.3f %+.3f)  target (%+.3f %+.3f %+.3f)  dx %+6.1f dy %+6.1f dz %+6.1f mm  |d| %5.1f mm%s  %s %-10s",
                       g.center[0], g.center[1], g.center[2], target[0], target[1], target[2],
                       g.delta[0] * 1e3, g.delta[1] * 1e3, g.delta[2] * 1e3, g.dist_m * 1e3, yaw,
                       st == PLACE_OK ? "OK  " : "HOLD", st == PLACE_OK ? "" : place_gate_reason(&g));
            }
            fflush(stdout);
        }
        int forced = 0;
        if (st != PLACE_OK && keys && key_hit()) {
            if (st == PLACE_NO_POSE) {
                printf("\nplacement: aborted: a key with no live pose to take\n");
                return 1;
            }
            forced = 1;
        }
        if (st == PLACE_OK || forced) {
            /* forced before the window filled: the latest center is all there is */
            const float* c = (g.n >= 3) ? g.mean : g.center;
            memcpy(out->center, c, sizeof out->center);
            /* the window's mean orientation: one pose carries Motive's single-frame jitter */
            memcpy(out->q, g.have_q ? g.qmean : ps.q, sizeof out->q);
            double d2 = 0.0;
            for (int a = 0; a < 3; ++a) { const double d = (double)c[a] - target[a]; d2 += d * d; }
            out->dist_m = (float)sqrt(d2);
            out->forced = forced;
            if (!place_mount_angles(out->q, &out->yaw_deg, &out->tilt_deg)) out->yaw_deg = out->tilt_deg = 0.f;
            printf("\n");
            if (forced)
                printf("placement: WARNING: taken by a key while the gate read %s (spread %.1f mm, %.1f mm off):\n"
                       "           the measurement uses this reading, not a settled one\n",
                       place_gate_reason(&g), g.spread_m * 1e3, out->dist_m * 1e3);
            const int aimed = mic_track_aim_capsules(T, out->q);
            printf("placement: measured center (%.4f %.4f %.4f), %.1f mm from the target; mount yaw %.1f deg, tilt %.1f deg%s\n",
                   out->center[0], out->center[1], out->center[2], out->dist_m * 1e3, out->yaw_deg, out->tilt_deg,
                   aimed ? "; capsule table re-aimed" : "");
            float tru[3];
            if (mic_track_sim_truth(T, tru))
                printf("track-sim: true center (%.4f %.4f %.4f)\n", tru[0], tru[1], tru[2]);
            return 0;
        }
        if (t - t0 > timeout_s) {
            printf("\nplacement: timed out after %.0f s (%s): the ZM-1 never sat %s and still for %.1f s.\n"
                   "           Check the rigid body, the target and the tolerance (--place-tol-mm), or raise\n"
                   "           --place-timeout.\n",
                   timeout_s, place_state_name(st), g.cfg.tol_m > 0.f ? "inside the tolerance" : "", g.cfg.hold_s);
            return 1;
        }
        sleep_poll(T);
    }
}

int mic_track_bump_console(MicTrack* T, const float taken[3], const float q_taken[4], float tol_m, float turn_limit_deg,
                           double capture_s, float* moved_m, float* turned_deg, float now_center[3]) {
    mic_track_note_capture(T, capture_s);
    MicPose ps;
    for (int tries = 0; tries < 50; ++tries) {          /* a live tracker: up to 0.5 s for a fresh pose */
        if (mic_track_read(T, &ps)) {
            if (now_center) memcpy(now_center, ps.center, sizeof ps.center);
            /* the turn is read in every mode (a report may show it); only a direction mode judges it */
            if (q_taken && turned_deg) place_turn_deg(q_taken, ps.q, turned_deg);
            return place_bump_pose(taken, q_taken, ps.center, ps.q, tol_m, q_taken ? turn_limit_deg : 0.f,
                                   moved_m, turned_deg);
        }
        if (T->sim) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return -1;
}
