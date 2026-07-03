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

/* Deconvolve capture vs ref -> the real impulse response (caller frees *ir_out) + the in-band level +
 * 3-band tilt from the spectrum. Returns the FFT length L, or 0 on allocation failure. */
static int deconvolve(const float* capture, int ncap, const float* ref, int nref,
                      double f1, double f2, double fs, const double band_hz[2],
                      float* level, float band[3], float** ir_out) {
    const int L = next_pow2(ncap + nref);
    double* cre = (double*)calloc((size_t)L, sizeof(double));   /* Capture -> H -> h (reused) */
    double* cim = (double*)calloc((size_t)L, sizeof(double));
    double* rre = (double*)calloc((size_t)L, sizeof(double));   /* Reference (kept for |Ref|^2) */
    double* rim = (double*)calloc((size_t)L, sizeof(double));
    float*  ir  = (float*) malloc((size_t)L * sizeof(float));
    if (!cre || !cim || !rre || !rim || !ir) { free(cre); free(cim); free(rre); free(rim); free(ir); return 0; }

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

    /* level + bands: mean |H| over frequency ranges (before the IFFT). Broadband over [2*f1, f2/2]
     * to dodge the sweep's faded extremes. */
    if (level) *level  = band_mean(cre, cim, L, fs, 2.0 * f1, 0.5 * f2);
    if (band) { band[0] = band_mean(cre, cim, L, fs, f1,         band_hz[0]);
                band[1] = band_mean(cre, cim, L, fs, band_hz[0], band_hz[1]);
                band[2] = band_mean(cre, cim, L, fs, band_hz[1], f2); }

    fft(cre, cim, L, -1);                       /* IFFT(H) -> real impulse response */
    for (int k = 0; k < L; ++k) ir[k] = (float)cre[k];
    *ir_out = ir;
    free(cre); free(cim); free(rre); free(rim);
    return L;
}

static int ir_peak(const float* ir, int from, int to) {        /* strongest |tap| in [from, to) */
    int best = from; double bm = -1.0;
    for (int i = from; i < to; ++i) { double m = (double)ir[i]*ir[i]; if (m > bm) { bm = m; best = i; } }
    return best;
}

int measure_response(const float* capture, int ncap, const float* ref, int nref,
                     double f1, double f2, double fs, const double band_hz[2], MeasureResult* out) {
    if (!capture || !ref || !out || ncap <= 0 || nref <= 0) return 0;
    float* ir = NULL;
    int L = deconvolve(capture, ncap, ref, nref, f1, f2, fs, band_hz, &out->level, out->band, &ir);
    if (!L) return 0;
    int p = ir_peak(ir, 0, ncap < L ? ncap : L);                /* physical arrival = strongest tap */
    out->delay_samples = p;
    /* sub-sample refinement: fit a parabola to |IR| at the peak and its neighbours (true peak of a
     * band-limited arrival lands between samples). Lifts delay precision from ~7 mm to well under 1 mm. */
    out->delay_frac = 0.f;
    if (p >= 1 && p + 1 < L) {
        float a = fabsf(ir[p-1]), b = fabsf(ir[p]), c = fabsf(ir[p+1]);
        float den = a - 2.f*b + c;
        if (den < 0.f) { float d = 0.5f * (a - c) / den; if (d > -0.5f && d < 0.5f) out->delay_frac = d; }
    }
    free(ir);
    return 1;
}

#define ER_THRESH   0.06f      /* early reflections at least this fraction of the direct (~ -24 dB) */
#define ER_WINDOW_S 0.08       /* search for reflections within this many seconds after the direct */

