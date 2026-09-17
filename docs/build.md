# Build, dependencies, and licensing

## Platform

**Windows is the production platform.** ASIO is Windows-only; the Digiface is Windows and macOS.
ASIO is only the Windows sink for the **array**. Headphone output has a second Windows backend,
**WASAPI** (`src/sink/wasapi_sink.cpp`), so a desk machine needs no ASIO driver at all: `binaural` and
`cave_sim` open the Windows default output, and `cave_both` opens ASIO for the array and WASAPI
for the monitor at the same time.

**Linux has two device backends**, JACK (`src/sink/jack_sink.c`) and ALSA (`src/sink/alsa_sink.c`). JACK is
the production path: one binary talks to a JACK2 server or to PipeWire through pipewire-jack, and
which one answers is decided at run time by what the box has installed. ALSA is the no-server path,
for an experiment that wants a raw card clock or a rig driving a multichannel card directly. Under
`BWA_SINK_AUTO` a Linux box tries JACK, then ALSA, then the null sink, at any channel count.

Neither library is linked. Both sinks load libjack and alsa-lib at run time, so the development
packages are a BUILD-time requirement (for the headers) and the libraries themselves are optional
at run time: a box with neither loads `libbw_audio.so` and runs the offline sink, and each backend
reports itself unavailable with the library named. See [Linux notes](#linux-notes) below.

**Android has one device backend**, AAudio (`src/sink/aaudio_sink.c`), which is the headphone path on
a standalone VR headset. It is stereo only: Android carries no array transport, so a request wider
than two channels falls to the offline sink. The library cross-builds with the NDK against API 26
or later. See [Android](#android) below.

**macOS builds the library, the tests and the console examples** with clang, on the **null and
manual sinks**. That is the whole platform surface there for now: there is no device backend, so
nothing reaches a speaker. What it buys is the offline path (`bwa_render_block` renders
bit-identically anywhere), a place to run ThreadSanitizer, and a CI gate that catches a Win32 call
sneaking back into the core. CoreAudio is specified, not implemented, in
[backends.md](./backends.md). Keep each backend's assumptions confined to its own `*_sink` file.

Everything platform-specific OUTSIDE the sinks goes through one shim, `src/os/os.h`, with
`src/os/os_win.c` and `src/os/os_posix.c` behind it: threads, sleep, the monotonic clock, an
absolute-deadline sleep, mutexes and a reader/writer lock, thread priority, `strdup` and
`strcasecmp`, and the UDP socket calls `natnet.c` makes. Nothing else in `src/` includes
`windows.h`. The GUI tools (`bwa_playground`, `bwa_layout_tool`, `bwa_calib_view`) and the
capture tools (`bwa_calibrate`, `bwa_validate`, `bwa_zylia_probe`) stay Windows-only targets and
are skipped with a status message elsewhere.

Build system: CMake, with MSVC (Visual Studio 2022 toolset) on Windows and gcc or clang
elsewhere. The core is C (C11 with `stdatomic.h`); Steam Audio and ASIO glue are C/C++. Off
Windows the library builds with `-fvisibility=hidden`, so the public ABI is what `BWA_API` marks
and nothing else.

MSVC gates C11 atomics behind `/experimental:c11atomics`. CMake applies that flag per source
file, and source properties are directory-scoped so one entry covers every target that compiles
the file. The list is `rt.c`, `stream.c`, `assets.c`, `fdn.c`, `natnet.c`, `engine.c`,
`null_sink.c`, `profile_self.c`, `steam_scene.c`, `steam_path.c` and `steam_reflect.c`, plus the
three test sources that reach the atomics directly (`test/os_test.c`, `test/natnet_test.c`,
`test/audio_sink_test.c`, `test/rt_feature_test.c`).

`src/tracking/pose.h` is the one HEADER that carries `stdatomic.h`, so `rt.h` and `natnet.h` forward-declare
`PoseSlot` instead of including it. Include `pose.h` only where the seqlock is actually used, or
the flag spreads to every consumer of those headers.

The C++ files (`src/sink/asio_sink.cpp`, `src/sink/wasapi_sink.cpp`) need `/std:c++20` for designated
initializers in their vtables. CMake asks for it per target rather than globally, so it cannot
leak into the vendored SDK sources.

**Race-checking the rings.** The `test_rt_core` target drives the SPSC ring/commit logic
off the real-time path (single-threaded, deterministic). It is what runs under
`ctest` on MSVC; the spatial-feature half lives in `test_rt_feature`, and both share
`test/rt_test_util.h`. `test_os` adds the genuinely concurrent part: two threads on the mutex,
and a writer and a reader hammering the `pose.h` seqlock with torn-read detection.

ThreadSanitizer needs a Linux or clang build, which the port now makes possible:

```
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_C_FLAGS="-fsanitize=thread -g" \
      -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread \
      -DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=thread
cmake --build build-tsan --target test_rt_core test_os
./build-tsan/test_rt_core && ./build-tsan/test_os
```

Two things to know before reading a report. gcc warns that `atomic_thread_fence` is not modeled
under `-fsanitize=thread`, so a fence-based seqlock (`pose.h`) can produce a FALSE report there;
judge one against the code, not the tool. And a sanitizer build runs about two orders of magnitude
slower, which is why `test_os` sets its contention floor low. Under WSL, ThreadSanitizer needs
`setarch -R` (or `vm.mmap_rnd_bits=28`) or it aborts with "unexpected memory mapping".

## CMake options

Everything beyond the DLL + test suite is opt-in; the default build stays lean.

| option | default | what it does |
|--------|---------|--------------|
| `BWA_BUILD_TESTS` | ON | the ctest suite (`test_*` targets) |
| `BWA_WITH_ASIO` | OFF* | the ASIO backend. *Auto-flips ON when the SDK sits at `third_party/asiosdk/` |
| `BWA_WITH_WASAPI` | ON | the WASAPI backend (headphone profiles, the `cave_both` monitor). Windows only, and forced OFF elsewhere. Needs no SDK: the headers ship with the Windows SDK, and it links `ole32` + `avrt` |
| `BWA_WITH_JACK` | ON | the JACK backend. Linux only, and forced OFF elsewhere. Needs `pkg-config jack` to succeed for the HEADERS (`libjack-jackd2-dev`, or pipewire-jack's development package); it turns itself off with a status line when that fails. The library itself is loaded at run time, never linked |
| `BWA_WITH_ALSA` | ON | the ALSA backend. Linux only, and forced OFF elsewhere. Needs `find_package(ALSA)` to succeed for the HEADERS (`libasound2-dev`); same self-disabling behavior, and the same run-time load |
| `BWA_WITH_AAUDIO` | ON | the AAudio backend. Android only, and forced OFF elsewhere. Needs no SDK: the header ships with the NDK and it links `aaudio`. Fails the configure when `ANDROID_PLATFORM` is under `android-26`, which is AAudio's own floor |
| `BWA_BUILD_PLAYGROUND` | OFF | `bwa_playground` + `bwa_layout_tool` (fetches raylib/rlImGui/imgui/test-engine) |
| `BWA_BUILD_CALIBVIEW` | OFF | `bwa_calib_view` (fetches imgui/test-engine/implot/implot3d) |
| `BWA_BUILD_CALIBRATE` | OFF | `bwa_calibrate` + `bwa_zylia_probe` |
| `BWA_BUILD_GODOT` | OFF | the Godot GDExtension (fetches godot-cpp - a multi-minute first build). `GODOTCPP_TARGET` picks the library flavor (`editor` default); `tools/godot/pack.ps1` builds both shippable ones. See `bindings/godot/README.md` |
| `BWA_BUILD_PYTHON` | OFF | the Python binding (`_bwa`, nanobind). Needs Python 3.9 or later with `nanobind` importable by the interpreter CMake finds; 3.12 or later gets the stable ABI. Adds three ctests (`python_bindings` plus the two examples). `uv build --wheel` in `bindings/python` takes this same path. See `bindings/python/README.md` |
| `BWA_BUILD_MATLAB` | OFF | the MATLAB and Octave binding (`bwa_mex`, a classic C MEX gateway). Builds whichever of the two toolchains CMake finds, and both when both are there: MATLAB through `matlab_add_mex` (needs a configured C compiler, `mex -setup C`), Octave through `mkoctfile`. Adds up to four ctests per interpreter found. See `bindings/matlab/README.md` |
| `BWA_ENGINE_SDK` | empty | build ONLY the bindings, against a prebuilt engine installed at this prefix. Nothing under `src/` compiles, and no test or tool is registered. See [The engine SDK](#the-engine-sdk) |
| `BWA_ASAN` | OFF | builds `test_sound` with AddressSanitizer (MSVC; needs tests ON) |
| `BWA_TRACY` | OFF | Tracy profiler instrumentation (fetches Tracy; collects only while a profiler is attached). See [profiling.md](./profiling.md) |
| `BWA_BUILD_BENCH` | OFF | the profiling benches (`bwa_profile_bench` + `bwa_bench_situations`). See [profiling.md](./profiling.md) |

Steam Audio has no option: CMake auto-enables it when the built phonon SDK sits at
`third_party/steam-audio-artifacts/` (see `third_party/README.md`). It is linked **statically**, as
four archives under `lib/<platform>/`: phonon itself plus its three companions (mysofa, zlib and
pffft). Nothing ships beside the engine library. If the directory for your platform is there but
incomplete, the configure fails and says so, because silently dropping the HRTF decode, occlusion,
reflections and pathing out of a build is the worst way to learn the staging is stale.

CI builds and stages phonon for **Windows**, **Linux**, **macOS** and **Android** (one per ABI),
each job into its own
`lib/<platform>/` and under its own cache key. A checkout can carry several sets at once, and the
detection picks the one for the platform you are configuring. The Android recipe is in
`third_party/README.md`, including the two android-only patches it applies. The Linux recipe is there too, including
the one flag it takes (`-DSTEAMAUDIO_ENABLE_AVX=OFF` on gcc 13 and older). The macOS build runs
**only in CI**, because nobody here has a Mac to verify it on.

### The engine SDK

Build the engine once, then link every binding against that one file.

An ordinary build compiles the engine into whatever tree you configured. That is fine for one
build, but each binding used to pull the engine in as a source dependency, so a job that built the
engine, two Godot extension flavors and a wheel compiled the engine four times and shipped four
files that were only meant to be the same. The SDK removes that.

**Install it.** Any build tree can produce one:

```
cmake -S . -B build -A x64
cmake --build build --config RelWithDebInfo
cmake --install build --prefix /path/to/sdk --component bwa_sdk --config RelWithDebInfo
```

`--component bwa_sdk` is required. The same tree also carries raylib's install rules and the
Python binding's, and an unfiltered install mixes all three into one prefix.

What you get:

```
<sdk>/include/bw_audio.h
<sdk>/lib/cmake/bw_audio/          bw_audioConfig.cmake + the version file + the exported targets
<sdk>/lib/                         bw_audio.lib | libbw_audio.so | libbw_audio.dylib
<sdk>/bin/                         bw_audio.dll + bw_audio.pdb            (Windows only)
<sdk>/LICENSE, THIRD_PARTY-NOTICES.md
```

Nothing is stripped. The library goes in as the build produced it, because the consumers that want
symbols gone already remove them at their own staging step.

**Use it from this repo.** One switch turns the root into bindings-only mode:

```
cmake -S . -B build-bind -A x64 -DBWA_ENGINE_SDK=/path/to/sdk -DBWA_BUILD_MATLAB=ON
cmake --build build-bind --config RelWithDebInfo
ctest --test-dir build-bind -C RelWithDebInfo
```

The configure prints `bw_audio: SDK MODE`, the ABI version the SDK carries and the configurations
it holds. `ctest -N` in that tree lists the binding tests and nothing else. The repo root stays the
CMake source directory in both modes, and both give the bindings one target name,
`bwa::bw_audio`, so a binding never asks which mode built it.

Use the same configuration on both sides where you can. An SDK installed from a RelWithDebInfo
build and consumed from a Debug tree works (CMake falls back to whatever configuration the imported
target holds), but the SDK-mode configure prints what it found so you can see it.

**Use it for a wheel.** `uv build` passes a config setting through to scikit-build-core, which puts
it on the CMake command line:

```
cd bindings/python
uv build --wheel --python 3.12 -C cmake.define.BWA_ENGINE_SDK=/path/to/sdk
```

Without it, the wheel build compiles a second full engine inside its own isolated build directory.

**Use it from your own project.** It is a normal CMake package, so a C client outside this repo
needs no part of this repo:

```cmake
find_package(bw_audio CONFIG REQUIRED PATHS /path/to/sdk)
target_link_libraries(my_app PRIVATE bwa::bw_audio)
```

**The guard.** The gateway and the library can now come from different builds, so both scripted
bindings check at load. `+bwa/setup.m` compares the MEX gateway's compiled `BWA_VERSION` against
`bwa_get_version()`; `bw_audio/__init__.py` does the same at import and raises `ImportError`. A
package built from one tree cannot trip either one.

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
| libjack        | the Linux JACK backend                       | LGPL-2.1. Headers at build time, `libjack.so.0` loaded at run time (never linked); pipewire-jack's drop-in (MIT) answers the same ABI |
| alsa-lib       | the Linux ALSA backend                       | LGPL-2.1. Headers at build time, `libasound.so.2` loaded at run time (never linked) |
| libaaudio      | the Android AAudio backend                   | part of the Android system image; the NDK ships the stub to link against. No license line to add |
| NatNet         | OptiTrack pose ingest                        | consume off-wire; see below              |

Two dependencies live under `third_party/`: `asiosdk/`, and the Steam Audio pair
(`steam-audio-source/` submodule + the built `steam-audio-artifacts/`). Neither is
**vendored**, that is, neither is committed to the repo, and you put both in place
yourself; `third_party/README.md` has the fetch/build steps.

dr_libs and cJSON need no such step. CMake fetches both via `FetchContent`, pinned
to exact commits. For an offline build, override with `-DFETCHCONTENT_SOURCE_DIR_<NAME>=<dir>`.

### Tool dependencies (opt-in builds only)

The GUI tools and the profiler pull their own pinned dependencies. None of these touch
the default build or `bw_audio.dll`:

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
  third-party components: the ASIO SDK under its **GPLv3** option, Steam Audio
  (**Apache-2.0**, statically linked, so phonon and its mysofa, zlib and pffft archives
  are inside this DLL), dr_libs (**public domain / MIT-0**, your choice), and cJSON
  (**MIT**). All four are GPLv3-compatible, so distributing the DLL under GPLv3 is
  consistent. Two of them ship their source beside the binaries rather than behind a
  link: `bw_audio-asio-sdk-src-<tag>.zip` and `bw_audio-steam-audio-src-<tag>.zip`.
  Apache-2.0 does not require the second one (Steam Audio ships no `NOTICE` file, so
  section 4(d) never triggers, and the pinned commit plus the in-repo patch is a
  complete recipe). It ships anyway, for parity: once phonon is inside the DLL rather
  than beside it, GPLv3 section 6 governs the combined work, and handing a recipient
  the source costs a release asset instead of an argument.
- **The GUI tools** (`bwa_playground`, `bwa_layout_tool`, `bwa_calib_view`, opt-in builds)
  additionally compile in imgui, implot, implot3d (**MIT**), raylib, rlImGui
  (**zlib/libpng**), the Roboto face (**Apache-2.0**), imgui_test_engine (**its own
  dual license**: free tier, see below), and optionally Tracy (**BSD-3-Clause**).
  The tools ship in the CI artifact; the notices ride along in
  [`THIRD_PARTY-NOTICES.md`](../THIRD_PARTY-NOTICES.md) (repo root, copied into the
  artifact). Keep that file in sync when a pin bumps.
- **`libbw_audio.so`** (Linux, Android) links the same dr_libs and cJSON, plus whichever system
  audio libraries the platform has: libjack and alsa-lib on Linux (**LGPL-2.1** both, loaded at run
  time with `dlopen` rather than linked), libaaudio on Android. None of the three is redistributed, and libaaudio is part of the
  Android system image, so it adds no notice at all. No ASIO there: it is a Windows driver model.
- **The Android artifact** (`bw_audio-android-<ver>`, the `lib/arm64-v8a` and `lib/x86_64`
  libraries, and the copies inside the Unity package and the Godot addon) is that same
  `libbw_audio.so`, so it carries dr_libs, cJSON and a statically linked Steam Audio (**Apache-2.0**,
  the same obligation the Windows DLL carries). No ASIO: it is a Windows driver model. It travels with `LICENSE`,
  `THIRD_PARTY-NOTICES.md` and a `DIST.txt` naming the commit, like every other artifact.
- **The Linux artifact** (`bw_audio-linux-x64-<ver>`, and the copies inside both bindings) is the
  same `libbw_audio.so` again: dr_libs, cJSON and a statically linked Steam Audio, plus libjack and
  alsa-lib (**LGPL-2.1** both) loaded with `dlopen` on the first device query or open. Neither of
  those two is redistributed, so neither adds a file to ship, but both add a notice line. Neither
  is required to be present: the library loads and runs the offline sink without them. No ASIO.
- **The macOS artifact** (`bw_audio-macos-universal-<ver>`, and the copies inside both bindings) is
  one universal `libbw_audio.dylib` carrying dr_libs, cJSON and a statically linked Steam Audio. No
  system audio library at all yet, because macOS has no device backend, and no ASIO. It is **not
  code-signed and not notarized**, which is a distribution question rather than a license one:
  internal use clears the quarantine flag, and shipping outside the lab needs a Developer ID
  signature and notarization.
- **The Godot GDExtension** (`bw_audio_gd.*`, one per platform and flavor inside the addon) links
  godot-cpp (**MIT**) beside the engine's C ABI. It ships with the addon's `LICENSE` and
  `THIRD_PARTY-NOTICES.md`, like the engine library it sits next to.
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
  into the DLL. The repo fetches the SDK at build time rather than vendoring it, so CI
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
"Dear ImGui Test Engine License" (v1.04 in the pinned tag). That license is free if
you are a natural person, an open-source project, an educational/research institution,
or a small business under the license's revenue threshold. Otherwise you need a paid
license. Read `imgui_test_engine/LICENSE.txt` in the fetched tree for the exact criteria.

That's fine here: this repo is GPLv3 open source, so the free tier applies. The
test engine compiles only into the GUI tools (`bwa_playground`, `bwa_layout_tool`,
`bwa_calib_view`), never into `bw_audio.dll`. The tools ship in the CI artifact with
the notice in `THIRD_PARTY-NOTICES.md`. Revisit if this repo's licensing changes.

### NatNet without the proprietary SDK

NaturalPoint's NatNet SDK is proprietary, which would conflict with GPLv3 under
distribution. The protocol is documented, so `natnet.c` consumes the
multicast/unicast stream directly rather than linking the SDK. The SDK copy at
`third_party/NatNetSDK/` is a protocol reference for that implementation: no target
compiles or links it, and it stays out of anything distributed. Reading the wire
protocol directly also lets the engine read pose itself (`bwa_tracker_connect`) and
sample the freshest head pose at audio-callback time. That is lower-latency than
marshaling pose through the engine each frame.

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
2. **Cut it:** `powershell -File tools/release.ps1 0.3.0`. The helper validates the version and
   refuses if the tag already exists, the tree is dirty, or `[Unreleased]` is empty. It then rolls
   `## [Unreleased]` to `## [0.3.0]` (leaving a fresh empty `[Unreleased]`), commits that, and
   creates an annotated `v0.3.0` tag. `-DryRun` previews the roll and changes nothing; `-Push` also
   pushes.
3. **Push:** `git push --follow-tags`. The tag triggers the CI release job.

Prefer to tag by hand? `git tag v0.3.0` works; the helper's only extra service is the CHANGELOG roll.

That job builds, tests, stamps the version, and cuts a **GitHub Release**. The Release IS the
distribution: no registry, no token. The asset breakdown and the GPLv3 corresponding source that
rides along are in [Continuous integration](#continuous-integration) below.

The Python wheel is the one asset whose filename does **not** carry the tag. Its version is the ABI
version read out of `include/bw_audio.h` at build time, which is a third thing a wheel could claim
and must not: a wheel that said `0.5.0` while the library inside it answered `0.14.0` to
`bwa_get_version` would be unfixable from the outside. See `bindings/python/README.md`.

### Dev versions

A non-tag build has no tag to stamp, so `pack.ps1` derives the version from `git describe`: the base
tag, the commit distance, and the short hash, as SemVer for UPM (`0.2.0-dev.4.g1a2b3c`; a dirty tree
adds `.dirty`). So a `main` push, a PR, or a local `pack.ps1` run labels itself to a commit instead of
a flat placeholder. The hash and distance sit in the prerelease field, not `+build` metadata, which
older UPM parsers reject. When git cannot answer (no tag, shallow clone, no git on `PATH`),
`pack.ps1` falls back to the manifest's `0.0.0-dev`.

## Continuous integration

`.github/workflows/ci.yml` builds and tests on `windows-latest`, `ubuntu-latest` and
`macos-latest`. A separate `package` job is the distribution channel:

- **One engine per platform.** Each desktop job builds the engine once, runs ctest on it, and
  installs it as an [engine SDK](#the-engine-sdk). Every binding in that job then configures with
  `-DBWA_ENGINE_SDK` against that tree: the MATLAB MEX, the Octave MEX, both Godot extension
  flavors and the wheel. So the binary a package ships is the binary that job tested, and each job
  builds one engine where it used to build four. Every step that stages the engine somewhere
  asserts the staged file against the SDK's, byte for byte where it can and by build id or UUID
  where the copy is stripped, so the claim cannot rot quietly.
- **The Linux engine is built inside the manylinux container.** A `manylinux_2_28` wheel needs an
  engine built against glibc 2.28 and `ubuntu-latest` carries 2.39, so until this the repository
  built two Linux engines: one on the runner for ctest, the MEX pair and the Godot addon, and a
  second inside cibuildwheel for the release wheel. `tools/ci/build-engine-manylinux.sh` now
  builds, tests and installs the one Linux engine inside that image, and everything else takes it
  from there. Only the engine step runs in the container. MATLAB and the Godot toolchain do not
  install in AlmaLinux 8 and do not need to: a binding compiled on the runner against a
  glibc-2.28 shared object is an ordinary ABI client, and the reverse is what does not work.

- **ASIO is built.** The workflow fetches the SDK from Steinberg's official URL
  (cached between runs). The configure step fails loudly unless the log says
  `ASIO backend ENABLED`: the artifact must contain the production device path.
- **Steam Audio is built static, and cached.** The recipe from
  [`third_party/README.md`](../third_party/README.md) (patched submodule, minimal
  core, `/MD`, `BUILD_SHARED_LIBS=OFF`) lives in the composite action
  `.github/actions/build-phonon/`, which takes a platform, an arch, a toolchain and a
  staging directory. All four jobs call it, the android job twice, once per ABI. CI caches
  `third_party/steam-audio-artifacts/` keyed on the submodule sha + the patch hash, with
  the platform and, on Android, the ABI in the key so no job restores another's archives (a cache
  is scoped to the repository, not to the runner OS): the
  first run pays the phonon build, every later run restores it. The five with-SDK tests
  (`reflect`/`bake`/`path`/`dynmesh`/`steam_decode`) run, and binaural is the real HRTF
  decode. One CI-only tweak: the pinned phonon build scripts hard-code the VS 2022
  generator, so the action rewrites that to whatever Visual Studio the runner image has.
- **Tests force the null sink (`bwa_desc.sink = BWA_SINK_NULL`).** Runners have no audio hardware; forcing the
  null sink keeps runs deterministic instead of relying on fallback. The three GUI
  test suites (`playground`/`layout_tool`/`calib_view`) are built but excluded from
  the CI ctest run. They need a display and OpenGL, which runners don't have. Run
  them locally.
- **Two configs, x64 only.** RelWithDebInfo is the full build: tested, tools and
  all. Debug builds the engine (`bw_audio` + `bwa_minimal`) and smoke-runs it, for
  stepping through engine code unoptimized. It uses the **release** CRT, like every
  other config in a with-SDK build: the staged phonon archive is built `/MD`, and a
  static archive carries that choice. Your own application's CRT is unaffected, since
  it only ever loads this DLL across a C ABI.
- **The tools are built and shipped.** CI configures with `BWA_BUILD_PLAYGROUND`,
  `BWA_BUILD_CALIBVIEW`, and `BWA_BUILD_CALIBRATE`, so the artifact carries
  `bwa_playground`, `bwa_layout_tool`, `bwa_calib_view` (GUI; they need a display),
  plus `bwa_calibrate` and `bwa_zylia_probe` (console).
- **The Godot binding is built AND tested.** Unlike the imgui tools, Godot's
  `--headless` needs no display, so its scene tests run on the runner (against a
  cached editor download). The addon packs as its own artifact and, on a tag, its
  own release asset, plus the `unity`/`godot` distribution branches
  (installable git refs; see `bindings/godot/README.md` and the workflow header).
- **The artifact is a GPLv3 distribution.** Each run uploads `RelWithDebInfo/`
  (engine + tools) and `Debug/` (engine) plus `bw_audio.h`, the
  example layout + `constraints.json`, `LICENSE`, `THIRD_PARTY-NOTICES.md`, a
  `DIST.txt` naming the commit, and `asio-sdk-src.zip` (the statically linked ASIO
  SDK source, so the GPLv3 corresponding source travels with the binary rather
  than living behind a fetch URL).
- **`bw_audio.dll` stands alone.** Steam Audio is linked statically, so a with-SDK build
  and a no-SDK build ship the same one file and there is nothing to keep beside it. This
  used to be the opposite rule (a `phonon.dll` the loader needed in the same directory),
  so delete any stale copy you find next to an engine build: it is dead weight, and the
  DLL never looks for it.
- **Android is cross-built in its own job.** `android` on `ubuntu-latest` configures with the
  runner's NDK (`ANDROID_NDK_ROOT`, the image's pinned default, with `ANDROID_NDK_LATEST_HOME` as
  the fallback) for `arm64-v8a` and `x86_64` at `android-26`, asserts the configure log says
  `AAudio backend ENABLED` and `Steam Audio ENABLED`, and builds the library, the examples and the
  tests. It builds a static phonon per ABI first, through the same composite action, so that second
  assertion is also what proves the staging survived. Nothing runs
  there: the binaries are the device's, which is what `tools\android\run-tests.ps1` is for. The
  job also builds the Godot GDExtension for `arm64-v8a` and hands both libraries to the `package`
  job, which is the only place that packs the bindings. It uploads
  `bw_audio-android-<ver>-r<N>`: `lib/arm64-v8a/libbw_audio.so`, `lib/x86_64/libbw_audio.so`,
  `include/bw_audio.h`, `LICENSE`, `THIRD_PARTY-NOTICES.md`, and a `DIST.txt` naming the commit.
- **Two audiences, separate artifacts.** Every run uploads them separately: the engine
  (`bw_audio-win64-<ver>-r<N>`: dll/lib/pdb + tools + header), the Unity package
  (`unity-package-<ver>-r<N>`: one `.tgz`) and the Godot addon (`godot-addon-<ver>-r<N>`), plus
  the Android, Linux and macOS engine artifacts their own jobs upload. `<ver>` is the packed version (the tag on a release,
  a git-describe dev version otherwise); the `r<N>` run number keeps re-runs of one commit from
  colliding on a name. Downloading one no longer drags in the other.
- **The Python wheel is built and tested on all three desktops.** The `windows`, `linux` and
  `macos` jobs each run `uv build --wheel` in `bindings/python`, assert the filename is tagged
  `cp312-abi3` (a version-specific tag means nanobind's stable ABI did not engage, which would
  mean one wheel per Python instead of one per platform), install it into a fresh venv, and run the
  pytest suite and both examples **from the installed wheel** rather than from the source tree.
  Each uploads `bw_audio-python-<platform>-<ver>-r<N>`. The wheel carries the engine library inside
  the package, so it is that job's own build. These three are the **fast gate**: they prove the
  binding still builds and tests where the engine was just built, and they tag for the machine
  that built them. The Android job builds no wheel: there is no Python there.
  `bindings/python/README.md` is the manual.
- **Both MEX files are built inside each desktop job, and the six become one toolbox.** A MEX is
  per platform and per interpreter, so the `windows`, `linux` and `macos` jobs each set up MATLAB with
  [matlab-actions/setup-matlab](https://github.com/matlab-actions/setup-matlab) (no license token
  is needed on a GitHub-hosted runner for a public repository), reconfigure **the same bindings
  tree** the Octave MEX used, build only the MEX target, and run the suite and all three examples
  through `matlab-actions/run-command`. Linking the job's own engine is the point: the MEX loads
  the SDK's library, with ASIO and WASAPI on Windows, JACK and ALSA on Linux, and the universal
  dylib on macOS, and no engine is compiled there at all. Each job asserts the configure log says
  `MATLAB MEX enabled`, because the option skips silently when it finds no toolchain and a job that
  built nothing would otherwise pass.
  The `package` job then assembles `bw_audio-matlab/`: `+bwa`, the examples, the README, the
  licenses, and `bin/win64`, `bin/glnxa64` and `bin/maca64` each holding that platform's two MEX
  files plus the one engine library both of them load. It asserts all six are present before
  zipping. That folder uploads as `bw_audio-matlab-<ver>-r<N>` and, on a tag, as
  `bw_audio-matlab-<tag>.zip`.
  The macOS runner is Apple silicon, so its MEX files are arm64 against the universal dylib; an
  Intel Mac would need MEX files built on one, which nothing here has.
  **Octave is built and tested in all three desktop jobs.** Each installs its own: `octave` and
  `octave-dev` from apt on Linux, `octave.portable` from Chocolatey on Windows, `octave` from
  Homebrew on macOS. The Windows package is `octave.portable` and not the `octave` meta package,
  because the meta package pulls `octave.install`, which drives the vendor GUI installer through
  AutoHotkey. The portable one unzips the official archive from `ftp.gnu.org` and cannot hang.
  Each job configures its own bindings tree with `BWA_BUILD_MATLAB=ON` against the engine SDK,
  builds the Octave MEX there and runs the four `octave_*` ctests. Each job asserts the configure
  log says both `SDK MODE` and `Octave MEX enabled`, and that the MEX file staged, because
  `octave_tests` exits 77 (SKIPPED) when no MEX is there for the running interpreter and a silent
  miss would otherwise read as a pass. Only the MATLAB half waits for the `matlab-actions` steps,
  because only MATLAB needs an action to install it and a licensed `run-command` step to drive it.
  **Unverified locally:** nothing here can run a GitHub-hosted MATLAB, so every MATLAB step was
  written from
  [mathworks/ci-configuration-examples](https://github.com/mathworks/ci-configuration-examples)
  rather than from a run, and the macOS Octave steps are unverified for the same reason the rest of
  that job is: nobody here has a Mac. The Windows and Linux Octave halves are verified locally.
  `bindings/matlab/README.md` is the manual.
- **The `wheels` job builds the two wheels a release ships.** A wheel `uv build` produces on a
  runner is tagged for that runner: the image's glibc on Linux, the runner's own architecture on
  macOS. Neither is what a release hands a stranger. So a separate job runs
  [cibuildwheel](https://cibuildwheel.pypa.io/) on `ubuntu-latest` and `macos-latest`, and the
  release takes the Windows wheel from the `windows` job's pack input and these two from here. Windows is not
  in this job: a Windows wheel already names one ABI and one architecture, so there is nothing a
  container could add.
  - **Neither wheel builds an engine.** The job downloads the engine SDK the `linux` and `macos`
    jobs installed and hands cibuildwheel the path, so it compiles the nanobind extension and
    nothing else, and builds no phonon at all. The cost is that the job now `needs: [linux,
    macos]` instead of running beside them; what it gives back is a phonon build and an engine
    build per platform.
  - **Linux is `manylinux_2_28`** (AlmaLinux 8, glibc 2.28: RHEL 8, Debian 10, Ubuntu 18.10 and
    later). The floor is set by the C++ toolchain, not by the engine: a static phonon is a C++
    link, and `manylinux_2_28` carries a `gcc-toolset` new enough to compile it where
    `manylinux2014` does not. It is also the image whose repositories still carry the two device
    backends' headers.
  - **Everything the Linux wheel links is built in that image**, phonon included, but the
    `linux` job is where it happens now. `tools/phonon/cibw-before-all-linux.sh` installs
    `alsa-lib-devel` and `jack-audio-connection-kit-devel`, then runs
    `tools/phonon/build-phonon.sh`, the same one recipe the composite action runs. It writes a
    stamp beside the staged archives naming the image and compiler that produced them, and
    rebuilds when the stamp does not match, so a developer's own `lib/linux-x64` cannot ride into
    a wheel on the project copy cibuildwheel puts in the container. The `wheels` job overrides
    cibuildwheel's `before-all` to nothing, because the engine arrives prebuilt. Run cibuildwheel
    by hand from a checkout and the `pyproject.toml` before-all still does the full job.
  - **auditwheel repairs with `--exclude libasound.so.2 --exclude libjack.so.0`.** The engine
    loads both at run time, so neither is in its `NEEDED` list and auditwheel would not see them
    anyway; the flags stay as GUARDS, and the job still asserts that no `libasound` or `libjack`
    landed in the wheel. Bundling either one was always wrong: `libjack` has to be the library the
    running JACK server uses, and `libasound` is on every Linux desktop already. Everything else
    the engine needs is in the manylinux policy, so a correct repair grafts nothing at all, which
    is also what keeps the extension's `$ORIGIN` runpath intact. The wheel imports on a machine
    with NEITHER library; each backend simply reports itself unavailable.
  - **macOS is one `universal2` wheel** for x86_64 and arm64, with
    `MACOSX_DEPLOYMENT_TARGET=10.13`. That is the only deployment target anything in this tree
    names (phonon's own `CMakeLists.txt`), and it is cibuildwheel's universal2 default, so the
    wheel's tag is predictable. Clang raises the arm64 slice to 11.0 by itself. The `macos` job
    now builds the engine with `CMAKE_OSX_DEPLOYMENT_TARGET=10.13` to match, and asserts it: a
    `macosx_10_13_universal2` wheel carrying a dylib built for the runner's own macOS installs
    anywhere and loads nowhere older.
  - **The suite runs against the installed wheel**, inside the container on Linux, the same rule
    the three per-runner jobs follow. Nothing in it opens a device, so a container is enough.
  - The `linux` job caches the container-built phonon under a key that names the image, and the
    `wheels` job pins the same image. They have to agree: the engine the wheel carries is that
    job's, and a wheel whose extension is compiled in one image against an engine from another is
    the mismatch a manylinux tag promises is absent. Bump the two pins together.
- **The Unity package is packed every run, and released on a tag.**
  `tools/upm/pack.ps1` produces `com.brainworks.bw_audio-<version>.tgz`, the C#
  binding with both DLLs inside it, so a broken package (a missing `.meta`, a lost
  plugin) fails the *build*, not a release. A `v*` tag cuts a GitHub Release, and
  **the Release is the distribution**: no registry, no token. Unity installs the
  tarball directly (Package Manager → `+` → *Install package from tarball…*). The
  tag stamps the version into the packaged manifest (the committed `package.json`
  stays `0.0.0-dev`), so a tarball can never claim a version it isn't. See
  [Releasing](#releasing).
- **Two more jobs guard the port, and they ship too.** `linux` on `ubuntu-latest` and `macos`
  on `macos-latest` build phonon, configure, build RelWithDebInfo, and run the whole engine ctest
  suite on the null and manual sinks: **38 tests**, the 33 a default off-Windows build registers
  plus the five SDK-gated ones. The binding tests are not in that count any more: they run in each
  job's own bindings tree against the SDK. On Linux the engine half of that happens inside the
  manylinux container. Both jobs exist to catch a Win32 call sneaking back into the core, so they
  assert on the configure log rather than trusting the build to fail on its own: `linux` wants
  `JACK backend ENABLED` and `ALSA backend ENABLED`, `macos` wants `null and manual sinks only`,
  and both want `Steam Audio ENABLED`. That last assertion is the one that matters after a cache
  round trip, because a build that quietly lost its phonon staging still passes every test.
  Neither job fetches ASIO, which is Windows-only. A cold run pays for the phonon build the way the
  Windows job does; a warm one restores the archives and costs a few minutes. The macOS job is
  UNVERIFIED locally in both halves, since no Mac was at hand: it is the first thing that will ever
  execute either the shim's Apple paths or a macOS phonon build.
- **Each of those two also builds a Godot GDExtension and ships an engine artifact**, the same way
  the android job does. Both flavors (`editor` and `template_release`) build in their own trees,
  cached on the pinned godot-cpp commit, and the copies bound for the bindings are stripped. The
  standalone artifact ships its library stripped too, with the symbols beside it as a separate
  file (`libbw_audio.so.debug` on Linux and Android, found through the gnu_debuglink;
  `libbw_audio.dylib.dSYM` on macOS), which is the split Windows already has with its `.pdb`: a
  consumer copies the 7 MB of code and a debugger finds the symbols when the two sit together.
  Uploads:
  `bw_audio-linux-x64-<ver>-r<N>` (`lib/libbw_audio.so`, `include/bw_audio.h`, `LICENSE`,
  `THIRD_PARTY-NOTICES.md`, `DIST.txt`) and `bw_audio-macos-universal-<ver>-r<N>` (the same tree
  with `lib/libbw_audio.dylib`), plus the fixed-name pack inputs `linux-pack-input` and
  `macos-pack-input` that the `package` job downloads. The macOS build is **universal**
  (`CMAKE_OSX_ARCHITECTURES="x86_64;arm64"`, and the composite action passes the same pair to
  phonon's own configure, because phonon's CMake does not make itself universal despite appearing
  to; see `third_party/README.md`) and both the action and the job fail on a `lipo -info` that
  does not show both architectures.
- **Building and packing are two jobs.** The `windows` job builds and tests the Windows engine,
  both MEX gateways, both Godot extension flavors and the Python wheel, and it waits for nothing.
  It ends by uploading `windows-pack-input`, the same fixed-name handoff the other three jobs make.
  The `package` job compiles nothing: it waits for all five build jobs
  (`needs: [windows, android, linux, macos, wheels]`), downloads the four pack inputs and the
  release wheels, packs the Godot addon, the Unity package and the MATLAB toolbox, and on a tag
  creates the release. Each package carries every platform's engine library, which is why one job
  has to see them all. Splitting it out is what lets the longest job in the matrix start at once
  instead of behind the cross-builds, and it keeps the per-platform engine artifacts available
  when packing fails, because the job that built each one uploads it.
- **A release carries TWELVE assets**, because workflow artifacts expire (30 days) and a
  release doesn't:
  - `com.brainworks.bw_audio-<ver>.tgz`: the Unity package.
  - `bw_audio-godot-<ver>.zip`: the installable Godot addon.
  - `bw_audio-win64-<tag>.zip`: the engine itself (`bw_audio.dll`/`.lib`/`.pdb`,
    `bw_audio.h`, and the tools). This is the durable download for a C/C++
    consumer or the CAVE machine.
  - `bw_audio-android-<tag>.zip`: the Android engine, `libbw_audio.so` for `arm64-v8a` and
    `x86_64` plus `bw_audio.h`. For a native consumer only: the Unity package and the Godot
    addon already carry the `arm64-v8a` library.
  - `bw_audio-linux-x64-<tag>.zip`: the Linux engine, `libbw_audio.so` plus `bw_audio.h`.
    JACK and ALSA, no ASIO.
  - `bw_audio-macos-universal-<tag>.zip`: the macOS engine, one universal `libbw_audio.dylib`
    (x86_64 and arm64) plus `bw_audio.h`. No device backend yet, so it is the offline path.
    It is **not code-signed and not notarized**: Gatekeeper blocks a downloaded unsigned library,
    so clear the quarantine flag (`xattr -dr com.apple.quarantine <folder>`) after unzipping.
    Distribution outside the lab would need a Developer ID signature and notarization.
  - `bw_audio-matlab-<tag>.zip`: the MATLAB and Octave toolbox. `+bwa`, the examples, the
    README and the licenses, plus `bin/win64`, `bin/glnxa64` and `bin/maca64`, each holding that
    platform's two MEX files and the one engine library both of them load. Unzip it and `addpath`
    the folder; there is nothing to build. Every MEX is built inside that platform's own desktop
    job, MATLAB's and Octave's alike, and `bwa.setup` picks the one for the interpreter you are
    in. Octave's extension is `.mex` on every platform, which costs nothing here: the layout is one
    directory per architecture, so what shares a directory is one `.mexw64` and one `.mex`. The
    macOS MEX files are arm64 and unsigned, so they take the same quarantine step as the macOS
    engine above.
  - `bw_audio-asio-sdk-src-<tag>.zip`: the ASIO SDK source statically linked into the DLL,
    kept as its own asset so it accompanies the binaries (GPLv3 corresponding source)
    without bloating either the engine `.zip` or the `.tgz`.
  - `bw_audio-steam-audio-src-<tag>.zip`: the Steam Audio source statically linked into the
    same DLL, at its pinned commit, with the in-repo patch and a README naming the commit.
    Same reason as the asset above, applied to the other statically linked dependency.
  - `bw_audio-<abi>-cp312-abi3-<platform>.whl`, three of them: Windows x64 from the `windows`
    job, `manylinux_2_28_x86_64` and macOS `universal2` from the `wheels` job. The Python
    binding, with the engine library inside the wheel. Attached as wheels rather than zipped,
    because `pip` and `uv` install a `.whl` straight from a URL. `<abi>` is the ABI version from
    `bw_audio.h`, not the tag; `abi3` means one wheel serves Python 3.12 and every later version.
    The release step asserts one wheel per platform and the tag each one carries.

  All ship under GPLv3 (the `.zip` carries `LICENSE`, `THIRD_PARTY-NOTICES.md`, and
  `DIST.txt`, which names the commit and links the complete source; the `.tgz` carries
  the same inside it, and both point at the two source assets for the third-party code the
  repo fetches rather than vendors): an app that ships this DLL to third
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
     public listing would invite installs into projects the GPLv3 would surprise). But
     the tarball IS what a registry would serve, so listing it later stays a config
     change rather than a rebuild. Preserving that costs nothing.

## Linux notes

Configuration the two Linux backends depend on. None of it is engine state: every item is a
property of the box.

### The backend libraries are optional at run time

`libbw_audio.so` links neither libjack nor alsa-lib. Each sink loads its own library on the first
device query or open:

| backend | soname loaded      | supplied by                                     |
|---------|--------------------|-------------------------------------------------|
| JACK    | `libjack.so.0`     | `libjack-jackd2-0`, or pipewire-jack's drop-in   |
| ALSA    | `libasound.so.2`   | `libasound2`, on every Linux desktop that plays audio |

So the development packages are needed to BUILD (the headers give the types and the callback
signatures) and the libraries are optional to RUN. A box with neither loads the library, reports a
device count of 0 for both backends, and falls through to the offline sink with the first
backend's reason on `bwa_last_error`. An explicit `BWA_SINK_JACK` or `BWA_SINK_ALSA` open fails
with the missing library named. Install a library later and the next open finds it; nothing has to
restart.

### Real-time scheduling for the ALSA sink

The ALSA sink owns its render thread and paces on blocking writes, so it asks for `SCHED_FIFO`. An
unprivileged process gets it only when `RLIMIT_RTPRIO` allows the priority, which on most distros
means membership of the `audio` group and a drop-in like this:

```
# /etc/security/limits.d/audio.conf
@audio   -  rtprio     95
@audio   -  memlock    unlimited
```

Log out and back in after adding yourself to the group. Without the budget the sink still opens and
still runs, at normal priority, and it says so through `bwa_last_error` after a successful
`bwa_start`. Treat that message as a warning that a busy desktop can starve the render thread, not
as a failure.

The JACK sink needs none of this. Its render callback runs on the server's process thread, which
the server already put on `SCHED_FIFO`.

### PipeWire graph rate and quantum

PipeWire runs one graph for the whole box, and a JACK client joins it at the graph's rate and
quantum. Two consequences.

The rate must match `bwa_desc.sample_rate`, because a JACK client has no resampler. A desktop
running its graph at 44.1 kHz fails the open with both rates named. Fix it in
`~/.config/pipewire/pipewire.conf.d/10-clock.conf`:

```
context.properties = {
    default.clock.rate     = 48000
    default.clock.quantum  = 256
}
```

The quantum is a preference the graph can move. Add `clock.force-quantum = 256` on a rig box to pin
it. Pin it to the engine block size and the fixed-quantum adapter runs in pass-through, which costs
no latency and no copy. Any other quantum works too, at one block of added latency.

**The sink never calls `jack_set_buffer_size`.** On pipewire-jack that call forces the global
quantum for every application on the box, which is the rig's decision to make in its own config,
not the engine's.

### Reaching the array

Audinate ships no Dante host driver for Linux, so the Digiface plays no part in a Linux rig. Two
routes reach 26 speakers, and the sink sees an ordinary card either way:

- **A multichannel card wired to the amps.** A MADI or ADAT card with an in-kernel ALSA driver, or
  a class-compliant USB interface. The Dante network is not involved.
- **AES67 into the existing Dante network.** `aes67-linux-daemon` on the RAVENNA ALSA kernel module
  presents a virtual ALSA card, and Dante devices in AES67 mode receive its multicast flows. The
  configuration is on the Dante side, in Dante Controller. Nothing in a backend knows about AES67.

[backends.md](./backends.md) has the full statement, including the hardware questions that are
still open.

## Android

Android is the standalone-headset path: a Meta Quest or Pico runs Android, and its audio is an
AAudio stream. Nothing here is VR-specific. The headset supplies the pose through the ordinary
listener API, and the array never comes near this build.

### Prerequisites

| piece | what and where |
|-------|----------------|
| JDK 17 | the command-line tools need one. `winget install EclipseAdoptium.Temurin.17.JDK` |
| Android command-line tools | unzip `commandlinetools-win-*.zip` from `dl.google.com/android/repository` into `%LOCALAPPDATA%\Android\Sdk\cmdline-tools\latest` |
| NDK r27 or later | `sdkmanager --install "ndk;27.3.13750724"`. Set `ANDROID_NDK_HOME` to it |
| platform-tools | `sdkmanager --install platform-tools`, for `adb` |
| ninja | the NDK ships no generator. `sdkmanager --install "cmake;3.31.6"` brings one, and any ninja on `PATH` does as well |
| the emulator (optional) | `sdkmanager --install emulator "system-images;android-34;google_apis;x86_64"`, then `avdmanager create avd -n bwa_x86 -k "system-images;android-34;google_apis;x86_64"` |

Set `ANDROID_HOME` to the SDK root. Accept the licenses once with `sdkmanager --licenses`.

### The two ABIs

Build `arm64-v8a` for a headset and `x86_64` for the emulator. Nothing in the engine is
architecture-specific: the DSP is scalar C with no intrinsics, so ARM needs no port.

```powershell
$ninja = "$env:ANDROID_HOME\cmake\3.31.6\bin\ninja.exe"   # or any ninja on PATH
cmake -S . -B build-android-arm64 -G Ninja `
  "-DCMAKE_MAKE_PROGRAM=$ninja" `
  "-DCMAKE_TOOLCHAIN_FILE=$env:ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" `
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_static `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-android-arm64
```

`ANDROID_STL` is pinned rather than left to default. A static phonon is C++, so the library needs a
libc++ from somewhere, and `c++_static` puts it inside `libbw_audio.so` so nothing ships beside the
engine. That is safe here because the ABI is C: no C++ type, exception or allocation crosses the
library boundary, which is the case where two static copies of libc++ in one process cause trouble.
The NDK's own CMake toolchain already defaults this way. Gradle's does not, so a project that
builds this engine through `externalNativeBuild` should say it there too. `readelf -d` on the
result names `libm`, `libaaudio`, `liblog`, `libdl` and `libc`, and nothing else.

`tools\android\run-tests.ps1` does all of that for you, the ninja included. The explicit form is
here for a CI script or a build system that wants the flags.

The build produces `libbw_audio.so` and the test executables. The GUI tools, the capture tools and
the ASIO backend are all Windows-only targets and are skipped with a status line.

**Steam Audio is linked in**, statically, once a phonon for that ABI is staged at
`third_party/steam-audio-artifacts/lib/android-arm64` or `.../android-x64` (CI builds and caches
both; `third_party/README.md` has the recipe and the two android-only patches it needs). The
configure says `Steam Audio ENABLED ... android-arm64`, the binaural path is the real HRTF decode,
and occlusion, pathing and the reflection bed all work. What an APK pays for it is size: the
stripped arm64 library is about 7 MB rather than 0.4 MB. Without a staged phonon the build is a
no-SDK one and still works; see
[Building without Steam Audio](#building-without-steam-audio).

### Running the tests on a device

`ctest` cannot drive an Android target, so `tools\android\run-tests.ps1` does it: it builds (or takes
a build directory), pushes `libbw_audio.so` and the test executables to `/data/local/tmp/bwa`, runs
each under `adb` with `LD_LIBRARY_PATH` set, and returns the worst exit code. The ctest skip code 77
maps through, so a device with no audio reports SKIPPED and never a pass.

```powershell
tools\android\run-tests.ps1                                     # build and run the whole suite
tools\android\run-tests.ps1 -BuildDir build-android-x86_64 -Tests audio_sink -Verbose
```

Start the emulator headless first:

```
emulator -avd bwa_x86 -no-window -no-boot-anim -gpu swiftshader_indirect
```

Check that hardware acceleration is available with `emulator -accel-check` before anything else.
On Windows that means the Windows Hypervisor Platform feature, which needs an administrator and a
reboot to turn on. Without it the emulator runs interpreted and the audio timing is meaningless.

### Packaging a binding for Android

Both bindings ship the `arm64-v8a` library, which is the ABI every current standalone headset
runs. Where it lands:

| package | path | how it gets there |
|---------|------|-------------------|
| Unity   | `Runtime/Plugins/Android/arm64-v8a/libbw_audio.so` | an `arm64-v8a` build of this repo stages it (CMake `POST_BUILD`), or `tools\upm\pack.ps1 -AndroidFrom <dir>` takes one |
| Godot   | `addons/bw_audio/bin/libbw_audio.so` plus `libbw_audio_gd.android.template_release.arm64.so` | `tools\godot\pack.ps1` builds the pair when an NDK is on the machine, or takes it with `-AndroidFrom <dir>` |

Both packs FAIL when the library is missing rather than packing without it. A package whose
Android plugin is absent installs, exports an APK, and throws on the headset, which is the latest
possible place to find out.

The Godot extension must be named `lib*.so`. An APK extracts only files matching that from its
`lib/<abi>/` directory, so a prefix-less extension is not on disk at all at load time. The
`.gdextension` manifest names `android.template_release.arm64` and points
`android.template_debug.arm64` at the same file, exactly as the Windows entries do: godot-cpp's
debug flavor differs only in its own internal checks, and a listed library that is not shipped is
a load failure.

`DllImport("bw_audio")` needs no change on Android. Mono maps that name to `libbw_audio.so`, the
same way it maps it to `bw_audio.dll` on Windows.

The packaged copies are stripped of their debug info. The standalone Android artifact ships the
same stripped library with its DWARF beside it as `libbw_audio.so.debug`, linked by gnu_debuglink,
so a crash from a headset can be symbolized against the artifact it shipped from. That is the
split Windows already has, where the shipped DLL leaves its symbols in a `.pdb` the packages do
not carry. Stripping is 11 MB down to 1.9 for the Godot extension and 20.7 down to 7.4 for the
engine library with phonon inside, on an APK that pays for every byte.

Both packages ship phonon for Android, inside `libbw_audio.so`: it links statically, so it stages
no file of its own and neither package gained an entry for it. What it costs is size, about 7 MB
on arm64 rather than 0.4 MB. See
[Building without Steam Audio](#building-without-steam-audio) for what a no-SDK build loses.

### Packaging a binding for Linux and macOS

Both bindings carry those two as well, by the same rule. Where each library lands:

| package | path | how it gets there |
|---------|------|-------------------|
| Unity   | `Runtime/Plugins/Linux/x86_64/libbw_audio.so` | a Linux build of this repo stages it (CMake `POST_BUILD`), or `tools\upm\pack.ps1 -LinuxFrom <dir>` takes one |
| Unity   | `Runtime/Plugins/macOS/libbw_audio.dylib` | a macOS build stages it, or `-MacFrom <dir>` |
| Godot   | `addons/bw_audio/bin/linux/libbw_audio.so` plus `bin/libbw_audio_gd.linux.{editor,template_release}.x86_64.so` | `tools\godot\pack.ps1 -LinuxFrom <dir>` |
| Godot   | `addons/bw_audio/bin/libbw_audio.dylib` plus `bin/libbw_audio_gd.macos.{editor,template_release}.universal.dylib` | `tools\godot\pack.ps1 -MacFrom <dir>` |

Neither is buildable on a Windows machine, so unlike Android there is no local toolchain path:
both arrive prebuilt or the pack fails with a message naming the switch. CI hands over the
`linux-pack-input` and `macos-pack-input` artifacts.

The Godot addon keeps the **Linux** engine library one directory down, in `bin/linux/`. Android's
has the same file name, `libbw_audio.so`, and one addon carries both. It is Linux that moves,
because Android's path is the one an APK export resolves and no machine here can test that. The
extension finds its engine library through an `$ORIGIN` run path: `$ORIGIN` for an exported game,
where the exporter copies every dependency flat beside the binary, and `$ORIGIN/linux` for the
editor. macOS uses `@loader_path` and keeps its `.dylib` at the top of `bin/`, where the name
collides with nothing.

The macOS libraries are **universal** (x86_64 and arm64) and **unsigned**. Gatekeeper blocks a
downloaded unsigned library, so a Mac user clears the quarantine flag on the unzipped addon or
package: `xattr -dr com.apple.quarantine <folder>`. Distribution outside the lab would need a
Developer ID signature and notarization.

`godot-cpp` names each library from its own suffix property, so the file names above are its
spelling, not ours. macOS is the one platform where the manifest key and the file name differ:
the key is `macos.editor` with no architecture, because Godot matches every dot-separated tag
against the running platform's feature tags and a universal binary has no single one to name,
while the file keeps godot-cpp's `.universal`.

### What the emulator can and cannot tell you

The emulator's audio HAL is a virtual device, so treat its timing as indicative only. Two
measurements from runs on an API 34 `google_apis` x86_64 image:

- The AAudio section of `test_audio_sink` passes: a 960-frame burst, a 1920-frame buffer, a
  constant 256-frame render quantum through the adapter, and an injected stall that
  `AAudioStream_getXRunCount` did report. The xrun path is therefore exercised, not assumed.
- `test_os` is FLAKY here, and only on its two tightest timing bounds. Ten back-to-back runs on an
  idle emulator passed six times: the `os_sleep_until_ns` median lateness straddles its 1.00 ms
  bound at p50 0.89 to 1.11 ms (p99 2.31 to 2.72 ms, max 2.43 to 3.11 ms), and one run also tripped
  "a past deadline returns at once", which allows 1 ms between two clock reads around a call that
  does not wait at all. Every other assertion in that test passed every time. The `SCHED_FIFO`
  request is refused, as Android refuses it for every app thread.

  Read that as the virtual machine's scheduler rather than the shim: the primitive is
  `clock_nanosleep(TIMER_ABSTIME)` either way, a Windows desk box measures 0.43 ms and WSL2
  0.13 ms, and a bound that a real device has never been measured against should not be widened to
  fit an emulator. A physical headset measurement is on the verify list in
  [backends.md](./backends.md).

The other 32 tests of the 33-test suite pass on the emulator, every run.

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
  leader. Pick deliberately and check it in Dante Controller: an unlocked domain
  shows up as a sample rate that wanders between runs (Stage 0 of the runbook measures this).
- **Latency:** start ASIO buffer ~512–1024 and Dante latency 4–10 ms, then tighten
  empirically against measured dropouts.
- **Isolation:** less of a concern than with a software endpoint. A software Dante driver
  packetizes on the host CPU, so it competes with engine frame-rate spikes and is worth
  moving to its own machine; a hardware endpoint does that work on the device. The
  control-only C ABI still tolerates splitting the machines if you want to.

## ASIO host bring-up (sequence for `asio_sink.cpp`)

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

This sequence lives in `src/sink/asio_sink.cpp`, behind the device-agnostic `src/sink/sink.h`
seam, so ASIO types never leak into the engine. It compiles only when the ASIO SDK is
vendored. Fetch the SDK per [`../third_party/README.md`](../third_party/README.md);
CMake auto-detects `third_party/asiosdk/` and prints `ASIO backend ENABLED`. Without
the SDK, the offline `null_sink.c` backend builds instead: the library always builds
and the audio loop is testable with no hardware. Pick a backend with `bwa_desc.sink`
(`null` | `asio`; default is ASIO with null fallback). The ASIO backend rejects any
driver that exposes fewer output channels than the layout needs (26 for the CAVE array).

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
