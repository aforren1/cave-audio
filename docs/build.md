# Build, dependencies, and licensing

## Platform

**Windows only.** ASIO is Windows-only; the Digiface is Windows/macOS. A cross-platform move
means abstracting the device layer: ASIO is only the Windows sink, and the backend
elsewhere would be ALSA/JACK (Linux) or CoreAudio (macOS). Keep ASIO assumptions
confined to `asio_sink.cpp`.

Build system: CMake + MSVC (Visual Studio 2022 toolset). The core is C (C11 with
`stdatomic.h`); Steam Audio and ASIO glue are C/C++.

MSVC gates C11 atomics behind `/experimental:c11atomics`. CMake applies that flag per
source file: `src/rt.c` and `src/stream.c` always, plus `src/steam_reflect.c` in the
with-SDK build (its IR-publish seqlock uses `stdatomic.h` too), both where the DLL
compiles it and in the test targets (`reflect`, `bake`) that compile it directly.

**Race-checking the rings.** The `test_rt_core` target drives the SPSC ring/commit logic
off the real-time path (single-threaded, deterministic) and is what runs under
`ctest` on MSVC (the spatial-feature half lives in `test_rt_feature`; both share
`test/rt_test_util.h`). The full ThreadSanitizer/Helgrind pass needs a
**Clang or Linux** build: MSVC ships no TSan, and Helgrind is Valgrind/Linux. Build
`rt.c` + a two-thread driver with `clang -fsanitize=thread` (or run under Helgrind)
and exercise one producer pushing commands while one consumer drains. Keep that
driver off the RT path: never let the sanitizer harness add allocation/locks to the
callback.

## CMake options

Everything beyond the DLL + test suite is opt-in; the default build stays lean.

| option | default | what it does |
|--------|---------|--------------|
| `BWA_BUILD_TESTS` | ON | the ctest suite (`test_*` targets) |
| `BWA_WITH_ASIO` | OFF* | the ASIO backend. *Auto-flips ON when the SDK sits at `third_party/asiosdk/` |
| `BWA_BUILD_PLAYGROUND` | OFF | `bwa_playground` + `bwa_layout_tool` (fetches raylib/rlImGui/imgui/test-engine) |
| `BWA_BUILD_CALIBVIEW` | OFF | `bwa_calib_view` (fetches imgui/test-engine/implot/implot3d) |
| `BWA_BUILD_CALIBRATE` | OFF | `bwa_calibrate` + `bwa_zylia_probe` |
| `BWA_BUILD_GODOT` | OFF | the Godot GDExtension (fetches godot-cpp - a multi-minute first build). `GODOTCPP_TARGET` picks the library flavour (`editor` default); `tools/godot/pack.ps1` builds both shippable ones. See `bindings/godot/README.md` |
| `BWA_ASAN` | OFF | builds `test_sound` with AddressSanitizer (MSVC; needs tests ON) |
| `BWA_TRACY` | OFF | Tracy profiler instrumentation (fetches Tracy; collects only while a profiler is attached). See [profiling.md](./profiling.md) |
| `BWA_BUILD_BENCH` | OFF | the profiling benches (`bwa_profile_bench` + `bwa_bench_situations`). See [profiling.md](./profiling.md) |

Steam Audio has no option: CMake auto-enables it when the built phonon SDK sits at
`third_party/steam-audio-artifacts/` (see `third_party/README.md`).

### Building without Steam Audio

**A no-SDK build is fully viable for the array**: it is not a degraded mode. You keep the whole
spatializer, plus a complete geometric acoustics path: **image-source early reflections**
(`bwa_source_set_early_reflections`, real parallax as the listener walks), the **directional FDN
reverb** (`bwa_fdn_config`), and **manual occlusion** (`bwa_source_set_occlusion_manual`, driven by
your own game logic). That is what a collaborator site should start from: clone, `cmake`, run.

What the SDK adds, and what you lose without it:

- **Ray-traced occlusion + transmission** against arbitrary geometry: automatic, where the manual
  path needs the game to know the answer. The main reason to build it.
- **Sound pathing** (routing around occluders). No equivalent.
- **A real HRTF binaural monitor.** Without it the `binaural` profile falls back to a lateral pan:
  fine for routing checks, useless for timbre or front/back. This is a *developer-workstation*
  dependency: the production CAVE render is the 26-speaker array, which never uses HRTF.
- The Steam **reflection bed**, which the recommended configuration does not use anyway (see
  [materials.md](./materials.md) → "Choosing an acoustics path").

## Dependencies

