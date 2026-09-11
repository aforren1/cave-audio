/*
 * jack_sink.c — the JACK backend (docs/backends.md, phase 5).
 *
 * WHY IT EXISTS. JACK is the Linux production path. The same binary talks to a JACK2 server
 * (libjack, LGPL-2.1) and to PipeWire (pipewire-jack's drop-in libjack, MIT); which one answers is
 * decided at run time by what the box has installed. On a desktop that reaches the headphones the
 * user is actually on; on a rig box it reaches the array, either a multichannel card or the AES67
 * daemon's virtual card, with the per-speaker routing living in the host's patchbay instead of in
 * the engine.
 *
 * This is the smallest sink of the set and the closest to ASIO: a fixed-size callback, planar
 * float ports that already match the bus layout (so nothing converts and nothing interleaves), an
 * xrun callback that reports faults directly, and a server-filtered time pair.
 *
 * THREE THINGS THE SINK DELIBERATELY DOES NOT DO:
 *   - It never calls jack_set_buffer_size. On pipewire-jack that forces the GLOBAL quantum for
 *     every application on the box, which is the rig's decision to make in its PipeWire config
 *     (default.clock.quantum), not the engine's. The adapter bridges whatever size the server
 *     picked, and a rig that pins the quantum to the engine block gets the pass-through free.
 *   - It never re-asserts its connections. They are a starting point; the patchbay can rewire
 *     them live and the engine must not fight it.
 *   - It never resamples. JACK has no per-client resampler, so a rate mismatch fails the open
 *     with both rates named, headphone profile or not (docs/backends.md rule 6).
 *
 * DEVIATION FROM THE SPEC, deliberate: the client is ACTIVATED AND CONNECTED at open(), not at
 * start(). start() only lifts the gate that lets the process callback render. The reason is the
 * error channel: "the server offers 2 playback ports and this sink has 26" is exactly the message
 * a caller has to see, and bwa_start reports a sink's start() failure with one fixed string while
 * it surfaces an OPEN failure verbatim (engine.c). Between open and start the callback writes
 * silence, which is also what a patchbay wants to see while the engine finishes starting.
 *
 * AUDIO THREAD. The process callback is the server's SCHED_FIFO thread; the audio-thread
 * invariants apply to everything in it. Everything it needs is allocated at open.
 */
#include "sink.h"
#include "sink_quant.h"
#include "os.h"
#include "profile.h"

#include <jack/jack.h>
#include <jack/statistics.h>   /* jack_get_xrun_delayed_usecs: the server's own measure of the fault */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The bus copies straight into the port buffers, so this has to hold. It does on every JACK and
 * pipewire-jack build in existence; the assert is here so a build where it stopped holding fails
 * at compile time rather than by writing halves of samples. */
_Static_assert(sizeof(jack_default_audio_sample_t) == sizeof(float),
               "jack_sink copies the planar float bus into the port buffers with no conversion");

/* The largest cycle the FIFO is sized for, matching engine.c's BWA_MAX_BLOCK device-block ceiling.
 * Allocated up front so jack_set_buffer_size_callback - which the server may fire at any time,
 * including from the process thread on pipewire-jack - never has to allocate. */
#define JACK_MAX_PERIOD 8192

typedef struct {
    bwa_sink base;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t block;              /* the engine quantum; block_size() reports this, not the server's */
    uint32_t q_capacity;         /* frames the adapter can serve in one pull                        */
    uint32_t latency_frames;     /* render->DAC, constant for the life of the sink                  */

    jack_client_t* client;
    jack_port_t*   ports[BWA_CHANNELS];
    bool           activated;

    SinkQuant      quant;

    /* Written by the server's threads, read from the control thread. Relaxed: nobody synchronizes
     * ON them and a reader one cycle stale still reads a monotonic count. */
    _Atomic uint32_t period;         /* the server's current buffer size (diagnostics + latency)  */
    _Atomic int      running;        /* start() opens the gate; before it the ports carry silence */
    _Atomic uint32_t lost;           /* the server went away (jack_on_shutdown)                   */
    /* The xrun callback runs on a JACK NOTIFICATION thread, not on the process thread, so it may
     * not touch the adapter's plain counters. It parks the fault here and the next process
     * callback folds it in through sink_quant_note_dropout, which keeps every count in one place
     * (sink_quant.h says why that matters) and keeps the whole thing race-free. */
    _Atomic uint64_t pending_xruns, pending_xrun_frames;

    /* Host-paced fallback after a server shutdown (rule 4): the engine's clocks and playheads keep
     * advancing on silence instead of freezing. Started from the shutdown callback, at most once. */
    os_thread    host_thread;
    _Atomic int  host_started, host_stop;

    char name[192];              /* "jack:<client prefix>", ASCII */
} JackSink;

