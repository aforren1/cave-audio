/*
 * zylia_capture.h — the ZM-1 ASIO input shell, shared by zylia_probe (console meter) and
 * bw_calib_view's Zylia tab (the live DOA view). Owns the driver, streams the first 19 input
 * channels into per-capsule rings, watches for a transient (a clap), and publishes snapshots +
 * live meters through ZpShared. The DOA math it feeds (zylia_tdoa -> zylia_doa) lives in zylia.c,
 * unit-tested off-hardware; this is the rig-bound part. Compiled only when the ASIO SDK is present
 * (BW_HAVE_ASIO) — consumers guard their calls the same way.
 */
#ifndef BW_ZYLIA_CAPTURE_H
#define BW_ZYLIA_CAPTURE_H

#include "zylia.h"

#define ZP_SNAP_N   4096      /* transient snapshot: ~85 ms at 48 kHz (clap + a little room) */
#define ZP_SNAP_PRE 512       /* pre-roll kept before the trigger point (onset never clipped) */

/* Written by the ASIO audio callback, polled by a UI/console loop. Tool-grade handoff, deliberately
 * simpler than the engine's rings: the writer fills snap[] COMPLETELY, then bumps seq (aligned
 * 32-bit volatile — atomic on x86/ARM64 Windows), and a ~300 ms retrigger holdoff means the reader
 * always has time to copy out long before the next write can start. Worst imaginable failure is one
 * garbled DOA dot. rms[] are the live per-capsule meters (writer smooths, reader just draws). */
typedef struct {
    volatile float rms[ZYLIA_MICS];      /* smoothed linear RMS per capsule */
    volatile long  blocks;               /* total audio callbacks (liveness) */
    volatile long  seq;                  /* bumped AFTER snap[] is fully written */
    int            nch;                  /* input channels the device exposes (capture uses <= 19) */
    double         rate;                 /* sample rate the device opened at */
    const char*    title;                /* driver name for display */

    /* Trigger tuning — written by the UI thread, read by the audio thread each block. A torn float is
     * harmless here (worst case: one block trips at a stale threshold), and these NEED to be live: the
     * defaults below have never met a real room's noise floor or a real clap, so the first thing you do
     * at the rig is watch nfloor and drag the threshold to sit above it. Recompiling to find that out
     * would be a miserable way to spend an afternoon. */
    volatile float trig_ratio;           /* trip when a block's peak exceeds this x the noise floor (8) */
    volatile float trig_min;             /* ...and this absolute level, so a silent room can't trip (0.005) */
    volatile float nfloor;               /* the capture's live noise-floor estimate — tune against it */

    float          snap[ZYLIA_MICS][ZP_SNAP_N];
} ZpShared;

#ifdef __cplusplus
extern "C" {
#endif

/* Enumerate ASIO drivers + channel counts to stdout (the probe's --list). Returns 0. */
int       zylia_capture_list(void);

/* Open `driver` (NULL/empty = auto-pick: a name containing "zylia", else the first input-capable
 * driver), set `rate` best-effort, create buffers for the first <= 19 inputs, and start streaming.
 * Returns the live ZpShared (single instance), or NULL with a message on stderr. */
ZpShared* zylia_capture_open(const char* driver, double rate);
void      zylia_capture_close(void);

#ifdef __cplusplus
}
#endif

#endif /* BW_ZYLIA_CAPTURE_H */
