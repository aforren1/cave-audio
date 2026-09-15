/*
 * audio_sink_test.c — verification of the audio loop, hardware where there is any and offline
 * where there is not. The null sink (the offline backend) carries the contract every backend
 * shares: a stable callback fires, and the timestamp (sample position + system time) advances
 * monotonically. Then one section per compiled DEVICE backend runs the same contract against
 * real hardware.
 *
 * THE SKIP RULE. A device section on a machine with no device must SKIP, never pass: a test that
 * cannot fail is worse than no test (CLAUDE.md). So a "no endpoint" open exits 77, which the
 * ctest target maps to SKIP_RETURN_CODE, and the dashboard says skipped. Any other failure is a
 * failure.
 */
#include "sink.h"

#include <stdio.h>
#include <stdlib.h>       /* getenv: every device section's device string is overridable */
#include <string.h>

#include "os.h"

#include <stdatomic.h>

#define SKIP_EXIT 77   /* ctest SKIP_RETURN_CODE; see CMakeLists.txt */

typedef struct {
    unsigned long blocks;
    uint64_t      last_sample_pos;
    uint64_t      last_ns;
    uint64_t      prev_ns;
    int           first;
    int           time_monotonic;
    int           pos_monotonic;
    uint64_t      prev_pos;
    uint32_t      channels;        /* the bus width this probe was opened for */
    uint32_t      nframes_first;   /* the first block's size...               */
    int           nframes_stable;  /* ...and whether every later one matched  */
    /* Set to N and the NEXT render blocks for N ms, once. That is a deliberate missed deadline:
     * the device drains while we hold its buffer, which is the only way to make a real dropout
     * happen on real hardware rather than assert about arithmetic. Consumed once. */
    _Atomic int   stall_ms_once;
} Probe;

static void probe_init(Probe* p, uint32_t channels) {
    memset(p, 0, sizeof *p);
    p->first = 1; p->time_monotonic = 1; p->pos_monotonic = 1;
    p->channels = channels; p->nframes_stable = 1;
}

static void on_render(void* user, float* bus, uint32_t nframes, const bwa_timestamp* ts) {
    Probe* p = (Probe*)user;
    /* prove the bus is writable for the full block (the engine would mix here) */
    memset(bus, 0, sizeof(float) * (size_t)nframes * p->channels);

    /* Contract rule 1: the render quantum is FIXED. Every backend past ASIO reaches it through
     * the adapter rather than from the device, so this is the assertion that says the adapter is
     * actually in the path. */
    if (p->first) p->nframes_first = nframes;
    else if (nframes != p->nframes_first) p->nframes_stable = 0;

    /* The injected overrun. Sleeping on the audio thread is exactly what this must never do in
     * production, which is why it lives in a test hook and not in the sink. */
    {
        const int stall = atomic_exchange_explicit(&p->stall_ms_once, 0, memory_order_relaxed);
        if (stall > 0) os_sleep_ms((unsigned)stall);
    }

    if (!p->first) {
        if (ts->system_time_ns < p->prev_ns)  p->time_monotonic = 0;
        if (ts->sample_pos    <  p->prev_pos) p->pos_monotonic  = 0;
    }
    p->prev_ns  = ts->system_time_ns;
    p->prev_pos = ts->sample_pos;
    p->first    = 0;

    p->blocks++;
    p->last_sample_pos = ts->sample_pos;
    p->last_ns         = ts->system_time_ns;
}

/* The gap rule, on its own. A real missed deadline needs a real device, but the arithmetic that
 * turns a position jump into a count is ordinary code — and it is the part that would silently
 * regress. Both refusals matter as much as the detection: a driver reset that rewinds or flings the
 * position must not be reported as millions of lost frames. */
static int test_gap_rule(void) {
    const uint32_t BS = 256;
    struct { uint64_t expected, actual; uint64_t want; const char* what; } cases[] = {
        { 1024, 1024,        0,        "continuous"                       },
        { 1024, 1024 + 256,  256,      "one block lost"                   },
        { 1024, 1024 + 2560, 2560,     "ten blocks lost"                  },
        { 1024, 512,         0,        "position went backward (reset)"   },
        { 1024, 1024ull + (uint64_t)256 * 4096 + 1, 0, "absurd jump (stale stamp)" },
        { 1024, 1024ull + (uint64_t)256 * 4096,     (uint64_t)256 * 4096, "the window's edge still counts" },
    };
    int ok = 1;
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        const uint64_t got = sink_position_gap(cases[i].expected, cases[i].actual, BS);
        if (got != cases[i].want) {
            fprintf(stderr, "FAIL: gap %s: got %llu, want %llu\n", cases[i].what,
                    (unsigned long long)got, (unsigned long long)cases[i].want);
            ok = 0;
        }
    }
    return ok;
}

/* End to end through the real sink: inject a device position skip and the counters must report
 * exactly one dropout of exactly that many frames. This is the closest an offline test can get to
 * a starved device, and it covers every line between the position compare and the readback. */
static int test_injected_drop(void) {
    Probe p;
    probe_init(&p, BWA_CHANNELS);

    const uint32_t SR = 48000, BS = 256, SKIP = 3;
    char err[256] = {0};
    bwa_sink* s = bwa_null_sink_open(SR, BS, BWA_CHANNELS, on_render, &p, err, sizeof err);
    if (!s) { fprintf(stderr, "FAIL: inject open: %s\n", err[0] ? err : "(no message)"); return 0; }
    if (bwa_sink_start(s) != 0) { fprintf(stderr, "FAIL: inject start\n"); bwa_sink_close(s); return 0; }

    os_sleep_ms(50);
    bwa_null_sink_skip_blocks = (int)SKIP;     /* the device runs on for 3 blocks without us */
    os_sleep_ms(100);
    bwa_sink_stop(s);

    bwa_sink_health h;
    bwa_sink_get_health(s, &h);
    int ok = 1;
    if (h.dropouts != 1) {
        fprintf(stderr, "FAIL: injected one dropout, counted %llu\n", (unsigned long long)h.dropouts); ok = 0;
    }
    if (h.dropped_frames != (uint64_t)SKIP * BS) {
        fprintf(stderr, "FAIL: dropped %llu frames, expected %u\n",
                (unsigned long long)h.dropped_frames, SKIP * BS); ok = 0;
    }
    if (!h.measured) { fprintf(stderr, "FAIL: injected run reports unmeasured\n"); ok = 0; }
    bwa_sink_close(s);
    return ok;
}

