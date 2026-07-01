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
 * A raygui control panel (right side, edit mode) mirrors every edit control below — speaker spinner +
 * gain slider, panner combo, DBAP-knob sliders, the audition/view toggles, and the action buttons — so
 * it's click-driven now; the keyboard shortcuts all still work (the panel just sets the same requests).
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
#include "ui_text.h"        /* crisp HUD text; ui_text() supersedes raylib's DrawText() */
#include "cJSON.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAYGUI_IMPLEMENTATION
#include "raygui.h"          /* immediate-mode control panel (header-only) */

#define NSPK           26
#define SR             48000u
#define SPEED_OF_SOUND 343.0f
#define TEST_GAIN      0.4f
#define PANEL_W        300       /* raygui control panel width (right side, edit mode) */

typedef struct { Vector3 pos; float gain_db; } Spk;
static Spk spk[NSPK];

/* dbap knobs (round-tripped through the file; defaults match layout_default) */
static float       dbap_r = 0.5f, dist_ref = 1.0f, dist_rolloff = 1.0f, dist_min_db = -40.0f;
static const char* dist_model = "inverse";
static float       obs_height = 1.4f;      /* observer EAR height above the floor origin (m; ~4.6 ft) — the
                                              listener/scoring point + the line-of-sight source sit here, not at y=0 */
/* Psychophysics: human localization is anisotropic — horizontal (azimuth) acuity is far finer than
 * vertical. The minimum audible angle is ~1 deg for a frontal azimuth displacement (Mills, JASA 1958)
 * vs ~3-4 deg for a vertical one (Perrott & Saberi, JASA 1990); 2-D localization scatter is likewise
 * ~2x larger in elevation than azimuth (Makous & Middlebrooks, JASA 1990), and median-plane blur is
 * coarser still (Blauert, Spatial Hearing, 1997). So azimuth is ~3-4x more resolvable than elevation:
 * split the localization error into azimuth + elevation and DOWN-weight elevation (elev_wt ~ 1/3.5), so
 * the optimizer trades vertical accuracy for horizontal, matching what a listener actually notices.
 * (elev_wt is a modelling choice from that ratio, not a value lifted from any single paper.) */
static int         perceptual = 1;         /* weight azimuth over elevation in the rE error */
static float       elev_wt    = 0.3f;      /* elevation-error weight vs azimuth (~1/3.5; slider 0..1) */

