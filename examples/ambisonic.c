/*
 * ambisonic.c — the ambisonic-bed walkthrough: load a pre-encoded soundfield and drive every
 * bed control by ear.
 *
 *   bwa_load_ambix / bwa_load_fuma   load a 4/9/16-ch B-format asset (FuMa converts at load)
 *   bwa_bed_create / bwa_bed_play    a bed is a voice playing a multichannel asset
 *   bwa_bed_set_rotation             yaw the field (glided at ~1 turn/s, click-free)
 *   bwa_bed_set_orientation          full 3-axis: level or tilt a capture (pitch/roll)
 *   bwa_set_bed_renderer             matrix decode vs parametric (DirAC) — live A/B
 *   bwa_set_max_re                   max-rE decode weighting — live A/B
 *
 * No assets needed: a 3rd-order AmbiX wav is synthesized (pink bursts from the FRONT, a click
 * train from the LEFT-UP, a low diffuse floor), plus the SAME field written in FuMa channel
 * order/normalization — the two loads must sound identical, which is the point of bwa_load_fuma.
 *
 * Runs anywhere: binaural profile, auto-picked 2-ch ASIO device, silent null-sink fallback
 * without one (bwa_get_audio_backend says which you got).
 *
 *   bwa_ambisonic
 */
#include "bw_audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define RATE   48000u
#define SECS   4u
#define FRAMES (RATE * SECS)

/* -- scaffolding: synthesize the field. A real client ships recorded/DAW-encoded files. -- */

/* SN3D real spherical harmonics to order 3 (the AmbiX set), taking a ROOM direction (right-handed,
 * +y up, +z front) — the ambi axes are x=front,y=left,z=up, i.e. (room z, room x, room y). */
static void sh16_room(float rx, float ry, float rz, float y[16]) {
    const float len = sqrtf(rx*rx + ry*ry + rz*rz);
    const float x = rz / len, yy = rx / len, z = ry / len;      /* room -> ambi, normalized */
    y[0]  = 1.0f;
    y[1]  = yy;             y[2]  = z;              y[3]  = x;
    y[4]  = 1.7320508f * x * yy;
    y[5]  = 1.7320508f * yy * z;
    y[6]  = 0.5f * (3.0f * z * z - 1.0f);
    y[7]  = 1.7320508f * x * z;
    y[8]  = 0.8660254f * (x * x - yy * yy);
    y[9]  = 0.7905694f * yy * (3.0f * x * x - yy * yy);
    y[10] = 3.8729833f * x * yy * z;
    y[11] = 0.6123724f * yy * (5.0f * z * z - 1.0f);
    y[12] = 0.5f * z * (5.0f * z * z - 3.0f);
    y[13] = 0.6123724f * x * (5.0f * z * z - 1.0f);
    y[14] = 1.9364917f * z * (x * x - yy * yy);
    y[15] = 0.7905694f * x * (x * x - 3.0f * yy * yy);
}

static float white(unsigned int* s) {
    *s = *s * 1664525u + 1013904223u;
    return (float)((int)(*s >> 9) - (1 << 22)) / (float)(1 << 22);
}
static float pink(float w, float b[7]) {                        /* Paul Kellet pink filter */
    b[0] = 0.99886f * b[0] + w * 0.0555179f;  b[1] = 0.99332f * b[1] + w * 0.0750759f;
    b[2] = 0.96900f * b[2] + w * 0.1538520f;  b[3] = 0.86650f * b[3] + w * 0.3104856f;
    b[4] = 0.55000f * b[4] + w * 0.5329522f;  b[5] = -0.7616f * b[5] - w * 0.0168980f;
    float p = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362f;
    b[6] = w * 0.115926f;
    return p * 0.11f;
}

