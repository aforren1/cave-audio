/* arraysim.c: the array-sim room stage. See arraysim.h for what it models and why. */
#include "binaural/arraysim.h"
#include "core/bits.h"           /* bwa_pow2_ge */
#include "dsp/biquad.h"          /* bwa_biquad_rbj (the per-block coefficient rebuild) */
#include "core/profile.h"         /* zones (no-ops unless BWA_TRACY / BWA_PROFILE_SELF) */
#include "dsp/vec.h"             /* vec_biquad8 (the graphic EQ), vec_dot (the delay tap) */

#include <math.h>
#include <stdlib.h>
#include <string.h>


#define ASIM_SIZING_SOS   343.0f  /* ring sizing only; the live c scales the targets */
#define ASIM_GAIN_MIN     0.0625f /* -24 dB: a listener far outside the array */
#define ASIM_GAIN_MAX     8.0f    /* +18 dB: the 0.3 m floor against a large array's r0 */
#define ASIM_EQ_PROTO_DB  -12.0   /* the interaction matrix's prototype gain: the fit is linear in
                                   * each section's dB gain only approximately, and -12 dB sits in the
                                   * middle of the losses that matter (0 to 90 deg), where the error
                                   * of that approximation is smallest */
#define ASIM_EQ_BW_PER_SPACING 2.13  /* section bandwidth in octaves per octave of band spacing: 0.71
                                      * octave at third-octave spacing, 2.1 at octave spacing */
#define ASIM_PHASES      1024    /* delay-tap kernel phases: nearest phase is at most 1/2048 frame off */
#define ASIM_SINC_BETA   6.0     /* Kaiser beta: +0.03 / -0.00 dB to 16 kHz at 44.1 and 48 kHz */

struct ArraySim {
    uint32_t channels, rate, max_block;
    float    pos[BWA_CHANNELS][3], aim[BWA_CHANNELS][3];
    float    r0;                                   /* distance-gain reference: mean |ref - pos| */
    float    cmin;                                 /* delay reference: min |ref - pos| */
    /* distance gain */
    float    g_cur[BWA_CHANNELS], g_tgt[BWA_CHANNELS];
    /* propagation delay: one power-of-two ring per channel, one shared write index */
    uint32_t d_max, len, mask, w;
    double   d_cur[BWA_CHANNELS];                  /* double for the same reason as align.c's lc_dcur:
                                                    * a tiny per-sample creep on a value near d_max */
    float    d_tgt[BWA_CHANNELS];
    double   d_rate;                               /* max delay change per frame (closing speed / c) */
    float*   ring;                                 /* channels * (len + TAPS): each ring's first TAPS slots
                                                    * are mirrored past its end, so a kernel window is
                                                    * always one contiguous run (no per-tap wrap mask) */
    float*   sinc;                                 /* (ASIM_PHASES + 1) * ARRAYSIM_INTERP_TAPS kernels,
                                                    * stored tap-REVERSED: element u weights the window's
                                                    * u-th sample, oldest first */
    /* directivity graphic EQ (neq = 0: no model, stage 1 skipped) */
    uint32_t neq;
    uint8_t  eq_row[BWA_DIR_MAX_BANDS];            /* the model band each section fits */
    float    eq_hz[BWA_DIR_MAX_BANDS];
    float    eq_cw0[BWA_DIR_MAX_BANDS], eq_alpha[BWA_DIR_MAX_BANDS];
    float    eq_inv[BWA_DIR_MAX_BANDS][BWA_DIR_MAX_BANDS];   /* B^-1: table dB -> section dB */
    float    eq_slew;                              /* dB per frame */
    float    eq_gcur[BWA_CHANNELS][BWA_DIR_MAX_BANDS], eq_gtgt[BWA_CHANNELS][BWA_DIR_MAX_BANDS];
    float    eq_gco[BWA_CHANNELS][BWA_DIR_MAX_BANDS];        /* the gain eq_co was built for */
    float    eq_co[BWA_CHANNELS][BWA_DIR_MAX_BANDS][5];
    float    eq_st[BWA_CHANNELS][BWA_DIR_MAX_BANDS][4];      /* x1 x2 y1 y2 */
    uint8_t  eq_flat[BWA_CHANNELS][BWA_DIR_MAX_BANDS];       /* settled at 0 dB: identity, skippable */
    uint8_t  nang;
    float    ang_deg[BWA_DIR_MAX_ANGLES];
    float    loss_db[BWA_DIR_MAX_BANDS][BWA_DIR_MAX_ANGLES]; /* the fitted rows, copied at create */
    /* retarget bookkeeping */
    float    last_p[3], last_sos;
    int      primed;
    float*   scratch;                              /* channels * max_block: what the decoders read */
    float*   quad;                                 /* 8 * max_block: eight channels interleaved for the EQ */
};

