/*
 * monitor_test.c — M5 verification of the binaural monitor DSP (no device/audio thread):
 *   - a source on a right-side speaker is louder in the right ear (and vice-versa);
 *   - a source on the median plane (x≈0) is balanced L≈R;
 *   - rotating the head 180° flips left/right;
 *   - the decode is finite and roughly energy-preserving;
 *   - the direct-binaural field (BWA_PROFILE_BINAURAL, cardioid fallback decode): laterality,
 *     the 180° flip, median balance, and the cardioid level;
 *   - the array sim's room (arraysim.h), the stage both decoders read in cave_sim and cave_both:
 *     an impulse on a channel arrives at (d_k - C) / c * rate frames to within a fraction of a
 *     sample, two speakers at different distances come out at the ratio of those distances, the
 *     graphic-EQ directivity matches the model's table at its band centers for two off-axis angles
 *     (the Genelec 4410A model, argv[1]), and with the array's tracked directivity comp ON the
 *     audition differs from the ideal flat response by the comp's two-band residual, which is the
 *     reason the audition fits the full table instead of reusing the comp's two bands. Plus: the bus
 *     it reads is never written, NULL and oversize blocks pass through, and what it costs.
 */
#include "binaural/binaural.h"
#include "binaural/arraysim.h"
#include "core/layout.h"
#include "core/rt.h"             /* the residual check drives the real comp through an rt core */
#include "os/os.h"               /* os_monotonic_ns (the cost print) */
#include "spatial/ambisonics.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CH   BWA_DEFAULT_GRID
#define N    64
#define RATE 48000u

static float bus[CH * N];
static float out[2 * N];

static double e_left(void)  { double e = 0; for (int i = 0; i < N; ++i) e += fabs(out[i]);     return e; }
static double e_right(void) { double e = 0; for (int i = 0; i < N; ++i) e += fabs(out[N + i]); return e; }

/* drive a constant 1.0 on exactly one bus channel, decode, with listener at origin + pose q.
 * The monitor ramps its pan gains one block toward a NEW pose (invariant 4 — no zipper as the head
 * turns), so render twice and measure the settled block when the pose changed from the last call. */
static void decode_channel(Monitor* m, int ch, const float q[4]) {
    const float p[3] = { 0, 1.5f, 0 };   /* the default grid's ear point (floor origin) */
    memset(bus, 0, sizeof bus);
    for (int i = 0; i < N; ++i) bus[(size_t)ch * N + i] = 1.0f;
    monitor_process(m, bus, NULL, p, q, out, N);
    monitor_process(m, bus, NULL, p, q, out, N);
}

/* drive the DIRECT field with a constant-amplitude plane wave from room direction `dir` (the
 * per-channel SH coefficients rt.c's direct solve would produce for a unit signal), silent bus. */
static float dfield[BWA_AMBI_CH * N];
static void decode_direct(Monitor* m, const float dir[3], const float q[4]) {
    const float p[3] = { 0, 1.5f, 0 };
    float y[BWA_AMBI_CH];
    ambi_encode_phonon(dir, y);
    memset(bus, 0, sizeof bus);
    for (int k = 0; k < BWA_AMBI_CH; ++k)
        for (int i = 0; i < N; ++i) dfield[(size_t)k * N + i] = y[k];
    monitor_process(m, bus, dfield, p, q, out, N);
    monitor_process(m, bus, dfield, p, q, out, N);
}

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", (msg)); ++fails; } } while (0)

/* ---- the array sim's room (section 7) ---- */

/* The magnitude (dB) of x[0..n) at f Hz: one DFT bin at exactly f, by a rotating phasor. */
static double dft_db(const float* x, int n, double f) {
    const double w = 2.0 * 3.14159265358979323846 * f / (double)RATE;
    const double cw = cos(w), sw = sin(w);
    double c = 1.0, s = 0.0, re = 0.0, im = 0.0;
    for (int i = 0; i < n; ++i) {
        re += x[i] * c; im -= x[i] * s;
        const double nc = c * cw - s * sw; s = s * cw + c * sw; c = nc;
    }
    return 10.0 * log10(re * re + im * im + 1e-300);
}

