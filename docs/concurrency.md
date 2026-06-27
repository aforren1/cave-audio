# Concurrency & real-time model

This is the most load-bearing document. The whole engine is two SPSC rings and a
voice table, with a hard split between the control thread and the audio thread.
The `Voice`, `Sound`, `Layout`, `Listener`, and `BwEngine` structs the code below uses are
defined in [`internal-types.md`](./internal-types.md).

## Threads

- **Control thread** — whatever calls the `bw_*` API (the engine main thread). Owns
  handle allocation, the slot free-list, generation tables, and asset memory. Does
  all wav decode, malloc/free, file I/O, logging.
- **Audio thread** — the ASIO driver's `bufferSwitch` callback *is* this thread.
  Owns all DSP state: the voice table, the 26-ch bus, per-voice gains, the listener
  active fields. Never allocates, locks, blocks, or does I/O.

They communicate only through two rings:
- **Command ring** (control → audio): source/listener updates, play/stop, commit.
- **Event ring** (audio → control): voice-ended, sound-retired acks.

Both are single-producer/single-consumer, so indices need only acquire/release —
no CAS.

## Why each mechanism exists

| mechanism                         | problem it solves                                            |
|-----------------------------------|-------------------------------------------------------------|
| SPSC command ring                 | get control onto the audio thread without locks             |
| staging→active + `CMD_COMMIT`     | the mixer never sees a half-updated frame (listener moved, source not) |
| per-voice `dirty` flag            | skip DBAP recompute for static sources                      |
| gain ramp `gcur→gtarget`          | avoid zipper noise on position jumps                        |
| generation-counted handles        | reuse source slots safely with no round-trip                |
| retire-ack via event ring         | free sound buffers only after the audio thread lets go      |

## Command type and ring

Fixed-size POD slots — no framing needed.

```c
#include <stdatomic.h>

typedef enum : uint8_t {
    CMD_SRC_CREATE, CMD_SRC_DESTROY, CMD_SET_POS, CMD_SET_GAIN,
    CMD_PLAY, CMD_STOP, CMD_SET_LISTENER, CMD_COMMIT, CMD_SOUND_RETIRE,
} CmdType;

typedef struct {
    CmdType  type;
    uint32_t handle;            // source (index|gen) or sound handle
    union {
        struct { float x, y, z; }                   pos;
        struct { float g; }                         gain;
        struct { uint32_t sound; uint8_t loop; }    play;
        struct { float px,py,pz, qx,qy,qz,qw; }     lis;
    } u;
} Cmd;

#define RING_CAP 4096                 // power of two; sized for worst-case frame burst
typedef struct {
    Cmd slots[RING_CAP];
    _Atomic uint32_t write, read;     // free-running; occupancy = write - read
} CmdRing;

#define H_IDX(h)   ((uint16_t)((h) & 0xFFFFu))
#define H_GEN(h)   ((uint16_t)((h) >> 16))
#define MK_H(i,g)  ((uint32_t)(i) | ((uint32_t)(g) << 16))
```

## Event type and ring

The return path is symmetric and likewise SPSC, but with the roles reversed: the **audio thread is
the sole producer** (it pushes from `drain_commands`/the block loop) and the **control thread is the
sole consumer** (`drain_events`, called from `bw_commit`). There is no second producer — keep it
that way, or the relaxed/acquire/release scheme below stops being sufficient.

```c
typedef enum : uint8_t {
    EVT_VOICE_ENDED,     // non-looping voice hit end-of-buffer; control thread may recycle the handle
    EVT_SOUND_RETIRED,   // audio thread has dropped all refs to a sound; control thread may free it
} EvtType;

typedef struct {
    EvtType  type;
    uint32_t handle;     // source handle (VOICE_ENDED) or sound handle (SOUND_RETIRED)
} Evt;

#define EVT_CAP 1024                  // power of two
typedef struct {
    Evt slots[EVT_CAP];
    _Atomic uint32_t write, read;     // write owned by audio thread, read by control thread
} EvtRing;

// audio thread only:
static bool ring_push_evt(EvtRing* r, const Evt* ev) {
    uint32_t w  = atomic_load_explicit(&r->write, memory_order_relaxed);
    uint32_t rd = atomic_load_explicit(&r->read,  memory_order_acquire);
    if (w - rd >= EVT_CAP) return false;            // full: control thread is lagging
    r->slots[w & (EVT_CAP - 1)] = *ev;
    atomic_store_explicit(&r->write, w + 1, memory_order_release);
    return true;
}
```

## Producer side (control thread)

Per-frame calls are pure encode-and-push; they never touch voice state.

