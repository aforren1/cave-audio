# C ABI reference

Authoritative header: [`include/bwaudio.h`](../include/bwaudio.h). This document
gives semantics and the threading contract per call. From a consumer's side it is a
**control-only** API: no audio buffers, no device, no queue, no threads — an opaque
handle, sounds, positioned sources, and per-frame updates.

## Contract

- **One control thread.** All `bw_*` calls come from a single thread. For Unity/
  Unreal that is naturally the main thread (`LateUpdate`/`Tick`), so single-producer
  holds for free. Calling from job threads requires funneling through one thread.
- **Non-blocking per-frame calls.** Every source/listener update just enqueues a
  command; it returns immediately and lands on the next audio block.
- **Latest-wins.** Position and pose are overwritten each frame; nothing accumulates
  or backs up. Push them every frame.
- **Allocation only at load time.** `create`/`start`/`load_sound`/`source_create`
  may allocate or do I/O. The per-frame loop is pure enqueue.

## Lifecycle

```c
BwEngine* bw_create(const BwConfig* cfg);
int       bw_start(BwEngine* e);     // opens device(s), starts audio thread; 0 = ok
int       bw_stop(BwEngine* e);
void      bw_destroy(BwEngine* e);
const char* bw_last_error(BwEngine* e);
```

`BwConfig`:

| field            | meaning                                                              |
|------------------|---------------------------------------------------------------------|
| `profile`        | `cave` / `binaural` / `both` (see architecture.md)                   |
| `layout_path`    | surveyed speaker geometry (JSON); cave/both                         |
| `hrtf_path`      | HRTF (SOFA) or NULL for built-in; binaural/both                     |
| `sample_rate`    | 48000                                                               |
| `block_size`     | ASIO buffer hint (256/512)                                          |
| `track_internal` | true: core reads OptiTrack/NatNet itself; false: engine pushes pose |

## Errors & return codes

The API reports failure three ways, all read on the **control thread**:

- **Pointer/handle returns** — `bw_create` returns `NULL`, `bw_load_sound`/`bw_source_create`
  return `0`, on failure.
- **`int` returns** — `bw_start`/`bw_stop` return `0` (`BW_OK`) on success, a nonzero `BwError`
  otherwise. (These codes live in api.md today; mirror them as an enum in
  [`include/bwaudio.h`](../include/bwaudio.h) when the lifecycle is implemented.)
- **`bw_last_error`** — a human-readable string for the most recent failure on that engine, or
  `NULL` if none. Engine-scoped; valid until the next `bw_*` call on the same engine. Call it right
  after a failing call.

| code            | value | cause |
|-----------------|-------|-------|
| `BW_OK`         | 0     | success |
| `BW_ERR_CONFIG` | 1     | invalid `BwConfig` (bad profile, sample_rate, block_size) |
| `BW_ERR_DEVICE` | 2     | ASIO/output device could not be opened, lacked ≥26 channels, or failed to start |
| `BW_ERR_LAYOUT` | 3     | `layout_path` missing/unparseable/failed validation (see [`layout-schema.md`](./layout-schema.md)) |
| `BW_ERR_HRTF`   | 4     | `hrtf_path` (SOFA) could not be loaded |
| `BW_ERR_STATE`  | 5     | called in the wrong state (e.g. `bw_start` while already running) |
| `BW_ERR_INTERNAL` | 6   | unexpected internal failure; `bw_last_error` carries detail |

`bw_create` validates the config and allocates; **device and asset I/O that can fail at runtime
happen in `bw_start`**, so a non-NULL engine from `bw_create` can still fail to `bw_start`. Always
check `bw_start`'s return and read `bw_last_error` on nonzero.

The per-frame `void` calls (`bw_source_set_pos`, `bw_commit`, …) never return an error: they only
enqueue onto the command ring. If that ring is ever full (a should-never-happen control-thread
stall — see [`concurrency.md`](./concurrency.md)), the policy is to briefly spin rather than drop a
structural command; a persistent stall is recorded in `bw_last_error`.

## Assets (control thread, file I/O)

```c
BwSound bw_load_sound(BwEngine* e, const char* path);   // 0 = failure
void    bw_unload_sound(BwEngine* e, BwSound snd);       // safe; retire-acked internally
```

Load sounds once, at load time. `bw_unload_sound` is safe to call any time — the
core detaches references on the audio thread and frees only after the retire-ack
(see concurrency.md), so it will not pull a buffer out from under a playing voice.

## Sources (control thread, non-blocking)

```c
BwSource bw_source_create(BwEngine* e);                  // handle returned synchronously
void     bw_source_destroy(BwEngine* e, BwSource s);
void     bw_source_set_pos (BwEngine* e, BwSource s, float x, float y, float z); // ROOM space, RH
void     bw_source_set_gain(BwEngine* e, BwSource s, float linear);
void     bw_source_play (BwEngine* e, BwSource s, BwSound snd, bool loop);
void     bw_source_stop (BwEngine* e, BwSource s);
void     bw_play_oneshot(BwEngine* e, BwSound snd, float x, float y, float z, float gain);
```

Positions are in **room space, right-handed** — the engine binding converts from its
own coordinate system at the boundary (see integration.md). `bw_play_oneshot` is the
fire-and-forget path: it allocates a transient voice internally and recycles it on
end, so the caller holds no handle.

## Listener (control thread; skip if `track_internal`)

```c
void bw_set_listener_pose(BwEngine* e, float px,float py,float pz,
                                       float qx,float qy,float qz,float qw);
```

Position is used by both consumers. Orientation (the quaternion) is used by the
**binaural monitor only**; the array render ignores it. If `track_internal` is true,
do not call this — the core samples the freshest OptiTrack pose at block time.

## Frame boundary

```c
void bw_commit(BwEngine* e);
```

Call once per frame after pushing all source and listener updates. It promotes this
frame's position/pose to a coherent snapshot (so the audio thread never mixes a
moved listener against a not-yet-moved source) and drains the event ring. For
independent point sources the tearing avoided by commit is often inaudible, but for
the moving-observer case — where listener and sources update together — it matters.

## Canonical call sequence

```c
BwConfig cfg = { .profile = BW_PROFILE_BINAURAL, .sample_rate = 48000,
                 .block_size = 256, .track_internal = false };
BwEngine* eng = bw_create(&cfg);
bw_start(eng);

BwSound steps = bw_load_sound(eng, "footsteps.wav");     // once, at load
BwSource s    = bw_source_create(eng);
bw_source_play(eng, s, steps, /*loop*/ true);

// per frame, from the control thread:
bw_set_listener_pose(eng, hx,hy,hz, qx,qy,qz,qw);
bw_source_set_pos(eng, s, sx,sy,sz);
bw_commit(eng);

// teardown:
bw_source_destroy(eng, s);
bw_unload_sound(eng, steps);
bw_stop(eng); bw_destroy(eng);
```

## Handle scheme

`BwSound` and `BwSource` are opaque `uint32_t` = `(index | generation<<16)`. A stale
handle (slot destroyed, then reused) fails the generation check on the audio side and
is silently dropped rather than crashing. `0` is always invalid. Callers should treat
handles as tokens — never do arithmetic on them.

## Planned extension point

If engine-generated or procedural audio is later needed (not just wav files), add a
`bw_source_create_stream` returning a handle the caller pushes PCM into via a
per-source ring — same control model, second feeding path. The source abstraction
should therefore not assume "backed by a file," so this slots in without churn.
