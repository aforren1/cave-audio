/*
 * zylia_probe_gui.h — the seam between zylia_probe's ASIO capture (a .cpp with windows.h) and its
 * raylib DOA view (a .c — raylib.h and windows.h can't share a translation unit: CloseWindow,
 * ShowCursor, Rectangle, ... collide). The ASIO side fills this struct from the audio callback; the
 * GUI polls it, runs zylia_tdoa -> zylia_doa on each fresh transient snapshot, and draws the arrival
 * direction on the capsule sphere. Built only when BOTH tool gates are on (calibrate for ASIO,
 * playground for raylib); without raylib the probe keeps its console meter.
 */
#ifndef BW_ZYLIA_PROBE_GUI_H
#define BW_ZYLIA_PROBE_GUI_H

#include "zylia.h"

#define ZP_SNAP_N   4096      /* transient snapshot: ~85 ms at 48 kHz (clap + a little room) */
#define ZP_SNAP_PRE 512       /* pre-roll kept before the trigger point (onset never clipped) */

/* Shared between the ASIO audio callback (writer) and the GUI loop (reader). Tool-grade handoff,
 * deliberately simpler than the engine's rings: the writer fills snap[] COMPLETELY, then bumps seq
 * (aligned 32-bit volatile — atomic on x86/ARM64 Windows), and a ~300 ms retrigger holdoff means the
 * reader always has time to copy out long before the next write can start. Worst imaginable failure
 * is one garbled DOA dot. rms[] are the live per-capsule meters (writer smooths, reader just draws). */
typedef struct {
    volatile float rms[ZYLIA_MICS];      /* smoothed linear RMS per capsule */
    volatile long  blocks;               /* total audio callbacks (liveness) */
    volatile long  seq;                  /* bumped AFTER snap[] is fully written */
    int            nch;                  /* channels actually streaming */
    double         rate;                 /* sample rate the device opened at */
    const char*    title;                /* driver name (or "simulate") for the HUD */
    float          snap[ZYLIA_MICS][ZP_SNAP_N];
} ZpShared;

#ifdef __cplusplus
extern "C" {
#endif

/* Run the GUI loop until the window is closed. sim == 0: `sh` is live-fed by the ASIO side. sim != 0:
 * no audio side at all — synthesize a clap from a slowly walking direction every ~1.5 s and push it
 * through the SAME snapshot -> tdoa -> doa path (full-pipeline demo / off-hardware GUI check; the
 * true direction is drawn as a ring the recovered dot should land in). */
void zylia_gui_run(ZpShared* sh, int sim);

#ifdef __cplusplus
}
#endif

#endif /* BW_ZYLIA_PROBE_GUI_H */
