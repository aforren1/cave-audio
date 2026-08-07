/*
 * tools_api_test.c — the PURE tools API: bwa_spcap_focus_default and bwa_panner_gains_batch.
 *
 * Both live in engine.c, so they are only reachable through the DLL (bwa_core does not carry them
 * and the standalone DSP suites cannot see them). This target links BOTH: the DLL for the public
 * calls, and bwa_core for the internal layout_derive_spcap_focus that the focus default is supposed
 * to agree with. That cross-check is the reason this is its own target rather than a smoke.c
 * section — smoke.c is deliberately "the public ABI, nothing else".
 *
 * No engine is created anywhere here. These calls take no handle by contract.
 */
#include "bw_audio.h"
#include "layout.h"            /* bwa_core: layout_derive_spcap_focus, the derivation under test */

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++failures; } \
    else         { printf("ok:   %s\n", (msg)); } \
} while (0)

/* the engine's own default grid: 3x3x3 boundary minus the center = 26, floor-origin y */
static uint32_t cube_grid(float* pos) {
    const float ax[3] = { -1.5f, 0.0f, 1.5f };
    const float ay[3] = {  0.0f, 1.5f, 3.0f };
    uint32_t k = 0;
    for (int yi = 0; yi < 3; ++yi)
        for (int xi = 0; xi < 3; ++xi)
            for (int zi = 0; zi < 3; ++zi) {
                if (ax[xi] == 0.f && ay[yi] == 1.5f && ax[zi] == 0.f) continue;   /* the center */
                pos[k*3+0] = ax[xi]; pos[k*3+1] = ay[yi]; pos[k*3+2] = ax[zi];
                ++k;
            }
    return k;
}

/* a ring of `n` speakers of radius `r` about (0, 1.5, 0): spacing is 360/n degrees, so the ring
 * size alone sets how dense the array is. */
static void ring(float* pos, uint32_t n, float r) {
    for (uint32_t k = 0; k < n; ++k) {
        float a = (float)k * (6.28318530718f / (float)n);
        pos[k*3+0] = r * sinf(a);
        pos[k*3+1] = 1.5f;
        pos[k*3+2] = r * cosf(a);
    }
}

/* ---- bwa_spcap_focus_default ---- */
static void test_focus_default(void) {
    float pos[BWA_CHANNELS * 3];
    uint32_t n = cube_grid(pos);
    CHECK(n == 26, "the fixture grid is 26 speakers");

    float f = bwa_spcap_focus_default(pos, n);
    printf("      cube grid derives focus %.4f\n", (double)f);
    CHECK(fabs((double)f - 12.7) < 0.2, "the 26-speaker cube grid derives about 12.7");

    /* the same geometry through the INTERNAL derivation the public call is supposed to wrap.
     * The public one builds its Layout from `positions` and centers it with layout_compute_ref,
     * so an identically-built Layout must give the identical float. */
    {
        Layout L;
        memset(&L, 0, sizeof L);
        L.count = n;
        for (uint32_t s = 0; s < n; ++s) {
            L.speakers[s].pos[0] = pos[s*3+0];
            L.speakers[s].pos[1] = pos[s*3+1];
            L.speakers[s].pos[2] = pos[s*3+2];
        }
        layout_compute_ref(&L);
        float internal = layout_derive_spcap_focus(&L);
        printf("      layout_derive_spcap_focus on the same geometry: %.4f\n", (double)internal);
        CHECK(f == internal, "bwa_spcap_focus_default matches layout_derive_spcap_focus exactly");
    }

    /* argument guards: every one of these is 0, not a fallback value */
    CHECK(bwa_spcap_focus_default(NULL, n) == 0.f,               "NULL positions returns 0");
    CHECK(bwa_spcap_focus_default(pos, 0) == 0.f,                "n = 0 returns 0");
    CHECK(bwa_spcap_focus_default(pos, 1) == 0.f,                "n = 1 (nothing to separate) returns 0");
    CHECK(bwa_spcap_focus_default(pos, BWA_CHANNELS + 1) == 0.f, "n > BWA_CHANNELS returns 0");
    CHECK(bwa_spcap_focus_default(pos, 2) > 0.f,                 "n = 2 is the smallest measurable array");

    /* sparser array, broader lobe. Two rings at the same radius, 6 versus 24 speakers: the only
     * thing that differs is the angular spacing. */
    {
        float wide[BWA_CHANNELS * 3], dense[BWA_CHANNELS * 3];
        ring(wide,  6, 2.0f);
        ring(dense, 24, 2.0f);
        float fw = bwa_spcap_focus_default(wide, 6);
        float fd = bwa_spcap_focus_default(dense, 24);
        printf("      6-speaker ring %.2f, 24-speaker ring %.2f\n", (double)fw, (double)fd);
        CHECK(fw < fd, "a wider (sparser) array derives a lower focus than a denser one");
        CHECK(fw >= 1.f && fd <= 64.f, "both stay inside the 1..64 clamp");
    }
}

