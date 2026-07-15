# bw_audio — usage guide & C ABI reference

From a consumer's side this is a **control-only** API: no audio buffers, no
device, no queue, no threads — an opaque engine handle, sounds, positioned
sources, and per-frame updates. Declarations in
[`include/bw_audio.h`](../include/bw_audio.h) carry their contracts as comments;
[`examples/minimal.c`](../examples/minimal.c) runs the whole client lifecycle;
[`examples/ambisonic.c`](../examples/ambisonic.c) walks the bed API (AmbiX/FuMa loading,
rotation/tilt, the renderer and max-rE A/Bs) and [`examples/streaming.c`](../examples/streaming.c)
walks disk streaming + push sources — all three are console programs built every build.

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
bwa_desc cfg = { .profile = BWA_PROFILE_BINAURAL, .sample_rate = 48000,
                 .block_size = 256 };
bwa_engine* eng = bwa_create(&cfg);
if (!eng || bwa_start(eng) != BWA_OK) { /* see bwa_last_error(eng) */ }

bwa_sound steps = bwa_load_sound(eng, "footsteps.wav");     // once, at load time
bwa_source s    = bwa_source_create(eng);
bwa_source_play(eng, s, steps, /*loop*/ true);

// per frame, from the control thread:
bwa_set_listener_pose(eng, hx,hy,hz, qx,qy,qz,qw);        // skip if a tracker is connected
bwa_source_set_pos(eng, s, sx,sy,sz);
bwa_commit(eng);                                          // ONE commit, last

