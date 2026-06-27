/*
 * engine.c — M0 stub implementation of the bwaudio public C ABI.
 *
 * This milestone exists only to prove the library builds, links, and hands back
 * a valid opaque handle (see docs/roadmap.md "M0 — Scaffolding"). Every call is a
 * no-op beyond bookkeeping: there is no audio thread, no ASIO device, no rings,
 * and no DSP yet. Those arrive in M1+ per the roadmap. The real types (Voice,
 * Sound, Layout, the SPSC rings) are specified in docs/internal-types.h and
 * docs/concurrency.md and will replace the placeholders below.
 */
#include "bwaudio.h"
#include "sink.h"

#include <stdlib.h>
#include <string.h>

#define BW_CHANNELS 26             /* the array width; see docs/architecture.md */

/* Opaque engine object. The real one (docs/internal-types.md) carries the rings,
 * voice table, bus, and layout; M1 adds the device sink + audio loop, but DSP
 * (mixing/DBAP) is still M2+, so the block just emits silence. */
struct BwEngine {
    BwConfig    cfg;
    int         started;
    const char* last_error;        /* points at errbuf or a literal; NULL when clean */
    char        errbuf[256];

    BwSink*     sink;              /* device/offline sink; owns the audio thread */
    /* audio-thread diagnostics (written only by engine_render) */
    uint64_t    blocks_rendered;
    uint64_t    last_sample_pos;

    uint32_t    next_source;       /* hands out distinct, non-zero stub handles */
    uint32_t    next_sound;
};

/* Handles are (index | generation<<16); 0 is invalid (see include/bwaudio.h).
 * The stub uses a fixed generation of 1 and a monotonically increasing index. */
static uint32_t make_handle(uint32_t index) {
    return (index & 0xFFFFu) | (1u << 16);
}

static void set_error(BwEngine* e, const char* msg) {
    if (!e) return;
    if (msg && msg != e->errbuf) { strncpy(e->errbuf, msg, sizeof e->errbuf - 1); e->errbuf[sizeof e->errbuf - 1] = 0; }
    e->last_error = (msg && e->errbuf[0]) ? e->errbuf : msg;
}

/* The audio block. Runs on the sink's audio thread. M1: 26 channels of silence +
 * timestamp capture. M2 drains the command ring here; M3+ mixes voices; the output
 * stage (align_speakers) and the binaural tap follow (see docs/concurrency.md). */
static void engine_render(void* user, float* bus, uint32_t nframes, const BwTimestamp* ts) {
    BwEngine* e = (BwEngine*)user;
    memset(bus, 0, sizeof(float) * (size_t)nframes * BW_CHANNELS);
    e->last_sample_pos = ts->sample_pos;
    e->blocks_rendered += 1;
}

/* ---- lifecycle ---- */

BwEngine* bw_create(const BwConfig* cfg) {
    if (!cfg) return NULL;
    BwEngine* e = (BwEngine*)calloc(1, sizeof *e);
    if (!e) return NULL;
    e->cfg = *cfg;
    if (e->cfg.sample_rate == 0) e->cfg.sample_rate = 48000;   /* sane defaults */
    if (e->cfg.block_size  == 0) e->cfg.block_size  = 256;
    e->next_source = 1;
    e->next_sound  = 1;
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
    if (e->sink) { bw_sink_close(e->sink); e->sink = NULL; }
    e->started = 0;
    return 0;
}

void bw_destroy(BwEngine* e) {
    if (!e) return;
    if (e->sink) bw_sink_close(e->sink);
    free(e);
}

const char* bw_last_error(BwEngine* e) {
    return e ? e->last_error : NULL;
}

/* ---- assets ---- */

BwSound bw_load_sound(BwEngine* e, const char* path) {
    (void)path;                 /* M3: dr_wav decode + buffer lifetime */
    if (!e) return 0;
    return make_handle(e->next_sound++);
}

void bw_unload_sound(BwEngine* e, BwSound snd) {
    (void)e; (void)snd;         /* M3: retire-ack handshake (docs/concurrency.md) */
}

/* ---- sources ---- */

BwSource bw_source_create(BwEngine* e) {
    if (!e) return 0;
    return make_handle(e->next_source++);
}

void bw_source_destroy(BwEngine* e, BwSource s)                          { (void)e; (void)s; }
void bw_source_set_pos(BwEngine* e, BwSource s, float x, float y, float z) {
    (void)e; (void)s; (void)x; (void)y; (void)z;                        /* M2: enqueue CMD_SET_POS */
}
void bw_source_set_gain(BwEngine* e, BwSource s, float linear)          { (void)e; (void)s; (void)linear; }
void bw_source_play(BwEngine* e, BwSource s, BwSound snd, bool loop)    { (void)e; (void)s; (void)snd; (void)loop; }
void bw_source_stop(BwEngine* e, BwSource s)                           { (void)e; (void)s; }
void bw_play_oneshot(BwEngine* e, BwSound snd, float x, float y, float z, float gain) {
    (void)e; (void)snd; (void)x; (void)y; (void)z; (void)gain;
}

/* ---- listener ---- */

void bw_set_listener_pose(BwEngine* e, float px, float py, float pz,
                                       float qx, float qy, float qz, float qw) {
    (void)e; (void)px; (void)py; (void)pz; (void)qx; (void)qy; (void)qz; (void)qw;
}

/* ---- frame boundary ---- */

void bw_commit(BwEngine* e) {
    (void)e;                    /* M2: enqueue CMD_COMMIT + drain the event ring */
}
