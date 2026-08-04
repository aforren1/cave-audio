# bw_audio

Self-hosted spatial audio engine for a 26-speaker CAVE installation. Drives the
array over **ASIO → RME Digiface Dante**, with **binaural HRTF headphone
rendering**: a first-class direct render, plus an array-audition monitor for
desk-side debugging. Unity and Unreal connect as thin control clients over a C ABI.

## Why self-hosted

Going direct (no FMOD/Wwise) gives sample-accurate access to ASIO timing hooks and
keeps the core engine-agnostic, so the same library serves both engines with only a
thin per-engine glue layer. The spatializer, mixer, output, and tracking all live
in one process behind one audio callback: the experiment deploys as one system on
the machine running the endpoint, with no external renderer to author, synchronize, and
maintain alongside it.

## Shape

```
 engine (Unity/Unreal) ──control──┐
 OptiTrack (NatNet) ─────pose─────┤
                                  ▼
                     ┌─ voice playback ─ DBAP pan ─► speaker bus ──────┐
                     │  (one audio callback)                           │
                     └─────────────────────────────────────────────────┘
                                  │                         │
                          ASIO ► Digiface ► array        HRTF decode ► stereo
                          (production)              (binaural render / array sim)
```

## Engine features

- **Spatialization**: per-voice listener-relative DBAP (SPCAP/VBAP selectable, a
  dual-band option), recomputed per block from the tracked head position; source
  extent as an angular spread (lobe or MDAP virtual-source ring) or a **metric
  size in meters** that holds constant as the listener walks, with optional
  velvet-noise **decorrelation** so wide sources don't collapse to phantom images;
  near-listener widening for fly-throughs; a **multi-listener compromise** mode
  (energy-mean over up to 4 occupants); per-speaker gain/delay/correction-EQ
  output stage, master gain, and a linked protection limiter as the final stage.
- **Acoustics**: ray-traced occlusion with per-band transmission EQ, source
  directivity, a directional reflection bed (real-time, or baked over a probe
  grid), and sound pathing with bending-loss EQ via Steam Audio, **plus a
  complete SDK-free path**: geometric **image-source early reflections** (each
  wall bounce panned as a point source, so it has parallax as you walk) into a
  **directional FDN reverb** (anisotropic decay), with **manual occlusion** driven
  from game logic.
- **Propagation**: distance attenuation, Doppler, air absorption, equal-loudness
  compensation, playback **pitch**; opt-in per source, ramped/glided.
- **Assets**: WAV/FLAC/MP3, decoded and resampled at load; disk streaming for long
  files; AmbiX ambisonic beds: matrix decode (AllRAD or EPAD) or a
  **parametric DirAC-style renderer** whose direct stream re-pans listener-relative
  (a walkable soundfield), with yaw rotation to line a capture up with the scene.
- **Voices**: fixed pool with priority stealing; pause (per-voice, per-group, and
  global) and click-free seek; engine-side timed fades; **mix groups** for
  category gain/ducking; sample-accurate start and scheduled click-free stop
  against a device-anchored DSP clock (`bwa_source_play_at` / `bwa_source_stop_at`
  / `bwa_get_dsp_time`); **intro→loop** regions (`bwa_source_play_loop`) and
  **gapless chaining** into queued sounds (`bwa_source_queue`). Starts and stops are
  click-free (one-block gain ramps either way).
- **Tracking**: OptiTrack NatNet parsed off-wire; the audio thread samples the
  freshest head pose at block time, with optional **pose prediction** to hide
  motion-to-ears latency; **tracked room EQ** interpolates measured LF room
  correction at the live listener position.
- **Headphones**: a first-class **direct binaural render** (`binaural` profile:
  point sources SH-encode at their true listener-relative directions → one Steam
  Audio HRTF decode, no speaker-array simulation in the direct path) and an
  **array-audition monitor** (`cave_sim`: the same speaker bus through virtual
  speakers, DBAP artifacts included), both to any 2-ch ASIO device; per-channel
  test signal, output meters, voice gauge.
- **Real-time discipline**: no allocation, locks, or I/O on the audio thread;
  lock-free SPSC command/event rings; `bwa_commit` gives frame-coherent updates;
  every parameter change ramps; nothing steps.
- **Validation**: the core math (SH encode, VBAP, AllRAD, EPAD, biquads, EQ rendering)
  is cross-checked against independent implementations (scipy/qhull/linear-
  programming goldens) in CI, alongside the DSP/concurrency test suite and
  UI-driven tests for all three tools.