/* The manual sink has no clock and no deadline, so it must say measured = false rather than report
 * a clean bill. "Cannot know" and "nothing happened" are different answers, and conflating them is
 * how a starved device goes unnoticed. */
static int test_manual_unmeasured(void) {
    Probe p;
    probe_init(&p, BWA_CHANNELS);
    char err[256] = {0};
    bwa_sink* s = bwa_manual_sink_open(48000, 256, BWA_CHANNELS, on_render, &p, err, sizeof err);
    if (!s) { fprintf(stderr, "FAIL: manual open: %s\n", err[0] ? err : "(no message)"); return 0; }

    uint32_t ch = 0, nf = 0;
    for (int i = 0; i < 4; ++i) bwa_sink_render_block(s, &ch, &nf);

    bwa_sink_health h;
    bwa_sink_get_health(s, &h);
    int ok = 1;
    if (h.measured) { fprintf(stderr, "FAIL: the manual sink cannot observe a dropout, but claims to\n"); ok = 0; }
    if (h.dropouts || h.blocks) { fprintf(stderr, "FAIL: unmeasured health must be zeroed\n"); ok = 0; }
    bwa_sink_close(s);
    return ok;
}

#ifdef BWA_HAVE_WASAPI
/* The live-device section. Same contract as the null sink above, on the Windows default render
 * endpoint: a stable callback at a CONSTANT nframes equal to block_size (the adapter's whole
 * job), monotonic position and time, measured health, and a stop and close that return.
 *
 * Returns 0 (ok), 1 (failed), or SKIP_EXIT when there is no endpoint to open. */
static int test_wasapi(void) {
    const uint32_t SR = 48000, BS = 256;
    Probe p;
    probe_init(&p, 2);
    char err[256] = {0};

    bwa_sink* s = bwa_wasapi_sink_open(SR, BS, 2, NULL, 0, false, on_render, &p, err, sizeof err);
    if (!s) {
        if (!err[0]) { fprintf(stderr, "FAIL: wasapi open failed with no message\n"); return 1; }
        /* "no endpoint" is the CI runner and a headless box. Anything else is a real failure:
         * a machine WITH audio that cannot open it is exactly what this test exists to catch. */
        if (strstr(err, "no active render endpoint")) {
            printf("SKIP: wasapi: %s\n", err);
            return SKIP_EXIT;
        }
        fprintf(stderr, "FAIL: wasapi open: %s\n", err);
        return 1;
    }
    if (err[0]) printf("wasapi: opened with a degradation reported: %s\n", err);

    const char* backend = bwa_sink_backend(s);
    const uint32_t block = bwa_sink_block_size(s);
    const uint32_t latency = bwa_sink_output_latency(s);
    if (bwa_sink_type_of(s) != BWA_SINK_WASAPI) {
        fprintf(stderr, "FAIL: wasapi sink reports the wrong backend type\n");
        bwa_sink_close(s); return 1;
    }
    if (strncmp(backend, "wasapi:", 7) != 0) {
        fprintf(stderr, "FAIL: backend string is '%s', want \"wasapi:<endpoint>\"\n", backend);
        bwa_sink_close(s); return 1;
    }
    if (bwa_sink_start(s) != 0) { fprintf(stderr, "FAIL: wasapi start\n"); bwa_sink_close(s); return 1; }

    /* --- phase 1: a healthy run --- */
    os_sleep_ms(200);                                   /* ~37 blocks at 256/48000 = 5.33 ms */

    const uint32_t dev_frames = sink_wasapi_device_frames(s);
    const uint32_t per_frames = sink_wasapi_period_frames(s);
    /* The shared-mode rule reads GetCurrentPadding == 0 as "the engine came for a period and found
     * nothing of ours". That only distinguishes anything when the buffer is DEEPER than a period:
     * when they are equal, padding is 0 at every event by design. */
    const int armed = (dev_frames > per_frames);

    bwa_sink_health h1;
    bwa_sink_get_health(s, &h1);                  /* control thread may sample a running sink */
    int ok = 1;
    if (h1.dropouts != 0) {
        fprintf(stderr, "FAIL: %llu dropouts on a healthy 200 ms run\n",
                (unsigned long long)h1.dropouts); ok = 0;
    }

    /* --- phase 2: starve it on purpose --- */
    int starved = 0;
    if (armed) {
        /* Long enough that the buffer certainly empties: two full buffers plus slack. */
        const unsigned stall_ms = (2u * dev_frames * 1000u / SR) + 20u;
        atomic_store_explicit(&p.stall_ms_once, (int)stall_ms, memory_order_relaxed);
        os_sleep_ms(200u + stall_ms);
        starved = 1;
    }

    bwa_sink_stop(s);                             /* joins the render thread */

    const unsigned long blocks = p.blocks;
    if (blocks < 10) { fprintf(stderr, "FAIL: %lu blocks in 200 ms, want at least 10\n", blocks); ok = 0; }
    if (block != BS) { fprintf(stderr, "FAIL: block_size() is %u, want the engine's %u\n", block, BS); ok = 0; }
    if (!p.nframes_stable || p.nframes_first != BS) {
        fprintf(stderr, "FAIL: nframes varied or was not the block size (first=%u, stable=%d)\n",
                p.nframes_first, p.nframes_stable); ok = 0;
    }
    if (!p.time_monotonic) { fprintf(stderr, "FAIL: system_time_ns not monotonic\n"); ok = 0; }
    if (!p.pos_monotonic)  { fprintf(stderr, "FAIL: sample_pos not monotonic\n");     ok = 0; }
    if (p.last_sample_pos != (uint64_t)BS * (blocks - 1)) {
        fprintf(stderr, "FAIL: sample_pos drift (last=%llu, expected=%llu)\n",
                (unsigned long long)p.last_sample_pos, (unsigned long long)((uint64_t)BS * (blocks - 1)));
        ok = 0;
    }

    bwa_sink_health h;
    bwa_sink_get_health(s, &h);
    if (h.blocks != blocks) {
        fprintf(stderr, "FAIL: health counted %llu blocks, the probe saw %lu\n",
                (unsigned long long)h.blocks, blocks); ok = 0;
    }
    if (h.device_lost) { fprintf(stderr, "FAIL: the device was reported lost during a clean run\n"); ok = 0; }
    if (h.period_ns == 0) { fprintf(stderr, "FAIL: no block period to measure the budget against\n"); ok = 0; }

    /* THE HONESTY RULE, both ways round. An armed endpoint must say it can see a dropout AND then
     * actually see the one that was injected; an endpoint whose buffer is one period deep must say
     * it CANNOT, rather than report a clean zero it had no way to earn. */
    if (armed) {
        if (!h.measured) {
            fprintf(stderr, "FAIL: buffer %u > period %u frames, so the underrun rule is armed, "
                            "but health reports unmeasured\n", dev_frames, per_frames); ok = 0;
        }
        if (h.dropouts < 1) {
            fprintf(stderr, "FAIL: the render thread was stalled past two full buffers and no "
                            "dropout was counted (%llu)\n", (unsigned long long)h.dropouts); ok = 0;
        }
        if (h.dropouts >= 1 && h.dropped_frames == 0) {
            fprintf(stderr, "FAIL: a dropout was counted with 0 dropped frames\n"); ok = 0;
        }
    } else {
        if (h.measured) {
            fprintf(stderr, "FAIL: buffer %u == period %u frames, so padding is 0 at every event "
                            "and nothing can be judged, but health claims it can\n",
                    dev_frames, per_frames); ok = 0;
        }
    }

    printf("wasapi OK: backend=%s blocks=%lu block=%u latency=%u frames\n", backend, blocks, block, latency);
    printf("  buffer=%u period=%u frames -> underrun rule %s%s\n", dev_frames, per_frames,
           armed ? "ARMED" : "not armed (buffer is one period; measured=false)",
           starved ? ", starve injected" : "");
    printf("  measured=%d xruns=%llu dropped=%llu late=%llu resyncs=%llu\n", h.measured ? 1 : 0,
           (unsigned long long)h.dropouts, (unsigned long long)h.dropped_frames,
           (unsigned long long)h.late_blocks, (unsigned long long)h.driver_resyncs);
    bwa_sink_close(s);                            /* must return, not hang */
    return ok ? 0 : 1;
}
#endif /* BWA_HAVE_WASAPI */

