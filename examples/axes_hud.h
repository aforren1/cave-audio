/*
 * axes_hud.h — a small screen-corner XYZ orientation triad for the raylib tools
 * (playground, layout tool): the three room axes (X red, Y green, Z blue) projected
 * through the current camera, so the viewer always knows which way is which while
 * orbiting or in head view. Room frame is right-handed, y up. Draw in the 2D pass
 * (after EndMode3D). Pure raylib primitives, stateless, C and C++ (no compound literals).
 */
#ifndef BWA_AXES_HUD_H
#define BWA_AXES_HUD_H

#include "raylib.h"
#include "raymath.h"

/* Anchor (cx, cy) in screen pixels, spokes `len` px long. */
static inline void draw_axes_hud(Camera3D cam, float cx, float cy, float len) {
    Vector3 fwd   = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    Vector3 right = Vector3Normalize(Vector3CrossProduct(fwd, cam.up));
    Vector3 up    = Vector3CrossProduct(right, fwd);

    Vector3 ax[3]; Color col[3];
    static const char* lab[3] = { "X", "Y", "Z" };
    ax[0].x = 1; ax[0].y = 0; ax[0].z = 0; col[0].r = 235; col[0].g =  90; col[0].b =  90; col[0].a = 255;
    ax[1].x = 0; ax[1].y = 1; ax[1].z = 0; col[1].r = 110; col[1].g = 220; col[1].b = 120; col[1].a = 255;
    ax[2].x = 0; ax[2].y = 0; ax[2].z = 1; col[2].r =  95; col[2].g = 145; col[2].b = 245; col[2].a = 255;

    Color back; back.r = 0; back.g = 0; back.b = 0; back.a = 110;
    DrawCircle((int)cx, (int)cy, len * 1.55f, back);       /* faint backing so it reads on a busy scene */

    /* painter's order: the spoke pointing away from the camera first, the nearest last */
    float d[3]; int order[3] = { 0, 1, 2 };
    for (int i = 0; i < 3; ++i) d[i] = Vector3DotProduct(ax[i], fwd);
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2 - i; ++j)
            if (d[order[j]] < d[order[j + 1]]) { int t = order[j]; order[j] = order[j + 1]; order[j + 1] = t; }

    int fs = (int)(len * 0.45f); if (fs < 10) fs = 10;
    for (int k = 0; k < 3; ++k) {
        int i = order[k];
        float sx = Vector3DotProduct(ax[i], right);        /* the axis on screen: x right, y up */
        float sy = Vector3DotProduct(ax[i], up);
        Vector2 a2, tip;
        a2.x = cx;                a2.y = cy;
        tip.x = cx + sx * len;    tip.y = cy - sy * len;
        DrawLineEx(a2, tip, 2.5f, col[i]);
        DrawText(lab[i], (int)(cx + sx * (len + 9.f)) - fs / 3,
                         (int)(cy - sy * (len + 9.f)) - fs / 2, fs, col[i]);
    }
    Color hub; hub.r = 225; hub.g = 225; hub.b = 232; hub.a = 255;
    DrawCircle((int)cx, (int)cy, 2.5f, hub);
}

#endif /* BWA_AXES_HUD_H */
