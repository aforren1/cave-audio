# Build, dependencies & licensing

## Platform

**Windows only.** ASIO is Windows-only; DVS is Windows/macOS. A future cross-platform
move means abstracting the device layer — ASIO is just the Windows sink — so keep
ASIO assumptions confined to `asio_sink.c`. On other platforms the device backend
would be ALSA/JACK (Linux) or CoreAudio (macOS), and you would *not* use ASIO there.

Build system: CMake + MSVC (Visual Studio 2022 toolset). The core is C (C11 with
`stdatomic.h`); Steam Audio and ASIO glue are C/C++. MSVC gates C11 atomics behind
`/experimental:c11atomics`; CMake scopes that flag to `src/rt.c` (the only file that uses
them).

**Race-checking the rings.** The `bw_rt_test` target drives the SPSC ring/commit logic
off the real-time path (single-threaded, deterministic) and is what runs under `ctest` on
MSVC. The full ThreadSanitizer/Helgrind pass the roadmap calls for needs a **Clang or
Linux** build (MSVC ships no TSan, and Helgrind is Valgrind/Linux): build `rt.c` + a
two-thread driver with `clang -fsanitize=thread` (or run under Helgrind) and exercise one
producer pushing commands while one consumer drains. Keep that driver off the RT path —
never let the sanitizer harness add allocation/locks to the callback.

## Dependencies

| dep            | role                                        | license / notes                          |
|----------------|---------------------------------------------|------------------------------------------|
| Steinberg ASIO SDK | device output to DVS; timing hooks       | dual GPLv3 / proprietary (see below)     |
| Steam Audio (C API)| binaural decode; later occlusion/reflect | Apache-2.0 (verify current)              |
| dr_wav         | wav loading (single header)                 | public domain / MIT-0                    |
| NatNet         | OptiTrack pose ingest                        | consume off-wire; see below              |

Vendor third-party under `third_party/` (`asiosdk/`, `steamaudio/`, `dr_wav.h`).

### ASIO SDK licensing (read before distributing)

As of October 2025 the ASIO SDK is **dual-licensed GPLv3 / proprietary** (previously
proprietary-only). Using it directly under the GPLv3 option is viable without signing
an agreement. The catch is copyleft:

- **Internal lab tool, not distributed** → GPL obligations don't trigger (copyleft is
  a distribution condition). Use freely. This is the expected case.
- **Open-sourcing the tool** → it must be GPLv3-compatible.
- **Shipping closed** → take the proprietary ASIO license (the other half of the dual).

This is the shape of the issue, not legal advice — confirm against the current SDK
license text. The same copyleft reasoning is why a permissive library (e.g. miniaudio)
still won't bundle ASIO upstream: a permissive core can't absorb GPLv3. A "miniaudio
ASIO backend" would be one *you* write under the GPLv3 (or proprietary) option.

### NatNet without the proprietary SDK

NaturalPoint's NatNet SDK is proprietary, which would conflict with GPLv3 under
distribution. The NatNet protocol is documented; consume the multicast/unicast stream
directly in `natnet.c` rather than linking the SDK. This also lets the core read pose
itself (`track_internal = true`) and sample the freshest head pose at audio-callback
time, which is lower-latency than marshaling pose through the engine each frame.

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
  engine to keep the audio thread and Dante stack away from engine frame-rate spikes —
  the usual source of dropouts. The control-only C ABI does not require this, but the
  design tolerates it.

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
seam (so ASIO types never leak into the engine). It compiles only when the ASIO SDK is
vendored — fetch it per [`../third_party/README.md`](../third_party/README.md); CMake
auto-detects `third_party/asiosdk/` and prints `ASIO backend ENABLED`. Without it, the
offline `null_sink.c` backend builds instead, so the library always builds and the audio
loop is testable with no hardware. Pick a backend at runtime with `BWAUDIO_SINK`
(`null` | `asio`; default is ASIO with null fallback). The ASIO backend rejects any
driver exposing fewer than 26 output channels.

## Verify before shipping

Several claims in these specs depend on third-party terms/APIs that move faster than this doc.
Re-confirm each against the actual vendored version before distributing or relying on it:

- [ ] **ASIO SDK license.** The dual GPLv3/proprietary terms described above were current as of
  **October 2025**. Read the `LICENSE` in the SDK build you actually vendor and pick the matching
  option for how you ship (internal/undistributed, open-source GPLv3, or proprietary).
- [ ] **Steam Audio C API.** Confirm `IPLSpeakerLayout` + `IPL_SPEAKERLAYOUTTYPE_CUSTOM`
  (unit-direction custom layouts) and the ambisonics→binaural effect exist in the linked version,
  and that the 3rd-order encode/decode path (see `spatialization.md`) is supported.
- [ ] **Other dependency licenses** (Steam Audio Apache-2.0, dr_wav PD/MIT-0, NatNet protocol)
  unchanged in the versions you ship; reflected in `LICENSE`/`THIRD_PARTY-NOTICES`.
- [ ] **Dante clock.** A hardware leader clock is present on the network — a pure-software DVS
  instance cannot be the standalone leader.
- [ ] **This repo's own license** chosen and committed (see `LICENSE`).