#ifdef BWA_HAVE_JACK
/* Rule 6 says a rate-mismatch message must carry the device's rate AND the engine's, because "it
 * did not work" is not actionable and "it runs at 44100 and you asked for 48000" is. Both Linux
 * sections check that the same way. */
static int names_both_rates(const char* msg, uint32_t engine_rate, uint32_t other_rate) {
    char a[32], b[32];
    snprintf(a, sizeof a, "%u", engine_rate);
    snprintf(b, sizeof b, "%u", other_rate);
    return strstr(msg, a) != NULL && strstr(msg, b) != NULL;
}
#endif

#if defined(BWA_HAVE_ALSA) || defined(BWA_HAVE_AAUDIO)
/* The same rule where only ONE of the two rates is known to the test: the requested rate must be
 * named, and so must a device rate, whatever the device chose to report. So: the message names the
 * request, and "runs at <n> Hz" names an n that is a real rate and not the request. Shared by the
 * ALSA and AAudio sections, which both meet a device whose own rate the test cannot predict. */
static int rejects_naming_both(const char* msg, uint32_t requested) {
    char want[32];
    snprintf(want, sizeof want, "%u", requested);
    if (!strstr(msg, want)) return 0;
    const char* at = strstr(msg, "runs at ");
    if (!at) return 0;
    const unsigned long dev = strtoul(at + 8, NULL, 10);
    return dev > 0 && dev != (unsigned long)requested;
}
#endif

#ifdef BWA_HAVE_JACK
/* The JACK section. Same contract as the null sink above, against a live server: a stable callback
 * at a CONSTANT nframes equal to block_size (the adapter's whole job, since the server's cycle need
 * not be the engine block), monotonic position and time, measured health, a deliberate stall, a
 * rate-mismatch open that must fail loudly, and a stop and close that return.
 *
 * BWA_TEST_JACK_PORTS overrides the port regex (the default is the physical playback ports) and
 * BWA_TEST_JACK_CHANNELS the width, so one binary reaches a 26-port rig, a dummy server, or a
 * PipeWire desktop without editing code.
 *
 * Returns 0 (ok), 1 (failed), or SKIP_EXIT when there is no server to talk to. */
