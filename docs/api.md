# bwaudio — usage guide & C ABI reference

From a consumer's side this is a **control-only** API: no audio buffers, no
device, no queue, no threads — an opaque engine handle, sounds, positioned
sources, and per-frame updates. Declarations in
[`include/bwaudio.h`](../include/bwaudio.h) carry their contracts as comments;
[`examples/minimal.c`](../examples/minimal.c) runs the whole client lifecycle.

## Feature overview

- Listener-relative spatialization over the speaker array (26 speakers on the
  CAVE; any 4..26 layout works), recomputed per audio block from the tracked head
  position: DBAP for a moving listener (the default), SPCAP/VBAP for a fixed one,
  an optional dual-band mode, per-source angular spread.
- Per-speaker gain/delay/correction-EQ output stage driven by a measured layout
  file, with a linked protection limiter as the final stage.
- Acoustics, **any build**: image-source early reflections (each wall bounce panned
  as a point source, so it has parallax as the listener walks), a directional FDN
  reverb tail, manual occlusion with per-band transmission EQ.
- Acoustics, **Steam Audio builds**: ray-traced occlusion, source directivity, a
  reflection bed (real-time or baked), sound pathing around occluders, and the HRTF
  binaural monitor (without the SDK the binaural profile falls back to a simple pan).
  Which reverb/reflection path to run is a real choice — see
  [materials.md](./materials.md) → "Choosing an acoustics path".
- Propagation (any build): Doppler, air absorption, equal-loudness compensation,
  pitch — opt-in per source.
- Assets: WAV/FLAC/MP3 decoded (and resampled) at load, disk streaming for long
  files, AmbiX ambisonic beds decoded world-locked to the array.
- Voices: fixed pool with priority stealing, pause and click-free seek,
  sample-accurate scheduled starts against a device-anchored DSP clock.
- Tracking: OptiTrack/NatNet ingested in-process; the audio thread samples the
  freshest head pose at block time.
- Diagnostics: per-channel test tone, output-level and listener-pose readbacks,
  offline panner evaluation for layout tools.

## Quickstart

```c
BwConfig cfg = { .profile = BW_PROFILE_BINAURAL, .sample_rate = 48000,
                 .block_size = 256, .track_internal = false };
BwEngine* eng = bw_create(&cfg);
if (!eng || bw_start(eng) != 0) { /* see bw_last_error(eng) */ }

BwSound steps = bw_load_sound(eng, "footsteps.wav");     // once, at load time
BwSource s    = bw_source_create(eng);
bw_source_play(eng, s, steps, /*loop*/ true);

// per frame, from the control thread:
bw_set_listener_pose(eng, hx,hy,hz, qx,qy,qz,qw);        // skip if track_internal
bw_source_set_pos(eng, s, sx,sy,sz);
bw_commit(eng);                                          // ONE commit, last

// teardown:
bw_source_destroy(eng, s);
bw_unload_sound(eng, steps);   // safe while playing: retire is acked internally
bw_stop(eng); bw_destroy(eng);
```

- **Production** uses `BW_PROFILE_CAVE`, with `cfg.layout_path` pointing at the
  surveyed `cave_layout.json`.
