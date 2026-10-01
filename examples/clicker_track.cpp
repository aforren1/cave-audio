/*
 * clicker_track.cpp - the tracked clicker (clicker_track.h): a NatNet session for one more rigid body,
 * a pose ring, the stillness take, and the simulated clicker.
 */
#include "clicker_track.h"

extern "C" {
#include "os/os.h"
}
#include <chrono>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const double PI = 3.14159265358979323846;

/* os_monotonic_ns, the function the ZM-1 capture callback stamps each clap's onset with
 * (zylia_capture.h), so the two halves share a clock by construction rather than because MSVC's
 * steady_clock happens to be QPC too */
double clicker_clock_s(void) { return (double)os_monotonic_ns() * 1e-9; }

void clicker_sim_tip(float out[3]) {
    out[0] = CLICKER_SIM_TIP_X; out[1] = CLICKER_SIM_TIP_Y; out[2] = CLICKER_SIM_TIP_Z;
}

/* ---- the simulated clicker's own quaternion code (xyzw), apart from placement.c on purpose ---- */
static void qmul(const double a[4], const double b[4], double o[4]) {
    o[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    o[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    o[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    o[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
}
static void qaxis(double ax, double ay, double az, double deg, double o[4]) {
    const double h = deg * PI / 360.0, n = sqrt(ax*ax + ay*ay + az*az);
    o[0] = sin(h) * ax / n; o[1] = sin(h) * ay / n; o[2] = sin(h) * az / n; o[3] = cos(h);
}
/* v' = v + 2w (u x v) + 2 u x (u x v) */
static void qrotv(const double q[4], const double v[3], double o[3]) {
    const double u[3] = { q[0], q[1], q[2] }, w = q[3];
    const double t[3] = { 2.0 * (u[1]*v[2] - u[2]*v[1]), 2.0 * (u[2]*v[0] - u[0]*v[2]), 2.0 * (u[0]*v[1] - u[1]*v[0]) };
    o[0] = v[0] + w * t[0] + (u[1]*t[2] - u[2]*t[1]);
    o[1] = v[1] + w * t[1] + (u[2]*t[0] - u[0]*t[2]);
    o[2] = v[2] + w * t[2] + (u[0]*t[1] - u[1]*t[0]);
}
static void qnlerp(const double a[4], const double b[4], double s, double o[4]) {
    const double d = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
    const double sg = d < 0.0 ? -1.0 : 1.0;
    double n = 0.0;
    for (int i = 0; i < 4; ++i) { o[i] = a[i] * (1.0 - s) + sg * b[i] * s; n += o[i] * o[i]; }
    n = sqrt(n);
    for (int i = 0; i < 4; ++i) o[i] /= n;
}

/* The legs, relative to the anchor (the true array center). SPREAD: ten spots high and low around the
 * array, 1.5 to 2.3 m out, heights from 1 m below to 1 m above it, so no plane holds them. RING: eight at
 * the anchor's own height, the coplanar set the survey must refuse. */
static int sim_legs(int set, float rel[][3]) {
    static const float SPREAD[10][3] = {    /* azimuth (deg), horizontal range (m), height (m) */
        {   0.f, 2.0f,  0.9f }, {  50.f, 1.8f, -1.0f }, { 100.f, 2.2f,  0.3f }, { 150.f, 1.9f, -0.6f },
        { 200.f, 2.1f,  0.8f }, { 250.f, 1.7f, -0.9f }, { 300.f, 2.3f,  0.1f }, { 340.f, 1.6f, -0.3f },
        {  75.f, 1.5f,  1.0f }, { 220.f, 2.0f, -0.2f } };
    const int n = set == CLICKER_SIM_RING ? 8 : 10;
    for (int k = 0; k < n; ++k) {
        const float az = set == CLICKER_SIM_RING ? 45.f * (float)k + 10.f : SPREAD[k][0];
        const float r  = set == CLICKER_SIM_RING ? 2.0f : SPREAD[k][1];
        const float h  = set == CLICKER_SIM_RING ? 0.0f : SPREAD[k][2];
        rel[k][0] = -r * sinf(az * (float)PI / 180.f);      /* room-right is -x */
        rel[k][1] = h;
        rel[k][2] = r * cosf(az * (float)PI / 180.f);
    }
    return n;
}

static void sim_init(ClickerTrack* T, double now) {
    float rel[CLICKER_SIM_MAX_LEGS][3];
    T->sim_nlegs = sim_legs(T->cfg.sim, rel);
    for (int k = 0; k < T->sim_nlegs; ++k) {
        for (int a = 0; a < 3; ++a) T->sim_legs[k][a] = T->cfg.sim_anchor[a] + rel[k][a];
        /* each spot held at its own orientation, so a tip offset turns a different way at every clap */
        double qy[4], qx[4], qz[4], t[4];
        qaxis(0, 1, 0, 15.0 + 37.0 * k, qy);
        qaxis(1, 0, 0, (k % 2) ? -22.0 : 28.0, qx);
        qaxis(0, 0, 1, 10.0 * (k % 4) - 15.0, qz);
        qmul(qy, qx, t);
        qmul(t, qz, T->sim_legq[k]);
    }
    memcpy(T->sim_from, T->sim_legs[0], sizeof T->sim_from);
    memcpy(T->sim_fromq, T->sim_legq[0], sizeof T->sim_fromq);
    T->sim_leg = 0;
    T->sim_moving_done = 0;
    T->sim_leg_t0 = now - CLICKER_SIM_WALK_S;         /* the first spot is already reached; its hold starts now */
    memset(T->sim_if_done, 0, sizeof T->sim_if_done);
    T->sim_ifp_on = 0;
}

/* the interferer configured on `leg` that has not fired yet: its index, or -1 */
static int sim_if_for(const ClickerTrack* T, int leg) {
    const int n = T->cfg.sim_nif < CLICKER_SIM_MAX_IF ? T->cfg.sim_nif : CLICKER_SIM_MAX_IF;
    for (int i = 0; i < n; ++i)
        if (T->cfg.sim_if[i].leg == leg && !T->sim_if_done[i]) return i;
    return -1;
}

/* An interferer's TRUE position: the leg's spot turned CLICKER_SIM_IF_AZ_DEG about the vertical through the
 * anchor (the true array center), same range and height. The script's own arithmetic. */
static void sim_if_pos(const ClickerTrack* T, int leg, float out[3]) {
    const double A = CLICKER_SIM_IF_AZ_DEG * PI / 180.0, ca = cos(A), sa = sin(A);
    const double x = T->sim_legs[leg][0] - T->cfg.sim_anchor[0], y = T->sim_legs[leg][1] - T->cfg.sim_anchor[1],
                 z = T->sim_legs[leg][2] - T->cfg.sim_anchor[2];
    out[0] = T->cfg.sim_anchor[0] + (float)(ca * x + sa * z);
    out[1] = T->cfg.sim_anchor[1] + (float)y;
    out[2] = T->cfg.sim_anchor[2] + (float)(-sa * x + ca * z);
}

/* the moving leg, before its click: the hand waves a 5 cm circle at 1.6 Hz about the last spot (0.5 m/s) */
static int sim_waving(const ClickerTrack* T) {
    return T->cfg.sim_moving_leg >= 1 && T->sim_leg == T->cfg.sim_moving_leg && !T->sim_moving_done &&
           T->sim_leg < T->sim_nlegs;
}

/* The TRUE pose at t (under mu): the tip on its path plus a hand's tremor, the orientation, and the body
 * pivot that puts the TRUE tip offset at that tip. */
static void sim_truth(const ClickerTrack* T, double t, double tip[3], double q[4], double pivot[3]) {
    const int leg = T->sim_leg < T->sim_nlegs ? T->sim_leg : T->sim_nlegs - 1;
    const double tau = t - T->sim_leg_t0;
    if (sim_waving(T)) {
        const double w = 2.0 * PI * 1.6, r = 0.05, a = tau > 0.0 ? w * tau : 0.0;
        tip[0] = T->sim_from[0] + r * (cos(a) - 1.0);
        tip[1] = T->sim_from[1] + 0.3 * r * sin(a);
        tip[2] = T->sim_from[2] + r * sin(a);
        memcpy(q, T->sim_fromq, sizeof T->sim_fromq);
    } else {
        double u = tau / CLICKER_SIM_WALK_S;
        u = u < 0.0 ? 0.0 : (u > 1.0 ? 1.0 : u);
        const double s = u * u * (3.0 - 2.0 * u);
        for (int a = 0; a < 3; ++a) tip[a] = T->sim_from[a] + ((double)T->sim_legs[leg][a] - T->sim_from[a]) * s;
        qnlerp(T->sim_fromq, T->sim_legq[leg], s, q);
    }
    tip[0] += 0.0003 * sin(2.0 * PI * 5.3 * t);                    /* a held hand's tremor: real motion */
    tip[1] += 0.0002 * sin(2.0 * PI * 4.1 * t + 0.7);
    tip[2] += 0.00025 * sin(2.0 * PI * 6.7 * t + 1.9);
    const double off[3] = { CLICKER_SIM_TIP_X, CLICKER_SIM_TIP_Y, CLICKER_SIM_TIP_Z };
    double ro[3];
    qrotv(q, off, ro);
    for (int a = 0; a < 3; ++a) pivot[a] = tip[a] - ro[a];
}

/* ---- NatNet 4.1 payloads the simulated clicker writes (the parsers read them like Motive's) ---- */
struct W { uint8_t* b; size_t n, cap; };
static void wput(W* w, const void* p, size_t k) { if (w->n + k <= w->cap) memcpy(w->b + w->n, p, k); w->n += k; }
static void wi32(W* w, int32_t v) { wput(w, &v, 4); }
static void wf32(W* w, float v)   { wput(w, &v, 4); }
static void wi16(W* w, int16_t v) { wput(w, &v, 2); }
static void wstr(W* w, const char* s) { wput(w, s, strlen(s) + 1); }

/* a FRAMEOFDATA up to the rigid bodies: no markersets, no loose markers, then the bodies (the parser
 * stops there) */
static int sim_frame(uint8_t* buf, size_t cap, int32_t frame, const float sp[3], const float sq[4],
                     const float cp[3], const float cq[4]) {
    W w = { buf, 0, cap };
    wi32(&w, frame);
    wi32(&w, 0); wi32(&w, 0);                          /* markersets: count, section bytes */
    wi32(&w, 0); wi32(&w, 0);                          /* other markers: count, section bytes */
    wi32(&w, 2); wi32(&w, 2 * 38);                     /* rigid bodies: count, section bytes */
    const int32_t ids[2] = { CLICKER_SIM_STAND_ID, CLICKER_SIM_BODY_ID };
    const float* ps[2] = { sp, cp };
    const float* qs[2] = { sq, cq };
    for (int b = 0; b < 2; ++b) {
        wi32(&w, ids[b]);
        for (int a = 0; a < 3; ++a) wf32(&w, ps[b][a]);
        for (int a = 0; a < 4; ++a) wf32(&w, qs[b][a]);
        wf32(&w, 0.0002f);                             /* mean marker error */
        wi16(&w, 1);                                   /* tracked this frame */
    }
    return w.n <= cap ? (int)w.n : -1;
}

/* a MODELDEF naming both bodies, so the simulated clicker resolves a NAME the way the live path does */
static int sim_modeldef(uint8_t* buf, size_t cap) {
    static uint8_t body[512];
    W w = { buf, 0, cap };
    wi32(&w, 2);
    const char* names[2] = { "zm1_stand", "clicker" };
    const int32_t ids[2] = { CLICKER_SIM_STAND_ID, CLICKER_SIM_BODY_ID };
    for (int b = 0; b < 2; ++b) {
        W d = { body, 0, sizeof body };
        wstr(&d, names[b]);
        wi32(&d, ids[b]); wi32(&d, -1);
        wf32(&d, 0.f); wf32(&d, 0.f); wf32(&d, 0.f);
        wi32(&d, 3);
        const float mk[3][3] = { { 0.03f, 0.f, 0.f }, { -0.02f, 0.f, 0.02f }, { 0.f, 0.02f, -0.03f } };
        for (int i = 0; i < 3; ++i) for (int a = 0; a < 3; ++a) wf32(&d, mk[i][a]);
        for (int i = 0; i < 3; ++i) wi32(&d, 0);
        for (int i = 0; i < 3; ++i) { char nm[16]; snprintf(nm, sizeof nm, "Marker%d", i + 1); wstr(&d, nm); }
        if (d.n > sizeof body) return -1;
        wi32(&w, 1);                                   /* type 1: rigid body */
        wi32(&w, (int32_t)d.n);
        wput(&w, body, d.n);
    }
    return w.n <= cap ? (int)w.n : -1;
}

/* ---- the session ---- */

static void fail(ClickerTrack* T, const char* m) {
    snprintf(T->msg, sizeof T->msg, "%s", m);
    T->state.store(CLICKER_FAILED, std::memory_order_release);
}

/* body: a decimal streaming id, or a name to find in the MODELDEF payload. 1 = id set. */
static int resolve_body(ClickerTrack* T, const uint8_t* md, int mdlen, int major, int minor, char* why, size_t cap) {
    char* endp = NULL;
    const long id = strtol(T->cfg.body, &endp, 10);
    if (T->cfg.body[0] && endp && *endp == 0) {
        if (id <= 0) { snprintf(why, cap, "clicker: a streaming id is a positive number (got %ld)", id); return 0; }
        T->id = (int32_t)id;
        return 1;
    }
    if (mdlen < 0) {
        snprintf(why, cap, "clicker: no model definitions from Motive, so the name '%s' cannot be resolved "
                           "(is Motive streaming? or give the streaming id)", T->cfg.body);
        return 0;
    }
    static NatNetBodyDesc desc[64];                    /* static: 64 x 1.7 KB; one session thread at a time */
    bool complete = false;
    const int nd = natnet_parse_modeldef(md, (size_t)mdlen, major, minor, desc, 64, &complete);
    for (int i = 0; i < nd; ++i)
        if (!strcmp(desc[i].name, T->cfg.body)) { T->id = desc[i].id; return 1; }
    snprintf(why, cap, "clicker: rigid body '%s' is not in Motive's model definitions (%d bodies%s)", T->cfg.body,
             nd < 0 ? 0 : nd, complete ? "" : ", the walk stopped early");
    return 0;
}

/* One FRAMEOFDATA payload, arrived at t: every body in it, the clicker picked by id, its tip into the
 * ring. The live and the simulated session both come through here: ingest parses, then takes mu;
 * sim_fill already holds it. */
static void push_locked(ClickerTrack* T, const NatNetBody* b, int nb, double t) {
    if (nb < 0) return;
    T->last_frame_t = t;
    for (int i = 0; i < nb; ++i) {
        if (b[i].id != T->id) continue;
        float tip[3];
        if (!b[i].tracking_valid || !place_center(b[i].pos, b[i].quat, T->cfg.tip_m, tip)) return;
        ClickerSample& s = T->ring[T->head];
        s.t = t;
        memcpy(s.tip, tip, sizeof tip);
        T->head = (T->head + 1) % CLICKER_HIST;
        if (T->n < CLICKER_HIST) ++T->n;
        T->last_pose_t = t;
        return;
    }
}
static void ingest(ClickerTrack* T, const uint8_t* p, size_t len, int major, int minor, double t) {
    NatNetBody b[64];
    const int nb = natnet_parse_bodies(p, len, major, minor, b, 64);
    std::lock_guard<std::mutex> lk(T->mu);
    push_locked(T, b, nb, t);
}

/* The simulated session's thread only resolves the name and goes live: the poses are written by
 * clicker_sim_advance on the virtual clock (see clicker_track.h), not as wall time passes. */
static void run_sim(ClickerTrack* T) {
    static uint8_t md[4096];
    char why[256];
    const int mdlen = sim_modeldef(md, sizeof md);
    if (!resolve_body(T, md, mdlen, 4, 1, why, sizeof why)) { fail(T, why); return; }
    {
        std::lock_guard<std::mutex> lk(T->mu);
        const double t0 = T->vt.load();
        sim_init(T, t0);
        T->sim_next = t0;
        T->sim_fno = 0;
    }
    T->state.store(CLICKER_LIVE, std::memory_order_release);
    while (!T->stop.load(std::memory_order_relaxed)) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

/* Every grid time up to `until` (under mu): the pose the script says, as Motive would report it, through a
 * FRAMEOFDATA payload and ingest. The script's state is only changed by clicker_sim_poll_click, after this
 * has written the poses up to the time it fires at, so no pose is ever written with a later leg's state. */
static void sim_fill(ClickerTrack* T, double until) {
    /* the stand: a decoy ahead of the clicker in every frame, so a session that took the first body (or
     * the wrong id) reads the stand's pose and lands nowhere near a clap */
    const float sp[3] = { T->cfg.sim_anchor[0], T->cfg.sim_anchor[1] - 0.10f, T->cfg.sim_anchor[2] };
    const float sq[4] = { 0.f, 0.f, 0.f, 1.f };
    uint8_t buf[256];
    for (; T->sim_next <= until; T->sim_next += 1.0 / CLICKER_SIM_RATE_HZ) {
        const double t = T->sim_next;
        double tip[3], q[4], pv[3];
        sim_truth(T, t, tip, q, pv);
        /* what Motive reports: the pivot with a tenth of a millimeter of noise, the orientation with a
         * few hundredths of a degree of wobble about the body's own axes */
        double wob[4], qr[4];
        qaxis(1.0, 1.0, 0.3, 0.05 * sin(2.0 * PI * 11.0 * t), wob);
        qmul(q, wob, qr);
        const float cp[3] = { (float)(pv[0] + 0.0001 * sin(2.0 * PI * 37.0 * t)),
                              (float)(pv[1] + 0.0001 * sin(2.0 * PI * 29.0 * t + 1.3)),
                              (float)(pv[2] + 0.0001 * sin(2.0 * PI * 43.0 * t + 2.1)) };
        const float cq[4] = { (float)qr[0], (float)qr[1], (float)qr[2], (float)qr[3] };
        const int n = sim_frame(buf, sizeof buf, T->sim_fno++, sp, sq, cp, cq);
        if (n > 0) {
            NatNetBody b[4];
            push_locked(T, b, natnet_parse_bodies(buf, (size_t)n, 4, 1, b, 4), t);
        }
    }
}

static void run_live(ClickerTrack* T) {
    NatNetConfig nc;
    memset(&nc, 0, sizeof nc);
    nc.multicast = T->cfg.multicast[0] ? T->cfg.multicast : "239.255.42.99";
    nc.server = T->cfg.server[0] ? T->cfg.server : NULL;
    nc.data_port = 1511; nc.command_port = 1510;
    char e[256] = { 0 }, why[320];
    NatNetRaw* r = natnet_raw_open(&nc, e, sizeof e);
    if (!r) { snprintf(why, sizeof why, "clicker: %s", e); fail(T, why); return; }
    int major = 3, minor = 1;
    natnet_raw_version(r, &major, &minor);
    static uint8_t buf[65536];                         /* one session thread at a time */
    const int mdlen = nc.server ? natnet_raw_modeldef(r, buf, sizeof buf) : -1;
    if (!resolve_body(T, buf, mdlen, major, minor, why, sizeof why)) { natnet_raw_close(r); fail(T, why); return; }
    T->state.store(CLICKER_LIVE, std::memory_order_release);
    while (!T->stop.load(std::memory_order_relaxed)) {
        const int n = natnet_raw_next_frame(r, buf, sizeof buf);   /* 0 = the 200 ms timeout: re-poll stop */
        if (n > 0) ingest(T, buf, (size_t)n, major, minor, clicker_clock_s());
        else if (n < 0) std::this_thread::sleep_for(std::chrono::milliseconds(50));   /* a hard error backs off */
    }
    natnet_raw_close(r);
}

static void worker(ClickerTrack* T) {
    if (!T->cfg.body[0]) { fail(T, "clicker: give the rigid body's streaming id or name"); return; }
    if (T->cfg.sim) run_sim(T);
    else            run_live(T);
    if (T->state.load() == CLICKER_LIVE) T->state.store(CLICKER_OFF, std::memory_order_release);
}

void clicker_start(ClickerTrack* T, const ClickerCfg* c) {
    if (T->th_live) clicker_stop(T);
    T->cfg = *c;
    T->cfg.body[sizeof T->cfg.body - 1] = 0;
    T->cfg.server[sizeof T->cfg.server - 1] = 0;
    T->cfg.multicast[sizeof T->cfg.multicast - 1] = 0;
    T->msg[0] = 0;
    T->id = -1;
    {
        std::lock_guard<std::mutex> lk(T->mu);
        T->head = T->n = 0;
        T->last_frame_t = T->last_pose_t = 0.0;
        T->sim_nlegs = T->sim_leg = 0;
    }
    T->vt.store(clicker_clock_s());                    /* simulate: the virtual clock starts at the wall's time */
    T->stop.store(false);
    T->state.store(CLICKER_CONNECTING, std::memory_order_release);
    T->th = std::thread(worker, T);
    T->th_live = true;
}

void clicker_stop(ClickerTrack* T) {
    if (!T->th_live) return;
    T->stop.store(true);
    T->th.join();
    T->th_live = false;
    if (T->state.load() != CLICKER_FAILED) T->state.store(CLICKER_OFF, std::memory_order_release);
}

int clicker_state(const ClickerTrack* T) { return T->state.load(std::memory_order_acquire); }

double clicker_now(const ClickerTrack* T) { return T->cfg.sim ? T->vt.load() : clicker_clock_s(); }

void clicker_sim_advance(ClickerTrack* T, double dt) {
    if (!T->cfg.sim || clicker_state(T) != CLICKER_LIVE) return;
    if (!(dt > 0.0)) dt = 0.0;                         /* NaN too */
    if (dt > CLICKER_SIM_MAX_STEP_S) dt = CLICKER_SIM_MAX_STEP_S;
    std::lock_guard<std::mutex> lk(T->mu);
    const double now = T->vt.load() + dt;
    T->vt.store(now);
    sim_fill(T, now);
}

/* the window's mean and spread over [t0, t1] (under mu); returns the sample count */
static int window_stats(const ClickerTrack* T, double t0, double t1, float mean[3], float* spread, double* first, double* last) {
    double m[3] = { 0, 0, 0 };
    int n = 0;
    *first = 1e300; *last = -1e300;
    for (int i = 0; i < T->n; ++i) {
        const ClickerSample& s = T->ring[(T->head - 1 - i + CLICKER_HIST) % CLICKER_HIST];
        if (s.t < t0) break;                           /* newest first: everything further back is older */
        if (s.t > t1) continue;
        for (int a = 0; a < 3; ++a) m[a] += s.tip[a];
        if (s.t < *first) *first = s.t;
        if (s.t > *last) *last = s.t;
        ++n;
    }
    if (!n) return 0;
    for (int a = 0; a < 3; ++a) mean[a] = (float)(m[a] / n);
    float sp = 0.f;
    for (int i = 0; i < T->n; ++i) {
        const ClickerSample& s = T->ring[(T->head - 1 - i + CLICKER_HIST) % CLICKER_HIST];
        if (s.t < t0) break;
        if (s.t > t1) continue;
        const float dx = s.tip[0] - mean[0], dy = s.tip[1] - mean[1], dz = s.tip[2] - mean[2];
        const float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d > sp) sp = d;
    }
    *spread = sp;
    return n;
}

int clicker_latest(ClickerTrack* T, float tip[3], double* age_s, float* spread_m) {
    std::lock_guard<std::mutex> lk(T->mu);
    if (!T->n) return 0;
    const ClickerSample& s = T->ring[(T->head - 1 + CLICKER_HIST) % CLICKER_HIST];
    memcpy(tip, s.tip, sizeof s.tip);
    if (age_s) *age_s = clicker_now(T) - s.t;
    if (spread_m) {
        float mean[3], sp = -1.f;
        double first, last;
        const int n = window_stats(T, s.t - CLICKER_WINDOW_S, s.t, mean, &sp, &first, &last);
        *spread_m = (n >= CLICKER_MIN_SAMPLES && last - first >= 0.9 * CLICKER_WINDOW_S) ? sp : -1.f;
    }
    return 1;
}

int clicker_take(ClickerTrack* T, double t_onset, ClickerTake* out) {
    memset(out, 0, sizeof *out);
    out->t1 = t_onset - CLICKER_GUARD_S;
    out->t0 = out->t1 - CLICKER_WINDOW_S;
    if (clicker_state(T) != CLICKER_LIVE) {
        snprintf(out->why, sizeof out->why, "the clicker is not tracking");
        return CLICKER_TAKE_REFUSED;
    }
    std::lock_guard<std::mutex> lk(T->mu);
    if (!T->n || T->ring[(T->head - 1 + CLICKER_HIST) % CLICKER_HIST].t < out->t1) {
        snprintf(out->why, sizeof out->why, "no clicker pose after the clap (occluded, wrong body, or Motive stopped)");
        return CLICKER_TAKE_PENDING;
    }
    double first, last;
    out->n = window_stats(T, out->t0, out->t1, out->tip, &out->spread_m, &first, &last);
    if (out->n < CLICKER_MIN_SAMPLES || last - first < 0.9 * CLICKER_WINDOW_S) {
        snprintf(out->why, sizeof out->why, "the clicker dropped out before the clap (%d poses over %.0f ms; occluded?)",
                 out->n, out->n ? (last - first) * 1e3 : 0.0);
        return CLICKER_TAKE_REFUSED;
    }
    if (out->spread_m > CLICKER_STILL_M) {
        snprintf(out->why, sizeof out->why, "the clicker was moving: its tip wandered %.1f mm in the %.0f ms before "
                 "the clap (limit %.1f mm). Hold it still, then click",
                 out->spread_m * 1e3f, CLICKER_WINDOW_S * 1e3, CLICKER_STILL_M * 1e3f);
        return CLICKER_TAKE_REFUSED;
    }
    return CLICKER_TAKE_OK;
}

int clicker_sim_poll_click(ClickerTrack* T, float true_tip[3]) {
    if (!T->cfg.sim || clicker_state(T) != CLICKER_LIVE) return CLICKER_SIM_EV_NONE;
    std::lock_guard<std::mutex> lk(T->mu);
    /* the session's clock, never a caller's: clicker_sim_advance has written every pose up to it, and the
     * state change below shapes only the poses after it */
    const double now = T->vt.load();
    if (T->sim_ifp_on && now >= T->sim_ifp_t) {        /* an AFTER interferer: the clicker is still holding */
        memcpy(true_tip, T->sim_ifp_pos, sizeof T->sim_ifp_pos);
        T->sim_if_done[T->sim_ifp_idx] = 1;
        T->sim_ifp_on = 0;
        return CLICKER_SIM_EV_INTERF;
    }
    if (T->sim_leg >= T->sim_nlegs) return CLICKER_SIM_EV_NONE;
    double tip[3], q[4], pv[3];
    if (sim_waving(T)) {
        if (now < T->sim_leg_t0 + 0.4) return 0;       /* the click lands mid-wave */
        sim_truth(T, now, tip, q, pv);
        for (int a = 0; a < 3; ++a) true_tip[a] = (float)tip[a];
        /* the wave is done: walk on from where the hand is, without the tremor */
        const double w = 2.0 * PI * 1.6, r = 0.05, a = w * (now - T->sim_leg_t0);
        T->sim_from[0] += (float)(r * (cos(a) - 1.0));
        T->sim_from[1] += (float)(0.3 * r * sin(a));
        T->sim_from[2] += (float)(r * sin(a));
        T->sim_moving_done = 1;
        T->sim_leg_t0 = now;
        return CLICKER_SIM_EV_CLICK;
    }
    const double reached = T->sim_leg_t0 + CLICKER_SIM_WALK_S;
    const int fi = sim_if_for(T, T->sim_leg);
    const int when = fi >= 0 ? T->cfg.sim_if[fi].when : 0;
    if (when == CLICKER_SIM_IF_BEFORE) {               /* the interferer first, timed like a click */
        if (now < reached + CLICKER_SIM_IF_AT_S) return CLICKER_SIM_EV_NONE;
        sim_if_pos(T, T->sim_leg, true_tip);
        T->sim_if_done[fi] = 1;
        T->sim_leg_t0 = now + CLICKER_SIM_IF_GAP_S - CLICKER_SIM_WALK_S - CLICKER_SIM_HOLD_S;   /* the click: GAP later */
        return CLICKER_SIM_EV_INTERF;
    }
    if (now < reached + CLICKER_SIM_HOLD_S) return CLICKER_SIM_EV_NONE;
    sim_truth(T, now, tip, q, pv);
    for (int a = 0; a < 3; ++a) true_tip[a] = (float)tip[a];
    memcpy(T->sim_from, T->sim_legs[T->sim_leg], sizeof T->sim_from);
    memcpy(T->sim_fromq, T->sim_legq[T->sim_leg], sizeof T->sim_fromq);
    if (when == CLICKER_SIM_IF_AFTER) {                /* hold still past the click; the interferer fires in the hold */
        T->sim_ifp_on = 1;
        T->sim_ifp_t = now + CLICKER_SIM_IF_GAP_S;
        T->sim_ifp_idx = fi;
        sim_if_pos(T, T->sim_leg, T->sim_ifp_pos);
    }
    ++T->sim_leg;
    T->sim_leg_t0 = when == CLICKER_SIM_IF_AFTER ? now + CLICKER_SIM_IF_POST_S : now;   /* before t0 the next walk has not begun */
    return CLICKER_SIM_EV_CLICK;
}

int clicker_sim_left(ClickerTrack* T) {
    if (!T->cfg.sim) return 0;
    std::lock_guard<std::mutex> lk(T->mu);
    int left = T->sim_nlegs - T->sim_leg;
    if (left > 0 && T->cfg.sim_moving_leg >= T->sim_leg && T->cfg.sim_moving_leg < T->sim_nlegs && !T->sim_moving_done)
        ++left;
    const int n = T->cfg.sim_nif < CLICKER_SIM_MAX_IF ? T->cfg.sim_nif : CLICKER_SIM_MAX_IF;
    for (int i = 0; i < n; ++i)                        /* interferers on a leg the script will still visit */
        if (!T->sim_if_done[i] && T->cfg.sim_if[i].leg >= 0 && T->cfg.sim_if[i].leg < T->sim_nlegs &&
            (T->cfg.sim_if[i].leg >= T->sim_leg || (T->sim_ifp_on && T->sim_ifp_idx == i))) ++left;
    return left;
}
