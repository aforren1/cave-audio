/*
 * natnet_test.c — M6: the off-wire NatNet FrameOfData parser + the pose seqlock, then the session
 * itself on loopback against a fake Motive (the handshake, the version from the frames, the status).
 * Build synthetic frames in memory and assert the parser extracts the selected rigid body,
 * honors the tracking-valid flag, handles the NatNet 4.1+ per-section size prefix, recovers
 * the frame-suffix stamps (4.1+/4.5+ section hop), and rejects truncated/old-version input
 * without over-reading.
 */
#include "tracking/natnet.h"
#include "tracking/pose.h"
#include "os/os.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", (msg)); ++fails; } } while (0)

/* little-endian append helpers into a growing buffer */
typedef struct { uint8_t b[4096]; size_t n; } Buf;
static void w_i32 (Buf* o, int32_t v)     { memcpy(o->b + o->n, &v, 4); o->n += 4; }
static void w_i16 (Buf* o, int16_t v)     { memcpy(o->b + o->n, &v, 2); o->n += 2; }
static void w_f32 (Buf* o, float v)       { memcpy(o->b + o->n, &v, 4); o->n += 4; }
static void w_f64 (Buf* o, double v)      { memcpy(o->b + o->n, &v, 8); o->n += 8; }
static void w_u64 (Buf* o, uint64_t v)    { memcpy(o->b + o->n, &v, 8); o->n += 8; }
static void w_cstr(Buf* o, const char* s) { size_t k = strlen(s) + 1; memcpy(o->b + o->n, s, k); o->n += k; }

/* a 4.1+ hoppable section: int32 count, int32 sectionBytes, opaque body */
static void w_sect(Buf* o, int32_t count, int bytes) {
    w_i32(o, count); w_i32(o, bytes);
    for (int i = 0; i < bytes; ++i) o->b[o->n++] = 0xEE;
}

/* the 4.1+ frame suffix: timecode, fTimestamp, the three high-res stamps, precision, params, eod */
static void w_suffix(Buf* o, double ts, uint64_t midexpo) {
    w_i32(o, 0); w_i32(o, 0);          /* timecode + subframe */
    w_f64(o, ts);                      /* fTimestamp (s) */
    w_u64(o, midexpo);                 /* cameraMidExposureTimestamp (server ticks) */
    w_u64(o, 0); w_u64(o, 0);          /* cameraDataReceived, transmit */
    w_i32(o, 0); w_i32(o, 0);          /* precision secs/frac (4.1+) */
    w_i16(o, 0);                       /* frame params */
    w_i32(o, 0);                       /* end-of-data tag */
}

static void w_rb(Buf* o, int32_t id, float x, float y, float z,
                 float qx, float qy, float qz, float qw, int valid) {
    w_i32(o, id);
    w_f32(o, x); w_f32(o, y); w_f32(o, z);
    w_f32(o, qx); w_f32(o, qy); w_f32(o, qz); w_f32(o, qw);
    w_f32(o, 0.001f);                          /* meanError */
    w_i16(o, (int16_t)(valid ? 1 : 0));        /* params: bit 0 = tracking valid */
}

/* a NAT_MODELDEF rigid-body description: type(1) + sizeInBytes + [name\0, int32 ID, filler] */
static void w_rbdesc(Buf* o, const char* name, int32_t id, int filler) {
    w_i32(o, 1);
    w_i32(o, (int32_t)(strlen(name) + 1) + 4 + filler);    /* sizeInBytes: name\0 + ID + opaque tail */
    w_cstr(o, name);
    w_i32(o, id);
    for (int i = 0; i < filler; ++i) o->b[o->n++] = 0xAB;  /* marker arrays etc. we skip via size */
}
/* a non-rigidbody description to be skipped: type + sizeInBytes + opaque body */
static void w_otherdesc(Buf* o, int32_t type, int body) {
    w_i32(o, type);
    w_i32(o, body);
    for (int i = 0; i < body; ++i) o->b[o->n++] = 0xCD;
}

/* ---- full rigid-body descriptions (natnet_parse_modeldef), written per the version rules:
 * name\0, ID, parentID, float[3] offset, [4.2+ float[4] rotation offset], nMarkers,
 * nMarkers float[3], nMarkers int32 labels, [4.0+ nMarkers name\0] ---- */
static void w_rbdesc_body(Buf* o, int major, int minor, const char* name, int32_t id,
                          const float (*mk)[3], int n) {
    w_cstr(o, name);
    w_i32(o, id);
    w_i32(o, -1);                                           /* parent */
    w_f32(o, 0.25f); w_f32(o, 0.5f); w_f32(o, 0.75f);        /* pivot offset */
    if (major > 4 || (major == 4 && minor >= 2)) { w_f32(o, 0); w_f32(o, 0); w_f32(o, 0); w_f32(o, 1); }
    w_i32(o, n);
    for (int i = 0; i < n; ++i) { w_f32(o, mk[i][0]); w_f32(o, mk[i][1]); w_f32(o, mk[i][2]); }
    for (int i = 0; i < n; ++i) w_i32(o, 100 + i);         /* active labels */
    if (major >= 4) for (int i = 0; i < n; ++i) { char nm[16]; snprintf(nm, sizeof nm, "M%d", i); w_cstr(o, nm); }
}
/* one description: type, then [4.1+ sizeInBytes], then its body */
static void w_desc(Buf* o, int major, int minor, int32_t type, const Buf* body) {
    w_i32(o, type);
    if ((major == 4 && minor > 0) || major > 4) w_i32(o, (int32_t)body->n);
    memcpy(o->b + o->n, body->b, body->n); o->n += body->n;
}
static void w_markerset_body(Buf* o, const char* name, int n) {
    w_cstr(o, name); w_i32(o, n);
    for (int i = 0; i < n; ++i) { char nm[16]; snprintf(nm, sizeof nm, "%s_%d", name, i); w_cstr(o, nm); }
}
static const float MK4[4][3] = { { 0.06f, 0.00f, 0.01f }, { -0.05f, 0.04f, 0.01f },
                                 { -0.03f, -0.07f, 0.01f }, { 0.02f, 0.08f, 0.01f } };
