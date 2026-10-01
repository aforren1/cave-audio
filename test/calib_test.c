/* calib_test.c — the calibration trim solve + layout writeback, verified without the rig. */
#include "calib/calib.h"
#include "core/layout.h"        /* BWA_ROOM_EQ_MAX (the room_eq writeback round-trip) */
#include "dsp/sos.h"           /* the temperature parsers + the plausible-c guard */

#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); ++fails; } } while (0)

int main(void) {
    const double fs = 48000.0, c = 343.0;

    /* three speakers in a line at 1/2/3 m from the mic at the origin, EQUAL sensitivity. A 1/r level
     * and a distance-proportional delay (+ common latency) is what an ideal rig would measure. */
    MeasureResult m[3];
    float pos[3][3] = { {1,0,0}, {2,0,0}, {3,0,0} };
    float mic[3]    = { 0,0,0 };
    for (int i = 0; i < 3; ++i) {
        double dist = i + 1.0;
        m[i].delay_samples = (int)lround(1000.0 + dist / c * fs);   /* latency + time of flight */
        m[i].level = (float)(1.0 / dist);                          /* 1/r, equal sensitivity */
        m[i].band[0] = m[i].band[1] = m[i].band[2] = m[i].level;
    }
    float gdb[3], dms[3];
    calib_solve(m, pos, mic, 3, fs, gdb, dms);
    printf("equal-sens: gain_db=[%.2f %.2f %.2f]  delay_ms=[%.3f %.3f %.3f]\n",
           gdb[0], gdb[1], gdb[2], dms[0], dms[1], dms[2]);
    /* farthest (speaker 2) is the reference: 0 added delay; nearer get delayed to match */
    CHECK(dms[2] == 0.f, "farthest speaker gets zero delay trim");
    CHECK(fabs(dms[0] - 2.0/c*1000.0) < 0.05, "nearest delayed by the 2 m extra time of flight (~5.83 ms)");
    CHECK(dms[0] > dms[1] && dms[1] > dms[2], "delay trim decreases with distance");
    /* equal sensitivity (1/r divided out) -> all gain trims ~ 0 dB */
    for (int i = 0; i < 3; ++i) CHECK(fabs(gdb[i]) < 0.3, "equal sensitivity -> ~0 dB trim");

    /* now make speaker 0 twice as sensitive: it must be cut ~6 dB, the others untouched */
    m[0].level = (float)(2.0 / 1.0);
    calib_solve(m, pos, mic, 3, fs, gdb, dms);
    printf("louder-spk0: gain_db=[%.2f %.2f %.2f]\n", gdb[0], gdb[1], gdb[2]);
    CHECK(fabs(gdb[0] - (-6.02)) < 0.3, "2x-sensitive speaker cut ~6 dB");
    CHECK(fabs(gdb[1]) < 0.3 && fabs(gdb[2]) < 0.3, "others stay ~0 dB (cut-only normalization)");

    /* a dead speaker (level ~ 0) is excluded + left at unity, not allowed to drag the reference */
    MeasureResult m2[3] = { m[0], m[1], m[2] };
    m2[1].level = 0.f;
    calib_solve(m2, pos, mic, 3, fs, gdb, dms);
    CHECK(gdb[1] == 0.f, "dead speaker left at 0 dB");
    CHECK(gdb[0] < -1.f, "live louder speaker still cut (dead one didn't poison the reference)");

    /* directivity re-aim (calib_solve_corr): speaker 0 measured 6 dB down because the mic sat off
     * its axis, equal sensitivity otherwise. Without the correction the other two get cut to match
     * it (cut-only normalization); with corr[0] = 2 (D(ref)/D(mic)) every trim returns to 0 dB. A
     * NaN factor reads as 1 rather than poisoning the file. */
    {
        MeasureResult m3[3] = { m[0], m[1], m[2] };
        m3[0].level = (float)(0.5 / 1.0);                /* 1/r x an off-axis loss of 0.5 */
        calib_solve_corr(m3, pos, mic, 3, fs, NULL, gdb, dms);
        CHECK(fabs(gdb[0]) < 0.3 && fabs(gdb[1] + 6.02) < 0.3 && fabs(gdb[2] + 6.02) < 0.3,
              "off-axis mic, no model: the others are cut to the quiet one");
        float corr[3] = { 2.f, 1.f, 1.f };
        calib_solve_corr(m3, pos, mic, 3, fs, corr, gdb, dms);
        for (int i = 0; i < 3; ++i) CHECK(fabs(gdb[i]) < 0.3, "off-axis mic, model correction: trims back to ~0 dB");
        float bad[3] = { NAN, 1.f, 1.f };
        calib_solve_corr(m3, pos, mic, 3, fs, bad, gdb, dms);
        CHECK(gdb[0] == gdb[0] && fabs(gdb[1] + 6.02) < 0.3, "a NaN correction factor reads as 1");
        CHECK(fabs(dms[0] - 2.0/c*1000.0) < 0.05, "the correction never touches the delay trims");
    }

    /* aim check (calib_check_aim): tilts synthesized from a strongly directional model with the
     * TRUE aim rotated 20 deg off the layout's, plus a per-speaker tilt constant and deterministic
     * noise. The fit must recover the rotation to a few degrees and beat the layout aim's residual;
     * an unrotated set must fit back to the layout aim; too few positions or a colinear set must be
     * refused rather than answered. */
    {
        static Layout T; layout_default(&T);
        T.dir.nband = 2; T.dir.nang = 5; T.dir.split_hz = 1000.f;
        T.dir.band_hz[0] = 250.f; T.dir.band_hz[1] = 4000.f;
        const float ang[5] = { 0, 30, 60, 90, 180 };
        for (int a = 0; a < 5; ++a) {
            T.dir.ang_deg[a] = ang[a];
            T.dir.loss_db[0][a] = -0.03f * ang[a];
            T.dir.loss_db[1][a] = -0.25f * ang[a];       /* -7.5 dB at 30 deg: a waveguide's treble */
        }
        directivity_derive(&T.dir);
        const double band_hz[2] = { 300.0, 3000.0 }, f2 = 20000.0;
        const int S = 3;
        const float* sp = T.speakers[S].pos;
        /* the true aim: the layout's, rotated 20 deg about a perpendicular */
        float aim_true[3], u[3], up[3] = { 0, 1, 0 };
        if (fabsf(T.speakers[S].aim[1]) > 0.9f) { up[0] = 1; up[1] = 0; }
        const float* a0 = T.speakers[S].aim;
        u[0] = a0[1]*up[2] - a0[2]*up[1]; u[1] = a0[2]*up[0] - a0[0]*up[2]; u[2] = a0[0]*up[1] - a0[1]*up[0];
        { float l = sqrtf(u[0]*u[0]+u[1]*u[1]+u[2]*u[2]); u[0]/=l; u[1]/=l; u[2]/=l; }
        { float ca = cosf(20.f*3.14159265f/180.f), sa = sinf(20.f*3.14159265f/180.f);
          for (int j = 0; j < 3; ++j) aim_true[j] = ca * a0[j] + sa * u[j]; }
        /* seven mic positions spread over the working area around ref */
        float micp[7][3]; float tilt[7], tilt0[7];
        const float off[7][3] = { {0,0,0}, {0.9f,0,0}, {-0.9f,0,0}, {0,0,0.9f}, {0,0,-0.9f}, {0,0.6f,0.5f}, {0.5f,-0.5f,0} };
        for (int k = 0; k < 7; ++k) {
            for (int j = 0; j < 3; ++j) micp[k][j] = T.ref[j] + off[k][j];
            float b[3]; unit_dir(sp, micp[k], b);
            float ct = b[0]*aim_true[0] + b[1]*aim_true[1] + b[2]*aim_true[2];
            float c0 = b[0]*a0[0] + b[1]*a0[1] + b[2]*a0[2];
            float th_true = acosf(ct > 1 ? 1 : ct) * 180.f / 3.14159265f;
            float th_0    = acosf(c0 > 1 ? 1 : c0) * 180.f / 3.14159265f;
            float noise = 0.1f * sinf(3.7f * k);                     /* +/- 0.1 dB, deterministic */
            tilt[k]  = calib_aim_tilt_db(&T.dir, th_true, band_hz, f2) - 2.0f + noise;   /* -2 dB: the box's own tilt */
            tilt0[k] = calib_aim_tilt_db(&T.dir, th_0,    band_hz, f2) - 2.0f + noise;
        }
        CalibAimResult ar;
        calib_check_aim(&T, S, micp, tilt, 7, band_hz, f2, &ar);
        float cfit = ar.aim_fit[0]*aim_true[0] + ar.aim_fit[1]*aim_true[1] + ar.aim_fit[2]*aim_true[2];
        float miss = acosf(cfit > 1 ? 1 : cfit) * 180.f / 3.14159265f;
        printf("check-aim: spread %.1f deg, rms %.2f -> %.2f dB, layout-vs-fit %.1f deg (true 20), fit-vs-true %.1f deg\n",
               ar.spread_deg, ar.rms_layout_db, ar.rms_fit_db, ar.aim_err_deg, miss);
        CHECK(ar.ok, "check-aim: seven spread positions are enough to fit");
        CHECK(fabs(ar.aim_err_deg - 20.f) < 4.f, "check-aim: recovers a 20 deg aim error to within 4 deg");
        CHECK(miss < 4.f, "check-aim: the fitted axis lands on the true one");
        CHECK(ar.rms_fit_db < 0.5f * ar.rms_layout_db && ar.rms_fit_db < 0.3f,
              "check-aim: the fit explains the tilts far better than the layout aim");
        calib_check_aim(&T, S, micp, tilt0, 7, band_hz, f2, &ar);
        CHECK(ar.ok && ar.aim_err_deg < 3.f, "check-aim: a correctly aimed speaker fits back to its layout aim");
        calib_check_aim(&T, S, micp, tilt, 2, band_hz, f2, &ar);
        CHECK(!ar.ok && ar.npos == 2, "check-aim: two positions are refused");
        float lin[5][3];                                    /* positions along the speaker's own axis: no spread */
        for (int k = 0; k < 5; ++k) for (int j = 0; j < 3; ++j) lin[k][j] = sp[j] + (0.5f + 0.3f * k) * a0[j];
        calib_check_aim(&T, S, lin, tilt, 5, band_hz, f2, &ar);
        CHECK(!ar.ok && ar.spread_deg < 1.f, "check-aim: colinear positions are refused, not answered");
        static Layout NM; layout_default(&NM);
        calib_check_aim(&NM, S, micp, tilt, 7, band_hz, f2, &ar);
        CHECK(!ar.ok, "check-aim: no model, no fit");
    }

    /* live aiming (calib_aim_curve / calib_aim_invert / calib_on_axis_tilt_db / calib_peak_*): a model
     * with a waveguide's flat top (loss ~ 1 - cos, so the tilt is quadratic near 0 deg, like the
     * 4410A's), inverted back to the angles it was sampled at; the bracket and the "on axis" read near
     * 0 deg; a reading past the curve's range; a non-monotonic curve; the file's 0 deg tilt; peak hold. */
    {
        static Directivity D; memset(&D, 0, sizeof D);
        const float bands[6] = { 500, 1000, 2000, 4000, 8000, 16000 };
        const float gk[6]    = { 0.5f, 1.f, 2.f, 4.f, 8.f, 14.f };   /* dB of loss at 90 deg per band */
        const float ang[8]   = { 0, 10, 20, 30, 45, 60, 90, 180 };
        D.nband = 6; D.nang = 8; D.split_hz = 1000.f;
        for (int b = 0; b < 6; ++b) {
            D.band_hz[b] = bands[b];
            for (int a = 0; a < 8; ++a) {
                D.ang_deg[a] = ang[a];
                D.loss_db[b][a] = -gk[b] * (1.f - cosf(ang[a] * 3.14159265f / 180.f));
            }
        }
        directivity_derive(&D);
        const double bh[2] = { CALIB_AIM_MID_HZ, CALIB_AIM_HIGH_HZ }, f2 = 20000.0;
        static float curve[CALIB_AIM_CURVE_N];
        calib_aim_curve(&D, bh, f2, curve);
        CHECK(curve[0] == 0.f && curve[4 * 30] < -0.5f, "live: the curve is 0 on axis and falls off it");
        const float th[6] = { 5.f, 12.f, 20.f, 30.f, 45.f, 60.f };
        float worst = 0.f;
        for (int i = 0; i < 6; ++i) {
            CalibAimAngle a;
            const float rel = calib_aim_tilt_db(&D, th[i], bh, f2);
            CHECK(calib_aim_invert(curve, rel, 0.f, &a), "live: an in-range reading inverts");
            const float e = fabsf(a.angle_deg - th[i]);
            if (e > worst) worst = e;
        }
        printf("live: round trip through the model at 5..60 deg: worst %.3f deg\n", worst);
        CHECK(worst < 0.3f, "live: the inversion round-trips the model to under 0.3 deg");
        {   /* the bracket: lo from rel + tol, hi from rel - tol, around the estimate */
            CalibAimAngle a, lo, hi;
            const float rel = calib_aim_tilt_db(&D, 30.f, bh, f2);
            calib_aim_invert(curve, rel, 0.3f, &a);
            calib_aim_invert(curve, rel + 0.3f, 0.f, &lo);
            calib_aim_invert(curve, rel - 0.3f, 0.f, &hi);
            printf("live: 30 deg reads %.2f deg [%.2f-%.2f] for +/-0.3 dB\n", a.angle_deg, a.lo_deg, a.hi_deg);
            CHECK(a.lo_deg < 29.f && a.hi_deg > 31.f && !a.on_axis && !a.beyond, "live: a 30 deg reading gets a bracket around it");
            CHECK(fabsf(a.lo_deg - lo.angle_deg) < 1e-4f && fabsf(a.hi_deg - hi.angle_deg) < 1e-4f,
                  "live: the bracket is the curve inverted at the reading +/- tol");
        }
        {   /* near 0 the curve is flat: a 3 deg box cannot be told from on axis within 0.3 dB */
            CalibAimAngle a;
            const float rel = calib_aim_tilt_db(&D, 3.f, bh, f2);
            calib_aim_invert(curve, rel, 0.3f, &a);
            const float at_hi = calib_aim_tilt_db(&D, a.hi_deg, bh, f2);
            printf("live: 3 deg reads rel %.3f dB -> on axis %d, under %.1f deg (curve there %.3f dB)\n",
                   rel, a.on_axis, a.hi_deg, at_hi);
            CHECK(a.on_axis && a.lo_deg == 0.f, "live: a reading within tol of 0 dB is on axis");
            CHECK(a.hi_deg > 5.f && fabsf(at_hi - (rel - 0.3f)) < 0.02f, "live: 'under N deg' is where the curve reaches the reading - tol");
            CalibAimAngle b;
            calib_aim_invert(curve, 0.5f, 0.3f, &b);
            CHECK(b.ok && b.angle_deg == 0.f && b.on_axis, "live: brighter than on axis reads 0 deg");
        }
        {   /* out of range, garbage, no model */
            CalibAimAngle a;
            calib_aim_invert(curve, -200.f, 0.3f, &a);
            CHECK(a.ok && a.beyond && a.angle_deg == a.max_deg && a.max_deg > 90.f, "live: a reading past the curve is 'beyond', at max_deg");
            CHECK(!calib_aim_invert(curve, NAN, 0.3f, &a) && !a.ok, "live: a NaN reading is refused");
            static float flat[CALIB_AIM_CURVE_N];
            memset(flat, 0, sizeof flat);
            CHECK(!calib_aim_invert(flat, -1.f, 0.3f, &a), "live: a curve that never falls (no model) is refused");
            calib_aim_invert(curve, -1.f, NAN, &a);
            CHECK(a.ok && a.lo_deg == a.angle_deg && a.hi_deg == a.angle_deg, "live: a NaN tol reads as 0");
        }
        {   /* a non-monotonic curve: a +0.05 dB bump, a fall, then a rear lobe that rises again */
            static float c2[CALIB_AIM_CURVE_N];
            for (int i = 0; i < CALIB_AIM_CURVE_N; ++i) {
                if (i < 40)       c2[i] = 0.05f * sinf(3.14159265f * (float)i / 40.f);
                else if (i < 360) c2[i] = -0.01f * (float)(i - 40);       /* -3.2 dB at 90 deg */
                else              c2[i] = -3.2f + 0.005f * (float)(i - 360);
            }
            CalibAimAngle a;
            calib_aim_invert(c2, -0.1f, 0.f, &a);
            CHECK(fabsf(a.angle_deg - 12.5f) < 0.01f, "live: the bump is walked past on the running minimum");
            CHECK(fabsf(a.max_deg - 90.f) < 0.01f, "live: the range ends where the running minimum bottoms out");
            calib_aim_invert(c2, -3.5f, 0.f, &a);
            CHECK(a.beyond && a.angle_deg == a.max_deg, "live: past the rear lobe's floor reads beyond, never a rear angle");
        }
        {   /* the file's 0 deg tilt: flat on axis reads 0; a treble lift reads positive; none is refused */
            float t = 99.f;
            CHECK(!calib_on_axis_tilt_db(&D, bh, f2, &t) && t == 99.f, "live: no on_axis_db, no file tilt");
            D.has_on_axis = 1;
            for (int b = 0; b < 6; ++b) D.on_axis_db[b] = 80.f;
            CHECK(calib_on_axis_tilt_db(&D, bh, f2, &t) && fabsf(t) < 1e-5f, "live: a flat on-axis response has no tilt");
            for (int b = 0; b < 6; ++b) D.on_axis_db[b] = 80.f + (b >= 3 ? 3.f : 0.f);   /* +3 dB from 4 kHz up */
            CHECK(calib_on_axis_tilt_db(&D, bh, f2, &t) && t > 1.5f && t < 3.f, "live: a treble lift reads as a positive tilt");
            printf("live: +3 dB from 4 kHz up reads a file tilt of %+.2f dB\n", t);
        }
        {   /* the measured tilt of one capture */
            MeasureResult m; memset(&m, 0, sizeof m);
            float t = 0.f;
            m.band_direct[1] = 1.f; m.band_direct[2] = 0.5f;
            CHECK(calib_direct_tilt_db(&m, &t) && fabsf(t + 6.0206f) < 1e-3f, "live: tilt = 20 log10(high / mid)");
            m.band_direct[2] = 0.f;
            CHECK(!calib_direct_tilt_db(&m, &t), "live: an empty band has no tilt");
            m.band_direct[2] = NAN;
            CHECK(!calib_direct_tilt_db(&m, &t), "live: a NaN band has no tilt");
        }
        {   /* peak hold: holds only CLEAN readings, only once two in a row agree (to the lower of the
             * pair), ignores NaN, resets. Broken on purpose (the old one-reading rule) this goes red on
             * the latched outlier. */
            const float tol = CALIB_LIVE_TILT_TOL_DB;
            CalibPeakHold pk; calib_peak_reset(&pk);
            /* -1.0, -0.9 agree (peak -1.0); NaN skipped; -0.2 alone; -0.25 agrees with it (peak -0.25);
             * +3.0 is a clean outlier with no partner; -0.3 does not pair with +3.0 */
            const float seq[7] = { -1.0f, -0.9f, NAN, -0.2f, -0.25f, 3.0f, -0.3f };
            float below[7];
            for (int i = 0; i < 7; ++i) below[i] = calib_peak_update(&pk, seq[i], 1, tol);
            CHECK(pk.n == 6 && fabsf(pk.peak_db + 0.25f) < 1e-6f && pk.peak_index == 4,
                  "live: the peak is the lower of the best agreeing pair; a lone outlier never latches");
            CHECK(below[0] == 0.f && below[1] == 0.f, "live: 'below' is 0 before any peak");
            CHECK(below[5] == 0.f && fabsf(below[6] - 0.05f) < 1e-5f, "live: 'below' is the peak minus the reading, 0 above it");
            CHECK(fabsf(below[2] - 0.f) < 1e-6f, "live: a NaN reading repeats the last 'below'");
            /* a contaminated reading (not clean) never pairs, even when its value agrees */
            calib_peak_reset(&pk);
            CHECK(pk.n == 0 && pk.peak_index == 0 && pk.nrejected == 0, "live: reset clears the peak");
            calib_peak_update(&pk, -2.0f, 1, tol);
            calib_peak_update(&pk, 5.0f, 0, tol);
            calib_peak_update(&pk, 5.1f, 1, tol);
            CHECK(pk.peak_index == 0 && pk.nrejected == 1, "live: a rejected reading breaks the pair and is not held");
            calib_peak_update(&pk, 5.0f, 1, tol);
            CHECK(pk.peak_index == 4 && fabsf(pk.peak_db - 5.0f) < 1e-6f, "live: two clean readings that agree set the peak");
        }
        {   /* the expected-arrival window and the sweep checks (calib.h) */
            CalibWindow w; memset(&w, 0, sizeof w);
            w.lat_s = 0.010; w.pos_m = 0.5;
            const float spk[3] = { 3.43f, 0.f, 0.f }, mic[3] = { 0.f, 0.f, 0.f };
            int lo = 0, hi = 0;
            CHECK(calib_arrival_window(&w, spk, mic, 343.0, 48000.0, &lo, &hi), "window: made");
            /* 10 ms + 2.93 m / 343 = 18.542 ms -> 890.03; 10 ms + 3.93 m / 343 + 2 ms = 23.458 ms -> 1125.97, ceil + 1 */
            CHECK(lo == 890 && hi == 1127, "window: lo/hi from latency, distance and margin");
            printf("window: [%d, %d) for 3.43 m +/- 0.5 m at 10 ms\n", lo, hi);
            w.lat_late_s = 0.150;
            CHECK(calib_arrival_window(&w, spk, mic, 343.0, 48000.0, &lo, &hi) && lo == 890 && hi == 1127 + 7200,
                  "window: the driver-loop slack only moves the high edge");
            const float bad[3] = { NAN, 0.f, 0.f }, huge[3] = { 3e38f, 0.f, 0.f };
            CHECK(!calib_arrival_window(&w, bad, mic, 343.0, 48000.0, &lo, &hi), "window: NaN refused");
            CHECK(!calib_arrival_window(&w, huge, mic, 343.0, 48000.0, &lo, &hi), "window: an absurd coordinate refused");
            MeasureResult a; memset(&a, 0, sizeof a);
            a.noise_n = 1000; a.snr_db = 60.f; a.level = 1.f; a.delay_samples = 100; a.delay_frac = 0.2f;
            CHECK(calib_sweep_check(&a, 40.f) == CALIB_SWEEP_OK, "sweep: clean");
            a.snr_db = 30.f;
            CHECK(calib_sweep_check(&a, 40.f) == CALIB_SWEEP_NOISY, "sweep: under the SNR floor");
            a.noise_n = 0;
            CHECK(calib_sweep_check(&a, 40.f) == CALIB_SWEEP_OK, "sweep: an unknown SNR passes");
            a.outside = 1;
            CHECK(calib_sweep_check(&a, 40.f) == CALIB_SWEEP_OUTSIDE, "sweep: outside the window");
            MeasureResult b = a;
            b.delay_samples = 101; b.delay_frac = 0.1f; b.level = 1.02f;       /* 0.9 sample, 0.17 dB */
            float ds = 0.f, ddb = 0.f;
            CHECK(calib_sweeps_agree(&a, &b, &ds, &ddb), "agree: within a sample and 0.2 dB");
            b.level = 1.03f;                                                    /* 0.26 dB */
            CHECK(!calib_sweeps_agree(&a, &b, &ds, &ddb), "agree: 0.26 dB apart does not");
            b.level = 1.f; b.delay_frac = 0.3f;                                 /* 1.1 samples */
            CHECK(!calib_sweeps_agree(&a, &b, &ds, &ddb), "agree: 1.1 samples apart does not");
        }
    }

    /* the solved latency against the driver's loop (calib_latency_check), per mic. The driver's loop
     * here is 10 ms. A ZM-1 run on Dante Via reads about 60 ms more on the rig: a CORRECT run, so no
     * warning for it, where the same residual from an omni is an unexpected buffer. The ZM-1's bound IS
     * the arrival window's allowance, so a residual the window could not have held still warns. Below the
     * driver's loop is impossible for both. */
    {
        const double drv = 0.010;
        double r = 0.0;
        CHECK(calib_latency_check(1, drv + 0.060, drv, &r) == CALIB_LAT_OK && fabs(r - 0.060) < 1e-12,
              "latency: ZM-1, a 60 ms Via leg is ok");
        CHECK(calib_latency_check(0, drv + 0.060, drv, NULL) == CALIB_LAT_WARN, "latency: omni, 60 ms warns");
        CHECK(calib_latency_check(0, drv + 0.003, drv, NULL) == CALIB_LAT_OK, "latency: omni, 3 ms of converters is ok");
        CHECK(calib_latency_check(0, drv + 0.021, drv, NULL) == CALIB_LAT_WARN, "latency: omni, past 20 ms warns");
        CHECK(calib_latency_bound_s(1) == CALIB_WIN_LAT_DRIVER_S, "latency: the ZM-1 bound is the window's allowance");
        CHECK(calib_latency_check(1, drv + CALIB_WIN_LAT_DRIVER_S * 0.99, drv, NULL) == CALIB_LAT_OK,
              "latency: ZM-1, just inside the window's allowance");
        CHECK(calib_latency_check(1, drv + CALIB_WIN_LAT_DRIVER_S * 1.01, drv, NULL) == CALIB_LAT_WARN,
              "latency: ZM-1, past the window's allowance warns");
        for (int z = 0; z < 2; ++z) {
            CHECK(calib_latency_check(z, drv - 0.002, drv, NULL) == CALIB_LAT_IMPOSSIBLE, "latency: below the driver's loop");
            CHECK(calib_latency_check(z, drv - 0.0004, drv, NULL) == CALIB_LAT_OK, "latency: within the rounding slack");
            CHECK(calib_latency_check(z, NAN, drv, NULL) == CALIB_LAT_WARN, "latency: NaN is not ok");
        }
    }

    /* directivity re-aim under DILUTION (calib_directivity_corr with measured direct_frac): the model
     * ratio r = D(ref)/D(mic) describes the DIRECT sound, and a live room's reverberant share does not
     * follow the axis. A synthesized capture (a direct sound plus a high-passed reflection 3 ms
     * later) gives a real direct_frac f in (0, 1); the factor must then land strictly between 1 and r,
     * equal sqrt(f r^2 + 1 - f), and stay r with no measurement (f = 1). A mic at the listening
     * point is exactly 1 whatever f is. With measure.c's gate forced open to the whole IR (f = 1, so
     * the factor collapses to plain r), the partial-share and "between" checks went red. */
    {
        const double f1 = 20.0, f2 = 20000.0, bh[2] = { 300.0, 3000.0 };
        const int nref = 48000, Dd = 300, R = 144, ncap = nref + Dd + R + 9600;
        float* sw  = (float*)malloc((size_t)nref * sizeof(float));
        float* cap = (float*)calloc((size_t)ncap, sizeof(float));
        CHECK(sw && cap, "alloc (dilution)");
        if (sw && cap) {
            measure_sweep(sw, nref, f1, f2, fs);
            float lp = 0.f;
            for (int i = 0; i < nref; ++i) {
                cap[Dd + i] += 0.5f * sw[i];
                lp += 0.12f * (sw[i] - lp);
                cap[Dd + R + i] += 0.4f * (sw[i] - lp);
            }
            MeasureResult mr;
            CHECK(measure_response(cap, ncap, sw, nref, f1, f2, fs, bh, &mr), "measure_response (dilution)");
            static Layout D; layout_default(&D);
            D.dir.nband = 2; D.dir.nang = 5; D.dir.split_hz = 1000.f;
            D.dir.band_hz[0] = 250.f; D.dir.band_hz[1] = 4000.f;
            const float ang[5] = { 0, 30, 60, 90, 180 };
            for (int a = 0; a < 5; ++a) {
                D.dir.ang_deg[a] = ang[a];
                D.dir.loss_db[0][a] = -0.03f * ang[a];
                D.dir.loss_db[1][a] = -0.25f * ang[a];
            }
            directivity_derive(&D.dir);
            static MeasureResult mm[BWA_MAX_CHANNELS];
            for (uint32_t i = 0; i < D.count; ++i) mm[i] = mr;
            const float micoff[3] = { D.ref[0] + 0.9f, D.ref[1], D.ref[2] };
            float r[BWA_MAX_CHANNELS], cf[BWA_MAX_CHANNELS];
            CHECK(calib_directivity_corr(&D, micoff, 2.0 * f1, 0.5 * f2, NULL, r), "corr (free field)");
            CHECK(calib_directivity_corr(&D, micoff, 2.0 * f1, 0.5 * f2, mm, cf), "corr (diluted)");
            const double f = mr.direct_frac;
            int nlev = 0, between = 1, formula = 1; double maxr = 1.0; int imax = 0;
            for (uint32_t i = 0; i < D.count; ++i) {
                if (fabs(r[i] - 1.0) < 0.05) continue;       /* bearing barely moved: nothing to dilute */
                ++nlev;
                double lo = r[i] < 1.f ? r[i] : 1.0, hi = r[i] < 1.f ? 1.0 : r[i];
                if (!(cf[i] > lo + 1e-3 && cf[i] < hi - 1e-3)) between = 0;
                if (fabs(cf[i] - sqrt(f * r[i] * r[i] + 1.0 - f)) > 1e-5) formula = 0;
                if (fabs(log(r[i])) > fabs(log(maxr))) { maxr = r[i]; imax = (int)i; }
            }
            printf("dilution: f=%.3f; e.g. spk %d r=%+.2f dB -> corr %+.2f dB (%d speakers moved)\n",
                   f, imax, 20.0 * log10(maxr), 20.0 * log10(cf[imax]), nlev);
            CHECK(f > 0.3 && f < 0.9, "dilution: the synthesized room gives a partial direct share");
            CHECK(nlev >= 4, "dilution: the off-reference mic moves several speakers' bearings");
            CHECK(between, "dilution: every diluted factor lies strictly between 1 and the free-field ratio");
            CHECK(formula, "dilution: the factor is sqrt(f r^2 + 1 - f)");
            float cref[BWA_MAX_CHANNELS];
            calib_directivity_corr(&D, D.ref, 2.0 * f1, 0.5 * f2, mm, cref);
            int unity = 1;
            for (uint32_t i = 0; i < D.count; ++i) if (cref[i] != 1.f) unity = 0;
            CHECK(unity, "dilution: a mic AT the listening point gives exactly 1 for any direct share");
            mm[0].direct_frac = NAN;
            calib_directivity_corr(&D, micoff, 2.0 * f1, 0.5 * f2, mm, cf);
            CHECK(cf[0] == r[0], "dilution: a NaN direct share reads as 1 (the free-field factor)");
        }
        free(sw); free(cap);
    }

    {   /* the writers key on "index", not array order: records written out of order (the loader accepts
         * any order) must still get speaker k's values. Measurement i used to land on ARRAY element i,
         * so here speaker 0's trim and position went to speaker 2. */
        const char* RI = "bwa_calib_reorder.json";
        FILE* rf0 = fopen(RI, "wb");
        CHECK(rf0 != NULL, "reorder: open fixture");
        if (rf0) {
            fputs("{ \"speakers\": [\n"
                  "  { \"index\": 2, \"position\": [3,0,0] },\n"
                  "  { \"index\": 0, \"position\": [1,0,0] },\n"
                  "  { \"index\": 3, \"position\": [4,0,0] },\n"
                  "  { \"index\": 1, \"position\": [2,0,0] } ] }\n", rf0);
            fclose(rf0);
            float wg[4] = { -6.f, -3.f, -1.f, -2.f }, wd[4] = { 4.f, 2.f, 0.f, 1.f };
            float np[4][3] = { { 1.1f, 0.f, 0.f }, { 2.2f, 0.f, 0.f }, { 3.3f, 0.f, 0.f }, { 4.4f, 0.f, 0.f } };
            char e[256] = { 0 };
            CHECK(calib_write_layout(RI, RI, wg, wd, 4, e, sizeof e), e[0] ? e : "reorder: trims writer");
            CHECK(calib_write_positions(RI, RI, (const float (*)[3])np, 4, NULL, e, sizeof e), e[0] ? e : "reorder: positions writer");
            static Layout RL;                                  /* never a stack local (layout.h) */
            CHECK(layout_load(RI, 48000, &RL, e, sizeof e), e[0] ? e : "reorder: reload");
            int ok = 1;
            for (int k = 0; k < 4; ++k) {
                const float want_g = powf(10.f, wg[k] / 20.f);
                if (fabsf(RL.speakers[k].gain_lin - want_g) > 1e-4f || fabsf(RL.speakers[k].pos[0] - np[k][0]) > 1e-4f) {
                    printf("  reorder: speaker %d got gain %.4f (want %.4f), x %.3f (want %.3f)\n",
                           k, RL.speakers[k].gain_lin, want_g, RL.speakers[k].pos[0], np[k][0]);
                    ok = 0;
                }
            }
            CHECK(ok, "reorder: every writer puts speaker k's value on the record whose index is k");
            CHECK(RL.speakers[0].has_plan && fabsf(RL.speakers[0].plan_pos[0] - 1.f) < 1e-6f &&
                  fabsf(RL.speakers[2].plan_pos[0] - 3.f) < 1e-6f, "reorder: and keeps each speaker's OWN plan");
            remove(RI);
        }
    }

    {   /* a dome ABOVE the listening point: the four-unknown solve's latency is poorly pinned there, the
         * known-latency solve is not, and the dilution says which is which */
        enum { ND = 16 };
        float dome[ND][3], sphere[ND][3];
        const float x0[3] = { 0.1f, 1.448f, -0.05f };
        for (int k = 0; k < ND; ++k) {
            const double az = 2.0 * M_PI * k / ND, el = (k & 1) ? 0.35 : 0.9;    /* 20 and 52 deg up */
            dome[k][0] = x0[0] + (float)(2.0 * cos(el) * cos(az));
            dome[k][1] = x0[1] + (float)(2.0 * sin(el));
            dome[k][2] = x0[2] + (float)(2.0 * cos(el) * sin(az));
            const double es = (k & 1) ? 0.6 : -0.6;                               /* half above, half below */
            sphere[k][0] = x0[0] + (float)(2.0 * cos(es) * cos(az));
            sphere[k][1] = x0[1] + (float)(2.0 * sin(es));
            sphere[k][2] = x0[2] + (float)(2.0 * cos(es) * sin(az));
        }
        const double dd = calib_latency_dilution((const float (*)[3])dome, ND, x0);
        const double ds = calib_latency_dilution((const float (*)[3])sphere, ND, x0);
        printf("latency dilution: dome above %.2f, surrounding %.2f\n", dd, ds);
        CHECK(ds > 0.0 && ds < 1.5, "dilution: speakers surrounding the point pin the latency (about 1)");
        CHECK(dd > 2.0 * ds, "dilution: a dome above the point pins it far worse");
        /* ranges = distance + 0.5 m of latency, with +/-3.5 mm of alternating position error */
        const double LAT = 0.5;
        double rng[ND];
        for (int k = 0; k < ND; ++k) {
            const double dx = dome[k][0] - x0[0], dy = dome[k][1] - x0[1], dz = dome[k][2] - x0[2];
            rng[k] = sqrt(dx*dx + dy*dy + dz*dz) + LAT + ((k % 3) ? 0.0035 : -0.0035);
        }
        float p4[3] = { 0.f, 0.f, 0.f }; double l4 = 0.0;
        const int ok4 = calib_trilaterate(rng, (const float (*)[3])dome, ND, p4, &l4);
        const double l4lin = l4;
        const int okr = ok4 && calib_trilaterate_refine(rng, (const float (*)[3])dome, ND, p4, &l4);
        float p3[3] = { 0.f, 1.2f, 0.f };
        const int ok3 = calib_locate_known_latency(rng, (const float (*)[3])dome, ND, LAT, p3);
        const float e3 = sqrtf((p3[0]-x0[0])*(p3[0]-x0[0]) + (p3[1]-x0[1])*(p3[1]-x0[1]) + (p3[2]-x0[2])*(p3[2]-x0[2]));
        printf("dome: four unknowns: latency off %.1f mm linear, %.1f mm refined; latency known: center off %.2f mm\n",
               fabs(l4lin - LAT) * 1e3, fabs(l4 - LAT) * 1e3, e3 * 1e3);
        CHECK(ok4 && okr && ok3, "dome: the solves run");
        CHECK(fabs(l4lin - LAT) > 0.1, "dome: the LINEAR solve's latency is lost (why the refine exists)");
        CHECK(fabs(l4 - LAT) < 2.0 * dd * 0.0035, "dome: refined, the latency is within twice dilution x the range error");
        CHECK(e3 < 0.004f, "dome: with the latency known the center lands within the position error");
        float bad[3] = { 0.f, 1.2f, 0.f };
        CHECK(!calib_locate_known_latency(rng, (const float (*)[3])dome, 3, LAT, bad) && bad[1] == 1.2f,
              "dome: fewer than 4 anchors is refused and the start is untouched");
    }

    /* writeback round-trip: write a layout, apply trims, reload, confirm fields updated + dbap kept */
    const char* IN = "bwa_calib_in.json", *OUT = "bwa_calib_out.json";
    FILE* f = fopen(IN, "wb");
    CHECK(f != NULL, "open calib in.json");
    if (f) {
        fputs("{\n \"dbap\": { \"rolloff_r\": 0.5 },\n \"speakers\": [\n"
              "  { \"index\": 0, \"position\": [1,0,0], \"gain_db\": 0, \"delay_ms\": 0 },\n"
              "  { \"index\": 1, \"position\": [2,0,0], \"gain_db\": 0, \"delay_ms\": 0 },\n"
              "  { \"index\": 2, \"position\": [3,0,0], \"gain_db\": 0, \"delay_ms\": 0 }\n ]\n}\n", f);
        fclose(f);
        float wg[3] = { -6.0f, -1.5f, 0.0f }, wd[3] = { 5.83f, 2.92f, 0.0f };
        char err[256] = {0};
        CHECK(calib_write_layout(IN, OUT, wg, wd, 3, err, sizeof err), err[0] ? err : "calib_write_layout");
        long len = 0; FILE* rf = fopen(OUT, "rb");
        CHECK(rf != NULL, "reopen calib out.json");
        if (rf) {
            fseek(rf, 0, SEEK_END); len = ftell(rf); fseek(rf, 0, SEEK_SET);
            char* buf = (char*)malloc((size_t)len + 1);
            size_t rd = fread(buf, 1, (size_t)len, rf); buf[rd] = 0; fclose(rf);
            cJSON* root = cJSON_Parse(buf);
            CHECK(root != NULL, "reparse written layout");
            if (root) {
                CHECK(cJSON_GetObjectItem(root, "dbap") != NULL, "dbap block preserved through writeback");
                cJSON* sps = cJSON_GetObjectItem(root, "speakers");
                cJSON* s0 = cJSON_GetArrayItem(sps, 0);
                double g0 = cJSON_GetObjectItem(s0, "gain_db")->valuedouble;
                double d0 = cJSON_GetObjectItem(s0, "delay_ms")->valuedouble;
                CHECK(fabs(g0 - (-6.0)) < 1e-4, "speaker 0 gain_db written");
                CHECK(fabs(d0 - 5.83) < 1e-4, "speaker 0 delay_ms written");
                cJSON_Delete(root);
            }
            free(buf);
        }
        remove(IN); remove(OUT);
    }

    /* calib_write_eq: per-speaker correction taps round-trip into each speaker's "eq" array */
    {
        const char* EIN = "bwa_eq_in.json", *EOUT = "bwa_eq_out.json";
        FILE* ef = fopen(EIN, "wb");
        CHECK(ef != NULL, "open eq in.json");
        if (ef) {
            fputs("{\n \"speakers\": [\n"
                  "  { \"index\": 0, \"position\": [1,0,0] },\n"
                  "  { \"index\": 1, \"position\": [2,0,0] },\n"
                  "  { \"index\": 2, \"position\": [3,0,0] }\n ]\n}\n", ef);
            fclose(ef);
            const int MT = 4;
            float taps[12] = { 0.5f, 0.25f, -0.1f, 0.f,   0,0,0,0,   1.0f, 0,0,0 };
            uint16_t lens[3] = { 3, 0, 1 };
            char err[256] = {0};
            CHECK(calib_write_eq(EIN, EOUT, taps, lens, 3, MT, err, sizeof err), err[0] ? err : "calib_write_eq");
            FILE* rf = fopen(EOUT, "rb");
            if (rf) {
                fseek(rf, 0, SEEK_END); long len = ftell(rf); fseek(rf, 0, SEEK_SET);
                char* buf = (char*)malloc((size_t)len + 1); size_t rd = fread(buf, 1, (size_t)len, rf); buf[rd] = 0; fclose(rf);
                cJSON* root = cJSON_Parse(buf);
                CHECK(root != NULL, "reparse written eq layout");
                if (root) {
                    cJSON* sps = cJSON_GetObjectItem(root, "speakers");
                    cJSON* e0 = cJSON_GetObjectItem(cJSON_GetArrayItem(sps, 0), "eq");
                    cJSON* e1 = cJSON_GetObjectItem(cJSON_GetArrayItem(sps, 1), "eq");
                    cJSON* e2 = cJSON_GetObjectItem(cJSON_GetArrayItem(sps, 2), "eq");
                    CHECK(cJSON_IsArray(e0) && cJSON_GetArraySize(e0) == 3, "speaker 0 eq has 3 taps");
                    CHECK(e0 && fabs(cJSON_GetArrayItem(e0, 1)->valuedouble - 0.25) < 1e-6, "eq tap value round-trips");
                    CHECK(e1 == NULL, "speaker 1 (length 0) gets no eq array");
                    CHECK(cJSON_IsArray(e2) && cJSON_GetArraySize(e2) == 1, "speaker 2 eq has 1 tap");
                    cJSON_Delete(root);
                }
                free(buf);
            }
            remove(EIN); remove(EOUT);
        }
    }

    /* calib_write_room_eq: LF modal cuts round-trip into each speaker's "room_eq" section array */
    {
        const char* RIN = "bwa_rq_in.json", *ROUT = "bwa_rq_out.json";
        FILE* rfw = fopen(RIN, "wb");
        CHECK(rfw != NULL, "open room_eq in.json");
        if (rfw) {
            fputs("{\n \"speakers\": [\n"
                  "  { \"index\": 0, \"position\": [1,0,0] },\n"
                  "  { \"index\": 1, \"position\": [2,0,0] }\n ]\n}\n", rfw);
            fclose(rfw);
            MeasureEqSection cuts[2 * BWA_ROOM_EQ_MAX];
            memset(cuts, 0, sizeof cuts);
            cuts[0].fc = 62.5f; cuts[0].gain_db = -6.2f; cuts[0].q = 4.3f;   /* speaker 0: two cuts */
            cuts[1].fc = 118.f; cuts[1].gain_db = -3.5f; cuts[1].q = 2.0f;
            int counts[2] = { 2, 0 };                                        /* speaker 1: none */
            char err[256] = {0};
            CHECK(calib_write_room_eq(RIN, ROUT, cuts, counts, 2, BWA_ROOM_EQ_MAX, err, sizeof err),
                  err[0] ? err : "calib_write_room_eq");
            FILE* rf = fopen(ROUT, "rb");
            if (rf) {
                fseek(rf, 0, SEEK_END); long len = ftell(rf); fseek(rf, 0, SEEK_SET);
                char* buf = (char*)malloc((size_t)len + 1); size_t rd = fread(buf, 1, (size_t)len, rf); buf[rd] = 0; fclose(rf);
                cJSON* root = cJSON_Parse(buf);
                CHECK(root != NULL, "reparse written room_eq layout");
                if (root) {
                    cJSON* sps = cJSON_GetObjectItem(root, "speakers");
                    cJSON* r0  = cJSON_GetObjectItem(cJSON_GetArrayItem(sps, 0), "room_eq");
                    cJSON* r1  = cJSON_GetObjectItem(cJSON_GetArrayItem(sps, 1), "room_eq");
                    CHECK(cJSON_IsArray(r0) && cJSON_GetArraySize(r0) == 2, "speaker 0 room_eq has 2 sections");
                    if (cJSON_IsArray(r0) && cJSON_GetArraySize(r0) == 2) {
                        cJSON* s0 = cJSON_GetArrayItem(r0, 0);
                        CHECK(fabs(cJSON_GetObjectItem(s0, "fc")->valuedouble      - 62.5) < 0.11 &&
                              fabs(cJSON_GetObjectItem(s0, "gain_db")->valuedouble + 6.2)  < 0.011 &&
                              fabs(cJSON_GetObjectItem(s0, "q")->valuedouble       - 4.3)  < 0.011,
                              "room_eq section values round-trip");
                    }
                    CHECK(r1 == NULL, "a speaker with no cuts gets no room_eq array");
                    cJSON_Delete(root);
                }
                free(buf);
            }
            /* same refusal policy as the trims writer: a NaN fit must be refused BEFORE the write —
             * serialized it becomes JSON `null`, and out_path is usually the layout itself */
            cuts[1].gain_db = NAN;
            err[0] = 0;
            CHECK(!calib_write_room_eq(RIN, ROUT, cuts, counts, 2, BWA_ROOM_EQ_MAX, err, sizeof err) && err[0],
                  "a NaN room_eq section is refused with a reason");
            cuts[1].gain_db = -3.5f;
            remove(RIN); remove(ROUT);
        }
    }

    /* calib_room_grid_merge: the same mode read from two positions clusters into ONE ladder section
     * (fcs within tolerance), keeping each position's own depth; a mode only one position saw gets a
     * 0 dB filler at the other (congruence). */
    {
        MeasureEqSection cuts[2 * BWA_ROOM_EQ_MAX];
        memset(cuts, 0, sizeof cuts);
        cuts[0].fc = 44.8f; cuts[0].gain_db = -8.f; cuts[0].q = 6.f;   /* position 0: the 45 Hz mode + 120 Hz */
        cuts[1].fc = 118.f; cuts[1].gain_db = -5.f; cuts[1].q = 4.f;
        cuts[BWA_ROOM_EQ_MAX + 0].fc = 45.9f;                            /* position 1: the same 45 Hz mode, 2% off */
        cuts[BWA_ROOM_EQ_MAX + 0].gain_db = -4.f;
        cuts[BWA_ROOM_EQ_MAX + 0].q = 8.f;
        int counts[2] = { 2, 1 };
        float fc[BWA_ROOM_EQ_MAX], q[BWA_ROOM_EQ_MAX], g[2 * BWA_ROOM_EQ_MAX];
        int lad = calib_room_grid_merge(cuts, counts, 2, BWA_ROOM_EQ_MAX, 0.08, BWA_ROOM_EQ_MAX, fc, q, g);
        CHECK(lad == 2, "two positions merge into a 2-section ladder (shared mode clustered)");
        if (lad == 2) {
            CHECK(fc[0] > 44.f && fc[0] < 46.f && fabs(q[0] - 7.f) < 0.01, "cluster takes the member-median fc/q");
            CHECK(fabs(g[0*BWA_ROOM_EQ_MAX + 0] + 8.f) < 1e-4 && fabs(g[1*BWA_ROOM_EQ_MAX + 0] + 4.f) < 1e-4,
                  "each position keeps its own depth for the shared mode");
            CHECK(fabs(fc[1] - 118.f) < 1e-3 && fabs(g[0*BWA_ROOM_EQ_MAX + 1] + 5.f) < 1e-4,
                  "the position-0-only mode survives");
            CHECK(g[1*BWA_ROOM_EQ_MAX + 1] == 0.f, "the position that missed a mode gets a 0 dB filler");
        }
    }

    /* calib_write_room_eq_grid: one run per mic placement ACCUMULATES the grid — the second position
     * appends, a rerun within 5 cm replaces, and the static room_eq is removed (mutually exclusive). */
    {
        const char* GIN = "bwa_grid_in.json";
        FILE* gf = fopen(GIN, "wb");
        CHECK(gf != NULL, "open grid in.json");
        if (gf) {
            fputs("{\n \"speakers\": [\n"
                  "  { \"index\": 0, \"position\": [1,0,0], \"room_eq\": [{\"fc\":80,\"gain_db\":-6,\"q\":4}] },\n"
                  "  { \"index\": 1, \"position\": [2,0,0] }\n ]\n}\n", gf);
            fclose(gf);
            MeasureEqSection cuts[2 * BWA_ROOM_EQ_MAX];
            int counts[2];
            char err[256] = {0};
            memset(cuts, 0, sizeof cuts);                              /* run 1: mic A sees 45 Hz on speaker 0 */
            cuts[0].fc = 45.f; cuts[0].gain_db = -8.f; cuts[0].q = 6.f;
            counts[0] = 1; counts[1] = 0;
            float micA[3] = { -0.5f, 1.5f, 0.f }, micB[3] = { 0.5f, 1.5f, 0.f };
            CHECK(calib_write_room_eq_grid(GIN, GIN, micA, cuts, counts, 2, BWA_ROOM_EQ_MAX, err, sizeof err),
                  err[0] ? err : "grid writeback run 1");
            memset(cuts, 0, sizeof cuts);                              /* run 2: mic B reads the mode shallower */
            cuts[0].fc = 45.5f; cuts[0].gain_db = -3.f; cuts[0].q = 6.f;
            counts[0] = 1; counts[1] = 0;
            CHECK(calib_write_room_eq_grid(GIN, GIN, micB, cuts, counts, 2, BWA_ROOM_EQ_MAX, err, sizeof err),
                  err[0] ? err : "grid writeback run 2");
            memset(cuts, 0, sizeof cuts);                              /* run 3: B re-measured (2 cm off: replaces) */
            cuts[0].fc = 45.5f; cuts[0].gain_db = -4.f; cuts[0].q = 6.f;
            counts[0] = 1; counts[1] = 0;
            float micB2[3] = { 0.52f, 1.5f, 0.f };
            CHECK(calib_write_room_eq_grid(GIN, GIN, micB2, cuts, counts, 2, BWA_ROOM_EQ_MAX, err, sizeof err),
                  err[0] ? err : "grid writeback run 3");
            /* the grid writer's schema clamps are two-sided compares that pass NaN: the refusal
             * must fire first, and the accumulated grid file must survive untouched */
            memset(cuts, 0, sizeof cuts);
            cuts[0].fc = 45.5f; cuts[0].gain_db = NAN; cuts[0].q = 6.f;
            counts[0] = 1; counts[1] = 0;
            err[0] = 0;
            CHECK(!calib_write_room_eq_grid(GIN, GIN, micB2, cuts, counts, 2, BWA_ROOM_EQ_MAX, err, sizeof err) && err[0],
                  "a NaN grid section is refused with a reason");
            FILE* rf = fopen(GIN, "rb");
            if (rf) {
                fseek(rf, 0, SEEK_END); long len = ftell(rf); fseek(rf, 0, SEEK_SET);
                char* buf = (char*)malloc((size_t)len + 1); size_t rd = fread(buf, 1, (size_t)len, rf); buf[rd] = 0; fclose(rf);
                cJSON* root = cJSON_Parse(buf);
                CHECK(root != NULL, "reparse written grid layout");
                if (root) {
                    cJSON* sps = cJSON_GetObjectItem(root, "speakers");
                    CHECK(cJSON_GetObjectItem(cJSON_GetArrayItem(sps, 0), "room_eq") == NULL,
                          "static room_eq removed by the grid writeback");
                    cJSON* grid = cJSON_GetObjectItem(root, "room_eq_grid");
                    CHECK(cJSON_IsArray(grid) && cJSON_GetArraySize(grid) == 2,
                          "three runs at two spots leave two grid positions (rerun replaced)");
                    if (cJSON_IsArray(grid) && cJSON_GetArraySize(grid) == 2) {
                        cJSON* eA = cJSON_GetArrayItem(grid, 0);
                        cJSON* eB = cJSON_GetArrayItem(grid, 1);
                        cJSON* sA = cJSON_GetArrayItem(cJSON_GetObjectItem(eA, "speakers"), 0);
                        cJSON* sB = cJSON_GetArrayItem(cJSON_GetObjectItem(eB, "speakers"), 0);
                        CHECK(cJSON_GetArraySize(sA) == 1 && cJSON_GetArraySize(sB) == 1,
                              "both positions carry the merged 1-section ladder");
                        double gA = cJSON_GetObjectItem(cJSON_GetArrayItem(sA, 0), "gain_db")->valuedouble;
                        double gB = cJSON_GetObjectItem(cJSON_GetArrayItem(sB, 0), "gain_db")->valuedouble;
                        CHECK(fabs(gA + 8.0) < 0.011, "position A keeps its depth through the re-merge");
                        CHECK(fabs(gB + 4.0) < 0.011, "the rerun's depth replaced the stale entry");
                        double fB = cJSON_GetObjectItem(cJSON_GetArrayItem(sB, 0), "fc")->valuedouble;
                        double fA = cJSON_GetObjectItem(cJSON_GetArrayItem(sA, 0), "fc")->valuedouble;
                        CHECK(fabs(fA - fB) < 1e-6, "positions share one fc ladder (congruent for the loader)");
                        cJSON* s1A = cJSON_GetArrayItem(cJSON_GetObjectItem(eA, "speakers"), 1);
                        CHECK(cJSON_GetArraySize(s1A) == 0, "a speaker with no modes gets an empty ladder");
                    }
                    cJSON_Delete(root);
                }
                free(buf);
            }
            remove(GIN);
        }
    }

    /* self-localization: recover a speaker position + the system latency from ranges (c*delay,
     * latency included) to 6 non-coplanar mic positions. */
    {
        float xt[3] = { 1.2f, 0.5f, 0.8f };
        float micp[6][3] = { {0,0,0}, {2,0,0}, {0,2,0}, {0,0,2}, {2,2,0.5f}, {1,1,1.6f} };
        double ctau = 3.66;                                    /* latency range (c*tau) */
        double range[6];
        for (int k = 0; k < 6; ++k) {
            double dx = xt[0]-micp[k][0], dy = xt[1]-micp[k][1], dz = xt[2]-micp[k][2];
            range[k] = ctau + sqrt(dx*dx + dy*dy + dz*dz);
        }
        float pos[3]; double lat = 0;
        CHECK(calib_trilaterate(range, micp, 6, pos, &lat), "calib_trilaterate solves");
        printf("trilat: pos=(%.3f %.3f %.3f) want (1.20 0.50 0.80)  latency=%.3f want 3.66\n", pos[0], pos[1], pos[2], lat);
        CHECK(fabs(pos[0]-1.2) < 0.01 && fabs(pos[1]-0.5) < 0.01 && fabs(pos[2]-0.8) < 0.01, "recovers the speaker position");
        CHECK(fabs(lat - 3.66) < 0.01, "recovers the system latency jointly");
        CHECK(!calib_trilaterate(range, micp, 4, pos, &lat), "rejects fewer than 5 mic positions");
    }

    /* position writeback round-trip */
    {
        const char* IN = "bwa_pos_in.json", *OUT = "bwa_pos_out.json";
        FILE* f = fopen(IN, "wb");
        CHECK(f != NULL, "open pos in.json");
        if (f) {
            fputs("{ \"speakers\": [ {\"index\":0,\"position\":[0,0,0]}, "
                  "{\"index\":1,\"position\":[0,0,0]}, {\"index\":2,\"position\":[0,0,0]} ] }", f);
            fclose(f);
            float pp[3][3] = { {1.25f, -0.5f, 0.8f}, {2.f, 0.f, 0.f}, {0.f, 2.f, 0.f} };
            char e[256] = {0};
            CHECK(calib_write_positions(IN, OUT, pp, 3, NULL, e, sizeof e), e[0] ? e : "calib_write_positions");
            FILE* rf = fopen(OUT, "rb");
            if (rf) {
                fseek(rf, 0, SEEK_END); long len = ftell(rf); fseek(rf, 0, SEEK_SET);
                char* buf = (char*)malloc((size_t)len + 1); size_t rd = fread(buf, 1, (size_t)len, rf); buf[rd] = 0; fclose(rf);
                cJSON* root = cJSON_Parse(buf);
                CHECK(root != NULL, "reparse written positions");
                if (root) {
                    cJSON* s0 = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "speakers"), 0);
                    cJSON* p0 = cJSON_GetObjectItem(s0, "position");
                    CHECK(fabs(cJSON_GetArrayItem(p0,0)->valuedouble - 1.25) < 1e-4 &&
                          fabs(cJSON_GetArrayItem(p0,1)->valuedouble - (-0.5)) < 1e-4 &&
                          fabs(cJSON_GetArrayItem(p0,2)->valuedouble - 0.8) < 1e-4, "speaker 0 position written");
                    cJSON_Delete(root);
                }
                free(buf);
            }
            remove(IN); remove(OUT);
        }
    }

    /* ---- the plan (plan_position / plan_aim, docs/layout-schema.md "Plan versus as-built") ----
     * The loader keeps it apart from position (the engine renders the as-built), every writer carries it
     * through, and a POSITION writer records it once: the first survey keeps what it replaces, a second
     * survey leaves it alone. The expected numbers are typed here, never read back from a writer. */
    {
        const char* PIN = "bwa_plan_in.json", *POUT = "bwa_plan_out.json";
        FILE* f = fopen(PIN, "wb");
        CHECK(f != NULL, "open plan in.json");
        if (f) {
            fputs("{ \"listening_point_m\": [0, 1.5, 0], \"note\": \"kept\", \"speakers\": [\n"
                  "  {\"index\":0,\"position\":[1.5,0,0],\"aim\":[0,1,0],\"plan_position\":[1.4,0.1,0],\"plan_aim\":[-2,2,0],"
                  "\"gain_db\":-1,\"delay_ms\":0.5},\n"
                  "  {\"index\":1,\"position\":[-1.5,0,0],\"plan_position\":[-1.5,0.2,0]},\n"
                  "  {\"index\":2,\"position\":[0,0,1.5]},\n"
                  "  {\"index\":3,\"position\":[0,3,0],\"aim\":[0,-1,0]} ] }\n", f);
            fclose(f);
            static Layout PL;                                  /* never a stack local (layout.h) */
            char e[256] = {0};
            CHECK(layout_load(PIN, 48000, &PL, e, sizeof e), e[0] ? e : "load the plan fixture");
            const float r2 = 0.70710678f;
            CHECK(PL.speakers[0].pos[0] == 1.5f && PL.speakers[0].aim[1] == 1.f, "plan: the engine's position and aim are the as-built ones");
            CHECK(PL.speakers[0].has_plan && fabsf(PL.speakers[0].plan_pos[0] - 1.4f) < 1e-6f && fabsf(PL.speakers[0].plan_pos[1] - 0.1f) < 1e-6f,
                  "plan: plan_position loads");
            CHECK(fabsf(PL.speakers[0].plan_aim[0] + r2) < 1e-5f && fabsf(PL.speakers[0].plan_aim[1] - r2) < 1e-5f,
                  "plan: plan_aim loads, normalized");
            {   /* speaker 1: a plan with no plan_aim faces the listening point FROM the plan position */
                const float dx = 1.5f, dy = 1.3f, inv = 1.f / sqrtf(dx * dx + dy * dy);
                CHECK(PL.speakers[1].has_plan && fabsf(PL.speakers[1].plan_aim[0] - dx * inv) < 1e-5f &&
                      fabsf(PL.speakers[1].plan_aim[1] - dy * inv) < 1e-5f, "plan: no plan_aim = toward the listening point from plan_position");
            }
            CHECK(!PL.speakers[2].has_plan && layout_plan_pos(&PL, 2) == PL.speakers[2].pos && layout_plan_aim(&PL, 2) == PL.speakers[2].aim,
                  "plan: a record with no plan is its own plan");
            CHECK(layout_plan_pos(&PL, 0) == PL.speakers[0].plan_pos, "plan: the accessor returns the plan when there is one");

            /* every non-position writer carries it through untouched */
            float wg[4] = { -1.f, 0.f, -2.f, 0.f }, wd[4] = { 0.5f, 0.f, 0.1f, 0.f };
            CHECK(calib_write_layout(PIN, POUT, wg, wd, 4, e, sizeof e), e[0] ? e : "plan: trims writer");
            CHECK(calib_write_sos(POUT, POUT, 344.0, e, sizeof e), e[0] ? e : "plan: sos writer");
            float taps[4 * 2] = { 1.f, 0.f, 1.f, 0.f, 1.f, 0.f, 1.f, 0.f };
            uint16_t lens[4] = { 2, 2, 2, 2 };
            CHECK(calib_write_eq(POUT, POUT, taps, lens, 4, 2, e, sizeof e), e[0] ? e : "plan: eq writer");
            MeasureEqSection gc[4 * BWA_ROOM_EQ_MAX]; int gn[4] = { 1, 0, 0, 0 };
            memset(gc, 0, sizeof gc); gc[0].fc = 50.f; gc[0].gain_db = -4.f; gc[0].q = 5.f;
            const float gm[3] = { 0.f, 1.5f, 0.f };
            CHECK(calib_write_room_eq_grid(POUT, POUT, gm, gc, gn, 4, BWA_ROOM_EQ_MAX, e, sizeof e), e[0] ? e : "plan: grid writer");
            CHECK(layout_load(POUT, 48000, &PL, e, sizeof e), e[0] ? e : "plan: reload after the writers");
            CHECK(PL.speakers[0].has_plan && fabsf(PL.speakers[0].plan_pos[0] - 1.4f) < 1e-6f &&
                  fabsf(PL.speakers[0].plan_aim[0] + r2) < 1e-5f && PL.speakers[1].has_plan &&
                  !PL.speakers[2].has_plan && !PL.speakers[3].has_plan, "plan: survives the trims, sos, eq and grid writers");
            CHECK(PL.speakers[0].pos[0] == 1.5f, "plan: those writers leave the position alone");

            /* the FIRST survey: speakers 2 and 3 had no plan and get one (their old position, and 3's old
             * aim); 0 and 1 keep theirs */
            float s1[4][3] = { { 1.52f, 0.01f, 0.f }, { -1.48f, 0.02f, 0.f }, { 0.03f, 0.f, 1.47f }, { 0.f, 2.96f, 0.02f } };
            int nrec = -1;
            CHECK(calib_write_positions(POUT, POUT, (const float (*)[3])s1, 4, &nrec, e, sizeof e), e[0] ? e : "plan: first survey");
            CHECK(nrec == 2, "plan: the first survey records a plan for the two speakers that had none");
            CHECK(layout_load(POUT, 48000, &PL, e, sizeof e), e[0] ? e : "plan: reload after the first survey");
            CHECK(fabsf(PL.speakers[2].pos[2] - 1.47f) < 1e-6f, "plan: the survey wrote the position");
            CHECK(PL.speakers[2].has_plan && PL.speakers[2].plan_pos[0] == 0.f && fabsf(PL.speakers[2].plan_pos[2] - 1.5f) < 1e-6f,
                  "plan: the first survey kept the old position as plan_position");
            CHECK(PL.speakers[3].has_plan && fabsf(PL.speakers[3].plan_pos[1] - 3.f) < 1e-6f && fabsf(PL.speakers[3].plan_aim[1] + 1.f) < 1e-6f,
                  "plan: ...and the old explicit aim as plan_aim");
            CHECK(fabsf(PL.speakers[0].plan_pos[0] - 1.4f) < 1e-6f && fabsf(PL.speakers[0].plan_pos[1] - 0.1f) < 1e-6f,
                  "plan: an existing plan is not touched by the first survey");
            /* the SECOND survey: nothing recorded, the plan still the pre-survey values */
            float s2[4][3] = { { 1.6f, 0.f, 0.f }, { -1.6f, 0.f, 0.f }, { 0.1f, 0.f, 1.6f }, { 0.f, 2.9f, 0.1f } };
            CHECK(calib_write_positions(POUT, POUT, (const float (*)[3])s2, 4, &nrec, e, sizeof e), e[0] ? e : "plan: second survey");
            CHECK(nrec == 0, "plan: the second survey records nothing");
            CHECK(layout_load(POUT, 48000, &PL, e, sizeof e), e[0] ? e : "plan: reload after the second survey");
            CHECK(fabsf(PL.speakers[2].pos[0] - 0.1f) < 1e-6f, "plan: the second survey wrote its positions");
            CHECK(PL.speakers[2].plan_pos[0] == 0.f && fabsf(PL.speakers[2].plan_pos[2] - 1.5f) < 1e-6f,
                  "plan: the second survey did not overwrite the plan with the first survey's positions");
            {   /* the free text survives it all */
                FILE* rf = fopen(POUT, "rb");
                if (rf) {
                    char buf[8192]; size_t rd = fread(buf, 1, sizeof buf - 1, rf); buf[rd] = 0; fclose(rf);
                    CHECK(strstr(buf, "\"note\":") != NULL, "plan: unknown fields survive every writer");
                }
            }

            /* malformed plans are load errors with a reason, like position's */
            const char* bad[][2] = {
                { "\"plan_position\":[1,2]",                      "plan_position" },
                { "\"plan_position\":[1e400,0,0]",                "plan_position" },   /* parses to inf */
                { "\"plan_position\":[2000,0,0]",                 "plan_position" },   /* position's +/-1000 m */
                { "\"plan_position\":[0,\"a\",0]",                "plan_position" },
                { "\"plan_position\":[1,0,0],\"plan_aim\":[0,0,0]", "plan_aim" },
                { "\"plan_position\":[1,0,0],\"plan_aim\":[0,1]",   "plan_aim" },
                { "\"plan_aim\":[0,1,0]",                         "plan_aim needs plan_position" },
            };
            for (size_t b = 0; b < sizeof bad / sizeof bad[0]; ++b) {
                FILE* bf = fopen(PIN, "wb");
                if (!bf) continue;
                fprintf(bf, "{ \"speakers\": [ {\"index\":0,\"position\":[1,0,0],%s}, {\"index\":1,\"position\":[-1,0,0]},"
                            " {\"index\":2,\"position\":[0,0,1]}, {\"index\":3,\"position\":[0,1,0]} ] }\n", bad[b][0]);
                fclose(bf);
                e[0] = 0;
                const int ok = layout_load(PIN, 48000, &PL, e, sizeof e);
                if (ok || !strstr(e, bad[b][1])) printf("  plan refusal %d: ok=%d err='%s'\n", (int)b, ok, e);
                CHECK(!ok && strstr(e, bad[b][1]) != NULL, "plan: a malformed plan is a load error naming the field");
            }
            remove(PIN); remove(POUT);
        }
    }

    /* calib_write_room_eq_grid_n: several positions in one write. Two within the 5 cm replace radius
     * collapse to one entry (the later wins, as with one call each); a set that overflows the 16-position
     * grid fails BEFORE the file is written. */
    {
        const char* GN = "bwa_gridn_in.json", *GO = "bwa_gridn_out.json";
        FILE* f = fopen(GN, "wb");
        CHECK(f != NULL, "open gridn in.json");
        if (f) {
            fputs("{ \"speakers\": [ {\"index\":0,\"position\":[1,0,0]}, {\"index\":1,\"position\":[2,0,0]}, "
                  "{\"index\":2,\"position\":[0,0,1]}, {\"index\":3,\"position\":[0,1,0]} ] }\n", f);
            fclose(f);
            char e[256] = {0};
            float mics[17][3];
            for (int k = 0; k < 17; ++k) { mics[k][0] = 0.3f * k; mics[k][1] = 1.5f; mics[k][2] = 0.f; }
            mics[2][0] = mics[1][0] + 0.02f;                   /* 2 cm from mic 1: replaces it */
            MeasureEqSection* c = (MeasureEqSection*)calloc((size_t)17 * 4 * BWA_ROOM_EQ_MAX, sizeof *c);
            int* cn = (int*)calloc((size_t)17 * 4, sizeof(int));
            for (int k = 0; k < 17 && c && cn; ++k) {           /* speaker 0 sees a 50 Hz mode, depth by row */
                c[((size_t)k * 4) * BWA_ROOM_EQ_MAX].fc = 50.f;
                c[((size_t)k * 4) * BWA_ROOM_EQ_MAX].gain_db = -1.f - 0.5f * k;
                c[((size_t)k * 4) * BWA_ROOM_EQ_MAX].q = 5.f;
                cn[k * 4] = 1;
            }
            if (c && cn) {
                remove(GO);
                CHECK(calib_write_room_eq_grid_n(GN, GO, 3, (const float (*)[3])mics, c, cn, 4, BWA_ROOM_EQ_MAX, e, sizeof e),
                      e[0] ? e : "grid_n: three positions");
                static Layout GL;
                CHECK(layout_load(GO, 48000, &GL, e, sizeof e), e[0] ? e : "grid_n: reload");
                CHECK(GL.rq_grid.npos == 2, "grid_n: two positions within 5 cm collapse into one entry");
                CHECK(fabsf(GL.rq_grid.pos[1][0] - mics[2][0]) < 1e-3f && fabsf(GL.rq_grid.gain_db[1][0][0] + 2.f) < 0.011f,
                      "grid_n: the later of the two wins, key and depth");
                remove(GO);
                e[0] = 0;
                mics[2][0] = 0.3f * 2;                          /* 17 distinct positions: one too many */
                CHECK(!calib_write_room_eq_grid_n(GN, GO, 17, (const float (*)[3])mics, c, cn, 4, BWA_ROOM_EQ_MAX, e, sizeof e) &&
                      strstr(e, "full") != NULL, "grid_n: 17 positions are refused");
                FILE* g = fopen(GO, "rb");
                CHECK(g == NULL, "grid_n: a refused set writes nothing");
                if (g) fclose(g);
                CHECK(calib_write_room_eq_grid_n(GN, GO, 16, (const float (*)[3])mics, c, cn, 4, BWA_ROOM_EQ_MAX, e, sizeof e),
                      e[0] ? e : "grid_n: 16 positions fit");
                CHECK(layout_load(GO, 48000, &GL, e, sizeof e) && GL.rq_grid.npos == 16, "grid_n: 16 positions load");
            }
            free(c); free(cn);
            remove(GN); remove(GO);
        }
    }

    /* drift check: one nudged speaker shows its deviation; the rest stay ~0 (latency removed) */
    {
        float pos[5][3] = { {1,0,0}, {0,1.5f,0}, {-1,0,0.5f}, {0,0,2}, {1,1,1} };
        float mic[3] = { 0,0,0 };
        double lat = 3.66, range[5];
        for (int s = 0; s < 5; ++s) {
            double d = sqrt((double)pos[s][0]*pos[s][0] + pos[s][1]*pos[s][1] + pos[s][2]*pos[s][2]);
            range[s] = lat + d;
        }
        range[2] += 0.05;                                      /* speaker 2 bumped 5 cm farther */
        float dev[5];
        calib_check_drift(range, pos, mic, 5, dev);
        printf("drift: dev=[%.3f %.3f %.3f %.3f %.3f]\n", dev[0], dev[1], dev[2], dev[3], dev[4]);
        CHECK(fabs(dev[2] - 0.05) < 0.005, "flags the 5 cm nudge on speaker 2");
        CHECK(fabs(dev[0]) < 0.005 && fabs(dev[1]) < 0.005 && fabs(dev[3]) < 0.005 && fabs(dev[4]) < 0.005,
              "unmoved speakers read ~0 (common latency removed by the median)");
    }

    /* ---- speed of sound: the temperature parsers + the layout round trip (sos.h, calib_*_sos) ---- */
    {
        double v = 0.0;
        CHECK(sos_parse_temp("20", &v)   && fabs(v - 343.42) < 0.01, "bare number parses as Celsius");
        CHECK(sos_parse_temp("20C", &v)  && fabs(v - 343.42) < 0.01, "C suffix");
        CHECK(sos_parse_temp("20c", &v)  && fabs(v - 343.42) < 0.01, "lowercase c suffix");
        CHECK(sos_parse_temp("73F", &v)  && fabs(v - 345.10) < 0.01, "F suffix converts (73 F = 22.78 C)");
        CHECK(sos_parse_temp("-10", &v)  && fabs(v - 325.24) < 0.01, "negative Celsius (a cold room)");
        /* rejects: no number, trailing junk, and out-of-guard values in BOTH directions */
        CHECK(!sos_parse_temp("", &v)     , "empty rejected");
        CHECK(!sos_parse_temp("abc", &v)  , "non-numeric rejected");
        CHECK(!sos_parse_temp("20X", &v)  , "unknown suffix rejected, not silently Celsius");
        CHECK(!sos_parse_temp("20CC", &v) , "trailing junk after a valid suffix rejected");
        CHECK(!sos_parse_temp("730", &v)  , "730 C rejected (the fat-finger case)");
        CHECK(!sos_parse_temp("-100", &v) , "-100 C rejected");
        CHECK(sos_parse_mps("345.1", &v) && fabs(v - 345.1) < 1e-9, "direct c parses");
        CHECK(!sos_parse_mps("5", &v)     , "5 m/s rejected");
        CHECK(!sos_parse_mps("345.1x", &v), "trailing junk on --c rejected");
        CHECK(!sos_parse_mps("", &v)      , "empty --c rejected");

        /* round trip through a layout file, including the case the field does not exist yet */
        const char* p = "calib_sos_rt.json";
        FILE* f = fopen(p, "wb");
        CHECK(f != NULL, "open the sos round-trip fixture");
        if (f) {
            fprintf(f, "{\n  \"reference\": { \"alignment\": \"max-distance\", \"ears_m\": 1.2 },\n"
                       "  \"note_kept\": \"unknown fields must survive\",\n"
                       "  \"speakers\": []\n}\n");
            fclose(f);
            double got = 0.0;
            CHECK(!calib_read_sos(p, &got), "a file with no speed_of_sound_mps reads as absent");
            char err[256] = {0};
            CHECK(calib_write_sos(p, p, 345.1, err, sizeof err), "write into an existing reference block");
            CHECK(calib_read_sos(p, &got) && fabs(got - 345.1) < 0.01, "reads back what was written");
            /* non-destructive, like every other calib_write_*: siblings and unknown keys survive */
            char* txt = NULL; long len = 0;
            FILE* rf = fopen(p, "rb");
            if (rf) { fseek(rf, 0, SEEK_END); len = ftell(rf); fseek(rf, 0, SEEK_SET);
                      txt = (char*)malloc((size_t)len + 1);
                      if (txt) { size_t rd = fread(txt, 1, (size_t)len, rf); txt[rd] = '\0'; }
                      fclose(rf); }
            CHECK(txt && strstr(txt, "note_kept")  != NULL, "unknown top-level field survives");
            CHECK(txt && strstr(txt, "ears_m")     != NULL, "the sibling ears_m survives");
            CHECK(txt && strstr(txt, "max-distance") != NULL, "the sibling alignment survives");
            free(txt);
            CHECK(!calib_write_sos(p, p, 5.0, err, sizeof err), "out-of-range c refused, file untouched");
            CHECK(calib_read_sos(p, &got) && fabs(got - 345.1) < 0.01, "refused write left the old value");
            remove(p);
        }
        CHECK(!calib_read_sos("no_such_layout_file.json", &v), "missing file reads as absent, no crash");
    }

    /* ---- --verify's residual math (calib_verify_residuals) ----
     * Five speakers around a mic, measured THROUGH trims that align them at the mic: every arrival is
     * latency + the farthest flight time, every level sens / d with one sensitivity. Then break one
     * delay by 0.5 ms and one gain by 3 dB and require exactly those two flags. */
    {
        const double vfs = 48000.0, vc = 343.0, lat = 0.060;
        const float vpos[5][3] = { {2,1.5,0}, {-1.5,2.5,1}, {0,0.2,-2.5}, {1,3,1.8}, {-2.2,1,-1} };
        const float vmic[3] = { 0.1f, 1.4f, 0.2f };
        double d[5], dmax = 0.0;
        for (int k = 0; k < 5; ++k) {
            const double dx = vpos[k][0]-vmic[0], dy = vpos[k][1]-vmic[1], dz = vpos[k][2]-vmic[2];
            d[k] = sqrt(dx*dx + dy*dy + dz*dz); if (d[k] > dmax) dmax = d[k];
        }
        MeasureResult vm[5];
        #define SET_ARR(k, t) do { const double s_ = (t) * vfs; vm[k].delay_samples = (int)floor(s_ + 0.5); \
                                   vm[k].delay_frac = (float)(s_ - floor(s_ + 0.5)); } while (0)
        memset(vm, 0, sizeof vm);
        for (int k = 0; k < 5; ++k) {
            SET_ARR(k, lat + d[k] / vc + (dmax - d[k]) / vc);    /* flight + the trim's delay */
            vm[k].level = (float)(0.7 / d[k]);
        }
        float aus[5], ldb[5]; int flg[5]; CalibVerifySummary vs;
        int nf = calib_verify_residuals(vm, vpos, vmic, vmic, 5, vfs, vc, NULL, aus, ldb, flg, &vs);
        int clean = nf == 0 && vs.nlive == 5;
        for (int k = 0; k < 5; ++k) clean &= fabsf(aus[k]) < 0.5f && fabsf(ldb[k]) < 0.01f && flg[k] == 0;
        CHECK(clean, "verify: correct trims leave no residual and no flag");
        CHECK(vs.arrival_spread_us < 1.f && vs.level_spread_db < 0.02f, "verify: the clean spreads are ~0");

        SET_ARR(1, lat + dmax / vc + 500e-6);                    /* a delay_ms 0.5 ms too long */
        vm[3].level *= (float)pow(10.0, -3.0 / 20.0);            /* a gain_db 3 dB too low */
        nf = calib_verify_residuals(vm, vpos, vmic, vmic, 5, vfs, vc, NULL, aus, ldb, flg, &vs);
        printf("verify: corrupted: arrival[1] %+.1f us, level[3] %+.2f dB, flags %d %d %d %d %d, spreads %.1f us %.2f dB\n",
               aus[1], ldb[3], flg[0], flg[1], flg[2], flg[3], flg[4], vs.arrival_spread_us, vs.level_spread_db);
        CHECK(nf == 2 && vs.nflag == 2, "verify: exactly two speakers flagged");
        CHECK(flg[1] == CALIB_VERIFY_FLAG_ARRIVAL && fabsf(aus[1] - 500.f) < 0.5f, "verify: the late speaker reads +500 us, arrival only");
        CHECK(flg[3] == CALIB_VERIFY_FLAG_LEVEL && fabsf(ldb[3] + 3.f) < 0.01f, "verify: the quiet speaker reads -3 dB, level only");
        CHECK(!flg[0] && !flg[2] && !flg[4], "verify: the other three stay clean (the median is robust to two)");
        CHECK(fabsf(vs.arrival_spread_us - 500.f) < 1.f && fabsf(vs.level_spread_db - 3.f) < 0.02f, "verify: spreads are peak to peak");

        /* the thresholds are the edges: 99 us passes, 101 us flags, the same for 0.99 / 1.01 dB */
        SET_ARR(1, lat + dmax / vc + 99e-6);
        vm[3].level = (float)(0.7 / d[3] * pow(10.0, -0.99 / 20.0));
        calib_verify_residuals(vm, vpos, vmic, vmic, 5, vfs, vc, NULL, aus, ldb, flg, &vs);
        CHECK(flg[1] == 0 && flg[3] == 0, "verify: 99 us and 0.99 dB are inside the thresholds");
        SET_ARR(1, lat + dmax / vc + 101e-6);
        vm[3].level = (float)(0.7 / d[3] * pow(10.0, -1.01 / 20.0));
        calib_verify_residuals(vm, vpos, vmic, vmic, 5, vfs, vc, NULL, aus, ldb, flg, &vs);
        CHECK(flg[1] == CALIB_VERIFY_FLAG_ARRIVAL && flg[3] == CALIB_VERIFY_FLAG_LEVEL, "verify: 101 us and 1.01 dB flag");

        /* the directivity re-aim: a speaker measured 4 dB down off-axis, whose trim the corr factor
         * already accounted for, is clean WITH the factor and flagged without it */
        SET_ARR(1, lat + dmax / vc);
        vm[3].level = (float)(0.7 / d[3]);
        vm[2].level = (float)(0.7 / d[2] * pow(10.0, -4.0 / 20.0));
        float cf[5] = { 1, 1, (float)pow(10.0, 4.0 / 20.0), 1, 1 };
        calib_verify_residuals(vm, vpos, vmic, vmic, 5, vfs, vc, cf, aus, ldb, flg, &vs);
        CHECK(flg[2] == 0 && fabsf(ldb[2]) < 0.01f, "verify: the corr factor normalizes the level the way the trims did");
        calib_verify_residuals(vm, vpos, vmic, vmic, 5, vfs, vc, NULL, aus, ldb, flg, &vs);
        CHECK(flg[2] == CALIB_VERIFY_FLAG_LEVEL, "verify: ... and without it the off-axis speaker flags");
        vm[2].level = (float)(0.7 / d[2]);

        /* align_pt: trims aligned at A, measured from the mic. Only the geometric term makes that clean. */
        {
            const float A[3] = { -0.4f, 1.6f, -0.5f };
            double da[5], damax = 0.0;
            for (int k = 0; k < 5; ++k) {
                const double dx = vpos[k][0]-A[0], dy = vpos[k][1]-A[1], dz = vpos[k][2]-A[2];
                da[k] = sqrt(dx*dx + dy*dy + dz*dz); if (da[k] > damax) damax = da[k];
            }
            for (int k = 0; k < 5; ++k) SET_ARR(k, lat + d[k] / vc + (damax - da[k]) / vc);
            calib_verify_residuals(vm, vpos, vmic, A, 5, vfs, vc, NULL, aus, ldb, flg, &vs);
            CHECK(vs.nflag == 0 && vs.arrival_spread_us < 1.f, "verify: aligned at A, checked with align_pt = A: clean");
            calib_verify_residuals(vm, vpos, vmic, vmic, 5, vfs, vc, NULL, aus, ldb, flg, &vs);
            CHECK(vs.arrival_spread_us > 300.f, "verify: ... the same arrivals against the mic spread by the geometry");
            for (int k = 0; k < 5; ++k) SET_ARR(k, lat + d[k] / vc + (dmax - d[k]) / vc);
        }

        /* a dead speaker is flagged DEAD and kept out of both medians */
        vm[4].level = 0.f;
        nf = calib_verify_residuals(vm, vpos, vmic, vmic, 5, vfs, vc, NULL, aus, ldb, flg, &vs);
        CHECK(nf == 1 && flg[4] == CALIB_VERIFY_FLAG_DEAD && vs.nlive == 4, "verify: a silent speaker is DEAD");
        CHECK(fabsf(ldb[0]) < 0.01f && fabsf(aus[0]) < 0.5f, "verify: ... and does not drag the others");
        vm[4].level = NAN;
        calib_verify_residuals(vm, vpos, vmic, vmic, 5, vfs, vc, NULL, aus, ldb, flg, &vs);
        CHECK(flg[4] == CALIB_VERIFY_FLAG_DEAD, "verify: a NaN level reads DEAD, not a number");
        CHECK(calib_verify_residuals(NULL, vpos, vmic, vmic, 5, vfs, vc, NULL, aus, ldb, flg, &vs) == -1,
              "verify: NULL input is refused");
        #undef SET_ARR
    }

    /* ---- the aiming sheet's angle conventions (calib_aim_angles), one axis at a time ----
     * room frame (frame.h): +z room-ahead, +y up, room-right = -x. Bearing clockwise from above,
     * 0 = ahead, 90 = right; down-tilt positive below the horizontal. */
    {
        struct { float v[3]; float b, t; const char* what; } cases[] = {
            { { 0, 0, 1 },   0.f,   0.f, "ahead (+z): bearing 0, level" },
            { {-1, 0, 0 },  90.f,   0.f, "room-right (-x): bearing 90" },
            { { 0, 0,-1 }, 180.f,   0.f, "behind (-z): bearing 180" },
            { { 1, 0, 0 }, 270.f,   0.f, "room-left (+x): bearing 270" },
            { { 0,-1, 0 },   0.f,  90.f, "straight down: down-tilt +90, bearing 0 by convention" },
            { { 0, 1, 0 },   0.f, -90.f, "straight up: down-tilt -90" },
            { { 0,-2, 2 },   0.f,  45.f, "ahead and 45 down (not unit length)" },
            { {-3,-3, 0 },  90.f,  45.f, "right and 45 down" },
            { { 1, 1,-1 }, 225.f, -35.26439f, "behind-left and up: bearing 225, tilt -atan(1/sqrt 2)" },
            { {-1, 0, 1 },  45.f,   0.f, "ahead-right: bearing 45" },
        };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
            float b = -1.f, t = -1.f;
            calib_aim_angles(cases[i].v, &b, &t);
            int ok = fabsf(b - cases[i].b) < 0.01f && fabsf(t - cases[i].t) < 0.01f;
            if (!ok) printf("  aim angles: got bearing %.3f tilt %.3f for %s\n", b, t, cases[i].what);
            CHECK(ok, cases[i].what);
        }

        /* rows from a hand-built layout: the target is ref, the default aim points at it */
        static Layout AL;                                   /* never a stack local (layout.h) */
        memset(&AL, 0, sizeof AL);
        AL.count = 4;
        AL.ref[0] = 0.f; AL.ref[1] = 1.448f; AL.ref[2] = 0.f;
        const float P[4][3] = { {0, 1.448f, 2}, {-2, 3.448f, 0}, {0, 0, 1.5f}, {1.5f, 2.5f, -1.5f} };
        for (int k = 0; k < 4; ++k) memcpy(AL.speakers[k].pos, P[k], sizeof P[k]);
        layout_default_aims(&AL);
        AL.speakers[2].aim[0] = 0.f; AL.speakers[2].aim[1] = 1.f; AL.speakers[2].aim[2] = 0.f;   /* mis-aimed: straight up */
        CalibAimRow r;
        CHECK(calib_aim_row(&AL, 0, &r) && fabsf(r.bearing_deg - 180.f) < 0.01f && fabsf(r.down_tilt_deg) < 0.01f &&
              fabsf(r.dist_m - 2.f) < 1e-5f && r.off_deg < 0.01f && !r.have_loss,
              "aim row: a speaker ahead at ear height faces back (180), level, 2 m, on aim");
        CHECK(calib_aim_row(&AL, 1, &r) && fabsf(r.bearing_deg - 270.f) < 0.01f && fabsf(r.down_tilt_deg - 45.f) < 0.01f,
              "aim row: a speaker high on the right faces left (270) and 45 down");
        CHECK(calib_aim_row(&AL, 2, &r) && fabsf(r.off_deg - (float)(atan2(1.5, 1.448) * 180.0 / M_PI)) < 0.05f &&
              fabsf(r.layout_down_tilt_deg + 90.f) < 0.01f,
              "aim row: the mis-aimed speaker's error is the angle between up and the listening point");
        CHECK(!calib_aim_row(&AL, 4, &r) && !calib_aim_row(NULL, 0, &r), "aim row: out of range is refused");
        /* with a plan, the sheet is the PLAN: speaker 0 measured ahead (+z, so it faces back, 180) but
         * planned behind the listener (-z, facing room-ahead, 0) must read 0 */
        AL.speakers[0].has_plan = 1;
        AL.speakers[0].plan_pos[0] = 0.f; AL.speakers[0].plan_pos[1] = 1.448f; AL.speakers[0].plan_pos[2] = -2.f;
        AL.speakers[0].plan_aim[0] = 0.f; AL.speakers[0].plan_aim[1] = 0.f;    AL.speakers[0].plan_aim[2] = 1.f;
        CHECK(calib_aim_row(&AL, 0, &r) && fabsf(r.bearing_deg) < 0.01f && fabsf(r.pos[2] + 2.f) < 1e-6f && r.off_deg < 0.01f,
              "aim row: a planned speaker's row is its plan, not where the survey found it");
        AL.speakers[0].has_plan = 0;
        /* with a model the loss columns are the model's at that error */
        AL.dir.nband = 2; AL.dir.nang = 2;
        AL.dir.band_hz[0] = 1000.f; AL.dir.band_hz[1] = 10000.f;
        AL.dir.ang_deg[0] = 0.f; AL.dir.ang_deg[1] = 180.f;
        AL.dir.loss_db[0][1] = -10.f; AL.dir.loss_db[1][1] = -20.f;
        AL.dir.split_hz = 3000.f;
        directivity_derive(&AL.dir);
        calib_aim_row(&AL, 2, &r);
        CHECK(r.have_loss && r.loss_16k_db < r.loss_2k_db && r.loss_2k_db < -1.f &&
              fabsf(r.loss_2k_db - directivity_loss_db_at(&AL.dir, r.off_deg, 2000.f)) < 1e-6f,
              "aim row: a model gives the loss at the aim error, treble worse");

        /* the file side: which speakers the FILE aims, whether it declares the listening point, and
         * the CSV itself (a plain header row, one row per speaker, the flag on the mis-aimed one) */
        const char* AP = "calib_aim_in.json";
        const char* AC = "calib_aim_out.csv";
        FILE* f = fopen(AP, "wb");
        CHECK(f != NULL, "open the aim fixture");
        if (f) {
            fprintf(f, "{ \"listening_point_m\": [0, 1.448, 0], \"speakers\": [\n"
                       "  { \"index\": 0, \"position\": [0, 1.448, 2] },\n"
                       "  { \"index\": 1, \"position\": [-2, 3.448, 0], \"aim\": [1, -1, 0] },\n"
                       "  { \"index\": 2, \"position\": [0, 0, 1.5], \"aim\": [0, 1, 0] },\n"
                       "  { \"index\": 3, \"position\": [1.5, 2.5, -1.5] } ] }\n");
            fclose(f);
            static Layout FL;
            char err[256] = { 0 };
            CHECK(layout_load(AP, 48000, &FL, err, sizeof err), err[0] ? err : "load the aim fixture");
            int lp = 0; unsigned char ex[4] = { 9, 9, 9, 9 };
            CHECK(calib_layout_declared(AP, 4, &lp, ex) && lp == 1 && ex[0] == 0 && ex[1] == 1 && ex[2] == 1 && ex[3] == 0,
                  "declared: the listening point and exactly the two explicit aims");
            CHECK(!calib_layout_declared(AP, 5, &lp, ex), "declared: a speaker-count mismatch is refused");
            int nfl = -1;
            CHECK(calib_write_aim_sheet(AC, &FL, ex, CALIB_AIM_SHEET_FLAG_DEG, &nfl, err, sizeof err) && nfl == 1,
                  "sheet: written, one speaker flagged");
            FILE* cf2 = fopen(AC, "rb");
            char line[1024]; int nl = 0, flagged_row = -1, header_ok = 0, lf_only = 1;
            while (cf2 && fgets(line, sizeof line, cf2)) {
                if (strchr(line, '\r')) lf_only = 0;
                if (nl == 0) header_ok = !strncmp(line, "speaker,x_m,y_m,z_m,target_x_m", 30) && strstr(line, ",flag\n") != NULL;
                else if (strstr(line, "OFF_AIM")) flagged_row = atoi(line);
                if (nl == 2) CHECK(strstr(line, ",explicit,") != NULL, "sheet: speaker 1's aim source reads explicit");
                if (nl == 1) CHECK(strstr(line, ",default,") != NULL, "sheet: speaker 0's aim source reads default");
                ++nl;
            }
            if (cf2) fclose(cf2);
            CHECK(header_ok && nl == 5, "sheet: one header row plus one row per speaker, no comment lines");
            CHECK(flagged_row == 2, "sheet: the OFF_AIM flag is on speaker 2");
            CHECK(lf_only, "sheet: plain LF rows");
            remove(AP); remove(AC);
        }
    }

    if (fails) { printf("calib_test: %d FAILURES\n", fails); return 1; }
    printf("calib_test OK (delay align, sensitivity EQ, dead-speaker guard, writeback, trilateration, positions, drift-check, speed-of-sound round trip, verify residuals, aim sheet verified)\n");
    return 0;
}
