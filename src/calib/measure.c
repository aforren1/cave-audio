/* measure.c — see measure.h. Exponential sine sweep + regularized deconvolution. Pure DSP. */
#include "calib/measure.h"
#include "dsp/fft.h"                                   /* shared radix-2 fft() */
#include "core/bits.h"                                  /* bwa_pow2_ge */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void measure_sweep(float* out, int n, double f1, double f2, double fs) {
    if (!out || n <= 0 || !(fs > 0.0) || !(f1 > 0.0) || !(f2 > f1)) return;
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

/* sum |H(k)|^2 over the same bins band_mean averages */
static double band_energy(const double* hre, const double* him, int L, double fs, double flo, double fhi) {
    int klo = (int)(flo * L / fs); if (klo < 1) klo = 1;
    int khi = (int)(fhi * L / fs); if (khi > L / 2) khi = L / 2;
    double acc = 0.0;
    for (int k = klo; k <= khi; ++k) acc += hre[k]*hre[k] + him[k]*him[k];
    return acc;
}

/* Deconvolve capture vs ref -> the real impulse response (caller frees *ir_out) + the in-band level +
 * 3-band tilt from the spectrum (+ the level band's energy, when e_level is non-NULL). Returns the
 * FFT length L, or 0 on allocation failure. */
static int deconvolve(const float* capture, int ncap, const float* ref, int nref,
                      double f1, double f2, double fs, const double band_hz[2],
                      float* level, float band[3], double* e_level, float** ir_out) {
    const int L = (int)bwa_pow2_ge((uint32_t)(ncap + nref));
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
    if (e_level) *e_level = band_energy(cre, cim, L, fs, 2.0 * f1, 0.5 * f2);
    if (band) { band[0] = band_mean(cre, cim, L, fs, f1,         band_hz[0]);
                band[1] = band_mean(cre, cim, L, fs, band_hz[0], band_hz[1]);
                band[2] = band_mean(cre, cim, L, fs, band_hz[1], f2); }

    fft(cre, cim, L, -1);                       /* IFFT(H) -> real impulse response */
    for (int k = 0; k < L; ++k) ir[k] = (float)cre[k];
    *ir_out = ir;
    free(cre); free(cim); free(rre); free(rim);
    return L;
}

int measure_direct_gate(int first_refl, double fs) {
    return (first_refl > 8) ? first_refl - 4 : (int)(0.004 * fs);
}

#define ER_THRESH   0.06f      /* early reflections at least this fraction of the direct (~ -24 dB) */
#define ER_WINDOW_S 0.08       /* search for reflections within this many seconds after the direct */
#define DG_MIN_S    0.001      /* the direct gate's floor: nothing earlier than this counts as a reflection */
#define DG_PRE_S    0.001      /* the direct gate opens this long before the peak */

/* The first reflection after the direct peak p, as samples after it (0 = none): measure_rt60's
 * early-reflection rule, searched from DG_MIN_S on. The floor keeps the speaker's OWN ringing (a
 * band-limited peak's sidelobes, a crossover's group delay) from reading as a room surface, which
 * would collapse the gate to a few samples; no real surface returns within 34 cm of extra path. */
static int first_reflection(const float* ir, int nir, int p, double fs) {
    const float direct = fabsf(ir[p]);
    if (!(direct > 0.f)) return 0;
    int from = p + (int)(DG_MIN_S * fs), to = p + 1 + (int)(ER_WINDOW_S * fs);
    if (to > nir - 1) to = nir - 1;
    for (int i = from; i < to; ++i) {
        float a = fabsf(ir[i]);
        if (a > ER_THRESH * direct && a >= fabsf(ir[i-1]) && a > fabsf(ir[i+1])) return i - p;
    }
    return 0;
}

/* The direct-sound fields of a MeasureResult: window the IR around its peak p (a raised-cosine rise
 * over the DG_PRE_S before it, flat to the gate's last quarter, a raised-cosine fall to the gate
 * end), FFT at the deconvolution's own length L so bin k means the same frequency as H's, and take
 * the same band means. Nothing arrives before the direct sound, so the pre-window costs only noise;
 * it is there for the deconvolution's own pre-ringing and a non-minimum-phase speaker's. Returns 0 on
 * allocation failure. */
static int direct_fields(const float* ir, int L, int p, double f1, double f2, double fs,
                         const double band_hz[2], double e_full, MeasureResult* out) {
    int gate = measure_direct_gate(first_reflection(ir, L, p, fs), fs);
    if (gate > L - 1 - p) gate = L - 1 - p;
    if (gate < 1) gate = 1;
    out->gate_samples = gate;
    double* re = (double*)calloc((size_t)L, sizeof(double));
    double* im = (double*)calloc((size_t)L, sizeof(double));
    if (!re || !im) { free(re); free(im); return 0; }
    int pre = (int)(DG_PRE_S * fs); if (pre > p) pre = p;   /* never wrap to the IR's circular end */
    int fall = gate / 4; if (fall < 1) fall = 1;
    for (int i = p - pre; i < p + gate; ++i) {
        double w = 1.0;
        if (i < p)                     w = 0.5 - 0.5 * cos(M_PI * (double)(i - (p - pre) + 1) / (double)(pre + 1));
        else if (i >= p + gate - fall) w = 0.5 + 0.5 * cos(M_PI * (double)(i - (p + gate - fall) + 1) / (double)fall);
        re[i] = (double)ir[i] * w;
    }
    fft(re, im, L, +1);
    out->level_direct   = band_mean(re, im, L, fs, 2.0 * f1, 0.5 * f2);
    out->band_direct[0] = band_mean(re, im, L, fs, f1,         band_hz[0]);
    out->band_direct[1] = band_mean(re, im, L, fs, band_hz[0], band_hz[1]);
    out->band_direct[2] = band_mean(re, im, L, fs, band_hz[1], f2);
    const double e_dir = band_energy(re, im, L, fs, 2.0 * f1, 0.5 * f2);
    double f = (e_full > 0.0) ? e_dir / e_full : 1.0;
    if (!(f >= 0.0)) f = 1.0; else if (f > 1.0) f = 1.0;     /* NaN-safe; the window can nudge past 1 */
    out->direct_frac = (float)f;
    free(re); free(im);
    return 1;
}

