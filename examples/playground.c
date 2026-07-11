/*
 * playground.c — interactive raylib harness for bwaudio's binaural monitor, split into SCENES,
 * each auditioning one feature by ear on headphones (the binaural profile, output through a 2-ch
 * ASIO driver the engine auto-picks — ASIO4ALL / FlexASIO / the Steinberg built-in). This is the
 * by-ear evaluation the automated tests can't do. Public C ABI only (bwaudio.h); raylib's raymath
 * provides the vector/quaternion math.
 *
 * Scenes (cycle with TAB):
 *   1 Localization      — pure listener-relative DBAP. Move a source, turn your head, switch the
 *                         test signal (1-4); hear it localise around the 26-speaker array. SPACE
 *                         auto-moves it: orbit + near/far + high/low, sweeping the whole space.
 *                         Opt-in per-source effects: V Doppler, B air absorption, C source size, M dual-band
 *                         (amplitude LF / power HF panning), X a fast straight flyby (X+V = race-car pitch sweep).
 *   2 Occlusion+Materials — a real Steam Audio occluder. Push the source BEHIND the wall and the
 *                         off-thread sim attenuates + spectrally tilts it by the wall MATERIAL (M to
 *                         cycle concrete/glass/carpet/wood/metal); in FRONT, a mirror-image source
 *                         is an audible specular reflection scaled by that material's reflectivity.
 *   3 Directivity       — a weighted-dipole radiation pattern (Z omni/cardioid/figure-8), aim it
 *                         with , / . ; the listener hears it attenuate off-axis (HUD shows the lobe).
 *   4 Channel walk      — bw_test_signal drives ONE raw output channel (speaker-check tool). Step
 *                         channels with LEFT/RIGHT (SPACE auto-walks); in binaural each channel is
 *                         HRTF'd as its virtual speaker, so the tone circles your head as you walk.
 *   5 Blind A/B/X       — double-blind self test over live engine knobs (dual-band, DBAP vs SPCAP /
 *                         VBAP, spread, air absorption): X is secretly A or B, listen with Z/X/C,
 *                         answer LEFT/RIGHT, and the running binomial p-value says whether the
 *                         difference is genuinely audible (p < 0.05) or you're guessing.
 *   6 Reverb bed        — a static shoebox room + the Steam Audio hybrid reverb bed. Move the source
 *                         and the room reverb follows; G dry/wet A-B, [ ] wet level, V distance->wet
 *                         (near dry / far wet), B A/Bs the bed
 *                         DECODER (sampling vs AllRAD — load-time, so it rebuilds the engine; differs
 *                         most on an irregular layout). The bed + room geometry are LOAD-time (the room
 *                         locks once the bed runs), so entering/leaving this scene REBUILDS the engine
 *                         (a brief audio gap). Transient signals (clicks/bursts) show the tail best.
 *
 * Global keys: WASD/RF move source, Q/E turn head, 1-4 signal, TAB scene, right-drag/wheel camera, ESC.
 * Needs the Steam Audio build for occlusion/materials/directivity/reverb; without it those are no-ops.
 * Usage: bw_playground [cave_layout.json] — audition with your surveyed layout (renders + pans with the
 *        engine's actual speaker positions); with no arg it auto-loads ./cave_layout.json or the default grid.
 * Build: cmake -S . -B build -DBWAUDIO_BUILD_PLAYGROUND=ON && cmake --build build
 */
#include "bwaudio.h"
#include "raylib.h"
#include "raymath.h"
#include "ui_text.h"        /* crisp HUD text; ui_text() supersedes raylib's DrawText() */
#include "speaker_gizmo.h"  /* the "real speaker" glyph (cabinet + cone aimed at the listener) */
#include "constraints_view.h"  /* constraints.json boxes, drawn for orientation (same view as the layout tool) */
#include "axes_hud.h"       /* screen-corner XYZ triad, shared with the layout tool */

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SR   48000u
#define NSPK 26

#define SRC_GAIN  0.8f
#define REFL_GAIN 0.5f    /* base image-source level (scaled further by the wall material reflectivity) */
#define TEST_GAIN 0.3f    /* channel-walk test signal level */

/* ---- localization test signals (synthesised at startup) ----
 * Choice of signal matters: broadband + sharp onsets localise best, and HF content is what lets you
 * hear elevation / front-back. 0 pink noise, 1 pink bursts, 2 click train, 3 a 1 kHz tone (ambiguous). */
#define SIG_SECS 2u
#define SIGLEN   (SR * SIG_SECS)
#define NSIG     4
static const char* SIG_NAMES[NSIG] = { "pink noise", "pink bursts", "click train", "1 kHz tone (ambiguous)" };
static const char* sig_files[NSIG] = { "pg_pink.wav", "pg_bursts.wav", "pg_clicks.wav", "pg_tone.wav" };

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

/* ---- wall geometry helpers (occlusion + reflection scene) ---- */
static Vector3 reflect_point(Vector3 p, Vector3 c, Vector3 n) {      /* mirror p across plane (c,n) */
    float d = Vector3DotProduct(Vector3Subtract(p, c), n);
    return Vector3Subtract(p, Vector3Scale(n, 2.0f * d));
}
static int seg_plane(Vector3 a, Vector3 b, Vector3 c, Vector3 n, float* t, Vector3* hit) {
    Vector3 ab = Vector3Subtract(b, a);
    float denom = Vector3DotProduct(ab, n);
    if (fabsf(denom) < 1e-6f) return 0;
    float tt = Vector3DotProduct(Vector3Subtract(c, a), n) / denom;
    *t = tt; *hit = Vector3Add(a, Vector3Scale(ab, tt));
    return 1;
}
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

/* ======================= shared playground state (file scope for the scene callbacks) ======================= */
static BwEngine*   e;
static BwSource    src, refl;
static BwSound     sounds[NSIG];
static const char* backend_name;
static int         backend_silent;
static const char* g_layout_path;          /* optional cave_layout.json; NULL = engine default grid */

static Vector3 speakers[NSPK];
static Vector3 g_head;            /* the ear point = array centroid (the engine's nominal listening point);
                                   * room origin is on the FLOOR, so the head is NOT at the origin */