static int test_jack(void) {
    const uint32_t SR = 48000, BS = 256;
    const char* ports = getenv("BWA_TEST_JACK_PORTS");
    if (ports && !*ports) ports = NULL;
    uint32_t ch = 2;
    {
        const char* e = getenv("BWA_TEST_JACK_CHANNELS");
        if (e) {
            const long v = strtol(e, NULL, 10);
            if (v >= 1 && v <= (long)BWA_CHANNELS) ch = (uint32_t)v;
        }
    }

    Probe p;
    probe_init(&p, ch);
    char err[256] = {0};

    bwa_sink* s = bwa_jack_sink_open(SR, BS, ch, ports, 0, false, on_render, &p, err, sizeof err);
    if (!s) {
        if (!err[0]) { fprintf(stderr, "FAIL: jack open failed with no message\n"); return 1; }
        /* No server is the CI runner and any box without JACK or PipeWire. A server at another
         * rate, or with fewer playback ports than this run asked for, is a CONFIGURATION the
         * section cannot run against rather than a broken sink. All three skip, visibly, with the
         * reason printed; anything else is a failure. */
        if (strstr(err, "no jack server")) { printf("SKIP: %s\n", err); return SKIP_EXIT; }
        if (strstr(err, "not the engine's")) {
            char want[32];
            snprintf(want, sizeof want, "%u Hz", SR);
            if (!strstr(err, want)) {
                fprintf(stderr, "FAIL: the rate-mismatch message does not name the engine rate: %s\n", err);
                return 1;
            }
            printf("SKIP: %s\n", err);
            return SKIP_EXIT;
        }
        if (strstr(err, "playback port")) { printf("SKIP: %s\n", err); return SKIP_EXIT; }
        fprintf(stderr, "FAIL: jack open: %s\n", err);
        return 1;
    }
    if (err[0]) printf("jack: opened with a degradation reported: %s\n", err);

    const char* backend = bwa_sink_backend(s);
    const uint32_t block   = bwa_sink_block_size(s);
    const uint32_t latency = bwa_sink_output_latency(s);
    const uint32_t period  = sink_jack_period_frames(s);
    int ok = 1;
    if (bwa_sink_type_of(s) != BWA_SINK_JACK) {
        fprintf(stderr, "FAIL: jack sink reports the wrong backend type\n");
        bwa_sink_close(s); return 1;
    }
    if (strncmp(backend, "jack:", 5) != 0) {
        fprintf(stderr, "FAIL: backend string is '%s', want \"jack:<client prefix>\"\n", backend);
        bwa_sink_close(s); return 1;
    }
    if (bwa_sink_start(s) != 0) { fprintf(stderr, "FAIL: jack start\n"); bwa_sink_close(s); return 1; }

    /* --- phase 1: a healthy run ---
     *
     * Gated on the SERVER running its graph in real-time mode, which is the only configuration
     * where a clean 200 ms was ever on offer. A server started with --no-realtime (jackd under WSL,
     * where mlock and SCHED_FIFO are both refused) puts its process thread in the ordinary
     * scheduler band, and a busy host then makes the client late through no fault of the sink.
     * Measured: at 26 channels on such a box, this assertion passed on some runs and counted 1 to 4
     * xruns on others. Reported rather than asserted there. */
    const bool srv_rt = sink_jack_is_realtime(s);
    os_sleep_ms(200);                                   /* ~37 blocks at 256/48000 = 5.33 ms */
    bwa_sink_health h1;
    bwa_sink_get_health(s, &h1);
    if (h1.dropouts != 0) {
        if (srv_rt) {
            fprintf(stderr, "FAIL: %llu xruns on a healthy 200 ms run\n",
                    (unsigned long long)h1.dropouts); ok = 0;
        } else {
            printf("  NOTE: %llu xruns on the healthy 200 ms run, on a server that is NOT in "
                   "real-time mode; not asserted (see the comment).\n",
                   (unsigned long long)h1.dropouts);
        }
    }

    /* --- phase 2: stall the process callback past a full cycle on purpose --- */
    const unsigned stall_ms = (period ? (4u * period * 1000u / SR) : 20u) + 20u;
    atomic_store_explicit(&p.stall_ms_once, (int)stall_ms, memory_order_relaxed);
    os_sleep_ms(200u + stall_ms);

    bwa_sink_stop(s);                             /* closes the render gate */
    os_sleep_ms(20);                              /* let the cycle that was in flight finish */

    const unsigned long blocks = p.blocks;
    if (blocks < 10) { fprintf(stderr, "FAIL: %lu blocks in 200 ms, want at least 10\n", blocks); ok = 0; }
    if (block != BS) { fprintf(stderr, "FAIL: block_size() is %u, want the engine's %u\n", block, BS); ok = 0; }
    if (!p.nframes_stable || p.nframes_first != BS) {
        fprintf(stderr, "FAIL: nframes varied or was not the block size (first=%u, stable=%d)\n",
                p.nframes_first, p.nframes_stable); ok = 0;
    }
    if (!p.time_monotonic) { fprintf(stderr, "FAIL: system_time_ns not monotonic\n"); ok = 0; }
    if (!p.pos_monotonic)  { fprintf(stderr, "FAIL: sample_pos not monotonic\n");     ok = 0; }
    if (p.last_sample_pos != (uint64_t)BS * (blocks - 1)) {
        fprintf(stderr, "FAIL: sample_pos drift (last=%llu, expected=%llu)\n",
                (unsigned long long)p.last_sample_pos, (unsigned long long)((uint64_t)BS * (blocks - 1)));
        ok = 0;
    }
    if (latency == 0) {
        fprintf(stderr, "FAIL: no playback latency read back from the connected ports\n"); ok = 0;
    }
    /* The routing landed, said directly rather than inferred from a nonzero latency. Every output
     * port must reach a playback port, or that bus channel is audible nowhere. */
    const uint32_t connected = sink_jack_connected_ports(s);
    if (connected != ch) {
        fprintf(stderr, "FAIL: %u of %u output ports are connected to a playback port\n", connected, ch);
        ok = 0;
    }

    bwa_sink_health h;
    bwa_sink_get_health(s, &h);
    if (!h.measured) { fprintf(stderr, "FAIL: the server reports xruns directly, so measured must be true\n"); ok = 0; }
    if (h.blocks != blocks) {
        fprintf(stderr, "FAIL: health counted %llu blocks, the probe saw %lu\n",
                (unsigned long long)h.blocks, blocks); ok = 0;
    }
    if (h.device_lost) { fprintf(stderr, "FAIL: the server was reported gone during a clean run\n"); ok = 0; }
    if (h.period_ns == 0) { fprintf(stderr, "FAIL: no block period to measure the budget against\n"); ok = 0; }
    /* Both counts are demanded, and both were confirmed against jackd's dummy driver, which does
     * call a stalled client late (JackTimedDriver::Process XRun). A server that stayed silent
     * through a stall of several cycles would be the finding, not a reason to soften this. */
    if (h.late_blocks < 1) {
        fprintf(stderr, "FAIL: a %u ms stall inside render() was not counted as a late block\n", stall_ms);
        ok = 0;
    }
    if (h.dropouts < 1) {
        fprintf(stderr, "FAIL: the process callback was stalled %u ms, past %u cycles, and the "
                        "server reported no xrun\n", stall_ms, 4u); ok = 0;
    }

    printf("jack OK: backend=%s blocks=%lu block=%u period=%u latency=%u frames, %u/%u ports "
           "connected, server %s%s\n",
           backend, blocks, block, period, latency, connected, ch,
           srv_rt ? "REALTIME" : "not realtime",
           (period == block) ? " (adapter in pass-through)" : "");
    printf("  measured=%d xruns=%llu dropped=%llu late=%llu resyncs=%llu (stall %u ms injected)\n",
           h.measured ? 1 : 0, (unsigned long long)h.dropouts, (unsigned long long)h.dropped_frames,
           (unsigned long long)h.late_blocks, (unsigned long long)h.driver_resyncs, stall_ms);
    if (h.dropouts >= 1 && h.dropped_frames == 0) {
        /* NOT a failure, and worth saying rather than asserting away. dropped_frames is the
         * SERVER'S own measure (jack_get_xrun_delayed_usecs), and it can legitimately round to
         * zero frames: jackd's dummy driver reported a 41 ms client stall as 19 us of graph
         * lateness, which is under one frame at 48 kHz. The event count is the trustworthy half
         * on this backend; ALSA's estimate, which the sink measures itself, is not. */
        printf("  NOTE: the server measured the xrun as under one frame of delay, so dropped=0 is "
               "its own number and not a lost count.\n");
    }
    bwa_sink_close(s);                            /* must return, not hang */

    /* --- rule 6: a rate the server does not run must fail the open, naming BOTH rates --- */
    {
        const uint32_t other = 44100;
        Probe q;
        probe_init(&q, ch);
        char e2[256] = {0};
        bwa_sink* bad = bwa_jack_sink_open(other, BS, ch, ports, 0, false, on_render, &q, e2, sizeof e2);
        if (bad) {
            fprintf(stderr, "FAIL: a %u Hz open succeeded against a %u Hz server; JACK has no "
                            "per-client resampler\n", other, SR);
            bwa_sink_close(bad);
            ok = 0;
        } else if (!names_both_rates(e2, other, SR)) {
            fprintf(stderr, "FAIL: the rate-mismatch message names only one rate: %s\n",
                    e2[0] ? e2 : "(no message)");
            ok = 0;
        } else {
            printf("  rate mismatch rejected: %s\n", e2);
        }
    }
    return ok ? 0 : 1;
}
#endif /* BWA_HAVE_JACK */

