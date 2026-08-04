/*
 * asio_sink.cpp — ASIO host backend (production path: 26 channels -> Digiface -> Dante).
 *
 * Compiled ONLY when the Steinberg ASIO SDK is vendored (BWA_HAVE_ASIO; see
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
 * no 26-ch Dante endpoint). Treat it as M1 code pending on-hardware verification.
 */
extern "C" {
#include "sink.h"
#include "profile.h"
}

#include "asiosys.h"
#include "asio.h"
#include "asiodrivers.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <new>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* Provided by the SDK host sources (asiodrivers.cpp). */
extern AsioDrivers* asioDrivers;
extern bool loadAsioDriver(char* name);

namespace {

struct AsioSink {
    bwa_sink          base;
    uint32_t        sample_rate;
    uint32_t        channels;        /* requested (26) */
    long            buffer_size;     /* frames per block, chosen from ASIOGetBufferSize */
    long            output_latency;  /* driver-reported render->DAC frames (ASIOGetLatencies); 0 = unknown */
    uint64_t        qpc_freq;        /* QueryPerformanceFrequency, for the synthesized host stamp */
    bwa_render_fn      render;
    void*           user;
    ASIOBufferInfo  bufferInfos[64];
    ASIOChannelInfo channelInfos[64];
    ASIOCallbacks   callbacks;
    float*          bus;             /* planar channels * buffer_size; engine renders here */
    uint64_t        fallback_pos;    /* internal block counter for when the driver's sample position is invalid */
    bool            post_output;     /* driver supports ASIOOutputReady */
    bool            running;
    char            name[64];

