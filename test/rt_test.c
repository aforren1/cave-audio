/*
 * rt_test.c — M2/M4 verification of the concurrency spine, driven off the RT path
 * (single-threaded, deterministic). Routing now goes through real DBAP (M4), so the
 * observable is "a source at speaker k's surveyed position makes channel k dominate".
 * Checks: the commit snapshot, generation stale-drop, play/stop, gain scaling, and the
 * rt_create event-ring bound. (DBAP/align properties live in dsp_test.c.)
 */
#include "rt.h"
#include "layout.h"
#include "ambisonics.h"   /* BWA_AMBI_CH for the pathing accumulator capture */
#include "ism.h"          /* IsmRoom for the early-reflection section */
#include "dr_wav.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>          /* Sleep, for the streaming-fill wait */

#define N    256
#define CH   BWA_CHANNELS
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
static void render2(RtCore* c) { bwa_timestamp ts = { 0, 0 }; rt_render(c, bus, N, &ts); rt_render(c, bus, N, &ts); }

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

static int write_sine_wav(const char* path, double freq, uint32_t frames) {
    drwav_data_format fmt = { drwav_container_riff, DR_WAVE_FORMAT_IEEE_FLOAT, 1, RATE, 32 };
    drwav wav;
    if (!drwav_init_file_write(&wav, path, &fmt, NULL)) return 0;
    float* buf = (float*)malloc((size_t)frames * sizeof(float));
    if (!buf) { drwav_uninit(&wav); return 0; }
    for (uint32_t i = 0; i < frames; ++i) buf[i] = (float)sin(2.0 * 3.14159265358979 * freq * i / RATE);
    drwav_uint64 wrote = drwav_write_pcm_frames(&wav, frames, buf);
    free(buf); drwav_uninit(&wav);
    return wrote == frames;
}
static int write_impulse_wav(const char* path, uint32_t frames) {
    drwav_data_format fmt = { drwav_container_riff, DR_WAVE_FORMAT_IEEE_FLOAT, 1, RATE, 32 };
    drwav wav;
    if (!drwav_init_file_write(&wav, path, &fmt, NULL)) return 0;
    float* buf = (float*)calloc((size_t)frames, sizeof(float));
    if (!buf) { drwav_uninit(&wav); return 0; }
    buf[0] = 1.0f;                                   /* unit impulse at frame 0, silence after */
    drwav_uint64 wrote = drwav_write_pcm_frames(&wav, frames, buf);
    free(buf); drwav_uninit(&wav);
    return wrote == frames;
}
/* an impulse at frame `at`, so the voice's gain ramp-in (one block from 0) is long settled when it
 * fires — otherwise the ramp would swallow an impulse sitting at frame 0. */
static int write_impulse_at_wav(const char* path, uint32_t at, uint32_t frames) {
    drwav_data_format fmt = { drwav_container_riff, DR_WAVE_FORMAT_IEEE_FLOAT, 1, RATE, 32 };
    drwav wav;
    if (!drwav_init_file_write(&wav, path, &fmt, NULL)) return 0;
    float* buf = (float*)calloc((size_t)frames, sizeof(float));
    if (!buf) { drwav_uninit(&wav); return 0; }
    if (at < frames) buf[at] = 1.0f;
    drwav_uint64 wrote = drwav_write_pcm_frames(&wav, frames, buf);
    free(buf); drwav_uninit(&wav);
    return wrote == frames;
}
static float lcg_noise_next(uint32_t* s) {           /* rt.c's LCG, [-1, 1) */
    *s = *s * 1664525u + 1013904223u;
    return (float)(*s >> 9) * (1.0f / 4194304.0f) - 1.0f;
}
static int write_noise_wav(const char* path, uint32_t frames) {
    drwav_data_format fmt = { drwav_container_riff, DR_WAVE_FORMAT_IEEE_FLOAT, 1, RATE, 32 };
    drwav wav;
    if (!drwav_init_file_write(&wav, path, &fmt, NULL)) return 0;
    float* buf = (float*)malloc((size_t)frames * sizeof(float));
    if (!buf) { drwav_uninit(&wav); return 0; }
    uint32_t s = 1;
    for (uint32_t i = 0; i < frames; ++i) buf[i] = 0.5f * lcg_noise_next(&s);
    drwav_uint64 wrote = drwav_write_pcm_frames(&wav, frames, buf);
    free(buf); drwav_uninit(&wav);
    return wrote == frames;
}
/* 4-ch (1st-order AmbiX) wav: ONE noise signal scaled per channel (ACN order W/Y/Z/X, SN3D) —
 * (1,0,0,1) with x = the W amp encodes a plane wave from ambi +x (room +z); (1,0,0,0) is W-only
 * (zero intensity: reads as fully diffuse to the parametric analysis). */
static int write_ambix4_noise_wav(const char* path, float w, float y, float z, float x, uint32_t frames) {
    drwav_data_format fmt = { drwav_container_riff, DR_WAVE_FORMAT_IEEE_FLOAT, 4, RATE, 32 };
    drwav wav;
    if (!drwav_init_file_write(&wav, path, &fmt, NULL)) return 0;
    float* buf = (float*)malloc((size_t)frames * 4 * sizeof(float));
    if (!buf) { drwav_uninit(&wav); return 0; }
    uint32_t s = 7;
    for (uint32_t i = 0; i < frames; ++i) {
        float v = 0.5f * lcg_noise_next(&s);
        buf[i*4+0] = w*v; buf[i*4+1] = y*v; buf[i*4+2] = z*v; buf[i*4+3] = x*v;
    }
    drwav_uint64 wrote = drwav_write_pcm_frames(&wav, frames, buf);
    free(buf); drwav_uninit(&wav);
    return wrote == frames;
}
/* render `kb` blocks, summing all 26 channels to mono per sample into out[kb*N]; the per-channel pan
 * gains are all >= 0 for one source, so the mono sum is a scaled copy of the (propagated) source. */
