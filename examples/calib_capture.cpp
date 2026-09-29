/*
 * calib_capture.cpp — see calib_capture.h. Moved verbatim out of calibrate.cpp so the CLI and
 * bwa_calib_view's Capture tab share ONE copy of the sweep-capture backends.
 */
#include "calib_capture.h"
#include "dsp/sos.h"                       /* BWA_SOS_REF_MPS + the plausible-c guard */
#include "dsp/fft.h"                       /* the simulated room's late tail (one convolution per run) */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

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

/* the exponential sweep's own time axis -> frequency (Farina; measure_sweep's constants) */
inline double sweep_freq(double i) { return CAL_F1 * pow(CAL_F2 / CAL_F1, i / (double)CAL_NSWEEP); }

/* Add amp * sweep(i - frac) * gain(i) into cap starting at di: measure_sweep's formula (fades
 * included) evaluated analytically, like the direct sound below. `knots` (NULL = 1) is a gain curve
 * sampled every KNOT sweep samples, linearly interpolated: an image's directivity loss changes
 * slowly along the sweep, and evaluating the model per sample for 24 images is the whole cost. The
 * exponential is stepped by multiplication (one rounding per sample, 1e-11 relative after 72000). */
const int KNOT = 64;
const int NKNOT = CAL_NSWEEP / KNOT + 2;
void add_sweep(float* cap, int di, double frac, double amp, const float* knots) {
    const double w1 = 2.0 * M_PI * CAL_F1 / CAL_FS, w2 = CAL_F2 * 2.0 * M_PI / CAL_FS;
    const double T = (double)CAL_NSWEEP, Lg = log(w2 / w1), Kp = T * w1 / Lg;
    const int fade = (int)(0.005 * CAL_FS);
    const double step = exp(Lg / T);
    double e = exp(-frac * Lg / T);                  /* exp(x / T * Lg) at x = -frac */
    for (int i = 0; i < CAL_NSWEEP && di + i < CAL_CAPLEN; ++i, e *= step) {
        const double x = (double)i - frac;
        if (x < 0.0 || di + i < 0) continue;
        double v = sin(Kp * (e - 1.0));
        if (x < fade)                  v *= 0.5 - 0.5 * cos(M_PI * x / fade);
        if (x > CAL_NSWEEP - 1 - fade) v *= 0.5 - 0.5 * cos(M_PI * (CAL_NSWEEP - 1 - x) / fade);
        double g = amp;
        if (knots) { const int kk = i / KNOT; const double t = (double)(i - kk * KNOT) / KNOT; g *= knots[kk] + (knots[kk + 1] - knots[kk]) * t; }
        cap[di + i] += (float)(g * v);
    }
}

/* The late tail's contribution to a capture, for a unit-sensitivity speaker whose direct sound
 * lands at sample 0: (sweep shaped by the model's power response) convolved with an exponentially
 * decaying deterministic noise, energy-normalized to the room equation's diffuse level. Depends only
 * on the room, the absorption and the model, so it is built once per run and reused for every
 * capture; `onset` is where it starts after the direct sound. */
struct TailCache {
    int    valid;
    double key[5];                        /* absorption, the box's three widths, the model's band count */
    int    onset;
    float* cap;                           /* CAL_CAPLEN */
} g_tail = { 0, { 0, 0, 0, 0, 0 }, 0, NULL };

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

const float* room_tail(const Layout* L, const SimRoom* r, double sos) {
    const double key[5] = { (double)g_room_alpha, r->w[0], r->w[1], r->w[2], (double)L->dir.nband };
    if (g_tail.valid && !memcmp(key, g_tail.key, sizeof key)) return g_tail.cap;
    const double a = (double)g_room_alpha;
    const int onset = (int)(2.0 * r->mfp / sos * CAL_FS);
    const int ntail = CAL_CAPLEN - onset > 16 ? CAL_CAPLEN - onset : 16;
    int L2 = 1; while (L2 < CAL_NSWEEP + ntail) L2 <<= 1;
    double* sre = (double*)calloc((size_t)L2, sizeof(double));
    double* sim = (double*)calloc((size_t)L2, sizeof(double));
    double* tre = (double*)calloc((size_t)L2, sizeof(double));
    double* tim = (double*)calloc((size_t)L2, sizeof(double));
    if (!g_tail.cap) g_tail.cap = (float*)calloc((size_t)CAL_CAPLEN, sizeof(float));
    if (!sre || !sim || !tre || !tim || !g_tail.cap) {
        free(sre); free(sim); free(tre); free(tim); return NULL;
    }
    /* the sweep, each sample scaled by sqrt(power response) at its instantaneous frequency (knots) */
    static float knots[NKNOT];
    for (int k = 0; k < NKNOT; ++k) knots[k] = (float)sqrt(power_response(&L->dir, sweep_freq((double)k * KNOT)));
    static float sw[CAL_NSWEEP];
    memset(sw, 0, sizeof sw);
    add_sweep(sw - 0, 0, 0.0, 1.0, knots);        /* di 0, frac 0: the sweep itself, shaped */
    for (int i = 0; i < CAL_NSWEEP; ++i) sre[i] = sw[i];
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
    for (int i = 0; i < CAL_CAPLEN; ++i) g_tail.cap[i] = (float)(i < L2 ? sre[i] * scale : 0.0);
    free(sre); free(sim); free(tre); free(tim);
    memcpy(g_tail.key, key, sizeof key);
    g_tail.onset = onset;
    g_tail.valid = 1;
    return g_tail.cap;
}
} /* namespace */

