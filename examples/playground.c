/*
 * playground.c — interactive raylib scene driving bwaudio's binaural monitor.
 *
 * Drag a sound source around a 26-speaker CAVE and turn your head; hear the result on
 * headphones in real time (the binaural profile, output through a 2-ch ASIO driver that
 * the engine auto-picks — ASIO4ALL / FlexASIO / the Steinberg built-in). This is the
 * by-ear evaluation the automated tests can't do. Uses only the public C ABI (bwaudio.h);
 * raylib's raymath provides the vector/quaternion math.
 *
 * A wall demonstrates two acoustic situations: move the source in FRONT of it to hear a
 * specular reflection (rendered as a mirror-image source — a second DBAP voice), or BEHIND it
 * for occlusion (the direct path is blocked and attenuated). The image-source reflection has no
 * delay or material filtering yet — that's the future Steam Audio reflection path (materials.md).
 *
 * Controls: WASD/RF move source, Q/E turn head, [ ] slide wall, T reflection, G occlusion,
 *           right-drag orbit, wheel zoom, ESC quit.
 * Build: cmake -S . -B build -DBWAUDIO_BUILD_PLAYGROUND=ON && cmake --build build
 */
#include "bwaudio.h"
#include "raylib.h"
#include "raymath.h"

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR   48000u
#define NSPK 26

/* the engine's default speaker grid (3x3x3 minus centre) — must match layout_default() */
static int default_speakers(Vector3* out) {
    const float ax[3] = { -1.5f, 0.f, 1.5f };
    int k = 0;
    for (int yi = 0; yi < 3; ++yi) for (int xi = 0; xi < 3; ++xi) for (int zi = 0; zi < 3; ++zi) {
        if (ax[xi] == 0 && ax[yi] == 0 && ax[zi] == 0) continue;
        out[k++] = (Vector3){ ax[xi], ax[yi], ax[zi] };
    }
    return k;
}

/* 1 s of mono broadband noise (good for localization) -> a float wav the engine can load */
static int write_noise_wav(const char* path) {
    drwav_data_format fmt = { drwav_container_riff, DR_WAVE_FORMAT_IEEE_FLOAT, 1, SR, 32 };
    drwav wav;
    if (!drwav_init_file_write(&wav, path, &fmt, NULL)) return 0;
    const drwav_uint64 n = SR;
    float* buf = (float*)malloc((size_t)n * sizeof(float));
    if (!buf) { drwav_uninit(&wav); return 0; }
    unsigned int s = 12345u;                                  /* deterministic LCG noise */
    for (drwav_uint64 i = 0; i < n; ++i) {
        s = s * 1664525u + 1013904223u;
        float r = (float)((int)(s >> 9) - (1 << 22)) / (float)(1 << 22);
        buf[i] = r * 0.15f;
    }
    drwav_write_pcm_frames(&wav, n, buf);
    free(buf);
    drwav_uninit(&wav);
    return 1;
}

#define SRC_GAIN  0.8f
#define REFL_GAIN 0.5f    /* image-source level for a moderately reflective wall */
#define OCC_GAIN  0.22f   /* direct level when the wall occludes the source */

