/*
 * coreaudio_sink.c — the CoreAudio backend (docs/backend-development.md, phase 4).
 *
 * WHY IT EXISTS. A Mac at the desk: the headphone profiles on whatever the system output is, from
 * a Unity or Godot editor, a Python session or Octave. The array is a bonus the design permits,
 * because the RME Digiface Dante has a macOS driver and the HAL shows a device's raw channels.
 *
 * THE HAL, NOT AUHAL. AudioDevice* with one IOProc: no converter and no channel map between the
 * bus and the device, so an array open sees every output channel, and the timestamps are the
 * device's own. The price is that nothing resamples. The device runs at the engine rate or the
 * open fails (rule 6), for a headphone open too, so the open SETS the device's nominal rate.
 *
 * THE SHAPE. The HAL owns the IO thread and its buffer size is a request it normally honors but
 * can change later, so everything goes through the fixed-quantum adapter. Each output stream is
 * one interleaved float32 buffer in the IOProc's AudioBufferList, and the bus channels map across
 * the streams in order. A stereo open takes the device's preferred stereo pair instead, which is
 * where Audio MIDI Setup puts the headphones on a multichannel interface.
 *
 * WHAT THE SINK DELIBERATELY DOES NOT DO:
 *   - It never reopens and never follows the default device. A device that dies, or whose rate
 *     another application changes, sets device_lost and a host-paced thread keeps the engine's
 *     clocks advancing (docs/backends.md rule 4).
 *   - It never touches the thread priority. The HAL's IO thread is a time-constraint thread in
 *     the device's workgroup already (rule 7).
 *   - It never takes hog mode. BWA_SINK_FLAG_EXCLUSIVE is a no-op here: unlike WASAPI, exclusive
 *     access buys no latency on a Mac, only the exclusion.
 *   - It never puts the device's rate back. Every other client resamples through its own
 *     converter, and flipping the rate twice per editor play session is two glitches for them
 *     instead of one.
 *
 * LISTENER LIFETIME. AudioObjectRemovePropertyListenerBlock does not say whether it waits for an
 * invocation already under way, so no listener block holds a pointer to the sink. Each captures a
 * SLOT INDEX into a static table. The device-change handler runs on one serial queue and looks
 * the sink up in its slot; close() clears the slot ON THAT QUEUE, after which a late notification
 * finds nothing. The overload listener is called directly on the IO thread (the header says so),
 * and it only increments an atomic that lives in the table and is never freed.
 *
 * AUDIO THREAD. The IOProc is the HAL's IO thread; the audio-thread invariants apply to every
 * line of it. Everything it needs is allocated at open.
 */
#include "sink/sink.h"
#include "sink/sink_quant.h"
#include "os/os.h"
#include "core/profile.h"

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Block.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>

#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef __BLOCKS__
#error "coreaudio_sink.c needs blocks (-fblocks): the HAL's queue-bound listeners take one"
#endif

/* kAudioObjectPropertyElementMain is macOS 12 and its older spelling is deprecated there, so
 * either name warns somewhere between the 10.13 deployment target and the current SDK. Both are 0. */
#define CA_ELEMENT_MAIN ((AudioObjectPropertyElement)0)

/* The largest IO cycle the FIFO is sized for. The device's buffer-size RANGE is the real ceiling
 * and is read at open; this caps what an absurd range could make the sink allocate, the role
 * JACK_MAX_PERIOD plays in jack_sink.c. */
#define CA_MAX_PERIOD 8192

/* CoreAudio sinks open at once in one process. An engine holds at most two (cave_both). */
#define CA_MAX_SINKS 8

/* Output streams the channel map can address. A device with more keeps silence on the rest. */
#define CA_MAX_STREAMS BWA_CHANNELS

typedef struct CASink {
    bwa_sink base;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t block;              /* the engine quantum; block_size() reports this               */
    uint32_t q_capacity;         /* frames the adapter can serve in one pull                    */
    uint32_t latency_base;       /* rule 5 without the IO buffer: device + safety + stream      */
    uint32_t prior_rate;         /* the device's nominal rate before this open changed it       */
    uint32_t slot;               /* this sink's entry in g_slot                                 */

    AudioObjectID       dev;
    AudioDeviceIOProcID proc;
    bool                started;        /* control thread: AudioDeviceStart succeeded          */
    mach_timebase_info_data_t tb;       /* mHostTime ticks -> ns, latched at open              */

    /* Bus channel c lands in AudioBufferList buffer map_buf[c], interleave slot map_ch[c]. */
    uint16_t map_buf[BWA_CHANNELS];
    uint16_t map_ch[BWA_CHANNELS];

    SinkQuant quant;

    /* --- IOProc state, audio thread only --- */
    bool     anchored;           /* `anchor` holds a value                                      */
    int64_t  anchor;             /* device sample time minus the adapter's `written`            */

    /* `running` and `lost` fence the pullers; `busy` counts pulls in flight. A puller raises
     * busy and THEN reads the fences; a stopper sets a fence and THEN waits for busy to reach 0.
     * Both sides are seq_cst, which is what makes that pair of store-then-load sequences safe:
     * either the puller sees the fence, or the stopper sees the pull. */
    /* The IO cycle the device is actually running, in frames: the read-back at open, then
     * whatever the IOProc is handed. It is not the sink's to keep: the buffer size belongs to the
     * device and the PROCESS, so another open, or a host engine, can change it under this one. */
    _Atomic uint32_t cycle_frames;

    _Atomic int      running;    /* start() opens the gate, stop() closes it                    */
    _Atomic uint32_t lost;       /* the device died or left the engine rate                     */
    _Atomic int      busy;

    /* Host-paced fallback after a loss (rule 4). Created on the listener queue, at most once.
     * host_started is queue-confined until close() has drained the queue. */
    os_thread   host_thread;
    bool        host_started;
    _Atomic int host_stop;

    AudioObjectPropertyListenerBlock on_change;     /* IsAlive + NominalSampleRate, on g_queue  */
    AudioObjectPropertyListenerBlock on_overload;   /* ProcessorOverload, direct on the IO thread */

    char name[288];              /* "coreaudio:<device name>", UTF-8 from the OS */
} CASink;

