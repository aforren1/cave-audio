/*
 * valid.c — phantom-localization validation. See valid.h for the model and what it does not include.
 */
#include "valid.h"

#include "bw_audio.h"      /* bwa_panner enum only — valid.c calls the panner solves directly */
#include "dbap.h"
#include "spcap.h"
#include "vbap.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define TWO_PI 6.283185307179586

#define VALID_NTONES 24        /* broadband enough to average the intensity over many bins */
#define VALID_F_LO   420.0     /* inside zylia_intensity_doa's 400-1200 band, clear of its edges */
#define VALID_F_HI   1150.0

/* The engine's OWN panner solves, so this scores what will actually ship rather than a copy. Called
 * directly rather than through bwa_panner_gains_batch: that public wrapper substitutes a
 * layout_default() for its DBAP/attenuation tuning and flattens the per-speaker trims, and here we
 * have the caller's REAL layout and want its rolloff_r and its trims to count. (It also lives in
 * engine.c, which is compiled into the DLL rather than into this library.) */
static void valid_gains(const Layout* L, int panner, const float src[3], const float lis[3],
                        float* out) {
    if (panner == BWA_PAN_SPCAP) {
        SpcapState sp; spcap_reset(&sp);
        spcap_gains(&sp, src, lis, L, 1u, 1.0f, out);
    } else if (panner == BWA_PAN_VBAP) {
        VbapState vb; vbap_reset(&vb);
        vbap_gains(&vb, src, lis, L, 1u, 1.0f, out);
    } else {
        dbap_gains(src, lis, L, 1.0f, out);
    }
}

/* The harness stimulus, in one place so the simulated and hardware paths cannot drift apart: a fixed
 * broadband tone sum inside the estimator's own band. Fixed seed — a cell must be reproducible. */
static void valid_stimulus(double* phase, double* freq) {
    unsigned int rng = 0x5eed1234u;
    for (int k = 0; k < VALID_NTONES; ++k) {
        rng = rng * 1664525u + 1013904223u;
        phase[k] = ((double)(rng >> 8) / (double)(1u << 24)) * TWO_PI;
        freq[k]  = VALID_F_LO + (VALID_F_HI - VALID_F_LO) * (double)k / (double)(VALID_NTONES - 1);
    }
}

int valid_speaker_feeds(const Layout* L, int panner, const float solve_pos[3],
                        const float src_world[3], double fs, float* feeds, uint32_t n) {
    if (!L || !solve_pos || !src_world || !feeds || fs <= 0.0 || n < 64u) return 0;
    uint32_t nspk = L->count;
    if (nspk < 4u || nspk > (uint32_t)BWA_CHANNELS) return 0;

    float gains[BWA_CHANNELS];
    memset(gains, 0, sizeof gains);
    valid_gains(L, panner, src_world, solve_pos, gains);

    double phase[VALID_NTONES], freq[VALID_NTONES];
    valid_stimulus(phase, freq);

    /* One stimulus, generated once over [-maxd, n); every speaker is a scaled, delayed copy of it.
     * Generating it per speaker would be nspk x n x ntones trig for no reason. */
    uint32_t maxd = L->max_delay_samples;
    for (uint32_t i = 0; i < nspk; ++i) if (L->speakers[i].delay_samples > maxd) maxd = L->speakers[i].delay_samples;
    double* s = (double*)malloc(sizeof(double) * ((size_t)n + maxd));
    if (!s) return 0;
    for (uint32_t j = 0; j < n + maxd; ++j) {
        double t = ((double)j - (double)maxd) / fs, v = 0.0;
        for (int k = 0; k < VALID_NTONES; ++k) v += sin(TWO_PI * freq[k] * t + phase[k]);
        s[j] = v / (double)VALID_NTONES;
    }
    for (uint32_t i = 0; i < nspk; ++i) {
        double A = (double)gains[i] * (double)L->speakers[i].gain_lin;
        uint32_t d = L->speakers[i].delay_samples;
        for (uint32_t t = 0; t < n; ++t)
            feeds[(size_t)i * n + t] = (float)(A * s[(size_t)t + (maxd - d)]);   /* maxd >= d */
    }
    free(s);
    return 1;
}