## Recommended settings per setup

The defaults target the CAVE: **one tracked listener roaming the array.** Change the
setup and the right settings change with it, mostly because a *sweet spot* either
exists or it doesn't. These are starting points; A/B them by ear in the playground.
The calibration commands behind the last row are under
[One array, several audiences](#calibrate-bwa_calib_view).

| | **tracked roamer** (the CAVE) | **fixed seat** (one chair) | **audience** (several people) | **desk** (headphones) |
|---|---|---|---|---|
| profile | `cave` | `cave` | `cave` | `cave_sim` (verify the array) / `binaural` (best headphone render) |
| panner | **DBAP** (default) | **VBAP**, else SPCAP | **DBAP** | any (DBAP) |
| tracking | `bwa_tracker_connect` | none - listener sits at the seat | track the main occupant + `bwa_set_extra_listeners` for the rest | push head pose, or track |
| dual-band | off | **on** | off | your call (A/B it) |
| calibration | `--eq` + `--room-eq-grid` | `--eq` + `--room-eq` at the seat | `--eq` only | `--eq` |
| bed decoder | AllRAD (default); A/B EPAD | AllRAD | AllRAD; A/B EPAD | either |

**Tracked roamer.** DBAP is listener-relative and re-solves every block, so the image
follows you instead of degrading away from a center. Leave dual-band and VBAP off:
both sharpen the image *at a sweet spot*, and there isn't one. For room correction use the
grid (`bwa_calibrate --room-eq-grid`, one run per mic position); the engine interpolates
the LF cuts at your live position and glides the biquads. `bwa_set_pose_prediction` (start
~20–40 ms, your measured motion-to-ears latency) hides the panning lag; `bwa_set_decorrelation`
keeps wide sources from comb-filtering as you move. Loading a static `room_eq` layout into
a moving session **fails `bwa_start`** on purpose: one measurement point cannot correct a roam.

**Fixed seat.** Now a sweet spot exists, so spend it: VBAP for the sharpest image (it needs
a cleanly triangulable array; it falls back to DBAP if not), SPCAP if the array is uneven
or you want a smoother, all-speaker image. Turn **dual-band on** for tighter bass
localization, and calibrate with `--room-eq` **with the mic at the seat**. Don't track;
set the listener pose once.

**Audience.** Panning is exact for one head and wrong for everyone else, so this is a
compromise by construction. Track the person who matters (the participant, the demo
driver) and hand the *other* occupants' positions to **`bwa_set_extra_listeners`** (up to
3): every source's gains become the per-speaker energy mean of the per-listener solves,
so each occupant gets an image biased toward their own seat instead of one exact and N
wrong. Otherwise play it safe rather than sharp: DBAP, dual-band off, and no room EQ
beyond `--eq` (which flattens the *speakers*, not the room, so it helps every seat).
Raise `bwa_source_set_spread` on ambience: wide sources survive off-center listening far
better than points do.

**Desk.** Neither headphone profile needs a layout, Dante, or hardware beyond
headphones. `cave_sim` renders the *same* speaker mix through virtual speakers, so
what you hear is the array render: use it to verify what the room will do.
`binaural` renders sources directly at their true directions: the better *listen*,
the wrong *probe*. Don't use either to judge timbre for the room.

**Everywhere:** the output limiter is on at −1 dBFS (leave it; it's speaker protection, not
mastering), and reflections are opt-in per source. Pick one reverb bed: Steam Audio's
(needs the SDK, ray-traced from your geometry) *or* the phonon-free FDN (`bwa_fdn_config`,
a designed decay: cheaper, works in no-SDK builds). Details in
[`docs/spatialization.md`](./docs/spatialization.md) and
[`docs/calibration.md`](./docs/calibration.md).

## Non-goals and current limitations

bw_audio is not middleware. There are no events, banks, mixer graphs, or authoring
app: the ABI is create/play/position/commit. Game-side audio (UI, menus) stays in
the game engine's own mixer.

- **Fixed target.** The speaker geometry, including the channel count, is data:
  a layout file carries 4..26 speakers and the engine's channel count follows it
  (26 is the compile-time capacity; collaborator arrays with fewer speakers load
  into the same binary). Still not a general 5.1/Atmos renderer.
