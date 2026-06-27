/*
 * asio_sink.cpp — ASIO host backend (production path: 26 channels -> DVS -> Dante).
 *
 * Compiled ONLY when the Steinberg ASIO SDK is vendored (BW_HAVE_ASIO; see
 * third_party/README.md). C++ because the SDK's driver-loading host helpers
 * (AsioDrivers, loadAsioDriver) are C++. Everything ASIO-specific stays inside this
 * file — the rest of the engine sees only the device-agnostic sink.h interface.
 *
 * Bring-up follows docs/build.md "ASIO host bring-up": load driver -> ASIOInit ->
 * ASIOGetChannels (>=26 out) -> ASIOGetBufferSize -> ASIOGetChannelInfo (sample type)
 * -> ASIOCreateBuffers -> ASIOStart, then convert the planar float bus to the driver's
 * sample type in bufferSwitchTimeInfo and capture the sample-position/systemTime stamp.
 *
 * NOTE: ASIO callbacks carry no user pointer and only one ASIO driver is active at a
 * time, so the live sink is held in a single file-scope pointer (g_sink).
 *
 * Build-only-with-SDK: this file has not been compiled in this environment (no SDK,
 * no 26-ch DVS endpoint). Treat it as M1 code pending on-hardware verification.
 */
extern "C" {
#include "sink.h"
}

#include "asiosys.h"
#include "asio.h"
#include "asiodrivers.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdlib>
#include <cstring>

/* Provided by the SDK host sources (asiodrivers.cpp). */
extern AsioDrivers* asioDrivers;
extern bool loadAsioDriver(char* name);

