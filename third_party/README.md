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
- `BWAUDIO_SINK=asio` — ask for ASIO explicitly (still falls back if unavailable).

The ASIO backend requires a driver exposing **≥26 output channels** (Dante Virtual
Soundcard in production). On a machine without one, `bw_asio_sink_open` rejects the
driver and the engine uses the null sink.

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

## Later milestones

- **Steam Audio** (M5, binaural) → `third_party/steamaudio/`