```c
static bool ring_push(CmdRing* r, const Cmd* c) {
    uint32_t w  = atomic_load_explicit(&r->write, memory_order_relaxed);
    uint32_t rd = atomic_load_explicit(&r->read,  memory_order_acquire);
    if (w - rd >= RING_CAP) return false;                  // full
    r->slots[w & (RING_CAP - 1)] = *c;
    atomic_store_explicit(&r->write, w + 1, memory_order_release);
    return true;
}

void bw_source_set_pos(BwEngine* e, BwSource s, float x, float y, float z) {
    Cmd c = { .type = CMD_SET_POS, .handle = s, .u.pos = { x, y, z } };
    ring_push(&e->cmds, &c);
}

void bw_commit(BwEngine* e) {
    Cmd c = { .type = CMD_COMMIT };
    ring_push(&e->cmds, &c);
    drain_events(e);              // consume VOICE_ENDED / SOUND_RETIRED here
}
```

`bw_source_create` is the one call that does **not** round-trip. The control thread
owns the slot free-list and generation table, so it allocates an index, bumps the
generation, returns `MK_H(idx,gen)` synchronously, and enqueues `CMD_SRC_CREATE`.
Synchronous handle, async activation.

## Consumer side (audio thread) — the snapshot

Drain runs once at block start. Structural commands apply immediately; continuous
parameters write to *pending* and promote to *active* only on `CMD_COMMIT`. The
mixer reads only active fields, so a frame the producer hasn't finished (set_pos
sent, commit not yet) leaves pending half-updated but active untouched — and because
pending is latest-wins and persists across blocks, the straggler promotes cleanly on
the next commit. No need to hold commands back in the ring.

```c
static Voice* voice_for(BwEngine* e, uint32_t h) {
    uint16_t i = H_IDX(h);
    if (i >= e->voice_cap) return NULL;
    Voice* v = &e->voices[i];
    return (v->active && v->gen == H_GEN(h)) ? v : NULL;   // stale gen => dropped
}

static void drain_commands(BwEngine* e) {
    CmdRing* r = &e->cmds;
    uint32_t rd = atomic_load_explicit(&r->read,  memory_order_relaxed);
    uint32_t w  = atomic_load_explicit(&r->write, memory_order_acquire);
    for (; rd != w; ++rd) {
        const Cmd* c = &r->slots[rd & (RING_CAP - 1)];
        switch (c->type) {
        case CMD_SRC_CREATE: {
            Voice* v = &e->voices[H_IDX(c->handle)];
            *v = (Voice){0};
            v->gen = H_GEN(c->handle); v->active = true;
            v->gain_user = 1.f; v->dirty = true;
        } break;
        case CMD_SRC_DESTROY: { Voice* v = voice_for(e, c->handle);
            if (v) { v->active = v->playing = false; v->sound = NULL; } } break;
        case CMD_SET_POS: { Voice* v = voice_for(e, c->handle);
            if (v) memcpy(v->pos_pending, &c->u.pos, sizeof v->pos_pending); } break;
        case CMD_SET_GAIN: { Voice* v = voice_for(e, c->handle);
            if (v) { v->gain_user = c->u.gain.g; v->dirty = true; } } break;
        case CMD_PLAY: { Voice* v = voice_for(e, c->handle);
            const Sound* s = sound_for(e, c->u.play.sound);
            if (v && s) { v->sound = s; v->cursor = 0; v->loop = c->u.play.loop;
                          v->playing = true; v->dirty = true; } } break;
        case CMD_STOP: { Voice* v = voice_for(e, c->handle);
            if (v) v->playing = false; } break;
        case CMD_SET_LISTENER:
            memcpy(e->lis.p_pending, &c->u.lis.px, sizeof(float)*3);
            memcpy(e->lis.q_pending, &c->u.lis.qx, sizeof(float)*4);
            break;
        case CMD_COMMIT: {
            bool lis_moved = memcmp(e->lis.p_active, e->lis.p_pending,
                                    sizeof e->lis.p_active) != 0;
            memcpy(e->lis.p_active, e->lis.p_pending, sizeof e->lis.p_active);
            memcpy(e->lis.q_active, e->lis.q_pending, sizeof e->lis.q_active);
            for (uint32_t i = 0; i < e->voice_cap; ++i) {
                Voice* v = &e->voices[i];
                if (!v->active) continue;
                if (memcmp(v->pos_active, v->pos_pending, sizeof v->pos_active)) {
                    memcpy(v->pos_active, v->pos_pending, sizeof v->pos_active);
                    v->dirty = true;
                }
                if (lis_moved) v->dirty = true;    // gains are all listener-relative
            }
        } break;
        case CMD_SOUND_RETIRE: {
            const Sound* s = sound_for(e, c->handle);
            for (uint32_t i = 0; i < e->voice_cap; ++i)
                if (e->voices[i].sound == s) {
                    e->voices[i].playing = false; e->voices[i].sound = NULL;
                }
            Evt ev = { .type = EVT_SOUND_RETIRED, .handle = c->handle };
            ring_push_evt(&e->events, &ev);        // control thread frees after this
        } break;
        }
    }
    atomic_store_explicit(&r->read, rd, memory_order_release);
}
```

