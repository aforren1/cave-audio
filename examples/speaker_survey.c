/*
 * speaker_survey.c - bwa_speaker_survey: check the speakers the cameras can see, optically.
 *
 * Most of the array hides behind the screens; a few speakers are in view of the OptiTrack cameras.
 * Give each of those a rigid body in Motive named spk<N> or speaker<N> (N = its layout index), with
 * 3 or more markers stuck FLAT on the front baffle around the tweeter, and this tool reads them off
 * the NatNet stream and reports, per speaker:
 *   - the optical position against the layout's, in mm;
 *   - the optical aim (the baffle plane's normal) against the layout's aim, in degrees;
 *   - the marker count and how flat they are, and how steady the body was;
 * then fits one rigid transform from the optical positions onto the layout's. That fit answers
 * whether Motive's frame (ground plane, axes, handedness) IS the room frame the layout is written
 * in. Layout positions are ACOUSTIC CENTERS, behind the baffle the markers sit on, so --baffle-offset-m
 * moves each optical point back along its aim. On a layout whose positions --localize MEASURED (each
 * differs from its plan_position), the tool then reads that depth itself: the baffle depth section
 * refits the frame with the depth as a fourth unknown (survey.h) and prints the value to pass. It
 * reports only by default; --write copies the aim and/or position of the MATCHED speakers into a copy
 * of the layout. --simulate runs the whole pipeline on synthesized packets, through the same parsers,
 * with no Motive: the rig-day rehearsal and the end-to-end ctest.
 *
 *   bwa_speaker_survey <layout.json> --server <motive-ip> [--local <ip>] [--multicast <ip>]
 *                      [--natnet M.m] [--data-port N] [--command-port N]
 *                      [--seconds 3] [--map name=index ...] [--baffle-offset-m x]
 *                      [--axis x,y,z] [--max-plane-mm 3] [--max-spread-mm 2] [--max-spread-deg 0.5]
 *                      [--csv out.csv] [--write out.json [--fields aim|position|aim,position]]
 *   bwa_speaker_survey <layout.json> --simulate [--sim-speakers i,j,k,...] [--sim-aim-error deg]
 *                      [--sim-yaw deg] [--sim-offset x,y,z] [--sim-mirror] [--sim-noise-mm x]
 *                      [--sim-jitter-mm x] [--sim-natnet M.m] [--sim-depth-m x] [the report/write options above]
 *
 * A tool, not ABI client code: it links the engine's internals (the layout loader, the NatNet
 * parsers, survey.c) the way bwa_calibrate does, so it reads the layout exactly as the engine does.
 * Math: src/tracking/survey.h. Wire format: src/tracking/natnet.h.
 */
#include "core/layout.h"
#include "tracking/natnet.h"
#include "tracking/survey.h"
#include "os/os.h"                  /* os_fopen (UTF-8 paths), os_monotonic_ns */

#include <cJSON.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BODIES   256
#define MAX_MAP      64
#define PI           3.14159265358979323846
#define FRAME_MAX_DEG 0.5f          /* Motive's frame agrees with the room frame within this rotation */
#define FRAME_MAX_M   0.01f         /* and this translation */

/* ---- arguments ---------------------------------------------------------------------------------- */

typedef struct {
    const char*    layout_path;
    NatNetConfig   nc;
    double         seconds;
    SurveyMapEntry map[MAX_MAP];
    int            nmap;
    SurveyOpts     opts;
    float          max_spread_m, max_spread_deg;
    const char*    csv_path;
    const char*    write_path;
    bool           write_aim, write_pos;
    bool           require_frame;         /* --require-frame: exit 3 unless the frame agrees */
    /* simulate */
    bool           simulate;
    int            sim_idx[64];
    int            sim_n;
    float          sim_aim_error_deg, sim_yaw_deg, sim_offset[3];
    bool           sim_mirror;
    float          sim_noise_mm, sim_jitter_mm;
    int            sim_major, sim_minor;
    float          sim_depth_m;           /* --sim-depth-m: the TRUE baffle-to-acoustic-center depth */
    bool           sim_depth_set;         /* unset: the sim builds the baffle at --baffle-offset-m */
} Args;

static void usage(void) {
    printf("usage: bwa_speaker_survey <layout.json> --server <motive-ip> [options]\n"
           "       bwa_speaker_survey <layout.json> --simulate [sim options] [options]\n"
           "connection: --server ip  --local ip  --multicast ip (default 239.255.42.99)\n"
           "            --natnet M.m (override; the server handshake wins)  --data-port N  --command-port N\n"
           "survey:     --seconds s (3)  --map name=index (repeatable)  --baffle-offset-m x (0: the\n"
           "            layout point sits x m behind the baffle)  --axis x,y,z (aim = this body-local\n"
           "            axis instead of the baffle plane)  --max-plane-mm (3)  --max-spread-mm (2)\n"
           "            --max-spread-deg (0.5)\n"
           "output:     --csv out.csv  --write out.json  --fields aim|position|aim,position (aim)\n"
           "verdict:    --require-frame (exit 3 unless Motive's frame agrees with the room frame: a right-\n"
           "            handed fit of 3 or more speakers within 0.5 deg and 10 mm)\n"
           "simulate:   --sim-speakers i,j,...  --sim-aim-error deg (on the first)  --sim-yaw deg\n"
           "            --sim-offset x,y,z  --sim-mirror  --sim-noise-mm x (marker placement)\n"
           "            --sim-jitter-mm x (per frame, 0.05)  --sim-natnet M.m (4.1)\n"
           "            --sim-depth-m x (the true baffle-to-acoustic-center depth; default --baffle-offset-m)\n");
}

static bool parse_vec3(const char* s, float v[3]) {
    return s && sscanf(s, "%f,%f,%f", &v[0], &v[1], &v[2]) == 3 &&
           isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
}
static bool parse_ver(const char* s, int* M, int* m) {
    return s && sscanf(s, "%d.%d", M, m) == 2 && *M >= 0 && *m >= 0;
}

