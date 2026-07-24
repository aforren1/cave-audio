/*
 * fdn.c — see fdn.h. Structure per sample:
 *
 *   read each line's delayed sample -> per-line 2-band decay filter (one-pole crossover between the
 *   low/high band gains, both derived from that line's length + its DIRECTION-scaled decay times)
 *   -> line outputs y[16] -> bus += gain * Dcomb[26][16] * y (each line rendered as a plane wave
 *   from its Fibonacci-sphere direction) -> Householder feedback (I - 2/N J, orthogonal = lossless
 *   prototype; the decay filters are the only loss) + the aux input (alternating-sign injection)
 *   -> write back into the delay lines.
 *
 * Line lengths are co-prime-ish (odd, spread 23..90 ms) so modes don't stack. Everything is sized
 * at create; the tap does no allocation. Cost ~ (16 filters + 32 feedback ops + 26x16 render MACs)
 * per sample ~= 0.5 MAC/sample/output-channel — comparable to one voice.
 */
#include "fdn.h"
#include "allrad.h"
#include "epad.h"
#include "ambisonics.h"
#include "bits.h"          /* bwa_pow2_ge */

#include <math.h>
#include <stdatomic.h>     /* wet gain: control thread writes it live, the tap reads it (like steam_reflect) */
#include <stdlib.h>
#include <string.h>

#define FDN_N 16

static const float fdn_ms[FDN_N] = {   /* line delays (ms); odd-sample rounding keeps them co-prime-ish */
    23.0f, 27.7f, 31.9f, 36.7f, 41.3f, 46.1f, 50.9f, 55.1f,
    59.3f, 63.1f, 67.9f, 71.3f, 76.7f, 81.1f, 85.7f, 89.9f
};

struct Fdn {
    uint32_t channels, sample_rate;
    uint32_t len[FDN_N];          /* delay in samples (odd) */
    uint32_t rlen[FDN_N], rmask[FDN_N];
    float*   ring[FDN_N];         /* one slice each of `mem` */
    float*   mem;
    uint32_t w;                   /* shared sample counter (each ring indexes it modulo its own pow2) */
    float    dir[FDN_N][3];       /* line directions, room axes (Fibonacci sphere) */
    float    dcomb[BWA_CHANNELS][FDN_N];   /* line -> speaker render (bed decode of each line's plane wave) */
    float    dcomb_re[BWA_CHANNELS][FDN_N];/* the same render with max-rE tapered encodes (fdn_set_max_re) */
    _Atomic int max_re;           /* live A/B target; the tap crossfades re_cur toward it */
    float    re_cur;              /* audio-thread crossfade state (0 = dcomb .. 1 = dcomb_re) */
    float    in_g[FDN_N];         /* aux injection gains (alternating sign, 1/sqrt(N)) */
    /* per-line decay filter: y = g_hf*x + (g_lf - g_hf)*lp, lp += a*(x - lp) */
    float    g_lf[FDN_N], g_hf[FDN_N], lp[FDN_N], xa;
    /* design parameters (kept to re-derive the gains when either setter runs) */
    float    rt_low, rt_high, xover_hz;
    float    ddir[3], dfactor;    /* anisotropy: decay-time scale toward ddir */
    _Atomic float gain;           /* wet return level target (written live by the control thread) */
    float    gain_cur;            /* per-block ramp state (audio-thread-only) */
};

/* Line directions use the shared fib_sphere_dir (ambisonics.h) — same float construction as allrad.c. */

/* re-derive every line's band gains from (rt_low, rt_high) x the direction scale:
 * g = 10^(-3 * len / (rt60_eff * fs)), the standard FDN loss for a target decay. */
static void fdn_gains(Fdn* f) {
    for (int l = 0; l < FDN_N; ++l) {
        float wdir = 0.5f + 0.5f * (f->dir[l][0]*f->ddir[0] + f->dir[l][1]*f->ddir[1] + f->dir[l][2]*f->ddir[2]);
        float scale = 1.f + (f->dfactor - 1.f) * wdir;          /* 1 away from ddir .. factor toward it */
        float rlo = f->rt_low  * scale, rhi = f->rt_high * scale;
        f->g_lf[l] = powf(10.f, -3.f * (float)f->len[l] / (rlo * (float)f->sample_rate));
        f->g_hf[l] = powf(10.f, -3.f * (float)f->len[l] / (rhi * (float)f->sample_rate));
    }
    f->xa = 1.f - expf(-6.2831853f * f->xover_hz / (float)f->sample_rate);
}

