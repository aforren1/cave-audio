/* measure.c — see measure.h. Exponential sine sweep + regularized deconvolution. Pure DSP. */
#include "measure.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* in-place iterative radix-2 FFT; dir = +1 forward, -1 inverse (inverse divides by n). n must be 2^k. */
static void fft(double* re, double* im, int n, int dir) {
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

static int next_pow2(int x) { int p = 1; while (p < x) p <<= 1; return p; }

void measure_sweep(float* out, int n, double f1, double f2, double fs) {
    if (n <= 0) return;
    const double w1 = 2.0 * M_PI * f1 / fs;           /* per-sample angular frequencies */
    const double w2 = 2.0 * M_PI * f2 / fs;
    const double T  = (double)n;
    const double L  = log(w2 / w1);
    const double K  = T * w1 / L;                      /* Farina ESS phase constants */
    for (int i = 0; i < n; ++i)
        out[i] = (float)sin(K * (exp((double)i / T * L) - 1.0));
    /* short raised-cosine fades so the endpoints don't click the speaker */
    int fade = (int)(0.005 * fs); if (fade > n / 2) fade = n / 2;
    for (int i = 0; i < fade; ++i) {
        double w = 0.5 - 0.5 * cos(M_PI * i / fade);
        out[i]         *= (float)w;
        out[n - 1 - i] *= (float)w;
    }
}

/* mean |H(k)| over the bins spanning [flo, fhi] Hz (one-sided). */
static float band_mean(const double* hre, const double* him, int L, double fs, double flo, double fhi) {
    int klo = (int)(flo * L / fs); if (klo < 1) klo = 1;
    int khi = (int)(fhi * L / fs); if (khi > L / 2) khi = L / 2;
    if (khi < klo) return 0.f;
    double acc = 0.0;
    for (int k = klo; k <= khi; ++k) acc += sqrt(hre[k]*hre[k] + him[k]*him[k]);
    return (float)(acc / (khi - klo + 1));
}

int measure_response(const float* capture, int ncap, const float* ref, int nref,
                     double f1, double f2, double fs, const double band_hz[2], MeasureResult* out) {
    if (!capture || !ref || !out || ncap <= 0 || nref <= 0) return 0;
    const int L = next_pow2(ncap + nref);
    double* cre = (double*)calloc((size_t)L, sizeof(double));   /* Capture -> H -> h (reused) */
    double* cim = (double*)calloc((size_t)L, sizeof(double));
    double* rre = (double*)calloc((size_t)L, sizeof(double));   /* Reference (kept for |Ref|^2) */
    double* rim = (double*)calloc((size_t)L, sizeof(double));
    if (!cre || !cim || !rre || !rim) { free(cre); free(cim); free(rre); free(rim); return 0; }

    for (int i = 0; i < ncap; ++i) cre[i] = capture[i];
    for (int i = 0; i < nref; ++i) rre[i] = ref[i];
    fft(cre, cim, L, +1);
    fft(rre, rim, L, +1);

    /* regularization: eps = -80 dB of the strongest reference bin, so the out-of-band nulls of the
     * sweep don't blow up the division while in-band gain is untouched. */
    double maxP = 0.0;
    for (int k = 0; k < L; ++k) { double p = rre[k]*rre[k] + rim[k]*rim[k]; if (p > maxP) maxP = p; }
    const double eps = 1e-8 * (maxP > 0.0 ? maxP : 1.0);

    /* H = Capture * conj(Ref) / (|Ref|^2 + eps), in place into (cre,cim) */
    for (int k = 0; k < L; ++k) {
        double cr = cre[k], ci = cim[k], rr = rre[k], ri = rim[k];
        double num_r = cr*rr + ci*ri;          /* Cap * conj(Ref) */
        double num_i = ci*rr - cr*ri;
        double den   = rr*rr + ri*ri + eps;
        cre[k] = num_r / den; cim[k] = num_i / den;
    }

    /* level + bands: mean |H| over frequency ranges (before the IFFT). Measure the broadband level
     * over [2*f1, f2/2] to dodge the sweep's faded extremes. */
    out->level   = band_mean(cre, cim, L, fs, 2.0 * f1, 0.5 * f2);
    out->band[0] = band_mean(cre, cim, L, fs, f1,          band_hz[0]);
    out->band[1] = band_mean(cre, cim, L, fs, band_hz[0],  band_hz[1]);
    out->band[2] = band_mean(cre, cim, L, fs, band_hz[1],  f2);

    /* delay: IFFT(H) -> h, take the strongest tap among non-negative lags (the physical arrival). */
    fft(cre, cim, L, -1);
    int    best_i = 0;
    double best_m = -1.0;
    int    search = ncap < L ? ncap : L;
    for (int i = 0; i < search; ++i) {
        double m = cre[i]*cre[i] + cim[i]*cim[i];
        if (m > best_m) { best_m = m; best_i = i; }
    }
    out->delay_samples = best_i;

    free(cre); free(cim); free(rre); free(rim);
    return 1;
}
