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
auditions binaural by ear across five feature **scenes** (TAB): localization (with a SPACE auto-move
sweep), occlusion+materials, directivity, a channel-walk speaker check, and a reverb-bed room (which
rebuilds the engine on entry/exit, since the bed + room geometry are load-time). **`bw_test_signal(channel, kind, gain)`** drives one
raw output channel with a 660 Hz sine/noise injected after align (`rt.c`) — a speaker-check / wiring
tool, not a spatial path. The **production Steam Audio HRTF decode** is built + running:
`ambisonics.c` (3rd-order encode) → `steam_decode.c` (phonon `iplAmbisonicsDecodeEffect`), gated
`BW_HAVE_STEAMAUDIO` (phonon built from the `third_party/steam-audio` submodule; see
third_party/README.md), with the simple-pan monitor as the no-SDK fallback. **Materials: occlusion +
per-band transmission EQ + source directivity are implemented** (`steam_scene.c`, same gate): a third
"simulation thread" owns an `IPLScene` + mesh + `IPLSimulator`, ray-traces volumetric occlusion +
transmission + directivity at 30 Hz, and publishes per source a (level, 3-band tilt, directivity-gain)
set via per-voice atomics (`rt`'s `occ_handle`/`occ_val`/`occ_eq`/`occ_dir`) gated on the audio
thread's own generation; the audio thread applies a 3-biquad transmission EQ (so a wall *muffles*, not
just attenuates — rate-derived, runs at 96 kHz too), a directivity dipole gain, and the level — all
ramped per sample. `bw_scene_set_mesh` / `bw_source_set_occlusion` / `bw_source_set_directivity` /
`bw_source_set_orientation` drive it; the playground wall is a real occluder. The **reflection bed** is
implemented (`steam_reflect.c`, same gate): an `IPLSimulator` reflections sim → ambisonic IR → the
SH→26 decode → bus, registered as the rt bus tap at `bw_start`; the `reflect` test proves it's
*directional*. Sources opt in via `bw_source_set_reflections`, with a per-source wet-send level
(`bw_source_set_reflection_send`) and an optional **distance→wet** scaling (`bw_source_set_reflection_distance`,
near = drier / far = wetter; the send gain is distance-derived in `rt.c` and ramped). **Opt-in per-source
propagation effects** are implemented (phonon-free,
pure `rt.c` DSP, default off): **Doppler** (`bw_source_set_doppler`) renders each voice through a
per-voice fractional delay ring (`RtCore.dop_ring`, one power-of-two ring per voice, allocated at
create) whose delay glides toward `distance/c` — the glide rate is the pitch shift, saturating past
~8 m; and **air absorption** (`bw_source_set_air_absorption`) a distance-driven one-pole HF low-pass.
Both compute the source↔listener distance per block, ramp per sample (invariant 4), and tap the
reflection send *before* themselves (direct path only); indices stay integer so a long-lived voice
never loses sample precision. **Source spread/size** (`bw_source_set_spread`, 0=point..1=wide) is also
in: the per-block gain solve blends the panner's point gains toward a width-controlled lobe centred on
the source direction, renormalised to the panner's own power (widening never re-levels) — panner-agnostic.
**Dual-band panning** (`bw_set_dual_band`, off by default, live A/B) wraps the selected panner: a 700 Hz
complementary crossover splits each voice, the low band panned amplitude-normalised (`Σg = gain`,
velocity-vector) and the high band power-normalised (the panners' usual `Σg² = gain²`) — SPAT's "VBP
Dual-Band", for sharper LF localisation near the sweet spot; `compute_gains` derives `gtarget_lo` every
solve so it A/Bs live, and the mixer reads it only when on. An **ambisonic bed** is implemented too (`bw_load_ambix` + `bw_bed_*`):
a file-fed AmbiX soundfield decoded world-locked to the 26-ch bus (`rt.c` `build_bed_decode`/`mix_bed`,
phonon-free), reusing the SH→26 decode the reflection bed will need. `sound.c` now decodes **WAV/FLAC/MP3**
(dr_libs, one pinned repo fetch) and **resamples to the engine rate at load** (windowed-sinc). **Streaming**
(`bw_load_sound_streaming`, `stream.c`) plays long files without decoding them into RAM: a background
thread decodes chunks (WAV/FLAC/MP3, downmixed to mono, engine rate required) into a per-stream **SPSC ring**;
the audio thread `stream_pull`s from the ring in `mix_voice` (no I/O/alloc/locks), distinguishing a true EOF
from a transient underrun. One voice per stream; the retire handshake detaches voices before the control
thread closes the stream.
**Voice management + scheduling**: sources carry a control-side steal **priority** (`bw_source_set_priority`,
255 = protected) — a full pool steals the lowest-priority voice instead of failing the create; and
**`bw_source_play_at(start_sample)`** fires a voice sample-accurately off a published dsp clock
(`bw_dsp_time`, device-anchored), the mixer holding it silent until the exact in-block offset. **Ray-tracing
acceleration**: `BWAUDIO_EMBREE=1` runs both sims on Intel Embree, opt-in with a graceful fallback to the
default tracer (the vendored prebuilt phonon isn't Embree-built, so it currently falls back — see docs/api.md).
**Speaker calibration** (`bw_calibrate`, opt-in `-DBWAUDIO_BUILD_CALIBRATE=ON`; DSP in `measure.c`, solve +
JSON writeback in `calib.c`, both unit-tested off-hardware): a full-duplex ASIO tool that sweeps each speaker,
records an omni mic, and writes `cave_layout.json` — per-speaker delay/gain trims (`calib_solve`, arrival-align
+ sensitivity-equalize), acoustic **position self-survey** through the screens (`--localize` → `calib_trilaterate`,
which recovers positions + the system latency jointly), and a **room report** (`--room`, Schroeder RT60 + early
reflections — a treatment diagnostic, NOT a model to match: matching double-counts the real room). `--save-irs`
retains the per-speaker IR kernels (one capture serves trims, the room report, and a future headphone room
simulator). The ASIO capture compiles but is unverified on hardware; `--simulate` runs the whole pipeline
hardware-free. See docs/calibration.md.
Remaining: the by-ear headphone check; and live Motive verification of M6 (parser + lifecycle are tested off-wire). Do not bake ASIO assumptions
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
- `docs/calibration.md` — `bw_calibrate`: acoustic position survey, delay/gain trims, room report → `cave_layout.json`.
- `docs/internal-types.md` — internal structs (`Voice`/`Sound`/`Layout`/`Listener`) + helper signatures. **Not ABI.**
- `docs/roadmap.md` — milestone-ordered implementation plan.
