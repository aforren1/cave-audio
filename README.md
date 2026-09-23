# bw_audio

Self-hosted spatial audio engine for a CAVE installation. It drives a speaker array (up
to 64 channels; the CAVE has 26) over ASIO into an RME Digiface Dante. A second path
does **binaural HRTF headphone rendering**: a first-class direct render, plus an array-audition monitor
for desk-side debugging. Game engines and experiment frameworks connect as thin
control clients over a C ABI. No rendered audio crosses that boundary, only control.

New to the vocabulary? [docs/glossary.md](docs/glossary.md) defines every term the docs use,
grouped by topic. Each entry says what the term means for a decision.

**Hear it now.** The engine runs in a browser: [aforren1.github.io/cave-audio](https://aforren1.github.io/cave-audio/)
is the minimal orbit and the six-scene playground, the same WebAssembly build of the same
code, on headphones. Chromium is what it is tested in. Every push to main redeploys it.

## Status

Every subsystem is implemented and covered off hardware: the ctest suite, cross-validation
goldens against independent implementations, the simulate modes of the calibration and
validation tools, and off-wire parser tests.

**Nothing is verified on the rig yet.** Do not read a green test run as a working
26-speaker array. These parts need the hardware:

- the 26-channel ASIO path into the Digiface, and the full-duplex capture that both
  measurement tools use,
- live Motive tracking. The NatNet parser and its lifecycle are tested off-wire only.
- the Zylia channel order and azimuth reference. Both survive every off-hardware check,
  and both report a confident wrong direction if they are wrong.
- the by-ear checks: HRTF quality, the A/B/X knob bake-off, room EQ on the array.

[`docs/hardware-validation.md`](./docs/hardware-validation.md) is the ordered runbook for
that day. The Unreal binding is planned and not written.

## Shape

```
 control client (Unity / Godot / Python / MATLAB) ──control──┐
 OptiTrack (NatNet) ─────────────────────────────────pose────┤
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
  a hole-aware spread floor for directions the array has no speaker in;
  near-listener widening for fly-throughs; a **multi-listener compromise** mode
  (energy-mean over up to 4 occupants); per-speaker gain/delay/correction-EQ
  output stage, master gain, and a linked protection limiter as the final stage.
  Two modifiers follow the head rather than the layout: **CAP** matches the rendered
  interaural time difference to a real source for the head's current orientation
  (`bwa_set_dual_band_cap`), and **tracked alignment** re-aims the per-speaker trims
  and delays onto the live head (`bwa_set_tracked_align`).
- **Acoustics**: ray-traced occlusion with per-band transmission EQ, source
  directivity, a directional reflection bed (real-time, or baked over a probe
  grid), and sound pathing with bending-loss EQ via Steam Audio, **plus a
  complete SDK-free path**: geometric **image-source early reflections** (each
  wall bounce panned as a point source, so it has parallax as you walk) into a
  **directional FDN reverb** (anisotropic decay, live decay retune), with **manual
  occlusion** driven from game logic.
- **Propagation**: distance attenuation, Doppler, air absorption, equal-loudness
  compensation, playback **pitch**; opt-in per source, ramped/glided.
- **Assets**: WAV/FLAC/MP3, decoded and resampled at load; disk streaming for long
  files; **push sources** that take caller-generated PCM into the engine
  (`bwa_source_push`, a source feed on the control thread, never a render path);
  an optional **shared-ownership cache** (`bwa_sound_acquire` / `bwa_sound_release`)
  that dedupes by path plus load flags and refcounts, with **async loading**
  (`bwa_sound_acquire_async`) for content that arrives mid-session; AmbiX
  ambisonic beds: matrix decode (AllRAD or EPAD) or a
  **parametric DirAC-style renderer** whose direct stream re-pans listener-relative
  (a walkable soundfield), with yaw rotation to line a capture up with the scene.
- **Voices**: fixed pool with priority stealing; pause (per-voice, per-group, and
  global) and click-free seek; engine-side timed fades; **mix groups** for
  category gain/ducking; sample-accurate start and scheduled click-free stop
  against a device-anchored DSP clock (`bwa_source_play_at` / `bwa_source_stop_at`);
  **intro to loop** and **play regions** that loop a sub-range or cut a one-shot
  short; **gapless chaining** into queued sounds (`bwa_source_queue`); **end and
  loop-wrap events** (`bwa_poll_ended`, `bwa_poll_looped`) you drain after each
  commit, because a clip shorter than your frame can start and finish unseen and a
  looping voice never ends at all. Starts and stops are click-free, including the
  scene-transition sweeps (`bwa_group_stop`, `bwa_stop_all`). A source's whole
  configuration is one printable, diffable struct (`bwa_source_desc`) you fill from
  a preset and apply in one call, instead of a setter per knob.
- **Tracking**: OptiTrack NatNet parsed off-wire; the audio thread samples the
  freshest head pose at block time, with optional **pose prediction** to hide
  motion-to-ears latency; **tracked room EQ** interpolates measured LF room
  correction at the live listener position.
- **Headphones**: a first-class **direct binaural render** (`binaural` profile:
  point sources SH-encode at their true listener-relative directions into one Steam
  Audio HRTF decode, no speaker-array simulation in the direct path) and an
  **array-audition monitor** (`cave_sim`: the same speaker bus through virtual
  speakers, DBAP artifacts included). Both open a normal 2-channel output device:
  WASAPI on Windows, JACK or ALSA on Linux, AAudio on Android. `cave_both` runs the
  array over ASIO and the monitor over WASAPI at the same time. **Headphone
  correction EQ** loads an AutoEq `ParametricEQ.txt` and swaps in click-free
  (`bwa_load_headphone_eq`).
- **Real-time discipline**: no allocation, locks, or I/O on the audio thread;
  lock-free SPSC command/event rings; `bwa_commit` gives frame-coherent updates;
  every parameter change ramps; nothing steps. Zone profiling is built in behind
  `-DBWA_TRACY=ON` (Tracy) or `-DBWA_PROFILE_SELF=ON` (headless, no server tooling).
- **Offline and diagnostics**: the manual sink drops its thread and lets you pump
  blocks yourself (`bwa_render_block`), bit-identically on every platform. That is the
  pre-render path for an experiment that plays its stimuli through something else, and
  it is what the test suite drives. Plus a per-channel test signal, output meters, a
  voice gauge, and a **direct single-speaker route** (`bwa_source_set_channel`) that
  plays a real source out of one speaker with no spatial processing, the ground-truth
  condition to A/B a phantom against.
- **Validation**: the core math (SH encode, VBAP, AllRAD, EPAD, biquads, EQ rendering)
  is cross-checked against independent implementations (scipy/qhull/linear-
  programming goldens) in CI, alongside the DSP/concurrency test suite and UI-driven
  tests for all three tools. `bwa_validate` grades the array itself: render a phantom,
  measure where it landed, report the angular miss and the comb depth.

## Control clients

The audio code is the same for all of them. They differ only in how they read transforms
and tracking at the boundary. See [`docs/integration.md`](./docs/integration.md).

| client | form | audience |
|---|---|---|
| [Unity](./bindings/unity/) | UPM package `com.brainworks.bw_audio`, P/Invoke plus `Engine` and `Emitter` components | games, the CAVE show |
| [Godot](./bindings/godot/) | GDExtension addon, one class per handle; the by-ear playground ships inside it | games, the CAVE show |
| [Python](./bindings/python/) | nanobind wheel, a raw 1:1 layer plus a Pythonic one | PsychoPy experiments |
| [MATLAB and Octave](./bindings/matlab/) | classic C MEX toolbox, one gateway on subcommand strings | Psychtoolbox experiments |
| [Web](./bindings/web/) | ES module over the WebAssembly build: a raw 1:1 layer plus an `Engine` layer, the control thread on a Worker, output through a Wasm Audio Worklet | browser demos and headphone experiments |
| Unreal | planned, notes only | games |

## Recommended settings per setup

The defaults target the CAVE: **one tracked listener roaming the array.** Change the
setup and the right settings change with it, mostly because a *sweet spot* either
exists or it doesn't. These are starting points. A/B them by ear in the playground.
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
both sharpen the image *at a sweet spot*, and there isn't one. Use the grid for room
correction (`bwa_calibrate --room-eq-grid`, one run per mic position). The engine
interpolates the LF cuts at your live position and glides the biquads.
`bwa_set_pose_prediction` (start at 20 to 40 ms, your measured motion-to-ears latency) hides
the panning lag. `bwa_set_decorrelation` keeps wide sources from comb-filtering as you
move. Loading a static `room_eq` layout into a moving session **fails `bwa_start`** on
purpose: one measurement point cannot correct a roam.

**Fixed seat.** Now a sweet spot exists, so spend it. Use VBAP for the sharpest image. It
needs a cleanly triangulable array, and it falls back to DBAP if not. Use SPCAP if the
array is uneven, or if you want a smoother, all-speaker image. Turn **dual-band on** for
tighter bass localization, and calibrate with `--room-eq` **with the mic at the seat**.
Don't track; set the listener pose once. If the seated listener turns their head, add
CAP on top of dual-band: it holds the rendered interaural time difference against that
rotation, and it reduces to the plain panner when the head faces the source.

**Audience.** Panning is exact for one head and wrong for everyone else, so this is a
compromise by construction. Track the person who matters. Hand the *other* occupants'
positions to **`bwa_set_extra_listeners`** (up to 3): every source's gains become the
per-speaker energy mean of the per-listener solves, so each occupant gets an image
biased toward their own seat instead of one exact image and N wrong ones. Otherwise play
it safe rather than sharp: DBAP, dual-band off, and no room EQ beyond `--eq` (which
flattens the *speakers*, not the room, so it helps every seat). Raise
`bwa_source_set_spread` on ambience: wide sources survive off-center listening far
better than points do.

**Desk.** Neither headphone profile needs a layout, Dante, or hardware beyond
headphones. `cave_sim` renders the *same* speaker mix through virtual speakers, so
what you hear is the array render. Use it to verify what the room will do.
`binaural` renders sources directly at their true directions: the better *listen*,
the wrong *probe*. Don't use either to judge timbre for the room. Load your
headphones' AutoEq correction with `bwa_load_headphone_eq` first, or you are judging
the headphones.

**Everywhere:** the output limiter is on at -1 dBFS. Leave it on; it is speaker
protection, not mastering. Reflections are opt-in per source. Pick one reverb bed: Steam
Audio's (needs the SDK, ray-traced from your geometry) *or* the phonon-free FDN
(`bwa_fdn_config`, a designed decay: cheaper, works in no-SDK builds). Details in
[`docs/spatialization.md`](./docs/spatialization.md) and
[`docs/calibration.md`](./docs/calibration.md).

## Non-goals and current limitations

bw_audio is not middleware. There are no events, banks, mixer graphs, or authoring
app: the ABI is create/play/position/commit. Game-side audio (UI, menus) stays in
the game engine's own mixer.

- **Fixed target.** The speaker geometry, including the channel count, is data.
  A layout file carries 4 to 64 speakers (`BWA_MAX_CHANNELS`) and the engine's channel
  count follows it, so a collaborator's array of any size in that range loads into the
  same binary. With no layout file, the engine runs a built-in 26-speaker grid
  (`BWA_DEFAULT_GRID`). Still not a general 5.1/Atmos renderer.
- **One *tracked* listener.** The multi-listener mode is a panning compromise for
  extra occupants, not per-head rendering.
- **The array is a Windows path.** ASIO is Windows-only and the Digiface is a Windows
  and macOS device. Everything else builds and runs everywhere (see
  [Platforms](#platforms-and-licensing)).
- **Room EQ is opt-in.** Static-listener correction at one point
  (`bwa_calibrate --room-eq`, fixed-seat installs), or **tracked room EQ** from a
  measured grid (`--room-eq-grid`) for a roaming listener: LF modal cuts only.
  Mid/HF stays speaker-only correction, because you cannot flatten one room for
  every position at once.

**Steam Audio is optional.** A no-SDK build is fully viable for the array: the whole
spatializer plus geometric early reflections, FDN reverb, and manual occlusion. The
SDK adds ray-traced (automatic) occlusion, sound pathing, and the real HRTF monitor.
That last one is a *developer-workstation* dependency, since the production array
render never uses HRTF. Which reverb/reflection path to run is a genuine choice.
[`docs/materials.md`](./docs/materials.md) has the comparison and the recommendation.

Current gaps (may change):

- Early reflections model a **shoebox** (the image-source path); arbitrary virtual
  geometry needs the Steam scene.
- Mono point sources only. Stereo and multichannel assets downmix; the ambisonic bed is
  the only non-point path.
- No completion callbacks. Ends and loop wraps come back as events you drain
  (`bwa_poll_ended`, `bwa_poll_looped`), never as a call into your code.
- No OGG/Opus. Seek, pitch, play regions, and the gapless queue apply to in-memory
  sounds only: a streamed or push source reads its ring in sequence and ignores them.
- Reverb bed configuration is load-time (`bwa_reflections_config`, `bwa_fdn_config`).
  Room geometry, occlusion meshes, and the FDN's decay do change live.

## Getting started

A default build vendors nothing by hand: dr_wav, dr_flac, dr_mp3, and cJSON are fetched
by CMake. Without the ASIO SDK you get the offline sink on Windows, and the library still
builds and links.

```
cmake -S . -B build -A x64
cmake --build build --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo
```

Two dependencies are yours to fetch and neither is committed here. The ASIO SDK (the
production array path) goes in `third_party/asiosdk`. Steam Audio is a git submodule at
`third_party/steam-audio-source`, built once by `tools/phonon/build-phonon.sh` and linked
statically, so there is no second file to ship.
[`third_party/README.md`](./third_party/README.md) has both recipes.

The bindings are opt-in flags on the same configure: `-DBWA_BUILD_GODOT=ON`,
`-DBWA_BUILD_PYTHON=ON`, `-DBWA_BUILD_MATLAB=ON`. To build them against an engine you
already have, install it as an SDK (`cmake --install build --prefix <sdk> --component
bwa_sdk`) and configure with `-DBWA_ENGINE_SDK=<sdk>`. That mode compiles nothing under
`src/`, so every binding links the same binary.

Usage docs live in [`docs/api.md`](./docs/api.md): quickstart, profiles, the threading
contract, coordinates, how-to guides for the common setups, error handling, environment
variables, then a per-call reference. [`examples/minimal.c`](./examples/minimal.c) runs
the whole client lifecycle and needs no hardware: it orbits a click around the listener's
head for six seconds. Every binding ships the same demo, so put headphones on and compare
them ([`docs/integration.md`](./docs/integration.md), "The minimal example").

```c
bwa_desc cfg = { .profile = BWA_PROFILE_BINAURAL, .sample_rate = 48000, .block_size = 256 };
bwa_engine* e = bwa_create(&cfg);
bwa_start(e);                                    // no output device? silent sink, keeps running

bwa_sound click = bwa_load_sound(e, "click.wav");  // WAV/FLAC/MP3, resampled at load
bwa_source s     = bwa_source_create(e);
bwa_source_play(e, s, click, /*loop*/ true);

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
| [`docs/signal-flow.md`](./docs/signal-flow.md) | the same signal-flow diagram, rendered |
| [`docs/concurrency.md`](./docs/concurrency.md) | threading model, rings, commit snapshot, lifetimes |
| [`docs/spatialization.md`](./docs/spatialization.md) | DBAP/SPCAP/VBAP, dual-band, binaural decode, alignment |
| [`docs/materials.md`](./docs/materials.md) | occlusion, reflections, sound pathing |
| [`docs/backends.md`](./docs/backends.md) | the sink contract and every device backend |
| [`docs/integration.md`](./docs/integration.md) | the five bindings, coordinate seam |
| [`docs/layout-schema.md`](./docs/layout-schema.md) | `cave_layout.json` format |
| [`docs/calibration.md`](./docs/calibration.md) | trims, EQ, acoustic survey, room report |
| [`docs/validation.md`](./docs/validation.md) | `bwa_validate`: grading the array's phantom images |
| [`docs/hardware-validation.md`](./docs/hardware-validation.md) | the rig-day runbook and its pass criteria |
| [`docs/glossary.md`](./docs/glossary.md) | one-line definitions for the whole vocabulary |
| [`docs/build.md`](./docs/build.md) | platform, dependencies, licensing, Dante config |
| [`docs/web.md`](./docs/web.md) | the WebAssembly target: toolchains, the worklet sink, the binding, hosting, what is measured |

Contributor-facing notes live in [`docs/internal-types.md`](./docs/internal-types.md),
[`docs/profiling.md`](./docs/profiling.md), and `CLAUDE.md` (agent working notes, not
user documentation).

## Tools

Opt-in, in workflow order (design, calibrate, audition). `bwa_layout_tool` and
`bwa_playground` build on Windows and Linux: they are raylib plus Dear ImGui and nothing
else. `bwa_calib_view` is a Windows-only target, because it is on the win32 and d3d11
ImGui backend and it links the full-duplex ASIO capture. So are the capture tools
(`bwa_calibrate`, `bwa_validate`, `bwa_zylia_probe`), for the ASIO half alone. The
browser playground under [`bindings/web/playground/`](./bindings/web/playground/) is a port
of `bwa_playground`'s scenes on three.js, minus the reverb bed; it is the one you can open
without building anything.

```
cmake -S . -B build -DBWA_BUILD_PLAYGROUND=ON -DBWA_BUILD_CALIBVIEW=ON -DBWA_BUILD_CALIBRATE=ON
cmake --build build --config RelWithDebInfo
```

### Design: `bwa_layout_tool`

![bwa_layout_tool](docs/img/layout_tool.png)

Authors `cave_layout.json`:

- Place the speakers (4 to 64; the count control sets the array size, and the file's
  count *is* the engine's channel count). A speaker's index is its output channel,
  so the built-in test tone tells you which physical speaker is which.
- Load placement constraints from `constraints.json`.
- Shade a coverage shell by nearest-speaker gap, or by the selected panner's
  rE-localization error. The tool computes that error with the engine's own gain solve.
- Optionally hill-climb the positions against that error.
- Preview a moving pink-noise source through the edited layout.

Headless: `--export`, `--score`, `--optimize`.

### Calibrate: `bwa_calib_view`

![bwa_calib_view: layout diff](docs/img/calib_view_diff.png)

The Capture tab runs sweep, measure, solve, and writeback (simulated, or full-duplex
ASIO with a measurement mic), then loads the result into a layout diff: A the
input, B what was written. You catch a swapped channel or a bad mic placement
before you trust the file.

Other tabs: the array in 3D, gain/delay trims, correction-EQ curves, retained IRs.
The Zylia tab shows clap direction-of-arrival on a ZM-1 capsule sphere: a
seconds-fast check of capsule mapping and geometry.

![bwa_calib_view: Zylia tab](docs/img/calib_view_zylia.png)

`bwa_calibrate` is the same pipeline headless (`--simulate`, `--localize`, `--zylia`);
`bwa_zylia_probe` is a console level meter. See
[`docs/calibration.md`](./docs/calibration.md).

**One array, several audiences.** You survey the speaker positions once. The trims and
EQ are relative to a reference point, so one installation can keep several calibrated
variants of the same geometry and pick one per session
(`bwa_desc.layout_path` + the panner):

```
bwa_calibrate --layout survey.json --mic 0 1.2 0 --room-eq      --out cave_layout.seated.json
bwa_calibrate --layout survey.json --mic 0 1.7 0 --eq           --out cave_layout.roaming.json
bwa_calibrate --layout cave_layout.roaming.json --mic -1 1.7 0 --room-eq-grid   # then rerun per mic spot
```

- **Seated** (SPCAP/VBAP, fixed listener pose): trims and room correction both at the
  seat. `--room-eq` is only valid for a listener who stays at the measurement point.
- **Roaming** (DBAP + tracking): trims aligned at the working-volume center at
  standing ear height, speaker-only EQ; one point can't room-correct a roam.
- **Roaming + tracked room EQ**: `--room-eq-grid` accumulates LF modal cuts one mic
  placement at a time (the `--mic` position is the grid key). The engine interpolates
  them at the live tracked position, so the correction survives a walk.

Diff the variants in calib_view: identical positions, only trim and EQ differences.
Unknown JSON fields survive recalibration, so a variant can carry its own annotation.
Loading the seated file into a moving-listener session fails `bwa_start`. It does not
quietly mis-correct the array.

### Grade: `bwa_validate`

Calibration fixes the array. This grades it. `bwa_validate` renders a phantom through
the real engine core, measures where a Zylia ZM-1 says it landed, and reports the
angular miss per direction, per panner, per listening position. Driving one speaker
alone gives a physical source through the same chain, so the miss reads against a floor
instead of in a vacuum, and every cell carries a comb depth beside its angle. Each live
A/B knob is a swept axis: dual-band, CAP, the spread modes, decorrelation, the
hole-aware floor, tracked alignment, and the SPCAP focus. `--simulate` runs the whole
flow with no hardware. Same `-DBWA_BUILD_CALIBRATE=ON` switch. See
[`docs/validation.md`](./docs/validation.md).

### Audition: `bwa_playground`

![bwa_playground](docs/img/playground.png)

The array sim (`cave_sim`) on headphones by default, out of the system default output
(WASAPI on Windows). The panel's device combo lists every backend's devices, so you can
pick an ASIO driver instead, and `--device <name or id>` does the same from the command
line (`--list-devices` prints them). With no device at all the engine falls back to the
null sink and keeps rendering, visual-only, live, just silent. The render picker switches
to `binaural` (the direct per-source render, for by-ear A/Bs against the sim) or `cave`
(the array itself over ASIO, the same harness pointed at real speakers on the rig
machine). A headphone-EQ field loads an AutoEq correction for your headphones.

Scenes: localization, occlusion + materials, directivity, channel walk,
reverb bed, an underwater medium boundary (live FDN retune, speed of sound,
the Lloyd's-mirror surface bounce), and a blind A/B/X comparison over single
engine knobs (dual-band, panner choice, spread, air absorption) scored with a
binomial p-value. The 3D speakers shade by their live output level (mirrored as a
meter strip in the panel), so you can watch the panner drive the array even with no
audio device. The playground draws a `constraints.json` next to the exe for
orientation: the same room boxes the layout tool edits against. The Godot addon ships
a port of it, so the demo installs with the binding.

All three GUI tools run their UI test suites under ctest (`--tests`). The screenshots
above come from those runs. Off Windows the two raylib suites need a display: an X or
Wayland session, or run ctest under `xvfb-run`.

## Platforms and licensing

Windows is the production platform. Nothing outside a `*_sink` file is platform-bound,
so the library, the tests, the console examples, and the two raylib tools
(`bwa_layout_tool`, `bwa_playground`) build with MSVC, gcc, and clang.

| platform | array | headphones |
|---|---|---|
| Windows | ASIO into the Digiface | WASAPI, shared mode by default |
| Linux | JACK or ALSA, through a multichannel card or AES67 into the Dante net. Neither route is tested. | JACK or ALSA |
| Android | none. Android carries no array transport. | AAudio, stereo only |
| macOS | none yet | none yet. CoreAudio is specified, not written. |
| Web | none | a Wasm Audio Worklet, stereo. Emscripten with threads, so the page must be cross-origin isolated |

macOS runs the null and manual sinks, so the offline render path works there and nothing
reaches a speaker. The web build keeps the engine's two-thread model over
`SharedArrayBuffer`, which is why it needs the isolation headers; on GitHub Pages a service
worker supplies them. A wasi-sdk build of the same core runs the offline suite under
wasmtime and is the CI leg. [`docs/backends.md`](./docs/backends.md) has the sink contract
and the per-backend rules, [`docs/web.md`](./docs/web.md) the browser side.

**GPLv3** ([`LICENSE`](./LICENSE)). Third-party components keep their own licenses; see
[`docs/build.md`](./docs/build.md) and
[`THIRD_PARTY-NOTICES.md`](./THIRD_PARTY-NOTICES.md).
