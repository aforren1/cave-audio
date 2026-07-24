/*
 * fdn_test.c â€” the directional FDN reverb bed (fdn.c), driven off-engine through its RtBusTap
 * interface (the way reflect_test drives the Steam bed). Feeds an impulse down the aux send and
 * checks the rendered 26-ch reverb:
 *   - a tail exists and decays (not silence, not runaway);
 *   - the broadband decay lands near the configured RT60 (Schroeder fit over the clean mid-decay);
 *   - HF decays faster than LF when configured (2-band decay filters work);
 *   - anisotropic decay is real: with the decay scaled down toward +x, the +x side's late energy
 *     dies measurably faster than the -x side's (the Directional-FDN property);
 *   - a long silent run stays exactly finite (lossless-prototype stability + decay losses).
 */
#include "fdn.h"
#include "layout.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CH   BWA_CHANNELS
#define RATE 48000u
#define N    256u

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); ++fails; } } while (0)

/* run `blocks` blocks: aux = impulse at t=0 then silence; sum per-block bus energy into e[]
 * (optionally restricted to speakers passing keep()) */
typedef int (*KeepFn)(const Layout* L, uint32_t s);
static void run_fdn(Fdn* f, const Layout* L, KeepFn keep, uint32_t blocks, double* e) {
    static float bus[CH * N];
    static float aux[N];
    for (uint32_t b = 0; b < blocks; ++b) {
        memset(bus, 0, sizeof bus);
        memset(aux, 0, sizeof aux);
        if (b == 0) aux[0] = 1.f;
        fdn_tap(f, bus, N, NULL, NULL, aux);
        double acc = 0;
        for (uint32_t s = 0; s < L->count; ++s) {
            if (keep && !keep(L, s)) continue;
            const float* p = &bus[(size_t)s * N];
            for (uint32_t i = 0; i < N; ++i) acc += (double)p[i] * p[i];
        }
        e[b] = acc;
    }
}

/* RT60 from the block-energy curve: fit the log-energy slope over [t0,t1] blocks (clean mid-decay,
 * past the build-up, above the float floor). Slope db/block -> seconds to -60 dB. */
static double rt60_fit(const double* e, uint32_t b0, uint32_t b1) {
    double sx = 0, sy = 0, sxx = 0, sxy = 0; int m = 0;
    for (uint32_t b = b0; b < b1; ++b) {
        if (e[b] <= 0) continue;
        double x = (double)b, y = 10.0 * log10(e[b]);
        sx += x; sy += y; sxx += x * x; sxy += x * y; ++m;
    }
    if (m < 8) return 0;
    double slope = (m * sxy - sx * sy) / (m * sxx - sx * sx);   /* dB per block (negative) */
    if (slope >= -1e-9) return 0;
    return (60.0 / -slope) * (double)N / (double)RATE;
}

static int keep_px(const Layout* L, uint32_t s) { return L->speakers[s].pos[0] >  1.0f; }
static int keep_nx(const Layout* L, uint32_t s) { return L->speakers[s].pos[0] < -1.0f; }

