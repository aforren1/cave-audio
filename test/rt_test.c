/*
 * rt_test.c — M2/M4 verification of the concurrency spine, driven off the RT path
 * (single-threaded, deterministic). Routing now goes through real DBAP (M4), so the
 * observable is "a source at speaker k's surveyed position makes channel k dominate".
 * Checks: the commit snapshot, generation stale-drop, play/stop, gain scaling, and the
 * rt_create event-ring bound. (DBAP/align properties live in dsp_test.c.)
 */
#include "rt.h"
#include "layout.h"
#include "dr_wav.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N    256
#define CH   BW_CHANNELS
#define RATE 48000u

static float  bus[CH * N];
static Layout LD;                       /* default layout: speaker positions for the test */

static double chan_energy(int ch) {
    double e = 0; for (int i = 0; i < N; ++i) e += fabs(bus[(size_t)ch * N + i]); return e;
}
static double total_energy(void) {
    double e = 0; for (int i = 0; i < CH * N; ++i) e += fabs(bus[i]); return e;
}
static int argmax_channel(void) {
    int best = 0; double bm = -1;
    for (int ch = 0; ch < CH; ++ch) { double e = chan_energy(ch); if (e > bm) { bm = e; best = ch; } }
    return best;
}
static void render2(RtCore* c) { BwTimestamp ts = { 0, 0 }; rt_render(c, bus, N, &ts); rt_render(c, bus, N, &ts); }

/* place a voice at speaker k's surveyed position (so DBAP localizes it to channel k) */
static void set_pos_spk(RtCore* c, uint32_t h, int k) {
    rt_source_set_pos(c, h, LD.speakers[k].pos[0], LD.speakers[k].pos[1], LD.speakers[k].pos[2]);
}

static int write_const_wav(const char* path, float value, uint32_t frames) {
    drwav_data_format fmt = { drwav_container_riff, DR_WAVE_FORMAT_IEEE_FLOAT, 1, RATE, 32 };
    drwav wav;
    if (!drwav_init_file_write(&wav, path, &fmt, NULL)) return 0;
    float* buf = (float*)malloc((size_t)frames * sizeof(float));
    if (!buf) { drwav_uninit(&wav); return 0; }
    for (uint32_t i = 0; i < frames; ++i) buf[i] = value;
    drwav_uint64 wrote = drwav_write_pcm_frames(&wav, frames, buf);
    free(buf); drwav_uninit(&wav);
    return wrote == frames;
}

static int write_sine_wav(const char* path, double freq, uint32_t frames) {
    drwav_data_format fmt = { drwav_container_riff, DR_WAVE_FORMAT_IEEE_FLOAT, 1, RATE, 32 };
    drwav wav;
    if (!drwav_init_file_write(&wav, path, &fmt, NULL)) return 0;
    float* buf = (float*)malloc((size_t)frames * sizeof(float));
    if (!buf) { drwav_uninit(&wav); return 0; }
    for (uint32_t i = 0; i < frames; ++i) buf[i] = (float)sin(2.0 * 3.14159265358979 * freq * i / RATE);
    drwav_uint64 wrote = drwav_write_pcm_frames(&wav, frames, buf);
    free(buf); drwav_uninit(&wav);
    return wrote == frames;
}
static int write_impulse_wav(const char* path, uint32_t frames) {
    drwav_data_format fmt = { drwav_container_riff, DR_WAVE_FORMAT_IEEE_FLOAT, 1, RATE, 32 };
    drwav wav;
    if (!drwav_init_file_write(&wav, path, &fmt, NULL)) return 0;
    float* buf = (float*)calloc((size_t)frames, sizeof(float));
    if (!buf) { drwav_uninit(&wav); return 0; }
    buf[0] = 1.0f;                                   /* unit impulse at frame 0, silence after */
    drwav_uint64 wrote = drwav_write_pcm_frames(&wav, frames, buf);
    free(buf); drwav_uninit(&wav);
    return wrote == frames;
}
/* render `kb` blocks, summing all 26 channels to mono per sample into out[kb*N]; the per-channel pan
 * gains are all >= 0 for one source, so the mono sum is a scaled copy of the (propagated) source. */
