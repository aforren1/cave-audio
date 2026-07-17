/*
 * bench_situations.c — per-situation audio-thread cost, headless and deterministic.
 *
 * Where profile_bench.c drives ONE representative load in real time for the Tracy GUI, this walks a
 * MATRIX of situations (idle, DBAP scaling, binaural HRTF, occlusion, spread/decorrelation,
 * propagation, FDN, Steam reflection bed, pathing, everything-on) and TIMES each directly: it renders
 * each configuration through the manual sink (bwa_render_block, synchronous, no device thread) and
 * measures the wall-clock per-block cost. Prints one row per situation: mean / median / p99 / max in
 * microseconds and as a percentage of the block budget (block_size / sample_rate).
 *
 * This measures the AUDIO-THREAD budget (the "are we going to glitch" question). The Steam sims
 * (occlusion/reflection/pathing ray-tracing) run on their own low-priority background threads and are
 * NOT in these numbers by design — only the audio-thread taps they feed (reflect/path decode) are.
 * Build with -DBWA_TRACY=ON to also explore the per-zone breakdown in the Tracy GUI; the Tracy client
 * is on-demand, so it's dormant (near-zero cost) while these numbers are taken.
 *
 * Usage: bwa_bench_situations [blocks=4000]
 */
#include "bw_audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* per-zone profiler readout, exported from bw_audio when built -DBWA_PROFILE_SELF (see profile_self.h).
 * reset returns 1 if that build flag is on (0 = the accumulation is compiled out). */
__declspec(dllimport) int bwa_prof_reset(void);
__declspec(dllimport) int bwa_prof_report(void);

#define SR   48000u
#define BLK  256u
#define WAVF "bwa_bench_sit.wav"
static int g_zones;   /* "zones" arg: dump the per-zone breakdown for each situation */

static double g_qpc_hz;
static double now_us(void) { LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart * 1.0e6 / g_qpc_hz; }

/* minimal mono 32-bit-float WAV (mirrors profile_bench; avoids pulling in a writer) */
static int write_sine_wav(const char* path, double freq, uint32_t sr, uint32_t frames) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    uint32_t data = frames * 4, riff = 36 + data, byterate = sr * 4, sz = 16;
    uint16_t fmt = 3, ch = 1, ba = 4, bits = 32;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); fwrite(&sz, 4, 1, f);
    fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f);
    fwrite(&byterate, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    for (uint32_t i = 0; i < frames; ++i) { float s = 0.5f * (float)sin(2.0 * 3.14159265358979 * freq * i / sr); fwrite(&s, 4, 1, f); }
    fclose(f);
    return 1;
}

typedef struct {
    const char* name;
    int binaural;                       /* profile: binaural (2-ch HRTF monitor) vs cave (26-ch) */
    int nvoices;
    int doppler, air, spread, decorr;   /* per-voice propagation + width (phonon-free) */
    int occl_manual;                    /* manual occlusion EQ (3 biquads/voice; deterministic, no sim) */
    int reflections, pathing;           /* Steam per-source sends (need the SDK + geometry + settle) */
    int fdn, reflect_bed;               /* load-time reverb bed (FDN = no-SDK, reflect_bed = Steam) */
    int settle_ms;                      /* let the async sims publish before timing */
} Sit;

typedef struct { double mean, median, p99, max; int nch; } Stats;
static int cmp_d(const void* a, const void* b) { double x = *(const double*)a, y = *(const double*)b; return (x < y) ? -1 : (x > y) ? 1 : 0; }