#ifdef BWA_HAVE_ALSA
/* The ALSA section. Same contract, against a live PCM, with two differences that follow from the
 * backend: there is no fixed-quantum adapter (the sink writes, so it picks the size, and the
 * assertion below still pins that the size is the engine block), and a stall long enough to drain
 * the buffer produces a real -EPIPE underrun, which the sink must count WITH a frame estimate.
 *
 * BWA_TEST_ALSA_DEVICE overrides the PCM name (the default is "default"), so one binary reaches
 * "pulse", "plughw:0,0" or a rig's "hw:" card without editing code. Do NOT point it at the
 * hardware-free "null" PCM: that one accepts writes as fast as they arrive and never paces, so the
 * device cannot run dry and the underrun assertion below fails by construction. Measured: 257,811
 * blocks in the same 470 ms a paced device renders 76 in. It is good for the open sequence and the
 * format walk, nothing else.
 *
 * Returns 0 (ok), 1 (failed), or SKIP_EXIT when there is no PCM to open. */
static int test_alsa(void) {
    const uint32_t SR = 48000, BS = 256;
    const char* dev = getenv("BWA_TEST_ALSA_DEVICE");
    if (dev && !*dev) dev = NULL;

    Probe p;
    probe_init(&p, 2);
    char err[256] = {0};

    bwa_sink* s = bwa_alsa_sink_open(SR, BS, 2, dev, 0, false, on_render, &p, err, sizeof err);
    if (!s) {
        if (!err[0]) { fprintf(stderr, "FAIL: alsa open failed with no message\n"); return 1; }
        /* "no PCM named" is the CI runner and any box with no sound card. Anything else is a real
         * failure: a machine WITH a card that cannot open it is what this section exists to catch. */
        if (strstr(err, "no PCM named")) { printf("SKIP: %s\n", err); return SKIP_EXIT; }
        fprintf(stderr, "FAIL: alsa open: %s\n", err);
        return 1;
    }
    /* Both degradations that can ride on a successful open reach the caller through here (rule 6's
     * resampler, and a render thread with no real-time budget). Printed rather than asserted:
     * which of them applies is a property of the box, not of the sink. */
    if (err[0]) printf("alsa: opened with a degradation reported: %s\n", err);

    const char* backend = bwa_sink_backend(s);
    const uint32_t block  = bwa_sink_block_size(s);
    const uint32_t period = sink_alsa_period_frames(s);
    const uint32_t buffer = sink_alsa_buffer_frames(s);
    int ok = 1;
    if (bwa_sink_type_of(s) != BWA_SINK_ALSA) {
        fprintf(stderr, "FAIL: alsa sink reports the wrong backend type\n");
        bwa_sink_close(s); return 1;
    }
    if (strncmp(backend, "alsa:", 5) != 0) {
        fprintf(stderr, "FAIL: backend string is '%s', want \"alsa:<pcm>\"\n", backend);
        bwa_sink_close(s); return 1;
    }
    if (bwa_sink_start(s) != 0) { fprintf(stderr, "FAIL: alsa start\n"); bwa_sink_close(s); return 1; }

    /* --- phase 1: a healthy run --- */
    os_sleep_ms(200);                                   /* ~37 blocks at 256/48000 = 5.33 ms */
    bwa_sink_health h1;
    bwa_sink_get_health(s, &h1);
    if (h1.dropouts != 0) {
        fprintf(stderr, "FAIL: %llu underruns on a healthy 200 ms run\n",
                (unsigned long long)h1.dropouts); ok = 0;
    }
    if (p.blocks < 10) {
        fprintf(stderr, "FAIL: %lu blocks in 200 ms, want at least 10\n", p.blocks); ok = 0;
    }

    /* --- phase 2: starve it on purpose. Twice the buffer plus slack, so the device certainly runs
     *     dry and snd_pcm_writei certainly returns -EPIPE. --- */
    const unsigned stall_ms = (2u * buffer * 1000u / SR) + 40u;
    atomic_store_explicit(&p.stall_ms_once, (int)stall_ms, memory_order_relaxed);
    os_sleep_ms(200u + stall_ms);

    bwa_sink_stop(s);                             /* joins the render thread */

    const unsigned long blocks = p.blocks;
    const uint32_t latency = bwa_sink_output_latency(s);
    if (block != BS) { fprintf(stderr, "FAIL: block_size() is %u, want the engine's %u\n", block, BS); ok = 0; }
    if (!p.nframes_stable || p.nframes_first != BS) {
        fprintf(stderr, "FAIL: nframes varied or was not the block size (first=%u, stable=%d)\n",
                p.nframes_first, p.nframes_stable); ok = 0;
    }
    if (!p.time_monotonic) { fprintf(stderr, "FAIL: system_time_ns not monotonic\n"); ok = 0; }
    if (!p.pos_monotonic)  { fprintf(stderr, "FAIL: sample_pos not monotonic\n");     ok = 0; }
    if (latency == 0) { fprintf(stderr, "FAIL: snd_pcm_delay reported no output latency\n"); ok = 0; }

    bwa_sink_health h;
    bwa_sink_get_health(s, &h);
    if (!h.measured) { fprintf(stderr, "FAIL: the write reports underruns directly, so measured must be true\n"); ok = 0; }
    if (h.blocks != blocks) {
        fprintf(stderr, "FAIL: health counted %llu blocks, the probe saw %lu\n",
                (unsigned long long)h.blocks, blocks); ok = 0;
    }
    if (h.device_lost) { fprintf(stderr, "FAIL: the device was reported lost during a clean run\n"); ok = 0; }
    if (h.period_ns == 0) { fprintf(stderr, "FAIL: no block period to measure the budget against\n"); ok = 0; }
    /* The honesty rule, and here it can be demanded in full rather than noted: -EPIPE is the
     * device's own report, the stall outlasted the buffer by a factor of two, and the frame
     * estimate is elapsed time minus that buffer, so both halves must be non-zero. */
    if (h.dropouts < 1) {
        fprintf(stderr, "FAIL: the render thread was stalled %u ms past a %u-frame buffer and no "
                        "underrun was counted\n", stall_ms, buffer); ok = 0;
    }
    if (h.dropouts >= 1 && h.dropped_frames == 0) {
        fprintf(stderr, "FAIL: an underrun was counted with 0 dropped frames\n"); ok = 0;
    }
    if (h.late_blocks < 1) {
        fprintf(stderr, "FAIL: a %u ms stall inside render() was not counted as a late block\n",
                stall_ms); ok = 0;
    }

    printf("alsa OK: backend=%s blocks=%lu block=%u period=%u buffer=%u latency=%u frames\n",
           backend, blocks, block, period, buffer, latency);
    printf("  measured=%d underruns=%llu dropped=%llu late=%llu resyncs=%llu (stall %u ms injected)\n",
           h.measured ? 1 : 0, (unsigned long long)h.dropouts, (unsigned long long)h.dropped_frames,
           (unsigned long long)h.late_blocks, (unsigned long long)h.driver_resyncs, stall_ms);
    bwa_sink_close(s);                            /* must return, not hang */

    /* --- rule 6: a rate the device cannot run must fail the open under exact_rate, naming BOTH
     *     rates. A plug, default or server PCM can genuinely accept almost any rate by converting,
     *     so a successful open there is REPORTED, not failed: the check does not apply to it. --- */
    {
        const uint32_t absurd = 999999;
        Probe q;
        probe_init(&q, 2);
        char e2[256] = {0};
        bwa_sink* bad = bwa_alsa_sink_open(absurd, BS, 2, dev, 0, true, on_render, &q, e2, sizeof e2);
        if (bad) {
            printf("  NOTE: PCM '%s' accepted %u Hz, so the exact-rate rejection is not applicable "
                   "to it (a plug or server PCM converts anything)\n", dev ? dev : "default", absurd);
            bwa_sink_close(bad);
        } else if (!rejects_naming_both(e2, absurd)) {
            /* Both: the rate that was asked for, and the rate the device would give instead.
             * Which rate the device names is its own business (whatever set_rate_near answered),
             * so the check is "a rate that is not the requested one", not a literal. */
            fprintf(stderr, "FAIL: the rate-mismatch message must name both the requested rate and "
                            "the device's; got: %s\n", e2[0] ? e2 : "(no message)");
            ok = 0;
        } else {
            printf("  rate mismatch rejected: %s\n", e2);
        }
    }
    return ok ? 0 : 1;
}
#endif /* BWA_HAVE_ALSA */

