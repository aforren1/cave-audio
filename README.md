# bwaudio

Self-hosted spatial audio engine for a 26-speaker CAVE installation. Drives the
array over **ASIO → Dante Virtual Soundcard**, with a **binaural HRTF monitor** for
desk-side debugging. Unity and Unreal connect as thin control clients over a C ABI.

> **Status:** implemented through M6 (engine, spatialization, Steam Audio
> materials/reflections/pathing, tracking, calibration, tooling; 18 ctests).
> Remaining: hardware verification at the rig. `include/bwaudio.h` is the
> authoritative contract.

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

## Read next

Start with [`CLAUDE.md`](./CLAUDE.md), then [`docs/architecture.md`](./docs/architecture.md).
Full doc index is in `CLAUDE.md`.

## Tools

Opt-in, in workflow order (design → calibrate → audition):

```
cmake -S . -B build -DBWAUDIO_BUILD_PLAYGROUND=ON -DBWAUDIO_BUILD_CALIBVIEW=ON -DBWAUDIO_BUILD_CALIBRATE=ON
cmake --build build --config RelWithDebInfo
```

### Design — `bw_layout_tool`

![bw_layout_tool](docs/img/layout_tool.png)

Authors `cave_layout.json`. Place the 26 speakers (a speaker's index is its output
channel, so the built-in test tone identifies which physical speaker is which);
load placement constraints from `constraints.json`; shade a coverage shell by
nearest-speaker gap or by the selected panner's rE-localization error (the
engine's own gain solve); optionally hill-climb the positions against that error;
preview a moving pink-noise source through the edited layout. Headless:
`--export`, `--score`, `--optimize`.

### Calibrate — `bw_calib_view`

![bw_calib_view: layout diff](docs/img/calib_view_diff.png)

The Capture tab runs sweep → measure → solve → writeback (simulated, or
full-duplex ASIO with a measurement mic) and loads the result into a layout diff —
A the input, B what was written — so a swapped channel or bad mic placement is
caught before the file is trusted. Other tabs: the array in 3D, gain/delay trims,
correction-EQ curves, retained IRs. The Zylia tab shows clap direction-of-arrival
on a ZM-1 capsule sphere to check capsule mapping and geometry.

![bw_calib_view: Zylia tab](docs/img/calib_view_zylia.png)

`bw_calibrate` is the same pipeline headless (`--simulate`, `--localize`);
`bw_zylia_probe` is a console level meter. See
[`docs/calibration.md`](./docs/calibration.md).

### Audition — `bw_playground`

![bw_playground](docs/img/playground.png)

Binaural monitor on headphones (auto-picked 2-ch ASIO driver; visual-only without
one). Scenes: localization, occlusion + materials, directivity, channel walk,
reverb bed, and a blind A/B/X comparison over single engine knobs (dual-band,
panner choice, spread, air absorption) scored with a binomial p-value.

The imgui tools run their UI test suites under ctest (`--tests`); screenshots
above are from those runs.

## Platform & licensing

Windows-only (ASIO). **GPLv3** ([`LICENSE`](./LICENSE)); third-party components
keep their own licenses — see [`docs/build.md`](./docs/build.md).
