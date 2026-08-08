/*
 * valid.c — phantom-localization validation. See valid.h for the model and what it does not include.
 */
#include "valid.h"

#include "bw_audio.h"      /* bwa_panner enum only — valid.c calls the panner solves directly */
#include "dbap.h"
#include "rt.h"            /* the phantom arm renders through a real engine core (see valid.h) */
#include "spcap.h"
#include "vbap.h"

#include <math.h>
#include <stdio.h>         /* snprintf, for the stimulus label */
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define TWO_PI 6.283185307179586

#define VALID_NTONES 24        /* broadband enough to average the intensity over many bins */
#define VALID_F_LO   420.0     /* inside zylia_intensity_doa's 400-1200 band, clear of its edges */
#define VALID_F_HI   1150.0

/* The <= 0 sentinel, in ONE place: either knob <= 0 reverts that one to the default for THIS array.
 * The layout already carries them resolved (spcap_focus is layout_derive_spcap_focus, spcap_density
 * the constant), which is what makes "the caller's real layout" the right fallback here rather than
 * bwa_panner_gains_batch's layout_default() stand-in. */
static void valid_tuning(const Layout* L, float focus, float density, float* f_out, float* d_out) {
    *f_out = (focus   > 0.f) ? focus   : L->spcap_focus;
    *d_out = (density > 0.f) ? density : L->spcap_density;
}

void valid_render_init(ValidRender* r) { if (r) memset(r, 0, sizeof *r); }

/* The all-defaults render, so every call below can take NULL and mean "the shape this harness had
 * before the knobs became sweepable". */
static const ValidRender VALID_RENDER_OFF = { 0.f, 0.f, 0, 0, 0.f, 0, 0, 0, 0.f, 0.f };
static const ValidRender* rr(const ValidRender* R) { return R ? R : &VALID_RENDER_OFF; }

/* `R` with the focus/density sentinel applied against THIS layout — what a cell records, and what a
 * report compares. Field-wise rather than memcmp: a struct with padding would compare garbage. */
static ValidRender valid_resolve(const Layout* L, const ValidRender* R) {
    ValidRender out = *rr(R);
    valid_tuning(L, out.focus, out.density, &out.focus, &out.density);
    return out;
}

int valid_render_equal(const ValidRender* pa, const ValidRender* pb) {
    const ValidRender* a = rr(pa);
    const ValidRender* b = rr(pb);
    return a->focus == b->focus && a->density == b->density &&
           a->dual_band == b->dual_band && a->cap == b->cap &&
           a->hole_spread == b->hole_spread && a->tracked_align == b->tracked_align &&
           a->spread_mode == b->spread_mode && a->decorrelation == b->decorrelation &&
           a->near_spread == b->near_spread && a->spread == b->spread;
}

/* The engine's OWN panner solves, so this scores what will actually ship rather than a copy. Called
 * directly rather than through bwa_panner_gains_batch: that public wrapper substitutes a
 * layout_default() for its DBAP/attenuation tuning and flattens the per-speaker trims, and here we
 * have the caller's REAL layout and want its rolloff_r and its trims to count. (It also lives in
 * engine.c, which is compiled into the DLL rather than into this library.) */
static void valid_gains(const Layout* L, int panner, const float src[3], const float lis[3],
                        float focus, float density, float* out) {
    if (panner == BWA_PAN_SPCAP) {
        float f, d;
        valid_tuning(L, focus, density, &f, &d);
        SpcapState sp; spcap_reset(&sp);
        spcap_gains(&sp, src, lis, L, 1u, f, d, 1.0f, out);
    } else if (panner == BWA_PAN_VBAP) {
        VbapState vb; vbap_reset(&vb);
        vbap_gains(&vb, src, lis, L, 1u, 1.0f, out);
    } else {
        dbap_gains(src, lis, L, 1.0f, out);
    }
}

/* Selected stimulus. Module-level and control-thread only (see valid.h): it is a property of a whole
 * run, and threading it through every feed/field/score signature would buy nothing. */
static ValidStimKind g_stim_kind = VALID_STIM_BROADBAND;
static double        g_stim_hz   = 0.0;
static char          g_stim_name[32] = "broadband 420-1150 Hz";

