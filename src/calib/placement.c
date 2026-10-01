/*
 * placement.c - the tracked ZM-1's placement math (placement.h). Pure: no clock, no tracker, no I/O.
 */
#include "calib/placement.h"
#include "core/frame.h"     /* BWA_ROOM_RIGHT / UP / AHEAD: the words follow the one frame definition */
#include "core/sane.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* a quaternion in, a unit quaternion out; refuse anything a tracker would not send */
static int unit_q(const float q[4], double u[4]) {
    if (!q) return 0;
    for (int i = 0; i < 4; ++i) if (!isfinite(q[i])) return 0;
    double n = sqrt((double)q[0]*q[0] + (double)q[1]*q[1] + (double)q[2]*q[2] + (double)q[3]*q[3]);
    if (!(n >= 0.5 && n <= 2.0)) return 0;
    for (int i = 0; i < 4; ++i) u[i] = q[i] / n;
    return 1;
}

/* v_room = R(q) . v_body, ROW-major, the same matrix zylia_quat_to_matrix builds */
static void rot(const double u[4], const double v[3], double out[3]) {
    const double x = u[0], y = u[1], z = u[2], w = u[3];
    const double R[9] = {
        1.0 - 2.0*(y*y + z*z), 2.0*(x*y - w*z),       2.0*(x*z + w*y),
        2.0*(x*y + w*z),       1.0 - 2.0*(x*x + z*z), 2.0*(y*z - w*x),
        2.0*(x*z - w*y),       2.0*(y*z + w*x),       1.0 - 2.0*(x*x + y*y) };
    for (int r = 0; r < 3; ++r) out[r] = R[3*r]*v[0] + R[3*r+1]*v[1] + R[3*r+2]*v[2];
}

int place_center(const float p[3], const float q[4], const float offset[3], float out[3]) {
    double u[4];
    if (!p || !out || !bwa_finite3_bounded(p, PLACE_MAX_COORD_M) || !unit_q(q, u)) return 0;
    if (offset && !bwa_finite3_bounded(offset, PLACE_MAX_OFFSET_M)) return 0;
    double o[3] = { offset ? offset[0] : 0.0, offset ? offset[1] : 0.0, offset ? offset[2] : 0.0 }, r[3];
    rot(u, o, r);
    for (int a = 0; a < 3; ++a) out[a] = (float)((double)p[a] + r[a]);
    return 1;
}

int place_mount_angles(const float q[4], float* yaw_deg, float* tilt_deg) {
    double u[4];
    if (!unit_q(q, u)) return 0;
    const double ez[3] = { 0.0, 0.0, 1.0 }, ey[3] = { 0.0, 1.0, 0.0 };
    double z[3], y[3];
    rot(u, ez, z);
    rot(u, ey, y);
    const double RAD = 57.29577951308232;
    /* clockwise from above with +y up: room-right is -x, so the bearing grows toward -x */
    if (yaw_deg)  *yaw_deg  = (float)(atan2(-z[0], z[2]) * RAD);
    if (tilt_deg) *tilt_deg = (float)(acos(y[1] > 1.0 ? 1.0 : (y[1] < -1.0 ? -1.0 : y[1])) * RAD);
    return 1;
}

static const double RAD2DEG = 57.29577951308232;

/* the angle of a^-1 b from two UNIT quaternions, degrees. The vector part's length against |w| through
 * atan2 keeps a hundredth of a degree exact, where acos of a dot product near 1 would round it away;
 * |w| is the sign-safety (q and -q). */
static double turn_u(const double a[4], const double b[4]) {
    /* r = conj(a) * b, xyzw */
    const double ax = -a[0], ay = -a[1], az = -a[2], aw = a[3];
    const double rx = aw*b[0] + ax*b[3] + ay*b[2] - az*b[1];
    const double ry = aw*b[1] - ax*b[2] + ay*b[3] + az*b[0];
    const double rz = aw*b[2] + ax*b[1] - ay*b[0] + az*b[3];
    const double rw = aw*b[3] - ax*b[0] - ay*b[1] - az*b[2];
    return 2.0 * atan2(sqrt(rx*rx + ry*ry + rz*rz), fabs(rw)) * RAD2DEG;
}

int place_turn_deg(const float a[4], const float b[4], float* deg) {
    double ua[4], ub[4];
    if (!unit_q(a, ua) || !unit_q(b, ub)) return 0;
    if (deg) *deg = (float)turn_u(ua, ub);
    return 1;
}

