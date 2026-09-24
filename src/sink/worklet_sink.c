/*
 * worklet_sink.c - the Wasm Audio Worklet backend (docs/backends.md, docs/web.md).
 *
 * WHY IT EXISTS. A browser page has exactly one audio output API, and the only part of it that
 * runs on the audio rendering thread is an AudioWorklet processor. Emscripten's Wasm Audio
 * Worklets put OUR wasm module in that processor's global scope, against the SAME shared
 * WebAssembly.Memory the rest of the engine runs on, so the processor's process() callback IS the
 * audio thread in the sense concurrency.md means: it reads the voice table and the commit snapshot
 * out of the same linear memory the control thread wrote them into. That is what makes the web
 * target the engine rather than a second engine that resembles it.
 *
 * THE SHAPE. An AudioWorklet's process() gets a fixed 128-frame render quantum and the engine's
 * block is 256, so everything goes through the fixed-quantum adapter like every backend but ALSA.
 * The adapter is the easiest customer it will ever have: 128 divides every sensible block size, so
 * one rendered block feeds exactly block/128 callbacks with no partial slot. The output buffer is
 * PLANAR FLOAT already (AudioSampleFrame.data is data[channel * samplesPerChannel + i]), which is
 * the bus layout, so the adapter's runs copy straight out - no convert, no interleave, the same
 * property jack_sink.c has.
 *
 * RENDER AHEAD WAS TRIED AND REJECTED (2026-09-24), and this is the note that should stop anyone
 * rebuilding it. Rendering a 256-frame block inside a 128-frame callback puts 5.3 ms of work inside
 * a 2.7 ms deadline, and a Galaxy XR headset chopped. The obvious fix is a render thread that fills
 * a ring while process() only copies, and it was built, tested and measured. It lost: the render
 * thread is a Worker and a Worker has NO priority, while this callback's thread is the one thread a
 * page owns that the OS treats as audio. On a busy laptop the render thread was descheduled
 * mid-render for 40 to 200 ms and the ring ran dry where this shape kept playing; a rescue render
 * from process() could not help, because the render thread held the (non-reentrant) render at
 * exactly those moments. Paired runs are in docs/web.md. The slack an overrun needs has to come from
 * the BROWSER'S output buffer, which the priority thread already owns: that is what
 * BWA_SINK_FLAG_DEEP_BUFFER asks for (a "playback" latencyHint, below).
 *
 * STEREO ONLY, said at the open rather than discovered as silence. A page can build a wide
 * AudioContext, but none of the array profiles has a transport here and a 26-channel browser
 * output is a speaker set nobody has. AUTO sends anything wider straight to the offline sink, the
 * way Android's AAudio sink does.
 *
 * EVERY webaudio.h CALL RUNS ON THE BROWSER MAIN THREAD, and this is not a style choice. Two facts
 * force it: `new AudioContext()` needs a Window, which a Worker global scope is not; and
 * Emscripten's handle table (`emAudio` in libwebaudio.js) is an ordinary JS variable, so it is
 * PER-THREAD - a handle minted on the main thread means nothing in a Worker. The control thread is
 * a Worker in the shipping shape (docs/web.md), so open/start/stop/close proxy their Web Audio
 * work to the main runtime thread with emscripten_proxy_sync and call it directly when they are
 * already there. Nothing in the render path proxies anything.
 *
 * THE SUSPENDED CONTEXT, and the one genuinely new thing on this platform. A browser starts an
 * AudioContext SUSPENDED and only a user gesture may resume it. A suspended context renders no
 * quanta at all, so process() is never called, so nothing pulls the adapter and the engine's dsp
 * clock, playheads and fades would simply stop - which is not a device fault and must not read as
 * one. Rule 4 already has the answer for a device that went away: host-pace silence so the clocks
 * keep advancing, set device_lost, and let the control thread decide. This sink does that for the
 * suspended case too, and it is the NORMAL case here: every page is suspended until the button is
 * pressed. The handoff between the host-paced thread and the worklet is the interesting part; see
 * "THE PULL GATE" below.
 *
 * WHAT IT MEASURES, AND ONLY WHERE THE BROWSER SAYS. Web Audio exposes no xrun counter and no device
 * position to the worklet: `currentFrame` advances by exactly one quantum per process() call
 * whether or not the output starved, and a browser renders several quanta back to back inside one
 * system audio callback, so a per-callback host-interval rule would false-positive on every batch.
 * What Chromium does expose is AudioContext.playbackStats (underrunDuration, underrunEvents): the
 * browser's own count of the silence its output played because a render came back late. That is a
 * real device-dropout figure, so worklet_health reads it on the CONTROL thread (a main-thread JS
 * read, never the audio thread) and books it as dropouts / dropped_frames, and `measured` is TRUE
 * exactly when the browser has it. On a browser without it, `measured` stays false and a zero means
 * "cannot know". late_blocks and render_ns_peak are the adapter's, on a 1 ms clock (see below).
 * bindings/web/xr/slip.js reads AudioContext.currentTime against performance.now on the main thread
 * as a second, cruder signal for browsers without playbackStats.
 *
 * WHY sample_pos IS NOT currentFrame. Rule 3 defines sample_pos as the frames the sink handed to
 * the device before this block, counted from the sink's start, which is exactly what the adapter
 * already counts. While the node is connected, `currentFrame - (currentFrame at our first
 * callback)` IS that number, by construction: one process() call per quantum. Reading it would
 * cost a wasm-to-JS transition per quantum for a value we hold, so the sink does not read it. The
 * pair's host half is os_monotonic_ns through sink_quant_now_ns, like every other backend - with
 * one platform caveat worth writing down, because it is invisible: AudioWorkletGlobalScope has no
 * `performance`, so emscripten's CLOCK_MONOTONIC returns ENOSYS on this thread, and until
 * 2026-09-23 os_monotonic_ns returned uninitialized stack here (measured: a constant, so every
 * render time read 0 except the first, which read a second and pinned peak_load at 401). It now
 * falls back to Date.now: milliseconds, not monotonic, same epoch as the control thread's
 * performance.timeOrigin + performance.now and a thousand times coarser. So on this backend a
 * render time is a whole number of milliseconds, late_blocks can be off by a block either way near
 * the budget, and the adapter's two backward-step rules (the stamp never steps back, a render time
 * that would go negative books 0) are load-bearing rather than defensive.
 *
 * AUDIO THREAD. worklet_process is the AudioWorklet's own rendering thread. Everything it touches
 * is allocated at open: no malloc, no JS call, no console, no proxying.
 */
