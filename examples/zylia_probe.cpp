/*
 * zylia_probe.cpp — "is the ZM-1 talking?" hardware bring-up tool (input-only ASIO).
 *
 * Plug in the Zylia ZM-1 (its own ASIO driver, or ASIO4ALL over its USB-audio interface) and run this
 * BEFORE the full rig to confirm the chain works: the device enumerates, opens at the engine rate,
 * exposes its 19 input channels, and every capsule is live and correctly mapped. Two views:
 *   - console meter (--console, or a build without raylib): live per-channel RMS — tap a capsule and
 *     watch its channel jump. Purely "does it stream".
 *   - LIVE DOA GUI (default when built with the playground gate too): the audio callback watches for a
 *     transient (a clap), snapshots all 19 channels, and the GUI runs zylia_tdoa -> zylia_doa on it —
 *     a dot appears on the capsule sphere where the clap came from. Verifies capsule mapping AND the
 *     geometry table in seconds (swapped channels / wrong geometry put the dot somewhere absurd).
 * The 19 capsules share one ADC (mutually sample-locked), which is why the ZM-1 can be a second device
 * from the Dante output without hurting the DOA (see docs/calibration.md).
 *
 *   zylia_probe --list                            # enumerate ASIO drivers + their channel counts
 *   zylia_probe [--driver name] [--rate 48000]    # open + DOA GUI (auto-picks a Zylia/ASIO4ALL driver)
 *   zylia_probe --console                         # the plain console meter instead
 *   zylia_probe --simulate                        # no hardware: synthesized claps through the same
 *                                                 #   snapshot->tdoa->doa->draw pipeline (GUI check)
 *
 * Build: -DBWAUDIO_BUILD_CALIBRATE=ON with the ASIO SDK (same gate as calibrate); the GUI needs
 * -DBWAUDIO_BUILD_PLAYGROUND=ON as well (raylib). The math is in zylia.c (unit-tested separately);
 * this file is the rig-bound ASIO shell.
 */
#include "zylia.h"              /* ZYLIA_MICS = the expected capsule count */
#include "zylia_probe_gui.h"    /* ZpShared seam (no raylib in it; zylia_gui_run only exists with BW_HAVE_RAYLIB) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef BW_HAVE_ASIO
int main(int argc, char** argv) {
#ifdef BW_HAVE_RAYLIB
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--simulate")) { static ZpShared sh; memset(&sh, 0, sizeof sh); zylia_gui_run(&sh, 1); return 0; }
#endif
    (void)argc; (void)argv;
    fprintf(stderr, "zylia_probe: built without ASIO. Reconfigure with the ASIO SDK (BWAUDIO_WITH_ASIO); --simulate needs the playground gate (raylib).\n");
    return 1;
}
#else
#include "asiosys.h"
#include "asio.h"
#include "asiodrivers.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <conio.h>
#include <atomic>

extern AsioDrivers* asioDrivers;
extern bool loadAsioDriver(char* name);

#define MAXCH    64
#define MAXNAMES 32
#define ZP_RING_N 16384                /* per-capsule capture ring (power of 2); ~340 ms at 48 kHz */

namespace {
struct Probe {
    long  bufsize, nin;
    ASIOBufferInfo  bi[MAXCH];
    ASIOChannelInfo ci[MAXCH];
    ASIOCallbacks   cb;
    std::atomic<float> rms[MAXCH];     /* smoothed per-channel RMS (linear); audio thread writes, UI reads */
    std::atomic<long>  blocks;         /* total callbacks (liveness) */
    float sm[MAXCH];                   /* smoothing state — audio thread only */

