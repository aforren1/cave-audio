/*
 * profile_bench.c — a headless, hardware-independent profiling load for Tracy.
 *
 * Forces the null sink (.sink = BWA_SINK_NULL), so the engine's render thread runs the FULL mix exactly
 * as the ASIO callback would, with no device needed. Spawns a representative moving voice load with
 * the propagation effects on (Doppler / air / reverb send / spread), then drives positions at a
 * ~60 Hz control rate (like a game client) for N seconds. Build with -DBWA_TRACY=ON and either
 * attach the Tracy GUI, or capture headless to a CSV budget report:
 *
 *   bwa_profile_bench 20 16 cave            # 20 s, 16 voices, cave (26-ch) profile
 *   tracy-capture -o bench.tracy -s 20 &   # connect + record (separate process)
 *   tracy-csvexport bench.tracy            # per-zone count / mean / median / min / max / total (CSV)
 *
 * Usage: bwa_profile_bench [seconds=20] [voices=16] [cave|binaural]
 */
#include "bw_audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* minimal mono 32-bit-float WAV (so we don't pull in a wav writer just for the bench) */
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
    for (uint32_t i = 0; i < frames; ++i) { float s = (float)sin(2.0 * 3.14159265358979 * freq * i / sr); fwrite(&s, 4, 1, f); }
    fclose(f);
    return 1;
}

int main(int argc, char** argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 20;
    int nv      = argc > 2 ? atoi(argv[2]) : 16;
    int binaural = (argc > 3 && strcmp(argv[3], "binaural") == 0);
    if (nv < 1) nv = 1;


    bwa_desc cfg; memset(&cfg, 0, sizeof cfg);
    cfg.sink = BWA_SINK_NULL;                /* hardware-independent; the null render thread does the mix */
    cfg.profile = binaural ? BWA_PROFILE_BINAURAL : BWA_PROFILE_CAVE;
    cfg.sample_rate = 48000;
    cfg.block_size  = 256;
    bwa_engine* e = bwa_create(&cfg);
    if (!e) { printf("bwa_create failed\n"); return 1; }

    /* configure the reflection bed + a box room (load-time; no-op without the Steam Audio SDK) so the
     * profile includes the audio-thread reverb convolution — the heaviest consumer in the production path. */
    bwa_reflections_desc rc; memset(&rc, 0, sizeof rc);
    rc.enabled = 1; rc.ir_seconds = 0.8f; rc.order = 1; rc.num_rays = 4096; rc.num_bounces = 8;
    bwa_reflections_config(e, &rc);
    bwa_material faces[6] = { 0, 0, 0, 0, 0, 0 };
    bwa_scene_set_box(e, 8.0f, 4.0f, 8.0f, faces);

    if (bwa_start(e) != 0) { const char* err = bwa_last_error(e); printf("bwa_start: %s\n", err ? err : "?"); }

    const char* WAV = "bwa_bench.wav";
    if (!write_sine_wav(WAV, 220.0, 48000, 48000 * 4)) { printf("write wav failed\n"); bwa_destroy(e); return 1; }
    bwa_sound snd = bwa_load_sound(e, WAV);
    if (!snd) { printf("load failed: %s\n", bwa_last_error(e)); bwa_stop(e); bwa_destroy(e); remove(WAV); return 1; }

    bwa_source* src = (bwa_source*)malloc((size_t)nv * sizeof(bwa_source));
    for (int i = 0; i < nv; ++i) {            /* fan the effects across the voices so every DSP path runs */
        src[i] = bwa_source_create(e);
        bwa_source_play(e, src[i], snd, true);
        bwa_source_set_gain(e, src[i], 0.15f);
        if (i % 2 == 0) bwa_source_set_doppler(e, src[i], true);
        if (i % 3 == 0) bwa_source_set_air_absorption(e, src[i], true);
        if (i % 4 == 0) { bwa_source_set_reflections(e, src[i], true); bwa_source_set_reflection_distance(e, src[i], true); }
        if (i % 5 == 0) bwa_source_set_spread(e, src[i], 0.5f);
    }
    bwa_commit(e);

    printf("profiling: %d voices, %s profile, %d s @ 256/48k (block budget = %.2f ms). Attach Tracy now...\n",
           nv, binaural ? "binaural" : "cave", seconds, 1000.0 * 256.0 / 48000.0);

    int ticks = seconds * 60;
    for (int t = 0; t < ticks; ++t) {
        float ph = (float)t * 0.05f;
        for (int i = 0; i < nv; ++i) {
            float a = ph + (float)i * (6.2831853f / (float)nv);
            float r = 2.0f + 1.5f * sinf(0.3f * ph + (float)i);
            /* orbit about the array centre at ear height (1.5 m), the engine's nominal listening
             * point under the floor-origin frame — a floor-level listener would profile skewed distances */
            bwa_source_set_pos(e, src[i], r * cosf(a), 1.5f + 0.8f * sinf(0.7f * ph + (float)i), r * sinf(a));
        }
        if (binaural) { float yaw = 0.2f * ph; bwa_set_listener_pose(e, 0, 1.5f, 0, 0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f)); }
        bwa_commit(e);
        Sleep(16);                            /* ~60 fps control rate */
    }

    bwa_stop(e); bwa_destroy(e);
    free(src); remove(WAV);
    return 0;
}
