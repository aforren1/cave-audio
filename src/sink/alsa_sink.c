/*
 * alsa_sink.c — the ALSA backend (docs/backends.md, phase 5).
 *
 * WHY IT EXISTS. It is the no-server Linux path. Two cases want it. A headphone experiment on a
 * bare box, where onset precision is the measurement and the fewest layers between the engine and
 * the DAC is the point: open the card as `hw:`, take its raw clock, be its only client. And a rig
 * box driving the array from a MADI or ADAT card, or from the AES67 daemon's RAVENNA card, which
 * is an ordinary ALSA PCM to this sink - nothing here knows what AES67 is. On a PipeWire or
 * PulseAudio desktop their plugins answer `default`, which is enough for a desk monitor, and AUTO
 * has already tried JACK first there.
 *
 * NO ADAPTER, and that is the one structural difference from every other backend. sink_quant
 * exists because a device API dictates the callback size; here the sink WRITES, so it chooses, and
 * it writes whole engine blocks. The period is a device-side number that only sizes the buffer.
 *
 * THE THREAD paces on the blocking write: snd_pcm_writei returns when the device has room, which
 * is the same wait the null sink synthesizes from a clock. It asks for SCHED_FIFO because a
 * blocking-write loop at normal priority loses its deadline to any busy desktop task; a refusal
 * (no RLIMIT_RTPRIO budget) is a degradation reported at open, never a failure.
 *
 * AUDIO THREAD. Everything the render loop needs is allocated at open: the planar bus, the
 * interleaved frame buffer, and the status object. The loop allocates, locks and logs nothing.
 * snd_pcm_writei and snd_pcm_status are the syscalls the invariant deliberately allows here - they
 * ARE the device, the way ASIO's callback is.
 */
#include "sink/sink.h"
#include "sink/sink_convert.h"
#include "os/os.h"
#include "core/profile.h"

#include <alsa/asoundlib.h>

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    bwa_sink base;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t block;              /* the engine quantum AND the write size                     */
    uint32_t period_frames;      /* what the device settled on                                */
    uint32_t buffer_frames;      /* ditto; the underrun estimate measures against this depth   */
    sink_fmt fmt;                /* the shared clamp/NaN rules pick the conversion             */
    bool     fmt_24_in_32;       /* S24_LE: a 24-bit value in a 32-bit container, not packed   */
    uint32_t bytes_per_sample;

    snd_pcm_t*  pcm;
    bwa_render_fn render;
    void*       user;
    float*      bus;             /* planar channels * block, what render() fills               */
    void*       frames;          /* interleaved block * channels samples of `fmt`              */

    os_thread   thread;
    _Atomic int stop_flag;
    bool        want_realtime;   /* the open predicted SCHED_FIFO would be granted             */

    /* Written on the render thread, read from the control thread. Relaxed: independent monotonic
     * counts that order nothing else, the same call null_sink.c makes. */
    _Atomic uint64_t h_blocks, h_dropouts, h_dropped_frames, h_resyncs, h_late, h_render_ns_peak;
    _Atomic uint32_t lost;
    _Atomic uint32_t latency_frames;   /* snd_pcm_delay, latched on the first good write        */

    char name[192];              /* "alsa:<pcm name>", ASCII */
} AlsaSink;

static void alsa_set_err(char* err, size_t cap, const char* msg) {
    if (err && cap) { strncpy(err, msg, cap - 1); err[cap - 1] = 0; }
}

/* S24_LE is the one format sink_convert.h has no variant for: 24 valid bits inside a 32-bit
 * container, where SINK_FMT_I24 is three PACKED bytes. The clamp and NaN rules still come from the
 * shared header (sink_to_i32 is the only place a float becomes an integer); only the container is
 * ALSA's business, so only the container lives here. */
static void alsa_interleave_24_in_32(int32_t* dst, const float* src, uint32_t channels,
                                     uint32_t nframes) {
    for (uint32_t f = 0; f < nframes; ++f)
        for (uint32_t c = 0; c < channels; ++c)
            *dst++ = sink_to_i32(src[(size_t)c * nframes + f]) >> 8;
}

