/* calib.c — see calib.h. Trim solve + cave_layout.json writeback. Pure + file I/O (no audio thread). */
#include "calib/calib.h"
#include "core/layout.h"        /* BWA_RQ_GRID_MAX / BWA_ROOM_EQ_MAX (the room_eq_grid schema caps) */
#include "dsp/sos.h"           /* BWA_SOS_MIN_MPS / BWA_SOS_MAX_MPS (the plausible-room guard) */
#include "os/os.h"        /* os_fopen: UTF-8 paths on Windows */
#include "core/frame.h"     /* BWA_ROOM_AHEAD / _UP / _RIGHT: the aiming sheet's angle convention */

#include <cJSON.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* the directivity correction factor for speaker i: 1 when absent, non-finite or non-positive (a
 * NaN factor would otherwise write gain_db = NaN, the same failure the level guard below prevents) */
static double cfac(const float* corr, int i) {
    if (!corr) return 1.0;
    double f = corr[i];
    return (f > 0.0 && f < 1e6) ? f : 1.0;
}

void calib_solve(const MeasureResult* m, const float (*pos)[3], const float mic[3], int n, double fs,
                 float* gain_db, float* delay_ms) {
    calib_solve_corr(m, pos, mic, n, fs, NULL, gain_db, delay_ms);
}

void calib_solve_corr(const MeasureResult* m, const float (*pos)[3], const float mic[3], int n, double fs,
                      const float* corr, float* gain_db, float* delay_ms) {
    if (!m || !pos || !mic || !gain_db || !delay_ms || n <= 0 || !(fs > 0.0)) return;

    /* delays: align every arrival to the farthest (largest measured delay). Latency cancels. */
    int maxd = 0;
    for (int i = 0; i < n; ++i) if (m[i].delay_samples > maxd) maxd = m[i].delay_samples;
    for (int i = 0; i < n; ++i) {
        double ms = (maxd - m[i].delay_samples) / fs * 1000.0;
        if (!(ms > 0.0)) ms = 0.0; else if (ms > 1000.0) ms = 1000.0;   /* NaN-safe */
        delay_ms[i] = (float)(round(ms * 1000.0) / 1000.0);
    }

    /* sensitivity = level * distance (factor out 1/r); equalize cut-only to the least-sensitive live
     * speaker so no channel is boosted. A near-silent speaker is excluded + left at 0 dB. */
    double sref = 1e30;
    for (int i = 0; i < n; ++i) {
        double dx = pos[i][0]-mic[0], dy = pos[i][1]-mic[1], dz = pos[i][2]-mic[2];
        double dist = sqrt(dx*dx + dy*dy + dz*dz); if (dist < 0.05) dist = 0.05;
        double s = (double)m[i].level * dist * cfac(corr, i);
        if (m[i].level > 1e-3 && s < sref) sref = s;
    }
    if (sref >= 1e30) sref = 1.0;                 /* every speaker silent: leave trims at unity */
    for (int i = 0; i < n; ++i) {
        /* NaN-safe skip: `level <= 1e-3` is FALSE for a NaN level (an unplugged/broken capture leg
         * delivers non-finite samples straight through deconvolution), which used to fall through
         * into the solve and write gain_db = NaN — serialized as `null`, destroying the layout. */
        if (!(m[i].level > 1e-3)) { gain_db[i] = 0.f; continue; }
        double dx = pos[i][0]-mic[0], dy = pos[i][1]-mic[1], dz = pos[i][2]-mic[2];
        double dist = sqrt(dx*dx + dy*dy + dz*dz); if (dist < 0.05) dist = 0.05;
        double s  = (double)m[i].level * dist * cfac(corr, i);
        double db = 20.0 * log10(sref / s);       /* sref <= s, so db <= 0 (cut-only) */
        if (!(db < 0.0)) db = 0.0; else if (db < -40.0) db = -40.0;   /* NaN-safe */
        gain_db[i] = (float)(round(db * 100.0) / 100.0);
    }
}

float calib_aim_tilt_db(const Directivity* d, float angle_deg, const double band_hz[2], double f2) {
    if (!d || !d->nband) return 0.f;
    float mid = directivity_loss_lin(d, angle_deg, (float)band_hz[0], (float)band_hz[1]);
    float hi  = directivity_loss_lin(d, angle_deg, (float)band_hz[1], (float)f2);
    if (!(mid > 1e-9f) || !(hi > 1e-9f)) return 0.f;
    return (float)(20.0 * log10((double)hi / (double)mid));
}

int calib_directivity_corr(const Layout* L, const float mic[3], double f_lo, double f_hi,
                           const MeasureResult* m, float* corr) {
    if (!L || !mic || !corr || !L->dir.nband) return 0;
    for (uint32_t i = 0; i < L->count; ++i) {
        float th_mic = layout_speaker_off_axis_deg(L, i, mic);
        float th_ref = layout_speaker_off_axis_deg(L, i, L->ref);
        float dmic = directivity_loss_lin(&L->dir, th_mic, (float)f_lo, (float)f_hi);
        float dref = directivity_loss_lin(&L->dir, th_ref, (float)f_lo, (float)f_hi);
        double r = (dmic > 1e-4f) ? (double)dref / (double)dmic : 1.0;
        /* the direct share of the measured level (the rest is reverberant and does not follow the
         * axis); NaN or out of range reads as 1, the free-field factor */
        double f = m ? (double)m[i].direct_frac : 1.0;
        if (!(f >= 0.0 && f <= 1.0)) f = 1.0;
        corr[i] = (float)sqrt(1.0 + f * (r * r - 1.0));     /* r == 1 gives exactly 1 for any f */
    }
    return 1;
}

static float aim_angle_deg(const float a[3], const float b[3]) {
    double c = (double)a[0]*b[0] + (double)a[1]*b[1] + (double)a[2]*b[2];
    if (c > 1.0) c = 1.0; else if (c < -1.0) c = -1.0;
    return (float)(acos(c) * 180.0 / M_PI);
}

/* the predicted tilt tabulated over 0..180 deg in 0.25 deg steps: the search evaluates ~1100
 * candidates x K bearings, and each direct evaluation is two 256-point band integrals */
#define AIM_TT_N CALIB_AIM_CURVE_N
static float aim_tilt_tab(const float* tt, float th) {
    float x = th * 4.f;
    if (!(x > 0.f)) return tt[0];
    if (x >= AIM_TT_N - 1) return tt[AIM_TT_N - 1];
    int i = (int)x; float f = x - (float)i;
    return tt[i] + (tt[i + 1] - tt[i]) * f;
}

/* tilt residual RMS (mean removed) for a candidate aim: the score the search minimizes */
static double aim_score(const float* tt, const float aim[3], const float (*bear)[3], const float* tilt_db, int K,
                        float* resid) {
    double r[64], mean = 0.0;
    for (int k = 0; k < K; ++k) {
        float th = aim_angle_deg(aim, bear[k]);
        r[k] = (double)tilt_db[k] - aim_tilt_tab(tt, th);
        mean += r[k];
    }
    mean /= K;
    double ss = 0.0;
    for (int k = 0; k < K; ++k) { r[k] -= mean; ss += r[k] * r[k]; if (resid) resid[k] = (float)r[k]; }
    return sqrt(ss / K);
}

int calib_check_aim(const Layout* L, int s, const float (*mic)[3], const float* tilt_db, int K,
                    const double band_hz[2], double f2, CalibAimResult* out) {
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!L || s < 0 || (uint32_t)s >= L->count || !mic || !tilt_db || K <= 0 || K > 64) return 0;
    const Speaker* sp = &L->speakers[s];
    memcpy(out->aim_fit, sp->aim, sizeof out->aim_fit);
    out->npos = K;
    if (!L->dir.nband) return 0;
    /* bearings from the speaker to each mic, and their spread (the fit's leverage) */
    float bear[64][3];
    for (int k = 0; k < K; ++k) unit_dir(sp->pos, mic[k], bear[k]);
    float spread = 0.f;
    for (int a = 0; a < K; ++a)
        for (int b = a + 1; b < K; ++b) { float d = aim_angle_deg(bear[a], bear[b]); if (d > spread) spread = d; }
    out->spread_deg = spread;
    static float tt[AIM_TT_N];                     /* control thread, one call at a time */
    calib_aim_curve(&L->dir, band_hz, f2, tt);
    out->rms_layout_db = (float)aim_score(tt, sp->aim, bear, tilt_db, K, NULL);
    out->rms_fit_db = out->rms_layout_db;
    if (K < 3 || spread < 8.f) return 0;           /* refuse: nothing to lever the fit with */
    /* local tangent basis around the layout aim, then a coarse disc search out to 45 deg and a fine
     * one around the best cell; every candidate is a unit vector by construction */
    float u[3], v[3];
    { float up[3] = { 0, 1, 0 };
      if (fabsf(sp->aim[1]) > 0.9f) { up[0] = 1; up[1] = 0; }
      u[0] = sp->aim[1]*up[2] - sp->aim[2]*up[1]; u[1] = sp->aim[2]*up[0] - sp->aim[0]*up[2]; u[2] = sp->aim[0]*up[1] - sp->aim[1]*up[0];
      float l = sqrtf(u[0]*u[0] + u[1]*u[1] + u[2]*u[2]); if (l < 1e-6f) return 0;
      u[0] /= l; u[1] /= l; u[2] /= l;
      v[0] = sp->aim[1]*u[2] - sp->aim[2]*u[1]; v[1] = sp->aim[2]*u[0] - sp->aim[0]*u[2]; v[2] = sp->aim[0]*u[1] - sp->aim[1]*u[0]; }
    double best = out->rms_layout_db; float bx = 0.f, by = 0.f;   /* tangent-plane offsets, degrees */
    for (int pass = 0; pass < 2; ++pass) {
        const float step = pass ? 0.5f : 3.f, reach = pass ? 3.f : 45.f;
        const float cx = bx, cy = by;
        for (float dx = -reach; dx <= reach + 1e-3f; dx += step)
            for (float dy = -reach; dy <= reach + 1e-3f; dy += step) {
                float ox = cx + dx, oy = cy + dy, rad = sqrtf(ox*ox + oy*oy);
                if (rad > 45.f) continue;
                float ang = rad * (float)(M_PI / 180.0), ca = cosf(ang), sa = rad > 1e-6f ? sinf(ang) / rad : 0.f;
                float cand[3];
                for (int j = 0; j < 3; ++j) cand[j] = ca * sp->aim[j] + sa * (ox * u[j] + oy * v[j]);
                double sc = aim_score(tt, cand, bear, tilt_db, K, NULL);
                if (sc < best - 1e-9) { best = sc; bx = ox; by = oy; memcpy(out->aim_fit, cand, sizeof out->aim_fit); }
            }
    }
    out->rms_fit_db  = (float)best;
    out->aim_err_deg = aim_angle_deg(sp->aim, out->aim_fit);
    out->ok = 1;
    return 1;
}

