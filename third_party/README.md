# third_party

Vendored dependencies. These are **not committed** to this repo (see `.gitignore`);
fetch them locally. CMake auto-detects each and enables the matching backend.

## ASIO SDK (required for the production `cave`/`both` device path)

The Steinberg ASIO SDK is dual-licensed **GPLv3 / proprietary** (see `docs/build.md`).
This project uses it under the GPLv3 option, consistent with the repo `LICENSE`.

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
`bwaudio: ASIO backend ENABLED` and compiles `src/asio_sink.cpp` plus the SDK host
sources (`common/asio.cpp`, `host/asiodrivers.cpp`, `host/pc/asiolist.cpp`) with
`BW_HAVE_ASIO`. Without it, only the offline null sink is built (`bwaudio: ASIO
backend disabled`) and the library still builds and links.

**Sink selection at runtime** (see `src/sink.c`):
- default — try ASIO, fall back to the null (offline) sink if no usable device;
- `BWAUDIO_SINK=null` — force the offline sink (CI, desk debugging, no hardware);
- `BWAUDIO_SINK=asio` — require ASIO; if no driver opens it **fails** (no silent
  null fallback), so a missing device surfaces instead of playing silence.

The `cave`/`both` array path needs a driver exposing **≥26 output channels** (Dante
Virtual Soundcard in production); `binaural` needs only 2. The auto-pick tries the
registered drivers and uses the first that opens with enough outputs (override with
`BWAUDIO_ASIO_DRIVER`); `bw_audio_backend()` reports which one opened.

## NatNet SDK (OptiTrack pose, M6) — reference only, NOT linked

NaturalPoint's NatNet SDK is **proprietary** and would conflict with GPLv3 under
distribution, so the engine parses the documented FrameOfData wire protocol itself in
`src/natnet.c` and **does not link the SDK**. A local copy at `third_party/NatNetSDK/`
(gitignored, not redistributed) is useful only as a wire-format reference — the sample
`Samples/PacketClient/PacketClient.cpp` `Unpack*` functions are the authoritative layout,
and `include/NatNetTypes.h` has the message IDs / default ports / multicast group. Nothing
in `third_party/NatNetSDK/` is required to build; M6 needs no vendored dependency.

## dr_wav (wav loading, M3)

**Not vendored here.** CMake fetches it via `FetchContent`, pinned to the **wav-0.14.5**
release commit (`fa931f3285ced10ace628f7f1ac951e1951e7ea6`) — see the `dr_wav` block in
`CMakeLists.txt`. dr_wav is public domain / MIT-0 and a single header; pinning to a commit
keeps builds reproducible even though upstream `master` moves often. To bump the version,
change the SHA. For an offline build, point CMake at a local copy:

```sh
cmake -S . -B build -DFETCHCONTENT_SOURCE_DIR_DR_WAV=/path/to/dir-containing-dr_wav.h ...
```

## cJSON (layout parsing, M4)

**Not vendored here.** CMake fetches `cJSON.c` + `cJSON.h` via `FetchContent`, pinned to the
**v1.7.19** release commit (`c859b25da02955fef659d658b8f324b5cde87be3`); cJSON is MIT and two
files, compiled straight into `bw_core`. `layout.c` uses it to parse `cave_layout.json`. Bump
by changing the SHA; offline builds can override `FETCHCONTENT_SOURCE_DIR_CJSON_SRC` /
`..._CJSON_HDR`.

## Steam Audio (binaural decode, M5 upgrade; occlusion/reflections later)

**Apache-2.0**, which is one-way compatible with GPLv3 — so, unlike the ASIO and NatNet SDKs,
Steam Audio is a *clean, redistributable* dependency: it can be linked and shipped under the repo
`LICENSE` with no special handling.

**Vendored as a source submodule** at `third_party/steam-audio`, pinned to
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

**Building phonon (minimal core, Windows x64).** This recipe produces a `phonon.dll`/`phonon.lib`
that links cleanly into `bwaudio.dll`. Run from `third_party/steam-audio/core/build`:

```sh
git submodule update --init third_party/steam-audio          # fetch the pinned source

# 0. Apply our local fixes (see third_party/patches/). REQUIRED for directional reflections.
git -C third_party/steam-audio apply ../patches/phonon-multiplyaccumulate-align.patch

# 1. Fetch ONLY the required deps, with the SHARED CRT (/MD) — see the CRT note below.
#    flatbuffers is a build tool (flatc); zlib/pffft/mysofa are linked into phonon.
python get_dependencies.py --dependency flatbuffers -p windows -a x64 -t vs2022
for d in zlib pffft mysofa; do
  python get_dependencies.py --dependency $d --sharedcrt -p windows -a x64 -t vs2022
done

# 2. Generate the phonon project (--minimal drops Embree / IPP / GPU / sample apps).
python build.py -p windows -a x64 -t vs2022 -c release --minimal -o generate

# 3. Build JUST the phonon target with the DYNAMIC CRT, and skip phonon_test (a CRT-fragile exe).
cmake -DSTEAMAUDIO_STATIC_RUNTIME=OFF build/windows-vs2022-x64
cmake --build build/windows-vs2022-x64 --config Release --target phonon
```

> **CRT gotcha (the build's one trap).** phonon defaults to `STEAMAUDIO_STATIC_RUNTIME=ON` (`/MT`),
> but its deps' CMake (mysofa especially) ignore the runtime flag and build `/MD`. Mixing them gives
> `unresolved external __imp_fgetc / __stdio_common_vsscanf`. Make **everything `/MD`**: `--sharedcrt`
> on the deps **and** `-DSTEAMAUDIO_STATIC_RUNTIME=OFF` on phonon. `/MD` also matches our `bwaudio.dll`.

**Stage it** where `BWAUDIO_WITH_STEAMAUDIO` auto-detects it (mirroring the ASIO block):

```
third_party/steamaudio/
  include/   phonon.h, phonon_version.h        # from core/src/core/ + the generated build dir
  lib/windows-x64/   phonon.lib, phonon.dll    # from build/windows-vs2022-x64/src/core/Release/
```

A prebuilt **release** zip is the lighter alternative if the ambisonics fix isn't needed. Apache-2.0
permits redistribution, so the built binaries are kept out of git (gitignored) for size only.
