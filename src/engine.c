/*
 * engine.c — public C ABI: lifecycle, the device sink, and the control-thread side of
 * the API. The real-time machinery (rings, voice table, commit snapshot, mixing) lives
 * in rt.c behind sink.h's render callback; engine.c forwards the per-frame bwa_* calls to
 * it. M0 = builds/links; M1 = device sink + audio loop; M2 = the concurrency spine.
 * Sounds (bwa_load_sound) are still stubs until M3.
 */
#include "bw_audio.h"
#include "frame.h"         /* BWA_ROOM_* identity basis + frame_qrot */
#include "sink.h"
#include "rt.h"
#include "layout.h"
#include "dbap.h"          /* offline panner evaluation (bwa_panner_gains_batch) */
#include "spcap.h"
#include "vbap.h"
#include "binaural.h"
#include "natnet.h"
#include "steam_decode.h"   /* phonon-free interfaces; impls linked only when BWA_HAVE_STEAMAUDIO */
#include "profile.h"
#include "steam_scene.h"
#include "steam_reflect.h"
#include "steam_path.h"
#include "fdn.h"
#include "ism.h"

#include <stdio.h>          /* snprintf (the backend readback string) */
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>           /* InterlockedExchange for the 'both' buffer handoff */

#define BWA_VOICE_CAP 256       /* max simultaneous sources */
#define BWA_SOUND_CAP 256       /* max loaded sounds */
#define BWA_MAX_BLOCK 8192      /* upper bound on a device's frames-per-block (ASIO picks its own) */
#define BWA_MAX_MATERIALS 64    /* material-table capacity (token == index; [0] = default) */

/* Built-in material presets. Coefficients are Steam Audio's published example materials: 3-band
 * absorption (low/mid/high), scattering, 3-band transmission. ORDER IS ABI — the row index is the
 * bwa_material_type value (bw_audio.h); the name column is documentation. */
static const struct { const char* name; float absorption[3], scattering, transmission[3]; } BWA_PRESETS[] = {
    { "generic",  {0.10f,0.20f,0.30f}, 0.05f, {0.100f,0.050f,0.030f} },
    { "brick",    {0.03f,0.04f,0.07f}, 0.05f, {0.015f,0.015f,0.015f} },
    { "concrete", {0.05f,0.07f,0.08f}, 0.05f, {0.015f,0.002f,0.001f} },
    { "ceramic",  {0.01f,0.02f,0.02f}, 0.05f, {0.060f,0.044f,0.011f} },
    { "gravel",   {0.60f,0.70f,0.80f}, 0.05f, {0.031f,0.012f,0.008f} },
    { "carpet",   {0.24f,0.69f,0.73f}, 0.05f, {0.020f,0.005f,0.003f} },
    { "glass",    {0.06f,0.03f,0.02f}, 0.05f, {0.060f,0.044f,0.011f} },
    { "plaster",  {0.12f,0.06f,0.04f}, 0.05f, {0.056f,0.056f,0.004f} },
    { "wood",     {0.11f,0.07f,0.06f}, 0.05f, {0.070f,0.014f,0.005f} },
    { "metal",    {0.20f,0.07f,0.06f}, 0.05f, {0.200f,0.025f,0.010f} },
    { "rock",     {0.13f,0.20f,0.24f}, 0.05f, {0.015f,0.002f,0.001f} },
};

/* The three profiles (docs/architecture.md):
 *   cave     — render the 26-ch array straight to a 26-ch device (ASIO/DVS).
 *   binaural — render the array into memory, decode to a 2-ch device (the monitor).
 *   both     — array to a 26-ch device AND the monitor to a 2-ch device, concurrently.
 * The monitor uses the listener's head orientation; the array render ignores it.
 *
 * NOTE: an ASIO driver picks its own buffer size, usually != cfg.block_size. binaural sizes
 * its render scratch to BWA_MAX_BLOCK and renders whatever block the device dictates, so it works
 * with any driver. The 'both' double-buffer still assumes the two devices share cfg.block_size (a
 * mismatched monitor block is silenced; the array renders any size). Live headphone output works
 * through a 2-ch ASIO driver (ASIO4ALL / FlexASIO / the Steinberg built-in); WASAPI is future. */
struct bwa_engine {
    bwa_desc    cfg;
    int         started;
    const char* last_error;        /* points at errbuf or a literal; NULL when clean */
    char        errbuf[256];
    char        backend_buf[96];   /* bwa_get_audio_backend readback (device + which monitor is live) */
    bwa_profile   profile;
    int         panner;            /* mirror of bwa_set_panner (bwa_panner; 0 = DBAP) for the room_eq start guard */

    RtCore*     rt;               /* rings + voice/sound tables + mixer (rt.c) */
    Monitor*    monitor;          /* binaural/both: 26->stereo decode */
    Layout      layout;           /* effective speaker geometry (for the Steam decoder at start) */
    uint32_t    cap;              /* scratch capacity in frames (== cfg.block_size) */

    bwa_sink*     sink;             /* primary: cave 26ch / binaural 2ch / both array 26ch */
    float*      scratch26;        /* binaural: 26-ch array render before the monitor */

    bwa_sink*     sink_mon;         /* both: the monitor (2-ch) device */
    float*      mon_buf[2];       /* both: stereo double-buffer (each 2*cap), array thread -> monitor thread */
    volatile LONG mon_idx;        /* both: index of the latest published buffer */

    NatNet*     tracker;          /* internal tracking (bwa_tracker_connect); NULL = pushed pose */
    SteamMonitor* steam;          /* production HRTF decode (binaural/both); NULL = first-cut pan */
    SteamScene*   scene;          /* materials occlusion sim (off-thread); NULL without the SDK */
    SteamReflect* reflect;        /* reflection bed (created at bwa_start if configured); NULL otherwise */
    bwa_reflections_desc refl_cfg;  /* set via bwa_reflections_config before bwa_start */
    IsmRoom       ism_room;       /* the shoebox for the image-source early reflections (bwa_scene_set_box;
                                   * captured with or without the Steam build — ism.c is phonon-free) */
    int           ism_warned;     /* the "ISM + Steam bed = double early reflections" warning, once */
    Fdn*          fdn;            /* directional FDN reverb bed (bwa_fdn_config; created at bwa_start) */
    bwa_fdn_desc   fdn_cfg;        /* staged pre-start (zeros normalized to defaults in bwa_fdn_config);
                                   * when enabled, the FDN takes the bus tap (not the Steam bed) */
    float         refl_wet;       /* reverb wet level (bwa_reflections_set_gain, the one control);
                                   * seeds whichever bed bwa_start creates, live thereafter */
    int           max_re;         /* mirror of bwa_set_max_re, so a pre-start toggle seeds the FDN */
    SteamPath*    path;           /* pathing (bwa_desc.enable_pathing); NULL otherwise */
    float         src_pos[BWA_VOICE_CAP][3];  /* control-side per-source positions, so set_pathing has the pos */

    /* material table (control-thread): token == index; [0] is the built-in default. Coefficients are
     * resolved to per-triangle materials when a mesh is set. mat_free marks released slots (reusable by
     * a later bwa_material_define); num_materials is the high-water mark of ever-allocated slots. */
    struct { float absorption[3], scattering, transmission[3]; } materials[BWA_MAX_MATERIALS];
    uint8_t       mat_free[BWA_MAX_MATERIALS];   /* calloc'd 0 = in use; 1 = released, available for reuse */
    uint32_t      num_materials;

    /* output capture (bwa_set_output_capture): the audio thread reads these once per block. volatile +
     * write-user-before-cb ordering (x64 store order) so a non-NULL cb is always seen with its user. */
    bwa_output_fn volatile capture_cb;
    void*         volatile capture_user;
};

/* audio thread: hand the final device-bound block to the capture callback, if one is set. */
static inline void engine_capture(bwa_engine* e, const float* planar, uint32_t channels, uint32_t n) {
    bwa_output_fn cb = e->capture_cb;
    if (cb) cb(e->capture_user, planar, channels, n);
}

static void set_error(bwa_engine* e, const char* msg) {
    if (!e) return;
    if (msg && msg != e->errbuf) { strncpy(e->errbuf, msg, sizeof e->errbuf - 1); e->errbuf[sizeof e->errbuf - 1] = 0; }
    e->last_error = (msg && e->errbuf[0]) ? e->errbuf : msg;
}
/* Reset the error state at the start of an operation, so a SUCCESSFUL call leaves bwa_last_error at
 * NULL (not a stale pointer to a now-empty errbuf, which reads as a spurious non-NULL "" error). */
static void clear_error(bwa_engine* e) { if (e) { e->errbuf[0] = 0; e->last_error = NULL; } }

