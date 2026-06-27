/*
 * sound_test.c — M3 verification, off the RT path (single-threaded, deterministic).
 * Generates wav files with dr_wav, loads them through the rt core, and checks:
 *   - multiple wav voices mix to their chosen channels at the expected amplitude;
 *   - a non-looping sound stops at its end;
 *   - a oneshot recycles its transient voice (a small table doesn't leak);
 *   - unloading a playing sound is safe: the audio thread detaches the voice and the
 *     buffer is freed only after the retire-ack (no use-after-free — clean under ASan).
 *
 * Compiles rt.c + sound.c directly; dr_wav's implementation comes from sound.c, so this
 * file includes dr_wav.h only for the write API used to synthesize the test files.
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

static double chan_energy(int ch) {
    double e = 0; for (int i = 0; i < N; ++i) e += fabs(bus[(size_t)ch * N + i]); return e;
}
static double chan_peak(int ch) {
    double m = 0; for (int i = 0; i < N; ++i) { double a = fabs(bus[(size_t)ch * N + i]); if (a > m) m = a; } return m;
}
static void render1(RtCore* c) { BwTimestamp ts = { 0, 0 }; rt_render(c, bus, N, &ts); }
static void render2(RtCore* c) { render1(c); render1(c); }

/* write `frames` mono samples (all = value) as a 32-bit float wav */
static bool write_const_wav(const char* path, float value, uint32_t frames) {
    drwav_data_format fmt;
    fmt.container     = drwav_container_riff;
    fmt.format        = DR_WAVE_FORMAT_IEEE_FLOAT;
    fmt.channels      = 1;
    fmt.sampleRate    = RATE;
    fmt.bitsPerSample = 32;
    drwav wav;
    if (!drwav_init_file_write(&wav, path, &fmt, NULL)) return false;
    float* buf = (float*)malloc((size_t)frames * sizeof(float));
    if (!buf) { drwav_uninit(&wav); return false; }
    for (uint32_t i = 0; i < frames; ++i) buf[i] = value;
    drwav_uint64 wrote = drwav_write_pcm_frames(&wav, frames, buf);
    free(buf);
    drwav_uninit(&wav);
    return wrote == frames;
}

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", (msg)); ++fails; } } while (0)

int main(void) {
    const char* WAV_LONG  = "bw_snd_long.wav";    /* constant 0.5, several blocks */
    const char* WAV_SHORT = "bw_snd_short.wav";   /* constant 1.0, ends inside one block */
    if (!write_const_wav(WAV_LONG, 0.5f, 4 * N) || !write_const_wav(WAV_SHORT, 1.0f, 100)) {
        printf("FAIL: could not write test wavs\n"); return 1;
    }

    char err[256] = {0};
    RtCore* c = rt_create(64, 8, RATE, CH);
    CHECK(c != NULL, "rt_create");
    if (!c) return 1;

    /* 1. multiple wav voices mix to chosen channels at the expected amplitude */
    uint32_t sLong = rt_load_sound(c, WAV_LONG, err, sizeof err);
    CHECK(sLong != 0, err[0] ? err : "load long wav");
    uint32_t v1 = rt_source_create(c), v2 = rt_source_create(c);
    rt_source_play(c, v1, sLong, true); rt_source_set_pos(c, v1, 3, 0, 0);
    rt_source_play(c, v2, sLong, true); rt_source_set_pos(c, v2, 7, 0, 0);
    rt_commit(c);
    render2(c);                                   /* settle the gain ramp */
    CHECK(fabs(chan_peak(3) - 0.5) < 0.01, "voice 1 plays the 0.5 sample on ch3");
    CHECK(fabs(chan_peak(7) - 0.5) < 0.01, "voice 2 plays the 0.5 sample on ch7");
    CHECK(chan_energy(0) == 0.0, "unused channel silent");

    /* 2. a non-looping sound stops at its end */
    uint32_t sShort = rt_load_sound(c, WAV_SHORT, err, sizeof err);
    CHECK(sShort != 0, "load short wav");
    uint32_t v3 = rt_source_create(c);
    rt_source_play(c, v3, sShort, false); rt_source_set_pos(c, v3, 5, 0, 0);
    rt_commit(c);
    render1(c);
    CHECK(chan_energy(5) > 0.0, "short sound audible in its first block");
    render1(c);
    CHECK(chan_energy(5) == 0.0, "non-looping voice is silent after its end");

    /* 3. unloading a sound while a voice plays it is safe (the UAF case) */
    uint32_t v4 = rt_source_create(c);
    rt_source_play(c, v4, sLong, true); rt_source_set_pos(c, v4, 4, 0, 0);
    rt_commit(c); render2(c);
    CHECK(chan_energy(4) > 0.0, "voice playing the loaded sound");
    rt_unload_sound(c, sLong);                    /* retiring + CMD_SOUND_RETIRE */
    render1(c);                                   /* audio detaches the voice, acks */
    CHECK(chan_energy(4) == 0.0, "voice detached when its sound was unloaded");
    rt_commit(c);                                 /* drain_events frees pcm + recycles slot */
    render1(c);                                   /* must not read freed pcm */
    CHECK(chan_energy(4) == 0.0, "still silent after the buffer is freed (no UAF)");
    rt_unload_sound(c, sLong);                     /* idempotent no-op */
    rt_source_play(c, v4, sLong, true);            /* play of a retired sound is dropped */
    render2(c);
    CHECK(chan_energy(4) == 0.0, "playing an unloaded sound is a no-op");

    rt_destroy(c);

    /* 4. a oneshot recycles its transient voice — a 2-slot table doesn't leak */
    RtCore* c2 = rt_create(2, 4, RATE, CH);
    CHECK(c2 != NULL, "rt_create (small)");
    if (c2) {
        uint32_t sB = rt_load_sound(c2, WAV_SHORT, err, sizeof err);
        CHECK(sB != 0, "load short wav (small core)");
        rt_play_oneshot(c2, sB, 6, 0, 0, 1.0f);
        rt_commit(c2);
        render1(c2);                              /* plays 100 frames then ends -> EVT_VOICE_ENDED */
        CHECK(chan_energy(6) > 0.0, "oneshot audible");
        render1(c2);
        rt_commit(c2);                            /* drain_events recycles the transient voice */
        uint32_t a = rt_source_create(c2);
        uint32_t b = rt_source_create(c2);
        CHECK(a != 0 && b != 0, "oneshot recycled its slot (both fresh sources allocate)");
        rt_destroy(c2);
    }

    remove(WAV_LONG);
    remove(WAV_SHORT);
    if (fails) { printf("sound_test: %d FAILURES\n", fails); return 1; }
    printf("sound_test OK (wav mix, natural end, oneshot recycle, unload-safety verified)\n");
    return 0;
}