static Vector3 source_pos = { 1.5f, 0.0f, 0.0f };   /* y re-based to ear height once the layout is known */
static float   head_yaw, source_yaw;
static int     cur_sig;
static int     highlight_spk = -1;          /* a scene may highlight one speaker each frame; reset per frame */

/* wall + materials (occlusion scene) */
static Vector3       wall_c = { 0.0f, 1.2f, -2.2f };   /* standing on the floor: spans y 0..2.4, blocks the ear line */
static const Vector3 wall_n = { 0, 0, 1 };
static const float   wall_hw = 2.0f, wall_hh = 1.2f;
static Vector3       wall_u, wall_v;
static const char*   mat_names[] = { "concrete", "glass", "carpet", "wood", "metal" };
/* broadband reflectivity (~1 - mean absorption of the same presets): scales the image-source level so
 * the wall MATERIAL drives reflectivity too — carpet reflects far less than metal/concrete. Broadband
 * only (one DBAP voice has no per-source EQ); the full per-band material reflection is the reverb bed's job. */
static const float   mat_refl[] = { 0.93f, 0.96f, 0.45f, 0.92f, 0.89f };
enum { NMAT = 5 };
static BwMaterial    mats[NMAT];
static int           cur_mat, refl_audible = 1, occ_audible = 1;
static Vector3       occ_image, occ_refl_pt;     /* computed in occ_update, drawn in occ_draw3d */
static int           occ_refl_valid, occ_occluded;
static float         occ_factor = 1.0f;

/* directivity scene */
static const char* dir_names[] = { "omni", "cardioid", "figure-8" };
static int         cur_dir = 1;                  /* default cardioid so the effect is audible on entry */
static float       dir_gain = 1.0f;

/* channel-walk scene */
static int   chan_active, chan_kind = BW_TEST_SINE, chan_auto;
static float chan_timer;

/* camera (shared) */
static Camera3D cam;
static float    cam_yaw = 195.0f * DEG2RAD, cam_pitch = 25.0f * DEG2RAD, cam_dist = 8.4f;

/* register the wall quad as occluding geometry with a material token. Safe at runtime because the
 * reverb bed isn't enabled (occlusion geometry is dynamic; it locks only while the bed runs). */
static void push_wall_mesh(BwMaterial mat) {
    Vector3 a  = Vector3Add(Vector3Add(wall_c, Vector3Scale(wall_u, -wall_hw)), Vector3Scale(wall_v, -wall_hh));
    Vector3 b  = Vector3Add(Vector3Add(wall_c, Vector3Scale(wall_u,  wall_hw)), Vector3Scale(wall_v, -wall_hh));
    Vector3 d  = Vector3Add(Vector3Add(wall_c, Vector3Scale(wall_u,  wall_hw)), Vector3Scale(wall_v,  wall_hh));
    Vector3 ee = Vector3Add(Vector3Add(wall_c, Vector3Scale(wall_u, -wall_hw)), Vector3Scale(wall_v,  wall_hh));
    float verts[12] = { a.x, a.y, a.z,  b.x, b.y, b.z,  d.x, d.y, d.z,  ee.x, ee.y, ee.z };
    int   tris[6]   = { 0, 1, 2,  0, 2, 3 };
    BwMaterial tri_mat[2] = { mat, mat };
    bw_scene_set_mesh_mat(e, verts, 4, tris, 2, tri_mat);
}

/* ---- shared drawing ---- */
static CvConstraints g_con;     /* ./constraints.json, if present — drawn in every scene for orientation */

static void draw_speakers(int hi) {
    cv_draw(&g_con);            /* the room's bounds/no-go/obstacle boxes, same colors as the layout tool */
    for (int k = 0; k < NSPK; ++k)
        draw_speaker_gizmo(speakers[k], g_head,                 /* cones aim at the head (the array centroid) */
                           k == hi ? 0.30f : 0.22f,
                           k == hi ? (Color){ 120, 240, 140, 255 } : (Color){ 120, 120, 140, 255 });
}
static void draw_head(Quaternion q) {
    /* ear/nose axes from the ABI's room-frame identity basis (bwaudio.h BW_ROOM_*) */
    Vector3 right = Vector3RotateByQuaternion((Vector3){ BW_ROOM_RIGHT[0], BW_ROOM_RIGHT[1], BW_ROOM_RIGHT[2] }, q);
    Vector3 fwd   = Vector3RotateByQuaternion((Vector3){ BW_ROOM_AHEAD[0], BW_ROOM_AHEAD[1], BW_ROOM_AHEAD[2] }, q);
    DrawSphere(g_head, 0.16f, SKYBLUE);
    DrawSphere(Vector3Add(g_head, Vector3Scale(right,  0.17f)), 0.055f, RED);       /* right ear -> audio R */
    DrawSphere(Vector3Add(g_head, Vector3Scale(right, -0.17f)), 0.055f, RAYWHITE);  /* left ear  -> audio L */
    DrawCylinderEx(Vector3Add(g_head, Vector3Scale(fwd, 0.13f)),
                   Vector3Add(g_head, Vector3Scale(fwd, 0.30f)), 0.06f, 0.0f, 10, ORANGE); /* nose */
}

static void build_engine(int with_reverb);   /* fwd: the reverb scene rebuilds to A/B the bed decoder */

/* ============================= Scene 1: Localization (pure DBAP) ============================= */
/* auto-move (SPACE): a hands-free demo that circles the listener while breathing near<->far and
 * bobbing high<->low on three incommensurate periods, so the source sweeps the whole space over time. */
static int   loc_auto, loc_flyby, loc_dop, loc_air, loc_dual;   /* loc_dop/loc_air/loc_spread/loc_dual persist */
static float loc_t, loc_fly_t, loc_spread;
enum { LOC_TRAIL = 96 };
static Vector3 loc_trail[LOC_TRAIL];
static int     loc_trail_len;

