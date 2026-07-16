/*
 * natnet.c — off-wire NatNet (OptiTrack) ingest. See natnet.h.
 *
 * Two parts:
 *   1. natnet_parse_frame — a pure, fully bounds-checked decoder of a NAT_FRAMEOFDATA payload
 *      down to the selected rigid body's pose. Unit-tested; safe on hostile/truncated input.
 *   2. the consumer — a UDP (multicast or unicast) data socket + a receiver thread that decodes
 *      each frame and publishes the pose into a seqlock. Windows/Winsock; on-hardware-pending.
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
 *   would need full decoding), so the receiver falls back to stamping QPC at arrival.
 */
#include "natnet.h"

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

bool natnet_parse_frame(const uint8_t* p, size_t len, int major, int minor,
                        int32_t want_id, float pos[3], float quat[4], bool* tracking_valid,
                        NatNetStamps* stamps) {
    if (stamps) { stamps->timestamp = -1.0; stamps->mid_exposure = 0; }
    if (major < 3) return false;                 /* pre-3 embeds per-RB marker data: unsupported */
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
    const bool have_params = ((major == 2 && minor >= 6) || major > 2);   /* always true for major >= 3 */
    bool found = false;
    for (int32_t i = 0; i < n && !found; ++i) {
        int32_t id;
        float x, y, z, qx, qy, qz, qw, meanErr;
        if (!rd_i32(p, len, &off, &id)) return false;
        if (!rd_f32(p, len, &off, &x)  || !rd_f32(p, len, &off, &y)  || !rd_f32(p, len, &off, &z))  return false;
        if (!rd_f32(p, len, &off, &qx) || !rd_f32(p, len, &off, &qy) ||
            !rd_f32(p, len, &off, &qz) || !rd_f32(p, len, &off, &qw)) return false;
        if (!rd_f32(p, len, &off, &meanErr)) return false;
        bool tv = true;
        if (have_params) {
            int16_t prm;
            if (!rd_i16(p, len, &off, &prm)) return false;
            tv = (prm & 0x01) != 0;                                    /* bit 0: tracked this frame */
        }
        if (want_id <= 0 || id == want_id) {
            pos[0] = x; pos[1] = y; pos[2] = z;
            quat[0] = qx; quat[1] = qy; quat[2] = qz; quat[3] = qw;
            if (tracking_valid) *tracking_valid = tv;
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
    (void)minor;
    if (major < 4 || !want_name || !out_id) return false;  /* per-description size prefix is NatNet 4.0+ */
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

/* ---- consumer (Winsock; on-hardware-pending) ------------------------------ */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

/* How the receiver derives pose_write_t's t_ns. Chosen ONCE at open and fixed for the
 * connection's lifetime — pose.h's contract is one writer, ONE clock, differences only, and a
 * per-frame fallback would splice two clocks into one velocity difference. */
enum {
    NN_STAMP_ARRIVAL = 0,   /* QPC at packet arrival (pre-4.1: the frame suffix is unreachable) */
    NN_STAMP_MIDEXPO,       /* CameraMidExposureTimestamp ticks -> ns (needs the handshake's clock
                             * frequency): the hardware capture instant — no network/scheduling
                             * jitter, immune to a mid-session camera-rate change in Motive */
    NN_STAMP_FTS            /* fTimestamp seconds -> ns (4.1+ forced by env, no command channel):
                             * software-stamped on the server — can carry sub-ms solve jitter, but
                             * still beats arrival time and stays rate-change immune */
};

struct NatNet {
    SOCKET    sock;
    HANDLE    thread;
    volatile LONG stop;
    int       major, minor;
    int       stamp_mode;    /* NN_STAMP_* */
    double    ticks_to_ns;   /* NN_STAMP_MIDEXPO: 1e9 / server HighResClockFrequency */
    int32_t   rigid_body;
    PoseSlot  pose;
};

static void nn_err(char* err, size_t cap, const char* msg) {
    if (err && cap) { strncpy(err, msg, cap - 1); err[cap - 1] = 0; }
}

/* Map OptiTrack/Motive space (default: Y-up, right-handed, +Z forward, metres) into the
 * engine's room space — which IS that convention (identity head faces +z), so position AND
 * orientation pass through unchanged; the room origin/orientation calibration (where the
 * CAVE centre is relative to the Motive origin) is applied here when a real survey is wired in. */
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
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    DWORD tmo = 500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof tmo);

    struct sockaddr_in srv; memset(&srv, 0, sizeof srv);
    srv.sin_family = AF_INET;
    srv.sin_port   = htons(cfg->command_port ? cfg->command_port : 1510);
    inet_pton(AF_INET, cfg->server, &srv.sin_addr);

    uint8_t req[4]; uint16_t id = NAT_CONNECT, nb = 0;          /* {messageID, nDataBytes} */
    memcpy(req, &id, 2); memcpy(req + 2, &nb, 2);
    sendto(s, (const char*)req, sizeof req, 0, (struct sockaddr*)&srv, sizeof srv);

    uint8_t buf[2048];
    int got = recv(s, (char*)buf, sizeof buf, 0);
    closesocket(s);
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
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    DWORD tmo = 800;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof tmo);

    struct sockaddr_in srv; memset(&srv, 0, sizeof srv);
    srv.sin_family = AF_INET;
    srv.sin_port   = htons(cfg->command_port ? cfg->command_port : 1510);
    inet_pton(AF_INET, cfg->server, &srv.sin_addr);

    uint8_t req[4]; uint16_t id = NAT_REQUEST_MODELDEF, nb = 0;
    memcpy(req, &id, 2); memcpy(req + 2, &nb, 2);
    sendto(s, (const char*)req, sizeof req, 0, (struct sockaddr*)&srv, sizeof srv);

    bool found = false;
    for (int tries = 0; tries < 8 && !found; ++tries) {        /* skip any data frames that arrive first */
        uint8_t buf[65536];
        int got = recv(s, (char*)buf, sizeof buf, 0);
        if (got < 4) break;
        uint16_t msg; memcpy(&msg, buf, 2);
        if (msg == NAT_MODELDEF)
            found = natnet_resolve_name(buf + 4, (size_t)got - 4, major, minor, name, out_id);
    }
    closesocket(s);
    return found;
}