void valid_get_stimulus_band(double* f_lo, double* f_hi) {
    if (g_stim_kind == VALID_STIM_TONE) {
        const double SIXTH = 1.1224620483;            /* 2^(1/6): the +-1/6-octave analysis band */
        if (f_lo) *f_lo = g_stim_hz / SIXTH;
        if (f_hi) *f_hi = g_stim_hz * SIXTH;
    } else {
        if (f_lo) *f_lo = 400.0;
        if (f_hi) *f_hi = ZYLIA_FOA_FMAX;
    }
}

const char* valid_stimulus_name(void) { return g_stim_name; }

int valid_set_stimulus(ValidStimKind kind, double hz) {
    if (kind == VALID_STIM_TONE) {
        if (!(hz > 0.0)) return 0;
        const double SIXTH = 1.1224620483;
        /* The whole band has to be reachable. A tone above the array's first-order ceiling is
         * refused HERE, with the frequency in hand, rather than letting every cell fail later with
         * no explanation — the 6 kHz condition the published study also had to drop. */
        if (hz / SIXTH >= ZYLIA_FOA_FMAX) return 0;
        g_stim_kind = kind;
        g_stim_hz   = hz;
        snprintf(g_stim_name, sizeof g_stim_name, "%.0f Hz tone", hz);
    } else {
        g_stim_kind = VALID_STIM_BROADBAND;
        g_stim_hz   = 0.0;
        snprintf(g_stim_name, sizeof g_stim_name, "broadband 420-1150 Hz");
    }
    return 1;
}

/* The harness stimulus, in one place so the simulated and hardware paths cannot drift apart. Returns
 * the number of tones written. Fixed seed — a cell must be reproducible. */
static int valid_stimulus(double* phase, double* freq) {
    if (g_stim_kind == VALID_STIM_TONE) {
        phase[0] = 0.0;
        freq[0]  = g_stim_hz;
        return 1;                                     /* ONE tone: no cross-bin averaging, by design */
    }
    unsigned int rng = 0x5eed1234u;
    for (int k = 0; k < VALID_NTONES; ++k) {
        rng = rng * 1664525u + 1013904223u;
        phase[k] = ((double)(rng >> 8) / (double)(1u << 24)) * TWO_PI;
        freq[k]  = VALID_F_LO + (VALID_F_HI - VALID_F_LO) * (double)k / (double)(VALID_NTONES - 1);
    }
    return VALID_NTONES;
}

/* The stimulus sampled on an ABSOLUTE frame clock: out[j] = s((start + j) / fs). Every path that
 * needs the signal — the legacy feed builder, the analytic field, and the engine's push voice —
 * takes it from here at a stated time origin, which is what lets an engine render and a hand-built
 * feed be compared sample for sample instead of only statistically. */
static void valid_stim_fill(double* out, uint32_t n, int64_t start, double fs) {
    double phase[VALID_NTONES], freq[VALID_NTONES];
    int nt = valid_stimulus(phase, freq);
    for (uint32_t j = 0; j < n; ++j) {
        double t = (double)(start + (int64_t)j) / fs, v = 0.0;
        for (int k = 0; k < nt; ++k) v += sin(TWO_PI * freq[k] * t + phase[k]);
        out[j] = v / (double)nt;
    }
}

/* Feeds for an ARBITRARY per-speaker gain vector. The single-speaker physical reference and the
 * pre-engine baseline both go through here, so they cannot drift apart in trim, delay or stimulus.
 * feed i at output frame t carries the stimulus at frame (t - delay_i) — the delay is filled from
 * the stimulus's own negative time rather than from silence, so there is no start transient. */
static int valid_feeds_from_gains(const Layout* L, const float* gains,
                                  double fs, float* feeds, uint32_t n) {
    uint32_t nspk = L->count;

    /* One stimulus, generated once over [-maxd, n); every speaker is a scaled, delayed copy of it.
     * Generating it per speaker would be nspk x n x ntones trig for no reason. */
    uint32_t maxd = L->max_delay_samples;
    for (uint32_t i = 0; i < nspk; ++i) if (L->speakers[i].delay_samples > maxd) maxd = L->speakers[i].delay_samples;
    double* s = (double*)malloc(sizeof(double) * ((size_t)n + maxd));
    if (!s) return 0;
    valid_stim_fill(s, n + maxd, -(int64_t)maxd, fs);
    for (uint32_t i = 0; i < nspk; ++i) {
        double A = (double)gains[i] * (double)L->speakers[i].gain_lin;
        uint32_t d = L->speakers[i].delay_samples;
        for (uint32_t t = 0; t < n; ++t)
            feeds[(size_t)i * n + t] = (float)(A * s[(size_t)t + (maxd - d)]);   /* maxd >= d */
    }
    free(s);
    return 1;
}

