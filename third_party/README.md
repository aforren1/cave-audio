# third_party

Vendored dependencies. These are **not committed** to this repo (see `.gitignore`);
fetch them locally. CMake auto-detects each and enables the matching backend.

## ASIO SDK (required for the production `cave`/`both` device path)

The Steinberg ASIO SDK is dual-licensed **GPLv3 / proprietary** (see `docs/build.md`).
This project uses it under the GPLv3 option, consistent with the repo `LICENSE`.

Because it is fetched here rather than committed, the SDK source is not in the repo tree —
but it is statically linked into `bw_audio.dll`, so it is part of that DLL's GPLv3
corresponding source. CI redistributes it under the GPLv3 option as `asio-sdk-src.zip`
beside every binary artifact (and as a release asset), which is what keeps the fetch-only
setup GPLv3-complete for downstream binary recipients.

**Fetch:**

```sh
# from the repo root
curl -fsSL -o asiosdk.zip https://www.steinberg.net/asiosdk
unzip asiosdk.zip                 # extracts an ASIOSDK/ folder
mv ASIOSDK third_party/asiosdk    # CMake looks for third_party/asiosdk/common/asio.h
```

Expected layout after vendoring:

```
third_party/asiosdk/
  common/   asio.h, asio.cpp, asiosys.h, iasiodrv.h, ...
  host/     asiodrivers.{h,cpp}, ginclude.h
  host/pc/  asiolist.{h,cpp}
```

**Build wiring:** with the SDK present, `cmake` prints
`bw_audio: ASIO backend ENABLED` and compiles `src/asio_sink.cpp` plus the SDK host
sources (`common/asio.cpp`, `host/asiodrivers.cpp`, `host/pc/asiolist.cpp`) with
`BWA_HAVE_ASIO`. Without it, only the offline null sink is built (`bw_audio: ASIO
backend disabled`) and the library still builds and links.

**Sink selection** (`bwa_desc.sink`; see `src/sink.c`, and `docs/backends.md` for the full rules):
- `BWA_SINK_AUTO` (default) — pick by channel count. A 2-channel request (the headphone
  profiles, the `cave_both` monitor) tries WASAPI then ASIO; a wider one (the array) tries ASIO
  only. Either falls back to the null (offline) sink when no device opens;
- `BWA_SINK_NULL` — force the offline sink (CI, desk debugging, no hardware);
- `BWA_SINK_ASIO` or `BWA_SINK_WASAPI` — require that backend; if it does not open, the start
  **fails** (no silent null fallback), so a missing device surfaces instead of playing silence.

The `cave`/`both` array path needs a driver exposing **≥26 output channels** (the RME Digiface
Dante in production); `binaural` needs only 2, and gets them from an ordinary Windows endpoint
with no ASIO driver installed. The ASIO auto-pick tries the registered drivers and uses the first
that opens with enough outputs; pin one with `bwa_desc.device` (the old spelling `asio_driver`
still works), and `bwa_get_audio_backend()` reports which one opened.

WASAPI needs nothing vendored: its headers ship with the Windows SDK, so `src/wasapi_sink.cpp`
builds by default on Windows (`BWA_WITH_WASAPI`, linking `ole32` + `avrt`) and adds no license
obligation.

## NatNet SDK (OptiTrack pose, M6) — reference only, NOT linked

NaturalPoint's NatNet SDK is **proprietary** and would conflict with GPLv3 under
distribution, so the engine parses the documented FrameOfData wire protocol itself in
`src/natnet.c` and **does not link the SDK**. A local copy (gitignored, not redistributed)
is useful only as a wire-format reference — the sample
`Samples/PacketClient/PacketClient.cpp` `Unpack*` functions are the authoritative layout,
and `include/NatNetTypes.h` has the message IDs / default ports / multicast group.
`third_party/NatNet-4.5/NatNetSDK/` is the current reference (it certifies the 4.1–4.5
frame-suffix hop `natnet.c` uses for the server-clock pose stamps — `stamps_supported` is
pinned to what this copy documents, so bump both together — and the 4.5 IMU/GPIO sections);
an older copy may sit at `third_party/NatNetSDK/`. Nothing in either is required to build;
M6 needs no vendored dependency.

## dr_libs — dr_wav / dr_flac / dr_mp3 (sound loading + streaming, M3)