#include "sink/sink.h"
#include "sink/sink_convert.h"
#include "sink/sink_quant.h"
#include "os/os.h"
#include "core/profile.h"

#include <emscripten/em_asm.h>
#include <emscripten/threading.h>
#include <emscripten/proxying.h>
#include <emscripten/webaudio.h>

#include <malloc.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The AudioWorklet thread's own stack. The engine renders on it, and the shadow-stack wall
 * docs/web.md bisected (64 KB and 128 KB fault, 256 KB and up survive rt_create) applies to this
 * thread exactly as it applies to a pthread - it is the same shadow stack, just allocated by us
 * instead of by the toolchain. 1 MB is what -sSTACK_SIZE and -sDEFAULT_PTHREAD_STACK_SIZE already
 * ask for elsewhere, and it costs address space only. Must be 16-byte aligned and a multiple of 16.
 */
#define WORKLET_STACK_BYTES 1048576u

/* How long the worklet may go quiet before the host-paced thread takes over the pacing, in block
 * periods. A browser renders quanta in batches, so consecutive process() calls are not evenly
 * spaced; 8 blocks is far longer than any batch and far shorter than a human noticing the clock
 * stall on a suspended context. */
#define WORKLET_QUIET_BLOCKS 8u

/* ...and never less than this many browser callbacks. A deep output buffer (latencyHint "playback"
 * or 0.05) makes the browser render its quanta in BURSTS as long as that buffer, 50 ms of quanta
 * back to back and then nothing, and eight 256-frame blocks is only 43 ms. Found when the XR page
 * moved to a 0.05 hint: the heartbeat went quiet inside every burst gap, the host-paced thread took
 * over, device_lost flapped, and two pullers alternated on one adapter. The burst is baseLatency,
 * read at open, the same figure rule 5 reports. */
#define WORKLET_QUIET_BURSTS 3u

/* The setup chain's state, readable from the control thread. It is async and its failures land
 * after open() has returned, so there has to be somewhere to put them. */
enum { WK_IDLE = 0, WK_STARTING = 1, WK_LIVE = 2, WK_FAILED = 3 };

typedef struct {
    bwa_sink base;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t block;             /* the engine quantum; block_size() reports this          */
    uint32_t quantum;           /* the context's render quantum, 128 in Web Audio 1.0     */
    uint32_t q_capacity;        /* frames the adapter can serve in one pull               */
    uint32_t latency_frames;    /* rule 5, latched at open; the header promises constant  */
    uint32_t quiet_blocks;      /* heartbeat silence, in blocks, before the host thread paces */
    bool     owns_ctx;          /* false when the page handed us its own AudioContext     */

    EMSCRIPTEN_WEBAUDIO_T ctx;
    EMSCRIPTEN_WEBAUDIO_T node;
    void*     wk_stack;
    SinkQuant quant;

    /* --- written by the worklet and the host-paced thread, read from the control thread.
     * Relaxed where they are monotonic counts or one-way flags nobody synchronizes ON. --- */
    _Atomic int      running;      /* start() opens the gate, stop() closes it            */
    _Atomic int      closing;      /* process() zeroes, acknowledges and returns false    */
    _Atomic int      state;        /* WK_*: how far the async setup chain got             */
    _Atomic uint64_t ticks;        /* one per process() call: the worklet's heartbeat     */
    _Atomic uint32_t host_paced;   /* 1 while the host-paced thread is doing the pacing   */

    /* THE PULL GATE. The worklet and the host-paced fallback both pull the same SinkQuant, and
     * sink_quant is single-threaded by contract. This is a wait-free try-lock and NOT a mutex:
     * whoever finds it taken BAILS immediately (the worklet writes one quantum of silence, the
     * host thread skips one period) rather than waiting, so the audio thread never blocks and
     * invariant 1 holds. It is contended only in the single handoff quantum when a context
     * resumes or suspends, which is why bailing is the right answer rather than a cost. */
    _Atomic int      pull_gate;

    os_thread   host_thread;
    _Atomic int host_started, host_stop;

    char name[96];              /* "worklet:default" or "worklet:<context handle>", ASCII */
} WorkletSink;

