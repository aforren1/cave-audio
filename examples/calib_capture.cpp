/*
 * calib_capture.cpp — see calib_capture.h. Moved verbatim out of calibrate.cpp so the CLI and
 * bw_calib_view's Capture tab share ONE copy of the sweep-capture backends.
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

#ifdef BW_HAVE_ASIO
/* ===== ASIO full-duplex capture (rig only; not run in this environment) ===== */
/* NOT wrapped in extern "C": the SDK builds these with C++ linkage (matches asio_sink.cpp). */
#include "asiosys.h"
#include "asio.h"
#include "asiodrivers.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>

extern AsioDrivers* asioDrivers;
extern bool loadAsioDriver(char* name);

namespace {
struct Cal {
    long            bufsize;
    ASIOBufferInfo  bi[BW_CHANNELS + 1];      /* 26 outputs + 1 mic input (last) */
    ASIOChannelInfo ci[BW_CHANNELS + 1];
    ASIOCallbacks   cb;
    const float*    sweep;
    float*          cap;
    std::atomic<int> active;                  /* output channel being measured, -1 = idle */
    std::atomic<int> done;
    int             play_pos, cap_pos;
} g;

int32_t to_i32(float v) { return v >= 1.f ? 2147483647 : v <= -1.f ? -2147483647-1 : (int32_t)(v*2147483647.f); }

void out_block(void* dst, const float* src, long n, long type) {     /* float -> driver type, one channel */
    switch (type) {
    case ASIOSTFloat32LSB: memcpy(dst, src, (size_t)n*sizeof(float)); break;
    case ASIOSTInt32LSB: { int32_t* d=(int32_t*)dst; for (long i=0;i<n;++i) d[i]=to_i32(src[i]); break; }
    case ASIOSTInt24LSB: { uint8_t* d=(uint8_t*)dst; for (long i=0;i<n;++i){ int32_t v=to_i32(src[i])>>8;
                           d[i*3]=v&0xFF; d[i*3+1]=(v>>8)&0xFF; d[i*3+2]=(v>>16)&0xFF; } break; }
    case ASIOSTInt16LSB: { int16_t* d=(int16_t*)dst; for (long i=0;i<n;++i){ float v=src[i];
                           v=v>1.f?1.f:(v<-1.f?-1.f:v); d[i]=(int16_t)(v*32767.f);} break; }
    default: memset(dst, 0, (size_t)n*4); break;
    }
}
void in_block(float* dst, const void* src, long n, long type) {      /* driver type -> float, the mic */
    switch (type) {
    case ASIOSTFloat32LSB: memcpy(dst, src, (size_t)n*sizeof(float)); break;
    case ASIOSTInt32LSB: { const int32_t* s=(const int32_t*)src; for (long i=0;i<n;++i) dst[i]=s[i]/2147483648.f; break; }
    case ASIOSTInt24LSB: { const uint8_t* s=(const uint8_t*)src; for (long i=0;i<n;++i){
                           int32_t v=(s[i*3])|(s[i*3+1]<<8)|(s[i*3+2]<<16); if (v&0x800000) v|=~0xFFFFFF; dst[i]=v/8388608.f; } break; }
    case ASIOSTInt16LSB: { const int16_t* s=(const int16_t*)src; for (long i=0;i<n;++i) dst[i]=s[i]/32768.f; break; }
    default: memset(dst, 0, (size_t)n*sizeof(float)); break;
    }
}

void buffer_switch(long index, ASIOBool) {
    int ac = g.active.load(std::memory_order_acquire);
    long n = g.bufsize;
    for (int c = 0; c < BW_CHANNELS; ++c) {
        void* dst = g.bi[c].buffers[index];
        if (c == ac && g.play_pos < CAL_NSWEEP) {
            float blk[1024];                                 /* bufsize <= 1024 enforced at open */
            for (long i = 0; i < n; ++i) blk[i] = (g.play_pos + i < CAL_NSWEEP) ? g.sweep[g.play_pos + i] : 0.f;
            out_block(dst, blk, n, g.ci[c].type);
        } else {
            memset(dst, 0, (size_t)n * 4);                   /* silence (zero bytes = 0 for all LSB types) */
        }
    }
    if (ac >= 0) {
        int rem = CAL_CAPLEN - g.cap_pos;
        long take = n < rem ? n : rem;
        if (take > 0) in_block(g.cap + g.cap_pos, g.bi[BW_CHANNELS].buffers[index], take, g.ci[BW_CHANNELS].type);
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

bool open_driver(const char* want, int mic_in) {
    char names[16][32], *ptr[16]; for (int i=0;i<16;++i) ptr[i]=names[i];
    auto tryone = [&](const char* nm)->bool {
        if (!loadAsioDriver((char*)nm)) return false;
        ASIODriverInfo di; memset(&di,0,sizeof di); di.asioVersion=2; di.sysRef=GetDesktopWindow();
        if (ASIOInit(&di) != ASE_OK) { asioDrivers->removeCurrentDriver(); return false; }
        long nin=0, nout=0;
        if (ASIOGetChannels(&nin,&nout)!=ASE_OK || nout<BW_CHANNELS || nin<=mic_in) {
            ASIOExit(); asioDrivers->removeCurrentDriver(); return false; }
        return true;
    };
    if (want && *want) return tryone(want);
    long nd = asioDrivers ? asioDrivers->getDriverNames(ptr,16) : 0;
    for (long i=0;i<nd;++i) if (tryone(names[i])) return true;
    return false;
}
} /* namespace */

int calib_asio_open(const char* driver, int mic_in, const float* sweep, float* cap) {
    memset(&g, 0, sizeof g); g.active = -1; g.sweep = sweep; g.cap = cap;
    if (!open_driver(driver, mic_in)) { fprintf(stderr, "calib_capture: no ASIO driver with >=26 out + the mic input\n"); return 1; }
    long bmin=0,bmax=0,bpref=0,bgran=0; ASIOGetBufferSize(&bmin,&bmax,&bpref,&bgran);
    g.bufsize = bpref > 1024 ? 1024 : bpref;                  /* blk[] in buffer_switch is 1024 */
    if (g.bufsize < bmin) g.bufsize = bmin;
    if (ASIOCanSampleRate((ASIOSampleRate)CAL_FS)!=ASE_OK || ASIOSetSampleRate((ASIOSampleRate)CAL_FS)!=ASE_OK) {
        fprintf(stderr, "calib_capture: driver cannot run at %.0f Hz\n", CAL_FS); ASIOExit(); asioDrivers->removeCurrentDriver(); return 1; }
    for (int c = 0; c < BW_CHANNELS; ++c) { g.bi[c].isInput=ASIOFalse; g.bi[c].channelNum=c; }
    g.bi[BW_CHANNELS].isInput=ASIOTrue;  g.bi[BW_CHANNELS].channelNum=mic_in;
    g.cb.bufferSwitch=&buffer_switch; g.cb.sampleRateDidChange=&rate_changed;
    g.cb.asioMessage=&asio_msg;       g.cb.bufferSwitchTimeInfo=&buffer_switch_ti;
    for (int c = 0; c <= BW_CHANNELS; ++c) { g.ci[c].channel=g.bi[c].channelNum; g.ci[c].isInput=g.bi[c].isInput; ASIOGetChannelInfo(&g.ci[c]); }
    if (ASIOCreateBuffers(g.bi, BW_CHANNELS+1, g.bufsize, &g.cb)!=ASE_OK) {
        fprintf(stderr, "calib_capture: ASIOCreateBuffers failed\n"); ASIOExit(); asioDrivers->removeCurrentDriver(); return 1; }
    if (ASIOStart()!=ASE_OK) { fprintf(stderr, "calib_capture: ASIOStart failed\n"); ASIODisposeBuffers(); ASIOExit(); asioDrivers->removeCurrentDriver(); return 1; }
    return 0;
}

/* Capture one speaker: drive the sweep on output `ch`, record CAL_CAPLEN samples of the mic. */
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

void calib_asio_close(void) { ASIOStop(); ASIODisposeBuffers(); ASIOExit(); if (asioDrivers) asioDrivers->removeCurrentDriver(); }
#endif /* BW_HAVE_ASIO */