void measure_rt60(const float* ir, int nir, int direct_idx, double fs, RoomResult* out) {
    memset(out, 0, sizeof *out);
    if (!ir || direct_idx < 0 || direct_idx >= nir - 2) return;
    const int n = nir - direct_idx;

    /* Schroeder backward energy integration from the direct arrival, then T20 (-5 dB -> -25 dB,
     * x3 = RT60). edc[i] = energy from sample (direct+i) to the end. */
    double* edc = (double*)malloc((size_t)n * sizeof(double));
    if (!edc) return;
    double acc = 0.0;
    for (int i = n - 1; i >= 0; --i) { double s = (double)ir[direct_idx + i]; acc += s * s; edc[i] = acc; }
    if (edc[0] > 0.0) {
        int i5 = -1, i25 = -1;
        for (int i = 0; i < n; ++i) {
            double db = 10.0 * log10(edc[i] / edc[0]);
            if (i5  < 0 && db <= -5.0)  i5  = i;
            if (i25 < 0 && db <= -25.0) { i25 = i; break; }
        }
        if (i5 >= 0 && i25 > i5) out->rt60 = (float)(3.0 * (i25 - i5) / fs);   /* T20 -> RT60 */
    }
    free(edc);

    /* early reflections: |ir| peaks after the direct, within the window, above ER_THRESH of it */
    const float direct = fabsf(ir[direct_idx]);
    if (direct <= 0.f) return;
    int win = direct_idx + 1 + (int)(ER_WINDOW_S * fs);
    if (win > nir - 1) win = nir - 1;
    for (int i = direct_idx + 1; i < win && out->er_count < 8; ++i) {
        float a = fabsf(ir[i]);
        if (a > ER_THRESH * direct && a >= fabsf(ir[i-1]) && a > fabsf(ir[i+1])) {
            out->er_delay[out->er_count] = i - direct_idx;
            out->er_level[out->er_count] = a / direct;
            ++out->er_count;
        }
    }
}

#define CORR_N 8192                           /* generous so the inverted spectrum + cepstrum are smooth */

/* Gated magnitude: |FFT(ir[direct .. direct+gate_len])| with a raised-cosine tail fade so the
 * truncation doesn't ring the spectrum. Fills mag[CORR_N/2 + 1]; re/im are CORR_N scratch. */
static void gated_mag(const float* ir, int direct, int gate_len, double* re, double* im, double* mag) {
    memset(re, 0, (size_t)CORR_N * sizeof(double));
    memset(im, 0, (size_t)CORR_N * sizeof(double));
    int gfade = gate_len / 4; if (gfade < 1) gfade = 1;
    for (int i = 0; i < gate_len; ++i) {
        double w = 1.0;
        if (i >= gate_len - gfade) { int j = i - (gate_len - gfade); w = 0.5 + 0.5 * cos(M_PI * (j + 1) / gfade); }
        re[i] = (double)ir[direct + i] * w;
    }
    fft(re, im, CORR_N, +1);
    for (int k = 0; k <= CORR_N / 2; ++k) mag[k] = sqrt(re[k]*re[k] + im[k]*im[k]);
}

/* Shared inversion back-end (steps 3-6 of the correction design): one-sided linear magnitude
 * mag[CORR_N/2+1] -> minimum-phase correction FIR. See measure_correction's contract. */