/* ---- the render thread --------------------------------------------------------------------- */

/* Pace one block from the host clock and throw it away: what the sink does once the device is
 * gone, so the engine's dsp clock, playheads and scheduled plays keep advancing instead of
 * freezing. Rule 4 - a lost device is REPORTED, never silently reopened. */
static void alsa_host_paced_block(AlsaSink* s, uint64_t* next_ns, uint64_t* pos) {
    const uint64_t block_ns = (uint64_t)s->block * 1000000000ull / (uint64_t)s->sample_rate;
    const uint64_t now = os_monotonic_ns();
    if (*next_ns == 0 || now > *next_ns + block_ns * 8ull) *next_ns = now;
    bwa_timestamp ts = { .sample_pos = *pos, .system_time_ns = now };
    s->render(s->user, s->bus, s->block, &ts);
    *pos += s->block;
    atomic_fetch_add_explicit(&s->h_blocks, 1u, memory_order_relaxed);
    *next_ns += block_ns;
    os_sleep_until_ns(*next_ns);          /* absolute deadline: a relative sleep drifts */
}

static void alsa_thread(void* arg) {
    AlsaSink* s = (AlsaSink*)arg;
    BWA_THREAD_NAME("bw-audio (ALSA)");

    const uint64_t block_ns = (uint64_t)s->block * 1000000000ull / (uint64_t)s->sample_rate;

    /* The attempt itself. Whether it would be granted was already answered at open, where the
     * degradation could still reach the caller; this is the half that actually changes anything. */
    const bool rt = s->want_realtime && os_thread_set_realtime(block_ns) == 0;

    const uint64_t sane     = (uint64_t)s->block * 4096ull;   /* the same window sink_position_gap uses */
    snd_pcm_status_t* status = NULL;
    snd_pcm_status_alloca(&status);         /* on this thread's stack: nothing allocates in the loop */

    uint64_t pos = 0;                       /* frames handed to the device: the stream position */
    uint64_t host_next_ns = 0;
    uint64_t last_stamp_ns = 0;             /* the previous block's stamp, so none steps backward */
    bool     have_stamp = false;
    uint64_t pair_ns = 0;                   /* the NEXT block's host time, from the last status  */
    bool     pair_valid = false;
    uint64_t last_good_ns = os_monotonic_ns();

    while (!atomic_load_explicit(&s->stop_flag, memory_order_relaxed)) {
        if (atomic_load_explicit(&s->lost, memory_order_relaxed)) {
            alsa_host_paced_block(s, &host_next_ns, &pos);
            continue;
        }

        /* RULE 3'S PAIR. The device side of it is (written - delay) at htstamp, read after the
         * previous write: the frame the DAC was on, and when. This block starts at `pos`, which is
         * `delay` frames later on the same stream, so its host time is htstamp + delay/rate. That
         * keeps the offset between "this sample_pos" and "plays at this host time" constant, which
         * is all the drift fit needs. Before the first status, and after a restart, the host clock
         * stands in - a stamp with a different offset is still monotonic, and the fit sees one
         * outlier rather than a broken correspondence. */
        uint64_t stamp_ns = pair_valid ? pair_ns : os_monotonic_ns();
        if (have_stamp && stamp_ns <= last_stamp_ns) stamp_ns = last_stamp_ns + block_ns;
        last_stamp_ns = stamp_ns;
        have_stamp    = true;

        bwa_timestamp ts = { .sample_pos = pos, .system_time_ns = stamp_ns };

        BWA_ZONE_BEGIN(zb, "alsa block");
        const uint64_t t0 = os_monotonic_ns();
        s->render(s->user, s->bus, s->block, &ts);
        const uint64_t render_ns = os_monotonic_ns() - t0;
        BWA_ZONE_END(zb);
        BWA_FRAME_MARK();

        if (render_ns > block_ns) atomic_fetch_add_explicit(&s->h_late, 1u, memory_order_relaxed);
        for (;;) {                          /* CAS-max: one writer, so it settles at once */
            uint64_t peak = atomic_load_explicit(&s->h_render_ns_peak, memory_order_relaxed);
            if (render_ns <= peak) break;
            if (atomic_compare_exchange_weak_explicit(&s->h_render_ns_peak, &peak, render_ns,
                                                      memory_order_relaxed, memory_order_relaxed)) break;
        }
        atomic_fetch_add_explicit(&s->h_blocks, 1u, memory_order_relaxed);

        if (s->fmt_24_in_32)
            alsa_interleave_24_in_32((int32_t*)s->frames, s->bus, s->channels, s->block);
        else
            sink_convert_interleaved(s->frames, s->bus, s->channels, s->block, s->block, s->fmt);

        /* The write. Blocking, so the wait IS the pacing. A short return is possible after a
         * signal, so finish the block rather than silently dropping its tail. */
        uint32_t done = 0;
        while (done < s->block && !atomic_load_explicit(&s->stop_flag, memory_order_relaxed)) {
            const uint8_t* p = (const uint8_t*)s->frames
                             + (size_t)done * s->channels * s->bytes_per_sample;
            const snd_pcm_sframes_t w = snd_pcm_writei(s->pcm, p, s->block - done);
            if (w >= 0) { done += (uint32_t)w; continue; }

            if (w == -EPIPE) {
                /* An underrun: the device ran dry while we were not there. Nothing reports HOW
                 * MANY frames it played silence for, so measure it - the time since the last good
                 * write, minus the depth the buffer held when that write returned, at the nominal
                 * rate. Clamped through the same sane window sink_position_gap uses, so one
                 * absurd delta cannot report millions of frames. */
                const uint64_t now  = os_monotonic_ns();
                const uint64_t el   = (now > last_good_ns) ? (now - last_good_ns) : 0;
                const uint64_t went = el / 1000ull * (uint64_t)s->sample_rate / 1000000ull;
                uint64_t lost = (went > s->buffer_frames) ? (went - s->buffer_frames) : 0;
                if (lost > sane) lost = sane;
                atomic_fetch_add_explicit(&s->h_dropouts, 1u, memory_order_relaxed);
                atomic_fetch_add_explicit(&s->h_dropped_frames, lost, memory_order_relaxed);
                /* Re-anchor the stream position on what the device actually clocked out, the way
                 * the adapter re-anchors to a device position: the next block's sample_pos then
                 * lands on the device's stream rather than on our own, now-behind one. */
                pos += lost;
                pair_valid = false;                 /* the old correspondence died with the stream */
                if (snd_pcm_prepare(s->pcm) < 0) { atomic_store_explicit(&s->lost, 1u, memory_order_relaxed); break; }
                last_good_ns = os_monotonic_ns();
                continue;
            }
            if (w == -ESTRPIPE) {
                /* Suspended (a laptop lid, a power event). Resume until the kernel stops saying
                 * "not yet", then prepare. This is the driver reporting a discontinuity to us
                 * rather than us inferring one, which is what driver_resyncs means. */
                int r;
                while ((r = snd_pcm_resume(s->pcm)) == -EAGAIN) os_sleep_ms(10);
                if (r < 0 && snd_pcm_prepare(s->pcm) < 0) {
                    atomic_store_explicit(&s->lost, 1u, memory_order_relaxed);
                    break;
                }
                atomic_fetch_add_explicit(&s->h_resyncs, 1u, memory_order_relaxed);
                pair_valid = false;
                last_good_ns = os_monotonic_ns();
                continue;
            }
            if (w == -EAGAIN || w == -EINTR) continue;
            /* Anything else is the device going away: report it and keep the engine's clocks
             * moving from the host clock. Rule 4 - no automatic recovery. */
            atomic_store_explicit(&s->lost, 1u, memory_order_relaxed);
            break;
        }

        if (atomic_load_explicit(&s->lost, memory_order_relaxed)) continue;
        pos += done;
        last_good_ns = os_monotonic_ns();

        /* The device half of rule 3's pair, plus rule 5's latency on the first pass. htstamp is
         * CLOCK_MONOTONIC because the sw_params asked for it, which is the clock os_monotonic_ns
         * reads on Linux - the two are directly comparable. */
        if (status && snd_pcm_status(s->pcm, status) == 0) {
            snd_htimestamp_t h;
            snd_pcm_status_get_htstamp(status, &h);
            const snd_pcm_sframes_t delay = snd_pcm_status_get_delay(status);
            const uint64_t d = (delay > 0) ? (uint64_t)delay : 0;
            if (h.tv_sec || h.tv_nsec) {
                pair_ns = (uint64_t)h.tv_sec * 1000000000ull + (uint64_t)h.tv_nsec
                        + d * 1000000000ull / (uint64_t)s->sample_rate;
                pair_valid = true;
            }
            /* output_latency() must be CONSTANT for the life of the sink (sink.h), so the measured
             * delay is latched once, on the first block, and the open's buffer-size estimate
             * stands until then. */
            if (d && atomic_load_explicit(&s->latency_frames, memory_order_relaxed) == s->buffer_frames)
                atomic_store_explicit(&s->latency_frames, (uint32_t)d, memory_order_relaxed);
        }
    }

    if (rt) os_thread_clear_realtime();
}

