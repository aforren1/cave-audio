/*
 * constraints_view.h — load + draw the placement-constraints file (constraints.json: an allowed
 * `bounds` box, `nogo` keep-out boxes, solid `obstacles`) shared by the raylib tools. The layout
 * tool edits AGAINST these (snap/projection/line-of-sight live there); the playground just draws
 * them for orientation — the same green/red/orange boxes in both tools means the room reads the
 * same everywhere. One copy of the JSON parsing, so a schema change lands everywhere at once.
 * C and C++ (no compound literals). cJSON + raylib.
 */
#ifndef BW_CONSTRAINTS_VIEW_H
#define BW_CONSTRAINTS_VIEW_H

#include "raylib.h"
#include "raymath.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>

#define CV_MAX_BOXES 24

typedef struct { Vector3 lo, hi; } CvBox;

typedef struct {
    int   loaded;                 /* a constraints file was parsed */
    CvBox bounds;                 /* the allowed box (speakers must be inside) */
    CvBox nogo[CV_MAX_BOXES];     /* keep-out boxes (screens, structure, doorways) */
    int   nnogo;
    CvBox obst[CV_MAX_BOXES];     /* solid occluders (projectors, beams): also block line of sight */
    int   nobst;
} CvConstraints;

static inline int cv_read_box(cJSON* o, CvBox* out) {
    cJSON* mn = cJSON_GetObjectItemCaseSensitive(o, "min");
    cJSON* mx = cJSON_GetObjectItemCaseSensitive(o, "max");
    if (!cJSON_IsArray(mn) || cJSON_GetArraySize(mn) != 3 || !cJSON_IsArray(mx) || cJSON_GetArraySize(mx) != 3) return 0;
    float a[3], b[3];
    for (int k = 0; k < 3; ++k) {
        cJSON *ak = cJSON_GetArrayItem(mn, k), *bk = cJSON_GetArrayItem(mx, k);
        if (!cJSON_IsNumber(ak) || !cJSON_IsNumber(bk)) return 0;
        a[k] = (float)ak->valuedouble; b[k] = (float)bk->valuedouble;
    }
    out->lo.x = fminf(a[0], b[0]); out->lo.y = fminf(a[1], b[1]); out->lo.z = fminf(a[2], b[2]);   /* tolerate min/max swapped */
    out->hi.x = fmaxf(a[0], b[0]); out->hi.y = fmaxf(a[1], b[1]); out->hi.z = fmaxf(a[2], b[2]);
    return 1;
}

/* Parse constraints.json into `c` (bounds default +/-3 m in x/z and 0..3 m in y — floor origin —
 * if the file omits them). Returns 1 when a
 * file was read; 0 (and c->loaded = 0) when absent/unparseable — every placement allowed. */
static inline int cv_load(const char* path, CvConstraints* c) {
    c->loaded = 0; c->nnogo = 0; c->nobst = 0;
    c->bounds.lo.x = c->bounds.lo.z = -3.f; c->bounds.lo.y = 0.f;   /* the floor is y = 0 */
    c->bounds.hi.x = c->bounds.hi.y = c->bounds.hi.z =  3.f;
    FILE* f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return 0; }
    char* buf = (char*)malloc((size_t)n + 1); if (!buf) { fclose(f); return 0; }
    size_t rd = fread(buf, 1, (size_t)n, f); buf[rd] = 0; fclose(f);
    cJSON* root = cJSON_Parse(buf); free(buf);
    if (!root) return 0;
    cJSON* b = cJSON_GetObjectItemCaseSensitive(root, "bounds");
    if (cJSON_IsObject(b)) cv_read_box(b, &c->bounds);
    cJSON* ng = cJSON_GetObjectItemCaseSensitive(root, "nogo");
    if (cJSON_IsArray(ng)) {
        cJSON* box;
        cJSON_ArrayForEach(box, ng) if (c->nnogo < CV_MAX_BOXES && cv_read_box(box, &c->nogo[c->nnogo])) ++c->nnogo;
    }
    cJSON* ob = cJSON_GetObjectItemCaseSensitive(root, "obstacles");
    if (cJSON_IsArray(ob)) {
        cJSON* box;
        cJSON_ArrayForEach(box, ob) if (c->nobst < CV_MAX_BOXES && cv_read_box(box, &c->obst[c->nobst])) ++c->nobst;
    }
    cJSON_Delete(root);
    c->loaded = 1;
    return 1;
}

/* Draw the boxes (inside BeginMode3D): green bounds wire, red no-go wires, orange filled solids. */
static inline void cv_draw(const CvConstraints* c) {
    if (!c->loaded) return;
    Color cb; cb.r =  90; cb.g = 200; cb.b = 120; cb.a = 110;
    Color cn; cn.r = 235; cn.g =  90; cn.b =  90; cn.a = 170;
    Color of; of.r = 235; of.g = 150; of.b =  60; of.a =  70;
    Color ow; ow.r = 245; ow.g = 165; ow.b =  70; ow.a = 200;
    DrawCubeWiresV(Vector3Scale(Vector3Add(c->bounds.lo, c->bounds.hi), 0.5f),
                   Vector3Subtract(c->bounds.hi, c->bounds.lo), cb);
    for (int i = 0; i < c->nnogo; ++i)
        DrawCubeWiresV(Vector3Scale(Vector3Add(c->nogo[i].lo, c->nogo[i].hi), 0.5f),
                       Vector3Subtract(c->nogo[i].hi, c->nogo[i].lo), cn);
    for (int i = 0; i < c->nobst; ++i) {
        Vector3 ctr = Vector3Scale(Vector3Add(c->obst[i].lo, c->obst[i].hi), 0.5f);
        Vector3 sz  = Vector3Subtract(c->obst[i].hi, c->obst[i].lo);
        DrawCubeV(ctr, sz, of);
        DrawCubeWiresV(ctr, sz, ow);
    }
}

#endif /* BW_CONSTRAINTS_VIEW_H */
