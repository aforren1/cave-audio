/*
 * calib_capture.cpp — see calib_capture.h. Moved verbatim out of calibrate.cpp so the CLI and
 * bwa_calib_view's Capture tab share ONE copy of the sweep-capture backends.
 */
#include "calib_capture.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>

void calib_write_wav_f32(const char* path, const float* x, int n, int fs) {
    FILE* f = fopen(path, "wb"); if (!f) return;
    int br = fs * 4, ds = n * 4, sz16 = 16, rc = 36 + ds; short fmt = 3, ch = 1, bps = 32, ba = 4;
    fwrite("RIFF",1,4,f); fwrite(&rc,4,1,f); fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f); fwrite(&sz16,4,1,f); fwrite(&fmt,2,1,f); fwrite(&ch,2,1,f);
    fwrite(&fs,4,1,f); fwrite(&br,4,1,f); fwrite(&ba,2,1,f); fwrite(&bps,2,1,f);
    fwrite("data",1,4,f); fwrite(&ds,4,1,f); fwrite(x,4,(size_t)n,f); fclose(f);
}

void calib_sim_capture(int ch, const Layout* L, const float mic[3], const float* sweep, float* cap) {
    memset(cap, 0, (size_t)CAL_CAPLEN * sizeof(float));
    const float* p = L->speakers[ch].pos;
    double dist = sqrt((p[0]-mic[0])*(p[0]-mic[0]) + (p[1]-mic[1])*(p[1]-mic[1]) + (p[2]-mic[2])*(p[2]-mic[2]));
    if (dist < 0.05) dist = 0.05;
    double delay_f = 512.0 + dist / 343.0 * CAL_FS;               /* system latency + time of flight (fractional) */
    int    di = (int)delay_f; float frac = (float)(delay_f - di);
    double sens = 1.0 + 0.15 * sin(ch * 1.3);                     /* deterministic +/- ~1.4 dB wobble */
    float  g    = (float)(sens / dist);                           /* 1/r at the mic */
    for (int i = 0; i < CAL_NSWEEP && di + i + 1 < CAL_CAPLEN; ++i) {   /* fractional delay via linear interp */
        cap[di + i]     += g * sweep[i] * (1.f - frac);
        cap[di + i + 1] += g * sweep[i] * frac;
    }
}

#ifdef BWA_HAVE_ASIO
/* ===== ASIO full-duplex capture (rig only; not run in this environment) ===== */
/* NOT wrapped in extern "C": the SDK builds these with C++ linkage (matches asio_sink.cpp). */
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
struct Cal {
    long            bufsize;
    int             nspk;                     /* the layout's speaker count; inputs ride slots [nspk..] */
    int             nin;                      /* lockstep inputs: 1 = omni mic, 19 = the ZM-1 (--zylia) */
    ASIOBufferInfo  bi[BWA_CHANNELS + CAL_MAX_INPUTS];   /* nspk outputs + nin inputs (after them) */
    ASIOChannelInfo ci[BWA_CHANNELS + CAL_MAX_INPUTS];
    ASIOCallbacks   cb;
    const float*    sweep;
    float*          cap;                      /* [nin][CAL_CAPLEN] flat */
    std::atomic<int> active;                  /* output channel being measured, -1 = idle */
    std::atomic<int> done;
    int             play_pos, cap_pos;
} g;

static const float g_zero[1024] = { 0 };                     /* type-correct silence via asio_float_to_out */

void buffer_switch(long index, ASIOBool) {
    int ac = g.active.load(std::memory_order_acquire);
    long n = g.bufsize;
    for (int c = 0; c < g.nspk; ++c) {
        void* dst = g.bi[c].buffers[index];
        if (c == ac && g.play_pos < CAL_NSWEEP) {
            float blk[1024];                                 /* bufsize <= 1024 enforced at open */
            for (long i = 0; i < n; ++i) blk[i] = (g.play_pos + i < CAL_NSWEEP) ? g.sweep[g.play_pos + i] : 0.f;
            asio_float_to_out(dst, blk, n, g.ci[c].type);
        } else {
            asio_float_to_out(dst, g_zero, n, g.ci[c].type); /* NOT memset(n*4): a 2/3-byte buffer would overrun */
        }
    }
    if (ac >= 0) {
        int rem = CAL_CAPLEN - g.cap_pos;
        long take = n < rem ? n : rem;
        if (take > 0)
            for (int j = 0; j < g.nin; ++j)
                asio_in_to_float(g.cap + (size_t)j * CAL_CAPLEN + g.cap_pos,
                                 g.bi[g.nspk + j].buffers[index], take, g.ci[g.nspk + j].type);
        g.play_pos += (int)n;
        g.cap_pos  += (int)n;
        if (g.cap_pos >= CAL_CAPLEN) g.done.store(1, std::memory_order_release);
    }
    ASIOOutputReady();
}
ASIOTime* buffer_switch_ti(ASIOTime* t, long index, ASIOBool pn) { buffer_switch(index, pn); return t; }
void rate_changed(ASIOSampleRate) {}
long asio_msg(long s, long v, void*, double*) {
    if (s == kAsioSelectorSupported) return (v==kAsioEngineVersion||v==kAsioSupportsTimeInfo)?1:0;
    if (s == kAsioEngineVersion) return 2; if (s == kAsioSupportsTimeInfo) return 1; return 0;
}

