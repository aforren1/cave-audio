/* measure_test.c — the calibration measurement DSP, verified against a synthetic capture with a
 * known delay + gain + low-pass. Proves measure_response without the rig (the ASIO capture is the
 * only untested piece). */
#include "measure.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); ++fails; } } while (0)

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

    if (fails) { printf("measure_test: %d FAILURES\n", fails); return 1; }
    printf("measure_test OK (sweep, deconvolution, delay+gain, band tilt, RT60, early reflections verified)\n");
    return 0;
}
