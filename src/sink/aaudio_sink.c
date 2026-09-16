/*
 * aaudio_sink.c — the AAudio backend (docs/backends.md, phase 3).
 *
 * WHY IT EXISTS. Standalone VR headsets (Meta Quest, Pico) run Android, and a headset's audio is
 * an AAudio stream. It is the same reach WASAPI buys on a PC headset: nothing here is VR-specific,
 * and the pose still arrives through bwa_set_listener_pose from the game engine. The array never
 * comes near this sink - Android AUTO only offers it for a 2-channel request, and anything wider
 * falls to the offline sink (sink.c).
 *
 * THE SHAPE. AAudio owns the thread and calls back with a frame count that is a HINT, not a
 * promise (setFramesPerDataCallback says so), so everything goes through the fixed-quantum adapter
 * like every backend but ALSA. The callback converts and interleaves straight out of the adapter's
 * FIFO into the device buffer, which for AAUDIO_FORMAT_PCM_FLOAT is a plain interleave.
 *
 * THREE THINGS THE SINK DELIBERATELY DOES NOT DO:
 *   - It never reopens. AAudio forbids touching the stream from the data callback, and the error
 *     callback's AAUDIO_ERROR_DISCONNECTED is a route change, not a fault to paper over. The sink
 *     sets device_lost and starts the host-paced thread so the engine's clocks keep advancing
 *     (docs/backends.md rule 4); what to do about it is the control thread's decision.
 *   - It never touches the thread priority. AAudio's callback thread is already elevated under
 *     AAUDIO_PERFORMANCE_MODE_LOW_LATENCY (rule 7). Only the host-paced fallback thread asks, and
 *     it tolerates the refusal Android always gives an unprivileged app.
 *   - It never calls AAudioStreamBuilder_setUsage. That entry point is __INTRODUCED_IN(28) and the
 *     library targets API 26, so an unguarded call is a link-time weak symbol that crashes on an
 *     API 26 device. docs/backends.md allows leaving the usage at its default, which is
 *     AAUDIO_USAGE_MEDIA - correct for a game's audio. A headset build that wants
 *     AAUDIO_USAGE_GAME can raise ANDROID_PLATFORM and add the call behind a runtime check.
 *
 * AUDIO THREAD. The data callback is AAudio's own high-priority thread; the audio-thread
 * invariants apply to every line of it. Everything it needs is allocated at open.
 */
#include "sink/sink.h"
#include "sink/sink_convert.h"
#include "sink/sink_quant.h"
#include "os/os.h"
#include "core/profile.h"

#include <aaudio/AAudio.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The largest callback the FIFO is sized for. The stream's buffer CAPACITY is the real ceiling and
 * is read at open; this only caps what a pathological capacity could make the sink allocate, the
 * same role JACK_MAX_PERIOD plays in jack_sink.c. */
#define AAUDIO_MAX_CALLBACK 8192

typedef struct {
    bwa_sink base;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t block;              /* the engine quantum; block_size() reports this               */
    uint32_t q_capacity;         /* frames the adapter can serve in one pull                    */
    uint32_t burst;              /* AAudioStream_getFramesPerBurst: the device's own quantum    */
    uint32_t buffer_frames;      /* what setBufferSizeInFrames(2 bursts) actually settled on    */
    uint32_t capacity_frames;    /* AAudioStream_getBufferCapacityInFrames                      */
    bool     exclusive;          /* the sharing mode the stream actually got                    */

    AAudioStream* stream;
    SinkQuant     quant;

    /* --- data-callback state, audio thread only --- */
    int32_t  prev_xruns;         /* AAudioStream_getXRunCount is cumulative; the DELTA is the event */

    /* --- written by the callback threads, read from the control thread. Relaxed: monotonic
     * counts and one-way flags nobody synchronizes ON. --- */
    _Atomic int      running;        /* start() opens the gate, stop() closes it                */
    _Atomic uint32_t lost;           /* AAUDIO_ERROR_DISCONNECTED: the route changed            */
    _Atomic uint32_t have_ts;        /* getTimestamp has answered at least once (rule 4)        */
    _Atomic uint32_t latency_frames; /* rule 5; latched once, since the header promises constant */

    /* Host-paced fallback after a disconnect (rule 4), the same shape jack_sink.c uses: the
     * engine's dsp clock and playheads keep advancing on silence instead of freezing. */
    os_thread    host_thread;
    _Atomic int  host_started, host_stop;

    char name[192];              /* "aaudio:default" or "aaudio:<device id>", ASCII */
} AAudioSink;