// teardown:
bwa_source_destroy(eng, s);
bwa_unload_sound(eng, steps);   // safe while playing: retire is acked internally
bwa_stop(eng); bwa_destroy(eng);
```

- **Production** uses `BWA_PROFILE_CAVE`, with `cfg.layout_path` pointing at the
  surveyed `cave_layout.json`.
- **`bwa_start` never demands hardware**: with no usable ASIO device the engine
  keeps rendering into a silent offline sink. `bwa_get_audio_backend` reports which
  backend you actually got (see [Errors](#errors--return-codes)).
- **Completion is polled, not called back**: `bwa_source_is_playing` publishes
  once per audio block. A play that hasn't reached the audio thread yet already
  reads as playing (the pending play counts), so `false` from a live handle means
  the voice really ended — poll-then-destroy is safe from the moment you play.

## Profiles and the master bus

Every voice is panned into an in-memory **master bus**, one channel per speaker
(26 on the CAVE array; see [Channel count](#channel-count)). The profile selects
who consumes it:

| profile               | consumers |
|-----------------------|-----------|
| `BWA_PROFILE_CAVE`     | bus → ASIO → Dante (production). Listener **position** only — real speakers, real ears. |
| `BWA_PROFILE_BINAURAL` | bus → HRTF monitor → any 2-ch ASIO device (desk debugging). Each bus channel is a virtual speaker at its surveyed room position; full **pose** — head orientation turns the virtual array around you. |
| `BWA_PROFILE_BOTH`     | both at once: array to Dante + a monitor tap. |

The binaural monitor hears the **same mix** production plays, panner and all —
headphone debugging exercises the array render, not a parallel stereo path. See
[architecture.md](./architecture.md).

## The threading contract

- **One control thread.** All `bwa_*` calls come from a single thread. For Unity/
  Unreal that is naturally the main thread (`LateUpdate`/`Tick`), so single-producer
  holds for free. Calling from job threads requires funneling through one thread.
- **Non-blocking per-frame calls.** Every source/listener update just enqueues a
  command; it returns immediately and lands on the next audio block.
- **Latest-wins.** Position and pose are overwritten each frame; nothing accumulates
  or backs up. Push them every frame.
- **Allocation only at load time.** `create`/`start`/`load_sound`/`source_create`
  may allocate or do I/O. The per-frame loop is pure enqueue.
- **`bwa_commit` defines frame coherence.** Position/pose updates land in *pending*
  fields; commit promotes them all to *active* as one snapshot, so the mixer never
  renders a moved listener against a not-yet-moved source. Once per frame, last
  (see [Frame boundary](#frame-boundary)).

The full model (rings, snapshot, lifetimes) is [concurrency.md](./concurrency.md).

## Coordinates and units

Room space is **right-handed, metres, +y up, +z forward**, and the origin sits
**on the floor** at the working-area centre (Motive's ground-plane calibration) —
OptiTrack rigid-body poses pass through unchanged and y is height above the
floor. An identity orientation faces +z with the right ear at −x; derive basis
vectors from `BWA_ROOM_AHEAD` / `BWA_ROOM_UP` / `BWA_ROOM_RIGHT` in the header
rather than re-hardcoding the convention. The engine's world-locked decodes and
its default listener position use the **array centroid** (the nominal listening
point), not the origin. Gains are linear (1 = unity); sound offsets are
engine-rate sample frames. The engine bindings convert from Unity/Unreal
coordinates at the boundary ([integration.md](./integration.md)).

## Lifecycle

```c
bwa_engine* bwa_create(const bwa_desc* cfg);
bwa_result  bwa_start(bwa_engine* e);   // opens device(s), starts audio thread; BWA_OK = 0
bwa_result  bwa_stop(bwa_engine* e);
void        bwa_destroy(bwa_engine* e);
const char* bwa_last_error(bwa_engine* e);
```

`bwa_desc` — zero-init and set what you need; every field's zero is its default:

| field            | meaning                                                              |
|------------------|---------------------------------------------------------------------|
| `profile`        | `cave` / `binaural` / `both` (see architecture.md)                   |
| `layout_path`    | surveyed speaker geometry (JSON); cave/both                         |
| `hrtf_path`      | HRTF (SOFA) or NULL for built-in; binaural/both                     |
| `sample_rate`    | 48000                                                               |
| `block_size`     | ASIO buffer hint (256/512)                                          |
| `sink`           | output-device policy: `BWA_SINK_AUTO` (0, default — try ASIO, fall back to the silent null sink), `BWA_SINK_ASIO` (demand a device: an open failure fails `bwa_start` loudly), `BWA_SINK_NULL` (force the offline sink — CI, profiling, tracking-only) |
| `asio_driver`    | ASIO driver name to open; NULL = auto-pick the first registered driver with enough output channels for the profile (binaural finds a 2-ch headphone driver, cave a ≥layout-count one) |
| `embree`         | ray-trace the acoustics sims on Intel Embree; silently falls back to the default tracer if the phonon build lacks it — see [Ray-tracing acceleration](#ray-tracing-acceleration-bwa_descembree) |
| `enable_pathing` | run the sound-pathing sim from `bwa_start` (needs scene geometry + the Steam Audio build); sources opt in via `bwa_source_set_pathing` |
| `bed_decoder`    | diffuse-bed SH→speaker decoder: sampling (0, default) or AllRAD — see [Panner & layout query](#panner--layout-query-control-thread) |
| `reserved[4]`    | zero; room to grow without an ABI break                             |

## Errors & return codes

The API reports failure three ways, all read on the **control thread**:

- **Pointer/handle returns.** `bwa_create` returns `NULL` on failure; `bwa_load_sound` /
  `bwa_source_create` return `0`.
- **`bwa_result` returns.** `bwa_create`'s companions `bwa_start`/`bwa_stop` and
  `bwa_tracker_connect` return `BWA_OK` (0) on success, a nonzero code otherwise — the
  `bwa_result` enum in [`include/bw_audio.h`](../include/bw_audio.h), listed below.
- **`bwa_last_error`.** A human-readable string for the most recent failure on that engine, or
  `NULL` if none. It stays valid until the next `bwa_*` call on the same engine, so read it
  right after the failing call.

| code            | value | cause |
|-----------------|-------|-------|
| `BWA_OK`         | 0     | success |
| `BWA_ERR_CONFIG` | 1     | invalid `bwa_desc` (bad profile, sample_rate, block_size) |
| `BWA_ERR_DEVICE` | 2     | ASIO/output device could not be opened, lacked enough output channels for the layout, or failed to start |
| `BWA_ERR_LAYOUT` | 3     | `layout_path` missing/unparseable/failed validation (see [`layout-schema.md`](./layout-schema.md)) |
| `BWA_ERR_HRTF`   | 4     | `hrtf_path` (SOFA) could not be loaded |
| `BWA_ERR_STATE`  | 5     | called in the wrong state (e.g. `bwa_start` while already running) |
| `BWA_ERR_INTERNAL` | 6   | unexpected internal failure; `bwa_last_error` carries detail |
| `BWA_ERR_TRACKER` | 7    | `bwa_tracker_connect` failed (socket open, name didn't resolve, room_eq layout) |

What actually comes back today:

- `bwa_create` returns `NULL` (bad config / out of memory). No code.
- `bwa_start` returns `BWA_OK`, `BWA_ERR_CONFIG` (NULL engine, or a `room_eq` layout in a
  moving-listener session), or `BWA_ERR_DEVICE`. Codes **3–6 are reserved**, not yet
  returned. A redundant `bwa_start` is a no-op that returns `BWA_OK`.
- `bwa_tracker_connect` returns `BWA_OK`, `BWA_ERR_CONFIG` (NULL args / a static-`room_eq`
  layout), or `BWA_ERR_TRACKER`.
- A bad `layout_path` or `hrtf_path` does **not** fail `bwa_start`. The engine degrades —
  default speaker grid, simple-pan monitor — and records why in `bwa_last_error`.

So if your session depends on the surveyed layout or a SOFA HRTF, read `bwa_last_error` after a
*successful* `bwa_create`/`bwa_start` to confirm they actually loaded.

That fallback also **changes the channel count**: the default grid is 26 speakers, so a failed
load on a 12- or 24-speaker install silently renders 26 channels. If your layout isn't 26, check
`bwa_last_error` (or `bwa_get_channel_count`) right after `bwa_create` — see
[Channel count](#channel-count).

Per-frame `void` calls (`bwa_source_set_pos`, `bwa_commit`, …) never report errors — they only
enqueue onto the command ring. A full ring drops the command silently, but the ring is sized
(`RING_CAP`) for a worst-case frame burst, and only a stalled control thread could fill it
(see [`concurrency.md`](./concurrency.md)). Position and pose are latest-wins, so a dropped
update is corrected one frame later.

## Environment variables

There are none. Everything is configured through the API: the output device is
`bwa_desc.sink`/`asio_driver`, Embree is `bwa_desc.embree`, pathing is `bwa_desc.enable_pathing`,
reflection baking is `bwa_reflections_desc.bake`, and the OptiTrack connection is
`bwa_tracker_connect` (all of these used to be `BWA_*` env vars). One door per knob.

### Ray-tracing acceleration (`bwa_desc.embree`)

The occlusion and reflection sims ray-trace on the Steam Audio scene. Set **`embree = true`**
to run them on **Intel Embree** instead of Steam Audio's built-in
ray tracer — faster, and the lever to pull if you raise scene complexity, ray counts, or bake at
high probe density. It is **opt-in and safe**: if the linked `phonon` was not built with Embree
(or the Embree/TBB runtime is missing), the engine logs that Embree is unavailable and falls back
to the default tracer — no failure. The scene is created once and both sims share it, so the flag
applies to occlusion and reflections together. The vendored prebuilt `phonon.dll` is **not**
Embree-enabled, so the flag currently falls back; to activate it, drop in a `phonon` built with
Embree (the SDK's `STEAMAUDIO_ENABLE_EMBREE` path) and ship `embree4.dll` + `tbb*.dll` alongside.

## Assets (control thread, file I/O)

```c
bwa_sound bwa_load_sound(bwa_engine* e, const char* path);           // decode fully into RAM; 0 = failure
bwa_sound bwa_load_sound_streaming(bwa_engine* e, const char* path); // stream from disk (long files); 0 = failure
void    bwa_unload_sound(bwa_engine* e, bwa_sound snd);               // safe; retire-acked internally
```

Load sounds once, at load time. **WAV, FLAC, and MP3** are accepted (decoded to mono
float by dr_libs, dispatched by file extension). If the file's sample rate differs from
the engine's, it is **resampled to the engine rate at load** (a windowed-sinc pass) — so a
44.1 kHz MP3 plays correctly on a 48 kHz engine; only the one-time load cost is paid.

`bwa_load_sound_streaming` is for long assets (music, ambience) you don't want resident in RAM: a
background thread feeds the voice from disk as it plays. It is **mono, at the engine sample rate**
(a rate mismatch fails — pre-convert, or use `bwa_load_sound` which resamples), plays on **one voice
at a time**, and does not support `bwa_source_seek` (the ring can't jump).

`bwa_unload_sound` is safe to call at any time. The core detaches references on the audio thread
and frees only after the retire-ack (see [concurrency.md](./concurrency.md)) — it will never pull
a buffer out from under a playing voice.

## Sources (control thread, non-blocking)

```c
bwa_source bwa_source_create(bwa_engine* e);                  // handle returned synchronously
void     bwa_source_destroy(bwa_engine* e, bwa_source s);
void     bwa_source_set_priority(bwa_engine* e, bwa_source s, int priority);  // 0 = expendable .. 255 = protected (default 128)
void     bwa_source_set_pos (bwa_engine* e, bwa_source s, float x, float y, float z); // ROOM space, RH
void     bwa_source_set_gain(bwa_engine* e, bwa_source s, float linear);
void     bwa_source_fade_to (bwa_engine* e, bwa_source s, float gain, float seconds); // engine-side timed fade
void     bwa_source_fade_out(bwa_engine* e, bwa_source s, float seconds);             // fade to 0, then stop
void     bwa_source_set_group(bwa_engine* e, bwa_source s, uint32_t group);           // mix group 0..7 (default 0)
void     bwa_group_set_gain  (bwa_engine* e, uint32_t group, float linear);         // scales every member (ramped)
void     bwa_group_set_paused(bwa_engine* e, uint32_t group, bool paused);          // pause a whole category
void     bwa_source_set_pitch(bwa_engine* e, bwa_source s, float rate);               // playback rate [0.25, 4]; glided
void     bwa_source_play (bwa_engine* e, bwa_source s, bwa_sound snd, bool loop);
void     bwa_source_play_at(bwa_engine* e, bwa_source s, bwa_sound snd, bool loop, uint64_t start_sample); // sample-accurate
uint64_t bwa_get_dsp_time(bwa_engine* e);                       // current dsp-sample clock (device-anchored, monotonic)
void     bwa_source_stop (bwa_engine* e, bwa_source s);
void     bwa_source_set_paused(bwa_engine* e, bwa_source s, bool paused);   // ramped; playhead freezes
void     bwa_source_seek (bwa_engine* e, bwa_source s, uint64_t frame);     // click-free jump (in-memory)
bool     bwa_source_is_playing(bwa_engine* e, bwa_source s);  // control-thread poll; see below
void     bwa_play_oneshot(bwa_engine* e, bwa_sound snd, float x, float y, float z, float gain);
```

**The voice pool steals, it doesn't fail.** The pool is fixed-size. When it's full,
`bwa_source_create` stops the lowest-**priority** active source to make room (255 = protected, never
stolen) — an overloaded scene drops its least important sound instead of refusing the new one. Set
music and critical SFX high. The steal is click-free: the stolen voice fades out over one block on
its own slot while the new source starts immediately on a small reserve of spare slots.

**Scheduled starts are sample-accurate.** `bwa_source_play_at` begins output exactly when the
engine's dsp clock reaches `start_sample` — silent until then, then starting at the precise in-block
sample. Read "now" from `bwa_get_dsp_time` (device sample position, monotonic) and add a delay:
`bwa_get_dsp_time(e) + sample_rate/2` plays half a second out. `0` means play immediately (same as
`bwa_source_play`); a start already in the past plays immediately, best-effort.

**Completion is a poll.** `bwa_source_is_playing` is a latest-wins readback (like
`bwa_get_listener_pose`): the audio thread republishes each source's playing state every block, gated
on the handle's generation. It reads `true` while a sound plays; `false` once a non-loop sound
finishes, after `stop`, or for a stale/destroyed handle. Poll it once per frame to drive an
"on finished" signal. It's best-effort — a sound shorter than your poll interval may never be
observed as playing.

**Pause and seek are click-free.**

- `bwa_source_set_paused` gates the voice with a one-block ramp (~5 ms) and freezes the playhead
  once silent, so resume continues exactly where pause landed. Works for in-memory, streamed, and
  bed sounds. A paused voice still reads as *playing* — it hasn't ended.
- `bwa_source_seek` jumps the content position (engine-rate frames). On a running voice: ramp out,
  jump, ramp back in (~10 ms end to end). On a paused voice the jump is immediate and it stays
  paused. Past-the-end seeks wrap for loops and end one-shots.
- Streamed sounds ignore seeks — the stream ring can't jump. `bwa_source_play` always restarts
  un-paused at frame 0.

**Fades are engine-side.** `bwa_source_fade_to` glides the gain over `seconds` on the audio thread
(no per-frame scripting; `seconds <= 0` sets immediately). A later `set_gain` or fade replaces the
fade in flight. `bwa_source_fade_out` is the one-call "fade to silence, then stop" — it lands on the
same click-free stop path `bwa_source_stop` uses.

**Mix groups (0..7)** are category-level control: a group's gain multiplies into every member's
gain solve (ramped like any solve), and pausing a group ramps its members out and freezes their
playheads exactly like per-voice pause — duck the SFX and keep the dialog, silence the ambience
category for a cutscene. Sources start in group 0; group state persists across `play`.

**Pitch** (`bwa_source_set_pitch`, 1 = native, clamped `[0.25, 4]`) resamples **in-memory** sounds
with a fractional playback cursor (linear interpolation; the cursor stays integer + fraction, so a
voice running for hours never loses precision). Rate changes **glide** across a block — a change
bends the pitch rather than stepping it — and compose with Doppler (which resamples via propagation
delay on top). Streamed sounds ignore it (the stream ring is sequential); beds are unaffected. Use
it for one-shot variation, slow-mo, engines.

Positions are in **room space** (see [Coordinates and units](#coordinates-and-units)).
`bwa_play_oneshot` is the fire-and-forget path: it allocates a transient voice internally
and recycles it on end, so the caller holds no handle.

### Procedural (push) sources

```c
bwa_source bwa_source_create_stream(bwa_engine* e);            // 0 = failure (see bwa_last_error)
uint32_t   bwa_source_push(bwa_engine* e, bwa_source s, const float* frames, uint32_t n); // frames accepted
uint32_t   bwa_source_push_space(bwa_engine* e, bwa_source s); // frames a push would accept right now
void       bwa_source_push_end(bwa_engine* e, bwa_source s);   // end-of-data: ends once drained
```

Engine-generated audio without a file: `bwa_source_create_stream` returns a source whose voice plays
PCM **you push** — mono float frames at the engine sample rate, through a per-source ring (~1.3 s
deep at 48 kHz). It is a normal source in every other way: position, gain, spread, occlusion,
Doppler, groups, fades, pause — the full spatial path applies. Use it for synthesis, network audio,
or bridging another engine's output.

Three rules cover the model:

- **The stream clock is data-driven.** The voice starts consuming at create: silence until your
  first push, and if you fall behind (**underrun**) it renders silence *without losing your place* —
  output resumes at the next pushed sample. It slips, it never drops. Stay a frame's worth ahead;
  `bwa_source_push` returns the count accepted (short when the ring is full — pace with
  `bwa_source_push_space`).
- **Push from the one control thread**, like every `bwa_*` call — the ring is single-producer/
  single-consumer. Non-finite samples are written as 0 (nothing hands NaN to the audio thread).
- **Ending is one-way.** `bwa_source_push_end` marks end-of-data: the voice ends
  (`bwa_source_is_playing` → false) once the ring drains, and further pushes are refused. A push
  source is not restartable — create a new one. `bwa_source_stop` and `bwa_source_fade_out` end it
  the same way (stop now / fade first; the unconsumed remainder is dropped, pushes are refused) —
  use `bwa_source_set_paused` to silence one temporarily. `bwa_source_destroy` releases the ring
  (safe while playing; retire-acked like any sound).

`bwa_source_play` / `seek` / `pitch` don't apply — a push source plays what you push (`play` is
rejected with an error; streams ignore seek/pitch as always). The reverse mix-up is reported too:
`bwa_source_push` / `push_space` / `push_end` on a live **non-push** source return 0 / do nothing
**with an error** (`bwa_last_error`), so a wrong handle never masquerades as ring backpressure (a
stale handle stays the usual silent no-op). A full pool can **steal** a push
source like any voice, dropping its pushed audio — protect an important one with priority 255.

### Master gain & global pause

```c
void bwa_set_master_gain(bwa_engine* e, float linear);   // one ramped scalar over the whole mix; live
void bwa_set_paused(bwa_engine* e, bool paused);         // pause EVERYTHING (ramped, playheads freeze); live
```

`bwa_set_master_gain` is the volume knob / scene fade: it scales everything mixed — voices, beds,
the reverb/pathing taps — **before** the per-speaker align stage (so trims and the raw channel-test
signal stay calibrated) and before the limiter (which still guards the sum). Ramped per block;
dragging a slider never zippers. `bwa_set_paused` is app-focus /
menu pause: every voice gates out with the per-voice pause machinery (memory, streamed, and bed
alike), playheads freeze, resume continues exactly, and paused voices still read as *playing*.

## Ambisonic beds (control thread)

```c
bwa_sound bwa_load_ambix(bwa_engine* e, const char* path);   // AmbiX (ACN/SN3D); 4/9/16 ch -> order 1/2/3
bwa_sound bwa_load_fuma (bwa_engine* e, const char* path);   // legacy FuMa B-format; converted at load
bwa_bed   bwa_bed_create  (bwa_engine* e);
void    bwa_bed_play    (bwa_engine* e, bwa_bed b, bwa_sound snd, bool loop);
void    bwa_bed_set_gain(bwa_engine* e, bwa_bed b, float linear);       // master gain, ramped
void    bwa_bed_set_rotation(bwa_engine* e, bwa_bed b, float yaw_rad);  // yaw the soundfield; glided
void    bwa_bed_set_orientation(bwa_engine* e, bwa_bed b,               // full 3-axis (yaw/pitch/roll);
                              float yaw_rad, float pitch_rad, float roll_rad);   //   glided, live