static void render_capture_mono(RtCore* c, float* out, int kb) {
    BwTimestamp ts = { 0, 0 };
    for (int b = 0; b < kb; ++b) {
        rt_render(c, bus, N, &ts);
        for (int i = 0; i < N; ++i) { double s = 0; for (int ch = 0; ch < CH; ++ch) s += bus[(size_t)ch*N + i]; out[b*N + i] = (float)s; }
    }
}
/* same, but move the source on +x from d0 to d1 (one step per block) so the Doppler delay glides */
static void render_capture_mono_moving(RtCore* c, uint32_t h, float d0, float d1, float* out, int kb) {
    BwTimestamp ts = { 0, 0 };
    for (int b = 0; b < kb; ++b) {
        float d = d0 + (d1 - d0) * ((float)b / (float)(kb - 1));
        rt_source_set_pos(c, h, d, 0.f, 0.f); rt_commit(c);
        rt_render(c, bus, N, &ts);
        for (int i = 0; i < N; ++i) { double s = 0; for (int ch = 0; ch < CH; ++ch) s += bus[(size_t)ch*N + i]; out[b*N + i] = (float)s; }
    }
}
static int count_zc(const float* x, int n) {     /* sign changes (zero crossings) */
    int z = 0; for (int i = 1; i < n; ++i) if ((x[i-1] <= 0.f) != (x[i] <= 0.f)) ++z; return z;
}
static int argmax_abs(const float* x, int n) {
    int best = 0; float bm = -1.f; for (int i = 0; i < n; ++i) { float a = fabsf(x[i]); if (a > bm) { bm = a; best = i; } } return best;
}
static double total_l2(void) { double e = 0; for (int i = 0; i < CH * N; ++i) e += (double)bus[i] * bus[i]; return sqrt(e); }
static int active_channels(double frac) {     /* channels carrying > frac of the total energy */
    double tot = total_energy(); int z = 0;
    if (tot <= 0) return 0;
    for (int ch = 0; ch < CH; ++ch) if (chan_energy(ch) > frac * tot) ++z;
    return z;
}

/* write a 4-channel (1st-order AmbiX) wav with constant W/Y/Z/X per frame (ACN order, SN3D) */
static int write_ambix4_wav(const char* path, float w, float y, float z, float x, uint32_t frames) {
    drwav_data_format fmt = { drwav_container_riff, DR_WAVE_FORMAT_IEEE_FLOAT, 4, RATE, 32 };
    drwav wav;
    if (!drwav_init_file_write(&wav, path, &fmt, NULL)) return 0;
    float* buf = (float*)malloc((size_t)frames * 4 * sizeof(float));
    if (!buf) { drwav_uninit(&wav); return 0; }
    for (uint32_t i = 0; i < frames; ++i) { buf[i*4+0]=w; buf[i*4+1]=y; buf[i*4+2]=z; buf[i*4+3]=x; }
    drwav_uint64 wrote = drwav_write_pcm_frames(&wav, frames, buf);
    free(buf); drwav_uninit(&wav);
    return wrote == frames;
}

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", (msg)); ++fails; } } while (0)

/* stub bus tap: counts calls, measures the aux-send energy, and writes a marker onto bus channel 0
 * (to prove the tap can sum onto the bus, like the reflection bed would). */
static int      g_tap_calls;
static uint32_t g_tap_n;
static double   g_aux_energy;
static void test_tap(void* ud, float* bus, uint32_t n, const float* lp, const float* lq, const float* aux) {
    (void)ud; (void)lp; (void)lq;
    ++g_tap_calls; g_tap_n = n;
    double e = 0; for (uint32_t i = 0; i < n; ++i) e += fabs(aux[i]);
    g_aux_energy = e;
    for (uint32_t i = 0; i < n; ++i) bus[0 * (size_t)n + i] += 0.125f;
}