/* EVERY play-shaped entry point (bwa_source_play / _play_at / bwa_bed_play) must refuse a push
 * source WITH an error: the rt-level guard (rt_source_play_at) drops the play silently and relies
 * on the engine layer to report. Returns true when the play must be refused. */
static bool refuse_push_play(bwa_engine* e, bwa_source s, const char* msg) {
    if (!rt_source_is_push(e->rt, s)) return false;
    set_error(e, msg);
    return true;
}

/* push-API guard: true = s is a live push source (proceed). A live NON-push handle is REPORTED —
 * a bare 0/no-op reads exactly like ring backpressure and the pacing loop would spin forever —
 * while a stale handle keeps the documented silent-no-op contract every bwa_* call shares. */
static bool push_guard(bwa_engine* e, bwa_source s, const char* msg) {
    if (rt_source_is_push(e->rt, s)) return true;
    if (rt_source_live(e->rt, s)) set_error(e, msg);
    return false;
}

/* cave: the 26-ch array goes straight to the device. */
static void render_cave(void* user, float* dev, uint32_t n, const bwa_timestamp* ts) {
    bwa_engine* e = (bwa_engine*)user;
    rt_render(e->rt, dev, n, ts);
    engine_capture(e, dev, e->layout.count, n);          /* final array output (post-limiter) */
}

/* binaural: render the 26-ch array to scratch, then decode to the 2-ch device. The scratch is
 * sized to BWA_MAX_BLOCK, so any device block size works (the binaural decode uses n internally
 * and consistently — no cross-buffer offset to keep in sync, unlike 'both'). */
static void render_binaural(void* user, float* dev2, uint32_t n, const bwa_timestamp* ts) {
    bwa_engine* e = (bwa_engine*)user;
    if (n > BWA_MAX_BLOCK) { memset(dev2, 0, sizeof(float) * (size_t)n * 2); return; }
    rt_render(e->rt, e->scratch26, n, ts);
    float p[3], q[4];
    rt_get_listener(e->rt, p, q);
    BWA_ZONE_BEGIN(zbin, "binaural decode");                /* 26 virtual speakers -> HRTF -> 2 ch */
#ifdef BWA_HAVE_STEAMAUDIO
    if (e->steam) { steam_monitor_process(e->steam, e->scratch26, p, q, dev2, n); }
    else
#endif
    monitor_process(e->monitor, e->scratch26, p, q, dev2, n);
    if (bwa_null_sink_tap) bwa_null_sink_tap(dev2, 2, n);   /* test hook: observe the device-bound
                                                             * stereo on ANY sink (see null_sink.c) */
    engine_capture(e, dev2, 2, n);                          /* public capture: the binaural headphone output */
    BWA_ZONE_END(zbin);
}

/* both, array thread: render the array to the 26-ch device (any block size), then — when the
 * block matches cap — decode the monitor into the back buffer and publish it. */
static void render_both_array(void* user, float* dev26, uint32_t n, const bwa_timestamp* ts) {
    bwa_engine* e = (bwa_engine*)user;
    rt_render(e->rt, dev26, n, ts);
    engine_capture(e, dev26, e->layout.count, n);       /* primary output = the 26-ch array (post-limiter) */
    if (n != e->cap) return;                            /* off-spec block: skip the fixed-size monitor publish */
    LONG cur = e->mon_idx;                              /* producer is the sole writer of mon_idx */
    float p[3], q[4];
    rt_get_listener(e->rt, p, q);
#ifdef BWA_HAVE_STEAMAUDIO
    if (e->steam) steam_monitor_process(e->steam, dev26, p, q, e->mon_buf[1 - cur], n);
    else
#endif
    monitor_process(e->monitor, dev26, p, q, e->mon_buf[1 - cur], n);
    InterlockedExchange(&e->mon_idx, 1 - cur);          /* publish the back buffer (full barrier) */
}

/* both, monitor thread: play the latest published stereo buffer. */
static void render_both_monitor(void* user, float* dev2, uint32_t n, const bwa_timestamp* ts) {
    bwa_engine* e = (bwa_engine*)user;
    (void)ts;
    memset(dev2, 0, sizeof(float) * (size_t)n * 2);
    if (n != e->cap) return;                            /* off-spec block: silence */
    LONG cur = InterlockedCompareExchange(&e->mon_idx, 0, 0);  /* atomic acquire read of the index */
    const float* src = e->mon_buf[cur];                 /* planar [L(cap), R(cap)], cap == n here */
    memcpy(dev2,     src,          sizeof(float) * n);  /* L */
    memcpy(dev2 + n, src + e->cap, sizeof(float) * n);  /* R */
}

/* ---- lifecycle ---- */

bwa_engine* bwa_create(const bwa_desc* cfg) {
    if (!cfg) return NULL;
    bwa_engine* e = (bwa_engine*)calloc(1, sizeof *e);
    if (!e) return NULL;
    e->cfg = *cfg;
    if (e->cfg.sample_rate == 0) e->cfg.sample_rate = 48000;   /* sane defaults */
    if (e->cfg.block_size  == 0) e->cfg.block_size  = 256;
    e->profile = e->cfg.profile;

    /* Resolve the speaker geometry FIRST: the engine's CHANNEL COUNT is the layout's speaker count
     * (a file with 4..BWA_CHANNELS speakers, or the built-in 26-grid default with no path — the
     * default IS a layout, so tests/desk-dev still need no file). A load failure is non-fatal
     * (default grid) but surfaces via bwa_last_error — note it now also means the channel count
     * falls back to 26, so a 24-speaker install MUST check bwa_last_error after bwa_create. */
    Layout L = layout_default();
    if (e->cfg.layout_path && e->cfg.layout_path[0]) {
        if (!layout_load(e->cfg.layout_path, e->cfg.sample_rate, &L, e->errbuf, sizeof e->errbuf))
            set_error(e, e->errbuf[0] ? e->errbuf : "bwa_create: layout load failed");
    }
    e->layout = L;                                  /* kept for the Steam decoder, built at bwa_start */

    e->rt = rt_create(BWA_VOICE_CAP, BWA_SOUND_CAP, e->cfg.sample_rate, L.count);
    if (!e->rt) { free(e); return NULL; }
    rt_set_layout(e->rt, &L);
    /* diffuse-bed decoder is create-time config: the SH->speaker decode matrix is (re)built here on
     * the control thread — it must not race mix_bed once the audio thread runs. */
    rt_set_bed_decoder(e->rt, (int)e->cfg.bed_decoder);
    e->refl_wet = 1.0f;                             /* reverb wet default; bwa_reflections_set_gain */

    /* material table: token 0 is always the built-in "generic" default (BWA_PRESETS[0]). */
    for (int b = 0; b < 3; ++b) {
        e->materials[0].absorption[b]   = BWA_PRESETS[0].absorption[b];
        e->materials[0].transmission[b] = BWA_PRESETS[0].transmission[b];
    }
    e->materials[0].scattering = BWA_PRESETS[0].scattering;
    e->num_materials = 1;

    if (e->profile == BWA_PROFILE_BINAURAL || e->profile == BWA_PROFILE_BOTH) {
        e->monitor = monitor_create(&e->layout, e->cfg.sample_rate);   /* count-driven virtual speakers */
        if (!e->monitor) { rt_destroy(e->rt); free(e); return NULL; }
    }
#ifdef BWA_HAVE_STEAMAUDIO
    /* materials occlusion sim (off-thread); non-fatal — occlusion is just unavailable if it fails */
    e->scene = steam_scene_create(e->rt, e->cfg.sample_rate, e->cfg.block_size, BWA_VOICE_CAP, e->cfg.embree);
#endif
    /* OWN the path strings: the caller's buffers (e.g. a P/Invoke binding's marshalled UTF-8 temporaries)
     * may be freed once bwa_create returns, but bwa_start dereferences hrtf_path later — a shallow pointer
     * copy would dangle. Copy here, free in bwa_destroy. _strdup-fail -> NULL = the documented default. */
    e->cfg.layout_path = cfg->layout_path ? _strdup(cfg->layout_path) : NULL;
    e->cfg.hrtf_path   = cfg->hrtf_path   ? _strdup(cfg->hrtf_path)   : NULL;
    e->cfg.asio_driver = cfg->asio_driver ? _strdup(cfg->asio_driver) : NULL;
    return e;
}

/* Close any open device(s) and free the start-time scratch. Monitor sink (consumer) first,
 * then the array sink (producer), so the monitor thread stops reading mon_buf before we free
 * it. Each bwa_sink_close joins its audio thread. */
