/*
 * layout_tool.c — interactive speaker-layout authoring for the CAVE array.
 *
 * Define where the 26 speakers physically are and export a valid cave_layout.json (the file the
 * engine loads via BwConfig.layout_path; see docs/layout-schema.md). The killer feature is identify-
 * by-ear: each speaker's INDEX is its bus/output channel, so selecting speaker N and pressing T
 * drives that exact channel with the built-in test signal (bw_test_signal) out the cave profile ->
 * DVS -> the physical speaker. So the survey loop is: pick an index, press T, walk to whichever
 * speaker sounds, read its position, and place marker N there (nudge, or ENTER to type exact coords).
 *
 * On export, delay_ms is auto-computed from the positions (max-distance alignment: the farthest
 * speaker from the origin gets 0, nearer ones wait for it); gain_db is your per-speaker trim. The
 * dbap knobs round-trip. Loads an existing layout (argv[1] or ./cave_layout.json) to keep iterating.
 *
 * Coordinate frame: room space, right-handed, origin at the working-area centre (matches Motive) —
 * the same numbers the JSON stores. The 3D view renders them directly; the audition (which physical
 * speaker sounds) is the ground truth for the channel<->speaker map, the numeric readout for position.
 *
 * Press P for a DBAP PREVIEW: a pink-noise source moves through your in-progress layout (the tool
 * rebuilds the engine with the edited positions, since the layout is load-time), so you can hear the
 * panning — gaps, smoothness, holes — and at the CAVE you walk the room to judge off-center coverage.
 *
 * Press C for a COVERAGE overlay: each direction is shaded by its nearest-speaker angular gap (green
 * = a speaker sits near that direction, red = a hole the DBAP pan would smear across far speakers). V
 * switches the observer model — both are real deployments: FIXED (a single audience sweet spot, a
 * supported mode served by a static listener; see docs/spatialization.md) or MOVING (the default:
 * mean coverage over a grid of listener positions across the working volume). Watch the worst-direction
 * number drop as you move speakers to fill the red patches — that is the layout optimization.
 *
 * Controls (edit): [ ] select speaker (or left-click)   arrows X/Z, R/F Y (SHIFT = fine)
 *           ENTER type "x y z"   PgUp/PgDn gain_db   T tone   N sine/noise   C coverage   V observer
 *           G switch the coverage shading: nearest-speaker gap (geometric) <-> the selected panner's
 *           per-direction rE-localization error (its real solve, cached + recomputed on a throttle)
 *           X score the layout for each panner (DBAP/SPCAP/VBAP rE-localization error)   S save   L reload
 *           B select the target panner   O auto-optimize the layout for it (a hill-climb that minimises
 *           the panner's rE error subject to the constraints; runs live, O again to stop, then S to save).
 *           K snap all speakers to the nearest allowed point. Drop a `constraints.json` next to the layout
 *           (an allowed `bounds` box + a `nogo` list of boxes for screens/structure) and the tool draws
 *           them (green bounds / red no-go), flags speakers that violate (red ring), and K projects them in.
 * Controls (preview, P toggles): WASD/RF move the source   SPACE auto-orbit/near-far/high-low sweep
 *           B A/B the panner DBAP<->SPCAP<->VBAP live   Common: right-drag/wheel camera   ESC quit
 * Build: cmake -S . -B build -DBWAUDIO_BUILD_PLAYGROUND=ON && cmake --build build --target bw_layout_tool
 */
#include "bwaudio.h"
#include "raylib.h"
#include "raymath.h"
#include "cJSON.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NSPK           26
#define SR             48000u
#define SPEED_OF_SOUND 343.0f
#define TEST_GAIN      0.4f

typedef struct { Vector3 pos; float gain_db; } Spk;
static Spk spk[NSPK];

/* dbap knobs (round-tripped through the file; defaults match layout_default) */
static float       dbap_r = 0.5f, dist_ref = 1.0f, dist_rolloff = 1.0f, dist_min_db = -40.0f;
static const char* dist_model = "inverse";

/* the engine's default 3x3x3-minus-centre grid — the starting point and the layout_default() order */
static void seed_default(void) {
    const float ax[3] = { -1.5f, 0.0f, 1.5f };
    int k = 0;
    for (int yi = 0; yi < 3; ++yi) for (int xi = 0; xi < 3; ++xi) for (int zi = 0; zi < 3; ++zi) {
        if (ax[xi] == 0 && ax[yi] == 0 && ax[zi] == 0) continue;
        spk[k].pos = (Vector3){ ax[xi], ax[yi], ax[zi] };
        spk[k].gain_db = 0.0f;
        ++k;
    }
}

/* load positions/gain + dbap knobs from an existing cave_layout.json; returns #speakers read (0 = none) */
static int load_json(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return 0; }
    char* buf = (char*)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return 0; }
    size_t rd = fread(buf, 1, (size_t)n, f); buf[rd] = 0; fclose(f);
    cJSON* root = cJSON_Parse(buf); free(buf);
    if (!root) return 0;

    int loaded = 0;
    cJSON* spks = cJSON_GetObjectItemCaseSensitive(root, "speakers");
    if (cJSON_IsArray(spks)) {
        cJSON* sp;
        cJSON_ArrayForEach(sp, spks) {
            cJSON* idxj = cJSON_GetObjectItemCaseSensitive(sp, "index");
            cJSON* posj = cJSON_GetObjectItemCaseSensitive(sp, "position");
            if (!cJSON_IsNumber(idxj) || !cJSON_IsArray(posj) || cJSON_GetArraySize(posj) != 3) continue;
            int idx = idxj->valueint;
            if (idx < 0 || idx >= NSPK) continue;
            cJSON* px = cJSON_GetArrayItem(posj, 0);
            cJSON* py = cJSON_GetArrayItem(posj, 1);
            cJSON* pz = cJSON_GetArrayItem(posj, 2);
            if (!cJSON_IsNumber(px) || !cJSON_IsNumber(py) || !cJSON_IsNumber(pz)) continue;  /* skip non-numeric */
            spk[idx].pos.x = (float)px->valuedouble;
            spk[idx].pos.y = (float)py->valuedouble;
            spk[idx].pos.z = (float)pz->valuedouble;
            cJSON* g = cJSON_GetObjectItemCaseSensitive(sp, "gain_db");
            spk[idx].gain_db = cJSON_IsNumber(g) ? (float)g->valuedouble : 0.0f;
            ++loaded;
        }
    }
    cJSON* dbap = cJSON_GetObjectItemCaseSensitive(root, "dbap");
    if (cJSON_IsObject(dbap)) {
        cJSON* r = cJSON_GetObjectItemCaseSensitive(dbap, "rolloff_r");
        if (cJSON_IsNumber(r)) dbap_r = (float)r->valuedouble;
        cJSON* da = cJSON_GetObjectItemCaseSensitive(dbap, "distance_attenuation");
        if (cJSON_IsObject(da)) {
            cJSON* rd = cJSON_GetObjectItemCaseSensitive(da, "reference_distance_m");
            cJSON* ro = cJSON_GetObjectItemCaseSensitive(da, "rolloff");
            cJSON* mg = cJSON_GetObjectItemCaseSensitive(da, "min_gain_db");
            if (cJSON_IsNumber(rd)) dist_ref     = (float)rd->valuedouble;
            if (cJSON_IsNumber(ro)) dist_rolloff = (float)ro->valuedouble;
            if (cJSON_IsNumber(mg)) dist_min_db  = (float)mg->valuedouble;
        }
    }
    cJSON_Delete(root);
    return loaded;
}

