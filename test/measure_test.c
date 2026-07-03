/* measure_test.c — the calibration measurement DSP, verified against a synthetic capture with a
 * known delay + gain + low-pass. Proves measure_response without the rig (the ASIO capture is the
 * only untested piece). */
#include "measure.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); ++fails; } } while (0)

#define TPI 6.283185307179586

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

    free(sweep);

    /* --- RT60: a synthetic exponential-decay tail with a known reverberation time --- */
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
        printf("rt60: %.3f s (target %.2f)\n", rr.rt60, rt60_target);
        CHECK(fabs(rr.rt60 - rt60_target) < 0.08, "Schroeder RT60 within 80 ms of the target decay");
        free(ir);
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

    if (fails) { printf("measure_test: %d FAILURES\n", fails); return 1; }
    printf("measure_test OK (sweep, deconvolution, delay+gain, band tilt, RT60, early reflections, speaker EQ, room EQ verified)\n");
    return 0;
}