static const float MK3[3][3] = { { 0.10f, 0.00f, 0.00f }, { 0.00f, 0.10f, 0.00f }, { 0.00f, 0.00f, 0.10f } };

/* A MODELDEF: markerset, "spk03" (4 markers), skeleton with one bone, "Wand" (3 markers), then a
 * force plate. The skeleton's bone must NOT surface as a body; the force plate is where a 3.x/4.0
 * walk has to stop. */
static void build_modeldef(Buf* d, int major, int minor, bool with_forceplate) {
    Buf body;
    w_i32(d, with_forceplate ? 5 : 4);
    body.n = 0; w_markerset_body(&body, "all", 7);                      w_desc(d, major, minor, 0, &body);
    body.n = 0; w_rbdesc_body(&body, major, minor, "spk03", 4, MK4, 4);  w_desc(d, major, minor, 1, &body);
    body.n = 0; w_cstr(&body, "Skel"); w_i32(&body, 20); w_i32(&body, 1);
    w_rbdesc_body(&body, major, minor, "Bone", 21, MK3, 3);              w_desc(d, major, minor, 2, &body);
    body.n = 0; w_rbdesc_body(&body, major, minor, "Wand", 9, MK3, 3);   w_desc(d, major, minor, 1, &body);
    if (with_forceplate) { body.n = 0; for (int i = 0; i < 40; ++i) body.b[body.n++] = 0x5A; w_desc(d, major, minor, 3, &body); }
}

/* deterministic garbage */
static uint32_t lcg_state = 12345u;
static uint32_t lcg(void) { lcg_state = lcg_state * 1664525u + 1013904223u; return lcg_state >> 8; }

/* NatNet 3.x frame prefix (no size prefixes): frameNumber, 1 markerset (2 markers), 1 other */
static void build_prefix_v3(Buf* o) {
    w_i32(o, 42);                              /* frameNumber */
    w_i32(o, 1);                               /* nMarkerSets */
    w_cstr(o, "rig");
    w_i32(o, 2);                               /* nMarkers */
    w_f32(o, 0); w_f32(o, 0); w_f32(o, 0);
    w_f32(o, 1); w_f32(o, 1); w_f32(o, 1);
    w_i32(o, 1);                               /* nOtherMarkers */
    w_f32(o, 9); w_f32(o, 9); w_f32(o, 9);
}

/* ---- the session on loopback: a fake Motive answers the command port, and the test aims frames at the
 * data port. Only the multicast delivery itself goes untested: a bound socket takes unicast too. ---- */
#define FAKE_DATA_PORT 47811
#define FAKE_CMD_PORT  47812

typedef struct {
    os_socket   s;
    os_thread   th;
    _Atomic int stop;
    int         major, minor;
    _Atomic int connect_bytes;     /* the last NAT_CONNECT's datagram size */
    _Atomic int connect_declared;  /* ... and its header's nDataBytes */
    Buf         model;
} FakeMotive;

static void fake_motive(void* arg) {
    FakeMotive* f = (FakeMotive*)arg;
    uint8_t in[2048];
    while (!atomic_load(&f->stop)) {
        char ip[64]; uint16_t port = 0;
        const int got = os_udp_recv_from(f->s, in, sizeof in, ip, &port);
        if (got < 4) continue;
        uint16_t msg, nb; memcpy(&msg, in, 2); memcpy(&nb, in + 2, 2);
        if (msg == 0) {                                  /* NAT_CONNECT -> NAT_SERVERINFO */
            atomic_store(&f->connect_bytes, got);
            atomic_store(&f->connect_declared, nb);
            uint8_t r[4 + 256 + 4 + 4 + 8];
            memset(r, 0, sizeof r);
            const uint16_t id = 1, n = (uint16_t)(sizeof r - 4);
            memcpy(r, &id, 2); memcpy(r + 2, &n, 2);
            memcpy(r + 4, "Motive", 6);
            r[4 + 256 + 4] = (uint8_t)f->major; r[4 + 256 + 4 + 1] = (uint8_t)f->minor;
            const uint64_t freq = 10000000ull;
            memcpy(r + 4 + 256 + 4 + 4, &freq, 8);
            os_udp_sendto(f->s, r, sizeof r, ip, port);
        } else if (msg == 4) {                           /* NAT_REQUEST_MODELDEF -> NAT_MODELDEF */
            Buf r = { 0 };
            const uint16_t id = 5, n = (uint16_t)f->model.n;
            memcpy(r.b, &id, 2); memcpy(r.b + 2, &n, 2); r.n = 4;
            memcpy(r.b + 4, f->model.b, f->model.n); r.n += f->model.n;
            os_udp_sendto(f->s, r.b, r.n, ip, port);
        }
    }
}

static void frame_v41(Buf* o, int nbodies, int32_t id, int valid) {
    o->n = 0;
    w_i32(o, 7);
    w_i32(o, 0); w_i32(o, 0);                   /* markersets */
    w_i32(o, 0); w_i32(o, 0);                   /* other markers */
    w_i32(o, nbodies); w_i32(o, nbodies * 38);
    for (int i = 0; i < nbodies; ++i) w_rb(o, id + i, 1.5f, 2.5f, 3.5f, 0, 0, 0, 1, valid);
}

static void frame_v31(Buf* o, int32_t id) {
    o->n = 0;
    build_prefix_v3(o);
    w_i32(o, 1);
    w_rb(o, id, 1.5f, 2.5f, 3.5f, 0, 0, 0, 1, 1);
}