#ifdef BWA_HAVE_AAUDIO
/* The AAudio section. Same contract as the null sink above, against a live Android output stream:
 * a stable callback at a CONSTANT nframes equal to block_size (the adapter's whole job, since
 * setFramesPerDataCallback is a hint the OS may ignore), monotonic position and time, health that
 * says what it can actually observe, a deliberate stall that must show up in the OS's own xrun
 * count, a rate-mismatch open, and a stop and close that return.
 *
 * BWA_TEST_AAUDIO_DEVICE overrides the device id (the default is the default output), so one
 * binary reaches a headset's built-in output or a Bluetooth route without editing code.
 *
 * Returns 0 (ok), 1 (failed), or SKIP_EXIT when there is no output stream to open. */
static int test_aaudio(void) {
    const uint32_t SR = 48000, BS = 256;
    const char* dev = getenv("BWA_TEST_AAUDIO_DEVICE");
    if (dev && !*dev) dev = NULL;

    Probe p;
    probe_init(&p, 2);
    char err[256] = {0};

    bwa_sink* s = bwa_aaudio_sink_open(SR, BS, 2, dev, 0, false, on_render, &p, err, sizeof err);
    if (!s) {
        if (!err[0]) { fprintf(stderr, "FAIL: aaudio open failed with no message\n"); return 1; }
        /* "no usable AAudio output device" is a headless image and any box with the audio HAL
         * switched off. Anything else is a real failure: a device WITH audio that cannot open it
         * is exactly what this section exists to catch. */
        if (strstr(err, "no usable AAudio output device")) { printf("SKIP: %s\n", err); return SKIP_EXIT; }
        fprintf(stderr, "FAIL: aaudio open: %s\n", err);
        return 1;
    }
    /* Rule 6's resampler and a refused exclusive mode both ride in on a SUCCESSFUL open. Printed
     * rather than asserted: which of them applies is a property of the device, not of the sink. */
    if (err[0]) printf("aaudio: opened with a degradation reported: %s\n", err);

    const char* backend = bwa_sink_backend(s);
    const uint32_t block  = bwa_sink_block_size(s);
    const uint32_t burst  = sink_aaudio_burst_frames(s);
    const uint32_t buffer = sink_aaudio_buffer_frames(s);
    int ok = 1;
    if (bwa_sink_type_of(s) != BWA_SINK_AAUDIO) {
        fprintf(stderr, "FAIL: aaudio sink reports the wrong backend type\n");
        bwa_sink_close(s); return 1;
    }
    if (strncmp(backend, "aaudio:", 7) != 0) {
        fprintf(stderr, "FAIL: backend string is '%s', want \"aaudio:<device>\"\n", backend);
        bwa_sink_close(s); return 1;
    }
    if (bwa_sink_start(s) != 0) { fprintf(stderr, "FAIL: aaudio start\n"); bwa_sink_close(s); return 1; }

    /* --- phase 1: a healthy run ---
     *
     * The xrun count is REPORTED here, not asserted to be zero. Unlike the WASAPI and ALSA
     * sections there is no configuration flag that says whether a clean 200 ms was ever on offer:
     * the callback thread belongs to AAudio, and on a virtualized audio HAL (the emulator) or a
     * busy headset the service can legitimately run dry through no fault of the sink. What this
     * section CAN demand is that the injected stall below is seen, which is the assertion that
     * goes red if the xrun fold breaks. */
    os_sleep_ms(200);                                   /* ~37 blocks at 256/48000 = 5.33 ms */
    bwa_sink_health h1;
    bwa_sink_get_health(s, &h1);
    if (h1.dropouts != 0)
        printf("  NOTE: %llu xruns on the healthy 200 ms run; not asserted on this backend (see "
               "the comment).\n", (unsigned long long)h1.dropouts);
    if (p.blocks < 10) {
        fprintf(stderr, "FAIL: %lu blocks in 200 ms, want at least 10\n", p.blocks); ok = 0;
    }

    /* --- phase 2: starve it on purpose. Twice the buffer plus slack, so the stream certainly runs
     *     dry and the service certainly counts an underrun. --- */
    const unsigned stall_ms = (2u * (buffer ? buffer : BS) * 1000u / SR) + 40u;
    atomic_store_explicit(&p.stall_ms_once, (int)stall_ms, memory_order_relaxed);
    os_sleep_ms(200u + stall_ms);

    bwa_sink_stop(s);            /* waits out the stream state change: the callback is done */

    const unsigned long blocks = p.blocks;
    const uint32_t latency = bwa_sink_output_latency(s);
    if (block != BS) { fprintf(stderr, "FAIL: block_size() is %u, want the engine's %u\n", block, BS); ok = 0; }
    if (!p.nframes_stable || p.nframes_first != BS) {
        fprintf(stderr, "FAIL: nframes varied or was not the block size (first=%u, stable=%d)\n",
                p.nframes_first, p.nframes_stable); ok = 0;
    }
    if (!p.time_monotonic) { fprintf(stderr, "FAIL: system_time_ns not monotonic\n"); ok = 0; }
    if (!p.pos_monotonic)  { fprintf(stderr, "FAIL: sample_pos not monotonic\n");     ok = 0; }
    if (p.last_sample_pos != (uint64_t)BS * (blocks - 1)) {
        fprintf(stderr, "FAIL: sample_pos drift (last=%llu, expected=%llu)\n",
                (unsigned long long)p.last_sample_pos, (unsigned long long)((uint64_t)BS * (blocks - 1)));
        ok = 0;
    }
    if (latency == 0) { fprintf(stderr, "FAIL: no output latency reported\n"); ok = 0; }

    bwa_sink_health h;
    bwa_sink_get_health(s, &h);
    /* Rule 4's row: measured follows getTimestamp having answered. A stream that delivered tens of
     * blocks has presented frames, so a false here means the device never reported a timestamp at
     * all, which is a finding rather than a tolerance. */
    if (!h.measured) {
        fprintf(stderr, "FAIL: %lu blocks ran and getTimestamp never answered, so health reports "
                        "unmeasured\n", blocks); ok = 0;
    }
    if (h.blocks != blocks) {
        fprintf(stderr, "FAIL: health counted %llu blocks, the probe saw %lu\n",
                (unsigned long long)h.blocks, blocks); ok = 0;
    }
    if (h.device_lost) { fprintf(stderr, "FAIL: the device was reported lost during a clean run\n"); ok = 0; }
    if (h.period_ns == 0) { fprintf(stderr, "FAIL: no block period to measure the budget against\n"); ok = 0; }
    /* The half the adapter measures itself, so it is demandable on every backend. */
    if (h.late_blocks < 1) {
        fprintf(stderr, "FAIL: a %u ms stall inside render() was not counted as a late block\n",
                stall_ms); ok = 0;
    }
    /* The OS's own count. dropped_frames stays 0 by design on this backend: AAudio says how many
     * times the stream ran dry and never for how long, and an estimate from the callback interval
     * would be a guess dressed as a measurement. */
    if (h.dropouts <= h1.dropouts) {
        fprintf(stderr, "FAIL: the callback was stalled %u ms past a %u-frame buffer and "
                        "getXRunCount did not rise (%llu before, %llu after)\n",
                stall_ms, buffer, (unsigned long long)h1.dropouts, (unsigned long long)h.dropouts);
        ok = 0;
    }
    if (h.dropped_frames != 0) {
        fprintf(stderr, "FAIL: AAudio cannot measure the LENGTH of an underrun, so dropped_frames "
                        "must stay 0; got %llu\n", (unsigned long long)h.dropped_frames); ok = 0;
    }

    printf("aaudio OK: backend=%s blocks=%lu block=%u burst=%u buffer=%u latency=%u frames%s\n",
           backend, blocks, block, burst, buffer, latency,
           (burst == block) ? " (adapter in pass-through)" : "");
    printf("  measured=%d xruns=%llu dropped=%llu late=%llu resyncs=%llu (stall %u ms injected, "
           "%llu xruns before it)\n", h.measured ? 1 : 0, (unsigned long long)h.dropouts,
           (unsigned long long)h.dropped_frames, (unsigned long long)h.late_blocks,
           (unsigned long long)h.driver_resyncs, stall_ms, (unsigned long long)h1.dropouts);
    bwa_sink_close(s);                            /* must return, not hang */

    /* --- rule 6: a rate the device cannot run. AAudio's shared mode may legitimately convert, so
     *     an open that SUCCEEDS is acceptable only when it reported the degradation; one that
     *     fails must name both the rate that was asked for and the device's. --- */
    {
        const uint32_t absurd = 999999;
        Probe q;
        probe_init(&q, 2);
        char e2[256] = {0};
        bwa_sink* bad = bwa_aaudio_sink_open(absurd, BS, 2, dev, 0, false, on_render, &q, e2, sizeof e2);
        if (bad) {
            if (!e2[0]) {
                fprintf(stderr, "FAIL: a %u Hz open succeeded with no degradation reported; the OS "
                                "cannot be running the device at that rate\n", absurd);
                ok = 0;
            } else {
                printf("  rate mismatch accepted with a degradation: %s\n", e2);
            }
            bwa_sink_close(bad);
        } else if (!rejects_naming_both(e2, absurd)) {
            fprintf(stderr, "FAIL: the rate-mismatch message must name both the requested rate and "
                            "the device's; got: %s\n", e2[0] ? e2 : "(no message)");
            ok = 0;
        } else {
            printf("  rate mismatch rejected: %s\n", e2);
        }
    }
    return ok ? 0 : 1;
}
#endif /* BWA_HAVE_AAUDIO */

