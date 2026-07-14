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
#include "ambisonics.h"

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
    float    in_g[FDN_N];         /* aux injection gains (alternating sign, 1/sqrt(N)) */
    /* per-line decay filter: y = g_hf*x + (g_lf - g_hf)*lp, lp += a*(x - lp) */
    float    g_lf[FDN_N], g_hf[FDN_N], lp[FDN_N], xa;
    /* design parameters (kept to re-derive the gains when either setter runs) */
    float    rt_low, rt_high, xover_hz;
    float    ddir[3], dfactor;    /* anisotropy: decay-time scale toward ddir */
    _Atomic float gain;           /* wet return level target (written live by the control thread) */
    float    gain_cur;            /* per-block ramp state (audio-thread-only) */
};

static void fib16(int i, float d[3]) {          /* same Fibonacci construction as allrad.c */
    float y = 1.f - 2.f * ((float)i + 0.5f) / (float)FDN_N;
    float r = sqrtf(fmaxf(0.f, 1.f - y * y)), th = (float)i * 2.39996323f;
    d[0] = r * cosf(th); d[1] = y; d[2] = r * sinf(th);
}

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

Fdn* fdn_create(const Layout* L, uint32_t sample_rate, uint32_t channels) {
    if (!L || sample_rate == 0 || channels == 0 || channels > BWA_CHANNELS) return NULL;
    Fdn* f = (Fdn*)calloc(1, sizeof *f);
    if (!f) return NULL;
    f->channels = channels; f->sample_rate = sample_rate;
    size_t total = 0;
    for (int l = 0; l < FDN_N; ++l) {
        uint32_t len = (uint32_t)(fdn_ms[l] * 1e-3f * (float)sample_rate) | 1u;   /* odd */
        f->len[l] = len;
        uint32_t rl = 1; while (rl < len + 1) rl <<= 1;
        f->rlen[l] = rl; f->rmask[l] = rl - 1;
        total += rl;
    }
    f->mem = (float*)calloc(total, sizeof(float));
    if (!f->mem) { free(f); return NULL; }
    size_t at = 0;
    for (int l = 0; l < FDN_N; ++l) { f->ring[l] = f->mem + at; at += f->rlen[l]; }

    /* line -> speaker render: each line is a plane wave from its direction, decoded through the
     * layout's bed decode (AllRAD; sampling-decode fallback mirrors rt.c's build_bed_decode_sad). */
    float dec[BWA_CHANNELS][BWA_AMBI_CH];
    if (!allrad_build_decode(L, dec)) {
        for (uint32_t s = 0; s < L->count; ++s) {               /* sampling decode: (2l+1)*Y/N per row */
            float p[3] = { L->speakers[s].pos[0] - L->ref[0], L->speakers[s].pos[1] - L->ref[1],
                           L->speakers[s].pos[2] - L->ref[2] };
            float pl = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
            float ad[3] = { pl > 1e-6f ? p[2]/pl : 1.f, pl > 1e-6f ? p[0]/pl : 0.f, pl > 1e-6f ? p[1]/pl : 0.f };
            float y[BWA_AMBI_CH]; ambi_encode_sn3d(ad, y);
            for (int k = 0; k < BWA_AMBI_CH; ++k) {
                int ll = (int)floorf(sqrtf((float)k));
                dec[s][k] = (float)(2*ll + 1) * y[k] / (float)L->count;
            }
        }
    }
    for (int l = 0; l < FDN_N; ++l) {
        fib16(l, f->dir[l]);
        float ad[3] = { f->dir[l][2], f->dir[l][0], f->dir[l][1] };   /* room -> ambi (z,x,y) */
        float y[BWA_AMBI_CH]; ambi_encode_sn3d(ad, y);
        for (uint32_t s = 0; s < channels; ++s) {
            float acc = 0.f;
            for (int k = 0; k < BWA_AMBI_CH; ++k) acc += dec[s][k] * y[k];
            f->dcomb[s][l] = acc;
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

void fdn_tap(void* ud, float* bus, uint32_t n, const float* lp_, const float* lq, const float* aux) {
    (void)lp_; (void)lq;
    Fdn* f = (Fdn*)ud;
    if (!f || !aux || n == 0) return;
    /* read the live target ONCE: the ramp must aim at and land on the same value (invariant 4 —
     * a second read racing a control-thread store would step the next block's first sample) */
    const float g_tgt  = atomic_load_explicit(&f->gain, memory_order_relaxed);
    const float g_step = (g_tgt - f->gain_cur) / (float)n;
    float g = f->gain_cur;
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
        for (uint32_t s = 0; s < f->channels; ++s) {            /* render each line as its plane wave */
            const float* dc = f->dcomb[s];
            float acc = 0.f;
            for (int l = 0; l < FDN_N; ++l) acc += dc[l] * y[l];
            bus[(size_t)s * n + i] += g * acc;
        }
        g += g_step;
        ++w;
    }
    f->gain_cur = g_tgt;                                        /* land exactly (same local) */
    f->w = w;
}