void calib_aim_curve(const Directivity* d, const double band_hz[2], double f2, float curve[CALIB_AIM_CURVE_N]) {
    if (!curve) return;
    for (int i = 0; i < CALIB_AIM_CURVE_N; ++i) curve[i] = calib_aim_tilt_db(d, 0.25f * (float)i, band_hz, f2);
}

/* the smallest angle (deg) at which the monotone envelope env[] reaches `v`, linear between the two
 * samples that straddle it; `imax` bounds the walk */
static float aim_envelope_angle(const float* env, int imax, float v) {
    if (v >= env[0]) return 0.f;
    for (int i = 1; i <= imax; ++i)
        if (env[i] <= v) {
            const float span = env[i - 1] - env[i];
            const float t = span > 0.f ? (env[i - 1] - v) / span : 1.f;
            return 0.25f * ((float)(i - 1) + t);
        }
    return 0.25f * (float)imax;
}

int calib_aim_invert(const float curve[CALIB_AIM_CURVE_N], float rel_db, float tol_db, CalibAimAngle* out) {
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    out->rel_db = rel_db;
    if (!curve || !isfinite(rel_db)) return 0;
    if (!(tol_db >= 0.f)) tol_db = 0.f;                    /* NaN reads as 0 too */
    /* the running minimum from 0 deg, relative to the 0 deg value, and where it stops falling. A
     * curve that never falls (no model: all zeros) cannot be inverted. */
    float env[CALIB_AIM_CURVE_N];
    env[0] = 0.f;
    int imax = 0;
    for (int i = 1; i < CALIB_AIM_CURVE_N; ++i) {
        const float v = curve[i] - curve[0];
        env[i] = v < env[i - 1] ? v : env[i - 1];
        if (env[i] < env[imax] - 1e-6f) imax = i;
    }
    if (imax == 0) return 0;
    out->max_deg   = 0.25f * (float)imax;
    out->angle_deg = aim_envelope_angle(env, imax, rel_db);
    out->lo_deg    = aim_envelope_angle(env, imax, rel_db + tol_db);
    out->hi_deg    = aim_envelope_angle(env, imax, rel_db - tol_db);
    out->beyond    = rel_db < env[imax];
    out->on_axis   = out->lo_deg == 0.f;
    out->ok = 1;
    return 1;
}

int calib_direct_tilt_db(const MeasureResult* m, float* out) {
    if (!m || !out) return 0;
    const double mid = m->band_direct[1], hi = m->band_direct[2];
    if (!(mid > 1e-12 && hi > 1e-12) || !isfinite(mid) || !isfinite(hi)) return 0;
    *out = (float)(20.0 * log10(hi / mid));
    return 1;
}

int calib_on_axis_tilt_db(const Directivity* d, const double band_hz[2], double f2, float* out) {
    if (!d || !d->nband || !d->has_on_axis || !band_hz || !out) return 0;
    const float mid = directivity_on_axis_lin(d, (float)band_hz[0], (float)band_hz[1]);
    const float hi  = directivity_on_axis_lin(d, (float)band_hz[1], (float)f2);
    if (!(mid > 1e-9f) || !(hi > 1e-9f)) return 0;
    *out = (float)(20.0 * log10((double)hi / (double)mid));
    return 1;
}

void calib_peak_reset(CalibPeakHold* p) {
    if (!p) return;
    memset(p, 0, sizeof *p);
}

static float peak_below(const CalibPeakHold* p, float v) {
    const float b = p->peak_index ? p->peak_db - v : 0.f;
    return b > 0.f ? b : 0.f;
}

float calib_peak_update(CalibPeakHold* p, float value_db, int clean, float tol_db) {
    if (!p) return 0.f;
    if (!isfinite(value_db)) return peak_below(p, p->last_db);
    if (!(tol_db >= 0.f)) tol_db = 0.f;
    ++p->n;
    p->last_db = value_db;
    if (!clean) { ++p->nrejected; p->prev_clean = 0; return peak_below(p, value_db); }
    if (p->prev_clean && fabsf(value_db - p->prev_db) <= tol_db) {
        const float pair = value_db < p->prev_db ? value_db : p->prev_db;   /* the level BOTH reached */
        if (!p->peak_index || pair > p->peak_db) { p->peak_db = pair; p->peak_index = p->n; }
    }
    p->prev_clean = 1;
    p->prev_db = value_db;
    return peak_below(p, value_db);
}

/* ---- sweep quality ---- */

int calib_arrival_window(const CalibWindow* w, const float spk[3], const float mic[3], double sos, double fs,
                         int* lo, int* hi) {
    if (!w || !spk || !mic || !lo || !hi) return 0;
    if (!isfinite(w->lat_s) || !(w->lat_early_s >= 0.0) || !(w->lat_late_s >= 0.0) || !(w->pos_m >= 0.0)) return 0;
    if (!(sos > 1.0) || !(fs > 0.0)) return 0;
    for (int a = 0; a < 3; ++a)       /* finite AND bounded: a 3e38 coordinate squares to Inf */
        if (!(fabsf(spk[a]) <= 1e4f) || !(fabsf(mic[a]) <= 1e4f)) return 0;
    if (w->lat_early_s > 10.0 || w->lat_late_s > 10.0 || w->pos_m > 100.0 || fabs(w->lat_s) > 10.0) return 0;
    const double dx = (double)spk[0] - mic[0], dy = (double)spk[1] - mic[1], dz = (double)spk[2] - mic[2];
    const double d = sqrt(dx * dx + dy * dy + dz * dz);
    if (d > 1000.0) return 0;
    const double dn = d - w->pos_m > 0.0 ? d - w->pos_m : 0.0;
    const double t0 = w->lat_s - w->lat_early_s + dn / sos;
    const double t1 = w->lat_s + w->lat_late_s + (d + w->pos_m) / sos + CALIB_WIN_SPK_S;
    *lo = (int)floor(t0 * fs);
    *hi = (int)ceil(t1 * fs) + 1;
    if (*lo < 0) *lo = 0;
    return *hi > *lo;
}

int calib_sweep_check(const MeasureResult* m, float min_snr_db) {
    if (!m) return CALIB_SWEEP_NOISY;
    if (m->outside) return CALIB_SWEEP_OUTSIDE;
    if (m->noise_n > 0 && !(m->snr_db >= min_snr_db)) return CALIB_SWEEP_NOISY;
    return CALIB_SWEEP_OK;
}

void calib_sweep_why(const MeasureResult* m, int code, double fs, char* buf, size_t cap) {
    if (!buf || cap == 0) return;
    if (!m || !(fs > 0.0)) { snprintf(buf, cap, "no measurement"); return; }
    const double ms = 1e3 / fs;
    if (code == CALIB_SWEEP_OUTSIDE)
        snprintf(buf, cap, "the strongest tap (%.2f ms) is %.1f dB above the arrival and outside the expected window "
                 "%.2f-%.2f ms: something other than this speaker was louder",
                 m->peak_any * ms, m->outside_db, m->win_lo * ms, m->win_hi * ms);
    else if (code == CALIB_SWEEP_NOISY)
        snprintf(buf, cap, "IR peak %.1f dB over its noise floor, under the %.0f dB the sweep needs",
                 m->snr_db, CALIB_SWEEP_MIN_SNR_DB);
    else
        snprintf(buf, cap, "clean: arrival %.2f ms, IR peak %.1f dB over its noise floor",
                 (m->delay_samples + m->delay_frac) * ms, m->snr_db);
}

