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
 * Usage: bwa_bench_situations [blocks=4000] [zones] [layout=<path>] [only=<substring>]
 *   layout=  a layout JSON for the "rig" rows (24 speakers + a `directivity` block exercises the
 *            tracked directivity comp and the array-sim room the default grid skips). Without it
 *            those rows are skipped.
 *   only=    run only the situations whose name contains the substring (A/B one row quickly).
 */
#include "bw_audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "portable.h"      /* bwa_sleep_ms / bwa_now_us: the demo's only OS calls */

/* per-zone profiler readout, exported from bw_audio when built -DBWA_PROFILE_SELF (see profile_self.h).
 * reset returns 1 if that build flag is on (0 = the accumulation is compiled out). */
__declspec(dllimport) int bwa_prof_reset(void);
__declspec(dllimport) int bwa_prof_report(void);

#define SR   48000u
#define BLK  256u
#define WAVF "bwa_bench_sit.wav"
#define BEDF "bwa_bench_bed.wav"
static int g_zones;   /* "zones" arg: dump the per-zone breakdown for each situation */
static const char* g_layout;   /* "layout=" arg: the rig rows' layout (NULL = skip them) */
static const char* g_only;     /* "only=" arg: name filter */


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

/* 16-channel (3rd-order AmbiX) float WAV for the bed rows: quieter higher orders, so every channel
 * the bed rotate + decode touches carries signal. */
static int write_bed_wav(const char* path, uint32_t sr, uint32_t frames) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    const uint16_t ch = 16;
    uint32_t data = frames * 4u * ch, riff = 36 + data, byterate = sr * 4u * ch, sz = 16;
    uint16_t fmt = 3, ba = (uint16_t)(4 * ch), bits = 32;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); fwrite(&sz, 4, 1, f);
    fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f);
    fwrite(&byterate, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    for (uint32_t i = 0; i < frames; ++i)
        for (uint16_t k = 0; k < ch; ++k) {
            float s = (0.4f / (float)(1 + k)) * (float)sin(2.0 * 3.14159265358979 * (110.0 + 37.0 * k) * i / sr);
            fwrite(&s, 4, 1, f);
        }
    fclose(f);
    return 1;
}

typedef struct {
    const char* name;
    int binaural;                       /* profile: binaural (direct HRTF render) vs cave */
    int nvoices;
    int doppler, air, spread, decorr;   /* per-voice propagation + width (phonon-free) */
    int occl_manual;                    /* manual occlusion EQ (3 biquads/voice; deterministic, no sim) */
    int reflections, pathing;           /* Steam per-source sends (need the SDK + geometry + settle) */
    int fdn, reflect_bed;               /* load-time reverb bed (FDN = no-SDK, reflect_bed = Steam) */
    int settle_ms;                      /* let the async sims publish before timing */
    int sim;                            /* profile: cave_sim (array-sim room + virtual-speaker HRTF) */
    int panner;                         /* bwa_panner: 0 DBAP, 1 SPCAP, 2 VBAP */
    int dual, cap;                      /* dual-band panning, + compensated amplitude panning */
    int ism;                            /* image-source early reflections (needs the ISM room) */
    int beds;                           /* 3rd-order (16-ch) ambisonic beds */
    int nolim;                          /* protection limiter OFF (it defaults on) */
    int rig;                            /* use the layout= file (skipped without one) */
    int moving;                         /* listener walks a small circle each block (re-solves every voice) */
} Sit;

/* Put the timing thread where a real audio thread runs. On a hybrid CPU (P + E cores) a
 * normal-priority thread migrates between core types and the same block measures 2x apart run to
 * run; MMCSS lands the ASIO/WASAPI thread on a performance core, so the bench asks for the same:
 * the CPU sets of the highest efficiency class, power throttling off, high priority. No-op on a
 * homogeneous CPU apart from the priority. */
