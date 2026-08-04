# Concurrency and real-time model

The spine of the engine is two SPSC rings and a voice table, with a hard split
between the control thread and the audio thread. That spine lives in
[`src/rt.c`](../src/rt.c) / [`src/rt.h`](../src/rt.h).
The `Voice`, `SoundData`, `Layout`, `Listener`, and `RtCore` structs are documented in
[`internal-types.md`](./internal-types.md).

## Threads

Two threads carry the core:

- **Control thread**: whatever calls the `bwa_*` API (the engine main thread).
  Owns handle allocation, the slot free-lists, generation tables, and asset
  memory. Does all file decode, malloc/free, file I/O, and logging.
- **Audio thread**: the ASIO driver's `bufferSwitch` callback (or the null
  sink's pacing thread) *is* this thread. It calls `rt_render`, which owns all
  DSP state: the voice table, the speaker bus, per-voice gains, the listener
  active fields. It never allocates, locks, blocks, or does I/O.

The bus is **N channels wide, where N is the loaded layout's speaker count**
(`RtCore.channels`, 4..26; 26 for the CAVE array and for the default grid). It is
fixed for the engine's lifetime: resolved at `bwa_create`, before `rt_create`.
`BWA_CHANNELS` (26, [`src/sink.h`](../src/sink.h)) is only the compile-time
*capacity* that sizes the fixed arrays.

These two communicate through two SPSC rings:

- **Command ring** (control → audio): source/listener updates, play/stop, commit.
- **Event ring** (audio → control): voice-ended and sound-retired acks.

Both are single-producer/single-consumer, so the indices need only
acquire/release, not CAS.

Around that backbone sit auxiliary producer threads, each with its own channel
into the audio thread. Every channel is wait-free on the audio side; the audio
thread never blocks on any of them:

- **NatNet receiver thread** (`bwa_tracker_connect`): publishes the tracked head pose
  through a single-slot seqlock (`PoseSlot`, [`src/pose.h`](../src/pose.h)).
  `rt_render` samples the freshest pose once per block; if the position moved,
  it dirties every voice. If the reader loses the seqlock race it keeps the
  previous pose: bounded retries, never a block. The audio thread publishes
  the active pose back through a second seqlock slot for control-thread
  readback (`rt_read_pose`).
- **Occlusion sim thread** (`steam_scene.c`): publishes per-voice
  (level, 3-band EQ tilt, directivity) through the `occ_handle` / `occ_val` /
  `occ_eq` / `occ_dir` atomic arrays in `RtCore`. These live *outside* the
  `Voice` struct so a voice-create memset never races a publish. The handle is
  stored last with release; the audio thread gates on its own `v->gen` and
  ramps toward the published values.
- **Pathing sim thread** (`steam_path.c`): publishes per-voice SH coefficients
  plus a bending-loss tilt through a per-voice double buffer (`rt_set_pathing`:
  write the back `PathPub` slot, flip `path_idx` with release).
- **Streaming thread** (`stream.c`): decodes file chunks into per-stream SPSC
  rings. `mix_voice` drains them with `stream_pull`: pure ring reads, no I/O
  (see [`src/stream.h`](../src/stream.h)).

One rule generalizes across all of these: the audio thread owns its state and
*samples* published values. Publishers never touch audio-thread fields directly,
publishes for a stale or recycled handle are dropped on the audio side, and
everything sampled is ramped in, never slammed.

## Why each mechanism exists

| mechanism                          | problem it solves                                            |
|------------------------------------|--------------------------------------------------------------|
| SPSC command ring                  | get control onto the audio thread without locks              |
| staging→active + `CMD_COMMIT`      | the mixer never sees a half-updated frame (listener moved, source not) |
| per-voice `dirty` flag             | skip the gain solve for static sources                       |
| gain ramp `gcur→gtarget`           | avoid zipper noise on position jumps                         |
| generation-counted handles         | reuse source slots safely with no round-trip                 |
| retire-ack via event ring          | free sound buffers only after the audio thread lets go       |
| seqlock pose slot                  | a 7-float pose is too wide for one atomic store; bounded reader retry |
| seqlock ISM-room slot              | a live room change (`bwa_scene_set_box`/`_set_ground`) must never tear under the mixer |
| handle-gated atomic publish        | the occlusion sim may publish for a since-recycled voice     |
| per-voice double buffer (pathing)  | a 16-float SH set must be read consistently, wait-free       |
| per-stream SPSC ring               | file decode off the audio thread; underrun ≠ EOF             |
| fade reserve + `CMD_SRC_STEAL`     | a full voice pool steals without a click                     |