int valid_simulate(const Layout* L, int panner, const float solve_pos[3], const float mic[3],
                   const float src_world[3], double fs, double c, float* buf, uint32_t n) {
    if (!L || !solve_pos || !mic || !src_world || !buf) return 0;
    if (fs <= 0.0 || c <= 0.0 || n < 64u) return 0;
    uint32_t nspk = L->count;
    if (nspk < 4u || nspk > (uint32_t)BWA_CHANNELS) return 0;

    float gains[BWA_CHANNELS];
    memset(gains, 0, sizeof gains);
    valid_gains(L, panner, src_world, solve_pos, gains);

    float capd[ZYLIA_MICS][3], R;
    zylia_geometry(capd, &R);

    double phase[VALID_NTONES], freq[VALID_NTONES];
    valid_stimulus(phase, freq);

    /* Every (speaker, capsule) path is one complex gain per tone:
     *   A_ij sin(w_k t + phi_k - w_k tau_ij) = Im{ e^{i w_k t} . A_ij e^{i(phi_k - w_k tau_ij)} }
     * so summing the array at a capsule collapses to ONE complex number per (capsule, tone). That
     * turns a 26 x 19 x 24 x n trig loop into 24 x n trig plus a multiply-accumulate — and it is
     * exact, not an approximation: fractional delays are carried in the phase. */
    double Cr[ZYLIA_MICS][VALID_NTONES], Ci[ZYLIA_MICS][VALID_NTONES];
    memset(Cr, 0, sizeof Cr);
    memset(Ci, 0, sizeof Ci);
    for (int j = 0; j < ZYLIA_MICS; ++j) {
        double mx = (double)mic[0] + R*capd[j][0];
        double my = (double)mic[1] + R*capd[j][1];
        double mz = (double)mic[2] + R*capd[j][2];
        for (uint32_t i = 0; i < nspk; ++i) {
            double dx = mx - L->speakers[i].pos[0];
            double dy = my - L->speakers[i].pos[1];
            double dz = mz - L->speakers[i].pos[2];
            double r  = sqrt(dx*dx + dy*dy + dz*dz);
            if (r < 0.05) r = 0.05;                   /* a capsule inside a cabinet is not a thing */
            /* propagation + the layout's own alignment delay: at an off-centre listener that
             * alignment is imperfect, which is a real effect and belongs in the measurement */
            double tau = r / c + (double)L->speakers[i].delay_samples / fs;
            double A   = (double)gains[i] * (double)L->speakers[i].gain_lin / r;
            for (int k = 0; k < VALID_NTONES; ++k) {
                double ang = phase[k] - TWO_PI * freq[k] * tau;
                Cr[j][k] += A * cos(ang);
                Ci[j][k] += A * sin(ang);
            }
        }
    }

    for (int j = 0; j < ZYLIA_MICS; ++j)
        for (uint32_t t = 0; t < n; ++t) buf[(size_t)j * n + t] = 0.0f;

    for (int k = 0; k < VALID_NTONES; ++k) {
        double w = TWO_PI * freq[k] / fs;
        for (uint32_t t = 0; t < n; ++t) {
            double s = sin(w * (double)t), cc = cos(w * (double)t);
            for (int j = 0; j < ZYLIA_MICS; ++j)
                buf[(size_t)j * n + t] += (float)((Cr[j][k]*s + Ci[j][k]*cc) / (double)VALID_NTONES);
        }
    }

    /* Level carries no information here (the estimator reads a normalized direction), but an
     * un-normalized sum of 26 speakers can sit anywhere, so pin the peak where a real capture would
     * be — otherwise zylia_check_capsules would report the whole array as clipping. */
    float peak = 0.0f;
    for (size_t i = 0; i < (size_t)ZYLIA_MICS * n; ++i) {
        float a = buf[i] < 0.0f ? -buf[i] : buf[i];
        if (a > peak) peak = a;
    }
    if (peak > 0.0f) {
        float g = 0.5f / peak;
        for (size_t i = 0; i < (size_t)ZYLIA_MICS * n; ++i) buf[i] *= g;
    }
    return 1;
}