static bool parse_args(int argc, char** argv, Args* a) {
    memset(a, 0, sizeof *a);
    a->nc.multicast = "239.255.42.99";
    a->seconds = 3.0;
    a->max_spread_m = SURVEY_MAX_SPREAD_M;
    a->max_spread_deg = SURVEY_MAX_SPREAD_DEG;
    a->write_aim = true;
    a->sim_jitter_mm = 0.05f;
    a->sim_major = 4; a->sim_minor = 1;
    for (int i = 1; i < argc; ++i) {
        const char* s = argv[i];
        const char* v = (i + 1 < argc) ? argv[i + 1] : NULL;
#define NEED() do { if (!v) { printf("error: %s needs a value\n", s); return false; } ++i; } while (0)
        if      (!strcmp(s, "--server"))         { NEED(); a->nc.server = v; }
        else if (!strcmp(s, "--local"))          { NEED(); a->nc.local_iface = v; }
        else if (!strcmp(s, "--multicast"))      { NEED(); a->nc.multicast = v; }
        else if (!strcmp(s, "--natnet"))         { NEED(); if (!parse_ver(v, &a->nc.major, &a->nc.minor)) { printf("error: --natnet wants M.m\n"); return false; } }
        else if (!strcmp(s, "--data-port"))      { NEED(); a->nc.data_port = (uint16_t)atoi(v); }
        else if (!strcmp(s, "--command-port"))   { NEED(); a->nc.command_port = (uint16_t)atoi(v); }
        else if (!strcmp(s, "--seconds"))        { NEED(); a->seconds = atof(v); }
        else if (!strcmp(s, "--baffle-offset-m")){ NEED(); a->opts.baffle_offset_m = (float)atof(v); }
        else if (!strcmp(s, "--max-plane-mm"))   { NEED(); a->opts.max_plane_rms_m = (float)atof(v) * 0.001f; }
        else if (!strcmp(s, "--max-spread-mm"))  { NEED(); a->max_spread_m = (float)atof(v) * 0.001f; }
        else if (!strcmp(s, "--max-spread-deg")) { NEED(); a->max_spread_deg = (float)atof(v); }
        else if (!strcmp(s, "--axis"))           { NEED(); if (!parse_vec3(v, a->opts.axis_local)) { printf("error: --axis wants x,y,z\n"); return false; } a->opts.use_axis = true; }
        else if (!strcmp(s, "--csv"))            { NEED(); a->csv_path = v; }
        else if (!strcmp(s, "--write"))          { NEED(); a->write_path = v; }
        else if (!strcmp(s, "--fields")) {
            NEED();
            a->write_aim = strstr(v, "aim") != NULL;
            a->write_pos = strstr(v, "position") != NULL;
            if (!a->write_aim && !a->write_pos) { printf("error: --fields wants aim, position or aim,position\n"); return false; }
        }
        else if (!strcmp(s, "--map")) {
            NEED();
            const char* eq = strrchr(v, '=');
            if (!eq || eq == v || !eq[1] || a->nmap >= MAX_MAP) { printf("error: --map wants name=index\n"); return false; }
            char* end;
            long idx = strtol(eq + 1, &end, 10);
            if (*end) { printf("error: --map index '%s' is not a number\n", eq + 1); return false; }
            size_t len = (size_t)(eq - v);
            char* name = (char*)malloc(len + 1);            /* lives for the run */
            if (!name) return false;
            memcpy(name, v, len); name[len] = 0;
            a->map[a->nmap].name = name;
            a->map[a->nmap].index = (int)idx;
            ++a->nmap;
        }
        else if (!strcmp(s, "--require-frame"))  a->require_frame = true;
        else if (!strcmp(s, "--simulate"))       a->simulate = true;
        else if (!strcmp(s, "--sim-mirror"))     a->sim_mirror = true;
        else if (!strcmp(s, "--sim-aim-error"))  { NEED(); a->sim_aim_error_deg = (float)atof(v); }
        else if (!strcmp(s, "--sim-yaw"))        { NEED(); a->sim_yaw_deg = (float)atof(v); }
        else if (!strcmp(s, "--sim-offset"))     { NEED(); if (!parse_vec3(v, a->sim_offset)) { printf("error: --sim-offset wants x,y,z\n"); return false; } }
        else if (!strcmp(s, "--sim-noise-mm"))   { NEED(); a->sim_noise_mm = (float)atof(v); }
        else if (!strcmp(s, "--sim-jitter-mm"))  { NEED(); a->sim_jitter_mm = (float)atof(v); }
        else if (!strcmp(s, "--sim-depth-m"))    { NEED(); a->sim_depth_m = (float)atof(v); a->sim_depth_set = true; }
        else if (!strcmp(s, "--sim-natnet"))     { NEED(); if (!parse_ver(v, &a->sim_major, &a->sim_minor)) { printf("error: --sim-natnet wants M.m\n"); return false; } }
        else if (!strcmp(s, "--sim-speakers")) {
            NEED();
            a->sim_n = 0;
            for (const char* p = v; *p && a->sim_n < 64;) {
                char* end;
                long k = strtol(p, &end, 10);
                if (end == p) { printf("error: --sim-speakers wants i,j,k\n"); return false; }
                a->sim_idx[a->sim_n++] = (int)k;
                p = (*end == ',') ? end + 1 : end;
            }
        }
        else if (!strcmp(s, "--help") || !strcmp(s, "-h")) { usage(); exit(0); }
        else if (s[0] == '-') { printf("error: unknown option %s\n", s); return false; }
        else if (!a->layout_path) a->layout_path = s;
        else { printf("error: unexpected argument %s\n", s); return false; }
#undef NEED
    }
    if (!a->layout_path) { printf("error: no layout given\n"); return false; }
    if (!a->simulate && !(a->nc.server && a->nc.server[0])) {
        printf("error: a live survey needs --server: the rigid-body names and marker offsets come from\n"
               "       the server's model definitions, which a multicast-only listen never sees\n");
        return false;
    }
    if (!(a->seconds > 0.0) || a->seconds > 600.0) { printf("error: --seconds out of range\n"); return false; }
    return true;
}

/* ---- the layout, and which aims it states explicitly ------------------------------------------- */

static Layout g_layout;                    /* ~176 KB: never a stack local (CLAUDE.md) */
static bool   g_aim_explicit[BWA_MAX_CHANNELS];

static char* read_text(const char* path) {
    FILE* f = os_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len < 0) { fclose(f); return NULL; }
    char* buf = (char*)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[rd] = 0;
    return buf;
}

/* The loader fills a missing aim with "toward the listening point" and forgets that it did; the
 * report must say which aims the layout actually states, so read the flag off the JSON. */
static void read_aim_flags(const char* path) {
    memset(g_aim_explicit, 0, sizeof g_aim_explicit);
    char* text = read_text(path);
    cJSON* root = text ? cJSON_Parse(text) : NULL;
    cJSON* spk = root ? cJSON_GetObjectItemCaseSensitive(root, "speakers") : NULL;
    cJSON* sp;
    cJSON_ArrayForEach(sp, spk) {
        cJSON* ix = cJSON_GetObjectItemCaseSensitive(sp, "index");
        if (cJSON_IsNumber(ix) && ix->valueint >= 0 && ix->valueint < BWA_MAX_CHANNELS)
            g_aim_explicit[ix->valueint] = cJSON_GetObjectItemCaseSensitive(sp, "aim") != NULL;
    }
    cJSON_Delete(root);
    free(text);
}

/* --write: re-parse the input JSON so every field this tool does not own survives, and set only
 * `aim` / `position` on the matched speakers' records (found by their `index`, not array order). A
 * measuring writer: a record it overwrites first keeps its old position and aim as the plan
 * (layout_json_keep_plan), once; *nplan_new / *nplan_kept count the records that got one now and the
 * ones that already had one. */
static bool write_layout(const char* in, const char* out, const bool* do_aim, const float (*aim)[3],
                         const bool* do_pos, const float (*pos)[3], int count, int* nplan_new, int* nplan_kept,
                         char* err, size_t cap) {
    bool ok = false;
    *nplan_new = *nplan_kept = 0;
    char* text = read_text(in);
    cJSON* root = text ? cJSON_Parse(text) : NULL;
    char* outtext = NULL;
    if (!root) { snprintf(err, cap, "cannot read %s as JSON", in); goto done; }
    cJSON* spk = cJSON_GetObjectItemCaseSensitive(root, "speakers");
    if (!cJSON_IsArray(spk)) { snprintf(err, cap, "no speakers array"); goto done; }
    for (int i = 0; i < count; ++i) {
        const float* v[2] = { do_aim[i] ? aim[i] : NULL, do_pos[i] ? pos[i] : NULL };
        for (int f = 0; f < 2; ++f)
            if (v[f] && (!isfinite(v[f][0]) || !isfinite(v[f][1]) || !isfinite(v[f][2]) ||
                         fabsf(v[f][0]) > 1000.f || fabsf(v[f][1]) > 1000.f || fabsf(v[f][2]) > 1000.f)) {
                snprintf(err, cap, "refusing to write a non-finite or out-of-range value (speaker %d)", i);
                goto done;
            }
    }
    cJSON* sp;
    cJSON_ArrayForEach(sp, spk) {
        cJSON* ix = cJSON_GetObjectItemCaseSensitive(sp, "index");
        if (!cJSON_IsNumber(ix) || ix->valueint < 0 || ix->valueint >= count) continue;
        const int i = ix->valueint;
        const char* key[2] = { "aim", "position" };
        const float* v[2]  = { do_aim[i] ? aim[i] : NULL, do_pos[i] ? pos[i] : NULL };
        if (v[0] || v[1]) {
            const int kp = layout_json_keep_plan(sp);
            *nplan_new += kp == 1;
            *nplan_kept += kp == 0;
        }
        for (int f = 0; f < 2; ++f) {
            if (!v[f]) continue;
            cJSON* arr = cJSON_CreateArray();
            for (int k = 0; k < 3; ++k)                        /* 0.1 mm / 1e-4 of a unit vector */
                cJSON_AddItemToArray(arr, cJSON_CreateNumber(round((double)v[f][k] * 10000.0) / 10000.0));
            if (cJSON_GetObjectItemCaseSensitive(sp, key[f])) cJSON_ReplaceItemInObjectCaseSensitive(sp, key[f], arr);
            else                                              cJSON_AddItemToObject(sp, key[f], arr);
        }
    }
    outtext = cJSON_Print(root);
    if (!outtext) { snprintf(err, cap, "failed to serialize"); goto done; }
    FILE* f = os_fopen(out, "wb");
    if (!f) { snprintf(err, cap, "cannot open %s for writing", out); goto done; }
    size_t n = strlen(outtext);
    ok = fwrite(outtext, 1, n, f) == n;
    fwrite("\n", 1, 1, f);
    fclose(f);
    if (!ok) snprintf(err, cap, "short write to %s", out);
done:
    free(outtext);
    cJSON_Delete(root);
    free(text);
    return ok;
}

