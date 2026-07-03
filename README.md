# bwaudio

Self-hosted spatial audio engine for a 26-speaker CAVE installation. Drives the
array over **ASIO → Dante Virtual Soundcard**, with a **binaural HRTF monitor** for
desk-side debugging. Unity and Unreal connect as thin control clients over a C ABI.

> **Status: implemented through M6** — the engine (`bwaudio.dll`), spatialization,
> Steam Audio materials/reflections/pathing, tracking ingest, calibration, and the
> tooling below, all under an 18-test ctest suite. `include/bwaudio.h` is the
> authoritative contract. What remains is hardware-in-the-loop verification at the
> rig (first real ASIO full-duplex runs, live Motive, by-ear HRTF checks).

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

## The GUI tools

Three tools cover the array's life cycle — **design** the layout at a desk,
**calibrate and verify** it at the rig, **audition** the result by ear. They are
opt-in so the default build stays lean:

```
cmake -S . -B build -DBWAUDIO_BUILD_PLAYGROUND=ON -DBWAUDIO_BUILD_CALIBVIEW=ON -DBWAUDIO_BUILD_CALIBRATE=ON
cmake --build build --config RelWithDebInfo
```

The imgui tools test themselves: `--tests` drives the real UI with fake inputs
(imgui_test_engine) under ctest, and the screenshots below were captured by those
suites — what you see is what the regression tests see.

### 1 · Design — `bw_layout_tool`

Author `cave_layout.json` before any hardware exists, and survey it once the array
is up. Hover any control for an explanation of what it does.

![bw_layout_tool: coverage overlay + control panel](docs/img/layout_tool.png)

- **Place speakers** in room space (drag, type exact survey coordinates, or nudge
  with keys). The killer feature is *identify-by-ear*: a speaker's index **is** its
  output channel, so enable the tone, walk the room, and place the marker on
  whichever physical speaker sounds.
- **Constraints**: drop a `constraints.json` next to the layout (allowed bounds,
  no-go boxes for screens/structure, solid occluders like projectors) and the tool
  draws them, flags violating speakers, and snaps them to the nearest legal spot.
- **Analyze**: the coverage shell shades every direction by nearest-speaker gap or
  by the selected panner's *real* rE-localization error (the engine's own gain
  solve, not a re-implementation); the scoreboard tracks DBAP/SPCAP/VBAP live.
- **Optimize**: a constrained hill-climb moves the speakers to minimize the target
  panner's error — watch it converge, stop it, save.
- **Audition**: preview pans a pink-noise source through the edited layout on the
  actual array; the head view puts the camera at the listener's ears.
- Headless modes for scripting: `--export`, `--score`, `--optimize`.

### 2 · Calibrate & verify — `bw_calib_view`

The calibration station: one window for rig day. The **Capture** tab runs the full
sweep → measure → solve → writeback pipeline on a worker thread (simulated, or
full-duplex ASIO with a measurement mic) with per-speaker progress, then loads the
result straight into the **Diff** view — A is what calibration read, B is what it
wrote, and a swapped channel or bad mic placement shows up as an absurd delta
*before* you trust the file:

![bw_calib_view: layout diff after a calibration run](docs/img/calib_view_diff.png)

The **Array / Trims / EQ / IRs** tabs inspect the geometry in 3D, the gain/delay
trims, the per-speaker correction-filter magnitude curves, and the retained impulse
responses. The **Zylia** tab is the one-clap sanity check: clap anywhere around a
ZM-1 and a dot appears on the capsule sphere where the clap came from — verifying
capsule mapping *and* the geometry table in seconds (a simulate mode synthesizes
claps from a known direction through the identical pipeline):

![bw_calib_view: Zylia clap-DOA on the capsule sphere](docs/img/calib_view_zylia.png)

`bw_calibrate` is the same pipeline headless (`--simulate` runs it hardware-free;
`--localize` adds the multi-position acoustic position survey), and
`bw_zylia_probe` is the console bring-up meter. See
[`docs/calibration.md`](./docs/calibration.md).

### 3 · Audition — `bw_playground`

![bw_playground: localization scene](docs/img/playground.png)

Six by-ear scenes for the binaural monitor on headphones (auto-picked 2-ch ASIO
driver — ASIO4ALL / FlexASIO / the Steinberg built-in; without one it runs visual
only): localization with an auto-move sweep, occlusion + materials, directivity, a
channel-walk speaker check, a reverb-bed room, and a **blind A/B/X harness** — X is
secretly A or B over one live engine knob (dual-band panning, DBAP vs SPCAP/VBAP,
spread, air absorption), and a one-sided binomial p-value over your answers says
whether a difference is genuinely audible, not just "sounds different to me".

### Testing the GUIs

Both imgui tools run under ctest (`layout_tool`, `calib_view`): fake inputs drive
the actual panels, assertions run against app state (a calibration run's trims, a
recovered clap direction within 2° of truth), and screenshots land in
`output/captures/`. Run one directly with `bw_layout_tool --tests [filter]`.

## Platform & licensing

Windows-only (ASIO). Links the Steinberg ASIO SDK under its GPLv3 option and Steam
Audio; see [`docs/build.md`](./docs/build.md) for the copyleft/distribution
implications before you ship anything.

Licensed under **GPLv3** ([`LICENSE`](./LICENSE)), consistent with the ASIO SDK's GPLv3 option.
Third-party components keep their own licenses (see [`docs/build.md`](./docs/build.md)).
