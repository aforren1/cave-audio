/*
 * natnet.c — off-wire NatNet (OptiTrack) ingest. See natnet.h.
 *
 * Two parts:
 *   1. natnet_parse_frame — a pure, fully bounds-checked decoder of a NAT_FRAMEOFDATA payload
 *      down to the selected rigid body's pose. Unit-tested; safe on hostile/truncated input.
 *   2. the consumer — a UDP (multicast or unicast) data socket + a receiver thread that decodes
 *      each frame and publishes the pose into a seqlock. Sockets through os.h; on-hardware-pending.
 *   3. for control-side tools: natnet_parse_bodies (every body in a frame; it shares the prefix
 *      walk and the per-body read with part 1, so the two cannot drift), natnet_parse_modeldef
 *      (every rigid-body description with its marker offsets), and NatNetRaw, a thread-free
 *      socket session. None of it touches the pose slot the audio thread reads.
 *
 * Wire format (from the documented protocol; cross-checked against the NatNet 4.5 SDK's
 * PacketClient sample — third_party/NatNet-4.5, referenced but never linked):
 *   packet header : uint16 messageID, uint16 nDataBytes
 *   FrameOfData   : int32 frameNumber
 *                   int32 nMarkerSets    [4.1+: int32 sectionBytes] then per set: name\0, int32 nMarkers, nMarkers*3 float
 *                   int32 nOtherMarkers  [4.1+: int32 sectionBytes] then nOtherMarkers*3 float
 *                   int32 nRigidBodies   [4.1+: int32 sectionBytes] then per body:
 *                       int32 ID, float x,y,z, float qx,qy,qz,qw, float meanError, int16 params (bit0 = tracking valid)
 *   (markersets/other-markers are skipped; rigid bodies follow.)
 *   After the rigid bodies, 4.1+ makes the FRAME SUFFIX reachable: every intervening section —
 *   skeletons, assets (4.1+), labeled markers, force plates, devices, + IMU and GPIO in 4.5+ —
 *   is `int32 count, int32 sectionBytes, data`, so the parser hops them and reads
 *     uint32 timecode, uint32 timecodeSub,
 *     double fTimestamp (seconds, "software timestamp"),
 *     uint64 cameraMidExposureTimestamp (capture instant, server high-res clock ticks),
 *     [uint64 dataReceived, uint64 transmit, 2x uint32 precision (4.1+), int16 params, int32 eod]
 *   The stamps feed pose prediction's velocity estimate (rt.c): server-clock stamps carry none
 *   of the network/scheduling jitter an arrival-time stamp does, and stay correct across a
 *   mid-session camera-rate change in Motive. Pre-4.1 the sections are not hoppable (skeletons
 *   would need full decoding), so the receiver falls back to the monotonic clock at arrival.
 */
#include "tracking/natnet.h"
#include "tracking/pose.h"        /* the seqlock slot the receiver publishes into */

#include <stdlib.h>
#include <string.h>

/* ---- pure parser ---------------------------------------------------------- */

#define NAT_FRAMEOFDATA       7
#define NAT_CONNECT           0
#define NAT_SERVERINFO        1
#define NAT_REQUEST_MODELDEF  4
#define NAT_MODELDEF          5

/* little-endian, bounds-checked cursor readers (the wire is LE; x86 is LE, so memcpy is fine) */
static bool rd_i32(const uint8_t* b, size_t len, size_t* off, int32_t* out) {
    if (*off + 4 > len) return false;
    memcpy(out, b + *off, 4); *off += 4; return true;
}
static bool rd_i16(const uint8_t* b, size_t len, size_t* off, int16_t* out) {
    if (*off + 2 > len) return false;
    memcpy(out, b + *off, 2); *off += 2; return true;
}
static bool rd_f32(const uint8_t* b, size_t len, size_t* off, float* out) {
    if (*off + 4 > len) return false;
    memcpy(out, b + *off, 4); *off += 4; return true;
}
static bool rd_f64(const uint8_t* b, size_t len, size_t* off, double* out) {
    if (*off + 8 > len) return false;
    memcpy(out, b + *off, 8); *off += 8; return true;
}
static bool rd_u64(const uint8_t* b, size_t len, size_t* off, uint64_t* out) {
    if (*off + 8 > len) return false;
    memcpy(out, b + *off, 8); *off += 8; return true;
}
static bool rd_skip(size_t len, size_t* off, size_t n) {
    if (*off + n > len || *off + n < *off) return false;   /* second test guards overflow */
    *off += n; return true;
}
static bool rd_cstr(const uint8_t* b, size_t len, size_t* off) {   /* skip a null-terminated name */
    while (*off < len) { if (b[(*off)++] == 0) return true; }
    return false;
}

static bool has_size_prefix(int major, int minor) {
    return ((major == 4) && (minor > 0)) || (major > 4);          /* per-section byte count, NatNet 4.1+ */
}

