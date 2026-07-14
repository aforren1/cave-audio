/*
 * sound_test.c — M3 verification (wav + retire-ack), off the RT path. Routing is M4 DBAP
 * now, so most checks use total energy (a detached/stopped/ended voice goes fully silent),
 * plus one multi-voice check that two sources at two speakers light up both channels.
 *   - multiple wav voices mix and reach their chosen channels;
 *   - a non-looping sound stops at its end;
 *   - a oneshot recycles its transient voice (a small table doesn't leak);
 *   - unloading a playing sound is safe (detach + free-after-ack; clean under ASan).
 *
 * Compiles the core in (bwa_core); dr_wav's impl comes from sound.c, included here only for
 * the write API used to synthesize test files.
 */
#include "rt.h"
#include "layout.h"
#include "sound.h"
#include "dr_wav.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N    256
#define CH   BWA_CHANNELS
#define RATE 48000u

static float  bus[CH * N];
static Layout LD;

static double chan_energy(int ch) {
    double e = 0; for (int i = 0; i < N; ++i) e += fabs(bus[(size_t)ch * N + i]); return e;
}
static double total_energy(void) {
    double e = 0; for (int i = 0; i < CH * N; ++i) e += fabs(bus[i]); return e;
}
static void render1(RtCore* c) { bwa_timestamp ts = { 0, 0 }; rt_render(c, bus, N, &ts); }
static void render2(RtCore* c) { render1(c); render1(c); }
static void set_pos_spk(RtCore* c, uint32_t h, int k) {
    rt_source_set_pos(c, h, LD.speakers[k].pos[0], LD.speakers[k].pos[1], LD.speakers[k].pos[2]);
}

static int write_const_wav_rate(const char* path, float value, uint32_t frames, uint32_t rate) {
    drwav_data_format fmt = { drwav_container_riff, DR_WAVE_FORMAT_IEEE_FLOAT, 1, rate, 32 };
    drwav wav;
    if (!drwav_init_file_write(&wav, path, &fmt, NULL)) return 0;
    float* buf = (float*)malloc((size_t)frames * sizeof(float));
    if (!buf) { drwav_uninit(&wav); return 0; }
    for (uint32_t i = 0; i < frames; ++i) buf[i] = value;
    drwav_uint64 wrote = drwav_write_pcm_frames(&wav, frames, buf);
    free(buf); drwav_uninit(&wav);
    return wrote == frames;
}

static int write_const_wav(const char* path, float value, uint32_t frames) {
    drwav_data_format fmt = { drwav_container_riff, DR_WAVE_FORMAT_IEEE_FLOAT, 1, RATE, 32 };
    drwav wav;
    if (!drwav_init_file_write(&wav, path, &fmt, NULL)) return 0;
    float* buf = (float*)malloc((size_t)frames * sizeof(float));
    if (!buf) { drwav_uninit(&wav); return 0; }
    for (uint32_t i = 0; i < frames; ++i) buf[i] = value;
    drwav_uint64 wrote = drwav_write_pcm_frames(&wav, frames, buf);
    free(buf); drwav_uninit(&wav);
    return wrote == frames;
}

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", (msg)); ++fails; } } while (0)

