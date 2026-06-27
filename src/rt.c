/*
 * rt.c — real-time core. Two SPSC rings + voice table + commit snapshot + generation
 * handles, exactly as specified in docs/concurrency.md. The mixing is an M2 placeholder
 * (generated tone -> one position-derived channel, block-linear gain ramp); M3 swaps in
 * wav playback and M4 the DBAP 26-gain solve. Nothing here allocates/locks on rt_render.
 */
#include "rt.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define RING_CAP 4096          /* power of two; sized for a worst-case frame burst */
#define EVT_CAP  1024          /* power of two */
#define TWO_PI   6.283185307179586

typedef struct { Cmd slots[RING_CAP]; _Atomic uint32_t write, read; } CmdRing;
typedef struct { Evt slots[EVT_CAP];  _Atomic uint32_t write, read; } EvtRing;

typedef struct {
    uint16_t gen;
    bool     active, playing, loop, dirty;
    uint32_t sound;                         /* bound sound handle; M3: const Sound* */
    uint32_t cursor;                        /* M3: sample cursor into the sound */
    float    pos_pending[3], pos_active[3];
    float    gain_user;
    float    gtarget[BW_CHANNELS], gcur[BW_CHANNELS];
    double   phase;                         /* M2 placeholder oscillator state */
} Voice;

typedef struct {
    float p_pending[3], q_pending[4];
    float p_active[3],  q_active[4];
} Listener;

struct RtCore {
    uint32_t voice_cap, channels, sample_rate;
    Voice*   voices;
    Listener lis;
    CmdRing  cmds;
    EvtRing  events;
    double   dphase;                        /* M2 test tone: radians/sample */

    /* control-thread-owned handle allocation */
    uint16_t* gen;                          /* current generation per slot */
    uint32_t* freelist;                     /* stack of free indices */
    uint32_t  free_count;
};

/* ---- ring primitives ---- */

static bool cmd_push(CmdRing* r, const Cmd* c) {
    uint32_t w  = atomic_load_explicit(&r->write, memory_order_relaxed);
    uint32_t rd = atomic_load_explicit(&r->read,  memory_order_acquire);
    if (w - rd >= RING_CAP) return false;                       /* full: should never happen */
    r->slots[w & (RING_CAP - 1)] = *c;
    atomic_store_explicit(&r->write, w + 1, memory_order_release);
    return true;
}

static bool evt_push(EvtRing* r, const Evt* e) {              /* audio thread */
    uint32_t w  = atomic_load_explicit(&r->write, memory_order_relaxed);
    uint32_t rd = atomic_load_explicit(&r->read,  memory_order_acquire);
    if (w - rd >= EVT_CAP) return false;
    r->slots[w & (EVT_CAP - 1)] = *e;
    atomic_store_explicit(&r->write, w + 1, memory_order_release);
    return true;
}

/* ---- control thread: handle allocation ---- */

static uint32_t alloc_handle(RtCore* c) {
    if (c->free_count == 0) return 0;                          /* table full */
    uint32_t idx = c->freelist[--c->free_count];
    uint16_t g = (uint16_t)(c->gen[idx] + 1);
    if (g == 0) g = 1;                                        /* skip 0 (invalid handle) */
    c->gen[idx] = g;
    return BW_MK_H(idx, g);
}

static void recycle_handle(RtCore* c, uint16_t idx) {
    if (idx < c->voice_cap) c->freelist[c->free_count++] = idx;
}

static void drain_events(RtCore* c) {                          /* control thread */
    EvtRing* r = &c->events;
    uint32_t rd = atomic_load_explicit(&r->read,  memory_order_relaxed);
    uint32_t w  = atomic_load_explicit(&r->write, memory_order_acquire);
    for (; rd != w; ++rd) {
        const Evt* ev = &r->slots[rd & (EVT_CAP - 1)];
        switch (ev->type) {
        case EVT_VOICE_ENDED:    recycle_handle(c, BW_H_IDX(ev->handle)); break; /* M3 emits these */
        case EVT_SOUND_RETIRED:  /* M3: free the sound buffer here */          break;
        }
    }
    atomic_store_explicit(&r->read, rd, memory_order_release);
}

/* ---- audio thread: drain + mix ---- */

static Voice* voice_for(RtCore* c, uint32_t h) {
    uint16_t i = BW_H_IDX(h);
    if (i >= c->voice_cap) return NULL;
    Voice* v = &c->voices[i];
    return (v->active && v->gen == BW_H_GEN(h)) ? v : NULL;    /* stale gen => dropped */
}