int calib_sweeps_agree(const MeasureResult* a, const MeasureResult* b, float* d_samples, float* d_db) {
    if (!a || !b) return 0;
    const double ta = (double)a->delay_samples + a->delay_frac, tb = (double)b->delay_samples + b->delay_frac;
    const double ds = fabs(ta - tb);
    const double db = (a->level > 0.f && b->level > 0.f) ? fabs(20.0 * log10((double)a->level / (double)b->level)) : 1e9;
    if (d_samples) *d_samples = (float)ds;
    if (d_db) *d_db = (float)(db < 1e9 ? db : 999.0);
    return ds <= CALIB_SWEEP_AGREE_SAMPLES && db <= CALIB_SWEEP_AGREE_DB;
}

double calib_latency_bound_s(int zylia) { return zylia ? CALIB_WIN_LAT_DRIVER_S : CALIB_LAT_OMNI_S; }

int calib_latency_check(int zylia, double solved_s, double driver_s, double* resid_s) {
    const double r = solved_s - driver_s;
    if (resid_s) *resid_s = r;
    if (!isfinite(r)) return CALIB_LAT_WARN;
    if (r < -CALIB_LAT_FLOOR_S) return CALIB_LAT_IMPOSSIBLE;
    if (r > calib_latency_bound_s(zylia)) return CALIB_LAT_WARN;
    return CALIB_LAT_OK;
}

/* Gaussian elimination with partial pivoting on a 4x4 system A*x = b. Returns 0 if singular. */
static int solve4(double A[4][4], double b[4], double x[4]) {
    for (int c = 0; c < 4; ++c) {
        int piv = c;
        for (int r = c + 1; r < 4; ++r) if (fabs(A[r][c]) > fabs(A[piv][c])) piv = r;
        if (!(fabs(A[piv][c]) >= 1e-12)) return 0;   /* NaN-safe: a NaN pivot (NaN mic positions from a
                                                      * text file) must read SINGULAR, not solvable */
        if (piv != c) {
            for (int j = 0; j < 4; ++j) { double t = A[c][j]; A[c][j] = A[piv][j]; A[piv][j] = t; }
            double t = b[c]; b[c] = b[piv]; b[piv] = t;
        }
        for (int r = c + 1; r < 4; ++r) {
            double f = A[r][c] / A[c][c];
            for (int j = c; j < 4; ++j) A[r][j] -= f * A[c][j];
            b[r] -= f * b[c];
        }
    }
    for (int r = 3; r >= 0; --r) {
        double s = b[r];
        for (int j = r + 1; j < 4; ++j) s -= A[r][j] * x[j];
        x[r] = s / A[r][r];
    }
    return 1;
}

int calib_trilaterate(const double* range, const float (*mic)[3], int K, float* pos_out, double* latency_out) {
    if (!range || !mic || !pos_out || K < 5) return 0;
    /* |x - m_k| = range[k] - r, with r = c*tau the unknown latency-range. Squaring and subtracting the
     * k=0 equation cancels |x|^2 and r^2, leaving a system that's LINEAR in (x, r):
     *   2*x.(m_k - m_0) - 2*r*(range[k]-range[0]) = |m_k|^2 - |m_0|^2 - range[k]^2 + range[0]^2.
     * Stack k=1..K-1 and solve the 4x4 normal equations by least squares. */
    const double m0[3] = { mic[0][0], mic[0][1], mic[0][2] };
    const double r0 = range[0];
    const double n0 = m0[0]*m0[0] + m0[1]*m0[1] + m0[2]*m0[2];
    double AtA[4][4] = {{0}}, Atb[4] = {0};
    for (int k = 1; k < K; ++k) {
        double mk[3] = { mic[k][0], mic[k][1], mic[k][2] };
        double row[4] = { 2.0*(mk[0]-m0[0]), 2.0*(mk[1]-m0[1]), 2.0*(mk[2]-m0[2]), -2.0*(range[k]-r0) };
        double nk  = mk[0]*mk[0] + mk[1]*mk[1] + mk[2]*mk[2];
        double rhs = nk - n0 - range[k]*range[k] + r0*r0;
        for (int a = 0; a < 4; ++a) { for (int bb = 0; bb < 4; ++bb) AtA[a][bb] += row[a]*row[bb]; Atb[a] += row[a]*rhs; }
    }
    double th[4];
    if (!solve4(AtA, Atb, th)) return 0;
    pos_out[0] = (float)th[0]; pos_out[1] = (float)th[1]; pos_out[2] = (float)th[2];
    if (latency_out) *latency_out = th[3];
    return 1;
}

int calib_locate_known_latency(const double* range, const float (*anchor)[3], int K, double latency, float pos_io[3]) {
    if (!range || !anchor || !pos_io || K < 4 || !isfinite(latency)) return 0;
    double x[3] = { pos_io[0], pos_io[1], pos_io[2] };
    if (!isfinite(x[0]) || !isfinite(x[1]) || !isfinite(x[2])) return 0;
    for (int it = 0; it < 50; ++it) {
        double JtJ[3][3] = {{0}}, Jtr[3] = {0};
        for (int k = 0; k < K; ++k) {
            const double d[3] = { x[0] - anchor[k][0], x[1] - anchor[k][1], x[2] - anchor[k][2] };
            const double r = sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
            if (!(r > 1e-6)) return 0;
            const double e = r - (range[k] - latency);           /* residual */
            const double g[3] = { d[0] / r, d[1] / r, d[2] / r }; /* d r / d x */
            for (int a = 0; a < 3; ++a) { Jtr[a] += g[a] * e; for (int b = 0; b < 3; ++b) JtJ[a][b] += g[a] * g[b]; }
        }
        const double det = JtJ[0][0]*(JtJ[1][1]*JtJ[2][2] - JtJ[1][2]*JtJ[2][1]) - JtJ[0][1]*(JtJ[1][0]*JtJ[2][2] - JtJ[1][2]*JtJ[2][0])
                         + JtJ[0][2]*(JtJ[1][0]*JtJ[2][1] - JtJ[1][1]*JtJ[2][0]);
        if (!(fabs(det) > 1e-12)) return 0;
        double step[3];
        for (int c = 0; c < 3; ++c) {                             /* Cramer: column c replaced by Jtr */
            double M[3][3];
            for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b) M[a][b] = (b == c) ? Jtr[a] : JtJ[a][b];
            step[c] = (M[0][0]*(M[1][1]*M[2][2] - M[1][2]*M[2][1]) - M[0][1]*(M[1][0]*M[2][2] - M[1][2]*M[2][0])
                     + M[0][2]*(M[1][0]*M[2][1] - M[1][1]*M[2][0])) / det;
        }
        double sn = 0.0;
        for (int a = 0; a < 3; ++a) { x[a] -= step[a]; sn += step[a] * step[a]; }
        if (!isfinite(x[0]) || !isfinite(x[1]) || !isfinite(x[2])) return 0;
        if (sn < 1e-14) {                                         /* converged to 0.1 um */
            for (int a = 0; a < 3; ++a) pos_io[a] = (float)x[a];
            return 1;
        }
    }
    return 0;
}

int calib_trilaterate_refine(const double* range, const float (*anchor)[3], int K, float pos_io[3], double* latency_io) {
    if (!range || !anchor || !pos_io || !latency_io || K < 5) return 0;
    float x[3] = { pos_io[0], pos_io[1], pos_io[2] };
    double lat = 0.0;
    for (int it = 0; it < 200; ++it) {
        double s = 0.0;                                           /* latency = mean residual at this point */
        for (int k = 0; k < K; ++k) {
            const double dx = anchor[k][0] - x[0], dy = anchor[k][1] - x[1], dz = anchor[k][2] - x[2];
            s += range[k] - sqrt(dx*dx + dy*dy + dz*dz);
        }
        const double nl = s / K;
        float nx[3] = { x[0], x[1], x[2] };
        if (!calib_locate_known_latency(range, anchor, K, nl, nx)) return 0;
        const double mv = (nx[0]-x[0])*(nx[0]-x[0]) + (nx[1]-x[1])*(nx[1]-x[1]) + (nx[2]-x[2])*(nx[2]-x[2]);
        memcpy(x, nx, sizeof x);
        const double dl = nl - lat;
        lat = nl;
        if (it > 0 && mv < 1e-14 && dl * dl < 1e-14) {
            memcpy(pos_io, x, sizeof x);
            *latency_io = lat;
            return 1;
        }
    }
    return 0;
}

double calib_latency_dilution(const float (*anchor)[3], int K, const float x[3]) {
    if (!anchor || !x || K < 5) return -1.0;
    double G[4][4] = {{0}};
    for (int k = 0; k < K; ++k) {
        const double d[3] = { anchor[k][0] - x[0], anchor[k][1] - x[1], anchor[k][2] - x[2] };
        const double r = sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
        if (!(r > 1e-6)) return -1.0;
        const double row[4] = { -d[0] / r, -d[1] / r, -d[2] / r, 1.0 };
        for (int a = 0; a < 4; ++a) for (int b = 0; b < 4; ++b) G[a][b] += row[a] * row[b];
    }
    double e3[4] = { 0.0, 0.0, 0.0, 1.0 }, col[4];
    if (!solve4(G, e3, col) || !(col[3] > 0.0)) return -1.0;    /* col = (J^T J)^-1 e3, so col[3] = [..]_33 */
    return sqrt((double)K * col[3]);
}