/* The suffix-stamp hop is certified against the vendored reference (third_party/NatNet-4.5):
 * the section list between the rigid bodies and the frame suffix is exactly known for
 * 4.1..4.5. A NEWER bitstream may insert a section the hop doesn't know about, landing it one
 * section short — where 8 garbage bytes could pass the structural check and become a garbage
 * timestamp. So unknown-future versions refuse the hop, and natnet_open falls back to
 * arrival-time stamps (pose prediction stays alive, just on the jittery clock) instead of
 * silently publishing t_ns = 0 forever. Bump when a newer SDK drop certifies the layout.
 * (The escape hatch for a too-new server without touching this code: connect UNICAST and pin
 * the syntax with the "Bitstream,4.5.0" command — unicast-only, Motive >= 3; see PacketClient.) */
static bool stamps_supported(int major, int minor) {
    return major == 4 && minor >= 1 && minor <= 5;
}

/* skip a count-prefixed section: either jump the 4.1+ byte count, or skip `each` bytes per item */
static bool skip_section_fixed(const uint8_t* b, size_t len, size_t* off,
                               int major, int minor, int32_t count, size_t each) {
    if (has_size_prefix(major, minor)) {
        int32_t bytes;
        if (!rd_i32(b, len, off, &bytes) || bytes < 0) return false;
        return rd_skip(len, off, (size_t)bytes);
    }
    if (count < 0) return false;
    return rd_skip(len, off, (size_t)count * each);
}

/* Walk a FrameOfData from its start to the first rigid-body record. On success *off_out sits on
 * body 0, *n_out is the body count and *rb_end_out the 4.1+ end of the rigid-body section (0 when
 * there is no size prefix, or the prefix lies). Shared by the single-body parser that feeds the
 * audio thread and the every-body parser the survey tool uses, so the two cannot drift. */
static bool seek_rigid_bodies(const uint8_t* p, size_t len, int major, int minor,
                              size_t* off_out, int32_t* n_out, size_t* rb_end_out) {
    size_t off = 0;
    int32_t n, sect;

    if (!rd_skip(len, &off, 4)) return false;    /* frameNumber */

    /* markersets — variable-length names, so when there's no 4.1+ size prefix, walk each one */
    if (!rd_i32(p, len, &off, &n)) return false;
    if (has_size_prefix(major, minor)) {
        if (!rd_i32(p, len, &off, &sect) || sect < 0 || !rd_skip(len, &off, (size_t)sect)) return false;
    } else {
        if (n < 0) return false;
        for (int32_t i = 0; i < n; ++i) {
            int32_t nm;
            if (!rd_cstr(p, len, &off)) return false;                  /* name */
            if (!rd_i32(p, len, &off, &nm) || nm < 0) return false;
            if (!rd_skip(len, &off, (size_t)nm * 12)) return false;    /* nMarkers * vec3 */
        }
    }

    /* legacy other (unlabeled) markers — fixed 3 floats each */
    if (!rd_i32(p, len, &off, &n)) return false;
    if (!skip_section_fixed(p, len, &off, major, minor, n, 12)) return false;

    /* rigid bodies */
    if (!rd_i32(p, len, &off, &n) || n < 0) return false;
    size_t rb_end = 0;                           /* 4.1+: end of the rigid-body section — the suffix hop's start */
    if (has_size_prefix(major, minor)) {
        if (!rd_i32(p, len, &off, &sect)) return false;                /* section byte count */
        if (sect >= 0 && off + (size_t)sect <= len && off + (size_t)sect >= off)
            rb_end = off + (size_t)sect;                               /* a lying count: no suffix hop */
    }
    *off_out = off; *n_out = n; *rb_end_out = rb_end;
    return true;
}

/* One rigid-body record: int32 ID, float[3] pos, float[4] quat, float meanError, int16 params. */
static bool read_rigid_body(const uint8_t* p, size_t len, size_t* off, int major, int minor, NatNetBody* b) {
    if (!rd_i32(p, len, off, &b->id)) return false;
    if (!rd_f32(p, len, off, &b->pos[0]) || !rd_f32(p, len, off, &b->pos[1]) ||
        !rd_f32(p, len, off, &b->pos[2])) return false;
    if (!rd_f32(p, len, off, &b->quat[0]) || !rd_f32(p, len, off, &b->quat[1]) ||
        !rd_f32(p, len, off, &b->quat[2]) || !rd_f32(p, len, off, &b->quat[3])) return false;
    if (!rd_f32(p, len, off, &b->mean_error)) return false;
    b->tracking_valid = true;
    if ((major == 2 && minor >= 6) || major > 2) {                     /* always true for major >= 3 */
        int16_t prm;
        if (!rd_i16(p, len, off, &prm)) return false;
        b->tracking_valid = (prm & 0x01) != 0;                         /* bit 0: tracked this frame */
    }
    return true;
}