/* write the full schema; delay_ms is derived from positions (max-distance alignment) */
static int save_json(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    float dmax = 0.0f;
    for (int i = 0; i < NSPK; ++i) { float d = Vector3Length(spk[i].pos); if (d > dmax) dmax = d; }
    fprintf(f,
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"units\": { \"position\": \"meters\", \"gain\": \"decibels\", \"delay\": \"milliseconds\" },\n"
        "  \"coordinate_space\": \"room, right-handed (matches OptiTrack/Motive); origin at working-area center\",\n"
        "  \"reference\": { \"alignment\": \"max-distance\", \"speed_of_sound_mps\": %g, \"note\": \"delay_ms time-aligns each speaker arrival to the farthest speaker; gain_db is a measured per-speaker trim\" },\n"
        "  \"dbap\": { \"rolloff_r\": %g, \"distance_attenuation\": { \"model\": \"%s\", \"reference_distance_m\": %g, \"rolloff\": %g, \"min_gain_db\": %g } },\n"
        "  \"speakers\": [\n",
        (double)SPEED_OF_SOUND, dbap_r, dist_model, dist_ref, dist_rolloff, dist_min_db);
    for (int i = 0; i < NSPK; ++i) {
        float d = Vector3Length(spk[i].pos);
        float delay_ms = (dmax - d) / SPEED_OF_SOUND * 1000.0f;
        fprintf(f, "    { \"index\": %d, \"position\": [%.4f, %.4f, %.4f], \"gain_db\": %.2f, \"delay_ms\": %.3f }%s\n",
                i, spk[i].pos.x, spk[i].pos.y, spk[i].pos.z, spk[i].gain_db, delay_ms, (i < NSPK - 1) ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    return 1;
}

/* ---- placement constraints / barriers (constraints.json: an allowed bounding box + no-go boxes for
 * screens / structure / doorways) ----  A speaker is valid if it is inside con_bounds and outside every
 * no-go box. Used to flag violations, to snap a speaker to the nearest allowed point, and (later) as the
 * optimizer's feasibility projection. */
typedef struct { Vector3 lo, hi; } Box;
#define MAXNOGO 24
static Box con_bounds = { { -3, -3, -3 }, { 3, 3, 3 } };
static Box con_nogo[MAXNOGO];
static int con_nnogo, con_loaded;

static int box_in(Box b, Vector3 p) {
    return p.x >= b.lo.x && p.x <= b.hi.x && p.y >= b.lo.y && p.y <= b.hi.y && p.z >= b.lo.z && p.z <= b.hi.z;
}
static int constraint_ok(Vector3 p) {
    if (!con_loaded) return 1;                            /* no constraints loaded -> all positions allowed */
    if (!box_in(con_bounds, p)) return 0;
    for (int i = 0; i < con_nnogo; ++i) if (box_in(con_nogo[i], p)) return 0;
    return 1;
}
/* move p just outside box b through the nearest face that stays inside con_bounds (so a no-go flush
 * against a bounds wall pushes INTO the room, not out of bounds — else the snap never converges) */
static Vector3 push_out(Vector3 p, Box b) {
    const float eps = 1e-3f;
    Vector3 cand[6] = {
        { b.lo.x - eps, p.y, p.z }, { b.hi.x + eps, p.y, p.z },
        { p.x, b.lo.y - eps, p.z }, { p.x, b.hi.y + eps, p.z },
        { p.x, p.y, b.lo.z - eps }, { p.x, p.y, b.hi.z + eps },
    };
    float d[6] = { p.x - b.lo.x, b.hi.x - p.x, p.y - b.lo.y, b.hi.y - p.y, p.z - b.lo.z, b.hi.z - p.z };
    int best = -1; float bd = 1e30f;
    for (int i = 0; i < 6; ++i) if (box_in(con_bounds, cand[i]) && d[i] < bd) { bd = d[i]; best = i; }
    if (best < 0) for (int i = 0; i < 6; ++i) if (d[i] < bd) { bd = d[i]; best = i; }  /* over-constrained: nearest face */
    return cand[best];
}
static Vector3 constraint_project(Vector3 p) {           /* nearest allowed point: clamp to bounds, push out of no-go */
    if (!con_loaded) return p;
    for (int pass = 0; pass < 4; ++pass) {               /* a few passes settle overlapping boxes */
        p.x = Clamp(p.x, con_bounds.lo.x, con_bounds.hi.x);
        p.y = Clamp(p.y, con_bounds.lo.y, con_bounds.hi.y);
        p.z = Clamp(p.z, con_bounds.lo.z, con_bounds.hi.z);
        for (int i = 0; i < con_nnogo; ++i) if (box_in(con_nogo[i], p)) p = push_out(p, con_nogo[i]);
    }
    p.x = Clamp(p.x, con_bounds.lo.x, con_bounds.hi.x);  /* final clamp: in-bounds even if over-constrained */
    p.y = Clamp(p.y, con_bounds.lo.y, con_bounds.hi.y);
    p.z = Clamp(p.z, con_bounds.lo.z, con_bounds.hi.z);
    return p;
}
static int read_box(cJSON* o, Box* out) {
    cJSON* mn = cJSON_GetObjectItemCaseSensitive(o, "min");
    cJSON* mx = cJSON_GetObjectItemCaseSensitive(o, "max");
    if (!cJSON_IsArray(mn) || cJSON_GetArraySize(mn) != 3 || !cJSON_IsArray(mx) || cJSON_GetArraySize(mx) != 3) return 0;
    float a[3], b[3];
    for (int k = 0; k < 3; ++k) {
        cJSON *ak = cJSON_GetArrayItem(mn, k), *bk = cJSON_GetArrayItem(mx, k);
        if (!cJSON_IsNumber(ak) || !cJSON_IsNumber(bk)) return 0;
        a[k] = (float)ak->valuedouble; b[k] = (float)bk->valuedouble;
    }
    out->lo = (Vector3){ fminf(a[0],b[0]), fminf(a[1],b[1]), fminf(a[2],b[2]) };  /* tolerate min/max swapped */
    out->hi = (Vector3){ fmaxf(a[0],b[0]), fmaxf(a[1],b[1]), fmaxf(a[2],b[2]) };
    return 1;
}
static int load_constraints(const char* path) {
    FILE* f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return 0; }
    char* buf = (char*)malloc((size_t)n + 1); if (!buf) { fclose(f); return 0; }
    size_t rd = fread(buf, 1, (size_t)n, f); buf[rd] = 0; fclose(f);
    cJSON* root = cJSON_Parse(buf); free(buf);
    if (!root) return 0;
    cJSON* b = cJSON_GetObjectItemCaseSensitive(root, "bounds");
    if (cJSON_IsObject(b)) read_box(b, &con_bounds);
    con_nnogo = 0;
    cJSON* ng = cJSON_GetObjectItemCaseSensitive(root, "nogo");
    if (cJSON_IsArray(ng)) {
        cJSON* box;
        cJSON_ArrayForEach(box, ng) if (con_nnogo < MAXNOGO && read_box(box, &con_nogo[con_nnogo])) ++con_nnogo;
    }
    cJSON_Delete(root);
    con_loaded = 1;
    return 1;
}

/* ---- engine + DBAP preview state (file scope) ---- */
#define PREV_WAV    "._bw_layout_preview.wav"
#define TEMP_LAYOUT "._bw_layout_preview.json"
#define SRC_GAIN    0.7f
static BwEngine*   e;
static int         audio;
static const char* backend = "none";
static BwSound     pv_sound;
static BwSource    pv_src;
static int         preview, pv_orbit, layout_dirty = 1;   /* dirty=1 forces the first preview to rebuild from spk[] */
static int         pv_panner;                             /* 0=DBAP 1=SPCAP 2=VBAP (= BwPanner; live A/B, B key) */
static const char* panner_names[] = { "DBAP (moving)", "SPCAP (fixed)", "VBAP (fixed)" };
static float       pv_t;
static Vector3     src_pos = { 1.5f, 0.0f, 0.0f };

/* ---- coverage overlay: angular gap to the nearest speaker, over a shell of source directions ----
 * A geometric proxy for DBAP localization: a direction with no nearby speaker forces the pan to
 * spread energy to far-off-axis speakers (a hole). Shades each direction green (covered) -> red (gap).
 * coverage_moving toggles the observer model: fixed = the centre sweet spot; moving = mean over a
 * working-volume grid of listener positions (this installation's case — see docs/spatialization.md). */
#define NCOV   380
#define COV_R  3.0f
static int         coverage_on, coverage_moving = 1;   /* default to the moving-observer worst case */
static Vector3     cov_dir[NCOV];                      /* even (Fibonacci) source directions */
static Vector3     cov_lis[27];                        /* [0]=origin (fixed); [0..26]=3x3x3 working-volume grid */
/* coverage overlay metric: 0 = nearest-speaker angular gap (geometric); 1 = the selected panner's
 * per-direction rE-localization error (cached + recomputed on a throttle, since it runs the real solve). */
static int         cov_metric, cov_frame;
static float       cov_err[NCOV];                      /* per-direction rE error (deg) for cov_err_panner */
static int         cov_err_valid, cov_err_stale, cov_err_panner = -1, cov_err_moving = -1, cov_err_frame;

/* a ~2 s mono 16-bit pink-noise loop for the preview source (broadband -> localises well) */
static void gen_pink_wav(const char* p) {
    uint32_t n = SR * 2;
    short* pcm = (short*)malloc((size_t)n * sizeof(short));
    if (!pcm) return;
    unsigned int s = 22222u; float b[7] = { 0 };
    for (uint32_t i = 0; i < n; ++i) {
        s = s * 1664525u + 1013904223u;
        float w = (float)((int)(s >> 9) - (1 << 22)) / (float)(1 << 22);
        b[0]=0.99886f*b[0]+w*0.0555179f; b[1]=0.99332f*b[1]+w*0.0750759f; b[2]=0.96900f*b[2]+w*0.1538520f;
        b[3]=0.86650f*b[3]+w*0.3104856f; b[4]=0.55000f*b[4]+w*0.5329522f; b[5]=-0.7616f*b[5]-w*0.0168980f;
        float pk = (b[0]+b[1]+b[2]+b[3]+b[4]+b[5]+b[6]+w*0.5362f) * 0.11f * 1.5f;
        b[6] = w * 0.115926f;
        if (pk > 1) pk = 1; if (pk < -1) pk = -1;
        pcm[i] = (short)(pk * 22000.0f);
    }
    FILE* f = fopen(p, "wb");
    if (f) {
        uint32_t sr = SR, databytes = n * 2u, riff = 36u + databytes, byterate = sr * 2u, fmtlen = 16u;
        uint16_t fmt = 1, ch = 1, bits = 16, align = 2;
        fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVE", 1, 4, f);
        fwrite("fmt ", 1, 4, f); fwrite(&fmtlen, 4, 1, f);
        fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f);
        fwrite(&byterate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
        fwrite("data", 1, 4, f); fwrite(&databytes, 4, 1, f);
        fwrite(pcm, sizeof(short), n, f);
        fclose(f);
    }
    free(pcm);
}

/* (re)create the cave-profile engine + the (muted) preview source. layout_path NULL = default grid;
 * the preview rebuilds with a temp file written from spk[] so DBAP pans through the edited positions. */
static void build_engine(const char* layout_path) {
    BwConfig cfg = {
        .profile = BW_PROFILE_CAVE, .layout_path = layout_path, .hrtf_path = NULL,
        .sample_rate = SR, .block_size = 256, .track_internal = false,
    };
    e = bw_create(&cfg);
    backend = "none"; audio = 0; pv_sound = 0; pv_src = 0;
    if (!e) return;
    if (bw_start(e) != 0) {
        const char* err = bw_last_error(e);
        printf("bw_start: %s — no audition (need a 26-ch ASIO device / DVS); the editor still runs.\n",
               err ? err : "?");
    }
    backend = bw_audio_backend(e);
    audio = (strncmp(backend, "asio", 4) == 0);
    pv_sound = bw_load_sound(e, PREV_WAV);
    pv_src   = bw_source_create(e);
    bw_source_play(e, pv_src, pv_sound, true);
    bw_source_set_gain(e, pv_src, 0.0f);          /* silent until preview mode */
}

/* ---- panner-specific layout scoring (offline, via the engine's real solve) ---- */
static float score_mean[3], score_worst[3];      /* [DBAP, SPCAP, VBAP] rE localization error (deg) */
static int   scored, score_stale;

/* mean + worst rE localization error (deg) over the shell, from the panner's observer model: DBAP over
 * the moving listener grid; SPCAP/VBAP from the fixed centre. Uses bw_panner_gains_batch (the ACTUAL
 * engine solve), so the score reflects what will ship — not a re-implementation. */
static void score_panner(BwPanner panner, int stride, float* mean_deg, float* worst_deg) {
    static float gains[NCOV * NSPK], srcs[NCOV * 3];
    float pos[NSPK * 3];
    for (int i = 0; i < NSPK; ++i) { pos[i*3] = spk[i].pos.x; pos[i*3+1] = spk[i].pos.y; pos[i*3+2] = spk[i].pos.z; }
    if (stride < 1) stride = 1;                     /* >1 subsamples the direction shell (coarse, for the optimizer) */
    int NL = (panner == BW_PAN_DBAP) ? 27 : 1;     /* DBAP: moving grid; SPCAP/VBAP: fixed centre */
    double sumerr = 0; float worst = 0; int cnt = 0;
    for (int l = 0; l < NL; ++l) {
        Vector3 Lp = cov_lis[l]; float lisf[3] = { Lp.x, Lp.y, Lp.z };
        int ns = 0;
        for (int i = 0; i < NCOV; i += stride) {
            srcs[ns*3]   = Lp.x + COV_R * cov_dir[i].x;
            srcs[ns*3+1] = Lp.y + COV_R * cov_dir[i].y;
            srcs[ns*3+2] = Lp.z + COV_R * cov_dir[i].z;
            ++ns;
        }
        bw_panner_gains_batch(panner, pos, NSPK, lisf, srcs, ns, gains);
        for (int j = 0; j < ns; ++j) {
            int i = j * stride;                     /* the cov_dir index this sample came from */
            float* g = &gains[j * NSPK];
            float rE[3] = { 0, 0, 0 };
            for (int s = 0; s < NSPK; ++s) {        /* energy-weighted speaker-direction vector (rE) */
                float w = g[s] * g[s];
                Vector3 sd = Vector3Normalize(Vector3Subtract(spk[s].pos, Lp));
                rE[0] += w * sd.x; rE[1] += w * sd.y; rE[2] += w * sd.z;
            }
            float rl = sqrtf(rE[0]*rE[0] + rE[1]*rE[1] + rE[2]*rE[2]);
            if (rl < 1e-9f) continue;
            float c = (rE[0]*cov_dir[i].x + rE[1]*cov_dir[i].y + rE[2]*cov_dir[i].z) / rl;  /* vs intended dir */
            if (c > 1.f) c = 1.f; else if (c < -1.f) c = -1.f;
            float err = acosf(c) * 57.2958f;
            sumerr += err; if (err > worst) worst = err; ++cnt;
        }
    }
    *mean_deg = cnt ? (float)(sumerr / cnt) : 0.f;
    *worst_deg = worst;
}

/* fill cov_err[] with the selected panner's per-direction rE error (deg), averaged over the observer
 * model score_panner uses (DBAP: the moving grid when coverage_moving; SPCAP/VBAP: the fixed centre).
 * Same real solve as the X-score — this is its per-direction breakdown, for the overlay. */
static void compute_cov_err(BwPanner panner) {
    static float gains[NCOV * NSPK], srcs[NCOV * 3];
    float pos[NSPK * 3];
    for (int i = 0; i < NSPK; ++i) { pos[i*3]=spk[i].pos.x; pos[i*3+1]=spk[i].pos.y; pos[i*3+2]=spk[i].pos.z; }
    int NL = (panner == BW_PAN_DBAP && coverage_moving) ? 27 : 1;
    for (int i = 0; i < NCOV; ++i) cov_err[i] = 0.0f;
    for (int l = 0; l < NL; ++l) {
        Vector3 Lp = cov_lis[l]; float lisf[3] = { Lp.x, Lp.y, Lp.z };
        for (int i = 0; i < NCOV; ++i) {
            srcs[i*3]=Lp.x+COV_R*cov_dir[i].x; srcs[i*3+1]=Lp.y+COV_R*cov_dir[i].y; srcs[i*3+2]=Lp.z+COV_R*cov_dir[i].z;
        }
        bw_panner_gains_batch(panner, pos, NSPK, lisf, srcs, NCOV, gains);
        for (int i = 0; i < NCOV; ++i) {
            float* g = &gains[i * NSPK];
            float rE[3] = { 0, 0, 0 };
            for (int s = 0; s < NSPK; ++s) {
                float w = g[s] * g[s];
                Vector3 sd = Vector3Normalize(Vector3Subtract(spk[s].pos, Lp));
                rE[0] += w*sd.x; rE[1] += w*sd.y; rE[2] += w*sd.z;
            }
            float rl = sqrtf(rE[0]*rE[0] + rE[1]*rE[1] + rE[2]*rE[2]);
            float err = 90.0f;                           /* no energy vector -> count as fully wrong */
            if (rl >= 1e-9f) {
                float c = (rE[0]*cov_dir[i].x + rE[1]*cov_dir[i].y + rE[2]*cov_dir[i].z) / rl;
                if (c > 1.f) c = 1.f; else if (c < -1.f) c = -1.f;
                err = acosf(c) * 57.2958f;
            }
            cov_err[i] += err;
        }
    }
    for (int i = 0; i < NCOV; ++i) cov_err[i] /= (float)NL;
    cov_err_valid = 1; cov_err_stale = 0; cov_err_panner = panner; cov_err_moving = coverage_moving; cov_err_frame = cov_frame;
}

/* ---- auto-optimizer: stochastic hill-climb over the free positions, minimising the panner cost
 * (mean + 0.5*worst rE error) subject to the constraints. Runs incrementally (a few trials per frame)
 * so the layout is seen converging and the GUI stays responsive; stop any time and save. ---- */
static int   opt_running, opt_iter, opt_stall;
static float opt_step = 0.30f, opt_cost;

static float opt_cost_of(BwPanner p) { float m, w; score_panner(p, 4, &m, &w); return m + 0.5f * w; }  /* coarse */
static float frand(void) { return (float)rand() / ((float)RAND_MAX + 1.0f); }

static void optimize_step(BwPanner p, int trials) {
    for (int t = 0; t < trials; ++t) {
        int s = rand() % NSPK;
        Vector3 old = spk[s].pos;
        Vector3 cand = { old.x + opt_step * (2*frand()-1), old.y + opt_step * (2*frand()-1), old.z + opt_step * (2*frand()-1) };
        spk[s].pos = constraint_project(cand);     /* keep the trial feasible */
        float c = opt_cost_of(p);
        if (c < opt_cost - 1e-4f) { opt_cost = c; opt_stall = 0; }              /* accept an improvement */
        else { spk[s].pos = old; if (++opt_stall > 6*NSPK) { opt_step *= 0.7f; opt_stall = 0; } }  /* revert; shrink when stuck */
        ++opt_iter;
    }
    layout_dirty = 1; score_stale = 1; cov_err_stale = 1;
}

int main(int argc, char** argv) {
    /* headless (no window/audio, scriptable):
     *   --export   [file]            write the layout (default grid, or an existing file with delay_ms recomputed)
     *   --score    [file]            print each panner's rE-localization error for the layout
     *   --optimize [file] [panner]   hill-climb the layout for one panner (dbap|spcap|vbap, default dbap),
     *                                within constraints.json if present, save in place, print before/after */
    int export_only   = (argc > 1 && strcmp(argv[1], "--export")   == 0);
    int score_only    = (argc > 1 && strcmp(argv[1], "--score")    == 0);
    int optimize_only = (argc > 1 && strcmp(argv[1], "--optimize") == 0);
    const char* path = (export_only || score_only || optimize_only) ? (argc > 2 ? argv[2] : "cave_layout.json")
                                                                     : (argc > 1 ? argv[1] : "cave_layout.json");
    seed_default();
    int loaded = load_json(path);
    if (load_constraints("constraints.json"))
        printf("constraints: bounds + %d no-go box(es) loaded from constraints.json\n", con_nnogo);

    /* coverage/scoring shell: even directions on a sphere (Fibonacci) + a working-volume listener grid */
    for (int i = 0; i < NCOV; ++i) {
        float y = 1.0f - 2.0f * ((float)i + 0.5f) / NCOV;
        float r = sqrtf(1.0f - y * y), th = (float)i * 2.39996323f;   /* golden angle */
        cov_dir[i] = (Vector3){ r * cosf(th), y, r * sinf(th) };
    }
    cov_lis[0] = (Vector3){ 0, 0, 0 };
    { const float ax[3] = { -1.0f, 0.0f, 1.0f }, ay[3] = { -0.3f, 0.0f, 0.3f }; int li = 1;   /* listener-movement envelope */
      for (int xi = 0; xi < 3; ++xi) for (int zi = 0; zi < 3; ++zi) for (int yi = 0; yi < 3; ++yi)
          if (!(ax[xi] == 0 && ay[yi] == 0 && ax[zi] == 0)) cov_lis[li++] = (Vector3){ ax[xi], ay[yi], ax[zi] }; }

    if (export_only) {
        if (!save_json(path)) { printf("export failed: %s\n", path); return 1; }
        printf("exported layout -> %s (from %s)\n", path, loaded ? "existing file" : "default grid");
        return 0;
    }
    if (score_only) {                              /* headless: score the layout for each panner + exit */
        printf("layout: %s (%s)\n", path, loaded ? "loaded" : "default grid");
        for (int p = 0; p < 3; ++p) {
            float m, w; score_panner((BwPanner)p, 1, &m, &w);
            printf("  %-14s rE-localize error:  mean %4.1f deg   worst %4.1f deg\n", panner_names[p], m, w);
        }
        return 0;
    }
    if (optimize_only) {                           /* headless: optimize in place for one panner + save */
        BwPanner p = BW_PAN_DBAP;
        if (argc > 3) { if (!strcmp(argv[3], "spcap")) p = BW_PAN_SPCAP; else if (!strcmp(argv[3], "vbap")) p = BW_PAN_VBAP; }
        float m0, w0; score_panner(p, 1, &m0, &w0);
        opt_cost = opt_cost_of(p); opt_step = 0.30f; opt_stall = 0; opt_iter = 0;
        while (opt_step > 0.02f && opt_iter < 120000) optimize_step(p, 200);   /* run to convergence (step floor) */
        float m1, w1; score_panner(p, 1, &m1, &w1);
        if (!save_json(path)) { printf("optimize: save failed: %s\n", path); return 1; }
        printf("optimized %s for %-5s:  rE mean %.1f -> %.1f deg   worst %.1f -> %.1f deg   (%d iters%s)\n",
               path, panner_names[p], m0, m1, w0, w1, opt_iter, con_loaded ? ", within constraints" : "");
        return 0;
    }
    printf("layout: %s (%s, %d speakers)\n", path, loaded ? "loaded" : "default grid", loaded ? loaded : NSPK);

    /* cave profile so the test signal / DBAP preview goes out the 26-ch DVS to the real speakers;
     * falls back to no audio (editor still works) if no 26-ch ASIO device is present (off-site). */
    _putenv("BWAUDIO_SINK=asio");
    gen_pink_wav(PREV_WAV);                        /* the moving DBAP-preview source signal */
    build_engine(NULL);                           /* edit-mode engine (the test signal is layout-independent) */

    InitWindow(1040, 720, "bwaudio - speaker layout tool");
    SetTargetFPS(60);
    Camera3D cam = { .target = { 0, 0, 0 }, .up = { 0, 1, 0 }, .fovy = 55, .projection = CAMERA_PERSPECTIVE };
    float cam_yaw = 45.0f * DEG2RAD, cam_pitch = 30.0f * DEG2RAD, cam_dist = 9.0f;

    int   sel = 0, tone_on = 0, tone_kind = BW_TEST_SINE, driven = -1;
    int   editing = 0, ilen = 0;
    char  ibuf[64] = { 0 };
    float save_flash = 0.0f;

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();

        if (editing) {                                   /* typing exact "x y z" for the selected speaker */
            int c;
            while ((c = GetCharPressed()) != 0)
                if (ilen < 63 && ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == ' ' || c == 'e'))
                    { ibuf[ilen++] = (char)c; ibuf[ilen] = 0; }
            if (IsKeyPressed(KEY_BACKSPACE) && ilen > 0) ibuf[--ilen] = 0;
            if (IsKeyPressed(KEY_ENTER)) {
                float x, y, z;
                if (sscanf(ibuf, "%f %f %f", &x, &y, &z) == 3) { spk[sel].pos = (Vector3){ x, y, z }; layout_dirty = 1; score_stale = 1; cov_err_stale = 1; }
                editing = 0; ilen = 0; ibuf[0] = 0;
            }
            if (IsKeyPressed(KEY_ESCAPE)) { editing = 0; ilen = 0; ibuf[0] = 0; }
        } else if (preview) {                            /* DBAP preview: move a source, hear it pan */
            if (IsKeyPressed(KEY_P)) {                   /* back to edit */
                preview = 0;
                if (e) { bw_source_set_gain(e, pv_src, 0.0f); bw_commit(e); }
            } else {
                if (IsKeyPressed(KEY_SPACE)) pv_orbit = !pv_orbit;
                if (IsKeyPressed(KEY_B)) {               /* live A/B: DBAP <-> SPCAP (atomic, safe while running) */
                    pv_panner = (pv_panner + 1) % 3;
                    if (e) bw_set_panner(e, (BwPanner)pv_panner);
                }
                float mv = ((IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) ? 0.4f : 1.5f) * dt;
                if (pv_orbit) {                          /* hands-free orbit + near/far + high/low sweep */
                    pv_t += dt;
                    float az = 0.6f * pv_t, r = 1.6f + 1.0f * sinf(0.8f * pv_t), yy = 1.0f * sinf(1.1f * pv_t);
                    src_pos = (Vector3){ r * cosf(az), yy, r * sinf(az) };
                } else {
                    if (IsKeyDown(KEY_W)) src_pos.z -= mv;
                    if (IsKeyDown(KEY_S)) src_pos.z += mv;
                    if (IsKeyDown(KEY_A)) src_pos.x -= mv;
                    if (IsKeyDown(KEY_D)) src_pos.x += mv;
                    if (IsKeyDown(KEY_R)) src_pos.y += mv;
                    if (IsKeyDown(KEY_F)) src_pos.y -= mv;
                }
                if (audio) {
                    bw_source_set_pos(e, pv_src, src_pos.x, src_pos.y, src_pos.z);
                    bw_set_listener_pose(e, 0, 0, 0, 0, 0, 0, 1);   /* listener at origin; walk the room to test off-center */
                    bw_commit(e);
                }
            }
        } else {
            if (IsKeyPressed(KEY_RIGHT_BRACKET)) sel = (sel + 1) % NSPK;
            if (IsKeyPressed(KEY_LEFT_BRACKET))  sel = (sel + NSPK - 1) % NSPK;
            float d = ((IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) ? 0.25f : 1.0f) * dt;
            if (IsKeyDown(KEY_LEFT))  spk[sel].pos.x -= d;
            if (IsKeyDown(KEY_RIGHT)) spk[sel].pos.x += d;
            if (IsKeyDown(KEY_UP))    spk[sel].pos.z -= d;
            if (IsKeyDown(KEY_DOWN))  spk[sel].pos.z += d;
            if (IsKeyDown(KEY_R))     spk[sel].pos.y += d;
            if (IsKeyDown(KEY_F))     spk[sel].pos.y -= d;
            if (IsKeyDown(KEY_PAGE_UP))   spk[sel].gain_db += 6.0f * dt;
            if (IsKeyDown(KEY_PAGE_DOWN)) spk[sel].gain_db -= 6.0f * dt;
            if (IsKeyPressed(KEY_T)) tone_on = !tone_on;
            if (IsKeyPressed(KEY_N)) tone_kind = (tone_kind == BW_TEST_SINE) ? BW_TEST_NOISE : BW_TEST_SINE;
            if (IsKeyPressed(KEY_ENTER)) { editing = 1; ilen = 0; ibuf[0] = 0; }
            if (IsKeyPressed(KEY_S)) { save_flash = save_json(path) ? 2.0f : -2.0f; }
            if (IsKeyPressed(KEY_L)) { load_json(path); load_constraints("constraints.json"); layout_dirty = 1; score_stale = 1; cov_err_stale = 1; }
            if (IsKeyPressed(KEY_K)) {                                 /* snap all speakers to the nearest allowed point */
                for (int i = 0; i < NSPK; ++i) spk[i].pos = constraint_project(spk[i].pos);
                layout_dirty = 1; score_stale = 1; cov_err_stale = 1;
            }
            if (IsKeyPressed(KEY_C)) coverage_on = !coverage_on;       /* coverage overlay */
            if (IsKeyPressed(KEY_V)) coverage_moving = !coverage_moving;
            if (IsKeyPressed(KEY_G)) cov_metric ^= 1;   /* shade: gap <-> selected-panner rE error (cache stays valid) */
            if (IsKeyPressed(KEY_X)) {                                 /* score the layout for each panner */
                for (int p = 0; p < 3; ++p) score_panner((BwPanner)p, 1, &score_mean[p], &score_worst[p]);
                scored = 1; score_stale = 0;
            }
            if (IsKeyPressed(KEY_B)) pv_panner = (pv_panner + 1) % 3;   /* select the target panner (score/optimize) */
            if (IsKeyPressed(KEY_O)) {                                 /* toggle the auto-optimizer for that panner */
                opt_running = !opt_running;
                if (opt_running) { opt_cost = opt_cost_of((BwPanner)pv_panner); opt_step = 0.30f; opt_stall = 0; opt_iter = 0; }
            }
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {  /* click-pick the nearest speaker (not while orbiting) */
                Ray ray = GetMouseRay(GetMousePosition(), cam);
                float best = 1e9f; int hit = -1;
                for (int i = 0; i < NSPK; ++i) {
                    RayCollision rc = GetRayCollisionSphere(ray, spk[i].pos, 0.16f);
                    if (rc.hit && rc.distance < best) { best = rc.distance; hit = i; }
                }
                if (hit >= 0) sel = hit;
            }
            if (IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_UP) || IsKeyDown(KEY_DOWN) ||
                IsKeyDown(KEY_R) || IsKeyDown(KEY_F) || IsKeyDown(KEY_PAGE_UP) || IsKeyDown(KEY_PAGE_DOWN)) { layout_dirty = 1; score_stale = 1; cov_err_stale = 1; }
            if (IsKeyPressed(KEY_P) && !editing) {        /* enter DBAP preview — rebuild so it pans through the edited layout */
                if (driven >= 0 && e) { bw_test_signal(e, (uint32_t)driven, BW_TEST_OFF, 0.0f); driven = -1; }
                tone_on = 0; pv_orbit = 0; pv_t = 0.0f;   /* each preview session starts manual, fresh orbit phase */
                if (layout_dirty && e && audio) {     /* rebuild only when there's a device to hear it on */
                    save_json(TEMP_LAYOUT);
                    bw_stop(e); bw_destroy(e);
                    build_engine(TEMP_LAYOUT);
                    layout_dirty = 0; driven = -1;
                }
                opt_running = 0;                          /* stop the optimizer when leaving edit for preview */
                preview = 1;
                if (e) {
                    bw_set_panner(e, (BwPanner)pv_panner);                      /* rebuilt engine defaults to DBAP */
                    bw_source_set_gain(e, pv_src, SRC_GAIN);
                    bw_commit(e);
                }
            }
        }

        if (opt_running && !editing && !preview) {        /* auto-optimizer: a few hill-climb trials per frame */
            opt_cost = opt_cost_of((BwPanner)pv_panner);  /* re-baseline so a manual nudge can't wedge the climb */
            optimize_step((BwPanner)pv_panner, 6);
        }

        /* drive the selected speaker's channel when auditioning (edit mode only) */
        if (audio && !preview) {
            if (tone_on) {
                if (driven >= 0 && driven != sel) bw_test_signal(e, (uint32_t)driven, BW_TEST_OFF, 0.0f);
                bw_test_signal(e, (uint32_t)sel, (BwTestKind)tone_kind, TEST_GAIN);
                driven = sel;
            } else if (driven >= 0) {
                bw_test_signal(e, (uint32_t)driven, BW_TEST_OFF, 0.0f);
                driven = -1;
            }
        }

        if (save_flash > 0) save_flash -= dt;            /* "saved" toast fades out */
        else if (save_flash < 0) save_flash += dt;       /* "save failed" toast fades out */

        /* arcball camera */
        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
            Vector2 md = GetMouseDelta();
            cam_yaw   -= md.x * 0.005f;
            cam_pitch += md.y * 0.005f;
            if (cam_pitch >  1.5f) cam_pitch =  1.5f;
            if (cam_pitch < -1.5f) cam_pitch = -1.5f;
        }
        cam_dist -= GetMouseWheelMove() * 0.6f;
        if (cam_dist < 2.0f)  cam_dist = 2.0f;
        if (cam_dist > 30.0f) cam_dist = 30.0f;
        cam.position.x = cam_dist * cosf(cam_pitch) * sinf(cam_yaw);
        cam.position.y = cam_dist * sinf(cam_pitch);
        cam.position.z = cam_dist * cosf(cam_pitch) * cosf(cam_yaw);

        /* max distance for the live delay readout */
        float dmax = 0.0f;
        for (int i = 0; i < NSPK; ++i) { float dd = Vector3Length(spk[i].pos); if (dd > dmax) dmax = dd; }
        float seld   = Vector3Length(spk[sel].pos);
        float seldel = (dmax - seld) / SPEED_OF_SOUND * 1000.0f;
        float cov_worst = 0.0f, cov_mean = 0.0f;        /* coverage summary, filled by the overlay below */
        int con_bad = 0;
        if (con_loaded) for (int i = 0; i < NSPK; ++i) if (!constraint_ok(spk[i].pos)) ++con_bad;
        ++cov_frame;

        BeginDrawing();
        ClearBackground((Color){ 22, 22, 28, 255 });
        BeginMode3D(cam);
        DrawGrid(16, 0.5f);
        if (con_loaded) {                                /* placement constraints: allowed bounds (green) + no-go (red) */
            DrawCubeWiresV(Vector3Scale(Vector3Add(con_bounds.lo, con_bounds.hi), 0.5f),
                           Vector3Subtract(con_bounds.hi, con_bounds.lo), (Color){ 90, 200, 120, 110 });
            for (int i = 0; i < con_nnogo; ++i)
                DrawCubeWiresV(Vector3Scale(Vector3Add(con_nogo[i].lo, con_nogo[i].hi), 0.5f),
                               Vector3Subtract(con_nogo[i].hi, con_nogo[i].lo), (Color){ 235, 90, 90, 170 });
        }
        DrawLine3D((Vector3){ 0, 0, 0 }, (Vector3){ 1.2f, 0, 0 }, (Color){ 230, 90, 90, 255 });   /* +X */
        DrawLine3D((Vector3){ 0, 0, 0 }, (Vector3){ 0, 1.2f, 0 }, (Color){ 90, 230, 90, 255 });   /* +Y */
        DrawLine3D((Vector3){ 0, 0, 0 }, (Vector3){ 0, 0, 1.2f }, (Color){ 90, 150, 230, 255 });  /* +Z */
        DrawSphere((Vector3){ 0, 0, 0 }, 0.08f, (Color){ 200, 200, 210, 255 });                   /* listener */
        DrawLine3D((Vector3){ 0, 0, 0 }, spk[sel].pos, (Color){ 240, 220, 120, 140 });
        for (int i = 0; i < NSPK; ++i) {
            int is_sel = (i == sel), is_drv = (audio && tone_on && i == sel);
            Color col = is_drv ? (Color){ 120, 245, 140, 255 }
                      : is_sel ? (Color){ 245, 220, 90, 255 }
                               : (Color){ 120, 120, 150, 255 };
            DrawSphere(spk[i].pos, is_sel ? 0.15f : 0.10f, col);
            if (!constraint_ok(spk[i].pos))              /* flag a speaker outside bounds / inside a no-go box */
                DrawSphereWires(spk[i].pos, is_sel ? 0.20f : 0.16f, 6, 6, (Color){ 245, 80, 80, 255 });
        }
        if (preview) {                                   /* the moving DBAP source */
            DrawLine3D((Vector3){ 0, 0, 0 }, src_pos, (Color){ 90, 220, 90, 200 });
            DrawSphere(src_pos, 0.16f, (Color){ 240, 120, 90, 255 });
        }
        if (coverage_on && !preview && cov_metric == 0) {  /* shade each direction by its nearest-speaker gap */
            int NL = coverage_moving ? 27 : 1;
            Vector3 sdir[27][26];                         /* speaker directions from each listener sample */
            for (int l = 0; l < NL; ++l)
                for (int i = 0; i < NSPK; ++i)
                    sdir[l][i] = Vector3Normalize(Vector3Subtract(spk[i].pos, cov_lis[l]));
            float worst = 1.0f; double macc = 0.0;       /* worst = min score (largest gap) */
            for (int s = 0; s < NCOV; ++s) {
                Vector3 d = cov_dir[s];                   /* a source DIRECTION, queried from each listener */
                float acc = 0.0f;                        /* mean over listeners of (max over speakers of alignment) */
                for (int l = 0; l < NL; ++l) {           /* from listener L, how near is the best speaker to direction d? */
                    float best = -1.0f;
                    for (int i = 0; i < NSPK; ++i) { float dp = Vector3DotProduct(d, sdir[l][i]); if (dp > best) best = dp; }
                    acc += best;
                }
                float score = acc / (float)NL;            /* typical coverage of d over the roam (mean, not worst corner) */
                float t = (score - 0.5f) / 0.366f; if (t < 0) t = 0; if (t > 1) t = 1;   /* >=60 deg gap red, <=30 green */
                DrawCubeV(Vector3Scale(d, COV_R), (Vector3){ 0.09f, 0.09f, 0.09f },
                          (Color){ (unsigned char)(230*(1-t)+50*t), (unsigned char)(60*(1-t)+225*t), 75, 205 });
                if (score < worst) worst = score;
                float a = score < -1 ? -1 : (score > 1 ? 1 : score);
                macc += acosf(a);
            }
            float w = worst < -1 ? -1 : (worst > 1 ? 1 : worst);
            cov_worst = acosf(w) * 57.2958f;
            cov_mean  = (float)(macc / NCOV) * 57.2958f;
        } else if (coverage_on && !preview && cov_metric == 1) {  /* shade by the selected panner's per-dir rE error */
            /* recompute a structural change (panner/observer/first-entry) immediately so the cubes and the
             * HUD label never disagree; throttle only the layout-edit churn (which fires every frame mid-optimize) */
            int structural = (!cov_err_valid || cov_err_panner != pv_panner || cov_err_moving != coverage_moving);
            if (structural || (cov_err_stale && cov_frame - cov_err_frame >= 6))
                compute_cov_err((BwPanner)pv_panner);
            double macc = 0.0;
            for (int s = 0; s < NCOV; ++s) {
                float err = cov_err[s];                   /* deg; 0 = exact, >=40 fully red */
                float t = err / 40.0f; if (t < 0) t = 0; if (t > 1) t = 1;
                DrawCubeV(Vector3Scale(cov_dir[s], COV_R), (Vector3){ 0.09f, 0.09f, 0.09f },
                          (Color){ (unsigned char)(230*t+50*(1-t)), (unsigned char)(225*(1-t)+60*t), 75, 205 });
                if (err > cov_worst) cov_worst = err;
                macc += err;
            }
            cov_mean = (float)(macc / NCOV);
        }
        EndMode3D();

        /* 2D index labels projected from 3D */
        for (int i = 0; i < NSPK; ++i) {
            Vector2 s = GetWorldToScreen(spk[i].pos, cam);
            DrawText(TextFormat("%d", i), (int)s.x + 6, (int)s.y - 6, i == sel ? 18 : 12,
                     i == sel ? (Color){ 250, 230, 120, 255 } : (Color){ 150, 150, 170, 220 });
        }

        /* HUD */
        DrawRectangle(0, 0, GetScreenWidth(), 96, (Color){ 0, 0, 0, 200 });
        if (preview) {
            DrawText(TextFormat("PREVIEW   WASD/RF move   SPACE auto-orbit   B panner: %s   P back to edit",
                                panner_names[pv_panner]),
                     10, 8, 13, RAYWHITE);
            DrawText(TextFormat("source (%.2f, %.2f, %.2f)   orbit %s   - A/B the panner by ear; walk the room for off-center coverage",
                                src_pos.x, src_pos.y, src_pos.z, pv_orbit ? "ON" : "off"),
                     10, 30, 16, (Color){ 240, 160, 120, 255 });
        } else {
            DrawText("[ ] select   arrows X/Z  R/F Y (SHIFT)   ENTER type   PgUp/Dn gain   T tone  N noise   C coverage  V obs  G gap/err   X score   K snap   P preview   S save  L reload",
                     10, 8, 13, RAYWHITE);
            DrawText(TextFormat("speaker %d / %d  ->  channel %d   pos (%.3f, %.3f, %.3f)   gain %+.1f dB   delay %.3f ms   dist %.3f m",
                                sel, NSPK, sel, spk[sel].pos.x, spk[sel].pos.y, spk[sel].pos.z, spk[sel].gain_db, seldel, seld),
                     10, 30, 16, (Color){ 245, 220, 90, 255 });
            if (editing)
                DrawText(TextFormat("type \"x y z\" then ENTER:  %s_", ibuf), 10, 54, 16, (Color){ 120, 245, 140, 255 });
            else
                DrawText(TextFormat("tone [T] %s (%s)   save target: %s",
                                    tone_on ? "ON" : "off", tone_kind == BW_TEST_SINE ? "sine" : "noise", path),
                         10, 54, 15, (Color){ 110, 200, 255, 255 });
        }
        DrawText(audio ? TextFormat("audio: %s  (T drives the selected channel out the array)", backend)
                       : "audio: none - editor only (needs a 26-ch ASIO/DVS device to audition)",
                 10, 76, 14, audio ? (Color){ 110, 235, 130, 255 } : (Color){ 235, 170, 110, 255 });
        if (save_flash > 0)
            DrawText(TextFormat("saved -> %s", path), GetScreenWidth() - 320, 76, 15, (Color){ 120, 245, 140, 255 });
        else if (save_flash < 0)
            DrawText("SAVE FAILED (path not writable?)", GetScreenWidth() - 320, 76, 15, (Color){ 245, 120, 120, 255 });
        if (con_loaded && !preview) {                    /* placement-constraint status */
            DrawRectangle(0, 96, 320, 22, (Color){ 0, 0, 0, 175 });
            DrawText(TextFormat("constraints: %d no-go   %d violating   [K snap to allowed]", con_nnogo, con_bad),
                     10, 100, 14, con_bad ? (Color){ 245, 130, 130, 255 } : (Color){ 120, 220, 140, 255 });
        }
        if (!preview) {                                  /* target panner + auto-optimizer status */
            int yo = con_loaded ? 122 : 100;
            DrawRectangle(0, yo - 4, 520, 22, (Color){ 0, 0, 0, 175 });
            if (opt_running)
                DrawText(TextFormat("OPTIMIZING %s   cost %.1f   iter %d   step %.2f m%s   [O] stop",
                                    panner_names[pv_panner], opt_cost, opt_iter, opt_step, editing ? "   (paused)" : ""),
                         10, yo, 14, (Color){ 120, 245, 160, 255 });
            else
                DrawText(TextFormat("target panner [B]: %s    [O] auto-optimize the layout for it",
                                    panner_names[pv_panner]), 10, yo, 14, (Color){ 180, 200, 240, 255 });
        }
        if (coverage_on && !preview) {                   /* bottom: the coverage summary */
            int yb = GetScreenHeight() - 26;
            const char* obs = coverage_moving ? "moving: mean over working volume" : "fixed: centre sweet spot";
            DrawRectangle(0, yb - 5, GetScreenWidth(), 31, (Color){ 0, 0, 0, 195 });
            if (cov_metric == 0)
                DrawText(TextFormat("nearest-speaker gap [%s]   worst dir %.0f deg   mean %.0f deg   green=covered red=gap   [G] %s rE error",
                                    obs, cov_worst, cov_mean, panner_names[pv_panner]),
                         10, yb, 15, cov_worst > 45.0f ? (Color){ 245, 150, 110, 255 } : (Color){ 150, 225, 160, 255 });
            else
                DrawText(TextFormat("%s rE error [%s]   worst dir %.0f deg   mean %.0f deg   green=accurate red=off   [G] nearest-speaker gap",
                                    panner_names[pv_panner], obs, cov_worst, cov_mean),
                         10, yb, 15, cov_worst > 30.0f ? (Color){ 245, 150, 110, 255 } : (Color){ 150, 225, 160, 255 });
        }
        if (scored && !preview) {                        /* panner-specific rE-localization scores (X) */
            int ys = GetScreenHeight() - (coverage_on ? 52 : 26);
            DrawRectangle(0, ys - 5, GetScreenWidth(), 31, (Color){ 0, 0, 0, 195 });
            DrawText(TextFormat("panner rE-err [X]%s   DBAP %.0f/%.0f   SPCAP %.0f/%.0f   VBAP %.0f/%.0f   deg mean/worst (lower = layout suits it)",
                                score_stale ? " STALE" : "",
                                score_mean[0], score_worst[0], score_mean[1], score_worst[1], score_mean[2], score_worst[2]),
                     10, ys, 15, score_stale ? (Color){ 210, 210, 130, 255 } : (Color){ 150, 200, 240, 255 });
        }
        EndDrawing();
    }

    CloseWindow();
    if (e) { bw_stop(e); bw_destroy(e); }
    remove(PREV_WAV); remove(TEMP_LAYOUT);
    return 0;
}