void    bwa_bed_stop    (bwa_engine* e, bwa_bed b);
void    bwa_bed_destroy (bwa_engine* e, bwa_bed b);

// same voice machinery as the bwa_source_* calls of the same name — bed-named so bed code
// never mixes prefixes (semantics under "Sources"):
void    bwa_bed_fade_to     (bwa_engine* e, bwa_bed b, float gain, float seconds);
void    bwa_bed_fade_out    (bwa_engine* e, bwa_bed b, float seconds);   // fade, then click-free stop
void    bwa_bed_set_paused  (bwa_engine* e, bwa_bed b, bool paused);     // freeze/resume in place
void    bwa_bed_seek        (bwa_engine* e, bwa_bed b, uint64_t frame);
void    bwa_bed_set_priority(bwa_engine* e, bwa_bed b, int priority);    // beds share the voice pool —
void    bwa_bed_set_group   (bwa_engine* e, bwa_bed b, uint32_t group);  //   protect a music bed with 255
bool    bwa_bed_is_playing  (bwa_engine* e, bwa_bed b);
```

`bwa_bed_set_rotation` yaws the recorded field about the room's vertical axis — line a capture up
with the scene, or rotate it slowly for effect. Positive yaw turns the field from room **+z**
(front) toward room **+x**. It's the closed-form yaw SH rotation (each degree's ±m channel pair
rotates by m·yaw — exact at every order, no Wigner matrices), it **glides** to the target at ~one
turn per second (click-free, live-safe), and it applies before *either* bed renderer, so the matrix
decode and the parametric analysis see the same turned field.

`bwa_bed_set_orientation` is the full 3-axis version — for **levelling** a capture whose "front"
wasn't upright, or tilting a field for effect. Positive **pitch** tilts the field's front (+z)
upward; positive **roll** tilts its top toward the room's right (−x); applied roll → pitch → yaw.
Yaw-only stays on the closed-form phasor path; any pitch/roll runs a full SH rotation matrix
(the Ivanic-Ruedenberg recursion), rebuilt per block from the glided angles and interpolated per
sample — same click-free, live semantics as yaw. `bwa_bed_set_rotation(yaw)` is the shorthand for
`bwa_bed_set_orientation(yaw, 0, 0)` (it resets pitch/roll).

A **bed** is a pre-encoded **AmbiX** soundfield (ACN ordering, SN3D normalization) decoded
**straight to the speakers**. It is not panned and has no position — use it for diffuse,
ambient content.

Beds are **world-locked**: the soundfield is fixed to the room. The physical speakers are
world-fixed too, so a listener walking through the field is handled by the real acoustics,
and the binaural monitor's head-tracking applies downstream.

Load with `bwa_load_ambix` (a multichannel asset; mono and other channel counts are rejected),
then drive with the `bwa_bed_*` family — no position, just a master gain. Legacy **FuMa** B-format
recordings (`.amb` and friends: WXYZ channel order, MaxN normalization, the W −3 dB) load with
`bwa_load_fuma` instead — the conversion happens at load, so past that call the asset is
indistinguishable from an AmbiX load of the same field. Full 3D sets only (4/9/16 channels).
Internally a bed is a
voice playing a multichannel asset, so handles and lifetime match `bwa_source_*` — which is why
fades, pause/seek, priority, and groups work on beds: they are the same per-voice machinery,
re-exported under the bed prefix. Note the priority default (128) means a bed **can be stolen**
by a full-pool `bwa_source_create` like any other voice — set 255 on a bed that must survive an
SFX overload. The decode is a static SN3D sampling decode `(2l+1)·Y_k(dir_s)/L`, rebuilt from
the layout.

```c
void bwa_set_max_re(bwa_engine* e, bool on);   // off by default; live A/B (crossfaded)
```

`bwa_set_max_re` puts **max-rE weighting** (Zotter & Frank's psychoacoustic decoder weights) on the
engine's SH→speaker decode: the higher ambisonic orders are tapered, which suppresses the decode's
sidelobes and lengthens the energy vector — **better localization away from the sweet spot**,
exactly the walking-listener case, at a slightly wider main lobe. The weights are
diffuse-energy-normalized per content order, so A and B stay level-fair. It reaches every consumer
of the engine's own decode — bed matrix rendering (sampling *and* AllRAD) and the FDN reverb's line
render — but not the point-source panners (DBAP/SPCAP/VBAP pan, they don't decode) and not phonon's
own decodes (reflection bed, pathing, the HRTF monitor). Off by default: the unweighted decode is
the incumbent; bake the winner after the hardware bake-off.

```c
typedef enum { BWA_BED_MATRIX = 0, BWA_BED_PARAMETRIC = 1 } bwa_bed_renderer;
void bwa_set_bed_renderer(bwa_engine* e, bwa_bed_renderer renderer);   // live A/B (each bed crossfades)
```

Two renderers sit behind the same bed API:

- **matrix** (default) — the static SH→speaker decode above (sampling or AllRAD per
  `bwa_desc.bed_decoder`). Cheap and robust, but an array this size (26 speakers on the CAVE) is
  sparse for a matrix decode (directional content blurs) and the decode is world-locked around
  the array centre.
- **parametric** (`BWA_BED_PARAMETRIC`) — first-order **DirAC-style** rendering: the bed's FOA
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

## Listener (control thread; skip when a tracker is connected)

```c
void bwa_set_listener_pose(bwa_engine* e, float px,float py,float pz,
                                       float qx,float qy,float qz,float qw);