static int correction_from_mag(const double* mag, double f1, double f2, double fs,
                               double max_boost_db, double max_cut_db, int ntaps, float* taps) {
    const int N = CORR_N;
    if (ntaps < 1 || ntaps > N) return 0;
    double* re = (double*)calloc((size_t)N, sizeof(double));
    double* im = (double*)calloc((size_t)N, sizeof(double));
    double* lr = (double*)calloc((size_t)N, sizeof(double));   /* target log-magnitude of the correction */
    if (!re || !im || !lr) { free(re); free(im); free(lr); return 0; }

    /* 3. in-band geometric-mean level = the reference the correction flattens toward. */
    int klo = (int)(f1 * N / fs); if (klo < 1) klo = 1;
    int khi = (int)(f2 * N / fs); if (khi > N / 2) khi = N / 2;
    if (khi <= klo) { free(re); free(im); free(lr); return 0; }
    double logsum = 0.0; int cnt = 0;
    for (int k = klo; k <= khi; ++k) { double m = mag[k]; if (m > 1e-12) { logsum += log(m); ++cnt; } }
    double target = (cnt > 0) ? exp(logsum / cnt) : 1.0;

    /* 4. desired |C(k)| = clamp(target/|G|) in-band (regularized so deep nulls aren't fought), 1 out of
     *    band, raised-cosine crossfade at the edges. Store as log-magnitude (symmetric). */
    const double maxb = pow(10.0,  max_boost_db / 20.0);
    const double maxc = pow(10.0, -max_cut_db  / 20.0);
    /* taper the correction to unity over the bottom/top HALF-OCTAVE only (constant-Q, so it doesn't
     * eat the midband the way a linear-bin edge does), and fully outside [f1, f2]. */
    const double flo_in = f1 * 1.41421356, fhi_in = f2 * 0.70710678;   /* +/- half an octave */
    for (int k = 0; k <= N / 2; ++k) {
        double m = mag[k]; if (m < 1e-12) m = 1e-12;
        double c = target / m; if (c > maxb) c = maxb; if (c < maxc) c = maxc;
        double fk = (double)k * fs / N, w;
        if      (fk < f1 || fk > f2) w = 0.0;
        else if (fk < flo_in)        w = 0.5 - 0.5 * cos(M_PI * log(fk / f1) / log(flo_in / f1));
        else if (fk > fhi_in)        w = 0.5 - 0.5 * cos(M_PI * log(f2 / fk) / log(f2 / fhi_in));
        else                         w = 1.0;
        lr[k] = w * log(c);                    /* w blends the correction toward unity at the band edges */
    }
    for (int k = N / 2 + 1; k < N; ++k) lr[k] = lr[N - k];   /* even symmetry */

    /* 5. minimum-phase realization (cepstral): the min-phase phase is the Hilbert transform of the
     *    log-magnitude. cepstrum -> fold to the causal side -> back -> exp. No latency / pre-ring. */
    for (int k = 0; k < N; ++k) { re[k] = lr[k]; im[k] = 0.0; }
    fft(re, im, N, -1);                         /* real cepstrum in re[] */
    for (int n = 0; n < N; ++n) {
        double w = (n == 0) ? 1.0 : (n < N / 2) ? 2.0 : (n == N / 2) ? 1.0 : 0.0;
        re[n] *= w; im[n] *= w;
    }
    fft(re, im, N, +1);                         /* re = log-mag, im = min-phase phase */
    for (int k = 0; k < N; ++k) { double mg = exp(re[k]), ph = im[k]; re[k] = mg * cos(ph); im[k] = mg * sin(ph); }
    fft(re, im, N, -1);                         /* 6. min-phase correction impulse response */

    int tfade = ntaps / 8; if (tfade < 1) tfade = 1;
    for (int i = 0; i < ntaps; ++i) {
        double w = (i >= ntaps - tfade) ? 0.5 + 0.5 * cos(M_PI * (i - (ntaps - tfade) + 1) / tfade) : 1.0;
        taps[i] = (float)(re[i] * w);
    }
    free(re); free(im); free(lr);
    return 1;
}

int measure_correction(const float* ir, int nir, int direct, int gate_len,
                       double f1, double f2, double fs, double max_boost_db, double max_cut_db,
                       int ntaps, float* taps) {
    if (!ir || !taps || direct < 0 || gate_len < 4 || direct + gate_len > nir || ntaps < 1) return 0;
    double* re  = (double*)malloc((size_t)CORR_N * sizeof(double));
    double* im  = (double*)malloc((size_t)CORR_N * sizeof(double));
    double* mag = (double*)malloc((size_t)(CORR_N / 2 + 1) * sizeof(double));
    if (!re || !im || !mag) { free(re); free(im); free(mag); return 0; }
    /* 1+2: gate the direct sound (window to before the first reflection -> this corrects the SPEAKER,
     * not the room) and take its magnitude. */
    gated_mag(ir, direct, gate_len, re, im, mag);
    int ok = correction_from_mag(mag, f1, f2, fs, max_boost_db, max_cut_db, ntaps, taps);
    free(re); free(im); free(mag);
    return ok;
}

