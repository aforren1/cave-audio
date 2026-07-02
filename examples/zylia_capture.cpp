/*
 * zylia_capture.cpp — see zylia_capture.h. Extracted from zylia_probe so the console meter and
 * bw_calib_view's Zylia tab share ONE copy of the ASIO shell (driver open, format conversion,
 * transient trigger, snapshot publish). Build-only-with-ASIO.
 */
#include "zylia_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef BW_HAVE_ASIO

#include "asiosys.h"
#include "asio.h"
#include "asiodrivers.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>

extern AsioDrivers* asioDrivers;
extern bool loadAsioDriver(char* name);

#define MAXNAMES  32
#define ZP_RING_N 16384                /* per-capsule capture ring (power of 2); ~340 ms at 48 kHz */

namespace {
struct Capture {
    long  bufsize, nin, ncap;          /* nin = device inputs; ncap = min(nin, 19) actually captured */
    ASIOBufferInfo  bi[ZYLIA_MICS];
    ASIOChannelInfo ci[ZYLIA_MICS];
    ASIOCallbacks   cb;
    float sm[ZYLIA_MICS];              /* meter smoothing state — audio thread only */
    bool  started;

    /* transient capture (audio thread owns everything but sh.seq's readers): rings + a trigger that
     * trips when a block's peak jumps 8x over the decaying noise floor; once the ring holds
     * ZP_SNAP_N - ZP_SNAP_PRE samples past it, [trig - PRE, trig - PRE + SNAP_N) is copied into
     * sh.snap and sh.seq bumped (see the ZpShared comment for why this handoff is safe here). */
    ZpShared sh;
    float    ring[ZYLIA_MICS][ZP_RING_N];
    uint64_t wabs;                     /* absolute samples written (ring index = wabs & (ZP_RING_N-1)) */
    float    nfloor;                   /* decaying noise-floor peak estimate */
    int      tstate, holdoff;          /* 0 idle / 1 filling / 2 re-arm holdoff */
    uint64_t trig_abs;
    std::atomic<long> blocks;
    char     title[64];
} g;

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
    for (long c = 0; c < g.ncap; ++c) {
        float p = ring_write(g.bi[c].buffers[index], n, g.ci[c].type, g.ring[c], g.wabs);
        if (p > pk) pk = p;
        /* meter: block RMS from the freshly-written ring slice (one pass, cache-hot) */
        const uint64_t M = ZP_RING_N - 1;
        double acc = 0.0;
        for (long i = 0; i < n; ++i) { float v = g.ring[c][(g.wabs + i) & M]; acc += (double)v * v; }
        float r = (float)sqrt(acc / (n > 0 ? n : 1));
        g.sm[c] = 0.8f * g.sm[c] + 0.2f * r;           /* a little smoothing so the meter doesn't flicker */
        g.sh.rms[c] = g.sm[c];
    }
    g.wabs += (uint64_t)n;

    /* transient trigger -> snapshot publish. Floor only adapts while idle, so the clap itself never
     * inflates it. */
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
            for (int c = 0; c < (int)g.ncap; ++c)
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

    long b = g.blocks.fetch_add(1, std::memory_order_relaxed) + 1;
    g.sh.blocks = b;
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

bool tryopen(const char* nm) {
    if (!loadAsioDriver((char*)nm)) return false;
    ASIODriverInfo di; memset(&di, 0, sizeof di); di.asioVersion = 2; di.sysRef = GetDesktopWindow();
    if (ASIOInit(&di) != ASE_OK) { asioDrivers->removeCurrentDriver(); return false; }
    long nin = 0, nout = 0;
    if (ASIOGetChannels(&nin, &nout) != ASE_OK || nin <= 0) { ASIOExit(); asioDrivers->removeCurrentDriver(); return false; }
    g.nin  = nin;
    g.ncap = nin < ZYLIA_MICS ? nin : ZYLIA_MICS;
    snprintf(g.title, sizeof g.title, "%s", nm);
    printf("opened '%s': %ld inputs%s (capturing %ld)\n", nm, nin,
           nin == ZYLIA_MICS ? " — matches the ZM-1's 19" : "", g.ncap);
    return true;
}
} /* namespace */