```

Position is used by both consumers. Orientation (the quaternion) is used by the
**binaural monitor only**; the array render ignores it. With a tracker connected,
do not call this — the core samples the freshest OptiTrack pose at block time.

### Internal tracking (`bwa_tracker_connect`, M6)

```c
typedef struct {
    const char* multicast;       // NULL -> "239.255.42.99"
    const char* server;          // Motive host (handshake + rigid-body names); NULL -> multicast-only
    const char* local_iface;     // local interface IP to bind/join on; NULL -> any
    uint16_t    data_port;       // 0 -> 1511
    uint16_t    command_port;    // 0 -> 1510
    int32_t     rigid_body_id;   // streaming ID; 0 -> first rigid body in the frame
    const char* rigid_body_name; // by name instead of ID (needs `server`); NULL -> use the ID
    int32_t     version_major, version_minor;   // 0 -> handshake (or default 3.1)
    uint32_t    reserved[4];
} bwa_tracker_desc;
bwa_result bwa_tracker_connect(bwa_engine* e, const bwa_tracker_desc* desc);   // may block
void       bwa_tracker_disconnect(bwa_engine* e);
```

Connect the engine to a NatNet (Motive) stream and it ingests the pose itself, sampling the
freshest head pose on the **audio thread at block time** — lower latency than pushing pose
through the command ring — overriding any `bwa_set_listener_pose`. This is a runtime call, like
every other NatNet client: connect before or after `bwa_start`, reconnect with new settings
(the replacement is glitch-free — the audio thread swaps pose sources between blocks), or
disconnect to fall back to the committed/pushed pose. It is lifecycle-class (opens a socket, may
block briefly), not a per-frame call. Zero fields take the defaults shown.

A rigid-body **name** is resolved to its streaming ID at connect via the model definitions (a
`NAT_REQUEST_MODELDEF` exchange) — this needs `server` and NatNet ≥ 4; a name that doesn't
resolve fails the connect with `BWA_ERR_TRACKER` and the reason in `bwa_last_error`. A failed
connect leaves the engine running on the committed/default listener — nothing tears down.
The tracker's lifetime is independent of `bwa_start`/`bwa_stop` (it survives a device restart);
`bwa_destroy` disconnects it. One invariant carries over from `bwa_start`: a layout carrying
**static `room_eq`** (fixed-listener room correction) refuses a tracker — recalibrate with
`--room-eq-grid` for tracked sessions (see [calibration.md](./calibration.md)).

### Pose prediction

```c
void bwa_set_pose_prediction(bwa_engine* e, float lead_ms);   // 0 = off (default); live
```

The tracking chain — Motive's solve, the network hop, the audio block, the DAC — puts the rendered
pose **20–40 ms behind the head**; at walking speed that is 3–6 cm of panning lag. With a lead set,
the tracked **position** is extrapolated `lead_ms` along a velocity estimated from the tracker's
own frame timestamps (smoothed over ~100 ms so Motive's frame-to-frame jitter doesn't shake the
image, speed-capped, and reset across drop-outs so a stale velocity never extrapolates). Set
`lead_ms` to your measured motion-to-ears latency; too much lead **overshoots on direction
changes**, so start at the measured value, not above it (clamped at 200 ms). Orientation is not
predicted (it only feeds the monitor). Internal tracking only (needs a connected tracker).

### Extra listeners (multi-occupant compromise)

```c
void bwa_set_extra_listeners(bwa_engine* e, const float* xyz, uint32_t count);  // up to 3; 0 clears
```

A CAVE usually holds more than one person; single-listener panning is exact for the tracked head
and wrong for everyone else. Give the *other* occupants' positions here (`count`·3 floats, room
space; per-frame-safe, commit-gated like the pose): every source's gains become the per-speaker
**energy mean** of the per-listener solves — the L2 barycentre of the individual renderings, so
each occupant hears an image biased toward their own solve instead of one exact and N wrong.
Constant-power; works with every panner (each extra keeps its own SPCAP/VBAP cache, so the solves
stay cache-warm). The primary listener remains `bwa_set_listener_pose`/tracking and still drives
spread direction, Doppler, air absorption, the reverb-send distance, and the binaural monitor.
`count = 0` restores single-listener panning. Cost: one extra point solve per listener per dirty
voice — block-rate, negligible.

### Reading back the pose

```c
void bwa_get_listener_pose(bwa_engine* e, float p[3], float q[4]);
```

Returns the pose the engine is currently rendering with — the committed pose, or, under
with a tracker connected — the freshest tracked pose. Safe to poll from the control thread (published by
the audio thread through a seqlock). For visuals, logging, or bringing up the tracker (see the
`bwa_track_monitor` example). Returns identity until the first audio block / tracked frame.

## Frame boundary

```c
void bwa_commit(bwa_engine* e);
```

Call once per frame after pushing all source and listener updates. It promotes this
frame's position/pose to a coherent snapshot (so the audio thread never mixes a
moved listener against a not-yet-moved source) and drains the event ring.

## Materials & scene geometry (control thread; load-time)

```c
typedef enum { BWA_MAT_GENERIC = 0, BWA_MAT_BRICK, BWA_MAT_CONCRETE, BWA_MAT_CERAMIC,
               BWA_MAT_GRAVEL, BWA_MAT_CARPET, BWA_MAT_GLASS, BWA_MAT_PLASTER,
               BWA_MAT_WOOD, BWA_MAT_METAL, BWA_MAT_ROCK } bwa_material_type;
