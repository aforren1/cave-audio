/*
 * playground.cpp — interactive harness for bw_audio's binaural monitor, split into SCENES,
 * each auditioning one feature by ear on headphones (the binaural profile, output through a 2-ch
 * ASIO driver the engine auto-picks — ASIO4ALL / FlexASIO / the Steinberg built-in; without one the
 * engine falls back to the offline null sink and everything still renders, just silent). This is the
 * by-ear evaluation the automated tests can't do. Public C ABI only (bw_audio.h); raylib's raymath
 * provides the vector/quaternion math.
 *
 * UI stack: the 3D scene (speakers with live level shading, source, head, room) stays raylib; every
 * control surface is Dear ImGui via rlImGui, themed like the station (bwa_theme.h) — the same split
 * as bwa_layout_tool. imgui_test_engine drives the ACTUAL panel with fake inputs under ctest
 * (`--tests [filter]`, forced onto the null sink for determinism), captures screenshots to
 * output/captures/, and exits pass/fail. Keyboard shortcuts act only when imgui doesn't want the
 * keyboard; the camera only drags when the cursor isn't over the UI (io.WantCapture*).
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
 *   4 Channel walk      — bwa_set_test_signal drives ONE raw output channel (speaker-check tool). Step
 *                         channels with LEFT/RIGHT (SPACE auto-walks); in binaural each channel is
 *                         HRTF'd as its virtual speaker, so the tone circles your head as you walk.
 *   5 Blind A/B/X       — double-blind self test over live engine knobs (dual-band, DBAP vs SPCAP /
 *                         VBAP, spread, spread RENDER (LOBE vs MDAP / LOBE vs SPECTRAL),
 *                         decorrelation, air absorption): X is secretly A or B, listen with Z/X/C,
 *                         answer LEFT/RIGHT, and the running binomial p-value says whether the
 *                         difference is genuinely audible (p < 0.05) or you're guessing.
 *   6 Ambisonic bed     — a synthesized 3rd-order AmbiX field (bursts FRONT, clicks LEFT-UP, a
 *                         diffuse floor) played world-locked through the bed decode. SPACE spins the
 *                         field (bwa_bed_set_orientation glides), the tilt slider pitches it, G A/Bs
 *                         matrix vs PARAMETRIC rendering, B A/Bs max-rE decode weighting — the by-ear
 *                         home for the bed knobs.
 *   7 Reverb bed        — a static shoebox room + the Steam Audio hybrid reverb bed. Move the source
 *                         and the room reverb follows; G dry/wet A-B, [ ] wet level, V distance->wet
 *                         (near dry / far wet), B A/Bs the bed
 *                         DECODER (sampling vs AllRAD — load-time, so it rebuilds the engine; differs
 *                         most on an irregular layout). The bed + room geometry are LOAD-time (the room
 *                         locks once the bed runs), so entering/leaving this scene REBUILDS the engine
 *                         (a brief audio gap). Transient signals (clicks/bursts) show the tail best.
 *
 * Global keys: WASD/RF move source, Q/E turn head, 1-4 signal, TAB scene, right-drag/wheel camera, ESC.
 * Needs the Steam Audio build for occlusion/materials/directivity/reverb; without it those are no-ops.
 * Usage: bwa_playground [cave_layout.json] — audition with your surveyed layout (renders + pans with the
 *        engine's actual speaker positions); with no arg it auto-loads ./cave_layout.json or the default grid.
 * Build: cmake -S . -B build -DBWA_BUILD_PLAYGROUND=ON && cmake --build build
 */
#include "bw_audio.h"
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"           /* rlDrawRenderBatchActive: flush the 3D batch before a screenshot */
#include "speaker_gizmo.h"  /* the "real speaker" glyph (cabinet + cone aimed at the listener) */
#include "constraints_view.h"  /* constraints.json boxes, drawn for orientation (same view as the layout tool) */
#include "axes_hud.h"       /* screen-corner XYZ triad, shared with the layout tool */

#include "imgui.h"
#include "rlImGui.h"
#include "bwa_theme.h"       /* station theme + embedded Roboto (applyTheme / loadEmbeddedFont / uiScaled) */
#include "imgui_te_engine.h"
#include "imgui_te_context.h"
#include "imgui_te_ui.h"

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SR   48000u
#define NSPK 26                 /* array CAPACITY; g_nspk is the engine's ACTIVE count (the layout's) */

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

/* ---- ambisonic bed content (the bed scene): a 3rd-order AmbiX field synthesized at startup ----
 * SN3D real SH to order 3 for a ROOM direction (ambi axes x=front,y=left,z=up = room z,x,y) — a
 * local copy of the encode so the playground stays a pure ABI client (mirrors examples/ambisonic.c). */
