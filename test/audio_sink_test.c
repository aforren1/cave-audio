/*
 * audio_sink_test.c — M1 verification of the audio loop without hardware. Drives the
 * null sink (the offline backend) and asserts the heart of M1's "done when": a stable
 * callback fires, and the timestamp (sample position + system time) advances
 * monotonically. The ASIO backend shares the same render contract; only the device
 * differs, so this exercises the engine-facing seam directly.
 */
#include "sink.h"

#include <stdio.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct {
    unsigned long blocks;
    uint64_t      last_sample_pos;
    uint64_t      last_ns;
    uint64_t      prev_ns;
    int           first;
    int           time_monotonic;
    int           pos_monotonic;
    uint64_t      prev_pos;
} Probe;

static void on_render(void* user, float* bus, uint32_t nframes, const bwa_timestamp* ts) {
    Probe* p = (Probe*)user;
    /* prove the bus is writable for the full block (the engine would mix here) */
    memset(bus, 0, sizeof(float) * (size_t)nframes * BWA_CHANNELS);

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
    memset(&p, 0, sizeof p);
    p.first = 1; p.time_monotonic = 1; p.pos_monotonic = 1;

    const uint32_t SR = 48000, BS = 256, SKIP = 3;
    char err[256] = {0};
    bwa_sink* s = bwa_null_sink_open(SR, BS, BWA_CHANNELS, on_render, &p, err, sizeof err);
    if (!s) { fprintf(stderr, "FAIL: inject open: %s\n", err[0] ? err : "(no message)"); return 0; }
    if (bwa_sink_start(s) != 0) { fprintf(stderr, "FAIL: inject start\n"); bwa_sink_close(s); return 0; }

    Sleep(50);
    bwa_null_sink_skip_blocks = (long)SKIP;     /* the device runs on for 3 blocks without us */
    Sleep(100);
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
    memset(&p, 0, sizeof p);
    p.first = 1;
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

int main(void) {
    Probe p;
    memset(&p, 0, sizeof p);
    p.first = 1; p.time_monotonic = 1; p.pos_monotonic = 1;

    const uint32_t SR = 48000, BS = 256, CH = BWA_CHANNELS;
    char err[256] = {0};
    bwa_sink* s = bwa_null_sink_open(SR, BS, CH, on_render, &p, err, sizeof err);
    if (!s) { fprintf(stderr, "FAIL: open: %s\n", err[0] ? err : "(no message)"); return 1; }

    const char* backend = bwa_sink_backend(s);
    if (bwa_sink_start(s) != 0) { fprintf(stderr, "FAIL: start\n"); bwa_sink_close(s); return 1; }

    Sleep(200);                                  /* ~37 blocks at 256/48000 = 5.33 ms */
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
    return 0;
}