static void loc_enter(void)  {
    bw_source_set_gain(e, src, SRC_GAIN); loc_auto = 0; loc_flyby = 0; loc_trail_len = 0;
    bw_source_set_doppler(e, src, loc_dop);            /* re-apply (switch_scene cleared them) */
    bw_source_set_air_absorption(e, src, loc_air);
    bw_source_set_spread(e, src, loc_spread);
    bw_set_dual_band(e, loc_dual);
}
static void loc_update(float dt) {
    if (IsKeyPressed(KEY_SPACE)) { loc_auto = !loc_auto; if (loc_auto) { loc_flyby = 0; loc_t = 0.0f; loc_trail_len = 0; } }
    if (IsKeyPressed(KEY_X))     { loc_flyby = !loc_flyby; if (loc_flyby) { loc_auto = 0; loc_fly_t = 0.0f; loc_trail_len = 0; } }
    if (IsKeyPressed(KEY_V)) { loc_dop = !loc_dop; bw_source_set_doppler(e, src, loc_dop); }
    if (IsKeyPressed(KEY_B)) { loc_air = !loc_air; bw_source_set_air_absorption(e, src, loc_air); }
    if (IsKeyPressed(KEY_C)) {                            /* cycle source size: point -> .4 -> .7 -> wide -> point */
        loc_spread = (loc_spread < 0.05f) ? 0.4f : (loc_spread < 0.5f) ? 0.7f : (loc_spread < 0.85f) ? 1.0f : 0.0f;
        bw_source_set_spread(e, src, loc_spread);
    }
    if (IsKeyPressed(KEY_M)) { loc_dual = !loc_dual; bw_set_dual_band(e, loc_dual); }   /* dual-band A/B */
    if (loc_flyby) {                                      /* fast straight pass 0.8 m in front: the Doppler demo */
        loc_fly_t += dt;
        float period = 3.6f, u = fmodf(loc_fly_t, period) / period;     /* there-and-back, ~7.8 m/s */
        float x = (u < 0.5f) ? (-7.0f + 28.0f * u) : (7.0f - 28.0f * (u - 0.5f));
        source_pos = (Vector3){ x, g_head.y, 0.8f };
    } else if (loc_auto) {
        loc_t += dt;
        float az = 0.62f * loc_t;                         /* circle the listener   (~10 s / orbit) */
        float r  = 2.0f + 1.2f * sinf(0.90f * loc_t);     /* near <-> far  0.8..3.2 (~7 s) */
        float y  = 1.2f  * sinf(1.14f * loc_t);           /* low  <-> high +/-1.2 about ear height (~5.5 s) */
        source_pos = (Vector3){ r * cosf(az), g_head.y + y, r * sinf(az) };
    }
    if (loc_auto || loc_flyby) {
        if (loc_trail_len < LOC_TRAIL) loc_trail[loc_trail_len++] = source_pos;
        else { memmove(loc_trail, loc_trail + 1, (LOC_TRAIL - 1) * sizeof(Vector3)); loc_trail[LOC_TRAIL - 1] = source_pos; }
    }
    bw_source_set_pos(e, src, source_pos.x, source_pos.y, source_pos.z);
    bw_source_set_gain(e, src, SRC_GAIN);
}
static void loc_draw3d(void) {
    if (loc_auto) {
        for (int i = 1; i < loc_trail_len; ++i)           /* fading trail of the swept path */
            DrawLine3D(loc_trail[i - 1], loc_trail[i], (Color){ 90, 220, 90, (unsigned char)(40 + 180 * i / loc_trail_len) });
        DrawLine3D(source_pos, (Vector3){ source_pos.x, 0, source_pos.z }, (Color){ 90, 220, 90, 60 }); /* height drop line */
    }
    DrawLine3D(g_head, source_pos, (Color){ 90, 220, 90, 220 });
    DrawSphere(source_pos, 0.18f, RED);
}
static void loc_hud(int y) {
    ui_text(TextFormat("[SPACE] auto-move %s   signal [1-4]: %s   (broadband + sharp onsets localise best)",
                        loc_auto ? "ON - orbit + near/far + high/low" : "off", SIG_NAMES[cur_sig]),
             12, y, 15, (Color){ 110, 200, 255, 255 });
    ui_text(TextFormat("source (%.2f, %.2f, %.2f)   head %.0f deg%s",
                        source_pos.x, source_pos.y, source_pos.z, head_yaw * 57.2958f,
                        (loc_auto || loc_flyby) ? "   (WASD paused while auto-move runs)" : ""),
             12, y + 22, 15, (Color){ 200, 200, 210, 255 });
    ui_text(TextFormat("propagation:  [V] Doppler %s   [B] air absorption %s   [X] fast flyby %s   [C] size %.0f%%   [M] dual-band %s",
                        loc_dop ? "ON" : "off", loc_air ? "ON" : "off", loc_flyby ? "ON" : "off", loc_spread * 100.0f, loc_dual ? "ON (LF amp)" : "off"),
             12, y + 44, 15, (Color){ 150, 225, 150, 255 });
}