bool natnet_parse_frame(const uint8_t* p, size_t len, int major, int minor,
                        int32_t want_id, float pos[3], float quat[4], bool* tracking_valid,
                        NatNetStamps* stamps) {
    if (stamps) { stamps->timestamp = -1.0; stamps->mid_exposure = 0; }
    if (major < 3) return false;                 /* pre-3 embeds per-RB marker data: unsupported */
    size_t off, rb_end;
    int32_t n;
    if (!seek_rigid_bodies(p, len, major, minor, &off, &n, &rb_end)) return false;

    bool found = false;
    for (int32_t i = 0; i < n && !found; ++i) {
        NatNetBody b;
        if (!read_rigid_body(p, len, &off, major, minor, &b)) return false;
        if (want_id <= 0 || b.id == want_id) {
            memcpy(pos, b.pos, sizeof b.pos);
            memcpy(quat, b.quat, sizeof b.quat);
            if (tracking_valid) *tracking_valid = b.tracking_valid;
            found = true;
        }
    }
    if (!found) return false;

    /* Frame-suffix stamps (4.1+ only; see the header comment for the layout). Every early-out
     * below returns TRUE: the pose above is already good, and a truncated/odd tail must degrade
     * to "no stamp" (the consumer publishes t_ns = 0), never to a lost pose. */
    if (stamps && rb_end && stamps_supported(major, minor)) {
        size_t toff = rb_end;
        int hops = 5;                                          /* skeletons, assets, labeled markers, force plates, devices */
        if (minor >= 5) hops += 2;                             /* 4.5: IMU, GPIO */
        for (int i = 0; i < hops; ++i) {
            int32_t cnt, bytes;
            if (!rd_i32(p, len, &toff, &cnt) || !rd_i32(p, len, &toff, &bytes) ||
                bytes < 0 || !rd_skip(len, &toff, (size_t)bytes)) return true;
        }
        double   ts;
        uint64_t me;
        if (!rd_skip(len, &toff, 8)) return true;              /* timecode + subframe */
        if (!rd_f64(p, len, &toff, &ts) || !rd_u64(p, len, &toff, &me)) return true;
        /* structural check that the hop landed on the suffix: what remains must hold the rest of
         * it — dataReceived + transmit (16) + precision stamps (8, 4.1+) + params (2) + eod (4).
         * `>=` tolerates fields a future version appends before params. */
        if (len - toff < 30) return true;
        stamps->timestamp    = ts;
        stamps->mid_exposure = me;
    }
    return true;
}

bool natnet_resolve_name(const uint8_t* p, size_t len, int major, int minor,
                         const char* want_name, int32_t* out_id) {
    if (major < 3 || !want_name || !out_id) return false;
    /* The per-description sizeInBytes prefix is NatNet 4.1+ (the SDK's version-aware Python client
     * gates it there; its C++ PacketClient reads it unconditionally but only talks 4.x). This
     * resolver used to assume it from 4.0, so a Motive 3.0 server (NatNet 4.0) misread every
     * description and name tracking never resolved. Below 4.1 there is no prefix to skip by, so
     * hand the payload to the typed walk natnet_parse_modeldef does. Receiver thread, at connect:
     * the heap scratch is fine here, never on the audio thread. */
    if (!((major == 4 && minor >= 1) || major > 4)) {
        enum { RN_CAP = 128 };
        NatNetBodyDesc* d = (NatNetBodyDesc*)malloc(sizeof *d * RN_CAP);
        if (!d) return false;
        const int n = natnet_parse_modeldef(p, len, major, minor, d, RN_CAP, NULL);
        bool found = false;
        for (int i = 0; i < n && !found; ++i)
            if (strcmp(d[i].name, want_name) == 0) { *out_id = d[i].id; found = true; }
        free(d);
        return found;
    }
    size_t off = 0;
    int32_t nDatasets;
    if (!rd_i32(p, len, &off, &nDatasets) || nDatasets < 0) return false;
    for (int32_t i = 0; i < nDatasets; ++i) {
        int32_t type, size;
        if (!rd_i32(p, len, &off, &type)) return false;
        if (!rd_i32(p, len, &off, &size) || size < 0) return false;
        size_t next = off + (size_t)size;
        if (next > len || next < off) return false;        /* size field within bounds (overflow-safe) */
        if (type == 1) {                                   /* rigid body description: name\0, int32 ID, ... */
            char name[256];
            size_t k = 0, no = off;
            bool term = false;
            while (no < next && k < sizeof name - 1) {
                char c = (char)p[no++];
                if (c == 0) { term = true; break; }
                name[k++] = c;
            }
            name[k] = 0;
            if (term && no + 4 <= next) {
                int32_t id; memcpy(&id, p + no, 4);
                if (strcmp(name, want_name) == 0) { *out_id = id; return true; }
            }
        }
        off = next;                                        /* skip the rest of this description */
    }
    return false;
}

/* ---- every rigid body (control-side tools; see natnet.h) ------------------ */

int natnet_parse_bodies(const uint8_t* p, size_t len, int major, int minor,
                        NatNetBody* out, int cap) {
    if (major < 3 || cap < 0 || (cap > 0 && !out)) return -1;
    size_t off, rb_end;
    int32_t n;
    if (!seek_rigid_bodies(p, len, major, minor, &off, &n, &rb_end)) return -1;
    int w = 0;
    for (int32_t i = 0; i < n; ++i) {
        NatNetBody b;
        if (!read_rigid_body(p, len, &off, major, minor, &b)) return -1;   /* a cut frame is dropped whole */
        if (w < cap) out[w++] = b;
    }
    return w;
}

/* Copy a NUL-terminated string that must end before `end` into dst (truncated to cap - 1). */
static bool rd_name(const uint8_t* b, size_t end, size_t* off, char* dst, size_t cap) {
    size_t k = 0;
    while (*off < end) {
        char c = (char)b[(*off)++];
        if (c == 0) { dst[k] = 0; return true; }
        if (k + 1 < cap) dst[k++] = c;
    }
    dst[k] = 0;
    return false;
}

