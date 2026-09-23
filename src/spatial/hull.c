/* hull.c — see hull.h. */
#include "spatial/hull.h"
#include "bw_audio.h"          /* BWA_MAX_CHANNELS */

#include <math.h>

/* allrad.c appends up to two imaginary pole speakers to a full layout */
#define HULL_MAXPTS (BWA_MAX_CHANNELS + 2)
#define HULL_MAXF   (2 * HULL_MAXPTS - 4)   /* Euler: a triangulated sphere on V vertices has 2V-4 faces */

/* The one tolerance, in the units of the input (unit vectors, so absolute). A point within HULL_EPS
 * of a face's plane counts as ON it: it neither sees that face nor becomes a vertex through it.
 * It is not zero for three reasons. A coplanar ring (a barrel cap, a cube face) must triangulate its
 * polygon once, which needs "on the plane" to be a stable answer rather than a rounding coin flip
 * that folds the cap into slivers. Float directions carry ~1e-7 of rounding and a surveyed layout
 * can carry ~1e-6 of jitter, and the tolerance has to sit above both. And 1e-5 is what the old
 * brute-force hull used, so the two agree on which sets are generic. The price: two speakers within
 * ~0.5 degree of each other collapse to one vertex, which VBAP could not tell apart anyway. */
#define HULL_EPS 1e-5

typedef struct {
    int    v[3];      /* outward: counter-clockwise seen from outside */
    int    nb[3];     /* nb[e] shares edge v[e] -> v[(e+1)%3] */
    double n[3];      /* unit outward normal */
    double off;       /* n . P[v[0]] */
    int    alive, seen;
} HFace;

static float dot3(const float a[3], const float b[3]) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
static void  cross3(const float a[3], const float b[3], float o[3]) {
    o[0] = a[1]*b[2] - a[2]*b[1]; o[1] = a[2]*b[0] - a[0]*b[2]; o[2] = a[0]*b[1] - a[1]*b[0];
}
static float triple(const float a[3], const float b[3], const float c[3]) {  /* a . (b x c) */
    float bc[3]; cross3(b, c, bc); return dot3(a, bc);
}

/* Double for every predicate: the input is float, so rounding then sits orders of magnitude below
 * HULL_EPS and the tolerance alone decides every "on the plane" question. */
static void   dsub(const double a[3], const double b[3], double o[3]) { o[0]=a[0]-b[0]; o[1]=a[1]-b[1]; o[2]=a[2]-b[2]; }
static double ddot(const double a[3], const double b[3]) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
static void   dcross(const double a[3], const double b[3], double o[3]) {
    o[0] = a[1]*b[2] - a[2]*b[1]; o[1] = a[2]*b[0] - a[0]*b[2]; o[2] = a[0]*b[1] - a[1]*b[0];
}

/* 0 for a sliver. The tolerance keeps every new apex > HULL_EPS off its rim edge, so this only
 * trips on input the seed checks should already have refused. */
static int face_plane(HFace* f, double (*P)[3]) {
    double e1[3], e2[3], c[3];
    dsub(P[f->v[1]], P[f->v[0]], e1); dsub(P[f->v[2]], P[f->v[0]], e2); dcross(e1, e2, c);
    double l = sqrt(ddot(c, c));
    if (l < 1e-12) return 0;
    f->n[0] = c[0]/l; f->n[1] = c[1]/l; f->n[2] = c[2]/l;
    f->off = ddot(f->n, P[f->v[0]]);
    return 1;
}
static double fdist(const HFace* f, const double p[3]) { return ddot(f->n, p) - f->off; }

static int tri_less(const int a[3], const int b[3]) {
    return a[0] != b[0] ? a[0] < b[0] : (a[1] != b[1] ? a[1] < b[1] : a[2] < b[2]);
}