/* See LISTENER LIFETIME in the file header. `sink` is read and written on g_queue only. A slot
 * is never freed, so a listener that fires after its sink closed touches valid memory: at worst
 * one stray overload count on a sink that reused the slot. */
typedef struct {
    _Atomic int      claimed;
    _Atomic uint64_t overloads;
    CASink*          sink;
} CASlot;
static CASlot           g_slot[CA_MAX_SINKS];
static dispatch_queue_t g_queue;
static dispatch_once_t  g_queue_once;

static void ca_make_queue(void* unused) {
    (void)unused;
    g_queue = dispatch_queue_create("bw_audio.coreaudio", DISPATCH_QUEUE_SERIAL);
}

/* The device-side test tap (sink.h). NULL outside the sink test. */
void (*sink_coreaudio_tap)(uint32_t first_channel, const float* interleaved,
                           uint32_t stream_channels, uint32_t nframes) = NULL;

static void ca_set_err(char* err, size_t cap, const char* msg) {
    if (err && cap) { strncpy(err, msg, cap - 1); err[cap - 1] = 0; }
}

/* ---- property helpers (control thread and the listener queue; never the IOProc) -------------- */

static OSStatus ca_get(AudioObjectID obj, AudioObjectPropertySelector sel,
                       AudioObjectPropertyScope scope, void* data, UInt32* size) {
    const AudioObjectPropertyAddress a = { sel, scope, CA_ELEMENT_MAIN };
    return AudioObjectGetPropertyData(obj, &a, 0, NULL, size, data);
}

static uint32_t ca_get_u32(AudioObjectID obj, AudioObjectPropertySelector sel,
                           AudioObjectPropertyScope scope, uint32_t fallback) {
    UInt32 v = 0, sz = sizeof v;
    return ca_get(obj, sel, scope, &v, &sz) == noErr ? (uint32_t)v : fallback;
}

/* A CFString property as UTF-8, under bwa_get_device_name's contract: truncated on a character
 * boundary, never failed for want of room. CFStringGetCString fails outright on a short buffer,
 * so it converts into one that is certainly large enough and sink_copy_device_name does the cut. */
static bool ca_string(AudioObjectID obj, AudioObjectPropertySelector sel, char* buf, uint32_t cap) {
    if (!buf || cap == 0) return false;
    buf[0] = 0;
    CFStringRef str = NULL;
    UInt32 sz = sizeof str;
    if (ca_get(obj, sel, kAudioObjectPropertyScopeGlobal, &str, &sz) != noErr || !str) return false;
    const CFIndex max = CFStringGetMaximumSizeForEncoding(CFStringGetLength(str),
                                                          kCFStringEncodingUTF8) + 1;
    char* tmp = (char*)malloc((size_t)max);
    bool ok = tmp && CFStringGetCString(str, tmp, max, kCFStringEncodingUTF8);
    if (ok) ok = sink_copy_device_name(buf, cap, tmp);
    free(tmp);
    CFRelease(str);
    return ok;
}

/* The output side's shape: one entry per stream, its channel count. Returns the stream count
 * (capped at `cap`) and the device's total output channels. This is the AudioBufferList the
 * IOProc will be handed, so it is the authority for the channel map. */
static uint32_t ca_output_layout(AudioObjectID dev, uint32_t* per_stream, uint32_t cap,
                                 uint32_t* total) {
    *total = 0;
    const AudioObjectPropertyAddress a = { kAudioDevicePropertyStreamConfiguration,
                                           kAudioObjectPropertyScopeOutput, CA_ELEMENT_MAIN };
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &sz) != noErr || sz < sizeof(UInt32)) return 0;
    AudioBufferList* abl = (AudioBufferList*)malloc(sz);
    if (!abl) return 0;
    uint32_t n = 0;
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, abl) == noErr) {
        for (UInt32 b = 0; b < abl->mNumberBuffers; ++b) {
            *total += abl->mBuffers[b].mNumberChannels;
            if (n < cap && per_stream) per_stream[n++] = abl->mBuffers[b].mNumberChannels;
        }
    }
    free(abl);
    return n;
}

static AudioObjectID ca_default_output(void) {
    AudioObjectID dev = kAudioObjectUnknown;
    UInt32 sz = sizeof dev;
    if (ca_get(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice,
               kAudioObjectPropertyScopeGlobal, &dev, &sz) != noErr) return kAudioObjectUnknown;
    return dev;
}

/* Every device with at least one output channel, the default output first (rule: index 0 is the
 * platform default). The caller frees *out. Re-read on every query: the list is a dozen entries
 * and a picker must see a device that was plugged in since the last call. */
static uint32_t ca_list_outputs(AudioObjectID** out) {
    *out = NULL;
    const AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices,
                                           kAudioObjectPropertyScopeGlobal, CA_ELEMENT_MAIN };
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &sz) != noErr || sz == 0)
        return 0;
    AudioObjectID* all = (AudioObjectID*)malloc(sz);
    AudioObjectID* res = (AudioObjectID*)malloc(sz);
    uint32_t n = 0;
    if (all && res && AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, all) == noErr) {
        const uint32_t count = (uint32_t)(sz / sizeof(AudioObjectID));
        const AudioObjectID def = ca_default_output();
        for (uint32_t pass = 0; pass < 2; ++pass) {          /* pass 0: the default; pass 1: the rest */
            for (uint32_t i = 0; i < count; ++i) {
                if ((all[i] == def) != (pass == 0)) continue;
                uint32_t total = 0;
                ca_output_layout(all[i], NULL, 0, &total);
                if (total > 0) res[n++] = all[i];
            }
        }
    }
    free(all);
    if (n == 0) { free(res); res = NULL; }
    *out = res;
    return n;
}

