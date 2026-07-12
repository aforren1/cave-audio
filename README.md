# bwaudio

Self-hosted spatial audio engine for a 26-speaker CAVE installation. Drives the
array over **ASIO → Dante Virtual Soundcard**, with a **binaural HRTF monitor** for
desk-side debugging. Unity and Unreal connect as thin control clients over a C ABI.

> **Status:** implemented through M6 (engine, spatialization, Steam Audio
> materials/reflections/pathing, tracking, calibration, tooling; 19 ctests).
> Remaining: hardware verification at the rig.

## Why self-hosted

Going direct (no FMOD/Wwise) gives sample-accurate access to ASIO timing hooks and
keeps the core engine-agnostic, so the same library serves both engines with only a
thin per-engine glue layer. The spatializer, mixer, output, and tracking all live
in one process behind one audio callback.

## Shape

```
 engine (Unity/Unreal) ──control──┐
 OptiTrack (NatNet) ─────pose─────┤
                                  ▼
                     ┌─ voice playback ─ DBAP pan ─► 26-ch master bus ─┐
                     │  (one audio callback)                           │
                     └─────────────────────────────────────────────────┘
                                  │                         │
                          ASIO ► DVS ► array        binaural monitor ► stereo
                          (production)              (debug)
```

## Engine features

- **Spatialization**: per-voice listener-relative DBAP (SPCAP/VBAP selectable, a
  dual-band option), recomputed per block from the tracked head position; source
  spread; per-speaker gain/delay/correction-EQ output stage, with a linked
  protection limiter as the final stage.
- **Acoustics** (Steam Audio): ray-traced occlusion with per-band transmission EQ,
  source directivity, a directional reflection bed (real-time, or baked over a
  probe grid), sound pathing around occluders with bending-loss EQ.
- **Propagation**: distance attenuation, Doppler, air absorption — opt-in per
  source, ramped.
- **Assets**: WAV/FLAC/MP3, decoded and resampled at load; disk streaming for long
  files; AmbiX ambisonic beds decoded world-locked to the array.
- **Voices**: fixed pool with priority stealing; pause and click-free seek;
  sample-accurate start against a device-anchored DSP clock
  (`bw_source_play_at` / `bw_dsp_time`).
- **Tracking**: OptiTrack NatNet parsed off-wire; the audio thread samples the
  freshest head pose at block time.
- **Monitoring**: binaural HRTF render of the same 26-channel bus (3rd-order
  ambisonic encode → Steam Audio decode) to any 2-ch ASIO device; per-channel
  test signal.
- **Real-time discipline**: no allocation, locks, or I/O on the audio thread;
  lock-free SPSC command/event rings; `bw_commit` gives frame-coherent updates.

## Non-goals & current limitations

bwaudio is not middleware. There are no events, banks, mixer graphs, or authoring
app — the ABI is create/play/position/commit. Game-side audio (UI, menus) stays in
the game engine's own mixer.

- **Fixed target.** The speaker geometry is data, but the channel count is
  compile-time. This is not a general 5.1/Atmos renderer.
- **Windows + ASIO only. One listener.**
- **Room EQ is opt-in and static-listener only** (`bw_calibrate --room-eq`, for the
  fixed-seat SPCAP/VBAP deployments). The moving-listener default flattens the
  speakers, not the room — one measurement point can't correct a roam.

Current gaps (may change):

- No user pitch control — Doppler is physics-derived.
- No master-bus gain or buses/groups (there is an output protection limiter).
- Mono point sources only. Stereo assets downmix; the ambisonic bed is the only
  non-point path.
- No completion callbacks — poll `bw_source_is_playing`.
- No OGG/Opus, and seek does not apply to streamed sounds.
- Reflection/room configuration is load-time (occlusion meshes can be replaced live).

## Getting started

Usage docs live in [`docs/api.md`](./docs/api.md): quickstart, profiles, the
threading contract, coordinates, error handling, environment variables, then a
per-call reference. [`examples/minimal.c`](./examples/minimal.c) runs the whole
client lifecycle (create → load → play → per-frame commit → teardown); it builds
as `bw_minimal` and needs no hardware.

```c
BwConfig cfg = { .profile = BW_PROFILE_BINAURAL, .sample_rate = 48000, .block_size = 256 };
BwEngine* e = bw_create(&cfg);
bw_start(e);                                    // no ASIO device? silent sink, keeps running

BwSound ping = bw_load_sound(e, "ping.wav");    // WAV/FLAC/MP3, resampled at load
BwSource s   = bw_source_create(e);
bw_source_play(e, s, ping, /*loop*/ true);

// per frame, from one thread:
bw_set_listener_pose(e, px,py,pz, qx,qy,qz,qw);
bw_source_set_pos(e, s, x, y, z);
bw_commit(e);
```

## Documentation