static int dcmp(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b; return x < y ? -1 : x > y ? 1 : 0;
}

void calib_check_drift(const double* range, const float (*pos)[3], const float mic[3], int n, float* deviation_m) {
    if (!range || !pos || !mic || !deviation_m || n <= 0) return;
    double* resid = (double*)malloc((size_t)n * sizeof(double));
    double* tmp   = (double*)malloc((size_t)n * sizeof(double));
    if (!resid || !tmp) { free(resid); free(tmp); return; }
    for (int s = 0; s < n; ++s) {                              /* residual = range - expected distance = the common latency if unmoved */
        double dx = pos[s][0]-mic[0], dy = pos[s][1]-mic[1], dz = pos[s][2]-mic[2];
        resid[s] = range[s] - sqrt(dx*dx + dy*dy + dz*dz);
        tmp[s] = resid[s];
    }
    qsort(tmp, (size_t)n, sizeof(double), dcmp);              /* median residual = the system latency (outlier-robust) */
    double med = (n & 1) ? tmp[n/2] : 0.5 * (tmp[n/2 - 1] + tmp[n/2]);
    for (int s = 0; s < n; ++s) deviation_m[s] = (float)(resid[s] - med);
    free(resid); free(tmp);
}

static char* read_file(const char* path, long* len_out) {
    FILE* f = os_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len < 0) { fclose(f); return NULL; }
    char* buf = (char*)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)len, f); fclose(f);
    buf[rd] = '\0'; if (len_out) *len_out = (long)rd;
    return buf;
}

/* The speaker record whose "index" is idx. Every writer below holds measurements indexed by SPEAKER
 * (the loader files record k at L.speakers[index], in any array order), so writing measurement i into
 * array element i put it on the wrong speaker in any file whose records are not in index order. The
 * loader has already insisted on a unique numeric index for every record. */
static cJSON* spk_by_index(cJSON* speakers, int idx) {
    cJSON* sp;
    cJSON_ArrayForEach(sp, speakers) {
        cJSON* ix = cJSON_GetObjectItemCaseSensitive(sp, "index");
        if (cJSON_IsNumber(ix) && ix->valuedouble == (double)idx) return sp;
    }
    return NULL;
}

int calib_write_layout(const char* in_path, const char* out_path,
                       const float* gain_db, const float* delay_ms, int n, char* err, size_t errcap) {
    #define FAIL(msg) do { if (err && errcap) snprintf(err, errcap, "%s", msg); goto fail; } while (0)
    char* text = NULL; cJSON* root = NULL; char* outtext = NULL; int ok = 0;

    text = read_file(in_path, NULL);
    if (!text) { if (err && errcap) snprintf(err, errcap, "calib: cannot read %s", in_path); return 0; }
    root = cJSON_Parse(text);
    if (!root) FAIL("calib: layout is not valid JSON");
    cJSON* speakers = cJSON_GetObjectItemCaseSensitive(root, "speakers");
    if (!cJSON_IsArray(speakers)) FAIL("calib: layout has no 'speakers' array");
    if (cJSON_GetArraySize(speakers) != n) FAIL("calib: speaker count does not match the measurements");

    /* refuse values the loader is KNOWN to reject before touching the file: a NaN trim (a broken
     * capture leg survives the solve as NaN) serializes as JSON `null`, and out_path usually IS the
     * layout — the write would destroy the good calibration in place and only fail at next load */
    for (int i = 0; i < n; ++i)
        if (!isfinite(gain_db[i]) || gain_db[i] < -100.f || gain_db[i] > 24.f ||
            !isfinite(delay_ms[i]) || delay_ms[i] < 0.f || delay_ms[i] > 1000.f)
            FAIL("calib: refusing to write a non-finite/out-of-range trim (bad measurement?)");

    for (int i = 0; i < n; ++i) {
        cJSON* sp = spk_by_index(speakers, i);
        if (!sp) FAIL("calib: layout has no speaker record with that index");
        cJSON* g  = cJSON_GetObjectItemCaseSensitive(sp, "gain_db");
        cJSON* d  = cJSON_GetObjectItemCaseSensitive(sp, "delay_ms");
        if (g) cJSON_SetNumberValue(g, gain_db[i]);  else cJSON_AddNumberToObject(sp, "gain_db",  gain_db[i]);
        if (d) cJSON_SetNumberValue(d, delay_ms[i]); else cJSON_AddNumberToObject(sp, "delay_ms", delay_ms[i]);
    }

    outtext = cJSON_Print(root);
    if (!outtext) FAIL("calib: failed to serialize layout");
    FILE* f = os_fopen(out_path, "wb");
    if (!f) FAIL("calib: cannot open output for writing");
    fwrite(outtext, 1, strlen(outtext), f); fclose(f);
    ok = 1;
fail:
    free(outtext); cJSON_Delete(root); free(text);
    return ok;
    #undef FAIL
}

int calib_read_sos(const char* path, double* out_mps) {
    if (!path || !out_mps) return 0;
    char* text = read_file(path, NULL);
    if (!text) return 0;
    cJSON* root = cJSON_Parse(text);
    int ok = 0;
    if (root) {
        cJSON* ref = cJSON_GetObjectItemCaseSensitive(root, "reference");
        if (cJSON_IsObject(ref)) {
            cJSON* c = cJSON_GetObjectItemCaseSensitive(ref, "speed_of_sound_mps");
            if (cJSON_IsNumber(c) && c->valuedouble >= BWA_SOS_MIN_MPS && c->valuedouble <= BWA_SOS_MAX_MPS) {
                *out_mps = c->valuedouble; ok = 1;
            }
        }
        cJSON_Delete(root);
    }
    free(text);
    return ok;
}

int calib_write_sos(const char* in_path, const char* out_path, double mps, char* err, size_t errcap) {
    #define FAIL(msg) do { if (err && errcap) snprintf(err, errcap, "%s", msg); goto fail; } while (0)
    char* text = NULL; cJSON* root = NULL; char* outtext = NULL; int ok = 0;
    if (!(mps >= BWA_SOS_MIN_MPS && mps <= BWA_SOS_MAX_MPS)) {
        if (err && errcap) snprintf(err, errcap, "calib: speed of sound %.1f m/s out of range", mps);
        return 0;
    }
    text = read_file(in_path, NULL);
    if (!text) { if (err && errcap) snprintf(err, errcap, "calib: cannot read %s", in_path); return 0; }
    root = cJSON_Parse(text);
    if (!root) FAIL("calib: layout is not valid JSON");
    /* the `reference` block is provenance: create it if the file predates this field */
    cJSON* ref = cJSON_GetObjectItemCaseSensitive(root, "reference");
    if (!cJSON_IsObject(ref)) { cJSON_DeleteItemFromObjectCaseSensitive(root, "reference");
                                ref = cJSON_AddObjectToObject(root, "reference"); }
    if (!ref) FAIL("calib: cannot create the 'reference' block");
    /* 2 decimals: 0.01 m/s is 3e-5 of c, about 0.1 mm on a 4 m range — far below anything that
     * matters, and it keeps the file readable instead of carrying a float artifact. */
    mps = round(mps * 100.0) / 100.0;
    cJSON* c = cJSON_GetObjectItemCaseSensitive(ref, "speed_of_sound_mps");
    if (c) cJSON_SetNumberValue(c, mps); else cJSON_AddNumberToObject(ref, "speed_of_sound_mps", mps);

    outtext = cJSON_Print(root);
    if (!outtext) FAIL("calib: failed to serialize layout");
    FILE* f = os_fopen(out_path, "wb");
    if (!f) FAIL("calib: cannot open output for writing");
    fwrite(outtext, 1, strlen(outtext), f); fclose(f);
    ok = 1;
fail:
    free(outtext); cJSON_Delete(root); free(text);
    return ok;
    #undef FAIL
}

int calib_eq(const float* ir, int nir, int first_refl, double fs, int ntaps, float* taps) {
    if (!ir || !taps) return 0;
    /* gate to just before the first reflection so we invert the SPEAKER, not the room; if unknown, a
     * 4 ms window (long enough to resolve the speaker's response, short enough to exclude most rooms). */
    int gate = measure_direct_gate(first_refl, fs);
    if (gate > nir) gate = nir;
    if (gate < 16) return 0;
    return measure_correction(ir, nir, 0, gate, 30.0, 18000.0, fs, 6.0, 18.0, ntaps, taps);
}

