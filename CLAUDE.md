# CLAUDE.md

Guidance for working in this repository. Read this first, then the file under
`docs/` relevant to the task. The design is settled; this is a planning/spec repo
with no implementation yet — your job will mostly be scaffolding against these specs.

## What this is

A self-hosted native (C/C++) spatial audio engine for a CAVE installation. It
drives a **26-speaker array** over **ASIO** into a **Dante Virtual Soundcard
(DVS)**, with a **binaural (HRTF) debug monitor** as a second output. Unity and
Unreal are *thin control clients* over a C ABI — no audio crosses that boundary,
only control (sound triggers, source positions, listener pose).

The engine is deliberately *not* built on FMOD/Wwise/middleware. Self-hosting buys
direct access to ASIO timing hooks (`ASIOTime.systemTime`, `ASIOGetSamplePosition`)
and a clean, engine-agnostic core. See `docs/architecture.md` for the why.

## The seam that organizes everything

Sources → per-voice **listener-relative DBAP** panning → an in-memory
**26-channel master bus**. The bus has *consumers*:
- **ASIO device** (production): writes the 26-ch bus straight to DVS.
- **Binaural monitor** (debug): treats each bus channel as a virtual speaker at
  its room position, HRTFs to stereo, writes to a normal output device.

Adding the binaural path did not complicate the core — it is just a second
consumer of the same bus. Protect that property.

## Hard invariants (do not violate)

These are real-time-audio correctness rules. Breaking them causes dropouts/glitches
that are painful to debug, so they are non-negotiable:

1. **No allocation, locks, syscalls, or file I/O on the audio thread.** The audio
   thread is the ASIO `bufferSwitch` callback. wav decode, malloc/free, and logging
   all live on the control thread.
2. **One control thread.** All `bw_*` calls come from a single thread. The command
   ring is single-producer/single-consumer; a second producer breaks it.
3. **Audio thread owns DSP state** (voice table, bus, panner gains, listener
   active fields). The control thread owns handle allocation and asset memory.
   They communicate only through the two SPSC rings.
4. **Gains ramp, never jump.** Per-voice `gcur -> gtarget` interpolated across the
   block. A discontinuous 26-gain change is audible zipper noise.
5. **Generation counts gate handle reuse.** A stale source handle must be dropped,
   not acted on. Sound *buffers* additionally need the retire-ack handshake before
   the control thread frees them.
6. **`CMD_COMMIT` defines frame coherence.** Position/pose write to *pending* fields;
   only commit promotes them to *active*. The mixer reads only active fields.

See `docs/concurrency.md` for the full model and reference code.

## Repo layout (intended)

```
include/bwaudio.h      Public C ABI (authoritative contract).
src/
  engine.c             public ABI: lifecycle + sink + forwards per-frame calls to rt. [M0/M1/M2]
  rt.h / rt.c          rings, voice table, commit snapshot, generation handles, mixer. [M2]
  sink.h / sink.c      device-sink abstraction + backend dispatch. [M1]
  null_sink.c          offline (no-hardware) sink: threaded silence + timestamps. [M1]
  asio_sink.cpp        ASIO host: driver load, bufferSwitch, sample-pos timestamp. [M1]
  sound.h / sound.c    wav decode to mono float via dr_wav (Sound table lives in rt.c). [M3]
  layout.h / layout.c  speaker geometry load (cave_layout.json via cJSON) + default grid. [M4]
  dbap.h / dbap.c      listener-relative, constant-power DBAP gain solve. [M4]
  align.h / align.c    per-speaker gain trim + delay-line output stage. [M4]
  binaural.h/binaural.c  head-oriented 26->stereo monitor (Steam Audio HRTF is the upgrade). [M5]
  ambisonics.h/.c      3rd-order ACN/SN3D encode (+ phonon N3D scale) for the Steam decode. [M5]
  steam_decode.h/.c    production ambisonics->stereo HRTF decode via phonon (with-SDK). [M5]
  steam_scene.h/.c     materials occlusion: IPLScene+IPLSimulator on a sim thread (with-SDK). [materials]
  natnet.c             OptiTrack pose ingest (off-wire, see docs/build.md). [M6]
test/                  bw_smoke (3 profiles), bw_audio_smoke, bw_rt_test, bw_sound_test, bw_dsp_test, bw_monitor_test.
bindings/
  unity/               P/Invoke + BwAudio/BwEmitter (see docs/integration.md).
  unreal/              module + component.
docs/                  Specs. Start here.
examples/              cave_layout.json (see docs/layout-schema.md).
third_party/           asiosdk/ (GPLv3 option, vendored), steamaudio/; dr_wav + cJSON are
                       fetched by CMake (FetchContent, pinned) — see third_party/README.md.
```

## Build

