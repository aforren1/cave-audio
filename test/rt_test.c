/*
 * rt_test.c — M2/M4 verification of the concurrency spine, driven off the RT path
 * (single-threaded, deterministic). Routing now goes through real DBAP (M4), so the
 * observable is "a source at speaker k's surveyed position makes channel k dominate".
 * Checks: the commit snapshot, generation stale-drop, play/stop, gain scaling, and the
 * rt_create event-ring bound. (DBAP/align properties live in dsp_test.c.)
 */
#include "rt.h"
#include "layout.h"
#include "dr_wav.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N    256
#define CH   BW_CHANNELS
#define RATE 48000u

static float  bus[CH * N];
static Layout LD;                       /* default layout: speaker positions for the test */

static double chan_energy(int ch) {
    double e = 0; for (int i = 0; i < N; ++i) e += fabs(bus[(size_t)ch * N + i]); return e;
}
static double total_energy(void) {
    double e = 0; for (int i = 0; i < CH * N; ++i) e += fabs(bus[i]); return e;
}
static int argmax_channel(void) {
    int best = 0; double bm = -1;
    for (int ch = 0; ch < CH; ++ch) { double e = chan_energy(ch); if (e > bm) { bm = e; best = ch; } }
    return best;
}
static void render2(RtCore* c) { BwTimestamp ts = { 0, 0 }; rt_render(c, bus, N, &ts); rt_render(c, bus, N, &ts); }

