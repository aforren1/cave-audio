/*
 * abuse_test.c — deliberate MISUSE of the public ABI, against the DLL's real exported entry
 * points (linked to bw_audio, not bwa_core, so every call crosses the same guards a client's
 * calls do). Feeds the API what the header says must be survivable: NULL engines and
 * out-pointers, zero counts, stale/recycled/cross-engine handles, wrong call order, NaN/Inf/
 * huge/denormal arguments, pool and ring exhaustion, and asset-kind mismatches.
 *
 * Assertions follow the DOCUMENTED contract (bw_audio.h + docs/api.md): a stale handle is a
 * silent no-op, a rejecting per-frame call sets bwa_last_error, getters answer zero/empty for
 * an invalid handle, and non-finite arguments must never reach the rendered output or a
 * readback. Where the engine currently violates that, the assertion is left expressing the
 * CORRECT behavior — a red run here is a real finding, not a broken test.
 *
 * Determinism: everything that needs a render pump runs on BWA_SINK_MANUAL (single-threaded,
 * sample-counted); the one section that needs a real audio thread uses BWA_SINK_NULL with the
 * shortest sleeps that let a few blocks land.
 */
#include "bw_audio.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>           /* Sleep (the null-sink section only) */

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { ++fails;                                   \
    fprintf(stderr, "FAIL %s:%d: ", "abuse_test.c", __LINE__);                          \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static const char* WAV  = "bwa_abuse_tone.wav";   /* mono 48 k point-source fixture */
static const char* QUAD = "bwa_abuse_quad.wav";   /* 4-ch 48 k AmbiX-shaped bed fixture */

/* 16-bit PCM wav, `nch` interleaved copies of a 440 Hz tone at the engine rate — mono for the
 * point-source cases, 4-ch so bwa_load_ambix yields a genuine multichannel (bed) handle. */
static int write_wav(const char* path, uint32_t frames, uint16_t nch) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    const uint32_t rate = 48000u, data = frames * 2u * nch;
    uint32_t u; uint16_t w;
    fwrite("RIFF", 1, 4, f); u = 36 + data; fwrite(&u, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    u = 16;             fwrite(&u, 4, 1, f);
    w = 1;              fwrite(&w, 2, 1, f);          /* PCM */
    w = nch;            fwrite(&w, 2, 1, f);
    u = rate;           fwrite(&u, 4, 1, f);
    u = rate * 2u * nch; fwrite(&u, 4, 1, f);
    w = (uint16_t)(2 * nch); fwrite(&w, 2, 1, f);
    w = 16;             fwrite(&w, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    for (uint32_t i = 0; i < frames; ++i) {
        int16_t v = (int16_t)(6000.0 * sin(6.2831853 * 440.0 * (double)i / (double)rate));
        for (uint16_t c = 0; c < nch; ++c) fwrite(&v, 2, 1, f);
    }
    fclose(f);
    return 1;
}

static bwa_desc manual_desc(void) {
    bwa_desc d = { .profile = BWA_PROFILE_CAVE, .sample_rate = 48000, .block_size = 256,
                   .sink = BWA_SINK_MANUAL };
    return d;
}

/* Pump `blocks` through the manual sink: 1 = rendered and every sample finite, 0 = the render
 * itself was refused, -1 = a non-finite sample reached the device-bound output (the "NaN got
 * onto the audio thread" failure every non-finite-argument case is probing for). */
static int render_blocks(bwa_engine* e, int blocks) {
    for (int b = 0; b < blocks; ++b) {
        uint32_t ch = 0, nf = 0;
        const float* p = bwa_render_block(e, &ch, &nf);
        if (!p || ch == 0 || nf == 0) return 0;
        for (uint32_t i = 0; i < ch * nf; ++i)
            if (!isfinite(p[i])) return -1;
    }
    return 1;
}

/* pump blocks and track the peak: 1 = all finite, -1 = a non-finite sample reached the output */
static int render_peak(bwa_engine* e, int blocks, double* peak) {
    for (int b = 0; b < blocks; ++b) {
        uint32_t ch = 0, nf = 0;
        const float* p = bwa_render_block(e, &ch, &nf);
        if (!p || ch == 0 || nf == 0) return 0;
        for (uint32_t i = 0; i < ch * nf; ++i) {
            if (!isfinite(p[i])) return -1;
            double a = fabs(p[i]);
            if (a > *peak) *peak = a;
        }
    }
    return 1;
}

/* Reset bwa_last_error to NULL so a "this must stay a SILENT no-op" assertion is not fooled by
 * a leftover string: the error is only cleared at entry to a lifecycle/load-class call, and
 * bwa_load_headphone_eq(e, NULL) is the cheapest one that always succeeds (it clears an EQ that
 * was never loaded). */
static void clr(bwa_engine* e) { bwa_load_headphone_eq(e, NULL); }

/* ---- 1. engine-free calls and the NULL-engine sweep -------------------------------------------
 * Every entry point with e = NULL. Nothing may crash, and the getters must answer the documented
 * inert value (0 / false / "none" / occlusion 1.0). The void setters have nothing to observe
 * beyond not-crashing, which on a NULL engine is the whole contract. */
static void null_engine_sweep(void) {
    float f3[3] = { 1, 2, 3 }, q4[4] = { 0, 0, 0, 1 }, buf[8];
    uint64_t u = 123, v = 456;
    char nm[32];

    CHECK(bwa_get_version() == BWA_VERSION, "DLL/header version mismatch");
    CHECK(bwa_create(NULL) == NULL, "bwa_create(NULL desc) must fail");
    CHECK(bwa_start(NULL) == BWA_ERR_CONFIG, "bwa_start(NULL) must return BWA_ERR_CONFIG");
    CHECK(bwa_stop(NULL) == BWA_ERR_CONFIG, "bwa_stop(NULL) must return BWA_ERR_CONFIG");
    bwa_destroy(NULL);
    CHECK(bwa_last_error(NULL) == NULL, "bwa_last_error(NULL) must be NULL");
    CHECK(strcmp(bwa_get_audio_backend(NULL), "none") == 0, "backend of no engine is \"none\"");
    CHECK(bwa_get_sample_rate(NULL) == 0 && bwa_get_block_size(NULL) == 0,
          "resolved config of a NULL engine reads 0");
    CHECK(bwa_get_channel_count(NULL) == 0, "channel count of a NULL engine reads 0");
    CHECK(bwa_get_dsp_time(NULL) == 0, "dsp time of a NULL engine reads 0");
    CHECK(bwa_get_output_latency_frames(NULL) == 0, "latency of a NULL engine reads 0");
    CHECK(bwa_get_active_voices(NULL) == 0, "active voices of a NULL engine reads 0");
    CHECK(bwa_get_xruns(NULL) == 0, "xruns of a NULL engine reads 0");
    CHECK(!bwa_get_clock(NULL, &u, &v), "clock of a NULL engine must report false");
    {
        bwa_clock_model m;
        CHECK(!bwa_get_clock_model(NULL, &m), "clock model of a NULL engine must report false");
    }
    {
        bwa_health h;
        memset(&h, 0xAA, sizeof h);
        CHECK(!bwa_get_health(NULL, &h), "health of a NULL engine must report false");
        CHECK(h.blocks == 0 && h.xruns == 0, "health out must be ZEROED even on a NULL engine");
    }
    CHECK(bwa_get_speakers(NULL, f3, 1) == 0, "speakers of a NULL engine reads 0");
    CHECK(bwa_get_bus_levels(NULL, buf, 8) == 0, "bus levels of a NULL engine reads 0");
    {
        bwa_source out[4]; uint64_t dropped = 77;
        CHECK(bwa_poll_ended(NULL, out, 4, &dropped) == 0 && dropped == 0,
              "poll_ended on a NULL engine reads 0 events, 0 dropped");
    }
    CHECK(bwa_source_create(NULL) == 0 && bwa_source_create_push(NULL) == 0,
          "source create on a NULL engine returns 0");
    CHECK(bwa_bed_create(NULL) == 0, "bed create on a NULL engine returns 0");
    CHECK(bwa_load_sound(NULL, WAV) == 0 && bwa_load_sound_streaming(NULL, WAV) == 0 &&
          bwa_load_ambix(NULL, QUAD) == 0 && bwa_load_fuma(NULL, QUAD) == 0,
          "loads on a NULL engine return 0");
    CHECK(bwa_sound_get_frames(NULL, 1) == 0 && bwa_sound_get_channels(NULL, 1) == 0,
          "asset metadata on a NULL engine reads 0");
    CHECK(!bwa_source_is_playing(NULL, 1) && bwa_source_get_playhead_frames(NULL, 1) == 0,
          "playback readbacks on a NULL engine read false/0");
    CHECK(!bwa_bed_is_playing(NULL, 1) && bwa_bed_get_playhead_frames(NULL, 1) == 0,
          "bed readbacks on a NULL engine read false/0");
    CHECK(bwa_source_push(NULL, 1, f3, 3) == 0 && bwa_source_push_space(NULL, 1) == 0,
          "push on a NULL engine accepts nothing");
    CHECK(!bwa_play_oneshot(NULL, 1, 0, 0, 0, 1.f), "oneshot on a NULL engine must be refused");
    CHECK(bwa_source_get_occlusion(NULL, 1) == 1.0f, "occlusion of a NULL engine reads clear (1)");
    CHECK(bwa_source_get_directivity(NULL, 1) == 1.0f, "directivity of a NULL engine reads 1");
    CHECK(bwa_material_define(NULL, f3, 0.5f, f3) == 0, "material define on a NULL engine returns 0");
    CHECK(bwa_material_preset(NULL, BWA_MAT_WOOD) == 0, "material preset on a NULL engine returns 0");
    CHECK(bwa_scene_add_dynamic_mesh(NULL, NULL, 0, NULL, 0, 0) == -1,
          "dynamic mesh add on a NULL engine returns -1");
    CHECK(bwa_tracker_status(NULL) == BWA_TRACKER_DISCONNECTED,
          "tracker status of a NULL engine reads DISCONNECTED");
    {
        bwa_tracker_desc td = { 0 };
        CHECK(bwa_tracker_connect(NULL, &td) == BWA_ERR_CONFIG, "tracker connect NULL engine");
    }
    {
        bwa_tuning t;
        bwa_tuning_preset(BWA_SETUP_SEATED, &t);
        CHECK(!bwa_apply_tuning(NULL, &t), "apply_tuning on a NULL engine must be refused");
        CHECK(!bwa_get_tuning(NULL, &t), "get_tuning on a NULL engine must report false");
        bwa_tuning_preset(BWA_SETUP_SEATED, NULL);        /* documented no-op on NULL out */
    }
    CHECK(bwa_load_headphone_eq(NULL, WAV) == BWA_ERR_CONFIG, "hpeq load on a NULL engine");
    CHECK(bwa_render_block(NULL, NULL, NULL) == NULL, "render_block on a NULL engine is NULL");

    /* every void entry point, NULL engine: the contract is simply "do not crash" */
    bwa_source_destroy(NULL, 1);            bwa_source_set_priority(NULL, 1, 200);
    bwa_source_set_pos(NULL, 1, 0, 0, 0);   bwa_source_set_gain(NULL, 1, 1.f);
    bwa_source_fade_to(NULL, 1, 1.f, 1.f);  bwa_source_fade_out(NULL, 1, 1.f);
    bwa_source_set_group(NULL, 1, 0);       bwa_group_set_gain(NULL, 0, 1.f);
    bwa_group_set_paused(NULL, 0, true);    bwa_source_set_pitch(NULL, 1, 1.f);
    bwa_group_stop(NULL, 0);                bwa_stop_all(NULL);
    bwa_source_play(NULL, 1, 1, false);     bwa_source_play_at(NULL, 1, 1, false, 0);
    bwa_source_play_loop(NULL, 1, 1, 0, 0); bwa_source_stop(NULL, 1);
    bwa_source_stop_at(NULL, 1, 0);         bwa_source_queue(NULL, 1, 1, false);
    bwa_source_clear_queue(NULL, 1);        bwa_source_set_paused(NULL, 1, true);
    bwa_source_seek(NULL, 1, 0);            bwa_source_push_end(NULL, 1);
    bwa_set_master_gain(NULL, 1.f);         bwa_set_paused(NULL, true);
    bwa_bed_play(NULL, 1, 1, false);        bwa_bed_set_gain(NULL, 1, 1.f);
    bwa_bed_set_orientation(NULL, 1, 0, 0, 0); bwa_bed_stop(NULL, 1);
    bwa_bed_destroy(NULL, 1);               bwa_bed_fade_to(NULL, 1, 1.f, 1.f);
    bwa_bed_fade_out(NULL, 1, 1.f);         bwa_bed_set_paused(NULL, 1, true);
    bwa_bed_seek(NULL, 1, 0);               bwa_bed_set_priority(NULL, 1, 1);
    bwa_bed_set_group(NULL, 1, 0);          bwa_unload_sound(NULL, 1);
    bwa_material_release(NULL, 1);
    bwa_scene_set_mesh_mat(NULL, NULL, 0, NULL, 0, NULL);
    bwa_scene_set_box(NULL, 4, 3, 5, NULL); bwa_scene_set_ism_room(NULL, 4, 3, 5, NULL);
    bwa_scene_set_ground(NULL, 0.f, 0, false); bwa_scene_set_pressure_release(NULL, 8);
    bwa_scene_set_dynamic_transform(NULL, 0, 0, 0, 0, 0, 0, 0, 1);
    bwa_scene_remove_dynamic_mesh(NULL, 0);
    bwa_source_set_early_reflections(NULL, 1, true); bwa_set_early_reflections_gain(NULL, 1.f);
    bwa_source_set_occlusion(NULL, 1, true);
    bwa_source_set_occlusion_manual(NULL, 1, 0.5f, NULL);
    bwa_reflections_config(NULL, NULL);     bwa_set_reverb_gain(NULL, 1.f);
    bwa_fdn_config(NULL, NULL);             bwa_fdn_set_decay(NULL, 1.f, 1.f, 1000.f);
    bwa_source_set_reverb(NULL, 1, true);   bwa_source_set_reverb_send(NULL, 1, 1.f);
    bwa_source_set_reverb_distance(NULL, 1, true); bwa_source_set_pathing(NULL, 1, true);
    bwa_source_set_orientation(NULL, 1, 0, 0, 0, 1);
    bwa_source_set_directivity(NULL, 1, 0.5f, 1.f);
    bwa_source_set_directivity_preset(NULL, 1, BWA_DIR_CARDIOID);
    bwa_source_set_doppler(NULL, 1, true);  bwa_source_set_air_absorption(NULL, 1, true);
    bwa_source_set_loudness_comp(NULL, 1, true); bwa_source_set_proximity(NULL, 1, true);
    bwa_set_speed_of_sound(NULL, 343.f);
    bwa_source_set_attenuation_override(NULL, 1, 1.f, 1.f, 0.f);
    bwa_source_set_spread(NULL, 1, 0.5f);   bwa_source_set_extent(NULL, 1, 0.5f, 0.5f);
    bwa_source_set_size(NULL, 1, 1.f);      bwa_set_test_signal(NULL, 0, BWA_TEST_SINE, 0.1f);
    bwa_set_panner(NULL, BWA_PAN_DBAP);     bwa_set_spcap_focus(NULL, 0, 0);
    bwa_set_dual_band(NULL, true);          bwa_set_dual_band_cap(NULL, true);
    bwa_set_max_re(NULL, true);             bwa_set_max_re_split(NULL, true);
    bwa_set_spread_mode(NULL, BWA_SPREAD_MDAP); bwa_set_decorrelation(NULL, true);
    bwa_set_near_spread(NULL, 1.f);         bwa_set_hole_spread(NULL, 1.f);
    bwa_set_limiter(NULL, false);           bwa_set_limiter_ceiling(NULL, 0.5f);
    bwa_set_headphone_eq(NULL, true);       bwa_set_bed_renderer(NULL, BWA_BED_PARAMETRIC);
    bwa_set_tracked_room_eq(NULL, false);   bwa_set_tracked_align(NULL, true);
    bwa_set_tracked_align_guards(NULL, 0.1f, 100.f); bwa_set_pose_prediction(NULL, 0.05f);
    bwa_set_extra_listeners(NULL, f3, 1);
    bwa_set_listener_pose(NULL, 0, 0, 0, 0, 0, 0, 1);
    bwa_get_listener_pose(NULL, f3, q4);
    bwa_set_output_capture(NULL, NULL, NULL);
    bwa_tracker_disconnect(NULL);
    bwa_commit(NULL);

    /* engine-free pure calls: NULL/zero/degenerate arguments must answer 0/false, never crash */
    {
        float pos[26 * 3] = { 0 }, lis[3] = { 0 }, srcs[3] = { 0, 1, 2 }, out[26];
        for (int i = 0; i < 26; ++i) { pos[i * 3] = (float)(i % 5); pos[i * 3 + 1] = 1.f + (float)(i % 3); pos[i * 3 + 2] = (float)(i % 7); }
        CHECK(bwa_spcap_focus_default(NULL, 26) == 0.f, "spcap default: NULL positions reads 0");
        CHECK(bwa_spcap_focus_default(pos, 1) == 0.f, "spcap default: n < 2 reads 0");
        CHECK(bwa_spcap_focus_default(pos, 27) == 0.f, "spcap default: n > capacity reads 0");
        CHECK(bwa_panner_gains_batch(BWA_PAN_DBAP, NULL, 26, lis, srcs, 1, 0, 0, out) == 0,
              "panner batch: NULL positions returns 0");
        CHECK(bwa_panner_gains_batch(BWA_PAN_DBAP, pos, 0, lis, srcs, 1, 0, 0, out) == 0,
              "panner batch: n = 0 returns 0");
        CHECK(bwa_panner_gains_batch(BWA_PAN_DBAP, pos, 27, lis, srcs, 1, 0, 0, out) == 0,
              "panner batch: n > capacity returns 0");
        CHECK(bwa_panner_gains_batch(BWA_PAN_DBAP, pos, 26, NULL, srcs, 1, 0, 0, out) == 0,
              "panner batch: NULL listener returns 0");
        CHECK(bwa_panner_gains_batch(BWA_PAN_DBAP, pos, 26, lis, NULL, 1, 0, 0, out) == 0,
              "panner batch: NULL sources returns 0");
        CHECK(bwa_panner_gains_batch(BWA_PAN_DBAP, pos, 26, lis, srcs, 0, 0, 0, out) == 0,
              "panner batch: nsrc = 0 returns 0");
        CHECK(bwa_panner_gains_batch(BWA_PAN_DBAP, pos, 26, lis, srcs, 1, 0, 0, NULL) == 0,
              "panner batch: NULL out returns 0");
        CHECK(bwa_bed_gains_batch(BWA_DECODE_ALLRAD, false, NULL, 26, srcs, 1, out) == 0,
              "bed batch: NULL positions returns 0");
        CHECK(bwa_bed_gains_batch(BWA_DECODE_ALLRAD, false, pos, 26, NULL, 1, out) == 0,
              "bed batch: NULL directions returns 0");
        CHECK(bwa_bed_gains_batch(BWA_DECODE_ALLRAD, false, pos, 26, srcs, 0, out) == 0,
              "bed batch: ndir = 0 returns 0");
    }
    {
        float v[24]; int tr[36]; bwa_material m[12];
        CHECK(!bwa_box_mesh(4, 3, 5, NULL, NULL, tr, m), "box mesh: NULL out_verts refused");
        CHECK(!bwa_box_mesh(4, 3, 5, NULL, v, NULL, m), "box mesh: NULL out_tris refused");
        CHECK(!bwa_box_mesh(0, 3, 5, NULL, v, tr, m), "box mesh: zero width refused");
        CHECK(!bwa_box_mesh(4, -3, 5, NULL, v, tr, m), "box mesh: negative height refused");
        CHECK(!bwa_box_mesh(4, 3, NAN, NULL, v, tr, m), "box mesh: NaN depth refused");
        CHECK(bwa_box_mesh(4, 3, 5, NULL, v, tr, NULL), "box mesh: NULL tri_material is allowed");
    }
    {
        uint32_t nd = bwa_get_asio_driver_count();
        CHECK(!bwa_get_asio_driver_name(nd, nm, sizeof nm), "driver name: index == count refused");
        CHECK(!bwa_get_asio_driver_name(0, NULL, 16), "driver name: NULL buffer refused");
        CHECK(!bwa_get_asio_driver_name(0, nm, 0), "driver name: zero cap refused");
    }
}

/* ---- 2. with-engine NULL/zero arguments -------------------------------------------------------- */
static void with_engine_null_zero(void) {
    bwa_desc d = manual_desc();
    bwa_engine* e = bwa_create(&d);
    CHECK(e != NULL, "engine create");
    if (!e) return;

    /* count-query and zero-cap conventions */
    CHECK(bwa_get_speakers(e, NULL, 0) == bwa_get_channel_count(e),
          "get_speakers(NULL) must answer the total count");
    { float xyz[3]; CHECK(bwa_get_speakers(e, xyz, 0) == 0, "get_speakers cap 0 fills nothing"); }
    { float pk[4];  CHECK(bwa_get_bus_levels(e, NULL, 4) == 0, "bus levels: NULL out reads 0");
                    CHECK(bwa_get_bus_levels(e, pk, 0) == 0, "bus levels: cap 0 fills nothing"); }

    /* NULL paths into the loaders: 0 + a reason, engine unharmed */
    CHECK(bwa_load_sound(e, NULL) == 0 && bwa_last_error(e) != NULL,
          "load_sound(NULL path) must fail with a reason");
    CHECK(bwa_load_sound(e, "") == 0 && bwa_last_error(e) != NULL,
          "load_sound(empty path) must fail with a reason");
    CHECK(bwa_load_ambix(e, NULL) == 0 && bwa_last_error(e) != NULL,
          "load_ambix(NULL path) must fail with a reason");
    CHECK(bwa_load_sound_streaming(e, NULL) == 0 && bwa_last_error(e) != NULL,
          "load_sound_streaming(NULL path) must fail with a reason");

    /* NULL coefficient arrays into a material: 0 + a reason */
    { float a[3] = { 0.1f, 0.1f, 0.1f };
      CHECK(bwa_material_define(e, NULL, 0.5f, a) == 0 && bwa_last_error(e) != NULL,
            "material define with NULL absorption must fail with a reason");
      CHECK(bwa_material_define(e, a, 0.5f, NULL) == 0 && bwa_last_error(e) != NULL,
            "material define with NULL transmission must fail with a reason"); }

    /* NULL out params where the header says they are optional */
    bwa_get_listener_pose(e, NULL, NULL);              /* documented guard: no crash */
    { bwa_source out[2];
      CHECK(bwa_poll_ended(e, NULL, 4, NULL) == 0, "poll_ended NULL out reads 0");
      CHECK(bwa_poll_ended(e, out, 0, NULL) == 0, "poll_ended cap 0 reads 0"); }
    { bwa_health h; CHECK(!bwa_get_health(e, NULL), "health with NULL out must report false");
      /* manual sink: no deadline, so health is documented UNMEASURABLE — false, out zeroed */
      memset(&h, 0xAA, sizeof h);
      CHECK(!bwa_get_health(e, &h) && h.blocks == 0,
            "manual sink health must be false with a zeroed out"); }

    /* extra listeners: NULL xyz / zero count must not crash (count 0 = single-listener) */
    bwa_set_extra_listeners(e, NULL, 3);
    bwa_set_extra_listeners(e, NULL, 0);

    /* zero-length push into a real push source, and NULL frames */
    { bwa_source p = bwa_source_create_push(e);
      CHECK(p != 0, "push source create");
      float blk[8] = { 0 };
      CHECK(bwa_source_push(e, p, blk, 0) == 0, "push of 0 frames accepts 0");
      CHECK(bwa_source_push(e, p, NULL, 8) == 0, "push of NULL frames accepts 0");
      bwa_source_destroy(e, p); }

    bwa_destroy(e);
}

/* ---- 3. call order ----------------------------------------------------------------------------- */
static void call_order(void) {
    bwa_desc d = manual_desc();
    bwa_engine* e = bwa_create(&d);
    CHECK(e != NULL, "engine create");
    if (!e) return;

    /* per-frame calls BEFORE start: all enqueue-only, must be safe with no audio thread */
    bwa_source s = bwa_source_create(e);
    bwa_source_set_pos(e, s, 1, 1, 1);
    bwa_source_set_gain(e, s, 0.5f);
    bwa_set_listener_pose(e, 0, 1.5f, 0, 0, 0, 0, 1);
    bwa_commit(e);
    bwa_commit(e);                                     /* commit with nothing new: harmless */

    /* render before start: documented NULL + a reason */
    CHECK(bwa_render_block(e, NULL, NULL) == NULL && bwa_last_error(e) != NULL,
          "render_block before start must be NULL with a reason");

    /* fdn_set_decay pre-start stages silently (documented "call it unconditionally") */
    clr(e);
    bwa_fdn_set_decay(e, 0.5f, 0.3f, 1500.f);
    CHECK(bwa_last_error(e) == NULL, "pre-start fdn_set_decay must stage without an error");
    /* NULL config pointers: no-ops */
    bwa_reflections_config(e, NULL);
    bwa_fdn_config(e, NULL);

    CHECK(bwa_start(e) == BWA_OK, "manual start: %s", bwa_last_error(e));
    CHECK(bwa_start(e) == BWA_OK, "a redundant bwa_start is a no-op returning BWA_OK");
    CHECK(bwa_get_sink_type(e) == BWA_SINK_MANUAL, "resolved sink type must be MANUAL");

    /* render with NULL out params (documented optional) */
    CHECK(bwa_render_block(e, NULL, NULL) != NULL, "render_block out params are optional");

    /* create-time-only config AFTER start: documented reject via bwa_last_error */
    clr(e);
    { bwa_reflections_desc rc = { .enabled = 1 };
      bwa_reflections_config(e, &rc);
      CHECK(bwa_last_error(e) && strstr(bwa_last_error(e), "load-time"),
            "post-start reflections_config must reject as load-time only"); }
    clr(e);
    { bwa_fdn_desc fc = { .enabled = 1 };
      bwa_fdn_config(e, &fc);
      CHECK(bwa_last_error(e) && strstr(bwa_last_error(e), "load-time"),
            "post-start fdn_config must reject as load-time only"); }
    /* live decay retune with NO FDN bed: documented reject */
    clr(e);
    bwa_fdn_set_decay(e, 1.f, 1.f, 1000.f);
    CHECK(bwa_last_error(e) && strstr(bwa_last_error(e), "FDN"),
          "post-start fdn_set_decay without an FDN must name the missing bed");

    /* stop, render after stop, restart, destroy while started */
    CHECK(bwa_stop(e) == BWA_OK, "stop");
    CHECK(bwa_stop(e) == BWA_OK, "second stop is a no-op");
    CHECK(bwa_render_block(e, NULL, NULL) == NULL && bwa_last_error(e) != NULL,
          "render_block after stop must be NULL with a reason");
    CHECK(bwa_start(e) == BWA_OK, "restart after stop");
    CHECK(render_blocks(e, 2) == 1, "render after restart");
    bwa_destroy(e);                                    /* destroy without stop must be safe */

    /* a FAILED start (explicit layout that does not exist) leaves the engine usable, repeatably */
    { bwa_desc bad = manual_desc();
      bad.layout_path = "bwa_abuse_no_such_layout.json";
      bwa_engine* f = bwa_create(&bad);
      CHECK(f != NULL, "create must survive a failed explicit layout load");
      if (f) {
          CHECK(bwa_last_error(f) != NULL, "the layout failure must be readable after create");
          CHECK(bwa_start(f) == BWA_ERR_LAYOUT, "start #1 must fail BWA_ERR_LAYOUT");
          CHECK(bwa_start(f) == BWA_ERR_LAYOUT, "start #2 must fail BWA_ERR_LAYOUT again");
          /* after the failed start the engine is still a working control surface */
          bwa_source fs = bwa_source_create(f);
          CHECK(fs != 0, "source create after a failed start");
          bwa_source_set_pos(f, fs, 1, 1, 1);
          bwa_commit(f);
          CHECK(bwa_render_block(f, NULL, NULL) == NULL, "render on a never-started engine is NULL");
          uint32_t snd = bwa_load_sound(f, WAV);
          CHECK(snd != 0, "asset load after a failed start");
          bwa_destroy(f);
      } }

    /* render_block on a NON-manual sink: documented NULL + a reason naming MANUAL */
    { bwa_desc nd = manual_desc();
      nd.sink = BWA_SINK_NULL;
      bwa_engine* n = bwa_create(&nd);
      CHECK(n && bwa_start(n) == BWA_OK, "null-sink start");
      if (n) {
          clr(n);
          CHECK(bwa_render_block(n, NULL, NULL) == NULL, "render_block on a null sink is NULL");
          CHECK(bwa_last_error(n) && strstr(bwa_last_error(n), "MANUAL"),
                "the render_block rejection must name the MANUAL sink");
          bwa_destroy(n);
      } }
}

/* ---- 4. handle lifetime ------------------------------------------------------------------------ */
static void handle_lifetime(void) {
    bwa_desc d = manual_desc();
    bwa_engine* e = bwa_create(&d);
    CHECK(e && bwa_start(e) == BWA_OK, "engine start");
    if (!e) return;
    uint32_t snd = bwa_load_sound(e, WAV);
    CHECK(snd != 0, "fixture load: %s", bwa_last_error(e));

    /* handle 0 is always invalid: silent no-op everywhere, inert getters */
    clr(e);
    bwa_source_play(e, 0, snd, true);
    bwa_source_stop(e, 0);
    bwa_source_destroy(e, 0);
    bwa_source_set_pos(e, 0, 1, 1, 1);
    bwa_source_set_gain(e, 0, 1.f);
    bwa_source_seek(e, 0, 100);
    CHECK(!bwa_source_is_playing(e, 0), "handle 0 never reads playing");
    CHECK(bwa_source_get_playhead_frames(e, 0) == 0, "handle 0 playhead reads 0");
    CHECK(bwa_source_get_occlusion(e, 0) == 1.0f, "handle 0 occlusion reads clear");
    CHECK(bwa_source_push(e, 0, NULL, 0) == 0 && bwa_source_push_space(e, 0) == 0,
          "handle 0 push accepts nothing");
    CHECK(render_blocks(e, 2) == 1, "engine renders after the handle-0 sweep");

    /* sound handle 0 / stale sound handles */
    CHECK(bwa_sound_get_frames(e, 0) == 0 && bwa_sound_get_channels(e, 0) == 0,
          "sound handle 0 metadata reads 0");
    clr(e);
    CHECK(!bwa_play_oneshot(e, 0, 0, 1, 0, 1.f) && bwa_last_error(e) != NULL,
          "a oneshot on sound 0 must be refused WITH a reason");
    bwa_unload_sound(e, 0);                              /* documented safe any time */
    bwa_unload_sound(e, 0x7FFF0001u);                    /* garbage sound handle: no-op */

    /* destroyed source handle, then a recycled slot: the stale handle must not act on the
     * NEW occupant — this is exactly what the generation count exists for */
    bwa_source s1 = bwa_source_create(e);
    CHECK(s1 != 0, "s1 create");
    bwa_source_play(e, s1, snd, true);
    bwa_commit(e);
    CHECK(render_blocks(e, 2) == 1, "s1 renders");
    CHECK(bwa_source_is_playing(e, s1), "s1 playing");
    bwa_source_destroy(e, s1);
    bwa_source_destroy(e, s1);                           /* double-destroy: documented harmless */
    CHECK(render_blocks(e, 2) == 1, "renders across the destroy");
    CHECK(!bwa_source_is_playing(e, s1), "a destroyed handle reads not-playing");

    bwa_source s2 = bwa_source_create(e);                /* very likely reuses s1's slot */
    CHECK(s2 != 0 && s2 != s1, "the recycled slot must carry a NEW generation");
    clr(e);
    bwa_source_play(e, s1, snd, true);                   /* stale play: silent no-op */
    bwa_source_set_gain(e, s1, 0.1f);
    bwa_source_stop(e, s1);
    bwa_source_fade_out(e, s1, 0.1f);
    bwa_commit(e);
    CHECK(render_blocks(e, 4) == 1, "renders across the stale-handle traffic");
    CHECK(bwa_last_error(e) == NULL, "stale-handle per-frame calls must stay SILENT no-ops");
    CHECK(!bwa_source_is_playing(e, s2),
          "a play on the STALE handle must not start the slot's new occupant");
    CHECK(bwa_get_active_voices(e) == 0, "no voice may be running after stale-only plays");

    /* bogus generation (handle ^ 0x10000): same slot, wrong gen — every call a no-op, and a
     * destroy with the wrong gen must NOT free the live handle's slot */
    bwa_source bogus = s2 ^ 0x10000u;
    clr(e);
    bwa_source_play(e, bogus, snd, true);
    bwa_source_destroy(e, bogus);
    bwa_commit(e);
    CHECK(render_blocks(e, 2) == 1, "renders across the bogus-generation traffic");
    CHECK(!bwa_source_is_playing(e, bogus), "a bogus-generation handle never reads playing");
    CHECK(bwa_source_get_playhead_frames(e, bogus) == 0, "bogus-generation playhead reads 0");
    bwa_source_play(e, s2, snd, true);                   /* s2 must still be LIVE after the fake destroy */
    bwa_commit(e);
    CHECK(render_blocks(e, 2) == 1, "renders after the real play");
    CHECK(bwa_source_is_playing(e, s2),
          "destroying a bogus-generation twin must not have freed the live handle");
    bwa_source_stop(e, s2);

    /* cross-engine handles: a handle minted by A used on a fresh B (no sources) is stale there */
    { bwa_desc bd = manual_desc();
      bwa_engine* b = bwa_create(&bd);
      CHECK(b && bwa_start(b) == BWA_OK, "engine B start");
      if (b) {
          clr(b);
          bwa_source_play(b, s2, 1, true);               /* A's live handle, B's empty tables */
          bwa_source_set_pos(b, s2, 1, 1, 1);
          bwa_source_destroy(b, s2);
          bwa_commit(b);
          CHECK(render_blocks(b, 2) == 1, "B renders across A's handles");
          CHECK(!bwa_source_is_playing(b, s2), "A's handle reads not-playing on B");
          CHECK(bwa_get_active_voices(b) == 0, "A's handles must not start anything on B");
          /* A's SOUND handle on B: refused with a reason (documented oneshot behavior) */
          CHECK(!bwa_play_oneshot(b, snd, 0, 1, 0, 1.f) && bwa_last_error(b) != NULL,
                "a foreign sound handle must be refused with a reason");
          bwa_destroy(b);
      } }

    /* using engine A's handles after destroying a DIFFERENT engine must be untouched by it */
    bwa_source_play(e, s2, snd, true);
    bwa_commit(e);
    CHECK(render_blocks(e, 2) == 1 && bwa_source_is_playing(e, s2),
          "engine A must be unaffected by engine B's destruction");

    bwa_destroy(e);
}

/* ---- 5. asset-kind mismatches ------------------------------------------------------------------ */
static void kind_mismatch(void) {
    bwa_desc d = manual_desc();
    bwa_engine* e = bwa_create(&d);
    CHECK(e && bwa_start(e) == BWA_OK, "engine start");
    if (!e) return;
    uint32_t mono = bwa_load_sound(e, WAV);
    uint32_t ambi = bwa_load_ambix(e, QUAD);
    uint32_t strm = bwa_load_sound_streaming(e, WAV);
    CHECK(mono != 0, "mono load: %s", bwa_last_error(e));
    CHECK(ambi != 0 && bwa_sound_get_channels(e, ambi) == 4, "ambix load: %s", bwa_last_error(e));
    CHECK(strm != 0, "stream load: %s", bwa_last_error(e));

    bwa_source s = bwa_source_create(e);
    bwa_source p = bwa_source_create_push(e);
    bwa_bed    b = bwa_bed_create(e);
    CHECK(s && p && b, "source/push/bed create");

    /* a multichannel asset on a plain source: documented reject + no voice starts */
    clr(e);
    bwa_source_play(e, s, ambi, false);
    CHECK(bwa_last_error(e) != NULL, "a bed asset on a plain source must be reported");
    bwa_commit(e);
    CHECK(render_blocks(e, 2) == 1 && !bwa_source_is_playing(e, s),
          "the mismatched play must not have started a voice");
    clr(e);
    bwa_source_play_at(e, s, ambi, false, 0);
    CHECK(bwa_last_error(e) != NULL, "play_at with a bed asset must be reported");
    clr(e);
    CHECK(!bwa_play_oneshot(e, ambi, 0, 1, 0, 1.f) && bwa_last_error(e) != NULL,
          "a oneshot on a bed asset must be refused with a reason");

    /* a mono asset on a bed: documented reject */
    clr(e);
    bwa_bed_play(e, b, mono, true);
    CHECK(bwa_last_error(e) != NULL, "a mono asset on a bed must be reported");
    CHECK(!bwa_bed_is_playing(e, b), "the mismatched bed play must not have started");
    /* and the right pairing still works after all the rejections */
    clr(e);
    bwa_bed_play(e, b, ambi, true);
    CHECK(bwa_last_error(e) == NULL, "a correct bed play must not report");
    bwa_commit(e);
    CHECK(render_blocks(e, 2) == 1 && bwa_bed_is_playing(e, b), "the bed actually plays");
    bwa_bed_stop(e, b);

    /* the queue is documented in-memory mono only: bed assets and streams are rejected */
    bwa_source_play(e, s, mono, false);
    clr(e);
    bwa_source_queue(e, s, ambi, false);
    CHECK(bwa_last_error(e) != NULL, "queueing a bed asset must be reported");
    clr(e);
    bwa_source_queue(e, s, strm, false);
    CHECK(bwa_last_error(e) != NULL, "queueing a streamed asset must be reported");
    bwa_source_stop(e, s);

    /* push calls on a plain source: documented reject WITH a reason (else it reads as
     * backpressure); a play on a push source is likewise refused with a reason */
    { float blk[16] = { 0 };
      clr(e);
      CHECK(bwa_source_push(e, s, blk, 16) == 0 && bwa_last_error(e) != NULL,
            "push on a plain source must accept nothing AND report");
      CHECK(bwa_source_push_space(e, s) == 0, "push_space on a plain source reads 0");
      clr(e);
      bwa_source_play(e, p, mono, false);
      CHECK(bwa_last_error(e) != NULL, "play on a push source must be reported");
      clr(e);
      bwa_source_play_loop(e, p, mono, 0, 0);
      CHECK(bwa_last_error(e) != NULL, "play_loop on a push source must be reported");
      clr(e);
      bwa_bed_play(e, p, ambi, false);
      CHECK(bwa_last_error(e) != NULL, "bed_play on a push source must be reported"); }

    /* pitch/seek on a streamed voice are documented as ignored: nothing to assert but survival */
    bwa_source st = bwa_source_create(e);
    bwa_source_play(e, st, strm, false);
    bwa_source_set_pitch(e, st, 2.f);
    bwa_source_seek(e, st, 4096);
    bwa_commit(e);
    CHECK(render_blocks(e, 3) == 1, "streamed voice survives pitch/seek");

    bwa_destroy(e);
}

/* ---- 6. non-finite and boundary arguments ------------------------------------------------------
 * The engine's own convention (rt.c) is "keep NaN/Inf off the audio thread": the source setters
 * reject non-finite input at the ABI edge. These probes feed a non-finite value into every float
 * parameter and assert the device-bound output stays finite — one throwaway engine per poison, so
 * a value that DOES stick (NaN gain state never recovers: x + (t-x)*k stays NaN) cannot mask the
 * next probe. */
typedef void (*poison_fn)(bwa_engine* e, bwa_source s);
static void nan_probe_settle(const char* what, poison_fn poison, int settle_ms) {
    bwa_desc d = manual_desc();
    bwa_engine* e = bwa_create(&d);
    if (!e) { CHECK(0, "%s: probe engine create failed", what); return; }
    uint32_t snd = bwa_load_sound(e, WAV);
    bwa_source s = bwa_source_create(e);
    bwa_source_play(e, s, snd, true);
    bwa_source_set_pos(e, s, 1.f, 1.5f, 0.f);
    bwa_set_listener_pose(e, 0, 1.5f, 0, 0, 0, 0, 1);
    bwa_commit(e);
    if (bwa_start(e) != BWA_OK) { CHECK(0, "%s: probe start failed", what); bwa_destroy(e); return; }
    /* Verify the probe is actually PROBING. If the load or the play were ever refused, every one of
     * these would pass vacuously on silence, which is the classic way a poison suite goes green. */
    if (!snd) { CHECK(0, "%s: probe sound failed to load", what); bwa_destroy(e); return; }
    if (!bwa_source_is_playing(e, s)) { CHECK(0, "%s: probe voice is not playing", what); bwa_destroy(e); return; }
    render_blocks(e, 4);                                 /* ramp the voice in first */
    {   /* and that it is audible, so "finite" is not just "silent" */
        uint32_t ch = 0, nf = 0; const float* p = bwa_render_block(e, &ch, &nf);
        double pk = 0; if (p) for (uint32_t i = 0; i < ch * nf; ++i) { double a = fabs(p[i]); if (a > pk) pk = a; }
        if (!(pk > 1e-6)) { CHECK(0, "%s: probe voice is silent before the poison", what); bwa_destroy(e); return; }
    }
    poison(e, s);
    if (settle_ms) Sleep((DWORD)settle_ms);              /* async publishers (the Steam sim thread)
                                                          * need a tick to land their value */
    bwa_commit(e);
    int r = render_blocks(e, 8);
    CHECK(r == 1, "%s must not push non-finite samples to the device (render=%d)", what, r);
    bwa_destroy(e);
}
static void nan_probe(const char* what, poison_fn poison) { nan_probe_settle(what, poison, 0); }
static void poison_master_nan(bwa_engine* e, bwa_source s)  { (void)s; bwa_set_master_gain(e, NAN); }
static void poison_master_inf(bwa_engine* e, bwa_source s)  { (void)s; bwa_set_master_gain(e, INFINITY); }
static void poison_group_nan(bwa_engine* e, bwa_source s)   { (void)s; bwa_group_set_gain(e, 0, NAN); }
static void poison_fade_nan_gain(bwa_engine* e, bwa_source s) { bwa_source_fade_to(e, s, NAN, 0.05f); }
static void poison_fade_nan_secs(bwa_engine* e, bwa_source s) { bwa_source_fade_to(e, s, 0.5f, NAN); }
static void poison_pitch_nan(bwa_engine* e, bwa_source s)   { bwa_source_set_pitch(e, s, NAN); }
static void poison_size_nan(bwa_engine* e, bwa_source s)    { bwa_source_set_size(e, s, NAN); }
static void poison_test_nan(bwa_engine* e, bwa_source s)    { (void)s; bwa_set_test_signal(e, 0, BWA_TEST_SINE, NAN); }
static void poison_listener_nan(bwa_engine* e, bwa_source s){ (void)s; bwa_set_listener_pose(e, NAN, NAN, NAN, 0, 0, 0, 1); }
static void poison_extra_lis_nan(bwa_engine* e, bwa_source s){ (void)s;
    float xyz[3] = { NAN, NAN, NAN }; bwa_set_extra_listeners(e, xyz, 1); }
/* INF, not NaN, is the one that bit: dbap_gains computes inv = 1/Inf = 0 and then -Inf * 0 = NaN,
 * which survives both cosang clamp branches. A NaN extra listener happened to be absorbed, so the
 * NaN-only probe above was green by numerical luck rather than by a guard. */
static void poison_extra_lis_inf(bwa_engine* e, bwa_source s){ (void)s;
    float xyz[3] = { INFINITY, 1.5f, 0.f }; bwa_set_extra_listeners(e, xyz, 1); }
/* FINITE-BUT-ABSURD is its own class, and the isfinite-reject family does not cover it: 3e38 passes
 * every isfinite() guard, then `bus * 3e38` OVERFLOWS to Inf. These gains are sticky, so every
 * later block overflows as well, and the Inf reaches the align delay line and the room-EQ biquads,
 * whose IIR state holds it past any later correction. The fuzzer found this on master gain (seed
 * 12648430); every linear-gain setter that scales the bus shares it, so they are all capped at
 * BWA_MAX_GAIN and probed together here. */
static void poison_absurd_gain(bwa_engine* e, bwa_source s) {
    bwa_set_master_gain(e, 3.0e38f);
    bwa_source_set_gain(e, s, 3.0e38f);
    bwa_source_set_reverb_send(e, s, 3.0e38f);
    bwa_set_test_signal(e, 0, 1, 3.0e38f);
}
/* the reject-class setters all at once: each is documented (or coded at the rt edge) to refuse
 * non-finite input, so ONE probe covers them and is expected green */
static void poison_reject_class(bwa_engine* e, bwa_source s) {
    bwa_source_set_pos(e, s, NAN, INFINITY, -INFINITY);
    bwa_source_set_gain(e, s, NAN);
    bwa_source_set_gain(e, s, -1.f);
    bwa_source_set_spread(e, s, NAN);
    bwa_source_set_extent(e, s, NAN, INFINITY);
    bwa_source_set_attenuation_override(e, s, NAN, NAN, NAN);
    bwa_source_set_reverb_send(e, s, NAN);
    bwa_set_speed_of_sound(e, NAN);
    bwa_set_speed_of_sound(e, 0.f);          /* clamps to the floor, never divides by zero */
    bwa_set_speed_of_sound(e, 1e9f);
    bwa_set_limiter_ceiling(e, NAN);         /* documented: non-positive/NaN ignored */
    bwa_set_limiter_ceiling(e, 0.f);
    bwa_set_limiter_ceiling(e, -1.f);
    bwa_set_limiter_ceiling(e, 2.f);         /* documented: clamps to 1 */
    bwa_set_spcap_focus(e, NAN, NAN);        /* <= 0 sentinel catches NaN */
    bwa_bed_set_orientation(e, s, NAN, NAN, NAN);
}
/* the occlusion/directivity family is probed one call at a time: their clamps are the
 * `x < lo ... x > hi` pattern a NaN slips straight through, and (with the SDK) directivity and
 * orientation route through the Steam sim's own thread, so each needs its own engine and a
 * settle window to land */
static void poison_occl_level_nan(bwa_engine* e, bwa_source s) {
    bwa_source_set_occlusion_manual(e, s, NAN, NULL);
}
static void poison_occl_bands_nan(bwa_engine* e, bwa_source s) {
    float bands[3] = { NAN, NAN, NAN };
    bwa_source_set_occlusion_manual(e, s, 0.5f, bands);
}
static void poison_directivity_power_nan(bwa_engine* e, bwa_source s) {
    bwa_source_set_directivity(e, s, 0.5f, NAN);   /* a LIVE weight: a NaN weight would disable the
                                                    * pattern and mask whatever power does */
}
static void poison_directivity_nan(bwa_engine* e, bwa_source s) {
    bwa_source_set_directivity(e, s, NAN, NAN);
}
/* NOTE: do NOT follow a poison with a valid value in the same probe. This one used to reset the
 * orientation to a zero quat immediately after the NaN, which overwrote the shadow before the sim's
 * next tick, so the probe could not fail even though the hole was real. */
static void poison_orientation_nan(bwa_engine* e, bwa_source s) {
    bwa_source_set_directivity(e, s, 0.5f, 2.f);         /* a real pattern, then poison its axis */
    bwa_source_set_orientation(e, s, NAN, NAN, NAN, NAN);
}
static void poison_orientation_zero(bwa_engine* e, bwa_source s) {
    bwa_source_set_directivity(e, s, 0.5f, 2.f);
    bwa_source_set_orientation(e, s, 0, 0, 0, 0);        /* degenerate: must fall back to identity */
}
static void poison_orientation_huge(bwa_engine* e, bwa_source s) {
    bwa_source_set_directivity(e, s, 0.5f, 2.f);
    bwa_source_set_orientation(e, s, 1e6f, -1e6f, 1e6f, 1e6f);   /* finite but wildly un-normalized */
}
/* boundary, not non-finite: huge / denormal / negative values that must clamp, not wrap */
static void poison_boundary(bwa_engine* e, bwa_source s) {
    bwa_source_set_pos(e, s, 1e30f, -1e30f, 1e30f);      /* astronomically far: attenuates, never NaNs */
    bwa_source_set_gain(e, s, 1e30f);                    /* limiter guards the sum */
    bwa_source_set_gain(e, s, 1e-42f);                   /* denormal */
    bwa_source_set_pitch(e, s, 1e30f);                   /* clamps to 4 */
    bwa_source_set_pitch(e, s, -3.f);                    /* clamps to 0.25 */
    bwa_source_set_spread(e, s, 42.f);
    bwa_source_set_spread(e, s, -42.f);
    bwa_source_set_size(e, s, 1e30f);
    bwa_source_set_priority(e, s, -100);                 /* documented: out-of-range clamps */
    bwa_source_set_priority(e, s, 100000);
    bwa_source_set_group(e, s, 9999u);                   /* documented: falls back to group 0 */
    bwa_group_set_gain(e, 9999u, 0.f);                   /* documented: ignored */
    bwa_group_set_paused(e, 9999u, true);
    bwa_group_stop(e, 9999u);
    bwa_set_test_signal(e, 9999u, BWA_TEST_SINE, 0.5f);  /* out-of-range channel: ignored */
    bwa_set_panner(e, (bwa_panner)7);                    /* out-of-range enum: sanitized */
    bwa_set_spread_mode(e, (bwa_spread_mode)9);
    bwa_source_set_pos(e, s, 1.f, 1.5f, 0.f);            /* back to sane, still renders */
    bwa_source_set_gain(e, s, 0.5f);
    bwa_source_play_at(e, s, 0, false, ~0ull);           /* far-future schedule on a dead sound: no-op */
    bwa_source_stop_at(e, s, ~0ull);                     /* far-future stop: pending, harmless */
    bwa_source_seek(e, s, ~0ull);                        /* past-the-end seek: wraps or ends */
}

static void nonfinite_and_boundary(void) {
    nan_probe("NaN master gain", poison_master_nan);
    nan_probe("Inf master gain", poison_master_inf);
    nan_probe("NaN group gain", poison_group_nan);
    nan_probe("NaN fade target", poison_fade_nan_gain);
    nan_probe("NaN fade seconds", poison_fade_nan_secs);
    nan_probe("NaN pitch", poison_pitch_nan);
    nan_probe("NaN source size", poison_size_nan);
    nan_probe("NaN test-signal gain", poison_test_nan);
    nan_probe("NaN listener pose", poison_listener_nan);
    nan_probe("NaN extra listener", poison_extra_lis_nan);
    nan_probe("Inf extra listener", poison_extra_lis_inf);
    nan_probe("absurd (finite) gain", poison_absurd_gain);
    nan_probe("NaN directivity power (live weight)", poison_directivity_power_nan);
    nan_probe("zero orientation quaternion", poison_orientation_zero);
    nan_probe("huge un-normalized orientation quaternion", poison_orientation_huge);
    nan_probe("reject-class non-finite sweep", poison_reject_class);
    nan_probe("NaN manual-occlusion level", poison_occl_level_nan);
    nan_probe("NaN manual-occlusion bands", poison_occl_bands_nan);
    nan_probe_settle("NaN directivity weight/power", poison_directivity_nan, 150);
    nan_probe_settle("NaN orientation quaternion", poison_orientation_nan, 150);
    nan_probe("boundary sweep", poison_boundary);

    /* Inf group gain and absurd fade targets: the two members of the linear-gain family the
     * BWA_MAX_GAIN cap missed (group gain's `!(g > 0)` cleansed NaN but stored Inf verbatim; a
     * fade with seconds <= 0 is INSTANT, so its 3e38 target bypassed every ramp). The limiter is
     * turned OFF in both: its hard clamp turns "0-or-Inf" bus samples into "0-or-ceiling" — an
     * audible, finite signal that masked BOTH defects when these probes first ran against the
     * unfixed build. With it off, a leaked Inf/NaN reaches the returned block and render_peak
     * catches it; audibility still guards against a fix that silences the voice instead. */
    { bwa_desc d = manual_desc();
      bwa_engine* e = bwa_create(&d);
      CHECK(e != NULL, "group-gain probe create");
      if (e) {
          uint32_t snd = bwa_load_sound(e, WAV);
          bwa_source s = bwa_source_create(e);
          bwa_source_play(e, s, snd, true);
          bwa_source_set_pos(e, s, 1.f, 1.5f, 0.f);
          bwa_commit(e);
          CHECK(bwa_start(e) == BWA_OK && snd != 0 && bwa_source_is_playing(e, s),
                "group-gain probe voice live");
          bwa_set_limiter(e, false);
          double pk = 0;
          CHECK(render_peak(e, 4, &pk) == 1 && pk > 1e-6, "group-gain probe audible pre-poison");
          /* Inf ONLY: a follow-up absurd-finite set would drain in the same block and overwrite
           * the Inf before a render ever saw it (which is how this probe first went green against
           * the unfixed build). The absurd-FINITE class is the fade probe below, where 32 targets
           * sum past FLT_MAX. */
          bwa_group_set_gain(e, 0, INFINITY);
          bwa_commit(e);
          pk = 0;
          int r = render_peak(e, 8, &pk);
          CHECK(r == 1, "an Inf group gain must not push non-finite samples (render=%d)", r);
          CHECK(pk > 1e-6, "the voice must stay audible through an Inf group gain (peak %g)", pk);
          bwa_destroy(e);
      } }
    { bwa_desc d = manual_desc();
      bwa_engine* e = bwa_create(&d);
      CHECK(e != NULL, "fade-target probe create");
      if (e) {
          uint32_t snd = bwa_load_sound(e, WAV);
          /* enough voices that the 3e38 targets SUM past FLT_MAX on the bus: one voice alone tops
           * out near 2e37, comfortably finite, and proves nothing */
          enum { NF = 32 };
          bwa_source vs[NF];
          for (int i = 0; i < NF; ++i) {
              vs[i] = bwa_source_create(e);
              bwa_source_play(e, vs[i], snd, true);
              bwa_source_set_pos(e, vs[i], 1.f, 1.5f, 0.f);
          }
          bwa_commit(e);
          CHECK(bwa_start(e) == BWA_OK && snd != 0 && bwa_source_is_playing(e, vs[0]),
                "fade-target probe voices live");
          bwa_set_limiter(e, false);
          double pk = 0;
          CHECK(render_peak(e, 4, &pk) == 1 && pk > 1e-6, "fade-target probe audible pre-poison");
          for (int i = 0; i < NF; ++i) bwa_source_fade_to(e, vs[i], 3.0e38f, 0.f);
          bwa_commit(e);
          pk = 0;
          int r = render_peak(e, 8, &pk);
          CHECK(r == 1, "absurd instant fade targets must not push non-finite samples (render=%d)", r);
          CHECK(pk > 1e-6, "the voices must stay audible through absurd fade targets (peak %g)", pk);
          bwa_destroy(e);
      } }

    /* the listener-pose READBACK must never report a non-finite pose: the pose feeds every solve
     * and bwa_get_listener_pose is the pose "the engine is currently rendering with" */
    { bwa_desc d = manual_desc();
      bwa_engine* e = bwa_create(&d);
      CHECK(e && bwa_start(e) == BWA_OK, "pose probe start");
      if (e) {
          bwa_set_listener_pose(e, NAN, INFINITY, NAN, NAN, 0, 0, 1);
          bwa_commit(e);
          render_blocks(e, 2);
          float p[3] = { 0 }, q[4] = { 0 };
          bwa_get_listener_pose(e, p, q);
          CHECK(isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2]) &&
                isfinite(q[0]) && isfinite(q[1]) && isfinite(q[2]) && isfinite(q[3]),
                "a non-finite listener pose must not reach the pose readback");
          bwa_destroy(e);
      } }

    /* the tuning READBACK is documented to report SANITIZED values only ("must not itself report
     * an unsanitized value") — so NaN into the live-knob setters must never come back out of it */
    { bwa_desc d = manual_desc();
      bwa_engine* e = bwa_create(&d);
      CHECK(e != NULL, "tuning probe create");
      if (e) {
          bwa_set_near_spread(e, NAN);
          bwa_set_hole_spread(e, NAN);
          bwa_set_tracked_align_guards(e, NAN, NAN);
          bwa_set_spcap_focus(e, INFINITY, INFINITY);
          bwa_tuning t;
          CHECK(bwa_get_tuning(e, &t), "get_tuning");
          CHECK(isfinite(t.near_spread), "get_tuning must not report a non-finite near_spread");
          CHECK(isfinite(t.hole_spread), "get_tuning must not report a non-finite hole_spread");
          CHECK(isfinite(t.align_dead_zone_m) && isfinite(t.align_slew_frames_per_s),
                "get_tuning must not report non-finite align guards");
          CHECK(isfinite(t.spcap_focus) && isfinite(t.spcap_density),
                "get_tuning must not report non-finite SPCAP knobs");
          /* recovery: a later sane value must land regardless of what the NaN did */
          bwa_set_near_spread(e, 1.0f);
          bwa_set_hole_spread(e, 1.0f);
          CHECK(bwa_get_tuning(e, &t) && t.near_spread == 1.0f && t.hole_spread == 1.0f,
                "sane values must land after the NaN attempts");
          bwa_destroy(e);
      } }

    /* NaN into a oneshot's position/gain: documented refuse-with-reason */
    { bwa_desc d = manual_desc();
      bwa_engine* e = bwa_create(&d);
      if (e) {
          uint32_t snd = bwa_load_sound(e, WAV);
          clr(e);
          CHECK(!bwa_play_oneshot(e, snd, NAN, 0, 0, 1.f) && bwa_last_error(e) != NULL,
                "a NaN oneshot position must be refused with a reason");
          clr(e);
          CHECK(!bwa_play_oneshot(e, snd, 0, 1, 0, NAN) && bwa_last_error(e) != NULL,
                "a NaN oneshot gain must be refused with a reason");
          bwa_destroy(e);
      } }

    /* scene geometry dimensions: documented positive-only, refused with a reason */
    { bwa_desc d = manual_desc();
      bwa_engine* e = bwa_create(&d);
      if (e) {
          clr(e);
          bwa_scene_set_ism_room(e, 0.f, 3.f, 5.f, NULL);
          CHECK(bwa_last_error(e) != NULL, "a zero-width ISM room must be refused with a reason");
          clr(e);
          bwa_scene_set_box(e, 4.f, NAN, 5.f, NULL);
          CHECK(bwa_last_error(e) != NULL, "a NaN box height must be refused with a reason");
          clr(e);
          bwa_scene_set_box(e, -4.f, 3.f, 5.f, NULL);
          CHECK(bwa_last_error(e) != NULL, "a negative box width must be refused with a reason");
          clr(e);
          bwa_scene_set_ground(e, NAN, 0, false);
          CHECK(bwa_last_error(e) != NULL, "a NaN ground height must be refused with a reason");
          /* no valid room was ever set, so opting into early reflections must say so */
          clr(e);
          bwa_source s = bwa_source_create(e);
          bwa_source_set_early_reflections(e, s, true);
          CHECK(bwa_last_error(e) != NULL, "ISM enable without a room must name the missing room");
          clr(e);
          bwa_scene_set_pressure_release(e, 0xFFu);
          CHECK(bwa_last_error(e) != NULL, "pressure release without a room must be refused");
          /* and a good box after all the refusals still works end to end */
          clr(e);
          bwa_scene_set_box(e, 4.f, 3.f, 5.f, NULL);
          CHECK(bwa_last_error(e) == NULL, "a valid box after refusals must not report");
          bwa_source_set_early_reflections(e, s, true);
          CHECK(bwa_last_error(e) == NULL, "ISM enable with a room must not report");
          bwa_destroy(e);
      } }
}