static void jack_set_err(char* err, size_t cap, const char* msg) {
    if (err && cap) { strncpy(err, msg, cap - 1); err[cap - 1] = 0; }
}

/* ---- the process callback ---------------------------------------------------------------- */

typedef struct { float* dst[BWA_CHANNELS]; uint32_t channels; } JackOut;

/* sink_quant hands one contiguous run of its FIFO at a time. JACK ports are planar float already,
 * so this is a per-channel memcpy and nothing else: no format conversion, no interleave. */
static void jack_out(void* user, const float* planar, uint32_t stride, uint32_t frame_offset,
                     uint32_t n) {
    JackOut* o = (JackOut*)user;
    for (uint32_t c = 0; c < o->channels; ++c)
        memcpy(o->dst[c] + frame_offset, planar + (size_t)c * stride, (size_t)n * sizeof(float));
}

static void jack_discard_out(void* u, const float* p, uint32_t s, uint32_t o, uint32_t n) {
    (void)u; (void)p; (void)s; (void)o; (void)n;
}

static int jack_process(jack_nframes_t nframes, void* arg) {
    JackSink* s = (JackSink*)arg;

    JackOut o;
    o.channels = s->channels;
    for (uint32_t c = 0; c < s->channels; ++c)
        o.dst[c] = (float*)jack_port_get_buffer(s->ports[c], nframes);

    if (!atomic_load_explicit(&s->running, memory_order_acquire)) {
        for (uint32_t c = 0; c < s->channels; ++c)
            memset(o.dst[c], 0, (size_t)nframes * sizeof(float));
        return 0;
    }

    /* Rule 3's pair, from the server's DLL-filtered clock rather than from the raw device: the
     * drift fit then reads the same filtered time base a QPC-synthesized ASIO stamp does.
     * current_usecs is CLOCK_MONOTONIC microseconds, the clock os_monotonic_ns reads on Linux. */
    uint64_t host_ns;
    {
        jack_nframes_t cur_frames = 0;
        jack_time_t cur_usecs = 0, next_usecs = 0;
        float period_usecs = 0.0f;
        if (jack_get_cycle_times(s->client, &cur_frames, &cur_usecs, &next_usecs, &period_usecs) == 0)
            host_ns = (uint64_t)cur_usecs * 1000ull;
        else
            host_ns = sink_quant_now_ns();
    }

    /* Fold in whatever the xrun callback parked since the last cycle. One event per xrun, the
     * frame estimate summed, so the counts stay exactly what the server reported. */
    const uint64_t xruns = atomic_exchange_explicit(&s->pending_xruns, 0, memory_order_relaxed);
    if (xruns) {
        const uint64_t frames = atomic_exchange_explicit(&s->pending_xrun_frames, 0, memory_order_relaxed);
        sink_quant_note_dropout(&s->quant, frames);
        for (uint64_t i = 1; i < xruns; ++i) sink_quant_note_dropout(&s->quant, 0);
    }

    uint32_t n = (uint32_t)nframes;
    if (n > s->q_capacity) {
        /* The server jumped to a cycle larger than the FIFO was sized for (only reachable past
         * JACK_MAX_PERIOD). Serve what we can and silence the rest rather than leave the port
         * buffers holding the previous cycle's audio, and book the shortfall as the dropout it is. */
        for (uint32_t c = 0; c < s->channels; ++c)
            memset(o.dst[c] + s->q_capacity, 0, (size_t)(n - s->q_capacity) * sizeof(float));
        sink_quant_note_dropout(&s->quant, n - s->q_capacity);
        n = s->q_capacity;
    }

    /* device_pos_valid is deliberately FALSE. JACK hands out exactly nframes every cycle, so the
     * adapter's `written` telescopes to follow the server's frame counter and the queued-depth
     * rule can never fire; on a real xrun the server's counter jumps and the depth rule would
     * report the SAME fault the xrun callback already reported. The server's direct report wins. */
    BWA_ZONE_BEGIN(zb, "jack cycle");
    sink_quant_pull(&s->quant, n, 0, false, host_ns, jack_out, &o);
    BWA_ZONE_END(zb);
    return 0;
}

