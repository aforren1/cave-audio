/*
 * rt.c — real-time core. Two SPSC rings + voice table + commit snapshot + generation
 * handles, exactly as specified in docs/concurrency.md, plus the Sound table and the
 * retire-ack handshake. M3 mixes wav playback (mix_voice reads sound->pcm); routing is
 * still the M2 placeholder (one position-derived channel) until M4's DBAP solve. Nothing
 * here allocates/locks on rt_render.
 */
#include "rt.h"
#include "sound.h"
#include "layout.h"
#include "dbap.h"
#include "align.h"

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
    /* occlusion ramp state (audio-thread-only). The published target lives in the RtCore.occ_*
     * atomic arrays (outside this memset'd struct, so the off-thread sim never races a voice
     * create). occ_cur ramps toward the gated published value, applied to the mono signal pre-pan. */
    float    occ_cur;
    float    dir_cur;                        /* directivity ramp (source-radiation gain, pre-pan) */
    /* per-band transmission EQ state (audio-thread-only). eqg_cur are the slewed band gains; eq_co
     * are the 3 sections' live coefficients {b0,b1,b2,a1,a2}, INTERPOLATED toward the block's target
     * per sample so the spectral envelope never steps at a block boundary (invariant 4). The 4
     * history arrays are the Direct-Form-I state; eq_engaged gates the chain (bypassed when settled flat). */
    float    eqg_cur[3];
    float    eq_co[3][5];
    float    eq_x1[3], eq_x2[3], eq_y1[3], eq_y2[3];
    int      eq_engaged;
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

    /* occlusion handoff (off-thread sim -> audio thread), parallel to `voices` but OUTSIDE the
     * Voice struct so CMD_SRC_CREATE's memset never races a concurrent sim publish. The sim stores
     * (occ_handle, occ_val); the audio thread gates on its own v->gen (which it owns) and applies. */
    _Atomic uint32_t* occ_handle;           /* handle the sim last published for (0 = none) */
    _Atomic float*    occ_val;              /* published broadband level (1 = clear) */
    _Atomic uint64_t* occ_eq;               /* published 3-band transmission tilt (3x16-bit, gated by occ_handle) */
    _Atomic float*    occ_dir;              /* published directivity gain (1 = on-axis/omni, gated by occ_handle) */
    struct { float cw0, alpha; int type; } eq_proto[3];   /* per-band biquad prototypes, rate-derived at create */

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

    /* spatialization (set at create/load time; read by the audio thread) */
    Layout   layout;
    Aligner* aligner;

    /* internal tracker (track_internal): the audio thread samples this each block, overriding
     * the committed listener. Set while the audio thread is stopped; NULL = no internal tracker. */
    const PoseSlot* tracker;

    /* readback of the active listener pose, published by the audio thread each block so the
     * control thread can sample it race-free (bw_get_listener_pose — visuals/logging). */
    PoseSlot readback;
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

/* Free slots, from the producer's view. The audio thread only advances `read` (frees
 * space), so once the single producer sees >= k free it can push k commands atomically. */
