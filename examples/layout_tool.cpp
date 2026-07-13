/*
 * layout_tool.cpp — interactive speaker-layout authoring for the CAVE array.
 *
 * Define where the 26 speakers physically are and export a valid cave_layout.json (the file the
 * engine loads via BwConfig.layout_path; see docs/layout-schema.md). The killer feature is identify-
 * by-ear: each speaker's INDEX is its bus/output channel, so selecting speaker N and enabling the
 * tone drives that exact channel with the built-in test signal (bw_test_signal) out the cave profile
 * -> DVS -> the physical speaker. So the survey loop is: pick an index, enable the tone, walk to
 * whichever speaker sounds, read its position, and place marker N there (nudge, or type exact coords
 * into the panel's position field).
 *
 * On export, delay_ms is auto-computed from the positions (max-distance alignment: the farthest
 * speaker from the origin gets 0, nearer ones wait for it); gain_db is your per-speaker trim. The
 * dbap knobs round-trip. Loads an existing layout (argv[1] or ./cave_layout.json) to keep iterating.
 *
 * Coordinate frame: room space, right-handed, +y up, +z forward, origin ON THE FLOOR at the
 * working-area centre in x/z (Motive's default; y = height above the floor) — the same numbers the
 * JSON stores. The 3D view renders them directly; the audition (which physical
 * speaker sounds) is the ground truth for the channel<->speaker map, the numeric readout for position.
 *
 * P / Preview: a pink-noise source moves through your in-progress layout (the tool rebuilds the
 * engine with the edited positions, since the layout is load-time), so you can hear the panning —
 * gaps, smoothness, holes — and at the CAVE you walk the room to judge off-center coverage.
 *
 * C / Coverage: each direction is shaded by its nearest-speaker angular gap (green = a speaker sits
 * near that direction, red = a hole the DBAP pan would smear across far speakers), or [G] by the
 * selected panner's per-direction rE-localization error (its real solve, cached + throttled). V
 * switches the observer model — FIXED (a single audience sweet spot) or MOVING (the default: mean
 * coverage over a grid of listener positions across the working volume; see docs/spatialization.md).
 * X scores the layout for each panner; O runs the auto-optimizer (a hill-climb that minimises the
 * selected panner's rE error subject to the constraints; runs live, O again to stop, then save).
 * K snaps all speakers to the nearest allowed point. Drop a `constraints.json` next to the layout
 * (an allowed `bounds` box + `nogo` boxes for screens/structure + solid `obstacles`) and the tool
 * draws them (green bounds / red no-go / orange solid), flags violating speakers, and K projects
 * them in. H is a first-person view from the observer's ears.
 *
 * UI stack: the 3D room view (orbit + head-view cameras, ray-picked speakers, the coverage shell)
 * stays raylib — it is a *scene*, not a plot — and every control surface (panel, HUD, tooltips) is
 * Dear ImGui rendered on top via rlImGui, themed like the calibration station (bw_theme.h).
 * imgui_test_engine drives the ACTUAL panel with fake inputs under ctest (`--tests [filter]`),
 * captures screenshots to output/captures/, and exits pass/fail — same harness as bw_calib_view.
 * Keyboard shortcuts act only when imgui doesn't want the keyboard; mouse picking/camera only when
 * the cursor isn't over the UI (io.WantCapture*).
 *
 * Controls (edit): [ ] select speaker (or left-click)   arrows X/Z, R/F Y (SHIFT = fine)
 *           PgUp/PgDn gain_db   T tone   N sine/noise   C coverage   V observer   G shade metric
 *           X score   B panner   O optimize   K snap   S save   L reload   H head view   F11 fullscreen
 * Controls (preview, P toggles): WASD/RF move the source   SPACE auto-orbit/near-far/high-low sweep
 *           B A/B the panner DBAP<->SPCAP<->VBAP live   Common: right-drag/wheel camera
 *           ESC / close quits — with a confirm (Save and quit / Quit / Cancel) if there are unsaved edits
 * Build: cmake -S . -B build -DBWAUDIO_BUILD_PLAYGROUND=ON && cmake --build build --target bw_layout_tool
 */
#include "bwaudio.h"
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"            /* rlDrawRenderBatchActive: flush the 3D batch before a screenshot */
#include "speaker_gizmo.h"   /* the "real speaker" glyph (cabinet + cone aimed at the listener) */
#include "cJSON.h"
#include "constraints_view.h"   /* constraints.json load + box drawing, shared with the playground */
#include "axes_hud.h"        /* screen-corner XYZ triad, shared with the playground */

#include "imgui.h"
#include "rlImGui.h"
#include "bw_theme.h"        /* station theme + embedded Roboto (applyTheme / loadEmbeddedFont / uiScaled) */
#include "imgui_te_engine.h"
#include "imgui_te_context.h"
#include "imgui_te_ui.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NSPK           26
#define SR             48000u
#define SPEED_OF_SOUND 343.0f
#define TEST_GAIN      0.4f
#define PANEL_W        300.0f     /* control panel width (right side), in unscaled UI px */

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
        spk[i].pos     = Vector3{ R * r * cosf(th), R * y, R * r * sinf(th) };
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
    /* delay_ms time-aligns arrivals at the LISTENING POINT (ears at obs_height), not the floor
     * origin — since the +z-forward/floor-origin move, distance-to-origin would skew alignment. */
    const Vector3 ear = { 0.0f, obs_height, 0.0f };
    float dmax = 0.0f;
    for (int i = 0; i < NSPK; ++i) { float d = Vector3Distance(spk[i].pos, ear); if (d > dmax) dmax = d; }
    fprintf(f,
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"units\": { \"position\": \"meters\", \"gain\": \"decibels\", \"delay\": \"milliseconds\" },\n"
        "  \"coordinate_space\": \"room, right-handed, +y up, +z forward (matches OptiTrack/Motive default); origin ON THE FLOOR at the working-area centre (x/z); y = height above the floor\",\n"
        "  \"reference\": { \"alignment\": \"max-distance\", \"speed_of_sound_mps\": %g, \"note\": \"delay_ms time-aligns each speaker arrival to the farthest speaker; gain_db is a measured per-speaker trim\" },\n"
        "  \"dbap\": { \"rolloff_r\": %g, \"distance_attenuation\": { \"model\": \"%s\", \"reference_distance_m\": %g, \"rolloff\": %g, \"min_gain_db\": %g } },\n"
        "  \"speakers\": [\n",
        (double)SPEED_OF_SOUND, dbap_r, dist_model, dist_ref, dist_rolloff, dist_min_db);
    for (int i = 0; i < NSPK; ++i) {
        float d = Vector3Distance(spk[i].pos, ear);
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
 * body + the near shadow you care about. Line-of-sight is to the single observer at (0, obs_height, 0).
 * Loading + drawing are shared with the playground (constraints_view.h: cv_load / cv_draw); the
 * snap/projection/line-of-sight logic below is this tool's own. */
typedef CvBox Box;
static CvConstraints CON;          /* CON.loaded == 0 -> every placement allowed (y >= 0 still holds) */

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

/* clear line of sight from a speaker at p to the observer's ears (obstacle boxes block it). */
static int los_clear(Vector3 p) {
    Vector3 obs = { 0, obs_height, 0 };
    for (int i = 0; i < CON.nobst; ++i) if (seg_hits_box(p, obs, CON.obst[i])) return 0;
    return 1;
}

static int constraint_ok(Vector3 p) {                    /* physical: in bounds, out of no-go AND out of solid bodies */
    if (!CON.loaded) return 1;                            /* no constraints loaded -> all positions allowed */
    if (!box_in(CON.bounds, p)) return 0;
    for (int i = 0; i < CON.nnogo; ++i) if (box_in(CON.nogo[i], p)) return 0;
    for (int i = 0; i < CON.nobst; ++i) if (box_in(CON.obst[i], p)) return 0;
    return 1;
}
/* move p just outside box b through the nearest face that stays inside CON.bounds (so a no-go flush
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
    for (int i = 0; i < 6; ++i) if (box_in(CON.bounds, cand[i]) && d[i] < bd) { bd = d[i]; best = i; }
    if (best < 0) for (int i = 0; i < 6; ++i) if (d[i] < bd) { bd = d[i]; best = i; }  /* over-constrained: nearest face */
    return cand[best];
}
static Vector3 constraint_project(Vector3 p) {           /* nearest allowed point: clamp to bounds, push out of no-go */
    if (!CON.loaded) { p.y = fmaxf(0.0f, p.y); return p; }   /* y >= 0 is a hard global floor even with no constraints file */
    for (int pass = 0; pass < 4; ++pass) {               /* a few passes settle overlapping boxes */
        p.x = Clamp(p.x, CON.bounds.lo.x, CON.bounds.hi.x);
        p.y = Clamp(p.y, CON.bounds.lo.y, CON.bounds.hi.y);
        p.z = Clamp(p.z, CON.bounds.lo.z, CON.bounds.hi.z);
        for (int i = 0; i < CON.nnogo; ++i) if (box_in(CON.nogo[i], p)) p = push_out(p, CON.nogo[i]);
        for (int i = 0; i < CON.nobst; ++i) if (box_in(CON.obst[i], p)) p = push_out(p, CON.obst[i]);  /* off solid bodies */
    }
    p.x = Clamp(p.x, CON.bounds.lo.x, CON.bounds.hi.x);  /* final clamp: in-bounds even if over-constrained */
    p.y = Clamp(p.y, CON.bounds.lo.y, CON.bounds.hi.y);
    p.z = Clamp(p.z, CON.bounds.lo.z, CON.bounds.hi.z);
    p.y = fmaxf(0.0f, p.y);                              /* ... but never below the floor */
    return p;
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
    BwConfig cfg = { BW_PROFILE_CAVE, layout_path, NULL, SR, 256, false };
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
    Vector3 azt = Vector3CrossProduct(d, Vector3{ 0, 1, 0 });     /* azimuth tangent (horizontal) */
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
            Vector3 ev = { rE[0]/rl, rE[1]/rl, rE[2]/rl };
            float err = loc_err_deg(cov_dir[i], ev);   /* perceptually weighted (azimuth >> elevation) */
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
                Vector3 ev = { rE[0]/rl, rE[1]/rl, rE[2]/rl };
                err = loc_err_deg(cov_dir[i], ev);       /* perceptually weighted (azimuth >> elevation) */
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

static int edited_unsaved;   /* any layout edit since the last successful save/reload (the quit guard) */

static void mark_edit(void)  { layout_dirty = 1; score_stale = 1; cov_err_stale = 1; edited_unsaved = 1; }
static void mark_score(void) { score_stale = 1; cov_err_stale = 1; }   /* metric knob changed; the layout file didn't */

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
    mark_edit();
}

