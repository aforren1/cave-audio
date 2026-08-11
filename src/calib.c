/* calib.c — see calib.h. Trim solve + cave_layout.json writeback. Pure + file I/O (no audio thread). */
#include "calib.h"
#include "layout.h"        /* BWA_RQ_GRID_MAX / BWA_ROOM_EQ_MAX (the room_eq_grid schema caps) */
#include "sos.h"           /* BWA_SOS_MIN_MPS / BWA_SOS_MAX_MPS (the plausible-room guard) */

#include <cJSON.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void calib_solve(const MeasureResult* m, const float (*pos)[3], const float mic[3], int n, double fs,
                 float* gain_db, float* delay_ms) {
    if (!m || !pos || !mic || !gain_db || !delay_ms || n <= 0 || !(fs > 0.0)) return;

    /* delays: align every arrival to the farthest (largest measured delay). Latency cancels. */
    int maxd = 0;
    for (int i = 0; i < n; ++i) if (m[i].delay_samples > maxd) maxd = m[i].delay_samples;
    for (int i = 0; i < n; ++i) {
        double ms = (maxd - m[i].delay_samples) / fs * 1000.0;
        if (!(ms > 0.0)) ms = 0.0; else if (ms > 1000.0) ms = 1000.0;   /* NaN-safe */
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
        /* NaN-safe skip: `level <= 1e-3` is FALSE for a NaN level (an unplugged/broken capture leg
         * delivers non-finite samples straight through deconvolution), which used to fall through
         * into the solve and write gain_db = NaN — serialized as `null`, destroying the layout. */
        if (!(m[i].level > 1e-3)) { gain_db[i] = 0.f; continue; }
        double dx = pos[i][0]-mic[0], dy = pos[i][1]-mic[1], dz = pos[i][2]-mic[2];
        double dist = sqrt(dx*dx + dy*dy + dz*dz); if (dist < 0.05) dist = 0.05;
        double s  = (double)m[i].level * dist;
        double db = 20.0 * log10(sref / s);       /* sref <= s, so db <= 0 (cut-only) */
        if (!(db < 0.0)) db = 0.0; else if (db < -40.0) db = -40.0;   /* NaN-safe */
        gain_db[i] = (float)(round(db * 100.0) / 100.0);
    }
}

