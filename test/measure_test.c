/* measure_test.c — the calibration measurement DSP, verified against a synthetic capture with a
 * known delay + gain + low-pass. Proves measure_response without the rig (the ASIO capture is the
 * only untested piece). */
#include "calib/measure.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); ++fails; } } while (0)

#define TPI 6.283185307179586
#define TWOSLOPE_FLOOR_DB (-40.0)   /* the two-slope case's floor, dB under the IR's peak */

/* |H(f)| of a real FIR h by direct DFT at one frequency. */
static double mag_at(const float* h, int n, double f, double fs) {
    double re = 0, im = 0, w = TPI * f / fs;
    for (int k = 0; k < n; ++k) { re += h[k] * cos(w * k); im -= h[k] * sin(w * k); }
    return sqrt(re * re + im * im);
}

/* one RBJ peaking-EQ biquad applied in place over x[] (direct form I). */
static void peak_biquad(float* x, int n, double f0, double Q, double gain_db, double fs) {
    double A = pow(10.0, gain_db / 40.0), w0 = TPI * f0 / fs, cw = cos(w0), alpha = sin(w0) / (2.0 * Q);
    double a0 = 1 + alpha / A;
    double b0 = (1 + alpha * A) / a0, b1 = (-2 * cw) / a0, b2 = (1 - alpha * A) / a0;
    double a1 = (-2 * cw) / a0, a2 = (1 - alpha / A) / a0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (int i = 0; i < n; ++i) {
        double xn = x[i], yn = b0*xn + b1*x1 + b2*x2 - a1*y1 - a2*y2;
        x2 = x1; x1 = xn; y2 = y1; y1 = yn; x[i] = (float)yn;
    }
}

/* Add to ir[at ..] one room mode: a unit tone at f under an exponential envelope that falls 60 dB in
 * rt seconds. A band of decaying NOISE would be the more room-like stimulus, but one realization of
 * it does not have a decay time to check against: its T20 scatters by about 1 / sqrt(B T), several
 * percent in the low bands (the white-decay case below reads 0.37 to 0.46 s for 0.40 under 1 kHz). A
 * mode's Schroeder curve is a straight line. */
static void add_mode(float* ir, int at, int n, double f, double rt, double phase, double fs) {
    for (int i = 0; i < n; ++i)
        ir[at + i] += (float)(exp(-6.9078 * (double)i / (rt * fs)) * sin(TPI * f * (double)i / fs + phase));
}

/* x + uniform noise whose RMS is `db` relative to x's peak sample */
static void add_floor(const float* x, float* out, int n, double db, unsigned seed) {
    float pk = 0.f;
    for (int i = 0; i < n; ++i) if (fabsf(x[i]) > pk) pk = fabsf(x[i]);
    const double amp = (double)pk * pow(10.0, db / 20.0) * sqrt(3.0);   /* uniform in [-amp, amp): rms = amp / sqrt 3 */
    for (int i = 0; i < n; ++i) {
        seed = seed * 1664525u + 1013904223u;
        out[i] = x[i] + (float)(amp * ((double)(seed >> 8) / 8388608.0 - 1.0));
    }
}

/* The reverberation time an ENERGY envelope e[0..n) has by the textbook rule, with no filter and no
 * noise: Schroeder's backward sum, then the least-squares line through it from hi_db down to lo_db,
 * scaled to 60 dB. The expectation for a decay that is not one exponential, where T20 (-5, -25), EDT
 * (0, -10) and T30 (-5, -35) are different numbers. */
static double ref_rt(const double* e, int n, double fs, double hi_db, double lo_db) {
    double tot = 0.0, acc = 0.0, sx = 0, sy = 0, sxx = 0, sxy = 0, m = 0;
    for (int i = 0; i < n; ++i) tot += e[i];
    for (int i = 0; i < n && tot > 0.0; ++i) {
        const double db = 10.0 * log10((tot - acc) / tot + 1e-300);
        acc += e[i];
        if (db < lo_db) break;
        if (db <= hi_db) { const double t = i / fs; sx += t; sy += db; sxx += t * t; sxy += t * db; m += 1; }
    }
    const double den = m * sxx - sx * sx;
    return den > 0.0 ? -60.0 * den / (m * sxy - sx * sy) : 0.0;
}

/* |H|^2 of the octave filter the report is specified to use, at f: 6th-order Butterworth edges at
 * fc / sqrt 2 and fc sqrt 2. The band-edge test's expectation, from the specification and not from
 * measure.c. */
static double octave_gain2(double f, double fc) {
    const double lo = fc / sqrt(2.0), hi = fc * sqrt(2.0);
    return 1.0 / (1.0 + pow(lo / f, 12.0)) / (1.0 + pow(f / hi, 12.0));
}

/* out[0..n) += the sound of x[0..nx) through a room whose impulse response is one mode (add_mode:
 * exp decay to -60 dB in rt, sin(w i + phase)), computed as the two-pole resonator that has exactly
 * that impulse response, so a 1.5 s sweep needs no long convolution. */
static void play_mode(const float* x, int nx, float* out, int n, double f, double rt, double phase, double fs) {
    const double r = exp(-6.9078 / (rt * fs)), w = TPI * f / fs;
    const double b0 = sin(phase), b1 = r * sin(w - phase), a1 = -2.0 * r * cos(w), a2 = r * r;
    double x1 = 0, y1 = 0, y2 = 0;
    for (int i = 0; i < n; ++i) {
        const double xn = i < nx ? x[i] : 0.0, yn = b0 * xn + b1 * x1 - a1 * y1 - a2 * y2;
        x1 = xn; y2 = y1; y1 = yn; out[i] += (float)yn;
    }
}

/* |H|^2 of a 6th-order Butterworth high-pass at f_hp into a 6th-order low-pass at f_lp, at f, as the
 * bilinear transform realizes it at fs (every frequency through tan(pi f / fs)): the exact response
 * the report's band filter is specified to have. */
static double butter6_gain2(double f, double f_hp, double f_lp, double fs) {
    const double w = tan(TPI * 0.5 * f / fs), a = tan(TPI * 0.5 * f_hp / fs), b = tan(TPI * 0.5 * f_lp / fs);
    return 1.0 / (1.0 + pow(a / w, 12.0)) / (1.0 + pow(w / b, 12.0));
}

