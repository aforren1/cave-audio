/*
 * sink_quant_test.c — the fixed-quantum adapter (src/sink/sink_quant.c).
 *
 * Every backend past ASIO leans on this, and none of them can be tested on CI, so the adapter
 * carries the weight alone. The design that makes it testable: the render writes a value derived
 * from the TIMESTAMP, not from a call counter, and the device side checks each frame against the
 * value its own stream index implies. One comparison then pins three separate claims at once —
 * the blocks concatenate in order, nothing is duplicated or dropped, and each block's sample_pos
 * really is the device frame index its first sample lands on. A wrong stamp does not merely fail
 * a stamp assertion, it corrupts the audio, which is what a wrong stamp does in the field.
 *
 * Per CLAUDE.md: the pop arithmetic was broken on purpose (rd_off advanced by run + 1) and this
 * test went red on the concatenation check before being trusted green.
 */
#include "sink/sink_quant.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL: "); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); fails++; } } while (0)

enum { BLOCK = 64, CHANS = 3, MAXREQ = 512, OUTCAP = 1 << 16 };

/* The value the frame at device stream index `pos` on channel `c` must carry. Distinct per
 * (pos, channel) and exact in float, so any shift, repeat, or channel swap shows up. */
static float expect(uint64_t pos, uint32_t c) { return (float)(pos * 8u + c) + 0.5f; }

typedef struct {
    /* render side */
    uint32_t nframes_seen[4096];    /* every render's nframes: all must equal BLOCK */
    uint64_t stamp_pos[4096];       /* every render's ts.sample_pos                 */
    uint64_t stamp_ns[4096];
    uint32_t nrenders;

    /* device side: the frames actually handed over, concatenated, planar per channel */
    float    out[CHANS][OUTCAP];
    uint32_t nout;
} Probe;

static Probe g;

static void render(void* user, float* bus, uint32_t nframes, const bwa_timestamp* ts) {
    Probe* p = (Probe*)user;
    if (p->nrenders < 4096) {
        p->nframes_seen[p->nrenders] = nframes;
        p->stamp_pos[p->nrenders]    = ts->sample_pos;
        p->stamp_ns[p->nrenders]     = ts->system_time_ns;
    }
    p->nrenders++;
    for (uint32_t c = 0; c < CHANS; ++c)
        for (uint32_t i = 0; i < nframes; ++i)
            bus[(size_t)c * nframes + i] = expect(ts->sample_pos + i, c);
}

/* The device's side of the seam: append the run to the output log exactly as a backend would
 * convert it into its device buffer. `frame_offset` is checked too, since a backend that
 * ignored it would overwrite the front of its buffer with every run. */
static void collect(void* user, const float* planar, uint32_t stride,
                    uint32_t frame_offset, uint32_t nframes) {
    Probe* p = (Probe*)user;
    for (uint32_t c = 0; c < CHANS; ++c)
        for (uint32_t i = 0; i < nframes; ++i) {
            const uint32_t at = p->nout + frame_offset + i;
            if (at < OUTCAP) p->out[c][at] = planar[(size_t)c * stride + i];
        }
}

/* Run one sequence of device requests through a fresh adapter and check every invariant that
 * does not depend on the sequence itself. Returns the adapter so the caller can read its
 * counters. `device_pos` tracks the device consuming everything it was handed. */
