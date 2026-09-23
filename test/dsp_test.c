/*
 * dsp_test.c — M4 verification of the spatialization DSP, standalone (no rt/audio thread):
 *   - layout_default has 26 speakers; layout_load parses cave_layout.json (positions, the
 *     dbap params, gain_db->linear, delay_ms->samples);
 *   - DBAP localizes a source at each speaker to that channel (centered listener), is
 *     constant-power, splits between two speakers, and responds to listener moves;
 *   - align applies the per-channel gain trim and integer-sample delay;
 *   - the hole-aware spread floor engages on a barrel (open poles) and is inert on the
 *     surrounding cube grid.
 */
#include "core/layout.h"
#include "spatial/dbap.h"
#include "spatial/spcap.h"
#include "spatial/vbap.h"
#include "spatial/hull.h"
#include "spatial/cap.h"
#include "spatial/hole.h"
#include "spatial/align.h"
#include "spatial/ambisonics.h"
#include "spatial/allrad.h"
#include "spatial/epad.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CH   BWA_DEFAULT_GRID
#define RATE 48000u

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", (msg)); ++fails; } } while (0)

static int argmax(const float* g, int n) {
    int b = 0; float m = g[0];
    for (int i = 1; i < n; ++i) if (g[i] > m) { m = g[i]; b = i; }
    return b;
}
static int argmax_abs(const float* x, int n) {
    int b = 0; float m = -1.f;
    for (int i = 0; i < n; ++i) { float v = fabsf(x[i]); if (v > m) { m = v; b = i; } }
    return b;
}
static float lcg_noise(uint32_t* s) {                /* rt.c's LCG, [-1, 1) */
    *s = *s * 1664525u + 1013904223u;
    return (float)(*s >> 9) * (1.0f / 4194304.0f) - 1.0f;
}

static double dot3(const float a[3], const float b[3]) {
    return (double)a[0]*b[0] + (double)a[1]*b[1] + (double)a[2]*b[2];
}

/* The interaural component of a gain vector's velocity vector, rV.e = sum(g*ce)/sum(g) — the LF
 * localization cue CAP constrains, and what a two-mic ITD measurement reads on the rig. */
static double itd_component(const float* g, const float* ce, uint32_t n) {
    double sg = 0.0, sce = 0.0;
    for (uint32_t k = 0; k < n; ++k) { sg += g[k]; sce += (double)g[k] * ce[k]; }
    return sg > 1e-12 ? sce / sg : 0.0;
}

/* The plain dual-band low band (rt.c's derivation), as the A-side reference. */
static void dual_band_lo(const float* g0, uint32_t n, float* out) {
    double gs = 0.0, gp = 0.0;
    for (uint32_t k = 0; k < n; ++k) { gs += g0[k]; gp += (double)g0[k] * g0[k]; }
    if (gs > 1e-9) { const float sc = (float)(sqrt(gp) / gs);
        for (uint32_t k = 0; k < n; ++k) out[k] = g0[k] * sc; }
    else for (uint32_t k = 0; k < n; ++k) out[k] = g0[k];
}

/* Emit a valid cave_layout.json: the default grid, with speaker 5 trimmed and 9 delayed. */
static int write_layout_json(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    const float ax[3] = { -1.5f, 0.f, 1.5f };
    fprintf(f, "{\n");
    fprintf(f, "  \"dbap\": { \"rolloff_r\": 0.7, \"distance_attenuation\": "
               "{ \"reference_distance_m\": 1.0, \"rolloff\": 1.0, \"min_gain_db\": -40.0 } },\n");
    fprintf(f, "  \"speakers\": [\n");
    int k = 0;
    for (int yi = 0; yi < 3; ++yi) for (int xi = 0; xi < 3; ++xi) for (int zi = 0; zi < 3; ++zi) {
        if (ax[xi] == 0 && ax[yi] == 0 && ax[zi] == 0) continue;
        double gain_db  = (k == 5) ? -6.0 : 0.0;
        double delay_ms = (k == 9) ?  1.0 : 0.0;
        fprintf(f, "    { \"index\": %d, \"position\": [%g, %g, %g], \"gain_db\": %g, \"delay_ms\": %g }%s\n",
                k, ax[xi], ax[yi], ax[zi], gain_db, delay_ms, (k == 25) ? "" : ",");
        ++k;
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    return k == 26;
}

/* a grid layout with overridable rolloff_r and speaker-0 gain_db/delay_ms (for negative tests) */
static int write_layout_with(const char* path, double rolloff_r, double g0_db, double d0_ms) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    const float ax[3] = { -1.5f, 0.f, 1.5f };
    fprintf(f, "{ \"dbap\": { \"rolloff_r\": %g }, \"speakers\": [\n", rolloff_r);
    int k = 0;
    for (int yi = 0; yi < 3; ++yi) for (int xi = 0; xi < 3; ++xi) for (int zi = 0; zi < 3; ++zi) {
        if (ax[xi] == 0 && ax[yi] == 0 && ax[zi] == 0) continue;
        double g = (k == 0) ? g0_db : 0.0, d = (k == 0) ? d0_ms : 0.0;
        fprintf(f, "  {\"index\":%d,\"position\":[%g,%g,%g],\"gain_db\":%g,\"delay_ms\":%g}%s\n",
                k, ax[xi], ax[yi], ax[zi], g, d, (k == 25) ? "" : ",");
        ++k;
    }
    fprintf(f, "] }\n");
    fclose(f);
    return k == 26;
}

/* a grid layout carrying a room_eq_grid: two positions, one 45 Hz section on speaker 0.
 * mode 0 = valid; 1 = the second position's fc disagrees (ladder mismatch); 2 = plus a static
 * room_eq on speaker 0 (the schemes are mutually exclusive). */
static int write_layout_grid(const char* path, int mode) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    const float ax[3] = { -1.5f, 0.f, 1.5f };
    fprintf(f, "{ \"speakers\": [\n");
    int k = 0;
    for (int yi = 0; yi < 3; ++yi) for (int xi = 0; xi < 3; ++xi) for (int zi = 0; zi < 3; ++zi) {
        if (ax[xi] == 0 && ax[yi] == 0 && ax[zi] == 0) continue;
        fprintf(f, "  {\"index\":%d,\"position\":[%g,%g,%g]%s}%s\n", k, ax[xi], ax[yi], ax[zi],
                (mode == 2 && k == 0) ? ",\"room_eq\":[{\"fc\":80,\"gain_db\":-6,\"q\":4}]" : "",
                (k == 25) ? "" : ",");
        ++k;
    }
    fprintf(f, "],\n\"room_eq_grid\": [\n");
    for (int p = 0; p < 2; ++p) {
        double fc = (p == 1 && mode == 1) ? 60.0 : 45.0;      /* mode 1: ladder mismatch */
        double g  = (p == 0) ? -8.0 : -4.0;
        fprintf(f, " {\"position\":[%g,1.5,0],\"speakers\":[\n  [{\"fc\":%g,\"gain_db\":%g,\"q\":6}]",
                p ? 1.0 : -1.0, fc, g);
        for (int s = 1; s < 26; ++s) fprintf(f, ",[]");
        fprintf(f, "\n ]}%s\n", p ? "" : ",");
    }
    fprintf(f, "]}\n");
    fclose(f);
    return k == 26;
}

/* an N-speaker layout: the first N positions of the default grid, indices 0..N-1 (bad_index >= 0
 * replaces speaker 0's index — a gap/out-of-range case for the loader to reject) */
static int write_layout_n(const char* path, int n, int bad_index) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    static Layout g; layout_default(&g);
    fprintf(f, "{ \"speakers\": [\n");
    for (int k = 0; k < n; ++k) {
        int idx = (k == 0 && bad_index >= 0) ? bad_index : k;
        float p[3];
        if (k < BWA_DEFAULT_GRID) { p[0] = g.speakers[k].pos[0]; p[1] = g.speakers[k].pos[1]; p[2] = g.speakers[k].pos[2]; }
        else {   /* past the grid (capacity tests): distinct points on a 2 m ring, stepped in height */
            const float a = 0.37f * (float)k;
            p[0] = 2.f * cosf(a); p[1] = 0.5f + 0.5f * (float)(k % 5); p[2] = 2.f * sinf(a);
        }
        fprintf(f, "  {\"index\":%d,\"position\":[%g,%g,%g]}%s\n", idx, p[0], p[1], p[2], (k == n - 1) ? "" : ",");
    }
    fprintf(f, "] }\n");
    fclose(f);
    return 1;
}

/* A BARREL: 8 perimeter positions x 3 heights, no top or bottom cap — the CAVE array's real shape
 * (speakers mount in the band between the screen cube and the truss, so nothing covers the poles).
 * 24 speakers, 1.5 m radius, ear-height listener. */
static void make_barrel(Layout* L) {
    memset(L, 0, sizeof *L);
    const float rad = 1.5f, ys[3] = { 0.5f, 1.5f, 2.5f };
    uint32_t k = 0;
    for (int ri = 0; ri < 3; ++ri)
        for (int a = 0; a < 8; ++a, ++k) {
            const float th = (float)a * 0.785398163f;          /* 8 azimuths, 45 deg apart */
            L->speakers[k].pos[0] = rad * cosf(th);
            L->speakers[k].pos[1] = ys[ri];
            L->speakers[k].pos[2] = rad * sinf(th);
            L->speakers[k].gain_lin = 1.f;
        }
    L->count = k;
    layout_compute_ref(L);
    L->rolloff_r     = 0.7f;
    L->spcap_focus   = layout_derive_spcap_focus(L);
    L->spcap_density = BWA_SPCAP_DENSITY_DEFAULT;
    L->atten_ref_m   = 1.f;
    L->atten_rolloff = 1.f;
    L->atten_min_lin = 0.01f;
}

/* i-th of `n` directions on a Fibonacci sphere (near-uniform coverage, no pole clustering) */
static void fib_dir(int i, int n, float out[3]) {
    const float y  = 1.f - 2.f * ((float)i + 0.5f) / (float)n;
    const float r  = sqrtf(fmaxf(0.f, 1.f - y * y));
    const float th = (float)i * 2.39996323f;
    out[0] = r * cosf(th); out[1] = y; out[2] = r * sinf(th);
}

/* angle (degrees) from `u` to the nearest speaker seen from `lis` — what hole.c's floor reads */
static double nearest_speaker_deg(const Layout* L, const float lis[3], const float u[3]) {
    double best = -2.0;
    for (uint32_t k = 0; k < L->count; ++k) {
        float d[3];
        unit_dir(lis, L->speakers[k].pos, d);
        double c = (double)u[0]*d[0] + (double)u[1]*d[1] + (double)u[2]*d[2];
        if (c > best) best = c;
    }
    if (best >  1.0) best =  1.0;
    if (best < -1.0) best = -1.0;
    return acos(best) * 57.2957795;
}

/* ---- hull.c pins ----------------------------------------------------------------------------------
 * The ORACLE is the brute-force hull hull.c used to be (every triple against every point), kept here
 * verbatim so the replacement is checked against the thing it replaced. It is exact on a generic set
 * and wrong on a coplanar one (it emits every triple of a coplanar cap), which is why coplanar sets
 * get the validity checks instead. */