static void engine_close_devices(bwa_engine* e) {
    if (e->sink_mon) { bwa_sink_close(e->sink_mon); e->sink_mon = NULL; }
    if (e->sink)     { bwa_sink_close(e->sink);     e->sink     = NULL; }
#ifdef BWA_HAVE_STEAMAUDIO
    /* after the audio thread joins (sinks closed above): unregister the tap, then destroy the bed —
     * its IR aliases the bed source, so it must die before steam_scene_destroy (bwa_destroy, later). */
    if (e->reflect)  { rt_set_bus_tap(e->rt, NULL, NULL); steam_reflect_destroy(e->reflect); e->reflect = NULL; }
    if (e->path)     { rt_set_path_tap(e->rt, NULL, NULL, 0); steam_path_destroy(e->path); e->path = NULL; }
    if (e->steam)    { steam_monitor_destroy(e->steam); e->steam = NULL; }   /* after the audio thread joins */
#endif
    if (e->fdn)      { rt_set_bus_tap(e->rt, NULL, NULL); fdn_destroy(e->fdn); e->fdn = NULL; }   /* after the audio thread joins */
    /* NOTE: the tracker is NOT closed here — its lifetime is bwa_tracker_connect/disconnect (or
     * bwa_destroy), independent of start/stop, so tracking survives a device restart. */
    free(e->scratch26);  e->scratch26  = NULL;
    free(e->mon_buf[0]); e->mon_buf[0] = NULL;
    free(e->mon_buf[1]); e->mon_buf[1] = NULL;
}

/* the room_eq invariant, shared by bwa_start and bwa_tracker_connect: static-listener room
 * correction (bwa_calibrate --room-eq) is only valid for a listener parked at the measurement
 * point (docs/calibration.md). */
static int layout_has_room_eq(const bwa_engine* e) {
    for (uint32_t i = 0; i < e->layout.count; ++i)
        if (e->layout.speakers[i].room_eq_count) return 1;
    return 0;
}

bwa_result bwa_start(bwa_engine* e) {
    if (!e) return BWA_ERR_CONFIG;
    if (e->started) return BWA_OK;
    e->errbuf[0] = 0; e->last_error = NULL;

    /* room_eq guard: a moving-listener session — the DBAP panner and/or a connected tracker — with
     * a room_eq'd layout is the wrong-file mistake; fail LOUDLY rather than quietly mis-correct the
     * whole array. (bwa_tracker_connect enforces the same invariant at its end.) */
    if (layout_has_room_eq(e) && (e->panner == (int)BWA_PAN_DBAP || e->tracker)) {
        set_error(e, "bwa_start: the layout carries room_eq (room correction at ONE point; static "
                     "listener only), but this session renders a MOVING listener (DBAP panner "
                     "and/or a connected tracker). Load the roaming layout, recalibrate with "
                     "--room-eq-grid (tracked room EQ), or use SPCAP/VBAP with a fixed pose.");
        return BWA_ERR_CONFIG;
    }

    const uint32_t sr = e->cfg.sample_rate, bs = e->cfg.block_size;
    e->cap = bs;

    if (e->profile == BWA_PROFILE_CAVE) {
        e->sink = bwa_sink_open(sr, bs, e->layout.count, (int)e->cfg.sink, e->cfg.asio_driver, render_cave, e, e->errbuf, sizeof e->errbuf);
    } else if (e->profile == BWA_PROFILE_BINAURAL) {
        e->scratch26 = (float*)calloc((size_t)BWA_MAX_BLOCK * BWA_CHANNELS, sizeof(float));
        if (e->scratch26)
            e->sink = bwa_sink_open(sr, bs, 2, (int)e->cfg.sink, e->cfg.asio_driver, render_binaural, e, e->errbuf, sizeof e->errbuf);
    } else { /* both: a 26-ch array sink + a 2-ch monitor sink sharing a double-buffer */
        /* Size the handoff for ANY device block (the ASIO driver picks its own, != cfg.block_size in
         * general); cap is set to the ACTUAL array block below, once the sink reports it. */
        e->mon_buf[0] = (float*)calloc((size_t)BWA_MAX_BLOCK * 2, sizeof(float));
        e->mon_buf[1] = (float*)calloc((size_t)BWA_MAX_BLOCK * 2, sizeof(float));
        e->mon_idx = 0;
        if (e->mon_buf[0] && e->mon_buf[1]) {
            e->sink = bwa_sink_open(sr, bs, e->layout.count, (int)e->cfg.sink, e->cfg.asio_driver, render_both_array, e, e->errbuf, sizeof e->errbuf);
            if (e->sink) {
                e->cap = bwa_sink_block_size(e->sink);   /* the array's real block; render_both_* gate on it */
                e->sink_mon = bwa_sink_open(sr, bs, 2, (int)e->cfg.sink, e->cfg.asio_driver, render_both_monitor, e, e->errbuf, sizeof e->errbuf);
                /* the double-buffer handoff exchanges cap-sized blocks, so the two devices must agree.
                 * A mismatch would leave the monitor silent — fail with a clear message instead. */
                if (e->sink_mon && bwa_sink_block_size(e->sink_mon) != e->cap) {
                    set_error(e, "both profile: the array and monitor ASIO devices must use the same buffer size");
                    engine_close_devices(e);
                    return BWA_ERR_DEVICE;
                }
            }
        }
    }

    if (!e->sink || (e->profile == BWA_PROFILE_BOTH && !e->sink_mon)) {
        set_error(e, e->errbuf[0] ? e->errbuf : "bwa_start: device open failed");
        engine_close_devices(e);
        return BWA_ERR_DEVICE;
    }

#ifdef BWA_HAVE_STEAMAUDIO
    /* production HRTF decode for binaural/both. phonon's effect frameSize is fixed at create and
     * must equal the device block n, which the ASIO driver dictates (!= cfg.block_size in general)
     * — so build it now that the sink is open and its real block size is known. Non-fatal: render
     * falls back to the simple-pan monitor if NULL. Torn down in engine_close_devices. */
    if (e->profile == BWA_PROFILE_BINAURAL || e->profile == BWA_PROFILE_BOTH)
        e->steam = steam_monitor_create(&e->layout, e->cfg.sample_rate,
                                        bwa_sink_block_size(e->sink), e->cfg.hrtf_path);

    /* reflection bed: a separate reflections sim + the audio-thread convolution registered as the rt
     * bus tap. Same frameSize-fixed-at-create reason as the monitor — build it now. Non-fatal: if it
     * fails, no tap is registered and the engine runs dry. Needs the occlusion scene (shared geometry). */
    if (e->scene && e->refl_cfg.enabled && !e->fdn_cfg.enabled) {
        e->reflect = steam_reflect_create(e->scene, e->rt, &e->layout, e->cfg.sample_rate,
                                          bwa_sink_block_size(e->sink), e->refl_cfg.order,
                                          e->refl_cfg.ir_seconds, e->refl_cfg.num_rays, e->refl_cfg.num_bounces,
                                          e->refl_wet, e->refl_cfg.bake != 0);
        if (e->reflect) rt_set_bus_tap(e->rt, steam_reflect_tap, e->reflect);
    }
    /* pathing (opt-in bwa_desc.enable_pathing): bake the visibility graph, register the order-1 path
     * tap. Sources opt in via bwa_source_set_pathing; the indirect field decodes onto the bus alongside
     * the reflection bed. */
    {
        if (e->scene && e->cfg.enable_pathing) {
            e->path = steam_path_create(e->scene, e->rt, &e->layout, e->cfg.sample_rate, bwa_sink_block_size(e->sink), 1);
            if (e->path) { rt_set_path_tap(e->rt, steam_path_tap, e->path, 4 /* (1+1)^2 */); steam_path_start(e->path); }
        }
    }
#endif

    /* directional FDN reverb bed (bwa_fdn_config; phonon-free): takes the bus tap the Steam bed would
     * otherwise use (mutually exclusive — one reverb bed at a time; the Steam block above is skipped
     * when the FDN is enabled). Consumes the same mono aux send + per-voice send levels. Non-fatal on
     * allocation failure: no tap, the engine runs dry, the reason surfaces via bwa_last_error. */
    if (e->fdn_cfg.enabled) {
        e->fdn = fdn_create(&e->layout, e->cfg.sample_rate, e->layout.count, (int)e->cfg.bed_decoder);
        if (e->fdn) {
            fdn_set_decay(e->fdn, e->fdn_cfg.rt60_low_s, e->fdn_cfg.rt60_high_s, e->fdn_cfg.xover_hz);
            const float* d = e->fdn_cfg.decay_dir;
            if ((d[0] != 0.f || d[1] != 0.f || d[2] != 0.f) &&
                e->fdn_cfg.decay_factor > 0.f && e->fdn_cfg.decay_factor != 1.f)
                fdn_set_decay_direction(e->fdn, d, e->fdn_cfg.decay_factor);
            fdn_set_gain(e->fdn, e->refl_wet);
            fdn_set_max_re(e->fdn, e->max_re);      /* a pre-start bwa_set_max_re reaches the FDN too */
            rt_set_bus_tap(e->rt, fdn_tap, e->fdn);
        } else set_error(e, "bwa_start: FDN reverb allocation failed (running dry)");
    }

    if (bwa_sink_start(e->sink) != 0 || (e->sink_mon && bwa_sink_start(e->sink_mon) != 0)) {
        set_error(e, "bwa_start: sink failed to start");
        engine_close_devices(e);
        return BWA_ERR_DEVICE;
    }
    e->started = 1;
    return BWA_OK;
}

