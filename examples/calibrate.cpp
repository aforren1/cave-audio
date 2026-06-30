/*
 * calibrate.c — speaker calibration tool for the CAVE array.
 *
 * Plays an exponential sweep out each speaker in turn, records it at an omnidirectional measurement
 * mic, recovers per-speaker delay + sensitivity (measure.c), turns those into layout trims that
 * arrival-align to the farthest speaker and equalize sensitivity (calib.c), and writes them back into
 * cave_layout.json. Run it once, at the listening position, with the mic where the head will be.
 *
 *   calibrate --layout examples/cave_layout.json --out tuned.json --mic 0 0 0 --input 0
 *   calibrate --simulate                     # no hardware: synthesize captures from the layout geometry
 *
 * Two capture backends:
 *   - ASIO full-duplex (gated on BW_HAVE_ASIO): 26 outputs + one mic input, sample-aligned. Built when
 *     the ASIO SDK is vendored. NOT verified on hardware here — treat as rig bring-up code (it mirrors
 *     asio_sink.cpp's host: load -> ASIOInit -> ASIOGetChannels -> create in+out buffers -> Start).
 *   - simulate: delays/attenuates the sweep per the layout's speaker->mic distances (+ a deterministic
 *     sensitivity wobble) so the whole measure -> solve -> writeback path runs without the rig.
 *
 * Compiled as C++ only because the ASIO host helpers are C++; the engine pieces it calls are C. Built
 * opt-in: cmake -DBWAUDIO_BUILD_CALIBRATE=ON.
 */
extern "C" {
#include "measure.h"
#include "calib.h"
#include "layout.h"
#include "sink.h"          /* BW_CHANNELS */
}

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const double FS        = 48000.0;
static const double F1        = 20.0, F2 = 20000.0;
static const double BAND_HZ[2] = { 300.0, 3000.0 };
static const int    NSWEEP    = 72000;                 /* 1.5 s sweep */
static const int    NTAIL     = 24000;                 /* 0.5 s room-decay tail */
static const int    CAPLEN    = NSWEEP + NTAIL;
static const int    IR_LEN    = 24000;                 /* 0.5 s room kernel retained per speaker (--save-irs) */

/* minimal mono IEEE-float WAV writer for the retained per-speaker impulse responses */
static void write_wav_f32(const char* path, const float* x, int n, int fs) {
    FILE* f = fopen(path, "wb"); if (!f) return;
    int br = fs * 4, ds = n * 4, sz16 = 16, rc = 36 + ds; short fmt = 3, ch = 1, bps = 32, ba = 4;
    fwrite("RIFF",1,4,f); fwrite(&rc,4,1,f); fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f); fwrite(&sz16,4,1,f); fwrite(&fmt,2,1,f); fwrite(&ch,2,1,f);
    fwrite(&fs,4,1,f); fwrite(&br,4,1,f); fwrite(&ba,2,1,f); fwrite(&bps,2,1,f);
    fwrite("data",1,4,f); fwrite(&ds,4,1,f); fwrite(x,4,(size_t)n,f); fclose(f);
}

/* ----- simulate backend: synthesize what an ideal rig would capture for speaker `ch` ----- */
static void simulate_capture(int ch, const Layout* L, const float* mic, const float* sweep, float* cap) {
    memset(cap, 0, (size_t)CAPLEN * sizeof(float));
    const float* p = L->speakers[ch].pos;
    double dist = sqrt((p[0]-mic[0])*(p[0]-mic[0]) + (p[1]-mic[1])*(p[1]-mic[1]) + (p[2]-mic[2])*(p[2]-mic[2]));
    if (dist < 0.05) dist = 0.05;
    int   delay = 512 + (int)(dist / 343.0 * FS);                 /* system latency + time of flight */
    double sens = 1.0 + 0.15 * sin(ch * 1.3);                     /* deterministic +/- ~1.4 dB wobble */
    float  g    = (float)(sens / dist);                           /* 1/r at the mic */
    for (int i = 0; i < NSWEEP && delay + i < CAPLEN; ++i) cap[delay + i] = g * sweep[i];
}

#ifdef BW_HAVE_ASIO
/* ===== ASIO full-duplex capture (rig only; not run in this environment) ===== */
/* NOT wrapped in extern "C": the SDK builds these with C++ linkage (matches asio_sink.cpp). */
#include "asiosys.h"
#include "asio.h"
#include "asiodrivers.h"
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
        if (c == ac && g.play_pos < NSWEEP) {
            float blk[1024];                                 /* bufsize <= 1024 enforced at open */
            for (long i = 0; i < n; ++i) blk[i] = (g.play_pos + i < NSWEEP) ? g.sweep[g.play_pos + i] : 0.f;
            out_block(dst, blk, n, g.ci[c].type);
        } else {
            memset(dst, 0, (size_t)n * 4);                   /* silence (zero bytes = 0 for all LSB types) */
        }
    }
    if (ac >= 0) {
        int rem = CAPLEN - g.cap_pos;
        long take = n < rem ? n : rem;
        if (take > 0) in_block(g.cap + g.cap_pos, g.bi[BW_CHANNELS].buffers[index], take, g.ci[BW_CHANNELS].type);
        g.play_pos += (int)n;
        g.cap_pos  += (int)n;
        if (g.cap_pos >= CAPLEN) g.done.store(1, std::memory_order_release);
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

/* Capture one speaker: drive the sweep on output `ch`, record CAPLEN samples of the mic into g.cap. */
bool asio_capture(int ch) {
    g.play_pos = 0; g.cap_pos = 0;
    g.done.store(0, std::memory_order_relaxed);
    g.active.store(ch, std::memory_order_release);
    for (int spins = 0; !g.done.load(std::memory_order_acquire); ++spins) {
        Sleep(5);
        if (spins > 2000) { g.active.store(-1, std::memory_order_release); return false; }  /* ~10 s watchdog */
    }
    g.active.store(-1, std::memory_order_release);
    return true;
}
} /* namespace */