/* The SDK's own sanity bound on a description's marker count (PacketClient: "Unreasonable number
 * of markers"). It also keeps count * 12 far from size_t overflow on a 32-bit build. */
#define NN_DESC_MAX_MARKERS 16000

/* One rigid-body description, bounded by `end` (the description's own size on 4.1+, the payload
 * length before). Layout in natnet.h. */
static bool read_rb_desc(const uint8_t* p, size_t end, size_t* off, int major, int minor, NatNetBodyDesc* d) {
    memset(d, 0, sizeof *d);
    d->rot_offset[3] = 1.f;
    if (!rd_name(p, end, off, d->name, sizeof d->name)) return false;
    if (!rd_i32(p, end, off, &d->id) || !rd_i32(p, end, off, &d->parent_id)) return false;
    for (int c = 0; c < 3; ++c) if (!rd_f32(p, end, off, &d->offset[c])) return false;
    if (major > 4 || (major == 4 && minor >= 2))
        for (int c = 0; c < 4; ++c) if (!rd_f32(p, end, off, &d->rot_offset[c])) return false;
    int32_t nm;
    if (!rd_i32(p, end, off, &nm) || nm < 0 || nm > NN_DESC_MAX_MARKERS) return false;
    d->n_markers = nm;
    d->n_stored  = nm < NATNET_RB_MAX_MARKERS ? nm : NATNET_RB_MAX_MARKERS;
    for (int32_t i = 0; i < nm; ++i) {
        float v[3];
        for (int c = 0; c < 3; ++c) if (!rd_f32(p, end, off, &v[c])) return false;
        if (i < NATNET_RB_MAX_MARKERS) memcpy(d->markers[i], v, sizeof v);
    }
    if (!rd_skip(end, off, (size_t)nm * 4)) return false;             /* active labels */
    if (major >= 4)
        for (int32_t i = 0; i < nm; ++i) if (!rd_cstr(p, end, off)) return false;   /* marker names */
    return true;
}

int natnet_parse_modeldef(const uint8_t* p, size_t len, int major, int minor,
                          NatNetBodyDesc* out, int cap, bool* complete) {
    if (complete) *complete = false;
    if (major < 3 || cap < 0 || (cap > 0 && !out)) return -1;
    size_t off = 0;
    int32_t nsets;
    if (!rd_i32(p, len, &off, &nsets) || nsets < 0) return -1;
    const bool sized = has_size_prefix(major, minor);
    NatNetBodyDesc scratch;                    /* skeleton bones, and bodies past cap */
    int w = 0;
    bool clean = true;
    for (int32_t i = 0; i < nsets; ++i) {
        int32_t type;
        if (!rd_i32(p, len, &off, &type)) return w;
        if (sized) {
            int32_t size;
            if (!rd_i32(p, len, &off, &size) || size < 0) return w;
            size_t next = off + (size_t)size;
            if (next > len || next < off) return w;          /* a lying size: stop, overflow-safe */
            if (type == 1) {
                NatNetBodyDesc* d = (w < cap) ? &out[w] : &scratch;
                size_t o = off;
                if (read_rb_desc(p, next, &o, major, minor, d)) { if (w < cap) ++w; }
                else clean = false;                          /* dropped; the size still gets us past it */
            }
            off = next;
            continue;
        }
        /* 3.0-4.0: no size prefix, so every description in front of a rigid body must be walked */
        switch (type) {
        case 0: {                                            /* markerset: name, nMarkers, names */
            int32_t nm;
            if (!rd_cstr(p, len, &off) || !rd_i32(p, len, &off, &nm) || nm < 0) return w;
            for (int32_t k = 0; k < nm; ++k) if (!rd_cstr(p, len, &off)) return w;
            break;
        }
        case 1: {
            NatNetBodyDesc* d = (w < cap) ? &out[w] : &scratch;
            if (!read_rb_desc(p, len, &off, major, minor, d)) return w;
            if (w < cap) ++w;
            break;
        }
        case 2: {                                            /* skeleton: name, ID, nBones, bone descs */
            int32_t sid, nb;
            if (!rd_cstr(p, len, &off) || !rd_i32(p, len, &off, &sid) ||
                !rd_i32(p, len, &off, &nb) || nb < 0) return w;
            for (int32_t k = 0; k < nb; ++k)
                if (!read_rb_desc(p, len, &off, major, minor, &scratch)) return w;
            break;
        }
        case 5:                                              /* camera: name, float[3] pos, float[4] quat */
            if (!rd_cstr(p, len, &off) || !rd_skip(len, &off, 28)) return w;
            break;
        default:
            return w;                                        /* force plate, device, ...: stop here */
        }
    }
    if (complete) *complete = clean;
    return w;
}

/* ---- consumer (UDP through the os.h shim; on-hardware-pending) ------------- */

#include "os/os.h"
#include <stdatomic.h>

/* How the receiver derives pose_write_t's t_ns. Chosen ONCE at open and fixed for the
 * connection's lifetime — pose.h's contract is one writer, ONE clock, differences only, and a
 * per-frame fallback would splice two clocks into one velocity difference. */
