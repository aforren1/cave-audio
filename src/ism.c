/*
 * ism.c — see ism.h. Mirroring a point across an axis-aligned plane is a one-line reflection:
 * the coordinate normal to the face becomes 2*plane - x. The room is floor-based (bw_scene_set_box):
 * x in [-w/2, w/2], y in [0, h], z in [-d/2, d/2].
 */
#include "ism.h"

#include <math.h>

int ism_images(const IsmRoom* r, const float src[3], IsmImage* out) {
    if (!r || !src || !out || !r->valid) return 0;
    if (!(r->w > 0.f) || !(r->h > 0.f) || !(r->d > 0.f)) return 0;

    const float lo[3] = { -0.5f * r->w, 0.f,  -0.5f * r->d };   /* the six planes, in face order */
    const float hi[3] = {  0.5f * r->w, r->h,  0.5f * r->d };
    for (int a = 0; a < 3; ++a)                                 /* a source outside the room has no valid
                                                                 * mirror geometry — render it dry */
        if (src[a] < lo[a] || src[a] > hi[a]) return 0;

    for (int f = 0; f < ISM_FACES; ++f) {
        const int   axis  = f >> 1;                             /* face order: -x,+x,-y,+y,-z,+z */
        const float plane = (f & 1) ? hi[axis] : lo[axis];
        for (int k = 0; k < 3; ++k) out[f].pos[k] = src[k];
        out[f].pos[axis] = 2.f * plane - src[axis];             /* mirror across the face */
        for (int b = 0; b < 3; ++b) {
            float a1 = r->absorb[f][b];
            if (a1 < 0.f) a1 = 0.f; else if (a1 > 1.f) a1 = 1.f;
            out[f].refl[b] = sqrtf(1.f - a1);                   /* energy absorption -> pressure coefficient */
        }
    }
    return ISM_IMAGES;
}
