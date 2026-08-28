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

/* ref = the array centroid: the nominal listening point the world-locked decodes reference.
 * Computed from the positions, so it holds for any origin convention (floor or center). */
void layout_compute_ref(Layout* L) {
    double s[3] = { 0, 0, 0 };
    for (uint32_t k = 0; k < L->count; ++k)
        for (int i = 0; i < 3; ++i) s[i] += L->speakers[k].pos[i];
    for (int i = 0; i < 3; ++i) L->ref[i] = L->count ? (float)(s[i] / L->count) : 0.f;
}

/* The array's own angular scale (see layout.h): mean nearest-neighbor angle between speaker
 * directions seen from ref. Two features derive from it — SPCAP's lobe width (below) and the
 * hole-aware spread floor's knee (hole.c) — so it lives here once. */
float layout_mean_speaker_spacing(const Layout* L) {
    const uint32_t N = L ? L->count : 0;
    if (N < 2) return 0.f;                         /* nothing to measure a separation against */
    double sum = 0.0;
    uint32_t used = 0;
    for (uint32_t k = 0; k < N; ++k) {
        float dk[3];
        unit_dir(L->ref, L->speakers[k].pos, dk);
        double best = -2.0;                        /* largest cos = smallest angle = nearest neighbor */
        for (uint32_t j = 0; j < N; ++j) {
            if (j == k) continue;
            float dj[3];
            unit_dir(L->ref, L->speakers[j].pos, dj);
            double c = (double)dk[0]*dj[0] + (double)dk[1]*dj[1] + (double)dk[2]*dj[2];
            if (c > best) best = c;
        }
        if (best <= -2.0) continue;
        if (best >  1.0) best =  1.0;
        if (best < -1.0) best = -1.0;
        sum += acos(best); ++used;
    }
    return used ? (float)(sum / used) : 0.f;
}

/* SPCAP focus from the geometry (see layout.h): the mean nearest-neighbor speaker angle above, then
 * the -6 dB-at-that-angle lobe exponent. Sanity: 30 deg -> ~20, 45 deg -> ~8.8, 60 deg -> ~4.8; the
 * default 26-speaker cube grid -> ~12.7. */
float layout_derive_spcap_focus(const Layout* L) {
    const double delta = layout_mean_speaker_spacing(L);
    /* co-located speakers (delta ~ 0) blow the log up; an antipodal-only array (delta ~ pi) sends the
     * denominator to -inf. Both are degenerate surveys (as is a < 2 speaker one, which reads 0
     * here) — keep the historical constant. */
    if (delta < 1e-3 || delta > 3.1) return 12.0f;
    const double denom = log(0.5 * (1.0 + cos(delta)));
    if (!(denom < -1e-9)) return 12.0f;
    double n = log(0.25) / denom;
    if (n < 1.0) n = 1.0; else if (n > 64.0) n = 64.0;
    return (float)n;
}