int valid_speaker_feeds_direct(const Layout* L, int panner, const float solve_pos[3],
                               const float src_world[3], double fs, float focus, float density,
                               float* feeds, uint32_t n) {
    if (!L || !solve_pos || !src_world || !feeds || fs <= 0.0 || n < 64u) return 0;
    if (L->count < 4u || L->count > (uint32_t)BWA_CHANNELS) return 0;
    float gains[BWA_CHANNELS];
    memset(gains, 0, sizeof gains);
    valid_gains(L, panner, src_world, solve_pos, focus, density, gains);
    return valid_feeds_from_gains(L, gains, fs, feeds, n);
}

/* ---- the engine the phantom arm renders through ------------------------------------------------
 *
 * One RtCore, one push voice, reused across cells. Building an engine per cell would be almost all
 * setup: a sweep is thousands of cells and rt_create allocates the voice table, the aligner's delay
 * lines and the bed decode every time. The cache is keyed on the LAYOUT and the sample rate, so a
 * test that switches layouts mid-run gets a fresh core rather than a stale aligner.
 *
 * Control thread only, exactly like g_stim_kind above. There is no audio thread here: rt_render runs
 * synchronously on this thread with a synthetic sample-counter timestamp, which is what
 * BWA_SINK_MANUAL does for the public ABI (manual_sink.c), so a cell renders identically every run.
 */
#define VE_BLOCK 256u                    /* the engine's default render quantum */

typedef struct {
    RtCore*  core;
    Layout   L;                          /* the layout this core was built for (cache key) */
    uint32_t rate;
    uint32_t src;                        /* the push voice */
    uint64_t pos;                        /* the deterministic sample clock, as the manual sink keeps it */
    float*   bus;                        /* VE_BLOCK * channels, planar — rt_render's output */
    double*  stim;                       /* VE_BLOCK of stimulus, refilled per block */
    float*   push;                       /* the same block as float, for rt_source_push */
    int      lc_dirty;                   /* the PREVIOUS cell left the tracked aligner displaced, so
                                          * this one must wait for it to glide home even if it does
                                          * not use the feature itself (see ve_settle) */
} ValidEngine;

static ValidEngine g_ve;

void valid_engine_release(void) {
    if (g_ve.core) rt_destroy(g_ve.core);
    free(g_ve.bus); free(g_ve.stim); free(g_ve.push);
    memset(&g_ve, 0, sizeof g_ve);
}

/* Layouts compare by VALUE: two Layout structs describing the same array must reuse the same core,
 * and the same pointer refilled with a different array must not. memcmp is exact here — Layout is
 * plain data with no padding worth worrying about, and a false MISS only costs a rebuild. */
static int ve_open(const Layout* L, uint32_t rate) {
    if (g_ve.core && g_ve.rate == rate && memcmp(&g_ve.L, L, sizeof(Layout)) == 0) return 1;
    valid_engine_release();
    g_ve.core = rt_create(4u, 4u, rate, L->count);
    if (!g_ve.core) return 0;
    rt_set_layout(g_ve.core, L);
    g_ve.bus  = (float*) malloc(sizeof(float)  * (size_t)VE_BLOCK * L->count);
    g_ve.stim = (double*)malloc(sizeof(double) * (size_t)VE_BLOCK);
    g_ve.push = (float*) malloc(sizeof(float)  * (size_t)VE_BLOCK);
    char err[128] = { 0 };
    g_ve.src = rt_source_create_stream(g_ve.core, err, sizeof err);
    if (!g_ve.bus || !g_ve.stim || !g_ve.push || !g_ve.src) { valid_engine_release(); return 0; }
    g_ve.L    = *L;
    g_ve.rate = rate;
    g_ve.pos  = 0;
    /* Measurement settings, not render settings. The limiter is ON at -1 dBFS by default and would
     * compress exactly the gains being measured; master gain is pinned at unity for the same reason.
     * The tracked-alignment guards manage the TRANSIENT of a moving head (jitter dead zone, delay
     * slew rate); this measurement is steady-state, so they are opened wide and the settle below
     * waits for the state they converge to. */
    rt_set_limiter(g_ve.core, 0);
    rt_set_master_gain(g_ve.core, 1.f);
    rt_set_tracked_align_guards(g_ve.core, 1e-4f, 1e6f);
    return 1;
}

