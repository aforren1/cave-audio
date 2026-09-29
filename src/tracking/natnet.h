/*
 * natnet.h — off-wire OptiTrack/NatNet pose ingest (M6).
 *
 * Parses the NatNet FrameOfData multicast/unicast stream directly — the NatNet SDK is
 * proprietary and would conflict with GPLv3 under distribution, so we consume the documented
 * wire protocol ourselves (see docs/build.md). A receiver thread decodes the selected rigid
 * body's pose and publishes it into a seqlock; with a tracker connected (bwa_tracker_connect) the audio thread samples
 * that slot at block time (lower latency than routing pose through the command ring). On
 * NatNet 4.1+ the pose is stamped with the SERVER's clock from the frame suffix (capture-grid
 * time — pose prediction's velocity estimate sees none of the delivery jitter); pre-4.1 falls
 * back to the local monotonic clock at arrival. One clock per connection, chosen at open
 * (pose.h contract).
 *
 * The socket/thread path goes through the os.h shim (Winsock or BSD sockets) and is
 * on-hardware-pending (it needs a live Motive server); the parser (natnet_parse_frame) is pure
 * and unit-tested against synthetic packets.
 *
 * The second half of this header serves control-side TOOLS, not the audio thread: every rigid
 * body in a frame (natnet_parse_bodies), every rigid-body description with its marker offsets
 * (natnet_parse_modeldef), and a thread-free socket session (NatNetRaw). bwa_speaker_survey
 * (examples/speaker_survey.c) is the consumer.
 */
#ifndef BWA_NATNET_H
#define BWA_NATNET_H

/* The forward declaration, shared verbatim by rt.h and natnet.h so neither drags <stdatomic.h>
 * (and with it MSVC's /experimental:c11atomics) into every translation unit that only passes the
 * slot around by pointer. Guarded rather than repeated, because a redundant typedef is a C11-only
 * allowance and not every compiler in play is in C11 mode. */
#ifndef BWA_POSESLOT_FWD
#define BWA_POSESLOT_FWD
typedef struct PoseSlot PoseSlot;
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct NatNet NatNet;

typedef struct {
    const char* multicast;     /* group address, e.g. "239.255.42.99". NULL/"" => a PASSIVE unicast
                                * listen: bind the data port and take what arrives. That is NOT a
                                * working Motive unicast client — unicast streaming requires the
                                * NAT_CONNECT subscription + periodic NAT_KEEPALIVE pings on the
                                * command port (see PacketClient's CommandListenThread), which this
                                * consumer deliberately doesn't implement. Useful only for replay /
                                * synthetic feeds; multicast is the supported transport, and
                                * bwa_tracker_connect always passes a group (engine.c defaults it). */
    const char* server;        /* server IP, for the version handshake; NULL => skip handshake */
    const char* local_iface;   /* local NIC IP to bind/join on; NULL => INADDR_ANY */
    uint16_t    data_port;     /* NatNet data port (default 1511) */
    uint16_t    command_port;  /* NatNet command port (default 1510) */
    int32_t     rigid_body;    /* streaming ID to track; <= 0 => first rigid body in the frame */
    const char* rigid_body_name; /* track by name instead of ID; resolved to an ID via the model
                                  * definitions at open (needs `server`). NULL/"" => use rigid_body. */
    int         major, minor;  /* NatNet bitstream version; <= 0 => handshake, else default 3.1 */
} NatNetConfig;

/* Open the data socket, learn the bitstream version (handshake if a server is given), and
 * spawn the receiver thread. Returns NULL on failure with a message in err. */
NatNet*         natnet_open(const NatNetConfig* cfg, char* err, size_t errcap);

/* The live pose slot the audio thread reads (stable for the NatNet's lifetime). C only: the slot
 * is a C11-atomics seqlock (pose.h), opaque to C++. */
const PoseSlot* natnet_pose(const NatNet* nn);

/* Read the latest published pose. The same seqlock sample the audio thread takes, wrapped so a
 * caller off that thread (bwa_validate's tracked placement, a C++ tool) needs no pose.h. Returns
 * false when nothing has been published yet or the read lost the race. */
bool natnet_read_pose(const NatNet* nn, float p[3], float q[4]);

/* Stop the receiver thread and release the socket. Call after the audio thread is stopped. */
void            natnet_close(NatNet* nn);

