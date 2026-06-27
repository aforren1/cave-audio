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

#include <stdlib.h>
#include <string.h>

/* Opaque engine object. The real one (docs/internal-types.md) carries the rings,
 * voice table, bus, and layout; the stub only needs enough to be a valid handle. */
struct BwEngine {
    BwConfig    cfg;
    int         started;
    const char* last_error;     /* points at a static string; NULL when clean */
    uint32_t    next_source;    /* hands out distinct, non-zero stub handles    */
    uint32_t    next_sound;
};

/* Handles are (index | generation<<16); 0 is invalid (see include/bwaudio.h).
 * The stub uses a fixed generation of 1 and a monotonically increasing index. */
static uint32_t make_handle(uint32_t index) {
    return (index & 0xFFFFu) | (1u << 16);
}

/* ---- lifecycle ---- */

BwEngine* bw_create(const BwConfig* cfg) {
    if (!cfg) return NULL;
    BwEngine* e = (BwEngine*)calloc(1, sizeof *e);
    if (!e) return NULL;
    e->cfg        = *cfg;
    e->next_source = 1;
    e->next_sound  = 1;
    return e;
}

int bw_start(BwEngine* e) {
    if (!e) return 1;           /* BW_ERR_CONFIG; see docs/api.md error codes */
    e->started = 1;             /* no device opened yet (M1) */
    return 0;
}

int bw_stop(BwEngine* e) {
    if (!e) return 1;
    e->started = 0;
    return 0;
}

void bw_destroy(BwEngine* e) {
    free(e);                    /* free(NULL) is a no-op */
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