void calib_sim_room_describe(const Layout* L, char* buf, size_t cap) {
    if (!buf || cap == 0) return;
    if (!L || !(g_room_alpha > 0.f)) { snprintf(buf, cap, "anechoic"); return; }
    SimRoom r; room_geometry(L, &r);
    snprintf(buf, cap, "shoebox %.2f x %.2f x %.2f m (the array + %.1f m), absorption %.2f, Sabine RT60 %.2f s, "
             "24 image sources (orders 1-2) + a late tail from two mean free paths (%.1f m) on",
             r.w[0], r.w[1], r.w[2], (double)CALIB_SIM_ROOM_MARGIN_M, (double)g_room_alpha, r.rt60, 2.0 * r.mfp);
}

/* the rotated (simulated-error) acoustic axis of speaker ch */
static void sim_aim(const Layout* L, int ch, float aim[3]) {
    aim[0] = L->speakers[ch].aim[0]; aim[1] = L->speakers[ch].aim[1]; aim[2] = L->speakers[ch].aim[2];
    if (g_sim_aim_err_deg == 0.f) return;
    float up[3] = { 0, 1, 0 };                    /* rotate about a perpendicular, deterministic axis */
    if (fabsf(aim[1]) > 0.9f) { up[0] = 1; up[1] = 0; }
    float u[3] = { aim[1]*up[2] - aim[2]*up[1], aim[2]*up[0] - aim[0]*up[2], aim[0]*up[1] - aim[1]*up[0] };
    float l = sqrtf(u[0]*u[0] + u[1]*u[1] + u[2]*u[2]);
    if (l > 1e-6f) {
        float ang = g_sim_aim_err_deg * 3.14159265f / 180.f, ca = cosf(ang), sa = sinf(ang) / l;
        for (int j = 0; j < 3; ++j) aim[j] = ca * aim[j] + sa * u[j];
    }
}