## Command type and ring

Fixed-size POD slots, no framing. The command `enum` is the authoritative
list: it lives in [`rt.h`](../src/rt.h) and grows as features land. Most
commands need no explanation here; the few that carry protocol do:

- `CMD_SRC_CREATE`: async activation of a handle the control thread already
  allocated and returned (synchronous handle, async activation).
- `CMD_SET_POS` / `CMD_SET_LISTENER`: write source position / listener pose to
  *pending*; a later `CMD_COMMIT` promotes them.
- `CMD_COMMIT`: promote pending → active. Defines frame coherence.
- `CMD_SOUND_RETIRE`: the control thread asks to free a sound, the audio thread
  acks once it has let go (the retire-ack handshake).
- `CMD_SRC_STEAL`: fade a stolen voice out on its own slot, then free it.

`Cmd` is `type` + `handle` + a union of small payload arms (see rt.h).
Most arms are a handle plus a bool or a float. `play` carries
`{ uint64_t start, loop_beg, loop_end; uint32_t sound; uint8_t loop, oneshot; }`:
`start` is the dsp-sample to begin at (0 = now) and `loop_beg`/`loop_end` are the
optional loop region (0/0 = whole clip), so both sample-accurate scheduling and
intro→loop are wider commands, not new mechanisms. Scheduling a *stop*
(`CMD_STOP_AT`, one `uint64_t` sample) is the mirror: the audio thread fires the
existing click-free stop once a block reaches it. `CMD_QUEUE` appends a sound to a
voice's gapless play queue (a fixed FIFO in the `Voice`, depth `BWA_QUEUE`): at a
non-looping end the mixer pops the next entry and continues in the same block
instead of stopping. The entries are resolved `SoundData*`, so `CMD_SOUND_RETIRE`
tombstones any it frees (NULL) before acking, the same "detach before free"
discipline that protects the voice's current `sound`.

```c
#define RING_CAP 4096                 /* power of two; sized for a worst-case frame burst */
typedef struct { Cmd slots[RING_CAP]; _Atomic uint32_t write, read; } CmdRing;
```

Handles are `(index | generation << 16)`; the macros live in rt.h:

```c
#define BWA_H_IDX(h)   ((uint16_t)((h) & 0xFFFFu))
#define BWA_H_GEN(h)   ((uint16_t)((h) >> 16))
#define BWA_MK_H(i, g) ((uint32_t)(i) | ((uint32_t)(g) << 16))
```

## Event type and ring

The return path is symmetric and likewise SPSC, roles reversed: the **audio
thread is the sole producer**, the **control thread the sole consumer**
(`drain_events`, called from `rt_commit` / `bwa_commit`). There is no second
producer; keep it that way, or the relaxed/acquire/release scheme stops being
sufficient.

```c
enum { EVT_VOICE_ENDED = 0, EVT_SOUND_RETIRED };
typedef struct { uint8_t type; uint32_t handle; } Evt;

#define EVT_CAP 1024                  /* power of two */
typedef struct { Evt slots[EVT_CAP]; _Atomic uint32_t write, read; } EvtRing;

/* audio thread only: */
static bool evt_push(EvtRing* r, const Evt* e) {
    uint32_t w  = atomic_load_explicit(&r->write, memory_order_relaxed);
    uint32_t rd = atomic_load_explicit(&r->read,  memory_order_acquire);
    if (w - rd >= EVT_CAP) return false;
    r->slots[w & (EVT_CAP - 1)] = *e;
    atomic_store_explicit(&r->write, w + 1, memory_order_release);
    return true;
}
```

(`cmd_push` is the same function modulo `RING_CAP`, with the control thread as
producer.)

Three facts about the event path:

- **`EVT_VOICE_ENDED` does not fire for every ended voice.** A regular
  non-looping voice only flips `playing` off at end-of-buffer; its handle still
  belongs to the app until `bwa_source_destroy`. The event fires only where the
  *audio thread* ends a slot's lifetime: a oneshot reaching end-of-buffer, and
  a stolen voice finishing its fade-out. The control side recycles the slot and
  clears its `stealing` flag.
- **The ring cannot overflow.** `rt_create` rejects configurations where
  `voice_cap + sound_cap > EVT_CAP`. Between two control-thread drains the
  audio thread emits at most one `EVT_VOICE_ENDED` per voice plus one
  `EVT_SOUND_RETIRED` per sound, so acks are never silently dropped.
- **`EVT_SOUND_RETIRED` also closes streams.** For a streamed sound, the
  control-side ack handler closes the stream (`stream_close`) before freeing
  the payload and recycling the slot.

## Producer side (control thread)

Per-frame calls are pure encode-and-push; they never touch voice state. From
rt.c:

```c
void rt_source_set_pos(RtCore* c, uint32_t h, float x, float y, float z) {
    if (!(isfinite(x) && isfinite(y) && isfinite(z))) return;  /* keep NaN/Inf off the audio thread */
    Cmd cmd = { .type = CMD_SET_POS, .handle = h };
    cmd.u.pos.x = x; cmd.u.pos.y = y; cmd.u.pos.z = z;
    cmd_push(&c->cmds, &cmd);
}

void rt_commit(RtCore* c) {
    Cmd cmd = { .type = CMD_COMMIT };
    cmd_push(&c->cmds, &cmd);
    drain_events(c);                    /* consume VOICE_ENDED / SOUND_RETIRED acks */
}
```

`rt_source_create` is the one call that does **not** round-trip. The control
thread owns the free-list and generation table, so it allocates an index, bumps
the generation, returns `BWA_MK_H(idx, gen)` synchronously, and enqueues
`CMD_SRC_CREATE`. Synchronous handle, async activation. When the pool is full it
steals a voice instead of failing (see "Voice steal" below).

## Consumer side (audio thread): the snapshot

Drain runs once at block start. Structural commands apply immediately;
continuous parameters write to *pending* fields and promote to *active* only on
`CMD_COMMIT`. The mixer reads only active fields. A frame the producer hasn't
finished (set_pos sent, commit not yet) leaves pending half-updated but active
untouched. Because pending is latest-wins and persists across blocks, the
straggler promotes cleanly on the next commit. No need to hold commands back in
the ring.

```c
static Voice* voice_for(RtCore* c, uint32_t h) {
    uint16_t i = BWA_H_IDX(h);
    if (i >= c->voice_cap) return NULL;
    Voice* v = &c->voices[i];
    return (v->active && v->gen == BWA_H_GEN(h)) ? v : NULL;    /* stale gen => dropped */
}
```

The drain is one switch over every command type (the rest is in rt.c). The
cases that carry the protocol:

```c
static void drain_commands(RtCore* c) {
    CmdRing* r = &c->cmds;
    uint32_t rd = atomic_load_explicit(&r->read,  memory_order_relaxed);
    uint32_t w  = atomic_load_explicit(&r->write, memory_order_acquire);
    for (; rd != w; ++rd) {
        const Cmd* cmd = &r->slots[rd & (RING_CAP - 1)];
        switch (cmd->type) {
        case CMD_SET_POS: { Voice* v = voice_for(c, cmd->handle);
            if (v) memcpy(v->pos_pending, &cmd->u.pos, sizeof v->pos_pending); } break;
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
            /* detach every voice still bound to this sound, then ack */
            const SoundData* s = sound_for(c, cmd->handle);
            if (s)
                for (uint32_t i = 0; i < c->voice_cap; ++i)
                    if (c->voices[i].sound == s) { c->voices[i].playing = false; c->voices[i].sound = NULL; }
            Evt ev = { .type = EVT_SOUND_RETIRED, .handle = cmd->handle };
            evt_push(&c->events, &ev);
        } break;
        /* ... create/destroy/play/stop, the per-voice feature toggles,
           pause/seek, steal, test signal - all handle-gated via voice_for. */
        }
    }
    atomic_store_explicit(&r->read, rd, memory_order_release);
}
```

