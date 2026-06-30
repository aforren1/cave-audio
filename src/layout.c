/*
 * layout.c — default speaker geometry + cave_layout.json loader (docs/layout-schema.md).
 * Control thread / load time only.
 */
#include "layout.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static void set_err(char* err, size_t cap, const char* msg) {
    if (err && cap) { strncpy(err, msg, cap - 1); err[cap - 1] = 0; }
}

static float db_to_lin(double db) { return (float)pow(10.0, db / 20.0); }

Layout layout_default(void) {
    Layout L;
    memset(&L, 0, sizeof L);
    const float ax[3] = { -1.5f, 0.0f, 1.5f };
    uint32_t k = 0;
    for (int yi = 0; yi < 3; ++yi)              /* 3x3x3 boundary grid minus the centre = 26 */
        for (int xi = 0; xi < 3; ++xi)
            for (int zi = 0; zi < 3; ++zi) {
                if (ax[xi] == 0.0f && ax[yi] == 0.0f && ax[zi] == 0.0f) continue;
                L.speakers[k].pos[0] = ax[xi];
                L.speakers[k].pos[1] = ax[yi];
                L.speakers[k].pos[2] = ax[zi];
                L.speakers[k].gain_lin = 1.0f;
                L.speakers[k].delay_samples = 0;
                ++k;
            }
    L.count             = k;                    /* 26 */
    L.rolloff_r         = 0.5f;
    L.atten_ref_m       = 1.0f;
    L.atten_rolloff     = 1.0f;
    L.atten_min_lin     = db_to_lin(-40.0);
    L.max_delay_samples = 0;
    return L;
}

static char* read_file(const char* path, char* err, size_t errcap) {
    FILE* f = fopen(path, "rb");
    if (!f) { set_err(err, errcap, "layout: cannot open file"); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); set_err(err, errcap, "layout: cannot size file"); return NULL; }
    char* buf = (char*)malloc((size_t)n + 1);
    if (!buf) { fclose(f); set_err(err, errcap, "layout: out of memory"); return NULL; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = 0;
    return buf;
}