- **`bw_start` never demands hardware**: with no usable ASIO device the engine
  keeps rendering into a silent offline sink. `bw_audio_backend` reports which
  backend you actually got (see [Errors](#errors--return-codes)).
- **Completion is polled, not called back**: `bw_source_is_playing` publishes
  once per audio block, so give a play command a moment to land before trusting
  a `false` answer.

## Profiles and the master bus

Every voice is panned into an in-memory **master bus**, one channel per speaker
(26 on the CAVE array; see [Channel count](#channel-count)). The profile selects
who consumes it:

| profile               | consumers |
|-----------------------|-----------|
| `BW_PROFILE_CAVE`     | bus → ASIO → Dante (production). Listener **position** only — real speakers, real ears. |
| `BW_PROFILE_BINAURAL` | bus → HRTF monitor → any 2-ch ASIO device (desk debugging). Each bus channel is a virtual speaker at its surveyed room position; full **pose** — head orientation turns the virtual array around you. |
| `BW_PROFILE_BOTH`     | both at once: array to Dante + a monitor tap. |

The binaural monitor hears the **same mix** production plays, panner and all —
headphone debugging exercises the array render, not a parallel stereo path. See
[architecture.md](./architecture.md).

## The threading contract

- **One control thread.** All `bw_*` calls come from a single thread. For Unity/
  Unreal that is naturally the main thread (`LateUpdate`/`Tick`), so single-producer
  holds for free. Calling from job threads requires funneling through one thread.
- **Non-blocking per-frame calls.** Every source/listener update just enqueues a
  command; it returns immediately and lands on the next audio block.
- **Latest-wins.** Position and pose are overwritten each frame; nothing accumulates
  or backs up. Push them every frame.
- **Allocation only at load time.** `create`/`start`/`load_sound`/`source_create`
  may allocate or do I/O. The per-frame loop is pure enqueue.
- **`bw_commit` defines frame coherence.** Position/pose updates land in *pending*
  fields; commit promotes them all to *active* as one snapshot, so the mixer never
  renders a moved listener against a not-yet-moved source. Once per frame, last
  (see [Frame boundary](#frame-boundary)).

The full model (rings, snapshot, lifetimes) is [concurrency.md](./concurrency.md).

## Coordinates and units

Room space is **right-handed, metres, +y up, +z forward**, and the origin sits
**on the floor** at the working-area centre (Motive's ground-plane calibration) —
OptiTrack rigid-body poses pass through unchanged and y is height above the
floor. An identity orientation faces +z with the right ear at −x; derive basis
vectors from `BW_ROOM_AHEAD` / `BW_ROOM_UP` / `BW_ROOM_RIGHT` in the header
rather than re-hardcoding the convention. The engine's world-locked decodes and
its default listener position use the **array centroid** (the nominal listening
point), not the origin. Gains are linear (1 = unity); sound offsets are
engine-rate sample frames. The engine bindings convert from Unity/Unreal
coordinates at the boundary ([integration.md](./integration.md)).

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

- **Pointer/handle returns.** `bw_create` returns `NULL` on failure; `bw_load_sound` /
  `bw_source_create` return `0`.
- **`int` returns.** `bw_start`/`bw_stop` return `0` (`BW_OK`) on success, a nonzero code
  otherwise. The codes are listed below; there is no matching enum in
  [`include/bwaudio.h`](../include/bwaudio.h) yet.
- **`bw_last_error`.** A human-readable string for the most recent failure on that engine, or
  `NULL` if none. It stays valid until the next `bw_*` call on the same engine, so read it
  right after the failing call.

| code            | value | cause |
|-----------------|-------|-------|
| `BW_OK`         | 0     | success |
| `BW_ERR_CONFIG` | 1     | invalid `BwConfig` (bad profile, sample_rate, block_size) |
| `BW_ERR_DEVICE` | 2     | ASIO/output device could not be opened, lacked enough output channels for the layout, or failed to start |
| `BW_ERR_LAYOUT` | 3     | `layout_path` missing/unparseable/failed validation (see [`layout-schema.md`](./layout-schema.md)) |
| `BW_ERR_HRTF`   | 4     | `hrtf_path` (SOFA) could not be loaded |
| `BW_ERR_STATE`  | 5     | called in the wrong state (e.g. `bw_start` while already running) |
| `BW_ERR_INTERNAL` | 6   | unexpected internal failure; `bw_last_error` carries detail |

What actually comes back today:

- `bw_create` returns `NULL` (bad config / out of memory). No code.
- `bw_start` returns `0`, `1` (`BW_ERR_CONFIG`: NULL engine, or a `room_eq` layout in a
  moving-listener session), or `2` (`BW_ERR_DEVICE`). Codes **3–6 are reserved**, not yet
  returned. A redundant `bw_start` is a no-op that returns `0`.
- A bad `layout_path` or `hrtf_path` does **not** fail `bw_start`. The engine degrades —
  default speaker grid, simple-pan monitor — and records why in `bw_last_error`.

So if your session depends on the surveyed layout or a SOFA HRTF, read `bw_last_error` after a
*successful* `bw_create`/`bw_start` to confirm they actually loaded.

That fallback also **changes the channel count**: the default grid is 26 speakers, so a failed
load on a 12- or 24-speaker install silently renders 26 channels. If your layout isn't 26, check
`bw_last_error` (or `bw_channel_count`) right after `bw_create` — see
[Channel count](#channel-count).

Per-frame `void` calls (`bw_source_set_pos`, `bw_commit`, …) never report errors — they only
enqueue onto the command ring. A full ring drops the command silently, but the ring is sized
(`RING_CAP`) for a worst-case frame burst, and only a stalled control thread could fill it
(see [`concurrency.md`](./concurrency.md)). Position and pose are latest-wins, so a dropped
update is corrected one frame later.

## Environment variables

Deployment knobs, kept out of `BwConfig` so the ABI stays stable:

| variable | meaning |
|----------|---------|
| `BWAUDIO_SINK` | `null` forces the silent offline sink (CI, desk debugging with no hardware); `asio` demands a real device — an open failure then fails `bw_start` instead of hiding behind the silent fallback. Default: try ASIO, fall back to the null sink. |
| `BWAUDIO_ASIO_DRIVER` | ASIO driver name to open. Default: auto-pick the first registered driver with enough output channels for the profile (so binaural finds a 2-ch headphone driver, cave one with at least the layout's speaker count — 26 for the CAVE array — without configuration). |
| `BWAUDIO_EMBREE` | non-`0`: ray-trace the acoustics sims on Intel Embree (see below). |
| `BWAUDIO_PATHING` | non-`0`: enable the sound-pathing sim at `bw_start` (needs scene geometry + the Steam Audio build); sources opt in via `bw_source_set_pathing`. |
| `BWAUDIO_BAKE` | non-`0`: precompute (bake) the reflection reverb over a probe grid at `bw_start`, so the reflections sim looks it up instead of ray-tracing live. |
| `BWAUDIO_NATNET_*` | `track_internal` configuration — multicast group, ports, server IP, rigid body, bitstream version, interface. Full table under [Internal tracking](#internal-tracking-track_internal-m6). |

### Ray-tracing acceleration (`BWAUDIO_EMBREE`)

The occlusion and reflection sims ray-trace on the Steam Audio scene. Set **`BWAUDIO_EMBREE=1`**
(any non-empty, non-`0` value) to run them on **Intel Embree** instead of Steam Audio's built-in
ray tracer — faster, and the lever to pull if you raise scene complexity, ray counts, or bake at
high probe density. It is **opt-in and safe**: if the linked `phonon` was not built with Embree
(or the Embree/TBB runtime is missing), the engine logs that Embree is unavailable and falls back
to the default tracer — no failure. The scene is created once and both sims share it, so the flag
applies to occlusion and reflections together. The vendored prebuilt `phonon.dll` is **not**
Embree-enabled, so the flag currently falls back; to activate it, drop in a `phonon` built with
Embree (the SDK's `STEAMAUDIO_ENABLE_EMBREE` path) and ship `embree4.dll` + `tbb*.dll` alongside.

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

`bw_unload_sound` is safe to call at any time. The core detaches references on the audio thread
and frees only after the retire-ack (see [concurrency.md](./concurrency.md)) — it will never pull
a buffer out from under a playing voice.

## Sources (control thread, non-blocking)

```c
BwSource bw_source_create(BwEngine* e);                  // handle returned synchronously
void     bw_source_destroy(BwEngine* e, BwSource s);
void     bw_source_set_priority(BwEngine* e, BwSource s, int priority);  // 0 = expendable .. 255 = protected (default 128)
void     bw_source_set_pos (BwEngine* e, BwSource s, float x, float y, float z); // ROOM space, RH
void     bw_source_set_gain(BwEngine* e, BwSource s, float linear);
void     bw_source_fade_to (BwEngine* e, BwSource s, float gain, float seconds); // engine-side timed fade
void     bw_source_fade_out(BwEngine* e, BwSource s, float seconds);             // fade to 0, then stop
void     bw_source_set_group(BwEngine* e, BwSource s, uint32_t group);           // mix group 0..7 (default 0)
void     bw_group_set_gain  (BwEngine* e, uint32_t group, float linear);         // scales every member (ramped)
void     bw_group_set_paused(BwEngine* e, uint32_t group, bool paused);          // pause a whole category
void     bw_source_set_pitch(BwEngine* e, BwSource s, float rate);               // playback rate [0.25, 4]; glided
void     bw_source_play (BwEngine* e, BwSource s, BwSound snd, bool loop);
void     bw_source_play_at(BwEngine* e, BwSource s, BwSound snd, bool loop, uint64_t start_sample); // sample-accurate
uint64_t bw_dsp_time(BwEngine* e);                       // current dsp-sample clock (device-anchored, monotonic)
void     bw_source_stop (BwEngine* e, BwSource s);
void     bw_source_set_paused(BwEngine* e, BwSource s, bool paused);   // ramped; playhead freezes
void     bw_source_seek (BwEngine* e, BwSource s, uint64_t frame);     // click-free jump (in-memory)
bool     bw_source_is_playing(BwEngine* e, BwSource s);  // control-thread poll; see below
void     bw_play_oneshot(BwEngine* e, BwSound snd, float x, float y, float z, float gain);
```

**The voice pool steals, it doesn't fail.** The pool is fixed-size. When it's full,
`bw_source_create` stops the lowest-**priority** active source to make room (255 = protected, never
stolen) — an overloaded scene drops its least important sound instead of refusing the new one. Set
music and critical SFX high. The steal is click-free: the stolen voice fades out over one block on
its own slot while the new source starts immediately on a small reserve of spare slots.

**Scheduled starts are sample-accurate.** `bw_source_play_at` begins output exactly when the
engine's dsp clock reaches `start_sample` — silent until then, then starting at the precise in-block
sample. Read "now" from `bw_dsp_time` (device sample position, monotonic) and add a delay:
`bw_dsp_time(e) + sample_rate/2` plays half a second out. `0` means play immediately (same as
`bw_source_play`); a start already in the past plays immediately, best-effort.

**Completion is a poll.** `bw_source_is_playing` is a latest-wins readback (like
`bw_get_listener_pose`): the audio thread republishes each source's playing state every block, gated
on the handle's generation. It reads `true` while a sound plays; `false` once a non-loop sound
finishes, after `stop`, or for a stale/destroyed handle. Poll it once per frame to drive an
"on finished" signal. It's best-effort — a sound shorter than your poll interval may never be
observed as playing.

**Pause and seek are click-free.**

- `bw_source_set_paused` gates the voice with a one-block ramp (~5 ms) and freezes the playhead
  once silent, so resume continues exactly where pause landed. Works for in-memory, streamed, and
  bed sounds. A paused voice still reads as *playing* — it hasn't ended.
- `bw_source_seek` jumps the content position (engine-rate frames). On a running voice: ramp out,
  jump, ramp back in (~10 ms end to end). On a paused voice the jump is immediate and it stays
  paused. Past-the-end seeks wrap for loops and end one-shots.
- Streamed sounds ignore seeks — the stream ring can't jump. `bw_source_play` always restarts
  un-paused at frame 0.

**Fades are engine-side.** `bw_source_fade_to` glides the gain over `seconds` on the audio thread
(no per-frame scripting; `seconds <= 0` sets immediately). A later `set_gain` or fade replaces the
fade in flight. `bw_source_fade_out` is the one-call "fade to silence, then stop" — it lands on the
same click-free stop path `bw_source_stop` uses.

**Mix groups (0..7)** are category-level control: a group's gain multiplies into every member's
gain solve (ramped like any solve), and pausing a group ramps its members out and freezes their
playheads exactly like per-voice pause — duck the SFX and keep the dialog, silence the ambience
category for a cutscene. Sources start in group 0; group state persists across `play`.

**Pitch** (`bw_source_set_pitch`, 1 = native, clamped `[0.25, 4]`) resamples **in-memory** sounds
with a fractional playback cursor (linear interpolation; the cursor stays integer + fraction, so a
voice running for hours never loses precision). Rate changes **glide** across a block — a change
bends the pitch rather than stepping it — and compose with Doppler (which resamples via propagation
delay on top). Streamed sounds ignore it (the stream ring is sequential); beds are unaffected. Use
it for one-shot variation, slow-mo, engines.

Positions are in **room space** (see [Coordinates and units](#coordinates-and-units)).
`bw_play_oneshot` is the fire-and-forget path: it allocates a transient voice internally
and recycles it on end, so the caller holds no handle.

### Master gain & global pause

```c
void bw_set_master_gain(BwEngine* e, float linear);   // one ramped scalar over the whole mix; live
void bw_set_paused(BwEngine* e, bool paused);         // pause EVERYTHING (ramped, playheads freeze); live
```

`bw_set_master_gain` is the volume knob / scene fade: it scales everything mixed — voices, beds,
the reverb/pathing taps — **before** the per-speaker align stage (so trims and the raw channel-test
signal stay calibrated) and before the limiter (which still guards the sum). Ramped per block;
dragging a slider never zippers. `bw_set_paused` is app-focus /
menu pause: every voice gates out with the per-voice pause machinery (memory, streamed, and bed
alike), playheads freeze, resume continues exactly, and paused voices still read as *playing*.

## Ambisonic beds (control thread)

```c
BwSound bw_load_ambix(BwEngine* e, const char* path);   // AmbiX (ACN/SN3D); 4/9/16 ch -> order 1/2/3
BwBed   bw_bed_create  (BwEngine* e);
void    bw_bed_play    (BwEngine* e, BwBed b, BwSound snd, bool loop);
void    bw_bed_set_gain(BwEngine* e, BwBed b, float linear);       // master gain, ramped
void    bw_bed_set_rotation(BwEngine* e, BwBed b, float yaw_rad);  // yaw the soundfield; glided
void    bw_bed_stop    (BwEngine* e, BwBed b);
void    bw_bed_destroy (BwEngine* e, BwBed b);
```

`bw_bed_set_rotation` yaws the recorded field about the room's vertical axis — line a capture up
with the scene, or rotate it slowly for effect. Positive yaw turns the field from room **+z**
(front) toward room **+x**. It's the closed-form yaw SH rotation (each degree's ±m channel pair
rotates by m·yaw — exact at every order, no Wigner matrices), it **glides** to the target at ~one
turn per second (click-free, live-safe), and it applies before *either* bed renderer, so the matrix
decode and the parametric analysis see the same turned field.

A **bed** is a pre-encoded **AmbiX** soundfield (ACN ordering, SN3D normalization) decoded
**straight to the speakers**. It is not panned and has no position — use it for diffuse,
ambient content.

Beds are **world-locked**: the soundfield is fixed to the room. The physical speakers are
world-fixed too, so a listener walking through the field is handled by the real acoustics,
and the binaural monitor's head-tracking applies downstream.

Load with `bw_load_ambix` (a multichannel asset; mono and other channel counts are rejected),
then drive with the `bw_bed_*` family — no position, just a master gain. Internally a bed is a
voice playing a multichannel asset, so handles and lifetime match `bw_source_*`. The decode is
a static SN3D sampling decode `(2l+1)·Y_k(dir_s)/L`, rebuilt from the layout.

```c
typedef enum { BW_BED_MATRIX = 0, BW_BED_PARAMETRIC = 1 } BwBedRenderer;
void bw_set_bed_renderer(BwEngine* e, BwBedRenderer renderer);   // live A/B (each bed crossfades)
```

Two renderers sit behind the same bed API:

- **matrix** (default) — the static SH→speaker decode above (sampling or AllRAD per
  `bw_set_bed_decoder`). Cheap and robust, but an array this size (26 speakers on the CAVE) is
  sparse for a matrix decode (directional content blurs) and the decode is world-locked around
  the array centre.
- **parametric** (`BW_BED_PARAMETRIC`) — first-order **DirAC-style** rendering: the bed's FOA
  channels are analyzed per frequency band (4 time-domain bands) into a **direction +
  diffuseness** from the smoothed intensity vector. The **non-diffuse stream is re-panned through
  the engine's own listener-relative panner** at a virtual source on the array shell — a recorded
  soundfield becomes **walkable**: an off-centre listener hears correct directions and parallax,
  which no matrix decode can provide. The **diffuse stream** decodes through the matrix into
  per-speaker **decorrelators** (incoherent envelopment instead of a correlated copy per
  speaker). Both
  streams are loudness-matched to the matrix decode; beds with fewer than 4 channels stay on the
  matrix. The switch crossfades per bed, so flipping it live is a clean A/B.

NOTE: FLAC is the natural container for lossless multichannel beds. MP3 can't carry ambisonics.

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
configured by environment variable:

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

### Pose prediction

```c
void bw_set_pose_prediction(BwEngine* e, float lead_ms);   // 0 = off (default); live
```

The tracking chain — Motive's solve, the network hop, the audio block, the DAC — puts the rendered
pose **20–40 ms behind the head**; at walking speed that is 3–6 cm of panning lag. With a lead set,
the tracked **position** is extrapolated `lead_ms` along a velocity estimated from the tracker's
own frame timestamps (smoothed over ~100 ms so Motive's frame-to-frame jitter doesn't shake the
image, speed-capped, and reset across drop-outs so a stale velocity never extrapolates). Set
`lead_ms` to your measured motion-to-ears latency; too much lead **overshoots on direction
changes**, so start at the measured value, not above it (clamped at 200 ms). Orientation is not
predicted (it only feeds the monitor). `track_internal` only.

### Extra listeners (multi-occupant compromise)

```c
void bw_set_extra_listeners(BwEngine* e, const float* xyz, uint32_t count);  // up to 3; 0 clears
```

A CAVE usually holds more than one person; single-listener panning is exact for the tracked head
and wrong for everyone else. Give the *other* occupants' positions here (`count`·3 floats, room
space; per-frame-safe, commit-gated like the pose): every source's gains become the per-speaker
**energy mean** of the per-listener solves — the L2 barycentre of the individual renderings, so
each occupant hears an image biased toward their own solve instead of one exact and N wrong.
Constant-power; works with every panner (each extra keeps its own SPCAP/VBAP cache, so the solves
stay cache-warm). The primary listener remains `bw_set_listener_pose`/tracking and still drives
spread direction, Doppler, air absorption, the reverb-send distance, and the binaural monitor.
`count = 0` restores single-listener panning. Cost: one extra point solve per listener per dirty
voice — block-rate, negligible.

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
moved listener against a not-yet-moved source) and drains the event ring.

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

A `BwMaterial` is an **opaque, engine-scoped token** — a small index, where `0` is always the
built-in `generic` default. Mint one from a preset name (11 presets, case-insensitive, Steam
Audio's published coefficients; `"generic"` returns `0` without minting) or from custom 3-band
coefficients (clamped to `[0,1]`, NaN-sanitized). The table is fixed at 64 entries; on overflow or
an unknown name the mint returns `0` and sets `bw_last_error`. Tokens are **not**
generation-checked handles — they stay valid for the engine's life, and per-triangle indices out
of range clamp to the default.

Geometry rules:

- **Room space, RH metres, CCW triangles.** `bw_scene_set_box` builds a floor-based shoebox
  (x/z centred on the origin, y from 0 — the floor — up to `h`) with inward-facing normals;
  the listener stands inside.
- **One scene, two consumers.** The same per-triangle materials feed both occlusion (per-band
  transmission) and the reflection bed (absorption/scattering) — one shared `IPLScene`.
- **Runtime swaps: occlusion yes, reflections no.** Geometry can change at runtime while only
  occlusion runs — that sim owns the scene and serializes its own commit + ray trace. Once the
  reflection bed is running the scene is locked: the reflection IR assumes a static scene, and
  an `iplSceneCommit` can't race its ray tracing. A locked call is rejected and sets
  `bw_last_error`.
- **No SDK, no-op.** Everything here is a documented no-op without the `BW_HAVE_STEAMAUDIO`
  build — except token minting, which is plain table state and still works.

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

### Manual occlusion (no SDK needed)

```c
void bw_source_set_occlusion_manual(BwEngine* e, BwSource s, float level, const float bands[3]);
```

Drives the **same** handle-gated, audio-thread-ramped publish path the ray-tracing sim uses — from
your own game logic. `level` is broadband transmittance (1 = clear .. 0 = blocked); `bands`
(optional; NULL = broadband only) is a low/mid/high tilt in `[0,1]` rendered as the same 3-biquad
transmission EQ, so a wall *muffles* rather than just attenuating. This is how a no-SDK build gets
gameplay-driven occlusion ("behind a door the game knows about", underwater, muffled-behind-a-menu)
with identical click-free rendering. Don't drive one source from both this and the sim
(`bw_source_set_occlusion`) — the sim republishes every tick and wins.

## Propagation effects (control thread; per-frame)

```c
void bw_source_set_doppler        (BwEngine* e, BwSource s, bool on);
void bw_source_set_air_absorption (BwEngine* e, BwSource s, bool on);
void bw_source_set_loudness_comp  (BwEngine* e, BwSource s, bool on);
```

Opt-in, per source, default **off**, and — unlike occlusion/directivity — **pure per-voice DSP that
needs no Steam Audio build**. All derive from the live source↔listener distance, recomputed each
block from the committed positions.

- **Doppler** renders the source through its acoustic propagation delay (`distance / c`). A per-voice
  fractional delay line glides toward that delay each block; the *glide rate is the pitch shift*
  (approaching → up, receding → down), so no velocity input is needed — it falls out of motion. The
  delay (hence the effect) **saturates past ~8 m**, which bounds the ring; enabling adds the real
  propagation latency. Best for fast movers — subtle for slow ones in a small room.
- **Air absorption** is a distance-driven one-pole **high-frequency low-pass** (far sources sound
  duller): cutoff falls ~650 Hz/m from 18 kHz near, down to a ~1.2 kHz floor. Subtle at a few metres,
  pronounced for sources placed at large *virtual* distances.
- **Loudness compensation** is the perceptual counterpart: distance attenuation takes level, and at
  lower levels the ear also loses **LF sensitivity** (ISO 226), so an attenuated source reads *thin*
  as well as far. This restores part of the body with a one-pole LF shelf (~250 Hz) whose boost
  tracks the attenuation the panner applied — +0.4 dB per dB taken, capped +8 dB. It's a
  stylization ("far, not tinny"), not physics; leave it off for strict realism.

All are **non-blocking, enqueue-only**, ramp on the audio thread (coefficients and gains glide across
the block — no zipper), and are independent of each other and of the panner / profile. They apply to
the **direct path only** — the reflection wet send is tapped *before* them, so reflections keep their
own propagation. They do not affect ambisonic beds (world-locked, no position).

## Source spread / size (control thread; per-frame)

```c
void bw_source_set_spread(BwEngine* e, BwSource s, float amount);   // 0 = point (default) .. 1 = wide
void bw_source_set_size  (BwEngine* e, BwSource s, float radius_m); // metric alternative: radius in m
typedef enum { BW_SPREAD_LOBE = 0, BW_SPREAD_MDAP = 1 } BwSpreadMode;
void bw_set_spread_mode(BwEngine* e, BwSpreadMode mode);            // engine-wide; live A/B
```

Angular **width** of a source. A waterfall, a crowd, an engine room, or ambience shouldn't
collapse to a single point — raise `spread` and the source's energy fans out across the speakers
around its direction.

Prefer **`bw_source_set_size`** when the content has a physical size: it floors the spread at the
angle the radius subtends from the tracked listener (`asin(r/d)`, fully wide once the listener is
inside the source), so a 2 m waterfall *stays* 2 m wide as the listener walks — an angular spread
changes physical size with distance. The larger of the two knobs wins, and a sized source subsumes
`bw_set_near_spread` (engulfment is just the `d < r` case). Both are per-frame-safe.

It's implemented in the per-block gain solve, not the sample loop, and **renormalised to the
panner's own power** — widening never changes loudness, and the perceived direction stays put.
It's **panner-agnostic** (works over DBAP/SPCAP/VBAP), and the change ramps click-free like any
gain change. Two render modes sit behind the knob (`bw_set_spread_mode`, an atomic live A/B like
the panner switch; sources with spread 0 are unaffected either way):

- **LOBE** (default): the panner's point gains are blended toward a width-controlled lobe
  centred on the source direction. One solve — smooth and cheap, but the extent is a reshaping
  of gains the panner computed for a *point*.
- **MDAP** (Pulkki's multiple-direction amplitude panning): a ring of virtual sources around the
  source direction (cone half-angle = `spread`·90°) is panned with the *selected panner* and
  summed. The extent is built from real panner solves, so it inherits the panner's character —
  VBAP stays sparse per direction, SPCAP stays placement-corrected — at ~13× the gain-solve cost
  (block-rate and dirty-gated, so still cheap). At spread→0 the ring collapses onto the point
  solve, so the modes meet continuously.

```c
void bw_set_decorrelation(BwEngine* e, bool on);   // off by default; live A/B
```

Either spread mode still feeds every speaker the **same signal** at different gains — coherent
copies, which collapse to phantom images between speakers and comb-filter as the tracked listener
walks. `bw_set_decorrelation` fixes that: a spread source's energy splits into a coherent share on
the normal path and an incoherent share routed through **per-speaker sparse velvet-noise filters**
(~30 taps over 30 ms, time-domain, no onset latency — Välimäki/Schlecht's velvet-noise
decorrelator). The split follows `spread` (a point source is untouched; at full spread the wide
part is fully decorrelated), power is conserved (incoherent energy adds), and the toggle ramps —
a click-free live A/B. This is what makes wide sources read as *extent* rather than *several
copies*, and it is the same decorrelator bank the parametric bed's diffuse stream uses.

```c
void bw_set_near_spread(BwEngine* e, float radius_m);   // 0 = off (default); live
```

**Near-listener widening**: a point source flying at the head physically subtends a growing solid
angle, but a point panner collapses it into the nearest speaker and snaps it across the head as it
passes. With a radius set, every source's spread is **floored at `1 − dist/radius`** — untouched
beyond the radius, fully wide at the head. `radius_m ≈ 1.0` is a good start. The widening rides the
selected spread mode and (when enabled) the decorrelators, and the changed gains ramp like any
solve. Engine-wide policy; it takes effect with each gain re-solve (continuous under tracking).

## Channel test / diagnostics (control thread)

```c
typedef enum { BW_TEST_OFF = 0, BW_TEST_SINE = 1, BW_TEST_NOISE = 2 } BwTestKind;
void     bw_test_signal(BwEngine* e, uint32_t channel, BwTestKind kind, float gain);
uint32_t bw_get_bus_levels(BwEngine* e, float* peaks, uint32_t cap);  // last block's per-channel output peak
uint32_t bw_get_active_voices(BwEngine* e);                           // last block's active voice count
```

`bw_get_active_voices` is the voice-pool gauge next to the meters: the audio thread publishes each
block's active count (playing, sound bound — paused voices count, they haven't ended). Poll it for
HUDs or health monitoring; it reads 0 until audio runs.

`bw_test_signal` drives a single **output channel** with a built-in signal (660 Hz sine or white
noise), injected **after** the per-speaker align stage — a raw value straight on the channel.
`channel` is in `[0, bw_channel_count())`; anything else is ignored.

This is a speaker-check / wiring / calibration tool: walk a tone across every channel to confirm
the channel→speaker map, find a dead speaker, set a trim. It is **not** a spatial path — it
bypasses the panner, so don't use it to "place" a sound. Per-frame-safe, takes effect next block,
no `bw_commit` needed. Any number of channels at once; `gain 0` / `BW_TEST_OFF` silences one.
Works in every profile (cave/both: a raw tone on that DVS channel; binaural: that bus channel
HRTF'd as its virtual speaker). Needs no SDK.

`bw_get_bus_levels` is the matching **readback**: each output channel's last-block peak `|sample|`
(linear), measured at the very end of the render — after align, the test signal, and the limiter.
That is exactly what the device channel received. It fills up to `cap` floats and returns the
count filled (`bw_channel_count()` when `cap` allows). Per-frame-safe (relaxed atomic reads, no
locks); levels read 0 until audio is
running. Drive channel meters or a speaker-activity display with it — the playground lights each
speaker gizmo from this.

## Panner & layout query (control thread)

```c
typedef enum { BW_PAN_DBAP = 0, BW_PAN_SPCAP = 1, BW_PAN_VBAP = 2 } BwPanner;
typedef enum { BW_DECODE_SAMPLING = 0, BW_DECODE_ALLRAD = 1 } BwBedDecoder;
void     bw_set_panner(BwEngine* e, BwPanner panner);            // load-time or live (atomic switch)
void     bw_set_dual_band(BwEngine* e, bool on);                // live A/B; wraps the selected panner
void     bw_set_bed_decoder(BwEngine* e, BwBedDecoder decoder);  // load-time
uint32_t bw_get_speakers(BwEngine* e, float* xyz, uint32_t cap); // read back the layout; NULL xyz = count only
```

`bw_set_panner` chooses the per-source panner behind the master bus:

- **DBAP** (default) is listener-relative and recomputed per block from the tracked pose.
  This is the panner for a **moving** observer roaming the array.
- **SPCAP** is a smooth, all-speaker, placement-correcting sweet-spot panner for a **fixed**
  observer: don't track, set the sweet spot once. It conserves loudspeaker power across an
  uneven array.
- **VBAP** is the sharpest — the 2-3 speakers of the containing triangle carry a source. Also
  fixed-observer, best on a cleanly-triangulable array; it falls back to DBAP otherwise.

The switch is atomic, so flipping it live is safe — the layout tool's `B` key A/Bs panners
exactly this way.

`bw_set_dual_band` (off by default, live-toggleable) **wraps** the selected panner. It splits
each source at ~700 Hz, then pans the low band with **amplitude** (pressure / velocity-vector)
normalisation and the high band with the panner's usual **power** (energy-vector) normalisation —
SPAT's "VBP Dual-Band". You get sharper low-frequency localisation for a near-centred listener.
The panning *direction* is unchanged; only the low band's level/coherence differs. It is
sweet-spot dependent like VBAP, so for a roaming listener it's a by-ear / measurement call.

`bw_set_bed_decoder` chooses the **diffuse-bed** SH→speaker decoder. It affects the ambisonic and
reflection beds only, never the point-source panner:

- **sampling** (default): the straightforward projection decode.
- **AllRAD**: decode to a uniform virtual layout, then VBAP onto the real array. Robust on an
  irregular array, at the cost of a heavier load-time build. A pole with no real speaker within
  ~60° (a floor-less array's nadir) gets an **imaginary speaker** whose decode share is
  discarded — diffuse energy aimed into the hole is dropped rather than smeared onto the
  nearest ring.

The decoder is load-time (between `bw_create` and `bw_start`); see
[`spatialization.md`](./spatialization.md) for the theory.

`bw_get_speakers` returns the effective layout (the default grid, or the `layout_path` file) as
`cap*3` floats in channel order, plus the count — the same count `bw_channel_count()` reports.
Use it to visualize or audition the geometry the engine is actually panning with.

### Channel count

```c
uint32_t bw_channel_count(BwEngine* e);   // the ACTIVE channel count (4..26), fixed at create
```

The engine's channel count **is the layout's speaker count**: a `layout_path` file with 4..26
speakers, or 26 (the default grid) with no path. `BW_CHANNELS` (26) is only the compile-time
*capacity* — a collaborator's 24-speaker array loads a 24-entry layout into the same binary, the
device opens 24 channels, and every consumer (panners, beds, reverb, monitor, calibration) follows.
Size meter/speaker arrays from this getter, not the constant.

One sharp edge: a **failed** layout load still falls back to the 26-grid default (non-fatal, reason
in `bw_last_error`) — which for a smaller install also means the *wrong channel count*. A
24-speaker deployment must check `bw_last_error` (or `bw_channel_count`) after `bw_create`.

## Tracked room EQ (control thread; live)

```c
void bw_set_tracked_room_eq(BwEngine* e, bool on);   // default ON when the layout carries a grid
```

Layouts carrying a `room_eq_grid` (written by `bw_calibrate --room-eq-grid`, one run per mic
placement) get **listener-tracked LF room correction**: each block the engine re-interpolates the
grid's per-speaker modal-cut depths at the live listener position (inverse-distance weights over
the measurement points) and the align-stage biquads glide toward them at 24 dB/s — click-free by
construction, fast enough to track a walking listener.

This is the moving-listener answer to the static `room_eq`, which `bw_start` rejects for moving
sessions. It works because the room's mode *frequencies* don't move with the listener — only how
strongly each mode reads at a position — so one per-speaker fc/Q ladder plus per-position depths
interpolate safely. Mid/HF room correction stays out of the tracked path: it is position-sensitive
at the centimetre scale ([`calibration.md`](./calibration.md)).

The switch is the live kill switch (off glides every cut to flat — a clean A/B). It's a no-op for
layouts without a grid.

## Output protection limiter (control thread; ON by default)

```c
void bw_set_limiter(BwEngine* e, bool on);                     // live
void bw_set_limiter_ceiling(BwEngine* e, float ceiling_db);    // default -1 dBFS; clamped [-60, 0]
```

The final stage on the speaker output. Everything — voices, beds, the reflection/pathing taps, the
per-speaker align stage, the test signal — passes through it before the device.

It is **linked** across channels: one gain, derived from the cross-channel peak, so engaging never
shifts the spatial image. ~1 ms attack / ~120 ms release one-poles, then a hard clamp at the
ceiling. The attack is not lookahead, so the first millisecond of a hot transient clips instead of
overshooting.

This is driver/speaker **protection** against digital overs and pathological content, not a
mastering limiter. If it engages in normal use, turn the content down. In the `binaural` profile
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

A single shared **listener-centric reverb bed**, decoded straight to the speaker channels and
summed onto the bus. It runs Steam Audio's **HYBRID** reverb: directional early-reflection
convolution (full ambisonic, order = `order`) plus a parametric (FDN) tail, decoded across the
array.
(This needs the vendored phonon's alignment patch — see [materials.md](./materials.md).) No-op
without the Steam Audio build.

> **Which reverb should you use?** There are three reflection/reverb implementations now, and they
> are complementary rather than rival. This bed is *listener-centric* — one ambisonic field decoded
> around one point — so its reflections have **no parallax** as the listener walks. The recommended
> configuration is the Steam **scene** (occlusion / directivity / pathing) + **ISM** early
> reflections (below — real parallax, per block) + the **FDN** late tail (below — infinite,
> designable), which never creates this bed at all. The trade is cost: this bed is O(1) in sources,
> the ISM costs per source. See [materials.md](./materials.md) → "Choosing an acoustics path".
> Do **not** run this bed and the ISM together: you would hear early reflections twice (the engine
> warns once through `bw_last_error`).

Configuration is load-time; the sends are live:

- **`bw_reflections_config`** must land before `bw_start` — the IR length and ambisonic order are
  baked there. Zero fields take defaults (`ir_seconds` 1.0, `order` 1, `num_rays` 4096,
  `num_bounces` 16, `wet_gain` 1.0). `enabled = 0` creates no bed; the engine behaves exactly as
  without one.
- **`bw_reflections_set_gain`** adjusts the wet level live — a single atomic the audio-thread tap
  reads.
- **`bw_source_set_reflections`** opts a source into the bed's wet send. Per-frame, non-blocking;
  with the bed disabled (or no SDK) it gates a send that goes nowhere.
- **`bw_source_set_reflection_send`** sets that source's send level (default 1.0). Drive it
  yourself for a manual dry/wet.
- **`bw_source_set_reflection_distance`** adds automatic **distance→wet** scaling on top:
  near = drier, far = wetter.

The effective send is computed and **ramped on the audio thread** (in `rt.c`, from the
source↔listener distance), so motion and on/off toggles never zipper the send.

### Directional FDN reverb (no SDK needed)

```c
void bw_reverb_fdn(BwEngine* e, bool on);                                            // LOAD-TIME
void bw_fdn_set_decay(BwEngine* e, float rt60_low_s, float rt60_high_s, float xover_hz);   // LOAD-TIME
void bw_fdn_set_decay_direction(BwEngine* e, const float dir[3], float factor);      // LOAD-TIME
```

A **phonon-free** late-reverb alternative that takes the reverb tap *instead of* the Steam bed
(one reverb bed at a time; with the FDN enabled the Steam bed is skipped). It consumes the same
mono aux send, so `bw_source_set_reflections` and the per-source send levels apply unchanged, and
`bw_reflections_set_gain` sets its return level live.

Inside: a 16-line **feedback delay network** (Householder feedback — lossless prototype, the decay
filters are the only loss), each line assigned a direction on the sphere and rendered as a plane
wave through the layout's SH→speaker bed decode. `bw_fdn_set_decay` sets a two-band decay (defaults
1.2 s low / 0.7 s high @ 2 kHz). `bw_fdn_set_decay_direction` makes the decay **anisotropic** — the
field dies faster (factor < 1) or slower toward a direction, the diagonal special case of the
Directional FDN (Alary/Politis/Schlecht, JAES 2019). Use it to *design* a space (an open side, a
treated wall); do **not** match the real room's RT60 (see [calibration.md](./calibration.md)).

Deterministic CPU (no rays, no IRs, infinite tail), works in no-SDK builds — the reverb path no
longer requires the Steam Audio SDK.

### Image-source early reflections (no SDK needed)

```c
void bw_scene_set_box(BwEngine* e, float w, float h, float d, const BwMaterial faces[6]);  // the room
void bw_source_set_early_reflections(BwEngine* e, BwSource s, bool on);   // per source; per-frame-safe
void bw_early_reflections_set_gain(BwEngine* e, float linear);            // live; default 1
```

The other half of the phonon-free acoustics path. The FDN renders the late diffuse tail; this
renders the **six first-order specular reflections** — the wall bounces that actually carry room
size and source distance. `bw_scene_set_box` now captures the shoebox **whether or not** the Steam
build is present, so one call configures the ray-traced scene (with SDK) *and* the geometric early
reflections (always).

Each reflection is rendered as a **real point source at its mirrored position**, panned through the
engine's own **listener-relative panner**. That is the payoff: reflections get correct direction
*and parallax as the listener walks* — something no shared listener-centric reverb bed (Steam's or
the FDN's) can give. Path delay, distance attenuation, and per-band wall absorption all fall out of
the geometry (walls eat treble, so a reflection is duller than the direct sound — a one-pole derived
from the material's high-vs-mid absorption). Delays **glide** and gains **ramp**, so a moving source
bends its reflections instead of stepping them.

**Order 1 only, by design.** Higher orders blend into the diffuse field within tens of milliseconds
— which is exactly what the FDN already renders, for free and with a proper decay. Spending
per-voice DSP to reproduce it would be double work. A source outside the room renders dry.

Cost: six panner solves per opted-in voice per block, plus six delay taps per sample. Opt in on the
sources that matter (a few), not on everything.

## Handle scheme

`BwSound`, `BwSource`, and `BwBed` are opaque `uint32_t` = `(index | generation<<16)`. A stale
handle (slot destroyed, then reused) fails the generation check on the audio side and is
silently dropped rather than crashing. `0` is always invalid. Treat handles as tokens — never
do arithmetic on them.

## Planned extension point

If engine-generated or procedural audio is later needed (not just wav files), add a
`bw_source_create_stream` returning a handle the caller pushes PCM into via a
per-source ring — same control model, second feeding path. The source abstraction
should therefore not assume "backed by a file," so this slots in without churn.