**Not vendored here.** CMake fetches the whole header-only repo as one pinned tarball
(`c629ca6f5ad6e013980b7db31043d5b4d1b63787` — **dr_wav 0.14.6 / dr_flac 0.13.4 / dr_mp3 0.7.4**),
so the commit lives in exactly one place; see the `dr_libs` block in `CMakeLists.txt`. dr_libs is
public domain / MIT-0 and header-only; pinning to a commit keeps builds reproducible even though
upstream `master` moves often. To bump, change the SHA.

The pin is a **master snapshot, not a release commit** — upstream tags a version only when it cuts
one, and the fixes we want (malformed `fmt`/`fact`/`bext`/`smpl` chunk handling, ADPCM + W64
underflows, FLAC picture-metadata and MP3 Xing/Info overflows) all landed after the 0.14.5 tag with
the version lines still reading "TBD". Prefer a tagged commit whenever one is available.

For an offline build, point CMake at a local copy (the variable is named after the
`FetchContent_Declare` name, `dr_libs` — not the header):

```sh
cmake -S . -B build -DFETCHCONTENT_SOURCE_DIR_DR_LIBS=/path/to/dir-containing-dr_wav.h ...
```

## cJSON (layout parsing, M4)

**Not vendored here.** CMake fetches `cJSON.c` + `cJSON.h` via `FetchContent`, pinned to the
**v1.7.19** release commit (`c859b25da02955fef659d658b8f324b5cde87be3`); cJSON is MIT and two
files, compiled straight into `bwa_core`. `layout.c` uses it to parse `cave_layout.json`. Bump
by changing the SHA; offline builds can override `FETCHCONTENT_SOURCE_DIR_CJSON_SRC` /
`..._CJSON_HDR`.

## Steam Audio (binaural decode + occlusion/reflections/pathing)

**Apache-2.0**, which is one-way compatible with GPLv3 — so, unlike the ASIO and NatNet SDKs,
Steam Audio is a *clean, redistributable* dependency: it can be linked and shipped under the repo
`LICENSE` with no special handling.

**Vendored as a source submodule** at `third_party/steam-audio-source`, pinned to
[`ValveSoftware/steam-audio`](https://github.com/ValveSoftware/steam-audio) **v4.8.1+10
(`480dd64`)** — chosen over a prebuilt release because those extra commits include an ambisonics
conversion fix the v4.8.1 binaries lack. Two reasons the source is the right vendor here:

1. **Convention reference.** `steam_decode.c` hand-encodes 3rd-order ambisonics to feed phonon's
   `iplAmbisonicsDecodeEffect`, which exposes no normalization parameter — so the encode must match
   phonon's *internal* SH convention exactly. The source (`core/src/core/sh.*`,
   `ambisonics_encode_effect.cpp`) is the authoritative answer (used like `PacketClient.cpp` was for
   NatNet). See [../docs/spatialization.md](../docs/spatialization.md) / [../docs/materials.md](../docs/materials.md).
2. **Gets the fix.** Building from the pinned commit yields a `phonon` lib that includes it.

There is also one **local patch** we apply before building (`third_party/patches/`): phonon 4.8.1's
complex `ArrayMath::multiplyAccumulate` reads its accumulator with an *aligned* SSE load on the path
it takes when the accumulator is *misaligned* — which it always is for the odd ambisonic channels
(the per-channel FFT stride is `8 mod 16` bytes), so any multichannel reflection effect access-violates
at channel 1. The one-line fix (`load`→`loadu`) is what lets the reflection bed render **directional**
early reflections instead of omni-only. This is the open upstream issue
[ValveSoftware/steam-audio#546](https://github.com/ValveSoftware/steam-audio/issues/546) (symptom only —
no root cause/fix there yet); drop the patch once a fixed release lands.

**Building phonon (minimal core, static, Windows x64).** This recipe produces the STATIC archives
that link into `bw_audio.dll`. Static is the only supported mode: nothing ships beside the engine
library on any platform, which is what deletes `phonon.dll` from the Godot manifest, the Unity
package and the release zips. Run from `third_party/steam-audio-source/core/build`:

```sh
git submodule update --init third_party/steam-audio-source   # fetch the pinned source

# 0. Apply our local fixes (see third_party/patches/). REQUIRED for directional reflections.
git -C third_party/steam-audio-source apply ../patches/phonon-multiplyaccumulate-align.patch

# 0.5. CMake >= 4 only: the pinned deps (flatbuffers 1.12, zlib, ...) declare pre-3.5
#      cmake_minimum_required, which CMake 4 refuses. This env var is the escape hatch.
export CMAKE_POLICY_VERSION_MINIMUM=3.5

# 1. Fetch ONLY the required deps, with the SHARED CRT (/MD) — see the CRT note below.
#    flatbuffers is a build tool (flatc); zlib/pffft/mysofa are linked into phonon.
python get_dependencies.py --dependency flatbuffers -p windows -a x64 -t vs2022
for d in zlib pffft mysofa; do
  python get_dependencies.py --dependency $d --sharedcrt -p windows -a x64 -t vs2022
done

# 2. Generate the phonon project (--minimal drops Embree / IPP / GPU / sample apps).
#    build.py creates its build tree in the CURRENT directory, so this lands in
#    core/build/windows-vs2022-x64 (the dir name comes from -t/-a, not the generator).
python build.py -p windows -a x64 -t vs2022 -c release --minimal -o generate

# 3. Build JUST the phonon target STATIC and with the DYNAMIC CRT, and skip phonon_test (a
#    CRT-fragile exe). Paths are relative to core/build — where you already are.
#    BUILD_SHARED_LIBS=OFF is the whole switch: core/src/core/CMakeLists.txt already branches on
#    it, so no upstream change is needed, and phonon.h serves both modes unchanged (IPLAPI is
#    __declspec(dllexport) only while STEAMAUDIO_BUILDING_CORE is defined, so a consumer never
#    sees a dllimport to fight).
cmake -DSTEAMAUDIO_STATIC_RUNTIME=OFF -DBUILD_SHARED_LIBS=OFF windows-vs2022-x64
cmake --build windows-vs2022-x64 --config Release --target phonon
```