static void wk_set_err(char* err, size_t cap, const char* msg) {
    if (err && cap) { strncpy(err, msg, cap - 1); err[cap - 1] = 0; }
}

/* ---- running a step on the browser main thread ---------------------------------------------- */

/* Control thread only. Direct when we are already the main runtime thread (a page that loaded the
 * module on the main thread and drives the engine from there), proxied otherwise (a control
 * thread on a Worker). emscripten_proxy_sync blocks until the main thread's event loop runs it.
 *
 * WHO WAITS ON THE MAIN THREAD, audited 2026-09-24 after a headset froze with its window (cause
 * still unknown). Every caller is on the CONTROL thread: the open (create or adopt the context, and
 * teardown on a failed open), start (the setup chain's first step), close (destroy the node and an
 * owned context), health (the playbackStats read) and sink_worklet_context_state (no caller). In
 * the one topology this sink opens in ("main", bindings/web/src/client.js) the control thread IS
 * the main thread, so none of them proxies at all. process() and the host-paced thread call
 * nothing proxied. What main waits for in return: close waits at most ~200 ms for the worklet
 * heartbeat to go quiet (the worklet never waits on main) and joins the host-paced thread, whose
 * only wait is an absolute-deadline sleep. */
static bool wk_on_main(void (*fn)(void*), void* arg) {
    if (emscripten_is_main_runtime_thread()) { fn(arg); return true; }
    return emscripten_proxy_sync(emscripten_proxy_get_system_queue(),
                                 emscripten_main_runtime_thread_id(), fn, arg);
}

/* ---- the process callback (the audio thread) ------------------------------------------------ */

typedef struct { float* dst; uint32_t channels; uint32_t frames; } WorkletOut;

/* One contiguous run of the adapter's FIFO into the worklet's output. Both sides are planar
 * float, so this is sink_convert_planar's memcpy per channel and nothing else - the same
 * no-conversion path jack_sink.c takes. Float to float carries no NaN rule: the shared header's
 * rule is a float-to-INT one, and the engine-side guard against a NaN reaching a device is the
 * limiter. */
static void worklet_out(void* user, const float* planar, uint32_t stride, uint32_t frame_offset,
                        uint32_t n) {
    WorkletOut* o = (WorkletOut*)user;
    for (uint32_t c = 0; c < o->channels; ++c)
        sink_convert_planar(o->dst + (size_t)c * o->frames + frame_offset,
                            planar + (size_t)c * stride, n, SINK_FMT_F32);
}

static void worklet_discard_out(void* u, const float* p, uint32_t s, uint32_t o, uint32_t n) {
    (void)u; (void)p; (void)s; (void)o; (void)n;
}

static bool worklet_process(int numInputs, const AudioSampleFrame* inputs, int numOutputs,
                            AudioSampleFrame* outputs, int numParams, const AudioParamFrame* params,
                            void* user) {
    (void)numInputs; (void)inputs; (void)numParams; (void)params;
    WorkletSink* s = (WorkletSink*)user;

    /* The heartbeat, first thing and unconditionally: it is what tells the host-paced thread to
     * stand down, and it has to be published even on the paths below that write silence. */
    atomic_fetch_add_explicit(&s->ticks, 1u, memory_order_release);

    if (numOutputs < 1 || !outputs[0].data) return true;
    float* out          = outputs[0].data;
    const uint32_t ch   = (uint32_t)outputs[0].numberOfChannels;
    const uint32_t n    = (uint32_t)outputs[0].samplesPerChannel;
    const size_t   all  = (size_t)ch * (size_t)n;

    /* Rule 8's browser spelling. Returning false tells the browser this processor is done and it
     * stops calling us, which is the only "the callback has returned and no further one can start"
     * this API offers - there is no worklet thread to join. close() waits for the heartbeat to go
     * quiet before it frees anything; see worklet_close. */
    if (atomic_load_explicit(&s->closing, memory_order_acquire)) {
        memset(out, 0, all * sizeof(float));
        return false;
    }

    if (!atomic_load_explicit(&s->running, memory_order_acquire)
        || ch != s->channels || n == 0 || n > s->q_capacity) {
        memset(out, 0, all * sizeof(float));
        return true;
    }

    /* The gate, not a lock: taken or bail. See the field's comment. */
    if (atomic_exchange_explicit(&s->pull_gate, 1, memory_order_acquire)) {
        memset(out, 0, all * sizeof(float));
        return true;
    }

    WorkletOut o = { out, ch, n };
    BWA_ZONE_BEGIN(zb, "worklet quantum");
    /* device_pos_valid is false and the position argument is unused: Web Audio reports no device
     * position this sink could compare against (see the file header). */
    sink_quant_pull(&s->quant, n, 0, false, sink_quant_now_ns(), worklet_out, &o);
    BWA_ZONE_END(zb);

    atomic_store_explicit(&s->pull_gate, 0, memory_order_release);
    return true;
}