/* the field: pink bursts from the front, an offset click train from the left-up, diffuse floor */
static void gen_field(float* buf /* FRAMES x 16 */) {
    float yf[16], yl[16];
    sh16_room(0.0f, 0.0f, 1.0f, yf);                            /* front  (room +z) */
    sh16_room(0.87f, 0.5f, 0.0f, yl);                           /* left-up (room +x is LEFT) */
    unsigned int s1 = 33333u, s2 = 44444u, s3 = 55555u;
    float pb1[7] = { 0 }, pb3[7] = { 0 };
    const unsigned period = RATE / 2, on = period / 2, ramp = RATE / 100, clicklen = RATE / 333;
    for (unsigned i = 0; i < FRAMES; ++i) {
        unsigned ph = i % period;
        float env = (ph < ramp) ? (float)ph / ramp
                  : (ph < on - ramp) ? 1.0f
                  : (ph < on) ? (float)(on - ph) / ramp : 0.0f;
        float burst = pink(white(&s1), pb1) * 0.5f * env;
        unsigned pc = (i + period / 2) % period;                /* clicks in the bursts' gaps */
        float click = (pc < clicklen) ? white(&s2) * expf(-6.0f * (float)pc / clicklen) * 0.7f : 0.0f;
        float dif   = pink(white(&s3), pb3) * 0.08f;            /* W-only: reads as diffuse */
        float* f = buf + (size_t)i * 16;
        for (int k = 0; k < 16; ++k) f[k] = burst * yf[k] + click * yl[k];
        f[0] += dif;
    }
}

/* hand-written RIFF float32 wav, `ch` interleaved channels (like minimal.c's ping writer) */
static void put_u32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put_u16(FILE* f, uint16_t v) { fwrite(&v, 2, 1, f); }
static int write_wavf(const char* path, const float* buf, uint32_t frames, uint16_t ch) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    const uint32_t bytes = frames * ch * 4;
    fwrite("RIFF", 1, 4, f); put_u32(f, 36 + bytes); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put_u32(f, 16); put_u16(f, 3 /* IEEE float */); put_u16(f, ch);
    put_u32(f, RATE); put_u32(f, RATE * ch * 4); put_u16(f, (uint16_t)(ch * 4)); put_u16(f, 32);
    fwrite("data", 1, 4, f); put_u32(f, bytes); fwrite(buf, 1, bytes, f);
    fclose(f);
    return 1;
}

/* AmbiX -> FuMa, to write the legacy variant: FuMa channel i carries ACN fuma_acn[i], scaled by
 * the SN3D->FuMa factor (the inverse of what bwa_load_fuma applies — same published table). */
static void ambix_to_fuma(const float* ambi, float* fuma, uint32_t frames) {
    static const int   acn[16] = { 0, 3, 1, 2, 6, 7, 5, 8, 4, 12, 13, 11, 14, 10, 15, 9 };
    static const float s2f[16] = { 0.70710678f, 1.f, 1.f, 1.f,
                                   1.f, 1.15470054f, 1.15470054f, 1.15470054f, 1.15470054f,
                                   1.f, 1.18585412f, 1.18585412f, 1.34164079f, 1.34164079f,
                                   1.26491106f, 1.26491106f };
    for (uint32_t i = 0; i < frames; ++i)
        for (int k = 0; k < 16; ++k)
            fuma[(size_t)i * 16 + k] = ambi[(size_t)i * 16 + acn[k]] * s2f[k];
}

/* run `secs` of the demo loop: per-frame commit, like an engine tick */
static void run(bwa_engine* e, double secs, const char* msg) {
    if (msg) printf("%s\n", msg);
    for (int t = 0; t < (int)(secs * 60.0); ++t) { bwa_commit(e); Sleep(16); }
}

