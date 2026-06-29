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
 *           ENTER type "x y z"   PgUp/PgDn gain_db   T tone   N sine/noise   C coverage   V observer   S save   L reload
 * Controls (preview, P toggles): WASD/RF move the source   SPACE auto-orbit/near-far/high-low sweep
 *           B A/B the panner DBAP<->SPCAP live (SPCAP is the fixed-observer sweet-spot panner)
 *           Common: right-drag/wheel camera   ESC quit
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

int main(int argc, char** argv) {
    /* headless: `bw_layout_tool --export [file]` writes the layout (default grid, or an existing file
     * with delay_ms recomputed from positions) and exits — scriptable, no window/audio. */
    int export_only = (argc > 1 && strcmp(argv[1], "--export") == 0);
    const char* path = export_only ? (argc > 2 ? argv[2] : "cave_layout.json")
                                    : (argc > 1 ? argv[1] : "cave_layout.json");
    seed_default();
    int loaded = load_json(path);
    if (export_only) {
        if (!save_json(path)) { printf("export failed: %s\n", path); return 1; }
        printf("exported layout -> %s (from %s)\n", path, loaded ? "existing file" : "default grid");
        return 0;
    }
    printf("layout: %s (%s, %d speakers)\n", path, loaded ? "loaded" : "default grid", loaded ? loaded : NSPK);

    /* cave profile so the test signal / DBAP preview goes out the 26-ch DVS to the real speakers;
     * falls back to no audio (editor still works) if no 26-ch ASIO device is present (off-site). */
    _putenv("BWAUDIO_SINK=asio");
    gen_pink_wav(PREV_WAV);                        /* the moving DBAP-preview source signal */
    build_engine(NULL);                           /* edit-mode engine (the test signal is layout-independent) */

    /* coverage shell: even directions on a sphere (Fibonacci) + a working-volume listener grid */
    for (int i = 0; i < NCOV; ++i) {
        float y = 1.0f - 2.0f * ((float)i + 0.5f) / NCOV;
        float r = sqrtf(1.0f - y * y), th = (float)i * 2.39996323f;   /* golden angle */
        cov_dir[i] = (Vector3){ r * cosf(th), y, r * sinf(th) };
    }
    cov_lis[0] = (Vector3){ 0, 0, 0 };
    { const float ax[3] = { -1.0f, 0.0f, 1.0f }, ay[3] = { -0.3f, 0.0f, 0.3f }; int li = 1;   /* listener-movement envelope */
      for (int xi = 0; xi < 3; ++xi) for (int zi = 0; zi < 3; ++zi) for (int yi = 0; yi < 3; ++yi)
          if (!(ax[xi] == 0 && ay[yi] == 0 && ax[zi] == 0)) cov_lis[li++] = (Vector3){ ax[xi], ay[yi], ax[zi] }; }

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
                if (sscanf(ibuf, "%f %f %f", &x, &y, &z) == 3) { spk[sel].pos = (Vector3){ x, y, z }; layout_dirty = 1; }
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
            if (IsKeyPressed(KEY_L)) { load_json(path); layout_dirty = 1; }
            if (IsKeyPressed(KEY_C)) coverage_on = !coverage_on;       /* coverage overlay */
            if (IsKeyPressed(KEY_V)) coverage_moving = !coverage_moving;
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
                IsKeyDown(KEY_R) || IsKeyDown(KEY_F) || IsKeyDown(KEY_PAGE_UP) || IsKeyDown(KEY_PAGE_DOWN)) layout_dirty = 1;
            if (IsKeyPressed(KEY_P) && !editing) {        /* enter DBAP preview — rebuild so it pans through the edited layout */
                if (driven >= 0 && e) { bw_test_signal(e, (uint32_t)driven, BW_TEST_OFF, 0.0f); driven = -1; }
                tone_on = 0; pv_orbit = 0; pv_t = 0.0f;   /* each preview session starts manual, fresh orbit phase */
                if (layout_dirty && e && audio) {     /* rebuild only when there's a device to hear it on */
                    save_json(TEMP_LAYOUT);
                    bw_stop(e); bw_destroy(e);
                    build_engine(TEMP_LAYOUT);
                    layout_dirty = 0; driven = -1;
                }
                preview = 1;
                if (e) {
                    bw_set_panner(e, (BwPanner)pv_panner);                      /* rebuilt engine defaults to DBAP */
                    bw_source_set_gain(e, pv_src, SRC_GAIN);
                    bw_commit(e);
                }
            }
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

        BeginDrawing();
        ClearBackground((Color){ 22, 22, 28, 255 });
        BeginMode3D(cam);
        DrawGrid(16, 0.5f);
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
        }
        if (preview) {                                   /* the moving DBAP source */
            DrawLine3D((Vector3){ 0, 0, 0 }, src_pos, (Color){ 90, 220, 90, 200 });
            DrawSphere(src_pos, 0.16f, (Color){ 240, 120, 90, 255 });
        }
        if (coverage_on && !preview) {                   /* shade each source direction by its nearest-speaker gap */
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
            DrawText("[ ] select (or click)   arrows X/Z  R/F Y (SHIFT fine)   ENTER type x y z   PgUp/Dn gain   T tone  N noise   C coverage  V obs   P preview   S save  L reload",
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
        if (coverage_on && !preview) {                   /* bottom: the angular-coverage summary */
            int yb = GetScreenHeight() - 26;
            DrawRectangle(0, yb - 5, GetScreenWidth(), 31, (Color){ 0, 0, 0, 195 });
            DrawText(TextFormat("angular coverage [%s]   worst direction %.0f deg   mean %.0f deg   green=covered  red=gap   (V toggles observer, C hides)",
                                coverage_moving ? "moving: mean over working volume" : "fixed: centre sweet spot", cov_worst, cov_mean),
                     10, yb, 15, cov_worst > 45.0f ? (Color){ 245, 150, 110, 255 } : (Color){ 150, 225, 160, 255 });
        }
        EndDrawing();
    }

    CloseWindow();
    if (e) { bw_stop(e); bw_destroy(e); }
    remove(PREV_WAV); remove(TEMP_LAYOUT);
    return 0;
}
