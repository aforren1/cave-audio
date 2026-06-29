/*
 * hull.h — convex-hull triangulation of unit direction vectors + VBAP gains within it. Pure and
 * alloc-free. Shared by allrad.c (load-time decode build over a virtual layer) and vbap.c (the VBAP
 * panner's per-listener triangulation cache). For N speakers around the origin the convex hull of
 * their unit directions IS the spherical triangulation the panner needs.
 */
#ifndef BW_HULL_H
#define BW_HULL_H

#include <stdint.h>

/* Brute-force convex-hull faces of `dirs` (n unit vectors). Writes up to `maxtri` triangles (vertex
 * index triples) to `tri` and their scalar triple products det([a b c]) to `det`. Returns the
 * triangle count, or 0 on overflow (> maxtri faces — a near-coplanar/degenerate set) or if the set is
 * not triangulable. O(n^4); intended for n <= ~64. */
int hull_triangulate(float (*dirs)[3], uint32_t n, int (*tri)[3], float* det, int maxtri);  /* dirs read-only */

/* VBAP a unit direction `dir` onto the array: find the containing hull triangle (largest min gain),
 * write its 3 speaker indices to `spk[3]` and the clamped, L2-normalised (constant-power) gains to
 * `gain[3]`. Returns 1 on success, 0 if no usable triangle (e.g. ntri == 0). (dirs, tri read-only —
 * non-const only because C won't implicitly add const through a `(*)[3]` array-pointer.) */
int hull_vbap(const float dir[3], float (*dirs)[3], int (*tri)[3], const float* det,
              int ntri, int spk[3], float gain[3]);

#endif /* BW_HULL_H */
