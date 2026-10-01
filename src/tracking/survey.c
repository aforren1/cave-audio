/* survey.c - see survey.h. Pure math: no allocation, no I/O, double precision inside. */
#include "tracking/survey.h"

#include <math.h>
#include <string.h>

#define SURVEY_FIT_MAX 64               /* matched points one frame fit takes: BWA_MAX_CHANNELS */
#define RAD2DEG (180.0 / 3.14159265358979323846)

/* ---- small linear algebra ---------------------------------------------------------------------- */

/* Cyclic Jacobi eigen-decomposition of a symmetric n x n matrix (n <= 4), in place. On return
 * d[] holds the eigenvalues and the COLUMNS of v the unit eigenvectors, sorted ascending. */
static void jacobi_sym(double a[4][4], int n, double d[4], double v[4][4]) {
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) v[i][j] = (i == j) ? 1.0 : 0.0;
    double scale = 0.0;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) scale += a[i][j] * a[i][j];
    for (int sweep = 0; sweep < 64; ++sweep) {
        double off = 0.0;
        for (int p = 0; p < n; ++p)
            for (int q = p + 1; q < n; ++q) off += a[p][q] * a[p][q];
        if (off <= 1e-30 * scale || off == 0.0) break;
        for (int p = 0; p < n; ++p)
            for (int q = p + 1; q < n; ++q) {
                if (a[p][q] == 0.0) continue;
                double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                double t = (theta >= 0.0 ? 1.0 : -1.0) / (fabs(theta) + sqrt(theta * theta + 1.0));
                double c = 1.0 / sqrt(t * t + 1.0), s = t * c;
                for (int k = 0; k < n; ++k) {              /* A <- A J */
                    double akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - s * akq;
                    a[k][q] = s * akp + c * akq;
                }
                for (int k = 0; k < n; ++k) {              /* A <- J^T A */
                    double apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk;
                    a[q][k] = s * apk + c * aqk;
                }
                for (int k = 0; k < n; ++k) {              /* V <- V J */
                    double vkp = v[k][p], vkq = v[k][q];
                    v[k][p] = c * vkp - s * vkq;
                    v[k][q] = s * vkp + c * vkq;
                }
            }
    }
    for (int i = 0; i < n; ++i) d[i] = a[i][i];
    for (int i = 0; i < n; ++i)                            /* selection sort, ascending, columns follow */
        for (int j = i + 1; j < n; ++j)
            if (d[j] < d[i]) {
                double td = d[i]; d[i] = d[j]; d[j] = td;
                for (int k = 0; k < n; ++k) { double tv = v[k][i]; v[k][i] = v[k][j]; v[k][j] = tv; }
            }
}

/* Centroid and the scatter eigen-decomposition of n points: l[0] <= l[1] <= l[2] are the scatter
 * eigenvalues (sums, not means), e[.][k] the matching unit eigenvector. */
static void scatter_eigen(const double (*p)[3], int n, double c[3], double l[3], double e[3][3]) {
    c[0] = c[1] = c[2] = 0.0;
    for (int i = 0; i < n; ++i) for (int k = 0; k < 3; ++k) c[k] += p[i][k];
    if (n > 0) for (int k = 0; k < 3; ++k) c[k] /= n;
    double a[4][4] = { { 0 } }, d[4], v[4][4];
    for (int i = 0; i < n; ++i) {
        double r[3] = { p[i][0] - c[0], p[i][1] - c[1], p[i][2] - c[2] };
        for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) a[j][k] += r[j] * r[k];
    }
    jacobi_sym(a, 3, d, v);
    for (int k = 0; k < 3; ++k) {
        l[k] = d[k] > 0.0 ? d[k] : 0.0;                    /* round-off can leave -1e-20 */
        for (int j = 0; j < 3; ++j) e[j][k] = v[j][k];
    }
}

