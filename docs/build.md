# Build, dependencies & licensing

## Platform

**Windows only.** ASIO is Windows-only; DVS is Windows/macOS. On other platforms the
device backend would be ALSA/JACK (Linux) or CoreAudio (macOS), and you would *not*
use ASIO there. A future cross-platform move means abstracting the device layer —
ASIO is just the Windows sink. Keep ASIO assumptions confined to `asio_sink.cpp`.

Build system: CMake + MSVC (Visual Studio 2022 toolset). The core is C (C11 with
`stdatomic.h`); Steam Audio and ASIO glue are C/C++.

MSVC gates C11 atomics behind `/experimental:c11atomics`. CMake applies that flag per
source file: `src/rt.c` and `src/stream.c` always, plus `src/steam_reflect.c` in the
with-SDK build (its IR-publish seqlock uses `stdatomic.h` too) — both where the DLL
compiles it and in the test targets (`reflect`, `bake`) that compile it directly.

**Race-checking the rings.** The `test_rt` target drives the SPSC ring/commit logic
off the real-time path — single-threaded, deterministic — and is what runs under
`ctest` on MSVC. The full ThreadSanitizer/Helgrind pass the roadmap calls for needs a
**Clang or Linux** build: MSVC ships no TSan, and Helgrind is Valgrind/Linux. Build
`rt.c` + a two-thread driver with `clang -fsanitize=thread` (or run under Helgrind)
and exercise one producer pushing commands while one consumer drains. Keep that
driver off the RT path — never let the sanitizer harness add allocation/locks to the
callback.

## CMake options

Everything beyond the DLL + test suite is opt-in; the default build stays lean.

| option | default | what it does |
|--------|---------|--------------|
| `BWAUDIO_BUILD_TESTS` | ON | the ctest suite (`test_*` targets) |
| `BWAUDIO_WITH_ASIO` | OFF* | the ASIO backend. *Auto-flips ON when the SDK sits at `third_party/asiosdk/` |
| `BWAUDIO_BUILD_PLAYGROUND` | OFF | `bw_playground` + `bw_layout_tool` (fetches raylib/rlImGui/imgui/test-engine) |
| `BWAUDIO_BUILD_CALIBVIEW` | OFF | `bw_calib_view` (fetches imgui/test-engine/implot/implot3d) |
| `BWAUDIO_BUILD_CALIBRATE` | OFF | `bw_calibrate` + `bw_zylia_probe` |
| `BWAUDIO_ASAN` | OFF | builds `test_sound` with AddressSanitizer (MSVC; needs tests ON) |
| `BWAUDIO_TRACY` | OFF | Tracy profiler instrumentation (fetches Tracy; collects only while a profiler is attached) |

Steam Audio has no option: CMake auto-enables it when the built phonon SDK sits at
`third_party/steam-audio-artifacts/` (see `third_party/README.md`).

## Dependencies

| dep            | role                                        | license / notes                          |
|----------------|---------------------------------------------|------------------------------------------|
| Steinberg ASIO SDK | device output to DVS; timing hooks       | dual GPLv3 / proprietary (see below)     |
| Steam Audio (C API)| binaural HRTF decode; occlusion, reflections (with baking), pathing — all implemented | Apache-2.0 (`steam-audio-source/LICENSE.md`) |
| dr_libs (dr_wav 0.14.5 / dr_flac 0.13.3 / dr_mp3 0.7.3) | WAV/FLAC/MP3 decode (`sound.c`, `stream.c`) | public domain / MIT-0; FetchContent, pinned |
| cJSON v1.7.19  | layout + calibration JSON                    | MIT; FetchContent, pinned                |
| NatNet         | OptiTrack pose ingest                        | consume off-wire; see below              |

Two dependencies are vendored under `third_party/`: `asiosdk/`, and the Steam Audio
pair (`steam-audio-source/` submodule + the built `steam-audio-artifacts/`). Neither
is committed to the repo; `third_party/README.md` has the fetch/build steps.