/* Stream liveness, derived from LOCAL monotonic arrival stamps — deliberately NOT the pose slot's
 * t_ns, which rides the server clock on NatNet 4.1+ and so can't be compared to a local now.
 * Control-thread poll; never blocks. Mirrors bwa_tracker_state minus DISCONNECTED (that case is
 * nn == NULL, resolved by the caller). NO_DATA = no FrameOfData packets recently; NO_BODY = frames
 * arriving but the selected rigid body has no recent valid pose (wrong id, or occluded). */
typedef enum {
    NN_STATUS_NO_DATA = 0,
    NN_STATUS_NO_BODY,
    NN_STATUS_LIVE
} NatNetStatus;
NatNetStatus natnet_status(const NatNet* nn);

/* Pure classifier (no socket): decide liveness from monotonic-clock stamps. `now`, `last_frame`
 * (last NAT_FRAMEOFDATA received) and `last_pose` (last valid pose published) share one clock;
 * a 0 stamp means "never happened". `stale` is the age past which a stamp counts as dead. Factored
 * out of natnet_status so the four-state logic is unit-testable off-wire (the socket path is not). */
NatNetStatus natnet_classify(int64_t now, int64_t last_frame, int64_t last_pose, int64_t stale);

/* Frame-suffix stamps, server-side clocks. NatNet 4.1..4.5 only: 4.1+ is where the size-prefixed
 * sections make the suffix reachable without decoding skeletons/force plates/devices, and 4.5 is
 * the newest layout the vendored reference certifies — an unknown newer bitstream refuses the
 * hop rather than risk misreading it (see stamps_supported in natnet.c). */
typedef struct {
    double   timestamp;     /* fTimestamp: seconds since the server software started; < 0 = not recovered */
    uint64_t mid_exposure;  /* CameraMidExposureTimestamp: the capture instant in server high-res
                             * clock ticks (ServerInfo's HighResClockFrequency); 0 = not recovered */
} NatNetStamps;

/* Pure parser (no socket): extract the selected rigid body's pose from a NAT_FRAMEOFDATA
 * payload (the bytes after the 4-byte packet header). Returns true and fills pos/quat (xyzw)
 * and *tracking_valid when the rigid body is found. want_id <= 0 selects the first rigid body.
 * Fully bounds-checked against len — a malformed/truncated packet returns false, never
 * over-reads. Requires NatNet major >= 3 (earlier versions embed per-RB marker data).
 * stamps (NULL ok): filled from the frame suffix on NatNet 4.1..4.5; left "not recovered" on
 * older/newer streams or a truncated tail — a bad tail never fails an already-good pose. */
bool natnet_parse_frame(const uint8_t* payload, size_t len, int major, int minor,
                        int32_t want_id, float pos[3], float quat[4], bool* tracking_valid,
                        NatNetStamps* stamps);

/* Pure parser (no socket): scan a NAT_MODELDEF (descriptions) payload for the rigid body named
 * want_name and return its streaming ID via *out_id. Uses the per-description size prefix to skip
 * datasets, so it needs NatNet major >= 4 (Motive's modern bitstream). Bounds-checked. */
bool natnet_resolve_name(const uint8_t* payload, size_t len, int major, int minor,
                         const char* want_name, int32_t* out_id);

/* ---- every rigid body, for control-side tools (bwa_speaker_survey) ----------------------------
 * The single-body path above feeds the audio thread and stays as it is. These two parsers return
 * EVERYTHING a frame and a MODELDEF carry about rigid bodies, for a tool that surveys many bodies
 * at once. Both are pure and bounds-checked like the parsers above. */

/* One rigid body from a NAT_FRAMEOFDATA payload. */
typedef struct {
    int32_t id;              /* streaming ID (matches NatNetBodyDesc.id) */
    float   pos[3];          /* Motive frame, meters */
    float   quat[4];         /* xyzw, Motive frame */
    float   mean_error;      /* mean marker residual (m) */
    bool    tracking_valid;  /* params bit 0: solved this frame */
} NatNetBody;

/* Every rigid body in a NAT_FRAMEOFDATA payload. Writes the first `cap` into out[] and returns how
 * many it wrote; bodies past cap are still walked (bounds-checked) and dropped. Returns -1 on a
 * malformed or truncated payload, or on NatNet major < 3 (the same floor as natnet_parse_frame). */
