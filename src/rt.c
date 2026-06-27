/*
 * rt.c — real-time core. Two SPSC rings + voice table + commit snapshot + generation
 * handles, exactly as specified in docs/concurrency.md, plus the Sound table and the
 * retire-ack handshake. M3 mixes wav playback (mix_voice reads sound->pcm); routing is
 * still the M2 placeholder (one position-derived channel) until M4's DBAP solve. Nothing
 * here allocates/locks on rt_render.
 */
#include "rt.h"
#include "sound.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#include <xmmintrin.h>     /* _MM_SET_FLUSH_ZERO_MODE */
#include <pmmintrin.h>     /* _MM_SET_DENORMALS_ZERO_MODE */
#endif

#define RING_CAP 4096          /* power of two; sized for a worst-case frame burst */
#define EVT_CAP  1024          /* power of two */

typedef struct { Cmd slots[RING_CAP]; _Atomic uint32_t write, read; } CmdRing;
typedef struct { Evt slots[EVT_CAP];  _Atomic uint32_t write, read; } EvtRing;

typedef struct {
    uint16_t gen;
    bool     active, playing, loop, dirty, oneshot;
    const SoundData* sound;                 /* bound sound (NULL when idle); audio reads pcm */
    uint32_t cursor;                        /* sample cursor into sound->pcm */
    float    pos_pending[3], pos_active[3];
    float    gain_user;
    float    gtarget[BW_CHANNELS], gcur[BW_CHANNELS];
} Voice;

typedef struct {
    float p_pending[3], q_pending[4];
    float p_active[3],  q_active[4];
} Listener;

typedef struct {
    SoundData data;
    uint16_t  gen;
    uint8_t   inuse;
    uint8_t   retiring;                     /* unload requested; awaiting EVT_SOUND_RETIRED ack */
} SoundSlot;

struct RtCore {
    uint32_t voice_cap, channels, sample_rate;
    Voice*   voices;
    Listener lis;
    CmdRing  cmds;
    EvtRing  events;

    /* control-thread-owned voice handle allocation */
    uint16_t* gen;                          /* current generation per voice slot */
    uint8_t*  inuse;                        /* 1 while a voice slot is allocated */
    uint32_t* freelist;
    uint32_t  free_count;

    /* control-thread-owned sound table + handle allocation */
    SoundSlot* sounds;
    uint32_t   sound_cap;
    uint32_t*  sfreelist;
    uint32_t   sfree_count;
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
    c->inuse[idx] = 1;
    return BW_MK_H(idx, g);
}

static void recycle_handle(RtCore* c, uint16_t idx) {
    /* Idempotent: only a currently-allocated slot is returned to the free-list, so a
     * double-destroy (or a destroy racing a future EVT_VOICE_ENDED) can neither double-
     * free the index nor overflow free_count past voice_cap. */
    if (idx < c->voice_cap && c->inuse[idx]) {
        c->inuse[idx] = 0;
        c->freelist[c->free_count++] = idx;
    }
}

/* sound-handle allocation (mirrors the voice allocator) */
static uint32_t salloc_sound(RtCore* c) {
    if (c->sfree_count == 0) return 0;
    uint32_t idx = c->sfreelist[--c->sfree_count];
    uint16_t g = (uint16_t)(c->sounds[idx].gen + 1);
    if (g == 0) g = 1;
    c->sounds[idx].gen = g;
    c->sounds[idx].inuse = 1;
    c->sounds[idx].retiring = 0;
    return BW_MK_H(idx, g);
}

static void srecycle_sound(RtCore* c, uint16_t idx) {
    if (idx < c->sound_cap && c->sounds[idx].inuse) {
        c->sounds[idx].inuse = 0;
        c->sounds[idx].retiring = 0;
        c->sfreelist[c->sfree_count++] = idx;
    }
}