static uint32_t ca_nominal_rate(AudioObjectID dev) {
    Float64 r = 0.0;
    UInt32 sz = sizeof r;
    if (ca_get(dev, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &r, &sz) != noErr
        || r <= 0.0) return 0;
    return (uint32_t)(r + 0.5);
}

/* Is `rate` inside one of the device's nominal-rate ranges? An unreadable list answers yes, so
 * the set itself decides. */
static bool ca_rate_available(AudioObjectID dev, uint32_t rate) {
    const AudioObjectPropertyAddress a = { kAudioDevicePropertyAvailableNominalSampleRates,
                                           kAudioObjectPropertyScopeGlobal, CA_ELEMENT_MAIN };
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &sz) != noErr || sz < sizeof(AudioValueRange))
        return true;
    AudioValueRange* r = (AudioValueRange*)malloc(sz);
    if (!r) return true;
    bool ok = true;
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, r) == noErr) {
        ok = false;
        for (UInt32 i = 0; i < sz / sizeof *r; ++i)
            if ((double)rate >= r[i].mMinimum - 0.5 && (double)rate <= r[i].mMaximum + 0.5) { ok = true; break; }
    }
    free(r);
    return ok;
}

/* ---- the IOProc ------------------------------------------------------------------------------ */

static inline uint64_t ca_ticks_to_ns(const CASink* s, uint64_t t) {
    /* Split the scale the way os_monotonic_ns does: ticks * numer overflows on a long uptime. */
    return (t / s->tb.denom) * s->tb.numer + (t % s->tb.denom) * s->tb.numer / s->tb.denom;
}

typedef struct { AudioBufferList* abl; const CASink* s; } CAOut;

/* sink_quant hands one contiguous run of its FIFO at a time. The streams are float32, so this is
 * an interleave across however many buffers the device presents. Channel-major: one contiguous
 * bus row in, one strided column out. Every index is checked against the list the HAL handed
 * THIS callback, so a stream configuration that changed after open cannot write out of bounds. */
static void ca_out(void* user, const float* planar, uint32_t stride, uint32_t frame_offset,
                   uint32_t n) {
    const CAOut* o = (const CAOut*)user;
    const CASink* s = o->s;
    for (uint32_t c = 0; c < s->channels; ++c) {
        const uint32_t b = s->map_buf[c];
        if (b >= o->abl->mNumberBuffers) continue;
        const AudioBuffer* ab = &o->abl->mBuffers[b];
        const uint32_t nch = ab->mNumberChannels, k = s->map_ch[c];
        if (!ab->mData || k >= nch) continue;
        const float* src = planar + (size_t)c * stride;
        float* dst = (float*)ab->mData + (size_t)frame_offset * nch + k;
        for (uint32_t i = 0; i < n; ++i) dst[(size_t)i * nch] = src[i];
    }
}

static void ca_discard_out(void* u, const float* p, uint32_t s, uint32_t o, uint32_t n) {
    (void)u; (void)p; (void)s; (void)o; (void)n;
}

static OSStatus ca_ioproc(AudioObjectID dev, const AudioTimeStamp* now,
                          const AudioBufferList* in, const AudioTimeStamp* in_time,
                          AudioBufferList* out, const AudioTimeStamp* out_time, void* user) {
    (void)dev; (void)in; (void)in_time;
    CASink* s = (CASink*)user;

    /* Every buffer, every callback, before anything can return: what the sink does not write the
     * HAL plays as it found it. The map below then overwrites the channels the bus carries. */
    uint32_t n = UINT32_MAX;
    for (UInt32 b = 0; b < out->mNumberBuffers; ++b) {
        AudioBuffer* ab = &out->mBuffers[b];
        if (!ab->mData || ab->mNumberChannels == 0) continue;
        memset(ab->mData, 0, ab->mDataByteSize);
        const uint32_t f = (uint32_t)(ab->mDataByteSize / (ab->mNumberChannels * sizeof(float)));
        if (f < n) n = f;
    }
    if (n == UINT32_MAX || n == 0) return noErr;
    if (n != atomic_load_explicit(&s->cycle_frames, memory_order_relaxed))
        atomic_store_explicit(&s->cycle_frames, n, memory_order_relaxed);   /* rule 5 follows it */

    atomic_fetch_add(&s->busy, 1);
    if (!atomic_load(&s->running) || atomic_load(&s->lost)) {
        atomic_fetch_sub(&s->busy, 1);
        return noErr;
    }

    /* Rule 3's host half is `now`, the IO cycle's own time, and NOT out_time. The output time
     * leads it by the buffer plus the safety offset (measured: 12.00 ms at 256 + 320 frames),
     * and output_latency() already counts both, so stamping the output time would count them
     * twice. `now` also means what every other backend's stamp means: the moment of the callback. */
    const uint64_t host_ns = (now->mFlags & kAudioTimeStampHostTimeValid)
        ? ca_ticks_to_ns(s, now->mHostTime) : sink_quant_now_ns();

    /* Rule 4's position: the output sample time, moved onto the adapter's stream. It advances by
     * exactly one buffer per cycle, and a cycle the HAL skipped after an overload shows as a jump,
     * which is the adapter's queued-depth rule. A sample time that steps backward or absurdly far
     * is a new timeline (the device restarted), so it re-anchors instead of reporting a gap. */
    uint64_t pos = 0;
    bool pos_valid = false;
    if (out_time->mFlags & kAudioTimeStampSampleTimeValid) {
        const int64_t st      = (int64_t)(out_time->mSampleTime + 0.5);
        const int64_t written = (int64_t)s->quant.written;
        const int64_t sane    = (int64_t)s->block * 4096;        /* sink_position_gap's window */
        int64_t rel = st - s->anchor;
        if (!s->anchored || rel < written || rel - written > sane) {
            s->anchor   = st - written;
            s->anchored = true;
            rel = written;
        }
        pos = (uint64_t)rel;
        pos_valid = true;
    }

    if (n > s->q_capacity) {
        /* A cycle past the range the FIFO was sized for. The buffers are already silent past
         * what gets served; book the shortfall, and move the anchor so the position rule does
         * not report the same frames again on the next cycle. */
        const uint32_t over = n - s->q_capacity;
        sink_quant_note_dropout(&s->quant, over);
        s->anchor += over;
        n = s->q_capacity;
    }

    CAOut o = { out, s };
    BWA_ZONE_BEGIN(zb, "coreaudio cycle");
    sink_quant_pull(&s->quant, n, pos, pos_valid, host_ns, ca_out, &o);
    BWA_ZONE_END(zb);

    if (sink_coreaudio_tap) {                   /* test only: what the HAL is about to play */
        uint32_t first = 0;
        for (UInt32 b = 0; b < out->mNumberBuffers; ++b) {
            const AudioBuffer* ab = &out->mBuffers[b];
            if (ab->mData && ab->mNumberChannels)
                sink_coreaudio_tap(first, (const float*)ab->mData, ab->mNumberChannels, n);
            first += ab->mNumberChannels;
        }
    }

    atomic_fetch_sub(&s->busy, 1);
    return noErr;
}