static void quat_to_mat_d(const double q[4], double R[3][3]) {   /* xyzw, unit */
    double x = q[0], y = q[1], z = q[2], w = q[3];
    R[0][0] = 1 - 2 * (y * y + z * z); R[0][1] = 2 * (x * y - z * w);     R[0][2] = 2 * (x * z + y * w);
    R[1][0] = 2 * (x * y + z * w);     R[1][1] = 1 - 2 * (x * x + z * z); R[1][2] = 2 * (y * z - x * w);
    R[2][0] = 2 * (x * z - y * w);     R[2][1] = 2 * (y * z + x * w);     R[2][2] = 1 - 2 * (x * x + y * y);
}

static void mat_vec(const double R[3][3], const double v[3], double o[3]) {
    for (int i = 0; i < 3; ++i) o[i] = R[i][0] * v[0] + R[i][1] * v[1] + R[i][2] * v[2];
}

static bool finite3(const float v[3]) { return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]); }

/* normalize xyzw into double; false (and identity) for a zero or non-finite quaternion */
static bool quat_norm_d(const float q[4], double o[4]) {
    double n = 0.0;
    for (int k = 0; k < 4; ++k) { if (!isfinite(q[k])) goto bad; n += (double)q[k] * q[k]; }
    if (!(n > 1e-12)) goto bad;
    n = 1.0 / sqrt(n);
    for (int k = 0; k < 4; ++k) o[k] = q[k] * n;
    return true;
bad:
    o[0] = o[1] = o[2] = 0.0; o[3] = 1.0;
    return false;
}

void survey_quat_to_mat(const float q[4], float R[3][3]) {
    double qd[4], Rd[3][3];
    quat_norm_d(q, qd);
    quat_to_mat_d(qd, Rd);
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) R[i][j] = (float)Rd[i][j];
}

static double angle_deg_d(const double a[3], const double b[3]) {
    double na = sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    double nb = sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    if (!(na > 0.0) || !(nb > 0.0)) return 0.0;
    /* atan2 of |a x b| and a.b: accurate at 0 and 180, where acos loses half its digits */
    double cx = a[1] * b[2] - a[2] * b[1], cy = a[2] * b[0] - a[0] * b[2], cz = a[0] * b[1] - a[1] * b[0];
    return atan2(sqrt(cx * cx + cy * cy + cz * cz), a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) * RAD2DEG;
}

float survey_angle_deg(const float a[3], const float b[3]) {
    double ad[3] = { a[0], a[1], a[2] }, bd[3] = { b[0], b[1], b[2] };
    return (float)angle_deg_d(ad, bd);
}

/* ---- one speaker --------------------------------------------------------------------------------- */

const char* survey_aim_status_str(int s) {
    switch (s) {
    case SURVEY_AIM_OK:          return "baffle plane";
    case SURVEY_AIM_AXIS:        return "configured axis";
    case SURVEY_AIM_FEW_MARKERS: return "fewer than 3 markers";
    case SURVEY_AIM_COLINEAR:    return "markers colinear";
    case SURVEY_AIM_NOT_FLAT:    return "markers not flat";
    case SURVEY_AIM_BAD_AXIS:    return "bad axis";
    default:                     return "?";
    }
}

