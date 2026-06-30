/*
 * engine.c — public C ABI: lifecycle, the device sink, and the control-thread side of
 * the API. The real-time machinery (rings, voice table, commit snapshot, mixing) lives
 * in rt.c behind sink.h's render callback; engine.c forwards the per-frame bw_* calls to
 * it. M0 = builds/links; M1 = device sink + audio loop; M2 = the concurrency spine.
 * Sounds (bw_load_sound) are still stubs until M3.
 */
#include "bwaudio.h"
#include "sink.h"
#include "rt.h"
#include "layout.h"
#include "dbap.h"          /* offline panner evaluation (bw_panner_gains_batch) */
#include "spcap.h"
#include "vbap.h"
#include "binaural.h"
#include "natnet.h"
#include "steam_decode.h"   /* phonon-free interfaces; impls linked only when BW_HAVE_STEAMAUDIO */
#include "profile.h"
#include "steam_scene.h"
#include "steam_reflect.h"

#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>           /* InterlockedExchange for the 'both' buffer handoff */

#define BW_VOICE_CAP 256       /* max simultaneous sources */
#define BW_SOUND_CAP 256       /* max loaded sounds */
#define BW_MAX_BLOCK 8192      /* upper bound on a device's frames-per-block (ASIO picks its own) */
#define BW_MAX_MATERIALS 64    /* material-table capacity (token == index; [0] = default) */

/* Named material presets. Coefficients are Steam Audio's published example materials: 3-band
 * absorption (low/mid/high), scattering, 3-band transmission. The default (index 0) is "generic". */