#define BED_FILE "pg_bed.wav"
static void sh16_room(float rx, float ry, float rz, float* y) {
    const float len = sqrtf(rx*rx + ry*ry + rz*rz);
    const float x = rz / len, yy = rx / len, z = ry / len;
    y[0]  = 1.0f;
    y[1]  = yy;             y[2]  = z;              y[3]  = x;
    y[4]  = 1.7320508f * x * yy;
    y[5]  = 1.7320508f * yy * z;
    y[6]  = 0.5f * (3.0f * z * z - 1.0f);
    y[7]  = 1.7320508f * x * z;
    y[8]  = 0.8660254f * (x * x - yy * yy);
    y[9]  = 0.7905694f * yy * (3.0f * x * x - yy * yy);
    y[10] = 3.8729833f * x * yy * z;
    y[11] = 0.6123724f * yy * (5.0f * z * z - 1.0f);
    y[12] = 0.5f * z * (5.0f * z * z - 3.0f);
    y[13] = 0.6123724f * x * (5.0f * z * z - 1.0f);
    y[14] = 1.9364917f * z * (x * x - yy * yy);
    y[15] = 0.7905694f * x * (x * x - 3.0f * yy * yy);
}
/* the bed scene's marker bearings (room space): bursts from the FRONT, clicks from LEFT-UP */
static const Vector3 bed_dirs[2] = { { 0.0f, 0.0f, 1.0f }, { 0.87f, 0.5f, 0.0f } };
static void gen_bed(float* buf /* n x 16 */, uint32_t n) {
    float yf[16], yl[16];
    sh16_room(bed_dirs[0].x, bed_dirs[0].y, bed_dirs[0].z, yf);
    sh16_room(bed_dirs[1].x, bed_dirs[1].y, bed_dirs[1].z, yl);
    unsigned int s1 = 33333u, s2 = 44444u, s3 = 55555u;
    float pb1[7] = { 0 }, pb3[7] = { 0 };
    const uint32_t period = SR / 2, on = period / 2, ramp = SR / 100, clicklen = SR / 333;
    for (uint32_t i = 0; i < n; ++i) {                    /* period divides SIGLEN: seamless loop */
        uint32_t ph = i % period;
        float env = (ph < ramp) ? (float)ph / ramp
                  : (ph < on - ramp) ? 1.0f
                  : (ph < on) ? (float)(on - ph) / ramp : 0.0f;
        float burst = pink(white(&s1), pb1) * 0.5f * env;
        uint32_t pc = (i + period / 2) % period;          /* clicks in the bursts' gaps */
        float click = (pc < clicklen) ? white(&s2) * expf(-6.0f * (float)pc / clicklen) * 0.7f : 0.0f;
        float dif   = pink(white(&s3), pb3) * 0.08f;      /* W-only: reads as diffuse */
        float* f = buf + (size_t)i * 16;
        for (int k = 0; k < 16; ++k) f[k] = burst * yf[k] + click * yl[k];
        f[0] += dif;
    }
}
static int write_wav16(const char* path, const float* buf, uint32_t n) {
    drwav_data_format fmt = { drwav_container_riff, DR_WAVE_FORMAT_IEEE_FLOAT, 16, SR, 32 };
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
    Vector3 up = (fabsf(n.y) > 0.9f) ? Vector3{ 1, 0, 0 } : Vector3{ 0, 1, 0 };
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
static bwa_engine*   e;
static bwa_source    src, refl;
static bwa_sound     sounds[NSIG];
static bwa_sound     g_bed_snd;    /* the synthesized AmbiX field (BED_FILE), loaded per engine build */
static bwa_bed       g_bed;        /* the bed voice (created per build; only the bed scene plays it) */
static const char* backend_name;
static int         backend_silent;
static const char* g_layout_path;          /* optional cave_layout.json; NULL = engine default grid */

static Vector3 speakers[NSPK];
static int     g_nspk = NSPK;     /* the engine's ACTIVE channel count (bwa_get_speakers); the layout's */
static Vector3 g_head;            /* the ear point = array centroid (the engine's nominal listening point);
                                   * room origin is on the FLOOR, so the head is NOT at the origin */
static Vector3 source_pos = { 1.5f, 0.0f, 0.0f };   /* y re-based to ear height once the layout is known */
static float   head_yaw, source_yaw;
static int     cur_sig;
static int     highlight_spk = -1;          /* a scene may highlight one speaker each frame; reset per frame */

/* keyboard/mouse gating: scene shortcuts act only when imgui doesn't want the device (set per frame
 * from io.WantCapture*; kp/kd wrap raylib so every scene handler is gated without touching them). */
static bool g_kb = true, g_ms = true;
static bool kp(int k) { return g_kb && IsKeyPressed(k); }
static bool kd(int k) { return g_kb && IsKeyDown(k); }

/* wall + materials (occlusion scene) */
static Vector3       wall_c = { 0.0f, 1.2f, -2.2f };   /* standing on the floor: spans y 0..2.4, blocks the ear line */
static const Vector3 wall_n = { 0, 0, 1 };
static const float   wall_hw = 2.0f, wall_hh = 1.2f;
static Vector3       wall_u, wall_v;
static const char*   mat_names[] = { "concrete", "glass", "carpet", "wood", "metal" };   /* UI labels */
static const bwa_material_type mat_types[] = { BWA_MAT_CONCRETE, BWA_MAT_GLASS, BWA_MAT_CARPET, BWA_MAT_WOOD, BWA_MAT_METAL };
/* broadband reflectivity (~1 - mean absorption of the same presets): scales the image-source level so
 * the wall MATERIAL drives reflectivity too — carpet reflects far less than metal/concrete. Broadband
 * only (one DBAP voice has no per-source EQ); the full per-band material reflection is the reverb bed's job. */
static const float   mat_refl[] = { 0.93f, 0.96f, 0.45f, 0.92f, 0.89f };
enum { NMAT = 5 };
static bwa_material    mats[NMAT];
static int           cur_mat, refl_audible = 1, occ_audible = 1;
static Vector3       occ_image, occ_refl_pt;     /* computed in occ_update, drawn in occ_draw3d */
static int           occ_refl_valid, occ_occluded;
static float         occ_factor = 1.0f;

/* directivity scene */
static const char* dir_names[] = { "omni", "cardioid", "figure-8" };
static int         cur_dir = 1;                  /* default cardioid so the effect is audible on entry */
static float       dir_gain = 1.0f;

/* channel-walk scene. chan_active/chan_kind are the REQUESTED state (keys or panel write them);
 * chan_update applies any change once per frame (old channel off, new on), so both input paths share
 * the same off/on switch. chan_applied = -1 marks "nothing driven yet" (scene entry re-applies). */
static int   chan_active, chan_kind = BWA_TEST_SINE, chan_auto;
static int   chan_applied = -1, chan_kind_applied = BWA_TEST_SINE;
static float chan_timer;

/* camera (shared) */
static Camera3D cam;
static float    cam_yaw = 195.0f * DEG2RAD, cam_pitch = 25.0f * DEG2RAD, cam_dist = 8.4f;

/* register the wall quad as occluding geometry with a material token. Safe at runtime because the
 * reverb bed isn't enabled (occlusion geometry is dynamic; it locks only while the bed runs). */
static void push_wall_mesh(bwa_material mat) {
    Vector3 a  = Vector3Add(Vector3Add(wall_c, Vector3Scale(wall_u, -wall_hw)), Vector3Scale(wall_v, -wall_hh));
    Vector3 b  = Vector3Add(Vector3Add(wall_c, Vector3Scale(wall_u,  wall_hw)), Vector3Scale(wall_v, -wall_hh));
    Vector3 d  = Vector3Add(Vector3Add(wall_c, Vector3Scale(wall_u,  wall_hw)), Vector3Scale(wall_v,  wall_hh));
    Vector3 ee = Vector3Add(Vector3Add(wall_c, Vector3Scale(wall_u, -wall_hw)), Vector3Scale(wall_v,  wall_hh));
    float verts[12] = { a.x, a.y, a.z,  b.x, b.y, b.z,  d.x, d.y, d.z,  ee.x, ee.y, ee.z };
    int   tris[6]   = { 0, 1, 2,  0, 2, 3 };
    bwa_material tri_mat[2] = { mat, mat };
    bwa_scene_set_mesh_mat(e, verts, 4, tris, 2, tri_mat);
}

/* ---- shared drawing ---- */
static CvConstraints g_con;     /* ./constraints.json, if present — drawn in every scene for orientation */

static float spk_lv[NSPK];      /* smoothed per-speaker activity (0..1): instant attack, ~1/3 s release */

static void draw_speakers(int hi) {
    cv_draw(&g_con);            /* the room's bounds/no-go/obstacle boxes, same colors as the layout tool */
    float pk[NSPK] = { 0 };
    bwa_get_bus_levels(e, pk, NSPK);                             /* last block's per-channel output peak */
    const float rel = fminf(1.0f, 6.0f * GetFrameTime());       /* release one-pole (attack is instant) */
    for (int k = 0; k < g_nspk; ++k) {
        /* block peak -> display level over a 60 dB window, so quiet DBAP tails still read */
        float db = pk[k] > 1e-6f ? 20.0f * log10f(pk[k]) : -120.0f;
        float lv = db <= -60.0f ? 0.0f : (db >= 0.0f ? 1.0f : 1.0f + db / 60.0f);
        spk_lv[k] = lv > spk_lv[k] ? lv : spk_lv[k] + (lv - spk_lv[k]) * rel;
        float t = spk_lv[k];
        Color col = (k == hi) ? Color{ 120, 240, 140, 255 }   /* a scene's explicit highlight wins */
                              : Color{ (unsigned char)(120 + t * 125.0f),   /* idle gray -> driven amber */
                                         (unsigned char)(120 + t *  85.0f),
                                         (unsigned char)(140 - t *  60.0f), 255 };
        draw_speaker_gizmo(speakers[k], g_head,                 /* cones aim at the head (the array centroid) */
                           (k == hi) ? 0.30f : 0.22f + 0.06f * t, col);
    }
}
static void draw_head(Quaternion q) {
    /* ear/nose axes from the ABI's room-frame identity basis (bw_audio.h BWA_ROOM_*) */
    Vector3 right = Vector3RotateByQuaternion(Vector3{ BWA_ROOM_RIGHT[0], BWA_ROOM_RIGHT[1], BWA_ROOM_RIGHT[2] }, q);
    Vector3 fwd   = Vector3RotateByQuaternion(Vector3{ BWA_ROOM_AHEAD[0], BWA_ROOM_AHEAD[1], BWA_ROOM_AHEAD[2] }, q);
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
    bwa_source_set_gain(e, src, SRC_GAIN); loc_auto = 0; loc_flyby = 0; loc_trail_len = 0;
    bwa_source_set_doppler(e, src, loc_dop);            /* re-apply (switch_scene cleared them) */
    bwa_source_set_air_absorption(e, src, loc_air);
    bwa_source_set_spread(e, src, loc_spread);
    bwa_set_dual_band(e, loc_dual);
}
static void loc_update(float dt) {
    if (kp(KEY_SPACE)) { loc_auto = !loc_auto; if (loc_auto) { loc_flyby = 0; loc_t = 0.0f; loc_trail_len = 0; } }
    if (kp(KEY_X))     { loc_flyby = !loc_flyby; if (loc_flyby) { loc_auto = 0; loc_fly_t = 0.0f; loc_trail_len = 0; } }
    if (kp(KEY_V)) { loc_dop = !loc_dop; bwa_source_set_doppler(e, src, loc_dop); }
    if (kp(KEY_B)) { loc_air = !loc_air; bwa_source_set_air_absorption(e, src, loc_air); }
    if (kp(KEY_C)) {                            /* cycle source size: point -> .4 -> .7 -> wide -> point */
        loc_spread = (loc_spread < 0.05f) ? 0.4f : (loc_spread < 0.5f) ? 0.7f : (loc_spread < 0.85f) ? 1.0f : 0.0f;
        bwa_source_set_spread(e, src, loc_spread);
    }
    if (kp(KEY_M)) { loc_dual = !loc_dual; bwa_set_dual_band(e, loc_dual); }   /* dual-band A/B */
    if (loc_flyby) {                                      /* fast straight pass 0.8 m in front: the Doppler demo */
        loc_fly_t += dt;
        float period = 3.6f, u = fmodf(loc_fly_t, period) / period;     /* there-and-back, ~7.8 m/s */
        float x = (u < 0.5f) ? (-7.0f + 28.0f * u) : (7.0f - 28.0f * (u - 0.5f));
        source_pos = Vector3{ x, g_head.y, 0.8f };
    } else if (loc_auto) {
        loc_t += dt;
        float az = 0.62f * loc_t;                         /* circle the listener   (~10 s / orbit) */
        float r  = 2.0f + 1.2f * sinf(0.90f * loc_t);     /* near <-> far  0.8..3.2 (~7 s) */
        float y  = 1.2f  * sinf(1.14f * loc_t);           /* low  <-> high +/-1.2 about ear height (~5.5 s) */
        source_pos = Vector3{ r * cosf(az), g_head.y + y, r * sinf(az) };
    }
    if (loc_auto || loc_flyby) {
        if (loc_trail_len < LOC_TRAIL) loc_trail[loc_trail_len++] = source_pos;
        else { memmove(loc_trail, loc_trail + 1, (LOC_TRAIL - 1) * sizeof(Vector3)); loc_trail[LOC_TRAIL - 1] = source_pos; }
    }
    bwa_source_set_pos(e, src, source_pos.x, source_pos.y, source_pos.z);
    bwa_source_set_gain(e, src, SRC_GAIN);
}
static void loc_draw3d(void) {
    if (loc_auto) {
        for (int i = 1; i < loc_trail_len; ++i)           /* fading trail of the swept path */
            DrawLine3D(loc_trail[i - 1], loc_trail[i], Color{ 90, 220, 90, (unsigned char)(40 + 180 * i / loc_trail_len) });
        DrawLine3D(source_pos, Vector3{ source_pos.x, 0, source_pos.z }, Color{ 90, 220, 90, 60 }); /* height drop line */
    }
    DrawLine3D(g_head, source_pos, Color{ 90, 220, 90, 220 });
    DrawSphere(source_pos, 0.18f, RED);
}
/* ====================== Scene 2: Occlusion & Materials (real Steam Audio) ====================== */
static void occ_enter(void) {
    bwa_source_set_gain(e, src, SRC_GAIN);
    bwa_source_set_occlusion(e, src, occ_audible);
    push_wall_mesh(mats[cur_mat]);
}
static void occ_update(float dt) {
    float mv = 2.5f * dt;
    if (kd(KEY_LEFT_BRACKET) || kd(KEY_RIGHT_BRACKET)) {
        wall_c = Vector3Add(wall_c, Vector3Scale(wall_n, kd(KEY_LEFT_BRACKET) ? -mv : mv));
        push_wall_mesh(mats[cur_mat]);
    }
    if (kp(KEY_M)) { cur_mat = (cur_mat + 1) % NMAT; push_wall_mesh(mats[cur_mat]); }
    if (kp(KEY_T)) refl_audible = !refl_audible;
    if (kp(KEY_G)) { occ_audible = !occ_audible; bwa_source_set_occlusion(e, src, occ_audible); }

    /* reflection: the source mirrored across the wall is valid when source + listener are on the same
     * side and the bounce lands on the panel. occlusion: the direct path crosses the panel. */
    const Vector3 L = g_head;
    occ_image = reflect_point(source_pos, wall_c, wall_n);
    int same_side = (Vector3DotProduct(Vector3Subtract(source_pos, wall_c), wall_n) > 0)
                 == (Vector3DotProduct(Vector3Subtract(L, wall_c), wall_n) > 0);
    float t; Vector3 hit; occ_refl_valid = 0; occ_refl_pt = source_pos;
    if (same_side && seg_plane(L, occ_image, wall_c, wall_n, &t, &hit) && t > 0 && t < 1 &&
        in_panel(hit, wall_c, wall_u, wall_v, wall_hw, wall_hh)) { occ_refl_valid = 1; occ_refl_pt = hit; }

    bwa_source_set_pos(e, src, source_pos.x, source_pos.y, source_pos.z);
    bwa_source_set_gain(e, src, SRC_GAIN);                       /* occlusion is applied by the engine */
    bwa_source_set_pos(e, refl, occ_image.x, occ_image.y, occ_image.z);
    bwa_source_set_gain(e, refl, (refl_audible && occ_refl_valid) ? SRC_GAIN * REFL_GAIN * mat_refl[cur_mat] : 0.0f);

    occ_factor   = bwa_source_get_occlusion(e, src);             /* real Steam Audio occlusion (1 = clear) */
    occ_occluded = occ_factor < 0.85f;
}
static void occ_draw3d(void) {
    draw_wall(wall_c, wall_u, wall_v, wall_hw, wall_hh,
              Color{ 90, 110, 140, 90 }, Color{ 150, 180, 220, 255 });
    DrawLine3D(g_head, source_pos, occ_occluded ? Color{ 230, 70, 70, 255 } : Color{ 90, 220, 90, 220 });
    DrawSphere(source_pos, 0.18f, RED);
    if (occ_refl_valid) {
        DrawLine3D(source_pos, occ_refl_pt, ORANGE);
        DrawLine3D(occ_refl_pt, g_head, ORANGE);
        DrawSphere(occ_refl_pt, 0.06f, ORANGE);
        DrawSphere(occ_image, 0.14f, Color{ 230, 160, 60, 130 });
        DrawLine3D(occ_image, occ_refl_pt, Color{ 230, 160, 60, 80 });
    }
}
/* ============================= Scene 3: Directivity (weighted dipole) ============================= */
static void dir_enter(void) {
    bwa_source_set_gain(e, src, SRC_GAIN);
    bwa_source_set_directivity_preset(e, src, (bwa_directivity)cur_dir);
}
static void dir_update(float dt) {
    float rt = 1.8f * dt;
    if (kp(KEY_Z)) { cur_dir = (cur_dir + 1) % 3; bwa_source_set_directivity_preset(e, src, (bwa_directivity)cur_dir); }
    if (kd(KEY_COMMA))  source_yaw += rt;
    if (kd(KEY_PERIOD)) source_yaw -= rt;
    Quaternion sq = QuaternionFromAxisAngle(Vector3{ 0, 1, 0 }, source_yaw);
    bwa_source_set_pos(e, src, source_pos.x, source_pos.y, source_pos.z);
    bwa_source_set_gain(e, src, SRC_GAIN);
    bwa_source_set_orientation(e, src, sq.x, sq.y, sq.z, sq.w);
    dir_gain = bwa_source_get_directivity(e, src);              /* 1 = on-axis/omni .. 0 = null */
}
static void dir_draw3d(void) {
    DrawLine3D(g_head, source_pos, Color{ 90, 220, 90, 180 });
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
        if (i > 0) DrawLine3D(prev, p, Color{ 255, 180, 80, 200 });
        prev = p;
    }
}
/* ============================= Scene 4: Channel walk (bwa_set_test_signal) ============================= */
static void chan_set(int ch) { bwa_set_test_signal(e, (uint32_t)ch, (bwa_test_kind)chan_kind, TEST_GAIN); }
static void chan_enter(void) {
    bwa_source_set_gain(e, src,  0.0f);          /* silence the spatial voices; only the test tone sounds */
    bwa_source_set_gain(e, refl, 0.0f);
    chan_timer = 0.0f;
    chan_auto  = 0;                             /* start manual each visit (don't resume a prior auto-walk) */
    chan_set(chan_active);
    chan_applied = chan_active; chan_kind_applied = chan_kind;
}
static void chan_update(float dt) {
    if (kp(KEY_RIGHT)) chan_active = (chan_active + 1) % g_nspk;
    if (kp(KEY_LEFT))  chan_active = (chan_active + g_nspk - 1) % g_nspk;
    if (kp(KEY_N))     chan_kind = (chan_kind == BWA_TEST_SINE) ? BWA_TEST_NOISE : BWA_TEST_SINE;
    if (kp(KEY_SPACE)) chan_auto = !chan_auto;
    if (chan_auto && (chan_timer += dt) >= 0.7f) { chan_timer = 0.0f; chan_active = (chan_active + 1) % g_nspk; }
    if (chan_active != chan_applied || chan_kind != chan_kind_applied) {   /* keys OR the panel moved it */
        if (chan_applied >= 0) bwa_set_test_signal(e, (uint32_t)chan_applied, BWA_TEST_OFF, 0.0f);
        chan_set(chan_active);
        chan_applied = chan_active; chan_kind_applied = chan_kind;
    }
    highlight_spk = chan_active;
}
static void chan_draw3d(void) {
    DrawLine3D(g_head, speakers[chan_active], Color{ 120, 235, 150, 200 });
}
/* ============================= Scene 5: Blind A/B/X ============================= */
/* Double-blind self test: A and B are two settings of ONE engine knob, X is randomly one of them.
 * Listen to all three freely (every switch is live + click-free: ramped gains / atomic toggles),
 * answer with LEFT ("X is A") or RIGHT ("X is B"), and the one-sided binomial tail over your trials
 * says whether you can ACTUALLY hear the difference (p < 0.05) or you're guessing — it turns
 * "sounds different to me" into a measurement. These are exactly the by-ear judgments the automated
 * tests can't make: dual-band panning, panner choice, source spread, air absorption. */