int main(void) {
    Layout L = layout_default();
    enum { BLOCKS = 400 };                       /* ~2.1 s of tail at 256/48k */
    static double e[BLOCKS];

    /* 1. decay lands near the configured RT60 (uniform, single-band: low == high) */
    {
        Fdn* f = fdn_create(&L, RATE, CH, 0);
        CHECK(f != NULL, "fdn_create");
        if (f) {
            fdn_set_decay(f, 0.8f, 0.8f, 2000.f);
            run_fdn(f, &L, NULL, BLOCKS, e);
            double tail = 0; for (int b = 20; b < BLOCKS; ++b) tail += e[b];
            CHECK(tail > 1e-12, "FDN produces a reverb tail");
            double rt = rt60_fit(e, 40, 150);    /* mid-decay: past build-up, above the floor */
            printf("fdn: uniform rt60 = %.3f s (want 0.8)\n", rt);
            CHECK(rt > 0.6 && rt < 1.0, "broadband decay lands near the configured RT60");
            CHECK(e[BLOCKS - 1] < e[40], "energy decays monotonically at the scale of the fit");
            fdn_destroy(f);
        }
    }

    /* 1b. EPAD line render (bed_decoder = 2): the knob reaches the FDN — it builds, produces a
     * tail, and the decay is untouched (the decode only changes WHERE the lines land on the
     * array, never the feedback loop). Any other decoder value keeps the house AllRAD. */
    {
        Fdn* f = fdn_create(&L, RATE, CH, 2);
        CHECK(f != NULL, "fdn_create (EPAD render)");
        if (f) {
            fdn_set_decay(f, 0.8f, 0.8f, 2000.f);
            run_fdn(f, &L, NULL, BLOCKS, e);
            double tail = 0; for (int b = 20; b < BLOCKS; ++b) tail += e[b];
            CHECK(tail > 1e-12, "EPAD-rendered FDN produces a reverb tail");
            double rt = rt60_fit(e, 40, 150);
            printf("fdn: epad-render rt60 = %.3f s (want 0.8)\n", rt);
            CHECK(rt > 0.6 && rt < 1.0, "EPAD-rendered FDN decays at the configured RT60");
            fdn_destroy(f);
        }
    }

    /* 2. two-band decay: with rt60_high << rt60_low the late tail is LF-dominated â€” compare the
     * decay measured early (both bands alive) vs late (HF dead): the late slope must be slower. */
    {
        Fdn* f = fdn_create(&L, RATE, CH, 0);
        if (f) {
            fdn_set_decay(f, 1.2f, 0.3f, 1000.f);
            run_fdn(f, &L, NULL, BLOCKS, e);
            double rt_early = rt60_fit(e, 20, 70), rt_late = rt60_fit(e, 150, 280);
            printf("fdn: 2-band rt early %.3f s late %.3f s\n", rt_early, rt_late);
            CHECK(rt_early > 0 && rt_late > rt_early * 1.2,
                  "HF band dies first: the late tail decays at the slower LF rate");
            fdn_destroy(f);
        }
    }

    /* 3. anisotropic decay: decay scaled 0.4x toward +x -> the +x wall's late energy dies faster
     * than the -x wall's. Compare per-side late/early energy ratios (level-independent). */
    {
        Fdn* f = fdn_create(&L, RATE, CH, 0);
        if (f) {
            fdn_set_decay(f, 1.0f, 1.0f, 2000.f);
            fdn_set_decay_direction(f, (const float[3]){ 1, 0, 0 }, 0.4f);
            static double epx[BLOCKS], enx[BLOCKS];
            run_fdn(f, &L, keep_px, BLOCKS, epx);
            fdn_destroy(f);
            f = fdn_create(&L, RATE, CH, 0);        /* fresh state, same excitation, other side */
            fdn_set_decay(f, 1.0f, 1.0f, 2000.f);
            fdn_set_decay_direction(f, (const float[3]){ 1, 0, 0 }, 0.4f);
            run_fdn(f, &L, keep_nx, BLOCKS, enx);
            fdn_destroy(f);
            double px_e = 0, px_l = 0, nx_e = 0, nx_l = 0;
            for (int b = 20;  b < 70;  ++b) { px_e += epx[b]; nx_e += enx[b]; }
            for (int b = 150; b < 280; ++b) { px_l += epx[b]; nx_l += enx[b]; }
            double rpx = px_l / px_e, rnx = nx_l / nx_e;
            printf("fdn: late/early  +x %.4g  -x %.4g\n", rpx, rnx);
            CHECK(px_e > 0 && nx_e > 0 && rpx < 0.5 * rnx,
                  "anisotropic decay: the +x side's tail dies measurably faster than the -x side's");
        }
    }

    /* 4. stability: after the impulse fully decays, a long silent run stays finite and near-zero */
    {
        Fdn* f = fdn_create(&L, RATE, CH, 0);
        if (f) {
            fdn_set_decay(f, 0.3f, 0.3f, 2000.f);
            static double es[BLOCKS];
            run_fdn(f, &L, NULL, BLOCKS, es);    /* 2.1 s at rt60 0.3 -> > 40 dB down */
            int finite = 1;
            for (int b = 0; b < BLOCKS; ++b) if (!isfinite(es[b])) finite = 0;
            CHECK(finite, "output stays finite");
            CHECK(es[BLOCKS - 1] < es[20] * 1e-4, "tail is far down after 7 time-constants (no regeneration)");
            fdn_destroy(f);
        }
    }

    /* 5. max-rE render pair (fdn_set_max_re): the taper only swaps the line->speaker render â€” the
     * decay loop is untouched, so the RT60 must stay put, the level must stay ~fair (energy-
     * normalized weights), and a mid-run toggle (the per-sample crossfade) must stay finite. */
    {
        Fdn* f = fdn_create(&L, RATE, CH, 0);
        if (f) {
            fdn_set_decay(f, 0.8f, 0.8f, 2000.f);
            fdn_set_max_re(f, 1);
            run_fdn(f, &L, NULL, BLOCKS, e);
            double rt = rt60_fit(e, 40, 150);
            printf("fdn: max-rE rt60 = %.3f s (want 0.8)\n", rt);
            CHECK(rt > 0.6 && rt < 1.0, "max-rE render keeps the configured RT60");
            int finite = 1;
            for (int b = 0; b < BLOCKS; ++b) if (!isfinite(e[b])) finite = 0;
            CHECK(finite, "max-rE render stays finite");
            fdn_destroy(f);
        }
        Fdn* f0 = fdn_create(&L, RATE, CH, 0);
        Fdn* f1 = fdn_create(&L, RATE, CH, 0);
        if (f0 && f1) {
            static double e0[BLOCKS], e1[BLOCKS];
            fdn_set_decay(f0, 0.8f, 0.8f, 2000.f);
            fdn_set_decay(f1, 0.8f, 0.8f, 2000.f);
            fdn_set_max_re(f1, 1);
            run_fdn(f0, &L, NULL, 100, e0);
            run_fdn(f1, &L, NULL, 100, e1);
            double s0 = 0, s1 = 0;
            for (int b = 5; b < 100; ++b) { s0 += e0[b]; s1 += e1[b]; }
            printf("fdn: max-rE level delta %.2f dB\n", 10.0 * log10(s1 / s0));
            CHECK(s0 > 0 && fabs(10.0 * log10(s1 / s0)) < 2.0, "max-rE render keeps the reverb level");
            fdn_set_max_re(f0, 1);                   /* toggle a warm instance: the crossfade path */
            static double e2[BLOCKS];
            run_fdn(f0, &L, NULL, 50, e2);
            int finite = 1;
            for (int b = 0; b < 50; ++b) if (!isfinite(e2[b])) finite = 0;
            CHECK(finite, "mid-run max-rE toggle stays finite");
            fdn_destroy(f0); fdn_destroy(f1);
        }
    }

    /* 6. LIVE decay retune (bwa_fdn_set_decay while the tap runs): a mid-run fdn_set_decay must
     * (a) keep the tail continuous — no restart, no click, energy keeps decaying through the seam
     * (the gains ramp across one block) — and (b) actually change the slope: the same tail decays
     * at the OLD rate before the call and at the NEW rate after it. */
    {
        Fdn* f = fdn_create(&L, RATE, CH, 0);
        CHECK(f != NULL, "fdn_create (live retune)");
        if (f) {
            enum { RETUNE = 200 };
            fdn_set_decay(f, 2.0f, 2.0f, 2000.f);
            static float bus[CH * N];
            static float aux[N];
            for (uint32_t b = 0; b < BLOCKS; ++b) {
                if (b == RETUNE) fdn_set_decay(f, 0.25f, 0.25f, 2000.f);   /* the live call under test */
                memset(bus, 0, sizeof bus);
                memset(aux, 0, sizeof aux);
                if (b == 0) aux[0] = 1.f;
                fdn_tap(f, bus, N, NULL, NULL, aux);
                double acc = 0;
                for (uint32_t s = 0; s < L.count; ++s) {
                    const float* p = &bus[(size_t)s * N];
                    for (uint32_t i = 0; i < N; ++i) acc += (double)p[i] * p[i];
                }
                e[b] = acc;
            }
            double rt_slow = rt60_fit(e, 40, 180);
            double rt_fast = rt60_fit(e, RETUNE + 10, RETUNE + 80);
            printf("fdn: live retune rt60 %.3f s -> %.3f s (want 2.0 -> 0.25)\n", rt_slow, rt_fast);
            CHECK(rt_slow > 1.5 && rt_slow < 2.7,  "pre-retune tail decays at the configured slow rate");
            CHECK(rt_fast > 0.15 && rt_fast < 0.40, "post-retune tail decays at the NEW rate — the retune is live");
            int cont = 1, finite = 1;
            for (uint32_t b = RETUNE - 1; b < RETUNE + 6; ++b)   /* a click/restart would spike the energy */
                if (e[b + 1] > e[b] * 2.0) cont = 0;
            for (uint32_t b = 0; b < BLOCKS; ++b) if (!isfinite(e[b])) finite = 0;
            CHECK(cont,   "the tail rides through the retune (no click, no restart)");
            CHECK(finite, "live retune stays finite");
            fdn_destroy(f);
        }
    }

    if (fails) { printf("fdn_test: %d FAILURES\n", fails); return 1; }
    printf("fdn_test OK (tail, RT60 landing, 2-band decay, anisotropic decay, max-rE pair, stability, live retune)\n");
    return 0;
}