    /* --- health (bwa_sink_health) ---
     * Written on the driver's callback thread, read from the control thread, so relaxed atomics:
     * these are counters nobody synchronizes ON, and a reader that sees one field a block stale is
     * reading a monotonic count either way. Relaxed also means no fences in the callback.
     *
     * `pos_measured` is the honesty flag: it turns true the first time the driver hands us a sample
     * position it flags VALID. A driver that never does leaves us comparing our own fallback counter
     * against itself, which can never show a gap — so reporting 0 dropouts there would be a lie of
     * omission, and health.measured stays false instead. */
    std::atomic<uint64_t> h_blocks{0};
    std::atomic<uint64_t> h_dropouts{0};
    std::atomic<uint64_t> h_dropped_frames{0};
    std::atomic<uint64_t> h_resyncs{0};
    std::atomic<uint64_t> h_late{0};
    std::atomic<uint64_t> h_render_ns_peak{0};
    std::atomic<bool>     pos_measured{false};
    uint64_t        predicted_pos;   /* where the NEXT valid callback should land; 0 = nothing to compare yet */
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

/* Sample types convert_out knows how to write. Channels with any other type are
 * rejected at open(), so the audio callback never hits convert_out's default case. */
inline bool type_supported(long type) {
    return type == ASIOSTFloat32LSB || type == ASIOSTInt32LSB ||
           type == ASIOSTInt24LSB   || type == ASIOSTInt16LSB;
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
    default:                                         /* unreachable: rejected at open() */
        break;
    }
}

ASIOTime* bufferSwitchTimeInfo(ASIOTime* timeInfo, long index, ASIOBool /*processNow*/) {
    AsioSink* s = g_sink;
    if (!s) return timeInfo;
    static bool named = false;
    if (!named) { BWA_THREAD_NAME("bw-audio (ASIO)"); named = true; }   /* the driver's callback thread */
    BWA_ZONE_BEGIN(zblk, "asio block");                                 /* one block; vs the period = the RT budget */

    /* Trust the driver's sample position ONLY when it flags it valid; otherwise the memset-0 (or a
     * stale value) would freeze the engine's dsp clock and break bwa_source_play_at scheduling. Fall
     * back to an internal block counter, kept in step with the device position while it IS valid. */
    bwa_timestamp ts;
    const bool pos_valid = (timeInfo->timeInfo.flags & kSamplePositionValid) != 0;
    if (pos_valid)
        ts.sample_pos = samples_u64(timeInfo->timeInfo.samplePosition);
    else
        ts.sample_pos = s->fallback_pos;

    /* THE XRUN. The device's own position says where the stream is; s->predicted_pos says where the
     * last callback expected this one to land. A forward gap between them is audio the device
     * clocked out while we were not there to render it — the definition of a dropout, and the one
     * fault an offline render cannot produce by construction.
     *
     * Only a driver-flagged position can answer this: the fallback counter is derived from our own
     * callbacks, so comparing it against itself is always continuous by construction. Hence
     * pos_measured, which gates health.measured. */
    if (pos_valid) {
        if (s->predicted_pos) {
            const uint64_t lost = sink_position_gap(s->predicted_pos, ts.sample_pos, (uint32_t)s->buffer_size);
            if (lost) {
                s->h_dropouts.fetch_add(1, std::memory_order_relaxed);
                s->h_dropped_frames.fetch_add(lost, std::memory_order_relaxed);
            }
        }
        s->pos_measured.store(true, std::memory_order_relaxed);
    }
    s->predicted_pos  = ts.sample_pos + (uint64_t)s->buffer_size;
    s->fallback_pos   = ts.sample_pos + (uint64_t)s->buffer_size;
    if (timeInfo->timeInfo.flags & kSystemTimeValid)
        ts.system_time_ns = timestamp_ns(timeInfo->timeInfo.systemTime);
    else {
        /* No driver stamp (FlexASIO omits systemTime on the TimeInfo path; the Digiface unknown until rig
         * day): synthesize one from QPC at callback entry so bwa_get_clock still has a live pair —
         * a hair noisier than a driver stamp (it includes callback-dispatch jitter, tens of µs)
         * but a true correspondence. QPC is a userspace counter read (no kernel transition) — the
         * same clock the null sink paces with; the split scale avoids overflow (see null_sink.c). */
        LARGE_INTEGER qc; QueryPerformanceCounter(&qc);
        const uint64_t t = (uint64_t)qc.QuadPart, f = s->qpc_freq;
        ts.system_time_ns = f ? (t / f) * 1000000000ull + (t % f) * 1000000000ull / f : 0;
    }

    /* The other half of "is the device being starved": whether WE are the ones starving it. The
     * budget is one block period; a render that overruns it hands the driver its buffer late, and
     * enough of those become the dropouts counted above. Two QPC reads, no kernel transition. */
    LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
    s->render(s->user, s->bus, (uint32_t)s->buffer_size, &ts);   /* engine fills the bus */
    LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
    if (s->qpc_freq) {
        const uint64_t ticks = (uint64_t)(t1.QuadPart - t0.QuadPart);
        const uint64_t ns    = (ticks / s->qpc_freq) * 1000000000ull
                             + (ticks % s->qpc_freq) * 1000000000ull / s->qpc_freq;
        const uint64_t budget_ns = (uint64_t)s->buffer_size * 1000000000ull / (uint64_t)s->sample_rate;
        if (ns > budget_ns) s->h_late.fetch_add(1, std::memory_order_relaxed);
        uint64_t peak = s->h_render_ns_peak.load(std::memory_order_relaxed);
        while (ns > peak && !s->h_render_ns_peak.compare_exchange_weak(peak, ns, std::memory_order_relaxed)) {}
    }
    s->h_blocks.fetch_add(1, std::memory_order_relaxed);

    BWA_ZONE_BEGIN(zcv, "convert_out");
    for (uint32_t c = 0; c < s->channels; ++c) {
        void*        dst = s->bufferInfos[c].buffers[index];
        const float* src = s->bus + (size_t)c * s->buffer_size;
        convert_out(dst, src, s->buffer_size, s->channelInfos[c].type);
    }
    BWA_ZONE_END(zcv);
    if (s->post_output) ASIOOutputReady();
    BWA_ZONE_END(zblk);
    BWA_FRAME_MARK();
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
    case kAsioResetRequest:
        /* KNOWN GAP (rig-day): we ack the request but don't yet perform the reset. A full handler must
         * signal the control thread to bwa_stop + bwa_start (the driver stops calling bufferSwitch until
         * we re-create buffers). Until that plumbing exists, a live buffer-size/rate change in the
         * driver control panel will stall output — change it before bwa_start, not during. */
        return 1L;
    case kAsioResyncRequest:
        /* The driver telling us ITSELF that the stream lost continuity (a dropped buffer, a clock
         * slip). Independent of the sample-position check in bufferSwitchTimeInfo — a driver may
         * report one, the other, or both — so it gets its own counter rather than being folded in. */
        if (g_sink) g_sink->h_resyncs.fetch_add(1, std::memory_order_relaxed);
        return 1L;
    case kAsioLatenciesChanged:  return 1L;
    default:                     return 0L;
    }
}

