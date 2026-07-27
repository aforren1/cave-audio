/*
 * golden_test.c — end-to-end golden-audio regression through the REAL dll, off-hardware and
 * DETERMINISTIC, on the manual sink (bwa_desc.sink = BWA_SINK_MANUAL).
 *
 * The manual sink runs no thread: bwa_render_block pumps one block synchronously on this thread with a
 * pure sample-counter clock, so a fixed scenario (a push-fed tone at a fixed position, default grid,
 * DBAP) renders bit-identically every run. That reproducibility is what makes a committed golden
 * meaningful. The scenario deliberately uses only the SYNCHRONOUS DSP (no occlusion/reflection sims,
 * which are wall-clock-timed and not reproducible), and the CAVE profile (the 26-ch array render is the
 * same with or without the Steam Audio SDK — no HRTF monitor variance).
 *
 * Three checks:
 *   1. reproducible — two independent renders are bit-identical (the harness's core guarantee).
 *   2. golden       — total + left/right energy match committed references within a relative tolerance
 *                     (catches a DSP regression a geometric assertion would miss — a pan-law/gain change).
 *   3. sane         — finite, non-silent, and a room -x (listener's right) source is right-biased.
 *
 * The golden constants are a coarse energy fingerprint, not exact PCM: robust to minor FP/codegen
 * differences while still failing on a real change. Regenerate by reading this test's own printout.
 */
#include "bw_audio.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define SR    48000u
#define BLK   256u
#define NBLK  48u             /* ~0.25 s */
#define NHEAD 256             /* first-channel samples kept as a reproducibility fingerprint */
#define MAXCH 32              /* safe upper bound on the array width (public ABI caps at 26) */

static void fill_tone(float* buf, uint32_t n, uint64_t start) {   /* continuous-phase 440 Hz sine */
    for (uint32_t i = 0; i < n; ++i)
        buf[i] = 0.25f * sinf(6.2831853f * 440.0f * (float)(start + i) / (float)SR);
}

/* Render the fixed scenario. Returns 0 on success, filling total/negX/posX channel energy (split by
 * speaker x-sign) and the first NHEAD samples of channel 0. */
static int render_scenario(double* total, double* negX, double* posX, float* head) {
    bwa_desc cfg; memset(&cfg, 0, sizeof cfg);
    cfg.profile = BWA_PROFILE_CAVE; cfg.sample_rate = SR; cfg.block_size = BLK; cfg.sink = BWA_SINK_MANUAL;
    bwa_engine* e = bwa_create(&cfg);
    if (!e) { fprintf(stderr, "golden: bwa_create failed: %s\n", bwa_last_error(NULL)); return -1; }

    uint32_t nch = bwa_get_channel_count(e);
    float spk[MAXCH * 3];
    bwa_get_speakers(e, spk, MAXCH);

    bwa_source s = bwa_source_create_push(e);
    if (!s) { fprintf(stderr, "golden: create_stream: %s\n", bwa_last_error(e)); bwa_destroy(e); return -1; }
    bwa_source_set_pos(e, s, -1.5f, 1.5f, 0.0f);                  /* room -x = the listener's RIGHT */
    bwa_set_listener_pose(e, 0.0f, 1.5f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    bwa_commit(e);
    if (bwa_start(e) != 0) { fprintf(stderr, "golden: bwa_start: %s\n", bwa_last_error(e)); bwa_destroy(e); return -1; }

    double e_ch[MAXCH]; for (uint32_t c = 0; c < nch; ++c) e_ch[c] = 0.0;
    float tone[BLK];
    int hc = 0;
    for (uint32_t b = 0; b < NBLK; ++b) {
        fill_tone(tone, BLK, (uint64_t)b * BLK);
        bwa_source_push(e, s, tone, BLK);                        /* one block of data before each pull: no underrun */
        uint32_t ch = 0, n = 0;
        const float* out = bwa_render_block(e, &ch, &n);
        if (!out || ch != nch || n != BLK) { fprintf(stderr, "golden: render_block bad (%p ch=%u n=%u)\n", (void*)out, ch, n); bwa_stop(e); bwa_destroy(e); return -1; }
        for (uint32_t c = 0; c < nch; ++c)
            for (uint32_t i = 0; i < n; ++i) { float v = out[(size_t)c * n + i]; e_ch[c] += (double)v * v; }
        for (uint32_t i = 0; i < n && hc < NHEAD; ++i) head[hc++] = out[i];   /* channel 0 fingerprint */
    }
    /* The manual sink has no clock and no deadline, so it cannot miss one — and must SAY so rather
     * than report a clean bill. This is the offline blind spot the counters exist to be honest
     * about: everything else in this file is deterministic precisely because nothing here can be
     * late. (bwa_get_xruns reads 0 here too, which is exactly why it is not proof of health.) */
    {
        bwa_health h;
        if (bwa_get_health(e, &h)) {
            fprintf(stderr, "golden: the manual sink cannot observe a dropout, but health claims it can\n");
            bwa_stop(e); bwa_destroy(e); return -1;
        }
    }

    bwa_stop(e);
    bwa_destroy(e);

    *total = *negX = *posX = 0.0;
    for (uint32_t c = 0; c < nch; ++c) {
        *total += e_ch[c];
        if      (spk[c * 3 + 0] < -0.5f) *negX += e_ch[c];
        else if (spk[c * 3 + 0] >  0.5f) *posX += e_ch[c];
    }
    return 0;
}

int main(void) {
    double t1, nx1, px1, t2, nx2, px2;
    static float head1[NHEAD], head2[NHEAD];
    if (render_scenario(&t1, &nx1, &px1, head1) != 0) { printf("FAIL: render 1\n"); return 1; }
    if (render_scenario(&t2, &nx2, &px2, head2) != 0) { printf("FAIL: render 2\n"); return 1; }

    int reproducible = (t1 == t2) && (nx1 == nx2) && (px1 == px2);
    for (int i = 0; i < NHEAD; ++i) if (head1[i] != head2[i]) reproducible = 0;

    printf("golden: total=%.6f  -x=%.6f  +x=%.6f   reproducible=%d\n", t1, nx1, px1, reproducible);

    /* committed references (this test's own printout); relative tolerance absorbs minor FP/codegen drift.
     * Golden the large, stable energies; the tiny +x spill is left to the laterality check below. To
     * regenerate after an intended DSP change, run test_golden and paste the printed total / -x. */
    const double G_TOTAL = 168.172397, G_NEGX = 167.502218, TOL = 2e-3;
    int golden_ok = fabs(t1 - G_TOTAL) <= TOL * G_TOTAL
                 && fabs(nx1 - G_NEGX) <= TOL * G_NEGX;

    int sane = isfinite(t1) && t1 > 0.0 && nx1 > px1 * 1.1;   /* right-biased for a -x source */

    int pass = reproducible && sane && golden_ok;
    printf("%s (reproducible=%d sane=%d golden=%d)\n",
           pass ? "PASS: deterministic end-to-end render matches the golden"
                : "FAIL: golden mismatch or non-deterministic render",
           reproducible, sane, golden_ok);
    return pass ? 0 : 1;
}
