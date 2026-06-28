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

**Linking (when stage 2 is compiled):** build phonon from the submodule (its `core/` CMake; pulls
Embree / FFT — see Steam Audio's build docs), which produces `phonon.dll` + `phonon.lib`. Point the
engine's `STEAMAUDIO_DIR` at the built `include/` + `lib/windows-x64/`, or drop them at
`third_party/steamaudio/` (the path CMake's `BWAUDIO_WITH_STEAMAUDIO` block auto-detects, mirroring
the ASIO block). A prebuilt **release** zip is the lighter alternative if the ambisonics fix isn't
needed. Apache-2.0 permits redistribution, so the built binaries are kept out of git for size only.

> The submodule pins the dependency at a known commit; `git submodule update --init` fetches it.
> It is a *reference + build source*, not auto-built by our CMake — the heavy phonon build is a
> deliberate, separate step taken when stage 2 is turned on.
