/*
 * calib_capture.cpp — see calib_capture.h. Moved verbatim out of calibrate.cpp so the CLI and
 * bwa_calib_view's Capture tab share ONE copy of the sweep-capture backends.
 */
#include "calib_capture.h"
#include "calib/calib.h"                   /* the live reading: CALIB_AIM_* bands, calib_direct_tilt_db */
#include "dsp/sos.h"                       /* BWA_SOS_REF_MPS + the plausible-c guard */
#include "dsp/fft.h"                       /* the simulated room's late tail (one convolution per run) */
extern "C" {
#include "spatial/align.h"                /* --verify: the engine's own output stage */
}

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <thread>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void calib_write_wav_f32(const char* path, const float* x, int n, int fs) {
    FILE* f = fopen(path, "wb"); if (!f) return;
    int br = fs * 4, ds = n * 4, sz16 = 16, rc = 36 + ds; short fmt = 3, ch = 1, bps = 32, ba = 4;
    fwrite("RIFF",1,4,f); fwrite(&rc,4,1,f); fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f); fwrite(&sz16,4,1,f); fwrite(&fmt,2,1,f); fwrite(&ch,2,1,f);
    fwrite(&fs,4,1,f); fwrite(&br,4,1,f); fwrite(&ba,2,1,f); fwrite(&bps,2,1,f);
    fwrite("data",1,4,f); fwrite(&ds,4,1,f); fwrite(x,4,(size_t)n,f); fclose(f);
}

static float g_sim_aim_err_deg = 0.f;
void calib_sim_set_aim_error(float deg) { g_sim_aim_err_deg = (deg == deg) ? deg : 0.f; }

static float g_room_alpha = 0.f;         /* 0 = anechoic */
void calib_sim_set_room(float absorption) {
    g_room_alpha = (absorption > 0.f && absorption <= 1.f) ? absorption : 0.f;
}

/* ---- the simulated room (calib_sim_set_room) ---- */

namespace {
struct SimRoom {
    double lo[3], hi[3], w[3];           /* the shoebox, room meters */
    double V, S, rt60, mfp;              /* volume, surface, Sabine RT60 (s), mean free path (m) */
};

void room_geometry(const Layout* L, SimRoom* r) {
    for (int a = 0; a < 3; ++a) { r->lo[a] = 1e30; r->hi[a] = -1e30; }
    for (uint32_t k = 0; k < L->count; ++k)
        for (int a = 0; a < 3; ++a) {
            if (L->speakers[k].pos[a] < r->lo[a]) r->lo[a] = L->speakers[k].pos[a];
            if (L->speakers[k].pos[a] > r->hi[a]) r->hi[a] = L->speakers[k].pos[a];
        }
    for (int a = 0; a < 3; ++a) {
        r->lo[a] -= CALIB_SIM_ROOM_MARGIN_M; r->hi[a] += CALIB_SIM_ROOM_MARGIN_M;
        r->w[a] = r->hi[a] - r->lo[a];
    }
    r->V = r->w[0] * r->w[1] * r->w[2];
    r->S = 2.0 * (r->w[0] * r->w[1] + r->w[1] * r->w[2] + r->w[0] * r->w[2]);
    const double a = g_room_alpha > 0.f ? (double)g_room_alpha : 1.0;
    r->rt60 = 0.161 * r->V / (r->S * a);
    r->mfp  = 4.0 * r->V / r->S;
}

/* the exponential sweep's own time axis -> frequency (Farina; measure_sweep's constants), for a
 * sweep of nsw samples */
inline double sweep_freq(double i, int nsw) { return CAL_F1 * pow(CAL_F2 / CAL_F1, i / (double)nsw); }

/* Add amp * sweep(i - frac) * gain(i) into cap (ncap samples) starting at di, for a sweep of nsw
 * samples: measure_sweep's formula (fades included) evaluated analytically, like the direct sound
 * below. `knots` (NULL = 1) is a gain curve sampled every KNOT sweep samples, linearly interpolated:
 * an image's directivity loss changes slowly along the sweep, and evaluating the model per sample for
 * 24 images is the whole cost. The exponential is stepped by multiplication (one rounding per
 * sample, 1e-11 relative after 72000). */
const int KNOT = 64;
const int NKNOT = CAL_NSWEEP / KNOT + 2;
void add_sweep(float* cap, int di, double frac, double amp, const float* knots, int nsw, int ncap) {
    const double w1 = 2.0 * M_PI * CAL_F1 / CAL_FS, w2 = CAL_F2 * 2.0 * M_PI / CAL_FS;
    const double T = (double)nsw, Lg = log(w2 / w1), Kp = T * w1 / Lg;
    const int fade = (int)(0.005 * CAL_FS);
    const double step = exp(Lg / T);
    double e = exp(-frac * Lg / T);                  /* exp(x / T * Lg) at x = -frac */
    for (int i = 0; i < nsw && di + i < ncap; ++i, e *= step) {
        const double x = (double)i - frac;
        if (x < 0.0 || di + i < 0) continue;
        double v = sin(Kp * (e - 1.0));
        if (x < fade)           v *= 0.5 - 0.5 * cos(M_PI * x / fade);
        if (x > nsw - 1 - fade) v *= 0.5 - 0.5 * cos(M_PI * (nsw - 1 - x) / fade);
        double g = amp;
        if (knots) { const int kk = i / KNOT; const double t = (double)(i - kk * KNOT) / KNOT; g *= knots[kk] + (knots[kk + 1] - knots[kk]) * t; }
        cap[di + i] += (float)(g * v);
    }
}

/* What the options put on every path out of the speaker, as an amplitude at f: the speaker's own
 * on-axis response (the model's on_axis_db, relative to its 1 kHz value so the simulated level stays
 * where the flat speaker put it), times a screen's high-frequency loss, a first-order-like shelf
 * that reaches `screen_db` well above CALIB_SIM_SCREEN_HZ. 1 when both are off. */
double resp_amp(const Directivity* d, int on_axis, float screen_db, double f) {
    double db = 0.0;
    if (on_axis && d->nband && d->has_on_axis)
        db += (double)directivity_on_axis_db_at(d, (float)f) - (double)directivity_on_axis_db_at(d, 1000.f);
    if (screen_db != 0.f) {
        const double x = (f / CALIB_SIM_SCREEN_HZ) * (f / CALIB_SIM_SCREEN_HZ);
        db -= (double)screen_db * x / (1.0 + x);
    }
    return db == 0.0 ? 1.0 : pow(10.0, db / 20.0);
}

/* The late tail's contribution to a capture, for a unit-sensitivity speaker whose direct sound
 * lands at sample 0: (sweep shaped by the model's power response) convolved with an exponentially
 * decaying deterministic noise, energy-normalized to the room equation's diffuse level. Depends only
 * on the room, the absorption, the model and the sweep and capture lengths, so it is built once per
 * run and reused for every capture; `onset` is where it starts after the direct sound. */
struct TailCache {
    int    valid;
    double key[9];                        /* absorption, the box's three widths, the model's band count,
                                           * the sweep and capture lengths, the on-axis and screen options */
    int    onset;
    float* cap;                           /* CAL_CAPLEN */
} g_tail = { 0, { 0, 0, 0, 0, 0, 0, 0, 0, 0 }, 0, NULL };

/* the model's power response relative to on axis at f: the mean of 10^(loss/10) over the sphere,
 * axisymmetric, so weighted by sin(theta); 1 without a model */
double power_response(const Directivity* d, double f) {
    if (!d->nband) return 1.0;
    double acc = 0.0, wsum = 0.0;
    for (int t = 0; t < 180; ++t) {
        const double th = (t + 0.5), w = sin(th * M_PI / 180.0);
        acc  += w * pow(10.0, directivity_loss_db_at(d, (float)th, (float)f) / 10.0);
        wsum += w;
    }
    return acc / wsum;
}

const float* room_tail(const Layout* L, const SimRoom* r, double sos, int nsw, int ncap, int on_axis, float screen_db) {
    const double key[9] = { (double)g_room_alpha, r->w[0], r->w[1], r->w[2], (double)L->dir.nband,
                            (double)nsw, (double)ncap, (double)on_axis, (double)screen_db };
    if (g_tail.valid && !memcmp(key, g_tail.key, sizeof key)) return g_tail.cap;
    const double a = (double)g_room_alpha;
    const int onset = (int)(2.0 * r->mfp / sos * CAL_FS);
    const int ntail = ncap - onset > 16 ? ncap - onset : 16;
    int L2 = 1; while (L2 < nsw + ntail) L2 <<= 1;
    double* sre = (double*)calloc((size_t)L2, sizeof(double));
    double* sim = (double*)calloc((size_t)L2, sizeof(double));
    double* tre = (double*)calloc((size_t)L2, sizeof(double));
    double* tim = (double*)calloc((size_t)L2, sizeof(double));
    if (!g_tail.cap) g_tail.cap = (float*)calloc((size_t)CAL_CAPLEN, sizeof(float));
    if (!sre || !sim || !tre || !tim || !g_tail.cap) {
        free(sre); free(sim); free(tre); free(tim); return NULL;
    }
    /* the sweep, each sample scaled by sqrt(power response) at its instantaneous frequency (knots),
     * and by the speaker's on-axis response when the option carries one */
    static float knots[NKNOT];
    for (int k = 0; k < NKNOT; ++k) {
        const double f = sweep_freq((double)k * KNOT, nsw);
        knots[k] = (float)(sqrt(power_response(&L->dir, f)) * resp_amp(&L->dir, on_axis, screen_db, f));
    }
    static float sw[CAL_NSWEEP];
    memset(sw, 0, sizeof sw);
    add_sweep(sw - 0, 0, 0.0, 1.0, knots, nsw, nsw);   /* di 0, frac 0: the sweep itself, shaped */
    for (int i = 0; i < nsw; ++i) sre[i] = sw[i];
    /* the tail IR: uniform deterministic noise under exp(-6.91 t / RT60), a 5 ms raised-cosine rise,
     * scaled so its energy is the diffuse field's share past order 2 */
    uint32_t seed = 0x5eedu;
    const double tau = r->rt60 / (3.0 * log(10.0));   /* amplitude e-folding time */
    const int rise = (int)(0.005 * CAL_FS);
    double e = 0.0;
    for (int i = 0; i < ntail; ++i) {
        seed = seed * 1664525u + 1013904223u;
        double v = ((double)(seed >> 8) / 8388608.0 - 1.0) * exp(-(double)i / CAL_FS / tau);
        if (i < rise) v *= 0.5 - 0.5 * cos(M_PI * (double)i / rise);
        tre[i] = v; e += v * v;
    }
    const double e_tail = 16.0 * M_PI * (1.0 - a) / (r->S * a) * (1.0 - a) * (1.0 - a);
    const double scale = e > 0.0 ? sqrt(e_tail / e) : 0.0;
    fft(sre, sim, L2, +1);
    fft(tre, tim, L2, +1);
    for (int k = 0; k < L2; ++k) {
        const double re = sre[k] * tre[k] - sim[k] * tim[k], im = sre[k] * tim[k] + sim[k] * tre[k];
        sre[k] = re; sim[k] = im;
    }
    fft(sre, sim, L2, -1);
    for (int i = 0; i < CAL_CAPLEN; ++i) g_tail.cap[i] = (float)(i < L2 && i < ncap ? sre[i] * scale : 0.0);
    free(sre); free(sim); free(tre); free(tim);
    memcpy(g_tail.key, key, sizeof key);
    g_tail.onset = onset;
    g_tail.valid = 1;
    return g_tail.cap;
}
} /* namespace */

