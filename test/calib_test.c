/* calib_test.c — the calibration trim solve + layout writeback, verified without the rig. */
#include "calib.h"

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
    const char* IN = "bw_calib_in.json", *OUT = "bw_calib_out.json";
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

    if (fails) { printf("calib_test: %d FAILURES\n", fails); return 1; }
    printf("calib_test OK (delay alignment, sensitivity equalization, dead-speaker guard, JSON writeback verified)\n");
    return 0;
}