/* Aim the frame at the data port until the session reports this status and reason, or 3 s pass. */
static bool pump(NatNet* nn, os_socket tx, const Buf* frame, NatNetStatus want, NatNetWhy why, NatNetInfo* out) {
    Buf pkt = { 0 };
    const uint16_t id = 7, n = (uint16_t)frame->n;
    memcpy(pkt.b, &id, 2); memcpy(pkt.b + 2, &n, 2);
    memcpy(pkt.b + 4, frame->b, frame->n);
    pkt.n = 4 + frame->n;
    for (int t = 0; t < 1500; ++t) {
        os_udp_sendto(tx, pkt.b, pkt.n, "127.0.0.1", FAKE_DATA_PORT);
        os_sleep_ms(2);
        natnet_info(nn, out);
        if (out->status == want && out->why == why) return true;
    }
    return false;
}

static NatNetConfig loop_cfg(const char* server, int32_t id) {
    NatNetConfig c;
    memset(&c, 0, sizeof c);
    c.server = server;
    c.data_port = FAKE_DATA_PORT; c.command_port = FAKE_CMD_PORT;
    c.rigid_body = id;
    return c;
}

static void live_session_tests(void) {
    if (os_net_startup() != 0) return;
    os_socket tx = os_udp_open();
    static FakeMotive f;
    f.s = os_udp_open();
    if (tx == OS_INVALID_SOCKET || f.s == OS_INVALID_SOCKET ||
        os_udp_bind_any(f.s, FAKE_CMD_PORT) != 0) {        /* wasi has no sockets; a port can be taken */
        printf("natnet_test: no loopback socket here, the live session section is skipped\n");
        os_udp_close(tx); os_udp_close(f.s); os_net_cleanup();
        return;
    }
    os_udp_set_rcvtimeo_ms(f.s, 50);
    f.major = 4; f.minor = 4;                               /* what a Motive 3.x sends: size prefixes */
    w_i32(&f.model, 1);
    w_rbdesc(&f.model, "stand", 12, 16);
    CHECK(os_thread_create(&f.th, fake_motive, &f) == 0, "fake motive thread");

    char err[320] = { 0 }, line[320];
    NatNetInfo info;
    Buf fr = { 0 };
    float p[3], q[4];

    char routed[NATNET_IP_MAX] = { 0 };
    CHECK(os_udp_route_local_ipv4("127.0.0.1", routed, sizeof routed) == 0 && !strcmp(routed, "127.0.0.1"),
          "the route to loopback leaves from loopback");
    CHECK(os_udp_route_local_ipv4("not an ip", routed, sizeof routed) != 0, "route: a bad address fails");

    /* by name, through the handshake and the model definitions: the path the tools take */
    {
        NatNetConfig c = loop_cfg("127.0.0.1", 0);
        c.rigid_body_name = "stand";
        NatNet* nn = natnet_open(&c, err, sizeof err);
        CHECK(nn != NULL, err);
        if (nn) {
            natnet_info(nn, &info);
            CHECK(atomic_load(&f.connect_bytes) == 4 + 273 && atomic_load(&f.connect_declared) == 273,
                  "NAT_CONNECT carries the SDK's payload, and its header declares it");
            CHECK(info.handshake_ok && !info.version_from_frame && info.major == 4 && info.minor == 4, "handshake: 4.4");
            CHECK(info.rigid_body == 12, "name resolved to its streaming id");
            CHECK(info.status == NN_STATUS_NO_DATA, "no frame yet: NO_DATA");
            natnet_explain(&info, line, sizeof line);
            CHECK(strstr(line, "no NatNet frames") != NULL, line);
            natnet_describe(&info, line, sizeof line);
            CHECK(strstr(line, "NatNet 4.4 from Motive at 127.0.0.1") != NULL, line);

            frame_v41(&fr, 2, 11, 1);                       /* ids 11 and 12: the second is ours */
            CHECK(pump(nn, tx, &fr, NN_STATUS_LIVE, NN_WHY_NONE, &info), "4.4 frames: LIVE");
            CHECK(natnet_read_pose(nn, p, q) && p[0] == 1.5f && p[1] == 2.5f && p[2] == 3.5f, "the pose came through");
            CHECK(info.poses > 0, "poses counted");

            frame_v41(&fr, 1, 12, 0);
            CHECK(pump(nn, tx, &fr, NN_STATUS_NO_BODY, NN_WHY_UNTRACKED, &info), "the body marked untracked: NO_BODY");
            natnet_explain(&info, line, sizeof line);
            CHECK(strstr(line, "untracked") != NULL && strstr(line, "id 12") != NULL, line);

            frame_v41(&fr, 1, 99, 1);
            CHECK(pump(nn, tx, &fr, NN_STATUS_NO_BODY, NN_WHY_MISSING, &info), "another body only: NO_BODY");
            natnet_explain(&info, line, sizeof line);
            CHECK(strstr(line, "not in them") != NULL, line);
            natnet_close(nn);
        }
    }

    /* A numeric id and no server: this read 3.1 by default and so never parsed a 4.x stream. */
    {
        NatNetConfig c = loop_cfg(NULL, 12);
        NatNet* nn = natnet_open(&c, err, sizeof err);
        CHECK(nn != NULL, err);
        if (nn) {
            natnet_info(nn, &info);
            CHECK(!info.handshake_ok && info.version_from_frame && info.major == 0, "no server: the version is unknown at open");
            frame_v41(&fr, 1, 12, 1);
            CHECK(pump(nn, tx, &fr, NN_STATUS_LIVE, NN_WHY_NONE, &info), "no server, 4.x frames: LIVE");
            CHECK(info.major == 4 && info.minor == 1, "no server: the layout came from the frames");
            natnet_close(nn);
        }
        nn = natnet_open(&c, err, sizeof err);
        CHECK(nn != NULL, err);
        if (nn) {
            frame_v31(&fr, 12);
            CHECK(pump(nn, tx, &fr, NN_STATUS_LIVE, NN_WHY_NONE, &info), "no server, 3.x frames: LIVE");
            CHECK(info.major == 3, "no server: a 3.x layout read as 3.x");
            natnet_close(nn);
        }
        nn = natnet_open(&c, err, sizeof err);
        CHECK(nn != NULL, err);
        if (nn) {
            frame_v41(&fr, 0, 0, 1);
            CHECK(pump(nn, tx, &fr, NN_STATUS_NO_BODY, NN_WHY_UNDECIDED, &info), "no bodies at all: the version stays undecided");
            natnet_explain(&info, line, sizeof line);
            CHECK(strstr(line, "rigid body") != NULL, line);
            /* and the undecided frames did not fix a wrong answer */
            frame_v41(&fr, 1, 12, 1);
            CHECK(pump(nn, tx, &fr, NN_STATUS_LIVE, NN_WHY_NONE, &info), "a body turns up: LIVE");
            natnet_close(nn);
        }
    }

    /* A server that does not answer: a port with nobody on it. */
    {
        NatNetConfig c = loop_cfg("127.0.0.1", 12);
        c.command_port = FAKE_CMD_PORT + 1;
        NatNet* nn = natnet_open(&c, err, sizeof err);
        CHECK(nn != NULL, err);
        if (nn) {
            natnet_info(nn, &info);
            CHECK(!info.handshake_ok && info.version_from_frame, "no reply: not a handshake");
            natnet_describe(&info, line, sizeof line);
            CHECK(strstr(line, "NO HANDSHAKE REPLY") != NULL, line);
            frame_v41(&fr, 1, 12, 1);
            CHECK(pump(nn, tx, &fr, NN_STATUS_LIVE, NN_WHY_NONE, &info), "no reply, numeric id: still LIVE off the frames");
            natnet_close(nn);
        }
        c.rigid_body = 0;
        c.rigid_body_name = "stand";
        err[0] = 0;
        nn = natnet_open(&c, err, sizeof err);
        CHECK(nn == NULL && strstr(err, "no reply") != NULL, "no reply, by name: the error says no reply, not 'not found'");
        if (nn) natnet_close(nn);
    }

    /* A name Motive does not have, with Motive answering. */
    {
        NatNetConfig c = loop_cfg("127.0.0.1", 0);
        c.rigid_body_name = "nobody";
        err[0] = 0;
        NatNet* nn = natnet_open(&c, err, sizeof err);
        CHECK(nn == NULL && strstr(err, "\"nobody\"") != NULL, "an unknown name is named");
        if (nn) natnet_close(nn);
    }

    /* A version the config forces is taken as given: a frame it cannot read says so. */
    {
        NatNetConfig c = loop_cfg(NULL, 12);
        c.major = 4; c.minor = 1;
        NatNet* nn = natnet_open(&c, err, sizeof err);
        CHECK(nn != NULL, err);
        if (nn) {
            fr.n = 0; w_i32(&fr, 7); w_i32(&fr, 0);         /* cut */
            CHECK(pump(nn, tx, &fr, NN_STATUS_NO_BODY, NN_WHY_MALFORMED, &info), "a cut frame: malformed");
            natnet_explain(&info, line, sizeof line);
            CHECK(strstr(line, "do not parse as NatNet 4.1") != NULL, line);
            natnet_describe(&info, line, sizeof line);
            CHECK(strstr(line, "from the config") != NULL, line);
            natnet_close(nn);
        }
    }

    /* The interface: the NIC routing to the server, unless the caller names one. */
    {
        NatNetConfig c = loop_cfg("127.0.0.1", 12);
        c.multicast = "239.255.42.99";
        NatNet* nn = natnet_open(&c, err, sizeof err);
        if (!nn) printf("natnet_test: no multicast here, the interface check is skipped (%s)\n", err);
        else {
            natnet_info(nn, &info);
#if defined(_WIN32)
            CHECK(!strcmp(info.iface, "127.0.0.1"), "joined on the NIC routing to the server");
#else       /* a kernel with no multicast on lo falls back to the OS default, and reads "" */
            CHECK(!strcmp(info.iface, "127.0.0.1") || info.iface[0] == 0, "joined on the NIC routing to the server");
#endif
            natnet_close(nn);
        }
        c.local_iface = "stand";
        err[0] = 0;
        CHECK(natnet_open(&c, err, sizeof err) == NULL && strstr(err, "local interface") != NULL, "a bad local interface is refused");
    }

    /* The raw session: the frames fix the version, for the clicker and for a tool with no server. */
    {
        NatNetConfig c = loop_cfg(NULL, 0);
        NatNetRaw* r = natnet_raw_open(&c, err, sizeof err);
        CHECK(r != NULL, err);
        if (r) {
            int ma = -1, mi = -1;
            natnet_raw_version(r, &ma, &mi);
            CHECK(ma == 0 && !natnet_raw_handshake_ok(r), "raw, no server: unknown at open");
            frame_v41(&fr, 1, 12, 1);
            Buf pkt = { 0 };
            const uint16_t id = 7, n = (uint16_t)fr.n;
            memcpy(pkt.b, &id, 2); memcpy(pkt.b + 2, &n, 2);
            memcpy(pkt.b + 4, fr.b, fr.n);
            os_udp_sendto(tx, pkt.b, 4 + fr.n, "127.0.0.1", FAKE_DATA_PORT);
            uint8_t rb[512];
            const int got = natnet_raw_next_frame(r, rb, sizeof rb);
            natnet_raw_version(r, &ma, &mi);
            CHECK(got == (int)fr.n && ma == 4, "raw, no server: the frame fixed the version");
            natnet_raw_close(r);
        }
    }

    atomic_store(&f.stop, 1);
    os_thread_join(&f.th);
    os_udp_close(f.s);
    os_udp_close(tx);
    os_net_cleanup();
}

