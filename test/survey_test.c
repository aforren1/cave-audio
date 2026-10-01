/*
 * survey_test.c - the optical speaker survey's math (src/tracking/survey.c): the baffle-plane aim
 * (sign, planarity, colinearity, recovery under a body rotation), pose averaging, the rigid frame
 * fit (an injected yaw + shift, mirror detection by positions and by aims, the coplanar blind
 * spot), the two-match check, the baffle depth (a joint fit), and body-name matching. The end-to-end run through the NatNet
 * parsers is the speaker_survey_* ctests (bwa_speaker_survey --simulate).
 */
#include "tracking/survey.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", (msg)); ++fails; } } while (0)

#define PI 3.14159265358979323846

static void quat_axis_angle(const double ax[3], double deg, float q[4]) {
    double n = sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
    double h = deg * PI / 360.0, s = sin(h) / n;
    q[0] = (float)(ax[0] * s); q[1] = (float)(ax[1] * s); q[2] = (float)(ax[2] * s); q[3] = (float)cos(h);
}
static void rot(const float R[3][3], const float v[3], float o[3]) {
    for (int i = 0; i < 3; ++i) o[i] = R[i][0] * v[0] + R[i][1] * v[1] + R[i][2] * v[2];
}
static void rotT(const float R[3][3], const float v[3], float o[3]) {
    for (int i = 0; i < 3; ++i) o[i] = R[0][i] * v[0] + R[1][i] * v[1] + R[2][i] * v[2];
}
static float dist(const float a[3], const float b[3]) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}
static void unit(float v[3]) { float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); v[0] /= n; v[1] /= n; v[2] /= n; }

/* Markers on a baffle: the plane through P with normal A, at these in-plane (u, v) spots, plus an
 * optional out-of-plane push on one marker. Then expressed in a body frame of pose (pivot, q):
 * pivot = the marker centroid, local = R^T (world - pivot), the way Motive stores a body. */
static const float SPOTS[5][2] = { { 0.06f, 0.00f }, { -0.05f, 0.04f }, { -0.03f, -0.07f }, { 0.02f, 0.08f }, { 0.07f, -0.05f } };
static void make_body(const float P[3], const float A[3], int n, float push_m, const float q[4],
                      float pivot[3], float local[][3]) {
    float u[3], v[3], t[3] = { 0, 1, 0 };
    if (fabsf(A[1]) > 0.9f) { t[0] = 1; t[1] = 0; }
    u[0] = t[1] * A[2] - t[2] * A[1]; u[1] = t[2] * A[0] - t[0] * A[2]; u[2] = t[0] * A[1] - t[1] * A[0]; unit(u);
    v[0] = A[1] * u[2] - A[2] * u[1]; v[1] = A[2] * u[0] - A[0] * u[2]; v[2] = A[0] * u[1] - A[1] * u[0];
    float w[5][3];
    pivot[0] = pivot[1] = pivot[2] = 0;
    for (int i = 0; i < n; ++i) {
        for (int k = 0; k < 3; ++k) w[i][k] = P[k] + SPOTS[i][0] * u[k] + SPOTS[i][1] * v[k] + (i == 0 ? push_m * A[k] : 0.f);
        for (int k = 0; k < 3; ++k) pivot[k] += w[i][k] / n;
    }
    float R[3][3];
    survey_quat_to_mat(q, R);
    for (int i = 0; i < n; ++i) {
        float d[3] = { w[i][0] - pivot[0], w[i][1] - pivot[1], w[i][2] - pivot[2] };
        rotT(R, d, local[i]);
    }
}