enum {
    NN_STAMP_ARRIVAL = 0,   /* the local monotonic clock at packet arrival (pre-4.1: the frame suffix is unreachable) */
    NN_STAMP_MIDEXPO,       /* CameraMidExposureTimestamp ticks -> ns (needs the handshake's clock
                             * frequency): the hardware capture instant — no network/scheduling
                             * jitter, immune to a mid-session camera-rate change in Motive */
    NN_STAMP_FTS            /* fTimestamp seconds -> ns (4.1+ forced by env, no command channel):
                             * software-stamped on the server — can carry sub-ms solve jitter, but
                             * still beats arrival time and stays rate-change immune */
};

struct NatNet {
    os_socket sock;
    os_thread thread;
    _Atomic int stop;
    int       major, minor;
    int       stamp_mode;    /* NN_STAMP_* */
    double    ticks_to_ns;   /* NN_STAMP_MIDEXPO: 1e9 / server HighResClockFrequency */
    int32_t   rigid_body;
    PoseSlot  pose;
    /* Liveness stamps, both on the LOCAL monotonic clock in NANOSECONDS (see natnet_status).
     * last_frame: any FrameOfData received; last_pose: a valid pose published for the selected
     * body; 0 = never. Written by the receiver thread, read by natnet_status on the control
     * thread - relaxed atomics, because a stamp orders nothing else. */
    _Atomic uint64_t last_frame_ns;
    _Atomic uint64_t last_pose_ns;
};

static void nn_err(char* err, size_t cap, const char* msg) {
    if (err && cap) { strncpy(err, msg, cap - 1); err[cap - 1] = 0; }
}

/* Map OptiTrack/Motive space (default: Y-up, right-handed, +Z forward, meters) into the
 * engine's room space — which IS that convention (identity head faces +z), so position AND
 * orientation pass through unchanged; the room origin/orientation calibration (where the
 * CAVE center is relative to the Motive origin) is applied here when a real survey is wired in. */
static void to_room(const float src_p[3], const float src_q[4], float p[3], float q[4]) {
    memcpy(p, src_p, sizeof(float) * 3);
    memcpy(q, src_q, sizeof(float) * 4);
}

/* Best-effort version handshake: ask the server (command port) for its NatNet version.
 * Returns true and sets major/minor on success. NatNet 3.0+ servers append their high-res
 * clock frequency (ticks/s) right after the version block (sSender_Server, NatNetTypes.h) —
 * it converts the frame suffix's CameraMidExposureTimestamp ticks to time; left 0 if absent. */
static bool handshake_version(const NatNetConfig* cfg, int* major, int* minor, uint64_t* freq) {
    if (!cfg->server || !cfg->server[0]) return false;
    os_socket s = os_udp_open();
    if (s == OS_INVALID_SOCKET) return false;
    os_udp_set_rcvtimeo_ms(s, 500);

    const uint16_t port = cfg->command_port ? cfg->command_port : 1510;
    uint8_t req[4]; uint16_t id = NAT_CONNECT, nb = 0;          /* {messageID, nDataBytes} */
    memcpy(req, &id, 2); memcpy(req + 2, &nb, 2);
    os_udp_sendto(s, req, sizeof req, cfg->server, port);

    uint8_t buf[2048];
    int got = os_udp_recv(s, buf, sizeof buf);
    os_udp_close(s);
    if (got < 4) return false;
    uint16_t msg; memcpy(&msg, buf, 2);
    if (msg != NAT_SERVERINFO) return false;
    /* payload = sSender { char szName[256]; uint8 Version[4]; uint8 NatNetVersion[4]; }
     * then (3.0+, sSender_Server) uint64 HighResClockFrequency — 264 is 8-aligned, no padding */
    const int nnv = 4 + 256 + 4;                               /* header + szName + Version */
    if (got < nnv + 2) return false;
    *major = buf[nnv]; *minor = buf[nnv + 1];
    if (freq && got >= nnv + 4 + 8) memcpy(freq, buf + nnv + 4, 8);
    return (*major > 0);
}

/* Request the model definitions from the server and resolve a rigid-body name to its streaming
 * ID. Returns true and sets *out_id on success. Needs a server (command port). */
static bool resolve_name_via_modeldef(const NatNetConfig* cfg, int major, int minor,
                                      const char* name, int32_t* out_id) {
    if (!cfg->server || !cfg->server[0]) return false;
    os_socket s = os_udp_open();
    if (s == OS_INVALID_SOCKET) return false;
    os_udp_set_rcvtimeo_ms(s, 800);

    const uint16_t port = cfg->command_port ? cfg->command_port : 1510;
    uint8_t req[4]; uint16_t id = NAT_REQUEST_MODELDEF, nb = 0;
    memcpy(req, &id, 2); memcpy(req + 2, &nb, 2);
    os_udp_sendto(s, req, sizeof req, cfg->server, port);

    bool found = false;
    for (int tries = 0; tries < 8 && !found; ++tries) {        /* skip any data frames that arrive first */
        uint8_t buf[65536];
        int got = os_udp_recv(s, buf, sizeof buf);
        if (got < 4) break;
        uint16_t msg; memcpy(&msg, buf, 2);
        if (msg == NAT_MODELDEF)
            found = natnet_resolve_name(buf + 4, (size_t)got - 4, major, minor, name, out_id);
    }
    os_udp_close(s);
    return found;
}