/* ---- vtable -------------------------------------------------------------------------------- */

static int alsa_start(bwa_sink* base) {
    AlsaSink* s = (AlsaSink*)base;
    if (os_thread_valid(&s->thread)) return 0;
    atomic_store_explicit(&s->stop_flag, 0, memory_order_relaxed);
    return os_thread_create(&s->thread, alsa_thread, s);
}

static void alsa_stop(bwa_sink* base) {
    AlsaSink* s = (AlsaSink*)base;
    if (!os_thread_valid(&s->thread)) return;
    atomic_store_explicit(&s->stop_flag, 1, memory_order_relaxed);
    /* Join FIRST, then touch the PCM. snd_pcm_drop would make an in-flight blocking write return
     * at once, which is tempting, but an snd_pcm_t is not thread-safe and calling it under a live
     * snd_pcm_writei is a race. The wait costs at most one blocking write, which is one period.
     * Rule 8: close() returns only once the loop has left. */
    os_thread_join(&s->thread);
    if (s->pcm) { snd_pcm_drop(s->pcm); snd_pcm_prepare(s->pcm); }   /* leave it restartable */
}

static void alsa_close(bwa_sink* base) {
    AlsaSink* s = (AlsaSink*)base;
    alsa_stop(base);
    if (s->pcm) { snd_pcm_close(s->pcm); s->pcm = NULL; }
    free(s->frames);
    free(s->bus);
    free(s);
}

