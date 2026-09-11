/*
 * ambisonic.c — the ambisonic-bed walkthrough: load a pre-encoded soundfield and drive every
 * bed control by ear.
 *
 *   bwa_load_ambix / bwa_load_fuma   load a 4/9/16-ch B-format asset (FuMa converts at load)
 *   bwa_bed_create / bwa_bed_play    a bed is a voice playing a multichannel asset
 *   bwa_bed_set_orientation          full 3-axis: yaw the field (glided at ~1 turn/s, click-free),
 *                                    or level/tilt a capture (pitch/roll)
 *   bwa_set_bed_renderer             matrix decode vs parametric (DirAC) — live A/B
 *   bwa_set_max_re                   max-rE decode weighting — live A/B
 *   bwa_bed_play_at / _play_loop     a bed IS a voice: the scheduled start and the intro->loop
 *   bwa_bed_set_region / _stop_at    region, and the scheduled click-free stop, bed-typed
 *   bwa_poll_looped                  a looping voice never ends, so the WRAP is the event
 *
 * No assets needed: a 3rd-order AmbiX wav is synthesized (pink bursts from the FRONT, a click
 * train from the LEFT-UP, a low diffuse floor), plus the SAME field written in FuMa channel
 * order/normalization — the two loads must sound identical, which is the point of bwa_load_fuma.
 *
 * The two loaders here are the EXPLICIT tier (you loaded it, you unload it). The shared tier
 * spells the same two loads bwa_sound_acquire(path, BWA_LOAD_AMBIX) and BWA_LOAD_FUMA, which is
 * how one file can be resident in more than one form at once; see bwa_convenience.
 *
 * Runs anywhere: binaural profile out of the platform's default stereo output (WASAPI on
 * Windows, JACK or ALSA on Linux), with a silent null-sink fallback if nothing opens
 * (bwa_get_audio_backend says which you got).
 *
 *   bwa_ambisonic
 */
#include "bw_audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "portable.h"      /* bwa_sleep_ms: the demo's only OS call */

#define RATE   48000u
#define SECS   4u
#define FRAMES (RATE * SECS)

/* -- scaffolding: synthesize the field. A real client ships recorded/DAW-encoded files. -- */

/* SN3D real spherical harmonics to order 3 (the AmbiX set), taking a ROOM direction (right-handed,
 * +y up, +z front) — the ambi axes are x=front,y=left,z=up, i.e. (room z, room x, room y). */
static void sh16_room(float rx, float ry, float rz, float y[16]) {
    const float len = sqrtf(rx*rx + ry*ry + rz*rz);
    const float x = rz / len, yy = rx / len, z = ry / len;      /* room -> ambi, normalized */
    y[0]  = 1.0f;
    y[1]  = yy;             y[2]  = z;              y[3]  = x;
    y[4]  = 1.7320508f * x * yy;
    y[5]  = 1.7320508f * yy * z;
    y[6]  = 0.5f * (3.0f * z * z - 1.0f);
    y[7]  = 1.7320508f * x * z;
    y[8]  = 0.8660254f * (x * x - yy * yy);
    y[9]  = 0.7905694f * yy * (3.0f * x * x - yy * yy);
    y[10] = 3.8729833f * x * yy * z;
    y[11] = 0.6123724f * yy * (5.0f * z * z - 1.0f);
    y[12] = 0.5f * z * (5.0f * z * z - 3.0f);
    y[13] = 0.6123724f * x * (5.0f * z * z - 1.0f);
    y[14] = 1.9364917f * z * (x * x - yy * yy);
    y[15] = 0.7905694f * x * (x * x - 3.0f * yy * yy);
}