static void test_aim(void) {
    const float ref[3] = { 0.f, 1.5f, 0.f };
    const float P[3] = { 1.8f, 2.4f, -1.2f };                 /* baffle point, up and to one side */
    float A[3] = { ref[0] - P[0], ref[1] - P[1], ref[2] - P[2] };
    unit(A);
    /* tilt the true aim 7 deg off "toward ref", so the test cannot pass by echoing the default */
    { float q[4], R[3][3], o[3]; const double ax[3] = { 0.3, 0.1, 0.9 }; quat_axis_angle(ax, 7.0, q); survey_quat_to_mat(q, R); rot(R, A, o); memcpy(A, o, sizeof o); }

    /* recovery under 24 body rotations (the body frame is arbitrary; the answer must not be) */
    float worst = 0.f, worst_pos = 0.f;
    for (int i = 0; i < 24; ++i) {
        const double ax[3] = { sin(i * 1.3), cos(i * 0.7), 0.4 + 0.1 * i };
        float q[4], pivot[3], local[5][3];
        quat_axis_angle(ax, -170.0 + 15.0 * i, q);
        make_body(P, A, 4, 0.f, q, pivot, local);
        SurveySpeaker s;
        survey_speaker(pivot, q, (const float (*)[3])local, 4, ref, NULL, &s);
        if (!s.aim_ok || s.aim_status != SURVEY_AIM_OK) { worst = 999.f; continue; }
        float e = survey_angle_deg(s.aim, A);
        if (e > worst) worst = e;
        float d = dist(s.pos, pivot);                       /* offset 0: pos = marker centroid = pivot */
        if (d > worst_pos) worst_pos = d;
    }
    printf("  aim recovery over 24 body rotations: worst %.4f deg, position %.3f mm\n", worst, worst_pos * 1000.f);
    CHECK(worst < 0.1f, "plane aim recovers a known aim to 0.1 deg under any body rotation");
    CHECK(worst_pos < 1e-5f, "plane position = the marker centroid");

    /* sign: the same body, the listening point moved behind the baffle -> the aim turns around */
    {
        float q[4] = { 0, 0, 0, 1 }, pivot[3], local[5][3];
        make_body(P, A, 4, 0.f, q, pivot, local);
        SurveySpeaker s1, s2;
        survey_speaker(pivot, q, (const float (*)[3])local, 4, ref, NULL, &s1);
        float behind[3] = { P[0] + 2.f * (P[0] - ref[0]), P[1] + 2.f * (P[1] - ref[1]), P[2] + 2.f * (P[2] - ref[2]) };
        survey_speaker(pivot, q, (const float (*)[3])local, 4, behind, NULL, &s2);
        CHECK(s1.aim_ok && survey_angle_deg(s1.aim, A) < 0.1f, "sign: faces the listening point");
        float negA[3] = { -A[0], -A[1], -A[2] };
        CHECK(s2.aim_ok && survey_angle_deg(s2.aim, negA) < 0.1f, "sign: flips when the listening point is behind");
        /* a body rotated 180 deg about an in-plane axis flips its LOCAL normal; the aim must not flip */
        float q180[4]; const double ax[3] = { 1, 0, 0 };
        quat_axis_angle(ax, 180.0, q180);
        make_body(P, A, 4, 0.f, q180, pivot, local);
        survey_speaker(pivot, q180, (const float (*)[3])local, 4, ref, NULL, &s2);
        CHECK(s2.aim_ok && survey_angle_deg(s2.aim, A) < 0.1f, "sign: independent of the body's own axes");
    }

    /* baffle offset: the layout point sits behind the baffle along -aim */
    {
        float q[4], pivot[3], local[5][3]; const double ax[3] = { 0, 1, 0 };
        quat_axis_angle(ax, 40.0, q);
        make_body(P, A, 4, 0.f, q, pivot, local);
        SurveyOpts o = { 0 };
        o.baffle_offset_m = 0.1f;
        SurveySpeaker s;
        survey_speaker(pivot, q, (const float (*)[3])local, 4, ref, &o, &s);
        float want[3] = { pivot[0] - 0.1f * A[0], pivot[1] - 0.1f * A[1], pivot[2] - 0.1f * A[2] };
        CHECK(dist(s.pos, want) < 1e-4f && dist(s.centroid, pivot) < 1e-5f, "baffle offset moves the point behind the baffle");
    }

    /* planarity: one marker 1 mm proud still passes, 20 mm proud is rejected with the RMS reported */
    {
        float q[4] = { 0, 0, 0, 1 }, pivot[3], local[5][3];
        SurveySpeaker s;
        make_body(P, A, 5, 0.001f, q, pivot, local);
        survey_speaker(pivot, q, (const float (*)[3])local, 5, ref, NULL, &s);
        CHECK(s.aim_ok && s.plane_rms_m < 0.001f, "planarity: 1 mm out of plane passes");
        make_body(P, A, 5, 0.020f, q, pivot, local);
        survey_speaker(pivot, q, (const float (*)[3])local, 5, ref, NULL, &s);
        printf("  one marker 20 mm proud: plane rms %.2f mm -> %s\n", s.plane_rms_m * 1000.f, survey_aim_status_str(s.aim_status));
        CHECK(!s.aim_ok && s.aim_status == SURVEY_AIM_NOT_FLAT && s.plane_rms_m > SURVEY_PLANE_MAX_RMS_M,
              "planarity: 20 mm out of plane rejected");
        CHECK(isfinite(s.pos[0]) && dist(s.pos, s.centroid) == 0.f, "planarity: position still reported");
        SurveyOpts o = { 0 };
        o.max_plane_rms_m = 0.02f;
        survey_speaker(pivot, q, (const float (*)[3])local, 5, ref, &o, &s);
        CHECK(s.aim_ok, "planarity: the threshold is configurable");
    }

    /* colinear and too few */
    {
        const float line[4][3] = { { 0, 0, 0 }, { 0.05f, 0.0002f, 0 }, { 0.10f, -0.0003f, 0.0001f }, { 0.15f, 0, 0 } };
        float q[4] = { 0, 0, 0, 1 }, bp[3] = { 1, 2, 1 };
        SurveySpeaker s;
        survey_speaker(bp, q, line, 4, ref, NULL, &s);
        CHECK(!s.aim_ok && s.aim_status == SURVEY_AIM_COLINEAR, "colinear markers rejected");
        survey_speaker(bp, q, line, 2, ref, NULL, &s);
        CHECK(!s.aim_ok && s.aim_status == SURVEY_AIM_FEW_MARKERS, "two markers rejected");
        CHECK(fabsf(s.pos[0] - 1.025f) < 1e-5f, "two markers: position = their centroid");
        survey_speaker(bp, q, NULL, 0, ref, NULL, &s);
        CHECK(!s.aim_ok && dist(s.pos, bp) == 0.f, "no markers: position = the body");
    }

    /* the configured-axis alternative: markers anywhere, the aim follows the body's local axis */
    {
        float q[4], R[3][3]; const double ax[3] = { 0.2, 1, -0.3 };
        quat_axis_angle(ax, 63.0, q);
        survey_quat_to_mat(q, R);
        SurveyOpts o = { 0 };
        o.use_axis = true;
        rotT(R, A, o.axis_local);                            /* the body axis that IS the aim */
        SurveySpeaker s;
        survey_speaker(P, q, NULL, 0, ref, &o, &s);
        CHECK(s.aim_ok && s.aim_status == SURVEY_AIM_AXIS && survey_angle_deg(s.aim, A) < 0.01f && !s.aim_away,
              "axis mode recovers the aim");
        o.axis_local[0] = -o.axis_local[0]; o.axis_local[1] = -o.axis_local[1]; o.axis_local[2] = -o.axis_local[2];
        survey_speaker(P, q, NULL, 0, ref, &o, &s);
        CHECK(s.aim_ok && s.aim_away, "axis mode: a backwards axis is reported, not flipped");
        o.axis_local[0] = o.axis_local[1] = o.axis_local[2] = 0.f;
        survey_speaker(P, q, NULL, 0, ref, &o, &s);
        CHECK(!s.aim_ok && s.aim_status == SURVEY_AIM_BAD_AXIS, "axis mode: zero axis rejected");
    }
}

