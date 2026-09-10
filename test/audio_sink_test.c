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

#ifdef BWA_HAVE_WASAPI
    {
        const int rc = test_wasapi();
        if (rc != 0) return rc;   /* 1 = failed, SKIP_EXIT = no endpoint on this machine */
    }
#endif
    return 0;
}