void set_err(char* err, size_t cap, const char* msg) {
    if (err && cap) { strncpy(err, msg, cap - 1); err[cap - 1] = 0; }
}

/* vtable ------------------------------------------------------------------ */

int asio_start(bwa_sink* base) {
    AsioSink* s = (AsioSink*)base;
    if (s->running) return 0;
    if (ASIOStart() != ASE_OK) return 1;
    s->running = true;
    return 0;
}
void asio_stop(bwa_sink* base) {
    AsioSink* s = (AsioSink*)base;
    if (s->running) { ASIOStop(); s->running = false; }
}
void asio_close(bwa_sink* base) {
    AsioSink* s = (AsioSink*)base;
    /* Teardown order matters: ASIOStop then ASIODisposeBuffers, which per the ASIO spec guarantees
     * the driver issues no further bufferSwitch. Only AFTER that do we clear g_sink and free s, so a
     * callback can never be in flight against freed memory (clearing g_sink first would have left a
     * window: a callback past its NULL check could still touch s while we freed it). */
    asio_stop(base);            /* ASIOStop: driver stops issuing callbacks */
    ASIODisposeBuffers();       /* no bufferSwitch after this returns */
    g_sink = nullptr;
    ASIOExit();
    if (asioDrivers) asioDrivers->removeCurrentDriver();
    free(s->bus);
    free(s);
}
const char* asio_backend(bwa_sink* base) { return ((AsioSink*)base)->name; }
uint32_t asio_block_size(bwa_sink* base) { return (uint32_t)((AsioSink*)base)->buffer_size; }
uint32_t asio_output_latency(bwa_sink* base) { return (uint32_t)((AsioSink*)base)->output_latency; }

void asio_health(bwa_sink* base, bwa_sink_health* out) {
    AsioSink* s = (AsioSink*)base;
    out->blocks         = s->h_blocks.load(std::memory_order_relaxed);
    out->dropouts       = s->h_dropouts.load(std::memory_order_relaxed);
    out->dropped_frames = s->h_dropped_frames.load(std::memory_order_relaxed);
    out->driver_resyncs = s->h_resyncs.load(std::memory_order_relaxed);
    out->late_blocks    = s->h_late.load(std::memory_order_relaxed);
    out->render_ns_peak = s->h_render_ns_peak.load(std::memory_order_relaxed);
    out->period_ns      = s->sample_rate
            ? (uint64_t)s->buffer_size * 1000000000ull / (uint64_t)s->sample_rate : 0;
    /* Not "no dropouts" — "we were in a position to see one". A driver that never flags a valid
     * sample position leaves the dropout count structurally 0, and saying `measured` there would
     * turn "cannot know" into a clean bill of health. The resync and late-block counts are real
     * either way, but the headline number is not. */
    out->measured       = s->pos_measured.load(std::memory_order_relaxed);
}

const bwa_sink_vtbl ASIO_VT = {   /* designated: stop/close share a signature, so a positional swap would be silent */
    .start = asio_start, .stop = asio_stop, .close = asio_close,
    .backend = asio_backend, .block_size = asio_block_size,
    .output_latency = asio_output_latency,
    .health = asio_health,
};

} /* namespace */