## Two correctness points

- **A listener move dirties every voice**, since DBAP gains are all listener-relative.
  That is the moving-observer case paying its cost: recompute the whole field on
  frames the head moves, nothing on frames it doesn't.
- **Generation counts make slot reuse safe without an ack.** A late `CMD_SET_POS`
  aimed at a destroyed-then-recycled slot fails the `gen` check and is dropped; the
  new voice in that slot has a fresh generation. Voices need no round-trip. Only
  *sound buffers* do — generations don't protect freed memory — hence
  `CMD_SOUND_RETIRE` detaches references and acks back so the control thread frees
  exactly once the audio thread has provably let go.

## Sound lifetime: the retire-ack handshake (control side)

The audio side (above) detaches references on `CMD_SOUND_RETIRE` and acks with `EVT_SOUND_RETIRED`.
The control side closes the loop: `bw_unload_sound` only *requests* retirement and marks the slot;
the actual `free` happens later, when `drain_events` (run from `bw_commit`) sees the ack. Freeing in
`bw_unload_sound` directly would be a use-after-free — an audio block can still be mid-mix on that
buffer.

```c
void bw_unload_sound(BwEngine* e, BwSound snd) {
    Sound* s = sound_slot(e, snd);          // control owns the table; NULL/stale-gen => no-op
    if (!s || s->retiring) return;
    s->retiring = true;                     // no new bw_source_play will bind it (control-side check)
    Cmd c = { .type = CMD_SOUND_RETIRE, .handle = snd };
    ring_push(&e->cmds, &c);                // audio thread detaches voices, then acks
    // do NOT free here — wait for EVT_SOUND_RETIRED.
}

// consumer of the event ring; control thread, called from bw_commit.
static void drain_events(BwEngine* e) {
    EvtRing* r = &e->events;
    uint32_t rd = atomic_load_explicit(&r->read,  memory_order_relaxed);
    uint32_t w  = atomic_load_explicit(&r->write, memory_order_acquire);
    for (; rd != w; ++rd) {
        const Evt* ev = &r->slots[rd & (EVT_CAP - 1)];
        switch (ev->type) {
        case EVT_VOICE_ENDED:
            recycle_source_handle(e, ev->handle);     // free-list + generation bump
            break;
        case EVT_SOUND_RETIRED: {
            Sound* s = sound_slot(e, ev->handle);
            if (s) { free(s->pcm); s->pcm = NULL; recycle_sound_handle(e, ev->handle); }
        } break;
        }
    }
    atomic_store_explicit(&r->read, rd, memory_order_release);
}
```

This is the only handshake the design needs that voices do not: generation counts make a stale
*source* handle a safe no-op, but they do not protect *freed buffer memory* — so sound buffers get
the explicit ack, voices do not (see "Two correctness points" above).

## Block structure

```c
void bufferSwitch_impl(BwEngine* e, long idx, uint32_t nframes) {
    drain_commands(e);
    float* bus = e->bus26;
    memset(bus, 0, sizeof(float) * nframes * 26);
    for (uint32_t i = 0; i < e->voice_cap; ++i) {
        Voice* v = &e->voices[i];
        if (!v->active || !v->playing || !v->sound) continue;
        if (v->dirty) {
            dbap_gains(v->pos_active, e->lis.p_active, &e->layout,
                       v->gain_user, v->gtarget);
            v->dirty = false;
        }
        mix_voice(v, bus, nframes);     // advance cursor; ramp gcur -> gtarget per sample
    }
    align_speakers(e, bus, nframes);    // per-channel gain + delay line
    if (e->monitor) binaural_tap(e->monitor, bus, nframes, &e->lis);
    asio_convert_write(e, idx, bus, nframes);
}
```

`mix_voice` must interpolate `gcur → gtarget` across the block, not slam the new
vector in at block start — see invariant 4 in `CLAUDE.md`. The `dirty` flag means
static sources cost only the multiply-accumulate. End-of-buffer on a non-looping
voice flips `playing` off and pushes `EVT_VOICE_ENDED` so the control thread can
recycle the handle.

## Invariants recap

- Nothing in `drain_commands` or the mix path allocates, locks, or blocks.
- wav decode and malloc/free live entirely on the control thread.
- The ring is sized so a worst-case frame's command burst can't fill it between two
  drains (~one audio block apart). On the should-never-happen full case, prefer
  briefly spinning the producer over dropping a structural command.

That is the entire concurrency surface. Everything else is plain mixing behind it.