- **Windows + ASIO only.** One *tracked* listener: the multi-listener mode is a
  panning compromise for extra occupants, not per-head rendering.
- **Room EQ is opt-in.** Static-listener correction at one point
  (`bwa_calibrate --room-eq`, fixed-seat installs), or **tracked room EQ** from a
  measured grid (`--room-eq-grid`) for a roaming listener: LF modal cuts only;
  mid/HF stays speaker-only correction, because one room can't be flattened for
  every position at once.

**Steam Audio is optional.** A no-SDK build is fully viable for the array: the whole
spatializer plus geometric early reflections, FDN reverb, and manual occlusion. The
SDK adds ray-traced (automatic) occlusion, sound pathing, and the real HRTF monitor;
that last one is a *developer-workstation* dependency, since the production array
render never uses HRTF. Which reverb/reflection path to run is a genuine choice;
[`docs/materials.md`](./docs/materials.md) has the comparison and the recommendation.

Current gaps (may change):

- Early reflections model a **shoebox** (the image-source path); arbitrary virtual
  geometry needs the Steam scene.
- Mono point sources only. Stereo assets downmix; the ambisonic bed is the only
  non-point path.
- No completion callbacks; poll `bwa_source_is_playing`.
- No OGG/Opus; seek and pitch do not apply to streamed sounds.
- Reflection/room configuration is load-time (occlusion meshes can be replaced live).

## Getting started

Build: no vendored dependencies required (without the ASIO SDK you get the
silent offline sink; [`docs/build.md`](./docs/build.md) has the device/SDK setup):

```
cmake -S . -B build -A x64
cmake --build build --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo
```

Usage docs live in [`docs/api.md`](./docs/api.md): quickstart, profiles, the
threading contract, coordinates, how-to guides for the common setups (a room
with reflections, audio-visual sync, desk versus rig), error handling,
environment variables, then a per-call reference. [`examples/minimal.c`](./examples/minimal.c) runs the whole
client lifecycle (create → load → play → per-frame commit → teardown); it builds
as `bwa_minimal` and needs no hardware.

```c
bwa_desc cfg = { .profile = BWA_PROFILE_BINAURAL, .sample_rate = 48000, .block_size = 256 };
bwa_engine* e = bwa_create(&cfg);
bwa_start(e);                                    // no ASIO device? silent sink, keeps running

bwa_sound ping = bwa_load_sound(e, "ping.wav");    // WAV/FLAC/MP3, resampled at load
bwa_source s   = bwa_source_create(e);
bwa_source_play(e, s, ping, /*loop*/ true);

// per frame, from one thread:
bwa_set_listener_pose(e, px,py,pz, qx,qy,qz,qw);
bwa_source_set_pos(e, s, x, y, z);
bwa_commit(e);
```

## Documentation

| doc | covers |
|-----|--------|
| [`docs/api.md`](./docs/api.md) | usage guide + per-call reference |
| [`include/bw_audio.h`](./include/bw_audio.h) | the C ABI |
| [`docs/architecture.md`](./docs/architecture.md) | system overview, the bus seam, locked decisions |
| [`docs/concurrency.md`](./docs/concurrency.md) | threading model, rings, commit snapshot, lifetimes |
| [`docs/spatialization.md`](./docs/spatialization.md) | DBAP/SPCAP/VBAP, dual-band, binaural decode, alignment |
| [`docs/materials.md`](./docs/materials.md) | occlusion, reflections, sound pathing |
| [`docs/integration.md`](./docs/integration.md) | Unity/Unreal bindings, coordinate seam |
| [`docs/layout-schema.md`](./docs/layout-schema.md) | `cave_layout.json` format |
| [`docs/calibration.md`](./docs/calibration.md) | trims, EQ, acoustic survey, room report |
| [`docs/build.md`](./docs/build.md) | platform, dependencies, licensing, Dante config |

Contributor-facing notes live in [`docs/internal-types.md`](./docs/internal-types.md),
[`docs/profiling.md`](./docs/profiling.md), and `CLAUDE.md` (agent working notes, not
user documentation).

## Tools

Opt-in, in workflow order (design → calibrate → audition):

```
cmake -S . -B build -DBWA_BUILD_PLAYGROUND=ON -DBWA_BUILD_CALIBVIEW=ON -DBWA_BUILD_CALIBRATE=ON
cmake --build build --config RelWithDebInfo
```

### Design: `bwa_layout_tool`

