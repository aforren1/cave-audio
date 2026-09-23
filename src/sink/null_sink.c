/*
 * null_sink.c — offline sink. A dedicated thread paces blocks from the
 * high-resolution clock at sample_rate/block_size and invokes the engine's render,
 * then discards the audio. No device, no Dante hardware. This is the desk/CI path
 * and the in-memory array render the `binaural` profile relies on.
 *
 * The render callback runs on this thread and must stay alloc/lock/syscall-free per
 * the audio-thread invariants; the pacing wait happens *outside* the callback.
 *
 * Portable: the thread, the clock and the sleep go through os.h, so this sink — the one every
 * platform gets, and the one CI runs on — builds wherever the library does.
 */
#include "sink/sink.h"
#include "os/os.h"
#include "core/profile.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* The longest single pacing wait. Only bounds how long a stop request can sit unnoticed; the
 * DEADLINE is absolute either way, so chopping the wait costs nothing in accuracy. */
#define STOP_POLL_NS 5000000ull   /* 5 ms */

typedef struct {
    bwa_sink     base;
    uint32_t     sample_rate, block_size, channels;
    bwa_render_fn render;
    void*        user;
    float*       bus;            /* planar channels * block_size */
    os_thread    thread;
    _Atomic int  stop_flag;

    /* Health counters (bwa_sink_health): written on the render thread, read from the control
     * thread. Relaxed — each is an independent count that orders nothing else. */
    _Atomic uint64_t h_blocks, h_dropouts, h_dropped_frames, h_late;
    SinkPeakWindow   h_peak;      /* recent render peak: one writer, packed words (sink.h) */
} NullSink;

/* TEST HOOK — declared in sink.h (exported from the dll there), deliberately not in bw_audio.h:
 * lets a test observe the blocks this sink otherwise discards, i.e. see a profile's device-bound
 * output off-hardware (the smoke test uses it to pin binaural laterality through the REAL dll). */
void (*bwa_null_sink_tap)(const float* bus, uint32_t channels, uint32_t block_size) = NULL;

/* TEST HOOK — declared in sink.h, deliberately not in bw_audio.h. Set it to N and the next block
 * advances the reported device position by N EXTRA blocks, exactly as a device that clocked out
 * audio while we were not there to render it. It is the only way to exercise the dropout accounting
 * off-hardware: a real missed deadline needs a real device, but the arithmetic that turns a position
 * jump into a count is ordinary code and must not be shipped untested. Consumed once (reset to 0).
 *
 * Plain `volatile int`, not an atomic, because sink.h has to compile as C++ (asio_sink.cpp and
 * wasapi_sink.cpp include it) and <stdatomic.h> cannot appear there. One test-thread writer, one
 * sink-thread read-and-clear, aligned int: the ordering nobody depends on is the only thing lost. */
volatile int bwa_null_sink_skip_blocks = 0;

static void null_set_err(char* err, size_t cap, const char* msg) {
    if (err && cap) { strncpy(err, msg, cap - 1); err[cap - 1] = 0; }
}

static void null_thread(void* arg) {
    NullSink* s = (NullSink*)arg;
    BWA_THREAD_NAME("bw-audio (null)");       /* same render() the ASIO callback drives — profile w/o hardware */
    /* No os_timer_resolution_begin here any more: os_sleep_until_ns gets its precision from a
     * high-resolution waitable timer, so a tool that merely happens to run this sink no longer
     * raises the whole machine's timer resolution for as long as it is open. */

    const uint64_t base   = os_monotonic_ns();
    const double block_ns = 1.0e9 * (double)s->block_size / (double)s->sample_rate;
    const uint64_t budget_ns = (uint64_t)s->block_size * 1000000000ull / (uint64_t)s->sample_rate;
    uint64_t sample_pos = 0, block_index = 0, predicted_pos = 0;

    /* This loop has a real deadline, so it asks for the platform's real-time footing like any other
     * render thread. Best effort, and the return value is deliberately ignored: an offline sink
     * that could not get it just runs at normal priority, and there is no caller to tell. On Darwin
     * the call is what exempts the pacing wait from timer coalescing, which is why an OFFLINE sink
     * wants it at all. */
    const bool rt = os_thread_set_realtime(budget_ns) == 0;

    while (!atomic_load_explicit(&s->stop_flag, memory_order_relaxed)) {
        /* The injected skip (bwa_null_sink_skip_blocks) advances the reported position without
         * rendering those blocks — what a starved device looks like from in here. */
        const int skip = bwa_null_sink_skip_blocks;
        if (skip > 0) { bwa_null_sink_skip_blocks = 0; sample_pos += (uint64_t)skip * s->block_size; }

        /* Same comparison the ASIO sink makes, through the same helper: where the last block said
         * this one would land, versus where it did. */
        if (predicted_pos) {
            const uint64_t lost = sink_position_gap(predicted_pos, sample_pos, s->block_size);
            if (lost) {
                atomic_fetch_add_explicit(&s->h_dropouts, 1u, memory_order_relaxed);
                atomic_fetch_add_explicit(&s->h_dropped_frames, lost, memory_order_relaxed);
            }
        }
        predicted_pos = sample_pos + s->block_size;

        /* The ABSOLUTE monotonic clock, not an offset from thread start. Two reasons, and the
         * second is a bug this had. Every other backend stamps the platform clock (the WASAPI sink
         * through sink_quant_now_ns, the ASIO sink through QPC), so subtracting `base` here made
         * the null sink the one backend with its own epoch. And on a clock whose tick is coarse
         * enough - Apple Silicon's mach_absolute_time is 41.67 ns - the FIRST block's stamp came
         * out exactly 0, which rt.c's publish gate reads as "no stamp"; bwa_get_clock then had no
         * pair until block 1, and a late wake on a loaded machine pushed that past the 30 ms window
         * the smoke test allows. Caught by the macOS CI runner, not by review. */
        bwa_timestamp ts = {
            .sample_pos     = sample_pos,
            .system_time_ns = os_monotonic_ns(),
        };
        BWA_ZONE_BEGIN(zb, "null block");
        const uint64_t t0 = os_monotonic_ns();
        s->render(s->user, s->bus, s->block_size, &ts);   /* engine produces a block */
        const uint64_t render_ns = os_monotonic_ns() - t0;
        if (bwa_null_sink_tap) bwa_null_sink_tap(s->bus, s->channels, s->block_size);
        /* null sink: the rendered bus is intentionally discarded. */
        BWA_ZONE_END(zb);
        BWA_FRAME_MARK();

        /* Overrunning the block period is real here, not simulated: this thread has a deadline and
         * the pacing loop below is what enforces it. On a device it is what eventually becomes a
         * dropout, so counting it off-hardware is the honest half of the measurement CI can do. */
        if (render_ns > budget_ns) atomic_fetch_add_explicit(&s->h_late, 1u, memory_order_relaxed);
        sink_peak_note(&s->h_peak, sample_pos, s->sample_rate, render_ns);
        atomic_fetch_add_explicit(&s->h_blocks, 1u, memory_order_relaxed);

        sample_pos  += s->block_size;
        block_index += 1;

        /* Pace to the block's ABSOLUTE deadline, counted from `base` rather than from the last
         * wake, so a late block is caught up instead of pushing every later one back. The wait is
         * chopped into STOP_POLL_NS pieces for one reason only: bwa_stop must not have to wait a
         * whole block period, and there is no way to wake this timer early. */
        const uint64_t deadline = base + (uint64_t)((double)block_index * block_ns);
        while (!atomic_load_explicit(&s->stop_flag, memory_order_relaxed)) {
            const uint64_t now = os_monotonic_ns();
            if (now >= deadline) break;
            const uint64_t piece = now + STOP_POLL_NS;
            os_sleep_until_ns(piece < deadline ? piece : deadline);
        }
    }

    if (rt) os_thread_clear_realtime();
}