static void aaudio_set_err(char* err, size_t cap, const char* msg) {
    if (err && cap) { strncpy(err, msg, cap - 1); err[cap - 1] = 0; }
}

/* ---- the data callback --------------------------------------------------------------------- */

typedef struct { float* dst; uint32_t channels; } AAudioOut;

/* sink_quant hands one contiguous run of its FIFO at a time. The stream is PCM_FLOAT, so this is
 * an interleave and nothing else - the shared header still owns the NaN rule, because a NaN
 * reaching the device is a click on every backend. */
static void aaudio_out(void* user, const float* planar, uint32_t stride, uint32_t frame_offset,
                       uint32_t n) {
    AAudioOut* o = (AAudioOut*)user;
    sink_convert_interleaved(o->dst + (size_t)frame_offset * o->channels, planar, o->channels, n,
                             stride, SINK_FMT_F32);
}

static void aaudio_discard_out(void* u, const float* p, uint32_t s, uint32_t o, uint32_t n) {
    (void)u; (void)p; (void)s; (void)o; (void)n;
}

static aaudio_data_callback_result_t aaudio_data(AAudioStream* stream, void* user, void* audio,
                                                 int32_t nframes) {
    AAudioSink* s = (AAudioSink*)user;
    float* out = (float*)audio;
    if (nframes <= 0) return AAUDIO_CALLBACK_RESULT_CONTINUE;
    uint32_t n = (uint32_t)nframes;

    if (!atomic_load_explicit(&s->running, memory_order_acquire)) {
        memset(out, 0, (size_t)n * s->channels * sizeof(float));
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    /* Rule 3's pair, read ONCE per callback. It fails for the first callbacks of a stream's life
     * (the service has presented nothing yet), which is exactly what rule 4 means by "measured
     * only after the first success". Until then the host clock stamps the block, which is the same
     * clock AAudio reports its timestamps on (CLOCK_MONOTONIC = os_monotonic_ns on Android), so
     * the switch is a one-time offset of about the stream latency rather than a change of base -
     * and the adapter floors a stamp at the previous one plus a nominal block, so even that step
     * cannot make a stamp run backward. */
    uint64_t host_ns = 0;
    int64_t  ts_pos = 0, ts_ns = 0;
    const bool ts_ok = (AAudioStream_getTimestamp(stream, CLOCK_MONOTONIC, &ts_pos, &ts_ns)
                        == AAUDIO_OK) && ts_ns > 0;
    if (ts_ok) {
        host_ns = (uint64_t)ts_ns;
        if (!atomic_load_explicit(&s->have_ts, memory_order_relaxed)) {
            /* Rule 5, latched the once: frames written minus frames presented, the difference
             * extrapolated to now because the timestamp describes a frame the device presented
             * some time ago. output_latency() must stay constant for the life of the sink (see
             * sink.h), so this replaces the open's buffer-size estimate and is never revised. */
            const uint64_t now = sink_quant_now_ns();
            const int64_t  ahead = (now > (uint64_t)ts_ns)
                ? (int64_t)(((now - (uint64_t)ts_ns) * (uint64_t)s->sample_rate) / 1000000000ull)
                : 0;
            const int64_t written = AAudioStream_getFramesWritten(stream);
            int64_t lat = written - (ts_pos + ahead);
            if (lat < 0) lat = 0;
            if (lat > (int64_t)s->capacity_frames * 4) lat = (int64_t)s->capacity_frames;
            atomic_store_explicit(&s->latency_frames,
                                  (uint32_t)lat + sink_quant_queued(&s->quant),
                                  memory_order_relaxed);
            atomic_store_explicit(&s->have_ts, 1u, memory_order_release);
        }
    } else {
        host_ns = sink_quant_now_ns();
    }

    /* The OS's own fault count, as a delta. dropped_frames is 0 on purpose: AAudio reports how
     * MANY times the stream ran dry and never for how long, and inventing an estimate from the
     * callback interval would be a guess dressed as a measurement (sink_quant_note_dropout's
     * contract allows exactly this). */
    {
        const int32_t x = AAudioStream_getXRunCount(stream);
        if (x > s->prev_xruns) {
            for (int32_t i = s->prev_xruns; i < x; ++i) sink_quant_note_dropout(&s->quant, 0);
            s->prev_xruns = x;
        }
    }

    if (n > s->q_capacity) {
        /* A callback past the buffer capacity the FIFO was sized for. Serve what we can and
         * silence the rest rather than leave the device buffer holding whatever was there, and
         * book the shortfall as the dropout it is. */
        memset(out + (size_t)s->q_capacity * s->channels, 0,
               (size_t)(n - s->q_capacity) * s->channels * sizeof(float));
        sink_quant_note_dropout(&s->quant, n - s->q_capacity);
        n = s->q_capacity;
    }

    /* device_pos_valid is deliberately FALSE. AAudio reports the fault directly through
     * getXRunCount above, and the queued-depth rule would count the SAME starve a second time:
     * the callback keeps asking for a burst whether or not we were late, so `written` lags the
     * presented position by exactly the silence the service already counted. */
    AAudioOut o = { out, s->channels };
    BWA_ZONE_BEGIN(zb, "aaudio callback");
    sink_quant_pull(&s->quant, n, 0, false, host_ns, aaudio_out, &o);
    BWA_ZONE_END(zb);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

/* ---- the host-paced fallback (rule 4) ------------------------------------------------------ */

static void aaudio_host_thread(void* arg) {
    AAudioSink* s = (AAudioSink*)arg;
    BWA_THREAD_NAME("bw-audio (AAudio lost)");
    /* Asked for, and its refusal tolerated: Android grants SCHED_FIFO to the audio service, not to
     * an app thread, so this is the degrade path by default. The loop is pacing silence anyway. */
    const uint64_t block_ns = (uint64_t)s->block * 1000000000ull / (uint64_t)s->sample_rate;
    const bool rt = (os_thread_set_realtime(block_ns) == 0);
    uint64_t next_ns = 0;
    while (!atomic_load_explicit(&s->host_stop, memory_order_relaxed)) {
        const uint64_t now = sink_quant_now_ns();
        if (next_ns == 0 || now > next_ns + block_ns * 8ull) next_ns = now;
        if (atomic_load_explicit(&s->running, memory_order_relaxed))
            sink_quant_pull(&s->quant, s->block, 0, false, now, aaudio_discard_out, NULL);
        next_ns += block_ns;
        os_sleep_until_ns(next_ns);          /* absolute deadline: a relative sleep drifts */
    }
    if (rt) os_thread_clear_realtime();
}

/* The route changed under us (a headset unplugged, a Bluetooth device taken). AAudio calls this on
 * its own error thread and forbids touching the stream from the DATA callback, so this is where a
 * reopen would have to live - and rule 4 says there is no reopen. Report, and keep the clocks
 * moving. Same claim-then-publish handshake jack_sink.c's shutdown uses, so close() can tell
 * "no thread" from "a thread is being created right now".
 *
 * Starting a thread that pulls the adapter is only safe here because AAudio has already stopped
 * the data callback by the time this runs: the two never pull concurrently. */
static void aaudio_error(AAudioStream* stream, void* user, aaudio_result_t error) {
    (void)stream;
    AAudioSink* s = (AAudioSink*)user;
    if (error != AAUDIO_ERROR_DISCONNECTED && error != AAUDIO_ERROR_UNAVAILABLE) return;
    atomic_store_explicit(&s->lost, 1u, memory_order_relaxed);
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&s->host_started, &expected, 1,
                                                memory_order_relaxed, memory_order_relaxed)) {
        if (os_thread_create(&s->host_thread, aaudio_host_thread, s) != 0)
            atomic_store_explicit(&s->host_started, 2, memory_order_release);   /* 2 = never ran */
        else
            atomic_store_explicit(&s->host_started, 3, memory_order_release);   /* 3 = joinable  */
    }
}

