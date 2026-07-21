/*
 * valid_capture.cpp — full-duplex ASIO for the phantom-localization harness: nspk speaker feeds out,
 * 19 Zylia capsules in, one device, one clock. See valid_capture.h for the model and the caveats.
 *
 * Rig bring-up code. It mirrors calib_capture.cpp's host sequence (which mirrors asio_sink.cpp's)
 * and carries the same warning: NOT verified on hardware.
 */
#include "valid_capture.h"

#include <stdio.h>
#include <string.h>

#ifdef BWA_HAVE_ASIO

/* NOT wrapped in extern "C": the SDK builds these with C++ linkage (matches asio_sink.cpp).
 * asiosys.h first — it defines IEEE754_64FLOAT, without which ASIOSampleRate is a byte struct
 * rather than a double and every rate cast fails to compile. */
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

namespace {
struct Val {
    long             bufsize;
    int              nspk;                                   /* layout speaker count */
    int              mic_in;                                 /* first Zylia input channel */
    ASIOBufferInfo   bi[BWA_CHANNELS + ZYLIA_MICS];
    ASIOChannelInfo  ci[BWA_CHANNELS + ZYLIA_MICS];
    ASIOCallbacks    cb;
    const float*     feeds;                                  /* [nspk][VAL_CAPLEN] */
    float*           cap;                                    /* [ZYLIA_MICS][VAL_CAPLEN], internal */
    std::atomic<int> running;
    std::atomic<int> done;
    uint32_t         pos;                                     /* frames played/recorded so far */
} g;

static const float g_zero[1024] = { 0 };                     /* type-correct silence via the converter */

void buffer_switch(long index, ASIOBool) {
    long n = g.bufsize;
    int  run = g.running.load(std::memory_order_acquire);
    uint32_t pos = g.pos;

    for (int c = 0; c < g.nspk; ++c) {
        void* dst = g.bi[c].buffers[index];
        if (run && pos < VAL_CAPLEN) {
            float blk[1024];                                 /* bufsize <= 1024 enforced at open */
            for (long i = 0; i < n; ++i) {
                uint32_t t = pos + (uint32_t)i;
                blk[i] = (t < VAL_CAPLEN) ? g.feeds[(size_t)c * VAL_CAPLEN + t] : 0.0f;
            }
            asio_float_to_out(dst, blk, n, g.ci[c].type);
        } else {
            asio_float_to_out(dst, g_zero, n, g.ci[c].type);  /* NOT memset(n*4): a 2/3-byte
                                                               * buffer would overrun */
        }
    }

    if (run && pos < VAL_CAPLEN) {
        uint32_t rem = VAL_CAPLEN - pos;
        long take = ((uint32_t)n < rem) ? n : (long)rem;
        for (int j = 0; j < ZYLIA_MICS; ++j) {
            int slot = g.nspk + j;
            asio_in_to_float(g.cap + (size_t)j * VAL_CAPLEN + pos,
                             g.bi[slot].buffers[index], take, g.ci[slot].type);
        }
        g.pos = pos + (uint32_t)n;
        if (g.pos >= VAL_CAPLEN) g.done.store(1, std::memory_order_release);
    }
    ASIOOutputReady();
}
ASIOTime* buffer_switch_ti(ASIOTime* t, long index, ASIOBool pn) { buffer_switch(index, pn); return t; }
void rate_changed(ASIOSampleRate) {}
long asio_msg(long s, long v, void*, double*) {
    if (s == kAsioSelectorSupported) return (v==kAsioEngineVersion||v==kAsioSupportsTimeInfo)?1:0;
    if (s == kAsioEngineVersion) return 2;
    if (s == kAsioSupportsTimeInfo) return 1;
    return 0;
}

bool open_driver(const char* want, int mic_in, int nspk) {
    char names[16][32], *ptr[16];
    for (int i = 0; i < 16; ++i) ptr[i] = names[i];
    auto tryone = [&](const char* nm)->bool {
        if (!loadAsioDriver((char*)nm)) return false;
        ASIODriverInfo di; memset(&di, 0, sizeof di); di.asioVersion = 2; di.sysRef = GetDesktopWindow();
        if (ASIOInit(&di) != ASE_OK) { asioDrivers->removeCurrentDriver(); return false; }
        long nin = 0, nout = 0;
        /* 19 capsules are non-negotiable: a device exposing fewer would feed the SH projection
         * silent channels and point somewhere confidently wrong (same rule as the DOA view). */
        if (ASIOGetChannels(&nin, &nout) != ASE_OK || nout < nspk || nin < mic_in + ZYLIA_MICS) {
            ASIOExit(); asioDrivers->removeCurrentDriver(); return false;
        }
        return true;
    };
    if (want && *want) return tryone(want);
    long nd = asioDrivers ? asioDrivers->getDriverNames(ptr, 16) : 0;
    for (long i = 0; i < nd; ++i) if (tryone(names[i])) return true;
    return false;
}
} /* namespace */