namespace {

struct AsioSink {
    BwSink          base;
    uint32_t        sample_rate;
    uint32_t        channels;        /* requested (26) */
    long            buffer_size;     /* frames per block, chosen from ASIOGetBufferSize */
    BwRenderFn      render;
    void*           user;
    ASIOBufferInfo  bufferInfos[64];
    ASIOChannelInfo channelInfos[64];
    ASIOCallbacks   callbacks;
    float*          bus;             /* planar channels * buffer_size; engine renders here */
    bool            post_output;     /* driver supports ASIOOutputReady */
    bool            running;
    char            name[64];
};

AsioSink* g_sink = nullptr;

inline uint64_t asio64(unsigned long hi, unsigned long lo) {
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}
inline uint64_t samples_u64(const ASIOSamples& s)   { return asio64(s.hi, s.lo); }
inline uint64_t timestamp_ns(const ASIOTimeStamp& t){ return asio64(t.hi, t.lo); }

inline int32_t to_i32(float v) {
    if (v >=  1.0f) return  2147483647;
    if (v <= -1.0f) return -2147483647 - 1;
    return (int32_t)(v * 2147483647.0f);
}

/* Convert one channel of the planar float bus to the driver's native sample type. */
void convert_out(void* dst, const float* src, long n, long type) {
    switch (type) {
    case ASIOSTFloat32LSB:
        memcpy(dst, src, (size_t)n * sizeof(float));
        break;
    case ASIOSTInt32LSB: {
        int32_t* d = (int32_t*)dst;
        for (long i = 0; i < n; ++i) d[i] = to_i32(src[i]);
        break;
    }
    case ASIOSTInt24LSB: {
        uint8_t* d = (uint8_t*)dst;                 /* packed 3-byte little-endian */
        for (long i = 0; i < n; ++i) {
            int32_t v = to_i32(src[i]) >> 8;        /* top 24 bits */
            d[i*3+0] = (uint8_t)(v       & 0xFF);
            d[i*3+1] = (uint8_t)((v >> 8)  & 0xFF);
            d[i*3+2] = (uint8_t)((v >> 16) & 0xFF);
        }
        break;
    }
    case ASIOSTInt16LSB: {
        int16_t* d = (int16_t*)dst;
        for (long i = 0; i < n; ++i) {
            float v = src[i];
            v = v >  1.0f ?  1.0f : (v < -1.0f ? -1.0f : v);
            d[i] = (int16_t)(v * 32767.0f);
        }
        break;
    }
    default:                                         /* unknown type: emit silence */
        memset(dst, 0, (size_t)n * sizeof(float));
        break;
    }
}

ASIOTime* bufferSwitchTimeInfo(ASIOTime* timeInfo, long index, ASIOBool /*processNow*/) {
    AsioSink* s = g_sink;
    if (!s) return timeInfo;

    BwTimestamp ts;
    ts.sample_pos     = samples_u64(timeInfo->timeInfo.samplePosition);
    ts.system_time_ns = timestamp_ns(timeInfo->timeInfo.systemTime);

    s->render(s->user, s->bus, (uint32_t)s->buffer_size, &ts);   /* engine fills the bus */

    for (uint32_t c = 0; c < s->channels; ++c) {
        void*        dst = s->bufferInfos[c].buffers[index];
        const float* src = s->bus + (size_t)c * s->buffer_size;
        convert_out(dst, src, s->buffer_size, s->channelInfos[c].type);
    }
    if (s->post_output) ASIOOutputReady();
    return timeInfo;
}

void bufferSwitch(long index, ASIOBool processNow) {
    ASIOTime t; memset(&t, 0, sizeof t);
    if (ASIOGetSamplePosition(&t.timeInfo.samplePosition, &t.timeInfo.systemTime) == ASE_OK)
        t.timeInfo.flags = kSystemTimeValid | kSamplePositionValid;
    bufferSwitchTimeInfo(&t, index, processNow);
}

void sampleRateDidChange(ASIOSampleRate /*rate*/) {}

long asioMessage(long selector, long value, void* /*msg*/, double* /*opt*/) {
    switch (selector) {
    case kAsioSelectorSupported:
        return (value == kAsioResetRequest || value == kAsioEngineVersion ||
                value == kAsioResyncRequest || value == kAsioLatenciesChanged ||
                value == kAsioSupportsTimeInfo) ? 1L : 0L;
    case kAsioEngineVersion:     return 2L;
    case kAsioSupportsTimeInfo:  return 1L;
    case kAsioResetRequest:      /* TODO(M1+): signal engine to re-open */ return 1L;
    case kAsioResyncRequest:
    case kAsioLatenciesChanged:  return 1L;
    default:                     return 0L;
    }
}

void set_err(char* err, size_t cap, const char* msg) {
    if (err && cap) { strncpy(err, msg, cap - 1); err[cap - 1] = 0; }
}

/* vtable ------------------------------------------------------------------ */

int asio_start(BwSink* base) {
    AsioSink* s = (AsioSink*)base;
    if (s->running) return 0;
    if (ASIOStart() != ASE_OK) return 1;
    s->running = true;
    return 0;
}
void asio_stop(BwSink* base) {
    AsioSink* s = (AsioSink*)base;
    if (s->running) { ASIOStop(); s->running = false; }
}
void asio_close(BwSink* base) {
    AsioSink* s = (AsioSink*)base;
    asio_stop(base);
    ASIODisposeBuffers();
    ASIOExit();
    if (asioDrivers) asioDrivers->removeCurrentDriver();
    g_sink = nullptr;
    free(s->bus);
    free(s);
}
const char* asio_backend(BwSink* base) { return ((AsioSink*)base)->name; }

const BwSinkVtbl ASIO_VT = { asio_start, asio_stop, asio_close, asio_backend };

} /* namespace */

/* Pick a driver: BWAUDIO_ASIO_DRIVER env override, else the first registered name. */
static bool choose_driver(char* out, size_t cap) {
    const char* env = getenv("BWAUDIO_ASIO_DRIVER");
    if (env && *env) { strncpy(out, env, cap - 1); out[cap - 1] = 0; return true; }
    if (!asioDrivers) return false;
    char names[16][32];
    char* ptrs[16];
    for (int i = 0; i < 16; ++i) ptrs[i] = names[i];
    long n = asioDrivers->getDriverNames(ptrs, 16);
    if (n <= 0) return false;
    strncpy(out, names[0], cap - 1); out[cap - 1] = 0;
    return true;
}

