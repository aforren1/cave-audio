/* hull.c — see hull.h. */
#include "hull.h"

#include <math.h>

static float dot3(const float a[3], const float b[3]) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
static void  cross3(const float a[3], const float b[3], float o[3]) {
    o[0] = a[1]*b[2] - a[2]*b[1]; o[1] = a[2]*b[0] - a[0]*b[2]; o[2] = a[0]*b[1] - a[1]*b[0];
}
static float triple(const float a[3], const float b[3], const float c[3]) {  /* a . (b x c) */
    float bc[3]; cross3(b, c, bc); return dot3(a, bc);
}

int hull_triangulate(float (*dirs)[3], uint32_t n, int (*tri)[3], float* det, int maxtri) {
    int ntri = 0;
    /* a triple (i,j,k) is a hull face iff every other point lies on the inner side of its
     * outward-oriented supporting plane (the hull contains the origin) */
    for (uint32_t i = 0; i < n; ++i)
    for (uint32_t j = i + 1; j < n; ++j)
    for (uint32_t k = j + 1; k < n; ++k) {
        float e1[3] = { dirs[j][0]-dirs[i][0], dirs[j][1]-dirs[i][1], dirs[j][2]-dirs[i][2] };
        float e2[3] = { dirs[k][0]-dirs[i][0], dirs[k][1]-dirs[i][1], dirs[k][2]-dirs[i][2] };
        float nrm[3]; cross3(e1, e2, nrm);
        float nl = sqrtf(dot3(nrm, nrm));
        if (nl < 1e-9f) continue;                              /* colinear triple */
        nrm[0]/=nl; nrm[1]/=nl; nrm[2]/=nl;
        float off = dot3(nrm, dirs[i]);
        if (off < 0.f) { nrm[0]=-nrm[0]; nrm[1]=-nrm[1]; nrm[2]=-nrm[2]; off=-off; }  /* orient outward */
        int face = 1;
        for (uint32_t m = 0; m < n; ++m) if (dot3(nrm, dirs[m]) > off + 1e-5f) { face = 0; break; }
        if (face) {
            if (ntri >= maxtri) return 0;                      /* hull too large (degenerate) -> caller decides */
            tri[ntri][0]=(int)i; tri[ntri][1]=(int)j; tri[ntri][2]=(int)k;
            det[ntri] = triple(dirs[i], dirs[j], dirs[k]);
            ++ntri;
        }
    }
    return ntri;                                               /* 0 if not triangulable */
}

int hull_vbap(const float dir[3], float (*dirs)[3], int (*tri)[3], const float* det,
              int ntri, int spk[3], float gain[3]) {
    int bt = -1; float bmin = -2.f, bg[3] = { 0, 0, 0 };
    for (int t = 0; t < ntri; ++t) {
        if (fabsf(det[t]) < 1e-9f) continue;
        const float *a = dirs[tri[t][0]], *b = dirs[tri[t][1]], *c = dirs[tri[t][2]];
        float inv = 1.f / det[t];
        float g0 = triple(dir, b, c) * inv;                    /* Cramer's rule: g = [a b c]^-1 dir */
        float g1 = triple(a, dir, c) * inv;
        float g2 = triple(a, b, dir) * inv;
        float mn = g0 < g1 ? (g0 < g2 ? g0 : g2) : (g1 < g2 ? g1 : g2);
        if (mn > bmin) { bmin = mn; bt = t; bg[0]=g0; bg[1]=g1; bg[2]=g2; }
    }
    if (bt < 0) return 0;
    for (int q = 0; q < 3; ++q) if (bg[q] < 0.f) bg[q] = 0.f;  /* clamp (dir just outside a triangle) */
    float gn = sqrtf(bg[0]*bg[0] + bg[1]*bg[1] + bg[2]*bg[2]);
    if (gn < 1e-9f) return 0;
    for (int q = 0; q < 3; ++q) { gain[q] = bg[q] / gn; spk[q] = tri[bt][q]; }  /* constant power */
    return 1;
}
