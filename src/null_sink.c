/*
 * null_sink.c — offline sink. A dedicated thread paces blocks from the
 * high-resolution clock at sample_rate/block_size and invokes the engine's render,
 * then discards the audio. No device, no Dante hardware. This is the desk/CI path
 * and the in-memory array render the `binaural` profile relies on.
 *
 * The render callback runs on this thread and must stay alloc/lock/syscall-free per
 * the audio-thread invariants; the pacing (Sleep) happens *outside* the callback.
 */
#include "sink.h"
#include "profile.h"

#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>        /* timeBeginPeriod/timeEndPeriod (link winmm) */

typedef struct {
    bwa_sink       base;
    uint32_t     sample_rate, block_size, channels;
    bwa_render_fn   render;
    void*        user;
    float*       bus;            /* planar channels * block_size */
    HANDLE       thread;
    volatile LONG stop_flag;

    /* Health counters (bwa_sink_health). Interlocked rather than stdatomic, the same choice pose.h
     * makes: it keeps this file off the /experimental:c11atomics list. Written on the render thread,
     * read from the control thread. */
    volatile LONG64 h_blocks, h_dropouts, h_dropped_frames, h_late, h_render_ns_peak;
} NullSink;

/* TEST HOOK — declared in sink.h (exported from the dll there), deliberately not in bw_audio.h:
 * lets a test observe the blocks this sink otherwise discards, i.e. see a profile's device-bound
 * output off-hardware (the smoke test uses it to pin binaural laterality through the REAL dll). */
void (*bwa_null_sink_tap)(const float* bus, uint32_t channels, uint32_t block_size) = NULL;

/* TEST HOOK — declared in sink.h, deliberately not in bw_audio.h. Set it to N and the next block
 * advances the reported device position by N EXTRA blocks, exactly as a device that clocked out
 * audio while we were not there to render it. It is the only way to exercise the dropout accounting
 * off-hardware: a real missed deadline needs a real device, but the arithmetic that turns a position
 * jump into a count is ordinary code and must not be shipped untested. Consumed once (reset to 0). */
volatile LONG bwa_null_sink_skip_blocks = 0;

static uint64_t qpc_now(void) {
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (uint64_t)c.QuadPart;
}
static uint64_t ticks_to_ns(uint64_t ticks, uint64_t freq) {
    /* Split the scale so ticks*1e9 cannot overflow uint64 — the naive form wraps after
     * only ~30 min at a 10 MHz QPC. This stays exact for centuries of runtime. */
    return (ticks / freq) * 1000000000ull + (ticks % freq) * 1000000000ull / freq;
}

static void null_set_err(char* err, size_t cap, const char* msg) {
    if (err && cap) { strncpy(err, msg, cap - 1); err[cap - 1] = 0; }
}

static DWORD WINAPI null_thread(LPVOID arg) {
    NullSink* s = (NullSink*)arg;
    BWA_THREAD_NAME("bw-audio (null)");       /* same render() the ASIO callback drives — profile w/o hardware */
    LARGE_INTEGER lf; QueryPerformanceFrequency(&lf);
    const uint64_t freq = (uint64_t)lf.QuadPart;
    timeBeginPeriod(1);                      /* ~1ms Sleep granularity */

    const uint64_t base   = qpc_now();
    const double block_ns = 1.0e9 * (double)s->block_size / (double)s->sample_rate;
    const uint64_t budget_ns = (uint64_t)s->block_size * 1000000000ull / (uint64_t)s->sample_rate;
    uint64_t sample_pos = 0, block_index = 0, predicted_pos = 0;

    while (!s->stop_flag) {
        /* The injected skip (bwa_null_sink_skip_blocks) advances the reported position without
         * rendering those blocks — what a starved device looks like from in here. */
        const LONG skip = InterlockedExchange(&bwa_null_sink_skip_blocks, 0);
        if (skip > 0) sample_pos += (uint64_t)skip * s->block_size;

        /* Same comparison the ASIO sink makes, through the same helper: where the last block said
         * this one would land, versus where it did. */
        if (predicted_pos) {
            const uint64_t lost = sink_position_gap(predicted_pos, sample_pos, s->block_size);
            if (lost) {
                InterlockedIncrement64(&s->h_dropouts);
                InterlockedExchangeAdd64(&s->h_dropped_frames, (LONG64)lost);
            }
        }
        predicted_pos = sample_pos + s->block_size;

        bwa_timestamp ts = {
            .sample_pos     = sample_pos,
            .system_time_ns = ticks_to_ns(qpc_now() - base, freq),
        };
        BWA_ZONE_BEGIN(zb, "null block");
        const uint64_t t0 = qpc_now();
        s->render(s->user, s->bus, s->block_size, &ts);   /* engine produces a block */
        const uint64_t render_ns = ticks_to_ns(qpc_now() - t0, freq);
        if (bwa_null_sink_tap) bwa_null_sink_tap(s->bus, s->channels, s->block_size);
        /* null sink: the rendered bus is intentionally discarded. */
        BWA_ZONE_END(zb);
        BWA_FRAME_MARK();

        /* Overrunning the block period is real here, not simulated: this thread has a deadline and
         * the pacing loop below is what enforces it. On a device it is what eventually becomes a
         * dropout, so counting it off-hardware is the honest half of the measurement CI can do. */
        if (render_ns > budget_ns) InterlockedIncrement64(&s->h_late);
        for (;;) {
            const LONG64 peak = s->h_render_ns_peak;
            if ((LONG64)render_ns <= peak) break;
            if (InterlockedCompareExchange64(&s->h_render_ns_peak, (LONG64)render_ns, peak) == peak) break;
        }
        InterlockedIncrement64(&s->h_blocks);

        sample_pos  += s->block_size;
        block_index += 1;

        /* pace to the next block deadline; stay responsive to stop */
        const double target_ns = (double)block_index * block_ns;
        for (;;) {
            if (s->stop_flag) break;
            const double elapsed_ns = (double)ticks_to_ns(qpc_now() - base, freq);
            const double remain_ms  = (target_ns - elapsed_ns) / 1.0e6;
            if (remain_ms <= 0.3) break;
            Sleep(remain_ms > 2.0 ? (DWORD)(remain_ms - 1.0) : 0);
        }
    }

    timeEndPeriod(1);
    return 0;
}

static int null_start(bwa_sink* base) {
    NullSink* s = (NullSink*)base;
    if (s->thread) return 0;                 /* already running */
    InterlockedExchange(&s->stop_flag, 0);
    s->thread = CreateThread(NULL, 0, null_thread, s, 0, NULL);
    return s->thread ? 0 : 1;
}

static void null_stop(bwa_sink* base) {
    NullSink* s = (NullSink*)base;
    if (!s->thread) return;
    InterlockedExchange(&s->stop_flag, 1);
    WaitForSingleObject(s->thread, INFINITE);
    CloseHandle(s->thread);
    s->thread = NULL;
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
    out->blocks         = (uint64_t)s->h_blocks;
    out->dropouts       = (uint64_t)s->h_dropouts;
    out->dropped_frames = (uint64_t)s->h_dropped_frames;
    out->driver_resyncs = 0;                    /* no driver to report one */
    out->late_blocks    = (uint64_t)s->h_late;
    out->render_ns_peak = (uint64_t)s->h_render_ns_peak;
    out->period_ns      = s->sample_rate
            ? (uint64_t)s->block_size * 1000000000ull / (uint64_t)s->sample_rate : 0;
    out->measured       = true;
}

static const bwa_sink_vtbl NULL_VT = {   /* designated: stop/close share a signature, so a positional swap would be silent */
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