/* ---- the simulated interferer (calib_sim_set_interferer) ---- */

static int  g_intf_caps[CALIB_SIM_INTF_MAX];
static int  g_intf_n, g_intf_count, g_intf_hits;
static CalibSimInterferer g_intf;

void calib_sim_set_interferer(const int* captures, int n, const CalibSimInterferer* it) {
    g_intf_count = g_intf_hits = 0;
    g_intf_n = 0;
    if (!captures || !it || n <= 0) return;
    if (n > CALIB_SIM_INTF_MAX) n = CALIB_SIM_INTF_MAX;
    memcpy(g_intf_caps, captures, (size_t)n * sizeof *captures);
    g_intf = *it;
    g_intf_n = n;
}
int calib_sim_capture_count(void)   { return g_intf_count; }
int calib_sim_interferer_hits(void) { return g_intf_hits; }

int calib_sim_parse_interferer(const char* spec, int* captures, int cap, CalibSimInterferer* it, char* err, size_t errcap) {
    if (err && errcap) err[0] = 0;
    if (!spec || !captures || !it || cap <= 0) return 0;
    it->t_s = 0.9f; it->level_db = 12.f; it->kind = CALIB_SIM_INTF_CLICK;
    int n = 0;
    const char* q = spec;
    for (;;) {
        char* end = NULL;
        const long v = strtol(q, &end, 10);
        if (end == q || v < 1 || v > 100000) { if (err) snprintf(err, errcap, "'%s': captures are 1-based numbers", spec); return 0; }
        if (n >= cap) { if (err) snprintf(err, errcap, "'%s': more than %d captures", spec, cap); return 0; }
        captures[n++] = (int)v;
        q = end;
        if (*q == ',') { ++q; continue; }
        break;
    }
    if (*q == ':') {
        char* end = NULL;
        const double t = strtod(q + 1, &end);
        if (end == q + 1 || !(t >= 0.0 && t < CAL_CAPLEN / CAL_FS)) {
            if (err) snprintf(err, errcap, "'%s': the time is seconds into the capture, 0 to %.1f", spec, CAL_CAPLEN / CAL_FS); return 0; }
        it->t_s = (float)t; q = end;
        if (*q == ':') {
            const double l = strtod(q + 1, &end);
            if (end == q + 1 || !(l >= -60.0 && l <= 60.0)) {
                if (err) snprintf(err, errcap, "'%s': the level is dB, -60 to 60", spec); return 0; }
            it->level_db = (float)l; q = end;
            if (*q == ':') {
                ++q;
                if      (!strcmp(q, "click")) it->kind = CALIB_SIM_INTF_CLICK;
                else if (!strcmp(q, "noise")) it->kind = CALIB_SIM_INTF_NOISE;
                else if (!strcmp(q, "sweep")) it->kind = CALIB_SIM_INTF_SWEEP;
                else { if (err) snprintf(err, errcap, "'%s': the kind is click, noise or sweep", spec); return 0; }
                q += strlen(q);
            }
        }
    }
    if (*q) { if (err) snprintf(err, errcap, "'%s': expected captures[:t_s[:level_db[:click|noise|sweep]]]", spec); return 0; }
    return n;
}

/* Count one capture and say whether the interferer sounds in it. */
static int intf_this_capture(void) {
    const int k = ++g_intf_count;
    for (int i = 0; i < g_intf_n; ++i) if (g_intf_caps[i] == k) { ++g_intf_hits; return 1; }
    return 0;
}

/* Add the armed interferer to one row heard at `at`, the capture point being `center` (the source
 * stands at center + CALIB_SIM_INTF_OFFSET). nsw: the capture's sweep length (the sweep kind plays it).
 * The event's noise comes from a fixed seed, so a run is repeatable and every capsule hears the same
 * event. */
static void intf_add(float* row, int ncap, const float at[3], const float center[3], double sos, int nsw) {
    const double src[3] = { center[0] + CALIB_SIM_INTF_OFFSET_X, center[1] + CALIB_SIM_INTF_OFFSET_Y, center[2] + CALIB_SIM_INTF_OFFSET_Z };
    const double rc = sqrt((double)CALIB_SIM_INTF_OFFSET_X * CALIB_SIM_INTF_OFFSET_X + (double)CALIB_SIM_INTF_OFFSET_Y * CALIB_SIM_INTF_OFFSET_Y +
                           (double)CALIB_SIM_INTF_OFFSET_Z * CALIB_SIM_INTF_OFFSET_Z);
    const double dx = at[0] - src[0], dy = at[1] - src[1], dz = at[2] - src[2];
    double r = sqrt(dx * dx + dy * dy + dz * dz);
    if (r < 0.05) r = 0.05;
    if (!(sos >= BWA_SOS_MIN_MPS && sos <= BWA_SOS_MAX_MPS)) sos = BWA_SOS_REF_MPS;
    const double rms = pow(10.0, g_intf.level_db / 20.0) * rc / r;      /* level_db is at the capture point */
    const double t0 = (double)g_intf.t_s * CAL_FS + r / sos * CAL_FS;
    const int i0 = (int)t0;
    if (g_intf.kind == CALIB_SIM_INTF_SWEEP) {
        add_sweep(row, i0, t0 - i0, rms * sqrt(2.0), NULL, nsw, ncap);   /* the sweep's RMS is its peak / sqrt 2 */
        return;
    }
    const int len = (int)((g_intf.kind == CALIB_SIM_INTF_CLICK ? 0.002 : 0.25) * CAL_FS);
    uint32_t seed = 0x1f7e5eedu;
    const double amp = rms * sqrt(3.0) / sqrt(0.375);              /* uniform noise (rms 1/sqrt3) under a Hann (mean w^2 3/8) */
    for (int i = 0; i < len; ++i) {
        seed = seed * 1664525u + 1013904223u;
        if (i0 + i < 0 || i0 + i >= ncap) continue;
        const double u = (double)(seed >> 8) / 8388608.0 - 1.0;
        const double w = 0.5 - 0.5 * cos(2.0 * M_PI * (i + 0.5) / len);
        row[i0 + i] += (float)(amp * u * w);
    }
}

void calib_sim_room_describe(const Layout* L, char* buf, size_t cap) {
    if (!buf || cap == 0) return;
    if (!L || !(g_room_alpha > 0.f)) { snprintf(buf, cap, "anechoic"); return; }
    SimRoom r; room_geometry(L, &r);
    snprintf(buf, cap, "shoebox %.2f x %.2f x %.2f m (the array + %.1f m), absorption %.2f, Sabine RT60 %.2f s, "
             "24 image sources (orders 1-2) + a late tail from two mean free paths (%.1f m) on",
             r.w[0], r.w[1], r.w[2], (double)CALIB_SIM_ROOM_MARGIN_M, (double)g_room_alpha, r.rt60, 2.0 * r.mfp);
}

