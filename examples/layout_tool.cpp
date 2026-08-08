/*
 * layout_tool.cpp — interactive speaker-layout authoring for the CAVE array.
 *
 * Define where the 26 speakers physically are and export a valid cave_layout.json (the file the
 * engine loads via bwa_desc.layout_path; see docs/layout-schema.md). The killer feature is identify-
 * by-ear: each speaker's INDEX is its bus/output channel, so selecting speaker N and enabling the
 * tone drives that exact channel with the built-in test signal (bwa_set_test_signal) out the cave profile
 * -> the Digiface -> the physical speaker. So the survey loop is: pick an index, enable the tone, walk to
 * whichever speaker sounds, read its position, and place marker N there (nudge, or type exact coords
 * into the panel's position field).
 *
 * On export, delay_ms is auto-computed from the positions (max-distance alignment: the farthest
 * speaker from the origin gets 0, nearer ones wait for it); gain_db is your per-speaker trim. The
 * dbap knobs round-trip. Loads an existing layout (argv[1] or ./cave_layout.json) to keep iterating.
 *
 * Coordinate frame: room space, right-handed, +y up, +z forward, origin ON THE FLOOR at the
 * working-area center in x/z (Motive's default; y = height above the floor) — the same numbers the
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
 * X scores the layout for each panner (mean/worst rE error + the Frank-spread FOCUS metric); O runs
 * the auto-optimizer — a hill-climb over a multi-objective scalarization of the selected panner's
 * scores ("worst wt" blends mean vs worst-case, 1 = pure maximin; "focus wt" adds image spread),
 * subject to the constraints; runs live, O again to stop, then save. A named CONDITION bundles the
 * objective knobs + a scoring-shell elevation band ("horizontal" = only directions near the ear
 * plane count — a collaborator who values planar localization); conditions chain as stages, in the
 * GUI by re-starting the optimizer under the next one, headless via --optimize's stage list. When
 * an allocation is a REQUIREMENT ("spend 12 speakers on the plane"), pin it: "pin to plane" holds
 * a speaker in the ear-plane slab through any stage (objectives compete; constraints hold).
 * K snaps all speakers to the nearest allowed point. Drop a `constraints.json` next to the layout
 * (an allowed `bounds` box + `nogo` boxes for screens/structure + solid `obstacles`) and the tool
 * draws them (green bounds / red no-go / orange solid), flags violating speakers, and K projects
 * them in. H is a first-person view from the observer's ears.
 *
 * UI stack: the 3D room view (orbit + head-view cameras, ray-picked speakers, the coverage shell)
 * stays raylib — it is a *scene*, not a plot — and every control surface (panel, HUD, tooltips) is
 * Dear ImGui rendered on top via rlImGui, themed like the calibration station (bwa_theme.h).
 * imgui_test_engine drives the ACTUAL panel with fake inputs under ctest (`--tests [filter]`),
 * captures screenshots to output/captures/, and exits pass/fail — same harness as bwa_calib_view.
 * Keyboard shortcuts act only when imgui doesn't want the keyboard; mouse picking/camera only when
 * the cursor isn't over the UI (io.WantCapture*).
 *
 * Controls (edit): [ ] select speaker (or left-click)   arrows X/Z, R/F Y (SHIFT = fine)
 *           PgUp/PgDn gain_db   T tone   N sine/noise   C coverage   V observer   G shade metric
 *           X score   B panner   O optimize   K snap   S save   L reload   H head view   F11 fullscreen
 * Controls (preview, P toggles): WASD/RF move the source   SPACE auto-orbit/near-far/high-low sweep
 *           B A/B the panner DBAP<->SPCAP<->VBAP live   Common: right-drag/wheel camera
 *           ESC / close quits — with a confirm (Save and quit / Quit / Cancel) if there are unsaved edits
 * Build: cmake -S . -B build -DBWA_BUILD_PLAYGROUND=ON && cmake --build build --target bwa_layout_tool
 */
#include "bw_audio.h"
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"            /* rlDrawRenderBatchActive: flush the 3D batch before a screenshot */
#include "speaker_gizmo.h"   /* the "real speaker" glyph (cabinet + cone aimed at the listener) */
#include "cJSON.h"
#include "sos.h"             /* BWA_SOS_REF_MPS: the delay-alignment speed of sound, layout-carried */
#include "constraints_view.h"   /* constraints.json load + box drawing, shared with the playground */
#include "axes_hud.h"        /* screen-corner XYZ triad, shared with the playground */

#include "imgui.h"
#include "rlImGui.h"
#include "bwa_theme.h"        /* station theme + embedded Roboto (applyTheme / loadEmbeddedFont / uiScaled) */
#include "imgui_te_engine.h"
#include "imgui_te_context.h"
#include "imgui_te_ui.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NSPK           26         /* array CAPACITY (== BWA_CHANNELS); g_nspk is the layout's ACTUAL count */
#define NSPK_MIN       4          /* the engine's layout loader accepts 4..NSPK speakers */
#define SR             48000u
/* Speed of sound for the delay-alignment derivation below. Seeded from the layout's
 * reference.speed_of_sound_mps (see sos.h) so a rig surveyed at its own temperature keeps its own c
 * rather than silently re-deriving delays at 20 C. 0.6% of a 3 ms alignment delay is 0.02 ms, which
 * is inaudible — this is here so the tool AGREES with bwa_calibrate on one file, not because the
 * delays need the precision. */
static float speed_of_sound = (float)BWA_SOS_REF_MPS;
#define PANEL_W        300.0f     /* control panel width (right side), in unscaled UI px */

typedef struct { Vector3 pos; float gain_db; int pin; } Spk;   /* pin: 1 = held to the ear-plane slab */
static Spk spk[NSPK];
static int g_nspk = NSPK;         /* speakers in the edited layout (4..NSPK) — the engine's channel count */

/* dbap knobs (round-tripped through the file; defaults match layout_default) */
static float       dbap_r = 0.5f, dist_ref = 1.0f, dist_rolloff = 1.0f, dist_min_db = -40.0f;
static const char* dist_model = "inverse";
static float       obs_height = 1.4f;      /* observer EAR height above the floor origin (m; ~4.6 ft) — the
                                              listener/scoring point + the line-of-sight source sit here, not at y=0 */
/* Speaker ALLOCATION as a constraint: a pinned speaker is confined to a slab about the ear plane
 * (|y - obs_height| <= pin_slab_m), enforced by the optimizer's trials and K snap. Objectives
 * compete — a later 3d stage happily pulls plane speakers back toward elevation — so "spend 12 on
 * the plane" must be a pin, not a weight. Round-trips through the layout file ("pin": "plane" per
 * speaker + pin_slab_m top-level); the engine's loader ignores both. */
static float       pin_slab_m = 0.3f;
/* Psychophysics: human localization is anisotropic — horizontal (azimuth) acuity is far finer than
 * vertical. The minimum audible angle is ~1 deg for a frontal azimuth displacement (Mills, JASA 1958)
 * vs ~3-4 deg for a vertical one (Perrott & Saberi, JASA 1990); 2-D localization scatter is likewise
 * ~2x larger in elevation than azimuth (Makous & Middlebrooks, JASA 1990), and median-plane blur is
 * coarser still (Blauert, Spatial Hearing, 1997). So azimuth is ~3-4x more resolvable than elevation:
 * split the localization error into azimuth + elevation and DOWN-weight elevation (elev_wt ~ 1/3.5), so
 * the optimizer trades vertical accuracy for horizontal, matching what a listener actually notices.
 * (elev_wt is a modeling choice from that ratio, not a value lifted from any single paper.) */
static int         perceptual = 1;         /* weight azimuth over elevation in the rE error */
static float       elev_wt    = 0.3f;      /* elevation-error weight vs azimuth (~1/3.5; slider 0..1) */
/* Scoring-shell elevation band: 0 = the full sphere; >0 keeps only source directions within that many
 * degrees of the ear plane. This is what "localization on the 2D plane" MEANS as an objective — not
 * elev_wt = 0, which still spends effort on the azimuth of overhead sources (where azimuth is nearly
 * meaningless). Applies to score_panner (the scoreboard + the optimizer's cost); the coverage/badness
 * overlays keep the full shell so the view never hides what a condition ignores. */
static float       shell_band_deg = 0.0f;
/* Azimuth wedge: 0 = all azimuths; >0 keeps only source directions within that many degrees of the
 * room's FORWARD (+z, the same facing convention as the head view). This is the "visual area"
 * objective: spend the array's accuracy where the listener looks. It bakes in one canonical facing,
 * so it fits an install with a dominant screen direction, not a turn-anywhere CAVE. */
static float       shell_azi_deg  = 0.0f;

/* speaker `i` of `n` on a hemisphere DOME (y >= 0): a Fibonacci half-sphere, radius ~2.4 m — the
 * listener sits under it. Even angular spread with no floor-crossing speakers (the CAVE floor is a
 * screen). The starting layout, and where a speaker added by the count control lands. */
static Vector3 dome_pos(int i, int n) {
    const float R = 2.4f, golden = 2.39996323f;   /* golden angle */
    float y  = ((float)i + 0.5f) / (float)n;      /* 0..1, all >= 0 (upper hemisphere) */
    float r  = sqrtf(1.0f - y * y);
    float th = golden * (float)i;
    return Vector3{ R * r * cosf(th), R * y, R * r * sinf(th) };
}
static void seed_default(void) {
    for (int i = 0; i < g_nspk; ++i) { spk[i].pos = dome_pos(i, g_nspk); spk[i].gain_db = 0.0f; spk[i].pin = 0; }
}