/* place a voice at speaker k's surveyed position (so DBAP localizes it to channel k) */
static void set_pos_spk(RtCore* c, uint32_t h, int k) {
    rt_source_set_pos(c, h, LD.speakers[k].pos[0], LD.speakers[k].pos[1], LD.speakers[k].pos[2]);
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
    LD = layout_default();                          /* listener stays at the origin (centre) */
    const char* WAV = "bw_rt_const.wav";
    if (!write_const_wav(WAV, 1.0f, 8 * N)) { printf("FAIL: write wav\n"); return 1; }

    CHECK(rt_create(1000, 1000, RATE, CH) == NULL, "rt_create rejects caps that could overflow the event ring");

    RtCore* c = rt_create(64, 8, RATE, CH);
    CHECK(c != NULL, "rt_create");
    if (!c) { remove(WAV); return 1; }
    char err[256] = {0};
    uint32_t snd = rt_load_sound(c, WAV, err, sizeof err);
    CHECK(snd != 0, err[0] ? err : "load wav");

    /* 1. a source at speaker 7's position localizes to channel 7 */
    uint32_t h = rt_source_create(c);
    CHECK(h != 0, "source_create returns a valid handle");
    rt_source_play(c, h, snd, true);
    set_pos_spk(c, h, 7);
    rt_commit(c);
    render2(c);
    CHECK(argmax_channel() == 7, "DBAP localizes a source at speaker 7 to channel 7");

    /* 2. commit snapshot: an uncommitted move does NOT relocate the voice */
    set_pos_spk(c, h, 13);
    render2(c);
    CHECK(argmax_channel() == 7, "uncommitted move: still localized to 7");
    rt_commit(c);
    render2(c);
    CHECK(argmax_channel() == 13, "committed move: now localized to 13");

    /* 3. set_gain scales total power (constant-power DBAP, constant 1.0 source) */
    rt_source_set_gain(c, h, 1.0f); rt_commit(c); render2(c);
    double e_full = total_energy();
    rt_source_set_gain(c, h, 0.5f); rt_commit(c); render2(c);
    double e_half = total_energy();
    CHECK(e_full > 0.0 && fabs(e_half / e_full - 0.5) < 0.02, "set_gain(0.5) ~ half the total energy");

    /* 4. stop silences */
    rt_source_stop(c, h);
    render2(c);
    CHECK(total_energy() == 0.0, "stopped voice is silent");

    /* 5. generation handles: destroy + recreate reuses the slot; a stale set is dropped */
    uint32_t old = h;
    rt_source_destroy(c, old);
    render2(c);
    CHECK(total_energy() == 0.0, "destroyed voice is silent");
    uint32_t h2 = rt_source_create(c);
    CHECK(BW_H_IDX(h2) == BW_H_IDX(old), "destroyed slot is reused");
    CHECK(BW_H_GEN(h2) != BW_H_GEN(old), "generation is bumped on reuse");
    set_pos_spk(c, old, 2);                          /* STALE handle -> must be dropped */
    rt_source_play(c, h2, snd, true);
    set_pos_spk(c, h2, 9);                           /* valid */
    rt_commit(c);
    render2(c);
    CHECK(argmax_channel() == 9, "new voice localizes to 9 (valid set applied)");

    /* 6. double-destroy is idempotent (free-list not corrupted) */
    rt_source_destroy(c, h2);
    rt_source_destroy(c, h2);
    render2(c);
    uint32_t h3 = rt_source_create(c);
    CHECK(h3 != 0, "create after double-destroy still works");
    rt_source_play(c, h3, snd, true);
    set_pos_spk(c, h3, 4);
    rt_commit(c); render2(c);
    CHECK(argmax_channel() == 4, "voice after double-destroy localizes correctly");

    /* 7. non-finite inputs rejected at the boundary (no NaN reaches the audio thread) */
    rt_source_set_pos(c, h3, NAN, 0, 0);
    rt_source_set_gain(c, h3, INFINITY);
    rt_commit(c); render2(c);
    double e4 = total_energy();
    CHECK(e4 > 0.0 && isfinite(e4) && argmax_channel() == 4, "voice unmoved and bus finite after NaN/Inf inputs");

    /* 8. occlusion attenuates the mono signal pre-pan (ramped), fed via rt_set_occlusion */
    double e_clear = e4;                              /* h3 un-occluded, at channel 4 */
    rt_set_occlusion(c, h3, 0.5f); render2(c);
    CHECK(fabs(total_energy() / e_clear - 0.5) < 0.05, "occlusion 0.5 ~ half energy");
    rt_set_occlusion(c, h3, 0.0f); render2(c);
    CHECK(total_energy() < e_clear * 0.02, "occlusion 0 ~ silent");
    rt_set_occlusion(c, h3, 1.0f); render2(c);
    CHECK(fabs(total_energy() / e_clear - 1.0) < 0.02, "occlusion restored to 1 ~ full");
    uint32_t stale_occ = BW_MK_H(BW_H_IDX(h3), (uint16_t)(BW_H_GEN(h3) + 7));
    rt_set_occlusion(c, stale_occ, 0.0f); render2(c);
    CHECK(total_energy() > e_clear * 0.5, "occlusion on a stale handle is dropped");

    /* 9. slot recycling clears occlusion: a publish for the prior occupant never attenuates the
     *    voice that reuses its slot (the audio thread gates the publish on its own generation). */
    rt_set_occlusion(c, h3, 0.0f); render2(c);
    CHECK(total_energy() < e_clear * 0.02, "h3 fully occluded before recycling");
    rt_source_destroy(c, h3); render2(c);
    uint32_t h4 = rt_source_create(c);
    CHECK(BW_H_IDX(h4) == BW_H_IDX(h3) && h4 != h3, "occlusion: slot reused with a bumped generation");
    rt_source_play(c, h4, snd, true); set_pos_spk(c, h4, 4); rt_commit(c); render2(c);
    CHECK(total_energy() > e_clear * 0.5, "recycled voice is clear despite the prior occupant's occlusion");

    rt_destroy(c);
    remove(WAV);
    if (fails) { printf("rt_test: %d FAILURES\n", fails); return 1; }
    printf("rt_test OK (DBAP routing, commit snapshot, generation drop, gain, occlusion verified)\n");
    return 0;
}
