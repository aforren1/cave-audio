/*
 * zylia_capture.cpp — see zylia_capture.h. Extracted from zylia_probe so the console meter and
 * bwa_calib_view's Zylia tab share ONE copy of the ASIO shell (driver open, format conversion,
 * transient trigger, snapshot publish, and the onset stamp each snapshot carries). Build-only-with-ASIO.
 *
 * The callback is a capture TOOL's, not the engine's audio thread, but it keeps the same rules: no
 * allocation, no locks, no file I/O. The clock reads it adds are QPC (os_monotonic_ns) and timeGetTime,
 * both user-mode reads; the engine's own ASIO sink reads QPC in its callback too.
 */
#include "zylia_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef BWA_HAVE_ASIO

#include "asiosys.h"
#include "asio.h"
#include "asiodrivers.h"
#include "asio_convert.h"              /* shared sample-format converters (after the SDK headers) */
#include "asio_session.h"              /* the one-driver-slot arbitration */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>                  /* timeGetTime: the base the SDK says systemTime is on */
#include <atomic>
extern "C" {
#include "os/os.h"                     /* os_monotonic_ns: the host clock the onset stamp is on */
}

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
    double   trig_onset_s;             /* the pending snapshot's onset stamp (published with it) */
    int      trig_src;                 /* ...and how it was derived (ZP_ONSET_*) */
    long     in_lat;                   /* ASIOGetLatencies' input latency, frames; 0 = none given */
    SinkTsClass tsc;                   /* the driver stamp's base (audio thread only) */
    std::atomic<long> blocks;
    char     title[64];
} g;

inline int64_t asio_ns(const ASIOTimeStamp& t) { return (int64_t)(((uint64_t)t.hi << 32) | (uint64_t)t.lo); }

/* host_ns: os_monotonic_ns read FIRST in the callback, before anything else spends time. t: the driver's
 * time info (systemTime valid or not). */