/* ---- vtable -------------------------------------------------------------------------------- */

static int aaudio_start(bwa_sink* base) {
    AAudioSink* s = (AAudioSink*)base;
    atomic_store_explicit(&s->running, 1, memory_order_release);
    if (!s->stream) return 0;   /* only reachable after close(); the gate above still opens so a
                                 * host-paced thread keeps the engine's clocks advancing */
    if (AAudioStream_requestStart(s->stream) != AAUDIO_OK) {
        atomic_store_explicit(&s->running, 0, memory_order_release);
        return 1;
    }
    return 0;
}

/* Rule 8's first half. requestStop is ASYNCHRONOUS, so waiting for the state change is what makes
 * "the callback has returned and no further one can start" true - which is what lets the sink test
 * read its probe without a race, and what lets close() free the adapter. */
static void aaudio_stop(bwa_sink* base) {
    AAudioSink* s = (AAudioSink*)base;
    atomic_store_explicit(&s->running, 0, memory_order_release);
    if (!s->stream) return;
    if (AAudioStream_requestStop(s->stream) == AAUDIO_OK) {
        aaudio_stream_state_t st = AAUDIO_STREAM_STATE_STOPPING;
        AAudioStream_waitForStateChange(s->stream, AAUDIO_STREAM_STATE_STOPPING, &st,
                                        500ull * 1000ull * 1000ull);   /* 500 ms, in nanoseconds */
    }
}