int calib_write_eq(const char* in_path, const char* out_path, const float* taps, const uint16_t* lens,
                   int n, int max_taps, char* err, size_t errcap) {
    #define FAIL(msg) do { if (err && errcap) snprintf(err, errcap, "%s", msg); goto fail; } while (0)
    char* text = NULL; cJSON* root = NULL; char* outtext = NULL; int ok = 0;
    text = read_file(in_path, NULL);
    if (!text) { if (err && errcap) snprintf(err, errcap, "calib: cannot read %s", in_path); return 0; }
    root = cJSON_Parse(text);
    if (!root) FAIL("calib: layout is not valid JSON");
    cJSON* speakers = cJSON_GetObjectItemCaseSensitive(root, "speakers");
    if (!cJSON_IsArray(speakers)) FAIL("calib: layout has no 'speakers' array");
    if (cJSON_GetArraySize(speakers) != n) FAIL("calib: speaker count does not match the measurements");

    for (int i = 0; i < n; ++i) {
        cJSON* sp = spk_by_index(speakers, i);
        if (!sp) FAIL("calib: layout has no speaker record with that index");
        cJSON_DeleteItemFromObjectCaseSensitive(sp, "eq");     /* replace any prior correction */
        int m = lens[i]; if (m > max_taps) m = max_taps;
        if (m > 0) {
            cJSON* arr = cJSON_CreateArray();
            if (!arr) FAIL("calib: eq array alloc");
            for (int t = 0; t < m; ++t) cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)taps[(size_t)i*max_taps + t]));
            cJSON_AddItemToObject(sp, "eq", arr);
        }
    }
    outtext = cJSON_Print(root);
    if (!outtext) FAIL("calib: failed to serialize layout");
    FILE* f = os_fopen(out_path, "wb");
    if (!f) FAIL("calib: cannot open output for writing");
    fwrite(outtext, 1, strlen(outtext), f); fclose(f);
    ok = 1;
fail:
    free(outtext); cJSON_Delete(root); free(text);
    return ok;
    #undef FAIL
}

#define BWA_ROOM_EQ_SPLIT_HZ 200.0   /* the FIR corrects above this; the modal cuts own [30, split] */

int calib_room_eq(const float* ir, int nir, int first_refl, double fs, int ntaps, float* taps,
                  MeasureEqSection* cuts, int max_cuts) {
    if (!ir || !taps || !cuts) return -1;
    /* same HF gate policy as calib_eq: at high frequencies the FD window shrinks to the direct sound */
    int gate = measure_direct_gate(first_refl, fs);
    if (gate > nir) gate = nir;
    if (gate < 16) return -1;
    /* 6 cycles/f window (~1/6-octave resolution — the broad-stroke smoothing that survives head sway),
     * up to 400 ms of room; boosts capped at +3 dB (a seated head still sways — never fight nulls hard). */
    if (!measure_correction_room(ir, nir, 0, gate, 6.0, 0.4,
                                 BWA_ROOM_EQ_SPLIT_HZ, 18000.0, fs, 3.0, 18.0, ntaps, taps)) return -1;
    return measure_room_cuts(ir, nir, 0, fs, 30.0, BWA_ROOM_EQ_SPLIT_HZ, 12.0, max_cuts, cuts);
}

int calib_write_room_eq(const char* in_path, const char* out_path,
                        const MeasureEqSection* cuts, const int* counts, int n,
                        int max_sections, char* err, size_t errcap) {
    #define FAIL(msg) do { if (err && errcap) snprintf(err, errcap, "%s", msg); goto fail; } while (0)
    char* text = NULL; cJSON* root = NULL; char* outtext = NULL; int ok = 0;
    text = read_file(in_path, NULL);
    if (!text) { if (err && errcap) snprintf(err, errcap, "calib: cannot read %s", in_path); return 0; }
    root = cJSON_Parse(text);
    if (!root) FAIL("calib: layout is not valid JSON");
    cJSON* speakers = cJSON_GetObjectItemCaseSensitive(root, "speakers");
    if (!cJSON_IsArray(speakers)) FAIL("calib: layout has no 'speakers' array");
    if (cJSON_GetArraySize(speakers) != n) FAIL("calib: speaker count does not match the measurements");

    /* same refusal as the trims writer (calib_write_layout): a NaN fit serializes as JSON `null`,
     * and out_path usually IS the layout — the write would destroy the good calibration in place */
    for (int i = 0; i < n; ++i) {
        int m = counts[i]; if (m > max_sections) m = max_sections;
        for (int s = 0; s < m; ++s) {
            const MeasureEqSection* c = &cuts[(size_t)i * max_sections + s];
            if (!isfinite(c->fc) || !isfinite(c->gain_db) || !isfinite(c->q))
                FAIL("calib: refusing to write a non-finite room-EQ section (bad measurement?)");
        }
    }

    for (int i = 0; i < n; ++i) {
        cJSON* sp = spk_by_index(speakers, i);
        if (!sp) FAIL("calib: layout has no speaker record with that index");
        cJSON_DeleteItemFromObjectCaseSensitive(sp, "room_eq");   /* replace any prior */
        int m = counts[i]; if (m > max_sections) m = max_sections;
        if (m > 0) {
            cJSON* arr = cJSON_CreateArray();
            if (!arr) FAIL("calib: room_eq array alloc");
            for (int s = 0; s < m; ++s) {
                const MeasureEqSection* c = &cuts[(size_t)i * max_sections + s];
                cJSON* o = cJSON_CreateObject();
                if (!o) FAIL("calib: room_eq section alloc");
                cJSON_AddNumberToObject(o, "fc",      round((double)c->fc * 10.0) / 10.0);
                cJSON_AddNumberToObject(o, "gain_db", round((double)c->gain_db * 100.0) / 100.0);
                cJSON_AddNumberToObject(o, "q",       round((double)c->q * 100.0) / 100.0);
                cJSON_AddItemToArray(arr, o);
            }
            cJSON_AddItemToObject(sp, "room_eq", arr);
        }
    }
    outtext = cJSON_Print(root);
    if (!outtext) FAIL("calib: failed to serialize layout");
    FILE* f = os_fopen(out_path, "wb");
    if (!f) FAIL("calib: cannot open output for writing");
    fwrite(outtext, 1, strlen(outtext), f); fclose(f);
    ok = 1;
fail:
    free(outtext); cJSON_Delete(root); free(text);
    return ok;
    #undef FAIL
}

static int fcmp(const void* a, const void* b) {
    float x = *(const float*)a, y = *(const float*)b; return x < y ? -1 : x > y ? 1 : 0;
}
static float fmedian(float* v, int n) {   /* sorts v in place */
    qsort(v, (size_t)n, sizeof(float), fcmp);
    return (n & 1) ? v[n/2] : 0.5f * (v[n/2 - 1] + v[n/2]);
}

int calib_room_grid_merge(const MeasureEqSection* cuts, const int* counts, int npos, int max_in,
                          double tol_rel, int max_out, float* fc, float* q, float* gain_db) {
    if (!cuts || !counts || !fc || !q || !gain_db || npos <= 0 || max_in <= 0 || max_out <= 0) return 0;
    if (npos > (int)BWA_RQ_GRID_MAX || max_in > BWA_ROOM_EQ_MAX) return 0;   /* schema caps bound the stack arrays */
    memset(gain_db, 0, (size_t)npos * (size_t)max_out * sizeof(float));

    /* flatten every real cut (a 0 dB section is congruence filler, not a measured mode) */
    int cap = npos * max_in, m = 0;
    float* ifc = (float*)malloc((size_t)cap * 3 * sizeof(float));
    int*   ip  = (int*)  malloc((size_t)cap * sizeof(int));
    if (!ifc || !ip) { free(ifc); free(ip); return 0; }
    float* iq = ifc + cap, *ig = ifc + 2 * cap;
    for (int p = 0; p < npos; ++p) {
        int cnum = counts[p] > max_in ? max_in : counts[p];
        for (int s = 0; s < cnum; ++s) {
            const MeasureEqSection* c = &cuts[(size_t)p * max_in + s];
            if (!(c->fc > 0.f) || !(c->q > 0.f) || c->gain_db > -0.05f) continue;
            ifc[m] = c->fc; iq[m] = c->q; ig[m] = c->gain_db; ip[m] = p; ++m;
        }
    }
    if (!m) { free(ifc); free(ip); return 0; }

    for (int i = 1; i < m; ++i) {                            /* insertion sort by fc (m <= npos*max_in, tiny) */
        float tf = ifc[i], tq = iq[i], tg = ig[i]; int tp = ip[i]; int j = i - 1;
        while (j >= 0 && ifc[j] > tf) { ifc[j+1]=ifc[j]; iq[j+1]=iq[j]; ig[j+1]=ig[j]; ip[j+1]=ip[j]; --j; }
        ifc[j+1] = tf; iq[j+1] = tq; ig[j+1] = tg; ip[j+1] = tp;
    }

    /* greedy clusters over the sorted list: a run belongs together while fc stays within tol_rel of
     * the run's lowest member (the same room mode read from different mic spots) */
    enum { CLU_CAP = BWA_RQ_GRID_MAX * BWA_ROOM_EQ_MAX };
    int cstart[CLU_CAP], clen[CLU_CAP], nclu = 0;
    for (int i = 0; i < m; ) {
        int j = i + 1;
        while (j < m && ifc[j] <= ifc[i] * (float)(1.0 + tol_rel)) ++j;
        cstart[nclu] = i; clen[nclu] = j - i; ++nclu;
        i = j;
    }

    float cfc[CLU_CAP], cq[CLU_CAP], cdepth[CLU_CAP]; int keep[CLU_CAP];
    for (int cI = 0; cI < nclu; ++cI) {                      /* per cluster: median fc/q, deepest member */
        float tmp[CLU_CAP];
        int s0 = cstart[cI], L = clen[cI];
        memcpy(tmp, &ifc[s0], (size_t)L * sizeof(float)); cfc[cI] = fmedian(tmp, L);
        memcpy(tmp, &iq[s0],  (size_t)L * sizeof(float)); cq[cI]  = fmedian(tmp, L);
        float d = 0.f;
        for (int i = s0; i < s0 + L; ++i) if (ig[i] < d) d = ig[i];
        cdepth[cI] = d;
        keep[cI] = 1;
    }
    int nsel = nclu;
    while (nsel > max_out) {                                 /* over the ladder cap: drop the shallowest */
        int worst = -1; float wd = -1e9f;
        for (int cI = 0; cI < nclu; ++cI)
            if (keep[cI] && cdepth[cI] > wd) { wd = cdepth[cI]; worst = cI; }
        keep[worst] = 0; --nsel;
    }

    int j = 0;
    for (int cI = 0; cI < nclu; ++cI) {                      /* emit in fc order (the list is fc-sorted) */
        if (!keep[cI]) continue;
        fc[j] = cfc[cI]; q[j] = cq[cI];
        for (int i = cstart[cI]; i < cstart[cI] + clen[cI]; ++i) {
            float* g = &gain_db[(size_t)ip[i] * max_out + j];
            if (ig[i] < *g) *g = ig[i];                      /* a position's deepest read of this mode */
        }
        ++j;
    }
    free(ifc); free(ip);
    return j;
}