float place_turn_limit_deg(float tol_m, float range_m) {
    if (!(tol_m > 0.f && tol_m <= PLACE_TOL_MAX_M) || !(range_m > 0.f && range_m <= PLACE_MAX_COORD_M))
        return PLACE_TURN_MIN_DEG;
    double d = atan(0.5 * (double)tol_m / (double)range_m) * RAD2DEG;
    if (d < PLACE_TURN_MIN_DEG) d = PLACE_TURN_MIN_DEG;
    if (d > PLACE_TURN_MAX_DEG) d = PLACE_TURN_MAX_DEG;
    return (float)d;
}

void place_cfg_direction(PlaceCfg* c, float turn_limit_deg) {
    if (!c) return;
    float s = isfinite(turn_limit_deg) ? 0.5f * turn_limit_deg : 0.f;
    if (s < PLACE_TURN_MIN_DEG) s = PLACE_TURN_MIN_DEG;
    if (s > PLACE_TURN_MAX_DEG) s = PLACE_TURN_MAX_DEG;
    c->still_deg = s;
}

void place_cfg_default(PlaceCfg* c, float tol_m) {
    if (!c) return;
    if (!isfinite(tol_m)) tol_m = PLACE_TOL_DEFAULT_M;
    if (tol_m < 0.f) tol_m = 0.f;
    if (tol_m > PLACE_TOL_MAX_M) tol_m = PLACE_TOL_MAX_M;
    c->tol_m = tol_m;
    float s = tol_m > 0.f ? 0.25f * tol_m : PLACE_STILL_MAX_M;
    if (s < PLACE_STILL_MIN_M) s = PLACE_STILL_MIN_M;
    if (s > PLACE_STILL_MAX_M) s = PLACE_STILL_MAX_M;
    c->still_m  = s;
    c->window_s = PLACE_WINDOW_S;
    c->hold_s   = PLACE_HOLD_S;
    c->still_deg = 0.f;
}

void place_gate_reset(PlaceGate* g) {
    if (!g) return;
    PlaceCfg cfg = g->cfg;
    memset(g, 0, sizeof *g);
    g->cfg = cfg;
    g->state = PLACE_NO_POSE;
}

void place_gate_init(PlaceGate* g, const PlaceCfg* cfg) {
    if (!g) return;
    memset(g, 0, sizeof *g);
    if (cfg) g->cfg = *cfg; else place_cfg_default(&g->cfg, PLACE_TOL_DEFAULT_M);
    /* a hand-filled config gets the same guards the default applies */
    if (!isfinite(g->cfg.tol_m) || g->cfg.tol_m < 0.f) g->cfg.tol_m = 0.f;
    if (g->cfg.tol_m > PLACE_TOL_MAX_M) g->cfg.tol_m = PLACE_TOL_MAX_M;
    if (!bwa_finite_clamp(&g->cfg.still_m, PLACE_STILL_MIN_M, PLACE_TOL_MAX_M)) g->cfg.still_m = PLACE_STILL_MAX_M;
    if (!(g->cfg.window_s > 0.0 && g->cfg.window_s <= 60.0)) g->cfg.window_s = PLACE_WINDOW_S;
    if (!(g->cfg.hold_s >= 0.0 && g->cfg.hold_s <= 600.0))   g->cfg.hold_s   = PLACE_HOLD_S;
    /* 0 = center only; anything else is a direction mode and gets the floor and the cap */
    if (!isfinite(g->cfg.still_deg) || g->cfg.still_deg < 0.f) g->cfg.still_deg = 0.f;
    else if (g->cfg.still_deg > 0.f) place_cfg_direction(&g->cfg, 2.f * g->cfg.still_deg);
    g->state = PLACE_NO_POSE;
}

static void drop(PlaceGate* g) {
    g->n = 0; g->head = 0; g->ok_run = 0; g->held_s = 0.0; g->span_s = 0.0; g->spread_m = 0.f;
    g->have_q = 0; g->turn_spread_deg = 0.f; g->turning = 0;
}

PlaceState place_gate_update(PlaceGate* g, double t, const float center[3], const float target[3]) {
    return place_gate_update_q(g, t, center, NULL, target);
}