/* the engine's default 3x3x3-minus-centre grid — the starting point and the layout_default() order */
static void seed_default(void) {
    /* 26 speakers on a hemisphere DOME (y >= 0): a Fibonacci half-sphere, radius ~2.4 m — the listener
     * sits under it. Even angular spread with no floor-crossing speakers (the CAVE floor is a screen). */
    const float R = 2.4f, golden = 2.39996323f;   /* golden angle */
    for (int i = 0; i < NSPK; ++i) {
        float y  = ((float)i + 0.5f) / (float)NSPK;          /* 0..1, all >= 0 (upper hemisphere) */
        float r  = sqrtf(1.0f - y * y);
        float th = golden * (float)i;
        spk[i].pos     = (Vector3){ R * r * cosf(th), R * y, R * r * sinf(th) };
        spk[i].gain_db = 0.0f;
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

/* ---- placement constraints / barriers (constraints.json; see examples/constraints.json) ----
 *   bounds      : the allowed box — speakers must be INSIDE it.
 *   nogo[]      : keep-out boxes — speakers must be OUTSIDE them (screens, structure, doorways, the CAVE
 *                 interior). Snappable (K) + the optimizer's feasibility projection.
 *   obstacles[] : SOLID occluders (projectors, beams) — a speaker can't be inside one NOR in its acoustic
 *                 shadow: a box on the segment from the speaker to the ears blocks its sound (los_clear).
 *                 The optimizer penalises shadowed speakers; the tool flags them orange (move them clear).
 * A box is axis-aligned; a projector's throw FRUSTUM is only crudely a box, so size the obstacle to the
 * body + the near shadow you care about. Line-of-sight is to the single observer at (0, obs_height, 0). */
typedef struct { Vector3 lo, hi; } Box;
#define MAXNOGO 24
static Box con_bounds = { { -3, -3, -3 }, { 3, 3, 3 } };
static Box con_nogo[MAXNOGO];
static int con_nnogo, con_loaded;

static int box_in(Box b, Vector3 p) {
    return p.x >= b.lo.x && p.x <= b.hi.x && p.y >= b.lo.y && p.y <= b.hi.y && p.z >= b.lo.z && p.z <= b.hi.z;
}

/* segment [a,b] vs an AABB (slab method): 1 if they intersect anywhere on the segment. */
static int seg_hits_box(Vector3 a, Vector3 b, Box box) {
    float pa[3] = { a.x, a.y, a.z }, pd[3] = { b.x-a.x, b.y-a.y, b.z-a.z };
    float lo[3] = { box.lo.x, box.lo.y, box.lo.z }, hi[3] = { box.hi.x, box.hi.y, box.hi.z };
    float tmin = 0.0f, tmax = 1.0f;
    for (int i = 0; i < 3; ++i) {
        if (fabsf(pd[i]) < 1e-9f) { if (pa[i] < lo[i] || pa[i] > hi[i]) return 0; }   /* parallel + outside the slab */
        else {
            float t1 = (lo[i]-pa[i])/pd[i], t2 = (hi[i]-pa[i])/pd[i];
            if (t1 > t2) { float t = t1; t1 = t2; t2 = t; }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return 0;
        }
    }
    return 1;
}

/* solid OCCLUDERS (projectors / structure): a speaker can't be inside one NOR in its acoustic shadow —
 * a box on the line from the speaker to the ears would block its sound. Loaded from constraints.json. */
static Box con_obst[MAXNOGO];
static int con_nobst;

/* clear line of sight from a speaker at p to the observer's ears (obstacle boxes block it). */
static int los_clear(Vector3 p) {
    Vector3 obs = { 0, obs_height, 0 };
    for (int i = 0; i < con_nobst; ++i) if (seg_hits_box(p, obs, con_obst[i])) return 0;
    return 1;
}

static int constraint_ok(Vector3 p) {                    /* physical: in bounds, out of no-go AND out of solid bodies */
    if (!con_loaded) return 1;                            /* no constraints loaded -> all positions allowed */
    if (!box_in(con_bounds, p)) return 0;
    for (int i = 0; i < con_nnogo; ++i) if (box_in(con_nogo[i], p)) return 0;
    for (int i = 0; i < con_nobst; ++i) if (box_in(con_obst[i], p)) return 0;
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
    if (!con_loaded) { p.y = fmaxf(0.0f, p.y); return p; }   /* y >= 0 is a hard global floor even with no constraints file */
    for (int pass = 0; pass < 4; ++pass) {               /* a few passes settle overlapping boxes */
        p.x = Clamp(p.x, con_bounds.lo.x, con_bounds.hi.x);
        p.y = Clamp(p.y, con_bounds.lo.y, con_bounds.hi.y);
        p.z = Clamp(p.z, con_bounds.lo.z, con_bounds.hi.z);
        for (int i = 0; i < con_nnogo; ++i) if (box_in(con_nogo[i], p)) p = push_out(p, con_nogo[i]);
        for (int i = 0; i < con_nobst; ++i) if (box_in(con_obst[i], p)) p = push_out(p, con_obst[i]);  /* off solid bodies */
    }
    p.x = Clamp(p.x, con_bounds.lo.x, con_bounds.hi.x);  /* final clamp: in-bounds even if over-constrained */
    p.y = Clamp(p.y, con_bounds.lo.y, con_bounds.hi.y);
    p.z = Clamp(p.z, con_bounds.lo.z, con_bounds.hi.z);
    p.y = fmaxf(0.0f, p.y);                              /* ... but never below the floor */
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
    con_nobst = 0;
    cJSON* ob = cJSON_GetObjectItemCaseSensitive(root, "obstacles");   /* solid occluders (projectors/structure) */
    if (cJSON_IsArray(ob)) {
        cJSON* box;
        cJSON_ArrayForEach(box, ob) if (con_nobst < MAXNOGO && read_box(box, &con_obst[con_nobst])) ++con_nobst;
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
static float       cov_val[NCOV];                      /* per-cube value shown on hover (gap deg, or rE err deg) */
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

/* Perceptually-weighted localization error (deg) between intended dir `d` and the actual energy-vector
 * dir `e` (both unit). With `perceptual`, the tangential error is split into an azimuth (horizontal) +
 * elevation (vertical) component and elevation is scaled by elev_wt, so a vertical miss counts less than
 * a horizontal one. Near-vertical dirs fall back to the raw angle (azimuth undefined at the poles). */
static float loc_err_deg(Vector3 d, Vector3 e) {
    float c = Vector3DotProduct(d, e); if (c > 1.f) c = 1.f; else if (c < -1.f) c = -1.f;
    float raw = acosf(c) * 57.2958f;
    if (!perceptual) return raw;
    Vector3 azt = Vector3CrossProduct(d, (Vector3){ 0, 1, 0 });   /* azimuth tangent (horizontal) */
    float azl = Vector3Length(azt);
    if (azl < 0.15f) return raw;                                  /* d ~ vertical: azimuth ill-defined */
    azt = Vector3Scale(azt, 1.0f / azl);
    Vector3 elt = Vector3CrossProduct(azt, d);                    /* elevation tangent (vertical), unit */
    Vector3 perp = Vector3Subtract(e, Vector3Scale(d, c));        /* e's deviation from d, in the tangent plane */
    float ae = Vector3DotProduct(perp, azt), ee = Vector3DotProduct(perp, elt);
    float w = sqrtf(ae*ae + elev_wt*elev_wt*ee*ee);               /* weighted deviation = sin(weighted error) */
    if (w > 1.f) w = 1.f;
    return asinf(w) * 57.2958f;
}

/* ---- panner-specific layout scoring (offline, via the engine's real solve) ---- */
static float score_mean[3], score_worst[3];      /* [DBAP, SPCAP, VBAP] rE localization error (deg) */
static int   scored, score_stale, last_score_frame;   /* the per-panner scoreboard auto-refreshes on a throttle */

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
        Vector3 Lp = cov_lis[l]; Lp.y += obs_height;    /* the listener's EARS are at obs_height, not the floor */
        float lisf[3] = { Lp.x, Lp.y, Lp.z };
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
            Vector3 e = { rE[0]/rl, rE[1]/rl, rE[2]/rl };
            float err = loc_err_deg(cov_dir[i], e);   /* perceptually weighted (azimuth >> elevation) */
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
        Vector3 Lp = cov_lis[l]; Lp.y += obs_height;    /* ears at obs_height */
        float lisf[3] = { Lp.x, Lp.y, Lp.z };
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
                Vector3 e = { rE[0]/rl, rE[1]/rl, rE[2]/rl };
                err = loc_err_deg(cov_dir[i], e);        /* perceptually weighted (azimuth >> elevation) */
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
static int     opt_running, opt_iter, opt_stall;
static float   opt_step = 0.30f, opt_cost;
static float   opt_leash = 3.0f;                          /* max optimizer displacement from the anchor (m); ~free at 3 m */
static Vector3 opt_anchor[NSPK];                          /* speaker positions captured when optimization started */

static float opt_cost_of(BwPanner p) {   /* coarse; + a penalty per speaker in a projector shadow so the climb clears them */
    float m, w; score_panner(p, 4, &m, &w);
    int occ = 0; for (int i = 0; i < NSPK; ++i) if (!los_clear(spk[i].pos)) ++occ;
    return m + 0.5f * w + 25.0f * (float)occ;
}
static float frand(void) { return (float)rand() / ((float)RAND_MAX + 1.0f); }

static void optimize_step(BwPanner p, int trials) {
    for (int t = 0; t < trials; ++t) {
        int s = rand() % NSPK;
        Vector3 old = spk[s].pos;
        Vector3 cand = { old.x + opt_step * (2*frand()-1), old.y + opt_step * (2*frand()-1), old.z + opt_step * (2*frand()-1) };
        Vector3 dv = Vector3Subtract(cand, opt_anchor[s]);       /* leash: never drift past opt_leash from where it started */
        float dl = Vector3Length(dv);
        if (dl > opt_leash) cand = Vector3Add(opt_anchor[s], Vector3Scale(dv, opt_leash / dl));
        spk[s].pos = constraint_project(cand);     /* keep the trial feasible */
        float c = opt_cost_of(p);
        if (c < opt_cost - 1e-4f) { opt_cost = c; opt_stall = 0; }              /* accept an improvement */
        else { spk[s].pos = old; if (++opt_stall > 6*NSPK) { opt_step *= 0.7f; opt_stall = 0; } }  /* revert; shrink when stuck */
        ++opt_iter;
    }
    layout_dirty = 1; score_stale = 1; cov_err_stale = 1;
}

/* green(good)->yellow->red(bad) ramp. t: 1 = green, 0.5 = yellow, 0 = red. */
static Color heat(float t) {
    if (t < 0) t = 0; else if (t > 1) t = 1;
    float r, g, b;
    if (t >= 0.5f) { float u = (t - 0.5f) * 2.0f;   /* yellow -> green */
        r = 235 + (70  - 235) * u; g = 205; b = 60 + (95 - 60) * u; }
    else { float u = t * 2.0f;                       /* red -> yellow */
        r = 235; g = 70 + (205 - 70) * u; b = 68 + (60 - 68) * u; }
    return (Color){ (unsigned char)r, (unsigned char)g, (unsigned char)b, 205 };
}
/* localization error (deg) -> heat, per the desired feel: <=2 great (green), 5-10 fine (a yellow
 * plateau), 10+ bad (ramps to red by 12). */
static Color err_heat(float e) {
    float t;
    if      (e <=  2.0f) t = 1.0f;                              /* great */
    else if (e <=  5.0f) t = 1.0f - 0.5f * (e -  2.0f) / 3.0f;  /* green -> yellow */
    else if (e <= 10.0f) t = 0.5f;                              /* fine: yellow plateau */
    else if (e <= 12.0f) t = 0.5f - 0.5f * (e - 10.0f) / 2.0f;  /* yellow -> red */
    else                 t = 0.0f;                              /* bad */
    return heat(t);
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
    printf("working dir: %s\n", GetWorkingDirectory());   /* cave_layout.json + constraints.json resolve here (CWD) */
    int loaded = load_json(path);
    if (load_constraints("constraints.json"))
        printf("constraints: bounds + %d no-go + %d obstacle box(es) from ./constraints.json\n", con_nnogo, con_nobst);
    else
        printf("constraints: none (no ./constraints.json here) — every placement allowed except the y>=0 floor\n");

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
        for (int i = 0; i < NSPK; ++i) opt_anchor[i] = spk[i].pos;             /* leash anchor (opt_leash defaults ~free) */
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

    SetConfigFlags(FLAG_WINDOW_HIGHDPI | FLAG_MSAA_4X_HINT);   /* native pixel density + smooth 3D edges */
    InitWindow(1040, 720, "bwaudio - speaker layout tool");
    ui_text_init();                                            /* crisp TTF HUD (see ui_text.h) */
    GuiSetFont(g_ui_font);                                     /* raygui draws through the same crisp atlas */
    GuiSetStyle(DEFAULT, TEXT_SIZE, 18);
    GuiSetStyle(DEFAULT, BACKGROUND_COLOR,    0x1c1c24ff);     /* dark theme to match the 3D view */
    GuiSetStyle(DEFAULT, BASE_COLOR_NORMAL,   0x2c2c38ff);
    GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL,   0xc8c8d8ff);
    GuiSetStyle(DEFAULT, BORDER_COLOR_NORMAL, 0x4a4a5aff);
    GuiSetStyle(DEFAULT, BASE_COLOR_FOCUSED,  0x3a3a4aff);
    GuiSetStyle(DEFAULT, TEXT_COLOR_FOCUSED,  0xffffffff);
    GuiSetStyle(DEFAULT, BORDER_COLOR_FOCUSED,0x6a8acaff);
    GuiSetStyle(DEFAULT, BASE_COLOR_PRESSED,  0x46506aff);
    GuiSetStyle(DEFAULT, TEXT_COLOR_PRESSED,  0xffffffff);
    GuiSetStyle(DEFAULT, BORDER_COLOR_PRESSED,0x8aaae0ff);
    GuiSetStyle(DEFAULT, LINE_COLOR,          0x40404eff);
    SetTargetFPS(60);
    Camera3D cam = { .target = { 0, 0, 0 }, .up = { 0, 1, 0 }, .fovy = 55, .projection = CAMERA_PERSPECTIVE };
    float cam_yaw = 45.0f * DEG2RAD, cam_pitch = 30.0f * DEG2RAD, cam_dist = 9.0f;

    int   sel = 0, tone_on = 0, tone_kind = BW_TEST_SINE, driven = -1;
    int   editing = 0, ilen = 0;
    char  ibuf[64] = { 0 };
    float save_flash = 0.0f;
    /* raygui panel -> key-handler bridge: a click sets a request flag the edit branch consumes next
     * frame (the panel draws at the bottom of the loop; the handlers run at the top), so buttons reuse
     * the exact same action code as the keys with no duplication. */
    int   req_save = 0, req_reload = 0, req_score = 0, req_snap = 0, req_preview = 0, req_opt = 0, req_type = 0;

    scored = 1; score_stale = 1;                                 /* per-panner scoreboard on from the start, live-refreshed */
    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        if (IsKeyPressed(KEY_F11)) ToggleBorderlessWindowed();   /* fullscreen (HUD/panel use GetScreenWidth, so they follow) */

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
                    bw_set_listener_pose(e, 0, obs_height, 0, 0, 0, 0, 1);   /* ears at obs_height; walk the room to test off-center */
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
            if (IsKeyPressed(KEY_ENTER) || req_type) { req_type = 0; editing = 1; ilen = 0; ibuf[0] = 0; }
            if (IsKeyPressed(KEY_S) || req_save) { req_save = 0; save_flash = save_json(path) ? 2.0f : -2.0f; }
            if (IsKeyPressed(KEY_L) || req_reload) { req_reload = 0; load_json(path); load_constraints("constraints.json"); layout_dirty = 1; score_stale = 1; cov_err_stale = 1; }
            if (IsKeyPressed(KEY_K) || req_snap) { req_snap = 0;       /* snap all speakers to the nearest allowed point */
                for (int i = 0; i < NSPK; ++i) spk[i].pos = constraint_project(spk[i].pos);
                layout_dirty = 1; score_stale = 1; cov_err_stale = 1;
            }
            if (IsKeyPressed(KEY_C)) coverage_on = !coverage_on;       /* coverage overlay */
            if (IsKeyPressed(KEY_V)) coverage_moving = !coverage_moving;
            if (IsKeyPressed(KEY_G)) cov_metric ^= 1;   /* shade: gap <-> selected-panner rE error (cache stays valid) */
            if (IsKeyPressed(KEY_X) || req_score) {  req_score = 0;     /* score the layout for each panner */
                for (int p = 0; p < 3; ++p) score_panner((BwPanner)p, 1, &score_mean[p], &score_worst[p]);
                scored = 1; score_stale = 0;
            }
            if (IsKeyPressed(KEY_B)) pv_panner = (pv_panner + 1) % 3;   /* select the target panner (score/optimize) */
            if (IsKeyPressed(KEY_O) || req_opt) { req_opt = 0;          /* toggle the auto-optimizer for that panner */
                opt_running = !opt_running;
                if (opt_running) { opt_cost = opt_cost_of((BwPanner)pv_panner); opt_step = 0.30f; opt_stall = 0; opt_iter = 0;
                                   for (int i = 0; i < NSPK; ++i) opt_anchor[i] = spk[i].pos; }   /* leash anchor = here */
            }
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !IsMouseButtonDown(MOUSE_BUTTON_RIGHT)
                && GetMouseX() < GetScreenWidth() - PANEL_W) {  /* click-pick a speaker (ignore clicks on the panel) */
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
            if ((IsKeyPressed(KEY_P) || req_preview) && !editing) { req_preview = 0;  /* enter DBAP preview — rebuild so it pans through the edited layout */
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
        for (int i = 0; i < NSPK; ++i) if (spk[i].pos.y < 0.0f) spk[i].pos.y = 0.0f;   /* y >= 0: speakers never below the floor */

        /* keep the per-panner rE-error scoreboard live: re-score (throttled, so a drag doesn't recompute
         * every frame) whenever the layout is dirty. X forces an immediate refresh. */
        if (scored && score_stale && !editing && (cov_frame - last_score_frame) >= 10) {
            for (int p = 0; p < 3; ++p) score_panner((BwPanner)p, 1, &score_mean[p], &score_worst[p]);
            score_stale = 0; last_score_frame = cov_frame;
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
        int con_bad = 0, con_occ = 0;
        if (con_loaded) for (int i = 0; i < NSPK; ++i) {
            if (!constraint_ok(spk[i].pos)) ++con_bad;   /* out of bounds / inside a solid body (snappable) */
            if (!los_clear(spk[i].pos))     ++con_occ;   /* line of sight to the ears blocked by an obstacle */
        }
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
            for (int i = 0; i < con_nobst; ++i) {        /* solid occluders (projectors/structure): filled orange */
                Vector3 ctr = Vector3Scale(Vector3Add(con_obst[i].lo, con_obst[i].hi), 0.5f);
                Vector3 sz  = Vector3Subtract(con_obst[i].hi, con_obst[i].lo);
                DrawCubeV(ctr, sz, (Color){ 235, 150, 60, 70 });
                DrawCubeWiresV(ctr, sz, (Color){ 245, 165, 70, 200 });
            }
        }
        DrawLine3D((Vector3){ 0, 0, 0 }, (Vector3){ 1.2f, 0, 0 }, (Color){ 230, 90, 90, 255 });   /* +X */
        DrawLine3D((Vector3){ 0, 0, 0 }, (Vector3){ 0, 1.2f, 0 }, (Color){ 90, 230, 90, 255 });   /* +Y */
        DrawLine3D((Vector3){ 0, 0, 0 }, (Vector3){ 0, 0, 1.2f }, (Color){ 90, 150, 230, 255 });  /* +Z */
        DrawSphere((Vector3){ 0, obs_height, 0 }, 0.09f, (Color){ 210, 210, 230, 255 });           /* listener (ear height) */
        DrawLine3D((Vector3){ 0, obs_height, 0 }, spk[sel].pos,   /* ear<->speaker sightline: green clear / red blocked */
                   los_clear(spk[sel].pos) ? (Color){ 240, 220, 120, 160 } : (Color){ 245, 90, 90, 230 });
        for (int i = 0; i < NSPK; ++i) {
            int is_sel = (i == sel), is_drv = (audio && tone_on && i == sel);
            Color col = is_drv ? (Color){ 120, 245, 140, 255 }
                      : is_sel ? (Color){ 245, 220, 90, 255 }
                               : (Color){ 120, 120, 150, 255 };
            DrawSphere(spk[i].pos, is_sel ? 0.15f : 0.10f, col);
            if (!constraint_ok(spk[i].pos))              /* red: outside bounds / inside a solid body (snap fixes) */
                DrawSphereWires(spk[i].pos, is_sel ? 0.20f : 0.16f, 6, 6, (Color){ 245, 80, 80, 255 });
            else if (!los_clear(spk[i].pos))             /* orange: sightline to the ears is blocked (move it clear) */
                DrawSphereWires(spk[i].pos, is_sel ? 0.20f : 0.16f, 6, 6, (Color){ 245, 165, 70, 255 });
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
                    sdir[l][i] = Vector3Normalize(Vector3Subtract(spk[i].pos, Vector3Add(cov_lis[l], (Vector3){ 0, obs_height, 0 })));
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
                float a = score < -1 ? -1 : (score > 1 ? 1 : score);
                float gap = acosf(a) * 57.2958f;          /* nearest-speaker angular gap (deg) for this direction */
                cov_val[s] = gap;
                DrawCubeV(Vector3Add((Vector3){ 0, obs_height, 0 }, Vector3Scale(d, COV_R)), (Vector3){ 0.09f, 0.09f, 0.09f },
                          heat((40.0f - gap) / 35.0f));   /* geometric gap: green <=5 deg, red >=40 deg */
                if (score < worst) worst = score;
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
                float err = cov_err[s];
                cov_val[s] = err;
                DrawCubeV(Vector3Add((Vector3){ 0, obs_height, 0 }, Vector3Scale(cov_dir[s], COV_R)), (Vector3){ 0.09f, 0.09f, 0.09f },
                          err_heat(err));               /* 2 deg great (green), ~7 fine (yellow), 12+ bad (red) */
                if (err > cov_worst) cov_worst = err;
                macc += err;
            }
            cov_mean = (float)(macc / NCOV);
        }
        EndMode3D();

        /* 2D index labels projected from 3D (skipping any that would fall under the panel) */
        int panel_on = (!preview && !editing);
        int hud_w    = panel_on ? GetScreenWidth() - PANEL_W : GetScreenWidth();
        BeginScissorMode(0, 0, hud_w, GetScreenHeight());     /* clip ALL HUD text/bars out of the panel column */
        for (int i = 0; i < NSPK; ++i) {
            Vector2 s = GetWorldToScreen(spk[i].pos, cam);
            if (panel_on && s.x > (float)hud_w - 6) continue;
            ui_text(TextFormat("%d", i), (int)s.x + 6, (int)s.y - 6, i == sel ? 18 : 12,
                     i == sel ? (Color){ 250, 230, 120, 255 } : (Color){ 150, 150, 170, 220 });
        }

        /* HUD (left region; the panel owns the right in edit mode) */
        DrawRectangle(0, 0, hud_w, 96, (Color){ 0, 0, 0, 200 });
        if (preview) {
            ui_text(TextFormat("PREVIEW   WASD/RF move   SPACE auto-orbit   B panner: %s   P back to edit",
                                panner_names[pv_panner]),
                     10, 8, 13, RAYWHITE);
            ui_text(TextFormat("source (%.2f, %.2f, %.2f)   orbit %s   - A/B the panner by ear; walk the room for off-center coverage",
                                src_pos.x, src_pos.y, src_pos.z, pv_orbit ? "ON" : "off"),
                     10, 30, 16, (Color){ 240, 160, 120, 255 });
        } else {
            ui_text("F11 fullscreen   drag/wheel: camera   click: pick   arrows/RF: move (SHIFT fine)",
                     10, 8, 13, RAYWHITE);
            ui_text(TextFormat("spk %d -> ch %d   pos (%.3f, %.3f, %.3f)   delay %.3f ms   dist %.2f m",
                                sel, sel, spk[sel].pos.x, spk[sel].pos.y, spk[sel].pos.z, seldel, seld),
                     10, 30, 16, (Color){ 245, 220, 90, 255 });
            if (editing)
                ui_text(TextFormat("type \"x y z\" then ENTER:  %s_", ibuf), 10, 54, 16, (Color){ 120, 245, 140, 255 });
            else
                ui_text(TextFormat("save target: %s", path), 10, 54, 15, (Color){ 110, 200, 255, 255 });
        }
        ui_text(audio ? TextFormat("audio: %s  (tone drives the selected channel)", backend)
                       : "audio: none - editor only (needs a 26-ch ASIO/DVS device to audition)",
                 10, 76, 14, audio ? (Color){ 110, 235, 130, 255 } : (Color){ 235, 170, 110, 255 });
        if (!preview && save_flash != 0.0f) {            /* transient save result in its OWN box (can't overlap a line) */
            int by = GetScreenHeight() - 88;
            const char* msg = save_flash > 0 ? TextFormat("saved -> %s", path) : "SAVE FAILED (path not writable?)";
            DrawRectangle(6, by, 452, 24, (Color){ 12, 12, 16, 235 });
            ui_text(msg, 14, by + 4, 15, save_flash > 0 ? (Color){ 130, 245, 150, 255 } : (Color){ 245, 130, 130, 255 });
        }
        if (con_loaded && !preview) {                    /* placement-constraint status */
            DrawRectangle(0, 96, 320, 22, (Color){ 0, 0, 0, 175 });
            ui_text(TextFormat("constraints: %d no-go  %d obstacle   %d out [K snap]  %d occluded (move clear)",
                                con_nnogo, con_nobst, con_bad, con_occ),
                     10, 100, 14, (con_bad || con_occ) ? (Color){ 245, 130, 130, 255 } : (Color){ 120, 220, 140, 255 });
        }
        if (!preview && opt_running) {                   /* auto-optimizer progress (the target panner lives in the panel) */
            int yo = con_loaded ? 122 : 100;
            DrawRectangle(0, yo - 4, 520, 22, (Color){ 0, 0, 0, 175 });
            ui_text(TextFormat("OPTIMIZING %s   cost %.1f   iter %d   step %.2f m%s   [O] stop",
                                panner_names[pv_panner], opt_cost, opt_iter, opt_step, editing ? "   (paused)" : ""),
                     10, yo, 14, (Color){ 120, 245, 160, 255 });
        }
        if (coverage_on && !preview) {                   /* bottom: the coverage summary */
            int yb = GetScreenHeight() - 26;
            const char* obs = coverage_moving ? "moving" : "fixed";
            DrawRectangle(0, yb - 5, hud_w, 31, (Color){ 0, 0, 0, 195 });
            if (cov_metric == 0)
                ui_text(TextFormat("nearest-speaker gap [%s]   worst %.0f deg   mean %.0f deg   [G] -> rE error",
                                    obs, cov_worst, cov_mean),
                         10, yb, 15, heat((40.0f - cov_mean) / 35.0f));
            else
                ui_text(TextFormat("%s rE error [%s]%s   worst %.0f  mean %.0f deg   (2 great / 5-10 ok / 10+ bad)   [G] -> gap",
                                    panner_names[pv_panner], obs, perceptual ? " az>el" : "", cov_worst, cov_mean),
                         10, yb, 15, err_heat(cov_mean));
        }
        if (scored && !preview) {                        /* per-panner rE-localization error, live (X = force refresh) */
            int ys = GetScreenHeight() - (coverage_on ? 52 : 26);
            DrawRectangle(0, ys - 5, hud_w, 31, (Color){ 0, 0, 0, 195 });
            ui_text(TextFormat("rE-err deg mean/worst (live%s):   %sDBAP %.0f/%.0f    %sSPCAP %.0f/%.0f    %sVBAP %.0f/%.0f",
                                perceptual ? ", az>el" : "",
                                pv_panner==0?">":"", score_mean[0], score_worst[0],
                                pv_panner==1?">":"", score_mean[1], score_worst[1],
                                pv_panner==2?">":"", score_mean[2], score_worst[2]),
                     10, ys, 15, (Color){ 150, 200, 240, 255 });
        }

        EndScissorMode();

        /* ---- raygui control panel (edit mode; the keyboard shortcuts all still work) ---- */
        if (!preview && !editing) {
            const float pw = PANEL_W, px = (float)GetScreenWidth() - pw;
            const float x = px + 10, w = pw - 20, rh = 22, gp = 5;
            float y = 8;
            GuiPanel((Rectangle){ px, 0, pw, (float)GetScreenHeight() }, NULL);

            GuiLabel((Rectangle){ x, y, w, rh }, "SPEAKER");  y += rh;
            GuiSpinner((Rectangle){ x, y, w, rh }, NULL, &sel, 0, NSPK - 1, false);  y += rh + gp;
            GuiLabel((Rectangle){ x, y, w, 16 }, TextFormat("gain  %+.1f dB", spk[sel].gain_db));  y += 16;
            { float g = spk[sel].gain_db;
              GuiSliderBar((Rectangle){ x, y, w, rh }, NULL, NULL, &spk[sel].gain_db, -24.0f, 12.0f);
              if (spk[sel].gain_db != g) layout_dirty = 1; }  y += rh + gp;
            if (GuiButton((Rectangle){ x, y, w, rh }, "Type X Y Z  [Enter]")) req_type = 1;  y += rh + gp + 6;

            GuiLabel((Rectangle){ x, y, w, rh }, "PANNER  (score/opt target)");  y += rh;
            { int prev = pv_panner;
              GuiComboBox((Rectangle){ x, y, w, rh }, "DBAP;SPCAP;VBAP", &pv_panner);
              if (pv_panner != prev) { score_stale = 1; cov_err_stale = 1; } }  y += rh + gp;
            if (GuiButton((Rectangle){ x, y, w/2 - 3, rh }, "Score [X]")) req_score = 1;
            { bool ob = opt_running;
              GuiToggle((Rectangle){ x + w/2 + 3, y, w/2 - 3, rh }, opt_running ? "Optimizing.." : "Optimize [O]", &ob);
              if (ob != (bool)opt_running) req_opt = 1; }  y += rh + gp;
            GuiLabel((Rectangle){ x, y, 52, rh }, "obs y");   /* observer ear height -> scored listener */
            { float v = obs_height; GuiSliderBar((Rectangle){ x+54, y, w-116, rh }, NULL, NULL, &obs_height, 0.0f, 2.0f);
              if (obs_height != v) { score_stale = 1; cov_err_stale = 1; } }
            GuiLabel((Rectangle){ x+w-56, y, 56, rh }, TextFormat("%.2fm", obs_height));  y += rh + gp;
            GuiLabel((Rectangle){ x, y, 52, rh }, "leash");   /* max optimizer move from the anchor */
            GuiSliderBar((Rectangle){ x+54, y, w-116, rh }, NULL, NULL, &opt_leash, 0.1f, 3.0f);
            GuiLabel((Rectangle){ x+w-56, y, 56, rh }, TextFormat("%.2fm", opt_leash));  y += rh + gp;
            { bool pc = perceptual;                        /* weight azimuth >> elevation in the error */
              GuiToggle((Rectangle){ x, y, w/2 - 3, rh }, "perceptual", &pc);
              if (pc != (bool)perceptual) { perceptual = pc; score_stale = 1; cov_err_stale = 1; }
              if (perceptual) { float v = elev_wt;
                  GuiSliderBar((Rectangle){ x+w/2+42, y, w/2-45, rh }, "el wt", NULL, &elev_wt, 0.0f, 1.0f);
                  if (elev_wt != v) { score_stale = 1; cov_err_stale = 1; } } }  y += rh + gp + 6;

            GuiLabel((Rectangle){ x, y, w, rh }, "DBAP KNOBS");  y += rh;
            { float v;
              GuiLabel((Rectangle){ x, y, w, 14 }, TextFormat("blur r  %.2f m", dbap_r));  y += 14;
              v = dbap_r;       GuiSliderBar((Rectangle){ x, y, w, rh }, NULL, NULL, &dbap_r,       0.05f, 3.0f);  if (dbap_r != v)       layout_dirty = 1;  y += rh + gp;
              GuiLabel((Rectangle){ x, y, w, 14 }, TextFormat("dist ref  %.2f m", dist_ref));  y += 14;
              v = dist_ref;     GuiSliderBar((Rectangle){ x, y, w, rh }, NULL, NULL, &dist_ref,     0.25f, 4.0f);  if (dist_ref != v)     layout_dirty = 1;  y += rh + gp;
              GuiLabel((Rectangle){ x, y, w, 14 }, TextFormat("rolloff  %.2f", dist_rolloff));  y += 14;
              v = dist_rolloff; GuiSliderBar((Rectangle){ x, y, w, rh }, NULL, NULL, &dist_rolloff, 0.0f,  2.0f);  if (dist_rolloff != v) layout_dirty = 1;  y += rh + gp;
              GuiLabel((Rectangle){ x, y, w, 14 }, TextFormat("min gain  %.0f dB", dist_min_db));  y += 14;
              v = dist_min_db;  GuiSliderBar((Rectangle){ x, y, w, rh }, NULL, NULL, &dist_min_db, -60.0f, 0.0f);  if (dist_min_db != v)  layout_dirty = 1;  y += rh + gp + 6; }

            GuiLabel((Rectangle){ x, y, w, rh }, "AUDITION / VIEW");  y += rh;
            { bool t = tone_on; GuiToggle((Rectangle){ x, y, w/2 - 3, rh }, "Tone [T]", &t); tone_on = t;
              bool n = (tone_kind == BW_TEST_NOISE);
              GuiToggle((Rectangle){ x + w/2 + 3, y, w/2 - 3, rh }, n ? "Noise [N]" : "Sine [N]", &n);
              tone_kind = n ? BW_TEST_NOISE : BW_TEST_SINE; }  y += rh + gp;
            { bool c = coverage_on; GuiToggle((Rectangle){ x, y, w/2 - 3, rh }, "Coverage [C]", &c); coverage_on = c;
              bool m = coverage_moving;
              GuiToggle((Rectangle){ x + w/2 + 3, y, w/2 - 3, rh }, m ? "Moving [V]" : "Fixed [V]", &m);
              coverage_moving = m; }  y += rh + gp;
            { bool me = cov_metric;
              GuiToggle((Rectangle){ x, y, w, rh }, cov_metric ? "Shade: rE error [G]" : "Shade: nearest gap [G]", &me);
              cov_metric = me; }  y += rh + gp;
            if (GuiButton((Rectangle){ x, y, w, rh }, "Preview - audition [P]")) req_preview = 1;  y += rh + gp;

            GuiLine((Rectangle){ x, y, w, 8 }, NULL);  y += 12;
            if (GuiButton((Rectangle){ x, y, w/2 - 3, rh }, "Snap [K]")) req_snap = 1;
            if (GuiButton((Rectangle){ x + w/2 + 3, y, w/2 - 3, rh }, "Save [S]")) req_save = 1;  y += rh + gp;
            if (GuiButton((Rectangle){ x, y, w, rh }, "Reload [L]")) req_reload = 1;  y += rh + gp + 6;
            if (GuiButton((Rectangle){ x, y, w, rh }, "Fullscreen [F11]")) ToggleBorderlessWindowed();
        }

        /* coverage hover: mouse over a shell cube -> read that sample's value (drawn on top) */
        if (coverage_on && !preview && GetMouseX() < hud_w) {
            Ray ray = GetMouseRay(GetMousePosition(), cam);
            int hit = -1; float best = 1e30f;
            for (int s = 0; s < NCOV; ++s) {
                Vector3 c = Vector3Add((Vector3){ 0, obs_height, 0 }, Vector3Scale(cov_dir[s], COV_R));
                BoundingBox bb = { { c.x-0.07f, c.y-0.07f, c.z-0.07f }, { c.x+0.07f, c.y+0.07f, c.z+0.07f } };
                RayCollision rc = GetRayCollisionBox(ray, bb);
                if (rc.hit && rc.distance < best) { best = rc.distance; hit = s; }
            }
            if (hit >= 0) {
                Vector2 mp = GetMousePosition();
                const char* lbl = TextFormat(cov_metric == 0 ? "gap %.0f deg" : "rE err %.0f deg", cov_val[hit]);
                int tx = (int)mp.x + 14, ty = (int)mp.y - 6;
                DrawRectangle(tx - 4, ty - 3, 128, 22, (Color){ 0, 0, 0, 225 });
                ui_text(lbl, tx, ty, 15, (Color){ 245, 235, 150, 255 });
            }
        }
        EndDrawing();
    }

    CloseWindow();
    if (e) { bw_stop(e); bw_destroy(e); }
    remove(PREV_WAV); remove(TEMP_LAYOUT);
    return 0;
}