    /* transient capture for the DOA view (audio thread owns everything but sh.seq's readers).
     * The first 19 channels stream into per-capsule rings; a block whose peak jumps 8x over the
     * decaying noise floor trips the trigger, and once the ring holds ZP_SNAP_N - ZP_SNAP_PRE
     * samples past it, the snapshot [trig - PRE, trig - PRE + SNAP_N) is copied into sh.snap and
     * sh.seq bumped (see zylia_probe_gui.h for why this handoff is safe at tool grade). */
    ZpShared sh;
    float    ring[ZYLIA_MICS][ZP_RING_N];
    uint64_t wabs;                     /* absolute samples written (ring index = wabs & (ZP_RING_N-1)) */
    float    nfloor;                   /* decaying noise-floor peak estimate */
    int      tstate, holdoff;          /* 0 idle / 1 filling / 2 re-arm holdoff */
    uint64_t trig_abs;
} g;

/* sum of squares of one channel's block, driver-type aware. */
double block_sos(const void* src, long n, long type) {
    double acc = 0;
    switch (type) {
    case ASIOSTFloat32LSB: { const float*  s=(const float*)src;  for (long i=0;i<n;++i) acc += (double)s[i]*s[i]; break; }
    case ASIOSTInt32LSB:   { const int32_t* s=(const int32_t*)src; for (long i=0;i<n;++i){ double v=s[i]/2147483648.0; acc+=v*v; } break; }
    case ASIOSTInt24LSB:   { const uint8_t* s=(const uint8_t*)src; for (long i=0;i<n;++i){ int32_t v=(s[i*3])|(s[i*3+1]<<8)|(s[i*3+2]<<16); if (v&0x800000) v|=~0xFFFFFF; double f=v/8388608.0; acc+=f*f; } break; }
    case ASIOSTInt16LSB:   { const int16_t* s=(const int16_t*)src; for (long i=0;i<n;++i){ double v=s[i]/32768.0; acc+=v*v; } break; }
    default: break;
    }
    return acc;
}

/* convert one channel's block to float into the capture ring; returns the block's peak |sample|. */
float ring_write(const void* src, long n, long type, float* ring, uint64_t wabs) {
    const uint64_t M = ZP_RING_N - 1;
    float pk = 0.f;
    switch (type) {
    case ASIOSTFloat32LSB: { const float* s = (const float*)src;
        for (long i = 0; i < n; ++i) { float v = s[i]; ring[(wabs + i) & M] = v; float a = fabsf(v); if (a > pk) pk = a; } break; }
    case ASIOSTInt32LSB:   { const int32_t* s = (const int32_t*)src;
        for (long i = 0; i < n; ++i) { float v = (float)(s[i] / 2147483648.0); ring[(wabs + i) & M] = v; float a = fabsf(v); if (a > pk) pk = a; } break; }
    case ASIOSTInt24LSB:   { const uint8_t* s = (const uint8_t*)src;
        for (long i = 0; i < n; ++i) { int32_t w = (s[i*3]) | (s[i*3+1] << 8) | (s[i*3+2] << 16); if (w & 0x800000) w |= ~0xFFFFFF;
                                       float v = (float)(w / 8388608.0); ring[(wabs + i) & M] = v; float a = fabsf(v); if (a > pk) pk = a; } break; }
    case ASIOSTInt16LSB:   { const int16_t* s = (const int16_t*)src;
        for (long i = 0; i < n; ++i) { float v = (float)(s[i] / 32768.0); ring[(wabs + i) & M] = v; float a = fabsf(v); if (a > pk) pk = a; } break; }
    default: for (long i = 0; i < n; ++i) ring[(wabs + i) & M] = 0.f; break;
    }
    return pk;
}

void buffer_switch(long index, ASIOBool) {
    const long n = g.bufsize;
    float pk = 0.f;
    for (long c = 0; c < g.nin; ++c) {
        double acc = block_sos(g.bi[c].buffers[index], n, g.ci[c].type);
        float  r   = (float)sqrt(acc / (n > 0 ? n : 1));
        g.sm[c]    = 0.8f * g.sm[c] + 0.2f * r;        /* a little smoothing so the meter doesn't flicker */
        g.rms[c].store(g.sm[c], std::memory_order_relaxed);
        if (c < ZYLIA_MICS) {                          /* DOA capture: first 19 channels into the rings */
            g.sh.rms[c] = g.sm[c];
            float p = ring_write(g.bi[c].buffers[index], n, g.ci[c].type, g.ring[c], g.wabs);
            if (p > pk) pk = p;
        }
    }
    g.wabs += (uint64_t)n;

    /* transient trigger -> snapshot publish (see the struct comment). Floor only adapts while idle,
     * so the clap itself never inflates it. */
    switch (g.tstate) {
    case 0:
        if (g.wabs > ZP_RING_N && pk > 8.f * g.nfloor && pk > 0.005f) {
            g.trig_abs = g.wabs - (uint64_t)n;         /* block start; ZP_SNAP_PRE covers the in-block offset */
            g.tstate = 1;
        } else g.nfloor = 0.98f * g.nfloor + 0.02f * pk;
        break;
    case 1:
        if (g.wabs >= g.trig_abs + (ZP_SNAP_N - ZP_SNAP_PRE)) {
            const uint64_t M = ZP_RING_N - 1, s0 = g.trig_abs - ZP_SNAP_PRE;
            for (int c = 0; c < ZYLIA_MICS && c < g.nin; ++c)
                for (int i = 0; i < ZP_SNAP_N; ++i) g.sh.snap[c][i] = g.ring[c][(s0 + (uint64_t)i) & M];
            InterlockedIncrement((volatile LONG*)&g.sh.seq);
            g.holdoff = (int)(0.3 * g.sh.rate / (n > 0 ? n : 256)) + 1;   /* ~300 ms before re-arming */
            g.tstate  = 2;
        }
        break;
    default:
        if (--g.holdoff <= 0) g.tstate = 0;
        break;
    }

    g.blocks.fetch_add(1, std::memory_order_relaxed);
    g.sh.blocks = g.blocks.load(std::memory_order_relaxed);
}
ASIOTime* buffer_switch_ti(ASIOTime* t, long index, ASIOBool pn) { buffer_switch(index, pn); return t; }
void rate_changed(ASIOSampleRate) {}
long asio_msg(long s, long v, void*, double*) {
    if (s == kAsioSelectorSupported) return (v==kAsioEngineVersion||v==kAsioSupportsTimeInfo)?1:0;
    if (s == kAsioEngineVersion) return 2; if (s == kAsioSupportsTimeInfo) return 1; return 0;
}

int get_driver_names(char buf[][32], int max) {
    AsioDrivers d;                                     /* local: enumerate the registry without loading */
    char* ptr[MAXNAMES];
    if (max > MAXNAMES) max = MAXNAMES;
    for (int i = 0; i < max; ++i) ptr[i] = buf[i];
    return (int)d.getDriverNames(ptr, max);
}

void istrlower(char* s) { for (; *s; ++s) if (*s>='A'&&*s<='Z') *s += 32; }
} /* namespace */