/* How many frames to render and throw away before the capture window. Three transients have to be
 * out of the way, and none of them is what a cell measures:
 *   - the per-voice gain ramp (invariant 4) and the push voice's own gate, one block each;
 *   - align.c's per-speaker delay lines, which start empty (max_delay frames of it);
 *   - with tracked alignment on, the comp's glide. Its delay slew is opened wide above, but the
 *     per-channel LEVEL trim slews at a fixed 4.0 linear/s and can travel the full 0.5 -> 2.0 span,
 *     so half a second covers it with room to spare.
 * Generous rather than tuned: this is offline analysis and a block of render is cheap, while a cell
 * captured mid-ramp is a wrong number that looks plausible. */
static uint32_t ve_settle(const Layout* L, const ValidRender* R, uint32_t rate) {
    uint32_t maxd = L->max_delay_samples;
    for (uint32_t i = 0; i < L->count; ++i)
        if (L->speakers[i].delay_samples > maxd) maxd = L->speakers[i].delay_samples;
    uint32_t s = maxd + 8u * VE_BLOCK;
    /* Tracked alignment glides BOTH ways, and the trip home lands on the NEXT cell. align.c slews the
     * level trim at a fixed 4.0 linear/s, so undoing a 0.7 m listener's trims takes up to ~0.37 s
     * while the base settle is ~43 ms. Waiting only when THIS cell asks for the feature leaves the
     * first cell after it opening its capture window on gains up to ~3 dB wrong and decaying, which
     * is a measurement corrupted by whatever ran before it. Wait when either end is displaced. */
    if (rr(R)->tracked_align || g_ve.lc_dirty) s += rate / 2u;
    return (s + VE_BLOCK - 1u) / VE_BLOCK * VE_BLOCK;
}

/* Render `pre + n` frames of the array bus into `feeds` ([nspk][pre+n], row stride pre+n), with the
 * stimulus timed so that feed frame `pre + t` carries the stimulus at frame t. That origin is what
 * makes an engine feed comparable sample-for-sample with valid_feeds_from_gains (pre = 0), and what
 * lets valid_simulate read backwards by a propagation delay without running off the front. */
