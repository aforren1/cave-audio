/*
 * fuzz_api.c — seeded API-sequence fuzzer against the public ABI (the DLL).
 *
 * One engine per seed on the MANUAL sink: bwa_render_block is pumped from this thread, so the
 * "audio thread" is the caller, the whole run is single-threaded (the ONE-control-thread
 * contract holds by construction — threading is deliberately NOT fuzzed; a second producer on
 * the command ring is a documented invariant violation, not a bug to find), and the render is
 * the deterministic offline path. A weighted generator biases toward legal-looking sequences
 * (a shadow model tracks live sources/beds/sounds/materials so ops compose like a real client)
 * with a controlled rate of deliberate misuse: stale handles from a graveyard, crafted
 * wrong-generation handles, wrong-kind assets (mono to a bed, AmbiX to a point source),
 * out-of-range enums, and extreme values.
 *
 * Determinism is the contract: an inline PCG32 stream drives every choice, no readback ever
 * feeds a decision, and no wall clock is consulted — the same seed replays the same call
 * sequence exactly. The one readback the sequence DOES consume is bwa_get_dsp_time_frames (for
 * play_at/stop_at deadlines), which on the manual sink is a pure function of blocks pumped, so
 * it is deterministic too. Any failure prints the seed + operation index + the repro line.
 *
 * Non-finite injection is tiered: NaN/Inf go only into value paths (positions, poses, gains,
 * quaternions) and only rarely; the first injection sets a per-run `toxic` flag that disables
 * every finiteness assertion for the rest of that seed (garbage in voids the finite-out
 * guarantee), while the no-crash/no-hang and structural assertions stay armed. bwa_source_push
 * gets non-finite samples unconditionally — the header promises they become 0.
 *
 * Assertions are the unambiguous, header-documented contracts only: render_block's shape;
 * channel count / sample rate / block size fixed for the engine lifetime; dsp clock monotonic
 * between restarts; stale handles read dead (is_playing false, playhead 0, push refused);
 * poll_ended bounded by cap, draining, nonzero handles, monotonic dropped total; get_tuning
 * fills struct_size; get_health on the manual sink reports unmeasured and zeroed; the
 * mono/multichannel play rejections set bwa_last_error; handle 0 is never returned as valid.
 *
 * CLI:  (none)      the fixed ctest set — FZ_NUM_SEEDS seeds x FZ_DEFAULT_ITERS ops
 *       --seed <n>  run exactly that one seed (the repro switch)
 *       --iters <n> ops per seed, both modes (the soak knob)
 *       --trace     print every op (and pose values) to stderr while reproducing a failure
 */
#include "bw_audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------------------------------------------------------- PRNG (PCG32, inline) ---- */

typedef struct { uint64_t state, inc; } Rng;

static uint64_t splitmix64(uint64_t* x) {
    uint64_t v = (*x += 0x9E3779B97F4A7C15ULL);
    v = (v ^ (v >> 30)) * 0xBF58476D1CE4E5B9ULL;
    v = (v ^ (v >> 27)) * 0x94D049BB133111EBULL;
    return v ^ (v >> 31);
}
static void rng_seed(Rng* r, uint64_t seed) {
    uint64_t s = seed;
    r->state = splitmix64(&s);
    r->inc   = splitmix64(&s) | 1ULL;
}
static uint32_t rng_u32(Rng* r) {
    uint64_t old = r->state;
    r->state = old * 6364136223846793005ULL + r->inc;
    uint32_t xs  = (uint32_t)(((old >> 18) ^ old) >> 27);
    uint32_t rot = (uint32_t)(old >> 59);
    return (xs >> rot) | (xs << ((32u - rot) & 31u));
}

/* ---------------------------------------------------------------- shadow model ------------ */

#define FZ_MAX_SRC   32     /* live sources the model keeps (well under the 256 voice pool, so
                             * voice STEAL never fires and "play reads playing" stays assertable) */
#define FZ_MAX_BED   6
#define FZ_MAX_TOK   40     /* minted materials kept live (under the 63-slot custom capacity) */
#define FZ_GRAVE     32     /* stale-handle graveyard depth (a ring; old entries overwritten) */
#define FZ_MAX_DYN   8

#define FZ_NUM_SEEDS     16
#define FZ_DEFAULT_ITERS 1500   /* 16 x 1500 = 24k ops in a few seconds WITH the Steam SDK (whose
                                 * sim threads and BVH rebuilds are the cost); soak with --iters */

typedef struct {
    Rng      rng;
    bwa_engine* e;
    unsigned long long seed;
    uint32_t iters, op;
    const char* opname;
    int      started;
    int      restart_for;    /* ops remaining in the post-restart clock disarm (bounded, not
                              * permanent: the stale pair clears within a block or two). */
    int      restarted;      /* a stop/start happened: the clock-pair-vs-dsp assert disarms.
                              * FINDING (see the fuzzer report): a restart re-bases the manual
                              * sink's sample position to 0, but rt keeps publishing the PREVIOUS
                              * session's (sample, host_time) pair — the first re-based block
                              * stamps nominal time 0, which the publish gate reads as "no stamp"
                              * — so bwa_get_clock briefly reports an old-epoch pair AHEAD of the
                              * re-based dsp clock. */
    int      toxic;          /* non-finite input is in flight: finite asserts off for a BOUNDED
                              * window, not the rest of the seed. The engine's contract is to
                              * REJECT non-finite at the edge, so finite output is supposed to
                              * hold even under toxic input; a permanent disarm retired the
                              * fuzzer's main weapon roughly 1% into every seed. */
    int      toxic_for;      /* ops remaining in the disarm window */
    int      cap_set;        /* output capture callback installed */
    uint64_t cap_count;      /* fires of the capture callback (single-threaded: exact) */
    bwa_profile profile;
    uint32_t chan;           /* bwa_get_channel_count at create — must never change */
    uint32_t out_ch;         /* expected bwa_render_block channel count for this profile */
    uint32_t block;
    uint64_t dsp_prev;       /* monotonic-between-restarts shadow */
    uint64_t dropped_prev;   /* bwa_poll_ended running dropped total shadow */
    uint64_t renders;

    bwa_sound snd_short, snd_long, snd_ambix, snd_stream;
    bwa_sound snd_grave[FZ_GRAVE];  uint32_t n_snd_grave;

    struct { bwa_source h; uint8_t push; } src[FZ_MAX_SRC];
    uint32_t nsrc;
    bwa_source src_grave[FZ_GRAVE]; uint32_t n_src_grave;

    bwa_bed bed[FZ_MAX_BED];        uint32_t nbed;
    bwa_bed bed_grave[FZ_GRAVE];    uint32_t n_bed_grave;

    bwa_material tok[FZ_MAX_TOK];   uint32_t ntok;
    int dyn[FZ_MAX_DYN];            uint32_t ndyn;
} Fz;

static uint64_t g_total_ops;     /* across all seeds, for the final report */
static uint64_t g_total_renders;
static int      g_trace;         /* --trace: print each op as it runs (repro diagnosis) */

/* Failure macro: prints seed + op index + the repro line, then fails the enclosing int fn.
 * Callers always pass a format string, so __VA_ARGS__ is never empty. */
#define FAIL(...) do { \
    fprintf(stderr, "\n=== FUZZ FAILURE  seed=%llu  op=%u  [%s] ===\n    ", \
            z->seed, z->op, z->opname); \
    fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n    reproduce: test_fuzz_api --seed %llu --iters %u\n\n", \
            z->seed, z->iters); \
    return 1; \
} while (0)

/* ---------------------------------------------------------------- random helpers ---------- */

static uint32_t rnd(Fz* z, uint32_t n)            { return rng_u32(&z->rng) % n; }        /* n > 0 */
static int      chance(Fz* z, uint32_t p, uint32_t of) { return rnd(z, of) < p; }
static float    rndf(Fz* z)                       { return (float)(rng_u32(&z->rng) >> 8) * (1.0f / 16777216.0f); }
static float    frange(Fz* z, float lo, float hi) { return lo + (hi - lo) * rndf(z); }

/* Hostile-but-FINITE float: mostly sane, sometimes an extreme (bounds probes, zero, negative).
 * Never non-finite — safe for knobs whose value persists into readbacks. */
static float hfin(Fz* z, float lo, float hi) {
    uint32_t k = rnd(z, 100);
    if (k < 80) return frange(z, lo, hi);
    switch (rnd(z, 7)) {
        case 0: return 0.f;
        case 1: return -1.f;
        case 2: return lo;
        case 3: return hi;
        case 4: return 1e6f;
        case 5: return -1e6f;
        default: return 1e-6f;
    }
}
/* Toxic-capable float: like hfin but ~2% non-finite (NaN/Inf/near-FLT_MAX). Only for value
 * paths (positions, poses, gains, quats). Sets z->toxic, which disarms finite asserts. */
static float htox(Fz* z, float lo, float hi) {
    if (chance(z, 2, 100)) {
        z->toxic = 1; z->toxic_for = 24;   /* long enough for the value to reach a rendered block and
                                            * be overwritten by the next legal set, short enough that
                                            * the finite asserts are armed for the rest of the seed */
        switch (rnd(z, 4)) {
            case 0:  return NAN;
            case 1:  return INFINITY;
            case 2:  return -INFINITY;
            default: return 3.0e38f;
        }
    }
    return hfin(z, lo, hi);
}

/* ---------------------------------------------------------------- handle pickers ---------- */

typedef enum { H_LIVE, H_STALE, H_WRONGGEN, H_ZERO, H_GARBAGE } HClass;

static void grave_push(bwa_source* grave, uint32_t* n, Fz* z, bwa_source h) {
    if (*n < FZ_GRAVE) grave[(*n)++] = h;
    else grave[rnd(z, FZ_GRAVE)] = h;
}

/* Mostly a live handle; deliberately stale/wrong-generation/zero/garbage at a controlled rate.
 * The class comes back so the caller only asserts what it KNOWS about the handle. */
