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

Load sounds once, at load time. **WAV, FLAC, and MP3** are accepted (decoded to mono
float by dr_libs, dispatched by file extension). If the file's sample rate differs from
the engine's, it is **resampled to the engine rate at load** (a windowed-sinc pass) — so a
44.1 kHz MP3 plays correctly on a 48 kHz engine; only the one-time load cost is paid.

`bw_unload_sound` is safe to call any time — the
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
bool     bw_source_is_playing(BwEngine* e, BwSource s);  // control-thread poll; see below
void     bw_play_oneshot(BwEngine* e, BwSound snd, float x, float y, float z, float gain);
```

`bw_source_is_playing` is a **latest-wins readback** (like `bw_get_listener_pose`): the audio thread
republishes each source's playing state every block, gated on the handle's generation. It reads
`true` while a sound plays, `false` once a non-loop sound finishes, after `stop`, or for a
stale/destroyed handle. Poll it once per frame to drive an "on finished" signal; it is best-effort
(a sound shorter than the poll interval may never be observed as playing).

Positions are in **room space, right-handed** — the engine binding converts from its
own coordinate system at the boundary (see integration.md). `bw_play_oneshot` is the
fire-and-forget path: it allocates a transient voice internally and recycles it on
end, so the caller holds no handle.

## Ambisonic beds (control thread)

```c
BwSound bw_load_ambix(BwEngine* e, const char* path);   // AmbiX (ACN/SN3D); 4/9/16 ch -> order 1/2/3
BwBed   bw_bed_create  (BwEngine* e);
void    bw_bed_play    (BwEngine* e, BwBed b, BwSound snd, bool loop);
void    bw_bed_set_gain(BwEngine* e, BwBed b, float linear);   // master gain, ramped
void    bw_bed_stop    (BwEngine* e, BwBed b);
void    bw_bed_destroy (BwEngine* e, BwBed b);
```

A **bed** is a pre-encoded **AmbiX** (ACN ordering, SN3D normalization) soundfield decoded
**straight to the 26 speakers** — *not* DBAP-panned — for diffuse/ambient content. It is
**world-locked** (the soundfield is fixed to the room; the physical speakers are world-fixed,
so the listener moving through it is handled by the real acoustics, and the binaural monitor's
head-tracking applies downstream). Load with `bw_load_ambix` (a multichannel asset; mono and other
channel counts are rejected), then drive with the `bw_bed_*` family — no position, just a master
gain. Internally a bed is a voice playing a multichannel asset, so handles/lifetime match
`bw_source_*`. The decode is a static SN3D sampling decode `(2l+1)·Y_k(dir_s)/L`, rebuilt from the
layout. FLAC is the natural container for lossless multichannel beds; MP3 cannot carry ambisonics.

## Listener (control thread; skip if `track_internal`)

```c
void bw_set_listener_pose(BwEngine* e, float px,float py,float pz,
                                       float qx,float qy,float qz,float qw);
```

Position is used by both consumers. Orientation (the quaternion) is used by the
**binaural monitor only**; the array render ignores it. If `track_internal` is true,
do not call this — the core samples the freshest OptiTrack pose at block time.

### Internal tracking (`track_internal`, M6)

With `track_internal = true` the core ingests OptiTrack/NatNet pose itself and samples the
freshest head pose on the **audio thread at block time** — lower latency than pushing pose
through the command ring — overriding any `bw_set_listener_pose`. The NatNet specifics are
configured by environment variable (kept out of `BwConfig` so the ABI stays stable):

| Variable                      | Default         | Meaning                                       |
|-------------------------------|-----------------|-----------------------------------------------|
| `BWAUDIO_NATNET_MULTICAST`    | `239.255.42.99` | data multicast group (empty ⇒ unicast)        |
| `BWAUDIO_NATNET_DATA_PORT`    | `1511`          | NatNet data port                              |
| `BWAUDIO_NATNET_SERVER`       | (unset)         | Motive server IP, for the version handshake   |
| `BWAUDIO_NATNET_COMMAND_PORT` | `1510`          | NatNet command port (handshake)               |
| `BWAUDIO_NATNET_RIGIDBODY`    | `0`             | body to track: a streaming **ID**, or a **name** (resolved via model defs — needs `SERVER`); 0 ⇒ first in frame |
| `BWAUDIO_NATNET_VERSION`      | auto, else 3.1  | bitstream version `major.minor` override      |
| `BWAUDIO_NATNET_IFACE`        | any NIC         | local interface IP to bind/join on            |

If `BWAUDIO_NATNET_RIGIDBODY` is non-numeric it is treated as a rigid-body **name** and resolved
to its streaming ID at startup via the model definitions (a `NAT_REQUEST_MODELDEF` exchange) — this
needs `BWAUDIO_NATNET_SERVER` and NatNet ≥ 4; a name that doesn't resolve fails the open (the
engine then runs untracked, with the reason in `bw_last_error`). A NatNet open failure is
non-fatal: the engine runs on the committed/default listener until tracking data arrives.

### Reading back the pose

```c
void bw_get_listener_pose(BwEngine* e, float p[3], float q[4]);
```

Returns the pose the engine is currently rendering with — the committed pose, or, under
`track_internal`, the freshest tracked pose. Safe to poll from the control thread (published by
the audio thread through a seqlock). For visuals, logging, or bringing up the tracker (see the
`bw_track_monitor` example). Returns identity until the first audio block / tracked frame.

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

## Materials & scene geometry (control thread; load-time)

```c
BwMaterial bw_material_preset(BwEngine* e, const char* name);  // 0 = default ("generic" or miss/full)
BwMaterial bw_material_define(BwEngine* e, const float absorption[3], float scattering,
                                           const float transmission[3]);