> **CRT gotcha (the build's one trap).** phonon defaults to `STEAMAUDIO_STATIC_RUNTIME=ON` (`/MT`),
> but its deps' CMake (mysofa especially) ignore the runtime flag and build `/MD`. Mixing them gives
> `unresolved external __imp_fgetc / __stdio_common_vsscanf`. Make **everything `/MD`**: `--sharedcrt`
> on the deps **and** `-DSTEAMAUDIO_STATIC_RUNTIME=OFF` on phonon. `/MD` also matches our `bw_audio.dll`.
>
> Static linking makes that choice travel. An archive carries its CRT in every object, so a `/MDd`
> Debug consumer gets one `LNK2038` per member rather than a warning. The root `CMakeLists.txt`
> therefore pins the **release** dynamic CRT for every config of a with-SDK build. Your own
> application's CRT is its own business: it reaches this engine across a C ABI, never by sharing
> CRT objects.

**Stage it** where CMake auto-detects it. The layout is platform-neutral: one shared `include/`,
and one `lib/<platform>/` per platform, so a checkout can carry several at once and a cross-build
picks its own. `<platform>` is the name the root `CMakeLists.txt` looks for (`BWA_PHONON_PLATFORMS`):

```
third_party/steam-audio-artifacts/
  include/               phonon.h, phonon_version.h   # core/src/core/ + the generated build dir
  lib/windows-x64/       phonon.lib  mysofa.lib  zlibstatic.lib  pffft.lib
  lib/linux-x64/         libphonon.a libmysofa.a libz.a         libpffft.a
  lib/osx-universal/     libphonon.a libmysofa.a libz.a         libpffft.a
  lib/android-arm64/     libphonon.a libmysofa.a libz.a         libpffft.a
  lib/android-x64/       libphonon.a libmysofa.a libz.a         libpffft.a
```

Four archives, not one: phonon's own archive carries only its `core` and `hrtf` objects, so its
three third-party dependencies link beside it. The set is the same on every platform, Android
included. `osx-arm64` and `osx-x64` are accepted too, and
`osx-universal` wins when more than one is staged. **All four must be present**: a `lib/<platform>/`
that exists but is incomplete fails the configure rather than falling back to a no-SDK build. The
commonest way to reach that is an artifacts directory left over from the old shared staging (a
`phonon.lib` beside a `phonon.dll`); rebuild with `BUILD_SHARED_LIBS=OFF` and restage.

The Windows archives come from `build/windows-vs2022-x64/src/core/Release/` (phonon) and
`core/deps/<dep>/lib/windows-x64/release/` (the three companions).

Apache-2.0 permits redistribution, so the built binaries are kept out of git (gitignored) for size
only.

CI runs this same recipe through the composite action `.github/actions/build-phonon/` (inputs:
`platform`, `arch`, `toolchain`, `stage-dir`, `fft`, `cmake-flags`) and caches the staged artifacts
on the submodule sha + patch hash. If you change the recipe here, change it there too.

**Other platforms.** The scripts support them and nothing downloads from Valve: every linked
dependency is cloned from GitHub at a pinned sha and built locally. Linux and macOS are built in
CI the same way Windows is, by the same composite action. Traps found so far.

- **`-t` is Windows only.** Both pinned scripts declare `--toolchain` with the five `vs20xx`
  spellings and nothing else, so `-t make` or `-t ndk` is an argparse *error*, not a no-op. Off
  Windows the flag buys nothing anyway: `build.py` picks Unix Makefiles for Linux and Android and
  Xcode for macOS from `-p` alone. The action passes `-t` only on the Windows branch.
- **Linux.** `build.py` names its tree `linux-x64-release`, not `linux-x64`, because a single-config
  generator appends the config, so locate the tree by its `CMakeCache.txt` rather than by name. And
  the default `STEAMAUDIO_ENABLE_AVX=ON` adds `-fabi-version=6`, which breaks `<future>` while
  compiling `hrtf.cpp`: `no matching function for call to std::__uniq_ptr_data<...>`. The bound is
  the **compiler**, not the distribution. Compiling the `hrtf` target on one machine gave gcc 11.5
  FAIL, gcc 13.3 FAIL, gcc 14.3 PASS, so libstdc++ 14 is the first that survives the flag. Runner
  images ship gcc 13, so CI passes `-DSTEAMAUDIO_ENABLE_AVX=OFF` (the action's `cmake-flags`
  input), at the cost of phonon's AVX paths. Drop the flag once the runner's default compiler is
  gcc 14 or newer.

  The Linux archives come from `core/build/linux-x64-release/src/core/` (phonon) and
  `core/deps/<dep>/lib/linux-x64/release/` (the three companions). Build one yourself with:

  ```sh
  export CMAKE_POLICY_VERSION_MINIMUM=3.5
  cd third_party/steam-audio-source/core/build
  python get_dependencies.py --dependency flatbuffers -p linux -a x64
  for d in zlib pffft mysofa; do python get_dependencies.py --dependency $d -p linux -a x64; done
  python build.py -p linux -a x64 -c release --minimal -o generate
  cmake -DSTEAMAUDIO_STATIC_RUNTIME=OFF -DBUILD_SHARED_LIBS=OFF \
        -DSTEAMAUDIO_ENABLE_AVX=OFF linux-x64-release
  cmake --build linux-x64-release --target phonon
  ```

  There is no CRT question here: `--sharedcrt` and `STEAMAUDIO_STATIC_RUNTIME` are MSVC ideas, and
  the flag does nothing off Windows. What does travel is the **C++ runtime**: a static phonon makes
  `libbw_audio.so` a C++ link (`LINKER_LANGUAGE CXX`), so it gains a `libstdc++.so.6` dependency it
  did not have. Build phonon with the same compiler family as the engine.
- **macOS.** `build.py -p osx` generates ONE tree named `osx` with the Xcode generator (no config
  suffix, because Xcode is multi-config). `core/CMakeLists.txt` looks as if it makes every macOS
  build universal, but its `set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64")` comes after `project()`,
  which CMake ignores for languages already enabled, so the core archive comes out host-arch only
  (arm64 on a current Mac) while `get_dependencies.py`, which passes the pair on the command line,
  builds the three companions universal. CI hit exactly that: an arm64-only `libphonon.a` beside
  universal companions, and the engine's x86_64 link failed. The composite action therefore passes
  `-DCMAKE_OSX_ARCHITECTURES=x86_64;arm64` on the phonon configure itself and checks every staged
  archive with `lipo -info` for both slices. Do the same when building by hand. That is why the
  stage dir is `osx-universal` rather than `osx-arm64`. The AVX flag is a
  Linux-only branch in `core/CMakeLists.txt`, so Apple clang never needs it. **Untested outside
  CI:** nobody here has a Mac, and the CI job is the first thing to run it.
- **Android.** One phonon per ABI, both built in CI. The scripts need `ANDROID_NDK` set and pick
  the toolchain file from `-a` (`toolchain_android_armv8.cmake` for `arm64`,
  `toolchain_android_x64.cmake` for `x64`). Those files use CMake's own Android support rather than
  the NDK's `android.toolchain.cmake`, so `CMAKE_ANDROID_NDK_TOOLCHAIN_HOST_TAG` is the variable
  that names the host, and `ANDROID_STL` plays no part. The build targets android-21, which every
  consumer of ours (android-26) links against without trouble.

  Two more patches apply here, both android-only, and the composite action applies every
  `patches/phonon-android-*.patch` when the platform is `android`. Each patch file states its own
  reasoning:

  - `phonon-android-deps-platform-alias.patch`, needed on **every** host. The scripts build the
    64-bit ARM target as `android-armv8` but `dependencies.json` keys its flag layers on
    `android-arm64`, and only the dependency *selection* knew the two names mean one target. Every
    flag layer for arm64 was dropped, so pffft built with no `-march` at all and FFTS, if you turn
    it on, does not compile.
  - `phonon-android-host-sysroot.patch`, needed on a **Linux or macOS** host. The scripts spell the
    NDK host tag `windows-x86_64` in six places: the libc++ include directory in
    `core/CMakeLists.txt` and five `make` paths across the two scripts. The patch derives the tag
    instead. Verified on a Windows host, where the derived tag is the literal it replaces; the
    Linux-host path it exists for runs in CI.

  **pffft, not FFTS.** Upstream can link FFTS on Android, and `get_dependencies.py` will build it,
  but `STEAMAUDIO_ENABLE_FFTS` defaults OFF and phonon falls through to pffft on Android the same
  as everywhere else. Leave it there: `dependencies.json` builds FFTS for arm64 with NEON **off**
  and its JIT disabled, while pffft gets `-march=armv8-a` and NEON on. Turning FFTS on also costs a
  third patch, because its dead cache-flush path calls `__clear_cache(long, long)` against an NDK
  header that declares `__clear_cache(void*, void*)`, which clang 18 (NDK r27) rejects.

  Android archives come from `core/build/android-<arch>-release/src/core/` (phonon) and
  `core/deps/<dep>/lib/android-<armv8|x64>/release/` (the three companions). Build one yourself,
  from a Windows host here, with:

  ```sh
  export ANDROID_NDK="$LOCALAPPDATA/Android/Sdk/ndk/27.3.13750724"
  export CMAKE_POLICY_VERSION_MINIMUM=3.5
  git -C third_party/steam-audio-source apply ../patches/phonon-android-deps-platform-alias.patch
  git -C third_party/steam-audio-source apply ../patches/phonon-android-host-sysroot.patch
  cd third_party/steam-audio-source/core/build
  # flatbuffers is a TOOL dependency: the script reads "tool": true and builds it for the HOST
  # whatever -p says, so it needs no separate host invocation. It still wants a valid -t, and
  # a target of android still requires the NDK path, which is why both travel here.
  python get_dependencies.py --dependency flatbuffers -p android -a arm64 -t vs2022
  for d in zlib pffft mysofa; do
    python get_dependencies.py --dependency $d -p android -a arm64 -t vs2022
  done
  python build.py -p android -a arm64 -t vs2022 -c release --minimal -o generate
  cmake -DSTEAMAUDIO_STATIC_RUNTIME=OFF -DBUILD_SHARED_LIBS=OFF android-arm64-release
  cmake --build android-arm64-release --target phonon
  ```

  Repeat with `-a x64` for the emulator ABI, staging into `lib/android-x64`. Measured on one
  16-core desktop: about 2 minutes 20 seconds for phonon per ABI, plus about 2 minutes for the
  one-time flatbuffers host build and 45 seconds for the three companions. `libphonon.a` is about
  30 MB per ABI, which is debug information more than code: it takes `libbw_audio.so` from 0.4 MB
  stripped to 7.0 MB.

  Two things the engine link needs on Android, both wired in the root `CMakeLists.txt`. phonon's
  logger calls `__android_log_print`, so the system `log` library goes on the end of the archive
  list. And phonon's Android build sets no `-fvisibility=hidden` of its own, so
  `-Wl,--exclude-libs,ALL` keeps its 3000-odd symbols out of the library's dynamic symbol table and
  leaves the 171 `bwa_*` exports alone.