static const char* alsa_backend(bwa_sink* base) { return ((AlsaSink*)base)->name; }
static uint32_t alsa_block_size(bwa_sink* base) { return ((AlsaSink*)base)->block; }
static uint32_t alsa_output_latency(bwa_sink* base) {
    return atomic_load_explicit(&((AlsaSink*)base)->latency_frames, memory_order_relaxed);
}

/* measured is unconditionally true, and earned: the write itself reports an underrun (-EPIPE), so
 * a zero count means "none happened" and never "could not know". */
static void alsa_health(bwa_sink* base, bwa_sink_health* out) {
    AlsaSink* s = (AlsaSink*)base;
    out->blocks         = atomic_load_explicit(&s->h_blocks, memory_order_relaxed);
    out->dropouts       = atomic_load_explicit(&s->h_dropouts, memory_order_relaxed);
    out->dropped_frames = atomic_load_explicit(&s->h_dropped_frames, memory_order_relaxed);
    out->driver_resyncs = atomic_load_explicit(&s->h_resyncs, memory_order_relaxed);
    out->late_blocks    = atomic_load_explicit(&s->h_late, memory_order_relaxed);
    out->render_ns_peak = atomic_load_explicit(&s->h_render_ns_peak, memory_order_relaxed);
    out->period_ns      = (uint64_t)s->block * 1000000000ull / (uint64_t)s->sample_rate;
    out->device_lost    = atomic_load_explicit(&s->lost, memory_order_relaxed);
    out->measured       = true;
}