/* A unit impulse on channel k at frame 0 through a freshly reset stage at listener p, c = 343 m/s:
 * channel k's output for nb blocks into out (nb * N frames). Also flags a written bus. */
static int asim_bus_written = 0;
static void asim_impulse(ArraySim* s, int k, const float p[3], int nb, float* out) {
    static float in[CH * N], keep[CH * N];
    arraysim_reset(s);
    for (int b = 0; b < nb; ++b) {
        memset(in, 0, sizeof in);
        if (b == 0) in[(size_t)k * N] = 1.f;
        memcpy(keep, in, sizeof in);
        const float* y = arraysim_apply(s, in, p, 343.f, N);
        if (memcmp(keep, in, sizeof in) != 0) asim_bus_written = 1;
        memcpy(out + (size_t)b * N, y + (size_t)k * N, sizeof(float) * N);
    }
}

/* A listener `deg` off speaker k's acoustic axis, at the speaker's own distance from ref: rotate the
 * aim about a perpendicular. */
static void off_axis_point(const Layout* L, int k, float deg, float p[3]) {
    const float* a = L->speakers[k].aim;
    const float* s = L->speakers[k].pos;
    float up[3] = { 0, 1, 0 };
    if (fabsf(a[1]) > 0.9f) { up[0] = 1; up[1] = 0; }
    float u[3] = { a[1]*up[2] - a[2]*up[1], a[2]*up[0] - a[0]*up[2], a[0]*up[1] - a[1]*up[0] };
    const float ul = sqrtf(u[0]*u[0] + u[1]*u[1] + u[2]*u[2]);
    const float dx = L->ref[0]-s[0], dy = L->ref[1]-s[1], dz = L->ref[2]-s[2];
    const float d = sqrtf(dx*dx + dy*dy + dz*dz), r = deg * 3.14159265f / 180.f;
    for (int j = 0; j < 3; ++j) p[j] = s[j] + d * (cosf(r) * a[j] + sinf(r) * u[j] / ul);
}

/* The default grid with the directivity model at `model_path` (a standalone model file, like
 * examples/genelec_4410a_directivity.json), through the real loader. 0 on failure. */
static int load_model_layout(const char* model_path, Layout* out) {
    FILE* f = fopen(model_path, "rb");
    if (!f) return 0;
    static char model[256 * 1024];
    const size_t nm = fread(model, 1, sizeof model - 1, f);
    fclose(f);
    model[nm] = 0;
    static Layout G; layout_default(&G);
    const char* path = "bwa_monitor_model_layout.json";
    FILE* o = fopen(path, "wb");
    if (!o) return 0;
    fprintf(o, "{ \"speakers\": [\n");
    for (uint32_t k = 0; k < G.count; ++k)
        fprintf(o, "  { \"index\": %u, \"position\": [%.4f, %.4f, %.4f] }%s\n", k,
                G.speakers[k].pos[0], G.speakers[k].pos[1], G.speakers[k].pos[2], k + 1 < G.count ? "," : "");
    fprintf(o, "],\n\"directivity\": %s }\n", model);
    fclose(o);
    char err[256] = { 0 };
    const int ok = layout_load(path, RATE, out, err, sizeof err);
    if (!ok) printf("model layout: %s\n", err);
    remove(path);
    return ok && out->dir.nband > 0;
}

/* The residual check's source: a bus tap that plants one impulse on channel `k` at absolute frame
 * `at`, the way the FDN or the reflection bed puts its output on the bus (before align). */
typedef struct { int k; long long at, frame; float amp; } ImpTap;
static void imp_tap(void* ud, float* b, uint32_t n, const float* lp, const float* lq, const float* aux) {
    ImpTap* t = (ImpTap*)ud;
    (void)lp; (void)lq; (void)aux;
    if (t->at >= t->frame && t->at < t->frame + (long long)n) b[(size_t)t->k * n + (size_t)(t->at - t->frame)] += t->amp;
    t->frame += n;
}

/* The full chain for the residual check: an rt core on layout `L` (comp on or off) with the listener
 * at p, settled for 0.5 s (the comp's 24 dB/s glide), then an impulse on channel k, then `s` (the
 * audition's room stage, run every block like the engine does). Channel k's output after the
 * impulse, nb blocks, into out. */