PlaceState place_gate_update_q(PlaceGate* g, double t, const float center[3], const float q[4], const float target[3]) {
    if (!g) return PLACE_NO_POSE;
    double uq[4];
    const int qok = q && unit_q(q, uq);
    if (!isfinite(t) || !center || !target ||
        !bwa_finite3_bounded(center, PLACE_MAX_COORD_M) || !bwa_finite3_bounded(target, PLACE_MAX_COORD_M) ||
        (g->cfg.still_deg > 0.f && !qok)) {             /* a direction mode cannot judge a pose with no orientation */
        drop(g);
        g->state = PLACE_NO_POSE;
        return g->state;
    }
    if (g->have_last && t < g->last_t) drop(g);               /* a clock step is not stillness */
    g->last_t = t; g->have_last = 1;
    memcpy(g->center, center, sizeof g->center);

    /* store, unless the previous stored sample is too recent (any polling rate fits the ring) */
    const double min_dt = g->cfg.window_s / (PLACE_HIST / 2);
    int newest = (g->head + PLACE_HIST - 1) % PLACE_HIST;
    if (g->n == 0 || t - g->t[newest] >= min_dt) {
        g->t[g->head] = t;
        memcpy(g->c[g->head], center, sizeof g->c[0]);
        g->hq[g->head] = (unsigned char)qok;
        for (int a = 0; a < 4; ++a) g->q[g->head][a] = qok ? (float)uq[a] : 0.f;
        g->head = (g->head + 1) % PLACE_HIST;
        if (g->n < PLACE_HIST) ++g->n;
        newest = (g->head + PLACE_HIST - 1) % PLACE_HIST;
    }
    /* age out: keep the samples inside the window of the newest */
    while (g->n > 1) {
        const int oldest = (g->head + PLACE_HIST - g->n) % PLACE_HIST;
        if (g->t[newest] - g->t[oldest] > g->cfg.window_s) --g->n; else break;
    }
    const int oldest = (g->head + PLACE_HIST - g->n) % PLACE_HIST;
    g->span_s = g->t[newest] - g->t[oldest];

    double m[3] = { 0.0, 0.0, 0.0 };
    for (int k = 0; k < g->n; ++k) {
        const int i = (oldest + k) % PLACE_HIST;
        for (int a = 0; a < 3; ++a) m[a] += g->c[i][a];
    }
    for (int a = 0; a < 3; ++a) { m[a] /= g->n; g->mean[a] = (float)m[a]; }
    double sp2 = 0.0;
    for (int k = 0; k < g->n; ++k) {
        const int i = (oldest + k) % PLACE_HIST;
        double d2 = 0.0;
        for (int a = 0; a < 3; ++a) { const double d = g->c[i][a] - m[a]; d2 += d * d; }
        if (d2 > sp2) sp2 = d2;
    }
    g->spread_m = (float)sqrt(sp2);

    double dd = 0.0, md = 0.0;
    for (int a = 0; a < 3; ++a) {
        g->delta[a] = center[a] - target[a];
        dd += (double)g->delta[a] * g->delta[a];
        const double e = m[a] - target[a];
        md += e * e;
    }
    g->dist_m = (float)sqrt(dd);
    g->mean_dist_m = (float)sqrt(md);

    /* The orientation: the mean of the window's quaternions, each sign-aligned to the oldest first (q
     * and -q are one rotation, and a tracker may send either), then the largest turn of a sample from
     * it. The chordal mean is exact to well under a hundredth of a degree for spreads of a few degrees. */
    g->have_q = 0; g->turn_spread_deg = 0.f;
    int allq = 1;
    for (int k = 0; k < g->n && allq; ++k) allq = g->hq[(oldest + k) % PLACE_HIST];
    if (allq) {
        double s[4] = { 0.0, 0.0, 0.0, 0.0 };
        const float* r0 = g->q[oldest];
        for (int k = 0; k < g->n; ++k) {
            const float* qi = g->q[(oldest + k) % PLACE_HIST];
            const double dot = (double)qi[0]*r0[0] + (double)qi[1]*r0[1] + (double)qi[2]*r0[2] + (double)qi[3]*r0[3];
            const double sg = dot < 0.0 ? -1.0 : 1.0;
            for (int a = 0; a < 4; ++a) s[a] += sg * qi[a];
        }
        const double sn = sqrt(s[0]*s[0] + s[1]*s[1] + s[2]*s[2] + s[3]*s[3]);
        if (sn > 1e-9) {
            double um[4], ui[4], worst = 0.0;
            for (int a = 0; a < 4; ++a) { um[a] = s[a] / sn; g->qmean[a] = (float)um[a]; }
            for (int k = 0; k < g->n; ++k) {
                const float* qi = g->q[(oldest + k) % PLACE_HIST];
                for (int a = 0; a < 4; ++a) ui[a] = qi[a];
                const double d = turn_u(um, ui);
                if (d > worst) worst = d;
            }
            g->have_q = 1;
            g->turn_spread_deg = (float)worst;
        }
    }

    const int pos_still = g->n >= 3 && g->span_s >= 0.9 * g->cfg.window_s && g->spread_m <= g->cfg.still_m;
    const int turn_still = g->cfg.still_deg <= 0.f || (g->have_q && g->turn_spread_deg <= g->cfg.still_deg);
    const int still = pos_still && turn_still;
    g->turning = pos_still && !turn_still;
    const int in_tol = g->cfg.tol_m <= 0.f || g->mean_dist_m <= g->cfg.tol_m;
    if (!still)       { g->ok_run = 0; g->held_s = 0.0; g->state = PLACE_MOVING; }
    else if (!in_tol) { g->ok_run = 0; g->held_s = 0.0; g->state = PLACE_OFF_TARGET; }
    else {
        if (!g->ok_run) { g->ok_run = 1; g->ok_since = t; }
        g->held_s = t - g->ok_since;
        g->state = g->held_s >= g->cfg.hold_s ? PLACE_OK : PLACE_SETTLING;
    }
    return g->state;
}