extern "C" BwSink* bw_asio_sink_open(uint32_t sample_rate, uint32_t block_size,
                                     uint32_t channels, BwRenderFn render, void* user,
                                     char* err, size_t errcap) {
    if (g_sink)                 { set_err(err, errcap, "asio: a driver is already open"); return nullptr; }
    if (!render || channels == 0 || channels > 64) { set_err(err, errcap, "asio: bad arguments"); return nullptr; }

    char drv[64] = {0};
    if (!choose_driver(drv, sizeof drv) || !loadAsioDriver(drv)) {
        set_err(err, errcap, "asio: no driver could be loaded");
        return nullptr;
    }

    ASIODriverInfo di; memset(&di, 0, sizeof di);
    di.asioVersion = 2;
    di.sysRef      = GetDesktopWindow();
    if (ASIOInit(&di) != ASE_OK) {
        set_err(err, errcap, di.errorMessage[0] ? di.errorMessage : "asio: ASIOInit failed");
        asioDrivers->removeCurrentDriver();
        return nullptr;
    }

    long nin = 0, nout = 0;
    if (ASIOGetChannels(&nin, &nout) != ASE_OK || nout < (long)channels) {
        set_err(err, errcap, "asio: driver exposes fewer than the required output channels");
        ASIOExit(); asioDrivers->removeCurrentDriver();
        return nullptr;
    }

    long bmin = 0, bmax = 0, bpref = 0, bgran = 0;
    ASIOGetBufferSize(&bmin, &bmax, &bpref, &bgran);
    long bufsize = (long)block_size;
    if (bufsize < bmin || bufsize > bmax) bufsize = bpref;   /* fall back to preferred */

    AsioSink* s = (AsioSink*)calloc(1, sizeof *s);
    if (!s) { set_err(err, errcap, "asio: out of memory"); ASIOExit(); asioDrivers->removeCurrentDriver(); return nullptr; }
    s->base.vt      = &ASIO_VT;
    s->sample_rate  = sample_rate;
    s->channels     = channels;
    s->buffer_size  = bufsize;
    s->render       = render;
    s->user         = user;
    strncpy(s->name, "asio:", sizeof s->name - 1);
    strncat(s->name, drv, sizeof s->name - strlen(s->name) - 1);

    ASIOSetSampleRate((ASIOSampleRate)sample_rate);   /* best-effort; DVS is fixed-rate */

    for (uint32_t c = 0; c < channels; ++c) {
        s->bufferInfos[c].isInput    = ASIOFalse;
        s->bufferInfos[c].channelNum = (long)c;
        s->bufferInfos[c].buffers[0] = s->bufferInfos[c].buffers[1] = nullptr;
    }
    s->callbacks.bufferSwitch         = &bufferSwitch;
    s->callbacks.sampleRateDidChange  = &sampleRateDidChange;
    s->callbacks.asioMessage          = &asioMessage;
    s->callbacks.bufferSwitchTimeInfo = &bufferSwitchTimeInfo;

    g_sink = s;   /* callbacks may fire during/after ASIOCreateBuffers */
    if (ASIOCreateBuffers(s->bufferInfos, (long)channels, bufsize, &s->callbacks) != ASE_OK) {
        set_err(err, errcap, "asio: ASIOCreateBuffers failed");
        g_sink = nullptr; ASIOExit(); asioDrivers->removeCurrentDriver(); free(s);
        return nullptr;
    }

    for (uint32_t c = 0; c < channels; ++c) {
        s->channelInfos[c].channel = (long)c;
        s->channelInfos[c].isInput = ASIOFalse;
        ASIOGetChannelInfo(&s->channelInfos[c]);
    }

    s->bus = (float*)calloc((size_t)bufsize * channels, sizeof(float));
    if (!s->bus) {
        set_err(err, errcap, "asio: bus alloc failed");
        ASIODisposeBuffers(); g_sink = nullptr; ASIOExit(); asioDrivers->removeCurrentDriver(); free(s);
        return nullptr;
    }

    long a = 0, b = 0;
    s->post_output = (ASIOOutputReady() == ASE_OK);
    (void)a; (void)b;
    return &s->base;
}