void calib_sim_rotate(const float in[3], float deg, float out[3]) {
    const float a0 = in[0], a1 = in[1], a2 = in[2];
    out[0] = a0; out[1] = a1; out[2] = a2;
    if (deg == 0.f || !(deg == deg)) return;
    float up[3] = { 0, 1, 0 };                    /* rotate about a perpendicular, deterministic axis */
    if (fabsf(a1) > 0.9f) { up[0] = 1; up[1] = 0; }
    float u[3] = { a1*up[2] - a2*up[1], a2*up[0] - a0*up[2], a0*up[1] - a1*up[0] };
    float l = sqrtf(u[0]*u[0] + u[1]*u[1] + u[2]*u[2]);
    if (l > 1e-6f) {
        float ang = deg * 3.14159265f / 180.f, ca = cosf(ang), sa = sinf(ang) / l;
        out[0] = ca * a0 + sa * u[0]; out[1] = ca * a1 + sa * u[1]; out[2] = ca * a2 + sa * u[2];
    }
}

/* calib_sim_set_truth: where the simulated speakers really stand, when that is not the layout's word */
static const Layout* g_sim_truth = NULL;
void calib_sim_set_truth(const Layout* T) { g_sim_truth = (T && T->count) ? T : NULL; }
static const Speaker* sim_speaker(const Layout* L, int ch) {
    return (g_sim_truth && (uint32_t)ch < g_sim_truth->count) ? &g_sim_truth->speakers[ch] : &L->speakers[ch];
}

/* calib_sim_set_speaker_latency: a per-speaker extra latency, in seconds, on top of the simulator's own */
static double g_sim_spk_lat_s[BWA_MAX_CHANNELS];
void calib_sim_set_speaker_latency(int ch, double seconds) {
    if (ch < 0 || ch >= BWA_MAX_CHANNELS || !(seconds == seconds)) return;
    g_sim_spk_lat_s[ch] = seconds;
}
double calib_sim_speaker_latency(int ch) {
    return (ch >= 0 && ch < BWA_MAX_CHANNELS) ? g_sim_spk_lat_s[ch] : 0.0;
}

/* the rotated (simulated-error) acoustic axis of speaker ch */
static void sim_aim(const Layout* L, int ch, float aim[3]) {
    calib_sim_rotate(sim_speaker(L, ch)->aim, g_sim_aim_err_deg, aim);
}

double calib_sim_sensitivity(int ch) { return 1.0 + 0.15 * sin(ch * 1.3); }

void calib_sim_capture(int ch, const Layout* L, const float mic[3], double sos, const float* sweep, float* cap) {
    (void)sweep;
    calib_sim_capture_ex(ch, L, mic, sos, NULL, cap);
}

/* the capture's sweep and capture lengths under the options */
static void sim_lengths(const CalibSimOpts* o, int* nsw, int* ncap) {
    *nsw  = (o && o->nsweep > 0) ? o->nsweep : CAL_NSWEEP;
    *ncap = (o && o->ncap > 0)   ? o->ncap   : CAL_CAPLEN;
    if (*nsw > CAL_NSWEEP) *nsw = CAL_NSWEEP;
    if (*ncap > CAL_CAPLEN) *ncap = CAL_CAPLEN;
}

static void sim_capture_core(int ch, const Layout* L, const float mic[3], double sos, const CalibSimOpts* o, float* cap);

void calib_sim_capture_ex(int ch, const Layout* L, const float mic[3], double sos, const CalibSimOpts* o, float* cap) {
    sim_capture_core(ch, L, mic, sos, o, cap);
    if (intf_this_capture()) {
        int nsw, ncap; sim_lengths(o, &nsw, &ncap);
        intf_add(cap, ncap, mic, mic, sos, nsw);
    }
}

static void sim_capture_core(int ch, const Layout* L, const float mic[3], double sos, const CalibSimOpts* o, float* cap) {
    int nsw  = (o && o->nsweep > 0) ? o->nsweep : CAL_NSWEEP;
    int ncap = (o && o->ncap > 0)   ? o->ncap   : CAL_CAPLEN;
    if (nsw > CAL_NSWEEP) nsw = CAL_NSWEEP;
    if (ncap > CAL_CAPLEN) ncap = CAL_CAPLEN;
    const int on_axis = (o && o->on_axis) ? 1 : 0;
    const float screen_db = (o && o->screen_db > 0.f && o->screen_db < 40.f) ? o->screen_db : 0.f;
    const int shaped = on_axis || screen_db != 0.f;     /* a response on every path besides the model's */
    memset(cap, 0, (size_t)ncap * sizeof(float));
    const float* p = (o && o->true_pos) ? o->true_pos : sim_speaker(L, ch)->pos;
    double dist = sqrt((p[0]-mic[0])*(p[0]-mic[0]) + (p[1]-mic[1])*(p[1]-mic[1]) + (p[2]-mic[2])*(p[2]-mic[2]));
    if (dist < 0.05) dist = 0.05;
    /* Directivity, when the layout carries a model: the mic's bearing off the speaker's TRUE axis
     * (the layout's, rotated by the sim-only error, or the caller's) sets a per-FREQUENCY loss,
     * applied to the sweep sample by sample at its instantaneous frequency. An exponential sweep maps
     * time to frequency exactly (Farina: f(i) = f1 (f2/f1)^(i/N), measure_sweep's phase constants),
     * so this is a frequency-dependent gain with no filter, and the deconvolved IR carries the
     * model's tilt. The on-axis option multiplies in the speaker's own response the same way. */
    static float dloss[CAL_NSWEEP];               /* 288 KB: static, off the caller's (possibly a
                                                    * std::thread's) stack; one capture at a time */
    int have_dir = L->dir.nband != 0;
    const int per_sample = have_dir || screen_db != 0.f;   /* dloss carries a frequency-dependent gain */
    float aim[3];
    if (o && o->true_aim) {
        const float* t = o->true_aim;
        const float l = sqrtf(t[0]*t[0] + t[1]*t[1] + t[2]*t[2]);
        if (l > 1e-6f) { aim[0] = t[0] / l; aim[1] = t[1] / l; aim[2] = t[2] / l; }
        else sim_aim(L, ch, aim);
    } else sim_aim(L, ch, aim);
    if (per_sample) {
        float dx = (mic[0]-p[0]) / (float)dist, dy = (mic[1]-p[1]) / (float)dist, dz = (mic[2]-p[2]) / (float)dist;
        float c = dx*aim[0] + dy*aim[1] + dz*aim[2];
        if (c > 1.f) c = 1.f; else if (c < -1.f) c = -1.f;
        float theta = acosf(c) * 180.f / 3.14159265f;
        const double ratio = CAL_F2 / CAL_F1;
        for (int i = 0; i < nsw; ++i) {
            double f = CAL_F1 * pow(ratio, (double)i / (double)nsw);
            dloss[i] = powf(10.f, directivity_loss_db_at(&L->dir, theta, (float)f) / 20.f);   /* 1 with no model */
            if (shaped) dloss[i] *= (float)resp_amp(&L->dir, on_axis, screen_db, f);
        }
    }
    /* MUST be the same c the caller's analyzer divides back out (range = c * delay). Pinning this to
     * 343.0 while the analyzer follows the layout's recorded temperature splits the matched pair and
     * inflates every solved position by their ratio — silently, and --localize writes it back. */
    if (!(sos >= BWA_SOS_MIN_MPS && sos <= BWA_SOS_MAX_MPS)) sos = BWA_SOS_REF_MPS;
    /* system latency (plus this box's own extra, --sim-speaker-latency) + time of flight (fractional) */
    const double lat_f = (double)CAL_SIM_LATENCY_SAMPLES + calib_sim_speaker_latency(ch) * CAL_FS;
    double delay_f = lat_f + dist / sos * CAL_FS;
    int    di = (int)delay_f; float frac = (float)(delay_f - di);
    double sens = calib_sim_sensitivity(ch);                      /* deterministic +/- ~1.4 dB wobble */
    float  g    = (float)(sens / dist);                           /* 1/r at the mic */
    /* The fractional part of the delay is applied ANALYTICALLY: the capture is the sweep formula
     * (measure_sweep's, fades included) evaluated at i - frac, not the passed array interpolated.
     * A two-tap linear interpolation is a low-pass whose treble loss follows the fraction (-6 dB at
     * 16 kHz at a half sample), which put a position-dependent tilt into every simulated capture and
     * read as an aim error in --check-aim. A real ADC has no such term, so the simulator must not. */
    const double w1 = 2.0 * M_PI * CAL_F1 / CAL_FS, w2 = CAL_F2 * 2.0 * M_PI / CAL_FS;
    const double T = (double)nsw, Lg = log(w2 / w1), Kp = T * w1 / Lg;
    const int fade = (int)(0.005 * CAL_FS);
    for (int i = 0; i < nsw && di + i < ncap; ++i) {
        double x = (double)i - (double)frac;                        /* the sweep's own time axis */
        if (x < 0.0) continue;
        double v = sin(Kp * (exp(x / T * Lg) - 1.0));
        if (x < fade)           v *= 0.5 - 0.5 * cos(M_PI * x / fade);
        if (x > nsw - 1 - fade) v *= 0.5 - 0.5 * cos(M_PI * (nsw - 1 - x) / fade);
        cap[di + i] += g * (float)v * (per_sample ? dloss[i] : 1.f);
    }
    if (!(g_room_alpha > 0.f)) return;

    /* the simulated room: image sources of orders 1 and 2 off the shoebox, then the late tail. The
     * shoebox is the LAYOUT's (the room does not move with a speaker the caller displaced). */
    SimRoom r; room_geometry(L, &r);
    const double R = sqrt(1.0 - (double)g_room_alpha);           /* pressure reflection per bounce */
    static float knots[NKNOT];
    for (int nx = -2; nx <= 2; ++nx) for (int ny = -2; ny <= 2; ++ny) for (int nz = -2; nz <= 2; ++nz) {
        const int n[3] = { nx, ny, nz };
        const int order = abs(nx) + abs(ny) + abs(nz);
        if (order < 1 || order > 2) continue;
        /* mirror the speaker (and its axis) through the walls: an even index is a translation by
         * whole room widths, an odd one a reflection across the far (n > 0) or near (n < 0) wall */
        double ip[3], ia[3];
        for (int a = 0; a < 3; ++a) {
            const double x = p[a];
            if ((n[a] & 1) == 0) { ip[a] = x + n[a] * r.w[a]; ia[a] = aim[a]; }
            else {
                ip[a] = (n[a] > 0) ? 2.0 * r.hi[a] - x + (n[a] - 1) * r.w[a] : 2.0 * r.lo[a] - x + (n[a] + 1) * r.w[a];
                ia[a] = -aim[a];
            }
        }
        const double vx = mic[0] - ip[0], vy = mic[1] - ip[1], vz = mic[2] - ip[2];
        double ri = sqrt(vx * vx + vy * vy + vz * vz);
        if (ri < 0.05) ri = 0.05;
        /* the departure angle: the mirrored axis against the mirrored ray, which is the real ray
         * leaving the real speaker toward the wall */
        double c = (vx * ia[0] + vy * ia[1] + vz * ia[2]) / ri;
        if (c > 1.0) c = 1.0; else if (c < -1.0) c = -1.0;
        const float th = (float)(acos(c) * 180.0 / M_PI);
        const float* kp = NULL;
        if (per_sample) {
            for (int k = 0; k < NKNOT; ++k) {
                const double f = sweep_freq((double)k * KNOT, nsw);
                knots[k] = powf(10.f, directivity_loss_db_at(&L->dir, th, (float)f) / 20.f);
                if (shaped) knots[k] *= (float)resp_amp(&L->dir, on_axis, screen_db, f);
            }
            kp = knots;
        }
        const double d_img = lat_f + ri / sos * CAL_FS;
        const int dii = (int)d_img;
        add_sweep(cap, dii, d_img - dii, sens / ri * pow(R, order), kp, nsw, ncap);
    }
    const float* tail = room_tail(L, &r, sos, nsw, ncap, on_axis, screen_db);
    if (tail) {
        const int off = di + g_tail.onset;
        for (int i = 0; off + i < ncap; ++i) cap[off + i] += (float)sens * tail[i];
    }
}