/* ---- 7. capacity ------------------------------------------------------------------------------- */
static void capacity(void) {
    bwa_desc d = manual_desc();
    bwa_engine* e = bwa_create(&d);
    CHECK(e && bwa_start(e) == BWA_OK, "engine start");
    if (!e) return;
    uint32_t snd = bwa_load_sound(e, WAV);
    CHECK(snd != 0, "fixture load");

    /* voice pool: with every source PROTECTED (priority 255) the documented steal path has no
     * victim, so creation must eventually refuse instead of evicting a protected voice */
    enum { MAXSRC = 400 };
    static bwa_source hs[MAXSRC];
    int made = 0;
    for (int i = 0; i < MAXSRC; ++i) {
        bwa_source s = bwa_source_create(e);
        if (!s) break;
        bwa_source_set_priority(e, s, 255);
        hs[made++] = s;
    }
    CHECK(made >= 200 && made < MAXSRC, "pool should refuse after ~256 protected sources (made %d)", made);
    CHECK(bwa_source_create(e) == 0, "a fully protected pool must keep refusing");
    /* un-protect one: the next create must succeed by stealing it (the documented eviction) */
    bwa_source_set_priority(e, hs[3], 10);
    { bwa_source stolen_in = bwa_source_create(e);
      CHECK(stolen_in != 0, "an unprotected victim must make room via the steal");
      bwa_source_destroy(e, stolen_in); }
    /* release one normally: creation must recover without any render in between */
    bwa_source_destroy(e, hs[7]);
    { bwa_source again = bwa_source_create(e);
      CHECK(again != 0, "a destroy must return its slot to the pool");
      bwa_source_destroy(e, again); }
    for (int i = 0; i < made; ++i) if (i != 7) bwa_source_destroy(e, hs[i]);
    CHECK(render_blocks(e, 4) == 1, "renders after the pool churn");

    /* sound table: load the same file until the table refuses, with a reason; an unload plus the
     * retire-ack drain (render + commit) must free a slot for the next load */
    enum { MAXSND = 400 };
    static uint32_t sn[MAXSND];
    int loaded = 0;
    for (int i = 0; i < MAXSND; ++i) {
        uint32_t s2 = bwa_load_sound(e, WAV);
        if (!s2) break;
        sn[loaded++] = s2;
    }
    CHECK(loaded >= 200 && loaded < MAXSND, "sound table should refuse after ~256 loads (got %d)", loaded);
    CHECK(bwa_last_error(e) && strstr(bwa_last_error(e), "full"),
          "the refused load must say the table is full");
    bwa_unload_sound(e, sn[0]);
    { uint32_t re = 0;
      for (int tries = 0; tries < 20 && !re; ++tries) {   /* the retire-ack takes a render + drain */
          render_blocks(e, 1);
          bwa_commit(e);
          re = bwa_load_sound(e, WAV);
      }
      CHECK(re != 0, "an unload must free a table slot once the retire is acked");
      if (re) bwa_unload_sound(e, re); }
    for (int i = 1; i < loaded; ++i) bwa_unload_sound(e, sn[i]);
    for (int b = 0; b < 8; ++b) { render_blocks(e, 1); bwa_commit(e); }   /* drain the retires */

    /* command ring: far more per-frame traffic than one block drains. Position is documented
     * latest-wins with silent drops under backpressure, so the only contract is that the engine
     * keeps rendering and keeps accepting work afterwards. */
    { bwa_source s = bwa_source_create(e);
      CHECK(s != 0, "flood source");
      for (int i = 0; i < 20000; ++i)
          bwa_source_set_pos(e, s, (float)(i % 7), 1.5f, (float)(i % 5));
      bwa_commit(e);
      CHECK(render_blocks(e, 4) == 1, "renders through the command flood");
      bwa_source_play(e, s, snd, true);
      bwa_commit(e);
      CHECK(render_blocks(e, 2) == 1, "renders after the flood");
      CHECK(bwa_source_is_playing(e, s), "work issued after the flood must still land");
      bwa_source_stop(e, s); }

    /* push ring: a fixed 65536-frame ring — over-pushing must saturate exactly, then report no
     * space, then free up as the voice consumes */
    { bwa_source p = bwa_source_create_push(e);
      CHECK(p != 0, "push source create");
      static float blk[4096];
      uint32_t total = 0;
      for (int i = 0; i < 20; ++i) total += bwa_source_push(e, p, blk, 4096);
      CHECK(total == 65536u, "the push ring must accept exactly its capacity (got %u)", total);
      CHECK(bwa_source_push(e, p, blk, 16) == 0, "a full push ring accepts nothing");
      CHECK(bwa_source_push_space(e, p) == 0, "a full push ring reports no space");
      render_blocks(e, 1);
      CHECK(bwa_source_push_space(e, p) >= 256, "a rendered block frees ring space");
      bwa_source_destroy(e, p); }

    bwa_destroy(e);
}