/* ---- the host-paced fallback (rule 4, and the suspended context) ---------------------------- */

/* Runs for the sink's whole started life, and pulls ONLY while the worklet is quiet. Two cases
 * reach it and they are the same code: a context that has never been resumed (the normal case on
 * a page before the user presses the button), and a context that was suspended again or lost. The
 * engine's dsp clock, playheads and fades keep advancing on discarded silence either way. */
static void worklet_host_thread(void* arg) {
    WorkletSink* s = (WorkletSink*)arg;
    BWA_THREAD_NAME("bw-audio (worklet host-paced)");
    const uint64_t block_ns = (uint64_t)s->block * 1000000000ull / (uint64_t)s->sample_rate;
    /* Asked for and its refusal tolerated, like AAudio's: a browser gives a Worker no priority
     * knob at all, so os_thread_set_realtime returns ENOTSUP here (docs/web.md, the OS shim). The
     * loop is pacing silence, so that costs nothing. */
    const bool rt = (os_thread_set_realtime(block_ns) == 0);

    uint64_t next_ns = 0;
    uint64_t last_tick = atomic_load_explicit(&s->ticks, memory_order_acquire);
    uint64_t quiet = 0;                       /* consecutive periods with no worklet callback */

    while (!atomic_load_explicit(&s->host_stop, memory_order_relaxed)) {
        const uint64_t now = sink_quant_now_ns();
        if (next_ns == 0 || now > next_ns + block_ns * 8ull) next_ns = now;

        const uint64_t tick = atomic_load_explicit(&s->ticks, memory_order_acquire);
        if (tick != last_tick) { last_tick = tick; quiet = 0; }
        else if (quiet < s->quiet_blocks) { quiet++; }

        const bool take_over = (quiet >= s->quiet_blocks);
        atomic_store_explicit(&s->host_paced, take_over ? 1u : 0u, memory_order_relaxed);

        if (take_over && atomic_load_explicit(&s->running, memory_order_relaxed)
            && !atomic_load_explicit(&s->closing, memory_order_relaxed)) {
            /* Same gate the worklet takes, same bail rule: if the worklet just woke up and is
             * mid-pull, skip this period rather than race it. */
            if (atomic_exchange_explicit(&s->pull_gate, 1, memory_order_acquire) == 0) {
                sink_quant_pull(&s->quant, s->block, 0, false, now, worklet_discard_out, NULL);
                atomic_store_explicit(&s->pull_gate, 0, memory_order_release);
            }
        }

        next_ns += block_ns;
        os_sleep_until_ns(next_ns);           /* absolute deadline: a relative sleep drifts */
    }
    if (rt) os_thread_clear_realtime();
}

/* ---- the async setup chain (browser main thread) -------------------------------------------- */

/* Step 3, and the only synchronous link in the chain: the node constructor returns its handle.
 * Connect it to the context's destination and the browser starts calling process() as soon as the
 * context is running. */
static void worklet_processor_created(EMSCRIPTEN_WEBAUDIO_T ctx, bool ok, void* user) {
    WorkletSink* s = (WorkletSink*)user;
    if (!ok) { atomic_store_explicit(&s->state, WK_FAILED, memory_order_release); return; }
    int out_channels[1] = { (int)s->channels };
    EmscriptenAudioWorkletNodeCreateOptions opts = {
        .numberOfInputs = 0,
        .numberOfOutputs = 1,
        .outputChannelCounts = out_channels,
    };
    const EMSCRIPTEN_WEBAUDIO_T node =
        emscripten_create_wasm_audio_worklet_node(ctx, "bw-audio", &opts, worklet_process, s);
    if (!node) { atomic_store_explicit(&s->state, WK_FAILED, memory_order_release); return; }
    s->node = node;
    emscripten_audio_node_connect(node, ctx, 0, 0);
    /* Published LAST, after the node is in the graph: a control thread that reads WK_LIVE has to
     * be able to conclude that a running context is now feeding the adapter. */
    atomic_store_explicit(&s->state, WK_LIVE, memory_order_release);
}

static void worklet_thread_started(EMSCRIPTEN_WEBAUDIO_T ctx, bool ok, void* user) {
    WorkletSink* s = (WorkletSink*)user;
    if (!ok) { atomic_store_explicit(&s->state, WK_FAILED, memory_order_release); return; }
    /* The processor NAME is the string the browser registers in the AudioWorkletGlobalScope, and
     * it has to match the name the node asks for. ASCII, per the repo rule. */
    WebAudioWorkletProcessorCreateOptions opts = { .name = "bw-audio", .numAudioParams = 0,
                                                   .audioParamDescriptors = NULL };
    emscripten_create_wasm_audio_worklet_processor_async(ctx, &opts, worklet_processor_created, s);
}

/* Step 1 of the chain, run on the main thread by start(). Allocating the worklet thread's stack
 * here rather than at open keeps a sink that is never started from holding a megabyte. */