static void aaudio_close(bwa_sink* base) {
    AAudioSink* s = (AAudioSink*)base;
    aaudio_stop(base);
    /* Rule 8: AAudioStream_close joins the data and error callback threads, so nothing can be
     * running in this file once it returns. That is what makes the host_started read below safe. */
    if (s->stream) { AAudioStream_close(s->stream); s->stream = NULL; }
    /* 1 is the transient state INSIDE the error callback, between claiming the slot and publishing
     * the outcome. Waiting it out is what makes "did a host-paced thread start?" answerable at all;
     * reading 1 and moving on would either leak the thread or free the adapter under it. */
    int hs = atomic_load_explicit(&s->host_started, memory_order_acquire);
    while (hs == 1) { os_sleep_ms(1); hs = atomic_load_explicit(&s->host_started, memory_order_acquire); }
    if (hs == 3) {
        atomic_store_explicit(&s->host_stop, 1, memory_order_relaxed);
        os_thread_join(&s->host_thread);
    }
    sink_quant_free(&s->quant);
    free(s);
}

static const char* aaudio_backend(bwa_sink* base)     { return ((AAudioSink*)base)->name; }
static uint32_t aaudio_block_size(bwa_sink* base)     { return ((AAudioSink*)base)->block; }
static uint32_t aaudio_output_latency(bwa_sink* base) {
    return atomic_load_explicit(&((AAudioSink*)base)->latency_frames, memory_order_relaxed);
}

/* `measured` follows getTimestamp having answered, per docs/backends.md rule 4. The xrun counter
 * is a direct device report and would justify `true` from the first callback, but both numbers
 * come from the same stream service: a stream that has never produced a timestamp has never
 * presented a frame, so a zero xrun count on it says nothing a caller should act on. */
static void aaudio_health(bwa_sink* base, bwa_sink_health* out) {
    AAudioSink* s = (AAudioSink*)base;
    sink_quant_health(&s->quant, out);      /* blocks, dropouts, late_blocks, peak, period */
    out->measured    = atomic_load_explicit(&s->have_ts, memory_order_relaxed) != 0;
    out->device_lost = atomic_load_explicit(&s->lost, memory_order_relaxed);
}

/* Internal readbacks for the sink test (declared in sink.h, deliberately not in bw_audio.h): the
 * device's own quantum and the buffer the sink settled on. The test needs both to size a stall
 * that certainly starves the stream, the same reason the WASAPI and ALSA pairs exist. */
uint32_t sink_aaudio_burst_frames(bwa_sink* base)  { return ((AAudioSink*)base)->burst; }
uint32_t sink_aaudio_buffer_frames(bwa_sink* base) { return ((AAudioSink*)base)->buffer_frames; }