bwa_material bwa_material_preset(bwa_engine* e, bwa_material_type preset);   // GENERIC = token 0
bwa_material bwa_material_define(bwa_engine* e, const float absorption[3], float scattering,
                                           const float transmission[3]);
void bwa_scene_set_mesh_mat(bwa_engine* e, const float* verts, int nverts, const int* tris, int ntris,
                           const bwa_material* tri_material);     // one token per triangle
void bwa_scene_set_box     (bwa_engine* e, float w, float h, float d, const bwa_material faces[6]); // -x,+x,-y,+y,-z,+z
```

A `bwa_material` is an **opaque, engine-scoped token** — a small index, where `0` is always the
built-in `GENERIC` default. Mint one from a preset (the `bwa_material_type` enum — Steam Audio's
published coefficients; `BWA_MAT_GENERIC` returns `0` without minting) or from custom 3-band
coefficients (clamped to `[0,1]`, NaN-sanitized). **Both paths return the same kind of token** —
the enum only names the preset; custom materials are handles from the same table. The table is
fixed at 64 entries; on overflow (or an out-of-range enum value) the mint returns `0` and sets
`bwa_last_error`. Tokens are **not** generation-checked handles — they stay valid for the
engine's life, and per-triangle indices out of range clamp to the default.

Geometry rules:

- **Room space, RH metres, CCW triangles.** `bwa_scene_set_box` builds a floor-based shoebox
  (x/z centred on the origin, y from 0 — the floor — up to `h`) with inward-facing normals;
  the listener stands inside.
- **One scene, two consumers.** The same per-triangle materials feed both occlusion (per-band
  transmission) and the reflection bed (absorption/scattering) — one shared `IPLScene`.
- **Runtime swaps: occlusion yes, reflections no.** Geometry can change at runtime while only
  occlusion runs — that sim owns the scene and serializes its own commit + ray trace. Once the
  reflection bed is running the scene is locked: the reflection IR assumes a static scene, and
  an `iplSceneCommit` can't race its ray tracing. A locked call is rejected and sets
  `bwa_last_error`.
- **No SDK, no-op.** Everything here is a documented no-op without the `BWA_HAVE_STEAMAUDIO`
  build — except token minting, which is plain table state and still works.

## Occlusion & directivity (control thread; per-frame except where noted)

```c
void  bwa_source_set_occlusion (bwa_engine* e, bwa_source s, bool on);
float bwa_source_get_occlusion (bwa_engine* e, bwa_source s);   // 1 = clear .. 0 = blocked (HUD)
void  bwa_source_set_orientation(bwa_engine* e, bwa_source s, float qx, float qy, float qz, float qw);
void  bwa_source_set_directivity(bwa_engine* e, bwa_source s, float weight, float power); // 0=omni/.5=card/1=fig8
void  bwa_source_set_directivity_preset(bwa_engine* e, bwa_source s, bwa_directivity pattern);
float bwa_source_get_directivity(bwa_engine* e, bwa_source s);   // 1 = on-axis/omni .. 0 = null (HUD)
```

The setters are **non-blocking, enqueue-only** (safe in the hot loop). The off-thread sim ray-traces
at a low rate and publishes a per-source scalar (+ a 3-band transmission tilt for occlusion) that the
**audio thread ramps** — never a jump. Occlusion and directivity are independent (a source can be
directional without being occluded). The `_get_` reads return the latest published scalar for
HUD/diagnostics and are safe to poll. No-ops without the Steam Audio build.

### Manual occlusion (no SDK needed)

```c
void bwa_source_set_occlusion_manual(bwa_engine* e, bwa_source s, float level, const float bands[3]);
```

Drives the **same** handle-gated, audio-thread-ramped publish path the ray-tracing sim uses — from
your own game logic. `level` is broadband transmittance (1 = clear .. 0 = blocked); `bands`
(optional; NULL = broadband only) is a low/mid/high tilt in `[0,1]` rendered as the same 3-biquad
transmission EQ, so a wall *muffles* rather than just attenuating. This is how a no-SDK build gets
gameplay-driven occlusion ("behind a door the game knows about", underwater, muffled-behind-a-menu)
with identical click-free rendering. Don't drive one source from both this and the sim
(`bwa_source_set_occlusion`) — the sim republishes every tick and wins.

## Propagation effects (control thread; per-frame)

```c
void bwa_source_set_doppler        (bwa_engine* e, bwa_source s, bool on);
void bwa_source_set_air_absorption (bwa_engine* e, bwa_source s, bool on);
void bwa_source_set_loudness_comp  (bwa_engine* e, bwa_source s, bool on);
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
void bwa_source_set_spread(bwa_engine* e, bwa_source s, float amount);   // 0 = point (default) .. 1 = wide
void bwa_source_set_size  (bwa_engine* e, bwa_source s, float radius_m); // metric alternative: radius in m
typedef enum { BWA_SPREAD_LOBE = 0, BWA_SPREAD_MDAP = 1, BWA_SPREAD_SPECTRAL = 2 } bwa_spread_mode;
void bwa_set_spread_mode(bwa_engine* e, bwa_spread_mode mode);            // engine-wide; live A/B
```

Angular **width** of a source. A waterfall, a crowd, an engine room, or ambience shouldn't
collapse to a single point — raise `spread` and the source's energy fans out across the speakers
around its direction.

Prefer **`bwa_source_set_size`** when the content has a physical size: it floors the spread at the
angle the radius subtends from the tracked listener (`asin(r/d)`, fully wide once the listener is
inside the source), so a 2 m waterfall *stays* 2 m wide as the listener walks — an angular spread
changes physical size with distance. The larger of the two knobs wins, and a sized source subsumes
`bwa_set_near_spread` (engulfment is just the `d < r` case). Both are per-frame-safe.

It's implemented in the per-block gain solve, not the sample loop, and **renormalised to the
panner's own power** — widening never changes loudness, and the perceived direction stays put.
It's **panner-agnostic** (works over DBAP/SPCAP/VBAP), and the change ramps click-free like any
gain change. Three render modes sit behind the knob (`bwa_set_spread_mode`, an atomic live A/B like
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
- **SPECTRAL** (frequency-dependent panning — Zotter & Frank's phantom-source widening): the
  source splits into **6 bands** and each band is panned to its **own direction** inside the
  spread cone — the LF band stays on the source direction, the upper bands scatter around the
  cone ring, each a real panner solve. The ear integrates the scattered spectrum into *width*,
  and because different frequencies come from different speakers, there are **no coherent copies
  to collapse or comb-filter** as the tracked listener walks — extent without decorrelation
  noise. Constant-power exactly (the band-overlap correlation is compensated at the solve).
  Costs ~6 band filters + 6 gain sets per *wide* voice; point sources pay nothing. With
  dual-band panning on, the sub-700 Hz bands take the amplitude norm, so the two A/Bs compose.

LOBE/MDAP + decorrelation and SPECTRAL are two different answers to the same phantom-collapse
problem — A/B/X them (the playground's blind harness has rows for both) and keep the winner.

```c
void bwa_set_decorrelation(bwa_engine* e, bool on);   // off by default; live A/B
```

Either spread mode still feeds every speaker the **same signal** at different gains — coherent
copies, which collapse to phantom images between speakers and comb-filter as the tracked listener
walks. `bwa_set_decorrelation` fixes that: a spread source's energy splits into a coherent share on
the normal path and an incoherent share routed through **per-speaker sparse velvet-noise filters**
(~30 taps over 30 ms, time-domain, no onset latency — Välimäki/Schlecht's velvet-noise
decorrelator). The split follows `spread` (a point source is untouched; at full spread the wide
part is fully decorrelated), power is conserved (incoherent energy adds), and the toggle ramps —
a click-free live A/B. This is what makes wide sources read as *extent* rather than *several
copies*, and it is the same decorrelator bank the parametric bed's diffuse stream uses.

```c
void bwa_set_near_spread(bwa_engine* e, float radius_m);   // 0 = off (default); live
```

**Near-listener widening**: a point source flying at the head physically subtends a growing solid
angle, but a point panner collapses it into the nearest speaker and snaps it across the head as it
passes. With a radius set, every source's spread is **floored at `1 − dist/radius`** — untouched
beyond the radius, fully wide at the head. `radius_m ≈ 1.0` is a good start. The widening rides the
selected spread mode and (when enabled) the decorrelators, and the changed gains ramp like any
solve. Engine-wide policy; it takes effect with each gain re-solve (continuous under tracking).

## Channel test / diagnostics (control thread)

```c
typedef enum { BWA_TEST_OFF = 0, BWA_TEST_SINE = 1, BWA_TEST_NOISE = 2 } bwa_test_kind;
void     bwa_set_test_signal(bwa_engine* e, uint32_t channel, bwa_test_kind kind, float gain);
uint32_t bwa_get_bus_levels(bwa_engine* e, float* peaks, uint32_t cap);  // last block's per-channel output peak
uint32_t bwa_get_active_voices(bwa_engine* e);                           // last block's active voice count
```

`bwa_get_active_voices` is the voice-pool gauge next to the meters: the audio thread publishes each
block's active count (playing, sound bound — paused voices count, they haven't ended). Poll it for
HUDs or health monitoring; it reads 0 until audio runs.

`bwa_set_test_signal` drives a single **output channel** with a built-in signal (660 Hz sine or white
noise), injected **after** the per-speaker align stage — a raw value straight on the channel.
`channel` is in `[0, bwa_get_channel_count())`; anything else is ignored.

This is a speaker-check / wiring / calibration tool: walk a tone across every channel to confirm
the channel→speaker map, find a dead speaker, set a trim. It is **not** a spatial path — it
bypasses the panner, so don't use it to "place" a sound. Per-frame-safe, takes effect next block,
no `bwa_commit` needed. Any number of channels at once; `gain 0` / `BWA_TEST_OFF` silences one.
Works in every profile (cave/both: a raw tone on that DVS channel; binaural: that bus channel
HRTF'd as its virtual speaker). Needs no SDK.

`bwa_get_bus_levels` is the matching **readback**: each output channel's last-block peak `|sample|`
(linear), measured at the very end of the render — after align, the test signal, and the limiter.
That is exactly what the device channel received. It fills up to `cap` floats and returns the
count filled (`bwa_get_channel_count()` when `cap` allows). Per-frame-safe (relaxed atomic reads, no
locks); levels read 0 until audio is
running. Drive channel meters or a speaker-activity display with it — the playground lights each
speaker gizmo from this.

## Panner & layout query (control thread)

```c
typedef enum { BWA_PAN_DBAP = 0, BWA_PAN_SPCAP = 1, BWA_PAN_VBAP = 2 } bwa_panner;
typedef enum { BWA_DECODE_SAMPLING = 0, BWA_DECODE_ALLRAD = 1 } bwa_bed_decoder;   // bwa_desc.bed_decoder
void     bwa_set_panner(bwa_engine* e, bwa_panner panner);            // load-time or live (atomic switch)
void     bwa_set_dual_band(bwa_engine* e, bool on);                // live A/B; wraps the selected panner
uint32_t bwa_get_speakers(bwa_engine* e, float* xyz, uint32_t cap); // read back the layout; NULL xyz = count only
```

`bwa_set_panner` chooses the per-source panner behind the master bus:

- **DBAP** (default) is listener-relative and recomputed per block from the tracked pose.
  This is the panner for a **moving** observer roaming the array.
- **SPCAP** is a smooth, all-speaker, placement-correcting sweet-spot panner for a **fixed**
  observer: don't track, set the sweet spot once. It conserves loudspeaker power across an
  uneven array.
- **VBAP** is the sharpest — the 2-3 speakers of the containing triangle carry a source. Also
  fixed-observer, best on a cleanly-triangulable array; it falls back to DBAP otherwise.

The switch is atomic, so flipping it live is safe — the layout tool's `B` key A/Bs panners
exactly this way.

`bwa_set_dual_band` (off by default, live-toggleable) **wraps** the selected panner. It splits
each source at ~700 Hz, then pans the low band with **amplitude** (pressure / velocity-vector)
normalisation and the high band with the panner's usual **power** (energy-vector) normalisation —
SPAT's "VBP Dual-Band". You get sharper low-frequency localisation for a near-centred listener.
The panning *direction* is unchanged; only the low band's level/coherence differs. It is
sweet-spot dependent like VBAP, so for a roaming listener it's a by-ear / measurement call.

`bwa_desc.bed_decoder` chooses the **diffuse-bed** SH→speaker decoder. It affects the ambisonic
and reflection beds only, never the point-source panner:

- **sampling** (default): the straightforward projection decode.
- **AllRAD**: decode to a uniform virtual layout, then VBAP onto the real array. Robust on an
  irregular array, at the cost of a heavier load-time build. A pole with no real speaker within
  ~60° (a floor-less array's nadir) gets an **imaginary speaker** whose decode share is
  discarded — diffuse energy aimed into the hole is dropped rather than smeared onto the
  nearest ring.

The decoder is create-time configuration (the decode matrix is built at `bwa_create`); see
[`spatialization.md`](./spatialization.md) for the theory.

`bwa_get_speakers` returns the effective layout (the default grid, or the `layout_path` file) as
`cap*3` floats in channel order, plus the count — the same count `bwa_get_channel_count()` reports.
Use it to visualize or audition the geometry the engine is actually panning with.

### Channel count

```c
uint32_t bwa_get_channel_count(bwa_engine* e);   // the ACTIVE channel count (4..26), fixed at create
```

The engine's channel count **is the layout's speaker count**: a `layout_path` file with 4..26
speakers, or 26 (the default grid) with no path. `BWA_CHANNELS` (26) is only the compile-time
*capacity* — a collaborator's 24-speaker array loads a 24-entry layout into the same binary, the
device opens 24 channels, and every consumer (panners, beds, reverb, monitor, calibration) follows.
Size meter/speaker arrays from this getter, not the constant.

One sharp edge: a **failed** layout load still falls back to the 26-grid default (non-fatal, reason
in `bwa_last_error`) — which for a smaller install also means the *wrong channel count*. A
24-speaker deployment must check `bwa_last_error` (or `bwa_get_channel_count`) after `bwa_create`.

## Tracked room EQ (control thread; live)

```c
void bwa_set_tracked_room_eq(bwa_engine* e, bool on);   // default ON when the layout carries a grid
```

Layouts carrying a `room_eq_grid` (written by `bwa_calibrate --room-eq-grid`, one run per mic
placement) get **listener-tracked LF room correction**: each block the engine re-interpolates the
grid's per-speaker modal-cut depths at the live listener position (inverse-distance weights over
the measurement points) and the align-stage biquads glide toward them at 24 dB/s — click-free by
construction, fast enough to track a walking listener.

This is the moving-listener answer to the static `room_eq`, which `bwa_start` rejects for moving
sessions. It works because the room's mode *frequencies* don't move with the listener — only how
strongly each mode reads at a position — so one per-speaker fc/Q ladder plus per-position depths
interpolate safely. Mid/HF room correction stays out of the tracked path: it is position-sensitive
at the centimetre scale ([`calibration.md`](./calibration.md)).

The switch is the live kill switch (off glides every cut to flat — a clean A/B). It's a no-op for
layouts without a grid.

## Output protection limiter (control thread; ON by default)

```c
void bwa_set_limiter(bwa_engine* e, bool on);                     // live
void bwa_set_limiter_ceiling(bwa_engine* e, float ceiling_db);    // default -1 dBFS; clamped [-60, 0]
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
typedef struct { float ir_seconds; uint32_t order, num_rays, num_bounces; int enabled, bake; uint32_t reserved[3]; } bwa_reflections_desc;
void bwa_reflections_config   (bwa_engine* e, const bwa_reflections_desc* cfg);  // LOAD-TIME (before bwa_start)
void bwa_reflections_set_gain (bwa_engine* e, float linear);                   // the wet level: live, default 1
void bwa_source_set_reflections(bwa_engine* e, bwa_source s, bool on);           // per-frame; gates the wet send
void bwa_source_set_reflection_send(bwa_engine* e, bwa_source s, float gain);    // per-source wet-send level (default 1)
void bwa_source_set_reflection_distance(bwa_engine* e, bwa_source s, bool on);   // far = wetter
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
> warns once through `bwa_last_error`).