int natnet_parse_bodies(const uint8_t* payload, size_t len, int major, int minor,
                        NatNetBody* out, int cap);

#define NATNET_NAME_MAX         256   /* the SDK's MAX_NAMELENGTH; a longer name is truncated */
#define NATNET_RB_MAX_MARKERS    32   /* marker offsets kept per body; more are counted, not stored */

/* One rigid-body description from a NAT_MODELDEF payload. */
typedef struct {
    char    name[NATNET_NAME_MAX];
    int32_t id;              /* streaming ID */
    int32_t parent_id;       /* -1 for a plain rigid body */
    float   offset[3];       /* pivot offset from the parent (m) */
    float   rot_offset[4];   /* xyzw, NatNet 4.2+ only; identity before */
    int     n_markers;       /* marker count as the description declares it */
    int     n_stored;        /* min(n_markers, NATNET_RB_MAX_MARKERS) */
    float   markers[NATNET_RB_MAX_MARKERS][3];   /* marker positions in the BODY's frame (m) */
} NatNetBodyDesc;

/* Every rigid-body description in a NAT_MODELDEF payload, in wire order. Writes the first `cap`
 * into out[] and returns how many it wrote (bodies past cap are walked and dropped), or -1 on
 * NatNet major < 3 or an unreadable dataset count. A truncated or garbage payload returns the
 * bodies read before the damage with `complete` (NULL ok) false; true means every description
 * was walked and every rigid body in it parsed.
 * Version handling (wire layouts from the SDK's PacketClient reference, never linked):
 *   4.1+  : every description carries a sizeInBytes prefix, so any type is skipped by size. The
 *           SDK's Python client gates that prefix on 4.1+ and its C++ PacketClient reads it
 *           unconditionally (it only ever talks 4.x); the version-aware one wins here. Note that
 *           natnet_resolve_name above assumes the prefix from 4.0.
 *   3.0-4.0: no size prefix. Markerset (0), rigid body (1), skeleton (2) and camera (5)
 *           descriptions are walked field by field; the walk STOPS at any other type (force
 *           plate, device, ...), returns what it has and reports complete = false. Motive sends
 *           markersets and rigid bodies first, so a stop there loses no rigid body in practice.
 *   Rigid body: name\0 (2.0+), int32 ID, int32 parentID, float[3] offset, float[4] rotation
 *           offset (4.2+), int32 nMarkers, then nMarkers float[3] positions, nMarkers int32
 *           active labels, and (4.0+) nMarkers name\0 strings.
 * A body with a malformed marker block is dropped rather than half-filled. */
int natnet_parse_modeldef(const uint8_t* payload, size_t len, int major, int minor,
                          NatNetBodyDesc* out, int cap, bool* complete);

/* ---- a thread-free session for control-side tools --------------------------------------------
 * natnet_open above owns a receiver thread and the audio thread's pose slot. A survey tool needs
 * neither: it wants the MODELDEF once and then raw frames, on its own thread, blocking. Same
 * config struct (rigid_body/rigid_body_name are ignored), same handshake, same sockets. */
typedef struct NatNetRaw NatNetRaw;

/* Handshake (when cfg->server is set), then bind the data port and join the group. NULL on
 * failure with a message in err. */
NatNetRaw* natnet_raw_open(const NatNetConfig* cfg, char* err, size_t errcap);
/* The bitstream version in use (handshake, else cfg->major/minor, else 3.1). */
void       natnet_raw_version(const NatNetRaw* r, int* major, int* minor);
/* Ask the server for the model definitions and copy the NAT_MODELDEF payload (without the 4-byte
 * packet header) into buf. Returns the payload length, or -1 (no server, no reply, or cap too
 * small). Blocks up to about a second. */
int        natnet_raw_modeldef(NatNetRaw* r, uint8_t* buf, size_t cap);
/* Wait for the next NAT_FRAMEOFDATA and copy its payload into buf (a payload longer than cap is
 * cut, and the parsers then reject it). Returns the payload length, 0 on a receive timeout (about
 * 200 ms), -1 on a socket error. Other message types are skipped. */
int        natnet_raw_next_frame(NatNetRaw* r, uint8_t* buf, size_t cap);
void       natnet_raw_close(NatNetRaw* r);

#endif /* BWA_NATNET_H */