bwa_result bwa_stop(bwa_engine* e) {
    if (!e) return BWA_ERR_CONFIG;
    engine_close_devices(e);                            /* joins the audio thread(s) */
    e->started = 0;
    return BWA_OK;
}

/* ---- internal tracking (OptiTrack/NatNet). Control thread; may block (socket open / a short
 * drain when replacing a live connection) — lifecycle-class calls, not per-frame ones. ---- */
bwa_result bwa_tracker_connect(bwa_engine* e, const bwa_tracker_desc* d) {
    if (!e || !d) return BWA_ERR_CONFIG;
    clear_error(e);
    /* the same invariant bwa_start enforces: static room_eq is room correction at ONE point —
     * refuse to attach a moving (tracked) listener to it (docs/calibration.md). */
    if (layout_has_room_eq(e)) {
        set_error(e, "bwa_tracker_connect: the layout carries static room_eq (fixed listener only) — "
                     "recalibrate with --room-eq-grid for a tracked session");
        return BWA_ERR_CONFIG;
    }
    NatNetConfig nc = {
        .multicast       = (d->multicast && d->multicast[0]) ? d->multicast : "239.255.42.99",
        .server          = (d->server && d->server[0]) ? d->server : NULL,
        .local_iface     = (d->local_iface && d->local_iface[0]) ? d->local_iface : NULL,
        .data_port       = d->data_port    ? d->data_port    : 1511,
        .command_port    = d->command_port ? d->command_port : 1510,
        .rigid_body      = d->rigid_body_id,
        .rigid_body_name = (d->rigid_body_name && d->rigid_body_name[0]) ? d->rigid_body_name : NULL,
        .major = (int)d->version_major, .minor = (int)d->version_minor,
    };
    NatNet* nn = natnet_open(&nc, e->errbuf, sizeof e->errbuf);
    if (!nn) {
        set_error(e, e->errbuf[0] ? e->errbuf : "bwa_tracker_connect: NatNet open failed");
        return BWA_ERR_TRACKER;
    }
    NatNet* old = e->tracker;
    e->tracker = nn;
    rt_set_tracker(e->rt, natnet_pose(nn));   /* release-published; the audio thread reads it next block */
    if (old) {
        if (e->started) Sleep(50);            /* an in-flight block may still read the old pose slot */
        natnet_close(old);
    }
    return BWA_OK;
}

void bwa_tracker_disconnect(bwa_engine* e) {
    if (!e || !e->tracker) return;
    NatNet* old = e->tracker;
    e->tracker = NULL;
    rt_set_tracker(e->rt, NULL);              /* back to the committed/pushed pose */
    if (e->started) Sleep(50);                /* an in-flight block may still read the old pose slot */
    natnet_close(old);
}

void bwa_destroy(bwa_engine* e) {
    if (!e) return;
    bwa_tracker_disconnect(e);                          /* tracker lifetime ends with the engine */
    engine_close_devices(e);                            /* stop audio + tear down the Steam decoder */
#ifdef BWA_HAVE_STEAMAUDIO
    steam_scene_destroy(e->scene);                      /* join the occlusion sim thread before rt is freed */
#endif
    monitor_destroy(e->monitor);
    rt_destroy(e->rt);
    free((void*)e->cfg.layout_path);                    /* owned copies from bwa_create */
    free((void*)e->cfg.hrtf_path);
    free((void*)e->cfg.asio_driver);
    free(e);
}

const char* bwa_last_error(bwa_engine* e) {
    return e ? e->last_error : NULL;
}

const char* bwa_get_audio_backend(bwa_engine* e) {
    if (!e || !e->sink) return "none";
    if (e->profile == BWA_PROFILE_BINAURAL || e->profile == BWA_PROFILE_BOTH) {
        /* name the monitor too: the HRTF decode falls back to the simple-pan monitor SILENTLY
         * (steam_monitor_create is non-fatal), and a by-ear report is meaningless without knowing
         * which of the two actually rendered it */
        snprintf(e->backend_buf, sizeof e->backend_buf, "%s (%s monitor)", bwa_sink_backend(e->sink),
                 e->steam ? "steam HRTF" : "simple-pan");
        return e->backend_buf;
    }
    return bwa_sink_backend(e->sink);   /* cave: the array device */
}

/* ---- assets ---- */

bwa_sound bwa_load_sound(bwa_engine* e, const char* path) {
    if (!e) return 0;
    clear_error(e);
    bwa_sound snd = rt_load_sound(e->rt, path, e->errbuf, sizeof e->errbuf);
    if (snd == 0) set_error(e, e->errbuf[0] ? e->errbuf : "bwa_load_sound: failed");
    return snd;
}
bwa_sound bwa_load_sound_streaming(bwa_engine* e, const char* path) {
    if (!e) return 0;
    clear_error(e);
    bwa_sound snd = rt_load_sound_streaming(e->rt, path, e->errbuf, sizeof e->errbuf);
    if (snd == 0) set_error(e, e->errbuf[0] ? e->errbuf : "bwa_load_sound_streaming: failed");
    return snd;
}

void bwa_unload_sound(bwa_engine* e, bwa_sound snd) {
    if (e) rt_unload_sound(e->rt, snd);   /* safe any time; retire-acked internally */
}

bwa_sound bwa_load_ambix(bwa_engine* e, const char* path) {
    if (!e) return 0;
    clear_error(e);
    bwa_sound snd = rt_load_ambix(e->rt, path, e->errbuf, sizeof e->errbuf);
    if (snd == 0) set_error(e, e->errbuf[0] ? e->errbuf : "bwa_load_ambix: failed");
    return snd;
}

bwa_sound bwa_load_fuma(bwa_engine* e, const char* path) {
    if (!e) return 0;
    clear_error(e);
    bwa_sound snd = rt_load_fuma(e->rt, path, e->errbuf, sizeof e->errbuf);
    if (snd == 0) set_error(e, e->errbuf[0] ? e->errbuf : "bwa_load_fuma: failed");
    return snd;
}

/* ---- ambisonic beds: a bed is a voice that plays a multichannel asset; mix_bed decodes it ---- */
bwa_bed bwa_bed_create(bwa_engine* e)                                { return bwa_source_create(e); }
void  bwa_bed_play(bwa_engine* e, bwa_bed b, bwa_sound snd, bool loop) {
    if (!e) return;
    if (rt_sound_channels(e->rt, snd) <= 1) {       /* a bed needs an ambisonic (multichannel) asset */
        set_error(e, "bwa_bed_play: asset is mono — load it with bwa_load_ambix (a 4/9/16-ch AmbiX file)");
        return;
    }
    if (refuse_push_play(e, b, "bwa_bed_play: push source (bwa_source_create_stream) — feed it with bwa_source_push"))
        return;
    rt_source_play(e->rt, b, snd, loop);            /* direct: bypass bwa_source_play's mono-only guard */
}
void  bwa_bed_set_gain(bwa_engine* e, bwa_bed b, float linear)       { bwa_source_set_gain(e, b, linear); }
void  bwa_bed_stop(bwa_engine* e, bwa_bed b)                         { bwa_source_stop(e, b); }
void  bwa_bed_destroy(bwa_engine* e, bwa_bed b)                      { bwa_source_destroy(e, b); }
/* the rest of the facade: same voice machinery, bed-named (see bw_audio.h — a bed IS a voice) */
void  bwa_bed_fade_to (bwa_engine* e, bwa_bed b, float gain, float seconds) { bwa_source_fade_to(e, b, gain, seconds); }
void  bwa_bed_fade_out(bwa_engine* e, bwa_bed b, float seconds)      { bwa_source_fade_out(e, b, seconds); }
void  bwa_bed_set_paused(bwa_engine* e, bwa_bed b, bool paused)      { bwa_source_set_paused(e, b, paused); }
void  bwa_bed_seek(bwa_engine* e, bwa_bed b, uint64_t frame)         { bwa_source_seek(e, b, frame); }
void  bwa_bed_set_priority(bwa_engine* e, bwa_bed b, int priority)   { bwa_source_set_priority(e, b, priority); }
void  bwa_bed_set_group(bwa_engine* e, bwa_bed b, uint32_t group)    { bwa_source_set_group(e, b, group); }
bool  bwa_bed_is_playing(bwa_engine* e, bwa_bed b)                   { return bwa_source_is_playing(e, b); }