/* ---- simulate: synthesize what Motive would send ----------------------------------------------- */

typedef struct { uint8_t* b; size_t n, cap; } Wr;
static void wr(Wr* w, const void* p, size_t n) { if (w->n + n <= w->cap) memcpy(w->b + w->n, p, n); w->n += n; }
static void wi32(Wr* w, int32_t v) { wr(w, &v, 4); }
static void wi16(Wr* w, int16_t v) { wr(w, &v, 2); }
static void wf32(Wr* w, float v)   { wr(w, &v, 4); }
static void wstr(Wr* w, const char* s) { wr(w, s, strlen(s) + 1); }
static bool sized_sections(int M, int m) { return (M == 4 && m > 0) || M > 4; }

static uint32_t g_rng = 0x5eed1234u;
static double urand(void) { g_rng = g_rng * 1664525u + 1013904223u; return ((g_rng >> 8) + 0.5) / 16777216.0; }
static double grand(void) { return sqrt(-2.0 * log(urand())) * cos(2.0 * PI * urand()); }

typedef struct {
    char    name[32];
    int32_t id;
    int     layout_idx;            /* -1: a decoy body */
    int     n_markers;
    float   local[5][3];           /* marker offsets in the body frame (what the MODELDEF carries) */
    float   pivot[3], q[4];        /* true pose, Motive frame */
    float   true_aim_room[3];      /* the aim the sim built the baffle around (room frame) */
    float   world[5][3];           /* markers, Motive frame (the frame's markerset) */
} SimBody;

typedef struct {
    int     major, minor;
    int     n;
    SimBody b[MAX_BODIES];
    int     frame;
    float   jitter_m;
    float   Rinj[3][3], tinj[3];   /* Motive = Rinj * (mirror ? S : I) * room + tinj */
    bool    mirror;
} Sim;

static Sim g_sim;

static void m3v(const float R[3][3], const float v[3], float o[3]) {
    for (int i = 0; i < 3; ++i) o[i] = R[i][0] * v[0] + R[i][1] * v[1] + R[i][2] * v[2];
}
static void m3tv(const float R[3][3], const float v[3], float o[3]) {
    for (int i = 0; i < 3; ++i) o[i] = R[0][i] * v[0] + R[1][i] * v[1] + R[2][i] * v[2];
}
static void norm3(float v[3]) { float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (n > 0) { v[0] /= n; v[1] /= n; v[2] /= n; } }
static void quat_aa(const float ax[3], float deg, float q[4]) {
    float a[3] = { ax[0], ax[1], ax[2] };
    norm3(a);
    float h = (float)(deg * PI / 360.0), s = sinf(h);
    q[0] = a[0] * s; q[1] = a[1] * s; q[2] = a[2] * s; q[3] = cosf(h);
}
static void to_motive(const Sim* s, const float room[3], float out[3], bool is_dir) {
    float v[3] = { s->mirror ? -room[0] : room[0], room[1], room[2] };
    m3v(s->Rinj, v, out);
    if (!is_dir) for (int k = 0; k < 3; ++k) out[k] += s->tinj[k];
}

/* marker spots on the baffle, (u, v) m about the tweeter: asymmetric (Motive needs that to tell the
 * markers apart) but CENTERED, sum zero, because the reported point is the marker centroid */
static const float SPOTS[4][2] = { { 0.065f, 0.00f }, { -0.045f, 0.05f }, { -0.04f, -0.06f }, { 0.02f, 0.01f } };

/* One speaker body: 4 markers on the baffle around the layout point (moved forward by the baffle
 * offset, which is where the baffle is when the layout point is the acoustic center), expressed in
 * a body frame at an arbitrary orientation, the way Motive stores a body created at any angle. */
static void sim_speaker_body(Sim* s, SimBody* b, const Layout* L, int idx, float aim_err_deg,
                             float baffle_off, float noise_m, int k) {
    float A[3];
    memcpy(A, L->speakers[idx].aim, sizeof A);
    if (aim_err_deg != 0.f) {                               /* tilt the truth about a perpendicular axis */
        float t[3] = { 0, 1, 0 }, ax[3];
        if (fabsf(A[1]) > 0.9f) { t[0] = 1; t[1] = 0; }
        ax[0] = t[1] * A[2] - t[2] * A[1]; ax[1] = t[2] * A[0] - t[0] * A[2]; ax[2] = t[0] * A[1] - t[1] * A[0];
        float q[4], R[3][3], o[3];
        quat_aa(ax, aim_err_deg, q);
        survey_quat_to_mat(q, R);
        m3v(R, A, o);
        memcpy(A, o, sizeof o);
    }
    memcpy(b->true_aim_room, A, sizeof A);
    float u[3], v[3], t[3] = { 0, 1, 0 };
    if (fabsf(A[1]) > 0.9f) { t[0] = 1; t[1] = 0; }
    u[0] = t[1] * A[2] - t[2] * A[1]; u[1] = t[2] * A[0] - t[0] * A[2]; u[2] = t[0] * A[1] - t[1] * A[0];
    norm3(u);
    v[0] = A[1] * u[2] - A[2] * u[1]; v[1] = A[2] * u[0] - A[0] * u[2]; v[2] = A[0] * u[1] - A[1] * u[0];
    const float* P = L->speakers[idx].pos;
    b->n_markers = 4;
    float pivot[3] = { 0, 0, 0 };
    for (int i = 0; i < 4; ++i) {
        float room[3];
        for (int c = 0; c < 3; ++c)
            room[c] = P[c] + baffle_off * A[c] + SPOTS[i][0] * u[c] + SPOTS[i][1] * v[c] + (float)(noise_m * grand());
        to_motive(s, room, b->world[i], false);
        for (int c = 0; c < 3; ++c) pivot[c] += b->world[i][c] * 0.25f;
    }
    const float bax[3] = { 0.3f + 0.2f * k, 1.f, -0.4f + 0.1f * k };
    quat_aa(bax, 25.f + 47.f * (float)k, b->q);              /* an arbitrary body orientation per speaker */
    memcpy(b->pivot, pivot, sizeof pivot);
    float R[3][3];
    survey_quat_to_mat(b->q, R);
    for (int i = 0; i < 4; ++i) {
        float d[3] = { b->world[i][0] - pivot[0], b->world[i][1] - pivot[1], b->world[i][2] - pivot[2] };
        m3tv(R, d, b->local[i]);
    }
    b->layout_idx = idx;
}

static void sim_build(Sim* s, const Args* a, const Layout* L) {
    memset(s, 0, sizeof *s);
    s->major = a->sim_major; s->minor = a->sim_minor;
    s->jitter_m = a->sim_jitter_mm * 0.001f;
    s->mirror = a->sim_mirror;
    const float y[3] = { 0, 1, 0 };
    float q[4];
    quat_aa(y, a->sim_yaw_deg, q);
    survey_quat_to_mat(q, s->Rinj);
    memcpy(s->tinj, a->sim_offset, sizeof s->tinj);
    for (int k = 0; k < a->sim_n && s->n < MAX_BODIES - 2; ++k) {
        SimBody* b = &s->b[s->n++];
        snprintf(b->name, sizeof b->name, (k & 1) ? "Speaker_%d" : "spk%02d", a->sim_idx[k]);   /* both spellings */
        b->id = 101 + k;
        sim_speaker_body(s, b, L, a->sim_idx[k], k == 0 ? a->sim_aim_error_deg : 0.f,
                         a->sim_depth_set ? a->sim_depth_m : a->opts.baffle_offset_m, a->sim_noise_mm * 0.001f, k);
    }
    /* two decoys: a wand, and a speaker body with a typo in its name */
    for (int d = 0; d < 2; ++d) {
        SimBody* b = &s->b[s->n++];
        snprintf(b->name, sizeof b->name, d == 0 ? "Wand" : "spkr%d", a->sim_n > 0 ? a->sim_idx[0] : 0);
        b->id = 7 + d;
        b->layout_idx = -1;
        b->n_markers = 3;
        const float mk[3][3] = { { 0.1f, 0, 0 }, { 0, 0.12f, 0 }, { 0, 0, 0.09f } };
        memcpy(b->local, mk, sizeof mk);
        b->pivot[0] = 0.5f * d; b->pivot[1] = 1.0f; b->pivot[2] = 0.3f;
        b->q[3] = 1.f;
        for (int i = 0; i < 3; ++i) for (int c = 0; c < 3; ++c) b->world[i][c] = b->pivot[c] + mk[i][c];
    }
}