static int ve_render(const Layout* L, int panner, const ValidRender* Rin, const float solve_pos[3],
                     const float src_world[3], double fs, double c, float* feeds,
                     uint32_t pre, uint32_t n) {
    const ValidRender* R = rr(Rin);
    if (!ve_open(L, (uint32_t)(fs + 0.5))) return 0;
    RtCore* co = g_ve.core;
    const uint32_t nspk = L->count, row = pre + n;

    rt_set_panner(co, panner);
    rt_set_spcap_focus(co, R->focus, R->density);
    rt_set_dual_band(co, R->dual_band ? 1 : 0);
    rt_set_cap(co, (R->cap && R->dual_band) ? 1 : 0);   /* CAP touches the low band only: no dual, no CAP */
    rt_set_spread_mode(co, R->spread_mode);
    rt_set_decorrelation(co, R->decorrelation ? 1 : 0);
    rt_set_near_spread(co, R->near_spread);
    rt_set_hole_spread(co, R->hole_spread);
    rt_set_tracked_align(co, R->tracked_align ? 1 : 0);
    rt_set_speed_of_sound(co, (float)c);
    rt_source_set_spread(co, g_ve.src, R->spread);
    /* The panner solves at the listener, so the harness's SOLVE position IS the engine's listener:
     * "fixed" is an install that never learned the head moved, which is exactly a listener parked at
     * the layout reference. Orientation is identity (facing room-ahead) — see valid.h on CAP. */
    const float q[4] = { 0.f, 0.f, 0.f, 1.f };
    rt_set_listener(co, solve_pos, q);
    rt_source_set_pos(co, g_ve.src, src_world[0], src_world[1], src_world[2]);
    rt_commit(co);

    const uint32_t settle = ve_settle(L, R, g_ve.rate);
    /* Record what this cell leaves behind BEFORE rendering, so an early return still marks the core
     * displaced rather than letting the next cell trust a stale clean flag. */
    g_ve.lc_dirty = rr(R)->tracked_align ? 1 : 0;
    const uint32_t total  = settle + row;
    for (uint32_t f = 0; f < total; f += VE_BLOCK) {
        const uint32_t bn = (total - f < VE_BLOCK) ? (total - f) : VE_BLOCK;
        /* Push before rendering, always: a push voice that underruns renders silence without losing
         * its place, which would silently punch a hole in the capture. */
        valid_stim_fill(g_ve.stim, bn, (int64_t)f - (int64_t)settle - (int64_t)pre, fs);
        for (uint32_t i = 0; i < bn; ++i) g_ve.push[i] = (float)g_ve.stim[i];
        if (rt_source_push(co, g_ve.src, g_ve.push, bn) != bn) return 0;
        bwa_timestamp ts = {                       /* manual_sink.c's deterministic clock, verbatim */
            .sample_pos     = g_ve.pos,
            .system_time_ns = (g_ve.pos / g_ve.rate) * 1000000000ull
                            + (g_ve.pos % g_ve.rate) * 1000000000ull / g_ve.rate,
        };
        rt_render(co, g_ve.bus, bn, &ts);
        g_ve.pos += bn;
        if (f + bn > settle) {                     /* the capture window starts inside this block */
            const uint32_t skip = (f < settle) ? (settle - f) : 0u;
            const uint32_t got  = bn - skip;
            const uint32_t at   = f + skip - settle;
            for (uint32_t k = 0; k < nspk; ++k)
                memcpy(feeds + (size_t)k * row + at, g_ve.bus + (size_t)k * bn + skip,
                       sizeof(float) * got);
        }
    }
    return 1;
}

int valid_speaker_feeds(const Layout* L, int panner, const ValidRender* R, const float solve_pos[3],
                        const float src_world[3], double fs, float* feeds, uint32_t n) {
    if (!L || !solve_pos || !src_world || !feeds || fs <= 0.0 || n < 64u) return 0;
    if (L->count < 4u || L->count > (uint32_t)BWA_CHANNELS) return 0;
    /* 343 m/s, not a caller-supplied value: this entry point has no propagation model of its own
     * (the room is the model), and the only thing the engine's speed of sound reaches here is the
     * tracked-alignment geometry. Every caller in the harness uses 343. */
    return ve_render(L, panner, R, solve_pos, src_world, fs, 343.0, feeds, 0u, n);
}

int valid_reference_feeds(const Layout* L, int spk, double fs, float* feeds, uint32_t n) {
    if (!L || !feeds || fs <= 0.0 || n < 64u) return 0;
    if (L->count < 4u || L->count > (uint32_t)BWA_CHANNELS) return 0;
    if (spk < 0 || (uint32_t)spk >= L->count) return 0;
    /* Unit drive on one channel, no panner. Its trim and alignment delay still apply — that IS the
     * physical source the room presents, and leaving them out would measure a different speaker
     * from the one the array actually uses. */
    float gains[BWA_CHANNELS];
    memset(gains, 0, sizeof gains);
    gains[spk] = 1.0f;
    return valid_feeds_from_gains(L, gains, fs, feeds, n);
}

/* Peak-normalize a 19-capsule buffer. Level carries no information here (the estimator reads a
 * normalized direction), but an un-normalized sum of 26 speakers can sit anywhere, so pin the peak
 * where a real capture would be — otherwise zylia_check_capsules would report the whole array as
 * clipping. Shared by the analytic field and the propagated one so both look alike to the checks. */
static void valid_norm19(float* buf, uint32_t n) {
    float peak = 0.0f;
    for (size_t i = 0; i < (size_t)ZYLIA_MICS * n; ++i) {
        float a = buf[i] < 0.0f ? -buf[i] : buf[i];
        if (a > peak) peak = a;
    }
    if (peak > 0.0f) {
        float g = 0.5f / peak;
        for (size_t i = 0; i < (size_t)ZYLIA_MICS * n; ++i) buf[i] *= g;
    }
}

/* The anechoic field at the 19 capsules for an ARBITRARY per-speaker gain vector. Shared by the
 * panned render and the single-speaker reference, so the reference is measured through exactly the
 * same propagation model, not a simplified one. */