/* ---- sources (forward to the rt core) ---- */

bwa_source bwa_source_create(bwa_engine* e) {
    return e ? rt_source_create(e->rt) : 0;
}
/* procedural (push) sources: the voice plays caller-pushed PCM (see bw_audio.h) */
bwa_source bwa_source_create_stream(bwa_engine* e) {
    if (!e) return 0;
    clear_error(e);
    bwa_source s = rt_source_create_stream(e->rt, e->errbuf, sizeof e->errbuf);
    if (s == 0) set_error(e, e->errbuf[0] ? e->errbuf : "bwa_source_create_stream: failed");
    return s;
}
uint32_t bwa_source_push(bwa_engine* e, bwa_source s, const float* frames, uint32_t n) {
    if (!e || !push_guard(e, s, "bwa_source_push: not a push source (create it with bwa_source_create_stream)")) return 0;
    return rt_source_push(e->rt, s, frames, n);
}
uint32_t bwa_source_push_space(bwa_engine* e, bwa_source s) {
    if (!e || !push_guard(e, s, "bwa_source_push_space: not a push source (create it with bwa_source_create_stream)")) return 0;
    return rt_source_push_space(e->rt, s);
}
void bwa_source_push_end(bwa_engine* e, bwa_source s) {
    if (e && push_guard(e, s, "bwa_source_push_end: not a push source (create it with bwa_source_create_stream)"))
        rt_source_push_end(e->rt, s);
}
void bwa_source_set_priority(bwa_engine* e, bwa_source s, int priority) {
    if (e) rt_source_set_priority(e->rt, s, priority);
}
void bwa_source_destroy(bwa_engine* e, bwa_source s) {
    if (!e) return;
#ifdef BWA_HAVE_STEAMAUDIO
    /* clear ALL scene features (occlusion + directivity) so the sim tears down this source's
     * IPLSource and stops simulating the slot (else it leaks + a recycled slot inherits stale state). */
    if (e->scene) steam_scene_source_gone(e->scene, s);
    if (e->path)  steam_path_set_source(e->path, s, (float[3]){0,0,0}, 0);   /* stop simulating this slot */
#endif
    rt_source_destroy(e->rt, s);
}
void bwa_source_set_pos(bwa_engine* e, bwa_source s, float x, float y, float z) {
    if (!e) return;
    rt_source_set_pos(e->rt, s, x, y, z);
    uint16_t idx = (uint16_t)(s & 0xFFFFu);
    if (idx < BWA_VOICE_CAP) { e->src_pos[idx][0]=x; e->src_pos[idx][1]=y; e->src_pos[idx][2]=z; }  /* so set_pathing has the pos */
#ifdef BWA_HAVE_STEAMAUDIO
    if (e->scene) steam_scene_set_pos(e->scene, s, x, y, z);   /* keep the occlusion sim in sync */
    if (e->path)  steam_path_set_pos(e->path, s, x, y, z);     /* keep the pathing sim in sync */
#endif
}
void bwa_source_set_gain(bwa_engine* e, bwa_source s, float linear)          { if (e) rt_source_set_gain(e->rt, s, linear); }
void bwa_source_play(bwa_engine* e, bwa_source s, bwa_sound snd, bool loop) {
    if (!e) return;
    if (rt_sound_channels(e->rt, snd) > 1) {        /* a multichannel asset is a bed, not a point source */
        set_error(e, "bwa_source_play: asset is multichannel — use bwa_bed_play (or bwa_load_sound for a point source)");
        return;
    }
    if (refuse_push_play(e, s, "bwa_source_play: push source (bwa_source_create_stream) — feed it with bwa_source_push"))
        return;
    rt_source_play(e->rt, s, snd, loop);
}
void bwa_source_play_at(bwa_engine* e, bwa_source s, bwa_sound snd, bool loop, uint64_t start_sample) {
    if (!e) return;
    if (rt_sound_channels(e->rt, snd) > 1) {
        set_error(e, "bwa_source_play_at: asset is multichannel — use a point source");
        return;
    }
    if (refuse_push_play(e, s, "bwa_source_play_at: push source (bwa_source_create_stream) — feed it with bwa_source_push"))
        return;
    rt_source_play_at(e->rt, s, snd, loop, start_sample);
}
uint64_t bwa_get_dsp_time(bwa_engine* e)                                     { return e ? rt_dsp_time(e->rt) : 0; }
void bwa_source_stop(bwa_engine* e, bwa_source s)                           { if (e) rt_source_stop(e->rt, s); }
void bwa_source_set_paused(bwa_engine* e, bwa_source s, bool paused)        { if (e) rt_source_set_paused(e->rt, s, paused); }
void bwa_source_seek(bwa_engine* e, bwa_source s, uint64_t frame)           { if (e) rt_source_seek(e->rt, s, frame); }
bool bwa_source_is_playing(bwa_engine* e, bwa_source s)                     { return e ? rt_source_is_playing(e->rt, s) : false; }
void bwa_set_test_signal(bwa_engine* e, uint32_t channel, bwa_test_kind kind, float gain) { if (e) rt_test_signal(e->rt, channel, (uint8_t)kind, gain); }

void bwa_set_panner(bwa_engine* e, bwa_panner panner) { if (e) { e->panner = (int)panner; rt_set_panner(e->rt, (int)panner); } }
void bwa_set_dual_band(bwa_engine* e, bool on)       { if (e) rt_set_dual_band(e->rt, on); }
void bwa_set_max_re(bwa_engine* e, bool on) {
    if (!e) return;
    e->max_re = on ? 1 : 0;                          /* staged for the FDN bwa_start may still create */
    rt_set_max_re(e->rt, e->max_re);
    if (e->fdn) fdn_set_max_re(e->fdn, e->max_re);   /* live: the FDN crossfades its render pair */
}
void bwa_set_spread_mode(bwa_engine* e, bwa_spread_mode mode) { if (e) rt_set_spread_mode(e->rt, (int)mode); }
void bwa_set_tracked_room_eq(bwa_engine* e, bool on)  { if (e) rt_set_room_eq_dyn(e->rt, on); }
void bwa_set_decorrelation(bwa_engine* e, bool on)    { if (e) rt_set_decorrelation(e->rt, on); }
void bwa_set_bed_renderer(bwa_engine* e, bwa_bed_renderer r) { if (e) rt_set_bed_renderer(e->rt, (int)r); }
void bwa_set_pose_prediction(bwa_engine* e, float lead_ms) { if (e) rt_set_pose_prediction(e->rt, lead_ms * 1e-3f); }
void bwa_set_near_spread(bwa_engine* e, float radius_m)    { if (e) rt_set_near_spread(e->rt, radius_m); }
void bwa_set_extra_listeners(bwa_engine* e, const float* xyz, uint32_t count) { if (e) rt_set_extra_listeners(e->rt, xyz, count); }
void bwa_source_set_loudness_comp(bwa_engine* e, bwa_source s, bool on) { if (e) rt_source_set_loudness_comp(e->rt, s, on); }
void bwa_source_set_size(bwa_engine* e, bwa_source s, float radius_m) { if (e) rt_source_set_size(e->rt, s, radius_m); }