int main(void) {
    const double fs = 48000.0, f1 = 20.0, f2 = 20000.0;
    const double band_hz[2] = { 300.0, 3000.0 };
    const int nref = 48000;                         /* 1 s sweep */
    float* sweep = (float*)malloc((size_t)nref * sizeof(float));
    CHECK(sweep != NULL, "alloc sweep");
    if (!sweep) return 1;
    measure_sweep(sweep, nref, f1, f2, fs);

    /* sweep is bounded and non-trivial */
    double e = 0; float amax = 0;
    for (int i = 0; i < nref; ++i) { e += (double)sweep[i]*sweep[i]; float a = fabsf(sweep[i]); if (a > amax) amax = a; }
    CHECK(amax > 0.5f && amax <= 1.0001f, "sweep is full-scale-ish and bounded");
    CHECK(e > 1.0, "sweep has energy");

    /* --- a known delay + gain: a clean delta system h = G*delta(D) --- */
    const int   D = 137;
    const float G = 0.5f;
    const int   ncap = nref + D + 4800;
    float* cap = (float*)calloc((size_t)ncap, sizeof(float));
    CHECK(cap != NULL, "alloc cap");
    if (cap) {
        for (int i = 0; i < nref; ++i) cap[i + D] = G * sweep[i];
        MeasureResult r;
        CHECK(measure_response(cap, ncap, sweep, nref, f1, f2, fs, band_hz, &r), "measure_response (delta)");
        printf("delta: delay=%d (want %d)  level=%.4f (want %.3f)  bands=[%.3f %.3f %.3f]\n",
               r.delay_samples, D, r.level, G, r.band[0], r.band[1], r.band[2]);
        CHECK(abs(r.delay_samples - D) <= 2, "recovers the delay within 2 samples");
        CHECK(fabs(r.level - G) / G < 0.1, "recovers the broadband gain within 10%");
        /* a flat system has roughly equal bands */
        CHECK(fabs(r.band[0] - r.band[2]) / r.band[0] < 0.2, "flat system: low ~ high band");

        /* --- the same, low-passed (~1 kHz one-pole): the high band must drop below the low band --- */
        float* capf = (float*)calloc((size_t)ncap, sizeof(float));
        CHECK(capf != NULL, "alloc capf");
        if (capf) {
            float y = 0.f; const float a = 0.12f;   /* one-pole LP, fc ~ 1 kHz at 48k */
            for (int i = 0; i < ncap; ++i) { y += a * (cap[i] - y); capf[i] = y; }
            MeasureResult r2;
            CHECK(measure_response(capf, ncap, sweep, nref, f1, f2, fs, band_hz, &r2), "measure_response (LP)");
            printf("lp:    delay=%d  bands=[%.3f %.3f %.3f]\n", r2.delay_samples, r2.band[0], r2.band[1], r2.band[2]);
            CHECK(r2.band[2] < 0.6f * r2.band[0], "low-pass: high band well below low band");
            free(capf);
        }
        free(cap);
    }
    /* --- sub-sample delay: a fractionally-delayed capture recovers the fractional part --- */
    {
        const int di = 137; const float frac = 0.4f;            /* true delay 137.4 samples */
        const int ncap2 = nref + di + 4800;
        float* capf = (float*)calloc((size_t)ncap2, sizeof(float));
        for (int i = 0; i < nref; ++i) { capf[di+i] += sweep[i]*(1.f-frac); capf[di+i+1] += sweep[i]*frac; }
        MeasureResult r;
        measure_response(capf, ncap2, sweep, nref, f1, f2, fs, band_hz, &r);
        double rec = r.delay_samples + r.delay_frac;
        printf("subsample: %d + %.3f = %.3f (want 137.40)\n", r.delay_samples, r.delay_frac, rec);
        CHECK(r.delay_frac > 0.1f, "sub-sample fraction points toward the true peak");
        CHECK(fabs(rec - 137.4) < 0.2, "sub-sample estimate well inside the 0.4-sample integer error");
        free(capf);
    }
    /* --- the direct-sound gate (level_direct / band_direct / direct_frac): a direct sound plus ONE
     *     reflection 3 ms later with a different spectrum, a one-pole HIGH-pass (~0.9 kHz) at 0.8 of
     *     the direct, so it adds to the mid and high bands and barely to the low one. The gate must
     *     close before the reflection, the gated means must match a reflection-free capture's, the
     *     whole-response means must NOT, and the direct share of the energy must land well inside
     *     (0, 1). A test that cannot fail is the default outcome: with the gate forced open to the
     *     whole IR, the gated-means checks and the direct_frac check went red, as they must. --- */
    {
        const int Dd = 300, R = 144;                            /* reflection 3 ms after the direct */
        const int ncap3 = nref + Dd + R + 9600;
        float* dry = (float*)calloc((size_t)ncap3, sizeof(float));
        float* wet = (float*)calloc((size_t)ncap3, sizeof(float));
        CHECK(dry && wet, "alloc gate captures");
        if (dry && wet) {
            float lp = 0.f;
            for (int i = 0; i < nref; ++i) {
                dry[Dd + i]  = 0.5f * sweep[i];
                wet[Dd + i] += 0.5f * sweep[i];                  /* += : the reflection is already there */
                lp += 0.12f * (sweep[i] - lp);                  /* one-pole LP; sweep - LP = the HP */
                wet[Dd + R + i] += 0.4f * (sweep[i] - lp);
            }
            MeasureResult rd, rw;
            CHECK(measure_response(dry, ncap3, sweep, nref, f1, f2, fs, band_hz, &rd), "measure_response (dry)");
            CHECK(measure_response(wet, ncap3, sweep, nref, f1, f2, fs, band_hz, &rw), "measure_response (wet)");
            #define DB(a, b) (20.0 * log10((double)(a) / (double)(b)))
            printf("gate: dry gate=%d frac=%.3f | wet gate=%d frac=%.3f\n",
                   rd.gate_samples, rd.direct_frac, rw.gate_samples, rw.direct_frac);
            printf("gate: wet vs dry, whole  level %+.2f  bands [%+.2f %+.2f %+.2f] dB\n",
                   DB(rw.level, rd.level), DB(rw.band[0], rd.band[0]), DB(rw.band[1], rd.band[1]), DB(rw.band[2], rd.band[2]));
            printf("gate: wet vs dry, gated  level %+.2f  bands [%+.2f %+.2f %+.2f] dB\n",
                   DB(rw.level_direct, rd.level), DB(rw.band_direct[0], rd.band[0]),
                   DB(rw.band_direct[1], rd.band[1]), DB(rw.band_direct[2], rd.band[2]));
            CHECK(rw.gate_samples > 48 && rw.gate_samples < R, "gate: closes after 1 ms and before the reflection");
            CHECK(rd.gate_samples == (int)(0.004 * fs), "gate: no reflection -> the 4 ms default");
            /* whole response: the reflection is in it, and it is in the treble more than the bass */
            CHECK(DB(rw.band[2], rd.band[2]) > 0.8, "whole-response high band includes the reflection");
            CHECK(DB(rw.band[1], rd.band[1]) > 0.5, "whole-response mid band includes the reflection");
            CHECK(DB(rw.band[2], rd.band[2]) > DB(rw.band[0], rd.band[0]) + 0.5,
                  "the reflection's own spectrum shows: the high band rises more than the low band");
            /* gated: the reflection is out (mid, high and the level band; the low band is 1/gate-smeared) */
            CHECK(fabs(DB(rw.band_direct[1], rd.band[1])) < 0.25, "gated mid band excludes the reflection");
            CHECK(fabs(DB(rw.band_direct[2], rd.band[2])) < 0.25, "gated high band excludes the reflection");
            CHECK(fabs(DB(rw.level_direct, rd.level)) < 0.25, "gated level excludes the reflection");
            CHECK(fabs(DB(rd.band_direct[2], rd.band[2])) < 0.25 && fabs(DB(rd.level_direct, rd.level)) < 0.25,
                  "an anechoic capture gates to its own whole response");
            /* the energy share: ~1 dry; about 1 / (1 + 0.64 x the HP's in-band power) wet */
            CHECK(rd.direct_frac > 0.97f, "direct_frac ~ 1 with no room");
            CHECK(rw.direct_frac > 0.45f && rw.direct_frac < 0.85f, "direct_frac sees the reflection's share");
            CHECK(rw.level > rd.level && rw.level_direct < rw.level, "the whole level is diluted, the gated one is not");
            #undef DB
        }
        free(dry); free(wet);
    }

    /* --- the expected-arrival window and the IR's SNR (measure_response_ex) ---
     * The speaker's sweep lands at D; an IMPOSTOR (the same sweep, 2x louder: a second speaker on a
     * mis-patched output) lands at D + 600. Searched over the whole IR the arrival is the impostor's;
     * inside a window around D it is the speaker's, and the capture is flagged. On a clean capture the
     * windowed and the whole-IR results are bit-identical. With the window and the outside flag gone
     * (exp_lo = exp_hi = 0) the first two checks go red. */
    {
        const int Dw = 900, ncw = nref + 24000;
        float* cw = (float*)calloc((size_t)ncw, sizeof(float));
        CHECK(cw != NULL, "alloc (window)");
        if (cw) {
            for (int i = 0; i < nref; ++i) cw[i + Dw] = 0.4f * sweep[i];
            MeasureResult a, b;
            CHECK(measure_response(cw, ncw, sweep, nref, f1, f2, fs, band_hz, &a), "clean, whole IR");
            CHECK(measure_response_ex(cw, ncw, sweep, nref, f1, f2, fs, band_hz, Dw - 70, Dw + 70, &b, NULL, 0, 0, NULL),
                  "clean, windowed");
            CHECK(a.delay_samples == b.delay_samples && a.delay_frac == b.delay_frac && a.level == b.level && !b.outside,
                  "window: a clean capture reads the same arrival and level to the bit, not flagged");
            CHECK(a.noise_n > 0 && a.snr_db > 60.f, "snr: a noise-free capture reads far over its floor");
            const float snr_clean = b.snr_db;
            for (int i = 0; i < nref; ++i) cw[i + Dw + 600] += 0.8f * sweep[i];
            CHECK(measure_response(cw, ncw, sweep, nref, f1, f2, fs, band_hz, &a), "impostor, whole IR");
            CHECK(measure_response_ex(cw, ncw, sweep, nref, f1, f2, fs, band_hz, Dw - 70, Dw + 70, &b, NULL, 0, 0, NULL),
                  "impostor, windowed");
            printf("window: whole-IR arrival %d, windowed %d (truth %d), outside %d by %.1f dB; clean snr %.1f dB\n",
                   a.delay_samples, b.delay_samples, Dw, b.outside, b.outside_db, snr_clean);
            CHECK(a.delay_samples == Dw + 600 && !a.outside, "no window: the louder impostor is taken, unflagged");
            CHECK(b.delay_samples == Dw && b.outside && b.peak_any == Dw + 600 && fabsf(b.outside_db - 6.02f) < 0.3f,
                  "window: the speaker's own arrival, flagged: a stronger tap 6 dB up outside the window");
            /* the first strong arrival, not the strongest: a reflection 150 samples after a direct sound
             * 2.5 dB under it (the rig's speakers 6, 12 and 15) */
            {
                float* ca = (float*)calloc((size_t)ncw, sizeof(float));
                CHECK(ca != NULL, "alloc (first arrival)");
                if (ca) {
                    const int Dr = Dw + 150, lo = Dw - 400, hi = Dw + 800;
                    MeasureResult fa, fb, fc;
                    for (int i = 0; i < nref; ++i) { ca[i + Dw] += 0.3f * sweep[i]; ca[i + Dr] += 0.4f * sweep[i]; }
                    measure_response_ex(ca, ncw, sweep, nref, f1, f2, fs, band_hz, lo, hi, &fa, NULL, 0, 0, NULL);
                    CHECK(fa.delay_samples == Dw && !fa.outside, "first arrival: the direct sound, not the louder reflection after it");
                    measure_response_pick(ca, ncw, sweep, nref, f1, f2, fs, band_hz, lo, hi, Dr - 20, Dr + 21, &fc, NULL, 0, 0, NULL);
                    CHECK(fc.delay_samples == Dr, "first arrival: a pick window names the arrival, with no walk back");
                    for (int i = 0; i < nref; ++i) ca[i + Dw] -= 0.26f * sweep[i];
                    measure_response_ex(ca, ncw, sweep, nref, f1, f2, fs, band_hz, lo, hi, &fb, NULL, 0, 0, NULL);
                    CHECK(fb.delay_samples == Dr, "first arrival: a tap 20 dB under the peak is not an arrival");
                    free(ca);
                }
            }
            /* a steady noise floor: the SNR falls with it, about 20 dB per decade of noise */
            for (int i = 0; i < nref; ++i) cw[i + Dw + 600] -= 0.8f * sweep[i];
            uint32_t sd = 12345u;
            MeasureResult n1, n2;
            float* cn = (float*)malloc((size_t)ncw * sizeof(float));
            CHECK(cn != NULL, "alloc (noise)");
            if (cn) {
                for (int k = 0; k < 2; ++k) {
                    const float amp = k ? 0.01f : 0.001f;
                    for (int i = 0; i < ncw; ++i) {
                        sd = sd * 1664525u + 1013904223u;
                        cn[i] = cw[i] + amp * ((float)(sd >> 8) / 8388608.f - 1.f);
                    }
                    measure_response_ex(cn, ncw, sweep, nref, f1, f2, fs, band_hz, Dw - 70, Dw + 70, k ? &n2 : &n1, NULL, 0, 0, NULL);
                }
                printf("snr: noise at 0.001 -> %.1f dB, at 0.01 -> %.1f dB\n", n1.snr_db, n2.snr_db);
                CHECK(n1.snr_db < snr_clean && fabsf((n1.snr_db - n2.snr_db) - 20.f) < 2.f,
                      "snr: ten times the noise is 20 dB less SNR");
                free(cn);
            }
            free(cw);
        }
    }

    /* --- the sweep's own distortion is not noise: a speaker with 20% second harmonic, arriving later than
     * the harmonic's lead (4816 samples for this 1 s sweep), puts the product at a POSITIVE lag inside the
     * "before" noise region. Counted, it costs the SNR about 30 dB (measured with the exclusion broken);
     * left out (MEASURE_HARM_*), the capture is as clean as it is. The harmonic is 2 s^2 - 1 = -cos(2 phase), faded out before it would alias. --- */
    {
        const int Dh = 6000, nch = nref + 24000;
        float* ch = (float*)calloc((size_t)nch, sizeof(float));
        CHECK(ch != NULL, "alloc (harmonic)");
        if (ch) {
            const int stop = (int)(0.85 * nref), fade = 2000;
            for (int i = 0; i < nref; ++i) {
                const double pi = 3.14159265358979;
                double h = i < stop ? 1.0 : (i < stop + fade ? 0.5 + 0.5 * cos(pi * (i - stop) / fade) : 0.0);
                if (i < fade) h *= 0.5 - 0.5 * cos(pi * i / fade);
                ch[i + Dh] = 0.4f * (sweep[i] + (float)(0.2 * h) * (2.f * sweep[i] * sweep[i] - 1.f));
            }
            MeasureResult hm;
            CHECK(measure_response_ex(ch, nch, sweep, nref, f1, f2, fs, band_hz, Dh - 70, Dh + 70, &hm, NULL, 0, 0, NULL),
                  "harmonic: measured");
            printf("harmonic: 20%% second harmonic %d samples ahead of the arrival: snr %.1f dB\n",
                   (int)measure_harmonic_lead(nref, 2, f1, f2), hm.snr_db);
            CHECK(hm.delay_samples == Dh && hm.snr_db > 60.f, "harmonic: the sweep's own second harmonic is not read as noise");
            free(ch);
        }
    }

    free(sweep);

    /* --- RT60: a synthetic exponential-decay tail with a known reverberation time. White noise under
     *     one envelope, so every octave band decays alike. The bands from 1 kHz up must read it too;
     *     below that one realization of band noise scatters too far to check (add_mode's comment). --- */
    {
        const int nir = 1000 + 16000;
        float* ir = (float*)calloc((size_t)nir, sizeof(float));
        const double rt60_target = 0.4;                         /* seconds */
        const double tau = rt60_target * fs / 6.908;            /* envelope time constant (samples) */
        unsigned seed = 12345u;
        for (int k = 0; k < nir - 1000; ++k) {
            seed = seed * 1103515245u + 12345u;
            double noise = ((double)((seed >> 16) & 0x7fff) / 16384.0) - 1.0;   /* white [-1,1) */
            ir[1000 + k] = (float)(exp(-(double)k / tau) * noise);
        }
        ir[1000] = 1.0f;                                        /* a clear direct spike */
        RoomResult rr;
        measure_rt60(ir, nir, 1000, fs, &rr);
        printf("rt60: %.3f s (target %.2f), range %.0f dB; bands", rr.rt60, rt60_target, rr.range_db);
        for (int b = 0; b < MEASURE_RT_BANDS; ++b) printf(" %.0f=%.3f/%.0f", measure_rt_band_hz[b], rr.band_rt60[b], rr.band_range_db[b]);
        printf("\n");
        CHECK(fabs(rr.rt60 - rt60_target) < 0.02, "Schroeder RT60 within 20 ms of the target decay");
        CHECK(rr.range_db >= 35.f, "a clean 0.33 s of a 0.4 s decay has the range for T20");
        for (int b = 4; b < MEASURE_RT_BANDS; ++b)
            CHECK(fabs(rr.band_rt60[b] - rt60_target) < 0.04, "white decay: each band from 1 kHz up reads the one decay");
        free(ir);
    }

    /* --- RT60 per octave band: a DIFFERENT decay in each band, one mode at each center from 125 Hz
     *     to 4 kHz. Profile 0 falls with frequency like a room (0.6 s at 125 Hz to 0.25 s at 4 kHz).
     *     Profile 1 turns it over, so the short decay (0.2 s) is in the low band and every band has a
     *     slower neighbor above it. Then the same IRs under a stationary noise floor 48 dB below the
     *     IR's peak, which the truncation, subtraction and compensation must take out, and under one
     *     20 dB below it, which leaves no band the range for T20.
     *     Broken on purpose (rt_decay integrating to the end of the IR: cut = n, noise = 0, comp = 0,
     *     the range left as found): under the -48 dB floor the 4 kHz band read 1.403 s for 0.25 and
     *     0.662 for 0.60, 2 kHz 0.339 for 0.30 and 0.528 for 0.50, and the broadband figure 0.764
     *     and 0.910 for 0.514 and 0.524. 9 checks red. The low bands hardly move (white noise puts
     *     little power in a narrow band), so they are not what proves the handling. --- */
    {
        enum { AT = 1000, NB = 6 };
        const int n = (int)(1.2 * fs), nir = AT + n;
        const double rt[2][NB] = { { 0.60, 0.50, 0.45, 0.40, 0.30, 0.25 }, { 0.20, 0.25, 0.30, 0.40, 0.50, 0.60 } };
        float* ir = (float*)calloc((size_t)nir, sizeof(float));
        float* nz = (float*)calloc((size_t)nir, sizeof(float));
        CHECK(ir && nz, "alloc (band decays)");
        for (int p = 0; ir && nz && p < 2; ++p) {
            for (int i = 0; i < nir; ++i) ir[i] = 0.f;
            for (int b = 0; b < NB; ++b) add_mode(ir, AT, n, measure_rt_band_hz[1 + b], rt[p][b], 0.7 * b, fs);
            RoomResult c, w, w2, d, v;
            measure_rt60(ir, nir, AT, fs, &c);
            add_floor(ir, nz, nir, -48.0, 777u); measure_rt60(nz, nir, AT, fs, &w);
            add_floor(ir, nz, nir, -20.0, 777u); measure_rt60(nz, nir, AT, fs, &d);
            add_floor(ir, nz, nir, -38.0, 777u); measure_rt60(nz, nir, AT, fs, &w2);
            printf("rt60 bands, profile %d (clean | floor -48 dB | floor -20 dB), s/range dB:\n", p);
            for (int b = 0; b < NB; ++b)
                printf("  %5.0f Hz  target %.2f | %.3f/%2.0f | %.3f/%2.0f | %.3f/%2.0f\n", measure_rt_band_hz[1 + b], rt[p][b],
                       c.band_rt60[1 + b], c.band_range_db[1 + b], w.band_rt60[1 + b], w.band_range_db[1 + b],
                       d.band_rt60[1 + b], d.band_range_db[1 + b]);
            printf("  broadband            | %.3f/%2.0f | %.3f/%2.0f | %.3f/%2.0f\n", c.rt60, c.range_db, w.rt60, w.range_db, d.rt60, d.range_db);
            for (int b = 0; b < NB; ++b) {
                const double tol = 0.04 * rt[p][b];
                CHECK(fabs(c.band_rt60[1 + b] - rt[p][b]) < tol, "band T20: each band recovers its own decay within 4% (clean)");
                CHECK(fabs(w.band_rt60[1 + b] - rt[p][b]) < tol, "band T20: ... and under a noise floor 48 dB down");
                CHECK(w.band_range_db[1 + b] >= 35.f && fabsf(w.band_range_db[1 + b] - w2.band_range_db[1 + b] - 10.f) < 1.5f,
                      "band range: enough for T20, and a floor 10 dB louder costs 10 dB of it");
                CHECK(d.band_rt60[1 + b] == 0.f && d.band_range_db[1 + b] > 0.f && d.band_range_db[1 + b] < 35.f,
                      "band range: a floor 20 dB down leaves no reading, and the range says why");
            }
            CHECK(c.rt60 > 0.f && fabs(w.rt60 - c.rt60) < 0.04 * c.rt60, "broadband: the noisy IR reads the clean one's figure");
            CHECK(d.rt60 == 0.f && d.range_db < 35.f, "broadband: no reading under the loud floor");
            /* the valid band: an octave whose edges leave [f_lo, f_hi] is not read */
            measure_rt60_ex(ir, nir, AT, fs, 100.0, 10000.0, &v);
            CHECK(v.band_rt60[0] == 0.f && v.band_range_db[0] == 0.f && v.band_rt60[1] == 0.f && v.band_range_db[1] == 0.f,
                  "valid band: 63 and 125 Hz reach below a 100 Hz low edge");
            CHECK(v.band_rt60[7] == 0.f && v.band_range_db[7] == 0.f, "valid band: 8 kHz reaches past a 10 kHz high edge");
            CHECK(v.band_rt60[2] == c.band_rt60[2] && v.band_rt60[6] == c.band_rt60[6] && c.band_rt60[2] > 0.f,
                  "valid band: the bands inside are untouched");
        }
        /* The octave filter's own ringing. It only shows on a decay shorter than the filter's, which
         * is shorter than the 0.2 s above: a 0.10 s mode at 63 Hz, then a 0.06 s one at 125 Hz, each
         * alone. Filtered backward in time both read true. Broken on purpose (filtered forward):
         * 0.0766 s and 0.0488 s, both checks red. */
        for (int k = 0; ir && k < 2; ++k) {
            const double target = k ? 0.06 : 0.10;
            for (int i = 0; i < nir; ++i) ir[i] = 0.f;
            add_mode(ir, AT, n, measure_rt_band_hz[k], target, 0.7, fs);
            RoomResult s;
            measure_rt60(ir, nir, AT, fs, &s);
            printf("rt60 ringing: %.0f Hz %.4f s (target %.2f)\n", measure_rt_band_hz[k], s.band_rt60[k], target);
            CHECK(fabs(s.band_rt60[k] - target) < 0.04 * target, "ringing: a decay shorter than the octave filter's reads true");
        }
        free(ir); free(nz);
    }

    /* --- RT60, each step of the noise handling on its own. The -48 dB floor above leaves 42 dB of
     *     range or more, where any one step can go missing unseen. Here one mode sits under a floor
     *     set to leave 36 to 38 dB, a little over the 35 dB gate, where a step is worth the most, and
     *     the tolerance is 1%. A floor 4 dB louder must then give NO reading: the two together hold the
     *     gate between 33 and 38 dB.
     *     Broken on purpose, one step at a time (what 0.40 s at 1 kHz and 0.60 s at 250 Hz read;
     *     whole, 0.399 and 0.603):
     *       no compensation            0.394, 0.596   red here
     *       no subtraction             0.416, 0.623   red here
     *       all three off              3.506, 1.310   red here
     *       no truncation alone        0.400, 0.606   NOT red here. Under a stationary floor the
     *                                  subtraction takes out what truncation would have. It is red on
     *                                  the ranges below (two slopes 49.0 dB for 47.1, no-floor, plateau).
     *       no late-slope fit, or      0.400, 0.603   NOT red here: one slope is its own late slope.
     *       one Lundeby pass           Red on the two-slope range below (48.1 and 48.3 dB for 47.1).
     *       no floor re-read           0.400, 0.605   NOT red anywhere. The re-read takes the floor
     *                                  from more of the IR, which lowers its scatter and not its mean,
     *                                  and one seed cannot show scatter. Unpinned, and said so.
     *       the gate at 32, 33, 38, 39 red (33 is the edge: one of the two cases). --- */
    {
        enum { AT = 1000 };
        const int n = (int)(1.2 * fs), nir = AT + n;
        const struct { int band; double rt, floor_db; } cs[2] = { { 4, 0.40, -25.0 }, { 2, 0.60, -19.5 } };
        float* ir = (float*)calloc((size_t)nir, sizeof(float));
        float* nz = (float*)calloc((size_t)nir, sizeof(float));
        CHECK(ir && nz, "alloc (noise steps)");
        for (int k = 0; ir && nz && k < 2; ++k) {
            const int b = cs[k].band;
            RoomResult lo, hi;
            for (int i = 0; i < nir; ++i) ir[i] = 0.f;
            add_mode(ir, AT, n, measure_rt_band_hz[b], cs[k].rt, 0.7, fs);
            add_floor(ir, nz, nir, cs[k].floor_db, 4321u);       measure_rt60(nz, nir, AT, fs, &lo);
            add_floor(ir, nz, nir, cs[k].floor_db + 4.0, 4321u); measure_rt60(nz, nir, AT, fs, &hi);
            printf("rt60 noise steps: %.0f Hz target %.2f: %.3f s / %.1f dB; floor 4 dB up: %.3f s / %.1f dB\n",
                   measure_rt_band_hz[b], cs[k].rt, lo.band_rt60[b], lo.band_range_db[b], hi.band_rt60[b], hi.band_range_db[b]);
            CHECK(lo.band_range_db[b] > 35.5f && lo.band_range_db[b] < 38.5f, "noise steps: the floor leaves 36 to 38 dB of range");
            CHECK(fabs(lo.band_rt60[b] - cs[k].rt) < 0.01 * cs[k].rt, "noise steps: T20 within 1% with the range near the gate");
            CHECK(hi.band_rt60[b] == 0.f && hi.band_range_db[b] > 31.5f && hi.band_range_db[b] < 35.f,
                  "gate: 4 dB less range is under 35 dB, and there is no reading");
        }

        /* A decay that never reaches a floor inside the window: 1.5 s at 1 kHz in 1.0 s of clean IR.
         * The last tenth is still the decay, and must not be subtracted as noise (which read 1.460 s
         * through measure_room). The range is then how far the decay got: 60 x 1.0 / 1.5 = 40 dB. */
        if (ir) {
            const int nw = (int)(1.0 * fs);
            RoomResult s;
            for (int i = 0; i < nir; ++i) ir[i] = 0.f;
            add_mode(ir, AT, n, 1000.0, 1.5, 0.7, fs);
            measure_rt60(ir, AT + nw, AT, fs, &s);
            printf("rt60 no floor in the window: 1 kHz %.4f s / %.1f dB (target 1.50, 40 dB)\n", s.band_rt60[4], s.band_range_db[4]);
            CHECK(fabs(s.band_rt60[4] - 1.5) < 0.01 * 1.5, "no floor: a decay cut off by the window reads true within 1%");
            CHECK(fabsf(s.band_range_db[4] - 40.f) < 1.5f, "no floor: the range is the decay the window holds");
        }

        /* --- T20 means -5 to -25 dB. A two-slope decay (one mode, a fast 0.2 s part over a slow
         *     0.8 s part 14 dB down) has a different EDT, T20 and T30, so the fit limits show.
         *     Broken on purpose (the clean figure, 0.5517 s whole): a fit from 0 dB 0.5015, to
         *     -35 dB (T30) 0.6687, to -15 dB (T10) 0.3802. Each red. --- */
        if (ir) {
            const double a2 = 0.2, r1 = 0.2, r2 = 0.8;
            double* e = (double*)malloc((size_t)n * sizeof(double));
            RoomResult s;
            for (int i = 0; i < nir; ++i) ir[i] = 0.f;
            for (int i = 0; e && i < n; ++i) {
                const double env = exp(-6.9078 * i / (r1 * fs)) + a2 * exp(-6.9078 * i / (r2 * fs));
                ir[AT + i] = (float)(env * sin(TPI * 1000.0 * i / fs + 0.7));
                e[i] = env * env;
            }
            if (e) {
                const double t20 = ref_rt(e, n, fs, -5.0, -25.0), t30 = ref_rt(e, n, fs, -5.0, -35.0);
                const double edt = ref_rt(e, n, fs, 0.0, -10.0), t0 = ref_rt(e, n, fs, 0.0, -25.0), t10 = ref_rt(e, n, fs, -5.0, -15.0);
                measure_rt60(ir, nir, AT, fs, &s);
                printf("rt60 two slopes: 1 kHz %.4f s; textbook T20 %.4f, T30 %.4f, EDT %.4f, 0..-25 %.4f, T10 %.4f\n",
                       s.band_rt60[4], t20, t30, edt, t0, t10);
                CHECK(fabs(t30 - t20) > 0.08 * t20 && fabs(t0 - t20) > 0.08 * t20 && fabs(t10 - t20) > 0.08 * t20,
                      "two slopes: the stimulus tells T20 from T30, T10 and a fit from 0 dB");
                CHECK(fabs(s.band_rt60[4] - t20) < 0.03 * t20, "two slopes: the figure is T20, -5 to -25 dB");
                /* The same decay under a floor. What crosses the floor is the SLOW part, so the
                 * crossing and the energy past it must come from the late slope: a line through
                 * the whole decay is too steep, puts the crossing early and the range high. The
                 * textbook range: the curve's level where the mode's in-band energy per sample
                 * (half the envelope squared, through the filter) falls to the floor's. */
                float pk = 0.f;
                for (int i = 0; i < nir; ++i) if (fabsf(ir[i]) > pk) pk = fabsf(ir[i]);
                const double fl_db = TWOSLOPE_FLOOR_DB, sig2 = pow((double)pk * pow(10.0, fl_db / 20.0), 2.0);
                double g = 0.0, tot = 0.0, acc = 0.0, want = 0.0;
                for (double f = 1.0; f < 0.5 * fs; f += 1.0) g += octave_gain2(f, 1000.0) / (0.5 * fs);
                for (int i = 0; i < n; ++i) tot += e[i];
                for (int i = 0; i < n; ++i) {
                    if (0.5 * octave_gain2(1000.0, 1000.0) * e[i] <= sig2 * g) { want = -10.0 * log10((tot - acc) / tot); break; }
                    acc += e[i];
                }
                RoomResult w;
                add_floor(ir, nz, nir, fl_db, 2468u);
                measure_rt60(nz, nir, AT, fs, &w);
                printf("rt60 two slopes under a floor: 1 kHz %.4f s / %.1f dB (textbook T20 %.4f, range %.1f dB)\n",
                       w.band_rt60[4], w.band_range_db[4], t20, want);
                CHECK(fabs(w.band_rt60[4] - t20) < 0.03 * t20, "two slopes under a floor: still T20");
                CHECK(fabs(w.band_range_db[4] - want) < 0.7, "two slopes under a floor: the range is where the SLOW part meets the floor");
            }
            free(e);
        }

        /* --- the octave band's edges. A fast 0.25 s mode at the 1 kHz center and a slow 0.5 s one
         *     0.38 octave off it, inside the band on either side (1300 Hz, then 769 Hz). The slow
         *     mode's weight in the band's curve is the filter's gain there, so the expected T20 is
         *     the textbook figure for the two energies weighted by the SPECIFIED filter. A band
         *     narrower than an octave drops the slow mode and reads short; a wider one reads long.
         *     Broken on purpose (0.4205 and 0.4200 s whole, 0.4183 expected): both edges at
         *     fc x 1.122 0.3353 and 0.3345; the top edge alone 0.3214 (1300 Hz); the bottom edge alone
         *     0.3212 (769 Hz); edges at fc / 2 and 2 fc 0.4365 and 0.4357. Each red. Order 4 reads
         *     0.4191 and 0.4181 and is NOT red here: the profile checks above are what hold the order. --- */
        for (int k = 0; ir && k < 2; ++k) {
            const double fe = k ? 1000.0 / 1.3 : 1300.0, ae = 0.5;
            double* e = (double*)malloc((size_t)n * sizeof(double));
            RoomResult s;
            for (int i = 0; i < nir; ++i) ir[i] = 0.f;
            add_mode(ir, AT, n, 1000.0, 0.25, 0.7, fs);
            for (int i = 0; i < n; ++i) ir[AT + i] += (float)(ae * exp(-6.9078 * i / (0.5 * fs)) * sin(TPI * fe * i / fs + 1.9));
            for (int i = 0; e && i < n; ++i)
                e[i] = octave_gain2(1000.0, 1000.0) * exp(-2.0 * 6.9078 * i / (0.25 * fs))
                     + octave_gain2(fe, 1000.0) * ae * ae * exp(-2.0 * 6.9078 * i / (0.5 * fs));
            if (e) {
                const double want = ref_rt(e, n, fs, -5.0, -25.0);
                measure_rt60(ir, nir, AT, fs, &s);
                printf("rt60 band edge: slow mode at %.0f Hz: 1 kHz band %.4f s (specified filter: %.4f)\n", fe, s.band_rt60[4], want);
                CHECK(fabs(s.band_rt60[4] - want) < 0.03 * want, "band edge: a mode 0.38 octave off center weighs what an octave filter gives it");
            }
            free(e);
        }

        /* --- inputs that must not hurt. A plateau fits a slope near zero, and a cast of 10 / slope
         *     to int read far outside the buffer. Two things now stand in the way: the fit's crossing
         *     lands past the window and the pass stops there, and the cast is clamped. With BOTH out
         *     (the code as first written) this case segfaults. With the clamp alone out it passes,
         *     because the crossing check bounds the slope first: the clamp is a second line and no
         *     input here reaches it. A non-finite sample is no measurement at all: range 0, never
         *     the 99 dB "not noise-limited" cap. The finite check on the energy sum is no longer what
         *     this case stands on: an Inf read 99 dB without it when it was written, and since the
         *     dead-curve exit an Inf reads 0 either way. The case holds the behavior. --- */
        if (ir) {
            RoomResult s;
            unsigned sp = 12345u;
            for (int i = 0; i < 48000; ++i) { sp = sp * 1664525u + 1013904223u; ir[i] = (float)(1e-3 * ((double)(sp >> 8) / 8388608.0 - 1.0)); }
            for (int i = 0; i < 4800; ++i) ir[i] = (float)sin(TPI * 1000.0 * i / fs);
            measure_rt60(ir, 48000, 0, fs, &s);
            printf("rt60 plateau: broadband %.3f s / %.0f dB\n", s.rt60, s.range_db);
            CHECK(s.rt60 == 0.f, "plateau: 0.1 s of steady tone is not a decay");
            for (int k = 0; k < 2; ++k) {
                for (int i = 0; i < 48000; ++i) ir[i] = 0.f;
                add_mode(ir, 0, 48000, 1000.0, 0.3, 0.7, fs);
                ir[5000] = k ? (float)NAN : (float)INFINITY;
                measure_rt60(ir, 48000, 0, fs, &s);
                int clean = s.rt60 == 0.f && s.range_db == 0.f;
                for (int b = 0; b < MEASURE_RT_BANDS; ++b) clean = clean && s.band_rt60[b] == 0.f && s.band_range_db[b] == 0.f;
                CHECK(clean, k ? "NaN in the IR: no reading and range 0 everywhere" : "Inf in the IR: no reading and range 0 everywhere");
            }
        }
        free(ir); free(nz);
    }

    /* --- measure_room: the same figures through a real 1.5 s sweep, 65 ms of latency and capture
     *     noise, the tools' own geometry. A room of seven modes, one per band from 63 Hz to 4 kHz,
     *     all 0.30 s, so every band and the broadband figure have one answer. What only this path
     *     has is the IR's EXTENT: past lag ncap - t(f) the capture ended before the sweep reached f,
     *     and the IR there holds neither decay nor noise. Read as the floor, that silence puts the
     *     truncation late and the figure long.
     *     Broken on purpose, at 30 dB capture SNR unless said (whole: broadband 0.301, 4 kHz 0.304):
     *       every analysis run to the capture's end    broadband 0.410, 2 kHz 0.333, 4 kHz 1.419
     *       the bands alone run to the end             2 kHz 0.333, 4 kHz 1.419
     *       the broadband IR not band-limited          no broadband reading: 30 dB of range at 50 dB
     *                                                  SNR (60 whole) and 4 dB at 30 dB SNR
     *     Each red. Before the band limit the broadband analysis also ended where only a band-limited
     *     IR ends, and read 0.56 s for this room's 0.30 at 50 dB SNR. --- */
    {
        enum { NSW = 72000, NCAP = 96000, LAT = 3120 };
        float* sw = (float*)malloc(NSW * sizeof(float));
        float* dry = (float*)calloc(NCAP, sizeof(float));
        float* cp = (float*)malloc(NCAP * sizeof(float));
        CHECK(sw && dry && cp, "alloc (measure_room)");
        if (sw && dry && cp) {
            measure_sweep(sw, NSW, 20.0, 20000.0, fs);
            for (int i = 0; i < NSW && LAT + i < NCAP; ++i) dry[LAT + i] = 8.f * sw[i];     /* the direct sound */
            for (int b = 0; b < 7; ++b) play_mode(sw, NSW, dry + LAT, NCAP - LAT, measure_rt_band_hz[b], 0.30, 0.7 * b, fs);
            double e = 0.0;
            for (int i = 0; i < NCAP; ++i) e += (double)dry[i] * dry[i];
            const double rms = sqrt(e / NCAP);
            for (int k = 0; k < 2; ++k) {
                const double snr = k ? 30.0 : 50.0, amp = rms * pow(10.0, -snr / 20.0) * sqrt(3.0);
                unsigned sn = 31337u;
                for (int i = 0; i < NCAP; ++i) {
                    sn = sn * 1664525u + 1013904223u;
                    cp[i] = dry[i] + (float)(amp * ((double)(sn >> 8) / 8388608.0 - 1.0));
                }
                RoomResult r;
                CHECK(measure_room(cp, NCAP, sw, NSW, 20.0, 20000.0, fs, &r, NULL, 0), "measure_room runs");
                printf("measure_room, capture SNR %.0f dB: broadband %.3f/%.0f, bands", snr, r.rt60, r.range_db);
                for (int b = 0; b < MEASURE_RT_BANDS; ++b) printf(" %.3f/%.0f", r.band_rt60[b], r.band_range_db[b]);
                printf("\n");
                CHECK(fabs(r.rt60 - 0.30) < 0.03 * 0.30, "measure_room: the broadband figure reads the room under capture noise");
                for (int b = 0; b < 7; ++b)
                    CHECK(fabs(r.band_rt60[b] - 0.30) < 0.03 * 0.30, "measure_room: every band from 63 Hz to 4 kHz reads the room");
                CHECK(r.band_rt60[7] == 0.f && r.band_range_db[7] == 0.f, "measure_room: 8 kHz is past the sweep's valid band");
                /* band-limited to the valid band, the broadband figure has range to spare at 50 dB;
                 * unfiltered it had under 30 */
                if (k == 0) CHECK(r.range_db > 50.f, "measure_room: the broadband range at 50 dB capture SNR");
            }
            /* The same capture with no room in it: the delayed sweep alone. The deconvolution's own
             * skirt follows the arrival, and it read as a 1 to 6 s decay in the low bands. No band and
             * no broadband figure may read. */
            {
                RoomResult r;
                for (int i = 0; i < NCAP; ++i) cp[i] = (i >= LAT && i - LAT < NSW) ? 8.f * sw[i - LAT] : 0.f;
                CHECK(measure_room(cp, NCAP, sw, NSW, 20.0, 20000.0, fs, &r, NULL, 0), "measure_room runs (anechoic)");
                int none = r.rt60 == 0.f && r.range_db == 0.f;
                for (int b = 0; b < MEASURE_RT_BANDS; ++b) none = none && r.band_rt60[b] == 0.f && r.band_range_db[b] == 0.f;
                if (!none) {
                    printf("measure_room, anechoic: broadband %.4f/%.0f, bands", r.rt60, r.range_db);
                    for (int b = 0; b < MEASURE_RT_BANDS; ++b) printf(" %.4f/%.0f", r.band_rt60[b], r.band_range_db[b]);
                    printf("\n");
                }
                CHECK(none, "measure_room: an anechoic capture reads nothing, range 0, in every band and broadband");
            }
        }
        free(sw); free(dry); free(cp);
    }

    /* --- the band filter's response, against the Butterworth formula: the impulse response of
     *     measure_rt_bandpass, its magnitude at the band center, both edges (-3.01 dB) and both
     *     neighbor centers (-18.1 dB), for the lowest band (where the poles crowd the unit circle)
     *     and a middle one. This holds the edges, the order and all three section Qs at once.
     *     Broken on purpose: the third section's Q at 0.707 is 11.0 dB off at an edge, red. What it
     *     does NOT show is a need for double precision: with the filter state in float the worst
     *     point is 0.0035 dB off at 63 Hz, and with float coefficients 0.0082 dB. Both pass. --- */
    {
        const int n = 1 << 17;
        double* x = (double*)malloc((size_t)n * sizeof(double));
        CHECK(x != NULL, "alloc (filter response)");
        for (int k = 0; x && k < 2; ++k) {
            const double fc = k ? 1000.0 : 63.0, lo = fc / sqrt(2.0), hi = fc * sqrt(2.0);
            const double at[5] = { fc, lo, hi, fc / 2.0, fc * 2.0 };
            for (int i = 0; i < n; ++i) x[i] = 0.0;
            x[0] = 1.0;
            measure_rt_bandpass(x, n, lo, hi, fs);
            double worst = 0.0;
            for (int j = 0; j < 5; ++j) {
                double re = 0, im = 0; const double w = TPI * at[j] / fs;
                for (int i = 0; i < n; ++i) { re += x[i] * cos(w * i); im -= x[i] * sin(w * i); }
                const double got = 10.0 * log10(re * re + im * im), want = 10.0 * log10(butter6_gain2(at[j], lo, hi, fs));
                if (fabs(got - want) > worst) worst = fabs(got - want);
                if (j == 3) printf("filter response, %.0f Hz band: %.3f dB at the lower neighbor's center (formula %.3f)", fc, got, want);
            }
            printf(", worst of five points %.4f dB off\n", worst);
            CHECK(worst < 0.02, "filter response: center, edges and neighbor centers within 0.02 dB of Butterworth 6");
        }
        free(x);
    }

    /* --- no decay, no reading. What follows the direct sound of an anechoic capture is not a room:
     *     a lone tap leaves nothing, a capsule cluster a millisecond, a deconvolution its skirt. Each
     *     had range to spare and printed a figure (5.8 s at 63 Hz through the simulated ZM-1). Every
     *     band and the broadband figure must read 0 with range 0, with and without a valid band.
     *     Broken on purpose, each rule out alone:
     *       the residue rule     the anechoic measure_room capture reads 0.371 s / 99 dB at 63 Hz, and
     *                            the tail 80 dB under the direct sound reads 0.300 s (both below). Red.
     *       the minimum decay    the tap cluster reads 0.0002 to 0.0005 s / 99 dB from 2 kHz up. Red.
     *       the dead-curve exit  the delta and the cluster read 99 dB of range broadband. Red. --- */
    {
        enum { PRE = 24000, NIR = 96000 };
        float* ir = (float*)calloc(NIR, sizeof(float));
        CHECK(ir != NULL, "alloc (no decay)");
        for (int k = 0; ir && k < 4; ++k) {
            /* 0: a bare delta. 1: 19 taps over 12 samples, a capsule mean's arrivals. 2: the delta
             * with a 0.3 s mode 80 dB under it at 1 kHz: a decay, but 38 dB under the direct sound's
             * own energy in that band, which is residue by the 30 dB rule. 3: the same mode 13 dB
             * louder, 25 dB under the direct sound in the band: a dead room, and it must READ. The
             * two hold the rule between 25 and 38 dB (at 20 dB case 3 reads nothing; at 50 dB case 2
             * reads 0.300 s). */
            for (int i = 0; i < NIR; ++i) ir[i] = 0.f;
            if (k == 1) { unsigned sd = 7u; for (int j = 0; j < 19; ++j) { sd = sd * 1664525u + 1013904223u; ir[PRE + (int)((sd >> 8) % 12u)] += 1.f / 19.f; } }
            else ir[PRE] = 1.f;
            if (k >= 2) add_mode(ir, PRE + 1, NIR - PRE - 1, 1000.0, 0.3, 0.7, fs);
            if (k >= 2) for (int i = PRE + 1; i < NIR; ++i) ir[i] *= (k == 2 ? 1e-4f : 4.5e-4f);
            for (int ex = 0; ex < 2; ++ex) {
                RoomResult s;
                if (ex) measure_rt60_ex(ir, NIR, PRE, fs, 40.0, 10000.0, &s); else measure_rt60(ir, NIR, PRE, fs, &s);
                int none = 1;
                for (int b = 0; b < MEASURE_RT_BANDS; ++b) none = none && s.band_rt60[b] == 0.f && s.band_range_db[b] == 0.f;
                if (k == 3) {
                    printf("a dead room, mode 67 dB under the direct (%s): 1 kHz %.3f/%.0f\n", ex ? "40 Hz-10 kHz" : "whole", s.band_rt60[4], s.band_range_db[4]);
                    CHECK(fabs(s.band_rt60[4] - 0.3) < 0.03 * 0.3, "a dead room: reverberation 25 dB under the direct sound in the band still reads");
                } else if (k == 2) {
                    /* only the 1 kHz band and the valid-band broadband are asked: the unfiltered
                     * broadband keeps the direct sample */
                    printf("no decay, mode 80 dB under the direct (%s): 1 kHz %.3f/%.0f, broadband %.3f/%.0f\n", ex ? "40 Hz-10 kHz" : "whole", s.band_rt60[4], s.band_range_db[4], s.rt60, s.range_db);
                    CHECK(s.band_rt60[4] == 0.f && s.band_range_db[4] == 0.f, "no decay: a tail 80 dB under the direct sound is residue, not a room");
                    if (ex) CHECK(s.rt60 == 0.f && s.range_db == 0.f, "no decay: ... for the band-limited broadband figure too");
                } else {
                    if (!none || s.rt60 != 0.f || s.range_db != 0.f) {
                        printf("no decay, case %d (%s): broadband %.4f/%.0f, bands", k, ex ? "40 Hz-10 kHz" : "whole", s.rt60, s.range_db);
                        for (int b = 0; b < MEASURE_RT_BANDS; ++b) printf(" %.4f/%.0f", s.band_rt60[b], s.band_range_db[b]);
                        printf("\n");
                    }
                    CHECK(none, k ? "no decay: a 12-sample cluster of taps reads nothing in any band" : "no decay: a bare delta reads nothing in any band");
                    CHECK(s.rt60 == 0.f && s.range_db == 0.f, k ? "no decay: ... nor broadband" : "no decay: ... nor broadband, and never 99 dB of range");
                }
            }
        }
        free(ir);
    }

    /* --- other sample rates. At 16 kHz the 8 kHz band's top edge (11.3 kHz) is past Nyquist: it
     *     must not be read, and nothing else may break. At 96 kHz all eight bands are read.
     *     The 0.45 fs skip itself is NOT pinned: with it out the 16 kHz case still passes, because a
     *     filter designed past Nyquist puts out NaN and the non-finite check drops the band. The skip
     *     says so on purpose; this case holds the behavior, by either route. --- */
    {
        const double fss[2] = { 16000.0, 96000.0 };
        for (int k = 0; k < 2; ++k) {
            const double f = fss[k];
            const int n = (int)(1.2 * f), nb = k ? 8 : 7;
            float* ir = (float*)calloc((size_t)n, sizeof(float));
            CHECK(ir != NULL, "alloc (sample rates)");
            if (!ir) continue;
            for (int b = 0; b < nb; ++b) add_mode(ir, 0, n, measure_rt_band_hz[b], 0.30, 0.7 * b, f);
            RoomResult s;
            measure_rt60(ir, n, 0, f, &s);
            printf("rt60 at fs %.0f: broadband %.3f, bands", f, s.rt60);
            for (int b = 0; b < MEASURE_RT_BANDS; ++b) printf(" %.3f/%.0f", s.band_rt60[b], s.band_range_db[b]);
            printf("\n");
            for (int b = 0; b < nb; ++b) CHECK(fabs(s.band_rt60[b] - 0.30) < 0.03 * 0.30, "sample rates: every band under 0.45 fs reads its mode");
            CHECK(fabs(s.rt60 - 0.30) < 0.03 * 0.30, "sample rates: the broadband figure");
            if (!k) CHECK(s.band_rt60[7] == 0.f && s.band_range_db[7] == 0.f, "sample rates: at 16 kHz the 8 kHz band is not read");
            free(ir);
        }
    }

    /* --- early reflections: a direct + two planted reflections at known delays/levels --- */
    {
        const int nir = 8000;
        float* ir = (float*)calloc((size_t)nir, sizeof(float));
        ir[2000] = 1.0f; ir[2000 + 300] = 0.4f; ir[2000 + 900] = 0.2f;
        RoomResult rr;
        measure_rt60(ir, nir, 2000, fs, &rr);
        printf("er: count=%d  d0=%d l0=%.2f  d1=%d l1=%.2f\n", rr.er_count,
               rr.er_count>0?rr.er_delay[0]:0, rr.er_count>0?rr.er_level[0]:0.f,
               rr.er_count>1?rr.er_delay[1]:0, rr.er_count>1?rr.er_level[1]:0.f);
        CHECK(rr.er_count >= 2, "found both planted reflections");
        CHECK(rr.er_delay[0] == 300 && fabs(rr.er_level[0] - 0.4f) < 0.01f, "reflection 1 at +300, level 0.4");
        CHECK(rr.er_delay[1] == 900 && fabs(rr.er_level[1] - 0.2f) < 0.01f, "reflection 2 at +900, level 0.2");
        free(ir);
    }

    /* --- per-speaker correction: a colored direct-sound IR (three peaking biquads) inverted by
     *     measure_correction must come out flat in-band. This is the speaker-EQ core. --- */
    {
        const int nir = 2048, gate = 1024, ntaps = 512;
        float* col = (float*)calloc((size_t)nir, sizeof(float));
        col[0] = 1.0f;                                  /* impulse -> the biquads color its response */
        peak_biquad(col, nir,  300.0, 1.5,  +5.0, fs);
        peak_biquad(col, nir, 1200.0, 2.0,  +9.0, fs);
        peak_biquad(col, nir, 5000.0, 1.5,  -7.0, fs);

        float* taps = (float*)calloc((size_t)ntaps, sizeof(float));
        int ok = measure_correction(col, nir, 0, gate, 20.0, 20000.0, fs, 6.0, 18.0, ntaps, taps);
        CHECK(ok, "measure_correction succeeds");

        const int ncor = nir + ntaps;
        float* cor = (float*)calloc((size_t)ncor, sizeof(float));   /* corrected = col * taps */
        for (int m = 0; m < ncor; ++m) {
            double s = 0;
            for (int k = 0; k < ntaps; ++k) { int idx = m - k; if (idx >= 0 && idx < nir) s += taps[k] * col[idx]; }
            cor[m] = (float)s;
        }

        double omin = 1e9, omax = 0, cmin = 1e9, cmax = 0;          /* in-band spread before vs after */
        for (int t = 0; t < 40; ++t) {
            double f = 120.0 * pow(12000.0 / 120.0, t / 39.0);      /* 120 .. 12000 Hz, log */
            double mo = mag_at(col, nir, f, fs), mc = mag_at(cor, ncor, f, fs);
            if (mo < omin) omin = mo; if (mo > omax) omax = mo;
            if (mc < cmin) cmin = mc; if (mc > cmax) cmax = mc;
        }
        double ospread = 20.0 * log10(omax / omin), cspread = 20.0 * log10(cmax / cmin);
        printf("eq: colored spread=%.1f dB -> corrected spread=%.1f dB\n", ospread, cspread);
        CHECK(ok && ospread > 10.0, "colored IR is genuinely non-flat (>10 dB in-band)");
        CHECK(ok && cspread < 3.0,  "minimum-phase correction flattens it to under 3 dB in-band");
        free(col); free(taps); free(cor);
    }

    /* --- static-listener room correction: the FD-window FIR sees (and cuts) a room resonance the
     *     direct gate misses, matches the gated view at HF, and the LF cut solver finds a mode. --- */
    {
        const int nir = 16384, gate = 192 /* 4 ms */, ntaps = 512;
        float* rm = (float*)calloc((size_t)nir, sizeof(float));
        rm[0] = 1.0f;                                   /* direct */
        peak_biquad(rm, nir,   80.0,  6.0, +12.0, fs);  /* an LF room mode: rings ~24 ms, way past the gate */
        peak_biquad(rm, nir,  500.0, 10.0, +10.0, fs);  /* a mid room resonance: rings ~6 ms, past the gate */

        /* LF modal cuts: the 80 Hz mode is found, as a CUT, with a sane Q; nothing else fires */
        MeasureEqSection cuts[8];
        int nc = measure_room_cuts(rm, nir, 0, fs, 30.0, 200.0, 12.0, 8, cuts);
        CHECK(nc >= 1, "room cuts: the 80 Hz mode is detected");
        int hit80 = 0;
        for (int s = 0; s < nc; ++s) {
            CHECK(cuts[s].gain_db < 0.f, "room cuts are cut-only (never a boost)");
            CHECK(cuts[s].q >= 1.f && cuts[s].q <= 12.f, "room cut Q is clamped sane");
            if (cuts[s].fc > 70.f && cuts[s].fc < 90.f && cuts[s].gain_db < -3.f) hit80 = 1;
        }
        CHECK(hit80, "a cut lands on the 80 Hz mode with meaningful depth");

        /* an anechoic IR yields no cuts (nothing above the smoothed baseline) */
        float* an = (float*)calloc((size_t)nir, sizeof(float));
        an[0] = 1.0f;
        CHECK(measure_room_cuts(an, nir, 0, fs, 30.0, 200.0, 12.0, 8, cuts) == 0,
              "an anechoic IR produces zero cuts");

        /* FD-window FIR vs the direct-gated FIR: normalize by their HF ratio (same gate there), then
         * the room FIR must attenuate the 500 Hz resonance noticeably more than the gated one. */
        float *tr = (float*)calloc((size_t)ntaps, sizeof(float));
        float *tg = (float*)calloc((size_t)ntaps, sizeof(float));
        CHECK(measure_correction_room(rm, nir, 0, gate, 6.0, 0.4, 200.0, 18000.0, fs, 3.0, 18.0, ntaps, tr),
              "measure_correction_room succeeds");
        CHECK(measure_correction(rm, nir, 0, gate, 200.0, 18000.0, fs, 3.0, 18.0, ntaps, tg),
              "gated correction succeeds (same band/caps)");
        double hf   = mag_at(tr, ntaps, 8000.0, fs) / mag_at(tg, ntaps, 8000.0, fs);
        double r500 = (mag_at(tr, ntaps, 500.0, fs) / mag_at(tg, ntaps, 500.0, fs)) / hf;
        printf("room-eq: FDW/gated at 8 kHz = %.2f, at 500 Hz (HF-normalized) = %.2f\n", hf, r500);
        CHECK(fabs(hf - 1.0) < 0.25, "FD window converges to the direct gate at HF");
        CHECK(r500 < 0.7, "FD window sees + cuts the room resonance the gate under-corrects");
        free(rm); free(an); free(tr); free(tg);
    }

    /* The room report's band coverage and its medians over speakers (bwa_calibrate --room and the
     * Capture tab print both), against hand-computed figures. */
    {
        const double sq2 = 1.41421356237309505;
        int ncov = 0, first = -1, last = -1;
        for (int b = 0; b < MEASURE_RT_BANDS; ++b)
            if (measure_rt_band_covered(measure_rt_band_hz[b], 2.0 * f1, 0.5 * f2, fs)) { ++ncov; if (first < 0) first = b; last = b; }
        CHECK(ncov == 7 && first == 0 && last == 6, "band coverage: a 20 Hz to 20 kHz sweep covers 63 Hz to 4 kHz, 8 kHz is out");
        CHECK(!measure_rt_band_covered(31.5, 40.0, 10000.0, fs), "band coverage: 31.5 Hz reaches under 40 Hz");
        CHECK(measure_rt_band_covered(40.0 * sq2 * (1.0 + 1e-9), 40.0, 10000.0, fs) &&
              !measure_rt_band_covered(40.0 * sq2 * (1.0 - 1e-9), 40.0, 10000.0, fs), "band coverage: the bottom edge sits on f_lo");
        CHECK(measure_rt_band_covered(10000.0 / sq2 * (1.0 - 1e-9), 40.0, 10000.0, fs) &&
              !measure_rt_band_covered(10000.0 / sq2 * (1.0 + 1e-9), 40.0, 10000.0, fs), "band coverage: the top edge sits on f_hi");
        CHECK(measure_rt_band_covered(8000.0, 40.0, 20000.0, 48000.0) && !measure_rt_band_covered(8000.0, 40.0, 20000.0, 24000.0) &&
              measure_rt_band_covered(8000.0, 40.0, 20000.0, 0.0), "band coverage: the top edge under 0.45 fs, no limit at fs 0");

        RoomResult rows[5];
        memset(rows, 0, sizeof rows);
        const float bb[5] = { 0.40f, 0.f, 0.30f, 0.50f, 0.35f };     /* 4 readings: (0.35 + 0.40) / 2 */
        const float b0[5] = { 0.30f, 0.30f, 0.10f, 0.30f, 0.f };     /* ties: 0.10 0.30 0.30 0.30 -> 0.30 */
        const float b2[5] = { 0.90f, 0.10f, 0.50f, 0.f, 0.f };       /* 3 readings, unsorted -> 0.50 */
        const float b3[5] = { -1.f, NAN, 0.42f, 0.f, 0.f };          /* a negative and a NaN are not readings */
        for (int i = 0; i < 5; ++i) {
            rows[i].rt60 = bb[i]; rows[i].band_rt60[0] = b0[i]; rows[i].band_rt60[2] = b2[i]; rows[i].band_rt60[3] = b3[i];
            rows[i].band_range_db[1] = 20.f;                         /* band 1: decay under the gate, no reading */
        }
        RoomSummary s;
        measure_room_summary(rows, 5, &s);
        printf("room summary: %d speakers, broadband %.4f (%d), 63 Hz %.4f (%d), 125 Hz %.4f (%d), 250 Hz %.4f (%d), 500 Hz %.4f (%d)\n",
               s.speakers, s.rt60, s.count, s.band_rt60[0], s.band_count[0], s.band_rt60[1], s.band_count[1],
               s.band_rt60[2], s.band_count[2], s.band_rt60[3], s.band_count[3]);
        CHECK(s.speakers == 5, "room summary: every row counts as a speaker");
        CHECK(s.count == 4 && fabsf(s.rt60 - 0.375f) < 1e-6f, "room summary: an even count takes the middle two, zeros left out");
        CHECK(s.band_count[0] == 4 && fabsf(s.band_rt60[0] - 0.30f) < 1e-6f, "room summary: ties");
        CHECK(s.band_count[1] == 0 && s.band_rt60[1] == 0.f, "room summary: a band nobody read is 0 s over 0, whatever its range");
        CHECK(s.band_count[2] == 3 && fabsf(s.band_rt60[2] - 0.50f) < 1e-6f, "room summary: an odd count takes the middle one");
        CHECK(s.band_count[3] == 1 && fabsf(s.band_rt60[3] - 0.42f) < 1e-6f, "room summary: a negative or NaN figure is no reading");
        CHECK(s.band_count[7] == 0 && s.band_rt60[7] == 0.f, "room summary: an empty band");
        measure_room_summary(NULL, 3, &s);
        CHECK(s.speakers == 0 && s.count == 0 && s.rt60 == 0.f, "room summary: no rows");
    }

    if (fails) { printf("measure_test: %d FAILURES\n", fails); return 1; }
    printf("measure_test OK (sweep, deconvolution, delay+gain, band tilt, direct-sound gate, RT60, early reflections, speaker EQ, room EQ, arrival window, IR SNR verified)\n");
    return 0;
}