void survey_speaker(const float body_pos[3], const float body_q[4], const float (*markers)[3], int n,
                    const float ref[3], const SurveyOpts* o, SurveySpeaker* out) {
    static const SurveyOpts defaults = { 0 };
    if (!o) o = &defaults;
    memset(out, 0, sizeof *out);
    if (n < 0 || !markers) n = 0;
    out->n_markers = n;

    double q[4], R[3][3];
    quat_norm_d(body_q, q);
    quat_to_mat_d(q, R);

    /* local marker cloud: centroid + plane (smallest-scatter direction) */
    double pts[64][3];
    const int np = n < 64 ? n : 64;
    for (int i = 0; i < np; ++i) for (int k = 0; k < 3; ++k) pts[i][k] = markers[i][k];
    double cl[3] = { 0, 0, 0 }, l[3] = { 0, 0, 0 }, e[3][3] = { { 0 } };
    if (np > 0) scatter_eigen((const double (*)[3])pts, np, cl, l, e);
    if (np >= 3) {
        out->plane_rms_m = (float)sqrt(l[0] / np);
        out->width_m     = (float)sqrt(l[1] / np);
        out->aspect      = l[2] > 0.0 ? (float)sqrt(l[1] / l[2]) : 0.f;
    }

    double cw[3];                                          /* the centroid on the baffle, room frame */
    mat_vec(R, cl, cw);
    for (int k = 0; k < 3; ++k) { cw[k] += body_pos[k]; out->centroid[k] = (float)cw[k]; }
    const double toref[3] = { ref[0] - cw[0], ref[1] - cw[1], ref[2] - cw[2] };

    double aim[3] = { 0, 0, 0 };
    if (o->use_axis) {
        double ax[3] = { o->axis_local[0], o->axis_local[1], o->axis_local[2] };
        double len = sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
        if (!finite3(o->axis_local) || !(len > 1e-9)) {
            out->aim_status = SURVEY_AIM_BAD_AXIS;
        } else {
            for (int k = 0; k < 3; ++k) ax[k] /= len;
            mat_vec(R, ax, aim);
            out->aim_status = SURVEY_AIM_AXIS;
            out->aim_ok = true;
            out->aim_away = aim[0] * toref[0] + aim[1] * toref[1] + aim[2] * toref[2] < 0.0;
        }
    } else {
        const float max_rms = o->max_plane_rms_m > 0.f ? o->max_plane_rms_m : SURVEY_PLANE_MAX_RMS_M;
        if (np < 3)                                                     out->aim_status = SURVEY_AIM_FEW_MARKERS;
        else if (out->width_m < SURVEY_MIN_WIDTH_M || out->aspect < SURVEY_MIN_ASPECT) out->aim_status = SURVEY_AIM_COLINEAR;
        else if (out->plane_rms_m > max_rms)                            out->aim_status = SURVEY_AIM_NOT_FLAT;
        else {
            const double nl[3] = { e[0][0], e[1][0], e[2][0] };        /* smallest-scatter axis = baffle normal */
            mat_vec(R, nl, aim);
            /* the plane has two normals; the baffle faces the room, so take the one toward the
             * listening point. This is the step that makes Motive's orientation convention moot. */
            if (aim[0] * toref[0] + aim[1] * toref[1] + aim[2] * toref[2] < 0.0)
                for (int k = 0; k < 3; ++k) aim[k] = -aim[k];
            out->aim_status = SURVEY_AIM_OK;
            out->aim_ok = true;
        }
    }
    if (out->aim_ok) {
        double len = sqrt(aim[0] * aim[0] + aim[1] * aim[1] + aim[2] * aim[2]);
        for (int k = 0; k < 3; ++k) out->aim[k] = (float)(aim[k] / len);
    }
    /* the layout's point sits behind the baffle; without a trusted normal there is no "behind" */
    for (int k = 0; k < 3; ++k)
        out->pos[k] = out->aim_ok ? (float)(cw[k] - o->baffle_offset_m * out->aim[k]) : (float)cw[k];
}

/* ---- averaging ------------------------------------------------------------------------------------ */

void survey_avg_reset(SurveyAvg* a) { memset(a, 0, sizeof *a); }

bool survey_avg_add(SurveyAvg* a, const float p[3], const float q[4]) {
    double qn[4];
    if (!finite3(p) || !quat_norm_d(q, qn)) return false;
    if (a->n == 0) for (int k = 0; k < 3; ++k) a->p0[k] = p[k];
    double d2 = 0.0;
    for (int k = 0; k < 3; ++k) { double d = p[k] - a->p0[k]; a->sp[k] += d; d2 += d * d; }
    a->spp += d2;
    /* q and -q are the same rotation: align each sample with the running sum before adding */
    double dot = a->sq[0] * qn[0] + a->sq[1] * qn[1] + a->sq[2] * qn[2] + a->sq[3] * qn[3];
    for (int k = 0; k < 4; ++k) a->sq[k] += dot < 0.0 ? -qn[k] : qn[k];
    ++a->n;
    return true;
}