/* Gaussian elimination with partial pivoting on a 4x4 system A*x = b. Returns 0 if singular. */
static int solve4(double A[4][4], double b[4], double x[4]) {
    for (int c = 0; c < 4; ++c) {
        int piv = c;
        for (int r = c + 1; r < 4; ++r) if (fabs(A[r][c]) > fabs(A[piv][c])) piv = r;
        if (!(fabs(A[piv][c]) >= 1e-12)) return 0;   /* NaN-safe: a NaN pivot (NaN mic positions from a
                                                      * text file) must read SINGULAR, not solvable */
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

    /* refuse values the loader is KNOWN to reject before touching the file: a NaN trim (a broken
     * capture leg survives the solve as NaN) serializes as JSON `null`, and out_path usually IS the
     * layout — the write would destroy the good calibration in place and only fail at next load */
    for (int i = 0; i < n; ++i)
        if (!isfinite(gain_db[i]) || gain_db[i] < -100.f || gain_db[i] > 24.f ||
            !isfinite(delay_ms[i]) || delay_ms[i] < 0.f || delay_ms[i] > 1000.f)
            FAIL("calib: refusing to write a non-finite/out-of-range trim (bad measurement?)");

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

int calib_read_sos(const char* path, double* out_mps) {
    if (!path || !out_mps) return 0;
    char* text = read_file(path, NULL);
    if (!text) return 0;
    cJSON* root = cJSON_Parse(text);
    int ok = 0;
    if (root) {
        cJSON* ref = cJSON_GetObjectItemCaseSensitive(root, "reference");
        if (cJSON_IsObject(ref)) {
            cJSON* c = cJSON_GetObjectItemCaseSensitive(ref, "speed_of_sound_mps");
            if (cJSON_IsNumber(c) && c->valuedouble >= BWA_SOS_MIN_MPS && c->valuedouble <= BWA_SOS_MAX_MPS) {
                *out_mps = c->valuedouble; ok = 1;
            }
        }
        cJSON_Delete(root);
    }
    free(text);
    return ok;
}

int calib_write_sos(const char* in_path, const char* out_path, double mps, char* err, size_t errcap) {
    #define FAIL(msg) do { if (err && errcap) snprintf(err, errcap, "%s", msg); goto fail; } while (0)
    char* text = NULL; cJSON* root = NULL; char* outtext = NULL; int ok = 0;
    if (!(mps >= BWA_SOS_MIN_MPS && mps <= BWA_SOS_MAX_MPS)) {
        if (err && errcap) snprintf(err, errcap, "calib: speed of sound %.1f m/s out of range", mps);
        return 0;
    }
    text = read_file(in_path, NULL);
    if (!text) { if (err && errcap) snprintf(err, errcap, "calib: cannot read %s", in_path); return 0; }
    root = cJSON_Parse(text);
    if (!root) FAIL("calib: layout is not valid JSON");
    /* the `reference` block is provenance: create it if the file predates this field */
    cJSON* ref = cJSON_GetObjectItemCaseSensitive(root, "reference");
    if (!cJSON_IsObject(ref)) { cJSON_DeleteItemFromObjectCaseSensitive(root, "reference");
                                ref = cJSON_AddObjectToObject(root, "reference"); }
    if (!ref) FAIL("calib: cannot create the 'reference' block");
    /* 2 decimals: 0.01 m/s is 3e-5 of c, about 0.1 mm on a 4 m range — far below anything that
     * matters, and it keeps the file readable instead of carrying a float artifact. */
    mps = round(mps * 100.0) / 100.0;
    cJSON* c = cJSON_GetObjectItemCaseSensitive(ref, "speed_of_sound_mps");
    if (c) cJSON_SetNumberValue(c, mps); else cJSON_AddNumberToObject(ref, "speed_of_sound_mps", mps);

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

int calib_eq(const float* ir, int nir, int first_refl, double fs, int ntaps, float* taps) {
    if (!ir || !taps) return 0;
    /* gate to just before the first reflection so we invert the SPEAKER, not the room; if unknown, a
     * 4 ms window (long enough to resolve the speaker's response, short enough to exclude most rooms). */
    int gate = (first_refl > 8) ? first_refl - 4 : (int)(0.004 * fs);
    if (gate > nir) gate = nir;
    if (gate < 16) return 0;
    return measure_correction(ir, nir, 0, gate, 30.0, 18000.0, fs, 6.0, 18.0, ntaps, taps);
}

int calib_write_eq(const char* in_path, const char* out_path, const float* taps, const uint16_t* lens,
                   int n, int max_taps, char* err, size_t errcap) {
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
        cJSON_DeleteItemFromObjectCaseSensitive(sp, "eq");     /* replace any prior correction */
        int m = lens[i]; if (m > max_taps) m = max_taps;
        if (m > 0) {
            cJSON* arr = cJSON_CreateArray();
            if (!arr) FAIL("calib: eq array alloc");
            for (int t = 0; t < m; ++t) cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)taps[(size_t)i*max_taps + t]));
            cJSON_AddItemToObject(sp, "eq", arr);
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

#define BWA_ROOM_EQ_SPLIT_HZ 200.0   /* the FIR corrects above this; the modal cuts own [30, split] */

int calib_room_eq(const float* ir, int nir, int first_refl, double fs, int ntaps, float* taps,
                  MeasureEqSection* cuts, int max_cuts) {
    if (!ir || !taps || !cuts) return -1;
    /* same HF gate policy as calib_eq: at high frequencies the FD window shrinks to the direct sound */
    int gate = (first_refl > 8) ? first_refl - 4 : (int)(0.004 * fs);
    if (gate > nir) gate = nir;
    if (gate < 16) return -1;
    /* 6 cycles/f window (~1/6-octave resolution — the broad-stroke smoothing that survives head sway),
     * up to 400 ms of room; boosts capped at +3 dB (a seated head still sways — never fight nulls hard). */
    if (!measure_correction_room(ir, nir, 0, gate, 6.0, 0.4,
                                 BWA_ROOM_EQ_SPLIT_HZ, 18000.0, fs, 3.0, 18.0, ntaps, taps)) return -1;
    return measure_room_cuts(ir, nir, 0, fs, 30.0, BWA_ROOM_EQ_SPLIT_HZ, 12.0, max_cuts, cuts);
}

int calib_write_room_eq(const char* in_path, const char* out_path,
                        const MeasureEqSection* cuts, const int* counts, int n,
                        int max_sections, char* err, size_t errcap) {
    #define FAIL(msg) do { if (err && errcap) snprintf(err, errcap, "%s", msg); goto fail; } while (0)
    char* text = NULL; cJSON* root = NULL; char* outtext = NULL; int ok = 0;
    text = read_file(in_path, NULL);
    if (!text) { if (err && errcap) snprintf(err, errcap, "calib: cannot read %s", in_path); return 0; }
    root = cJSON_Parse(text);
    if (!root) FAIL("calib: layout is not valid JSON");
    cJSON* speakers = cJSON_GetObjectItemCaseSensitive(root, "speakers");
    if (!cJSON_IsArray(speakers)) FAIL("calib: layout has no 'speakers' array");
    if (cJSON_GetArraySize(speakers) != n) FAIL("calib: speaker count does not match the measurements");

    /* same refusal as the trims writer (calib_write_layout): a NaN fit serializes as JSON `null`,
     * and out_path usually IS the layout — the write would destroy the good calibration in place */
    for (int i = 0; i < n; ++i) {
        int m = counts[i]; if (m > max_sections) m = max_sections;
        for (int s = 0; s < m; ++s) {
            const MeasureEqSection* c = &cuts[(size_t)i * max_sections + s];
            if (!isfinite(c->fc) || !isfinite(c->gain_db) || !isfinite(c->q))
                FAIL("calib: refusing to write a non-finite room-EQ section (bad measurement?)");
        }
    }

    for (int i = 0; i < n; ++i) {
        cJSON* sp = cJSON_GetArrayItem(speakers, i);
        cJSON_DeleteItemFromObjectCaseSensitive(sp, "room_eq");   /* replace any prior */
        int m = counts[i]; if (m > max_sections) m = max_sections;
        if (m > 0) {
            cJSON* arr = cJSON_CreateArray();
            if (!arr) FAIL("calib: room_eq array alloc");
            for (int s = 0; s < m; ++s) {
                const MeasureEqSection* c = &cuts[(size_t)i * max_sections + s];
                cJSON* o = cJSON_CreateObject();
                if (!o) FAIL("calib: room_eq section alloc");
                cJSON_AddNumberToObject(o, "fc",      round((double)c->fc * 10.0) / 10.0);
                cJSON_AddNumberToObject(o, "gain_db", round((double)c->gain_db * 100.0) / 100.0);
                cJSON_AddNumberToObject(o, "q",       round((double)c->q * 100.0) / 100.0);
                cJSON_AddItemToArray(arr, o);
            }
            cJSON_AddItemToObject(sp, "room_eq", arr);
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

static int fcmp(const void* a, const void* b) {
    float x = *(const float*)a, y = *(const float*)b; return x < y ? -1 : x > y ? 1 : 0;
}
static float fmedian(float* v, int n) {   /* sorts v in place */
    qsort(v, (size_t)n, sizeof(float), fcmp);
    return (n & 1) ? v[n/2] : 0.5f * (v[n/2 - 1] + v[n/2]);
}

int calib_room_grid_merge(const MeasureEqSection* cuts, const int* counts, int npos, int max_in,
                          double tol_rel, int max_out, float* fc, float* q, float* gain_db) {
    if (!cuts || !counts || !fc || !q || !gain_db || npos <= 0 || max_in <= 0 || max_out <= 0) return 0;
    if (npos > (int)BWA_RQ_GRID_MAX || max_in > BWA_ROOM_EQ_MAX) return 0;   /* schema caps bound the stack arrays */
    memset(gain_db, 0, (size_t)npos * (size_t)max_out * sizeof(float));

    /* flatten every real cut (a 0 dB section is congruence filler, not a measured mode) */
    int cap = npos * max_in, m = 0;
    float* ifc = (float*)malloc((size_t)cap * 3 * sizeof(float));
    int*   ip  = (int*)  malloc((size_t)cap * sizeof(int));
    if (!ifc || !ip) { free(ifc); free(ip); return 0; }
    float* iq = ifc + cap, *ig = ifc + 2 * cap;
    for (int p = 0; p < npos; ++p) {
        int cnum = counts[p] > max_in ? max_in : counts[p];
        for (int s = 0; s < cnum; ++s) {
            const MeasureEqSection* c = &cuts[(size_t)p * max_in + s];
            if (!(c->fc > 0.f) || !(c->q > 0.f) || c->gain_db > -0.05f) continue;
            ifc[m] = c->fc; iq[m] = c->q; ig[m] = c->gain_db; ip[m] = p; ++m;
        }
    }
    if (!m) { free(ifc); free(ip); return 0; }

    for (int i = 1; i < m; ++i) {                            /* insertion sort by fc (m <= npos*max_in, tiny) */
        float tf = ifc[i], tq = iq[i], tg = ig[i]; int tp = ip[i]; int j = i - 1;
        while (j >= 0 && ifc[j] > tf) { ifc[j+1]=ifc[j]; iq[j+1]=iq[j]; ig[j+1]=ig[j]; ip[j+1]=ip[j]; --j; }
        ifc[j+1] = tf; iq[j+1] = tq; ig[j+1] = tg; ip[j+1] = tp;
    }

    /* greedy clusters over the sorted list: a run belongs together while fc stays within tol_rel of
     * the run's lowest member (the same room mode read from different mic spots) */
    enum { CLU_CAP = BWA_RQ_GRID_MAX * BWA_ROOM_EQ_MAX };
    int cstart[CLU_CAP], clen[CLU_CAP], nclu = 0;
    for (int i = 0; i < m; ) {
        int j = i + 1;
        while (j < m && ifc[j] <= ifc[i] * (float)(1.0 + tol_rel)) ++j;
        cstart[nclu] = i; clen[nclu] = j - i; ++nclu;
        i = j;
    }

    float cfc[CLU_CAP], cq[CLU_CAP], cdepth[CLU_CAP]; int keep[CLU_CAP];
    for (int cI = 0; cI < nclu; ++cI) {                      /* per cluster: median fc/q, deepest member */
        float tmp[CLU_CAP];
        int s0 = cstart[cI], L = clen[cI];
        memcpy(tmp, &ifc[s0], (size_t)L * sizeof(float)); cfc[cI] = fmedian(tmp, L);
        memcpy(tmp, &iq[s0],  (size_t)L * sizeof(float)); cq[cI]  = fmedian(tmp, L);
        float d = 0.f;
        for (int i = s0; i < s0 + L; ++i) if (ig[i] < d) d = ig[i];
        cdepth[cI] = d;
        keep[cI] = 1;
    }
    int nsel = nclu;
    while (nsel > max_out) {                                 /* over the ladder cap: drop the shallowest */
        int worst = -1; float wd = -1e9f;
        for (int cI = 0; cI < nclu; ++cI)
            if (keep[cI] && cdepth[cI] > wd) { wd = cdepth[cI]; worst = cI; }
        keep[worst] = 0; --nsel;
    }

    int j = 0;
    for (int cI = 0; cI < nclu; ++cI) {                      /* emit in fc order (the list is fc-sorted) */
        if (!keep[cI]) continue;
        fc[j] = cfc[cI]; q[j] = cq[cI];
        for (int i = cstart[cI]; i < cstart[cI] + clen[cI]; ++i) {
            float* g = &gain_db[(size_t)ip[i] * max_out + j];
            if (ig[i] < *g) *g = ig[i];                      /* a position's deepest read of this mode */
        }
        ++j;
    }
    free(ifc); free(ip);
    return j;
}

int calib_write_room_eq_grid(const char* in_path, const char* out_path, const float mic[3],
                             const MeasureEqSection* cuts, const int* counts, int n,
                             int max_sections, char* err, size_t errcap) {
    #define FAIL(msg) do { if (err && errcap) snprintf(err, errcap, "%s", msg); goto fail; } while (0)
    char* text = NULL; cJSON* root = NULL; char* outtext = NULL; int ok = 0;
    cJSON* narr = NULL;             /* the rebuilt grid; owned here until attached to root */
    MeasureEqSection* all = NULL;   /* [pos][speaker][BWA_ROOM_EQ_MAX] — every position's cuts */
    int* acount = NULL;             /* [pos][speaker] used sections */
    float gpos[BWA_RQ_GRID_MAX][3]; int npos = 0;

    text = read_file(in_path, NULL);
    if (!text) { if (err && errcap) snprintf(err, errcap, "calib: cannot read %s", in_path); return 0; }
    root = cJSON_Parse(text);
    if (!root) FAIL("calib: layout is not valid JSON");
    cJSON* speakers = cJSON_GetObjectItemCaseSensitive(root, "speakers");
    if (!cJSON_IsArray(speakers)) FAIL("calib: layout has no 'speakers' array");
    if (cJSON_GetArraySize(speakers) != n) FAIL("calib: speaker count does not match the measurements");

    /* same refusal as the trims writer: the schema clamps below are two-sided compares that PASS
     * NaN, so a NaN fit would serialize as JSON `null` and destroy the layout in place */
    if (!isfinite(mic[0]) || !isfinite(mic[1]) || !isfinite(mic[2]))
        FAIL("calib: refusing to write a non-finite mic position");
    for (int s = 0; s < n; ++s) {
        int m = counts[s] > BWA_ROOM_EQ_MAX ? BWA_ROOM_EQ_MAX : counts[s];
        if (m > max_sections) m = max_sections;
        for (int t = 0; t < m; ++t) {
            const MeasureEqSection* c = &cuts[(size_t)s * max_sections + t];
            if (!isfinite(c->fc) || !isfinite(c->gain_db) || !isfinite(c->q))
                FAIL("calib: refusing to write a non-finite room-EQ section (bad measurement?)");
        }
    }

    all    = (MeasureEqSection*)calloc((size_t)BWA_RQ_GRID_MAX * n * BWA_ROOM_EQ_MAX, sizeof *all);
    acount = (int*)calloc((size_t)BWA_RQ_GRID_MAX * n, sizeof *acount);
    if (!all || !acount) FAIL("calib: out of memory");
    #define ALL(p, s)    (&all[((size_t)(p) * n + (s)) * BWA_ROOM_EQ_MAX])
    #define ACOUNT(p, s) (acount[(size_t)(p) * n + (s)])

    /* read back the existing grid: each entry's sections become that position's cuts for the
     * re-merge (its 0 dB congruence fillers are skipped by calib_room_grid_merge) */
    cJSON* grid = cJSON_GetObjectItemCaseSensitive(root, "room_eq_grid");
    if (cJSON_IsArray(grid)) {
        int np = cJSON_GetArraySize(grid);
        if (np > (int)BWA_RQ_GRID_MAX) np = BWA_RQ_GRID_MAX;
        for (int p = 0; p < np; ++p) {
            cJSON* ent  = cJSON_GetArrayItem(grid, p);
            cJSON* posj = cJSON_IsObject(ent) ? cJSON_GetObjectItemCaseSensitive(ent, "position") : NULL;
            cJSON* spks = cJSON_IsObject(ent) ? cJSON_GetObjectItemCaseSensitive(ent, "speakers") : NULL;
            if (!cJSON_IsArray(posj) || cJSON_GetArraySize(posj) != 3 ||
                !cJSON_IsArray(spks) || cJSON_GetArraySize(spks) != n)
                FAIL("calib: existing room_eq_grid entry is malformed");
            for (int c = 0; c < 3; ++c) gpos[npos][c] = (float)cJSON_GetArrayItem(posj, c)->valuedouble;
            for (int s = 0; s < n; ++s) {
                cJSON* secs = cJSON_GetArrayItem(spks, s);
                int m = cJSON_IsArray(secs) ? cJSON_GetArraySize(secs) : 0;
                if (m > BWA_ROOM_EQ_MAX) m = BWA_ROOM_EQ_MAX;
                for (int t = 0; t < m; ++t) {
                    cJSON* o = cJSON_GetArrayItem(secs, t);
                    MeasureEqSection* dst = &ALL(npos, s)[t];
                    cJSON* fj = cJSON_GetObjectItemCaseSensitive(o, "fc");
                    cJSON* gj = cJSON_GetObjectItemCaseSensitive(o, "gain_db");
                    cJSON* qj = cJSON_GetObjectItemCaseSensitive(o, "q");
                    if (!cJSON_IsNumber(fj) || !cJSON_IsNumber(gj) || !cJSON_IsNumber(qj))
                        FAIL("calib: existing room_eq_grid section is malformed");
                    dst->fc = (float)fj->valuedouble; dst->gain_db = (float)gj->valuedouble; dst->q = (float)qj->valuedouble;
                }
                ACOUNT(npos, s) = m;
            }
            ++npos;
        }
    }

    /* replace the entry at (or append) this mic position */
    {
        int slot = -1;
        for (int p = 0; p < npos; ++p) {
            float dx = gpos[p][0]-mic[0], dy = gpos[p][1]-mic[1], dz = gpos[p][2]-mic[2];
            if (dx*dx + dy*dy + dz*dz < 0.05f * 0.05f) { slot = p; break; }
        }
        if (slot < 0) {
            if (npos >= (int)BWA_RQ_GRID_MAX) FAIL("calib: room_eq_grid is full (16 positions)");
            slot = npos++;
        }
        memcpy(gpos[slot], mic, sizeof(float) * 3);
        for (int s = 0; s < n; ++s) {
            int m = counts[s] > BWA_ROOM_EQ_MAX ? BWA_ROOM_EQ_MAX : counts[s];
            if (m > max_sections) m = max_sections;
            for (int t = 0; t < m; ++t) ALL(slot, s)[t] = cuts[(size_t)s * max_sections + t];
            ACOUNT(slot, s) = m;
        }
    }

    /* re-merge every speaker across all positions and rewrite the congruent grid */
    {
        narr = cJSON_CreateArray();
        if (!narr) FAIL("calib: room_eq_grid array alloc");
        cJSON* entries[BWA_RQ_GRID_MAX];
        for (int p = 0; p < npos; ++p) {
            cJSON* ent = cJSON_CreateObject();
            cJSON* posj = cJSON_CreateArray();
            cJSON* spks = cJSON_CreateArray();
            if (!ent || !posj || !spks) FAIL("calib: room_eq_grid entry alloc");
            for (int c = 0; c < 3; ++c)
                cJSON_AddItemToArray(posj, cJSON_CreateNumber(round((double)gpos[p][c] * 1000.0) / 1000.0));
            cJSON_AddItemToObject(ent, "position", posj);
            cJSON_AddItemToObject(ent, "speakers", spks);
            cJSON_AddItemToArray(narr, ent);
            entries[p] = spks;
        }
        MeasureEqSection percut[BWA_RQ_GRID_MAX * BWA_ROOM_EQ_MAX];
        int   percnt[BWA_RQ_GRID_MAX];
        float lfc[BWA_ROOM_EQ_MAX], lq[BWA_ROOM_EQ_MAX], lg[BWA_RQ_GRID_MAX * BWA_ROOM_EQ_MAX];
        for (int s = 0; s < n; ++s) {
            for (int p = 0; p < npos; ++p) {
                memcpy(&percut[(size_t)p * BWA_ROOM_EQ_MAX], ALL(p, s), sizeof(MeasureEqSection) * BWA_ROOM_EQ_MAX);
                percnt[p] = ACOUNT(p, s);
            }
            int lad = calib_room_grid_merge(percut, percnt, npos, BWA_ROOM_EQ_MAX,
                                            0.08, BWA_ROOM_EQ_MAX, lfc, lq, lg);
            for (int p = 0; p < npos; ++p) {
                cJSON* secs = cJSON_CreateArray();
                if (!secs) FAIL("calib: room_eq_grid sections alloc");
                for (int j = 0; j < lad; ++j) {              /* the FULL ladder at every position (congruent) */
                    cJSON* o = cJSON_CreateObject();
                    if (!o) FAIL("calib: room_eq_grid section alloc");
                    /* clamp into the loader's schema ranges so the writeback always round-trips */
                    double wfc = round((double)lfc[j] * 10.0) / 10.0;
                    double wg  = round((double)lg[(size_t)p * BWA_ROOM_EQ_MAX + j] * 100.0) / 100.0;
                    double wq  = round((double)lq[j] * 100.0) / 100.0;
                    if (wfc < 10.0)  wfc = 10.0;  else if (wfc > 1000.0) wfc = 1000.0;
                    if (wg  < -24.0) wg  = -24.0; else if (wg  > 0.0)    wg  = 0.0;
                    if (wq  < 0.25)  wq  = 0.25;  else if (wq  > 24.0)   wq  = 24.0;
                    cJSON_AddNumberToObject(o, "fc",      wfc);
                    cJSON_AddNumberToObject(o, "gain_db", wg);
                    cJSON_AddNumberToObject(o, "q",       wq);
                    cJSON_AddItemToArray(secs, o);
                }
                cJSON_AddItemToArray(entries[p], secs);
            }
            cJSON* sp = cJSON_GetArrayItem(speakers, s);     /* the schemes are mutually exclusive */
            cJSON_DeleteItemFromObjectCaseSensitive(sp, "room_eq");
        }
        cJSON_DeleteItemFromObjectCaseSensitive(root, "room_eq_grid");
        cJSON_AddItemToObject(root, "room_eq_grid", narr);
        narr = NULL;                                     /* root owns it now (fail must not double-free) */
    }

    outtext = cJSON_Print(root);
    if (!outtext) FAIL("calib: failed to serialize layout");
    FILE* f = fopen(out_path, "wb");
    if (!f) FAIL("calib: cannot open output for writing");
    fwrite(outtext, 1, strlen(outtext), f); fclose(f);
    ok = 1;
fail:
    cJSON_Delete(narr);                                  /* non-NULL only if a FAIL fired pre-attach */
    free(all); free(acount);
    free(outtext); cJSON_Delete(root); free(text);
    return ok;
    #undef ACOUNT
    #undef ALL
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

    /* same refusal as calib_write_layout: a NaN position (NaN mic file -> a "successful"
     * trilateration) serializes as `null` and destroys the layout in place */
    for (int i = 0; i < n; ++i)
        if (!isfinite(pos[i][0]) || !isfinite(pos[i][1]) || !isfinite(pos[i][2]) ||
            fabs(pos[i][0]) > 1000.f || fabs(pos[i][1]) > 1000.f || fabs(pos[i][2]) > 1000.f)
            FAIL("calib: refusing to write a non-finite/out-of-range position (degenerate solve?)");

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