/* ---- 8. the newest surfaces -------------------------------------------------------------------- */
static void newest_surfaces(void) {
    bwa_desc d = manual_desc();
    bwa_engine* e = bwa_create(&d);
    CHECK(e && bwa_start(e) == BWA_OK, "engine start");
    if (!e) return;
    uint32_t snd = bwa_load_sound(e, WAV);
    CHECK(snd != 0, "fixture load");

    /* poll_ended: cap 0 and NULL out must not DRAIN — the event has to survive for a real poll */
    { bwa_source s = bwa_source_create(e);
      bwa_source_play(e, s, snd, false);                 /* 512 frames: ends within 8 blocks */
      bwa_commit(e);
      render_blocks(e, 8);
      bwa_commit(e);                                     /* the drain poll_ended reads from */
      bwa_source out[4]; uint64_t dropped = 99;
      CHECK(bwa_poll_ended(e, out, 0, &dropped) == 0, "cap 0 reads nothing");
      CHECK(dropped == 0, "nothing may have been dropped here");
      CHECK(bwa_poll_ended(e, NULL, 4, NULL) == 0, "NULL out reads nothing");
      CHECK(bwa_poll_ended(e, out, 4, NULL) == 1 && out[0] == s,
            "the zero-cap and NULL-out polls must not have consumed the event");
      CHECK(bwa_poll_ended(e, out, 4, NULL) == 0, "a drained queue reads empty");
      bwa_source_destroy(e, s); }

    /* tuning: a zero-initialized struct MUST be refused (its zero is not its default), as must a
     * struct_size off by a word in either direction; get_tuning refills struct_size for reuse */
    { bwa_tuning t;
      memset(&t, 0, sizeof t);
      clr(e);
      CHECK(!bwa_apply_tuning(e, &t) && bwa_last_error(e) != NULL,
            "a zero-init bwa_tuning must be refused with a reason");
      bwa_tuning_preset(BWA_SETUP_ROAMING, &t);
      t.struct_size += 4;
      clr(e);
      CHECK(!bwa_apply_tuning(e, &t), "an oversized struct_size must be refused");
      t.struct_size -= 8;
      CHECK(!bwa_apply_tuning(e, &t), "an undersized struct_size must be refused");
      clr(e);
      CHECK(!bwa_apply_tuning(e, NULL) && bwa_last_error(e) != NULL,
            "a NULL tuning must be refused with a reason");
      bwa_tuning back;
      memset(&back, 0, sizeof back);
      CHECK(bwa_get_tuning(e, &back) && back.struct_size == (uint32_t)sizeof(bwa_tuning),
            "get_tuning must fill struct_size so the result can be fed straight back");
      CHECK(bwa_apply_tuning(e, &back), "the get_tuning result must round-trip into apply"); }

    /* scene_set_mesh_mat: the deliberate all-NULL CLEAR versus malformed PARTIAL meshes. The
     * header distinguishes them (clear = both pointers NULL AND both counts <= 0); a partial mesh
     * must be dropped intact, never adopted or crashed on. */
    { float v[24]; int tr[36]; bwa_material m12[12];
      CHECK(bwa_box_mesh(4, 3, 5, NULL, v, tr, m12), "box mesh for the mesh_mat probes");
      bwa_scene_set_mesh_mat(e, v, 8, tr, 12, m12);      /* a well-formed set first */
      bwa_scene_set_mesh_mat(e, v, 8, NULL, 12, NULL);   /* tris missing but counted: dropped */
      bwa_scene_set_mesh_mat(e, NULL, 8, tr, 12, NULL);  /* verts missing but counted: dropped */
      bwa_scene_set_mesh_mat(e, v, 0, tr, 12, NULL);     /* zero verts with real tris: dropped */
      bwa_scene_set_mesh_mat(e, v, 8, tr, 0, NULL);      /* zero tris: dropped */
      bwa_scene_set_mesh_mat(e, v, -8, tr, -12, m12);    /* negative counts: dropped */
      bwa_scene_set_mesh_mat(e, NULL, 0, NULL, 0, NULL); /* the documented CLEAR */
      CHECK(render_blocks(e, 2) == 1, "renders across the malformed-mesh traffic"); }

    /* dynamic meshes: garbage handles are documented ignored; bad geometry is -1 with a reason
     * (with the SDK) or -1 by construction (without) */
    { clr(e);
      int h = bwa_scene_add_dynamic_mesh(e, NULL, 0, NULL, 0, 0);
      CHECK(h == -1, "a dynamic mesh with no geometry must fail");
      bwa_scene_set_dynamic_transform(e, -1, 0, 0, 0, 0, 0, 0, 1);
      bwa_scene_set_dynamic_transform(e, 12345, 1, 2, 3, 0, 0, 0, 1);
      bwa_scene_remove_dynamic_mesh(e, -1);
      bwa_scene_remove_dynamic_mesh(e, 12345);
      CHECK(render_blocks(e, 2) == 1, "renders across the bogus dynamic-mesh traffic"); }

    /* material misuse on top of the smoke coverage: preset out of range, release of 0 and of an
     * out-of-range token — all refused with a reason */
    clr(e);
    CHECK(bwa_material_preset(e, (bwa_material_type)999) == 0 && bwa_last_error(e) != NULL,
          "an unknown material preset must be refused with a reason");
    clr(e);
    bwa_material_release(e, 0);
    CHECK(bwa_last_error(e) != NULL, "releasing token 0 must be refused with a reason");
    clr(e);
    bwa_material_release(e, 5000);
    CHECK(bwa_last_error(e) != NULL, "releasing an out-of-range token must be refused");

    bwa_destroy(e);
}