bool open_driver(const char* want, int in_first, int nin_want, int nspk) {
    char names[16][32], *ptr[16]; for (int i=0;i<16;++i) ptr[i]=names[i];
    auto tryone = [&](const char* nm)->bool {
        if (!loadAsioDriver((char*)nm)) return false;
        ASIODriverInfo di; memset(&di,0,sizeof di); di.asioVersion=2; di.sysRef=GetDesktopWindow();
        if (ASIOInit(&di) != ASE_OK) { asioDrivers->removeCurrentDriver(); return false; }
        long nin=0, nout=0;
        /* every input is non-negotiable: a device exposing fewer would feed the solve silent
         * channels — for the ZM-1 that is a confident wrong direction (same rule as valid_capture) */
        if (ASIOGetChannels(&nin,&nout)!=ASE_OK || nout<nspk || nin<in_first+nin_want) {
            ASIOExit(); asioDrivers->removeCurrentDriver(); return false; }
        return true;
    };
    if (want && *want) return tryone(want);
    long nd = asioDrivers ? asioDrivers->getDriverNames(ptr,16) : 0;
    for (long i=0;i<nd;++i) if (tryone(names[i])) return true;
    return false;
}
} /* namespace */

/* driver latencies from the last open (calib_asio_latencies); -1 = unknown. Deliberately NOT part
 * of `g` (which is memset per open) so the values survive calib_asio_close — the localize solve
 * cross-checks them after the device is already torn down. */
static long s_lat_in = -1, s_lat_out = -1;

int calib_asio_open_multi(const char* driver, int in_first, int nin, int nspk, const float* sweep, float* cap) {
    if (in_first < 0) { fprintf(stderr, "calib_capture: first input channel must be >= 0 (got %d)\n", in_first); return 1; }
    if (nin < 1 || nin > CAL_MAX_INPUTS) { fprintf(stderr, "calib_capture: input count %d out of range (1..%d)\n", nin, CAL_MAX_INPUTS); return 1; }
    if (nspk < 1 || nspk > BWA_CHANNELS) { fprintf(stderr, "calib_capture: speaker count %d out of range\n", nspk); return 1; }
    if (!asio_session_acquire("the calibration sweep (Capture tab / bwa_calibrate)")) return 1;
    memset(&g, 0, sizeof g); g.active = -1; g.sweep = sweep; g.cap = cap; g.nspk = nspk; g.nin = nin;
    if (!open_driver(driver, in_first, nin, nspk)) {
        fprintf(stderr, "calib_capture: no ASIO driver with >=%d out + %d input(s) from %d\n", nspk, nin, in_first);
        asio_session_release(); return 1; }
    long bmin=0,bmax=0,bpref=0,bgran=0; ASIOGetBufferSize(&bmin,&bmax,&bpref,&bgran);
    long bs = bpref > 1024 ? 1024 : bpref;                    /* prefer <=1024 (blk[] in buffer_switch is 1024) */
    if (bs < bmin) bs = bmin;                                 /* honor the driver minimum ... */
    if (bs > 1024) {                                          /* ... but the stack scratch can't exceed 1024 */
        fprintf(stderr, "calib_capture: driver minimum buffer %ld samples exceeds the 1024 limit\n", bs);
        ASIOExit(); asioDrivers->removeCurrentDriver(); asio_session_release(); return 1;
    }
    g.bufsize = bs;
    if (ASIOCanSampleRate((ASIOSampleRate)CAL_FS)!=ASE_OK || ASIOSetSampleRate((ASIOSampleRate)CAL_FS)!=ASE_OK) {
        fprintf(stderr, "calib_capture: driver cannot run at %.0f Hz\n", CAL_FS); ASIOExit(); asioDrivers->removeCurrentDriver(); asio_session_release(); return 1; }
    for (int c = 0; c < nspk; ++c) { g.bi[c].isInput=ASIOFalse; g.bi[c].channelNum=c; }
    for (int j = 0; j < nin; ++j)  { g.bi[nspk+j].isInput=ASIOTrue; g.bi[nspk+j].channelNum=in_first+j; }
    g.cb.bufferSwitch=&buffer_switch; g.cb.sampleRateDidChange=&rate_changed;
    g.cb.asioMessage=&asio_msg;       g.cb.bufferSwitchTimeInfo=&buffer_switch_ti;
    for (int c = 0; c < nspk+nin; ++c) { g.ci[c].channel=g.bi[c].channelNum; g.ci[c].isInput=g.bi[c].isInput; ASIOGetChannelInfo(&g.ci[c]); }
    if (ASIOCreateBuffers(g.bi, nspk+nin, g.bufsize, &g.cb)!=ASE_OK) {
        fprintf(stderr, "calib_capture: ASIOCreateBuffers failed (%d channels)\n", nspk+nin); ASIOExit(); asioDrivers->removeCurrentDriver(); asio_session_release(); return 1; }
    /* Log the driver's own latencies (final only after CreateBuffers — they depend on the
     * negotiated buffer). out + in is the DIGITAL half of the sweep's round trip; every measured
     * delay contains it, plus DAC/ADC + analog. A rig-day diagnostic: if a solved system latency
     * ever lands BELOW this sum, the measurement chain is misconfigured, and if it lands tens of
     * ms above, look at the Dante latency setting. */
    s_lat_in = s_lat_out = -1;
    { long il = 0, ol = 0;
      if (ASIOGetLatencies(&il, &ol) == ASE_OK && il >= 0 && ol >= 0) {
          s_lat_in = il; s_lat_out = ol;
          printf("calib_capture: driver latency out %ld + in %ld frames (%.2f + %.2f ms) — digital loop %.2f ms = %.3f m at c\n",
                 ol, il, 1e3 * (double)ol / CAL_FS, 1e3 * (double)il / CAL_FS,
                 1e3 * (double)(ol + il) / CAL_FS, 343.0 * (double)(ol + il) / CAL_FS);
      } else printf("calib_capture: driver did not report latencies (ASIOGetLatencies)\n"); }
    if (ASIOStart()!=ASE_OK) { fprintf(stderr, "calib_capture: ASIOStart failed\n"); ASIODisposeBuffers(); ASIOExit(); asioDrivers->removeCurrentDriver(); asio_session_release(); return 1; }
    return 0;
}