dr_libs and cJSON are **not** vendored. CMake fetches both via `FetchContent`, pinned
to exact commits. Offline builds override with `-DFETCHCONTENT_SOURCE_DIR_<NAME>=<dir>`.

### Tool dependencies (opt-in builds only)

The GUI tools and the profiler pull their own pinned deps. None of these touch the
default build or `bwaudio.dll`:

| dep | pin | pulled in by | license |
|-----|-----|--------------|---------|
| imgui | v1.92.8 | PLAYGROUND, CALIBVIEW | MIT |
| imgui_test_engine | v1.92.8 | PLAYGROUND, CALIBVIEW | Dear ImGui Test Engine License — **not MIT**, see below |
| implot | v1.0 | CALIBVIEW | MIT |
| implot3d | v0.4 | CALIBVIEW | MIT |
| raylib | 5.5 | PLAYGROUND | zlib/libpng |
| rlImGui | `Raylib_5_5` tag | PLAYGROUND | zlib/libpng |
| Tracy | v0.13.1 | `BWAUDIO_TRACY` | BSD-3-Clause |
| Roboto-Regular | embedded (`examples/roboto_font.h`) | all GUI tools | Apache-2.0 (Google) |

The imgui and imgui_test_engine tags track each other. Bump them together.

### License inventory — what ships where

Everything above, sorted by what it actually ends up in:

- **`bwaudio.dll`** (the artifact CI distributes, under this repo's GPLv3) links four
  third-party components: the ASIO SDK under its **GPLv3** option, Steam Audio's
  `phonon.dll` (**Apache-2.0**, a separate DLL loaded at runtime), dr_libs (**public
  domain / MIT-0**, your choice), and cJSON (**MIT**). All four are GPLv3-compatible,
  so distributing the DLL under GPLv3 is consistent.
- **The GUI tools** (`bw_playground`, `bw_layout_tool`, `bw_calib_view`, opt-in builds)
  additionally compile in imgui, implot, implot3d (**MIT**), raylib, rlImGui
  (**zlib/libpng**), the Roboto face (**Apache-2.0**), imgui_test_engine (**its own
  dual license** — free tier, see below), and optionally Tracy (**BSD-3-Clause**).
  The tools ship in the CI artifact; the notices ride along in
  [`THIRD_PARTY-NOTICES.md`](../THIRD_PARTY-NOTICES.md) (repo root, copied into the
  artifact). Keep that file in sync when a pin bumps.
- **Never linked**: the NatNet SDK. `third_party/NatNetSDK/` sits in the tree as a
  **protocol reference only** (it is proprietary — OptiTrack's plugin license); no
  target compiles or links it, and it must never be distributed with this repo.
  `natnet.c` speaks the documented wire protocol instead — see below.
- **Optional, user-supplied**: Intel Embree + TBB (**Apache-2.0** both) if you drop in
  an Embree-enabled `phonon` for `BWAUDIO_EMBREE` (see `docs/api.md`). This repo
  ships neither.

### ASIO SDK licensing (read before distributing)

As of October 2025 the ASIO SDK is **dual-licensed GPLv3 / proprietary** (previously
proprietary-only). You can use it under the GPLv3 option without signing an agreement.
The catch is copyleft — pick the case that matches how you ship:

- **Distributed under GPLv3** → what this repo does. CI publishes `bwaudio.dll` as a
  workflow artifact under the repo's GPLv3 `LICENSE`, with the complete source
  available as this repo at the built commit. That meets the GPLv3 terms — see the
  CI section below.
- **Internal, undistributed** → GPL obligations don't trigger (copyleft is a
  distribution condition). Use freely.
- **Shipping closed** → take the proprietary ASIO license (the other half of the dual).

This is the shape of the issue, not legal advice. Confirm against the current SDK
license text. The same copyleft reasoning is why a permissive library (e.g. miniaudio)
still won't bundle ASIO upstream: a permissive core can't absorb GPLv3. A "miniaudio
ASIO backend" would be one *you* write under the GPLv3 (or proprietary) option.

### imgui_test_engine licensing

`imgui_test_engine` is **not MIT**, unlike imgui itself. It ships under the
"Dear ImGui Test Engine License" (v1.04 in the pinned tag): free if you are a natural
person, an open-source project, an educational/research institution, or a small
business under the license's revenue threshold — a paid license otherwise. Read
`imgui_test_engine/LICENSE.txt` in the fetched tree for the exact criteria.

That's fine here: this repo is GPLv3 open source, so the free tier applies, and the
test engine compiles only into the GUI tools (`bw_playground`, `bw_layout_tool`,
`bw_calib_view`) — never into `bwaudio.dll`. The tools ship in the CI artifact with
the notice in `THIRD_PARTY-NOTICES.md`. Revisit if this repo's licensing changes.

### NatNet without the proprietary SDK

NaturalPoint's NatNet SDK is proprietary, which would conflict with GPLv3 under
distribution. The NatNet protocol is documented, so `natnet.c` consumes the
multicast/unicast stream directly rather than linking the SDK. A copy of the SDK sits
at `third_party/NatNetSDK/` as a protocol reference for that implementation — no
target compiles or links it, and it stays out of anything distributed. This also lets the core
read pose itself (`track_internal = true`) and sample the freshest head pose at
audio-callback time — lower-latency than marshaling pose through the engine each frame.

## Continuous integration

`.github/workflows/ci.yml` builds and tests on `windows-latest`, and doubles as the
distribution channel:

- **ASIO is built.** The workflow fetches the SDK from Steinberg's official URL
  (cached between runs), and the configure step fails loudly unless the log says
  `ASIO backend ENABLED` — the artifact must contain the production device path.
- **Steam Audio is built, and cached.** CI runs the phonon recipe from
  [`third_party/README.md`](../third_party/README.md) (patched submodule, minimal
  core, `/MD`) and caches `third_party/steam-audio-artifacts/` keyed on the
  submodule sha + the patch hash — the first run pays the phonon build, every later
  run restores it. The four with-SDK tests (`reflect`/`bake`/`path`/`steam_decode`)
  run, and binaural is the real HRTF decode. One CI-only tweak: the pinned phonon
  build scripts hard-code the VS 2022 generator, so the workflow rewrites that to
  whatever Visual Studio the runner image has (via `vswhere`).
- **Tests run with `BWAUDIO_SINK=null`.** Runners have no audio hardware; forcing the
  null sink keeps runs deterministic instead of relying on fallback. The three GUI
  UI suites (`playground`/`layout_tool`/`calib_view`) are built but excluded from
  the CI ctest run — they need a display and OpenGL, which runners don't have; run
  them locally.
- **Two configs, x64 only.** RelWithDebInfo is the full build: tested, tools and
  all. Debug builds the engine (`bwaudio` + `bw_minimal`) and smoke-runs it, for
  downstream debugging against a debug CRT.
- **The tools are built and shipped.** CI configures with `BWAUDIO_BUILD_PLAYGROUND`,
  `BWAUDIO_BUILD_CALIBVIEW`, and `BWAUDIO_BUILD_CALIBRATE`, so the artifact carries
  `bw_playground`, `bw_layout_tool`, `bw_calib_view` (GUI — they need a display),
  plus `bw_calibrate` and `bw_zylia_probe` (console).
- **The artifact is a GPLv3 distribution.** Each run uploads `RelWithDebInfo/`
  (engine + phonon + tools) and `Debug/` (engine + phonon) folders plus `bwaudio.h`,
  the example layout + `constraints.json`, the `LICENSE`, `THIRD_PARTY-NOTICES.md`,
  and a `DIST.txt` naming the commit and linking the complete source (this repo at
  that commit). Downloading requires repo access; the artifact carries the GPLv3
  terms with it.
- **`bwaudio.dll` needs `phonon.dll` beside it.** With-SDK builds (including the CI
  artifact) link `phonon.lib`, so the DLL won't load without `phonon.dll` in the
  same directory — keep the pair together. The CMake post-build step and the Unity
  plugin staging both copy it for you. Only a no-SDK build is self-contained (with
  the simple-pan binaural fallback and acoustics calls as no-ops).

## DVS / Dante configuration

- **Driver: ASIO.** Do not use the WDM driver — it caps at 16 channels; 26 needs ASIO
  (up to 64).
- **Format:** 48 kHz, 24-bit. Match the bit depth end-to-end; DVS truncates on
  mismatch. Read the driver's reported sample type via `ASIOGetChannelInfo` and
  convert (DVS is typically Int32 or packed Int24).
- **Clock:** a Dante network needs one **leader clock**. In the CAVE, let a hardware
  Dante node (interface/amp/processor) be leader; DVS slaves to it. A pure-software
  DVS instance can't run standalone without a leader on the net.
- **Latency:** start ASIO buffer ~512–1024 and Dante latency 4–10 ms, then tighten
  empirically against measured dropouts.
- **Isolation (recommended):** consider running DVS on a machine separate from the
  engine, keeping the audio thread and Dante stack away from engine frame-rate
  spikes — the usual source of dropouts. The control-only C ABI does not require
  this, but the design tolerates it.

## ASIO host bring-up (sequence for `asio_sink.c`)

1. COM-load the driver (the SDK's `AsioDrivers`/`asiolist` helpers handle registry
   enumeration; DVS registers an ASIO driver). `CoInitialize` on the thread.
2. `ASIOInit` → `ASIOGetChannels` (expect ≥26 out) → `ASIOGetBufferSize`.
3. `ASIOGetChannelInfo` per output channel to learn the sample type.
4. `ASIOCreateBuffers` with the `bufferSwitch` / `bufferSwitchTimeInfo` callbacks →
   `ASIOStart`.
5. In `bufferSwitchTimeInfo`, capture `ASIOTime.timeInfo.systemTime` (ns) and
   `samplePosition` for the timestamping path, then run the block (see
   concurrency.md), convert the 26-ch float bus to the driver's sample type, and
   write into the driver buffers for `index`.

Keep the callback allocation-free and lock-free per the invariants in `CLAUDE.md`.

### Implementation note (M1)

This sequence lives in `src/asio_sink.cpp`, behind the device-agnostic `src/sink.h`
seam, so ASIO types never leak into the engine. It compiles only when the ASIO SDK is
vendored — fetch it per [`../third_party/README.md`](../third_party/README.md); CMake
auto-detects `third_party/asiosdk/` and prints `ASIO backend ENABLED`. Without it, the
offline `null_sink.c` backend builds instead: the library always builds and the audio
loop is testable with no hardware. Pick a backend at runtime with `BWAUDIO_SINK`
(`null` | `asio`; default is ASIO with null fallback). The ASIO backend rejects any
driver exposing fewer than 26 output channels.

## Verify before shipping

Several claims in these specs depend on third-party terms/APIs that move faster than
this doc. Re-confirm each against the actual vendored version before distributing or
relying on it:

- [ ] **ASIO SDK license.** The dual GPLv3/proprietary terms above were current as of
  **October 2025**. Read the `LICENSE` in the SDK build you actually vendor and pick
  the option that matches how you ship (GPLv3 like the CI artifact,
  internal/undistributed, or proprietary).
- [ ] **Steam Audio C API.** Confirm `IPLSpeakerLayout` + `IPL_SPEAKERLAYOUTTYPE_CUSTOM`
  (unit-direction custom layouts) and the ambisonics→binaural effect exist in the
  linked version, and that the 3rd-order encode/decode path (see `spatialization.md`)
  is supported.
- [ ] **Other dependency licenses** unchanged in the versions you ship. The pinned
  versions are verified in the "License inventory" above — re-check on any pin bump
  and keep `THIRD_PARTY-NOTICES.md` in sync.
- [ ] **Dante clock.** A hardware leader clock is present on the network — a
  pure-software DVS instance cannot be the standalone leader.
- [ ] **This repo's own license** is GPLv3 (see `LICENSE`), matching the ASIO GPLv3
  option and the CI artifact. Revisit the ASIO and test-engine terms above before
  changing it.