static void worklet_begin_setup(void* arg) {
    WorkletSink* s = (WorkletSink*)arg;
    if (atomic_load_explicit(&s->state, memory_order_acquire) != WK_IDLE) return;
    if (!s->wk_stack) s->wk_stack = memalign(16, WORKLET_STACK_BYTES);
    if (!s->wk_stack) { atomic_store_explicit(&s->state, WK_FAILED, memory_order_release); return; }
    atomic_store_explicit(&s->state, WK_STARTING, memory_order_release);
    emscripten_start_wasm_audio_worklet_thread_async(s->ctx, s->wk_stack, WORKLET_STACK_BYTES,
                                                     worklet_thread_started, s);
}

/* ---- open: the context step, on the main thread --------------------------------------------- */

typedef struct {
    uint32_t want_rate;
    uint32_t want_channels;
    int32_t  adopt;              /* >0: adopt this existing handle instead of creating one */
    bool     deep;               /* BWA_SINK_FLAG_DEEP_BUFFER: ask for the deeper output buffer */
    /* out */
    EMSCRIPTEN_WEBAUDIO_T ctx;
    bool     owns;
    uint32_t rate;
    uint32_t quantum;
    uint32_t latency_frames;
    uint32_t burst_frames;       /* baseLatency alone: how long a browser render burst can be */
    int      ctx_state;
} WkOpen;

static void worklet_open_context(void* arg) {
    WkOpen* o = (WkOpen*)arg;
    if (o->adopt > 0) {
        o->ctx  = (EMSCRIPTEN_WEBAUDIO_T)o->adopt;
        o->owns = false;
    } else {
        /* "interactive" is the latencyHint a game wants: the shortest buffer the browser will
         * give. DEEP_BUFFER asks for "playback" instead, the browser's deeper output buffer, which
         * is the slack a render that overruns its quantum needs, held by the thread that already
         * has audio priority (the file header says why that and not a ring). webaudio.h takes the
         * hint as a string, so the numeric form a page can pass (0.05) is not available here. The
         * rate is REQUESTED, and a browser honors it by resampling its own output if it has to -
         * which it does not report, so neither does this sink (see rule 6 below). */
        EmscriptenWebAudioCreateAttributes attr = {
            .latencyHint = o->deep ? "playback" : "interactive",
            .sampleRate = o->want_rate,
            .renderSizeHint = AUDIO_CONTEXT_RENDER_SIZE_DEFAULT,
        };
        o->ctx  = emscripten_create_audio_context(&attr);
        o->owns = true;
    }
    if (!o->ctx) return;
    const int r = emscripten_audio_context_sample_rate(o->ctx);
    const int q = emscripten_audio_context_quantum_size(o->ctx);
    o->rate      = (r > 0) ? (uint32_t)r : 0u;
    o->quantum   = (q > 0) ? (uint32_t)q : 128u;
    o->ctx_state = emscripten_audio_context_state(o->ctx);

    /* Rule 5's figure, and the only place in this file that runs JS of its own. webaudio.h has no
     * accessor for either latency, and both are plain numbers on the AudioContext:
     * baseLatency is the graph's own buffering and outputLatency the path to the speakers
     * (outputLatency is absent on some engines, hence the ?? 0). Main thread, at open, once -
     * never the audio thread. emscriptenGetAudioObject is libwebaudio.js's own handle lookup and
     * is guaranteed present, because emscripten_create_audio_context declares it a dependency. */
    const double lat_s = EM_ASM_DOUBLE({
        var c = emscriptenGetAudioObject($0);
        if (!c) return 0;
        return (c.baseLatency || 0) + (c.outputLatency || 0);
    }, (int)o->ctx);
    o->latency_frames = (lat_s > 0.0 && o->rate)
        ? (uint32_t)(lat_s * (double)o->rate + 0.5) : 0u;
    const double base_s = EM_ASM_DOUBLE({
        var c = emscriptenGetAudioObject($0);
        return c ? (c.baseLatency || 0) : 0;
    }, (int)o->ctx);
    o->burst_frames = (base_s > 0.0 && o->rate) ? (uint32_t)(base_s * (double)o->rate + 0.5) : 0u;
}

typedef struct { EMSCRIPTEN_WEBAUDIO_T ctx; EMSCRIPTEN_WEBAUDIO_T node; bool owns; } WkTeardown;

static void worklet_teardown_main(void* arg) {
    WkTeardown* t = (WkTeardown*)arg;
    if (t->node) emscripten_destroy_web_audio_node(t->node);
    /* A context the PAGE created is the page's to close: it may hold other nodes, and it is what
     * the page resumes from its gesture handler. Only a context this sink made is destroyed here. */
    if (t->ctx && t->owns) emscripten_destroy_audio_context(t->ctx);
}

typedef struct { EMSCRIPTEN_WEBAUDIO_T ctx; int state; } WkState;
static void worklet_read_state(void* arg) {
    WkState* w = (WkState*)arg;
    w->state = w->ctx ? emscripten_audio_context_state(w->ctx) : AUDIO_CONTEXT_STATE_CLOSED;
}

/* ---- vtable --------------------------------------------------------------------------------- */