int main(void) {
    float p[3], q[4];
    bool  tv;
    NatNetStamps st;

    /* --- NatNet 3.1: two rigid bodies --- */
    Buf o = { 0 };
    build_prefix_v3(&o);
    w_i32(&o, 2);                              /* nRigidBodies */
    w_rb(&o, 1,  1.0f, 2.0f, 3.0f, 0, 0, 0, 1, 1);
    w_rb(&o, 7, -1.5f, 0.5f, 4.0f, 0, 1, 0, 0, 1);

    CHECK(natnet_parse_frame(o.b, o.n, 3, 1, 0, p, q, &tv, NULL), "parse v3 first RB");
    CHECK(p[0] == 1 && p[1] == 2 && p[2] == 3 && q[3] == 1, "first RB pose");
    CHECK(natnet_parse_frame(o.b, o.n, 3, 1, 7, p, q, &tv, NULL), "parse v3 RB id=7");
    CHECK(p[0] == -1.5f && p[2] == 4.0f && q[1] == 1.0f, "RB id=7 pose");
    CHECK(!natnet_parse_frame(o.b, o.n, 3, 1, 99, p, q, &tv, NULL), "missing RB id -> false");

    /* pre-4.1: no suffix hop — stamps must come back "not recovered" */
    st.timestamp = 99.0; st.mid_exposure = 99;
    CHECK(natnet_parse_frame(o.b, o.n, 3, 1, 0, p, q, &tv, &st), "parse v3 with stamps arg");
    CHECK(st.timestamp < 0 && st.mid_exposure == 0, "v3 -> stamps unset");

    /* --- tracking-valid bit honored --- */
    Buf u = { 0 };
    build_prefix_v3(&u);
    w_i32(&u, 1);
    w_rb(&u, 3, 0, 0, 0, 0, 0, 0, 1, 0);       /* not tracked this frame */
    CHECK(natnet_parse_frame(u.b, u.n, 3, 1, 0, p, q, &tv, NULL) && tv == false, "tracking-invalid flag read");

    /* --- NatNet 4.1 size-prefix skip path (empty markersets/other) --- */
    Buf v = { 0 };
    w_i32(&v, 7);                              /* frameNumber */
    w_i32(&v, 0); w_i32(&v, 0);                /* nMarkerSets=0, sectionBytes=0 */
    w_i32(&v, 0); w_i32(&v, 0);                /* nOtherMarkers=0, sectionBytes=0 */
    w_i32(&v, 1); w_i32(&v, 38);               /* nRigidBodies=1, sectionBytes=38 */
    w_rb(&v, 5, 7.0f, 8.0f, 9.0f, 0, 0, 0, 1, 1);
    CHECK(natnet_parse_frame(v.b, v.n, 4, 1, 0, p, q, &tv, NULL), "parse v4.1 with size prefix");
    CHECK(p[0] == 7 && p[1] == 8 && p[2] == 9, "v4.1 RB pose");

    /* truncated tail: the pose is still good, the stamps degrade to "not recovered" */
    CHECK(natnet_parse_frame(v.b, v.n, 4, 1, 0, p, q, &tv, &st), "v4.1 without tail still parses");
    CHECK(st.timestamp < 0 && st.mid_exposure == 0, "v4.1 without tail -> stamps unset");

    /* --- NatNet 4.1 frame suffix: stamps recovered through the section hop --- */
    Buf w = { 0 };
    w_i32(&w, 8);                              /* frameNumber */
    w_i32(&w, 0); w_i32(&w, 0);                /* markersets */
    w_i32(&w, 0); w_i32(&w, 0);                /* legacy other markers */
    w_i32(&w, 1); w_i32(&w, 38);               /* rigid bodies */
    w_rb(&w, 5, 7.0f, 8.0f, 9.0f, 0, 0, 0, 1, 1);
    w_sect(&w, 1, 13);                         /* skeletons (opaque) */
    w_sect(&w, 2, 7);                          /* assets (4.1+) */
    w_sect(&w, 3, 21);                         /* labeled markers */
    w_sect(&w, 0, 0);                          /* force plates */
    w_sect(&w, 1, 5);                          /* devices */
    size_t w_no_suffix = w.n;                  /* keep: truncation sweep boundary below */
    w_suffix(&w, 123.456, 987654321ull);
    CHECK(natnet_parse_frame(w.b, w.n, 4, 1, 0, p, q, &tv, &st), "parse v4.1 with suffix");
    CHECK(p[0] == 7 && p[1] == 8 && p[2] == 9, "v4.1 suffix frame pose");
    CHECK(st.timestamp == 123.456, "v4.1 fTimestamp recovered");
    CHECK(st.mid_exposure == 987654321ull, "v4.1 mid-exposure recovered");

    /* --- NatNet 4.5 adds IMU + GPIO sections before the suffix --- */
    Buf x = { 0 };
    memcpy(x.b, w.b, w_no_suffix); x.n = w_no_suffix;  /* same frame up to the devices section */
    w_sect(&x, 2, 11);                         /* IMU (4.5+) */
    w_sect(&x, 1, 6);                          /* GPIO (4.5+) */
    w_suffix(&x, 42.5, 111222333ull);
    CHECK(natnet_parse_frame(x.b, x.n, 4, 5, 0, p, q, &tv, &st), "parse v4.5 with IMU/GPIO + suffix");
    CHECK(st.timestamp == 42.5 && st.mid_exposure == 111222333ull, "v4.5 stamps recovered");

    /* a bitstream NEWER than the certified hop (4.6, 5.0) refuses the stamps — an unknown layout
     * could mis-hop into garbage — but the pose (stable prefix sections) still parses */
    CHECK(natnet_parse_frame(x.b, x.n, 4, 6, 0, p, q, &tv, &st), "v4.6 pose still parses");
    CHECK(st.timestamp < 0 && st.mid_exposure == 0, "v4.6 -> stamps refused");
    CHECK(natnet_parse_frame(x.b, x.n, 5, 0, 0, p, q, &tv, &st), "v5.0 pose still parses");
    CHECK(st.timestamp < 0 && st.mid_exposure == 0, "v5.0 -> stamps refused");

    /* a suffix cut anywhere degrades to unset stamps, never a lost pose (or an over-read) */
    for (size_t k = w_no_suffix; k < w.n; ++k) {
        CHECK(natnet_parse_frame(w.b, k, 4, 1, 0, p, q, &tv, &st), "cut tail: pose survives");
        CHECK(st.timestamp < 0 && st.mid_exposure == 0, "cut tail: stamps unset");
    }

    /* --- truncation safety: every prefix must return false and never over-read (ASan-clean) --- */
    for (size_t k = 0; k < o.n; ++k)
        (void)natnet_parse_frame(o.b, k, 3, 1, 0, p, q, &tv, NULL);
    for (size_t k = 0; k < w.n; ++k)
        (void)natnet_parse_frame(w.b, k, 4, 1, 0, p, q, &tv, &st);
    CHECK(!natnet_parse_frame(o.b, 10, 3, 1, 0, p, q, &tv, NULL), "truncated frame -> false");
    CHECK(!natnet_parse_frame(o.b, o.n, 2, 5, 0, p, q, &tv, NULL), "NatNet < 3 rejected");

    /* --- NAT_MODELDEF name -> streaming ID resolution (NatNet 4.x) --- */
    Buf d = { 0 };
    w_i32(&d, 3);                              /* nDatasets */
    w_otherdesc(&d, 0, 8);                     /* a markerset description, skipped via sizeInBytes */
    w_rbdesc(&d, "Head", 12, 20);
    w_rbdesc(&d, "Wand", 5, 12);
    int32_t rid = -1;
    CHECK(natnet_resolve_name(d.b, d.n, 4, 1, "Wand", &rid) && rid == 5,  "resolve name Wand -> 5");
    CHECK(natnet_resolve_name(d.b, d.n, 4, 1, "Head", &rid) && rid == 12, "resolve name Head -> 12");
    CHECK(!natnet_resolve_name(d.b, d.n, 4, 1, "Nope", &rid), "resolve missing name -> false");
    CHECK(!natnet_resolve_name(d.b, d.n, 2, 9, "Head", &rid), "resolve needs NatNet >= 3");
    for (size_t k = 0; k < d.n; ++k)
        (void)natnet_resolve_name(d.b, k, 4, 1, "Head", &rid);   /* truncation-safe (ASan-clean) */

    /* --- every rigid body in a frame (natnet_parse_bodies) --- */
    {
        NatNetBody bb[8];
        CHECK(natnet_parse_bodies(o.b, o.n, 3, 1, bb, 8) == 2, "bodies v3: both");
        CHECK(bb[0].id == 1 && bb[0].pos[2] == 3.0f && bb[0].quat[3] == 1.0f && bb[0].tracking_valid, "bodies v3: first");
        CHECK(bb[1].id == 7 && bb[1].pos[0] == -1.5f && bb[1].quat[1] == 1.0f && bb[1].mean_error == 0.001f, "bodies v3: second");
        CHECK(natnet_parse_bodies(o.b, o.n, 3, 1, bb, 1) == 1 && bb[0].id == 1, "bodies: cap 1 keeps the first");
        CHECK(natnet_parse_bodies(o.b, o.n, 3, 1, NULL, 0) == 0, "bodies: cap 0 still walks");
        CHECK(natnet_parse_bodies(u.b, u.n, 3, 1, bb, 8) == 1 && !bb[0].tracking_valid, "bodies: tracking-invalid flag");
        CHECK(natnet_parse_bodies(w.b, w.n, 4, 1, bb, 8) == 1 && bb[0].id == 5 && bb[0].pos[1] == 8.0f, "bodies v4.1 with sections");
        CHECK(natnet_parse_bodies(o.b, o.n, 2, 9, bb, 8) == -1, "bodies: NatNet < 3 rejected");
        /* any cut inside the frame fails WHOLE (no half-read body); a cut after the rigid-body
         * section of a 4.1 frame still yields the body, since nothing after it is read */
        int bad = 0;
        for (size_t k = 0; k < o.n; ++k) if (natnet_parse_bodies(o.b, k, 3, 1, bb, 8) != -1) ++bad;
        CHECK(bad == 0, "bodies v3: every truncation -> -1");
        const size_t w_rb_end = 4 + 8 + 8 + 8 + 38;
        bad = 0;
        for (size_t k = 0; k < w.n; ++k) {
            int r = natnet_parse_bodies(w.b, k, 4, 1, bb, 8);
            if ((k < w_rb_end && r != -1) || (k >= w_rb_end && r != 1)) ++bad;
        }
        CHECK(bad == 0, "bodies v4.1: truncation inside the bodies -> -1, after them -> 1");
        /* garbage: never crash or over-read (ASan), never report more than cap */
        uint8_t g[512];
        bad = 0;
        for (int it = 0; it < 20000; ++it) {
            size_t gl = lcg() % sizeof g;
            for (size_t k = 0; k < gl; ++k) g[k] = (uint8_t)lcg();
            if (it & 1) { int32_t small = (int32_t)(lcg() % 4); memcpy(g + 4, &small, 4); }   /* plausible counts too */
            int r = natnet_parse_bodies(g, gl, 3 + (it % 3), it % 6, bb, 8);
            if (r < -1 || r > 8) ++bad;
        }
        CHECK(bad == 0, "bodies: garbage stays in range");
    }

    /* --- rigid-body descriptions with marker offsets (natnet_parse_modeldef) --- */
    {
        static NatNetBodyDesc dd[8];
        const int vers[][2] = { { 3, 0 }, { 3, 1 }, { 4, 0 }, { 4, 1 }, { 4, 2 }, { 4, 5 } };
        for (size_t vi = 0; vi < sizeof vers / sizeof vers[0]; ++vi) {
            const int M = vers[vi][0], m = vers[vi][1];
            const bool sized = (M == 4 && m > 0) || M > 4;
            char label[96];
            Buf d = { 0 };
            build_modeldef(&d, M, m, false);
            bool complete = false;
            int r = natnet_parse_modeldef(d.b, d.n, M, m, dd, 8, &complete);
            snprintf(label, sizeof label, "modeldef %d.%d: two bodies, the bone skipped, complete", M, m);
            CHECK(r == 2 && complete, label);
            snprintf(label, sizeof label, "modeldef %d.%d: spk03 fields", M, m);
            CHECK(r >= 1 && strcmp(dd[0].name, "spk03") == 0 && dd[0].id == 4 && dd[0].parent_id == -1 &&
                  dd[0].offset[2] == 0.75f && dd[0].n_markers == 4 && dd[0].n_stored == 4 &&
                  dd[0].rot_offset[3] == 1.0f, label);
            snprintf(label, sizeof label, "modeldef %d.%d: marker offsets exact", M, m);
            CHECK(r >= 1 && memcmp(dd[0].markers, MK4, sizeof MK4) == 0, label);
            snprintf(label, sizeof label, "modeldef %d.%d: Wand", M, m);
            CHECK(r == 2 && strcmp(dd[1].name, "Wand") == 0 && dd[1].id == 9 && dd[1].n_markers == 3 &&
                  dd[1].markers[2][2] == 0.10f, label);
            /* name tracking (natnet_resolve_name) must read the same payload in every version: it
             * assumed the size prefix from 4.0 and missed every name on a 4.0 (Motive 3.0) stream */
            {
                int32_t rid = -1;
                snprintf(label, sizeof label, "modeldef %d.%d: resolve_name finds Wand", M, m);
                CHECK(natnet_resolve_name(d.b, d.n, M, m, "Wand", &rid) && rid == 9, label);
                snprintf(label, sizeof label, "modeldef %d.%d: resolve_name misses a name that is absent", M, m);
                CHECK(!natnet_resolve_name(d.b, d.n, M, m, "Nope", &rid), label);
            }

            /* the force plate: skipped by size on 4.1+, a clean STOP before */
            Buf f = { 0 };
            build_modeldef(&f, M, m, true);
            r = natnet_parse_modeldef(f.b, f.n, M, m, dd, 8, &complete);
            snprintf(label, sizeof label, "modeldef %d.%d: force plate -> %s", M, m, sized ? "skipped" : "walk stops");
            CHECK(r == 2 && complete == sized, label);

            /* every truncation: no crash, never MORE bodies, never "complete" */
            int bad = 0;
            for (size_t k = 0; k < d.n; ++k) {
                bool c = true;
                int rr = natnet_parse_modeldef(d.b, k, M, m, dd, 8, &c);
                if (rr > 2 || c) ++bad;
                if (k < 4 && rr != -1) ++bad;                    /* no dataset count -> -1 */
            }
            snprintf(label, sizeof label, "modeldef %d.%d: truncation sweep", M, m);
            CHECK(bad == 0, label);
        }
        /* cap: the first body only, the rest walked */
        Buf d = { 0 };
        build_modeldef(&d, 4, 1, false);
        bool complete = false;
        CHECK(natnet_parse_modeldef(d.b, d.n, 4, 1, dd, 1, &complete) == 1 && complete &&
              strcmp(dd[0].name, "spk03") == 0, "modeldef: cap 1");
        CHECK(natnet_parse_modeldef(d.b, d.n, 2, 9, dd, 8, &complete) == -1, "modeldef: NatNet < 3 rejected");

        /* more markers than NATNET_RB_MAX_MARKERS: counted, first 32 stored, the walk stays aligned */
        static float mk40[40][3];
        for (int i = 0; i < 40; ++i) { mk40[i][0] = (float)i; mk40[i][1] = 0.5f; mk40[i][2] = -1.f; }
        Buf big = { 0 }, body = { 0 };
        w_i32(&big, 2);
        w_rbdesc_body(&body, 3, 1, "Big", 1, (const float (*)[3])mk40, 40); w_desc(&big, 3, 1, 1, &body);
        body.n = 0; w_rbdesc_body(&body, 3, 1, "After", 2, MK3, 3);        w_desc(&big, 3, 1, 1, &body);
        int r = natnet_parse_modeldef(big.b, big.n, 3, 1, dd, 8, &complete);
        CHECK(r == 2 && complete && dd[0].n_markers == 40 && dd[0].n_stored == NATNET_RB_MAX_MARKERS &&
              dd[0].markers[31][0] == 31.f && strcmp(dd[1].name, "After") == 0, "modeldef: 40 markers, 32 kept, aligned");

        /* 4.1: a body whose marker count lies is dropped, and its size prefix still gets the walk
         * to the next body */
        Buf lie = { 0 };
        w_i32(&lie, 2);
        body.n = 0; w_rbdesc_body(&body, 4, 1, "Liar", 3, MK3, 3);
        {   int32_t huge = 900; memcpy(body.b + 5 + 4 + 4 + 12, &huge, 4); }   /* past "Liar\0", ID, parent, offset */
        w_desc(&lie, 4, 1, 1, &body);
        body.n = 0; w_rbdesc_body(&body, 4, 1, "Next", 4, MK4, 4); w_desc(&lie, 4, 1, 1, &body);
        r = natnet_parse_modeldef(lie.b, lie.n, 4, 1, dd, 8, &complete);
        CHECK(r == 1 && !complete && strcmp(dd[0].name, "Next") == 0, "modeldef 4.1: lying marker count dropped, next body kept");
        /* 4.1: a size that runs past the payload stops the walk */
        Buf over = { 0 };
        w_i32(&over, 1); w_i32(&over, 1); w_i32(&over, 5000); w_cstr(&over, "X");
        CHECK(natnet_parse_modeldef(over.b, over.n, 4, 1, dd, 8, &complete) == 0 && !complete, "modeldef 4.1: size past the end");

        /* garbage */
        uint8_t g[768];
        int bad = 0;
        for (int it = 0; it < 20000; ++it) {
            size_t gl = lcg() % sizeof g;
            for (size_t k = 0; k < gl; ++k) g[k] = (uint8_t)lcg();
            if (gl >= 8 && (it & 1)) { int32_t c = 1 + (int32_t)(lcg() % 3), t = (int32_t)(lcg() % 3); memcpy(g, &c, 4); memcpy(g + 4, &t, 4); }
            int rr = natnet_parse_modeldef(g, gl, 3 + (it % 2), it % 6, dd, 8, &complete);
            if (rr < -1 || rr > 8) ++bad;
            for (int i = 0; i < rr; ++i) if (dd[i].n_stored < 0 || dd[i].n_stored > NATNET_RB_MAX_MARKERS ||
                                              memchr(dd[i].name, 0, NATNET_NAME_MAX) == NULL) ++bad;
        }
        CHECK(bad == 0, "modeldef: garbage stays in range, names terminated");
    }

    /* --- the version read off a frame, with no handshake to ask --- */
    {
        int ma = 0, mi = 0;
        CHECK(natnet_sniff_version(v.b, v.n, &ma, &mi) && ma == 4 && mi == 1, "sniff: a 4.1 frame");
        CHECK(natnet_sniff_version(w.b, w.n, &ma, &mi) && ma == 4 && mi == 1, "sniff: a 4.1 frame with a suffix");
        ma = mi = 0;
        CHECK(natnet_sniff_version(o.b, o.n, &ma, &mi) && ma == 3 && mi == 1, "sniff: a 3.1 frame");
        Buf none = { 0 };
        frame_v41(&none, 0, 0, 1);
        CHECK(!natnet_sniff_version(none.b, none.n, &ma, &mi), "sniff: no rigid body, no vote");
        /* 3.x with markersets ahead of the bodies, whose names shift a 4.1 read off its fields */
        Buf named = { 0 };
        w_i32(&named, 9);
        w_i32(&named, 1); w_cstr(&named, "all"); w_i32(&named, 1); w_f32(&named, 0); w_f32(&named, 0); w_f32(&named, 0);
        w_i32(&named, 0);
        w_i32(&named, 1); w_rb(&named, 12, 1, 2, 3, 0, 0, 0, 1, 1);
        CHECK(natnet_sniff_version(named.b, named.n, &ma, &mi) && ma == 3, "sniff: 3.x with a markerset");
        /* 4.1 with markersets and other markers */
        Buf full = { 0 };
        w_i32(&full, 9);
        w_i32(&full, 1); w_i32(&full, 20);
        w_cstr(&full, "all"); w_i32(&full, 1); w_f32(&full, 0); w_f32(&full, 0); w_f32(&full, 0);
        w_i32(&full, 2); w_i32(&full, 24);
        for (int i = 0; i < 6; ++i) w_f32(&full, 0);
        w_i32(&full, 1); w_i32(&full, 38); w_rb(&full, 12, 1, 2, 3, 0, 0, 0, 1, 1);
        CHECK(natnet_sniff_version(full.b, full.n, &ma, &mi) && ma == 4, "sniff: 4.1 with markersets and other markers");
        CHECK(natnet_parse_frame(full.b, full.n, ma, mi, 12, p, q, &tv, NULL) && p[1] == 2, "sniff's answer parses that frame");
        /* a cut or a garbage frame never reads past its length */
        for (size_t k = 0; k < full.n; ++k) (void)natnet_sniff_version(full.b, k, &ma, &mi);
        for (int it = 0; it < 20000; ++it) {
            uint8_t g[256];
            const size_t gl = lcg() % sizeof g;
            for (size_t k = 0; k < gl; ++k) g[k] = (uint8_t)lcg();
            (void)natnet_sniff_version(g, gl, &ma, &mi);
        }
    }

    /* --- stream-liveness classifier (pure; the live socket path is on-hardware-pending) --- */
    {
        const int64_t S = 1000;                 /* stale threshold, abstract ticks */
        const int64_t now = 5000000;            /* a large "now", like a real QPC count */
        CHECK(natnet_classify(now, 0, 0, S) == NN_STATUS_NO_DATA, "never a frame -> NO_DATA");
        CHECK(natnet_classify(now, now, 0, S) == NN_STATUS_NO_BODY, "frames but no body -> NO_BODY");
        CHECK(natnet_classify(now, now, now, S) == NN_STATUS_LIVE, "fresh frame + body -> LIVE");
        CHECK(natnet_classify(now, now - S - 1, now - S - 1, S) == NN_STATUS_NO_DATA, "stale frames -> NO_DATA");
        CHECK(natnet_classify(now, now - 1, now - S - 1, S) == NN_STATUS_NO_BODY, "fresh frames, stale body (occluded) -> NO_BODY");
        CHECK(natnet_classify(now, now - S, now - S, S) == NN_STATUS_LIVE, "exactly at threshold -> still LIVE");
    }

    /* --- pose seqlock roundtrip --- */
    PoseSlot slot; memset(&slot, 0, sizeof slot);
    float wp[3] = { 1, 2, 3 }, wq[4] = { 0, 0, 0, 1 }, rp[3], rq[4];
    CHECK(!pose_read(&slot, rp, rq), "fresh slot -> pose_read false (never published)");
    pose_write(&slot, wp, wq);
    CHECK(pose_read(&slot, rp, rq), "pose_read after write");
    CHECK(rp[0] == 1 && rp[1] == 2 && rp[2] == 3 && rq[3] == 1, "pose roundtrip values");

    live_session_tests();

    if (fails) { printf("natnet_test: %d FAILURES\n", fails); return 1; }
    printf("natnet_test OK (parse v3/v4.1/v4.5, RB select, tracking-valid, suffix stamps, truncation-safe, every-body frames, MODELDEF markers 3.0-4.5, garbage-safe, liveness classify, seqlock, version sniff, loopback session)\n");
    return 0;
}