typedef struct { const char* name; const char* a; const char* b; void (*apply)(int v); } AbxCmp;
static void abx_ap_dual (int v) { bwa_set_dual_band(e, v); }
static void abx_ap_spcap(int v) { bwa_set_panner(e, v ? BWA_PAN_SPCAP : BWA_PAN_DBAP); }
static void abx_ap_vbap (int v) { bwa_set_panner(e, v ? BWA_PAN_VBAP  : BWA_PAN_DBAP); }
static void abx_ap_sprd (int v) { bwa_source_set_spread(e, src, v ? 0.6f : 0.0f); }
static void abx_ap_air  (int v) { bwa_source_set_air_absorption(e, src, v); }
/* the spread-RENDER comparisons need a wide source to have anything to render */
static void abx_ap_mdap (int v) { bwa_source_set_spread(e, src, 0.6f);
                                  bwa_set_spread_mode(e, v ? BWA_SPREAD_MDAP : BWA_SPREAD_LOBE); }
static void abx_ap_spec (int v) { bwa_source_set_spread(e, src, 0.6f);
                                  bwa_set_spread_mode(e, v ? BWA_SPREAD_SPECTRAL : BWA_SPREAD_LOBE); }
static void abx_ap_decor(int v) { bwa_source_set_spread(e, src, 0.6f); bwa_set_decorrelation(e, v); }
static const AbxCmp abx_cmps[] = {
    { "dual-band panning",     "single-band (power)", "dual-band (LF amplitude)", abx_ap_dual  },
    { "panner: DBAP vs SPCAP", "DBAP",                "SPCAP",                    abx_ap_spcap },
    { "panner: DBAP vs VBAP",  "DBAP",                "VBAP",                     abx_ap_vbap  },
    { "source spread",         "point source",        "spread 60%",               abx_ap_sprd  },
    { "spread render: MDAP",   "LOBE (reshape)",      "MDAP (virtual ring)",      abx_ap_mdap  },
    { "spread render: SPECTRAL", "LOBE (reshape)",    "SPECTRAL (freq-dep pan)",  abx_ap_spec  },
    { "decorrelation (wide src)", "coherent copies",  "velvet-noise decorrelated", abx_ap_decor },
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
    bwa_set_dual_band(e, false);
    bwa_set_panner(e, BWA_PAN_DBAP);
    bwa_source_set_spread(e, src, 0.0f);
    bwa_set_spread_mode(e, BWA_SPREAD_LOBE);
    bwa_set_decorrelation(e, false);
    bwa_source_set_air_absorption(e, src, false);
    abx_cmps[abx_cmp].apply(abx_listen == 2 ? abx_x : abx_listen);
}
static void abx_new_trial(void) { abx_x = GetRandomValue(0, 1); abx_listen = 2; abx_apply_listen(); }
static void abx_reset(void)     { abx_trials = abx_correct = 0; abx_flash_t = 0.0f; abx_new_trial(); }
/* shared by the key handlers AND the panel buttons, so both paths score identically */
static void abx_set_listen(int which) { abx_listen = which; abx_apply_listen(); }
static void abx_answer(int guess) {
    abx_last_x   = abx_x;
    abx_flash_ok = (guess == abx_x); abx_correct += abx_flash_ok; ++abx_trials; abx_flash_t = 1.6f;
    abx_new_trial();                             /* reveal + immediately deal the next X */
}
static void abx_enter(void) {
    bwa_source_set_gain(e, src, SRC_GAIN);
    abx_orbit = 1; abx_orbit_t = 0.0f;           /* default: slow orbit — motion exposes panner differences */
    abx_new_trial();                             /* keep the tally across visits; only X is redrawn */
}
static void abx_update(float dt) {
    if (kp(KEY_Z)) abx_set_listen(0);
    if (kp(KEY_X)) abx_set_listen(1);
    if (kp(KEY_C)) abx_set_listen(2);
    if (kp(KEY_LEFT) || kp(KEY_RIGHT)) abx_answer(kp(KEY_RIGHT) ? 1 : 0);
    if (kp(KEY_G)) { abx_cmp = (abx_cmp + 1) % NABX; abx_reset(); }   /* new knob -> fresh tally */
    if (kp(KEY_V)) abx_reset();
    if (kp(KEY_SPACE)) abx_orbit = !abx_orbit;
    if (abx_orbit) {                             /* slow orbit + gentle bob; identical for A/B/X, so it never cues */
        abx_orbit_t += dt;
        float az = 0.45f * abx_orbit_t;
        source_pos = Vector3{ 2.2f * cosf(az), g_head.y + 0.5f * sinf(0.31f * abx_orbit_t), 2.2f * sinf(az) };
    }
    if (abx_flash_t > 0.0f) { abx_flash_t -= dt; if (abx_flash_t < 0.0f) abx_flash_t = 0.0f; }
    bwa_source_set_pos(e, src, source_pos.x, source_pos.y, source_pos.z);
    bwa_source_set_gain(e, src, SRC_GAIN);
}
static void abx_draw3d(void) {
    DrawLine3D(g_head, source_pos, Color{ 90, 220, 90, 200 });
    DrawSphere(source_pos, 0.18f, RED);
}
/* ============================= Scene 6: Ambisonic bed ============================= */
/* A pre-encoded 3rd-order AmbiX field (synthesized at startup — see gen_bed) played WORLD-LOCKED
 * through the bed decode: the by-ear home for the bed knobs. SPACE spins the field
 * (bwa_bed_set_orientation glides, click-free), the tilt slider pitches it (the full 3-axis
 * rotation), G A/Bs matrix vs parametric rendering, B A/Bs max-rE decode weighting. */