static int ir_peak(const float* ir, int from, int to) {        /* strongest |tap| in [from, to) */
    int best = from; double bm = -1.0;
    for (int i = from; i < to; ++i) { double m = (double)ir[i]*ir[i]; if (m > bm) { bm = m; best = i; } }
    return best;
}

/* RMS of the loudest `blk`-sample block in ir[a, b) (a shorter tail block counts when it holds at
 * least MEASURE_NOISE_MIN_N samples); 0 and *n untouched for a region under that */
/* ... leaving out the samples within `xg` of each of the `nx` lags in `x` (the arrival's own harmonic
 * products); a block left with under MEASURE_NOISE_MIN_N samples does not count. */
static double loudest_block_rms(const float* ir, int a, int b, int blk, const int* x, int nx, int xg, int* n) {
    if (b - a < MEASURE_NOISE_MIN_N) return 0.0;
    double best = 0.0;
    for (int s = a; s < b; s += blk) {
        const int e = s + blk < b ? s + blk : b;
        if (e - s < MEASURE_NOISE_MIN_N) break;
        double acc = 0.0;
        int cnt = 0;
        for (int i = s; i < e; ++i) {
            int skip = 0;
            for (int k = 0; k < nx; ++k) skip |= (i >= x[k] - xg && i <= x[k] + xg);
            if (!skip) { acc += (double)ir[i] * ir[i]; ++cnt; }
        }
        if (cnt < MEASURE_NOISE_MIN_N) continue;
        const double r = sqrt(acc / (double)cnt);
        if (r > best) best = r;
        *n += cnt;
    }
    return best;
}

static double noise_floor_x(const float* ir, int ncap, int lo, int hi, double fs, const int* x, int nx, int* n);

double measure_noise_floor(const float* ir, int ncap, int lo, int hi, double fs, int* n) {
    return noise_floor_x(ir, ncap, lo, hi, fs, NULL, 0, n);
}

double measure_harmonic_lead(int nref, int k, double f1, double f2) {
    if (nref <= 0 || k < 1 || !(f1 > 0.0) || !(f2 > f1)) return 0.0;
    return (double)nref * log((double)k) / log(f2 / f1);
}

static double noise_floor_x(const float* ir, int ncap, int lo, int hi, double fs, const int* x, int nx, int* n) {
    int nn = 0;
    if (n) *n = 0;
    if (!ir || ncap <= 0 || !(fs > 0.0)) return 0.0;
    int blk = (int)(MEASURE_NOISE_BLOCK_S * fs); if (blk < MEASURE_NOISE_MIN_N) blk = MEASURE_NOISE_MIN_N;
    const int guard = (int)(MEASURE_NOISE_GUARD_S * fs), skip = (int)(MEASURE_NOISE_SKIP_S * fs);
    int b1 = lo - guard; if (b1 > ncap) b1 = ncap;
    int a2 = hi + skip;  if (a2 < 0) a2 = 0;
    const int xg = (int)(MEASURE_HARM_GUARD_S * fs);
    const double r1 = b1 > 0 ? loudest_block_rms(ir, 0, b1, blk, x, nx, xg, &nn) : 0.0;
    const double r2 = a2 < ncap ? loudest_block_rms(ir, a2, ncap, blk, x, nx, xg, &nn) : 0.0;
    if (n) *n = nn;
    return r1 > r2 ? r1 : r2;
}

int measure_response(const float* capture, int ncap, const float* ref, int nref,
                     double f1, double f2, double fs, const double band_hz[2], MeasureResult* out) {
    return measure_response_ex(capture, ncap, ref, nref, f1, f2, fs, band_hz, 0, 0, out, NULL, 0, 0, NULL);
}

int measure_response_win(const float* capture, int ncap, const float* ref, int nref,
                         double f1, double f2, double fs, const double band_hz[2], MeasureResult* out,
                         float* win, int win_len, int pre, int* win_start) {
    return measure_response_ex(capture, ncap, ref, nref, f1, f2, fs, band_hz, 0, 0, out, win, win_len, pre, win_start);
}

/* The earliest tap within MEASURE_FIRST_DB of the strongest (p), walking back no further than lo. */
static int first_arrival(const float* ir, int p, int lo, double fs) {
    const double thr = (double)ir[p] * ir[p] * pow(10.0, MEASURE_FIRST_DB / 10.0);
    const int back = (int)(MEASURE_FIRST_BACK_S * fs), lead = (int)(MEASURE_FIRST_MIN_S * fs);
    const int a = p - back > lo ? p - back : lo;
    for (int i = a; i < p - lead; ++i)
        if ((double)ir[i] * ir[i] >= thr) return ir_peak(ir, i, i + lead < p - lead ? i + lead : p - lead);
    return p;
}