static int null_start(bwa_sink* base) {
    NullSink* s = (NullSink*)base;
    if (os_thread_valid(&s->thread)) return 0;   /* already running */
    atomic_store_explicit(&s->stop_flag, 0, memory_order_relaxed);
    return os_thread_create(&s->thread, null_thread, s);
}

static void null_stop(bwa_sink* base) {
    NullSink* s = (NullSink*)base;
    if (!os_thread_valid(&s->thread)) return;
    atomic_store_explicit(&s->stop_flag, 1, memory_order_relaxed);
    os_thread_join(&s->thread);
}

static void null_close(bwa_sink* base) {
    NullSink* s = (NullSink*)base;
    null_stop(base);
    free(s->bus);
    free(s);
}

static const char* null_backend(bwa_sink* base) { (void)base; return "null"; }
static uint32_t null_block_size(bwa_sink* base) { return ((NullSink*)base)->block_size; }

/* The null sink has no DAC, so output_latency stays absent (0 = unknown) — but it DOES have a
 * thread with a deadline, so its health is real: late blocks are genuine overruns, and the dropout
 * path is the same helper the ASIO sink uses, driven by the injection hook. measured = true. */
static void null_health(bwa_sink* base, bwa_sink_health* out) {
    NullSink* s = (NullSink*)base;
    out->blocks         = atomic_load_explicit(&s->h_blocks, memory_order_relaxed);
    out->dropouts       = atomic_load_explicit(&s->h_dropouts, memory_order_relaxed);
    out->dropped_frames = atomic_load_explicit(&s->h_dropped_frames, memory_order_relaxed);
    out->driver_resyncs = 0;                    /* no driver to report one */
    out->late_blocks    = atomic_load_explicit(&s->h_late, memory_order_relaxed);
    out->render_ns_peak = sink_peak_recent(&s->h_peak);
    out->period_ns      = s->sample_rate
            ? (uint64_t)s->block_size * 1000000000ull / (uint64_t)s->sample_rate : 0;
    out->device_lost    = 0;                    /* no device to lose */
    out->measured       = true;
}

static const bwa_sink_vtbl NULL_VT = {   /* designated: stop/close share a signature, so a positional swap would be silent */
    .type = BWA_SINK_NULL,
    .start = null_start, .stop = null_stop, .close = null_close,
    .backend = null_backend, .block_size = null_block_size,
    .health = null_health,
};

bwa_sink* bwa_null_sink_open(uint32_t sample_rate, uint32_t block_size, uint32_t channels,
                          bwa_render_fn render, void* user, char* err, size_t errcap) {
    if (!render || channels == 0 || block_size == 0 || sample_rate == 0) {
        null_set_err(err, errcap, "null_sink: bad arguments");
        return NULL;
    }
    NullSink* s = (NullSink*)calloc(1, sizeof *s);
    if (!s) { null_set_err(err, errcap, "null_sink: out of memory"); return NULL; }
    s->base.vt     = &NULL_VT;
    s->sample_rate = sample_rate;
    s->block_size  = block_size;
    s->channels    = channels;
    s->render      = render;
    s->user        = user;
    s->bus = (float*)calloc((size_t)block_size * channels, sizeof(float));
    if (!s->bus) {
        free(s);
        null_set_err(err, errcap, "null_sink: bus alloc failed");
        return NULL;
    }
    return &s->base;
}