/* Internal readbacks for the sink test (declared in sink.h, deliberately not in bw_audio.h): a
 * stall only underruns when it outlasts the buffer, and only the sink knows how deep that is. */
uint32_t sink_alsa_period_frames(bwa_sink* base) { return ((AlsaSink*)base)->period_frames; }
uint32_t sink_alsa_buffer_frames(bwa_sink* base) { return ((AlsaSink*)base)->buffer_frames; }

static const bwa_sink_vtbl ALSA_VT = {   /* designated: stop/close share a signature, so a positional swap would be silent */
    .type = BWA_SINK_ALSA,
    .start = alsa_start, .stop = alsa_stop, .close = alsa_close,
    .backend = alsa_backend, .block_size = alsa_block_size,
    .output_latency = alsa_output_latency,
    .health = alsa_health,
};

/* ---- device enumeration -------------------------------------------------------------------- */

/* snd_device_name_hint's PCM list, filtered to the output-capable entries: IOID absent means the
 * PCM does both directions, "Output" means playback only, "Input" is skipped. The NAME is the
 * stable id (it is what snd_pcm_open takes) and DESC's first line is the friendly name - the rest
 * of DESC is the card's own multi-line blurb, which no picker wants. `default` is listed first,
 * because ALSA has no default device beyond that PCM and index 0 should be it. */
#define ALSA_MAX_HINTS 64

typedef struct { char name[128]; char desc[128]; } AlsaHint;

static uint32_t alsa_hints(AlsaHint* out, uint32_t cap) {
    void** hints = NULL;
    if (snd_device_name_hint(-1, "pcm", &hints) != 0 || !hints) return 0;
    uint32_t n = 0;
    int default_at = -1;
    for (uint32_t i = 0; hints[i] && n < cap; ++i) {
        char* io = snd_device_name_get_hint(hints[i], "IOID");
        const bool output = (io == NULL) || (strcmp(io, "Output") == 0);
        free(io);
        if (!output) continue;
        char* nm = snd_device_name_get_hint(hints[i], "NAME");
        if (!nm) continue;
        char* ds = snd_device_name_get_hint(hints[i], "DESC");
        snprintf(out[n].name, sizeof out[n].name, "%s", nm);
        if (ds) {
            char* nl = strchr(ds, '\n');
            if (nl) *nl = 0;
            snprintf(out[n].desc, sizeof out[n].desc, "%s", ds);
        } else {
            snprintf(out[n].desc, sizeof out[n].desc, "%s", nm);
        }
        if (default_at < 0 && strcmp(nm, "default") == 0) default_at = (int)n;
        free(nm);
        free(ds);
        ++n;
    }
    snd_device_name_free_hint(hints);
    if (default_at > 0) {                       /* rotate `default` to index 0 */
        AlsaHint tmp = out[default_at];
        for (int k = default_at; k > 0; --k) out[k] = out[k - 1];
        out[0] = tmp;
    } else if (default_at < 0 && n < cap) {
        /* SYNTHESIZED, because snd_device_name_hint only lists PCMs that declare a hint block. A
         * `pcm.!default` in an asoundrc (the PulseAudio or PipeWire plugin route, and every
         * user-defined default) declares none, so the list comes back without the one name
         * snd_pcm_open always understands. Index 0 has to be the default device or a picker
         * cannot offer it. */
        for (uint32_t k = n; k > 0; --k) out[k] = out[k - 1];
        snprintf(out[0].name, sizeof out[0].name, "default");
        snprintf(out[0].desc, sizeof out[0].desc, "Default Audio Device");
        ++n;
    }
    return n;
}