static int   bed_spin, bed_param, bed_re;
static float bed_yaw, bed_pitch;

static void bed_apply(void) {
    bwa_set_bed_renderer(e, bed_param ? BWA_BED_PARAMETRIC : BWA_BED_MATRIX);
    bwa_set_max_re(e, bed_re != 0);
    bwa_bed_set_orientation(e, g_bed, bed_yaw, bed_pitch, 0.0f);
}
static void bed_enter(void) {
    bwa_source_set_gain(e, src,  0.0f);            /* the bed IS the content here */
    bwa_source_set_gain(e, refl, 0.0f);
    bed_apply();
    bwa_bed_set_gain(e, g_bed, 0.9f);
    bwa_bed_play(e, g_bed, g_bed_snd, true);
}
static void bed_update(float dt) {
    if (kp(KEY_SPACE)) bed_spin = !bed_spin;
    if (kp(KEY_G)) bed_param = !bed_param;
    if (kp(KEY_B)) bed_re = !bed_re;
    /* yaw ACCUMULATES while spinning (no wrap: a target wrapped by 2pi would glide the long way
     * back at the engine's ~1 turn/s rate); float precision is fine for hours of spin */
    if (bed_spin) bed_yaw += 0.45f * dt;
    bed_apply();
}
static void bed_draw3d(void) {
    /* markers where the field's content sits NOW: the encode bearings through the live orientation
     * (pitch about room right, then yaw about room up — the engine's own convention) */
    static const Color cols[2] = { { 235, 120, 120, 255 }, { 120, 200, 235, 255 } };   /* front / left-up */
    const float cy = cosf(bed_yaw), sy = sinf(bed_yaw), cp = cosf(bed_pitch), sp = sinf(bed_pitch);
    for (int i = 0; i < 2; ++i) {
        Vector3 d = Vector3Normalize(bed_dirs[i]);
        Vector3 t = { d.x, cp * d.y + sp * d.z, cp * d.z - sp * d.y };
        Vector3 r = { cy * t.x + sy * t.z, t.y, cy * t.z - sy * t.x };
        Vector3 p = Vector3Add(g_head, Vector3Scale(r, 2.2f));
        DrawSphere(p, 0.15f, cols[i]);
        DrawLine3D(g_head, p, Color{ cols[i].r, cols[i].g, cols[i].b, 160 });
    }
}
/* ============================= Scene 7: Reverb bed (static room) ============================= */
/* The hybrid reverb bed needs reflections configured + the room geometry set BEFORE bwa_start (the
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
    bwa_source_set_gain(e, src, SRC_GAIN);
    bwa_source_set_reflections(e, src, rev_on);    /* feed the source into the shared reverb bed */
    bwa_source_set_reflection_distance(e, src, rev_dist);
    bwa_reflections_set_gain(e, rev_wet);
}
static void rev_update(float dt) {
    if (kp(KEY_B)) {                     /* A/B the bed decoder: load-time, so rebuild the engine */
        rev_decoder ^= 1;
        if (e) { bwa_stop(e); bwa_destroy(e); e = NULL; }
        build_engine(1);
        rev_enter();                               /* re-apply source gain + reflections + wet on the new engine */
    }
    if (kp(KEY_G)) { rev_on = !rev_on; bwa_source_set_reflections(e, src, rev_on); }
    if (kp(KEY_V)) { rev_dist = !rev_dist; bwa_source_set_reflection_distance(e, src, rev_dist); }
    if (kd(KEY_LEFT_BRACKET))  rev_wet = fmaxf(0.0f, rev_wet - 0.7f * dt);
    if (kd(KEY_RIGHT_BRACKET)) rev_wet = fminf(2.0f, rev_wet + 0.7f * dt);
    bwa_reflections_set_gain(e, rev_wet);
    source_pos.x = Clamp(source_pos.x, -ROOM_W * 0.5f + 0.5f, ROOM_W * 0.5f - 0.5f); /* keep it inside the room */
    source_pos.y = Clamp(source_pos.y, 0.5f, ROOM_H - 0.5f);                         /* floor-based box: y 0..H */
    source_pos.z = Clamp(source_pos.z, -ROOM_D * 0.5f + 0.5f, ROOM_D * 0.5f - 0.5f);
    bwa_source_set_pos(e, src, source_pos.x, source_pos.y, source_pos.z);
    bwa_source_set_gain(e, src, SRC_GAIN);
}
static void rev_draw3d(void) {
    DrawCubeWires(Vector3{ 0, ROOM_H * 0.5f, 0 }, ROOM_W, ROOM_H, ROOM_D, Color{ 90, 110, 150, 130 });
    DrawLine3D(g_head, source_pos, Color{ 90, 220, 90, 200 });
    DrawSphere(source_pos, 0.18f, RED);
}
/* ---- scene table (per-scene panel sections live in draw_panel) ---- */
typedef struct {
    const char* name;
    void (*enter)(void);
    void (*update)(float dt);
    void (*draw3d)(void);
} Scene;
static const Scene scenes[] = {
    { "Localization (DBAP)",     loc_enter,  loc_update,  loc_draw3d  },
    { "Occlusion & Materials",   occ_enter,  occ_update,  occ_draw3d  },
    { "Directivity",             dir_enter,  dir_update,  dir_draw3d  },
    { "Channel walk (speaker check)", chan_enter, chan_update, chan_draw3d },
    { "Blind A/B/X (hear it, prove it)", abx_enter, abx_update, abx_draw3d },
    { "Ambisonic bed (world-locked)", bed_enter, bed_update, bed_draw3d },
    { "Reverb bed (static room)", rev_enter,  rev_update,  rev_draw3d  },
};
enum { NSCENE = sizeof scenes / sizeof scenes[0], SCENE_REVERB = NSCENE - 1 };
static int cur_scene;
static int engine_has_reverb;       /* which config the live engine was built in */

/* (re)build the engine in the interactive (no-bed, dynamic geometry) or reverb (bed + static room)
 * config, reloading assets + recreating sources. Used at startup and on each reverb-boundary switch. */