Configuration is load-time; the sends are live:

- **`bwa_reflections_config`** must land before `bwa_start` — the IR length and ambisonic order are
  baked there. Zero fields take defaults (`ir_seconds` 1.0, `order` 1, `num_rays` 4096,
  `num_bounces` 16). `enabled = 0` creates no bed; the engine behaves exactly as without one.
  `bake` non-zero precomputes the reverb over a probe grid at `bwa_start`, so the sim thread looks
  it up instead of ray-tracing live (static scenes only — which the bed requires anyway).
- **`bwa_reflections_set_gain`** is the wet level — the one control, live (a single atomic the
  audio-thread tap reads), default 1. A value set before `bwa_start` seeds whichever reverb bed it
  creates (this bed or the FDN).
- **`bwa_source_set_reflections`** opts a source into the bed's wet send. Per-frame, non-blocking;
  with the bed disabled (or no SDK) it gates a send that goes nowhere.
- **`bwa_source_set_reflection_send`** sets that source's send level (default 1.0). Drive it
  yourself for a manual dry/wet.
- **`bwa_source_set_reflection_distance`** adds automatic **distance→wet** scaling on top:
  near = drier, far = wetter.

The effective send is computed and **ramped on the audio thread** (in `rt.c`, from the
source↔listener distance), so motion and on/off toggles never zipper the send.

