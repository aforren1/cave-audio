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
#include "asio_convert.h"              /* shared sample-format converters (after the SDK headers) */
#include "asio_session.h"              /* the one-driver-slot arbitration */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>

extern AsioDrivers* asioDrivers;
extern bool loadAsioDriver(char* name);

#define MAXNAMES     32
#define ZP_RING_N    16384             /* per-capsule capture ring (power of 2); ~340 ms at 48 kHz */
#define ZP_MAX_BLOCK 8192              /* conversion scratch bound; open() refuses larger driver blocks */

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

void buffer_switch(long index, ASIOBool) {
    const long n = g.bufsize;
    const uint64_t M = ZP_RING_N - 1;
    static float tmp[ZP_MAX_BLOCK];                    /* audio thread only; bufsize bound enforced at open */
    float pk = 0.f;
    for (long c = 0; c < g.ncap; ++c) {
        asio_in_to_float(tmp, g.bi[c].buffers[index], n, g.ci[c].type);
        double acc = 0.0;
        for (long i = 0; i < n; ++i) {                 /* one fused pass: ring write + peak + RMS accumulate */
            float v = tmp[i];
            g.ring[c][(g.wabs + i) & M] = v;
            float a = fabsf(v); if (a > pk) pk = a;
            acc += (double)v * v;
        }
        float r = (float)sqrt(acc / (n > 0 ? n : 1));
        g.sm[c] = 0.8f * g.sm[c] + 0.2f * r;           /* a little smoothing so the meter doesn't flicker */
        g.sh.rms[c] = g.sm[c];
    }
    g.wabs += (uint64_t)n;

    /* transient trigger -> snapshot publish. Floor only adapts while idle, so the clap itself never
     * inflates it. */
    switch (g.tstate) {
    case 0:
        if (g.wabs > ZP_RING_N && pk > g.sh.trig_ratio * g.nfloor && pk > g.sh.trig_min) {
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
    g.sh.nfloor = g.nfloor;                            /* publish for the tuning readout */

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
    if (!asio_session_acquire("the ZM-1 capture (Zylia tab / zylia_probe)")) return NULL;
    memset(&g, 0, sizeof g);
    g.nfloor = 0.02f;                                  /* start the trigger floor above quiet-room noise */

    bool ok = false;
    if (driver && *driver) {
        ok = tryopen(driver);
        if (!ok) { fprintf(stderr, "zylia_capture: could not open '%s' (try --list)\n", driver); asio_session_release(); return NULL; }
    } else {
        char names[MAXNAMES][32]; int nd = get_driver_names(names, MAXNAMES);
        for (int i = 0; i < nd && !ok; ++i) {          /* first pass: a driver that looks like the Zylia */
            char low[32]; strncpy(low, names[i], sizeof low - 1); low[sizeof low - 1] = 0; istrlower(low);
            if (strstr(low, "zylia")) ok = tryopen(names[i]);
        }
        for (int i = 0; i < nd && !ok; ++i) ok = tryopen(names[i]);   /* else the first input-capable one */
        if (!ok) { fprintf(stderr, "zylia_capture: no ASIO input device opened. For the ZM-1 use its ASIO driver or ASIO4ALL.\n"); asio_session_release(); return NULL; }
    }

    if (ASIOCanSampleRate((ASIOSampleRate)rate) != ASE_OK || ASIOSetSampleRate((ASIOSampleRate)rate) != ASE_OK)
        fprintf(stderr, "  note: driver would not set %.0f Hz; running at its current rate\n", rate);
    long bmin = 0, bmax = 0, bpref = 0, bgran = 0; ASIOGetBufferSize(&bmin, &bmax, &bpref, &bgran);
    g.bufsize = bpref;
    if (g.bufsize > ZP_MAX_BLOCK) {                    /* the callback's conversion scratch is fixed-size */
        fprintf(stderr, "zylia_capture: driver block %ld exceeds %d\n", g.bufsize, ZP_MAX_BLOCK);
        ASIOExit(); asioDrivers->removeCurrentDriver(); asio_session_release(); return NULL;
    }
    { ASIOSampleRate ar = rate; ASIOGetSampleRate(&ar);            /* the callback's holdoff math + trigger */
      g.sh.nch = (int)g.nin; g.sh.rate = (double)ar; g.sh.title = g.title;     /* read sh, so fill it BEFORE  */
      g.sh.trig_ratio = 8.0f; g.sh.trig_min = 0.005f; g.sh.nfloor = g.nfloor; } /* the stream starts          */
    for (long c = 0; c < g.ncap; ++c) { g.bi[c].isInput = ASIOTrue; g.bi[c].channelNum = c; }
    g.cb.bufferSwitch = &buffer_switch; g.cb.sampleRateDidChange = &rate_changed;
    g.cb.asioMessage  = &asio_msg;      g.cb.bufferSwitchTimeInfo = &buffer_switch_ti;
    for (long c = 0; c < g.ncap; ++c) { g.ci[c].channel = c; g.ci[c].isInput = ASIOTrue; ASIOGetChannelInfo(&g.ci[c]); }

    if (ASIOCreateBuffers(g.bi, g.ncap, g.bufsize, &g.cb) != ASE_OK) {
        fprintf(stderr, "zylia_capture: ASIOCreateBuffers failed\n");
        ASIOExit(); asioDrivers->removeCurrentDriver(); asio_session_release(); return NULL;
    }
    if (ASIOStart() != ASE_OK) {
        fprintf(stderr, "zylia_capture: ASIOStart failed\n");
        ASIODisposeBuffers(); ASIOExit(); asioDrivers->removeCurrentDriver(); asio_session_release(); return NULL;
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
    asio_session_release();
}

#else /* !BW_HAVE_ASIO: stubs so consumers can link unconditionally */

int       zylia_capture_list(void)                    { fprintf(stderr, "zylia_capture: built without ASIO\n"); return 1; }
ZpShared* zylia_capture_open(const char*, double)     { fprintf(stderr, "zylia_capture: built without ASIO (vendor the SDK; see third_party/README.md)\n"); return NULL; }
void      zylia_capture_close(void)                   {}

#endif /* BW_HAVE_ASIO */