/* The server dropped or gained frames. Runs on a notification thread; park it for the process
 * callback (see the struct comment). jack_get_xrun_delayed_usecs is the server's own measure of
 * how long the graph was late. */
static int jack_xrun(void* arg) {
    JackSink* s = (JackSink*)arg;
    const float usecs = jack_get_xrun_delayed_usecs(s->client);
    uint64_t frames = 0;
    if (usecs > 0.0f)
        frames = (uint64_t)((double)usecs * 1.0e-6 * (double)s->sample_rate);
    atomic_fetch_add_explicit(&s->pending_xrun_frames, frames, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->pending_xruns, 1u, memory_order_relaxed);
    return 0;
}

/* A quantum change. Nothing to resize: the adapter takes the count per pull and its FIFO was
 * allocated for JACK_MAX_PERIOD at open, so this records the new size and allocates nothing. */
static int jack_bufsize(jack_nframes_t nframes, void* arg) {
    JackSink* s = (JackSink*)arg;
    atomic_store_explicit(&s->period, (uint32_t)nframes, memory_order_relaxed);
    return 0;
}

/* ---- the host-paced fallback (rule 4) ------------------------------------------------------ */

static void jack_host_thread(void* arg) {
    JackSink* s = (JackSink*)arg;
    BWA_THREAD_NAME("bw-audio (JACK lost)");
    const uint64_t block_ns = (uint64_t)s->block * 1000000000ull / (uint64_t)s->sample_rate;
    uint64_t next_ns = 0;
    while (!atomic_load_explicit(&s->host_stop, memory_order_relaxed)) {
        const uint64_t now = sink_quant_now_ns();
        if (next_ns == 0 || now > next_ns + block_ns * 8ull) next_ns = now;
        if (atomic_load_explicit(&s->running, memory_order_relaxed))
            sink_quant_pull(&s->quant, s->block, 0, false, now, jack_discard_out, NULL);
        next_ns += block_ns;
        os_sleep_until_ns(next_ns);          /* absolute deadline: a relative sleep drifts */
    }
}

/* The server went away. Rule 4: report it, never reopen. The engine's dsp clock, playheads and
 * scheduled plays keep advancing on the host clock so a caller can decide what to do. */
static void jack_shutdown(void* arg) {
    JackSink* s = (JackSink*)arg;
    atomic_store_explicit(&s->lost, 1u, memory_order_relaxed);
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&s->host_started, &expected, 1,
                                                memory_order_relaxed, memory_order_relaxed)) {
        if (os_thread_create(&s->host_thread, jack_host_thread, s) != 0)
            atomic_store_explicit(&s->host_started, 2, memory_order_release);   /* 2 = never ran */
        else
            atomic_store_explicit(&s->host_started, 3, memory_order_release);   /* 3 = joinable  */
    }
}

/* ---- vtable -------------------------------------------------------------------------------- */

static int jack_start(bwa_sink* base) {
    JackSink* s = (JackSink*)base;
    atomic_store_explicit(&s->running, 1, memory_order_release);
    return 0;
}

static void jack_stop(bwa_sink* base) {
    JackSink* s = (JackSink*)base;
    atomic_store_explicit(&s->running, 0, memory_order_release);
}

static void jack_close(bwa_sink* base) {
    JackSink* s = (JackSink*)base;
    jack_stop(base);
    /* Rule 8: no callback may still be running when this returns. jack_deactivate takes the client
     * out of the graph and waits for its process callback; jack_client_close ends the notification
     * threads, which is what makes the host_started read below safe. */
    if (s->client) {
        if (s->activated) jack_deactivate(s->client);
        jack_client_close(s->client);
        s->client = NULL;
    }
    /* 1 is the transient state INSIDE the shutdown callback, between claiming the slot and
     * publishing the outcome. Waiting it out is what makes "did a host-paced thread start?" a
     * question with an answer: reading 1 and moving on would either leak the thread or free the
     * adapter under it. Bounded by one os_thread_create. */
    int hs = atomic_load_explicit(&s->host_started, memory_order_acquire);
    while (hs == 1) { os_sleep_ms(1); hs = atomic_load_explicit(&s->host_started, memory_order_acquire); }
    if (hs == 3) {
        atomic_store_explicit(&s->host_stop, 1, memory_order_relaxed);
        os_thread_join(&s->host_thread);
    }
    sink_quant_free(&s->quant);
    free(s);
}