static bwa_sink_type g_sink_mode = BWA_SINK_AUTO;   /* --tests forces BWA_SINK_NULL (hermetic) */
static const char*   g_asio_driver = NULL;          /* --driver <name>; NULL = auto-pick */

static void build_engine(int with_reverb) {
    bwa_desc cfg = {
        .profile = BWA_PROFILE_BINAURAL, .layout_path = g_layout_path, .hrtf_path = NULL,
        .sample_rate = SR, .block_size = 256, .sink = g_sink_mode, .asio_driver = g_asio_driver,
        /* create-time: only the reverb scene cares, but it's harmless for the others */
        .bed_decoder = rev_decoder ? BWA_DECODE_ALLRAD : BWA_DECODE_SAMPLING,
    };
    e = bwa_create(&cfg);
    if (!e) { printf("bwa_create failed\n"); exit(1); }
    if (with_reverb) {
        bwa_reflections_desc rc = { .ir_seconds = 1.0f, .order = 1, .num_rays = 4096,
                                  .num_bounces = 16, .enabled = 1 };
        bwa_reflections_config(e, &rc);
        bwa_material rm = bwa_material_preset(e, BWA_MAT_PLASTER);
        bwa_material faces[6] = { rm, rm, rm, rm, rm, rm };
        bwa_scene_set_box(e, ROOM_W, ROOM_H, ROOM_D, faces);     /* static room, BEFORE bwa_start */
    }
    if (bwa_start(e) != 0) {
        const char* err = bwa_last_error(e);
        printf("bwa_start: %s — no audio (install/select an ASIO driver, e.g. ASIO4ALL); the scene still runs.\n",
               err ? err : "?");
    }
    backend_name   = bwa_get_audio_backend(e);
    backend_silent = (strncmp(backend_name, "asio", 4) != 0);
    g_nspk = (int)bwa_get_speakers(e, (float*)speakers, NSPK);   /* the geometry AND count the engine pans with */
    if (g_nspk < 1) g_nspk = 1;                          /* (a layout always has >= 4; keep the divides safe) */
    if (chan_active >= g_nspk) chan_active = 0;          /* a smaller array may have retired the walked channel */
    g_head = Vector3{ 0, 0, 0 };                         /* ear point = array centroid (the engine's own ref) */
    for (int i = 0; i < g_nspk; ++i) g_head = Vector3Add(g_head, speakers[i]);
    g_head = Vector3Scale(g_head, 1.0f / (float)g_nspk);
    for (int i = 0; i < NSIG; ++i) sounds[i] = bwa_load_sound(e, sig_files[i]);
    g_bed_snd = bwa_load_ambix(e, BED_FILE);             /* the bed scene's field (silent until played) */
    for (int i = 0; i < NMAT; ++i) mats[i] = bwa_material_preset(e, mat_types[i]);
    src  = bwa_source_create(e);  bwa_source_play(e, src,  sounds[cur_sig], true);
    refl = bwa_source_create(e);  bwa_source_play(e, refl, sounds[cur_sig], true);
    g_bed = bwa_bed_create(e);
    bwa_source_set_gain(e, refl, 0.0f);
    if (with_reverb) bwa_source_set_reflections(e, src, rev_on);
    engine_has_reverb = with_reverb;
}

/* leave the current scene at a clean baseline, then enter the new one (rebuilding the engine if we
 * are crossing the reverb boundary, since the bed + room geometry are load-time) */
static void switch_scene(int idx) {
    int want_reverb = (idx == SCENE_REVERB);
    if (want_reverb != engine_has_reverb) {
        printf("rebuilding engine: %s\n", want_reverb ? "reverb (bed + static room)" : "interactive");
        if (e) { bwa_stop(e); bwa_destroy(e); e = NULL; }
        build_engine(want_reverb);
    }
    for (uint32_t ch = 0; ch < (uint32_t)g_nspk; ++ch) bwa_set_test_signal(e, ch, BWA_TEST_OFF, 0.0f);  /* clear channel walk */
    bwa_source_set_gain(e, refl, 0.0f);
    bwa_source_set_occlusion(e, src, false);
    bwa_source_set_directivity_preset(e, src, BWA_DIR_OMNI);
    bwa_source_set_orientation(e, src, 0.0f, 0.0f, 0.0f, 1.0f);   /* clear any aim left by the directivity scene */
    bwa_source_set_doppler(e, src, false);                        /* propagation/size effects are localization-scene only */
    bwa_source_set_air_absorption(e, src, false);
    bwa_source_set_spread(e, src, 0.0f);
    bwa_set_dual_band(e, false);
    bwa_set_panner(e, BWA_PAN_DBAP);                               /* the ABX scene may leave SPCAP/VBAP selected */
    bwa_set_spread_mode(e, BWA_SPREAD_LOBE);                       /* ...or a spread render / decorrelation */
    bwa_set_decorrelation(e, false);
    bwa_bed_stop(e, g_bed);                                        /* the bed scene's field + its knobs */
    bwa_set_bed_renderer(e, BWA_BED_MATRIX);
    bwa_set_max_re(e, false);
    source_yaw = 0.0f;
    bwa_source_set_gain(e, src, SRC_GAIN);
    cur_scene = idx;
    scenes[idx].enter();
}

/* ============================== imgui control panel ============================== */

#define PANEL_W 330.0f            /* control panel width (right side), in unscaled UI px */
static bool show_te_ui = false;   /* imgui_test_engine windows — run the --tests suite interactively */

/* checkbox over an int flag; returns true when toggled (the caller applies the engine side effect) */
static bool chk(const char* label, int* v) {
    bool b = *v != 0;
    if (!ImGui::Checkbox(label, &b)) return false;
    *v = b ? 1 : 0;
    return true;
}