static void render_capture_mono(RtCore* c, float* out, int kb) {
    bwa_timestamp ts = { 0, 0 };
    for (int b = 0; b < kb; ++b) {
        rt_render(c, bus, N, &ts);
        for (int i = 0; i < N; ++i) { double s = 0; for (int ch = 0; ch < CH; ++ch) s += bus[(size_t)ch*N + i]; out[b*N + i] = (float)s; }
    }
}
/* same, but move the source on +x from d0 to d1 (one step per block) so the Doppler delay glides */
static void render_capture_mono_moving(RtCore* c, uint32_t h, float d0, float d1, float* out, int kb) {
    bwa_timestamp ts = { 0, 0 };
    for (int b = 0; b < kb; ++b) {
        float d = d0 + (d1 - d0) * ((float)b / (float)(kb - 1));
        rt_source_set_pos(c, h, d, LD.ref[1], 0.f); rt_commit(c);   /* on the ear plane: distance == d */
        rt_render(c, bus, N, &ts);
        for (int i = 0; i < N; ++i) { double s = 0; for (int ch = 0; ch < CH; ++ch) s += bus[(size_t)ch*N + i]; out[b*N + i] = (float)s; }
    }
}
static int count_zc(const float* x, int n) {     /* sign changes (zero crossings) */
    int z = 0; for (int i = 1; i < n; ++i) if ((x[i-1] <= 0.f) != (x[i] <= 0.f)) ++z; return z;
}
static int argmax_abs(const float* x, int n) {
    int best = 0; float bm = -1.f; for (int i = 0; i < n; ++i) { float a = fabsf(x[i]); if (a > bm) { bm = a; best = i; } } return best;
}
static double total_l2(void) { double e = 0; for (int i = 0; i < CH * N; ++i) e += (double)bus[i] * bus[i]; return sqrt(e); }
static int active_channels(double frac) {     /* channels carrying > frac of the total energy */
    double tot = total_energy(); int z = 0;
    if (tot <= 0) return 0;
    for (int ch = 0; ch < CH; ++ch) if (chan_energy(ch) > frac * tot) ++z;
    return z;
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

static int      g_path_calls;
static uint32_t g_path_chn;
static float    g_path_cap[BWA_AMBI_CH];   /* last-sample value of each accumulator channel */
static void test_path_tap(void* ud, float* bus, uint32_t n, const float* lp, const float* lq, const float* ambi, uint32_t ambi_ch) {
    (void)ud; (void)lp; (void)lq; (void)bus;
    ++g_path_calls; g_path_chn = ambi_ch;
    for (uint32_t k = 0; k < ambi_ch && k < BWA_AMBI_CH; ++k) g_path_cap[k] = ambi[(size_t)k * n + (n - 1)];
}

int main(void) {
    LD = layout_default();                          /* listener stays at the default (the array centre, LD.ref) */
    const char* WAV = "bwa_rt_const.wav";
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
    CHECK(BWA_H_IDX(h2) == BWA_H_IDX(old), "destroyed slot is reused");
    CHECK(BWA_H_GEN(h2) != BWA_H_GEN(old), "generation is bumped on reuse");
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
    uint32_t stale_occ = BWA_MK_H(BWA_H_IDX(h3), (uint16_t)(BWA_H_GEN(h3) + 7));
    rt_set_occlusion(c, stale_occ, 0.0f); render2(c);
    CHECK(total_energy() > e_clear * 0.5, "occlusion on a stale handle is dropped");

    /* 9. slot recycling clears occlusion: a publish for the prior occupant never attenuates the
     *    voice that reuses its slot (the audio thread gates the publish on its own generation). */
    rt_set_occlusion(c, h3, 0.0f); render2(c);
    CHECK(total_energy() < e_clear * 0.02, "h3 fully occluded before recycling");
    rt_source_destroy(c, h3); render2(c);
    uint32_t h4 = rt_source_create(c);
    CHECK(BWA_H_IDX(h4) == BWA_H_IDX(h3) && h4 != h3, "occlusion: slot reused with a bumped generation");
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
     *     field favors the front speaker over the back one. Room convention (post +z-forward flip):
     *     the listener faces +z, so AmbiX-front (ACN3 X) must decode to the room +z speaker. */
    const char* AMB_OMNI = "bwa_amb_omni.wav", *AMB_FRONT = "bwa_amb_front.wav";
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
                    if (LD.speakers[s].pos[2] >  1.0f) s_front = s;   /* room +z: the listener faces +z */
                    if (LD.speakers[s].pos[2] < -1.0f) s_back  = s;
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

    /* 14. pathing render: an opted-in voice SH-encodes its (un-occluded) signal s*shCoeffs into the
     *     shared ambisonic accumulator, which the path tap receives. The const-1.0 source means the
     *     landed accumulator equals the published shCoeffs exactly; opting out zeroes it. */
    RtCore* cp = rt_create(8, 4, RATE, CH);
    CHECK(cp != NULL, "rt_create (path)");
    if (cp) {
        uint32_t sp = rt_load_sound(cp, WAV, err, sizeof err);
        uint32_t vp = rt_source_create(cp);
        rt_source_play(cp, vp, sp, true);
        rt_source_set_pos(cp, vp, LD.speakers[2].pos[0], LD.speakers[2].pos[1], LD.speakers[2].pos[2]);
        rt_set_path_tap(cp, test_path_tap, NULL, 4);
        rt_source_set_pathing(cp, vp, true);
        const float want[4] = { 0.5f, 0.25f, -0.1f, 0.3f };
        rt_set_pathing(cp, vp, want, NULL, 4);           /* publish a fixed indirect field (flat EQ) for this voice */
        g_path_calls = 0; g_path_chn = 0;
        rt_commit(cp); render2(cp);                      /* block 1 ramps 0->want, block 2 holds at want */
        CHECK(g_path_calls == 2 && g_path_chn == 4, "path tap called once per block with the ambi channel count");
        int matched = 1;
        for (int k = 0; k < 4; ++k) if (fabs((double)g_path_cap[k] - want[k]) > 1e-3) matched = 0;
        CHECK(matched, "accumulator lands on s*shCoeffs (s=1) — the indirect field is encoded from the published directions");
        /* bending-loss EQ: a non-flat band tilt colours the indirect signal BEFORE the SH-encode. The
         * source is DC (s=1) and the RBJ low-shelf DC gain is exactly its band gain, so {0.5,1,1} scales
         * every SH channel by 0.5 once the band-gain slew + biquads settle -> accumulator = 0.5*shCoeffs. */
        const float eqtilt[3] = { 0.5f, 1.0f, 1.0f };
        rt_set_pathing(cp, vp, want, eqtilt, 4);
        for (int b = 0; b < 16; ++b) render2(cp);        /* settle the EQ_SLEW glide + biquad transient */
        int tilted = 1;
        for (int k = 0; k < 4; ++k) if (fabs((double)g_path_cap[k] - 0.5 * want[k]) > 5e-3) tilted = 0;
        CHECK(tilted, "bending-loss EQ tilts the indirect field pre-encode (low-shelf DC gain applied to s_raw)");
        rt_set_pathing(cp, vp, want, NULL, 4);           /* back to flat for the opt-out check */
        for (int b = 0; b < 16; ++b) render2(cp);        /* let the EQ settle back to bypass */
        rt_source_set_pathing(cp, vp, false);            /* opt out -> the accumulator is silent */
        for (int k = 0; k < 4; ++k) g_path_cap[k] = 9.f;
        rt_commit(cp); render2(cp);
        int silent = 1; for (int k = 0; k < 4; ++k) if (fabs((double)g_path_cap[k]) > 1e-6) silent = 0;
        CHECK(silent, "opting out removes the voice from the pathing render");
        rt_destroy(cp);
    }

    /* channel test signal: drives a raw output channel (after align), only that channel */
    {
        RtCore* cs = rt_create(8, 4, RATE, CH);
        CHECK(cs != NULL, "rt_create (test signal)");
        if (cs) {
            bwa_timestamp ts = { 0, 0 };
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

    /* source spread: widening a point source spreads its energy across more speakers, constant-power */
    {
        RtCore* cs = rt_create(8, 4, RATE, CH);
        CHECK(cs != NULL, "rt_create (spread)");
        if (cs) {
            uint32_t ssnd = rt_load_sound(cs, WAV, err, sizeof err);
            uint32_t hsp = rt_source_create(cs);
            rt_source_play(cs, hsp, ssnd, true);
            set_pos_spk(cs, hsp, 7);                         /* a point at speaker 7 */
            rt_commit(cs); render2(cs);
            int    act_point = active_channels(0.03);
            double l2_point  = total_l2();
            double share_pt  = chan_energy(argmax_channel()) / total_energy();
            rt_source_set_spread(cs, hsp, 1.0f);             /* widen to maximum */
            rt_commit(cs); render2(cs);
            int    act_spread = active_channels(0.03);
            double l2_spread  = total_l2();
            double share_sp   = chan_energy(argmax_channel()) / total_energy();
            CHECK(act_spread > act_point + 2, "spread widens the source across more speakers");
            CHECK(share_sp < share_pt, "spread lowers the dominant channel's share");
            CHECK(l2_point > 0 && fabs(l2_spread - l2_point) / l2_point < 0.02, "spread preserves total power (constant-power)");

            /* MDAP mode: the same widening contract from a different construction (a ring of virtual
             * panner solves). Live A/B atomic like the panner: switch, re-dirty via commit, render. */
            rt_set_spread_mode(cs, 1);
            rt_source_set_pos(cs, hsp, 0.f, 0.f, 0.f); rt_commit(cs); render2(cs);   /* nudge -> re-solve */
            set_pos_spk(cs, hsp, 7); rt_commit(cs); render2(cs);
            int    act_mdap = active_channels(0.03);
            double l2_mdap  = total_l2();
            double share_md = chan_energy(argmax_channel()) / total_energy();
            CHECK(act_mdap > act_point + 2, "MDAP spread widens the source across more speakers");
            CHECK(share_md < share_pt, "MDAP spread lowers the dominant channel's share");
            CHECK(l2_point > 0 && fabs(l2_mdap - l2_point) / l2_point < 0.02, "MDAP spread preserves total power");
            /* spread 0 under MDAP mode = the plain point solve (the ring collapses onto the source) */
            rt_source_set_spread(cs, hsp, 0.0f); rt_commit(cs); render2(cs);
            int    act_md0 = active_channels(0.03);
            double l2_md0  = total_l2();
            CHECK(act_md0 == act_point && fabs(l2_md0 - l2_point) / l2_point < 0.02,
                  "MDAP at spread 0 is the point solve");
            rt_set_spread_mode(cs, 0);
            rt_source_destroy(cs, hsp); rt_commit(cs);
            rt_destroy(cs);
        }
    }

    /* tracked room EQ (room_eq_grid): the align biquads re-aim as the committed listener moves — a
     * 100 Hz voice equidistant from two grid positions (flat at A, -12 dB at B on EVERY channel)
     * drops by ~the IDW-interpolated depth when the listener walks A -> B, and the live kill switch
     * glides it back to flat. Total L2 isolates the EQ: the move also redistributes the panning, but
     * constant power keeps ||bus|| fixed, and the same cut on every channel scales it uniformly. */
    {
        RtCore* cg = rt_create(8, 4, RATE, CH);
        CHECK(cg != NULL, "rt_create (tracked room eq)");
        if (cg) {
            Layout G = layout_default();
            G.rq_grid.npos = 2;
            G.rq_grid.pos[0][0] = -0.5f; G.rq_grid.pos[0][1] = 1.5f; G.rq_grid.pos[0][2] = 0.f;
            G.rq_grid.pos[1][0] =  0.5f; G.rq_grid.pos[1][1] = 1.5f; G.rq_grid.pos[1][2] = 0.f;
            for (int k = 0; k < CH; ++k) {
                G.rq_grid.nsec[k]  = 1;
                G.rq_grid.fc[k][0] = 100.f; G.rq_grid.q[k][0] = 2.f;
                G.rq_grid.gain_db[0][k][0] = 0.f;
                G.rq_grid.gain_db[1][k][0] = -12.f;
            }
            rt_set_layout(cg, &G);
            const char* SW = "bwa_rt_sine100.wav";
            if (write_sine_wav(SW, 100.0, 4800)) {               /* exactly 10 cycles: seamless loop */
                uint32_t sg = rt_load_sound(cg, SW, err, sizeof err);
                uint32_t hg = rt_source_create(cg);
                rt_source_play(cg, hg, sg, true);
                rt_source_set_pos(cg, hg, 0.f, 1.5f, 0.f);       /* equidistant from A and B (same atten) */
                const float qid[4] = { 0, 0, 0, 1 };
                const float pa[3] = { -0.5f, 1.5f, 0.f }, pb[3] = { 0.5f, 1.5f, 0.f };
                rt_set_listener(cg, pa, qid); rt_commit(cg);
                double l2_a = 0, l2_b = 0, l2_off = 0;
                for (int b = 0; b < 100; ++b) render2(cg);       /* settle at A */
                for (int b = 0; b <   8; ++b) { render2(cg); l2_a += total_l2(); }
                rt_set_listener(cg, pb, qid); rt_commit(cg);
                for (int b = 0; b < 200; ++b) render2(cg);       /* walk + settle (12 dB at 24 dB/s = 0.5 s) */
                for (int b = 0; b <   8; ++b) { render2(cg); l2_b += total_l2(); }
                double drop_db = 20.0 * log10(l2_a / l2_b);      /* IDW at B: ~ -11.7 dB (A still pulls a little) */
                CHECK(drop_db > 9.0 && drop_db < 14.0, "tracked room EQ follows the listener (A flat -> B cut)");
                rt_set_room_eq_dyn(cg, 0);                       /* kill switch: glide every section to flat */
                for (int b = 0; b < 200; ++b) render2(cg);
                for (int b = 0; b <   8; ++b) { render2(cg); l2_off += total_l2(); }
                CHECK(fabs(20.0 * log10(l2_a / l2_off)) < 1.5, "tracked room EQ off glides back to flat");
                rt_source_destroy(cg, hg); rt_commit(cg);
            } else CHECK(0, "write 100 Hz sine");
            rt_destroy(cg);
            remove("bwa_rt_sine100.wav");
        }
    }

    /* decorrelation (bwa_set_decorrelation): a fully-spread noise source's speaker feeds are IDENTICAL
     * scaled copies with decor off (zero-lag correlation ~ +1) and mutually incoherent with it on
     * (each channel passes its own velvet filter), at the same total power; toggling back restores
     * coherence (the split amplitude ramps out and the velvet tail flushes). */
    {
        RtCore* cd = rt_create(8, 4, RATE, CH);
        CHECK(cd != NULL, "rt_create (decorrelation)");
        if (cd) {
            const char* NW = "bwa_rt_noise.wav";
            if (write_noise_wav(NW, 8 * N)) {
                uint32_t nd = rt_load_sound(cd, NW, err, sizeof err);
                uint32_t hd = rt_source_create(cd);
                rt_source_play(cd, hd, nd, true);
                rt_source_set_pos(cd, hd, 0.7f, 1.5f, 0.4f);
                rt_source_set_spread(cd, hd, 1.0f);              /* wide: many active channels */
                rt_commit(cd);
                for (int b = 0; b < 8; ++b) render2(cd);
                /* pick the two strongest channels while coherent (stable across the toggle) */
                int ca = argmax_channel(); double ea = chan_energy(ca);
                int cb2 = -1; double eb = -1;
                for (int ch = 0; ch < CH; ++ch) if (ch != ca && chan_energy(ch) > eb) { eb = chan_energy(ch); cb2 = ch; }
                double l2_coh = total_l2();
                #define XCORR(A, B, OUT) do {                                                   \
                    double sab_ = 0, saa_ = 0, sbb_ = 0;                                        \
                    for (int i_ = 0; i_ < (int)N; ++i_) {                                       \
                        double xa_ = bus[(size_t)(A)*N + i_], xb_ = bus[(size_t)(B)*N + i_];    \
                        sab_ += xa_*xb_; saa_ += xa_*xa_; sbb_ += xb_*xb_;                      \
                    }                                                                           \
                    (OUT) = (saa_ > 0 && sbb_ > 0) ? sab_ / sqrt(saa_*sbb_) : 0.0;              \
                } while (0)
                double r_off; XCORR(ca, cb2, r_off);
                CHECK(r_off > 0.95, "decor off: spread feeds are coherent copies (corr ~ +1)");
                rt_set_decorrelation(cd, 1);
                for (int b = 0; b < 20; ++b) render2(cd);        /* ramp in + fill the velvet history */
                double r_on; XCORR(ca, cb2, r_on);
                double l2_dc = total_l2();
                CHECK(fabs(r_on) < 0.4, "decor on: the same feeds are mutually incoherent");
                CHECK(l2_coh > 0 && fabs(20.0 * log10(l2_dc / l2_coh)) < 1.5,
                      "decorrelation preserves total power (~unit-energy filters)");
                rt_set_decorrelation(cd, 0);
                for (int b = 0; b < 20; ++b) render2(cd);        /* ramp out + flush the tail */
                double r_back; XCORR(ca, cb2, r_back);
                CHECK(r_back > 0.95, "decor off again: coherence restored (click-free A/B round trip)");
                #undef XCORR
                rt_source_destroy(cd, hd); rt_commit(cd);
            } else CHECK(0, "write noise wav");
            rt_destroy(cd);
            remove("bwa_rt_noise.wav");
        }
    }

    /* parametric bed renderer (bwa_set_bed_renderer): a noise PLANE-WAVE bed (W=X: from room +z)
     * localizes SHARPER than the matrix decode and stays loudness-matched; a W-only bed (zero
     * intensity -> fully diffuse) spreads across many channels through the decorrelators at matched
     * power. The switch crossfades live. */
    {
        RtCore* cb = rt_create(8, 4, RATE, CH);
        CHECK(cb != NULL, "rt_create (parametric bed)");
        if (cb) {
            const char* PW = "bwa_rt_bed_pw.wav", *DW = "bwa_rt_bed_w.wav";
            if (write_ambix4_noise_wav(PW, 1.f, 0.f, 0.f, 1.f, 8 * N) &&
                write_ambix4_noise_wav(DW, 1.f, 0.f, 0.f, 0.f, 8 * N)) {
                uint32_t sp = rt_load_ambix(cb, PW, err, sizeof err);
                CHECK(sp != 0, err[0] ? err : "load plane-wave bed");
                uint32_t hp = rt_source_create(cb);
                rt_source_play(cb, hp, sp, true);
                rt_commit(cb);
                for (int b = 0; b < 8; ++b) render2(cb);
                double l2_m    = total_l2();
                double share_m = chan_energy(argmax_channel()) / total_energy();
                rt_set_bed_renderer(cb, 1);
                for (int b = 0; b < 60; ++b) render2(cb);        /* crossfade + analysis smoothing settle */
                double l2_p    = total_l2();
                double share_p = chan_energy(argmax_channel()) / total_energy();
                int    amax    = argmax_channel();
                CHECK(LD.speakers[amax].pos[2] > 1.0f, "parametric: plane wave from room +z lands on the +z wall");
                CHECK(share_p > share_m * 1.3, "parametric: the direct stream localizes sharper than the matrix decode");
                CHECK(l2_m > 0 && fabs(20.0 * log10(l2_p / l2_m)) < 2.5,
                      "parametric: plane-wave loudness matches the matrix decode (bed_pref)");
                rt_set_bed_renderer(cb, 0);
                for (int b = 0; b < 60; ++b) render2(cb);
                double l2_back = total_l2();
                CHECK(fabs(20.0 * log10(l2_back / l2_m)) < 0.5, "parametric -> matrix round trip restores the decode");
                rt_source_destroy(cb, hp); rt_commit(cb);

                uint32_t sd = rt_load_ambix(cb, DW, err, sizeof err);   /* W-only: fully diffuse */
                uint32_t hd2 = rt_source_create(cb);
                rt_source_play(cb, hd2, sd, true);
                rt_commit(cb);
                for (int b = 0; b < 8; ++b) render2(cb);
                double l2_dm = total_l2();
                rt_set_bed_renderer(cb, 1);
                for (int b = 0; b < 60; ++b) render2(cb);
                double l2_dp = total_l2();
                CHECK(active_channels(0.02) >= 10, "parametric: a diffuse bed stays spread over many speakers");
                CHECK(l2_dm > 0 && fabs(20.0 * log10(l2_dp / l2_dm)) < 2.0,
                      "parametric: diffuse loudness matches the matrix decode (decorrelated, unit energy)");
                rt_source_destroy(cb, hd2); rt_commit(cb);
            } else CHECK(0, "write ambix noise beds");
            rt_destroy(cb);
            remove(PW); remove(DW);
        }
    }

    /* pose prediction (rt_set_pose_prediction): with a tracked listener walking at a constant
     * velocity, the rendered pose LEADS the freshest tracker sample by lead x velocity — the
     * velocity estimated purely from the tracker's own timestamps. Lead 0 = passthrough. */
    {
        RtCore* cp = rt_create(8, 4, RATE, CH);
        CHECK(cp != NULL, "rt_create (pose prediction)");
        if (cp) {
            static PoseSlot slot;                            /* single writer (this thread) */
            memset(&slot, 0, sizeof slot);
            rt_set_tracker(cp, &slot);
            const float q[4] = { 0, 0, 0, 1 };
            bwa_timestamp ts = { 0, 0 };
            float px = 0.f;
            rt_set_pose_prediction(cp, 0.f);                 /* off: readback == written */
            for (int k = 0; k < 20; ++k) {
                px = 0.5f * (float)k * 0.01f;                /* 0.5 m/s, one write per 10 ms */
                float p[3] = { px, 1.5f, 0.f };
                pose_write_t(&slot, p, q, (uint64_t)(k + 1) * 10000000ull);
                rt_render(cp, bus, N, &ts);
            }
            float rp[3], rq[4];
            rt_read_pose(cp, rp, rq);
            CHECK(fabsf(rp[0] - px) < 1e-6f, "prediction off: rendered pose == tracked pose");
            rt_set_pose_prediction(cp, 0.05f);               /* 50 ms lead */
            int k0 = 20;
            for (int k = k0; k < k0 + 60; ++k) {             /* 0.6 s: the velocity estimate settles */
                px = 0.5f * (float)k * 0.01f;
                float p[3] = { px, 1.5f, 0.f };
                pose_write_t(&slot, p, q, (uint64_t)(k + 1) * 10000000ull);
                rt_render(cp, bus, N, &ts);
            }
            rt_read_pose(cp, rp, rq);
            float lead_m = rp[0] - px;                       /* want 0.5 m/s * 0.05 s = 25 mm */
            printf("posepred: lead = %.1f mm (want 25)\n", lead_m * 1000.f);
            CHECK(lead_m > 0.018f && lead_m < 0.030f, "predicted pose leads by ~velocity x lead");
            CHECK(fabsf(rp[1] - 1.5f) < 1e-4f, "no lead on the static axes");
            rt_set_tracker(cp, NULL);
            rt_destroy(cp);
        }
    }

    /* near-listener widening (rt_set_near_spread): a point source close to the listener widens
     * (spread floored by 1 - dist/radius) instead of collapsing into the nearest speaker; a source
     * beyond the radius is untouched. */
    {
        RtCore* cn = rt_create(8, 4, RATE, CH);
        CHECK(cn != NULL, "rt_create (near spread)");
        if (cn) {
            uint32_t ns = rt_load_sound(cn, WAV, err, sizeof err);
            uint32_t hn = rt_source_create(cn);
            rt_source_play(cn, hn, ns, true);
            /* stand the listener 0.3 m from speaker 7, source AT the speaker: without the policy the
             * point solve concentrates there (the collapse the feature exists to prevent) */
            const float* sp7 = LD.speakers[7].pos;
            const float qn[4] = { 0, 0, 0, 1 };
            const float lp7[3] = { sp7[0] - 0.3f, sp7[1], sp7[2] };
            rt_set_listener(cn, lp7, qn);
            set_pos_spk(cn, hn, 7);
            rt_commit(cn); render2(cn);
            int act_close = active_channels(0.03);
            rt_set_near_spread(cn, 1.0f);
            rt_source_set_pos(cn, hn, sp7[0], sp7[1] + 0.001f, sp7[2]);   /* nudge: re-solve with the policy */
            rt_commit(cn); render2(cn);
            int act_near = active_channels(0.03);
            CHECK(act_near > act_close + 2, "a source inside the radius widens (spread floor engages)");
            rt_source_set_pos(cn, hn, lp7[0] - 2.5f, sp7[1], sp7[2]);     /* beyond the radius: point behavior */
            rt_commit(cn); render2(cn); render2(cn);
            int act_far = active_channels(0.03);
            rt_set_near_spread(cn, 0.f);
            rt_source_set_pos(cn, hn, lp7[0] - 2.501f, sp7[1], sp7[2]);
            rt_commit(cn); render2(cn); render2(cn);
            int act_far_off = active_channels(0.03);
            CHECK(abs(act_far - act_far_off) <= 1, "a source beyond the radius is untouched");
            rt_source_destroy(cn, hn); rt_commit(cn);
            rt_destroy(cn);
        }
    }

    /* metric source size (bwa_source_set_size): the rendered width is the angle the radius subtends
     * from the listener — a source that engulfs the listener is fully wide, the SAME physical size
     * narrows with distance, and size 0 restores the point solve. */
    {
        RtCore* cz = rt_create(8, 4, RATE, CH);
        CHECK(cz != NULL, "rt_create (source size)");
        if (cz) {
            uint32_t zs = rt_load_sound(cz, WAV, err, sizeof err);
            uint32_t hz = rt_source_create(cz);
            rt_source_play(cz, hz, zs, true);
            const float* sp7 = LD.speakers[7].pos;
            const float qz[4] = { 0, 0, 0, 1 };
            const float lz[3] = { sp7[0] - 0.5f, sp7[1], sp7[2] };
            rt_set_listener(cz, lz, qz);                 /* 0.5 m from speaker 7 */
            set_pos_spk(cz, hz, 7);                      /* source at the speaker: concentrated point */
            rt_commit(cz); render2(cz);
            int act_point = active_channels(0.03);
            rt_source_set_size(cz, hz, 1.0f);            /* radius 1 m > dist 0.5 m: engulfed */
            rt_commit(cz); render2(cz);
            int act_engulfed = active_channels(0.03);
            CHECK(act_engulfed > act_point + 2, "a source that engulfs the listener goes fully wide");
            rt_source_set_pos(cz, hz, lz[0] + 4.0f, sp7[1], sp7[2]);   /* same 1 m radius, 4 m away */
            rt_commit(cz); render2(cz); render2(cz);
            int act_far = active_channels(0.03);
            CHECK(act_far < act_engulfed, "the same physical size subtends less at distance (narrows)");
            rt_source_set_size(cz, hz, 0.f);             /* back to a point */
            set_pos_spk(cz, hz, 7);
            rt_commit(cz); render2(cz); render2(cz);
            int act_back = active_channels(0.03);
            CHECK(abs(act_back - act_point) <= 1, "size 0 restores the point solve");
            rt_source_destroy(cz, hz); rt_commit(cz);
            rt_destroy(cz);
        }
    }

    /* equal-loudness distance compensation (bwa_source_set_loudness_comp): at -12 dB of distance
     * attenuation a 100 Hz tone gains ~ +4.5 dB of shelf (0.4 dB/dB, below the 250 Hz corner);
     * a 5 kHz tone is untouched; opt-out ramps back to flat. */
    {
        RtCore* cl = rt_create(8, 4, RATE, CH);
        CHECK(cl != NULL, "rt_create (loudness comp)");
        if (cl) {
            const char* LW = "bwa_rt_ldc100.wav";
            if (write_sine_wav(LW, 100.0, 4800)) {
                uint32_t sl = rt_load_sound(cl, LW, err, sizeof err);
                uint32_t hl = rt_source_create(cl);
                rt_source_play(cl, hl, sl, true);
                rt_source_set_pos(cl, hl, 4.0f, 1.5f, 0.f);  /* 4 m: atten = 1/4 = -12 dB (ref 1 m, rolloff 1) */
                rt_commit(cl);
                for (int b = 0; b < 4; ++b) render2(cl);
                double l2_off = 0; for (int b = 0; b < 4; ++b) { render2(cl); l2_off += total_l2(); }
                rt_source_set_loudness_comp(cl, hl, true);
                for (int b = 0; b < 8; ++b) render2(cl);     /* ramp + shelf settle */
                double l2_on = 0; for (int b = 0; b < 4; ++b) { render2(cl); l2_on += total_l2(); }
                double boost_db = 20.0 * log10(l2_on / l2_off);
                printf("ldc: 100 Hz boost at -12 dB atten = %.2f dB (want ~4.5)\n", boost_db);
                CHECK(boost_db > 3.2 && boost_db < 5.6, "LF shelf tracks the attenuation (~0.4 dB/dB)");
                rt_source_set_loudness_comp(cl, hl, false);
                for (int b = 0; b < 8; ++b) render2(cl);
                double l2_back = 0; for (int b = 0; b < 4; ++b) { render2(cl); l2_back += total_l2(); }
                CHECK(fabs(20.0 * log10(l2_back / l2_off)) < 0.3, "opt-out ramps back to flat");
                rt_source_destroy(cl, hl); rt_commit(cl);

                const char* HW2 = "bwa_rt_ldc5k.wav";         /* HF: the shelf must not touch it */
                if (write_sine_wav(HW2, 5000.0, 4800)) {
                    uint32_t sh = rt_load_sound(cl, HW2, err, sizeof err);
                    uint32_t hh = rt_source_create(cl);
                    rt_source_play(cl, hh, sh, true);
                    rt_source_set_pos(cl, hh, 4.0f, 1.5f, 0.f);
                    rt_commit(cl);
                    for (int b = 0; b < 4; ++b) render2(cl);
                    double h_off = 0; for (int b = 0; b < 4; ++b) { render2(cl); h_off += total_l2(); }
                    rt_source_set_loudness_comp(cl, hh, true);
                    for (int b = 0; b < 8; ++b) render2(cl);
                    double h_on = 0; for (int b = 0; b < 4; ++b) { render2(cl); h_on += total_l2(); }
                    CHECK(fabs(20.0 * log10(h_on / h_off)) < 0.8, "the shelf leaves HF content alone");
                    rt_source_destroy(cl, hh); rt_commit(cl);
                    remove(HW2);
                } else CHECK(0, "write 5 kHz sine (ldc)");
                remove(LW);
            } else CHECK(0, "write 100 Hz sine (ldc)");
            rt_destroy(cl);
        }
    }

    /* multi-listener compromise (rt_set_extra_listeners): one listener west of a centred source
     * biases the render east (DBAP weights the source's bearing); adding a mirrored second listener
     * makes the compromise SYMMETRIC at unchanged total power; clearing restores the bias. */
    {
        RtCore* cm = rt_create(8, 4, RATE, CH);
        CHECK(cm != NULL, "rt_create (multi-listener)");
        if (cm) {
            uint32_t sm = rt_load_sound(cm, WAV, err, sizeof err);
            uint32_t hm = rt_source_create(cm);
            rt_source_play(cm, hm, sm, true);
            rt_source_set_pos(cm, hm, 0.f, 1.5f, 0.f);       /* the array centre */
            const float qid2[4] = { 0, 0, 0, 1 };
            rt_set_listener(cm, (const float[3]){ -1.f, 1.5f, 0.f }, qid2);
            rt_commit(cm); render2(cm);
            #define SIDE_E(SGN, OUT) do {                                                  \
                double e_ = 0;                                                             \
                for (int ch_ = 0; ch_ < CH; ++ch_)                                         \
                    if ((SGN) * LD.speakers[ch_].pos[0] > 1.f) e_ += chan_energy(ch_);     \
                (OUT) = e_;                                                                \
            } while (0)
            double epx, enx;
            SIDE_E(+1, epx); SIDE_E(-1, enx);
            double l2_single = total_l2();
            CHECK(epx > enx * 1.15, "single listener west of the source biases the render east");
            const float exl[3] = { 1.f, 1.5f, 0.f };         /* the mirrored second occupant */
            rt_set_extra_listeners(cm, exl, 1);
            rt_commit(cm); render2(cm); render2(cm);
            double epx2, enx2;
            SIDE_E(+1, epx2); SIDE_E(-1, enx2);
            double l2_multi = total_l2();
            CHECK(fabs(epx2 - enx2) / (epx2 + enx2) < 0.08,
                  "mirrored second listener makes the compromise symmetric (energy mean)");
            CHECK(fabs(20.0 * log10(l2_multi / l2_single)) < 1.0,
                  "compromise panning preserves total power");
            rt_set_extra_listeners(cm, NULL, 0);             /* back to single-listener panning */
            rt_commit(cm); render2(cm); render2(cm);
            double epx3, enx3;
            SIDE_E(+1, epx3); SIDE_E(-1, enx3);
            CHECK(epx3 > enx3 * 1.15, "clearing the extras restores single-listener panning");
            #undef SIDE_E
            rt_source_destroy(cm, hm); rt_commit(cm);
            rt_destroy(cm);
        }
    }

    /* QoL batch: master gain, mix groups (gain + pause), global pause, timed fades, voice gauge.
     * Two voices in different groups at different speakers, so per-group effects read per-channel. */
    {
        RtCore* cq = rt_create(8, 4, RATE, CH);
        CHECK(cq != NULL, "rt_create (qol)");
        if (cq) {
            uint32_t sq = rt_load_sound(cq, WAV, err, sizeof err);
            uint32_t h1 = rt_source_create(cq), h2 = rt_source_create(cq);
            rt_source_play(cq, h1, sq, true); set_pos_spk(cq, h1, 3);  rt_source_set_group(cq, h1, 1);
            rt_source_play(cq, h2, sq, true); set_pos_spk(cq, h2, 20); rt_source_set_group(cq, h2, 2);
            rt_commit(cq); render2(cq);
            CHECK(rt_active_voices(cq) == 2, "active-voice gauge reads 2");
            double e3 = chan_energy(3), e20 = chan_energy(20), l2_base = total_l2();
            CHECK(e3 > 0.1 && e20 > 0.1, "both group voices render");

            rt_set_master_gain(cq, 0.5f);                      /* master: -6 dB over everything */
            render2(cq); render2(cq);
            CHECK(fabs(20.0*log10(total_l2()/l2_base) + 6.02) < 0.3, "master gain scales the whole mix");
            rt_set_master_gain(cq, 1.f); render2(cq); render2(cq);

            rt_group_set_gain(cq, 1, 0.25f);                   /* group 1: -12 dB; group 2 untouched */
            render2(cq); render2(cq);
            CHECK(fabs(20.0*log10(chan_energy(3)/e3) + 12.04) < 0.8, "group gain scales its members");
            CHECK(fabs(20.0*log10(chan_energy(20)/e20)) < 0.3,       "other groups untouched");
            rt_group_set_gain(cq, 1, 1.f); render2(cq); render2(cq);

            rt_group_set_paused(cq, 2, true);                  /* group pause: silent, frozen, still 'playing' */
            render2(cq); render2(cq);
            CHECK(chan_energy(20) < e20 * 0.02, "paused group is silent (only voice-1's DBAP leakage remains)");
            CHECK(chan_energy(3) > 0.1, "unpaused group keeps playing");
            CHECK(rt_source_is_playing(cq, h2), "a group-paused voice still reads as playing");
            rt_group_set_paused(cq, 2, false); render2(cq);
            CHECK(chan_energy(20) > 0.05, "group resume");

            rt_set_all_paused(cq, 1);                          /* global pause: everything out, everything back */
            render2(cq); render2(cq);
            CHECK(total_energy() < 1e-6, "global pause silences the mix");
            rt_set_all_paused(cq, 0); render2(cq);
            CHECK(total_energy() > 0.1, "global resume");

            rt_source_fade_to(cq, h1, 0.25f, 0.1f, false);     /* timed fade: glide to -12 dB over 0.1 s */
            for (int b = 0; b < 5; ++b) render2(cq);           /* ~0.053 s: mid-fade */
            double e_mid = chan_energy(3);
            for (int b = 0; b < 10; ++b) render2(cq);          /* well past the landing */
            double e_end = chan_energy(3);
            CHECK(e_mid < e3 * 0.95 && e_mid > e_end * 1.1, "fade glides through intermediate levels");
            CHECK(fabs(20.0*log10(e_end/e3) + 12.04) < 0.8,   "fade lands on its target");

            rt_source_fade_to(cq, h2, 0.f, 0.05f, true);       /* fade-out-and-stop */
            for (int b = 0; b < 15; ++b) render2(cq);
            CHECK(!rt_source_is_playing(cq, h2), "fade_out stops the voice once landed");
            CHECK(chan_energy(20) < e20 * 0.02, "faded-out voice is silent (only leakage remains)");
            CHECK(rt_active_voices(cq) == 1, "the gauge tracks the stop");

            rt_source_destroy(cq, h1); rt_source_destroy(cq, h2); rt_commit(cq);
            rt_destroy(cq);
        }
    }

    /* pitch (bwa_source_set_pitch): a looping 1 kHz sine at rate 2 doubles its zero-crossing count,
     * at rate 0.5 halves it; rate 1 is the untouched integer path. */
    {
        RtCore* cz = rt_create(8, 4, RATE, CH);
        CHECK(cz != NULL, "rt_create (pitch)");
        if (cz) {
            const char* PW2 = "bwa_rt_pitch1k.wav";
            if (write_sine_wav(PW2, 1000.0, 4800)) {           /* integer cycles: seamless loop */
                enum { KB = 8 };
                static float mono[KB * N];
                uint32_t sp2 = rt_load_sound(cz, PW2, err, sizeof err);
                uint32_t hp2 = rt_source_create(cz);
                rt_source_play(cz, hp2, sp2, true);
                rt_source_set_pos(cz, hp2, 0.f, 1.5f, 0.f);
                rt_commit(cz); render2(cz);
                render_capture_mono(cz, mono, KB);
                int zc1 = count_zc(mono, KB * N);
                rt_source_set_pitch(cz, hp2, 2.0f);
                render2(cz); render2(cz);                      /* glide lands within a block; settle */
                render_capture_mono(cz, mono, KB);
                int zc2 = count_zc(mono, KB * N);
                rt_source_set_pitch(cz, hp2, 0.5f);
                render2(cz); render2(cz);
                render_capture_mono(cz, mono, KB);
                int zch = count_zc(mono, KB * N);
                printf("pitch: zc x1=%d x2=%d x0.5=%d\n", zc1, zc2, zch);
                CHECK(zc1 > 60, "baseline tone renders");
                CHECK(fabs((double)zc2 / zc1 - 2.0) < 0.12, "pitch 2.0 doubles the frequency");
                CHECK(fabs((double)zch / zc1 - 0.5) < 0.06, "pitch 0.5 halves the frequency");
                rt_source_destroy(cz, hp2); rt_commit(cz);
                remove(PW2);
            } else CHECK(0, "write 1 kHz sine (pitch)");
            rt_destroy(cz);
        }
    }

    /* bed rotation (bwa_bed_set_rotation): a plane-wave bed from room +z, yawed +pi/2, re-localizes
     * on the +x wall at conserved level — the closed-form yaw SH rotation, glided. */
    {
        RtCore* cr = rt_create(8, 4, RATE, CH);
        CHECK(cr != NULL, "rt_create (bed rotation)");
        if (cr) {
            const char* BW2 = "bwa_rt_bed_rot.wav";
            if (write_ambix4_noise_wav(BW2, 1.f, 0.f, 0.f, 1.f, 8 * N)) {
                uint32_t sb = rt_load_ambix(cr, BW2, err, sizeof err);
                uint32_t hb = rt_source_create(cr);
                rt_source_play(cr, hb, sb, true);
                rt_commit(cr);
                for (int b = 0; b < 8; ++b) render2(cr);
                int    a0  = argmax_channel();
                double l0  = total_l2();
                CHECK(LD.speakers[a0].pos[2] > 1.0f, "unrotated plane wave lands on the +z wall");
                rt_bed_set_rotation(cr, hb, 1.5707963f);       /* +90°: field turns toward room +x */
                for (int b = 0; b < 80; ++b) render2(cr);      /* glide (0.25 s at 1 turn/s) + settle */
                int    a1 = argmax_channel();
                double l1 = total_l2();
                CHECK(LD.speakers[a1].pos[0] > 1.0f && fabsf(LD.speakers[a1].pos[2]) < 1.0f,
                      "rotated +90 deg: the field re-localizes on the +x wall");
                CHECK(fabs(20.0 * log10(l1 / l0)) < 1.5, "rotation conserves level (orthogonal transform)");
                rt_source_destroy(cr, hb); rt_commit(cr);
                remove(BW2);
            } else CHECK(0, "write rotation bed");
            rt_destroy(cr);
        }
    }

    /* runtime channel count: a 24-speaker layout drives a 24-channel core end to end — point panning,
     * a bed decode, meters — and a canary proves NOTHING writes beyond the active channel count into
     * a capacity-sized buffer (the exact overrun class the BWA_CHANNELS->count migration must prevent). */
    {
        RtCore* c24 = rt_create(8, 4, RATE, 24);
        CHECK(c24 != NULL, "rt_create (24 ch)");
        if (c24) {
            Layout L24 = layout_default();
            L24.count = 24;                              /* the first 24 grid speakers, indices 0..23 */
            layout_compute_ref(&L24);
            rt_set_layout(c24, &L24);
            uint32_t s24 = rt_load_sound(c24, WAV, err, sizeof err);
            uint32_t h24 = rt_source_create(c24);
            rt_source_play(c24, h24, s24, true);
            rt_source_set_pos(c24, h24, L24.speakers[5].pos[0], L24.speakers[5].pos[1], L24.speakers[5].pos[2]);
            rt_commit(c24);
            static float b24[CH * N];
            for (int i = 24 * (int)N; i < CH * (int)N; ++i) b24[i] = 123.f;   /* canary beyond channel 24 */
            bwa_timestamp t24 = { 0, 0 };
            rt_render(c24, b24, N, &t24); rt_render(c24, b24, N, &t24);
            int best = 0; double bm = -1;
            for (int ch = 0; ch < 24; ++ch) {            /* 24-wide PLANAR indexing */
                double e = 0; for (int i = 0; i < (int)N; ++i) e += fabs(b24[(size_t)ch * N + i]);
                if (e > bm) { bm = e; best = ch; }
            }
            CHECK(best == 5, "24-ch: a source at speaker 5 localizes to channel 5");
            float pk[CH];
            CHECK(rt_bus_peaks(c24, pk, CH) == 24, "24-ch: the meter readback reports 24 channels");
            const char* B24 = "bwa_rt_bed24.wav";         /* a bed too: SH->24 decode, same canary */
            if (write_ambix4_noise_wav(B24, 1.f, 0.f, 0.f, 1.f, 4 * N)) {
                uint32_t sb5 = rt_load_ambix(c24, B24, err, sizeof err);
                uint32_t hb5 = rt_source_create(c24);
                rt_source_play(c24, hb5, sb5, true);
                rt_commit(c24);
                rt_render(c24, b24, N, &t24); rt_render(c24, b24, N, &t24);
                double etot = 0; for (int i = 0; i < 24 * (int)N; ++i) etot += fabs(b24[i]);
                CHECK(etot > 0.1, "24-ch: the bed decodes onto the 24 active channels");
                rt_source_destroy(c24, hb5); rt_commit(c24);
                remove(B24);
            } else CHECK(0, "write 24-ch bed");
            int canary_ok = 1;
            for (int i = 24 * (int)N; i < CH * (int)N; ++i) if (b24[i] != 123.f) canary_ok = 0;
            CHECK(canary_ok, "24-ch: nothing writes beyond the active channel count");
            rt_source_destroy(c24, h24); rt_commit(c24);
            rt_destroy(c24);
        }
    }

    /* image-source early reflections (bwa_source_set_early_reflections): a source hard against the +x
     * wall of a shoebox. Its +x image sits just BEYOND that wall, so the reflection must (a) appear
     * only when enabled, (b) arrive AFTER the direct sound, and (c) come from the +x side — a real
     * point source panned at the mirrored position, not a diffuse bed. */
    {
        RtCore* ci = rt_create(8, 4, RATE, CH);
        CHECK(ci != NULL, "rt_create (early reflections)");
        if (ci) {
            IsmRoom room; memset(&room, 0, sizeof room);
            room.w = 6.f; room.h = 3.f; room.d = 6.f; room.valid = 1;
            for (int f = 0; f < ISM_FACES; ++f)                  /* lively walls: strong first-order returns */
                for (int b = 0; b < 3; ++b) room.absorb[f][b] = 0.1f;
            rt_set_ism_room(ci, &room);
            const float qi[4] = { 0, 0, 0, 1 };
            rt_set_listener(ci, (const float[3]){ 0.f, 1.5f, 0.f }, qi);   /* centre of the room */

            const char* IW = "bwa_rt_imp.wav";
            enum { IMP_AT = 300 };                               /* fires after the voice's one-block gain ramp-in */
            if (write_impulse_at_wav(IW, IMP_AT, 8 * N)) {
                enum { KB = 6 };                                 /* 1536 samples: past every reflection path */
                static double env[KB * N], envp[KB * N], envn[KB * N];   /* |sum| all / +x side / -x side */
                uint32_t si = rt_load_sound(ci, IW, err, sizeof err);
                uint32_t hi = rt_source_create(ci);
                /* fire the impulse and capture KB blocks: the per-sample envelope over the whole
                 * capture, split into the +x and -x speaker halves (the reflection's direction). */
                #define ISM_CAPTURE() do {                                                        \
                    rt_source_play(ci, hi, si, false);                                            \
                    rt_source_set_pos(ci, hi, 2.5f, 1.5f, 0.f);   /* 0.5 m from the +x wall */    \
                    rt_commit(ci);                                                                \
                    bwa_timestamp ts_ = { 0, 0 };                                                   \
                    for (int b_ = 0; b_ < KB; ++b_) {                                             \
                        rt_render(ci, bus, N, &ts_);                                              \
                        for (int i_ = 0; i_ < (int)N; ++i_) {                                     \
                            double a_ = 0, p_ = 0, n_ = 0;                                        \
                            for (int ch_ = 0; ch_ < CH; ++ch_) {                                  \
                                double v_ = fabs(bus[(size_t)ch_ * N + i_]);                      \
                                a_ += v_;                                                         \
                                if (LD.speakers[ch_].pos[0] >  1.f) p_ += v_;                     \
                                if (LD.speakers[ch_].pos[0] < -1.f) n_ += v_;                     \
                            }                                                                     \
                            env[b_ * (int)N + i_] = a_; envp[b_ * (int)N + i_] = p_;              \
                            envn[b_ * (int)N + i_] = n_;                                          \
                        }                                                                         \
                    }                                                                             \
                } while (0)
                #define ISM_SUM(A, B, OUT, SRC) do {                                              \
                    double s_ = 0; for (int i_ = (A); i_ < (B); ++i_) s_ += (SRC)[i_]; (OUT) = s_; \
                } while (0)

                ISM_CAPTURE();                                   /* dry (ISM off): the direct sound only */
                double dry_direct, dry_late;
                ISM_SUM(IMP_AT - 10, IMP_AT + 50, dry_direct, env);
                ISM_SUM(IMP_AT + 120, KB * (int)N, dry_late, env);   /* the reflection window: silent when dry */
                CHECK(dry_direct > 1e-3, "the direct impulse renders");
                CHECK(dry_late < dry_direct * 0.02, "no reflections without the opt-in");

                rt_source_set_ism(ci, hi, true);
                rt_commit(ci);
                ISM_CAPTURE();                                   /* wet: direct + the six wall images */
                double wet_direct, wet_late, wet_px, wet_nx;
                ISM_SUM(IMP_AT - 10, IMP_AT + 50, wet_direct, env);
                ISM_SUM(IMP_AT + 120, KB * (int)N, wet_late, env);
                CHECK(fabs(wet_direct - dry_direct) / dry_direct < 0.05, "the direct sound is unchanged");
                CHECK(wet_late > dry_direct * 0.05, "reflections arrive AFTER the direct sound");
                /* Every arrival is pinned by geometry (source (2.5,1.5,0), listener (0,1.5,0), room
                 * 6x3x6 -> x,z in +-3, y in [0,3]):
                 *   +x wall  image (3.5, 1.5, 0)     -> 3.50 m -> 490 samples
                 *   floor    image (2.5,-1.5, 0)     -> 3.91 m -> 546  (coincident with the ceiling,
                 *   ceiling  image (2.5, 4.5, 0)     -> 3.91 m -> 546   so this PAIR is the largest peak)
                 *   +-z wall images (2.5, 1.5, +-6)  -> 6.50 m -> 909
                 * The peak past the direct sound is therefore the floor/ceiling pair at ~546. */
                int pk = IMP_AT + 120; double pkv = 0;          /* search PAST the direct arrival */
                for (int i = IMP_AT + 120; i < KB * (int)N; ++i) if (env[i] > pkv) { pkv = env[i]; pk = i; }
                printf("ism: strongest reflection %d samples after the direct (floor+ceiling pair: 546)\n", pk - IMP_AT);
                CHECK(pk - IMP_AT > 500 && pk - IMP_AT < 590, "reflections land at their geometric path delays");
                /* the near +x wall's own reflection (~490) must come from the +x side: it is a point
                 * source at the mirrored position, not a diffuse bed */
                ISM_SUM(IMP_AT + 460, IMP_AT + 520, wet_px, envp);
                ISM_SUM(IMP_AT + 460, IMP_AT + 520, wet_nx, envn);
                CHECK(wet_px > wet_nx * 1.2, "the +x wall's reflection arrives from the +x side");

                rt_source_set_ism(ci, hi, false);                /* opt out: the reflections ramp away */
                rt_commit(ci);
                ISM_CAPTURE();
                double off_direct, off_late;
                ISM_SUM(IMP_AT - 10, IMP_AT + 50, off_direct, env);
                ISM_SUM(IMP_AT + 120, KB * (int)N, off_late, env);
                CHECK(fabs(off_direct - dry_direct) / dry_direct < 0.05, "direct sound survives the opt-out");
                CHECK(off_late < dry_direct * 0.02, "opting out silences the reflections");
                #undef ISM_SUM
                #undef ISM_CAPTURE
                rt_source_destroy(ci, hi); rt_commit(ci);
                remove(IW);
            } else CHECK(0, "write impulse wav (ism)");
            rt_destroy(ci);
        }
    }

    /* propagation effects (opt-in per voice): air absorption (distance low-pass) + Doppler (glided delay) */
    {
        RtCore* cp = rt_create(8, 4, RATE, CH);
        CHECK(cp != NULL, "rt_create (propagation)");
        if (cp) {
            /* air absorption: at a far distance, enabling it dulls an 8 kHz tone (same position, so
             * the panning + distance attenuation are identical — the energy drop is purely the LPF). */
            const char* SW8 = "bwa_rt_sine8k.wav";
            if (write_sine_wav(SW8, 8000.0, 8 * N)) {
                uint32_t s8 = rt_load_sound(cp, SW8, err, sizeof err);
                uint32_t hv = rt_source_create(cp);
                rt_source_set_pos(cp, hv, 24.f, 0.f, 0.f);          /* far: air cutoff well below 8 kHz */
                rt_source_play(cp, hv, s8, true);
                rt_commit(cp); render2(cp); render2(cp);
                double e_off = total_energy();
                rt_source_set_air_absorption(cp, hv, true);
                rt_commit(cp); render2(cp); render2(cp);            /* settle the ramped coeff */
                double e_on = total_energy();
                CHECK(e_off > 0.0 && e_on < 0.6 * e_off, "air absorption dulls an 8 kHz tone at distance");
                rt_source_destroy(cp, hv); rt_commit(cp);
                remove(SW8);
            } else CHECK(0, "write 8k sine wav");

            /* Doppler delay: a static source's signal arrives delayed by distance/c. At 3.43 m that's
             * 480 samples (343 m/s, 48 kHz); an impulse peaks there with Doppler on, at ~0 with it off. */
            const char* IW = "bwa_rt_impulse.wav";
            if (write_impulse_wav(IW, 16 * N)) {
                uint32_t si = rt_load_sound(cp, IW, err, sizeof err);
                float cap[4 * N];
                uint32_t hoff = rt_source_create(cp);
                rt_source_set_pos(cp, hoff, 3.43f, LD.ref[1], 0.f);   /* ear plane: distance == 3.43 m */
                rt_source_play(cp, hoff, si, false);
                rt_commit(cp);
                render_capture_mono(cp, cap, 4);
                int peak_off = argmax_abs(cap, 4 * N);
                rt_source_destroy(cp, hoff); rt_commit(cp);

                uint32_t hon = rt_source_create(cp);
                rt_source_set_pos(cp, hon, 3.43f, LD.ref[1], 0.f);
                rt_source_set_doppler(cp, hon, true);
                rt_source_play(cp, hon, si, false);
                rt_commit(cp);
                render_capture_mono(cp, cap, 4);
                int peak_on = argmax_abs(cap, 4 * N);
                CHECK(peak_off < 4, "no Doppler -> impulse arrives immediately");
                CHECK(peak_on >= 476 && peak_on <= 484, "Doppler -> impulse delayed by distance/c (~480 samples)");
                rt_source_destroy(cp, hon); rt_commit(cp);
                remove(IW);
            } else CHECK(0, "write impulse wav");

            /* Doppler pitch: an approaching source is pitched up vs a static one (more zero crossings).
             * Both start at 4 m with Doppler on (same initial propagation fill); the moving one glides
             * in to 0.5 m, so its read pointer outruns its write -> the 1 kHz tone resamples higher. */
            const char* SW1 = "bwa_rt_sine1k.wav";
            if (write_sine_wav(SW1, 1000.0, 128 * N)) {
                /* The delay smoother is heavy (low cutoff, for a clean spectrum), so warm up past its
                 * group delay + the ring fill, then measure the STEADY-STATE pitch over the tail. */
                enum { WARM = 16, KB = 24, TOT = WARM + KB };
                uint32_t s1 = rt_load_sound(cp, SW1, err, sizeof err);
                float cap[KB * N];
                bwa_timestamp ts = { 0, 0 };

                uint32_t hst = rt_source_create(cp);                  /* static reference at 4 m */
                rt_source_set_pos(cp, hst, 4.f, 0.f, 0.f);
                rt_source_set_doppler(cp, hst, true);
                rt_source_play(cp, hst, s1, true); rt_commit(cp);
                for (int b = 0; b < TOT; ++b) {
                    rt_render(cp, bus, N, &ts);
                    if (b >= WARM) for (uint32_t i = 0; i < N; ++i) {
                        double s = 0; for (int ch = 0; ch < CH; ++ch) s += bus[(size_t)ch*N + i]; cap[(b-WARM)*N + i] = (float)s; }
                }
                int zc_static = count_zc(cap, KB * N);
                rt_source_destroy(cp, hst); rt_commit(cp);

                uint32_t hmv = rt_source_create(cp);                  /* constant approach 7.5 m -> 0.5 m */
                rt_source_set_doppler(cp, hmv, true);
                rt_source_play(cp, hmv, s1, true);
                float md = 7.5f; const float mstep = (7.5f - 0.5f) / (TOT - 1);
                for (int b = 0; b < TOT; ++b) {
                    rt_source_set_pos(cp, hmv, md, 0.f, 0.f); rt_commit(cp);
                    rt_render(cp, bus, N, &ts);
                    if (b >= WARM) for (uint32_t i = 0; i < N; ++i) {
                        double s = 0; for (int ch = 0; ch < CH; ++ch) s += bus[(size_t)ch*N + i]; cap[(b-WARM)*N + i] = (float)s; }
                    md -= mstep;
                }
                int zc_moving = count_zc(cap, KB * N);
                CHECK(zc_moving > zc_static + 8, "Doppler: an approaching source is pitched up");
                rt_source_destroy(cp, hmv); rt_commit(cp);
                remove(SW1);
            } else CHECK(0, "write 1k sine wav");
            rt_destroy(cp);
        }
    }

    /* distance->reverb send: the per-source wet send scales with the level, and (in distance mode) with
     * range. Measured via the aux-send energy the bus tap reports (the bed itself needs the SDK). */
    {
        RtCore* cr = rt_create(8, 4, RATE, CH);
        CHECK(cr != NULL, "rt_create (reverb send)");
        if (cr) {
            uint32_t rsnd = rt_load_sound(cr, WAV, err, sizeof err);
            rt_set_bus_tap(cr, test_tap, NULL);
            uint32_t hr = rt_source_create(cr);
            rt_source_play(cr, hr, rsnd, true);
            rt_source_set_pos(cr, hr, 2.f, 0.f, 0.f);
            rt_source_set_reflections(cr, hr, true);             /* full send, no distance scaling */
            g_aux_energy = 0; rt_commit(cr); render2(cr);
            double aux_full = g_aux_energy;
            rt_source_set_reflection_send(cr, hr, 0.5f);         /* halve the send level */
            g_aux_energy = 0; rt_commit(cr); render2(cr);
            double aux_half = g_aux_energy;
            CHECK(aux_full > 0 && fabs(aux_half - aux_full * 0.5) < aux_full * 0.05, "reflection_send scales the wet send");

            rt_source_set_reflection_send(cr, hr, 1.0f);
            rt_source_set_reflection_distance(cr, hr, true);     /* near = drier, far = wetter */
            rt_source_set_pos(cr, hr, 0.5f, 0.f, 0.f);           /* near (< 1 m -> floor send) */
            g_aux_energy = 0; rt_commit(cr); render2(cr);
            double aux_near = g_aux_energy;
            rt_source_set_pos(cr, hr, 8.f, 0.f, 0.f);            /* far (> 6 m -> full send) */
            g_aux_energy = 0; rt_commit(cr); render2(cr);
            double aux_far = g_aux_energy;
            CHECK(aux_near > 0 && aux_far > aux_near * 2.0, "distance->reverb send: far sends more than near");

            /* replay after disabling reflections must not bleed a stale send burst (refl_g_cur reset on play) */
            rt_source_set_reflection_distance(cr, hr, false);
            rt_source_set_pos(cr, hr, 2.f, 0.f, 0.f);
            rt_commit(cr); render2(cr);                          /* send ramps up to full */
            rt_source_stop(cr, hr);
            rt_source_set_reflections(cr, hr, false);            /* disable while stopped */
            rt_commit(cr);
            rt_source_play(cr, hr, rsnd, true);                  /* replay -> refl_g_cur reset to 0 */
            g_aux_energy = -1.0; rt_commit(cr);
            { bwa_timestamp ts = { 0, 0 }; rt_render(cr, bus, N, &ts); }   /* first block after replay */
            CHECK(g_aux_energy == 0.0, "replay after disabling reflections sends no stale burst");
            rt_source_destroy(cr, hr); rt_commit(cr);
            rt_destroy(cr);
        }
    }

    /* dual-band panning: low band amplitude-normalised (Sigma|g|=gain), high band power (Sigma g^2=gain^2) */
    {
        RtCore* cdb = rt_create(8, 4, RATE, CH);
        CHECK(cdb != NULL, "rt_create (dual-band)");
        if (cdb) {
            const char* LW = "bwa_rt_lo.wav";
            if (write_sine_wav(LW, 200.0, 8 * N)) {              /* a tone below the 700 Hz crossover */
                uint32_t sl = rt_load_sound(cdb, LW, err, sizeof err);
                uint32_t hd = rt_source_create(cdb);
                rt_source_play(cdb, hd, sl, true);
                rt_source_set_pos(cdb, hd, 1.0f, 0.0f, 1.0f);    /* off-speaker -> spreads across channels */
                rt_set_dual_band(cdb, 0); rt_commit(cdb); render2(cdb);
                double l2_off = total_l2(); int amax_off = argmax_channel();
                rt_set_dual_band(cdb, 1); rt_commit(cdb); render2(cdb);
                double l2_on = total_l2(); int amax_on = argmax_channel();
                /* amplitude-norm gains have lower L2 than power-norm for a spread source (the LF relies on
                 * coherent summation at the listener), and the panning DIRECTION is unchanged. */
                CHECK(l2_off > 0 && l2_on < 0.85 * l2_off, "dual-band LF uses amplitude norm (lower per-channel power)");
                CHECK(amax_on == amax_off, "dual-band preserves the localization direction");
                /* the A/B toggle CROSSFADES (invariant 4): the first block after enabling lands BETWEEN
                 * single and dual, not straight at dual — a hard switch would step the LF re-weight in
                 * one sample. (dual L2 < single L2, so a gradual transition sits above the settled dual.) */
                {
                    bwa_timestamp tsd = { 0, 0 };
                    rt_set_dual_band(cdb, 0); rt_commit(cdb); render2(cdb);        /* settle back to single */
                    rt_set_dual_band(cdb, 1); rt_commit(cdb);
                    rt_render(cdb, bus, N, &tsd);  double l2_trans = total_l2();   /* one block: crossfade in progress */
                    CHECK(l2_trans > l2_on * 1.02 && l2_trans <= l2_off * 1.02,
                          "dual-band toggle crossfades (transition block between dual and single, not a jump)");
                }
                /* a HIGH tone (above the crossover) is unaffected by dual-band: it stays in the power band */
                const char* HW = "bwa_rt_hi.wav";
                if (write_sine_wav(HW, 5000.0, 8 * N)) {
                    uint32_t sh = rt_load_sound(cdb, HW, err, sizeof err);
                    uint32_t hh = rt_source_create(cdb);
                    rt_source_play(cdb, hh, sh, true);
                    rt_source_set_pos(cdb, hh, 1.0f, 0.0f, 1.0f);
                    rt_set_dual_band(cdb, 0); rt_commit(cdb); render2(cdb); double h_off = total_l2();
                    rt_set_dual_band(cdb, 1); rt_commit(cdb); render2(cdb); double h_on = total_l2();
                    double lf_chg = (l2_off - l2_on) / l2_off, hf_chg = h_off > 0 ? fabs(h_on - h_off) / h_off : 1;
                    CHECK(hf_chg < lf_chg, "dual-band changes the LF band more than the HF band");
                    rt_source_destroy(cdb, hh); rt_commit(cdb); remove(HW);
                } else CHECK(0, "write 5 kHz sine");
                /* the LF (amplitude) band must still attenuate with distance like the HF — the renorm
                 * targets ||g|| (which carries atten), not bare gain (which would cancel it). */
                rt_set_dual_band(cdb, 1);
                rt_source_set_pos(cdb, hd, 1.0f, 0.f, 0.f); rt_commit(cdb); render2(cdb); double lf_near = total_l2();
                rt_source_set_pos(cdb, hd, 4.0f, 0.f, 0.f); rt_commit(cdb); render2(cdb); double lf_far = total_l2();
                CHECK(lf_near > 0 && lf_far < 0.5 * lf_near, "dual-band LF attenuates with distance (atten preserved)");
                rt_source_destroy(cdb, hd); rt_commit(cdb); remove(LW);
            } else CHECK(0, "write 200 Hz sine");
            rt_destroy(cdb);
        }
    }

    /* voice priority + stealing: a create on a full pool stops the lowest-priority active source */
    {
        RtCore* cs = rt_create(4, 4, RATE, CH);
        CHECK(cs != NULL, "rt_create (steal)");
        if (cs) {
            uint32_t ss = rt_load_sound(cs, WAV, err, sizeof err);
            uint32_t hh[4];
            for (int i = 0; i < 4; ++i) {
                hh[i] = rt_source_create(cs);
                rt_source_play(cs, hh[i], ss, true);
                rt_source_set_priority(cs, hh[i], i == 2 ? 10 : 200);   /* hh[2] is the expendable one */
            }
            rt_commit(cs); render2(cs);
            CHECK(rt_source_is_playing(cs, hh[2]) && rt_source_is_playing(cs, hh[0]), "4 voices fill the pool");
            uint32_t h5 = rt_source_create(cs);                        /* pool full -> steal hh[2] (priority 10) */
            CHECK(h5 != 0, "create on a full pool succeeds by stealing");
            rt_source_play(cs, h5, ss, true);
            rt_commit(cs); render2(cs);
            CHECK(!rt_source_is_playing(cs, hh[2]), "the lowest-priority voice was stolen");
            CHECK(rt_source_is_playing(cs, hh[0]) && rt_source_is_playing(cs, h5), "higher-priority + new voices survive");
            rt_destroy(cs);
        }
    }

    /* voice-steal is CLICK-FREE: the stolen voice fades out over one block on its own slot (the new
     * source starts on a reserve slot), instead of a hard cut. Pool of 1 isolates it; the stealing
     * source is created but NOT played, so the steal block contains only the victim's fade. */
    {
        RtCore* cf = rt_create(1, 4, RATE, CH);
        if (cf) {
            bwa_timestamp ts0 = { 0, 0 };
            uint32_t sf = rt_load_sound(cf, WAV, err, sizeof err);   /* WAV = constant 1.0 */
            uint32_t a = rt_source_create(cf);
            rt_source_play(cf, a, sf, true);
            rt_source_set_priority(cf, a, 10);
            rt_commit(cf); render2(cf);
            double e_full = total_energy();
            CHECK(e_full > 1e-6, "steal fade: baseline voice audible");
            uint32_t b = rt_source_create(cf);                       /* pool full (1) -> steal a; leave b unplayed */
            CHECK(b != 0, "steal fade: create-on-full succeeds by stealing");
            rt_commit(cf);
            rt_render(cf, bus, N, &ts0);  double e_fade  = total_energy();   /* the steal block: a fades */
            rt_render(cf, bus, N, &ts0);  double e_after = total_energy();   /* next block: a gone, b silent */
            CHECK(e_fade > 0.05 * e_full, "steal fade: the stolen voice fades, not a hard cut to silence");
            CHECK(e_fade < e_full,        "steal fade: fading DOWN, not still full");
            CHECK(e_after < 1e-6,         "steal fade: silent the block after the fade completes");
            rt_destroy(cf);
        }
    }

    /* sample-accurate scheduled play: a voice is held silent until its start_sample, then fires */
    {
        RtCore* cp = rt_create(4, 4, RATE, CH);
        CHECK(cp != NULL, "rt_create (schedule)");
        if (cp) {
            uint32_t sp = rt_load_sound(cp, WAV, err, sizeof err);
            uint32_t h  = rt_source_create(cp);
            rt_source_set_pos(cp, h, 1.f, 0.f, 1.f);
            rt_source_play_at(cp, h, sp, true, (uint64_t)3 * N);    /* start at the 4th block */
            rt_commit(cp);
            double pre = 0; uint64_t pos = 0;
            for (int blk = 0; blk < 3; ++blk) {                    /* blocks spanning [0, 3N): held silent */
                bwa_timestamp ts = { pos, 0 };
                rt_render(cp, bus, N, &ts);
                pre += total_l2();
                pos += N;
            }
            CHECK(pre < 1e-6, "scheduled voice is silent before its start_sample");
            bwa_timestamp ts3 = { pos, 0 };                          /* pos == 3N: the voice fires here */
            rt_render(cp, bus, N, &ts3);
            CHECK(total_l2() > 1e-3, "scheduled voice fires at its start_sample");
            CHECK(rt_dsp_time(cp) == (uint64_t)3 * N, "rt_dsp_time tracks the device sample clock");
            rt_destroy(cp);
        }
    }

    /* streaming: a streamed sound feeds the mixer through the background ring (the standalone ring
     * mechanics are covered by stream_test; here we verify the rt integration produces audio). */
    {
        RtCore* cst = rt_create(4, 4, RATE, CH);
        CHECK(cst != NULL, "rt_create (stream)");
        if (cst) {
            uint32_t ss = rt_load_sound_streaming(cst, WAV, err, sizeof err);
            CHECK(ss != 0, err[0] ? err : "rt_load_sound_streaming");
            if (ss) {
                uint32_t h = rt_source_create(cst);
                rt_source_set_pos(cst, h, 1.f, 0.f, 1.f);
                rt_source_play(cst, h, ss, true);   /* loop a short file */
                rt_commit(cst);
                Sleep(60);                          /* let the streaming thread fill the ring */
                double e = 0;
                for (int blk = 0; blk < 30; ++blk) { render2(cst); e += total_l2(); }
                CHECK(e > 1e-3, "streamed voice produces audio through the mixer");
            }
            rt_destroy(cst);
        }
    }

    /* push (procedural) source: caller-pushed PCM plays through the full mix path; an underrun
     * renders silence WITHOUT ending the voice or losing the caller's place; push_end drains then
     * ends; the internal sound slot retires with the source handle (cycles don't exhaust tables). */
    {
        RtCore* cps = rt_create(4, 4, RATE, CH);
        CHECK(cps != NULL, "rt_create (push)");
        if (cps) {
            char perr[256] = {0};
            uint32_t h = rt_source_create_stream(cps, perr, sizeof perr);
            CHECK(h != 0, perr[0] ? perr : "rt_source_create_stream");
            CHECK(rt_source_is_push(cps, h), "push source reads as push");
            /* the play is still queued (no render yet) — a pending play must READ as playing, or the
             * documented create->push->push_end->poll->destroy flow drops the clip in the first-block
             * window (the poll sees false, the caller destroys early) */
            CHECK(rt_source_is_playing(cps, h), "push: pending play reads as playing before the first block");
            rt_source_set_pos(cps, h, 1.f, 0.f, 1.f);
            rt_commit(cps);
            render2(cps);                                       /* binds + consumes an EMPTY ring */
            CHECK(total_l2() < 1e-9, "push: silent before any data (underrun, not garbage)");
            CHECK(rt_source_is_playing(cps, h), "push: an empty ring does not end the voice");

            float pblk[2 * N];
            for (int i = 0; i < 2 * N; ++i) pblk[i] = 0.5f;
            uint32_t space = rt_source_push_space(cps, h);
            CHECK(space >= 2 * N, "push: space available");
            CHECK(rt_source_push(cps, h, pblk, 2 * N) == 2 * N, "push accepts two blocks");
            CHECK(rt_source_push_space(cps, h) == space - 2 * N, "push: space accounts for the pushed frames");
            render2(cps);                                       /* consumes both pushed blocks */
            CHECK(total_l2() > 1e-3, "pushed audio reaches the bus");
            bwa_timestamp pts = { 0, 0 };
            rt_render(cps, bus, N, &pts);
            CHECK(total_l2() < 1e-9, "underrun after the pushed data: silence again");
            CHECK(rt_source_is_playing(cps, h), "underrun does not end the voice");

            /* data-driven clock: audio pushed after an underrun still plays (nothing was skipped) */
            CHECK(rt_source_push(cps, h, pblk, N) == N, "push after an underrun");
            rt_source_push_end(cps, h);
            CHECK(rt_source_push(cps, h, pblk, N) == 0, "push after push_end is refused");
            CHECK(rt_source_push_space(cps, h) == 0, "space is 0 after push_end");
            rt_render(cps, bus, N, &pts);                       /* the tail drains this block */
            CHECK(total_l2() > 1e-3, "the tail pushed after the underrun still plays");
            rt_render(cps, bus, N, &pts);
            CHECK(!rt_source_is_playing(cps, h), "voice ends once the pushed data drains");

            /* rebinding a push source to a loaded asset is refused (the ring is the content) */
            uint32_t sq = rt_load_sound(cps, WAV, err, sizeof err);
            CHECK(sq != 0, "push: load asset for the rebind-refusal check");   /* sq==0 would pass vacuously */
            rt_source_play(cps, h, sq, true);
            render2(cps);
            CHECK(total_l2() < 1e-9, "rt_source_play on a push source is refused");

            /* handle death retires the internal sound: 8 create/destroy cycles through a 4-slot
             * sound table only pass if each destroy recycles its slot (retire-ack per cycle) */
            rt_source_destroy(cps, h);
            CHECK(rt_source_push(cps, h, pblk, N) == 0, "push on a destroyed handle is dropped");
            CHECK(!rt_source_is_playing(cps, h), "destroyed push handle reads not playing");
            render2(cps); rt_commit(cps);                       /* retire lands; the ack drains on commit */
            for (int k = 0; k < 8; ++k) {
                uint32_t hk = rt_source_create_stream(cps, perr, sizeof perr);
                CHECK(hk != 0, "push sound slots recycle across create/destroy cycles");
                if (!hk) break;
                rt_source_destroy(cps, hk);
                render2(cps); rt_commit(cps);
            }

            /* stop ENDS a push source one-way (like push_end): a stopped push voice cannot re-arm
             * (play is refused), so pushes must be refused too — not silently swallowed forever */
            uint32_t hst = rt_source_create_stream(cps, perr, sizeof perr);
            CHECK(hst != 0, "push: create for the stop test");
            if (hst) {
                CHECK(rt_source_push(cps, hst, pblk, N) == N, "push: feed before stop");
                rt_source_stop(cps, hst);
                CHECK(rt_source_push(cps, hst, pblk, N) == 0, "push after stop is refused (stop ends the stream)");
                CHECK(rt_source_push_space(cps, hst) == 0, "push: no space after stop");
                render2(cps);
                CHECK(!rt_source_is_playing(cps, hst), "stop finalizes the push voice");
                rt_source_destroy(cps, hst);
                render2(cps); rt_commit(cps);
            }
            rt_destroy(cps);
        }
    }

    /* steal reaps a DRAINED push source (playing=false after push_end, handle still held — every push
     * source's normal terminal state): the steal must finalize + ack immediately (there is no fade to
     * wait for), recycling the voice slot AND retiring the internal sound. Without that the control
     * side waits forever (stealing[] sticks) and the sound/stream slots leak. */
    {
        RtCore* cs = rt_create(4, 4, RATE, CH);
        CHECK(cs != NULL, "rt_create (steal-push)");
        if (cs) {
            char perr[256] = {0};
            float blk[N]; for (int i = 0; i < N; ++i) blk[i] = 0.25f;
            uint32_t hs[4] = {0};
            for (int i = 0; i < 4; ++i) {
                hs[i] = rt_source_create_stream(cs, perr, sizeof perr);
                CHECK(hs[i] != 0, "steal-push: fill the pool");
                if (!hs[i]) break;
                rt_source_push(cs, hs[i], blk, N);
                rt_source_push_end(cs, hs[i]);
            }
            render2(cs); render2(cs);                       /* bind, play the block, drain, end */
            for (int i = 0; i < 4; ++i) CHECK(!rt_source_is_playing(cs, hs[i]), "steal-push: victim drained");
            /* the user pool (4) is full of drained-but-held push sources: this create must steal one */
            uint32_t hn = rt_source_create(cs);
            CHECK(hn != 0, "steal-push: create steals a drained victim");
            render2(cs);                                    /* CMD_SRC_STEAL -> immediate EVT (victim silent) */
            rt_commit(cs);                                  /* ack: victim recycles, its internal sound retires */
            render2(cs); rt_commit(cs);                     /* retire-ack: the ring closes, the slot frees */
            int live = 0; for (int i = 0; i < 4; ++i) live += rt_source_is_push(cs, hs[i]) ? 1 : 0;
            CHECK(live == 3, "steal-push: exactly one drained victim recycled");
            /* the victim's sound slot must be free again: a new push source fits the 4-slot table
             * (it steals another drained victim for the VOICE and needs the freed SOUND slot) */
            uint32_t hp2 = rt_source_create_stream(cs, perr, sizeof perr);
            CHECK(hp2 != 0, "steal-push: the steal retired the internal sound (slot reusable)");
            rt_destroy(cs);
        }
    }

    /* steal of a PLAYING push source rides the fade path: stopping=2 -> fade -> EVT_VOICE_ENDED ->
     * push_sound_release. Same contract as the drained case, different audio-side route. */
    {
        RtCore* cp = rt_create(4, 4, RATE, CH);
        CHECK(cp != NULL, "rt_create (steal-playing-push)");
        if (cp) {
            char perr[256] = {0};
            float blk[N]; for (int i = 0; i < N; ++i) blk[i] = 0.25f;
            uint32_t hs[4] = {0};
            for (int i = 0; i < 4; ++i) {
                hs[i] = rt_source_create_stream(cp, perr, sizeof perr);
                CHECK(hs[i] != 0, "steal-playing: fill the pool");
                if (!hs[i]) break;
                for (int k = 0; k < 16; ++k) rt_source_push(cp, hs[i], blk, N);   /* deep buffer: stays playing */
            }
            render2(cp);                                    /* bind + consume; everything still playing */
            for (int i = 0; i < 4; ++i) CHECK(rt_source_is_playing(cp, hs[i]), "steal-playing: victims live");
            uint32_t hn = rt_source_create(cp);             /* full pool: steals a PLAYING push source */
            CHECK(hn != 0, "steal-playing: create steals");
            render2(cp);                                    /* block 1 fades the victim, block 2 finalizes + EVT */
            rt_commit(cp);                                  /* ack: recycle + internal sound retires */
            render2(cp); rt_commit(cp);                     /* retire-ack: ring closes, slot frees */
            int live = 0; for (int i = 0; i < 4; ++i) live += rt_source_is_push(cp, hs[i]) ? 1 : 0;
            CHECK(live == 3, "steal-playing: exactly one victim recycled through the fade");
            uint32_t hp3 = rt_source_create_stream(cp, perr, sizeof perr);
            CHECK(hp3 != 0, "steal-playing: the faded steal retired the internal sound");
            rt_destroy(cp);
        }
    }

    /* a push-source death whose internal CMD_SOUND_RETIRE hits a FULL command ring must park the
     * retire and re-try at drain_events — not drop it (the handle is internal; nobody else can retry,
     * so a drop leaks the sound slot + stream ring for the engine's lifetime). The pad sweep walks the
     * ring fill across the boundary (RING_CAP = 4096 in rt.c; create_stream = 2 cmds, destroy = 1, the
     * retire is the +1 that lands on the full ring at one pad in the sweep). */
    {
        RtCore* cf = rt_create(4, 4, RATE, CH);
        CHECK(cf != NULL, "rt_create (parked retire)");
        if (cf) {
            char perr[256] = {0};
            for (int pad = 4090; pad <= 4098; ++pad) {
                uint32_t hp = rt_source_create_stream(cf, perr, sizeof perr);
                CHECK(hp != 0, "parked retire: create_stream");
                if (!hp) break;
                for (int i = 0; i < pad; ++i) rt_source_set_pos(cf, hp, 0.f, 0.f, 1.f);
                rt_source_destroy(cf, hp);                  /* one pad lands the retire on a full ring */
                render2(cf); rt_commit(cf);                 /* drain; a parked retire re-enqueues here */
                render2(cf); rt_commit(cf);                 /* retire-ack: ring closes, sound slot frees */
                if (rt_source_is_push(cf, hp)) {            /* ring was dead-full: the DESTROY itself was
                                                             * dropped (documented no-op) — retry it */
                    rt_source_destroy(cf, hp);
                    render2(cf); rt_commit(cf); render2(cf); rt_commit(cf);
                }
            }
            /* nothing leaked across the sweep: all 4 sound slots must be allocatable AT ONCE */
            uint32_t hk[4] = {0};
            for (int k = 0; k < 4; ++k) {
                hk[k] = rt_source_create_stream(cf, perr, sizeof perr);
                CHECK(hk[k] != 0, "parked retire: no sound/stream slot leaked across the sweep");
            }
            for (int k = 0; k < 4; ++k) if (hk[k]) rt_source_destroy(cf, hk[k]);
            render2(cf); rt_commit(cf); render2(cf); rt_commit(cf);
            rt_destroy(cf);
        }
    }

    /* pause/resume + seek: the gate ramps to silence, the playhead freezes, seeks land click-free */
    {
        RtCore* cq = rt_create(4, 4, RATE, CH);
        CHECK(cq != NULL, "rt_create (pause/seek)");
        if (cq && write_const_wav("bwa_rt_seek.wav", 0.8f, 5 * N)) {   /* finite, non-loop: 5 blocks of content */
            uint32_t sq = rt_load_sound(cq, "bwa_rt_seek.wav", err, sizeof err);
            uint32_t h  = rt_source_create(cq);
            rt_source_set_pos(cq, h, 1.f, 0.f, 1.f);
            bwa_timestamp ts = { 0, 0 };
            rt_source_play(cq, h, sq, false);
            rt_commit(cq);
            rt_render(cq, bus, N, &ts);                    /* block 1 of 5 plays */
            CHECK(total_l2() > 1e-3, "voice audible before pause");
            rt_source_set_paused(cq, h, true);
            rt_render(cq, bus, N, &ts);                    /* ramp-out block (consumes block 2) */
            rt_render(cq, bus, N, &ts);
            CHECK(total_l2() < 1e-9, "paused voice is silent");
            CHECK(rt_source_is_playing(cq, h), "a paused voice still reads as playing");
            for (int b = 0; b < 10; ++b) rt_render(cq, bus, N, &ts);   /* 10N frames >> the 3N remaining */
            rt_source_set_paused(cq, h, false);
            rt_render(cq, bus, N, &ts);                    /* ramp back in: block 3 of 5 */
            CHECK(total_l2() > 1e-3, "resume continues from the frozen position (nothing consumed while paused)");
            rt_source_seek(cq, h, (uint64_t)4 * N);        /* jump to the last block of content */
            rt_render(cq, bus, N, &ts);                    /* ramp-out */
            rt_render(cq, bus, N, &ts);                    /* seek lands: plays [4N, 5N) ramping in */
            CHECK(total_l2() > 1e-3, "seek lands and plays the target region");
            rt_render(cq, bus, N, &ts);                    /* past the end: the non-loop voice ends */
            CHECK(total_l2() < 1e-9, "silence after the seeked tail");
            CHECK(!rt_source_is_playing(cq, h), "seeking near the end ends the non-loop voice on time");
            rt_destroy(cq);
            remove("bwa_rt_seek.wav");
        } else if (cq) { CHECK(0, "write seek wav"); rt_destroy(cq); }
    }

    /* output protection limiter: a linked gain caps the peak without shifting inter-channel balance.
     * The test signal is injected after align, so it hits the limiter (the final stage) directly. */
    {
        RtCore* cl = rt_create(4, 4, RATE, CH);
        CHECK(cl != NULL, "rt_create (limiter)");
        if (cl) {
            bwa_timestamp ts = { 0, 0 };
            rt_test_signal(cl, 0, 1, 2.0f);                /* sine, peak 2.0 — over the -1 dBFS ceiling */
            rt_test_signal(cl, 1, 1, 0.5f);                /* the same waveform at 1/4 the level */
            for (int b = 0; b < 20; ++b) rt_render(cl, bus, N, &ts);   /* settle the envelope */
            rt_render(cl, bus, N, &ts);
            float p0 = 0, p1 = 0;
            for (int i = 0; i < N; ++i) {
                float a0 = fabsf(bus[0 * N + i]), a1 = fabsf(bus[1 * N + i]);
                if (a0 > p0) p0 = a0;
                if (a1 > p1) p1 = a1;
            }
            CHECK(p0 <= 0.8915f && p0 > 0.80f, "limiter holds the hot channel at the -1 dBFS ceiling");
            CHECK(fabsf(p1 / p0 - 0.25f) < 0.02f, "linked limiting preserves inter-channel balance");
            /* the output meter publishes exactly this block's post-limiter per-channel peaks */
            float mtr[CH] = { 0 };
            CHECK(rt_bus_peaks(cl, mtr, CH) == CH, "bus meter reports the channel count");
            CHECK(fabsf(mtr[0] - p0) < 1e-6f && fabsf(mtr[1] - p1) < 1e-6f,
                  "bus meter matches the rendered block's peaks");
            CHECK(mtr[2] == 0.f, "a silent channel meters zero");
            rt_set_limiter_ceiling(cl, 0.5f);
            for (int b = 0; b < 20; ++b) rt_render(cl, bus, N, &ts);
            rt_render(cl, bus, N, &ts);
            p0 = 0; for (int i = 0; i < N; ++i) { float a = fabsf(bus[0 * N + i]); if (a > p0) p0 = a; }
            CHECK(p0 <= 0.5001f, "limiter ceiling is adjustable");
            rt_set_limiter(cl, 0);
            rt_render(cl, bus, N, &ts); rt_render(cl, bus, N, &ts);
            p0 = 0; for (int i = 0; i < N; ++i) { float a = fabsf(bus[0 * N + i]); if (a > p0) p0 = a; }
            CHECK(p0 > 1.5f, "limiter off passes the raw signal");
            rt_destroy(cl);
        }
    }

    remove(WAV);
    if (fails) { printf("rt_test: %d FAILURES\n", fails); return 1; }
    printf("rt_test OK (DBAP, commit, gen-drop, gain, occlusion, EQ, directivity, bed, reflection-tap, channel-test, air+Doppler, spread+MDAP+size, tracked-room-EQ, decorrelation, parametric-bed, pose-pred, near-spread, loudness-comp, multi-listener, master+groups+fades+global-pause, pitch, bed-rotation, reverb-send, dual-band, voice-steal, scheduled-play, streaming, pause/seek, limiter, bus-meter verified)\n");
    return 0;
}