/* ====================== Scene 2: Occlusion & Materials (real Steam Audio) ====================== */
static void occ_enter(void) {
    bw_source_set_gain(e, src, SRC_GAIN);
    bw_source_set_occlusion(e, src, occ_audible);
    push_wall_mesh(mats[cur_mat]);
}
static void occ_update(float dt) {
    float mv = 2.5f * dt;
    if (IsKeyDown(KEY_LEFT_BRACKET) || IsKeyDown(KEY_RIGHT_BRACKET)) {
        wall_c = Vector3Add(wall_c, Vector3Scale(wall_n, IsKeyDown(KEY_LEFT_BRACKET) ? -mv : mv));
        push_wall_mesh(mats[cur_mat]);
    }
    if (IsKeyPressed(KEY_M)) { cur_mat = (cur_mat + 1) % NMAT; push_wall_mesh(mats[cur_mat]); }
    if (IsKeyPressed(KEY_T)) refl_audible = !refl_audible;
    if (IsKeyPressed(KEY_G)) { occ_audible = !occ_audible; bw_source_set_occlusion(e, src, occ_audible); }

    /* reflection: the source mirrored across the wall is valid when source + listener are on the same
     * side and the bounce lands on the panel. occlusion: the direct path crosses the panel. */
    const Vector3 L = g_head;
    occ_image = reflect_point(source_pos, wall_c, wall_n);
    int same_side = (Vector3DotProduct(Vector3Subtract(source_pos, wall_c), wall_n) > 0)
                 == (Vector3DotProduct(Vector3Subtract(L, wall_c), wall_n) > 0);
    float t; Vector3 hit; occ_refl_valid = 0; occ_refl_pt = source_pos;
    if (same_side && seg_plane(L, occ_image, wall_c, wall_n, &t, &hit) && t > 0 && t < 1 &&
        in_panel(hit, wall_c, wall_u, wall_v, wall_hw, wall_hh)) { occ_refl_valid = 1; occ_refl_pt = hit; }

    bw_source_set_pos(e, src, source_pos.x, source_pos.y, source_pos.z);
    bw_source_set_gain(e, src, SRC_GAIN);                       /* occlusion is applied by the engine */
    bw_source_set_pos(e, refl, occ_image.x, occ_image.y, occ_image.z);
    bw_source_set_gain(e, refl, (refl_audible && occ_refl_valid) ? SRC_GAIN * REFL_GAIN * mat_refl[cur_mat] : 0.0f);

    occ_factor   = bw_source_get_occlusion(e, src);             /* real Steam Audio occlusion (1 = clear) */
    occ_occluded = occ_factor < 0.85f;
}
static void occ_draw3d(void) {
    draw_wall(wall_c, wall_u, wall_v, wall_hw, wall_hh,
              (Color){ 90, 110, 140, 90 }, (Color){ 150, 180, 220, 255 });
    DrawLine3D(g_head, source_pos, occ_occluded ? (Color){ 230, 70, 70, 255 } : (Color){ 90, 220, 90, 220 });
    DrawSphere(source_pos, 0.18f, RED);
    if (occ_refl_valid) {
        DrawLine3D(source_pos, occ_refl_pt, ORANGE);
        DrawLine3D(occ_refl_pt, g_head, ORANGE);
        DrawSphere(occ_refl_pt, 0.06f, ORANGE);
        DrawSphere(occ_image, 0.14f, (Color){ 230, 160, 60, 130 });
        DrawLine3D(occ_image, occ_refl_pt, (Color){ 230, 160, 60, 80 });
    }
}
static void occ_hud(int y) {
    ui_text(TextFormat("[ ] slide wall   M material: %s (refl %.0f%%)   T reflection %s   G occlusion %s",
                        mat_names[cur_mat], mat_refl[cur_mat] * 100.0f,
                        refl_audible ? "ON" : "off", occ_audible ? "ON" : "off"),
             12, y, 15, (Color){ 110, 200, 255, 255 });
    ui_text(TextFormat("%s   (occlusion %.0f%% audible)",
                        occ_refl_valid ? "in FRONT: REFLECTING - audible image source" :
                        (occ_occluded ? "BEHIND: OCCLUDED - Steam Audio attenuates + tilts by material"
                                      : "wall: clear line of sight"),
                        occ_factor * 100.0f),
             12, y + 22, 15, occ_refl_valid ? ORANGE
                           : (occ_occluded ? (Color){ 245, 140, 140, 255 } : (Color){ 200, 200, 210, 255 }));
}

/* ============================= Scene 3: Directivity (weighted dipole) ============================= */
static void dir_enter(void) {
    bw_source_set_gain(e, src, SRC_GAIN);
    bw_source_set_directivity_preset(e, src, (BwDirectivity)cur_dir);
}
static void dir_update(float dt) {
    float rt = 1.8f * dt;
    if (IsKeyPressed(KEY_Z)) { cur_dir = (cur_dir + 1) % 3; bw_source_set_directivity_preset(e, src, (BwDirectivity)cur_dir); }
    if (IsKeyDown(KEY_COMMA))  source_yaw += rt;
    if (IsKeyDown(KEY_PERIOD)) source_yaw -= rt;
    Quaternion sq = QuaternionFromAxisAngle((Vector3){ 0, 1, 0 }, source_yaw);
    bw_source_set_pos(e, src, source_pos.x, source_pos.y, source_pos.z);
    bw_source_set_gain(e, src, SRC_GAIN);
    bw_source_set_orientation(e, src, sq.x, sq.y, sq.z, sq.w);
    dir_gain = bw_source_get_directivity(e, src);              /* 1 = on-axis/omni .. 0 = null */
}
static void dir_draw3d(void) {
    DrawLine3D(g_head, source_pos, (Color){ 90, 220, 90, 180 });
    DrawSphere(source_pos, 0.18f, RED);
    /* illustrative horizontal lobe (weighted dipole) pointing the source's aim */
    float w = (cur_dir == 1) ? 0.5f : (cur_dir == 2) ? 1.0f : 0.0f;
    Vector3 prev = { 0 };
    for (int i = 0; i <= 48; ++i) {
        float a = (float)i / 48.0f * 2.0f * PI;                /* angle off the source forward */
        float g = fabsf((1.0f - w) + w * cosf(a));
        float r = 0.15f + 0.7f * g, wa = source_yaw + a;
        /* source forward = yaw-rotated +z (room convention) */
        Vector3 p = { source_pos.x + r * sinf(wa), source_pos.y, source_pos.z + r * cosf(wa) };
        if (i > 0) DrawLine3D(prev, p, (Color){ 255, 180, 80, 200 });
        prev = p;
    }
}
static void dir_hud(int y) {
    ui_text(TextFormat("directivity [Z]: %s   aim with , / .   (%.0f%% on-axis at the listener)",
                        dir_names[cur_dir], dir_gain * 100.0f), 12, y, 15, (Color){ 110, 200, 255, 255 });
    ui_text("move/aim the source so its forward points AWAY from the head and hear it drop",
             12, y + 22, 15, (Color){ 200, 200, 210, 255 });
}