## Two correctness points

- **A listener move dirties every voice**, since the panner gains are all
  listener-relative. That is the moving-observer case paying its cost:
  recompute the whole field on frames the head moves, nothing on frames it
  doesn't. The tracker override (a connected tracker) applies the same rule when
  the sampled pose moves.
- **Generation counts make slot reuse safe without an ack.** A late
  `CMD_SET_POS` aimed at a destroyed-then-recycled slot fails the `gen` check
  and is dropped; the new voice in that slot has a fresh generation. Voices
  need no round-trip. Only *sound buffers* do (generations don't protect
  freed memory); hence `CMD_SOUND_RETIRE` detaches references and acks back so
  the control thread frees exactly once the audio thread has provably let go.

## Sound lifetime: the retire-ack handshake (control side)

The audio side (above) detaches references on `CMD_SOUND_RETIRE` and acks with
`EVT_SOUND_RETIRED`. The control side closes the loop: `rt_unload_sound` only
*requests* retirement and marks the slot; the actual free happens later, when
`drain_events` (run from `rt_commit`) sees the ack. Freeing in
`rt_unload_sound` directly would be a use-after-free: an audio block can still
be mid-mix on that buffer.

```c
void rt_unload_sound(RtCore* c, uint32_t sound) {
    SoundSlot* s = sound_slot_ctrl(c, sound);
    if (!s || s->retiring) return;       /* invalid or already retiring: idempotent no-op */
    s->retiring = 1;                     /* refuse new binds (rt_source_play checks this) */
    Cmd cmd = { .type = CMD_SOUND_RETIRE, .handle = sound };
    if (!cmd_push(&c->cmds, &cmd)) s->retiring = 0;   /* ring full: revert so it can be retried */
}

/* consumer of the event ring; control thread, called from rt_commit. */
static void drain_events(RtCore* c) {
    EvtRing* r = &c->events;
    uint32_t rd = atomic_load_explicit(&r->read,  memory_order_relaxed);
    uint32_t w  = atomic_load_explicit(&r->write, memory_order_acquire);
    for (; rd != w; ++rd) {
        const Evt* ev = &r->slots[rd & (EVT_CAP - 1)];
        switch (ev->type) {
        case EVT_VOICE_ENDED:    recycle_handle(c, ev->handle);
                                 c->stealing[BWA_H_IDX(ev->handle)] = 0; break;
        case EVT_SOUND_RETIRED: {        /* audio dropped all refs: free pcm / close the stream */
            SoundSlot* s = sound_slot_ctrl(c, ev->handle);
            if (s) {
                if (s->data.stream) { stream_close(c->streams, s->data.stream); s->data.stream = NULL; }
                sound_unload(&s->data);
                srecycle_sound(c, BWA_H_IDX(ev->handle));
            }
        } break;
        }
    }
    atomic_store_explicit(&r->read, rd, memory_order_release);
}
```

This is the only handshake voices don't get: generation counts make a stale
*source* handle a safe no-op, but they do not protect *freed buffer memory*.
`recycle_handle` itself is gen-checked and idempotent, so a double-destroy or a
late `EVT_VOICE_ENDED` for a since-reallocated slot can never free a live
source's slot.

## Block structure

`rt_render(RtCore*, float* bus, uint32_t nframes, const bwa_timestamp* ts)` is
the whole audio-thread entry point (rt.c). The sink's render callback calls it
with the device's planar buffer (`cave` profile) or a scratch buffer
(`binaural`/`both`; see below). The bus is planar, channel-major:
`bus[ch * nframes + i]`, `channels * nframes` floats. Stages, in order:

1. **Clock.** Take the block-start dsp-sample from the device timestamp
   (`ts->sample_pos`), falling back to an internal block counter when no
   timestamp is supplied (direct `rt_render` in tests). Publish it (`dsp_now`)
   for control-thread scheduling via `rt_dsp_time`. Denormals are flushed to
   zero (FTZ/DAZ): gain ramps toward 0 otherwise produce subnormals that
   stall the FP pipeline.