static void test_avg(void) {
    SurveyAvg a;
    survey_avg_reset(&a);
    const double z[3] = { 0, 0, 1 };
    float base[4], plus[4], minus[4];
    quat_axis_angle(z, 30.0, base);
    const double th = 0.2;                                   /* +/- 0.2 deg about z around base */
    quat_axis_angle(z, 30.0 + th, plus);
    quat_axis_angle(z, 30.0 - th, minus);
    for (int i = 0; i < 100; ++i) {
        float p[3] = { 2.f + ((i & 1) ? 0.001f : -0.001f), 1.f, -3.f };
        float q[4];
        memcpy(q, (i & 1) ? plus : minus, sizeof q);
        if (i % 3 == 0) for (int k = 0; k < 4; ++k) q[k] = -q[k];   /* same rotation, other sign */
        CHECK(survey_avg_add(&a, p, q), "avg add");
    }
    const float nanp[3] = { NAN, 0, 0 }, okp[3] = { 0, 0, 0 }, zq[4] = { 0, 0, 0, 0 };
    CHECK(!survey_avg_add(&a, nanp, base) && !survey_avg_add(&a, okp, zq), "avg rejects non-finite / zero quat");
    float p[3], q[4], sm, sd;
    CHECK(survey_avg_result(&a, p, q, &sm, &sd), "avg result");
    float dq = fabsf(q[0] * base[0] + q[1] * base[1] + q[2] * base[2] + q[3] * base[3]);
    printf("  averaging: position spread %.4f mm (want 1), rotation spread %.4f deg (want %.1f)\n", sm * 1000.f, sd, th);
    CHECK(fabsf(p[0] - 2.f) < 1e-6f && fabsf(p[2] + 3.f) < 1e-6f, "avg mean position");
    CHECK(fabsf(sm - 0.001f) < 1e-6f, "avg position spread");
    CHECK(dq > 0.9999999f, "avg mean rotation survives sign flips");
    CHECK(fabsf(sd - (float)th) < 0.002f, "avg rotation spread");
    SurveyAvg e;
    survey_avg_reset(&e);
    CHECK(!survey_avg_result(&e, p, q, &sm, &sd), "avg with no samples");
}