static const bwa_sink_vtbl AAUDIO_VT = {   /* designated: stop/close share a signature, so a positional swap would be silent */
    .type = BWA_SINK_AAUDIO,
    .start = aaudio_start, .stop = aaudio_stop, .close = aaudio_close,
    .backend = aaudio_backend, .block_size = aaudio_block_size,
    .output_latency = aaudio_output_latency,
    .health = aaudio_health,
};

/* ---- device enumeration -------------------------------------------------------------------- */

/* AAudio has no C enumeration: the device list lives in Java's AudioManager, which an NDK library
 * cannot reach without a JNIEnv it was never given. So the backend reports exactly what it can
 * open without one - the default output - and bwa_desc.device additionally accepts a decimal
 * Android device id the host app got from AudioManager.getDevices, which goes straight to
 * AAudioStreamBuilder_setDeviceId. */
uint32_t sink_aaudio_device_count(void) { return 1; }

bool sink_aaudio_device_name(uint32_t index, char* buf, uint32_t cap) {
    if (!buf || cap == 0) return false;
    buf[0] = 0;
    if (index != 0) return false;
    return sink_copy_device_name(buf, cap, "default");
}

bool sink_aaudio_device_id(uint32_t index, char* buf, uint32_t cap) {
    if (!buf || cap == 0) return false;
    buf[0] = 0;
    if (index != 0) return false;
    return sink_copy_device_name(buf, cap, "0");   /* AAUDIO_UNSPECIFIED, spelled for a picker */
}

/* ---- open ----------------------------------------------------------------------------------- */

/* The device's own output rate, or 0 when it cannot be had. Opening a stream with everything
 * unspecified is the only way to ask AAudio this from C: the builder resolves to the default
 * device's native format, and the stream reports it. Nothing is started, so the cost is one open
 * and one close on the control thread.
 *
 * It exists for rule 6. A shared-mode stream opened AT the engine rate reports the engine rate
 * whether or not the OS is resampling underneath, so without this probe the sink could not tell a
 * native 48 kHz device from a 44.1 kHz one being converted, and the "succeeded but degraded"
 * channel would have nothing to say. */
static uint32_t aaudio_device_rate(int32_t device_id) {
    AAudioStreamBuilder* b = NULL;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK || !b) return 0;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_NONE);
    if (device_id > 0) AAudioStreamBuilder_setDeviceId(b, device_id);
    AAudioStream* probe = NULL;
    uint32_t rate = 0;
    if (AAudioStreamBuilder_openStream(b, &probe) == AAUDIO_OK && probe) {
        const int32_t r = AAudioStream_getSampleRate(probe);
        if (r > 0) rate = (uint32_t)r;
        AAudioStream_close(probe);
    }
    AAudioStreamBuilder_delete(b);
    return rate;
}

