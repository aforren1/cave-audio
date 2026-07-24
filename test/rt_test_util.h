/*
 * rt_test_util.h — shared harness for the rt_core / rt_feature test split.
 *
 * The rt test was one ~2.3k-line monolith; it now lives in two executables
 * (test/rt_core_test.c = the concurrency/lifecycle spine, test/rt_feature_test.c =
 * the spatial-feature DSP toggles), each a standalone main(). Everything they share —
 * the default layout + bus scratch, the energy/channel probes, the render harness, the
 * wav writers, the CHECK macro + fail counter, and the bus/path tap stubs — is defined
 * here, static, so each translation unit gets its own copy (the two exes never link
 * together). Moved verbatim from the original rt_test.c; do not edit behaviour here.
 */
#ifndef RT_TEST_UTIL_H
#define RT_TEST_UTIL_H

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
/* same 4-ch AmbiX plane-wave encode, but a SINE — for the band-split max-rE test, which needs
 * content isolated to one side of the 700 Hz crossover (integer cycles per file: loops seamlessly) */
static int write_ambix4_sine_wav(const char* path, float w, float y, float z, float x,
                                 double freq, uint32_t frames) {
    drwav_data_format fmt = { drwav_container_riff, DR_WAVE_FORMAT_IEEE_FLOAT, 4, RATE, 32 };
    drwav wav;
    if (!drwav_init_file_write(&wav, path, &fmt, NULL)) return 0;
    float* buf = (float*)malloc((size_t)frames * 4 * sizeof(float));
    if (!buf) { drwav_uninit(&wav); return 0; }
    for (uint32_t i = 0; i < frames; ++i) {
        float v = 0.5f * (float)sin(2.0 * 3.14159265358979 * freq * i / RATE);
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

#endif /* RT_TEST_UTIL_H */