/* ============================= Scene 4: Channel walk (bw_test_signal) ============================= */
static void chan_set(int ch) { bw_test_signal(e, (uint32_t)ch, (BwTestKind)chan_kind, TEST_GAIN); }
static void chan_enter(void) {
    bw_source_set_gain(e, src,  0.0f);          /* silence the spatial voices; only the test tone sounds */
    bw_source_set_gain(e, refl, 0.0f);
    chan_timer = 0.0f;
    chan_auto  = 0;                             /* start manual each visit (don't resume a prior auto-walk) */
    chan_set(chan_active);
}
static void chan_update(float dt) {
    int prev = chan_active;
    if (IsKeyPressed(KEY_RIGHT)) chan_active = (chan_active + 1) % NSPK;
    if (IsKeyPressed(KEY_LEFT))  chan_active = (chan_active + NSPK - 1) % NSPK;
    if (IsKeyPressed(KEY_N))     { chan_kind = (chan_kind == BW_TEST_SINE) ? BW_TEST_NOISE : BW_TEST_SINE; chan_set(chan_active); }
    if (IsKeyPressed(KEY_SPACE)) chan_auto = !chan_auto;
    if (chan_auto && (chan_timer += dt) >= 0.7f) { chan_timer = 0.0f; chan_active = (chan_active + 1) % NSPK; }
    if (chan_active != prev) { bw_test_signal(e, (uint32_t)prev, BW_TEST_OFF, 0.0f); chan_set(chan_active); }
    highlight_spk = chan_active;
}
static void chan_draw3d(void) {
    DrawLine3D(g_head, speakers[chan_active], (Color){ 120, 235, 150, 200 });
}
static void chan_hud(int y) {
    ui_text(TextFormat("LEFT/RIGHT step channel   N %s   SPACE auto-walk %s",
                        chan_kind == BW_TEST_SINE ? "sine" : "noise", chan_auto ? "ON" : "off"),
             12, y, 15, (Color){ 110, 200, 255, 255 });
    Vector3 p = speakers[chan_active];
    ui_text(TextFormat("channel %d / %d   %s   speaker (%.2f, %.2f, %.2f)   (binaural: heard from that direction)",
                        chan_active, NSPK, chan_kind == BW_TEST_SINE ? "660 Hz sine" : "white noise", p.x, p.y, p.z),
             12, y + 22, 15, (Color){ 200, 200, 210, 255 });
}

/* ============================= Scene 5: Blind A/B/X ============================= */
/* Double-blind self test: A and B are two settings of ONE engine knob, X is randomly one of them.
 * Listen to all three freely (every switch is live + click-free: ramped gains / atomic toggles),
 * answer with LEFT ("X is A") or RIGHT ("X is B"), and the one-sided binomial tail over your trials
 * says whether you can ACTUALLY hear the difference (p < 0.05) or you're guessing — it turns
 * "sounds different to me" into a measurement. These are exactly the by-ear judgments the automated
 * tests can't make: dual-band panning, panner choice, source spread, air absorption. */
typedef struct { const char* name; const char* a; const char* b; void (*apply)(int v); } AbxCmp;
static void abx_ap_dual (int v) { bw_set_dual_band(e, v); }
static void abx_ap_spcap(int v) { bw_set_panner(e, v ? BW_PAN_SPCAP : BW_PAN_DBAP); }
static void abx_ap_vbap (int v) { bw_set_panner(e, v ? BW_PAN_VBAP  : BW_PAN_DBAP); }
static void abx_ap_sprd (int v) { bw_source_set_spread(e, src, v ? 0.6f : 0.0f); }
static void abx_ap_air  (int v) { bw_source_set_air_absorption(e, src, v); }
static const AbxCmp abx_cmps[] = {
    { "dual-band panning",     "single-band (power)", "dual-band (LF amplitude)", abx_ap_dual  },
    { "panner: DBAP vs SPCAP", "DBAP",                "SPCAP",                    abx_ap_spcap },
    { "panner: DBAP vs VBAP",  "DBAP",                "VBAP",                     abx_ap_vbap  },
    { "source spread",         "point source",        "spread 60%",               abx_ap_sprd  },
    { "air absorption",        "off",                 "on (distance LPF)",        abx_ap_air   },
};
enum { NABX = sizeof abx_cmps / sizeof abx_cmps[0] };
static int   abx_cmp, abx_listen = 2, abx_x;     /* listening to 0=A 1=B 2=X; abx_x = X's hidden identity */
static int   abx_trials, abx_correct, abx_last_x;
static float abx_flash_t; static int abx_flash_ok;
static int   abx_orbit; static float abx_orbit_t;

static double abx_pvalue(int n, int k) {         /* one-sided binomial tail P(correct >= k | n, 1/2) */
    double p = 0.0;
    for (int i = k; i <= n; ++i)
        p += exp(lgamma(n + 1.0) - lgamma(i + 1.0) - lgamma(n - i + 1.0) - n * log(2.0));
    return p > 1.0 ? 1.0 : p;
}
/* every knob back to baseline, then the tested knob to whichever variant we're listening to — so
 * switching COMPARISONS can't leave the previous knob stuck on its B setting. */
