/*
 * zylia_capture.h — the ZM-1 ASIO input shell, shared by zylia_probe (console meter) and
 * bwa_calib_view's Zylia tab (the live DOA view). Owns the driver, streams the first 19 input
 * channels into per-capsule rings, watches for a transient (a clap), and publishes snapshots +
 * live meters through ZpShared. The DOA math it feeds (zylia_tdoa -> zylia_doa) lives in zylia.c,
 * unit-tested off-hardware; this is the rig-bound part. Compiled only when the ASIO SDK is present
 * (BWA_HAVE_ASIO) — consumers guard their calls the same way.
 */
#ifndef BWA_ZYLIA_CAPTURE_H
#define BWA_ZYLIA_CAPTURE_H

#include "calib/zylia.h"
#include "sink/sink_tsbase.h"       /* SinkTsClass: the driver stamp's base */

#include <stdint.h>

#define ZP_SNAP_N   4096      /* transient snapshot: ~85 ms at 48 kHz (clap + a little room) */
#define ZP_SNAP_PRE 512       /* pre-roll kept before the trigger point (onset never clipped) */

/* THE ONSET STAMP. Each snapshot carries the time its transient crossed the trigger threshold, on the
 * HOST clock: os_monotonic_ns (QPC on Windows), in seconds, the clock clicker_clock_s() reads, so the
 * tracked clicker's pose window is placed at the click and not at the frame that happened to see the
 * snapshot. The capture callback stamps it from the trigger's sample index within its block and that
 * block's buffer-switch time:
 *
 *   onset = switch - (in_lat - k) / rate          (zp_onset_s; in_lat = ASIOGetLatencies' input, the
 *                                                   age of the block's FIRST frame at the switch)
 *
 * The switch time is the driver's ASIOTime.systemTime when that stamp is on the host's own clock (the
 * classifier in src/sink/sink_tsbase.h decides, from the stamps themselves), else os_monotonic_ns read
 * at callback entry, which is late by the driver's dispatch delay (tens of microseconds on a working
 * ASIO driver).
 *
 * WHY THE DRIVER STAMP NEEDS A CLASSIFIER. The ASIO SDK says systemTime is derived from timeGetTime, not
 * QPC; some drivers stamp from QPC anyway. src/sink/sink_tsbase.h owns the classifier (shared with the
 * engine's ASIO sink, so the two cannot disagree about a driver) and its reasoning. Here:
 *   SINK_TS_HOST: QPC. The stamp IS the host clock; used as is. Exact, and free of dispatch jitter.
 *   SINK_TS_TGT:  timeGetTime. NOT used: converting needs the QPC-timeGetTime offset, which is only known
 *                 to timeGetTime's own tick (up to 15.6 ms), coarser than the callback-entry read.
 *   SINK_TS_UNKNOWN / SINK_TS_ABSENT: no usable stamp; the callback-entry read.
 * Two bases that happen to sit within 5 ms of each other classify as HOST; the error is then that
 * offset, under 5 ms, and well inside CLICKER_GUARD_S. */
enum { ZP_ONSET_NONE = 0,       /* no stamp: the consumer estimates (an older shell, or nothing usable) */
       ZP_ONSET_DRIVER = 1,     /* the driver's systemTime, on the host's QPC base */
       ZP_ONSET_CALLBACK = 2,   /* os_monotonic_ns at callback entry */
       ZP_ONSET_SIM = 3 };      /* simulate: the session's clock at the synthesized clap */

/* Sample k (0-based) of an n-frame block whose buffer switch was at switch_s: the block's first frame is
 * in_lat frames old at the switch (ASIOGetLatencies' input latency; <= 0 = unknown, then one block, the
 * least it can be). */
static inline double zp_onset_s(double switch_s, long k, long n, long in_lat, double rate) {
    const long lat = in_lat > 0 ? in_lat : n;
    return switch_s - (double)(lat - k) / rate;
}

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

    /* The snapshot's onset (see "THE ONSET STAMP"): written with snap[], before seq is bumped, so a reader
     * that saw the new seq reads the stamp that belongs to it. onset_src == ZP_ONSET_NONE = no stamp. */
    volatile double onset_s;             /* seconds, os_monotonic_ns's base (= clicker_clock_s) */
    volatile int    onset_src;           /* ZP_ONSET_* */
    /* rig readouts (the runbook records them): the driver's systemTime base as classified, the callback's
     * dispatch floor after the driver's stamp, and what the driver opened with */
    volatile int    ts_base;             /* SINK_TS_* */
    volatile float  ts_lag_ms;           /* floor of (host - systemTime), ms; meaningful under SINK_TS_HOST */
    int             block;               /* the ASIO block, frames */
    int             in_lat;              /* ASIOGetLatencies' input latency, frames (0 = the driver gave none) */

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

#endif /* BWA_ZYLIA_CAPTURE_H */
