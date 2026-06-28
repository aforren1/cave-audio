/*
 * playground.c — interactive raylib scene driving bwaudio's binaural monitor.
 *
 * Drag a sound source around a 26-speaker CAVE and turn your head; hear the result on
 * headphones in real time (the binaural profile, output through a 2-ch ASIO driver that
 * the engine auto-picks — ASIO4ALL / FlexASIO / the Steinberg built-in). This is the
 * by-ear evaluation the automated tests can't do. Uses only the public C ABI (bwaudio.h);
 * raylib's raymath provides the vector/quaternion math.
 *
 * A wall demonstrates two acoustic situations: move the source in FRONT of it to hear a specular
 * reflection (a mirror-image source — a second DBAP voice; no delay/material yet, materials.md), or
 * BEHIND it for OCCLUSION — REAL Steam Audio occlusion: the engine's off-thread sim ray-traces the
 * wall blocking the line to the listener and attenuates the source (the HUD shows the live factor).
 * Occlusion needs the Steam Audio build; without it bw_source_set_occlusion is a no-op (no blocking).
 *
 * Keys 1-4 switch the localization test signal (pink noise / pink bursts / click train / 1 kHz
 * tone) — broadband + sharp onsets localise best; the tone is there to feel the ambiguity.
 *
 * Controls: WASD/RF move source, Q/E turn head, [ ] slide wall, 1-4 signal, T reflection,
 *           G occlusion, right-drag orbit, wheel zoom, ESC quit.
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

/* ---- localization test signals (synthesised at startup; see the note on the HUD) ----
 * Choice of signal matters: broadband + sharp onsets localise best, and HF content is what lets
 * you hear elevation / front-back. 0 pink noise (general), 1 pink-noise bursts (crisp onsets),
 * 2 click train (transients, precedence), 3 a 1 kHz tone (deliberately ambiguous, to feel the limit). */
#define SIG_SECS 2u
#define SIGLEN   (SR * SIG_SECS)
#define NSIG     4
static const char* SIG_NAMES[NSIG] = { "pink noise", "pink bursts", "click train", "1 kHz tone (ambiguous)" };

