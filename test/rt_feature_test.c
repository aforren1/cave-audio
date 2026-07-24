/*
 * rt_feature_test.c — the spatial-feature DSP half of the old rt_test monolith, driven off the RT
 * path (single-threaded, deterministic). Where rt_core_test.c pins rt.c's machinery (rings, commit,
 * generation handles, voice lifecycle, scheduling, streaming, limiter, readbacks), this pins the
 * DSP behaviour toggles layered on top: the ambisonic bed + bed renderers (rotation/orientation/
 * parametric/max-rE + band split), the panner variants (dual-band) and reflection/pathing taps,
 * source spread (lobe/MDAP/spectral) + extent + frame transport + near-spread + metric size,
 * decorrelation, pathing transmission EQ, air absorption + Doppler + loudness comp +
 * distance→reverb send + attenuation override, tracked room EQ, pose prediction, multi-listener
 * compromise panning, image-source early reflections, and pitch.
 *
 * Routing goes through real DBAP, so the observable is "a source at speaker k's surveyed position
 * makes channel k dominate"; the shared harness (layout, probes, wav writers, taps, CHECK) is
 * rt_test_util.h. Uses its own scratch wav name so it never collides with rt_core_test's.
 */
#include "rt_test_util.h"

int main(void) {
    LD = layout_default();                          /* listener stays at the default (the array centre, LD.ref) */
    const char* WAV = "bwa_rt_fconst.wav";          /* distinct from rt_core_test's scratch wav */
    if (!write_const_wav(WAV, 1.0f, 8 * N)) { printf("FAIL: write wav\n"); return 1; }
    char err[256] = {0};

    /* 12. ambisonic bed: a W-only field decodes equally to all speakers; a front-encoded 1st-order
     *     field favors the front speaker over the back one. Room convention (post +z-forward flip):
     *     the listener faces +z, so AmbiX-front (ACN3 X) must decode to the room +z speaker. */
    {
        const char* AMB_OMNI = "bwa_amb_omni.wav", *AMB_FRONT = "bwa_amb_front.wav";
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
                        if (LD.speakers[s].pos[2] >  1.0f) s_front = s;   /* room +z: the listener faces +z */
                        if (LD.speakers[s].pos[2] < -1.0f) s_back  = s;
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
    }

    /* 13. bus tap + reflection aux send: the tap is called once per block with the summed mono send
     *     of opted-in voices, and it can sum onto the bus; opting out removes the voice. */
    {
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
    }

    /* 14. pathing render: an opted-in voice SH-encodes its (un-occluded) signal s*shCoeffs into the
     *     shared ambisonic accumulator, which the path tap receives. The const-1.0 source means the
     *     landed accumulator equals the published shCoeffs exactly; opting out zeroes it. */
    {
        RtCore* cp = rt_create(8, 4, RATE, CH);
        CHECK(cp != NULL, "rt_create (path)");
        if (cp) {
            uint32_t sp = rt_load_sound(cp, WAV, err, sizeof err);
            uint32_t vp = rt_source_create(cp);
            rt_source_play(cp, vp, sp, true);
            rt_source_set_pos(cp, vp, LD.speakers[2].pos[0], LD.speakers[2].pos[1], LD.speakers[2].pos[2]);
            rt_set_path_tap(cp, test_path_tap, NULL, 4);
            rt_source_set_pathing(cp, vp, true);
            const float want[4] = { 0.5f, 0.25f, -0.1f, 0.3f };
            rt_set_pathing(cp, vp, want, NULL, 4);           /* publish a fixed indirect field (flat EQ) for this voice */
            g_path_calls = 0; g_path_chn = 0;
            rt_commit(cp); render2(cp);                      /* block 1 ramps 0->want, block 2 holds at want */
            CHECK(g_path_calls == 2 && g_path_chn == 4, "path tap called once per block with the ambi channel count");
            int matched = 1;
            for (int k = 0; k < 4; ++k) if (fabs((double)g_path_cap[k] - want[k]) > 1e-3) matched = 0;
            CHECK(matched, "accumulator lands on s*shCoeffs (s=1) — the indirect field is encoded from the published directions");
            /* bending-loss EQ: a non-flat band tilt colours the indirect signal BEFORE the SH-encode. The
             * source is DC (s=1) and the RBJ low-shelf DC gain is exactly its band gain, so {0.5,1,1} scales
             * every SH channel by 0.5 once the band-gain slew + biquads settle -> accumulator = 0.5*shCoeffs. */
            const float eqtilt[3] = { 0.5f, 1.0f, 1.0f };
            rt_set_pathing(cp, vp, want, eqtilt, 4);
            for (int b = 0; b < 16; ++b) render2(cp);        /* settle the EQ_SLEW glide + biquad transient */
            int tilted = 1;
            for (int k = 0; k < 4; ++k) if (fabs((double)g_path_cap[k] - 0.5 * want[k]) > 5e-3) tilted = 0;
            CHECK(tilted, "bending-loss EQ tilts the indirect field pre-encode (low-shelf DC gain applied to s_raw)");
            rt_set_pathing(cp, vp, want, NULL, 4);           /* back to flat for the opt-out check */
            for (int b = 0; b < 16; ++b) render2(cp);        /* let the EQ settle back to bypass */
            rt_source_set_pathing(cp, vp, false);            /* opt out -> the accumulator is silent */
            for (int k = 0; k < 4; ++k) g_path_cap[k] = 9.f;
            rt_commit(cp); render2(cp);
            int silent = 1; for (int k = 0; k < 4; ++k) if (fabs((double)g_path_cap[k]) > 1e-6) silent = 0;
            CHECK(silent, "opting out removes the voice from the pathing render");
            rt_destroy(cp);
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

            /* MDAP mode: the same widening contract from a different construction (a ring of virtual
             * panner solves). Live A/B atomic like the panner: switch, re-dirty via commit, render. */
            rt_set_spread_mode(cs, 1);
            rt_source_set_pos(cs, hsp, 0.f, 0.f, 0.f); rt_commit(cs); render2(cs);   /* nudge -> re-solve */
            set_pos_spk(cs, hsp, 7); rt_commit(cs); render2(cs);
            int    act_mdap = active_channels(0.03);
            double l2_mdap  = total_l2();
            double share_md = chan_energy(argmax_channel()) / total_energy();
            CHECK(act_mdap > act_point + 2, "MDAP spread widens the source across more speakers");
            CHECK(share_md < share_pt, "MDAP spread lowers the dominant channel's share");
            CHECK(l2_point > 0 && fabs(l2_mdap - l2_point) / l2_point < 0.02, "MDAP spread preserves total power");
            /* spread 0 under MDAP mode = the plain point solve (the ring collapses onto the source) */
            rt_source_set_spread(cs, hsp, 0.0f); rt_commit(cs); render2(cs);
            int    act_md0 = active_channels(0.03);
            double l2_md0  = total_l2();
            CHECK(act_md0 == act_point && fabs(l2_md0 - l2_point) / l2_point < 0.02,
                  "MDAP at spread 0 is the point solve");
            rt_set_spread_mode(cs, 0);
            rt_source_destroy(cs, hsp); rt_commit(cs);
            rt_destroy(cs);
        }
    }

    /* spectral spread (BWA_SPREAD_SPECTRAL = mode 2): the widening contract again (more speakers,
     * lower dominant share, ~constant power — power to within the crossover overlap), PLUS the
     * mode's signature no other spread mode has: a LOW tone and a HIGH tone leave the same spread
     * source from DIFFERENT directions (frequency-dependent panning), while at spread 0 they pan
     * identically. Finally the retire handoff: leaving mode 2 lands back on the lobe rendering. */
    {
        RtCore* cf = rt_create(8, 4, RATE, CH);
        CHECK(cf != NULL, "rt_create (spectral spread)");
        if (cf) {
            const char* NW2 = "bwa_rt_fsnoise.wav";
            bwa_timestamp ts0 = { 0, 0 };
            /* 8 blocks = exactly one noise-loop pass, so the summed power is loop-phase-independent */
            #define L2_LOOP(C, OUT) do { double acc_ = 0; \
                for (int b_ = 0; b_ < 8; ++b_) { rt_render((C), bus, N, &ts0); acc_ += total_l2(); } \
                (OUT) = acc_; } while (0)
            if (write_noise_wav(NW2, 8 * N)) {
                uint32_t nf = rt_load_sound(cf, NW2, err, sizeof err);
                uint32_t hf = rt_source_create(cf);
                rt_source_play(cf, hf, nf, true);
                set_pos_spk(cf, hf, 7);
                rt_commit(cf); render2(cf);
                int    act_pt = active_channels(0.03);
                double share_pt = chan_energy(argmax_channel()) / total_energy();
                double l2_pt; L2_LOOP(cf, l2_pt);
                rt_set_spread_mode(cf, 2);
                rt_source_set_spread(cf, hf, 1.0f); rt_commit(cf);   /* re-dirties: the mode lands */
                for (int b = 0; b < 4; ++b) render2(cf);             /* engage + splitter settle */
                int    act_fs = active_channels(0.03);
                double share_fs = chan_energy(argmax_channel()) / total_energy();
                double l2_fs; L2_LOOP(cf, l2_fs);
                CHECK(act_fs > act_pt + 2, "spectral spread widens the source across more speakers");
                CHECK(share_fs < share_pt, "spectral spread lowers the dominant channel's share");
                printf("spectral: power delta %.2f dB\n", 20.0 * log10(l2_fs / l2_pt));
                CHECK(l2_pt > 0 && fabs(20.0 * log10(l2_fs / l2_pt)) < 0.5,
                      "spectral spread is constant-power (overlap-compensated)");
                /* retire handoff: back to LOBE — the rendering matches lobe's own, not a stuck band path */
                rt_set_spread_mode(cf, 0);
                rt_source_set_pos(cf, hf, 0.f, 0.f, 0.f); rt_commit(cf); render2(cf);   /* nudge -> re-solve */
                set_pos_spk(cf, hf, 7); rt_commit(cf);
                for (int b = 0; b < 4; ++b) render2(cf);
                int    act_lb = active_channels(0.03);
                double l2_lb; L2_LOOP(cf, l2_lb);
                CHECK(act_lb > act_pt + 2 && fabs(20.0 * log10(l2_lb / l2_pt)) < 0.5,
                      "leaving spectral mode hands back to the lobe rendering (constant power)");
                rt_source_destroy(cf, hf); rt_commit(cf);
            } else CHECK(0, "write noise wav (spectral)");
            #undef L2_LOOP
            /* the signature: frequency splits direction. 200 Hz sits in the LF band (kept on the
             * source direction), 6 kHz in a scattered band — their dominant speakers must differ at
             * spread 1 and agree at spread 0. Integer cycles per file: both loop seamlessly. */
            const char* SL = "bwa_rt_fs_lo.wav", *SH2 = "bwa_rt_fs_hi.wav";
            if (write_sine_wav(SL, 200.0, 4800) && write_sine_wav(SH2, 6000.0, 4800)) {
                uint32_t slo = rt_load_sound(cf, SL, err, sizeof err);
                uint32_t shi = rt_load_sound(cf, SH2, err, sizeof err);
                uint32_t hv  = rt_source_create(cf);
                rt_set_spread_mode(cf, 2);
                int a_lo0, a_hi0, a_lo1, a_hi1;
                rt_source_play(cf, hv, slo, true); set_pos_spk(cf, hv, 7);
                rt_commit(cf); for (int b = 0; b < 4; ++b) render2(cf);
                a_lo0 = argmax_channel();                            /* spread 0: the point solve */
                rt_source_play(cf, hv, shi, true);
                rt_commit(cf); for (int b = 0; b < 4; ++b) render2(cf);
                a_hi0 = argmax_channel();
                CHECK(a_lo0 == a_hi0, "spread 0: low and high tones pan identically (mode 2 inactive)");
                rt_source_set_spread(cf, hv, 1.0f);
                rt_commit(cf); for (int b = 0; b < 6; ++b) render2(cf);
                a_hi1 = argmax_channel();
                rt_source_play(cf, hv, slo, true);
                rt_commit(cf); for (int b = 0; b < 6; ++b) render2(cf);
                a_lo1 = argmax_channel();
                CHECK(a_lo1 == a_lo0, "spread 1: the LF band stays on the source direction");
                CHECK(a_hi1 != a_lo1, "spread 1: the HF band leaves from its own direction (frequency-dependent panning)");
                rt_set_spread_mode(cf, 0);
                rt_source_destroy(cf, hv); rt_commit(cf);
                remove(SL); remove(SH2);
            } else CHECK(0, "write tone wavs (spectral)");
            rt_destroy(cf);
            remove(NW2);
        }
    }

    /* spread-frame continuity over the pole: the ring/band frame (u, w) around the source direction
     * is PARALLEL-TRANSPORTED per voice (spread_frame in rt.c) rather than derived from a fixed
     * up-vector. The fixed-up construction flips the frame ~180° in ONE solve when a moving source
     * leaves the |d.y| > 0.9 zone — under spectral spread every scattered band's direction teleports
     * and the speaker distribution steps (Pulkki's reference vbap external transports the same state
     * for the same reason). Sweep a wide spectral noise source along a 2°-step arc over the zenith
     * and pin the worst step-to-step change of the normalized per-channel energy distribution to the
     * same order as the geometric drift of a 2° move. */
    {
        RtCore* cw = rt_create(8, 4, RATE, CH);
        CHECK(cw != NULL, "rt_create (spread frame)");
        if (cw) {
            const char* NW3 = "bwa_rt_pole.wav";
            if (write_noise_wav(NW3, 8 * N)) {
                uint32_t nf = rt_load_sound(cw, NW3, err, sizeof err);
                uint32_t hf = rt_source_create(cw);
                rt_source_play(cw, hf, nf, true);
                rt_set_spread_mode(cw, 2);
                rt_source_set_spread(cw, hf, 0.8f);
                bwa_timestamp tsw = { 0, 0 };
                double prev[CH], worst = 0.0;
                int first = 1;
                for (int s = -20; s <= 20; ++s) {            /* -40°..+40° across the zenith */
                    float th = (float)s * 0.0349066f;        /* 2° per step */
                    rt_source_set_pos(cw, hf, LD.ref[0] + 1.5f * sinf(th),
                                              LD.ref[1] + 1.5f * cosf(th), LD.ref[2]);
                    rt_commit(cw);
                    for (int b = 0; b < 6; ++b) rt_render(cw, bus, N, &tsw);   /* ramps + splitter land */
                    double e[CH], tot = 0.0;
                    for (int k = 0; k < CH; ++k) e[k] = 0.0;
                    for (int b = 0; b < 8; ++b) {            /* one full noise loop: phase-independent */
                        rt_render(cw, bus, N, &tsw);
                        for (int k = 0; k < CH; ++k) e[k] += chan_energy(k);
                    }
                    for (int k = 0; k < CH; ++k) tot += e[k];
                    for (int k = 0; k < CH; ++k) e[k] /= tot;
                    if (!first) {
                        double dmax = 0.0;
                        for (int k = 0; k < CH; ++k) { double d = fabs(e[k] - prev[k]); if (d > dmax) dmax = d; }
                        if (dmax > worst) worst = dmax;
                    }
                    memcpy(prev, e, sizeof prev);
                    first = 0;
                }
                printf("spread frame: worst pole-crossing step %.4f\n", worst);
                CHECK(worst < 0.02, "the spread frame is continuous over the pole (no ring snap)");
                rt_set_spread_mode(cw, 0);
                rt_source_destroy(cw, hf); rt_commit(cw);
                remove(NW3);
            } else CHECK(0, "write noise wav (spread frame)");
            rt_destroy(cw);
        }
    }

    /* tracked room EQ (room_eq_grid): the align biquads re-aim as the committed listener moves — a
     * 100 Hz voice equidistant from two grid positions (flat at A, -12 dB at B on EVERY channel)
     * drops by ~the IDW-interpolated depth when the listener walks A -> B, and the live kill switch
     * glides it back to flat. Total L2 isolates the EQ: the move also redistributes the panning, but
     * constant power keeps ||bus|| fixed, and the same cut on every channel scales it uniformly. */
    {
        RtCore* cg = rt_create(8, 4, RATE, CH);
        CHECK(cg != NULL, "rt_create (tracked room eq)");
        if (cg) {
            Layout G = layout_default();
            G.rq_grid.npos = 2;
            G.rq_grid.pos[0][0] = -0.5f; G.rq_grid.pos[0][1] = 1.5f; G.rq_grid.pos[0][2] = 0.f;
            G.rq_grid.pos[1][0] =  0.5f; G.rq_grid.pos[1][1] = 1.5f; G.rq_grid.pos[1][2] = 0.f;
            for (int k = 0; k < CH; ++k) {
                G.rq_grid.nsec[k]  = 1;
                G.rq_grid.fc[k][0] = 100.f; G.rq_grid.q[k][0] = 2.f;
                G.rq_grid.gain_db[0][k][0] = 0.f;
                G.rq_grid.gain_db[1][k][0] = -12.f;
            }
            rt_set_layout(cg, &G);
            const char* SW = "bwa_rt_sine100.wav";
            if (write_sine_wav(SW, 100.0, 4800)) {               /* exactly 10 cycles: seamless loop */
                uint32_t sg = rt_load_sound(cg, SW, err, sizeof err);
                uint32_t hg = rt_source_create(cg);
                rt_source_play(cg, hg, sg, true);
                rt_source_set_pos(cg, hg, 0.f, 1.5f, 0.f);       /* equidistant from A and B (same atten) */
                const float qid[4] = { 0, 0, 0, 1 };
                const float pa[3] = { -0.5f, 1.5f, 0.f }, pb[3] = { 0.5f, 1.5f, 0.f };
                rt_set_listener(cg, pa, qid); rt_commit(cg);
                double l2_a = 0, l2_b = 0, l2_off = 0;
                for (int b = 0; b < 100; ++b) render2(cg);       /* settle at A */
                for (int b = 0; b <   8; ++b) { render2(cg); l2_a += total_l2(); }
                rt_set_listener(cg, pb, qid); rt_commit(cg);
                for (int b = 0; b < 200; ++b) render2(cg);       /* walk + settle (12 dB at 24 dB/s = 0.5 s) */
                for (int b = 0; b <   8; ++b) { render2(cg); l2_b += total_l2(); }
                double drop_db = 20.0 * log10(l2_a / l2_b);      /* IDW at B: ~ -11.7 dB (A still pulls a little) */
                CHECK(drop_db > 9.0 && drop_db < 14.0, "tracked room EQ follows the listener (A flat -> B cut)");
                rt_set_room_eq_dyn(cg, 0);                       /* kill switch: glide every section to flat */
                for (int b = 0; b < 200; ++b) render2(cg);
                for (int b = 0; b <   8; ++b) { render2(cg); l2_off += total_l2(); }
                CHECK(fabs(20.0 * log10(l2_a / l2_off)) < 1.5, "tracked room EQ off glides back to flat");
                rt_source_destroy(cg, hg); rt_commit(cg);
            } else CHECK(0, "write 100 Hz sine");
            rt_destroy(cg);
            remove("bwa_rt_sine100.wav");
        }
    }

    /* decorrelation (bwa_set_decorrelation): a fully-spread noise source's speaker feeds are IDENTICAL
     * scaled copies with decor off (zero-lag correlation ~ +1) and mutually incoherent with it on
     * (each channel passes its own velvet filter), at the same total power; toggling back restores
     * coherence (the split amplitude ramps out and the velvet tail flushes). */
    {
        RtCore* cd = rt_create(8, 4, RATE, CH);
        CHECK(cd != NULL, "rt_create (decorrelation)");
        if (cd) {
            const char* NW = "bwa_rt_noise.wav";
            if (write_noise_wav(NW, 8 * N)) {
                uint32_t nd = rt_load_sound(cd, NW, err, sizeof err);
                uint32_t hd = rt_source_create(cd);
                rt_source_play(cd, hd, nd, true);
                rt_source_set_pos(cd, hd, 0.7f, 1.5f, 0.4f);
                rt_source_set_spread(cd, hd, 1.0f);              /* wide: many active channels */
                rt_commit(cd);
                for (int b = 0; b < 8; ++b) render2(cd);
                /* pick the two strongest channels while coherent (stable across the toggle) */
                int ca = argmax_channel(); double ea = chan_energy(ca);
                int cb2 = -1; double eb = -1;
                for (int ch = 0; ch < CH; ++ch) if (ch != ca && chan_energy(ch) > eb) { eb = chan_energy(ch); cb2 = ch; }
                double l2_coh = total_l2();
                #define XCORR(A, B, OUT) do {                                                   \
                    double sab_ = 0, saa_ = 0, sbb_ = 0;                                        \
                    for (int i_ = 0; i_ < (int)N; ++i_) {                                       \
                        double xa_ = bus[(size_t)(A)*N + i_], xb_ = bus[(size_t)(B)*N + i_];    \
                        sab_ += xa_*xb_; saa_ += xa_*xa_; sbb_ += xb_*xb_;                      \
                    }                                                                           \
                    (OUT) = (saa_ > 0 && sbb_ > 0) ? sab_ / sqrt(saa_*sbb_) : 0.0;              \
                } while (0)
                double r_off; XCORR(ca, cb2, r_off);
                CHECK(r_off > 0.95, "decor off: spread feeds are coherent copies (corr ~ +1)");
                rt_set_decorrelation(cd, 1);
                for (int b = 0; b < 20; ++b) render2(cd);        /* ramp in + fill the velvet history */
                double r_on; XCORR(ca, cb2, r_on);
                double l2_dc = total_l2();
                CHECK(fabs(r_on) < 0.4, "decor on: the same feeds are mutually incoherent");
                CHECK(l2_coh > 0 && fabs(20.0 * log10(l2_dc / l2_coh)) < 1.5,
                      "decorrelation preserves total power (~unit-energy filters)");
                rt_set_decorrelation(cd, 0);
                for (int b = 0; b < 20; ++b) render2(cd);        /* ramp out + flush the tail */
                double r_back; XCORR(ca, cb2, r_back);
                CHECK(r_back > 0.95, "decor off again: coherence restored (click-free A/B round trip)");
                #undef XCORR
                rt_source_destroy(cd, hd); rt_commit(cd);
            } else CHECK(0, "write noise wav");
            rt_destroy(cd);
            remove("bwa_rt_noise.wav");
        }
    }

    /* parametric bed renderer (bwa_set_bed_renderer): a noise PLANE-WAVE bed (W=X: from room +z)
     * localizes SHARPER than the matrix decode and stays loudness-matched; a W-only bed (zero
     * intensity -> fully diffuse) spreads across many channels through the decorrelators at matched
     * power. The switch crossfades live. */
    {
        RtCore* cb = rt_create(8, 4, RATE, CH);
        CHECK(cb != NULL, "rt_create (parametric bed)");
        if (cb) {
            const char* PW = "bwa_rt_bed_pw.wav", *DW = "bwa_rt_bed_w.wav";
            if (write_ambix4_noise_wav(PW, 1.f, 0.f, 0.f, 1.f, 8 * N) &&
                write_ambix4_noise_wav(DW, 1.f, 0.f, 0.f, 0.f, 8 * N)) {
                uint32_t sp = rt_load_ambix(cb, PW, err, sizeof err);
                CHECK(sp != 0, err[0] ? err : "load plane-wave bed");
                uint32_t hp = rt_source_create(cb);
                rt_source_play(cb, hp, sp, true);
                rt_commit(cb);
                for (int b = 0; b < 8; ++b) render2(cb);
                double l2_m    = total_l2();
                double share_m = chan_energy(argmax_channel()) / total_energy();
                rt_set_bed_renderer(cb, 1);
                for (int b = 0; b < 60; ++b) render2(cb);        /* crossfade + analysis smoothing settle */
                double l2_p    = total_l2();
                double share_p = chan_energy(argmax_channel()) / total_energy();
                int    amax    = argmax_channel();
                CHECK(LD.speakers[amax].pos[2] > 1.0f, "parametric: plane wave from room +z lands on the +z wall");
                CHECK(share_p > share_m * 1.3, "parametric: the direct stream localizes sharper than the matrix decode");
                CHECK(l2_m > 0 && fabs(20.0 * log10(l2_p / l2_m)) < 2.5,
                      "parametric: plane-wave loudness matches the matrix decode (bed_pref)");
                rt_set_bed_renderer(cb, 0);
                for (int b = 0; b < 60; ++b) render2(cb);
                double l2_back = total_l2();
                CHECK(fabs(20.0 * log10(l2_back / l2_m)) < 0.5, "parametric -> matrix round trip restores the decode");
                rt_source_destroy(cb, hp); rt_commit(cb);

                uint32_t sd = rt_load_ambix(cb, DW, err, sizeof err);   /* W-only: fully diffuse */
                uint32_t hd2 = rt_source_create(cb);
                rt_source_play(cb, hd2, sd, true);
                rt_commit(cb);
                for (int b = 0; b < 8; ++b) render2(cb);
                double l2_dm = total_l2();
                rt_set_bed_renderer(cb, 1);
                for (int b = 0; b < 60; ++b) render2(cb);
                double l2_dp = total_l2();
                CHECK(active_channels(0.02) >= 10, "parametric: a diffuse bed stays spread over many speakers");
                CHECK(l2_dm > 0 && fabs(20.0 * log10(l2_dp / l2_dm)) < 2.0,
                      "parametric: diffuse loudness matches the matrix decode (decorrelated, unit energy)");
                rt_source_destroy(cb, hd2); rt_commit(cb);
            } else CHECK(0, "write ambix noise beds");
            rt_destroy(cb);
            remove(PW); remove(DW);
        }
    }

    /* pose prediction (rt_set_pose_prediction): with a tracked listener walking at a constant
     * velocity, the rendered pose LEADS the freshest tracker sample by lead x velocity — the
     * velocity estimated purely from the tracker's own timestamps. Lead 0 = passthrough. */
    {
        RtCore* cp = rt_create(8, 4, RATE, CH);
        CHECK(cp != NULL, "rt_create (pose prediction)");
        if (cp) {
            static PoseSlot slot;                            /* single writer (this thread) */
            memset(&slot, 0, sizeof slot);
            rt_set_tracker(cp, &slot);
            const float q[4] = { 0, 0, 0, 1 };
            bwa_timestamp ts = { 0, 0 };
            float px = 0.f;
            rt_set_pose_prediction(cp, 0.f);                 /* off: readback == written */
            for (int k = 0; k < 20; ++k) {
                px = 0.5f * (float)k * 0.01f;                /* 0.5 m/s, one write per 10 ms */
                float p[3] = { px, 1.5f, 0.f };
                pose_write_t(&slot, p, q, (uint64_t)(k + 1) * 10000000ull);
                rt_render(cp, bus, N, &ts);
            }
            float rp[3], rq[4];
            rt_read_pose(cp, rp, rq);
            CHECK(fabsf(rp[0] - px) < 1e-6f, "prediction off: rendered pose == tracked pose");
            rt_set_pose_prediction(cp, 0.05f);               /* 50 ms lead */
            int k0 = 20;
            for (int k = k0; k < k0 + 60; ++k) {             /* 0.6 s: the velocity estimate settles */
                px = 0.5f * (float)k * 0.01f;
                float p[3] = { px, 1.5f, 0.f };
                pose_write_t(&slot, p, q, (uint64_t)(k + 1) * 10000000ull);
                rt_render(cp, bus, N, &ts);
            }
            rt_read_pose(cp, rp, rq);
            float lead_m = rp[0] - px;                       /* want 0.5 m/s * 0.05 s = 25 mm */
            printf("posepred: lead = %.1f mm (want 25)\n", lead_m * 1000.f);
            CHECK(lead_m > 0.018f && lead_m < 0.030f, "predicted pose leads by ~velocity x lead");
            CHECK(fabsf(rp[1] - 1.5f) < 1e-4f, "no lead on the static axes");
            rt_set_tracker(cp, NULL);
            rt_destroy(cp);
        }
    }

    /* near-listener widening (rt_set_near_spread): a point source close to the listener widens
     * (spread floored by 1 - dist/radius) instead of collapsing into the nearest speaker; a source
     * beyond the radius is untouched. */
    {
        RtCore* cn = rt_create(8, 4, RATE, CH);
        CHECK(cn != NULL, "rt_create (near spread)");
        if (cn) {
            uint32_t ns = rt_load_sound(cn, WAV, err, sizeof err);
            uint32_t hn = rt_source_create(cn);
            rt_source_play(cn, hn, ns, true);
            /* stand the listener 0.3 m from speaker 7, source AT the speaker: without the policy the
             * point solve concentrates there (the collapse the feature exists to prevent) */
            const float* sp7 = LD.speakers[7].pos;
            const float qn[4] = { 0, 0, 0, 1 };
            const float lp7[3] = { sp7[0] - 0.3f, sp7[1], sp7[2] };
            rt_set_listener(cn, lp7, qn);
            set_pos_spk(cn, hn, 7);
            rt_commit(cn); render2(cn);
            int act_close = active_channels(0.03);
            rt_set_near_spread(cn, 1.0f);
            rt_source_set_pos(cn, hn, sp7[0], sp7[1] + 0.001f, sp7[2]);   /* nudge: re-solve with the policy */
            rt_commit(cn); render2(cn);
            int act_near = active_channels(0.03);
            CHECK(act_near > act_close + 2, "a source inside the radius widens (spread floor engages)");
            rt_source_set_pos(cn, hn, lp7[0] - 2.5f, sp7[1], sp7[2]);     /* beyond the radius: point behavior */
            rt_commit(cn); render2(cn); render2(cn);
            int act_far = active_channels(0.03);
            rt_set_near_spread(cn, 0.f);
            rt_source_set_pos(cn, hn, lp7[0] - 2.501f, sp7[1], sp7[2]);
            rt_commit(cn); render2(cn); render2(cn);
            int act_far_off = active_channels(0.03);
            CHECK(abs(act_far - act_far_off) <= 1, "a source beyond the radius is untouched");
            rt_source_destroy(cn, hn); rt_commit(cn);
            rt_destroy(cn);
        }
    }

    /* metric source size (bwa_source_set_size): the rendered width is the angle the radius subtends
     * from the listener — a source that engulfs the listener is fully wide, the SAME physical size
     * narrows with distance, and size 0 restores the point solve. */
    {
        RtCore* cz = rt_create(8, 4, RATE, CH);
        CHECK(cz != NULL, "rt_create (source size)");
        if (cz) {
            uint32_t zs = rt_load_sound(cz, WAV, err, sizeof err);
            uint32_t hz = rt_source_create(cz);
            rt_source_play(cz, hz, zs, true);
            const float* sp7 = LD.speakers[7].pos;
            const float qz[4] = { 0, 0, 0, 1 };
            const float lz[3] = { sp7[0] - 0.5f, sp7[1], sp7[2] };
            rt_set_listener(cz, lz, qz);                 /* 0.5 m from speaker 7 */
            set_pos_spk(cz, hz, 7);                      /* source at the speaker: concentrated point */
            rt_commit(cz); render2(cz);
            int act_point = active_channels(0.03);
            rt_source_set_size(cz, hz, 1.0f);            /* radius 1 m > dist 0.5 m: engulfed */
            rt_commit(cz); render2(cz);
            int act_engulfed = active_channels(0.03);
            CHECK(act_engulfed > act_point + 2, "a source that engulfs the listener goes fully wide");
            rt_source_set_pos(cz, hz, lz[0] + 4.0f, sp7[1], sp7[2]);   /* same 1 m radius, 4 m away */
            rt_commit(cz); render2(cz); render2(cz);
            int act_far = active_channels(0.03);
            CHECK(act_far < act_engulfed, "the same physical size subtends less at distance (narrows)");
            rt_source_set_size(cz, hz, 0.f);             /* back to a point */
            set_pos_spk(cz, hz, 7);
            rt_commit(cz); render2(cz); render2(cz);
            int act_back = active_channels(0.03);
            CHECK(abs(act_back - act_point) <= 1, "size 0 restores the point solve");
            rt_source_destroy(cz, hz); rt_commit(cz);
            rt_destroy(cz);
        }
    }

    /* equal-loudness distance compensation (bwa_source_set_loudness_comp): at -12 dB of distance
     * attenuation a 100 Hz tone gains ~ +4.5 dB of shelf (0.4 dB/dB, below the 250 Hz corner);
     * a 5 kHz tone is untouched; opt-out ramps back to flat. */
    {
        RtCore* cl = rt_create(8, 4, RATE, CH);
        CHECK(cl != NULL, "rt_create (loudness comp)");
        if (cl) {
            const char* LW = "bwa_rt_ldc100.wav";
            if (write_sine_wav(LW, 100.0, 4800)) {
                uint32_t sl = rt_load_sound(cl, LW, err, sizeof err);
                uint32_t hl = rt_source_create(cl);
                rt_source_play(cl, hl, sl, true);
                rt_source_set_pos(cl, hl, 4.0f, 1.5f, 0.f);  /* 4 m: atten = 1/4 = -12 dB (ref 1 m, rolloff 1) */
                rt_commit(cl);
                for (int b = 0; b < 4; ++b) render2(cl);
                double l2_off = 0; for (int b = 0; b < 4; ++b) { render2(cl); l2_off += total_l2(); }
                rt_source_set_loudness_comp(cl, hl, true);
                for (int b = 0; b < 8; ++b) render2(cl);     /* ramp + shelf settle */
                double l2_on = 0; for (int b = 0; b < 4; ++b) { render2(cl); l2_on += total_l2(); }
                double boost_db = 20.0 * log10(l2_on / l2_off);
                printf("ldc: 100 Hz boost at -12 dB atten = %.2f dB (want ~4.5)\n", boost_db);
                CHECK(boost_db > 3.2 && boost_db < 5.6, "LF shelf tracks the attenuation (~0.4 dB/dB)");
                rt_source_set_loudness_comp(cl, hl, false);
                for (int b = 0; b < 8; ++b) render2(cl);
                double l2_back = 0; for (int b = 0; b < 4; ++b) { render2(cl); l2_back += total_l2(); }
                CHECK(fabs(20.0 * log10(l2_back / l2_off)) < 0.3, "opt-out ramps back to flat");
                rt_source_destroy(cl, hl); rt_commit(cl);

                const char* HW2 = "bwa_rt_ldc5k.wav";         /* HF: the shelf must not touch it */
                if (write_sine_wav(HW2, 5000.0, 4800)) {
                    uint32_t sh = rt_load_sound(cl, HW2, err, sizeof err);
                    uint32_t hh = rt_source_create(cl);
                    rt_source_play(cl, hh, sh, true);
                    rt_source_set_pos(cl, hh, 4.0f, 1.5f, 0.f);
                    rt_commit(cl);
                    for (int b = 0; b < 4; ++b) render2(cl);
                    double h_off = 0; for (int b = 0; b < 4; ++b) { render2(cl); h_off += total_l2(); }
                    rt_source_set_loudness_comp(cl, hh, true);
                    for (int b = 0; b < 8; ++b) render2(cl);
                    double h_on = 0; for (int b = 0; b < 4; ++b) { render2(cl); h_on += total_l2(); }
                    CHECK(fabs(20.0 * log10(h_on / h_off)) < 0.8, "the shelf leaves HF content alone");
                    rt_source_destroy(cl, hh); rt_commit(cl);
                    remove(HW2);
                } else CHECK(0, "write 5 kHz sine (ldc)");
                remove(LW);
            } else CHECK(0, "write 100 Hz sine (ldc)");
            rt_destroy(cl);
        }
    }

    /* multi-listener compromise (rt_set_extra_listeners): one listener west of a centred source
     * biases the render east (DBAP weights the source's bearing); adding a mirrored second listener
     * makes the compromise SYMMETRIC at unchanged total power; clearing restores the bias. */
    {
        RtCore* cm = rt_create(8, 4, RATE, CH);
        CHECK(cm != NULL, "rt_create (multi-listener)");
        if (cm) {
            uint32_t sm = rt_load_sound(cm, WAV, err, sizeof err);
            uint32_t hm = rt_source_create(cm);
            rt_source_play(cm, hm, sm, true);
            rt_source_set_pos(cm, hm, 0.f, 1.5f, 0.f);       /* the array centre */
            const float qid2[4] = { 0, 0, 0, 1 };
            rt_set_listener(cm, (const float[3]){ -1.f, 1.5f, 0.f }, qid2);
            rt_commit(cm); render2(cm);
            #define SIDE_E(SGN, OUT) do {                                                  \
                double e_ = 0;                                                             \
                for (int ch_ = 0; ch_ < CH; ++ch_)                                         \
                    if ((SGN) * LD.speakers[ch_].pos[0] > 1.f) e_ += chan_energy(ch_);     \
                (OUT) = e_;                                                                \
            } while (0)
            double epx, enx;
            SIDE_E(+1, epx); SIDE_E(-1, enx);
            double l2_single = total_l2();
            CHECK(epx > enx * 1.15, "single listener west of the source biases the render east");
            const float exl[3] = { 1.f, 1.5f, 0.f };         /* the mirrored second occupant */
            rt_set_extra_listeners(cm, exl, 1);
            rt_commit(cm); render2(cm); render2(cm);
            double epx2, enx2;
            SIDE_E(+1, epx2); SIDE_E(-1, enx2);
            double l2_multi = total_l2();
            CHECK(fabs(epx2 - enx2) / (epx2 + enx2) < 0.08,
                  "mirrored second listener makes the compromise symmetric (energy mean)");
            CHECK(fabs(20.0 * log10(l2_multi / l2_single)) < 1.0,
                  "compromise panning preserves total power");
            rt_set_extra_listeners(cm, NULL, 0);             /* back to single-listener panning */
            rt_commit(cm); render2(cm); render2(cm);
            double epx3, enx3;
            SIDE_E(+1, epx3); SIDE_E(-1, enx3);
            CHECK(epx3 > enx3 * 1.15, "clearing the extras restores single-listener panning");
            #undef SIDE_E
            rt_source_destroy(cm, hm); rt_commit(cm);
            rt_destroy(cm);
        }
    }

    /* pitch (bwa_source_set_pitch): a looping 1 kHz sine at rate 2 doubles its zero-crossing count,
     * at rate 0.5 halves it; rate 1 is the untouched integer path. */
    {
        RtCore* cz = rt_create(8, 4, RATE, CH);
        CHECK(cz != NULL, "rt_create (pitch)");
        if (cz) {
            const char* PW2 = "bwa_rt_pitch1k.wav";
            if (write_sine_wav(PW2, 1000.0, 4800)) {           /* integer cycles: seamless loop */
                enum { KB = 8 };
                static float mono[KB * N];
                uint32_t sp2 = rt_load_sound(cz, PW2, err, sizeof err);
                uint32_t hp2 = rt_source_create(cz);
                rt_source_play(cz, hp2, sp2, true);
                rt_source_set_pos(cz, hp2, 0.f, 1.5f, 0.f);
                rt_commit(cz); render2(cz);
                render_capture_mono(cz, mono, KB);
                int zc1 = count_zc(mono, KB * N);
                rt_source_set_pitch(cz, hp2, 2.0f);
                render2(cz); render2(cz);                      /* glide lands within a block; settle */
                render_capture_mono(cz, mono, KB);
                int zc2 = count_zc(mono, KB * N);
                rt_source_set_pitch(cz, hp2, 0.5f);
                render2(cz); render2(cz);
                render_capture_mono(cz, mono, KB);
                int zch = count_zc(mono, KB * N);
                printf("pitch: zc x1=%d x2=%d x0.5=%d\n", zc1, zc2, zch);
                CHECK(zc1 > 60, "baseline tone renders");
                CHECK(fabs((double)zc2 / zc1 - 2.0) < 0.12, "pitch 2.0 doubles the frequency");
                CHECK(fabs((double)zch / zc1 - 0.5) < 0.06, "pitch 0.5 halves the frequency");
                rt_source_destroy(cz, hp2); rt_commit(cz);
                remove(PW2);
            } else CHECK(0, "write 1 kHz sine (pitch)");
            rt_destroy(cz);
        }
    }

    /* bed rotation (bwa_bed_set_orientation yaw-only): a plane-wave bed from room +z, yawed +pi/2,
     * re-localizes on the +x wall at conserved level — the closed-form yaw SH rotation, glided. */
    {
        RtCore* cr = rt_create(8, 4, RATE, CH);
        CHECK(cr != NULL, "rt_create (bed rotation)");
        if (cr) {
            const char* BW2 = "bwa_rt_bed_rot.wav";
            if (write_ambix4_noise_wav(BW2, 1.f, 0.f, 0.f, 1.f, 8 * N)) {
                uint32_t sb = rt_load_ambix(cr, BW2, err, sizeof err);
                uint32_t hb = rt_source_create(cr);
                rt_source_play(cr, hb, sb, true);
                rt_commit(cr);
                for (int b = 0; b < 8; ++b) render2(cr);
                int    a0  = argmax_channel();
                double l0  = total_l2();
                CHECK(LD.speakers[a0].pos[2] > 1.0f, "unrotated plane wave lands on the +z wall");
                rt_bed_set_orientation(cr, hb, 1.5707963f, 0.f, 0.f);  /* +90° yaw: field turns toward room +x */
                for (int b = 0; b < 80; ++b) render2(cr);      /* glide (0.25 s at 1 turn/s) + settle */
                int    a1 = argmax_channel();
                double l1 = total_l2();
                CHECK(LD.speakers[a1].pos[0] > 1.0f && fabsf(LD.speakers[a1].pos[2]) < 1.0f,
                      "rotated +90 deg: the field re-localizes on the +x wall");
                CHECK(fabs(20.0 * log10(l1 / l0)) < 1.5, "rotation conserves level (orthogonal transform)");
                rt_source_destroy(cr, hb); rt_commit(cr);
                remove(BW2);
            } else CHECK(0, "write rotation bed");
            rt_destroy(cr);
        }
    }

    /* bed orientation (bwa_bed_set_orientation): pitch +90 deg tilts a front plane wave to the
     * CEILING at conserved level (the full Ivanic-Ruedenberg matrix path), and steering back to a
     * yaw-only pose re-localizes on the +x wall — the matrix -> phasor handoff is seamless. */
    {
        RtCore* co = rt_create(8, 4, RATE, CH);
        CHECK(co != NULL, "rt_create (bed orientation)");
        if (co) {
            const char* BW3 = "bwa_rt_bed_ori.wav";
            if (write_ambix4_noise_wav(BW3, 1.f, 0.f, 0.f, 1.f, 8 * N)) {
                uint32_t sb = rt_load_ambix(co, BW3, err, sizeof err);
                uint32_t hb = rt_source_create(co);
                rt_source_play(co, hb, sb, true);
                rt_commit(co);
                for (int b = 0; b < 8; ++b) render2(co);
                double l0 = total_l2();
                rt_bed_set_orientation(co, hb, 0.f, 1.5707963f, 0.f);   /* front tilts up */
                for (int b = 0; b < 80; ++b) render2(co);               /* glide (0.25 s) + settle */
                int    a1 = argmax_channel();
                double l1 = total_l2();
                CHECK(LD.speakers[a1].pos[1] > 2.0f && fabsf(LD.speakers[a1].pos[0]) < 1.0f &&
                      fabsf(LD.speakers[a1].pos[2]) < 1.0f,
                      "pitch +90 deg: the field re-localizes on the ceiling");
                CHECK(fabs(20.0 * log10(l1 / l0)) < 1.5, "full rotation conserves level (orthogonal blocks)");
                rt_bed_set_orientation(co, hb, 1.5707963f, 0.f, 0.f);   /* back to yaw-only: phasor handoff */
                for (int b = 0; b < 120; ++b) render2(co);
                int    a2 = argmax_channel();
                double l2 = total_l2();
                CHECK(LD.speakers[a2].pos[0] > 1.0f && fabsf(LD.speakers[a2].pos[2]) < 1.0f,
                      "settling to yaw-only lands on the +x wall (matrix -> phasor handoff)");
                CHECK(fabs(20.0 * log10(l2 / l0)) < 1.5, "level still conserved after the handoff");
                rt_source_destroy(co, hb); rt_commit(co);
                remove(BW3);
            } else CHECK(0, "write orientation bed");
            rt_destroy(co);
        }
    }

    /* max-rE bed weighting (bwa_set_max_re): for a plane-wave FOA bed the matrix decode's REAR
     * hemisphere is pure sidelobe — the taper must shrink its share of the energy at ~unchanged
     * total level (diffuse-normalized weights), and toggling back restores the raw decode. */
    {
        RtCore* cm = rt_create(8, 4, RATE, CH);
        CHECK(cm != NULL, "rt_create (max-rE)");
        if (cm) {
            const char* BW4 = "bwa_rt_bed_re.wav";
            if (write_ambix4_noise_wav(BW4, 1.f, 0.f, 0.f, 1.f, 8 * N)) {   /* plane wave from room +z */
                uint32_t sb = rt_load_ambix(cm, BW4, err, sizeof err);
                uint32_t hb = rt_source_create(cm);
                rt_source_play(cm, hb, sb, true);
                rt_commit(cm);
                for (int b = 0; b < 8; ++b) render2(cm);
                #define REAR_SHARE(OUT) do { double r_ = 0, t_ = 0;                     \
                    for (int ch_ = 0; ch_ < CH; ++ch_) { double e_ = chan_energy(ch_);  \
                        t_ += e_; if (LD.speakers[ch_].pos[2] < LD.ref[2] - 0.5f) r_ += e_; } \
                    (OUT) = t_ > 0 ? r_ / t_ : 1.0; } while (0)
                double share_off, share_on, share_back, l_off, l_on;
                REAR_SHARE(share_off); l_off = total_l2();
                rt_set_max_re(cm, 1);
                for (int b = 0; b < 8; ++b) render2(cm);     /* re_mix crossfades within a block */
                REAR_SHARE(share_on); l_on = total_l2();
                printf("max-rE: rear share %.3f -> %.3f\n", share_off, share_on);
                CHECK(share_on < 0.6 * share_off, "max-rE shrinks the rear-hemisphere sidelobes");
                CHECK(fabs(20.0 * log10(l_on / l_off)) < 2.0, "max-rE keeps the level (energy-normalized weights)");
                rt_set_max_re(cm, 0);
                for (int b = 0; b < 8; ++b) render2(cm);
                REAR_SHARE(share_back);
                CHECK(fabs(share_back - share_off) < 0.05, "max-rE off restores the raw decode (A/B round trip)");
                #undef REAR_SHARE
                rt_source_destroy(cm, hb); rt_commit(cm);
                remove(BW4);
            } else CHECK(0, "write max-rE bed");
            rt_destroy(cm);
        }
    }

    /* band-split max-rE (bwa_set_max_re_split): with the split on, the taper acts only ABOVE the
     * 700 Hz crossover — a LOW tone's speaker distribution stays (nearly) the raw decode's while a
     * HIGH tone's matches the broadband taper's. Tones at 187.5 Hz / 6 kHz (integer cycles per
     * 8-block loop; the one-pole crossover leaks ~7% LF energy into the high band, so "nearly" =
     * much closer to raw than the broadband taper is, ratio-asserted). */
    {
        RtCore* cs = rt_create(8, 4, RATE, CH);
        CHECK(cs != NULL, "rt_create (max-rE split)");
        if (cs) {
            const char* BLO = "bwa_rt_bed_relo.wav", *BHI = "bwa_rt_bed_rehi.wav";
            if (write_ambix4_sine_wav(BLO, 1.f, 0.f, 0.f, 1.f, 187.5, 8 * N) &&
                write_ambix4_sine_wav(BHI, 1.f, 0.f, 0.f, 1.f, 6000.0, 8 * N)) {
                uint32_t slo = rt_load_ambix(cs, BLO, err, sizeof err);
                uint32_t shi = rt_load_ambix(cs, BHI, err, sizeof err);
                uint32_t hb  = rt_source_create(cs);
                bwa_timestamp tsb = { 0, 0 };
                double D[3][CH];                     /* [config][ch]: normalized loop-energy distribution */
                #define BED_DIST(OUT) do { double t_ = 0;                                   \
                    for (int b_ = 0; b_ < 6; ++b_) rt_render(cs, bus, N, &tsb);   /* settle */ \
                    for (int ch_ = 0; ch_ < CH; ++ch_) (OUT)[ch_] = 0.0;                    \
                    for (int b_ = 0; b_ < 8; ++b_) {                            /* one loop */ \
                        rt_render(cs, bus, N, &tsb);                                        \
                        for (int ch_ = 0; ch_ < CH; ++ch_) (OUT)[ch_] += chan_energy(ch_);  \
                    }                                                                       \
                    for (int ch_ = 0; ch_ < CH; ++ch_) t_ += (OUT)[ch_];                    \
                    for (int ch_ = 0; ch_ < CH; ++ch_) (OUT)[ch_] /= (t_ > 0 ? t_ : 1.0);   \
                } while (0)
                #define DIST_LINF(A, B, OUT) do { double m_ = 0;                            \
                    for (int ch_ = 0; ch_ < CH; ++ch_) { double d_ = fabs((A)[ch_] - (B)[ch_]); \
                        if (d_ > m_) m_ = d_; } (OUT) = m_; } while (0)
                for (int tone = 0; tone < 2; ++tone) {
                    rt_set_max_re(cs, 0); rt_set_max_re_split(cs, 0);
                    rt_source_play(cs, hb, tone ? shi : slo, true);
                    rt_commit(cs);
                    BED_DIST(D[0]);                                  /* raw decode */
                    rt_set_max_re(cs, 1);
                    BED_DIST(D[1]);                                  /* broadband taper */
                    rt_set_max_re_split(cs, 1);
                    BED_DIST(D[2]);                                  /* band-split taper */
                    double d_bb, d_sp;
                    DIST_LINF(D[1], D[0], d_bb);                     /* broadband vs raw */
                    DIST_LINF(D[2], tone ? D[1] : D[0], d_sp);       /* split vs its own reference */
                    printf("re-split %s: broadband-vs-raw %.4f  split-vs-%s %.4f\n",
                           tone ? "hi" : "lo", d_bb, tone ? "broadband" : "raw", d_sp);
                    if (tone) CHECK(d_sp < 0.25 * d_bb, "high tone: the split taper matches the broadband taper");
                    else      CHECK(d_bb > 0.01 && d_sp < 0.4 * d_bb,
                                    "low tone: the split leaves the raw (rV-optimal) decode in place");
                }
                #undef BED_DIST
                #undef DIST_LINF
                rt_set_max_re(cs, 0); rt_set_max_re_split(cs, 0);
                rt_source_destroy(cs, hb); rt_commit(cs);
                remove(BLO); remove(BHI);
            } else CHECK(0, "write split-re beds");
            rt_destroy(cs);
        }
    }

    /* anisotropic extent (bwa_source_set_extent): a WIDE-FLAT source (width 1, height 0) keeps its
     * energy in the horizontal band around the source — its vertical spill is well under the
     * isotropic spread's — and a TALL-THIN one (0, 1) reverses that; equal extents ARE the isotropic
     * spread (identical solve path). MDAP + lobe modes (spectral shares the same ellipse). */
    {
        RtCore* ce = rt_create(8, 4, RATE, CH);
        CHECK(ce != NULL, "rt_create (extent)");
        if (ce) {
            const char* NW4 = "bwa_rt_extnoise.wav";
            if (write_noise_wav(NW4, 8 * N)) {
                uint32_t nf = rt_load_sound(ce, NW4, err, sizeof err);
                uint32_t hx = rt_source_create(ce);
                rt_source_play(ce, hx, nf, true);
                rt_source_set_pos(ce, hx, 1.5f, 1.5f, 0.f);          /* the +x wall's centre speaker */
                bwa_timestamp tse = { 0, 0 };
                /* vertical spill: energy on speakers OFF the source's horizontal plane (y != 1.5) */
                #define VSPILL(OUT) do { double v_ = 0, t_ = 0;                                \
                    for (int b_ = 0; b_ < 4; ++b_) rt_render(ce, bus, N, &tse);   /* settle */  \
                    for (int b_ = 0; b_ < 8; ++b_) {                              /* one loop */ \
                        rt_render(ce, bus, N, &tse);                                           \
                        for (int ch_ = 0; ch_ < CH; ++ch_) { double e_ = chan_energy(ch_);     \
                            t_ += e_; if (fabs(LD.speakers[ch_].pos[1] - 1.5f) > 0.1f) v_ += e_; } \
                    }                                                                          \
                    (OUT) = t_ > 0 ? v_ / t_ : 0.0; } while (0)
                for (int mode = 0; mode <= 1; ++mode) {              /* lobe, MDAP */
                    rt_set_spread_mode(ce, mode);
                    double sp_iso, sp_flat, sp_tall;
                    rt_source_set_spread(ce, hx, 1.f);   rt_commit(ce); VSPILL(sp_iso);
                    rt_source_set_extent(ce, hx, 1.f, 0.f); rt_commit(ce); VSPILL(sp_flat);
                    rt_source_set_extent(ce, hx, 0.f, 1.f); rt_commit(ce); VSPILL(sp_tall);
                    printf("extent mode %d: vspill iso %.3f flat %.3f tall %.3f\n",
                           mode, sp_iso, sp_flat, sp_tall);
                    CHECK(sp_flat < 0.6 * sp_iso, "wide-flat extent keeps energy near the horizontal plane");
                    CHECK(sp_tall > sp_iso * 0.9, "tall-thin extent keeps (or grows) the vertical share");
                }
                /* equal extents = the isotropic spread, same solve path (bit-exact distributions) */
                {
                    rt_set_spread_mode(ce, 1);
                    double da[CH], db[CH];
                    rt_source_set_spread(ce, hx, 0.7f); rt_commit(ce);
                    { double t_ = 0; for (int b_ = 0; b_ < 4; ++b_) rt_render(ce, bus, N, &tse);
                      for (int ch_ = 0; ch_ < CH; ++ch_) da[ch_] = 0.0;
                      for (int b_ = 0; b_ < 8; ++b_) { rt_render(ce, bus, N, &tse);
                          for (int ch_ = 0; ch_ < CH; ++ch_) da[ch_] += chan_energy(ch_); }
                      for (int ch_ = 0; ch_ < CH; ++ch_) t_ += da[ch_];
                      for (int ch_ = 0; ch_ < CH; ++ch_) da[ch_] /= (t_ > 0 ? t_ : 1.0); }
                    rt_source_set_extent(ce, hx, 0.7f, 0.7f); rt_commit(ce);
                    { double t_ = 0; for (int b_ = 0; b_ < 4; ++b_) rt_render(ce, bus, N, &tse);
                      for (int ch_ = 0; ch_ < CH; ++ch_) db[ch_] = 0.0;
                      for (int b_ = 0; b_ < 8; ++b_) { rt_render(ce, bus, N, &tse);
                          for (int ch_ = 0; ch_ < CH; ++ch_) db[ch_] += chan_energy(ch_); }
                      for (int ch_ = 0; ch_ < CH; ++ch_) t_ += db[ch_];
                      for (int ch_ = 0; ch_ < CH; ++ch_) db[ch_] /= (t_ > 0 ? t_ : 1.0); }
                    double dmax = 0;
                    for (int ch_ = 0; ch_ < CH; ++ch_) { double d_ = fabs(da[ch_] - db[ch_]); if (d_ > dmax) dmax = d_; }
                    CHECK(dmax < 1e-6, "equal extents render exactly as the isotropic spread");
                }
                #undef VSPILL
                rt_set_spread_mode(ce, 0);
                rt_source_destroy(ce, hx); rt_commit(ce);
                remove(NW4);
            } else CHECK(0, "write extent noise wav");
            rt_destroy(ce);
        }
    }

    /* asset metadata (rt_sound_frames / rt_sound_channels) + the per-source attenuation override
     * (rt_source_set_attenuation): metadata reports what load stored — streamed assets report the
     * decoder's file length; the override swaps the LAYOUT distance curve for the source's own,
     * applied by ratio in the solve — rolloff 0 pins a constant-level source, a steeper curve
     * attenuates harder, and clearing (ref <= 0) restores the layout behavior exactly. */
    {
        RtCore* ca = rt_create(8, 8, RATE, CH);
        CHECK(ca != NULL, "rt_create (atten/meta)");
        if (ca) {
            uint32_t sm = rt_load_sound(ca, WAV, err, sizeof err);
            CHECK(rt_sound_frames(ca, sm) == 8 * N && rt_sound_channels(ca, sm) == 1,
                  "in-memory metadata: frames + channels as loaded");
            uint32_t st = rt_load_sound_streaming(ca, WAV, err, sizeof err);
            CHECK(st != 0 && rt_sound_frames(ca, st) == 8 * N,
                  "streamed metadata: the decoder's file length");
            CHECK(rt_sound_frames(ca, 0) == 0 && rt_sound_channels(ca, 0) == 0, "invalid handle reads 0");
            const char* B4 = "bwa_rt_meta4.wav";
            if (write_ambix4_noise_wav(B4, 1.f, 0.f, 0.f, 1.f, 8 * N)) {
                uint32_t sb = rt_load_ambix(ca, B4, err, sizeof err);
                CHECK(rt_sound_channels(ca, sb) == 4 && rt_sound_frames(ca, sb) == 8 * N,
                      "bed metadata: channel count + frames");
                remove(B4);
            } else CHECK(0, "write meta bed wav");

            uint32_t h = rt_source_create(ca);
            rt_source_play(ca, h, sm, true);                 /* WAV = constant 1.0: a clean level probe */
            bwa_timestamp tsa = { 0, 0 };
            #define LVL(OUT) do { double a_ = 0;                                                  \
                rt_commit(ca);                                                                    \
                for (int b_ = 0; b_ < 2; ++b_) rt_render(ca, bus, N, &tsa);   /* ramps land */    \
                for (int b_ = 0; b_ < 8; ++b_) { rt_render(ca, bus, N, &tsa); a_ += total_l2(); } \
                (OUT) = a_; } while (0)
            double l_near, l_far, l_flat, l_steep, l_back;
            rt_source_set_pos(ca, h, 0.5f, LD.ref[1], 0.f);  LVL(l_near);   /* inside ref: atten = 1 */
            rt_source_set_pos(ca, h, 4.0f, LD.ref[1], 0.f);  LVL(l_far);    /* layout rolloff 1: 1/4 */
            printf("atten: near/far %.2f (want ~4)  ", l_near / l_far);
            CHECK(fabs(l_near / l_far - 4.0) < 0.5, "layout curve attenuates 1/d (rolloff 1)");
            rt_source_set_attenuation(ca, h, 1.0f, 0.0f, 0.0f); LVL(l_flat);
            CHECK(fabs(l_flat / l_near - 1.0) < 0.05, "override rolloff 0 = constant level at any distance");
            rt_source_set_attenuation(ca, h, 1.0f, 2.0f, 0.0f); LVL(l_steep);
            printf("near/steep %.2f (want ~16)\n", l_near / l_steep);
            CHECK(fabs(l_near / l_steep - 16.0) < 2.0, "override rolloff 2 attenuates 1/d^2");
            rt_source_set_attenuation(ca, h, 0.0f, 0.0f, 0.0f); LVL(l_back);
            CHECK(fabs(l_back / l_far - 1.0) < 0.05, "clearing the override restores the layout curve");
            #undef LVL
            rt_source_destroy(ca, h); rt_commit(ca);
            rt_destroy(ca);
        }
    }

    /* image-source early reflections (bwa_source_set_early_reflections): a source hard against the +x
     * wall of a shoebox. Its +x image sits just BEYOND that wall, so the reflection must (a) appear
     * only when enabled, (b) arrive AFTER the direct sound, and (c) come from the +x side — a real
     * point source panned at the mirrored position, not a diffuse bed. */
    {
        RtCore* ci = rt_create(8, 4, RATE, CH);
        CHECK(ci != NULL, "rt_create (early reflections)");
        if (ci) {
            IsmRoom room; memset(&room, 0, sizeof room);
            room.w = 6.f; room.h = 3.f; room.d = 6.f; room.valid = 1;
            for (int f = 0; f < ISM_FACES; ++f)                  /* lively walls: strong first-order returns */
                for (int b = 0; b < 3; ++b) room.absorb[f][b] = 0.1f;
            rt_set_ism_room(ci, &room);
            const float qi[4] = { 0, 0, 0, 1 };
            rt_set_listener(ci, (const float[3]){ 0.f, 1.5f, 0.f }, qi);   /* centre of the room */

            const char* IW = "bwa_rt_imp.wav";
            enum { IMP_AT = 300 };                               /* fires after the voice's one-block gain ramp-in */
            if (write_impulse_at_wav(IW, IMP_AT, 8 * N)) {
                enum { KB = 6 };                                 /* 1536 samples: past every reflection path */
                static double env[KB * N], envp[KB * N], envn[KB * N];   /* |sum| all / +x side / -x side */
                uint32_t si = rt_load_sound(ci, IW, err, sizeof err);
                uint32_t hi = rt_source_create(ci);
                /* fire the impulse and capture KB blocks: the per-sample envelope over the whole
                 * capture, split into the +x and -x speaker halves (the reflection's direction). */
                #define ISM_CAPTURE() do {                                                        \
                    rt_source_play(ci, hi, si, false);                                            \
                    rt_source_set_pos(ci, hi, 2.5f, 1.5f, 0.f);   /* 0.5 m from the +x wall */    \
                    rt_commit(ci);                                                                \
                    bwa_timestamp ts_ = { 0, 0 };                                                   \
                    for (int b_ = 0; b_ < KB; ++b_) {                                             \
                        rt_render(ci, bus, N, &ts_);                                              \
                        for (int i_ = 0; i_ < (int)N; ++i_) {                                     \
                            double a_ = 0, p_ = 0, n_ = 0;                                        \
                            for (int ch_ = 0; ch_ < CH; ++ch_) {                                  \
                                double v_ = fabs(bus[(size_t)ch_ * N + i_]);                      \
                                a_ += v_;                                                         \
                                if (LD.speakers[ch_].pos[0] >  1.f) p_ += v_;                     \
                                if (LD.speakers[ch_].pos[0] < -1.f) n_ += v_;                     \
                            }                                                                     \
                            env[b_ * (int)N + i_] = a_; envp[b_ * (int)N + i_] = p_;              \
                            envn[b_ * (int)N + i_] = n_;                                          \
                        }                                                                         \
                    }                                                                             \
                } while (0)
                #define ISM_SUM(A, B, OUT, SRC) do {                                              \
                    double s_ = 0; for (int i_ = (A); i_ < (B); ++i_) s_ += (SRC)[i_]; (OUT) = s_; \
                } while (0)

                ISM_CAPTURE();                                   /* dry (ISM off): the direct sound only */
                double dry_direct, dry_late;
                ISM_SUM(IMP_AT - 10, IMP_AT + 50, dry_direct, env);
                ISM_SUM(IMP_AT + 120, KB * (int)N, dry_late, env);   /* the reflection window: silent when dry */
                CHECK(dry_direct > 1e-3, "the direct impulse renders");
                CHECK(dry_late < dry_direct * 0.02, "no reflections without the opt-in");

                rt_source_set_ism(ci, hi, true);
                rt_commit(ci);
                ISM_CAPTURE();                                   /* wet: direct + the six wall images */
                double wet_direct, wet_late, wet_px, wet_nx;
                ISM_SUM(IMP_AT - 10, IMP_AT + 50, wet_direct, env);
                ISM_SUM(IMP_AT + 120, KB * (int)N, wet_late, env);
                CHECK(fabs(wet_direct - dry_direct) / dry_direct < 0.05, "the direct sound is unchanged");
                CHECK(wet_late > dry_direct * 0.05, "reflections arrive AFTER the direct sound");
                /* Every arrival is pinned by geometry (source (2.5,1.5,0), listener (0,1.5,0), room
                 * 6x3x6 -> x,z in +-3, y in [0,3]):
                 *   +x wall  image (3.5, 1.5, 0)     -> 3.50 m -> 490 samples
                 *   floor    image (2.5,-1.5, 0)     -> 3.91 m -> 546  (coincident with the ceiling,
                 *   ceiling  image (2.5, 4.5, 0)     -> 3.91 m -> 546   so this PAIR is the largest peak)
                 *   +-z wall images (2.5, 1.5, +-6)  -> 6.50 m -> 909
                 * The peak past the direct sound is therefore the floor/ceiling pair at ~546. */
                int pk = IMP_AT + 120; double pkv = 0;          /* search PAST the direct arrival */
                for (int i = IMP_AT + 120; i < KB * (int)N; ++i) if (env[i] > pkv) { pkv = env[i]; pk = i; }
                printf("ism: strongest reflection %d samples after the direct (floor+ceiling pair: 546)\n", pk - IMP_AT);
                CHECK(pk - IMP_AT > 500 && pk - IMP_AT < 590, "reflections land at their geometric path delays");
                /* the near +x wall's own reflection (~490) must come from the +x side: it is a point
                 * source at the mirrored position, not a diffuse bed */
                ISM_SUM(IMP_AT + 460, IMP_AT + 520, wet_px, envp);
                ISM_SUM(IMP_AT + 460, IMP_AT + 520, wet_nx, envn);
                CHECK(wet_px > wet_nx * 1.2, "the +x wall's reflection arrives from the +x side");

                rt_source_set_ism(ci, hi, false);                /* opt out: the reflections ramp away */
                rt_commit(ci);
                ISM_CAPTURE();
                double off_direct, off_late;
                ISM_SUM(IMP_AT - 10, IMP_AT + 50, off_direct, env);
                ISM_SUM(IMP_AT + 120, KB * (int)N, off_late, env);
                CHECK(fabs(off_direct - dry_direct) / dry_direct < 0.05, "direct sound survives the opt-out");
                CHECK(off_late < dry_direct * 0.02, "opting out silences the reflections");
                #undef ISM_SUM
                #undef ISM_CAPTURE
                rt_source_destroy(ci, hi); rt_commit(ci);
                remove(IW);
            } else CHECK(0, "write impulse wav (ism)");
            rt_destroy(ci);
        }
    }

    /* propagation effects (opt-in per voice): air absorption (distance low-pass) + Doppler (glided delay) */
    {
        RtCore* cp = rt_create(8, 4, RATE, CH);
        CHECK(cp != NULL, "rt_create (propagation)");
        if (cp) {
            /* air absorption: at a far distance, enabling it dulls an 8 kHz tone (same position, so
             * the panning + distance attenuation are identical — the energy drop is purely the LPF). */
            const char* SW8 = "bwa_rt_sine8k.wav";
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
            const char* IW = "bwa_rt_impulse.wav";
            if (write_impulse_wav(IW, 16 * N)) {
                uint32_t si = rt_load_sound(cp, IW, err, sizeof err);
                float cap[4 * N];
                uint32_t hoff = rt_source_create(cp);
                rt_source_set_pos(cp, hoff, 3.43f, LD.ref[1], 0.f);   /* ear plane: distance == 3.43 m */
                rt_source_play(cp, hoff, si, false);
                rt_commit(cp);
                render_capture_mono(cp, cap, 4);
                int peak_off = argmax_abs(cap, 4 * N);
                rt_source_destroy(cp, hoff); rt_commit(cp);

                uint32_t hon = rt_source_create(cp);
                rt_source_set_pos(cp, hon, 3.43f, LD.ref[1], 0.f);
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
            const char* SW1 = "bwa_rt_sine1k.wav";
            if (write_sine_wav(SW1, 1000.0, 128 * N)) {
                /* The delay smoother is heavy (low cutoff, for a clean spectrum), so warm up past its
                 * group delay + the ring fill, then measure the STEADY-STATE pitch over the tail. */
                enum { WARM = 16, KB = 24, TOT = WARM + KB };
                uint32_t s1 = rt_load_sound(cp, SW1, err, sizeof err);
                float cap[KB * N];
                bwa_timestamp ts = { 0, 0 };

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
            { bwa_timestamp ts = { 0, 0 }; rt_render(cr, bus, N, &ts); }   /* first block after replay */
            CHECK(g_aux_energy == 0.0, "replay after disabling reflections sends no stale burst");
            rt_source_destroy(cr, hr); rt_commit(cr);
            rt_destroy(cr);
        }
    }

    /* dual-band panning: low band amplitude-normalised (Sigma|g|=gain), high band power (Sigma g^2=gain^2) */
    {
        RtCore* cdb = rt_create(8, 4, RATE, CH);
        CHECK(cdb != NULL, "rt_create (dual-band)");
        if (cdb) {
            const char* LW = "bwa_rt_lo.wav";
            if (write_sine_wav(LW, 200.0, 8 * N)) {              /* a tone below the 700 Hz crossover */
                uint32_t sl = rt_load_sound(cdb, LW, err, sizeof err);
                uint32_t hd = rt_source_create(cdb);
                rt_source_play(cdb, hd, sl, true);
                rt_source_set_pos(cdb, hd, 1.0f, 0.0f, 1.0f);    /* off-speaker -> spreads across channels */
                rt_set_dual_band(cdb, 0); rt_commit(cdb); render2(cdb);
                double l2_off = total_l2(); int amax_off = argmax_channel();
                rt_set_dual_band(cdb, 1); rt_commit(cdb); render2(cdb);
                double l2_on = total_l2(); int amax_on = argmax_channel();
                /* amplitude-norm gains have lower L2 than power-norm for a spread source (the LF relies on
                 * coherent summation at the listener), and the panning DIRECTION is unchanged. */
                CHECK(l2_off > 0 && l2_on < 0.85 * l2_off, "dual-band LF uses amplitude norm (lower per-channel power)");
                CHECK(amax_on == amax_off, "dual-band preserves the localization direction");
                /* the A/B toggle CROSSFADES (invariant 4): the first block after enabling lands BETWEEN
                 * single and dual, not straight at dual — a hard switch would step the LF re-weight in
                 * one sample. (dual L2 < single L2, so a gradual transition sits above the settled dual.) */
                {
                    bwa_timestamp tsd = { 0, 0 };
                    rt_set_dual_band(cdb, 0); rt_commit(cdb); render2(cdb);        /* settle back to single */
                    rt_set_dual_band(cdb, 1); rt_commit(cdb);
                    rt_render(cdb, bus, N, &tsd);  double l2_trans = total_l2();   /* one block: crossfade in progress */
                    CHECK(l2_trans > l2_on * 1.02 && l2_trans <= l2_off * 1.02,
                          "dual-band toggle crossfades (transition block between dual and single, not a jump)");
                }
                /* a HIGH tone (above the crossover) is unaffected by dual-band: it stays in the power band */
                const char* HW = "bwa_rt_hi.wav";
                if (write_sine_wav(HW, 5000.0, 8 * N)) {
                    uint32_t sh = rt_load_sound(cdb, HW, err, sizeof err);
                    uint32_t hh = rt_source_create(cdb);
                    rt_source_play(cdb, hh, sh, true);
                    rt_source_set_pos(cdb, hh, 1.0f, 0.0f, 1.0f);
                    rt_set_dual_band(cdb, 0); rt_commit(cdb); render2(cdb); double h_off = total_l2();
                    rt_set_dual_band(cdb, 1); rt_commit(cdb); render2(cdb); double h_on = total_l2();
                    double lf_chg = (l2_off - l2_on) / l2_off, hf_chg = h_off > 0 ? fabs(h_on - h_off) / h_off : 1;
                    CHECK(hf_chg < lf_chg, "dual-band changes the LF band more than the HF band");
                    rt_source_destroy(cdb, hh); rt_commit(cdb); remove(HW);
                } else CHECK(0, "write 5 kHz sine");
                /* the LF (amplitude) band must still attenuate with distance like the HF — the renorm
                 * targets ||g|| (which carries atten), not bare gain (which would cancel it). */
                rt_set_dual_band(cdb, 1);
                rt_source_set_pos(cdb, hd, 1.0f, 0.f, 0.f); rt_commit(cdb); render2(cdb); double lf_near = total_l2();
                rt_source_set_pos(cdb, hd, 4.0f, 0.f, 0.f); rt_commit(cdb); render2(cdb); double lf_far = total_l2();
                CHECK(lf_near > 0 && lf_far < 0.5 * lf_near, "dual-band LF attenuates with distance (atten preserved)");
                rt_source_destroy(cdb, hd); rt_commit(cdb); remove(LW);
            } else CHECK(0, "write 200 Hz sine");
            rt_destroy(cdb);
        }
    }

    remove(WAV);
    if (fails) { printf("rt_feature_test: %d FAILURES\n", fails); return 1; }
    printf("rt_feature_test OK (ambisonic-bed, reflection-tap, pathing+EQ, spread+MDAP+spectral+frame, "
           "tracked-room-EQ, decorrelation, parametric-bed, pose-pred, near-spread, source-size, "
           "loudness-comp, multi-listener, pitch, bed-rotation+orientation, max-rE+split, extent, "
           "asset-meta+attenuation, ISM early-reflections, air+Doppler, reverb-send, dual-band verified)\n");
    return 0;
}