void     bwa_set_master_gain(bwa_engine* e, float linear)  { if (e) rt_set_master_gain(e->rt, linear); }
void     bwa_set_paused(bwa_engine* e, bool paused)        { if (e) rt_set_all_paused(e->rt, paused); }
uint32_t bwa_get_active_voices(bwa_engine* e)              { return e ? rt_active_voices(e->rt) : 0; }
uint32_t bwa_get_channel_count(bwa_engine* e)                  { return e ? e->layout.count : 0; }
void bwa_set_output_capture(bwa_engine* e, bwa_output_fn cb, void* user) {
    if (!e) return;
    e->capture_user = user;      /* publish user BEFORE cb: the audio thread gates on cb (x64 store order + */
    e->capture_cb   = cb;        /* volatile), so a non-NULL cb is never seen with a stale/mismatched user */
}
const float* bwa_render_block(bwa_engine* e, uint32_t* channels, uint32_t* nframes) {
    if (!e) return NULL;
    if (!e->started) { set_error(e, "bwa_render_block: engine not started"); return NULL; }
    const float* out = bwa_sink_render_block(e->sink, channels, nframes);   /* NULL unless a MANUAL sink */
    if (!out) set_error(e, "bwa_render_block: requires bwa_desc.sink = BWA_SINK_MANUAL");
    return out;
}
void bwa_source_fade_to (bwa_engine* e, bwa_source s, float gain, float seconds) { if (e) rt_source_fade_to(e->rt, s, gain, seconds, false); }
void bwa_source_fade_out(bwa_engine* e, bwa_source s, float seconds)             { if (e) rt_source_fade_to(e->rt, s, 0.f, seconds, true); }
void bwa_source_set_group(bwa_engine* e, bwa_source s, uint32_t group)  { if (e) rt_source_set_group(e->rt, s, group); }
void bwa_group_set_gain  (bwa_engine* e, uint32_t group, float linear){ if (e) rt_group_set_gain(e->rt, group, linear); }
void bwa_group_set_paused(bwa_engine* e, uint32_t group, bool paused) { if (e) rt_group_set_paused(e->rt, group, paused); }
void bwa_source_set_pitch(bwa_engine* e, bwa_source s, float rate)      { if (e) rt_source_set_pitch(e->rt, s, rate); }
void bwa_bed_set_rotation(bwa_engine* e, bwa_bed b, float yaw_rad)      { if (e) rt_bed_set_rotation(e->rt, b, yaw_rad); }
void bwa_bed_set_orientation(bwa_engine* e, bwa_bed b, float yaw_rad, float pitch_rad, float roll_rad) {
    if (e) rt_bed_set_orientation(e->rt, b, yaw_rad, pitch_rad, roll_rad);
}

/* Manual occlusion: the same handle-gated, audio-thread-ramped publish path the Steam sim uses
 * (rt_set_occlusion / rt_set_occlusion_eq), driven from the control thread. Do not drive a source
 * from BOTH this and the sim (bwa_source_set_occlusion) — the sim republishes every tick and wins. */
void bwa_source_set_occlusion_manual(bwa_engine* e, bwa_source s, float level, const float bands[3]) {
    if (!e) return;
    if (level < 0.f) level = 0.f; else if (level > 1.f) level = 1.f;
    if (bands) {
        float b[3];
        for (int i = 0; i < 3; ++i) b[i] = bands[i] < 0.f ? 0.f : (bands[i] > 1.f ? 1.f : bands[i]);
        rt_set_occlusion_eq(e->rt, s, level, b);
    } else {
        rt_set_occlusion(e->rt, s, level);
    }
}
void bwa_set_limiter(bwa_engine* e, bool on)         { if (e) rt_set_limiter(e->rt, on); }
void bwa_set_limiter_ceiling(bwa_engine* e, float ceiling_db) {
    if (!e) return;
    if (ceiling_db > 0.f)   ceiling_db = 0.f;      /* the ceiling is a maximum, never a boost */
    if (ceiling_db < -60.f) ceiling_db = -60.f;
    rt_set_limiter_ceiling(e->rt, powf(10.f, ceiling_db / 20.f));
}

/* Offline: the chosen panner's per-speaker gains for `nsrc` source positions heard from one listener,
 * over a layout given as `n` speaker positions (3 floats each). Default DBAP/distance tuning. Shares
 * the SPCAP/VBAP per-listener cache across the batch, so it is efficient for grid evaluation. Writes
 * out[i*n + s]; returns nsrc. Not engine state — pure, for layout scoring/optimization in tools. */
uint32_t bwa_panner_gains_batch(bwa_panner panner, const float* positions, uint32_t n,
                               const float lis[3], const float* srcs, uint32_t nsrc, float* out) {
    if (!positions || !lis || !srcs || !out || n == 0 || n > BWA_CHANNELS || nsrc == 0) return 0;
    Layout L = layout_default();                         /* default rolloff_r / distance attenuation */
    L.count = n;
    for (uint32_t s = 0; s < n; ++s) {
        L.speakers[s].pos[0] = positions[s*3+0];
        L.speakers[s].pos[1] = positions[s*3+1];
        L.speakers[s].pos[2] = positions[s*3+2];
        L.speakers[s].gain_lin = 1.0f;
        L.speakers[s].delay_samples = 0;
    }
    layout_compute_ref(&L);          /* keep the struct coherent (the panners themselves are listener-relative) */
    SpcapState sp; VbapState vb;
    if (panner == BWA_PAN_SPCAP) spcap_reset(&sp);
    else if (panner == BWA_PAN_VBAP) vbap_reset(&vb);
    for (uint32_t i = 0; i < nsrc; ++i) {
        const float* src = &srcs[(size_t)i * 3];
        float* o = &out[(size_t)i * n];
        if (panner == BWA_PAN_SPCAP)     spcap_gains(&sp, src, lis, &L, 1u, 1.0f, o);  /* cache reused across the batch */
        else if (panner == BWA_PAN_VBAP) vbap_gains(&vb, src, lis, &L, 1u, 1.0f, o);
        else                            dbap_gains(src, lis, &L, 1.0f, o);
    }
    return nsrc;
}
uint32_t bwa_get_speakers(bwa_engine* e, float* xyz, uint32_t cap) {
    if (!e) return 0;
    uint32_t n = e->layout.count;
    if (xyz) {
        uint32_t m = (n < cap) ? n : cap;
        for (uint32_t i = 0; i < m; ++i) {
            xyz[i * 3 + 0] = e->layout.speakers[i].pos[0];
            xyz[i * 3 + 1] = e->layout.speakers[i].pos[1];
            xyz[i * 3 + 2] = e->layout.speakers[i].pos[2];
        }
    }
    return n;
}

uint32_t bwa_get_bus_levels(bwa_engine* e, float* peaks, uint32_t cap) {
    if (!e) return 0;
    return rt_bus_peaks(e->rt, peaks, cap);
}

void bwa_play_oneshot(bwa_engine* e, bwa_sound snd, float x, float y, float z, float gain) {
    if (!e) return;
    if (rt_sound_channels(e->rt, snd) > 1) {
        set_error(e, "bwa_play_oneshot: asset is multichannel — oneshots are point sources (use bwa_load_sound)");
        return;
    }
    rt_play_oneshot(e->rt, snd, x, y, z, gain);
}

/* ---- materials / occlusion (no-ops without the Steam Audio backend) ---- */

#ifdef BWA_HAVE_STEAMAUDIO
/* Geometry can change at runtime. The occlusion sim owns the IPLScene and serializes commits on its
 * own thread; the borrowing reflection/pathing sims take the scene lock SHARED around their ray traces
 * (steam_scene_ray_lock), so an iplSceneCommit can no longer race a RunReflections/RunPathing. The one
 * caveat is BAKED reflections/pathing: the bake froze the geometry, so a runtime change won't move the
 * baked reverb/paths (real-time reflections + occlusion track it fine). "Locked" now means only "no
 * scene" (no SDK, or a failed create). */
static int scene_locked(bwa_engine* e) {
    return (!e || !e->scene);
}
#endif

/* Clamp a coefficient to [0,1] and sanitize non-finite input (NaN/Inf -> 0): the `!(x>=0)` test is
 * true for NaN and negatives, so both map to 0 — keeping garbage out of phonon's ray/reverb math. */
static float clamp01(float x) { if (!(x >= 0.f)) return 0.f; return (x > 1.f) ? 1.f : x; }

/* Material tokens are plain control-thread state — the table exists with or without the SDK, so
 * minting works regardless; the mesh setters that consume the tokens are the SDK-gated no-ops. */
bwa_material bwa_material_define(bwa_engine* e, const float absorption[3], float scattering, const float transmission[3]) {
    if (!e) return 0;
    if (!absorption || !transmission) { set_error(e, "bwa_material_define: NULL coefficients"); return 0; }
    uint32_t i = 0; int found = 0;
    for (uint32_t k = 1; k < e->num_materials; ++k)          /* reuse a released slot before growing (skip 0 = default) */
        if (e->mat_free[k]) { i = k; found = 1; break; }
    if (!found) {
        if (e->num_materials >= BWA_MAX_MATERIALS) { set_error(e, "bwa_material_define: material table full"); return 0; }
        i = e->num_materials++;
    }
    e->mat_free[i] = 0;
    for (int b = 0; b < 3; ++b) { e->materials[i].absorption[b] = clamp01(absorption[b]); e->materials[i].transmission[b] = clamp01(transmission[b]); }
    e->materials[i].scattering = clamp01(scattering);
    return (bwa_material)i;
}

/* Release a token so its slot can be reused. Caller-managed lifetime (like free()): only release a
 * token no live mesh/source still references — already-set meshes copied the coefficients at set time,
 * so they're unaffected, but a FUTURE mesh set with a released token gets whatever the slot is reused
 * for. Token 0 (the built-in default) and out-of-range tokens are refused. */