/* four speakers of a dome-like array: NOT coplanar */
static const float LAY4[4][3] = { { 1.8f, 2.4f, -1.2f }, { -1.6f, 1.2f, -1.4f }, { -0.3f, 2.9f, 1.7f }, { 1.4f, 0.9f, 1.5f } };
/* four on one ring: coplanar */
static const float RING[4][3] = { { 1.5f, 2.5f, 0.f }, { 0.f, 2.5f, 1.5f }, { -1.5f, 2.5f, 0.f }, { 0.2f, 2.5f, -1.5f } };

/* opt = R_inj (mirror? S lay : lay) + t_inj: what Motive would report if its frame were off */
static void motive_view(const float (*lay)[3], int n, double yaw_deg, const float t[3], bool mirror, float (*opt)[3]) {
    const double y[3] = { 0, 1, 0 };
    float q[4], R[3][3];
    quat_axis_angle(y, yaw_deg, q);
    survey_quat_to_mat(q, R);
    for (int i = 0; i < n; ++i) {
        float v[3] = { mirror ? -lay[i][0] : lay[i][0], lay[i][1], lay[i][2] };
        rot(R, v, opt[i]);
        for (int k = 0; k < 3; ++k) opt[i][k] += t[k];
    }
}

static void test_frame(void) {
    const float shift[3] = { 0.05f, 0.f, 0.f };
    float opt[4][3];
    SurveyFrameFit f;

    /* an injected 2 deg yaw + 5 cm shift */
    motive_view(LAY4, 4, 2.0, shift, false, opt);
    CHECK(survey_fit_frame((const float (*)[3])opt, LAY4, NULL, NULL, NULL, 4, &f), "fit: 4 points");
    float tlen = sqrtf(f.t[0] * f.t[0] + f.t[1] * f.t[1] + f.t[2] * f.t[2]);
    printf("  frame fit: %.4f deg about [%.3f %.3f %.3f], |t| %.2f mm, rms %.4f mm, mirrored rms %.1f mm\n",
           f.angle_deg, f.axis[0], f.axis[1], f.axis[2], tlen * 1000.f, f.rms_m * 1000.f, f.rms_mirror_m * 1000.f);
    CHECK(fabsf(f.angle_deg - 2.0f) < 0.01f && fabsf(fabsf(f.axis[1]) - 1.f) < 1e-3f, "fit: recovers the 2 deg yaw");
    CHECK(f.axis[1] < 0.f, "fit: the correction is the INVERSE yaw (optical -> layout)");
    CHECK(fabsf(tlen - 0.05f) < 1e-4f && f.rms_m < 1e-4f, "fit: recovers the 5 cm shift, exact residual");
    {   /* and it maps each optical point onto its layout point */
        float worst = 0.f;
        for (int i = 0; i < 4; ++i) {
            float r[3]; rot((const float (*)[3])f.R, opt[i], r);
            for (int k = 0; k < 3; ++k) r[k] += f.t[k];
            if (dist(r, LAY4[i]) > worst) worst = dist(r, LAY4[i]);
        }
        CHECK(worst < 1e-4f, "fit: R opt + t = layout");
    }
    CHECK(f.hand == SURVEY_HAND_OK && f.hand_by == SURVEY_HAND_BY_POSITIONS, "fit: right-handed, decided by positions");

    /* a mirrored Motive frame, non-coplanar speakers: the positions catch it */
    motive_view(LAY4, 4, 2.0, shift, true, opt);
    CHECK(survey_fit_frame((const float (*)[3])opt, LAY4, NULL, NULL, NULL, 4, &f), "fit mirrored");
    printf("  mirrored frame: proper rms %.1f mm, mirrored rms %.4f mm\n", f.rms_m * 1000.f, f.rms_mirror_m * 1000.f);
    CHECK(f.hand == SURVEY_HAND_MIRRORED && f.hand_by == SURVEY_HAND_BY_POSITIONS, "fit: mirror detected by positions");

    /* coplanar ring: positions are blind, aims tilted out of the plane decide it */
    const float ref[3] = { 0.f, 1.5f, 0.f };
    float lay_aim[4][3], opt_aim[4][3];
    bool ok[4] = { true, true, true, true };
    for (int i = 0; i < 4; ++i) { for (int k = 0; k < 3; ++k) lay_aim[i][k] = ref[k] - RING[i][k]; unit(lay_aim[i]); }
    for (int mir = 0; mir < 2; ++mir) {
        motive_view(RING, 4, 2.0, shift, mir != 0, opt);
        const float zero[3] = { 0, 0, 0 };
        motive_view((const float (*)[3])lay_aim, 4, 2.0, zero, mir != 0, opt_aim);   /* aims rotate, never shift */
        CHECK(survey_fit_frame((const float (*)[3])opt, RING, NULL, NULL, NULL, 4, &f), "ring fit");
        CHECK(f.hand == SURVEY_HAND_UNDETERMINED, mir ? "ring, mirrored, no aims: UNDETERMINED, not a guess"
                                                      : "ring, no aims: UNDETERMINED, not a guess");
        CHECK(survey_fit_frame((const float (*)[3])opt, RING, (const float (*)[3])opt_aim,
                               (const float (*)[3])lay_aim, ok, 4, &f), "ring fit with aims");
        printf("  coplanar ring%s: thickness %.1f mm, aims proper %.2f deg, mirrored %.2f deg\n",
               mir ? " (mirrored)" : "", f.thickness_m * 1000.f, f.aim_deg, f.aim_mirror_deg);
        CHECK(f.hand == (mir ? SURVEY_HAND_MIRRORED : SURVEY_HAND_OK) && f.hand_by == SURVEY_HAND_BY_AIMS,
              mir ? "ring: mirror caught by the aims" : "ring: handedness confirmed by the aims");
    }

    /* three points are always coplanar: a mirror cannot be told from the truth by position */
    motive_view(LAY4, 3, 2.0, shift, true, opt);
    CHECK(survey_fit_frame((const float (*)[3])opt, LAY4, NULL, NULL, NULL, 3, &f), "fit: 3 points");
    CHECK(f.hand == SURVEY_HAND_UNDETERMINED && f.rms_m < 1e-4f, "fit: 3 mirrored points fit exactly, UNDETERMINED");

    /* degenerate: colinear layout points, and n out of range */
    const float line[3][3] = { { 0, 1, 0 }, { 1, 1, 0 }, { 2, 1, 0 } };
    CHECK(!survey_fit_frame(line, line, NULL, NULL, NULL, 3, &f), "fit: colinear refused");
    CHECK(!survey_fit_frame(LAY4, LAY4, NULL, NULL, NULL, 2, &f), "fit: 2 points refused");

    /* the two-match check */
    motive_view(LAY4, 2, 2.0, shift, false, opt);
    CHECK(fabsf(survey_pair_delta(opt[0], opt[1], LAY4[0], LAY4[1])) < 1e-5f, "pair: a rigid move keeps the distance");
    float far1[3] = { opt[1][0] * 1.01f, opt[1][1] * 1.01f, opt[1][2] * 1.01f };
    CHECK(fabsf(survey_pair_delta(opt[0], far1, LAY4[0], LAY4[1])) > 0.01f, "pair: a distance change shows");
}