bool survey_avg_result(const SurveyAvg* a, float p[3], float q[4], float* spread_m, float* spread_deg) {
    if (a->n <= 0) return false;
    double m[3], mm = 0.0;
    for (int k = 0; k < 3; ++k) { m[k] = a->sp[k] / a->n; mm += m[k] * m[k]; p[k] = (float)(a->p0[k] + m[k]); }
    double var = a->spp / a->n - mm;
    if (spread_m) *spread_m = (float)sqrt(var > 0.0 ? var : 0.0);
    double qn = sqrt(a->sq[0] * a->sq[0] + a->sq[1] * a->sq[1] + a->sq[2] * a->sq[2] + a->sq[3] * a->sq[3]);
    for (int k = 0; k < 4; ++k) q[k] = (float)(a->sq[k] / qn);
    /* |mean of unit quats| = mean cos(delta_i / 2), delta_i each sample's angle from the mean */
    double c = qn / a->n;
    if (c > 1.0) c = 1.0;
    if (spread_deg) *spread_deg = (float)(2.0 * acos(c) * RAD2DEG);
    return true;
}

/* ---- frame agreement ------------------------------------------------------------------------------ */

/* Horn (1987) closed-form absolute orientation: the PROPER rotation R and t minimizing
 * sum |R a_i + t - b_i|^2. qo = that rotation as xyzw with w >= 0. */
static bool horn_fit(const double (*a)[3], const double (*b)[3], int n, double R[3][3], double t[3], double qo[4]) {
    double ca[3] = { 0, 0, 0 }, cb[3] = { 0, 0, 0 };
    for (int i = 0; i < n; ++i) for (int k = 0; k < 3; ++k) { ca[k] += a[i][k]; cb[k] += b[i][k]; }
    for (int k = 0; k < 3; ++k) { ca[k] /= n; cb[k] /= n; }
    double S[3][3] = { { 0 } };                            /* S[j][k] = sum a'_j b'_k */
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) S[j][k] += (a[i][j] - ca[j]) * (b[i][k] - cb[k]);
    const double Sxx = S[0][0], Sxy = S[0][1], Sxz = S[0][2];
    const double Syx = S[1][0], Syy = S[1][1], Syz = S[1][2];
    const double Szx = S[2][0], Szy = S[2][1], Szz = S[2][2];
    double N[4][4] = {                                     /* quaternion order (w, x, y, z) */
        { Sxx + Syy + Szz, Syz - Szy,        Szx - Sxz,        Sxy - Syx        },
        { Syz - Szy,       Sxx - Syy - Szz,  Sxy + Syx,        Szx + Sxz        },
        { Szx - Sxz,       Sxy + Syx,       -Sxx + Syy - Szz,  Syz + Szy        },
        { Sxy - Syx,       Szx + Sxz,        Syz + Szy,       -Sxx - Syy + Szz  } };
    double d[4], v[4][4];
    jacobi_sym(N, 4, d, v);
    double w = v[0][3], x = v[1][3], y = v[2][3], z = v[3][3];   /* the largest eigenvalue's vector */
    double len = sqrt(w * w + x * x + y * y + z * z);
    if (!(len > 0.0) || !isfinite(len)) return false;
    if (w < 0.0) len = -len;
    qo[0] = x / len; qo[1] = y / len; qo[2] = z / len; qo[3] = w / len;
    quat_to_mat_d(qo, R);
    double Rc[3];
    mat_vec(R, ca, Rc);
    for (int k = 0; k < 3; ++k) t[k] = cb[k] - Rc[k];
    return true;
}

static double fit_rms(const double (*a)[3], const double (*b)[3], int n, const double R[3][3],
                      const double t[3], double* maxd, int* maxi) {
    double s = 0.0;
    if (maxd) *maxd = 0.0;
    if (maxi) *maxi = 0;
    for (int i = 0; i < n; ++i) {
        double r[3];
        mat_vec(R, a[i], r);
        double d2 = 0.0;
        for (int k = 0; k < 3; ++k) { double e = r[k] + t[k] - b[i][k]; d2 += e * e; }
        s += d2;
        if (maxd && sqrt(d2) > *maxd) { *maxd = sqrt(d2); if (maxi) *maxi = i; }
    }
    return sqrt(s / n);
}