| dep            | role                                        | license / notes                          |
|----------------|---------------------------------------------|------------------------------------------|
| Steinberg ASIO SDK | device output to the Digiface; timing hooks       | dual GPLv3 / proprietary (see below)     |
| Steam Audio (C API)| binaural HRTF decode; occlusion, reflections (with baking), pathing - all implemented | Apache-2.0 (`steam-audio-source/LICENSE.md`) |
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
default build or `bw_audio.dll`:

| dep | pin | pulled in by | license |
|-----|-----|--------------|---------|
| imgui | v1.92.8 | PLAYGROUND, CALIBVIEW | MIT |
| imgui_test_engine | v1.92.8 | PLAYGROUND, CALIBVIEW | Dear ImGui Test Engine License - **not MIT**, see below |
| implot | v1.0 | CALIBVIEW | MIT |
| implot3d | v0.4 | CALIBVIEW | MIT |
| raylib | 5.5 | PLAYGROUND | zlib/libpng |
| rlImGui | `Raylib_5_5` tag | PLAYGROUND | zlib/libpng |
| Tracy | v0.13.1 | `BWA_TRACY` | BSD-3-Clause |
| Roboto-Regular | embedded (`examples/roboto_font.h`) | all GUI tools | Apache-2.0 (Google) |

The imgui and imgui_test_engine tags track each other. Bump them together.

### License inventory: what ships where

Everything above, sorted by what it actually ends up in:

- **`bw_audio.dll`** (the artifact CI distributes, under this repo's GPLv3) links four
  third-party components: the ASIO SDK under its **GPLv3** option, Steam Audio's
  `phonon.dll` (**Apache-2.0**, a separate DLL loaded at runtime), dr_libs (**public
  domain / MIT-0**, your choice), and cJSON (**MIT**). All four are GPLv3-compatible,
  so distributing the DLL under GPLv3 is consistent.
- **The GUI tools** (`bwa_playground`, `bwa_layout_tool`, `bwa_calib_view`, opt-in builds)
  additionally compile in imgui, implot, implot3d (**MIT**), raylib, rlImGui
  (**zlib/libpng**), the Roboto face (**Apache-2.0**), imgui_test_engine (**its own
  dual license**: free tier, see below), and optionally Tracy (**BSD-3-Clause**).
  The tools ship in the CI artifact; the notices ride along in
  [`THIRD_PARTY-NOTICES.md`](../THIRD_PARTY-NOTICES.md) (repo root, copied into the
  artifact). Keep that file in sync when a pin bumps.
- **Never linked**: the NatNet SDK. `third_party/NatNetSDK/` sits in the tree as a
  **protocol reference only** (it is proprietary: OptiTrack's plugin license); no
  target compiles or links it, and it must never be distributed with this repo.
  `natnet.c` speaks the documented wire protocol instead; see below.
- **Optional, user-supplied**: Intel Embree + TBB (**Apache-2.0** both) if you drop in
  an Embree-enabled `phonon` for `bwa_desc.embree` (see `docs/api.md`). This repo
  ships neither.

### ASIO SDK licensing (read before distributing)

As of October 2025 the ASIO SDK is **dual-licensed GPLv3 / proprietary** (previously
proprietary-only). You can use it under the GPLv3 option without signing an agreement.
The catch is copyleft. Pick the case that matches how you ship:

- **Distributed under GPLv3** → what this repo does. CI publishes `bw_audio.dll` as a
  workflow artifact under the repo's GPLv3 `LICENSE`. The complete corresponding source
  is this repo at the built commit **plus** the ASIO SDK source that is statically linked
  into the DLL (the repo fetches the SDK at build time rather than vendoring it), so CI
  ships that source as `asio-sdk-src.zip` beside the binaries (a separate release asset,
  `bw_audio-asio-sdk-src-<tag>.zip`, on a tag). That closes the corresponding-source loop
  the fetch-only setup would otherwise leave open; see the CI section below.
- **Internal, undistributed** → GPL obligations don't trigger (copyleft is a
  distribution condition). Use freely.
- **Shipping closed** → take the proprietary ASIO license (the other half of the dual).

This is the shape of the issue, not legal advice. Confirm against the current SDK
license text.

### imgui_test_engine licensing

`imgui_test_engine` is **not MIT**, unlike imgui itself. It ships under the
"Dear ImGui Test Engine License" (v1.04 in the pinned tag): free if you are a natural
person, an open-source project, an educational/research institution, or a small
business under the license's revenue threshold, and a paid license otherwise. Read
`imgui_test_engine/LICENSE.txt` in the fetched tree for the exact criteria.

That's fine here: this repo is GPLv3 open source, so the free tier applies, and the
test engine compiles only into the GUI tools (`bwa_playground`, `bwa_layout_tool`,
`bwa_calib_view`), never into `bw_audio.dll`. The tools ship in the CI artifact with
the notice in `THIRD_PARTY-NOTICES.md`. Revisit if this repo's licensing changes.

### NatNet without the proprietary SDK

NaturalPoint's NatNet SDK is proprietary, which would conflict with GPLv3 under
distribution. The protocol is documented, so `natnet.c` consumes the
multicast/unicast stream directly rather than linking the SDK. The SDK copy at
`third_party/NatNetSDK/` is a protocol reference for that implementation: no target
compiles or links it, and it stays out of anything distributed. It also lets the core
read pose itself (`bwa_tracker_connect`) and sample the freshest head pose at
audio-callback time, lower-latency than marshaling pose through the engine each frame.

## Releasing

The **git tag is the single source of truth for the release version.** You cut a release by
pushing a `v*` tag; nothing else carries a version to bump. Two version streams stay separate,
on purpose:

- **Release version:** the tag (`v0.3.0`). `tools/upm/pack.ps1` stamps it into the packaged
  `package.json` at build time, so the committed manifest is a permanent `0.0.0-dev` placeholder.
  Nothing to keep in sync.
- **ABI version:** `BWA_VERSION_*` in `include/bw_audio.h`, what `bwa_get_version()` returns. It
  tracks binary compatibility (struct and enum layout) and moves only when the ABI changes. Bump it
  by hand, independent of any release.

### Steps

1. **Fill in the CHANGELOG.** Entries land under `## [Unreleased]` in
   `bindings/unity/CHANGELOG.md` as features merge.
2. **Cut it:** `powershell -File tools/release.ps1 0.3.0`. The helper validates the version,
   refuses if the tag already exists or the tree is dirty or `[Unreleased]` is empty, rolls
   `## [Unreleased]` to `## [0.3.0]` (leaving a fresh empty `[Unreleased]`), commits that, and
   creates an annotated `v0.3.0` tag. `-DryRun` previews the roll and changes nothing; `-Push` also
   pushes.
3. **Push:** `git push --follow-tags`. The tag triggers the CI release job.

Prefer to tag by hand? `git tag v0.3.0` works; the helper's only extra service is the CHANGELOG roll.

The tag builds, tests, stamps the version, and cuts a **GitHub Release** with three assets. The
Release IS the distribution: no registry, no token. The asset breakdown and the GPLv3 corresponding
source that rides along are in [Continuous integration](#continuous-integration) below.

### Dev versions

A non-tag build has no tag to stamp, so `pack.ps1` derives the version from `git describe`: the base
tag, the commit distance, and the short hash, as SemVer for UPM (`0.2.0-dev.4.g1a2b3c`; a dirty tree
adds `.dirty`). So a `main` push, a PR, or a local `pack.ps1` run labels itself to a commit instead of
a flat placeholder. The hash and distance sit in the prerelease field, not `+build` metadata, which
older UPM parsers reject. When git cannot answer (no tag, shallow clone, no git on `PATH`) it falls
back to the manifest's `0.0.0-dev`.

## Continuous integration

`.github/workflows/ci.yml` builds and tests on `windows-latest`, and doubles as the
distribution channel:

- **ASIO is built.** The workflow fetches the SDK from Steinberg's official URL
  (cached between runs), and the configure step fails loudly unless the log says
  `ASIO backend ENABLED`: the artifact must contain the production device path.
- **Steam Audio is built, and cached.** CI runs the phonon recipe from
  [`third_party/README.md`](../third_party/README.md) (patched submodule, minimal
  core, `/MD`) and caches `third_party/steam-audio-artifacts/` keyed on the
  submodule sha + the patch hash: the first run pays the phonon build, every later
  run restores it. The four with-SDK tests (`reflect`/`bake`/`path`/`steam_decode`)
  run, and binaural is the real HRTF decode. One CI-only tweak: the pinned phonon
  build scripts hard-code the VS 2022 generator, so the workflow rewrites that to
  whatever Visual Studio the runner image has (via `vswhere`).
- **Tests force the null sink (`bwa_desc.sink = BWA_SINK_NULL`).** Runners have no audio hardware; forcing the
  null sink keeps runs deterministic instead of relying on fallback. The three GUI
  UI suites (`playground`/`layout_tool`/`calib_view`) are built but excluded from
  the CI ctest run: they need a display and OpenGL, which runners don't have; run
  them locally.
- **Two configs, x64 only.** RelWithDebInfo is the full build: tested, tools and
  all. Debug builds the engine (`bw_audio` + `bwa_minimal`) and smoke-runs it, for
  downstream debugging against a debug CRT.
- **The tools are built and shipped.** CI configures with `BWA_BUILD_PLAYGROUND`,
  `BWA_BUILD_CALIBVIEW`, and `BWA_BUILD_CALIBRATE`, so the artifact carries
  `bwa_playground`, `bwa_layout_tool`, `bwa_calib_view` (GUI; they need a display),
  plus `bwa_calibrate` and `bwa_zylia_probe` (console).
- **The Godot binding is built AND tested.** Unlike the imgui tools, Godot's
  `--headless` needs no display, so its scene tests run on the runner (against a
  cached editor download). The addon packs as its own artifact and, on a tag, a
  fourth release asset, plus the `unity`/`godot` distribution branches
  (installable git refs; see `bindings/godot/README.md` and the workflow header).
- **The artifact is a GPLv3 distribution.** Each run uploads `RelWithDebInfo/`
  (engine + phonon + tools) and `Debug/` (engine + phonon) folders plus `bw_audio.h`,
  the example layout + `constraints.json`, the `LICENSE`, `THIRD_PARTY-NOTICES.md`,
  a `DIST.txt` naming the commit and linking the complete source (this repo at
  that commit), and `asio-sdk-src.zip` (the ASIO SDK source statically linked into
  the DLL, redistributed under its GPLv3 option so the corresponding source travels
  with the binary rather than living behind a fetch URL). Downloading requires repo
  access; the artifact carries the GPLv3 terms with it.
- **`bw_audio.dll` needs `phonon.dll` beside it.** With-SDK builds (including the CI
  artifact) link `phonon.lib`, so the DLL won't load without `phonon.dll` in the
  same directory; keep the pair together. The CMake post-build step and the Unity
  plugin staging both copy it for you. Only a no-SDK build is self-contained (with
  the simple-pan binaural fallback and acoustics calls as no-ops).
- **Two audiences, two artifacts.** Every run uploads them separately: the engine
  (`bw_audio-win64-<ver>-r<N>`: dll/lib/pdb + phonon + tools + header) and the Unity package
  (`unity-package-<ver>-r<N>`: one `.tgz`). `<ver>` is the packed version (the tag on a release,
  a git-describe dev version otherwise); the `r<N>` run number keeps re-runs of one commit from
  colliding on a name. Downloading one no longer drags in the other.
- **The Unity package is packed every run, and released on a tag.**
  `tools/upm/pack.ps1` produces `com.brainworks.bw_audio-<version>.tgz`, the C#
  binding with both DLLs inside it, so a broken package (a missing `.meta`, a lost
  plugin) fails the *build*, not a release. A `v*` tag then cuts a GitHub Release, and
  **the Release is the distribution**: there is no registry, no token, nothing to keep
  in sync. Unity installs the tarball directly (Package Manager → `+` → *Install
  package from tarball…*). The tag stamps the version into the packaged manifest (the committed
  `package.json` stays `0.0.0-dev`), so a tarball can never claim a version it isn't and there is no
  manifest field to bump. See [Releasing](#releasing) for the maintainer steps.
- **A release carries THREE assets**, because workflow artifacts expire (30 days) and a
  release doesn't:
  - `com.brainworks.bw_audio-<ver>.tgz`: the Unity package.
  - `bw_audio-win64-<tag>.zip`: the engine itself (`bw_audio.dll`/`.lib`/`.pdb` +
    `phonon.dll`, `bw_audio.h`, and the tools). This is the durable download for a C/C++
    consumer or the CAVE machine.
  - `bw_audio-asio-sdk-src-<tag>.zip`: the ASIO SDK source statically linked into the DLL,
    kept as its own asset so it accompanies the binaries (GPLv3 corresponding source)
    without bloating either the engine `.zip` or the `.tgz`.

  All ship under GPLv3 (the `.zip` carries `LICENSE`, `THIRD_PARTY-NOTICES.md`, and
  `DIST.txt`, which names the commit and links the complete source; the `.tgz` carries
  the same inside it, and both point at the SDK-source asset for the one GPL-covered
  component the repo fetches rather than vendors): an app that ships this DLL to third
  parties inherits GPLv3; see the ASIO section.

  Two constraints worth knowing before changing any of this:

  1. **The package's `.meta` files are committed on purpose**
     (`tools/upm/gen-meta.ps1`): an installed package is immutable, so assets without a
     `.meta` get a fresh GUID per project and scenes lose their script references. The
     native plugins' import settings (Windows x64, Editor on) ship the same way; they
     cannot be fixed in the Inspector afterwards.
  2. **Keep the engine bundle a `.zip`, and the package the only `.tgz` on a release.**
     A UPM registry (they all speak the npm protocol) keys on a single publishable
     tarball. Nothing is listed on one today (the audience already has the repo, and a
     public listing would invite installs into projects the GPLv3 would surprise), but
     the tarball IS what a registry would serve, so listing it later stays a config
     change rather than a rebuild. Preserving that costs nothing.

## Dante configuration

The endpoint is an **RME Digiface Dante**: a hardware Dante interface that presents ASIO
directly. Dante is still the transport to the amps, so everything about the network below
is unchanged; what a hardware endpoint buys over a software one is in the clock and
isolation bullets.

- **Driver: ASIO.** The device must expose **enough output channels for your layout**:
  26 for the CAVE array; fewer for a smaller install (the engine's channel count is the
  layout's speaker count, 4..26). Use the ASIO driver, not the WDM/DirectSound one: WDM is
  a consumer path with its own mixing and resampling, and it is not the multichannel
  low-latency route the array needs.
- **Format:** 48 kHz, 24-bit. Match the bit depth end-to-end. Read the driver's reported
  sample type via `ASIOGetChannelInfo` and convert rather than assuming: the engine renders
  float and the device may want Int32 or packed Int24.
- **Channel count at rate.** Confirm the device still offers your channel count at the rate
  you intend to run. Dante endpoints commonly **reduce channel count at 96 kHz**, and 26 out
  plus 19 Zylia inputs is 45 channels. Calibrate and validate at 48 kHz.
- **Clock:** a Dante network needs one **leader clock**, and a hardware endpoint can *be* it.
  That is the practical gain over a software endpoint, which has to follow some other node on
  the net. Either let the Digiface lead, or point it at whichever hardware node you prefer as
  leader, but pick deliberately and check it in Dante Controller, because an unlocked domain
  shows up as a sample rate that wanders between runs (Stage 0 of the runbook measures this).
- **Latency:** start ASIO buffer ~512–1024 and Dante latency 4–10 ms, then tighten
  empirically against measured dropouts.
- **Isolation:** less of a concern than with a software endpoint. A software Dante driver
  packetizes on the host CPU, so it competes with engine frame-rate spikes and is worth
  moving to its own machine; a hardware endpoint does that work on the device. The
  control-only C ABI still tolerates splitting the machines if you want to.

## ASIO host bring-up (sequence for `asio_sink.c`)

1. COM-load the driver (the SDK's `AsioDrivers`/`asiolist` helpers handle registry
   enumeration; the Digiface registers an ASIO driver). `CoInitialize` on the thread.
2. `ASIOInit` → `ASIOGetChannels` (expect ≥ the layout's speaker count out; 26 for the
   CAVE) → `ASIOGetBufferSize`.
3. `ASIOGetChannelInfo` per output channel to learn the sample type.
4. `ASIOCreateBuffers` with the `bufferSwitch` / `bufferSwitchTimeInfo` callbacks →
   `ASIOStart`.
5. In `bufferSwitchTimeInfo`, capture `ASIOTime.timeInfo.systemTime` (ns) and
   `samplePosition` for the timestamping path, then run the block (see
   concurrency.md), convert the float speaker bus to the driver's sample type, and
   write into the driver buffers for `index`.

Keep the callback allocation-free and lock-free per the invariants in `CLAUDE.md`.

### Implementation note

This sequence lives in `src/asio_sink.cpp`, behind the device-agnostic `src/sink.h`
seam, so ASIO types never leak into the engine. It compiles only when the ASIO SDK is
vendored: fetch it per [`../third_party/README.md`](../third_party/README.md); CMake
auto-detects `third_party/asiosdk/` and prints `ASIO backend ENABLED`. Without it, the
offline `null_sink.c` backend builds instead: the library always builds and the audio
loop is testable with no hardware. Pick a backend with `bwa_desc.sink`
(`null` | `asio`; default is ASIO with null fallback). The ASIO backend rejects any
driver exposing fewer output channels than the layout needs (26 for the CAVE array).

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
  versions are verified in the "License inventory" above; re-check on any pin bump
  and keep `THIRD_PARTY-NOTICES.md` in sync.
- [ ] **Dante clock.** Exactly one leader on the network, chosen deliberately. The
  Digiface is hardware, so it can lead; confirm in Dante Controller either way.
- [ ] **This repo's own license** is GPLv3 (see `LICENSE`), matching the ASIO GPLv3
  option and the CI artifact. Revisit the ASIO and test-engine terms above before
  changing it.