/* The baffle depth. The layout points are acoustic centers, each box faces the listening point, and
 * the markers' centroid sits D IN FRONT of the center along that aim; Motive sees it through a yaw and
 * a shift. opt/oaim: what the survey reports (points and aims in Motive's frame). */
static const float REF15[3] = { 0.f, 1.5f, 0.f };
/* four dome_24 speakers (8, 13, 16, 21) on one side: their aims are alike, which is what makes the
 * plain frame fit eat the depth */
static const float LOP4[4][3] = { { 1.863f, 1.24f, 0.68f }, { 1.899f, 1.969f, -0.417f }, { 1.363f, 2.406f, 1.149f },
                                  { 1.141f, 3.135f, 0.154f } };
static void depth_view(const float (*lay)[3], float D, double yaw_deg, const float t[3], float (*opt)[3], float (*oaim)[3]) {
    const float zero[3] = { 0.f, 0.f, 0.f };
    float aim[4][3], front[4][3];
    for (int i = 0; i < 4; ++i) {
        for (int k = 0; k < 3; ++k) aim[i][k] = REF15[k] - lay[i][k];
        unit(aim[i]);
        for (int k = 0; k < 3; ++k) front[i][k] = lay[i][k] + D * aim[i][k];
    }
    motive_view((const float (*)[3])front, 4, yaw_deg, t, false, opt);
    motive_view((const float (*)[3])aim, 4, yaw_deg, zero, false, oaim);    /* aims rotate, never shift */
}
/* The joint fit must return +60 mm (the sign: a center behind the baffle is positive), per speaker
 * too, with nothing across the aims, and the frame it was seen through, on a spread set and on a
 * lopsided one. The plain frame fit's reading of the lopsided set is the reason the depth is a fit
 * unknown, so its error is demanded too: a check that cannot see the difference proves nothing. */