#define HT_MAXPTS (BWA_MAX_CHANNELS + 2)
#define HT_BIGTRI 8192
static float ho_dot(const float a[3], const float b[3]) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
static void  ho_cross(const float a[3], const float b[3], float o[3]) {
    o[0] = a[1]*b[2] - a[2]*b[1]; o[1] = a[2]*b[0] - a[0]*b[2]; o[2] = a[0]*b[1] - a[1]*b[0];
}
static float ho_triple(const float a[3], const float b[3], const float c[3]) {
    float bc[3]; ho_cross(b, c, bc); return ho_dot(a, bc);
}
static int hull_oracle(float (*dirs)[3], uint32_t n, int (*tri)[3], float* det, int maxtri) {
    int ntri = 0;
    for (uint32_t i = 0; i < n; ++i)
    for (uint32_t j = i + 1; j < n; ++j)
    for (uint32_t k = j + 1; k < n; ++k) {
        float e1[3] = { dirs[j][0]-dirs[i][0], dirs[j][1]-dirs[i][1], dirs[j][2]-dirs[i][2] };
        float e2[3] = { dirs[k][0]-dirs[i][0], dirs[k][1]-dirs[i][1], dirs[k][2]-dirs[i][2] };
        float nrm[3]; ho_cross(e1, e2, nrm);
        float nl = sqrtf(ho_dot(nrm, nrm));
        if (nl < 1e-9f) continue;
        nrm[0]/=nl; nrm[1]/=nl; nrm[2]/=nl;
        float off = ho_dot(nrm, dirs[i]);
        if (off < 0.f) { nrm[0]=-nrm[0]; nrm[1]=-nrm[1]; nrm[2]=-nrm[2]; off=-off; }
        int face = 1;
        for (uint32_t m = 0; m < n; ++m) if (ho_dot(nrm, dirs[m]) > off + 1e-5f) { face = 0; break; }
        if (face) {
            if (ntri >= maxtri) return 0;
            tri[ntri][0]=(int)i; tri[ntri][1]=(int)j; tri[ntri][2]=(int)k;
            det[ntri] = ho_triple(dirs[i], dirs[j], dirs[k]);
            ++ntri;
        }
    }
    return ntri;
}

/* Orientation-free canonical form: flip to positive det, rotate the lowest index first, sort. The
 * oracle emits sorted index triples whatever their orientation; hull.c emits outward ones. */
typedef struct { int v[3]; float det; } HtTri;
static int ht_cmp(const void* pa, const void* pb) {
    const HtTri *a = (const HtTri*)pa, *b = (const HtTri*)pb;
    for (int q = 0; q < 3; ++q) if (a->v[q] != b->v[q]) return a->v[q] < b->v[q] ? -1 : 1;
    return 0;
}
static void ht_canon(int (*tri)[3], const float* det, int nt, HtTri* out) {
    for (int t = 0; t < nt; ++t) {
        int v[3] = { tri[t][0], tri[t][1], tri[t][2] }; float d = det[t];
        if (d < 0.f) { int x = v[1]; v[1] = v[2]; v[2] = x; d = -d; }
        int r = (v[0] < v[1] && v[0] < v[2]) ? 0 : (v[1] < v[2] ? 1 : 2);
        for (int q = 0; q < 3; ++q) out[t].v[q] = v[(r + q) % 3];
        out[t].det = d;
    }
    qsort(out, (size_t)nt, sizeof *out, ht_cmp);
}

/* In general position: no point other than a face's own three within 1e-4 of its plane, and the
 * origin not within 1e-4 of it either. Judged on the ORACLE's faces, so hull.c does not grade
 * its own input. */
static int ht_generic(float (*dirs)[3], uint32_t n, int (*tri)[3], int nt) {
    for (int t = 0; t < nt; ++t) {
        const float *a = dirs[tri[t][0]], *b = dirs[tri[t][1]], *c = dirs[tri[t][2]];
        float e1[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] }, e2[3] = { c[0]-a[0], c[1]-a[1], c[2]-a[2] }, nr[3];
        ho_cross(e1, e2, nr);
        float l = sqrtf(ho_dot(nr, nr)); nr[0] /= l; nr[1] /= l; nr[2] /= l;
        float off = ho_dot(nr, a);
        if (fabsf(off) < 1e-4f) return 0;
        for (uint32_t m = 0; m < n; ++m) {
            if ((int)m == tri[t][0] || (int)m == tri[t][1] || (int)m == tri[t][2]) continue;
            if (fabsf(ho_dot(nr, dirs[m]) - off) < 1e-4f) return 0;
        }
    }
    return 1;
}

/* hull.c against the oracle as a face SET, with dets. Returns 1 on a match. */
static int ht_match(float (*dirs)[3], uint32_t n, const char* name) {
    static int to[HT_BIGTRI][3], tn[VBAP_MAXTRI][3];
    static float dO[HT_BIGTRI], dN[VBAP_MAXTRI];
    static HtTri co[HT_BIGTRI], cn[VBAP_MAXTRI];
    int no = hull_oracle(dirs, n, to, dO, HT_BIGTRI);
    int nn = hull_triangulate(dirs, n, tn, dN, VBAP_MAXTRI);
    if (no <= 0 || nn != no) { printf("  hull %s (n=%u): %d faces vs oracle %d\n", name, n, nn, no); return 0; }
    ht_canon(to, dO, no, co); ht_canon(tn, dN, nn, cn);
    for (int t = 0; t < no; ++t) {
        if (ht_cmp(&co[t], &cn[t]) != 0 || fabsf(co[t].det - cn[t].det) > 1e-5f) {
            printf("  hull %s (n=%u): face %d differs (%d %d %d / %g vs %d %d %d / %g)\n", name, n, t,
                   cn[t].v[0], cn[t].v[1], cn[t].v[2], cn[t].det, co[t].v[0], co[t].v[1], co[t].v[2], co[t].det);
            return 0;
        }
    }
    return 1;
}

/* A valid triangulated hull, for sets the oracle gets wrong: every point on or inside every face
 * plane, no more than 2n-4 faces, no sliver, outward faces with det > 0, every point a vertex, each
 * directed edge used once and its reverse once (a closed manifold), and hull_vbap solving 1000
 * random bearings to finite, non-negative, unit-power gains. */
static int ht_valid_ex(float (*dirs)[3], uint32_t n, const char* name, uint32_t seed, int all_vertices) {
    static int tri[VBAP_MAXTRI][3]; static float det[VBAP_MAXTRI];
    static unsigned char edge[HT_MAXPTS][HT_MAXPTS];
    int nt = hull_triangulate(dirs, n, tri, det, VBAP_MAXTRI);
    int ok = 1;
#define HT_BAD(...) do { printf("  hull %s (n=%u): ", name, n); printf(__VA_ARGS__); printf("\n"); ok = 0; } while (0)
    if (nt <= 0) { HT_BAD("did not triangulate"); return 0; }
    if (nt > 2 * (int)n - 4) HT_BAD("%d faces > 2n-4", nt);
    memset(edge, 0, sizeof edge);
    int used[HT_MAXPTS] = { 0 };
    for (int t = 0; t < nt; ++t) {
        const float *a = dirs[tri[t][0]], *b = dirs[tri[t][1]], *c = dirs[tri[t][2]];
        float e1[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] }, e2[3] = { c[0]-a[0], c[1]-a[1], c[2]-a[2] }, nr[3];
        ho_cross(e1, e2, nr);
        float l = sqrtf(ho_dot(nr, nr));
        if (l < 1e-6f) { HT_BAD("face %d is a sliver (|cross| %g)", t, l); continue; }
        nr[0] /= l; nr[1] /= l; nr[2] /= l;
        float off = ho_dot(nr, a);
        if (!(det[t] > 0.f) || fabsf(det[t] - ho_triple(a, b, c)) > 1e-6f) HT_BAD("face %d det %g not outward", t, det[t]);
        for (uint32_t m = 0; m < n; ++m)
            if (ho_dot(nr, dirs[m]) > off + 2e-5f) HT_BAD("point %u outside face %d by %g", m, t, ho_dot(nr, dirs[m]) - off);
        for (int q = 0; q < 3; ++q) { ++edge[tri[t][q]][tri[t][(q + 1) % 3]]; used[tri[t][q]] = 1; }
    }
    for (uint32_t i = 0; i < n; ++i) {
        if (!used[i] && all_vertices) HT_BAD("point %u is not a vertex", i);
        for (uint32_t j = 0; j < n; ++j) {
            if (edge[i][j] > 1) HT_BAD("edge %u->%u used %d times", i, j, edge[i][j]);
            if (edge[i][j] != edge[j][i]) HT_BAD("edge %u-%u is open (not a closed manifold)", i, j);
        }
    }
    for (int r = 0; r < 1000; ++r) {
        float d[3], l2;
        do { d[0] = lcg_noise(&seed); d[1] = lcg_noise(&seed); d[2] = lcg_noise(&seed); l2 = d[0]*d[0] + d[1]*d[1] + d[2]*d[2]; }
        while (l2 < 1e-4f || l2 > 1.f);
        float il = 1.f / sqrtf(l2); d[0] *= il; d[1] *= il; d[2] *= il;
        int spk[3]; float g[3];
        if (!hull_vbap(d, dirs, tri, det, nt, spk, g)) { HT_BAD("hull_vbap failed a bearing"); break; }
        float p = g[0]*g[0] + g[1]*g[1] + g[2]*g[2];
        if (!isfinite(p) || g[0] < 0.f || g[1] < 0.f || g[2] < 0.f || fabsf(p - 1.f) > 1e-4f) {
            HT_BAD("bad gains %g %g %g", g[0], g[1], g[2]); break; }
    }
#undef HT_BAD
    return ok;
}
static int ht_valid(float (*dirs)[3], uint32_t n, const char* name, uint32_t seed) {
    return ht_valid_ex(dirs, n, name, seed, 1);
}

/* A fixed set is compared to the oracle when it is in general position and checked for validity
 * when it is not; the caller learns which, so a set cannot silently drop out of the comparison. */
static int ht_check(float (*dirs)[3], uint32_t n, const char* name, uint32_t seed, int* compared) {
    static int to[HT_BIGTRI][3]; static float dO[HT_BIGTRI];
    int no = hull_oracle(dirs, n, to, dO, HT_BIGTRI);
    if (no > 0 && ht_generic(dirs, n, to, no)) { ++*compared; return ht_match(dirs, n, name); }
    printf("hull: %s (n=%u) is not in general position (oracle %d faces): validity only\n", name, n, no);
    return ht_valid(dirs, n, name, seed);
}

static void ht_norm(float v[3]) { float l = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]); v[0] /= l; v[1] /= l; v[2] /= l; }

/* The 64-speaker barrel: 4 rings of 16 around a centered listener, so the top and bottom rings are
 * coplanar caps. `stagger` offsets alternate rings by half a step. */
static void ht_barrel64(float (*d)[3], int stagger) {
    const float ys[4] = { -1.2f, -0.4f, 0.4f, 1.2f };
    int k = 0;
    for (int r = 0; r < 4; ++r)
        for (int a = 0; a < 16; ++a, ++k) {
            const float th = (float)a * 6.2831853f / 16.f + (stagger && (r & 1) ? 0.19634954f : 0.f);
            d[k][0] = 2.f * cosf(th); d[k][1] = ys[r]; d[k][2] = 2.f * sinf(th); ht_norm(d[k]);
        }
}