static void bench_pin_thread(void) {
#if defined(_WIN32)
    ULONG len = 0;
    GetSystemCpuSetInformation(NULL, 0, &len, GetCurrentProcess(), 0);
    unsigned char* buf = len ? (unsigned char*)malloc(len) : NULL;
    if (buf && GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buf, len, &len, GetCurrentProcess(), 0)) {
        ULONG ids[256]; ULONG n = 0; BYTE top = 0;
        for (ULONG off = 0; off < len; ) {
            const SYSTEM_CPU_SET_INFORMATION* i = (const SYSTEM_CPU_SET_INFORMATION*)(buf + off);
            if (i->Type == CpuSetInformation && i->CpuSet.EfficiencyClass > top) top = i->CpuSet.EfficiencyClass;
            off += i->Size;
        }
        for (ULONG off = 0; off < len && n < 256; ) {
            const SYSTEM_CPU_SET_INFORMATION* i = (const SYSTEM_CPU_SET_INFORMATION*)(buf + off);
            if (i->Type == CpuSetInformation && i->CpuSet.EfficiencyClass == top) ids[n++] = i->CpuSet.Id;
            off += i->Size;
        }
        if (n) SetThreadSelectedCpuSets(GetCurrentThread(), ids, n);
    }
    free(buf);
    THREAD_POWER_THROTTLING_STATE pt;
    memset(&pt, 0, sizeof pt);
    pt.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    pt.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    pt.StateMask = 0;                                      /* 0 = throttling OFF (high QoS) */
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &pt, sizeof pt);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
}

typedef struct { double mean, median, p99, max; int nch; } Stats;
static int cmp_d(const void* a, const void* b) { double x = *(const double*)a, y = *(const double*)b; return (x < y) ? -1 : (x > y) ? 1 : 0; }