static void draw_panel(void) {
    const float W = uiScaled(PANEL_W);
    ImGui::SetNextWindowPos(ImVec2((float)GetScreenWidth() - W - uiScaled(8.0f), uiScaled(8.0f)), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(W, (float)GetScreenHeight() - uiScaled(16.0f)), ImGuiCond_Always);
    if (!ImGui::Begin("playground", NULL, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End(); return;
    }

    /* scene selector (TAB cycles too; crossing the reverb boundary rebuilds the engine) */
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##scene", scenes[cur_scene].name)) {
        for (int i = 0; i < NSCENE; ++i)
            if (ImGui::Selectable(scenes[i].name, i == cur_scene) && i != cur_scene) switch_scene(i);
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("TAB scene  WASD/RF source  Q/E head  F11  ESC");

    /* audio status + live output meters (bwa_get_bus_levels -> spk_lv, the same data shading the 3D gizmos) */
    ImGui::SeparatorText("output");
    if (backend_silent) ImGui::TextColored(ImVec4(1.00f, 0.45f, 0.45f, 1.0f), "audio: %s - NO SOUND", backend_name);
    else                ImGui::TextColored(ImVec4(0.45f, 0.92f, 0.55f, 1.0f), "audio: %s", backend_name);
    if (backend_silent && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("no ASIO device opened - the offline null sink renders silently\n"
                          "(run with --driver <your headphone driver>)");
    char mlabel[48];
    snprintf(mlabel, sizeof mlabel, "speakers 0-%d (60 dB window)", g_nspk - 1);
    ImGui::PlotHistogram("##meters", spk_lv, g_nspk, 0, mlabel, 0.0f, 1.0f,
                         ImVec2(-FLT_MIN, uiScaled(46.0f)));

    /* the localization signal is global (the 1-4 keys work in every scene) */
    ImGui::SeparatorText("signal [1-4]");
    for (int i = 0; i < NSIG; ++i) {
        if (ImGui::RadioButton(SIG_NAMES[i], cur_sig == i) && cur_sig != i) {
            cur_sig = i;
            bwa_source_play(e, src,  sounds[i], true);
            bwa_source_play(e, refl, sounds[i], true);
        }
    }

    ImGui::SeparatorText(scenes[cur_scene].name);
    if (cur_scene == 0) {                                 /* Localization */
        if (chk("auto-move [SPACE]", &loc_auto) && loc_auto) { loc_flyby = 0; loc_t = 0.0f; loc_trail_len = 0; }
        bwTip("orbit the source around the head - listen for smooth motion, no zipper");
        if (chk("fast flyby [X]", &loc_flyby) && loc_flyby) { loc_auto = 0; loc_fly_t = 0.0f; loc_trail_len = 0; }
        bwTip("a fast close pass - the speed Doppler needs to be obvious");
        if (chk("Doppler [V]", &loc_dop))            bwa_source_set_doppler(e, src, loc_dop);
        bwTip("renders the true propagation delay: pitch up approaching, down receding "
              "(subtle at orbit speed - pair it with fast flyby)");
        if (chk("air absorption [B]", &loc_air))     bwa_source_set_air_absorption(e, src, loc_air);
        bwTip("distance-driven high-frequency roll-off: far sources sound duller");
        if (chk("dual-band panning [M]", &loc_dual)) bwa_set_dual_band(e, loc_dual);
        bwTip("below ~700 Hz pans amplitude-normalised - sharper bass localisation "
              "near the sweet spot; toggle it live and compare");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderFloat("##spread", &loc_spread, 0.0f, 1.0f, "size %.2f [C]"))
            bwa_source_set_spread(e, src, loc_spread);
        bwTip("angular width: 0 = point source, 1 = wide - a crowd or waterfall "
              "that shouldn't collapse to a point");
        ImGui::TextDisabled("source (%.2f, %.2f, %.2f)  head %.0f deg",
                            source_pos.x, source_pos.y, source_pos.z, head_yaw * 57.2958f);
        ImGui::TextWrapped("broadband + sharp onsets localise best");
    } else if (cur_scene == 1) {                          /* Occlusion & Materials */
        ImGui::SetNextItemWidth(-FLT_MIN);
        int m = cur_mat;
        if (ImGui::Combo("##mat", &m, mat_names, NMAT) && m != cur_mat) { cur_mat = m; push_wall_mesh(mats[cur_mat]); }
        bwTip("wall material: each has its own per-band transmission (how muffled "
              "the occluded sound gets) and reflectivity");
        chk("audible reflection [T]", &refl_audible);
        bwTip("the wall throws an image-source reflection when the source is in front of it");
        if (chk("occlusion [G]", &occ_audible)) bwa_source_set_occlusion(e, src, occ_audible);
        bwTip("ray-traced: the wall between source and listener attenuates AND muffles "
              "it (per-band transmission EQ), ramped - not a hard mute");
        ImGui::TextDisabled("[ ] keys slide the wall");
        if (occ_refl_valid)    ImGui::TextColored(ImVec4(1.00f, 0.70f, 0.30f, 1.0f), "in FRONT: REFLECTING (image source)");
        else if (occ_occluded) ImGui::TextColored(ImVec4(0.96f, 0.55f, 0.55f, 1.0f), "BEHIND: OCCLUDED (material tilt)");
        else                   ImGui::TextUnformatted("wall: clear line of sight");
        ImGui::Text("occlusion %.0f%% audible   material refl %.0f%%", occ_factor * 100.0f, mat_refl[cur_mat] * 100.0f);
    } else if (cur_scene == 2) {                          /* Directivity */
        ImGui::SetNextItemWidth(-FLT_MIN);
        int d = cur_dir;
        if (ImGui::Combo("##dir", &d, dir_names, 3) && d != cur_dir) {
            cur_dir = d;
            bwa_source_set_directivity_preset(e, src, (bwa_directivity)cur_dir);
        }
        bwTip("radiation pattern: omni (no directivity), cardioid (one-sided), "
              "figure-8 (dipole with a null at 90 degrees)");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderAngle("##aim", &source_yaw, -180.0f, 180.0f, "aim %.0f deg [,/.]");
        ImGui::Text("%.0f%% on-axis at the listener", dir_gain * 100.0f);
        ImGui::TextWrapped("aim the lobe away from the head and hear it drop");
    } else if (cur_scene == 3) {                          /* Channel walk */
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderInt("##chan", &chan_active, 0, g_nspk - 1, "channel %d [LEFT/RIGHT]");
        if (ImGui::RadioButton("660 Hz sine [N]", chan_kind == BWA_TEST_SINE)) chan_kind = BWA_TEST_SINE;
        ImGui::SameLine();
        if (ImGui::RadioButton("noise", chan_kind == BWA_TEST_NOISE)) chan_kind = BWA_TEST_NOISE;
        chk("auto-walk [SPACE]", &chan_auto);
        Vector3 p = speakers[chan_active];
        ImGui::TextDisabled("speaker (%.2f, %.2f, %.2f)", p.x, p.y, p.z);
        ImGui::TextWrapped("binaural: each channel is HRTF'd as its virtual speaker");
    } else if (cur_scene == 4) {                          /* Blind A/B/X */
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##cmp", abx_cmps[abx_cmp].name)) {   /* names contain '/' — keep them out of test paths */
            for (int i = 0; i < (int)NABX; ++i)
                if (ImGui::Selectable(abx_cmps[i].name, i == abx_cmp) && i != abx_cmp) { abx_cmp = i; abx_reset(); }
            ImGui::EndCombo();
        }
        bwTip("one engine knob differs between A and B; X is secretly one of them. "
              "Listen to all three, answer, repeat - the p-value says whether the "
              "difference is genuinely audible, not just 'sounds different to me'");
        ImGui::TextDisabled("A = %s\nB = %s", abx_cmps[abx_cmp].a, abx_cmps[abx_cmp].b);
        if (ImGui::RadioButton("A [Z]", abx_listen == 0)) abx_set_listen(0);
        ImGui::SameLine();
        if (ImGui::RadioButton("B [X]", abx_listen == 1)) abx_set_listen(1);
        ImGui::SameLine();
        if (ImGui::RadioButton("X [C]", abx_listen == 2)) abx_set_listen(2);
        if (ImGui::Button("X is A [LEFT]"))  abx_answer(0);
        ImGui::SameLine();
        if (ImGui::Button("X is B [RIGHT]")) abx_answer(1);
        if (ImGui::Button("reset tally [V]")) abx_reset();
        chk("orbit the source [SPACE]", &abx_orbit);
        if (abx_trials > 0) {
            double p = abx_pvalue(abx_trials, abx_correct);
            const char* verdict = (abx_trials < 6) ? "keep going (need ~6+ trials)"
                                : (p < 0.05)       ? "DISTINGUISHABLE - you can hear it"
                                                   : "not distinguishable yet (guessing?)";
            ImGui::Text("score %d/%d   p = %.3f", abx_correct, abx_trials, p);
            ImGui::TextWrapped("%s", verdict);
            if (abx_flash_t > 0.0f)
                ImGui::TextColored(abx_flash_ok ? ImVec4(0.50f, 0.92f, 0.55f, 1.0f) : ImVec4(0.96f, 0.55f, 0.55f, 1.0f),
                                   "last: %s (X was %s)", abx_flash_ok ? "CORRECT" : "wrong", abx_last_x ? "B" : "A");
        } else {
            ImGui::TextWrapped("listen to A, B, X (switching is seamless - that's the point), then answer.");
        }
    } else if (cur_scene == 5) {                          /* Ambisonic bed */
        chk("spin the field [SPACE]", &bed_spin);
        bwTip("accumulating yaw through bwa_bed_set_orientation - the engine glides at "
              "~1 turn/s, so every move is click-free");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderAngle("##tilt", &bed_pitch, -90.0f, 90.0f, "tilt (pitch) %.0f deg");
        bwTip("the full 3-axis orientation: positive pitch tilts the field's front upward - "
              "watch the red (front) marker climb toward the ceiling and follow it by ear");
        chk("parametric renderer [G]", &bed_param);
        bwTip("matrix decode vs DirAC-style parametric (direction + diffuseness per band, the "
              "direct stream re-panned listener-relative - a walkable bed). Live crossfade");
        chk("max-rE decode [B]", &bed_re);
        bwTip("max-rE weighting tapers the high SH orders: fewer decode sidelobes, a longer "
              "energy vector - better localization off-centre, slightly wider main lobe. Live A/B");
        ImGui::TextDisabled("yaw %.0f deg", fmodf(bed_yaw * 57.2958f, 360.0f));
        ImGui::TextWrapped("bursts = front (red), clicks = left-up (blue), plus a diffuse floor. "
                           "World-locked: the field is glued to the room, not the head.");
    } else {                                              /* Reverb bed */
        if (chk("reverb send [G]", &rev_on)) bwa_source_set_reflections(e, src, rev_on);
        bwTip("opt the source into the shared reverb bed's wet send");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##wet", &rev_wet, 0.0f, 2.0f, "wet %.2f  [ ] keys");   /* applied per frame in rev_update */
        bwTip("live wet level of the whole bed (bwa_reflections_set_gain)");
        if (chk("distance->wet [V]", &rev_dist)) bwa_source_set_reflection_distance(e, src, rev_dist);
        bwTip("near = drier, far = wetter - walk the source away (WASD) and hear "
              "the room take over");
        ImGui::SetNextItemWidth(-FLT_MIN);
        int dec = rev_decoder;
        const char* dec_names[2] = { "bed decoder: sampling (SAD)", "bed decoder: AllRAD" };
        if (ImGui::Combo("##dec", &dec, dec_names, 2) && dec != rev_decoder) {   /* load-time: rebuild the engine */
            rev_decoder = dec;
            if (e) { bwa_stop(e); bwa_destroy(e); e = NULL; }
            build_engine(1);
            rev_enter();
        }
        bwTip("diffuse-bed decoder. Load-time, so switching REBUILDS the engine - "
              "expect a brief pause; audio and playhead restart");
        ImGui::TextWrapped("8x4x8 m plaster room; clicks/bursts show the tail. SAD vs AllRAD differ most on an irregular layout.");
    }

    ImGui::Separator();
    ImGui::Checkbox("test engine", &show_te_ui);
    ImGui::End();
}

/* ============================== --tests harness (imgui_test_engine) ============================== */

static ImGuiTestEngine* g_te;

/* test-engine screenshot hook — identical to layout_tool's: read GL_BACK via raylib from PostSwap
 * (called BEFORE EndDrawing's buffer swap, after flushing the rlgl batch). */
static bool screen_capture(ImGuiID viewport_id, int x, int y, int w, int h, unsigned int* pixels, void* user) {
    (void)viewport_id; (void)user;
    Image img = LoadImageFromScreen();
    if (!img.data || img.format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8) { UnloadImage(img); return false; }
    const unsigned int* srcpx = (const unsigned int*)img.data;
    for (int row = 0; row < h; ++row)
        for (int c = 0; c < w; ++c) {
            int sx = x + c, sy = y + row;
            unsigned int v = (sx >= 0 && sx < img.width && sy >= 0 && sy < img.height)
                           ? srcpx[(size_t)sy * img.width + sx] : 0;
            pixels[(size_t)row * w + c] = v | 0xFF000000u;
        }
    UnloadImage(img);
    return true;
}

