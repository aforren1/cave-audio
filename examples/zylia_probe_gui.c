/*
 * zylia_probe_gui.c — live DOA view for zylia_probe (see zylia_probe_gui.h for the seam).
 *
 * The console meter answers "does the ZM-1 stream?". This answers the next question the moment the
 * mic is plugged in: "is the capsule MAPPING + GEOMETRY right?" — clap anywhere around the array and
 * a dot appears on the capsule sphere where the clap came from (zylia_tdoa's arrival differences ->
 * zylia_doa's direction, the same math the speaker survey uses). If channels are swapped or the
 * geometry table is wrong, the dot lands somewhere absurd — you find out in seconds, not during a
 * calibration session. --simulate drives the identical pipeline with synthesized claps from a
 * walking direction (drawn as a ring the recovered dot must land in): the off-hardware check of
 * everything but the ASIO capture itself.
 *
 * Left column: the 19 per-capsule meters (the console view, kept). Right: the capsule sphere,
 * arrival dots fading with age, and the latest direction as an arrow. Right-drag orbits, wheel zooms.
 */
#include "zylia_probe_gui.h"
#include "raylib.h"
#include "ui_text.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define ZP_HIST 12                   /* recent arrival directions kept fading on the sphere */

typedef struct { float dir[3]; float age; int valid; } DoaHit;

static float zp_azimuth  (const float d[3]) { return atan2f(d[0], -d[2]) * 57.29578f; }  /* deg from -Z toward +X */
static float zp_elevation(const float d[3]) { return asinf (d[1] > 1.f ? 1.f : (d[1] < -1.f ? -1.f : d[1])) * 57.29578f; }

/* ---- simulate: a Gaussian clap sampled at each capsule's exact fractional arrival time (the same
 * synthesis the zylia unit test validates against), from a direction that walks the sphere. ---- */
static void zp_sim_clap(ZpShared* sh, const float dir[3]) {
    float dirs[ZYLIA_MICS][3]; float R; zylia_geometry(dirs, &R);
    const double C = 343.0, FS = sh->rate, SIGMA = 1.0e-4, DIST = 2.0;
    unsigned int rng = (unsigned int)(sh->seq * 2654435761u + 12345u);
    double src[3] = { dir[0] * DIST, dir[1] * DIST, dir[2] * DIST };
    for (int ch = 0; ch < ZYLIA_MICS; ++ch) {
        double mx = R * dirs[ch][0] - src[0], my = R * dirs[ch][1] - src[1], mz = R * dirs[ch][2] - src[2];
        double t0 = 0.020 + sqrt(mx * mx + my * my + mz * mz) / C;
        for (int i = 0; i < ZP_SNAP_N; ++i) {
            double td = (double)i / FS - t0;
            double s  = exp(-0.5 * (td / SIGMA) * (td / SIGMA));
            rng = rng * 1664525u + 1013904223u;
            double nz = ((double)(int)(rng >> 9) / (double)(1 << 22) - 1.0) * 1e-3;
            sh->snap[ch][i] = (float)(0.7 * s + nz);
        }
        sh->rms[ch] = 0.25f;                                   /* kick the meters; the loop decays them */
    }
    sh->blocks++; sh->seq++;                                   /* same publish the ASIO side does */
}