int valid_asio_open(const char* driver, int mic_in, int nspk) {
    if (mic_in < 0) { fprintf(stderr, "valid_capture: mic input channel must be >= 0 (got %d)\n", mic_in); return 1; }
    if (nspk < 4 || nspk > BWA_CHANNELS) { fprintf(stderr, "valid_capture: speaker count %d out of range\n", nspk); return 1; }
    if (!asio_session_acquire("the validation capture (bwa_validate)")) return 1;

    float* cap = (float*)calloc((size_t)ZYLIA_MICS * VAL_CAPLEN, sizeof(float));
    if (!cap) { fprintf(stderr, "valid_capture: out of memory\n"); asio_session_release(); return 1; }
    memset(&g, 0, sizeof g);
    g.nspk = nspk; g.mic_in = mic_in; g.cap = cap;

    if (!open_driver(driver, mic_in, nspk)) {
        fprintf(stderr, "valid_capture: no ASIO driver with >=%d outputs and %d inputs from %d\n",
                nspk, ZYLIA_MICS, mic_in);
        free(cap); asio_session_release(); return 1;
    }
    long bmin = 0, bmax = 0, bpref = 0, bgran = 0;
    ASIOGetBufferSize(&bmin, &bmax, &bpref, &bgran);
    long bs = bpref > 1024 ? 1024 : bpref;
    if (bs < bmin) bs = bmin;
    if (bs > 1024) {
        fprintf(stderr, "valid_capture: driver minimum buffer %ld exceeds the 1024 limit\n", bs);
        ASIOExit(); asioDrivers->removeCurrentDriver(); free(cap); asio_session_release(); return 1;
    }
    g.bufsize = bs;
    if (ASIOCanSampleRate((ASIOSampleRate)VAL_FS) != ASE_OK ||
        ASIOSetSampleRate((ASIOSampleRate)VAL_FS) != ASE_OK) {
        fprintf(stderr, "valid_capture: driver cannot run at %.0f Hz\n", VAL_FS);
        ASIOExit(); asioDrivers->removeCurrentDriver(); free(cap); asio_session_release(); return 1;
    }
    for (int c = 0; c < nspk; ++c) { g.bi[c].isInput = ASIOFalse; g.bi[c].channelNum = c; }
    for (int j = 0; j < ZYLIA_MICS; ++j) {
        g.bi[nspk + j].isInput = ASIOTrue;
        g.bi[nspk + j].channelNum = mic_in + j;
    }
    g.cb.bufferSwitch = &buffer_switch; g.cb.sampleRateDidChange = &rate_changed;
    g.cb.asioMessage  = &asio_msg;      g.cb.bufferSwitchTimeInfo = &buffer_switch_ti;
    const int nch = nspk + ZYLIA_MICS;
    for (int c = 0; c < nch; ++c) {
        g.ci[c].channel = g.bi[c].channelNum; g.ci[c].isInput = g.bi[c].isInput;
        ASIOGetChannelInfo(&g.ci[c]);
    }
    if (ASIOCreateBuffers(g.bi, nch, g.bufsize, &g.cb) != ASE_OK) {
        fprintf(stderr, "valid_capture: ASIOCreateBuffers failed (%d channels)\n", nch);
        ASIOExit(); asioDrivers->removeCurrentDriver(); free(cap); asio_session_release(); return 1;
    }
    /* Logged for the record, NOT used: the steady-state stimulus makes the measurement
     * latency-independent (see valid_capture.h). If these numbers look absurd, the routing is wrong
     * — which is worth knowing before spending an afternoon collecting cells. */
    { long il = 0, ol = 0;
      if (ASIOGetLatencies(&il, &ol) == ASE_OK)
          printf("valid_capture: driver latency out %ld + in %ld frames (%.1f ms round trip) — not used, steady state\n",
                 ol, il, 1e3 * (double)(ol + il) / VAL_FS); }
    if (ASIOStart() != ASE_OK) {
        fprintf(stderr, "valid_capture: ASIOStart failed\n");
        ASIODisposeBuffers(); ASIOExit(); asioDrivers->removeCurrentDriver();
        free(cap); asio_session_release(); return 1;
    }
    return 0;
}

int valid_asio_capture(const float* feeds, float* cap19) {
    if (!feeds || !cap19 || !g.cap) return 0;
    g.feeds = feeds;
    g.pos = 0;
    g.done.store(0, std::memory_order_release);
    g.running.store(1, std::memory_order_release);

    /* watchdog: VAL_CAPLEN at 48 kHz is ~370 ms, so 5 s is a very generous ceiling */
    for (int waited = 0; !g.done.load(std::memory_order_acquire); waited += 5) {
        if (waited > 5000) {
            g.running.store(0, std::memory_order_release);
            fprintf(stderr, "valid_capture: timed out waiting for the capture to fill\n");
            return 0;
        }
        Sleep(5);
    }
    g.running.store(0, std::memory_order_release);

    /* hand back only the steady-state tail; the head covered latency and build-up */
    for (int j = 0; j < ZYLIA_MICS; ++j)
        memcpy(cap19 + (size_t)j * VAL_ANALYZE,
               g.cap + (size_t)j * VAL_CAPLEN + VAL_SKIP,
               sizeof(float) * VAL_ANALYZE);
    return 1;
}

void valid_asio_close(void) {
    if (!g.cap) return;
    g.running.store(0, std::memory_order_release);
    ASIOStop(); ASIODisposeBuffers(); ASIOExit();
    if (asioDrivers) asioDrivers->removeCurrentDriver();
    free(g.cap);
    memset(&g, 0, sizeof g);
    asio_session_release();
}

#endif /* BWA_HAVE_ASIO */