static float white(unsigned int* s) {
    *s = *s * 1664525u + 1013904223u;
    return (float)((int)(*s >> 9) - (1 << 22)) / (float)(1 << 22);
}
static float pink(float w, float b[7]) {                        /* Paul Kellet pink filter */
    b[0] = 0.99886f * b[0] + w * 0.0555179f;  b[1] = 0.99332f * b[1] + w * 0.0750759f;
    b[2] = 0.96900f * b[2] + w * 0.1538520f;  b[3] = 0.86650f * b[3] + w * 0.3104856f;
    b[4] = 0.55000f * b[4] + w * 0.5329522f;  b[5] = -0.7616f * b[5] - w * 0.0168980f;
    float p = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362f;
    b[6] = w * 0.115926f;
    return p * 0.11f;
}

/* the field: pink bursts from the front, an offset click train from the left-up, diffuse floor */
static void gen_field(float* buf /* FRAMES x 16 */) {
    float yf[16], yl[16];
    sh16_room(0.0f, 0.0f, 1.0f, yf);                            /* front  (room +z) */
    sh16_room(0.87f, 0.5f, 0.0f, yl);                           /* left-up (room +x is LEFT) */
    unsigned int s1 = 33333u, s2 = 44444u, s3 = 55555u;
    float pb1[7] = { 0 }, pb3[7] = { 0 };
    const unsigned period = RATE / 2, on = period / 2, ramp = RATE / 100, clicklen = RATE / 333;
    for (unsigned i = 0; i < FRAMES; ++i) {
        unsigned ph = i % period;
        float env = (ph < ramp) ? (float)ph / ramp
                  : (ph < on - ramp) ? 1.0f
                  : (ph < on) ? (float)(on - ph) / ramp : 0.0f;
        float burst = pink(white(&s1), pb1) * 0.5f * env;
        unsigned pc = (i + period / 2) % period;                /* clicks in the bursts' gaps */
        float click = (pc < clicklen) ? white(&s2) * expf(-6.0f * (float)pc / clicklen) * 0.7f : 0.0f;
        float dif   = pink(white(&s3), pb3) * 0.08f;            /* W-only: reads as diffuse */
        float* f = buf + (size_t)i * 16;
        for (int k = 0; k < 16; ++k) f[k] = burst * yf[k] + click * yl[k];
        f[0] += dif;
    }
}

/* hand-written RIFF float32 wav, `ch` interleaved channels (like minimal.c's ping writer) */
static void put_u32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put_u16(FILE* f, uint16_t v) { fwrite(&v, 2, 1, f); }
static int write_wavf(const char* path, const float* buf, uint32_t frames, uint16_t ch) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    const uint32_t bytes = frames * ch * 4;
    fwrite("RIFF", 1, 4, f); put_u32(f, 36 + bytes); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put_u32(f, 16); put_u16(f, 3 /* IEEE float */); put_u16(f, ch);
    put_u32(f, RATE); put_u32(f, RATE * ch * 4); put_u16(f, (uint16_t)(ch * 4)); put_u16(f, 32);
    fwrite("data", 1, 4, f); put_u32(f, bytes); fwrite(buf, 1, bytes, f);
    fclose(f);
    return 1;
}

/* AmbiX -> FuMa, to write the legacy variant: FuMa channel i carries ACN fuma_acn[i], scaled by
 * the SN3D->FuMa factor (the inverse of what bwa_load_fuma applies — same published table). */
static void ambix_to_fuma(const float* ambi, float* fuma, uint32_t frames) {
    static const int   acn[16] = { 0, 3, 1, 2, 6, 7, 5, 8, 4, 12, 13, 11, 14, 10, 15, 9 };
    static const float s2f[16] = { 0.70710678f, 1.f, 1.f, 1.f,
                                   1.f, 1.15470054f, 1.15470054f, 1.15470054f, 1.15470054f,
                                   1.f, 1.18585412f, 1.18585412f, 1.34164079f, 1.34164079f,
                                   1.26491106f, 1.26491106f };
    for (uint32_t i = 0; i < frames; ++i)
        for (int k = 0; k < 16; ++k)
            fuma[(size_t)i * 16 + k] = ambi[(size_t)i * 16 + acn[k]] * s2f[k];
}


/* --tests: force the offline sink and cut the waits, so ctest runs this without a device. The
 * CALLS are identical either way; only the listening time goes. */