static void test_depth(void) {
    const float shift[3] = { 0.05f, 0.f, 0.f }, zero[3] = { 0.f, 0.f, 0.f }, D = 0.06f;
    float opt[4][3], oaim[4][3], di[4], ac[4];
    SurveyDepthFit f;
    for (int set = 0; set < 2; ++set) {
        const float (*lay)[3] = set ? LOP4 : LAY4;
        const char* name = set ? "lopsided" : "spread";
        depth_view(lay, D, 2.0, shift, opt, oaim);
        CHECK(survey_fit_depth((const float (*)[3])opt, (const float (*)[3])oaim, lay, 4, &f, di, ac), "depth: fit");
        float worst_d = 0.f, worst_a = 0.f;
        for (int i = 0; i < 4; ++i) {
            if (fabsf(di[i] - D) > worst_d) worst_d = fabsf(di[i] - D);
            if (ac[i] > worst_a) worst_a = ac[i];
        }
        /* the plain frame fit + a dot product: the translation eats part of the depth */
        SurveyFrameFit pf;
        CHECK(survey_fit_frame((const float (*)[3])opt, lay, NULL, NULL, NULL, 4, &pf), "depth: plain fit");
        float plain = 0.f;
        for (int i = 0; i < 4; ++i) {
            float r[3], b[3];
            rot((const float (*)[3])pf.R, opt[i], r);
            rot((const float (*)[3])pf.R, oaim[i], b);
            for (int k = 0; k < 3; ++k) plain += (r[k] + pf.t[k] - lay[i][k]) * b[k] / 4.f;
        }
        printf("  depth fit, %s set: %.3f mm (true %.1f), per speaker within %.4f mm, across %.4f mm, %.3f deg, |t| %.2f mm,\n"
               "    leverage %.2f, %d iterations; the plain frame fit reads %.1f mm\n", name, f.depth_m * 1e3f, D * 1e3f,
               worst_d * 1e3f, worst_a * 1e3f, f.angle_deg, f.t_m * 1e3f, f.leverage, f.iters, plain * 1e3f);
        CHECK(fabsf(f.depth_m - D) < 1e-4f, "depth: recovers +60 mm (positive = behind the baffle)");
        CHECK(worst_d < 1e-4f && worst_a < 1e-4f, "depth: per speaker, nothing across the aims");
        CHECK(fabsf(f.angle_deg - 2.0f) < 0.01f && fabsf(f.t_m - 0.05f) < 1e-4f, "depth: the frame it was seen through");
        if (set) CHECK(fabsf(plain - D) > 0.02f, "depth: the plain fit cannot read the lopsided set (the power to tell)");
    }

    /* a center IN FRONT of the baffle reads negative */
    depth_view(LAY4, -0.02f, 0.0, zero, opt, oaim);
    CHECK(survey_fit_depth((const float (*)[3])opt, (const float (*)[3])oaim, LAY4, 4, &f, NULL, NULL) &&
          fabsf(f.depth_m + 0.02f) < 1e-4f, "depth: a center in front of the baffle is negative");

    /* every box facing one way: the depth is a translation, refused; and too few speakers */
    float same[4][3];
    for (int i = 0; i < 4; ++i) { same[i][0] = 0.f; same[i][1] = 0.f; same[i][2] = 1.f; }
    CHECK(!survey_fit_depth(LAY4, (const float (*)[3])same, LAY4, 4, &f, NULL, NULL), "depth: parallel aims refused");
    CHECK(!survey_fit_depth(LAY4, (const float (*)[3])oaim, LAY4, 2, &f, NULL, NULL), "depth: 2 speakers refused");
}