Layout layout_default(void) {
    Layout L;
    memset(&L, 0, sizeof L);
    const float ax[3] = { -1.5f, 0.0f, 1.5f };  /* x/z: centered on the room */
    const float ay[3] = {  0.0f, 1.5f, 3.0f };  /* y: FLOOR origin, Motive-style */
    uint32_t k = 0;
    for (int yi = 0; yi < 3; ++yi)              /* 3x3x3 boundary grid minus the center = 26 */
        for (int xi = 0; xi < 3; ++xi)
            for (int zi = 0; zi < 3; ++zi) {
                if (ax[xi] == 0.0f && ay[yi] == 1.5f && ax[zi] == 0.0f) continue;
                L.speakers[k].pos[0] = ax[xi];
                L.speakers[k].pos[1] = ay[yi];
                L.speakers[k].pos[2] = ax[zi];
                L.speakers[k].gain_lin = 1.0f;
                L.speakers[k].delay_samples = 0;
                ++k;
            }
    L.count             = k;                    /* 26 */
    layout_compute_ref(&L);                            /* (0, 1.5, 0) — the cube's center */
    L.rolloff_r         = 0.5f;
    L.spcap_focus       = layout_derive_spcap_focus(&L);   /* ~12.7 on this grid (37.5 deg spacing) */
    L.spcap_density     = BWA_SPCAP_DENSITY_DEFAULT;
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
    /* the layout's speaker count IS the engine's channel count: 4..BWA_CHANNELS (the compile-time
     * CAPACITY — collaborator arrays with fewer speakers load into the same binary) */
    const int nspk = cJSON_GetArraySize(speakers);
    if (nspk < 4 || nspk > (int)BWA_CHANNELS) {
        set_err(err, errcap, "layout: 'speakers' must have 4..26 entries (26 = BWA_CHANNELS cap)"); goto done;
    }

    bool have_rolloff = false;
    cJSON* dbap = cJSON_GetObjectItemCaseSensitive(root, "dbap");
    if (cJSON_IsObject(dbap)) {
        cJSON* rr = cJSON_GetObjectItemCaseSensitive(dbap, "rolloff_r");
        if (cJSON_IsNumber(rr)) {
            if (!(rr->valuedouble > 0.0)) { set_err(err, errcap, "layout: dbap.rolloff_r must be > 0"); goto done; }
            out->rolloff_r = (float)rr->valuedouble;
            /* 1 mm floor: below any audible blur, but r^2 can no longer underflow to 0 in the DBAP
             * solve (a tiny r with a source ON a speaker made 1/d^2 overflow into a sticky NaN gain). */
            if (out->rolloff_r < 0.001f) out->rolloff_r = 0.001f;
            have_rolloff = true;
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

    bool seen[BWA_CHANNELS];
    memset(seen, 0, sizeof seen);
    uint32_t maxdelay = 0;
    for (int i = 0; i < nspk; ++i) {
        cJSON* sp = cJSON_GetArrayItem(speakers, i);
        if (!cJSON_IsObject(sp)) { set_err(err, errcap, "layout: speaker entry is not an object"); goto done; }
        cJSON* idxj = cJSON_GetObjectItemCaseSensitive(sp, "index");
        cJSON* posj = cJSON_GetObjectItemCaseSensitive(sp, "position");
        if (!cJSON_IsNumber(idxj) || !cJSON_IsArray(posj) || cJSON_GetArraySize(posj) != 3) {
            set_err(err, errcap, "layout: bad speaker record (need numeric index + position[3])"); goto done;
        }
        int idx = idxj->valueint;
        if (idx < 0 || idx >= nspk || seen[idx]) {   /* indices must be a complete permutation of 0..N-1 */
            set_err(err, errcap, "layout: speaker index out of range or duplicated"); goto done;
        }
        seen[idx] = true;
        Speaker* spk = &out->speakers[idx];
        for (int c = 0; c < 3; ++c) {
            cJSON* v = cJSON_GetArrayItem(posj, c);
            if (!cJSON_IsNumber(v)) { set_err(err, errcap, "layout: non-numeric position component"); goto done; }
            if (!isfinite(v->valuedouble) || fabs(v->valuedouble) > 1000.0) {   /* NaN/inf/absurd -> NaN gains on the bus */
                set_err(err, errcap, "layout: position component non-finite or out of range (+/-1000 m)"); goto done;
            }
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
            if (m > BWA_EQ_TAPS) {   /* reject rather than silently truncate the kernel mid-tap (a discontinuity) */
                set_err(err, errcap, "layout: eq FIR longer than BWA_EQ_TAPS (512)"); goto done;
            }
            for (int t = 0; t < m; ++t) {
                cJSON* v = cJSON_GetArrayItem(eqj, t);
                /* the magnitude bound matters as much as isfinite: the check is on the DOUBLE but the
                 * store is a FLOAT, so 1e300 passes isfinite and the cast overflows to Inf — and a
                 * finite 1e30 tap overflows the bus the first time the FIR runs (a correction kernel's
                 * taps are O(1); 100 is generous) */
                if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || fabs(v->valuedouble) > 100.0) {
                    set_err(err, errcap, "layout: eq tap non-numeric, non-finite, or out of range (+/-100)"); goto done;
                }
                spk->eq[t] = (float)v->valuedouble;
            }
            spk->eq_len = (uint16_t)m;
        }

        spk->room_eq_count = 0;                     /* optional LF modal cuts (calibrate --room-eq writes them) */
        cJSON* rqj = cJSON_GetObjectItemCaseSensitive(sp, "room_eq");
        if (cJSON_IsArray(rqj)) {
            int m = cJSON_GetArraySize(rqj);
            if (m > BWA_ROOM_EQ_MAX) { set_err(err, errcap, "layout: room_eq has more than BWA_ROOM_EQ_MAX (8) sections"); goto done; }
            for (int t = 0; t < m; ++t) {
                cJSON* o  = cJSON_GetArrayItem(rqj, t);
                cJSON* fj = cJSON_IsObject(o) ? cJSON_GetObjectItemCaseSensitive(o, "fc")      : NULL;
                cJSON* gj2= cJSON_IsObject(o) ? cJSON_GetObjectItemCaseSensitive(o, "gain_db") : NULL;
                cJSON* qj = cJSON_IsObject(o) ? cJSON_GetObjectItemCaseSensitive(o, "q")       : NULL;
                if (!cJSON_IsNumber(fj) || !cJSON_IsNumber(gj2) || !cJSON_IsNumber(qj)) {
                    set_err(err, errcap, "layout: bad room_eq section (need numeric fc/gain_db/q)"); goto done;
                }
                double fc = fj->valuedouble, g = gj2->valuedouble, q = qj->valuedouble;
                if (!(fc >= 10.0 && fc <= 1000.0)) { set_err(err, errcap, "layout: room_eq fc out of range [10, 1000]"); goto done; }
                if (!(g >= -24.0 && g <= 0.0))     { set_err(err, errcap, "layout: room_eq gain_db out of range [-24, 0] (cuts only)"); goto done; }
                if (!(q >= 0.25 && q <= 24.0))     { set_err(err, errcap, "layout: room_eq q out of range [0.25, 24]"); goto done; }
                spk->room_eq[spk->room_eq_count].fc      = (float)fc;
                spk->room_eq[spk->room_eq_count].gain_db = (float)g;
                spk->room_eq[spk->room_eq_count].q       = (float)q;
                ++spk->room_eq_count;
            }
        }
    }
    for (int i = 0; i < nspk; ++i)
        if (!seen[i]) { set_err(err, errcap, "layout: missing a speaker index in 0..count-1"); goto done; }

    /* optional tracked-room-EQ grid (bwa_calibrate --room-eq-grid writes it; see layout.h). Every
     * position must carry the SAME per-speaker fc/q ladder — only the depths vary — because the
     * runtime interpolates depths by ladder index; a mismatched ladder would blend unrelated modes. */
    cJSON* grid = cJSON_GetObjectItemCaseSensitive(root, "room_eq_grid");
    if (cJSON_IsArray(grid)) {
        int np = cJSON_GetArraySize(grid);
        if (np < 1 || np > (int)BWA_RQ_GRID_MAX) {
            set_err(err, errcap, "layout: room_eq_grid must have 1..16 positions"); goto done;
        }
        for (int p = 0; p < np; ++p) {
            cJSON* ent  = cJSON_GetArrayItem(grid, p);
            cJSON* posj = cJSON_IsObject(ent) ? cJSON_GetObjectItemCaseSensitive(ent, "position") : NULL;
            cJSON* spks = cJSON_IsObject(ent) ? cJSON_GetObjectItemCaseSensitive(ent, "speakers") : NULL;
            if (!cJSON_IsArray(posj) || cJSON_GetArraySize(posj) != 3 ||
                !cJSON_IsArray(spks) || cJSON_GetArraySize(spks) != nspk) {
                set_err(err, errcap, "layout: room_eq_grid entry needs position[3] + one speakers entry per speaker"); goto done;
            }
            for (int c = 0; c < 3; ++c) {
                cJSON* v = cJSON_GetArrayItem(posj, c);
                if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || fabs(v->valuedouble) > 1000.0) {
                    set_err(err, errcap, "layout: room_eq_grid position component non-finite or out of range"); goto done;
                }
                out->rq_grid.pos[p][c] = (float)v->valuedouble;
            }
            for (int s = 0; s < nspk; ++s) {
                cJSON* secs = cJSON_GetArrayItem(spks, s);
                if (!cJSON_IsArray(secs)) { set_err(err, errcap, "layout: room_eq_grid speaker entry is not an array"); goto done; }
                int m = cJSON_GetArraySize(secs);
                if (m > BWA_ROOM_EQ_MAX) { set_err(err, errcap, "layout: room_eq_grid has more than 8 sections for a speaker"); goto done; }
                if (p == 0) out->rq_grid.nsec[s] = (uint8_t)m;
                else if (m != (int)out->rq_grid.nsec[s]) {
                    set_err(err, errcap, "layout: room_eq_grid positions disagree on a speaker's section count"); goto done;
                }
                for (int t = 0; t < m; ++t) {
                    cJSON* o  = cJSON_GetArrayItem(secs, t);
                    cJSON* fj = cJSON_IsObject(o) ? cJSON_GetObjectItemCaseSensitive(o, "fc")      : NULL;
                    cJSON* gj2= cJSON_IsObject(o) ? cJSON_GetObjectItemCaseSensitive(o, "gain_db") : NULL;
                    cJSON* qj = cJSON_IsObject(o) ? cJSON_GetObjectItemCaseSensitive(o, "q")       : NULL;
                    if (!cJSON_IsNumber(fj) || !cJSON_IsNumber(gj2) || !cJSON_IsNumber(qj)) {
                        set_err(err, errcap, "layout: bad room_eq_grid section (need numeric fc/gain_db/q)"); goto done;
                    }
                    double fc = fj->valuedouble, g = gj2->valuedouble, q = qj->valuedouble;
                    if (!(fc >= 10.0 && fc <= 1000.0)) { set_err(err, errcap, "layout: room_eq_grid fc out of range [10, 1000]"); goto done; }
                    if (!(g >= -24.0 && g <= 0.0))     { set_err(err, errcap, "layout: room_eq_grid gain_db out of range [-24, 0] (cuts only)"); goto done; }
                    if (!(q >= 0.25 && q <= 24.0))     { set_err(err, errcap, "layout: room_eq_grid q out of range [0.25, 24]"); goto done; }
                    if (p == 0) {
                        out->rq_grid.fc[s][t] = (float)fc;
                        out->rq_grid.q [s][t] = (float)q;
                    } else if (fabs(fc - out->rq_grid.fc[s][t]) > 0.005 * out->rq_grid.fc[s][t] ||
                               fabs(q  - out->rq_grid.q [s][t]) > 0.005 * out->rq_grid.q [s][t]) {
                        set_err(err, errcap, "layout: room_eq_grid positions disagree on the fc/q ladder"); goto done;
                    }
                    out->rq_grid.gain_db[p][s][t] = (float)g;
                }
            }
        }
        out->rq_grid.npos = (uint8_t)np;
        for (int s = 0; s < nspk; ++s)               /* one room-correction scheme at a time */
            if (out->speakers[s].room_eq_count) {
                set_err(err, errcap, "layout: carries both room_eq (static) and room_eq_grid (tracked) - pick one"); goto done;
            }
    }

    /* a loaded layout with fewer than BWA_CHANNELS speakers leaves the tail entries at the default
     * grid's values — harmless: count gates every consumer, and the engine's channel count follows it */
    out->count             = (uint32_t)nspk;
    out->max_delay_samples = maxdelay;
    layout_compute_ref(out);                  /* nominal listening point = the surveyed array's centroid */
    if (!have_rolloff) {
        /* file omits the blur: derive it from the geometry, r = 0.25 x the mean centroid->speaker
         * distance (Sundstrom 2021 recommends 0.2-0.5 of it; docs/spatialization.md). An explicit
         * value in the file always wins; layout_default's constant only covers the no-file grid. */
        double s = 0.0;
        for (int i = 0; i < nspk; ++i) {
            double dx = out->speakers[i].pos[0] - out->ref[0];
            double dy = out->speakers[i].pos[1] - out->ref[1];
            double dz = out->speakers[i].pos[2] - out->ref[2];
            s += sqrt(dx * dx + dy * dy + dz * dz);
        }
        out->rolloff_r = (float)(0.25 * s / nspk);
        if (out->rolloff_r < 0.05f) out->rolloff_r = 0.05f;   /* keep the >0 invariant on a degenerate survey */
    }
    /* SPCAP's lobe width follows the survey the same way: the file never carries it (it is a
     * perceptual knob, not a measured quantity), so it is always derived here. */
    out->spcap_focus   = layout_derive_spcap_focus(out);
    out->spcap_density = BWA_SPCAP_DENSITY_DEFAULT;
    ok = true;
done:
    cJSON_Delete(root);
    return ok;
}