static int list_drivers(void) {
    char names[MAXNAMES][32];
    int nd = get_driver_names(names, MAXNAMES);
    printf("ASIO drivers (%d):\n", nd);
    for (int i = 0; i < nd; ++i) {
        if (!loadAsioDriver(names[i])) { printf("  %2d. %-30s [load failed]\n", i, names[i]); continue; }
        ASIODriverInfo di; memset(&di, 0, sizeof di); di.asioVersion = 2; di.sysRef = GetDesktopWindow();
        if (ASIOInit(&di) != ASE_OK) { printf("  %2d. %-30s [init failed]\n", i, names[i]); asioDrivers->removeCurrentDriver(); continue; }
        long nin = 0, nout = 0; ASIOGetChannels(&nin, &nout);
        ASIOSampleRate sr = 0; ASIOGetSampleRate(&sr);
        printf("  %2d. %-30s  in=%ld out=%ld  rate=%.0f%s\n", i, names[i], nin, nout, (double)sr,
               nin >= ZYLIA_MICS ? "   <- enough inputs for the ZM-1" : "");
        ASIOExit(); asioDrivers->removeCurrentDriver();
    }
    return 0;
}

int main(int argc, char** argv) {
    const char* driver = getenv("BWAUDIO_ASIO_DRIVER");
    double rate = 48000.0;
    int    do_list = 0, do_console = 0, do_sim = 0;
    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--list"))                  do_list    = 1;
        else if (!strcmp(argv[i], "--console"))               do_console = 1;
        else if (!strcmp(argv[i], "--simulate"))              do_sim     = 1;
        else if (!strcmp(argv[i], "--driver") && i+1 < argc)  driver  = argv[++i];
        else if (!strcmp(argv[i], "--rate")   && i+1 < argc)  rate    = atof(argv[++i]);
        else { fprintf(stderr, "usage: zylia_probe [--list] [--console] [--simulate] [--driver name] [--rate hz]\n"); return 2; }
    }
    if (do_list) return list_drivers();
    if (do_sim) {
#ifdef BW_HAVE_RAYLIB
        static ZpShared sh; memset(&sh, 0, sizeof sh);
        zylia_gui_run(&sh, 1);
        return 0;
#else
        fprintf(stderr, "zylia_probe: --simulate needs the raylib GUI (build with -DBWAUDIO_BUILD_PLAYGROUND=ON too).\n");
        return 1;
#endif
    }

    memset(&g, 0, sizeof g);
    g.nfloor = 0.02f;                                  /* start the trigger floor above quiet-room noise */

    /* open: the named driver, else auto-pick (prefer one whose name says "zylia", then any with inputs). */
    static char title[64];
    auto tryopen = [&](const char* nm) -> bool {
        if (!loadAsioDriver((char*)nm)) return false;
        ASIODriverInfo di; memset(&di, 0, sizeof di); di.asioVersion = 2; di.sysRef = GetDesktopWindow();
        if (ASIOInit(&di) != ASE_OK) { asioDrivers->removeCurrentDriver(); return false; }
        long nin = 0, nout = 0;
        if (ASIOGetChannels(&nin, &nout) != ASE_OK || nin <= 0) { ASIOExit(); asioDrivers->removeCurrentDriver(); return false; }
        g.nin = nin < MAXCH ? nin : MAXCH;
        snprintf(title, sizeof title, "%s", nm);
        printf("opened '%s': %ld inputs%s (metering %ld)\n", nm, nin,
               nin == ZYLIA_MICS ? " — matches the ZM-1's 19" : "", g.nin);
        return true;
    };

    bool ok = false;
    if (driver && *driver) {
        ok = tryopen(driver);
        if (!ok) { fprintf(stderr, "zylia_probe: could not open '%s'. Try --list.\n", driver); return 1; }
    } else {
        char names[MAXNAMES][32]; int nd = get_driver_names(names, MAXNAMES);
        for (int i = 0; i < nd && !ok; ++i) {           /* first pass: a driver that looks like the Zylia */
            char low[32]; strncpy(low, names[i], sizeof low - 1); low[sizeof low - 1] = 0; istrlower(low);
            if (strstr(low, "zylia")) ok = tryopen(names[i]);
        }
        for (int i = 0; i < nd && !ok; ++i) ok = tryopen(names[i]);   /* else the first input-capable one */
        if (!ok) { fprintf(stderr, "zylia_probe: no ASIO input device opened. Run --list; for the ZM-1 use its ASIO driver or ASIO4ALL.\n"); return 1; }
    }

    if (ASIOCanSampleRate((ASIOSampleRate)rate) != ASE_OK || ASIOSetSampleRate((ASIOSampleRate)rate) != ASE_OK)
        fprintf(stderr, "  note: driver would not set %.0f Hz; running at its current rate\n", rate);
    long bmin = 0, bmax = 0, bpref = 0, bgran = 0; ASIOGetBufferSize(&bmin, &bmax, &bpref, &bgran);
    g.bufsize = bpref;
    g.blocks.store(0);
    { ASIOSampleRate ar = rate; ASIOGetSampleRate(&ar);            /* the callback's holdoff math reads */
      g.sh.nch = (int)g.nin; g.sh.rate = (double)ar; g.sh.title = title; }   /* sh BEFORE the stream starts */
    for (long c = 0; c < g.nin; ++c) { g.bi[c].isInput = ASIOTrue; g.bi[c].channelNum = c; g.rms[c].store(0.f); g.sm[c] = 0.f; }
    g.cb.bufferSwitch = &buffer_switch; g.cb.sampleRateDidChange = &rate_changed;
    g.cb.asioMessage  = &asio_msg;      g.cb.bufferSwitchTimeInfo = &buffer_switch_ti;
    for (long c = 0; c < g.nin; ++c) { g.ci[c].channel = c; g.ci[c].isInput = ASIOTrue; ASIOGetChannelInfo(&g.ci[c]); }

    if (ASIOCreateBuffers(g.bi, g.nin, g.bufsize, &g.cb) != ASE_OK) {
        fprintf(stderr, "zylia_probe: ASIOCreateBuffers failed\n"); ASIOExit(); asioDrivers->removeCurrentDriver(); return 1;
    }
    if (ASIOStart() != ASE_OK) {
        fprintf(stderr, "zylia_probe: ASIOStart failed\n"); ASIODisposeBuffers(); ASIOExit(); asioDrivers->removeCurrentDriver(); return 1;
    }

    printf("\nstreaming %ld ch @ %.0f Hz, block %ld.\n", g.nin, g.sh.rate, g.bufsize);