static int run_sit(const Sit* s, bwa_sound snd_unused, Stats* out, int K) {
    (void)snd_unused;
    bwa_desc cfg; memset(&cfg, 0, sizeof cfg);
    cfg.sink = BWA_SINK_MANUAL; cfg.sample_rate = SR; cfg.block_size = BLK;
    cfg.profile = s->binaural ? BWA_PROFILE_BINAURAL : BWA_PROFILE_CAVE;
    cfg.enable_pathing = s->pathing ? true : false;
    bwa_engine* e = bwa_create(&cfg);
    if (!e) { printf("  %-22s bwa_create FAILED\n", s->name); return -1; }

    if (s->reflect_bed) {
        bwa_reflections_desc rc; memset(&rc, 0, sizeof rc);
        rc.enabled = 1; rc.ir_seconds = 0.8f; rc.order = 1; rc.num_rays = 2048; rc.num_bounces = 8;
        bwa_reflections_config(e, &rc);
    }
    if (s->fdn) { bwa_fdn_desc fc; memset(&fc, 0, sizeof fc); fc.enabled = 1; bwa_fdn_config(e, &fc); }
    if (s->reflect_bed || s->pathing) {                    /* the ray-traced beds need geometry to bounce/route */
        bwa_material faces[6] = { 0, 0, 0, 0, 0, 0 };
        bwa_scene_set_box(e, 8.0f, 4.0f, 8.0f, faces);
    }
    if (bwa_start(e) != 0) { printf("  %-22s bwa_start: %s\n", s->name, bwa_last_error(e)); bwa_destroy(e); return -1; }

    bwa_sound snd = bwa_load_sound(e, WAVF);
    if (!snd) { printf("  %-22s load: %s\n", s->name, bwa_last_error(e)); bwa_stop(e); bwa_destroy(e); return -1; }

    if (s->decorr) bwa_set_decorrelation(e, true);
    bwa_set_listener_pose(e, 0.0f, 1.5f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    for (int i = 0; i < s->nvoices; ++i) {
        bwa_source v = bwa_source_create(e);
        bwa_source_play(e, v, snd, true);                  /* looped: steady mix load every block */
        bwa_source_set_gain(e, v, 0.1f);
        float a = (float)i * (6.2831853f / (float)(s->nvoices > 0 ? s->nvoices : 1));
        bwa_source_set_pos(e, v, 2.5f * cosf(a), 1.5f, 2.5f * sinf(a));
        if (s->doppler)     bwa_source_set_doppler(e, v, true);
        if (s->air)         bwa_source_set_air_absorption(e, v, true);
        if (s->spread)      bwa_source_set_spread(e, v, 0.6f);
        if (s->occl_manual) { float bands[3] = { 0.5f, 0.3f, 0.12f }; bwa_source_set_occlusion_manual(e, v, 0.35f, bands); }
        if (s->reflections || s->fdn || s->reflect_bed) bwa_source_set_reverb(e, v, true);   /* feed the aux send */
        if (s->pathing)     bwa_source_set_pathing(e, v, true);
    }
    bwa_commit(e);

    if (s->settle_ms) Sleep((DWORD)s->settle_ms);          /* let occlusion/reflection/pathing sims publish */
    for (int b = 0; b < 128; ++b) bwa_render_block(e, NULL, NULL);   /* warm up (gain ramps, caches) */

    if (g_zones) bwa_prof_reset();                         /* start the per-zone tally over the timed window */
    double* t = (double*)malloc((size_t)K * sizeof(double));
    if (!t) { bwa_stop(e); bwa_destroy(e); return -1; }
    uint32_t nch = 0;
    for (int b = 0; b < K; ++b) {
        uint32_t c = 0, n = 0;
        double t0 = now_us();
        const float* out = bwa_render_block(e, &c, &n);
        double dt = now_us() - t0;
        if (!out) { free(t); bwa_stop(e); bwa_destroy(e); return -1; }
        nch = c; t[b] = dt;
    }
    qsort(t, K, sizeof(double), cmp_d);
    double sum = 0; for (int i = 0; i < K; ++i) sum += t[i];
    out->mean = sum / K; out->median = t[K / 2]; out->p99 = t[(int)(K * 0.99)]; out->max = t[K - 1]; out->nch = (int)nch;
    free(t);
    bwa_stop(e); bwa_destroy(e);
    return 0;
}

int main(int argc, char** argv) {
    int K = 4000;
    for (int i = 1; i < argc; ++i) {                       /* args: a block count, and/or "zones" */
        if (strcmp(argv[i], "zones") == 0) g_zones = 1;
        else { int v = atoi(argv[i]); if (v > 0) K = v; }
    }
    if (K < 500) K = 500;
    if (g_zones && !bwa_prof_reset()) {                    /* probe: is the dll a -DBWA_PROFILE_SELF build? */
        printf("note: 'zones' needs a dll built with -DBWA_PROFILE_SELF; showing totals only.\n");
        g_zones = 0;
    }
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpc_hz = (double)f.QuadPart;
    if (!write_sine_wav(WAVF, 220.0, SR, SR)) { printf("wav write failed\n"); return 1; }

    const double budget_us = 1.0e6 * (double)BLK / (double)SR;   /* 5333.3 us at 256/48k */

    static const Sit sits[] = {
        /* name                     bin  nv  dop air spr dec  occ  refl path fdn bed  settle */
        { "idle (cave)",              0,   0,  0,  0,  0,  0,   0,   0,   0,  0,  0,   0 },
        { "1 voice DBAP",             0,   1,  0,  0,  0,  0,   0,   0,   0,  0,  0,   0 },
        { "8 voices DBAP",            0,   8,  0,  0,  0,  0,   0,   0,   0,  0,  0,   0 },
        { "26 voices DBAP",           0,  26,  0,  0,  0,  0,   0,   0,   0,  0,  0,   0 },
        { "8v binaural (HRTF)",       1,   8,  0,  0,  0,  0,   0,   0,   0,  0,  0,   0 },
        { "8v occlusion EQ",          0,   8,  0,  0,  0,  0,   1,   0,   0,  0,  0,   0 },
        { "8v Doppler+air",           0,   8,  1,  1,  0,  0,   0,   0,   0,  0,  0,   0 },
        { "8v spread (LOBE)",         0,   8,  0,  0,  1,  0,   0,   0,   0,  0,  0,   0 },
        { "8v spread+decorrelate",    0,   8,  0,  0,  1,  1,   0,   0,   0,  0,  0,   0 },
        { "8v FDN reverb",            0,   8,  0,  0,  0,  0,   0,   0,   0,  1,  0,   0 },
        { "8v Steam reflect bed",     0,   8,  0,  0,  0,  0,   0,   1,   0,  0,  1, 500 },
        { "8v Steam pathing",         0,   8,  0,  0,  0,  0,   0,   0,   1,  0,  0, 500 },
        { "16v all-on (no-SDK)",      0,  16,  1,  1,  1,  1,   1,   0,   0,  1,  0,   0 },
    };
    const int N = (int)(sizeof sits / sizeof sits[0]);

    printf("bw_audio per-situation bench  |  %u/%u = %.2f ms block budget  |  %d blocks/situation\n",
           BLK, SR, budget_us / 1000.0, K);
    printf("%-24s %4s  %9s %9s %9s %9s   %6s\n", "situation", "ch", "mean us", "median", "p99", "max", "%budget");
    printf("--------------------------------------------------------------------------------------\n");
    for (int i = 0; i < N; ++i) {
        Stats st; memset(&st, 0, sizeof st);
        if (run_sit(&sits[i], 0, &st, K) != 0) continue;
        printf("%-24s %4d  %9.1f %9.1f %9.1f %9.1f   %5.1f%%\n",
               sits[i].name, st.nch, st.mean, st.median, st.p99, st.max, 100.0 * st.mean / budget_us);
        if (g_zones) { bwa_prof_report(); printf("\n"); }   /* per-zone breakdown for this situation */
    }
    printf("--------------------------------------------------------------------------------------\n");
    printf("note: audio-thread cost only; Steam occlusion/reflection/pathing RAY-TRACING runs on\n"
           "      background sim threads (30/12/10 Hz, below-normal priority) and is not counted here.\n");
    remove(WAVF);
    return 0;
}