static uint32_t cmd_free(const CmdRing* r) {
    uint32_t w  = atomic_load_explicit(&r->write, memory_order_relaxed);
    uint32_t rd = atomic_load_explicit(&r->read,  memory_order_acquire);
    return RING_CAP - (w - rd);
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

/* ---- per-band transmission EQ (matches Steam Audio's default direct-effect EQ) ----
 * Three RBJ biquads in series — low-shelf @800, peaking @~2530, high-shelf @8000 — applied to the
 * mono voice signal. The band *gains* are the spectral tilt of occluded sound; the broadband level
 * rides the existing occ_cur scalar. fc's are fixed, so the trig (cos w0 / alpha) is precomputed
 * per sample-rate at rt_create; only A=sqrt(g) varies per update => no transcendentals per sample. */
enum { EQ_LOWSHELF = 0, EQ_PEAK = 1, EQ_HIGHSHELF = 2 };
#define EQ_SLEW     0.5f       /* per-block band-gain glide toward the published target */
#define EQ_FLAT     0xFFFFFFFFFFFFull /* eq_pack({1,1,1}): the flat (passthrough) tilt, as a constant */
#define EQ_FLAT_EPS 0.001f     /* band gains within +/-0.1% (~0.009 dB) of unity count as flat -> EQ bypassed
                                * (a deliberately inaudible threshold that keeps un-occluded voices off the
                                * biquad path; the per-band attenuation floor lives in steam_scene.c). */

/* pack 3 band gains (each clamped to [0,1]) into one u64 = 3x16-bit, for a tear-free atomic publish */
static inline uint64_t eq_pack(const float g[3]) {
    uint64_t p = 0;
    for (int i = 0; i < 3; ++i) {
        float v = g[i] < 0.f ? 0.f : (g[i] > 1.f ? 1.f : g[i]);
        p |= (uint64_t)(uint16_t)(v * 65535.f + 0.5f) << (16 * i);
    }
    return p;
}
static inline void eq_unpack(uint64_t p, float g[3]) {
    for (int i = 0; i < 3; ++i) g[i] = (float)((p >> (16 * i)) & 0xFFFFu) * (1.f / 65535.f);
}

/* RBJ Audio-EQ-Cookbook coefficients (a0-normalized, Direct Form I). cw0/alpha are precomputed per
 * filter; g is the linear band gain (A = sqrt(g)). out = {b0,b1,b2,a1,a2}. */
static void eq_coeffs(int type, float cw0, float alpha, float g, float out[5]) {
    float A = sqrtf(g), b0, b1, b2, a0, a1, a2;
    if (type == EQ_PEAK) {
        a0 = 1.f + alpha / A; a1 = -2.f * cw0;        a2 = 1.f - alpha / A;
        b0 = 1.f + alpha * A; b1 = -2.f * cw0;        b2 = 1.f - alpha * A;
    } else {
        float t = 2.f * sqrtf(A) * alpha;
        if (type == EQ_LOWSHELF) {
            a0 =  (A + 1) + (A - 1) * cw0 + t;
            a1 = -2.f * ((A - 1) + (A + 1) * cw0);
            a2 =  (A + 1) + (A - 1) * cw0 - t;
            b0 =  A * ((A + 1) - (A - 1) * cw0 + t);
            b1 =  2.f * A * ((A - 1) - (A + 1) * cw0);
            b2 =  A * ((A + 1) - (A - 1) * cw0 - t);
        } else { /* EQ_HIGHSHELF */
            a0 =  (A + 1) - (A - 1) * cw0 + t;
            a1 =  2.f * ((A - 1) - (A + 1) * cw0);
            a2 =  (A + 1) - (A - 1) * cw0 - t;
            b0 =  A * ((A + 1) + (A - 1) * cw0 + t);
            b1 = -2.f * A * ((A - 1) + (A + 1) * cw0);
            b2 =  A * ((A + 1) + (A - 1) * cw0 - t);
        }
    }
    float inv = 1.f / a0;
    out[0] = b0 * inv; out[1] = b1 * inv; out[2] = b2 * inv; out[3] = a1 * inv; out[4] = a2 * inv;
}

static void drain_commands(RtCore* c) {
    CmdRing* r = &c->cmds;
    uint32_t rd = atomic_load_explicit(&r->read,  memory_order_relaxed);
    uint32_t w  = atomic_load_explicit(&r->write, memory_order_acquire);
    for (; rd != w; ++rd) {
        const Cmd* cmd = &r->slots[rd & (RING_CAP - 1)];
        switch (cmd->type) {
        case CMD_SRC_CREATE: {
            uint16_t idx = BW_H_IDX(cmd->handle);
            Voice* v = &c->voices[idx];
            memset(v, 0, sizeof *v);
            v->gen = BW_H_GEN(cmd->handle);
            v->active = true; v->gain_user = 1.f; v->dirty = true;
            v->occ_cur = 1.f;                       /* clear (un-occluded) by default */
            v->dir_cur = 1.f;                       /* on-axis/omni by default */
            v->eqg_cur[0] = v->eqg_cur[1] = v->eqg_cur[2] = 1.f;   /* flat EQ (history zeroed by memset) */
            for (int b = 0; b < 3; ++b) v->eq_co[b][0] = 1.f;     /* passthrough coeffs {1,0,0,0,0} */
            /* drop any publish the sim left for the prior occupant of this slot (the stores are
             * atomic, so a concurrent sim publish for the old handle can't tear; either way the new
             * gen won't match it). */
            atomic_store_explicit(&c->occ_handle[idx], 0u,   memory_order_relaxed);
            atomic_store_explicit(&c->occ_val[idx],    1.f,  memory_order_relaxed);
            atomic_store_explicit(&c->occ_eq[idx], eq_pack((float[3]){1.f,1.f,1.f}), memory_order_relaxed);
            atomic_store_explicit(&c->occ_dir[idx],    1.f,  memory_order_relaxed);
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

/* DBAP gain solve (M4): listener-relative, dirty-gated. CMD_COMMIT re-dirties a voice on a
 * position change and dirties all voices on a listener move (gains are listener-relative). */
static void compute_gains(RtCore* c, Voice* v) {
    dbap_gains(v->pos_active, c->lis.p_active, &c->layout, v->gain_user, v->gtarget);
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
    /* gate the sim's publish on our own generation (we own v->gen, so this is race-free): apply the
     * published transmittance only if it was published for THIS occupant, else treat as clear. Read
     * once into a local so the ramp aims at and lands on the same value (invariant 4 — no jump). */
    const uint32_t myh = BW_MK_H(idx, v->gen);
    const bool mine = atomic_load_explicit(&c->occ_handle[idx], memory_order_acquire) == myh;
    const float occ_tgt = mine ? atomic_load_explicit(&c->occ_val[idx], memory_order_relaxed) : 1.0f;
    const float occ_step = (occ_tgt - v->occ_cur) / (float)n;   /* occlusion ramp (invariant 4) */
    /* directivity (source-radiation gain): own ramp — it tracks source/listener motion, so a raw
     * per-block jump would zipper (invariant 4). Gated on the same handle. */
    const float dir_tgt = mine ? atomic_load_explicit(&c->occ_dir[idx], memory_order_relaxed) : 1.0f;
    const float dir_step = (dir_tgt - v->dir_cur) / (float)n;

    /* per-band EQ: read the gated tilt once + glide the band gains; compute this block's TARGET
     * biquad coeffs and interpolate the live coeffs (eq_co) toward them per sample, so the spectral
     * envelope never steps at a block boundary (invariant 4). Target is passthrough when flat; the
     * chain is bypassed once it has fully settled flat. */
    float gt[3];
    eq_unpack(mine ? atomic_load_explicit(&c->occ_eq[idx], memory_order_relaxed) : EQ_FLAT, gt);
    bool flat = true;
    for (int b = 0; b < 3; ++b) {
        v->eqg_cur[b] += (gt[b] - v->eqg_cur[b]) * EQ_SLEW;
        if (v->eqg_cur[b] < 1.f - EQ_FLAT_EPS || v->eqg_cur[b] > 1.f + EQ_FLAT_EPS) flat = false;
    }
    if (!flat) v->eq_engaged = 1;
    float co_tgt[3][5], co_step[3][5];
    if (v->eq_engaged) {
        for (int b = 0; b < 3; ++b) {
            if (flat) { co_tgt[b][0] = 1.f; co_tgt[b][1] = co_tgt[b][2] = co_tgt[b][3] = co_tgt[b][4] = 0.f; }
            else eq_coeffs(c->eq_proto[b].type, c->eq_proto[b].cw0, c->eq_proto[b].alpha, v->eqg_cur[b], co_tgt[b]);
            for (int k = 0; k < 5; ++k) co_step[b][k] = (co_tgt[b][k] - v->eq_co[b][k]) / (float)n;
        }
    }

    uint32_t cur = v->cursor;
    bool ended = false;
    for (uint32_t i = 0; i < n; ++i) {
        if (cur >= snd->frames) {
            if (v->loop) cur = 0;
            else         ended = true;
        }
        float s = ended ? 0.f : snd->pcm[cur];
        if (v->eq_engaged) {                                    /* 3 biquads (DF-I), coeffs interpolated per sample */
            for (int b = 0; b < 3; ++b) {
                float* co = v->eq_co[b];
                float y = co[0]*s + co[1]*v->eq_x1[b] + co[2]*v->eq_x2[b] - co[3]*v->eq_y1[b] - co[4]*v->eq_y2[b];
                v->eq_x2[b]=v->eq_x1[b]; v->eq_x1[b]=s; v->eq_y2[b]=v->eq_y1[b]; v->eq_y1[b]=y; s=y;
                for (int k = 0; k < 5; ++k) co[k] += co_step[b][k];   /* glide toward the block target */
            }
        }
        s *= v->occ_cur * v->dir_cur;                           /* occlusion level + directivity, pre-pan */
        if (!ended) ++cur;
        v->occ_cur += occ_step;
        v->dir_cur += dir_step;
        for (uint32_t ch = 0; ch < c->channels; ++ch) {
            bus[(size_t)ch * n + i] += v->gcur[ch] * s;
            v->gcur[ch] += step[ch];
        }
    }
    v->cursor = cur;
    v->occ_cur = occ_tgt;                                        /* land exactly (same local) */
    v->dir_cur = dir_tgt;
    if (v->eq_engaged) {
        for (int b = 0; b < 3; ++b) for (int k = 0; k < 5; ++k) v->eq_co[b][k] = co_tgt[b][k];   /* land coeffs */
        if (flat) {                                              /* settled to passthrough: bypass + reset history */
            v->eq_engaged = 0;
            for (int b = 0; b < 3; ++b) { v->eq_x1[b]=v->eq_x2[b]=v->eq_y1[b]=v->eq_y2[b]=0.f; }
        }
    }
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

    /* track_internal: sample the freshest tracked head pose at block time, overriding the
     * committed listener (lower latency than routing pose through the command ring). A position
     * change dirties every voice, since DBAP gains are all listener-relative. */
    if (c->tracker) {
        float tp[3], tq[4];
        if (pose_read(c->tracker, tp, tq)) {
            bool moved = memcmp(c->lis.p_active, tp, sizeof tp) != 0;
            memcpy(c->lis.p_active, tp, sizeof tp);
            memcpy(c->lis.q_active, tq, sizeof tq);
            if (moved) for (uint32_t i = 0; i < c->voice_cap; ++i) c->voices[i].dirty = true;
        }
    }

    memset(bus, 0, sizeof(float) * (size_t)nframes * c->channels);
    for (uint32_t i = 0; i < c->voice_cap; ++i) {
        Voice* v = &c->voices[i];
        if (!v->active || !v->playing || !v->sound) continue;
        if (v->dirty) { compute_gains(c, v); v->dirty = false; }
        mix_voice(c, v, (uint16_t)i, bus, nframes);
    }
    align_process(c->aligner, bus, nframes);   /* per-speaker gain trim + delay (output stage) */
    pose_write(&c->readback, c->lis.p_active, c->lis.q_active);   /* publish for control-thread readback */
}

void rt_read_pose(RtCore* c, float p[3], float q[4]) {
    if (!c) return;
    if (!pose_read(&c->readback, p, q)) {       /* lost the seqlock race (rare): best-effort direct read */
        memcpy(p, c->lis.p_active, sizeof(float) * 3);
        memcpy(q, c->lis.q_active, sizeof(float) * 4);
    }
}

/* Active listener pose, for the binaural monitor. Audio thread only (same thread as
 * rt_render, which is the sole writer of the active fields) — no extra synchronization. */
void rt_get_listener(RtCore* c, float p[3], float q[4]) {
    memcpy(p, c->lis.p_active, sizeof(float) * 3);
    memcpy(q, c->lis.q_active, sizeof(float) * 4);
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
    if (!cmd_push(&c->cmds, &cmd)) s->retiring = 0;   /* ring full: revert so it can be retried */
}

/* Fire-and-forget: a transient voice that recycles itself on EVT_VOICE_ENDED. Its position
 * takes effect on the next rt_commit (the engine's per-frame commit). */
void rt_play_oneshot(RtCore* c, uint32_t sound, float x, float y, float z, float gain) {
    SoundSlot* s = sound_slot_ctrl(c, sound);
    if (!s || s->retiring) return;
    if (!(isfinite(x) && isfinite(y) && isfinite(z) && isfinite(gain) && gain >= 0.f)) return;
    /* A oneshot enqueues 4 commands (CREATE/SET_POS/SET_GAIN/PLAY) that must all land, or
     * the transient voice is created-but-never-played and leaks (it never ends -> never
     * acks EVT_VOICE_ENDED -> never recycled). Reserve room for all 4 up front so a
     * ring-full case drops the whole oneshot rather than half of it. */
    if (cmd_free(&c->cmds) < 4) return;
    uint32_t h = alloc_handle(c);
    if (!h) return;
    Cmd create = { .type = CMD_SRC_CREATE, .handle = h };
    cmd_push(&c->cmds, &create);              /* the 4 pushes are guaranteed by cmd_free >= 4 */
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
    /* Bound the event ring: between two control-thread drains the audio thread emits at
     * most one EVT_VOICE_ENDED per voice plus one EVT_SOUND_RETIRED per sound, so
     * EVT_CAP >= voice_cap + sound_cap makes the event ring un-overflowable (acks can
     * never be silently dropped on the audio thread). */
    if ((uint64_t)voice_cap + sound_cap > EVT_CAP) return NULL;
    RtCore* c = (RtCore*)calloc(1, sizeof *c);
    if (!c) return NULL;
    c->voice_cap   = voice_cap;
    c->sound_cap   = sound_cap;
    c->channels    = channels;
    c->sample_rate = sample_rate;
    c->voices    = (Voice*)    calloc(voice_cap, sizeof(Voice));
    c->occ_handle = (_Atomic uint32_t*)calloc(voice_cap, sizeof(_Atomic uint32_t));
    c->occ_val    = (_Atomic float*)   calloc(voice_cap, sizeof(_Atomic float));
    c->occ_eq     = (_Atomic uint64_t*)calloc(voice_cap, sizeof(_Atomic uint64_t));
    c->occ_dir    = (_Atomic float*)   calloc(voice_cap, sizeof(_Atomic float));
    c->gen       = (uint16_t*) calloc(voice_cap, sizeof(uint16_t));
    c->inuse     = (uint8_t*)  calloc(voice_cap, sizeof(uint8_t));
    c->freelist  = (uint32_t*) calloc(voice_cap, sizeof(uint32_t));
    c->sounds    = (SoundSlot*)calloc(sound_cap, sizeof(SoundSlot));
    c->sfreelist = (uint32_t*) calloc(sound_cap, sizeof(uint32_t));
    if (!c->voices || !c->occ_handle || !c->occ_val || !c->occ_eq || !c->occ_dir || !c->gen ||
        !c->inuse || !c->freelist || !c->sounds || !c->sfreelist) {
        rt_destroy(c); return NULL;
    }
    const uint64_t eq_flat = eq_pack((float[3]){ 1.f, 1.f, 1.f });
    for (uint32_t i = 0; i < voice_cap; ++i) {   /* occlusion/directivity start clear (handle 0 = no publish) */
        atomic_store_explicit(&c->occ_val[i], 1.0f,    memory_order_relaxed);
        atomic_store_explicit(&c->occ_eq[i],  eq_flat, memory_order_relaxed);
        atomic_store_explicit(&c->occ_dir[i], 1.0f,    memory_order_relaxed);
    }
    /* precompute the 3 EQ band prototypes from the runtime sample rate (so the EQ is correct at
     * 48/96/192k). fc's: low-shelf 800, peaking at the geometric centre, high-shelf 8000. */
    {
        const float fc[3]  = { 800.f, sqrtf(800.f * 8000.f), 8000.f };
        const int   ty[3]  = { EQ_LOWSHELF, EQ_PEAK, EQ_HIGHSHELF };
        const float two_pi = 6.28318530717958648f;
        for (int i = 0; i < 3; ++i) {
            float w0 = two_pi * fc[i] / (float)sample_rate, sw0 = sinf(w0);
            c->eq_proto[i].cw0   = cosf(w0);
            c->eq_proto[i].alpha = (ty[i] == EQ_PEAK) ? sw0 * ((8000.f - 800.f) / fc[1]) * 0.5f   /* band-edge Q */
                                                      : sw0 / (2.f * 0.70710678f);                /* shelf Q=0.707 */
            c->eq_proto[i].type  = ty[i];
        }
    }
    c->lis.q_active[3]  = 1.0f;        /* default head orientation = identity (facing forward) */
    c->lis.q_pending[3] = 1.0f;
    c->readback.q[3]    = 1.0f;        /* readback identity until the first block publishes */
    c->layout  = layout_default();
    c->aligner = align_create(channels, &c->layout);
    if (!c->aligner) { rt_destroy(c); return NULL; }
    /* push indices so the first alloc hands out slot 0, then 1, ... */
    for (uint32_t i = 0; i < voice_cap; ++i) c->freelist[c->free_count++]   = voice_cap - 1 - i;
    for (uint32_t i = 0; i < sound_cap; ++i) c->sfreelist[c->sfree_count++] = sound_cap - 1 - i;
    return c;
}

/* Replace the speaker layout. Control thread, call BEFORE bw_start (or while stopped) —
 * it swaps the aligner the audio thread reads, so it is not safe concurrently with
 * rt_render. Voices recompute their DBAP gains on the next render (created dirty). */
void rt_set_layout(RtCore* c, const Layout* L) {
    if (!c || !L) return;
    Aligner* a = align_create(c->channels, L);
    if (!a) return;                         /* keep the old layout on alloc failure */
    c->layout = *L;
    align_destroy(c->aligner);
    c->aligner = a;
    for (uint32_t i = 0; i < c->voice_cap; ++i)
        if (c->voices[i].active) c->voices[i].dirty = true;
}

void rt_set_tracker(RtCore* c, const PoseSlot* slot) {
    if (c) c->tracker = slot;               /* audio thread reads it; set while stopped */
}

/* Publish a voice's occlusion transmittance (1 = clear, 0 = blocked). Called from the off-thread
 * occlusion sim, NOT the control thread — so it touches no rings/free-list and (crucially) NONE of
 * the audio-thread-owned Voice fields: it just deposits (handle, value) into the atomic publish
 * slot. The audio thread gates on its own generation when it consumes, so a stale/recycled handle
 * is dropped there (race-free, since the sim never reads v->gen/v->active). */
/* Unified per-source direct-effect publish: broadband `level`, 3-band transmission `bands` (each in
 * [0,1], normalized so the loudest is 1 — the audio thread's EQ tilt), and a `dir` directivity gain.
 * occ_handle is written LAST with release, so a publish for a DIFFERENT occupant is never half-applied
 * (the audio thread's gen-gate flips atomically with the handle). For a LIVE voice the handle is
 * unchanged across re-publishes, so the three fields can be observed one 30 Hz tick apart (e.g. level
 * from update N, bands/dir from N+1) — a bounded cross-field staleness that is harmless because the
 * audio thread ramps/glides all three toward their targets every block (no jump). */
void rt_set_direct(RtCore* c, uint32_t handle, float level, const float bands[3], float dir) {
    if (!c) return;
    uint16_t idx = BW_H_IDX(handle);
    if (idx >= c->voice_cap) return;
    if (level < 0.f) level = 0.f; if (level > 1.f) level = 1.f;
    if (dir   < 0.f) dir   = 0.f; if (dir   > 1.f) dir   = 1.f;
    atomic_store_explicit(&c->occ_val[idx],  level,          memory_order_relaxed);
    atomic_store_explicit(&c->occ_eq[idx],   eq_pack(bands), memory_order_relaxed);
    atomic_store_explicit(&c->occ_dir[idx],  dir,            memory_order_relaxed);
    atomic_store_explicit(&c->occ_handle[idx], handle,       memory_order_release);
}

void rt_set_occlusion(RtCore* c, uint32_t handle, float transmittance) {
    const float flat[3] = { 1.f, 1.f, 1.f };
    rt_set_direct(c, handle, transmittance, flat, 1.f);    /* broadband level, no tilt, omni */
}
void rt_set_occlusion_eq(RtCore* c, uint32_t handle, float level, const float band_gains[3]) {
    rt_set_direct(c, handle, level, band_gains, 1.f);      /* level + tilt, omni */
}

/* Read back a voice's published occlusion factor (1 = clear) — for HUD/diagnostics; 1 if the latest
 * publish was for a different occupant. Race-free: reads only the atomic publish slot. */
float rt_get_occlusion(RtCore* c, uint32_t handle) {
    if (!c) return 1.f;
    uint16_t idx = BW_H_IDX(handle);
    if (idx >= c->voice_cap) return 1.f;
    return (atomic_load_explicit(&c->occ_handle[idx], memory_order_acquire) == handle)
         ? atomic_load_explicit(&c->occ_val[idx], memory_order_relaxed) : 1.f;
}

/* Read back the published directivity gain (1 = on-axis/omni) — for HUD/diagnostics. */
float rt_get_directivity(RtCore* c, uint32_t handle) {
    if (!c) return 1.f;
    uint16_t idx = BW_H_IDX(handle);
    if (idx >= c->voice_cap) return 1.f;
    return (atomic_load_explicit(&c->occ_handle[idx], memory_order_acquire) == handle)
         ? atomic_load_explicit(&c->occ_dir[idx], memory_order_relaxed) : 1.f;
}

void rt_destroy(RtCore* c) {
    if (!c) return;
    align_destroy(c->aligner);
    if (c->sounds)                                  /* free any pcm still loaded */
        for (uint32_t i = 0; i < c->sound_cap; ++i)
            if (c->sounds[i].inuse) sound_unload(&c->sounds[i].data);
    free(c->sfreelist);
    free(c->sounds);
    free(c->freelist);
    free(c->inuse);
    free(c->gen);
    free((void*)c->occ_dir);            /* cast drops the _Atomic qualifier for free() */
    free((void*)c->occ_eq);
    free((void*)c->occ_val);
    free((void*)c->occ_handle);
    free(c->voices);
    free(c);
}
