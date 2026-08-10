# bw_audio: usage guide and C ABI reference

From a consumer's side this is a **control-only** API: no audio buffers, no
device, no queue, no threads. You get an opaque engine handle, sounds, positioned
sources, and per-frame updates. Declarations in
[`include/bw_audio.h`](../include/bw_audio.h) carry their contracts as comments;
[`examples/minimal.c`](../examples/minimal.c) runs the whole client lifecycle;
[`examples/ambisonic.c`](../examples/ambisonic.c) walks the bed API (AmbiX/FuMa loading,
rotation/tilt, the renderer and max-rE A/Bs) and [`examples/streaming.c`](../examples/streaming.c)
walks disk streaming + push sources. All three are console programs built every build.

Terms used here without definition are in [glossary.md](./glossary.md).

## Feature overview

- Listener-relative spatialization over the speaker array (26 speakers on the
  CAVE; any 4..26 layout works), recomputed per audio block from the tracked head
  position: DBAP for a moving listener (the default), SPCAP/VBAP for a fixed one,
  an optional dual-band mode, per-source angular spread. SPCAP's lobe width defaults
  to what the array's own speaker spacing implies. It is a live knob
  (`bwa_set_spcap_focus`) you can dial by ear.
- Per-speaker gain/delay/correction-EQ output stage driven by a measured layout
  file, with a linked protection limiter as the final stage. Its LF room correction and,
  opt-in, its whole time alignment can follow the tracked listener instead of one fixed
  point (`bwa_set_tracked_room_eq`, `bwa_set_tracked_align`).
- Acoustics, **any build**: image-source early reflections (each wall bounce panned
  as a point source, so it has parallax as the listener walks; a shoebox or a bare
  ground plane, with pressure-release faces for water surfaces), a directional FDN
  reverb tail with live decay retune, manual occlusion with per-band transmission EQ,
  weighted-dipole source directivity.
- Acoustics, **Steam Audio builds**: ray-traced occlusion, a reflection bed
  (real-time or baked), sound pathing around occluders, and the HRTF binaural decode
  (without the SDK the headphone profiles fall back to a simple pan). Which
  reverb/reflection path to run is a real choice; see
  [materials.md](./materials.md) → "Choosing an acoustics path".
- Propagation (any build): Doppler, air absorption, equal-loudness compensation,
  near-field proximity, pitch; opt-in per source. The speed of sound itself is a
  live engine-wide parameter (underwater, slow motion).
- Assets: WAV/FLAC/MP3 decoded (and resampled) at load, disk streaming for long
  files, AmbiX ambisonic beds decoded world-locked to the array.
- Voices: fixed pool with priority stealing, pause and click-free seek,
  sample-accurate scheduled starts against a device-anchored DSP clock. The device's own
  block stamps bridge that clock to wall time for AV sync, with the device-vs-host drift
  fitted in ppm for shows long enough to care.
- Tracking: OptiTrack/NatNet ingested in-process; the audio thread samples the
  freshest head pose at block time.
