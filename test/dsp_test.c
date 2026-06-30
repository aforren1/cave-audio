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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CH   BW_CHANNELS
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

int main(void) {
    /* 1. default layout */
    Layout LD = layout_default();
    CHECK(LD.count == CH, "default layout has 26 speakers");

    /* 2. layout_load parse */
    const char* LJ = "bw_layout.json";
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
        float lis[3] = { 0, 0, 0 }, g[CH];
        int ok = 1;
        for (int k = 0; k < CH; ++k) {
            dbap_gains(LD.speakers[k].pos, lis, &LD, 1.0f, g);
            if (argmax(g, CH) != k) ok = 0;
        }
        CHECK(ok, "DBAP localizes a source at each speaker to that channel");
    }

    /* 4. constant power: ||g|| ~ user_gain (atten == 1 within the reference distance) */
    {
        float lis[3] = { 0, 0, 0 }, src[3] = { 0.5f, 0.f, 0.5f }, g[CH];
        float gain = 0.8f;
        dbap_gains(src, lis, &LD, gain, g);
        double p = 0; for (int k = 0; k < CH; ++k) p += (double)g[k] * g[k];
        CHECK(fabs(sqrt(p) - gain) < 0.02, "DBAP is constant-power (||g|| ~ user_gain)");
    }

    /* 5. two-speaker split: a source midway between two speakers feeds both above average */
    {
        const int A = 7, B = 8;
        float lis[3] = { 0, 0, 0 }, g[CH];
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
        float src[3] = { 1.5f, 0.f, 0.f }, g0[CH], g1[CH];
        float lis0[3] = { 0, 0, 0 }, lis1[3] = { -1.0f, 0, 0 };
        dbap_gains(src, lis0, &LD, 1.0f, g0);
        dbap_gains(src, lis1, &LD, 1.0f, g1);
        double diff = 0; for (int k = 0; k < CH; ++k) diff += fabs(g0[k] - g1[k]);
        CHECK(diff > 1e-3, "moving the listener changes the gain distribution");
    }

    /* 7. align: gain trim halves a channel; delay shifts the impulse */
    {
        Layout AL = layout_default();
        AL.speakers[2].gain_lin = 0.5f;
        AL.speakers[3].delay_samples = 4;
        AL.max_delay_samples = 4;
        Aligner* a = align_create(CH, &AL);
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
        Aligner* a = align_create(CH, &EQ);
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

    /* 7b. layout_load rejects out-of-range values (so bad JSON can't reach the audio thread) */
    {
        const char* BJ = "bw_bad_layout.json";
        Layout B;
        write_layout_with(BJ, 0.0, 0.0, 0.0);     CHECK(!layout_load(BJ, RATE, &B, err, sizeof err), "rolloff_r=0 is rejected");
        write_layout_with(BJ, 0.7, 1000.0, 0.0);  CHECK(!layout_load(BJ, RATE, &B, err, sizeof err), "gain_db=1000 is rejected");
        write_layout_with(BJ, 0.7, 0.0, 1.0e7);   CHECK(!layout_load(BJ, RATE, &B, err, sizeof err), "huge delay_ms is rejected");
        write_layout_with(BJ, 0.7, 0.0, 0.0);     CHECK( layout_load(BJ, RATE, &B, err, sizeof err), "valid layout still loads");
        remove(BJ);
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
        float lis[3] = { 0, 0, 0 }, g[CH];
        int loc_ok = 1, nonneg = 1;
        for (int k = 0; k < CH; ++k) {
            spcap_gains(&sp, LD.speakers[k].pos, lis, &LD, 1u, 1.0f, g);   /* source at speaker k's bearing */
            if (argmax(g, CH) != k) loc_ok = 0;
            for (int i = 0; i < CH; ++i) if (g[i] < -1e-6f) nonneg = 0;
        }
        CHECK(loc_ok, "SPCAP localizes a source at each speaker to that channel");
        CHECK(nonneg, "SPCAP gains are non-negative");

        float src[3] = { 0.5f, 0.f, 0.5f }, gain = 0.8f;
        spcap_gains(&sp, src, lis, &LD, 1u, gain, g);
        double p = 0; int active = 0;
        for (int k = 0; k < CH; ++k) { p += (double)g[k] * g[k]; if (g[k] > 0.05f * gain) ++active; }
        CHECK(fabs(sqrt(p) - gain) < 0.02, "SPCAP is constant-power (||g|| ~ user_gain)");
        CHECK(active >= 3 && active <= 20, "SPCAP spreads across several speakers (not 1, not all)");
    }

    /* 10. AllRAD bed decoder: builds, finite, energy ~ the sampling decode, localizes plane waves */
    {
        float dec[BW_CHANNELS][BW_AMBI_CH];
        int ok = allrad_build_decode(&LD, dec);
        CHECK(ok, "allrad_build_decode succeeds on the default grid");
        if (ok) {
            int finite = 1; double ediff = 0;
            for (int s = 0; s < CH; ++s) for (int k = 0; k < BW_AMBI_CH; ++k) {
                if (!isfinite(dec[s][k])) finite = 0;
                int l = (int)floorf(sqrtf((float)k)); ediff += (double)dec[s][k]*dec[s][k]/(2*l+1);
            }
            CHECK(finite, "AllRAD matrix is finite");
            /* the sampling decode's ACTUAL diffuse energy (built from its formula, not a constant), so a
             * wrong normalization target in AllRAD would fail this rather than match a shared mistake */
            double esad = 0;
            for (int s = 0; s < CH; ++s) {
                float* p = LD.speakers[s].pos; float pl = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
                float ad2[3] = { -p[2]/pl, -p[0]/pl, p[1]/pl }, ys[BW_AMBI_CH]; ambi_encode_sn3d(ad2, ys);
                for (int k = 0; k < BW_AMBI_CH; ++k) { int l = (int)floorf(sqrtf((float)k));
                    double d = (double)(2*l+1)*ys[k]/CH; esad += d*d/(2*l+1); }
            }
            CHECK(fabs(ediff - esad)/esad < 0.05, "AllRAD diffuse energy matches the sampling decode");
            int loc_ok = 1;
            float dirs[3][3] = { { 1, 0, 0 }, { 0, 1, 0.3f }, { -0.5f, 0.2f, -1 } };
            for (int t = 0; t < 3; ++t) {
                float* sd = dirs[t]; float sl = sqrtf(sd[0]*sd[0] + sd[1]*sd[1] + sd[2]*sd[2]);
                float s3[3] = { sd[0]/sl, sd[1]/sl, sd[2]/sl };
                float ad[3] = { -s3[2], -s3[0], s3[1] }, sh[BW_AMBI_CH]; ambi_encode_sn3d(ad, sh);
                float rE[3] = { 0, 0, 0 };
                for (int s = 0; s < CH; ++s) {
                    float f = 0; for (int k = 0; k < BW_AMBI_CH; ++k) f += dec[s][k]*sh[k];
                    float* p = LD.speakers[s].pos; float pl = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
                    float w = f*f; rE[0]+=w*p[0]/pl; rE[1]+=w*p[1]/pl; rE[2]+=w*p[2]/pl;
                }
                float rl = sqrtf(rE[0]*rE[0] + rE[1]*rE[1] + rE[2]*rE[2]);
                if (rl <= 0 || (rE[0]*s3[0] + rE[1]*s3[1] + rE[2]*s3[2])/rl < 0.9f) loc_ok = 0;  /* within ~25 deg */
            }
            CHECK(loc_ok, "AllRAD localizes plane waves toward their direction");
        }
    }

    /* 11. VBAP panner: localization, constant power, at most 3 active speakers (the containing triangle) */
    {
        VbapState vb; vbap_reset(&vb);
        float lis[3] = { 0, 0, 0 }, g[CH];
        int loc_ok = 1;
        for (int k = 0; k < CH; ++k) {
            vbap_gains(&vb, LD.speakers[k].pos, lis, &LD, 1u, 1.0f, g);   /* source at speaker k's bearing */
            if (argmax(g, CH) != k) loc_ok = 0;
        }
        CHECK(loc_ok, "VBAP localizes a source at each speaker to that channel");

        float src[3] = { 0.7f, 0.2f, 0.6f }, gain = 0.8f;                  /* ds < 1 m -> atten = 1 */
        vbap_gains(&vb, src, lis, &LD, 1u, gain, g);
        double p = 0; int active = 0;
        for (int k = 0; k < CH; ++k) { p += (double)g[k] * g[k]; if (g[k] > 1e-4f) ++active; }
        CHECK(fabs(sqrt(p) - gain) < 0.02, "VBAP is constant-power (||g|| ~ user_gain)");
        CHECK(active >= 1 && active <= 3, "VBAP uses at most 3 speakers (the containing triangle)");
    }

    remove(LJ);
    if (fails) { printf("dsp_test: %d FAILURES\n", fails); return 1; }
    printf("dsp_test OK (layout parse, DBAP + SPCAP + VBAP, AllRAD bed decode, align gain+delay)\n");
    return 0;
}