static const char* jack_backend(bwa_sink* base)     { return ((JackSink*)base)->name; }
static uint32_t jack_block_size(bwa_sink* base)     { return ((JackSink*)base)->block; }
static uint32_t jack_output_latency(bwa_sink* base) { return ((JackSink*)base)->latency_frames; }

/* measured is unconditionally true here, and it is earned rather than assumed: the server reports
 * an xrun directly through its own callback, so a zero count means "none happened" and never
 * "could not know". That is the whole distinction bwa_sink_health.measured exists to keep. */
static void jack_health(bwa_sink* base, bwa_sink_health* out) {
    JackSink* s = (JackSink*)base;
    sink_quant_health(&s->quant, out);      /* blocks, dropouts, late_blocks, peak, period */
    out->measured    = true;
    out->device_lost = atomic_load_explicit(&s->lost, memory_order_relaxed);
}

/* Internal readbacks for the sink test (declared in sink.h, deliberately not in bw_audio.h): the
 * server's current cycle size, which decides both how long a deliberate stall has to be and
 * whether the adapter is running in pass-through, and how many output ports are actually connected
 * to something, and whether the server is running its graph in real-time mode. The second is the
 * only way a test can say the ROUTING landed rather than inferring it from a nonzero latency; the
 * third is what tells a test whether a clean 200 ms was ever on offer. */
uint32_t sink_jack_period_frames(bwa_sink* base) {
    return atomic_load_explicit(&((JackSink*)base)->period, memory_order_relaxed);
}

bool sink_jack_is_realtime(bwa_sink* base) {
    JackSink* s = (JackSink*)base;
    return s->client && jack_is_realtime(s->client) != 0;
}

uint32_t sink_jack_connected_ports(bwa_sink* base) {
    JackSink* s = (JackSink*)base;
    uint32_t n = 0;
    for (uint32_t c = 0; c < s->channels; ++c)
        if (s->ports[c] && jack_port_connected(s->ports[c]) > 0) ++n;
    return n;
}

static const bwa_sink_vtbl JACK_VT = {   /* designated: stop/close share a signature, so a positional swap would be silent */
    .type = BWA_SINK_JACK,
    .start = jack_start, .stop = jack_stop, .close = jack_close,
    .backend = jack_backend, .block_size = jack_block_size,
    .output_latency = jack_output_latency,
    .health = jack_health,
};

/* ---- device enumeration -------------------------------------------------------------------- */

/* JACK has ports, not devices. A picker wants the CARD, so the "devices" are the distinct client
 * prefixes of the physical playback ports ("system", "RAVENNA"), one entry each, and the id is the
 * same string - which is exactly what bwa_desc.device wants, since the sink treats it as the
 * jack_get_ports regex. Opens a throwaway client: enumeration needs a connection to the server and
 * there is no lighter way to ask. */
static uint32_t jack_prefixes(char out[][64], uint32_t cap) {
    uint32_t n = 0;
    jack_status_t st = 0;
    jack_client_t* c = jack_client_open("bw_audio_probe", JackNoStartServer, &st);
    if (!c) return 0;
    const char** ports = jack_get_ports(c, NULL, JACK_DEFAULT_AUDIO_TYPE,
                                        JackPortIsPhysical | JackPortIsInput);
    if (ports) {
        for (uint32_t i = 0; ports[i] && n < cap; ++i) {
            const char* colon = strchr(ports[i], ':');
            const size_t len = colon ? (size_t)(colon - ports[i]) : strlen(ports[i]);
            if (len == 0 || len >= 64) continue;
            bool seen = false;
            for (uint32_t k = 0; k < n && !seen; ++k)
                seen = (strncmp(out[k], ports[i], len) == 0 && out[k][len] == 0);
            if (seen) continue;
            memcpy(out[n], ports[i], len);
            out[n][len] = 0;
            ++n;
        }
        jack_free((void*)ports);
    }
    jack_client_close(c);
    return n;
}

