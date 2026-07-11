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

`bw_create` returns `NULL` on failure (bad config / out of memory); it does **not** return a code.
`bw_start` currently returns only **`0`, `1` (`BW_ERR_CONFIG`: NULL engine, or a `room_eq` layout in a
moving-listener session), and `2` (`BW_ERR_DEVICE`)**. Codes **3–6 are reserved**, not yet returned:
a bad `layout_path` or `hrtf_path` **degrades gracefully** (the engine falls back to the default grid
/ the simple-pan monitor and records the reason in `bw_last_error`) rather than failing `bw_start`,
and a redundant `bw_start` is a no-op that returns `0`. So **read `bw_last_error` after `bw_create`
and `bw_start` even on success** if you must confirm the surveyed layout / SOFA HRTF actually loaded.

The per-frame `void` calls (`bw_source_set_pos`, `bw_commit`, …) never return an error: they only
enqueue onto the command ring. If that ring is ever full (a should-never-happen control-thread
stall — see [`concurrency.md`](./concurrency.md)) the command is **dropped** (silently — no
`bw_last_error`); the ring is sized (`RING_CAP`) for a worst-case frame burst so this does not occur
in normal use. Position/pose are latest-wins, so a dropped update is corrected by the next frame.

## Assets (control thread, file I/O)

```c
BwSound bw_load_sound(BwEngine* e, const char* path);           // decode fully into RAM; 0 = failure
BwSound bw_load_sound_streaming(BwEngine* e, const char* path); // stream from disk (long files); 0 = failure
void    bw_unload_sound(BwEngine* e, BwSound snd);               // safe; retire-acked internally
```

Load sounds once, at load time. **WAV, FLAC, and MP3** are accepted (decoded to mono
float by dr_libs, dispatched by file extension). If the file's sample rate differs from
the engine's, it is **resampled to the engine rate at load** (a windowed-sinc pass) — so a
44.1 kHz MP3 plays correctly on a 48 kHz engine; only the one-time load cost is paid.