2. **Drain commands** (the snapshot above). A block larger than
   `BWA_RT_MAX_BLOCK` then renders silence instead of overflowing the RT
   scratch buffers, but commands were already drained, so the ring never
   backs up.
3. **Sample the tracker pose.** With a tracker connected, read the seqlock slot
   and overwrite the active listener; a moved position dirties every voice.
   This bypasses the commit path: lower latency than routing pose through the
   command ring.
4. **Zero the bus; latch the taps.** The reflection and path tap function
   pointers are acquire-loaded once per block (they were published with
   release *after* their user-data, so registration mid-run can't tear). Zero
   the aux-send and ambisonic scratch buffers if the taps want them.
5. **Voice loop.** Per playing voice: hold a scheduled start until the block
   containing its `start_sample`, then begin at the exact in-block offset. If
   dirty, solve gains through the *selected* panner (DBAP/SPCAP/VBAP), apply
   spread, and derive the dual-band LF gain set. Then `mix_bed` (ambisonic
   assets: SH → bus through the bed-decode matrix) or `mix_voice` (mono point
   sources). `mix_voice` is where the per-voice DSP lives: the pause/seek
   gate, streaming pulls (`stream_pull`), the occlusion EQ + level +
   directivity ramps, the reflection send, air absorption, the Doppler delay
   ring, the pathing SH-encode of the *un-occluded* signal, and the per-sample
   gain ramps.
6. **Publish playback state** per slot (`play_pub`, gen-tagged) for
   `rt_source_is_playing`.
7. **Bus taps.** Call the reflection tap (convolves the aux send, sums onto the
   bus) and the path tap (decodes the accumulated ambisonic field onto the
   bus): `RtBusTap` / `RtPathTap` in rt.h. Both run *before* align, so their
   output gets the per-speaker trims too.
8. **`align_process`** ([`src/align.c`](../src/align.c)): the per-speaker
   output stage: correction FIR, room-EQ modal cuts, gain trim, delay line.
9. **Test signal.** `bwa_set_test_signal` injects its sine/noise onto a raw channel
   *after* align: a wiring check, outside the spatial path.
10. **Limiter** (final stage; on by default at -1 dBFS). One gain computed
    from the cross-channel peak (linked, so engaging never shifts the spatial
    image), with ~1 ms attack / ~120 ms release one-poles and a hard clamp at
    the ceiling. Protection, not mastering.
11. **Meters + readback.** Publish per-channel output peaks (`chan_peak` →
    `bwa_get_bus_levels`) and the active pose (the `readback` seqlock →
    `rt_read_pose`).

The headphone decode is not a bus tap: it is a *sink render callback* in
[`src/engine.c`](../src/engine.c) (`render_binaural`: `rt_render` into
`scratch26`; and, in the `binaural` profile, `rt_direct_ambi` /
`rt_direct_voices` read the block's direct SH field and per-voice point taps on
the same thread right after; then `monitor_process` / `steam_monitor_process`
decodes to the 2-ch device). Neither the direct field nor the point taps ever
cross a thread: `rt_render` fills them and the same callback consumes them before
returning. Device output likewise goes through the `bwa_sink` abstraction
([`src/sink.h`](../src/sink.h)); the render callback fills the device's planar
buffers directly.

`mix_voice` interpolates `gcur → gtarget` across the block, never slams the new
vector in at block start (invariant 4 in `CLAUDE.md`). The `dirty` flag means a
static source costs only the multiply-accumulate. End-of-buffer on a
non-looping voice flips `playing` off; only oneshots (and steal fades) push
`EVT_VOICE_ENDED`.

## Newer machinery, same rules

None of the machinery below adds a new kind of synchronization; each piece is
the existing invariants applied again: allocation on the control thread, ramps
for every audible change, generation-gated handles, acks over the event ring.

### Voice steal (fade reserve + priority)

`rt_create` allocates `BWA_FADE_RESERVE` (8) physical slots beyond the user pool;
normal allocation never draws them down. When the user pool is full,
`rt_source_create` scans for the lowest-priority active source
(`bwa_source_set_priority`; control-side bytes, 255 = protected, slots already
mid-steal are skipped), gives the *new* source a reserve slot, and enqueues
`CMD_SRC_STEAL` for the victim. The audio thread fades the victim's gate to
zero over one block, then finalizes in `pause_gate`: `active = false` plus
`EVT_VOICE_ENDED`, and the control thread recycles the slot on the ack. If the
reserve is exhausted (a steal burst) or the ring is full, the steal degrades to
a hard cut (`rt_source_destroy` + immediate reuse). Oneshots never spend the
reserve: a full pool drops them.

### The dsp clock and scheduled starts

The audio thread publishes each block's starting dsp-sample (`dsp_now`,
device-anchored via the sink timestamp). `rt_dsp_time` reads it on the control
thread; `rt_source_play_at` carries the target sample in the `play` command,
and the mixer holds the voice silent until the block that contains it, starting
at the exact in-block offset. All per-sample ramps span only the audible part
of the block, so a scheduled start lands its gains exactly.