uint32_t sink_jack_device_count(void) {
    char names[16][64];
    return jack_prefixes(names, 16);
}

bool sink_jack_device_name(uint32_t index, char* buf, uint32_t cap) {
    if (!buf || cap == 0) return false;
    buf[0] = 0;
    char names[16][64];
    const uint32_t n = jack_prefixes(names, 16);
    if (index >= n) return false;
    return sink_copy_device_name(buf, cap, names[index]);
}

bool sink_jack_device_id(uint32_t index, char* buf, uint32_t cap) {
    return sink_jack_device_name(index, buf, cap);   /* the prefix IS the stable key */
}

/* ---- open ----------------------------------------------------------------------------------- */

bwa_sink* bwa_jack_sink_open(uint32_t sample_rate, uint32_t block_size, uint32_t channels,
                             const char* device, uint32_t flags, bool exact_rate,
                             bwa_render_fn render, void* user, char* err, size_t errcap) {
    (void)flags;        /* no exclusive mode: a JACK client always shares the graph */
    (void)exact_rate;   /* no per-client resampler either, so the rate rule is the array's, always */

    if (!render || channels == 0 || channels > BWA_CHANNELS || block_size == 0 || sample_rate == 0) {
        jack_set_err(err, errcap, "jack: bad arguments");
        return NULL;
    }

    jack_status_t st = 0;
    jack_client_t* client = jack_client_open("bw_audio", JackNoStartServer, &st);
    if (!client) {
        /* JackNoStartServer is what makes this the FAST path to ALSA under AUTO: a box with
         * neither a JACK server nor PipeWire fails here in microseconds instead of forking a
         * jackd and waiting for it. The message names the flag so the failure is not mistaken
         * for "libjack is missing". */
        char m[224];
        snprintf(m, sizeof m, "jack: no jack server is running (JackNoStartServer, status 0x%x); "
                              "start jackd or pipewire-jack, or use the ALSA backend",
                 (unsigned)st);
        jack_set_err(err, errcap, m);
        return NULL;
    }

    /* Rule 6, and it does not relax for a headphone open: JACK has no per-client resampler, so a
     * mismatch is a hard failure with BOTH rates in the message. A PipeWire desktop running its
     * graph at 44.1 kHz needs default.clock.rate = 48000, or an engine created at 44100. */
    const uint32_t srv_rate = (uint32_t)jack_get_sample_rate(client);
    if (srv_rate != sample_rate) {
        char m[240];
        snprintf(m, sizeof m, "jack: the server runs at %u Hz, not the engine's %u Hz, and a JACK "
                              "client cannot resample; set the server rate (PipeWire: "
                              "default.clock.rate) or match bwa_desc.sample_rate to it",
                 srv_rate, sample_rate);
        jack_set_err(err, errcap, m);
        jack_client_close(client);
        return NULL;
    }

    JackSink* s = (JackSink*)calloc(1, sizeof *s);
    if (!s) {
        jack_set_err(err, errcap, "jack: out of memory");
        jack_client_close(client);
        return NULL;
    }
    s->base.vt     = &JACK_VT;
    s->sample_rate = sample_rate;
    s->channels    = channels;
    s->block       = block_size;
    s->client      = client;
    atomic_store_explicit(&s->period, (uint32_t)jack_get_buffer_size(client), memory_order_relaxed);

    /* One output port per bus channel, out_01..out_NN, in bus order. The patchbay sees the array
     * channel numbers rather than a pair of anonymous stereo ports. */
    for (uint32_t c = 0; c < channels; ++c) {
        char pname[16];
        snprintf(pname, sizeof pname, "out_%02u", (unsigned)(c + 1));
        s->ports[c] = jack_port_register(client, pname, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
        if (!s->ports[c]) {
            char m[192];
            snprintf(m, sizeof m, "jack: the server refused output port %u of %u (a client port "
                                  "limit?)", (unsigned)(c + 1), channels);
            jack_set_err(err, errcap, m);
            jack_client_close(client);
            free(s);
            return NULL;
        }
    }

    /* Sized for JACK_MAX_PERIOD, not for the server's current buffer size, so a later quantum
     * change is bookkeeping rather than an allocation on a callback (see jack_bufsize). */
    if (sink_quant_init(&s->quant, sample_rate, block_size, channels, JACK_MAX_PERIOD,
                        render, user) != 0) {
        jack_set_err(err, errcap, "jack: the fixed-quantum adapter could not be allocated");
        jack_client_close(client);
        free(s);
        return NULL;
    }
    s->q_capacity = s->quant.slots * s->quant.block;

    jack_set_process_callback(client, jack_process, s);
    jack_set_buffer_size_callback(client, jack_bufsize, s);
    jack_set_xrun_callback(client, jack_xrun, s);
    jack_on_shutdown(client, jack_shutdown, s);

    if (jack_activate(client) != 0) {
        jack_set_err(err, errcap, "jack: the client could not be activated");
        sink_quant_free(&s->quant);
        jack_client_close(client);
        free(s);
        return NULL;
    }
    s->activated = true;

    /* Rule 10 for a port graph: `device` is a jack_get_ports regex ("system:playback_",
     * "RAVENNA:playback_"), NULL the physical playback ports in order. The connections are a
     * STARTING POINT - the patchbay can rewire them live and the sink never reasserts them. */
    const char** targets = (device && *device)
        ? jack_get_ports(client, device, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput)
        : jack_get_ports(client, NULL, JACK_DEFAULT_AUDIO_TYPE, JackPortIsPhysical | JackPortIsInput);
    uint32_t ntargets = 0;
    if (targets) while (targets[ntargets]) ++ntargets;

    if (ntargets < channels) {
        char m[256];
        snprintf(m, sizeof m, "jack: %u playback port%s match%s '%s', but this sink has %u channels; "
                              "connect a wider device or lower the layout's speaker count",
                 ntargets, ntargets == 1 ? "" : "s", ntargets == 1 ? "es" : "",
                 (device && *device) ? device : "the physical playback ports", channels);
        jack_set_err(err, errcap, m);
        if (targets) jack_free((void*)targets);
        /* The sink is live in the graph by now, and a server shutdown between activate and here
         * would have started the host-paced thread. jack_close is the one teardown that accounts
         * for that, so use it rather than a hand-rolled unwind that could free the adapter under
         * a thread still pulling on it. */
        jack_close(&s->base);
        return NULL;
    }

    char prefix[64] = {0};
    uint32_t connected = 0;
    for (uint32_t c = 0; c < channels; ++c) {
        if (jack_connect(client, jack_port_name(s->ports[c]), targets[c]) == 0) ++connected;
        if (c == 0) {
            const char* colon = strchr(targets[0], ':');
            const size_t len = colon ? (size_t)(colon - targets[0]) : strlen(targets[0]);
            if (len && len < sizeof prefix) { memcpy(prefix, targets[0], len); prefix[len] = 0; }
        }
    }
    jack_free((void*)targets);

    /* Rule 5: the server recomputes port latency on connect, and the client has to ask for the
     * recomputation before reading it back. Max over the ports, because the bus plays as one. */
    jack_recompute_total_latencies(client);
    uint32_t lat = 0;
    for (uint32_t c = 0; c < channels; ++c) {
        jack_latency_range_t r = { 0, 0 };
        jack_port_get_latency_range(s->ports[c], JackPlaybackLatency, &r);
        if ((uint32_t)r.max > lat) lat = (uint32_t)r.max;
    }
    /* Plus what the adapter holds. A server period equal to the engine block is the pass-through
     * case and holds nothing; anything else costs one block. */
    const uint32_t period = atomic_load_explicit(&s->period, memory_order_relaxed);
    if (period != block_size) lat += block_size;
    s->latency_frames = lat;

    if (prefix[0]) snprintf(s->name, sizeof s->name, "jack:%s", prefix);
    else           snprintf(s->name, sizeof s->name, "jack:%s", jack_get_client_name(client));

    /* Rule 6's "succeeded but degraded" channel. A connection the server refused leaves that bus
     * channel audible nowhere, which is a silent-speaker defect if it goes unsaid. */
    if (connected != channels) {
        char m[224];
        snprintf(m, sizeof m, "jack: connected %u of %u output ports; the rest reach no playback "
                              "port until the patchbay routes them", connected, channels);
        jack_set_err(err, errcap, m);
    } else if (err && errcap) {
        err[0] = 0;
    }
    return &s->base;
}