Fdn* fdn_create(const Layout* L, uint32_t sample_rate, uint32_t channels, int bed_decoder) {
    if (!L || sample_rate == 0 || channels == 0 || channels > BWA_CHANNELS) return NULL;
    Fdn* f = (Fdn*)calloc(1, sizeof *f);
    if (!f) return NULL;
    f->channels = channels; f->sample_rate = sample_rate;
    size_t total = 0;
    for (int l = 0; l < FDN_N; ++l) {
        uint32_t len = (uint32_t)(fdn_ms[l] * 1e-3f * (float)sample_rate) | 1u;   /* odd */
        f->len[l] = len;
        uint32_t rl = bwa_pow2_ge(len + 1);
        f->rlen[l] = rl; f->rmask[l] = rl - 1;
        total += rl;
    }
    f->mem = (float*)calloc(total, sizeof(float));
    if (!f->mem) { free(f); return NULL; }
    size_t at = 0;
    for (int l = 0; l < FDN_N; ++l) { f->ring[l] = f->mem + at; at += f->rlen[l]; }

    /* line -> speaker render: each line is a plane wave from its direction, decoded through the
     * layout's bed decode. `bed_decoder` is the rt-internal id engine.c maps from the public enum:
     * 2 = EPAD renders the lines through the same energy-preserving decode the beds use; anything
     * else keeps the FDN's house AllRAD (the sampling form below is only the non-triangulable
     * fallback, mirroring rt.c's build_bed_decode fallback — not selectable). */
    float dec[BWA_CHANNELS][BWA_AMBI_CH];
    int built = 0;
    if (bed_decoder == 2) built = epad_build_decode(L, dec);
    if (!built)           built = allrad_build_decode(L, dec);
    if (!built) ambi_sad_decode(L, L->count, dec);              /* shared sampling decode (degenerate-array fallback) */
    float rw[BWA_AMBI_CH];
    ambi_max_re_weights(BWA_AMBI_ORDER, rw);                    /* the lines encode at full order */
    for (int l = 0; l < FDN_N; ++l) {
        fib_sphere_dir(l, FDN_N, f->dir[l]);
        float ad[3]; room_to_ambi(f->dir[l], ad);                     /* room -> ambi (z,x,y) */
        float y[BWA_AMBI_CH]; ambi_encode_sn3d(ad, y);
        for (uint32_t s = 0; s < channels; ++s) {
            float acc = 0.f, acr = 0.f;
            for (int k = 0; k < BWA_AMBI_CH; ++k) { acc += dec[s][k] * y[k]; acr += dec[s][k] * y[k] * rw[k]; }
            f->dcomb[s][l]    = acc;
            f->dcomb_re[s][l] = acr;                            /* the max-rE render of the same line */
        }
        f->in_g[l] = ((l & 1) ? 1.f : -1.f) * 0.25f;            /* 1/sqrt(N), alternating sign */
    }

    f->rt_low = 1.2f; f->rt_high = 0.7f; f->xover_hz = 2000.f;
    f->ddir[0] = f->ddir[1] = f->ddir[2] = 0.f; f->dfactor = 1.f;   /* uniform until configured */
    atomic_store_explicit(&f->gain, 1.f, memory_order_relaxed);
    f->gain_cur = 1.f;
    fdn_gains(f);
    return f;
}

void fdn_destroy(Fdn* f) { if (f) { free(f->mem); free(f); } }

void fdn_set_decay(Fdn* f, float rt60_low_s, float rt60_high_s, float xover_hz) {
    if (!f) return;
    f->rt_low   = rt60_low_s  < 0.05f ? 0.05f : (rt60_low_s  > 30.f ? 30.f : rt60_low_s);
    f->rt_high  = rt60_high_s < 0.05f ? 0.05f : (rt60_high_s > 30.f ? 30.f : rt60_high_s);
    f->xover_hz = xover_hz < 100.f ? 100.f : (xover_hz > 0.4f * (float)f->sample_rate ? 0.4f * (float)f->sample_rate : xover_hz);
    fdn_gains(f);
}

