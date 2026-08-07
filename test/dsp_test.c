/*
 * dsp_test.c — M4 verification of the spatialization DSP, standalone (no rt/audio thread):
 *   - layout_default has 26 speakers; layout_load parses cave_layout.json (positions, the
 *     dbap params, gain_db->linear, delay_ms->samples);
 *   - DBAP localizes a source at each speaker to that channel (centered listener), is
 *     constant-power, splits between two speakers, and responds to listener moves;
 *   - align applies the per-channel gain trim and integer-sample delay.
 */
#include "layout.h"
#include "dbap.h"
#include "spcap.h"
#include "vbap.h"
#include "align.h"
#include "ambisonics.h"
#include "allrad.h"
#include "epad.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CH   BWA_CHANNELS
#define RATE 48000u

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", (msg)); ++fails; } } while (0)

static int argmax(const float* g, int n) {
    int b = 0; float m = g[0];
    for (int i = 1; i < n; ++i) if (g[i] > m) { m = g[i]; b = i; }
    return b;
}

/* Emit a valid cave_layout.json: the default grid, with speaker 5 trimmed and 9 delayed. */
static int write_layout_json(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    const float ax[3] = { -1.5f, 0.f, 1.5f };
    fprintf(f, "{\n");
    fprintf(f, "  \"dbap\": { \"rolloff_r\": 0.7, \"distance_attenuation\": "
               "{ \"reference_distance_m\": 1.0, \"rolloff\": 1.0, \"min_gain_db\": -40.0 } },\n");
    fprintf(f, "  \"speakers\": [\n");
    int k = 0;
    for (int yi = 0; yi < 3; ++yi) for (int xi = 0; xi < 3; ++xi) for (int zi = 0; zi < 3; ++zi) {
        if (ax[xi] == 0 && ax[yi] == 0 && ax[zi] == 0) continue;
        double gain_db  = (k == 5) ? -6.0 : 0.0;
        double delay_ms = (k == 9) ?  1.0 : 0.0;
        fprintf(f, "    { \"index\": %d, \"position\": [%g, %g, %g], \"gain_db\": %g, \"delay_ms\": %g }%s\n",
                k, ax[xi], ax[yi], ax[zi], gain_db, delay_ms, (k == 25) ? "" : ",");
        ++k;
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    return k == 26;
}

/* a grid layout with overridable rolloff_r and speaker-0 gain_db/delay_ms (for negative tests) */
static int write_layout_with(const char* path, double rolloff_r, double g0_db, double d0_ms) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    const float ax[3] = { -1.5f, 0.f, 1.5f };
    fprintf(f, "{ \"dbap\": { \"rolloff_r\": %g }, \"speakers\": [\n", rolloff_r);
    int k = 0;
    for (int yi = 0; yi < 3; ++yi) for (int xi = 0; xi < 3; ++xi) for (int zi = 0; zi < 3; ++zi) {
        if (ax[xi] == 0 && ax[yi] == 0 && ax[zi] == 0) continue;
        double g = (k == 0) ? g0_db : 0.0, d = (k == 0) ? d0_ms : 0.0;
        fprintf(f, "  {\"index\":%d,\"position\":[%g,%g,%g],\"gain_db\":%g,\"delay_ms\":%g}%s\n",
                k, ax[xi], ax[yi], ax[zi], g, d, (k == 25) ? "" : ",");
        ++k;
    }
    fprintf(f, "] }\n");
    fclose(f);
    return k == 26;
}

/* a grid layout carrying a room_eq_grid: two positions, one 45 Hz section on speaker 0.
 * mode 0 = valid; 1 = the second position's fc disagrees (ladder mismatch); 2 = plus a static
 * room_eq on speaker 0 (the schemes are mutually exclusive). */
static int write_layout_grid(const char* path, int mode) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    const float ax[3] = { -1.5f, 0.f, 1.5f };
    fprintf(f, "{ \"speakers\": [\n");
    int k = 0;
    for (int yi = 0; yi < 3; ++yi) for (int xi = 0; xi < 3; ++xi) for (int zi = 0; zi < 3; ++zi) {
        if (ax[xi] == 0 && ax[yi] == 0 && ax[zi] == 0) continue;
        fprintf(f, "  {\"index\":%d,\"position\":[%g,%g,%g]%s}%s\n", k, ax[xi], ax[yi], ax[zi],
                (mode == 2 && k == 0) ? ",\"room_eq\":[{\"fc\":80,\"gain_db\":-6,\"q\":4}]" : "",
                (k == 25) ? "" : ",");
        ++k;
    }
    fprintf(f, "],\n\"room_eq_grid\": [\n");
    for (int p = 0; p < 2; ++p) {
        double fc = (p == 1 && mode == 1) ? 60.0 : 45.0;      /* mode 1: ladder mismatch */
        double g  = (p == 0) ? -8.0 : -4.0;
        fprintf(f, " {\"position\":[%g,1.5,0],\"speakers\":[\n  [{\"fc\":%g,\"gain_db\":%g,\"q\":6}]",
                p ? 1.0 : -1.0, fc, g);
        for (int s = 1; s < 26; ++s) fprintf(f, ",[]");
        fprintf(f, "\n ]}%s\n", p ? "" : ",");
    }
    fprintf(f, "]}\n");
    fclose(f);
    return k == 26;
}

/* an N-speaker layout: the first N positions of the default grid, indices 0..N-1 (bad_index >= 0
 * replaces speaker 0's index — a gap/out-of-range case for the loader to reject) */
static int write_layout_n(const char* path, int n, int bad_index) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    Layout g = layout_default();
    fprintf(f, "{ \"speakers\": [\n");
    for (int k = 0; k < n; ++k) {
        int idx = (k == 0 && bad_index >= 0) ? bad_index : k;
        fprintf(f, "  {\"index\":%d,\"position\":[%g,%g,%g]}%s\n", idx,
                g.speakers[k].pos[0], g.speakers[k].pos[1], g.speakers[k].pos[2], (k == n - 1) ? "" : ",");
    }
    fprintf(f, "] }\n");
    fclose(f);
    return 1;
}