/* reflect a point across the plane (point c, unit normal n) — the image source */
static Vector3 reflect_point(Vector3 p, Vector3 c, Vector3 n) {
    float d = Vector3DotProduct(Vector3Subtract(p, c), n);
    return Vector3Subtract(p, Vector3Scale(n, 2.0f * d));
}
/* where segment a->b crosses plane (c,n): fills t and hit, returns 0 if ~parallel */
static int seg_plane(Vector3 a, Vector3 b, Vector3 c, Vector3 n, float* t, Vector3* hit) {
    Vector3 ab = Vector3Subtract(b, a);
    float denom = Vector3DotProduct(ab, n);
    if (fabsf(denom) < 1e-6f) return 0;
    float tt = Vector3DotProduct(Vector3Subtract(c, a), n) / denom;
    *t = tt; *hit = Vector3Add(a, Vector3Scale(ab, tt));
    return 1;
}
/* in-plane basis for a wall with normal n (for the panel extent test + drawing) */
static void wall_basis(Vector3 n, Vector3* u, Vector3* v) {
    Vector3 up = (fabsf(n.y) > 0.9f) ? (Vector3){ 1, 0, 0 } : (Vector3){ 0, 1, 0 };
    *u = Vector3Normalize(Vector3CrossProduct(up, n));
    *v = Vector3CrossProduct(n, *u);
}
static int in_panel(Vector3 p, Vector3 c, Vector3 u, Vector3 v, float hw, float hh) {
    Vector3 d = Vector3Subtract(p, c);
    return fabsf(Vector3DotProduct(d, u)) <= hw && fabsf(Vector3DotProduct(d, v)) <= hh;
}
static void draw_wall(Vector3 c, Vector3 u, Vector3 v, float hw, float hh, Color fill, Color line) {
    Vector3 a  = Vector3Add(Vector3Add(c, Vector3Scale(u, -hw)), Vector3Scale(v, -hh));
    Vector3 b  = Vector3Add(Vector3Add(c, Vector3Scale(u,  hw)), Vector3Scale(v, -hh));
    Vector3 d  = Vector3Add(Vector3Add(c, Vector3Scale(u,  hw)), Vector3Scale(v,  hh));
    Vector3 ee = Vector3Add(Vector3Add(c, Vector3Scale(u, -hw)), Vector3Scale(v,  hh));
    DrawTriangle3D(a, b, d, fill); DrawTriangle3D(a, d, ee, fill);    /* front face */
    DrawTriangle3D(a, d, b, fill); DrawTriangle3D(a, ee, d, fill);    /* back (double-sided) */
    DrawLine3D(a, b, line); DrawLine3D(b, d, line); DrawLine3D(d, ee, line); DrawLine3D(ee, a, line);
}