static void abx_apply_listen(void) {
    bw_set_dual_band(e, false);
    bw_set_panner(e, BW_PAN_DBAP);
    bw_source_set_spread(e, src, 0.0f);
    bw_source_set_air_absorption(e, src, false);
    abx_cmps[abx_cmp].apply(abx_listen == 2 ? abx_x : abx_listen);
}
static void abx_new_trial(void) { abx_x = GetRandomValue(0, 1); abx_listen = 2; abx_apply_listen(); }
static void abx_reset(void)     { abx_trials = abx_correct = 0; abx_flash_t = 0.0f; abx_new_trial(); }
static void abx_enter(void) {
    bw_source_set_gain(e, src, SRC_GAIN);
    abx_orbit = 1; abx_orbit_t = 0.0f;           /* default: slow orbit — motion exposes panner differences */
    abx_new_trial();                             /* keep the tally across visits; only X is redrawn */
}
static void abx_update(float dt) {
    if (IsKeyPressed(KEY_Z)) { abx_listen = 0; abx_apply_listen(); }
    if (IsKeyPressed(KEY_X)) { abx_listen = 1; abx_apply_listen(); }
    if (IsKeyPressed(KEY_C)) { abx_listen = 2; abx_apply_listen(); }
    if (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_RIGHT)) {
        int guess = IsKeyPressed(KEY_RIGHT) ? 1 : 0;
        abx_last_x = abx_x;
        abx_flash_ok = (guess == abx_x); abx_correct += abx_flash_ok; ++abx_trials; abx_flash_t = 1.6f;
        abx_new_trial();                         /* reveal + immediately deal the next X */
    }
    if (IsKeyPressed(KEY_G)) { abx_cmp = (abx_cmp + 1) % NABX; abx_reset(); }   /* new knob -> fresh tally */
    if (IsKeyPressed(KEY_V)) abx_reset();
    if (IsKeyPressed(KEY_SPACE)) abx_orbit = !abx_orbit;
    if (abx_orbit) {                             /* slow orbit + gentle bob; identical for A/B/X, so it never cues */
        abx_orbit_t += dt;
        float az = 0.45f * abx_orbit_t;
        source_pos = (Vector3){ 2.2f * cosf(az), g_head.y + 0.5f * sinf(0.31f * abx_orbit_t), 2.2f * sinf(az) };
    }
    if (abx_flash_t > 0.0f) { abx_flash_t -= dt; if (abx_flash_t < 0.0f) abx_flash_t = 0.0f; }
    bw_source_set_pos(e, src, source_pos.x, source_pos.y, source_pos.z);
    bw_source_set_gain(e, src, SRC_GAIN);
}
static void abx_draw3d(void) {
    DrawLine3D(g_head, source_pos, (Color){ 90, 220, 90, 200 });
    DrawSphere(source_pos, 0.18f, RED);
}
static void abx_hud(int y) {
    const AbxCmp* cm = &abx_cmps[abx_cmp];
    ui_text(TextFormat("[G] compare: %s   (A = %s, B = %s)   [SPACE] orbit %s",
                        cm->name, cm->a, cm->b, abx_orbit ? "ON" : "off"),
             12, y, 15, (Color){ 110, 200, 255, 255 });
    ui_text(TextFormat("listen:  [Z] A %s   [X] B %s   [C] X %s      answer:  [LEFT] X is A   [RIGHT] X is B   [V] reset",
                        abx_listen == 0 ? "<--" : "   ", abx_listen == 1 ? "<--" : "   ", abx_listen == 2 ? "<--" : "   "),
             12, y + 22, 15, (Color){ 235, 235, 120, 255 });
    if (abx_trials > 0) {
        double p = abx_pvalue(abx_trials, abx_correct);
        const char* verdict = (abx_trials < 6)  ? "keep going (need ~6+ trials)"
                            : (p < 0.05)        ? "DISTINGUISHABLE - you can hear it"
                                                : "not distinguishable yet (guessing?)";
        ui_text(TextFormat("score %d/%d   p = %.3f   %s%s", abx_correct, abx_trials, p, verdict,
                            abx_flash_t > 0.0f ? TextFormat("      last: %s (X was %s)",
                                                            abx_flash_ok ? "CORRECT" : "wrong",
                                                            abx_last_x ? "B" : "A") : ""),
                 12, y + 44, 15, (abx_flash_t > 0.0f) ? (abx_flash_ok ? (Color){ 120, 235, 130, 255 } : (Color){ 245, 140, 140, 255 })
                                                      : (Color){ 200, 200, 210, 255 });
    } else {
        ui_text("listen to A, B, X (switching is seamless - that's the point), then answer. 1-4 picks the signal.",
                 12, y + 44, 15, (Color){ 200, 200, 210, 255 });
    }
}

/* ============================= Scene 6: Reverb bed (static room) ============================= */
/* The hybrid reverb bed needs reflections configured + the room geometry set BEFORE bw_start (the
 * scene locks once the bed runs), so this scene runs on a SEPARATE engine config — build_engine()
 * rebuilds the engine when crossing this boundary (see switch_scene). */
#define ROOM_W 8.0f
#define ROOM_H 4.0f
#define ROOM_D 8.0f
static int   rev_on  = 1;
static float rev_wet = 1.0f;
static int   rev_decoder;                          /* bed decoder: 0 = sampling (SAD), 1 = AllRAD (B to A/B) */
static int   rev_dist;                             /* distance->wet send (V): near = drier, far = wetter */

static void rev_enter(void) {
    bw_source_set_gain(e, src, SRC_GAIN);
    bw_source_set_reflections(e, src, rev_on);    /* feed the source into the shared reverb bed */
    bw_source_set_reflection_distance(e, src, rev_dist);
    bw_reflections_set_gain(e, rev_wet);
}
static void rev_update(float dt) {
    if (IsKeyPressed(KEY_B)) {                     /* A/B the bed decoder: load-time, so rebuild the engine */
        rev_decoder ^= 1;
        if (e) { bw_stop(e); bw_destroy(e); e = NULL; }
        build_engine(1);
        rev_enter();                               /* re-apply source gain + reflections + wet on the new engine */
    }
    if (IsKeyPressed(KEY_G)) { rev_on = !rev_on; bw_source_set_reflections(e, src, rev_on); }
    if (IsKeyPressed(KEY_V)) { rev_dist = !rev_dist; bw_source_set_reflection_distance(e, src, rev_dist); }
    if (IsKeyDown(KEY_LEFT_BRACKET))  rev_wet = fmaxf(0.0f, rev_wet - 0.7f * dt);
    if (IsKeyDown(KEY_RIGHT_BRACKET)) rev_wet = fminf(2.0f, rev_wet + 0.7f * dt);
    bw_reflections_set_gain(e, rev_wet);
    source_pos.x = Clamp(source_pos.x, -ROOM_W * 0.5f + 0.5f, ROOM_W * 0.5f - 0.5f); /* keep it inside the room */
    source_pos.y = Clamp(source_pos.y, 0.5f, ROOM_H - 0.5f);                         /* floor-based box: y 0..H */
    source_pos.z = Clamp(source_pos.z, -ROOM_D * 0.5f + 0.5f, ROOM_D * 0.5f - 0.5f);
    bw_source_set_pos(e, src, source_pos.x, source_pos.y, source_pos.z);
    bw_source_set_gain(e, src, SRC_GAIN);
}
static void rev_draw3d(void) {
    DrawCubeWires((Vector3){ 0, ROOM_H * 0.5f, 0 }, ROOM_W, ROOM_H, ROOM_D, (Color){ 90, 110, 150, 130 });
    DrawLine3D(g_head, source_pos, (Color){ 90, 220, 90, 200 });
    DrawSphere(source_pos, 0.18f, RED);
}
static void rev_hud(int y) {
    ui_text(TextFormat("[G] reverb %s   [ ] wet %.2f   [B] bed decoder: %s   [V] distance->wet %s   move the source",
                        rev_on ? "ON (wet)" : "off (dry)", rev_wet, rev_decoder ? "AllRAD" : "sampling",
                        rev_dist ? "ON (near dry / far wet)" : "off"),
             12, y, 15, (Color){ 110, 200, 255, 255 });
    ui_text("8x4x8 m plaster room (Steam Audio bed); clicks [3]/bursts [2] show the tail. SAD vs AllRAD differ most on an IRREGULAR layout",
             12, y + 22, 15, (Color){ 200, 200, 210, 255 });
}

