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
 * back to QPC at arrival. One clock per connection, chosen at open (pose.h contract).
 *
 * The socket/thread path is Windows-only and on-hardware-pending (needs a live Motive server);
 * the parser (natnet_parse_frame) is pure and unit-tested against synthetic packets.
 */
#ifndef BWA_NATNET_H
#define BWA_NATNET_H

#include "pose.h"
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

/* The live pose slot the audio thread reads (stable for the NatNet's lifetime). */
const PoseSlot* natnet_pose(const NatNet* nn);

/* Stop the receiver thread and release the socket. Call after the audio thread is stopped. */
void            natnet_close(NatNet* nn);

/* Stream liveness, derived from LOCAL (QPC) arrival stamps — deliberately NOT the pose slot's
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

#endif /* BWA_NATNET_H */
