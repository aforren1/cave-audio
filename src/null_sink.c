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
    BwSink       base;
    uint32_t     sample_rate, block_size, channels;
    BwRenderFn   render;
    void*        user;
    float*       bus;            /* planar channels * block_size */
    HANDLE       thread;
    volatile LONG stop_flag;
} NullSink;

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
    BW_THREAD_NAME("bw-audio (null)");       /* same render() the ASIO callback drives — profile w/o hardware */
    LARGE_INTEGER lf; QueryPerformanceFrequency(&lf);
    const uint64_t freq = (uint64_t)lf.QuadPart;
    timeBeginPeriod(1);                      /* ~1ms Sleep granularity */

    const uint64_t base   = qpc_now();
    const double block_ns = 1.0e9 * (double)s->block_size / (double)s->sample_rate;
    uint64_t sample_pos = 0, block_index = 0;

    while (!s->stop_flag) {
        BwTimestamp ts = {
            .sample_pos     = sample_pos,
            .system_time_ns = ticks_to_ns(qpc_now() - base, freq),
        };
        BW_ZONE_BEGIN(zb, "null block");
        s->render(s->user, s->bus, s->block_size, &ts);   /* engine produces a block */
        /* null sink: the rendered bus is intentionally discarded. */
        BW_ZONE_END(zb);
        BW_FRAME_MARK();

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

static int null_start(BwSink* base) {
    NullSink* s = (NullSink*)base;
    if (s->thread) return 0;                 /* already running */
    InterlockedExchange(&s->stop_flag, 0);
    s->thread = CreateThread(NULL, 0, null_thread, s, 0, NULL);
    return s->thread ? 0 : 1;
}

static void null_stop(BwSink* base) {
    NullSink* s = (NullSink*)base;
    if (!s->thread) return;
    InterlockedExchange(&s->stop_flag, 1);
    WaitForSingleObject(s->thread, INFINITE);
    CloseHandle(s->thread);
    s->thread = NULL;
}

static void null_close(BwSink* base) {
    NullSink* s = (NullSink*)base;
    null_stop(base);
    free(s->bus);
    free(s);
}

static const char* null_backend(BwSink* base) { (void)base; return "null"; }
static uint32_t null_block_size(BwSink* base) { return ((NullSink*)base)->block_size; }

static const BwSinkVtbl NULL_VT = { null_start, null_stop, null_close, null_backend, null_block_size };

BwSink* bw_null_sink_open(uint32_t sample_rate, uint32_t block_size, uint32_t channels,
                          BwRenderFn render, void* user, char* err, size_t errcap) {
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
