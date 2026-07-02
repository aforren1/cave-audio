/*
 * zylia_probe.cpp — "is the ZM-1 talking?" console bring-up tool (input-only ASIO).
 *
 * Plug in the Zylia ZM-1 (its own ASIO driver, or ASIO4ALL over its USB-audio interface) and run this
 * BEFORE the full rig to confirm the chain works: the device enumerates, opens at the engine rate,
 * exposes its 19 input channels, and every capsule is live and correctly mapped — tap a capsule and
 * watch its channel jump. A channel stuck at digital silence is dead or unmapped. The 19 capsules
 * share one ADC (mutually sample-locked), which is why the ZM-1 can be a second device from the Dante
 * output without hurting the DOA (see docs/calibration.md).
 *
 * The LIVE DOA view (clap -> a dot on the capsule sphere, verifying mapping + geometry in one gesture)
 * lives in bw_calib_view's Zylia tab now — same capture shell (zylia_capture.cpp), richer display,
 * and a --tests-able UI. This tool stays a dependency-free console check.
 *
 *   zylia_probe --list                            # enumerate ASIO drivers + their channel counts
 *   zylia_probe [--driver name] [--rate 48000]    # open + live meter (auto-picks a Zylia/ASIO4ALL driver)
 *
 * Build: -DBWAUDIO_BUILD_CALIBRATE=ON with the ASIO SDK (same gate as calibrate).
 */
#include "zylia.h"
#include "zylia_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef BW_HAVE_ASIO
int main(void) {
    fprintf(stderr, "zylia_probe: built without ASIO. Reconfigure with the ASIO SDK (BWAUDIO_WITH_ASIO).\n"
                    "(the hardware-free DOA demo lives in bw_calib_view's Zylia tab, simulate mode.)\n");
    return 1;
}
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <conio.h>

int main(int argc, char** argv) {
    const char* driver = getenv("BWAUDIO_ASIO_DRIVER");
    double rate = 48000.0;
    int    do_list = 0;
    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--list"))                  do_list = 1;
        else if (!strcmp(argv[i], "--driver") && i+1 < argc)  driver  = argv[++i];
        else if (!strcmp(argv[i], "--rate")   && i+1 < argc)  rate    = atof(argv[++i]);
        else if (!strcmp(argv[i], "--console"))               ;   /* legacy no-op: console is all there is now */
        else if (!strcmp(argv[i], "--simulate")) {
            fprintf(stderr, "zylia_probe: the DOA view (and its simulate mode) moved to bw_calib_view (Zylia tab).\n");
            return 2;
        }
        else { fprintf(stderr, "usage: zylia_probe [--list] [--driver name] [--rate hz]\n"); return 2; }
    }
    if (do_list) return zylia_capture_list();

    ZpShared* sh = zylia_capture_open(driver, rate);
    if (!sh) return 1;
    int ncap = sh->nch < ZYLIA_MICS ? sh->nch : ZYLIA_MICS;

    printf("\nTap a capsule -> its channel jumps. (a 'live' channel carries real audio; a dead/unmapped\n");
    printf("one sits at digital silence.) Press any key to stop.\n\n");

    const float LIVE = 1e-4f;                            /* ~-80 dBFS: above digital silence */
    while (!_kbhit()) {
        Sleep(150);
        int live = 0, peak = -1; float peakv = 0.f;
        for (int c = 0; c < ncap; ++c) {
            float r = sh->rms[c];
            if (r > LIVE) ++live;
            if (r > peakv) { peakv = r; peak = c; }
        }
        double pkdb = peakv > 1e-9f ? 20.0 * log10(peakv) : -120.0;
        printf("\r  %2d/%d channels live   loudest: ch %2d @ %6.1f dBFS   (%ld blocks)        ",
               live, ncap, peak, pkdb, sh->blocks);
        fflush(stdout);
    }
    _getch();

    printf("\n\nfinal per-channel RMS:\n");                /* the full picture on the way out */
    for (int c = 0; c < ncap; ++c) {
        float r = sh->rms[c];
        double db = r > 1e-9f ? 20.0 * log10(r) : -120.0;
        int bars = (int)((db + 60.0) / 3.0); if (bars < 0) bars = 0; if (bars > 20) bars = 20;
        char bar[21]; for (int b = 0; b < 20; ++b) bar[b] = b < bars ? '#' : '.'; bar[20] = 0;
        printf("  ch %2d  %6.1f dB |%s|%s\n", c, db, bar, (r > LIVE) ? "" : "  (silent?)");
    }

    zylia_capture_close();
    printf("\nzylia_probe: stopped.\n");
    return 0;
}
#endif /* BW_HAVE_ASIO */
