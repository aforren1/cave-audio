/*
 * natnet.h — off-wire OptiTrack/NatNet pose ingest (M6).
 *
 * Parses the NatNet FrameOfData multicast/unicast stream directly — the NatNet SDK is
 * proprietary and would conflict with GPLv3 under distribution, so we consume the documented
 * wire protocol ourselves (see docs/build.md). A receiver thread decodes the selected rigid
 * body's pose and publishes it into a seqlock; with a tracker connected (bwa_tracker_connect) the audio thread samples
 * that slot at block time (lower latency than routing pose through the command ring).
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
    const char* multicast;     /* group address, e.g. "239.255.42.99"; NULL/"" => unicast */
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

/* Pure parser (no socket): extract the selected rigid body's pose from a NAT_FRAMEOFDATA
 * payload (the bytes after the 4-byte packet header). Returns true and fills pos/quat (xyzw)
 * and *tracking_valid when the rigid body is found. want_id <= 0 selects the first rigid body.
 * Fully bounds-checked against len — a malformed/truncated packet returns false, never
 * over-reads. Requires NatNet major >= 3 (earlier versions embed per-RB marker data). */
bool natnet_parse_frame(const uint8_t* payload, size_t len, int major, int minor,
                        int32_t want_id, float pos[3], float quat[4], bool* tracking_valid);

/* Pure parser (no socket): scan a NAT_MODELDEF (descriptions) payload for the rigid body named
 * want_name and return its streaming ID via *out_id. Uses the per-description size prefix to skip
 * datasets, so it needs NatNet major >= 4 (Motive's modern bitstream). Bounds-checked. */
bool natnet_resolve_name(const uint8_t* payload, size_t len, int major, int minor,
                         const char* want_name, int32_t* out_id);

#endif /* BWA_NATNET_H */