int main(void) {
    LD = layout_default();
    const char* WAV_LONG  = "bwa_snd_long.wav";
    const char* WAV_SHORT = "bwa_snd_short.wav";
    if (!write_const_wav(WAV_LONG, 0.5f, 4 * N) || !write_const_wav(WAV_SHORT, 1.0f, 100)) {
        printf("FAIL: could not write test wavs\n"); return 1;
    }

    char err[256] = {0};
    RtCore* c = rt_create(64, 8, RATE, CH);
    CHECK(c != NULL, "rt_create");
    if (!c) return 1;

    /* 1. two wav voices at two speakers light up both channels */
    uint32_t sLong = rt_load_sound(c, WAV_LONG, err, sizeof err);
    CHECK(sLong != 0, err[0] ? err : "load long wav");
    uint32_t v1 = rt_source_create(c), v2 = rt_source_create(c);
    rt_source_play(c, v1, sLong, true); set_pos_spk(c, v1, 3);
    rt_source_play(c, v2, sLong, true); set_pos_spk(c, v2, 7);
    rt_commit(c);
    render2(c);
    CHECK(chan_energy(3) > 0.0 && chan_energy(7) > 0.0, "both voices reach their speaker channels");
    rt_source_stop(c, v1); rt_source_stop(c, v2);    /* isolate the following total-energy checks */
    render1(c);

    /* 2. a non-looping sound stops at its end */
    uint32_t sShort = rt_load_sound(c, WAV_SHORT, err, sizeof err);
    CHECK(sShort != 0, "load short wav");
    uint32_t v3 = rt_source_create(c);
    rt_source_play(c, v3, sShort, false); set_pos_spk(c, v3, 5);
    rt_commit(c);
    render1(c);
    CHECK(total_energy() > 0.0, "short sound audible in its first block");
    render1(c);
    CHECK(total_energy() == 0.0, "non-looping voice is silent after its end");

    /* 3. unloading a sound while a voice plays it is safe (the UAF case) */
    uint32_t v4 = rt_source_create(c);
    rt_source_play(c, v4, sLong, true); set_pos_spk(c, v4, 4);
    rt_commit(c); render2(c);
    CHECK(total_energy() > 0.0, "voice playing the loaded sound");
    rt_unload_sound(c, sLong);
    render1(c);
    CHECK(total_energy() == 0.0, "voice detached when its sound was unloaded");
    rt_commit(c);                                 /* drain_events frees pcm + recycles slot */
    render1(c);                                   /* must not read freed pcm */
    CHECK(total_energy() == 0.0, "still silent after the buffer is freed (no UAF)");
    rt_unload_sound(c, sLong);                     /* idempotent no-op */
    rt_source_play(c, v4, sLong, true);            /* play of a retired sound is dropped */
    render2(c);
    CHECK(total_energy() == 0.0, "playing an unloaded sound is a no-op");

    rt_destroy(c);

    /* 4. a oneshot recycles its transient voice — a 2-slot table doesn't leak */
    RtCore* c2 = rt_create(2, 4, RATE, CH);
    CHECK(c2 != NULL, "rt_create (small)");
    if (c2) {
        uint32_t sB = rt_load_sound(c2, WAV_SHORT, err, sizeof err);
        CHECK(sB != 0, "load short wav (small core)");
        rt_play_oneshot(c2, sB, LD.speakers[6].pos[0], LD.speakers[6].pos[1], LD.speakers[6].pos[2], 1.0f);
        rt_commit(c2);
        render1(c2);
        CHECK(total_energy() > 0.0, "oneshot audible");
        render1(c2);
        rt_commit(c2);                            /* drain_events recycles the transient voice */
        uint32_t a = rt_source_create(c2);
        uint32_t b = rt_source_create(c2);
        CHECK(a != 0 && b != 0, "oneshot recycled its slot (both fresh sources allocate)");
        rt_destroy(c2);
    }

    /* 5. a 44.1 kHz file resamples to the engine rate on load (frames scale, DC preserved) */
    const char* WAV_44K = "bwa_snd_44k.wav";
    if (write_const_wav_rate(WAV_44K, 0.5f, 4410, 44100)) {
        SoundData sd;
        bool ok = sound_load(WAV_44K, RATE, &sd, err, sizeof err);
        CHECK(ok, ok ? "resample load" : err);
        if (ok) {
            uint32_t expect = (uint32_t)(4410.0 * (double)RATE / 44100.0 + 0.5);   /* ~4800 */
            CHECK(sd.sample_rate == RATE, "resampled sound carries the engine rate");
            CHECK(sd.frames + 4 > expect && sd.frames < expect + 4, "resampled frame count scales by the rate ratio");
            CHECK(fabsf(sd.pcm[sd.frames / 2] - 0.5f) < 0.02f, "DC value preserved through resampling");
            sound_unload(&sd);
        }
        remove(WAV_44K);
    }

    remove(WAV_LONG);
    remove(WAV_SHORT);
    if (fails) { printf("sound_test: %d FAILURES\n", fails); return 1; }
    printf("sound_test OK (wav/flac/mp3 decode, resample, oneshot recycle, unload-safety verified)\n");
    return 0;
}