void bwa_material_release(bwa_engine* e, bwa_material token) {
    if (!e) return;
    uint32_t t = (uint32_t)token;
    if (t == 0)                 { set_error(e, "bwa_material_release: token 0 is the built-in default (can't release)"); return; }
    if (t >= e->num_materials)  { set_error(e, "bwa_material_release: token out of range"); return; }
    e->mat_free[t] = 1;
}

bwa_material bwa_material_preset(bwa_engine* e, bwa_material_type preset) {
    if (!e) return 0;
    if ((unsigned)preset >= sizeof BWA_PRESETS / sizeof BWA_PRESETS[0]) {
        set_error(e, "bwa_material_preset: unknown preset value");
        return 0;
    }
    /* GENERIC (preset 0) IS the built-in default — return the canonical token 0 rather than
     * minting a duplicate slot (callers tag many surfaces with it; don't burn the table). */
    return (preset == BWA_MAT_GENERIC) ? 0
                    : bwa_material_define(e, BWA_PRESETS[preset].absorption, BWA_PRESETS[preset].scattering, BWA_PRESETS[preset].transmission);
}

void bwa_scene_set_mesh_mat(bwa_engine* e, const float* verts, int nverts, const int* tris, int ntris,
                           const bwa_material* tri_material) {
#ifdef BWA_HAVE_STEAMAUDIO
    if (scene_locked(e)) return;
    uint32_t nmat = e->num_materials;                    /* flatten the table to the arrays steam_scene wants */
    float absorption[BWA_MAX_MATERIALS*3], transmission[BWA_MAX_MATERIALS*3], scattering[BWA_MAX_MATERIALS];
    for (uint32_t k = 0; k < nmat; ++k) {
        for (int b = 0; b < 3; ++b) { absorption[k*3+b] = e->materials[k].absorption[b]; transmission[k*3+b] = e->materials[k].transmission[b]; }
        scattering[k] = e->materials[k].scattering;
    }
    /* bwa_material tokens ARE the indices (uint32 -> int, small values); steam_scene clamps any out-of-range. */
    steam_scene_set_mesh_mat(e->scene, verts, nverts, tris, ntris, (int)nmat,
                             absorption, scattering, transmission, (const int*)tri_material);
#else
    (void)e; (void)verts; (void)nverts; (void)tris; (void)ntris; (void)tri_material;
#endif
}

#ifdef BWA_HAVE_STEAMAUDIO
/* Emit triangle (i0,i1,i2) into tris[*n], flipping the last two indices if needed so the face normal
 * points toward `ctr` (the box centre — inward; the listener is inside). Testing toward the ORIGIN
 * would degenerate for a floor-based box, whose bottom face contains the origin. */
static void emit_inward(const float* v, const float ctr[3], int* tris, int* n, int i0, int i1, int i2) {
    const float *p0 = &v[i0*3], *p1 = &v[i1*3], *p2 = &v[i2*3];
    float e1x = p1[0]-p0[0], e1y = p1[1]-p0[1], e1z = p1[2]-p0[2];
    float e2x = p2[0]-p0[0], e2y = p2[1]-p0[1], e2z = p2[2]-p0[2];
    float nx = e1y*e2z - e1z*e2y, ny = e1z*e2x - e1x*e2z, nz = e1x*e2y - e1y*e2x;
    float cx = (p0[0]+p1[0]+p2[0])/3.f, cy = (p0[1]+p1[1]+p2[1])/3.f, cz = (p0[2]+p1[2]+p2[2])/3.f;
    if (nx*(ctr[0]-cx) + ny*(ctr[1]-cy) + nz*(ctr[2]-cz) < 0.f) { int tmp = i1; i1 = i2; i2 = tmp; }   /* outward -> flip */
    tris[*n*3+0] = i0; tris[*n*3+1] = i1; tris[*n*3+2] = i2; (*n)++;
}
#endif

void bwa_scene_set_box(bwa_engine* e, float w, float h, float d, const bwa_material faces[6]) {
    if (!e) return;
    if (!(w > 0.f) || !(h > 0.f) || !(d > 0.f)) {   /* reject zero/negative/NaN dims (degenerate triangles) */
        set_error(e, "bwa_scene_set_box: w/h/d must be positive");
        return;
    }
    /* The box is captured for the IMAGE-SOURCE reflections (ism.c) whether or not the Steam Audio
     * build is present — the same call configures the ray-traced scene (with SDK) and the geometric
     * early reflections (always), so a no-SDK build still knows the room it is in. */
    e->ism_room.w = w; e->ism_room.h = h; e->ism_room.d = d;
    for (int f = 0; f < 6; ++f) {
        uint32_t m = faces ? faces[f] : 0;
        if (m >= e->num_materials) m = 0;
        for (int b = 0; b < 3; ++b) e->ism_room.absorb[f][b] = e->materials[m].absorption[b];
    }
    e->ism_room.valid = 1;
    rt_set_ism_room(e->rt, &e->ism_room);          /* audio thread is stopped (scene setup is load-time) */
#ifdef BWA_HAVE_STEAMAUDIO
    if (scene_locked(e)) return;
    /* floor-based: x/z centred on the origin, y from 0 (the floor, where the room origin
     * canonically sits) up to h — so a listener at ear height stands inside the box */
    float hw = w*0.5f, hd = d*0.5f;
    float verts[8*3] = {
        -hw, 0.f,-hd,   hw, 0.f,-hd,   hw, h,-hd,  -hw, h,-hd,
        -hw, 0.f, hd,   hw, 0.f, hd,   hw, h, hd,  -hw, h, hd };
    static const int quad[6][4] = {            /* face order: -x,+x,-y,+y,-z,+z (matches faces[6]) */
        {0,4,7,3}, {1,2,6,5}, {0,1,5,4}, {3,7,6,2}, {0,3,2,1}, {4,5,6,7} };
    int tris[12*3]; bwa_material tri_mat[12]; int n = 0;
    const float ctr[3] = { 0.f, h * 0.5f, 0.f };
    for (int f = 0; f < 6; ++f) {
        bwa_material m = faces ? faces[f] : 0;
        int a = quad[f][0], b = quad[f][1], c = quad[f][2], dd = quad[f][3];
        emit_inward(verts, ctr, tris, &n, a, b, c);  tri_mat[n-1] = m;
        emit_inward(verts, ctr, tris, &n, a, c, dd); tri_mat[n-1] = m;
    }
    bwa_scene_set_mesh_mat(e, verts, 8, tris, 12, tri_mat);
#endif
}

/* ---- dynamic (movable) occluders/reflectors (instanced meshes; SDK-gated no-ops otherwise) ---- */

int bwa_scene_add_dynamic_mesh(bwa_engine* e, const float* verts, int nverts, const int* tris, int ntris,
                               bwa_material material) {
#ifdef BWA_HAVE_STEAMAUDIO
    if (!e) return -1;
    if (!e->scene) { set_error(e, "bwa_scene_add_dynamic_mesh: no scene (needs the Steam Audio backend)"); return -1; }
    uint32_t m = material; if (m >= e->num_materials) m = 0;      /* out-of-range token -> default */
    int h = steam_scene_add_dynamic_mesh(e->scene, verts, nverts, tris, ntris,
                                         e->materials[m].absorption, e->materials[m].scattering, e->materials[m].transmission);
    if (h < 0) set_error(e, "bwa_scene_add_dynamic_mesh: invalid geometry or the movable-mesh table is full");
    return h;
#else
    (void)e; (void)verts; (void)nverts; (void)tris; (void)ntris; (void)material; return -1;
#endif
}

void bwa_scene_set_dynamic_transform(bwa_engine* e, int handle, float x, float y, float z,
                                     float qx, float qy, float qz, float qw) {
#ifdef BWA_HAVE_STEAMAUDIO
    if (!e || !e->scene) return;
    /* rigid local-to-room affine: rotation from the (normalized) quaternion + translation, row-major
     * to match phonon's IPLMatrix4x4. The Unity binding delivers room-space pos/quat (Room.Pos/Rot). */
    float n = qx*qx + qy*qy + qz*qz + qw*qw, s = (n > 1e-12f) ? 2.0f / n : 0.0f;
    float xs = qx*s, ys = qy*s, zs = qz*s;
    float wx = qw*xs, wy = qw*ys, wz = qw*zs, xx = qx*xs, xy = qx*ys, xz = qx*zs, yy = qy*ys, yz = qy*zs, zz = qz*zs;
    float m16[16] = {
        1.f-(yy+zz), xy-wz,       xz+wy,       x,
        xy+wz,       1.f-(xx+zz), yz-wx,       y,
        xz-wy,       yz+wx,       1.f-(xx+yy), z,
        0.f,         0.f,         0.f,         1.f };
    steam_scene_set_dynamic_transform(e->scene, handle, m16);
#else
    (void)e; (void)handle; (void)x; (void)y; (void)z; (void)qx; (void)qy; (void)qz; (void)qw;
#endif
}

