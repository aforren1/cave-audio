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
#include "sink/sink.h"

#include <stdio.h>
#include <stdlib.h>       /* getenv: every device section's device string is overridable */
#include <string.h>

#include "os/os.h"

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

/* Both subsections below open through bwa_sink_open rather than bwa_wasapi_sink_open, on purpose:
 * that is the path a caller's bwa_desc.sink_flags actually takes, and it is where
 * BWA_SINK_FLAG_EXACT_RATE is folded into the backend's exact_rate argument. Calling the backend
 * directly would test the sink and skip the policy. A named backend is a DEMAND, so there is no
 * silent null-sink fallback to mistake for a pass. */
static bwa_sink* wasapi_open(uint32_t sr, uint32_t bs, uint32_t flags, Probe* p,
                             char* err, size_t errcap) {
    err[0] = 0;
    return bwa_sink_open(sr, bs, 2, BWA_SINK_WASAPI, NULL, flags, false, on_render, p, err, errcap);
}

/* EXCLUSIVE MODE (BWA_SINK_FLAG_EXCLUSIVE). Three claims, none of which the shared-mode section
 * above can make:
 *   1. it is LOWER LATENCY than a shared open at the same block, which is the whole reason to pay
 *      for it (the endpoint leaves every other application on the machine);
 *   2. health.measured is TRUE, because exclusive mode uses sink_quant's device-position DEPTH
 *      rule rather than the shared release-interval rule;
 *   3. an injected stall trips that depth rule - one dropout, with a nonzero frame count.
 * Claim 3 is the one that had never been exercised anywhere: every earlier live dropout in this
 * file came through the shared-mode release-interval rule or the null sink's own accounting.
 *
 * A block size the endpoint refuses is PRINTED and skipped, not failed: an exclusive-mode device
 * period is the driver's to set, and a device that will not do 64 frames is not a defect here.
 *
 * Returns 0 (ok), 1 (failed), or SKIP_EXIT when there is no endpoint at all. */