int calib_asio_open(const char* driver, int mic_in, int nspk, const float* sweep, float* cap) {
    return calib_asio_open_multi(driver, mic_in, 1, nspk, sweep, cap);
}

int calib_asio_latencies(long* in_frames, long* out_frames) {
    if (s_lat_in < 0 || s_lat_out < 0) return 0;
    if (in_frames)  *in_frames  = s_lat_in;
    if (out_frames) *out_frames = s_lat_out;
    return 1;
}

/* Capture one speaker: drive the sweep on output `ch`, record CAL_CAPLEN samples of every input. */
int calib_asio_capture(int ch) {
    g.play_pos = 0; g.cap_pos = 0;
    g.done.store(0, std::memory_order_relaxed);
    g.active.store(ch, std::memory_order_release);
    for (int spins = 0; !g.done.load(std::memory_order_acquire); ++spins) {
        Sleep(5);
        if (spins > 2000) { g.active.store(-1, std::memory_order_release); return 0; }  /* ~10 s watchdog */
    }
    g.active.store(-1, std::memory_order_release);
    return 1;
}

void calib_asio_close(void) {
    ASIOStop(); ASIODisposeBuffers(); ASIOExit();
    if (asioDrivers) asioDrivers->removeCurrentDriver();
    asio_session_release();
}

/* Registered-driver enumeration for the tools' pickers/--list-drivers: a LOCAL AsioDrivers reads
 * the registry fresh each call and loads nothing, so it needs no session slot and is safe while
 * a capture (or the engine) has a driver open — the zylia shell's pattern. */
int calib_asio_driver_names(char (*names)[32], int max) {
    AsioDrivers list;
    char* ptrs[32];
    if (max > 32) max = 32;
    for (int i = 0; i < max; ++i) ptrs[i] = names[i];
    long n = list.getDriverNames(ptrs, max);
    return n < 0 ? 0 : (int)n;
}

int calib_asio_list(void) {
    char names[32][32];
    int nd = calib_asio_driver_names(names, 32);
    printf("registered ASIO drivers (%d):\n", nd);
    for (int i = 0; i < nd; ++i) printf("  %2d. %s\n", i, names[i]);
    return 0;
}
#else
int calib_asio_driver_names(char (*names)[32], int max) { (void)names; (void)max; return 0; }
int calib_asio_list(void) {
    printf("built without the ASIO SDK - no drivers to list (simulate mode only)\n");
    return 2;
}
#endif /* BWA_HAVE_ASIO */