int place_bump(const float taken[3], const float now[3], float tol_m, float* moved_m) {
    if (!taken || !now || !bwa_finite3_bounded(taken, PLACE_MAX_COORD_M) ||
        !bwa_finite3_bounded(now, PLACE_MAX_COORD_M) || !(tol_m > 0.f && tol_m <= PLACE_TOL_MAX_M))
        return -1;
    double d2 = 0.0;
    for (int a = 0; a < 3; ++a) { const double d = (double)now[a] - taken[a]; d2 += d * d; }
    const double d = sqrt(d2);
    if (moved_m) *moved_m = (float)d;
    return d > 0.5 * tol_m ? 1 : 0;
}

int place_bump_pose(const float c_taken[3], const float q_taken[4], const float c_now[3], const float q_now[4],
                    float tol_m, float turn_limit_deg, float* moved_m, float* turned_deg) {
    const int b = place_bump(c_taken, c_now, tol_m, moved_m);
    if (isnan(turn_limit_deg)) return -1;               /* a broken limit is not "center only" */
    if (turn_limit_deg <= 0.f) return b;                /* center only */
    if (turn_limit_deg > PLACE_TURN_MAX_DEG) turn_limit_deg = PLACE_TURN_MAX_DEG;   /* never off by size */
    float d = 0.f;
    const int have = place_turn_deg(q_taken, q_now, &d);
    if (have && turned_deg) *turned_deg = d;
    if (b == 1) return PLACE_BUMP_MOVED | (have && d > turn_limit_deg ? PLACE_BUMP_TURNED : 0);
    if (b < 0 || !have) return -1;
    return d > turn_limit_deg ? PLACE_BUMP_TURNED : 0;
}

/* eigen-decompose a symmetric 3x3 (cyclic Jacobi): A is destroyed, V's COLUMNS are the vectors */
static void jacobi3(double A[3][3], double V[3][3], double w[3]) {
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) V[i][j] = (i == j) ? 1.0 : 0.0;
    for (int sweep = 0; sweep < 50; ++sweep) {
        const double off = A[0][1]*A[0][1] + A[0][2]*A[0][2] + A[1][2]*A[1][2];
        if (off < 1e-30) break;
        for (int p = 0; p < 2; ++p) for (int q = p + 1; q < 3; ++q) {
            if (fabs(A[p][q]) < 1e-300) continue;
            const double th = 0.5 * atan2(2.0 * A[p][q], A[q][q] - A[p][p]);
            const double c = cos(th), s = sin(th);
            for (int k = 0; k < 3; ++k) {                     /* A <- A J */
                const double akp = A[k][p], akq = A[k][q];
                A[k][p] = c * akp - s * akq; A[k][q] = s * akp + c * akq;
            }
            for (int k = 0; k < 3; ++k) {                     /* A <- J^T A */
                const double apk = A[p][k], aqk = A[q][k];
                A[p][k] = c * apk - s * aqk; A[q][k] = s * apk + c * aqk;
            }
            for (int k = 0; k < 3; ++k) {                     /* V <- V J */
                const double vkp = V[k][p], vkq = V[k][q];
                V[k][p] = c * vkp - s * vkq; V[k][q] = s * vkp + c * vkq;
            }
        }
    }
    for (int i = 0; i < 3; ++i) w[i] = A[i][i];
}