/* mean angle between R * (mirror ? S : I) * opt_aim and lay_aim; -1 with no usable aim */
static double aim_disagreement(const float (*oa)[3], const float (*la)[3], const bool* ok, int n,
                               const double R[3][3], bool mirror) {
    double s = 0.0;
    int m = 0;
    for (int i = 0; i < n; ++i) {
        if (!ok[i] || !finite3(oa[i]) || !finite3(la[i])) continue;
        double v[3] = { mirror ? -oa[i][0] : oa[i][0], oa[i][1], oa[i][2] }, r[3];
        double l[3] = { la[i][0], la[i][1], la[i][2] };
        mat_vec(R, v, r);
        s += angle_deg_d(r, l);
        ++m;
    }
    return m ? s / m : -1.0;
}

bool survey_fit_frame(const float (*opt)[3], const float (*lay)[3],
                      const float (*opt_aim)[3], const float (*lay_aim)[3], const bool* aim_ok,
                      int n, SurveyFrameFit* out) {
    memset(out, 0, sizeof *out);
    out->aim_deg = out->aim_mirror_deg = -1.f;
    out->axis[1] = 1.f;
    if (n < 3 || n > SURVEY_FIT_MAX) return false;
    double a[SURVEY_FIT_MAX][3], am[SURVEY_FIT_MAX][3], b[SURVEY_FIT_MAX][3];   /* 4.6 KB of stack */
    for (int i = 0; i < n; ++i) {
        if (!finite3(opt[i]) || !finite3(lay[i])) return false;
        for (int k = 0; k < 3; ++k) { a[i][k] = opt[i][k]; b[i][k] = lay[i][k]; am[i][k] = opt[i][k]; }
        am[i][0] = -am[i][0];
    }
    double c[3], l[3], e[3][3];
    scatter_eigen((const double (*)[3])b, n, c, l, e);
    if (sqrt(l[1] / n) < 0.01) return false;               /* colinear (or coincident) layout points:
                                                            * the rotation about their line is free */
    out->n = n;
    out->thickness_m = (float)sqrt(l[0] / n);

    double R[3][3], t[3], q[4], Rm[3][3], tm[3], qm[4], maxd;
    int maxi;
    if (!horn_fit((const double (*)[3])a, (const double (*)[3])b, n, R, t, q)) return false;
    if (!horn_fit((const double (*)[3])am, (const double (*)[3])b, n, Rm, tm, qm)) return false;
    const double rms  = fit_rms((const double (*)[3])a,  (const double (*)[3])b, n, R,  t,  &maxd, &maxi);
    const double rmsm = fit_rms((const double (*)[3])am, (const double (*)[3])b, n, Rm, tm, NULL, NULL);
    for (int i = 0; i < 3; ++i) { out->t[i] = (float)t[i]; for (int j = 0; j < 3; ++j) out->R[i][j] = (float)R[i][j]; }
    out->rms_m = (float)rms; out->max_m = (float)maxd; out->max_i = maxi;
    out->rms_mirror_m = (float)rmsm;

    const double sh = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
    out->angle_deg = (float)(2.0 * atan2(sh, q[3]) * RAD2DEG);
    if (sh > 1e-12) for (int k = 0; k < 3; ++k) out->axis[k] = (float)(q[k] / sh);

    if (opt_aim && lay_aim && aim_ok) {
        out->aim_deg        = (float)aim_disagreement(opt_aim, lay_aim, aim_ok, n, R,  false);
        out->aim_mirror_deg = (float)aim_disagreement(opt_aim, lay_aim, aim_ok, n, Rm, true);
    }

    /* Handedness. Positions decide it when the matched set leaves a plane, which shows as one fit
     * being clearly worse (a factor of 2 plus 1 cm, so survey noise on a near-flat set cannot
     * decide it). Otherwise aims decide it the same way (2x plus 10 deg, because a default aim is
     * only good to about 10 deg). Neither: say so. */
    if (rmsm > 2.0 * rms + 0.01)      { out->hand = SURVEY_HAND_OK;       out->hand_by = SURVEY_HAND_BY_POSITIONS; }
    else if (rms > 2.0 * rmsm + 0.01) { out->hand = SURVEY_HAND_MIRRORED; out->hand_by = SURVEY_HAND_BY_POSITIONS; }
    else if (out->aim_deg >= 0.f) {
        if (out->aim_mirror_deg > 2.f * out->aim_deg + 10.f)      { out->hand = SURVEY_HAND_OK;       out->hand_by = SURVEY_HAND_BY_AIMS; }
        else if (out->aim_deg > 2.f * out->aim_mirror_deg + 10.f) { out->hand = SURVEY_HAND_MIRRORED; out->hand_by = SURVEY_HAND_BY_AIMS; }
    }
    return true;
}