void bwa_scene_remove_dynamic_mesh(bwa_engine* e, int handle) {
#ifdef BWA_HAVE_STEAMAUDIO
    if (e && e->scene) steam_scene_remove_dynamic_mesh(e->scene, handle);
#else
    (void)e; (void)handle;
#endif
}

/* ---- image-source early reflections (phonon-free; see ism.h) ---- */
void bwa_source_set_early_reflections(bwa_engine* e, bwa_source s, bool on) {
    if (!e) return;
    if (on && !e->ism_room.valid) {
        set_error(e, "bwa_source_set_early_reflections: no room — call bwa_scene_set_box first");
        return;
    }
#ifdef BWA_HAVE_STEAMAUDIO
    /* the Steam reflection bed ALREADY contains early reflections — running both renders them twice.
     * Warn once (this is a per-frame call; don't spam), and let it through: the caller may be
     * deliberately A/B-ing the two. The recommended pairing is Steam scene + ISM + FDN, which never
     * creates the Steam bed at all (docs/materials.md). */
    if (on && e->reflect && !e->ism_warned) {
        e->ism_warned = 1;
        set_error(e, "bwa_source_set_early_reflections: the Steam reflection bed is running and already "
                     "renders early reflections — you will hear them twice. Use the FDN (bwa_fdn_config; "
                     "it takes the reverb tap instead) for the late tail, or drop the ISM.");
    }
#endif
    rt_source_set_ism(e->rt, s, on);
}
void bwa_early_reflections_set_gain(bwa_engine* e, float linear) { if (e) rt_set_ism_gain(e->rt, linear); }

void bwa_source_set_occlusion(bwa_engine* e, bwa_source s, bool on) {
#ifdef BWA_HAVE_STEAMAUDIO
    if (e && e->scene) steam_scene_set_occlusion(e->scene, s, on);
#else
    (void)e; (void)s; (void)on;
#endif
}

float bwa_source_get_occlusion(bwa_engine* e, bwa_source s) {
    return e ? rt_get_occlusion(e->rt, s) : 1.0f;
}

void bwa_reflections_config(bwa_engine* e, const bwa_reflections_desc* cfg) {
    if (!e || !cfg) return;
    if (e->started) {   /* load-time only: the bed's IR length + order are baked at bwa_start, so a
                         * post-start config would silently do nothing until the next stop/start */
        set_error(e, "bwa_reflections_config: load-time only (call between bwa_create and bwa_start)");
        return;
    }
    e->refl_cfg = *cfg;                                         /* applied at bwa_start; zero -> defaults */
    if (e->refl_cfg.ir_seconds <= 0.f) e->refl_cfg.ir_seconds = 1.0f;
    if (e->refl_cfg.order == 0)        e->refl_cfg.order       = 1;
    if (e->refl_cfg.order > 2)         e->refl_cfg.order       = 2;   /* v1: order 1 or 2 (3 = 16ch is heavy) */
    if (e->refl_cfg.num_rays == 0)     e->refl_cfg.num_rays    = 4096;
    if (e->refl_cfg.num_bounces == 0)  e->refl_cfg.num_bounces = 16;
}

/* The ONE reverb wet control: stored on the engine (so a pre-start value seeds whichever bed
 * bwa_start creates) and forwarded live to whichever bed owns the tap. */
void bwa_reflections_set_gain(bwa_engine* e, float linear) {
    if (!e) return;
    e->refl_wet = (linear < 0.f) ? 0.f : linear;
#ifdef BWA_HAVE_STEAMAUDIO
    if (e->reflect) steam_reflect_set_gain(e->reflect, e->refl_wet);
#endif
    if (e->fdn) fdn_set_gain(e->fdn, e->refl_wet);   /* the FDN is "the reflection bed" when enabled */
}

/* ---- directional FDN reverb bed (phonon-free; see fdn.h) ---- */
void bwa_fdn_config(bwa_engine* e, const bwa_fdn_desc* cfg) {
    if (!e || !cfg) return;
    if (e->started) {   /* load-time only: the FDN's lines + decay filters are built at bwa_start */
        set_error(e, "bwa_fdn_config: load-time only (call between bwa_create and bwa_start)");
        return;
    }
    e->fdn_cfg = *cfg;                                          /* applied at bwa_start; zero -> defaults */
    if (e->fdn_cfg.rt60_low_s  <= 0.f) e->fdn_cfg.rt60_low_s  = 1.2f;
    if (e->fdn_cfg.rt60_high_s <= 0.f) e->fdn_cfg.rt60_high_s = 0.7f;
    if (e->fdn_cfg.xover_hz    <= 0.f) e->fdn_cfg.xover_hz    = 2000.f;
    /* out-of-range decay/direction values reach fdn_set_decay(_direction)'s own clamps at start */
}

void bwa_source_set_reflections(bwa_engine* e, bwa_source s, bool on) {
    if (e) rt_source_set_reflections(e->rt, s, on);             /* phonon-free; the tap consumes the send */
}

void bwa_source_set_pathing(bwa_engine* e, bwa_source s, bool on) {
    if (!e) return;
    rt_source_set_pathing(e->rt, s, on);                        /* gate the voice into the indirect render */
#ifdef BWA_HAVE_STEAMAUDIO
    if (e->path) {                                              /* register/enable in the sim with the tracked pos */
        uint16_t idx = (uint16_t)(s & 0xFFFFu);
        float zero[3] = {0,0,0};
        steam_path_set_source(e->path, s, (idx < BWA_VOICE_CAP) ? e->src_pos[idx] : zero, on);
    }
#else
    (void)on;
#endif
}

void bwa_source_set_reflection_send(bwa_engine* e, bwa_source s, float gain) {
    if (e) rt_source_set_reflection_send(e->rt, s, gain);
}

void bwa_source_set_reflection_distance(bwa_engine* e, bwa_source s, bool on) {
    if (e) rt_source_set_reflection_distance(e->rt, s, on);
}

void bwa_source_set_doppler(bwa_engine* e, bwa_source s, bool on) {
    if (e) rt_source_set_doppler(e->rt, s, on);
}

void bwa_source_set_air_absorption(bwa_engine* e, bwa_source s, bool on) {
    if (e) rt_source_set_air_absorption(e->rt, s, on);
}

void bwa_source_set_spread(bwa_engine* e, bwa_source s, float amount) {
    if (e) rt_source_set_spread(e->rt, s, amount);
}

void bwa_source_set_orientation(bwa_engine* e, bwa_source s, float qx, float qy, float qz, float qw) {
#ifdef BWA_HAVE_STEAMAUDIO
    if (!e || !e->scene) return;
    /* the dipole axis is the source forward = q rotating the room frame's identity ahead */
    float q4[4] = { qx, qy, qz, qw }, f[3];
    frame_qrot(q4, BWA_ROOM_AHEAD, f);
    steam_scene_set_orientation(e->scene, s, f[0], f[1], f[2]);
#else
    (void)e; (void)s; (void)qx; (void)qy; (void)qz; (void)qw;
#endif
}

void bwa_source_set_directivity(bwa_engine* e, bwa_source s, float weight, float power) {
#ifdef BWA_HAVE_STEAMAUDIO
    if (e && e->scene) steam_scene_set_directivity(e->scene, s, weight, power);
#else
    (void)e; (void)s; (void)weight; (void)power;
#endif
}

void bwa_source_set_directivity_preset(bwa_engine* e, bwa_source s, bwa_directivity pattern) {
    float weight = 0.0f, power = 1.0f;
    if      (pattern == BWA_DIR_CARDIOID) weight = 0.5f;
    else if (pattern == BWA_DIR_FIGURE8)  weight = 1.0f;   /* OMNI -> weight 0 (off) */
    bwa_source_set_directivity(e, s, weight, power);
}

float bwa_source_get_directivity(bwa_engine* e, bwa_source s) {
    return e ? rt_get_directivity(e->rt, s) : 1.0f;
}

/* ---- listener ---- */

void bwa_set_listener_pose(bwa_engine* e, float px, float py, float pz,
                                       float qx, float qy, float qz, float qw) {
    if (!e) return;
    const float p[3] = { px, py, pz };
    const float q[4] = { qx, qy, qz, qw };
    rt_set_listener(e->rt, p, q);
}

void bwa_get_listener_pose(bwa_engine* e, float p[3], float q[4]) {
    if (!e || !p || !q) return;
    rt_read_pose(e->rt, p, q);
}

/* ---- frame boundary ---- */

void bwa_commit(bwa_engine* e) {
    if (e) rt_commit(e->rt);
}