static void test_names(void) {
    struct { const char* s; int want; } cases[] = {
        { "spk07", 7 }, { "Speaker_7", 7 }, { "SPK-7", 7 }, { "speaker 12", 12 }, { "spk.3", 3 },
        { "spk-007", 7 }, { "SpK0", 0 }, { "spk", -1 }, { "spk_", -1 }, { "spk7a", -1 }, { "spk__7", -1 },
        { "Wand", -1 }, { "spkr3", -1 }, { "speaker", -1 }, { "xspk3", -1 }, { "spk 7 ", -1 }, { "", -1 },
        { "spk99999999999", -1 },
    };
    int bad = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i)
        if (survey_name_index(cases[i].s) != cases[i].want) { printf("  name '%s' -> %d, want %d\n", cases[i].s, survey_name_index(cases[i].s), cases[i].want); ++bad; }
    CHECK(bad == 0, "name rule");
    CHECK(survey_name_index(NULL) == -1, "name rule: NULL");

    const char* names[] = { "spk03", "Wand", "Speaker_7", "spk99", "custom", "spk3", "spk7" };
    const SurveyMapEntry map[] = { { "CUSTOM", 5 }, { "missing", 2 }, { "spk7", 8 } };
    SurveyMatch m[7];
    bool used[3];
    survey_match_names(names, 7, map, 3, 24, m, used);
    CHECK(m[0].status == SURVEY_MATCH_OK && m[0].index == 3 && !m[0].by_map, "match: spk03 -> 3");
    CHECK(m[1].status == SURVEY_MATCH_NO_RULE, "match: Wand ignored");
    CHECK(m[2].status == SURVEY_MATCH_OK && m[2].index == 7, "match: Speaker_7 -> 7");
    CHECK(m[3].status == SURVEY_MATCH_OUT_OF_RANGE && m[3].index == 99, "match: spk99 out of range");
    CHECK(m[4].status == SURVEY_MATCH_OK && m[4].index == 5 && m[4].by_map, "match: --map custom=5 (case-insensitive)");
    CHECK(m[5].status == SURVEY_MATCH_DUPLICATE && m[5].other == 0, "match: spk3 duplicates spk03");
    CHECK(m[6].status == SURVEY_MATCH_OK && m[6].index == 8 && m[6].by_map, "match: --map overrides the rule");
    CHECK(used[0] && !used[1] && used[2], "match: the unused --map entry is reported");

    /* the map wins even when the rule's body comes first on the wire */
    const char* names2[] = { "spk5", "custom" };
    SurveyMatch m2[2];
    survey_match_names(names2, 2, map, 1, 24, m2, NULL);
    CHECK(m2[1].status == SURVEY_MATCH_OK && m2[1].index == 5 && m2[0].status == SURVEY_MATCH_DUPLICATE && m2[0].other == 1,
          "match: --map beats the rule regardless of order");
    const SurveyMapEntry neg[] = { { "Wand", -1 } };
    survey_match_names(names, 2, neg, 1, 24, m, NULL);
    CHECK(m[1].status == SURVEY_MATCH_OUT_OF_RANGE, "match: a negative --map index is out of range");
}

int main(void) {
    test_aim();
    test_avg();
    test_frame();
    test_depth();
    test_names();
    if (fails) { printf("survey_test: %d FAILURES\n", fails); return 1; }
    printf("survey_test OK (plane aim + sign + planarity + colinear + axis mode, averaging, frame fit + mirror + coplanar blind spot, pair check, baffle depth, names)\n");
    return 0;
}