static void run_sequence(SinkQuant* q, const uint32_t* req, uint32_t nreq, bool report_pos) {
    memset(&g, 0, sizeof g);
    CHECK(sink_quant_init(q, 48000, BLOCK, CHANS, MAXREQ, render, &g) == 0, "quant init");

    uint64_t device_pos = 0;
    for (uint32_t r = 0; r < nreq; ++r) {
        const uint32_t n = req[r];
        sink_quant_pull(q, n, device_pos, report_pos, sink_quant_now_ns(), collect, &g);
        g.nout     += n;
        device_pos += n;                    /* a device that keeps up: it consumed what we wrote */
    }

    /* 1. Every render call saw exactly the engine block. This is the whole point of the adapter. */
    for (uint32_t i = 0; i < g.nrenders && i < 4096; ++i)
        CHECK(g.nframes_seen[i] == BLOCK, "render %u got nframes=%u, want %u",
              i, g.nframes_seen[i], (uint32_t)BLOCK);

    /* 2. Each block's sample_pos is the device frame index its first sample landed on, and the
     *    stamps advance by exactly one block. */
    for (uint32_t i = 0; i < g.nrenders && i < 4096; ++i)
        CHECK(g.stamp_pos[i] == (uint64_t)i * BLOCK, "render %u sample_pos=%llu, want %llu",
              i, (unsigned long long)g.stamp_pos[i], (unsigned long long)((uint64_t)i * BLOCK));

    /* 3. The device output is the exact concatenation of the rendered blocks: frame d carries
     *    what the block covering stream index d rendered for it. Nothing repeated, nothing lost. */
    for (uint32_t c = 0; c < CHANS; ++c)
        for (uint32_t d = 0; d < g.nout && d < OUTCAP; ++d)
            if (g.out[c][d] != expect(d, c)) {
                CHECK(0, "device frame %u ch%u = %f, want %f (stream discontinuity)",
                      d, c, (double)g.out[c][d], (double)expect(d, c));
                d = g.nout;                 /* one report is enough; do not flood */
            }
}

/* Blocks rendered inside ONE device callback must not share a host time. Reusing it feeds the
 * device-versus-host drift fit (bwa_get_clock_model) a pair with zero slope, which is worse than
 * no pair at all: it drags the fit toward a rate the device is not running at. */
static void test_stamp_spacing_within_one_pull(void) {
    SinkQuant q;
    memset(&g, 0, sizeof g);
    CHECK(sink_quant_init(&q, 48000, BLOCK, CHANS, MAXREQ, render, &g) == 0, "init");

    const uint64_t host = 1000000000ull;
    sink_quant_pull(&q, BLOCK * 3, 0, false, host, collect, &g);   /* 3 blocks, one callback */
    CHECK(g.nrenders == 3, "one pull of 3 blocks rendered %u", g.nrenders);
    CHECK(g.stamp_ns[0] == host, "first block stamps the callback's own host time");
    const uint64_t block_ns = (uint64_t)BLOCK * 1000000000ull / 48000ull;
    for (uint32_t i = 1; i < 3; ++i)
        CHECK(g.stamp_ns[i] == host + (uint64_t)i * block_ns,
              "block %u stamp %llu, want %llu (one nominal block apart)", i,
              (unsigned long long)g.stamp_ns[i], (unsigned long long)(host + (uint64_t)i * block_ns));
    sink_quant_free(&q);
}

/* The stamp must never step BACKWARD, and the only way to make that happen is the one a device
 * does after a fault: a callback that rendered several blocks extrapolated their times forward,
 * and the catch-up callback then arrives with a host time EARLIER than that extrapolation. A
 * negative interval against a forward sample position poisons the drift fit
 * (bwa_get_clock_model) and makes bwa_get_clock run backward.
 *
 * This is a REGRESSION test: the live WASAPI section caught it on a deliberately starved stream
 * before the guard existed. Equal stamps are not acceptable either - two sample positions sharing
 * a host time is the zero-slope pair rule 3 warns about. */
static void test_stamps_never_go_backward(void) {
    SinkQuant q;
    memset(&g, 0, sizeof g);
    CHECK(sink_quant_init(&q, 48000, BLOCK, CHANS, MAXREQ, render, &g) == 0, "init");

    const uint64_t host = 1000000000ull;
    /* One pull of four blocks: the last three are stamped ahead of `host`. */
    sink_quant_pull(&q, BLOCK * 4, 0, false, host, collect, &g);
    g.nout += BLOCK * 4;
    /* Then the catch-up: the device fires again at once, so this callback's host time is BEHIND
     * where the previous batch was extrapolated to. Deliberately earlier still, to leave no doubt. */
    sink_quant_pull(&q, BLOCK * 2, 0, false, host - 1000000ull, collect, &g);
    g.nout += BLOCK * 2;
    sink_quant_pull(&q, BLOCK, 0, false, host, collect, &g);

    CHECK(g.nrenders == 7, "expected 7 blocks, rendered %u", g.nrenders);
    for (uint32_t i = 1; i < g.nrenders && i < 4096; ++i)
        CHECK(g.stamp_ns[i] > g.stamp_ns[i - 1],
              "block %u stamp %llu is not AFTER block %u's %llu", i,
              (unsigned long long)g.stamp_ns[i], i - 1, (unsigned long long)g.stamp_ns[i - 1]);
    /* And the sample positions kept marching forward the whole time, so the pairs stay usable. */
    for (uint32_t i = 1; i < g.nrenders && i < 4096; ++i)
        CHECK(g.stamp_pos[i] == g.stamp_pos[i - 1] + BLOCK, "sample_pos gap at block %u", i);
    sink_quant_free(&q);
}