static int worklet_start(bwa_sink* base) {
    WorkletSink* s = (WorkletSink*)base;
    atomic_store_explicit(&s->running, 1, memory_order_release);

    /* The host-paced thread first, and deliberately: the context is suspended until a user gesture
     * resumes it, so on a page this thread does ALL the pacing until the button is pressed. It
     * stands down on its own the moment the worklet's heartbeat moves. */
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&s->host_started, &expected, 1,
                                                memory_order_relaxed, memory_order_relaxed)) {
        if (os_thread_create(&s->host_thread, worklet_host_thread, s) != 0)
            atomic_store_explicit(&s->host_started, 2, memory_order_release);   /* 2 = never ran */
        else
            atomic_store_explicit(&s->host_started, 3, memory_order_release);   /* 3 = joinable  */
    }

    /* The setup chain is ASYNCHRONOUS and its failures land after this returns; sink_worklet_state
     * is where a control thread reads the outcome. Nothing here can fail the start, because a
     * start that returned an error for "the worklet thread has not finished spawning" would be
     * wrong on every page. */
    wk_on_main(worklet_begin_setup, s);
    return 0;
}

static void worklet_stop(bwa_sink* base) {
    WorkletSink* s = (WorkletSink*)base;
    atomic_store_explicit(&s->running, 0, memory_order_release);
}

static void worklet_close(bwa_sink* base) {
    WorkletSink* s = (WorkletSink*)base;
    worklet_stop(base);

    /* Rule 8, and the browser makes it awkward: there is no worklet thread to join and no API that
     * says "this processor has stopped". What there is: returning false from process() removes the
     * processor, and our own heartbeat. So set the flag, take the node out of the graph on the
     * main thread (which stops the browser scheduling it), then WAIT FOR THE HEARTBEAT TO GO
     * QUIET before freeing anything the callback touches. The wait is bounded, because a suspended
     * context never calls process() at all and would otherwise hang the close forever - and in
     * that case the node is already disconnected and destroyed, so nothing can start. */
    atomic_store_explicit(&s->closing, 1, memory_order_release);

    WkTeardown t = { s->ctx, s->node, s->owns_ctx };
    wk_on_main(worklet_teardown_main, &t);
    s->node = 0;

    {
        uint64_t last = atomic_load_explicit(&s->ticks, memory_order_acquire);
        uint64_t still = 0;
        for (int i = 0; i < 200 && still < 3; ++i) {     /* at most ~200 ms */
            os_sleep_ms(1);
            const uint64_t now = atomic_load_explicit(&s->ticks, memory_order_acquire);
            if (now == last) still++; else { last = now; still = 0; }
        }
    }

    /* 1 is the transient state INSIDE start(), between claiming the slot and publishing the
     * outcome - the same handshake aaudio_sink.c uses, and for the same reason: reading 1 and
     * moving on would either leak the thread or free the adapter under it. */
    int hs = atomic_load_explicit(&s->host_started, memory_order_acquire);
    while (hs == 1) { os_sleep_ms(1); hs = atomic_load_explicit(&s->host_started, memory_order_acquire); }
    if (hs == 3) {
        atomic_store_explicit(&s->host_stop, 1, memory_order_relaxed);
        os_thread_join(&s->host_thread);
    }

    sink_quant_free(&s->quant);
    free(s->wk_stack);
    free(s);
}

static const char* worklet_backend(bwa_sink* base)     { return ((WorkletSink*)base)->name; }
static uint32_t worklet_block_size(bwa_sink* base)     { return ((WorkletSink*)base)->block; }
/* CONSTANT for the life of the sink, as sink.h promises, which is why the adapter's contribution
 * is the worst case latched at open and not sink_quant_queued() read live: the FIFO holds between
 * 0 and one block depending on where in the quantum you ask, and a figure that breathes is worse
 * than one that is a little pessimistic. Same shape aaudio_sink.c uses. 0 stays "unknown". */
static uint32_t worklet_output_latency(bwa_sink* base) {
    return ((WorkletSink*)base)->latency_frames;
}

/* The browser's own output underruns, read on the main thread (health() runs on the control
 * thread, which is the main thread in the one topology this sink opens in; from a control Worker
 * wk_on_main proxies it). Cumulative since the context started, which is what bwa_health's counts
 * are too. `ok` is 0 when the browser has no playbackStats (every engine but Chromium's today). */
typedef struct { EMSCRIPTEN_WEBAUDIO_T ctx; uint32_t rate; int ok; double events; double frames; } WkStats;
static void worklet_read_stats(void* arg) {
    WkStats* w = (WkStats*)arg;
    /* Two scalar reads rather than a write into the heap: an EM_ASM that stores through HEAPF64
     * would need the growable-heap view refresh a -pthread + ALLOW_MEMORY_GROWTH build does not
     * promise inside EM_ASM. -1 means the browser has no playbackStats. */
    w->events = EM_ASM_DOUBLE({
        var c = emscriptenGetAudioObject($0);
        var s = c && c.playbackStats;
        return (s && typeof s.underrunEvents === "number") ? s.underrunEvents : -1;
    }, (int)w->ctx);
    if (w->events < 0.0) { w->ok = 0; return; }
    w->frames = EM_ASM_DOUBLE({
        var s = emscriptenGetAudioObject($0).playbackStats;
        return (s.underrunDuration || 0) * $1;
    }, (int)w->ctx, (int)w->rate);
    w->ok = 1;
}