Target: **Windows only** (ASIO is Windows-only; DVS is Windows/macOS). CMake.
A future cross-platform move means abstracting the device layer (ASIO is just the
Windows sink) — do not bake ASIO assumptions outside `asio_sink.c`.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo      # runs the bw_smoke lifecycle check
```

**Current state (M6 + occlusion):** builds `bwaudio.dll` + eight tests. `rt.c` is the concurrency spine
(two SPSC rings, voice + sound tables, commit snapshot, generation handles) + retire-ack;
the whole `bw_*` API forwards to it. `sound.c` decodes wav (dr_wav) and `mix_voice` plays
`sound->pcm` with a gain ramp. Spatialization is real: `layout.c` loads the surveyed
geometry, `dbap.c` is the listener-relative constant-power gain solve, `align.c` applies
the per-speaker gain trim + delay. `binaural.c` is the head-oriented 26→stereo monitor, and
`engine.c` wires all three profiles (`cave` 26→device, `binaural` 26→2ch via the monitor,
`both` array+monitor on two sinks via a double-buffer). Binaural/both reach headphones live
through an auto-picked 2-ch **ASIO** driver (sizes its render scratch to the device block, so
any driver buffer size works); `bw_audio_backend()` reports the device actually opened.
**`natnet.c` is M6: an off-wire NatNet (OptiTrack) FrameOfData parser + a seqlock pose handoff
(`pose.h`); with `track_internal` the audio thread samples the freshest head pose at block
time** (`rt_set_tracker`), configured via `BWAUDIO_NATNET_*` env (see docs/api.md). An
interactive **raylib playground** (`examples/playground.c`, opt-in `-DBWAUDIO_BUILD_PLAYGROUND=ON`)
auditions binaural by ear. The **production Steam Audio HRTF decode** is built + running:
`ambisonics.c` (3rd-order encode) → `steam_decode.c` (phonon `iplAmbisonicsDecodeEffect`), gated
`BW_HAVE_STEAMAUDIO` (phonon built from the `third_party/steam-audio` submodule; see
third_party/README.md), with the simple-pan monitor as the no-SDK fallback. **Materials occlusion is
implemented** (`steam_scene.c`, same gate): a third "simulation thread" owns an `IPLScene` + mesh +
`IPLSimulator`, ray-traces volumetric occlusion + transmission at 30 Hz, and publishes one
transmittance scalar per source into the mixer via a pair of per-voice atomics (`rt`'s `occ_handle`/
`occ_val`) the audio thread gates on its own generation + ramps; `bw_scene_set_mesh` /
`bw_source_set_occlusion` drive it and the playground wall is a real occluder. v1 is level-only
(geometry × material mean transmittance — concrete vs glass differ); the per-band transmission EQ,
directivity, and the reflection bed are the next increments (docs/materials.md). Remaining: the by-ear
headphone check; and live Motive verification of M6 (parser + lifecycle are tested off-wire). Do not bake ASIO assumptions
outside `asio_sink.cpp`, and do not link the NatNet SDK (proprietary; reference only — GPLv3).
The atomics in `rt.c` need `/experimental:c11atomics` on MSVC (wired in CMake); `pose.h` uses
Interlocked intrinsics instead, so `natnet.c`/tests need no extra flag. `-DBWAUDIO_ASAN=ON`
builds `bw_sound_test` under ASan.

## What NOT to do

- Do not introduce FMOD/Wwise or route audio through the engine's mixer.
- Do not use Unity's built-in audio (8-channel cap) or DVS's WDM driver
  (16-channel cap). 26 channels requires ASIO. This is settled.
- Do not pan via pure ambisonics for localized point sources — the listener moves
  across ~3×3 m and a single sweet spot fails. DBAP is recomputed per frame from
  tracked position. See `docs/spatialization.md`.
- Do not assume Steam Audio's Unity/FMOD *integration* limits apply to its C API.
  The C API supports custom speaker layouts; the integrations do not expose them.
- Do not let any `bw_*` per-frame call block or allocate.

## Doc index

- `docs/architecture.md` — system overview, the bus seam, locked decisions + rationale.
- `docs/concurrency.md` — threading model, SPSC rings, commit snapshot, lifetimes. **Most load-bearing.**
- `docs/api.md` — C ABI reference and per-call threading semantics.
- `docs/spatialization.md` — DBAP, moving observer, binaural decode (3rd-order), speaker alignment.
- `docs/materials.md` — material/geometry model → Steam Audio occlusion + reflections → the bus. **Design (Later).**
- `docs/integration.md` — Unity binding + coordinate seam; Unreal notes.
- `docs/build.md` — platform, dependencies, licensing, DVS/Dante config.
- `docs/layout-schema.md` — `cave_layout.json` format: speaker geometry, per-speaker gain/delay, DBAP knobs.
- `docs/internal-types.md` — internal structs (`Voice`/`Sound`/`Layout`/`Listener`) + helper signatures. **Not ABI.**
- `docs/roadmap.md` — milestone-ordered implementation plan.