/* load positions/gain + dbap knobs from an existing cave_layout.json; returns #speakers read (0 = none).
 * The file's speaker COUNT becomes the edited layout's count (4..NSPK — the engine loader's range). */
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
        int n = cJSON_GetArraySize(spks);
        if (n < NSPK_MIN || n > NSPK) { cJSON_Delete(root); return 0; }   /* the engine would reject it too */
        g_nspk = n;
        cJSON* sp;
        cJSON_ArrayForEach(sp, spks) {
            cJSON* idxj = cJSON_GetObjectItemCaseSensitive(sp, "index");
            cJSON* posj = cJSON_GetObjectItemCaseSensitive(sp, "position");
            if (!cJSON_IsNumber(idxj) || !cJSON_IsArray(posj) || cJSON_GetArraySize(posj) != 3) continue;
            int idx = idxj->valueint;
            if (idx < 0 || idx >= g_nspk) continue;   /* indices are a 0..count-1 permutation */
            cJSON* px = cJSON_GetArrayItem(posj, 0);
            cJSON* py = cJSON_GetArrayItem(posj, 1);
            cJSON* pz = cJSON_GetArrayItem(posj, 2);
            if (!cJSON_IsNumber(px) || !cJSON_IsNumber(py) || !cJSON_IsNumber(pz)) continue;  /* skip non-numeric */
            spk[idx].pos.x = (float)px->valuedouble;
            spk[idx].pos.y = (float)py->valuedouble;
            spk[idx].pos.z = (float)pz->valuedouble;
            cJSON* g = cJSON_GetObjectItemCaseSensitive(sp, "gain_db");
            spk[idx].gain_db = cJSON_IsNumber(g) ? (float)g->valuedouble : 0.0f;
            cJSON* pj = cJSON_GetObjectItemCaseSensitive(sp, "pin");
            spk[idx].pin = (cJSON_IsString(pj) && strcmp(pj->valuestring, "plane") == 0) ? 1 : 0;
            ++loaded;
        }
    }
    cJSON* psj = cJSON_GetObjectItemCaseSensitive(root, "pin_slab_m");
    if (cJSON_IsNumber(psj)) pin_slab_m = (float)psj->valuedouble;
    /* reference.ears_m: the listening-point height this file's delay_ms was derived at. Resume at
     * the file's OWN anchor rather than the 1.4 default, so reopening a 1.2 m layout and saving
     * can't silently re-align every delay to a plane 20 cm too high. A CLI ears= parses after the
     * load (main) and still wins. Same 0..3 m guard the CLI applies. */
    cJSON* refj = cJSON_GetObjectItemCaseSensitive(root, "reference");
    if (cJSON_IsObject(refj)) {
        cJSON* ej = cJSON_GetObjectItemCaseSensitive(refj, "ears_m");
        if (cJSON_IsNumber(ej) && ej->valuedouble > 0.0 && ej->valuedouble <= 3.0)
            obs_height = (float)ej->valuedouble;
        /* same idea for the rig's speed of sound: bwa_calibrate records the temperature it surveyed
         * at, and re-deriving delays here at 20 C would quietly disagree with it. */
        cJSON* cj = cJSON_GetObjectItemCaseSensitive(refj, "speed_of_sound_mps");
        if (cJSON_IsNumber(cj) && cj->valuedouble >= BWA_SOS_MIN_MPS && cj->valuedouble <= BWA_SOS_MAX_MPS)
            speed_of_sound = (float)cj->valuedouble;
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
    for (int i = 0; i < g_nspk; ++i) { float d = Vector3Distance(spk[i].pos, ear); if (d > dmax) dmax = d; }
    fprintf(f,
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"units\": { \"position\": \"meters\", \"gain\": \"decibels\", \"delay\": \"milliseconds\" },\n"
        "  \"coordinate_space\": \"room, right-handed, +y up, +z forward (matches OptiTrack/Motive default); origin ON THE FLOOR at the working-area center (x/z); y = height above the floor\",\n"
        "  \"reference\": { \"alignment\": \"max-distance\", \"ears_m\": %g, \"speed_of_sound_mps\": %g, \"note\": \"delay_ms time-aligns each speaker arrival to the farthest speaker, heard at ears_m above the floor; gain_db is a measured per-speaker trim\" },\n"
        "  \"dbap\": { \"rolloff_r\": %g, \"distance_attenuation\": { \"model\": \"%s\", \"reference_distance_m\": %g, \"rolloff\": %g, \"min_gain_db\": %g } },\n",
        (double)obs_height, (double)speed_of_sound, dbap_r, dist_model, dist_ref, dist_rolloff, dist_min_db);
    int npin = 0;
    for (int i = 0; i < g_nspk; ++i) npin += spk[i].pin;
    if (npin) fprintf(f, "  \"pin_slab_m\": %g,\n", pin_slab_m);   /* authoring-only, like \"pin\"; the engine ignores it */
    fprintf(f, "  \"speakers\": [\n");
    for (int i = 0; i < g_nspk; ++i) {
        float d = Vector3Distance(spk[i].pos, ear);
        float delay_ms = (dmax - d) / speed_of_sound * 1000.0f;
        fprintf(f, "    { \"index\": %d, \"position\": [%.4f, %.4f, %.4f], \"gain_db\": %.2f, \"delay_ms\": %.3f%s }%s\n",
                i, spk[i].pos.x, spk[i].pos.y, spk[i].pos.z, spk[i].gain_db, delay_ms,
                spk[i].pin ? ", \"pin\": \"plane\"" : "", (i < g_nspk - 1) ? "," : "");
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
/* the pin constraint: a pinned speaker lives in the ear-plane slab. Applied BEFORE
 * constraint_project so the box constraints keep the last word on feasibility. */
static Vector3 pin_project(Vector3 p, int pin) {
    if (pin) p.y = Clamp(p.y, fmaxf(0.0f, obs_height - pin_slab_m), obs_height + pin_slab_m);
    return p;
}
/* ---- engine + DBAP preview state (file scope) ---- */
#define PREV_WAV    "._bw_layout_preview.wav"
#define TEMP_LAYOUT "._bw_layout_preview.json"
#define SRC_GAIN    0.7f
static bwa_engine*   e;
static int         audio;
static const char* backend = "none";
static bwa_sound     pv_sound;
static bwa_source    pv_src;
static int         preview, pv_orbit, layout_dirty = 1;   /* dirty=1 forces the first preview to rebuild from spk[] */
static int         pv_panner;                             /* 0=DBAP 1=SPCAP 2=VBAP (= bwa_panner; live A/B, B key) */
/* SPCAP's live tuning exponents (bwa_set_spcap_focus). 0 on either = its default: focus derived from
 * the edited geometry, density 2.0. pv_focus_def is what the CURRENT spk[] implies, recomputed on
 * every edit so you can see the geometry's own answer before overriding it.
 * They drive BOTH sides of the tool: the audible preview through bwa_set_spcap_focus, and every
 * SCORE (scoreboard, --score, the rE overlay, the badness map, the optimizer cost) through
 * bwa_panner_gains_batch's focus/density arguments. Same sentinel on both, so 0 means the same
 * thing to the ear and to the number. */
static float       pv_focus, pv_density, pv_focus_def;
static const char* panner_names[] = { "DBAP (moving)", "SPCAP (fixed)", "VBAP (fixed)" };
static float       pv_t;
static Vector3     src_pos = { 1.5f, 0.0f, 0.0f };

/* ---- coverage overlay: angular gap to the nearest speaker, over a shell of source directions ----
 * A geometric proxy for DBAP localization: a direction with no nearby speaker forces the pan to
 * spread energy to far-off-axis speakers (a hole). Shades each direction green (covered) -> red (gap).
 * coverage_moving toggles the observer model: fixed = the center sweet spot; moving = mean over a
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

/* a ~2 s mono 16-bit pink-noise loop for the preview source (broadband -> localizes well) */
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
    /* demand a real device (BWA_SINK_ASIO): the audition drives REAL speakers — silence must fail
     * loudly, not hide behind the null sink. A failed start leaves the editor running (below). */
    bwa_desc cfg = { };
    cfg.profile = BWA_PROFILE_CAVE; cfg.layout_path = layout_path;
    cfg.sample_rate = SR; cfg.block_size = 256;
    cfg.sink = BWA_SINK_ASIO;
    e = bwa_create(&cfg);
    backend = "none"; audio = 0; pv_sound = 0; pv_src = 0;
    if (!e) return;
    if (bwa_start(e) != 0) {
        const char* err = bwa_last_error(e);
        printf("bwa_start: %s - no audition (needs an ASIO device with an output per speaker); "
               "the editor still runs.\n", err ? err : "?");
    }
    backend = bwa_get_audio_backend(e);
    audio = (strncmp(backend, "asio", 4) == 0);
    pv_sound = bwa_load_sound(e, PREV_WAV);
    pv_src   = bwa_source_create(e);
    bwa_source_play(e, pv_src, pv_sound, true);
    bwa_source_set_gain(e, pv_src, 0.0f);          /* silent until preview mode */
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
static float score_spread[3];                    /* mean perceived spread (deg): Frank 2013's 186.4·(1−|rE|)+10.7 */
static float score_bed_mean, score_bed_worst, score_bed_spread;   /* the AMBI-bed row */
/* The bed row evaluates the decode the INSTALL ships: bwa_desc.bed_decoder (AllRAD default, EPAD
 * opt-in) and bwa_set_max_re. Scoring a different decoder than the rig runs would grade the wrong
 * render - same trap class as scoring only at the sweet spot. CLI: `epad` / `maxre` tokens. */
static int bed_decoder = 0;    /* UI index: 0 = AllRAD, 1 = EPAD. NOT the bwa_bed_decoder value -
                                * that enum reserves 0 for default-init, so map before passing. */
static int bed_max_re  = 1;    /* tracks the ENGINE default, which flipped ON after the offline
                                * bake-off (rt.c). A scorer whose default disagrees with the engine
                                * grades a render nobody ships - the trap this comment block names. */
static int   scored, score_stale, last_score_frame;   /* the per-panner scoreboard auto-refreshes on a throttle */

/* SOLVE position vs EVALUATION position — the distinction this scoring turns on.
 *
 * A panner solves its gains for SOME listener position, and a listener then hears the result from
 * wherever they actually are. Those are the same point only for a tracked renderer. A fixed install
 * solves once, at the sweet spot, and every step the listener takes away from it is un-corrected.
 * Scoring such a layout AT the sweet spot — which is what this tool used to do for SPCAP and VBAP —
 * cannot see that degradation at all: it optimizes a single point and reports a number that is
 * structurally incapable of moving. bwa_validate measures the same contrast acoustically and finds
 * it large (SPCAP +10 deg, VBAP +5 deg at 0.7 m off-center), so it is not a rounding error.
 *
 *   tracked = 1  solve at the listener's own position (what DBAP does per block)
 *   tracked = 0  solve at the sweet spot, listen from elsewhere (what a fixed install does)
 *
 * Sources therefore sit at FIXED WORLD positions off the sweet spot rather than following the
 * listener: only then is every listener position judged against the same physical sources, which is
 * what makes an off-center cell comparable to a centered one. At the sweet spot the two modes are the
 * same render, and the tests pin exactly that. */
static int track_mode = 0;      /* 0 = auto (DBAP tracked, SPCAP/VBAP fixed), 1 = force tracked, 2 = force fixed */
/* The observer model is the other half of a per-install objective: this CAVE's listener roams
 * (score over the 27-point grid, the default), but a seated install listens from the sweet spot
 * only, and scoring it over the roam punishes off-center cells it will never occupy. Headless
 * `fixed` token; the GUI keeps the moving model (this installation's case). */
static int score_fixed_obs = 0; /* 1 = evaluate at the sweet spot only (seated install) */
static int panner_tracked(bwa_panner p) {
    return (track_mode == 1) ? 1 : (track_mode == 2) ? 0 : (p == BWA_PAN_DBAP);
}

/* Default image-FOCUS weight per panner, from bwa_validate's proxy-vs-measurement correlation on
 * this array: against the acoustic measurement, rE DIRECTION error ranks DBAP cells well
 * (Spearman ~0.8) but VBAP cells barely at all (~0.2), where the Frank SPREAD is the strong
 * predictor (~0.77). So the axis worth optimizing is not the same for every panner, and a single
 * global default would be wrong for two of the three. Overridable — it is a starting point. */
static float panner_focus_default(bwa_panner p) {
    return (p == BWA_PAN_DBAP) ? 0.0f : (p == BWA_PAN_SPCAP) ? 0.5f : 1.0f;
}

/* mean + worst rE localization error (deg) over the shell, evaluated across the WHOLE listener grid
 * for every panner (see the solve/eval note above). Uses bwa_panner_gains_batch (the ACTUAL engine
 * solve), so the score reflects what will ship — not a re-implementation. spread_deg (optional) is
 * the mean perceived source width from the energy-vector MAGNITUDE (Frank 2013: ≈186.4°·(1−|rE|)
 * + 10.7°) — the image-focus axis direction error alone can't see: a defocused-but-centered image
 * scores 0° error. Under SPCAP it solves at the dialed pv_focus/pv_density (0 = this array's
 * derived default), so the score grades the tuning you will ship, not a fixed one. */
static void score_panner(bwa_panner panner, int stride, int tracked,
                         float* mean_deg, float* worst_deg, float* spread_deg) {
    static float gains[NCOV * NSPK], srcs[NCOV * 3];
    float pos[NSPK * 3];
    for (int i = 0; i < g_nspk; ++i) { pos[i*3] = spk[i].pos.x; pos[i*3+1] = spk[i].pos.y; pos[i*3+2] = spk[i].pos.z; }
    if (stride < 1) stride = 1;                     /* >1 subsamples the direction shell (coarse, for the optimizer) */

    Vector3 S = { 0.0f, obs_height, 0.0f };         /* the sweet spot: where a fixed install solves */
    /* cov_dir is unit, so |y| = sin(elevation): the band keeps directions near the ear plane */
    float band_sin = (shell_band_deg > 0.0f) ? sinf(shell_band_deg * DEG2RAD) : 2.0f;
    float azi_rad  = shell_azi_deg * DEG2RAD;       /* wedge about +z (room forward); 0 = all azimuths */
    int ns = 0;                                     /* sources are world-fixed, so build them ONCE */
    for (int i = 0; i < NCOV; i += stride) {
        if (fabsf(cov_dir[i].y) > band_sin) continue;
        if (azi_rad > 0.0f && fabsf(atan2f(cov_dir[i].x, cov_dir[i].z)) > azi_rad) continue;
        srcs[ns*3]   = S.x + COV_R * cov_dir[i].x;
        srcs[ns*3+1] = S.y + COV_R * cov_dir[i].y;
        srcs[ns*3+2] = S.z + COV_R * cov_dir[i].z;
        ++ns;
    }
    double sumerr = 0, sumspread = 0; float worst = 0; int cnt = 0;
    for (int l = 0; l < (score_fixed_obs ? 1 : 27); ++l) {     /* [0] is the sweet spot */
        Vector3 Lp = cov_lis[l]; Lp.y += obs_height;    /* the listener's EARS are at obs_height, not the floor */
        /* the SOLVE position; the listener stays Lp, and the two differ whenever tracked == 0 */
        float solvef[3] = { tracked ? Lp.x : S.x, tracked ? Lp.y : S.y, tracked ? Lp.z : S.z };
        bwa_panner_gains_batch(panner, pos, (uint32_t)g_nspk, solvef, srcs, ns, pv_focus, pv_density, gains);
        for (int j = 0; j < ns; ++j) {
            float* g = &gains[j * g_nspk];
            float rE[3] = { 0, 0, 0 }, esum = 0;
            for (int s = 0; s < g_nspk; ++s) {      /* energy-weighted speaker-direction vector (rE) */
                float w = g[s] * g[s];
                Vector3 sd = Vector3Normalize(Vector3Subtract(spk[s].pos, Lp));
                rE[0] += w * sd.x; rE[1] += w * sd.y; rE[2] += w * sd.z;
                esum  += w;
            }
            float rl = sqrtf(rE[0]*rE[0] + rE[1]*rE[1] + rE[2]*rE[2]);
            if (rl < 1e-9f || esum < 1e-12f) continue;
            Vector3 ev = { rE[0]/rl, rE[1]/rl, rE[2]/rl };
            /* the direction the source ACTUALLY subtends from this listener — not the shell
             * direction, which is only the same thing at the sweet spot */
            Vector3 tgt = Vector3Normalize(Vector3Subtract(
                Vector3{ srcs[j*3], srcs[j*3+1], srcs[j*3+2] }, Lp));
            float err = loc_err_deg(tgt, ev);          /* perceptually weighted (azimuth >> elevation) */
            float re_mag = rl / esum;                  /* |rE| in [0,1]: 1 = a single speaker carries it */
            if (re_mag > 1.f) re_mag = 1.f;
            sumspread += 186.4 * (1.0 - re_mag) + 10.7;   /* Frank 2013 perceived-spread model */
            sumerr += err; if (err > worst) worst = err; ++cnt;
        }
    }
    /* an EMPTY evaluation (a band/azi shell so narrow the stride skips every direction, or a layout
     * with no energy anywhere) must read as PESSIMAL, not perfect — a 0 here would send the
     * optimizer hunting for shells it cannot see */
    *mean_deg  = cnt ? (float)(sumerr / cnt) : 90.f;
    *worst_deg = cnt ? worst : 180.f;
    if (spread_deg) *spread_deg = cnt ? (float)(sumspread / cnt) : 197.1f;
}

/* ---- bed (ambisonic) scoring: what DIFFUSE/bed content wants from the layout ----
 * A bed decodes SH->speakers through a layout-FIXED matrix (AllRAD, the engine default; via
 * bwa_bed_gains_batch = the engine's actual build), so it is fixed-solve by construction and there
 * is no tracked variant. Content is plane waves at infinity: the target direction is the WORLD
 * direction from every listener, not a positioned source. A layout that scores well here is a good
 * quadrature of the sphere - a property the point-source panners cannot see (they want coverage/
 * triangulation; a bed wants uniformity). Shares the condition shell (band/azi) and observer model. */
static void score_bed(int stride, float* mean_deg, float* worst_deg, float* spread_deg) {
    static float gains[NCOV * NSPK], dirs[NCOV * 3];
    float pos[NSPK * 3];
    for (int i = 0; i < g_nspk; ++i) { pos[i*3] = spk[i].pos.x; pos[i*3+1] = spk[i].pos.y; pos[i*3+2] = spk[i].pos.z; }
    if (stride < 1) stride = 1;
    float band_sin = (shell_band_deg > 0.0f) ? sinf(shell_band_deg * DEG2RAD) : 2.0f;
    float azi_rad  = shell_azi_deg * DEG2RAD;
    int nd = 0;
    for (int i = 0; i < NCOV; i += stride) {
        if (fabsf(cov_dir[i].y) > band_sin) continue;
        if (azi_rad > 0.0f && fabsf(atan2f(cov_dir[i].x, cov_dir[i].z)) > azi_rad) continue;
        dirs[nd*3] = cov_dir[i].x; dirs[nd*3+1] = cov_dir[i].y; dirs[nd*3+2] = cov_dir[i].z;
        ++nd;
    }
    bwa_bed_gains_batch(bed_decoder ? BWA_DECODE_EPAD : BWA_DECODE_ALLRAD, bed_max_re != 0,
                        pos, (uint32_t)g_nspk, dirs, (uint32_t)nd, gains);
    double sumerr = 0, sumspread = 0; float worst = 0; int cnt = 0;
    for (int l = 0; l < (score_fixed_obs ? 1 : 27); ++l) {
        Vector3 Lp = cov_lis[l]; Lp.y += obs_height;
        for (int j = 0; j < nd; ++j) {
            float* g = &gains[j * g_nspk];
            float rE[3] = { 0, 0, 0 }, esum = 0;
            for (int s = 0; s < g_nspk; ++s) {
                float w2 = g[s] * g[s];
                Vector3 sd = Vector3Normalize(Vector3Subtract(spk[s].pos, Lp));
                rE[0] += w2 * sd.x; rE[1] += w2 * sd.y; rE[2] += w2 * sd.z;
                esum  += w2;
            }
            float rl = sqrtf(rE[0]*rE[0] + rE[1]*rE[1] + rE[2]*rE[2]);
            if (rl < 1e-9f || esum < 1e-12f) continue;
            Vector3 ev  = { rE[0]/rl, rE[1]/rl, rE[2]/rl };
            Vector3 tgt = { dirs[j*3], dirs[j*3+1], dirs[j*3+2] };   /* infinity: the same from everywhere */
            float err = loc_err_deg(tgt, ev);
            float re_mag = rl / esum;
            if (re_mag > 1.f) re_mag = 1.f;
            sumspread += 186.4 * (1.0 - re_mag) + 10.7;
            sumerr += err; if (err > worst) worst = err; ++cnt;
        }
    }
    *mean_deg  = cnt ? (float)(sumerr / cnt) : 90.f;   /* empty shell = pessimal, same as score_panner */
    *worst_deg = cnt ? worst : 180.f;
    if (spread_deg) *spread_deg = cnt ? (float)(sumspread / cnt) : 197.1f;
}

/* fill cov_err[] with the selected panner's per-direction rE error (deg), averaged over the observer
 * model score_panner uses (DBAP: the moving grid when coverage_moving; SPCAP/VBAP: the fixed center).
 * Same real solve as the X-score — this is its per-direction breakdown, for the overlay. */
static void compute_cov_err(bwa_panner panner) {
    static float gains[NCOV * NSPK], srcs[NCOV * 3];
    float pos[NSPK * 3];
    for (int i = 0; i < g_nspk; ++i) { pos[i*3]=spk[i].pos.x; pos[i*3+1]=spk[i].pos.y; pos[i*3+2]=spk[i].pos.z; }
    int NL = (panner == BWA_PAN_DBAP && coverage_moving) ? 27 : 1;
    for (int i = 0; i < NCOV; ++i) cov_err[i] = 0.0f;
    for (int l = 0; l < NL; ++l) {
        Vector3 Lp = cov_lis[l]; Lp.y += obs_height;    /* ears at obs_height */
        float lisf[3] = { Lp.x, Lp.y, Lp.z };
        for (int i = 0; i < NCOV; ++i) {
            srcs[i*3]=Lp.x+COV_R*cov_dir[i].x; srcs[i*3+1]=Lp.y+COV_R*cov_dir[i].y; srcs[i*3+2]=Lp.z+COV_R*cov_dir[i].z;
        }
        bwa_panner_gains_batch(panner, pos, (uint32_t)g_nspk, lisf, srcs, NCOV, pv_focus, pv_density, gains);
        for (int i = 0; i < NCOV; ++i) {
            float* g = &gains[i * g_nspk];
            float rE[3] = { 0, 0, 0 };
            for (int s = 0; s < g_nspk; ++s) {
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

/* ---- volumetric BADNESS MAP: where in the ROOM does this layout work? ----
 *
 * The coverage shell answers "which DIRECTIONS work, from one listening point". This answers the
 * question a walking-listener installation actually has: "where can somebody STAND". Same solve,
 * transposed — a grid of listener positions through the working volume, each scored over a coarse
 * spread of directions, drawn as semitransparent voxels. Good regions fade out and bad ones light
 * up, so the shape of the usable volume is the thing you see.
 *
 * It is worth toggling `tracked` while looking at it: a fixed solve draws a bright island around the
 * sweet spot decaying outward, a tracked one stays flat. That contrast is the whole DBAP-vs-SPCAP
 * argument in one picture, and no table makes it as obvious.
 *
 * The metric follows the panner for the reason panner_focus_default explains — direction error is
 * what predicts the acoustic measurement for DBAP, spread is what predicts it for VBAP. Rendering
 * the wrong one would draw a confident, misleading map. The acoustic measurement itself (an FFT per
 * point, via bwa_validate) is far too slow to paint interactively; this is the cheap proxy it
 * vouched for. */
#define BAD_NX     9
#define BAD_NY     5
#define BAD_NZ     9
#define NBAD       (BAD_NX * BAD_NY * BAD_NZ)
#define BAD_STRIDE 8                       /* ~48 of the 380 shell directions per point */
#define BAD_HALF   1.5f                    /* working volume half-extent in x/z (m) */
#define BAD_VHALF  0.5f                    /* ...and in HEIGHT: seated to tall about the ear plane */
static int     bad_on, bad_valid, bad_stale, bad_panner = -1, bad_tracked = -1;
static int     bad_metric;                 /* 0 = rE direction error, 1 = Frank spread */
static Vector3 bad_pos[NBAD];
static float   bad_val[NBAD];
static float   bad_lo, bad_hi, bad_mean;

static void compute_badness(bwa_panner panner, int tracked) {
    static float gains[NCOV * NSPK], srcs[NCOV * 3];
    float pos[NSPK * 3];
    for (int i = 0; i < g_nspk; ++i) { pos[i*3]=spk[i].pos.x; pos[i*3+1]=spk[i].pos.y; pos[i*3+2]=spk[i].pos.z; }

    Vector3 S = { 0.0f, obs_height, 0.0f };
    int ns = 0;
    for (int i = 0; i < NCOV; i += BAD_STRIDE) {
        srcs[ns*3]   = S.x + COV_R * cov_dir[i].x;
        srcs[ns*3+1] = S.y + COV_R * cov_dir[i].y;
        srcs[ns*3+2] = S.z + COV_R * cov_dir[i].z;
        ++ns;
    }
    int w = 0;
    double macc = 0.0;
    bad_lo = 1e9f; bad_hi = -1e9f;
    for (int iy = 0; iy < BAD_NY; ++iy)
        for (int iz = 0; iz < BAD_NZ; ++iz)
            for (int ix = 0; ix < BAD_NX; ++ix) {
                Vector3 Lp = {
                    -BAD_HALF + 2.0f*BAD_HALF * (float)ix / (float)(BAD_NX - 1),
                    obs_height - BAD_VHALF + 2.0f*BAD_VHALF * (float)iy / (float)(BAD_NY - 1),
                    -BAD_HALF + 2.0f*BAD_HALF * (float)iz / (float)(BAD_NZ - 1)
                };
                bad_pos[w] = Lp;
                float solvef[3] = { tracked ? Lp.x : S.x, tracked ? Lp.y : S.y, tracked ? Lp.z : S.z };
                bwa_panner_gains_batch(panner, pos, (uint32_t)g_nspk, solvef, srcs, ns, pv_focus, pv_density, gains);
                double acc = 0.0; int cnt = 0;
                for (int j = 0; j < ns; ++j) {
                    float* g = &gains[j * g_nspk];
                    float rE[3] = { 0, 0, 0 }, esum = 0;
                    for (int s = 0; s < g_nspk; ++s) {
                        float ww = g[s] * g[s];
                        Vector3 sd = Vector3Normalize(Vector3Subtract(spk[s].pos, Lp));
                        rE[0] += ww*sd.x; rE[1] += ww*sd.y; rE[2] += ww*sd.z;
                        esum  += ww;
                    }
                    float rl = sqrtf(rE[0]*rE[0] + rE[1]*rE[1] + rE[2]*rE[2]);
                    if (rl < 1e-9f || esum < 1e-12f) continue;
                    if (bad_metric == 0) {
                        Vector3 ev  = { rE[0]/rl, rE[1]/rl, rE[2]/rl };
                        Vector3 tgt = Vector3Normalize(Vector3Subtract(
                            Vector3{ srcs[j*3], srcs[j*3+1], srcs[j*3+2] }, Lp));
                        acc += loc_err_deg(tgt, ev);
                    } else {
                        float m = rl / esum;
                        if (m > 1.f) m = 1.f;
                        acc += 186.4 * (1.0 - m) + 10.7;
                    }
                    ++cnt;
                }
                bad_val[w] = cnt ? (float)(acc / cnt) : 0.f;
                if (bad_val[w] < bad_lo) bad_lo = bad_val[w];
                if (bad_val[w] > bad_hi) bad_hi = bad_val[w];
                macc += bad_val[w];
                ++w;
            }
    bad_mean = (float)(macc / NBAD);
    bad_valid = 1; bad_stale = 0; bad_panner = panner; bad_tracked = tracked;
}

/* ---- auto-optimizer: stochastic hill-climb over the free positions, minimizing a MULTI-OBJECTIVE
 * scalarization subject to the constraints: (1−w)·mean + w·worst rE error blends the average
 * experience against the worst direction/observer position (w = 1 is pure MAXIMIN — nothing is
 * sacrificed for the average, the right target when every occupant matters; cf. Yang et al. 2025,
 * Acoustics 7(4), who formalize that both can't be optimal at once for off-center listeners), plus
 * an optional image-FOCUS term (mean Frank spread) direction error alone can't see. Each weight
 * setting climbs to a different point on the accuracy/robustness Pareto front. Runs incrementally
 * (a few trials per frame) so the layout is seen converging; stop any time and save. ---- */
static int     opt_running, opt_iter, opt_stall;
static float   opt_step = 0.30f, opt_cost;
static float   opt_leash = 3.0f;                          /* max optimizer displacement from the anchor (m); ~free at 3 m */
static int     opt_radial = 0;                            /* trials move only ALONG the ear ray: directions (a panner's
                                                           * triangulation) frozen, radii free - the cross-panner refit
                                                           * pass ("optimize for vbap, then a dbap pass for radii") */
/* Cross-panner GUARD: while climbing the target panner, a trial is additionally rejected if the
 * guard panner's cost slips more than opt_guard_tol above its value at optimize start. This is the
 * constrained form of "best VBAP layout that never lets DBAP slip" - two objectives cannot both be
 * climbed, but one can be climbed inside the other's feasible set. The guard is evaluated under the
 * SAME condition/weights as the climb (its own tracked mode), and only for trials that already
 * improve the target, so the extra cost is small. Start FROM a layout already optimized for the
 * guard panner, or the baseline it protects is a weak one. */
static int     opt_guard_panner = -1;                     /* -1 = off; else bwa_panner to protect */
static float   opt_guard_tol = 0.5f;                      /* allowed slip, in cost units (~degrees) */
static float   opt_guard_base;                            /* guard cost captured at optimize start */
/* Search upgrades over the greedy climb, both opt-in. The measured motivation: the objective is
 * multimodal (identical inputs landed 26.8 vs 31.2 deg worst on different rand paths, and stiff
 * seeds barely improved at all), and a strictly greedy walk cannot cross a cost barrier.
 *  - BEST-SO-FAR is always tracked and shipped: a run can never end worse than its best moment.
 *    (This is what makes uphill acceptance safe to stop at any time.)
 *  - `anneal` = Metropolis acceptance: an uphill move of cost slip d is accepted with probability
 *    exp(-d/T); T starts at SA_T0 and cools a little every trial, so the walk explores early and
 *    freezes into a basin late. The step floor still terminates the climb.
 *  - `restarts=<n>` = basin hopping: re-climb from the best layout plus a 0.25 m kick. Restarts
 *    diversify NEAR the seed's basin; they do not invent a new structure (seeds still dominate). */
#define SA_T0      0.75f                                  /* initial temperature (cost units ~degrees) */
#define SA_DECAY   0.9997f                                /* per-trial cooling */
static int     opt_sa = 0, opt_restarts = 1;
static float   opt_sa_t = 0.0f;                           /* current temperature; 0 = greedy */
static Vector3 opt_best_pos[NSPK];
static float   opt_best_cost = 1e30f;
static int     opt_converged = 0;                         /* the GUI climb auto-stopped at the step floor */
static float   opt_worst_wt = 0.333f;                     /* mean<->worst blend (1/3 = the historical mean + 0.5*worst) */
static float   opt_focus_wt = 0.0f;                       /* deg-of-spread per deg-of-error trade; 0 = direction only */
static Vector3 opt_anchor[NSPK];                          /* speaker positions captured when optimization started */

static float opt_bed_wt = 0.0f;            /* weight of the AMBI-bed decode error in the cost (0 = point sources only) */

static float opt_cost_of(bwa_panner p) {   /* coarse; + a penalty per speaker in a projector shadow so the climb clears them */
    float m, w, sp; score_panner(p, 4, panner_tracked(p), &m, &w, &sp);
    int occ = 0; for (int i = 0; i < g_nspk; ++i) if (!los_clear(spk[i].pos)) ++occ;
    float c = 2.0f * ((1.0f - opt_worst_wt) * m + opt_worst_wt * w) + opt_focus_wt * sp + 25.0f * (float)occ;
    if (opt_bed_wt > 0.0f) {               /* what the ambisonic beds want, same mean/worst blend */
        float bm, bw, bs; score_bed(4, &bm, &bw, &bs);
        c += opt_bed_wt * 2.0f * ((1.0f - opt_worst_wt) * bm + opt_worst_wt * bw);
    }
    return c;
}
static float frand(void) { return (float)rand() / ((float)RAND_MAX + 1.0f); }

static int edited_unsaved;   /* any layout edit since the last successful save/reload (the quit guard) */

static void mark_edit(void)  { layout_dirty = 1; score_stale = 1; cov_err_stale = 1; bad_stale = 1; edited_unsaved = 1; }
static void mark_score(void) { score_stale = 1; cov_err_stale = 1; bad_stale = 1; }   /* metric knob changed; the layout file didn't */

/* ---- named optimization CONDITIONS: what a collaborator's preference IS, as an artifact rather
 * than slider positions. A condition bundles the objective knobs (mean/worst blend, focus, the
 * perceptual elevation weight), the scoring-shell band, and a leash. They are starting values —
 * every slider still overrides — and the headless --optimize chains them as STAGES (each stage
 * re-anchors at the previous stage's result, so `horizontal,3d` seeds the 3D climb with the
 * plane-optimal layout). The horizontal leash is deliberately modest: a pure-plane objective
 * happily pulls speakers toward the ear plane (elevated coverage costs it nothing), and the leash
 * is the knob that decides how much of that migration — speaker ALLOCATION to the plane — the
 * stage may do. focus_wt < 0 = use the panner's own default (panner_focus_default). ---- */
typedef struct { const char* name; float worst_wt, focus_wt, elev_wt, band_deg, azi_deg, leash_m; } OptCondition;
static const OptCondition opt_conditions[] = {
    { "3d",         0.333f, -1.0f, 0.3f,   0.0f,  0.0f, 3.0f },   /* the historical default: full sphere */
    { "horizontal", 0.333f, -1.0f, 0.15f, 15.0f,  0.0f, 1.0f },   /* sources near the ear plane only */
    { "visual",     0.333f, -1.0f, 0.3f,  30.0f, 30.0f, 1.0f },   /* the visual area: a front wedge about +z */
};
#define NCONDITIONS ((int)(sizeof opt_conditions / sizeof opt_conditions[0]))
static int opt_condition_idx = 0;                          /* panel combo state */

static void apply_condition(int i, bwa_panner p) {
    const OptCondition* c = &opt_conditions[i];
    opt_worst_wt   = c->worst_wt;
    opt_focus_wt   = (c->focus_wt < 0.0f) ? panner_focus_default(p) : c->focus_wt;
    elev_wt        = c->elev_wt;
    shell_band_deg = c->band_deg;
    shell_azi_deg  = c->azi_deg;
    opt_leash      = c->leash_m;
    opt_condition_idx = i;
    mark_score();
    if (opt_running) opt_cost = opt_cost_of(p);            /* the cached cost was on the old objective */
}
static int condition_find(const char* name) {
    for (int i = 0; i < NCONDITIONS; ++i) if (strcmp(name, opt_conditions[i].name) == 0) return i;
    return -1;
}
/* speakers within slab_m of the ear plane — the readout for how a stage ALLOCATED the array */
static int plane_count(float slab_m) {
    int n = 0;
    for (int i = 0; i < g_nspk; ++i) if (fabsf(spk[i].pos.y - obs_height) <= slab_m) ++n;
    return n;
}

static void optimize_step(bwa_panner p, int trials) {
    for (int t = 0; t < trials; ++t) {
        int s = rand() % g_nspk;
        Vector3 old = spk[s].pos;
        Vector3 cand;
        if (opt_radial) {                          /* slide along the ear ray only (direction preserved; the
                                                    * y>=0 floor / constraint projection keep the last word) */
            Vector3 rd = Vector3Subtract(old, Vector3{ 0, obs_height, 0 });
            float rl = Vector3Length(rd);
            cand = (rl < 1e-3f) ? old : Vector3Add(old, Vector3Scale(rd, opt_step * (2*frand()-1) / rl));
        } else {
            cand = Vector3{ old.x + opt_step * (2*frand()-1), old.y + opt_step * (2*frand()-1), old.z + opt_step * (2*frand()-1) };
        }
        Vector3 dv = Vector3Subtract(cand, opt_anchor[s]);       /* leash: never drift past opt_leash from where it started */
        float dl = Vector3Length(dv);
        if (dl > opt_leash) cand = Vector3Add(opt_anchor[s], Vector3Scale(dv, opt_leash / dl));
        spk[s].pos = constraint_project(pin_project(cand, spk[s].pin));   /* keep the trial feasible */
        float c = opt_cost_of(p);
        float d = c - opt_cost;
        int ok = d < -1e-4f;
        if (!ok && opt_sa_t > 1e-3f && frand() < expf(-d / opt_sa_t)) ok = 1;   /* Metropolis: sometimes uphill */
        if (ok && opt_guard_panner >= 0) {         /* the guard vetoes moves that trade its panner away.
                                                    * It protects the PANNER alone: the bed term is layout-
                                                    * global and would contaminate the guard's meaning. */
            float bw_save = opt_bed_wt; opt_bed_wt = 0.0f;
            ok = opt_cost_of((bwa_panner)opt_guard_panner) <= opt_guard_base + opt_guard_tol;
            opt_bed_wt = bw_save;
        }
        if (ok) {
            opt_cost = c; opt_stall = 0;
            if (c < opt_best_cost) {               /* best-so-far: the layout a run actually ships */
                opt_best_cost = c;
                for (int i = 0; i < g_nspk; ++i) opt_best_pos[i] = spk[i].pos;
            }
        }
        else { spk[s].pos = old; if (++opt_stall > 6*g_nspk) { opt_step *= 0.7f; opt_stall = 0; } }  /* revert; shrink when stuck */
        if (opt_sa_t > 0.0f) opt_sa_t *= SA_DECAY;
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
/* Spread runs 10.7 (a point source) to ~197 (fully diffuse); the interesting band for a real array
 * is roughly 20-90, so map that rather than the full theoretical range. */
static Color spread_heat(float deg) { return heat((90.0f - deg) / 70.0f); }

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
static int   sel = 0, tone_on = 0, tone_kind = BWA_TEST_SINE, driven = -1;
static float g_test_gain = 0.15f;                   /* test-signal output level (linear); panel slider, live */
static int   fps_view = 0;                          /* first-person view from the observer's ears (H) */
static float fps_yaw = 0.0f, fps_pitch = 0.0f, fps_fov = 75.0f;   /* yaw 0 = +z, the room's default facing */
static float cam_yaw = 45.0f * DEG2RAD, cam_pitch = 30.0f * DEG2RAD, cam_dist = 9.0f;
static float save_flash = 0.0f;
static bool  show_te_ui = false;

static void do_save(void) {
    int ok = save_json(g_path);
    save_flash = ok ? 2.0f : -2.0f;
    if (ok) edited_unsaved = 0;
    else fprintf(stderr, "save failed: cannot write '%s' (working dir %s) - check the path / permissions\n",
                 g_path, GetWorkingDirectory());
}
static void do_reload(void) {
    load_json(g_path); cv_load("constraints.json", &CON); mark_edit();
    edited_unsaved = 0;                          /* state now equals the file — nothing to lose on quit */
}
static void do_snap(void) {
    for (int i = 0; i < g_nspk; ++i) spk[i].pos = constraint_project(pin_project(spk[i].pos, spk[i].pin));
    mark_edit();
}
static void do_score(void) {
    for (int p = 0; p < 3; ++p)
        score_panner((bwa_panner)p, 1, panner_tracked((bwa_panner)p),
                     &score_mean[p], &score_worst[p], &score_spread[p]);
    score_bed(1, &score_bed_mean, &score_bed_worst, &score_bed_spread);
    scored = 1; score_stale = 0;
}
static void set_optimizing(int on) {
    if (on && !opt_running) {
        /* a file-authored layout may start OUTSIDE its constraints (the checkbox and K snap
         * project, a hand-edited or generated file doesn't) — without this a violating speaker
         * only becomes feasible if some trial happens to be accepted, so start feasible: pins
         * AND the room constraints, the same projection every trial gets */
        for (int i = 0; i < g_nspk; ++i) {
            Vector3 q = constraint_project(pin_project(spk[i].pos, spk[i].pin));
            if (memcmp(&q, &spk[i].pos, sizeof q) != 0) { spk[i].pos = q; mark_edit(); }
        }
        opt_cost = opt_cost_of((bwa_panner)pv_panner); opt_step = 0.30f; opt_stall = 0; opt_iter = 0;
        opt_sa_t = opt_sa ? SA_T0 : 0.0f;
        opt_best_cost = opt_cost;
        for (int i = 0; i < g_nspk; ++i) { opt_anchor[i] = spk[i].pos; opt_best_pos[i] = spk[i].pos; }
        if (opt_guard_panner >= 0) {               /* bed-term-free, matching the veto's evaluation */
            float bw_save = opt_bed_wt; opt_bed_wt = 0.0f;
            opt_guard_base = opt_cost_of((bwa_panner)opt_guard_panner);
            opt_bed_wt = bw_save;
        }
    }
    if (!on && opt_running && opt_best_cost < opt_cost - 1e-4f) {
        /* an annealed walk can stop above the best it saw; ship the best, always */
        for (int i = 0; i < g_nspk; ++i) spk[i].pos = opt_best_pos[i];
        opt_cost = opt_best_cost;
        mark_edit();
    }
    if (on) opt_converged = 0;
    opt_running = on;
}

/* per-frame climb + the SAME stopping condition the headless runs use (the step floor): the GUI
 * optimizer used to run forever, refining nothing at an ever-shrinking step until stopped by hand */
static void optimize_tick(void) {
    opt_cost = opt_cost_of((bwa_panner)pv_panner);   /* re-baseline so a manual nudge can't wedge the climb */
    optimize_step((bwa_panner)pv_panner, 6);
    if (opt_step <= 0.02f) { opt_converged = 1; set_optimizing(0); }   /* auto-stop; ships the best layout */
}
static void enter_preview(void) {      /* rebuild so the preview pans through the edited layout */
    if (driven >= 0 && e) { bwa_set_test_signal(e, (uint32_t)driven, BWA_TEST_OFF, 0.0f); driven = -1; }
    set_optimizing(0);                           /* stop BEFORE the rebuild snapshots the layout, so the
                                                  * preview plays the best layout seen, not the last walk */
    tone_on = 0; pv_orbit = 0; pv_t = 0.0f;      /* each preview session starts manual, fresh orbit phase */
    if (layout_dirty && e && audio) {            /* rebuild only when there's a device to hear it on */
        if (save_json(TEMP_LAYOUT)) {            /* ... and only if the temp layout actually wrote */
            bwa_stop(e); bwa_destroy(e);
            build_engine(TEMP_LAYOUT);
            layout_dirty = 0; driven = -1;
        } else {
            fprintf(stderr, "preview: cannot write %s (working dir not writable?) - previewing the last build\n", TEMP_LAYOUT);
        }
    }
    preview = 1;
    {   /* what the edited geometry implies for SPCAP's lobe, shown next to the override */
        static float pos[NSPK][3];
        for (int i = 0; i < g_nspk; ++i) {
            pos[i][0] = spk[i].pos.x; pos[i][1] = spk[i].pos.y; pos[i][2] = spk[i].pos.z;
        }
        pv_focus_def = bwa_spcap_focus_default((const float*)pos, (uint32_t)g_nspk);
    }
    if (e) {
        bwa_set_panner(e, (bwa_panner)pv_panner);   /* rebuilt engine defaults to DBAP */
        bwa_set_spcap_focus(e, pv_focus, pv_density);   /* ...and to the derived focus */
        bwa_source_set_gain(e, pv_src, SRC_GAIN);
        bwa_commit(e);
    }
}
static void leave_preview(void) {
    preview = 0;
    if (e) { bwa_source_set_gain(e, pv_src, 0.0f); bwa_commit(e); }
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
        SetWindowTitle(edited_unsaved ? "bw_audio - speaker layout tool  [unsaved]"
                                      : "bw_audio - speaker layout tool");
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
        if (e) bwa_set_panner(e, (bwa_panner)pv_panner);
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
        bwa_source_set_pos(e, pv_src, src_pos.x, src_pos.y, src_pos.z);
        bwa_set_listener_pose(e, 0, obs_height, 0, 0, 0, 0, 1);   /* ears at obs_height; walk the room to test off-center */
        bwa_commit(e);
    }
}

/* edit mode: select/nudge speakers, toggle views, fire the shared actions (the panel mirrors all of it) */
static void handle_edit_input(float dt, bool kb, bool ms, const Camera3D& cam) {
    if (kb) {
        if (IsKeyPressed(KEY_RIGHT_BRACKET)) sel = (sel + 1) % g_nspk;
        if (IsKeyPressed(KEY_LEFT_BRACKET))  sel = (sel + g_nspk - 1) % g_nspk;
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
        if (IsKeyPressed(KEY_N)) tone_kind = (tone_kind == BWA_TEST_SINE) ? BWA_TEST_NOISE : BWA_TEST_SINE;
        if (IsKeyPressed(KEY_S)) do_save();
        if (IsKeyPressed(KEY_L)) do_reload();
        if (IsKeyPressed(KEY_K)) do_snap();
        if (IsKeyPressed(KEY_C)) coverage_on = !coverage_on;
        if (IsKeyPressed(KEY_V)) coverage_moving = !coverage_moving;
        if (IsKeyPressed(KEY_M)) bad_on = !bad_on;   /* volumetric badness map: where can you STAND */
        if (IsKeyPressed(KEY_G)) cov_metric ^= 1;   /* shade: gap <-> selected-panner rE error (cache stays valid) */
        if (IsKeyPressed(KEY_X)) do_score();
        if (IsKeyPressed(KEY_B)) pv_panner = (pv_panner + 1) % 3;   /* the score/optimize target panner */
        if (IsKeyPressed(KEY_O)) set_optimizing(!opt_running);
        if (IsKeyPressed(KEY_P)) enter_preview();
    }
    if (ms && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
        Ray ray = GetMouseRay(GetMousePosition(), cam);        /* click-pick a speaker (imgui owns its own area) */
        float best = 1e9f; int hit = -1;
        for (int i = 0; i < g_nspk; ++i) {
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
        if (driven >= 0 && driven != sel) bwa_set_test_signal(e, (uint32_t)driven, BWA_TEST_OFF, 0.0f);
        bwa_set_test_signal(e, (uint32_t)sel, (bwa_test_kind)tone_kind, g_test_gain);
        driven = sel;
    } else if (driven >= 0) {
        bwa_set_test_signal(e, (uint32_t)driven, BWA_TEST_OFF, 0.0f);
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
    for (int i = 0; i < g_nspk; ++i) {
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
        static Vector3 sdir[27][NSPK];                /* speaker directions from each listener sample */
        for (int l = 0; l < NL; ++l)
            for (int i = 0; i < g_nspk; ++i)
                sdir[l][i] = Vector3Normalize(Vector3Subtract(spk[i].pos, Vector3Add(cov_lis[l], Vector3{ 0, obs_height, 0 })));
        float worst = 1.0f; double macc = 0.0;       /* worst = min score (largest gap) */
        for (int s = 0; s < NCOV; ++s) {
            Vector3 d = cov_dir[s];                   /* a source DIRECTION, queried from each listener */
            float acc = 0.0f;                        /* mean over listeners of (max over speakers of alignment) */
            for (int l = 0; l < NL; ++l) {           /* from listener L, how near is the best speaker to direction d? */
                float best = -1.0f;
                for (int i = 0; i < g_nspk; ++i) { float dp = Vector3DotProduct(d, sdir[l][i]); if (dp > best) best = dp; }
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
            compute_cov_err((bwa_panner)pv_panner);
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
    if (bad_on && !preview) {                        /* volumetric badness map through the room */
        int tracked = panner_tracked((bwa_panner)pv_panner);
        int structural = (!bad_valid || bad_panner != pv_panner || bad_tracked != tracked);
        if (structural || (bad_stale && cov_frame - cov_err_frame >= 6))
            compute_badness((bwa_panner)pv_panner, tracked);
        /* Transparent voxels with depth WRITES off: they are an overlay, and letting them occlude
         * each other (or the speakers) by draw order would read as structure that isn't there. */
        rlDisableDepthMask();
        float span = (bad_hi > bad_lo + 1e-3f) ? (bad_hi - bad_lo) : 1.0f;
        for (int i = 0; i < NBAD; ++i) {
            float v = bad_val[i];
            Color c = (bad_metric == 0) ? err_heat(v) : spread_heat(v);
            float t = (v - bad_lo) / span;           /* fade the good regions out, light the bad ones up */
            if (t < 0.f) t = 0.f; if (t > 1.f) t = 1.f;
            c.a = (unsigned char)(20.0f + 150.0f * t * t);
            DrawCubeV(bad_pos[i], Vector3{ 0.22f, 0.16f, 0.22f }, c);
        }
        rlEnableDepthMask();
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
    for (int i = 0; i < g_nspk; ++i) {
        if (fps_view && Vector3DotProduct(Vector3Subtract(spk[i].pos, cam.position), camfwd) <= 0) continue; /* behind the head */
        Vector2 s = GetWorldToScreen(spk[i].pos, cam);
        char buf[8]; snprintf(buf, sizeof buf, "%d", i);
        float sz = ImGui::GetFontSize() * (i == sel ? 1.15f : 0.8f);
        ImU32 col = (i == sel) ? IM_COL32(250, 230, 120, 255)      /* pinned reads cyan, so the */
                  : spk[i].pin ? IM_COL32(110, 200, 235, 235)      /* allocation is visible at a glance */
                               : IM_COL32(160, 160, 180, 220);
        dl->AddText(ImGui::GetFont(), sz, ImVec2(s.x + 6, s.y - 6), col, buf);
    }
}

/* top-left status overlay: pure text, never captures the mouse (scene clicks pass through it) */
static void draw_hud(float cov_worst, float cov_mean) {
    const Vector3 ear = { 0.0f, obs_height, 0.0f };  /* live delay readout: max-distance alignment at the ears */
    float dmax = 0.0f;
    for (int i = 0; i < g_nspk; ++i) { float dd = Vector3Distance(spk[i].pos, ear); if (dd > dmax) dmax = dd; }
    float seld   = Vector3Distance(spk[sel].pos, ear);
    float seldel = (dmax - seld) / speed_of_sound * 1000.0f;
    int con_bad = 0, con_occ = 0;
    if (CON.loaded) for (int i = 0; i < g_nspk; ++i) {
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
    else       ImGui::TextColored(ImVec4(0.92f, 0.67f, 0.43f, 1), "audio: none - editor only (needs an ASIO/Digiface device to audition)");
    if (CON.loaded && !preview)
        ImGui::TextColored((con_bad || con_occ) ? ImVec4(0.96f, 0.51f, 0.51f, 1) : ImVec4(0.47f, 0.86f, 0.55f, 1),
                           "constraints: %d no-go  %d obstacle   %d out [K snap]  %d occluded (move clear)",
                           CON.nnogo, CON.nobst, con_bad, con_occ);
    if (opt_running && !preview)
        ImGui::TextColored(ImVec4(0.47f, 0.96f, 0.63f, 1), "OPTIMIZING %s   cost %.1f (best %.1f)   iter %d   step %.2f m   [O] stop",
                           panner_names[pv_panner], opt_cost, opt_best_cost, opt_iter, opt_step);
    else if (opt_converged && !preview)
        ImGui::TextColored(ImVec4(0.59f, 0.86f, 0.71f, 1), "optimizer CONVERGED (step floor)   cost %.1f   %d iters   [O] climbs again",
                           opt_cost, opt_iter);
    if (scored && !preview) {
        ImGui::TextColored(ImVec4(0.59f, 0.78f, 0.94f, 1),
                           "rE-err deg mean/worst (live%s):   %sDBAP %.0f/%.0f    %sSPCAP %.0f/%.0f    %sVBAP %.0f/%.0f",
                           perceptual ? ", az>el" : "",
                           pv_panner==0?">":"", score_mean[0], score_worst[0],
                           pv_panner==1?">":"", score_mean[1], score_worst[1],
                           pv_panner==2?">":"", score_mean[2], score_worst[2]);
        ImGui::TextColored(ImVec4(0.59f, 0.78f, 0.94f, 1),
                           "focus - Frank spread deg (lower = sharper):   DBAP %.0f    SPCAP %.0f    VBAP %.0f",
                           score_spread[0], score_spread[1], score_spread[2]);
        ImGui::TextColored(ImVec4(0.73f, 0.66f, 0.92f, 1),
                           "AMBI bed (%s%s):   rE err %.0f/%.0f deg   spread %.0f deg%s",
                           bed_decoder ? "EPAD" : "AllRAD", bed_max_re ? ", max-rE" : "",
                           score_bed_mean, score_bed_worst, score_bed_spread,
                           opt_bed_wt > 0 ? "   (in the cost)" : "");
    }
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
    if (bad_on && bad_valid && !preview)
        ImGui::TextColored(imcol(bad_metric ? spread_heat(bad_mean) : err_heat(bad_mean)),
                           "badness map [%s solve]  %s:  best %.0f / mean %.0f / worst %.0f deg  over %d room points",
                           bad_tracked ? "tracked" : "fixed",
                           bad_metric ? "Frank spread" : "rE direction error",
                           bad_lo, bad_mean, bad_hi, NBAD);
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
            bwa_set_panner(e, (bwa_panner)pv_panner);   /* live A/B (atomic, safe while running) */
        bwTip("A/B by ear while it plays - the switch is atomic (no glitch)");
        /* SPCAP's own knobs, in the same live-A/B group as the panner (B) and the observer model (V) */
        ImGui::BeginDisabled(pv_panner != BWA_PAN_SPCAP);
        if (ImGui::SliderFloat("focus", &pv_focus, 0.0f, 48.0f,
                               pv_focus > 0.0f ? "%.1f" : "(default)")) {
            if (e) bwa_set_spcap_focus(e, pv_focus, pv_density);
            mark_score();                            /* the scoring path solves at this value too */
        }
        bwTip("SPCAP lobe sharpness: higher concentrates a source on fewer speakers, lower spreads "
              "it. 0 = the default this array's speaker spacing implies. Live, and even a parked "
              "source re-solves - drag it while the orbit runs and listen to the image tighten. "
              "The Score board and the rE overlay follow it.");
        if (ImGui::SliderFloat("density", &pv_density, 0.0f, 8.0f,
                               pv_density > 0.0f ? "%.2f" : "(default 2.0)")) {
            if (e) bwa_set_spcap_focus(e, pv_focus, pv_density);
            mark_score();
        }
        bwTip("placement-correction exponent: how hard a cluster of speakers is de-weighted so it "
              "can't pull the image. 2.0 is the default and is rarely worth moving.");
        if (ImGui::Button("SPCAP default", ImVec2(-1, 0))) {
            pv_focus = 0.0f; pv_density = 0.0f;
            if (e) bwa_set_spcap_focus(e, pv_focus, pv_density);
            mark_score();
        }
        bwTip("send 0 for both (the ABI's revert-to-default sentinel)");
        ImGui::TextDisabled("this layout derives focus %.1f", (double)pv_focus_def);
        ImGui::EndDisabled();
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
    { int n = g_nspk;                                /* the array's SIZE = the engine's channel count */
      if (ImGui::InputInt("count", &n)) {
          if (n < NSPK_MIN) n = NSPK_MIN; if (n > NSPK) n = NSPK;
          for (int i = g_nspk; i < n; ++i) { spk[i].pos = dome_pos(i, n); spk[i].gain_db = 0.0f; }  /* grow: on the dome */
          if (n != g_nspk) {                         /* shrink drops the tail; grow keeps what is already placed */
              g_nspk = n; if (sel >= g_nspk) sel = g_nspk - 1;
              mark_edit();
              /* the engine's channel count IS the layout's, so a count change must rebuild it —
               * otherwise the test tone on a newly-added channel goes nowhere */
              if (e && audio && save_json(TEMP_LAYOUT)) {
                  if (driven >= 0) { bwa_set_test_signal(e, (uint32_t)driven, BWA_TEST_OFF, 0.0f); driven = -1; }
                  bwa_stop(e); bwa_destroy(e);
                  build_engine(TEMP_LAYOUT);
                  layout_dirty = 0;
              }
          }
      } }
    bwTip("speakers in this layout (4-26) - the file's count IS the engine's channel count, so a "
          "24-speaker array is a 24-entry file; growing adds one on the dome, shrinking drops the last");
    if (ImGui::InputInt("##spk", &sel)) { if (sel < 0) sel = 0; if (sel >= g_nspk) sel = g_nspk - 1; }
    bwTip("the speaker's index IS its output/bus channel; [ ] steps, or click a sphere");
    ImGui::SameLine(); ImGui::Text("-> ch %d", sel);
    if (ImGui::DragFloat3("pos", &spk[sel].pos.x, 0.01f, 0, 0, "%.3f")) mark_edit();
    bwTip("room-space meters, right-handed, +y up, origin on the floor at the working-area center (Motive); drag, or ctrl-click to type");
    if (ImGui::SliderFloat("gain", &spk[sel].gain_db, -24.0f, 12.0f, "%+.1f dB")) mark_edit();
    bwTip("per-speaker level trim (gain_db in the file)");
    { bool pb = spk[sel].pin != 0;
      if (ImGui::Checkbox("pin to plane", &pb)) {
          spk[sel].pin = pb;
          if (pb) spk[sel].pos = constraint_project(pin_project(spk[sel].pos, 1));
          mark_edit();
      } }
    bwTip("ALLOCATION as a constraint: hold this speaker in a slab about the ear plane - the "
          "optimizer's trials and [K] snap both project into it, so a later 3d stage cannot pull "
          "it back toward elevation ('spend 12 on the plane' = 12 pinned speakers). Saved in the "
          "layout file (\"pin\": \"plane\"); the engine ignores it");
    { int npin = 0; for (int i = 0; i < g_nspk; ++i) npin += spk[i].pin;
      if (npin) {
          ImGui::SameLine(); ImGui::TextDisabled("%d pinned", npin);
          if (ImGui::SliderFloat("pin slab", &pin_slab_m, 0.05f, 1.0f, "%.2f m")) mark_edit();
          bwTip("slab half-height about the ear plane a pinned speaker may occupy "
                "(pin_slab_m in the file; snap re-applies it)");
      } }
    CheckboxInt("tone [T]", &tone_on);
    bwTip("drive THIS channel with the test signal out the array: walk the room, hear which "
          "physical speaker it is, place its marker (needs the ASIO/Digiface device)");
    ImGui::SameLine();
    { bool nb = tone_kind == BWA_TEST_NOISE;
      if (ImGui::Checkbox("noise [N]", &nb)) tone_kind = nb ? BWA_TEST_NOISE : BWA_TEST_SINE; }
    bwTip("test-signal type; noise is easier to localize by ear than a sine");
    ImGui::SliderFloat("tone gain", &g_test_gain, 0.0f, 1.0f, "%.3f");
    bwTip("test-signal output level (linear). It is injected AFTER align, so the per-speaker gain trim "
          "above does NOT affect it - this is the only in-app control; the amps/Dante set room SPL. "
          "Keep it modest (old fixed value was 0.4).");
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
    if (ImGui::Combo("panner", &pv_panner, "DBAP (moving)\0SPCAP (fixed)\0VBAP (fixed)\0")) {
        /* the focus weight and the map metric are not panner-independent — see panner_focus_default */
        opt_focus_wt = panner_focus_default((bwa_panner)pv_panner);
        bad_metric   = (pv_panner == BWA_PAN_DBAP) ? 0 : 1;
        mark_score();
    }
    bwTip("the panner Score / Optimize / the rE overlay evaluate ([B] cycles). DBAP tracks a "
          "MOVING listener; SPCAP/VBAP assume the center sweet spot");
    /* SPCAP's tuning, on the SCORING side of the tool: the same two globals the preview panel dials
     * by ear, passed straight into bwa_panner_gains_batch. A layout graded at the derived focus says
     * nothing about how it behaves at the focus you actually ship, so the score follows the knob. */
    if (pv_panner == BWA_PAN_SPCAP) {
        int knob = 0;
        knob |= ImGui::SliderFloat("focus", &pv_focus, 0.0f, 48.0f,
                                   pv_focus > 0.0f ? "%.1f" : "(default)") ? 1 : 0;
        bwTip("SPCAP lobe sharpness the Score / Optimize / overlay solve at. 0 = the value this "
              "geometry derives (shown below). Dial it by ear in the preview, then score it here");
        knob |= ImGui::SliderFloat("density", &pv_density, 0.0f, 8.0f,
                                   pv_density > 0.0f ? "%.2f" : "(default 2.0)") ? 1 : 0;
        bwTip("placement-correction exponent: how hard a cluster of speakers is de-weighted so it "
              "can't pull the image. 2.0 is the default and is rarely worth moving");
        if (knob) {
            if (e) bwa_set_spcap_focus(e, pv_focus, pv_density);   /* keep a live preview in step */
            mark_score();                                          /* scoreboard + overlays + map */
            if (opt_running) opt_cost = opt_cost_of((bwa_panner)pv_panner);   /* the cached cost was on the old lobe */
        }
        ImGui::TextDisabled("this layout derives focus %.1f", (double)pv_focus_def);
    }
    { int ci = opt_condition_idx;
      if (ImGui::Combo("condition", &ci, "3d (full sphere)\0horizontal (plane band)\0visual (front wedge)\0"))
          apply_condition(ci, (bwa_panner)pv_panner); }
    bwTip("a named objective: what to optimize FOR. 'horizontal' scores only source directions "
          "within 15 deg of the ear plane (planar localization); 'visual' only a front wedge "
          "(azimuth and elevation within 30 deg of +z - where the listener looks). Both use a "
          "1 m leash: a narrow objective pulls speakers toward its region, and the leash decides "
          "how much. Starting values; every slider below still overrides. To STAGE them, optimize "
          "under one, stop, switch, optimize again - each start re-anchors the leash");
    if (ImGui::Button("Score [X]")) do_score();
    bwTip("rE localization error over a shell of directions, via the engine's real gain solve - "
          "mean/worst per panner land in the HUD scoreboard");
    ImGui::SameLine();
    { bool ob = opt_running != 0; if (ImGui::Checkbox("Optimize [O]", &ob)) set_optimizing(ob); }
    bwTip("stochastic hill-climb of the speaker positions, minimizing the target panner's rE error "
          "within the constraints; runs live - watch it converge. Stops ITSELF when the step size "
          "hits the floor (same condition as the headless runs) and restores the best layout seen; "
          "[O] again re-climbs from there");
    ImGui::SliderFloat("leash", &opt_leash, 0.1f, 3.0f, "%.2f m");   /* max optimizer move from the anchor */
    bwTip("how far the optimizer may move any speaker from where it started (3 m = essentially free)");
    ImGui::SameLine();
    CheckboxInt("radial", &opt_radial);
    bwTip("trials move speakers only ALONG the ray from the ears: directions stay put, radii refit. "
          "The cross-panner pass - optimize for VBAP, then a radial DBAP pass tunes distances "
          "without disturbing the triangulation");
    CheckboxInt("anneal", &opt_sa);
    bwTip("Metropolis acceptance: uphill moves are sometimes taken while the temperature is high, "
          "so the climb can cross cost barriers a greedy walk cannot. The best layout seen is "
          "always kept and restored when you stop, so annealing can never end worse than it began");
    { int gp = opt_guard_panner + 1;
      if (ImGui::Combo("guard", &gp, "off\0DBAP\0SPCAP\0VBAP\0")) {
          opt_guard_panner = gp - 1;
          if (opt_running && opt_guard_panner >= 0) opt_guard_base = opt_cost_of((bwa_panner)opt_guard_panner);
      } }
    bwTip("constrained climb: while optimizing the target panner, REJECT any move that lets this "
          "panner's cost slip more than the tolerance above where it started - 'the best VBAP "
          "layout that never lets DBAP slip'. Start from a layout already optimized for the guard "
          "panner, or the baseline it protects is a weak one");
    if (opt_guard_panner >= 0) {
        ImGui::SliderFloat("guard tol", &opt_guard_tol, 0.0f, 2.0f, "%.2f");
        bwTip("allowed guard-cost slip, in cost units (roughly degrees); 0 = the guard may not "
              "get worse at all (very restrictive for a stochastic climb)");
    }
    if (ImGui::SliderFloat("worst wt", &opt_worst_wt, 0.0f, 1.0f, "%.2f") && opt_running)
        opt_cost = opt_cost_of((bwa_panner)pv_panner);   /* re-baseline: the cached cost is on the old blend */
    bwTip("mean<->worst-case blend the optimizer climbs: 0 optimizes the AVERAGE direction/observer "
          "position, 1 is pure MAXIMIN - no direction or seat is sacrificed for the average (they "
          "can't all be exact at once, so this slider picks the compromise); each setting lands a "
          "different point on the accuracy/robustness Pareto front");
    if (ImGui::SliderFloat("focus wt", &opt_focus_wt, 0.0f, 2.0f, "%.2f") && opt_running)
        opt_cost = opt_cost_of((bwa_panner)pv_panner);
    bwTip("adds the mean perceived source width (Frank 2013: ~186\xC2\xB0\xC2\xB7(1-|rE|)+11\xC2\xB0) to the cost - "
          "an accurate but DEFOCUSED image scores 0\xC2\xB0 direction error, this is the axis that sees it; "
          "0 = direction only, 1 = a degree of spread costs a degree of error");
    if (ImGui::SliderFloat("bed wt", &opt_bed_wt, 0.0f, 2.0f, "%.2f") && opt_running)
        opt_cost = opt_cost_of((bwa_panner)pv_panner);
    bwTip("adds the AMBISONIC BED's decode error to the cost (plane waves at infinity, via the "
          "engine's real build). Bed content wants a good spherical QUADRATURE - uniformity the "
          "point-source score cannot see; 0 = point sources only, 1 = the bed's mean/worst blend "
          "counts as much as the panner's. The scoreboard's AMBI row shows it either way");
    { int bd = bed_decoder;
      if (ImGui::Combo("bed decode", &bd, "AllRAD\0EPAD\0")) {
          bed_decoder = bd; mark_score();
          if (opt_running && opt_bed_wt > 0.0f) opt_cost = opt_cost_of((bwa_panner)pv_panner);
      } }
    bwTip("which SH->speaker decode the AMBI row (and bed wt) evaluates - match your install's "
          "bwa_desc.bed_decoder, or you grade a render the rig never runs");
    ImGui::SameLine();
    { bool mr = bed_max_re != 0;
      if (ImGui::Checkbox("max-rE", &mr)) {
          bed_max_re = mr; mark_score();
          if (opt_running && opt_bed_wt > 0.0f) opt_cost = opt_cost_of((bwa_panner)pv_panner);
      } }
    bwTip("apply max-rE weighting before the decode, matching bwa_set_max_re on the rig");
    /* Lower bound is 0.1, not 0: save_json writes obs_height as reference.ears_m and load_json only
     * accepts (0, 3]. A slider that can reach exactly 0 writes an anchor the loader then drops, so a
     * reopen would silently re-derive every delay at the 1.4 default — the round trip this field
     * exists to close. Keep this range inside the loader's. */
    if (ImGui::SliderFloat("obs ear y", &obs_height, 0.1f, 2.0f, "%.2f m")) mark_score();
    bwTip("listener EAR height above the floor - scoring, coverage, and the sightline checks all measure from here");
    if (CheckboxInt("perceptual (az>el)", &perceptual)) mark_score();   /* weight azimuth >> elevation */
    bwTip("weight azimuth error over elevation: human azimuth acuity is ~3.5x finer, so the "
          "optimizer trades vertical accuracy for horizontal");
    if (perceptual && ImGui::SliderFloat("elev wt", &elev_wt, 0.0f, 1.0f, "%.2f")) mark_score();
    bwTip("elevation-error weight vs azimuth (0.3 ~ the psychophysics ratio; 1 = isotropic)");
    if (ImGui::SliderFloat("band", &shell_band_deg, 0.0f, 90.0f, "%.0f deg")) {
        mark_score();
        if (opt_running) opt_cost = opt_cost_of((bwa_panner)pv_panner);   /* the cached cost is on the old shell */
    }
    bwTip("scoring-shell elevation band: only source directions within this many degrees of the "
          "ear plane are scored/optimized; 0 = the full sphere. The Score board follows it too, "
          "so the numbers always mean the active condition; the coverage overlay stays full-sphere");
    if (ImGui::SliderFloat("azi band", &shell_azi_deg, 0.0f, 180.0f, "%.0f deg")) {
        mark_score();
        if (opt_running) opt_cost = opt_cost_of((bwa_panner)pv_panner);
    }
    bwTip("scoring-shell azimuth wedge about +z (the room's forward): only source directions "
          "within this many degrees of straight ahead count; 0 = all azimuths. Anchored to ONE "
          "facing - right for a dominant-screen install, wrong for a turn-anywhere CAVE");
    CheckboxInt("coverage [C]", &coverage_on);
    bwTip("shade a shell of source directions: green = the array localizes it well, red = a hole; "
          "hover a cube for its value");
    ImGui::SameLine();
    CheckboxInt("moving [V]", &coverage_moving);
    bwTip("observer model: average over a grid of listener positions across the working volume "
          "(this installation's case) instead of the center sweet spot");
    if (ImGui::Combo("solve at", &track_mode, "auto (per panner)\0tracked listener\0fixed sweet spot\0"))
        mark_score();
    bwTip("where the panner SOLVES, as opposed to where you listen from. A fixed install solves once "
          "at the sweet spot and never corrects for you walking away; a tracked one re-solves at your "
          "position every block. Scoring only at the sweet spot cannot see that difference at all - "
          "auto gives each panner its real behavior, and forcing a mode A/Bs the contrast");

    CheckboxInt("badness map [M]", &bad_on);
    bwTip("where in the ROOM does this layout work, rather than which directions work from the "
          "center: a grid of LISTENER positions through the working volume, each scored over a "
          "spread of directions. Good regions fade out, bad ones light up. Toggle 'solve at' while "
          "watching - a fixed solve draws an island around the sweet spot, a tracked one stays flat");
    if (bad_on && ImGui::Combo("map metric", &bad_metric, "rE direction error\0Frank spread\0")) bad_stale = 1;
    if (bad_on) bwTip("which proxy to paint. They are not interchangeable: against the acoustic "
                      "measurement (bwa_validate) direction error ranks DBAP well and VBAP barely, "
                      "where spread is the strong predictor - so this follows the panner by default");

    CheckboxInt("shade rE error (vs gap) [G]", &cov_metric);
    bwTip("color by the selected panner's REAL solve (rE error) instead of the geometric "
          "nearest-speaker gap");

    ImGui::SeparatorText("Audition");
    if (ImGui::Button("Preview - audition [P]", ImVec2(-1, 0))) enter_preview();
    bwTip("pan a pink-noise source through the edited layout and judge it by ear (rebuilds the "
          "engine - the layout is load-time); at the CAVE, walk the room for off-center coverage");

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
        spk[3].pin = 1; pin_slab_m = 0.4f;                       /* the allocation tag rides the file too */
        IM_CHECK(save_json(TEST_OUT));
        seed_default(); dbap_r = 0.5f; pin_slab_m = 0.3f;
        IM_CHECK_EQ(load_json(TEST_OUT), NSPK);
        IM_CHECK_LT(fabsf(spk[3].pos.x - 1.25f), 1e-3f);
        IM_CHECK_LT(fabsf(spk[3].pos.z + 0.75f), 1e-3f);
        IM_CHECK_LT(fabsf(spk[3].gain_db + 4.5f), 1e-2f);
        IM_CHECK_LT(fabsf(dbap_r - 0.77f), 1e-5f);
        IM_CHECK_EQ(spk[3].pin, 1);
        IM_CHECK_EQ(spk[4].pin, 0);
        IM_CHECK_LT(fabsf(pin_slab_m - 0.4f), 1e-5f);
        pin_slab_m = 0.3f;
        dbap_r = 0.5f; seed_default(); layout_dirty = 1;
    };

    /* a SMALLER array (a collaborator's 24 speakers): the count round-trips through the file, the
     * engine's own loader accepts what we wrote, and scoring runs on the reduced array. */
    t = IM_REGISTER_TEST(te, "logic", "save_load_24");
    t->TestFunc = [](ImGuiTestContext*) {
        const int keep = g_nspk;
        g_nspk = 24; seed_default();
        spk[23].pos = Vector3{ -0.5f, 1.8f, 1.1f };
        IM_CHECK(save_json(TEST_OUT));
        g_nspk = NSPK; seed_default();                           /* clobber, then read the count back */
        IM_CHECK_EQ(load_json(TEST_OUT), 24);
        IM_CHECK_EQ(g_nspk, 24);
        IM_CHECK_LT(fabsf(spk[23].pos.z - 1.1f), 1e-3f);
        /* the ENGINE must accept what we wrote, at OUR count (bwa_create only loads — no device) */
        bwa_desc cfg; memset(&cfg, 0, sizeof cfg);
        cfg.profile = BWA_PROFILE_CAVE; cfg.sample_rate = SR; cfg.block_size = 256;
        cfg.layout_path = TEST_OUT;
        bwa_engine* te2 = bwa_create(&cfg);
        IM_CHECK(te2 != NULL);
        if (te2) {
            IM_CHECK(bwa_last_error(te2) == NULL);                /* not silently defaulted to the 26 grid */
            IM_CHECK_EQ(bwa_get_channel_count(te2), 24u);
            bwa_destroy(te2);
        }
        float m = 0, w = 0;                                      /* the real panner solve, on 24 speakers */
        score_panner(BWA_PAN_DBAP, 4, 1, &m, &w, NULL);
        IM_CHECK_GT(w, 0.0f);
        g_nspk = keep; seed_default(); layout_dirty = 1;
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

    /* The solve/eval split is the whole point of scoring off-center at all: a fixed solve keeps the
     * sweet spot's gains while the listener walks away, so over the grid it cannot beat a tracked
     * one. If `tracked` were being ignored the two would come back bit-identical, which is exactly
     * what this catches. */
    t = IM_REGISTER_TEST(te, "logic", "solve_eval");
    t->TestFunc = [](ImGuiTestContext*) {
        seed_default(); layout_dirty = 1;
        float mt = -1, wt = -1, mf = -1, wf = -1;
        score_panner(BWA_PAN_SPCAP, 4, 1, &mt, &wt, NULL);       /* solved at each listener */
        score_panner(BWA_PAN_SPCAP, 4, 0, &mf, &wf, NULL);       /* solved once at the sweet spot */
        IM_CHECK_GT(mt, 0.0f);
        IM_CHECK_GT(fabsf(mf - mt), 1e-4f);                      /* the mode actually changes the render */
        IM_CHECK_GE(mf, mt - 0.5f);                              /* ...and leaving the sweet spot never helps */
        /* track_mode overrides it for A/B; auto gives each panner its real behavior */
        int save = track_mode;
        track_mode = 0; IM_CHECK(panner_tracked(BWA_PAN_DBAP) && !panner_tracked(BWA_PAN_VBAP));
        track_mode = 1; IM_CHECK(panner_tracked(BWA_PAN_VBAP));
        track_mode = 2; IM_CHECK(!panner_tracked(BWA_PAN_DBAP));
        track_mode = save;
        /* the OBSERVER model is the other objective axis: a seated install evaluates only the
         * sweet spot, where a fixed solve is at home — strictly easier than the roam's mean */
        score_fixed_obs = 1;
        float ms = -1, ws = -1;
        score_panner(BWA_PAN_SPCAP, 4, 0, &ms, &ws, NULL);
        score_fixed_obs = 0;
        IM_CHECK_GT(ms, 0.0f);
        IM_CHECK_LT(ms, mf - 1e-3f);                             /* sweet spot beats the roam mean */
        IM_CHECK_LE(ws, wf + 1e-4f);                             /* and its worst can't exceed the roam's */
    };

    /* The badness map's headline claim is visual: a FIXED solve draws an island around the sweet
     * spot, a tracked one stays flat. That is a spread-of-values statement, so it is testable. */
    t = IM_REGISTER_TEST(te, "logic", "badness_map");
    t->TestFunc = [](ImGuiTestContext*) {
        seed_default(); layout_dirty = 1;
        int save_metric = bad_metric;
        bad_metric = 0;
        compute_badness(BWA_PAN_SPCAP, 1);
        IM_CHECK(bad_valid);
        float track_span = bad_hi - bad_lo, track_mean = bad_mean;
        IM_CHECK_GT(bad_mean, 0.0f);
        IM_CHECK_LT(bad_hi, 90.0f);
        for (int i = 0; i < NBAD; ++i) IM_CHECK(bad_val[i] == bad_val[i]);   /* no NaN anywhere */
        compute_badness(BWA_PAN_SPCAP, 0);
        IM_CHECK_GT(bad_hi - bad_lo, track_span);                /* the fixed map is the uneven one */
        IM_CHECK_GE(bad_mean, track_mean - 0.5f);
        /* the map grid really does span the working volume in HEIGHT, not just the ear plane */
        float ylo = 1e9f, yhi = -1e9f;
        for (int i = 0; i < NBAD; ++i) { if (bad_pos[i].y < ylo) ylo = bad_pos[i].y;
                                         if (bad_pos[i].y > yhi) yhi = bad_pos[i].y; }
        IM_CHECK_GT(yhi - ylo, 0.9f);
        bad_metric = save_metric;
    };

    /* The bed row goes through the engine's REAL AllRAD build (bwa_bed_gains_batch): sane scores on
     * the dome, the batch API's contract, and the bed term lands in the optimizer cost when weighted. */
    t = IM_REGISTER_TEST(te, "logic", "bed_score");
    t->TestFunc = [](ImGuiTestContext*) {
        seed_default(); layout_dirty = 1;
        float bm = -1, bw = -1, bs = -1;
        score_bed(2, &bm, &bw, &bs);
        IM_CHECK_GT(bm, 0.0f);
        IM_CHECK_LT(bm, 90.0f);
        IM_CHECK_GE(bw, bm);
        IM_CHECK_GT(bs, 10.0f);                                  /* Frank spread bounds */
        IM_CHECK_LT(bs, 197.2f);
        float pos[NSPK * 3], g1[NSPK], d1[3] = { 0, 0, 1 };      /* the batch API directly: one plane wave */
        for (int i = 0; i < g_nspk; ++i) { pos[i*3]=spk[i].pos.x; pos[i*3+1]=spk[i].pos.y; pos[i*3+2]=spk[i].pos.z; }
        IM_CHECK_EQ(bwa_bed_gains_batch(BWA_DECODE_ALLRAD, false, pos, (uint32_t)g_nspk, d1, 1, g1), 1u);
        float esum = 0; for (int s = 0; s < g_nspk; ++s) esum += g1[s] * g1[s];
        IM_CHECK_GT(esum, 1e-6f);                                /* the decode carries energy */
        const float saveW = opt_worst_wt, saveF = opt_focus_wt, saveB = opt_bed_wt;
        float m, w2, sp; score_panner(BWA_PAN_DBAP, 4, 1, &m, &w2, &sp);
        float bm4, bw4; score_bed(4, &bm4, &bw4, NULL);
        opt_worst_wt = 0.0f; opt_focus_wt = 0.0f; opt_bed_wt = 1.0f;   /* cost = 2*mean + 2*bed_mean */
        IM_CHECK_LT(fabsf(opt_cost_of(BWA_PAN_DBAP) - (2.0f * m + 2.0f * bm4)), 0.75f);
        opt_worst_wt = saveW; opt_focus_wt = saveF; opt_bed_wt = saveB;
        bed_decoder = 1; bed_max_re = 1;                         /* the EPAD + max-rE grading path */
        float em, ew, es;
        score_bed(2, &em, &ew, &es);
        IM_CHECK_GT(em, 0.0f);
        IM_CHECK_LT(em, 90.0f);
        IM_CHECK_GE(ew, em);
        IM_CHECK_GT(es, 10.0f);
        IM_CHECK_LT(es, 197.2f);
        bed_decoder = 0; bed_max_re = 0;
    };

    t = IM_REGISTER_TEST(te, "logic", "score");                  /* the real engine solve scores the default dome sanely */
    t->TestFunc = [](ImGuiTestContext*) {
        seed_default(); layout_dirty = 1;
        float m = -1, w = -1, sp = -1;
        score_panner(BWA_PAN_DBAP, 2, 1, &m, &w, &sp);
        IM_CHECK_GT(m, 0.0f);
        IM_CHECK_LT(m, 90.0f);
        IM_CHECK_GE(w, m);
        IM_CHECK_LT(w, 181.0f);
        IM_CHECK_GT(sp, 10.0f);                                  /* Frank spread: 10.7° floor .. 197° ceiling */
        IM_CHECK_LT(sp, 197.2f);
    };

    t = IM_REGISTER_TEST(te, "logic", "maximin");                /* the objective blend is wired: the weights select
                                                                  * which score the optimizer actually climbs */
    t->TestFunc = [](ImGuiTestContext*) {
        seed_default(); layout_dirty = 1;
        float m, w, sp;
        score_panner(BWA_PAN_DBAP, 4, 1, &m, &w, &sp);           /* stride 4 = opt_cost_of's own sampling */
        const float saveW = opt_worst_wt, saveF = opt_focus_wt;
        opt_worst_wt = 1.0f; opt_focus_wt = 0.0f;                /* pure maximin: cost = 2*worst (no occluders) */
        IM_CHECK_LT(fabsf(opt_cost_of(BWA_PAN_DBAP) - 2.0f * w), 0.5f);
        opt_worst_wt = 0.0f;                                     /* pure mean */
        IM_CHECK_LT(fabsf(opt_cost_of(BWA_PAN_DBAP) - 2.0f * m), 0.5f);
        opt_worst_wt = 0.0f; opt_focus_wt = 1.0f;                /* the focus axis lands in the cost too */
        IM_CHECK_LT(fabsf(opt_cost_of(BWA_PAN_DBAP) - (2.0f * m + sp)), 0.75f);
        opt_worst_wt = saveW; opt_focus_wt = saveF;
    };

    /* The 2D condition really is the shell band, not just an elevation weight: a flat ring of
     * speakers at ear height localizes in-plane sources well and overhead ones not at all, so the
     * band-limited score must be clearly better than the full-sphere one on that layout. Plus the
     * condition-preset wiring: applying 'horizontal' sets the band/leash the table promises. */
    t = IM_REGISTER_TEST(te, "logic", "band_shell");
    t->TestFunc = [](ImGuiTestContext*) {
        const int keep = g_nspk;
        g_nspk = 8;
        for (int i = 0; i < 8; ++i) {                            /* a pure-planar array: a ring at ear height */
            float a = (float)i / 8.0f * 2.0f * PI;
            spk[i].pos = Vector3{ 2.4f * cosf(a), obs_height, 2.4f * sinf(a) };
            spk[i].gain_db = 0.0f;
        }
        float fm, fw, bm, bw;
        shell_band_deg = 0.0f;  score_panner(BWA_PAN_DBAP, 2, 1, &fm, &fw, NULL);
        shell_band_deg = 15.0f; score_panner(BWA_PAN_DBAP, 2, 1, &bm, &bw, NULL);
        IM_CHECK_LT(bm, fm);                                     /* dropping overhead directions must help a ring */
        IM_CHECK_LT(bw, fw);
        IM_CHECK_LT(bm, 30.0f);                                  /* in-band, the ring actually localizes */
        IM_CHECK_GT(fw, 45.0f);                                  /* straight up is hopeless for a ring */
        const float saveW = opt_worst_wt, saveF = opt_focus_wt, saveE = elev_wt, saveL = opt_leash;
        const int   saveC = opt_condition_idx;
        int hi = condition_find("horizontal");
        IM_CHECK(hi >= 0);
        IM_CHECK(condition_find("nonsense") < 0);
        apply_condition(hi, BWA_PAN_DBAP);
        IM_CHECK_EQ(shell_band_deg, opt_conditions[hi].band_deg);
        IM_CHECK_EQ(opt_leash, opt_conditions[hi].leash_m);
        IM_CHECK_EQ(plane_count(0.5f), 8);                       /* the whole ring sits on the ear plane */
        /* the azimuth wedge: a FRONT-ONLY cluster is fine where you look, hopeless behind */
        for (int i = 0; i < 8; ++i) {
            float az = (-24.5f + 7.0f * (float)i) * DEG2RAD;     /* azimuths spread across +-25 deg of +z */
            float el = ((i & 1) ? 15.0f : -15.0f) * DEG2RAD;
            spk[i].pos = Vector3{ 2.4f * sinf(az) * cosf(el), obs_height + 2.4f * sinf(el), 2.4f * cosf(az) * cosf(el) };
        }
        int vi = condition_find("visual");
        IM_CHECK(vi >= 0);
        apply_condition(vi, BWA_PAN_DBAP);
        IM_CHECK_EQ(shell_azi_deg, opt_conditions[vi].azi_deg);
        float vm, vw;
        score_panner(BWA_PAN_DBAP, 2, 1, &vm, &vw, NULL);        /* wedge only */
        shell_band_deg = 0.0f; shell_azi_deg = 0.0f;
        score_panner(BWA_PAN_DBAP, 2, 1, &fm, &fw, NULL);        /* full sphere, same layout */
        IM_CHECK_LT(vm, fm);
        IM_CHECK_LT(vm, 30.0f);                                  /* in the wedge the cluster localizes */
        IM_CHECK_GT(fw, 45.0f);                                  /* behind the head it cannot */
        opt_worst_wt = saveW; opt_focus_wt = saveF; elev_wt = saveE; opt_leash = saveL;
        opt_condition_idx = saveC; shell_band_deg = 0.0f; shell_azi_deg = 0.0f;
        g_nspk = keep; seed_default(); layout_dirty = 1;
    };

    /* Pins are the allocation guarantee: snap projects a pinned speaker into the ear-plane slab,
     * and a full-3d climb — the objective that WANTS elevation — cannot pull it back out. Without
     * the constraint that erosion is exactly what happens (measured: 10 -> 5 in-slab speakers). */
    t = IM_REGISTER_TEST(te, "logic", "pin_plane");
    t->TestFunc = [](ImGuiTestContext*) {
        seed_default(); layout_dirty = 1;
        for (int i = 0; i < 12; ++i) spk[i].pin = 1;             /* "26 total, spend 12 on the plane" */
        spk[0].pos = Vector3{ 0.5f, 2.4f, 0.5f };                /* parked well off the plane */
        do_snap();
        for (int i = 0; i < 12; ++i) IM_CHECK_LE(fabsf(spk[i].pos.y - obs_height), pin_slab_m + 1e-4f);
        IM_CHECK_GE(plane_count(pin_slab_m + 1e-3f), 12);
        srand(7);                                                /* deterministic climb */
        const float saveL = opt_leash;
        opt_leash = 3.0f;                                        /* ~free: only the pin holds them */
        opt_cost = opt_cost_of(BWA_PAN_DBAP); opt_step = 0.30f; opt_stall = 0; opt_iter = 0;
        for (int i = 0; i < g_nspk; ++i) opt_anchor[i] = spk[i].pos;
        optimize_step(BWA_PAN_DBAP, 25);                         /* a 3d climb, full sphere */
        for (int i = 0; i < 12; ++i) IM_CHECK_LE(fabsf(spk[i].pos.y - obs_height), pin_slab_m + 1e-4f);
        opt_leash = saveL;
        /* a FILE-authored pin can start outside its slab (no checkbox, no snap): starting the
         * optimizer must project it in, not leave it to a lucky accepted trial */
        spk[3].pos = Vector3{ 0.4f, 2.3f, -0.4f };               /* pinned, but parked off-plane */
        set_optimizing(1);
        IM_CHECK_LE(fabsf(spk[3].pos.y - obs_height), pin_slab_m + 1e-4f);
        set_optimizing(0);
        seed_default(); layout_dirty = 1;
    };

    /* Radial mode is a promise about what a pass may NOT touch: directions. A radial climb on the
     * dome must change radii while every surviving direction stays put (the y>=0 floor clamp may
     * bend the lowest speakers; those are exempt). */
    t = IM_REGISTER_TEST(te, "logic", "radial");
    t->TestFunc = [](ImGuiTestContext*) {
        seed_default(); layout_dirty = 1;
        Vector3 dir0[NSPK]; float r0[NSPK];
        const Vector3 ear = { 0, obs_height, 0 };
        for (int i = 0; i < g_nspk; ++i) {
            Vector3 d = Vector3Subtract(spk[i].pos, ear);
            r0[i] = Vector3Length(d); dir0[i] = Vector3Scale(d, 1.0f / r0[i]);
        }
        srand(11);
        opt_radial = 1;
        opt_cost = opt_cost_of(BWA_PAN_DBAP); opt_step = 0.30f; opt_stall = 0; opt_iter = 0;
        for (int i = 0; i < g_nspk; ++i) opt_anchor[i] = spk[i].pos;
        optimize_step(BWA_PAN_DBAP, 60);
        opt_radial = 0;
        int moved = 0;
        for (int i = 0; i < g_nspk; ++i) {
            Vector3 d = Vector3Subtract(spk[i].pos, ear);
            float r = Vector3Length(d);
            if (fabsf(r - r0[i]) > 1e-4f) ++moved;
            if (spk[i].pos.y > 0.01f) {
                Vector3 dn = Vector3Scale(d, 1.0f / r);
                IM_CHECK_GT(Vector3DotProduct(dn, dir0[i]), 0.9999f);
            }
        }
        IM_CHECK_GT(moved, 0);                                   /* radii DID move; directions did not */
        seed_default(); layout_dirty = 1;
    };

    /* The guard is a veto, and the veto must actually bind: an impossible tolerance freezes the
     * layout (every improving trial fails the guard), a huge one reduces to the unguarded climb. */
    t = IM_REGISTER_TEST(te, "logic", "guard");
    t->TestFunc = [](ImGuiTestContext*) {
        seed_default(); layout_dirty = 1;
        Vector3 before[NSPK];
        for (int i = 0; i < g_nspk; ++i) before[i] = spk[i].pos;
        srand(3);
        opt_guard_panner = BWA_PAN_DBAP; opt_guard_tol = -1e9f;  /* nothing can satisfy this */
        opt_guard_base = opt_cost_of(BWA_PAN_DBAP);
        opt_cost = opt_cost_of(BWA_PAN_VBAP); opt_step = 0.30f; opt_stall = 0; opt_iter = 0;
        for (int i = 0; i < g_nspk; ++i) opt_anchor[i] = spk[i].pos;
        optimize_step(BWA_PAN_VBAP, 40);
        for (int i = 0; i < g_nspk; ++i) IM_CHECK(memcmp(&spk[i].pos, &before[i], sizeof(Vector3)) == 0);
        opt_guard_tol = 1e9f;                                    /* now the guard never binds */
        opt_cost = opt_cost_of(BWA_PAN_VBAP); opt_step = 0.30f; opt_stall = 0;
        optimize_step(BWA_PAN_VBAP, 120);
        int moved = 0;
        for (int i = 0; i < g_nspk; ++i) if (memcmp(&spk[i].pos, &before[i], sizeof(Vector3)) != 0) ++moved;
        IM_CHECK_GT(moved, 0);
        opt_guard_panner = -1; opt_guard_tol = 0.5f;
        seed_default(); layout_dirty = 1;
    };

    /* Annealing's safety net is best-so-far: cooling actually runs, best never sits above current,
     * and stopping restores the best layout even if the walk (or anything else) wandered off it. */
    t = IM_REGISTER_TEST(te, "logic", "anneal_best");
    t->TestFunc = [](ImGuiTestContext*) {
        seed_default(); layout_dirty = 1;
        srand(5);
        opt_sa = 1;
        set_optimizing(1);
        IM_CHECK_GT(opt_sa_t, 0.0f);                             /* the temperature armed */
        optimize_step(BWA_PAN_DBAP, 80);
        IM_CHECK_LT(opt_sa_t, SA_T0);                            /* per-trial cooling ran */
        IM_CHECK_LE(opt_best_cost, opt_cost + 1e-4f);            /* best <= current, always */
        Vector3 best5 = opt_best_pos[5];
        spk[5].pos = Vector3{ 9, 9, 9 };                         /* wreck the current state... */
        opt_cost = opt_cost_of(BWA_PAN_DBAP);
        set_optimizing(0);                                       /* ...stopping must ship the best */
        IM_CHECK_LT(fabsf(spk[5].pos.x - best5.x), 1e-5f);
        IM_CHECK_LT(fabsf(spk[5].pos.y - best5.y), 1e-5f);
        opt_sa = 0; opt_sa_t = 0.0f;
        seed_default(); layout_dirty = 1;
    };

    /* the GUI climb terminates itself: once the step decays to the floor, a tick stops the run
     * (restoring the best) instead of burning trials forever */
    t = IM_REGISTER_TEST(te, "logic", "auto_stop");
    t->TestFunc = [](ImGuiTestContext*) {
        seed_default(); layout_dirty = 1;
        srand(9);
        set_optimizing(1);
        IM_CHECK(opt_running);
        IM_CHECK(!opt_converged);
        opt_step = 0.02f;                                        /* at the floor: the next tick must stop */
        optimize_tick();
        IM_CHECK(!opt_running);
        IM_CHECK(opt_converged);
        set_optimizing(1);                                       /* restarting clears the converged flag */
        IM_CHECK(!opt_converged);
        IM_CHECK_GT(opt_step, 0.02f);                            /* ...and re-arms the step schedule */
        set_optimizing(0);
        seed_default(); layout_dirty = 1;
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

    /* the speaker-count control: shrinking retires the tail (and the selection follows), growing
     * places the new speaker on the dome — the array size IS the engine's channel count. */
    t = IM_REGISTER_TEST(te, "viewer", "panel_count");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const int keep = g_nspk;
        ctx->SetRef("layout");
        ctx->ItemInputValue("**/##spk", 25);
        IM_CHECK_EQ(sel, 25);
        ctx->ItemInputValue("**/count", 24);
        IM_CHECK_EQ(g_nspk, 24);
        IM_CHECK_EQ(sel, 23);                                    /* the retired selection moved in-range */
        ctx->ItemInputValue("**/count", 26);
        IM_CHECK_EQ(g_nspk, 26);
        IM_CHECK_GT(Vector3Length(spk[25].pos), 0.1f);           /* the re-added speaker landed on the dome */
        ctx->ItemInputValue("**/count", 2);                      /* below the engine's minimum: clamped */
        IM_CHECK_EQ(g_nspk, NSPK_MIN);
        g_nspk = keep; seed_default(); sel = 0; layout_dirty = 1;
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

    /* SPCAP's live knobs on the preview side, in the same group as the panner A/B: the derived
     * default reflects the EDITED geometry, the sliders arm only under SPCAP, and the button sends
     * the revert sentinel. Spreading the array wider must lower what it derives. */
    t = IM_REGISTER_TEST(te, "viewer", "spcap_focus");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("layout");
        ctx->ItemClick("**/Preview - audition [P]");
        IM_CHECK(preview);
        IM_CHECK_GT(pv_focus_def, 1.0f);                         /* the edited layout derives a usable one */
        IM_CHECK_LT(pv_focus_def, 64.0f);
        float dense = pv_focus_def;
        ctx->ItemClick("**/Back to edit [P]");

        int keep = g_nspk;                                       /* a 6-speaker array is far sparser */
        g_nspk = 6;
        mark_edit();
        ctx->ItemClick("**/Preview - audition [P]");
        IM_CHECK_LT(pv_focus_def, dense);                        /* wider spacing -> broader lobe */
        ctx->ItemClick("**/Back to edit [P]");
        g_nspk = keep;
        mark_edit();

        ctx->ItemClick("**/Preview - audition [P]");
        int keep_pan = pv_panner;
        pv_panner = BWA_PAN_SPCAP;                               /* arm the sliders (BeginDisabled gate) */
        if (e) bwa_set_panner(e, BWA_PAN_SPCAP);
        ctx->Yield(2);
        ctx->ItemInputValue("**/focus", 24.0f);
        IM_CHECK_EQ(pv_focus, 24.0f);
        ctx->ItemInputValue("**/density", 3.0f);
        IM_CHECK_EQ(pv_density, 3.0f);
        ctx->Yield(4);
        ctx->ItemClick("**/SPCAP default");                      /* the revert sentinel */
        IM_CHECK_EQ(pv_focus, 0.0f);
        IM_CHECK_EQ(pv_density, 0.0f);
        pv_panner = keep_pan;
        if (e) bwa_set_panner(e, (bwa_panner)pv_panner);
        ctx->ItemClick("**/Back to edit [P]");
        IM_CHECK(!preview);
    };

    /* the SCORING side of the same two globals: they ride bwa_panner_gains_batch's focus/density
     * arguments, so the board, the rE overlay and the optimizer cost must all MOVE with the knob —
     * and must not move for DBAP/VBAP, where the arguments are inert. Without this the tool could
     * only A/B the lobe by ear, which is the whole reason the signature widened. */
    t = IM_REGISTER_TEST(te, "viewer", "spcap_focus_score");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("layout");
        seed_default(); mark_edit();
        const float keep_f = pv_focus, keep_d = pv_density;
        const int   keep_pan = pv_panner;

        pv_focus = 0.0f; pv_density = 0.0f;                      /* the derived default */
        float m_def, w_def, sp_def;
        score_panner(BWA_PAN_SPCAP, 4, 0, &m_def, &w_def, &sp_def);
        pv_focus = 40.0f;                                        /* well above the grid's ~12.7 */
        float m_hi, w_hi, sp_hi;
        score_panner(BWA_PAN_SPCAP, 4, 0, &m_hi, &w_hi, &sp_hi);
        IM_CHECK_LT(sp_hi, sp_def - 1.0f);                       /* a tighter lobe = a narrower image */
        pv_focus = 3.0f;                                         /* well below it */
        float m_lo, w_lo, sp_lo;
        score_panner(BWA_PAN_SPCAP, 4, 0, &m_lo, &w_lo, &sp_lo);
        IM_CHECK_GT(sp_lo, sp_def + 1.0f);                       /* a broader lobe = a wider image */

        /* the per-direction overlay is the same solve, so it must follow too */
        pv_focus = 0.0f;  compute_cov_err(BWA_PAN_SPCAP);
        float e_def[NCOV]; memcpy(e_def, cov_err, sizeof e_def);
        pv_focus = 40.0f; compute_cov_err(BWA_PAN_SPCAP);
        float dmax = 0.0f;
        for (int i = 0; i < NCOV; ++i) { float d = fabsf(cov_err[i] - e_def[i]); if (d > dmax) dmax = d; }
        IM_CHECK_GT(dmax, 0.01f);

        /* inert under the other two panners: same arguments, bit-identical score */
        for (int p = 0; p < 3; ++p) {
            if (p == BWA_PAN_SPCAP) continue;
            float ma, wa, mb, wb;
            pv_focus = 0.0f;  pv_density = 0.0f;
            score_panner((bwa_panner)p, 4, panner_tracked((bwa_panner)p), &ma, &wa, NULL);
            pv_focus = 40.0f; pv_density = 6.0f;
            score_panner((bwa_panner)p, 4, panner_tracked((bwa_panner)p), &mb, &wb, NULL);
            IM_CHECK_EQ(ma, mb);
            IM_CHECK_EQ(wa, wb);
        }

        /* and the edit-panel sliders reach those globals + invalidate the cached score */
        pv_focus = 0.0f; pv_density = 0.0f;
        pv_panner = BWA_PAN_SPCAP;                               /* the sliders only draw under SPCAP */
        ctx->Yield(2);
        ctx->ItemClick("**/Score [X]");
        IM_CHECK(scored);
        IM_CHECK_EQ(score_stale, 0);
        const float board_def = score_spread[BWA_PAN_SPCAP];
        ctx->ItemInputValue("**/focus", 40.0f);                   /* the slider marks the board stale... */
        IM_CHECK_EQ(pv_focus, 40.0f);
        ctx->Yield(16);                                          /* ...and the throttled auto-refresh re-scores */
        IM_CHECK_LT(score_spread[BWA_PAN_SPCAP], board_def - 1.0f);
        IM_CHECK_EQ(score_stale, 0);                             /* the board is current again, at the new lobe */

        pv_focus = keep_f; pv_density = keep_d; pv_panner = keep_pan;
        mark_score();
    };

    /* the badness map through the panel, both solve modes, with a shot of each: the fixed one should
     * look like an island and the tracked one flat, which is the pair of pictures worth keeping */
    t = IM_REGISTER_TEST(te, "viewer", "badness_shots");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("layout");
        ctx->ItemCheck("**/badness map [M]");
        ctx->Yield(4);                                           /* compute + draw */
        IM_CHECK(bad_on && bad_valid);
        ctx->CaptureScreenshot();
        int before = bad_tracked;
        track_mode = (before == 1) ? 2 : 1;                      /* flip the solve mode and recompute */
        mark_score();
        ctx->Yield(8);
        IM_CHECK_NE(bad_tracked, before);
        ctx->CaptureReset();
        ctx->CaptureScreenshot();
        track_mode = 0;
        ctx->ItemUncheck("**/badness map [M]");
    };
}

/* ============================== main ============================== */

int main(int argc, char** argv) {
    /* headless (no window/audio, scriptable):
     *   --export   [file]            write the layout (default grid, or an existing file with delay_ms recomputed)
     *   --score    [file] [condition]   print each panner's rE-localization error for the layout,
     *                                under a named condition if given (default: the full sphere),
     *                                at an SPCAP tuning if given (focus=/density=; 0 = derived)
     *   --optimize [file] [panner] [stages]   hill-climb the layout for one panner (dbap|spcap|vbap, default
     *                                dbap) under a comma-separated chain of named conditions (default "3d";
     *                                "horizontal,3d" seeds the 3D climb from the plane-optimal result),
     *                                within constraints.json if present, save in place, print before/after
     *   --tests    [filter]          run the imgui_test_engine suite (logic + the real UI) and exit pass/fail */
    bool selftest = false;
    char filter[64] = "";
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("usage: bwa_layout_tool [cave_layout.json | mode]\n"
                   "  edit a speaker layout in 3D (default file: ./cave_layout.json;\n"
                   "  ./constraints.json bounds the placement if present)\n"
                   "  --export   [file]                    write the layout headless\n"
                   "  --score    [file] [condition] [fixed|moving] [ears=<m>] [focus=<n>]\n"
                   "             [density=<n>] [epad|allrad] [maxre]\n"
                   "             print each panner's rE-localization error, under a named condition\n"
                   "             if given (3d, horizontal, visual); 'fixed' scores the sweet spot only\n"
                   "             (a seated install) instead of the moving-listener grid; ears=<m>\n"
                   "             sets the listener ear height (the plane; default 1.4); focus=<n>\n"
                   "             and density=<n> score the SPCAP row at that tuning (0 = the value\n"
                   "             the geometry derives), so the knob can be swept offline; epad/maxre\n"
                   "             grade the AMBI row with the decode the install ships\n"
                   "  --optimize [file] [dbap|spcap|vbap] [stages] [fixed|moving] [ears=<m>] [radial]\n"
                   "             [guard=<panner>[:tol]] [anneal] [restarts=<n>] [leash=<m>] [bed=<wt>]\n"
                   "             hill-climb within constraints, save in place; stages = a comma-\n"
                   "             separated chain of conditions (3d, horizontal, visual), each stage\n"
                   "             seeding the next - horizontal,3d optimizes the plane first, then 3D;\n"
                   "             'fixed' optimizes for the sweet spot only, ears=<m> sets the\n"
                   "             listener ear height (the plane; default 1.4); 'radial' moves\n"
                   "             speakers only along the ear ray - refit radii for one panner\n"
                   "             without disturbing another's direction structure; guard=<panner>\n"
                   "             rejects moves that let that panner's cost slip more than tol\n"
                   "             (default 0.5) above its value at the stage start; 'anneal' allows\n"
                   "             uphill moves early (escapes local basins), restarts=<n> re-climbs\n"
                   "             from the best + a kick; both always ship the best layout seen;\n"
                   "             leash=<m> caps each speaker's move from the stage start,\n"
                   "             overriding the condition's own leash; bed=<wt> weighs the ambisonic\n"
                   "             BED's decode error into the cost (what diffuse content wants;\n"
                   "             --score prints its row either way); epad/allrad + maxre pick the\n"
                   "             bed decode graded, matching the install\n"
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
            printf("constraints: none (no ./constraints.json here) - every placement allowed except the y>=0 floor\n");
    }

    /* coverage/scoring shell: even directions on a sphere (Fibonacci) + a working-volume listener grid */
    for (int i = 0; i < NCOV; ++i) {
        float y = 1.0f - 2.0f * ((float)i + 0.5f) / NCOV;
        float r = sqrtf(1.0f - y * y), th = (float)i * 2.39996323f;   /* golden angle */
        cov_dir[i] = Vector3{ r * cosf(th), y, r * sinf(th) };
    }
    cov_lis[0] = Vector3{ 0, 0, 0 };
    /* Listener-movement envelope. HEIGHT gets a realistic +-0.45 m (seated to tall), not a token
     * nudge: bwa_validate measures off-height as a DISTINCT failure mode from off-center-in-plane —
     * the array's speakers sit mostly overhead and the alignment delays are computed for one
     * reference height, so tracking the listener re-aims the solve but cannot fix either. A grid
     * that barely varies height cannot see the problem it most needs to. */
    { const float ax[3] = { -1.0f, 0.0f, 1.0f }, ay[3] = { -0.45f, 0.0f, 0.45f }; int li = 1;
      for (int xi = 0; xi < 3; ++xi) for (int zi = 0; zi < 3; ++zi) for (int yi = 0; yi < 3; ++yi)
          if (!(ax[xi] == 0 && ay[yi] == 0 && ax[zi] == 0)) cov_lis[li++] = Vector3{ ax[xi], ay[yi], ax[zi] }; }

    if (export_only) {
        if (!save_json(g_path)) { printf("export failed: %s\n", g_path); return 1; }
        printf("exported layout -> %s (from %s)\n", g_path, loaded ? "existing file" : "default grid");
        return 0;
    }
    if (score_only) {                              /* headless: score the layout for each panner + exit,
                                                    * optionally under a named condition (argv[3]) */
        int ci = -1, ears_set = 0;
        for (int a = 3; a < argc && a < 12; ++a) {
            if      (!strcmp(argv[a], "fixed"))  score_fixed_obs = 1;   /* seated install: sweet spot only */
            else if (!strcmp(argv[a], "moving")) score_fixed_obs = 0;
            else if (!strcmp(argv[a], "epad"))   bed_decoder = 1;       /* grade the decode the install ships */
            else if (!strcmp(argv[a], "allrad")) bed_decoder = 0;
            else if (!strcmp(argv[a], "maxre"))  bed_max_re = 1;
            /* SPCAP's knobs: sweep the lobe headless. 0 is a LEGAL value here (the revert sentinel),
             * so a garbage token cannot be caught by the range alone - check that it parsed at all. */
            else if (!strncmp(argv[a], "focus=", 6)) {
                const char* v = argv[a] + 6;
                char* end = NULL;
                pv_focus = strtof(v, &end);
                if (end == v || *end || !(pv_focus >= 0.0f && pv_focus <= 64.0f)) {
                    printf("score: focus=%s is not a number in 0..64 (0 = the derived default)\n", v); return 1; }
            }
            else if (!strncmp(argv[a], "density=", 8)) {
                const char* v = argv[a] + 8;
                char* end = NULL;
                pv_density = strtof(v, &end);
                if (end == v || *end || !(pv_density >= 0.0f && pv_density <= 8.0f)) {
                    printf("score: density=%s is not a number in 0..8 (0 = the 2.0 default)\n", v); return 1; }
            }
            else if (!strncmp(argv[a], "ears=", 5)) {
                obs_height = (float)atof(argv[a] + 5);                  /* the plane / scoring / alignment height */
                if (!(obs_height > 0.0f && obs_height <= 3.0f)) { printf("score: ears=%s out of range (0..3 m)\n", argv[a] + 5); return 1; }
                ears_set = 1;
            }
            else {
                ci = condition_find(argv[a]);
                if (ci < 0) {
                    printf("score: unknown condition '%s' (have:", argv[a]);
                    for (int i = 0; i < NCONDITIONS; ++i) printf(" %s", opt_conditions[i].name);
                    printf(" fixed moving ears=<m> focus=<n> density=<n> epad allrad maxre)\n");
                    return 1;
                }
            }
        }
        printf("layout: %s (%s)", g_path, loaded ? "loaded" : "default grid");
        if (score_fixed_obs) printf("   observer: fixed (sweet spot only)");
        if (ears_set)        printf("   ears %.2f m", obs_height);
        if (pv_focus   > 0)  printf("   SPCAP focus %.2f", (double)pv_focus);
        if (pv_density > 0)  printf("   SPCAP density %.2f", (double)pv_density);
        if (ci >= 0) {
            const OptCondition* c = &opt_conditions[ci];
            printf("   condition '%s' (", c->name);
            if (c->band_deg > 0) printf("el +-%.0f deg", c->band_deg); else printf("full elevation");
            if (c->azi_deg  > 0) printf(", azi +-%.0f deg", c->azi_deg);
            printf(", elev wt %.2f)", c->elev_wt);
        }
        printf("\n");
        for (int p = 0; p < 3; ++p) {
            if (ci >= 0) apply_condition(ci, (bwa_panner)p);   /* per panner: the focus default differs */
            float m, w, sp; score_panner((bwa_panner)p, 1, panner_tracked((bwa_panner)p), &m, &w, &sp);
            printf("  %-14s rE-localize error:  mean %4.1f deg   worst %4.1f deg   focus (Frank spread) %4.1f deg\n",
                   panner_names[p], m, w, sp);
        }
        { float bm, bw, bs; score_bed(1, &bm, &bw, &bs);       /* what bed content gets from this array */
          char lbl[24]; snprintf(lbl, sizeof lbl, "AMBI (%s)", bed_decoder ? "EPAD" : "AllRAD");
          printf("  %-14s rE-localize error:  mean %4.1f deg   worst %4.1f deg   focus (Frank spread) %4.1f deg%s\n",
                 lbl, bm, bw, bs, bed_max_re ? "   [max-rE]" : ""); }
        return 0;
    }
    if (optimize_only) {                           /* headless: optimize in place for one panner + save.
                                                    * STAGES chain conditions: each stage re-anchors at the
                                                    * previous result, so `horizontal,3d` seeds the 3D climb
                                                    * with the plane-optimal layout. */
        bwa_panner p = BWA_PAN_DBAP;
        int stages[8], nstages = 0;
        int ears_set = 0;
        float cli_leash = -1.0f;                   /* >0 = override every stage's condition leash */
        for (int a = 3; a < argc && a < 15; ++a) { /* panner, stages, observer, ears, radial, guard, anneal,
                                                    * restarts, leash, bed, epad/allrad, maxre: any order */
            if      (!strcmp(argv[a], "dbap"))  p = BWA_PAN_DBAP;
            else if (!strcmp(argv[a], "spcap")) p = BWA_PAN_SPCAP;
            else if (!strcmp(argv[a], "vbap"))  p = BWA_PAN_VBAP;
            else if (!strcmp(argv[a], "fixed"))  score_fixed_obs = 1;   /* seated install: optimize FOR the sweet spot */
            else if (!strcmp(argv[a], "moving")) score_fixed_obs = 0;
            else if (!strcmp(argv[a], "radial")) opt_radial = 1;        /* refit radii only; keep the direction structure */
            else if (!strcmp(argv[a], "anneal")) opt_sa = 1;            /* Metropolis acceptance (escape local basins) */
            else if (!strncmp(argv[a], "restarts=", 9)) {
                opt_restarts = atoi(argv[a] + 9);
                if (opt_restarts < 1 || opt_restarts > 16) { printf("optimize: restarts=%s out of range (1..16)\n", argv[a] + 9); return 1; }
            }
            else if (!strncmp(argv[a], "guard=", 6)) {                  /* protect another panner while climbing this one */
                char nm[24]; snprintf(nm, sizeof nm, "%s", argv[a] + 6);
                char* colon = strchr(nm, ':');
                if (colon) { *colon = 0; opt_guard_tol = (float)atof(colon + 1); }
                if      (!strcmp(nm, "dbap"))  opt_guard_panner = BWA_PAN_DBAP;
                else if (!strcmp(nm, "spcap")) opt_guard_panner = BWA_PAN_SPCAP;
                else if (!strcmp(nm, "vbap"))  opt_guard_panner = BWA_PAN_VBAP;
                else { printf("optimize: guard=%s is not a panner (dbap|spcap|vbap)\n", nm); return 1; }
            }
            else if (!strncmp(argv[a], "ears=", 5)) {
                obs_height = (float)atof(argv[a] + 5);                  /* moves the plane, the pin slab, and the
                                                                         * delay-alignment point on save */
                if (!(obs_height > 0.0f && obs_height <= 3.0f)) { printf("optimize: ears=%s out of range (0..3 m)\n", argv[a] + 5); return 1; }
                ears_set = 1;
            }
            else if (!strncmp(argv[a], "leash=", 6)) {                  /* cap displacement from the stage anchor */
                cli_leash = (float)atof(argv[a] + 6);
                if (!(cli_leash >= 0.05f && cli_leash <= 10.0f)) { printf("optimize: leash=%s out of range (0.05..10 m)\n", argv[a] + 6); return 1; }
            }
            else if (!strncmp(argv[a], "bed=", 4)) {                    /* weight the AMBI-bed decode into the cost */
                opt_bed_wt = (float)atof(argv[a] + 4);
                if (!(opt_bed_wt >= 0.0f && opt_bed_wt <= 10.0f)) { printf("optimize: bed=%s out of range (0..10)\n", argv[a] + 4); return 1; }
            }
            else if (!strcmp(argv[a], "epad"))   bed_decoder = 1;       /* grade the decode the install ships */
            else if (!strcmp(argv[a], "allrad")) bed_decoder = 0;
            else if (!strcmp(argv[a], "maxre"))  bed_max_re = 1;
            else {
                char buf[128]; snprintf(buf, sizeof buf, "%s", argv[a]);
                for (char* tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
                    int ci = condition_find(tok);
                    if (ci < 0) {
                        printf("optimize: unknown condition '%s' (have:", tok);
                        for (int i = 0; i < NCONDITIONS; ++i) printf(" %s", opt_conditions[i].name);
                        printf(" fixed moving)\n");
                        return 1;
                    }
                    if (nstages < 8) stages[nstages++] = ci;
                }
            }
        }
        if (!nstages) stages[nstages++] = 0;                                   /* default: the 3d condition */
        const int last = stages[nstages - 1];
        if (ears_set) printf("  ears: %.2f m (the plane, scoring, and delay-alignment height)\n", obs_height);
        if (opt_sa || opt_restarts > 1)
            printf("  search: %s, %d restart(s), best-so-far kept\n", opt_sa ? "annealed" : "greedy", opt_restarts);
        if (opt_bed_wt > 0.0f || bed_decoder || bed_max_re)
            printf("  bed: %s%s decode, weight %.2f in the objective\n",
                   bed_decoder ? "EPAD" : "AllRAD", bed_max_re ? " + max-rE" : "", opt_bed_wt);
        { int np = 0; for (int i = 0; i < g_nspk; ++i) np += spk[i].pin;
          if (np) printf("  pins: %d speaker(s) held to the ear plane (slab +-%.2f m)\n", np, pin_slab_m); }
        apply_condition(last, p);                  /* before/after report under the objective the result ships against */
        float m0, w0; score_panner(p, 1, panner_tracked(p), &m0, &w0, NULL);
        int iters_total = 0;
        for (int s = 0; s < nstages; ++s) {
            const OptCondition* c = &opt_conditions[stages[s]];
            apply_condition(stages[s], p);
            if (cli_leash > 0.0f) opt_leash = cli_leash;   /* an explicit leash beats the condition's */
            for (int i = 0; i < g_nspk; ++i)       /* start feasible: pins and room constraints alike (a generated
                                                    * or hand-edited file may violate either; trials are projected,
                                                    * the incoming layout must be too). BEFORE the before-score, so
                                                    * the stage report and the guard baseline measure the same state */
                spk[i].pos = constraint_project(pin_project(spk[i].pos, spk[i].pin));
            float sm0, sw0; score_panner(p, 1, panner_tracked(p), &sm0, &sw0, NULL);
            float gm0 = 0, gw0 = 0;
            if (opt_guard_panner >= 0) {           /* the guard baseline: where this stage must not slip from
                                                    * (bed-term-free, matching the veto's evaluation) */
                float bw_save = opt_bed_wt; opt_bed_wt = 0.0f;
                opt_guard_base = opt_cost_of((bwa_panner)opt_guard_panner);
                opt_bed_wt = bw_save;
                score_panner((bwa_panner)opt_guard_panner, 1, panner_tracked((bwa_panner)opt_guard_panner), &gm0, &gw0, NULL);
            }
            opt_iter = 0;
            for (int i = 0; i < g_nspk; ++i) opt_anchor[i] = spk[i].pos;       /* re-anchor: start from the previous stage */
            opt_best_cost = 1e30f;
            for (int r = 0; r < opt_restarts; ++r) {
                if (r > 0) {                       /* basin hop: restart from the best layout plus a kick */
                    for (int i = 0; i < g_nspk; ++i) spk[i].pos = opt_best_pos[i];
                    for (int i = 0; i < g_nspk; ++i) {
                        Vector3 kp;
                        if (opt_radial) {          /* radial mode's direction promise holds through the kick */
                            Vector3 rd = Vector3Subtract(spk[i].pos, Vector3{ 0, obs_height, 0 });
                            float rl = Vector3Length(rd);
                            kp = (rl < 1e-3f) ? spk[i].pos
                               : Vector3Add(spk[i].pos, Vector3Scale(rd, 0.25f * (2*frand()-1) / rl));
                        } else {
                            kp = Vector3{ spk[i].pos.x + 0.25f*(2*frand()-1),
                                          spk[i].pos.y + 0.25f*(2*frand()-1),
                                          spk[i].pos.z + 0.25f*(2*frand()-1) };
                        }
                        spk[i].pos = constraint_project(pin_project(kp, spk[i].pin));
                    }
                }
                opt_cost = opt_cost_of(p); opt_step = 0.30f; opt_stall = 0;
                opt_sa_t = opt_sa ? SA_T0 : 0.0f;
                if (r == 0 && opt_cost < opt_best_cost) {    /* the incoming layout is a valid best; a
                                                              * kicked state is not (it skipped the guard) */
                    opt_best_cost = opt_cost;
                    for (int i = 0; i < g_nspk; ++i) opt_best_pos[i] = spk[i].pos;
                }
                while (opt_step > 0.02f && opt_iter < 120000) optimize_step(p, 200);   /* run to the step floor */
                if (opt_restarts > 1)
                    printf("    restart %d/%d:  cost %.1f   best %.1f\n", r + 1, opt_restarts, opt_cost, opt_best_cost);
            }
            for (int i = 0; i < g_nspk; ++i) spk[i].pos = opt_best_pos[i];     /* ship the best layout seen */
            float sm1, sw1; score_panner(p, 1, panner_tracked(p), &sm1, &sw1, NULL);
            if (opt_guard_panner >= 0) {
                float gm1, gw1;
                score_panner((bwa_panner)opt_guard_panner, 1, panner_tracked((bwa_panner)opt_guard_panner), &gm1, &gw1, NULL);
                printf("    guard %-5s (tol %.2f):  mean %.1f -> %.1f deg   worst %.1f -> %.1f deg\n",
                       panner_names[opt_guard_panner], opt_guard_tol, gm0, gm1, gw0, gw1);
            }
            if (opt_bed_wt > 0.0f) {
                float bm1, bw1;
                score_bed(1, &bm1, &bw1, NULL);
                printf("    bed (wt %.2f):  mean %.1f deg   worst %.1f deg  (after this stage)\n",
                       opt_bed_wt, bm1, bw1);
            }
            char bandtxt[48];
            if      (c->band_deg > 0 && c->azi_deg > 0)
                snprintf(bandtxt, sizeof bandtxt, "el +-%.0f, azi +-%.0f deg", c->band_deg, c->azi_deg);
            else if (c->band_deg > 0) snprintf(bandtxt, sizeof bandtxt, "band %.0f deg", c->band_deg);
            else if (c->azi_deg  > 0) snprintf(bandtxt, sizeof bandtxt, "azi +-%.0f deg", c->azi_deg);
            else                      snprintf(bandtxt, sizeof bandtxt, "full sphere");
            printf("  stage %-10s (%s, leash %.1f m%s):  mean %.1f -> %.1f deg   worst %.1f -> %.1f deg   "
                   "(%d iters, %d/%d speakers within 0.5 m of the ear plane)\n",
                   c->name, bandtxt, opt_leash, opt_radial ? ", radial" : "",
                   sm0, sm1, sw0, sw1, opt_iter, plane_count(0.5f), g_nspk);
            iters_total += opt_iter;
        }
        apply_condition(last, p);
        float m1, w1; score_panner(p, 1, panner_tracked(p), &m1, &w1, NULL);
        if (!save_json(g_path)) { printf("optimize: save failed: %s\n", g_path); return 1; }
        printf("optimized %s for %-5s under '%s'%s:  rE mean %.1f -> %.1f deg   worst %.1f -> %.1f deg   (%d iters%s)\n",
               g_path, panner_names[p], opt_conditions[last].name,
               score_fixed_obs ? " [fixed observer]" : "", m0, m1, w0, w1, iters_total,
               CON.loaded ? ", within constraints" : "");
        return 0;
    }
    printf("layout: %s (%s, %d speakers)\n", g_path, loaded ? "loaded" : "default dome", g_nspk);

    /* cave profile so the test signal / DBAP preview goes out the Digiface to the real speakers;
     * no audio (the editor still works) if no such ASIO device is present (off-site). */
    gen_pink_wav(PREV_WAV);                        /* the moving DBAP-preview source signal */
    build_engine(NULL);                            /* edit-mode engine (the test signal is layout-independent) */

    /* No FLAG_WINDOW_HIGHDPI: on Windows the framebuffer matches the window pixels either way, and
     * without the flag rlImGui keeps DisplayFramebufferScale at 1 so screenshots/scissors stay 1:1;
     * DPI rides the theme's FontScaleMain instead (the same pattern as calib_view — and as rlImGui's
     * own default-font path). */
    SetConfigFlags(FLAG_MSAA_4X_HINT | (selftest ? FLAG_WINDOW_UNFOCUSED : 0));
    InitWindow(1280, 800, "bw_audio - speaker layout tool");
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

        if (opt_running && !preview)                      /* auto-optimizer: a few trials per frame, then
                                                           * auto-stop at the step floor (optimize_tick) */
            optimize_tick();
        for (int i = 0; i < g_nspk; ++i) if (spk[i].pos.y < 0.0f) spk[i].pos.y = 0.0f;   /* y >= 0: speakers never below the floor */
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
    if (e) { bwa_stop(e); bwa_destroy(e); }
    remove(PREV_WAV); remove(TEMP_LAYOUT);
    return rc;
}