int valid_score(const Layout* L, int panner, int tracked, const float mic[3], const float src_world[3],
                const float* cap19, uint32_t n, double fs, double c,
                const unsigned char* exclude, ValidCell* out) {
    if (!L || !mic || !src_world || !cap19 || !out) return 0;

    memset(out, 0, sizeof *out);
    out->panner = panner;
    out->tracked = tracked ? 1 : 0;
    out->mic[0] = mic[0]; out->mic[1] = mic[1]; out->mic[2] = mic[2];

    double tx = (double)src_world[0] - mic[0];
    double ty = (double)src_world[1] - mic[1];
    double tz = (double)src_world[2] - mic[2];
    double tn = sqrt(tx*tx + ty*ty + tz*tz);
    if (tn < 1e-6) return 0;                          /* source on top of the listener */
    out->target[0] = (float)(tx/tn); out->target[1] = (float)(ty/tn); out->target[2] = (float)(tz/tn);

    const float* ptr[ZYLIA_MICS];
    for (int j = 0; j < ZYLIA_MICS; ++j) ptr[j] = cap19 + (size_t)j * n;

    float d[3], psi = 0.0f;
    if (!zylia_intensity_doa(ptr, n, fs, c, 400.0, 1200.0, exclude, d, &psi)) { out->ok = 0; return 1; }

    out->measured[0] = d[0]; out->measured[1] = d[1]; out->measured[2] = d[2];
    out->diffuseness = psi;
    double dot = (double)d[0]*out->target[0] + (double)d[1]*out->target[1] + (double)d[2]*out->target[2];
    if (dot >  1.0) dot =  1.0;
    if (dot < -1.0) dot = -1.0;
    out->miss_deg = (float)(acos(dot) * 180.0 / M_PI);
    out->ok = 1;
    return 1;
}

int valid_cell(const Layout* L, int panner, int tracked, const float mic[3], const float src_world[3],
               double fs, double c, uint32_t n, ValidCell* out) {
    if (!L || !mic || !src_world || !out) return 0;
    float* buf = (float*)malloc(sizeof(float) * (size_t)ZYLIA_MICS * n);
    if (!buf) return 0;
    const float* solve = tracked ? mic : L->ref;
    if (!valid_simulate(L, panner, solve, mic, src_world, fs, c, buf, n)) { free(buf); return 0; }
    int r = valid_score(L, panner, tracked, mic, src_world, buf, n, fs, c, NULL, out);
    free(buf);
    return r;
}

int valid_re_proxy(const Layout* L, int panner, const float solve_pos[3], const float mic[3],
                   const float src_world[3], float* re_err_deg, float* spread_deg) {
    if (!L || !solve_pos || !mic || !src_world) return 0;
    uint32_t nspk = L->count;
    if (nspk < 4u || nspk > (uint32_t)BWA_CHANNELS) return 0;

    float gains[BWA_CHANNELS];
    memset(gains, 0, sizeof gains);
    valid_gains(L, panner, src_world, solve_pos, gains);

    /* energy vector, speaker directions taken FROM THE LISTENER (layout_tool does the same) */
    double rE[3] = { 0, 0, 0 }, esum = 0.0;
    for (uint32_t i = 0; i < nspk; ++i) {
        double w = (double)gains[i] * gains[i];
        double dx = L->speakers[i].pos[0] - mic[0];
        double dy = L->speakers[i].pos[1] - mic[1];
        double dz = L->speakers[i].pos[2] - mic[2];
        double m = sqrt(dx*dx + dy*dy + dz*dz);
        if (m < 1e-9) continue;
        rE[0] += w * dx/m; rE[1] += w * dy/m; rE[2] += w * dz/m;
        esum  += w;
    }
    double rl = sqrt(rE[0]*rE[0] + rE[1]*rE[1] + rE[2]*rE[2]);
    if (rl < 1e-12 || esum < 1e-15) return 0;

    double tx = src_world[0] - mic[0], ty = src_world[1] - mic[1], tz = src_world[2] - mic[2];
    double tn = sqrt(tx*tx + ty*ty + tz*tz);
    if (tn < 1e-9) return 0;
    double dot = (rE[0]*tx + rE[1]*ty + rE[2]*tz) / (rl * tn);
    if (dot >  1.0) dot =  1.0;
    if (dot < -1.0) dot = -1.0;
    if (re_err_deg) *re_err_deg = (float)(acos(dot) * 180.0 / M_PI);

    double mag = rl / esum;
    if (mag > 1.0) mag = 1.0;
    if (spread_deg) *spread_deg = (float)(186.4 * (1.0 - mag) + 10.7);
    return 1;
}