int main(void) {
    /* 1. default layout */
    Layout LD = layout_default();
    CHECK(LD.count == CH, "default layout has 26 speakers");

    /* 2. layout_load parse */
    const char* LJ = "bwa_layout.json";
    CHECK(write_layout_json(LJ), "write layout json");
    char err[256] = {0};
    Layout L;
    CHECK(layout_load(LJ, RATE, &L, err, sizeof err), err[0] ? err : "layout_load");
    CHECK(L.count == CH, "loaded 26 speakers");
    CHECK(fabs(L.rolloff_r - 0.7) < 1e-5, "parsed dbap.rolloff_r");
    CHECK(fabs(L.speakers[5].gain_lin - powf(10.f, -6.f / 20.f)) < 1e-4, "parsed gain_db -> linear");
    CHECK(L.speakers[9].delay_samples == (uint32_t)(1.0 * 1e-3 * RATE + 0.5), "parsed delay_ms -> samples");
    CHECK(L.max_delay_samples == L.speakers[9].delay_samples, "max_delay_samples");

    /* 3. DBAP localization (centered listener): source at speaker k -> channel k dominates */
    {
        float lis[3] = { LD.ref[0], LD.ref[1], LD.ref[2] }, g[CH];   /* the array center (floor origin) */
        int ok = 1;
        for (int k = 0; k < CH; ++k) {
            dbap_gains(LD.speakers[k].pos, lis, &LD, 1.0f, g);
            if (argmax(g, CH) != k) ok = 0;
        }
        CHECK(ok, "DBAP localizes a source at each speaker to that channel");
    }

    /* 4. constant power: ||g|| ~ user_gain (atten == 1 within the reference distance) */
    {
        float lis[3] = { LD.ref[0], LD.ref[1], LD.ref[2] };
        float src[3] = { 0.5f, LD.ref[1], 0.5f }, g[CH];             /* ds ~ 0.7 m < ref -> atten = 1 */
        float gain = 0.8f;
        dbap_gains(src, lis, &LD, gain, g);
        double p = 0; for (int k = 0; k < CH; ++k) p += (double)g[k] * g[k];
        CHECK(fabs(sqrt(p) - gain) < 0.02, "DBAP is constant-power (||g|| ~ user_gain)");
    }

    /* 5. two-speaker split: a source midway between two speakers feeds both above average */
    {
        const int A = 7, B = 8;
        float lis[3] = { LD.ref[0], LD.ref[1], LD.ref[2] }, g[CH];
        float mid[3] = { (LD.speakers[A].pos[0] + LD.speakers[B].pos[0]) * 0.5f,
                         (LD.speakers[A].pos[1] + LD.speakers[B].pos[1]) * 0.5f,
                         (LD.speakers[A].pos[2] + LD.speakers[B].pos[2]) * 0.5f };
        dbap_gains(mid, lis, &LD, 1.0f, g);
        double avg = 0; for (int k = 0; k < CH; ++k) avg += g[k];
        avg /= CH;
        CHECK(g[A] > avg && g[B] > avg, "source between two speakers feeds both above average");
    }

    /* 6. listener move changes the distribution */
    {
        float src[3] = { 1.5f, LD.ref[1], 0.f }, g0[CH], g1[CH];
        float lis0[3] = { 0, LD.ref[1], 0 }, lis1[3] = { -1.0f, LD.ref[1], 0 };
        dbap_gains(src, lis0, &LD, 1.0f, g0);
        dbap_gains(src, lis1, &LD, 1.0f, g1);
        double diff = 0; for (int k = 0; k < CH; ++k) diff += fabs(g0[k] - g1[k]);
        CHECK(diff > 1e-3, "moving the listener changes the gain distribution");
    }

    /* 6b. moving-listener localization (the headline feature): a source co-located with speaker k
     *     localizes to channel k from ANY listener position, because DBAP is listener-relative — the
     *     source and speaker k share a bearing from every point. A sign error in the listener-relative
     *     direction would pull the image to the OPPOSITE speaker, which the centered test 3 can't see. */
    {
        float lis[3] = { -1.0f, LD.ref[1] - 0.4f, 0.6f }, g[CH];   /* off-center AND off the ear plane */
        int ok = 1;
        for (int k = 0; k < CH; ++k) {
            dbap_gains(LD.speakers[k].pos, lis, &LD, 1.0f, g);
            if (argmax(g, CH) != k) ok = 0;
        }
        CHECK(ok, "DBAP localizes a source at each speaker to that channel from an off-center listener");
    }

    /* 6c. boundary-crossing continuity + injectivity: hull-projection DBAP has two documented
     *     failure modes outside the array (Sundstrom 2021, I3DA): gains become non-unique when the
     *     projection lands on a hull vertex, and total power "undulates wildly" across the boundary.
     *     The engine's formulation is hull-free, so a source swept from the center out through a
     *     corner speaker and beyond must give (a) per-speaker gains continuous in position, (b) a
     *     total level monotone non-increasing past the reference distance, and (c) distinct gain
     *     vectors for distinct positions (no exterior collapse). The ray passes THROUGH the corner
     *     speaker — the hull-vertex case — and runs to ~4x the array radius. */
    {
        const float* corner = LD.speakers[0].pos;                    /* a corner of the grid */
        float lis[3] = { LD.ref[0], LD.ref[1], LD.ref[2] };
        float dir[3] = { corner[0] - lis[0], corner[1] - lis[1], corner[2] - lis[2] };
        float dl = sqrtf(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
        for (int c = 0; c < 3; ++c) dir[c] /= dl;
        const float step = 0.01f;                                    /* 1 cm */
        float gprev[CH], g[CH];
        double pprev = -1.0;
        int cont_ok = 1, mono_ok = 1, inj_ok = 1;
        /* start at i=1: at src == lis the bearing is undefined and the solve goes omnidirectional
         * (documented in spatialization.md) — a genuine discontinuity AT the listener, excluded
         * here; near-listener widening is the feature that covers fly-throughs. */
        for (int i = 1; i <= 1000; ++i) {                            /* 1 cm .. 10 m along the ray */
            float t = step * (float)i;
            float src[3] = { lis[0] + dir[0]*t, lis[1] + dir[1]*t, lis[2] + dir[2]*t };
            dbap_gains(src, lis, &LD, 1.0f, g);
            double p = 0; for (int k = 0; k < CH; ++k) p += (double)g[k] * g[k];
            p = sqrt(p);
            if (i > 1) {
                double dmax = 0, dsum = 0;
                for (int k = 0; k < CH; ++k) {
                    double d = fabs((double)g[k] - gprev[k]);
                    if (d > dmax) dmax = d;
                    dsum += d;
                }
                if (dmax > 0.02) cont_ok = 0;                        /* no per-cm gain jump */
                if (t > LD.atten_ref_m + step && p > pprev + 1e-6) mono_ok = 0;
                if (t < 4.0f && dsum < 1e-7) inj_ok = 0;             /* interior+near field: still injective */
            }
            memcpy(gprev, g, sizeof gprev);
            pprev = p;
        }
        CHECK(cont_ok, "DBAP gains are continuous across the array boundary (no undulation)");
        CHECK(mono_ok, "DBAP total level is monotone non-increasing past the reference distance");
        CHECK(inj_ok,  "DBAP gain vectors stay distinct as an exterior source recedes");
    }

    /* 7. align: gain trim halves a channel; delay shifts the impulse */
    {
        Layout AL = layout_default();
        AL.speakers[2].gain_lin = 0.5f;
        AL.speakers[3].delay_samples = 4;
        AL.max_delay_samples = 4;
        Aligner* a = align_create(CH, &AL, RATE);
        CHECK(a != NULL, "align_create");
        if (a) {
            const uint32_t n = 16;
            float buf[CH * 16];
            memset(buf, 0, sizeof buf);
            buf[0 * n + 0] = 1.0f;             /* ch0: gain 1, delay 0 (passthrough) */
            buf[2 * n + 0] = 1.0f;             /* ch2: gain 0.5 */
            buf[3 * n + 0] = 1.0f;             /* ch3: delay 4 */
            align_process(a, buf, n);
            CHECK(fabs(buf[0 * n + 0] - 1.0f) < 1e-6, "passthrough channel unchanged");
            CHECK(fabs(buf[2 * n + 0] - 0.5f) < 1e-6, "gain trim 0.5 halves the channel");
            CHECK(fabs(buf[3 * n + 0]) < 1e-6 && fabs(buf[3 * n + 4] - 1.0f) < 1e-6,
                  "delay shifts the impulse by 4 samples");
            align_destroy(a);
        }
    }

    /* 7a. per-speaker correction FIR: a channel's kernel convolves its signal (before gain+delay) */
    {
        Layout EQ = layout_default();
        EQ.speakers[5].eq_len = 3;
        EQ.speakers[5].eq[0] = 0.5f; EQ.speakers[5].eq[1] = 0.25f; EQ.speakers[5].eq[2] = -0.1f;
        Aligner* a = align_create(CH, &EQ, RATE);
        CHECK(a != NULL, "align_create (eq)");
        if (a) {
            const uint32_t n = 16;
            float buf[CH * 16];
            memset(buf, 0, sizeof buf);
            buf[5 * n + 0] = 1.0f;             /* impulse on the EQ'd channel (gain 1, delay 0) */
            buf[1 * n + 0] = 1.0f;             /* a non-EQ channel passes through */
            align_process(a, buf, n);
            CHECK(fabs(buf[5*n+0] - 0.5f)  < 1e-6 &&
                  fabs(buf[5*n+1] - 0.25f) < 1e-6 &&
                  fabs(buf[5*n+2] + 0.1f)  < 1e-6, "correction FIR convolves the channel with its kernel");
            CHECK(fabs(buf[1*n+0] - 1.0f) < 1e-6, "a channel with no correction passes through");
            align_destroy(a);
        }
    }

    /* 7a2. room_eq modal cut: a -6 dB peaking section at 100 Hz attenuates a 100 Hz tone by ~6 dB
     * and leaves 1 kHz (and other channels) alone. Steady-state RMS over the tail (filter settled). */
    {
        Layout RQ = layout_default();
        RQ.speakers[4].room_eq_count = 1;
        RQ.speakers[4].room_eq[0].fc = 100.f; RQ.speakers[4].room_eq[0].gain_db = -6.f; RQ.speakers[4].room_eq[0].q = 2.f;
        Aligner* a = align_create(CH, &RQ, RATE);
        CHECK(a != NULL, "align_create (room_eq)");
        if (a) {
            enum { NN = 48000 };
            static float buf[CH * NN];
            memset(buf, 0, sizeof buf);
            for (int i = 0; i < NN; ++i) {
                buf[4 * (size_t)NN + i] = sinf(2.f * 3.14159265f * 100.f  * i / (float)RATE);
                buf[6 * (size_t)NN + i] = sinf(2.f * 3.14159265f * 100.f  * i / (float)RATE);   /* no room_eq */
                buf[7 * (size_t)NN + i] = sinf(2.f * 3.14159265f * 1000.f * i / (float)RATE);
            }
            align_process(a, buf, NN);
            double r4 = 0, r6 = 0, r7 = 0;
            for (int i = NN / 2; i < NN; ++i) {                     /* settled tail only */
                r4 += (double)buf[4*(size_t)NN+i] * buf[4*(size_t)NN+i];
                r6 += (double)buf[6*(size_t)NN+i] * buf[6*(size_t)NN+i];
                r7 += (double)buf[7*(size_t)NN+i] * buf[7*(size_t)NN+i];
            }
            double att_db = 10.0 * log10(r4 / r6);                  /* cut channel vs untouched at fc */
            CHECK(att_db < -5.0 && att_db > -7.0, "room_eq section cuts ~6 dB at its center frequency");
            CHECK(fabs(10.0 * log10(r7 / (0.5 * (NN / 2)))) < 0.6,  "room_eq leaves off-fc content alone");
            align_destroy(a);
        }
    }

    /* 7a3. tracked room EQ (room_eq_grid ladder): align SLEWS each section toward the targets set by
     * align_room_eq_targets (24 dB/s) — a -12 dB target glides in (the first 0.25 s window sits
     * mid-slew, not already cut), lands at depth, and releases back to flat. Windowed RMS of a 100 Hz
     * tone on the cut channel, block-sized processing like the audio thread. */
    {
        Layout G = layout_default();
        G.rq_grid.npos = 1;              /* align only reads the ladder — rt.c owns the interpolation */
        G.rq_grid.nsec[4]  = 1;
        G.rq_grid.fc[4][0] = 100.f; G.rq_grid.q[4][0] = 2.f;
        Aligner* a = align_create(CH, &G, RATE);
        CHECK(a != NULL, "align_create (room_eq_grid)");
        if (a) {
            enum { BL = 256, WIN = 47 };                      /* 47 blocks ~ 0.25 s per window */
            static float blk[CH * BL];
            float tgt[BWA_CHANNELS][BWA_ROOM_EQ_MAX];
            memset(tgt, 0, sizeof tgt);
            int ph = 0;
            #define GRID_WIN_DB(out) do {                                                        \
                double r_ = 0;                                                                   \
                for (int b_ = 0; b_ < WIN; ++b_) {                                               \
                    memset(blk, 0, sizeof blk);                                                  \
                    for (int i_ = 0; i_ < BL; ++i_, ++ph)                                        \
                        blk[4*(size_t)BL + i_] = sinf(2.f*3.14159265f*100.f*(float)ph/(float)RATE); \
                    align_process(a, blk, BL);                                                   \
                    for (int i_ = 0; i_ < BL; ++i_) r_ += (double)blk[4*(size_t)BL+i_]*blk[4*(size_t)BL+i_]; \
                }                                                                                \
                (out) = 10.0 * log10(r_ / (0.5 * WIN * BL));                                     \
            } while (0)
            tgt[4][0] = -12.f;
            align_room_eq_targets(a, tgt);
            double w0, wlast;
            GRID_WIN_DB(w0);                                  /* mid-glide: ~ -3 dB average */
            for (int wn = 0; wn < 7; ++wn) GRID_WIN_DB(wlast); /* by 2 s: fully landed */
            CHECK(w0 > -9.0,                     "tracked cut glides in (first window is mid-slew, not a step)");
            CHECK(wlast > -13.0 && wlast < -11.0, "tracked cut lands at its target depth");
            memset(tgt, 0, sizeof tgt);
            align_room_eq_targets(a, tgt);                    /* release */
            for (int wn = 0; wn < 4; ++wn) GRID_WIN_DB(wlast);
            CHECK(fabs(wlast) < 1.0,             "tracked cut releases back to flat");
            #undef GRID_WIN_DB
            align_destroy(a);
        }
    }

    /* 7b. layout_load rejects out-of-range values (so bad JSON can't reach the audio thread) */
    {
        const char* BJ = "bwa_bad_layout.json";
        Layout B;
        write_layout_with(BJ, 0.0, 0.0, 0.0);     CHECK(!layout_load(BJ, RATE, &B, err, sizeof err), "rolloff_r=0 is rejected");
        write_layout_with(BJ, 0.7, 1000.0, 0.0);  CHECK(!layout_load(BJ, RATE, &B, err, sizeof err), "gain_db=1000 is rejected");
        write_layout_with(BJ, 0.7, 0.0, 1.0e7);   CHECK(!layout_load(BJ, RATE, &B, err, sizeof err), "huge delay_ms is rejected");
        write_layout_with(BJ, 0.7, 0.0, 0.0);     CHECK( layout_load(BJ, RATE, &B, err, sizeof err), "valid layout still loads");
        remove(BJ);
    }

    /* 7b2. room_eq_grid schema: parses into the shared ladder + per-position depths; positions must
     * agree on the ladder (depths interpolate by index); static room_eq + grid don't mix. */
    {
        const char* GJ = "bwa_grid_layout.json";
        Layout B;
        write_layout_grid(GJ, 0);
        CHECK(layout_load(GJ, RATE, &B, err, sizeof err), err[0] ? err : "room_eq_grid layout loads");
        CHECK(B.rq_grid.npos == 2, "room_eq_grid has both positions");
        CHECK(B.rq_grid.nsec[0] == 1 && B.rq_grid.nsec[1] == 0, "ladder sizes parsed per speaker");
        CHECK(fabs(B.rq_grid.fc[0][0] - 45.f) < 1e-3 && fabs(B.rq_grid.q[0][0] - 6.f) < 1e-3,
              "ladder fc/q parsed");
        CHECK(fabs(B.rq_grid.gain_db[0][0][0] + 8.f) < 1e-3 && fabs(B.rq_grid.gain_db[1][0][0] + 4.f) < 1e-3,
              "per-position depths parsed");
        CHECK(fabs(B.rq_grid.pos[1][0] - 1.f) < 1e-3, "grid positions parsed");
        write_layout_grid(GJ, 1);
        CHECK(!layout_load(GJ, RATE, &B, err, sizeof err), "a ladder mismatch across positions is rejected");
        write_layout_grid(GJ, 2);
        CHECK(!layout_load(GJ, RATE, &B, err, sizeof err), "room_eq + room_eq_grid together are rejected");
        remove(GJ);
    }

    /* 7b3. runtime channel count: the loader accepts 4..26 speakers whose indices form a complete
     * 0..N-1 permutation — the engine's channel count follows the file (BWA_CHANNELS is the cap). */
    {
        const char* NJ = "bwa_n_layout.json";
        Layout B;
        write_layout_n(NJ, 24, -1);
        CHECK(layout_load(NJ, RATE, &B, err, sizeof err), err[0] ? err : "a 24-speaker layout loads");
        CHECK(B.count == 24, "count follows the file");
        {   /* no dbap block in the file -> the blur derives from the geometry:
             * r = 0.25 x the mean centroid->speaker distance (Sundstrom 2021) */
            double s = 0;
            for (uint32_t k = 0; k < B.count; ++k) {
                double dx = B.speakers[k].pos[0] - B.ref[0], dy = B.speakers[k].pos[1] - B.ref[1],
                       dz = B.speakers[k].pos[2] - B.ref[2];
                s += sqrt(dx * dx + dy * dy + dz * dz);
            }
            CHECK(fabs(B.rolloff_r - 0.25 * s / B.count) < 1e-5,
                  "omitted rolloff_r derives from the mean centroid->speaker distance");
        }
        write_layout_n(NJ, 3, -1);
        CHECK(!layout_load(NJ, RATE, &B, err, sizeof err), "fewer than 4 speakers is rejected");
        write_layout_n(NJ, 24, 24);                       /* index 24 in a 24-speaker file: gap at 0 */
        CHECK(!layout_load(NJ, RATE, &B, err, sizeof err), "an index outside 0..N-1 is rejected");
        remove(NJ);
    }

    /* 8. the committed example layout parses with this loader (schema-vs-parser integration) */
    {
        const char* cands[] = { "examples/cave_layout.json", "../examples/cave_layout.json",
                                "../../examples/cave_layout.json", "../../../examples/cave_layout.json" };
        Layout EX;
        int found = 0;
        for (size_t i = 0; i < sizeof cands / sizeof cands[0]; ++i) {
            FILE* probe = fopen(cands[i], "rb");
            if (probe) {
                fclose(probe);
                found = 1;
                CHECK(layout_load(cands[i], RATE, &EX, err, sizeof err), err[0] ? err : "load examples/cave_layout.json");
                CHECK(EX.count == CH, "examples/cave_layout.json has 26 speakers");
                break;
            }
        }
        if (!found) printf("note: examples/cave_layout.json not found from CWD; integration check skipped\n");
    }

    /* 9. SPCAP panner: localization, constant power, multi-speaker spread, non-negative gains */
    {
        SpcapState sp; spcap_reset(&sp);
        float lis[3] = { LD.ref[0], LD.ref[1], LD.ref[2] }, g[CH];   /* sweet spot = the array center */
        int loc_ok = 1, nonneg = 1;
        for (int k = 0; k < CH; ++k) {
            spcap_gains(&sp, LD.speakers[k].pos, lis, &LD, 1u, LD.spcap_focus, LD.spcap_density,
                        1.0f, g);                                          /* source at speaker k's bearing */
            if (argmax(g, CH) != k) loc_ok = 0;
            for (int i = 0; i < CH; ++i) if (g[i] < -1e-6f) nonneg = 0;
        }
        CHECK(loc_ok, "SPCAP localizes a source at each speaker to that channel");
        CHECK(nonneg, "SPCAP gains are non-negative");

        float src[3] = { 0.5f, LD.ref[1], 0.5f }, gain = 0.8f;
        spcap_gains(&sp, src, lis, &LD, 1u, LD.spcap_focus, LD.spcap_density, gain, g);
        double p = 0; int active = 0;
        for (int k = 0; k < CH; ++k) { p += (double)g[k] * g[k]; if (g[k] > 0.05f * gain) ++active; }
        CHECK(fabs(sqrt(p) - gain) < 0.02, "SPCAP is constant-power (||g|| ~ user_gain)");
        CHECK(active >= 3 && active <= 20, "SPCAP spreads across several speakers (not 1, not all)");
    }

    /* 9b. SPCAP tuning knobs: the geometry-derived focus default, focus monotonicity (with constant
     * power held), and the density change invalidating the cached placement correction. */
    {
        const float lis[3] = { LD.ref[0], LD.ref[1], LD.ref[2] };   /* sweet spot = the array center */
        printf("spcap: derived focus on the default cube grid = %.2f\n", (double)LD.spcap_focus);
        CHECK(LD.spcap_focus > 8.f && LD.spcap_focus < 20.f,
              "derived SPCAP focus lands in a sane band on the default grid");

        /* the -6 dB-at-nearest-neighbor formula, checked at three known angles: a two-speaker array
         * separated by `deg` has exactly that mean nearest-neighbor separation */
        const double want[3] = { 19.99, 8.75, 4.82 };
        const float  degs[3] = { 30.f, 45.f, 60.f };
        for (int t = 0; t < 3; ++t) {
            Layout P; memset(&P, 0, sizeof P);
            P.count = 2;
            float a = degs[t] * 3.14159265358979f / 180.f;
            P.speakers[0].pos[0] =  sinf(0.5f*a); P.speakers[0].pos[2] = cosf(0.5f*a);
            P.speakers[1].pos[0] = -sinf(0.5f*a); P.speakers[1].pos[2] = cosf(0.5f*a);
            P.speakers[0].pos[1] = P.speakers[1].pos[1] = 0.f;
            P.ref[0] = P.ref[1] = P.ref[2] = 0.f;             /* solve from the origin, not the centroid */
            float f = layout_derive_spcap_focus(&P);
            CHECK(fabs((double)f - want[t]) < 0.05, "derived focus matches the -6 dB formula");
        }

        /* a WIDER array wants a broader lobe (lower focus), a DENSER one a tighter lobe. Six
         * speakers on the axes sit 90 deg apart; a 5x5x5 shell is packed tighter than the 3x3x3. */
        Layout WIDE = LD;
        const float ax6[6][3] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
        WIDE.count = 6;
        for (int k = 0; k < 6; ++k) for (int j = 0; j < 3; ++j)
            WIDE.speakers[k].pos[j] = LD.ref[j] + 2.f * ax6[k][j];
        float f_wide = layout_derive_spcap_focus(&WIDE);

        Layout DENSE = LD;                                    /* a 12-speaker ring: 30 deg apart, so
                                                               * denser than the grid's 37.5 deg */
        DENSE.count = 12;
        for (int k = 0; k < 12; ++k) {
            float a = (float)k * (2.f * 3.14159265358979f / 12.f);
            DENSE.speakers[k].pos[0] = LD.ref[0] + 2.f * sinf(a);
            DENSE.speakers[k].pos[1] = LD.ref[1];
            DENSE.speakers[k].pos[2] = LD.ref[2] + 2.f * cosf(a);
        }
        float f_dense = layout_derive_spcap_focus(&DENSE);
        printf("spcap: derived focus, grid %.2f, 6-speaker cross %.2f, 12-speaker ring %.2f\n",
               (double)LD.spcap_focus, (double)f_wide, (double)f_dense);
        CHECK(f_wide < LD.spcap_focus, "a wider array derives a broader lobe (lower focus)");
        CHECK(f_dense > LD.spcap_focus, "a denser array derives a tighter lobe (higher focus)");
        CHECK(f_wide >= 1.f && f_wide <= 64.f && f_dense >= 1.f && f_dense <= 64.f,
              "derived focus stays inside the clamp band");

        /* focus monotonicity: raise it and the energy concentrates, while ||g|| stays at user_gain */
        float src2[3] = { 0.35f, LD.ref[1] + 0.2f, 0.9f };
        float gl[CH], gh[CH];
        SpcapState sl; spcap_reset(&sl);
        SpcapState sh; spcap_reset(&sh);
        spcap_gains(&sl, src2, lis, &LD, 1u,  4.f, LD.spcap_density, 1.0f, gl);
        spcap_gains(&sh, src2, lis, &LD, 1u, 32.f, LD.spcap_density, 1.0f, gh);
        double pl = 0, ph = 0; float peakl = 0, peakh = 0; int nl = 0, nh = 0;
        for (int k = 0; k < CH; ++k) {
            pl += (double)gl[k]*gl[k]; ph += (double)gh[k]*gh[k];
            if (gl[k] > peakl) peakl = gl[k];
            if (gh[k] > peakh) peakh = gh[k];
        }
        for (int k = 0; k < CH; ++k) { if (gl[k] > 0.25f * peakl) ++nl; if (gh[k] > 0.25f * peakh) ++nh; }
        CHECK(peakh > peakl, "higher focus raises the peak speaker gain");
        CHECK(nh < nl, "higher focus lights fewer speakers above a quarter of peak");
        CHECK(fabs(sqrt(pl) - 1.0) < 0.02 && fabs(sqrt(ph) - 1.0) < 0.02,
              "constant power holds across focus values");

        /* density feeds the CACHED correction: reusing one state across a density change must
         * rebuild it, not silently keep the old c[] */
        float gd1[CH], gd2[CH];
        SpcapState sd; spcap_reset(&sd);
        spcap_gains(&sd, src2, lis, &LD, 1u, LD.spcap_focus, 1.0f, 1.0f, gd1);
        spcap_gains(&sd, src2, lis, &LD, 1u, LD.spcap_focus, 6.0f, 1.0f, gd2);
        double dmax = 0;
        for (int k = 0; k < CH; ++k) { double d = fabs((double)gd1[k] - gd2[k]); if (d > dmax) dmax = d; }
        CHECK(dmax > 1e-4, "a density change invalidates the cached placement correction");
    }

    /* 10. AllRAD bed decoder: builds, finite, energy ~ the sampling decode, localizes plane waves */
    {
        float dec[BWA_CHANNELS][BWA_AMBI_CH];
        int ok = allrad_build_decode(&LD, dec);
        CHECK(ok, "allrad_build_decode succeeds on the default grid");
        if (ok) {
            int finite = 1; double ediff = 0;
            for (int s = 0; s < CH; ++s) for (int k = 0; k < BWA_AMBI_CH; ++k) {
                if (!isfinite(dec[s][k])) finite = 0;
                int l = (int)floorf(sqrtf((float)k)); ediff += (double)dec[s][k]*dec[s][k]/(2*l+1);
            }
            CHECK(finite, "AllRAD matrix is finite");
            /* the sampling decode's ACTUAL diffuse energy (built from its formula, not a constant), so a
             * wrong normalization target in AllRAD would fail this rather than match a shared mistake */
            double esad = 0;
            for (int s = 0; s < CH; ++s) {
                float p[3] = { LD.speakers[s].pos[0] - LD.ref[0], LD.speakers[s].pos[1] - LD.ref[1],
                               LD.speakers[s].pos[2] - LD.ref[2] };       /* dirs from the array center */
                float pl = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
                float ad2[3] = { p[2]/pl, p[0]/pl, p[1]/pl }, ys[BWA_AMBI_CH]; ambi_encode_sn3d(ad2, ys);   /* (z,x,y): matches build_bed_decode */
                for (int k = 0; k < BWA_AMBI_CH; ++k) { int l = (int)floorf(sqrtf((float)k));
                    double d = (double)(2*l+1)*ys[k]/CH; esad += d*d/(2*l+1); }
            }
            CHECK(fabs(ediff - esad)/esad < 0.05, "AllRAD diffuse energy matches the sampling decode");
            int loc_ok = 1;
            float dirs[3][3] = { { 1, 0, 0 }, { 0, 1, 0.3f }, { -0.5f, 0.2f, -1 } };
            for (int t = 0; t < 3; ++t) {
                float* sd = dirs[t]; float sl = sqrtf(sd[0]*sd[0] + sd[1]*sd[1] + sd[2]*sd[2]);
                float s3[3] = { sd[0]/sl, sd[1]/sl, sd[2]/sl };
                float ad[3] = { s3[2], s3[0], s3[1] }, sh[BWA_AMBI_CH]; ambi_encode_sn3d(ad, sh);   /* (z,x,y): matches build_bed_decode */
                float rE[3] = { 0, 0, 0 };
                for (int s = 0; s < CH; ++s) {
                    float f = 0; for (int k = 0; k < BWA_AMBI_CH; ++k) f += dec[s][k]*sh[k];
                    float p[3] = { LD.speakers[s].pos[0] - LD.ref[0], LD.speakers[s].pos[1] - LD.ref[1],
                                   LD.speakers[s].pos[2] - LD.ref[2] };
                    float pl = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
                    float w = f*f; rE[0]+=w*p[0]/pl; rE[1]+=w*p[1]/pl; rE[2]+=w*p[2]/pl;
                }
                float rl = sqrtf(rE[0]*rE[0] + rE[1]*rE[1] + rE[2]*rE[2]);
                if (rl <= 0 || (rE[0]*s3[0] + rE[1]*s3[1] + rE[2]*s3[2])/rl < 0.9f) loc_ok = 0;  /* within ~25 deg */
            }
            CHECK(loc_ok, "AllRAD localizes plane waves toward their direction");

            /* max-rE weights (ambi_max_re_weights) lengthen the ENERGY VECTOR through the real
             * decode: |rE|/E with the tapered encode beats the raw encode for every test plane wave
             * — the acoustic claim behind bwa_set_max_re (sharper energy concentration = better
             * localization away from the sweet spot). */
            {
                float rw[BWA_AMBI_CH];
                ambi_max_re_weights(3, rw);
                int re_ok = 1;
                for (int t2 = 0; t2 < 3; ++t2) {
                    float* sd = dirs[t2]; float sl = sqrtf(sd[0]*sd[0] + sd[1]*sd[1] + sd[2]*sd[2]);
                    float s3[3] = { sd[0]/sl, sd[1]/sl, sd[2]/sl };
                    float ad[3] = { s3[2], s3[0], s3[1] }, sh[BWA_AMBI_CH]; ambi_encode_sn3d(ad, sh);
                    double rlen[2];
                    for (int m = 0; m < 2; ++m) {
                        double rE[3] = { 0, 0, 0 }, E = 0;
                        for (int s = 0; s < CH; ++s) {
                            float f = 0;
                            for (int k = 0; k < BWA_AMBI_CH; ++k) f += dec[s][k] * sh[k] * (m ? rw[k] : 1.f);
                            float p[3] = { LD.speakers[s].pos[0] - LD.ref[0], LD.speakers[s].pos[1] - LD.ref[1],
                                           LD.speakers[s].pos[2] - LD.ref[2] };
                            float pl = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
                            double w = (double)f * f;
                            E += w; rE[0] += w*p[0]/pl; rE[1] += w*p[1]/pl; rE[2] += w*p[2]/pl;
                        }
                        rlen[m] = E > 0 ? sqrt(rE[0]*rE[0] + rE[1]*rE[1] + rE[2]*rE[2]) / E : 0;
                    }
                    if (!(rlen[1] > rlen[0] + 0.01)) re_ok = 0;
                }
                CHECK(re_ok, "max-rE weights lengthen the energy vector through the AllRAD decode");
            }
        }

        /* imaginary pole speaker: a FLOOR-LESS array (the default grid minus its y=0 level) leaves a
         * > 60° hole at the nadir. The imaginary nadir speaker closes the hull there and its share is
         * discarded, so a straight-down plane wave decodes to (near) nothing instead of smearing full
         * power onto the bottom ring. The cube grid (nadir gap ~55°) takes the unfixed path above. */
        {
            Layout LH;
            memset(&LH, 0, sizeof LH);
            uint32_t nh = 0;
            for (uint32_t s = 0; s < LD.count; ++s) {
                if (LD.speakers[s].pos[1] < 0.1f) continue;    /* drop the floor level */
                LH.speakers[nh].pos[0] = LD.speakers[s].pos[0];
                LH.speakers[nh].pos[1] = LD.speakers[s].pos[1];
                LH.speakers[nh].pos[2] = LD.speakers[s].pos[2];
                LH.speakers[nh].gain_lin = 1.0f;
                ++nh;
            }
            LH.count = nh;                                     /* 17: the 1.5 m ring (8) + the ceiling (9) */
            layout_compute_ref(&LH);
            float dech[BWA_CHANNELS][BWA_AMBI_CH];
            int okh = allrad_build_decode(&LH, dech);
            CHECK(okh, "allrad_build_decode succeeds on a floor-less array");
            if (okh) {
                /* decoded energy of a plane wave from direction d (room axes) */
                double e_down = 0, e_side = 0;
                float dirs2[2][3] = { { 0, -1, 0 }, { 1, 0, 0 } };
                double* acc[2] = { &e_down, &e_side };
                for (int t = 0; t < 2; ++t) {
                    float ad[3] = { dirs2[t][2], dirs2[t][0], dirs2[t][1] }, sh[BWA_AMBI_CH];
                    ambi_encode_sn3d(ad, sh);                  /* (z,x,y): matches build_bed_decode */
                    for (uint32_t s = 0; s < nh; ++s) {
                        float f = 0; for (int k = 0; k < BWA_AMBI_CH; ++k) f += dech[s][k]*sh[k];
                        *acc[t] += (double)f * f;
                    }
                }
                CHECK(e_side > 0 && e_down < 0.25 * e_side,
                      "imaginary nadir speaker discards straight-down diffuse energy (no bottom-ring smear)");
            }
        }
    }

    /* 10b. EPAD bed decoder (epad.c): builds, finite, diffuse energy matched to the sampling
     *      decode (level-fair swap), localizes plane waves — and delivers THE property it exists
     *      for: a panned plane wave's decoded ENERGY is ~constant over direction on an irregular
     *      array, where the sampling decode over-energises dense speaker regions. Pinned as the
     *      loudness-vs-direction spread (CV) on a deliberately clustered array: EPAD's must come
     *      in far under the sampling decode's. */
    {
        float dep[BWA_CHANNELS][BWA_AMBI_CH];
        int ok = epad_build_decode(&LD, dep);
        CHECK(ok, "epad_build_decode succeeds on the default grid");
        if (ok) {
            int finite = 1; double ediff = 0;
            for (int s = 0; s < CH; ++s) for (int k = 0; k < BWA_AMBI_CH; ++k) {
                if (!isfinite(dep[s][k])) finite = 0;
                int l = (int)floorf(sqrtf((float)k)); ediff += (double)dep[s][k]*dep[s][k]/(2*l+1);
            }
            CHECK(finite, "EPAD matrix is finite");
            double esad = 0;
            for (int s = 0; s < CH; ++s) {
                float p[3] = { LD.speakers[s].pos[0] - LD.ref[0], LD.speakers[s].pos[1] - LD.ref[1],
                               LD.speakers[s].pos[2] - LD.ref[2] };
                float pl = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
                float ad2[3] = { p[2]/pl, p[0]/pl, p[1]/pl }, ys[BWA_AMBI_CH]; ambi_encode_sn3d(ad2, ys);
                for (int k = 0; k < BWA_AMBI_CH; ++k) { int l = (int)floorf(sqrtf((float)k));
                    double d = (double)(2*l+1)*ys[k]/CH; esad += d*d/(2*l+1); }
            }
            CHECK(fabs(ediff - esad)/esad < 0.05, "EPAD diffuse energy matches the sampling decode");
            int loc_ok = 1;
            float dirs[3][3] = { { 1, 0, 0 }, { 0, 1, 0.3f }, { -0.5f, 0.2f, -1 } };
            for (int t = 0; t < 3; ++t) {
                float* sd = dirs[t]; float sl = sqrtf(sd[0]*sd[0] + sd[1]*sd[1] + sd[2]*sd[2]);
                float s3[3] = { sd[0]/sl, sd[1]/sl, sd[2]/sl };
                float ad[3] = { s3[2], s3[0], s3[1] }, sh[BWA_AMBI_CH]; ambi_encode_sn3d(ad, sh);
                float rE[3] = { 0, 0, 0 };
                for (int s = 0; s < CH; ++s) {
                    float f = 0; for (int k = 0; k < BWA_AMBI_CH; ++k) f += dep[s][k]*sh[k];
                    float p[3] = { LD.speakers[s].pos[0] - LD.ref[0], LD.speakers[s].pos[1] - LD.ref[1],
                                   LD.speakers[s].pos[2] - LD.ref[2] };
                    float pl = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
                    float w = f*f; rE[0]+=w*p[0]/pl; rE[1]+=w*p[1]/pl; rE[2]+=w*p[2]/pl;
                }
                float rl = sqrtf(rE[0]*rE[0] + rE[1]*rE[1] + rE[2]*rE[2]);
                if (rl <= 0 || (rE[0]*s3[0] + rE[1]*s3[1] + rE[2]*s3[2])/rl < 0.9f) loc_ok = 0;
            }
            CHECK(loc_ok, "EPAD localizes plane waves toward their direction");
        }

        /* the energy-preserving claim, on a lopsided array: 10 speakers bunched into a ~45° cone
         * toward +x plus 6 covering the rest. E(d) = the decoded energy of a plane wave, swept
         * over a Fibonacci sphere; CV = std/mean of E over the sweep. */
        {
            Layout LC;
            memset(&LC, 0, sizeof LC);
            uint32_t nc = 0;
            for (int i = 0; i < 10; ++i) {                       /* the cluster: a cap around +x */
                float yy = -0.45f + 0.9f * ((float)i + 0.5f) / 10.f;
                float rr = sqrtf(1.f - yy * yy), th = (float)i * 2.39996323f;
                float d[3] = { 1.6f, yy, rr * sinf(th) * 0.45f };
                float dl = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
                LC.speakers[nc].pos[0] = 2.f * d[0] / dl;
                LC.speakers[nc].pos[1] = 1.5f + 2.f * d[1] / dl;
                LC.speakers[nc].pos[2] = 2.f * d[2] / dl;
                LC.speakers[nc].gain_lin = 1.f; ++nc;
            }
            const float rest[6][3] = { { -1,0,0 }, { 0,1,0 }, { 0,-1,0 }, { 0,0,1 }, { 0,0,-1 }, { -0.7f,0.7f,0 } };
            for (int i = 0; i < 6; ++i) {
                float dl = sqrtf(rest[i][0]*rest[i][0] + rest[i][1]*rest[i][1] + rest[i][2]*rest[i][2]);
                LC.speakers[nc].pos[0] = 2.f * rest[i][0] / dl;
                LC.speakers[nc].pos[1] = 1.5f + 2.f * rest[i][1] / dl;
                LC.speakers[nc].pos[2] = 2.f * rest[i][2] / dl;
                LC.speakers[nc].gain_lin = 1.f; ++nc;
            }
            LC.count = nc;                                       /* 16 */
            layout_compute_ref(&LC);
            float dep2[BWA_CHANNELS][BWA_AMBI_CH], dsad[BWA_CHANNELS][BWA_AMBI_CH];
            int okc = epad_build_decode(&LC, dep2);
            CHECK(okc, "epad_build_decode succeeds on the clustered array");
            ambi_sad_decode(&LC, LC.count, dsad);                /* the engine's actual sampling decode
                                                                  * (the foil guards allrad/epad's own
                                                                  * closed-form constant, not this) */
            if (okc) {
                double cv[2];
                float (*mats[2])[BWA_AMBI_CH] = { dep2, dsad };
                for (int m = 0; m < 2; ++m) {
                    enum { SWEEP = 128 };
                    double sum = 0, sum2 = 0;
                    for (int i = 0; i < SWEEP; ++i) {            /* Fibonacci sweep of plane waves */
                        float yy = 1.f - 2.f * ((float)i + 0.5f) / (float)SWEEP;
                        float rr = sqrtf(fmaxf(0.f, 1.f - yy * yy)), th = (float)i * 2.39996323f;
                        float d[3] = { rr * cosf(th), yy, rr * sinf(th) };
                        float ad[3] = { d[2], d[0], d[1] }, sh[BWA_AMBI_CH]; ambi_encode_sn3d(ad, sh);
                        double E = 0;
                        for (uint32_t s = 0; s < LC.count; ++s) {
                            float f = 0; for (int k = 0; k < BWA_AMBI_CH; ++k) f += mats[m][s][k]*sh[k];
                            E += (double)f * f;
                        }
                        sum += E; sum2 += E * E;
                    }
                    double mean = sum / 128.0, var = sum2 / 128.0 - mean * mean;
                    cv[m] = mean > 0 ? sqrt(var > 0 ? var : 0) / mean : 1e9;
                }
                printf("epad: clustered-array energy CV %.3f (EPAD) vs %.3f (sampling)\n", cv[0], cv[1]);
                CHECK(cv[0] < 0.5 * cv[1],
                      "EPAD flattens loudness-vs-direction on a clustered array (energy-preserving)");
            }
        }
    }

    /* 11. VBAP panner: localization, constant power, at most 3 active speakers (the containing triangle) */
    {
        VbapState vb; vbap_reset(&vb);
        float lis[3] = { LD.ref[0], LD.ref[1], LD.ref[2] }, g[CH];   /* inside the hull (the floor is ON it) */
        int loc_ok = 1;
        for (int k = 0; k < CH; ++k) {
            vbap_gains(&vb, LD.speakers[k].pos, lis, &LD, 1u, 1.0f, g);   /* source at speaker k's bearing */
            if (argmax(g, CH) != k) loc_ok = 0;
        }
        CHECK(loc_ok, "VBAP localizes a source at each speaker to that channel");

        float src[3] = { 0.7f, LD.ref[1] + 0.2f, 0.6f }, gain = 0.8f;      /* ds < 1 m -> atten = 1 */
        vbap_gains(&vb, src, lis, &LD, 1u, gain, g);
        double p = 0; int active = 0;
        for (int k = 0; k < CH; ++k) { p += (double)g[k] * g[k]; if (g[k] > 1e-4f) ++active; }
        CHECK(fabs(sqrt(p) - gain) < 0.02, "VBAP is constant-power (||g|| ~ user_gain)");
        CHECK(active >= 1 && active <= 3, "VBAP uses at most 3 speakers (the containing triangle)");
    }

    remove(LJ);
    if (fails) { printf("dsp_test: %d FAILURES\n", fails); return 1; }
    printf("dsp_test OK (layout parse, DBAP + SPCAP + VBAP, AllRAD + EPAD bed decodes + max-rE, align gain+delay)\n");
    return 0;
}
