/*
 * manual_sink.c — offline/deterministic sink. Unlike null_sink.c (a thread paced from the wall
 * clock), the manual sink spawns NO thread: the caller drives it one block at a time via
 * bwa_render_block -> bwa_sink_render_block. The engine's render() runs synchronously on the caller's
 * thread, and the timestamp is derived from a running sample counter (plus the nominal block time),
 * NOT QueryPerformanceCounter — so a fixed input renders to bit-identical output every run. That
 * reproducibility is what makes end-to-end golden-audio tests possible off-hardware.
 *
 * Only the SYNCHRONOUS DSP is deterministic this way. The Steam sim threads (occlusion/reflection/
 * pathing) still run on their own wall-clock timers, so a golden render must avoid them (or use the
 * manual occlusion path) — see docs/api.md.
 */
#include "sink.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    bwa_sink      base;
    uint32_t      sample_rate, block_size, channels;
    bwa_render_fn render;
    void*         user;
    float*        bus;           /* planar channels * block_size */
    uint64_t      sample_pos;    /* deterministic clock: advances by block_size each render */
} ManualSink;

static int         manual_start(bwa_sink* b)      { (void)b; return 0; }   /* no thread; the caller pumps */
static void        manual_stop(bwa_sink* b)       { (void)b; }
static void        manual_close(bwa_sink* b)      { ManualSink* s = (ManualSink*)b; free(s->bus); free(s); }
static const char* manual_backend(bwa_sink* b)    { (void)b; return "manual"; }
static uint32_t    manual_block_size(bwa_sink* b) { return ((ManualSink*)b)->block_size; }

static const float* manual_render_block(bwa_sink* b, uint32_t* channels, uint32_t* nframes) {
    ManualSink* s = (ManualSink*)b;
    /* Deterministic timestamp: sample_pos is exact; system_time_ns is the nominal block time derived
     * from it (no wall clock), so nothing downstream reads a run-varying value. Split the ns math the
     * way null_sink does — pos*1e9 would overflow uint64 after ~100 h of rendered audio. */
    bwa_timestamp ts = {
        .sample_pos     = s->sample_pos,
        .system_time_ns = (s->sample_pos / s->sample_rate) * 1000000000ull
                        + (s->sample_pos % s->sample_rate) * 1000000000ull / s->sample_rate,
    };
    s->render(s->user, s->bus, s->block_size, &ts);   /* the engine fully writes the bus (rt_render zero-inits) */
    s->sample_pos += s->block_size;
    if (channels) *channels = s->channels;
    if (nframes)  *nframes  = s->block_size;
    return s->bus;
}

static const bwa_sink_vtbl MANUAL_VT = {   /* designated: render_block is the manual-only extension */
    .start = manual_start, .stop = manual_stop, .close = manual_close,
    .backend = manual_backend, .block_size = manual_block_size, .render_block = manual_render_block,
};

static void manual_set_err(char* err, size_t cap, const char* msg) {
    if (err && cap) { strncpy(err, msg, cap - 1); err[cap - 1] = 0; }
}

bwa_sink* bwa_manual_sink_open(uint32_t sample_rate, uint32_t block_size, uint32_t channels,
                            bwa_render_fn render, void* user, char* err, size_t errcap) {
    if (!render || channels == 0 || block_size == 0 || sample_rate == 0) {
        manual_set_err(err, errcap, "manual_sink: bad arguments");
        return NULL;
    }
    ManualSink* s = (ManualSink*)calloc(1, sizeof *s);
    if (!s) { manual_set_err(err, errcap, "manual_sink: out of memory"); return NULL; }
    s->base.vt     = &MANUAL_VT;
    s->sample_rate = sample_rate;
    s->block_size  = block_size;
    s->channels    = channels;
    s->render      = render;
    s->user        = user;
    s->bus = (float*)calloc((size_t)block_size * channels, sizeof(float));
    if (!s->bus) { free(s); manual_set_err(err, errcap, "manual_sink: bus alloc failed"); return NULL; }
    return &s->base;
}