int valid_run(const Layout* L, const int* panners, int npan, int tracked,
              const float (*listeners)[3], int nlis,
              const float (*targets)[3], int ntgt,
              float radius, double fs, double c, uint32_t n, ValidCell* cells_out) {
    if (!L || !panners || !listeners || !targets || !cells_out) return 0;
    if (npan < 1 || nlis < 1 || ntgt < 1 || radius <= 0.0f) return 0;

    int w = 0;
    for (int p = 0; p < npan; ++p)
        for (int li = 0; li < nlis; ++li)
            for (int t = 0; t < ntgt; ++t) {
                /* one physical source per target direction, placed off the SWEET SPOT, so every
                 * listener position is judged against the same sources */
                float src[3] = { L->ref[0] + radius * targets[t][0],
                                 L->ref[1] + radius * targets[t][1],
                                 L->ref[2] + radius * targets[t][2] };
                ValidCell* cc = &cells_out[w];
                if (!valid_cell(L, panners[p], tracked, listeners[li], src, fs, c, n, cc)) {
                    memset(cc, 0, sizeof *cc);
                    cc->panner = panners[p];
                    cc->tracked = tracked ? 1 : 0;
                }
                cc->lis = li;
                cc->tgt = t;
                ++w;
            }
    return w;
}

int valid_target_grid(int naz, const float* elev_deg, int nel, float (*out)[3], int cap) {
    if (naz < 1 || nel < 1 || !elev_deg || !out) return 0;
    if (naz * nel > cap) return 0;
    int w = 0;
    for (int e = 0; e < nel; ++e) {
        double el = (double)elev_deg[e] * M_PI / 180.0, ce = cos(el), se = sin(el);
        for (int a = 0; a < naz; ++a) {
            double az = TWO_PI * (double)a / (double)naz;
            /* room axes: azimuth from -z (front) toward +x (right), elevation = asin(y) — the same
             * convention zylia_geometry and the layout use */
            out[w][0] = (float)( ce * sin(az));
            out[w][1] = (float)( se);
            out[w][2] = (float)(-ce * cos(az));
            ++w;
        }
    }
    return w;
}

/* ---- statistics ------------------------------------------------------------------------------ */

static int cmp_double(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x < y) ? -1 : ((x > y) ? 1 : 0);
}

static double median_sorted(const double* s, int n) {
    return (n & 1) ? s[n/2] : 0.5 * (s[n/2 - 1] + s[n/2]);
}

double valid_median(const double* v, int n) {
    if (!v || n < 1) return 0.0;
    double* s = (double*)malloc(sizeof(double) * (size_t)n);
    if (!s) return 0.0;
    memcpy(s, v, sizeof(double) * (size_t)n);
    qsort(s, (size_t)n, sizeof(double), cmp_double);
    double m = median_sorted(s, n);
    free(s);
    return m;
}

/* percentile bootstrap of the median: resample n-with-replacement nresamp times, sort the resulting
 * medians, read the 2.5th and 97.5th percentiles off them. */
static int bootstrap_median_ci(const double* v, int n, int nresamp, unsigned int seed,
                               double* lo, double* hi) {
    if (!v || n < 2 || nresamp < 20 || !lo || !hi) return 0;
    double* meds = (double*)malloc(sizeof(double) * (size_t)nresamp);
    double* work = (double*)malloc(sizeof(double) * (size_t)n);
    if (!meds || !work) { free(meds); free(work); return 0; }
    unsigned int rng = seed ? seed : 1u;
    for (int r = 0; r < nresamp; ++r) {
        for (int i = 0; i < n; ++i) {
            rng = rng * 1664525u + 1013904223u;
            work[i] = v[(int)((rng >> 8) % (unsigned int)n)];
        }
        qsort(work, (size_t)n, sizeof(double), cmp_double);
        meds[r] = median_sorted(work, n);
    }
    qsort(meds, (size_t)nresamp, sizeof(double), cmp_double);
    int ilo = (int)(0.025 * (double)nresamp);
    int ihi = (int)(0.975 * (double)nresamp);
    if (ihi >= nresamp) ihi = nresamp - 1;
    *lo = meds[ilo];
    *hi = meds[ihi];
    free(meds); free(work);
    return 1;
}

int valid_bootstrap_ci(const double* v, int n, int nresamp, unsigned int seed,
                       double* lo, double* hi) {
    return bootstrap_median_ci(v, n, nresamp, seed, lo, hi);
}

int valid_contrast(const double* a, const double* b, int n, int nresamp, unsigned int seed,
                   double* med_diff, double* lo, double* hi) {
    if (!a || !b || n < 2 || !med_diff) return 0;
    double* d = (double*)malloc(sizeof(double) * (size_t)n);
    if (!d) return 0;
    for (int i = 0; i < n; ++i) d[i] = b[i] - a[i];       /* PAIRED: same cell, two conditions */
    *med_diff = valid_median(d, n);
    int ok = 1;
    if (lo && hi) ok = bootstrap_median_ci(d, n, nresamp, seed, lo, hi);
    free(d);
    return ok;
}