static int valid_field(const Layout* L, const float* gains, const float mic[3],
                       double fs, double c, float* buf, uint32_t n) {
    uint32_t nspk = L->count;
    float capd[ZYLIA_MICS][3], R;
    zylia_geometry(capd, &R);

    double phase[VALID_NTONES], freq[VALID_NTONES];
    int nt = valid_stimulus(phase, freq);

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
            /* propagation + the layout's own alignment delay: at an off-center listener that
             * alignment is imperfect, which is a real effect and belongs in the measurement */
            double tau = r / c + (double)L->speakers[i].delay_samples / fs;
            double A   = (double)gains[i] * (double)L->speakers[i].gain_lin / r;
            for (int k = 0; k < nt; ++k) {
                double ang = phase[k] - TWO_PI * freq[k] * tau;
                Cr[j][k] += A * cos(ang);
                Ci[j][k] += A * sin(ang);
            }
        }
    }

    for (int j = 0; j < ZYLIA_MICS; ++j)
        for (uint32_t t = 0; t < n; ++t) buf[(size_t)j * n + t] = 0.0f;

    for (int k = 0; k < nt; ++k) {
        double w = TWO_PI * freq[k] / fs;
        for (uint32_t t = 0; t < n; ++t) {
            double s = sin(w * (double)t), cc = cos(w * (double)t);
            for (int j = 0; j < ZYLIA_MICS; ++j)
                buf[(size_t)j * n + t] += (float)((Cr[j][k]*s + Ci[j][k]*cc) / (double)nt);
        }
    }

    valid_norm19(buf, n);
    return 1;
}

/* Longest propagation path from any speaker to any capsule, in frames — the pre-roll ve_render has
 * to produce so every capsule can read backwards without running off the front of the feed. */
static uint32_t valid_pre_frames(const Layout* L, const float mic[3], double fs, double c) {
    float capd[ZYLIA_MICS][3], R;
    zylia_geometry(capd, &R);
    double rmax = 0.0;
    for (uint32_t i = 0; i < L->count; ++i) {
        double dx = (double)mic[0] - L->speakers[i].pos[0];
        double dy = (double)mic[1] - L->speakers[i].pos[1];
        double dz = (double)mic[2] - L->speakers[i].pos[2];
        double r  = sqrt(dx*dx + dy*dy + dz*dz) + R;    /* + the sphere radius: the far capsule */
        if (r > rmax) rmax = r;
    }
    return (uint32_t)(rmax / c * fs) + 4u;              /* + the cubic tap's own reach */
}

/* Propagate REAL speaker feeds to the 19 capsules, anechoically: 1/r and the exact propagation
 * delay, summed coherently. feeds = [nspk][pre + n]; output frame t of capsule j reads feed frame
 * pre + t - delay.
 *
 * The fractional part is a 4-tap Lagrange (cubic) interpolation. The pre-engine model carried every
 * delay as a phase shift and was exact, which a feed built from one stimulus allows and an engine
 * render does not (dual-band, the spread modes, decorrelation and tracked alignment all make a feed
 * something other than a scaled copy). Cubic costs almost nothing here: the analysis band tops out
 * at 1200 Hz against a 48 kHz rate, where the interpolator is flat to better than 1e-5 dB. */