/* Registered-driver enumeration (bwa_get_asio_driver_count/_name forward here): pre-lifecycle and
 * engine-free, so a host can populate a device picker before bwa_create. A LOCAL AsioDrivers reads
 * the registry fresh on construction — so each call sees drivers installed since the last one, and
 * nothing touches the SDK's loaded-driver global (the calib/zylia capture shells use the same
 * pattern). Enumeration only READS the list: nothing is loaded, initialized, or opened, so it is
 * safe alongside a live sink. Control thread only, like all lifecycle calls. */
enum { ASIO_ENUM_MAX = 32 };
static long asio_driver_names(char names[ASIO_ENUM_MAX][32]) {
    AsioDrivers list;
    char* ptrs[ASIO_ENUM_MAX];
    for (int i = 0; i < ASIO_ENUM_MAX; ++i) ptrs[i] = names[i];
    return list.getDriverNames(ptrs, ASIO_ENUM_MAX);
}

/* The SDK's asiolist reads driver names from the registry with the ANSI (-A) APIs, so its bytes are
 * CP_ACP — but the bwa ABI speaks UTF-8 (both bindings marshal it that way). Convert at this seam:
 * names go out CP_ACP -> UTF-8, and an explicit bwa_desc.asio_driver converts back before the SDK's
 * byte-exact strcmp match. ASCII names (every mainstream ASIO driver) pass through unchanged; on a
 * conversion failure the raw bytes pass through, which is the pre-conversion behavior. */
static void acp_to_utf8(const char* in, char* out, int outcap) {
    wchar_t w[96];
    int wn = MultiByteToWideChar(CP_ACP, 0, in, -1, w, 96);
    if (wn <= 0 || WideCharToMultiByte(CP_UTF8, 0, w, -1, out, outcap, nullptr, nullptr) <= 0) {
        strncpy(out, in, (size_t)outcap - 1); out[outcap - 1] = 0;
    }
}
static void utf8_to_acp(const char* in, char* out, int outcap) {
    wchar_t w[96];
    int wn = MultiByteToWideChar(CP_UTF8, 0, in, -1, w, 96);
    if (wn <= 0 || WideCharToMultiByte(CP_ACP, 0, w, -1, out, outcap, nullptr, nullptr) <= 0) {
        strncpy(out, in, (size_t)outcap - 1); out[outcap - 1] = 0;
    }
}
extern "C" uint32_t sink_asio_driver_count(void) {
    char names[ASIO_ENUM_MAX][32];
    long n = asio_driver_names(names);
    return n < 0 ? 0u : (uint32_t)n;
}
extern "C" bool sink_asio_driver_name(uint32_t index, char* buf, uint32_t cap) {
    if (!buf || cap == 0) return false;
    buf[0] = 0;
    char names[ASIO_ENUM_MAX][32];
    long n = asio_driver_names(names);
    if ((long)index >= (n < 0 ? 0 : n)) return false;
    acp_to_utf8(names[index], buf, (int)cap);
    return true;
}

/* Load + init driver `name` and confirm it has >= `channels` outputs. On success the driver
 * stays loaded+inited; on failure it is fully cleaned up. (ASIO callbacks have no user ptr,
 * so only one driver is ever live.) */
static bool try_driver(const char* name, uint32_t channels) {
    if (!loadAsioDriver((char*)name)) return false;
    ASIODriverInfo di; memset(&di, 0, sizeof di);
    di.asioVersion = 2;
    di.sysRef      = GetDesktopWindow();
    if (ASIOInit(&di) != ASE_OK) { asioDrivers->removeCurrentDriver(); return false; }
    long nin = 0, nout = 0;
    if (ASIOGetChannels(&nin, &nout) != ASE_OK || nout < (long)channels) {
        ASIOExit(); asioDrivers->removeCurrentDriver();
        return false;
    }
    return true;
}