void calib_sim_capture_zylia(int ch, const Layout* L, const float center[3], const float (*caps)[3], int ncaps,
                             double sos, const float* sweep, float* cap19) {
    (void)sweep;
    calib_sim_capture_zylia_ex(ch, L, center, caps, ncaps, sos, NULL, cap19, CAL_CAPLEN);
}

void calib_sim_capture_zylia_ex(int ch, const Layout* L, const float center[3], const float (*caps)[3], int ncaps,
                                double sos, const CalibSimOpts* o, float* cap19, int stride) {
    for (int j = 0; j < ncaps; ++j) {
        const float at[3] = { center[0] + caps[j][0], center[1] + caps[j][1], center[2] + caps[j][2] };
        sim_capture_core(ch, L, at, sos, o, cap19 + (size_t)j * (size_t)stride);
    }
    if (intf_this_capture()) {                          /* one capture: every capsule hears the one event */
        int nsw, ncap; sim_lengths(o, &nsw, &ncap);
        for (int j = 0; j < ncaps; ++j) {
            const float at[3] = { center[0] + caps[j][0], center[1] + caps[j][1], center[2] + caps[j][2] };
            intf_add(cap19 + (size_t)j * (size_t)stride, ncap, at, center, sos, nsw);
        }
    }
}

/* ---- the simulated physical ZM-1 (calib_sim_zm1_*) ----
 * Its own rotation code, v' = v + 2w (u x v) + 2 u x (u x v) on a unit xyzw quaternion, written out here
 * rather than borrowed from zylia_quat_to_matrix or mic_track: a truth built with the solve's own
 * rotation could share its mistake. */
static void zm1_qrot(const double q[4], const double v[3], double o[3]) {
    const double u[3] = { q[0], q[1], q[2] }, w = q[3];
    const double t[3] = { 2.0 * (u[1]*v[2] - u[2]*v[1]), 2.0 * (u[2]*v[0] - u[0]*v[2]), 2.0 * (u[0]*v[1] - u[1]*v[0]) };
    o[0] = v[0] + w * t[0] + (u[1]*t[2] - u[2]*t[1]);
    o[1] = v[1] + w * t[1] + (u[2]*t[0] - u[0]*t[2]);
    o[2] = v[2] + w * t[2] + (u[0]*t[1] - u[1]*t[0]);
}

void calib_sim_zm1_body(float caps[ZYLIA_MICS][3]) {
    float b[ZYLIA_MICS][3];
    zylia_builtin_capsules(b);                           /* never the installed survey */
    float r0 = 0.f;
    for (int i = 0; i < ZYLIA_MICS; ++i) r0 += sqrtf(b[i][0]*b[i][0] + b[i][1]*b[i][1] + b[i][2]*b[i][2]) / ZYLIA_MICS;
    const double sc = (double)CALIB_SIM_ZM1_RADIUS_M / (double)r0;
    const double hy = 0.5 * CALIB_SIM_ZM1_MOUNT_YAW_DEG * M_PI / 180.0, hx = 0.5 * CALIB_SIM_ZM1_MOUNT_TILT_DEG * M_PI / 180.0;
    const double qy[4] = { 0.0, sin(hy), 0.0, cos(hy) }, qx[4] = { sin(hx), 0.0, 0.0, cos(hx) };
    for (int i = 0; i < ZYLIA_MICS; ++i) {
        const double v[3] = { b[i][0] * sc, b[i][1] * sc, b[i][2] * sc };
        double t[3], o[3];
        zm1_qrot(qx, v, t);                              /* tilted about the mount's x, then yawed */
        zm1_qrot(qy, t, o);
        for (int a = 0; a < 3; ++a) caps[i][a] = (float)o[a];
    }
}

void calib_sim_zm1_room(const float q[4], float caps[ZYLIA_MICS][3]) {
    double qq[4];
    if (q) {
        double n = 0.0;
        for (int a = 0; a < 4; ++a) { qq[a] = q[a]; n += qq[a] * qq[a]; }
        n = sqrt(n);
        if (!(n > 1e-9)) { qq[0] = qq[1] = qq[2] = 0.0; qq[3] = 1.0; n = 1.0; }
        for (int a = 0; a < 4; ++a) qq[a] /= n;
    } else {
        const double h = 0.5 * CALIB_SIM_ZM1_UNTRACKED_YAW_DEG * M_PI / 180.0;
        qq[0] = 0.0; qq[1] = sin(h); qq[2] = 0.0; qq[3] = cos(h);
    }
    float b[ZYLIA_MICS][3];
    calib_sim_zm1_body(b);
    for (int i = 0; i < ZYLIA_MICS; ++i) {
        const double v[3] = { b[i][0], b[i][1], b[i][2] };
        double o[3];
        zm1_qrot(qq, v, o);
        for (int a = 0; a < 3; ++a) caps[i][a] = (float)o[a];
    }
}

void calib_measure_rows(const float* rows, int nrows, int stride, int ncap, const float* sweep, int nsweep,
                        const double band_hz[2], MeasureResult* res, int* ok) {
    unsigned hw = std::thread::hardware_concurrency();
    const int nw = hw < 2 ? 1 : (hw > 8 ? 8 : (int)hw);
    std::atomic<int> next(0);
    auto work = [&]() {
        for (int j; (j = next.fetch_add(1)) < nrows; )
            ok[j] = measure_response(rows + (size_t)j * (size_t)stride, ncap, sweep, nsweep,
                                     CAL_F1, CAL_F2, CAL_FS, band_hz, &res[j]);
    };
    std::vector<std::thread> pool;
    for (int w = 1; w < nw; ++w) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
}