static int run_sit(const Sit* s, bwa_sound snd_unused, Stats* out, int K) {
    (void)snd_unused;
    bwa_desc cfg; memset(&cfg, 0, sizeof cfg);
    cfg.sink = BWA_SINK_MANUAL; cfg.sample_rate = SR; cfg.block_size = BLK;
    cfg.profile = s->binaural ? BWA_PROFILE_BINAURAL : s->sim ? BWA_PROFILE_CAVE_SIM : BWA_PROFILE_CAVE;
    cfg.enable_pathing = s->pathing ? true : false;
    cfg.layout_path = s->rig ? g_layout : NULL;
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
    if (s->ism) {
        bwa_material faces[6] = { 0, 0, 0, 0, 0, 0 };
        bwa_scene_set_ism_room(e, 8.0f, 4.0f, 8.0f, faces);
    }
    if (bwa_start(e) != 0) { printf("  %-22s bwa_start: %s\n", s->name, bwa_last_error(e)); bwa_destroy(e); return -1; }

    bwa_sound snd = bwa_load_sound(e, WAVF);
    if (!snd) { printf("  %-22s load: %s\n", s->name, bwa_last_error(e)); bwa_stop(e); bwa_destroy(e); return -1; }

    if (s->decorr) bwa_set_decorrelation(e, true);
    if (s->panner) bwa_set_panner(e, (bwa_panner)s->panner);
    if (s->dual)   bwa_set_dual_band(e, true);
    if (s->cap)    bwa_set_dual_band_cap(e, true);
    if (s->nolim)  bwa_set_limiter(e, false);
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
        if (s->ism)         bwa_source_set_early_reflections(e, v, true);
    }
    if (s->beds) {
        bwa_sound bs = bwa_load_ambix(e, BEDF);
        if (!bs) { printf("  %-22s bed load: %s\n", s->name, bwa_last_error(e)); bwa_stop(e); bwa_destroy(e); return -1; }
        for (int i = 0; i < s->beds; ++i) { bwa_bed b = bwa_bed_create(e); bwa_bed_play(e, b, bs, true); }
    }
    bwa_commit(e);

    if (s->settle_ms) bwa_sleep_ms((unsigned)s->settle_ms);          /* let occlusion/reflection/pathing sims publish */
    for (int b = 0; b < 128; ++b) bwa_render_block(e, NULL, NULL);   /* warm up (gain ramps, caches) */

    if (g_zones) bwa_prof_reset();                         /* start the per-zone tally over the timed window */
    double* t = (double*)malloc((size_t)K * sizeof(double));
    if (!t) { bwa_stop(e); bwa_destroy(e); return -1; }
    uint32_t nch = 0;
    for (int b = 0; b < K; ++b) {
        uint32_t c = 0, n = 0;
        if (s->moving) {                                   /* a walking, turning listener: every voice re-solves */
            float a = 0.01f * (float)b;
            bwa_set_listener_pose(e, 0.3f * cosf(a), 1.5f, 0.3f * sinf(a), 0.0f, sinf(0.5f * a), 0.0f, cosf(0.5f * a));
            bwa_commit(e);
        }
        double t0 = bwa_now_us();
        const float* out = bwa_render_block(e, &c, &n);
        double dt = bwa_now_us() - t0;
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
        else if (strncmp(argv[i], "layout=", 7) == 0) g_layout = argv[i] + 7;
        else if (strncmp(argv[i], "only=", 5) == 0) g_only = argv[i] + 5;
        else { int v = atoi(argv[i]); if (v > 0) K = v; }
    }
    if (K < 500) K = 500;
    bench_pin_thread();
    if (g_zones && !bwa_prof_reset()) {                    /* probe: is the dll a -DBWA_PROFILE_SELF build? */
        printf("note: 'zones' needs a dll built with -DBWA_PROFILE_SELF; showing totals only.\n");
        g_zones = 0;
    }
    if (!write_sine_wav(WAVF, 220.0, SR, SR) || !write_bed_wav(BEDF, SR, SR)) { printf("wav write failed\n"); return 1; }

    const double budget_us = 1.0e6 * (double)BLK / (double)SR;   /* 5333.3 us at 256/48k */

    static const Sit sits[] = {
        { "idle (cave)" },
        { "idle, limiter off",          .nolim = 1 },
        { "1 voice DBAP",               .nvoices = 1 },
        { "8 voices DBAP",              .nvoices = 8 },
        { "26 voices DBAP",             .nvoices = 26 },
        { "26v DBAP, moving",           .nvoices = 26, .moving = 1 },
        { "26v SPCAP, moving",          .nvoices = 26, .moving = 1, .panner = 1 },
        { "26v VBAP, moving",           .nvoices = 26, .moving = 1, .panner = 2 },
        { "8v dual-band",               .nvoices = 8, .dual = 1 },
        { "8v dual-band + CAP, moving", .nvoices = 8, .dual = 1, .cap = 1, .moving = 1 },
        { "8v binaural (HRTF)",         .binaural = 1, .nvoices = 8 },
        { "8v cave_sim",                .sim = 1, .nvoices = 8 },
        { "8v occlusion EQ",            .nvoices = 8, .occl_manual = 1 },
        { "8v Doppler+air",             .nvoices = 8, .doppler = 1, .air = 1 },
        { "8v spread (LOBE)",           .nvoices = 8, .spread = 1 },
        { "8v spread+decorrelate",      .nvoices = 8, .spread = 1, .decorr = 1 },
        { "8v ISM early refl",          .nvoices = 8, .ism = 1 },
        { "2 beds (3rd order)",         .beds = 2 },
        { "8v FDN reverb",              .nvoices = 8, .fdn = 1 },
        { "8v Steam reflect bed",       .nvoices = 8, .reflections = 1, .reflect_bed = 1, .settle_ms = 500 },
        { "8v Steam pathing",           .nvoices = 8, .pathing = 1, .settle_ms = 500 },
        { "16v all-on (no-SDK)",        .nvoices = 16, .doppler = 1, .air = 1, .spread = 1, .decorr = 1,
                                        .occl_manual = 1, .fdn = 1 },
        { "rig: idle",                  .rig = 1 },
        { "rig: 16v DBAP, moving",      .rig = 1, .nvoices = 16, .moving = 1 },
        { "rig: 8v cave_sim, moving",   .rig = 1, .sim = 1, .nvoices = 8, .moving = 1 },
        { "rig: 16v show, moving",      .rig = 1, .nvoices = 16, .moving = 1, .doppler = 1, .air = 1,
                                        .spread = 1, .decorr = 1, .ism = 1, .fdn = 1, .beds = 1 },
    };
    const int N = (int)(sizeof sits / sizeof sits[0]);

    printf("bw_audio per-situation bench  |  %u/%u = %.2f ms block budget  |  %d blocks/situation\n",
           BLK, SR, budget_us / 1000.0, K);
    printf("%-28s %4s  %9s %9s %9s %9s   %6s\n", "situation", "ch", "mean us", "median", "p99", "max", "%budget");
    printf("--------------------------------------------------------------------------------------\n");
    for (int i = 0; i < N; ++i) {
        if (sits[i].rig && !g_layout) continue;
        if (g_only && !strstr(sits[i].name, g_only)) continue;
        Stats st; memset(&st, 0, sizeof st);
        if (run_sit(&sits[i], 0, &st, K) != 0) continue;
        printf("%-28s %4d  %9.1f %9.1f %9.1f %9.1f   %5.1f%%\n",
               sits[i].name, st.nch, st.mean, st.median, st.p99, st.max, 100.0 * st.mean / budget_us);
        if (g_zones) { bwa_prof_report(); printf("\n"); }   /* per-zone breakdown for this situation */
    }
    printf("--------------------------------------------------------------------------------------\n");
    printf("note: audio-thread cost only; Steam occlusion/reflection/pathing RAY-TRACING runs on\n"
           "      background sim threads (30/12/10 Hz, below-normal priority) and is not counted here.\n");
    remove(WAVF);
    remove(BEDF);
    return 0;
}