uint32_t sink_alsa_device_count(void) {
    AlsaHint h[ALSA_MAX_HINTS];
    return alsa_hints(h, ALSA_MAX_HINTS);
}

bool sink_alsa_device_name(uint32_t index, char* buf, uint32_t cap) {
    if (!buf || cap == 0) return false;
    buf[0] = 0;
    AlsaHint h[ALSA_MAX_HINTS];
    const uint32_t n = alsa_hints(h, ALSA_MAX_HINTS);
    if (index >= n) return false;
    return sink_copy_device_name(buf, cap, h[index].desc);
}

bool sink_alsa_device_id(uint32_t index, char* buf, uint32_t cap) {
    if (!buf || cap == 0) return false;
    buf[0] = 0;
    AlsaHint h[ALSA_MAX_HINTS];
    const uint32_t n = alsa_hints(h, ALSA_MAX_HINTS);
    if (index >= n) return false;
    return sink_copy_device_name(buf, cap, h[index].name);
}

/* ---- open ----------------------------------------------------------------------------------- */

/* The rule 2 preference order, walked with snd_pcm_hw_params_test_format so nothing is set until
 * one is known to work. float32 first (no conversion at all), then descending integer width;
 * S24_3LE before S24_LE because the packed layout is what most cards actually present. */
static const struct { snd_pcm_format_t a; sink_fmt f; bool in32; const char* label; } ALSA_FORMATS[] = {
    { SND_PCM_FORMAT_FLOAT_LE, SINK_FMT_F32, false, "float32"    },
    { SND_PCM_FORMAT_S32_LE,   SINK_FMT_I32, false, "int32"      },
    { SND_PCM_FORMAT_S24_3LE,  SINK_FMT_I24, false, "int24"      },
    { SND_PCM_FORMAT_S24_LE,   SINK_FMT_I32, true,  "int24in32"  },
    { SND_PCM_FORMAT_S16_LE,   SINK_FMT_I16, false, "int16"      },
};