int calib_measure_zylia_rows(const float* rows, int stride, int ncap, const float* sweep, int nsweep,
                             const double band_hz[2], int exp_lo, int exp_hi,
                             MeasureResult res[ZYLIA_MICS], int ok[ZYLIA_MICS]) {
    /* each capsule's IR window: from 64 samples before its own peak, 256 long, which holds the
     * reference taper (-ZYLIA_XC_PRE .. +ZYLIA_XC_POST around the strongest peak) plus the array's
     * aperture either side */
    enum { XW = 256, XPRE = 64 };
    static float win[ZYLIA_MICS][XW];                 /* control thread, one caller at a time */
    int start[ZYLIA_MICS] = { 0 };
    unsigned hw = std::thread::hardware_concurrency();
    const int nw = hw < 2 ? 1 : (hw > 8 ? 8 : (int)hw);
    std::atomic<int> next(0);
    auto work = [&]() {
        for (int j; (j = next.fetch_add(1)) < ZYLIA_MICS; )
            ok[j] = measure_response_ex(rows + (size_t)j * (size_t)stride, ncap, sweep, nsweep,
                                        CAL_F1, CAL_F2, CAL_FS, band_hz, exp_lo, exp_hi, &res[j],
                                        win[j], XW, XPRE, &start[j]);
    };
    std::vector<std::thread> pool;
    for (int w = 1; w < nw; ++w) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
    for (int j = 0; j < ZYLIA_MICS; ++j) if (!ok[j]) return 0;
    const float* wp[ZYLIA_MICS];
    double arr[ZYLIA_MICS];
    for (int j = 0; j < ZYLIA_MICS; ++j) wp[j] = win[j];
    if (!zylia_ir_tdoa(wp, start, XW, CAL_FS, arr)) return 0;
    for (int j = 0; j < ZYLIA_MICS; ++j) {
        const double a = arr[j] * CAL_FS;
        const double ai = floor(a + 0.5);
        res[j].delay_samples = (int)ai;
        res[j].delay_frac    = (float)(a - ai);
    }
    return 1;
}

void calib_pool_quality(const MeasureResult rj[ZYLIA_MICS], MeasureResult* out) {
    int nout = 0, ns = 0;
    float snr[ZYLIA_MICS];
    for (int j = 0; j < ZYLIA_MICS; ++j) {
        nout += rj[j].outside != 0;
        if (rj[j].noise_n > 0) {                             /* insertion sort for the median */
            const float v = rj[j].snr_db; int b = ns;
            while (b > 0 && snr[b-1] > v) { snr[b] = snr[b-1]; --b; } snr[b] = v; ++ns;
        }
    }
    out->win_lo = rj[0].win_lo; out->win_hi = rj[0].win_hi;
    out->outside = 2 * nout > ZYLIA_MICS;
    /* the outside tap and its level, from the capsule that saw it strongest (the message's numbers) */
    int w = 0;
    for (int j = 1; j < ZYLIA_MICS; ++j) if (rj[j].outside_db > rj[w].outside_db) w = j;
    out->peak_any = rj[w].peak_any; out->outside_db = rj[w].outside_db;
    out->noise_n = ns ? rj[0].noise_n : 0;
    out->snr_db  = ns ? snr[ns / 2] : 0.f;
    out->floor_rms = 0.f;
    for (int j = 0; j < ZYLIA_MICS; ++j) if (rj[j].floor_rms > out->floor_rms) out->floor_rms = rj[j].floor_rms;
}

double calib_window_pos_m(const Layout* L, int s) {
    return (L && s >= 0 && (uint32_t)s < L->count && L->speakers[s].has_plan) ? CALIB_WIN_SURVEYED_M : CALIB_WIN_PLAN_M;
}

int calib_window_prior(int simulate, double known_latency_s, double pos_m, CalibWindow* w, char* desc, size_t cap) {
    memset(w, 0, sizeof *w);
    w->pos_m = pos_m;
    char pm[96];
    if (pos_m >= 0.0) snprintf(pm, sizeof pm, "+/- %.2f m", pos_m);
    else snprintf(pm, sizeof pm, "+/- %.2f m for a surveyed speaker, %.2f m for one at its plan", CALIB_WIN_SURVEYED_M, CALIB_WIN_PLAN_M);
    if (simulate) {
        w->lat_s = CAL_SIM_LATENCY_SAMPLES / CAL_FS;
        if (desc) snprintf(desc, cap, "the simulator's own latency (%d samples, exact), distance %s", CAL_SIM_LATENCY_SAMPLES, pm);
        return 1;
    }
    if (known_latency_s >= 0.0) {
        w->lat_s = known_latency_s; w->lat_early_s = w->lat_late_s = CALIB_WIN_LAT_KNOWN_S;
        if (desc) snprintf(desc, cap, "latency %.3f ms +/- %.1f ms (measured), distance %s", known_latency_s * 1e3,
                           CALIB_WIN_LAT_KNOWN_S * 1e3, pm);
        return 1;
    }
#ifdef BWA_HAVE_ASIO
    long il = 0, ol = 0;
    if (calib_asio_latencies(&il, &ol)) {
        w->lat_s = (double)(il + ol) / CAL_FS; w->lat_early_s = 0.0; w->lat_late_s = CALIB_WIN_LAT_DRIVER_S;
        if (desc) snprintf(desc, cap, "the driver's loop %.2f ms, a lower bound, up to %.0f ms later (pass --latency to "
                           "narrow it), distance %s", w->lat_s * 1e3, CALIB_WIN_LAT_DRIVER_S * 1e3, pm);
        return 1;
    }
#endif
    if (desc) snprintf(desc, cap, "none: no latency to time the arrival from (no --latency, no driver number); the whole IR is searched");
    return 0;
}

/* The expected-arrival window of speaker s under P (0, 0 = none): the center's, widened by the
 * capsules' radius for the ZM-1 */
static void pass_window(const CalibPass* P, int s, int* lo, int* hi) {
    *lo = *hi = 0;
    if (!P->have_win) return;
    CalibWindow w = P->win;
    if (w.pos_m < 0.0) w.pos_m = calib_window_pos_m(P->L, s);
    if (P->zylia) w.pos_m += CALIB_WIN_ZM1_M;
    const float* m = P->mic ? P->mic : P->sim_at;
    if (!calib_arrival_window(&w, P->L->speakers[s].pos, m, P->sos, CAL_FS, lo, hi)) *lo = *hi = 0;
}

/* The raw capture's power over [0, n) of each of `nrow` rows (stride CAL_CAPLEN), the median row, in
 * dBFS; CALIB_BG_SILENT_DBFS for a silent capture or an empty span. Nothing of the sweep has arrived
 * there: it is the room. */
static float raw_background_dbfs(const float* rows, int nrow, int n) {
    if (n < MEASURE_NOISE_MIN_N) return CALIB_BG_SILENT_DBFS;
    if (n > CAL_CAPLEN) n = CAL_CAPLEN;
    double p[ZYLIA_MICS];
    for (int j = 0; j < nrow && j < ZYLIA_MICS; ++j) {
        double acc = 0.0;
        const float* r = rows + (size_t)j * CAL_CAPLEN;
        for (int i = 0; i < n; ++i) acc += (double)r[i] * r[i];
        int b = j; const double v = acc / n;              /* insertion sort for the median */
        while (b > 0 && p[b-1] > v) { p[b] = p[b-1]; --b; } p[b] = v;
    }
    const double m = p[nrow / 2];
    return m > 0.0 ? (float)(10.0 * log10(m)) : CALIB_BG_SILENT_DBFS;
}