int main(void) {
    /* 1. default layout */
    static Layout LD; layout_default(&LD);
    CHECK(LD.count == CH, "default layout has 26 speakers");

    /* 2. layout_load parse */
    const char* LJ = "bwa_layout.json";
    CHECK(write_layout_json(LJ), "write layout json");
    char err[256] = {0};
    static Layout L;
    CHECK(layout_load(LJ, RATE, &L, err, sizeof err), err[0] ? err : "layout_load");
    CHECK(L.count == CH, "loaded 26 speakers");
    CHECK(fabs(L.rolloff_r - 0.7) < 1e-5, "parsed dbap.rolloff_r");
    CHECK(fabs(L.speakers[5].gain_lin - powf(10.f, -6.f / 20.f)) < 1e-4, "parsed gain_db -> linear");
    CHECK(L.speakers[9].delay_samples == (uint32_t)(1.0 * 1e-3 * RATE + 0.5), "parsed delay_ms -> samples");
    CHECK(L.max_delay_samples == L.speakers[9].delay_samples, "max_delay_samples");

    /* 3. DBAP localization (centered listener): source at speaker k -> channel k dominates */
    {
        float lis[3] = { LD.ref[0], LD.ref[1], LD.ref[2] }, g[CH];   /* the array center (floor origin) */
        int ok = 1;
        for (int k = 0; k < CH; ++k) {
            dbap_gains(LD.speakers[k].pos, lis, &LD, 1.0f, g);
            if (argmax(g, CH) != k) ok = 0;
        }
        CHECK(ok, "DBAP localizes a source at each speaker to that channel");
    }

    /* 4. constant power: ||g|| ~ user_gain (atten == 1 within the reference distance) */
    {
        float lis[3] = { LD.ref[0], LD.ref[1], LD.ref[2] };
        float src[3] = { 0.5f, LD.ref[1], 0.5f }, g[CH];             /* ds ~ 0.7 m < ref -> atten = 1 */
        float gain = 0.8f;
        dbap_gains(src, lis, &LD, gain, g);
        double p = 0; for (int k = 0; k < CH; ++k) p += (double)g[k] * g[k];
        CHECK(fabs(sqrt(p) - gain) < 0.02, "DBAP is constant-power (||g|| ~ user_gain)");
    }

    /* 5. two-speaker split: a source midway between two speakers feeds both above average */
    {
        const int A = 7, B = 8;
        float lis[3] = { LD.ref[0], LD.ref[1], LD.ref[2] }, g[CH];
        float mid[3] = { (LD.speakers[A].pos[0] + LD.speakers[B].pos[0]) * 0.5f,
                         (LD.speakers[A].pos[1] + LD.speakers[B].pos[1]) * 0.5f,
                         (LD.speakers[A].pos[2] + LD.speakers[B].pos[2]) * 0.5f };
        dbap_gains(mid, lis, &LD, 1.0f, g);
        double avg = 0; for (int k = 0; k < CH; ++k) avg += g[k];
        avg /= CH;
        CHECK(g[A] > avg && g[B] > avg, "source between two speakers feeds both above average");
    }

    /* 6. listener move changes the distribution */
    {
        float src[3] = { 1.5f, LD.ref[1], 0.f }, g0[CH], g1[CH];
        float lis0[3] = { 0, LD.ref[1], 0 }, lis1[3] = { -1.0f, LD.ref[1], 0 };
        dbap_gains(src, lis0, &LD, 1.0f, g0);
        dbap_gains(src, lis1, &LD, 1.0f, g1);
        double diff = 0; for (int k = 0; k < CH; ++k) diff += fabs(g0[k] - g1[k]);
        CHECK(diff > 1e-3, "moving the listener changes the gain distribution");
    }

    /* 6b. moving-listener localization (the headline feature): a source co-located with speaker k
     *     localizes to channel k from ANY listener position, because DBAP is listener-relative — the
     *     source and speaker k share a bearing from every point. A sign error in the listener-relative
     *     direction would pull the image to the OPPOSITE speaker, which the centered test 3 can't see. */
    {
        float lis[3] = { -1.0f, LD.ref[1] - 0.4f, 0.6f }, g[CH];   /* off-center AND off the ear plane */
        int ok = 1;
        for (int k = 0; k < CH; ++k) {
            dbap_gains(LD.speakers[k].pos, lis, &LD, 1.0f, g);
            if (argmax(g, CH) != k) ok = 0;
        }
        CHECK(ok, "DBAP localizes a source at each speaker to that channel from an off-center listener");
    }

    /* 6c. boundary-crossing continuity + injectivity: hull-projection DBAP has two documented
     *     failure modes outside the array (Sundstrom 2021, I3DA): gains become non-unique when the
     *     projection lands on a hull vertex, and total power "undulates wildly" across the boundary.
     *     The engine's formulation is hull-free, so a source swept from the center out through a
     *     corner speaker and beyond must give (a) per-speaker gains continuous in position, (b) a
     *     total level monotone non-increasing past the reference distance, and (c) distinct gain
     *     vectors for distinct positions (no exterior collapse). The ray passes THROUGH the corner
     *     speaker — the hull-vertex case — and runs to ~4x the array radius. */
    {
        const float* corner = LD.speakers[0].pos;                    /* a corner of the grid */
        float lis[3] = { LD.ref[0], LD.ref[1], LD.ref[2] };
        float dir[3] = { corner[0] - lis[0], corner[1] - lis[1], corner[2] - lis[2] };
        float dl = sqrtf(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
        for (int c = 0; c < 3; ++c) dir[c] /= dl;
        const float step = 0.01f;                                    /* 1 cm */
        float gprev[CH], g[CH];
        double pprev = -1.0;
        int cont_ok = 1, mono_ok = 1, inj_ok = 1;
        /* start at i=1: at src == lis the bearing is undefined and the solve goes omnidirectional
         * (documented in spatialization.md) — a genuine discontinuity AT the listener, excluded
         * here; near-listener widening is the feature that covers fly-throughs. */
        for (int i = 1; i <= 1000; ++i) {                            /* 1 cm .. 10 m along the ray */
            float t = step * (float)i;
            float src[3] = { lis[0] + dir[0]*t, lis[1] + dir[1]*t, lis[2] + dir[2]*t };
            dbap_gains(src, lis, &LD, 1.0f, g);
            double p = 0; for (int k = 0; k < CH; ++k) p += (double)g[k] * g[k];
            p = sqrt(p);
            if (i > 1) {
                double dmax = 0, dsum = 0;
                for (int k = 0; k < CH; ++k) {
                    double d = fabs((double)g[k] - gprev[k]);
                    if (d > dmax) dmax = d;
                    dsum += d;
                }
                if (dmax > 0.02) cont_ok = 0;                        /* no per-cm gain jump */
                if (t > LD.atten_ref_m + step && p > pprev + 1e-6) mono_ok = 0;
                if (t < 4.0f && dsum < 1e-7) inj_ok = 0;             /* interior+near field: still injective */
            }
            memcpy(gprev, g, sizeof gprev);
            pprev = p;
        }
        CHECK(cont_ok, "DBAP gains are continuous across the array boundary (no undulation)");
        CHECK(mono_ok, "DBAP total level is monotone non-increasing past the reference distance");
        CHECK(inj_ok,  "DBAP gain vectors stay distinct as an exterior source recedes");
    }

    /* 7. align: gain trim halves a channel; delay shifts the impulse */
    {
        static Layout AL; layout_default(&AL);
        AL.speakers[2].gain_lin = 0.5f;
        AL.speakers[3].delay_samples = 4;
        AL.max_delay_samples = 4;
        Aligner* a = align_create(CH, &AL, RATE);
        CHECK(a != NULL, "align_create");
        if (a) {
            const uint32_t n = 16;
            float buf[CH * 16];
            memset(buf, 0, sizeof buf);
            buf[0 * n + 0] = 1.0f;             /* ch0: gain 1, delay 0 (passthrough) */
            buf[2 * n + 0] = 1.0f;             /* ch2: gain 0.5 */
            buf[3 * n + 0] = 1.0f;             /* ch3: delay 4 */
            align_process(a, buf, n);
            CHECK(fabs(buf[0 * n + 0] - 1.0f) < 1e-6, "passthrough channel unchanged");
            CHECK(fabs(buf[2 * n + 0] - 0.5f) < 1e-6, "gain trim 0.5 halves the channel");
            CHECK(fabs(buf[3 * n + 0]) < 1e-6 && fabs(buf[3 * n + 4] - 1.0f) < 1e-6,
                  "delay shifts the impulse by 4 samples");
            align_destroy(a);
        }
    }

    /* 7a. per-speaker correction FIR: a channel's kernel convolves its signal (before gain+delay) */
    {
        static Layout EQ; layout_default(&EQ);
        EQ.speakers[5].eq_len = 3;
        EQ.speakers[5].eq[0] = 0.5f; EQ.speakers[5].eq[1] = 0.25f; EQ.speakers[5].eq[2] = -0.1f;
        Aligner* a = align_create(CH, &EQ, RATE);
        CHECK(a != NULL, "align_create (eq)");
        if (a) {
            const uint32_t n = 16;
            float buf[CH * 16];
            memset(buf, 0, sizeof buf);
            buf[5 * n + 0] = 1.0f;             /* impulse on the EQ'd channel (gain 1, delay 0) */
            buf[1 * n + 0] = 1.0f;             /* a non-EQ channel passes through */
            align_process(a, buf, n);
            CHECK(fabs(buf[5*n+0] - 0.5f)  < 1e-6 &&
                  fabs(buf[5*n+1] - 0.25f) < 1e-6 &&
                  fabs(buf[5*n+2] + 0.1f)  < 1e-6, "correction FIR convolves the channel with its kernel");
            CHECK(fabs(buf[1*n+0] - 1.0f) < 1e-6, "a channel with no correction passes through");
            align_destroy(a);
        }
    }

    /* 7a2. room_eq modal cut: a -6 dB peaking section at 100 Hz attenuates a 100 Hz tone by ~6 dB
     * and leaves 1 kHz (and other channels) alone. Steady-state RMS over the tail (filter settled). */
    {
        static Layout RQ; layout_default(&RQ);
        RQ.speakers[4].room_eq_count = 1;
        RQ.speakers[4].room_eq[0].fc = 100.f; RQ.speakers[4].room_eq[0].gain_db = -6.f; RQ.speakers[4].room_eq[0].q = 2.f;
        Aligner* a = align_create(CH, &RQ, RATE);
        CHECK(a != NULL, "align_create (room_eq)");
        if (a) {
            enum { NN = 48000 };
            static float buf[CH * NN];
            memset(buf, 0, sizeof buf);
            for (int i = 0; i < NN; ++i) {
                buf[4 * (size_t)NN + i] = sinf(2.f * 3.14159265f * 100.f  * i / (float)RATE);
                buf[6 * (size_t)NN + i] = sinf(2.f * 3.14159265f * 100.f  * i / (float)RATE);   /* no room_eq */
                buf[7 * (size_t)NN + i] = sinf(2.f * 3.14159265f * 1000.f * i / (float)RATE);
            }
            align_process(a, buf, NN);
            double r4 = 0, r6 = 0, r7 = 0;
            for (int i = NN / 2; i < NN; ++i) {                     /* settled tail only */
                r4 += (double)buf[4*(size_t)NN+i] * buf[4*(size_t)NN+i];
                r6 += (double)buf[6*(size_t)NN+i] * buf[6*(size_t)NN+i];
                r7 += (double)buf[7*(size_t)NN+i] * buf[7*(size_t)NN+i];
            }
            double att_db = 10.0 * log10(r4 / r6);                  /* cut channel vs untouched at fc */
            CHECK(att_db < -5.0 && att_db > -7.0, "room_eq section cuts ~6 dB at its center frequency");
            CHECK(fabs(10.0 * log10(r7 / (0.5 * (NN / 2)))) < 0.6,  "room_eq leaves off-fc content alone");
            align_destroy(a);
        }
    }

    /* 7a3. tracked room EQ (room_eq_grid ladder): align SLEWS each section toward the targets set by
     * align_room_eq_targets (24 dB/s) — a -12 dB target glides in (the first 0.25 s window sits
     * mid-slew, not already cut), lands at depth, and releases back to flat. Windowed RMS of a 100 Hz
     * tone on the cut channel, block-sized processing like the audio thread. */
    {
        static Layout G; layout_default(&G);
        G.rq_grid.npos = 1;              /* align only reads the ladder — rt.c owns the interpolation */
        G.rq_grid.nsec[4]  = 1;
        G.rq_grid.fc[4][0] = 100.f; G.rq_grid.q[4][0] = 2.f;
        Aligner* a = align_create(CH, &G, RATE);
        CHECK(a != NULL, "align_create (room_eq_grid)");
        if (a) {
            enum { BL = 256, WIN = 47 };                      /* 47 blocks ~ 0.25 s per window */
            static float blk[CH * BL];
            float tgt[BWA_CHANNELS][BWA_ROOM_EQ_MAX];
            memset(tgt, 0, sizeof tgt);
            int ph = 0;
            #define GRID_WIN_DB(out) do {                                                        \
                double r_ = 0;                                                                   \
                for (int b_ = 0; b_ < WIN; ++b_) {                                               \
                    memset(blk, 0, sizeof blk);                                                  \
                    for (int i_ = 0; i_ < BL; ++i_, ++ph)                                        \
                        blk[4*(size_t)BL + i_] = sinf(2.f*3.14159265f*100.f*(float)ph/(float)RATE); \
                    align_process(a, blk, BL);                                                   \
                    for (int i_ = 0; i_ < BL; ++i_) r_ += (double)blk[4*(size_t)BL+i_]*blk[4*(size_t)BL+i_]; \
                }                                                                                \
                (out) = 10.0 * log10(r_ / (0.5 * WIN * BL));                                     \
            } while (0)
            tgt[4][0] = -12.f;
            align_room_eq_targets(a, tgt);
            double w0, wlast;
            GRID_WIN_DB(w0);                                  /* mid-glide: ~ -3 dB average */
            for (int wn = 0; wn < 7; ++wn) GRID_WIN_DB(wlast); /* by 2 s: fully landed */
            CHECK(w0 > -9.0,                     "tracked cut glides in (first window is mid-slew, not a step)");
            CHECK(wlast > -13.0 && wlast < -11.0, "tracked cut lands at its target depth");
            memset(tgt, 0, sizeof tgt);
            align_room_eq_targets(a, tgt);                    /* release */
            for (int wn = 0; wn < 4; ++wn) GRID_WIN_DB(wlast);
            CHECK(fabs(wlast) < 1.0,             "tracked cut releases back to flat");
            #undef GRID_WIN_DB
            align_destroy(a);
        }
    }

    /* 7a4. tracked listener alignment (align_tracked_targets / align_tracked_slew): an EXTRA
     * fractional delay + gain per channel on top of the layout trims. Checked here at the DSP level:
     * OFF is bit-identical to an aligner that was never told about the feature, an engaged target
     * lands where it was aimed (fraction included), the rate limit bounds the per-block delay change,
     * and a gliding tap does not click. rt.c owns turning a listener position into these targets. */
    {
        static Layout T; layout_default(&T);                  /* unity gains, zero delays: align is identity */
        Aligner* a  = align_create(CH, &T, RATE);
        Aligner* rf = align_create(CH, &T, RATE);     /* control: never told about the feature */
        CHECK(a != NULL && rf != NULL, "align_create (tracked align)");
        if (a && rf) {
            enum { BL = 256 };
            static float blk[CH * BL], blk2[CH * BL];
            float tgt[BWA_CHANNELS], gtg[BWA_CHANNELS], dst[BWA_CHANNELS], gst[BWA_CHANNELS];
            for (int k = 0; k < CH; ++k) { tgt[k] = 0.f; gtg[k] = 1.f; }
            uint32_t rs = 12345;
            /* (a) off: the knobs are set, the targets are identity — every sample must match the
             * control aligner bit for bit (this is the "default costs nothing" guarantee). */
            align_tracked_slew(a, 96.f);
            align_tracked_targets(a, NULL, NULL);
            int bitsame = 1;
            for (int b = 0; b < 8; ++b) {
                for (int i = 0; i < CH * BL; ++i) { blk[i] = 0.25f * lcg_noise(&rs); blk2[i] = blk[i]; }
                align_process(a, blk, BL);
                align_process(rf, blk2, BL);
                if (memcmp(blk, blk2, sizeof blk) != 0) bitsame = 0;
            }
            CHECK(bitsame, "tracked align off: output is bit-identical to an aligner without it");
            align_destroy(rf);

            /* flush the rings, then land a 10-frame comp on channel 3 with the rate limit wide open */
            memset(blk, 0, sizeof blk);
            for (int b = 0; b < 4; ++b) { memset(blk, 0, sizeof blk); align_process(a, blk, BL); }
            align_tracked_slew(a, 20000.f);
            tgt[3] = 10.f;
            align_tracked_targets(a, tgt, gtg);
            for (int b = 0; b < 8; ++b) { memset(blk, 0, sizeof blk); align_process(a, blk, BL); }
            align_tracked_state(a, dst, gst);
            CHECK(fabs(dst[3] - 10.0) < 1e-4, "tracked align: the comp delay lands on its target");
            memset(blk, 0, sizeof blk);
            blk[3 * BL + 0] = 1.0f;                   /* impulse on the displaced channel */
            blk[1 * BL + 0] = 1.0f;                   /* and on an untouched one */
            align_process(a, blk, BL);
            printf("tracked align: 10-frame target -> peak at frame %d (want 10)\n", argmax_abs(&blk[3*BL], 32));
            CHECK(fabs(blk[3*BL + 10] - 1.0f) < 1e-5 && fabs(blk[3*BL + 0]) < 1e-6,
                  "tracked align: an integer comp delay shifts the impulse by exactly that many frames");
            CHECK(fabs(blk[1*BL + 0] - 1.0f) < 1e-6,
                  "tracked align: a channel with no comp still passes through untouched");

            /* (b) fractional target: linear interpolation splits the impulse across two frames */
            tgt[3] = 4.25f;
            align_tracked_targets(a, tgt, gtg);
            for (int b = 0; b < 8; ++b) { memset(blk, 0, sizeof blk); align_process(a, blk, BL); }
            memset(blk, 0, sizeof blk);
            blk[3 * BL + 0] = 1.0f;
            align_process(a, blk, BL);
            printf("tracked align: 4.25-frame target -> taps %.3f / %.3f (want 0.750 / 0.250)\n",
                   blk[3*BL + 4], blk[3*BL + 5]);
            CHECK(fabs(blk[3*BL + 4] - 0.75f) < 1e-5 && fabs(blk[3*BL + 5] - 0.25f) < 1e-5,
                  "tracked align: a fractional comp delay interpolates between the two taps");

            /* (c) extra gain: a 0.5 target halves the channel once landed */
            gtg[3] = 0.5f;
            align_tracked_targets(a, tgt, gtg);       /* gain slews at 4/s: 0.5 needs ~0.125 s */
            for (int b = 0; b < 40; ++b) { memset(blk, 0, sizeof blk); align_process(a, blk, BL); }
            memset(blk, 0, sizeof blk);
            blk[3 * BL + 0] = 1.0f;
            align_process(a, blk, BL);
            CHECK(fabs((blk[3*BL+4] + blk[3*BL+5]) - 0.5f) < 1e-5,
                  "tracked align: the comp gain scales the channel");
            gtg[3] = 1.f;

            /* (d) rate limit: with the delay ceiling at 64 frames/s, ONE 256-frame block may move the
             * tap by at most 64 * 256 / 48000 = 0.3413 frames, however far away the target is. */
            tgt[3] = 0.f;
            align_tracked_targets(a, tgt, gtg);
            for (int b = 0; b < 40; ++b) { memset(blk, 0, sizeof blk); align_process(a, blk, BL); }
            align_tracked_slew(a, 64.f);
            tgt[3] = 400.f;                           /* far out of reach in one block */
            align_tracked_targets(a, tgt, gtg);
            memset(blk, 0, sizeof blk);
            align_process(a, blk, BL);
            align_tracked_state(a, dst, gst);
            double want = 64.0 * BL / (double)RATE;
            printf("tracked align: rate limit moved the tap %.4f frames in one 256-frame block (want %.4f)\n",
                   dst[3], want);
            CHECK(fabs(dst[3] - want) < 1e-4, "tracked align: the rate limit bounds the per-block delay change");
            double per_blk_max = 0, prev = dst[3];
            for (int b = 0; b < 200; ++b) {
                memset(blk, 0, sizeof blk); align_process(a, blk, BL);
                align_tracked_state(a, dst, gst);
                if (dst[3] - prev > per_blk_max) per_blk_max = dst[3] - prev;
                prev = dst[3];
            }
            CHECK(per_blk_max <= want + 1e-4, "tracked align: no block ever exceeds the rate limit");

            /* (e) no click: a 500 Hz tone through a tap gliding at an aggressive 4096 frames/s stays
             * continuous — the biggest sample-to-sample step must stay near the tone's own max slope
             * (2*pi*500/48000 = 0.0654 for unit amplitude), not jump. */
            tgt[3] = 0.f;
            align_tracked_targets(a, tgt, gtg);
            for (int b = 0; b < 600; ++b) { memset(blk, 0, sizeof blk); align_process(a, blk, BL); }
            align_tracked_slew(a, 4096.f);
            tgt[3] = 300.f;
            align_tracked_targets(a, tgt, gtg);
            double maxstep = 0; int ph = 0, finite = 1; float last = 0.f;
            for (int b = 0; b < 60; ++b) {
                memset(blk, 0, sizeof blk);
                for (int i = 0; i < BL; ++i, ++ph)
                    blk[3 * BL + i] = sinf(2.f * 3.14159265f * 500.f * (float)ph / (float)RATE);
                align_process(a, blk, BL);
                for (int i = 0; i < BL; ++i) {
                    float v = blk[3 * BL + i];
                    if (!(v > -2.f && v < 2.f)) finite = 0;
                    if (b > 2) { double s = fabs((double)v - last); if (s > maxstep) maxstep = s; }
                    last = v;
                }
            }
            printf("tracked align: max sample step while gliding = %.5f (tone slope 0.0654)\n", maxstep);
            CHECK(finite, "tracked align: the gliding tap stays bounded (no NaN, no blowup)");
            CHECK(maxstep < 0.08, "tracked align: a gliding comp delay does not click");

            /* (f) release: aimed back at identity it slews home and the aligner drops back to the
             * integer path (state exactly 0 / 1, so the bit-identical guarantee is restored). */
            align_tracked_slew(a, 20000.f);
            align_tracked_targets(a, NULL, NULL);
            for (int b = 0; b < 8; ++b) { memset(blk, 0, sizeof blk); align_process(a, blk, BL); }
            align_tracked_state(a, dst, gst);
            CHECK(dst[3] == 0.f && gst[3] == 1.f, "tracked align: aiming at identity returns exactly home");
            align_destroy(a);
        } else if (rf) align_destroy(rf);
    }

    /* 7b. layout_load rejects out-of-range values (so bad JSON can't reach the audio thread) */
    {
        const char* BJ = "bwa_bad_layout.json";
        static Layout B;
        write_layout_with(BJ, 0.0, 0.0, 0.0);     CHECK(!layout_load(BJ, RATE, &B, err, sizeof err), "rolloff_r=0 is rejected");
        write_layout_with(BJ, 0.7, 1000.0, 0.0);  CHECK(!layout_load(BJ, RATE, &B, err, sizeof err), "gain_db=1000 is rejected");
        write_layout_with(BJ, 0.7, 0.0, 1.0e7);   CHECK(!layout_load(BJ, RATE, &B, err, sizeof err), "huge delay_ms is rejected");
        write_layout_with(BJ, 0.7, 0.0, 0.0);     CHECK( layout_load(BJ, RATE, &B, err, sizeof err), "valid layout still loads");
        remove(BJ);
    }

    /* 7b2. room_eq_grid schema: parses into the shared ladder + per-position depths; positions must
     * agree on the ladder (depths interpolate by index); static room_eq + grid don't mix. */
    {
        const char* GJ = "bwa_grid_layout.json";
        static Layout B;
        write_layout_grid(GJ, 0);
        CHECK(layout_load(GJ, RATE, &B, err, sizeof err), err[0] ? err : "room_eq_grid layout loads");
        CHECK(B.rq_grid.npos == 2, "room_eq_grid has both positions");
        CHECK(B.rq_grid.nsec[0] == 1 && B.rq_grid.nsec[1] == 0, "ladder sizes parsed per speaker");
        CHECK(fabs(B.rq_grid.fc[0][0] - 45.f) < 1e-3 && fabs(B.rq_grid.q[0][0] - 6.f) < 1e-3,
              "ladder fc/q parsed");
        CHECK(fabs(B.rq_grid.gain_db[0][0][0] + 8.f) < 1e-3 && fabs(B.rq_grid.gain_db[1][0][0] + 4.f) < 1e-3,
              "per-position depths parsed");
        CHECK(fabs(B.rq_grid.pos[1][0] - 1.f) < 1e-3, "grid positions parsed");
        write_layout_grid(GJ, 1);
        CHECK(!layout_load(GJ, RATE, &B, err, sizeof err), "a ladder mismatch across positions is rejected");
        write_layout_grid(GJ, 2);
        CHECK(!layout_load(GJ, RATE, &B, err, sizeof err), "room_eq + room_eq_grid together are rejected");
        remove(GJ);
    }

    /* 7b3. runtime channel count: the loader accepts 4..BWA_MAX_CHANNELS speakers whose indices form a complete
     * 0..N-1 permutation — the engine's channel count follows the file (BWA_CHANNELS is the cap). */
    {
        const char* NJ = "bwa_n_layout.json";
        static Layout B;
        write_layout_n(NJ, 24, -1);
        CHECK(layout_load(NJ, RATE, &B, err, sizeof err), err[0] ? err : "a 24-speaker layout loads");
        CHECK(B.count == 24, "count follows the file");
        {   /* no dbap block in the file -> the blur derives from the geometry:
             * r = 0.25 x the mean centroid->speaker distance (Sundstrom 2021) */
            double s = 0;
            for (uint32_t k = 0; k < B.count; ++k) {
                double dx = B.speakers[k].pos[0] - B.ref[0], dy = B.speakers[k].pos[1] - B.ref[1],
                       dz = B.speakers[k].pos[2] - B.ref[2];
                s += sqrt(dx * dx + dy * dy + dz * dz);
            }
            CHECK(fabs(B.rolloff_r - 0.25 * s / B.count) < 1e-5,
                  "omitted rolloff_r derives from the mean centroid->speaker distance");
        }
        write_layout_n(NJ, 3, -1);
        CHECK(!layout_load(NJ, RATE, &B, err, sizeof err), "fewer than 4 speakers is rejected");
        write_layout_n(NJ, BWA_MAX_CHANNELS, -1);
        CHECK(layout_load(NJ, RATE, &B, err, sizeof err), err[0] ? err : "a capacity-sized layout loads");
        CHECK(B.count == BWA_MAX_CHANNELS, "a capacity-sized layout keeps every speaker");
        write_layout_n(NJ, BWA_MAX_CHANNELS + 1, -1);
        err[0] = 0;
        CHECK(!layout_load(NJ, RATE, &B, err, sizeof err), "more than BWA_MAX_CHANNELS speakers is rejected");
        {   /* the message names the real cap, not a stale literal */
            char want[32];
            snprintf(want, sizeof want, "4..%d entries", (int)BWA_MAX_CHANNELS);
            CHECK(strstr(err, want) != NULL, "the over-capacity error names BWA_MAX_CHANNELS");
        }
        write_layout_n(NJ, 24, 24);                       /* index 24 in a 24-speaker file: gap at 0 */
        CHECK(!layout_load(NJ, RATE, &B, err, sizeof err), "an index outside 0..N-1 is rejected");
        remove(NJ);
    }

    /* 8. the committed example layout parses with this loader (schema-vs-parser integration) */
    {
        const char* cands[] = { "examples/cave_layout.json", "../examples/cave_layout.json",
                                "../../examples/cave_layout.json", "../../../examples/cave_layout.json" };
        static Layout EX;
        int found = 0;
        for (size_t i = 0; i < sizeof cands / sizeof cands[0]; ++i) {
            FILE* probe = fopen(cands[i], "rb");
            if (probe) {
                fclose(probe);
                found = 1;
                CHECK(layout_load(cands[i], RATE, &EX, err, sizeof err), err[0] ? err : "load examples/cave_layout.json");
                CHECK(EX.count == CH, "examples/cave_layout.json has 26 speakers");
                break;
            }
        }
        if (!found) printf("note: examples/cave_layout.json not found from CWD; integration check skipped\n");
    }

    /* 9. SPCAP panner: localization, constant power, multi-speaker spread, non-negative gains */
    {
        SpcapState sp; spcap_reset(&sp);
        float lis[3] = { LD.ref[0], LD.ref[1], LD.ref[2] }, g[CH];   /* sweet spot = the array center */
        int loc_ok = 1, nonneg = 1;
        for (int k = 0; k < CH; ++k) {
            spcap_gains(&sp, LD.speakers[k].pos, lis, &LD, 1u, LD.spcap_focus, LD.spcap_density,
                        1.0f, g);                                          /* source at speaker k's bearing */
            if (argmax(g, CH) != k) loc_ok = 0;
            for (int i = 0; i < CH; ++i) if (g[i] < -1e-6f) nonneg = 0;
        }
        CHECK(loc_ok, "SPCAP localizes a source at each speaker to that channel");
        CHECK(nonneg, "SPCAP gains are non-negative");

        float src[3] = { 0.5f, LD.ref[1], 0.5f }, gain = 0.8f;
        spcap_gains(&sp, src, lis, &LD, 1u, LD.spcap_focus, LD.spcap_density, gain, g);
        double p = 0; int active = 0;
        for (int k = 0; k < CH; ++k) { p += (double)g[k] * g[k]; if (g[k] > 0.05f * gain) ++active; }
        CHECK(fabs(sqrt(p) - gain) < 0.02, "SPCAP is constant-power (||g|| ~ user_gain)");
        CHECK(active >= 3 && active <= 20, "SPCAP spreads across several speakers (not 1, not all)");
    }

    /* 9b. SPCAP tuning knobs: the geometry-derived focus default, focus monotonicity (with constant
     * power held), and the density change invalidating the cached placement correction. */
    {
        const float lis[3] = { LD.ref[0], LD.ref[1], LD.ref[2] };   /* sweet spot = the array center */
        printf("spcap: derived focus on the default cube grid = %.2f\n", (double)LD.spcap_focus);
        CHECK(LD.spcap_focus > 8.f && LD.spcap_focus < 20.f,
              "derived SPCAP focus lands in a sane band on the default grid");

        /* the -6 dB-at-nearest-neighbor formula, checked at three known angles: a two-speaker array
         * separated by `deg` has exactly that mean nearest-neighbor separation */
        const double want[3] = { 19.99, 8.75, 4.82 };
        const float  degs[3] = { 30.f, 45.f, 60.f };
        for (int t = 0; t < 3; ++t) {
            static Layout P; memset(&P, 0, sizeof P);
            P.count = 2;
            float a = degs[t] * 3.14159265358979f / 180.f;
            P.speakers[0].pos[0] =  sinf(0.5f*a); P.speakers[0].pos[2] = cosf(0.5f*a);
            P.speakers[1].pos[0] = -sinf(0.5f*a); P.speakers[1].pos[2] = cosf(0.5f*a);
            P.speakers[0].pos[1] = P.speakers[1].pos[1] = 0.f;
            P.ref[0] = P.ref[1] = P.ref[2] = 0.f;             /* solve from the origin, not the centroid */
            float f = layout_derive_spcap_focus(&P);
            CHECK(fabs((double)f - want[t]) < 0.05, "derived focus matches the -6 dB formula");
        }

        /* a WIDER array wants a broader lobe (lower focus), a DENSER one a tighter lobe. Six
         * speakers on the axes sit 90 deg apart; a 5x5x5 shell is packed tighter than the 3x3x3. */
        static Layout WIDE; WIDE = LD;
        const float ax6[6][3] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
        WIDE.count = 6;
        for (int k = 0; k < 6; ++k) for (int j = 0; j < 3; ++j)
            WIDE.speakers[k].pos[j] = LD.ref[j] + 2.f * ax6[k][j];
        float f_wide = layout_derive_spcap_focus(&WIDE);

        static Layout DENSE; DENSE = LD;                                    /* a 12-speaker ring: 30 deg apart, so
                                                               * denser than the grid's 37.5 deg */
        DENSE.count = 12;
        for (int k = 0; k < 12; ++k) {
            float a = (float)k * (2.f * 3.14159265358979f / 12.f);
            DENSE.speakers[k].pos[0] = LD.ref[0] + 2.f * sinf(a);
            DENSE.speakers[k].pos[1] = LD.ref[1];
            DENSE.speakers[k].pos[2] = LD.ref[2] + 2.f * cosf(a);
        }
        float f_dense = layout_derive_spcap_focus(&DENSE);
        printf("spcap: derived focus, grid %.2f, 6-speaker cross %.2f, 12-speaker ring %.2f\n",
               (double)LD.spcap_focus, (double)f_wide, (double)f_dense);
        CHECK(f_wide < LD.spcap_focus, "a wider array derives a broader lobe (lower focus)");
        CHECK(f_dense > LD.spcap_focus, "a denser array derives a tighter lobe (higher focus)");
        CHECK(f_wide >= 1.f && f_wide <= 64.f && f_dense >= 1.f && f_dense <= 64.f,
              "derived focus stays inside the clamp band");

        /* focus monotonicity: raise it and the energy concentrates, while ||g|| stays at user_gain */
        float src2[3] = { 0.35f, LD.ref[1] + 0.2f, 0.9f };
        float gl[CH], gh[CH];
        SpcapState sl; spcap_reset(&sl);
        SpcapState sh; spcap_reset(&sh);
        spcap_gains(&sl, src2, lis, &LD, 1u,  4.f, LD.spcap_density, 1.0f, gl);
        spcap_gains(&sh, src2, lis, &LD, 1u, 32.f, LD.spcap_density, 1.0f, gh);
        double pl = 0, ph = 0; float peakl = 0, peakh = 0; int nl = 0, nh = 0;
        for (int k = 0; k < CH; ++k) {
            pl += (double)gl[k]*gl[k]; ph += (double)gh[k]*gh[k];
            if (gl[k] > peakl) peakl = gl[k];
            if (gh[k] > peakh) peakh = gh[k];
        }
        for (int k = 0; k < CH; ++k) { if (gl[k] > 0.25f * peakl) ++nl; if (gh[k] > 0.25f * peakh) ++nh; }
        CHECK(peakh > peakl, "higher focus raises the peak speaker gain");
        CHECK(nh < nl, "higher focus lights fewer speakers above a quarter of peak");
        CHECK(fabs(sqrt(pl) - 1.0) < 0.02 && fabs(sqrt(ph) - 1.0) < 0.02,
              "constant power holds across focus values");

        /* density feeds the CACHED correction: reusing one state across a density change must
         * rebuild it, not silently keep the old c[] */
        float gd1[CH], gd2[CH];
        SpcapState sd; spcap_reset(&sd);
        spcap_gains(&sd, src2, lis, &LD, 1u, LD.spcap_focus, 1.0f, 1.0f, gd1);
        spcap_gains(&sd, src2, lis, &LD, 1u, LD.spcap_focus, 6.0f, 1.0f, gd2);
        double dmax = 0;
        for (int k = 0; k < CH; ++k) { double d = fabs((double)gd1[k] - gd2[k]); if (d > dmax) dmax = d; }
        CHECK(dmax > 1e-4, "a density change invalidates the cached placement correction");
    }

    /* 10. AllRAD bed decoder: builds, finite, energy ~ the sampling decode, localizes plane waves */
    {
        float dec[BWA_CHANNELS][BWA_AMBI_CH];
        int ok = allrad_build_decode(&LD, dec);
        CHECK(ok, "allrad_build_decode succeeds on the default grid");
        if (ok) {
            int finite = 1; double ediff = 0;
            for (int s = 0; s < CH; ++s) for (int k = 0; k < BWA_AMBI_CH; ++k) {
                if (!isfinite(dec[s][k])) finite = 0;
                int l = (int)floorf(sqrtf((float)k)); ediff += (double)dec[s][k]*dec[s][k]/(2*l+1);
            }
            CHECK(finite, "AllRAD matrix is finite");
            /* the sampling decode's ACTUAL diffuse energy (built from its formula, not a constant), so a
             * wrong normalization target in AllRAD would fail this rather than match a shared mistake */
            double esad = 0;
            for (int s = 0; s < CH; ++s) {
                float p[3] = { LD.speakers[s].pos[0] - LD.ref[0], LD.speakers[s].pos[1] - LD.ref[1],
                               LD.speakers[s].pos[2] - LD.ref[2] };       /* dirs from the array center */
                float pl = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
                float ad2[3] = { p[2]/pl, p[0]/pl, p[1]/pl }, ys[BWA_AMBI_CH]; ambi_encode_sn3d(ad2, ys);   /* (z,x,y): matches build_bed_decode */
                for (int k = 0; k < BWA_AMBI_CH; ++k) { int l = (int)floorf(sqrtf((float)k));
                    double d = (double)(2*l+1)*ys[k]/CH; esad += d*d/(2*l+1); }
            }
            CHECK(fabs(ediff - esad)/esad < 0.05, "AllRAD diffuse energy matches the sampling decode");
            int loc_ok = 1;
            float dirs[3][3] = { { 1, 0, 0 }, { 0, 1, 0.3f }, { -0.5f, 0.2f, -1 } };
            for (int t = 0; t < 3; ++t) {
                float* sd = dirs[t]; float sl = sqrtf(sd[0]*sd[0] + sd[1]*sd[1] + sd[2]*sd[2]);
                float s3[3] = { sd[0]/sl, sd[1]/sl, sd[2]/sl };
                float ad[3] = { s3[2], s3[0], s3[1] }, sh[BWA_AMBI_CH]; ambi_encode_sn3d(ad, sh);   /* (z,x,y): matches build_bed_decode */
                float rE[3] = { 0, 0, 0 };
                for (int s = 0; s < CH; ++s) {
                    float f = 0; for (int k = 0; k < BWA_AMBI_CH; ++k) f += dec[s][k]*sh[k];
                    float p[3] = { LD.speakers[s].pos[0] - LD.ref[0], LD.speakers[s].pos[1] - LD.ref[1],
                                   LD.speakers[s].pos[2] - LD.ref[2] };
                    float pl = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
                    float w = f*f; rE[0]+=w*p[0]/pl; rE[1]+=w*p[1]/pl; rE[2]+=w*p[2]/pl;
                }
                float rl = sqrtf(rE[0]*rE[0] + rE[1]*rE[1] + rE[2]*rE[2]);
                if (rl <= 0 || (rE[0]*s3[0] + rE[1]*s3[1] + rE[2]*s3[2])/rl < 0.9f) loc_ok = 0;  /* within ~25 deg */
            }
            CHECK(loc_ok, "AllRAD localizes plane waves toward their direction");

            /* max-rE weights (ambi_max_re_weights) lengthen the ENERGY VECTOR through the real
             * decode: |rE|/E with the tapered encode beats the raw encode for every test plane wave
             * — the acoustic claim behind bwa_set_max_re (sharper energy concentration = better
             * localization away from the sweet spot). */
            {
                float rw[BWA_AMBI_CH];
                ambi_max_re_weights(3, rw);
                int re_ok = 1;
                for (int t2 = 0; t2 < 3; ++t2) {
                    float* sd = dirs[t2]; float sl = sqrtf(sd[0]*sd[0] + sd[1]*sd[1] + sd[2]*sd[2]);
                    float s3[3] = { sd[0]/sl, sd[1]/sl, sd[2]/sl };
                    float ad[3] = { s3[2], s3[0], s3[1] }, sh[BWA_AMBI_CH]; ambi_encode_sn3d(ad, sh);
                    double rlen[2];
                    for (int m = 0; m < 2; ++m) {
                        double rE[3] = { 0, 0, 0 }, E = 0;
                        for (int s = 0; s < CH; ++s) {
                            float f = 0;
                            for (int k = 0; k < BWA_AMBI_CH; ++k) f += dec[s][k] * sh[k] * (m ? rw[k] : 1.f);
                            float p[3] = { LD.speakers[s].pos[0] - LD.ref[0], LD.speakers[s].pos[1] - LD.ref[1],
                                           LD.speakers[s].pos[2] - LD.ref[2] };
                            float pl = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
                            double w = (double)f * f;
                            E += w; rE[0] += w*p[0]/pl; rE[1] += w*p[1]/pl; rE[2] += w*p[2]/pl;
                        }
                        rlen[m] = E > 0 ? sqrt(rE[0]*rE[0] + rE[1]*rE[1] + rE[2]*rE[2]) / E : 0;
                    }
                    if (!(rlen[1] > rlen[0] + 0.01)) re_ok = 0;
                }
                CHECK(re_ok, "max-rE weights lengthen the energy vector through the AllRAD decode");
            }
        }

        /* imaginary pole speaker: a FLOOR-LESS array (the default grid minus its y=0 level) leaves a
         * > 60° hole at the nadir. The imaginary nadir speaker closes the hull there and its share is
         * discarded, so a straight-down plane wave decodes to (near) nothing instead of smearing full
         * power onto the bottom ring. The cube grid (nadir gap ~55°) takes the unfixed path above. */
        {
            static Layout LH;
            memset(&LH, 0, sizeof LH);
            uint32_t nh = 0;
            for (uint32_t s = 0; s < LD.count; ++s) {
                if (LD.speakers[s].pos[1] < 0.1f) continue;    /* drop the floor level */
                LH.speakers[nh].pos[0] = LD.speakers[s].pos[0];
                LH.speakers[nh].pos[1] = LD.speakers[s].pos[1];
                LH.speakers[nh].pos[2] = LD.speakers[s].pos[2];
                LH.speakers[nh].gain_lin = 1.0f;
                ++nh;
            }
            LH.count = nh;                                     /* 17: the 1.5 m ring (8) + the ceiling (9) */
            layout_compute_ref(&LH);
            float dech[BWA_CHANNELS][BWA_AMBI_CH];
            int okh = allrad_build_decode(&LH, dech);
            CHECK(okh, "allrad_build_decode succeeds on a floor-less array");
            if (okh) {
                /* decoded energy of a plane wave from direction d (room axes) */
                double e_down = 0, e_side = 0;
                float dirs2[2][3] = { { 0, -1, 0 }, { 1, 0, 0 } };
                double* acc[2] = { &e_down, &e_side };
                for (int t = 0; t < 2; ++t) {
                    float ad[3] = { dirs2[t][2], dirs2[t][0], dirs2[t][1] }, sh[BWA_AMBI_CH];
                    ambi_encode_sn3d(ad, sh);                  /* (z,x,y): matches build_bed_decode */
                    for (uint32_t s = 0; s < nh; ++s) {
                        float f = 0; for (int k = 0; k < BWA_AMBI_CH; ++k) f += dech[s][k]*sh[k];
                        *acc[t] += (double)f * f;
                    }
                }
                CHECK(e_side > 0 && e_down < 0.25 * e_side,
                      "imaginary nadir speaker discards straight-down diffuse energy (no bottom-ring smear)");
            }
        }
    }

    /* 10b. EPAD bed decoder (epad.c): builds, finite, diffuse energy matched to the sampling
     *      decode (level-fair swap), localizes plane waves — and delivers THE property it exists
     *      for: a panned plane wave's decoded ENERGY is ~constant over direction on an irregular
     *      array, where the sampling decode over-energizes dense speaker regions. Pinned as the
     *      loudness-vs-direction spread (CV) on a deliberately clustered array: EPAD's must come
     *      in far under the sampling decode's. */
    {
        float dep[BWA_CHANNELS][BWA_AMBI_CH];
        int ok = epad_build_decode(&LD, dep);
        CHECK(ok, "epad_build_decode succeeds on the default grid");
        if (ok) {
            int finite = 1; double ediff = 0;
            for (int s = 0; s < CH; ++s) for (int k = 0; k < BWA_AMBI_CH; ++k) {
                if (!isfinite(dep[s][k])) finite = 0;
                int l = (int)floorf(sqrtf((float)k)); ediff += (double)dep[s][k]*dep[s][k]/(2*l+1);
            }
            CHECK(finite, "EPAD matrix is finite");
            double esad = 0;
            for (int s = 0; s < CH; ++s) {
                float p[3] = { LD.speakers[s].pos[0] - LD.ref[0], LD.speakers[s].pos[1] - LD.ref[1],
                               LD.speakers[s].pos[2] - LD.ref[2] };
                float pl = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
                float ad2[3] = { p[2]/pl, p[0]/pl, p[1]/pl }, ys[BWA_AMBI_CH]; ambi_encode_sn3d(ad2, ys);
                for (int k = 0; k < BWA_AMBI_CH; ++k) { int l = (int)floorf(sqrtf((float)k));
                    double d = (double)(2*l+1)*ys[k]/CH; esad += d*d/(2*l+1); }
            }
            CHECK(fabs(ediff - esad)/esad < 0.05, "EPAD diffuse energy matches the sampling decode");
            int loc_ok = 1;
            float dirs[3][3] = { { 1, 0, 0 }, { 0, 1, 0.3f }, { -0.5f, 0.2f, -1 } };
            for (int t = 0; t < 3; ++t) {
                float* sd = dirs[t]; float sl = sqrtf(sd[0]*sd[0] + sd[1]*sd[1] + sd[2]*sd[2]);
                float s3[3] = { sd[0]/sl, sd[1]/sl, sd[2]/sl };
                float ad[3] = { s3[2], s3[0], s3[1] }, sh[BWA_AMBI_CH]; ambi_encode_sn3d(ad, sh);
                float rE[3] = { 0, 0, 0 };
                for (int s = 0; s < CH; ++s) {
                    float f = 0; for (int k = 0; k < BWA_AMBI_CH; ++k) f += dep[s][k]*sh[k];
                    float p[3] = { LD.speakers[s].pos[0] - LD.ref[0], LD.speakers[s].pos[1] - LD.ref[1],
                                   LD.speakers[s].pos[2] - LD.ref[2] };
                    float pl = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
                    float w = f*f; rE[0]+=w*p[0]/pl; rE[1]+=w*p[1]/pl; rE[2]+=w*p[2]/pl;
                }
                float rl = sqrtf(rE[0]*rE[0] + rE[1]*rE[1] + rE[2]*rE[2]);
                if (rl <= 0 || (rE[0]*s3[0] + rE[1]*s3[1] + rE[2]*s3[2])/rl < 0.9f) loc_ok = 0;
            }
            CHECK(loc_ok, "EPAD localizes plane waves toward their direction");
        }

        /* the energy-preserving claim, on a lopsided array: 10 speakers bunched into a ~45° cone
         * toward +x plus 6 covering the rest. E(d) = the decoded energy of a plane wave, swept
         * over a Fibonacci sphere; CV = std/mean of E over the sweep. */
        {
            static Layout LC;
            memset(&LC, 0, sizeof LC);
            uint32_t nc = 0;
            for (int i = 0; i < 10; ++i) {                       /* the cluster: a cap around +x */
                float yy = -0.45f + 0.9f * ((float)i + 0.5f) / 10.f;
                float rr = sqrtf(1.f - yy * yy), th = (float)i * 2.39996323f;
                float d[3] = { 1.6f, yy, rr * sinf(th) * 0.45f };
                float dl = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
                LC.speakers[nc].pos[0] = 2.f * d[0] / dl;
                LC.speakers[nc].pos[1] = 1.5f + 2.f * d[1] / dl;
                LC.speakers[nc].pos[2] = 2.f * d[2] / dl;
                LC.speakers[nc].gain_lin = 1.f; ++nc;
            }
            const float rest[6][3] = { { -1,0,0 }, { 0,1,0 }, { 0,-1,0 }, { 0,0,1 }, { 0,0,-1 }, { -0.7f,0.7f,0 } };
            for (int i = 0; i < 6; ++i) {
                float dl = sqrtf(rest[i][0]*rest[i][0] + rest[i][1]*rest[i][1] + rest[i][2]*rest[i][2]);
                LC.speakers[nc].pos[0] = 2.f * rest[i][0] / dl;
                LC.speakers[nc].pos[1] = 1.5f + 2.f * rest[i][1] / dl;
                LC.speakers[nc].pos[2] = 2.f * rest[i][2] / dl;
                LC.speakers[nc].gain_lin = 1.f; ++nc;
            }
            LC.count = nc;                                       /* 16 */
            layout_compute_ref(&LC);
            float dep2[BWA_CHANNELS][BWA_AMBI_CH], dsad[BWA_CHANNELS][BWA_AMBI_CH];
            int okc = epad_build_decode(&LC, dep2);
            CHECK(okc, "epad_build_decode succeeds on the clustered array");
            ambi_sad_decode(&LC, LC.count, dsad);                /* the engine's actual sampling decode
                                                                  * (the foil guards allrad/epad's own
                                                                  * closed-form constant, not this) */
            if (okc) {
                double cv[2];
                float (*mats[2])[BWA_AMBI_CH] = { dep2, dsad };
                for (int m = 0; m < 2; ++m) {
                    enum { SWEEP = 128 };
                    double sum = 0, sum2 = 0;
                    for (int i = 0; i < SWEEP; ++i) {            /* Fibonacci sweep of plane waves */
                        float yy = 1.f - 2.f * ((float)i + 0.5f) / (float)SWEEP;
                        float rr = sqrtf(fmaxf(0.f, 1.f - yy * yy)), th = (float)i * 2.39996323f;
                        float d[3] = { rr * cosf(th), yy, rr * sinf(th) };
                        float ad[3] = { d[2], d[0], d[1] }, sh[BWA_AMBI_CH]; ambi_encode_sn3d(ad, sh);
                        double E = 0;
                        for (uint32_t s = 0; s < LC.count; ++s) {
                            float f = 0; for (int k = 0; k < BWA_AMBI_CH; ++k) f += mats[m][s][k]*sh[k];
                            E += (double)f * f;
                        }
                        sum += E; sum2 += E * E;
                    }
                    double mean = sum / 128.0, var = sum2 / 128.0 - mean * mean;
                    cv[m] = mean > 0 ? sqrt(var > 0 ? var : 0) / mean : 1e9;
                }
                printf("epad: clustered-array energy CV %.3f (EPAD) vs %.3f (sampling)\n", cv[0], cv[1]);
                CHECK(cv[0] < 0.5 * cv[1],
                      "EPAD flattens loudness-vs-direction on a clustered array (energy-preserving)");
            }
        }
    }

    /* 11. VBAP panner: localization, constant power, at most 3 active speakers (the containing triangle) */
    {
        VbapState vb; vbap_reset(&vb);
        float lis[3] = { LD.ref[0], LD.ref[1], LD.ref[2] }, g[CH];   /* inside the hull (the floor is ON it) */
        int loc_ok = 1;
        for (int k = 0; k < CH; ++k) {
            vbap_gains(&vb, LD.speakers[k].pos, lis, &LD, 1u, 1.0f, g);   /* source at speaker k's bearing */
            if (argmax(g, CH) != k) loc_ok = 0;
        }
        CHECK(loc_ok, "VBAP localizes a source at each speaker to that channel");

        float src[3] = { 0.7f, LD.ref[1] + 0.2f, 0.6f }, gain = 0.8f;      /* ds < 1 m -> atten = 1 */
        vbap_gains(&vb, src, lis, &LD, 1u, gain, g);
        double p = 0; int active = 0;
        for (int k = 0; k < CH; ++k) { p += (double)g[k] * g[k]; if (g[k] > 1e-4f) ++active; }
        CHECK(fabs(sqrt(p) - gain) < 0.02, "VBAP is constant-power (||g|| ~ user_gain)");
        CHECK(active >= 1 && active <= 3, "VBAP uses at most 3 speakers (the containing triangle)");
    }

    /* 11b. hull.c: the incremental hull equals the old brute force as a face set on generic input,
     *      and is a valid closed triangulation where the brute force was not (coplanar caps). */
    {
        static float hd[HT_MAXPTS][3];
        int generic_ok = 1, compared = 0;

        /* the default grid and the 24-speaker layout the load test builds, from their own refs */
        for (uint32_t k = 0; k < LD.count; ++k) unit_dir(LD.ref, LD.speakers[k].pos, hd[k]);
        if (!ht_check(hd, LD.count, "default grid", 21u, &compared)) generic_ok = 0;
        {
            static Layout L24; L24 = LD; L24.count = 24; layout_compute_ref(&L24);
            for (uint32_t k = 0; k < 24; ++k) unit_dir(L24.ref, L24.speakers[k].pos, hd[k]);
            if (!ht_check(hd, 24, "24-speaker grid subset", 22u, &compared)) generic_ok = 0;
        }
        static const int fibn[] = { 8, 12, 16, 20, 26, 32, 40, 48, 56, 64 };
        for (size_t i = 0; i < sizeof fibn / sizeof fibn[0]; ++i) {
            for (int k = 0; k < fibn[i]; ++k) fib_dir(k, fibn[i], hd[k]);
            if (!ht_check(hd, (uint32_t)fibn[i], "Fibonacci sphere", 23u + (uint32_t)i, &compared)) generic_ok = 0;
        }
        printf("hull: %d of 12 fixed sets compared to the oracle, the rest checked for validity\n", compared);
        CHECK(generic_ok, "hull matches the oracle (or is valid where the set is degenerate): grid, 24-subset, Fibonacci 8..64");
        CHECK(compared >= 10, "the fixed-set comparison is not vacuous (the default grid and the Fibonacci spheres are generic)");

        /* 200 random sets in general position, sizes 4..64. A draw the oracle cannot answer (a point
         * within 1e-4 of a face plane, where it emits BOTH triangulations) is redrawn and counted. */
        {
            static int to[HT_BIGTRI][3]; static float dO[HT_BIGTRI];
            uint32_t seed = 0x5eed1234u;
            int matched = 0, redrawn = 0, bad = 0;
            while (matched + bad < 200 && redrawn < 2000) {
                const uint32_t n = 4u + (uint32_t)((matched + bad) % 61);
                for (uint32_t k = 0; k < n; ++k) {
                    float l2;
                    do { hd[k][0] = lcg_noise(&seed); hd[k][1] = lcg_noise(&seed); hd[k][2] = lcg_noise(&seed);
                         l2 = hd[k][0]*hd[k][0] + hd[k][1]*hd[k][1] + hd[k][2]*hd[k][2]; } while (l2 < 1e-4f || l2 > 1.f);
                    ht_norm(hd[k]);
                }
                int no = hull_oracle(hd, n, to, dO, HT_BIGTRI);
                if (no <= 0 || !ht_generic(hd, n, to, no)) { ++redrawn; continue; }
                if (ht_match(hd, n, "random")) ++matched; else ++bad;
            }
            printf("hull: %d/200 random sets match the oracle (%d near-degenerate draws redrawn)\n", matched, redrawn);
            CHECK(matched == 200, "hull matches the brute-force oracle on 200 random sets (n 4..64)");
        }

        /* coplanar sets: the oracle is wrong here, so assert validity instead */
        int valid_ok = 1;
        ht_barrel64(hd, 0); valid_ok &= ht_valid(hd, 64, "64 barrel, aligned", 11u);
        ht_barrel64(hd, 1); valid_ok &= ht_valid(hd, 64, "64 barrel, staggered", 12u);
        {
            uint32_t js = 77u;                                          /* 1e-6 survey jitter */
            ht_barrel64(hd, 0);
            for (int k = 0; k < 64; ++k) { for (int q = 0; q < 3; ++q) hd[k][q] += 1e-6f * lcg_noise(&js); ht_norm(hd[k]); }
            valid_ok &= ht_valid(hd, 64, "64 barrel, jittered", 13u);
            int k = 0;
            for (int ix = -1; ix <= 1; ix += 2) for (int iy = -1; iy <= 1; iy += 2) for (int iz = -1; iz <= 1; iz += 2, ++k) {
                hd[k][0] = (float)ix; hd[k][1] = (float)iy; hd[k][2] = (float)iz; ht_norm(hd[k]); }
            valid_ok &= ht_valid(hd, 8, "cube corners", 14u);
            for (k = 0; k < 8; ++k) { for (int q = 0; q < 3; ++q) hd[k][q] += 1e-6f * lcg_noise(&js); ht_norm(hd[k]); }
            valid_ok &= ht_valid(hd, 8, "cube corners, jittered", 15u);
        }
        {
            for (int k = 0; k < 8; ++k) { const float th = (float)k * 0.785398163f;
                hd[k][0] = cosf(th); hd[k][1] = 0.f; hd[k][2] = sinf(th); }
            hd[8][0] = 0.f; hd[8][1] =  1.f; hd[8][2] = 0.f;
            hd[9][0] = 0.f; hd[9][1] = -1.f; hd[9][2] = 0.f;
            valid_ok &= ht_valid(hd, 10, "ring + two poles", 16u);
        }
        {
            static Layout LB24; make_barrel(&LB24);                     /* the 24-speaker CAVE barrel */
            for (uint32_t k = 0; k < LB24.count; ++k) unit_dir(LB24.ref, LB24.speakers[k].pos, hd[k]);
            valid_ok &= ht_valid(hd, LB24.count, "24 barrel", 17u);
        }
        CHECK(valid_ok, "hull is a valid closed triangulation on coplanar sets (barrels, cube, ring + poles)");

        /* a speaker doubled at float-rounding distance: inside the tolerance it must merge into its
         * twin rather than cut sliver faces (this is the case HULL_EPS exists for) */
        {
            for (uint32_t k = 0; k < LD.count; ++k) unit_dir(LD.ref, LD.speakers[k].pos, hd[k]);
            hd[26][0] = hd[5][0] + 3e-7f; hd[26][1] = hd[5][1] - 2e-7f; hd[26][2] = hd[5][2]; ht_norm(hd[26]);
            CHECK(ht_valid_ex(hd, 27, "grid + near-duplicate", 18u, 0),
                  "hull merges a near-duplicate speaker instead of emitting slivers");
        }

        /* the 64 barrel through the panner: the brute force overflowed VBAP_MAXTRI there and VBAP
         * silently fell back to DBAP */
        {
            static int to[HT_BIGTRI][3]; static float dO[HT_BIGTRI];
            ht_barrel64(hd, 0);
            CHECK(hull_oracle(hd, 64, to, dO, VBAP_MAXTRI) == 0,
                  "the brute-force hull overflows VBAP_MAXTRI on the 64 barrel (so the next checks discriminate)");
            static Layout LB64; memset(&LB64, 0, sizeof LB64);
            for (int k = 0; k < 64; ++k) {
                LB64.speakers[k].pos[0] = 2.f * hd[k][0] * 2.5f;        /* any radius: only bearings matter */
                LB64.speakers[k].pos[1] = 1.5f + 2.f * hd[k][1] * 2.5f;
                LB64.speakers[k].pos[2] = 2.f * hd[k][2] * 2.5f;
                LB64.speakers[k].gain_lin = 1.f;
            }
            LB64.count = 64; layout_compute_ref(&LB64);
            LB64.rolloff_r = 0.7f; LB64.atten_ref_m = 1.f; LB64.atten_rolloff = 1.f; LB64.atten_min_lin = 0.01f;
            static VbapState vb; vbap_reset(&vb);
            float lis[3] = { 0.f, 1.5f, 0.f }, src[3] = { 0.9f, 1.8f, 0.4f }, g[BWA_MAX_CHANNELS];
            vbap_gains(&vb, src, lis, &LB64, 1u, 1.0f, g);
            int active = 0;
            for (int k = 0; k < 64; ++k) if (g[k] > 1e-4f) ++active;
            printf("hull: 64 barrel -> %d VBAP triangles, %d active speakers\n", vb.ntri, active);
            CHECK(vb.ntri > 0 && vb.ntri <= 2 * 64 - 4, "the 64 barrel triangulates within 2n-4 faces under VBAP_MAXTRI");
            CHECK(active >= 1 && active <= 3, "VBAP on the 64 barrel pans on a triangle (no DBAP fallback)");
        }
    }

    /* 12. CAP: the dual-band low band projected so the rendered ITD matches a real source's, for the
     *     head's current orientation (cap.c). The quantity under test throughout is the interaural
     *     component of the velocity vector, rV.e = sum(g*ce)/sum(g) — what the ear reads as ITD
     *     below the crossover — against the real source's own u_s.e. */
    {
        CapState cp; cap_reset(&cp);
        float lis[3] = { LD.ref[0], LD.ref[1], LD.ref[2] };
        const float qid[4] = { 0.f, 0.f, 0.f, 1.f };            /* identity head: facing room-ahead */
        float g0[CH], gl[CH], ref[CH];

        /* a source OFF the median plane, close enough that atten == 1 */
        float src[3] = { lis[0] + 1.0f, lis[1] + 0.2f, lis[2] + 0.5f };
        dbap_gains(src, lis, &LD, 1.0f, g0);
        cap_block(&cp, &LD, lis, qid, 1u);
        float us[3]; unit_dir(lis, src, us);
        cap_gains_lo(&cp, g0, us, 1.f, gl);

        CHECK(fabs(itd_component(gl, cp.ce, LD.count) - dot3(us, cp.e)) < 1e-5,
              "CAP makes the LF interaural component exact (rV.e == u_s.e)");
        /* the test must be able to FAIL: plain dual-band renormalization leaves rV.e short */
        dual_band_lo(g0, LD.count, ref);
        CHECK(fabs(itd_component(ref, cp.ce, LD.count) - dot3(us, cp.e)) > 1e-3,
              "plain dual-band does NOT (so the check above discriminates)");

        /* level convention unchanged: the LF amplitude sum still equals the HF power magnitude */
        double as = 0.0, pw = 0.0;
        for (uint32_t k = 0; k < LD.count; ++k) { as += gl[k]; pw += (double)g0[k] * g0[k]; }
        CHECK(fabs(as - sqrt(pw)) < 1e-4, "CAP keeps the dual-band level convention (sum g_lo == ||g||_2)");

        /* HEAD ROTATION is the whole claim: sweep yaw and the ITD must not drift.
         *
         * The target is only achievable while it lies inside [min ce, max ce] over the lit speakers.
         * rV is a convex combination of speaker directions, so the array cannot render an ITD MORE
         * LATERAL than its own most lateral speaker — an array-density bound, not a CAP defect. On
         * this grid that bites only where the head turns to put the source within ~11 deg of the
         * interaural axis while the nearest speaker is 15 deg off it. So the contract under test is:
         * exact wherever feasible, saturated at the bound otherwise, and never worse than plain. */
        {
            int cap_ok = 1, plain_drifts = 0, infeasible = 0, worse = 0;
            double cap_worst = 0.0, plain_worst = 0.0;
            for (int d = 0; d < 360; d += 15) {
                const float a = (float)d * 3.14159265358979f / 180.f;
                const float q[4] = { 0.f, sinf(a * 0.5f), 0.f, cosf(a * 0.5f) };   /* yaw about room up */
                cap_block(&cp, &LD, lis, q, 1u);
                cap_gains_lo(&cp, g0, us, 1.f, gl);

                const double want = dot3(us, cp.e);
                double lo = 1.0, hi = -1.0;
                for (uint32_t k = 0; k < LD.count; ++k) {
                    if (g0[k] <= 0.f) continue;
                    if (cp.ce[k] < lo) lo = cp.ce[k];
                    if (cp.ce[k] > hi) hi = cp.ce[k];
                }
                double feasible = want < lo ? lo : (want > hi ? hi : want);
                if (feasible != want) ++infeasible;

                const double ec = fabs(itd_component(gl,  cp.ce, LD.count) - want);
                const double ep = fabs(itd_component(ref, cp.ce, LD.count) - want);
                if (fabs(itd_component(gl, cp.ce, LD.count) - feasible) > 1e-5) cap_ok = 0;
                if (ep > 1e-3) plain_drifts = 1;
                if (ec > ep + 1e-6) worse = 1;
                if (ec > cap_worst)   cap_worst = ec;
                if (ep > plain_worst) plain_worst = ep;
            }
            printf("cap: yaw sweep worst |rV.e - u_s.e| = %.4f (CAP) vs %.4f (dual-band); "
                   "%d/24 yaws beyond the array's lateral limit\n", cap_worst, plain_worst, infeasible);
            CHECK(cap_ok, "CAP holds the ITD at its feasible target through a full head yaw sweep");
            CHECK(!worse, "CAP is never worse than plain dual-band at any head yaw");
            CHECK(plain_drifts, "plain dual-band's ITD drifts with head yaw (the failure CAP fixes)");
        }

        /* facing the source (median plane, symmetric array) the constraint is already satisfied:
         * CAP must reduce to the panner it wrapped, not merely approximate it */
        {
            float msrc[3] = { lis[0], lis[1] + 0.2f, lis[2] + 0.9f };   /* x == listener x -> median plane */
            dbap_gains(msrc, lis, &LD, 1.0f, g0);
            cap_block(&cp, &LD, lis, qid, 1u);
            unit_dir(lis, msrc, us);
            cap_gains_lo(&cp, g0, us, 1.f, gl);
            dual_band_lo(g0, LD.count, ref);
            double worst = 0.0;
            for (uint32_t k = 0; k < LD.count; ++k) { double e = fabs(gl[k] - ref[k]); if (e > worst) worst = e; }
            CHECK(worst < 1e-5, "CAP is a no-op for a median-plane source (reduces to the panner)");
        }

        /* a VBAP seed must stay sparse: the g0-weighted projection is multiplicative, so a speaker
         * the panner left silent can never be recruited to buy an ITD */
        {
            VbapState vb; vbap_reset(&vb);
            float vsrc[3] = { lis[0] + 0.8f, lis[1] + 0.3f, lis[2] + 0.4f };
            vbap_gains(&vb, vsrc, lis, &LD, 1u, 1.0f, g0);
            cap_block(&cp, &LD, lis, qid, 1u);
            unit_dir(lis, vsrc, us);
            cap_gains_lo(&cp, g0, us, 1.f, gl);
            int leaked = 0, active = 0;
            for (uint32_t k = 0; k < LD.count; ++k) {
                if (g0[k] <= 0.f && gl[k] != 0.f) leaked = 1;
                if (gl[k] > 1e-4f) ++active;
            }
            CHECK(!leaked, "CAP never activates a speaker the panner left silent (support preserved)");
            CHECK(active >= 1 && active <= 3, "CAP keeps a VBAP seed on its triangle");
            CHECK(fabs(itd_component(gl, cp.ce, LD.count) - dot3(us, cp.e)) < 1e-5,
                  "CAP is ITD-exact on a 3-speaker VBAP seed too");
        }

        /* strength 0 (an engulfing, fully-spread source) is provably inert: the target collapses
         * onto the seed's own rV.e, so there is nothing to correct and the widening survives */
        {
            dbap_gains(src, lis, &LD, 1.0f, g0);
            cap_block(&cp, &LD, lis, qid, 1u);
            unit_dir(lis, src, us);
            cap_gains_lo(&cp, g0, us, 0.f, gl);
            dual_band_lo(g0, LD.count, ref);
            double worst = 0.0;
            for (uint32_t k = 0; k < LD.count; ++k) { double e = fabs(gl[k] - ref[k]); if (e > worst) worst = e; }
            CHECK(worst < 1e-5, "CAP at strength 0 is exactly the plain dual-band low band");
        }
    }

    /* 13. hole-aware spread floor (hole.c): a source aimed where the array has NO speaker is floored
     *     wide instead of rendered as a split image. The discriminating pair is a BARREL (open at
     *     both poles, the real CAVE shape) against the default cube grid (speakers at both poles,
     *     no holes at all) — the floor must engage on the first and be identically inert on the
     *     second, with no per-layout tuning: the knee is the array's own mean speaker spacing. */
    {
        static Layout LB; make_barrel(&LB);
        const float lisB[3] = { 0.f, 1.4f, 0.f };                 /* seated ear height in the barrel */
        const float lisG[3] = { LD.ref[0], LD.ref[1], LD.ref[2] };
        const float NADIR[3] = { 0.f, -1.f, 0.f }, ZENITH[3] = { 0.f, 1.f, 0.f };
        HoleState hb, hg;
        hole_reset(&hb); hole_reset(&hg);
        hole_block(&hb, &LB, lisB, 1u);
        hole_block(&hg, &LD, lisG, 1u);

        const double knee_b = layout_mean_speaker_spacing(&LB) * 57.2957795;
        const double knee_g = layout_mean_speaker_spacing(&LD) * 57.2957795;
        printf("hole: mean speaker spacing %.1f deg (barrel, %u spk) vs %.1f deg (cube grid, %u spk)\n",
               knee_b, LB.count, knee_g, LD.count);
        CHECK(knee_g > 30.0 && knee_g < 45.0, "cube grid's mean speaker spacing is ~37 deg");
        /* the exported spacing IS what the SPCAP focus default is built on (one geometry, two users) */
        CHECK(fabs(layout_derive_spcap_focus(&LD) -
                   log(0.25) / log(0.5 * (1.0 + cos(layout_mean_speaker_spacing(&LD))))) < 1e-3,
              "layout_mean_speaker_spacing is the same delta SPCAP's derived focus uses");

        /* the poles: the barrel's holes, and where the split image lives */
        const double gap_n = nearest_speaker_deg(&LB, lisB, NADIR);
        const double gap_z = nearest_speaker_deg(&LB, lisB, ZENITH);
        const float  f_n = hole_floor(&hb, NADIR), f_z = hole_floor(&hb, ZENITH);
        printf("hole: barrel nadir gap %.1f deg -> spread floor %.3f; zenith gap %.1f deg -> %.3f\n",
               gap_n, (double)f_n, gap_z, (double)f_z);
        CHECK(f_n > 0.25f, "the barrel's nadir hole floors the spread (a wide source, not a split image)");
        CHECK(f_z > 0.15f, "the barrel's zenith hole floors it too");

        /* inert wherever the array actually has a speaker, on both layouts */
        {
            int quiet = 1;
            for (uint32_t k = 0; k < LB.count; ++k) {
                float u[3]; unit_dir(lisB, LB.speakers[k].pos, u);
                if (hole_floor(&hb, u) != 0.f) quiet = 0;
            }
            CHECK(quiet, "no floor at any barrel speaker's own bearing");
        }

        /* the discriminating half: sweep the whole sphere on the SURROUNDING grid and find nothing */
        {
            enum { SWEEP = 4096 };
            double worst_gap_g = 0.0, worst_gap_b = 0.0;
            float  worst_f_g = 0.f, worst_f_b = 0.f;
            for (int i = 0; i < SWEEP; ++i) {
                float u[3]; fib_dir(i, SWEEP, u);
                double gg = nearest_speaker_deg(&LD, lisG, u), gb = nearest_speaker_deg(&LB, lisB, u);
                float  fg = hole_floor(&hg, u),                fb = hole_floor(&hb, u);
                if (gg > worst_gap_g) worst_gap_g = gg;
                if (gb > worst_gap_b) worst_gap_b = gb;
                if (fg > worst_f_g)   worst_f_g = fg;
                if (fb > worst_f_b)   worst_f_b = fb;
            }
            printf("hole: sphere sweep worst gap %.1f deg -> floor %.3f (cube grid) vs "
                   "%.1f deg -> %.3f (barrel)\n", worst_gap_g, (double)worst_f_g,
                   worst_gap_b, (double)worst_f_b);
            CHECK(worst_f_g == 0.f, "the surrounding cube grid never floors anything (inert by construction)");
            CHECK(worst_f_b > 0.25f, "the barrel does (so the check above discriminates)");
        }

        /* monotone in depth: diving from the horizon toward nadir can only widen, never narrow */
        {
            int mono = 1; float prev = -1.f;
            for (int d = 0; d <= 90; d += 5) {
                const float a = (float)d * 0.0174532925f;
                const float u[3] = { cosf(a), -sinf(a), 0.f };     /* az 0 (a speaker column), dive to nadir */
                const float f = hole_floor(&hb, u);
                if (f < prev - 1e-6f) mono = 0;
                prev = f;
            }
            CHECK(mono, "the floor rises monotonically as a source dives into the hole");
        }

        /* the cache self-invalidates on a listener move: standing up shrinks the nadir hole (the
         * bottom ring drops further below the horizon), so the floor must FALL — a stale direction
         * cache would hold the old value */
        {
            const float lis_hi[3] = { 0.f, 2.3f, 0.f };
            hole_block(&hb, &LB, lis_hi, 1u);
            const float f_hi = hole_floor(&hb, NADIR);
            printf("hole: listener 1.4 m -> nadir floor %.3f; 2.3 m -> %.3f (gap %.1f deg)\n",
                   (double)f_n, (double)f_hi, nearest_speaker_deg(&LB, lis_hi, NADIR));
            CHECK(f_hi < f_n - 0.05f, "the direction cache self-invalidates on a listener move");
            hole_block(&hb, &LB, lisB, 1u);                        /* back, and idempotent */
            CHECK(fabsf(hole_floor(&hb, NADIR) - f_n) < 1e-6f, "hole_block is idempotent");
        }

        /* a layout-generation change rebuilds the cache too: raise the bottom ring toward ear height
         * and the nadir hole opens wider, so the floor must rise */
        {
            static Layout LB2; LB2 = LB;
            for (uint32_t k = 0; k < 8; ++k) LB2.speakers[k].pos[1] = 1.0f;
            layout_compute_ref(&LB2);
            hole_block(&hb, &LB2, lisB, 2u);
            const float f2 = hole_floor(&hb, NADIR);
            CHECK(f2 > f_n + 0.05f, "a layout change rebuilds the cache (a raised ring opens the nadir hole)");
        }
    }

    remove(LJ);
    if (fails) { printf("dsp_test: %d FAILURES\n", fails); return 1; }
    printf("dsp_test OK (layout parse, DBAP + SPCAP + VBAP + CAP, AllRAD + EPAD bed decodes + max-rE, "
           "hole-aware spread floor, align gain+delay)\n");
    return 0;
}