static void drain_commands(RtCore* c) {
    CmdRing* r = &c->cmds;
    uint32_t rd = atomic_load_explicit(&r->read,  memory_order_relaxed);
    uint32_t w  = atomic_load_explicit(&r->write, memory_order_acquire);
    for (; rd != w; ++rd) {
        const Cmd* cmd = &r->slots[rd & (RING_CAP - 1)];
        switch (cmd->type) {
        case CMD_SRC_CREATE: {
            Voice* v = &c->voices[BW_H_IDX(cmd->handle)];
            memset(v, 0, sizeof *v);
            v->gen = BW_H_GEN(cmd->handle);
            v->active = true; v->gain_user = 1.f; v->dirty = true;
        } break;
        case CMD_SRC_DESTROY: { Voice* v = voice_for(c, cmd->handle);
            if (v) { v->active = false; v->playing = false; v->sound = 0; } } break;
        case CMD_SET_POS: { Voice* v = voice_for(c, cmd->handle);
            if (v) memcpy(v->pos_pending, &cmd->u.pos, sizeof v->pos_pending); } break;
        case CMD_SET_GAIN: { Voice* v = voice_for(c, cmd->handle);
            if (v) { v->gain_user = cmd->u.gain.g; v->dirty = true; } } break;
        case CMD_PLAY: { Voice* v = voice_for(c, cmd->handle);
            if (v) { v->sound = cmd->u.play.sound; v->cursor = 0; v->loop = cmd->u.play.loop != 0;
                     v->playing = true; v->dirty = true; v->phase = 0.0; } } break;
        case CMD_STOP: { Voice* v = voice_for(c, cmd->handle);
            if (v) v->playing = false; } break;
        case CMD_SET_LISTENER:
            memcpy(c->lis.p_pending, &cmd->u.lis.px, sizeof(float) * 3);
            memcpy(c->lis.q_pending, &cmd->u.lis.qx, sizeof(float) * 4);
            break;
        case CMD_COMMIT: {
            bool lis_moved = memcmp(c->lis.p_active, c->lis.p_pending, sizeof c->lis.p_active) != 0;
            memcpy(c->lis.p_active, c->lis.p_pending, sizeof c->lis.p_active);
            memcpy(c->lis.q_active, c->lis.q_pending, sizeof c->lis.q_active);
            for (uint32_t i = 0; i < c->voice_cap; ++i) {
                Voice* v = &c->voices[i];
                if (!v->active) continue;
                if (memcmp(v->pos_active, v->pos_pending, sizeof v->pos_active)) {
                    memcpy(v->pos_active, v->pos_pending, sizeof v->pos_active);
                    v->dirty = true;
                }
                if (lis_moved) v->dirty = true;                /* gains are listener-relative */
            }
        } break;
        case CMD_SOUND_RETIRE: {
            /* M3 detaches voices referencing the sound, then acks. M2 has no sound table,
             * so ack immediately to exercise the retire-ack path. */
            Evt ev = { .type = EVT_SOUND_RETIRED, .handle = cmd->handle };
            evt_push(&c->events, &ev);
        } break;
        }
    }
    atomic_store_explicit(&r->read, rd, memory_order_release);
}

/* M2 placeholder for dbap_gains: route a voice to a single channel from its x position. */
static void compute_gains(RtCore* c, Voice* v) {
    for (uint32_t ch = 0; ch < c->channels; ++ch) v->gtarget[ch] = 0.f;
    int ch = (int)v->pos_active[0];
    if (ch < 0) ch = 0;
    if (ch >= (int)c->channels) ch = (int)c->channels - 1;
    v->gtarget[ch] = v->gain_user;
}

/* M2 placeholder for mix_voice: generated tone, per-channel block-linear gcur->gtarget
 * ramp (invariant 4). M3 replaces the tone with sound->pcm at v->cursor. */
static void mix_voice(RtCore* c, Voice* v, float* bus, uint32_t n) {
    float step[BW_CHANNELS];
    for (uint32_t ch = 0; ch < c->channels; ++ch)
        step[ch] = (v->gtarget[ch] - v->gcur[ch]) / (float)n;

    double phase = v->phase;
    for (uint32_t i = 0; i < n; ++i) {
        float s = (float)sin(phase);
        phase += c->dphase;
        if (phase >= TWO_PI) phase -= TWO_PI;
        for (uint32_t ch = 0; ch < c->channels; ++ch) {
            bus[(size_t)ch * n + i] += v->gcur[ch] * s;
            v->gcur[ch] += step[ch];
        }
    }
    v->phase = phase;
    for (uint32_t ch = 0; ch < c->channels; ++ch) v->gcur[ch] = v->gtarget[ch]; /* land exactly */
}