static int ring_refuse(PlaceRing* r, const char* why) { r->ok = 0; r->why = why; return 0; }

int place_ring_fit(const float (*mk)[3], int n, PlaceRing* r) {
    if (!r) return 0;
    memset(r, 0, sizeof *r);
    r->why = "";
    r->n = n;
    if (!mk || n < 3) return ring_refuse(r, "a ring needs at least 3 markers");
    if (n > PLACE_RING_MAX_N) return ring_refuse(r, "too many markers for a ring");
    double m[3] = { 0.0, 0.0, 0.0 };
    for (int i = 0; i < n; ++i) {
        if (!bwa_finite3_bounded(mk[i], PLACE_MAX_OFFSET_M)) return ring_refuse(r, "a marker is non-finite or absurd");
        for (int a = 0; a < 3; ++a) m[a] += mk[i][a];
    }
    for (int a = 0; a < 3; ++a) m[a] /= n;
    double C[3][3] = { { 0 } };
    for (int i = 0; i < n; ++i) {
        const double d[3] = { mk[i][0] - m[0], mk[i][1] - m[1], mk[i][2] - m[2] };
        for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b) C[a][b] += d[a] * d[b] / n;
    }
    double V[3][3], w[3];
    jacobi3(C, V, w);
    int imin = 0, imax = 0;
    for (int k = 1; k < 3; ++k) { if (w[k] < w[imin]) imin = k; if (w[k] > w[imax]) imax = k; }
    if (imin == imax) imax = (imin + 1) % 3;
    const int imid = 3 - imin - imax;
    for (int a = 0; a < 3; ++a) r->centroid[a] = (float)m[a];
    /* a line first: its "plane" is undefined, and its circle is a division by zero */
    if (!(w[imid] > 0.0) || sqrt(w[imid]) < PLACE_RING_MIN_WIDTH_M)
        return ring_refuse(r, "the markers lie on one line (a ring needs width across it)");
    double nv[3] = { V[0][imin], V[1][imin], V[2][imin] }, u[3] = { V[0][imax], V[1][imax], V[2][imax] };
    if (nv[1] < 0.0) for (int a = 0; a < 3; ++a) nv[a] = -nv[a];   /* the frame's up side */
    const double v[3] = { nv[1]*u[2] - nv[2]*u[1], nv[2]*u[0] - nv[0]*u[2], nv[0]*u[1] - nv[1]*u[0] };
    /* in-plane coordinates, and the distances off the plane */
    double h2 = 0.0, S[3][3] = { { 0 } }, t[3] = { 0.0, 0.0, 0.0 };
    double xs[PLACE_RING_MAX_N], ys[PLACE_RING_MAX_N];
    for (int i = 0; i < n; ++i) {
        const double d[3] = { mk[i][0] - m[0], mk[i][1] - m[1], mk[i][2] - m[2] };
        const double x = d[0]*u[0] + d[1]*u[1] + d[2]*u[2];
        const double y = d[0]*v[0] + d[1]*v[1] + d[2]*v[2];
        const double h = d[0]*nv[0] + d[1]*nv[1] + d[2]*nv[2];
        h2 += h * h;
        xs[i] = x; ys[i] = y;
        /* the algebraic circle: x^2 + y^2 + D x + E y + F = 0, least squares in (D, E, F) */
        const double row[3] = { x, y, 1.0 }, b = -(x * x + y * y);
        for (int a = 0; a < 3; ++a) { t[a] += row[a] * b; for (int c = 0; c < 3; ++c) S[a][c] += row[a] * row[c]; }
    }
    r->plane_rms_m = (float)sqrt(h2 / n);
    for (int a = 0; a < 3; ++a) r->normal[a] = (float)nv[a];
    if (r->plane_rms_m > PLACE_RING_MAX_RMS_M) return ring_refuse(r, "the markers are more than 3 mm RMS off one plane");
    const double det = S[0][0]*(S[1][1]*S[2][2] - S[1][2]*S[2][1]) - S[0][1]*(S[1][0]*S[2][2] - S[1][2]*S[2][0])
                     + S[0][2]*(S[1][0]*S[2][1] - S[1][1]*S[2][0]);
    const double scale = S[0][0] * S[1][1] * S[2][2];
    if (!(fabs(det) > 1e-12 * scale) || !(scale > 0.0)) return ring_refuse(r, "the ring fit is singular (markers on one line?)");
    double sol[3];
    for (int k = 0; k < 3; ++k) {                              /* Cramer: column k replaced by t */
        double M[3][3];
        for (int a = 0; a < 3; ++a) for (int c = 0; c < 3; ++c) M[a][c] = (c == k) ? t[a] : S[a][c];
        sol[k] = (M[0][0]*(M[1][1]*M[2][2] - M[1][2]*M[2][1]) - M[0][1]*(M[1][0]*M[2][2] - M[1][2]*M[2][0])
                + M[0][2]*(M[1][0]*M[2][1] - M[1][1]*M[2][0])) / det;
    }
    const double cx = -0.5 * sol[0], cy = -0.5 * sol[1], r2 = cx * cx + cy * cy - sol[2];
    if (!(r2 > 0.0) || !isfinite(r2)) return ring_refuse(r, "the ring fit found no circle");
    const double rad = sqrt(r2);
    double e2 = 0.0;
    for (int i = 0; i < n; ++i) {
        const double e = sqrt((xs[i] - cx) * (xs[i] - cx) + (ys[i] - cy) * (ys[i] - cy)) - rad;
        e2 += e * e;
    }
    r->radius_m = (float)rad;
    r->circle_rms_m = (float)sqrt(e2 / n);
    for (int a = 0; a < 3; ++a) r->center[a] = (float)(m[a] + cx * u[a] + cy * v[a]);
    r->centroid_to_center_m = (float)sqrt(cx * cx + cy * cy);
    if (!(rad <= PLACE_MAX_OFFSET_M)) return ring_refuse(r, "the ring fit found an absurd radius");
    if (r->circle_rms_m > PLACE_RING_MAX_RMS_M) return ring_refuse(r, "the markers are more than 3 mm RMS off one circle");
    r->ok = 1;
    return 1;
}

