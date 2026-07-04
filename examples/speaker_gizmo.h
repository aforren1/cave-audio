/*
 * speaker_gizmo.h — a small "real speaker" glyph for the raylib tools (playground, layout tool):
 * a tapered cabinet with a dark driver cone AIMED at the listener. The aim is real information —
 * every cabinet in the installation points at the sweet spot, so the glyph shows channel
 * orientation at a glance (and makes a mis-surveyed position read wrong immediately).
 * Pure raylib primitives, stateless, C and C++ (no compound literals).
 */
#ifndef BW_SPEAKER_GIZMO_H
#define BW_SPEAKER_GIZMO_H

#include "raylib.h"
#include "raymath.h"

static inline void draw_speaker_gizmo(Vector3 pos, Vector3 aim_at, float scale, Color body) {
    Vector3 d = Vector3Subtract(aim_at, pos);
    float   L = Vector3Length(d);
    Vector3 a;
    if (L > 1e-6f) a = Vector3Scale(d, 1.0f / L);
    else { a.x = 0.f; a.y = 0.f; a.z = 1.f; }              /* degenerate: aim +Z */

    Color grille; grille.r = 30; grille.g = 30; grille.b = 36; grille.a = 255;
    Color cone;   cone.r   = 62; cone.g   = 62; cone.b   = 72; cone.a   = 255;
    Color wire;                                            /* brightened body -> a crisp cabinet silhouette */
    wire.r = (unsigned char)(body.r + (255 - body.r) / 2);
    wire.g = (unsigned char)(body.g + (255 - body.g) / 2);
    wire.b = (unsigned char)(body.b + (255 - body.b) / 2);
    wire.a = 220;

    Vector3 back   = Vector3Subtract(pos, Vector3Scale(a, 0.55f * scale));
    Vector3 front  = Vector3Add(pos, Vector3Scale(a, 0.45f * scale));
    Vector3 f_rim  = Vector3Add(front, Vector3Scale(a, 0.05f * scale));
    Vector3 f_apex = Vector3Add(front, Vector3Scale(a, 0.34f * scale));
    DrawCylinderEx(back, front, 0.50f * scale, 0.42f * scale, 10, body);    /* cabinet, tapered to the face */
    DrawCylinderWiresEx(back, front, 0.50f * scale, 0.42f * scale, 10, wire);   /* wireframe outline */
    DrawCylinderEx(front, f_rim, 0.34f * scale, 0.32f * scale, 12, grille); /* driver surround */
    DrawCylinderEx(f_rim, f_apex, 0.27f * scale, 0.04f * scale, 12, cone);  /* the cone, at the listener */
}

#endif /* BW_SPEAKER_GIZMO_H */