void calib_sim_capture(int ch, const Layout* L, const float mic[3], double sos, const float* sweep, float* cap) {
    memset(cap, 0, (size_t)CAL_CAPLEN * sizeof(float));
    const float* p = L->speakers[ch].pos;
    double dist = sqrt((p[0]-mic[0])*(p[0]-mic[0]) + (p[1]-mic[1])*(p[1]-mic[1]) + (p[2]-mic[2])*(p[2]-mic[2]));
    if (dist < 0.05) dist = 0.05;
    /* Directivity, when the layout carries a model: the mic's bearing off the speaker's TRUE axis
     * (the layout's, rotated by the sim-only error) sets a per-FREQUENCY loss, applied to the sweep
     * sample by sample at its instantaneous frequency. An exponential sweep maps time to frequency
     * exactly (Farina: f(i) = f1 (f2/f1)^(i/N), measure_sweep's phase constants), so this is a
     * frequency-dependent gain with no filter, and the deconvolved IR carries the model's tilt. */
    static float dloss[CAL_NSWEEP];               /* 288 KB: static, off the caller's (possibly a
                                                    * std::thread's) stack; one capture at a time */
    int have_dir = L->dir.nband != 0;
    float aim[3];
    sim_aim(L, ch, aim);
    if (have_dir) {
        float dx = (mic[0]-p[0]) / (float)dist, dy = (mic[1]-p[1]) / (float)dist, dz = (mic[2]-p[2]) / (float)dist;
        float c = dx*aim[0] + dy*aim[1] + dz*aim[2];
        if (c > 1.f) c = 1.f; else if (c < -1.f) c = -1.f;
        float theta = acosf(c) * 180.f / 3.14159265f;
        const double ratio = CAL_F2 / CAL_F1;
        for (int i = 0; i < CAL_NSWEEP; ++i) {
            double f = CAL_F1 * pow(ratio, (double)i / (double)CAL_NSWEEP);
            dloss[i] = powf(10.f, directivity_loss_db_at(&L->dir, theta, (float)f) / 20.f);
        }
    }
    /* MUST be the same c the caller's analyzer divides back out (range = c * delay). Pinning this to
     * 343.0 while the analyzer follows the layout's recorded temperature splits the matched pair and
     * inflates every solved position by their ratio — silently, and --localize writes it back. */
    if (!(sos >= BWA_SOS_MIN_MPS && sos <= BWA_SOS_MAX_MPS)) sos = BWA_SOS_REF_MPS;
    double delay_f = 512.0 + dist / sos * CAL_FS;                 /* system latency + time of flight (fractional) */
    int    di = (int)delay_f; float frac = (float)(delay_f - di);
    double sens = 1.0 + 0.15 * sin(ch * 1.3);                     /* deterministic +/- ~1.4 dB wobble */
    float  g    = (float)(sens / dist);                           /* 1/r at the mic */
    /* The fractional part of the delay is applied ANALYTICALLY: the capture is the sweep formula
     * (measure_sweep's, fades included) evaluated at i - frac, not the passed array interpolated.
     * A two-tap linear interpolation is a low-pass whose treble loss follows the fraction (-6 dB at
     * 16 kHz at a half sample), which put a position-dependent tilt into every simulated capture and
     * read as an aim error in --check-aim. A real ADC has no such term, so the simulator must not. */
    (void)sweep;
    const double w1 = 2.0 * M_PI * CAL_F1 / CAL_FS, w2 = CAL_F2 * 2.0 * M_PI / CAL_FS;
    const double T = (double)CAL_NSWEEP, Lg = log(w2 / w1), Kp = T * w1 / Lg;
    const int fade = (int)(0.005 * CAL_FS);
    for (int i = 0; i < CAL_NSWEEP && di + i < CAL_CAPLEN; ++i) {
        double x = (double)i - (double)frac;                        /* the sweep's own time axis */
        if (x < 0.0) continue;
        double v = sin(Kp * (exp(x / T * Lg) - 1.0));
        if (x < fade)              v *= 0.5 - 0.5 * cos(M_PI * x / fade);
        if (x > CAL_NSWEEP - 1 - fade) v *= 0.5 - 0.5 * cos(M_PI * (CAL_NSWEEP - 1 - x) / fade);
        cap[di + i] += g * (float)v * (have_dir ? dloss[i] : 1.f);
    }
    if (!(g_room_alpha > 0.f)) return;

    /* the simulated room: image sources of orders 1 and 2 off the shoebox, then the late tail */
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
        if (have_dir) {
            for (int k = 0; k < NKNOT; ++k)
                knots[k] = powf(10.f, directivity_loss_db_at(&L->dir, th, (float)sweep_freq((double)k * KNOT)) / 20.f);
            kp = knots;
        }
        const double d_img = 512.0 + ri / sos * CAL_FS;
        const int dii = (int)d_img;
        add_sweep(cap, dii, d_img - dii, sens / ri * pow(R, order), kp);
    }
    const float* tail = room_tail(L, &r, sos);
    if (tail) {
        const int off = di + g_tail.onset;
        for (int i = 0; off + i < CAL_CAPLEN; ++i) cap[off + i] += (float)sens * tail[i];
    }
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
    const float*    sweep;
    float*          cap;                      /* [nin][CAL_CAPLEN] flat */
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
        if (c == ac && g.play_pos < CAL_NSWEEP) {
            float blk[1024];                                 /* bufsize <= 1024 enforced at open */
            for (long i = 0; i < n; ++i) blk[i] = (g.play_pos + i < CAL_NSWEEP) ? g.sweep[g.play_pos + i] : 0.f;
            asio_float_to_out(dst, blk, n, g.ci[c].type);
        } else {
            asio_float_to_out(dst, g_zero, n, g.ci[c].type); /* NOT memset(n*4): a 2/3-byte buffer would overrun */
        }
    }
    if (ac >= 0) {
        int rem = CAL_CAPLEN - g.cap_pos;
        long take = n < rem ? n : rem;
        if (take > 0)
            for (int j = 0; j < g.nin; ++j)
                asio_in_to_float(g.cap + (size_t)j * CAL_CAPLEN + g.cap_pos,
                                 g.bi[g.nspk + j].buffers[index], take, g.ci[g.nspk + j].type);
        g.play_pos += (int)n;
        g.cap_pos  += (int)n;
        if (g.cap_pos >= CAL_CAPLEN) g.done.store(1, std::memory_order_release);
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
    memset(&g, 0, sizeof g); g.active = -1; g.sweep = sweep; g.cap = cap; g.nspk = nspk; g.nin = nin;
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
          printf("calib_capture: driver latency out %ld + in %ld frames (%.2f + %.2f ms) — digital loop %.2f ms = %.3f m at c\n",
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
