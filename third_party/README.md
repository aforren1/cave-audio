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

**Use the prebuilt SDK, not the source repo.** [`ValveSoftware/steam-audio`](https://github.com/ValveSoftware/steam-audio)
is the full C++ SDK *source* — building it pulls in Embree / Radeon Rays (ray tracing), an FFT
library, and a large CMake tree. We don't need any of that: the engine links the small prebuilt
**`phonon` C API** (the `IPLContext` / `IPLMaterial` / ambisonics→binaural path — see
[../docs/spatialization.md](../docs/spatialization.md) and [../docs/materials.md](../docs/materials.md)).
So a git *submodule of the source* is the wrong tool; fetch the released SDK instead.

**Fetch** (from the [Releases](https://github.com/ValveSoftware/steam-audio/releases), pin a version):

```sh
# from the repo root
curl -fsSL -o steamaudio.zip https://github.com/ValveSoftware/steam-audio/releases/download/vX.Y.Z/steamaudio_api.zip
unzip steamaudio.zip
mv steamaudio third_party/steamaudio     # CMake looks for third_party/steamaudio/include/phonon.h
```

Expected layout after vendoring:

```
third_party/steamaudio/
  include/   phonon.h, phonon_version.h, ...
  lib/windows-x64/   phonon.dll, phonon.lib
```

**Build wiring:** deferred — it lands with the code that uses it (the production HRTF decode behind
`monitor_process`, then occlusion/reflections). At that point CMake auto-detects
`third_party/steamaudio/include/phonon.h` and links `phonon` under a `BWAUDIO_WITH_STEAMAUDIO`
flag, exactly like the ASIO block. Because Apache-2.0 permits redistribution, a pinned
**FetchContent** of the release zip is also an option (cleaner than manual vendoring); the binaries
are still kept out of git (below) for size, not licensing.

> If you specifically want to build Steam Audio from source (to patch it, or for an unsupported
> platform), *then* a submodule of the source repo makes sense — but it's a much larger build and
> isn't needed for normal use.