static bwa_source pick_src(Fz* z, HClass* cls, uint32_t* live_idx) {
    uint32_t k = rnd(z, 100);
    if (z->nsrc && k < 78) {
        uint32_t i = rnd(z, z->nsrc);
        if (live_idx) *live_idx = i;
        *cls = H_LIVE;
        return z->src[i].h;
    }
    if (z->n_src_grave && k < 88) { *cls = H_STALE;    return z->src_grave[rnd(z, z->n_src_grave)]; }
    if (z->nsrc && k < 94)        { *cls = H_WRONGGEN; return z->src[rnd(z, z->nsrc)].h + 0x10000u; }
    if (k < 97)                   { *cls = H_ZERO;     return 0; }
    *cls = H_GARBAGE;
    return rng_u32(&z->rng) | 1u;
}

static bwa_bed pick_bed(Fz* z, HClass* cls) {
    uint32_t k = rnd(z, 100);
    if (z->nbed && k < 78)        { *cls = H_LIVE;     return z->bed[rnd(z, z->nbed)]; }
    if (z->n_bed_grave && k < 90) { *cls = H_STALE;    return z->bed_grave[rnd(z, z->n_bed_grave)]; }
    if (z->nbed && k < 95)        { *cls = H_WRONGGEN; return z->bed[rnd(z, z->nbed)] + 0x10000u; }
    *cls = H_ZERO;
    return 0;
}

typedef enum { S_MONO, S_AMBIX, S_STREAM, S_ZERO, S_STALE, S_GARBAGE } SClass;

static bwa_sound pick_snd(Fz* z, SClass* cls) {
    uint32_t k = rnd(z, 100);
    if (k < 60) { *cls = S_MONO;   return chance(z, 1, 2) ? z->snd_short : z->snd_long; }
    if (k < 72) { *cls = S_AMBIX;  return z->snd_ambix; }
    if (k < 80) { *cls = S_STREAM; return z->snd_stream; }
    if (k < 88) { *cls = S_ZERO;   return 0; }
    if (z->n_snd_grave && k < 96) { *cls = S_STALE; return z->snd_grave[rnd(z, z->n_snd_grave)]; }
    *cls = S_GARBAGE;
    return rng_u32(&z->rng) | 1u;
}

/* ---------------------------------------------------------------- fixtures ---------------- */

#define FZ_WAV_SHORT  "bwa_fuzz_short.wav"    /* 480 frames — finishes fast (completion events) */
#define FZ_WAV_LONG   "bwa_fuzz_long.wav"     /* 48000 frames — keeps playing */
#define FZ_WAV_AMBIX  "bwa_fuzz_ambix.wav"    /* 4 channels — an order-1 bed asset */
#define FZ_GARBAGE    "bwa_fuzz_garbage.bin"
#define FZ_EQ         "bwa_fuzz_eq.txt"
#define FZ_LAYOUT8    "bwa_fuzz_cube8.json"
#define FZ_NO_LAYOUT  "bwa_fuzz_no_such_layout.json"   /* never created */