/* ---- design (control thread) ---- */

/* the cookbook peaking EQ in double, from cw0/alpha/gain dB, and its dB magnitude at w */
static void peak_coef_d(double cw0, double alpha, double gain_db, double co[5]) {
    const double A = pow(10.0, gain_db / 40.0);
    const double a0 = 1.0 + alpha / A;
    co[0] = (1.0 + alpha * A) / a0; co[1] = -2.0 * cw0 / a0; co[2] = (1.0 - alpha * A) / a0;
    co[3] = -2.0 * cw0 / a0;        co[4] = (1.0 - alpha / A) / a0;
}
static double biquad_db_d(const double co[5], double w) {
    const double c1 = cos(w), c2 = cos(2.0 * w);
    const double num = co[0]*co[0] + co[1]*co[1] + co[2]*co[2] + 2.0*(co[0]*co[1] + co[1]*co[2])*c1 + 2.0*co[0]*co[2]*c2;
    const double den = 1.0 + co[3]*co[3] + co[4]*co[4] + 2.0*(co[3] + co[3]*co[4])*c1 + 2.0*co[4]*c2;
    return 10.0 * log10(num / den);
}

/* modified Bessel I0, for the Kaiser window */
static double bessel_i0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 64 && term > 1e-12 * sum; ++k) { term *= (x / (2.0 * k)) * (x / (2.0 * k)); sum += term; }
    return sum;
}

/* The delay tap's kernels: row p is the windowed sinc for fraction f = p / ASIM_PHASES, tap i
 * weighting the sample (i - (TAPS/2 - 1)) frames before the integer delay, normalized to unity DC,
 * then stored REVERSED (row[TAPS-1-i]) so the read walks the ring forward. Row 0 is an exact unit
 * impulse, so an integer delay passes the samples through bit for bit. */
static void sinc_design(float* tab) {
    const int N = ARRAYSIM_INTERP_TAPS, half = N / 2;
    const double i0b = bessel_i0(ASIM_SINC_BETA);
    for (int p = 0; p <= ASIM_PHASES; ++p) {
        float* row = tab + (size_t)p * N;
        if (p == 0) { for (int i = 0; i < N; ++i) row[N - 1 - i] = (i == half - 1) ? 1.f : 0.f; continue; }
        const double f = (double)p / ASIM_PHASES;
        double h[ARRAYSIM_INTERP_TAPS], sum = 0.0;
        for (int i = 0; i < N; ++i) {
            const double x = (double)(i - (half - 1)) - f;
            const double sn = fabs(x) < 1e-12 ? 1.0 : sin(M_PI * x) / (M_PI * x);
            const double r = x / (half + 0.5);
            const double w = bessel_i0(ASIM_SINC_BETA * sqrt(r * r < 1.0 ? 1.0 - r * r : 0.0)) / i0b;
            h[i] = sn * w; sum += h[i];
        }
        for (int i = 0; i < N; ++i) row[N - 1 - i] = (float)(h[i] / sum);
    }
}

/* Gauss-Jordan with partial pivoting: inv = A^-1 (n x n, row stride BWA_DIR_MAX_BANDS). 0 = singular. */
static int invert(double (*A)[BWA_DIR_MAX_BANDS], double (*inv)[BWA_DIR_MAX_BANDS], int n) {
    for (int i = 0; i < n; ++i) for (int j = 0; j < n; ++j) inv[i][j] = (i == j) ? 1.0 : 0.0;
    for (int c = 0; c < n; ++c) {
        int piv = c;
        for (int r = c + 1; r < n; ++r) if (fabs(A[r][c]) > fabs(A[piv][c])) piv = r;
        if (!(fabs(A[piv][c]) > 1e-12)) return 0;
        if (piv != c)
            for (int j = 0; j < n; ++j) {
                double t = A[c][j]; A[c][j] = A[piv][j]; A[piv][j] = t;
                t = inv[c][j]; inv[c][j] = inv[piv][j]; inv[piv][j] = t;
            }
        const double d = 1.0 / A[c][c];
        for (int j = 0; j < n; ++j) { A[c][j] *= d; inv[c][j] *= d; }
        for (int r = 0; r < n; ++r) {
            if (r == c || A[r][c] == 0.0) continue;
            const double f = A[r][c];
            for (int j = 0; j < n; ++j) { A[r][j] -= f * A[c][j]; inv[r][j] -= f * inv[c][j]; }
        }
    }
    return 1;
}