bool survey_fit_depth(const float (*opt)[3], const float (*aim)[3], const float (*lay)[3], int n,
                      SurveyDepthFit* out, float* depth_i, float* across_i) {
    memset(out, 0, sizeof *out);
    if (n < 3 || n > SURVEY_FIT_MAX) return false;
    double o[SURVEY_FIT_MAX][3], u[SURVEY_FIT_MAX][3], p[SURVEY_FIT_MAX][3], a[SURVEY_FIT_MAX][3];   /* 6 KB of stack */
    for (int i = 0; i < n; ++i) {
        if (!finite3(opt[i]) || !finite3(aim[i]) || !finite3(lay[i])) return false;
        for (int k = 0; k < 3; ++k) { o[i][k] = opt[i][k]; u[i][k] = aim[i][k]; p[i][k] = lay[i][k]; }
    }
    double c[3], l[3], e[3][3];
    scatter_eigen((const double (*)[3])p, n, c, l, e);
    if (sqrt(l[1] / n) < 0.01) return false;               /* colinear: the rotation about the line is free */

    double R[3][3], t[3], q[4], d = 0.0, lev2 = 0.0;
    int it;
    for (it = 1; it <= 500; ++it) {
        for (int i = 0; i < n; ++i) for (int k = 0; k < 3; ++k) a[i][k] = o[i][k] - d * u[i][k];
        if (!horn_fit((const double (*)[3])a, (const double (*)[3])p, n, R, t, q)) return false;
        /* rotation fixed: t - d b_i = p_i - R o_i =: v_i, so t = mean v + d mean b and d is the slope of
         * (mean v - v_i) on (b_i - mean b) */
        double b[SURVEY_FIT_MAX][3], v[SURVEY_FIT_MAX][3], bm[3] = { 0, 0, 0 }, vm[3] = { 0, 0, 0 };
        for (int i = 0; i < n; ++i) {
            double ro[3];
            mat_vec(R, u[i], b[i]);
            mat_vec(R, o[i], ro);
            for (int k = 0; k < 3; ++k) { v[i][k] = p[i][k] - ro[k]; bm[k] += b[i][k] / n; vm[k] += v[i][k] / n; }
        }
        double num = 0.0, den = 0.0;
        for (int i = 0; i < n; ++i)
            for (int k = 0; k < 3; ++k) {
                const double db = b[i][k] - bm[k];
                num += db * (vm[k] - v[i][k]);
                den += db * db;
            }
        if (!(den > 1e-6)) return false;                    /* every aim alike: depth = translation */
        const double dn = num / den;
        for (int k = 0; k < 3; ++k) t[k] = vm[k] + dn * bm[k];
        lev2 = den;
        const double step = fabs(dn - d);
        d = dn;
        if (step < 1e-9) break;
    }
    double s2 = 0.0;
    for (int i = 0; i < n; ++i) {
        double ro[3], b[3], diff[3];
        mat_vec(R, o[i], ro);
        mat_vec(R, u[i], b);
        double di = 0.0, ac = 0.0;
        for (int k = 0; k < 3; ++k) { diff[k] = ro[k] + t[k] - p[i][k]; di += diff[k] * b[k]; }
        for (int k = 0; k < 3; ++k) { const double x = diff[k] - di * b[k], r = diff[k] - d * b[k]; ac += x * x; s2 += r * r; }
        if (depth_i)  depth_i[i] = (float)di;
        if (across_i) across_i[i] = (float)sqrt(ac);
    }
    out->n = n;
    for (int i = 0; i < 3; ++i) { out->t[i] = (float)t[i]; for (int j = 0; j < 3; ++j) out->R[i][j] = (float)R[i][j]; }
    const double sh = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
    out->angle_deg = (float)(2.0 * atan2(sh, q[3]) * RAD2DEG);
    out->t_m = (float)sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
    out->depth_m = (float)d;
    out->leverage = (float)sqrt(lev2);
    out->rms_m = (float)sqrt(s2 / n);
    out->iters = it > 500 ? 500 : it;
    return true;
}