/* ---- the host-paced fallback (rule 4) -------------------------------------------------------- */

static void ca_host_thread(void* arg) {
    CASink* s = (CASink*)arg;
    BWA_THREAD_NAME("bw-audio (CoreAudio lost)");
    const uint64_t block_ns = (uint64_t)s->block * 1000000000ull / (uint64_t)s->sample_rate;
    /* On Darwin this is about timers, not priority: without a time-constraint policy the kernel
     * coalesces the deadline sleep below and the dsp clock runs milliseconds late. */
    const bool rt = (os_thread_set_realtime(block_ns) == 0);
    uint64_t next_ns = 0;
    while (!atomic_load_explicit(&s->host_stop, memory_order_relaxed)) {
        const uint64_t now = sink_quant_now_ns();
        if (next_ns == 0 || now > next_ns + block_ns * 8ull) next_ns = now;
        atomic_fetch_add(&s->busy, 1);
        if (atomic_load(&s->running))
            sink_quant_pull(&s->quant, s->block, 0, false, now, ca_discard_out, NULL);
        atomic_fetch_sub(&s->busy, 1);
        next_ns += block_ns;
        os_sleep_until_ns(next_ns);          /* absolute deadline: a relative sleep drifts */
    }
    if (rt) os_thread_clear_realtime();
}

/* Wait until no pull is in flight. Call it AFTER setting a fence (see the struct comment). False
 * means a render never returned inside the bound, which is a hung engine, not a slow one. */
static bool ca_wait_idle(CASink* s) {
    for (int i = 0; i < 2000; ++i) {
        if (atomic_load(&s->busy) == 0) return true;
        os_sleep_ms(1);
    }
    return false;
}

/* A device property changed. Runs on g_queue. It trusts the DEVICE, not the notification, so a
 * stray call (a late one for a sink that closed, landing on the sink that reused its slot) reads
 * a healthy device and does nothing.
 *
 * Two ways to lose a device: it died, or it no longer runs at the engine rate (another
 * application, or Audio MIDI Setup, changed it). The second is a loss too. Nothing here
 * resamples, so playing on would be a pitched render that looks healthy. */
static void ca_device_changed(uint32_t slot) {
    CASink* s = g_slot[slot].sink;
    if (!s || atomic_load(&s->lost)) return;

    UInt32 alive = 0, sz = sizeof alive;
    bool gone = ca_get(s->dev, kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal,
                       &alive, &sz) != noErr || !alive;
    if (!gone) {
        const uint32_t rate = ca_nominal_rate(s->dev);
        gone = (rate != 0 && rate != s->sample_rate);
    }
    if (!gone) return;

    atomic_store(&s->lost, 1u);                 /* fence the IOProc ...                           */
    AudioDeviceStop(s->dev, s->proc);           /* ... stop it; fails harmlessly on a dead device */
    if (!ca_wait_idle(s)) return;               /* ... and never start a second puller beside it  */
    if (os_thread_create(&s->host_thread, ca_host_thread, s) == 0) s->host_started = true;
}

typedef struct { uint32_t slot; CASink* sink; } CABind;
static void ca_bind(void* ctx) {
    const CABind* b = (const CABind*)ctx;
    g_slot[b->slot].sink = b->sink;
}

/* ---- vtable ---------------------------------------------------------------------------------- */

static int ca_start(bwa_sink* base) {
    CASink* s = (CASink*)base;
    atomic_store(&s->running, 1);
    /* Lost before the start: the host-paced thread is already there and the gate above is all it
     * was waiting for. The engine runs, silent, with device_lost saying why. */
    if (atomic_load(&s->lost)) return 0;
    /* A restart may or may not be a new sample timeline: the device keeps its old one when
     * another IOProc in this process kept it running. Either way the frames it played while this
     * sink was stopped are not a dropout, so the first cycle re-anchors. Safe to write here: no
     * IOProc of ours runs between stop() and AudioDeviceStart. */
    s->anchored = false;
    if (AudioDeviceStart(s->dev, s->proc) != noErr) {
        atomic_store(&s->running, 0);
        return 1;
    }
    s->started = true;
    return 0;
}

/* Rule 8's first half. AudioDeviceStop returned with no callback in flight and none after it on
 * every run measured (60 stops, some with the IOProc overrunning), but the header does not
 * promise it. The busy count covers the half that can be covered: a pull still in flight. A
 * callback that BEGAN after this returned would not be seen, and nothing here can see it. */
static void ca_stop(bwa_sink* base) {
    CASink* s = (CASink*)base;
    atomic_store(&s->running, 0);
    if (s->started) { AudioDeviceStop(s->dev, s->proc); s->started = false; }
    ca_wait_idle(s);
}

