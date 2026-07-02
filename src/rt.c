/*
 * rt.c — real-time core. Two SPSC rings + voice table + commit snapshot + generation
 * handles, exactly as specified in docs/concurrency.md, plus the Sound table and the
 * retire-ack handshake. M3 mixes wav playback (mix_voice reads sound->pcm); routing is
 * still the M2 placeholder (one position-derived channel) until M4's DBAP solve. Nothing
 * here allocates/locks on rt_render.
 */
#include "rt.h"
#include "sound.h"
#include "stream.h"
#include "layout.h"
#include "dbap.h"
#include "spcap.h"
#include "vbap.h"
#include "align.h"
#include "ambisonics.h"   /* SH->26 decode for ambisonic beds */
#include "allrad.h"       /* robust SH->26 decode for irregular arrays */
#include "profile.h"      /* Tracy zones/plots (no-ops unless BWAUDIO_TRACY=ON) */

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
#define BW_RT_MAX_BLOCK 8192   /* aux-send scratch cap; must be >= any device block (matches engine's BW_MAX_BLOCK) */

/* propagation effects (opt-in, per voice) */
#define BW_SPEED_OF_SOUND   343.0f    /* m/s */
#define BW_DOPPLER_MAX_DIST 8.0f      /* propagation delay saturates past this (bounds the per-voice ring) */
#define BW_DOPPLER_TAU      0.032f    /* per-pole delay low-pass time (s); FFT-tuned, see mix_voice */
#define BW_AIR_FC_NEAR   18000.0f     /* air-absorption low-pass cutoff (Hz) at zero distance ... */
#define BW_AIR_FC_PER_M    650.0f     /* ... falling this many Hz per metre ... */
#define BW_AIR_FC_FLOOR   1200.0f     /* ... down to this floor */
/* distance->reverb send: the wet-send factor ramps from NEAR_SEND at NEAR_DIST to 1.0 at FAR_DIST */
#define BW_REFL_NEAR_DIST  1.0f
#define BW_REFL_FAR_DIST   6.0f
#define BW_REFL_NEAR_SEND  0.25f
#define BW_DUALBAND_FC     700.0f     /* dual-band panning crossover (Hz): amplitude below, power above */

typedef struct { Cmd slots[RING_CAP]; _Atomic uint32_t write, read; } CmdRing;
typedef struct { Evt slots[EVT_CAP];  _Atomic uint32_t write, read; } EvtRing;
typedef struct { uint32_t handle; float sh[BW_AMBI_CH]; float eq[3]; } PathPub;   /* one double-buffer slot of a voice's path field (directions + bending-loss band tilt) */