/* One sweep of speaker s: the capture and measurement calib_measure_speaker made before the re-sweep. */
static int measure_once(const CalibPass* P, int s, int through, MeasureResult* out, CalibMeasInfo* mi) {
    const int nrow = P->zylia ? ZYLIA_MICS : 1;
    float* rows = P->zylia ? P->cap19 : P->cap;
    const double level_band[2] = { CAL_BAND_LO, CAL_BAND_HI };
    const double* band = P->band_hz ? P->band_hz : level_band;
    int lo, hi;
    pass_window(P, s, &lo, &hi);
    if (P->simulate) {
        if (P->zylia) calib_sim_capture_zylia(s, P->L, P->sim_at, P->caps, ZYLIA_MICS, P->sos, P->sweep, P->cap19);
        else          calib_sim_capture(s, P->L, P->sim_at, P->sos, P->sweep, P->cap);
        if (through && !calib_stage_rows(P->L, s, rows, nrow, CAL_CAPLEN, P->tmp)) return CALIB_MEAS_FAILED;
    }
#ifdef BWA_HAVE_ASIO
    else {
        int ok;
        if (through) {
            if (!calib_stage_signal(P->L, s, P->sweep, CAL_NSWEEP, P->play, P->nplay)) return CALIB_MEAS_FAILED;
            ok = calib_asio_capture_signal(s, P->play, P->nplay);
        } else ok = calib_asio_capture(s);
        if (!ok) return CALIB_MEAS_TIMEOUT;
    }
#else
    else return CALIB_MEAS_FAILED;
#endif
    /* through the output stage the arrival carries the speaker's own delay trim: the window moves with it */
    if (through && hi > lo) {
        const int d = (int)P->L->speakers[s].delay_samples;     /* loaded at CAL_FS */
        lo += d; hi += d;
    }
    if (!P->zylia) {
        if (!measure_response_ex(P->cap, CAL_CAPLEN, P->sweep, CAL_NSWEEP, CAL_F1, CAL_F2, CAL_FS, band, lo, hi, out,
                                 NULL, 0, 0, NULL)) return CALIB_MEAS_FAILED;
        mi->bg_dbfs = raw_background_dbfs(P->cap, 1, (hi > lo ? lo : out->delay_samples) - (int)(MEASURE_NOISE_GUARD_S * CAL_FS));
        return CALIB_MEAS_OK;
    }
    MeasureResult rj[ZYLIA_MICS];
    int okj[ZYLIA_MICS] = { 0 };
    /* 19 deconvolutions, threaded, the arrivals refined by cross-correlation */
    mi->refined = calib_measure_zylia_rows(P->cap19, CAL_CAPLEN, CAL_CAPLEN, P->sweep, CAL_NSWEEP, band, lo, hi, rj, okj);
    float lmin = 1e30f, lmax = 0.f;
    for (int j = 0; j < ZYLIA_MICS; ++j) {
        if (!okj[j]) return CALIB_MEAS_FAILED;
        if (rj[j].level < lmin) lmin = rj[j].level;
        if (rj[j].level > lmax) lmax = rj[j].level;
    }
    mi->capsule_spread_db = (lmin > 0.f) ? (float)(20.0 * log10((double)lmax / (double)lmin)) : 99.f;
    for (int j = 0; j < ZYLIA_MICS; ++j) mi->arrival_s[j] = ((double)rj[j].delay_samples + rj[j].delay_frac) / CAL_FS;
    for (int i = 0; i < CAL_CAPLEN; ++i) {
        double acc = 0.0;
        for (int j = 0; j < ZYLIA_MICS; ++j) acc += P->cap19[(size_t)j * CAL_CAPLEN + i];
        P->cap[i] = (float)(acc / ZYLIA_MICS);
    }
    int dead = -1;
    const int pr = zylia_pressure_proxy(rj, CAL_FS, P->sos, out, &dead);
    if (pr < 0) {
        double lv[ZYLIA_MICS];
        for (int j = 0; j < ZYLIA_MICS; ++j) {             /* sorted copy for the median (insertion sort) */
            double v = std::isfinite(rj[j].level) ? rj[j].level : 0.0; int b = j;
            while (b > 0 && lv[b-1] > v) { lv[b] = lv[b-1]; --b; } lv[b] = v;
        }
        mi->dead = dead;
        mi->dead_level = dead >= 0 ? (double)rj[dead].level : 0.0;
        mi->median_level = lv[ZYLIA_MICS / 2];
        return CALIB_MEAS_DEAD;
    }
    if (pr != 1) return CALIB_MEAS_FAILED;
    calib_pool_quality(rj, out);
    mi->bg_dbfs = raw_background_dbfs(P->cap19, ZYLIA_MICS, (hi > lo ? lo : out->delay_samples) - (int)(MEASURE_NOISE_GUARD_S * CAL_FS));
    return CALIB_MEAS_OK;
}

static void meas_log(CalibMeasInfo* mi, const char* fmt, ...) {
    const size_t k = strlen(mi->log);
    if (k + 2 >= sizeof mi->log) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(mi->log + k, sizeof mi->log - k, fmt, ap);
    va_end(ap);
}

int calib_measure_speaker(const CalibPass* P, int s, int through, MeasureResult* out, CalibMeasInfo* info) {
    CalibMeasInfo dummy;
    CalibMeasInfo* mi = info ? info : &dummy;
    memset(mi, 0, sizeof *mi);
    mi->dead = -1;
    /* Re-sweep until a sweep is clean (the window and the SNR floor), and with P->agree until two clean
     * sweeps agree. Every clean sweep is kept, so a third one that agrees with the first settles it
     * whatever the second was. The later sweep of the agreeing pair is the result: `cap` holds its
     * capture for the IR consumers that read it next. */
    MeasureResult clean[CALIB_SWEEP_MAX_TRIES];
    int nclean = 0;
    for (int k = 0; k < CALIB_SWEEP_MAX_TRIES; ++k) {
        MeasureResult r;
        const int rc = measure_once(P, s, through, &r, mi);
        ++mi->sweeps;
        if (rc != CALIB_MEAS_OK) return rc;
        const int q = calib_sweep_check(&r, CALIB_SWEEP_MIN_SNR_DB);
        if (q != CALIB_SWEEP_OK) {
            char why[240];
            calib_sweep_why(&r, q, CAL_FS, why, sizeof why);
            if (q == CALIB_SWEEP_OUTSIDE) ++mi->rej_outside; else ++mi->rej_noisy;
            meas_log(mi, "sweep %d: %s\n", k + 1, why);
            continue;
        }
        if (P->agree) {
            int hit = -1;
            for (int j = nclean - 1; j >= 0 && hit < 0; --j)
                if (calib_sweeps_agree(&clean[j], &r, NULL, NULL)) hit = j;
            if (hit < 0) {
                if (nclean > 0) {
                    float ds = 0.f, ddb = 0.f;
                    calib_sweeps_agree(&clean[nclean - 1], &r, &ds, &ddb);
                    ++mi->rej_disagree;
                    meas_log(mi, "sweep %d: %.2f samples and %.2f dB from the last clean sweep (agreeing takes %.0f sample, %.1f dB)\n",
                             k + 1, ds, ddb, CALIB_SWEEP_AGREE_SAMPLES, CALIB_SWEEP_AGREE_DB);
                }
                clean[nclean++] = r;
                continue;
            }
        }
        *out = r;
        mi->snr_db = r.snr_db;
        return CALIB_MEAS_OK;
    }
    meas_log(mi, "%d sweeps and no %s: nothing from this speaker is used\n", CALIB_SWEEP_MAX_TRIES,
             P->agree ? "two clean sweeps that agree" : "clean sweep");
    return CALIB_MEAS_UNCLEAN;
}

int calib_live_read(int spk, const Layout* L, const float center[3], double sos, int simulate,
                    const CalibSimOpts* sim, const float* lsweep, float* cap19, CalibLiveReading* out,
                    const CalibWindow* win, const float* believed) {
    memset(out, 0, sizeof *out);
    out->dead = -1;
    if (simulate) {
        float caps[ZYLIA_MICS][3]; zylia_capsules(caps);
        CalibSimOpts o; memset(&o, 0, sizeof o);
        if (sim) o = *sim;
        o.nsweep = CAL_LIVE_NSWEEP; o.ncap = CAL_LIVE_CAPLEN;
        calib_sim_capture_zylia_ex(spk, L, center, caps, ZYLIA_MICS, sos, &o, cap19, CAL_CAPLEN);
    }
#ifdef BWA_HAVE_ASIO
    else if (!calib_asio_capture_len(spk, lsweep, CAL_LIVE_NSWEEP, CAL_LIVE_CAPLEN)) return 0;
#else
    else return 0;
#endif
    /* the window: the union of the as-built's and the plan's, the box moving from one toward the other */
    int lo = 0, hi = 0;
    if (win) {
        CalibWindow w = *win;
        if (w.pos_m < 0.0) w.pos_m = CALIB_WIN_PLAN_M;
        w.pos_m += CALIB_WIN_ZM1_M;
        const float* m = believed ? believed : center;
        int a0, a1, b0, b1;
        const int ha = calib_arrival_window(&w, L->speakers[spk].pos, m, sos, CAL_FS, &a0, &a1);
        const int hb = calib_arrival_window(&w, layout_plan_pos(L, (uint32_t)spk), m, sos, CAL_FS, &b0, &b1);
        if (ha && hb) { lo = a0 < b0 ? a0 : b0; hi = a1 > b1 ? a1 : b1; }
    }
    /* the LIVE tilt bands (calib.h CALIB_LIVE_*, steeper near on-axis than --check-aim's): only the
     * arrivals and the direct-sound tilt are read */
    const double band[2] = { CALIB_LIVE_MID_HZ, CALIB_LIVE_HIGH_HZ };
    MeasureResult rj[ZYLIA_MICS];
    int okj[ZYLIA_MICS] = { 0 };
    calib_measure_zylia_rows(cap19, CAL_CAPLEN, CAL_LIVE_CAPLEN, lsweep, CAL_LIVE_NSWEEP, band, lo, hi, rj, okj);
    for (int j = 0; j < ZYLIA_MICS; ++j) if (!okj[j]) { out->dead = j; return 1; }
    int dead = -1;
    if (zylia_pressure_proxy(rj, CAL_FS, sos, &out->pooled, &dead) != 1) { out->dead = dead; return 1; }
    calib_pool_quality(rj, &out->pooled);
    out->quality = calib_sweep_check(&out->pooled, CALIB_SWEEP_MIN_SNR_DB);
    if (out->quality != CALIB_SWEEP_OK) calib_sweep_why(&out->pooled, out->quality, CAL_FS, out->why, sizeof out->why);
    for (int j = 0; j < ZYLIA_MICS; ++j) out->arr[j] = ((double)rj[j].delay_samples + rj[j].delay_frac) / CAL_FS;
    out->have_tilt = calib_direct_tilt_db(&out->pooled, &out->tilt_db);
    out->ok = 1;
    return 1;
}