- Diagnostics: per-channel test tone, output-level and listener-pose readbacks,
  offline panner and bed-decode evaluation for layout tools
  (`bwa_panner_gains_batch`, which also takes SPCAP's focus and density knobs so a
  layout can be graded at the tuning it ships with, and `bwa_bed_gains_batch`).

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
  surveyed `cave_layout.json`. A named layout that fails to load fails `bwa_start` with
  `BWA_ERR_LAYOUT` (a wrong-channel-count session can't start silently); `NULL` opts into
  the default 26-grid deliberately.
- **`bwa_start` never demands hardware**: with no usable ASIO device the engine
  keeps rendering into a silent offline sink. `bwa_get_audio_backend` reports which
  backend you actually got (see [Errors](#errors-and-return-codes)).
- **Completion is an event, and also a poll**: `bwa_poll_ended` drains the handles
  whose voices finished since you last asked. Prefer it. `bwa_source_is_playing` is
  still there and still correct, but it is a per-block readback. A sound shorter
  than your frame interval can begin and end without you ever seeing it play.

## Profiles and the master bus

Every voice is panned into an in-memory **master bus**, one channel per speaker
(26 on the CAVE array; see [Channel count](#channel-count)). The profile selects
who consumes it (and, for `BWA_PROFILE_BINAURAL`, changes how point sources
render):

| profile                 | what renders |
|-------------------------|--------------|
| `BWA_PROFILE_CAVE`      | bus → ASIO → Dante (production). Listener **position** only - real speakers, real ears. |
| `BWA_PROFILE_BINAURAL`  | the first-class headphone render → any 2-ch ASIO device. Point sources (and their ISM reflections) skip the speaker panner: with the SDK each point voice gets its **own true HRTF convolution**, without it each SH-encodes at its **true** listener-relative direction into a shared field. Beds pass SH→SH, pathing sums in directly; only the FDN/reflection-bed tails ride the bus as virtual speakers. None of the array's phantom-source spread. Full **pose**. Render details: [spatialization.md](./spatialization.md#headphone-renders-direct-binaural-and-the-array-sim). |
| `BWA_PROFILE_CAVE_SIM`  | bus → HRTF monitor → any 2-ch ASIO device (array auditioning). Each bus channel is a virtual speaker at its surveyed room position, DBAP artifacts included; full **pose** - head orientation turns the virtual array around you. |
| `BWA_PROFILE_CAVE_BOTH` | array to Dante + the `CAVE_SIM` monitor tap, concurrently. |

Pick by question, not habit: *"what will the room do?"* is `CAVE_SIM`; it hears
the **same mix** production plays, panner and all. *"Best possible headphone
rendering of this scene"* (demos, remote listening, development without the
array) is `BINAURAL`. See [architecture.md](./architecture.md).

## The threading contract

- **One control thread.** All `bwa_*` calls come from a single thread. For Unity/
  Unreal that is naturally the main thread (`LateUpdate`/`Tick`), so single-producer
  holds for free. If you call from job threads, funnel them through one thread.
- **Non-blocking per-frame calls.** Every source/listener update enqueues a
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

Room space is **right-handed, meters, +y up, +z forward, origin on the floor** at
the working-area center. [integration.md](./integration.md) →
"Coordinate seam" owns the frame: Motive's default streamed frame, the per-engine
conversions, and the handedness rationale. OptiTrack poses pass through unchanged, and
an identity orientation faces +z with the right ear at −x; derive basis vectors
from `BWA_ROOM_AHEAD` / `BWA_ROOM_UP` / `BWA_ROOM_RIGHT` in the header rather than
re-hardcoding them. The engine's world-locked decodes and its default listener
position use the **array centroid** (the nominal listening point), not the origin.
Gains are linear (1 = unity); sound offsets are engine-rate sample frames.

**Where units live in a name.** One quantity in this ABI has two live units: time is genuinely
both frames (the dsp clock, `play_at`, `stop_at`, `seek`, the playheads, the output latency,
`bwa_sound_get_frames`) and seconds (fades, RT60, IR length, the pose lead). So every time-valued
name says which: a getter ends `_frames`, a parameter is named `seconds` or ends `_s`. Nothing
else needs a suffix, because nothing else has a competitor: distances are meters, frequencies Hz,
angles radians, gains linear. Those carry the unit on the *value* where it helps
(`radius_m`, `xover_hz`, `yaw_rad`, `host_time_ns`) and never on the call.

The one rule that constrains new calls: a **decibel** value must say `_db`. Linear is the unmarked
default across the whole ABI, so a dB parameter that doesn't say so is invisible: `limiter_ceiling`
changed from dB to linear in 0.4.0 and only its parameter name records it.

Bindings inherit this and add one rule of their own: never borrow a host-engine name that carries
a *different* unit. Godot's `AudioServer.get_output_latency()` is seconds and
`AudioStreamPlayer3D.seek()` takes seconds, so the Godot binding spells its frame-valued twins
`get_output_latency_frames` / `seek_frames` and offers `_seconds` beside them.

## How-to guides

Recipes for situations every client hits, composed from calls documented in the
reference below; follow the links for the full contracts. The snippets are C. The
Unity binding wraps each pattern ([integration.md](./integration.md)).

### Put a moving source in a room

The recommended reflections-and-reverb stack, no Steam Audio build required: the six
first-order wall bounces from the
[image-source model](#image-source-early-reflections-no-sdk-needed) carry the early
field, a [directional FDN](#directional-fdn-reverb-no-sdk-needed) carries the late
tail. You set up the room and the reverb once; only the motion is per-frame.

```c
// load time: geometry and reverb are baked before start
bwa_desc cfg = { .profile = BWA_PROFILE_BINAURAL };        // CAVE on the rig
bwa_engine* e = bwa_create(&cfg);

bwa_material brick  = bwa_material_preset(e, BWA_MAT_BRICK);
bwa_material carpet = bwa_material_preset(e, BWA_MAT_CARPET);
bwa_material faces[6] = { brick, brick, carpet, brick, brick, brick };  // -x,+x,floor,+y,-z,+z
bwa_scene_set_box(e, 8.f, 3.f, 10.f, faces);               // the VIRTUAL room; before start

bwa_fdn_desc fdn = { .enabled = 1, .rt60_low_s = 1.4f, .rt60_high_s = 0.8f };
bwa_fdn_config(e, &fdn);                                   // late tail; the Steam bed stays off

bwa_start(e);

// each source opts into the acoustics it needs
bwa_sound hum = bwa_load_sound(e, "drone.wav");
bwa_source s  = bwa_source_create(e);
bwa_source_set_early_reflections(e, s, true);              // wall bounces, panned with parallax
bwa_source_set_reverb(e, s, true);                         // feed the FDN's aux send
bwa_source_set_reverb_distance(e, s, true);                // far = wetter
bwa_source_set_doppler(e, s, true);                        // it is going to move
bwa_source_play(e, s, hum, true);

// per frame: move it, and the acoustics follow
bwa_source_set_pos(e, s, x, y, z);
bwa_set_listener_pose(e, hx, hy, hz, qx, qy, qz, qw);      // skip if a tracker is connected
bwa_commit(e);
```

- **The box is the *virtual* room**, never the physical CAVE: the real room adds its own
  reflections, and modeling it double-counts ([materials.md](./materials.md)). Safe to call
  live: a room change re-solves the reflections next block.
- **Materials are audible in any build.** Each bounce is damped per band by its wall's
  absorption: the carpet floor kills the floor slap, the brick walls keep their treble.
- **Only the position updates per frame.** Reflection delays glide and gains ramp, so bounces
  bend instead of stepping, and the FDN send follows distance on the audio thread.
- **Leave `bwa_reflections_config` alone here.** The Steam reflection bed also renders early
  reflections; running it with the ISM plays them twice (the engine warns once through
  `bwa_last_error`).
- **The Steam Audio build adds occlusion on top**: the same box feeds the ray-traced scene, so
  `bwa_source_set_occlusion(e, s, true)` makes the walls block and muffle. Without the SDK,
  drive [`bwa_source_set_occlusion_manual`](#manual-occlusion-no-sdk-needed) from game logic.
- **Wet levels**: `bwa_set_reverb_gain` and `bwa_set_early_reflections_gain` are live, default
  1. Opt in the few sources that matter: each opted-in voice costs six panner solves per block.

### Land a sound on a visual event

Your renderer decides *now* that something will be seen at wall time T (an animation
lands, a metronome flashes), and the sound must be *heard* at T. Wall time and the
dsp-sample clock are different clocks, so the recipe is: map T to a dsp sample using the
device's own block stamps (`bwa_get_clock`), subtract the device's render→DAC latency
(`bwa_get_output_latency_frames`), and schedule with `bwa_source_play_at`
(see [Sources](#sources-control-thread-non-blocking)). Here in a raylib client;
`GetTime()` is the app clock, and any monotonic seconds clock works as long as you use
the same one throughout.

```c
/* The wall<->dsp bridge. The driver-stamped (sample, host time) pair is exact, so the
 * only thing to estimate is the constant epoch offset between the device's host clock
 * and GetTime(). Each frame observes (offset - pair age); a decaying max converges on
 * the true offset and tracks ppm drift. Same estimator as the Unity binding's DspTimeAt. */
static struct { bool valid; uint64_t sample; double host, off; } clk;

static void clock_refresh(bwa_engine* e) {
    uint64_t cs, ct;
    if (!bwa_get_clock(e, &cs, &ct)) { clk.valid = false; return; }
    double host = ct * 1e-9, cand = host - GetTime();
    if (!clk.valid || cs < clk.sample || fabs(cand - clk.off) > 0.5)
        clk.off = cand;                          // first pair / device restart / epoch change
    else
        clk.off = fmax(cand, clk.off - 2e-6);    // decaying max
    clk.sample = cs; clk.host = host; clk.valid = true;
}

static uint64_t dsp_at(bwa_engine* e, double t) {   // GetTime() seconds -> dsp sample
    double fs = (double)bwa_get_sample_rate(e);
    double d = clk.valid ? (double)clk.sample + (t + clk.off - clk.host) * fs
                         : (double)bwa_get_dsp_time(e) + (t - GetTime()) * fs;  // pre-stamp fallback
    return d > 0. ? (uint64_t)d : 0;
}
```

The game loop schedules against it:

```c
const double display_s = 0.030;      // draw -> photons: measure once, one constant
double anim_start = -1.;

while (!WindowShouldClose()) {
    clock_refresh(e);                             // once per frame, before any dsp_at

    if (IsKeyPressed(KEY_SPACE)) {                // swing starts now, impact lands in 0.5 s
        double t_seen  = GetTime() + 0.5 + display_s;   // when the impact frame is SEEN
        uint64_t heard = dsp_at(e, t_seen);
        uint32_t lat   = bwa_get_output_latency_frames(e);     // render -> DAC, frames
        bwa_source_play_at(e, s, thump, false, heard > lat ? heard - lat : 0);
        anim_start = GetTime();
    }

    bwa_commit(e);

    BeginDrawing();
    // draw the swing; the impact frame renders at anim_start + 0.5
    EndDrawing();
}
```

- **Schedule with margin.** The play command lands on the audio thread at the next block;
  give the start a few blocks of lead (the 0.5 s swing has plenty). A start already in
  the past plays immediately, so the failure mode is graceful. But a *spontaneous* event
  ("the collision is this frame") can never beat the physical output chain: play it
  immediately and accept up to one output latency of error.
- **`display_s` is yours to measure.** The engine reports its own output chain
  (`bwa_get_output_latency_frames`; the Digiface includes its Dante buffering) but cannot see your
  display's. Measure draw→photons once (photodiode, or an AV-sync clapper against the
  array) and that one constant aligns the whole chain.
- **The fallback is often enough.** Before the first stamped block (and always on the
  manual sink) `dsp_at` degrades to pairing `bwa_get_dsp_time` with your clock:
  block-granular, about 5 ms at 256/48 kHz, already under half a 60 Hz frame. The
  estimator buys sub-millisecond.
- **The other direction needs no wall clock.** An event on the *audio* timeline (a cue
  inside a track you scheduled) fires its visual when `bwa_get_dsp_time` crosses
  `start + cue`, or off `bwa_source_get_playhead_frames`.
- **It holds for a two-hour show.** `clock_refresh` runs every frame, so crystal drift
  never accumulates; see "Long shows drift" under
  [Sources](#sources-control-thread-non-blocking).
- **Unity**: `emitter.PlayAt(engine.DspTimeAt(tEvent))` is this whole recipe
  ([integration.md](./integration.md)).

### Develop at the desk, run on the rig

The profile is the only seam. For rig verification use `BWA_PROFILE_CAVE_SIM`: it
renders the **same 26-channel mix** production plays, through virtual speakers at
the surveyed room positions
([Profiles and the master bus](#profiles-and-the-master-bus)). Panning, acoustics,
and gain-staging bugs show up on headphones before the rig exists.
`BWA_PROFILE_BINAURAL` is the other headphone profile: the best-quality direct
render, for when headphones are the product rather than the probe.

```c
// The rig's endpoint is an RME Digiface Dante. Its ASIO driver registers under RME's own
// name, which is not the product name - enumerate with bwa_get_asio_driver_count/_name (or
// bwa_calibrate --list-drivers) rather than hardcoding a guess. NULL auto-picks the first
// driver with enough channels for the profile, which is usually what you want anyway.
bwa_desc cfg = { 0 };
cfg.profile     = rig ? BWA_PROFILE_CAVE : BWA_PROFILE_CAVE_SIM;   // desk: audition the array render
cfg.layout_path = rig ? "cave_layout.json" : NULL;         // desk: the default grid
cfg.asio_driver = rig ? rig_driver_name : NULL;            // NULL = auto-pick
bwa_engine* e = bwa_create(&cfg);
if (bwa_start(e) != BWA_OK) { /* a rig layout that fails to load refuses to start */ }
printf("backend: %s\n", bwa_get_audio_backend(e));         // "asio:<driver>" or "null"

if (rig) {
    bwa_tracker_desc trk = { .server = "192.168.1.42" };   // Motive drives the listener
    bwa_tracker_connect(e, &trk);
}
```

Everything else (assets, sources, the per-frame loop) is identical. The differences that
matter:

- **Pose.** At the desk you push `bwa_set_listener_pose` per frame, and orientation turns the
  virtual array around your head. On the rig the tracker overrides it, and the array render
  uses position only.
- **No device, still live.** With no usable ASIO driver the engine falls back to the silent
  null sink and keeps rendering (playing states, playheads, and clocks all advance), so CI and
  visual-only demos run unchanged. Set `cfg.sink = BWA_SINK_ASIO` where silence would hide a
  failure.
- **`BWA_PROFILE_CAVE_BOTH`** is the rig plus a monitor tap: the array over Dante and
  headphones at the operator's desk at once (the tap is the `CAVE_SIM` audition).

### Recipes: physical emulation

Composable patterns for physical effects that live in *your* code, built from calls the engine
already has. Each is a few lines on the control thread; everything ramps, nothing blocks. The
common thread: most "environment" effects are really per **source-listener pair** effects; apply
them to each source whose path crosses the boundary, not as a master-bus wash.

#### The listener submerges

Physics at an air-to-water boundary: ~30 dB of broadband transmission loss, water passes almost no
treble, and localization collapses (only near-vertical rays penetrate; underwater interaural delays
shrink 4.3x). Each maps to a knob. A bubbling vent next to the submerged listener needs *none* of
this (its path never crosses the surface), which is why it's per source:

```c
/* every source on the OTHER side of the surface, on submerge/emerge: */
void cross_surface(bwa_engine* e, bwa_source s, bool crossed) {
    static const float water[3] = { 0.30f, 0.06f, 0.01f };  /* low/mid/high transmission */
    bwa_source_set_occlusion_manual(e, s, crossed ? 0.03f : 1.0f, crossed ? water : NULL);
    bwa_source_set_spread(e, s, crossed ? 0.8f : 0.0f);     /* direction goes diffuse */
}

/* the room-wide half, once per transition: */
bwa_fdn_set_decay(e, 3.0f, 0.3f, 800.0f);    /* long LF tail, dead HF - hard walls under water */
bwa_set_reverb_gain(e, 1.5f);
bwa_set_speed_of_sound(e, 1480.0f);          /* Doppler + reflection timing follow the medium */
```

Sources submerged *with* the listener keep playing untouched: the FDN and the speed of sound
already carry the medium. Resist the classic game pitch-drop: frequency is invariant across a
medium boundary, so it's an aesthetic, not physics (`bwa_source_set_pitch` if you want it anyway).
Hear the whole recipe live in the playground's **Underwater** scene (TAB to it; SPACE dives).

#### The water surface overhead (Lloyd's mirror)

A surface seen from below reflects inverted (pressure-release, R ≈ -1): the flipped image cancels
the direct sound near the boundary, which is why near-surface sources sound thin. Two assemblies,
both live-safe (a mid-scene call re-solves the reflections next block, such as the submerge
transition):

```c
/* a bounded underwater room: the ceiling (+y) is the surface */
bwa_material faces[6] = { rock, rock, sand, 0 /*surface*/, rock, rock };
bwa_scene_set_box(e, 10.0f, 4.0f, 10.0f, faces);
bwa_scene_set_pressure_release(e, 1u << 3);            /* +y reflects inverted */

/* open water: no walls, just the surface plane at y = 2 m, listener below it */
bwa_scene_set_ground(e, 2.0f, 0, true);
```

Then `bwa_source_set_early_reflections(e, s, true)` on the sources near the surface: the comb
falls out of the image-source render. (The same `bwa_scene_set_ground` with `false` is the plain
outdoor recipe: the ground bounce, the one early reflection an open scene has.)

#### A doorway to the next room

The aperture *is* a secondary source. Park a proxy at the door frame, play the far room's content
on it, and the listener-relative panner does the walking-past for you:

```c
bwa_source door = bwa_source_create(e);
bwa_source_set_pos(e, door, 2.0f, 1.2f, -3.0f);        /* the frame's center */
bwa_source_set_size(e, door, 0.5f);                    /* the opening's radius: correct nearness */
static const float leak[3] = { 1.0f, 0.6f, 0.3f };     /* an opening, not a wall: mild HF loss */
bwa_source_set_occlusion_manual(e, door, 0.5f, leak);
bwa_source_set_reverb_send(e, door, 1.5f);             /* it carries the far room's wet */
```

With a Steam scene, `bwa_source_set_pathing` automates this (the sim finds the aperture); the
proxy is the no-SDK version, and also the *authored* version when you want control.

#### Wind

Upwind sources read quieter and duller. The engine has no weather model; your game does, so
drive the same manual-occlusion knob from it per frame:

```c
/* updot in [-1, 1]: how upwind the source is; strength in [0, 1] */
float loss = 1.0f - 0.4f * fmaxf(0.0f, updot) * strength;
float tilt[3] = { 1.0f, 1.0f - 0.3f * updot * strength, 1.0f - 0.6f * updot * strength };
bwa_source_set_occlusion_manual(e, s, loss, tilt);
```

#### Slow motion

```c
bwa_set_speed_of_sound(e, 70.0f);      /* delays and Doppler stretch 5x: bullet-time acoustics */
bwa_source_set_pitch(e, s, 0.5f);      /* content rate is a separate, aesthetic decision */
```

Delays saturate at each voice's ring capacity (~40 ms at 48 kHz), so extreme factors cap; the
pitch bends still track motion below that.

#### At arm's length

Not a boundary effect, but the same idea: finish the physics the panner starts. A close source
should widen *and* gain body:

```c
bwa_set_near_spread(e, 1.0f);            /* geometric: sources widen inside 1 m (engine-wide) */
bwa_source_set_proximity(e, s, true);    /* spectral: LF rises inside 1 m (per source) */
```

## Lifecycle

```c
bwa_engine* bwa_create(const bwa_desc* cfg);
bwa_result  bwa_start(bwa_engine* e);   // opens device(s), starts audio thread; BWA_OK = 0
bwa_result  bwa_stop(bwa_engine* e);
void        bwa_destroy(bwa_engine* e);
const char* bwa_last_error(bwa_engine* e);
uint32_t    bwa_get_version(void);            // the DLL's packed BWA_VERSION (major<<16|minor<<8|patch)
uint32_t    bwa_get_sample_rate(bwa_engine* e);  // resolved config, valid from create on -
uint32_t    bwa_get_block_size (bwa_engine* e);  //   derive seconds from THESE, not from your desc
bwa_sink_type bwa_get_sink_type(bwa_engine* e);  // the sink actually running (AUTO resolved at start)
```

`bwa_get_version` lets a client verify the DLL matches the header it compiled against (the desc
structs grow via reserved fields, but enum values and struct layouts are only guaranteed within a
major.minor). The resolved-config getters exist because every zero desc field means "default":
`seconds = frames / bwa_get_sample_rate(e)` is always right, even when the desc said 0.
`bwa_get_sink_type` is the machine-readable side of `bwa_get_audio_backend`: after `bwa_start`,
`BWA_SINK_AUTO` has resolved to `ASIO` or `NULL`.

Every pointer argument across the API is **consumed before the call returns**: no call retains
caller memory (`bwa_create` copies its path strings; descs, geometry, and position arrays are
copied at call time).

Zero-init `bwa_desc` and set what you need; every field's zero is its default:

| field            | meaning                                                              |
|------------------|---------------------------------------------------------------------|
| `profile`        | `cave` / `binaural` / `cave_sim` / `cave_both` (see [Profiles](#profiles-and-the-master-bus)) |
| `layout_path`    | surveyed speaker geometry (JSON); cave/cave_both. NULL = the default 26-grid deliberately; a named file that fails to load fails `bwa_start` (`BWA_ERR_LAYOUT`) |
| `hrtf_path`      | HRTF (SOFA) or NULL for built-in; the headphone profiles             |
| `sample_rate`    | Hz; 0 = 48000, the **validated** rate. The DSP is rate-derived and 96 kHz renders correctly in software, but rates above 48 k are unverified against the real Digiface/Dante chain - treat 48 kHz as supported until the rig confirms more |
| `block_size`     | render quantum, frames; 0 = 256. Also the ASIO buffer-size *hint* - a driver may run its own size (the sinks adapt); `bwa_get_block_size` reads back the resolved quantum |
| `sink`           | output-device policy: `BWA_SINK_AUTO` (0, default - try ASIO, fall back to the silent null sink), `BWA_SINK_ASIO` (demand a device: an open failure fails `bwa_start` loudly), `BWA_SINK_NULL` (force the offline sink - CI, profiling, tracking-only), `BWA_SINK_MANUAL` (no device/thread - pump blocks yourself with `bwa_render_block`; deterministic, for golden tests) |
| `asio_driver`    | ASIO driver name to open; NULL = auto-pick the first registered driver with enough output channels for the profile (the headphone profiles find a 2-ch driver, cave a ≥layout-count one) |
| `embree`         | ray-trace the acoustics sims on Intel Embree; silently falls back to the default tracer if the phonon build lacks it - see [Ray-tracing acceleration](#ray-tracing-acceleration-bwa_descembree) |
| `enable_pathing` | run the sound-pathing sim from `bwa_start` (needs scene geometry + the Steam Audio build); sources opt in via `bwa_source_set_pathing` |
| `bed_decoder`    | diffuse-bed SH→speaker decoder: 0 is the engine default (reserved, currently AllRAD), AllRAD (1) or EPAD (2) - see [Panner and layout query](#panner-and-layout-query-control-thread) |
| `reserved[4]`    | zero; room to grow without an ABI break                             |

## Errors and return codes

The API reports failure three ways, all read on the **control thread**:

- **Pointer/handle returns.** `bwa_create` returns `NULL` on failure; `bwa_load_sound` /
  `bwa_source_create` return `0`.
- **`bwa_result` returns.** `bwa_create`'s companions `bwa_start`/`bwa_stop` and
  `bwa_tracker_connect` return `BWA_OK` (0) on success, a nonzero code otherwise: the
  `bwa_result` enum in [`include/bw_audio.h`](../include/bw_audio.h), listed below.
- **`bwa_last_error`.** A human-readable string for the most recent failure on that engine, or
  `NULL` if none. Lifetime: the engine clears it at **entry** to the lifecycle/load-class calls (create,
  start, the loads, the source creates, tracker_connect), so a successful one leaves it `NULL`.
  A per-frame call sets it only when it REJECTS the call. Read it right after the call you are checking; a later
  successful lifecycle call wipes it. The string itself stays valid until the next `bwa_*` call
  on the same engine.

| code            | value | cause |
|-----------------|-------|-------|
| `BWA_OK`         | 0     | success |
| `BWA_ERR_CONFIG` | 1     | invalid `bwa_desc` (bad profile, sample_rate, block_size) |
| `BWA_ERR_DEVICE` | 2     | ASIO/output device could not be opened, lacked enough output channels for the layout, or failed to start |
| `BWA_ERR_LAYOUT` | 3     | `bwa_start`: an explicitly-passed `layout_path` failed to load at create (missing/unparseable/failed validation - see [`layout-schema.md`](./layout-schema.md)) |
| `BWA_ERR_HRTF`   | 4     | reserved; not currently returned. HRTF (SOFA) load failures are non-fatal - the monitor degrades and the reason lands in `bwa_last_error` |
| `BWA_ERR_STATE`  | 5     | reserved; not currently returned. Wrong-state calls report through `bwa_last_error` instead |
| `BWA_ERR_INTERNAL` | 6   | reserved; not currently returned |
| `BWA_ERR_TRACKER` | 7    | `bwa_tracker_connect` failed (socket open, name didn't resolve, room_eq layout) |

What actually comes back today:

- `bwa_create` returns `NULL` (bad config / out of memory). No code.
- `bwa_start` returns `BWA_OK`, `BWA_ERR_CONFIG` (NULL engine, or a `room_eq` layout in a
  moving-listener session), `BWA_ERR_LAYOUT` (below), or `BWA_ERR_DEVICE`. A redundant
  `bwa_start` is a no-op returning `BWA_OK`.
- `bwa_tracker_connect` returns `BWA_OK`, `BWA_ERR_CONFIG` (NULL args / a static-`room_eq`
  layout), or `BWA_ERR_TRACKER`.
- A failed **explicit** `layout_path` leaves `bwa_create` usable (the engine sits on the default
  26-grid; the reason is in `bwa_last_error`, readable right after create) but **fails
  `bwa_start` with `BWA_ERR_LAYOUT`**: a session that named a layout never silently runs 26
  channels of the wrong geometry. `layout_path = NULL` means the default grid deliberately.
- A bad `hrtf_path` does **not** fail `bwa_start`. The monitor degrades to the simple pan and
  records why in `bwa_last_error`; if your session depends on a SOFA HRTF, read
  `bwa_last_error` after a *successful* `bwa_start` to confirm it loaded.

Per-frame calls that **reject** the call do set `bwa_last_error` (`bwa_source_push` on a plain
source, `bwa_source_queue`, `bwa_play_oneshot` on a dead handle, `bwa_fdn_set_decay`,
`bwa_material_release`, `bwa_apply_tuning`); a per-frame call that merely enqueues never does.

Per-frame `void` calls that only enqueue (`bwa_source_set_pos`, `bwa_commit`, …) report nothing. They
enqueue onto the command ring and return. A full ring drops the command silently, but the ring is sized
(`RING_CAP`) for a worst-case frame burst, and only a stalled control thread could fill it
(see [`concurrency.md`](./concurrency.md)). Position and pose are latest-wins, so a dropped
update is corrected one frame later.

## Environment variables

There are none. You configure everything through the API: the output device is
`bwa_desc.sink`/`asio_driver`, Embree is `bwa_desc.embree`, pathing is `bwa_desc.enable_pathing`,
reflection baking is `bwa_reflections_desc.bake`, and the OptiTrack connection is
`bwa_tracker_connect` (all of these used to be `BWA_*` env vars). One door per knob.

### Ray-tracing acceleration (`bwa_desc.embree`)

The occlusion and reflection sims ray-trace on the Steam Audio scene. Set **`embree = true`**
to run them on **Intel Embree** instead of Steam Audio's built-in
ray tracer: faster, and the lever to pull if you raise scene complexity, ray counts, or bake at
high probe density. It is **opt-in and safe**: if the linked `phonon` was not built with Embree
(or the Embree/TBB runtime is missing), the engine logs that Embree is unavailable and falls back
to the default tracer (no failure). The engine creates the scene once and both sims share it, so the flag
applies to occlusion and reflections together. The vendored prebuilt `phonon.dll` is **not**
Embree-enabled, so the flag currently falls back; to activate it, drop in a `phonon` built with
Embree (the SDK's `STEAMAUDIO_ENABLE_EMBREE` path) and ship `embree4.dll` + `tbb*.dll` alongside.

## ASIO device query (control thread, no engine needed)

```c
uint32_t bwa_get_asio_driver_count(void);
bool     bwa_get_asio_driver_name(uint32_t index, char* buf, uint32_t cap);  // NUL-terminated; false = out of range
```

Enumerate the OS's **registered** ASIO drivers: call before `bwa_create` to populate a device
picker, then pass the chosen name as `bwa_desc.asio_driver`. Names are UTF-8 (converted from the
registry's ANSI codepage and back, so a localized name round-trips). Nothing is loaded or opened
(it reads the registry fresh on every call), so it's safe alongside a running engine. A
registered driver isn't necessarily *openable* (hardware unplugged, exclusive-mode busy); the
truth test is still `bwa_start` + `bwa_get_audio_backend`. A no-ASIO build reports zero drivers.
The tools ride the same registry (`--list-drivers` on the CLIs, picker dropdowns in the GUIs).

## Assets (control thread, file I/O)

```c
bwa_sound bwa_load_sound(bwa_engine* e, const char* path);           // decode fully into RAM; 0 = failure
bwa_sound bwa_load_sound_streaming(bwa_engine* e, const char* path); // stream from disk (long files); 0 = failure
void    bwa_unload_sound(bwa_engine* e, bwa_sound snd);               // safe; retire-acked internally
uint64_t bwa_sound_get_frames  (bwa_engine* e, bwa_sound snd);        // length in frames at the ENGINE rate
uint32_t bwa_sound_get_channels(bwa_engine* e, bwa_sound snd);        // 1 = mono; 4/9/16 = ambisonic bed
```

Load sounds once, at load time. **WAV, FLAC, and MP3** are accepted (decoded to mono
float by dr_libs, dispatched by file extension). If the file's sample rate differs from
the engine's, it is **resampled to the engine rate at load** (a windowed-sinc pass), so a
44.1 kHz MP3 plays correctly on a 48 kHz engine; you pay only the one-time load cost.

`bwa_load_sound_streaming` is for long assets (music, ambience) you don't want resident in RAM: a
background thread feeds the voice from disk as it plays. It is **mono, at the engine sample rate**
(a rate mismatch fails; pre-convert, or use `bwa_load_sound` which resamples), plays on **one voice
at a time**, and does not support `bwa_source_seek` (the ring can't jump).

`bwa_unload_sound` is safe to call at any time. The core detaches references on the audio thread
and frees only after the retire-ack (see [concurrency.md](./concurrency.md)); it never pulls
a buffer out from under a playing voice.

The metadata getters answer "what did I just load" (progress bars, validation, mono-versus-bed
dispatch). Frames are at the **engine** rate (in-memory assets were resampled at load, streams
must already match), so `seconds = frames / bwa_get_sample_rate(e)`. `get_frames` returns 0 for an
invalid handle or a stream whose length is unknown (push sources); `get_channels` returns 0 for
an invalid handle. Control thread, any time after the load.

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
void     bwa_source_play_loop(bwa_engine* e, bwa_source s, bwa_sound snd, uint64_t loop_beg, uint64_t loop_end); // intro->loop
void     bwa_source_stop (bwa_engine* e, bwa_source s);
void     bwa_source_stop_at(bwa_engine* e, bwa_source s, uint64_t stop_sample);      // click-free stop on the dsp clock
void     bwa_source_queue(bwa_engine* e, bwa_source s, bwa_sound snd, bool loop);    // chain: play gaplessly after the current sound
void     bwa_source_clear_queue(bwa_engine* e, bwa_source s);                        // drop the pending chain
void     bwa_source_set_paused(bwa_engine* e, bwa_source s, bool paused);   // ramped; playhead freezes
void     bwa_source_seek (bwa_engine* e, bwa_source s, uint64_t frame);     // click-free jump (in-memory)
bool     bwa_source_is_playing(bwa_engine* e, bwa_source s);  // control-thread poll; see below
uint64_t bwa_source_get_playhead_frames(bwa_engine* e, bwa_source s); // CONTENT playhead, engine-rate frames
bool     bwa_play_oneshot(bwa_engine* e, bwa_sound snd, float x, float y, float z, float gain); // false = dropped
```

**The model: a source drives at most one voice.** Play on an already-playing source restarts it
(a new sound replaces the old, un-paused at frame 0); the same `bwa_sound` can play on any number
of sources at once. And mind the near-namesakes: `bwa_source_set_pos` is the **spatial** position,
`bwa_source_get_playhead_frames` the **content** position: unrelated readouts.

**Starts are click-free.** Every `play` / `play_at` / `play_loop` ramps the per-channel gains up
from silence over the first block (~5 ms), the mirror of the one-block fade `stop` / `stop_at` use
to ramp down. So re-triggering a source whose asset doesn't begin near a zero crossing never pops.
The one hard cut left is `bwa_source_destroy` on a *still-playing* source: that's teardown, so stop
(or fade) first if the tail is audible.

**The voice pool steals, it doesn't fail.** The pool is fixed-size. When it's full,
`bwa_source_create` stops the lowest-**priority** active source to make room (255 = protected, never
stolen): an overloaded scene drops its least important sound instead of refusing the new one. Set
music and critical SFX high. The steal is click-free: the stolen voice fades out over one block on
its own slot while the new source starts immediately on a small reserve of spare slots.

**Scheduled starts are sample-accurate.** `bwa_source_play_at` begins output exactly when the
engine's dsp clock reaches `start_sample`: silent until then, then starting at the precise in-block
sample. Read "now" from `bwa_get_dsp_time` (device sample position, monotonic) and add a delay:
`bwa_get_dsp_time(e) + sample_rate/2` plays half a second out. `0` means play immediately (same as
`bwa_source_play`); a start already in the past plays immediately, best-effort.

**Loop a sub-region for intro→loop content.** `bwa_source_play_loop` plays from the start but
wraps at `loop_end` back to `loop_beg` instead of the clip end: the intro `[0, loop_beg)` plays
once, the body `[loop_beg, loop_end)` loops forever. Frames are engine-rate. `loop_end` 0 means
the clip end, so `play_loop(.., 0, 0)` is just `play(.., true)`; out-of-range bounds or
`loop_beg >= loop_end` fall back to whole-clip looping. In-memory sounds only; a streamed source
loops its whole file. The seam is a hard wrap with no crossfade, so author the loop points on
matched endpoints.

**Scheduled stops can't pop.** `bwa_source_stop_at` fires the click-free stop when the dsp clock
reaches `stop_sample`: a one-block fade (the same path `bwa_source_stop` takes), deliberately not
a hard cut at the exact sample, so silence lands within ~one block (~5 ms at 256/48k). Same time
base as `play_at`; a `stop_sample` in the past stops now; a later `play` / `play_at` /
`play_loop` clears a pending stop. Push sources don't take a scheduled stop; use
`bwa_source_stop` or `bwa_source_push_end`.

**Chaining is gapless.** `bwa_source_queue` plays a sound the instant the current one ends, with
no silence at the seam (the mixer swaps mid-block). Queue several for a sequence (A→B→C…); a
queued sound with `loop = true` is the terminal looping item, so the two-file intro→loop is
`play(intro, false)` then `queue(body, true)`. Up to seven pending (further queues drop). Queue
*after* the play: `play` / `play_at` / `play_loop` restart the source and clear the queue.
Nothing chains after a *looping* current sound (it never ends) or a *stopping* one. In-memory
mono on both ends: a bed/stream/push `snd` is rejected (`bwa_last_error`), and a queue behind a
*streamed* current sound is ignored. The playhead restarts at each chained item and the source
reads as playing throughout; `bwa_source_clear_queue` drops the pending chain. To know *which*
item is playing, watch the playhead reset across the seam; there's no separate event.

```c
// clock / scheduling - the time base for bwa_source_play_at:
uint64_t bwa_get_dsp_time(bwa_engine* e);                       // current dsp-sample clock (device-anchored, monotonic)
bool     bwa_get_clock(bwa_engine* e, uint64_t* dsp_sample, uint64_t* host_time_ns); // device (sample, host-time) pair
uint32_t bwa_get_output_latency_frames(bwa_engine* e);                 // device render->DAC latency, frames (0 = unknown)
bool     bwa_get_clock_model(bwa_engine* e, bwa_clock_model* out); // fitted device-vs-host drift (ppm + its sigma)
```

**Syncing with graphics.** Two cases. Events on the **audio timeline** (a cue in a track you
scheduled) never need wall time: keep the `start_sample` you passed to `play_at` and fire the
visual when `bwa_get_dsp_time` crosses `start + cue` (or poll `bwa_source_get_playhead_frames`).
Events that originate on the **graphics side** need the wall→dsp mapping, and that is
`bwa_get_clock`: the (output sample position, host time) pair the audio stack stamps inside each
block callback (ASIO's `ASIOGetSamplePosition` pair, synthesized from QPC on the null sink).
Because the pair is captured *in* the callback, the mapping
`dsp_at(T) = sample + (T_ns − host_time_ns) · rate / 1e9` carries none of the
block-plus-scheduling jitter that pairing `bwa_get_dsp_time` with your own clock read does.
`host_time_ns` sits on a backend-defined epoch: anchor it against your clock once and re-sample
per frame (the Unity binding's `Engine.DspTimeAt`/`RealtimeAt` do this with a decaying-max offset
estimator). It returns **false with the outputs untouched** until a host-stamped block has
rendered: before `bwa_start`, or under a driver that reports no `systemTime` (FlexASIO is one).
The **manual** sink is the deliberate exception: it stamps a *nominal* time derived from the
sample position (`sample / rate`), so a fixed call sequence renders bit-identically; treat that
pair as a sample-accurate fiction, exact for arithmetic and meaningless as wall time. Its first
block carries nominal time 0 (read as "unstamped"), so the manual clock goes valid on block 2.
Finally, the dsp clock stamps when a block is *rendered*, not heard:
`bwa_get_output_latency_frames` is the device's render→DAC delay in frames (`ASIOGetLatencies`;
the Digiface includes its Dante buffering), so sound scheduled for dsp time T reaches the room at
`T + latency`; subtract your measured display delay and one constant aligns the whole AV chain.

**Long shows drift.** The pair is exact at the instant it was stamped, but the device crystal and
the host clock are different oscillators: 10 ppm is 36 ms an hour, a spec-worst ±50 ppm part
~180 ms. Re-anchoring off `bwa_get_clock` every frame makes that vanish, because you never
integrate the error. The rule that follows: **schedule far-out events in dsp samples, not by
predicting a future wall time**. Take `t0 = bwa_get_dsp_time` once at show start and place every
cue at `t0 + cue × rate`; now there is one clock in the system and drift is impossible rather
than corrected.

`bwa_get_clock_model` is for when you can't do that: something else owns the timeline, you want a
minutes-long extrapolation to hold, or you want the drift on the rig log. It fits the *slope* by
exponentially weighted least squares over the same per-block stamps (~2 minute window, a few
dozen flops a block) and reports `ppm` with its own `ppm_sigma`; use `rate_hz` in place of the
nominal rate in `dsp_at(T)` and long extrapolations stop walking off. It returns false until the
fit has ~1 s of stamps, and reseeds (going quiet ~1 s) after a restart re-bases the sample
position. Two honest caveats: read `ppm_sigma` as a lower bound (it assumes independent stamp
noise; real jitter is correlated), and `jitter_ns` reports *stamp* quality, so a driver without
`kSystemTimeValid` reads worse from QPC dispatch noise. The manual sink fits `ppm = 0` exactly;
that is true of the fiction, not of any hardware.

Correcting drift is a different problem from measuring it, and the engine deliberately doesn't do
it. If audio must follow an external master indefinitely, the clean lever is a **push source**:
run your own resampler with `bwa_source_push_space` as the error signal. The best fix isn't in
software at all: clock the display machines from the same reference as the audio device (the
Digiface is a Dante endpoint, already disciplined by a PTP grandmaster) and the two clocks become
one.

**Completion is an event.** `bwa_poll_ended(e, out, cap, dropped_out)` drains the handles whose
voices ended since the last call, oldest first, and returns how many it wrote. It fills from the same
drain `bwa_commit` runs, so poll after commit.

```c
bwa_commit(e);
bwa_source done[32];
uint32_t n = bwa_poll_ended(e, done, 32, NULL);
for (uint32_t i = 0; i < n; ++i) on_finished(done[i]);
```

Handles come back as you knew them, so comparing against a stored handle works. A plain source's
handle stays valid and re-playing it is normal. Oneshots are never reported, because the engine never
gave you a handle for one, and a stolen voice is not reported either: the engine took the slot, the sound
did not finish. A completion superseded by a re-play of the same handle is dropped rather than
reported, so you cannot free or re-trigger the wrong play. Unpolled events are bounded and drop
oldest; pass `dropped_out` to tell "nothing finished" from "I did not poll often enough".

**Completion is also a poll, and that path is weaker.** `bwa_source_is_playing` is a latest-wins
readback (like `bwa_get_listener_pose`): the audio thread republishes each source's playing state
every block, gated on the handle's generation and on a play sequence, so a re-play on a handle whose
voice already ended reads `true` immediately rather than lying until the next block. It reads `true`
while a sound plays; `false` once a non-loop sound finishes, after `stop`, or for a stale handle.
What it cannot do is see a sound shorter than your poll interval, which no amount of sequencing
fixes. Use it for "is this still going", not for "did it finish".

**The playhead is a poll too.** `bwa_source_get_playhead_frames` rides the same per-block
republish: the voice's **content** position in engine-rate frames. It is correct exactly where
deriving a playhead from `bwa_get_dsp_time` breaks: it freezes under pause, lands where a seek
lands, follows a pitched voice at its actual rate, wraps with a loop, and for stream/push
sources counts frames actually **consumed** (an underrun slips it, exactly like what you hear).
A finished non-loop voice keeps reporting its final position; an idle voice, a still-held
scheduled play, or a stale handle reads 0. Block-granular and one block behind a just-issued
play/seek; for tighter scheduling, stay on `bwa_get_dsp_time` arithmetic.

**Pause and seek are click-free.**

- `bwa_source_set_paused` gates the voice with a one-block ramp (~5 ms) and freezes the playhead
  once silent, so resume continues exactly where pause landed. Works for in-memory, streamed, and
  bed sounds. A paused voice still reads as *playing*: it hasn't ended.
- `bwa_source_seek` jumps the content position (engine-rate frames). On a running voice: ramp out,
  jump, ramp back in (~10 ms end to end). On a paused voice the jump is immediate and it stays
  paused. Past-the-end seeks wrap for loops and end one-shots.
- Streamed sounds ignore seeks: the stream ring can't jump. `bwa_source_play` always restarts
  un-paused at frame 0.

**Fades are engine-side.** `bwa_source_fade_to` glides the gain over `seconds` on the audio thread
(no per-frame scripting; `seconds <= 0` sets immediately). A later `set_gain` or fade replaces the
fade in flight. `bwa_source_fade_out` is the one-call "fade to silence, then stop"; it lands on the
same click-free stop path `bwa_source_stop` uses.

**Mix groups (ids 0..`BWA_GROUPS`-1, that is, 0..7)** are category-level control: a group's gain
multiplies into every member's gain solve (ramped like any solve). Pausing a group ramps its
members out and freezes their playheads exactly like per-voice pause: duck the SFX and keep the
dialog, silence the ambience category for a cutscene. Sources start in group 0; group state
persists across `play`. Out of range: `set_group` falls back to group 0, group gain/pause calls
are ignored.

**Pitch** (`bwa_source_set_pitch`, 1 = native, clamped `[0.25, 4]`) resamples **in-memory** sounds
with a fractional playback cursor (linear interpolation; the cursor stays integer + fraction, so a
voice running for hours never loses precision). Rate changes **glide** across a block (a change
bends the pitch rather than stepping it) and compose with Doppler (which resamples via propagation
delay on top). Streamed sounds ignore it (the stream ring is sequential); beds are unaffected. Use
it for one-shot variation, slow-mo, engines.

Positions are in **room space** (see [Coordinates and units](#coordinates-and-units)).
`bwa_play_oneshot` is the fire-and-forget path: it allocates a transient voice internally and
recycles it on end, so the caller holds no handle. Unlike `bwa_source_create` it never
**steals**: with the voice pool (or command ring) momentarily full the oneshot is dropped, so
oneshot spam can't evict your named sources. It returns whether it was **accepted**, the only
way to tell "played" from "never loaded" from "dropped under load"; false sets `bwa_last_error`
to which of the three it was. If you need to track the voice afterwards, that is what
`bwa_source_create` is for.

```c
if (!bwa_play_oneshot(e, impact, x, y, z, 1.f))
    log("impact dropped: %s", bwa_last_error(e));
```

### Procedural (push) sources

```c
bwa_source bwa_source_create_push(bwa_engine* e);            // 0 = failure (see bwa_last_error)
uint32_t   bwa_source_push(bwa_engine* e, bwa_source s, const float* frames, uint32_t n); // frames accepted
uint32_t   bwa_source_push_space(bwa_engine* e, bwa_source s); // frames a push would accept right now
void       bwa_source_push_end(bwa_engine* e, bwa_source s);   // end-of-data: ends once drained
```

Engine-generated audio without a file: `bwa_source_create_push` returns a source whose voice
plays PCM **you push**: mono float frames at the engine rate, through a per-source ring (65536
frames, ~1.37 s at 48 kHz). It is a normal source in every other way (position, gain, spread,
occlusion, Doppler, groups, fades, pause). Use it for synthesis, network audio, or bridging
another engine's output.

Three rules cover the model:

- **The stream clock is data-driven.** The voice consumes from create: silence until your first
  push, and an **underrun** renders silence *without losing your place*: output resumes at the
  next pushed sample. It slips, it never drops. Stay a frame's worth ahead; `bwa_source_push`
  returns the count accepted (pace with `bwa_source_push_space`).
- **Push from the one control thread**: the ring is SPSC. The engine writes non-finite samples as 0
  (nothing hands NaN to the audio thread).
- **Ending is one-way.** `bwa_source_push_end` marks end-of-data: the voice ends once the ring
  drains, and further pushes are refused. A push source is not restartable: create a new one.
  `bwa_source_stop` / `bwa_source_fade_out` end it the same way (the unconsumed remainder is
  dropped); use `bwa_source_set_paused` to silence one temporarily. `bwa_source_destroy`
  releases the ring (safe while playing; retire-acked like any sound).

`bwa_source_play` / `seek` / `pitch` don't apply: a push source plays what you push (`play` is
rejected with an error). The reverse mix-up is reported too: `push` / `push_space` / `push_end`
on a live **non-push** source return 0 / do nothing **with an error**, so a wrong handle never
masquerades as ring backpressure (a stale handle stays the usual silent no-op). A full pool can
**steal** a push source like any voice; protect an important one with priority 255.

### Master gain and global pause

```c
void bwa_set_master_gain(bwa_engine* e, float linear);   // one ramped scalar over the whole mix; live
void bwa_set_paused(bwa_engine* e, bool paused);         // pause EVERYTHING (ramped, playheads freeze); live
```

`bwa_set_master_gain` is the volume knob / scene fade: it scales everything mixed (voices, beds,
the reverb/pathing taps) **before** the per-speaker align stage (so trims and the raw channel-test
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
void    bwa_bed_set_orientation(bwa_engine* e, bwa_bed b,               // full 3-axis (yaw/pitch/roll);
                              float yaw_rad, float pitch_rad, float roll_rad);   //   glided, live
void    bwa_bed_stop    (bwa_engine* e, bwa_bed b);
void    bwa_bed_destroy (bwa_engine* e, bwa_bed b);

// same voice machinery as the bwa_source_* calls of the same name - DELIBERATELY bed-named so bed
// code never mixes prefixes (semantics under "Sources"):
void    bwa_bed_fade_to     (bwa_engine* e, bwa_bed b, float gain, float seconds);
void    bwa_bed_fade_out    (bwa_engine* e, bwa_bed b, float seconds);   // fade, then click-free stop
void    bwa_bed_set_paused  (bwa_engine* e, bwa_bed b, bool paused);     // freeze/resume in place
void    bwa_bed_seek        (bwa_engine* e, bwa_bed b, uint64_t frame);
void    bwa_bed_set_priority(bwa_engine* e, bwa_bed b, int priority);    // beds share the voice pool -
void    bwa_bed_set_group   (bwa_engine* e, bwa_bed b, uint32_t group);  //   protect a music bed with 255
bool    bwa_bed_is_playing  (bwa_engine* e, bwa_bed b);
uint64_t bwa_bed_get_playhead_frames(bwa_engine* e, bwa_bed b);   // content playhead, engine-rate frames
```

`bwa_bed_set_orientation` orients the recorded field in 3 axes: yaw to line a capture up with the
scene, pitch/roll to level a capture whose "front" wasn't upright. Positive **yaw** turns the
field from room **+z** toward **+x**; positive **pitch** tilts its front upward; positive
**roll** tilts its top toward the room's right (−x); applied roll → pitch → yaw. **Yaw-only**
stays on the exact closed-form phasor path (each degree's ±m pair rotates by m·yaw; exact at
every order); any pitch/roll runs a full SH rotation matrix (the Ivanic-Ruedenberg recursion),
rebuilt per block from the glided angles. Either way it **glides** at ~one turn per second
(click-free, live-safe) and applies before *either* bed renderer, so the matrix decode and the
parametric analysis see the same turned field.

A **bed** is a pre-encoded **AmbiX** soundfield (ACN ordering, SN3D normalization) decoded
**straight to the speakers**. It is not panned and has no position; use it for diffuse,
ambient content.

Beds are **world-locked**: the soundfield is fixed to the room. The physical speakers are
world-fixed too, so the real acoustics handle a listener walking through the field,
and the headphone renders' head-tracking applies downstream.

Load with `bwa_load_ambix` (full 3D sets only: 4/9/16 channels; mono and other counts are
rejected), then drive with the `bwa_bed_*` family: no position, only a master gain. Legacy
**FuMa** B-format (`.amb` and friends: WXYZ order, MaxN normalization, the W −3 dB) loads with
`bwa_load_fuma`; the conversion happens at load, so past that call the asset is indistinguishable
from an AmbiX load of the same field. Internally a bed is a voice playing a multichannel asset,
so handles and lifetime match `bwa_source_*`: fades, pause/seek, priority, and groups are the
same per-voice machinery re-exported under the bed prefix. The priority default (128) means a bed
**can be stolen** by a full-pool `bwa_source_create`; set 255 on a bed that must survive an SFX
overload. The decode is a static SN3D SH→speaker matrix (AllRAD or EPAD per
`bwa_desc.bed_decoder`; see below), rebuilt from the layout.

```c
void bwa_set_max_re(bwa_engine* e, bool on);         // ON by default; live A/B (crossfaded)
void bwa_set_max_re_split(bwa_engine* e, bool on);   // off by default; live A/B; needs max_re on
```

`bwa_set_max_re` puts **max-rE weighting** (Zotter and Frank's psychoacoustic decoder weights) on the
engine's SH→speaker decode: it tapers the higher ambisonic orders, which suppresses the decode's
sidelobes and lengthens the energy vector. You get **better localization away from the sweet spot**,
exactly the walking-listener case, at a slightly wider main lobe. The weights are
diffuse-energy-normalized per content order, so A and B stay level-fair. It reaches every consumer
of the engine's own decode: bed matrix rendering (the sampling fallback / AllRAD / EPAD) and the FDN reverb's
line render. It doesn't reach the point-source panners (DBAP/SPCAP/VBAP pan, they don't decode) or
phonon's own decodes (reflection bed, pathing, the HRTF monitor). **ON by default** since the offline
bed sweep, which had the taper winning every axis under both decoders and both observer models. The
rig trial confirms rather than gates; if it disagrees, revert the default.

`bwa_set_max_re_split` is the **band-split** refinement (the literature-standard Gerzon
basic-LF/max-rE-HF decode): with it on, the taper acts only **above a ~700 Hz crossover**; below,
the unweighted decode stays, because the ear localizes LF by summed pressure (the velocity vector,
which the plain decode maximizes) and HF by energy. Same 700 Hz boundary as dual-band panning; a
per-bed one-pole splitter, crossfaded like the taper itself, so both toggles are click-free live
A/Bs. Bed matrix decodes only: the FDN's line render stays broadband either way (a diffuse tail
has no LF image to sharpen). Broadband versus split is a by-ear call for the rig.

```c
typedef enum { BWA_BED_MATRIX = 0, BWA_BED_PARAMETRIC = 1 } bwa_bed_renderer;
void bwa_set_bed_renderer(bwa_engine* e, bwa_bed_renderer renderer);   // live A/B (each bed crossfades)
```

Two renderers sit behind the same bed API:

- **matrix** (default): the static SH→speaker decode above (AllRAD or EPAD per
  `bwa_desc.bed_decoder`). Cheap and robust, but a 26-speaker array is sparse for a matrix
  decode (directional content blurs) and the decode is world-locked around the array center.
- **parametric** (`BWA_BED_PARAMETRIC`): first-order **DirAC-style** rendering. Per band, the
  non-diffuse stream is re-panned through the engine's own listener-relative panner, so a
  recorded soundfield becomes **walkable** (correct directions and parallax off-center); the
  diffuse stream decodes through the matrix into the per-speaker decorrelators. Both streams
  are loudness-matched to the matrix decode; beds with fewer than 4 channels stay on the
  matrix; the switch crossfades per bed, a clean live A/B. Full treatment:
  [spatialization.md](./spatialization.md#parametric-bed-rendering-bwa_set_bed_renderer-live-ab).

Note: FLAC is the natural container for lossless multichannel beds. MP3 can't carry ambisonics.

## Listener (control thread; skip when a tracker is connected)

```c
void bwa_set_listener_pose(bwa_engine* e, float px,float py,float pz,
                                       float qx,float qy,float qz,float qw);
```

Every render uses position. Orientation (the quaternion) reaches the
**headphone renders only**; the array render ignores it. With a tracker connected,
do not call this: the engine samples the freshest OptiTrack pose at block time.

### Internal tracking (`bwa_tracker_connect`)

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

Connect the engine to a NatNet (Motive) stream and it ingests the pose itself. It samples the
freshest head pose on the **audio thread at block time** (lower latency than pushing pose
through the command ring), and it overrides any `bwa_set_listener_pose`. This is a runtime call, like
every other NatNet client: connect before or after `bwa_start`, reconnect with new settings
(the replacement is glitch-free: the audio thread swaps pose sources between blocks), or
disconnect to fall back to the committed/pushed pose. It is lifecycle-class (opens a socket, may
block briefly), not a per-frame call. Zero fields take the defaults shown.

The engine resolves a rigid-body **name** to its streaming ID at connect through the model
definitions (a `NAT_REQUEST_MODELDEF` exchange); this needs `server` and NatNet ≥ 4. A name that doesn't
resolve fails the connect with `BWA_ERR_TRACKER` and the reason in `bwa_last_error`. A failed
connect leaves the engine running on the committed/default listener: nothing tears down.
The tracker's lifetime is independent of `bwa_start`/`bwa_stop` (it survives a device restart);
`bwa_destroy` disconnects it. One invariant carries over from `bwa_start`: a layout carrying
**static `room_eq`** (fixed-listener room correction) refuses a tracker; recalibrate with
`--room-eq-grid` for tracked sessions (see [calibration.md](./calibration.md)).

### Tracker status

```c
typedef enum {
    BWA_TRACKER_DISCONNECTED = 0,  // no tracker connected on this engine
    BWA_TRACKER_NO_DATA      = 1,  // connected, but no frames arriving
    BWA_TRACKER_NO_BODY      = 2,  // frames arriving, the followed body has no valid pose
    BWA_TRACKER_LIVE         = 3,  // the followed body's pose is current
} bwa_tracker_state;
bwa_tracker_state bwa_tracker_status(bwa_engine* e);   // control thread; never blocks
```

A successful `bwa_tracker_connect` only means the socket opened. On multicast there is no handshake,
so the connect return tells you nothing about whether Motive is actually streaming or whether your
rigid body is in view. And if the stream drops mid-session, nothing tears the connection down.
`bwa_tracker_status` reports what the wire is doing right now, so you can drive a status light or a
dropout alarm. You
cannot get it from `bwa_get_listener_pose`: a dead tracker just holds the last pose, which reads the
same as a working one parked at that spot.

The two failure states are split because they need different fixes:

| State | Meaning | What to check |
|-------|---------|---------------|
| `DISCONNECTED` | No tracker on this engine (also for a `NULL` engine). | You never called `bwa_tracker_connect`, or you disconnected. |
| `NO_DATA` | No `FrameOfData` packets recently. | Motive is streaming, the network path, the data port, the multicast group. |
| `NO_BODY` | Frames arrive, but the followed body has no valid pose. | The `rigid_body_id`/`rigid_body_name`, or the body is occluded right now. |
| `LIVE` | The followed body's pose is arriving and fresh. | - |

"Recent" is a ~250 ms window, so a live stream at 100–360 Hz never trips it and a couple of dropped
frames don't flap the status. It derives from local packet-arrival timing, not the pose stamps
(those ride the server clock on NatNet 4.1+ and can't be compared to a local clock). The call never
blocks and is cheap enough to poll every frame.

### Pose prediction

```c
void bwa_set_pose_prediction(bwa_engine* e, float lead_s);    // 0 = off (default); live
```

The tracking chain (Motive's solve, the network hop, the audio block, the DAC) puts the rendered
pose **20–40 ms behind the head**; at walking speed that is 3–6 cm of panning lag. With a lead set,
the engine extrapolates the tracked **position** `lead_s` seconds along a velocity estimated from the tracker's
own frame timestamps (smoothed over ~100 ms so Motive's frame-to-frame jitter doesn't shake the
image, speed-capped, and reset across drop-outs so a stale velocity never extrapolates). On
NatNet 4.1–4.5 those timestamps come off the wire: the **server's camera clock** (mid-exposure when
the command channel is available, the frame's software timestamp when the version is env-forced
over pure multicast), so bursty packet delivery doesn't shake the estimate and a camera-rate
change in Motive doesn't mis-scale it. Older streams (and streams **newer** than the parser's
certified suffix layout) fall back to stamping at packet arrival, so prediction keeps working
either way. (If a future Motive outruns the parser, the unicast-only `Bitstream` command can pin
the server to a known syntax; see `src/natnet.c`.)
Set `lead_s` to your measured motion-to-ears latency (in seconds); too much lead **overshoots on
direction changes**, so start at the measured value, not above it (clamped at 0.2 s). The engine does
not predict orientation (it only feeds the headphone decodes). Internal tracking only (needs a
connected tracker).

### Extra listeners (multi-occupant compromise)

```c
void bwa_set_extra_listeners(bwa_engine* e, const float* xyz, uint32_t count);  // up to 3; 0 clears
```

A CAVE usually holds more than one person; single-listener panning is exact for the tracked head
and wrong for everyone else. Give the *other* occupants' positions here (`count`·3 floats, room
space; per-frame-safe, commit-gated like the pose). Every source's gains become the per-speaker
**energy mean** of the per-listener solves: the L2 barycenter of the individual renderings, so
each occupant hears an image biased toward their own solve instead of one exact and N wrong.
Constant-power; works with every panner (each extra keeps its own SPCAP/VBAP cache, so the solves
stay cache-warm). The primary listener remains `bwa_set_listener_pose`/tracking and still drives
spread direction, Doppler, air absorption, the reverb-send distance, and the headphone renders
(`BWA_PROFILE_BINAURAL` ignores extras entirely: headphones are one head by construction).
`count = 0` restores single-listener panning. Cost: one extra point solve per listener per dirty
voice (block-rate, negligible).

### Reading back the pose

```c
void bwa_get_listener_pose(bwa_engine* e, float p[3], float q[4]);
```

Returns the pose the engine is currently rendering with: the committed pose, or, with a tracker
connected, the freshest tracked pose. Safe to poll from the control thread (published by
the audio thread through a seqlock). For visuals, logging, or bringing up the tracker (see the
`bwa_track_monitor` example). Returns identity until the first audio block / tracked frame.

## Frame boundary

```c
void bwa_commit(bwa_engine* e);
```

Call once per frame after pushing all source and listener updates. It promotes this
frame's position/pose to a coherent snapshot (so the audio thread never mixes a
moved listener against a not-yet-moved source) and drains the event ring.

## Materials and scene geometry (control thread; static at load, dynamic per-frame)

```c
typedef enum { BWA_MAT_GENERIC = 0, BWA_MAT_BRICK, BWA_MAT_CONCRETE, BWA_MAT_CERAMIC,
               BWA_MAT_GRAVEL, BWA_MAT_CARPET, BWA_MAT_GLASS, BWA_MAT_PLASTER,
               BWA_MAT_WOOD, BWA_MAT_METAL, BWA_MAT_ROCK } bwa_material_type;
bwa_material bwa_material_preset(bwa_engine* e, bwa_material_type preset);   // GENERIC = token 0
bwa_material bwa_material_define(bwa_engine* e, const float absorption[3], float scattering,
                                           const float transmission[3]);
void         bwa_material_release(bwa_engine* e, bwa_material token);        // free the slot for reuse
void bwa_scene_set_mesh_mat(bwa_engine* e, const float* verts, int nverts, const int* tris, int ntris,
                           const bwa_material* tri_material);     // one token per triangle (STATIC world)
void bwa_scene_set_box     (bwa_engine* e, float w, float h, float d, const bwa_material faces[6]); // -x,+x,-y,+y,-z,+z
void bwa_scene_set_ism_room(bwa_engine* e, float w, float h, float d, const bwa_material faces[6]); // the shoebox ONLY
bool bwa_box_mesh(float w, float h, float d, const bwa_material faces[6],
                  float* out_verts, int* out_tris, bwa_material* out_tri_material);  // pure: 8 verts, 12 tris
// Dynamic (movable) occluders/reflectors - one instanced sub-scene per mover, placed by a rigid transform.
int  bwa_scene_add_dynamic_mesh(bwa_engine* e, const float* verts, int nverts, const int* tris, int ntris,
                               bwa_material material);            // -> handle >= 0, or -1
void bwa_scene_set_dynamic_transform(bwa_engine* e, int handle, float x, float y, float z,
                                    float qx, float qy, float qz, float qw);   // room-space pos + quat
void bwa_scene_remove_dynamic_mesh(bwa_engine* e, int handle);
```

A `bwa_material` is an **opaque, engine-scoped token**: a small index, where `0` is always the
built-in `GENERIC` default. Mint one from a preset (Steam Audio's published coefficients;
`BWA_MAT_GENERIC` returns `0` without minting) or from custom 3-band coefficients (clamped to
`[0,1]`, NaN-sanitized). **Both paths return the same kind of token** from one fixed 64-entry
table; on overflow (or an out-of-range enum) the mint returns `0` and sets `bwa_last_error`.
Tokens are **not** generation-checked: they stay valid until released, and per-triangle indices
out of range clamp to the default.

Most apps **mint once and never release**: the table is a palette, not a per-object allocation.
For an app that churns many *distinct* materials, `bwa_material_release(token)` frees a slot for
reuse. It's **caller-managed, like `free()`**: only release a token no live mesh or source still
references. Already-set meshes copied their coefficients at set time, so they're unaffected; a
*future* `bwa_scene_set_mesh_mat` with a released token gets whatever the slot was reused for.
Token `0` can't be released.

Geometry rules:

- **Room space, RH meters, CCW triangles.** `bwa_scene_set_box` builds a floor-based shoebox
  with inward-facing normals: x/z centered on the origin, y from 0 (the floor) up to `h`;
  the listener stands inside.
- **The outdoor degenerate: `bwa_scene_set_ground(e, y, mat, pressure_release)`.** One horizontal
  mirror plane at height `y` instead of a box: the ground bounce, the dominant early reflection
  when there is no room. Same dual capture as the box (ISM always, a large ground quad for the
  ray tracer with SDK); replaces any prior box (one room at a time); live-safe like the box.
  `pressure_release` flips the reflection's polarity: set it when the "ground" is a water
  surface and the listener is under it.
- **Pressure-release faces: `bwa_scene_set_pressure_release(e, face_mask)`.** Flags box faces
  (bit f = face f, the `-x,+x,-y,+y,-z,+z` order) whose image-source reflection should NEGATE:
  the physics of reflecting off a much softer medium. The flagship case is a virtual underwater
  room whose ceiling is the surface (`1u << 3`): the inverted image interferes destructively with
  the direct sound near the boundary, the Lloyd's-mirror comb. ISM only (polarity is a specular
  concept); occlusion and the reverb bed keep the face's material. Call after `_set_box` or
  `_set_ground`; a later room call resets every face.
- **One scene, two consumers.** The same per-triangle materials feed both occlusion (per-band
  transmission) and the reflection bed (absorption/scattering): one shared `IPLScene`.
- **The box and your own mesh are alternatives, not layers.** `bwa_scene_set_box` *is* a
  `bwa_scene_set_mesh_mat` call, so it replaces the static mesh exactly like one. Calling
  `_set_box` and then `_set_mesh_mat` (the natural order, room first and then the pillar
  standing in it) drops the walls, and drops them **half-way**: the image-source shoebox is
  separate state and survives, so early reflections keep bouncing off a room that occlusion
  and the reflection bed can no longer see. To have both, call `_set_box` first (nothing else
  captures the ISM room) and then **one** `_set_mesh_mat` carrying the box's 12 inward-facing
  triangles alongside your own geometry.
- **Static versus dynamic.** `bwa_scene_set_mesh_mat`/`_set_box` set the STATIC world: committed once,
  BVH built once. For things that MOVE, `bwa_scene_add_dynamic_mesh` registers a rigid **instance**
  (its own sub-scene) that you reposition with `bwa_scene_set_dynamic_transform` (room-space position
  + orientation quaternion), a cheap top-level BVH refit, not a geometry rebuild, so it's the
  per-frame path. Geometry is in the mover's LOCAL space; one material token covers all its triangles;
  the movable table holds 32 instances. `add` returns a handle (or `-1`: no SDK / bad geometry / full).
- **Runtime-safe.** Both the static swap and dynamic-mesh moves are safe to call at any time:
  `iplSceneCommit` runs under an **exclusive** scene lock and every ray trace holds it
  **shared**, so a commit can never race a trace (phonon documents exactly this constraint).
  Staged geometry is **flushed synchronously** when the reflection bed or pathing is created, so
  `bwa_scene_set_box` followed immediately by `bwa_start` bakes the room you just set. Caveat:
  **baked** reflections/pathing don't track a runtime change (the bake froze the geometry; see
  [materials.md](./materials.md)); real-time reflections and occlusion do. Replacing the whole
  static mesh at runtime rebuilds the entire BVH; prefer dynamic meshes for movers.
- **No SDK, no-op.** Everything here is a documented no-op without the `BWA_HAVE_STEAMAUDIO`
  build, except token minting (plain table state) and `bwa_scene_set_box`, which ALWAYS captures
  the shoebox for the image-source early reflections. `bwa_scene_add_dynamic_mesh` returns `-1`
  without the backend.
- **The room calls are live-safe.** `bwa_scene_set_box` / `_set_ground` / `_set_pressure_release`
  publish the ISM room to the audio thread through a seqlock, and each opted-in source re-solves
  its images next block: gains ramp, delays glide, so a mid-scene room change (the submerge
  transition) bends the reflections instead of clicking. With the SDK a live box/ground also pays
  the static-mesh BVH rebuild.


### Composing a scene out of the box and your own geometry

`bwa_scene_set_box` does two jobs: it captures the shoebox for the image-source early reflections
(which works with or without the Steam Audio build) and it sets the ray-traced static mesh to that
box. That is what you want when the box **is** the scene, and it replaces the static mesh, so the box
and your own occluders used to be alternatives rather than layers.

Use the two calls it is made of when you have both:

```c
bwa_scene_set_ism_room(e, w, h, d, faces);        // the shoebox; leaves the static mesh alone
float bv[24]; int bt[36]; bwa_material bm[12];
bwa_box_mesh(w, h, d, faces, bv, bt, bm);         // its 12 inward-facing triangles
// concatenate bv/bt/bm with your own geometry, then commit ONE mesh:
bwa_scene_set_mesh_mat(e, all_verts, nv, all_tris, nt, all_mats);
```

Neither of those is order-dependent, and `bwa_box_mesh` is pure (no engine handle, no allocation), so
composing never means re-deriving the box's winding and inward-facing normals yourself.

**Clearing geometry does not need a decoy.** `bwa_scene_set_mesh_mat(e, NULL, 0, NULL, 0, NULL)`
removes the static mesh. A malformed partial mesh (vertices but no triangles, counts disagreeing with
pointers) is still dropped intact and leaves the previous geometry in place, so the engine
recognizes the clear only when both pointers are NULL and both counts are non-positive.

## Occlusion and directivity (control thread; per-frame except where noted)

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
**audio thread ramps**: never a jump. Occlusion and directivity are independent (a source can be
directional without being occluded). The `_get_` reads return the latest published scalar for
HUD/diagnostics and are safe to poll. Occlusion is a no-op without the Steam Audio build (use the
manual path below).

**Directivity works in every build.** Same weighted-dipole model, `|(1-w) + w·cos θ|^p`, two
renderers: with a Steam scene the occlusion sim evaluates it (~10-30 Hz, published + ramped);
without one (no SDK, or SDK with no scene) the audio thread evaluates it per block from the
source's forward axis and the active listener. Both are walk-correct; the readback reports
whichever path is live. Don't expect bit-identical gains between the two (update rates differ),
just the same pattern.

### Manual occlusion (no SDK needed)

```c
void bwa_source_set_occlusion_manual(bwa_engine* e, bwa_source s, float level, const float bands[3]);
```

Drives the **same** handle-gated, audio-thread-ramped publish path the ray-tracing sim uses, from
your own game logic. `level` is broadband transmittance (1 = clear .. 0 = blocked); `bands`
(optional; NULL = broadband only) is a low/mid/high tilt in `[0,1]` rendered as the same 3-biquad
transmission EQ, so a wall *muffles* rather than only attenuating. This is how a no-SDK build gets
gameplay-driven occlusion ("behind a door the game knows about", underwater, muffled-behind-a-menu)
with identical click-free rendering. Don't drive one source from both this and the sim
(`bwa_source_set_occlusion`): the sim republishes every tick and wins.

## Propagation effects (control thread; per-frame)

```c
void bwa_source_set_doppler        (bwa_engine* e, bwa_source s, bool on);
void bwa_source_set_air_absorption (bwa_engine* e, bwa_source s, bool on);
void bwa_source_set_loudness_comp  (bwa_engine* e, bwa_source s, bool on);
void bwa_source_set_proximity      (bwa_engine* e, bwa_source s, bool on);
void bwa_set_speed_of_sound        (bwa_engine* e, float meters_per_s);   // engine-wide; live
```

Opt-in, per source, default **off**, and (unlike occlusion/directivity) **pure per-voice DSP that
needs no Steam Audio build**. All derive from the live source↔listener distance, recomputed each
block from the committed positions.

- **Doppler** renders the source through its acoustic propagation delay (`distance / c`). A per-voice
  fractional delay line glides toward that delay each block; the *glide rate is the pitch shift*
  (approaching → up, receding → down), so no velocity input is needed: it falls out of motion. The
  delay (hence the effect) **saturates past ~8 m**, which bounds the ring; enabling adds the real
  propagation latency. Best for fast movers; subtle for slow ones in a small room.
- **Air absorption** is a distance-driven one-pole **high-frequency low-pass** (far sources sound
  duller): cutoff falls ~650 Hz/m from 18 kHz near, down to a ~1.2 kHz floor. Subtle at a few meters,
  pronounced for sources placed at large *virtual* distances.
- **Loudness compensation** is the perceptual counterpart: distance attenuation takes level, and at
  lower levels the ear also loses **LF sensitivity** (ISO 226), so an attenuated source reads *thin*
  as well as far. This restores part of the body with a one-pole LF shelf (~250 Hz) whose boost
  tracks the attenuation the panner applied: +0.4 dB per dB taken, capped +8 dB. It's a
  stylization ("far, not tinny"), not physics; leave it off for strict realism.
- **Proximity** is loudness compensation's near mirror: an LF shelf (~300 Hz) that *rises* as the
  source closes inside ~1 m (0 dB at the radius, up to +6 dB at the head): the spherical-wavefront
  proximity effect. In a walkable volume this is the missing half of distance: at arm's length a
  source should read as *bass*, not just level. Pair with `bwa_set_near_spread` (the geometric
  half: a close source also widens).
- **Speed of sound** (engine-wide, not per source): everything that renders a propagation *delay*
  derives from `c`: Doppler (delay and pitch-shift magnitude) and the image-source reflection
  delays. Default 343 (air); 1480 is underwater; small values exaggerate Doppler for slow-motion
  effects. Live: a change **glides** every delay to its new target (bends, never steps). Delays
  saturate against their ring capacity (~40 ms at 48 kHz), so extreme slow motion caps. Clamped to
  [30, 20000]; the Steam sim's ray clock is phonon's own and does not follow it.

All are **non-blocking, enqueue-only**, ramp on the audio thread (coefficients and gains glide across
the block; no zipper), and are independent of each other and of the panner / profile. They apply to
the **direct path only**: the reflection wet send is tapped *before* them, so reflections keep their
own propagation. They do not affect ambisonic beds (world-locked, no position).

```c
void bwa_source_set_attenuation_override(bwa_engine* e, bwa_source s,
                                         float ref_dist, float rolloff, float min_gain);
```

Override the **layout's distance-attenuation curve** for one source, using the same formula with
this source's own parameters: `atten = clamp((ref/max(d,ref))^rolloff, min_gain, 1)`, `d` the
source→primary-listener distance. The two useful shapes: **`rolloff 0` = constant level at any
distance** (a direction-only cue such as narration, UI, or a beacon, which the layout-global curve
can't express), and a steeper/shallower curve than the room default for content that should die
faster or carry farther. `ref_dist <= 0` **clears** the override (back to the layout curve). The gain
solve applies it by *ratio*, so it's panner-agnostic and composes with spread, dual-band,
decorrelation, and loudness compensation (which tracks the override's curve: a constant-level
source gets no LF compensation); the reflection distance→wet send keeps its own mapping. Ramps
like any gain change; per-frame-safe; beds unaffected (no distance).

## Source spread / size (control thread; per-frame)

```c
void bwa_source_set_spread(bwa_engine* e, bwa_source s, float amount);   // 0 = point (default) .. 1 = wide
void bwa_source_set_extent(bwa_engine* e, bwa_source s, float width, float height); // anisotropic w/h
void bwa_source_set_size  (bwa_engine* e, bwa_source s, float radius_m); // metric alternative: radius in m
typedef enum { BWA_SPREAD_LOBE = 0, BWA_SPREAD_MDAP = 1, BWA_SPREAD_SPECTRAL = 2 } bwa_spread_mode;
void bwa_set_spread_mode(bwa_engine* e, bwa_spread_mode mode);            // engine-wide; live A/B
```

Angular **width** of a source. A waterfall, a crowd, an engine room, or ambience shouldn't
collapse to a single point: raise `spread` and the source's energy fans out across the speakers
around its direction.

**`bwa_source_set_extent`** is the anisotropic form (BS.2127-style width/height): a shoreline is
wide but not tall, rain is tall but not wide. Separate horizontal and vertical extents, each 0..1;
equal values behave exactly as the isotropic spread, and a later `bwa_source_set_spread` resets to
isotropic (last call wins). The ring modes squash their virtual-source cap per axis (a 1×0 extent
renders as a horizontal *arc* of panner solves), the lobe stretches its angular falloff on the
extent ellipse. Width/height are **room-referenced** (anchored to the room's up axis), so straight
overhead the split is inherently ill-defined; BS.2127's polar extent shares that singularity, and
anisotropic sources give up the transported ring frame's pole-crossing continuity for exactly that
reason. Rides everything spread rides: the size/near floors (they floor *both* axes, the larger
axis drives decorrelation), constant power, per-frame-safe.

Prefer **`bwa_source_set_size`** when the content has a physical size: it floors the spread at the
angle the radius subtends from the tracked listener (`asin(r/d)`, fully wide once the listener is
inside the source), so a 2 m waterfall *stays* 2 m wide as the listener walks; an angular spread
changes physical size with distance. The larger of the two knobs wins, and a sized source subsumes
`bwa_set_near_spread` (engulfment is the `d < r` case). All three are per-frame-safe.

It's implemented in the per-block gain solve, not the sample loop, and **renormalized to the
panner's own power**: widening never changes loudness, and the perceived direction stays put.
It's **panner-agnostic** (works over DBAP/SPCAP/VBAP), and the change ramps click-free like any
gain change. Three render modes sit behind the knob (`bwa_set_spread_mode`, an atomic live A/B
like the panner switch; sources with spread 0 are unaffected either way). Full treatment of the
modes: [spatialization.md](./spatialization.md#propagation-effects-opt-in-per-source).

- **LOBE** (default): the panner's point gains blend toward a width-controlled lobe. One solve,
  smooth and cheap, but the extent reshapes gains solved for a *point*.
- **MDAP** (Pulkki): a ring of virtual sources (cone half-angle = `spread`·90°) each panned with
  the *selected panner* and summed, so the extent inherits the panner's character. ~13× the
  gain-solve cost, block-rate and dirty-gated.
- **SPECTRAL** (Zotter and Frank's phantom-source widening): 6 bands, each panned to its own
  direction inside the cone (the LF band stays put). Different frequencies come from different
  speakers, so there are **no coherent copies to collapse or comb-filter** as the tracked
  listener walks: extent without decorrelation noise. ~6 band filters + 6 gain sets per *wide*
  voice; point sources pay nothing. With dual-band on, the sub-700 Hz bands take the amplitude
  norm, so the two A/Bs compose.

LOBE/MDAP + decorrelation and SPECTRAL are two different answers to the same phantom-collapse
problem: A/B/X them (the playground's blind harness has rows for both) and keep the winner.

```c
void bwa_set_decorrelation(bwa_engine* e, bool on);   // off by default; live A/B
```

LOBE and MDAP both feed every speaker the **same signal** at different gains: coherent
copies, which collapse to phantom images between speakers and comb-filter as the tracked listener
walks. `bwa_set_decorrelation` fixes that: a spread source's energy splits into a coherent share on
the normal path and an incoherent share routed through **per-speaker sparse velvet-noise filters**
(~30 taps over 30 ms, time-domain, no onset latency; Välimäki/Schlecht's velvet-noise
decorrelator). The split follows `spread` (a point source is untouched; at full spread the wide
part is fully decorrelated), power is conserved (incoherent energy adds), and the toggle ramps:
a click-free live A/B. This is what makes wide sources read as *extent* rather than *several
copies*, and it is the same decorrelator bank the parametric bed's diffuse stream uses.

```c
void bwa_set_near_spread(bwa_engine* e, float radius_m);   // 0 = off (default); live
```

**Near-listener widening**: a point source flying at the head physically subtends a growing solid
angle, but a point panner collapses it into the nearest speaker and snaps it across the head as it
passes. With a radius set, every source's spread is **floored at `1 − dist/radius`**: untouched
beyond the radius, fully wide at the head. `radius_m ≈ 1.0` is a good start. The widening rides the
selected spread mode and (when enabled) the decorrelators, and the changed gains ramp like any
solve. Engine-wide policy; it takes effect with each gain re-solve (continuous under tracking).

```c
void bwa_set_hole_spread(bwa_engine* e, float strength);   // 0 = off (default); live
```

**Hole-aware widening**: the other geometric floor. The CAVE array is a barrel with nothing
covering the poles, so a source aimed into a pole is carried by the big hull triangle that closes
the hole: a split image, not a phantom. With this on, a source's spread is **floored by how far
its bearing sits from the nearest speaker**: 0 until that gap exceeds the array's own mean
speaker spacing, then rising to fully wide at a 90 degree gap, scaled by `strength` (1.0 is the
honest width, clamped at 2). An array that surrounds the listener derives no floor at any
bearing, though the knob still pays its measurement (one dot product per speaker plus one arc
cosine per voice per solve). It composes with the other spread floors as a max, rides the
selected spread mode and the decorrelators, and re-solves every source on the next block, static
ones included. No effect in `BWA_PROFILE_BINAURAL`. Design and measured numbers:
[spatialization.md](./spatialization.md) → "Array holes".

## Channel test / diagnostics (control thread)

```c
typedef enum { BWA_TEST_OFF = 0, BWA_TEST_SINE = 1, BWA_TEST_NOISE = 2 } bwa_test_kind;
void     bwa_set_test_signal(bwa_engine* e, uint32_t channel, bwa_test_kind kind, float gain);
uint32_t bwa_get_bus_levels(bwa_engine* e, float* peaks, uint32_t cap);  // last block's per-channel output peak
uint32_t bwa_get_active_voices(bwa_engine* e);                           // last block's active voice count
bool     bwa_get_health(bwa_engine* e, bwa_health* out);                 // xruns + load; see below
uint64_t bwa_get_xruns(bwa_engine* e);                                   // device dropouts, one line
```

`bwa_get_active_voices` is the voice-pool gauge next to the meters: the audio thread publishes each
block's active count (playing, sound bound; paused voices count, they haven't ended). Poll it for
HUDs or health monitoring; it reads 0 until audio runs.

`bwa_set_test_signal` drives a single **output channel** with a built-in signal (660 Hz sine or white
noise), injected **after** the per-speaker align stage: a raw value straight on the channel.
`channel` is in `[0, bwa_get_channel_count())`; anything else is ignored.

This is a speaker-check / wiring / calibration tool: walk a tone across every channel to confirm
the channel→speaker map, find a dead speaker, set a trim. It is **not** a spatial path: it
bypasses the panner, so don't use it to "place" a sound. Per-frame-safe, takes effect next block,
no `bwa_commit` needed. Any number of channels at once; `gain 0` / `BWA_TEST_OFF` silences one.
Works in every profile (cave/cave_both: a raw tone on that Digiface channel; the headphone
profiles: that bus channel HRTF'd as its virtual speaker; in `binaural` the tone rides the
diffuse/virtual-speaker path, since only point sources render direct). Needs no SDK.

`bwa_get_bus_levels` is the matching **readback**: each output channel's last-block peak `|sample|`
(linear), measured at the very end of the render, after align, the test signal, and the limiter.
That is exactly what the device channel received. It fills up to `cap` floats and returns the
count filled (`bwa_get_channel_count()` when `cap` allows). Per-frame-safe (relaxed atomic reads, no
locks); levels read 0 until audio is
running. Drive channel meters or a speaker-activity display with it; the playground lights each
speaker gizmo from this.

### Device health: was the callback starved?

```c
typedef struct bwa_health {
    uint64_t blocks, xruns, dropped_frames, driver_resyncs, late_blocks, stream_starves;
    float    peak_load;      // worst block's render time / block period
} bwa_health;
bool     bwa_get_health(bwa_engine* e, bwa_health* out);  // false = this setup cannot measure
uint64_t bwa_get_xruns(bwa_engine* e);                    // the one-line form
```

Everything else on this page describes the **render**. None of it can tell you the device asked for
a block and did not get one. That's the failure that makes a clean render sound broken, and the
one thing an offline render cannot reproduce by construction. That is what these count.

The fields answer two different questions with two different fixes:

- **`xruns`** is the *device* running on without us. Its sample position jumped past where the last
  callback said the next one would land, so it clocked out audio nobody rendered. Fix upstream: a
  bigger buffer, fewer competing loads, a look at the driver. `dropped_frames` is how much audio
  those gaps swallowed.
- **`late_blocks`** is *us* overrunning the block period, which is what eventually produces xruns.
  Fix downstream: a cheaper scene, fewer voices, and watch `peak_load`; it is the worst single block's
  render time as a fraction of the period, so 1.0 means a block exactly consumed its budget and
  anything near it is living dangerously.
- **`stream_starves`** is neither. The device kept its deadline and a *streamed voice* had nothing to
  give it, because the disk thread didn't refill in time; that block's tail rendered silence. It
  sounds identical to a clip ending, which is exactly why it is counted separately.

Read them against `blocks`. One xrun in two million blocks is a machine hiccup; one in a thousand is
a configuration to fix.

**The return value is the load-bearing part.** `bwa_get_health` returns whether the numbers mean
anything at all. It is false when nothing here can observe a dropout: before `bwa_start`, on the
manual sink (no clock, no deadline; it cannot miss one), or on an ASIO driver that never flags a valid
sample position, leaving nothing to compare against. In every one of those cases `xruns` is zero, and
reading that zero as a clean bill of health is precisely how a starved device goes unnoticed.

```c
bwa_health h;
if (!bwa_get_health(e, &h))
    log("device health unavailable on this driver - xruns cannot be detected here");
else if (h.xruns)
    log("%llu dropouts (%llu frames) over %llu blocks, peak load %.2f",
        h.xruns, h.dropped_frames, h.blocks, h.peak_load);
```

Check the boolean once after `bwa_start` on an unfamiliar driver; poll `bwa_get_xruns` for a HUD
afterwards. Both are control-thread, per-frame-safe (relaxed atomic reads), and every count is
monotonic since start.

### Output capture (recording / golden checks)

```c
typedef void (*bwa_output_fn)(void* user, const float* planar, uint32_t channels, uint32_t nframes);
void bwa_set_output_capture(bwa_engine* e, bwa_output_fn cb, void* user);
```

`bwa_set_output_capture` taps the **final device-bound output**: post-limiter, exactly what reaches
the device. The callback runs on the **audio thread**, once per block, with **planar** channel-major
data (`planar[c*nframes + i]`). `channels` is the primary device's channel count: the array count
(`bwa_get_channel_count`) for `cave`/`cave_both`, `2` for `binaural`/`cave_sim`. Same audio-thread contract as any
callback here: **copy out only, no alloc/lock/syscall/file I/O**; write to a ring your own thread
drains to a file or comparison buffer. Pass `cb = NULL` to stop, and keep `user` alive until after
the NULL set plus one block.

Two uses: **recording** (grab what you're hearing; the playground's `F9` writes the binaural output
to a WAV this way) and **offline sanity / golden-audio tests**. For recording, any sink works. For
golden tests, use the **manual sink** below: it's deterministic, where the null sink is only
*paced* (a real thread, a wall clock, a run-varying block count).

### Offline / deterministic render: `bwa_render_block`

```c
const float* bwa_render_block(bwa_engine* e, uint32_t* channels, uint32_t* nframes);
```

Set `bwa_desc.sink = BWA_SINK_MANUAL` and the engine creates no device and no audio thread: **you** pump one block
at a time on your own thread. Each call renders exactly one block (`bwa_desc.block_size` frames) of the
profile's primary output (`binaural`/`cave_sim`: 2 ch; `cave`/`cave_both`: `bwa_get_channel_count()` ch) into
engine-owned memory and returns a pointer (**planar**, `channels * nframes` floats, valid until the
next call or `bwa_stop`). Fills `*channels`/`*nframes` (either may be `NULL`). Returns `NULL` if the
engine isn't started or the sink isn't `MANUAL`.

The timestamp is a pure sample counter (no wall clock), so a **fixed input + fixed call sequence
renders bit-identically every run**; that reproducibility is what makes a committed golden meaningful
(see `test/golden_test.c`: push a tone at a fixed position → render N blocks → compare per-channel
energy against a reference). Same caveat as capture: only the **synchronous DSP** is reproducible. The
async sim layers (Steam occlusion, reflection, pathing) run on their own wall-clock-timed threads
(30 / 12 / 10 Hz), so keep golden renders off them, or drive the deterministic entry points
(`bwa_source_set_occlusion_manual` instead of the ray-traced sim). You can still register a capture
callback on a manual sink, but reading `bwa_render_block`'s return value directly is simpler.

## Panner and layout query (control thread)

```c
typedef enum { BWA_PAN_DBAP = 0, BWA_PAN_SPCAP = 1, BWA_PAN_VBAP = 2 } bwa_panner;
typedef enum { BWA_DECODE_DEFAULT = 0,   // reserved for default-init; NOT a named algorithm
               BWA_DECODE_ALLRAD  = 1,
               BWA_DECODE_EPAD    = 2 } bwa_bed_decoder;   // bwa_desc.bed_decoder
void     bwa_set_panner(bwa_engine* e, bwa_panner panner);            // load-time or live (atomic switch)
void     bwa_set_dual_band(bwa_engine* e, bool on);                // live A/B; wraps the selected panner
void     bwa_set_dual_band_cap(bwa_engine* e, bool on);                      // live A/B; ITD-corrects dual_band's low band
void     bwa_set_spcap_focus(bwa_engine* e, float focus, float density);  // SPCAP tuning; <= 0 = default
float    bwa_spcap_focus_default(const float* positions, uint32_t n);     // pure: what the geometry implies
uint32_t bwa_get_speakers(bwa_engine* e, float* xyz, uint32_t cap); // read back the layout; NULL xyz = count only
```

`bwa_set_panner` chooses the per-source panner behind the master bus:

- **DBAP** (default) is listener-relative and recomputed per block from the tracked pose.
  This is the panner for a **moving** observer roaming the array.
- **SPCAP** is a smooth, all-speaker, placement-correcting sweet-spot panner for a **fixed**
  observer: don't track, set the sweet spot once. It conserves loudspeaker power across an
  uneven array.
- **VBAP** is the sharpest: the 2-3 speakers of the containing triangle carry a source. Also
  fixed-observer, best on a cleanly-triangulable array; it falls back to DBAP otherwise.

The switch is atomic, so flipping it live is safe; the layout tool's `B` key A/Bs panners
exactly this way.

`bwa_set_spcap_focus` is SPCAP's own tuning, and it does nothing under the other two panners.
`focus` is the lobe sharpness in `((1+cos)/2)^focus`: raise it to concentrate a source on fewer
speakers (tighter image, harder edges), lower it to spread the source out (smoother, blurrier).
`density` is the exponent of the placement-correction kernel that de-biases a clustered array,
and 2.0 is right almost always. Pass 0 or less for either argument to revert that one to its
default.

The focus default is **derived from your array**, not a constant: the geometry-derived lobe from
[spatialization.md](./spatialization.md) (the 26-speaker cube grid lands near 12.7).
`bwa_spcap_focus_default` computes the same number for an arbitrary set of speaker positions
(3 floats each, the `bwa_panner_gains_batch` convention) without an engine, so a tool can show
what a layout implies before you override it. `bwa_panner_gains_batch` takes the same two knobs
with the same sentinel, so you can score a layout at the tuning you will ship ("Offline panner
and bed evaluation" below).

Both knobs are live and per-frame-safe, and the change reaches **every** source on the next
block, including sources that never move. Neither value lives in the layout file: like the rest
of the live A/B surface, persisting a dialed value is your application's business. Dial by ear
with the playground or the layout tool's preview, then set it at startup. You do not have to
settle it by ear alone: `bwa_validate --focus 4,32` sweeps the knob in one measurement session
and reports the comb depth each setting costs, against one speaker driven alone as the floor
(`docs/validation.md`, including where that sweep has power and where it does not).

`bwa_set_dual_band` (off by default, live-toggleable) **wraps** the selected panner. It splits
each source at ~700 Hz, then pans the low band with **amplitude** (pressure / velocity-vector)
normalization and the high band with the panner's usual **power** (energy-vector) normalization:
SPAT's "VBP Dual-Band". You get sharper low-frequency localization for a near-centered listener.
The panning *direction* is unchanged; only the low band's level/coherence differs. It is
sweet-spot dependent like VBAP, so for a roaming listener it's a by-ear / measurement call.

`bwa_set_dual_band_cap` (off by default, live-toggleable) corrects that low band's **interaural
time difference**, and **requires `bwa_set_dual_band`** since the low band is the only thing it
touches. CAP constrains the interaural component of the summed field, so the rendered ITD comes
out exact and stays exact as the head turns, which plain dual-band does not do. It is a
projection on top of the selected panner, not a fourth panner: facing the source it reduces to
the seed, and it fades out with `bwa_source_set_spread`. This is the only feature that reads
head **orientation** into the speaker path, so it wants a real tracked pose. Two known
exclusions: `BWA_SPREAD_SPECTRAL` bypasses it entirely, and the offline
`bwa_panner_gains_batch` never solves a low band, so it cannot be swept there. Theory, limits,
and the open crossover question: [spatialization.md](./spatialization.md).

`bwa_desc.bed_decoder` reserves **value 0 for default-init**, the sokol convention. A zero-filled
`bwa_desc` therefore asks for whatever the engine currently defaults to rather than naming an
algorithm, so that default can move later without an ABI break and without silently re-pointing a
caller who did name one. `engine.c`'s `resolve_bed_decoder` is the single line that says what the
default is today (AllRAD). Every public enum also carries a `*_FORCE_U32 = 0x7FFFFFFF` member that
pins its width to 32 bits. C leaves an enum's underlying type implementation-defined, and this DLL is
consumed by C# P/Invoke and a GDExtension, both of which marshal enums as 4-byte ints. Never pass it.

`bwa_desc.bed_decoder` chooses the **diffuse-bed** SH→speaker decoder. It affects the ambisonic
and reflection beds only, never the point-source panner:

- **AllRAD** (`BWA_DECODE_ALLRAD`, the default): decode to a uniform virtual layout, then VBAP
  onto the real array. Robust on an irregular array, localizes a touch sharper. A pole with no
  real speaker within ~60° (a floor-less array's nadir) gets an **imaginary speaker** whose
  decode share is discarded: diffuse energy aimed into the hole is dropped rather than smeared
  onto the nearest ring.
- **EPAD** (`BWA_DECODE_EPAD`): energy-preserving decode (Zotter/Pomberger/Noisternig 2012); a
  panned plane wave's decoded energy is constant over direction *by construction*, the
  flattest loudness-versus-direction of the two. AllRAD versus EPAD on the real array is a
  by-ear call. The **FDN's line render follows EPAD too** (with AllRAD selected the FDN keeps
  its house AllRAD render).

The plain sampling (projection) decode is **not selectable**: on an irregular array it
over-energizes dense speaker regions (dominated by both options above), so it survives only as
the engine's automatic fallback when a *degenerate* layout defeats the chosen build (a failed
fallback is silent by design; fix a degenerate survey rather than decoding around it).

The decoder is create-time configuration (the decode matrix is built at `bwa_create`); see
[`spatialization.md`](./spatialization.md) for the theory.

`bwa_get_speakers` returns the effective layout (the default grid, or the `layout_path` file) as
`cap*3` floats in channel order, and returns the count **filled**: `min(cap, count)`, the same
convention as `bwa_get_bus_levels`. Pass `xyz = NULL` to query the total count (the same count
`bwa_get_channel_count()` reports). Use it to visualize or audition the geometry the engine is
actually panning with.

### Channel count

```c
uint32_t bwa_get_channel_count(bwa_engine* e);   // the ACTIVE channel count (4..26), fixed at create
```

The engine's channel count **is the layout's speaker count**: a `layout_path` file with 4..26
speakers, or 26 (the default grid) with no path. `BWA_CHANNELS` (26) is only the compile-time
*capacity*: a collaborator's 24-speaker array loads a 24-entry layout into the same binary, the
device opens 24 channels, and every consumer (panners, beds, reverb, monitor, calibration) follows.
Size meter/speaker arrays from this getter, not the constant.

The old sharp edge here is now fenced: a **failed** explicit layout load still leaves the engine
on the 26-grid default (reason in `bwa_last_error`, readable after `bwa_create`), but
`bwa_start` refuses it with `BWA_ERR_LAYOUT`, so a 24-speaker deployment can no longer silently
render 26 channels. Only `layout_path = NULL` runs the default grid.

### Offline panner and bed evaluation

```c
uint32_t bwa_panner_gains_batch(bwa_panner panner, const float* positions, uint32_t n,
                                const float lis[3], const float* srcs, uint32_t nsrc,
                                float focus, float density, float* out);
uint32_t bwa_bed_gains_batch(bwa_bed_decoder decoder, bool max_re,
                             const float* positions, uint32_t n,
                             const float* dirs, uint32_t ndir, float* out);
```

Both take no engine handle. They run the engine's own solves over a **candidate** layout you pass
in, so a layout tool scores what will ship instead of a re-implementation. Both are pure and
reentrant: the per-listener cache is per-call stack state, so any thread can call them, including
while an engine renders.

`bwa_panner_gains_batch` writes `out[i*n + s]`, `nsrc*n` floats, and returns `nsrc`. It uses the
default DBAP and distance tuning, and shares the SPCAP/VBAP per-listener cache across the batch,
which is what makes a grid sweep cheap.

`focus` and `density` are SPCAP's two knobs, the same pair `bwa_set_spcap_focus` sets live and
under the same rule: pass 0 or less for either one to get the default for **this** array, meaning
the geometry-derived focus (what `bwa_spcap_focus_default` returns for `positions`) and density
2.0. Both are inert under `BWA_PAN_DBAP` and `BWA_PAN_VBAP`, which have no lobe to sharpen, so any
value scores the same there. Score at the tuning you will ship: a layout graded at the derived
focus tells you nothing about how it behaves once you dial the knob.

`bwa_bed_gains_batch` is the diffuse-bed counterpart. It takes plane-wave **directions** rather
than positions, because a bed is content at infinity. It builds the same AllRAD or EPAD decode the
engine builds for this layout, applies max-rE weighting when you ask for it, and writes `ndir*n`
gains. Those gains can be negative (SH sidelobes). A layout that scores well here is a good
quadrature for the sphere, which is what ambisonic content wants from an array and what the
point-source panners cannot see.

`bwa_layout_tool` drives both: the Score board, the rE coverage overlay, the badness map, and the
optimizer cost all go through them. See [`layout-schema.md`](./layout-schema.md).

## Tracked room EQ (control thread; live)

```c
void bwa_set_tracked_room_eq(bwa_engine* e, bool on);   // default ON when the layout carries a grid
```

Layouts carrying a `room_eq_grid` (written by `bwa_calibrate --room-eq-grid`, one run per mic
placement) get **listener-tracked LF room correction**: each block the engine re-interpolates the
grid's per-speaker modal-cut depths at the live listener position (inverse-distance weights over
the measurement points). The align-stage biquads glide toward them at 24 dB/s, click-free by
construction, fast enough to track a walking listener.

This is the moving-listener answer to the static `room_eq`, which `bwa_start` rejects for moving
sessions. It works because the room's mode *frequencies* don't move with the listener, only how
strongly each mode reads at a position, so one per-speaker fc/Q ladder plus per-position depths
interpolate safely. Mid/HF room correction stays out of the tracked path: it is position-sensitive
at the centimeter scale ([`calibration.md`](./calibration.md)).

The switch is the live kill switch (off glides every cut to flat, a clean A/B). It's a no-op for
layouts without a grid.

## Tracked listener alignment (control thread; live, OFF by default)

```c
void bwa_set_tracked_align(bwa_engine* e, bool on);
void bwa_set_tracked_align_guards(bwa_engine* e, float dead_zone_m, float slew_frames_per_s);
```

The layout's per-speaker trims align the array's arrival times at **one** point, the array
centroid. Turn this on and the output stage re-references those trims onto the **tracked**
listener, so time coherence follows the head: per speaker, the extra propagation delay and 1/r
level for `|speaker - listener|` against `|speaker - centroid|`.

It is off by default because a moving delay line is a resampling event: a walking listener glides
every speaker's delay at once, a Doppler shift on everything the array plays. Two guards make it
usable; pass 0 or less for either to take its default. The enable and the guards are **separate
calls** (the same split as `bwa_set_limiter` / `bwa_set_limiter_ceiling`): the enable is the
thing you A/B, and toggling it must not quietly reset a guard you dialed. Order does not matter,
and the guards are live (re-read every block).

| Argument | Default | What it does |
| --- | --- | --- |
| `dead_zone_m` | 0.05 | How far the head must move before the engine recomputes anything. Tracker output is jittery; without a dead zone the array glides permanently. 5 cm of slack is 0.15 ms of residual arrival error. |
| `slew_frames_per_s` | about 63 at 48 kHz | Ceiling on how fast a speaker's delay may change. That rate over the sample rate **is** the resampling ratio, so it bounds the pitch shift. The default follows a 0.45 m/s closing speed, 0.13% of shift (2.3 cents). |

A listener who outruns the rate limit gets a **lagging** alignment rather than a pitch-shifted
array, the trade the default takes on purpose. Off is exact, not approximate: while nothing is
displaced the align stage runs its original integer delay tap, bit-identical to a build without
the feature; toggling either way glides. It reads the same active listener position everything
else does (committed pose or internal tracker), touches no gain solve, and re-solves no sources.
Saturation limits, the level-clamp caveat, the unreported added latency, and why the defaults are
what they are: [spatialization.md](./spatialization.md#re-aligning-to-the-tracked-listener-bwa_set_tracked_align-off-by-default).
Untested on hardware.

## Situation tuning (`bwa_tuning_preset`, `bwa_apply_tuning`)

```c
typedef enum { BWA_SETUP_DEFAULT = 0, BWA_SETUP_SEATED = 1, BWA_SETUP_ROAMING = 2 } bwa_setup;
void bwa_tuning_preset(bwa_setup setup, bwa_tuning* out);   // pure, no engine handle
bool bwa_apply_tuning(bwa_engine* e, const bwa_tuning* t);  // one call, every live knob
bool bwa_get_tuning(bwa_engine* e, bwa_tuning* out);        // what the engine is rendering at
```

`bwa_get_tuning` is the readback half. It fills `struct_size`, so you can edit the result and hand it
straight back to `bwa_apply_tuning`. It reports what the engine will **render** rather than what
you passed: rt clamps several of these (the align guards, near and hole spread, SPCAP focus and
density, and an out-of-range panner or spread mode), and a readback whose job is to keep a UI honest
must not itself report an unsanitized value. This is not per-knob getters, which the live A/B surface
still deliberately lacks; it is one struct the ABI already owns.

Fourteen rendering knobs is a lot to get right. `bwa_setup` names how the **listener** uses the
install, `bwa_tuning_preset` fills a complete tuning for it, and `bwa_apply_tuning` pushes the lot.

Fill-then-apply rather than one `apply_preset(e, SEATED)`, deliberately. You can print what the preset
chose, which matters because most of these values are contested. Preset, override a field, apply,
composes where apply-then-re-override is order dependent. The preset is pure, so a tool can show the
table off-hardware, the same contract as `bwa_spcap_focus_default`. And two setups diff field by
field, which is the natural rig-day question.

`bwa_setup` is **orthogonal to `bwa_profile`**. Profile is what you render *to*; setup is how the
listener uses it. Seated-CAVE, roaming-CAVE and seated-binaural are all real. Create-time config is
not part of a preset: `bwa_desc.bed_decoder` already defaults correctly, and the profile is yours.

**This struct's zero is NOT its default.** A zero-filled `bwa_tuning` would force max-rE off, which
stopped being the engine default. `struct_size` exists to make that fail loudly: `bwa_apply_tuning`
refuses a struct whose size is wrong, which is what a zero-init mistake looks like. Always start from
`bwa_tuning_preset`.

### What each field rests on

Read this before treating a preset as a recommendation. Where there is no evidence, the field stays
at the engine default rather than a guess.

| Field | Seated | Roaming | Evidence |
| --- | --- | --- | --- |
| `panner` | SPCAP | DBAP | **Intent.** SPCAP is the documented fixed-observer default; DBAP re-solves per block for a moving one |
| `dual_band` | on | off | **Intent.** Sweet-spot dependent, which a seat satisfies and roaming does not |
| `dual_band_cap` | on | off | **Intent, unmeasured on hardware.** Its stated target is the seated case; the ITD claim needs the rotating two-mic rig |
| `max_re` | on | on | **Measured.** Won every axis of the offline bed sweep, under both decoders and both observer models |
| `spread_mode` | LOBE | LOBE | **Measured.** MDAP 19.1 deg against LOBE's 11.9 |
| `decorrelation` | off | off | **Measured.** Worse on both axes at the sweet spot when swept properly |
| `hole_spread` | 0 | 0 | **Measured.** Costs direction, and its benefit is invisible to these estimators |
| `near_spread` | 0 | 0 | Measured mixed, and the useful radius is install specific |
| `tracked_align` | off | off | **Measured, and the largest effect there is** (comb 8.54 to 0.80 dB off-center). It needs a *calibrated* layout and measures worse without one, and a preset cannot know whether you surveyed. Turn it on after `bwa_calibrate` |
| `max_re_split` | off | off | No evidence either way |
| `bed_renderer` | MATRIX | MATRIX | Parametric's claim needs a walking listener, which nothing has measured |
| `tracked_room_eq` | on | on | Engine default, and a no-op without a `room_eq_grid` |
| `spcap_focus` / `_density` | 0 | 0 | 0 means the array-derived default |
| `align_*` guards | 0 | 0 | 0 means the built-in guards |

Seated and roaming differ in exactly **three** fields today. That is not an oversight, it is what the
evidence supports. The gap should widen after the rig day, and `smoke` asserts the count so a fourth
opinion cannot arrive unannounced.

## Output protection limiter (control thread; ON by default)

```c
void bwa_set_limiter(bwa_engine* e, bool on);                     // live
void bwa_set_limiter_ceiling(bwa_engine* e, float linear);        // linear peak ceiling in (0..1]; default 0.891251 (-1 dBFS)
```

The final stage on the speaker output. Everything (voices, beds, the reflection/pathing taps, the
per-speaker align stage, the test signal) passes through it before the device.

It is **linked** across channels: one gain, derived from the cross-channel peak, so engaging never
shifts the spatial image. ~1 ms attack / ~120 ms release one-poles, then a hard clamp at the
ceiling. The attack is not lookahead, so the first millisecond of a hot transient clips instead of
overshooting. The ceiling is a **linear** peak amplitude in `(0..1]` (like every other gain in the
ABI), so `0.891251` is −1 dBFS; a value above 1 clamps to 1 and a non-positive value is ignored.

This is driver/speaker **protection** against digital overs and pathological content, not a
mastering limiter. If it engages in normal use, turn the content down. In `cave_sim`/`cave_both`
the same limited bus feeds the monitor, so headphones inherit the ceiling too. In `binaural` only
the diffuse layer passes through it: the direct field bypasses the speaker bus, so the engine
hard-clamps the final stereo at ±1 instead (no ramped limiting on the direct path; content that
clips there is too hot).

## Headphone correction EQ (control thread)

```c
bwa_result bwa_load_headphone_eq(bwa_engine* e, const char* path);   // load-class: file I/O; NULL/"" clears
void       bwa_set_headphone_eq (bwa_engine* e, bool on);            // the ramped live A/B; default on
```

The headphone-side align stage. The HRTF decode assumes acoustically transparent headphones;
real ones color the signal, so a serious binaural chain EQs the headphone flat first. This
corrects the **transducer**, not the render: the cascade runs on the final device-bound stereo
of every headphone profile (`binaural`, `cave_sim`, `cave_both`'s monitor tap) after the HRTF
decode and before the output clamp, so A/B-ing the two renders stays fair, and the array
render never sees it (speakers get the per-speaker align stage instead). In `cave` it is inert.

The format is **AutoEq's `ParametricEQ.txt`**
([github.com/jaakkopasanen/AutoEq](https://github.com/jaakkopasanen/AutoEq): measured,
target-compensated corrections for thousands of headphone models):

```
Preamp: -6.4 dB
Filter 1: ON PK Fc 105 Hz Gain -4.6 dB Q 0.70
```

`PK`/`LSC`/`HSC` map onto the engine's RBJ biquads; `LS`/`HS` are accepted, a missing Q
defaults to 0.707, `OFF` filters and unknown lines are skipped. The `Preamp` line is honored:
corrections **boost** dips, and the preamp is the headroom that keeps them out of the clamp.
A malformed `Filter` line, an unreadable file, or a file with no filters fails with
`BWA_ERR_CONFIG` (`bwa_last_error` has the reason) and **keeps the previous EQ**.

Loading is click-free: a running correction ramps out, the new one ramps in. The toggle
crossfades the same way: flip it mid-listen to hear what the correction does. State does not
survive `bwa_destroy`: reload after rebuilding an engine (the playground's EQ field does this
for you across its render/driver rebuilds). Personalized **HRTFs are the other half**: SOFA via
`bwa_desc.hrtf_path` corrects your ears, this corrects your headphones; they compose.

## Reflection bed (control thread)

```c
typedef struct { float ir_seconds; uint32_t order, num_rays, num_bounces; int enabled, bake; uint32_t reserved[3]; } bwa_reflections_desc;
void bwa_reflections_config   (bwa_engine* e, const bwa_reflections_desc* cfg);  // LOAD-TIME (before bwa_start)
void bwa_set_reverb_gain (bwa_engine* e, float linear);                   // the wet level: live, default 1
void bwa_source_set_reverb(bwa_engine* e, bwa_source s, bool on);           // per-frame; gates the wet send
void bwa_source_set_reverb_send(bwa_engine* e, bwa_source s, float gain);    // per-source wet-send level (default 1)
void bwa_source_set_reverb_distance(bwa_engine* e, bwa_source s, bool on);   // far = wetter
```

A single shared **listener-centric reverb bed**, decoded straight to the speaker channels and
summed onto the bus. It runs Steam Audio's **HYBRID** reverb: directional early-reflection
convolution (full ambisonic, order = `order`) plus a parametric (FDN) tail, decoded across the
array.
(This needs the vendored phonon's alignment patch; see [materials.md](./materials.md).) No-op
without the Steam Audio build.

> **Which reverb should you use?** Three reflection/reverb implementations exist and they are
> complementary, not rival; the recommended configuration (Steam scene + ISM + FDN) never creates
> this bed at all. See [materials.md](./materials.md) → "Choosing an acoustics path". Do **not**
> run this bed and the ISM together: the bed already contains early reflections, so you would
> hear them twice (the engine warns once through `bwa_last_error`).

Configuration is load-time; the sends are live:

- **`bwa_reflections_config`** must land before `bwa_start`: the IR length and ambisonic order are
  baked there. Zero fields take defaults (`ir_seconds` 1.0, `order` 1, `num_rays` 4096,
  `num_bounces` 16). `enabled = 0` creates no bed; the engine behaves exactly as without one.
  `bake` non-zero precomputes the reverb over a probe grid at `bwa_start`, so the sim thread looks
  it up instead of ray-tracing live (static scenes only, which the bed requires anyway).
- **`bwa_set_reverb_gain`** is the wet level: the one control, live (a single atomic the
  audio-thread tap reads), default 1. A value set before `bwa_start` seeds whichever reverb bed it
  creates (this bed or the FDN).
- **`bwa_source_set_reverb`** opts a source into the bed's wet send. Per-frame, non-blocking;
  with the bed disabled (or no SDK) it gates a send that goes nowhere.
- **`bwa_source_set_reverb_send`** sets that source's send level (default 1.0). Drive it
  yourself for a manual dry/wet.
- **`bwa_source_set_reverb_distance`** adds automatic **distance→wet** scaling on top:
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
void bwa_fdn_set_decay(bwa_engine* e, float rt60_low_s, float rt60_high_s, float xover_hz);  // LIVE
```

A **phonon-free** late-reverb alternative that takes the reverb tap *instead of* the Steam bed
(one reverb bed at a time; with the FDN enabled the Steam bed is skipped). It consumes the same
mono aux send, so `bwa_source_set_reverb` and the per-source send levels apply unchanged, and
`bwa_set_reverb_gain` sets its return level live.

Inside: a 16-line **feedback delay network** (Householder feedback; lossless prototype, the decay
filters are the only loss), each line assigned a direction on the sphere and rendered as a plane
wave through the layout's SH→speaker bed decode. `rt60_low_s`/`rt60_high_s`/`xover_hz` set a
two-band decay (defaults 1.2 s low / 0.7 s high at 2 kHz). `decay_dir` + `decay_factor` make the
decay **anisotropic**: the field dies faster (factor < 1) or slower toward that direction, the
diagonal special case of the Directional FDN (Alary/Politis/Schlecht, JAES 2019). Use it to
*design* a space (an open side, a treated wall); do **not** match the real room's RT60 (see
[calibration.md](./calibration.md)).

**The decay is live.** `bwa_fdn_set_decay` retunes the two-band decay while the engine runs: the
FDN ramps its per-line loss gains to the new values across one block (~5 ms), so the tail keeps
ringing and only its *slope* changes: no click, no restart. This is what a room transition sounds
like: stepping into a cathedral, submerging (long low band, dead high band), walking out into open
air. `<= 0` keeps a parameter's current value; pre-start it just updates the staged config, so a
scene can call it unconditionally. Structure (enable, anisotropy) stays load-time; the wet level is
`bwa_set_reverb_gain`, as always.

Deterministic CPU (no rays, no IRs, infinite tail), works in no-SDK builds: the reverb path no
longer requires the Steam Audio SDK.

### Image-source early reflections (no SDK needed)

```c
void bwa_scene_set_box(bwa_engine* e, float w, float h, float d, const bwa_material faces[6]);  // the room
void bwa_scene_set_ground(bwa_engine* e, float y, bwa_material mat, bool pressure_release);     // or a bare plane
void bwa_scene_set_pressure_release(bwa_engine* e, uint32_t face_mask);      // flag inverting faces (water)
void bwa_source_set_early_reflections(bwa_engine* e, bwa_source s, bool on);   // per source; per-frame-safe
void bwa_set_early_reflections_gain(bwa_engine* e, float linear);            // live; default 1
```

The other half of the phonon-free acoustics path. The FDN renders the late diffuse tail; this
renders the **six first-order specular reflections**, each a real point source at its mirrored
position, panned through the engine's own listener-relative panner: correct direction *and
parallax as the listener walks*, which no shared listener-centric bed can give
([spatialization.md](./spatialization.md#early-reflections-image-sources-panned-like-point-sources-ismc)).
Path delay, distance attenuation, and per-band wall absorption fall out of the geometry; delays
glide and gains ramp, so a moving source bends its reflections instead of stepping them.
`bwa_scene_set_box` captures the shoebox **whether or not** the Steam build is present.

**Order 1 only, by design.** Higher orders blend into the diffuse field within tens of
milliseconds, which is exactly what the FDN already renders, for free and with a proper decay.
A source outside the room renders dry.

Two room variants beyond the box: `bwa_scene_set_ground` renders just the **ground bounce**
(outdoor scenes: one plane, one image, mirrors a source on either side), and
`bwa_scene_set_pressure_release` flags faces that reflect **inverted**: a water surface seen from
below has reflection coefficient ≈ -1, so the flipped image cancels the direct sound near the
boundary (the Lloyd's-mirror comb). See "Materials and scene geometry" for both, and the
physical-emulation recipes (under "How-to guides") for the underwater assembly.

Cost: six panner solves per opted-in voice per block (one for the ground plane), plus the delay
taps per sample. Opt in on the sources that matter (a few), not on everything.

## Handle scheme

`bwa_sound`, `bwa_source`, and `bwa_bed` are opaque `uint32_t` = `(index | generation<<16)`. A stale
handle (slot destroyed, then reused) fails the generation check on the audio side and is
silently dropped rather than crashing. `0` is always invalid. Treat handles as tokens; never
do arithmetic on them. The one deliberate deviation: `bwa_scene_add_dynamic_mesh` returns a plain
small `int` index (`-1` = failure); movers are few and app-managed, so they skip the
generation machinery.