/* A backend can book a dropout it detected some other way than the queued-depth rule (WASAPI
 * shared mode measures the release interval). It must land on the same counters, and an absurd
 * frame estimate must be clamped rather than believed. */
static void test_backend_reported_dropout(void) {
    SinkQuant q;
    memset(&g, 0, sizeof g);
    CHECK(sink_quant_init(&q, 48000, BLOCK, CHANS, MAXREQ, render, &g) == 0, "init");

    bwa_sink_health h;
    sink_quant_health(&q, &h);
    CHECK(!h.measured, "a fresh adapter has observed nothing");

    sink_quant_note_dropout(&q, 900);
    sink_quant_health(&q, &h);
    CHECK(h.dropouts == 1 && h.dropped_frames == 900, "backend dropout: %llu / %llu frames",
          (unsigned long long)h.dropouts, (unsigned long long)h.dropped_frames);
    CHECK(h.measured, "a backend that reports a dropout can evidently observe one");

    /* Past the sane window the EVENT still counts - the backend saw a real fault - but the
     * unbelievable frame count does not, exactly as sink_position_gap refuses an absurd jump. */
    const uint64_t sane = (uint64_t)BLOCK * 4096ull;
    sink_quant_note_dropout(&q, sane * 100ull);
    sink_quant_health(&q, &h);
    CHECK(h.dropouts == 2, "the second event must count");
    CHECK(h.dropped_frames == 900 + sane, "an absurd estimate must clamp to the window, got %llu",
          (unsigned long long)h.dropped_frames);
    sink_quant_free(&q);
}

/* The dropout rule: the device's own position running PAST what the adapter wrote is audio it
 * clocked out that nobody rendered. Only a device position can show this, so `measured` follows
 * it, and a run that never reported one must say measured = false rather than "0 dropouts". */
static void test_dropout_and_measured(void) {
    SinkQuant q;
    memset(&g, 0, sizeof g);
    CHECK(sink_quant_init(&q, 48000, BLOCK, CHANS, MAXREQ, render, &g) == 0, "init");

    uint64_t device_pos = 0;
    for (int i = 0; i < 4; ++i) {
        sink_quant_pull(&q, BLOCK, device_pos, true, sink_quant_now_ns(), collect, &g);
        g.nout += BLOCK; device_pos += BLOCK;
    }
    bwa_sink_health h;
    sink_quant_health(&q, &h);
    CHECK(h.dropouts == 0, "clean run reported %llu dropouts", (unsigned long long)h.dropouts);
    CHECK(h.measured, "a device that reports its position is measurable");

    device_pos += BLOCK * 2;              /* the device ran on for two blocks without us */
    sink_quant_pull(&q, BLOCK, device_pos, true, sink_quant_now_ns(), collect, &g);
    g.nout += BLOCK; device_pos += BLOCK;
    sink_quant_health(&q, &h);
    CHECK(h.dropouts == 1, "one gap, counted %llu", (unsigned long long)h.dropouts);
    CHECK(h.dropped_frames == (uint64_t)BLOCK * 2, "gap of %llu frames, want %u",
          (unsigned long long)h.dropped_frames, BLOCK * 2);

    /* Re-anchored: the next callback compares against the DEVICE's position, so a device that
     * keeps up from here on must not produce a second dropout from the same gap. */
    sink_quant_pull(&q, BLOCK, device_pos, true, sink_quant_now_ns(), collect, &g);
    g.nout += BLOCK;
    sink_quant_health(&q, &h);
    CHECK(h.dropouts == 1, "the gap was counted twice (%llu)", (unsigned long long)h.dropouts);
    CHECK(h.period_ns == (uint64_t)BLOCK * 1000000000ull / 48000ull, "period_ns");
    CHECK(h.blocks == q.h_blocks && h.blocks > 0, "blocks counted");
    sink_quant_free(&q);

    /* The other half of the honesty rule: no device position, no claim. */
    memset(&g, 0, sizeof g);
    CHECK(sink_quant_init(&q, 48000, BLOCK, CHANS, MAXREQ, render, &g) == 0, "init");
    for (int i = 0; i < 4; ++i) { sink_quant_pull(&q, BLOCK, 0, false, sink_quant_now_ns(), collect, &g); g.nout += BLOCK; }
    sink_quant_health(&q, &h);
    CHECK(!h.measured, "no device position reported, but the adapter claims it can see dropouts");
    sink_quant_free(&q);
}