static void ca_release(CASink* s) {
    if (s->proc) AudioDeviceDestroyIOProcID(s->dev, s->proc);
    if (s->on_change)   Block_release(s->on_change);
    if (s->on_overload) Block_release(s->on_overload);
    sink_quant_free(&s->quant);
    atomic_store(&g_slot[s->slot].claimed, 0);
    free(s);
}

static void ca_close(bwa_sink* base) {
    CASink* s = (CASink*)base;
    ca_stop(base);

    const AudioObjectPropertyAddress alive = { kAudioDevicePropertyDeviceIsAlive,
                                               kAudioObjectPropertyScopeGlobal, CA_ELEMENT_MAIN };
    const AudioObjectPropertyAddress rate  = { kAudioDevicePropertyNominalSampleRate,
                                               kAudioObjectPropertyScopeGlobal, CA_ELEMENT_MAIN };
    const AudioObjectPropertyAddress over  = { kAudioDeviceProcessorOverload,
                                               kAudioObjectPropertyScopeGlobal, CA_ELEMENT_MAIN };
    AudioObjectRemovePropertyListenerBlock(s->dev, &alive, g_queue, s->on_change);
    AudioObjectRemovePropertyListenerBlock(s->dev, &rate,  g_queue, s->on_change);
    AudioObjectRemovePropertyListenerBlock(s->dev, &over,  NULL,    s->on_overload);

    /* The barrier. Once this returns, every handler that had started has finished and none that
     * starts later can find the sink, so host_started below is final and `s` is ours to free. */
    CABind unbind = { s->slot, NULL };
    dispatch_sync_f(g_queue, &unbind, ca_bind);

    if (s->host_started) {
        atomic_store_explicit(&s->host_stop, 1, memory_order_relaxed);
        os_thread_join(&s->host_thread);
    }
    ca_release(s);
}

static const char* ca_backend(bwa_sink* base)     { return ((CASink*)base)->name; }
static uint32_t ca_block_size(bwa_sink* base)     { return ((CASink*)base)->block; }
/* Rule 5: everything between the stamp and the DAC. The stamp is the cycle's own time (see the
 * IOProc), so that is this cycle's buffer, the safety offset the HAL keeps ahead of the hardware,
 * and the device's and the stream's own latency.
 *
 * NO TERM FOR THE ADAPTER, measured: at every block and buffer pairing tried (pass-through,
 * 8192 on 4096, 6000 on 4096, 256 on 1024, 1024 on 256, 8 on 15) a block's first frame reached
 * the device within 5 frames of its own stamp. sink_quant stamps a block with the callback time
 * plus the frames queued ahead of it, so what the adapter holds is already in the stamp, and
 * adding a block here would count it twice.
 *
 * And it FOLLOWS the IO cycle. The header's "constant while the device is open" holds
 * for as long as nothing else in the process resizes the device's buffer. When something does, a
 * constant would be wrong by the difference, which is worse than a number that moved. */
static uint32_t ca_output_latency(bwa_sink* base) {
    CASink* s = (CASink*)base;
    return s->latency_base + atomic_load_explicit(&s->cycle_frames, memory_order_relaxed);
}

/* `measured` is the adapter's: true once an output sample time arrived, which is what makes a
 * zero dropout count mean "none happened". driver_resyncs is the HAL's own overload report. A
 * stalled render shows in both, by design: the overload is the device saying a cycle ran late,
 * the dropout is how many frames that cost. */
static void ca_health(bwa_sink* base, bwa_sink_health* out) {
    CASink* s = (CASink*)base;
    sink_quant_health(&s->quant, out);
    out->driver_resyncs = atomic_load_explicit(&g_slot[s->slot].overloads, memory_order_relaxed);
    out->device_lost    = atomic_load_explicit(&s->lost, memory_order_relaxed);
}

/* Internal readbacks for the sink test (declared in sink.h, deliberately not in bw_audio.h): the
 * IO cycle the device is running now, and the nominal rate it ran at before this open, which a
 * test needs to put the machine back the way it found it. */
uint32_t sink_coreaudio_buffer_frames(bwa_sink* base) {
    return atomic_load_explicit(&((CASink*)base)->cycle_frames, memory_order_relaxed);
}
uint32_t sink_coreaudio_prior_rate(bwa_sink* base)    { return ((CASink*)base)->prior_rate; }

static const bwa_sink_vtbl COREAUDIO_VT = {   /* designated: stop/close share a signature, so a positional swap would be silent */
    .type = BWA_SINK_COREAUDIO,
    .start = ca_start, .stop = ca_stop, .close = ca_close,
    .backend = ca_backend, .block_size = ca_block_size,
    .output_latency = ca_output_latency,
    .health = ca_health,
};

/* ---- device enumeration ---------------------------------------------------------------------- */

uint32_t sink_coreaudio_device_count(void) {
    AudioObjectID* ids = NULL;
    const uint32_t n = ca_list_outputs(&ids);
    free(ids);
    return n;
}

static bool ca_device_string(uint32_t index, AudioObjectPropertySelector sel, char* buf, uint32_t cap) {
    if (!buf || cap == 0) return false;
    buf[0] = 0;
    AudioObjectID* ids = NULL;
    const uint32_t n = ca_list_outputs(&ids);
    const bool ok = (index < n) && ca_string(ids[index], sel, buf, cap);
    free(ids);
    return ok;
}

bool sink_coreaudio_device_name(uint32_t index, char* buf, uint32_t cap) {
    return ca_device_string(index, kAudioObjectPropertyName, buf, cap);
}

/* The UID survives a reboot and a replug, and tells two identical displays apart. */
bool sink_coreaudio_device_id(uint32_t index, char* buf, uint32_t cap) {
    return ca_device_string(index, kAudioDevicePropertyDeviceUID, buf, cap);
}

/* ---- open ------------------------------------------------------------------------------------ */

/* Rule 10: the exact name first, across every device, then the exact UID. NULL is the default
 * output. Returns kAudioObjectUnknown with the reason in `err`. */
