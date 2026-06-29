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
 * Controls: [ ] select speaker (or left-click)   arrows move X/Z, R/F move Y (hold SHIFT = fine)
 *           ENTER type exact "x y z"   PgUp/PgDn gain_db   T tone on/off   N sine/noise
 *           S save   L reload   right-drag/wheel camera   ESC quit
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

    /* cave profile so the test signal goes out the 26-ch DVS to the real speakers; falls back to no
     * audio (editor still works) if no 26-ch ASIO device is present (e.g. off-site, no Dante). */
    _putenv("BWAUDIO_SINK=asio");
    BwConfig cfg = {
        .profile = BW_PROFILE_CAVE, .layout_path = NULL, .hrtf_path = NULL,
        .sample_rate = SR, .block_size = 256, .track_internal = false,
    };
    BwEngine* e = bw_create(&cfg);
    const char* backend = "none";
    int audio = 0;
    if (e) {
        if (bw_start(e) != 0) {
            const char* err = bw_last_error(e);
            printf("bw_start: %s — no audition (need a 26-ch ASIO device / DVS); the editor still runs.\n",
                   err ? err : "?");
        }
        backend = bw_audio_backend(e);
        audio = (strncmp(backend, "asio", 4) == 0);
    }

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
                if (sscanf(ibuf, "%f %f %f", &x, &y, &z) == 3) spk[sel].pos = (Vector3){ x, y, z };
                editing = 0; ilen = 0; ibuf[0] = 0;
            }
            if (IsKeyPressed(KEY_ESCAPE)) { editing = 0; ilen = 0; ibuf[0] = 0; }
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
            if (IsKeyPressed(KEY_L)) load_json(path);
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {  /* click-pick the nearest speaker (not while orbiting) */
                Ray ray = GetMouseRay(GetMousePosition(), cam);
                float best = 1e9f; int hit = -1;
                for (int i = 0; i < NSPK; ++i) {
                    RayCollision rc = GetRayCollisionSphere(ray, spk[i].pos, 0.16f);
                    if (rc.hit && rc.distance < best) { best = rc.distance; hit = i; }
                }
                if (hit >= 0) sel = hit;
            }
        }

        /* drive the selected speaker's channel when auditioning; clear the old one on a change */
        if (audio) {
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
        EndMode3D();

        /* 2D index labels projected from 3D */
        for (int i = 0; i < NSPK; ++i) {
            Vector2 s = GetWorldToScreen(spk[i].pos, cam);
            DrawText(TextFormat("%d", i), (int)s.x + 6, (int)s.y - 6, i == sel ? 18 : 12,
                     i == sel ? (Color){ 250, 230, 120, 255 } : (Color){ 150, 150, 170, 220 });
        }

        /* HUD */
        DrawRectangle(0, 0, GetScreenWidth(), 96, (Color){ 0, 0, 0, 200 });
        DrawText("[ ] select (or click)   arrows X/Z  R/F Y  (SHIFT fine)   ENTER type x y z   PgUp/Dn gain   T tone  N sine/noise   S save  L reload",
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
        DrawText(audio ? TextFormat("audio: %s  (T drives the selected channel out the array)", backend)
                       : "audio: none - editor only (needs a 26-ch ASIO/DVS device to audition)",
                 10, 76, 14, audio ? (Color){ 110, 235, 130, 255 } : (Color){ 235, 170, 110, 255 });
        if (save_flash > 0)
            DrawText(TextFormat("saved -> %s", path), GetScreenWidth() - 320, 76, 15, (Color){ 120, 245, 140, 255 });
        else if (save_flash < 0)
            DrawText("SAVE FAILED (path not writable?)", GetScreenWidth() - 320, 76, 15, (Color){ 245, 120, 120, 255 });
        EndDrawing();
    }

    CloseWindow();
    if (e) { bw_stop(e); bw_destroy(e); }
    return 0;
}