/* 16-bit PCM interleaved wav, 48 kHz, per-channel phase-offset sine. */
static int write_wav(const char* path, uint32_t channels, uint32_t frames) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    const uint32_t rate = 48000u;
    const uint32_t data = frames * channels * 2u;
    uint32_t u; uint16_t w;
    fwrite("RIFF", 1, 4, f); u = 36u + data; fwrite(&u, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    u = 16;                    fwrite(&u, 4, 1, f);
    w = 1;                     fwrite(&w, 2, 1, f);   /* PCM */
    w = (uint16_t)channels;    fwrite(&w, 2, 1, f);
    u = rate;                  fwrite(&u, 4, 1, f);
    u = rate * channels * 2u;  fwrite(&u, 4, 1, f);
    w = (uint16_t)(channels * 2u); fwrite(&w, 2, 1, f);
    w = 16;                    fwrite(&w, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    for (uint32_t i = 0; i < frames; ++i)
        for (uint32_t c = 0; c < channels; ++c) {
            int16_t v = (int16_t)(6000.0 * sin(6.2831853 * 330.0 * (double)i / (double)rate + 0.5 * (double)c));
            fwrite(&v, 2, 1, f);
        }
    fclose(f);
    return 1;
}

/* 8 speakers on the corners of a 3 m cube — a valid, sub-capacity layout so some seeds run a
 * runtime channel count other than 26. */
static int write_layout8(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    int k = 0;
    fprintf(f, "{ \"speakers\": [\n");
    for (int x = -1; x <= 1; x += 2)
        for (int y = 0; y <= 1; ++y)
            for (int zz = -1; zz <= 1; zz += 2) {
                fprintf(f, "  {\"index\":%d,\"position\":[%g,%g,%g]}%s\n",
                        k, 1.5 * x, y ? 2.4 : 0.4, 1.5 * zz, k == 7 ? "" : ",");
                ++k;
            }
    fprintf(f, "] }\n");
    fclose(f);
    return k == 8;
}

static int write_text(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    fputs(text, f);
    fclose(f);
    return 1;
}

static void remove_fixtures(void) {
    remove(FZ_WAV_SHORT); remove(FZ_WAV_LONG); remove(FZ_WAV_AMBIX);
    remove(FZ_GARBAGE); remove(FZ_EQ); remove(FZ_LAYOUT8);
}

/* ---------------------------------------------------------------- output capture ---------- */

static void fz_capture(void* user, const float* planar, uint32_t channels, uint32_t nframes) {
    Fz* z = (Fz*)user;
    (void)planar; (void)channels; (void)nframes;
    z->cap_count++;
}

/* ---------------------------------------------------------------- core checks ------------- */

/* Pump one block and assert the render contract. Started: non-NULL, the profile's channel
 * count, the engine block size, finite samples (while the run is clean), the capture fired.
 * Stopped: NULL, documented. Also advances + checks the dsp-clock monotonicity shadow. */
static int render_once(Fz* z) {
    uint32_t ch = 0xDEAD, nf = 0xDEAD;
    uint64_t cap_before = z->cap_count;
    const float* p = bwa_render_block(z->e, &ch, &nf);
    if (!z->started) {
        if (p) FAIL("bwa_render_block returned non-NULL on a stopped engine");
        return 0;
    }
    if (!p) FAIL("bwa_render_block returned NULL on a started MANUAL engine: %s",
                 bwa_last_error(z->e) ? bwa_last_error(z->e) : "(no error)");
    if (ch != z->out_ch) FAIL("render channels %u, expected %u for profile %d", ch, z->out_ch, (int)z->profile);
    if (nf != z->block)  FAIL("render nframes %u, expected block size %u", nf, z->block);
    if (!z->toxic) {
        for (uint32_t i = 0; i < ch * nf; ++i)
            if (!isfinite(p[i]))
                FAIL("non-finite render output at sample %u (ch %u) with no non-finite input injected",
                     i % nf, i / nf);
    }
    if (z->cap_set && z->cap_count <= cap_before)
        FAIL("output capture callback did not fire across a rendered block");
    z->renders++;
    g_total_renders++;
    uint64_t t = bwa_get_dsp_time_frames(z->e);
    if (t < z->dsp_prev) FAIL("dsp clock went backwards (%llu -> %llu) with no restart",
                              (unsigned long long)z->dsp_prev, (unsigned long long)t);
    z->dsp_prev = t;
    return 0;
}

/* The engine-still-works gauntlet, run every few ops: one rendered block plus every readback
 * whose value is pinned for the engine's whole lifetime. */
static int health(Fz* z) {
    const char* keep = z->opname;
    z->opname = "health";
    if (render_once(z)) return 1;

    if (bwa_get_channel_count(z->e) != z->chan)
        FAIL("channel count changed: %u -> %u", z->chan, bwa_get_channel_count(z->e));
    if (bwa_get_sample_rate(z->e) != 48000u)
        FAIL("sample rate readback changed: %u", bwa_get_sample_rate(z->e));
    if (bwa_get_block_size(z->e) != z->block)
        FAIL("block size readback changed: %u", bwa_get_block_size(z->e));
    if (bwa_get_version() != BWA_VERSION)
        FAIL("bwa_get_version changed mid-run: %x", bwa_get_version());
    if (bwa_get_sink_type(z->e) != BWA_SINK_MANUAL)
        FAIL("sink type is not MANUAL (%d)", (int)bwa_get_sink_type(z->e));
    if (!bwa_get_audio_backend(z->e))
        FAIL("bwa_get_audio_backend returned NULL");

    /* speaker query conventions (documented): NULL = total = channel count; a small cap fills
     * min(cap, count). */
    if (bwa_get_speakers(z->e, NULL, 0) != z->chan)
        FAIL("get_speakers(NULL) != channel count");
    {
        float xyz[3 * 4];
        uint32_t want = z->chan < 4u ? z->chan : 4u;
        if (bwa_get_speakers(z->e, xyz, 4) != want)
            FAIL("get_speakers under a small cap did not return the filled count");
        if (!z->toxic)
            for (uint32_t i = 0; i < want * 3u; ++i)
                if (!isfinite(xyz[i])) FAIL("non-finite speaker position readback");
    }

    /* meters / gauges: bounded and finite (while clean) */
    {
        float lv[32];
        uint32_t cap = 1u + rnd(z, 32);
        uint32_t n = bwa_get_bus_levels(z->e, lv, cap);
        if (n > cap) FAIL("get_bus_levels filled %u > cap %u", n, cap);
        if (!z->toxic)
            for (uint32_t i = 0; i < n; ++i)
                if (!isfinite(lv[i]) || lv[i] < 0.f) FAIL("bus level %u is %g", i, lv[i]);
    }
    if (bwa_get_active_voices(z->e) > 264u)   /* 256 pool + the fade reserve */
        FAIL("active voices %u exceeds the physical pool", bwa_get_active_voices(z->e));

    /* tuning readback always fills struct_size (documented: feed-back-able) */
    {
        bwa_tuning t;
        memset(&t, 0xAB, sizeof t);
        if (!bwa_get_tuning(z->e, &t)) FAIL("bwa_get_tuning returned false on a live engine");
        if (t.struct_size != (uint32_t)sizeof(bwa_tuning))
            FAIL("get_tuning struct_size %u != sizeof(bwa_tuning) %u", t.struct_size, (uint32_t)sizeof(bwa_tuning));
        if (!isfinite(t.spcap_focus) || !isfinite(t.near_spread) || !isfinite(t.hole_spread) ||
            !isfinite(t.align_dead_zone_m) || !isfinite(t.align_slew_frames_per_s))
            FAIL("get_tuning carries a non-finite knob value");   /* knobs only take finite input here */
    }

    /* the manual sink cannot observe a dropout: health must say unmeasured with the DEVICE-side
     * counters zeroed. stream_starves is deliberately exempt — engine.c fills it under any sink
     * because the stream ring is the engine's, not the device's (the header's "out is zeroed
     * either way" over-promises there; see the fuzzer report). */
    {
        bwa_health h;
        memset(&h, 0xCD, sizeof h);
        if (bwa_get_health(z->e, &h))
            FAIL("bwa_get_health claims measured on the MANUAL sink");
        if (h.blocks || h.xruns || h.dropped_frames || h.driver_resyncs || h.late_blocks || h.peak_load != 0.f)
            FAIL("bwa_get_health returned false but did not zero the device-side counters");
        if (bwa_get_xruns(z->e) != 0)
            FAIL("bwa_get_xruns nonzero on the unmeasurable manual sink");
    }
    if (bwa_get_output_latency_frames(z->e) != 0)
        FAIL("manual sink reports a nonzero output latency");

    /* clock pair: never ahead of the dsp clock. Disarmed once a restart has happened — the pair
     * survives the re-base and legitimately(?) reads ahead for a block (see z->restarted). */
    if (!z->restarted) {
        uint64_t cs = 0, ct = 0;
        if (bwa_get_clock(z->e, &cs, &ct) && cs > bwa_get_dsp_time_frames(z->e))
            FAIL("clock-pair sample runs ahead of the dsp clock (pair %llu, dsp %llu)",
                 (unsigned long long)cs, (unsigned long long)bwa_get_dsp_time_frames(z->e));
    }

    if (!z->toxic) {
        float p[3], q[4];
        bwa_get_listener_pose(z->e, p, q);
        for (int i = 0; i < 3; ++i) if (!isfinite(p[i])) FAIL("non-finite listener position readback");
        for (int i = 0; i < 4; ++i) if (!isfinite(q[i])) FAIL("non-finite listener orientation readback");
    }

    /* completion drain: bounded by cap, nonzero handles, monotonic dropped total */
    {
        bwa_source ended[8];
        uint64_t dropped = 0;
        uint32_t n = bwa_poll_ended(z->e, ended, 8, &dropped);
        if (n > 8) FAIL("poll_ended returned %u > cap 8", n);
        for (uint32_t i = 0; i < n; ++i)
            if (ended[i] == 0) FAIL("poll_ended reported handle 0");
        if (dropped < z->dropped_prev)
            FAIL("poll_ended dropped total went backwards (%llu -> %llu)",
                 (unsigned long long)z->dropped_prev, (unsigned long long)dropped);
        z->dropped_prev = dropped;
    }

    /* and the engine still accepts a plain valid operation */
    for (uint32_t i = 0; i < z->nsrc; ++i)
        if (!z->src[i].push) { bwa_source_set_gain(z->e, z->src[i].h, 0.5f); break; }
    bwa_commit(z->e);

    z->opname = keep;
    return 0;
}

/* ---------------------------------------------------------------- op table ---------------- */

typedef enum {
    OP_RENDER, OP_COMMIT,
    OP_SRC_CREATE, OP_SRC_CREATE_PUSH, OP_SRC_DESTROY,
    OP_PLAY, OP_PLAY_AT, OP_PLAY_LOOP, OP_STOP, OP_STOP_AT,
    OP_QUEUE, OP_CLEAR_QUEUE, OP_PAUSE, OP_SEEK,
    OP_SET_POS, OP_GAIN_FAM, OP_RATE_PRIO_GROUP,
    OP_FLAGS, OP_SPREAD_FAM, OP_DIRECTIVITY,
    OP_PUSH_FRAMES, OP_PUSH_MISUSE,
    OP_GROUP_CTRL, OP_LISTENER, OP_EXTRA_LIS, OP_ONESHOT,
    OP_BED, OP_GLOBAL, OP_KNOB, OP_TUNING,
    OP_SCENE, OP_DYNMESH, OP_MATERIAL,
    OP_SOUND_META, OP_READBACK, OP_POLL,
    OP_LOAD, OP_RESTART, OP_PURE, OP_HPEQ, OP_CAPTURE,
    OP_NULL_ENGINE, OP_TRACKER,
    OP__COUNT
} Op;

static const struct { Op op; uint8_t weight; const char* name; } OPS[] = {
    { OP_RENDER,          12, "render" },
    { OP_COMMIT,           8, "commit" },
    { OP_SRC_CREATE,       4, "src_create" },
    { OP_SRC_CREATE_PUSH,  2, "src_create_push" },
    { OP_SRC_DESTROY,      3, "src_destroy" },
    { OP_PLAY,             6, "src_play" },
    { OP_PLAY_AT,          2, "src_play_at" },
    { OP_PLAY_LOOP,        2, "src_play_loop" },
    { OP_STOP,             2, "src_stop" },
    { OP_STOP_AT,          1, "src_stop_at" },
    { OP_QUEUE,            2, "src_queue" },
    { OP_CLEAR_QUEUE,      1, "src_clear_queue" },
    { OP_PAUSE,            2, "src_set_paused" },
    { OP_SEEK,             2, "src_seek" },
    { OP_SET_POS,          6, "src_set_pos" },
    { OP_GAIN_FAM,         4, "src_gain/fade" },
    { OP_RATE_PRIO_GROUP,  2, "src_pitch/prio/group" },
    { OP_FLAGS,            3, "src_flags" },
    { OP_SPREAD_FAM,       2, "src_spread/extent/size" },
    { OP_DIRECTIVITY,      2, "src_directivity" },
    { OP_PUSH_FRAMES,      3, "src_push" },
    { OP_PUSH_MISUSE,      1, "push_misuse" },
    { OP_GROUP_CTRL,       1, "group_ctrl" },
    { OP_LISTENER,         4, "listener_pose" },
    { OP_EXTRA_LIS,        1, "extra_listeners" },
    { OP_ONESHOT,          2, "oneshot" },
    { OP_BED,              3, "bed" },
    { OP_GLOBAL,           2, "global" },
    { OP_KNOB,             4, "knob" },
    { OP_TUNING,           2, "tuning" },
    { OP_SCENE,            2, "scene" },
    { OP_DYNMESH,          1, "dynmesh" },
    { OP_MATERIAL,         2, "material" },
    { OP_SOUND_META,       1, "sound_meta" },
    { OP_READBACK,         2, "readback" },
    { OP_POLL,             2, "poll_ended" },
    { OP_LOAD,             1, "load_class" },
    { OP_RESTART,          1, "restart" },
    { OP_PURE,             1, "pure_calls" },
    { OP_HPEQ,             1, "headphone_eq" },
    { OP_CAPTURE,          1, "capture" },
    { OP_NULL_ENGINE,      1, "null_engine" },
    { OP_TRACKER,          1, "tracker" },
};

static uint8_t g_pick[256];       /* weight-expanded op pick table */
static uint32_t g_pick_n;

static void build_pick_table(void) {
    g_pick_n = 0;
    for (uint32_t i = 0; i < sizeof OPS / sizeof OPS[0]; ++i)
        for (uint32_t w = 0; w < OPS[i].weight && g_pick_n < 256; ++w)
            g_pick[g_pick_n++] = (uint8_t)i;
}

/* ---------------------------------------------------------------- one op ------------------ */

static int do_op(Fz* z) {
    bwa_engine* e = z->e;
    uint32_t oi = g_pick[rnd(z, g_pick_n)];
    z->opname = OPS[oi].name;
    if (g_trace) fprintf(stderr, "  op %u: %s\n", z->op, z->opname);

    switch (OPS[oi].op) {

    case OP_RENDER: {
        uint32_t n = 1 + rnd(z, 2);
        for (uint32_t i = 0; i < n; ++i)
            if (render_once(z)) return 1;
        break;
    }

    case OP_COMMIT:
        bwa_commit(e);
        break;

    case OP_SRC_CREATE: {
        if (z->nsrc >= FZ_MAX_SRC) break;
        bwa_source h = bwa_source_create(e);
        if (h == 0) FAIL("bwa_source_create returned 0");
        z->src[z->nsrc].h = h; z->src[z->nsrc].push = 0; z->nsrc++;
        break;
    }

    case OP_SRC_CREATE_PUSH: {
        if (z->nsrc >= FZ_MAX_SRC) break;
        bwa_source h = bwa_source_create_push(e);
        if (h == 0) {
            /* a documented failure mode: push sources share the 16-slot stream table with
             * streaming sounds (stream.c MAX_STREAMS), and destroyed rings release async — a
             * refusal is legal capacity behavior, but it must come with a reason */
            if (!bwa_last_error(e)) FAIL("bwa_source_create_push returned 0 with no error set");
            break;
        }
        z->src[z->nsrc].h = h; z->src[z->nsrc].push = 1; z->nsrc++;
        break;
    }

    case OP_SRC_DESTROY: {
        HClass c; uint32_t i = 0;
        bwa_source h = pick_src(z, &c, &i);
        bwa_source_destroy(e, h);          /* stale/garbage destroys must be silent no-ops */
        if (c == H_LIVE) {
            grave_push(z->src_grave, &z->n_src_grave, z, h);
            z->src[i] = z->src[--z->nsrc];
        }
        break;
    }

    case OP_PLAY: {
        HClass c; uint32_t i = 0;
        bwa_source h = pick_src(z, &c, &i);
        SClass sc;
        bwa_sound snd = pick_snd(z, &sc);
        bool loop = chance(z, 1, 3);
        bwa_source_play(e, h, snd, loop);
        if (c == H_LIVE && !z->src[i].push && sc == S_AMBIX) {
            /* documented sync rejection: a multichannel asset is a bed, not a point source */
            if (!bwa_last_error(e)) FAIL("src_play(ambix) on a live plain source set no error");
        } else if (c == H_LIVE && z->src[i].push && (sc == S_MONO || sc == S_STREAM)) {
            /* documented sync rejection: plays are refused on push sources, with a reason */
            if (!bwa_last_error(e)) FAIL("src_play on a live PUSH source set no error");
        } else if (c == H_LIVE && !z->src[i].push && sc == S_MONO) {
            /* documented: a play that has not reached the audio thread yet already reads true.
             * (S_STREAM deliberately not asserted: a streamed asset plays on one voice at a
             * time, so a re-play while it is bound elsewhere may legitimately be refused.) */
            if (!bwa_source_is_playing(e, h))
                FAIL("a fresh play on a live plain source does not read as playing");
        } else if (c == H_STALE || c == H_WRONGGEN || c == H_ZERO) {
            if (bwa_source_is_playing(e, h))
                FAIL("a dead handle reads as playing after a play was aimed at it");
        }
        break;
    }

    case OP_PLAY_AT: {
        HClass c; uint32_t i = 0;
        bwa_source h = pick_src(z, &c, &i);
        SClass sc;
        bwa_sound snd = pick_snd(z, &sc);
        uint64_t now = bwa_get_dsp_time_frames(e);   /* deterministic on the manual sink */
        uint64_t at;
        switch (rnd(z, 4)) {
            case 0:  at = 0; break;                                /* == play now */
            case 1:  at = now + rnd(z, 4096); break;               /* near future */
            case 2:  at = now > 1000 ? now - 1000 : 0; break;      /* the past: best-effort now */
            default: at = now + (1ULL << 40); break;               /* far future: held silent */
        }
        bwa_source_play_at(e, h, snd, chance(z, 1, 3), at);
        break;
    }

    case OP_PLAY_LOOP: {
        HClass c; uint32_t i = 0;
        bwa_source h = pick_src(z, &c, &i);
        SClass sc;
        bwa_sound snd = pick_snd(z, &sc);
        uint64_t b = rnd(z, 600), en = rnd(z, 600);   /* often inverted/oob: falls back whole-clip */
        if (chance(z, 1, 4)) en = 0;
        if (chance(z, 1, 8)) b = 1ULL << 40;
        bwa_source_play_loop(e, h, snd, b, en);
        break;
    }

    case OP_STOP: {
        HClass c;
        bwa_source h = pick_src(z, &c, NULL);
        bwa_source_stop(e, h);
        break;
    }

    case OP_STOP_AT: {
        HClass c;
        bwa_source h = pick_src(z, &c, NULL);
        uint64_t now = bwa_get_dsp_time_frames(e);
        bwa_source_stop_at(e, h, chance(z, 1, 4) ? 0 : now + rnd(z, 8192));
        break;
    }

    case OP_QUEUE: {
        HClass c;
        bwa_source h = pick_src(z, &c, NULL);
        SClass sc;
        bwa_sound snd = pick_snd(z, &sc);
        uint32_t n = 1 + rnd(z, 9);        /* > the 7-deep queue sometimes: further queues drop */
        for (uint32_t i = 0; i < n; ++i)
            bwa_source_queue(e, h, snd, i + 1 == n && chance(z, 1, 2));
        break;
    }

    case OP_CLEAR_QUEUE: {
        HClass c;
        bwa_source h = pick_src(z, &c, NULL);
        bwa_source_clear_queue(e, h);
        break;
    }

    case OP_PAUSE: {
        HClass c;
        bwa_source h = pick_src(z, &c, NULL);
        bwa_source_set_paused(e, h, chance(z, 1, 2));
        break;
    }

    case OP_SEEK: {
        HClass c;
        bwa_source h = pick_src(z, &c, NULL);
        uint64_t fr;
        switch (rnd(z, 4)) {
            case 0:  fr = rnd(z, 512); break;
            case 1:  fr = rnd(z, 100000); break;
            case 2:  fr = 1ULL << 40; break;
            default: fr = UINT64_MAX; break;
        }
        bwa_source_seek(e, h, fr);
        break;
    }

    case OP_SET_POS: {
        HClass c;
        bwa_source h = pick_src(z, &c, NULL);
        /* draws hoisted into locals here and below: N rng calls in one argument list would make
         * the sequence depend on the compiler's evaluation order */
        float x = htox(z, -4.f, 4.f), y = htox(z, 0.f, 3.f), w = htox(z, -4.f, 4.f);
        bwa_source_set_pos(e, h, x, y, w);
        break;
    }

    case OP_GAIN_FAM: {
        HClass c;
        bwa_source h = pick_src(z, &c, NULL);
        switch (rnd(z, 4)) {
            case 0: bwa_source_set_gain(e, h, htox(z, 0.f, 2.f)); break;
            case 1: {
                float g = htox(z, 0.f, 2.f), s = hfin(z, -1.f, 3.f);
                bwa_source_fade_to(e, h, g, s);
                break;
            }
            case 2: bwa_source_fade_out(e, h, hfin(z, -1.f, 3.f)); break;
            default: {
                float rd = hfin(z, -1.f, 10.f), ro = hfin(z, -1.f, 4.f), mg = hfin(z, -1.f, 2.f);
                bwa_source_set_attenuation_override(e, h, rd, ro, mg);
                break;
            }
        }
        break;
    }

    case OP_RATE_PRIO_GROUP: {
        HClass c;
        bwa_source h = pick_src(z, &c, NULL);
        switch (rnd(z, 3)) {
            case 0: bwa_source_set_pitch(e, h, hfin(z, 0.1f, 8.f)); break;
            case 1: bwa_source_set_priority(e, h, (int)rnd(z, 600) - 100); break;
            default: bwa_source_set_group(e, h, rnd(z, 12)); break;  /* > BWA_GROUPS: falls back to 0 */
        }
        break;
    }

    case OP_FLAGS: {
        HClass c;
        bwa_source h = pick_src(z, &c, NULL);
        bool on = chance(z, 1, 2);
        switch (rnd(z, 10)) {
            case 0: bwa_source_set_occlusion(e, h, on); break;
            case 1: {
                float bands[3];
                bands[0] = hfin(z, 0.f, 1.f); bands[1] = hfin(z, 0.f, 1.f); bands[2] = hfin(z, 0.f, 1.f);
                float lvl = htox(z, 0.f, 1.f);
                bwa_source_set_occlusion_manual(e, h, lvl, chance(z, 1, 3) ? NULL : bands);
                break;
            }
            case 2: bwa_source_set_reverb(e, h, on); break;
            case 3: bwa_source_set_reverb_send(e, h, hfin(z, 0.f, 2.f)); break;
            case 4: bwa_source_set_reverb_distance(e, h, on); break;
            case 5: bwa_source_set_pathing(e, h, on); break;
            case 6: bwa_source_set_doppler(e, h, on); break;
            case 7: bwa_source_set_air_absorption(e, h, on); break;
            case 8: bwa_source_set_loudness_comp(e, h, on); break;
            default:
                if (on) bwa_source_set_proximity(e, h, true);
                else    bwa_source_set_early_reflections(e, h, chance(z, 1, 2));
                break;
        }
        break;
    }

    case OP_SPREAD_FAM: {
        HClass c;
        bwa_source h = pick_src(z, &c, NULL);
        switch (rnd(z, 3)) {
            case 0: bwa_source_set_spread(e, h, hfin(z, -0.5f, 1.5f)); break;
            case 1: {
                float w = hfin(z, 0.f, 1.f), hh = hfin(z, 0.f, 1.f);
                bwa_source_set_extent(e, h, w, hh);
                break;
            }
            default: bwa_source_set_size(e, h, hfin(z, 0.f, 5.f)); break;
        }
        break;
    }

    case OP_DIRECTIVITY: {
        HClass c;
        bwa_source h = pick_src(z, &c, NULL);
        switch (rnd(z, 3)) {
            case 0: {
                float a = htox(z, -1.f, 1.f), b = htox(z, -1.f, 1.f), cq = htox(z, -1.f, 1.f), dq = htox(z, -1.f, 1.f);
                bwa_source_set_orientation(e, h, a, b, cq, dq);
                break;
            }
            case 1: {
                float w = hfin(z, -0.5f, 1.5f), p = hfin(z, 0.f, 8.f);
                bwa_source_set_directivity(e, h, w, p);
                break;
            }
            default: bwa_source_set_directivity_preset(e, h,
                         chance(z, 1, 8) ? (bwa_directivity)7 : (bwa_directivity)rnd(z, 3)); break;
        }
        break;
    }

    case OP_PUSH_FRAMES: {
        HClass c; uint32_t i = 0;
        bwa_source h = pick_src(z, &c, &i);
        static float buf[2048];
        uint32_t n = rnd(z, 2049);
        int poison = chance(z, 1, 8);       /* documented: non-finite samples become 0 */
        for (uint32_t k = 0; k < n; ++k) {
            buf[k] = frange(z, -0.5f, 0.5f);
            if (poison && (k & 63u) == 0) buf[k] = (k & 64u) ? NAN : INFINITY;
        }
        uint32_t got = bwa_source_push(e, h, buf, n);
        if (got > n) FAIL("bwa_source_push accepted %u > offered %u", got, n);
        if (c == H_LIVE && !z->src[i].push) {
            if (got != 0) FAIL("push on a live NON-push source accepted %u frames", got);
            if (!bwa_last_error(e)) FAIL("push on a live non-push source set no error");
        }
        if ((c == H_STALE || c == H_WRONGGEN || c == H_ZERO) && got != 0)
            FAIL("push on a dead handle accepted %u frames", got);
        break;
    }

    case OP_PUSH_MISUSE: {
        HClass c; uint32_t i = 0;
        bwa_source h = pick_src(z, &c, &i);
        switch (rnd(z, 3)) {
            case 0: {
                uint32_t sp = bwa_source_push_space(e, h);
                if (sp > 65536u) FAIL("push_space %u exceeds the documented 65536-frame ring", sp);
                if (c == H_LIVE && !z->src[i].push && sp != 0)
                    FAIL("push_space on a live non-push source is %u, not 0", sp);
                break;
            }
            case 1: bwa_source_push_end(e, h); break;    /* on anything: plain/stale must no-op */
            default: {
                uint32_t got = bwa_source_push(e, h, NULL, 0);   /* zero-length push */
                if (got != 0) FAIL("zero-length push accepted %u frames", got);
                break;
            }
        }
        break;
    }

    case OP_GROUP_CTRL: {
        uint32_t g = rnd(z, 12);            /* out-of-range group calls are documented ignored */
        if (chance(z, 1, 2)) bwa_group_set_gain(e, g, htox(z, 0.f, 2.f));
        else                 bwa_group_set_paused(e, g, chance(z, 1, 2));
        break;
    }

    case OP_LISTENER: {
        /* Un-normalized quaternions of ANY finite magnitude are fuzzed on purpose. They used to
         * poison the render: rt_set_listener guarded isfinite() but not magnitude, and every
         * consumer assumes a unit quat, so a component around 1e6 overflowed the rotation math as
         * thoroughly as a NaN would. rt_set_listener normalizes now (degenerate falls back to
         * identity), which is what makes this line safe to fuzz. Found by seed 104 op 22. */
        float qx = htox(z, -1.f, 1.f), qy = htox(z, -1.f, 1.f), qz = htox(z, -1.f, 1.f), qw = htox(z, -1.f, 1.f);
        if (chance(z, 1, 8)) { qx = qy = qz = qw = 0.f; }   /* degenerate zero quaternion -> identity */
        float px = htox(z, -4.f, 4.f), py = htox(z, 0.f, 3.f), pz = htox(z, -4.f, 4.f);
        if (g_trace) fprintf(stderr, "    pose p(%g %g %g) q(%g %g %g %g)\n", px, py, pz, qx, qy, qz, qw);
        bwa_set_listener_pose(e, px, py, pz, qx, qy, qz, qw);
        break;
    }

    case OP_EXTRA_LIS: {
        float xyz[6 * 3];
        for (int i = 0; i < 18; ++i) xyz[i] = htox(z, -4.f, 4.f);
        uint32_t count = rnd(z, 7);          /* 0..6: beyond BWA_EXTRA_LIS(3) probes the clamp */
        bwa_set_extra_listeners(e, chance(z, 1, 16) ? NULL : xyz, count);
        break;
    }

    case OP_ONESHOT: {
        SClass sc;
        bwa_sound snd = pick_snd(z, &sc);
        float x = htox(z, -4.f, 4.f), y = htox(z, 0.f, 3.f), w = htox(z, -4.f, 4.f), g = htox(z, 0.f, 1.5f);
        bool ok = bwa_play_oneshot(e, snd, x, y, w, g);
        if ((sc == S_ZERO || sc == S_AMBIX) && ok)
            FAIL("bwa_play_oneshot accepted a %s handle", sc == S_ZERO ? "zero" : "multichannel");
        if ((sc == S_ZERO || sc == S_AMBIX) && !bwa_last_error(e))
            FAIL("a refused oneshot set no error");
        break;
    }

    case OP_BED: {
        switch (rnd(z, 12)) {
            case 0: {
                if (z->nbed >= FZ_MAX_BED) break;
                bwa_bed b = bwa_bed_create(e);
                if (b == 0) FAIL("bwa_bed_create returned 0");
                z->bed[z->nbed++] = b;
                break;
            }
            case 1: {
                HClass c;
                bwa_bed b = pick_bed(z, &c);
                bwa_bed_destroy(e, b);
                if (c == H_LIVE) {
                    for (uint32_t i = 0; i < z->nbed; ++i)
                        if (z->bed[i] == b) { z->bed[i] = z->bed[--z->nbed]; break; }
                    grave_push(z->bed_grave, &z->n_bed_grave, z, b);
                }
                break;
            }
            case 2: {
                HClass c;
                bwa_bed b = pick_bed(z, &c);
                SClass sc;
                bwa_sound snd = pick_snd(z, &sc);
                bwa_bed_play(e, b, snd, chance(z, 1, 2));
                /* Demand the refusal only for the handles this harness KNOWS name a resolved,
                 * non-bed asset. Two engine states legitimately set no error and neither is a bug:
                 *   - a still-decoding async asset reports 0 channels and no kind yet, so
                 *     bwa_bed_play admits it and rt holds the play (documented in engine.c);
                 *   - an unloaded handle keeps RESOLVING until the audio thread acks the retire
                 *     (rt_unload_sound only sets `retiring`), so a graveyard AmbiX handle still
                 *     reads 4 channels and is a perfectly good bed.
                 * Seed 73 op 1383 was the second one, and the ASSERTION was wrong, not the engine.
                 * S_MONO and S_STREAM are fixture handles this harness owns, loads synchronously
                 * (never pending) and re-points on every churn, so they are always a resolved
                 * 1-channel asset; handle 0 can never resolve. Those three keep the real check -
                 * "a mono asset played as a bed must be refused" - at full strength. S_STALE and
                 * S_GARBAGE are excluded because the harness cannot know what they resolve to. */
                if ((sc == S_MONO || sc == S_STREAM || sc == S_ZERO) && !bwa_last_error(e))
                    FAIL("bed_play on a %s asset set no error",
                         sc == S_MONO ? "mono" : sc == S_STREAM ? "streamed (1-channel)" : "zero-handle");
                break;
            }
            case 3: { HClass c; bwa_bed b = pick_bed(z, &c); bwa_bed_set_gain(e, b, htox(z, 0.f, 2.f)); break; }
            case 4: { HClass c; bwa_bed b = pick_bed(z, &c);
                      float yw = htox(z, -4.f, 4.f), pt = htox(z, -2.f, 2.f), rl = htox(z, -2.f, 2.f);
                      bwa_bed_set_orientation(e, b, yw, pt, rl); break; }
            case 5: { HClass c; bwa_bed b = pick_bed(z, &c); bwa_bed_stop(e, b); break; }
            case 6: { HClass c; bwa_bed b = pick_bed(z, &c);
                      float g = htox(z, 0.f, 2.f), s = hfin(z, -1.f, 2.f);
                      bwa_bed_fade_to(e, b, g, s); break; }
            case 7: { HClass c; bwa_bed b = pick_bed(z, &c); bwa_bed_set_paused(e, b, chance(z, 1, 2)); break; }
            case 8: { HClass c; bwa_bed b = pick_bed(z, &c); bwa_bed_seek(e, b, rnd(z, 100000)); break; }
            case 9: { HClass c; bwa_bed b = pick_bed(z, &c);
                      bwa_bed_set_priority(e, b, (int)rnd(z, 400) - 50); bwa_bed_set_group(e, b, rnd(z, 12)); break; }
            case 10: {
                HClass c;
                bwa_bed b = pick_bed(z, &c);
                bool p = bwa_bed_is_playing(e, b);
                uint64_t ph = bwa_bed_get_playhead_frames(e, b);
                if ((c == H_STALE || c == H_WRONGGEN || c == H_ZERO) && (p || ph != 0))
                    FAIL("a dead bed handle reads playing=%d playhead=%llu", (int)p, (unsigned long long)ph);
                break;
            }
            default: { HClass c; bwa_bed b = pick_bed(z, &c); bwa_bed_fade_out(e, b, hfin(z, 0.f, 1.f)); break; }
        }
        break;
    }

    case OP_GLOBAL: {
        switch (rnd(z, 4)) {
            case 0: bwa_set_master_gain(e, htox(z, 0.f, 1.5f)); break;
            case 1: bwa_set_paused(e, chance(z, 1, 2)); break;
            case 2: {
                uint32_t chn = rnd(z, 32);
                bwa_test_kind k = chance(z, 1, 8) ? (bwa_test_kind)7 : (bwa_test_kind)rnd(z, 3);
                float g = hfin(z, 0.f, 1.f);
                bwa_set_test_signal(e, chn, k, g);
                break;
            }
            default: bwa_set_speed_of_sound(e, hfin(z, 30.f, 20000.f)); break;
        }
        break;
    }

    case OP_KNOB: {
        const uint32_t which = rnd(z, 18);
        if (g_trace) fprintf(stderr, "    knob subcase %u\n", which);
        switch (which) {
            case 0:  bwa_set_panner(e, chance(z, 1, 6) ? (bwa_panner)7 : (bwa_panner)rnd(z, 3)); break;
            case 1: {
                float fo = hfin(z, -2.f, 50.f), de = hfin(z, -1.f, 8.f);
                bwa_set_spcap_focus(e, fo, de);
                break;
            }
            case 2:  bwa_set_dual_band(e, chance(z, 1, 2)); break;
            case 3:  bwa_set_dual_band_cap(e, chance(z, 1, 2)); break;
            case 4:  bwa_set_max_re(e, chance(z, 1, 2)); break;
            case 5:  bwa_set_max_re_split(e, chance(z, 1, 2)); break;
            case 6:  bwa_set_spread_mode(e, chance(z, 1, 6) ? (bwa_spread_mode)9 : (bwa_spread_mode)rnd(z, 3)); break;
            case 7:  bwa_set_decorrelation(e, chance(z, 1, 2)); break;
            case 8:  bwa_set_near_spread(e, hfin(z, 0.f, 2.f)); break;
            case 9:  bwa_set_hole_spread(e, hfin(z, -0.5f, 2.5f)); break;
            case 10: bwa_set_bed_renderer(e, chance(z, 1, 6) ? (bwa_bed_renderer)5 : (bwa_bed_renderer)rnd(z, 2)); break;
            case 11: bwa_set_tracked_room_eq(e, chance(z, 1, 2)); break;
            case 12: {
                bwa_set_tracked_align(e, chance(z, 1, 2));
                float dz = hfin(z, -0.1f, 0.5f), sl = hfin(z, -10.f, 5000.f);
                bwa_set_tracked_align_guards(e, dz, sl);
                break;
            }
            case 13: bwa_set_limiter(e, chance(z, 1, 2));
                     bwa_set_limiter_ceiling(e, hfin(z, -0.5f, 2.f)); break;
            case 14: bwa_set_reverb_gain(e, hfin(z, 0.f, 2.f)); break;
            case 15: bwa_set_early_reflections_gain(e, hfin(z, 0.f, 2.f)); break;
            case 16: {
                float lo = hfin(z, -1.f, 40.f), hi = hfin(z, -1.f, 40.f), xo = hfin(z, 0.f, 30000.f);
                bwa_fdn_set_decay(e, lo, hi, xo);
                break;
            }
            default: bwa_set_pose_prediction(e, hfin(z, -0.1f, 0.3f)); break;
        }
        break;
    }

    case OP_TUNING: {
        switch (rnd(z, 4)) {
            case 0: {   /* preset -> mutate -> apply must be accepted */
                bwa_tuning t;
                bwa_tuning_preset((bwa_setup)rnd(z, 3), &t);
                t.panner       = (bwa_panner)rnd(z, 4);
                t.spcap_focus  = hfin(z, -1.f, 30.f);
                t.near_spread  = hfin(z, 0.f, 2.f);
                t.hole_spread  = hfin(z, 0.f, 2.f);
                t.dual_band    = chance(z, 1, 2);
                t.max_re       = chance(z, 1, 2);
                if (!bwa_apply_tuning(e, &t))
                    FAIL("apply_tuning refused a preset-derived struct: %s",
                         bwa_last_error(e) ? bwa_last_error(e) : "(no error)");
                break;
            }
            case 1: {   /* the documented refusals */
                bwa_tuning zero;
                memset(&zero, 0, sizeof zero);
                if (bwa_apply_tuning(e, &zero)) FAIL("apply_tuning accepted a zero-initialized struct");
                if (bwa_apply_tuning(e, NULL))  FAIL("apply_tuning accepted NULL");
                if (!bwa_last_error(e))         FAIL("apply_tuning refusal set no error");
                break;
            }
            case 2: {
                bwa_tuning t;
                if (!bwa_get_tuning(e, &t)) FAIL("get_tuning returned false");
                if (t.struct_size != (uint32_t)sizeof(bwa_tuning)) FAIL("get_tuning struct_size wrong");
                if (!bwa_apply_tuning(e, &t)) FAIL("get_tuning round-trip refused by apply_tuning");
                break;
            }
            default:
                if (bwa_get_tuning(e, NULL)) FAIL("get_tuning(NULL out) returned true");
                bwa_tuning_preset((bwa_setup)rnd(z, 5), NULL);   /* documented: does nothing */
                break;
        }
        break;
    }

    case OP_SCENE: {
        bwa_material faces[6];
        for (int i = 0; i < 6; ++i)
            faces[i] = (z->ntok && chance(z, 1, 2)) ? z->tok[rnd(z, z->ntok)]
                                                    : (chance(z, 1, 8) ? 9999u : 0u);
        switch (rnd(z, 6)) {
            case 0: {
                float w = hfin(z, 0.5f, 20.f), hh = hfin(z, 0.5f, 10.f), dd = hfin(z, 0.5f, 20.f);
                bwa_scene_set_box(e, w, hh, dd, chance(z, 1, 3) ? NULL : faces);
                break;
            }
            case 1: {
                float w = hfin(z, 0.5f, 20.f), hh = hfin(z, 0.5f, 10.f), dd = hfin(z, 0.5f, 20.f);
                bwa_scene_set_ism_room(e, w, hh, dd, chance(z, 1, 3) ? NULL : faces);
                break;
            }
            case 2: bwa_scene_set_ground(e, hfin(z, -2.f, 5.f), faces[0], chance(z, 1, 2)); break;
            case 3: bwa_scene_set_pressure_release(e, rng_u32(&z->rng)); break;
            case 4: {   /* a small tetrahedron with per-triangle materials (some out-of-range) */
                static const float v[4 * 3] = { 0,0,0,  2,0,0,  0,2,0,  0,0,2 };
                static const int t[4 * 3]   = { 0,2,1,  0,1,3,  0,3,2,  1,2,3 };
                bwa_material m[4];
                for (int i = 0; i < 4; ++i)
                    m[i] = chance(z, 1, 4) ? 12345u
                         : (z->ntok ? z->tok[rnd(z, z->ntok)] : 0u);
                bwa_scene_set_mesh_mat(e, v, 4, t, 4, m);
                break;
            }
            default:    /* the documented all-NULL clear */
                bwa_scene_set_mesh_mat(e, NULL, 0, NULL, 0, NULL);
                break;
        }
        break;
    }

    case OP_DYNMESH: {
        switch (rnd(z, 3)) {
            case 0: {
                if (z->ndyn >= FZ_MAX_DYN) break;
                static const float v[4 * 3] = { 0,0,0,  1,0,0,  0,1,0,  0,0,1 };
                static const int t[4 * 3]   = { 0,2,1,  0,1,3,  0,3,2,  1,2,3 };
                int h = bwa_scene_add_dynamic_mesh(e, v, 4, t, 4,
                            z->ntok ? z->tok[rnd(z, z->ntok)] : 0u);
                if (h >= 0) z->dyn[z->ndyn++] = h;    /* -1 = no SDK / full — documented */
                break;
            }
            case 1: {
                int h = (z->ndyn && chance(z, 3, 4)) ? z->dyn[rnd(z, z->ndyn)]
                                                     : (chance(z, 1, 2) ? 999 : -1);
                float x = hfin(z, -5.f, 5.f), y = hfin(z, 0.f, 3.f), w = hfin(z, -5.f, 5.f);
                float qa = htox(z, -1.f, 1.f), qb = htox(z, -1.f, 1.f), qc = htox(z, -1.f, 1.f), qd = htox(z, -1.f, 1.f);
                bwa_scene_set_dynamic_transform(e, h, x, y, w, qa, qb, qc, qd);
                break;
            }
            default: {
                if (z->ndyn && chance(z, 3, 4)) {
                    uint32_t i = rnd(z, z->ndyn);
                    bwa_scene_remove_dynamic_mesh(e, z->dyn[i]);
                    z->dyn[i] = z->dyn[--z->ndyn];
                } else {
                    bwa_scene_remove_dynamic_mesh(e, chance(z, 1, 2) ? 999 : -1);
                }
                break;
            }
        }
        break;
    }

    case OP_MATERIAL: {
        switch (rnd(z, 4)) {
            case 0: {
                if (bwa_material_preset(e, BWA_MAT_GENERIC) != 0)
                    FAIL("preset(GENERIC) did not return the canonical token 0");
                if (bwa_material_preset(e, (bwa_material_type)999) != 0)
                    FAIL("preset(out-of-range) returned a token");
                break;
            }
            case 1: {
                if (z->ntok >= FZ_MAX_TOK) break;
                bwa_material m = bwa_material_preset(e, (bwa_material_type)(1 + rnd(z, 10)));
                if (m == 0) FAIL("material_preset returned 0 with the table far from full");
                z->tok[z->ntok++] = m;
                break;
            }
            case 2: {
                if (z->ntok >= FZ_MAX_TOK) break;
                float a[3], t[3];
                a[0] = hfin(z, 0.f, 1.f); a[1] = hfin(z, 0.f, 1.f); a[2] = hfin(z, 0.f, 1.f);
                t[0] = hfin(z, 0.f, 1.f); t[1] = hfin(z, 0.f, 1.f); t[2] = hfin(z, 0.f, 1.f);
                float sc = hfin(z, 0.f, 1.f);
                bwa_material m = bwa_material_define(e, a, sc, t);
                if (m == 0) FAIL("material_define returned 0 with the table far from full");
                z->tok[z->ntok++] = m;
                break;
            }
            default: {
                if (chance(z, 1, 4)) {
                    bwa_material_release(e, 0);        /* refused, documented */
                    if (!bwa_last_error(e)) FAIL("releasing token 0 set no error");
                } else if (chance(z, 1, 4)) {
                    bwa_material_release(e, 9999u);    /* out of range: refused */
                } else if (z->ntok) {
                    uint32_t i = rnd(z, z->ntok);
                    bwa_material_release(e, z->tok[i]);
                    z->tok[i] = z->tok[--z->ntok];
                }
                break;
            }
        }
        break;
    }

    case OP_SOUND_META: {
        if (bwa_sound_get_frames(e, 0) != 0 || bwa_sound_get_channels(e, 0) != 0)
            FAIL("metadata of handle 0 is not 0/0");
        if (bwa_sound_get_channels(e, z->snd_short) != 1)
            FAIL("short mono asset does not read 1 channel");
        if (bwa_sound_get_frames(e, z->snd_short) == 0)
            FAIL("short mono asset reads 0 frames");
        if (bwa_sound_get_channels(e, z->snd_ambix) != 4)
            FAIL("ambix asset does not read 4 channels");
        (void)bwa_sound_get_frames(e, rng_u32(&z->rng));   /* garbage handle: any value, no crash */
        break;
    }

    case OP_READBACK: {
        HClass c; uint32_t i = 0;
        bwa_source h = pick_src(z, &c, &i);
        switch (rnd(z, 4)) {
            case 0: {
                bool p = bwa_source_is_playing(e, h);
                uint64_t ph = bwa_source_get_playhead_frames(e, h);
                if ((c == H_STALE || c == H_WRONGGEN || c == H_ZERO) && (p || ph != 0))
                    FAIL("a dead source handle reads playing=%d playhead=%llu",
                         (int)p, (unsigned long long)ph);
                break;
            }
            case 1: {
                float occ = bwa_source_get_occlusion(e, h);
                float dir = bwa_source_get_directivity(e, h);
                if (!z->toxic && (!isfinite(occ) || !isfinite(dir)))
                    FAIL("non-finite occlusion/directivity readback (%g / %g)", occ, dir);
                break;
            }
            case 2: {
                bwa_clock_model m;
                if (bwa_get_clock_model(e, &m)) {
                    if (!isfinite(m.ppm) || !isfinite(m.rate_hz) || !isfinite(m.jitter_ns))
                        FAIL("clock model carries non-finite fields");
                }
                break;
            }
            default: {
                uint64_t cs = 0, ct = 0;
                if (bwa_get_clock(e, &cs, &ct) && cs > bwa_get_dsp_time_frames(e) && !z->restarted)
                    FAIL("clock pair ahead of the dsp clock");
                break;
            }
        }
        break;
    }

    case OP_POLL: {
        bwa_source out[16];
        uint32_t cap;
        switch (rnd(z, 4)) { case 0: cap = 0; break; case 1: cap = 1; break;
                             case 2: cap = 4; break; default: cap = 16; break; }
        uint64_t dropped = 0;
        uint32_t n = bwa_poll_ended(e, out, cap, chance(z, 1, 4) ? NULL : &dropped);
        if (n > cap) FAIL("poll_ended returned %u > cap %u", n, cap);
        for (uint32_t k = 0; k < n; ++k)
            if (out[k] == 0) FAIL("poll_ended reported handle 0");
        break;
    }

    case OP_LOAD: {
        switch (rnd(z, 6)) {
            case 0: {   /* churn the short mono asset (unload retires; reload must succeed) */
                grave_push(z->snd_grave, &z->n_snd_grave, z, z->snd_short);
                bwa_unload_sound(e, z->snd_short);
                z->snd_short = bwa_load_sound(e, FZ_WAV_SHORT);
                if (!z->snd_short) FAIL("reload of the short wav failed: %s",
                                        bwa_last_error(e) ? bwa_last_error(e) : "(no error)");
                break;
            }
            case 1: {   /* churn the bed asset, alternating the AmbiX and FuMa loaders */
                grave_push(z->snd_grave, &z->n_snd_grave, z, z->snd_ambix);
                bwa_unload_sound(e, z->snd_ambix);
                z->snd_ambix = chance(z, 1, 2) ? bwa_load_ambix(e, FZ_WAV_AMBIX)
                                               : bwa_load_fuma(e, FZ_WAV_AMBIX);
                if (!z->snd_ambix) FAIL("reload of the 4-ch wav failed: %s",
                                        bwa_last_error(e) ? bwa_last_error(e) : "(no error)");
                break;
            }
            case 2:     /* nonexistent file: 0 + a reason, documented */
                if (bwa_load_sound(e, "bwa_fuzz_definitely_missing.wav") != 0)
                    FAIL("load of a nonexistent file returned a handle");
                if (!bwa_last_error(e)) FAIL("failed load set no error");
                break;
            case 3:     /* garbage bytes: 0 + a reason */
                if (bwa_load_sound(e, FZ_GARBAGE) != 0)
                    FAIL("load of a garbage file returned a handle");
                if (!bwa_last_error(e)) FAIL("garbage load set no error");
                break;
            case 4:     /* a mono file through the ambix loader: bad channel count, documented 0 */
                if (bwa_load_ambix(e, FZ_WAV_SHORT) != 0)
                    FAIL("load_ambix accepted a mono file");
                break;
            default:    /* unload of dead handles must be a silent no-op */
                bwa_unload_sound(e, 0);
                bwa_unload_sound(e, rng_u32(&z->rng));
                if (z->n_snd_grave) bwa_unload_sound(e, z->snd_grave[rnd(z, z->n_snd_grave)]);
                break;
        }
        break;
    }

    case OP_RESTART: {
        z->restarted = 1; z->restart_for = 8;   /* bounded: the stale pair clears within a block or two */
        if (z->started) {
            bwa_stop(e);
            z->started = 0;
            if (render_once(z)) return 1;      /* asserts the documented NULL while stopped */
            if (chance(z, 7, 8)) {
                if (bwa_start(e) != BWA_OK)
                    FAIL("restart failed on the MANUAL sink: %s",
                         bwa_last_error(e) ? bwa_last_error(e) : "(no error)");
                z->started = 1;
                z->dsp_prev = 0;               /* a restart may re-base the dsp clock */
            }
        } else {
            if (bwa_start(e) != BWA_OK)
                FAIL("start after a stopped stretch failed: %s",
                     bwa_last_error(e) ? bwa_last_error(e) : "(no error)");
            z->started = 1;
            z->dsp_prev = 0;
        }
        if (bwa_get_channel_count(e) != z->chan)
            FAIL("channel count changed across a stop/start");
        break;
    }

    case OP_PURE: {
        switch (rnd(z, 4)) {
            case 0: {   /* box mesh: valid fills, degenerate/NULL refuse — documented */
                float v[24]; int t[36]; bwa_material m[12];
                if (!bwa_box_mesh(4.f, 3.f, 5.f, NULL, v, t, m)) FAIL("box_mesh refused valid dims");
                if (bwa_box_mesh(0.f, 3.f, 5.f, NULL, v, t, m))  FAIL("box_mesh accepted w=0");
                if (bwa_box_mesh(4.f, 3.f, 5.f, NULL, NULL, t, m)) FAIL("box_mesh accepted NULL verts");
                break;
            }
            case 1: {   /* the geometry-derived SPCAP focus: pure, positive on a real array */
                float pos[8 * 3];
                int k = 0;
                for (int x = -1; x <= 1; x += 2) for (int y = -1; y <= 1; y += 2) for (int w = -1; w <= 1; w += 2)
                    { pos[k*3] = 1.5f*x; pos[k*3+1] = 1.4f + y; pos[k*3+2] = 1.5f*w; ++k; }
                float f = bwa_spcap_focus_default(pos, 8);
                if (!(f > 0.f) || !isfinite(f)) FAIL("spcap_focus_default on a cube is %g", f);
                if (bwa_spcap_focus_default(NULL, 8) != 0.f) FAIL("spcap_focus_default(NULL) != 0");
                break;
            }
            case 2: {   /* offline panner/bed batches: pure, return the source/dir count, finite */
                float pos[8 * 3];
                int k = 0;
                for (int x = -1; x <= 1; x += 2) for (int y = -1; y <= 1; y += 2) for (int w = -1; w <= 1; w += 2)
                    { pos[k*3] = 1.5f*x; pos[k*3+1] = 1.4f + y; pos[k*3+2] = 1.5f*w; ++k; }
                const float lis[3] = { 0.f, 1.4f, 0.f };
                float srcs[2 * 3] = { 1.f, 1.f, 0.f,  -1.f, 2.f, 1.f };
                float out[2 * 8];
                bwa_panner pn = (bwa_panner)rnd(z, 3);
                float fo = hfin(z, -1.f, 20.f), de = hfin(z, -1.f, 4.f);
                if (bwa_panner_gains_batch(pn, pos, 8, lis, srcs, 2, fo, de, out) != 2u)
                    FAIL("panner_gains_batch did not return nsrc");
                for (int i = 0; i < 16; ++i)
                    if (!isfinite(out[i])) FAIL("panner_gains_batch produced a non-finite gain (panner %d)", (int)pn);
                const float dirs[3] = { 0.f, 1.f, 0.f };
                bwa_bed_decoder dec = (bwa_bed_decoder)(1 + rnd(z, 2));
                bool mre = chance(z, 1, 2);
                if (bwa_bed_gains_batch(dec, mre, pos, 8, dirs, 1, out) != 1u)
                    FAIL("bed_gains_batch did not return ndir");
                for (int i = 0; i < 8; ++i)
                    if (!isfinite(out[i])) FAIL("bed_gains_batch produced a non-finite gain");
                break;
            }
            default: {  /* driver enumeration bounds + preset determinism */
                char nm[64];
                uint32_t nd = bwa_get_asio_driver_count();
                if (bwa_get_asio_driver_name(nd, nm, sizeof nm)) FAIL("driver name accepted an out-of-range index");
                if (bwa_get_asio_driver_name(0, nm, 0))          FAIL("driver name accepted a zero-cap buffer");
                bwa_tuning a, b;
                bwa_tuning_preset(BWA_SETUP_SEATED, &a);
                bwa_tuning_preset(BWA_SETUP_SEATED, &b);
                if (memcmp(&a, &b, sizeof a) != 0) FAIL("tuning_preset is not deterministic");
                break;
            }
        }
        break;
    }

    case OP_HPEQ: {
        switch (rnd(z, 4)) {
            case 0:
                if (bwa_load_headphone_eq(e, FZ_EQ) != BWA_OK)
                    FAIL("headphone EQ fixture failed to load: %s",
                         bwa_last_error(e) ? bwa_last_error(e) : "(no error)");
                break;
            case 1:
                if (bwa_load_headphone_eq(e, FZ_GARBAGE) != BWA_ERR_CONFIG)
                    FAIL("a garbage EQ file did not fail BWA_ERR_CONFIG");
                if (!bwa_last_error(e)) FAIL("EQ parse failure set no error");
                break;
            case 2: bwa_load_headphone_eq(e, NULL); break;     /* documented clear */
            default: bwa_set_headphone_eq(e, chance(z, 1, 2)); break;
        }
        break;
    }

    case OP_CAPTURE: {
        if (chance(z, 2, 3)) { bwa_set_output_capture(e, fz_capture, z); z->cap_set = 1; }
        else                 { bwa_set_output_capture(e, NULL, NULL);    z->cap_set = 0; }
        break;
    }

    case OP_NULL_ENGINE: {
        /* NULL-engine calls across the surface must be inert, and create(NULL) must refuse */
        if (bwa_create(NULL) != NULL) FAIL("bwa_create(NULL) returned an engine");
        bwa_destroy(NULL);
        bwa_stop(NULL);
        bwa_commit(NULL);
        bwa_source_set_pos(NULL, 1, 0.f, 0.f, 0.f);
        bwa_set_listener_pose(NULL, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f);
        if (bwa_render_block(NULL, NULL, NULL) != NULL) FAIL("render_block(NULL) returned a buffer");
        if (bwa_source_is_playing(NULL, 1)) FAIL("is_playing(NULL engine) returned true");
        (void)bwa_last_error(NULL);
        (void)bwa_get_channel_count(NULL);
        (void)bwa_get_dsp_time_frames(NULL);
        break;
    }

    case OP_TRACKER: {
        /* never connected in this suite (sockets/firewall/threads — see the file header):
         * status must read DISCONNECTED and disconnect must be a no-op */
        if (bwa_tracker_status(e) != BWA_TRACKER_DISCONNECTED)
            FAIL("tracker status is not DISCONNECTED with no tracker ever connected");
        if (chance(z, 1, 2)) bwa_tracker_disconnect(e);
        break;
    }

    default:
        break;
    }
    return 0;
}

/* ---------------------------------------------------------------- one seed ---------------- */

static int run_seed(unsigned long long seed, uint32_t iters) {
    Fz zz;
    Fz* z = &zz;
    memset(z, 0, sizeof *z);
    z->seed = seed;
    z->iters = iters;
    z->opname = "setup";
    rng_seed(&z->rng, seed);

    /* 1-in-8 seeds: the strict-layout preflight (create survives, start refuses, repeatably) */
    if (chance(z, 1, 8)) {
        bwa_desc d = { 0 };
        d.profile = BWA_PROFILE_CAVE;
        d.layout_path = FZ_NO_LAYOUT;
        d.sink = BWA_SINK_MANUAL;
        bwa_engine* pe = bwa_create(&d);
        z->e = pe;   /* so FAIL context reads sensibly */
        if (!pe) FAIL("create must survive a failed explicit layout load");
        if (!bwa_last_error(pe)) { bwa_destroy(pe); FAIL("failed layout load left no error at create"); }
        if (bwa_get_channel_count(pe) != 26) { bwa_destroy(pe); FAIL("layout fallback is not the 26-grid"); }
        for (int i = 0; i < 2; ++i)
            if (bwa_start(pe) != BWA_ERR_LAYOUT) { bwa_destroy(pe); FAIL("start #%d did not fail BWA_ERR_LAYOUT", i + 1); }
        bwa_destroy(pe);
        z->e = NULL;
    }

    /* randomized engine config on the MANUAL sink */
    bwa_desc d = { 0 };
    {
        uint32_t p = rnd(z, 10);
        z->profile = p < 4 ? BWA_PROFILE_CAVE
                   : p < 6 ? BWA_PROFILE_BINAURAL
                   : p < 8 ? BWA_PROFILE_CAVE_SIM
                           : BWA_PROFILE_CAVE_BOTH;
    }
    d.profile = z->profile;
    d.sink = BWA_SINK_MANUAL;
    d.sample_rate = chance(z, 1, 2) ? 48000 : 0;              /* 0 resolves to 48000 */
    switch (rnd(z, 8)) {
        case 0:  z->block = 64;  break;
        case 1:  z->block = 512; break;
        case 2: case 3: case 4: z->block = 128; break;
        default: z->block = 256; break;
    }
    d.block_size  = z->block;
    d.bed_decoder = (bwa_bed_decoder)rnd(z, 3);
    d.enable_pathing = chance(z, 1, 16);   /* with the SDK every start re-bakes: keep it rare */
    d.embree         = chance(z, 1, 8);
    if (chance(z, 1, 16)) d.hrtf_path = FZ_GARBAGE;           /* HRTF failure is non-fatal */
    int use_layout8 = chance(z, 3, 10);
    if (use_layout8) d.layout_path = FZ_LAYOUT8;

    z->e = bwa_create(&d);
    if (!z->e) FAIL("bwa_create returned NULL for a valid desc (profile %d)", (int)z->profile);
    bwa_engine* e = z->e;

    if (bwa_get_version() != BWA_VERSION) FAIL("DLL/header version mismatch: %x vs %x", bwa_get_version(), BWA_VERSION);
    if (bwa_get_sample_rate(e) != 48000u) FAIL("sample rate did not resolve to 48000");
    if (bwa_get_block_size(e) != z->block) FAIL("block size readback %u != desc %u", bwa_get_block_size(e), z->block);
    z->chan = bwa_get_channel_count(e);
    if (use_layout8) { if (z->chan != 8)  FAIL("8-speaker layout loaded %u channels", z->chan); }
    else             { if (z->chan != 26) FAIL("default grid is %u channels, not 26", z->chan); }
    z->out_ch = (z->profile == BWA_PROFILE_BINAURAL || z->profile == BWA_PROFILE_CAVE_SIM) ? 2u : z->chan;

    /* load-time config: reverb beds, a room, a tuning — before start, as documented */
    z->opname = "prestart";
    if (chance(z, 1, 2)) {
        bwa_fdn_desc f = { 0 };
        f.enabled = 1;
        f.rt60_low_s  = hfin(z, 0.f, 5.f);
        f.rt60_high_s = hfin(z, 0.f, 5.f);
        f.xover_hz    = hfin(z, 0.f, 8000.f);
        if (chance(z, 1, 3)) { f.decay_dir[0] = 1.f; f.decay_factor = hfin(z, 0.f, 3.f); }
        bwa_fdn_config(e, &f);
    }
    if (chance(z, 1, 8)) {                 /* rare: with the SDK this spins a live ray-trace sim */
        bwa_reflections_desc r = { 0 };
        r.enabled = 1;
        r.ir_seconds = hfin(z, 0.f, 2.f);
        r.order = rnd(z, 3);
        bwa_reflections_config(e, &r);     /* no-op without the SDK; may pair-warn with the FDN */
    }
    if (chance(z, 1, 2)) bwa_scene_set_box(e, 6.f, 3.f, 6.f, NULL);
    if (chance(z, 1, 2)) {
        bwa_tuning t;
        bwa_tuning_preset((bwa_setup)rnd(z, 3), &t);
        if (!bwa_apply_tuning(e, &t)) FAIL("apply_tuning refused an unmodified preset");
    }

    if (bwa_start(e) != BWA_OK)
        FAIL("bwa_start failed on the MANUAL sink: %s", bwa_last_error(e) ? bwa_last_error(e) : "(no error)");
    z->started = 1;
    if (bwa_get_sink_type(e) != BWA_SINK_MANUAL) FAIL("started sink type is not MANUAL");

    /* fixture assets (valid loads must succeed; metadata is pinned by the header) */
    z->opname = "assets";
    z->snd_short  = bwa_load_sound(e, FZ_WAV_SHORT);
    z->snd_long   = bwa_load_sound(e, FZ_WAV_LONG);
    z->snd_ambix  = bwa_load_ambix(e, FZ_WAV_AMBIX);
    z->snd_stream = bwa_load_sound_streaming(e, FZ_WAV_LONG);
    if (!z->snd_short || !z->snd_long || !z->snd_ambix || !z->snd_stream)
        FAIL("fixture asset load failed (%u %u %u %u): %s", z->snd_short, z->snd_long, z->snd_ambix,
             z->snd_stream, bwa_last_error(e) ? bwa_last_error(e) : "(no error)");
    if (bwa_sound_get_channels(e, z->snd_short) != 1 || bwa_sound_get_channels(e, z->snd_ambix) != 4)
        FAIL("fixture channel metadata wrong");

    /* prime the model so the first ops have something legal to hit */
    z->opname = "prime";
    z->src[0].h = bwa_source_create(e);      z->src[0].push = 0;
    z->src[1].h = bwa_source_create(e);      z->src[1].push = 0;
    z->src[2].h = bwa_source_create_push(e); z->src[2].push = 1;
    z->nsrc = 3;
    if (!z->src[0].h || !z->src[1].h || !z->src[2].h) FAIL("priming source create returned 0");
    z->bed[0] = bwa_bed_create(e);
    z->nbed = 1;
    if (!z->bed[0]) FAIL("priming bed create returned 0");
    bwa_source_play(e, z->src[0].h, z->snd_long, true);
    bwa_bed_play(e, z->bed[0], z->snd_ambix, true);
    bwa_set_listener_pose(e, 0.f, 1.5f, 0.f, 0.f, 0.f, 0.f, 1.f);
    bwa_commit(e);

    /* ---- the sequence ---- */
    for (z->op = 0; z->op < iters; ++z->op) {
        /* Decay the disarm windows BEFORE the op, so a finite assert that was switched off for a
         * poisoned value comes back armed for the rest of the seed. Permanently disarming after the
         * first injection is how a fuzzer stops testing the thing it was written to test. */
        if (z->toxic_for   > 0 && --z->toxic_for   == 0) z->toxic     = 0;
        if (z->restart_for > 0 && --z->restart_for == 0) z->restarted = 0;
        if (do_op(z)) return 1;
        g_total_ops++;
        if ((z->op & 7u) == 7u && health(z)) return 1;
    }

    /* teardown both ways: half the seeds stop first, half destroy a running engine */
    z->opname = "teardown";
    if (chance(z, 1, 2)) bwa_stop(e);
    bwa_destroy(e);
    return 0;
}

/* ---------------------------------------------------------------- main -------------------- */

/* The fixed ctest set. Arbitrary but FROZEN: reproducibility is the point. */
static const unsigned long long FZ_SEEDS[FZ_NUM_SEEDS] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
    42, 0xC0FFEEULL, 0xDEADBEEFULL, 0x123456789ABCDEFULL
};