int measure_response_ex(const float* capture, int ncap, const float* ref, int nref,
                        double f1, double f2, double fs, const double band_hz[2],
                        int exp_lo, int exp_hi, MeasureResult* out,
                        float* win, int win_len, int pre, int* win_start) {
    return measure_response_pick(capture, ncap, ref, nref, f1, f2, fs, band_hz, exp_lo, exp_hi, 0, 0,
                                 out, win, win_len, pre, win_start);
}

int measure_response_pick(const float* capture, int ncap, const float* ref, int nref,
                          double f1, double f2, double fs, const double band_hz[2],
                          int exp_lo, int exp_hi, int pick_lo, int pick_hi, MeasureResult* out,
                          float* win, int win_len, int pre, int* win_start) {
    return measure_response_lim(capture, ncap, ref, nref, f1, f2, fs, band_hz, exp_lo, exp_hi, pick_lo, pick_hi, 0,
                                out, win, win_len, pre, win_start);
}

int measure_response_lim(const float* capture, int ncap, const float* ref, int nref,
                         double f1, double f2, double fs, const double band_hz[2],
                         int exp_lo, int exp_hi, int pick_lo, int pick_hi, int lag_hi, MeasureResult* out,
                         float* win, int win_len, int pre, int* win_start) {
    if (!capture || !ref || !out || ncap <= 0 || nref <= 0 || !(fs > 0.0)) return 0;
    if (win && (win_len <= 0 || pre < 0 || !win_start)) return 0;
    /* bwa_pow2_ge spins forever above 2^31, and ncap + nref is signed int arithmetic — bound it */
    if ((int64_t)ncap + nref > (1 << 30)) return 0;
    float* ir = NULL;
    double e_full = 0.0;
    memset(out, 0, sizeof *out);
    int L = deconvolve(capture, ncap, ref, nref, f1, f2, fs, band_hz, &out->level, out->band, &e_full, &ir);
    if (!L) return 0;
    out->energy = (float)e_full;
    int npos = ncap < L ? ncap : L;                             /* the positive lags */
    if (lag_hi > 0 && lag_hi < npos) npos = lag_hi;             /* ... up to a second sweep's arrival */
    const int pany = ir_peak(ir, 0, npos);                      /* the strongest tap anywhere */
    int lo = exp_lo < 0 ? 0 : exp_lo, hi = exp_hi > npos ? npos : exp_hi;
    const int windowed = exp_hi > exp_lo && hi > lo;
    /* physical arrival = the strongest tap where this speaker's sound can arrive. A window that the
     * clamp emptied (it lies past the capture) falls back to the whole IR and flags it. */
    int p = windowed ? ir_peak(ir, lo, hi) : pany;
    const int pl = pick_lo < 0 ? 0 : pick_lo, ph = pick_hi > npos ? npos : pick_hi;
    if (pick_hi > pick_lo && ph > pl) p = ir_peak(ir, pl, ph);
    else if (windowed)                p = first_arrival(ir, p, lo, fs);
    out->delay_samples = p;
    out->peak_any = pany;
    if (exp_hi > exp_lo) {
        out->win_lo = exp_lo; out->win_hi = exp_hi;
        out->outside = !windowed || pany < lo || pany >= hi;
    }
    {
        const double a = fabs((double)ir[p]), b = fabs((double)ir[pany]);
        out->outside_db = (a > 0.0 && b > 0.0) ? (float)(20.0 * log10(b / a)) : 0.f;
        /* the floor around the window, or around the found arrival when there is none */
        int nn = 0;
        /* ... less the arrival's own harmonic products, which sit at positive lags once the arrival
         * is later than their lead (measure.h) */
        int hx[MEASURE_HARM_MAX - 1];
        for (int k = 2; k <= MEASURE_HARM_MAX; ++k) hx[k - 2] = p - (int)measure_harmonic_lead(nref, k, f1, f2);
        const double fl = noise_floor_x(ir, npos, windowed ? lo : p, windowed ? hi : p + 1, fs, hx, MEASURE_HARM_MAX - 1, &nn);
        out->floor_rms = (float)fl;
        out->noise_n = nn;
        out->snr_db = (nn > 0 && fl > 0.0 && a > 0.0) ? (float)(20.0 * log10(a / fl)) : (nn > 0 && a > 0.0 ? 300.f : 0.f);
    }
    /* sub-sample refinement: fit a parabola to |IR| at the peak and its neighbors (true peak of a
     * band-limited arrival lands between samples). Lifts delay precision from ~7 mm to well under 1 mm. */
    out->delay_frac = 0.f;
    if (p >= 1 && p + 1 < L) {
        float a = fabsf(ir[p-1]), b = fabsf(ir[p]), c = fabsf(ir[p+1]);
        float den = a - 2.f*b + c;
        if (den < 0.f) { float d = 0.5f * (a - c) / den; if (d > -0.5f && d < 0.5f) out->delay_frac = d; }
    }
    if (win) {
        const int s0 = p > pre ? p - pre : 0;
        for (int i = 0; i < win_len; ++i) win[i] = (s0 + i < L) ? ir[s0 + i] : 0.f;
        *win_start = s0;
    }
    int ok = direct_fields(ir, L, p, f1, f2, fs, band_hz, e_full, out);
    free(ir);
    return ok;
}