void process_block(long index, const ASIOTime* t, int64_t host_ns) {
    const long n = g.bufsize;
    const uint64_t M = ZP_RING_N - 1;
    static float tmp[ZP_MAX_BLOCK];                    /* audio thread only; bufsize bound enforced at open */
    float pk = 0.f;
    /* the trigger level for THIS block, read once (the UI may drag it mid-block); the onset is the first
     * sample past it on any capsule */
    float thr = g.sh.trig_ratio * g.nfloor;
    if (thr < g.sh.trig_min) thr = g.sh.trig_min;
    long kfirst = n;
    for (long c = 0; c < g.ncap; ++c) {
        asio_in_to_float(tmp, g.bi[c].buffers[index], n, g.ci[c].type);
        double acc = 0.0;
        for (long i = 0; i < n; ++i) {                 /* one fused pass: ring write + peak + RMS accumulate */
            float v = tmp[i];
            g.ring[c][(g.wabs + i) & M] = v;
            float a = fabsf(v); if (a > pk) pk = a;
            if (a > thr && i < kfirst) kfirst = i;
            acc += (double)v * v;
        }
        float r = (float)sqrt(acc / (n > 0 ? n : 1));
        g.sm[c] = 0.8f * g.sm[c] + 0.2f * r;           /* a little smoothing so the meter doesn't flicker */
        g.sh.rms[c] = g.sm[c];
    }
    g.wabs += (uint64_t)n;

    /* this block's buffer-switch time on the host clock: the driver's stamp when it is on that clock
     * (zylia_capture.h, "THE ONSET STAMP"), else the callback-entry read */
    int64_t sw_ns = host_ns;
    int     src = ZP_ONSET_CALLBACK;
    if (t->timeInfo.flags & kSystemTimeValid) {
        const int64_t sys = asio_ns(t->timeInfo.systemTime);
        const int64_t tgt = (int64_t)timeGetTime() * 1000000;      /* a shared-page read, no kernel call */
        const int base = sink_ts_note(&g.tsc, host_ns, tgt, sys);
        if (base == SINK_TS_HOST) { sw_ns = sys; src = ZP_ONSET_DRIVER; }
        g.sh.ts_base = base;
        const int64_t lag = sink_ts_lag_ns(&g.tsc);
        g.sh.ts_lag_ms = (base == SINK_TS_HOST && lag != INT64_MAX) ? (float)((double)lag * 1e-6) : -1.f;
    } else { g.sh.ts_base = SINK_TS_ABSENT; g.sh.ts_lag_ms = -1.f; }

    /* transient trigger -> snapshot publish. Floor only adapts while idle, so the clap itself never
     * inflates it. */
    switch (g.tstate) {
    case 0:
        if (g.wabs > ZP_RING_N && pk > thr) {
            g.trig_abs = g.wabs - (uint64_t)n;         /* block start; ZP_SNAP_PRE covers the in-block offset */
            g.trig_onset_s = zp_onset_s((double)sw_ns * 1e-9, kfirst < n ? kfirst : 0, n, g.in_lat, g.sh.rate);
            g.trig_src = src;
            g.tstate = 1;
        } else g.nfloor = 0.98f * g.nfloor + 0.02f * pk;
        break;
    case 1:
        if (g.wabs >= g.trig_abs + (ZP_SNAP_N - ZP_SNAP_PRE)) {
            const uint64_t M = ZP_RING_N - 1, s0 = g.trig_abs - ZP_SNAP_PRE;
            for (int c = 0; c < (int)g.ncap; ++c)
                for (int i = 0; i < ZP_SNAP_N; ++i) g.sh.snap[c][i] = g.ring[c][(s0 + (uint64_t)i) & M];
            g.sh.onset_s = g.trig_onset_s;             /* with snap[], before the bump: the reader pairs them */
            g.sh.onset_src = g.trig_src;
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
/* The driver calls one of these two. Without time info, ask for the stamp the way the engine's sink does
 * (asio_sink.cpp, bufferSwitch). */
void buffer_switch(long index, ASIOBool) {
    const int64_t host = (int64_t)os_monotonic_ns();
    ASIOTime t; memset(&t, 0, sizeof t);
    if (ASIOGetSamplePosition(&t.timeInfo.samplePosition, &t.timeInfo.systemTime) == ASE_OK)
        t.timeInfo.flags = kSystemTimeValid | kSamplePositionValid;
    process_block(index, &t, host);
}
ASIOTime* buffer_switch_ti(ASIOTime* t, long index, ASIOBool) {
    const int64_t host = (int64_t)os_monotonic_ns();
    process_block(index, t, host);
    return t;
}
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
           nin == ZYLIA_MICS ? ", the ZM-1's 19," : "", g.ncap);
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
    g.sh.block = (int)g.bufsize;
    g.sh.ts_base = SINK_TS_ABSENT; g.sh.ts_lag_ms = -1.f;
    sink_ts_init(&g.tsc, (int)(g.sh.rate / (g.bufsize > 0 ? g.bufsize : 256)) + 1);   /* ~1 s buckets */
    for (long c = 0; c < g.ncap; ++c) { g.bi[c].isInput = ASIOTrue; g.bi[c].channelNum = c; }
    g.cb.bufferSwitch = &buffer_switch; g.cb.sampleRateDidChange = &rate_changed;
    g.cb.asioMessage  = &asio_msg;      g.cb.bufferSwitchTimeInfo = &buffer_switch_ti;
    for (long c = 0; c < g.ncap; ++c) { g.ci[c].channel = c; g.ci[c].isInput = ASIOTrue; ASIOGetChannelInfo(&g.ci[c]); }

    if (ASIOCreateBuffers(g.bi, g.ncap, g.bufsize, &g.cb) != ASE_OK) {
        fprintf(stderr, "zylia_capture: ASIOCreateBuffers failed\n");
        ASIOExit(); asioDrivers->removeCurrentDriver(); asio_session_release(); return NULL;
    }
    {   /* the onset stamp's latency term: the age of a block's first frame at its switch (valid once the
         * buffers exist). A driver that gives none is taken as one block, the least it can be. */
        long il = 0, ol = 0;
        g.in_lat = (ASIOGetLatencies(&il, &ol) == ASE_OK && il > 0) ? il : 0;
        g.sh.in_lat = (int)g.in_lat;
    }
    (void)os_monotonic_ns();                           /* its lazy QPC-frequency read happens here, not in the callback */
    if (ASIOStart() != ASE_OK) {
        fprintf(stderr, "zylia_capture: ASIOStart failed\n");
        ASIODisposeBuffers(); ASIOExit(); asioDrivers->removeCurrentDriver(); asio_session_release(); return NULL;
    }
    g.started = true;
    printf("streaming %ld ch @ %.0f Hz, block %ld, input latency %ld frames%s\n", g.ncap, g.sh.rate, g.bufsize,
           g.in_lat, g.in_lat ? "" : " (driver gave none: one block assumed)");
    return &g.sh;
}

void zylia_capture_close(void) {
    if (!g.started) return;
    ASIOStop(); ASIODisposeBuffers(); ASIOExit();
    if (asioDrivers) asioDrivers->removeCurrentDriver();
    g.started = false;
    asio_session_release();
}

#else /* !BWA_HAVE_ASIO: stubs so consumers can link unconditionally */

int       zylia_capture_list(void)                    { fprintf(stderr, "zylia_capture: built without ASIO\n"); return 1; }
ZpShared* zylia_capture_open(const char*, double)     { fprintf(stderr, "zylia_capture: built without ASIO (vendor the SDK; see third_party/README.md)\n"); return NULL; }
void      zylia_capture_close(void)                   {}

#endif /* BWA_HAVE_ASIO */