/* green(good)->yellow->red(bad) ramp. t: 1 = green, 0.5 = yellow, 0 = red. */
static Color heat(float t) {
    if (t < 0) t = 0; else if (t > 1) t = 1;
    float r, g, b;
    if (t >= 0.5f) { float u = (t - 0.5f) * 2.0f;   /* yellow -> green */
        r = 235 + (70  - 235) * u; g = 205; b = 60 + (95 - 60) * u; }
    else { float u = t * 2.0f;                       /* red -> yellow */
        r = 235; g = 70 + (205 - 70) * u; b = 68 + (60 - 68) * u; }
    return Color{ (unsigned char)r, (unsigned char)g, (unsigned char)b, 205 };
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
static ImVec4 imcol(Color c) { return ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f); }

/* checkbox over an int flag (this file's toggles are C ints shared with the key handlers) */
static bool CheckboxInt(const char* label, int* v) {
    bool b = *v != 0;
    bool changed = ImGui::Checkbox(label, &b);
    if (changed) *v = b;
    return changed;
}

/* ============================== UI state + actions ============================== */

static char  g_path[512] = "cave_layout.json";      /* save/load target (panel-editable) */
static int   sel = 0, tone_on = 0, tone_kind = BW_TEST_SINE, driven = -1;
static int   fps_view = 0;                          /* first-person view from the observer's ears (H) */
static float fps_yaw = 0.0f, fps_pitch = 0.0f, fps_fov = 75.0f;   /* yaw 0 = +z, the room's default facing */
static float cam_yaw = 45.0f * DEG2RAD, cam_pitch = 30.0f * DEG2RAD, cam_dist = 9.0f;
static float save_flash = 0.0f;
static bool  show_te_ui = false;

static void do_save(void) {
    int ok = save_json(g_path);
    save_flash = ok ? 2.0f : -2.0f;
    if (ok) edited_unsaved = 0;
    else fprintf(stderr, "save failed: cannot write '%s' (working dir %s) — check the path / permissions\n",
                 g_path, GetWorkingDirectory());
}
static void do_reload(void) {
    load_json(g_path); cv_load("constraints.json", &CON); mark_edit();
    edited_unsaved = 0;                          /* state now equals the file — nothing to lose on quit */
}
static void do_snap(void) {
    for (int i = 0; i < NSPK; ++i) spk[i].pos = constraint_project(spk[i].pos);
    mark_edit();
}
static void do_score(void) {
    for (int p = 0; p < 3; ++p) score_panner((BwPanner)p, 1, &score_mean[p], &score_worst[p]);
    scored = 1; score_stale = 0;
}
static void set_optimizing(int on) {
    if (on && !opt_running) {
        opt_cost = opt_cost_of((BwPanner)pv_panner); opt_step = 0.30f; opt_stall = 0; opt_iter = 0;
        for (int i = 0; i < NSPK; ++i) opt_anchor[i] = spk[i].pos;   /* leash anchor = here */
    }
    opt_running = on;
}
static void enter_preview(void) {      /* rebuild so the preview pans through the edited layout */
    if (driven >= 0 && e) { bw_test_signal(e, (uint32_t)driven, BW_TEST_OFF, 0.0f); driven = -1; }
    tone_on = 0; pv_orbit = 0; pv_t = 0.0f;      /* each preview session starts manual, fresh orbit phase */
    if (layout_dirty && e && audio) {            /* rebuild only when there's a device to hear it on */
        if (save_json(TEMP_LAYOUT)) {            /* ... and only if the temp layout actually wrote */
            bw_stop(e); bw_destroy(e);
            build_engine(TEMP_LAYOUT);
            layout_dirty = 0; driven = -1;
        } else {
            fprintf(stderr, "preview: cannot write %s (working dir not writable?) — previewing the last build\n", TEMP_LAYOUT);
        }
    }
    opt_running = 0;                             /* stop the optimizer when leaving edit for preview */
    preview = 1;
    if (e) {
        bw_set_panner(e, (BwPanner)pv_panner);   /* rebuilt engine defaults to DBAP */
        bw_source_set_gain(e, pv_src, SRC_GAIN);
        bw_commit(e);
    }
}
static void leave_preview(void) {
    preview = 0;
    if (e) { bw_source_set_gain(e, pv_src, 0.0f); bw_commit(e); }
}

/* ---- quit guard: ESC / the close button ask before dropping unsaved edits ---- */
static int quit_ask;     /* open the confirm modal next frame */
static int quit_now;     /* confirmed: leave the main loop */

static void request_quit(void) {
    if (edited_unsaved) quit_ask = 1; else quit_now = 1;
}

/* the [unsaved] marker rides the window title (the Save [S] label stays stable for the tests) */
static void update_title(void) {
    static int shown = -1;
    if (edited_unsaved != shown) {
        SetWindowTitle(edited_unsaved ? "bwaudio - speaker layout tool  [unsaved]"
                                      : "bwaudio - speaker layout tool");
        shown = edited_unsaved;
    }
}