/* ---- 9. parser abuse: the file-fed surfaces ----------------------------------------------------
 * The loaders are the only places the engine consumes bytes it did not produce. Every probe here
 * follows the same shape: a poisoned file must either be REFUSED with a reason or be SANITIZED so
 * the render stays finite — and a well-formed sibling of the same file must still load, so a green
 * refusal probe cannot hide behind a parser that rejects everything. */
static const char* F32WAV   = "bwa_abuse_f32_nan.wav";     /* mono float wav with NaN/Inf/3e38 samples */
static const char* F32QUAD  = "bwa_abuse_f32_nan_quad.wav";/* 4-ch float wav, same poison (bed path) */
static const char* RATEWAV  = "bwa_abuse_rate100.wav";     /* declares a 100 Hz sample rate */
static const char* EQTXT    = "bwa_abuse_eq.txt";
static const char* LAYJSON  = "bwa_abuse_layout.json";

/* 32-bit IEEE-float wav: `nch` interleaved channels of a 440 Hz tone at `rate`. An IEEE-float wav
 * stores bit patterns VERBATIM, so (poison) scatters NaN, Inf, and finite-but-absurd 3e38 through
 * it — the one asset format that can hand the engine a NaN directly. */
static int write_wav_f32(const char* path, uint32_t frames, uint16_t nch, uint32_t rate, int poison) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    const uint32_t data = frames * 4u * nch;
    uint32_t u; uint16_t w;
    fwrite("RIFF", 1, 4, f); u = 36 + data; fwrite(&u, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    u = 16;              fwrite(&u, 4, 1, f);
    w = 3;               fwrite(&w, 2, 1, f);          /* WAVE_FORMAT_IEEE_FLOAT */
    w = nch;             fwrite(&w, 2, 1, f);
    u = rate;            fwrite(&u, 4, 1, f);
    u = rate * 4u * nch; fwrite(&u, 4, 1, f);
    w = (uint16_t)(4 * nch); fwrite(&w, 2, 1, f);
    w = 32;              fwrite(&w, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    for (uint32_t i = 0; i < frames; ++i) {
        float v = (float)(0.25 * sin(6.2831853 * 440.0 * (double)i / 48000.0));
        if (poison) {
            if      ((i & 127u) == 13) v = NAN;
            else if ((i & 127u) == 29) v = INFINITY;
            else if ((i & 127u) == 61) v = 3.0e38f;    /* passes isfinite; overflows the bus if unscrubbed */
        }
        for (uint16_t c = 0; c < nch; ++c) fwrite(&v, 4, 1, f);
    }
    fclose(f);
    return 1;
}

static int write_text_file(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    fputs(text, f);
    fclose(f);
    return 1;
}

/* a minimal valid 4-speaker layout, each speaker carrying one eq FIR tap given as literal JSON
 * text (so the probe can write "1e300" — finite as a double, Inf once cast to float) */
static int write_layout_eq(const char* path, const char* tap) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    static const float P[4][3] = { {-1.5f,1.5f,-1.5f},{1.5f,1.5f,-1.5f},{-1.5f,1.5f,1.5f},{1.5f,1.5f,1.5f} };
    fprintf(f, "{ \"speakers\": [\n");
    for (int i = 0; i < 4; ++i)
        fprintf(f, "  {\"index\":%d,\"position\":[%g,%g,%g],\"eq\":[%s]}%s\n",
                i, P[i][0], P[i][1], P[i][2], tap, i == 3 ? "" : ",");
    fprintf(f, "] }\n");
    fclose(f);
    return 1;
}