/* The decay analysis's knobs (rt_decay). measure_test pins what the analysis DOES (truncation,
 * subtraction, compensation, the late slope, the gate, the fit limits, the band edges and filter,
 * the IR's extent, the two no-decay rules). It does not pin these values one by one: the tail
 * tenth, a fit margin of 5 dB, a block of no periods and the broadband high-pass (which only
 * matters with rumble under 2 f1 in the capture, and no test has any) all pass it. They are
 * tuning choices from Lundeby's ranges, untested on rig data. */
#define RT_SQRT2          1.41421356237309505
#define RT_BLK_MIN_S      0.010    /* the envelope's block: Lundeby's 10 to 50 ms */
#define RT_BLK_MAX_S      0.050
#define RT_BLK_PERIODS    2.0      /* ... and at least this many periods of the band center */
#define RT_BLKS_PER_10DB  5.0
#define RT_FIT_MARGIN_DB  10.0     /* the slope fit stops this far above the floor */
#define RT_LATE_SPAN_DB   20.0     /* ... and the late-slope fit starts this far above that */
#define RT_NOISE_PAST_DB  10.0     /* the floor is re-read from this much decay past the crossing */
#define RT_RANGE_CAP_DB   99.0
/* The two rules that say "there is no decay here" (rt_decay). An anechoic capture, or a dead room,
 * still leaves something after the direct sample: the deconvolution's own skirt, a capsule
 * cluster's spread, the filter's last bit of the direct sound. It has range to spare and fits a
 * line, and before these rules it printed 5.8 s at 63 Hz and 0.3 ms from 125 Hz up.
 *   RT_RESIDUE_DB: the band's energy from the direct arrival on, against its energy over the
 *     whole IR. Backward filtering puts the direct sound's own band energy AHEAD of the arrival,
 *     so what follows is the room's part: -10 dB in a room whose direct sound is 10 dB over its
 *     reverberation, and -113 dB (1 kHz) to -178 dB (63 Hz) for a lone tap. 30 dB sits between.
 *   RT_MIN_S and RT_MIN_OVER_BW: a decay shorter than the band can resolve. A 19-capsule mean's
 *     arrival cluster keeps -9 dB at 4 kHz, past the first rule, and decays in under a
 *     millisecond. The floor is 30 ms, or 2 / bandwidth where that is longer (45 ms at 63 Hz):
 *     backward filtering reads true down to about B T = 4, and the suite's shortest real decays
 *     (0.10 s at 63 Hz, 0.06 s at 125 Hz) sit at twice the floor. */
#define RT_RESIDUE_DB     30.0
#define RT_MIN_S          0.030
#define RT_MIN_OVER_BW    2.0

const float measure_rt_band_hz[MEASURE_RT_BANDS] = { 63.f, 125.f, 250.f, 500.f, 1000.f, 2000.f, 4000.f, 8000.f };

/* One RBJ low-pass or high-pass section, designed and run in double. biquad.h has no low-pass or
 * high-pass, and double costs nothing off the audio thread. It is not a NEED at 48 kHz: the 63 Hz
 * band's pole pairs sit 0.0015, 0.0041 and 0.0056 from the unit circle, and float coefficients
 * moved its response 0.008 dB, float state 0.004 dB (measure_test's filter response case). */
typedef struct { double b0, b1, b2, a1, a2; } RtBq;

static void rt_bq_design(RtBq* q, int highpass, double fc, double Q, double fs) {
    const double w0 = 2.0 * M_PI * fc / fs, cw = cos(w0), alpha = sin(w0) / (2.0 * Q), a0 = 1.0 + alpha;
    if (highpass) { q->b0 = (1.0 + cw) / 2.0 / a0; q->b1 = -(1.0 + cw) / a0; q->b2 = q->b0; }
    else          { q->b0 = (1.0 - cw) / 2.0 / a0; q->b1 =  (1.0 - cw) / a0; q->b2 = q->b0; }
    q->a1 = -2.0 * cw / a0; q->a2 = (1.0 - alpha) / a0;
}

static void rt_bq_run(const RtBq* q, double* x, int n) {       /* transposed direct form II, in place */
    double s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < n; ++i) {
        const double in = x[i], y = q->b0 * in + s1;
        s1 = q->b1 * in - q->a1 * y + s2;
        s2 = q->b2 * in - q->a2 * y;
        x[i] = y;
    }
}

/* A 6th-order Butterworth high-pass at f_hp into a 6th-order low-pass at f_lp, x in place; a
 * side at 0 is left out. For an octave band the edges are fc / sqrt 2 and fc sqrt 2, and the order
 * is set by the NEIGHBOR band: its center sits half an octave past the edge, where order N is down
 * 3 N dB, and a neighbor that decays more slowly shows through late in this band's curve. 18 dB is
 * about what IEC 61260 asks of an octave filter there; order 4 left 12 dB. */
void measure_rt_bandpass(double* x, int n, double f_hp, double f_lp, double fs) {
    static const double Q[3] = { 0.51763809020504152, 0.70710678118654752, 1.93185165257813657 };   /* Butterworth 6 */
    for (int hp = 1; hp >= 0; --hp) {
        const double f = hp ? f_hp : f_lp;
        if (!(f > 0.0)) continue;
        for (int s = 0; s < 3; ++s) {
            RtBq q;
            rt_bq_design(&q, hp, f, Q[s], fs);
            rt_bq_run(&q, x, n);
        }
    }
}

/* A decay rate in dB per sample (negative) to the samples it takes to fall `db`, bounded to [0, cap]
 * BEFORE the cast: a plateau fits a slope of -1e-12, and (int)(10 / 1e-12) is undefined. */