int calib_stage_pad(const Layout* L) {
    int eq = 0;
    for (uint32_t k = 0; k < L->count; ++k) if (L->speakers[k].eq_len > eq) eq = L->speakers[k].eq_len;
    return (int)L->max_delay_samples + eq + (int)(0.05 * CAL_FS);
}

int calib_stage_signal(const Layout* L, int ch, const float* in, int nin, float* out, int nout) {
    const uint32_t n = L->count, B = 256;
    if (ch < 0 || (uint32_t)ch >= n || nout <= 0) return 0;
    /* The other channels carry silence, and align.c's channels never mix, so their FIRs and modal
     * cuts only cost time (the FIR is n x 512 taps a sample). A copy with THEIR filters removed builds
     * channel ch's stage exactly as the engine does and skips the rest. Static: a Layout is never a
     * stack local, and this runs on the one control thread. */
    static Layout LS;
    LS = *L;
    for (uint32_t k = 0; k < n; ++k)
        if (k != (uint32_t)ch) { LS.speakers[k].eq_len = 0; LS.speakers[k].room_eq_count = 0; LS.rq_grid.nsec[k] = 0; }
    Aligner* a = align_create(n, &LS, (uint32_t)CAL_FS);
    float* bus = (float*)calloc((size_t)n * B, sizeof(float));
    if (!a || !bus) { align_destroy(a); free(bus); return 0; }
    for (int i0 = 0; i0 < nout; i0 += (int)B) {
        const int m = nout - i0 < (int)B ? nout - i0 : (int)B;
        /* the planar stride is the block length: a short last block is still a full-stride bus */
        memset(bus, 0, (size_t)n * B * sizeof(float));
        float* x = bus + (size_t)ch * (size_t)m;
        for (int i = 0; i < m; ++i) x[i] = (i0 + i < nin) ? in[i0 + i] : 0.f;
        align_process(a, bus, (uint32_t)m);
        memcpy(out + i0, x, (size_t)m * sizeof(float));
    }
    free(bus);
    align_destroy(a);
    return 1;
}

int calib_stage_rows(const Layout* L, int ch, float* rows, int nrows, int len, float* tmp) {
    /* The stage's impulse response first, from the stage itself. When it is ONE tap (no eq and no
     * room_eq: a gain and a whole-sample delay) every row is that tap's shift and scale, exactly;
     * otherwise each row runs through a fresh stage. The shortcut matters for the ZM-1, whose 19 rows
     * per speaker would otherwise each pay the n-channel stage. */
    const int nh = calib_stage_pad(L) + 1;
    if (nh > len) return 0;
    float* imp = (float*)calloc((size_t)nh, sizeof(float));
    if (!imp) return 0;
    imp[0] = 1.f;
    if (!calib_stage_signal(L, ch, imp, 1, tmp, nh)) { free(imp); return 0; }
    int tap = -1, ntap = 0;
    for (int i = 0; i < nh; ++i) if (tmp[i] != 0.f) { tap = i; ++ntap; }
    const float g = tap >= 0 ? tmp[tap] : 0.f;
    free(imp);
    for (int j = 0; j < nrows; ++j) {
        float* r = rows + (size_t)j * (size_t)len;
        if (ntap == 1) {
            memmove(r + tap, r, (size_t)(len - tap) * sizeof(float));
            for (int i = 0; i < tap; ++i) r[i] = 0.f;
            for (int i = tap; i < len; ++i) r[i] *= g;
        } else {
            if (!calib_stage_signal(L, ch, r, len, tmp, len)) return 0;
            memcpy(r, tmp, (size_t)len * sizeof(float));
        }
    }
    return 1;
}

#ifdef BWA_HAVE_ASIO
/* ===== ASIO full-duplex capture (rig only; not run in this environment) ===== */
/* NOT wrapped in extern "C": the SDK builds these with C++ linkage (matches asio_sink.cpp). */
#include "asiosys.h"
#include "asio.h"
#include "asiodrivers.h"
#include "asio_convert.h"              /* shared sample-format converters (after the SDK headers) */
#include "asio_session.h"              /* the one-driver-slot arbitration */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>

extern AsioDrivers* asioDrivers;
extern bool loadAsioDriver(char* name);

namespace {
struct Cal {
    long            bufsize;
    int             nspk;                     /* the layout's speaker count; inputs ride slots [nspk..] */
    int             nin;                      /* lockstep inputs: 1 = omni mic, 19 = the ZM-1 (--zylia) */
    ASIOBufferInfo  bi[BWA_MAX_CHANNELS + CAL_MAX_INPUTS];   /* nspk outputs + nin inputs (after them) */
    ASIOChannelInfo ci[BWA_MAX_CHANNELS + CAL_MAX_INPUTS];
    ASIOCallbacks   cb;
    const float*    sweep;                    /* the signal given at open */
    const float*    play;                     /* what this capture plays: sweep, or a --verify signal */
    int             play_len;
    float*          cap;                      /* [nin][CAL_CAPLEN] flat */
    int             cap_len;                  /* samples this capture records per input (<= CAL_CAPLEN) */
    std::atomic<int> active;                  /* output channel being measured, -1 = idle */
    std::atomic<int> done;
    int             play_pos, cap_pos;
} g;

static const float g_zero[1024] = { 0 };                     /* type-correct silence via asio_float_to_out */

void buffer_switch(long index, ASIOBool) {
    int ac = g.active.load(std::memory_order_acquire);
    long n = g.bufsize;
    for (int c = 0; c < g.nspk; ++c) {
        void* dst = g.bi[c].buffers[index];
        if (c == ac && g.play_pos < g.play_len) {
            float blk[1024];                                 /* bufsize <= 1024 enforced at open */
            for (long i = 0; i < n; ++i) blk[i] = (g.play_pos + i < g.play_len) ? g.play[g.play_pos + i] : 0.f;
            asio_float_to_out(dst, blk, n, g.ci[c].type);
        } else {
            asio_float_to_out(dst, g_zero, n, g.ci[c].type); /* NOT memset(n*4): a 2/3-byte buffer would overrun */
        }
    }
    if (ac >= 0) {
        int rem = g.cap_len - g.cap_pos;
        long take = n < rem ? n : rem;
        if (take > 0)
            for (int j = 0; j < g.nin; ++j)
                asio_in_to_float(g.cap + (size_t)j * CAL_CAPLEN + g.cap_pos,
                                 g.bi[g.nspk + j].buffers[index], take, g.ci[g.nspk + j].type);
        g.play_pos += (int)n;
        g.cap_pos  += (int)n;
        if (g.cap_pos >= g.cap_len) g.done.store(1, std::memory_order_release);
    }
    ASIOOutputReady();
}
ASIOTime* buffer_switch_ti(ASIOTime* t, long index, ASIOBool pn) { buffer_switch(index, pn); return t; }
void rate_changed(ASIOSampleRate) {}
long asio_msg(long s, long v, void*, double*) {
    if (s == kAsioSelectorSupported) return (v==kAsioEngineVersion||v==kAsioSupportsTimeInfo)?1:0;
    if (s == kAsioEngineVersion) return 2; if (s == kAsioSupportsTimeInfo) return 1; return 0;
}

bool open_driver(const char* want, int in_first, int nin_want, int nspk) {
    char names[16][32], *ptr[16]; for (int i=0;i<16;++i) ptr[i]=names[i];
    auto tryone = [&](const char* nm)->bool {
        if (!loadAsioDriver((char*)nm)) return false;
        ASIODriverInfo di; memset(&di,0,sizeof di); di.asioVersion=2; di.sysRef=GetDesktopWindow();
        if (ASIOInit(&di) != ASE_OK) { asioDrivers->removeCurrentDriver(); return false; }
        long nin=0, nout=0;
        /* every input is non-negotiable: a device exposing fewer would feed the solve silent
         * channels — for the ZM-1 that is a confident wrong direction (same rule as valid_capture) */
        if (ASIOGetChannels(&nin,&nout)!=ASE_OK || nout<nspk || nin<in_first+nin_want) {
            ASIOExit(); asioDrivers->removeCurrentDriver(); return false; }
        return true;
    };
    if (want && *want) return tryone(want);
    long nd = asioDrivers ? asioDrivers->getDriverNames(ptr,16) : 0;
    for (long i=0;i<nd;++i) if (tryone(names[i])) return true;
    return false;
}
} /* namespace */