### Directional FDN reverb (no SDK needed)

```c
typedef struct {
    int      enabled;                          // 0 = no FDN created
    float    rt60_low_s, rt60_high_s, xover_hz;   // 0 -> defaults 1.2 / 0.7 / 2000
    float    decay_dir[3], decay_factor;       // anisotropy; zero dir or factor 0/1 = uniform
    uint32_t reserved[3];
} bwa_fdn_desc;
void bwa_fdn_config(bwa_engine* e, const bwa_fdn_desc* cfg);                             // LOAD-TIME
```

A **phonon-free** late-reverb alternative that takes the reverb tap *instead of* the Steam bed
(one reverb bed at a time; with the FDN enabled the Steam bed is skipped). It consumes the same
mono aux send, so `bwa_source_set_reflections` and the per-source send levels apply unchanged, and
`bwa_reflections_set_gain` sets its return level live.

Inside: a 16-line **feedback delay network** (Householder feedback — lossless prototype, the decay
filters are the only loss), each line assigned a direction on the sphere and rendered as a plane
wave through the layout's SH→speaker bed decode. `rt60_low_s`/`rt60_high_s`/`xover_hz` set a
two-band decay (defaults 1.2 s low / 0.7 s high @ 2 kHz). `decay_dir` + `decay_factor` make the
decay **anisotropic** — the field dies faster (factor < 1) or slower toward that direction, the
diagonal special case of the Directional FDN (Alary/Politis/Schlecht, JAES 2019). Use it to
*design* a space (an open side, a treated wall); do **not** match the real room's RT60 (see
[calibration.md](./calibration.md)).