static int rt_span(double db, double slope, int cap) {
    const double v = db / -slope;
    return !(v > 0.0) ? 0 : (v < (double)cap ? (int)v : cap);
}

static double rt_blk_mean(const double* cs, int b, int blk) { return (cs[(b + 1) * blk] - cs[b * blk]) / (double)blk; }

/* The decay of one band: cs[i] = the energy of samples [0, i) from the direct arrival (n + 1 entries).
 * Truncates the Schroeder integral where the decay meets the noise, subtracts the noise's own energy
 * from what is left, adds back the decay's energy past the cut, and fits T20.
 *
 * Why all three: an integral run to the end of the IR adds noise x time, which flattens the curve and
 * reads long. Cut at the crossing alone, it drops the decay's remaining energy and reads short
 * (Lundeby et al. 1995 add it back from the late slope). And the noise under the kept part is still
 * 0.9 dB of the curve at the -25 dB point when the floor is the minimum 10 dB below it (subtraction;
 * Guski and Vorlaender 2014 found the three together the least biased). */
static void rt_decay(const double* cs, int n, double fs, double blk_s, double min_rt, double e_whole,
                     float* rt60, float* range_db) {
    *rt60 = 0.f; *range_db = 0.f;
    /* residue (RT_RESIDUE_DB): nothing follows the direct sound in this band. e_whole 0 = not asked.
     * Range 0, like every "no decay to measure" exit here: 99 would say "clean and not noise-limited". */
    if (!(e_whole >= 0.0) || !isfinite(e_whole)) return;
    if (e_whole > 0.0 && cs[n] < e_whole * pow(10.0, -RT_RESIDUE_DB / 10.0)) return;
    const int min_blk = (int)(blk_s * fs) > 8 ? (int)(blk_s * fs) : 8;
    const int max_blk = (int)(RT_BLK_MAX_S * fs) > min_blk ? (int)(RT_BLK_MAX_S * fs) : min_blk;
    /* an Inf or NaN anywhere in the IR is in cs[n]: no measurement, and never the "not noise-limited"
     * cap, which an Inf total would otherwise reach */
    if (n < 4 * min_blk || !(cs[n] > 0.0) || !isfinite(cs[n])) return;
    const int tail = n / 10;                                   /* Lundeby: the floor from the last tenth */
    double noise = (cs[n] - cs[n - tail]) / (double)tail;
    int    cut = n;                                            /* the integral's upper limit */
    double comp = 0.0;                                         /* the decay's energy past it */
    double head_db = 0.0;                                      /* peak block over the floor, when nothing fits */
    int    fitted = 0;
    if (noise > 0.0) {
        int blk = min_blk, prev = -1;
        for (int it = 0; it < 6; ++it) {
            const int nb = n / blk;
            if (nb < 4) break;
            int bpk = 0; double pk = 0.0;
            for (int b = 0; b < nb; ++b) { const double m = rt_blk_mean(cs, b, blk); if (m > pk) { pk = m; bpk = b; } }
            if (!(pk > 0.0)) break;
            head_db = 10.0 * log10(pk / noise);
            /* the fit ends at the first block under floor + RT_FIT_MARGIN_DB, and (from the second
             * pass on, when there are blocks enough) starts RT_LATE_SPAN_DB above that: the LATE
             * slope is what crosses the floor, and a two-slope decay's early part is not it */
            const double thr = noise * pow(10.0, RT_FIT_MARGIN_DB / 10.0);
            const double thr_late = noise * pow(10.0, (RT_FIT_MARGIN_DB + RT_LATE_SPAN_DB) / 10.0);
            int b1 = bpk;
            while (b1 < nb && rt_blk_mean(cs, b1, blk) >= thr) ++b1;
            int b0 = bpk;
            if (it > 0) {
                int bl = bpk;
                while (bl < b1 && rt_blk_mean(cs, bl, blk) >= thr_late) ++bl;
                if (b1 - bl >= 3) b0 = bl;
            }
            double slope = 0.0, icpt = 0.0;                    /* dB per sample, dB at sample 0 */
            if (b1 - b0 >= 3) {
                double sx = 0, sy = 0, sxx = 0, sxy = 0; const double m = (double)(b1 - b0);
                for (int b = b0; b < b1; ++b) {
                    const double t = ((double)b + 0.5) * blk, y = 10.0 * log10(rt_blk_mean(cs, b, blk));
                    sx += t; sy += y; sxx += t * t; sxy += t * y;
                }
                const double den = m * sxx - sx * sx;
                if (den > 0.0) { slope = (m * sxy - sx * sy) / den; icpt = (sy - slope * sx) / m; }
            }
            if (!(slope < 0.0)) {
                /* too short a decay to fit a line through (an anechoic capture), or none: cut where
                 * the envelope reaches the margin, nothing to extrapolate */
                if (!fitted) { cut = b1 * blk; comp = 0.0; }
                break;
            }
            fitted = 1;
            const double k = -slope * log(10.0) / 10.0;        /* energy decay rate per sample */
            const double tc = (10.0 * log10(noise) - icpt) / slope;
            if (tc >= (double)(n - tail)) {
                /* The line meets the "floor" inside the stretch the floor was first read from, the
                 * last tenth: that stretch is still the decay, and the window ended before any
                 * noise. Nothing to truncate or subtract (subtracting the decay's own end as noise
                 * read 2 to 3% short); the part past the window comes from the line. Only the first
                 * pass may say so: its fit runs over the whole decay, and a later pass's three-block
                 * late slope can come out flat enough to cross anywhere. Such a pass changes
                 * nothing. UNPINNED: letting every pass say so passes measure_test. It was seen on
                 * one decaying-noise room through measure_room at 15 dB capture SNR, where the
                 * 125 Hz band then read 0.193 s with 99 dB of range for 0.116 s with 46. */
                if (it == 0) { noise = 0.0; cut = n; comp = pow(10.0, (icpt + slope * (double)n) / 10.0) / k; }
                break;
            }
            cut = tc < (double)blk ? blk : (int)tc; comp = noise / k;
            /* the floor again, from RT_NOISE_PAST_DB of further decay past the crossing on (at least
             * the last tenth), so the decay's own skirt is out of it */
            int ns = cut + rt_span(RT_NOISE_PAST_DB, slope, n);
            if (ns > n - tail) ns = n - tail;
            noise = (cs[n] - cs[ns]) / (double)(n - ns);
            if (!(noise > 0.0)) { noise = 0.0; cut = n; comp = 0.0; break; }
            if (prev >= 0 && abs(cut - prev) < blk) break;
            prev = cut;
            blk = rt_span(10.0 / RT_BLKS_PER_10DB, slope, max_blk);   /* Lundeby: 3 to 10 intervals per 10 dB */
            if (blk < min_blk) blk = min_blk;
        }
    }
    if (!(noise > 0.0)) noise = 0.0;
    if (cut < 2) return;
    #define RT_POS(v) ((v) > 0.0 ? (v) : 0.0)
    #define RT_EDC(i) (RT_POS(cs[cut] - cs[i] - noise * (double)(cut - (i))) + comp)
    const double e0 = RT_EDC(0);
    if (!(e0 > 0.0)) return;
    /* the usable range: how far the curve falls before the floor, or before the window ends when
     * no floor is inside it. With neither (a synthetic IR that ends in zeros) the cap says "not
     * noise-limited". */
    double range = comp > 0.0 ? -10.0 * log10(comp / e0) : (noise > 0.0 ? head_db : RT_RANGE_CAP_DB);
    if (range > RT_RANGE_CAP_DB) range = RT_RANGE_CAP_DB;
    if (!(range > 0.0)) range = 0.0;
    *range_db = (float)range;
    if (range < MEASURE_RT_MIN_RANGE_DB) return;
    /* T20: the least-squares line through the curve from -5 to -25 dB, as ISO 3382 has it */
    int i5 = -1, i25 = -1, dead = 0;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int i = 0; i < cut; ++i) {
        const double e = RT_EDC(i);
        if (!(e > 0.0)) { dead = 1; break; }
        const double db = 10.0 * log10(e / e0);
        if (db <= -25.0) { i25 = i; break; }
        if (i5 < 0 && db <= -5.0) i5 = i;
        if (i5 >= 0) { const double t = (double)(i - i5); sx += t; sy += db; sxx += t * t; sxy += t * db; }
    }
    #undef RT_EDC
    #undef RT_POS
    /* the curve fell 25 dB inside a sample or two, or ran out of energy before it got there (a
     * lone tap leaves nothing after itself): a click, not a decay */
    if ((i25 >= 0 && i25 - (i5 < 0 ? 0 : i5) < 2) || (i25 < 0 && dead)) { *range_db = 0.f; return; }
    if (i5 < 0 || i25 < 0) return;
    const double m = (double)(i25 - i5), den = m * sxx - sx * sx;
    if (!(den > 0.0)) return;
    const double slope = (m * sxy - sx * sy) / den;            /* dB per sample */
    if (!(slope < 0.0)) return;
    const double rt = -60.0 / slope / fs;
    if (rt < min_rt) { *range_db = 0.f; return; }               /* RT_MIN_*: shorter than the band resolves */
    *rt60 = (float)rt;
}

