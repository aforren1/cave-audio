/*
 * doppler_fft.c — objective Doppler-quality probe (not a pass/fail test; prints a spectrum report).
 *
 * Renders a sine through the engine's Doppler path with the source approaching at a CONSTANT radial
 * velocity, committing the position per "video frame" (configurable blocks/commit) the way a real
 * client does. A clean constant-velocity Doppler is a single pitch-shifted tone, so any chop / glide
 * corners / interpolation error shows up as sidebands. We FFT the (settled) output and report the
 * shifted fundamental, the spurious-free dynamic range, and the total spurious power OUTSIDE a guard
 * band around the fundamental (the slow distance-attenuation AM lives inside that guard and is not a
 * Doppler artifact). Also lists the top spurious peaks + their offset from the fundamental, so we can
 * see whether they sit at the commit rate, the block rate, or harmonics.
 *
 *   doppler_fft [f_in_hz=1000] [vel_mps=8] [blocks_per_commit=3]
 */
#include "rt.h"
#include "layout.h"
#include "fft.h"
#include "dr_wav.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SR     48000u
#define N      256u
#define FFTN   32768          /* power of two; ~0.68 s window, ~1.46 Hz/bin */
#define WARMUP 8192           /* discard while the 2-pole delay settles */

static float bus[BWA_CHANNELS * N];

static int write_sine(const char* path, double freq, uint32_t frames) {
    drwav_data_format fmt = { drwav_container_riff, DR_WAVE_FORMAT_IEEE_FLOAT, 1, SR, 32 };
    drwav wav;
    if (!drwav_init_file_write(&wav, path, &fmt, NULL)) return 0;
    float* buf = (float*)malloc((size_t)frames * sizeof(float));
    if (!buf) { drwav_uninit(&wav); return 0; }
    for (uint32_t i = 0; i < frames; ++i) buf[i] = (float)sin(2.0 * M_PI * freq * i / SR);
    drwav_uint64 w = drwav_write_pcm_frames(&wav, frames, buf);
    free(buf); drwav_uninit(&wav);
    return w == frames;
}

int main(int argc, char** argv) {
    double f_in = argc > 1 ? atof(argv[1]) : 1000.0;
    double vel  = argc > 2 ? atof(argv[2]) : 8.0;       /* m/s, approaching */
    int    bpc  = argc > 3 ? atoi(argv[3]) : 3;          /* blocks per position commit (~60 fps @ 256/48k) */
    if (bpc < 1) bpc = 1;

    Layout L = layout_default();
    RtCore* rt = rt_create(8, 4, SR, BWA_CHANNELS);
    if (!rt) { printf("FAIL rt_create\n"); return 1; }
    rt_set_layout(rt, &L);
    const char* WAV = "bwa_dop_sine.wav";
    if (!write_sine(WAV, f_in, SR * 4)) { printf("FAIL write sine\n"); rt_destroy(rt); return 1; }
    char err[256] = {0};
    uint32_t snd = rt_load_sound(rt, WAV, err, sizeof err);
    if (!snd) { printf("FAIL load: %s\n", err); rt_destroy(rt); remove(WAV); return 1; }
    uint32_t h = rt_source_create(rt);
    rt_source_set_doppler(rt, h, true);
    rt_source_play(rt, h, snd, true);

    double* re = (double*)calloc(FFTN, sizeof(double));
    double* im = (double*)calloc(FFTN, sizeof(double));
    float dist = 7.5f;                                   /* start far; stays inside the 8 m Doppler range */
    const double dt = (double)N / SR;
    bwa_timestamp ts; memset(&ts, 0, sizeof ts);
    int cap = 0, blk = 0, warm = 0;
    while (cap < FFTN) {
        if (blk % bpc == 0) { rt_source_set_pos(rt, h, dist, L.ref[1], 0.f); rt_commit(rt); }   /* ear plane: distance == dist */
        rt_render(rt, bus, N, &ts);
        for (uint32_t i = 0; i < N; ++i) {
            double s = 0; for (int ch = 0; ch < BWA_CHANNELS; ++ch) s += bus[(size_t)ch * N + i];
            if (warm < WARMUP) { ++warm; continue; }
            if (cap < FFTN) re[cap++] = s;               /* mono sum = source * sum(gains) */
        }
        dist -= (float)(vel * dt); if (dist < 1.0f) dist = 1.0f;
        ++blk;
    }

    /* Hann window + FFT */
    for (int i = 0; i < FFTN; ++i) { double w = 0.5 * (1.0 - cos(2.0 * M_PI * i / (FFTN - 1))); re[i] *= w; im[i] = 0.0; }
    fft(re, im, FFTN, +1);

    int half = FFTN / 2;
    double binhz = (double)SR / FFTN;
    double* mag = (double*)calloc(half, sizeof(double));
    int kpeak = 1; double mpeak = 0;
    for (int k = 1; k < half; ++k) { mag[k] = sqrt(re[k]*re[k] + im[k]*im[k]); if (mag[k] > mpeak) { mpeak = mag[k]; kpeak = k; } }
    double fpeak = kpeak * binhz;

    /* fundamental power = bins within +-3 of the peak; guard band excludes peak +- 25 Hz (peak leakage
     * + the slow distance-AM sidebands) and DC; everything else is Doppler-artifact spurious. */
    int guard = (int)(25.0 / binhz);
    double fund_pow = 0; for (int k = kpeak - 3; k <= kpeak + 3 && k < half; ++k) if (k > 0) fund_pow += mag[k]*mag[k];
    double spur_pow = 0, max_spur = 0; int kspur = 0;
    for (int k = 5; k < half; ++k) {
        if (abs(k - kpeak) <= guard) continue;
        spur_pow += mag[k]*mag[k];
        if (mag[k] > max_spur) { max_spur = mag[k]; kspur = k; }
    }
    double sfdr = 20.0 * log10(mpeak / (max_spur + 1e-300));
    double thdn = 10.0 * log10((spur_pow + 1e-300) / (fund_pow + 1e-300));

    printf("Doppler FFT probe:  f_in=%.0f Hz  vel=%.1f m/s  commit every %d blocks (~%.0f Hz)\n",
           f_in, vel, bpc, (double)SR / (bpc * N));
    printf("  fundamental: %.1f Hz (expected ~%.1f, shift %+.2f%%)   SFDR=%.1f dB   spurious(out of band)=%.1f dB\n",
           fpeak, f_in * (1.0 + vel / 343.0), 100.0 * (fpeak / f_in - 1.0), sfdr, thdn);
    printf("  top spurious peaks (dB below fundamental, offset from fundamental):\n");
    /* greedily report the 6 largest spurious peaks, suppressing +-guard around each found one */
    char* used = (char*)calloc(half, 1);
    for (int n_ = 0; n_ < 6; ++n_) {
        int kb = 0; double mb = 0;
        for (int k = 5; k < half; ++k) {
            if (abs(k - kpeak) <= guard || used[k]) continue;
            if (mag[k] > mb) { mb = mag[k]; kb = k; }
        }
        if (kb == 0) break;
        printf("      %6.1f Hz   %5.1f dB   (%+.1f Hz from fundamental)\n",
               kb * binhz, 20.0 * log10(mb / mpeak), kb * binhz - fpeak);
        for (int k = kb - guard; k <= kb + guard && k < half; ++k) if (k >= 0) used[k] = 1;
    }

    free(used); free(mag); free(re); free(im);
    rt_source_destroy(rt, h); rt_commit(rt);
    rt_destroy(rt); remove(WAV);
    return 0;
}