int main(void) {
    LD = layout_default();                          /* listener stays at the origin (centre) */
    const char* WAV = "bw_rt_const.wav";
    if (!write_const_wav(WAV, 1.0f, 8 * N)) { printf("FAIL: write wav\n"); return 1; }

    CHECK(rt_create(1000, 1000, RATE, CH) == NULL, "rt_create rejects caps that could overflow the event ring");

    RtCore* c = rt_create(64, 8, RATE, CH);
    CHECK(c != NULL, "rt_create");
    if (!c) { remove(WAV); return 1; }
    char err[256] = {0};
    uint32_t snd = rt_load_sound(c, WAV, err, sizeof err);
    CHECK(snd != 0, err[0] ? err : "load wav");

    /* 1. a source at speaker 7's position localizes to channel 7 */
    uint32_t h = rt_source_create(c);
    CHECK(h != 0, "source_create returns a valid handle");
    rt_source_play(c, h, snd, true);
    set_pos_spk(c, h, 7);
    rt_commit(c);
    render2(c);
    CHECK(argmax_channel() == 7, "DBAP localizes a source at speaker 7 to channel 7");

    /* 2. commit snapshot: an uncommitted move does NOT relocate the voice */
    set_pos_spk(c, h, 13);
    render2(c);
    CHECK(argmax_channel() == 7, "uncommitted move: still localized to 7");
    rt_commit(c);
    render2(c);
    CHECK(argmax_channel() == 13, "committed move: now localized to 13");

    /* 3. set_gain scales total power (constant-power DBAP, constant 1.0 source) */
    rt_source_set_gain(c, h, 1.0f); rt_commit(c); render2(c);
    double e_full = total_energy();
    rt_source_set_gain(c, h, 0.5f); rt_commit(c); render2(c);
    double e_half = total_energy();
    CHECK(e_full > 0.0 && fabs(e_half / e_full - 0.5) < 0.02, "set_gain(0.5) ~ half the total energy");

    /* 4. stop silences */
    rt_source_stop(c, h);
    render2(c);
    CHECK(total_energy() == 0.0, "stopped voice is silent");

    /* 5. generation handles: destroy + recreate reuses the slot; a stale set is dropped */
    uint32_t old = h;
    rt_source_destroy(c, old);
    render2(c);
    CHECK(total_energy() == 0.0, "destroyed voice is silent");
    uint32_t h2 = rt_source_create(c);
    CHECK(BW_H_IDX(h2) == BW_H_IDX(old), "destroyed slot is reused");
    CHECK(BW_H_GEN(h2) != BW_H_GEN(old), "generation is bumped on reuse");
    set_pos_spk(c, old, 2);                          /* STALE handle -> must be dropped */
    rt_source_play(c, h2, snd, true);
    set_pos_spk(c, h2, 9);                           /* valid */
    rt_commit(c);
    render2(c);
    CHECK(argmax_channel() == 9, "new voice localizes to 9 (valid set applied)");

    /* 6. double-destroy is idempotent (free-list not corrupted) */
    rt_source_destroy(c, h2);
    rt_source_destroy(c, h2);
    render2(c);
    uint32_t h3 = rt_source_create(c);
    CHECK(h3 != 0, "create after double-destroy still works");
    rt_source_play(c, h3, snd, true);
    set_pos_spk(c, h3, 4);
    rt_commit(c); render2(c);
    CHECK(argmax_channel() == 4, "voice after double-destroy localizes correctly");

    /* 7. non-finite inputs rejected at the boundary (no NaN reaches the audio thread) */
    rt_source_set_pos(c, h3, NAN, 0, 0);
    rt_source_set_gain(c, h3, INFINITY);
    rt_commit(c); render2(c);
    double e4 = total_energy();
    CHECK(e4 > 0.0 && isfinite(e4) && argmax_channel() == 4, "voice unmoved and bus finite after NaN/Inf inputs");

    /* 8. occlusion attenuates the mono signal pre-pan (ramped), fed via rt_set_occlusion */
    double e_clear = e4;                              /* h3 un-occluded, at channel 4 */
    rt_set_occlusion(c, h3, 0.5f); render2(c);
    CHECK(fabs(total_energy() / e_clear - 0.5) < 0.05, "occlusion 0.5 ~ half energy");
    rt_set_occlusion(c, h3, 0.0f); render2(c);
    CHECK(total_energy() < e_clear * 0.02, "occlusion 0 ~ silent");
    rt_set_occlusion(c, h3, 1.0f); render2(c);
    CHECK(fabs(total_energy() / e_clear - 1.0) < 0.02, "occlusion restored to 1 ~ full");
    uint32_t stale_occ = BW_MK_H(BW_H_IDX(h3), (uint16_t)(BW_H_GEN(h3) + 7));
    rt_set_occlusion(c, stale_occ, 0.0f); render2(c);
    CHECK(total_energy() > e_clear * 0.5, "occlusion on a stale handle is dropped");

    /* 9. slot recycling clears occlusion: a publish for the prior occupant never attenuates the
     *    voice that reuses its slot (the audio thread gates the publish on its own generation). */
    rt_set_occlusion(c, h3, 0.0f); render2(c);
    CHECK(total_energy() < e_clear * 0.02, "h3 fully occluded before recycling");
    rt_source_destroy(c, h3); render2(c);
    uint32_t h4 = rt_source_create(c);
    CHECK(BW_H_IDX(h4) == BW_H_IDX(h3) && h4 != h3, "occlusion: slot reused with a bumped generation");
    rt_source_play(c, h4, snd, true); set_pos_spk(c, h4, 4); rt_commit(c); render2(c);
    CHECK(total_energy() > e_clear * 0.5, "recycled voice is clear despite the prior occupant's occlusion");

    /* 10. per-band transmission EQ: a low-band cut darkens the (DC) test signal; flat restores it.
     *     The const wav is DC, which sits in the low-shelf band, so band[0] sets its level. */
    double e4b = total_energy();                          /* h4 clear at channel 4 */
    const float lo_cut[3] = { 0.0625f, 1.f, 1.f };        /* kill the low band, keep mid/high */
    rt_set_occlusion_eq(c, h4, 1.0f, lo_cut);
    for (int k = 0; k < 12; ++k) render2(c);             /* let the band-gain glide settle */
    CHECK(total_energy() < e4b * 0.1, "low-band EQ cut darkens the DC signal (level held at 1.0)");
    const float flat[3] = { 1.f, 1.f, 1.f };
    rt_set_occlusion_eq(c, h4, 1.0f, flat);
    for (int k = 0; k < 12; ++k) render2(c);
    CHECK(fabs(total_energy() / e4b - 1.0) < 0.02, "flat EQ restores full level (bypass re-engaged)");

    /* 11. directivity rides its own pre-pan gain ramp (the dir term of rt_set_direct). */
    rt_set_direct(c, h4, 1.0f, flat, 0.5f);
    for (int k = 0; k < 4; ++k) render2(c);
    CHECK(fabs(total_energy() / e4b - 0.5) < 0.05, "directivity 0.5 ~ half (same linear scale as occlusion)");
    rt_set_direct(c, h4, 1.0f, flat, 1.0f);
    for (int k = 0; k < 4; ++k) render2(c);
    CHECK(fabs(total_energy() / e4b - 1.0) < 0.02, "directivity restored to 1 ~ full energy");

    rt_destroy(c);

    /* 12. ambisonic bed: a W-only field decodes equally to all speakers; a front-encoded 1st-order
     *     field favors the front speaker (room -z) over the back one (+z). */
    const char* AMB_OMNI = "bw_amb_omni.wav", *AMB_FRONT = "bw_amb_front.wav";
    if (write_ambix4_wav(AMB_OMNI, 0.5f, 0.f, 0.f, 0.f, 4 * N) &&
        write_ambix4_wav(AMB_FRONT, 0.5f, 0.f, 0.f, 0.5f, 4 * N)) {     /* W,Y,Z,X — X(ACN3) = front */
        RtCore* cb = rt_create(8, 4, RATE, CH);
        CHECK(cb != NULL, "rt_create (bed)");
        if (cb) {
            uint32_t so = rt_load_ambix(cb, AMB_OMNI, err, sizeof err);
            CHECK(so != 0, err[0] ? err : "load ambix omni");
            uint32_t b1 = rt_source_create(cb);
            rt_source_play(cb, b1, so, true);
            rt_commit(cb); render2(cb);
            CHECK(chan_energy(0) > 0.0 && fabs(chan_energy(25) / chan_energy(0) - 1.0) < 0.02,
                  "omni (W-only) bed decodes equally across speakers");
            rt_source_stop(cb, b1); render2(cb);

            int s_front = -1, s_back = -1;
            for (int s = 0; s < CH; ++s)
                if (fabsf(LD.speakers[s].pos[0]) < 0.1f && fabsf(LD.speakers[s].pos[1]) < 0.1f) {
                    if (LD.speakers[s].pos[2] < -1.0f) s_front = s;
                    if (LD.speakers[s].pos[2] >  1.0f) s_back  = s;
                }
            uint32_t sf = rt_load_ambix(cb, AMB_FRONT, err, sizeof err);
            CHECK(sf != 0, "load ambix front");
            uint32_t b2 = rt_source_create(cb);
            rt_source_play(cb, b2, sf, true);
            rt_commit(cb); render2(cb);
            CHECK(s_front >= 0 && s_back >= 0 && chan_energy(s_front) > chan_energy(s_back) * 1.5,
                  "front-encoded bed favors the front speaker");
            rt_destroy(cb);
        }
        remove(AMB_OMNI); remove(AMB_FRONT);
    } else {
        CHECK(0, "could not write ambix test wavs");
    }

    /* 13. bus tap + reflection aux send: the tap is called once per block with the summed mono send
     *     of opted-in voices, and it can sum onto the bus; opting out removes the voice. */
    RtCore* ct = rt_create(8, 4, RATE, CH);
    CHECK(ct != NULL, "rt_create (tap)");
    if (ct) {
        uint32_t st = rt_load_sound(ct, WAV, err, sizeof err);
        uint32_t vt = rt_source_create(ct);
        rt_source_play(ct, vt, st, true);
        rt_source_set_pos(ct, vt, LD.speakers[4].pos[0], LD.speakers[4].pos[1], LD.speakers[4].pos[2]);
        rt_source_set_reflections(ct, vt, true);
        rt_set_bus_tap(ct, test_tap, NULL);
        g_tap_calls = 0; g_aux_energy = 0; g_tap_n = 0;
        rt_commit(ct); render2(ct);
        CHECK(g_tap_calls == 2 && g_tap_n == (uint32_t)N, "bus tap called once per block with the block size");
        CHECK(g_aux_energy > 0.0, "aux send carries the opted-in voice's signal");
        CHECK(chan_energy(0) > 0.0, "tap can sum onto the bus");
        rt_source_set_reflections(ct, vt, false);    /* opt out -> aux is silent */
        g_aux_energy = -1.0; rt_commit(ct); render2(ct);
        CHECK(g_aux_energy == 0.0, "opting out removes the voice from the aux send");
        rt_destroy(ct);
    }

    /* channel test signal: drives a raw output channel (after align), only that channel */
    {
        RtCore* cs = rt_create(8, 4, RATE, CH);
        CHECK(cs != NULL, "rt_create (test signal)");
        if (cs) {
            BwTimestamp ts = { 0, 0 };
            rt_test_signal(cs, 5, 1, 0.5f);      /* sine on channel 5 (drained + injected in one render) */
            rt_render(cs, bus, N, &ts);
            double e5 = chan_energy(5);
            CHECK(e5 > 0.1 && (total_energy() - e5) < 1e-6, "test signal drives only its channel");
            rt_test_signal(cs, 5, 0, 0.0f);      /* off */
            rt_render(cs, bus, N, &ts);
            CHECK(chan_energy(5) < 1e-6, "test signal off -> channel silent");
            rt_destroy(cs);
        }
    }

    /* source spread: widening a point source spreads its energy across more speakers, constant-power */
    {
        RtCore* cs = rt_create(8, 4, RATE, CH);
        CHECK(cs != NULL, "rt_create (spread)");
        if (cs) {
            uint32_t ssnd = rt_load_sound(cs, WAV, err, sizeof err);
            uint32_t hsp = rt_source_create(cs);
            rt_source_play(cs, hsp, ssnd, true);
            set_pos_spk(cs, hsp, 7);                         /* a point at speaker 7 */
            rt_commit(cs); render2(cs);
            int    act_point = active_channels(0.03);
            double l2_point  = total_l2();
            double share_pt  = chan_energy(argmax_channel()) / total_energy();
            rt_source_set_spread(cs, hsp, 1.0f);             /* widen to maximum */
            rt_commit(cs); render2(cs);
            int    act_spread = active_channels(0.03);
            double l2_spread  = total_l2();
            double share_sp   = chan_energy(argmax_channel()) / total_energy();
            CHECK(act_spread > act_point + 2, "spread widens the source across more speakers");
            CHECK(share_sp < share_pt, "spread lowers the dominant channel's share");
            CHECK(l2_point > 0 && fabs(l2_spread - l2_point) / l2_point < 0.02, "spread preserves total power (constant-power)");
            rt_source_destroy(cs, hsp); rt_commit(cs);
            rt_destroy(cs);
        }
    }

    /* propagation effects (opt-in per voice): air absorption (distance low-pass) + Doppler (glided delay) */
    {
        RtCore* cp = rt_create(8, 4, RATE, CH);
        CHECK(cp != NULL, "rt_create (propagation)");
        if (cp) {
            /* air absorption: at a far distance, enabling it dulls an 8 kHz tone (same position, so
             * the panning + distance attenuation are identical — the energy drop is purely the LPF). */
            const char* SW8 = "bw_rt_sine8k.wav";
            if (write_sine_wav(SW8, 8000.0, 8 * N)) {
                uint32_t s8 = rt_load_sound(cp, SW8, err, sizeof err);
                uint32_t hv = rt_source_create(cp);
                rt_source_set_pos(cp, hv, 24.f, 0.f, 0.f);          /* far: air cutoff well below 8 kHz */
                rt_source_play(cp, hv, s8, true);
                rt_commit(cp); render2(cp); render2(cp);
                double e_off = total_energy();
                rt_source_set_air_absorption(cp, hv, true);
                rt_commit(cp); render2(cp); render2(cp);            /* settle the ramped coeff */
                double e_on = total_energy();
                CHECK(e_off > 0.0 && e_on < 0.6 * e_off, "air absorption dulls an 8 kHz tone at distance");
                rt_source_destroy(cp, hv); rt_commit(cp);
                remove(SW8);
            } else CHECK(0, "write 8k sine wav");

            /* Doppler delay: a static source's signal arrives delayed by distance/c. At 3.43 m that's
             * 480 samples (343 m/s, 48 kHz); an impulse peaks there with Doppler on, at ~0 with it off. */
            const char* IW = "bw_rt_impulse.wav";
            if (write_impulse_wav(IW, 16 * N)) {
                uint32_t si = rt_load_sound(cp, IW, err, sizeof err);
                float cap[4 * N];
                uint32_t hoff = rt_source_create(cp);
                rt_source_set_pos(cp, hoff, 3.43f, 0.f, 0.f);
                rt_source_play(cp, hoff, si, false);
                rt_commit(cp);
                render_capture_mono(cp, cap, 4);
                int peak_off = argmax_abs(cap, 4 * N);
                rt_source_destroy(cp, hoff); rt_commit(cp);

                uint32_t hon = rt_source_create(cp);
                rt_source_set_pos(cp, hon, 3.43f, 0.f, 0.f);
                rt_source_set_doppler(cp, hon, true);
                rt_source_play(cp, hon, si, false);
                rt_commit(cp);
                render_capture_mono(cp, cap, 4);
                int peak_on = argmax_abs(cap, 4 * N);
                CHECK(peak_off < 4, "no Doppler -> impulse arrives immediately");
                CHECK(peak_on >= 476 && peak_on <= 484, "Doppler -> impulse delayed by distance/c (~480 samples)");
                rt_source_destroy(cp, hon); rt_commit(cp);
                remove(IW);
            } else CHECK(0, "write impulse wav");

            /* Doppler pitch: an approaching source is pitched up vs a static one (more zero crossings).
             * Both start at 4 m with Doppler on (same initial propagation fill); the moving one glides
             * in to 0.5 m, so its read pointer outruns its write -> the 1 kHz tone resamples higher. */
            const char* SW1 = "bw_rt_sine1k.wav";
            if (write_sine_wav(SW1, 1000.0, 128 * N)) {
                /* The delay smoother is heavy (low cutoff, for a clean spectrum), so warm up past its
                 * group delay + the ring fill, then measure the STEADY-STATE pitch over the tail. */
                enum { WARM = 16, KB = 24, TOT = WARM + KB };
                uint32_t s1 = rt_load_sound(cp, SW1, err, sizeof err);
                float cap[KB * N];
                BwTimestamp ts = { 0, 0 };

                uint32_t hst = rt_source_create(cp);                  /* static reference at 4 m */
                rt_source_set_pos(cp, hst, 4.f, 0.f, 0.f);
                rt_source_set_doppler(cp, hst, true);
                rt_source_play(cp, hst, s1, true); rt_commit(cp);
                for (int b = 0; b < TOT; ++b) {
                    rt_render(cp, bus, N, &ts);
                    if (b >= WARM) for (uint32_t i = 0; i < N; ++i) {
                        double s = 0; for (int ch = 0; ch < CH; ++ch) s += bus[(size_t)ch*N + i]; cap[(b-WARM)*N + i] = (float)s; }
                }
                int zc_static = count_zc(cap, KB * N);
                rt_source_destroy(cp, hst); rt_commit(cp);

                uint32_t hmv = rt_source_create(cp);                  /* constant approach 7.5 m -> 0.5 m */
                rt_source_set_doppler(cp, hmv, true);
                rt_source_play(cp, hmv, s1, true);
                float md = 7.5f; const float mstep = (7.5f - 0.5f) / (TOT - 1);
                for (int b = 0; b < TOT; ++b) {
                    rt_source_set_pos(cp, hmv, md, 0.f, 0.f); rt_commit(cp);
                    rt_render(cp, bus, N, &ts);
                    if (b >= WARM) for (uint32_t i = 0; i < N; ++i) {
                        double s = 0; for (int ch = 0; ch < CH; ++ch) s += bus[(size_t)ch*N + i]; cap[(b-WARM)*N + i] = (float)s; }
                    md -= mstep;
                }
                int zc_moving = count_zc(cap, KB * N);
                CHECK(zc_moving > zc_static + 8, "Doppler: an approaching source is pitched up");
                rt_source_destroy(cp, hmv); rt_commit(cp);
                remove(SW1);
            } else CHECK(0, "write 1k sine wav");
            rt_destroy(cp);
        }
    }

    /* distance->reverb send: the per-source wet send scales with the level, and (in distance mode) with
     * range. Measured via the aux-send energy the bus tap reports (the bed itself needs the SDK). */
    {
        RtCore* cr = rt_create(8, 4, RATE, CH);
        CHECK(cr != NULL, "rt_create (reverb send)");
        if (cr) {
            uint32_t rsnd = rt_load_sound(cr, WAV, err, sizeof err);
            rt_set_bus_tap(cr, test_tap, NULL);
            uint32_t hr = rt_source_create(cr);
            rt_source_play(cr, hr, rsnd, true);
            rt_source_set_pos(cr, hr, 2.f, 0.f, 0.f);
            rt_source_set_reflections(cr, hr, true);             /* full send, no distance scaling */
            g_aux_energy = 0; rt_commit(cr); render2(cr);
            double aux_full = g_aux_energy;
            rt_source_set_reflection_send(cr, hr, 0.5f);         /* halve the send level */
            g_aux_energy = 0; rt_commit(cr); render2(cr);
            double aux_half = g_aux_energy;
            CHECK(aux_full > 0 && fabs(aux_half - aux_full * 0.5) < aux_full * 0.05, "reflection_send scales the wet send");

            rt_source_set_reflection_send(cr, hr, 1.0f);
            rt_source_set_reflection_distance(cr, hr, true);     /* near = drier, far = wetter */
            rt_source_set_pos(cr, hr, 0.5f, 0.f, 0.f);           /* near (< 1 m -> floor send) */
            g_aux_energy = 0; rt_commit(cr); render2(cr);
            double aux_near = g_aux_energy;
            rt_source_set_pos(cr, hr, 8.f, 0.f, 0.f);            /* far (> 6 m -> full send) */
            g_aux_energy = 0; rt_commit(cr); render2(cr);
            double aux_far = g_aux_energy;
            CHECK(aux_near > 0 && aux_far > aux_near * 2.0, "distance->reverb send: far sends more than near");

            /* replay after disabling reflections must not bleed a stale send burst (refl_g_cur reset on play) */
            rt_source_set_reflection_distance(cr, hr, false);
            rt_source_set_pos(cr, hr, 2.f, 0.f, 0.f);
            rt_commit(cr); render2(cr);                          /* send ramps up to full */
            rt_source_stop(cr, hr);
            rt_source_set_reflections(cr, hr, false);            /* disable while stopped */
            rt_commit(cr);
            rt_source_play(cr, hr, rsnd, true);                  /* replay -> refl_g_cur reset to 0 */
            g_aux_energy = -1.0; rt_commit(cr);
            { BwTimestamp ts = { 0, 0 }; rt_render(cr, bus, N, &ts); }   /* first block after replay */
            CHECK(g_aux_energy == 0.0, "replay after disabling reflections sends no stale burst");
            rt_source_destroy(cr, hr); rt_commit(cr);
            rt_destroy(cr);
        }
    }

    remove(WAV);
    if (fails) { printf("rt_test: %d FAILURES\n", fails); return 1; }
    printf("rt_test OK (DBAP, commit, gen-drop, gain, occlusion, EQ, directivity, bed, reflection-tap, channel-test, air+Doppler, spread, reverb-send verified)\n");
    return 0;
}
