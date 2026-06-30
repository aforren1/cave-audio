/* calib.c — see calib.h. Trim solve + cave_layout.json writeback. Pure + file I/O (no audio thread). */
#include "calib.h"

#include <cJSON.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void calib_solve(const MeasureResult* m, const float (*pos)[3], const float mic[3], int n, double fs,
                 float* gain_db, float* delay_ms) {
    if (!m || !pos || !mic || !gain_db || !delay_ms || n <= 0) return;

    /* delays: align every arrival to the farthest (largest measured delay). Latency cancels. */
    int maxd = 0;
    for (int i = 0; i < n; ++i) if (m[i].delay_samples > maxd) maxd = m[i].delay_samples;
    for (int i = 0; i < n; ++i) {
        double ms = (maxd - m[i].delay_samples) / fs * 1000.0;
        if (ms < 0.0) ms = 0.0; else if (ms > 1000.0) ms = 1000.0;
        delay_ms[i] = (float)(round(ms * 1000.0) / 1000.0);
    }

    /* sensitivity = level * distance (factor out 1/r); equalize cut-only to the least-sensitive live
     * speaker so no channel is boosted. A near-silent speaker is excluded + left at 0 dB. */
    double sref = 1e30;
    for (int i = 0; i < n; ++i) {
        double dx = pos[i][0]-mic[0], dy = pos[i][1]-mic[1], dz = pos[i][2]-mic[2];
        double dist = sqrt(dx*dx + dy*dy + dz*dz); if (dist < 0.05) dist = 0.05;
        double s = (double)m[i].level * dist;
        if (m[i].level > 1e-3 && s < sref) sref = s;
    }
    if (sref >= 1e30) sref = 1.0;                 /* every speaker silent: leave trims at unity */
    for (int i = 0; i < n; ++i) {
        if (m[i].level <= 1e-3) { gain_db[i] = 0.f; continue; }
        double dx = pos[i][0]-mic[0], dy = pos[i][1]-mic[1], dz = pos[i][2]-mic[2];
        double dist = sqrt(dx*dx + dy*dy + dz*dz); if (dist < 0.05) dist = 0.05;
        double s  = (double)m[i].level * dist;
        double db = 20.0 * log10(sref / s);       /* sref <= s, so db <= 0 (cut-only) */
        if (db > 0.0) db = 0.0; else if (db < -40.0) db = -40.0;
        gain_db[i] = (float)(round(db * 100.0) / 100.0);
    }
}

static char* read_file(const char* path, long* len_out) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len < 0) { fclose(f); return NULL; }
    char* buf = (char*)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)len, f); fclose(f);
    buf[rd] = '\0'; if (len_out) *len_out = (long)rd;
    return buf;
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

    for (int i = 0; i < n; ++i) {
        cJSON* sp = cJSON_GetArrayItem(speakers, i);
        cJSON* g  = cJSON_GetObjectItemCaseSensitive(sp, "gain_db");
        cJSON* d  = cJSON_GetObjectItemCaseSensitive(sp, "delay_ms");
        if (g) cJSON_SetNumberValue(g, gain_db[i]);  else cJSON_AddNumberToObject(sp, "gain_db",  gain_db[i]);
        if (d) cJSON_SetNumberValue(d, delay_ms[i]); else cJSON_AddNumberToObject(sp, "delay_ms", delay_ms[i]);
    }

    outtext = cJSON_Print(root);
    if (!outtext) FAIL("calib: failed to serialize layout");
    FILE* f = fopen(out_path, "wb");
    if (!f) FAIL("calib: cannot open output for writing");
    fwrite(outtext, 1, strlen(outtext), f); fclose(f);
    ok = 1;
fail:
    free(outtext); cJSON_Delete(root); free(text);
    return ok;
    #undef FAIL
}