static bool valid_ipv4(const char* s) { struct in_addr a; return inet_pton(AF_INET, s, &a) == 1; }

static DWORD WINAPI receiver(LPVOID arg) {
    NatNet* nn = (NatNet*)arg;
    uint8_t buf[65536];                                        /* max UDP datagram */
    while (!nn->stop) {
        int got = recvfrom(nn->sock, (char*)buf, sizeof buf, 0, NULL, NULL);
        if (got == SOCKET_ERROR) {                             /* timeout is normal (200 ms, to re-poll stop); */
            int e = WSAGetLastError();                         /* a HARD error must back off, not hot-spin a core */
            if (e != WSAETIMEDOUT && e != WSAEWOULDBLOCK) Sleep(50);
            continue;
        }
        if (got < 4) continue;                                 /* runt: re-poll stop */
        uint16_t msg, nbytes;
        memcpy(&msg, buf, 2); memcpy(&nbytes, buf + 2, 2);
        if (msg != NAT_FRAMEOFDATA) continue;
        size_t plen = (size_t)got - 4;
        if (nbytes <= plen) plen = nbytes;                     /* trust the smaller of header/recv */
        float sp[3], sq[4];
        bool tv = true;
        NatNetStamps st;
        if (natnet_parse_frame(buf + 4, plen, nn->major, nn->minor, nn->rigid_body, sp, sq, &tv, &st) && tv) {
            float p[3], q[4];
            to_room(sp, sq, p, q);
            /* stamp with the connection's clock (stamp_mode, fixed at open): rt.c differences
             * successive stamps for the pose-prediction velocity estimate — same clock, same
             * writer, never compared to another clock. Server-clock stamps are preferred: the
             * positions were SAMPLED on the camera grid, so pairing them with arrival time
             * mis-measures dt by the network + scheduling jitter (worst when a held-up thread
             * drains several frames back to back). A frame whose suffix didn't parse publishes
             * t_ns = 0 ("untimestamped") — prediction resets rather than mixing clocks. */
            unsigned __int64 t_ns = 0;
            switch (nn->stamp_mode) {
            case NN_STAMP_MIDEXPO:
                if (st.mid_exposure) t_ns = (unsigned __int64)((double)st.mid_exposure * nn->ticks_to_ns);
                break;
            case NN_STAMP_FTS:
                if (st.timestamp > 0.0) t_ns = (unsigned __int64)(st.timestamp * 1e9);
                break;
            default: {
                LARGE_INTEGER qc, qf;
                QueryPerformanceCounter(&qc); QueryPerformanceFrequency(&qf);
                t_ns = (unsigned __int64)((double)qc.QuadPart * 1e9 / (double)qf.QuadPart);
                break;
            }
            }
            pose_write_t(&nn->pose, p, q, t_ns);
        }
    }
    return 0;
}