bwa_sink* bwa_aaudio_sink_open(uint32_t sample_rate, uint32_t block_size, uint32_t channels,
                               const char* device, uint32_t flags, bool exact_rate,
                               bwa_render_fn render, void* user, char* err, size_t errcap) {
    if (!render || channels == 0 || block_size == 0 || sample_rate == 0) {
        aaudio_set_err(err, errcap, "aaudio: bad arguments");
        return NULL;
    }
    /* Stereo only, said at the open rather than discovered as silence. Android AUTO already sends
     * a wider request straight to the offline sink; this is the message an explicit
     * BWA_SINK_AAUDIO with a 26-speaker layout has to see. */
    if (channels > 2) {
        char m[192];
        snprintf(m, sizeof m, "aaudio: this backend is stereo only and was asked for %u channels; "
                              "Android has no array transport (see docs/backends.md)", channels);
        aaudio_set_err(err, errcap, m);
        return NULL;
    }

    /* Rule 10 for a backend with no device list: `device` is a decimal Android device id from
     * Java's AudioManager. A non-numeric string is a name meant for another backend, and saying so
     * beats opening the default output and playing into whatever that is.
     *
     * "0" is AAUDIO_UNSPECIFIED, which is also the id the device query reports for the one entry
     * it lists, so a picker that persisted that id round-trips to the default output rather than
     * to a device numbered zero. There is no such device: AudioManager ids start at 1. */
    int32_t device_id = AAUDIO_UNSPECIFIED;
    if (device && *device) {
        char* end = NULL;
        const long v = strtol(device, &end, 10);
        if (!end || *end || v < 0) {
            char m[224];
            snprintf(m, sizeof m, "aaudio: device '%s' is not a decimal Android device id; AAudio "
                                  "has no device names, so pass the id AudioManager.getDevices "
                                  "reports (or NULL for the default output)", device);
            aaudio_set_err(err, errcap, m);
            return NULL;
        }
        device_id = (int32_t)v;
    }

    const uint32_t dev_rate = aaudio_device_rate(device_id);

    /* The array's rule (rule 6): the device runs at the engine rate or the open fails. Checked
     * BEFORE the real open, because a shared-mode stream would happily report the rate we asked
     * for while the service resampled underneath it. */
    if (exact_rate && dev_rate && dev_rate != sample_rate) {
        char m[240];
        snprintf(m, sizeof m, "aaudio: the output device runs at %u Hz, not the engine's %u Hz, and "
                              "an exact-rate sink may not be resampled; create the engine at %u Hz",
                 dev_rate, sample_rate, dev_rate);
        aaudio_set_err(err, errcap, m);
        return NULL;
    }

    AAudioStreamBuilder* b = NULL;
    aaudio_result_t rc = AAudio_createStreamBuilder(&b);
    if (rc != AAUDIO_OK || !b) {
        char m[192];
        snprintf(m, sizeof m, "aaudio: no AAudio stream builder (%s); this needs Android 8.0 "
                              "(API 26) or later", AAudio_convertResultToText(rc));
        aaudio_set_err(err, errcap, m);
        return NULL;
    }

    AAudioSink* s = (AAudioSink*)calloc(1, sizeof *s);
    if (!s) {
        aaudio_set_err(err, errcap, "aaudio: out of memory");
        AAudioStreamBuilder_delete(b);
        return NULL;
    }
    s->base.vt     = &AAUDIO_VT;
    s->sample_rate = sample_rate;
    s->channels    = channels;
    s->block       = block_size;

    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    /* SHARED by default, for the same reason WASAPI opens shared: a VR runtime keeps its own audio
     * open beside the engine, and AAudio grants exclusive (the MMAP path) rarely in any case. */
    AAudioStreamBuilder_setSharingMode(b, (flags & BWA_SINK_FLAG_EXCLUSIVE)
                                          ? AAUDIO_SHARING_MODE_EXCLUSIVE
                                          : AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(b, (int32_t)channels);
    AAudioStreamBuilder_setSampleRate(b, (int32_t)sample_rate);
    /* A HINT, which is the whole reason the fixed-quantum adapter is in this path: AAudio may call
     * back with the burst instead, or with a different count after a route change. */
    AAudioStreamBuilder_setFramesPerDataCallback(b, (int32_t)block_size);
    AAudioStreamBuilder_setDataCallback(b, aaudio_data, s);
    AAudioStreamBuilder_setErrorCallback(b, aaudio_error, s);
    if (device_id != AAUDIO_UNSPECIFIED) AAudioStreamBuilder_setDeviceId(b, device_id);

    rc = AAudioStreamBuilder_openStream(b, &s->stream);
    AAudioStreamBuilder_delete(b);
    if (rc != AAUDIO_OK || !s->stream) {
        /* The "there is no device" message the sink test's skip rule keys on, and the only failure
         * here that is a property of the machine rather than of the sink. Everything else keeps
         * AAudio's own result text, which names the real cause. */
        char m[240];
        if (dev_rate && dev_rate != sample_rate)
            snprintf(m, sizeof m, "aaudio: the stream could not be opened at %u Hz (%s); the output "
                                  "device runs at %u Hz", sample_rate,
                     AAudio_convertResultToText(rc), dev_rate);
        else
            snprintf(m, sizeof m, "aaudio: no output stream (%s); there is no usable AAudio output "
                                  "device", AAudio_convertResultToText(rc));
        aaudio_set_err(err, errcap, m);
        free(s);
        return NULL;
    }

    /* The engine has no resampler, so a stream that did not land on the engine rate is a failure
     * however it got there. In shared mode this practically never fires - the service converts and
     * reports our rate - which is why the dev_rate probe above exists for the degradation report. */
    const uint32_t got_rate = (uint32_t)AAudioStream_getSampleRate(s->stream);
    const uint32_t got_ch   = (uint32_t)AAudioStream_getChannelCount(s->stream);
    const aaudio_format_t got_fmt = AAudioStream_getFormat(s->stream);
    if (got_rate != sample_rate || got_ch != channels || got_fmt != AAUDIO_FORMAT_PCM_FLOAT) {
        char m[248];
        snprintf(m, sizeof m, "aaudio: the stream opened at %u Hz / %u channels / format %d, not "
                              "the engine's %u Hz / %u channels / float, and the engine does not "
                              "resample", got_rate, got_ch, (int)got_fmt, sample_rate, channels);
        aaudio_set_err(err, errcap, m);
        AAudioStream_close(s->stream);
        free(s);
        return NULL;
    }

    s->exclusive = (AAudioStream_getSharingMode(s->stream) == AAUDIO_SHARING_MODE_EXCLUSIVE);
    {
        const int32_t burst = AAudioStream_getFramesPerBurst(s->stream);
        s->burst = (burst > 0) ? (uint32_t)burst : block_size;
    }
    {
        const int32_t cap = AAudioStream_getBufferCapacityInFrames(s->stream);
        s->capacity_frames = (cap > 0) ? (uint32_t)cap : (2u * s->burst);
    }
    /* Two bursts: the shallowest buffer that still survives one late callback, which is what
     * AAUDIO_PERFORMANCE_MODE_LOW_LATENCY is for. The device decides what it can actually give. */
    AAudioStream_setBufferSizeInFrames(s->stream, (int32_t)(2u * s->burst));
    {
        const int32_t bs = AAudioStream_getBufferSizeInFrames(s->stream);
        s->buffer_frames = (bs > 0) ? (uint32_t)bs : (2u * s->burst);
    }

    /* Sized for the stream's buffer CAPACITY, which is the most a callback can ask for, capped so
     * an absurd capacity cannot turn into an absurd allocation. */
    uint32_t max_request = s->capacity_frames;
    if (max_request < block_size) max_request = block_size;
    if (max_request > AAUDIO_MAX_CALLBACK) max_request = AAUDIO_MAX_CALLBACK;
    if (sink_quant_init(&s->quant, sample_rate, block_size, channels, max_request,
                        render, user) != 0) {
        aaudio_set_err(err, errcap, "aaudio: the fixed-quantum adapter could not be allocated");
        AAudioStream_close(s->stream);
        free(s);
        return NULL;
    }
    s->q_capacity = s->quant.slots * s->quant.block;
    s->prev_xruns = AAudioStream_getXRunCount(s->stream);
    if (s->prev_xruns < 0) s->prev_xruns = 0;

    /* Rule 5's starting value. The real figure is latched on the first timestamp (see the data
     * callback); until then the buffer the sink asked for plus what the adapter holds is the best
     * honest answer, and it is never 0, which would mean "unknown". */
    atomic_store_explicit(&s->latency_frames,
                          s->buffer_frames + ((s->burst == block_size) ? 0u : block_size),
                          memory_order_relaxed);

    if (device_id != AAUDIO_UNSPECIFIED)
        snprintf(s->name, sizeof s->name, "aaudio:%d", (int)device_id);
    else
        snprintf(s->name, sizeof s->name, "aaudio:default");

    /* Rule 6's "succeeded but degraded" channel, and the sharing-mode fallback beside it. Neither
     * is a failure; both are things a caller has to be able to read after a successful start. */
    if (err && errcap) err[0] = 0;
    if (dev_rate && dev_rate != sample_rate) {
        char m[240];
        snprintf(m, sizeof m, "aaudio: the output device runs at %u Hz and the OS is resampling the "
                              "engine's %u Hz; create the engine at %u Hz to avoid it",
                 dev_rate, sample_rate, dev_rate);
        aaudio_set_err(err, errcap, m);
    } else if ((flags & BWA_SINK_FLAG_EXCLUSIVE) && !s->exclusive) {
        aaudio_set_err(err, errcap, "aaudio: exclusive sharing was refused and the stream is "
                                    "shared; Android grants the MMAP path rarely");
    }
    return &s->base;
}