static int test_wasapi_exclusive(void) {
    const uint32_t SR = 48000;
    const uint32_t SIZES[3] = { 256, 128, 64 };
    char err[256] = {0};

    Probe pb;
    probe_init(&pb, 2);
    bwa_sink* sh = wasapi_open(SR, 256, 0, &pb, err, sizeof err);
    if (!sh) {
        if (strstr(err, "no active render endpoint")) {
            printf("SKIP: wasapi exclusive: %s\n", err);
            return SKIP_EXIT;
        }
        fprintf(stderr, "FAIL: wasapi exclusive: the shared baseline open failed: %s\n", err);
        return 1;
    }
    const uint32_t shared_lat = bwa_sink_output_latency(sh);
    bwa_sink_close(sh);
    printf("wasapi exclusive: shared baseline at 256 = %u frames (%.1f ms)\n",
           shared_lat, 1000.0 * (double)shared_lat / (double)SR);

    int ok = 1, ran = 0;
    for (int i = 0; i < 3; ++i) {
        const uint32_t BS = SIZES[i];
        Probe p;
        probe_init(&p, 2);
        char e2[256] = {0};
        bwa_sink* s = wasapi_open(SR, BS, BWA_SINK_FLAG_EXCLUSIVE, &p, e2, sizeof e2);
        if (!s) {
            printf("  %3u frames: REFUSED - %s\n", BS, e2[0] ? e2 : "(no message)");
            continue;
        }
        ran = 1;
        const uint32_t lat  = bwa_sink_output_latency(s);
        const uint32_t devf = sink_wasapi_device_frames(s);
        const uint32_t perf = sink_wasapi_period_frames(s);
        printf("  %3u frames: latency=%u (%.1f ms) buffer=%u period=%u format=%s align-retry=%s\n",
               BS, lat, 1000.0 * (double)lat / (double)SR, devf, perf,
               sink_wasapi_format_name(s), sink_wasapi_align_retry(s) ? "yes" : "no");

        if (BS == 256 && lat >= shared_lat) {
            fprintf(stderr, "FAIL: exclusive at 256 reports %u frames, no better than shared's %u; "
                            "exclusive mode exists to be lower\n", lat, shared_lat);
            ok = 0;
        }
        if (bwa_sink_block_size(s) != BS) {
            fprintf(stderr, "FAIL: block_size() is %u, want the engine's %u\n",
                    bwa_sink_block_size(s), BS); ok = 0;
        }
        if (bwa_sink_start(s) != 0) {
            fprintf(stderr, "FAIL: exclusive start at %u frames\n", BS);
            bwa_sink_close(s); ok = 0; continue;
        }

        os_sleep_ms(200);
        bwa_sink_health h1;
        bwa_sink_get_health(s, &h1);
        if (h1.dropouts != 0) {
            fprintf(stderr, "FAIL: %llu dropouts on a healthy 200 ms exclusive run at %u frames\n",
                    (unsigned long long)h1.dropouts, BS); ok = 0;
        }

        /* The injected stall. "Two buffers plus slack" is the right size for the shared-mode rule
         * above and is NOT enough here, which is worth knowing: an exclusive-mode buffer on this
         * class of endpoint is one 3 ms period, and IAudioClock's position advances by only about
         * half the wall time the stream is starved - it counts frames READ FROM OUR BUFFER, and a
         * starved device is not reading any. A 36 ms stall over a 144-frame buffer therefore
         * produced a gap on one block size out of three, which reads as a flaky test rather than
         * as what it is. 150 ms is long enough that the partial advance still clears the buffer at
         * every size, measured. */
        const unsigned stall_ms = (2u * devf * 1000u / SR) + 150u;
        atomic_store_explicit(&p.stall_ms_once, (int)stall_ms, memory_order_relaxed);
        os_sleep_ms(200u + stall_ms);
        bwa_sink_stop(s);

        bwa_sink_health h;
        bwa_sink_get_health(s, &h);
        if (!p.nframes_stable || p.nframes_first != BS) {
            fprintf(stderr, "FAIL: nframes varied or was not the block size (first=%u, stable=%d)\n",
                    p.nframes_first, p.nframes_stable); ok = 0;
        }
        if (!p.time_monotonic) { fprintf(stderr, "FAIL: system_time_ns not monotonic\n"); ok = 0; }
        if (!p.pos_monotonic)  { fprintf(stderr, "FAIL: sample_pos not monotonic\n");     ok = 0; }
        /* Exclusive mode reads the device's own position every callback, so it can always judge.
         * An unmeasured exclusive stream means IAudioClock never answered. */
        if (!h.measured) {
            fprintf(stderr, "FAIL: an exclusive stream must report measured (the depth rule reads "
                            "IAudioClock every callback)\n"); ok = 0;
        }
        if (h.dropouts != 1) {
            fprintf(stderr, "FAIL: one %u ms stall past two buffers, but the depth rule counted "
                            "%llu dropouts (want exactly 1: it re-anchors to the device position "
                            "after each one)\n", stall_ms, (unsigned long long)h.dropouts); ok = 0;
        }
        if (h.dropouts >= 1 && h.dropped_frames == 0) {
            fprintf(stderr, "FAIL: the depth rule counted a dropout with 0 dropped frames; the gap "
                            "IS the frame count there, so zero means the arithmetic is wrong\n");
            ok = 0;
        }
        if (h.device_lost) { fprintf(stderr, "FAIL: the device was reported lost\n"); ok = 0; }
        printf("      stall %u ms -> dropouts=%llu dropped=%llu late=%llu measured=%d blocks=%lu\n",
               stall_ms, (unsigned long long)h.dropouts, (unsigned long long)h.dropped_frames,
               (unsigned long long)h.late_blocks, h.measured ? 1 : 0, p.blocks);
        bwa_sink_close(s);
    }

    if (!ran) {
        /* Every size refused. The endpoint exists, so this is not the no-device skip - but it is
         * also not a defect in this code, and saying "passed" would be a lie. */
        printf("SKIP: wasapi exclusive: this endpoint refused exclusive mode at 256, 128 and 64\n");
        return SKIP_EXIT;
    }
    return ok ? 0 : 1;
}

/* The first decimal run in a message, so a rate the sink REPORTED can be compared rather than
 * assumed. Both messages below open with the endpoint's own rate. 0 = no digits. */
static uint32_t first_uint(const char* s) {
    for (; s && *s; ++s)
        if (*s >= '0' && *s <= '9') return (uint32_t)strtoul(s, NULL, 10);
    return 0;
}