void bw_scene_set_mesh    (BwEngine* e, const float* verts, int nverts, const int* tris, int ntris,
                           const float absorption[3], float scattering, const float transmission[3]);
void bw_scene_set_mesh_mat(BwEngine* e, const float* verts, int nverts, const int* tris, int ntris,
                           const BwMaterial* tri_material);     // one token per triangle
void bw_scene_set_box     (BwEngine* e, float w, float h, float d, const BwMaterial faces[6]); // -x,+x,-y,+y,-z,+z
```

A `BwMaterial` is an **opaque, engine-scoped token** (a small index; `0` is always the built-in
`generic` default). Mint with a preset name (11 presets, case-insensitive, Steam Audio's published
coefficients — `"generic"` returns `0` without minting) or custom 3-band coefficients (clamped to
`[0,1]`, NaN-sanitized). The table is fixed (64 entries); on overflow / unknown name the mint returns
`0` and sets `bw_last_error`. Tokens are **not** generation-checked handles — they stay valid for the
engine's life; per-triangle indices out of range clamp to the default.

Geometry is in **room space (RH metres)**, triangles CCW; `bw_scene_set_box` builds an origin-centred
shoebox with **inward-facing** normals (the listener is inside). The same per-triangle materials feed
**both** occlusion (per-band transmission) and the reflection bed (absorption/scattering) — one shared
`IPLScene`. The geometry **can change at runtime** for occlusion (the occlusion sim owns the scene and
serializes its own commit + ray trace) — but it is **locked once the reflection bed is running** (that
sim shares the scene and an `iplSceneCommit` can't race its ray tracing; the reflection IR assumes a
static scene). A locked call is rejected (sets `bw_last_error`). All of the above are **no-ops without
the `BW_HAVE_STEAMAUDIO` build** (token minting still works — it is plain table state).

## Occlusion & directivity (control thread; per-frame except where noted)

```c
void  bw_source_set_occlusion (BwEngine* e, BwSource s, bool on);
float bw_source_get_occlusion (BwEngine* e, BwSource s);   // 1 = clear .. 0 = blocked (HUD)
void  bw_source_set_orientation(BwEngine* e, BwSource s, float qx, float qy, float qz, float qw);
void  bw_source_set_directivity(BwEngine* e, BwSource s, float weight, float power); // 0=omni/.5=card/1=fig8
void  bw_source_set_directivity_preset(BwEngine* e, BwSource s, BwDirectivity pattern);
float bw_source_get_directivity(BwEngine* e, BwSource s);   // 1 = on-axis/omni .. 0 = null (HUD)
```

The setters are **non-blocking, enqueue-only** (safe in the hot loop). The off-thread sim ray-traces
at a low rate and publishes a per-source scalar (+ a 3-band transmission tilt for occlusion) that the
**audio thread ramps** — never a jump. Occlusion and directivity are independent (a source can be
directional without being occluded). The `_get_` reads return the latest published scalar for
HUD/diagnostics and are safe to poll. No-ops without the Steam Audio build.

## Channel test / diagnostics (control thread)

```c
typedef enum { BW_TEST_OFF = 0, BW_TEST_SINE = 1, BW_TEST_NOISE = 2 } BwTestKind;
void bw_test_signal(BwEngine* e, uint32_t channel, BwTestKind kind, float gain);
```

Drive a single **output channel** with a built-in signal (660 Hz sine or white noise), injected
**after** the per-speaker align stage — a raw value straight on the channel. This is a **speaker-check
/ wiring-verification / calibration** tool (walk a tone across all 26 to confirm the channel→speaker
map, find a dead speaker, set a trim), **not** a spatial path: it bypasses the panner, so don't use
it to "place" a sound. Per-frame-safe, takes effect next block, no `bw_commit` needed; any number of
channels at once; `gain 0` / `BW_TEST_OFF` silences one. Works in every profile (cave/both: a raw tone
on that DVS channel; binaural: that bus channel HRTF'd as its virtual speaker). Needs no SDK.

## Reflection bed (control thread)

```c
typedef struct { float ir_seconds; uint32_t order, num_rays, num_bounces; int enabled; float wet_gain; uint32_t reserved[3]; } BwReflectionConfig;
void bw_reflections_config   (BwEngine* e, const BwReflectionConfig* cfg);  // LOAD-TIME (before bw_start)
void bw_reflections_set_gain (BwEngine* e, float linear);                   // per-frame; live wet level
void bw_source_set_reflections(BwEngine* e, BwSource s, bool on);           // per-frame; gates the wet send
```

A single shared **listener-centric reverb bed** decoded straight to the 26 channels and summed onto
the bus. `bw_reflections_config` is **load-time** (the IR length + ambisonic order are baked at
`bw_start`); zero fields take defaults (`ir_seconds` 1.0, `order` 1, `num_rays` 4096, `num_bounces`
16, `wet_gain` 1.0), `enabled = 0` means no bed is created and the engine behaves exactly as without
it. `bw_reflections_set_gain` adjusts the wet level live (a single atomic the audio-thread tap reads).
`bw_source_set_reflections` is the per-frame, non-blocking opt-in of a source into the bed's wet send
(with the bed disabled or no SDK, it gates a send that goes nowhere). The bed runs Steam Audio's
**HYBRID** reverb — **directional** early-reflection convolution (full ambisonic, order = `order`) +
parametric (FDN) tail, decoded across the 26 speakers (requires the vendored phonon's alignment patch;
see [materials.md](.\materials.md)). No-op without the Steam Audio build.

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
