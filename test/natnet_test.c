/*
 * natnet_test.c — M6: the off-wire NatNet FrameOfData parser + the pose seqlock (no socket).
 * Build synthetic frames in memory and assert the parser extracts the selected rigid body,
 * honors the tracking-valid flag, handles the NatNet 4.1+ per-section size prefix, recovers
 * the frame-suffix stamps (4.1+/4.5+ section hop), and rejects truncated/old-version input
 * without over-reading.
 */
#include "natnet.h"
#include "pose.h"

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
    CHECK(!natnet_resolve_name(d.b, d.n, 3, 1, "Head", &rid), "resolve needs NatNet >= 4");
    for (size_t k = 0; k < d.n; ++k)
        (void)natnet_resolve_name(d.b, k, 4, 1, "Head", &rid);   /* truncation-safe (ASan-clean) */

    /* --- pose seqlock roundtrip --- */
    PoseSlot slot; memset(&slot, 0, sizeof slot);
    float wp[3] = { 1, 2, 3 }, wq[4] = { 0, 0, 0, 1 }, rp[3], rq[4];
    CHECK(!pose_read(&slot, rp, rq), "fresh slot -> pose_read false (never published)");
    pose_write(&slot, wp, wq);
    CHECK(pose_read(&slot, rp, rq), "pose_read after write");
    CHECK(rp[0] == 1 && rp[1] == 2 && rp[2] == 3 && rq[3] == 1, "pose roundtrip values");

    if (fails) { printf("natnet_test: %d FAILURES\n", fails); return 1; }
    printf("natnet_test OK (parse v3/v4.1/v4.5, RB select, tracking-valid, suffix stamps, truncation-safe, seqlock)\n");
    return 0;
}