int main(void) {
    _putenv("BWAUDIO_SINK=asio");                             /* headphone output via a 2-ch ASIO driver */

    const char* WAV = "playground_src.wav";
    if (!write_noise_wav(WAV)) { printf("could not write %s\n", WAV); return 1; }

    BwConfig cfg = {
        .profile = BW_PROFILE_BINAURAL, .layout_path = NULL, .hrtf_path = NULL,
        .sample_rate = SR, .block_size = 256, .track_internal = false,
    };
    BwEngine* e = bw_create(&cfg);
    if (!e) { printf("bw_create failed\n"); return 1; }
    if (bw_start(e) != 0) {
        const char* err = bw_last_error(e);
        printf("bw_start: %s — no audio (install/select an ASIO driver, e.g. ASIO4ALL); the scene still runs.\n",
               err ? err : "?");
    }
    const char* backend = bw_audio_backend(e);                /* "asio:<driver>", "null", or "none" */
    int silent = (strncmp(backend, "asio", 4) != 0);          /* anything but a real ASIO device = no sound */
    printf("audio backend: %s%s\n", backend,
           silent ? "   (SILENT — set BWAUDIO_ASIO_DRIVER to your headphone driver)" : "");

    BwSound  snd = bw_load_sound(e, WAV);
    BwSource src = bw_source_create(e);
    bw_source_set_gain(e, src, SRC_GAIN);
    bw_source_play(e, src, snd, true);

    /* a second voice rendered at the source's mirror image across the wall — an audible
     * single specular reflection, using only the existing per-source DBAP path (no delay or
     * material filtering yet; that's the future Steam Audio reflection path, docs/materials.md) */
    BwSource refl = bw_source_create(e);
    bw_source_play(e, refl, snd, true);
    bw_source_set_gain(e, refl, 0.0f);

    Vector3 speakers[NSPK];
    default_speakers(speakers);
    Vector3 source_pos = { 1.5f, 0.0f, 0.0f };
    float   head_yaw = 0.0f;                                  /* radians, about +y */

    /* the reflecting/occluding surface: a vertical wall (normal +z) the source moves in front
     * of (reflection) or behind (occlusion). Slide it along its normal with [ and ]. */
    Vector3 wall_c = { 0.0f, 0.6f, -2.2f };
    const Vector3 wall_n = { 0, 0, 1 };
    const float wall_hw = 2.0f, wall_hh = 1.2f;
    Vector3 wall_u, wall_v; wall_basis(wall_n, &wall_u, &wall_v);
    int refl_audible = 1, occ_audible = 1;                   /* T / G toggles */

    InitWindow(1000, 700, "bwaudio - binaural playground");
    SetTargetFPS(60);
    Camera3D cam = { .target = { 0, 0.5f, 0 }, .up = { 0, 1, 0 },
                     .fovy = 55, .projection = CAMERA_PERSPECTIVE };
    float cam_yaw = 45.0f * DEG2RAD, cam_pitch = 33.0f * DEG2RAD, cam_dist = 8.4f;  /* arcball orbit state */

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        float mv = 2.5f * dt, rt = 1.8f * dt;
        if (IsKeyDown(KEY_W)) source_pos.z -= mv;
        if (IsKeyDown(KEY_S)) source_pos.z += mv;
        if (IsKeyDown(KEY_A)) source_pos.x -= mv;
        if (IsKeyDown(KEY_D)) source_pos.x += mv;
        if (IsKeyDown(KEY_R)) source_pos.y += mv;
        if (IsKeyDown(KEY_F)) source_pos.y -= mv;
        if (IsKeyDown(KEY_Q)) head_yaw += rt;
        if (IsKeyDown(KEY_E)) head_yaw -= rt;
        if (IsKeyDown(KEY_LEFT_BRACKET))  wall_c = Vector3Add(wall_c, Vector3Scale(wall_n, -mv));
        if (IsKeyDown(KEY_RIGHT_BRACKET)) wall_c = Vector3Add(wall_c, Vector3Scale(wall_n,  mv));
        if (IsKeyPressed(KEY_T)) refl_audible = !refl_audible;
        if (IsKeyPressed(KEY_G)) occ_audible  = !occ_audible;

        /* arcball camera: right-drag orbits around the array, the wheel zooms */
        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
            Vector2 md = GetMouseDelta();
            cam_yaw   -= md.x * 0.005f;
            cam_pitch += md.y * 0.005f;
            if (cam_pitch >  1.5f) cam_pitch =  1.5f;       /* clamp shy of the poles */
            if (cam_pitch < -1.5f) cam_pitch = -1.5f;
        }
        cam_dist -= GetMouseWheelMove() * 0.6f;
        if (cam_dist < 1.5f)  cam_dist = 1.5f;
        if (cam_dist > 25.0f) cam_dist = 25.0f;
        cam.position.x = cam.target.x + cam_dist * cosf(cam_pitch) * sinf(cam_yaw);
        cam.position.y = cam.target.y + cam_dist * sinf(cam_pitch);
        cam.position.z = cam.target.z + cam_dist * cosf(cam_pitch) * cosf(cam_yaw);

        Quaternion q = QuaternionFromAxisAngle((Vector3){ 0, 1, 0 }, head_yaw);

        /* reflection: the source mirrored across the wall is a valid specular reflection when
         * source and listener are on the same side and the bounce lands on the panel. occlusion:
         * the direct path crosses the panel (source pushed behind the wall). */
        const Vector3 L = { 0, 0, 0 };
        Vector3 image = reflect_point(source_pos, wall_c, wall_n);
        float side_src = Vector3DotProduct(Vector3Subtract(source_pos, wall_c), wall_n);
        float side_lis = Vector3DotProduct(Vector3Subtract(L, wall_c), wall_n);
        int same_side = (side_src > 0) == (side_lis > 0);

        float t; Vector3 hit;
        Vector3 refl_pt = source_pos; int refl_valid = 0;
        if (same_side && seg_plane(L, image, wall_c, wall_n, &t, &hit) && t > 0 && t < 1 &&
            in_panel(hit, wall_c, wall_u, wall_v, wall_hw, wall_hh)) { refl_valid = 1; refl_pt = hit; }

        int occluded = 0;
        if (!same_side && seg_plane(source_pos, L, wall_c, wall_n, &t, &hit) && t > 0 && t < 1 &&
            in_panel(hit, wall_c, wall_u, wall_v, wall_hw, wall_hh)) { occluded = 1; }

        bw_source_set_pos(e, src, source_pos.x, source_pos.y, source_pos.z);
        bw_source_set_gain(e, src, (occ_audible && occluded) ? SRC_GAIN * OCC_GAIN : SRC_GAIN);
        bw_source_set_pos(e, refl, image.x, image.y, image.z);
        bw_source_set_gain(e, refl, (refl_audible && refl_valid) ? SRC_GAIN * REFL_GAIN : 0.0f);
        bw_set_listener_pose(e, 0, 0, 0, q.x, q.y, q.z, q.w);
        bw_commit(e);

        Vector3 right = Vector3RotateByQuaternion((Vector3){ 1, 0, 0 }, q);
        Vector3 fwd   = Vector3RotateByQuaternion((Vector3){ 0, 0, -1 }, q);

        BeginDrawing();
        ClearBackground((Color){ 25, 25, 30, 255 });
        BeginMode3D(cam);
        DrawGrid(12, 0.5f);
        for (int k = 0; k < NSPK; ++k) DrawSphere(speakers[k], 0.10f, (Color){ 120, 120, 140, 255 });

        draw_wall(wall_c, wall_u, wall_v, wall_hw, wall_hh,
                  (Color){ 90, 110, 140, 90 }, (Color){ 150, 180, 220, 255 });

        /* direct path: green when clear, red when the wall occludes it */
        DrawLine3D(L, source_pos, occluded ? (Color){ 230, 70, 70, 255 } : (Color){ 90, 220, 90, 220 });
        DrawSphere(source_pos, 0.18f, RED);

        /* reflected path source -> bounce -> listener, and the mirror-image source */
        if (refl_valid) {
            DrawLine3D(source_pos, refl_pt, ORANGE);
            DrawLine3D(refl_pt, L, ORANGE);
            DrawSphere(refl_pt, 0.06f, ORANGE);
            DrawSphere(image, 0.14f, (Color){ 230, 160, 60, 130 });
            DrawLine3D(image, refl_pt, (Color){ 230, 160, 60, 80 });
        }
        /* the head, at the array centre, with a face that turns with the listener pose:
         * an orange nose marks "forward", colour-coded ears mark the L/R audio channels. */
        DrawSphere((Vector3){ 0, 0, 0 }, 0.16f, SKYBLUE);
        DrawSphere(Vector3Scale(right,  0.17f), 0.055f, RED);       /* right ear -> audio R */
        DrawSphere(Vector3Scale(right, -0.17f), 0.055f, RAYWHITE);  /* left ear  -> audio L */
        DrawCylinderEx(Vector3Scale(fwd, 0.13f), Vector3Scale(fwd, 0.30f),
                       0.06f, 0.0f, 10, ORANGE);                    /* nose -> facing */
        EndMode3D();

        DrawText("WASD/RF move source   Q/E head   [ ] move wall   T reflection   G occlusion   right-drag orbit   wheel zoom", 12, 12, 16, RAYWHITE);
        DrawText(TextFormat("source (%.2f, %.2f, %.2f)   head %.0f deg",
                            source_pos.x, source_pos.y, source_pos.z, head_yaw * 57.2958f), 12, 34, 16, LIGHTGRAY);
        DrawText(TextFormat("%s    [T] reflection %s   [G] occlusion %s",
                            refl_valid ? "REFLECTING — audible image source" :
                            (occluded ? "OCCLUDING — direct path blocked" : "wall: no interaction"),
                            refl_audible ? "ON" : "off", occ_audible ? "ON" : "off"), 12, 56, 16,
                 refl_valid ? ORANGE : (occluded ? (Color){ 230, 120, 120, 255 } : GRAY));
        DrawText("green = direct (red = occluded)   orange = reflected path + image source   "
                 "(image source has no delay/material filter yet — that's the Steam Audio path)", 12, 78, 13, GRAY);
        if (silent)
            DrawText("audio: NULL sink — NO SOUND (set BWAUDIO_ASIO_DRIVER; see console)", 12, 98, 16, RED);
        else
            DrawText(TextFormat("audio: %s", backend), 12, 98, 16, GREEN);
        EndDrawing();
    }

    CloseWindow();
    bw_stop(e);
    bw_destroy(e);
    remove(WAV);
    return 0;
}