static void receiver(void* arg) {
    NatNet* nn = (NatNet*)arg;
    uint8_t buf[65536];                                        /* max UDP datagram */
    while (!atomic_load_explicit(&nn->stop, memory_order_relaxed)) {
        int got = os_udp_recv(nn->sock, buf, sizeof buf);
        if (got < 0) {                                         /* timeout is normal (200 ms, to re-poll stop); */
            if (got == OS_UDP_ERROR) os_sleep_ms(50);          /* a HARD error must back off, not hot-spin a core */
            continue;
        }
        if (got < 4) continue;                                 /* runt: re-poll stop */
        uint16_t msg, nbytes;
        memcpy(&msg, buf, 2); memcpy(&nbytes, buf + 2, 2);
        if (msg != NAT_FRAMEOFDATA) continue;
        const uint64_t now_ns = os_monotonic_ns();             /* one read: liveness stamps + arrival stamp */
        atomic_store_explicit(&nn->last_frame_ns, now_ns, memory_order_relaxed);  /* a frame arrived (any body) */
        size_t plen = (size_t)got - 4;
        if (nbytes <= plen) plen = nbytes;                     /* trust the smaller of header/recv */
        float sp[3], sq[4];
        bool tv = true;
        NatNetStamps st;
        if (natnet_parse_frame(buf + 4, plen, nn->major, nn->minor, nn->rigid_body, sp, sq, &tv, &st) && tv) {
            atomic_store_explicit(&nn->last_pose_ns, now_ns, memory_order_relaxed);  /* a valid pose for our body */
            float p[3], q[4];
            to_room(sp, sq, p, q);
            /* stamp with the connection's clock (stamp_mode, fixed at open): rt.c differences
             * successive stamps for the pose-prediction velocity estimate — same clock, same
             * writer, never compared to another clock. Server-clock stamps are preferred: the
             * positions were SAMPLED on the camera grid, so pairing them with arrival time
             * mis-measures dt by the network + scheduling jitter (worst when a held-up thread
             * drains several frames back to back). A frame whose suffix didn't parse publishes
             * t_ns = 0 ("untimestamped") — prediction resets rather than mixing clocks. */
            uint64_t t_ns = 0;
            switch (nn->stamp_mode) {
            case NN_STAMP_MIDEXPO:
                if (st.mid_exposure) t_ns = (uint64_t)((double)st.mid_exposure * nn->ticks_to_ns);
                break;
            case NN_STAMP_FTS:
                if (st.timestamp > 0.0) t_ns = (uint64_t)(st.timestamp * 1e9);
                break;
            default:
                t_ns = now_ns;                                 /* the arrival clock IS os_monotonic_ns now */
                break;
            }
            pose_write_t(&nn->pose, p, q, t_ns);
        }
    }
}

/* A body/stream is "current" if its last stamp is within this window. Loose enough that a couple
 * of dropped frames at any Motive rate (100-360 Hz) don't flap the status, tight enough to catch a
 * real dropout within a quarter second. */
#define NN_STALE_MS 250

NatNetStatus natnet_classify(int64_t now, int64_t last_frame, int64_t last_pose, int64_t stale) {
    if (last_frame == 0 || now - last_frame > stale) return NN_STATUS_NO_DATA;
    if (last_pose  == 0 || now - last_pose  > stale) return NN_STATUS_NO_BODY;
    return NN_STATUS_LIVE;
}

NatNetStatus natnet_status(const NatNet* nn) {
    if (!nn) return NN_STATUS_NO_DATA;
    const int64_t now   = (int64_t)os_monotonic_ns();
    const int64_t frame = (int64_t)atomic_load_explicit(&nn->last_frame_ns, memory_order_relaxed);
    const int64_t pose  = (int64_t)atomic_load_explicit(&nn->last_pose_ns,  memory_order_relaxed);
    return natnet_classify(now, frame, pose, (int64_t)NN_STALE_MS * 1000000);
}

/* The pose, read out for a caller that is not the audio thread (bwa_validate's tracked placement).
 * It exists so a C++ tool never has to include pose.h, whose seqlock is C11-atomics code. */
bool natnet_read_pose(const NatNet* nn, float p[3], float q[4]) {
    return nn && pose_read(&nn->pose, p, q);
}