const char* place_gate_reason(const PlaceGate* g) {
    if (!g) return place_state_name(PLACE_NO_POSE);
    if (g->state == PLACE_MOVING && g->turning) return "turning";
    return place_state_name(g->state);
}

int place_move_words(const float err_m[3], float dead_m, char* buf, size_t cap) {
    if (!buf || cap < 1) return -1;
    buf[0] = 0;
    if (!err_m || !isfinite(err_m[0]) || !isfinite(err_m[1]) || !isfinite(err_m[2]) || !(dead_m >= 0.f)) {
        snprintf(buf, cap, "n/a");
        return -1;
    }
    /* the move is the opposite of the error, read on the room basis rather than off x/y/z, so the
     * words follow the frame convention from the one place that defines it (room-right is -x) */
    const float* basis[3] = { BWA_ROOM_RIGHT, BWA_ROOM_UP, BWA_ROOM_AHEAD };
    static const char* const pos_word[3] = { "toward room-right", "up",   "toward the front wall" };
    static const char* const neg_word[3] = { "toward room-left",  "down", "toward the back wall" };
    size_t k = 0;
    int named = 0;
    for (int a = 0; a < 3; ++a) {
        const double mv = -((double)err_m[0] * basis[a][0] + (double)err_m[1] * basis[a][1] + (double)err_m[2] * basis[a][2]);
        const long mm = lround(fabs(mv) * 1e3);
        if (!(fabs(mv) >= (double)dead_m) || mm < 1) continue;          /* the dead band, and never "0 mm" */
        if (k >= cap) break;
        const int w = snprintf(buf + k, cap - k, "%s%ld mm %s", named ? ", " : "move ", mm,
                               mv > 0.0 ? pos_word[a] : neg_word[a]);
        if (w < 0) break;
        k += (size_t)w;
        if (k >= cap) k = cap - 1;                                      /* truncated, still terminated */
        ++named;
    }
    if (!named) snprintf(buf, cap, "no move: every axis within %g mm", (double)dead_m * 1e3);
    return named;
}

const char* place_state_name(PlaceState s) {
    switch (s) {
    case PLACE_MOVING:     return "moving";
    case PLACE_OFF_TARGET: return "off target";
    case PLACE_SETTLING:   return "settling";
    case PLACE_OK:         return "OK";
    default:               return "no pose";
    }
}