extern "C" bwa_sink* bwa_asio_sink_open(uint32_t sample_rate, uint32_t block_size,
                                     uint32_t channels, const char* driver,
                                     bwa_render_fn render, void* user,
                                     char* err, size_t errcap) {
    if (g_sink)                 { set_err(err, errcap, "asio: a driver is already open"); return nullptr; }
    if (!render || channels == 0 || channels > 64) { set_err(err, errcap, "asio: bad arguments"); return nullptr; }

    /* Auto-pick: bwa_desc.asio_driver if set, else the first registered driver that opens with
     * enough output channels (so binaural finds a 2-ch headphone driver — ASIO4ALL / FlexASIO /
     * the Steinberg built-in — and cave finds a >=26-ch one, without configuration). */
    char drv[64] = {0};       /* CP_ACP form, for the SDK's byte-exact registry match */
    char drv_u8[96] = {0};    /* UTF-8 form, for error messages and the sink name */
    bool opened = false;
    if (driver && *driver) {
        utf8_to_acp(driver, drv, sizeof drv);
        strncpy(drv_u8, driver, sizeof drv_u8 - 1);
        opened = try_driver(drv, channels);
    } else {
        /* the SDK's global is created lazily by loadAsioDriver — on the PROCESS'S FIRST open it
         * doesn't exist yet, and gating on it would silently skip the auto-pick. Create it here
         * (the same object loadAsioDriver would make). */
        if (!asioDrivers) asioDrivers = new AsioDrivers();
        char names[16][32];
        char* ptrs[16];
        for (int i = 0; i < 16; ++i) ptrs[i] = names[i];
        long ndrv = asioDrivers->getDriverNames(ptrs, 16);
        for (long i = 0; i < ndrv && !opened; ++i)
            if (try_driver(names[i], channels)) {
                strncpy(drv, names[i], sizeof drv - 1);
                acp_to_utf8(names[i], drv_u8, sizeof drv_u8);
                opened = true;
            }
    }
    if (!opened) {
        set_err(err, errcap, "asio: no driver opened with enough output channels");
        return nullptr;
    }

    /* The driver dictates the true block size; bwa_desc.block_size is only a hint, and
     * the nframes passed to the render callback (== bufsize, the bus is sized to it) is
     * authoritative. Clamp the hint into range and honor the driver's granularity so
     * ASIOCreateBuffers never gets an invalid size. */
    long bmin = 0, bmax = 0, bpref = 0, bgran = 0;
    ASIOGetBufferSize(&bmin, &bmax, &bpref, &bgran);
    long bufsize = (long)block_size;
    if (bufsize < bmin || bufsize > bmax) {
        bufsize = bpref;                                              /* out of range */
    } else if (bgran > 0) {
        bufsize -= (bufsize - bmin) % bgran;                         /* snap to bmin + k*bgran */
    } else if (bgran == -1 && (bufsize & (bufsize - 1)) != 0) {
        bufsize = bpref;                                             /* driver wants powers of two */
    }

    /* Confirm the driver can actually run at the requested rate before committing. the Digiface is
     * fixed-rate (set in Dante Controller), and the engine is already built at cfg.sample_rate, so a
     * mismatch must fail loudly with the device's actual rate rather than run the whole engine at
     * the wrong rate. Query first (ASIOCanSampleRate), set, then re-query — some drivers report OK
     * but ignore the set. (bwa_desc.asio_driver pins the driver if the auto-pick chose a wrong one.) */
    if (ASIOCanSampleRate((ASIOSampleRate)sample_rate) != ASE_OK) {
        ASIOSampleRate cur = 0; ASIOGetSampleRate(&cur);
        char m[176];
        snprintf(m, sizeof m, "asio: driver '%s' cannot run at %u Hz (device is at %.0f Hz; set Dante "
                 "to %u Hz or match cfg.sample_rate to the device)", drv_u8, sample_rate, (double)cur, sample_rate);
        set_err(err, errcap, m);
        ASIOExit(); asioDrivers->removeCurrentDriver(); return nullptr;
    }
    if (ASIOSetSampleRate((ASIOSampleRate)sample_rate) != ASE_OK) {
        set_err(err, errcap, "asio: ASIOSetSampleRate failed for a rate the driver reported it could do");
        ASIOExit(); asioDrivers->removeCurrentDriver(); return nullptr;
    }
    ASIOSampleRate got = 0;
    if (ASIOGetSampleRate(&got) == ASE_OK && (got < (double)sample_rate - 1.0 || got > (double)sample_rate + 1.0)) {
        char m[176];
        snprintf(m, sizeof m, "asio: driver '%s' stayed at %.0f Hz, not the requested %u Hz", drv_u8, (double)got, sample_rate);
        set_err(err, errcap, m);
        ASIOExit(); asioDrivers->removeCurrentDriver(); return nullptr;
    }

    AsioSink* s = (AsioSink*)calloc(1, sizeof *s);
    if (!s) { set_err(err, errcap, "asio: out of memory"); ASIOExit(); asioDrivers->removeCurrentDriver(); return nullptr; }
    new (s) AsioSink();             /* the health counters are std::atomic — calloc'd storage is not a
                                     * constructed object, so value-initialize in place (zeroes the rest
                                     * exactly as calloc already did) */
    s->base.vt      = &ASIO_VT;
    s->sample_rate  = sample_rate;
    s->channels     = channels;
    s->buffer_size  = bufsize;
    s->render       = render;
    s->user         = user;
    strncpy(s->name, "asio:", sizeof s->name - 1);
    strncat(s->name, drv_u8, sizeof s->name - strlen(s->name) - 1);

    for (uint32_t c = 0; c < channels; ++c) {
        s->bufferInfos[c].isInput    = ASIOFalse;
        s->bufferInfos[c].channelNum = (long)c;
        s->bufferInfos[c].buffers[0] = s->bufferInfos[c].buffers[1] = nullptr;
    }
    s->callbacks.bufferSwitch         = &bufferSwitch;
    s->callbacks.sampleRateDidChange  = &sampleRateDidChange;
    s->callbacks.asioMessage          = &asioMessage;
    s->callbacks.bufferSwitchTimeInfo = &bufferSwitchTimeInfo;

    /* Learn each output channel's sample type (needs only ASIOInit, not buffers) and
     * reject any we cannot convert, so the audio callback never meets an unhandled type
     * (which could write the wrong byte width into the driver buffer). */
    for (uint32_t c = 0; c < channels; ++c) {
        s->channelInfos[c].channel = (long)c;
        s->channelInfos[c].isInput = ASIOFalse;
        ASIOGetChannelInfo(&s->channelInfos[c]);
        if (!type_supported(s->channelInfos[c].type)) {
            set_err(err, errcap, "asio: driver uses an unsupported output sample type");
            ASIOExit(); asioDrivers->removeCurrentDriver(); free(s);
            return nullptr;
        }
    }

    /* Allocate the bus and publish g_sink BEFORE ASIOCreateBuffers: a driver may pre-roll
     * a bufferSwitch during CreateBuffers, and that callback reads g_sink->bus and the
     * channel sample types — both must already be valid, or it dereferences a null bus. */
    s->bus = (float*)calloc((size_t)bufsize * channels, sizeof(float));
    if (!s->bus) {
        set_err(err, errcap, "asio: bus alloc failed");
        ASIOExit(); asioDrivers->removeCurrentDriver(); free(s);
        return nullptr;
    }

    g_sink = s;
    if (ASIOCreateBuffers(s->bufferInfos, (long)channels, bufsize, &s->callbacks) != ASE_OK) {
        set_err(err, errcap, "asio: ASIOCreateBuffers failed");
        g_sink = nullptr; ASIOExit(); asioDrivers->removeCurrentDriver(); free(s->bus); free(s);
        return nullptr;
    }

    s->post_output = (ASIOOutputReady() == ASE_OK);

    /* Latencies are only final once the buffers exist (they depend on the negotiated size). The
     * driver's outputLatency is the render->DAC delay in frames — for the Digiface that includes its Dante
     * network buffering — surfaced as bwa_get_output_latency_frames for AV-latency alignment. */
    long ilat = 0, olat = 0;
    s->output_latency = (ASIOGetLatencies(&ilat, &olat) == ASE_OK && olat > 0) ? olat : 0;
    { LARGE_INTEGER qf; s->qpc_freq = QueryPerformanceFrequency(&qf) ? (uint64_t)qf.QuadPart : 0; }
    return &s->base;
}