typedef struct {
    uint16_t gen;
    bool     active, playing, loop, dirty, oneshot;
    const SoundData* sound;                 /* bound sound (NULL when idle); audio reads pcm */
    uint32_t cursor;                        /* sample cursor into sound->pcm (in-memory sounds) */
    uint64_t stream_pos;                    /* absolute sample position into a streamed sound's ring */
    uint64_t start_sample;                  /* dsp-sample to begin output (0 = immediate); for scheduled play */
    float    pos_pending[3], pos_active[3];
    float    gain_user;
    float    gtarget[BW_CHANNELS], gcur[BW_CHANNELS];
    float    gtarget_lo[BW_CHANNELS], gcur_lo[BW_CHANNELS];   /* dual-band low (amplitude-norm) band gains */
    float    xover_lp;                                        /* dual-band crossover one-pole LP state */
    int      path_on;                                         /* gated into the pathing (indirect) render */
    float    path_sh_cur[BW_AMBI_CH];                         /* ramped path shCoeffs (toward the published target) */
    /* pathing bending-loss EQ (audio-thread-only): a 3-band tilt on the indirect signal before the
     * SH-encode, structurally identical to the occlusion EQ above but on the un-occluded s_raw and
     * its own filter state. Bypassed while the tilt is flat (path_eq_engaged == 0). */
    float    path_eqg_cur[3];
    float    path_eq_co[3][5];
    float    path_eq_x1[3], path_eq_x2[3], path_eq_y1[3], path_eq_y2[3];
    int      path_eq_engaged;
    /* occlusion ramp state (audio-thread-only). The published target lives in the RtCore.occ_*
     * atomic arrays (outside this memset'd struct, so the off-thread sim never races a voice
     * create). occ_cur ramps toward the gated published value, applied to the mono signal pre-pan. */
    float    occ_cur;
    float    dir_cur;                        /* directivity ramp (source-radiation gain, pre-pan) */
    bool     refl_send;                      /* opted into the reflection aux send (CMD_SET_REFLECTIONS) */
    bool     refl_dist;                      /* scale the wet send by distance (far = wetter) */
    float    refl_gain;                      /* per-voice wet-send level (default 1) */
    float    refl_g_cur;                     /* ramped effective send gain (audio-thread-only) */
    /* per-band transmission EQ state (audio-thread-only). eqg_cur are the slewed band gains; eq_co
     * are the 3 sections' live coefficients {b0,b1,b2,a1,a2}, INTERPOLATED toward the block's target
     * per sample so the spectral envelope never steps at a block boundary (invariant 4). The 4
     * history arrays are the Direct-Form-I state; eq_engaged gates the chain (bypassed when settled flat). */
    float    eqg_cur[3];
    float    eq_co[3][5];
    float    eq_x1[3], eq_x2[3], eq_y1[3], eq_y2[3];
    int      eq_engaged;
    /* propagation effects (audio-thread-only, opt-in). air_a_cur is the slewed one-pole coeff, air_y1
     * the filter memory. dop_delay is the current fractional delay (samples), gliding toward distance/c
     * each block - the glide IS the pitch shift; dop_w indexes this voice's slice of RtCore.dop_ring;
     * dop_init snaps the delay to distance/c on the first block after enable (no enable glitch). */
    bool     air_on, dop_on, dop_init;
    float    air_a_cur, air_y1;
    float    dop_delay, dop_dtgt;            /* read delay + its smoothed target (2-pole, per-sample) */
    uint32_t dop_w;
    float    spread;                         /* source angular width 0..1 (0 = point); blends the pan gains */
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

    /* per-slot playback state for control-thread readback (rt_source_is_playing): packed
     * (gen<<1 | playing-bit), republished by the audio thread each block. The gen guards a stale
     * or recycled handle (a mismatched gen reads as not-playing). */
    _Atomic uint32_t* play_pub;

    /* debug channel test signal (bw_test_signal): audio-thread DSP state, set by CMD_TEST_SIGNAL,
     * generated + summed onto each channel AFTER align (raw channel). 0 kind = off. */
    uint8_t  test_kind[BW_CHANNELS];
    float    test_gain[BW_CHANNELS];
    float    test_phase[BW_CHANNELS];   /* sine phase accumulator, radians */
    uint32_t test_noise;                /* shared LCG state for the noise kind */
    struct { float cw0, alpha; int type; } eq_proto[3];   /* per-band biquad prototypes, rate-derived at create */

    /* control-thread-owned voice handle allocation */
    uint16_t* gen;                          /* current generation per voice slot */
    uint8_t*  inuse;                        /* 1 while a voice slot is allocated */
    uint8_t*  priority;                     /* per-source steal priority (control-side; 0=expendable..255=protected) */
    uint32_t* freelist;
    uint32_t  free_count;

    /* control-thread-owned sound table + handle allocation */
    SoundSlot* sounds;
    uint32_t   sound_cap;
    uint32_t*  sfreelist;
    uint32_t   sfree_count;

    /* spatialization (set at create/load time; read by the audio thread) */
    Layout    layout;
    Aligner*  aligner;
    _Atomic int panner;      /* 0 = DBAP (moving observer); 1 = SPCAP; 2 = VBAP (both fixed observer); atomic for A/B */
    _Atomic int dual_band;   /* 0 = single (power) panning; 1 = dual-band (amplitude LF / power HF); atomic for A/B */
    float       xover_a;     /* one-pole LP coeff for the dual-band crossover (BW_DUALBAND_FC), rate-derived */
    uint64_t    dsp_block;   /* audio-thread: next block's dsp-sample (fallback clock when no device timestamp) */
    _Atomic uint64_t dsp_now;/* published block-start dsp-sample; control thread reads via rt_dsp_time for scheduling */
    uint32_t   layout_gen;   /* bumped on rt_set_layout; the SPCAP/VBAP caches compare it to self-invalidate */
    SpcapState spcap;        /* SPCAP cache (audio-thread-owned; rebuilt on listener/layout change) */
    VbapState  vbap;         /* VBAP cache (same) */
    int        bed_decoder;  /* 0 = sampling decode (SAD); 1 = AllRAD (robust on irregular arrays) */
    /* ambisonic bed decode: [speaker][ACN] = (2l+1)*Y_k^SN3D(speaker_dir)/L (sampling decode, SN3D),
     * rebuilt from the layout whenever it changes. A bed voice decodes its SH channels through this. */
    float    bed_decode[BW_CHANNELS][BW_AMBI_CH];

    /* internal tracker (track_internal): the audio thread samples this each block, overriding
     * the committed listener. Set while the audio thread is stopped; NULL = no internal tracker. */
    const PoseSlot* tracker;

    /* readback of the active listener pose, published by the audio thread each block so the
     * control thread can sample it race-free (bw_get_listener_pose — visuals/logging). */
    PoseSlot readback;

    /* post-mix aux-send tap (the reflection bed): a phonon-free hook the audio thread calls after the
     * voice loop. `aux` is the summed mono send (opted-in voices); set while stopped. */
    RtBusTap bus_tap;
    void*    bus_tap_ud;
    float*   aux;                /* BW_RT_MAX_BLOCK mono samples; the per-block aux send scratch */
    StreamSet* streams;          /* background file-streaming thread + ring pool (control thread owns lifecycle) */
    float*   stream_scratch;     /* BW_RT_MAX_BLOCK mono samples; a streaming voice's block, pulled before the mix */

    /* pathing: rt_render SH-encodes pathing voices into path_accum, then path_tap decodes it to the bus.
     * Per voice the sim publishes shCoeffs via a handle-gated double buffer (path_pub[idx*2 + path_idx]). */
    RtPathTap path_tap;
    void*    path_tap_ud;
    uint32_t path_ambi_ch;       /* (order+1)^2 of the pathing field; 0 = no path tap */
    float*   path_accum;         /* BW_AMBI_CH * BW_RT_MAX_BLOCK; summed ambisonic indirect field */
    PathPub* path_pub;           /* voice_cap * 2 (double-buffered per voice) */
    _Atomic int* path_idx;       /* voice_cap: front-buffer index the sim flips after writing the back */

    /* per-voice Doppler delay rings: voice_cap contiguous power-of-two rings of dop_ringlen floats each
     * (slice idx = dop_ring + idx*dop_ringlen), sized to BW_DOPPLER_MAX_DIST at the engine rate.
     * Allocated once at create (control thread); the audio thread writes/reads its voice's slice. */
    float*   dop_ring;
    uint32_t dop_ringlen;
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
        case EVT_SOUND_RETIRED: {        /* audio dropped all refs: free pcm / close the stream + recycle the slot */
            SoundSlot* s = sound_slot_ctrl(c, ev->handle);
            if (s) {
                if (s->data.stream) { stream_close(c->streams, s->data.stream); s->data.stream = NULL; }
                sound_unload(&s->data);
                srecycle_sound(c, BW_H_IDX(ev->handle));
            }
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

/* (re)start a voice's Doppler delay line clean: clear its ring slice + snap the delay next block, so a
 * fresh enable or a replay doesn't bleed the previous tail through the line. Audio thread (bounded). */
static void dop_line_reset(RtCore* c, Voice* v, uint16_t idx) {
    v->dop_w = 0; v->dop_delay = 0.f; v->dop_dtgt = 0.f; v->dop_init = true;
    memset(c->dop_ring + (size_t)idx * c->dop_ringlen, 0, (size_t)c->dop_ringlen * sizeof(float));
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
            v->air_a_cur = 1.f;                     /* air low-pass passthrough by default */
            v->refl_gain = 1.f;                     /* full wet-send level by default (gated by refl_send) */
            v->eqg_cur[0] = v->eqg_cur[1] = v->eqg_cur[2] = 1.f;   /* flat EQ (history zeroed by memset) */
            for (int b = 0; b < 3; ++b) v->eq_co[b][0] = 1.f;     /* passthrough coeffs {1,0,0,0,0} */
            v->path_eqg_cur[0] = v->path_eqg_cur[1] = v->path_eqg_cur[2] = 1.f;  /* flat pathing EQ */
            for (int b = 0; b < 3; ++b) v->path_eq_co[b][0] = 1.f;
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
            if (v && s) { v->sound = s; v->cursor = 0; v->stream_pos = 0; v->loop = cmd->u.play.loop != 0;
                          v->oneshot = cmd->u.play.oneshot != 0; v->playing = true; v->dirty = true;
                          v->start_sample = cmd->u.play.start;  /* 0 = now; else hold output until this dsp-sample */
                          v->refl_g_cur = 0.f;                  /* fresh start: ramp the wet send up from 0, no stale burst */
                          v->xover_lp = 0.f;                    /* fresh dual-band crossover state */
                          if (v->dop_on) dop_line_reset(c, v, BW_H_IDX(cmd->handle)); } } break;
        case CMD_STOP: { Voice* v = voice_for(c, cmd->handle);
            if (v) v->playing = false; } break;
        case CMD_SET_REFLECTIONS: { Voice* v = voice_for(c, cmd->handle);
            if (v) v->refl_send = cmd->u.refl.on != 0; } break;
        case CMD_SET_PATHING: { Voice* v = voice_for(c, cmd->handle);
            if (v) { v->path_on = cmd->u.path.on != 0;
                     if (!v->path_on) {                          /* clean restart: zero the ramp + flatten the EQ */
                         for (int k = 0; k < BW_AMBI_CH; ++k) v->path_sh_cur[k] = 0.f;
                         v->path_eq_engaged = 0;
                         for (int b = 0; b < 3; ++b) { v->path_eqg_cur[b] = 1.f;
                             v->path_eq_x1[b]=v->path_eq_x2[b]=v->path_eq_y1[b]=v->path_eq_y2[b]=0.f; } } } } break;
        case CMD_SET_REFL_SEND: { Voice* v = voice_for(c, cmd->handle);
            if (v) { float g = cmd->u.rsend.gain; v->refl_gain = g < 0.f ? 0.f : g; } } break;
        case CMD_SET_REFL_DIST: { Voice* v = voice_for(c, cmd->handle);
            if (v) v->refl_dist = cmd->u.rdist.on != 0; } break;
        case CMD_SET_DOPPLER: { Voice* v = voice_for(c, cmd->handle);
            if (v) {
                if (cmd->u.dop.on && !v->dop_on) dop_line_reset(c, v, BW_H_IDX(cmd->handle));  /* fresh enable */
                v->dop_on = cmd->u.dop.on != 0;
            } } break;
        case CMD_SET_AIR: { Voice* v = voice_for(c, cmd->handle);
            if (v) v->air_on = cmd->u.air.on != 0; } break;
        case CMD_SET_SPREAD: { Voice* v = voice_for(c, cmd->handle);
            if (v) { float a = cmd->u.spread.amount; v->spread = a < 0.f ? 0.f : (a > 1.f ? 1.f : a); v->dirty = true; } } break;
        case CMD_TEST_SIGNAL: {
            uint32_t ch = cmd->u.test.channel;
            if (ch < c->channels) { c->test_kind[ch] = cmd->u.test.kind; c->test_gain[ch] = cmd->u.test.gain; }
        } break;
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

/* Build the ambisonic bed decode matrix from the layout: for each speaker, sample the SH basis at
 * its direction (room -> ambisonic axes: room x=right/y=up/z=back -> ambi x=front/y=left/z=up) and
 * scale by (2l+1)/L. World-locked: directions are from the room origin, not the moving listener.
 * This is the projection/sampling decode, which assumes a roughly UNIFORM speaker distribution; the
 * cave grid is only approximately uniform, so it is good for a diffuse bed but a pseudo-inverse
 * (mode-matching) decode would be exact — a refinement, not needed for v1's diffuse content. */
static void build_bed_decode_sad(RtCore* c) {
    const float invL = 1.0f / (float)c->channels;
    for (uint32_t s = 0; s < c->channels; ++s) {
        const float* p = c->layout.speakers[s].pos;
        float len = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
        float ad[3];
        if (len < 1e-6f) { ad[0] = 1.f; ad[1] = 0.f; ad[2] = 0.f; }          /* degenerate: face front */
        else { ad[0] = -p[2]/len; ad[1] = -p[0]/len; ad[2] = p[1]/len; }     /* (-z,-x,y) = ambisonic axes */
        float y[BW_AMBI_CH];
        ambi_encode_sn3d(ad, y);
        for (int k = 0; k < BW_AMBI_CH; ++k) {
            int l = (int)floorf(sqrtf((float)k));                            /* ACN order of channel k */
            c->bed_decode[s][k] = (float)(2*l + 1) * y[k] * invL;
        }
    }
}

/* Dispatch the bed decode: AllRAD if selected (and the array triangulates), else the sampling decode. */
static void build_bed_decode(RtCore* c) {
    if (c->bed_decoder == 1 && allrad_build_decode(&c->layout, c->bed_decode))
        return;
    build_bed_decode_sad(c);
}

/* Source spread/size: blend the panner's point gains toward a width-controlled lobe centred on the
 * source direction (from the listener), then renormalise constant-power. spread 0 = the point gains;
 * 1 = a wide lobe. Panner-agnostic; runs only in the per-block gain solve (not the sample loop). */
static void spread_gains(RtCore* c, const Voice* v, float* g) {
    float d[3] = { v->pos_active[0]-c->lis.p_active[0], v->pos_active[1]-c->lis.p_active[1], v->pos_active[2]-c->lis.p_active[2] };
    float dl = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    if (dl < 1e-6f) return;                              /* source on the listener: no direction to spread around */
    d[0]/=dl; d[1]/=dl; d[2]/=dl;
    double p0 = 0.0; for (uint32_t k = 0; k < c->channels; ++k) p0 += (double)g[k]*g[k];
    float P = (float)sqrt(p0);                           /* preserve the panner's own power (never re-level) */
    if (P < 1e-9f) return;
    float s = v->spread; if (s > 1.f) s = 1.f;
    float q = 1.5f + (1.f - s) * 6.f;                    /* lobe exponent: wide at s=1, tight as s->0 */
    float lobe[BW_CHANNELS]; double ln = 0.0;
    for (uint32_t k = 0; k < c->channels; ++k) {
        const float* sp = c->layout.speakers[k].pos;
        float sd[3] = { sp[0]-c->lis.p_active[0], sp[1]-c->lis.p_active[1], sp[2]-c->lis.p_active[2] };
        float sl = sqrtf(sd[0]*sd[0] + sd[1]*sd[1] + sd[2]*sd[2]);
        float dot = sl > 1e-6f ? (sd[0]*d[0] + sd[1]*d[1] + sd[2]*d[2]) / sl : 0.f;
        float w = 0.5f * (1.f + dot); if (w < 0.f) w = 0.f;   /* [0,1], 1 toward the source */
        lobe[k] = powf(w, q);
        ln += (double)lobe[k] * lobe[k];
    }
    if (ln < 1e-12) return;
    float lnorm = (float)(P / sqrt(ln));                 /* scale the lobe to the same power P */
    double bn = 0.0;
    for (uint32_t k = 0; k < c->channels; ++k) { g[k] = (1.f - s) * g[k] + s * lobe[k] * lnorm; bn += (double)g[k]*g[k]; }
    if (bn < 1e-12) return;
    float bnorm = P / (float)sqrt(bn);                   /* the blend isn't norm-P -> renormalise back to P */
    for (uint32_t k = 0; k < c->channels; ++k) g[k] *= bnorm;
}

/* DBAP gain solve (M4): listener-relative, dirty-gated. CMD_COMMIT re-dirties a voice on a
 * position change and dirties all voices on a listener move (gains are listener-relative). A bed
 * voice (multi-channel asset) has no DBAP position — its master gain rides gtarget[0]. */
static void compute_gains(RtCore* c, Voice* v) {
    if (v->sound && v->sound->channels > 1) { v->gtarget[0] = v->gain_user; return; }
    int p = atomic_load_explicit(&c->panner, memory_order_acquire);
    if (p == 1)
        spcap_gains(&c->spcap, v->pos_active, c->lis.p_active, &c->layout, c->layout_gen, v->gain_user, v->gtarget);
    else if (p == 2)
        vbap_gains(&c->vbap, v->pos_active, c->lis.p_active, &c->layout, c->layout_gen, v->gain_user, v->gtarget);
    else
        dbap_gains(v->pos_active, c->lis.p_active, &c->layout, v->gain_user, v->gtarget);
    if (v->spread > 1e-3f) spread_gains(c, v, v->gtarget);   /* widen the image if this source has size */

    /* dual-band low band: the SAME gain directions, renormalised to amplitude (pressure) sum instead of
     * power. The target sum is the power gains' OWN magnitude ||g||_2 (= gain_user * distance_atten, set
     * by the panner) — NOT bare gain_user, which would cancel the distance attenuation and leave a
     * distant source's bass at full level. So the LF coherent pressure sum matches the HF energy level
     * at every distance. Always computed (cheap) so dual_band A/Bs live; the mixer reads it only when on. */
    double gs = 0.0, gp = 0.0;
    for (uint32_t k = 0; k < c->channels; ++k) { gs += v->gtarget[k]; gp += (double)v->gtarget[k] * v->gtarget[k]; }
    if (gs > 1e-9) { float sc = (float)(sqrt(gp) / gs);
        for (uint32_t k = 0; k < c->channels; ++k) v->gtarget_lo[k] = v->gtarget[k] * sc; }
    else for (uint32_t k = 0; k < c->channels; ++k) v->gtarget_lo[k] = v->gtarget[k];
}

/* Mix one voice: read its sound at the cursor (looping or ending), spatialize through the
 * per-channel block-linear gcur->gtarget ramp (invariant 4), and accumulate into the bus.
 * Routing (gtarget) is still the M2 placeholder until M4. On a non-looping end the voice
 * stops; a oneshot additionally acks EVT_VOICE_ENDED so the control thread recycles its
 * transient handle. */
static void mix_voice(RtCore* c, Voice* v, uint16_t idx, float* bus, uint32_t n, uint32_t start, float* aux) {
    const SoundData* snd = v->sound;
    float step[BW_CHANNELS];
    for (uint32_t ch = 0; ch < c->channels; ++ch)
        step[ch] = (v->gtarget[ch] - v->gcur[ch]) / (float)n;
    /* dual-band panning: split each sample at BW_DUALBAND_FC; the low band uses amplitude-normalised
     * gains (gcur_lo, better LF velocity vector), the high band the power gains. The complementary
     * 1st-order crossover (hi = s - lo) sums flat, so it composes with everything upstream. */
    const int dual = atomic_load_explicit(&c->dual_band, memory_order_acquire);
    const float xover_a = c->xover_a;
    float step_lo[BW_CHANNELS];
    if (dual) for (uint32_t ch = 0; ch < c->channels; ++ch)
        step_lo[ch] = (v->gtarget_lo[ch] - v->gcur_lo[ch]) / (float)n;
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

    /* propagation (opt-in): a distance-driven air-absorption low-pass + a glided Doppler delay line.
     * Both ride the block: the air coeff ramps (invariant 4); the Doppler delay glides toward
     * distance/c and the glide rate IS the pitch shift. Indices stay integer (the ring is masked,
     * the delay's frac is a separate small float) so a long-lived voice never loses sample precision. */
    float dist = 0.f;
    if (v->air_on || v->dop_on || (v->refl_send && v->refl_dist)) {
        float dx = v->pos_active[0]-c->lis.p_active[0], dy = v->pos_active[1]-c->lis.p_active[1], dz = v->pos_active[2]-c->lis.p_active[2];
        dist = sqrtf(dx*dx + dy*dy + dz*dz);
    }
    /* reverb wet-send level: refl_gain, optionally scaled by distance (near = drier, far = wetter); ramped
     * (so motion + on/off don't zipper the send). do_send keeps ramping a just-disabled voice down to 0. */
    float refl_tgt = 0.f;
    if (aux && v->refl_send) {
        refl_tgt = v->refl_gain;
        if (v->refl_dist) {
            float t = (dist - BW_REFL_NEAR_DIST) / (BW_REFL_FAR_DIST - BW_REFL_NEAR_DIST);
            if (t < 0.f) t = 0.f; else if (t > 1.f) t = 1.f;
            refl_tgt *= BW_REFL_NEAR_SEND + (1.f - BW_REFL_NEAR_SEND) * t;
        }
    }
    const float refl_step = (refl_tgt - v->refl_g_cur) / (float)n;
    const bool do_send = aux && (v->refl_send || v->refl_g_cur > 1e-6f);
    float air_a_tgt = 1.f, air_a_step = 0.f;
    if (v->air_on) {
        float fc = BW_AIR_FC_NEAR - dist * BW_AIR_FC_PER_M;
        if (fc < BW_AIR_FC_FLOOR) fc = BW_AIR_FC_FLOOR;
        air_a_tgt = 1.f - expf(-6.28318530718f * fc / (float)c->sample_rate);
        if (air_a_tgt > 1.f) air_a_tgt = 1.f;
        air_a_step = (air_a_tgt - v->air_a_cur) / (float)n;
    }
    float dop_ds = 0.f, dop_k = 0.f, *dring = NULL; uint32_t dmask = 0;
    if (v->dop_on && c->dop_ring) {
        dop_ds = dist / BW_SPEED_OF_SOUND * (float)c->sample_rate;     /* raw propagation delay (samples) */
        float maxd = (float)(c->dop_ringlen - 2);          /* keep both interpolation taps in-ring */
        if (dop_ds > maxd) dop_ds = maxd;
        if (v->dop_init) { v->dop_delay = dop_ds; v->dop_dtgt = dop_ds; v->dop_init = false; }  /* snap: no enable glitch */
        /* The read delay is low-passed toward distance/c PER SAMPLE with a 2-pole filter (target then
         * delay, BW_DOPPLER_TAU each). Position is committed per video frame (~60 Hz) but we render many
         * samples per frame, so the raw target is a staircase; its fundamental (the commit rate) would
         * FM-modulate the carrier into audible sidebands (worse at HF). The cutoff (~5 Hz, BW_DOPPLER_TAU
         * = 32 ms) was set with test_doppler_fft: it keeps the commit-rate sidebands below ~ -45 dB at
         * 1 kHz / -38 dB at 4 kHz. Low-passing the delay preserves the ramp's SLOPE, so steady-motion
         * pitch is exact; it only offsets the delay value by ~v*group_delay (sub-millisecond), and
         * rounds pitch transitions over the group delay (natural). A plain velocity tracker is worse
         * here (it overshoots each staircase step). */
        dop_k = 1.f / (BW_DOPPLER_TAU * (float)c->sample_rate);
        if (dop_k > 0.5f) dop_k = 0.5f;
        dring = c->dop_ring + (size_t)idx * c->dop_ringlen; dmask = c->dop_ringlen - 1;
    }

    /* streaming sounds: pull this block's mono samples from the background ring (no I/O on the audio
     * thread). want covers [start, n); a short pull (underrun or EOF) leaves the tail silent. */
    const int streaming = (snd->stream != NULL);
    uint32_t strm_got = 0;
    if (streaming) {
        uint32_t want = (start < n) ? (n - start) : 0;
        strm_got = stream_pull(snd->stream, v->stream_pos, c->stream_scratch + start, want);
        for (uint32_t i = start + strm_got; i < n; ++i) c->stream_scratch[i] = 0.f;
    }

    /* pathing: SH-encode the UN-occluded source (the indirect path goes around the occluder, so it is
     * not occluded by the direct-path occlusion) into the shared ambisonic accumulator. Read the sim's
     * published field (handle-gated double buffer) once and ramp toward it (invariant 4). */
    const int path_on = v->path_on && c->path_tap && c->path_ambi_ch;
    const uint32_t pac = c->path_ambi_ch;
    float path_tgt[BW_AMBI_CH], path_step[BW_AMBI_CH];
    float pco_tgt[3][5], pco_step[3][5];      /* pathing bending-loss EQ: block target coeffs + per-sample glide */
    int   path_flat = 1;
    if (path_on) {
        int pf = atomic_load_explicit(&c->path_idx[idx], memory_order_acquire);
        const PathPub* pp = &c->path_pub[(size_t)idx * 2 + (size_t)pf];
        const int mine_path = (pp->handle == myh);
        for (uint32_t k = 0; k < pac; ++k) path_tgt[k] = mine_path ? pp->sh[k] : 0.f;
        for (uint32_t k = 0; k < pac; ++k) path_step[k] = (path_tgt[k] - v->path_sh_cur[k]) / (float)n;
        /* bending-loss EQ: glide the band gains toward the published tilt (flat when not mine), then
         * compute this block's target biquad coeffs and interpolate the live coeffs per sample — the
         * same low-shelf/peak/high-shelf cascade the occlusion EQ uses, applied to s_raw pre-encode
         * (phonon's own path effect: EQ the mono signal, then scale each SH channel — path_effect.cpp). */
        float peq_tgt[3];
        for (int b = 0; b < 3; ++b) {
            peq_tgt[b] = mine_path ? pp->eq[b] : 1.f;
            v->path_eqg_cur[b] += (peq_tgt[b] - v->path_eqg_cur[b]) * EQ_SLEW;
            if (v->path_eqg_cur[b] < 1.f - EQ_FLAT_EPS || v->path_eqg_cur[b] > 1.f + EQ_FLAT_EPS) path_flat = 0;
        }
        if (!path_flat) v->path_eq_engaged = 1;
        if (v->path_eq_engaged) for (int b = 0; b < 3; ++b) {
            if (path_flat) { pco_tgt[b][0] = 1.f; pco_tgt[b][1] = pco_tgt[b][2] = pco_tgt[b][3] = pco_tgt[b][4] = 0.f; }
            else eq_coeffs(c->eq_proto[b].type, c->eq_proto[b].cw0, c->eq_proto[b].alpha, v->path_eqg_cur[b], pco_tgt[b]);
            for (int k = 0; k < 5; ++k) pco_step[b][k] = (pco_tgt[b][k] - v->path_eq_co[b][k]) / (float)n;
        }
    }

    uint32_t cur = v->cursor;
    bool ended = false;
    for (uint32_t i = 0; i < n; ++i) {
        if (i < start) continue;               /* scheduled start: this voice stays silent (and frozen) until the in-block offset */
        if (!streaming && cur >= snd->frames) {
            if (v->loop) cur = 0;
            else         ended = true;
        }
        float s = streaming ? c->stream_scratch[i] : (ended ? 0.f : snd->pcm[cur]);
        const float s_raw = s;                                 /* pre-occlusion source, for the indirect (pathing) field */
        if (v->eq_engaged) {                                    /* 3 biquads (DF-I), coeffs interpolated per sample */
            for (int b = 0; b < 3; ++b) {
                float* co = v->eq_co[b];
                float y = co[0]*s + co[1]*v->eq_x1[b] + co[2]*v->eq_x2[b] - co[3]*v->eq_y1[b] - co[4]*v->eq_y2[b];
                v->eq_x2[b]=v->eq_x1[b]; v->eq_x1[b]=s; v->eq_y2[b]=v->eq_y1[b]; v->eq_y1[b]=y; s=y;
                for (int k = 0; k < 5; ++k) co[k] += co_step[b][k];   /* glide toward the block target */
            }
        }
        s *= v->occ_cur * v->dir_cur;                           /* occlusion level + directivity, pre-pan */
        if (do_send) { aux[i] += s * v->refl_g_cur; v->refl_g_cur += refl_step; }  /* reverb send: pre-propagation, distance/level-scaled */
        if (v->air_on) {                                        /* air absorption: distance one-pole LPF (direct path) */
            v->air_y1 += v->air_a_cur * (s - v->air_y1); s = v->air_y1; v->air_a_cur += air_a_step;
        }
        if (dring) {                                            /* Doppler: write, read at the gliding fractional delay */
            dring[v->dop_w & dmask] = s;
            uint32_t di = (uint32_t)v->dop_delay;               /* integer delay; (dop_w-di) stays integer (no float index) */
            float    df = v->dop_delay - (float)di;             /* fractional delay [0,1) */
            float newer = dring[(v->dop_w - di)     & dmask];   /* di samples ago */
            float older = dring[(v->dop_w - di - 1) & dmask];   /* di+1 samples ago */
            s = newer * (1.f - df) + older * df;
            v->dop_w++;
            v->dop_dtgt  += (dop_ds      - v->dop_dtgt)  * dop_k;   /* 2-pole, per sample: smooth the target ... */
            v->dop_delay += (v->dop_dtgt - v->dop_delay) * dop_k;   /* ... then the read delay -> continuous rate */
        }
        if (!streaming && !ended) ++cur;
        v->occ_cur += occ_step;
        v->dir_cur += dir_step;
        if (path_on) {                                         /* SH-encode the indirect field (decoded later by the tap) */
            float sp = s_raw;                                  /* the indirect path is un-occluded (it bends around) ... */
            if (v->path_eq_engaged) for (int b = 0; b < 3; ++b) {   /* ... but takes the bending-loss tilt (3 biquads, DF-I) */
                float* co = v->path_eq_co[b];
                float y = co[0]*sp + co[1]*v->path_eq_x1[b] + co[2]*v->path_eq_x2[b] - co[3]*v->path_eq_y1[b] - co[4]*v->path_eq_y2[b];
                v->path_eq_x2[b]=v->path_eq_x1[b]; v->path_eq_x1[b]=sp; v->path_eq_y2[b]=v->path_eq_y1[b]; v->path_eq_y1[b]=y; sp=y;
                for (int k = 0; k < 5; ++k) co[k] += pco_step[b][k];   /* glide toward the block target */
            }
            for (uint32_t k = 0; k < pac; ++k) {
                c->path_accum[(size_t)k * n + i] += sp * v->path_sh_cur[k];
                v->path_sh_cur[k] += path_step[k];
            }
        }
        if (dual) {
            float lo = v->xover_lp + xover_a * (s - v->xover_lp); v->xover_lp = lo;   /* LP @ 700 Hz */
            float hi = s - lo;                                                        /* complementary HP */
            for (uint32_t ch = 0; ch < c->channels; ++ch) {
                bus[(size_t)ch * n + i] += v->gcur_lo[ch] * lo + v->gcur[ch] * hi;
                v->gcur_lo[ch] += step_lo[ch]; v->gcur[ch] += step[ch];
            }
        } else {
            for (uint32_t ch = 0; ch < c->channels; ++ch) {
                bus[(size_t)ch * n + i] += v->gcur[ch] * s;
                v->gcur[ch] += step[ch];
            }
        }
    }
    v->cursor = cur;
    if (path_on) for (uint32_t k = 0; k < pac; ++k) v->path_sh_cur[k] = path_tgt[k];   /* land exactly */
    if (path_on && v->path_eq_engaged) {                        /* land the pathing-EQ coeffs; bypass once settled flat */
        for (int b = 0; b < 3; ++b) for (int k = 0; k < 5; ++k) v->path_eq_co[b][k] = pco_tgt[b][k];
        if (path_flat) { v->path_eq_engaged = 0;
            for (int b = 0; b < 3; ++b) { v->path_eq_x1[b]=v->path_eq_x2[b]=v->path_eq_y1[b]=v->path_eq_y2[b]=0.f; } }
    }
    if (streaming) {                                            /* advance the stream position; end at a true EOF (not underrun) */
        v->stream_pos += strm_got;
        if (stream_ended(snd->stream, v->stream_pos)) ended = true;
    }
    v->occ_cur = occ_tgt;                                        /* land exactly (same local) */
    v->dir_cur = dir_tgt;
    if (v->air_on) v->air_a_cur = air_a_tgt;                     /* land the ramped propagation params */
    if (do_send)   v->refl_g_cur = refl_tgt;                     /* (the Doppler delay self-tracks per sample) */
    if (dual) for (uint32_t ch = 0; ch < c->channels; ++ch) v->gcur_lo[ch] = v->gtarget_lo[ch];  /* land lo band */
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

/* Mix an ambisonic BED voice: decode its SH channels straight onto the 26-ch bus through the static
 * decode matrix (world-locked — no DBAP, occlusion, or directivity), with a master-gain ramp on
 * gcur[0]. Looping / natural end / oneshot-ack are identical to mix_voice. */
static void mix_bed(RtCore* c, Voice* v, uint16_t idx, float* bus, uint32_t n, uint32_t start) {
    const SoundData* snd = v->sound;
    const int nch = (int)snd->channels;
    const float g_step = (v->gtarget[0] - v->gcur[0]) / (float)n;   /* master gain ramp (invariant 4) */
    uint32_t cur = v->cursor;
    bool ended = false;
    for (uint32_t i = 0; i < n; ++i) {
        if (i < start) continue;               /* scheduled start: silent until the in-block offset */
        if (cur >= snd->frames) { if (v->loop) cur = 0; else ended = true; }
        if (!ended) {
            const float* sh = &snd->pcm[(size_t)cur * nch];
            const float g = v->gcur[0];
            for (uint32_t s = 0; s < c->channels; ++s) {
                const float* D = c->bed_decode[s];
                float acc = 0.f;
                for (int k = 0; k < nch; ++k) acc += sh[k] * D[k];
                bus[(size_t)s * n + i] += g * acc;
            }
            ++cur;
        }
        v->gcur[0] += g_step;
    }
    v->cursor = cur;
    v->gcur[0] = v->gtarget[0];
    if (ended) {
        v->playing = false;
        if (v->oneshot) {
            v->active = false;
            Evt ev = { .type = EVT_VOICE_ENDED, .handle = BW_MK_H(idx, v->gen) };
            evt_push(&c->events, &ev);
        }
    }
}

void rt_render(RtCore* c, float* bus, uint32_t nframes, const BwTimestamp* ts) {
    /* dsp clock for sample-accurate scheduling: prefer the device sample position (ASIO/null); fall
     * back to an internal block counter when no timestamp is supplied (e.g. direct rt_render in tests).
     * block_start is the absolute dsp-sample of bus[0]; publish it so the control thread can schedule
     * relative to "now" (rt_dsp_time). */
    uint64_t block_start = ts ? ts->sample_pos : c->dsp_block;
    c->dsp_block = block_start + nframes;
    atomic_store_explicit(&c->dsp_now, block_start, memory_order_relaxed);
#if defined(_MSC_VER)
    /* Flush denormals to zero on the audio thread: gain ramps toward 0 (e.g. a voice
     * moving off a channel) otherwise produce subnormals that stall the FP pipeline. */
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
    BW_ZONE_BEGIN(zr, "rt_render");
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
    /* the reflection aux send: collected this block if a tap is registered + the block fits the scratch */
    float* aux = (c->bus_tap && nframes <= BW_RT_MAX_BLOCK) ? c->aux : NULL;
    if (aux) memset(aux, 0, sizeof(float) * (size_t)nframes);
    const int path_active = (c->path_tap && c->path_ambi_ch && nframes <= BW_RT_MAX_BLOCK);
    if (path_active) memset(c->path_accum, 0, sizeof(float) * (size_t)c->path_ambi_ch * nframes);  /* pathing accumulator */
    int rt_active = 0;
    BW_ZONE_BEGIN(zmix, "mix voices");
    for (uint32_t i = 0; i < c->voice_cap; ++i) {
        Voice* v = &c->voices[i];
        if (!v->active || !v->playing || !v->sound) continue;
        ++rt_active;
        /* scheduled start: hold the voice until the block that contains its start_sample, then begin
         * at the exact in-block offset (sample-accurate). 0 = play immediately. */
        uint32_t off = 0;
        if (v->start_sample > block_start) {
            uint64_t d = v->start_sample - block_start;
            if (d >= nframes) continue;                /* starts in a later block: nothing to mix yet */
            off = (uint32_t)d;
        }
        if (v->dirty) { compute_gains(c, v); v->dirty = false; }
        if (v->sound->channels > 1) mix_bed  (c, v, (uint16_t)i, bus, nframes, off);        /* ambisonic bed */
        else                        mix_voice(c, v, (uint16_t)i, bus, nframes, off, aux);   /* mono point source */
    }
    BW_ZONE_END(zmix);
    BW_PLOT("rt voices", rt_active);
    for (uint32_t i = 0; i < c->voice_cap; ++i) {   /* publish playback state for rt_source_is_playing (control thread) */
        const Voice* v = &c->voices[i];
        uint32_t st = ((uint32_t)v->gen << 1) | ((v->active && v->playing && v->sound) ? 1u : 0u);
        atomic_store_explicit(&c->play_pub[i], st, memory_order_release);
    }
    if (aux) {   /* reflection bed: convolve the aux send + sum onto the bus BEFORE align (so it gets trim+delay too) */
        BW_ZONE_BEGIN(zt, "reflect tap");
        c->bus_tap(c->bus_tap_ud, bus, nframes, c->lis.p_active, c->lis.q_active, aux);
        BW_ZONE_END(zt);
    }
    if (path_active) {   /* pathing: decode the summed indirect ambisonic field onto the bus (also pre-align) */
        BW_ZONE_BEGIN(zp, "path tap");
        c->path_tap(c->path_tap_ud, bus, nframes, c->lis.p_active, c->lis.q_active, c->path_accum, c->path_ambi_ch);
        BW_ZONE_END(zp);
    }
    BW_ZONE_BEGIN(za, "align");
    align_process(c->aligner, bus, nframes);   /* per-speaker gain trim + delay (output stage) */
    BW_ZONE_END(za);

    /* debug channel test (bw_test_signal): inject a built-in signal onto a raw output channel AFTER
     * align, so it is independent of the per-speaker trim/delay — a clean speaker-check / wiring tool. */
    for (uint32_t ch = 0; ch < c->channels; ++ch) {
        uint8_t k = c->test_kind[ch];
        if (!k) continue;
        float g = c->test_gain[ch];
        float* out = &bus[(size_t)ch * nframes];
        if (k == 1) {                              /* sine, ~660 Hz */
            const float inc = 6.2831853f * 660.0f / (float)c->sample_rate;
            float ph = c->test_phase[ch];
            for (uint32_t i = 0; i < nframes; ++i) { out[i] += g * sinf(ph); ph += inc; if (ph > 6.2831853f) ph -= 6.2831853f; }
            c->test_phase[ch] = ph;
        } else {                                   /* white noise (shared LCG) */
            uint32_t n = c->test_noise;
            for (uint32_t i = 0; i < nframes; ++i) { n = n * 1664525u + 1013904223u; out[i] += g * ((float)(n >> 9) * (1.0f / 4194304.0f) - 1.0f); }
            c->test_noise = n;
        }
    }
    pose_write(&c->readback, c->lis.p_active, c->lis.q_active);   /* publish for control-thread readback */
    BW_ZONE_END(zr);
}

void rt_read_pose(RtCore* c, float p[3], float q[4]) {
    if (!c) return;
    if (!pose_read(&c->readback, p, q)) {       /* lost the seqlock race (rare): best-effort direct read */
        memcpy(p, c->lis.p_active, sizeof(float) * 3);
        memcpy(q, c->lis.q_active, sizeof(float) * 4);
    }
}

/* Control-thread readback: is the source's voice still producing audio? Reads the per-slot state the
 * audio thread republishes each block, gated on the handle's generation (a stale/recycled handle, or
 * a finished non-loop voice, reads as not-playing). Best-effort: a sound shorter than a poll interval
 * may never be observed playing. */
bool rt_source_is_playing(RtCore* c, uint32_t h) {
    if (!c || h == 0) return false;
    uint32_t idx = BW_H_IDX(h);
    if (idx >= c->voice_cap) return false;
    uint32_t st = atomic_load_explicit(&c->play_pub[idx], memory_order_acquire);
    return ((st >> 1) == BW_H_GEN(h)) && (st & 1u) != 0u;
}

/* Control thread: the engine's current dsp-sample clock (the most recently rendered block's first
 * sample). Schedule a sample-accurate play with rt_source_play_at(.., rt_dsp_time(c) + delay_samples). */
uint64_t rt_dsp_time(RtCore* c) {
    return c ? atomic_load_explicit(&c->dsp_now, memory_order_relaxed) : 0;
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
    if (!h) {                               /* pool full: steal the lowest-priority active source for the new one */
        int victim = -1, lowest = 256;
        for (uint32_t i = 0; i < c->voice_cap; ++i)   /* 255 = protected: never stolen */
            if (c->inuse[i] && c->priority[i] < 255 && (int)c->priority[i] < lowest) { lowest = c->priority[i]; victim = (int)i; }
        if (victim >= 0) {
            rt_source_destroy(c, BW_MK_H((uint16_t)victim, c->gen[victim]));   /* stops it + frees the slot */
            h = alloc_handle(c);
        }
        if (!h) return 0;                   /* nothing to steal (or the destroy didn't enqueue): genuinely full */
    }
    uint16_t idx = BW_H_IDX(h);
    c->priority[idx] = 128;                 /* default mid priority until the caller sets one */
    Cmd cmd = { .type = CMD_SRC_CREATE, .handle = h };
    if (!cmd_push(&c->cmds, &cmd)) {        /* ring full (should never happen): don't leak the slot */
        recycle_handle(c, idx);
        return 0;
    }
    return h;
}

/* Steal priority (control-side only — the audio thread never reads it): 0 = first to be stolen when
 * the pool is full .. 255 = protected. Take effect immediately; safe any time. */
void rt_source_set_priority(RtCore* c, uint32_t h, int priority) {
    if (!c) return;
    uint16_t idx = BW_H_IDX(h);
    if (idx < c->voice_cap && c->inuse[idx] && c->gen[idx] == BW_H_GEN(h))
        c->priority[idx] = (uint8_t)(priority < 0 ? 0 : priority > 255 ? 255 : priority);
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

void rt_source_set_reflections(RtCore* c, uint32_t h, bool on) {
    Cmd cmd = { .type = CMD_SET_REFLECTIONS, .handle = h };
    cmd.u.refl.on = on ? 1u : 0u;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_set_pathing(RtCore* c, uint32_t h, bool on) {
    Cmd cmd = { .type = CMD_SET_PATHING, .handle = h };
    cmd.u.path.on = on ? 1u : 0u;
    cmd_push(&c->cmds, &cmd);
}

void rt_set_path_tap(RtCore* c, RtPathTap tap, void* ud, uint32_t ambi_ch) {
    if (!c) return;
    c->path_tap = tap; c->path_tap_ud = ud;
    c->path_ambi_ch = (ambi_ch > BW_AMBI_CH) ? BW_AMBI_CH : ambi_ch;   /* set while the audio thread is stopped */
}

/* Off-thread pathing sim publishes a voice's path field: write the back buffer, flip the index (release).
 * `sh` = shCoeffs (directions + level); `eq` = the 3-band bending-loss tilt (NULL = flat). Handle stored
 * alongside so the audio thread drops a stale/recycled slot. */
void rt_set_pathing(RtCore* c, uint32_t handle, const float* sh, const float* eq, uint32_t ambi_ch) {
    if (!c || !sh) return;
    uint32_t idx = BW_H_IDX(handle);
    if (idx >= c->voice_cap) return;
    if (ambi_ch > BW_AMBI_CH) ambi_ch = BW_AMBI_CH;
    int cur = atomic_load_explicit(&c->path_idx[idx], memory_order_relaxed);
    PathPub* back = &c->path_pub[(size_t)idx * 2 + (size_t)(1 - cur)];
    back->handle = handle;
    for (uint32_t k = 0; k < ambi_ch; ++k)        back->sh[k] = sh[k];
    for (uint32_t k = ambi_ch; k < BW_AMBI_CH; ++k) back->sh[k] = 0.f;
    for (int b = 0; b < 3; ++b) back->eq[b] = eq ? eq[b] : 1.f;   /* NULL eq = flat (no bending loss) */
    atomic_store_explicit(&c->path_idx[idx], 1 - cur, memory_order_release);
}

void rt_source_set_reflection_send(RtCore* c, uint32_t h, float gain) {
    if (!isfinite(gain) || gain < 0.f) return;
    Cmd cmd = { .type = CMD_SET_REFL_SEND, .handle = h };
    cmd.u.rsend.gain = gain;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_set_reflection_distance(RtCore* c, uint32_t h, bool on) {
    Cmd cmd = { .type = CMD_SET_REFL_DIST, .handle = h };
    cmd.u.rdist.on = on ? 1u : 0u;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_set_doppler(RtCore* c, uint32_t h, bool on) {
    Cmd cmd = { .type = CMD_SET_DOPPLER, .handle = h };
    cmd.u.dop.on = on ? 1u : 0u;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_set_air_absorption(RtCore* c, uint32_t h, bool on) {
    Cmd cmd = { .type = CMD_SET_AIR, .handle = h };
    cmd.u.air.on = on ? 1u : 0u;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_set_spread(RtCore* c, uint32_t h, float amount) {
    if (!isfinite(amount)) return;
    Cmd cmd = { .type = CMD_SET_SPREAD, .handle = h };
    cmd.u.spread.amount = amount;
    cmd_push(&c->cmds, &cmd);
}

void rt_test_signal(RtCore* c, uint32_t channel, uint8_t kind, float gain) {
    if (!c) return;
    Cmd cmd = { .type = CMD_TEST_SIGNAL };
    cmd.u.test.channel = channel; cmd.u.test.kind = kind; cmd.u.test.gain = gain;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_play(RtCore* c, uint32_t h, uint32_t sound, bool loop) {
    rt_source_play_at(c, h, sound, loop, 0);   /* 0 = start immediately */
}

void rt_source_play_at(RtCore* c, uint32_t h, uint32_t sound, bool loop, uint64_t start_sample) {
    SoundSlot* s = sound_slot_ctrl(c, sound);
    if (!s || s->retiring) return;          /* invalid or being unloaded: drop the play so the
                                             * audio thread can never bind a retiring sound */
    /* streamed sound: kick the background decode (re-seek + fill) now, off the audio thread. The
     * voice reads its ring from sample 0; the first blocks may be silent until the ring fills (~ms). */
    if (s->data.stream) stream_start(s->data.stream, loop ? 1 : 0);
    Cmd cmd = { .type = CMD_PLAY, .handle = h };
    cmd.u.play.sound = sound; cmd.u.play.loop = loop ? 1u : 0u; cmd.u.play.oneshot = 0u;
    cmd.u.play.start = start_sample;
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

/* Load a sound for STREAMING (mono point source, engine rate): the file is not decoded into RAM —
 * a background thread feeds its ring as the voice plays (rt_source_play). 0 + err on failure. */
uint32_t rt_load_sound_streaming(RtCore* c, const char* path, char* err, size_t errcap) {
    if (!c) return 0;
    Stream* st = stream_open(c->streams, path, err, errcap);
    if (!st) return 0;
    uint32_t h = salloc_sound(c);
    if (!h) {
        stream_close(c->streams, st);
        if (err && errcap) { strncpy(err, "stream: sound table full", errcap - 1); err[errcap - 1] = 0; }
        return 0;
    }
    SoundData d; memset(&d, 0, sizeof d);
    d.stream = st; d.channels = 1; d.sample_rate = c->sample_rate;   /* pcm NULL, frames 0: the stream tracks EOF */
    c->sounds[BW_H_IDX(h)].data = d;
    return h;
}

/* Load a multichannel AmbiX asset into the sound table (plays as an ambisonic bed via mix_bed). */
uint32_t rt_load_ambix(RtCore* c, const char* path, char* err, size_t errcap) {
    SoundData d;
    if (!sound_load_ambix(path, c->sample_rate, &d, err, errcap)) return 0;
    uint32_t h = salloc_sound(c);
    if (!h) {
        sound_unload(&d);
        if (err && errcap) { strncpy(err, "ambix: sound table full", errcap - 1); err[errcap - 1] = 0; }
        return 0;
    }
    c->sounds[BW_H_IDX(h)].data = d;
    return h;
}

/* Channel count of a loaded asset (control thread): 1 = mono point source, 4/9/16 = ambisonic bed.
 * 0 if the handle is invalid. Lets the engine reject a type-mismatched play (mono on a bed / vice versa). */
uint16_t rt_sound_channels(RtCore* c, uint32_t sound) {
    SoundSlot* s = sound_slot_ctrl(c, sound);
    return s ? s->data.channels : 0;
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
    c->xover_a     = 1.f - expf(-6.2831853f * BW_DUALBAND_FC / (float)sample_rate);   /* dual-band crossover */
    c->test_noise  = 0x9e3779b9u;       /* non-zero LCG seed for the channel-test noise */
    c->voices    = (Voice*)    calloc(voice_cap, sizeof(Voice));
    c->occ_handle = (_Atomic uint32_t*)calloc(voice_cap, sizeof(_Atomic uint32_t));
    c->occ_val    = (_Atomic float*)   calloc(voice_cap, sizeof(_Atomic float));
    c->occ_eq     = (_Atomic uint64_t*)calloc(voice_cap, sizeof(_Atomic uint64_t));
    c->occ_dir    = (_Atomic float*)   calloc(voice_cap, sizeof(_Atomic float));
    c->play_pub   = (_Atomic uint32_t*)calloc(voice_cap, sizeof(_Atomic uint32_t));
    c->aux        = (float*)   calloc(BW_RT_MAX_BLOCK, sizeof(float));   /* reflection aux-send scratch */
    c->stream_scratch = (float*)calloc(BW_RT_MAX_BLOCK, sizeof(float));  /* per-block streaming pull scratch */
    c->path_accum = (float*)calloc((size_t)BW_AMBI_CH * BW_RT_MAX_BLOCK, sizeof(float));  /* pathing ambisonic accumulator */
    c->path_pub   = calloc((size_t)voice_cap * 2, sizeof *c->path_pub);  /* per-voice double-buffered path field */
    c->path_idx   = (_Atomic int*)calloc(voice_cap, sizeof(_Atomic int));
    c->streams    = stream_set_create(sample_rate);                     /* background file streaming */
    c->gen       = (uint16_t*) calloc(voice_cap, sizeof(uint16_t));
    c->inuse     = (uint8_t*)  calloc(voice_cap, sizeof(uint8_t));
    c->priority  = (uint8_t*)  calloc(voice_cap, sizeof(uint8_t));
    c->freelist  = (uint32_t*) calloc(voice_cap, sizeof(uint32_t));
    c->sounds    = (SoundSlot*)calloc(sound_cap, sizeof(SoundSlot));
    c->sfreelist = (uint32_t*) calloc(sound_cap, sizeof(uint32_t));
    {   /* Doppler ring pool: power-of-two ring per voice, sized to BW_DOPPLER_MAX_DIST at this rate */
        uint32_t need = (uint32_t)(BW_DOPPLER_MAX_DIST / BW_SPEED_OF_SOUND * (float)sample_rate) + 2;
        uint32_t rl = 1; while (rl < need) rl <<= 1;
        c->dop_ringlen = rl;
        c->dop_ring = (float*)calloc((size_t)voice_cap * rl, sizeof(float));
    }
    if (!c->voices || !c->occ_handle || !c->occ_val || !c->occ_eq || !c->occ_dir || !c->play_pub || !c->aux ||
        !c->stream_scratch || !c->streams || !c->path_accum || !c->path_pub || !c->path_idx ||
        !c->gen || !c->inuse || !c->priority || !c->freelist || !c->sounds || !c->sfreelist || !c->dop_ring) {
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
    build_bed_decode(c);                        /* ambisonic bed decode from the default layout */
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
    build_bed_decode(c);                         /* re-derive the bed decode for the new geometry */
    c->layout_gen++;                             /* the SPCAP cache self-invalidates on the next gains call */
    for (uint32_t i = 0; i < c->voice_cap; ++i)
        if (c->voices[i].active) c->voices[i].dirty = true;
}

/* Select the panner: 0 = DBAP (default, moving observer), 1 = SPCAP, 2 = VBAP (both fixed observer).
 * Atomic-release store, and the SPCAP/VBAP caches self-invalidate on listener/layout change, so this
 * is safe to call at runtime (live A/B) as well as before bw_start. */
void rt_set_panner(RtCore* c, int panner) {
    if (!c) return;
    atomic_store_explicit(&c->panner, (panner >= 0 && panner <= 2) ? panner : 0, memory_order_release);
}

/* Dual-band panning: 0 = off (power panning across the band), 1 = on (amplitude below BW_DUALBAND_FC,
 * power above). The low-band gains are computed every gain solve, so this is a live A/B atomic. */
void rt_set_dual_band(RtCore* c, int on) {
    if (!c) return;
    atomic_store_explicit(&c->dual_band, on ? 1 : 0, memory_order_release);
}

/* Select the diffuse-bed decoder: 0 = sampling (SAD, default), 1 = AllRAD. Rebuilds the decode matrix
 * the audio thread reads, so call BEFORE bw_start (or while stopped), like rt_set_layout. */
void rt_set_bed_decoder(RtCore* c, int decoder) {
    if (!c) return;
    c->bed_decoder = (decoder == 1) ? 1 : 0;
    build_bed_decode(c);
}

void rt_set_tracker(RtCore* c, const PoseSlot* slot) {
    if (c) c->tracker = slot;               /* audio thread reads it; set while stopped */
}

void rt_set_bus_tap(RtCore* c, RtBusTap tap, void* ud) {
    if (c) { c->bus_tap = tap; c->bus_tap_ud = ud; }   /* audio thread reads them; set while stopped */
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
    free(c->priority);
    free(c->inuse);
    free(c->gen);
    free(c->dop_ring);
    stream_set_destroy(c->streams);     /* stops the streaming thread, releases every open stream + ring */
    free(c->stream_scratch);
    free(c->path_accum);
    free(c->path_pub);
    free((void*)c->path_idx);
    free(c->aux);
    free((void*)c->occ_dir);            /* cast drops the _Atomic qualifier for free() */
    free((void*)c->play_pub);
    free((void*)c->occ_eq);
    free((void*)c->occ_val);
    free((void*)c->occ_handle);
    free(c->voices);
    free(c);
}