/* driver latencies from the last open (calib_asio_latencies); -1 = unknown. Deliberately NOT part
 * of `g` (which is memset per open) so the values survive calib_asio_close — the localize solve
 * cross-checks them after the device is already torn down. */
static long s_lat_in = -1, s_lat_out = -1;

int calib_asio_open_multi(const char* driver, int in_first, int nin, int nspk, const float* sweep, float* cap) {
    if (in_first < 0) { fprintf(stderr, "calib_capture: first input channel must be >= 0 (got %d)\n", in_first); return 1; }
    if (nin < 1 || nin > CAL_MAX_INPUTS) { fprintf(stderr, "calib_capture: input count %d out of range (1..%d)\n", nin, CAL_MAX_INPUTS); return 1; }
    if (nspk < 1 || nspk > BWA_MAX_CHANNELS) { fprintf(stderr, "calib_capture: speaker count %d out of range\n", nspk); return 1; }
    if (!asio_session_acquire("the calibration sweep (Capture tab / bwa_calibrate)")) return 1;
    memset(&g, 0, sizeof g); g.active = -1; g.sweep = sweep; g.play = sweep; g.play_len = CAL_NSWEEP; g.cap = cap; g.cap_len = CAL_CAPLEN; g.nspk = nspk; g.nin = nin;
    if (!open_driver(driver, in_first, nin, nspk)) {
        fprintf(stderr, "calib_capture: no ASIO driver with >=%d out + %d input(s) from %d\n", nspk, nin, in_first);
        asio_session_release(); return 1; }
    long bmin=0,bmax=0,bpref=0,bgran=0; ASIOGetBufferSize(&bmin,&bmax,&bpref,&bgran);
    long bs = bpref > 1024 ? 1024 : bpref;                    /* prefer <=1024 (blk[] in buffer_switch is 1024) */
    if (bs < bmin) bs = bmin;                                 /* honor the driver minimum ... */
    if (bs > 1024) {                                          /* ... but the stack scratch can't exceed 1024 */
        fprintf(stderr, "calib_capture: driver minimum buffer %ld samples exceeds the 1024 limit\n", bs);
        ASIOExit(); asioDrivers->removeCurrentDriver(); asio_session_release(); return 1;
    }
    g.bufsize = bs;
    if (ASIOCanSampleRate((ASIOSampleRate)CAL_FS)!=ASE_OK || ASIOSetSampleRate((ASIOSampleRate)CAL_FS)!=ASE_OK) {
        fprintf(stderr, "calib_capture: driver cannot run at %.0f Hz\n", CAL_FS); ASIOExit(); asioDrivers->removeCurrentDriver(); asio_session_release(); return 1; }
    for (int c = 0; c < nspk; ++c) { g.bi[c].isInput=ASIOFalse; g.bi[c].channelNum=c; }
    for (int j = 0; j < nin; ++j)  { g.bi[nspk+j].isInput=ASIOTrue; g.bi[nspk+j].channelNum=in_first+j; }
    g.cb.bufferSwitch=&buffer_switch; g.cb.sampleRateDidChange=&rate_changed;
    g.cb.asioMessage=&asio_msg;       g.cb.bufferSwitchTimeInfo=&buffer_switch_ti;
    for (int c = 0; c < nspk+nin; ++c) { g.ci[c].channel=g.bi[c].channelNum; g.ci[c].isInput=g.bi[c].isInput; ASIOGetChannelInfo(&g.ci[c]); }
    if (ASIOCreateBuffers(g.bi, nspk+nin, g.bufsize, &g.cb)!=ASE_OK) {
        fprintf(stderr, "calib_capture: ASIOCreateBuffers failed (%d channels)\n", nspk+nin); ASIOExit(); asioDrivers->removeCurrentDriver(); asio_session_release(); return 1; }
    /* Log the driver's own latencies (final only after CreateBuffers — they depend on the
     * negotiated buffer). out + in is the DIGITAL half of the sweep's round trip; every measured
     * delay contains it, plus DAC/ADC + analog. A rig-day diagnostic: if a solved system latency
     * ever lands BELOW this sum, the measurement chain is misconfigured, and if it lands tens of
     * ms above, look at the Dante latency setting. */
    s_lat_in = s_lat_out = -1;
    { long il = 0, ol = 0;
      if (ASIOGetLatencies(&il, &ol) == ASE_OK && il >= 0 && ol >= 0) {
          s_lat_in = il; s_lat_out = ol;
          printf("calib_capture: driver latency out %ld + in %ld frames (%.2f + %.2f ms), digital loop %.2f ms = %.3f m at c\n",
                 ol, il, 1e3 * (double)ol / CAL_FS, 1e3 * (double)il / CAL_FS,
                 1e3 * (double)(ol + il) / CAL_FS, 343.0 * (double)(ol + il) / CAL_FS);
      } else printf("calib_capture: driver did not report latencies (ASIOGetLatencies)\n"); }
    if (ASIOStart()!=ASE_OK) { fprintf(stderr, "calib_capture: ASIOStart failed\n"); ASIODisposeBuffers(); ASIOExit(); asioDrivers->removeCurrentDriver(); asio_session_release(); return 1; }
    return 0;
}

int calib_asio_open(const char* driver, int mic_in, int nspk, const float* sweep, float* cap) {
    return calib_asio_open_multi(driver, mic_in, 1, nspk, sweep, cap);
}

int calib_asio_latencies(long* in_frames, long* out_frames) {
    if (s_lat_in < 0 || s_lat_out < 0) return 0;
    if (in_frames)  *in_frames  = s_lat_in;
    if (out_frames) *out_frames = s_lat_out;
    return 1;
}

/* Capture one speaker: drive the sweep on output `ch`, record CAL_CAPLEN samples of every input. */
int calib_asio_capture(int ch) {
    g.play = g.sweep; g.play_len = CAL_NSWEEP;               /* idle (active == -1): the callback reads neither */
    g.cap_len = CAL_CAPLEN;
    g.play_pos = 0; g.cap_pos = 0;
    g.done.store(0, std::memory_order_relaxed);
    g.active.store(ch, std::memory_order_release);
    for (int spins = 0; !g.done.load(std::memory_order_acquire); ++spins) {
        Sleep(5);
        if (spins > 2000) { g.active.store(-1, std::memory_order_release); return 0; }  /* ~10 s watchdog */
    }
    g.active.store(-1, std::memory_order_release);
    return 1;
}

int calib_asio_capture_signal(int ch, const float* sig, int nsig) {
    return calib_asio_capture_len(ch, sig, nsig, CAL_CAPLEN);
}

int calib_asio_capture_len(int ch, const float* sig, int nsig, int ncap) {
    if (ncap < 1 || ncap > CAL_CAPLEN) {
        fprintf(stderr, "calib_capture: capture length %d out of range (1..%d)\n", ncap, CAL_CAPLEN); return 0; }
    if (!sig || nsig < 1 || nsig > ncap) {
        fprintf(stderr, "calib_capture: play signal length %d out of range (1..%d)\n", nsig, ncap); return 0; }
    g.play_pos = 0; g.cap_pos = 0;
    g.cap_len = ncap;                                        /* idle: the callback reads it only once active */
    g.play = sig; g.play_len = nsig;                         /* published by the release store below */
    g.done.store(0, std::memory_order_relaxed);
    g.active.store(ch, std::memory_order_release);
    int ok = 1;
    for (int spins = 0; !g.done.load(std::memory_order_acquire); ++spins) {
        Sleep(5);
        if (spins > 2000) { ok = 0; break; }                 /* ~10 s watchdog */
    }
    g.active.store(-1, std::memory_order_release);
    g.play = g.sweep; g.play_len = CAL_NSWEEP; g.cap_len = CAL_CAPLEN;
    return ok;
}

void calib_asio_close(void) {
    ASIOStop(); ASIODisposeBuffers(); ASIOExit();
    if (asioDrivers) asioDrivers->removeCurrentDriver();
    asio_session_release();
}

/* Registered-driver enumeration for the tools' pickers/--list-drivers: a LOCAL AsioDrivers reads
 * the registry fresh each call and loads nothing, so it needs no session slot and is safe while
 * a capture (or the engine) has a driver open — the zylia shell's pattern. */
int calib_asio_driver_names(char (*names)[32], int max) {
    AsioDrivers list;
    char* ptrs[32];
    if (max > 32) max = 32;
    for (int i = 0; i < max; ++i) ptrs[i] = names[i];
    long n = list.getDriverNames(ptrs, max);
    return n < 0 ? 0 : (int)n;
}

int calib_asio_list(void) {
    char names[32][32];
    int nd = calib_asio_driver_names(names, 32);
    printf("registered ASIO drivers (%d):\n", nd);
    for (int i = 0; i < nd; ++i) printf("  %2d. %s\n", i, names[i]);
    return 0;
}
#else
int calib_asio_driver_names(char (*names)[32], int max) { (void)names; (void)max; return 0; }
int calib_asio_list(void) {
    printf("built without the ASIO SDK - no drivers to list (simulate mode only)\n");
    return 2;
}
#endif /* BWA_HAVE_ASIO */