/* EXACT RATE (BWA_SINK_FLAG_EXACT_RATE), the strictness PsychPortAudio spells
 * paMacCoreFailIfConversionRequired / eStreamOptionMatchFormat. Shared mode's DEFAULT is to accept
 * the OS resampler and report the degradation (rule 6); with the flag the open must fail instead,
 * and the message must name BOTH rates, because "it did not work" is not actionable.
 *
 * ORDER MATTERS HERE, and the first draft of this test had it backwards. Asking the STRICT open
 * first and treating its success as "the endpoint takes that rate" cannot distinguish an endpoint
 * that takes both rates from a build where the flag is not wired up at all: unwiring the fold in
 * sink.c turned this test SKIP rather than red, which is the cannot-fail test CLAUDE.md warns
 * about. So the DEFAULT open goes first and its rule-6 degradation is the ground truth about what
 * the OS is resampling. Only a rate that open PROVED is resampled goes to the strict one, where a
 * success is then unambiguously a failure of the flag.
 *
 * Returns 0 (ok), 1 (failed), or SKIP_EXIT when there is no endpoint at all. */
static int test_wasapi_exact_rate(void) {
    /* Step 1: find a rate this endpoint does NOT take natively, from the default open's own
     * degradation report. 48 kHz is the engine's rate and the likeliest endpoint rate, so a
     * mismatch there is worth knowing about; 44.1 is the fallback probe. */
    const uint32_t PROBE[2] = { 48000, 44100 };
    uint32_t miss = 0, dev_rate = 0;
    char degraded[256] = {0};
    for (int i = 0; i < 2 && !miss; ++i) {
        Probe p;
        probe_init(&p, 2);
        char e[256] = {0};
        bwa_sink* s = wasapi_open(PROBE[i], 256, 0, &p, e, sizeof e);
        if (!s) {
            if (strstr(e, "no active render endpoint")) {
                printf("SKIP: wasapi exact-rate: %s\n", e);
                return SKIP_EXIT;
            }
            fprintf(stderr, "FAIL: a default (shared, resampling allowed) open at %u Hz must "
                            "succeed; it failed: %s\n", PROBE[i], e);
            return 1;
        }
        if (strstr(e, "resampling")) {      /* rule 6's degradation: this rate is NOT native */
            miss     = PROBE[i];
            dev_rate = first_uint(e);
            snprintf(degraded, sizeof degraded, "%s", e);
        }
        bwa_sink_close(s);
    }
    if (!miss) {
        /* Nothing to refuse: the endpoint takes both probe rates without conversion. Not a pass. */
        printf("SKIP: wasapi exact-rate: this endpoint takes 48000 and 44100 Hz natively in shared "
               "mode, so there is no resampled open to refuse\n");
        return SKIP_EXIT;
    }
    printf("wasapi exact-rate: the endpoint runs at %u Hz, so %u Hz is resampled by default\n",
           dev_rate, miss);
    printf("  without the flag: open OK, degraded - %s\n", degraded);

    /* Step 2: the SAME open with the flag must fail, naming both rates and the reason. */
    Probe p2;
    probe_init(&p2, 2);
    char e1[256] = {0};
    bwa_sink* bad = wasapi_open(miss, 256, BWA_SINK_FLAG_EXACT_RATE, &p2, e1, sizeof e1);
    if (bad) {
        bwa_sink_close(bad);
        fprintf(stderr, "FAIL: %u Hz was resampled on the open above, so BWA_SINK_FLAG_EXACT_RATE "
                        "must refuse it - the open succeeded instead, which means the flag never "
                        "reached the backend\n", miss);
        return 1;
    }
    int ok = 1;
    char want_miss[16], want_dev[16];
    snprintf(want_miss, sizeof want_miss, "%u", miss);
    snprintf(want_dev,  sizeof want_dev,  "%u", dev_rate);
    if (!strstr(e1, want_miss) || !dev_rate || !strstr(e1, want_dev)) {
        fprintf(stderr, "FAIL: the strict-open failure must name BOTH rates (the device's %u and "
                        "the engine's %u); it said: %s\n", dev_rate, miss, e1);
        ok = 0;
    }
    if (!strstr(e1, "must not resample")) {
        fprintf(stderr, "FAIL: the strict-open failure does not say why: %s\n", e1);
        ok = 0;
    }
    printf("  with the flag:    open REFUSED - %s\n", e1);
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
        /* And a box with no libjack at all, which is a SKIP for the same reason: the library is
         * loaded at run time now, so its absence is a missing device rather than a broken sink.
         * The UNAVAILABLE path itself is demanded by test_dl_unavailable, which needs no library. */
        if (strstr(err, "could not be loaded")) { printf("SKIP: %s\n", err); return SKIP_EXIT; }
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
 * blocks in the same 470 ms a paced device renders 76 in. It is good for the open sequence, the
 * format walk and the hw-params negotiation (alsa_tight_buffer uses it for exactly that), and
 * nothing else.
 *
 * Returns 0 (ok), 1 (failed), or SKIP_EXIT when there is no PCM to open. */

/* BWA_SINK_FLAG_TIGHT_BUFFER on one PCM: open it twice, plain and tight, and compare what the
 * device granted. Returns 1 for ok, 0 for a failure, and prints either way.
 *
 * `hard` is the difference between a claim and a report, and it exists because a SERVER PCM can
 * refuse: the ALSA pulse plugin returns three periods whether or not the smaller buffer is asked
 * for, measured, so demanding two on whatever PCM the box happens to offer would fail a correct
 * sink. The hardware-free "null" PCM has no such constraint (nothing paces it, so nothing bounds
 * its buffer) and every ALSA install has one, so that is where the claim is demanded - the same
 * split the section header describes for the format walk. One rule is hard on every PCM: a tight
 * open must never come back DEEPER than the default one. */
static int alsa_tight_buffer(const char* pcm, int hard, uint32_t SR, uint32_t BS) {
    Probe q0, q1;
    probe_init(&q0, 2);
    probe_init(&q1, 2);
    char e0[256] = {0}, e1[256] = {0};
    bwa_sink* a = bwa_sink_open(SR, BS, 2, BWA_SINK_ALSA, pcm, 0, false, on_render, &q0, e0, sizeof e0);
    bwa_sink* b = bwa_sink_open(SR, BS, 2, BWA_SINK_ALSA, pcm, BWA_SINK_FLAG_TIGHT_BUFFER,
                                false, on_render, &q1, e1, sizeof e1);
    int ok = 1;
    if (!a || !b) {
        /* The default open already succeeded once for the section's own PCM, and "null" is always
         * there, so a failure here is real either way. */
        fprintf(stderr, "FAIL: PCM '%s' refused the %s open: %s\n", pcm ? pcm : "default",
                a ? "tight-buffer" : "plain", (a ? e1 : e0)[0] ? (a ? e1 : e0) : "(no message)");
        ok = 0;
    } else {
        const uint32_t p0 = sink_alsa_period_frames(a), b0 = sink_alsa_buffer_frames(a);
        const uint32_t p1 = sink_alsa_period_frames(b), b1 = sink_alsa_buffer_frames(b);
        printf("  tight buffer on '%s': period=%u buffer=%u (plain: period=%u buffer=%u)\n",
               pcm ? pcm : "default", p1, b1, p0, b0);
        if (b1 > b0) {
            fprintf(stderr, "FAIL: TIGHT_BUFFER made the buffer DEEPER on '%s' (%u frames against "
                            "the plain open's %u)\n", pcm ? pcm : "default", b1, b0); ok = 0;
        }
        if (p1 == 0 || b1 != 2u * p1) {
            if (hard) {
                fprintf(stderr, "FAIL: TIGHT_BUFFER must settle on two periods on '%s'; it reports "
                                "buffer=%u against period=%u\n", pcm ? pcm : "default", b1, p1);
                ok = 0;
            } else {
                printf("      NOTE: this PCM did not grant two periods (buffer=%u, period=%u); a "
                       "server PCM bounds its own buffer\n", b1, p1);
            }
        }
    }
    if (a) bwa_sink_close(a);
    if (b) bwa_sink_close(b);
    return ok;
}

/* The ALSA section proper. Returns 0 (ok), 1 (failed), or SKIP_EXIT when there is no PCM. */
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
        /* No libasound is a SKIP for the same reason "no PCM" is: the library loads at run time,
         * so its absence is a missing device. test_dl_unavailable demands that path itself. */
        if (strstr(err, "could not be loaded")) { printf("SKIP: %s\n", err); return SKIP_EXIT; }
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

    /* --- BWA_SINK_FLAG_TIGHT_BUFFER: three periods of buffer become two. Read back from the sink
     *     rather than assumed, because set_buffer_size_near is a REQUEST and the device rounds it
     *     to whatever it can do. Run twice: once on the section's real PCM, where the result is
     *     REPORTED, and once on "null", where it is DEMANDED. See alsa_tight_buffer. --- */
    if (!alsa_tight_buffer(dev, 0, SR, BS)) ok = 0;
    if (!alsa_tight_buffer("null", 1, SR, BS)) ok = 0;

    /* --- BWA_SINK_FLAG_EXACT_RATE: the flag must reach the backend as exact_rate does. The
     *     assertion is that the two AGREE at an absurd rate, which is what goes red if the fold in
     *     sink.c is removed: without it the flag open falls back on the plug PCM's converter and
     *     succeeds while the argument open refuses. A hw: PCM refuses both, which agrees too. --- */
    {
        const uint32_t absurd = 999999;
        Probe q1, q2;
        probe_init(&q1, 2);
        probe_init(&q2, 2);
        char ea[256] = {0}, eb[256] = {0};
        bwa_sink* by_arg  = bwa_alsa_sink_open(absurd, BS, 2, dev, 0, true, on_render, &q1, ea, sizeof ea);
        bwa_sink* by_flag = bwa_sink_open(absurd, BS, 2, BWA_SINK_ALSA, dev,
                                          BWA_SINK_FLAG_EXACT_RATE, false, on_render, &q2, eb, sizeof eb);
        if ((by_arg != NULL) != (by_flag != NULL)) {
            fprintf(stderr, "FAIL: BWA_SINK_FLAG_EXACT_RATE and the exact_rate argument disagree at "
                            "%u Hz (argument %s, flag %s); the flag is not reaching the backend\n",
                    absurd, by_arg ? "opened" : "refused", by_flag ? "opened" : "refused");
            ok = 0;
        } else if (!by_flag) {
            if (!rejects_naming_both(eb, absurd)) {
                fprintf(stderr, "FAIL: the flag's rejection must name both rates; got: %s\n",
                        eb[0] ? eb : "(no message)");
                ok = 0;
            } else {
                printf("  exact-rate flag rejected %u Hz: %s\n", absurd, eb);
            }
        } else {
            printf("  NOTE: PCM '%s' accepts %u Hz either way, so the flag has nothing to refuse\n",
                   dev ? dev : "default", absurd);
        }
        if (by_arg)  bwa_sink_close(by_arg);
        if (by_flag) bwa_sink_close(by_flag);
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

    /* --- BWA_SINK_FLAG_TIGHT_BUFFER: one burst instead of two, read back with
     *     AAudioStream_getBufferSizeInFrames. The device can refuse to go that low (the service
     *     clamps against the stream's capacity), so a REFUSED request is printed rather than
     *     failed - what must hold is that it did not come back DEEPER than the default. Opened
     *     through bwa_sink_open, the path a caller's bwa_desc.sink_flags actually takes. --- */
    {
        Probe q;
        probe_init(&q, 2);
        char e3[256] = {0};
        bwa_sink* t = bwa_sink_open(SR, BS, 2, BWA_SINK_AAUDIO, dev, BWA_SINK_FLAG_TIGHT_BUFFER,
                                    false, on_render, &q, e3, sizeof e3);
        if (!t) {
            fprintf(stderr, "FAIL: the same device refused a tight-buffer open: %s\n",
                    e3[0] ? e3 : "(no message)");
            ok = 0;
        } else {
            const uint32_t tburst  = sink_aaudio_burst_frames(t);
            const uint32_t tbuffer = sink_aaudio_buffer_frames(t);
            printf("  tight buffer: burst=%u buffer=%u frames (default was burst=%u buffer=%u)\n",
                   tburst, tbuffer, burst, buffer);
            if (tbuffer > buffer) {
                fprintf(stderr, "FAIL: TIGHT_BUFFER asked for one burst and got %u frames, DEEPER "
                                "than the default open's %u\n", tbuffer, buffer); ok = 0;
            }
            /* The demandable half. Where the default open actually got the two bursts it asked
             * for, the tight one must come back SHALLOWER - otherwise the flag reached nothing.
             * Where the service had already clamped the default to one burst there is nothing
             * left to give, and that is reported instead of failed. */
            if (buffer > burst && tbuffer >= buffer) {
                fprintf(stderr, "FAIL: the default open got %u frames (burst %u) and the tight one "
                                "got %u; TIGHT_BUFFER bought nothing\n", buffer, burst, tbuffer);
                ok = 0;
            } else if (buffer <= burst) {
                printf("  NOTE: the default open was already one burst deep (%u), so there is "
                       "nothing for TIGHT_BUFFER to shrink on this device\n", buffer);
            }
            if (tburst && tbuffer != tburst) {
                printf("  NOTE: the service did not grant exactly one burst (%u); it settled on "
                       "%u frames\n", tburst, tbuffer);
            }
            bwa_sink_close(t);
        }
    }

    /* --- BWA_SINK_FLAG_EXACT_RATE: the rate open above was ACCEPTED with a degradation, which is
     *     rule 6's default. The flag must turn that same open into a refusal, checked against the
     *     device-rate probe rather than the stream's own report (a shared-mode stream reports the
     *     rate it was asked for whether or not the service is resampling). --- */
    {
        const uint32_t absurd = 999999;
        Probe q;
        probe_init(&q, 2);
        char e4[256] = {0};
        bwa_sink* strict = bwa_sink_open(absurd, BS, 2, BWA_SINK_AAUDIO, dev,
                                         BWA_SINK_FLAG_EXACT_RATE, false, on_render, &q, e4, sizeof e4);
        if (strict) {
            fprintf(stderr, "FAIL: BWA_SINK_FLAG_EXACT_RATE must refuse a %u Hz open on a device "
                            "that does not run at that rate; it opened instead\n", absurd);
            bwa_sink_close(strict);
            ok = 0;
        } else if (!rejects_naming_both(e4, absurd)) {
            fprintf(stderr, "FAIL: the flag's rejection must name both rates; got: %s\n",
                    e4[0] ? e4 : "(no message)");
            ok = 0;
        } else {
            printf("  exact-rate flag rejected %u Hz: %s\n", absurd, e4);
        }
    }
    return ok ? 0 : 1;
}
#endif /* BWA_HAVE_AAUDIO */

#if defined(BWA_HAVE_JACK) || defined(BWA_HAVE_ALSA)
/* ---- the UNAVAILABLE path: a Linux backend whose library will not load ----------------------
 *
 * The two Linux backends dlopen their libraries at run time (jack_sink.c, alsa_sink.c), so a box
 * with neither still loads libbw_audio.so and runs the offline sink. That path is the interesting
 * one and it is exactly the one a developer box cannot reach, because a developer box HAS the
 * libraries. bwa_sink_dl_override (sink.h, a dll-exported test hook, not public ABI) re-aims both
 * loaders at a soname that cannot exist, which is the same failure to the sink as a missing
 * library.
 *
 * WHAT IS DEMANDED, per backend: the device count is 0, an explicit open fails with a message
 * NAMING the library, and clearing the override makes the next open find the real one again. The
 * retry assertion holds on a box with the libraries and on a box without: it demands only that the
 * message stop naming the override, and a loader that ignored the override would already have
 * failed the assertion above it.
 *
 * ONE HOOK SERVES BOTH backends on purpose, which is why the AUTO check below lands on the null
 * sink rather than on the other Linux backend: with neither library loadable there is no next
 * backend to fall through TO. What it proves is the part that matters, that the reason survives
 * the fall-through instead of being replaced by silence. */
#define DL_NOWHERE "libbwa_no_such_library.so.999"

static int dl_unavailable_backend(bwa_sink_type type, const char* label, const char* soname,
                                  bwa_sink* (*open_fn)(uint32_t, uint32_t, uint32_t, const char*,
                                                       uint32_t, bool, bwa_render_fn, void*,
                                                       char*, size_t)) {
    const uint32_t SR = 48000, BS = 256;
    Probe q; probe_init(&q, 2);
    int ok = 1;

    bwa_sink_dl_override = DL_NOWHERE;

    const uint32_t n = sink_device_count(type);
    if (n != 0) {
        fprintf(stderr, "FAIL: %s reports %u devices with no library loaded\n", label, n);
        ok = 0;
    }

    char err[256] = {0};
    bwa_sink* s = open_fn(SR, BS, 2, NULL, 0, false, on_render, &q, err, sizeof err);
    if (s) {
        fprintf(stderr, "FAIL: %s opened against " DL_NOWHERE "\n", label);
        bwa_sink_close(s);
        ok = 0;
    } else if (!strstr(err, DL_NOWHERE)) {
        fprintf(stderr, "FAIL: %s failed without naming the library it could not load: %s\n",
                label, err[0] ? err : "(no message)");
        ok = 0;
    } else {
        printf("%s unavailable: %s\n", label, err);
    }

    /* An explicitly NAMED backend is a demand, so the failure must reach the caller through
     * bwa_sink_open too rather than being softened into the null sink. */
    char err2[256] = {0};
    bwa_sink* named = bwa_sink_open(SR, BS, 2, type, NULL, 0, false, on_render, &q,
                                    err2, sizeof err2);
    if (named) {
        fprintf(stderr, "FAIL: an explicit %s open succeeded with no library\n", label);
        bwa_sink_close(named);
        ok = 0;
    } else if (!strstr(err2, DL_NOWHERE)) {
        fprintf(stderr, "FAIL: the explicit %s failure lost the library name: %s\n",
                label, err2[0] ? err2 : "(no message)");
        ok = 0;
    }

    /* THE RETRY. The loader caches by soname and re-resolves when it changes, so a library
     * installed after a failed attempt is found on the next open. Clearing the hook is the same
     * event. A box with no real library still fails here - it just has to stop blaming the
     * override, and it names `soname` instead. */
    bwa_sink_dl_override = NULL;
    char err3[256] = {0};
    bwa_sink* again = open_fn(SR, BS, 2, NULL, 0, false, on_render, &q, err3, sizeof err3);
    if (again) {
        bwa_sink_close(again);
        printf("%s retry: the real %s loaded and a sink opened\n", label, soname);
    } else if (strstr(err3, DL_NOWHERE)) {
        fprintf(stderr, "FAIL: %s did not retry after the override was cleared: %s\n", label, err3);
        ok = 0;
    } else {
        printf("%s retry: back to the real loader (%s)\n", label, err3[0] ? err3 : "(no message)");
    }
    return ok;
}

static int test_dl_unavailable(void) {
    int ok = 1;
#ifdef BWA_HAVE_JACK
    if (!dl_unavailable_backend(BWA_SINK_JACK, "jack", "libjack.so.0", bwa_jack_sink_open)) ok = 0;
#endif
#ifdef BWA_HAVE_ALSA
    if (!dl_unavailable_backend(BWA_SINK_ALSA, "alsa", "libasound.so.2", bwa_alsa_sink_open)) ok = 0;
#endif

    /* AUTO with neither library loadable: the null sink, and the first backend's reason carried
     * out on the degradation channel. A silent null sink here would read exactly like working
     * hardware that happens to be muted. */
    {
        Probe q; probe_init(&q, 2);
        char err[256] = {0};
        bwa_sink_dl_override = DL_NOWHERE;
        bwa_sink* s = bwa_sink_open(48000, 256, 2, BWA_SINK_AUTO, NULL, 0, false, on_render, &q,
                                    err, sizeof err);
        bwa_sink_dl_override = NULL;
        if (!s) {
            fprintf(stderr, "FAIL: AUTO opened nothing at all with the Linux libraries absent\n");
            ok = 0;
        } else {
            if (bwa_sink_type_of(s) != BWA_SINK_NULL) {
                fprintf(stderr, "FAIL: AUTO did not fall through to the offline sink (%s)\n",
                        bwa_sink_backend(s));
                ok = 0;
            }
            if (!strstr(err, DL_NOWHERE)) {
                fprintf(stderr, "FAIL: AUTO dropped the reason it fell through: %s\n",
                        err[0] ? err : "(no message)");
                ok = 0;
            } else {
                printf("auto fall-through kept the reason: %s\n", err);
            }
            bwa_sink_close(s);
        }
    }
    return ok;
}
#endif /* BWA_HAVE_JACK || BWA_HAVE_ALSA */

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
#if defined(BWA_HAVE_JACK) || defined(BWA_HAVE_ALSA)
    /* Before the live sections: this one needs no device, so its verdict should not depend on
     * whether a server or a card happens to be up on the machine running the suite. */
    if (!test_dl_unavailable()) return 1;
#endif

    int device_ran = 0, device_skipped = 0;
#ifdef BWA_HAVE_WASAPI
    {
        const int rc = test_wasapi();
        if (rc == 1) return 1;
        if (rc == SKIP_EXIT) device_skipped = 1; else device_ran = 1;
    }
    {
        const int rc = test_wasapi_exclusive();
        if (rc == 1) return 1;
        if (rc == SKIP_EXIT) device_skipped = 1; else device_ran = 1;
    }
    {
        const int rc = test_wasapi_exact_rate();
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
