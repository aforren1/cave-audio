/*
 * natnet_test.c — M6: the off-wire NatNet FrameOfData parser + the pose seqlock (no socket).
 * Build synthetic frames in memory and assert the parser extracts the selected rigid body,
 * honors the tracking-valid flag, handles the NatNet 4.1+ per-section size prefix, and
 * rejects truncated/old-version input without over-reading.
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
static void w_cstr(Buf* o, const char* s) { size_t k = strlen(s) + 1; memcpy(o->b + o->n, s, k); o->n += k; }

static void w_rb(Buf* o, int32_t id, float x, float y, float z,
                 float qx, float qy, float qz, float qw, int valid) {
    w_i32(o, id);
    w_f32(o, x); w_f32(o, y); w_f32(o, z);
    w_f32(o, qx); w_f32(o, qy); w_f32(o, qz); w_f32(o, qw);
    w_f32(o, 0.001f);                          /* meanError */
    w_i16(o, (int16_t)(valid ? 1 : 0));        /* params: bit 0 = tracking valid */
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

    /* --- NatNet 3.1: two rigid bodies --- */
    Buf o = { 0 };
    build_prefix_v3(&o);
    w_i32(&o, 2);                              /* nRigidBodies */
    w_rb(&o, 1,  1.0f, 2.0f, 3.0f, 0, 0, 0, 1, 1);
    w_rb(&o, 7, -1.5f, 0.5f, 4.0f, 0, 1, 0, 0, 1);

    CHECK(natnet_parse_frame(o.b, o.n, 3, 1, 0, p, q, &tv), "parse v3 first RB");
    CHECK(p[0] == 1 && p[1] == 2 && p[2] == 3 && q[3] == 1, "first RB pose");
    CHECK(natnet_parse_frame(o.b, o.n, 3, 1, 7, p, q, &tv), "parse v3 RB id=7");
    CHECK(p[0] == -1.5f && p[2] == 4.0f && q[1] == 1.0f, "RB id=7 pose");
    CHECK(!natnet_parse_frame(o.b, o.n, 3, 1, 99, p, q, &tv), "missing RB id -> false");

    /* --- tracking-valid bit honored --- */
    Buf u = { 0 };
    build_prefix_v3(&u);
    w_i32(&u, 1);
    w_rb(&u, 3, 0, 0, 0, 0, 0, 0, 1, 0);       /* not tracked this frame */
    CHECK(natnet_parse_frame(u.b, u.n, 3, 1, 0, p, q, &tv) && tv == false, "tracking-invalid flag read");

    /* --- NatNet 4.1 size-prefix skip path (empty markersets/other) --- */
    Buf v = { 0 };
    w_i32(&v, 7);                              /* frameNumber */
    w_i32(&v, 0); w_i32(&v, 0);                /* nMarkerSets=0, sectionBytes=0 */
    w_i32(&v, 0); w_i32(&v, 0);                /* nOtherMarkers=0, sectionBytes=0 */
    w_i32(&v, 1); w_i32(&v, 38);               /* nRigidBodies=1, sectionBytes=38 */
    w_rb(&v, 5, 7.0f, 8.0f, 9.0f, 0, 0, 0, 1, 1);
    CHECK(natnet_parse_frame(v.b, v.n, 4, 1, 0, p, q, &tv), "parse v4.1 with size prefix");
    CHECK(p[0] == 7 && p[1] == 8 && p[2] == 9, "v4.1 RB pose");

    /* --- truncation safety: every prefix must return false and never over-read (ASan-clean) --- */
    for (size_t k = 0; k < o.n; ++k)
        (void)natnet_parse_frame(o.b, k, 3, 1, 0, p, q, &tv);
    CHECK(!natnet_parse_frame(o.b, 10, 3, 1, 0, p, q, &tv), "truncated frame -> false");
    CHECK(!natnet_parse_frame(o.b, o.n, 2, 5, 0, p, q, &tv), "NatNet < 3 rejected");

    /* --- pose seqlock roundtrip --- */
    PoseSlot slot; memset(&slot, 0, sizeof slot);
    float wp[3] = { 1, 2, 3 }, wq[4] = { 0, 0, 0, 1 }, rp[3], rq[4];
    pose_write(&slot, wp, wq);
    CHECK(pose_read(&slot, rp, rq), "pose_read after write");
    CHECK(rp[0] == 1 && rp[1] == 2 && rp[2] == 3 && rq[3] == 1, "pose roundtrip values");

    if (fails) { printf("natnet_test: %d FAILURES\n", fails); return 1; }
    printf("natnet_test OK (parse v3/v4.1, RB select, tracking-valid, truncation-safe, seqlock)\n");
    return 0;
}