int hull_triangulate(float (*dirs)[3], uint32_t n, int (*tri)[3], float* det, int maxtri) {
    if (n < 4 || n > HULL_MAXPTS) return 0;

    double P[HULL_MAXPTS][3];
    for (uint32_t i = 0; i < n; ++i) { P[i][0] = dirs[i][0]; P[i][1] = dirs[i][1]; P[i][2] = dirs[i][2]; }

    /* Seed tetrahedron from farthest-from choices, so the first planes are well conditioned. Strict >
     * breaks ties toward the lower index. */
    int s[4] = { 0, -1, -1, -1 };
    double best = 0.0, d[3], c[3];
    for (uint32_t i = 1; i < n; ++i) { dsub(P[i], P[0], d); double q = ddot(d, d); if (q > best) { best = q; s[1] = (int)i; } }
    if (s[1] < 0 || sqrt(best) <= HULL_EPS) return 0;                     /* all one point */
    double u[3]; dsub(P[s[1]], P[0], u);
    best = 0.0;
    for (uint32_t i = 1; i < n; ++i) {
        dsub(P[i], P[0], d); dcross(u, d, c);
        double q = ddot(c, c); if (q > best) { best = q; s[2] = (int)i; }
    }
    if (s[2] < 0 || sqrt(best / ddot(u, u)) <= HULL_EPS) return 0;       /* collinear */
    double w[3], nrm[3]; dsub(P[s[2]], P[0], w); dcross(u, w, nrm);
    { double l = sqrt(ddot(nrm, nrm)); nrm[0] /= l; nrm[1] /= l; nrm[2] /= l; }
    best = 0.0;
    for (uint32_t i = 1; i < n; ++i) {
        dsub(P[i], P[0], d); double q = fabs(ddot(nrm, d));
        if (q > best) { best = q; s[3] = (int)i; }
    }
    if (s[3] < 0 || best <= HULL_EPS) return 0;                           /* coplanar */

    HFace F[HULL_MAXF];
    int nf = 0, freel[HULL_MAXF], nfree = 0;
    {
        static const int fv[4][4] = { {0,1,2,3}, {0,3,1,2}, {1,3,2,0}, {0,2,3,1} };  /* 3 verts + opposite */
        for (int k = 0; k < 4; ++k) {
            HFace* f = &F[nf++];
            f->v[0] = s[fv[k][0]]; f->v[1] = s[fv[k][1]]; f->v[2] = s[fv[k][2]];
            f->alive = 1; f->seen = -1;
            if (!face_plane(f, P)) return 0;
            if (fdist(f, P[s[fv[k][3]]]) > 0.0) {                         /* opposite vertex goes behind */
                int t = f->v[1]; f->v[1] = f->v[2]; f->v[2] = t;
                face_plane(f, P);
            }
        }
        for (int a = 0; a < 4; ++a)
            for (int e = 0; e < 3; ++e) {
                int va = F[a].v[e], vb = F[a].v[(e+1)%3];
                for (int b = 0; b < 4; ++b) if (b != a)
                    for (int g = 0; g < 3; ++g)
                        if (F[b].v[g] == vb && F[b].v[(g+1)%3] == va) F[a].nb[e] = b;
            }
    }

    int stack[HULL_MAXF], vis[HULL_MAXF], newf[HULL_MAXF];
    int hz_a[HULL_MAXF], hz_b[HULL_MAXF], hz_g[HULL_MAXF];   /* rim edge a -> b, and the kept face across it */
    int at_start[HULL_MAXPTS], at_end[HULL_MAXPTS], st_start[HULL_MAXPTS], st_end[HULL_MAXPTS];
    for (uint32_t i = 0; i < n; ++i) st_start[i] = st_end[i] = -1;

    /* Index order rather than farthest-point order: the final hull does not depend on it, and a
     * fixed order is the simplest guarantee of the same output on every platform. */
    for (uint32_t pi = 0; pi < n; ++pi) {
        const int p = (int)pi;
        if (p == s[0] || p == s[1] || p == s[2] || p == s[3]) continue;

        int seed = -1; double far = HULL_EPS;
        for (int f = 0; f < nf; ++f)
            if (F[f].alive) { double q = fdist(&F[f], P[p]); if (q > far) { far = q; seed = f; } }
        if (seed < 0) continue;                               /* inside, or on the surface */

        /* Grow the visible set from the most visible face so it is connected by construction. Faces
         * tested one by one can come out in pieces near the tolerance, and no cone closes that. */
        int nvis = 0, sp = 0;
        stack[sp++] = seed; F[seed].seen = p;
        while (sp) {
            int f = stack[--sp]; vis[nvis++] = f;
            for (int e = 0; e < 3; ++e) {
                int g = F[f].nb[e];
                if (F[g].seen != p && fdist(&F[g], P[p]) > HULL_EPS) { F[g].seen = p; stack[sp++] = g; }
            }
        }

        int nh = 0;
        for (int k = 0; k < nvis; ++k) {
            const HFace* f = &F[vis[k]];
            for (int e = 0; e < 3; ++e) {
                int g = f->nb[e];
                if (F[g].seen == p) continue;
                int a = f->v[e], b = f->v[(e+1)%3];
                /* A rim vertex used twice means the visible set is not a disk (a within-tolerance
                 * face enclosed by visible ones). Refuse rather than stitch a non-manifold. */
                if (st_start[a] == p || st_end[b] == p) return 0;
                st_start[a] = p; st_end[b] = p;
                at_start[a] = nh; at_end[b] = nh;
                hz_a[nh] = a; hz_b[nh] = b; hz_g[nh] = g; ++nh;
            }
        }
        {   /* and one loop, not several */
            int k = 0, steps = 0;
            do {
                if (st_start[hz_b[k]] != p) return 0;          /* the rim is open */
                k = at_start[hz_b[k]]; ++steps;
            } while (k != 0 && steps <= nh);
            if (nh < 3 || steps != nh) return 0;
        }

        for (int k = 0; k < nvis; ++k) { F[vis[k]].alive = 0; freel[nfree++] = vis[k]; }
        for (int k = 0; k < nh; ++k) {
            if (!nfree && nf >= HULL_MAXF) return 0;
            int f = nfree ? freel[--nfree] : nf++;
            newf[k] = f;
            HFace* h = &F[f];
            h->v[0] = hz_a[k]; h->v[1] = hz_b[k]; h->v[2] = p;
            h->alive = 1; h->seen = p;
            if (!face_plane(h, P)) return 0;
            h->nb[0] = hz_g[k];
            HFace* g = &F[hz_g[k]];
            for (int e = 0; e < 3; ++e)
                if (g->v[e] == hz_b[k] && g->v[(e+1)%3] == hz_a[k]) g->nb[e] = f;
        }
        for (int k = 0; k < nh; ++k) {
            HFace* h = &F[newf[k]];
            h->nb[1] = newf[at_start[hz_b[k]]];   /* b -> p is the next face's p -> b */
            h->nb[2] = newf[at_end[hz_a[k]]];     /* p -> a is the previous face's a -> p */
        }
    }

    /* Emit only faces the origin sits behind. With the listener inside the array that is every
     * face. With it outside, the faces it sees from the front cover the same bearings as the far
     * faces, and hull_vbap would have two answers to choose between. */
    int nt = 0;
    for (int f = 0; f < nf; ++f) {
        if (!F[f].alive || !(F[f].off > 0.0)) continue;
        if (nt >= maxtri) return 0;
        const int a = F[f].v[0], b = F[f].v[1], cc = F[f].v[2];
        int r[3];                                           /* lowest index first, orientation kept */
        if (a < b && a < cc) { r[0] = a;  r[1] = b;  r[2] = cc; }
        else if (b < cc)     { r[0] = b;  r[1] = cc; r[2] = a;  }
        else                 { r[0] = cc; r[1] = a;  r[2] = b;  }
        /* sorted, so the output depends on the face set alone and not on slot reuse */
        int j = nt;
        while (j > 0 && tri_less(r, tri[j-1])) { tri[j][0] = tri[j-1][0]; tri[j][1] = tri[j-1][1]; tri[j][2] = tri[j-1][2]; --j; }
        tri[j][0] = r[0]; tri[j][1] = r[1]; tri[j][2] = r[2];
        ++nt;
    }
    for (int t = 0; t < nt; ++t) det[t] = triple(dirs[tri[t][0]], dirs[tri[t][1]], dirs[tri[t][2]]);
    return nt;
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