static float white(unsigned int* s) {            /* white noise sample in ~[-1,1] from an LCG */
    *s = *s * 1664525u + 1013904223u;
    return (float)((int)(*s >> 9) - (1 << 22)) / (float)(1 << 22);
}
static float pink(float w, float b[7]) {         /* Paul Kellet pink filter */
    b[0] = 0.99886f * b[0] + w * 0.0555179f;  b[1] = 0.99332f * b[1] + w * 0.0750759f;
    b[2] = 0.96900f * b[2] + w * 0.1538520f;  b[3] = 0.86650f * b[3] + w * 0.3104856f;
    b[4] = 0.55000f * b[4] + w * 0.5329522f;  b[5] = -0.7616f * b[5] - w * 0.0168980f;
    float p = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362f;
    b[6] = w * 0.115926f;
    return p * 0.11f;
}
static void gen_signal(int which, float* buf, uint32_t n) {
    unsigned int s = 22222u;
    float b[7] = { 0 };
    const uint32_t period = SR / 5;              /* 200 ms; divides SIGLEN exactly -> seamless loop */
    if (which == 0) {                            /* continuous pink noise */
        for (uint32_t i = 0; i < n; ++i) buf[i] = pink(white(&s), b) * 0.55f;
    } else if (which == 1) {                     /* pink bursts: 100 ms on / 100 ms off, 5 ms fades */
        const uint32_t on = period / 2, ramp = SR / 200;
        for (uint32_t i = 0; i < n; ++i) {
            float p = pink(white(&s), b) * 0.55f, env = 0.0f;
            uint32_t ph = i % period;
            if (ph < ramp)              env = (float)ph / ramp;
            else if (ph < on - ramp)    env = 1.0f;
            else if (ph < on)           env = (float)(on - ph) / ramp;
            buf[i] = p * env;
        }
    } else if (which == 2) {                     /* click train: 5/s, ~3 ms decaying broadband ticks */
        const uint32_t clicklen = SR / 333;
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t ph = i % period;
            buf[i] = (ph < clicklen) ? white(&s) * expf(-6.0f * (float)ph / clicklen) * 0.6f : 0.0f;
        }
    } else {                                     /* 1 kHz sine — narrowband, ambiguous on purpose */
        for (uint32_t i = 0; i < n; ++i) buf[i] = sinf(2.0f * PI * 1000.0f * (float)i / SR) * 0.3f;
    }
}
static int write_wav(const char* path, const float* buf, uint32_t n) {
    drwav_data_format fmt = { drwav_container_riff, DR_WAVE_FORMAT_IEEE_FLOAT, 1, SR, 32 };
    drwav wav;
    if (!drwav_init_file_write(&wav, path, &fmt, NULL)) return 0;
    drwav_write_pcm_frames(&wav, n, buf);
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

/* register the wall quad as the occluding scene geometry (one concrete-ish material) */
static void push_wall_mesh(BwEngine* e, Vector3 c, Vector3 u, Vector3 v, float hw, float hh) {
    Vector3 a  = Vector3Add(Vector3Add(c, Vector3Scale(u, -hw)), Vector3Scale(v, -hh));
    Vector3 b  = Vector3Add(Vector3Add(c, Vector3Scale(u,  hw)), Vector3Scale(v, -hh));
    Vector3 d  = Vector3Add(Vector3Add(c, Vector3Scale(u,  hw)), Vector3Scale(v,  hh));
    Vector3 ee = Vector3Add(Vector3Add(c, Vector3Scale(u, -hw)), Vector3Scale(v,  hh));
    float verts[12] = { a.x, a.y, a.z,  b.x, b.y, b.z,  d.x, d.y, d.z,  ee.x, ee.y, ee.z };
    int   tris[6]   = { 0, 1, 2,  0, 2, 3 };
    float absorption[3]   = { 0.1f, 0.1f, 0.1f };
    float transmission[3] = { 0.05f, 0.05f, 0.05f };          /* concrete-ish: blocks most */
    bw_scene_set_mesh(e, verts, 4, tris, 2, absorption, 0.5f, transmission);
}

int main(void) {
    _putenv("BWAUDIO_SINK=asio");                             /* headphone output via a 2-ch ASIO driver */

    /* synthesise the localization test signals to wav (the engine loads sounds from file) */
    const char* sig_files[NSIG] = { "pg_pink.wav", "pg_bursts.wav", "pg_clicks.wav", "pg_tone.wav" };
    float* sigbuf = (float*)malloc((size_t)SIGLEN * sizeof(float));
    if (!sigbuf) { printf("out of memory\n"); return 1; }
    for (int i = 0; i < NSIG; ++i) { gen_signal(i, sigbuf, SIGLEN); write_wav(sig_files[i], sigbuf, SIGLEN); }
    free(sigbuf);

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

    BwSound sounds[NSIG];
    for (int i = 0; i < NSIG; ++i) sounds[i] = bw_load_sound(e, sig_files[i]);
    int cur_sig = 0;

    BwSource src = bw_source_create(e);
    bw_source_set_gain(e, src, SRC_GAIN);
    bw_source_play(e, src, sounds[cur_sig], true);

    /* a second voice rendered at the source's mirror image across the wall — an audible
     * single specular reflection, using only the existing per-source DBAP path (no delay or
     * material filtering yet; that's the future Steam Audio reflection path, docs/materials.md) */
    BwSource refl = bw_source_create(e);
    bw_source_play(e, refl, sounds[cur_sig], true);
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

    /* register the wall as REAL occluding geometry and enable occlusion on the direct source
     * (the engine's Steam Audio sim attenuates it when the wall blocks the line to the listener) */
    push_wall_mesh(e, wall_c, wall_u, wall_v, wall_hw, wall_hh);
    bw_source_set_occlusion(e, src, true);

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
        if (IsKeyDown(KEY_LEFT_BRACKET) || IsKeyDown(KEY_RIGHT_BRACKET)) {
            wall_c = Vector3Add(wall_c, Vector3Scale(wall_n, IsKeyDown(KEY_LEFT_BRACKET) ? -mv : mv));
            push_wall_mesh(e, wall_c, wall_u, wall_v, wall_hw, wall_hh);   /* update the occluder (sim throttles) */
        }
        if (IsKeyPressed(KEY_T)) refl_audible = !refl_audible;
        if (IsKeyPressed(KEY_G)) { occ_audible = !occ_audible; bw_source_set_occlusion(e, src, occ_audible); }
        for (int i = 0; i < NSIG; ++i)                       /* 1-4: switch the test signal */
            if (IsKeyPressed(KEY_ONE + i)) {
                cur_sig = i;
                bw_source_play(e, src,  sounds[i], true);
                bw_source_play(e, refl, sounds[i], true);
            }

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

        bw_source_set_pos(e, src, source_pos.x, source_pos.y, source_pos.z);
        bw_source_set_gain(e, src, SRC_GAIN);                 /* occlusion is now applied by the engine */
        bw_source_set_pos(e, refl, image.x, image.y, image.z);
        bw_source_set_gain(e, refl, (refl_audible && refl_valid) ? SRC_GAIN * REFL_GAIN : 0.0f);
        bw_set_listener_pose(e, 0, 0, 0, q.x, q.y, q.z, q.w);
        bw_commit(e);

        float occ = bw_source_get_occlusion(e, src);          /* real Steam Audio occlusion (1 = clear) */
        int occluded = occ < 0.85f;

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

        /* readable HUD: a dark backing panel + bright text (ASCII only — raylib's default font
         * has no em-dash/box glyphs) */
        DrawRectangle(0, 0, GetScreenWidth(), 124, (Color){ 0, 0, 0, 185 });
        DrawText("WASD/RF move source   Q/E turn head   [ ] move wall   1-4 signal   T reflection   G occlusion   right-drag orbit   wheel zoom",
                 12, 10, 15, RAYWHITE);
        DrawText(TextFormat("source (%.2f, %.2f, %.2f)   head %.0f deg",
                            source_pos.x, source_pos.y, source_pos.z, head_yaw * 57.2958f),
                 12, 32, 16, (Color){ 215, 215, 225, 255 });
        DrawText(TextFormat("signal [1-4]: %s", SIG_NAMES[cur_sig]), 12, 54, 18, (Color){ 110, 200, 255, 255 });
        DrawText(TextFormat("%s    [T] reflection %s   [G] occlusion %s (%.0f%% audible)",
                            refl_valid ? "REFLECTING - audible image source" :
                            (occluded ? "OCCLUDED - Steam Audio attenuates the source" : "wall: clear line of sight"),
                            refl_audible ? "ON" : "off", occ_audible ? "ON" : "off", occ * 100.0f),
                 12, 78, 16, refl_valid ? ORANGE : (occluded ? (Color){ 245, 140, 140, 255 } : (Color){ 200, 200, 210, 255 }));
        if (silent)
            DrawText("audio: NULL sink - NO SOUND (set BWAUDIO_ASIO_DRIVER; see console)", 12, 100, 16, (Color){ 255, 110, 110, 255 });
        else
            DrawText(TextFormat("audio: %s", backend), 12, 100, 16, (Color){ 110, 235, 130, 255 });
        EndDrawing();
    }

    CloseWindow();
    bw_stop(e);
    bw_destroy(e);
    for (int i = 0; i < NSIG; ++i) remove(sig_files[i]);
    return 0;
}
