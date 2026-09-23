/*
 * hull.h — convex-hull triangulation of unit direction vectors + VBAP gains within it. Pure and
 * alloc-free. Shared by allrad.c (load-time decode build over a virtual layer) and vbap.c (the VBAP
 * panner's per-listener triangulation cache). For N speakers around the origin the convex hull of
 * their unit directions IS the spherical triangulation the panner needs.
 */
#ifndef BWA_HULL_H
#define BWA_HULL_H

#include <stdint.h>

/* Convex-hull faces of `dirs` (n unit vectors, n <= BWA_MAX_CHANNELS + 2). Writes up to `maxtri`
 * triangles (vertex index triples, outward, lowest index first, sorted) to `tri` and their scalar
 * triple products det([a b c]) to `det`. Only faces the origin sits behind are written, so det > 0.
 * A coplanar cap comes out as ONE triangulation of its polygon; the face count is at most 2n-4
 * (Euler), 124 at n = 64. Returns the triangle count, or 0 if the set is degenerate (all points
 * coplanar, collinear or coincident), n is out of range, or the count exceeds `maxtri`.
 * Incremental hull, O(n^2) worst case, ~15 KB of stack and no allocation: the VBAP panner rebuilds
 * it on the AUDIO thread whenever the tracked listener moves. */
int hull_triangulate(float (*dirs)[3], uint32_t n, int (*tri)[3], float* det, int maxtri);  /* dirs read-only */

/* VBAP a unit direction `dir` onto the array: find the containing hull triangle (largest min gain),
 * write its 3 speaker indices to `spk[3]` and the clamped, L2-normalized (constant-power) gains to
 * `gain[3]`. Returns 1 on success, 0 if no usable triangle (e.g. ntri == 0). (dirs, tri read-only —
 * non-const only because C won't implicitly add const through a `(*)[3]` array-pointer.) */
int hull_vbap(const float dir[3], float (*dirs)[3], int (*tri)[3], const float* det,
              int ntri, int spk[3], float gain[3]);

#endif /* BWA_HULL_H */
