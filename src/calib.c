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

/* Gaussian elimination with partial pivoting on a 4x4 system A*x = b. Returns 0 if singular. */
static int solve4(double A[4][4], double b[4], double x[4]) {
    for (int c = 0; c < 4; ++c) {
        int piv = c;
        for (int r = c + 1; r < 4; ++r) if (fabs(A[r][c]) > fabs(A[piv][c])) piv = r;
        if (fabs(A[piv][c]) < 1e-12) return 0;
        if (piv != c) {
            for (int j = 0; j < 4; ++j) { double t = A[c][j]; A[c][j] = A[piv][j]; A[piv][j] = t; }
            double t = b[c]; b[c] = b[piv]; b[piv] = t;
        }
        for (int r = c + 1; r < 4; ++r) {
            double f = A[r][c] / A[c][c];
            for (int j = c; j < 4; ++j) A[r][j] -= f * A[c][j];
            b[r] -= f * b[c];
        }
    }
    for (int r = 3; r >= 0; --r) {
        double s = b[r];
        for (int j = r + 1; j < 4; ++j) s -= A[r][j] * x[j];
        x[r] = s / A[r][r];
    }
    return 1;
}

int calib_trilaterate(const double* range, const float (*mic)[3], int K, float* pos_out, double* latency_out) {
    if (!range || !mic || !pos_out || K < 5) return 0;
    /* |x - m_k| = range[k] - r, with r = c*tau the unknown latency-range. Squaring and subtracting the
     * k=0 equation cancels |x|^2 and r^2, leaving a system that's LINEAR in (x, r):
     *   2*x.(m_k - m_0) - 2*r*(range[k]-range[0]) = |m_k|^2 - |m_0|^2 - range[k]^2 + range[0]^2.
     * Stack k=1..K-1 and solve the 4x4 normal equations by least squares. */
    const double m0[3] = { mic[0][0], mic[0][1], mic[0][2] };
    const double r0 = range[0];
    const double n0 = m0[0]*m0[0] + m0[1]*m0[1] + m0[2]*m0[2];
    double AtA[4][4] = {{0}}, Atb[4] = {0};
    for (int k = 1; k < K; ++k) {
        double mk[3] = { mic[k][0], mic[k][1], mic[k][2] };
        double row[4] = { 2.0*(mk[0]-m0[0]), 2.0*(mk[1]-m0[1]), 2.0*(mk[2]-m0[2]), -2.0*(range[k]-r0) };
        double nk  = mk[0]*mk[0] + mk[1]*mk[1] + mk[2]*mk[2];
        double rhs = nk - n0 - range[k]*range[k] + r0*r0;
        for (int a = 0; a < 4; ++a) { for (int bb = 0; bb < 4; ++bb) AtA[a][bb] += row[a]*row[bb]; Atb[a] += row[a]*rhs; }
    }
    double th[4];
    if (!solve4(AtA, Atb, th)) return 0;
    pos_out[0] = (float)th[0]; pos_out[1] = (float)th[1]; pos_out[2] = (float)th[2];
    if (latency_out) *latency_out = th[3];
    return 1;
}

static int dcmp(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b; return x < y ? -1 : x > y ? 1 : 0;
}

void calib_check_drift(const double* range, const float (*pos)[3], const float mic[3], int n, float* deviation_m) {
    if (!range || !pos || !mic || !deviation_m || n <= 0) return;
    double* resid = (double*)malloc((size_t)n * sizeof(double));
    double* tmp   = (double*)malloc((size_t)n * sizeof(double));
    if (!resid || !tmp) { free(resid); free(tmp); return; }
    for (int s = 0; s < n; ++s) {                              /* residual = range - expected distance = the common latency if unmoved */
        double dx = pos[s][0]-mic[0], dy = pos[s][1]-mic[1], dz = pos[s][2]-mic[2];
        resid[s] = range[s] - sqrt(dx*dx + dy*dy + dz*dz);
        tmp[s] = resid[s];
    }
    qsort(tmp, (size_t)n, sizeof(double), dcmp);              /* median residual = the system latency (outlier-robust) */
    double med = (n & 1) ? tmp[n/2] : 0.5 * (tmp[n/2 - 1] + tmp[n/2]);
    for (int s = 0; s < n; ++s) deviation_m[s] = (float)(resid[s] - med);
    free(resid); free(tmp);
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

int calib_write_positions(const char* in_path, const char* out_path, const float (*pos)[3], int n, char* err, size_t errcap) {
    #define FAIL(msg) do { if (err && errcap) snprintf(err, errcap, "%s", msg); goto fail; } while (0)
    char* text = NULL; cJSON* root = NULL; char* outtext = NULL; int ok = 0;

    text = read_file(in_path, NULL);
    if (!text) { if (err && errcap) snprintf(err, errcap, "calib: cannot read %s", in_path); return 0; }
    root = cJSON_Parse(text);
    if (!root) FAIL("calib: layout is not valid JSON");
    cJSON* speakers = cJSON_GetObjectItemCaseSensitive(root, "speakers");
    if (!cJSON_IsArray(speakers)) FAIL("calib: layout has no 'speakers' array");
    if (cJSON_GetArraySize(speakers) != n) FAIL("calib: speaker count does not match the positions");

    for (int i = 0; i < n; ++i) {
        cJSON* sp = cJSON_GetArrayItem(speakers, i);
        double xyz[3] = { round(pos[i][0]*1000.0)/1000.0, round(pos[i][1]*1000.0)/1000.0, round(pos[i][2]*1000.0)/1000.0 };
        cJSON* p = cJSON_GetObjectItemCaseSensitive(sp, "position");
        if (cJSON_IsArray(p) && cJSON_GetArraySize(p) == 3) {
            for (int j = 0; j < 3; ++j) cJSON_SetNumberValue(cJSON_GetArrayItem(p, j), xyz[j]);
        } else {
            cJSON_DeleteItemFromObjectCaseSensitive(sp, "position");
            cJSON* arr = cJSON_CreateArray();
            for (int j = 0; j < 3; ++j) cJSON_AddItemToArray(arr, cJSON_CreateNumber(xyz[j]));
            cJSON_AddItemToObject(sp, "position", arr);
        }
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