/* One peaking section per model band below 0.45 x the rate, bandwidth from the table's own band
 * spacing, then the interaction matrix: B[i][j] = section j's dB response at band center i when
 * set to the prototype gain, divided by that gain. Solving B g = t for the section gains g puts the
 * cascade on the target t at every band center, to the extent the sections' dB shapes scale
 * linearly with their gain. Returns the section count (0 = no EQ). */
static uint32_t eq_design(ArraySim* s, const Directivity* d) {
    uint32_t n = 0;
    for (uint32_t b = 0; b < d->nband && b < BWA_DIR_MAX_BANDS; ++b)
        if (d->band_hz[b] > 0.f && (double)d->band_hz[b] < 0.45 * (double)s->rate) {
            s->eq_row[n] = (uint8_t)b;
            s->eq_hz[n]  = d->band_hz[b];
            ++n;
        }
    if (n == 0 || d->nang < 2) return 0;
    double (*B)[BWA_DIR_MAX_BANDS]  = (double (*)[BWA_DIR_MAX_BANDS])calloc(BWA_DIR_MAX_BANDS, sizeof *B);
    double (*Bi)[BWA_DIR_MAX_BANDS] = (double (*)[BWA_DIR_MAX_BANDS])calloc(BWA_DIR_MAX_BANDS, sizeof *Bi);
    if (!B || !Bi) { free(B); free(Bi); return 0; }
    const double sp = n > 1 ? log2((double)s->eq_hz[n - 1] / (double)s->eq_hz[0]) / (double)(n - 1) : 1.0;
    double bw = ASIM_EQ_BW_PER_SPACING * sp;
    if (bw < 0.2) bw = 0.2; else if (bw > 3.0) bw = 3.0;
    double w0[BWA_DIR_MAX_BANDS], cw[BWA_DIR_MAX_BANDS], al[BWA_DIR_MAX_BANDS];
    for (uint32_t j = 0; j < n; ++j) {
        w0[j] = 2.0 * M_PI * (double)s->eq_hz[j] / (double)s->rate;
        cw[j] = cos(w0[j]);
        /* the cookbook's bandwidth-in-octaves alpha: it corrects for the bilinear warp, so a section
         * at 16 kHz keeps the same width in octaves as one at 1 kHz. The Q form narrows toward
         * Nyquist and left a 4.7 dB bump between the 12.5 and 16 kHz centers at 90 deg. */
        al[j] = sin(w0[j]) * sinh(0.5 * log(2.0) * bw * w0[j] / sin(w0[j]));
        s->eq_cw0[j]   = (float)cw[j];
        s->eq_alpha[j] = (float)al[j];
    }
    for (uint32_t j = 0; j < n; ++j) {
        double co[5];
        peak_coef_d(cw[j], al[j], ASIM_EQ_PROTO_DB, co);
        for (uint32_t i = 0; i < n; ++i) B[i][j] = biquad_db_d(co, w0[i]) / ASIM_EQ_PROTO_DB;
    }
    const int ok = invert(B, Bi, (int)n);
    if (ok)
        for (uint32_t i = 0; i < n; ++i)
            for (uint32_t j = 0; j < n; ++j) s->eq_inv[i][j] = (float)Bi[i][j];
    free(B); free(Bi);
    if (!ok) return 0;
    s->nang = d->nang;
    memcpy(s->ang_deg, d->ang_deg, sizeof s->ang_deg);
    for (uint32_t j = 0; j < n; ++j) memcpy(s->loss_db[j], d->loss_db[s->eq_row[j]], sizeof s->loss_db[j]);
    s->eq_slew = ARRAYSIM_EQ_SLEW_DB_S / (float)s->rate;
    return n;
}