int main(void) {
    Probe p;
    probe_init(&p, BWA_CHANNELS);

    const uint32_t SR = 48000, BS = 256, CH = BWA_CHANNELS;
    char err[256] = {0};
    bwa_sink* s = bwa_null_sink_open(SR, BS, CH, on_render, &p, err, sizeof err);
    if (!s) { fprintf(stderr, "FAIL: open: %s\n", err[0] ? err : "(no message)"); return 1; }

    const char* backend = bwa_sink_backend(s);
    if (bwa_sink_start(s) != 0) { fprintf(stderr, "FAIL: start\n"); bwa_sink_close(s); return 1; }

    os_sleep_ms(200);                                  /* ~37 blocks at 256/48000 = 5.33 ms */
    bwa_sink_stop(s);                             /* joins the audio thread */

    /* thread is joined; reading the probe is race-free now */
    const unsigned long blocks = p.blocks;
    int ok = 1;
    if (blocks < 5)             { fprintf(stderr, "FAIL: too few blocks (%lu)\n", blocks); ok = 0; }
    if (!p.time_monotonic)      { fprintf(stderr, "FAIL: system_time_ns not monotonic\n"); ok = 0; }
    if (!p.pos_monotonic)       { fprintf(stderr, "FAIL: sample_pos not monotonic\n");     ok = 0; }
    if (p.last_sample_pos != (uint64_t)BS * (blocks - 1)) {
        fprintf(stderr, "FAIL: sample_pos drift (last=%llu, expected=%llu)\n",
                (unsigned long long)p.last_sample_pos, (unsigned long long)((uint64_t)BS * (blocks - 1)));
        ok = 0;
    }

    /* --- health, clean run --- */
    bwa_sink_health h;
    bwa_sink_get_health(s, &h);
    if (!h.measured)          { fprintf(stderr, "FAIL: a threaded sink with a deadline must report measured\n"); ok = 0; }
    if (h.blocks != blocks)   { fprintf(stderr, "FAIL: health counted %llu blocks, probe saw %lu\n",
                                        (unsigned long long)h.blocks, blocks); ok = 0; }
    if (h.dropouts != 0)      { fprintf(stderr, "FAIL: %llu dropouts on an uninjected run\n",
                                        (unsigned long long)h.dropouts); ok = 0; }
    if (h.dropped_frames != 0){ fprintf(stderr, "FAIL: frames dropped with no dropout\n"); ok = 0; }
    if (h.period_ns == 0)     { fprintf(stderr, "FAIL: no block period to measure the budget against\n"); ok = 0; }

    bwa_sink_close(s);
    if (!ok) return 1;

    if (!test_gap_rule())      return 1;
    if (!test_injected_drop()) return 1;
    if (!test_manual_unmeasured()) return 1;

    printf("audio sink OK: backend=%s blocks=%lu last_sample_pos=%llu last_ns=%llu\n",
           backend, blocks, (unsigned long long)p.last_sample_pos, (unsigned long long)p.last_ns);

    /* One section per compiled device backend, and the results AGGREGATE: a failure anywhere fails
     * the test, a section that actually ran makes the test a pass, and only "every device section
     * had no device" reports the ctest skip. Returning the first skip instead would hide a backend
     * that did run beside one that could not. */
    int device_ran = 0, device_skipped = 0;
#ifdef BWA_HAVE_WASAPI
    {
        const int rc = test_wasapi();
        if (rc == 1) return 1;
        if (rc == SKIP_EXIT) device_skipped = 1; else device_ran = 1;
    }
#endif
#ifdef BWA_HAVE_JACK
    {
        const int rc = test_jack();
        if (rc == 1) return 1;
        if (rc == SKIP_EXIT) device_skipped = 1; else device_ran = 1;
    }
#endif
#ifdef BWA_HAVE_ALSA
    {
        const int rc = test_alsa();
        if (rc == 1) return 1;
        if (rc == SKIP_EXIT) device_skipped = 1; else device_ran = 1;
    }
#endif
#ifdef BWA_HAVE_AAUDIO
    {
        const int rc = test_aaudio();
        if (rc == 1) return 1;
        if (rc == SKIP_EXIT) device_skipped = 1; else device_ran = 1;
    }
#endif
    if (!device_ran && device_skipped) return SKIP_EXIT;
    return 0;
}