NatNet* natnet_open(const NatNetConfig* cfg, char* err, size_t errcap) {
    if (!cfg) { nn_err(err, errcap, "natnet: null config"); return NULL; }

    if (os_net_startup() != 0) { nn_err(err, errcap, "natnet: socket startup failed"); return NULL; }

    NatNet* nn = (NatNet*)calloc(1, sizeof *nn);
    if (!nn) { nn_err(err, errcap, "natnet: out of memory"); os_net_cleanup(); return NULL; }
    nn->sock = OS_INVALID_SOCKET;
    nn->rigid_body = cfg->rigid_body;
    /* Identity until the first frame. A plain field store is no longer the right access now that
     * pose.h's payload is atomic, and the slot stays "never published" either way (seq == 0). */
    atomic_store_explicit(&nn->pose.q[3], 1.0f, memory_order_relaxed);

    /* inet_pton accepts only numeric IPv4 literals; a hostname or typo silently becomes 0.0.0.0,
     * which downstream surfaces as a misleading "server didn't respond" / "rigid body not found".
     * Reject a bad server/multicast address up front with a clear message. */
    if (cfg->server && cfg->server[0] && !os_ipv4_valid(cfg->server)) {
        nn_err(err, errcap, "natnet: tracker server must be a numeric IPv4 address (e.g. 192.168.1.10), not a hostname"); goto fail;
    }
    if (cfg->multicast && cfg->multicast[0] && !os_ipv4_valid(cfg->multicast)) {
        nn_err(err, errcap, "natnet: tracker multicast must be a numeric IPv4 multicast address (e.g. 239.255.42.99)"); goto fail;
    }

    /* Bitstream version. The server knows its own version, so when one is configured the
     * handshake is authoritative — it can't be desynced by a wrong version override (a
     * 4.0-vs-4.1 mistake silently mis-parses the size-prefixed sections). The env value is only
     * a fallback for a pure multicast listen with no command channel; 3.1 is the last resort. */
    nn->major = 0; nn->minor = 0;
    uint64_t clock_freq = 0;
    if (cfg->server && cfg->server[0]) handshake_version(cfg, &nn->major, &nn->minor, &clock_freq);
    if (nn->major <= 0) { nn->major = cfg->major; nn->minor = cfg->minor; clock_freq = 0; }
    if (nn->major <= 0) { nn->major = 3; nn->minor = 1; }

    /* Pose stamp policy, fixed for the connection (see the NN_STAMP_* enum): the frame suffix's
     * server-clock stamps on 4.1..4.5 (mid-exposure ticks when the handshake gave the tick rate,
     * fTimestamp seconds when the version was forced without a command channel), the local clock at arrival
     * otherwise — including bitstreams NEWER than the certified hop (stamps_supported), which
     * must degrade to a working arrival-clock prediction, not a dead one. rt.c only ever
     * differences the stamps, so which clock they're on is free to choose — but it must be ONE
     * clock (pose.h). */
    if (stamps_supported(nn->major, nn->minor)) {
        if (clock_freq) { nn->stamp_mode = NN_STAMP_MIDEXPO; nn->ticks_to_ns = 1e9 / (double)clock_freq; }
        else            nn->stamp_mode = NN_STAMP_FTS;
    } else nn->stamp_mode = NN_STAMP_ARRIVAL;

    /* Track by name: resolve to a streaming ID via the model definitions (needs a server). A
     * miss is fatal here — the caller asked for a specific body, so don't silently track another. */
    if (cfg->rigid_body_name && cfg->rigid_body_name[0]) {
        if (!cfg->server || !cfg->server[0]) {
            nn_err(err, errcap, "natnet: tracking by name needs the server address"); goto fail;
        }
        int32_t id;
        if (!resolve_name_via_modeldef(cfg, nn->major, nn->minor, cfg->rigid_body_name, &id)) {
            nn_err(err, errcap, "natnet: rigid body name not found in model definitions"); goto fail;
        }
        nn->rigid_body = id;
    }

    nn->sock = os_udp_open();
    if (nn->sock == OS_INVALID_SOCKET) { nn_err(err, errcap, "natnet: socket() failed"); goto fail; }

    os_udp_set_reuseaddr(nn->sock);
    os_udp_set_rcvtimeo_ms(nn->sock, 200);                      /* so the thread can poll ->stop */

    /* multicast binds ANY, then joins */
    if (os_udp_bind_any(nn->sock, cfg->data_port ? cfg->data_port : 1511) != 0) {
        nn_err(err, errcap, "natnet: bind() failed"); goto fail;
    }

    if (cfg->multicast && cfg->multicast[0]) {
        if (os_udp_join_multicast(nn->sock, cfg->multicast, cfg->local_iface) != 0) {
            nn_err(err, errcap, "natnet: multicast join failed"); goto fail;
        }
    }

    if (os_thread_create(&nn->thread, receiver, nn) != 0) {
        nn_err(err, errcap, "natnet: receiver thread failed to start"); goto fail;
    }
    return nn;

fail:
    os_udp_close(nn->sock);
    free(nn);
    os_net_cleanup();
    return NULL;
}

const PoseSlot* natnet_pose(const NatNet* nn) { return nn ? &nn->pose : NULL; }

void natnet_close(NatNet* nn) {
    if (!nn) return;
    atomic_store_explicit(&nn->stop, 1, memory_order_relaxed);
    /* Join FIRST: the receiver's recvfrom has a 200 ms timeout, so it returns and sees ->stop
     * on its own within one tick. Closing the socket here (from another thread) before the join
     * would risk the socket descriptor being recycled by another subsystem and the receiver
     * issuing a receive on a foreign socket. Close only after the thread has exited. */
    os_thread_join(&nn->thread);
    os_udp_close(nn->sock);
    free(nn);
    os_net_cleanup();
}

/* ---- thread-free session for control-side tools (see natnet.h) ------------ */

struct NatNetRaw {
    os_socket sock;
    int       major, minor;
    char      server[64];          /* copied: the caller's config need not outlive the open */
    uint16_t  command_port;
    uint8_t   pkt[65536];          /* one whole datagram (max UDP); heap-resident with the session */
};