void zylia_gui_run(ZpShared* sh, int sim) {
    if (sim) { sh->nch = ZYLIA_MICS; if (sh->rate <= 0) sh->rate = 48000.0; sh->title = "simulate"; }

    float dirs[ZYLIA_MICS][3]; float R; zylia_geometry(dirs, &R);

    SetConfigFlags(FLAG_WINDOW_HIGHDPI | FLAG_MSAA_4X_HINT);
    InitWindow(1100, 720, "bwaudio - zylia DOA probe");
    ui_text_init();
    SetTargetFPS(60);

    Camera3D cam = { .target = { 0, 0, 0 }, .up = { 0, 1, 0 }, .fovy = 45, .projection = CAMERA_PERSPECTIVE };
    float cyaw = 0.7f, cpitch = 0.35f, cdist = 3.6f;

    DoaHit hist[ZP_HIST] = { 0 };
    int    hist_n = 0;
    long   last_seq = sh->seq;
    int    snaps = 0, rejects = 0;
    float  sim_t = 1.0f, sim_az = 0.6f;
    float  sim_true[3] = { 0, 0, -1 };                        /* ground-truth direction of the last sim clap */
    static float snap[ZYLIA_MICS][ZP_SNAP_N];                 /* local copy (static: 300 KB off the stack) */

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        if (IsKeyPressed(KEY_F11)) ToggleBorderlessWindowed();

        if (sim) {                                            /* walk the direction, clap every ~1.5 s */
            sim_t += dt;
            if (sim_t >= 1.5f) {
                sim_t = 0.0f; sim_az += 0.44f;                /* ~25 deg steps; elevation sweeps too */
                float el = 0.45f * sinf(0.8f * sim_az);
                sim_true[0] = cosf(el) * sinf(sim_az); sim_true[1] = sinf(el); sim_true[2] = -cosf(el) * cosf(sim_az);
                zp_sim_clap(sh, sim_true);
            }
            for (int ch = 0; ch < ZYLIA_MICS; ++ch) sh->rms[ch] *= expf(-4.0f * dt);   /* meter decay */
        }

        long seq = sh->seq;                                   /* fresh transient snapshot -> TDOA -> DOA */
        if (seq != last_seq) {
            last_seq = seq;
            memcpy(snap, (const void*)sh->snap, sizeof snap);
            const float* ptr[ZYLIA_MICS];
            for (int ch = 0; ch < ZYLIA_MICS; ++ch) ptr[ch] = snap[ch];
            uint32_t max_lag = (uint32_t)(sh->rate * (2.0 * 0.049 / 343.0) * 2.0) + 4;   /* 2x the array's max TDOA */
            double arr[ZYLIA_MICS]; float dir[3];
            if (zylia_tdoa(ptr, ZP_SNAP_N, sh->rate, max_lag, arr) && zylia_doa(arr, dir)) {
                DoaHit* h = &hist[hist_n++ % ZP_HIST];
                h->dir[0] = dir[0]; h->dir[1] = dir[1]; h->dir[2] = dir[2];
                h->age = 0.0f; h->valid = 1;
                ++snaps;
                fprintf(stderr, "arrival %d: az %+.1f el %+.1f  dir (%+.3f, %+.3f, %+.3f)%s\n",
                        snaps, zp_azimuth(dir), zp_elevation(dir), dir[0], dir[1], dir[2],
                        sim ? TextFormat("  truth (%+.3f, %+.3f, %+.3f)", sim_true[0], sim_true[1], sim_true[2]) : "");
            } else ++rejects;                                 /* not transient enough / degenerate solve */
        }
        for (int i = 0; i < ZP_HIST; ++i) if (hist[i].valid) hist[i].age += dt;

        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {          /* arcball */
            Vector2 md = GetMouseDelta();
            cyaw -= md.x * 0.005f; cpitch += md.y * 0.005f;
            if (cpitch >  1.5f) cpitch =  1.5f;
            if (cpitch < -1.5f) cpitch = -1.5f;
        }
        cdist -= GetMouseWheelMove() * 0.3f;
        if (cdist < 1.6f) cdist = 1.6f;
        if (cdist > 10.f) cdist = 10.f;
        cam.position = (Vector3){ cdist * cosf(cpitch) * sinf(cyaw), cdist * sinf(cpitch), cdist * cosf(cpitch) * cosf(cyaw) };

        BeginDrawing();
        ClearBackground((Color){ 25, 25, 30, 255 });
        BeginMode3D(cam);
        DrawSphereWires((Vector3){ 0, 0, 0 }, 1.0f, 12, 16, (Color){ 70, 80, 100, 160 });
        DrawLine3D((Vector3){ 0, 0, 0 }, (Vector3){ 1.5f, 0, 0 }, (Color){ 200, 80, 80, 160 });    /* +X */
        DrawLine3D((Vector3){ 0, 0, 0 }, (Vector3){ 0, 1.5f, 0 }, (Color){ 80, 200, 80, 160 });    /* +Y */
        DrawLine3D((Vector3){ 0, 0, 0 }, (Vector3){ 0, 0, -1.5f }, (Color){ 80, 120, 220, 160 });  /* -Z = front */
        for (int ch = 0; ch < ZYLIA_MICS; ++ch) {             /* capsules, sized + lit by their live meter */
            float r = sh->rms[ch];
            float db = (r > 1e-6f) ? 20.0f * log10f(r) : -120.0f;
            float t  = (db + 80.0f) / 80.0f;                  /* -80 dBFS .. 0 dBFS -> 0..1 */
            if (t < 0) t = 0; if (t > 1) t = 1;
            Color c = { (unsigned char)(70 + 50 * t), (unsigned char)(90 + 165 * t), (unsigned char)(110 + 30 * t), 255 };
            DrawSphere((Vector3){ dirs[ch][0], dirs[ch][1], dirs[ch][2] }, 0.035f + 0.06f * t, c);
        }
        if (sim)                                              /* ground truth: the dot must land in this ring */
            DrawCircle3D((Vector3){ 1.18f * sim_true[0], 1.18f * sim_true[1], 1.18f * sim_true[2] },
                         0.10f, (Vector3){ 0, 1, 0 }, 0.0f, (Color){ 245, 245, 120, 200 });
        for (int i = 0; i < ZP_HIST; ++i) {                   /* arrival dots, fading; the newest gets the arrow */
            if (!hist[i].valid || hist[i].age > 6.0f) continue;
            float a = 1.0f - hist[i].age / 6.0f;
            Vector3 p = { 1.18f * hist[i].dir[0], 1.18f * hist[i].dir[1], 1.18f * hist[i].dir[2] };
            DrawSphere(p, 0.045f + 0.03f * a, (Color){ 245, 120, 80, (unsigned char)(60 + 195 * a) });
            if (hist[i].age < 1.5f)
                DrawLine3D((Vector3){ 0, 0, 0 }, p, (Color){ 245, 140, 90, (unsigned char)(120 + 135 * a) });
        }
        EndMode3D();

        /* ---- HUD: status line + the 19 per-capsule meters (the console view, kept visible) ---- */
        DrawRectangle(0, 0, GetScreenWidth(), 58, (Color){ 0, 0, 0, 195 });
        ui_text(TextFormat("%s   %d ch @ %.0f Hz   blocks %ld   claps %d%s   right-drag orbit, wheel zoom, F11, ESC",
                            sh->title ? sh->title : "?", sh->nch, sh->rate, sh->blocks, snaps,
                            rejects ? TextFormat("  (rejected %d)", rejects) : ""),
                 12, 8, 15, RAYWHITE);
        int newest = -1; float best = 1e9f;
        for (int i = 0; i < ZP_HIST; ++i) if (hist[i].valid && hist[i].age < best) { best = hist[i].age; newest = i; }
        if (newest >= 0)
            ui_text(TextFormat("last arrival:  az %+.1f deg   el %+.1f deg   dir (%+.2f, %+.2f, %+.2f)%s",
                                zp_azimuth(hist[newest].dir), zp_elevation(hist[newest].dir),
                                hist[newest].dir[0], hist[newest].dir[1], hist[newest].dir[2],
                                sim ? "   (ring = truth)" : ""),
                     12, 30, 15, (Color){ 245, 160, 110, 255 });
        else
            ui_text(sim ? "synthesizing claps..." : "CLAP (or snap fingers) anywhere around the array -> a dot appears where it came from",
                     12, 30, 15, (Color){ 110, 200, 255, 255 });

        const int mx = 12, my = 76, mw = 14, mh = 120;        /* meter strip, left edge */
        for (int ch = 0; ch < ZYLIA_MICS && ch < sh->nch; ++ch) {
            float r  = sh->rms[ch];
            float db = (r > 1e-6f) ? 20.0f * log10f(r) : -120.0f;
            float t  = (db + 80.0f) / 80.0f;
            if (t < 0) t = 0; if (t > 1) t = 1;
            int h = (int)(t * mh);
            DrawRectangle(mx + ch * (mw + 3), my + mh - h, mw, h,
                          (Color){ (unsigned char)(80 + 60 * t), (unsigned char)(120 + 135 * t), 130, 255 });
            DrawRectangleLines(mx + ch * (mw + 3), my, mw, mh, (Color){ 70, 80, 100, 200 });
            ui_text(TextFormat("%d", ch), mx + ch * (mw + 3) + 1, my + mh + 4, 12, (Color){ 160, 160, 175, 255 });
        }
        EndDrawing();
    }
    CloseWindow();
}