/* One analysis: the IR from 0 to `end`, band-passed backward in time when a side is given, then the
 * decay from the direct arrival on. The filter runs over the samples AHEAD of the arrival too. They
 * change nothing at or after it (run backward, the filter has not reached them yet); they are what
 * the residue rule compares against. x holds `end` doubles, cs `end + 1`. */
static void rt_analyze(const float* ir, int end, int direct_idx, double fs, double f_hp, double f_lp,
                       double blk_s, double min_rt, double* x, double* cs, float* rt60, float* range_db) {
    const int n = end - direct_idx;
    if (n <= 0) return;
    double e_whole = 0.0;
    for (int i = 0; i < end; ++i) x[i] = (double)ir[end - 1 - i];
    if (f_hp > 0.0 || f_lp > 0.0) {
        measure_rt_bandpass(x, end, f_hp, f_lp, fs);
        for (int i = 0; i < end; ++i) e_whole += x[i] * x[i];
    }
    cs[0] = 0.0;
    for (int i = 0; i < n; ++i) { const double s = x[n - 1 - i]; cs[i + 1] = cs[i] + s * s; }
    rt_decay(cs, n, fs, blk_s, min_rt, e_whole, rt60, range_db);
}

/* measure_rt60_ex with each analysis ending at its own sample (absolute IR indices, clamped to nir;
 * band_end NULL = nir): measure_room knows where the sweep stopped covering each band. An end at or
 * before the direct arrival leaves that analysis with no reading. */