int measure_correction_room(const float* ir, int nir, int direct, int gate_len,
                            double cycles, double max_win_s,
                            double f1, double f2, double fs, double max_boost_db, double max_cut_db,
                            int ntaps, float* taps) {
    if (!ir || !taps || direct < 0 || gate_len < 4 || direct + gate_len > nir || ntaps < 1) return 0;
    if (!(cycles > 0.0) || !(max_win_s > 0.0)) return 0;
    const int N = CORR_N;
    /* dyadic window ladder: gate_len, 2*gate_len, ... up to max_win_s (clamped to the IR and to N).
     * The per-frequency magnitude is log-blended between the two windows bracketing cycles/f. */
    int maxwin = (int)(max_win_s * fs);
    if (maxwin > nir - direct) maxwin = nir - direct;
    if (maxwin > N)            maxwin = N;
    if (maxwin < gate_len)     maxwin = gate_len;
    int ws[16]; int nw = 0;
    for (int w = gate_len; nw < 15; ) {
        ws[nw++] = w;
        if (w >= maxwin) break;
        w *= 2; if (w > maxwin) w = maxwin;
    }
    double* re  = (double*)malloc((size_t)N * sizeof(double));
    double* im  = (double*)malloc((size_t)N * sizeof(double));
    double* mag = (double*)malloc((size_t)(N / 2 + 1) * sizeof(double));
    double* lm  = (double*)malloc((size_t)nw * (size_t)(N / 2 + 1) * sizeof(double));   /* per-window log-mags */
    if (!re || !im || !mag || !lm) { free(re); free(im); free(mag); free(lm); return 0; }
    for (int j = 0; j < nw; ++j) {
        gated_mag(ir, direct, ws[j], re, im, mag);
        for (int k = 0; k <= N / 2; ++k) lm[(size_t)j * (N / 2 + 1) + k] = log(mag[k] > 1e-12 ? mag[k] : 1e-12);
    }
    for (int k = 0; k <= N / 2; ++k) {
        double fk = (double)k * fs / N;
        double W  = (fk > 0.0) ? cycles * fs / fk : (double)ws[nw - 1];   /* target window (samples) */
        if (W <= ws[0])           mag[k] = exp(lm[k]);                                      /* HF: the direct gate */
        else if (W >= ws[nw - 1]) mag[k] = exp(lm[(size_t)(nw - 1) * (N / 2 + 1) + k]);     /* LF: the longest view */
        else {
            int j = 0; while (j + 1 < nw && ws[j + 1] < W) ++j;                             /* ws[j] <= W < ws[j+1] */
            double t = log(W / ws[j]) / log((double)ws[j + 1] / ws[j]);
            mag[k] = exp((1.0 - t) * lm[(size_t)j * (N / 2 + 1) + k] + t * lm[(size_t)(j + 1) * (N / 2 + 1) + k]);
        }
    }
    int ok = correction_from_mag(mag, f1, f2, fs, max_boost_db, max_cut_db, ntaps, taps);
    free(re); free(im); free(mag); free(lm);
    return ok;
}

#define CUTS_N        32768    /* long-window FFT: ~1.5 Hz bins at 48 kHz — enough to resolve modes */
#define CUTS_MIN_DB   3.0      /* a peak must stand this far above the smoothed baseline to be cut */
#define CUTS_BASE_OCT 0.75     /* baseline smoothing half-width (octaves) */