`bw_load_sound_streaming` is for long assets (music, ambience) you don't want resident in RAM: a
background thread feeds the voice from disk as it plays. It is **mono, at the engine sample rate**
(a rate mismatch fails — pre-convert, or use `bw_load_sound` which resamples), plays on **one voice
at a time**, and does not support `bw_source_seek` (the ring can't jump).

`bw_unload_sound` is safe to call any time — the
core detaches references on the audio thread and frees only after the retire-ack
(see concurrency.md), so it will not pull a buffer out from under a playing voice.

## Sources (control thread, non-blocking)

```c
BwSource bw_source_create(BwEngine* e);                  // handle returned synchronously
void     bw_source_destroy(BwEngine* e, BwSource s);
void     bw_source_set_priority(BwEngine* e, BwSource s, int priority);  // 0 = expendable .. 255 = protected (default 128)
void     bw_source_set_pos (BwEngine* e, BwSource s, float x, float y, float z); // ROOM space, RH
void     bw_source_set_gain(BwEngine* e, BwSource s, float linear);
void     bw_source_play (BwEngine* e, BwSource s, BwSound snd, bool loop);
void     bw_source_play_at(BwEngine* e, BwSource s, BwSound snd, bool loop, uint64_t start_sample); // sample-accurate
uint64_t bw_dsp_time(BwEngine* e);                       // current dsp-sample clock (device-anchored, monotonic)
void     bw_source_stop (BwEngine* e, BwSource s);
void     bw_source_set_paused(BwEngine* e, BwSource s, bool paused);   // ramped; playhead freezes
void     bw_source_seek (BwEngine* e, BwSource s, uint64_t frame);     // click-free jump (in-memory)
bool     bw_source_is_playing(BwEngine* e, BwSource s);  // control-thread poll; see below
void     bw_play_oneshot(BwEngine* e, BwSound snd, float x, float y, float z, float gain);
```

**Voice pool + scheduling.** The voice pool is fixed; when it is full, `bw_source_create` steals the
lowest-**priority** active source (255 = protected, never stolen) rather than failing — so set music
and critical SFX high. The steal is **click-free**: the stolen voice fades out over one block on its
own slot (the new source starts immediately on a small reserve of extra slots), so a scene churning
voices under load doesn't tick. `bw_source_play_at` begins output exactly when the engine's dsp clock reaches
`start_sample`; read "now" from `bw_dsp_time` (device sample position, monotonic) and add a delay,
e.g. play 0.5 s out with `bw_dsp_time(e) + sample_rate/2`. `0` = play immediately (== `bw_source_play`).

`bw_source_is_playing` is a **latest-wins readback** (like `bw_get_listener_pose`): the audio thread
republishes each source's playing state every block, gated on the handle's generation. It reads
`true` while a sound plays, `false` once a non-loop sound finishes, after `stop`, or for a
stale/destroyed handle. Poll it once per frame to drive an "on finished" signal; it is best-effort
(a sound shorter than the poll interval may never be observed as playing).

`bw_source_set_paused` gates the voice with a one-block ramp (~5 ms — no click) and freezes the
playhead once silent, so resume continues exactly where pause landed; it works for in-memory,
streamed, and bed sounds, and a paused voice still reads as *playing* (it has not ended).
`bw_source_seek` jumps the content position (engine-rate frames): on a running voice it ramps out,
jumps, and ramps back in (~10 ms end to end); on a paused voice the jump is immediate and it stays
paused. Past-the-end seeks wrap for loops and end one-shots. Streamed sounds ignore seeks (the
stream ring cannot jump); `bw_source_play` always restarts un-paused at frame 0.

Positions are in **room space: right-handed, +Y up, +Z forward, origin on the floor**
(Motive's ground-plane default: y = height above the floor; an identity orientation faces
+Z) — the engine binding converts from its own coordinate system at the boundary (see
integration.md). The engine's world-locked decodes and its default listener position use
the **array centroid** (the nominal listening point), not the origin. `bw_play_oneshot` is the
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

### Ray-tracing acceleration (`BWAUDIO_EMBREE`)

The occlusion and reflection sims ray-trace on the Steam Audio scene. Set **`BWAUDIO_EMBREE=1`**
(any non-empty, non-`0` value) to run them on **Intel Embree** instead of Steam Audio's built-in
ray tracer — faster, and the lever to pull if you raise scene complexity, ray counts, or bake at
high probe density. It is **opt-in and safe**: if the linked `phonon` was not built with Embree
(or the Embree/TBB runtime is missing), the engine logs that Embree is unavailable and falls back
to the default tracer — no failure. The scene is created once and both sims share it, so the flag
applies to occlusion and reflections together. **Note:** the vendored prebuilt `phonon.dll` is not
Embree-enabled (the flag currently falls back); to activate it, drop in a `phonon` built with
Embree (the SDK's `STEAMAUDIO_ENABLE_EMBREE` path) and ship `embree4.dll` + `tbb*.dll` alongside.

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

Geometry is in **room space (RH metres)**, triangles CCW; `bw_scene_set_box` builds a floor-based
shoebox (x/z centred on the origin, y from 0 — the floor — up to `h`) with **inward-facing** normals
(the listener stands inside). The same per-triangle materials feed
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

## Propagation effects (control thread; per-frame)

```c
void bw_source_set_doppler        (BwEngine* e, BwSource s, bool on);
void bw_source_set_air_absorption (BwEngine* e, BwSource s, bool on);
```

Opt-in, per source, default **off**, and — unlike occlusion/directivity — **pure per-voice DSP that
needs no Steam Audio build**. Both derive from the live source↔listener distance, recomputed each
block from the committed positions.

- **Doppler** renders the source through its acoustic propagation delay (`distance / c`). A per-voice
  fractional delay line glides toward that delay each block; the *glide rate is the pitch shift*
  (approaching → up, receding → down), so no velocity input is needed — it falls out of motion. The
  delay (hence the effect) **saturates past ~8 m**, which bounds the ring; enabling adds the real
  propagation latency. Best for fast movers — subtle for slow ones in a small room.
- **Air absorption** is a distance-driven one-pole **high-frequency low-pass** (far sources sound
  duller): cutoff falls ~650 Hz/m from 18 kHz near, down to a ~1.2 kHz floor. Subtle at a few metres,
  pronounced for sources placed at large *virtual* distances.

Both are **non-blocking, enqueue-only**, ramp on the audio thread (the air coefficient and the Doppler
delay both glide across the block — no zipper), and are independent of each other and of the panner /
profile. They apply to the **direct path only** — the reflection wet send is tapped *before* them, so
reflections keep their own propagation. They do not affect ambisonic beds (world-locked, no position).

## Source spread / size (control thread; per-frame)

```c
void bw_source_set_spread(BwEngine* e, BwSource s, float amount);   // 0 = point (default) .. 1 = wide
```

Angular **width** of a source. A waterfall, a crowd, an engine room, or ambience shouldn't collapse to
a single point; raise `spread` and the source's energy fans out across the speakers around its
direction. Implemented in the per-block gain solve (not the sample loop): the panner's point gains are
blended toward a width-controlled lobe centred on the source direction, then **renormalised to the
panner's own power** — so widening never changes loudness, and the perceived direction stays put. It's
**panner-agnostic** (works over DBAP/SPCAP/VBAP) and the change ramps click-free like any gain change.

## Channel test / diagnostics (control thread)

```c
typedef enum { BW_TEST_OFF = 0, BW_TEST_SINE = 1, BW_TEST_NOISE = 2 } BwTestKind;
void     bw_test_signal(BwEngine* e, uint32_t channel, BwTestKind kind, float gain);
uint32_t bw_get_bus_levels(BwEngine* e, float* peaks, uint32_t cap);  // last block's per-channel output peak
```

Drive a single **output channel** with a built-in signal (660 Hz sine or white noise), injected
**after** the per-speaker align stage — a raw value straight on the channel. This is a **speaker-check
/ wiring-verification / calibration** tool (walk a tone across all 26 to confirm the channel→speaker
map, find a dead speaker, set a trim), **not** a spatial path: it bypasses the panner, so don't use
it to "place" a sound. Per-frame-safe, takes effect next block, no `bw_commit` needed; any number of
channels at once; `gain 0` / `BW_TEST_OFF` silences one. Works in every profile (cave/both: a raw tone
on that DVS channel; binaural: that bus channel HRTF'd as its virtual speaker). Needs no SDK.

`bw_get_bus_levels` is the matching **readback**: each output channel's last-block peak `|sample|`
(linear), measured at the very end of the render — after align, the test signal, and the limiter, i.e.
exactly what the device channel received. Fills up to `cap` floats, returns the count filled.
Per-frame-safe (relaxed atomic reads, no locks); levels read 0 until audio is running. Drive channel
meters or a speaker-activity display with it — the playground lights each speaker gizmo from this.

## Panner & layout query (control thread)

```c
typedef enum { BW_PAN_DBAP = 0, BW_PAN_SPCAP = 1, BW_PAN_VBAP = 2 } BwPanner;
typedef enum { BW_DECODE_SAMPLING = 0, BW_DECODE_ALLRAD = 1 } BwBedDecoder;
void     bw_set_panner(BwEngine* e, BwPanner panner);            // load-time (between create and start)
void     bw_set_dual_band(BwEngine* e, bool on);                // live A/B; wraps the selected panner
void     bw_set_bed_decoder(BwEngine* e, BwBedDecoder decoder);  // load-time
uint32_t bw_get_speakers(BwEngine* e, float* xyz, uint32_t cap); // read back the layout; NULL xyz = count only
```

`bw_set_panner` chooses the per-source panner behind the 26-ch bus. **DBAP** (default) is
listener-relative, recomputed per frame from the tracked pose — for a **moving** observer roaming the
array. **SPCAP** is a smooth, all-speaker, placement-correcting sweet-spot panner for a **fixed**
observer (a static listener: don't track, set the sweet spot once); it conserves loudspeaker power
across an uneven array. **VBAP** is the sharpest (2-3 nearest speakers), also fixed-observer, best on a
cleanly-triangulable array (falls back to DBAP otherwise). The switch is atomic (safe live, e.g. the
layout tool's `B` A/B). `bw_set_dual_band` (off by default, live-toggleable) **wraps** the selected
panner: it splits each source at ~700 Hz and pans the low band with **amplitude** (pressure /
velocity-vector) normalisation, the high band with the panner's usual **power** (energy-vector)
normalisation — sharper low-frequency localisation for a near-centred listener (SPAT's "VBP
Dual-Band"). The panning *direction* is unchanged; only the low band's level/coherence differs. It is
sweet-spot dependent like VBAP, so it's a by-ear/measurement call for a roaming listener. `bw_set_bed_decoder` chooses the **diffuse-bed** SH→26 decoder:
**sampling** (default projection decode) or **AllRAD** (decode to a uniform virtual layout + VBAP onto
the real array — robust on an irregular array, heavier load-time build). It affects the ambisonic +
reflection beds only, not the point-source panner. Both are load-time; see
[`spatialization.md`](./spatialization.md). `bw_get_speakers` returns the effective layout (the default
grid or the `layout_path` file) as `cap*3` floats in channel order + the count (26) — for visualizing
or auditioning the geometry the engine actually pans with.

## Output protection limiter (control thread; ON by default)

```c
void bw_set_limiter(BwEngine* e, bool on);                     // live
void bw_set_limiter_ceiling(BwEngine* e, float ceiling_db);    // default -1 dBFS; clamped [-60, 0]
```

The final stage on the 26-ch output — everything (voices, beds, the reflection/pathing taps, the
per-speaker align stage, the test signal) passes through it before the device. It is **linked**
across channels: one gain, derived from the cross-channel peak, so engaging never shifts the
spatial image; ~1 ms attack / ~120 ms release one-poles, then a hard clamp at the ceiling (the
attack is not lookahead, so the first millisecond of a hot transient clips instead of overshooting).
This is driver/speaker **protection** against digital overs and pathological content, not a
mastering limiter — if it engages in normal use, turn the content down. In the `binaural` profile
the same limited bus feeds the monitor, so headphones inherit the ceiling too.

## Reflection bed (control thread)

```c
typedef struct { float ir_seconds; uint32_t order, num_rays, num_bounces; int enabled; float wet_gain; uint32_t reserved[3]; } BwReflectionConfig;
void bw_reflections_config   (BwEngine* e, const BwReflectionConfig* cfg);  // LOAD-TIME (before bw_start)
void bw_reflections_set_gain (BwEngine* e, float linear);                   // per-frame; live wet level
void bw_source_set_reflections(BwEngine* e, BwSource s, bool on);           // per-frame; gates the wet send
void bw_source_set_reflection_send(BwEngine* e, BwSource s, float gain);    // per-source wet-send level (default 1)
void bw_source_set_reflection_distance(BwEngine* e, BwSource s, bool on);   // far = wetter
```

A single shared **listener-centric reverb bed** decoded straight to the 26 channels and summed onto
the bus. `bw_reflections_config` is **load-time** (the IR length + ambisonic order are baked at
`bw_start`); zero fields take defaults (`ir_seconds` 1.0, `order` 1, `num_rays` 4096, `num_bounces`
16, `wet_gain` 1.0), `enabled = 0` means no bed is created and the engine behaves exactly as without
it. `bw_reflections_set_gain` adjusts the wet level live (a single atomic the audio-thread tap reads).
`bw_source_set_reflections` is the per-frame, non-blocking opt-in of a source into the bed's wet send
(with the bed disabled or no SDK, it gates a send that goes nowhere). Per source, the send level is
`bw_source_set_reflection_send` (default 1.0 — drive it for a manual dry/wet), and
`bw_source_set_reflection_distance` enables an automatic **distance→wet** scaling (near = drier, far =
wetter, on top of the level). The effective send is computed + **ramped on the audio thread** (in
`rt.c`, from the source↔listener distance), so motion and on/off don't zipper the send. The bed runs Steam Audio's
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