ArraySim* arraysim_create(const Layout* L, uint32_t sample_rate, uint32_t max_block) {
    if (!L || L->count == 0 || L->count > BWA_CHANNELS || sample_rate == 0 || max_block == 0) return NULL;
    ArraySim* s = (ArraySim*)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->channels  = L->count;
    s->rate      = sample_rate;
    s->max_block = max_block;
    double sum = 0.0;
    float cmin = 1e30f, cmax = 0.f;
    for (uint32_t k = 0; k < L->count; ++k) {
        memcpy(s->pos[k], L->speakers[k].pos, sizeof s->pos[k]);
        memcpy(s->aim[k], L->speakers[k].aim, sizeof s->aim[k]);
        const float dx = L->ref[0] - s->pos[k][0], dy = L->ref[1] - s->pos[k][1], dz = L->ref[2] - s->pos[k][2];
        const float r = sqrtf(dx*dx + dy*dy + dz*dz);
        sum += r;
        if (r < cmin) cmin = r;
        if (r > cmax) cmax = r;
    }
    s->r0   = (float)(sum / L->count);
    if (!(s->r0 > ARRAYSIM_MIN_DIST_M)) s->r0 = ARRAYSIM_MIN_DIST_M;   /* a degenerate survey */
    s->cmin = cmin;
    s->d_max = (uint32_t)ceilf((cmax - cmin + ARRAYSIM_EXCURSION_M) / ASIM_SIZING_SOS * (float)sample_rate);
    /* the kernel reads from BASE - (TAPS/2 - 1) = 1 to BASE + TAPS/2 frames past the delay, plus the
     * next integer when the phase rounds up: d_max + TAPS + 2 slots hold every read */
    s->len   = bwa_pow2_ge(s->d_max + ARRAYSIM_INTERP_TAPS + 2);
    s->mask  = s->len - 1;
    s->d_rate = (double)(ARRAYSIM_CLOSING_MS / ASIM_SIZING_SOS);
    s->ring    = (float*)calloc((size_t)s->channels * (s->len + ARRAYSIM_INTERP_TAPS), sizeof(float));
    s->scratch = (float*)calloc((size_t)s->channels * max_block, sizeof(float));
    s->quad    = (float*)calloc((size_t)8 * max_block, sizeof(float));
    s->sinc    = (float*)malloc(sizeof(float) * (size_t)(ASIM_PHASES + 1) * ARRAYSIM_INTERP_TAPS);
    if (!s->ring || !s->scratch || !s->quad || !s->sinc) { arraysim_destroy(s); return NULL; }
    sinc_design(s->sinc);
    if (L->dir.nband) s->neq = eq_design(s, &L->dir);
    arraysim_reset(s);
    return s;
}

void arraysim_destroy(ArraySim* s) {
    if (!s) return;
    free(s->ring);
    free(s->scratch);
    free(s->quad);
    free(s->sinc);
    free(s);
}

void arraysim_reset(ArraySim* s) {
    if (!s) return;
    memset(s->ring, 0, sizeof(float) * (size_t)s->channels * (s->len + ARRAYSIM_INTERP_TAPS));
    memset(s->eq_st, 0, sizeof s->eq_st);
    for (uint32_t k = 0; k < s->channels; ++k) {
        s->g_cur[k] = s->g_tgt[k] = 1.f;
        s->d_cur[k] = 0.0; s->d_tgt[k] = 0.f;
        for (uint32_t j = 0; j < BWA_DIR_MAX_BANDS; ++j) {
            s->eq_gcur[k][j] = s->eq_gtgt[k][j] = 0.f;
            s->eq_gco[k][j]  = -1e9f;              /* no coefficients built yet */
        }
    }
    s->w = 0;
    s->primed = 0;
}

/* ---- audio thread ---- */


/* The fitted table rows at `ang` (linear in angle, clamped at the table's ends; NaN reads 0 deg):
 * exactly layout.c's directivity_loss_db_at at the band centers, with the log-frequency step
 * skipped because the sections sit ON the centers. */