static int valid_propagate(const Layout* L, const float* feeds, uint32_t pre, uint32_t n,
                           const float mic[3], double fs, double c, float* buf) {
    float capd[ZYLIA_MICS][3], R;
    zylia_geometry(capd, &R);
    const uint32_t nspk = L->count, row = pre + n;

    for (int j = 0; j < ZYLIA_MICS; ++j)
        for (uint32_t t = 0; t < n; ++t) buf[(size_t)j * n + t] = 0.0f;

    /* A silent feed contributes nothing to any of the 19 capsules, so find those once rather than
     * convolving zeros 19 times. VBAP puts a source on two or three speakers and SPCAP at a tight
     * focus is not far off, so this is most of the work on exactly the conditions a sweep multiplies. */
    unsigned char live[BWA_CHANNELS];
    for (uint32_t i = 0; i < nspk; ++i) {
        const float* f = feeds + (size_t)i * row;
        live[i] = 0;
        for (uint32_t t = 0; t < row; ++t) if (f[t] != 0.0f) { live[i] = 1; break; }
    }

    for (int j = 0; j < ZYLIA_MICS; ++j) {
        double mx = (double)mic[0] + R*capd[j][0];
        double my = (double)mic[1] + R*capd[j][1];
        double mz = (double)mic[2] + R*capd[j][2];
        float* out = buf + (size_t)j * n;
        for (uint32_t i = 0; i < nspk; ++i) {
            if (!live[i]) continue;
            double dx = mx - L->speakers[i].pos[0];
            double dy = my - L->speakers[i].pos[1];
            double dz = mz - L->speakers[i].pos[2];
            double r  = sqrt(dx*dx + dy*dy + dz*dz);
            if (r < 0.05) r = 0.05;                   /* a capsule inside a cabinet is not a thing */
            double dl = r / c * fs;
            if (dl < 1.0) dl = 1.0;                   /* the cubic tap reads one sample AHEAD of the
                                                       * integer part, so it needs a frame of slack */
            uint32_t d0 = (uint32_t)dl;
            double   fr = dl - (double)d0;
            if (d0 + 2u > pre) return 0;              /* pre-roll too short: refuse, never read short */
            const float* h = feeds + (size_t)i * row + (size_t)pre - d0;
            const double a = 1.0 / r;
            /* Lagrange-3 through x[-2..+1] around the fractional offset fr */
            const double w_1 = -fr*(fr-1.0)*(fr-2.0)/6.0;
            const double w0  = (fr*fr-1.0)*(fr-2.0)/2.0;
            const double w1  = -fr*(fr+1.0)*(fr-2.0)/2.0;
            const double w2  = fr*(fr*fr-1.0)/6.0;
            for (uint32_t t = 0; t < n; ++t) {
                const float* p = h + t;
                out[t] += (float)(a * (w_1*p[1] + w0*p[0] + w1*p[-1] + w2*p[-2]));
            }
        }
    }
    valid_norm19(buf, n);
    return 1;
}

int valid_simulate(const Layout* L, int panner, const ValidRender* R, const float solve_pos[3],
                   const float mic[3], const float src_world[3], double fs, double c,
                   float* buf, uint32_t n) {
    if (!L || !solve_pos || !mic || !src_world || !buf) return 0;
    if (fs <= 0.0 || c <= 0.0 || n < 64u) return 0;
    if (L->count < 4u || L->count > (uint32_t)BWA_CHANNELS) return 0;
    const uint32_t pre = valid_pre_frames(L, mic, fs, c);
    float* feeds = (float*)malloc(sizeof(float) * (size_t)L->count * (pre + n));
    if (!feeds) return 0;
    int ok = ve_render(L, panner, R, solve_pos, src_world, fs, c, feeds, pre, n) &&
             valid_propagate(L, feeds, pre, n, mic, fs, c, buf);
    free(feeds);
    return ok;
}

int valid_score(const Layout* L, int panner, const ValidRender* R, int tracked, const float mic[3],
                const float src_world[3], const float* cap19, uint32_t n, double fs, double c,
                const unsigned char* exclude, ValidCell* out) {
    if (!L || !mic || !src_world || !cap19 || !out) return 0;

    memset(out, 0, sizeof *out);
    out->panner = panner;
    out->tracked = tracked ? 1 : 0;
    out->mic[0] = mic[0]; out->mic[1] = mic[1]; out->mic[2] = mic[2];
    out->render = valid_resolve(L, R);

    double tx = (double)src_world[0] - mic[0];
    double ty = (double)src_world[1] - mic[1];
    double tz = (double)src_world[2] - mic[2];
    double tn = sqrt(tx*tx + ty*ty + tz*tz);
    if (tn < 1e-6) return 0;                          /* source on top of the listener */
    out->target[0] = (float)(tx/tn); out->target[1] = (float)(ty/tn); out->target[2] = (float)(tz/tn);

    const float* ptr[ZYLIA_MICS];
    for (int j = 0; j < ZYLIA_MICS; ++j) ptr[j] = cap19 + (size_t)j * n;

    float d[3], psi = 0.0f;
    double f_lo, f_hi;
    valid_get_stimulus_band(&f_lo, &f_hi);   /* a tone is scored in its own +-1/6 octave */

    /* Comb depth FIRST, and unconditionally: it is an independent measurement of the same capture, so
     * a cell whose direction the estimator refuses can still report what the render did to the
     * spectrum. A tone's narrow band is refused by zylia_comb_depth on purpose (one frequency cannot
     * show a frequency-dependent effect), which is why comb_ok is its own flag. */
    float cdb = 0.0f, cq = 0.0f;
    if (zylia_comb_depth(ptr, n, fs, f_lo, f_hi, exclude, &cdb, &cq)) {
        out->comb_db = cdb;
        out->comb_q  = cq;
        out->comb_ok = 1;
    }

    if (!zylia_intensity_doa(ptr, n, fs, c, f_lo, f_hi, exclude, d, &psi)) { out->ok = 0; return 1; }

    out->measured[0] = d[0]; out->measured[1] = d[1]; out->measured[2] = d[2];
    out->diffuseness = psi;
    double dot = (double)d[0]*out->target[0] + (double)d[1]*out->target[1] + (double)d[2]*out->target[2];
    if (dot >  1.0) dot =  1.0;
    if (dot < -1.0) dot = -1.0;
    out->miss_deg = (float)(acos(dot) * 180.0 / M_PI);
    out->ok = 1;
    return 1;
}