### Pause, seek, and click-free stop

`CMD_SET_PAUSED` sets a target; the mixer's gate (`pause_g`) ramps toward it
across one block (invariant 4) and the playhead freezes only once fully silent,
so resume continues exactly where pause landed. Seek is click-free: a pending
`CMD_SEEK` lands only while the gate is silent, so seeking a running voice is
ramp-out → jump → ramp-in (two blocks, ~10 ms at 256/48k). Streamed sounds
ignore seek: the ring can't jump. `CMD_STOP` rides the same gate: fade out,
*then* `playing = false`. `CMD_STOP_AT` is the scheduled form: `rt_render` fires
that same fade once a block reaches `stop_at` (block-granular, so it can't pop).
**Starts** are symmetric: `CMD_PLAY` zeroes `gcur` (the final per-channel gain),
so the first block ramps up from silence; a fresh voice is already zero (create
memset), so this only fades in a *replayed* slot that would otherwise reuse the
prior solve's gains and click on the asset's first sample. Only
`CMD_SRC_DESTROY` hard-cuts. A paused voice still reads as playing.

### Doppler delay rings

Per-voice fractional-delay rings live in one contiguous `RtCore.dop_ring`
allocation: one power-of-two slice per voice, sized at `rt_create` for
`BWA_DOPPLER_MAX_DIST` (8 m) at the engine rate. The audio thread writes each
sample and reads at a delay gliding toward `distance/c`; the glide *is* the
pitch shift. The write index stays integer (masked ring) with the fraction as a
separate small float, so a long-lived voice never loses sample precision.
Allocation at create time, DSP on the audio thread: invariant 1.

### Dual-band gains and the bed decode

Both are plain audio-thread state, not new channels. `compute_gains` derives
the amplitude-normalized LF gain set (`gtarget_lo`) on every solve, so
`bwa_set_dual_band` A/Bs live: an atomic flag the mixer reads, crossfaded per
voice via `dual_mix`. The ambisonic-bed decode matrix (`bed_decode`) is rebuilt
on the control thread only while the audio thread is stopped (`rt_set_layout` /
`rt_set_bed_decoder`), like the aligner and the taps.

## Invariants recap

- Nothing in `drain_commands`, the mix path, or any registered tap allocates,
  locks, or blocks.
- File decode and malloc/free live on the control thread (or the streaming and
  sim threads, which are also not the audio thread).
- The command ring is sized so a worst-case frame's burst can't fill it between
  two drains (~one audio block apart). If it fills anyway, the code does not
  spin; it makes the drop safe: `rt_source_destroy` recycles the
  handle only if the destroy actually enqueued, `rt_source_create` undoes its
  allocation, `rt_unload_sound` reverts the `retiring` flag so it can be
  retried, and a oneshot reserves its 4 commands up front (`cmd_free`) so it
  drops whole, never half.
- The event ring cannot fill at all: `EVT_CAP >= voice_cap + sound_cap` is
  enforced at `rt_create`.

That is the entire concurrency surface: two rings, a commit snapshot,
generation gates, one ack handshake, and a handful of wait-free publish slots.
Everything else (panners, EQs, the limiter) is single-threaded DSP behind it.