bool layout_load(const char* path, uint32_t sample_rate, Layout* out, char* err, size_t errcap) {
    *out = layout_default();        /* keep sane defaults for any dbap fields the file omits */
    if (!path)        { set_err(err, errcap, "layout: null path"); return false; }
    if (sample_rate == 0) { set_err(err, errcap, "layout: zero sample rate"); return false; }

    char* text = read_file(path, err, errcap);
    if (!text) return false;
    cJSON* root = cJSON_Parse(text);
    free(text);
    if (!root) { set_err(err, errcap, "layout: JSON parse error"); return false; }

    bool ok = false;
    cJSON* speakers = cJSON_GetObjectItemCaseSensitive(root, "speakers");
    if (!cJSON_IsArray(speakers)) { set_err(err, errcap, "layout: missing 'speakers' array"); goto done; }
    if (cJSON_GetArraySize(speakers) != (int)BW_CHANNELS) {
        set_err(err, errcap, "layout: 'speakers' must have exactly 26 entries"); goto done;
    }

    cJSON* dbap = cJSON_GetObjectItemCaseSensitive(root, "dbap");
    if (cJSON_IsObject(dbap)) {
        cJSON* rr = cJSON_GetObjectItemCaseSensitive(dbap, "rolloff_r");
        if (cJSON_IsNumber(rr)) {
            if (!(rr->valuedouble > 0.0)) { set_err(err, errcap, "layout: dbap.rolloff_r must be > 0"); goto done; }
            out->rolloff_r = (float)rr->valuedouble;
        }
        cJSON* da = cJSON_GetObjectItemCaseSensitive(dbap, "distance_attenuation");
        if (cJSON_IsObject(da)) {
            cJSON* ref = cJSON_GetObjectItemCaseSensitive(da, "reference_distance_m");
            if (cJSON_IsNumber(ref)) {
                if (!(ref->valuedouble > 0.0)) { set_err(err, errcap, "layout: reference_distance_m must be > 0"); goto done; }
                out->atten_ref_m = (float)ref->valuedouble;
            }
            cJSON* ro = cJSON_GetObjectItemCaseSensitive(da, "rolloff");
            if (cJSON_IsNumber(ro)) {
                if (!(ro->valuedouble > 0.0)) { set_err(err, errcap, "layout: distance_attenuation.rolloff must be > 0"); goto done; }
                out->atten_rolloff = (float)ro->valuedouble;
            }
            cJSON* mg = cJSON_GetObjectItemCaseSensitive(da, "min_gain_db");
            if (cJSON_IsNumber(mg)) {
                if (mg->valuedouble > 0.0) { set_err(err, errcap, "layout: min_gain_db must be <= 0"); goto done; }
                out->atten_min_lin = db_to_lin(mg->valuedouble);
            }
        }
    }

    bool seen[BW_CHANNELS];
    memset(seen, 0, sizeof seen);
    uint32_t maxdelay = 0;
    for (int i = 0; i < (int)BW_CHANNELS; ++i) {
        cJSON* sp = cJSON_GetArrayItem(speakers, i);
        if (!cJSON_IsObject(sp)) { set_err(err, errcap, "layout: speaker entry is not an object"); goto done; }
        cJSON* idxj = cJSON_GetObjectItemCaseSensitive(sp, "index");
        cJSON* posj = cJSON_GetObjectItemCaseSensitive(sp, "position");
        if (!cJSON_IsNumber(idxj) || !cJSON_IsArray(posj) || cJSON_GetArraySize(posj) != 3) {
            set_err(err, errcap, "layout: bad speaker record (need numeric index + position[3])"); goto done;
        }
        int idx = idxj->valueint;
        if (idx < 0 || idx >= (int)BW_CHANNELS || seen[idx]) {
            set_err(err, errcap, "layout: speaker index out of range or duplicated"); goto done;
        }
        seen[idx] = true;
        Speaker* spk = &out->speakers[idx];
        for (int c = 0; c < 3; ++c) {
            cJSON* v = cJSON_GetArrayItem(posj, c);
            if (!cJSON_IsNumber(v)) { set_err(err, errcap, "layout: non-numeric position component"); goto done; }
            spk->pos[c] = (float)v->valuedouble;
        }
        cJSON* gj = cJSON_GetObjectItemCaseSensitive(sp, "gain_db");
        if (cJSON_IsNumber(gj)) {
            double db = gj->valuedouble;
            if (!(db >= -100.0 && db <= 24.0)) { set_err(err, errcap, "layout: gain_db out of range [-100, 24]"); goto done; }
            spk->gain_lin = db_to_lin(db);
        } else {
            spk->gain_lin = 1.0f;
        }
        cJSON* dj = cJSON_GetObjectItemCaseSensitive(sp, "delay_ms");
        double dms = 0.0;
        if (cJSON_IsNumber(dj)) {
            dms = dj->valuedouble;
            if (dms < 0.0) dms = 0.0;
            if (dms > 1000.0) { set_err(err, errcap, "layout: delay_ms too large (> 1000 ms)"); goto done; }
        }
        uint32_t dsamp = (uint32_t)(dms * 1e-3 * (double)sample_rate + 0.5);
        spk->delay_samples = dsamp;
        if (dsamp > maxdelay) maxdelay = dsamp;

        spk->eq_len = 0;                            /* optional per-speaker correction FIR (calibrate writes it) */
        cJSON* eqj = cJSON_GetObjectItemCaseSensitive(sp, "eq");
        if (cJSON_IsArray(eqj)) {
            int m = cJSON_GetArraySize(eqj);
            if (m > BW_EQ_TAPS) m = BW_EQ_TAPS;
            for (int t = 0; t < m; ++t) {
                cJSON* v = cJSON_GetArrayItem(eqj, t);
                spk->eq[t] = cJSON_IsNumber(v) ? (float)v->valuedouble : 0.f;
            }
            spk->eq_len = (uint16_t)m;
        }
    }
    for (uint32_t i = 0; i < BW_CHANNELS; ++i)
        if (!seen[i]) { set_err(err, errcap, "layout: missing a speaker index in 0..25"); goto done; }

    out->count             = BW_CHANNELS;
    out->max_delay_samples = maxdelay;
    ok = true;
done:
    cJSON_Delete(root);
    return ok;
}