/* Returns 0 on success; fills g + starts the driver. mic_in = ASIO input channel index of the mic. */
static int asio_open(const char* driver, int mic_in, const float* sweep, float* cap) {
    memset(&g, 0, sizeof g); g.active = -1; g.sweep = sweep; g.cap = cap;
    if (!open_driver(driver, mic_in)) { fprintf(stderr, "calibrate: no ASIO driver with >=26 out + the mic input\n"); return 1; }
    long bmin=0,bmax=0,bpref=0,bgran=0; ASIOGetBufferSize(&bmin,&bmax,&bpref,&bgran);
    g.bufsize = bpref > 1024 ? 1024 : bpref;                  /* blk[] in buffer_switch is 1024 */
    if (g.bufsize < bmin) g.bufsize = bmin;
    if (ASIOCanSampleRate((ASIOSampleRate)FS)!=ASE_OK || ASIOSetSampleRate((ASIOSampleRate)FS)!=ASE_OK) {
        fprintf(stderr, "calibrate: driver cannot run at %.0f Hz\n", FS); ASIOExit(); asioDrivers->removeCurrentDriver(); return 1; }
    for (int c = 0; c < BW_CHANNELS; ++c) { g.bi[c].isInput=ASIOFalse; g.bi[c].channelNum=c; }
    g.bi[BW_CHANNELS].isInput=ASIOTrue;  g.bi[BW_CHANNELS].channelNum=mic_in;
    g.cb.bufferSwitch=&buffer_switch; g.cb.sampleRateDidChange=&rate_changed;
    g.cb.asioMessage=&asio_msg;       g.cb.bufferSwitchTimeInfo=&buffer_switch_ti;
    for (int c = 0; c <= BW_CHANNELS; ++c) { g.ci[c].channel=g.bi[c].channelNum; g.ci[c].isInput=g.bi[c].isInput; ASIOGetChannelInfo(&g.ci[c]); }
    if (ASIOCreateBuffers(g.bi, BW_CHANNELS+1, g.bufsize, &g.cb)!=ASE_OK) {
        fprintf(stderr, "calibrate: ASIOCreateBuffers failed\n"); ASIOExit(); asioDrivers->removeCurrentDriver(); return 1; }
    if (ASIOStart()!=ASE_OK) { fprintf(stderr, "calibrate: ASIOStart failed\n"); ASIODisposeBuffers(); ASIOExit(); asioDrivers->removeCurrentDriver(); return 1; }
    return 0;
}
static void asio_close(void) { ASIOStop(); ASIODisposeBuffers(); ASIOExit(); if (asioDrivers) asioDrivers->removeCurrentDriver(); }
#endif /* BW_HAVE_ASIO */

int main(int argc, char** argv) {
    const char* layout_path = "examples/cave_layout.json";
    const char* out_path    = NULL;
    const char* driver      = getenv("BWAUDIO_ASIO_DRIVER");
    float mic[3] = { 0.f, 0.f, 0.f };
    int   mic_in = 0, simulate = 0, room = 0;
    const char* ir_prefix = NULL;
    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i],"--layout") && i+1<argc) layout_path = argv[++i];
        else if (!strcmp(argv[i],"--out")    && i+1<argc) out_path    = argv[++i];
        else if (!strcmp(argv[i],"--driver") && i+1<argc) driver      = argv[++i];
        else if (!strcmp(argv[i],"--input")  && i+1<argc) mic_in      = atoi(argv[++i]);
        else if (!strcmp(argv[i],"--simulate"))           simulate    = 1;
        else if (!strcmp(argv[i],"--room"))               room        = 1;   /* RT60 + early reflections report */
        else if (!strcmp(argv[i],"--save-irs") && i+1<argc) ir_prefix = argv[++i];  /* dump per-speaker IR WAVs */
        else if (!strcmp(argv[i],"--mic") && i+3<argc) { mic[0]=(float)atof(argv[++i]); mic[1]=(float)atof(argv[++i]); mic[2]=(float)atof(argv[++i]); }
        else { fprintf(stderr, "usage: calibrate [--layout f] [--out f] [--mic x y z] [--input ch] [--driver name] [--simulate] [--room] [--save-irs prefix]\n"); return 2; }
    }
    if (!out_path) out_path = layout_path;                    /* in-place by default */

    char err[256] = {0};
    Layout L;
    if (!layout_load(layout_path, (uint32_t)FS, &L, err, sizeof err)) {
        fprintf(stderr, "calibrate: %s\n", err); return 1;
    }
    const int n = (int)L.count;
    printf("calibrate: %d speakers from %s; mic at (%.2f %.2f %.2f)%s\n",
           n, layout_path, mic[0], mic[1], mic[2], simulate ? "  [SIMULATE]" : "");

    float* sweep = (float*)malloc((size_t)NSWEEP * sizeof(float));
    float* cap   = (float*)malloc((size_t)CAPLEN * sizeof(float));
    MeasureResult* res = (MeasureResult*)calloc((size_t)n, sizeof(MeasureResult));
    if (!sweep || !cap || !res) { fprintf(stderr, "calibrate: out of memory\n"); return 1; }
    measure_sweep(sweep, NSWEEP, F1, F2, FS);
    double rt60_sum = 0.0; int rt60_n = 0;                     /* room-report aggregate */