/* late_blocks and render_ns_peak come from the adapter and are real, on a 1 ms clock (see the file
 * header). The DROPOUT half comes from the browser: AudioContext.playbackStats, where it exists,
 * is the silence the output really played, so it is booked as dropouts / dropped_frames and makes
 * `measured` true. Where it does not exist, `measured` stays false, because Web Audio gives this
 * sink no other way to see a dropout and a zero would be "could not know" dressed as a clean bill.
 * device_lost reports the host-paced state, which on this platform means either a suspended
 * context or a setup chain that failed - both cases where the sink is pacing silence so the
 * engine's clocks keep advancing. CONTROL THREAD: the stats read is a main-thread JS read and must
 * never move to the audio path. */
static void worklet_health(bwa_sink* base, bwa_sink_health* out) {
    WorkletSink* s = (WorkletSink*)base;
    sink_quant_health(&s->quant, out);
    out->measured    = false;
    out->device_lost = atomic_load_explicit(&s->host_paced, memory_order_relaxed);
    WkStats w = { s->ctx, s->sample_rate, 0, 0.0, 0.0 };
    if (s->ctx && wk_on_main(worklet_read_stats, &w) && w.ok) {
        out->dropouts       = (uint64_t)(w.events > 0.0 ? w.events : 0.0);
        out->dropped_frames = (uint64_t)(w.frames > 0.0 ? w.frames + 0.5 : 0.0);
        out->measured       = true;
    }
}

/* Internal readbacks (declared in sink.h, deliberately not in bw_audio.h). The setup chain is
 * async, so "did the worklet come up?" cannot be an open() return value, and a page needs the
 * AudioContext handle to resume the context from its gesture handler. Control thread. */
uint32_t sink_worklet_state(bwa_sink* base) {
    return (uint32_t)atomic_load_explicit(&((WorkletSink*)base)->state, memory_order_acquire);
}
int32_t sink_worklet_context(bwa_sink* base) { return (int32_t)((WorkletSink*)base)->ctx; }
uint32_t sink_worklet_quantum(bwa_sink* base) { return ((WorkletSink*)base)->quantum; }
uint32_t sink_worklet_context_state(bwa_sink* base) {
    WkState w = { ((WorkletSink*)base)->ctx, AUDIO_CONTEXT_STATE_CLOSED };
    wk_on_main(worklet_read_state, &w);
    return (uint32_t)w.state;
}

static const bwa_sink_vtbl WORKLET_VT = {   /* designated: stop/close share a signature, so a positional swap would be silent */
    .type = BWA_SINK_WORKLET,
    .start = worklet_start, .stop = worklet_stop, .close = worklet_close,
    .backend = worklet_backend, .block_size = worklet_block_size,
    .output_latency = worklet_output_latency,
    .health = worklet_health,
};

/* ---- device enumeration --------------------------------------------------------------------- */

/* Web Audio has no output device list a page can enumerate without permission, and an AudioContext
 * follows the browser's own default output whatever that is. So the backend reports exactly what
 * it can open - the default output - and `device` additionally accepts a decimal Emscripten
 * AudioContext HANDLE, the same shape AAudio's decimal Android device id takes. "0" is that
 * backend's spelling of "unspecified" and is what the one listed entry reports, so a picker that
 * persisted the id round-trips to the default output. */
uint32_t sink_worklet_device_count(void) { return 1; }

bool sink_worklet_device_name(uint32_t index, char* buf, uint32_t cap) {
    if (!buf || cap == 0) return false;
    buf[0] = 0;
    if (index != 0) return false;
    return sink_copy_device_name(buf, cap, "default");
}

bool sink_worklet_device_id(uint32_t index, char* buf, uint32_t cap) {
    if (!buf || cap == 0) return false;
    buf[0] = 0;
    if (index != 0) return false;
    return sink_copy_device_name(buf, cap, "0");
}

/* ---- open ------------------------------------------------------------------------------------ */