static void parser_abuse(void) {
    CHECK(write_wav_f32(F32WAV, 4096, 1, 48000, 1), "float wav fixture");
    CHECK(write_wav_f32(F32QUAD, 4096, 4, 48000, 1), "float quad fixture");
    CHECK(write_wav_f32(RATEWAV, 256, 1, 100, 0), "rate-100 fixture");

    /* a float wav carrying NaN/Inf/3e38 SAMPLE DATA: the loader must scrub (or refuse) — decoded
     * NaN must never reach the audio thread, and 3e38 must not overflow the bus into the align/
     * room-EQ filter state (which sits before the limiter and never recovers). The tone around the
     * poison must still be audible, so "finite" is not just "the load silently failed". */
    { bwa_desc d = manual_desc();
      bwa_engine* e = bwa_create(&d);
      CHECK(e && bwa_start(e) == BWA_OK, "parser probe engine");
      if (e) {
          /* the LOADER's guarantee is under probe, so the output limiter must not be allowed to
           * scrub a leaked NaN at the device edge and turn a real hole into a green run */
          bwa_set_limiter(e, false);
          uint32_t snd = bwa_load_sound(e, F32WAV);
          CHECK(snd != 0, "a poisoned float wav must load (scrubbed): %s",
                bwa_last_error(e) ? bwa_last_error(e) : "");
          bwa_source s = bwa_source_create(e);
          bwa_source_play(e, s, snd, true);
          bwa_source_set_pos(e, s, 1.f, 1.5f, 0.f);
          bwa_commit(e);
          CHECK(bwa_source_is_playing(e, s), "the poisoned-wav voice must be playing");
          double pk = 0;
          int r = render_peak(e, 16, &pk);
          CHECK(r == 1, "NaN/Inf/3e38 wav samples must not reach the device (render=%d)", r);
          CHECK(pk > 1e-6, "the poisoned wav's tone must still be audible (peak %g)", pk);
          CHECK(pk < 1e3, "a 3e38 wav sample must be capped, not amplified onto the bus (peak %g)", pk);

          /* the same poison through the BED path (4-ch AmbiX-shaped float wav) */
          uint32_t ambi = bwa_load_ambix(e, F32QUAD);
          CHECK(ambi != 0, "a poisoned float quad must load (scrubbed): %s",
                bwa_last_error(e) ? bwa_last_error(e) : "");
          bwa_bed b = bwa_bed_create(e);
          bwa_bed_play(e, b, ambi, true);
          bwa_commit(e);
          CHECK(bwa_bed_is_playing(e, b), "the poisoned-quad bed must be playing");
          pk = 0;
          r = render_peak(e, 16, &pk);
          CHECK(r == 1, "poisoned bed samples must not reach the device (render=%d)", r);
          CHECK(pk > 1e-6, "the poisoned bed's tone must still be audible (peak %g)", pk);
          bwa_bed_stop(e, b);

          /* the same poison through the STREAMING path (background decode thread -> ring) */
          uint32_t strm = bwa_load_sound_streaming(e, F32WAV);
          CHECK(strm != 0, "a poisoned float wav must stream (scrubbed): %s",
                bwa_last_error(e) ? bwa_last_error(e) : "");
          bwa_source ss = bwa_source_create(e);
          bwa_source_play(e, ss, strm, true);
          bwa_commit(e);
          pk = 0;
          int ok = 1;
          for (int tries = 0; tries < 40 && pk <= 1e-6 && ok == 1; ++tries) {
              Sleep(5);                                  /* let the streaming thread prebuffer/fill */
              ok = render_peak(e, 2, &pk);
          }
          CHECK(ok == 1, "poisoned streamed samples must not reach the device (render=%d)", ok);
          CHECK(pk > 1e-6, "the poisoned stream's tone must become audible (peak %g)", pk);
          bwa_source_stop(e, ss);

          /* an absurd-but-finite PUSHED sample is the same class: capped, never onto the bus raw */
          bwa_source p = bwa_source_create_push(e);
          CHECK(p != 0, "push source for the absurd-sample probe");
          { float blk[512];
            for (int i = 0; i < 512; ++i) blk[i] = (i & 1) ? 3.0e38f : 0.25f;
            for (int reps = 0; reps < 4; ++reps) bwa_source_push(e, p, blk, 512);
            bwa_commit(e);
            pk = 0;
            r = render_peak(e, 6, &pk);
            CHECK(r == 1, "a 3e38 pushed sample must not reach the device non-finite (render=%d)", r);
            CHECK(pk > 1e-6, "the pushed signal must be audible around the poison (peak %g)", pk);
            CHECK(pk < 1e3, "a 3e38 pushed sample must be capped (peak %g)", pk); }
          bwa_source_destroy(e, p);

          /* a wav declaring an implausible sample rate would ask the resampler for a 480x upsample
           * (multi-GB allocation): refused with a reason, engine unharmed */
          clr(e);
          CHECK(bwa_load_sound(e, RATEWAV) == 0 && bwa_last_error(e) != NULL,
                "a rate-100 wav must be refused with a reason");
          clr(e);
          CHECK(bwa_load_ambix(e, RATEWAV) == 0 && bwa_last_error(e) != NULL,
                "a rate-100 wav must be refused by the bed loader too");
          CHECK(render_blocks(e, 2) == 1, "renders after the refused loads");
          bwa_destroy(e);
      } }

    /* headphone EQ: the AutoEq text parser. sscanf %f parses "nan"/"inf", `fc <= 0` passes NaN
     * (every NaN comparison is false), and gain used to have no check at all — each poisoned file
     * must be refused with a reason, and the well-formed control must still load. */
    { bwa_desc d = manual_desc();
      bwa_engine* e = bwa_create(&d);
      CHECK(e != NULL, "hpeq probe engine");
      if (e) {
          static const struct { const char* name; const char* text; } bad[] = {
              { "NaN Fc",       "Filter 1: ON PK Fc nan Hz Gain 2.0 dB Q 1.41\n" },
              { "Inf Fc",       "Filter 1: ON PK Fc inf Hz Gain 2.0 dB Q 1.41\n" },
              { "NaN Q",        "Filter 1: ON PK Fc 1000 Hz Gain 2.0 dB Q nan\n" },
              { "NaN Gain",     "Filter 1: ON PK Fc 1000 Hz Gain nan dB Q 1.41\n" },
              { "absurd Gain",  "Filter 1: ON PK Fc 1000 Hz Gain 4000 dB Q 1.41\n" },
              { "absurd Q",     "Filter 1: ON PK Fc 1000 Hz Gain 2.0 dB Q 3e8\n" },
              { "NaN Preamp",   "Preamp: nan dB\nFilter 1: ON PK Fc 1000 Hz Gain 2.0 dB Q 1.41\n" },
              { "absurd Preamp","Preamp: 400 dB\nFilter 1: ON PK Fc 1000 Hz Gain 2.0 dB Q 1.41\n" },
              /* Q passes `q > 0` but designs a marginally-stable section whose ringing never decays */
              { "marginal Q",   "Filter 1: ON PK Fc 1000 Hz Gain 2.0 dB Q 1e-9\n" },
              /* each section is in range; the CASCADE composes to +60 dB (per-section bounds
               * don't bound the composed response) */
              { "composed boost", "Filter 1: ON PK Fc 500 Hz Gain 20.0 dB Q 1.41\n"
                                  "Filter 2: ON PK Fc 1000 Hz Gain 20.0 dB Q 1.41\n"
                                  "Filter 3: ON PK Fc 2000 Hz Gain 20.0 dB Q 1.41\n" },
          };
          for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
              CHECK(write_text_file(EQTXT, bad[i].text), "eq fixture write");
              clr(e);
              CHECK(bwa_load_headphone_eq(e, EQTXT) != BWA_OK && bwa_last_error(e) != NULL,
                    "a %s EQ file must be refused with a reason", bad[i].name);
          }
          /* the control: same shape, sane numbers — the parser must accept it, proving the
           * refusals above rejected the VALUES and not the format */
          CHECK(write_text_file(EQTXT,
                "Preamp: -6.0 dB\nFilter 1: ON PK Fc 1000 Hz Gain 2.0 dB Q 1.41\n"), "eq control write");
          CHECK(bwa_load_headphone_eq(e, EQTXT) == BWA_OK,
                "the well-formed EQ control must load: %s", bwa_last_error(e) ? bwa_last_error(e) : "");
          bwa_destroy(e);
      } }

    /* layout: an eq tap of 1e300 is FINITE as a double but Inf once cast to float, and a finite
     * 1e30 tap overflows the bus the first time the FIR runs — both must refuse the layout. The
     * control tap proves the schema itself is accepted. */
    { static const struct { const char* name; const char* tap; int ok; } lay[] = {
          { "1e300 (double-finite, float-Inf) eq tap", "1e300", 0 },
          { "1e30 (finite-but-absurd) eq tap",         "1e30",  0 },
          { "1e999 (Inf as parsed) eq tap",            "1e999", 0 },
          { "sane eq tap",                             "0.25",  1 },
      };
      for (size_t i = 0; i < sizeof lay / sizeof lay[0]; ++i) {
          CHECK(write_layout_eq(LAYJSON, lay[i].tap), "layout fixture write");
          bwa_desc d = manual_desc();
          d.layout_path = LAYJSON;
          bwa_engine* e = bwa_create(&d);
          CHECK(e != NULL, "create must survive a %s layout", lay[i].name);
          if (!e) continue;
          if (lay[i].ok) {
              CHECK(bwa_start(e) == BWA_OK, "a layout with a %s must start: %s",
                    lay[i].name, bwa_last_error(e) ? bwa_last_error(e) : "");
              CHECK(bwa_get_channel_count(e) == 4, "the 4-speaker layout must drive 4 channels");
              CHECK(render_blocks(e, 2) == 1, "renders on the sane-eq layout");
          } else {
              CHECK(bwa_last_error(e) != NULL, "the %s failure must be readable after create", lay[i].name);
              CHECK(bwa_start(e) == BWA_ERR_LAYOUT, "a layout with a %s must refuse to start", lay[i].name);
          }
          bwa_destroy(e);
      } }

    remove(F32WAV); remove(F32QUAD); remove(RATEWAV); remove(EQTXT); remove(LAYJSON);
}