#ifdef BW_HAVE_RAYLIB
    if (!do_console) {
        printf("DOA view: clap around the array -> a dot on the capsule sphere (close the window to stop).\n\n");
        zylia_gui_run(&g.sh, 0);                         /* returns when the window closes */
    } else
#endif
    {
        (void)do_console;
        printf("Tap a capsule -> its channel jumps. (a 'live' channel carries real audio; a dead/unmapped\n");
        printf("one sits at digital silence.) Press any key to stop.\n\n");
        const float LIVE = 1e-4f;                        /* ~-80 dBFS: above digital silence */
        while (!_kbhit()) {
            Sleep(150);
            int live = 0, peak = -1; float peakv = 0.f;
            for (long c = 0; c < g.nin; ++c) {
                float r = g.rms[c].load(std::memory_order_relaxed);
                if (r > LIVE) ++live;
                if (r > peakv) { peakv = r; peak = (int)c; }
            }
            double pkdb = peakv > 1e-9f ? 20.0 * log10(peakv) : -120.0;
            printf("\r  %2d/%ld channels live   loudest: ch %2d @ %6.1f dBFS   (%ld blocks)        ",
                   live, g.nin, peak, pkdb, g.blocks.load(std::memory_order_relaxed));
            fflush(stdout);
        }
        _getch();
    }

    printf("\n\nfinal per-channel RMS:\n");                /* the full picture on the way out */
    for (long c = 0; c < g.nin; ++c) {
        float r = g.rms[c].load(std::memory_order_relaxed);
        double db = r > 1e-9f ? 20.0 * log10(r) : -120.0;
        int bars = (int)((db + 60.0) / 3.0); if (bars < 0) bars = 0; if (bars > 20) bars = 20;
        char bar[21]; for (int b = 0; b < 20; ++b) bar[b] = b < bars ? '#' : '.'; bar[20] = 0;
        printf("  ch %2ld  %6.1f dB |%s|%s\n", c, db, bar, (r > 1e-4f) ? "" : "  (silent?)");   /* ~-80 dBFS */
    }

    ASIOStop(); ASIODisposeBuffers(); ASIOExit(); if (asioDrivers) asioDrivers->removeCurrentDriver();
    printf("\nzylia_probe: stopped.\n");
    return 0;
}
#endif /* BW_HAVE_ASIO */