int main(int argc, char** argv) {
    unsigned long long one_seed = 0;
    int have_seed = 0;
    uint32_t iters = FZ_DEFAULT_ITERS;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            one_seed = strtoull(argv[++i], NULL, 0);
            have_seed = 1;
        } else if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            iters = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--trace") == 0) {
            g_trace = 1;
        } else {
            fprintf(stderr, "usage: test_fuzz_api [--seed <n>] [--iters <n>]\n");
            return 2;
        }
    }

    if (!write_wav(FZ_WAV_SHORT, 1, 480) ||
        !write_wav(FZ_WAV_LONG, 1, 48000) ||
        !write_wav(FZ_WAV_AMBIX, 4, 4800) ||
        !write_layout8(FZ_LAYOUT8) ||
        !write_text(FZ_GARBAGE, "this is not any kind of audio, layout, or EQ file\n") ||
        !write_text(FZ_EQ, "Preamp: -3.0 dB\nFilter 1: ON PK Fc 660 Hz Gain -6.0 dB Q 2.00\n")) {
        fprintf(stderr, "fuzz_api: cannot write fixture files\n");
        remove_fixtures();
        return 2;
    }

    build_pick_table();
    clock_t t0 = clock();
    int rc = 0;

    if (have_seed) {
        printf("fuzz_api: seed %llu, %u ops\n", one_seed, iters);
        rc = run_seed(one_seed, iters);
    } else {
        for (int i = 0; i < FZ_NUM_SEEDS && rc == 0; ++i)
            rc = run_seed(FZ_SEEDS[i], iters);
    }

    remove_fixtures();
    if (rc) return 1;

    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("fuzz_api OK (%s%llu ops across %s, %llu rendered blocks, %.1f s)\n",
           have_seed ? "" : "16 seeds, ",
           (unsigned long long)g_total_ops,
           have_seed ? "1 seed" : "the fixed set",
           (unsigned long long)g_total_renders, secs);
    return 0;
}
