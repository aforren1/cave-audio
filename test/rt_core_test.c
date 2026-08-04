/*
 * rt_core_test.c — the concurrency / lifecycle SPINE of rt.c, driven off the RT path
 * (single-threaded, deterministic). This is the "is rt.c's machinery correct" half of the
 * old rt_test monolith: the command rings + commit snapshot + generation-gated handles, voice
 * lifecycle (create/destroy/steal/priority), scheduling (play_at / stop_at / loop regions /
 * gapless queue), streaming + push sources, pause/seek, fades + master gain + groups, the output
 * limiter, the clock / playhead / bus-meter readbacks, and occlusion level/EQ/directivity (kept
 * here as the generation-gating and slot-recycling vehicle, sections 8-11). The other
 * spatial-feature DSP toggles live in rt_feature_test.c. Shared harness (layout, probes, wav
 * writers, taps, CHECK) is rt_test_util.h.
 *
 * The routing still goes through real DBAP, so the spine's observable is "a source at speaker k's
 * surveyed position makes channel k dominate"; the panner/DBAP properties themselves live in
 * dsp_test.c and rt_feature_test.c.
 */
#include "rt_test_util.h"

int main(void) {
    LD = layout_default();                          /* listener stays at the default (the array center, LD.ref) */
    const char* WAV = "bwa_rt_const.wav";
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
    CHECK(BWA_H_IDX(h2) == BWA_H_IDX(old), "destroyed slot is reused");
    CHECK(BWA_H_GEN(h2) != BWA_H_GEN(old), "generation is bumped on reuse");
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
    uint32_t stale_occ = BWA_MK_H(BWA_H_IDX(h3), (uint16_t)(BWA_H_GEN(h3) + 7));
    rt_set_occlusion(c, stale_occ, 0.0f); render2(c);
    CHECK(total_energy() > e_clear * 0.5, "occlusion on a stale handle is dropped");

    /* 9. slot recycling clears occlusion: a publish for the prior occupant never attenuates the
     *    voice that reuses its slot (the audio thread gates the publish on its own generation). */
    rt_set_occlusion(c, h3, 0.0f); render2(c);
    CHECK(total_energy() < e_clear * 0.02, "h3 fully occluded before recycling");
    rt_source_destroy(c, h3); render2(c);
    uint32_t h4 = rt_source_create(c);
    CHECK(BWA_H_IDX(h4) == BWA_H_IDX(h3) && h4 != h3, "occlusion: slot reused with a bumped generation");
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

    /* channel test signal: drives a raw output channel (after align), only that channel */
    {
        RtCore* cs = rt_create(8, 4, RATE, CH);
        CHECK(cs != NULL, "rt_create (test signal)");
        if (cs) {
            bwa_timestamp ts = { 0, 0 };
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

    /* QoL batch: master gain, mix groups (gain + pause), global pause, timed fades, voice gauge.
     * Two voices in different groups at different speakers, so per-group effects read per-channel. */
    {
        RtCore* cq = rt_create(8, 4, RATE, CH);
        CHECK(cq != NULL, "rt_create (qol)");
        if (cq) {
            uint32_t sq = rt_load_sound(cq, WAV, err, sizeof err);
            uint32_t h1 = rt_source_create(cq), h2 = rt_source_create(cq);
            rt_source_play(cq, h1, sq, true); set_pos_spk(cq, h1, 3);  rt_source_set_group(cq, h1, 1);
            rt_source_play(cq, h2, sq, true); set_pos_spk(cq, h2, 20); rt_source_set_group(cq, h2, 2);
            rt_commit(cq); render2(cq);
            CHECK(rt_active_voices(cq) == 2, "active-voice gauge reads 2");
            double e3 = chan_energy(3), e20 = chan_energy(20), l2_base = total_l2();
            CHECK(e3 > 0.1 && e20 > 0.1, "both group voices render");

            rt_set_master_gain(cq, 0.5f);                      /* master: -6 dB over everything */
            render2(cq); render2(cq);
            CHECK(fabs(20.0*log10(total_l2()/l2_base) + 6.02) < 0.3, "master gain scales the whole mix");
            rt_set_master_gain(cq, 1.f); render2(cq); render2(cq);

            rt_group_set_gain(cq, 1, 0.25f);                   /* group 1: -12 dB; group 2 untouched */
            render2(cq); render2(cq);
            CHECK(fabs(20.0*log10(chan_energy(3)/e3) + 12.04) < 0.8, "group gain scales its members");
            CHECK(fabs(20.0*log10(chan_energy(20)/e20)) < 0.3,       "other groups untouched");
            rt_group_set_gain(cq, 1, 1.f); render2(cq); render2(cq);

            rt_group_set_paused(cq, 2, true);                  /* group pause: silent, frozen, still 'playing' */
            render2(cq); render2(cq);
            CHECK(chan_energy(20) < e20 * 0.02, "paused group is silent (only voice-1's DBAP leakage remains)");
            CHECK(chan_energy(3) > 0.1, "unpaused group keeps playing");
            CHECK(rt_source_is_playing(cq, h2), "a group-paused voice still reads as playing");
            rt_group_set_paused(cq, 2, false); render2(cq);
            CHECK(chan_energy(20) > 0.05, "group resume");

            rt_set_all_paused(cq, 1);                          /* global pause: everything out, everything back */
            render2(cq); render2(cq);
            CHECK(total_energy() < 1e-6, "global pause silences the mix");
            rt_set_all_paused(cq, 0); render2(cq);
            CHECK(total_energy() > 0.1, "global resume");

            rt_source_fade_to(cq, h1, 0.25f, 0.1f, false);     /* timed fade: glide to -12 dB over 0.1 s */
            for (int b = 0; b < 5; ++b) render2(cq);           /* ~0.053 s: mid-fade */
            double e_mid = chan_energy(3);
            for (int b = 0; b < 10; ++b) render2(cq);          /* well past the landing */
            double e_end = chan_energy(3);
            CHECK(e_mid < e3 * 0.95 && e_mid > e_end * 1.1, "fade glides through intermediate levels");
            CHECK(fabs(20.0*log10(e_end/e3) + 12.04) < 0.8,   "fade lands on its target");

            rt_source_fade_to(cq, h2, 0.f, 0.05f, true);       /* fade-out-and-stop */
            for (int b = 0; b < 15; ++b) render2(cq);
            CHECK(!rt_source_is_playing(cq, h2), "fade_out stops the voice once landed");
            CHECK(chan_energy(20) < e20 * 0.02, "faded-out voice is silent (only leakage remains)");
            CHECK(rt_active_voices(cq) == 1, "the gauge tracks the stop");

            rt_source_destroy(cq, h1); rt_source_destroy(cq, h2); rt_commit(cq);
            rt_destroy(cq);
        }
    }

    /* runtime channel count: a 24-speaker layout drives a 24-channel core end to end — point panning,
     * a bed decode, meters — and a canary proves NOTHING writes beyond the active channel count into
     * a capacity-sized buffer (the exact overrun class the BWA_CHANNELS->count migration must prevent). */
    {
        RtCore* c24 = rt_create(8, 4, RATE, 24);
        CHECK(c24 != NULL, "rt_create (24 ch)");
        if (c24) {
            Layout L24 = layout_default();
            L24.count = 24;                              /* the first 24 grid speakers, indices 0..23 */
            layout_compute_ref(&L24);
            rt_set_layout(c24, &L24);
            uint32_t s24 = rt_load_sound(c24, WAV, err, sizeof err);
            uint32_t h24 = rt_source_create(c24);
            rt_source_play(c24, h24, s24, true);
            rt_source_set_pos(c24, h24, L24.speakers[5].pos[0], L24.speakers[5].pos[1], L24.speakers[5].pos[2]);
            rt_commit(c24);
            static float b24[CH * N];
            for (int i = 24 * (int)N; i < CH * (int)N; ++i) b24[i] = 123.f;   /* canary beyond channel 24 */
            bwa_timestamp t24 = { 0, 0 };
            rt_render(c24, b24, N, &t24); rt_render(c24, b24, N, &t24);
            int best = 0; double bm = -1;
            for (int ch = 0; ch < 24; ++ch) {            /* 24-wide PLANAR indexing */
                double e = 0; for (int i = 0; i < (int)N; ++i) e += fabs(b24[(size_t)ch * N + i]);
                if (e > bm) { bm = e; best = ch; }
            }
            CHECK(best == 5, "24-ch: a source at speaker 5 localizes to channel 5");
            float pk[CH];
            CHECK(rt_bus_peaks(c24, pk, CH) == 24, "24-ch: the meter readback reports 24 channels");
            const char* B24 = "bwa_rt_bed24.wav";         /* a bed too: SH->24 decode, same canary */
            if (write_ambix4_noise_wav(B24, 1.f, 0.f, 0.f, 1.f, 4 * N)) {
                uint32_t sb5 = rt_load_ambix(c24, B24, err, sizeof err);
                uint32_t hb5 = rt_source_create(c24);
                rt_source_play(c24, hb5, sb5, true);
                rt_commit(c24);
                rt_render(c24, b24, N, &t24); rt_render(c24, b24, N, &t24);
                double etot = 0; for (int i = 0; i < 24 * (int)N; ++i) etot += fabs(b24[i]);
                CHECK(etot > 0.1, "24-ch: the bed decodes onto the 24 active channels");
                rt_source_destroy(c24, hb5); rt_commit(c24);
                remove(B24);
            } else CHECK(0, "write 24-ch bed");
            int canary_ok = 1;
            for (int i = 24 * (int)N; i < CH * (int)N; ++i) if (b24[i] != 123.f) canary_ok = 0;
            CHECK(canary_ok, "24-ch: nothing writes beyond the active channel count");
            rt_source_destroy(c24, h24); rt_commit(c24);
            rt_destroy(c24);
        }
    }

    /* voice priority + stealing: a create on a full pool stops the lowest-priority active source */
    {
        RtCore* cs = rt_create(4, 4, RATE, CH);
        CHECK(cs != NULL, "rt_create (steal)");
        if (cs) {
            uint32_t ss = rt_load_sound(cs, WAV, err, sizeof err);
            uint32_t hh[4];
            for (int i = 0; i < 4; ++i) {
                hh[i] = rt_source_create(cs);
                rt_source_play(cs, hh[i], ss, true);
                rt_source_set_priority(cs, hh[i], i == 2 ? 10 : 200);   /* hh[2] is the expendable one */
            }
            rt_commit(cs); render2(cs);
            CHECK(rt_source_is_playing(cs, hh[2]) && rt_source_is_playing(cs, hh[0]), "4 voices fill the pool");
            uint32_t h5 = rt_source_create(cs);                        /* pool full -> steal hh[2] (priority 10) */
            CHECK(h5 != 0, "create on a full pool succeeds by stealing");
            rt_source_play(cs, h5, ss, true);
            rt_commit(cs); render2(cs);
            CHECK(!rt_source_is_playing(cs, hh[2]), "the lowest-priority voice was stolen");
            CHECK(rt_source_is_playing(cs, hh[0]) && rt_source_is_playing(cs, h5), "higher-priority + new voices survive");
            rt_destroy(cs);
        }
    }

    /* voice-steal is CLICK-FREE: the stolen voice fades out over one block on its own slot (the new
     * source starts on a reserve slot), instead of a hard cut. Pool of 1 isolates it; the stealing
     * source is created but NOT played, so the steal block contains only the victim's fade. */
    {
        RtCore* cf = rt_create(1, 4, RATE, CH);
        if (cf) {
            bwa_timestamp ts0 = { 0, 0 };
            uint32_t sf = rt_load_sound(cf, WAV, err, sizeof err);   /* WAV = constant 1.0 */
            uint32_t a = rt_source_create(cf);
            rt_source_play(cf, a, sf, true);
            rt_source_set_priority(cf, a, 10);
            rt_commit(cf); render2(cf);
            double e_full = total_energy();
            CHECK(e_full > 1e-6, "steal fade: baseline voice audible");
            uint32_t b = rt_source_create(cf);                       /* pool full (1) -> steal a; leave b unplayed */
            CHECK(b != 0, "steal fade: create-on-full succeeds by stealing");
            rt_commit(cf);
            rt_render(cf, bus, N, &ts0);  double e_fade  = total_energy();   /* the steal block: a fades */
            rt_render(cf, bus, N, &ts0);  double e_after = total_energy();   /* next block: a gone, b silent */
            CHECK(e_fade > 0.05 * e_full, "steal fade: the stolen voice fades, not a hard cut to silence");
            CHECK(e_fade < e_full,        "steal fade: fading DOWN, not still full");
            CHECK(e_after < 1e-6,         "steal fade: silent the block after the fade completes");
            rt_destroy(cf);
        }
    }

    /* sample-accurate scheduled play: a voice is held silent until its start_sample, then fires */
    {
        RtCore* cp = rt_create(4, 4, RATE, CH);
        CHECK(cp != NULL, "rt_create (schedule)");
        if (cp) {
            uint32_t sp = rt_load_sound(cp, WAV, err, sizeof err);
            uint32_t h  = rt_source_create(cp);
            rt_source_set_pos(cp, h, 1.f, 0.f, 1.f);
            rt_source_play_at(cp, h, sp, true, (uint64_t)3 * N);    /* start at the 4th block */
            rt_commit(cp);
            double pre = 0; uint64_t pos = 0;
            for (int blk = 0; blk < 3; ++blk) {                    /* blocks spanning [0, 3N): held silent */
                bwa_timestamp ts = { pos, 0 };
                rt_render(cp, bus, N, &ts);
                pre += total_l2();
                pos += N;
            }
            CHECK(pre < 1e-6, "scheduled voice is silent before its start_sample");
            CHECK(rt_source_get_position(cp, h) == 0, "position reads 0 while the scheduled start holds");
            bwa_timestamp ts3 = { pos, 0 };                          /* pos == 3N: the voice fires here */
            rt_render(cp, bus, N, &ts3);
            CHECK(total_l2() > 1e-3, "scheduled voice fires at its start_sample");
            CHECK(rt_source_get_position(cp, h) == (uint64_t)N, "position starts counting at the scheduled start");
            CHECK(rt_dsp_time(cp) == (uint64_t)3 * N, "rt_dsp_time tracks the device sample clock");
            /* device clock pair (rt_get_clock): every render above carried system_time_ns == 0
             * (no host stamp), so no pair exists yet; a stamped block publishes exactly its
             * (sample, time); an unstamped one KEEPS the last valid pair rather than clobbering it */
            uint64_t cs = 1, ct = 1;
            CHECK(!rt_get_clock(cp, &cs, &ct), "no clock pair before a host-stamped block renders");
            bwa_timestamp tsc = { (uint64_t)4 * N, 123456789ull };
            rt_render(cp, bus, N, &tsc);
            CHECK(rt_get_clock(cp, &cs, &ct) && cs == (uint64_t)4 * N && ct == 123456789ull,
                  "the clock pair publishes the sink's (sample, host-time) stamp");
            bwa_timestamp tsc2 = { (uint64_t)5 * N, 0 };            /* driver dropped kSystemTimeValid */
            rt_render(cp, bus, N, &tsc2);
            CHECK(rt_get_clock(cp, &cs, &ct) && cs == (uint64_t)4 * N && ct == 123456789ull,
                  "an unstamped block keeps the last valid pair");
            bwa_timestamp tsc3 = { (uint64_t)6 * N, 223456789ull };
            rt_render(cp, bus, N, &tsc3);
            CHECK(rt_get_clock(cp, &cs, &ct) && cs == (uint64_t)6 * N && ct == 223456789ull,
                  "the pair tracks each stamped block");
            rt_destroy(cp);
        }
    }

    /* clock drift model (rt_get_clock_model): the device-vs-host SLOPE, fitted over the same block
     * stamps. Drive a host clock deliberately out of step with the sample clock and the fit must
     * recover the ppm — this is what makes a minutes-long extrapolation hold instead of walking off
     * at the nominal rate. `drift` is extra host seconds per sample, so the device runs SLOW against
     * the host and ppm comes out negative: rate_hz = RATE / (1 + drift). */
    {
        RtCore* cd = rt_create(4, 4, RATE, CH);
        CHECK(cd != NULL, "rt_create (clock model)");
        if (cd) {
            const double   drift = 1e-5;                  /* -9.9999 ppm */
            const uint64_t epoch = 1000000000ull;         /* nonzero host epoch (time 0 reads as unstamped) */
            RtClockFit f;
            CHECK(!rt_get_clock_model(cd, &f), "no clock model before a stamped block renders");

            uint64_t k = 0;
            #define STAMP_BLOCK(cc, kk, dd) do {                                                  \
                bwa_timestamp t_;                                                                 \
                t_.sample_pos     = (kk) * N;                                                     \
                t_.system_time_ns = epoch + (uint64_t)((double)((kk) * N) / (double)RATE          \
                                                       * (1.0 + (dd)) * 1e9 + 0.5);               \
                rt_render((cc), bus, N, &t_);                                                     \
            } while (0)

            for (; k < 100; ++k) STAMP_BLOCK(cd, k, drift);          /* ~0.53 s: under the min span */
            CHECK(!rt_get_clock_model(cd, &f), "clock model withholds a slope until it has ~1 s of span");
            for (; k < 1200; ++k) STAMP_BLOCK(cd, k, drift);         /* ~6.4 s */

            CHECK(rt_get_clock_model(cd, &f), "clock model publishes once the fit has span");
            CHECK(fabs(f.ppm + 9.9999) < 0.05, "clock model recovers the drift in ppm");
            CHECK(fabs(f.rate_hz - (double)RATE / (1.0 + drift)) < 0.01, "clock model fits the device rate");
            CHECK(f.ppm_sigma > 0.0 && f.ppm_sigma < 0.5, "clock model reports a tight standard error on clean stamps");
            CHECK(f.jitter_ns >= 0.0 && f.jitter_ns < 5.0, "clock model reports near-zero jitter on clean stamps");
            CHECK(f.span_s > 5.0 && f.span_s < 7.0, "clock model reports its accumulated span");
            CHECK(f.stamps > 1000, "clock model counts the stamps behind the fit");

            /* a restart re-bases the device sample position while the host clock runs on: fitting a
             * line through that jump would report fiction, so the fit reseeds and goes quiet */
            bwa_timestamp restart = { 0, epoch + 20000000000ull };
            rt_render(cd, bus, N, &restart);
            CHECK(!rt_get_clock_model(cd, &f), "a re-based sample position reseeds the fit");

            /* ...and it re-converges on the new run's slope, not a blend with the old one */
            const double drift2 = 2e-5;                              /* -19.9996 ppm */
            for (k = 0; k < 1200; ++k) {
                bwa_timestamp t2;
                t2.sample_pos     = k * N;
                t2.system_time_ns = epoch + 20000000000ull
                                  + (uint64_t)((double)(k * N) / (double)RATE * (1.0 + drift2) * 1e9 + 0.5);
                rt_render(cd, bus, N, &t2);
            }
            CHECK(rt_get_clock_model(cd, &f) && fabs(f.ppm + 19.9996) < 0.05,
                  "the reseeded fit converges on the new run's drift");

            /* jitter_ns is a real measurement, not a rounding artifact: hold the rate at nominal and
             * dither the host stamp by a 4 us square wave (rms 2 us about its mean). The slope must
             * survive it — white stamp noise averages out over a thousand blocks — while jitter_ns
             * reports it. This is the assertion the closed-form residual could not pass. */
            const uint64_t ep3 = epoch + 40000000000ull;
            bwa_timestamp restart2 = { 0, ep3 };
            rt_render(cd, bus, N, &restart2);
            for (k = 0; k < 1200; ++k) {
                bwa_timestamp t3;
                t3.sample_pos     = k * N;
                t3.system_time_ns = ep3 + (uint64_t)((double)(k * N) / (double)RATE * 1e9 + 0.5)
                                  + ((k & 1) ? 4000ull : 0ull);
                rt_render(cd, bus, N, &t3);
            }
            CHECK(rt_get_clock_model(cd, &f), "clock model publishes through stamp jitter");
            CHECK(fabs(f.jitter_ns - 2000.0) < 200.0, "jitter_ns measures the injected stamp jitter");
            CHECK(fabs(f.ppm) < 0.5, "the slope survives white stamp jitter");
            #undef STAMP_BLOCK
            rt_destroy(cd);
        }
    }

    /* streaming: a streamed sound feeds the mixer through the background ring (the standalone ring
     * mechanics are covered by stream_test; here we verify the rt integration produces audio). */
    {
        RtCore* cst = rt_create(4, 4, RATE, CH);
        CHECK(cst != NULL, "rt_create (stream)");
        if (cst) {
            uint32_t ss = rt_load_sound_streaming(cst, WAV, err, sizeof err);
            CHECK(ss != 0, err[0] ? err : "rt_load_sound_streaming");
            if (ss) {
                uint32_t h = rt_source_create(cst);
                rt_source_set_pos(cst, h, 1.f, 0.f, 1.f);
                rt_source_play(cst, h, ss, true);   /* loop a short file */
                rt_commit(cst);
                Sleep(60);                          /* let the streaming thread fill the ring */
                double e = 0;
                for (int blk = 0; blk < 30; ++blk) { render2(cst); e += total_l2(); }
                CHECK(e > 1e-3, "streamed voice produces audio through the mixer");
            }
            rt_destroy(cst);
        }
    }

    /* push (procedural) source: caller-pushed PCM plays through the full mix path; an underrun
     * renders silence WITHOUT ending the voice or losing the caller's place; push_end drains then
     * ends; the internal sound slot retires with the source handle (cycles don't exhaust tables). */
    {
        RtCore* cps = rt_create(4, 4, RATE, CH);
        CHECK(cps != NULL, "rt_create (push)");
        if (cps) {
            char perr[256] = {0};
            uint32_t h = rt_source_create_stream(cps, perr, sizeof perr);
            CHECK(h != 0, perr[0] ? perr : "rt_source_create_stream");
            CHECK(rt_source_is_push(cps, h), "push source reads as push");
            /* the play is still queued (no render yet) — a pending play must READ as playing, or the
             * documented create->push->push_end->poll->destroy flow drops the clip in the first-block
             * window (the poll sees false, the caller destroys early) */
            CHECK(rt_source_is_playing(cps, h), "push: pending play reads as playing before the first block");
            rt_source_set_pos(cps, h, 1.f, 0.f, 1.f);
            rt_commit(cps);
            render2(cps);                                       /* binds + consumes an EMPTY ring */
            CHECK(total_l2() < 1e-9, "push: silent before any data (underrun, not garbage)");
            CHECK(rt_source_is_playing(cps, h), "push: an empty ring does not end the voice");
            CHECK(rt_source_get_position(cps, h) == 0, "push: position 0 before any data arrives");

            float pblk[2 * N];
            for (int i = 0; i < 2 * N; ++i) pblk[i] = 0.5f;
            uint32_t space = rt_source_push_space(cps, h);
            CHECK(space >= 2 * N, "push: space available");
            CHECK(rt_source_push(cps, h, pblk, 2 * N) == 2 * N, "push accepts two blocks");
            CHECK(rt_source_push_space(cps, h) == space - 2 * N, "push: space accounts for the pushed frames");
            render2(cps);                                       /* consumes both pushed blocks */
            CHECK(total_l2() > 1e-3, "pushed audio reaches the bus");
            bwa_timestamp pts = { 0, 0 };
            rt_render(cps, bus, N, &pts);
            CHECK(total_l2() < 1e-9, "underrun after the pushed data: silence again");
            CHECK(rt_source_is_playing(cps, h), "underrun does not end the voice");
            CHECK(rt_source_get_position(cps, h) == (uint64_t)2 * N,
                  "push: position counts frames CONSUMED — an underrun slips it, never advances it");
            /* That silence is a STARVE, and it has to be countable: it sounds identical to the end
               of an asset, which is exactly why bwa_health separates the two. The empty-ring render
               above is one too, so the count is simply nonzero rather than pinned. */
            CHECK(rt_stream_starves(cps) > 0, "an empty ring counts as a stream starve");

            /* data-driven clock: audio pushed after an underrun still plays (nothing was skipped) */
            CHECK(rt_source_push(cps, h, pblk, N) == N, "push after an underrun");
            rt_source_push_end(cps, h);
            CHECK(rt_source_push(cps, h, pblk, N) == 0, "push after push_end is refused");
            CHECK(rt_source_push_space(cps, h) == 0, "space is 0 after push_end");
            rt_render(cps, bus, N, &pts);                       /* the tail drains this block */
            CHECK(total_l2() > 1e-3, "the tail pushed after the underrun still plays");
            rt_render(cps, bus, N, &pts);
            CHECK(!rt_source_is_playing(cps, h), "voice ends once the pushed data drains");

            /* rebinding a push source to a loaded asset is refused (the ring is the content) */
            uint32_t sq = rt_load_sound(cps, WAV, err, sizeof err);
            CHECK(sq != 0, "push: load asset for the rebind-refusal check");   /* sq==0 would pass vacuously */
            rt_source_play(cps, h, sq, true);
            render2(cps);
            CHECK(total_l2() < 1e-9, "rt_source_play on a push source is refused");

            /* handle death retires the internal sound: 8 create/destroy cycles through a 4-slot
             * sound table only pass if each destroy recycles its slot (retire-ack per cycle) */
            rt_source_destroy(cps, h);
            CHECK(rt_source_push(cps, h, pblk, N) == 0, "push on a destroyed handle is dropped");
            CHECK(!rt_source_is_playing(cps, h), "destroyed push handle reads not playing");
            render2(cps); rt_commit(cps);                       /* retire lands; the ack drains on commit */
            for (int k = 0; k < 8; ++k) {
                uint32_t hk = rt_source_create_stream(cps, perr, sizeof perr);
                CHECK(hk != 0, "push sound slots recycle across create/destroy cycles");
                if (!hk) break;
                rt_source_destroy(cps, hk);
                render2(cps); rt_commit(cps);
            }

            /* stop ENDS a push source one-way (like push_end): a stopped push voice cannot re-arm
             * (play is refused), so pushes must be refused too — not silently swallowed forever */
            uint32_t hst = rt_source_create_stream(cps, perr, sizeof perr);
            CHECK(hst != 0, "push: create for the stop test");
            if (hst) {
                CHECK(rt_source_push(cps, hst, pblk, N) == N, "push: feed before stop");
                rt_source_stop(cps, hst);
                CHECK(rt_source_push(cps, hst, pblk, N) == 0, "push after stop is refused (stop ends the stream)");
                CHECK(rt_source_push_space(cps, hst) == 0, "push: no space after stop");
                render2(cps);
                CHECK(!rt_source_is_playing(cps, hst), "stop finalizes the push voice");
                rt_source_destroy(cps, hst);
                render2(cps); rt_commit(cps);
            }
            rt_destroy(cps);
        }
    }

    /* steal reaps a DRAINED push source (playing=false after push_end, handle still held — every push
     * source's normal terminal state): the steal must finalize + ack immediately (there is no fade to
     * wait for), recycling the voice slot AND retiring the internal sound. Without that the control
     * side waits forever (stealing[] sticks) and the sound/stream slots leak. */
    {
        RtCore* cs = rt_create(4, 4, RATE, CH);
        CHECK(cs != NULL, "rt_create (steal-push)");
        if (cs) {
            char perr[256] = {0};
            float blk[N]; for (int i = 0; i < N; ++i) blk[i] = 0.25f;
            uint32_t hs[4] = {0};
            for (int i = 0; i < 4; ++i) {
                hs[i] = rt_source_create_stream(cs, perr, sizeof perr);
                CHECK(hs[i] != 0, "steal-push: fill the pool");
                if (!hs[i]) break;
                rt_source_push(cs, hs[i], blk, N);
                rt_source_push_end(cs, hs[i]);
            }
            render2(cs); render2(cs);                       /* bind, play the block, drain, end */
            for (int i = 0; i < 4; ++i) CHECK(!rt_source_is_playing(cs, hs[i]), "steal-push: victim drained");
            /* the user pool (4) is full of drained-but-held push sources: this create must steal one */
            uint32_t hn = rt_source_create(cs);
            CHECK(hn != 0, "steal-push: create steals a drained victim");
            render2(cs);                                    /* CMD_SRC_STEAL -> immediate EVT (victim silent) */
            rt_commit(cs);                                  /* ack: victim recycles, its internal sound retires */
            render2(cs); rt_commit(cs);                     /* retire-ack: the ring closes, the slot frees */
            int live = 0; for (int i = 0; i < 4; ++i) live += rt_source_is_push(cs, hs[i]) ? 1 : 0;
            CHECK(live == 3, "steal-push: exactly one drained victim recycled");
            /* the victim's sound slot must be free again: a new push source fits the 4-slot table
             * (it steals another drained victim for the VOICE and needs the freed SOUND slot) */
            uint32_t hp2 = rt_source_create_stream(cs, perr, sizeof perr);
            CHECK(hp2 != 0, "steal-push: the steal retired the internal sound (slot reusable)");
            rt_destroy(cs);
        }
    }

    /* steal of a PLAYING push source rides the fade path: stopping=2 -> fade -> EVT_VOICE_ENDED ->
     * push_sound_release. Same contract as the drained case, different audio-side route. */
    {
        RtCore* cp = rt_create(4, 4, RATE, CH);
        CHECK(cp != NULL, "rt_create (steal-playing-push)");
        if (cp) {
            char perr[256] = {0};
            float blk[N]; for (int i = 0; i < N; ++i) blk[i] = 0.25f;
            uint32_t hs[4] = {0};
            for (int i = 0; i < 4; ++i) {
                hs[i] = rt_source_create_stream(cp, perr, sizeof perr);
                CHECK(hs[i] != 0, "steal-playing: fill the pool");
                if (!hs[i]) break;
                for (int k = 0; k < 16; ++k) rt_source_push(cp, hs[i], blk, N);   /* deep buffer: stays playing */
            }
            render2(cp);                                    /* bind + consume; everything still playing */
            for (int i = 0; i < 4; ++i) CHECK(rt_source_is_playing(cp, hs[i]), "steal-playing: victims live");
            uint32_t hn = rt_source_create(cp);             /* full pool: steals a PLAYING push source */
            CHECK(hn != 0, "steal-playing: create steals");
            render2(cp);                                    /* block 1 fades the victim, block 2 finalizes + EVT */
            rt_commit(cp);                                  /* ack: recycle + internal sound retires */
            render2(cp); rt_commit(cp);                     /* retire-ack: ring closes, slot frees */
            int live = 0; for (int i = 0; i < 4; ++i) live += rt_source_is_push(cp, hs[i]) ? 1 : 0;
            CHECK(live == 3, "steal-playing: exactly one victim recycled through the fade");
            uint32_t hp3 = rt_source_create_stream(cp, perr, sizeof perr);
            CHECK(hp3 != 0, "steal-playing: the faded steal retired the internal sound");
            rt_destroy(cp);
        }
    }

    /* a push-source death whose internal CMD_SOUND_RETIRE hits a FULL command ring must park the
     * retire and re-try at drain_events — not drop it (the handle is internal; nobody else can retry,
     * so a drop leaks the sound slot + stream ring for the engine's lifetime). The pad sweep walks the
     * ring fill across the boundary (RING_CAP = 4096 in rt.c; create_stream = 2 cmds, destroy = 1, the
     * retire is the +1 that lands on the full ring at one pad in the sweep). */
    {
        RtCore* cf = rt_create(4, 4, RATE, CH);
        CHECK(cf != NULL, "rt_create (parked retire)");
        if (cf) {
            char perr[256] = {0};
            for (int pad = 4090; pad <= 4098; ++pad) {
                uint32_t hp = rt_source_create_stream(cf, perr, sizeof perr);
                CHECK(hp != 0, "parked retire: create_stream");
                if (!hp) break;
                for (int i = 0; i < pad; ++i) rt_source_set_pos(cf, hp, 0.f, 0.f, 1.f);
                rt_source_destroy(cf, hp);                  /* one pad lands the retire on a full ring */
                render2(cf); rt_commit(cf);                 /* drain; a parked retire re-enqueues here */
                render2(cf); rt_commit(cf);                 /* retire-ack: ring closes, sound slot frees */
                if (rt_source_is_push(cf, hp)) {            /* ring was dead-full: the DESTROY itself was
                                                             * dropped (documented no-op) — retry it */
                    rt_source_destroy(cf, hp);
                    render2(cf); rt_commit(cf); render2(cf); rt_commit(cf);
                }
            }
            /* nothing leaked across the sweep: all 4 sound slots must be allocatable AT ONCE */
            uint32_t hk[4] = {0};
            for (int k = 0; k < 4; ++k) {
                hk[k] = rt_source_create_stream(cf, perr, sizeof perr);
                CHECK(hk[k] != 0, "parked retire: no sound/stream slot leaked across the sweep");
            }
            for (int k = 0; k < 4; ++k) if (hk[k]) rt_source_destroy(cf, hk[k]);
            render2(cf); rt_commit(cf); render2(cf); rt_commit(cf);
            rt_destroy(cf);
        }
    }

    /* pause/resume + seek: the gate ramps to silence, the playhead freezes, seeks land click-free */
    {
        RtCore* cq = rt_create(4, 4, RATE, CH);
        CHECK(cq != NULL, "rt_create (pause/seek)");
        if (cq && write_const_wav("bwa_rt_seek.wav", 0.8f, 5 * N)) {   /* finite, non-loop: 5 blocks of content */
            uint32_t sq = rt_load_sound(cq, "bwa_rt_seek.wav", err, sizeof err);
            uint32_t h  = rt_source_create(cq);
            rt_source_set_pos(cq, h, 1.f, 0.f, 1.f);
            bwa_timestamp ts = { 0, 0 };
            rt_source_play(cq, h, sq, false);
            rt_commit(cq);
            rt_render(cq, bus, N, &ts);                    /* block 1 of 5 plays */
            CHECK(total_l2() > 1e-3, "voice audible before pause");
            CHECK(rt_source_get_position(cq, h) == (uint64_t)N, "position tracks the playhead");
            rt_source_set_paused(cq, h, true);
            rt_render(cq, bus, N, &ts);                    /* ramp-out block (consumes block 2) */
            rt_render(cq, bus, N, &ts);
            CHECK(total_l2() < 1e-9, "paused voice is silent");
            CHECK(rt_source_is_playing(cq, h), "a paused voice still reads as playing");
            CHECK(rt_source_get_position(cq, h) == (uint64_t)2 * N, "position freezes where the pause ramp landed");
            for (int b = 0; b < 10; ++b) rt_render(cq, bus, N, &ts);   /* 10N frames >> the 3N remaining */
            CHECK(rt_source_get_position(cq, h) == (uint64_t)2 * N, "position stays frozen across paused blocks");
            rt_source_set_paused(cq, h, false);
            rt_render(cq, bus, N, &ts);                    /* ramp back in: block 3 of 5 */
            CHECK(total_l2() > 1e-3, "resume continues from the frozen position (nothing consumed while paused)");
            CHECK(rt_source_get_position(cq, h) == (uint64_t)3 * N, "position resumes from the frozen point");
            rt_source_seek(cq, h, (uint64_t)4 * N);        /* jump to the last block of content */
            rt_render(cq, bus, N, &ts);                    /* ramp-out */
            rt_render(cq, bus, N, &ts);                    /* seek lands: plays [4N, 5N) ramping in */
            CHECK(total_l2() > 1e-3, "seek lands and plays the target region");
            CHECK(rt_source_get_position(cq, h) == (uint64_t)5 * N, "position followed the seek through the target region");
            rt_render(cq, bus, N, &ts);                    /* past the end: the non-loop voice ends */
            CHECK(total_l2() < 1e-9, "silence after the seeked tail");
            CHECK(!rt_source_is_playing(cq, h), "seeking near the end ends the non-loop voice on time");
            CHECK(rt_source_get_position(cq, h) == (uint64_t)5 * N, "a finished voice keeps its final position");
            /* loop wrap + stale handle: 6N frames through a 5N looped sound lands one block past the seam */
            uint32_t h2 = rt_source_create(cq);
            rt_source_set_pos(cq, h2, 1.f, 0.f, 1.f);
            rt_source_play(cq, h2, sq, true);
            rt_commit(cq);
            for (int b = 0; b < 6; ++b) rt_render(cq, bus, N, &ts);
            CHECK(rt_source_get_position(cq, h2) == (uint64_t)N, "a looped position wraps at the sound length");
            rt_source_destroy(cq, h2);
            rt_render(cq, bus, N, &ts);
            CHECK(rt_source_get_position(cq, h2) == 0, "a destroyed handle's position reads 0");
            rt_destroy(cq);
            remove("bwa_rt_seek.wav");
        } else if (cq) { CHECK(0, "write seek wav"); rt_destroy(cq); }
    }

    /* loop REGION (rt_source_play_loop / the intro->loop pattern): playback starts at 0, plays the
     * intro once, then wraps at loop_end back to loop_beg — NOT to 0 and NOT at the clip end. A 5N
     * const sound, body region [N, 3N). */
    {
        RtCore* cr = rt_create(4, 4, RATE, CH);
        CHECK(cr != NULL, "rt_create (loop region)");
        if (cr && write_const_wav("bwa_rt_loopreg.wav", 0.7f, 5 * N)) {
            uint32_t sr = rt_load_sound(cr, "bwa_rt_loopreg.wav", err, sizeof err);
            uint32_t h  = rt_source_create(cr);
            rt_source_set_pos(cr, h, 1.f, 0.f, 1.f);
            bwa_timestamp ts = { 0, 0 };
            rt_source_play_loop(cr, h, sr, (uint64_t)N, (uint64_t)(3 * N));   /* body loops [N, 3N) */
            rt_commit(cr);
            rt_render(cr, bus, N, &ts);                                       /* intro: cursor 0 -> N */
            CHECK(rt_source_get_position(cr, h) == (uint64_t)N, "region: intro plays from frame 0");
            rt_render(cr, bus, N, &ts);                                       /* N -> 2N */
            rt_render(cr, bus, N, &ts);                                       /* 2N -> 3N (reaches loop_end) */
            CHECK(rt_source_get_position(cr, h) == (uint64_t)(3 * N), "region: cursor reaches loop_end");
            rt_render(cr, bus, N, &ts);                                       /* wraps to loop_beg (N), then N -> 2N */
            CHECK(rt_source_get_position(cr, h) == (uint64_t)(2 * N),
                  "region: wraps to loop_beg (=> 2N), not to 0 (would read N) nor to the clip end");
            for (int b = 0; b < 20; ++b) rt_render(cr, bus, N, &ts);         /* many wraps: stays confined */
            uint64_t p = rt_source_get_position(cr, h);
            CHECK(p >= (uint64_t)N && p <= (uint64_t)(3 * N), "region: cursor never leaves [loop_beg, loop_end]");
            CHECK(rt_source_is_playing(cr, h), "region: a looped region never ends");
            rt_destroy(cr);
            remove("bwa_rt_loopreg.wav");
        } else if (cr) { CHECK(0, "write loop-region wav"); rt_destroy(cr); }
    }

    /* scheduled STOP (rt_source_stop_at): when the dsp clock reaches stop_at the voice takes the
     * click-free one-block fade (the same path as rt_source_stop) and ends — audible through the
     * fade block, silent after, is_playing flips off on schedule. A looping const sound so only the
     * schedule stops it; the dsp clock advances via the block-start timestamp (as the schedule test). */
    {
        RtCore* cs = rt_create(4, 4, RATE, CH);
        CHECK(cs != NULL, "rt_create (stop_at)");
        if (cs && write_const_wav("bwa_rt_stopat.wav", 0.8f, 64 * N)) {
            uint32_t ss = rt_load_sound(cs, "bwa_rt_stopat.wav", err, sizeof err);
            uint32_t h  = rt_source_create(cs);
            rt_source_set_pos(cs, h, 1.f, 0.f, 1.f);
            rt_source_play(cs, h, ss, true);                                  /* loop: only the schedule stops it */
            rt_commit(cs);
            uint64_t pos = 0;
            bwa_timestamp t0 = { pos, 0 }; rt_render(cs, bus, N, &t0); pos += N;   /* [0, N) */
            CHECK(total_l2() > 1e-3 && rt_source_is_playing(cs, h), "stop_at: audible before the schedule");
            rt_source_stop_at(cs, h, (uint64_t)(3 * N));                      /* stop when the clock reaches 3N */
            bwa_timestamp t1 = { pos, 0 }; rt_render(cs, bus, N, &t1); pos += N;   /* [N, 2N): armed, not yet reached */
            CHECK(rt_source_is_playing(cs, h), "stop_at: still playing before stop_at");
            bwa_timestamp t2 = { pos, 0 }; rt_render(cs, bus, N, &t2); pos += N;   /* [2N, 3N): the fade block */
            CHECK(total_l2() > 1e-3, "stop_at: the stop is a one-block fade, not a hard cut (fade block audible)");
            CHECK(rt_source_is_playing(cs, h), "stop_at: still playing during the fade block");
            bwa_timestamp t3 = { pos, 0 }; rt_render(cs, bus, N, &t3); pos += N;   /* [3N, 4N): finalized */
            CHECK(total_l2() < 1e-9, "stop_at: silent after the fade lands");
            CHECK(!rt_source_is_playing(cs, h), "stop_at: the voice ended on schedule");
            /* a fresh play clears a stale schedule: schedule far out, replay, run past it, still playing */
            uint32_t h2 = rt_source_create(cs);
            rt_source_set_pos(cs, h2, 1.f, 0.f, 1.f);
            rt_source_play(cs, h2, ss, true); rt_commit(cs);
            rt_source_stop_at(cs, h2, (uint64_t)(pos + 2 * N));
            rt_source_play(cs, h2, ss, true);                                 /* replay cancels the pending stop */
            rt_commit(cs);
            for (int b = 0; b < 6; ++b) { bwa_timestamp t = { pos, 0 }; rt_render(cs, bus, N, &t); pos += N; }
            CHECK(rt_source_is_playing(cs, h2), "stop_at: a fresh play cancels a pending scheduled stop");
            rt_destroy(cs);
            remove("bwa_rt_stopat.wav");
        } else if (cs) { CHECK(0, "write stop_at wav"); rt_destroy(cs); }
    }

    /* gapless CHAINING (rt_source_queue): a queued sound plays the instant the current one ends, with
     * no silence at the seam. A = one block, non-loop; B = looping terminal. Chaining keeps the voice
     * alive past A's end (a non-queued A ends and goes silent — the negative control h2). */
    {
        RtCore* cc = rt_create(4, 4, RATE, CH);
        CHECK(cc != NULL, "rt_create (chain)");
        if (cc && write_const_wav("bwa_rt_chainA.wav", 0.5f, N)          /* A: exactly one block */
               && write_const_wav("bwa_rt_chainB.wav", 0.5f, 4 * N)) {   /* B: the looping body */
            uint32_t sa = rt_load_sound(cc, "bwa_rt_chainA.wav", err, sizeof err);
            uint32_t sb = rt_load_sound(cc, "bwa_rt_chainB.wav", err, sizeof err);
            uint32_t h  = rt_source_create(cc);
            uint32_t h2 = rt_source_create(cc);                          /* negative control: A alone */
            rt_source_set_pos(cc, h,  1.f, 0.f, 1.f);
            rt_source_set_pos(cc, h2, 1.f, 0.f, 1.f);
            bwa_timestamp ts = { 0, 0 };
            rt_source_play (cc, h,  sa, false);
            rt_source_queue(cc, h,  sb, true);                           /* chain: A -> looping B */
            rt_source_play (cc, h2, sa, false);                          /* no queue */
            rt_commit(cc);
            rt_render(cc, bus, N, &ts);                                  /* block 1: A plays on both */
            CHECK(rt_source_is_playing(cc, h) && rt_source_is_playing(cc, h2), "chain: A playing (both)");
            rt_render(cc, bus, N, &ts);                                  /* block 2: h chains to B; h2's A ends */
            CHECK(rt_source_is_playing(cc, h),  "chain: the queued voice keeps playing past A's end");
            CHECK(!rt_source_is_playing(cc, h2), "chain: the un-queued voice ends at A's end (control)");
            CHECK(rt_source_get_position(cc, h) == (uint64_t)N, "chain: the playhead restarts on the chained sound");
            for (int b = 0; b < 8; ++b) rt_render(cc, bus, N, &ts);     /* B is the looping terminal */
            CHECK(rt_source_is_playing(cc, h), "chain: the looping terminal item plays on");
            rt_destroy(cc);
            remove("bwa_rt_chainA.wav"); remove("bwa_rt_chainB.wav");
        } else if (cc) { CHECK(0, "write chain wavs"); rt_destroy(cc); }
    }

    /* chain seam is gapless WITHIN a block: A spans 1.5 blocks, so A ends mid-block-2 and B fills the
     * rest — block 2 must stay a full block (not half). And clear_queue drops the pending chain. */
    {
        RtCore* cg = rt_create(4, 4, RATE, CH);
        CHECK(cg != NULL, "rt_create (chain seam)");
        if (cg && write_const_wav("bwa_rt_seamA.wav", 0.5f, N + N / 2)   /* A: 1.5 blocks */
               && write_const_wav("bwa_rt_seamB.wav", 0.5f, 4 * N)) {
            uint32_t sa = rt_load_sound(cg, "bwa_rt_seamA.wav", err, sizeof err);
            uint32_t sb = rt_load_sound(cg, "bwa_rt_seamB.wav", err, sizeof err);
            uint32_t h  = rt_source_create(cg);
            rt_source_set_pos(cg, h, 1.f, 0.f, 1.f);
            bwa_timestamp ts = { 0, 0 };
            rt_source_play (cg, h, sa, false);
            rt_source_queue(cg, h, sb, true);
            rt_commit(cg);
            rt_render(cg, bus, N, &ts);                                  /* block 1: A (fade-in) */
            rt_render(cg, bus, N, &ts); double e2 = total_l2();          /* block 2: A ends at N/2, B fills the rest */
            rt_render(cg, bus, N, &ts); double e3 = total_l2();          /* block 3: pure B, a full reference block */
            CHECK(e3 > 1e-3 && e2 > 0.7 * e3,                            /* e3 real (B chained in) AND block 2 full */
                  "chain: no gap at a mid-block seam (the spanning block stays full)");
            /* clear_queue drops the pending chain: a fresh A with the queue cleared ends alone */
            uint32_t h3 = rt_source_create(cg);
            rt_source_set_pos(cg, h3, 1.f, 0.f, 1.f);
            rt_source_play (cg, h3, sa, false);
            rt_source_queue(cg, h3, sb, true);
            rt_source_clear_queue(cg, h3);
            rt_commit(cg);
            for (int b = 0; b < 3; ++b) rt_render(cg, bus, N, &ts);      /* A (1.5 blocks) ends, nothing chained */
            CHECK(!rt_source_is_playing(cg, h3), "chain: clear_queue drops the pending chain (A ends alone)");
            rt_destroy(cg);
            remove("bwa_rt_seamA.wav"); remove("bwa_rt_seamB.wav");
        } else if (cg) { CHECK(0, "write chain-seam wavs"); rt_destroy(cg); }
    }

    /* a stopping voice must NOT chain: a scheduled stop landing in the block where the current sound
     * ends has to end the voice, not start the queued one (the seam's !stopping guard). */
    {
        RtCore* cx = rt_create(4, 4, RATE, CH);
        CHECK(cx != NULL, "rt_create (chain vs stop)");
        if (cx && write_const_wav("bwa_rt_csA.wav", 0.5f, N)            /* A: one block */
               && write_const_wav("bwa_rt_csB.wav", 0.5f, 4 * N)) {     /* B: would-be looping chain */
            uint32_t sa = rt_load_sound(cx, "bwa_rt_csA.wav", err, sizeof err);
            uint32_t sb = rt_load_sound(cx, "bwa_rt_csB.wav", err, sizeof err);
            uint32_t h  = rt_source_create(cx);
            rt_source_set_pos(cx, h, 1.f, 0.f, 1.f);
            rt_source_play (cx, h, sa, false);
            rt_source_queue(cx, h, sb, true);
            rt_source_stop_at(cx, h, (uint64_t)(2 * N));                /* fires in block 2, where A ends */
            rt_commit(cx);
            uint64_t pos = 0;
            bwa_timestamp t0 = { pos, 0 }; rt_render(cx, bus, N, &t0); pos += N;   /* block 1: A */
            CHECK(rt_source_is_playing(cx, h), "chain-vs-stop: playing during A");
            bwa_timestamp t1 = { pos, 0 }; rt_render(cx, bus, N, &t1); pos += N;   /* block 2: A ends AND stop fires */
            CHECK(!rt_source_is_playing(cx, h), "chain-vs-stop: a scheduled stop ends the voice, it does NOT chain into B");
            rt_destroy(cx);
            remove("bwa_rt_csA.wav"); remove("bwa_rt_csB.wav");
        } else if (cx) { CHECK(0, "write chain-vs-stop wavs"); rt_destroy(cx); }
    }

    /* output protection limiter: a linked gain caps the peak without shifting inter-channel balance.
     * The test signal is injected after align, so it hits the limiter (the final stage) directly. */
    {
        RtCore* cl = rt_create(4, 4, RATE, CH);
        CHECK(cl != NULL, "rt_create (limiter)");
        if (cl) {
            bwa_timestamp ts = { 0, 0 };
            rt_test_signal(cl, 0, 1, 2.0f);                /* sine, peak 2.0 — over the -1 dBFS ceiling */
            rt_test_signal(cl, 1, 1, 0.5f);                /* the same waveform at 1/4 the level */
            for (int b = 0; b < 20; ++b) rt_render(cl, bus, N, &ts);   /* settle the envelope */
            rt_render(cl, bus, N, &ts);
            float p0 = 0, p1 = 0;
            for (int i = 0; i < N; ++i) {
                float a0 = fabsf(bus[0 * N + i]), a1 = fabsf(bus[1 * N + i]);
                if (a0 > p0) p0 = a0;
                if (a1 > p1) p1 = a1;
            }
            CHECK(p0 <= 0.8915f && p0 > 0.80f, "limiter holds the hot channel at the -1 dBFS ceiling");
            CHECK(fabsf(p1 / p0 - 0.25f) < 0.02f, "linked limiting preserves inter-channel balance");
            /* the output meter publishes exactly this block's post-limiter per-channel peaks */
            float mtr[CH] = { 0 };
            CHECK(rt_bus_peaks(cl, mtr, CH) == CH, "bus meter reports the channel count");
            CHECK(fabsf(mtr[0] - p0) < 1e-6f && fabsf(mtr[1] - p1) < 1e-6f,
                  "bus meter matches the rendered block's peaks");
            CHECK(mtr[2] == 0.f, "a silent channel meters zero");
            rt_set_limiter_ceiling(cl, 0.5f);
            for (int b = 0; b < 20; ++b) rt_render(cl, bus, N, &ts);
            rt_render(cl, bus, N, &ts);
            p0 = 0; for (int i = 0; i < N; ++i) { float a = fabsf(bus[0 * N + i]); if (a > p0) p0 = a; }
            CHECK(p0 <= 0.5001f, "limiter ceiling is adjustable");
            rt_set_limiter(cl, 0);
            rt_render(cl, bus, N, &ts); rt_render(cl, bus, N, &ts);
            p0 = 0; for (int i = 0; i < N; ++i) { float a = fabsf(bus[0 * N + i]); if (a > p0) p0 = a; }
            CHECK(p0 > 1.5f, "limiter off passes the raw signal");
            rt_destroy(cl);
        }
    }

    remove(WAV);
    if (fails) { printf("rt_core_test: %d FAILURES\n", fails); return 1; }
    printf("rt_core_test OK (rt_create bound, DBAP+commit+gen-drop, gain, occlusion/EQ/directivity, "
           "channel-test, master+groups+fades+global-pause, runtime-channel-count, voice-steal+priority, "
           "scheduled-play, clock-pair, streaming, push-sources+steal+parked-retire, pause/seek, "
           "loop-region, scheduled-stop, gapless-chaining, position-readback, limiter, bus-meter verified)\n");
    return 0;
}