int calib_write_room_eq_grid(const char* in_path, const char* out_path, const float mic[3],
                             const MeasureEqSection* cuts, const int* counts, int n,
                             int max_sections, char* err, size_t errcap) {
    return calib_write_room_eq_grid_n(in_path, out_path, 1, (const float (*)[3])mic, cuts, counts, n,
                                      max_sections, err, errcap);
}

int calib_write_room_eq_grid_n(const char* in_path, const char* out_path, int nmic, const float (*mics)[3],
                               const MeasureEqSection* cuts, const int* counts, int n,
                               int max_sections, char* err, size_t errcap) {
    #define FAIL(msg) do { if (err && errcap) snprintf(err, errcap, "%s", msg); goto fail; } while (0)
    char* text = NULL; cJSON* root = NULL; char* outtext = NULL; int ok = 0;
    cJSON* narr = NULL;             /* the rebuilt grid; owned here until attached to root */
    MeasureEqSection* all = NULL;   /* [pos][speaker][BWA_ROOM_EQ_MAX] — every position's cuts */
    int* acount = NULL;             /* [pos][speaker] used sections */
    float gpos[BWA_RQ_GRID_MAX][3]; int npos = 0;

    text = read_file(in_path, NULL);
    if (!text) { if (err && errcap) snprintf(err, errcap, "calib: cannot read %s", in_path); return 0; }
    root = cJSON_Parse(text);
    if (!root) FAIL("calib: layout is not valid JSON");
    cJSON* speakers = cJSON_GetObjectItemCaseSensitive(root, "speakers");
    if (!cJSON_IsArray(speakers)) FAIL("calib: layout has no 'speakers' array");
    if (cJSON_GetArraySize(speakers) != n) FAIL("calib: speaker count does not match the measurements");

    /* same refusal as the trims writer: the schema clamps below are two-sided compares that PASS
     * NaN, so a NaN fit would serialize as JSON `null` and destroy the layout in place */
    if (nmic < 1 || !mics) FAIL("calib: no grid position to write");
    for (int k = 0; k < nmic; ++k) {
        const float* mic = mics[k];
        if (!isfinite(mic[0]) || !isfinite(mic[1]) || !isfinite(mic[2]))
            FAIL("calib: refusing to write a non-finite mic position");
        for (int s = 0; s < n; ++s) {
            int m = counts[(size_t)k * n + s] > BWA_ROOM_EQ_MAX ? BWA_ROOM_EQ_MAX : counts[(size_t)k * n + s];
            if (m > max_sections) m = max_sections;
            for (int t = 0; t < m; ++t) {
                const MeasureEqSection* c = &cuts[((size_t)k * n + s) * max_sections + t];
                if (!isfinite(c->fc) || !isfinite(c->gain_db) || !isfinite(c->q))
                    FAIL("calib: refusing to write a non-finite room-EQ section (bad measurement?)");
            }
        }
    }

    all    = (MeasureEqSection*)calloc((size_t)BWA_RQ_GRID_MAX * n * BWA_ROOM_EQ_MAX, sizeof *all);
    acount = (int*)calloc((size_t)BWA_RQ_GRID_MAX * n, sizeof *acount);
    if (!all || !acount) FAIL("calib: out of memory");
    #define ALL(p, s)    (&all[((size_t)(p) * n + (s)) * BWA_ROOM_EQ_MAX])
    #define ACOUNT(p, s) (acount[(size_t)(p) * n + (s)])

    /* read back the existing grid: each entry's sections become that position's cuts for the
     * re-merge (its 0 dB congruence fillers are skipped by calib_room_grid_merge) */
    cJSON* grid = cJSON_GetObjectItemCaseSensitive(root, "room_eq_grid");
    if (cJSON_IsArray(grid)) {
        int np = cJSON_GetArraySize(grid);
        if (np > (int)BWA_RQ_GRID_MAX) np = BWA_RQ_GRID_MAX;
        for (int p = 0; p < np; ++p) {
            cJSON* ent  = cJSON_GetArrayItem(grid, p);
            cJSON* posj = cJSON_IsObject(ent) ? cJSON_GetObjectItemCaseSensitive(ent, "position") : NULL;
            cJSON* spks = cJSON_IsObject(ent) ? cJSON_GetObjectItemCaseSensitive(ent, "speakers") : NULL;
            if (!cJSON_IsArray(posj) || cJSON_GetArraySize(posj) != 3 ||
                !cJSON_IsArray(spks) || cJSON_GetArraySize(spks) != n)
                FAIL("calib: existing room_eq_grid entry is malformed");
            for (int c = 0; c < 3; ++c) gpos[npos][c] = (float)cJSON_GetArrayItem(posj, c)->valuedouble;
            for (int s = 0; s < n; ++s) {
                cJSON* secs = cJSON_GetArrayItem(spks, s);
                int m = cJSON_IsArray(secs) ? cJSON_GetArraySize(secs) : 0;
                if (m > BWA_ROOM_EQ_MAX) m = BWA_ROOM_EQ_MAX;
                for (int t = 0; t < m; ++t) {
                    cJSON* o = cJSON_GetArrayItem(secs, t);
                    MeasureEqSection* dst = &ALL(npos, s)[t];
                    cJSON* fj = cJSON_GetObjectItemCaseSensitive(o, "fc");
                    cJSON* gj = cJSON_GetObjectItemCaseSensitive(o, "gain_db");
                    cJSON* qj = cJSON_GetObjectItemCaseSensitive(o, "q");
                    if (!cJSON_IsNumber(fj) || !cJSON_IsNumber(gj) || !cJSON_IsNumber(qj))
                        FAIL("calib: existing room_eq_grid section is malformed");
                    dst->fc = (float)fj->valuedouble; dst->gain_db = (float)gj->valuedouble; dst->q = (float)qj->valuedouble;
                }
                ACOUNT(npos, s) = m;
            }
            ++npos;
        }
    }

    /* replace the entry at (or append) each mic position, in order: the same result as one call per
     * mic, but a grid that fills up part-way fails here, before anything is written */
    for (int k = 0; k < nmic; ++k) {
        const float* mic = mics[k];
        int slot = -1;
        for (int p = 0; p < npos; ++p) {
            float dx = gpos[p][0]-mic[0], dy = gpos[p][1]-mic[1], dz = gpos[p][2]-mic[2];
            if (dx*dx + dy*dy + dz*dz < CALIB_RQ_GRID_REPLACE_M * CALIB_RQ_GRID_REPLACE_M) { slot = p; break; }
        }
        if (slot < 0) {
            if (npos >= (int)BWA_RQ_GRID_MAX) FAIL("calib: room_eq_grid is full (16 positions)");
            slot = npos++;
        }
        memcpy(gpos[slot], mic, sizeof(float) * 3);
        for (int s = 0; s < n; ++s) {
            const int ks = k * n + s;
            int m = counts[ks] > BWA_ROOM_EQ_MAX ? BWA_ROOM_EQ_MAX : counts[ks];
            if (m > max_sections) m = max_sections;
            for (int t = 0; t < m; ++t) ALL(slot, s)[t] = cuts[(size_t)ks * max_sections + t];
            ACOUNT(slot, s) = m;
        }
    }

    /* re-merge every speaker across all positions and rewrite the congruent grid */
    {
        narr = cJSON_CreateArray();
        if (!narr) FAIL("calib: room_eq_grid array alloc");
        cJSON* entries[BWA_RQ_GRID_MAX];
        for (int p = 0; p < npos; ++p) {
            cJSON* ent = cJSON_CreateObject();
            cJSON* posj = cJSON_CreateArray();
            cJSON* spks = cJSON_CreateArray();
            if (!ent || !posj || !spks) FAIL("calib: room_eq_grid entry alloc");
            for (int c = 0; c < 3; ++c)
                cJSON_AddItemToArray(posj, cJSON_CreateNumber(round((double)gpos[p][c] * 1000.0) / 1000.0));
            cJSON_AddItemToObject(ent, "position", posj);
            cJSON_AddItemToObject(ent, "speakers", spks);
            cJSON_AddItemToArray(narr, ent);
            entries[p] = spks;
        }
        MeasureEqSection percut[BWA_RQ_GRID_MAX * BWA_ROOM_EQ_MAX];
        int   percnt[BWA_RQ_GRID_MAX];
        float lfc[BWA_ROOM_EQ_MAX], lq[BWA_ROOM_EQ_MAX], lg[BWA_RQ_GRID_MAX * BWA_ROOM_EQ_MAX];
        for (int s = 0; s < n; ++s) {
            for (int p = 0; p < npos; ++p) {
                memcpy(&percut[(size_t)p * BWA_ROOM_EQ_MAX], ALL(p, s), sizeof(MeasureEqSection) * BWA_ROOM_EQ_MAX);
                percnt[p] = ACOUNT(p, s);
            }
            int lad = calib_room_grid_merge(percut, percnt, npos, BWA_ROOM_EQ_MAX,
                                            0.08, BWA_ROOM_EQ_MAX, lfc, lq, lg);
            for (int p = 0; p < npos; ++p) {
                cJSON* secs = cJSON_CreateArray();
                if (!secs) FAIL("calib: room_eq_grid sections alloc");
                for (int j = 0; j < lad; ++j) {              /* the FULL ladder at every position (congruent) */
                    cJSON* o = cJSON_CreateObject();
                    if (!o) FAIL("calib: room_eq_grid section alloc");
                    /* clamp into the loader's schema ranges so the writeback always round-trips */
                    double wfc = round((double)lfc[j] * 10.0) / 10.0;
                    double wg  = round((double)lg[(size_t)p * BWA_ROOM_EQ_MAX + j] * 100.0) / 100.0;
                    double wq  = round((double)lq[j] * 100.0) / 100.0;
                    if (wfc < 10.0)  wfc = 10.0;  else if (wfc > 1000.0) wfc = 1000.0;
                    if (wg  < -24.0) wg  = -24.0; else if (wg  > 0.0)    wg  = 0.0;
                    if (wq  < 0.25)  wq  = 0.25;  else if (wq  > 24.0)   wq  = 24.0;
                    cJSON_AddNumberToObject(o, "fc",      wfc);
                    cJSON_AddNumberToObject(o, "gain_db", wg);
                    cJSON_AddNumberToObject(o, "q",       wq);
                    cJSON_AddItemToArray(secs, o);
                }
                cJSON_AddItemToArray(entries[p], secs);
            }
            cJSON* sp = spk_by_index(speakers, s);           /* the schemes are mutually exclusive */
            if (!sp) FAIL("calib: layout has no speaker record with that index");
            cJSON_DeleteItemFromObjectCaseSensitive(sp, "room_eq");
        }
        cJSON_DeleteItemFromObjectCaseSensitive(root, "room_eq_grid");
        cJSON_AddItemToObject(root, "room_eq_grid", narr);
        narr = NULL;                                     /* root owns it now (fail must not double-free) */
    }

    outtext = cJSON_Print(root);
    if (!outtext) FAIL("calib: failed to serialize layout");
    FILE* f = os_fopen(out_path, "wb");
    if (!f) FAIL("calib: cannot open output for writing");
    fwrite(outtext, 1, strlen(outtext), f); fclose(f);
    ok = 1;
fail:
    cJSON_Delete(narr);                                  /* non-NULL only if a FAIL fired pre-attach */
    free(all); free(acount);
    free(outtext); cJSON_Delete(root); free(text);
    return ok;
    #undef ACOUNT
    #undef ALL
    #undef FAIL
}