NatNetRaw* natnet_raw_open(const NatNetConfig* cfg, char* err, size_t errcap) {
    if (!cfg) { nn_err(err, errcap, "natnet: null config"); return NULL; }
    if (cfg->server && cfg->server[0] && !os_ipv4_valid(cfg->server)) {
        nn_err(err, errcap, "natnet: tracker server must be a numeric IPv4 address (e.g. 192.168.1.10), not a hostname");
        return NULL;
    }
    if (cfg->server && strlen(cfg->server) >= sizeof ((NatNetRaw*)0)->server) {
        nn_err(err, errcap, "natnet: tracker server address too long"); return NULL;
    }
    if (cfg->multicast && cfg->multicast[0] && !os_ipv4_valid(cfg->multicast)) {
        nn_err(err, errcap, "natnet: tracker multicast must be a numeric IPv4 multicast address (e.g. 239.255.42.99)");
        return NULL;
    }
    if (os_net_startup() != 0) { nn_err(err, errcap, "natnet: socket startup failed"); return NULL; }
    NatNetRaw* r = (NatNetRaw*)calloc(1, sizeof *r);
    if (!r) { nn_err(err, errcap, "natnet: out of memory"); os_net_cleanup(); return NULL; }
    r->sock = OS_INVALID_SOCKET;
    if (cfg->server) strcpy(r->server, cfg->server);
    r->command_port = cfg->command_port ? cfg->command_port : 1510;

    /* the same version policy as natnet_open: the handshake is authoritative when a server is set */
    if (r->server[0]) handshake_version(cfg, &r->major, &r->minor, NULL);
    if (r->major <= 0) { r->major = cfg->major; r->minor = cfg->minor; }
    if (r->major <= 0) { r->major = 3; r->minor = 1; }

    r->sock = os_udp_open();
    if (r->sock == OS_INVALID_SOCKET) { nn_err(err, errcap, "natnet: socket() failed"); goto fail; }
    os_udp_set_reuseaddr(r->sock);
    os_udp_set_rcvtimeo_ms(r->sock, 200);
    if (os_udp_bind_any(r->sock, cfg->data_port ? cfg->data_port : 1511) != 0) {
        nn_err(err, errcap, "natnet: bind() failed"); goto fail;
    }
    if (cfg->multicast && cfg->multicast[0] &&
        os_udp_join_multicast(r->sock, cfg->multicast, cfg->local_iface) != 0) {
        nn_err(err, errcap, "natnet: multicast join failed"); goto fail;
    }
    return r;
fail:
    os_udp_close(r->sock);
    free(r);
    os_net_cleanup();
    return NULL;
}

void natnet_raw_version(const NatNetRaw* r, int* major, int* minor) {
    if (major) *major = r ? r->major : 0;
    if (minor) *minor = r ? r->minor : 0;
}

int natnet_raw_modeldef(NatNetRaw* r, uint8_t* buf, size_t cap) {
    if (!r || !r->server[0] || !buf) return -1;
    os_socket s = os_udp_open();
    if (s == OS_INVALID_SOCKET) return -1;
    os_udp_set_rcvtimeo_ms(s, 800);
    uint8_t req[4]; uint16_t id = NAT_REQUEST_MODELDEF, nb = 0;
    memcpy(req, &id, 2); memcpy(req + 2, &nb, 2);
    os_udp_sendto(s, req, sizeof req, r->server, r->command_port);

    uint8_t* pkt = r->pkt;
    int out = -1;
    for (int tries = 0; tries < 8 && out < 0; ++tries) {
        int got = os_udp_recv(s, pkt, sizeof r->pkt);
        if (got < 4) break;
        uint16_t msg, nbytes;
        memcpy(&msg, pkt, 2); memcpy(&nbytes, pkt + 2, 2);
        if (msg != NAT_MODELDEF) continue;
        size_t plen = (size_t)got - 4;
        if (nbytes <= plen) plen = nbytes;
        if (plen > cap) break;
        memcpy(buf, pkt + 4, plen);
        out = (int)plen;
    }
    os_udp_close(s);
    return out;
}

int natnet_raw_next_frame(NatNetRaw* r, uint8_t* buf, size_t cap) {
    if (!r || !buf) return -1;
    uint8_t* pkt = r->pkt;
    for (;;) {
        int got = os_udp_recv(r->sock, pkt, sizeof r->pkt);
        if (got == OS_UDP_TIMEOUT) return 0;
        if (got < 0) return -1;
        if (got < 4) continue;
        uint16_t msg, nbytes;
        memcpy(&msg, pkt, 2); memcpy(&nbytes, pkt + 2, 2);
        if (msg != NAT_FRAMEOFDATA) continue;
        size_t plen = (size_t)got - 4;
        if (nbytes <= plen) plen = nbytes;             /* trust the smaller of header/recv */
        if (plen > cap) plen = cap;                    /* a cut payload fails the parser, safely */
        memcpy(buf, pkt + 4, plen);
        return (int)plen;
    }
}

void natnet_raw_close(NatNetRaw* r) {
    if (!r) return;
    os_udp_close(r->sock);
    free(r);
    os_net_cleanup();
}