static AudioObjectID ca_resolve(const char* device, char* err, size_t errcap) {
    if (!device || !*device) {
        const AudioObjectID def = ca_default_output();
        /* The phrase the sink test's skip rule keys on: a CI runner, or a Mac with every output
         * disabled. The only open failure that is a property of the machine. */
        if (def == kAudioObjectUnknown)
            ca_set_err(err, errcap, "coreaudio: there is no default output device");
        return def;
    }
    AudioObjectID* ids = NULL;
    const uint32_t n = ca_list_outputs(&ids);
    AudioObjectID found = kAudioObjectUnknown;
    char buf[256];
    for (uint32_t pass = 0; pass < 2 && found == kAudioObjectUnknown; ++pass) {
        const AudioObjectPropertySelector sel = pass == 0 ? kAudioObjectPropertyName
                                                          : kAudioDevicePropertyDeviceUID;
        for (uint32_t i = 0; i < n; ++i)
            if (ca_string(ids[i], sel, buf, sizeof buf) && strcmp(buf, device) == 0) { found = ids[i]; break; }
    }
    free(ids);
    if (found == kAudioObjectUnknown) {
        char m[240];
        snprintf(m, sizeof m, "coreaudio: no output device named '%.120s' (checked the exact name "
                              "and the UID)", device);
        ca_set_err(err, errcap, m);
    }
    return found;
}

/* Put the device on the engine rate, or say why it cannot be. The set is asynchronous, so poll
 * the read-back with a bound. Returns false with a message that names both rates (rule 6). */
static bool ca_set_rate(AudioObjectID dev, const char* devname, uint32_t rate, uint32_t current,
                        char* err, size_t errcap) {
    bool ok = ca_rate_available(dev, rate);
    if (ok) {
        const AudioObjectPropertyAddress a = { kAudioDevicePropertyNominalSampleRate,
                                               kAudioObjectPropertyScopeGlobal, CA_ELEMENT_MAIN };
        const Float64 want = (Float64)rate;
        ok = AudioObjectSetPropertyData(dev, &a, 0, NULL, sizeof want, &want) == noErr;
        if (ok) {
            ok = false;
            for (int i = 0; i < 200 && !ok; ++i) {           /* 2 s; a USB interface takes ~100 ms */
                ok = (ca_nominal_rate(dev) == rate);
                if (!ok) os_sleep_ms(10);
            }
        }
    }
    if (!ok) {
        const uint32_t now_rate = ca_nominal_rate(dev);
        char m[250];
        snprintf(m, sizeof m, "coreaudio: '%.60s' runs at %u Hz and cannot run at the engine's %u Hz, "
                              "and nothing in this path resamples; create the engine at %u Hz",
                 devname, now_rate ? now_rate : current, rate, now_rate ? now_rate : current);
        ca_set_err(err, errcap, m);
    }
    return ok;
}