static void rt60_core(const float* ir, int nir, int direct_idx, double fs, double f_lo, double f_hi,
                      int bb_end, const int* band_end, RoomResult* out) {
    if (!out) return;                    /* the memset dereferenced out BEFORE any guard */
    memset(out, 0, sizeof *out);
    if (!ir || direct_idx < 0 || direct_idx >= nir - 2 || !(fs > 0.0)) return;
    double* x  = (double*)malloc((size_t)nir * sizeof(double));
    double* cs = (double*)malloc((size_t)(nir + 1) * sizeof(double));
    if (x && cs) {
        /* The broadband figure, over the VALID band only. Unfiltered it carries the noise outside
         * [f_lo, f_hi], and a sweep's deconvolution raises noise 3 dB per octave, so the top octave
         * the sweep does not vouch for was most of the floor; and bb_end is where the capture stops
         * covering f_hi, which is only the end of the IR once nothing above f_hi is in it. */
        rt_analyze(ir, bb_end < nir ? bb_end : nir, direct_idx, fs, f_lo > 0.0 ? f_lo : 0.0, f_hi < 0.45 * fs ? f_hi : 0.0,
                   RT_BLK_MIN_S, RT_MIN_S, x, cs, &out->rt60, &out->range_db);
        for (int b = 0; b < MEASURE_RT_BANDS; ++b) {
            const double fc = (double)measure_rt_band_hz[b], lo = fc / RT_SQRT2, hi = fc * RT_SQRT2;
            /* no reading, never a guess. Past 0.45 fs the low-pass has no design (its edge would be
             * at or over Nyquist); without this skip it puts out NaN and the band is dropped by the
             * non-finite check instead, so no test can tell the two apart. */
            if (!measure_rt_band_covered(fc, f_lo, f_hi, fs)) continue;
            const int e = band_end ? band_end[b] : nir;
            /* Filtered BACKWARD in time (Jacobsen and Rasmussen 1987; ISO 3382-1 allows it): an octave
             * filter rings, and its ringing is a decay of its own. Run in reverse, the ringing lands
             * AHEAD of the sound that excited it, where a backward integral never looks, and the
             * shortest decay a band can show drops about four times (B T > 4 against B T > 16).
             * Measured: a 0.10 s mode at 63 Hz reads 0.101 s backward and 0.077 forward, and a
             * 0.06 s one at 125 Hz 0.061 and 0.049 (measure_test's ringing cases). Zero-phase
             * (both directions) leaves half the ringing on the decay side. */
            double blk_s = RT_BLK_PERIODS / fc;                          /* an envelope needs a few periods a block */
            if (blk_s < RT_BLK_MIN_S) blk_s = RT_BLK_MIN_S;
            double min_rt = RT_MIN_OVER_BW / (hi - lo);
            if (min_rt < RT_MIN_S) min_rt = RT_MIN_S;
            rt_analyze(ir, e < nir ? e : nir, direct_idx, fs, lo, hi, blk_s, min_rt, x, cs,
                       &out->band_rt60[b], &out->band_range_db[b]);
        }
    }
    free(x); free(cs);

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

void measure_rt60_ex(const float* ir, int nir, int direct_idx, double fs, double f_lo, double f_hi, RoomResult* out) {
    rt60_core(ir, nir, direct_idx, fs, f_lo, f_hi, nir, NULL, out);
}

void measure_rt60(const float* ir, int nir, int direct_idx, double fs, RoomResult* out) {
    rt60_core(ir, nir, direct_idx, fs, 0.0, 0.5 * fs, nir, NULL, out);
}

int measure_rt_band_covered(double fc, double f_lo, double f_hi, double fs) {
    const double lo = fc / RT_SQRT2, hi = fc * RT_SQRT2;
    return lo >= f_lo && hi <= f_hi && (!(fs > 0.0) || hi <= 0.45 * fs);
}

static float room_value(const RoomResult* r, int b) { return b < MEASURE_RT_BANDS ? r->band_rt60[b] : r->rt60; }

/* Rank selection by counting, so the rows are never copied or reordered: n is a speaker count, and
 * the O(n^2) is under 5000 compares at 64. The even case is 0.5f * (a + b) in float, the arithmetic
 * bwa_calibrate printed its medians with before this moved here. */
static void room_median(const RoomResult* rows, int n, int b, float* med, int* cnt) {
    int m = 0;
    for (int i = 0; i < n; ++i) if (room_value(&rows[i], b) > 0.f) ++m;
    *cnt = m;
    *med = 0.f;
    if (!m) return;
    const int k1 = (m - 1) / 2, k2 = m / 2;          /* the same rank when m is odd */
    float a = 0.f, c = 0.f;
    for (int i = 0; i < n; ++i) {
        const float x = room_value(&rows[i], b);
        if (!(x > 0.f)) continue;
        int lt = 0, le = 0;
        for (int j = 0; j < n; ++j) {
            const float y = room_value(&rows[j], b);
            if (!(y > 0.f)) continue;
            lt += y < x;
            le += y <= x;
        }
        if (lt <= k1 && k1 < le) a = x;
        if (lt <= k2 && k2 < le) c = x;
    }
    *med = (m & 1) ? c : 0.5f * (a + c);
}

void measure_room_summary(const RoomResult* rows, int n, RoomSummary* out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!rows || n <= 0) return;
    out->speakers = n;
    for (int b = 0; b < MEASURE_RT_BANDS; ++b) room_median(rows, n, b, &out->band_rt60[b], &out->band_count[b]);
    room_median(rows, n, MEASURE_RT_BANDS, &out->rt60, &out->count);
}