int calib_write_positions(const char* in_path, const char* out_path, const float (*pos)[3], int n,
                          int* plan_recorded, char* err, size_t errcap) {
    #define FAIL(msg) do { if (err && errcap) snprintf(err, errcap, "%s", msg); goto fail; } while (0)
    char* text = NULL; cJSON* root = NULL; char* outtext = NULL; int ok = 0;
    if (plan_recorded) *plan_recorded = 0;

    text = read_file(in_path, NULL);
    if (!text) { if (err && errcap) snprintf(err, errcap, "calib: cannot read %s", in_path); return 0; }
    root = cJSON_Parse(text);
    if (!root) FAIL("calib: layout is not valid JSON");
    cJSON* speakers = cJSON_GetObjectItemCaseSensitive(root, "speakers");
    if (!cJSON_IsArray(speakers)) FAIL("calib: layout has no 'speakers' array");
    if (cJSON_GetArraySize(speakers) != n) FAIL("calib: speaker count does not match the positions");

    /* same refusal as calib_write_layout: a NaN position (NaN mic file -> a "successful"
     * trilateration) serializes as `null` and destroys the layout in place */
    for (int i = 0; i < n; ++i)
        if (!isfinite(pos[i][0]) || !isfinite(pos[i][1]) || !isfinite(pos[i][2]) ||
            fabs(pos[i][0]) > 1000.f || fabs(pos[i][1]) > 1000.f || fabs(pos[i][2]) > 1000.f)
            FAIL("calib: refusing to write a non-finite/out-of-range position (degenerate solve?)");

    int nplan = 0;
    for (int i = 0; i < n; ++i) {
        cJSON* sp = spk_by_index(speakers, i);
        if (!sp) FAIL("calib: layout has no speaker record with that index");
        /* a measurement replaces `position`: the first survey keeps what it replaces as the plan */
        if (layout_json_keep_plan(sp) == 1) ++nplan;
        double xyz[3] = { round(pos[i][0]*1000.0)/1000.0, round(pos[i][1]*1000.0)/1000.0, round(pos[i][2]*1000.0)/1000.0 };
        cJSON* p = cJSON_GetObjectItemCaseSensitive(sp, "position");
        if (cJSON_IsArray(p) && cJSON_GetArraySize(p) == 3) {
            for (int j = 0; j < 3; ++j) cJSON_SetNumberValue(cJSON_GetArrayItem(p, j), xyz[j]);
        } else {
            cJSON_DeleteItemFromObjectCaseSensitive(sp, "position");
            cJSON* arr = cJSON_CreateArray();
            for (int j = 0; j < 3; ++j) cJSON_AddItemToArray(arr, cJSON_CreateNumber(xyz[j]));
            cJSON_AddItemToObject(sp, "position", arr);
        }
    }

    outtext = cJSON_Print(root);
    if (!outtext) FAIL("calib: failed to serialize layout");
    FILE* f = os_fopen(out_path, "wb");
    if (!f) FAIL("calib: cannot open output for writing");
    fwrite(outtext, 1, strlen(outtext), f); fclose(f);
    if (plan_recorded) *plan_recorded = nplan;
    ok = 1;
fail:
    free(outtext); cJSON_Delete(root); free(text);
    return ok;
    #undef FAIL
}

/* ---- the second pass: --verify (calib.h) ---- */