int valid_cell(const Layout* L, int panner, const ValidRender* R, int tracked, const float mic[3],
               const float src_world[3], double fs, double c, uint32_t n, ValidCell* out) {
    if (!L || !mic || !src_world || !out) return 0;
    float* buf = (float*)malloc(sizeof(float) * (size_t)ZYLIA_MICS * n);
    if (!buf) return 0;
    const float* solve = tracked ? mic : L->ref;
    if (!valid_simulate(L, panner, R, solve, mic, src_world, fs, c, buf, n)) {
        free(buf); return 0;
    }
    int r = valid_score(L, panner, R, tracked, mic, src_world, buf, n, fs, c, NULL, out);
    free(buf);
    return r;
}

int valid_reference_cell(const Layout* L, int spk, const float mic[3],
                         double fs, double c, uint32_t n, ValidCell* out) {
    if (!L || !mic || !out) return 0;
    if (spk < 0 || (uint32_t)spk >= L->count) return 0;

    float gains[BWA_CHANNELS];
    memset(gains, 0, sizeof gains);
    gains[spk] = 1.0f;

    float* buf = (float*)malloc(sizeof(float) * (size_t)ZYLIA_MICS * n);
    if (!buf) return 0;
    if (!valid_field(L, gains, mic, fs, c, buf, n)) { free(buf); return 0; }

    /* The speaker's own surveyed position IS the target. A reference miss therefore also prices in
     * survey error, which is correct: a layout that misplaces a speaker misaims every phantom too. */
    float src[3] = { L->speakers[spk].pos[0], L->speakers[spk].pos[1], L->speakers[spk].pos[2] };
    int r = valid_score(L, 0, NULL, 0, mic, src, buf, n, fs, c, NULL, out);
    free(buf);
    /* No panner and no knob ran, so every render column is meaningless here — blank them rather than
     * report the array's defaults on a row that never used them. */
    if (r) { out->reference = 1; out->tgt = spk; memset(&out->render, 0, sizeof out->render); }
    return r;
}

int valid_re_proxy(const Layout* L, int panner, const float solve_pos[3], const float mic[3],
                   const float src_world[3], float focus, float density,
                   float* re_err_deg, float* spread_deg) {
    if (!L || !solve_pos || !mic || !src_world) return 0;
    uint32_t nspk = L->count;
    if (nspk < 4u || nspk > (uint32_t)BWA_CHANNELS) return 0;

    float gains[BWA_CHANNELS];
    memset(gains, 0, sizeof gains);
    valid_gains(L, panner, src_world, solve_pos, focus, density, gains);

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

int valid_run(const Layout* L, const int* panners, int npan, const ValidRender* R, int tracked,
              const float (*listeners)[3], int nlis,
              const float (*targets)[3], int ntgt,
              float radius, double fs, double c,
              uint32_t n, ValidCell* cells_out) {
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
                if (!valid_cell(L, panners[p], R, tracked, listeners[li], src, fs, c, n, cc)) {
                    memset(cc, 0, sizeof *cc);
                    cc->panner = panners[p];
                    cc->tracked = tracked ? 1 : 0;
                    cc->render = valid_resolve(L, R);
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
