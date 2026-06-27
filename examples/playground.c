/*
 * playground.c — interactive raylib scene driving bwaudio's binaural monitor.
 *
 * Drag a sound source around a 26-speaker CAVE and turn your head; hear the result on
 * headphones in real time (the binaural profile, output through a 2-ch ASIO driver that
 * the engine auto-picks — ASIO4ALL / FlexASIO / the Steinberg built-in). This is the
 * by-ear evaluation the automated tests can't do. Uses only the public C ABI (bwaudio.h);
 * raylib's raymath provides the vector/quaternion math.
 *
 * Controls: WASD move the source (X/Z), R/F up/down, Q/E turn the head, ESC quit.
 * Build: cmake -S . -B build -DBWAUDIO_BUILD_PLAYGROUND=ON && cmake --build build
 */
#include "bwaudio.h"
#include "raylib.h"
#include "raymath.h"

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include <stdio.h>
#include <stdlib.h>

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
        printf("bw_start: %s — no audio (install ASIO4ALL for headphone output); the scene still runs.\n",
               err ? err : "?");
    }

    BwSound  snd = bw_load_sound(e, WAV);
    BwSource src = bw_source_create(e);
    bw_source_set_gain(e, src, 0.8f);
    bw_source_play(e, src, snd, true);

    Vector3 speakers[NSPK];
    default_speakers(speakers);
    Vector3 source_pos = { 1.5f, 0.0f, 0.0f };
    float   head_yaw = 0.0f;                                  /* radians, about +y */

    InitWindow(1000, 700, "bwaudio - binaural playground");
    SetTargetFPS(60);
    Camera3D cam = { .position = { 5, 5, 5 }, .target = { 0, 0.5f, 0 }, .up = { 0, 1, 0 },
                     .fovy = 55, .projection = CAMERA_PERSPECTIVE };

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

        Quaternion q = QuaternionFromAxisAngle((Vector3){ 0, 1, 0 }, head_yaw);

        bw_source_set_pos(e, src, source_pos.x, source_pos.y, source_pos.z);
        bw_set_listener_pose(e, 0, 0, 0, q.x, q.y, q.z, q.w);
        bw_commit(e);

        Vector3 right = Vector3RotateByQuaternion((Vector3){ 1, 0, 0 }, q);
        Vector3 fwd   = Vector3RotateByQuaternion((Vector3){ 0, 0, -1 }, q);

        BeginDrawing();
        ClearBackground((Color){ 25, 25, 30, 255 });
        BeginMode3D(cam);
        DrawGrid(12, 0.5f);
        for (int k = 0; k < NSPK; ++k) DrawSphere(speakers[k], 0.10f, (Color){ 120, 120, 140, 255 });
        DrawLine3D((Vector3){ 0, 0, 0 }, source_pos, (Color){ 200, 80, 80, 180 });
        DrawSphere(source_pos, 0.18f, RED);
        /* the head, at the array centre, with a face that turns with the listener pose:
         * an orange nose marks "forward", colour-coded ears mark the L/R audio channels. */
        DrawSphere((Vector3){ 0, 0, 0 }, 0.16f, SKYBLUE);
        DrawSphere(Vector3Scale(right,  0.17f), 0.055f, RED);       /* right ear -> audio R */
        DrawSphere(Vector3Scale(right, -0.17f), 0.055f, RAYWHITE);  /* left ear  -> audio L */
        DrawCylinderEx(Vector3Scale(fwd, 0.13f), Vector3Scale(fwd, 0.30f),
                       0.06f, 0.0f, 10, ORANGE);                    /* nose -> facing */
        EndMode3D();

        DrawText("WASD: move source   R/F: up/down   Q/E: turn head   ESC: quit", 12, 12, 18, RAYWHITE);
        DrawText(TextFormat("source = (%.2f, %.2f, %.2f)   head yaw = %.0f deg",
                            source_pos.x, source_pos.y, source_pos.z, head_yaw * 57.2958f), 12, 36, 18, LIGHTGRAY);
        DrawText("head: orange nose = facing   red ear = right (audio R)   white ear = left   (binaural via ASIO)", 12, 60, 16, GRAY);
        EndDrawing();
    }

    CloseWindow();
    bw_stop(e);
    bw_destroy(e);
    remove(WAV);
    return 0;
}