bwa_sink* bwa_alsa_sink_open(uint32_t sample_rate, uint32_t block_size, uint32_t channels,
                             const char* device, uint32_t flags, bool exact_rate,
                             bwa_render_fn render, void* user, char* err, size_t errcap) {
    (void)flags;   /* ALSA has no shared/exclusive distinction: a hw: PCM IS exclusive */

    if (!render || channels == 0 || channels > BWA_CHANNELS || block_size == 0 || sample_rate == 0) {
        alsa_set_err(err, errcap, "alsa: bad arguments");
        return NULL;
    }
    const char* pcm_name = (device && *device) ? device : "default";
    /* 256 because that is what engine.c's errbuf holds, and BOTH degradations can apply at once (a
     * plug PCM on a box with no real-time budget). Anything longer is silently truncated there,
     * which would cut the second one in half. */
    char degraded[256] = {0};

    snd_pcm_t* pcm = NULL;
    int e = snd_pcm_open(&pcm, pcm_name, SND_PCM_STREAM_PLAYBACK, 0);
    if (e < 0 || !pcm) {
        char m[256];
        if (e == -ENOENT || e == -ENODEV || e == -ENXIO) {
            /* The runner case and the headless-box case. The phrase is stable on purpose: the sink
             * test matches it to decide between a visible ctest SKIP and a real failure. */
            snprintf(m, sizeof m, "alsa: no PCM named '%s' on this machine (%s)",
                     pcm_name, snd_strerror(e));
        } else {
            snprintf(m, sizeof m, "alsa: could not open PCM '%s' (%s)", pcm_name, snd_strerror(e));
        }
        alsa_set_err(err, errcap, m);
        return NULL;
    }

    snd_pcm_hw_params_t* hw = NULL;
    snd_pcm_hw_params_alloca(&hw);
    if (snd_pcm_hw_params_any(pcm, hw) < 0) {
        char m[192];
        snprintf(m, sizeof m, "alsa: PCM '%s' offers no usable configuration", pcm_name);
        alsa_set_err(err, errcap, m);
        snd_pcm_close(pcm);
        return NULL;
    }

    if (snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED) < 0) {
        char m[192];
        snprintf(m, sizeof m, "alsa: PCM '%s' does not take interleaved read/write access", pcm_name);
        alsa_set_err(err, errcap, m);
        snd_pcm_close(pcm);
        return NULL;
    }

    sink_fmt fmt = SINK_FMT_F32;
    bool in32 = false;
    uint32_t bytes = 4;
    bool have_fmt = false;
    for (size_t i = 0; i < sizeof ALSA_FORMATS / sizeof *ALSA_FORMATS && !have_fmt; ++i) {
        if (snd_pcm_hw_params_test_format(pcm, hw, ALSA_FORMATS[i].a) != 0) continue;
        if (snd_pcm_hw_params_set_format(pcm, hw, ALSA_FORMATS[i].a) < 0) continue;
        fmt      = ALSA_FORMATS[i].f;
        in32     = ALSA_FORMATS[i].in32;
        bytes    = in32 ? 4u : sink_fmt_bytes(ALSA_FORMATS[i].f);
        have_fmt = true;
    }
    if (!have_fmt) {
        char m[224];
        snprintf(m, sizeof m, "alsa: PCM '%s' accepts none of float32, int32, int24, int16", pcm_name);
        alsa_set_err(err, errcap, m);
        snd_pcm_close(pcm);
        return NULL;
    }

    /* RULE 6. Turn the library's own rate converter OFF for a raw card, and for ANY device under
     * exact_rate: the array's rule is that a resampled stream shifts every per-speaker delay, and
     * a `plug` PCM would silently satisfy the request instead of failing it. A headphone open on a
     * plug, default or server PCM keeps the converter and reports the degradation. */
    const bool raw = (strncmp(pcm_name, "hw:", 3) == 0);
    if (raw || exact_rate) snd_pcm_hw_params_set_rate_resample(pcm, hw, 0);

    if (snd_pcm_hw_params_set_rate(pcm, hw, sample_rate, 0) < 0) {
        /* Name BOTH rates: "it did not work" is useless, "it runs at 44100 and you asked for
         * 48000" is actionable. set_rate_near on the same params answers what it would give. */
        unsigned int near_rate = sample_rate;
        int dir = 0;
        snd_pcm_hw_params_set_rate_near(pcm, hw, &near_rate, &dir);
        char m[256];
        snprintf(m, sizeof m, "alsa: PCM '%s' runs at %u Hz, not the engine's %u Hz, and this open "
                              "must not resample; pick a plug or default PCM, or match "
                              "bwa_desc.sample_rate to the device",
                 pcm_name, near_rate, sample_rate);
        alsa_set_err(err, errcap, m);
        snd_pcm_close(pcm);
        return NULL;
    }
    if (!raw && !exact_rate) {
        /* A plug, default or server PCM may have accepted the rate by promising to convert it.
         * Timing stays exact (the stream IS at the engine's rate); timbre is the converter's. */
        snprintf(degraded, sizeof degraded,
                 "alsa: PCM '%s' may resample the engine's %u Hz; open a hw: PCM for an exact "
                 "device clock",
                 pcm_name, sample_rate);
    }

    if (snd_pcm_hw_params_set_channels(pcm, hw, channels) < 0) {
        unsigned int mn = 0, mx = 0;
        snd_pcm_hw_params_get_channels_min(hw, &mn);
        snd_pcm_hw_params_get_channels_max(hw, &mx);
        char m[224];
        snprintf(m, sizeof m, "alsa: PCM '%s' offers %u..%u channels, not the %u requested",
                 pcm_name, mn, mx, channels);
        alsa_set_err(err, errcap, m);
        snd_pcm_close(pcm);
        return NULL;
    }

    /* The period is a DEVICE-side number: it sizes the buffer and decides how often the card
     * interrupts, and the write loop hands over whole engine blocks regardless. Three periods of
     * buffer is the usual compromise between latency and tolerance of a late block. */
    snd_pcm_uframes_t period = block_size;
    int dir = 0;
    snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, &dir);
    snd_pcm_uframes_t buffer = period * 3;
    snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);

    if ((e = snd_pcm_hw_params(pcm, hw)) < 0) {
        char m[224];
        snprintf(m, sizeof m, "alsa: PCM '%s' refused the negotiated configuration (%s)",
                 pcm_name, snd_strerror(e));
        alsa_set_err(err, errcap, m);
        snd_pcm_close(pcm);
        return NULL;
    }
    snd_pcm_hw_params_get_period_size(hw, &period, &dir);
    snd_pcm_hw_params_get_buffer_size(hw, &buffer);

    /* sw_params: the timestamp has to be on CLOCK_MONOTONIC or it cannot be compared with
     * os_monotonic_ns, and the start threshold has to be a full buffer or the stream starts on the
     * first period and underruns before the second arrives. */
    snd_pcm_sw_params_t* sw = NULL;
    snd_pcm_sw_params_alloca(&sw);
    if (snd_pcm_sw_params_current(pcm, sw) == 0) {
        snd_pcm_sw_params_set_tstamp_mode(pcm, sw, SND_PCM_TSTAMP_ENABLE);
        snd_pcm_sw_params_set_tstamp_type(pcm, sw, SND_PCM_TSTAMP_TYPE_MONOTONIC);
        snd_pcm_sw_params_set_start_threshold(pcm, sw, buffer);
        if ((e = snd_pcm_sw_params(pcm, sw)) < 0) {
            char m[224];
            snprintf(m, sizeof m, "alsa: PCM '%s' refused the software parameters (%s)",
                     pcm_name, snd_strerror(e));
            alsa_set_err(err, errcap, m);
            snd_pcm_close(pcm);
            return NULL;
        }
    }

    AlsaSink* s = (AlsaSink*)calloc(1, sizeof *s);
    if (!s) {
        alsa_set_err(err, errcap, "alsa: out of memory");
        snd_pcm_close(pcm);
        return NULL;
    }
    s->base.vt        = &ALSA_VT;
    s->render         = render;
    s->user           = user;
    s->sample_rate    = sample_rate;
    s->channels       = channels;
    s->block          = block_size;
    s->period_frames  = (uint32_t)period;
    s->buffer_frames  = (uint32_t)buffer;
    s->fmt            = fmt;
    s->fmt_24_in_32   = in32;
    s->bytes_per_sample = bytes;
    s->pcm            = pcm;
    atomic_store_explicit(&s->latency_frames, (uint32_t)buffer, memory_order_relaxed);

    s->bus    = (float*)calloc((size_t)block_size * channels, sizeof(float));
    s->frames = calloc((size_t)block_size * channels, bytes);
    if (!s->bus || !s->frames) {
        alsa_set_err(err, errcap, "alsa: the render buffers could not be allocated");
        free(s->frames); free(s->bus); free(s);
        snd_pcm_close(pcm);
        return NULL;
    }

    /* Asked HERE rather than on the render thread, because this is the last moment a degradation
     * can reach the caller (bwa_last_error after a successful bwa_start). The thread still makes
     * the attempt; this only decides whether to make it and what to say. */
    s->want_realtime = os_thread_realtime_available();
    if (!s->want_realtime) {
        const size_t at = strlen(degraded);
        snprintf(degraded + at, sizeof degraded - at, "%s"
                 "alsa: no real-time budget, so the render thread runs at normal priority and a "
                 "busy box can starve it; raise RLIMIT_RTPRIO (the audio group)", at ? "; " : "");
    }

    snprintf(s->name, sizeof s->name, "alsa:%s", pcm_name);

    if (degraded[0]) alsa_set_err(err, errcap, degraded);
    else if (err && errcap) err[0] = 0;
    return &s->base;
}
