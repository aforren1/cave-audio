/* calib_test.c — the calibration trim solve + layout writeback, verified without the rig. */
#include "calib.h"
#include "layout.h"        /* BWA_ROOM_EQ_MAX (the room_eq writeback round-trip) */
#include "sos.h"           /* the temperature parsers + the plausible-c guard */

#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
            CHECK(calib_write_positions(IN, OUT, pp, 3, e, sizeof e), e[0] ? e : "calib_write_positions");
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

    if (fails) { printf("calib_test: %d FAILURES\n", fails); return 1; }
    printf("calib_test OK (delay align, sensitivity EQ, dead-speaker guard, writeback, trilateration, positions, drift-check, speed-of-sound round trip verified)\n");
    return 0;
}