int zylia_capture_list(void) {
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

ZpShared* zylia_capture_open(const char* driver, double rate) {
    if (g.started) { fprintf(stderr, "zylia_capture: already open\n"); return &g.sh; }
    memset(&g, 0, sizeof g);
    g.nfloor = 0.02f;                                  /* start the trigger floor above quiet-room noise */

    bool ok = false;
    if (driver && *driver) {
        ok = tryopen(driver);
        if (!ok) { fprintf(stderr, "zylia_capture: could not open '%s' (try --list)\n", driver); return NULL; }
    } else {
        char names[MAXNAMES][32]; int nd = get_driver_names(names, MAXNAMES);
        for (int i = 0; i < nd && !ok; ++i) {          /* first pass: a driver that looks like the Zylia */
            char low[32]; strncpy(low, names[i], sizeof low - 1); low[sizeof low - 1] = 0; istrlower(low);
            if (strstr(low, "zylia")) ok = tryopen(names[i]);
        }
        for (int i = 0; i < nd && !ok; ++i) ok = tryopen(names[i]);   /* else the first input-capable one */
        if (!ok) { fprintf(stderr, "zylia_capture: no ASIO input device opened. For the ZM-1 use its ASIO driver or ASIO4ALL.\n"); return NULL; }
    }

    if (ASIOCanSampleRate((ASIOSampleRate)rate) != ASE_OK || ASIOSetSampleRate((ASIOSampleRate)rate) != ASE_OK)
        fprintf(stderr, "  note: driver would not set %.0f Hz; running at its current rate\n", rate);
    long bmin = 0, bmax = 0, bpref = 0, bgran = 0; ASIOGetBufferSize(&bmin, &bmax, &bpref, &bgran);
    g.bufsize = bpref;
    { ASIOSampleRate ar = rate; ASIOGetSampleRate(&ar);            /* the callback's holdoff math reads */
      g.sh.nch = (int)g.nin; g.sh.rate = (double)ar; g.sh.title = g.title; }   /* sh BEFORE the stream starts */
    for (long c = 0; c < g.ncap; ++c) { g.bi[c].isInput = ASIOTrue; g.bi[c].channelNum = c; }
    g.cb.bufferSwitch = &buffer_switch; g.cb.sampleRateDidChange = &rate_changed;
    g.cb.asioMessage  = &asio_msg;      g.cb.bufferSwitchTimeInfo = &buffer_switch_ti;
    for (long c = 0; c < g.ncap; ++c) { g.ci[c].channel = c; g.ci[c].isInput = ASIOTrue; ASIOGetChannelInfo(&g.ci[c]); }

    if (ASIOCreateBuffers(g.bi, g.ncap, g.bufsize, &g.cb) != ASE_OK) {
        fprintf(stderr, "zylia_capture: ASIOCreateBuffers failed\n");
        ASIOExit(); asioDrivers->removeCurrentDriver(); return NULL;
    }
    if (ASIOStart() != ASE_OK) {
        fprintf(stderr, "zylia_capture: ASIOStart failed\n");
        ASIODisposeBuffers(); ASIOExit(); asioDrivers->removeCurrentDriver(); return NULL;
    }
    g.started = true;
    printf("streaming %ld ch @ %.0f Hz, block %ld\n", g.ncap, g.sh.rate, g.bufsize);
    return &g.sh;
}

void zylia_capture_close(void) {
    if (!g.started) return;
    ASIOStop(); ASIODisposeBuffers(); ASIOExit();
    if (asioDrivers) asioDrivers->removeCurrentDriver();
    g.started = false;
}

#else /* !BW_HAVE_ASIO: stubs so consumers can link unconditionally */

int       zylia_capture_list(void)                    { fprintf(stderr, "zylia_capture: built without ASIO\n"); return 1; }
ZpShared* zylia_capture_open(const char*, double)     { fprintf(stderr, "zylia_capture: built without ASIO (vendor the SDK; see third_party/README.md)\n"); return NULL; }
void      zylia_capture_close(void)                   {}

#endif /* BW_HAVE_ASIO */
