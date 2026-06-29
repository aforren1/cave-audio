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

/* write a 4-channel (1st-order AmbiX) wav with constant W/Y/Z/X per frame (ACN order, SN3D) */
static int write_ambix4_wav(const char* path, float w, float y, float z, float x, uint32_t frames) {
    drwav_data_format fmt = { drwav_container_riff, DR_WAVE_FORMAT_IEEE_FLOAT, 4, RATE, 32 };
    drwav wav;
    if (!drwav_init_file_write(&wav, path, &fmt, NULL)) return 0;
    float* buf = (float*)malloc((size_t)frames * 4 * sizeof(float));
    if (!buf) { drwav_uninit(&wav); return 0; }
    for (uint32_t i = 0; i < frames; ++i) { buf[i*4+0]=w; buf[i*4+1]=y; buf[i*4+2]=z; buf[i*4+3]=x; }
    drwav_uint64 wrote = drwav_write_pcm_frames(&wav, frames, buf);
    free(buf); drwav_uninit(&wav);
    return wrote == frames;
}

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", (msg)); ++fails; } } while (0)

/* stub bus tap: counts calls, measures the aux-send energy, and writes a marker onto bus channel 0
 * (to prove the tap can sum onto the bus, like the reflection bed would). */
static int      g_tap_calls;
static uint32_t g_tap_n;
static double   g_aux_energy;
static void test_tap(void* ud, float* bus, uint32_t n, const float* lp, const float* lq, const float* aux) {
    (void)ud; (void)lp; (void)lq;
    ++g_tap_calls; g_tap_n = n;
    double e = 0; for (uint32_t i = 0; i < n; ++i) e += fabs(aux[i]);
    g_aux_energy = e;
    for (uint32_t i = 0; i < n; ++i) bus[0 * (size_t)n + i] += 0.125f;
}

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

    /* 10. per-band transmission EQ: a low-band cut darkens the (DC) test signal; flat restores it.
     *     The const wav is DC, which sits in the low-shelf band, so band[0] sets its level. */
    double e4b = total_energy();                          /* h4 clear at channel 4 */
    const float lo_cut[3] = { 0.0625f, 1.f, 1.f };        /* kill the low band, keep mid/high */
    rt_set_occlusion_eq(c, h4, 1.0f, lo_cut);
    for (int k = 0; k < 12; ++k) render2(c);             /* let the band-gain glide settle */
    CHECK(total_energy() < e4b * 0.1, "low-band EQ cut darkens the DC signal (level held at 1.0)");
    const float flat[3] = { 1.f, 1.f, 1.f };
    rt_set_occlusion_eq(c, h4, 1.0f, flat);
    for (int k = 0; k < 12; ++k) render2(c);
    CHECK(fabs(total_energy() / e4b - 1.0) < 0.02, "flat EQ restores full level (bypass re-engaged)");

    /* 11. directivity rides its own pre-pan gain ramp (the dir term of rt_set_direct). */
    rt_set_direct(c, h4, 1.0f, flat, 0.5f);
    for (int k = 0; k < 4; ++k) render2(c);
    CHECK(fabs(total_energy() / e4b - 0.5) < 0.05, "directivity 0.5 ~ half (same linear scale as occlusion)");
    rt_set_direct(c, h4, 1.0f, flat, 1.0f);
    for (int k = 0; k < 4; ++k) render2(c);
    CHECK(fabs(total_energy() / e4b - 1.0) < 0.02, "directivity restored to 1 ~ full energy");

    rt_destroy(c);

    /* 12. ambisonic bed: a W-only field decodes equally to all speakers; a front-encoded 1st-order
     *     field favors the front speaker (room -z) over the back one (+z). */
    const char* AMB_OMNI = "bw_amb_omni.wav", *AMB_FRONT = "bw_amb_front.wav";
    if (write_ambix4_wav(AMB_OMNI, 0.5f, 0.f, 0.f, 0.f, 4 * N) &&
        write_ambix4_wav(AMB_FRONT, 0.5f, 0.f, 0.f, 0.5f, 4 * N)) {     /* W,Y,Z,X — X(ACN3) = front */
        RtCore* cb = rt_create(8, 4, RATE, CH);
        CHECK(cb != NULL, "rt_create (bed)");
        if (cb) {
            uint32_t so = rt_load_ambix(cb, AMB_OMNI, err, sizeof err);
            CHECK(so != 0, err[0] ? err : "load ambix omni");
            uint32_t b1 = rt_source_create(cb);
            rt_source_play(cb, b1, so, true);
            rt_commit(cb); render2(cb);
            CHECK(chan_energy(0) > 0.0 && fabs(chan_energy(25) / chan_energy(0) - 1.0) < 0.02,
                  "omni (W-only) bed decodes equally across speakers");
            rt_source_stop(cb, b1); render2(cb);

            int s_front = -1, s_back = -1;
            for (int s = 0; s < CH; ++s)
                if (fabsf(LD.speakers[s].pos[0]) < 0.1f && fabsf(LD.speakers[s].pos[1]) < 0.1f) {
                    if (LD.speakers[s].pos[2] < -1.0f) s_front = s;
                    if (LD.speakers[s].pos[2] >  1.0f) s_back  = s;
                }
            uint32_t sf = rt_load_ambix(cb, AMB_FRONT, err, sizeof err);
            CHECK(sf != 0, "load ambix front");
            uint32_t b2 = rt_source_create(cb);
            rt_source_play(cb, b2, sf, true);
            rt_commit(cb); render2(cb);
            CHECK(s_front >= 0 && s_back >= 0 && chan_energy(s_front) > chan_energy(s_back) * 1.5,
                  "front-encoded bed favors the front speaker");
            rt_destroy(cb);
        }
        remove(AMB_OMNI); remove(AMB_FRONT);
    } else {
        CHECK(0, "could not write ambix test wavs");
    }

    /* 13. bus tap + reflection aux send: the tap is called once per block with the summed mono send
     *     of opted-in voices, and it can sum onto the bus; opting out removes the voice. */
    RtCore* ct = rt_create(8, 4, RATE, CH);
    CHECK(ct != NULL, "rt_create (tap)");
    if (ct) {
        uint32_t st = rt_load_sound(ct, WAV, err, sizeof err);
        uint32_t vt = rt_source_create(ct);
        rt_source_play(ct, vt, st, true);
        rt_source_set_pos(ct, vt, LD.speakers[4].pos[0], LD.speakers[4].pos[1], LD.speakers[4].pos[2]);
        rt_source_set_reflections(ct, vt, true);
        rt_set_bus_tap(ct, test_tap, NULL);
        g_tap_calls = 0; g_aux_energy = 0; g_tap_n = 0;
        rt_commit(ct); render2(ct);
        CHECK(g_tap_calls == 2 && g_tap_n == (uint32_t)N, "bus tap called once per block with the block size");
        CHECK(g_aux_energy > 0.0, "aux send carries the opted-in voice's signal");
        CHECK(chan_energy(0) > 0.0, "tap can sum onto the bus");
        rt_source_set_reflections(ct, vt, false);    /* opt out -> aux is silent */
        g_aux_energy = -1.0; rt_commit(ct); render2(ct);
        CHECK(g_aux_energy == 0.0, "opting out removes the voice from the aux send");
        rt_destroy(ct);
    }

    /* channel test signal: drives a raw output channel (after align), only that channel */
    {
        RtCore* cs = rt_create(8, 4, RATE, CH);
        CHECK(cs != NULL, "rt_create (test signal)");
        if (cs) {
            BwTimestamp ts = { 0, 0 };
            rt_test_signal(cs, 5, 1, 0.5f);      /* sine on channel 5 (drained + injected in one render) */
            rt_render(cs, bus, N, &ts);
            double e5 = chan_energy(5);
            CHECK(e5 > 0.1 && (total_energy() - e5) < 1e-6, "test signal drives only its channel");
            rt_test_signal(cs, 5, 0, 0.0f);      /* off */
            rt_render(cs, bus, N, &ts);
            CHECK(chan_energy(5) < 1e-6, "test signal off -> channel silent");
            rt_destroy(cs);
        }
    }

    remove(WAV);
    if (fails) { printf("rt_test: %d FAILURES\n", fails); return 1; }
    printf("rt_test OK (DBAP, commit, gen-drop, gain, occlusion, EQ, directivity, bed, reflection-tap, channel-test verified)\n");
    return 0;
}