bwa_sink* bwa_worklet_sink_open(uint32_t sample_rate, uint32_t block_size, uint32_t channels,
                                const char* device, uint32_t flags, bool exact_rate,
                                bwa_render_fn render, void* user, char* err, size_t errcap) {
    /* One bit means something here, DEEP_BUFFER, and only when the sink creates the context: an
     * ADOPTED context's latencyHint was fixed by the page that made it, so the page must pass the
     * hint itself (bindings/web/README.md, and the XR page does). EXCLUSIVE and TIGHT_BUFFER have no
     * browser meaning, and rule 6 is handled below whatever EXACT_RATE says. */
    const bool deep = (flags & BWA_SINK_FLAG_DEEP_BUFFER) != 0;
    if (!render || channels == 0 || block_size == 0 || sample_rate == 0) {
        wk_set_err(err, errcap, "worklet: bad arguments");
        return NULL;
    }
    if (channels > 2) {
        char m[208];
        snprintf(m, sizeof m, "worklet: this backend is stereo only and was asked for %u channels; "
                              "a browser carries no array transport (see docs/web.md)", channels);
        wk_set_err(err, errcap, m);
        return NULL;
    }

    /* Rule 10 for a backend with no device list: `device` is a decimal AudioContext handle from
     * emscriptenRegisterAudioObject(new AudioContext(...)), which is how a page hands the engine
     * the context it created inside its own user-gesture handler and keeps the right to resume it.
     * A non-numeric string is a name meant for another backend. */
    int32_t adopt = 0;
    if (device && *device) {
        char* end = NULL;
        const long v = strtol(device, &end, 10);
        if (!end || *end || v < 0) {
            char m[240];
            snprintf(m, sizeof m, "worklet: device '%s' is not a decimal AudioContext handle; a "
                                  "browser has no enumerable output devices, so pass the handle "
                                  "emscriptenRegisterAudioObject returned (or NULL to let the sink "
                                  "create the context)", device);
            wk_set_err(err, errcap, m);
            return NULL;
        }
        adopt = (int32_t)v;
    }

    /* Designated, so a new field cannot silently shift the others. */
    WkOpen o = { .want_rate = sample_rate, .want_channels = channels, .adopt = adopt, .deep = deep,
                 .quantum = 128u, .ctx_state = AUDIO_CONTEXT_STATE_SUSPENDED };
    if (!wk_on_main(worklet_open_context, &o)) {
        wk_set_err(err, errcap, "worklet: the browser main thread could not be reached to create "
                                "the AudioContext; the page must keep its event loop running");
        return NULL;
    }
    if (!o.ctx) {
        wk_set_err(err, errcap, "worklet: no AudioContext; this browser has no Web Audio, or the "
                                "handle passed as the device names nothing");
        return NULL;
    }

    /* Rule 6. The engine has no resampler, and a worklet processor is called at the CONTEXT's
     * rate, so a context that did not land on the engine rate is a failure however it got there -
     * for the headphone policy as much as the array one. What this sink cannot do is the other
     * half of rule 6: a browser that is resampling its own output to the hardware rate reports
     * nothing about it, so there is no degradation to hand back. Create the engine at the rate a
     * page reads off its own AudioContext and the question does not arise. */
    if (o.rate && o.rate != sample_rate) {
        char m[248];
        snprintf(m, sizeof m, "worklet: the AudioContext runs at %u Hz, not the engine's %u Hz, and "
                              "the engine does not resample; create the engine at %u Hz",
                 o.rate, sample_rate, o.rate);
        wk_set_err(err, errcap, m);
        WkTeardown t = { o.ctx, 0, o.owns };
        wk_on_main(worklet_teardown_main, &t);
        return NULL;
    }
    (void)exact_rate;   /* both policies are the same one here: see above */

    WorkletSink* s = (WorkletSink*)calloc(1, sizeof *s);
    if (!s) {
        wk_set_err(err, errcap, "worklet: out of memory");
        WkTeardown t = { o.ctx, 0, o.owns };
        wk_on_main(worklet_teardown_main, &t);
        return NULL;
    }
    s->base.vt        = &WORKLET_VT;
    s->sample_rate    = sample_rate;
    s->channels       = channels;
    s->block          = block_size;
    s->quantum        = o.quantum;
    s->ctx            = o.ctx;
    s->owns_ctx       = o.owns;
    s->latency_frames = o.latency_frames;
    {
        const uint32_t bursts = (WORKLET_QUIET_BURSTS * o.burst_frames + block_size - 1u) / block_size;
        s->quiet_blocks = bursts > WORKLET_QUIET_BLOCKS ? bursts : WORKLET_QUIET_BLOCKS;
    }

    /* The adapter is sized for ONE quantum, because that is the most a process() call can ask for:
     * Web Audio 1.0 fixes it at 128 and the 1.1 renderSizeHint only moves it, never makes it vary.
     * With the default 256-frame block that is 2 slots and one block of adapter latency. */
    uint32_t max_request = s->quantum;
    if (max_request < block_size) max_request = block_size;
    if (sink_quant_init(&s->quant, sample_rate, block_size, channels, max_request,
                        render, user) != 0) {
        wk_set_err(err, errcap, "worklet: the fixed-quantum adapter could not be allocated");
        WkTeardown t = { o.ctx, 0, o.owns };
        wk_on_main(worklet_teardown_main, &t);
        free(s);
        return NULL;
    }
    s->q_capacity = s->quant.slots * s->quant.block;

    /* Rule 5's figure, finished: the context's own latency plus the most the adapter can hold. A
     * quantum equal to the block is the pass-through case and holds nothing. */
    if (s->latency_frames && s->quantum != block_size) s->latency_frames += block_size;

    if (adopt > 0) snprintf(s->name, sizeof s->name, "worklet:%d", (int)adopt);
    else           snprintf(s->name, sizeof s->name, "worklet:default");

    /* Rule 6's "succeeded but degraded" channel carries the one thing a page has to act on: the
     * context is suspended and only a user gesture may resume it. Until then the sink host-paces
     * silence and the engine's clocks keep running, which is a degradation and not a failure. */
    if (err && errcap) err[0] = 0;
    if (o.ctx_state != AUDIO_CONTEXT_STATE_RUNNING) {
        wk_set_err(err, errcap, "worklet: the AudioContext is not running yet; a browser resumes "
                                "one only from a user gesture, and until then this sink paces "
                                "silence so the engine's clocks keep advancing");
    }
    return &s->base;
}