![bwa_layout_tool](docs/img/layout_tool.png)

Authors `cave_layout.json`:

- Place the speakers (4–26; the count control sets the array size, and the file's
  count *is* the engine's channel count). A speaker's index is its output channel,
  so the built-in test tone tells you which physical speaker is which.
- Load placement constraints from `constraints.json`.
- Shade a coverage shell by nearest-speaker gap, or by the selected panner's
  rE-localization error, computed with the engine's own gain solve.
- Optionally hill-climb the positions against that error.
- Preview a moving pink-noise source through the edited layout.

Headless: `--export`, `--score`, `--optimize`.

### Calibrate: `bwa_calib_view`

![bwa_calib_view: layout diff](docs/img/calib_view_diff.png)

The Capture tab runs sweep → measure → solve → writeback (simulated, or full-duplex
ASIO with a measurement mic), then loads the result into a layout diff: A the
input, B what was written. A swapped channel or a bad mic placement is caught
before the file is trusted.

Other tabs: the array in 3D, gain/delay trims, correction-EQ curves, retained IRs.
The Zylia tab shows clap direction-of-arrival on a ZM-1 capsule sphere: a
seconds-fast check of capsule mapping and geometry.

![bwa_calib_view: Zylia tab](docs/img/calib_view_zylia.png)

`bwa_calibrate` is the same pipeline headless (`--simulate`, `--localize`);
`bwa_zylia_probe` is a console level meter. See
[`docs/calibration.md`](./docs/calibration.md).

**One array, several audiences.** Speaker positions are surveyed once, but trims and
EQ are measured relative to a reference point, so one installation can keep several
calibrated variants of the same geometry and pick one per session
(`bwa_desc.layout_path` + the panner):

```
bwa_calibrate --layout survey.json --mic 0 1.2 0 --room-eq      --out cave_layout.seated.json
bwa_calibrate --layout survey.json --mic 0 1.7 0 --eq           --out cave_layout.roaming.json
bwa_calibrate --layout cave_layout.roaming.json --mic -1 1.7 0 --room-eq-grid   # then rerun per mic spot
```

- **Seated** (SPCAP/VBAP, fixed listener pose): trims aligned at the seat, room
  correction at the seat. `--room-eq` is only valid for a listener who stays at the
  measurement point.
- **Roaming** (DBAP + tracking): trims aligned at the working-volume center at
  standing ear height, speaker-only EQ; one point can't room-correct a roam.
- **Roaming + tracked room EQ**: `--room-eq-grid` accumulates LF modal cuts one mic
  placement at a time (the `--mic` position is the grid key); the engine then
  interpolates the cuts at the live tracked position: grid-based room correction
  that survives a walk.

Diffing the two files in calib_view should show identical positions and only trim/EQ
differences. Unknown JSON fields survive recalibration, so a variant can carry its
own annotation (for example, `"intent": "seated, SPCAP"`). And loading the seated file into
a moving-listener session (DBAP or tracking) fails `bwa_start` rather than quietly
mis-correcting the array.

### Audition: `bwa_playground`

![bwa_playground](docs/img/playground.png)

The array sim (`cave_sim`) on headphones by default (auto-picked 2-ch ASIO driver;
without one the engine falls back to the null sink and keeps rendering, visual-only,
live, just silent). The panel's render picker switches to `binaural` (the direct
per-source render, for by-ear A/Bs against the sim) or `cave` (the array itself over
26-ch ASIO, the same harness pointed at real speakers on the rig machine), and a
headphone-EQ field loads an AutoEq correction for your headphones.
Scenes: localization, occlusion + materials, directivity, channel walk,
reverb bed, an underwater medium boundary (live FDN retune, speed of sound,
the Lloyd's-mirror surface bounce), and a blind A/B/X comparison over single
engine knobs (dual-band, panner choice, spread, air absorption) scored with a
binomial p-value. The 3D
speakers shade by their live output level (mirrored as a meter strip in the
panel), so you can watch the panner drive the array even with no audio device. A
`constraints.json` next to the exe is drawn for orientation: the same room
boxes the layout tool edits against.

All three GUI tools run their UI test suites under ctest (`--tests`); screenshots
above are from those runs.

## Platform and licensing

Windows-only (ASIO). **GPLv3** ([`LICENSE`](./LICENSE)); third-party components
keep their own licenses; see [`docs/build.md`](./docs/build.md).