static int chain_impulse(const Layout* L, int comp_on, ArraySim* s, int k, const float p[3], int nb, float* out) {
    RtCore* c = rt_create(4, 4, RATE, CH);
    if (!c) return 0;
    rt_set_layout(c, L);
    rt_set_limiter(c, 0);                            /* linear: an impulse through a treble boost */
    rt_set_tracked_directivity(c, comp_on);
    static ImpTap tap;
    const int settle = (int)(0.5 * RATE / N);
    tap.k = k; tap.at = (long long)settle * N; tap.frame = 0; tap.amp = 0.01f;
    rt_set_bus_tap(c, imp_tap, &tap);
    const float q[4] = { 0, 0, 0, 1 };
    rt_set_listener(c, p, q);
    rt_commit(c);
    arraysim_reset(s);
    static float b[CH * N];
    bwa_timestamp ts = { 0, 0 };
    for (int i = 0; i < settle + nb; ++i) {
        rt_render(c, b, N, &ts);
        const float* y = arraysim_apply(s, b, p, 343.f, N);
        if (i >= settle) memcpy(out + (size_t)(i - settle) * N, y + (size_t)k * N, sizeof(float) * N);
    }
    rt_set_bus_tap(c, NULL, NULL);
    rt_destroy(c);
    return 1;
}

