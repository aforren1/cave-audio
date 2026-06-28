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
#include "binaural.h"
#include "natnet.h"

#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>           /* InterlockedExchange for the 'both' buffer handoff */

#define BW_VOICE_CAP 256       /* max simultaneous sources */
#define BW_SOUND_CAP 256       /* max loaded sounds */
#define BW_MAX_BLOCK 8192      /* upper bound on a device's frames-per-block (ASIO picks its own) */

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
    uint32_t    cap;              /* scratch capacity in frames (== cfg.block_size) */

    BwSink*     sink;             /* primary: cave 26ch / binaural 2ch / both array 26ch */
    float*      scratch26;        /* binaural: 26-ch array render before the monitor */

    BwSink*     sink_mon;         /* both: the monitor (2-ch) device */
    float*      mon_buf[2];       /* both: stereo double-buffer (each 2*cap), array thread -> monitor thread */
    volatile LONG mon_idx;        /* both: index of the latest published buffer */

    NatNet*     tracker;          /* track_internal: NatNet pose ingest (NULL otherwise) */
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
    monitor_process(e->monitor, e->scratch26, p, q, dev2, n);
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
    if (e->profile == BW_PROFILE_BINAURAL || e->profile == BW_PROFILE_BOTH) {
        e->monitor = monitor_create(&L, e->cfg.sample_rate);
        if (!e->monitor) { rt_destroy(e->rt); free(e); return NULL; }
    }
    return e;
}

/* Close any open device(s) and free the start-time scratch. Monitor sink (consumer) first,
 * then the array sink (producer), so the monitor thread stops reading mon_buf before we free
 * it. Each bw_sink_close joins its audio thread. */
static void engine_close_devices(BwEngine* e) {
    if (e->sink_mon) { bw_sink_close(e->sink_mon); e->sink_mon = NULL; }
    if (e->sink)     { bw_sink_close(e->sink);     e->sink     = NULL; }
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
    engine_close_devices(e);                            /* stop audio before freeing rt/monitor */
    monitor_destroy(e->monitor);
    rt_destroy(e->rt);
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

/* ---- sources (forward to the rt core) ---- */

BwSource bw_source_create(BwEngine* e) {
    return e ? rt_source_create(e->rt) : 0;
}
void bw_source_destroy(BwEngine* e, BwSource s)                          { if (e) rt_source_destroy(e->rt, s); }
void bw_source_set_pos(BwEngine* e, BwSource s, float x, float y, float z) { if (e) rt_source_set_pos(e->rt, s, x, y, z); }
void bw_source_set_gain(BwEngine* e, BwSource s, float linear)          { if (e) rt_source_set_gain(e->rt, s, linear); }
void bw_source_play(BwEngine* e, BwSource s, BwSound snd, bool loop)    { if (e) rt_source_play(e->rt, s, snd, loop); }
void bw_source_stop(BwEngine* e, BwSource s)                           { if (e) rt_source_stop(e->rt, s); }

void bw_play_oneshot(BwEngine* e, BwSound snd, float x, float y, float z, float gain) {
    if (e) rt_play_oneshot(e->rt, snd, x, y, z, gain);
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