static int g_tests = 0;
static int ticks(int n) { return g_tests ? (n < 8 ? 1 : n / 8) : n; }

/* Part 7 is self-checking: the A/Bs above are for your ears, but a scheduled start, a play
 * region and a scheduled stop each have a definite right answer, so ctest running this catches a
 * broken one instead of only a crash. Each check below is demanded with a MARGIN over what the
 * broken mechanism would produce, because a check whose two sides both land near zero when the
 * mechanism breaks is a coin flip. */
static int g_bad = 0;
static void check(int ok, const char* what) {
    printf("   %-52s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++g_bad;
}

/* The timing rule for part 7: no check may conclude from how long a SLEEP took. A sleep is a
 * request, not a promise - a 16 ms sleep measured about 100 ms on the macOS CI runner, and a
 * loaded Windows box does the same - so a fixed number of them says nothing about where the
 * engine's clock is. Every claim about a SCHEDULED sample therefore reads bwa_get_dsp_time_frames
 * at the moment of the check:
 *   "not yet" checks are only meaningful while the clock is still short of the sample. Past it,
 *   this run cannot tell a working schedule from a broken one, so it says INCONCLUSIVE rather
 *   than passing (which would imply coverage it does not have) or failing (which would blame the
 *   engine for a slow machine).
 *   "it happened" checks wait for the CLOCK to reach the sample, bounded by wall time so a
 *   stalled clock fails loudly instead of hanging. */
static int g_incon = 0;
static void inconclusive(const char* what, uint64_t sched, uint64_t now) {
    printf("   %-52s INCONCLUSIVE (scheduled %llu, dsp clock %llu: this run\n"
           "   %-52s  reached the sample before the check could look)\n",
           what, (unsigned long long)sched, (unsigned long long)now, "");
    ++g_incon;
}

/* Was the clock still short of `sched` when the readback just taken was read? Read the clock
 * AFTER the readback: if it has not reached the sample now, it had not reached it then either.
 * One block of slack covers the block the audio thread is inside. */
static int was_before(bwa_engine* e, uint64_t sched, uint64_t* now_out) {
    const uint64_t now = bwa_get_dsp_time_frames(e);
    if (now_out) *now_out = now;
    return now + bwa_get_block_size(e) <= sched;
}

/* Pump until the dsp clock reaches `target`. Bounded by WALL time, so a clock that stopped
 * advancing (a dead sink) reports 0 and fails the check instead of hanging the example. */
static int dsp_wait_until(bwa_engine* e, uint64_t target, uint64_t timeout_ms) {
    const uint64_t t0 = bwa_ticks_ms();
    while (bwa_get_dsp_time_frames(e) < target) {
        if (bwa_ticks_ms() - t0 > timeout_ms) return 0;
        bwa_commit(e);
        bwa_sleep_ms(4);
    }
    bwa_commit(e);
    return 1;
}

/* run `secs` of the demo loop: per-frame commit, like an engine tick */
static void run(bwa_engine* e, double secs, const char* msg) {
    if (msg) printf("%s\n", msg);
    for (int t = 0; t < ticks((int)(secs * 60.0)); ++t) { bwa_commit(e); bwa_sleep_ms(16); }
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--tests")) g_tests = 1;

    /* ---- synthesize the two variants of the same field ---- */
    printf("synthesizing a 3rd-order field (front bursts / left-up clicks / diffuse floor)...\n");
    float* ambi = (float*)malloc((size_t)FRAMES * 16 * sizeof(float));
    float* fuma = (float*)malloc((size_t)FRAMES * 16 * sizeof(float));
    if (!ambi || !fuma) { fprintf(stderr, "out of memory\n"); return 1; }
    gen_field(ambi);
    ambix_to_fuma(ambi, fuma, FRAMES);
    write_wavf("bwa_demo_ambix.wav", ambi, FRAMES, 16);
    write_wavf("bwa_demo_fuma.wav",  fuma, FRAMES, 16);
    free(ambi); free(fuma);

    /* ---- engine up (binaural monitor; silent null-sink fallback without a device) ---- */
    bwa_desc cfg = { 0 };
    cfg.profile     = BWA_PROFILE_BINAURAL;
    cfg.sample_rate = RATE;
    cfg.block_size  = 256;
    if (g_tests) cfg.sink = BWA_SINK_NULL;   /* no device, deterministic */
    bwa_engine* e = bwa_create(&cfg);
    if (!e) { fprintf(stderr, "bwa_create failed\n"); return 1; }
    if (bwa_start(e) != 0) { fprintf(stderr, "bwa_start: %s\n", bwa_last_error(e)); bwa_destroy(e); return 1; }
    const char* be = bwa_get_audio_backend(e);
    printf("backend: %s%s\n", be, strncmp(be, "null", 4) == 0 ? "  (no output device - silent run)" : "");

    bwa_sound field = bwa_load_ambix(e, "bwa_demo_ambix.wav");
    bwa_sound legacy = bwa_load_fuma(e, "bwa_demo_fuma.wav");   /* converted at load: an AmbiX asset now */
    if (!field || !legacy) { fprintf(stderr, "load: %s\n", bwa_last_error(e)); bwa_stop(e); bwa_destroy(e); return 1; }

    bwa_bed bed = bwa_bed_create(e);
    bwa_bed_set_gain(e, bed, 0.9f);
    bwa_bed_play(e, bed, field, true);

    /* ---- the walkthrough ---- */
    run(e, 5, "1) matrix decode, world-locked: bursts FRONT, clicks LEFT-UP, a diffuse floor");

    printf("2) yaw: spinning the whole field (bwa_bed_set_orientation yaw glides, click-free)\n");
    for (int t = 0; t < ticks(8 * 60); ++t) {                 /* ~one slow turn over 8 s */
        bwa_bed_set_orientation(e, bed, 0.8f * (float)t / 60.0f, 0.f, 0.f);
        bwa_commit(e); bwa_sleep_ms(16);
    }

    printf("3) tilt: pitch +80 deg (bwa_bed_set_orientation) - the front content moves to the ceiling\n");
    bwa_bed_set_orientation(e, bed, 0.0f, 1.4f, 0.0f);
    run(e, 5, NULL);
    bwa_bed_set_orientation(e, bed, 0.0f, 0.0f, 0.0f);          /* back level (also clears the yaw) */
    run(e, 3, "   ...and back level");

    for (int i = 0; i < 2; ++i) {                      /* live renderer A/B */
        bwa_set_bed_renderer(e, BWA_BED_PARAMETRIC);
        run(e, 3, "4) PARAMETRIC renderer (DirAC: direct part re-panned listener-relative, walkable)");
        bwa_set_bed_renderer(e, BWA_BED_MATRIX);
        run(e, 3, "   MATRIX renderer (the static decode)");
    }

    for (int i = 0; i < 2; ++i) {                      /* live max-rE A/B */
        bwa_set_max_re(e, true);
        run(e, 3, "5) max-rE decode weighting ON (fewer sidelobes, better off-center localization)");
        bwa_set_max_re(e, false);
        run(e, 3, "   max-rE OFF (the raw decode)");
    }

    bwa_bed_play(e, bed, legacy, true);                /* the FuMa load of the SAME field */
    run(e, 5, "6) the FuMa-loaded copy - converted at load, it should sound identical to (1)");

    /* ---- 7) the bed's PLAYBACK surface: a bed is a voice, so it has the whole of one ----
     * Every call here is the bwa_source_* call of the same name under the bed prefix, so bed code
     * never mixes prefixes. They are separate entry points rather than aliases because the
     * multichannel-asset check runs the other way round: these DEMAND a 4/9/16-ch asset where the
     * bwa_source_* forms refuse one.
     *
     * This part is CHECKED (see check() above): unlike the A/Bs, each claim has a definite answer.
     * The waits below are real Sleeps rather than run(), because what is being timed is the
     * engine's own clock and compressing it would test nothing. The sleeps only PACE the loop:
     * every verdict comes from bwa_get_dsp_time_frames, per the timing rule above the helpers. */
    printf("\n7) the bed's playback surface (scheduled start, play region, loop events, scheduled stop)\n");

    /* 7a) bwa_bed_play_at: silent until the dsp clock reaches start_sample. Same time base as
     *     bwa_source_play_at, so "now" comes from bwa_get_dsp_time_frames. A FULL SECOND out: the
     *     lead has to outlast the pacing sleeps below on the slowest machine that runs this, and
     *     0.25 s did not on a CI runner where a 16 ms sleep took 100. */
    const uint32_t block = bwa_get_block_size(e);
    uint64_t now = 0;
    bwa_bed_stop(e, bed);
    for (int t = 0; t < 12; ++t) { bwa_commit(e); bwa_sleep_ms(16); }
    const uint64_t start_at = bwa_get_dsp_time_frames(e) + RATE;      /* 1 s out */
    bwa_bed_play_at(e, bed, field, true, start_at);
    /* five pacing ticks, not nine: each one can take 100 ms on a loaded box, and every ms spent
     * here eats the lead the check needs. Five is still ~2400 frames of "an unscheduled play
     * would be well into the asset by now" at nominal speed, which is the evidence this wants. */
    for (int t = 0; t < 5; ++t) { bwa_commit(e); bwa_sleep_ms(16); }
    const uint64_t held = bwa_bed_get_playhead_frames(e, bed);
    if (!was_before(e, start_at, &now)) {
        inconclusive("bwa_bed_play_at held the field silent", start_at, now);
    } else {
        /* A play_at wired to the unscheduled call would be thousands of frames in by now, so 0 is
         * not a coin flip. The bound is generous: anything past the first block means it did not hold. */
        check(held < block, "bwa_bed_play_at held the field silent until its start sample");
    }
    if (!dsp_wait_until(e, start_at + block, 10000)) {
        check(0, "the dsp clock reached the start sample (it stalled)");
    } else {
        /* the playhead publishes per audio block, so poll for it rather than sleeping a fixed
         * count: this ends the moment it moves, and only a playhead that NEVER moves pays the bound */
        int started = 0;
        for (uint64_t w0 = bwa_ticks_ms(); !started && bwa_ticks_ms() - w0 < 2000; ) {
            bwa_commit(e);
            started = bwa_bed_get_playhead_frames(e, bed) > 0;
            if (!started) bwa_sleep_ms(4);
        }
        check(started, "...and started once the clock reached it");
    }

    /* 7b) bwa_bed_set_region + bwa_poll_looped: bound the bed to [start, end) content frames. A
     *     looping voice never ENDS, so bwa_poll_ended reports it exactly never; the wrap is the
     *     event you get, one entry per wrap, drained after the commit that fills it. Set the
     *     region AFTER the play - a play resolves the bounds against the asset and resets it. */
    bwa_bed_play(e, bed, field, true);
    bwa_bed_set_region(e, bed, 0, RATE / 5);                   /* the field's first 200 ms */
    /* Drain first, and count only after. Both rings are ENGINE-WIDE and DESTRUCTIVE, and nothing
     * above drained the loop one, so every wrap the parts above produced is still queued --
     * counted here they would let this check pass on a region that never took. Drain what you
     * did not ask for before you measure what you did. */
    { bwa_source flush[64]; while (bwa_poll_looped(e, flush, 64, NULL) == 64) { } }
    /* The window is ONE SECOND OF DSP TIME, not 63 sleeps: on a machine where the sleeps run long
     * the loop simply does fewer of them, and the expected wrap count is derived from the frames
     * that actually elapsed rather than from the count that was asked for. */
    const uint64_t region = RATE / 5;
    const uint64_t w_beg = bwa_get_dsp_time_frames(e);
    uint64_t w_end = w_beg;
    int wraps = 0;
    for (uint64_t w0 = bwa_ticks_ms(); ; ) {
        bwa_commit(e);
        bwa_source hit[16];
        uint32_t n;
        while ((n = bwa_poll_looped(e, hit, 16, NULL)) > 0) {   /* drain fully: a long tick queues several */
            for (uint32_t i = 0; i < n; ++i) if (hit[i] == bed) ++wraps;
            if (n < 16) break;
        }
        w_end = bwa_get_dsp_time_frames(e);
        if (w_end - w_beg >= RATE) break;                       /* a second of the engine's own time */
        if (bwa_ticks_ms() - w0 > 10000) break;                 /* the clock stalled: the check below fails */
        bwa_sleep_ms(16);
    }
    const uint64_t elapsed = w_end - w_beg;
    const int expect = (int)(elapsed / region);                 /* one wrap per region length */
    printf("   %d wraps of a 200 ms region in %.2f s of dsp time (about %d expected)\n",
           wraps, (double)elapsed / RATE, expect);
    /* Demanded against the frames that ACTUALLY elapsed, with the original margin: the field is 4 s
     * long, so a region that never reached the core gives exactly 0 here and a ">0" check would
     * prove nothing about the region. A stalled clock fails on the first term. */
    check(elapsed >= RATE && wraps >= expect - 2, "bwa_poll_looped reported the bed's wraps");

    /* 7c) bwa_bed_stop_at: the scheduled click-free stop, same time base again. A second out, for
     *     the reason 7a is: 0.25 s was shorter than six sleeps on a loaded runner. */
    const uint64_t stop_at = bwa_get_dsp_time_frames(e) + RATE;
    bwa_bed_stop_at(e, bed, stop_at);
    for (int t = 0; t < 4; ++t) { bwa_commit(e); bwa_sleep_ms(16); }  /* pacing only, and few: see 7a */
    const int still_playing = bwa_bed_is_playing(e, bed);
    if (!was_before(e, stop_at, &now))
        inconclusive("bwa_bed_stop_at did not stop the bed at once", stop_at, now);
    else
        check(still_playing, "bwa_bed_stop_at did not stop the bed immediately");
    if (!dsp_wait_until(e, stop_at + block, 10000)) {
        check(0, "the dsp clock reached the stop sample (it stalled)");
    } else {
        int stopped = 0;                                          /* published per block: poll, do not sleep a count */
        for (uint64_t w0 = bwa_ticks_ms(); !stopped && bwa_ticks_ms() - w0 < 2000; ) {
            bwa_commit(e);
            stopped = !bwa_bed_is_playing(e, bed);
            if (!stopped) bwa_sleep_ms(4);
        }
        check(stopped, "...and stopped it once the clock reached it");
    }

    /* 7d) bwa_bed_play_loop is the same region set at PLAY time: play [0, loop_end), then repeat
     *     [loop_beg, loop_end) - the intro-to-loop pattern, bed-typed. By ear only: it makes no
     *     claim about a scheduled sample, so there is nothing here for a slow sleep to break. */
    bwa_bed_play_loop(e, bed, field, RATE / 10, RATE / 5);     /* intro 0..200 ms, body 100..200 ms */
    run(e, 1.5, "   intro -> loop body, via bwa_bed_play_loop");

    /* ---- teardown ---- */
    bwa_bed_fade_out(e, bed, 0.5f);
    run(e, 0.8, NULL);
    bwa_bed_destroy(e, bed);
    bwa_unload_sound(e, field);
    bwa_unload_sound(e, legacy);
    bwa_stop(e);
    bwa_destroy(e);
    remove("bwa_demo_ambix.wav");
    remove("bwa_demo_fuma.wav");
    if (g_incon) printf("\nambisonic: %d check(s) INCONCLUSIVE on this run - the machine was too slow\n"
                        "  to look before the scheduled sample. Not a failure, and not a pass either.\n", g_incon);
    if (g_bad) { printf("\nambisonic: %d CHECK(S) FAILED\n", g_bad); return 1; }
    printf("done\n");
    return 0;
}