bwa_sink* bwa_coreaudio_sink_open(uint32_t sample_rate, uint32_t block_size, uint32_t channels,
                                  const char* device, uint32_t flags, bool exact_rate,
                                  bwa_render_fn render, void* user, char* err, size_t errcap) {
    /* No flag changes anything here, and the rate is exact whether or not it was asked for: the
     * HAL has no per-client resampler. See the file header. */
    (void)flags; (void)exact_rate;
    if (!render || channels == 0 || channels > BWA_CHANNELS || block_size == 0 || sample_rate == 0) {
        ca_set_err(err, errcap, "coreaudio: bad arguments");
        return NULL;
    }

    const AudioObjectID dev = ca_resolve(device, err, errcap);
    if (dev == kAudioObjectUnknown) return NULL;

    char devname[256];
    if (!ca_string(dev, kAudioObjectPropertyName, devname, sizeof devname))
        snprintf(devname, sizeof devname, "device %u", (unsigned)dev);

    /* Width, then rate, then width again. A device's channel count can depend on its rate (an
     * interface that halves its outputs at 96 kHz), so the layout that counts is the one AT the
     * engine rate. But a device that is too narrow and already at the engine rate is refused
     * before anything changes, which is the desk Mac asked for an array. */
    uint32_t per_stream[CA_MAX_STREAMS];
    uint32_t total = 0;
    uint32_t nstreams = ca_output_layout(dev, per_stream, CA_MAX_STREAMS, &total);
    const uint32_t prior_rate = ca_nominal_rate(dev);
    const bool move_rate = (prior_rate != sample_rate);
    if (move_rate) {
        /* Rule 6. The one step of this open that changes the machine for everyone else on it. */
        if (!ca_set_rate(dev, devname, sample_rate, prior_rate, err, errcap)) return NULL;
        nstreams = ca_output_layout(dev, per_stream, CA_MAX_STREAMS, &total);
    }
    if (total < channels) {
        char m[240];
        snprintf(m, sizeof m, "coreaudio: '%.80s' has %u output channels and this open needs %u",
                 devname, total, channels);
        /* The rate was moved for an open that is not going to happen: put it back. */
        if (move_rate && prior_rate) ca_set_rate(dev, devname, prior_rate, sample_rate, NULL, 0);
        ca_set_err(err, errcap, m);
        return NULL;
    }

    /* Rule 2. The HAL virtualizes every device it knows to native-endian float32, interleaved
     * within a stream. Checked rather than assumed, so the IOProc never meets anything else. */
    AudioObjectID stream_ids[CA_MAX_STREAMS];
    uint32_t stream_count = 0;
    {
        const AudioObjectPropertyAddress a = { kAudioDevicePropertyStreams,
                                               kAudioObjectPropertyScopeOutput, CA_ELEMENT_MAIN };
        UInt32 sz = sizeof stream_ids;
        if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, stream_ids) == noErr)
            stream_count = (uint32_t)(sz / sizeof(AudioObjectID));
    }
    uint32_t stream_latency = 0;
    for (uint32_t i = 0; i < stream_count; ++i) {
        AudioStreamBasicDescription f;
        UInt32 sz = sizeof f;
        memset(&f, 0, sizeof f);
        if (ca_get(stream_ids[i], kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal,
                   &f, &sz) != noErr) continue;
        const bool float32 = f.mFormatID == kAudioFormatLinearPCM
            && (f.mFormatFlags & kAudioFormatFlagIsFloat) && f.mBitsPerChannel == 32
            && (f.mFormatFlags & kAudioFormatFlagIsBigEndian) == kAudioFormatFlagsNativeEndian
            && (!(f.mFormatFlags & kAudioFormatFlagIsNonInterleaved) || f.mChannelsPerFrame == 1);
        if (!float32) {
            char m[240];
            snprintf(m, sizeof m, "coreaudio: '%.80s' output stream %u is not interleaved 32-bit "
                                  "float (format 0x%08x, flags 0x%x, %u bits)", devname, i,
                     (unsigned)f.mFormatID, (unsigned)f.mFormatFlags, (unsigned)f.mBitsPerChannel);
            ca_set_err(err, errcap, m);
            return NULL;
        }
        const uint32_t lat = ca_get_u32(stream_ids[i], kAudioStreamPropertyLatency,
                                        kAudioObjectPropertyScopeGlobal, 0);
        if (lat > stream_latency) stream_latency = lat;
    }

    /* Rule 1. Ask for the engine block, clamped to what the device takes, and read it back: the
     * set is a request. Equal means the adapter passes straight through; anything else it bridges. */
    AudioValueRange range = { 0.0, 0.0 };
    bool have_range = false;
    {
        UInt32 sz = sizeof range;
        have_range = ca_get(dev, kAudioDevicePropertyBufferFrameSizeRange,
                            kAudioObjectPropertyScopeGlobal, &range, &sz) == noErr
                     && range.mMaximum >= range.mMinimum && range.mMinimum >= 1.0;
    }
    uint32_t want = block_size;
    if (have_range) {
        if ((double)want < range.mMinimum) want = (uint32_t)range.mMinimum;
        if ((double)want > range.mMaximum) want = (uint32_t)range.mMaximum;
    }
    uint32_t buffer_frames = 0;
    {
        const AudioObjectPropertyAddress a = { kAudioDevicePropertyBufferFrameSize,
                                               kAudioObjectPropertyScopeGlobal, CA_ELEMENT_MAIN };
        const UInt32 v = want;
        AudioObjectSetPropertyData(dev, &a, 0, NULL, sizeof v, &v);
        for (int i = 0; i < 20; ++i) {                        /* 200 ms */
            buffer_frames = ca_get_u32(dev, kAudioDevicePropertyBufferFrameSize,
                                       kAudioObjectPropertyScopeGlobal, 0);
            if (buffer_frames == want) break;
            os_sleep_ms(10);
        }
        if (buffer_frames == 0) buffer_frames = want;
    }

    uint32_t slot = CA_MAX_SINKS;
    for (uint32_t i = 0; i < CA_MAX_SINKS; ++i) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&g_slot[i].claimed, &expected, 1)) { slot = i; break; }
    }
    if (slot == CA_MAX_SINKS) {
        char m[160];
        snprintf(m, sizeof m, "coreaudio: %d sinks are already open in this process, which is the "
                              "limit", CA_MAX_SINKS);
        ca_set_err(err, errcap, m);
        return NULL;
    }
    atomic_store(&g_slot[slot].overloads, 0);

    CASink* s = (CASink*)calloc(1, sizeof *s);
    if (!s) {
        ca_set_err(err, errcap, "coreaudio: out of memory");
        atomic_store(&g_slot[slot].claimed, 0);
        return NULL;
    }
    s->base.vt       = &COREAUDIO_VT;
    s->sample_rate   = sample_rate;
    s->channels      = channels;
    s->block         = block_size;
    atomic_store_explicit(&s->cycle_frames, buffer_frames, memory_order_relaxed);
    s->prior_rate    = prior_rate;
    s->slot          = slot;
    s->dev           = dev;
    mach_timebase_info(&s->tb);
    if (s->tb.denom == 0) { s->tb.numer = 1; s->tb.denom = 1; }

    /* The channel map: bus channels across the streams in order. */
    {
        uint32_t c = 0;
        for (uint32_t b = 0; b < nstreams && c < channels; ++b)
            for (uint32_t k = 0; k < per_stream[b] && c < channels; ++k, ++c) {
                s->map_buf[c] = (uint16_t)b;
                s->map_ch[c]  = (uint16_t)k;
            }
        if (c < channels) {      /* more streams than CA_MAX_STREAMS and the bus did not fit */
            char m[200];
            snprintf(m, sizeof m, "coreaudio: '%.80s' spreads its outputs over more than %d streams",
                     devname, CA_MAX_STREAMS);
            ca_set_err(err, errcap, m);
            ca_release(s);
            return NULL;
        }
    }
    /* A stereo open goes where the device says stereo goes (1-based device channels). On a
     * built-in output that is channels 1 and 2 and changes nothing. */
    if (channels == 2) {
        UInt32 pair[2] = { 0, 0 }, sz = sizeof pair;
        if (ca_get(dev, kAudioDevicePropertyPreferredChannelsForStereo, kAudioObjectPropertyScopeOutput,
                   pair, &sz) == noErr
            && pair[0] >= 1 && pair[1] >= 1 && pair[0] != pair[1]) {
            uint16_t mb[2] = { 0, 0 }, mc[2] = { 0, 0 };
            bool placed[2] = { false, false };
            for (uint32_t i = 0; i < 2; ++i) {
                uint32_t ch = pair[i] - 1;
                for (uint32_t b = 0; b < nstreams; ++b) {
                    if (ch < per_stream[b]) { mb[i] = (uint16_t)b; mc[i] = (uint16_t)ch; placed[i] = true; break; }
                    ch -= per_stream[b];
                }
            }
            if (placed[0] && placed[1]) {
                s->map_buf[0] = mb[0]; s->map_ch[0] = mc[0];
                s->map_buf[1] = mb[1]; s->map_ch[1] = mc[1];
            }
        }
    }

    /* Sized for the largest cycle the device's range allows, so a buffer size another client
     * changes later still fits. Capped, and never below one engine block. */
    uint32_t max_request = buffer_frames;
    if (have_range && range.mMaximum > (double)max_request)
        max_request = range.mMaximum > (double)CA_MAX_PERIOD ? CA_MAX_PERIOD : (uint32_t)range.mMaximum;
    if (max_request > CA_MAX_PERIOD) max_request = CA_MAX_PERIOD;
    if (max_request < block_size) max_request = block_size;
    if (sink_quant_init(&s->quant, sample_rate, block_size, channels, max_request, render, user) != 0) {
        ca_set_err(err, errcap, "coreaudio: the fixed-quantum adapter could not be allocated");
        ca_release(s);
        return NULL;
    }
    s->q_capacity = s->quant.slots * s->quant.block;

    /* Rule 5's fixed part, read once. The IO buffer is added at readback (ca_output_latency). */
    s->latency_base =
          ca_get_u32(dev, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput, 0)
        + ca_get_u32(dev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput, 0)
        + stream_latency;

    snprintf(s->name, sizeof s->name, "coreaudio:%s", devname);

    const OSStatus rc = AudioDeviceCreateIOProcID(dev, ca_ioproc, s, &s->proc);
    if (rc != noErr || !s->proc) {
        char m[240];
        snprintf(m, sizeof m, "coreaudio: '%.80s' refused an IOProc (OSStatus %d)", devname, (int)rc);
        ca_set_err(err, errcap, m);
        s->proc = NULL;
        ca_release(s);
        return NULL;
    }

    /* Output only. An IOProc is on every stream of the device by default, INPUT included, and
     * starting it on a duplex device (a USB headset) starts the microphone for a sink that never
     * reads it. Mark every input stream unused for this IOProc. A refusal is not fatal: the sink
     * still plays. */
    {
        const AudioObjectPropertyAddress in_streams = { kAudioDevicePropertyStreams,
                                                        kAudioObjectPropertyScopeInput, CA_ELEMENT_MAIN };
        UInt32 sz = 0;
        if (AudioObjectGetPropertyDataSize(dev, &in_streams, 0, NULL, &sz) == noErr
            && sz >= sizeof(AudioObjectID)) {
            const UInt32 nin = (UInt32)(sz / sizeof(AudioObjectID));
            const UInt32 usz = (UInt32)(offsetof(AudioHardwareIOProcStreamUsage, mStreamIsOn)
                                        + nin * sizeof(UInt32));
            AudioHardwareIOProcStreamUsage* u = (AudioHardwareIOProcStreamUsage*)calloc(1, usz);
            if (u) {
                const AudioObjectPropertyAddress usage = { kAudioDevicePropertyIOProcStreamUsage,
                                                           kAudioObjectPropertyScopeInput, CA_ELEMENT_MAIN };
                u->mIOProc        = (void*)s->proc;
                u->mNumberStreams = nin;                     /* calloc left every mStreamIsOn 0 */
                AudioObjectSetPropertyData(dev, &usage, 0, NULL, usz, u);
                free(u);
            }
        }
    }

    /* Listeners last, once nothing after them can fail. A listener the HAL refuses costs the
     * report it would have made, not the open. */
    dispatch_once_f(&g_queue_once, NULL, ca_make_queue);
    {
        CABind bind = { slot, s };
        dispatch_sync_f(g_queue, &bind, ca_bind);
    }
    s->on_change = Block_copy(^(UInt32 n, const AudioObjectPropertyAddress* a) {
        (void)n; (void)a;
        ca_device_changed(slot);
    });
    s->on_overload = Block_copy(^(UInt32 n, const AudioObjectPropertyAddress* a) {
        (void)n; (void)a;                       /* IO thread: one atomic add, nothing else */
        atomic_fetch_add_explicit(&g_slot[slot].overloads, 1u, memory_order_relaxed);
    });
    {
        const AudioObjectPropertyAddress alive = { kAudioDevicePropertyDeviceIsAlive,
                                                   kAudioObjectPropertyScopeGlobal, CA_ELEMENT_MAIN };
        const AudioObjectPropertyAddress rate  = { kAudioDevicePropertyNominalSampleRate,
                                                   kAudioObjectPropertyScopeGlobal, CA_ELEMENT_MAIN };
        const AudioObjectPropertyAddress over  = { kAudioDeviceProcessorOverload,
                                                   kAudioObjectPropertyScopeGlobal, CA_ELEMENT_MAIN };
        AudioObjectAddPropertyListenerBlock(dev, &alive, g_queue, s->on_change);
        AudioObjectAddPropertyListenerBlock(dev, &rate,  g_queue, s->on_change);
        /* NULL queue: invoked directly. With a queue the HAL dispatches this one SYNCHRONOUSLY
         * from the IO thread, which would deadlock against a handler that is stopping the device. */
        AudioObjectAddPropertyListenerBlock(dev, &over,  NULL,    s->on_overload);
    }

    /* The rate was read back before the buffer-size poll and the listener only exists now.
     * Another application moving the device in between would never be seen, and the sink would
     * render pitched and report itself healthy. One more read closes the window. */
    {
        const uint32_t now_rate = ca_nominal_rate(dev);
        if (now_rate != 0 && now_rate != sample_rate) {
            char m[250];
            snprintf(m, sizeof m, "coreaudio: '%.60s' runs at %u Hz, not the engine's %u Hz: another "
                                  "application changed its rate during the open", devname, now_rate,
                     sample_rate);
            ca_close(&s->base);
            ca_set_err(err, errcap, m);
            return NULL;
        }
    }

    if (err && errcap) err[0] = 0;      /* nothing degrades on this backend: it opens exact or not at all */
    return &s->base;
}
