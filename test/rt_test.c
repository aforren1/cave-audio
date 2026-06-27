/*
 * rt_test.c — M2 verification of the concurrency spine, driven off the RT path
 * (single-threaded, deterministic). It enqueues control commands and steps the consumer
 * (rt_render) directly, asserting behaviour through the mixed bus:
 *   - a played voice routes a tone to its position-derived channel;
 *   - the commit snapshot: an uncommitted set_pos does NOT move the voice;
 *   - generation handles: a stale handle's command is dropped after slot reuse;
 *   - stop silences; set_gain scales amplitude.
 * (The two-thread, ThreadSanitizer/Helgrind run needs a Clang/Linux build — see build.md.)
 *
 * Compiles rt.c directly so it can drive the core without a device or audio thread.
 */
#include "rt.h"
#include "dr_wav.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N    256
#define CH   BW_CHANNELS
#define RATE 48000u

static float bus[CH * N];

/* Write `frames` mono samples (all = value) as a 32-bit float wav (dr_wav impl from sound.c). */
static int write_const_wav(const char* path, float value, uint32_t frames) {
    drwav_data_format fmt = { drwav_container_riff, DR_WAVE_FORMAT_IEEE_FLOAT, 1, RATE, 32 };
    drwav wav;
    if (!drwav_init_file_write(&wav, path, &fmt, NULL)) return 0;
    float* buf = (float*)malloc((size_t)frames * sizeof(float));
    if (!buf) { drwav_uninit(&wav); return 0; }
    for (uint32_t i = 0; i < frames; ++i) buf[i] = value;
    drwav_uint64 wrote = drwav_write_pcm_frames(&wav, frames, buf);
    free(buf);
    drwav_uninit(&wav);
    return wrote == frames;
}

static double chan_energy(int ch) {
    double e = 0; for (int i = 0; i < N; ++i) e += fabs(bus[(size_t)ch * N + i]); return e;
}
static double total_energy(void) {
    double e = 0; for (int i = 0; i < CH * N; ++i) e += fabs(bus[i]); return e;
}
/* peak amplitude is phase-window-independent (the tone reaches its max within a block),
 * so it gives a clean amplitude ratio for the gain test. */
static double total_peak(void) {
    double m = 0; for (int i = 0; i < CH * N; ++i) { double a = fabs(bus[i]); if (a > m) m = a; } return m;
}
/* render two blocks: block 1 drains commands + ramps gains, block 2 settles to steady state */
static void render2(RtCore* c) {
    BwTimestamp ts = { 0, 0 };
    rt_render(c, bus, N, &ts);
    rt_render(c, bus, N, &ts);
}

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", (msg)); ++fails; } } while (0)