/* the MODELDEF: markerset "all", then every body's description */
static size_t sim_modeldef(const Sim* s, uint8_t* buf, size_t cap) {
    Wr w = { buf, 0, cap };
    static uint8_t tmp[8192];
    wi32(&w, 1 + s->n);
    for (int d = -1; d < s->n; ++d) {
        Wr body = { tmp, 0, sizeof tmp };
        int32_t type;
        if (d < 0) {                                          /* markerset "all" */
            type = 0;
            wstr(&body, "all");
            int nm = 0;
            for (int i = 0; i < s->n; ++i) nm += s->b[i].n_markers;
            wi32(&body, nm);
            for (int i = 0; i < s->n; ++i)
                for (int k = 0; k < s->b[i].n_markers; ++k) { char nmn[48]; snprintf(nmn, sizeof nmn, "%s_%d", s->b[i].name, k + 1); wstr(&body, nmn); }
        } else {
            const SimBody* b = &s->b[d];
            type = 1;
            wstr(&body, b->name);
            wi32(&body, b->id);
            wi32(&body, -1);
            wf32(&body, 0); wf32(&body, 0); wf32(&body, 0);
            if (s->major > 4 || (s->major == 4 && s->minor >= 2)) { wf32(&body, 0); wf32(&body, 0); wf32(&body, 0); wf32(&body, 1); }
            wi32(&body, b->n_markers);
            for (int k = 0; k < b->n_markers; ++k) { wf32(&body, b->local[k][0]); wf32(&body, b->local[k][1]); wf32(&body, b->local[k][2]); }
            for (int k = 0; k < b->n_markers; ++k) wi32(&body, 0);
            if (s->major >= 4) for (int k = 0; k < b->n_markers; ++k) { char nmn[16]; snprintf(nmn, sizeof nmn, "Marker%d", k + 1); wstr(&body, nmn); }
        }
        wi32(&w, type);
        if (sized_sections(s->major, s->minor)) wi32(&w, (int32_t)body.n);
        wr(&w, tmp, body.n);
    }
    return w.n <= cap ? w.n : 0;
}

/* one FrameOfData: the "all" markerset, no legacy markers, every body (the decoys flicker), then
 * the empty sections and suffix a real frame carries */
static size_t sim_frame(Sim* s, uint8_t* buf, size_t cap) {
    Wr w = { buf, 0, cap };
    static uint8_t tmp[16384];
    const bool sized = sized_sections(s->major, s->minor);
    wi32(&w, s->frame);

    Wr ms = { tmp, 0, sizeof tmp };                           /* markersets */
    wstr(&ms, "all");
    int nm = 0;
    for (int i = 0; i < s->n; ++i) nm += s->b[i].n_markers;
    wi32(&ms, nm);
    for (int i = 0; i < s->n; ++i)
        for (int k = 0; k < s->b[i].n_markers; ++k) { wf32(&ms, s->b[i].world[k][0]); wf32(&ms, s->b[i].world[k][1]); wf32(&ms, s->b[i].world[k][2]); }
    wi32(&w, 1);
    if (sized) wi32(&w, (int32_t)ms.n);
    wr(&w, tmp, ms.n);

    wi32(&w, 0);                                              /* legacy other markers */
    if (sized) wi32(&w, 0);

    wi32(&w, s->n);                                           /* rigid bodies: 38 bytes each */
    if (sized) wi32(&w, 38 * s->n);
    for (int i = 0; i < s->n; ++i) {
        const SimBody* b = &s->b[i];
        float p[3], q[4];
        for (int c = 0; c < 3; ++c) p[c] = b->pivot[c] + (float)(s->jitter_m * grand());
        /* rotation jitter of the same size at the marker radius (about 7 cm) */
        const float ax[3] = { (float)grand(), (float)grand(), (float)grand() };
        float dq[4];
        quat_aa(ax, (float)(s->jitter_m / 0.07 * grand() * 180.0 / PI), dq);
        q[0] = dq[3] * b->q[0] + dq[0] * b->q[3] + dq[1] * b->q[2] - dq[2] * b->q[1];
        q[1] = dq[3] * b->q[1] - dq[0] * b->q[2] + dq[1] * b->q[3] + dq[2] * b->q[0];
        q[2] = dq[3] * b->q[2] + dq[0] * b->q[1] - dq[1] * b->q[0] + dq[2] * b->q[3];
        q[3] = dq[3] * b->q[3] - dq[0] * b->q[0] - dq[1] * b->q[1] - dq[2] * b->q[2];
        if (s->frame % 2) for (int c = 0; c < 4; ++c) q[c] = -q[c];     /* Motive may send either sign */
        const bool valid = !(b->layout_idx < 0 && s->frame % 5 == 0) && s->frame % 17 != 3;
        wi32(&w, b->id);
        for (int c = 0; c < 3; ++c) wf32(&w, p[c]);
        for (int c = 0; c < 4; ++c) wf32(&w, q[c]);
        wf32(&w, 0.0002f);
        wi16(&w, valid ? 1 : 0);
    }
    if (sized) {
        int hops = 5 + ((s->major == 4 && s->minor >= 5) || s->major > 4 ? 2 : 0);
        for (int h = 0; h < hops; ++h) { wi32(&w, 0); wi32(&w, 0); }
    } else {
        for (int h = 0; h < 4; ++h) wi32(&w, 0);              /* skeletons, labeled markers, plates, devices */
    }
    uint8_t suffix[46] = { 0 };                               /* timecode .. end-of-data, contents unused */
    wr(&w, suffix, sizeof suffix);
    ++s->frame;
    return w.n <= cap ? w.n : 0;
}

/* ---- the survey --------------------------------------------------------------------------------- */

typedef struct {
    int            desc;           /* index into g_desc */
    int            layout_idx;
    SurveyAvg      avg;
    bool           accepted;
    char           why[96];        /* refusal reason */
    float          pos_avg[3], q_avg[4], spread_m, spread_deg;
    SurveySpeaker  sp;
} Row;

static NatNetBodyDesc g_desc[MAX_BODIES];
static NatNetBody     g_bodies[MAX_BODIES];
static Row            g_rows[MAX_BODIES];
static uint8_t        g_buf[65536];

static const char* match_why(int st) {
    switch (st) {
    case SURVEY_MATCH_NO_RULE:      return "not spk<N>/speaker<N> and not in --map";
    case SURVEY_MATCH_OUT_OF_RANGE: return "index is not a speaker in this layout";
    case SURVEY_MATCH_DUPLICATE:    return "another body already holds that speaker";
    default:                        return "";
    }
}