NatNet* natnet_open(const NatNetConfig* cfg, char* err, size_t errcap) {
    if (!cfg) { nn_err(err, errcap, "natnet: null config"); return NULL; }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { nn_err(err, errcap, "natnet: WSAStartup failed"); return NULL; }

    NatNet* nn = (NatNet*)calloc(1, sizeof *nn);
    if (!nn) { nn_err(err, errcap, "natnet: out of memory"); WSACleanup(); return NULL; }
    nn->sock = INVALID_SOCKET;
    nn->rigid_body = cfg->rigid_body;
    nn->pose.q[3] = 1.0f;                                       /* identity until the first frame */

    /* inet_pton accepts only numeric IPv4 literals; a hostname or typo silently becomes 0.0.0.0,
     * which downstream surfaces as a misleading "server didn't respond" / "rigid body not found".
     * Reject a bad server/multicast address up front with a clear message. */
    if (cfg->server && cfg->server[0] && !valid_ipv4(cfg->server)) {
        nn_err(err, errcap, "natnet: tracker server must be a numeric IPv4 address (e.g. 192.168.1.10), not a hostname"); goto fail;
    }
    if (cfg->multicast && cfg->multicast[0] && !valid_ipv4(cfg->multicast)) {
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
     * fTimestamp seconds when the version was forced without a command channel), QPC at arrival
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

    nn->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (nn->sock == INVALID_SOCKET) { nn_err(err, errcap, "natnet: socket() failed"); goto fail; }

    BOOL reuse = TRUE;
    setsockopt(nn->sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof reuse);
    DWORD tmo = 200;                                            /* so the thread can poll ->stop */
    setsockopt(nn->sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof tmo);

    struct in_addr iface; iface.s_addr = htonl(INADDR_ANY);
    if (cfg->local_iface && cfg->local_iface[0]) inet_pton(AF_INET, cfg->local_iface, &iface);

    struct sockaddr_in addr; memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(cfg->data_port ? cfg->data_port : 1511);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);                  /* multicast bind to ANY, then join */
    if (bind(nn->sock, (struct sockaddr*)&addr, sizeof addr) != 0) { nn_err(err, errcap, "natnet: bind() failed"); goto fail; }

    if (cfg->multicast && cfg->multicast[0]) {
        struct ip_mreq mreq; memset(&mreq, 0, sizeof mreq);
        inet_pton(AF_INET, cfg->multicast, &mreq.imr_multiaddr);
        mreq.imr_interface = iface;
        if (setsockopt(nn->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mreq, sizeof mreq) != 0) {
            nn_err(err, errcap, "natnet: multicast join failed"); goto fail;
        }
    }

    nn->thread = CreateThread(NULL, 0, receiver, nn, 0, NULL);
    if (!nn->thread) { nn_err(err, errcap, "natnet: CreateThread failed"); goto fail; }
    return nn;

fail:
    if (nn->sock != INVALID_SOCKET) closesocket(nn->sock);
    free(nn);
    WSACleanup();
    return NULL;
}

const PoseSlot* natnet_pose(const NatNet* nn) { return nn ? &nn->pose : NULL; }

void natnet_close(NatNet* nn) {
    if (!nn) return;
    InterlockedExchange(&nn->stop, 1);
    /* Join FIRST: the receiver's recvfrom has a 200 ms timeout, so it returns and sees ->stop
     * on its own within one tick. Closing the socket here (from another thread) before the join
     * would risk the SOCKET handle being recycled by another subsystem and the receiver issuing
     * recvfrom on a foreign socket. Close only after the thread has exited and released it. */
    if (nn->thread) { WaitForSingleObject(nn->thread, INFINITE); CloseHandle(nn->thread); }
    if (nn->sock != INVALID_SOCKET) closesocket(nn->sock);
    free(nn);
    WSACleanup();
}