int main(void) {
    /* --- the pass-through path: the device asks for exactly one engine block --- */
    static uint32_t req_pass[64];
    for (int i = 0; i < 64; ++i) req_pass[i] = BLOCK;
    SinkQuant qa;
    run_sequence(&qa, req_pass, 64, true);
    CHECK(qa.h_passthrough == 64, "every equal-size pull should be pass-through, got %llu",
          (unsigned long long)qa.h_passthrough);
    CHECK(sink_quant_queued(&qa) == 0, "pass-through holds nothing, holds %u", sink_quant_queued(&qa));
    static float pass_out[CHANS][OUTCAP];
    memcpy(pass_out, g.out, sizeof pass_out);
    const uint32_t pass_frames = g.nout;
    sink_quant_free(&qa);

    /* --- the FIFO path: the spec's mixed sequence, B/3, B, 2.5B and a single frame --- */
    static uint32_t req_mixed[256];
    uint32_t nmixed = 0, total = 0;
    while (total + BLOCK * 3 < pass_frames && nmixed + 4 < 256) {
        const uint32_t seq[4] = { BLOCK / 3, BLOCK, (BLOCK * 5) / 2, 1 };
        for (int k = 0; k < 4 && total + seq[k] <= pass_frames; ++k) {
            req_mixed[nmixed++] = seq[k];
            total += seq[k];
        }
        if (total >= pass_frames) break;
    }
    while (total < pass_frames && nmixed < 256) {       /* pad to the same total, so the two runs compare */
        const uint32_t n = pass_frames - total > BLOCK ? BLOCK : pass_frames - total;
        req_mixed[nmixed++] = n; total += n;
    }
    SinkQuant qb;
    run_sequence(&qb, req_mixed, nmixed, true);
    CHECK(g.nout == pass_frames, "the two runs must cover the same stream (%u vs %u)", g.nout, pass_frames);
    CHECK(qb.h_passthrough < qb.h_blocks, "the mixed run must exercise the FIFO path, not only the "
          "fast one (%llu pass-through pulls over %llu blocks)",
          (unsigned long long)qb.h_passthrough, (unsigned long long)qb.h_blocks);

    /* THE BIT-IDENTITY CLAIM: the two paths produce the same device stream, sample for sample. */
    for (uint32_t c = 0; c < CHANS; ++c)
        for (uint32_t d = 0; d < pass_frames && d < OUTCAP; ++d)
            if (memcmp(&pass_out[c][d], &g.out[c][d], sizeof(float)) != 0) {
                CHECK(0, "pass-through and FIFO diverge at frame %u ch%u: %f vs %f",
                      d, c, (double)pass_out[c][d], (double)g.out[c][d]);
                d = pass_frames;
            }
    sink_quant_free(&qb);

    /* --- a single-frame device, the worst case for the FIFO bookkeeping --- */
    static uint32_t req_one[600];
    for (int i = 0; i < 600; ++i) req_one[i] = 1;
    SinkQuant qc;
    run_sequence(&qc, req_one, 600, true);
    CHECK(g.nrenders == (600 + BLOCK - 1) / BLOCK, "1-frame requests rendered %u blocks, want %u",
          g.nrenders, (600 + BLOCK - 1) / BLOCK);
    sink_quant_free(&qc);

    /* --- a device larger than the block, not a multiple of it --- */
    static uint32_t req_big[40];
    for (int i = 0; i < 40; ++i) req_big[i] = (BLOCK * 5) / 2;
    SinkQuant qd;
    run_sequence(&qd, req_big, 40, true);
    sink_quant_free(&qd);

    test_stamp_spacing_within_one_pull();
    test_stamps_never_go_backward();
    test_backend_reported_dropout();
    test_dropout_and_measured();

    if (fails) { fprintf(stderr, "sink_quant: %d failure(s)\n", fails); return 1; }
    printf("sink_quant OK\n");
    return 0;
}