/* engine-liveness helper for the tests: current max output-channel peak (0 while dead or silent) */
static float meters_max(uint32_t* count_out) {
    float pk[NSPK] = { 0 };
    uint32_t n = bwa_get_bus_levels(e, pk, NSPK);
    if (count_out) *count_out = n;
    float m = 0.0f;
    for (uint32_t i = 0; i < n; ++i) if (pk[i] > m) m = pk[i];
    return m;
}

static void register_tests(ImGuiTestEngine* te) {
    ImGuiTest* t;

    /* pure-logic checks ride the same suite (the station's pattern) — filterable via --tests logic */
    t = IM_REGISTER_TEST(te, "logic", "abx_pvalue");
    t->TestFunc = [](ImGuiTestContext*) {
        IM_CHECK_LT(fabs(abx_pvalue(6, 6) - 1.0 / 64.0), 1e-9);    /* 6/6 = (1/2)^6 */
        IM_CHECK_GT(abx_pvalue(6, 3), 0.5);                        /* chance level: not significant */
        IM_CHECK_LT(abx_pvalue(10, 9), 0.05);                      /* 9/10 clearly is */
        IM_CHECK_LT(fabs(abx_pvalue(1, 0) - 1.0), 1e-9);           /* >= 0 correct is certain */
    };

    t = IM_REGISTER_TEST(te, "logic", "signals");
    t->TestFunc = [](ImGuiTestContext*) {
        static float buf[SR];                       /* 1 s is plenty for these checks */
        for (int w = 0; w < NSIG; ++w) {
            gen_signal(w, buf, SR);
            float peak = 0, sum = 0;
            for (uint32_t i = 0; i < SR; ++i) { float a = fabsf(buf[i]); if (a > peak) peak = a; sum += a; }
            IM_CHECK_GT(peak, 0.05f);               /* every signal produces output */
            IM_CHECK_LE(peak, 1.0f);                /* and stays in range */
            if (w == 2) IM_CHECK_LT(sum / SR, 0.05f);   /* the click train is mostly silence */
        }
    };

    /* THE regression this harness exists to pin: with no ASIO device (the suite forces
     * BWA_SINK_NULL) the engine must still be LIVE — null-sink fallback rendering in real
     * time, output meters flowing. A dead engine here once shipped as "visual-only mode". */
    t = IM_REGISTER_TEST(te, "viewer", "meters_live");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        /* fallback engaged; NOT "none" (dead engine). Prefix match: binaural appends the live
         * monitor kind — "null (steam HRTF monitor)" / "null (simple-pan monitor)". */
        IM_CHECK(strncmp(backend_name, "null", 4) == 0);
        uint32_t n = 0;
        float m = 0.0f;
        for (int tries = 0; tries < 60 && m <= 1e-6f; ++tries) { ctx->Yield(4); m = meters_max(&n); }
        IM_CHECK_EQ(n, (uint32_t)g_nspk);           /* the engine's channel count == the layout's */
        IM_CHECK_GT(m, 1e-4f);                      /* pink noise through DBAP reaches the output bus */
    };

    t = IM_REGISTER_TEST(te, "viewer", "panel_controls");   /* fake inputs drive the real panel */
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("playground");
        ctx->ItemClick("**/pink bursts");
        IM_CHECK_EQ(cur_sig, 1);
        ctx->ItemCheck("**/auto-move [SPACE]");
        IM_CHECK_EQ(loc_auto, 1);
        ctx->Yield(8);                              /* the orbit moves the source */
        ctx->CaptureScreenshot();
        ctx->ItemUncheck("**/auto-move [SPACE]");
        IM_CHECK_EQ(loc_auto, 0);
        ctx->ItemClick("**/pink noise");
        IM_CHECK_EQ(cur_sig, 0);
    };

    t = IM_REGISTER_TEST(te, "viewer", "tooltips");              /* the hover help actually shows */
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("playground");
        ctx->MouseMove("**/dual-band panning [M]");
        double t0 = ImGui::GetTime();
        while (ImGui::GetTime() - t0 < 1.2) ctx->Yield();        /* the ForTooltip delay is wall-clock */
        ImGuiWindow* tip = ImGui::FindWindowByName("##Tooltip_00");
        IM_CHECK(tip != NULL && tip->WasActive);
        ctx->CaptureScreenshot();
    };

    /* every scene enters cleanly; the reverb boundary rebuilds the engine BOTH ways and it stays
     * live. (switch_scene is called directly: the keyboard path polls raylib, which the test
     * engine's synthetic input can't reach — the panel path is covered by panel_controls.) */
    t = IM_REGISTER_TEST(te, "viewer", "scene_cycle");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        for (int i = 1; i <= (int)NSCENE; ++i) {
            int idx = i % NSCENE;
            switch_scene(idx);
            IM_CHECK_EQ(cur_scene, idx);
            ctx->Yield(6);
            uint32_t n = 0;
            meters_max(&n);
            IM_CHECK_EQ(n, (uint32_t)g_nspk);       /* engine alive after every switch (incl. rebuilds) */
        }
    };

    t = IM_REGISTER_TEST(te, "viewer", "bed_scene");     /* the ambisonic bed plays + its live A/Bs stay live */
    t->TestFunc = [](ImGuiTestContext* ctx) {
        switch_scene(5);
        int playing = 0;                                 /* is_playing publishes per AUDIO block; UI
                                                          * frames outpace it — poll, don't race */
        for (int tries = 0; tries < 60 && !playing; ++tries) { ctx->Yield(4); playing = bwa_bed_is_playing(e, g_bed); }
        IM_CHECK(playing);
        uint32_t n = 0;
        float m = 0.0f;
        for (int tries = 0; tries < 60 && m <= 1e-6f; ++tries) { ctx->Yield(4); m = meters_max(&n); }
        IM_CHECK_GT(m, 1e-4f);                           /* the field reaches the bus through the decode */
        ctx->SetRef("playground");
        ctx->ItemCheck("**/max-rE decode [B]");
        IM_CHECK_EQ(bed_re, 1);
        ctx->ItemCheck("**/parametric renderer [G]");
        IM_CHECK_EQ(bed_param, 1);
        ctx->Yield(10);                                  /* both A/Bs crossfade in */
        m = meters_max(&n);
        IM_CHECK_GT(m, 1e-4f);                           /* still live through both live A/Bs */
        ctx->CaptureReset();
        ctx->CaptureScreenshot();
        ctx->ItemUncheck("**/parametric renderer [G]");
        ctx->ItemUncheck("**/max-rE decode [B]");
        switch_scene(0);
        int stopped = 0;                                 /* the stop is click-free: it needs an audio
                                                          * block to ramp out + publish — poll, don't race */
        for (int tries = 0; tries < 60 && !stopped; ++tries) { ctx->Yield(4); stopped = !bwa_bed_is_playing(e, g_bed); }
        IM_CHECK(stopped);                               /* leaving the scene stops the bed */
    };

    t = IM_REGISTER_TEST(te, "viewer", "abx_flow");  /* the blind test scores through the panel buttons */
    t->TestFunc = [](ImGuiTestContext* ctx) {
        switch_scene(4);
        ctx->Yield(2);
        ctx->SetRef("playground");
        ctx->ItemClick("**/A [Z]");
        IM_CHECK_EQ(abx_listen, 0);
        ctx->ItemClick("**/X [C]");
        IM_CHECK_EQ(abx_listen, 2);
        int t0 = abx_trials;
        ctx->ItemClick("**/X is A [LEFT]");
        ctx->ItemClick("**/X is B [RIGHT]");
        IM_CHECK_EQ(abx_trials, t0 + 2);
        double p = abx_pvalue(abx_trials, abx_correct);
        IM_CHECK(p >= 0.0 && p <= 1.0);
        ctx->CaptureReset();                        /* else this shot would reuse the previous filename */
        ctx->CaptureScreenshot();
        ctx->ItemClick("**/reset tally [V]");
        IM_CHECK_EQ(abx_trials, 0);
        switch_scene(0);
    };
}

/* ============================== main ============================== */