int main(int argc, char** argv) {
    static Layout L; layout_default(&L);
    Monitor* m = monitor_create(&L, RATE);
    CHECK(m != NULL, "monitor_create");
    if (!m) return 1;

    /* find a right (-x), left (+x), and median-plane (x≈0) speaker. Room convention:
     * identity faces +z with +y up (right-handed), so the listener's right is -x. */
    int right = -1, left = -1, center = -1;
    for (int k = 0; k < CH; ++k) {
        float x = L.speakers[k].pos[0];
        if (x < -1.0f) right  = k;
        if (x >  1.0f) left   = k;
        if (fabsf(x) < 0.01f) center = k;
    }
    CHECK(right >= 0 && left >= 0 && center >= 0, "default layout has right/left/median speakers");

    const float ident[4]   = { 0, 0, 0, 1 };       /* head facing forward (+z) */
    const float yaw180[4]  = { 0, 1, 0, 0 };       /* head turned 180° about +y */

    /* 1. right speaker -> right ear louder; left speaker -> left ear louder */
    decode_channel(m, right, ident);
    CHECK(e_right() > e_left() * 1.2, "right speaker is louder in the right ear");
    double rL = e_left(), rR = e_right();
    decode_channel(m, left, ident);
    CHECK(e_left() > e_right() * 1.2, "left speaker is louder in the left ear");

    /* 2. median-plane speaker -> balanced, at the constant-power level (each ear = sqrt(0.5)) */
    decode_channel(m, center, ident);
    CHECK(fabs(e_left() - e_right()) < 0.01 * (e_left() + e_right()) + 1e-6, "median speaker is balanced L≈R");
    CHECK(fabs(e_left() - (double)N * 0.70710678) < 0.02 * (double)N, "median decode at the constant-power level");

    /* 3. rotating the head 180° flips the right speaker to the left ear */
    decode_channel(m, right, yaw180);
    CHECK(e_left() > e_right() * 1.2, "head turned 180°: the right speaker now favors the left ear");
    CHECK(fabs(e_left()  - rR) < 1e-4 && fabs(e_right() - rL) < 1e-4, "180° turn swaps L/R (to float tolerance)");

    /* 4. finite + energy-bearing */
    decode_channel(m, right, ident);
    CHECK(isfinite(e_left()) && isfinite(e_right()) && (e_left() + e_right()) > 0.0, "output finite and audible");

    /* 5. direct-binaural field (the BWA_PROFILE_BINAURAL fallback decode): a plane wave from the
     * listener's right (-x room) favors the right ear (the cardioid there reads 0.5*(1+1) = 1, the
     * opposed one 0), flips on a 180° turn, and balances dead ahead at the 0.5 cardioid level. */
    const float dRight[3] = { -1.f, 0.f, 0.f }, dAhead[3] = { 0.f, 0.f, 1.f };
    decode_direct(m, dRight, ident);
    CHECK(e_right() > e_left() * 1.2, "direct field from the right favors the right ear");
    decode_direct(m, dRight, yaw180);
    CHECK(e_left() > e_right() * 1.2, "direct field: a 180° turn flips the ears");
    decode_direct(m, dAhead, ident);
    CHECK(fabs(e_left() - e_right()) < 0.01 * (e_left() + e_right()) + 1e-6, "direct field ahead is balanced");
    CHECK(fabs(e_left() - (double)N * 0.5) < 0.02 * (double)N, "direct field ahead at the cardioid level");

    /* 6. the bed pass-through diagonal (ambi_canon_to_phonon): a canon-basis encode times the
     * diagonal must equal the monitor-basis encode for ANY direction — pins the (-1)^|m| x
     * orthonormal-rescale table against the shared encode, so it cannot silently drift. */
    {
        const float dirs[4][3] = { { 1, 0, 0 }, { 0, 0, 1 },
                                   { 0.5773503f, 0.5773503f, 0.5773503f }, { -0.7f, 0.14f, 0.7f } };
        double maxerr = 0.0;
        for (int t = 0; t < 4; ++t) {
            float dr[3] = { dirs[t][0], dirs[t][1], dirs[t][2] };
            const float len = sqrtf(dr[0]*dr[0] + dr[1]*dr[1] + dr[2]*dr[2]);
            dr[0] /= len; dr[1] /= len; dr[2] /= len;
            float a[3]; room_to_ambi(dr, a);
            float yc[BWA_AMBI_CH], yp[BWA_AMBI_CH];
            ambi_encode_sn3d(a, yc);
            ambi_encode_phonon(dr, yp);
            for (int k = 0; k < BWA_AMBI_CH; ++k) {
                const double e = fabs((double)yc[k] * ambi_canon_to_phonon[k] - yp[k]);
                if (e > maxerr) maxerr = e;
            }
        }
        CHECK(maxerr < 1e-5, "canon->phonon diagonal matches the monitor-basis encode");
    }

    /* 7. the array sim's room (arraysim.h). */
    {
        enum { NB = 256 };                            /* 256 blocks of 64 = 0.34 s per capture */
        static float y1[NB * N], y2[NB * N], y3[NB * N], y4[NB * N];
        ArraySim* s = arraysim_create(&L, RATE, N);
        CHECK(s != NULL, "arraysim_create (no model)");
        if (s) {
            printf("arraysim: delay ring ceiling %u frames, footprint %.1f KB (26 ch, max_block %d)\n",
                   arraysim_max_delay_frames(s), (double)arraysim_bytes(s) / 1024.0, N);
            /* 7a. arrival time and distance gain. A listener off ref, so every speaker's distance
             * differs from its reference one; two speakers, one near and one far. The impulse's
             * arrival is its CENTROID (the windowed-sinc tap spreads a fractional delay over 16
             * frames, weights summing to 1, centroid within 0.003 frames of the fraction), its total
             * the distance gain. Every arrival carries the tap's fixed ARRAYSIM_BASE_FRAMES. With the
             * delay target built from d_k instead of d_k - C, or the gain left at 1, these went red. */
            const float p[3] = { 0.45f, 1.32f, -0.27f };
            float dref[CH], dp[CH], r0 = 0.f, cmin = 1e9f;
            for (int k = 0; k < CH; ++k) {
                const float* q = L.speakers[k].pos;
                dref[k] = sqrtf((q[0]-L.ref[0])*(q[0]-L.ref[0]) + (q[1]-L.ref[1])*(q[1]-L.ref[1]) + (q[2]-L.ref[2])*(q[2]-L.ref[2]));
                dp[k]   = sqrtf((q[0]-p[0])*(q[0]-p[0]) + (q[1]-p[1])*(q[1]-p[1]) + (q[2]-p[2])*(q[2]-p[2]));
                r0 += dref[k] / CH;
                if (dref[k] < cmin) cmin = dref[k];
            }
            /* near = the closest speaker still arriving after the reference (a closer one clamps at 0) */
            int kn = -1, kf = 0;
            for (int k = 0; k < CH; ++k) {
                if (dp[k] > cmin + 0.1f && (kn < 0 || dp[k] < dp[kn])) kn = k;
                if (dp[k] > dp[kf]) kf = k;
            }
            if (kn < 0) kn = 0;
            asim_bus_written = 0;
            asim_impulse(s, kn, p, NB, y1);
            asim_impulse(s, kf, p, NB, y2);
            double sn = 0, mn = 0, sf = 0, mf = 0;
            for (int i = 0; i < NB * N; ++i) { sn += y1[i]; mn += (double)i * y1[i]; sf += y2[i]; mf += (double)i * y2[i]; }
            const double an = mn / sn, af = mf / sf;
            const double wn = (dp[kn] - cmin) / 343.0 * RATE + ARRAYSIM_BASE_FRAMES,
                         wf = (dp[kf] - cmin) / 343.0 * RATE + ARRAYSIM_BASE_FRAMES;
            printf("arraysim: arrival spk %d %.4f frames (want %.4f), spk %d %.4f (want %.4f); "
                   "gain ratio %.5f (want d_far/d_near %.5f), near gain %.4f (want r0/d %.4f)\n",
                   kn, an, wn, kf, af, wf, sn / sf, dp[kf] / dp[kn], sn, r0 / dp[kn]);
            CHECK(fabs(an - wn) < 0.02 && fabs(af - wf) < 0.02, "arraysim: an impulse arrives at (d_k - C) / c * rate, to a fraction of a sample");
            CHECK(fabs(wn - floor(wn + 0.5)) > 0.05 || fabs(wf - floor(wf + 0.5)) > 0.05, "arraysim: the test delays are fractional (the check can see the fraction)");
            CHECK(fabs(sn / sf - dp[kf] / dp[kn]) < 1e-3 * (dp[kf] / dp[kn]), "arraysim: two speakers come out at the ratio of their distances");
            CHECK(fabs(sn - r0 / dp[kn]) < 1e-3, "arraysim: the distance gain is r0 / d, absolute");
            /* the delay tap is FLAT: the arrival's magnitude at 16 kHz equals its DC total, whatever
             * the fraction. Both test delays are fractional (checked above), and a linear tap there
             * is a low-pass: at these fractions (about 0.8 and 0.23 of a frame) it reads 2 to 3 dB
             * down at 16 kHz, and went red here when the tap was put back to linear. */
            {
                const double w16 = 2.0 * 3.14159265358979 * 16000.0 / RATE;
                double worst = 0.0;
                const float* ys[2] = { y1, y2 };
                const double tot[2] = { sn, sf };
                for (int q = 0; q < 2; ++q) {
                    double re = 0.0, im = 0.0;
                    for (int i = 0; i < NB * N; ++i) { re += ys[q][i] * cos(w16 * i); im -= ys[q][i] * sin(w16 * i); }
                    const double db = 20.0 * log10(sqrt(re * re + im * im) / fabs(tot[q]));
                    if (fabs(db) > fabs(worst)) worst = db;
                }
                printf("arraysim: delay tap at 16 kHz vs DC, worst of the two arrivals %+.3f dB\n", worst);
                CHECK(fabs(worst) < 0.1, "arraysim: the fractional delay tap is flat to 16 kHz (no interpolation low-pass)");
            }
            CHECK(!asim_bus_written, "arraysim: the bus it reads is never written");
            /* at ref the nearest speaker arrives with no added latency */
            asim_impulse(s, kn, L.ref, 4, y1);
            float dly[CH];
            arraysim_state(s, dly, NULL);
            int zero = 0;
            for (int k = 0; k < CH; ++k) if (dref[k] <= cmin + 1e-4f && dly[k] == 0.f) zero = 1;
            CHECK(zero, "arraysim: at ref the nearest speaker's delay is 0");
            /* a listener far outside the stated excursion clamps at the ring's ceiling, never wraps */
            const float far[3] = { 40.f, 1.5f, 0.f };
            asim_impulse(s, 0, far, 2, y1);
            arraysim_state(s, dly, NULL);
            float dmax = 0.f;
            for (int k = 0; k < CH; ++k) if (dly[k] > dmax) dmax = dly[k];
            CHECK(dmax == (float)arraysim_max_delay_frames(s), "arraysim: past the excursion the delay clamps at the ring's ceiling");
            CHECK(arraysim_apply(NULL, bus, p, 343.f, N) == bus, "arraysim: a NULL stage returns the bus unchanged");
            CHECK(arraysim_eq_bands(s, NULL) == 0, "arraysim: no model, no graphic EQ");
            arraysim_destroy(s);
            ArraySim* sc = arraysim_create(&L, RATE, N / 2);
            CHECK(sc && arraysim_apply(sc, bus, p, 343.f, N) == bus, "arraysim: a block past max_block passes through");
            arraysim_destroy(sc);
        }

        /* 7b + 7c need the model: the real Genelec 4410A table through the real loader */
        static Layout G;
        const char* model = argc > 1 ? argv[1] : NULL;
        const int have_model = model && load_model_layout(model, &G);
        CHECK(have_model, "arraysim: load the directivity model (argv[1] = the 4410A model file)");
        if (have_model) {
            ArraySim* sm = arraysim_create(&G, RATE, N);   /* with the model */
            ArraySim* sp = arraysim_create(&L, RATE, N);   /* the same geometry, no model */
            CHECK(sm && sp, "arraysim_create (model / no model)");
            /* the checks walk the MODEL's bands, never the stage's own list: a stage that built no EQ
             * would otherwise hand back zero bands and pass every loop below vacuously */
            const float* hz = G.dir.band_hz;
            const uint32_t nb = G.dir.nband;
            CHECK(sm && arraysim_eq_bands(sm, NULL) == nb, "arraysim: one graphic-EQ section per model band");
            const int K = 7;
            /* 7b. the graphic EQ against the table, at every band center, two angles off axis. Same
             * listener through both stages, so distance and delay cancel and the ratio is the EQ.
             * With the EQ disabled (neq forced to 0) both angles went red. */
            const float angs[2] = { 30.f, 60.f };
            for (int a = 0; a < 2 && sm && sp; ++a) {
                float p[3];
                off_axis_point(&G, K, angs[a], p);
                const float th = layout_speaker_off_axis_deg(&G, (uint32_t)K, p);
                asim_impulse(sm, K, p, NB, y1);
                asim_impulse(sp, K, p, NB, y2);
                double worst = 0.0; float fw = 0.f;
                for (uint32_t j = 0; j < nb; ++j) {
                    const double got  = dft_db(y1, NB * N, hz[j]) - dft_db(y2, NB * N, hz[j]);
                    const double want = directivity_loss_db_at(&G.dir, th, hz[j]);
                    if (fabs(got - want) > worst) { worst = fabs(got - want); fw = hz[j]; }
                }
                printf("arraysim: graphic EQ %.1f deg off axis: worst |EQ - table| %.2f dB (at %.0f Hz) over %u bands; "
                       "table at 8 kHz %+.1f dB\n", th, worst, fw, nb, directivity_loss_db_at(&G.dir, th, 8000.f));
                CHECK(fabs(th - angs[a]) < 0.1f, "arraysim: the test geometry puts the listener at the angle asked for");
                CHECK(worst < 1.0, "arraysim: the graphic EQ is within 1 dB of the model table at every band center");
            }
            /* 7c. the POINT of the full-resolution fit. Speaker K aims at ref, so there the ideal is a
             * flat response. The listener stands 45 deg off its axis with the array's tracked
             * directivity comp ON: the comp's two bands (a broadband gain, a shelf at the split) undo
             * the room's loss only on average per half band, so what the audition plays is flat plus
             * the comp's TWO-BAND RESIDUAL. Measured three ways, each against comp OFF and no room
             * loss (the flat reference):
             *   A = comp on + the room's loss      (the audition)
             *   C = comp on alone                  (what the array sends)
             * and the audition must equal C + the table, band by band (the residual predicted from
             * the model), and must NOT be flat. A monitor sharing the comp's two-band approximation
             * would read flat here to within the approximation's own rounding, which is exactly what
             * the second check refuses. With the audition's EQ disabled, A == C and the first check
             * went red; with its targets swapped for the two half-band means, both went red. */
            if (sm && sp) {
                float p[3];
                off_axis_point(&G, K, 45.f, p);
                const float th = layout_speaker_off_axis_deg(&G, (uint32_t)K, p);
                const int ok = chain_impulse(&G, 1, sm, K, p, NB, y1)     /* A */
                            && chain_impulse(&G, 0, sp, K, p, NB, y2)     /* reference */
                            && chain_impulse(&G, 1, sp, K, p, NB, y3)     /* C */
                            && chain_impulse(&G, 0, sm, K, p, NB, y4);    /* the room's loss alone */
                CHECK(ok, "arraysim: residual chain (rt_create)");
                if (ok) {
                    double worst = 0.0, resid_max = 0.0; float fw = 0.f, fr = 0.f;
                    printf("arraysim: comp ON, listener %.1f deg off speaker %d's axis (ideal: flat):\n"
                           "          band Hz   audition  predicted  comp alone  room alone\n", th, K);
                    for (uint32_t j = 0; j < nb; ++j) {
                        const double ref  = dft_db(y2, NB * N, hz[j]);
                        const double A    = dft_db(y1, NB * N, hz[j]) - ref;
                        const double C    = dft_db(y3, NB * N, hz[j]) - ref;
                        const double R    = dft_db(y4, NB * N, hz[j]) - ref;
                        const double pred = C + directivity_loss_db_at(&G.dir, th, hz[j]);
                        if (fabs(A - pred) > worst) { worst = fabs(A - pred); fw = hz[j]; }
                        if (fabs(A) > resid_max)    { resid_max = fabs(A); fr = hz[j]; }
                        if (j % 3 == 0) printf("          %7.0f  %+7.2f   %+7.2f    %+7.2f     %+7.2f\n", hz[j], A, pred, C, R);
                    }
                    printf("arraysim: audition vs comp + table: worst %.2f dB (at %.0f Hz); the two-band residual "
                           "peaks at %.2f dB (at %.0f Hz)\n", worst, fw, resid_max, fr);
                    CHECK(worst < 1.0, "arraysim: the audition equals the comp plus the model's loss (the residual, predicted)");
                    /* 4 dB: the real residual peaks at 6.4 dB on this model; an audition re-simulating the
                     * room with the comp's own two half-band means (tried on purpose) peaked at 2.2 */
                    CHECK(resid_max > 4.0, "arraysim: the comp's two-band residual is audible in the audition (not flat)");
                }
            }
            /* 7d. what it costs: the whole stage over 26 channels with every EQ section engaged (a
             * listener far off every speaker's axis), 256-frame blocks. Printed, not asserted. */
            if (sm) {
                ArraySim* sb = arraysim_create(&G, RATE, 256);
                static float big[CH * 256];
                for (int i = 0; i < CH * 256; ++i) big[i] = 0.01f * (float)((i * 7919) % 97 - 48);
                const float pc[3] = { 1.1f, 0.4f, 1.2f };
                if (sb) {
                    for (int b = 0; b < 16; ++b) arraysim_apply(sb, big, pc, 343.f, 256);
                    const int reps = 400;
                    const uint64_t t0 = os_monotonic_ns();
                    for (int b = 0; b < reps; ++b) arraysim_apply(sb, big, pc, 343.f, 256);
                    const double us = (double)(os_monotonic_ns() - t0) / 1e3 / reps;
                    printf("arraysim: %.1f us per 256-frame block, 26 channels x %u EQ sections (%.1f%% of the 5.33 ms block)\n",
                           us, nb, 100.0 * us / (256.0 / RATE * 1e6));
                }
                arraysim_destroy(sb);
            }
            arraysim_destroy(sm);
            arraysim_destroy(sp);
        }
    }

    monitor_destroy(m);
    if (fails) { printf("monitor_test: %d FAILURES\n", fails); return 1; }
    printf("monitor_test OK (L/R directionality, median balance, head-rotation flip, direct-field decode, "
           "canon->phonon diagonal, the array sim's room)\n");
    return 0;
}