void rt_render(RtCore* c, float* bus, uint32_t nframes, const BwTimestamp* ts) {
    (void)ts;
    drain_commands(c);
    memset(bus, 0, sizeof(float) * (size_t)nframes * c->channels);
    for (uint32_t i = 0; i < c->voice_cap; ++i) {
        Voice* v = &c->voices[i];
        if (!v->active || !v->playing) continue;
        if (v->dirty) { compute_gains(c, v); v->dirty = false; }
        mix_voice(c, v, bus, nframes);
    }
}

/* ---- control-thread API (enqueue) ---- */

uint32_t rt_source_create(RtCore* c) {
    uint32_t h = alloc_handle(c);
    if (!h) return 0;
    Cmd cmd = { .type = CMD_SRC_CREATE, .handle = h };
    cmd_push(&c->cmds, &cmd);
    return h;
}

void rt_source_destroy(RtCore* c, uint32_t h) {
    Cmd cmd = { .type = CMD_SRC_DESTROY, .handle = h };
    cmd_push(&c->cmds, &cmd);
    recycle_handle(c, BW_H_IDX(h));     /* generations make immediate reuse safe (no ack) */
}

void rt_source_set_pos(RtCore* c, uint32_t h, float x, float y, float z) {
    Cmd cmd = { .type = CMD_SET_POS, .handle = h };
    cmd.u.pos.x = x; cmd.u.pos.y = y; cmd.u.pos.z = z;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_set_gain(RtCore* c, uint32_t h, float linear) {
    Cmd cmd = { .type = CMD_SET_GAIN, .handle = h };
    cmd.u.gain.g = linear;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_play(RtCore* c, uint32_t h, uint32_t sound, bool loop) {
    Cmd cmd = { .type = CMD_PLAY, .handle = h };
    cmd.u.play.sound = sound; cmd.u.play.loop = loop ? 1u : 0u;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_stop(RtCore* c, uint32_t h) {
    Cmd cmd = { .type = CMD_STOP, .handle = h };
    cmd_push(&c->cmds, &cmd);
}

void rt_set_listener(RtCore* c, const float p[3], const float q[4]) {
    Cmd cmd = { .type = CMD_SET_LISTENER };
    cmd.u.lis.px = p[0]; cmd.u.lis.py = p[1]; cmd.u.lis.pz = p[2];
    cmd.u.lis.qx = q[0]; cmd.u.lis.qy = q[1]; cmd.u.lis.qz = q[2]; cmd.u.lis.qw = q[3];
    cmd_push(&c->cmds, &cmd);
}

void rt_commit(RtCore* c) {
    Cmd cmd = { .type = CMD_COMMIT };
    cmd_push(&c->cmds, &cmd);
    drain_events(c);                    /* consume VOICE_ENDED / SOUND_RETIRED acks */
}

/* ---- lifecycle ---- */

RtCore* rt_create(uint32_t voice_cap, uint32_t sample_rate, uint32_t channels) {
    if (voice_cap == 0 || voice_cap > 0xFFFFu ||
        channels  == 0 || channels  > BW_CHANNELS || sample_rate == 0)
        return NULL;
    RtCore* c = (RtCore*)calloc(1, sizeof *c);
    if (!c) return NULL;
    c->voice_cap   = voice_cap;
    c->channels    = channels;
    c->sample_rate = sample_rate;
    c->dphase      = TWO_PI * 440.0 / (double)sample_rate;     /* M2 test tone: 440 Hz */
    c->voices   = (Voice*)   calloc(voice_cap, sizeof(Voice));
    c->gen      = (uint16_t*)calloc(voice_cap, sizeof(uint16_t));
    c->freelist = (uint32_t*)calloc(voice_cap, sizeof(uint32_t));
    if (!c->voices || !c->gen || !c->freelist) { rt_destroy(c); return NULL; }
    /* push indices so the first alloc hands out slot 0, then 1, ... */
    for (uint32_t i = 0; i < voice_cap; ++i) c->freelist[c->free_count++] = voice_cap - 1 - i;
    return c;
}

void rt_destroy(RtCore* c) {
    if (!c) return;
    free(c->freelist);
    free(c->gen);
    free(c->voices);
    free(c);
}