int main(int argc, char** argv) {
    /* unbuffered through a pipe: a caller streaming the report (calib_view's Session tab) sees it live */
    setvbuf(stdout, NULL, _IONBF, 0);
    Args a;
    if (!parse_args(argc, argv, &a)) { usage(); return 1; }

    char err[512] = { 0 };
    if (!layout_load(a.layout_path, 48000, &g_layout, err, sizeof err)) {
        printf("error: %s\n", err);
        return 1;
    }
    read_aim_flags(a.layout_path);
    const Layout* L = &g_layout;
    const int count = (int)L->count;
    printf("bwa_speaker_survey: layout %s, %d speakers, listening point [%.3f %.3f %.3f] m\n",
           a.layout_path, count, L->ref[0], L->ref[1], L->ref[2]);

    /* ---- the source: live Motive, or the simulator ---- */
    int major = 0, minor = 0, nmodel = -1;
    NatNetRaw* raw = NULL;
    if (a.simulate) {
        if (a.sim_n == 0) {                                   /* four spread over the index range */
            for (int k = 0; k < 4 && k < count; ++k) a.sim_idx[a.sim_n++] = k * count / 4;
        }
        for (int k = 0; k < a.sim_n; ++k)
            if (a.sim_idx[k] < 0 || a.sim_idx[k] >= count) { printf("error: --sim-speakers %d is not in the layout\n", a.sim_idx[k]); return 1; }
        sim_build(&g_sim, &a, L);
        major = g_sim.major; minor = g_sim.minor;
        size_t n = sim_modeldef(&g_sim, g_buf, sizeof g_buf);
        nmodel = n ? (int)n : -1;
        printf("source: simulated Motive (NatNet %d.%d): %d speaker bodies, yaw %.2f deg, offset [%.3f %.3f %.3f] m%s,\n"
               "        aim error %.2f deg on speaker %d, marker noise %.2f mm, jitter %.2f mm\n",
               major, minor, a.sim_n, a.sim_yaw_deg, a.sim_offset[0], a.sim_offset[1], a.sim_offset[2],
               a.sim_mirror ? ", MIRRORED" : "", a.sim_aim_error_deg, a.sim_n ? a.sim_idx[0] : -1,
               a.sim_noise_mm, a.sim_jitter_mm);
    } else {
        raw = natnet_raw_open(&a.nc, err, sizeof err);
        if (!raw) { printf("error: %s\n", err); return 2; }
        natnet_raw_version(raw, &major, &minor);
        nmodel = natnet_raw_modeldef(raw, g_buf, sizeof g_buf);
        printf("source: Motive at %s (NatNet %d.%d), multicast %s\n", a.nc.server, major, minor,
               a.nc.multicast ? a.nc.multicast : "(none)");
        if (nmodel < 0) { printf("error: no model definitions from %s (is Motive streaming? right --server?)\n", a.nc.server); natnet_raw_close(raw); return 2; }
    }

    /* ---- the model: names, ids, marker offsets ---- */
    bool complete = false;
    const int ndesc = natnet_parse_modeldef(g_buf, (size_t)nmodel, major, minor, g_desc, MAX_BODIES, &complete);
    if (ndesc < 0) { printf("error: the model definitions did not parse (NatNet %d.%d)\n", major, minor); if (raw) natnet_raw_close(raw); return 2; }
    printf("model: %d rigid bodies described, %s\n", ndesc,
           complete ? "every description read" : "the description walk stopped early (see natnet.h)");

    static const char* names[MAX_BODIES];
    static SurveyMatch match[MAX_BODIES];
    static bool map_used[MAX_MAP];
    for (int i = 0; i < ndesc; ++i) names[i] = g_desc[i].name;
    survey_match_names(names, ndesc, a.map, a.nmap, count, match, map_used);
    int nrows = 0;
    for (int i = 0; i < ndesc; ++i) {
        if (match[i].status == SURVEY_MATCH_OK) {
            Row* r = &g_rows[nrows++];
            memset(r, 0, sizeof *r);
            r->desc = i;
            r->layout_idx = match[i].index;
            survey_avg_reset(&r->avg);
            printf("  \"%s\" (id %d) -> speaker %d%s\n", g_desc[i].name, g_desc[i].id, match[i].index, match[i].by_map ? " (--map)" : "");
        } else if (match[i].status == SURVEY_MATCH_DUPLICATE) {
            printf("  \"%s\" (id %d): ignored, %s (\"%s\")\n", g_desc[i].name, g_desc[i].id,
                   match_why(match[i].status), g_desc[match[i].other].name);
        } else {
            printf("  \"%s\" (id %d): ignored, %s\n", g_desc[i].name, g_desc[i].id, match_why(match[i].status));
        }
    }
    for (int k = 0; k < a.nmap; ++k)
        if (!map_used[k]) printf("  --map %s=%d: no rigid body has that name\n", a.map[k].name, a.map[k].index);
    if (nrows == 0) {
        printf("error: no rigid body maps to a speaker. Name them spk<N> or speaker<N>, or pass --map name=index.\n");
        if (raw) natnet_raw_close(raw);
        return 2;
    }

    /* ---- collect ---- */
    int frames = 0, bad_frames = 0;
    if (a.simulate) {
        const int want = (int)(a.seconds * 120.0 + 0.5);
        for (int f = 0; f < want; ++f) {
            size_t n = sim_frame(&g_sim, g_buf, sizeof g_buf);
            int nb = natnet_parse_bodies(g_buf, n, major, minor, g_bodies, MAX_BODIES);
            if (nb < 0) { ++bad_frames; continue; }
            ++frames;
            for (int b = 0; b < nb; ++b) {
                if (!g_bodies[b].tracking_valid) continue;
                for (int r = 0; r < nrows; ++r)
                    if (g_desc[g_rows[r].desc].id == g_bodies[b].id) survey_avg_add(&g_rows[r].avg, g_bodies[b].pos, g_bodies[b].quat);
            }
        }
    } else {
        printf("collecting %.1f s of frames...\n", a.seconds);
        const uint64_t end = os_monotonic_ns() + (uint64_t)(a.seconds * 1e9);
        while (os_monotonic_ns() < end) {
            int n = natnet_raw_next_frame(raw, g_buf, sizeof g_buf);
            if (n < 0) { printf("error: socket receive failed\n"); break; }
            if (n == 0) continue;
            int nb = natnet_parse_bodies(g_buf, (size_t)n, major, minor, g_bodies, MAX_BODIES);
            if (nb < 0) { ++bad_frames; continue; }
            ++frames;
            for (int b = 0; b < nb; ++b) {
                if (!g_bodies[b].tracking_valid) continue;
                for (int r = 0; r < nrows; ++r)
                    if (g_desc[g_rows[r].desc].id == g_bodies[b].id) survey_avg_add(&g_rows[r].avg, g_bodies[b].pos, g_bodies[b].quat);
            }
        }
        natnet_raw_close(raw);
    }
    printf("frames: %d parsed, %d malformed\n", frames, bad_frames);
    if (frames == 0) {
        printf("error: no frames arrived. Check that Motive is streaming rigid bodies, the multicast group\n"
               "       and interface (--multicast, --local), and the firewall.\n");
        return 2;
    }

    /* ---- evaluate ---- */
    int naccepted = 0, naims = 0;
    for (int r = 0; r < nrows; ++r) {
        Row* w = &g_rows[r];
        const NatNetBodyDesc* d = &g_desc[w->desc];
        if (w->avg.n < SURVEY_MIN_FRAMES) {
            snprintf(w->why, sizeof w->why, "%d tracking-valid frames, need %d", w->avg.n, SURVEY_MIN_FRAMES);
            continue;
        }
        survey_avg_result(&w->avg, w->pos_avg, w->q_avg, &w->spread_m, &w->spread_deg);
        if (w->spread_m > a.max_spread_m || w->spread_deg > a.max_spread_deg) {
            snprintf(w->why, sizeof w->why, "unsteady: spread %.2f mm, %.3f deg (limits %.2f mm, %.3f deg)",
                     w->spread_m * 1000.f, w->spread_deg, a.max_spread_m * 1000.f, a.max_spread_deg);
            continue;
        }
        survey_speaker(w->pos_avg, w->q_avg, (const float (*)[3])d->markers, d->n_stored, L->ref, &a.opts, &w->sp);
        w->accepted = true;
        ++naccepted;
        if (w->sp.aim_ok) ++naims;
    }

    /* ---- report ---- */
    FILE* csv = NULL;
    if (a.csv_path) {
        csv = os_fopen(a.csv_path, "wb");
        if (!csv) printf("warning: cannot open %s for writing\n", a.csv_path);
        else fprintf(csv, "index,name,id,frames,spread_mm,spread_deg,accepted,opt_x,opt_y,opt_z,lay_x,lay_y,lay_z,pos_delta_mm,"
                          "aim_status,opt_ax,opt_ay,opt_az,lay_ax,lay_ay,lay_az,lay_aim_explicit,aim_delta_deg,markers,plane_rms_mm,aspect\n");
    }
    printf("\n");
    for (int r = 0; r < nrows; ++r) {
        const Row* w = &g_rows[r];
        const NatNetBodyDesc* d = &g_desc[w->desc];
        const Speaker* S = &L->speakers[w->layout_idx];
        printf("speaker %d  \"%s\" id %d  %d frames", w->layout_idx, d->name, d->id, w->avg.n);
        if (!w->accepted) {
            printf("  REFUSED: %s\n", w->why);
            if (csv) fprintf(csv, "%d,%s,%d,%d,,,0,,,,,,,,refused,,,,,,,,,%d,,\n", w->layout_idx, d->name, d->id, w->avg.n, d->n_markers);
            continue;
        }
        const float dpos = sqrtf((w->sp.pos[0] - S->pos[0]) * (w->sp.pos[0] - S->pos[0]) +
                                 (w->sp.pos[1] - S->pos[1]) * (w->sp.pos[1] - S->pos[1]) +
                                 (w->sp.pos[2] - S->pos[2]) * (w->sp.pos[2] - S->pos[2]));
        const float daim = w->sp.aim_ok ? survey_angle_deg(w->sp.aim, S->aim) : -1.f;
        printf("  spread %.2f mm, %.3f deg\n", w->spread_m * 1000.f, w->spread_deg);
        printf("  position  optical [%7.3f %7.3f %7.3f]  layout [%7.3f %7.3f %7.3f]  delta %6.1f mm%s\n",
               w->sp.pos[0], w->sp.pos[1], w->sp.pos[2], S->pos[0], S->pos[1], S->pos[2], dpos * 1000.f,
               w->sp.aim_ok ? "" : "  (marker centroid: no aim, so no baffle offset)");
        if (w->sp.aim_ok)
            printf("  aim       optical [%7.3f %7.3f %7.3f]  layout [%7.3f %7.3f %7.3f]  delta %6.2f deg  (layout aim %s)%s\n",
                   w->sp.aim[0], w->sp.aim[1], w->sp.aim[2], S->aim[0], S->aim[1], S->aim[2], daim,
                   g_aim_explicit[w->layout_idx] ? "explicit" : "default: toward the listening point",
                   w->sp.aim_away ? "  WARNING: points away from the listening point" : "");
        else
            printf("  aim       NONE: %s\n", survey_aim_status_str(w->sp.aim_status));
        printf("  markers   %d, plane rms %.2f mm, aspect %.2f (%s)\n", w->sp.n_markers,
               w->sp.plane_rms_m * 1000.f, w->sp.aspect, survey_aim_status_str(w->sp.aim_status));
        if (csv)
            fprintf(csv, "%d,%s,%d,%d,%.4f,%.4f,1,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.2f,%s,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%d,%.3f,%d,%.3f,%.3f\n",
                    w->layout_idx, d->name, d->id, w->avg.n, w->spread_m * 1000.f, w->spread_deg,
                    w->sp.pos[0], w->sp.pos[1], w->sp.pos[2], S->pos[0], S->pos[1], S->pos[2], dpos * 1000.f,
                    survey_aim_status_str(w->sp.aim_status), w->sp.aim[0], w->sp.aim[1], w->sp.aim[2],
                    S->aim[0], S->aim[1], S->aim[2], g_aim_explicit[w->layout_idx] ? 1 : 0, daim,
                    w->sp.n_markers, w->sp.plane_rms_m * 1000.f, w->sp.aspect);
    }
    if (csv) fclose(csv);

    /* ---- frame agreement ---- */
    static float opt[BWA_MAX_CHANNELS][3], lay[BWA_MAX_CHANNELS][3], oaim[BWA_MAX_CHANNELS][3], laim[BWA_MAX_CHANNELS][3];
    static bool aok[BWA_MAX_CHANNELS];
    static int  who[BWA_MAX_CHANNELS];
    int nf = 0;
    for (int r = 0; r < nrows && nf < BWA_MAX_CHANNELS; ++r) {
        const Row* w = &g_rows[r];
        if (!w->accepted) continue;
        memcpy(opt[nf], w->sp.pos, sizeof opt[nf]);
        memcpy(lay[nf], L->speakers[w->layout_idx].pos, sizeof lay[nf]);
        memcpy(oaim[nf], w->sp.aim, sizeof oaim[nf]);
        memcpy(laim[nf], L->speakers[w->layout_idx].aim, sizeof laim[nf]);
        aok[nf] = w->sp.aim_ok;
        who[nf] = r;
        ++nf;
    }
    printf("\nframe agreement: %d speaker%s, optical (Motive) -> layout (room), rigid, no scale\n", nf, nf == 1 ? "" : "s");
    SurveyFrameFit fit;
    bool have_fit = false;
    const char* hand_str = "n/a";
    if (nf >= 3 && survey_fit_frame((const float (*)[3])opt, (const float (*)[3])lay, (const float (*)[3])oaim,
                                    (const float (*)[3])laim, aok, nf, &fit)) {
        have_fit = true;
        const float tlen = sqrtf(fit.t[0] * fit.t[0] + fit.t[1] * fit.t[1] + fit.t[2] * fit.t[2]);
        printf("  rotation     %.2f deg about [%6.3f %6.3f %6.3f]\n", fit.angle_deg, fit.axis[0], fit.axis[1], fit.axis[2]);
        printf("  translation  [%7.3f %7.3f %7.3f] m (%.1f mm)\n", fit.t[0], fit.t[1], fit.t[2], tlen * 1000.f);
        printf("  residual     rms %.1f mm, max %.1f mm (speaker %d)\n", fit.rms_m * 1000.f, fit.max_m * 1000.f,
               g_rows[who[fit.max_i]].layout_idx);
        printf("  mirrored     a fit with Motive's x axis flipped leaves rms %.1f mm\n", fit.rms_mirror_m * 1000.f);
        if (fit.aim_deg >= 0.f)
            printf("  aims         %.2f deg mean off the layout after the fit, %.2f deg under the mirrored fit\n",
                   fit.aim_deg, fit.aim_mirror_deg);
        printf("  spread       the matched speakers sit %.0f mm RMS off their best plane\n", fit.thickness_m * 1000.f);
        hand_str = fit.hand == SURVEY_HAND_OK ? "OK" : fit.hand == SURVEY_HAND_MIRRORED ? "MIRRORED" : "UNDETERMINED";
        const char* by = fit.hand_by == SURVEY_HAND_BY_POSITIONS ? "positions" : fit.hand_by == SURVEY_HAND_BY_AIMS ? "aims" : "nothing";
        printf("  handedness   %s (decided by %s)\n", hand_str, by);
        if (fit.hand == SURVEY_HAND_UNDETERMINED)
            printf("               the matched speakers are (nearly) coplanar and their aims do not leave that plane,\n"
                   "               so a mirror fits exactly as well as the truth. Add a speaker off that plane.\n");
        printf("  after the fit, per speaker (what is left once the frame is corrected):\n");
        for (int i = 0; i < nf; ++i) {
            float p[3], d = 0.f;
            m3v(fit.R, opt[i], p);
            for (int k = 0; k < 3; ++k) { p[k] += fit.t[k]; d += (p[k] - lay[i][k]) * (p[k] - lay[i][k]); }
            printf("    speaker %2d  position %6.1f mm", g_rows[who[i]].layout_idx, sqrtf(d) * 1000.f);
            if (aok[i]) { float v[3]; m3v(fit.R, oaim[i], v); printf("  aim %6.2f deg", survey_angle_deg(v, laim[i])); }
            printf("\n");
        }
        if (fit.hand == SURVEY_HAND_MIRRORED)
            printf("  verdict      MIRRORED: Motive's frame has the opposite handedness to the layout's. Fix the axis\n"
                   "               convention in Motive (or the layout's source), then re-run. --write refuses.\n"
                   "               The rotation and translation above are the best PROPER fit and mean nothing here.\n");
        else if (fit.angle_deg <= FRAME_MAX_DEG && tlen <= FRAME_MAX_M)
            printf("  verdict      Motive's frame agrees with the room frame (within 0.5 deg and 10 mm)\n");
        else
            printf("  verdict      Motive's frame is OFF the room frame by %.2f deg and %.1f mm. Fix the ground plane\n"
                   "               in Motive (the engine takes poses unchanged), then re-run.\n", fit.angle_deg, tlen * 1000.f);
    } else if (nf == 2) {
        const float dd = survey_pair_delta(opt[0], opt[1], lay[0], lay[1]);
        printf("  two speakers: their optical distance differs from the layout's by %.1f mm.\n"
               "  Rotation, translation and handedness need 3 or more; 4 off one plane to see a mirror.\n", dd * 1000.f);
    } else if (nf >= 3) {
        printf("  no fit: the matched layout points are (nearly) on one line\n");
    } else {
        printf("  needs 2 or more accepted speakers\n");
    }

    /* ---- baffle depth: the optical points against MEASURED acoustic positions ----
     * Layout positions are acoustic centers. Where --localize measured them, each optical point sits in
     * front of its center by the baffle-to-acoustic-center depth, along the box's aim. Only after the
     * frame is known does that difference mean anything, and a plain frame fit absorbs much of the depth
     * into its translation, so the depth is solved INSIDE a refit of the frame (survey_fit_depth), over
     * the measured speakers only. A plan position was never measured: comparing against it would read
     * the installer's tape, not the acoustic center. */
    enum { DEPTH_NO_DATA = 0, DEPTH_NO_FRAME, DEPTH_FEW, DEPTH_BAD_GEOMETRY, DEPTH_FRAME_OFF, DEPTH_OK };
    int   depth_state = DEPTH_NO_DATA, nd = 0, ndisagree = 0;
    float depth_med = 0.f, depth_worst_across = 0.f;
    SurveyDepthFit dfit;
    memset(&dfit, 0, sizeof dfit);
    {
        static float dopt[BWA_MAX_CHANNELS][3], daim[BWA_MAX_CHANNELS][3], dlay[BWA_MAX_CHANNELS][3];
        static float ddep[BWA_MAX_CHANNELS], dacr[BWA_MAX_CHANNELS], dsort[BWA_MAX_CHANNELS];
        static int   dli[BWA_MAX_CHANNELS];
        const float b = a.opts.baffle_offset_m;
        printf("\nbaffle depth: each optical point against its MEASURED acoustic position, along the box's optical aim,\n"
               "              the depth solved inside a refit of the frame (positive = the center is behind the baffle)\n");
        for (int i = 0; i < nf; ++i) {
            const int li = g_rows[who[i]].layout_idx;
            const Speaker* S = &L->speakers[li];
            const char* skip = NULL;
            if (!aok[i])           skip = "no optical aim, so no \"along the aim\"";
            else if (!S->has_plan) skip = "no plan_position: its position was never measured";
            else if (fabsf(S->pos[0] - S->plan_pos[0]) <= 1e-6f && fabsf(S->pos[1] - S->plan_pos[1]) <= 1e-6f &&
                     fabsf(S->pos[2] - S->plan_pos[2]) <= 1e-6f)
                                   skip = "its position equals its plan_position: never measured";
            if (skip) { printf("    speaker %2d  skipped, %s\n", li, skip); continue; }
            memcpy(dopt[nd], opt[i], sizeof dopt[nd]);
            memcpy(daim[nd], oaim[i], sizeof daim[nd]);
            memcpy(dlay[nd], S->pos, sizeof dlay[nd]);
            dli[nd++] = li;
        }
        if (nd == 0) {
            printf("  not measured: no matched speaker has a measured position. Run this on --localize's output\n"
                   "  (as_built.json), not on the plan.\n");
        } else if (!have_fit || fit.hand != SURVEY_HAND_OK) {
            depth_state = DEPTH_NO_FRAME;
            printf("  not measured: the frame check has no right-handed fit (above). Fix the frame first.\n");
        } else if (nd < 3) {
            depth_state = DEPTH_FEW;
            printf("  not measured: %d measured speaker%s with an aim; the refit needs 3 or more\n", nd, nd == 1 ? "" : "s");
        } else if (!survey_fit_depth((const float (*)[3])dopt, (const float (*)[3])daim, (const float (*)[3])dlay, nd,
                                     &dfit, ddep, dacr)) {
            depth_state = DEPTH_BAD_GEOMETRY;
            printf("  not measured: the measured speakers face too nearly one way (the depth is then a translation),\n"
                   "  or stand on one line\n");
        } else if (!(dfit.angle_deg <= FRAME_MAX_DEG && dfit.t_m <= FRAME_MAX_M)) {
            depth_state = DEPTH_FRAME_OFF;
            printf("  not measured: with the depth solved, Motive's frame is still off the room frame by %.2f deg and\n"
                   "  %.1f mm. An unaligned frame makes the difference meaningless: fix the frame first.\n",
                   dfit.angle_deg, dfit.t_m * 1000.f);
        } else {
            depth_state = DEPTH_OK;
            for (int k = 0; k < nd; ++k) dsort[k] = ddep[k];
            for (int x = 1; x < nd; ++x) { const float v = dsort[x]; int y = x;           /* insertion sort */
                while (y > 0 && dsort[y - 1] > v) { dsort[y] = dsort[y - 1]; --y; } dsort[y] = v; }
            depth_med = (nd & 1) ? dsort[nd / 2] : 0.5f * (dsort[nd / 2 - 1] + dsort[nd / 2]);
            for (int k = 0; k < nd; ++k) {
                const bool off = fabsf(ddep[k] - depth_med) > SURVEY_DEPTH_AGREE_M || dacr[k] > SURVEY_DEPTH_AGREE_M;
                if (dacr[k] > depth_worst_across) depth_worst_across = dacr[k];
                ndisagree += off;
                printf("    speaker %2d  depth %+7.1f mm  across the aim %5.1f mm%s\n", dli[k], ddep[k] * 1000.f,
                       dacr[k] * 1000.f, off ? "  <-- DISAGREES" : "");
            }
            printf("  median %+.1f mm over %d speakers, spread %.1f mm (%+.1f to %+.1f), across the aim %.1f mm at worst\n",
                   depth_med * 1000.f, nd, (dsort[nd - 1] - dsort[0]) * 1000.f, dsort[0] * 1000.f, dsort[nd - 1] * 1000.f,
                   depth_worst_across * 1000.f);
            printf("  the refit: depth %+.1f mm, the frame %.2f deg and %.1f mm with it solved, rms %.1f mm; the depth moves\n"
                   "  %.2f mm per mm of position error (aim leverage %.2f)\n", dfit.depth_m * 1000.f, dfit.angle_deg,
                   dfit.t_m * 1000.f, dfit.rms_m * 1000.f, 1.f / dfit.leverage, dfit.leverage);
            if (b != 0.f)
                printf("  these are the RESIDUAL beyond --baffle-offset-m %.4f: the depth itself is %+.1f mm\n", b,
                       (b + depth_med) * 1000.f);
            if (dfit.leverage < SURVEY_DEPTH_MIN_LEVERAGE)
                printf("  WARNING: the measured speakers face nearly one way, so position error moves the depth a lot.\n"
                       "           Add a visible speaker that faces another way.\n");
            if (ndisagree)
                printf("  WARNING: %d speaker%s off the median depth, or across the aim, by more than %.0f mm: the optical\n"
                       "           and acoustic surveys disagree there. Check that speaker's body, aim and measured position.\n",
                       ndisagree, ndisagree == 1 ? " is" : "s are", SURVEY_DEPTH_AGREE_M * 1000.f);
            const float tl = sqrtf(fit.t[0] * fit.t[0] + fit.t[1] * fit.t[1] + fit.t[2] * fit.t[2]);
            if (!(fit.angle_deg <= FRAME_MAX_DEG && tl <= FRAME_MAX_M))
                printf("  note: the frame check above failed with --baffle-offset-m %.4f, and the refit agrees once the\n"
                       "        depth is solved. Rerun with the value below.\n", b);
            const float sug = fabsf(b + depth_med) < 0.00005f ? 0.f : b + depth_med;    /* no "-0.0000" */
            printf("  suggested --baffle-offset-m %.4f\n", sug);
        }
    }
    int rc = 0;
    const char* sim_verdict = NULL;         /* the summary line's tail, so ONE line says everything */
    /* --require-frame: the report is the instrument, and its exit code says nothing about the frame
     * unless asked. A caller that reads only the exit code (the Session tab) asks, so a mirrored or
     * rotated Motive frame stops the run instead of reading as a pass. */
    const char* frame_verdict = NULL;
    if (a.require_frame) {
        const float tl = have_fit ? sqrtf(fit.t[0] * fit.t[0] + fit.t[1] * fit.t[1] + fit.t[2] * fit.t[2]) : 0.f;
        if (!have_fit)                                frame_verdict = "frame check FAIL (no fit: 3 or more accepted speakers off one line)";
        else if (fit.hand == SURVEY_HAND_MIRRORED)     frame_verdict = "frame check FAIL (mirrored)";
        else if (fit.hand == SURVEY_HAND_UNDETERMINED) frame_verdict = "frame check FAIL (handedness undetermined)";
        else if (!(fit.angle_deg <= FRAME_MAX_DEG && tl <= FRAME_MAX_M)) frame_verdict = "frame check FAIL (off the room frame)";
        else                                           frame_verdict = "frame check PASS";
        printf("\n%s\n", frame_verdict);
        if (strstr(frame_verdict, "FAIL")) rc = 3;
    }
    char write_verdict[160] = { 0 };

    /* ---- simulate: check the answer against what was injected ---- */
    if (a.simulate) {
        int bad = 0;
        const float tol_deg = 0.25f + 2.f * a.sim_noise_mm + 2.f * a.sim_jitter_mm;
        const float tol_m   = 0.001f + 3.f * a.sim_noise_mm * 0.001f + 3.f * a.sim_jitter_mm * 0.001f;
        if (naccepted != a.sim_n) { printf("simulate check: %d of %d speaker bodies accepted\n", naccepted, a.sim_n); ++bad; }
        if (naims != naccepted)   { printf("simulate check: %d of %d aims measured\n", naims, naccepted); ++bad; }
        if (a.sim_n >= 3) {
            if (!have_fit) { printf("simulate check: no frame fit\n"); ++bad; }
            else if (a.sim_mirror) {
                if (fit.hand == SURVEY_HAND_OK) { printf("simulate check: an injected mirror read as right-handed\n"); ++bad; }
            } else {
                if (fit.hand == SURVEY_HAND_MIRRORED) { printf("simulate check: a right-handed frame read as MIRRORED\n"); ++bad; }
                /* With a baffle depth unlike --baffle-offset-m every point is pushed along its aim, and the
                 * plain fit absorbs part of the push (survey.h): the frame to hold to the truth is then the
                 * depth refit's, when there is one */
                const bool depth_wrong = a.sim_depth_set && a.sim_depth_m != a.opts.baffle_offset_m;
                const bool use_refit = depth_wrong && depth_state == DEPTH_OK;
                const float  ang = use_refit ? dfit.angle_deg : fit.angle_deg;
                const float* ft  = use_refit ? dfit.t : fit.t;
                if (depth_wrong && !use_refit)
                    printf("simulate check: the baffle depth differs from --baffle-offset-m and was not measured, so the\n"
                           "                frame's rotation and translation are not checked\n");
                else {
                    if (fabsf(ang - fabsf(a.sim_yaw_deg)) > 0.05f + tol_deg * 0.2f) {
                        printf("simulate check: rotation %.3f deg, injected %.3f\n", ang, fabsf(a.sim_yaw_deg)); ++bad;
                    }
                    /* truth: layout = Rinj^T (optical - tinj), so t = -Rinj^T tinj */
                    float tt[3];
                    m3tv(g_sim.Rinj, g_sim.tinj, tt);
                    float e = 0.f;
                    for (int k = 0; k < 3; ++k) e += (ft[k] + tt[k]) * (ft[k] + tt[k]);
                    if (sqrtf(e) > tol_m) { printf("simulate check: translation off the injected one by %.2f mm%s\n", sqrtf(e) * 1000.f,
                                                   use_refit ? " (the depth refit)" : ""); ++bad; }
                }
                /* each aim, after the frame fit, against the aim the sim built */
                for (int i = 0; i < nf; ++i) {
                    if (!aok[i]) continue;
                    const int li = g_rows[who[i]].layout_idx;
                    const SimBody* sb = NULL;
                    for (int b = 0; b < g_sim.n; ++b) if (g_sim.b[b].layout_idx == li) { sb = &g_sim.b[b]; break; }
                    if (!sb) continue;
                    float v[3];
                    m3v(fit.R, oaim[i], v);
                    float e_deg = survey_angle_deg(v, sb->true_aim_room);
                    if (e_deg > tol_deg) { printf("simulate check: speaker %d aim %.3f deg off the truth\n", li, e_deg); ++bad; }
                }
            }
        }
        /* the baffle depth, against the depth the sim built the baffles at: measured exactly when the
         * injected frame is within the frame check's tolerance, and then right */
        if (a.sim_depth_set) {
            const float off = sqrtf(a.sim_offset[0] * a.sim_offset[0] + a.sim_offset[1] * a.sim_offset[1] +
                                    a.sim_offset[2] * a.sim_offset[2]);
            const bool frame_in = !a.sim_mirror && fabsf(a.sim_yaw_deg) <= FRAME_MAX_DEG && off <= FRAME_MAX_M;
            if (frame_in && depth_state != DEPTH_OK) {
                printf("simulate check: the injected frame is within tolerance, but no baffle depth was measured\n"); ++bad;
            }
            if (!frame_in && depth_state == DEPTH_OK) {
                printf("simulate check: a baffle depth was measured through an injected frame that is off\n"); ++bad;
            }
            if (depth_state == DEPTH_OK) {
                const float got = a.opts.baffle_offset_m + depth_med, err = fabsf(got - a.sim_depth_m);
                printf("simulate check: baffle depth %.2f mm, injected %.2f (off by %.3f mm, tolerance %.2f)\n",
                       got * 1000.f, a.sim_depth_m * 1000.f, err * 1000.f, tol_m * 1000.f);
                if (err > tol_m) { printf("simulate check: the baffle depth is off the injected one\n"); ++bad; }
                if (depth_worst_across > tol_m) {
                    printf("simulate check: %.2f mm across the aim, the surveys agree exactly here\n", depth_worst_across * 1000.f); ++bad;
                }
            }
        }
        printf("simulate check: %s\n", bad ? "FAIL" : "PASS");
        sim_verdict = bad ? "simulate FAIL" : "simulate PASS";
        if (bad) rc = 3;
    }

    /* ---- write ---- */
    if (a.write_path) do {
        if (have_fit && fit.hand == SURVEY_HAND_MIRRORED) {
            printf("write: REFUSED, the frame is mirrored\n");
            snprintf(write_verdict, sizeof write_verdict, "write refused");
            rc = 3;
            break;
        }
        static bool do_aim[BWA_MAX_CHANNELS], do_pos[BWA_MAX_CHANNELS];
        static float waim[BWA_MAX_CHANNELS][3], wpos[BWA_MAX_CHANNELS][3];
        int na = 0, np = 0;
        for (int r = 0; r < nrows; ++r) {
            const Row* w = &g_rows[r];
            if (!w->accepted) continue;
            const int i = w->layout_idx;
            if (a.write_aim && w->sp.aim_ok) { do_aim[i] = true; memcpy(waim[i], w->sp.aim, sizeof waim[i]); ++na; }
            if (a.write_pos)                 { do_pos[i] = true; memcpy(wpos[i], w->sp.pos, sizeof wpos[i]); ++np; }
        }
        if (have_fit && !(fit.angle_deg <= FRAME_MAX_DEG && sqrtf(fit.t[0] * fit.t[0] + fit.t[1] * fit.t[1] + fit.t[2] * fit.t[2]) <= FRAME_MAX_M))
            printf("write: WARNING, Motive's frame disagrees with the layout's; these values are in Motive's frame\n");
        int npn = 0, npk = 0;
        if (!write_layout(a.layout_path, a.write_path, do_aim, (const float (*)[3])waim, do_pos,
                          (const float (*)[3])wpos, count, &npn, &npk, err, sizeof err)) {
            printf("write: FAILED, %s\n", err);
            snprintf(write_verdict, sizeof write_verdict, "write FAILED");
            rc = 3;
            break;
        }
        static Layout check;                /* the written file must load, or it must not be trusted */
        const bool reload = layout_load(a.write_path, 48000, &check, err, sizeof err);
        printf("write: %s, %d aim%s and %d position%s; reload %s%s%s\n", a.write_path, na, na == 1 ? "" : "s",
               np, np == 1 ? "" : "s", reload ? "OK" : "FAILED (", reload ? "" : err, reload ? "" : ")");
        printf("write: plan recorded for %d speaker(s) (the position and aim this run replaced are now their\n"
               "       plan_position and plan_aim), left alone on %d that already had one\n", npn, npk);
        snprintf(write_verdict, sizeof write_verdict, "wrote %d aims, %d positions (reload %s), plan %d new %d kept",
                 na, np, reload ? "OK" : "FAILED", npn, npk);
        if (!reload) rc = 3;
    } while (0);

    /* ---- one line that says everything (the ctests read it) ---- */
    if (have_fit)
        printf("\nsummary: matched %d, accepted %d, aims %d, rotation %.2f deg, translation %.1f mm, rms %.1f mm, handedness %s",
               nrows, naccepted, naims, fit.angle_deg,
               sqrtf(fit.t[0] * fit.t[0] + fit.t[1] * fit.t[1] + fit.t[2] * fit.t[2]) * 1000.f, fit.rms_m * 1000.f, hand_str);
    else
        printf("\nsummary: matched %d, accepted %d, aims %d, no frame fit", nrows, naccepted, naims);
    if (sim_verdict)      printf(", %s", sim_verdict);
    if (frame_verdict)    printf(", %s", frame_verdict);
    if (write_verdict[0]) printf(", %s", write_verdict);
    if (depth_state == DEPTH_OK)
        printf(", baffle depth%s %+.1f mm over %d", a.opts.baffle_offset_m != 0.f ? " residual" : "", depth_med * 1000.f, nd);
    else if (depth_state != DEPTH_NO_DATA)
        printf(", baffle depth not measured");
    printf("\n");
    return rc;
}