int main(void) {
    const char* WAV = "bw_rt_const.wav";           /* constant 1.0 so peak == gain exactly */
    if (!write_const_wav(WAV, 1.0f, 8 * N)) { printf("FAIL: write wav\n"); return 1; }
    CHECK(rt_create(1000, 1000, RATE, CH) == NULL, "rt_create rejects caps that could overflow the event ring");
    RtCore* c = rt_create(64, 8, RATE, CH);        /* voice_cap, sound_cap, rate, channels */
    CHECK(c != NULL, "rt_create");
    if (!c) { remove(WAV); return 1; }
    char err[256] = {0};
    uint32_t snd = rt_load_sound(c, WAV, err, sizeof err);   /* a real sound for the voices to play */
    CHECK(snd != 0, err[0] ? err : "load wav");

    /* 1. create + play + set_pos(ch 3) + commit -> tone on channel 3 only */
    uint32_t h = rt_source_create(c);
    CHECK(h != 0, "source_create returns a valid handle");
    rt_source_play(c, h, snd, true);
    rt_source_set_pos(c, h, 3.0f, 0, 0);
    rt_commit(c);
    render2(c);
    CHECK(chan_energy(3) > 0.0, "tone present on channel 3");
    CHECK(chan_energy(0) == 0.0 && chan_energy(8) == 0.0, "non-target channels silent");

    /* 2. commit snapshot: set_pos(ch 8) WITHOUT commit -> voice stays on channel 3 */
    rt_source_set_pos(c, h, 8.0f, 0, 0);
    render2(c);
    CHECK(chan_energy(3) > 0.0, "uncommitted move: still on ch3");
    CHECK(chan_energy(8) == 0.0, "uncommitted move: ch8 silent");
    rt_commit(c);                                   /* now promote the snapshot */
    render2(c);
    CHECK(chan_energy(8) > 0.0, "committed move: now on ch8");
    CHECK(chan_energy(3) == 0.0, "committed move: ch3 silent");

    /* 3. set_gain scales amplitude (compare steady-state peak amplitude) */
    rt_source_set_gain(c, h, 1.0f); rt_commit(c); render2(c);
    double p_full = total_peak();
    rt_source_set_gain(c, h, 0.5f); rt_commit(c); render2(c);
    double p_half = total_peak();
    CHECK(p_full > 0.0 && p_half > 0.0 && fabs(p_half / p_full - 0.5) < 0.02,
          "set_gain(0.5) yields ~half the amplitude of gain 1.0");

    /* 4. stop silences (structural; no commit needed) */
    rt_source_stop(c, h);
    render2(c);
    CHECK(total_energy() == 0.0, "stopped voice is silent");

    /* 5. generation handles: destroy + recreate reuses the slot with a bumped gen,
     *    and a stale handle's command is dropped. */
    uint32_t old = h;
    rt_source_destroy(c, old);
    render2(c);
    CHECK(total_energy() == 0.0, "destroyed voice is silent");

    uint32_t h2 = rt_source_create(c);
    CHECK(BW_H_IDX(h2) == BW_H_IDX(old), "destroyed slot is reused");
    CHECK(BW_H_GEN(h2) != BW_H_GEN(old), "generation is bumped on reuse");

    rt_source_set_pos(c, old, 4.0f, 0, 0);          /* STALE handle -> must be dropped */
    rt_source_play(c, h2, snd, true);
    rt_source_set_pos(c, h2, 9.0f, 0, 0);           /* valid */
    rt_commit(c);
    render2(c);
    CHECK(chan_energy(9) > 0.0, "new voice routes to ch9 (valid set applied)");
    CHECK(chan_energy(4) == 0.0, "stale handle's set_pos was dropped");

    /* 6. double-destroy is idempotent (must not corrupt the free-list) */
    rt_source_destroy(c, h2);
    rt_source_destroy(c, h2);                       /* second call is a harmless no-op */
    render2(c);
    uint32_t h3 = rt_source_create(c);              /* slot must still be allocatable */
    CHECK(h3 != 0, "create after double-destroy still works");
    rt_source_play(c, h3, snd, true);
    rt_source_set_pos(c, h3, 6.0f, 0, 0);
    rt_commit(c); render2(c);
    CHECK(chan_energy(6) > 0.0, "voice after double-destroy routes correctly");

    /* 7. non-finite inputs are rejected at the boundary (no UB / NaN on the audio thread) */
    rt_source_set_pos(c, h3, NAN, 0, 0);            /* must be ignored */
    rt_source_set_gain(c, h3, INFINITY);           /* must be ignored */
    rt_commit(c); render2(c);
    double e6 = chan_energy(6);
    CHECK(e6 > 0.0 && isfinite(e6), "voice unmoved and bus finite after NaN/Inf inputs");
    CHECK(chan_energy(0) == 0.0, "no spurious routing from bad inputs");

    rt_destroy(c);
    remove(WAV);
    if (fails) { printf("rt_test: %d FAILURES\n", fails); return 1; }
    printf("rt_test OK (commit snapshot, generation drop, routing, gain all verified)\n");
    return 0;
}