float survey_pair_delta(const float o0[3], const float o1[3], const float l0[3], const float l1[3]) {
    double dO = 0.0, dL = 0.0;
    for (int k = 0; k < 3; ++k) {
        double a = (double)o1[k] - o0[k], b = (double)l1[k] - l0[k];
        dO += a * a; dL += b * b;
    }
    return (float)(sqrt(dO) - sqrt(dL));
}

/* ---- names ---------------------------------------------------------------------------------------- */

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }

static bool ci_prefix(const char* s, const char* pre) {   /* pre is lowercase */
    for (; *pre; ++s, ++pre) if (lower((unsigned char)*s) != *pre) return false;
    return true;
}

static bool ci_equal(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) if (lower((unsigned char)*a) != lower((unsigned char)*b)) return false;
    return *a == *b;
}

int survey_name_index(const char* s) {
    if (!s) return -1;
    size_t k;
    if (ci_prefix(s, "speaker"))  k = 7;                 /* before "spk": the two share "sp" only */
    else if (ci_prefix(s, "spk")) k = 3;
    else return -1;
    if (s[k] == '_' || s[k] == '-' || s[k] == '.' || s[k] == ' ') ++k;
    if (s[k] < '0' || s[k] > '9') return -1;
    long v = 0;
    for (; s[k] >= '0' && s[k] <= '9'; ++k) {
        v = v * 10 + (s[k] - '0');
        if (v > 1000000) return -1;                      /* absurd, and it keeps v inside an int */
    }
    return s[k] == 0 ? (int)v : -1;
}

void survey_match_names(const char* const* names, int n, const SurveyMapEntry* map, int nmap,
                        int count, SurveyMatch* out, bool* map_used) {
    for (int k = 0; k < nmap; ++k) if (map_used) map_used[k] = false;
    for (int i = 0; i < n; ++i) { out[i].index = -1; out[i].status = SURVEY_MATCH_NO_RULE; out[i].by_map = false; out[i].other = -1; }

    /* two passes so an explicit --map entry beats the default rule whatever the wire order */
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < n; ++i) {
            if (!names[i]) continue;
            int want = -1;
            if (pass == 0) {
                for (int k = 0; k < nmap; ++k)
                    if (map[k].name && ci_equal(names[i], map[k].name)) {
                        if (map_used) map_used[k] = true;
                        if (!out[i].by_map) { want = map[k].index; out[i].by_map = true; }
                    }
                if (!out[i].by_map) continue;
            } else {
                if (out[i].by_map) continue;
                want = survey_name_index(names[i]);
                if (want < 0) continue;                  /* stays NO_RULE */
            }
            out[i].index = want;
            if (want < 0 || want >= count) { out[i].status = SURVEY_MATCH_OUT_OF_RANGE; continue; }
            int holder = -1;
            for (int j = 0; j < n; ++j)
                if (j != i && out[j].status == SURVEY_MATCH_OK && out[j].index == want) { holder = j; break; }
            if (holder >= 0) { out[i].status = SURVEY_MATCH_DUPLICATE; out[i].other = holder; }
            else               out[i].status = SURVEY_MATCH_OK;
        }
}