static const struct { const char* name; float absorption[3], scattering, transmission[3]; } BW_PRESETS[] = {
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
 * its render scratch to BW_MAX_BLOCK and renders whatever block the device dictates, so it works
 * with any driver. The 'both' double-buffer still assumes the two devices share cfg.block_size (a
 * mismatched monitor block is silenced; the array renders any size). Live headphone output works
 * through a 2-ch ASIO driver (ASIO4ALL / FlexASIO / the Steinberg built-in); WASAPI is future. */
struct BwEngine {
    BwConfig    cfg;
    int         started;
    const char* last_error;        /* points at errbuf or a literal; NULL when clean */
    char        errbuf[256];
    BwProfile   profile;

    RtCore*     rt;               /* rings + voice/sound tables + mixer (rt.c) */
    Monitor*    monitor;          /* binaural/both: 26->stereo decode */
    Layout      layout;           /* effective speaker geometry (for the Steam decoder at start) */
    uint32_t    cap;              /* scratch capacity in frames (== cfg.block_size) */

    BwSink*     sink;             /* primary: cave 26ch / binaural 2ch / both array 26ch */
    float*      scratch26;        /* binaural: 26-ch array render before the monitor */

    BwSink*     sink_mon;         /* both: the monitor (2-ch) device */
    float*      mon_buf[2];       /* both: stereo double-buffer (each 2*cap), array thread -> monitor thread */
    volatile LONG mon_idx;        /* both: index of the latest published buffer */

    NatNet*     tracker;          /* track_internal: NatNet pose ingest (NULL otherwise) */
    SteamMonitor* steam;          /* production HRTF decode (binaural/both); NULL = first-cut pan */
    SteamScene*   scene;          /* materials occlusion sim (off-thread); NULL without the SDK */
    SteamReflect* reflect;        /* reflection bed (created at bw_start if configured); NULL otherwise */
    BwReflectionConfig refl_cfg;  /* set via bw_reflections_config before bw_start */

    /* material table (control-thread): token == index; [0] is the built-in default. Coefficients are
     * resolved to per-triangle materials when a mesh is set. */
    struct { float absorption[3], scattering, transmission[3]; } materials[BW_MAX_MATERIALS];
    uint32_t      num_materials;
};

static void set_error(BwEngine* e, const char* msg) {
    if (!e) return;
    if (msg && msg != e->errbuf) { strncpy(e->errbuf, msg, sizeof e->errbuf - 1); e->errbuf[sizeof e->errbuf - 1] = 0; }
    e->last_error = (msg && e->errbuf[0]) ? e->errbuf : msg;
}

/* env helpers for the track_internal (NatNet) config — keeps the NatNet wiring out of BwConfig */
static const char* env_or(const char* name, const char* def) { const char* v = getenv(name); return (v && v[0]) ? v : def; }
static uint16_t    env_u16(const char* name, uint16_t def)    {
    const char* v = getenv(name);
    if (!v || !v[0]) return def;
    unsigned long u = strtoul(v, NULL, 10);
    return (u == 0 || u > 65535) ? def : (uint16_t)u;   /* out-of-range port -> default, not a truncation */
}

/* cave: the 26-ch array goes straight to the device. */
static void render_cave(void* user, float* dev, uint32_t n, const BwTimestamp* ts) {
    rt_render(((BwEngine*)user)->rt, dev, n, ts);
}

/* binaural: render the 26-ch array to scratch, then decode to the 2-ch device. The scratch is
 * sized to BW_MAX_BLOCK, so any device block size works (the binaural decode uses n internally
 * and consistently — no cross-buffer offset to keep in sync, unlike 'both'). */
static void render_binaural(void* user, float* dev2, uint32_t n, const BwTimestamp* ts) {
    BwEngine* e = (BwEngine*)user;
    if (n > BW_MAX_BLOCK) { memset(dev2, 0, sizeof(float) * (size_t)n * 2); return; }
    rt_render(e->rt, e->scratch26, n, ts);
    float p[3], q[4];
    rt_get_listener(e->rt, p, q);
    BW_ZONE_BEGIN(zbin, "binaural decode");                /* 26 virtual speakers -> HRTF -> 2 ch */
#ifdef BW_HAVE_STEAMAUDIO
    if (e->steam) { steam_monitor_process(e->steam, e->scratch26, p, q, dev2, n); BW_ZONE_END(zbin); return; }
#endif
    monitor_process(e->monitor, e->scratch26, p, q, dev2, n);
    BW_ZONE_END(zbin);
}

/* both, array thread: render the array to the 26-ch device (any block size), then — when the
 * block matches cap — decode the monitor into the back buffer and publish it. */
static void render_both_array(void* user, float* dev26, uint32_t n, const BwTimestamp* ts) {
    BwEngine* e = (BwEngine*)user;
    rt_render(e->rt, dev26, n, ts);
    if (n != e->cap) return;                            /* off-spec block: skip the fixed-size monitor publish */
    LONG cur = e->mon_idx;                              /* producer is the sole writer of mon_idx */
    float p[3], q[4];
    rt_get_listener(e->rt, p, q);
#ifdef BW_HAVE_STEAMAUDIO
    if (e->steam) steam_monitor_process(e->steam, dev26, p, q, e->mon_buf[1 - cur], n);
    else
#endif
    monitor_process(e->monitor, dev26, p, q, e->mon_buf[1 - cur], n);
    InterlockedExchange(&e->mon_idx, 1 - cur);          /* publish the back buffer (full barrier) */
}

/* both, monitor thread: play the latest published stereo buffer. */
static void render_both_monitor(void* user, float* dev2, uint32_t n, const BwTimestamp* ts) {
    BwEngine* e = (BwEngine*)user;
    (void)ts;
    memset(dev2, 0, sizeof(float) * (size_t)n * 2);
    if (n != e->cap) return;                            /* off-spec block: silence */
    LONG cur = InterlockedCompareExchange(&e->mon_idx, 0, 0);  /* atomic acquire read of the index */
    const float* src = e->mon_buf[cur];                 /* planar [L(cap), R(cap)], cap == n here */
    memcpy(dev2,     src,          sizeof(float) * n);  /* L */
    memcpy(dev2 + n, src + e->cap, sizeof(float) * n);  /* R */
}

/* ---- lifecycle ---- */

BwEngine* bw_create(const BwConfig* cfg) {
    if (!cfg) return NULL;
    BwEngine* e = (BwEngine*)calloc(1, sizeof *e);
    if (!e) return NULL;
    e->cfg = *cfg;
    if (e->cfg.sample_rate == 0) e->cfg.sample_rate = 48000;   /* sane defaults */
    if (e->cfg.block_size  == 0) e->cfg.block_size  = 256;
    e->profile = e->cfg.profile;
    e->rt = rt_create(BW_VOICE_CAP, BW_SOUND_CAP, e->cfg.sample_rate, BW_CHANNELS);
    if (!e->rt) { free(e); return NULL; }

    /* material table: token 0 is always the built-in "generic" default (BW_PRESETS[0]). */
    for (int b = 0; b < 3; ++b) {
        e->materials[0].absorption[b]   = BW_PRESETS[0].absorption[b];
        e->materials[0].transmission[b] = BW_PRESETS[0].transmission[b];
    }
    e->materials[0].scattering = BW_PRESETS[0].scattering;
    e->num_materials = 1;

    /* Load the surveyed speaker geometry if given; otherwise keep the default grid. A load
     * failure is non-fatal (usable with the default layout) but surfaces via bw_last_error.
     * Done here, before bw_start, so the audio thread isn't running. L is the effective
     * layout (default or loaded) — the binaural monitor needs the same geometry. */
    Layout L = layout_default();
    if (e->cfg.layout_path && e->cfg.layout_path[0]) {
        if (layout_load(e->cfg.layout_path, e->cfg.sample_rate, &L, e->errbuf, sizeof e->errbuf))
            rt_set_layout(e->rt, &L);
        else
            set_error(e, e->errbuf[0] ? e->errbuf : "bw_create: layout load failed");
    }
    e->layout = L;                                  /* kept for the Steam decoder, built at bw_start */
    if (e->profile == BW_PROFILE_BINAURAL || e->profile == BW_PROFILE_BOTH) {
        e->monitor = monitor_create(&L, e->cfg.sample_rate);
        if (!e->monitor) { rt_destroy(e->rt); free(e); return NULL; }
    }
#ifdef BW_HAVE_STEAMAUDIO
    /* materials occlusion sim (off-thread); non-fatal — occlusion is just unavailable if it fails */
    e->scene = steam_scene_create(e->rt, e->cfg.sample_rate, e->cfg.block_size, BW_VOICE_CAP);
#endif
    /* OWN the path strings: the caller's buffers (e.g. a P/Invoke binding's marshalled UTF-8 temporaries)
     * may be freed once bw_create returns, but bw_start dereferences hrtf_path later — a shallow pointer
     * copy would dangle. Copy here, free in bw_destroy. _strdup-fail -> NULL = the documented default. */
    e->cfg.layout_path = cfg->layout_path ? _strdup(cfg->layout_path) : NULL;
    e->cfg.hrtf_path   = cfg->hrtf_path   ? _strdup(cfg->hrtf_path)   : NULL;
    return e;
}

/* Close any open device(s) and free the start-time scratch. Monitor sink (consumer) first,
 * then the array sink (producer), so the monitor thread stops reading mon_buf before we free
 * it. Each bw_sink_close joins its audio thread. */
static void engine_close_devices(BwEngine* e) {
    if (e->sink_mon) { bw_sink_close(e->sink_mon); e->sink_mon = NULL; }
    if (e->sink)     { bw_sink_close(e->sink);     e->sink     = NULL; }
#ifdef BW_HAVE_STEAMAUDIO
    /* after the audio thread joins (sinks closed above): unregister the tap, then destroy the bed —
     * its IR aliases the bed source, so it must die before steam_scene_destroy (bw_destroy, later). */
    if (e->reflect)  { rt_set_bus_tap(e->rt, NULL, NULL); steam_reflect_destroy(e->reflect); e->reflect = NULL; }
    if (e->steam)    { steam_monitor_destroy(e->steam); e->steam = NULL; }   /* after the audio thread joins */
#endif
    if (e->tracker)  { rt_set_tracker(e->rt, NULL); natnet_close(e->tracker); e->tracker = NULL; }  /* after audio stops */
    free(e->scratch26);  e->scratch26  = NULL;
    free(e->mon_buf[0]); e->mon_buf[0] = NULL;
    free(e->mon_buf[1]); e->mon_buf[1] = NULL;
}

int bw_start(BwEngine* e) {
    if (!e) return 1;                                  /* BW_ERR_CONFIG (docs/api.md) */
    if (e->started) return 0;
    e->errbuf[0] = 0; e->last_error = NULL;
    const uint32_t sr = e->cfg.sample_rate, bs = e->cfg.block_size;
    e->cap = bs;

    if (e->profile == BW_PROFILE_CAVE) {
        e->sink = bw_sink_open(sr, bs, BW_CHANNELS, render_cave, e, e->errbuf, sizeof e->errbuf);
    } else if (e->profile == BW_PROFILE_BINAURAL) {
        e->scratch26 = (float*)calloc((size_t)BW_MAX_BLOCK * BW_CHANNELS, sizeof(float));
        if (e->scratch26)
            e->sink = bw_sink_open(sr, bs, 2, render_binaural, e, e->errbuf, sizeof e->errbuf);
    } else { /* both: a 26-ch array sink + a 2-ch monitor sink sharing a double-buffer */
        e->mon_buf[0] = (float*)calloc((size_t)bs * 2, sizeof(float));
        e->mon_buf[1] = (float*)calloc((size_t)bs * 2, sizeof(float));
        e->mon_idx = 0;
        if (e->mon_buf[0] && e->mon_buf[1]) {
            e->sink = bw_sink_open(sr, bs, BW_CHANNELS, render_both_array, e, e->errbuf, sizeof e->errbuf);
            if (e->sink)
                e->sink_mon = bw_sink_open(sr, bs, 2, render_both_monitor, e, e->errbuf, sizeof e->errbuf);
        }
    }

    if (!e->sink || (e->profile == BW_PROFILE_BOTH && !e->sink_mon)) {
        set_error(e, e->errbuf[0] ? e->errbuf : "bw_start: device open failed");
        engine_close_devices(e);
        return 2;                                       /* BW_ERR_DEVICE */
    }

#ifdef BW_HAVE_STEAMAUDIO
    /* production HRTF decode for binaural/both. phonon's effect frameSize is fixed at create and
     * must equal the device block n, which the ASIO driver dictates (!= cfg.block_size in general)
     * — so build it now that the sink is open and its real block size is known. Non-fatal: render
     * falls back to the simple-pan monitor if NULL. Torn down in engine_close_devices. */
    if (e->profile == BW_PROFILE_BINAURAL || e->profile == BW_PROFILE_BOTH)
        e->steam = steam_monitor_create(&e->layout, e->cfg.sample_rate,
                                        bw_sink_block_size(e->sink), e->cfg.hrtf_path);

    /* reflection bed: a separate reflections sim + the audio-thread convolution registered as the rt
     * bus tap. Same frameSize-fixed-at-create reason as the monitor — build it now. Non-fatal: if it
     * fails, no tap is registered and the engine runs dry. Needs the occlusion scene (shared geometry). */
    if (e->scene && e->refl_cfg.enabled) {
        e->reflect = steam_reflect_create(e->scene, e->rt, &e->layout, e->cfg.sample_rate,
                                          bw_sink_block_size(e->sink), e->refl_cfg.order,
                                          e->refl_cfg.ir_seconds, e->refl_cfg.num_rays, e->refl_cfg.num_bounces,
                                          e->refl_cfg.wet_gain);
        if (e->reflect) rt_set_bus_tap(e->rt, steam_reflect_tap, e->reflect);
    }
#endif

    /* track_internal: ingest OptiTrack pose ourselves and sample it on the audio thread.
     * Configured via env (no NatNet specifics in BwConfig). Non-fatal: a failure leaves the
     * engine running on the committed/default listener and surfaces via bw_last_error. Done
     * before bw_sink_start so the pose slot is wired before the audio thread reads it. */
    if (e->cfg.track_internal) {
        /* BWAUDIO_NATNET_RIGIDBODY is a streaming ID if fully numeric, else a rigid-body name. */
        const char* rb = getenv("BWAUDIO_NATNET_RIGIDBODY");
        int32_t rb_id = 0; const char* rb_name = NULL;
        if (rb && rb[0]) { char* end; long v = strtol(rb, &end, 10); if (*end == 0) rb_id = (int32_t)v; else rb_name = rb; }
        NatNetConfig nc = {
            .multicast       = env_or("BWAUDIO_NATNET_MULTICAST", "239.255.42.99"),
            .server          = getenv("BWAUDIO_NATNET_SERVER"),
            .local_iface     = getenv("BWAUDIO_NATNET_IFACE"),
            .data_port       = env_u16("BWAUDIO_NATNET_DATA_PORT", 1511),
            .command_port    = env_u16("BWAUDIO_NATNET_COMMAND_PORT", 1510),
            .rigid_body      = rb_id,
            .rigid_body_name = rb_name,
            .major = 0, .minor = 0,
        };
        const char* ver = getenv("BWAUDIO_NATNET_VERSION");
        if (ver && ver[0]) { nc.major = atoi(ver); const char* d = strchr(ver, '.'); nc.minor = d ? atoi(d + 1) : 0; }
        e->tracker = natnet_open(&nc, e->errbuf, sizeof e->errbuf);
        if (e->tracker) rt_set_tracker(e->rt, natnet_pose(e->tracker));
        else            set_error(e, e->errbuf[0] ? e->errbuf : "track_internal: NatNet open failed");
    }

    if (bw_sink_start(e->sink) != 0 || (e->sink_mon && bw_sink_start(e->sink_mon) != 0)) {
        set_error(e, "bw_start: sink failed to start");
        engine_close_devices(e);
        return 2;
    }
    e->started = 1;
    return 0;
}

int bw_stop(BwEngine* e) {
    if (!e) return 1;
    engine_close_devices(e);                            /* joins the audio thread(s) */
    e->started = 0;
    return 0;
}

void bw_destroy(BwEngine* e) {
    if (!e) return;
    engine_close_devices(e);                            /* stop audio + tear down the Steam decoder */
#ifdef BW_HAVE_STEAMAUDIO
    steam_scene_destroy(e->scene);                      /* join the occlusion sim thread before rt is freed */
#endif
    monitor_destroy(e->monitor);
    rt_destroy(e->rt);
    free((void*)e->cfg.layout_path);                    /* owned copies from bw_create */
    free((void*)e->cfg.hrtf_path);
    free(e);
}

const char* bw_last_error(BwEngine* e) {
    return e ? e->last_error : NULL;
}

const char* bw_audio_backend(BwEngine* e) {
    if (!e || !e->sink) return "none";
    return bw_sink_backend(e->sink);   /* binaural/both: the primary (headphone/array) device */
}

/* ---- assets ---- */

BwSound bw_load_sound(BwEngine* e, const char* path) {
    if (!e) return 0;
    e->errbuf[0] = 0;
    BwSound snd = rt_load_sound(e->rt, path, e->errbuf, sizeof e->errbuf);
    if (snd == 0) set_error(e, e->errbuf[0] ? e->errbuf : "bw_load_sound: failed");
    return snd;
}

void bw_unload_sound(BwEngine* e, BwSound snd) {
    if (e) rt_unload_sound(e->rt, snd);   /* safe any time; retire-acked internally */
}

BwSound bw_load_ambix(BwEngine* e, const char* path) {
    if (!e) return 0;
    e->errbuf[0] = 0;
    BwSound snd = rt_load_ambix(e->rt, path, e->errbuf, sizeof e->errbuf);
    if (snd == 0) set_error(e, e->errbuf[0] ? e->errbuf : "bw_load_ambix: failed");
    return snd;
}

/* ---- ambisonic beds: a bed is a voice that plays a multichannel asset; mix_bed decodes it ---- */
BwBed bw_bed_create(BwEngine* e)                                { return bw_source_create(e); }
void  bw_bed_play(BwEngine* e, BwBed b, BwSound snd, bool loop) {
    if (!e) return;
    if (rt_sound_channels(e->rt, snd) <= 1) {       /* a bed needs an ambisonic (multichannel) asset */
        set_error(e, "bw_bed_play: asset is mono — load it with bw_load_ambix (a 4/9/16-ch AmbiX file)");
        return;
    }
    rt_source_play(e->rt, b, snd, loop);            /* direct: bypass bw_source_play's mono-only guard */
}
void  bw_bed_set_gain(BwEngine* e, BwBed b, float linear)       { bw_source_set_gain(e, b, linear); }
void  bw_bed_stop(BwEngine* e, BwBed b)                         { bw_source_stop(e, b); }
void  bw_bed_destroy(BwEngine* e, BwBed b)                      { bw_source_destroy(e, b); }

/* ---- sources (forward to the rt core) ---- */

BwSource bw_source_create(BwEngine* e) {
    return e ? rt_source_create(e->rt) : 0;
}
void bw_source_set_priority(BwEngine* e, BwSource s, int priority) {
    if (e) rt_source_set_priority(e->rt, s, priority);
}
void bw_source_destroy(BwEngine* e, BwSource s) {
    if (!e) return;
#ifdef BW_HAVE_STEAMAUDIO
    /* clear ALL scene features (occlusion + directivity) so the sim tears down this source's
     * IPLSource and stops simulating the slot (else it leaks + a recycled slot inherits stale state). */
    if (e->scene) steam_scene_source_gone(e->scene, s);
#endif
    rt_source_destroy(e->rt, s);
}
void bw_source_set_pos(BwEngine* e, BwSource s, float x, float y, float z) {
    if (!e) return;
    rt_source_set_pos(e->rt, s, x, y, z);
#ifdef BW_HAVE_STEAMAUDIO
    if (e->scene) steam_scene_set_pos(e->scene, s, x, y, z);   /* keep the occlusion sim in sync */
#endif
}
void bw_source_set_gain(BwEngine* e, BwSource s, float linear)          { if (e) rt_source_set_gain(e->rt, s, linear); }
void bw_source_play(BwEngine* e, BwSource s, BwSound snd, bool loop) {
    if (!e) return;
    if (rt_sound_channels(e->rt, snd) > 1) {        /* a multichannel asset is a bed, not a point source */
        set_error(e, "bw_source_play: asset is multichannel — use bw_bed_play (or bw_load_sound for a point source)");
        return;
    }
    rt_source_play(e->rt, s, snd, loop);
}
void bw_source_stop(BwEngine* e, BwSource s)                           { if (e) rt_source_stop(e->rt, s); }
bool bw_source_is_playing(BwEngine* e, BwSource s)                     { return e ? rt_source_is_playing(e->rt, s) : false; }
void bw_test_signal(BwEngine* e, uint32_t channel, BwTestKind kind, float gain) { if (e) rt_test_signal(e->rt, channel, (uint8_t)kind, gain); }

void bw_set_panner(BwEngine* e, BwPanner panner) { if (e) rt_set_panner(e->rt, (int)panner); }
void bw_set_dual_band(BwEngine* e, bool on)       { if (e) rt_set_dual_band(e->rt, on); }

/* Offline: the chosen panner's per-speaker gains for `nsrc` source positions heard from one listener,
 * over a layout given as `n` speaker positions (3 floats each). Default DBAP/distance tuning. Shares
 * the SPCAP/VBAP per-listener cache across the batch, so it is efficient for grid evaluation. Writes
 * out[i*n + s]; returns nsrc. Not engine state — pure, for layout scoring/optimization in tools. */
uint32_t bw_panner_gains_batch(BwPanner panner, const float* positions, uint32_t n,
                               const float lis[3], const float* srcs, uint32_t nsrc, float* out) {
    if (!positions || !lis || !srcs || !out || n == 0 || n > BW_CHANNELS || nsrc == 0) return 0;
    Layout L = layout_default();                         /* default rolloff_r / distance attenuation */
    L.count = n;
    for (uint32_t s = 0; s < n; ++s) {
        L.speakers[s].pos[0] = positions[s*3+0];
        L.speakers[s].pos[1] = positions[s*3+1];
        L.speakers[s].pos[2] = positions[s*3+2];
        L.speakers[s].gain_lin = 1.0f;
        L.speakers[s].delay_samples = 0;
    }
    SpcapState sp; VbapState vb;
    if (panner == BW_PAN_SPCAP) spcap_reset(&sp);
    else if (panner == BW_PAN_VBAP) vbap_reset(&vb);
    for (uint32_t i = 0; i < nsrc; ++i) {
        const float* src = &srcs[(size_t)i * 3];
        float* o = &out[(size_t)i * n];
        if (panner == BW_PAN_SPCAP)     spcap_gains(&sp, src, lis, &L, 1u, 1.0f, o);  /* cache reused across the batch */
        else if (panner == BW_PAN_VBAP) vbap_gains(&vb, src, lis, &L, 1u, 1.0f, o);
        else                            dbap_gains(src, lis, &L, 1.0f, o);
    }
    return nsrc;
}
void bw_set_bed_decoder(BwEngine* e, BwBedDecoder decoder) { if (e) rt_set_bed_decoder(e->rt, (int)decoder); }

uint32_t bw_get_speakers(BwEngine* e, float* xyz, uint32_t cap) {
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

void bw_play_oneshot(BwEngine* e, BwSound snd, float x, float y, float z, float gain) {
    if (!e) return;
    if (rt_sound_channels(e->rt, snd) > 1) {
        set_error(e, "bw_play_oneshot: asset is multichannel — oneshots are point sources (use bw_load_sound)");
        return;
    }
    rt_play_oneshot(e->rt, snd, x, y, z, gain);
}

/* ---- materials / occlusion (no-ops without the Steam Audio backend) ---- */

#ifdef BW_HAVE_STEAMAUDIO
/* Geometry CAN change at runtime for occlusion: the occlusion sim owns the IPLScene and serializes
 * its commit + ray trace on its own single thread, so a mesh swap there is safe (the control thread
 * only hands it a pending buffer under a lock). It is locked only once the REFLECTION bed is running:
 * that sim shares the same IPLScene, and an iplSceneCommit cannot race its RunReflections (v1 has no
 * scene-swap handshake — the reflection IR assumes a static scene). */
static int scene_locked(BwEngine* e) {
    if (!e || !e->scene) return 1;
    if (e->reflect) { set_error(e, "scene geometry is locked while the reflection bed is running"); return 1; }
    return 0;
}
#endif

void bw_scene_set_mesh(BwEngine* e, const float* verts, int nverts, const int* tris, int ntris,
                       const float absorption[3], float scattering, const float transmission[3]) {
#ifdef BW_HAVE_STEAMAUDIO
    if (scene_locked(e)) return;
    steam_scene_set_mesh(e->scene, verts, nverts, tris, ntris, absorption, scattering, transmission);
#else
    (void)e; (void)verts; (void)nverts; (void)tris; (void)ntris; (void)absorption; (void)scattering; (void)transmission;
#endif
}

/* Clamp a coefficient to [0,1] and sanitize non-finite input (NaN/Inf -> 0): the `!(x>=0)` test is
 * true for NaN and negatives, so both map to 0 — keeping garbage out of phonon's ray/reverb math. */
static float clamp01(float x) { if (!(x >= 0.f)) return 0.f; return (x > 1.f) ? 1.f : x; }

/* Material tokens are plain control-thread state — the table exists with or without the SDK, so
 * minting works regardless; the mesh setters that consume the tokens are the SDK-gated no-ops. */
BwMaterial bw_material_define(BwEngine* e, const float absorption[3], float scattering, const float transmission[3]) {
    if (!e) return 0;
    if (!absorption || !transmission) { set_error(e, "bw_material_define: NULL coefficients"); return 0; }
    if (e->num_materials >= BW_MAX_MATERIALS) { set_error(e, "bw_material_define: material table full"); return 0; }
    uint32_t i = e->num_materials++;
    for (int b = 0; b < 3; ++b) { e->materials[i].absorption[b] = clamp01(absorption[b]); e->materials[i].transmission[b] = clamp01(transmission[b]); }
    e->materials[i].scattering = clamp01(scattering);
    return (BwMaterial)i;
}

BwMaterial bw_material_preset(BwEngine* e, const char* name) {
    if (!e || !name) return 0;
    for (size_t k = 0; k < sizeof BW_PRESETS / sizeof BW_PRESETS[0]; ++k)
        if (_stricmp(name, BW_PRESETS[k].name) == 0)
            /* "generic" (preset 0) IS the built-in default — return the canonical token 0 rather than
             * minting a duplicate slot (callers tag many surfaces with it; don't burn the table). */
            return (k == 0) ? 0
                            : bw_material_define(e, BW_PRESETS[k].absorption, BW_PRESETS[k].scattering, BW_PRESETS[k].transmission);
    set_error(e, "bw_material_preset: unknown preset name");
    return 0;
}

void bw_scene_set_mesh_mat(BwEngine* e, const float* verts, int nverts, const int* tris, int ntris,
                           const BwMaterial* tri_material) {
#ifdef BW_HAVE_STEAMAUDIO
    if (scene_locked(e)) return;
    uint32_t nmat = e->num_materials;                    /* flatten the table to the arrays steam_scene wants */
    float absorption[BW_MAX_MATERIALS*3], transmission[BW_MAX_MATERIALS*3], scattering[BW_MAX_MATERIALS];
    for (uint32_t k = 0; k < nmat; ++k) {
        for (int b = 0; b < 3; ++b) { absorption[k*3+b] = e->materials[k].absorption[b]; transmission[k*3+b] = e->materials[k].transmission[b]; }
        scattering[k] = e->materials[k].scattering;
    }
    /* BwMaterial tokens ARE the indices (uint32 -> int, small values); steam_scene clamps any out-of-range. */
    steam_scene_set_mesh_mat(e->scene, verts, nverts, tris, ntris, (int)nmat,
                             absorption, scattering, transmission, (const int*)tri_material);
#else
    (void)e; (void)verts; (void)nverts; (void)tris; (void)ntris; (void)tri_material;
#endif
}

#ifdef BW_HAVE_STEAMAUDIO
/* Emit triangle (i0,i1,i2) into tris[*n], flipping the last two indices if needed so the face normal
 * points toward the origin (inward — the listener is inside the box). */
static void emit_inward(const float* v, int* tris, int* n, int i0, int i1, int i2) {
    const float *p0 = &v[i0*3], *p1 = &v[i1*3], *p2 = &v[i2*3];
    float e1x = p1[0]-p0[0], e1y = p1[1]-p0[1], e1z = p1[2]-p0[2];
    float e2x = p2[0]-p0[0], e2y = p2[1]-p0[1], e2z = p2[2]-p0[2];
    float nx = e1y*e2z - e1z*e2y, ny = e1z*e2x - e1x*e2z, nz = e1x*e2y - e1y*e2x;
    float cx = (p0[0]+p1[0]+p2[0])/3.f, cy = (p0[1]+p1[1]+p2[1])/3.f, cz = (p0[2]+p1[2]+p2[2])/3.f;
    if (nx*(-cx) + ny*(-cy) + nz*(-cz) < 0.f) { int tmp = i1; i1 = i2; i2 = tmp; }   /* normal points outward -> flip */
    tris[*n*3+0] = i0; tris[*n*3+1] = i1; tris[*n*3+2] = i2; (*n)++;
}
#endif

void bw_scene_set_box(BwEngine* e, float w, float h, float d, const BwMaterial faces[6]) {
#ifdef BW_HAVE_STEAMAUDIO
    if (scene_locked(e)) return;
    if (!(w > 0.f) || !(h > 0.f) || !(d > 0.f)) {   /* reject zero/negative/NaN dims (degenerate triangles) */
        set_error(e, "bw_scene_set_box: w/h/d must be positive");
        return;
    }
    float hw = w*0.5f, hh = h*0.5f, hd = d*0.5f;
    float verts[8*3] = {
        -hw,-hh,-hd,   hw,-hh,-hd,   hw, hh,-hd,  -hw, hh,-hd,
        -hw,-hh, hd,   hw,-hh, hd,   hw, hh, hd,  -hw, hh, hd };
    static const int quad[6][4] = {            /* face order: -x,+x,-y,+y,-z,+z (matches faces[6]) */
        {0,4,7,3}, {1,2,6,5}, {0,1,5,4}, {3,7,6,2}, {0,3,2,1}, {4,5,6,7} };
    int tris[12*3]; BwMaterial tri_mat[12]; int n = 0;
    for (int f = 0; f < 6; ++f) {
        BwMaterial m = faces ? faces[f] : 0;
        int a = quad[f][0], b = quad[f][1], c = quad[f][2], dd = quad[f][3];
        emit_inward(verts, tris, &n, a, b, c);  tri_mat[n-1] = m;
        emit_inward(verts, tris, &n, a, c, dd); tri_mat[n-1] = m;
    }
    bw_scene_set_mesh_mat(e, verts, 8, tris, 12, tri_mat);
#else
    (void)e; (void)w; (void)h; (void)d; (void)faces;
#endif
}

void bw_source_set_occlusion(BwEngine* e, BwSource s, bool on) {
#ifdef BW_HAVE_STEAMAUDIO
    if (e && e->scene) steam_scene_set_occlusion(e->scene, s, on);
#else
    (void)e; (void)s; (void)on;
#endif
}

float bw_source_get_occlusion(BwEngine* e, BwSource s) {
    return e ? rt_get_occlusion(e->rt, s) : 1.0f;
}

void bw_reflections_config(BwEngine* e, const BwReflectionConfig* cfg) {
    if (!e || !cfg) return;
    e->refl_cfg = *cfg;                                         /* applied at bw_start; zero -> defaults */
    if (e->refl_cfg.ir_seconds <= 0.f) e->refl_cfg.ir_seconds = 1.0f;
    if (e->refl_cfg.order == 0)        e->refl_cfg.order       = 1;
    if (e->refl_cfg.order > 2)         e->refl_cfg.order       = 2;   /* v1: order 1 or 2 (3 = 16ch is heavy) */
    if (e->refl_cfg.num_rays == 0)     e->refl_cfg.num_rays    = 4096;
    if (e->refl_cfg.num_bounces == 0)  e->refl_cfg.num_bounces = 16;
    if (e->refl_cfg.wet_gain <= 0.f)   e->refl_cfg.wet_gain    = 1.0f;
}

void bw_reflections_set_gain(BwEngine* e, float linear) {
#ifdef BW_HAVE_STEAMAUDIO
    if (e && e->reflect) steam_reflect_set_gain(e->reflect, linear);
#else
    (void)e; (void)linear;
#endif
}

void bw_source_set_reflections(BwEngine* e, BwSource s, bool on) {
    if (e) rt_source_set_reflections(e->rt, s, on);             /* phonon-free; the tap consumes the send */
}

void bw_source_set_reflection_send(BwEngine* e, BwSource s, float gain) {
    if (e) rt_source_set_reflection_send(e->rt, s, gain);
}

void bw_source_set_reflection_distance(BwEngine* e, BwSource s, bool on) {
    if (e) rt_source_set_reflection_distance(e->rt, s, on);
}

void bw_source_set_doppler(BwEngine* e, BwSource s, bool on) {
    if (e) rt_source_set_doppler(e->rt, s, on);
}

void bw_source_set_air_absorption(BwEngine* e, BwSource s, bool on) {
    if (e) rt_source_set_air_absorption(e->rt, s, on);
}

void bw_source_set_spread(BwEngine* e, BwSource s, float amount) {
    if (e) rt_source_set_spread(e->rt, s, amount);
}

void bw_source_set_orientation(BwEngine* e, BwSource s, float qx, float qy, float qz, float qw) {
#ifdef BW_HAVE_STEAMAUDIO
    if (!e || !e->scene) return;
    /* the dipole axis is the source forward = q * (0,0,-1) * q^-1 */
    float fx = -2.0f * (qw * qy + qx * qz);
    float fy =  2.0f * (qw * qx - qy * qz);
    float fz = -1.0f + 2.0f * (qx * qx + qy * qy);
    steam_scene_set_orientation(e->scene, s, fx, fy, fz);
#else
    (void)e; (void)s; (void)qx; (void)qy; (void)qz; (void)qw;
#endif
}

void bw_source_set_directivity(BwEngine* e, BwSource s, float weight, float power) {
#ifdef BW_HAVE_STEAMAUDIO
    if (e && e->scene) steam_scene_set_directivity(e->scene, s, weight, power);
#else
    (void)e; (void)s; (void)weight; (void)power;
#endif
}

void bw_source_set_directivity_preset(BwEngine* e, BwSource s, BwDirectivity pattern) {
    float weight = 0.0f, power = 1.0f;
    if      (pattern == BW_DIR_CARDIOID) weight = 0.5f;
    else if (pattern == BW_DIR_FIGURE8)  weight = 1.0f;   /* OMNI -> weight 0 (off) */
    bw_source_set_directivity(e, s, weight, power);
}

float bw_source_get_directivity(BwEngine* e, BwSource s) {
    return e ? rt_get_directivity(e->rt, s) : 1.0f;
}

/* ---- listener ---- */

void bw_set_listener_pose(BwEngine* e, float px, float py, float pz,
                                       float qx, float qy, float qz, float qw) {
    if (!e) return;
    const float p[3] = { px, py, pz };
    const float q[4] = { qx, qy, qz, qw };
    rt_set_listener(e->rt, p, q);
}

void bw_get_listener_pose(BwEngine* e, float p[3], float q[4]) {
    if (!e || !p || !q) return;
    rt_read_pose(e->rt, p, q);
}

/* ---- frame boundary ---- */

void bw_commit(BwEngine* e) {
    if (e) rt_commit(e->rt);
}