/* ---- bwa_panner_gains_batch: the new focus/density arguments ---- */

/* how many speakers carry more than `frac` of the loudest one: the concentration measure */
static int lit(const float* g, uint32_t n, float frac) {
    float peak = 0.f;
    for (uint32_t s = 0; s < n; ++s) if (g[s] > peak) peak = g[s];
    int c = 0;
    for (uint32_t s = 0; s < n; ++s) if (g[s] > frac * peak) ++c;
    return c;
}
static double power(const float* g, uint32_t n) {
    double p = 0;
    for (uint32_t s = 0; s < n; ++s) p += (double)g[s] * g[s];
    return sqrt(p);
}

static void test_batch_focus(void) {
    float pos[BWA_CHANNELS * 3];
    uint32_t n = cube_grid(pos);
    const float lis[3]  = { 0.f, 1.5f, 0.f };            /* the grid's sweet spot */
    /* two off-axis bearings, both INSIDE the default 1 m attenuation reference, so the solve's
     * distance term is exactly 1 and ||g|| is the user gain (1.0) rather than a curve value */
    const float srcs[6] = {  0.55f, 1.85f, 0.40f,
                            -0.45f, 1.20f, 0.60f };
    const uint32_t nsrc = 2;
    float g_def[2 * BWA_CHANNELS], g_lo[2 * BWA_CHANNELS], g_hi[2 * BWA_CHANNELS];

    float derived = bwa_spcap_focus_default(pos, n);

    /* well below versus well above the derived default */
    CHECK(bwa_panner_gains_batch(BWA_PAN_SPCAP, pos, n, lis, srcs, nsrc, 3.0f,  0.f, g_lo) == nsrc,
          "SPCAP batch returns nsrc at a low focus");
    CHECK(bwa_panner_gains_batch(BWA_PAN_SPCAP, pos, n, lis, srcs, nsrc, 40.0f, 0.f, g_hi) == nsrc,
          "SPCAP batch returns nsrc at a high focus");
    for (uint32_t i = 0; i < nsrc; ++i) {
        const float* lo = &g_lo[i * n];
        const float* hi = &g_hi[i * n];
        int nlo = lit(lo, n, 0.25f), nhi = lit(hi, n, 0.25f);
        printf("      src %u: focus 3 lights %d speakers, focus 40 lights %d\n", i, nlo, nhi);
        CHECK(nhi < nlo, "a focus above the default concentrates energy on fewer speakers");
        CHECK(fabs(power(lo, n) - 1.0) < 0.02 && fabs(power(hi, n) - 1.0) < 0.02,
              "constant power holds at both focus values");
    }

    /* THE SENTINEL: focus <= 0 must reproduce, bit for bit, what passing the derived value gives.
     * This is what pins the public default to the public derivation. */
    {
        float g_sent[2 * BWA_CHANNELS], g_expl[2 * BWA_CHANNELS];
        bwa_panner_gains_batch(BWA_PAN_SPCAP, pos, n, lis, srcs, nsrc,  0.f,     0.f, g_sent);
        bwa_panner_gains_batch(BWA_PAN_SPCAP, pos, n, lis, srcs, nsrc, derived, 2.f, g_expl);
        int same = 1;
        for (uint32_t k = 0; k < nsrc * n; ++k) if (g_sent[k] != g_expl[k]) same = 0;
        CHECK(same, "focus <= 0 reproduces bwa_spcap_focus_default's value exactly");

        /* and a NEGATIVE argument is the same sentinel, not a distinct value */
        bwa_panner_gains_batch(BWA_PAN_SPCAP, pos, n, lis, srcs, nsrc, -5.f, -5.f, g_sent);
        same = 1;
        for (uint32_t k = 0; k < nsrc * n; ++k) if (g_sent[k] != g_expl[k]) same = 0;
        CHECK(same, "a negative focus/density is the same revert sentinel as 0");

        /* the sentinel derives from the CALLER's array, not the default grid: a ring whose derived
         * focus differs must still agree with its own bwa_spcap_focus_default */
        float rpos[BWA_CHANNELS * 3];
        ring(rpos, 12, 2.0f);
        float rderived = bwa_spcap_focus_default(rpos, 12);
        printf("      12-speaker ring derives %.3f (grid derives %.3f)\n", (double)rderived, (double)derived);
        CHECK(fabsf(rderived - derived) > 0.5f, "the ring's derived focus really differs from the grid's");
        float r_sent[2 * BWA_CHANNELS], r_expl[2 * BWA_CHANNELS];
        bwa_panner_gains_batch(BWA_PAN_SPCAP, rpos, 12, lis, srcs, nsrc, 0.f,       0.f, r_sent);
        bwa_panner_gains_batch(BWA_PAN_SPCAP, rpos, 12, lis, srcs, nsrc, rderived, 2.f, r_expl);
        same = 1;
        for (uint32_t k = 0; k < nsrc * 12; ++k) if (r_sent[k] != r_expl[k]) same = 0;
        CHECK(same, "the sentinel derives from the caller's own geometry");
    }

    /* density reaches the placement correction (the cache is per-call, so this also proves the
     * batch does not carry a stale c[] across calls) */
    {
        float g_d1[2 * BWA_CHANNELS], g_d2[2 * BWA_CHANNELS];
        bwa_panner_gains_batch(BWA_PAN_SPCAP, pos, n, lis, srcs, nsrc, derived, 1.0f, g_d1);
        bwa_panner_gains_batch(BWA_PAN_SPCAP, pos, n, lis, srcs, nsrc, derived, 6.0f, g_d2);
        double dmax = 0;
        for (uint32_t k = 0; k < nsrc * n; ++k) {
            double d = fabs((double)g_d1[k] - g_d2[k]);
            if (d > dmax) dmax = d;
        }
        CHECK(dmax > 1e-5, "the density argument reaches the placement correction");
    }

    /* INERT under DBAP and VBAP: the arguments must not perturb a panner with no lobe */
    {
        const bwa_panner other[2] = { BWA_PAN_DBAP, BWA_PAN_VBAP };
        const char* nm[2] = { "DBAP", "VBAP" };
        for (int p = 0; p < 2; ++p) {
            float a[2 * BWA_CHANNELS], b[2 * BWA_CHANNELS], c[2 * BWA_CHANNELS];
            bwa_panner_gains_batch(other[p], pos, n, lis, srcs, nsrc,  0.f,  0.f, a);
            bwa_panner_gains_batch(other[p], pos, n, lis, srcs, nsrc, 40.f,  6.f, b);
            bwa_panner_gains_batch(other[p], pos, n, lis, srcs, nsrc,  1.5f, 0.1f, c);
            int same = 1;
            for (uint32_t k = 0; k < nsrc * n; ++k) if (a[k] != b[k] || a[k] != c[k]) same = 0;
            printf("      %s unchanged across focus/density: %s\n", nm[p], same ? "yes" : "NO");
            CHECK(same, "focus/density are inert under this panner");
            CHECK(power(a, n) > 0.0, "the panner still produced gains (the check is not vacuous)");
        }
    }

    /* argument guards survive the widened signature */
    {
        float g[2 * BWA_CHANNELS];
        CHECK(bwa_panner_gains_batch(BWA_PAN_SPCAP, NULL, n, lis, srcs, nsrc, 0.f, 0.f, g) == 0,
              "NULL positions returns 0");
        CHECK(bwa_panner_gains_batch(BWA_PAN_SPCAP, pos, n, NULL, srcs, nsrc, 0.f, 0.f, g) == 0,
              "NULL listener returns 0");
        CHECK(bwa_panner_gains_batch(BWA_PAN_SPCAP, pos, n, lis, srcs, nsrc, 0.f, 0.f, NULL) == 0,
              "NULL out returns 0");
        CHECK(bwa_panner_gains_batch(BWA_PAN_SPCAP, pos, BWA_CHANNELS + 1, lis, srcs, nsrc, 0.f, 0.f, g) == 0,
              "n > BWA_CHANNELS returns 0");
        CHECK(bwa_panner_gains_batch(BWA_PAN_SPCAP, pos, n, lis, srcs, 0, 0.f, 0.f, g) == 0,
              "nsrc = 0 returns 0");
    }
}

int main(void) {
    printf("-- bwa_spcap_focus_default --\n");
    test_focus_default();
    printf("-- bwa_panner_gains_batch (focus/density) --\n");
    test_batch_focus();
    if (failures) { printf("tools_api FAILED (%d)\n", failures); return 1; }
    printf("tools_api OK\n");
    return 0;
}