int calib_verify_residuals(const MeasureResult* m, const float (*pos)[3], const float mic[3],
                           const float align_pt[3], int n, double fs, double c, const float* corr,
                           float* arrival_us, float* level_db, int* flags, CalibVerifySummary* sum) {
    if (sum) memset(sum, 0, sizeof *sum);
    if (!m || !pos || !mic || !align_pt || !arrival_us || !level_db || !flags || n <= 0 || !(fs > 0.0) || !(c > 1.0))
        return -1;
    double* ra  = (double*)malloc((size_t)n * sizeof(double));   /* raw arrival residual, s */
    double* rl  = (double*)malloc((size_t)n * sizeof(double));   /* raw normalized level, dB */
    float*  tmp = (float*)malloc((size_t)n * sizeof(float));
    if (!ra || !rl || !tmp) { free(ra); free(rl); free(tmp); return -1; }
    int nlive = 0;
    for (int k = 0; k < n; ++k) {
        flags[k] = 0;
        const double lv = (double)m[k].level;
        const double t  = ((double)m[k].delay_samples + (double)m[k].delay_frac) / fs;
        if (!(lv > 0.0) || !isfinite(lv) || !isfinite(t)) { flags[k] = CALIB_VERIFY_FLAG_DEAD; continue; }
        double dx = pos[k][0]-mic[0], dy = pos[k][1]-mic[1], dz = pos[k][2]-mic[2];
        double dm = sqrt(dx*dx + dy*dy + dz*dz); if (dm < 0.05) dm = 0.05;   /* calib_solve's clamp */
        dx = pos[k][0]-align_pt[0]; dy = pos[k][1]-align_pt[1]; dz = pos[k][2]-align_pt[2];
        const double da = sqrt(dx*dx + dy*dy + dz*dz);
        ra[k] = t - (dm - da) / c;
        rl[k] = 20.0 * log10(lv * dm * cfac(corr, k));
        ++nlive;
    }
    float ma = 0.f, ml = 0.f;
    if (nlive) {
        int j = 0;
        for (int k = 0; k < n; ++k) if (!flags[k]) tmp[j++] = (float)(ra[k] * 1e6);
        ma = fmedian(tmp, nlive);
        j = 0;
        for (int k = 0; k < n; ++k) if (!flags[k]) tmp[j++] = (float)rl[k];
        ml = fmedian(tmp, nlive);
    }
    int nflag = 0, first = 1;
    float amin = 0.f, amax = 0.f, lmin = 0.f, lmax = 0.f;
    for (int k = 0; k < n; ++k) {
        if (flags[k]) { arrival_us[k] = 0.f; level_db[k] = 0.f; ++nflag; continue; }
        arrival_us[k] = (float)(ra[k] * 1e6) - ma;
        level_db[k]   = (float)rl[k] - ml;
        if (!(fabsf(arrival_us[k]) <= CALIB_VERIFY_ARRIVAL_US)) flags[k] |= CALIB_VERIFY_FLAG_ARRIVAL;
        if (!(fabsf(level_db[k])   <= CALIB_VERIFY_LEVEL_DB))   flags[k] |= CALIB_VERIFY_FLAG_LEVEL;
        if (flags[k]) ++nflag;
        if (first || arrival_us[k] < amin) amin = arrival_us[k];
        if (first || arrival_us[k] > amax) amax = arrival_us[k];
        if (first || level_db[k] < lmin)   lmin = level_db[k];
        if (first || level_db[k] > lmax)   lmax = level_db[k];
        first = 0;
    }
    if (sum) {
        sum->nlive = nlive;
        sum->nflag = nflag;
        sum->arrival_spread_us = amax - amin;
        sum->level_spread_db   = lmax - lmin;
    }
    free(ra); free(rl); free(tmp);
    return nflag;
}

/* ---- the aiming sheet: --aim-sheet (calib.h) ---- */

void calib_aim_angles(const float v[3], float* bearing_deg, float* down_tilt_deg) {
    /* projected on the room basis rather than read off x/y/z, so the sheet follows the frame
     * convention from the one place that defines it */
    const double ahead = (double)v[0]*BWA_ROOM_AHEAD[0] + (double)v[1]*BWA_ROOM_AHEAD[1] + (double)v[2]*BWA_ROOM_AHEAD[2];
    const double right = (double)v[0]*BWA_ROOM_RIGHT[0] + (double)v[1]*BWA_ROOM_RIGHT[1] + (double)v[2]*BWA_ROOM_RIGHT[2];
    const double up    = (double)v[0]*BWA_ROOM_UP[0]    + (double)v[1]*BWA_ROOM_UP[1]    + (double)v[2]*BWA_ROOM_UP[2];
    const double h = sqrt(ahead * ahead + right * right);
    double b = 0.0, t = 0.0;
    if (h > 1e-6 * (fabs(up) > 1.0 ? fabs(up) : 1.0)) {   /* a vertical vector has no bearing */
        b = atan2(right, ahead) * 180.0 / M_PI;           /* clockwise from above: ahead toward right */
        if (b < 0.0) b += 360.0;
        if (b >= 360.0) b -= 360.0;
    }
    if (h > 0.0 || up != 0.0) t = atan2(-up, h) * 180.0 / M_PI;   /* + = below the horizontal */
    if (bearing_deg)   *bearing_deg   = (float)b;
    if (down_tilt_deg) *down_tilt_deg = (float)t;
}

int calib_aim_row(const Layout* L, int s, CalibAimRow* out) {
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!L || s < 0 || (uint32_t)s >= L->count) return 0;
    /* the sheet is what the installer mounts the box TO, so it reads the PLAN when the file carries
     * one: after a survey, `position`/`aim` are what was measured, and a sheet built from them would
     * tell you to aim the box where it already points */
    const float* ppos = layout_plan_pos(L, (uint32_t)s);
    const float* paim = layout_plan_aim(L, (uint32_t)s);
    memcpy(out->pos, ppos, sizeof out->pos);
    memcpy(out->target, L->ref, sizeof out->target);
    const double dx = L->ref[0]-ppos[0], dy = L->ref[1]-ppos[1], dz = L->ref[2]-ppos[2];
    out->dist_m = (float)sqrt(dx*dx + dy*dy + dz*dz);
    unit_dir(ppos, L->ref, out->aim);                     /* the loader's own default-aim rule */
    calib_aim_angles(out->aim, &out->bearing_deg, &out->down_tilt_deg);
    memcpy(out->layout_aim, paim, sizeof out->layout_aim);
    calib_aim_angles(out->layout_aim, &out->layout_bearing_deg, &out->layout_down_tilt_deg);
    out->off_deg = aim_angle_deg(out->layout_aim, out->aim);
    if (L->dir.nband) {
        out->have_loss   = 1;
        out->loss_2k_db  = directivity_loss_db_at(&L->dir, out->off_deg, 2000.f);
        out->loss_16k_db = directivity_loss_db_at(&L->dir, out->off_deg, 16000.f);
    }
    return 1;
}

int calib_layout_declared(const char* path, int n, int* has_listening_point, unsigned char* aim_explicit) {
    if (has_listening_point) *has_listening_point = 0;
    if (!path || n <= 0) return 0;
    char* text = read_file(path, NULL);
    if (!text) return 0;
    cJSON* root = cJSON_Parse(text);
    int ok = 0;
    if (root) {
        cJSON* spk = cJSON_GetObjectItemCaseSensitive(root, "speakers");
        if (cJSON_IsArray(spk) && cJSON_GetArraySize(spk) == n) {
            if (has_listening_point)
                *has_listening_point = cJSON_GetObjectItemCaseSensitive(root, "listening_point_m") != NULL;
            if (aim_explicit)
                for (int i = 0; i < n; ++i)
                {
                    /* with a plan, the aim the sheet reports is plan_aim, so "explicit" means that field */
                    cJSON* rec = spk_by_index(spk, i);
                    const int planned = cJSON_GetObjectItemCaseSensitive(rec, "plan_position") != NULL;
                    aim_explicit[i] = cJSON_GetObjectItemCaseSensitive(rec, planned ? "plan_aim" : "aim") != NULL;
                }
            ok = 1;
        }
        cJSON_Delete(root);
    }
    free(text);
    return ok;
}

int calib_write_aim_sheet(const char* csv_path, const Layout* L, const unsigned char* aim_explicit,
                          float flag_deg, int* nflag, char* err, size_t errcap) {
    if (nflag) *nflag = 0;
    if (!csv_path || !L || L->count == 0) {
        if (err && errcap) snprintf(err, errcap, "calib: aim sheet needs a path and a loaded layout");
        return 0;
    }
    FILE* f = os_fopen(csv_path, "wb");
    if (!f) { if (err && errcap) snprintf(err, errcap, "calib: cannot open %s for writing", csv_path); return 0; }
    fprintf(f, "speaker,x_m,y_m,z_m,target_x_m,target_y_m,target_z_m,distance_m,"
               "aim_x,aim_y,aim_z,bearing_deg,down_tilt_deg,"
               "layout_aim_x,layout_aim_y,layout_aim_z,layout_aim_source,layout_bearing_deg,layout_down_tilt_deg,"
               "layout_aim_off_deg,loss_2k_db,loss_16k_db,flag\n");
    int nf = 0;
    for (uint32_t s = 0; s < L->count; ++s) {
        CalibAimRow r;
        calib_aim_row(L, (int)s, &r);
        const int flag = !(r.off_deg <= flag_deg);
        nf += flag;
        char loss[64] = ",";                              /* no model: two empty cells */
        if (r.have_loss) snprintf(loss, sizeof loss, "%.1f,%.1f", r.loss_2k_db, r.loss_16k_db);
        fprintf(f, "%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.4f,%.4f,%.4f,%.1f,%.1f,%.4f,%.4f,%.4f,%s,%.1f,%.1f,%.1f,%s,%s\n",
                s, r.pos[0], r.pos[1], r.pos[2], r.target[0], r.target[1], r.target[2], r.dist_m,
                r.aim[0], r.aim[1], r.aim[2], r.bearing_deg, r.down_tilt_deg,
                r.layout_aim[0], r.layout_aim[1], r.layout_aim[2],
                (aim_explicit && aim_explicit[s]) ? "explicit" : "default",
                r.layout_bearing_deg, r.layout_down_tilt_deg, r.off_deg, loss, flag ? "OFF_AIM" : "");
    }
    const int bad = ferror(f);
    fclose(f);
    if (bad) { if (err && errcap) snprintf(err, errcap, "calib: write error on %s", csv_path); return 0; }
    if (nflag) *nflag = nf;
    return 1;
}