/* drawn at top level every frame (a modal must outlive the panel's edit/preview branches) */
static void draw_quit_modal(void) {
    if (quit_ask) { ImGui::OpenPopup("Unsaved changes"); quit_ask = 0; }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Unsaved changes", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("The layout has edits that are not on disk.");
        ImGui::TextDisabled("save target: %s", g_path);
        if (save_flash < 0)
            ImGui::TextColored(ImVec4(0.96f, 0.51f, 0.51f, 1), "save failed - check the path / permissions");
        ImGui::Separator();
        if (ImGui::Button("Save and quit")) {
            do_save();
            if (!edited_unsaved) { quit_now = 1; ImGui::CloseCurrentPopup(); }
            /* else: the save failed — stay open so the edits aren't silently lost */
        }
        ImGui::SameLine();
        if (ImGui::Button("Quit without saving")) { quit_now = 1; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

/* ============================== per-frame input / camera / scene ============================== */

/* preview mode: move the source (or auto-orbit), A/B the panner, feed the engine */
static void handle_preview_input(float dt, bool kb) {
    if (kb && IsKeyPressed(KEY_P)) { leave_preview(); return; }
    if (kb && IsKeyPressed(KEY_SPACE)) pv_orbit = !pv_orbit;
    if (kb && IsKeyPressed(KEY_B)) {                 /* live A/B: DBAP <-> SPCAP <-> VBAP (atomic, safe while running) */
        pv_panner = (pv_panner + 1) % 3;
        if (e) bw_set_panner(e, (BwPanner)pv_panner);
    }
    if (pv_orbit) {                                  /* hands-free orbit + near/far + high/low sweep */
        pv_t += dt;
        float az = 0.6f * pv_t, r = 1.6f + 1.0f * sinf(0.8f * pv_t), yy = 1.0f * sinf(1.1f * pv_t);
        src_pos = Vector3{ r * cosf(az), yy, r * sinf(az) };
    } else if (kb) {
        float mv = ((IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) ? 0.4f : 1.5f) * dt;
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

/* edit mode: select/nudge speakers, toggle views, fire the shared actions (the panel mirrors all of it) */
static void handle_edit_input(float dt, bool kb, bool ms, const Camera3D& cam) {
    if (kb) {
        if (IsKeyPressed(KEY_RIGHT_BRACKET)) sel = (sel + 1) % NSPK;
        if (IsKeyPressed(KEY_LEFT_BRACKET))  sel = (sel + NSPK - 1) % NSPK;
        Vector3 p0 = spk[sel].pos; float g0 = spk[sel].gain_db;
        float d = ((IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) ? 0.25f : 1.0f) * dt;
        if (IsKeyDown(KEY_LEFT))  spk[sel].pos.x -= d;
        if (IsKeyDown(KEY_RIGHT)) spk[sel].pos.x += d;
        if (IsKeyDown(KEY_UP))    spk[sel].pos.z -= d;
        if (IsKeyDown(KEY_DOWN))  spk[sel].pos.z += d;
        if (IsKeyDown(KEY_R))     spk[sel].pos.y += d;
        if (IsKeyDown(KEY_F))     spk[sel].pos.y -= d;
        if (IsKeyDown(KEY_PAGE_UP))   spk[sel].gain_db += 6.0f * dt;
        if (IsKeyDown(KEY_PAGE_DOWN)) spk[sel].gain_db -= 6.0f * dt;
        if (memcmp(&p0, &spk[sel].pos, sizeof p0) != 0 || g0 != spk[sel].gain_db) mark_edit();
        if (IsKeyPressed(KEY_T)) tone_on = !tone_on;
        if (IsKeyPressed(KEY_N)) tone_kind = (tone_kind == BW_TEST_SINE) ? BW_TEST_NOISE : BW_TEST_SINE;
        if (IsKeyPressed(KEY_S)) do_save();
        if (IsKeyPressed(KEY_L)) do_reload();
        if (IsKeyPressed(KEY_K)) do_snap();
        if (IsKeyPressed(KEY_C)) coverage_on = !coverage_on;
        if (IsKeyPressed(KEY_V)) coverage_moving = !coverage_moving;
        if (IsKeyPressed(KEY_G)) cov_metric ^= 1;   /* shade: gap <-> selected-panner rE error (cache stays valid) */
        if (IsKeyPressed(KEY_X)) do_score();
        if (IsKeyPressed(KEY_B)) pv_panner = (pv_panner + 1) % 3;   /* the score/optimize target panner */
        if (IsKeyPressed(KEY_O)) set_optimizing(!opt_running);
        if (IsKeyPressed(KEY_P)) enter_preview();
    }
    if (ms && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
        Ray ray = GetMouseRay(GetMousePosition(), cam);        /* click-pick a speaker (imgui owns its own area) */
        float best = 1e9f; int hit = -1;
        for (int i = 0; i < NSPK; ++i) {
            RayCollision rc = GetRayCollisionSphere(ray, spk[i].pos, 0.16f);
            if (rc.hit && rc.distance < best) { best = rc.distance; hit = i; }
        }
        if (hit >= 0) sel = hit;
    }
}

/* drive the selected speaker's channel when auditioning (edit mode only) */
static void drive_tone(void) {
    if (!audio || preview) return;
    if (tone_on) {
        if (driven >= 0 && driven != sel) bw_test_signal(e, (uint32_t)driven, BW_TEST_OFF, 0.0f);
        bw_test_signal(e, (uint32_t)sel, (BwTestKind)tone_kind, TEST_GAIN);
        driven = sel;
    } else if (driven >= 0) {
        bw_test_signal(e, (uint32_t)driven, BW_TEST_OFF, 0.0f);
        driven = -1;
    }
}

/* camera: FIRST-PERSON from the observer's ears (H), or orbit (default). Right-drag looks/orbits. */
static void update_camera(Camera3D* cam, bool ms) {
    if (fps_view) {
        if (ms && IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {    /* mouse-look */
            Vector2 md = GetMouseDelta();
            fps_yaw   -= md.x * 0.004f;
            fps_pitch -= md.y * 0.004f;
            if (fps_pitch >  1.5f) fps_pitch =  1.5f;
            if (fps_pitch < -1.5f) fps_pitch = -1.5f;
        }
        if (ms) fps_fov -= GetMouseWheelMove() * 4.0f;        /* wheel zooms the view */
        if (fps_fov < 25.0f) fps_fov = 25.0f; if (fps_fov > 100.0f) fps_fov = 100.0f;
        Vector3 fwd = { cosf(fps_pitch)*sinf(fps_yaw), sinf(fps_pitch), cosf(fps_pitch)*cosf(fps_yaw) };
        cam->position = Vector3{ 0, obs_height, 0 };          /* AT the ears */
        cam->target   = Vector3Add(cam->position, fwd);
        cam->fovy     = fps_fov;
    } else {
        if (ms && IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
            Vector2 md = GetMouseDelta();
            cam_yaw   -= md.x * 0.005f;
            cam_pitch += md.y * 0.005f;
            if (cam_pitch >  1.5f) cam_pitch =  1.5f;
            if (cam_pitch < -1.5f) cam_pitch = -1.5f;
        }
        if (ms) cam_dist -= GetMouseWheelMove() * 0.6f;
        if (cam_dist < 2.0f)  cam_dist = 2.0f;
        if (cam_dist > 30.0f) cam_dist = 30.0f;
        cam->target   = Vector3{ 0, 0, 0 };
        cam->fovy     = 55.0f;
        cam->position.x = cam_dist * cosf(cam_pitch) * sinf(cam_yaw);
        cam->position.y = cam_dist * sinf(cam_pitch);
        cam->position.z = cam_dist * cosf(cam_pitch) * cosf(cam_yaw);
    }
}

/* the raylib 3D scene: grid, constraints, speakers, preview source, coverage shell.
 * Fills the coverage summary (worst/mean, deg) for the HUD line. */
static void draw_scene(const Camera3D& cam, float* cov_worst_out, float* cov_mean_out) {
    float cov_worst = 0.0f, cov_mean = 0.0f;
    BeginMode3D(cam);
    DrawGrid(16, 0.5f);
    cv_draw(&CON);                                   /* constraints: green bounds / red no-go / orange solids */
    DrawLine3D(Vector3{ 0, 0, 0 }, Vector3{ 1.2f, 0, 0 }, Color{ 230, 90, 90, 255 });   /* +X */
    DrawLine3D(Vector3{ 0, 0, 0 }, Vector3{ 0, 1.2f, 0 }, Color{ 90, 230, 90, 255 });   /* +Y */
    DrawLine3D(Vector3{ 0, 0, 0 }, Vector3{ 0, 0, 1.2f }, Color{ 90, 150, 230, 255 });  /* +Z */
    if (!fps_view) DrawSphere(Vector3{ 0, obs_height, 0 }, 0.09f, Color{ 210, 210, 230, 255 });  /* listener (hidden in FPS: you're inside it) */
    DrawLine3D(Vector3{ 0, obs_height, 0 }, spk[sel].pos,   /* ear<->speaker sightline: green clear / red blocked */
               los_clear(spk[sel].pos) ? Color{ 240, 220, 120, 160 } : Color{ 245, 90, 90, 230 });
    for (int i = 0; i < NSPK; ++i) {
        int is_sel = (i == sel), is_drv = (audio && tone_on && i == sel);
        Color col = is_drv ? Color{ 120, 245, 140, 255 }
                  : is_sel ? Color{ 245, 220, 90, 255 }
                           : Color{ 120, 120, 150, 255 };
        draw_speaker_gizmo(spk[i].pos, Vector3{ 0, obs_height, 0 },   /* cones aim at the listener's ears */
                           is_sel ? 0.30f : 0.22f, col);
        if (!constraint_ok(spk[i].pos))              /* red: outside bounds / inside a solid body (snap fixes) */
            DrawSphereWires(spk[i].pos, is_sel ? 0.22f : 0.18f, 6, 6, Color{ 245, 80, 80, 255 });
        else if (!los_clear(spk[i].pos))             /* orange: sightline to the ears is blocked (move it clear) */
            DrawSphereWires(spk[i].pos, is_sel ? 0.22f : 0.18f, 6, 6, Color{ 245, 165, 70, 255 });
    }
    if (preview) {                                   /* the moving DBAP source */
        DrawLine3D(Vector3{ 0, 0, 0 }, src_pos, Color{ 90, 220, 90, 200 });
        DrawSphere(src_pos, 0.16f, Color{ 240, 120, 90, 255 });
    }
    if (coverage_on && !preview && cov_metric == 0) {  /* shade each direction by its nearest-speaker gap */
        int NL = coverage_moving ? 27 : 1;
        static Vector3 sdir[27][26];                  /* speaker directions from each listener sample */
        for (int l = 0; l < NL; ++l)
            for (int i = 0; i < NSPK; ++i)
                sdir[l][i] = Vector3Normalize(Vector3Subtract(spk[i].pos, Vector3Add(cov_lis[l], Vector3{ 0, obs_height, 0 })));
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
            DrawCubeV(Vector3Add(Vector3{ 0, obs_height, 0 }, Vector3Scale(d, COV_R)), Vector3{ 0.09f, 0.09f, 0.09f },
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
            DrawCubeV(Vector3Add(Vector3{ 0, obs_height, 0 }, Vector3Scale(cov_dir[s], COV_R)), Vector3{ 0.09f, 0.09f, 0.09f },
                      err_heat(err));               /* 2 deg great (green), ~7 fine (yellow), 12+ bad (red) */
            if (err > cov_worst) cov_worst = err;
            macc += err;
        }
        cov_mean = (float)(macc / NCOV);
    }
    EndMode3D();
    *cov_worst_out = cov_worst;
    *cov_mean_out  = cov_mean;
}

/* ============================== imgui: labels / HUD / panel ============================== */

/* 2D speaker-index labels projected from 3D — on the BACKGROUND drawlist, so they sit over the
 * raylib scene but under every imgui window (the panel simply covers the ones beneath it). */
static void draw_labels(const Camera3D& cam) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    Vector3 camfwd = Vector3Subtract(cam.target, cam.position);
    for (int i = 0; i < NSPK; ++i) {
        if (fps_view && Vector3DotProduct(Vector3Subtract(spk[i].pos, cam.position), camfwd) <= 0) continue; /* behind the head */
        Vector2 s = GetWorldToScreen(spk[i].pos, cam);
        char buf[8]; snprintf(buf, sizeof buf, "%d", i);
        float sz = ImGui::GetFontSize() * (i == sel ? 1.15f : 0.8f);
        ImU32 col = (i == sel) ? IM_COL32(250, 230, 120, 255) : IM_COL32(160, 160, 180, 220);
        dl->AddText(ImGui::GetFont(), sz, ImVec2(s.x + 6, s.y - 6), col, buf);
    }
}

/* top-left status overlay: pure text, never captures the mouse (scene clicks pass through it) */
static void draw_hud(float cov_worst, float cov_mean) {
    const Vector3 ear = { 0.0f, obs_height, 0.0f };  /* live delay readout: max-distance alignment at the ears */
    float dmax = 0.0f;
    for (int i = 0; i < NSPK; ++i) { float dd = Vector3Distance(spk[i].pos, ear); if (dd > dmax) dmax = dd; }
    float seld   = Vector3Distance(spk[sel].pos, ear);
    float seldel = (dmax - seld) / SPEED_OF_SOUND * 1000.0f;
    int con_bad = 0, con_occ = 0;
    if (CON.loaded) for (int i = 0; i < NSPK; ++i) {
        if (!constraint_ok(spk[i].pos)) ++con_bad;   /* out of bounds / inside a solid body (snappable) */
        if (!los_clear(spk[i].pos))     ++con_occ;   /* line of sight to the ears blocked by an obstacle */
    }
    ImGui::SetNextWindowPos(ImVec2(uiScaled(8), uiScaled(8)));
    ImGui::SetNextWindowBgAlpha(0.72f);
    ImGui::Begin("hud", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                              ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                              ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs);
    if (preview) {
        ImGui::TextDisabled("PREVIEW   WASD/RF move   SPACE auto-orbit   B panner   P back to edit");
        ImGui::TextColored(ImVec4(0.94f, 0.63f, 0.47f, 1), "source (%.2f, %.2f, %.2f)   %s   orbit %s",
                           src_pos.x, src_pos.y, src_pos.z, panner_names[pv_panner], pv_orbit ? "ON" : "off");
        ImGui::TextDisabled("A/B the panner by ear; walk the room for off-center coverage");
    } else {
        ImGui::TextDisabled(fps_view
            ? "HEAD VIEW (from the ears)   right-drag: look   wheel: zoom   [H] exit   (red = worst spots around you)"
            : "[H] head view   right-drag/wheel: camera   click: pick   arrows/R/F: move (SHIFT fine)   F11 fullscreen");
        ImGui::TextColored(ImVec4(0.96f, 0.86f, 0.35f, 1), "spk %d -> ch %d   pos (%.3f, %.3f, %.3f)   delay %.3f ms   dist %.2f m",
                           sel, sel, spk[sel].pos.x, spk[sel].pos.y, spk[sel].pos.z, seldel, seld);
    }
    if (audio) ImGui::TextColored(ImVec4(0.43f, 0.92f, 0.51f, 1), "audio: %s  (tone drives the selected channel)", backend);
    else       ImGui::TextColored(ImVec4(0.92f, 0.67f, 0.43f, 1), "audio: none - editor only (needs a 26-ch ASIO/DVS device to audition)");
    if (CON.loaded && !preview)
        ImGui::TextColored((con_bad || con_occ) ? ImVec4(0.96f, 0.51f, 0.51f, 1) : ImVec4(0.47f, 0.86f, 0.55f, 1),
                           "constraints: %d no-go  %d obstacle   %d out [K snap]  %d occluded (move clear)",
                           CON.nnogo, CON.nobst, con_bad, con_occ);
    if (opt_running && !preview)
        ImGui::TextColored(ImVec4(0.47f, 0.96f, 0.63f, 1), "OPTIMIZING %s   cost %.1f   iter %d   step %.2f m   [O] stop",
                           panner_names[pv_panner], opt_cost, opt_iter, opt_step);
    if (scored && !preview)
        ImGui::TextColored(ImVec4(0.59f, 0.78f, 0.94f, 1),
                           "rE-err deg mean/worst (live%s):   %sDBAP %.0f/%.0f    %sSPCAP %.0f/%.0f    %sVBAP %.0f/%.0f",
                           perceptual ? ", az>el" : "",
                           pv_panner==0?">":"", score_mean[0], score_worst[0],
                           pv_panner==1?">":"", score_mean[1], score_worst[1],
                           pv_panner==2?">":"", score_mean[2], score_worst[2]);
    if (coverage_on && !preview) {
        const char* obs = coverage_moving ? "moving" : "fixed";
        if (cov_metric == 0)
            ImGui::TextColored(imcol(heat((40.0f - cov_mean) / 35.0f)),
                               "nearest-speaker gap [%s]   worst %.0f deg   mean %.0f deg   [G] -> rE error", obs, cov_worst, cov_mean);
        else
            ImGui::TextColored(imcol(err_heat(cov_mean)),
                               "%s rE error [%s]%s   worst %.0f  mean %.0f deg   (2 great / 5-10 ok / 10+ bad)   [G] -> gap",
                               panner_names[pv_panner], obs, perceptual ? " az>el" : "", cov_worst, cov_mean);
    }
    if (!preview && save_flash != 0.0f)
        ImGui::TextColored(save_flash > 0 ? ImVec4(0.51f, 0.96f, 0.59f, 1) : ImVec4(0.96f, 0.51f, 0.51f, 1),
                           save_flash > 0 ? "saved -> %s" : "SAVE FAILED (path not writable?)", g_path);
    ImGui::End();
}

/* right-side control panel, ordered by the workflow: load the file -> place + identify speakers ->
 * tune the panning knobs -> analyze/optimize -> audition -> view. Every control mirrors a keyboard
 * shortcut and fires the same do_* action. */
static void draw_panel(void) {
    const float pw = uiScaled(PANEL_W);
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - pw, vp->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(pw, vp->WorkSize.y));
    ImGui::Begin("layout", NULL, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                                 ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushItemWidth(-uiScaled(70));

    if (preview) {                                   /* preview swaps the whole panel for its own controls */
        ImGui::SeparatorText("Preview");
        ImGui::Text("source (%.2f, %.2f, %.2f)", src_pos.x, src_pos.y, src_pos.z);
        CheckboxInt("auto-orbit [SPACE]", &pv_orbit);
        bwTip("hands-free sweep: the source orbits while moving near/far and high/low");
        if (ImGui::Combo("panner", &pv_panner, "DBAP (moving)\0SPCAP (fixed)\0VBAP (fixed)\0") && e)
            bw_set_panner(e, (BwPanner)pv_panner);   /* live A/B (atomic, safe while running) */
        bwTip("A/B by ear while it plays - the switch is atomic (no glitch)");
        ImGui::TextDisabled("WASD/RF move the source");
        if (ImGui::Button("Back to edit [P]", ImVec2(-1, 0))) leave_preview();
        ImGui::PopItemWidth();
        ImGui::End();
        return;
    }

    ImGui::SeparatorText("File");
    ImGui::PushItemWidth(-1);
    ImGui::InputText("##path", g_path, sizeof g_path);
    bwTip("the layout file: Save writes it, Reload reads it (cave_layout.json schema)");
    ImGui::PopItemWidth();
    float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
    if (ImGui::Button("Reload [L]", ImVec2(half, 0))) do_reload();
    bwTip("re-read the layout + constraints.json from disk - discards unsaved edits");
    ImGui::SameLine();
    if (ImGui::Button("Save [S]", ImVec2(half, 0))) do_save();
    bwTip("write the layout; delay_ms is derived from the positions (max-distance alignment)");

    ImGui::SeparatorText("Speaker");                 /* the survey loop: pick an index, tone it, place it */
    if (ImGui::InputInt("##spk", &sel)) { if (sel < 0) sel = 0; if (sel >= NSPK) sel = NSPK - 1; }
    bwTip("the speaker's index IS its output/bus channel; [ ] steps, or click a sphere");
    ImGui::SameLine(); ImGui::Text("-> ch %d", sel);
    if (ImGui::DragFloat3("pos", &spk[sel].pos.x, 0.01f, 0, 0, "%.3f")) mark_edit();
    bwTip("room-space metres, right-handed, +y up, origin on the floor at the working-area centre (Motive); drag, or ctrl-click to type");
    if (ImGui::SliderFloat("gain", &spk[sel].gain_db, -24.0f, 12.0f, "%+.1f dB")) mark_edit();
    bwTip("per-speaker level trim (gain_db in the file)");
    CheckboxInt("tone [T]", &tone_on);
    bwTip("drive THIS channel with the test signal out the array: walk the room, hear which "
          "physical speaker it is, place its marker (needs the 26-ch ASIO/DVS device)");
    ImGui::SameLine();
    { bool nb = tone_kind == BW_TEST_NOISE;
      if (ImGui::Checkbox("noise [N]", &nb)) tone_kind = nb ? BW_TEST_NOISE : BW_TEST_SINE; }
    bwTip("test-signal type; noise is easier to localize by ear than a sine");
    if (ImGui::Button("Snap all to constraints [K]", ImVec2(-1, 0))) do_snap();
    bwTip("project every speaker to its nearest allowed point: inside bounds, out of no-go and "
          "solid boxes (constraints.json next to the layout); y >= 0 always");

    ImGui::SeparatorText("DBAP knobs");              /* panning params; round-trip through the file */
    if (ImGui::SliderFloat("blur r",   &dbap_r,       0.05f, 3.0f, "%.2f m"))  mark_edit();
    bwTip("DBAP rolloff radius: larger spreads energy over more speakers - smoother pans, less pinpoint");
    if (ImGui::SliderFloat("dist ref", &dist_ref,     0.25f, 4.0f, "%.2f m"))  mark_edit();
    bwTip("distance attenuation: reference distance (no attenuation closer than this)");
    if (ImGui::SliderFloat("rolloff",  &dist_rolloff, 0.0f,  2.0f, "%.2f"))    mark_edit();
    bwTip("distance attenuation: exponent - 1 = inverse law (-6 dB per doubling), 0 = off");
    if (ImGui::SliderFloat("min gain", &dist_min_db, -60.0f, 0.0f, "%.0f dB")) mark_edit();
    bwTip("distance attenuation: floor - a source never drops below this");

    ImGui::SeparatorText("Analyze / optimize");      /* rE error of the target panner; O climbs it */
    if (ImGui::Combo("panner", &pv_panner, "DBAP (moving)\0SPCAP (fixed)\0VBAP (fixed)\0")) mark_score();
    bwTip("the panner Score / Optimize / the rE overlay evaluate ([B] cycles). DBAP tracks a "
          "MOVING listener; SPCAP/VBAP assume the centre sweet spot");
    if (ImGui::Button("Score [X]")) do_score();
    bwTip("rE localization error over a shell of directions, via the engine's real gain solve - "
          "mean/worst per panner land in the HUD scoreboard");
    ImGui::SameLine();
    { bool ob = opt_running != 0; if (ImGui::Checkbox("Optimize [O]", &ob)) set_optimizing(ob); }
    bwTip("stochastic hill-climb of the speaker positions, minimising the target panner's rE error "
          "within the constraints; runs live - watch it converge, stop, then Save");
    ImGui::SliderFloat("leash", &opt_leash, 0.1f, 3.0f, "%.2f m");   /* max optimizer move from the anchor */
    bwTip("how far the optimizer may move any speaker from where it started (3 m = essentially free)");
    if (ImGui::SliderFloat("obs ear y", &obs_height, 0.0f, 2.0f, "%.2f m")) mark_score();
    bwTip("listener EAR height above the floor - scoring, coverage, and the sightline checks all measure from here");
    if (CheckboxInt("perceptual (az>el)", &perceptual)) mark_score();   /* weight azimuth >> elevation */
    bwTip("weight azimuth error over elevation: human azimuth acuity is ~3.5x finer, so the "
          "optimizer trades vertical accuracy for horizontal");
    if (perceptual && ImGui::SliderFloat("elev wt", &elev_wt, 0.0f, 1.0f, "%.2f")) mark_score();
    bwTip("elevation-error weight vs azimuth (0.3 ~ the psychophysics ratio; 1 = isotropic)");
    CheckboxInt("coverage [C]", &coverage_on);
    bwTip("shade a shell of source directions: green = the array localizes it well, red = a hole; "
          "hover a cube for its value");
    ImGui::SameLine();
    CheckboxInt("moving [V]", &coverage_moving);
    bwTip("observer model: average over a grid of listener positions across the working volume "
          "(this installation's case) instead of the centre sweet spot");
    CheckboxInt("shade rE error (vs gap) [G]", &cov_metric);
    bwTip("colour by the selected panner's REAL solve (rE error) instead of the geometric "
          "nearest-speaker gap");

    ImGui::SeparatorText("Audition");
    if (ImGui::Button("Preview - audition [P]", ImVec2(-1, 0))) enter_preview();
    bwTip("pan a pink-noise source through the edited layout and judge it by ear (rebuilds the "
          "engine - the layout is load-time); at the CAVE, walk the room for off-centre coverage");

    ImGui::SeparatorText("View");
    CheckboxInt("head view [H]", &fps_view);
    bwTip("first-person from the observer's ears - see the coverage shell around YOU");
    ImGui::SameLine();
    if (ImGui::Button("Fullscreen [F11]")) ToggleBorderlessWindowed();
    ImGui::Checkbox("test engine UI", &show_te_ui);
    bwTip("imgui_test_engine windows - run the --tests suite interactively");

    ImGui::PopItemWidth();
    ImGui::End();
}

/* coverage hover: mouse over a shell cube -> that sample's value as a tooltip */
static void draw_cov_tooltip(const Camera3D& cam, bool mouse_free) {
    if (!coverage_on || preview || !mouse_free) return;
    Ray ray = GetMouseRay(GetMousePosition(), cam);
    int hit = -1; float best = 1e30f;
    for (int s = 0; s < NCOV; ++s) {
        Vector3 c = Vector3Add(Vector3{ 0, obs_height, 0 }, Vector3Scale(cov_dir[s], COV_R));
        BoundingBox bb = { { c.x-0.07f, c.y-0.07f, c.z-0.07f }, { c.x+0.07f, c.y+0.07f, c.z+0.07f } };
        RayCollision rc = GetRayCollisionBox(ray, bb);
        if (rc.hit && rc.distance < best) { best = rc.distance; hit = s; }
    }
    if (hit >= 0) ImGui::SetTooltip(cov_metric == 0 ? "gap %.0f deg" : "rE err %.0f deg", cov_val[hit]);
}

/* ============================== test engine ============================== */

static ImGuiTestEngine* g_te;
#define TEST_OUT "layout_tool_test.json"

/* test-engine screenshot hook: read the framebuffer via raylib. Runs from PostSwap, which this tool
 * calls BEFORE EndDrawing's buffer swap (after flushing the rlgl batch), so GL_BACK still holds the
 * completed frame. LoadImageFromScreen returns top-down RGBA8 — the layout the capture tool wants. */
static bool screen_capture(ImGuiID viewport_id, int x, int y, int w, int h, unsigned int* pixels, void* user) {
    (void)viewport_id; (void)user;
    Image img = LoadImageFromScreen();
    if (!img.data || img.format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8) { UnloadImage(img); return false; }
    const unsigned int* src = (const unsigned int*)img.data;
    for (int row = 0; row < h; ++row)
        for (int c = 0; c < w; ++c) {
            int sx = x + c, sy = y + row;
            unsigned int v = (sx >= 0 && sx < img.width && sy >= 0 && sy < img.height) ? src[(size_t)sy * img.width + sx] : 0;
            pixels[(size_t)row * w + c] = v | 0xFF000000u;   /* force opaque alpha */
        }
    UnloadImage(img);
    return true;
}

static void register_tests(ImGuiTestEngine* te) {
    ImGuiTest* t;

    /* pure-logic checks ride the same suite (the station's pattern: the test engine is the app's
     * whole harness, not just its UI driver) — no UI touched, still filterable via --tests logic. */
    t = IM_REGISTER_TEST(te, "logic", "save_load");              /* positions/gain + dbap knobs round-trip */
    t->TestFunc = [](ImGuiTestContext*) {
        seed_default();
        spk[3].pos = Vector3{ 1.25f, 2.0f, -0.75f }; spk[3].gain_db = -4.5f; dbap_r = 0.77f;
        IM_CHECK(save_json(TEST_OUT));
        seed_default(); dbap_r = 0.5f;
        IM_CHECK_EQ(load_json(TEST_OUT), NSPK);
        IM_CHECK_LT(fabsf(spk[3].pos.x - 1.25f), 1e-3f);
        IM_CHECK_LT(fabsf(spk[3].pos.z + 0.75f), 1e-3f);
        IM_CHECK_LT(fabsf(spk[3].gain_db + 4.5f), 1e-2f);
        IM_CHECK_LT(fabsf(dbap_r - 0.77f), 1e-5f);
        dbap_r = 0.5f; seed_default(); layout_dirty = 1;
    };

    t = IM_REGISTER_TEST(te, "logic", "constraints");            /* projection escapes a no-go; obstacles block LOS */
    t->TestFunc = [](ImGuiTestContext*) {
        Box sb = CON.bounds; int sn = CON.nnogo, so = CON.nobst, sl = CON.loaded;
        CON.loaded = 1; CON.bounds = { { -3, -3, -3 }, { 3, 3, 3 } };
        CON.nnogo = 1;  CON.nogo[0] = { { -1, -1, -1 }, { 1, 1, 1 } };
        CON.nobst = 1;  CON.obst[0] = { { 0.9f, 0.9f, 0.9f }, { 1.4f, 2.2f, 1.4f } };
        Vector3 p = constraint_project(Vector3{ 0.2f, 0.5f, 0.1f });
        IM_CHECK(!box_in(CON.nogo[0], p));                       /* pushed out of the keep-out box */
        IM_CHECK(constraint_ok(p));
        IM_CHECK(!los_clear(Vector3{ 2, 3, 2 }));                /* the obstacle sits on the sightline to the ears */
        IM_CHECK(los_clear(Vector3{ -2, 1, -2 }));               /* opposite side: clear */
        CON.bounds = sb; CON.nnogo = sn; CON.nobst = so; CON.loaded = sl;
    };

    t = IM_REGISTER_TEST(te, "logic", "score");                  /* the real engine solve scores the default dome sanely */
    t->TestFunc = [](ImGuiTestContext*) {
        seed_default(); layout_dirty = 1;
        float m = -1, w = -1;
        score_panner(BW_PAN_DBAP, 2, &m, &w);
        IM_CHECK_GT(m, 0.0f);
        IM_CHECK_LT(m, 90.0f);
        IM_CHECK_GE(w, m);
        IM_CHECK_LT(w, 181.0f);
    };

    t = IM_REGISTER_TEST(te, "viewer", "panel_edit");            /* fake inputs drive the panel; state follows */
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("layout");
        ctx->ItemInputValue("**/##spk", 7);
        IM_CHECK_EQ(sel, 7);
        /* no wildcard here: a wildcard search hashes its last segment as a literal string, so "$$0"
         * only parses as PushID(int) in a plain path relative to the window ref. DragFloat3 pushes
         * the label then the component index, and its leaf label is "" (which hashes to a no-op). */
        ctx->ItemInputValue("pos/$$0", 2.25f);
        IM_CHECK_LT(fabsf(spk[7].pos.x - 2.25f), 1e-3f);
        IM_CHECK_EQ(layout_dirty, 1);
        ctx->ItemInputValue("**/gain", -3.5f);
        IM_CHECK_LT(fabsf(spk[7].gain_db + 3.5f), 1e-2f);
    };

    t = IM_REGISTER_TEST(te, "viewer", "save_reload");           /* Save writes the file; Reload restores from it */
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("layout");
        ctx->ItemClick("**/##path");
        ctx->KeyCharsReplaceEnter(TEST_OUT);
        seed_default(); spk[5].pos = Vector3{ 0.5f, 1.5f, 2.0f }; mark_edit();
        ctx->ItemClick("**/Save [S]");
        IM_CHECK_GT(save_flash, 0.0f);
        spk[5].pos = Vector3{ -9, 9, -9 };                       /* wreck it, then reload from the file */
        ctx->ItemClick("**/Reload [L]");
        IM_CHECK_LT(fabsf(spk[5].pos.x - 0.5f), 1e-3f);
        IM_CHECK_LT(fabsf(spk[5].pos.z - 2.0f), 1e-3f);
    };

    t = IM_REGISTER_TEST(te, "viewer", "quit_guard");            /* unsaved edits gate ESC/close behind a confirm */
    t->TestFunc = [](ImGuiTestContext* ctx) {
        seed_default(); mark_edit();
        IM_CHECK_EQ(edited_unsaved, 1);
        request_quit();
        IM_CHECK_EQ(quit_now, 0);                                /* guarded: no instant exit */
        ctx->Yield(2);                                           /* the modal opens */
        ctx->SetRef("//Unsaved changes");
        IM_CHECK(ctx->ItemInfo("Save and quit").ID != 0);        /* both exits offered */
        IM_CHECK(ctx->ItemInfo("Quit without saving").ID != 0);
        ctx->CaptureScreenshot();
        ctx->ItemClick("Cancel");
        IM_CHECK_EQ(quit_now, 0);                                /* cancel keeps the session */
        ctx->Yield(2);
        do_save();                                               /* Save-and-quit relies on exactly this */
        IM_CHECK_EQ(edited_unsaved, 0);
        IM_CHECK_GT(save_flash, 0.0f);
    };

    t = IM_REGISTER_TEST(te, "viewer", "score_optimize");        /* Score fills the board; the optimizer actually climbs */
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("layout");
        seed_default(); mark_edit();
        ctx->ItemClick("**/Score [X]");
        IM_CHECK(scored);
        for (int p = 0; p < 3; ++p) { IM_CHECK_GT(score_mean[p], 0.0f); IM_CHECK_GE(score_worst[p], score_mean[p]); }
        srand(42);                                               /* deterministic climb */
        ctx->ItemCheck("**/Optimize [O]");
        IM_CHECK(opt_running);
        float cost0 = opt_cost;
        ctx->Yield(40);
        IM_CHECK_GT(opt_iter, 0);
        IM_CHECK_LE(opt_cost, cost0 + 1e-3f);                    /* hill-climb never accepts a regression */
        ctx->ItemUncheck("**/Optimize [O]");
        IM_CHECK(!opt_running);
    };

    t = IM_REGISTER_TEST(te, "viewer", "tooltips");              /* the hover help actually shows */
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("layout");
        ctx->MouseMove("**/blur r");
        double t0 = ImGui::GetTime();
        while (ImGui::GetTime() - t0 < 1.2) ctx->Yield();        /* the ForTooltip delay is wall-clock */
        ImGuiWindow* tip = ImGui::FindWindowByName("##Tooltip_00");
        IM_CHECK(tip != NULL && tip->WasActive);
        ctx->CaptureScreenshot();
    };

    t = IM_REGISTER_TEST(te, "viewer", "scene_shots");           /* coverage overlay + preview render; screenshots */
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("layout");
        ctx->ItemCheck("**/coverage [C]");
        ctx->Yield(4);                                           /* let the overlay draw */
        ctx->CaptureScreenshot();                                /* whole frame: the 3D scene + panel + HUD */
        ctx->ItemClick("**/Preview - audition [P]");
        ctx->Yield(2);
        IM_CHECK(preview);
        ctx->CaptureReset();                                     /* else the 2nd shot reuses (overwrites) the 1st filename */
        ctx->CaptureScreenshot();
        ctx->ItemClick("**/Back to edit [P]");
        IM_CHECK(!preview);
        ctx->ItemUncheck("**/coverage [C]");
    };
}

