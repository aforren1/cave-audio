/*
 * biquad.h — RBJ "Audio EQ Cookbook" biquad coefficients, a0-normalized, Direct Form I.
 *
 * out = {b0, b1, b2, a1, a2}; apply as:  y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2
 * (the sign convention rt.c's transmission/pathing EQ and align.c's room_eq cuts both use).
 * Pure math, no allocation — safe on the audio thread. Shared by the occlusion/pathing spectral
 * tilt (rt.c) and the LF modal cuts (align.c), which had independently grown the same cookbook.
 *
 * Computes in double, stores float: coefficient precision is free here (this never runs per sample —
 * rt.c calls it per block as the band gain glides, align.c once per section at create).
 */
#ifndef BWA_BIQUAD_H
#define BWA_BIQUAD_H

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum { BWA_BIQUAD_LOWSHELF = 0, BWA_BIQUAD_PEAK = 1, BWA_BIQUAD_HIGHSHELF = 2 };

/* Core: coefficients from precomputed cw0 = cos(w0), alpha = sin(w0)/(2Q), and A = sqrt(linear gain).
 * rt.c precomputes cw0/alpha once per sample rate (fixed fc) so only A varies per update — the trig
 * stays off the hot path, which is why this takes them ready-made rather than deriving from fc/Q. */
static inline void bwa_biquad_rbj(int type, double cw0, double alpha, double A, float out[5]) {
    double b0, b1, b2, a0, a1, a2;
    if (type == BWA_BIQUAD_PEAK) {
        a0 = 1.0 + alpha / A; a1 = -2.0 * cw0;        a2 = 1.0 - alpha / A;
        b0 = 1.0 + alpha * A; b1 = -2.0 * cw0;        b2 = 1.0 - alpha * A;
    } else {
        double t = 2.0 * sqrt(A) * alpha;
        if (type == BWA_BIQUAD_LOWSHELF) {
            a0 =  (A + 1) + (A - 1) * cw0 + t;
            a1 = -2.0 * ((A - 1) + (A + 1) * cw0);
            a2 =  (A + 1) + (A - 1) * cw0 - t;
            b0 =  A * ((A + 1) - (A - 1) * cw0 + t);
            b1 =  2.0 * A * ((A - 1) - (A + 1) * cw0);
            b2 =  A * ((A + 1) - (A - 1) * cw0 - t);
        } else { /* BWA_BIQUAD_HIGHSHELF */
            a0 =  (A + 1) - (A - 1) * cw0 + t;
            a1 =  2.0 * ((A - 1) - (A + 1) * cw0);
            a2 =  (A + 1) - (A - 1) * cw0 - t;
            b0 =  A * ((A + 1) + (A - 1) * cw0 + t);
            b1 = -2.0 * A * ((A - 1) + (A + 1) * cw0);
            b2 =  A * ((A + 1) + (A - 1) * cw0 - t);
        }
    }
    double inv = 1.0 / a0;
    out[0] = (float)(b0 * inv); out[1] = (float)(b1 * inv); out[2] = (float)(b2 * inv);
    out[3] = (float)(a1 * inv); out[4] = (float)(a2 * inv);
}

/* Convenience for off-hot-path design: derive (cw0, alpha, A) from (fc, Q, gain_db, fs) then solve.
 * A = 10^(gain_db/40) = sqrt(linear power gain), per the cookbook. */
static inline void bwa_biquad_rbj_hz(int type, double fc, double q, double gain_db, double fs, float out[5]) {
    double w0 = 2.0 * M_PI * fc / fs;
    bwa_biquad_rbj(type, cos(w0), sin(w0) / (2.0 * q), pow(10.0, gain_db / 40.0), out);
}

#endif /* BWA_BIQUAD_H */