void fdn_set_decay_direction(Fdn* f, const float dir[3], float factor) {
    if (!f || !dir) return;
    float n = sqrtf(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
    if (n < 1e-6f) { f->ddir[0] = f->ddir[1] = f->ddir[2] = 0.f; f->dfactor = 1.f; }
    else {
        f->ddir[0] = dir[0]/n; f->ddir[1] = dir[1]/n; f->ddir[2] = dir[2]/n;
        f->dfactor = factor < 0.25f ? 0.25f : (factor > 4.f ? 4.f : factor);
    }
    fdn_gains(f);
}

void fdn_set_gain(Fdn* f, float gain) {
    if (f) atomic_store_explicit(&f->gain, gain < 0.f ? 0.f : gain, memory_order_relaxed);
}

void fdn_set_max_re(Fdn* f, int on) {
    if (f) atomic_store_explicit(&f->max_re, on ? 1 : 0, memory_order_relaxed);
}

void fdn_tap(void* ud, float* bus, uint32_t n, const float* lp_, const float* lq, const float* aux) {
    (void)lp_; (void)lq;
    Fdn* f = (Fdn*)ud;
    if (!f || !aux || n == 0) return;
    /* read the live target ONCE: the ramp must aim at and land on the same value (invariant 4 —
     * a second read racing a control-thread store would step the next block's first sample) */
    const float g_tgt  = atomic_load_explicit(&f->gain, memory_order_relaxed);
    const float g_step = (g_tgt - f->gain_cur) / (float)n;
    float g = f->gain_cur;
    /* max-rE render pair: settled picks ONE matrix; mid-crossfade renders both and lerps per sample
     * (the same aim-and-land single read as the gain — invariant 4). */
    const float re_tgt  = atomic_load_explicit(&f->max_re, memory_order_relaxed) ? 1.f : 0.f;
    const float re_step = (re_tgt - f->re_cur) / (float)n;
    float re = f->re_cur;
    const int   re_fade = (re_tgt != f->re_cur);
    float (*dc_set)[FDN_N] = (f->re_cur >= 1.f) ? f->dcomb_re : f->dcomb;   /* settled matrix */
    uint32_t w = f->w;
    for (uint32_t i = 0; i < n; ++i) {
        float y[FDN_N], sum = 0.f;
        for (int l = 0; l < FDN_N; ++l) {                       /* delayed read -> 2-band decay filter */
            float x = f->ring[l][(w - f->len[l]) & f->rmask[l]];
            f->lp[l] += f->xa * (x - f->lp[l]);
            float yl = f->g_hf[l] * x + (f->g_lf[l] - f->g_hf[l]) * f->lp[l];
            y[l] = yl;
            sum += yl;
        }
        const float in = aux[i];
        const float h = sum * (2.f / (float)FDN_N);
        for (int l = 0; l < FDN_N; ++l)                         /* Householder feedback + input */
            f->ring[l][w & f->rmask[l]] = y[l] - h + f->in_g[l] * in;
        if (!re_fade) {
            for (uint32_t s = 0; s < f->channels; ++s) {        /* render each line as its plane wave */
                const float* dc = dc_set[s];
                float acc = 0.f;
                for (int l = 0; l < FDN_N; ++l) acc += dc[l] * y[l];
                bus[(size_t)s * n + i] += g * acc;
            }
        } else {
            for (uint32_t s = 0; s < f->channels; ++s) {        /* A/B crossfade: both renders, lerped */
                const float* d0 = f->dcomb[s];
                const float* d1 = f->dcomb_re[s];
                float a0 = 0.f, a1 = 0.f;
                for (int l = 0; l < FDN_N; ++l) { a0 += d0[l] * y[l]; a1 += d1[l] * y[l]; }
                bus[(size_t)s * n + i] += g * (a0 + re * (a1 - a0));
            }
            re += re_step;
        }
        g += g_step;
        ++w;
    }
    f->gain_cur = g_tgt;                                        /* land exactly (same local) */
    f->re_cur   = re_tgt;
    f->w = w;
}