#ifdef BW_HAVE_ASIO
    int asio_up = 0;
    if (!simulate) { if (asio_open(driver, mic_in, sweep, cap) != 0) return 1; asio_up = 1; }
#else
    if (!simulate) { fprintf(stderr, "calibrate: built without ASIO; re-run with --simulate or build the ASIO backend\n"); return 1; }
#endif

    for (int i = 0; i < n; ++i) {
        if (simulate) {
            simulate_capture(i, &L, mic, sweep, cap);
        } else {
#ifdef BW_HAVE_ASIO
            printf("  speaker %2d: playing sweep...\n", i); fflush(stdout);
            if (!asio_capture(i)) { fprintf(stderr, "calibrate: capture timed out on speaker %d\n", i); asio_close(); return 1; }
#endif
        }
        measure_response(cap, CAPLEN, sweep, NSWEEP, F1, F2, FS, BAND_HZ, &res[i]);
        printf("  speaker %2d: delay=%6d  level=%.4f  bands=[%.3f %.3f %.3f]\n",
               i, res[i].delay_samples, res[i].level, res[i].band[0], res[i].band[1], res[i].band[2]);
        if (room || ir_prefix) {                               /* room report + retained IR kernels */
            RoomResult rr; static float irbuf[IR_LEN];
            measure_room(cap, CAPLEN, sweep, NSWEEP, F1, F2, FS, &rr, ir_prefix ? irbuf : NULL, ir_prefix ? IR_LEN : 0);
            if (room) {
                printf("            RT60=%.3f s  early reflections:", rr.rt60);
                for (int e = 0; e < rr.er_count; ++e) printf(" %.1fms/%.2f", rr.er_delay[e] * 1000.0 / FS, rr.er_level[e]);
                printf("%s\n", rr.er_count ? "" : " (none)");
                if (rr.rt60 > 0.f) { rt60_sum += rr.rt60; ++rt60_n; }
            }
            if (ir_prefix) { char p[512]; snprintf(p, sizeof p, "%s_%02d.wav", ir_prefix, i); write_wav_f32(p, irbuf, IR_LEN, (int)FS); }
        }
    }
    if (room && rt60_n) printf("room: mean RT60 ~ %.3f s (the floor on renderable reverb; treat the room to lower it)\n", rt60_sum / rt60_n);
#ifdef BW_HAVE_ASIO
    if (asio_up) asio_close();
#endif

    float* gdb = (float*)malloc((size_t)n * sizeof(float));
    float* dms = (float*)malloc((size_t)n * sizeof(float));
    /* pack positions contiguously: the Speaker struct has gain/delay between pos[] entries, so it is
     * not a float[n][3] — calib_solve needs a packed [3]-stride array. */
    float (*pos)[3] = (float(*)[3])malloc((size_t)n * 3 * sizeof(float));
    for (int i = 0; i < n; ++i) { pos[i][0]=L.speakers[i].pos[0]; pos[i][1]=L.speakers[i].pos[1]; pos[i][2]=L.speakers[i].pos[2]; }
    calib_solve(res, pos, mic, n, FS, gdb, dms);
    free(pos);

    /* report the spread + write back */
    float gmin=1e9f, gmax=-1e9f, dmax=0.f;
    for (int i = 0; i < n; ++i) { if (gdb[i]<gmin) gmin=gdb[i]; if (gdb[i]>gmax) gmax=gdb[i]; if (dms[i]>dmax) dmax=dms[i]; }
    printf("trims: gain_db in [%.2f, %.2f]  max delay %.3f ms\n", gmin, gmax, dmax);
    for (int i = 0; i < n; ++i) printf("  spk %2d: gain_db=%+.2f  delay_ms=%.3f\n", i, gdb[i], dms[i]);

    if (!calib_write_layout(layout_path, out_path, gdb, dms, n, err, sizeof err)) {
        fprintf(stderr, "calibrate: %s\n", err); return 1;
    }
    printf("calibrate: wrote %s\n", out_path);

    free(gdb); free(dms); free(res); free(cap); free(sweep);
    return 0;
}