int main(void) {
    /* ---- synthesize the two variants of the same field ---- */
    printf("synthesizing a 3rd-order field (front bursts / left-up clicks / diffuse floor)...\n");
    float* ambi = (float*)malloc((size_t)FRAMES * 16 * sizeof(float));
    float* fuma = (float*)malloc((size_t)FRAMES * 16 * sizeof(float));
    if (!ambi || !fuma) { fprintf(stderr, "out of memory\n"); return 1; }
    gen_field(ambi);
    ambix_to_fuma(ambi, fuma, FRAMES);
    write_wavf("bwa_demo_ambix.wav", ambi, FRAMES, 16);
    write_wavf("bwa_demo_fuma.wav",  fuma, FRAMES, 16);
    free(ambi); free(fuma);

    /* ---- engine up (binaural monitor; silent null-sink fallback without a device) ---- */
    bwa_desc cfg = { 0 };
    cfg.profile     = BWA_PROFILE_BINAURAL;
    cfg.sample_rate = RATE;
    cfg.block_size  = 256;
    bwa_engine* e = bwa_create(&cfg);
    if (!e) { fprintf(stderr, "bwa_create failed\n"); return 1; }
    if (bwa_start(e) != 0) { fprintf(stderr, "bwa_start: %s\n", bwa_last_error(e)); bwa_destroy(e); return 1; }
    const char* be = bwa_get_audio_backend(e);
    printf("backend: %s%s\n", be, strncmp(be, "null", 4) == 0 ? "  (no ASIO device - silent run)" : "");

    bwa_sound field = bwa_load_ambix(e, "bwa_demo_ambix.wav");
    bwa_sound legacy = bwa_load_fuma(e, "bwa_demo_fuma.wav");   /* converted at load: an AmbiX asset now */
    if (!field || !legacy) { fprintf(stderr, "load: %s\n", bwa_last_error(e)); bwa_stop(e); bwa_destroy(e); return 1; }

    bwa_bed bed = bwa_bed_create(e);
    bwa_bed_set_gain(e, bed, 0.9f);
    bwa_bed_play(e, bed, field, true);

    /* ---- the walkthrough ---- */
    run(e, 5, "1) matrix decode, world-locked: bursts FRONT, clicks LEFT-UP, a diffuse floor");

    printf("2) yaw: spinning the whole field (bwa_bed_set_rotation glides, click-free)\n");
    for (int t = 0; t < 8 * 60; ++t) {                 /* ~one slow turn over 8 s */
        bwa_bed_set_rotation(e, bed, 0.8f * (float)t / 60.0f);
        bwa_commit(e); Sleep(16);
    }

    printf("3) tilt: pitch +80 deg (bwa_bed_set_orientation) - the front content moves to the ceiling\n");
    bwa_bed_set_orientation(e, bed, 0.0f, 1.4f, 0.0f);
    run(e, 5, NULL);
    bwa_bed_set_orientation(e, bed, 0.0f, 0.0f, 0.0f);          /* back level (also clears the yaw) */
    run(e, 3, "   ...and back level");

    for (int i = 0; i < 2; ++i) {                      /* live renderer A/B */
        bwa_set_bed_renderer(e, BWA_BED_PARAMETRIC);
        run(e, 3, "4) PARAMETRIC renderer (DirAC: direct part re-panned listener-relative, walkable)");
        bwa_set_bed_renderer(e, BWA_BED_MATRIX);
        run(e, 3, "   MATRIX renderer (the static decode)");
    }

    for (int i = 0; i < 2; ++i) {                      /* live max-rE A/B */
        bwa_set_max_re(e, true);
        run(e, 3, "5) max-rE decode weighting ON (fewer sidelobes, better off-centre localization)");
        bwa_set_max_re(e, false);
        run(e, 3, "   max-rE OFF (the raw decode)");
    }

    bwa_bed_play(e, bed, legacy, true);                /* the FuMa load of the SAME field */
    run(e, 5, "6) the FuMa-loaded copy - converted at load, it should sound identical to (1)");

    /* ---- teardown ---- */
    bwa_bed_fade_out(e, bed, 0.5f);
    run(e, 0.8, NULL);
    bwa_bed_destroy(e, bed);
    bwa_unload_sound(e, field);
    bwa_unload_sound(e, legacy);
    bwa_stop(e);
    bwa_destroy(e);
    remove("bwa_demo_ambix.wav");
    remove("bwa_demo_fuma.wav");
    printf("done\n");
    return 0;
}