/* ============================== main ============================== */

int main(int argc, char** argv) {
    /* headless (no window/audio, scriptable):
     *   --export   [file]            write the layout (default grid, or an existing file with delay_ms recomputed)
     *   --score    [file]            print each panner's rE-localization error for the layout
     *   --optimize [file] [panner]   hill-climb the layout for one panner (dbap|spcap|vbap, default dbap),
     *                                within constraints.json if present, save in place, print before/after
     *   --tests    [filter]          run the imgui_test_engine suite (logic + the real UI) and exit pass/fail */
    bool selftest = false;
    char filter[64] = "";
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("usage: bw_layout_tool [cave_layout.json | mode]\n"
                   "  edit a speaker layout in 3D (default file: ./cave_layout.json;\n"
                   "  ./constraints.json bounds the placement if present)\n"
                   "  --export   [file]                    write the layout headless\n"
                   "  --score    [file]                    print each panner's rE-localization error\n"
                   "  --optimize [file] [dbap|spcap|vbap]  hill-climb within constraints, save in place\n"
                   "  --tests    [filter]                  run the UI test suite and exit pass/fail\n");
            return 0;
        }
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--tests") || !strcmp(argv[i], "--selftest")) {
            selftest = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') snprintf(filter, sizeof filter, "%s", argv[i + 1]);
        }
    int export_only   = (!selftest && argc > 1 && strcmp(argv[1], "--export")   == 0);
    int score_only    = (!selftest && argc > 1 && strcmp(argv[1], "--score")    == 0);
    int optimize_only = (!selftest && argc > 1 && strcmp(argv[1], "--optimize") == 0);
    if (!selftest) {
        const char* p = (export_only || score_only || optimize_only) ? (argc > 2 ? argv[2] : "cave_layout.json")
                                                                     : (argc > 1 ? argv[1] : "cave_layout.json");
        snprintf(g_path, sizeof g_path, "%s", p);
    } else {
        snprintf(g_path, sizeof g_path, "%s", TEST_OUT);   /* never clobber a real layout from the suite */
        srand(42);
    }
    seed_default();
    printf("working dir: %s\n", GetWorkingDirectory());   /* cave_layout.json + constraints.json resolve here (CWD) */
    int loaded = selftest ? 0 : load_json(g_path);
    if (!selftest) {
        if (cv_load("constraints.json", &CON))
            printf("constraints: bounds + %d no-go + %d obstacle box(es) from ./constraints.json\n", CON.nnogo, CON.nobst);
        else
            printf("constraints: none (no ./constraints.json here) — every placement allowed except the y>=0 floor\n");
    }

    /* coverage/scoring shell: even directions on a sphere (Fibonacci) + a working-volume listener grid */
    for (int i = 0; i < NCOV; ++i) {
        float y = 1.0f - 2.0f * ((float)i + 0.5f) / NCOV;
        float r = sqrtf(1.0f - y * y), th = (float)i * 2.39996323f;   /* golden angle */
        cov_dir[i] = Vector3{ r * cosf(th), y, r * sinf(th) };
    }
    cov_lis[0] = Vector3{ 0, 0, 0 };
    { const float ax[3] = { -1.0f, 0.0f, 1.0f }, ay[3] = { -0.3f, 0.0f, 0.3f }; int li = 1;   /* listener-movement envelope */
      for (int xi = 0; xi < 3; ++xi) for (int zi = 0; zi < 3; ++zi) for (int yi = 0; yi < 3; ++yi)
          if (!(ax[xi] == 0 && ay[yi] == 0 && ax[zi] == 0)) cov_lis[li++] = Vector3{ ax[xi], ay[yi], ax[zi] }; }

    if (export_only) {
        if (!save_json(g_path)) { printf("export failed: %s\n", g_path); return 1; }
        printf("exported layout -> %s (from %s)\n", g_path, loaded ? "existing file" : "default grid");
        return 0;
    }
    if (score_only) {                              /* headless: score the layout for each panner + exit */
        printf("layout: %s (%s)\n", g_path, loaded ? "loaded" : "default grid");
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
        if (!save_json(g_path)) { printf("optimize: save failed: %s\n", g_path); return 1; }
        printf("optimized %s for %-5s:  rE mean %.1f -> %.1f deg   worst %.1f -> %.1f deg   (%d iters%s)\n",
               g_path, panner_names[p], m0, m1, w0, w1, opt_iter, CON.loaded ? ", within constraints" : "");
        return 0;
    }
    printf("layout: %s (%s, %d speakers)\n", g_path, loaded ? "loaded" : "default grid", loaded ? loaded : NSPK);

    /* cave profile so the test signal / DBAP preview goes out the 26-ch DVS to the real speakers;
     * falls back to no audio (editor still works) if no 26-ch ASIO device is present (off-site). */
    _putenv((char*)"BWAUDIO_SINK=asio");
    gen_pink_wav(PREV_WAV);                        /* the moving DBAP-preview source signal */
    build_engine(NULL);                            /* edit-mode engine (the test signal is layout-independent) */

    /* No FLAG_WINDOW_HIGHDPI: on Windows the framebuffer matches the window pixels either way, and
     * without the flag rlImGui keeps DisplayFramebufferScale at 1 so screenshots/scissors stay 1:1;
     * DPI rides the theme's FontScaleMain instead (the same pattern as calib_view — and as rlImGui's
     * own default-font path). */
    SetConfigFlags(FLAG_MSAA_4X_HINT | (selftest ? FLAG_WINDOW_UNFOCUSED : 0));
    InitWindow(1280, 800, "bwaudio - speaker layout tool");
    SetExitKey(KEY_NULL);                          /* ESC is handled below (it must not quit while typing) */
    SetTargetFPS(selftest ? 0 : 60);               /* selftest: run the suite unthrottled */
    g_uiScale = GetWindowScaleDPI().y;
    if (g_uiScale > 1.01f) SetWindowSize((int)uiScaled(1280), (int)uiScaled(800));

    rlImGuiBeginInitImGui();                       /* split init: our Roboto must be the only/default font */
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL;                         /* fixed layout; don't scatter imgui.ini */
    loadEmbeddedFont(io);
    applyTheme(false);                             /* the station theme (dark) */
    rlImGuiEndInitImGui();

    g_te = ImGuiTestEngine_CreateContext();
    ImGuiTestEngineIO& teio = ImGuiTestEngine_GetIO(g_te);
    teio.ConfigVerboseLevel        = ImGuiTestVerboseLevel_Warning;
    teio.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Debug;
    teio.ConfigLogToTTY            = selftest;     /* ctest: name each test + why it failed */
    teio.ConfigCaptureEnabled      = true;         /* actually write the screenshots (output/captures/) */
    teio.ConfigRunSpeed            = selftest ? ImGuiTestRunSpeed_Fast : ImGuiTestRunSpeed_Normal;
    teio.ScreenCaptureFunc         = screen_capture;
    ImGuiTestEngine_Start(g_te, ImGui::GetCurrentContext());
    ImGuiTestEngine_InstallDefaultCrashHandler();
    register_tests(g_te);
    if (selftest) ImGuiTestEngine_QueueTests(g_te, ImGuiTestGroup_Tests, filter[0] ? filter : NULL,
                                             ImGuiTestRunFlags_RunFromCommandLine);

    Camera3D cam = {};
    cam.up = Vector3{ 0, 1, 0 }; cam.fovy = 55; cam.projection = CAMERA_PERSPECTIVE;

    scored = 1; score_stale = 1;                   /* per-panner scoreboard on from the start, live-refreshed */
    int frames = 0, drain = 0;
    while (!quit_now) {
        float dt = GetFrameTime();
        const bool kb = !io.WantCaptureKeyboard;   /* imgui typing must not trigger scene shortcuts */
        const bool ms = !io.WantCaptureMouse;      /* the panel/tooltips own the mouse when hovered */
        if (WindowShouldClose() || (kb && IsKeyPressed(KEY_ESCAPE)))
            request_quit();                        /* asks first if there are unsaved edits */
        if (kb && IsKeyPressed(KEY_F11)) ToggleBorderlessWindowed();
        if (kb && IsKeyPressed(KEY_H)) fps_view = !fps_view;     /* first-person view from the observer's ears */
        if (preview) handle_preview_input(dt, kb);
        else         handle_edit_input(dt, kb, ms, cam);

        if (opt_running && !preview) {                    /* auto-optimizer: a few hill-climb trials per frame */
            opt_cost = opt_cost_of((BwPanner)pv_panner);  /* re-baseline so a manual nudge can't wedge the climb */
            optimize_step((BwPanner)pv_panner, 6);
        }
        for (int i = 0; i < NSPK; ++i) if (spk[i].pos.y < 0.0f) spk[i].pos.y = 0.0f;   /* y >= 0: speakers never below the floor */
        if (scored && score_stale && (cov_frame - last_score_frame) >= 10) {
            do_score();                                   /* throttled live re-score (X forces an immediate one) */
            last_score_frame = cov_frame;
        }
        drive_tone();
        if      (save_flash > 0) { save_flash -= dt; if (save_flash < 0) save_flash = 0; }  /* fade out, then STOP at 0 */
        else if (save_flash < 0) { save_flash += dt; if (save_flash > 0) save_flash = 0; }  /* (else it oscillated -> flicker) */
        update_camera(&cam, ms);
        update_title();                                  /* [unsaved] marker in the title bar */
        ++cov_frame;

        BeginDrawing();
        ClearBackground(Color{ 24, 24, 27, 255 });       /* matches the theme's WindowBg */
        float cov_worst, cov_mean;
        draw_scene(cam, &cov_worst, &cov_mean);
        draw_axes_hud(cam, uiScaled(56), (float)GetScreenHeight() - uiScaled(56), uiScaled(30));  /* room axes, bottom-left */

        rlImGuiBegin();
        draw_labels(cam);                                /* background drawlist: under the windows, over the scene */
        draw_hud(cov_worst, cov_mean);
        draw_panel();
        draw_quit_modal();                               /* the unsaved-changes confirm (top level: outlives modes) */
        draw_cov_tooltip(cam, ms);
        if (show_te_ui && g_te) ImGuiTestEngine_ShowTestEngineWindows(g_te, &show_te_ui);
        rlImGuiEnd();

        rlDrawRenderBatchActive();                       /* flush rlgl so a capture sees the whole frame */
        ImGuiTestEngine_PostSwap(g_te);                  /* BEFORE the swap: GL_BACK still holds this frame */
        EndDrawing();

        ++frames;
        if (selftest && frames > 5 && ImGuiTestEngine_IsTestQueueEmpty(g_te) && ++drain > 3) quit_now = 1;
    }

    int rc = 0;
    if (selftest) {
        ImGuiTestEngineResultSummary sum;
        ImGuiTestEngine_GetResultSummary(g_te, &sum);
        printf("[tests] %d/%d passed\n", sum.CountSuccess, sum.CountTested);
        rc = (sum.CountTested == 0 || sum.CountSuccess != sum.CountTested) ? 1 : 0;
    }

    ImGuiTestEngine_Stop(g_te);
    rlImGuiShutdown();                                   /* destroys the imgui context */
    ImGuiTestEngine_DestroyContext(g_te);                /* after DestroyContext, per the te docs */
    CloseWindow();
    if (e) { bw_stop(e); bw_destroy(e); }
    remove(PREV_WAV); remove(TEMP_LAYOUT);
    return rc;
}