Deterministic CPU (no rays, no IRs, infinite tail), works in no-SDK builds — the reverb path no
longer requires the Steam Audio SDK.

### Image-source early reflections (no SDK needed)

```c
void bwa_scene_set_box(bwa_engine* e, float w, float h, float d, const bwa_material faces[6]);  // the room
void bwa_source_set_early_reflections(bwa_engine* e, bwa_source s, bool on);   // per source; per-frame-safe
void bwa_early_reflections_set_gain(bwa_engine* e, float linear);            // live; default 1
```

The other half of the phonon-free acoustics path. The FDN renders the late diffuse tail; this
renders the **six first-order specular reflections** — the wall bounces that actually carry room
size and source distance. `bwa_scene_set_box` now captures the shoebox **whether or not** the Steam
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

`bwa_sound`, `bwa_source`, and `bwa_bed` are opaque `uint32_t` = `(index | generation<<16)`. A stale
handle (slot destroyed, then reused) fails the generation check on the audio side and is
silently dropped rather than crashing. `0` is always invalid. Treat handles as tokens — never
do arithmetic on them.

## Planned extension point

This section used to sketch a `bwa_source_create_stream` for engine-generated audio. It exists now —
see [Procedural (push) sources](#procedural-push-sources). It landed exactly as sketched: a handle
the caller pushes PCM into via a per-source ring, same control model, second feeding path — the
source abstraction never assumed "backed by a file," so it slotted in without churn.
