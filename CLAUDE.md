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
src/                   Core implementation (to be written).
  engine.c             create/start/stop/destroy, rings, voice table.
  asio_sink.c          ASIO host: driver load, bufferSwitch, sample-pos timestamp.
  binaural.c           26->ambisonics->HRTF monitor (Steam Audio).
  dbap.c               listener-relative gain solve.
  layout.c             speaker geometry load + per-speaker gain/delay alignment.
  sound.c              wav load (dr_wav), buffer lifetime.
  natnet.c             OptiTrack pose ingest (off-wire, see docs/build.md).
bindings/
  unity/               P/Invoke + BwAudio/BwEmitter (see docs/integration.md).
  unreal/              module + component.
docs/                  Specs. Start here.
third_party/           asiosdk/ (GPLv3 option), steamaudio/, dr_wav.h
```

## Build (intended — not yet implemented)

Target: **Windows only** (ASIO is Windows-only; DVS is Windows/macOS). CMake.
A future cross-platform move means abstracting the device layer (ASIO is just the
Windows sink) — do not bake ASIO assumptions outside `asio_sink.c`.

```
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config RelWithDebInfo
```

When you add a build system, wire these placeholder commands into reality and
update this section.

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
- `docs/integration.md` — Unity binding + coordinate seam; Unreal notes.
- `docs/build.md` — platform, dependencies, licensing, DVS/Dante config.
- `docs/layout-schema.md` — `cave_layout.json` format: speaker geometry, per-speaker gain/delay, DBAP knobs.
- `docs/internal-types.md` — internal structs (`Voice`/`Sound`/`Layout`/`Listener`) + helper signatures. **Not ABI.**
- `docs/roadmap.md` — milestone-ordered implementation plan.