static void eq_targets(const ArraySim* s, float ang, float* t) {
    const int last = (int)s->nang - 1;
    int i = 0; float f = 0.f;
    if (!(ang > 0.f))                 { i = 0; f = 0.f; }
    else if (ang >= s->ang_deg[last]) { i = last - 1; f = 1.f; }
    else {
        while (i < last - 1 && s->ang_deg[i + 1] <= ang) ++i;
        const float span = s->ang_deg[i + 1] - s->ang_deg[i];
        f = span > 0.f ? (ang - s->ang_deg[i]) / span : 0.f;
    }
    for (uint32_t j = 0; j < s->neq; ++j) t[j] = s->loss_db[j][i] + (s->loss_db[j][i + 1] - s->loss_db[j][i]) * f;
}

static void retarget(ArraySim* s, const float p[3], float sos) {
    const float k_frames = (float)s->rate / sos;
    for (uint32_t k = 0; k < s->channels; ++k) {
        const float dx = p[0] - s->pos[k][0], dy = p[1] - s->pos[k][1], dz = p[2] - s->pos[k][2];
        const float d = sqrtf(dx*dx + dy*dy + dz*dz);
        /* NaN-safe forms throughout: p is the rt core's sanitized pose, but a NaN here would index the
         * ring from a NaN cast and poison the gain ramp for good */
        float g = s->r0 / (d > ARRAYSIM_MIN_DIST_M ? d : ARRAYSIM_MIN_DIST_M);
        if (!(g > ASIM_GAIN_MIN)) g = ASIM_GAIN_MIN;
        if (g > ASIM_GAIN_MAX)    g = ASIM_GAIN_MAX;
        s->g_tgt[k] = g;
        float del = (d - s->cmin) * k_frames;
        if (!(del > 0.f)) del = 0.f;
        if (del > (float)s->d_max) del = (float)s->d_max;   /* past the stated excursion: clamp, never wrap */
        s->d_tgt[k] = del;
        if (s->neq) {
            float t[BWA_DIR_MAX_BANDS];
            eq_targets(s, directivity_off_axis_deg(s->pos[k], s->aim[k], p), t);
            for (uint32_t i = 0; i < s->neq; ++i) {
                const float* row = s->eq_inv[i];
                float acc = 0.f;
                for (uint32_t j = 0; j < s->neq; ++j) acc += row[j] * t[j];
                if (acc != acc)                   acc = 0.f;   /* NaN: flat, not a full cut */
                if (acc < -ARRAYSIM_EQ_MAX_DB)    acc = -ARRAYSIM_EQ_MAX_DB;
                if (acc > ARRAYSIM_EQ_MAX_DB)     acc = ARRAYSIM_EQ_MAX_DB;
                s->eq_gtgt[k][i] = acc;
            }
        }
    }
    s->d_rate = (double)(ARRAYSIM_CLOSING_MS / sos);
    memcpy(s->last_p, p, sizeof s->last_p);
    s->last_sos = sos;
}

/* Channel k's graphic-EQ bookkeeping for one block: each section's gain slews toward its target by
 * a per-block step (the tracked-room-EQ shape: coefficients rebuilt from the precomputed cw0/alpha
 * only when the gain moved). A section settled at 0 dB is FLAT: identity coefficients and a zeroed
 * state, so running it passes the samples through exactly (1 * x + 0 * finite). */
static void eq_prepare(ArraySim* s, uint32_t k, uint32_t n) {
    const float step = s->eq_slew * (float)n;
    for (uint32_t j = 0; j < s->neq; ++j) {
        float g = s->eq_gcur[k][j];
        const float t = s->eq_gtgt[k][j];
        if (g != t) {
            if      (g < t - step) g += step;
            else if (g > t + step) g -= step;
            else                   g  = t;
            s->eq_gcur[k][j] = g;
        }
        float* co = s->eq_co[k][j];
        if (g == 0.f && t == 0.f) {
            if (s->eq_gco[k][j] != 0.f) {
                co[0] = 1.f; co[1] = co[2] = co[3] = co[4] = 0.f;
                s->eq_gco[k][j] = 0.f;
            }
            float* st = s->eq_st[k][j];
            st[0] = st[1] = st[2] = st[3] = 0.f;
            s->eq_flat[k][j] = 1;
            continue;
        }
        s->eq_flat[k][j] = 0;
        if (g != s->eq_gco[k][j]) {
            bwa_biquad_rbj(BWA_BIQUAD_PEAK, (double)s->eq_cw0[j], (double)s->eq_alpha[j],
                           pow(10.0, (double)g / 40.0), co);
            s->eq_gco[k][j] = g;
        }
    }
}

