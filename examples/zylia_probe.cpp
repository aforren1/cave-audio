/*
 * zylia_probe.cpp — "is the ZM-1 talking?" hardware bring-up tool (input-only ASIO).
 *
 * Plug in the Zylia ZM-1 (its own ASIO driver, or ASIO4ALL over its USB-audio interface) and run this
 * BEFORE the full rig to confirm the chain works: the device enumerates, opens at the engine rate,
 * exposes its 19 input channels, and every capsule is live and correctly mapped. It opens the input
 * and prints a live per-channel RMS meter — tap a capsule and watch its channel jump, so you can verify
 * all 19 respond and learn which channel is which capsule. No output, no sweep: purely "does it stream".
 * The 19 capsules share one ADC (mutually sample-locked), which is why the ZM-1 can be a second device
 * from the Dante output without hurting the DOA (see docs/calibration.md).
 *
 *   zylia_probe --list                            # enumerate ASIO drivers + their channel counts
 *   zylia_probe [--driver name] [--rate 48000]    # open + live meter (auto-picks a Zylia/ASIO4ALL driver)
 *
 * Build: -DBWAUDIO_BUILD_CALIBRATE=ON with the ASIO SDK (same gate as calibrate). There is no simulate
 * mode — this IS the hardware test. The localization math it feeds is in zylia.c (unit-tested separately).
 */
#include "zylia.h"          /* ZYLIA_MICS = the expected capsule count */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef BW_HAVE_ASIO
int main(void) {
    fprintf(stderr, "zylia_probe: built without ASIO. Reconfigure with the ASIO SDK (BWAUDIO_WITH_ASIO).\n");
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

namespace {
struct Probe {
    long  bufsize, nin;
    ASIOBufferInfo  bi[MAXCH];
    ASIOChannelInfo ci[MAXCH];
    ASIOCallbacks   cb;
    std::atomic<float> rms[MAXCH];     /* smoothed per-channel RMS (linear); audio thread writes, UI reads */
    std::atomic<long>  blocks;         /* total callbacks (liveness) */
    float sm[MAXCH];                   /* smoothing state — audio thread only */
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

void buffer_switch(long index, ASIOBool) {
    const long n = g.bufsize;
    for (long c = 0; c < g.nin; ++c) {
        double acc = block_sos(g.bi[c].buffers[index], n, g.ci[c].type);
        float  r   = (float)sqrt(acc / (n > 0 ? n : 1));
        g.sm[c]    = 0.8f * g.sm[c] + 0.2f * r;        /* a little smoothing so the meter doesn't flicker */
        g.rms[c].store(g.sm[c], std::memory_order_relaxed);
    }
    g.blocks.fetch_add(1, std::memory_order_relaxed);
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
    int    do_list = 0;
    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--list"))                  do_list = 1;
        else if (!strcmp(argv[i], "--driver") && i+1 < argc)  driver  = argv[++i];
        else if (!strcmp(argv[i], "--rate")   && i+1 < argc)  rate    = atof(argv[++i]);
        else { fprintf(stderr, "usage: zylia_probe [--list] [--driver name] [--rate hz]\n"); return 2; }
    }
    if (do_list) return list_drivers();

    memset(&g, 0, sizeof g);

    /* open: the named driver, else auto-pick (prefer one whose name says "zylia", then any with inputs). */
    auto tryopen = [&](const char* nm) -> bool {
        if (!loadAsioDriver((char*)nm)) return false;
        ASIODriverInfo di; memset(&di, 0, sizeof di); di.asioVersion = 2; di.sysRef = GetDesktopWindow();
        if (ASIOInit(&di) != ASE_OK) { asioDrivers->removeCurrentDriver(); return false; }
        long nin = 0, nout = 0;
        if (ASIOGetChannels(&nin, &nout) != ASE_OK || nin <= 0) { ASIOExit(); asioDrivers->removeCurrentDriver(); return false; }
        g.nin = nin < MAXCH ? nin : MAXCH;
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

    printf("\nstreaming %ld ch @ %.0f Hz, block %ld. Tap a capsule -> its channel jumps.\n", g.nin, rate, g.bufsize);
    printf("(a 'live' channel carries real audio; a dead/unmapped one sits at digital silence.) Press any key to stop.\n\n");

    const float LIVE = 1e-4f;                            /* ~-80 dBFS: above digital silence */
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

    printf("\n\nfinal per-channel RMS:\n");                /* the full picture on the way out */
    for (long c = 0; c < g.nin; ++c) {
        float r = g.rms[c].load(std::memory_order_relaxed);
        double db = r > 1e-9f ? 20.0 * log10(r) : -120.0;
        int bars = (int)((db + 60.0) / 3.0); if (bars < 0) bars = 0; if (bars > 20) bars = 20;
        char bar[21]; for (int b = 0; b < 20; ++b) bar[b] = b < bars ? '#' : '.'; bar[20] = 0;
        printf("  ch %2ld  %6.1f dB |%s|%s\n", c, db, bar, (r > LIVE) ? "" : "  (silent?)");
    }

    ASIOStop(); ASIODisposeBuffers(); ASIOExit(); if (asioDrivers) asioDrivers->removeCurrentDriver();
    printf("\nzylia_probe: stopped.\n");
    return 0;
}
#endif /* BW_HAVE_ASIO */
