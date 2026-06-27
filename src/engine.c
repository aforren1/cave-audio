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

#include <stdlib.h>
#include <string.h>

#define BW_VOICE_CAP 256       /* max simultaneous sources */
#define BW_SOUND_CAP 256       /* max loaded sounds */

struct BwEngine {
    BwConfig    cfg;
    int         started;
    const char* last_error;        /* points at errbuf or a literal; NULL when clean */
    char        errbuf[256];

    BwSink*     sink;              /* device/offline sink; owns the audio thread */
    RtCore*     rt;               /* rings + voice/sound tables + mixer (rt.c) */
};

static void set_error(BwEngine* e, const char* msg) {
    if (!e) return;
    if (msg && msg != e->errbuf) { strncpy(e->errbuf, msg, sizeof e->errbuf - 1); e->errbuf[sizeof e->errbuf - 1] = 0; }
    e->last_error = (msg && e->errbuf[0]) ? e->errbuf : msg;
}

/* The audio block (sink's audio thread): drain the command ring and mix into `bus`. */
static void engine_render(void* user, float* bus, uint32_t nframes, const BwTimestamp* ts) {
    BwEngine* e = (BwEngine*)user;
    rt_render(e->rt, bus, nframes, ts);
}

/* ---- lifecycle ---- */

BwEngine* bw_create(const BwConfig* cfg) {
    if (!cfg) return NULL;
    BwEngine* e = (BwEngine*)calloc(1, sizeof *e);
    if (!e) return NULL;
    e->cfg = *cfg;
    if (e->cfg.sample_rate == 0) e->cfg.sample_rate = 48000;   /* sane defaults */
    if (e->cfg.block_size  == 0) e->cfg.block_size  = 256;
    e->rt = rt_create(BW_VOICE_CAP, BW_SOUND_CAP, e->cfg.sample_rate, BW_CHANNELS);
    if (!e->rt) { free(e); return NULL; }
    return e;
}

int bw_start(BwEngine* e) {
    if (!e) return 1;                                  /* BW_ERR_CONFIG (docs/api.md) */
    if (e->started) return 0;
    e->errbuf[0] = 0; e->last_error = NULL;
    e->sink = bw_sink_open(e->cfg.sample_rate, e->cfg.block_size, BW_CHANNELS,
                           engine_render, e, e->errbuf, sizeof e->errbuf);
    if (!e->sink) { set_error(e, e->errbuf[0] ? e->errbuf : "bw_start: no audio sink"); return 2; /* BW_ERR_DEVICE */ }
    if (bw_sink_start(e->sink) != 0) {
        set_error(e, "bw_start: sink failed to start");
        bw_sink_close(e->sink); e->sink = NULL;
        return 2;
    }
    e->started = 1;
    return 0;
}

int bw_stop(BwEngine* e) {
    if (!e) return 1;
    if (e->sink) { bw_sink_close(e->sink); e->sink = NULL; }   /* joins the audio thread */
    e->started = 0;
    return 0;
}

void bw_destroy(BwEngine* e) {
    if (!e) return;
    if (e->sink) bw_sink_close(e->sink);                       /* stop audio before freeing rt */
    rt_destroy(e->rt);
    free(e);
}

const char* bw_last_error(BwEngine* e) {
    return e ? e->last_error : NULL;
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

/* ---- frame boundary ---- */

void bw_commit(BwEngine* e) {
    if (e) rt_commit(e->rt);
}
