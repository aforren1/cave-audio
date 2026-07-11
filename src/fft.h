/*
 * fft.h — small in-place radix-2 Cooley-Tukey FFT (double precision), shared by the OFFLINE DSP:
 * calibration/measurement (measure.c) and the Doppler-quality probe (test/doppler_fft.c). Header-only
 * (static inline) so it links into both the library and standalone test tools with no separate object.
 *
 * NOT for the audio thread. Runtime frequency-domain work (convolution reflections, HRTF) goes through
 * phonon's own SIMD FFT; this is the calibration/analysis workhorse where correctness, not speed, rules.
 */
#ifndef BW_FFT_H
#define BW_FFT_H

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* in-place iterative radix-2 FFT; dir = +1 forward, -1 inverse (inverse divides by n). n must be 2^k. */
static inline void fft(double* re, double* im, int n, int dir) {
    for (int i = 1, j = 0; i < n; ++i) {              /* bit-reversal permutation */
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = dir * 2.0 * M_PI / len;
        double wr = cos(ang), wi = sin(ang);
        for (int i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (int k = 0; k < len / 2; ++k) {
                double ur = re[i + k],            ui = im[i + k];
                double vr = re[i + k + len/2]*cr - im[i + k + len/2]*ci;
                double vi = re[i + k + len/2]*ci + im[i + k + len/2]*cr;
                re[i + k] = ur + vr;            im[i + k] = ui + vi;
                re[i + k + len/2] = ur - vr;    im[i + k + len/2] = ui - vi;
                double ncr = cr*wr - ci*wi;     ci = cr*wi + ci*wr; cr = ncr;
            }
        }
    }
    if (dir < 0) for (int i = 0; i < n; ++i) { re[i] /= n; im[i] /= n; }
}

/* smallest power of two >= x (for zero-padding a real signal up to a radix-2 length). */
static inline int next_pow2(int x) { int p = 1; while (p < x) p <<= 1; return p; }

#endif /* BW_FFT_H */