int main(int argc, char** argv) {
    /* --tests [filter]: run the imgui_test_engine suite (logic + the real panel) and exit pass/fail.
     * The suite forces the offline null sink: deterministic on any machine (no audio device needed),
     * and it PINS the no-device fallback path — see the meters_live test. */
    bool selftest = false;
    char filter[64] = "";
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("usage: bwa_playground [cave_layout.json] [--driver <asio driver>] [--tests [filter]]\n"
                   "  audition the engine by ear on the binaural monitor (auto-picked 2-ch ASIO\n"
                   "  driver; with no device it renders silently -- visual-only mode stays live)\n"
                   "  cave_layout.json   optional surveyed layout (default: ./cave_layout.json if\n"
                   "                     present, else the built-in grid); ./constraints.json is\n"
                   "                     drawn for orientation if present\n"
                   "  --driver <name>    ASIO driver to open (default: auto-pick a 2-ch one)\n"
                   "  --tests [filter]   run the UI test suite (offline) and exit pass/fail\n"
                   "  keys: TAB scene | WASD/RF source | Q/E head | 1-4 signal | SPACE auto-move | F11\n");
            return 0;
        }
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--tests") || !strcmp(argv[i], "--selftest")) {
            selftest = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') snprintf(filter, sizeof filter, "%s", argv[i + 1]);
        } else if (!strcmp(argv[i], "--driver") && i + 1 < argc) {
            g_asio_driver = argv[++i];                        /* pin the headphone driver (bwa_desc.asio_driver) */
        }
    if (selftest) g_sink_mode = BWA_SINK_NULL;

    /* Sink policy (interactive): engine default (AUTO) — try a 2-ch ASIO driver for headphones, fall
     * back to the offline null sink. Do NOT demand BWA_SINK_ASIO: the fallback is what keeps
     * visual-only mode live (no device -> the engine still renders in real time, so speaker
     * activity, panning and occlusion still animate, just silent). The panel's audio line +
     * meters keep the no-audio state visible. */

    /* optional surveyed layout: argv[1], else ./cave_layout.json if present, else the default grid.
     * selftest always uses the default grid — the suite must not depend on a machine-local file. */
    g_layout_path = (!selftest && argc > 1 && argv[1][0] != '-') ? argv[1] : NULL;
    if (!selftest && !g_layout_path) {
        FILE* lf = fopen("cave_layout.json", "rb");
        if (lf) { fclose(lf); g_layout_path = "cave_layout.json"; }
    }

    /* synthesise the localization test signals to wav (the engine loads sounds from file) */
    float* sigbuf = (float*)malloc((size_t)SIGLEN * sizeof(float));
    if (!sigbuf) { printf("out of memory\n"); return 1; }
    for (int i = 0; i < NSIG; ++i) { gen_signal(i, sigbuf, SIGLEN); write_wav(sig_files[i], sigbuf, SIGLEN); }
    free(sigbuf);
    /* ...and the ambisonic-bed field (16-ch AmbiX; the bed scene plays it) */
    float* bedbuf = (float*)malloc((size_t)SIGLEN * 16 * sizeof(float));
    if (!bedbuf) { printf("out of memory\n"); return 1; }
    gen_bed(bedbuf, SIGLEN);
    write_wav16(BED_FILE, bedbuf, SIGLEN);
    free(bedbuf);

    wall_basis(wall_n, &wall_u, &wall_v);
    build_engine(0);                                          /* start in the interactive config (fills speakers[], g_head) */
    source_pos.y = g_head.y;                                  /* start the source on the ear plane */
    printf("layout: %s    audio backend: %s%s\n", g_layout_path ? g_layout_path : "default grid", backend_name,
           backend_silent ? "   (SILENT — run with --driver <your headphone driver>)" : "");
    if (cv_load("constraints.json", &g_con))                  /* orientation only; the layout tool edits against these */
        printf("constraints: bounds + %d no-go + %d obstacle box(es) drawn from ./constraints.json\n", g_con.nnogo, g_con.nobst);

    /* No FLAG_WINDOW_HIGHDPI: framebuffer == window pixels keeps rlImGui's scale at 1 so the test
     * engine's screenshots/scissors stay 1:1; DPI rides the theme's FontScaleMain (layout_tool's pattern). */
    SetConfigFlags(FLAG_MSAA_4X_HINT | (selftest ? FLAG_WINDOW_UNFOCUSED : 0));
    InitWindow(1280, 800, "bw_audio - binaural playground");
    SetExitKey(KEY_NULL);                                     /* ESC handled below (must not quit while typing) */
    SetRandomSeed(selftest ? 42u : (unsigned int)time(NULL)); /* ABX X-draws: deterministic only under test */
    SetTargetFPS(selftest ? 0 : 60);                          /* selftest: run the suite unthrottled */
    g_uiScale = GetWindowScaleDPI().y;
    if (g_uiScale > 1.01f) SetWindowSize((int)uiScaled(1280), (int)uiScaled(800));

    rlImGuiBeginInitImGui();                                  /* split init: our Roboto must be the default font */
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL;                                    /* fixed layout; don't scatter imgui.ini */
    loadEmbeddedFont(io);
    applyTheme(false);                                        /* the station theme (dark) */
    rlImGuiEndInitImGui();

    g_te = ImGuiTestEngine_CreateContext();
    {
        ImGuiTestEngineIO& teio = ImGuiTestEngine_GetIO(g_te);
        teio.ConfigVerboseLevel        = ImGuiTestVerboseLevel_Warning;
        teio.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Debug;
        teio.ConfigLogToTTY            = selftest;            /* ctest: name each test + why it failed */
        teio.ConfigCaptureEnabled      = true;                /* actually write screenshots (output/captures/) */
        teio.ConfigRunSpeed            = selftest ? ImGuiTestRunSpeed_Fast : ImGuiTestRunSpeed_Normal;
        teio.ScreenCaptureFunc         = screen_capture;
    }
    ImGuiTestEngine_Start(g_te, ImGui::GetCurrentContext());
    ImGuiTestEngine_InstallDefaultCrashHandler();
    register_tests(g_te);
    if (selftest) ImGuiTestEngine_QueueTests(g_te, ImGuiTestGroup_Tests, filter[0] ? filter : NULL,
                                             ImGuiTestRunFlags_RunFromCommandLine);

    cam = Camera3D{};
    cam.target = Vector3{ 0, g_head.y, 0 };
    cam.up = Vector3{ 0, 1, 0 };
    cam.fovy = 55;
    cam.projection = CAMERA_PERSPECTIVE;
    switch_scene(0);

    int quit_now = 0, frames = 0, drain = 0;
    while (!quit_now) {
        float dt = GetFrameTime();
        g_kb = !io.WantCaptureKeyboard;             /* imgui typing must not trigger scene shortcuts */
        g_ms = !io.WantCaptureMouse;                /* the panel owns the mouse when hovered */
        if (WindowShouldClose() || kp(KEY_ESCAPE)) quit_now = 1;
        if (kp(KEY_F11)) ToggleBorderlessWindowed();
        float mv = 2.5f * dt, rt = 1.8f * dt;

        /* ---- global navigation (every scene) ---- */
        if (kd(KEY_W)) source_pos.z += mv;
        if (kd(KEY_S)) source_pos.z -= mv;
        if (kd(KEY_A)) source_pos.x += mv;
        if (kd(KEY_D)) source_pos.x -= mv;
        if (kd(KEY_R)) source_pos.y += mv;
        if (kd(KEY_F)) source_pos.y -= mv;
        if (kd(KEY_Q)) head_yaw += rt;
        if (kd(KEY_E)) head_yaw -= rt;
        for (int i = 0; i < NSIG; ++i)              /* 1-4: switch the test signal everywhere */
            if (kp(KEY_ONE + i)) {
                cur_sig = i;
                bwa_source_play(e, src,  sounds[i], true);
                bwa_source_play(e, refl, sounds[i], true);
            }
        /* TAB last in this block ON PURPOSE: a reverb-boundary switch REBUILDS the engine (destroys e,
         * src, refl), so all per-source calls above must run against the still-valid engine first. */
        if (kp(KEY_TAB)) switch_scene((cur_scene + 1) % NSCENE);

        /* arcball camera: right-drag orbits, the wheel zooms — only when imgui doesn't own the mouse */
        if (g_ms && IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
            Vector2 md = GetMouseDelta();
            cam_yaw   -= md.x * 0.005f;
            cam_pitch += md.y * 0.005f;
            if (cam_pitch >  1.5f) cam_pitch =  1.5f;
            if (cam_pitch < -1.5f) cam_pitch = -1.5f;
        }
        if (g_ms) cam_dist -= GetMouseWheelMove() * 0.6f;
        if (cam_dist < 1.5f)  cam_dist = 1.5f;
        if (cam_dist > 25.0f) cam_dist = 25.0f;
        cam.position.x = cam.target.x + cam_dist * cosf(cam_pitch) * sinf(cam_yaw);
        cam.position.y = cam.target.y + cam_dist * sinf(cam_pitch);
        cam.position.z = cam.target.z + cam_dist * cosf(cam_pitch) * cosf(cam_yaw);

        /* ---- per-scene update, then publish the frame ---- */
        highlight_spk = -1;
        scenes[cur_scene].update(dt);
        Quaternion q = QuaternionFromAxisAngle(Vector3{ 0, 1, 0 }, head_yaw);
        bwa_set_listener_pose(e, g_head.x, g_head.y, g_head.z, q.x, q.y, q.z, q.w);
        bwa_commit(e);

        BeginDrawing();
        ClearBackground(Color{ 24, 24, 27, 255 });  /* matches the theme's WindowBg */
        BeginMode3D(cam);
        DrawGrid(12, 0.5f);
        draw_speakers(highlight_spk);
        scenes[cur_scene].draw3d();
        draw_head(q);
        EndMode3D();
        draw_axes_hud(cam, uiScaled(56.0f), (float)GetScreenHeight() - uiScaled(56.0f), uiScaled(30.0f));

        rlImGuiBegin();
        draw_panel();
        if (show_te_ui && g_te) ImGuiTestEngine_ShowTestEngineWindows(g_te, &show_te_ui);
        rlImGuiEnd();

        rlDrawRenderBatchActive();                  /* flush rlgl so a capture sees the whole frame */
        ImGuiTestEngine_PostSwap(g_te);             /* BEFORE the swap: GL_BACK still holds this frame */
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
    rlImGuiShutdown();                              /* destroys the imgui context */
    ImGuiTestEngine_DestroyContext(g_te);           /* after DestroyContext, per the te docs */
    CloseWindow();
    if (e) { bwa_stop(e); bwa_destroy(e); }
    for (int i = 0; i < NSIG; ++i) remove(sig_files[i]);
    remove(BED_FILE);
    return rc;
}