/* control-thread resolve: the slot iff the handle is its current occupant */
static SoundSlot* sound_slot_ctrl(RtCore* c, uint32_t h) {
    uint16_t i = BW_H_IDX(h);
    if (i >= c->sound_cap) return NULL;
    SoundSlot* s = &c->sounds[i];
    return (s->inuse && s->gen == BW_H_GEN(h)) ? s : NULL;
}

static void drain_events(RtCore* c) {                          /* control thread */
    EvtRing* r = &c->events;
    uint32_t rd = atomic_load_explicit(&r->read,  memory_order_relaxed);
    uint32_t w  = atomic_load_explicit(&r->write, memory_order_acquire);
    for (; rd != w; ++rd) {
        const Evt* ev = &r->slots[rd & (EVT_CAP - 1)];
        switch (ev->type) {
        case EVT_VOICE_ENDED:    recycle_handle(c, BW_H_IDX(ev->handle)); break;
        case EVT_SOUND_RETIRED: {        /* audio dropped all refs: free pcm + recycle the slot */
            SoundSlot* s = sound_slot_ctrl(c, ev->handle);
            if (s) { sound_unload(&s->data); srecycle_sound(c, BW_H_IDX(ev->handle)); }
        } break;
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

/* audio-thread resolve: handle -> published SoundData (NULL if stale/retired) */
static const SoundData* sound_for(RtCore* c, uint32_t h) {
    uint16_t i = BW_H_IDX(h);
    if (i >= c->sound_cap) return NULL;
    SoundSlot* s = &c->sounds[i];
    return (s->inuse && s->gen == BW_H_GEN(h)) ? &s->data : NULL;
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
            if (v) { v->active = false; v->playing = false; v->sound = NULL; } } break;
        case CMD_SET_POS: { Voice* v = voice_for(c, cmd->handle);
            if (v) memcpy(v->pos_pending, &cmd->u.pos, sizeof v->pos_pending); } break;
        case CMD_SET_GAIN: { Voice* v = voice_for(c, cmd->handle);
            if (v) { v->gain_user = cmd->u.gain.g; v->dirty = true; } } break;
        case CMD_PLAY: { Voice* v = voice_for(c, cmd->handle);
            const SoundData* s = sound_for(c, cmd->u.play.sound);
            if (v && s) { v->sound = s; v->cursor = 0; v->loop = cmd->u.play.loop != 0;
                          v->oneshot = cmd->u.play.oneshot != 0; v->playing = true; v->dirty = true; } } break;
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
            /* Detach every voice still bound to this sound, then ack so the control thread
             * frees the buffer exactly once the audio thread has provably let go. */
            const SoundData* s = sound_for(c, cmd->handle);
            if (s) {
                for (uint32_t i = 0; i < c->voice_cap; ++i)
                    if (c->voices[i].sound == s) { c->voices[i].playing = false; c->voices[i].sound = NULL; }
            }
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

/* Mix one voice: read its sound at the cursor (looping or ending), spatialize through the
 * per-channel block-linear gcur->gtarget ramp (invariant 4), and accumulate into the bus.
 * Routing (gtarget) is still the M2 placeholder until M4. On a non-looping end the voice
 * stops; a oneshot additionally acks EVT_VOICE_ENDED so the control thread recycles its
 * transient handle. */
static void mix_voice(RtCore* c, Voice* v, uint16_t idx, float* bus, uint32_t n) {
    const SoundData* snd = v->sound;
    float step[BW_CHANNELS];
    for (uint32_t ch = 0; ch < c->channels; ++ch)
        step[ch] = (v->gtarget[ch] - v->gcur[ch]) / (float)n;

    uint32_t cur = v->cursor;
    bool ended = false;
    for (uint32_t i = 0; i < n; ++i) {
        if (cur >= snd->frames) {
            if (v->loop) cur = 0;
            else         ended = true;
        }
        float s = ended ? 0.f : snd->pcm[cur];
        if (!ended) ++cur;
        for (uint32_t ch = 0; ch < c->channels; ++ch) {
            bus[(size_t)ch * n + i] += v->gcur[ch] * s;
            v->gcur[ch] += step[ch];
        }
    }
    v->cursor = cur;
    for (uint32_t ch = 0; ch < c->channels; ++ch) v->gcur[ch] = v->gtarget[ch]; /* land exactly */

    if (ended) {
        v->playing = false;
        if (v->oneshot) {
            v->active = false;                 /* transient voice is finished */
            Evt ev = { .type = EVT_VOICE_ENDED, .handle = BW_MK_H(idx, v->gen) };
            evt_push(&c->events, &ev);
        }
    }
}

void rt_render(RtCore* c, float* bus, uint32_t nframes, const BwTimestamp* ts) {
    (void)ts;
#if defined(_MSC_VER)
    /* Flush denormals to zero on the audio thread: gain ramps toward 0 (e.g. a voice
     * moving off a channel) otherwise produce subnormals that stall the FP pipeline. */
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
    drain_commands(c);
    memset(bus, 0, sizeof(float) * (size_t)nframes * c->channels);
    for (uint32_t i = 0; i < c->voice_cap; ++i) {
        Voice* v = &c->voices[i];
        if (!v->active || !v->playing || !v->sound) continue;
        if (v->dirty) { compute_gains(c, v); v->dirty = false; }
        mix_voice(c, v, (uint16_t)i, bus, nframes);
    }
}

/* ---- control-thread API (enqueue) ---- */

uint32_t rt_source_create(RtCore* c) {
    uint32_t h = alloc_handle(c);
    if (!h) return 0;
    Cmd cmd = { .type = CMD_SRC_CREATE, .handle = h };
    if (!cmd_push(&c->cmds, &cmd)) {        /* ring full (should never happen): don't leak the slot */
        recycle_handle(c, BW_H_IDX(h));
        return 0;
    }
    return h;
}

void rt_source_destroy(RtCore* c, uint32_t h) {
    Cmd cmd = { .type = CMD_SRC_DESTROY, .handle = h };
    /* Recycle only if the destroy was actually enqueued, so a dropped command can't leave
     * the voice active while the index is handed out again. recycle is idempotent, so a
     * double-destroy is harmless. */
    if (cmd_push(&c->cmds, &cmd)) recycle_handle(c, BW_H_IDX(h));
}

void rt_source_set_pos(RtCore* c, uint32_t h, float x, float y, float z) {
    if (!(isfinite(x) && isfinite(y) && isfinite(z))) return;  /* keep NaN/Inf off the audio thread */
    Cmd cmd = { .type = CMD_SET_POS, .handle = h };
    cmd.u.pos.x = x; cmd.u.pos.y = y; cmd.u.pos.z = z;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_set_gain(RtCore* c, uint32_t h, float linear) {
    if (!isfinite(linear) || linear < 0.f) return;             /* reject NaN/Inf/negative gain */
    Cmd cmd = { .type = CMD_SET_GAIN, .handle = h };
    cmd.u.gain.g = linear;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_play(RtCore* c, uint32_t h, uint32_t sound, bool loop) {
    SoundSlot* s = sound_slot_ctrl(c, sound);
    if (!s || s->retiring) return;          /* invalid or being unloaded: drop the play so the
                                             * audio thread can never bind a retiring sound */
    Cmd cmd = { .type = CMD_PLAY, .handle = h };
    cmd.u.play.sound = sound; cmd.u.play.loop = loop ? 1u : 0u; cmd.u.play.oneshot = 0u;
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

/* ---- assets (control thread; file I/O + alloc) ---- */

uint32_t rt_load_sound(RtCore* c, const char* path, char* err, size_t errcap) {
    SoundData d;
    if (!sound_load(path, c->sample_rate, &d, err, errcap)) return 0;
    uint32_t h = salloc_sound(c);
    if (!h) {
        sound_unload(&d);
        if (err && errcap) { strncpy(err, "sound: table full", errcap - 1); err[errcap - 1] = 0; }
        return 0;
    }
    c->sounds[BW_H_IDX(h)].data = d;     /* published to the audio thread when a CMD_PLAY references it */
    return h;
}

void rt_unload_sound(RtCore* c, uint32_t sound) {
    SoundSlot* s = sound_slot_ctrl(c, sound);
    if (!s || s->retiring) return;       /* invalid or already retiring: idempotent no-op */
    s->retiring = 1;                     /* refuse new binds (rt_source_play checks this) */
    Cmd cmd = { .type = CMD_SOUND_RETIRE, .handle = sound };
    cmd_push(&c->cmds, &cmd);            /* on ring-full the buffer lingers until rt_destroy */
}

/* Fire-and-forget: a transient voice that recycles itself on EVT_VOICE_ENDED. Its position
 * takes effect on the next rt_commit (the engine's per-frame commit). */
void rt_play_oneshot(RtCore* c, uint32_t sound, float x, float y, float z, float gain) {
    SoundSlot* s = sound_slot_ctrl(c, sound);
    if (!s || s->retiring) return;
    if (!(isfinite(x) && isfinite(y) && isfinite(z) && isfinite(gain) && gain >= 0.f)) return;
    uint32_t h = alloc_handle(c);
    if (!h) return;
    Cmd create = { .type = CMD_SRC_CREATE, .handle = h };
    if (!cmd_push(&c->cmds, &create)) { recycle_handle(c, BW_H_IDX(h)); return; }
    rt_source_set_pos(c, h, x, y, z);
    rt_source_set_gain(c, h, gain);
    Cmd play = { .type = CMD_PLAY, .handle = h };
    play.u.play.sound = sound; play.u.play.loop = 0u; play.u.play.oneshot = 1u;
    cmd_push(&c->cmds, &play);
}

/* ---- lifecycle ---- */

RtCore* rt_create(uint32_t voice_cap, uint32_t sound_cap, uint32_t sample_rate, uint32_t channels) {
    if (voice_cap == 0 || voice_cap > 0xFFFFu || sound_cap == 0 || sound_cap > 0xFFFFu ||
        channels  == 0 || channels  > BW_CHANNELS || sample_rate == 0)
        return NULL;
    RtCore* c = (RtCore*)calloc(1, sizeof *c);
    if (!c) return NULL;
    c->voice_cap   = voice_cap;
    c->sound_cap   = sound_cap;
    c->channels    = channels;
    c->sample_rate = sample_rate;
    c->voices    = (Voice*)    calloc(voice_cap, sizeof(Voice));
    c->gen       = (uint16_t*) calloc(voice_cap, sizeof(uint16_t));
    c->inuse     = (uint8_t*)  calloc(voice_cap, sizeof(uint8_t));
    c->freelist  = (uint32_t*) calloc(voice_cap, sizeof(uint32_t));
    c->sounds    = (SoundSlot*)calloc(sound_cap, sizeof(SoundSlot));
    c->sfreelist = (uint32_t*) calloc(sound_cap, sizeof(uint32_t));
    if (!c->voices || !c->gen || !c->inuse || !c->freelist || !c->sounds || !c->sfreelist) {
        rt_destroy(c); return NULL;
    }
    /* push indices so the first alloc hands out slot 0, then 1, ... */
    for (uint32_t i = 0; i < voice_cap; ++i) c->freelist[c->free_count++]   = voice_cap - 1 - i;
    for (uint32_t i = 0; i < sound_cap; ++i) c->sfreelist[c->sfree_count++] = sound_cap - 1 - i;
    return c;
}

void rt_destroy(RtCore* c) {
    if (!c) return;
    if (c->sounds)                                  /* free any pcm still loaded */
        for (uint32_t i = 0; i < c->sound_cap; ++i)
            if (c->sounds[i].inuse) sound_unload(&c->sounds[i].data);
    free(c->sfreelist);
    free(c->sounds);
    free(c->freelist);
    free(c->inuse);
    free(c->gen);
    free(c->voices);
    free(c);
}