/* ---- scene table ---- */
typedef struct {
    const char* name;
    void (*enter)(void);
    void (*update)(float dt);
    void (*draw3d)(void);
    void (*hud)(int y);
} Scene;
static const Scene scenes[] = {
    { "Localization (DBAP)",     loc_enter,  loc_update,  loc_draw3d,  loc_hud  },
    { "Occlusion & Materials",   occ_enter,  occ_update,  occ_draw3d,  occ_hud  },
    { "Directivity",             dir_enter,  dir_update,  dir_draw3d,  dir_hud  },
    { "Channel walk (speaker check)", chan_enter, chan_update, chan_draw3d, chan_hud },
    { "Blind A/B/X (hear it, prove it)", abx_enter, abx_update, abx_draw3d, abx_hud },
    { "Reverb bed (static room)", rev_enter,  rev_update,  rev_draw3d,  rev_hud  },
};
enum { NSCENE = sizeof scenes / sizeof scenes[0], SCENE_REVERB = NSCENE - 1 };
static int cur_scene;
static int engine_has_reverb;       /* which config the live engine was built in */

/* (re)build the engine in the interactive (no-bed, dynamic geometry) or reverb (bed + static room)
 * config, reloading assets + recreating sources. Used at startup and on each reverb-boundary switch. */
static void build_engine(int with_reverb) {
    BwConfig cfg = {
        .profile = BW_PROFILE_BINAURAL, .layout_path = g_layout_path, .hrtf_path = NULL,
        .sample_rate = SR, .block_size = 256, .track_internal = false,
    };
    e = bw_create(&cfg);
    if (!e) { printf("bw_create failed\n"); exit(1); }
    if (with_reverb) {
        BwReflectionConfig rc = { .ir_seconds = 1.0f, .order = 1, .num_rays = 4096,
                                  .num_bounces = 16, .enabled = 1, .wet_gain = 1.0f };
        bw_reflections_config(e, &rc);
        BwMaterial rm = bw_material_preset(e, "plaster");
        BwMaterial faces[6] = { rm, rm, rm, rm, rm, rm };
        bw_scene_set_box(e, ROOM_W, ROOM_H, ROOM_D, faces);     /* static room, BEFORE bw_start */
        bw_set_bed_decoder(e, rev_decoder ? BW_DECODE_ALLRAD : BW_DECODE_SAMPLING);  /* load-time */
    }
    if (bw_start(e) != 0) {
        const char* err = bw_last_error(e);
        printf("bw_start: %s — no audio (install/select an ASIO driver, e.g. ASIO4ALL); the scene still runs.\n",
               err ? err : "?");
    }
    backend_name   = bw_audio_backend(e);
    backend_silent = (strncmp(backend_name, "asio", 4) != 0);
    bw_get_speakers(e, (float*)speakers, NSPK);            /* render the geometry the engine pans with */
    g_head = (Vector3){ 0, 0, 0 };                         /* ear point = array centroid (the engine's own ref) */
    for (int i = 0; i < NSPK; ++i) g_head = Vector3Add(g_head, speakers[i]);
    g_head = Vector3Scale(g_head, 1.0f / NSPK);
    for (int i = 0; i < NSIG; ++i) sounds[i] = bw_load_sound(e, sig_files[i]);
    for (int i = 0; i < NMAT; ++i) mats[i] = bw_material_preset(e, mat_names[i]);
    src  = bw_source_create(e);  bw_source_play(e, src,  sounds[cur_sig], true);
    refl = bw_source_create(e);  bw_source_play(e, refl, sounds[cur_sig], true);
    bw_source_set_gain(e, refl, 0.0f);
    if (with_reverb) bw_source_set_reflections(e, src, rev_on);
    engine_has_reverb = with_reverb;
}

/* leave the current scene at a clean baseline, then enter the new one (rebuilding the engine if we
 * are crossing the reverb boundary, since the bed + room geometry are load-time) */
static void switch_scene(int idx) {
    int want_reverb = (idx == SCENE_REVERB);
    if (want_reverb != engine_has_reverb) {
        printf("rebuilding engine: %s\n", want_reverb ? "reverb (bed + static room)" : "interactive");
        if (e) { bw_stop(e); bw_destroy(e); e = NULL; }
        build_engine(want_reverb);
    }
    for (uint32_t ch = 0; ch < NSPK; ++ch) bw_test_signal(e, ch, BW_TEST_OFF, 0.0f);  /* clear channel walk */
    bw_source_set_gain(e, refl, 0.0f);
    bw_source_set_occlusion(e, src, false);
    bw_source_set_directivity_preset(e, src, BW_DIR_OMNI);
    bw_source_set_orientation(e, src, 0.0f, 0.0f, 0.0f, 1.0f);   /* clear any aim left by the directivity scene */
    bw_source_set_doppler(e, src, false);                        /* propagation/size effects are localization-scene only */
    bw_source_set_air_absorption(e, src, false);
    bw_source_set_spread(e, src, 0.0f);
    bw_set_dual_band(e, false);
    bw_set_panner(e, BW_PAN_DBAP);                               /* the ABX scene may leave SPCAP/VBAP selected */
    source_yaw = 0.0f;
    bw_source_set_gain(e, src, SRC_GAIN);
    cur_scene = idx;
    scenes[idx].enter();
}