/* One section over channels k0..k0+nl-1 (nl <= 8) at once, on s->quad (eight lanes interleaved,
 * frame by frame; lanes past nl are identity padding). The cascade is latency-bound on one channel,
 * so vec_biquad8 runs eight channels as independent chains: one channel per pass measured 459 us
 * per 256-frame block at 26 channels x 27 sections, four per vector 152 us. A FLAT lane holds
 * identity coefficients, passes its samples through exactly, and has its state re-zeroed after. */
static void eq_section8(ArraySim* s, uint32_t k0, uint32_t nl, uint32_t j, uint32_t n) {
    float c[5][8], st[4][8];
    for (uint32_t l = 0; l < 8; ++l) {
        if (l < nl) {
            const float* co = s->eq_co[k0 + l][j];
            const float* sv = s->eq_st[k0 + l][j];
            for (int q = 0; q < 5; ++q) c[q][l] = co[q];
            for (int q = 0; q < 4; ++q) st[q][l] = sv[q];
        } else {
            c[0][l] = 1.f; c[1][l] = c[2][l] = c[3][l] = c[4][l] = 0.f;
            st[0][l] = st[1][l] = st[2][l] = st[3][l] = 0.f;
        }
    }
    vec_biquad8(s->quad, n, (const float (*)[8])c, st);
    for (uint32_t l = 0; l < nl; ++l) {
        float* sv = s->eq_st[k0 + l][j];
        if (s->eq_flat[k0 + l][j]) { sv[0] = sv[1] = sv[2] = sv[3] = 0.f; continue; }
        for (int q = 0; q < 4; ++q) sv[q] = st[q][l];
    }
}

/* Stage 1 over the whole scratch, in place, eight channels at a time (the last group padded). */
static void eq_run(ArraySim* s, uint32_t n) {
    for (uint32_t k = 0; k < s->channels; ++k) eq_prepare(s, k, n);
    for (uint32_t k0 = 0; k0 < s->channels; k0 += 8) {
        const uint32_t nl = s->channels - k0 < 8 ? s->channels - k0 : 8;
        int any = 0;
        for (uint32_t j = 0; j < s->neq && !any; ++j)
            for (uint32_t l = 0; l < nl && !any; ++l) any = !s->eq_flat[k0 + l][j];
        if (!any) continue;                                        /* every channel flat: identity */
        float* v = s->quad;
        for (uint32_t l = 0; l < 8; ++l) {
            if (l < nl) { const float* x = s->scratch + (size_t)(k0 + l) * n; for (uint32_t i = 0; i < n; ++i) v[8*i + l] = x[i]; }
            else        { for (uint32_t i = 0; i < n; ++i) v[8*i + l] = 0.f; }
        }
        for (uint32_t j = 0; j < s->neq; ++j) {
            int live = 0;
            for (uint32_t l = 0; l < nl && !live; ++l) live = !s->eq_flat[k0 + l][j];
            if (live) eq_section8(s, k0, nl, j, n);
        }
        for (uint32_t l = 0; l < nl; ++l) { float* x = s->scratch + (size_t)(k0 + l) * n; for (uint32_t i = 0; i < n; ++i) x[i] = v[8*i + l]; }
    }
}