/* ---- 10. the null sink: the same misuse against a REAL audio thread ----------------------------
 * Everything above ran single-threaded on the manual sink. The lifecycle traffic (double start,
 * stale handles, destroy-while-running) must also hold with an actual concurrent renderer. */
static void null_sink_lifecycle(void) {
    bwa_desc d = manual_desc();
    d.sink = BWA_SINK_NULL;
    bwa_engine* e = bwa_create(&d);
    CHECK(e != NULL, "null-sink create");
    if (!e) return;
    uint32_t snd = bwa_load_sound(e, WAV);
    bwa_source s = bwa_source_create(e);
    CHECK(bwa_start(e) == BWA_OK, "null-sink start: %s", bwa_last_error(e));
    CHECK(bwa_start(e) == BWA_OK, "double start on a running audio thread is a no-op");
    bwa_source_play(e, s, snd, true);
    bwa_source_set_pos(e, s, 1, 1.5f, 0);
    bwa_commit(e);
    Sleep(30);                                           /* let several blocks land */
    CHECK(bwa_get_dsp_time(e) > 0, "the audio thread must have rendered");
    /* stale traffic against the live thread */
    bwa_source stale = s ^ 0x10000u;
    bwa_source_play(e, stale, snd, true);
    bwa_source_stop(e, stale);
    bwa_source_destroy(e, stale);
    bwa_play_oneshot(e, 0, 0, 0, 0, 1.f);
    bwa_commit(e);
    Sleep(15);
    CHECK(bwa_source_is_playing(e, s), "the live voice must survive the stale traffic");
    CHECK(bwa_stop(e) == BWA_OK && bwa_stop(e) == BWA_OK, "stop twice");
    CHECK(bwa_start(e) == BWA_OK, "restart after stop");
    Sleep(15);
    bwa_destroy(e);                                      /* destroy while running: must join + free */
}

int main(void) {
    if (!write_wav(WAV, 512, 1) || !write_wav(QUAD, 512, 4)) {
        fprintf(stderr, "FAIL: cannot write wav fixtures\n");
        return 1;
    }
    null_engine_sweep();
    with_engine_null_zero();
    call_order();
    handle_lifetime();
    kind_mismatch();
    nonfinite_and_boundary();
    capacity();
    newest_surfaces();
    parser_abuse();
    null_sink_lifecycle();
    remove(WAV);
    remove(QUAD);
    if (fails) {
        fprintf(stderr, "abuse: %d FAILED assertion%s\n", fails, fails == 1 ? "" : "s");
        return 1;
    }
    printf("abuse OK (NULL/zero args; stale/bogus/cross-engine handles; call order; non-finite and "
           "boundary arguments; pool/table/ring exhaustion; poll_ended/tuning/box-mesh/mesh-mat "
           "surfaces; kind mismatches; poisoned wav/EQ/layout parsers; null-sink lifecycle)\n");
    return 0;
}