int main(int argc, char** argv) {
    _putenv("BWAUDIO_SINK=asio");                             /* headphone output via a 2-ch ASIO driver */

    /* optional surveyed layout: argv[1], else ./cave_layout.json if present, else the default grid */
    g_layout_path = (argc > 1) ? argv[1] : NULL;
    if (!g_layout_path) { FILE* lf = fopen("cave_layout.json", "rb"); if (lf) { fclose(lf); g_layout_path = "cave_layout.json"; } }

    /* synthesise the localization test signals to wav (the engine loads sounds from file) */
    float* sigbuf = (float*)malloc((size_t)SIGLEN * sizeof(float));
    if (!sigbuf) { printf("out of memory\n"); return 1; }
    for (int i = 0; i < NSIG; ++i) { gen_signal(i, sigbuf, SIGLEN); write_wav(sig_files[i], sigbuf, SIGLEN); }
    free(sigbuf);

    wall_basis(wall_n, &wall_u, &wall_v);
    build_engine(0);                                          /* start in the interactive config (fills speakers[], g_head) */
    source_pos.y = g_head.y;                                  /* start the source on the ear plane */
    printf("layout: %s    audio backend: %s%s\n", g_layout_path ? g_layout_path : "default grid", backend_name,
           backend_silent ? "   (SILENT — set BWAUDIO_ASIO_DRIVER to your headphone driver)" : "");
    if (cv_load("constraints.json", &g_con))                  /* orientation only; the layout tool edits against these */
        printf("constraints: bounds + %d no-go + %d obstacle box(es) drawn from ./constraints.json\n", g_con.nnogo, g_con.nobst);

    SetConfigFlags(FLAG_WINDOW_HIGHDPI | FLAG_MSAA_4X_HINT);   /* native pixel density + smooth 3D edges */
    InitWindow(1000, 700, "bwaudio - binaural playground");
    SetRandomSeed((unsigned int)time(NULL));                   /* the ABX scene's X draw must not repeat run-to-run */
    ui_text_init();                                            /* crisp TTF HUD (see ui_text.h) */
    SetTargetFPS(60);
    cam = (Camera3D){ .target = { 0, g_head.y, 0 }, .up = { 0, 1, 0 }, .fovy = 55, .projection = CAMERA_PERSPECTIVE };
    switch_scene(0);

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        if (IsKeyPressed(KEY_F11)) ToggleBorderlessWindowed();   /* fullscreen */
        float mv = 2.5f * dt, rt = 1.8f * dt;

        /* ---- global navigation (every scene) ---- */
        if (IsKeyDown(KEY_W)) source_pos.z += mv;
        if (IsKeyDown(KEY_S)) source_pos.z -= mv;
        if (IsKeyDown(KEY_A)) source_pos.x += mv;
        if (IsKeyDown(KEY_D)) source_pos.x -= mv;
        if (IsKeyDown(KEY_R)) source_pos.y += mv;
        if (IsKeyDown(KEY_F)) source_pos.y -= mv;
        if (IsKeyDown(KEY_Q)) head_yaw += rt;
        if (IsKeyDown(KEY_E)) head_yaw -= rt;
        for (int i = 0; i < NSIG; ++i)                        /* 1-4: switch the test signal everywhere */
            if (IsKeyPressed(KEY_ONE + i)) {
                cur_sig = i;
                bw_source_play(e, src,  sounds[i], true);
                bw_source_play(e, refl, sounds[i], true);
            }
        /* TAB last in this block ON PURPOSE: a reverb-boundary switch REBUILDS the engine (destroys e,
         * src, refl), so all per-source calls above must run against the still-valid engine first. */
        if (IsKeyPressed(KEY_TAB)) switch_scene((cur_scene + 1) % NSCENE);

        /* arcball camera: right-drag orbits, the wheel zooms */
        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
            Vector2 md = GetMouseDelta();
            cam_yaw   -= md.x * 0.005f;
            cam_pitch += md.y * 0.005f;
            if (cam_pitch >  1.5f) cam_pitch =  1.5f;
            if (cam_pitch < -1.5f) cam_pitch = -1.5f;
        }
        cam_dist -= GetMouseWheelMove() * 0.6f;
        if (cam_dist < 1.5f)  cam_dist = 1.5f;
        if (cam_dist > 25.0f) cam_dist = 25.0f;
        cam.position.x = cam.target.x + cam_dist * cosf(cam_pitch) * sinf(cam_yaw);
        cam.position.y = cam.target.y + cam_dist * sinf(cam_pitch);
        cam.position.z = cam.target.z + cam_dist * cosf(cam_pitch) * cosf(cam_yaw);

        /* ---- per-scene update, then publish the frame ---- */
        highlight_spk = -1;
        scenes[cur_scene].update(dt);
        Quaternion q = QuaternionFromAxisAngle((Vector3){ 0, 1, 0 }, head_yaw);
        bw_set_listener_pose(e, g_head.x, g_head.y, g_head.z, q.x, q.y, q.z, q.w);
        bw_commit(e);

        BeginDrawing();
        ClearBackground((Color){ 25, 25, 30, 255 });
        BeginMode3D(cam);
        DrawGrid(12, 0.5f);
        draw_speakers(highlight_spk);
        scenes[cur_scene].draw3d();
        draw_head(q);
        EndMode3D();
        draw_axes_hud(cam, 56.f, (float)GetScreenHeight() - 56.f, 30.f);   /* room axes, bottom-left */

        /* HUD: a dark backing panel + bright ASCII text (raylib's default font has no em-dash/box glyphs).
         * Scene HUDs use up to three lines from y=52 (the localization scene's propagation line is the
         * tallest), so the audio-backend line sits below them at y=120 to avoid overlapping. */
        DrawRectangle(0, 0, GetScreenWidth(), 144, (Color){ 0, 0, 0, 195 });
        ui_text("[TAB] scene   WASD/RF move source   Q/E head   1-4 signal   right-drag/wheel camera   F11 fullscreen   ESC",
                 12, 8, 14, RAYWHITE);
        ui_text(TextFormat("scene %d/%d:  %s", cur_scene + 1, NSCENE, scenes[cur_scene].name),
                 12, 28, 16, (Color){ 235, 235, 120, 255 });
        scenes[cur_scene].hud(52);
        if (backend_silent)
            ui_text("audio: NULL sink - NO SOUND (set BWAUDIO_ASIO_DRIVER; see console)", 12, 120, 15, (Color){ 255, 110, 110, 255 });
        else
            ui_text(TextFormat("audio: %s", backend_name), 12, 120, 15, (Color){ 110, 235, 130, 255 });
        EndDrawing();
    }

    CloseWindow();
    if (e) { bw_stop(e); bw_destroy(e); }
    for (int i = 0; i < NSIG; ++i) remove(sig_files[i]);
    return 0;
}