#define CORR_N 8192                           /* generous so the inverted spectrum + cepstrum are smooth */

/* Gated magnitude: |FFT(ir[direct .. direct+gate_len])| with a raised-cosine tail fade so the
 * truncation doesn't ring the spectrum. Fills mag[CORR_N/2 + 1]; re/im are CORR_N scratch. */
static void gated_mag(const float* ir, int direct, int gate_len, double* re, double* im, double* mag) {
    if (gate_len > CORR_N) gate_len = CORR_N;   /* the window can't exceed the FFT scratch (heap overflow) — a
                                                 * gate this long already spans the whole analysis window anyway */
    if (gate_len < 1) gate_len = 1;
    if (direct < 0) direct = 0;
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
    /* `direct > nir - gate_len` rather than `direct + gate_len > nir`: the sum overflows int for an
     * absurd `direct` and the wrapped negative passes the check into an out-of-bounds gate read */
    if (!ir || !taps || direct < 0 || gate_len < 4 || gate_len > nir || direct > nir - gate_len || ntaps < 1) return 0;
    if (!(fs > 0.0)) return 0;
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
    if (!ir || !taps || direct < 0 || gate_len < 4 || gate_len > nir || direct > nir - gate_len || ntaps < 1) return 0;
    if (!(cycles > 0.0) || !(max_win_s > 0.0) || !(fs > 0.0)) return 0;
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
    if (!(f_lo > 0.0) || !(f_hi > f_lo) || !(max_cut_db > 0.0) || !(fs > 0.0)) return -1;
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

int measure_ir_around(const float* capture, int ncap, const float* ref, int nref,
                      double f1, double f2, double fs, int pre, float* out, int cap) {
    if (!capture || !ref || !out || ncap <= 0 || nref <= 0 || !(fs > 0.0) || pre < 0 || cap <= pre) return 0;
    if ((int64_t)ncap + nref > (1 << 30)) return 0;
    const double band_hz[2] = { 300.0, 3000.0 };
    float* ir = NULL;
    const int L = deconvolve(capture, ncap, ref, nref, f1, f2, fs, band_hz, NULL, NULL, NULL, &ir);
    if (!L) return 0;
    const int direct = ir_peak(ir, 0, ncap < L ? ncap : L);
    for (int k = 0; k < cap; ++k) {
        const int j = direct - pre + k;
        out[k] = (j >= 0 && j < L) ? ir[j] : 0.f;
    }
    free(ir);
    return 1;
}

int measure_room(const float* capture, int ncap, const float* ref, int nref,
                 double f1, double f2, double fs, RoomResult* out, float* ir_out, int ir_cap) {
    if (!capture || !ref || !out || ncap <= 0 || nref <= 0 || !(fs > 0.0)) return 0;
    if ((int64_t)ncap + nref > (1 << 30)) return 0;   /* bwa_pow2_ge spins forever above 2^31 */
    const double band_hz[2] = { 300.0, 3000.0 };
    float* ir = NULL;
    int L = deconvolve(capture, ncap, ref, nref, f1, f2, fs, band_hz, NULL, NULL, NULL, &ir);
    if (!L) return 0;
    int direct = ir_peak(ir, 0, ncap < L ? ncap : L);
    /* Where each decay analysis must stop. The deconvolution puts the capture's sound at time
     * lag + t(f) on lag `lag`, t(f) being when the sweep passes f, so past ncap - t(f) a band has
     * neither the room's decay nor the capture's noise in it: the capture ended first. A floor read
     * there is silence, and the truncation would trust it. The low bands keep the longest IR. Lags
     * past ncap are the negative ones (the sweep's harmonic products), never the room. */
    const int npos = ncap < L ? ncap : L;
    int band_end[MEASURE_RT_BANDS], bb_end = npos;
    const double f_lo = 2.0 * f1, f_hi = 0.5 * f2;
    if (f1 > 0.0 && f2 > f1) {
        const double per = (double)nref / log(f2 / f1);
        bb_end = npos - (int)(per * log(f_hi / f1));
        for (int b = 0; b < MEASURE_RT_BANDS; ++b) {
            double top = (double)measure_rt_band_hz[b] * RT_SQRT2;
            if (top > f2) top = f2; else if (top < f1) top = f1;
            band_end[b] = npos - (int)(per * log(top / f1));
        }
    } else {
        for (int b = 0; b < MEASURE_RT_BANDS; ++b) band_end[b] = npos;
    }
    /* The end is taken from each band's TOP edge, the first frequency of the band the capture stops
     * covering. Taking it from the center or the bottom edge runs 75 or 150 ms longer, into lags
     * where part of the band's noise is gone; in measure_test that moves a range by 1 dB and no
     * reading by more than 0.001 s, so the choice is unpinned. What is pinned is having an end at
     * all (every analysis run to the capture's end reads 1.4 s for 0.30 at 4 kHz).
     * A sweep under an octave wide puts f_hi below f1, and the log above goes negative. */
    if (bb_end > npos) bb_end = npos;
    for (int b = 0; b < MEASURE_RT_BANDS; ++b) if (band_end[b] > npos) band_end[b] = npos;
    rt60_core(ir, L, direct, fs, f_lo, f_hi, bb_end, band_end, out);
    if (ir_out && ir_cap > 0)                                   /* retain the kernel from the direct arrival */
        for (int k = 0; k < ir_cap; ++k) ir_out[k] = (direct + k < L) ? ir[direct + k] : 0.f;
    free(ir);
    return 1;
}