const float* arraysim_apply(ArraySim* s, const float* bus, const float p[3], float sos, uint32_t n) {
    if (!s || !bus || !p || n == 0 || n > s->max_block) return bus;
    if (!(sos >= 100.f && sos <= 1000.f)) sos = 343.f;
    const float dx = p[0] - s->last_p[0], dy = p[1] - s->last_p[1], dz = p[2] - s->last_p[2];
    if (!s->primed || !(dx*dx + dy*dy + dz*dz < 1e-4f) || sos != s->last_sos)   /* > 1 cm (or NaN) */
        retarget(s, p, sos);
    if (!s->primed) {                               /* nothing to glide from: land on the targets */
        for (uint32_t k = 0; k < s->channels; ++k) {
            s->g_cur[k] = s->g_tgt[k];
            s->d_cur[k] = (double)s->d_tgt[k];
            memcpy(s->eq_gcur[k], s->eq_gtgt[k], sizeof s->eq_gcur[k]);
        }
        s->primed = 1;
    }
    const uint32_t len = s->len, mask = s->mask, w = s->w;
    const double rate = s->d_rate;
    const float inv_n = 1.f / (float)n;
    BWA_ZONE_BEGIN(zas, "array-sim room");
    memcpy(s->scratch, bus, sizeof(float) * (size_t)s->channels * n);
    if (s->neq) {                                                  /* 1. directivity */
        BWA_ZONE_BEGIN(zeq, "array-sim eq");
        eq_run(s, n);
        BWA_ZONE_END(zeq);
    }
    for (uint32_t k = 0; k < s->channels; ++k) {
        float* x = s->scratch + (size_t)k * n;
        float g = s->g_cur[k];                                     /* 2. distance gain, per-sample ramp */
        const float dg = (s->g_tgt[k] - g) * inv_n;
        float* h = s->ring + (size_t)k * (len + ARRAYSIM_INTERP_TAPS);   /* 3. propagation delay */
        double dc = s->d_cur[k];
        const double dt = (double)s->d_tgt[k];
        const float* tab = s->sinc;
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t wi = (w + i) & mask;
            const float in = x[i] * g;
            h[wi] = in;
            if (wi < ARRAYSIM_INTERP_TAPS) h[len + wi] = in;      /* the mirror a window near the end reads */
            g += dg;
            /* the windowed-sinc tap at dc + BASE: dc in [0, d_max], so the newest sample the kernel
             * reads is BASE - (TAPS/2 - 1) = 1 frame back, never the one just written */
            const double   dl = dc + (double)ARRAYSIM_BASE_FRAMES;
            uint32_t       di = (uint32_t)dl;
            uint32_t       ph = (uint32_t)((dl - (double)di) * ASIM_PHASES + 0.5);
            if (ph >= ASIM_PHASES) { ph = 0; ++di; }               /* rounds to the next integer delay */
            const float*   kr = tab + (size_t)ph * ARRAYSIM_INTERP_TAPS;
            /* the window's oldest sample: tap 0 weights (wi - di + TAPS/2 - 1), the last tap TAPS-1
             * frames before it, and the mirrored tail keeps the whole window contiguous */
            const uint32_t b = (wi - di + (ARRAYSIM_INTERP_TAPS / 2 - 1) - (ARRAYSIM_INTERP_TAPS - 1)) & mask;
            x[i] = vec_dot(kr, h + b, ARRAYSIM_INTERP_TAPS);
            if      (dc < dt - rate) dc += rate;                  /* the creep IS the Doppler */
            else if (dc > dt + rate) dc -= rate;
            else                     dc  = dt;
        }
        s->g_cur[k] = s->g_tgt[k];                                 /* land exactly */
        s->d_cur[k] = dc;
    }
    s->w = (w + n) & mask;
    BWA_ZONE_END(zas);
    return s->scratch;
}

void arraysim_state(const ArraySim* s, float* delay_frames, float* gain_lin) {
    if (!s) return;
    for (uint32_t k = 0; k < s->channels; ++k) {
        if (delay_frames) delay_frames[k] = (float)s->d_cur[k];
        if (gain_lin)     gain_lin[k]     = s->g_cur[k];
    }
}

uint32_t arraysim_eq_bands(const ArraySim* s, float* hz) {
    if (!s) return 0;
    if (hz) memcpy(hz, s->eq_hz, sizeof(float) * s->neq);
    return s->neq;
}

void arraysim_eq_gains(const ArraySim* s, uint32_t k, float* gain_db) {
    if (!s || !gain_db || k >= s->channels) return;
    memcpy(gain_db, s->eq_gcur[k], sizeof(float) * s->neq);
}

uint32_t arraysim_max_delay_frames(const ArraySim* s) { return s ? s->d_max : 0; }

size_t arraysim_bytes(const ArraySim* s) {
    if (!s) return 0;
    return sizeof *s + sizeof(float) * ((size_t)s->channels * (s->len + ARRAYSIM_INTERP_TAPS + s->max_block) + (size_t)8 * s->max_block
                                        + (size_t)(ASIM_PHASES + 1) * ARRAYSIM_INTERP_TAPS);
}