int measure_room_cuts(const float* ir, int nir, int direct, double fs,
                      double f_lo, double f_hi, double max_cut_db,
                      int max_sections, MeasureEqSection* out) {
    if (!ir || !out || direct < 0 || nir - direct < 256 || max_sections < 1) return -1;
    if (!(f_lo > 0.0) || !(f_hi > f_lo) || !(max_cut_db > 0.0)) return -1;
    const int N = CUTS_N;
    int avail = nir - direct; if (avail > N) avail = N;
    double* re = (double*)calloc((size_t)N, sizeof(double));
    double* im = (double*)calloc((size_t)N, sizeof(double));
    if (!re || !im) { free(re); free(im); return -1; }
    int tfade = avail / 8; if (tfade < 1) tfade = 1;                 /* tail fade against truncation ringing */
    for (int i = 0; i < avail; ++i) {
        double w = (i >= avail - tfade) ? 0.5 + 0.5 * cos(M_PI * (i - (avail - tfade) + 1) / tfade) : 1.0;
        re[i] = (double)ir[direct + i] * w;
    }
    fft(re, im, N, +1);

    /* magnitude (dB) over the EXTENDED band [f_lo/2, f_hi*2] so the octave-smoothed baseline has
     * support at the target-band edges. */
    int kelo = (int)(0.5 * f_lo * N / fs); if (kelo < 1) kelo = 1;
    int kehi = (int)(2.0 * f_hi * N / fs); if (kehi > N / 2) kehi = N / 2;
    int klo  = (int)(f_lo * N / fs); if (klo < kelo) klo = kelo;
    int khi  = (int)(f_hi * N / fs); if (khi > kehi) khi = kehi;
    if (khi - klo < 4) { free(re); free(im); return -1; }
    double* mdb = (double*)calloc((size_t)(kehi + 2), sizeof(double));
    double* exc = (double*)calloc((size_t)(kehi + 2), sizeof(double));
    if (!mdb || !exc) { free(re); free(im); free(mdb); free(exc); return -1; }
    for (int k = kelo; k <= kehi; ++k) {
        double m = sqrt(re[k]*re[k] + im[k]*im[k]); if (m < 1e-12) m = 1e-12;
        mdb[k] = 20.0 * log10(m);
    }
    /* excess over a ±CUTS_BASE_OCT-octave mean baseline: what a MODE sticks up by */
    const double span = pow(2.0, CUTS_BASE_OCT);
    for (int k = klo; k <= khi; ++k) {
        int a = (int)(k / span); if (a < kelo) a = kelo;
        int b = (int)(k * span); if (b > kehi) b = kehi;
        double acc = 0.0; int c = 0;
        for (int j = a; j <= b; ++j) { acc += mdb[j]; ++c; }
        exc[k] = mdb[k] - (c ? acc / c : mdb[k]);
    }

    /* greedy peak pick, loudest first; each accepted peak claims its half-prominence width so one
     * broad mode never spends two sections. Cut-only: dips are position-dependent nulls, never fought. */
    int count = 0;
    while (count < max_sections) {
        int    pk = -1; double pe = CUTS_MIN_DB;
        for (int k = klo + 1; k < khi; ++k)
            if (exc[k] >= pe && exc[k] >= exc[k-1] && exc[k] > exc[k+1]) { pe = exc[k]; pk = k; }
        if (pk < 0) break;
        int a = pk; while (a > kelo && exc[a] > pe * 0.5) --a;       /* half-prominence edges */
        int b = pk; while (b < kehi && exc[b] > pe * 0.5) ++b;
        double fc = (double)pk * fs / N;
        double d2 = mdb[pk-1] - 2.0*mdb[pk] + mdb[pk+1];             /* sub-bin refinement (parabolic) */
        if (d2 < 0.0) { double d = 0.5 * (mdb[pk-1] - mdb[pk+1]) / d2; if (d > -0.5 && d < 0.5) fc = (pk + d) * fs / N; }
        double bw = (double)(b - a) * fs / N; if (bw < fs / N) bw = fs / N;
        double q  = fc / bw; if (q < 1.0) q = 1.0; if (q > 12.0) q = 12.0;
        double cut = pe; if (cut > max_cut_db) cut = max_cut_db;
        out[count].fc      = (float)fc;
        out[count].gain_db = (float)-cut;
        out[count].q       = (float)q;
        ++count;
        for (int k = a; k <= b; ++k) exc[k] = 0.0;                   /* claim the peak's width */
    }
    free(re); free(im); free(mdb); free(exc);
    return count;
}

int measure_room(const float* capture, int ncap, const float* ref, int nref,
                 double f1, double f2, double fs, RoomResult* out, float* ir_out, int ir_cap) {
    if (!capture || !ref || !out || ncap <= 0 || nref <= 0) return 0;
    const double band_hz[2] = { 300.0, 3000.0 };
    float* ir = NULL;
    int L = deconvolve(capture, ncap, ref, nref, f1, f2, fs, band_hz, NULL, NULL, &ir);
    if (!L) return 0;
    int direct = ir_peak(ir, 0, ncap < L ? ncap : L);
    measure_rt60(ir, L, direct, fs, out);
    if (ir_out && ir_cap > 0)                                   /* retain the kernel from the direct arrival */
        for (int k = 0; k < ir_cap; ++k) ir_out[k] = (direct + k < L) ? ir[direct + k] : 0.f;
    free(ir);
    return 1;
}