| doc | covers |
|-----|--------|
| [`docs/api.md`](./docs/api.md) | usage guide + per-call reference |
| [`include/bwaudio.h`](./include/bwaudio.h) | the C ABI; every declaration commented |
| [`docs/architecture.md`](./docs/architecture.md) | system overview, the bus seam, locked decisions |
| [`docs/concurrency.md`](./docs/concurrency.md) | threading model, rings, commit snapshot, lifetimes |
| [`docs/spatialization.md`](./docs/spatialization.md) | DBAP/SPCAP/VBAP, dual-band, binaural decode, alignment |
| [`docs/materials.md`](./docs/materials.md) | occlusion, reflections, sound pathing |
| [`docs/integration.md`](./docs/integration.md) | Unity/Unreal bindings, coordinate seam |
| [`docs/layout-schema.md`](./docs/layout-schema.md) | `cave_layout.json` format |
| [`docs/calibration.md`](./docs/calibration.md) | trims, EQ, acoustic survey, room report |
| [`docs/build.md`](./docs/build.md) | platform, dependencies, licensing, DVS/Dante config |

Contributor-facing notes live in [`docs/internal-types.md`](./docs/internal-types.md),
[`docs/roadmap.md`](./docs/roadmap.md), [`docs/profiling.md`](./docs/profiling.md), and
`CLAUDE.md` (agent working notes, not user documentation).

## Tools

Opt-in, in workflow order (design → calibrate → audition):

```
cmake -S . -B build -DBWAUDIO_BUILD_PLAYGROUND=ON -DBWAUDIO_BUILD_CALIBVIEW=ON -DBWAUDIO_BUILD_CALIBRATE=ON
cmake --build build --config RelWithDebInfo
```

### Design — `bw_layout_tool`

![bw_layout_tool](docs/img/layout_tool.png)

Authors `cave_layout.json`:

- Place the 26 speakers. A speaker's index is its output channel, so the built-in
  test tone tells you which physical speaker is which.
- Load placement constraints from `constraints.json`.
- Shade a coverage shell by nearest-speaker gap, or by the selected panner's
  rE-localization error — computed with the engine's own gain solve.
- Optionally hill-climb the positions against that error.
- Preview a moving pink-noise source through the edited layout.

Headless: `--export`, `--score`, `--optimize`.

### Calibrate — `bw_calib_view`

![bw_calib_view: layout diff](docs/img/calib_view_diff.png)

The Capture tab runs sweep → measure → solve → writeback (simulated, or full-duplex
ASIO with a measurement mic), then loads the result into a layout diff — A the
input, B what was written. A swapped channel or a bad mic placement is caught
before the file is trusted.

Other tabs: the array in 3D, gain/delay trims, correction-EQ curves, retained IRs.
The Zylia tab shows clap direction-of-arrival on a ZM-1 capsule sphere — a
seconds-fast check of capsule mapping and geometry.

![bw_calib_view: Zylia tab](docs/img/calib_view_zylia.png)

`bw_calibrate` is the same pipeline headless (`--simulate`, `--localize`);
`bw_zylia_probe` is a console level meter. See
[`docs/calibration.md`](./docs/calibration.md).

**One array, several audiences.** Speaker positions are surveyed once, but trims and
EQ are measured relative to a reference point — so one installation can keep several
calibrated variants of the same geometry and pick one per session
(`BwConfig.layout_path` + the panner):

```
bw_calibrate --layout survey.json --mic 0 1.2 0 --room-eq --out cave_layout.seated.json
bw_calibrate --layout survey.json --mic 0 1.7 0 --eq      --out cave_layout.roaming.json
```

- **Seated** (SPCAP/VBAP, fixed listener pose): trims aligned at the seat, room
  correction at the seat. `--room-eq` is only valid for a listener who stays at the
  measurement point.
- **Roaming** (DBAP + tracking): trims aligned at the working-volume centre at
  standing ear height, speaker-only EQ — one point can't room-correct a roam.

Diffing the two files in calib_view should show identical positions and only trim/EQ
differences. Unknown JSON fields survive recalibration, so a variant can carry its
own annotation (e.g. `"intent": "seated, SPCAP"`). And loading the seated file into
a moving-listener session (DBAP or tracking) fails `bw_start` rather than quietly
mis-correcting the array.

### Audition — `bw_playground`

![bw_playground](docs/img/playground.png)

Binaural monitor on headphones (auto-picked 2-ch ASIO driver; without one the
engine falls back to the null sink and keeps rendering — visual-only, live, just
silent). Scenes: localization, occlusion + materials, directivity, channel walk,
reverb bed, and a blind A/B/X comparison over single engine knobs (dual-band,
panner choice, spread, air absorption) scored with a binomial p-value. The 3D
speakers shade by their live output level (mirrored as a meter strip in the
panel), so you can watch the panner drive the array even with no audio device. A
`constraints.json` next to the exe is drawn for orientation — the same room
boxes the layout tool edits against.

All three GUI tools run their UI test suites under ctest (`--tests`); screenshots
above are from those runs.

## Platform & licensing

Windows-only (ASIO). **GPLv3** ([`LICENSE`](./LICENSE)); third-party components
keep their own licenses — see [`docs/build.md`](./docs/build.md).
