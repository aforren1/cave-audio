# Architecture

## Goal

Render spatialized audio for a CAVE: a tracked observer moving within a ~3×3 m area
inside a **26-speaker array**, fed from a game engine (Unity and/or Unreal). Output
goes to a **Dante Virtual Soundcard (DVS)** over **ASIO**. Latency and timing
precision are first-class requirements.

## Top-level decision: self-hosted core, engines as control clients

The spatializer, mixer, device output, and tracking ingest all live in one native
process behind a single audio callback. Unity/Unreal call a C ABI to trigger sounds
and push positions. **No audio buffers cross the boundary**, only control.

Why:

- **Timing.** Direct ASIO gives you the hardware-anchored clock the design needs:
  `bufferSwitchTimeInfo` delivers an `ASIOTime` with sample position and a
  nanosecond `systemTime`, plus `ASIOGetSamplePosition` on demand. Middleware hides
  these.
- **Engine-agnostic.** One core, two thin clients. The only per-engine code reads
  transforms and tracking.
- **No middleware jitter.** Control is in-process over a lock-free ring, not OSC/UDP
  to a separate process with its own clock.

The cost: you own voice management, wav playback, and the ASIO host glue.

## The central seam: an in-memory master bus

The panner does not write "to the device". It writes to an in-memory **master bus**,
one channel per speaker. *Consumers* read that bus:

- **ASIO device sink (production):** writes the bus straight to DVS.
- **Binaural monitor (debug):** treats each bus channel as a virtual speaker at that
  speaker's surveyed room position, HRTFs them to stereo, and writes to an ordinary
  output device (a headphone DAC). It sits *after* the panner, so it auditions the
  real array render — DBAP behavior and all — not an idealized version.

The binaural monitor is just a second consumer of the bus. Keep it that way. The
abstraction set is: a **render target** (the bus), one or more **device sinks**
consuming buses, and the binaural monitor as a **bus→bus transform** (N→2) feeding a
stereo sink.

```
            sources
              │  per-voice, listener-relative DBAP (recomputed on move)
              ▼
      ┌───────────────────┐
      │  N-ch master bus  │  (in memory, owned by audio thread)
      └───────────────────┘
        │                  │
   per-ch gain/delay   bus→ambisonics→HRTF
   align               (single binaural decode)
        │                  │
     ASIO ► DVS         stereo device
     (cave)             (binaural debug)
```

### How wide is the bus?

**The layout's speaker count.** `BW_CHANNELS` (26, `src/sink.h`) is the compile-time
*capacity*; the **active** count is whatever the loaded `cave_layout.json` declares —
any N in **4..26**, resolved at `bw_create` and fixed for the engine's lifetime. With
no `layout_path` you get the built-in 26-speaker default grid. `bw_channel_count()`
is the readback, and everything downstream (panners, bed decodes, reverb, the
monitor, the device sink, calibration) is driven from it.

The CAVE installation is 26 speakers — that is the target deployment, not an engine
limit. A collaborator's 12- or 24-speaker rig loads its own layout into the same
binary and the device opens that many channels.

## Profiles

You select a profile at startup (`BwConfig.profile`); usage from the engine is
identical across all three:

| profile    | bus consumers                         | tracking needed | Dante HW |
|------------|---------------------------------------|-----------------|----------|
| `cave`     | ASIO/DVS                              | position only   | yes      |
| `binaural` | binaural monitor → stereo             | full head pose  | no       |
| `both`     | ASIO/DVS + binaural monitor → stereo  | full head pose  | yes      |

`binaural` runs the array render into memory; only the stereo monitor touches a
device, so it needs no Dante hardware. This is the desk-development profile.

`both` gives you a live headphone monitor of the CAVE render while standing at the
rack. The two device clocks are independent: the monitor isn't sample-locked to the
array, it just reads the same bus.

## Tracking asymmetry (a correctness point)

The two consumers need different tracking, and the API reflects this:

- **Array render** needs listener **position only**. Head orientation is irrelevant:
  this is real sound from real speakers, and the listener's actual ears localize it.
- **Binaural monitor** needs full head **pose** (position + orientation), because it
  simulates the room on headphones and must rotate the virtual speakers with the head.

So the tracking layer always provides full pose; the array renderer ignores the
orientation component.

## Locked decisions

- **Transport: ASIO, not WDM.** DVS's WDM driver caps at 16 channels; ASIO carries
  up to 64. The CAVE's 26 channels make ASIO mandatory. The device must expose enough
  outputs for your layout — 26 for the CAVE array.
- **ASIO SDK used directly** under its GPLv3 option (dual-licensed GPLv3/proprietary
  as of Oct 2025), for direct access to the timing hooks. See `docs/build.md` for
  copyleft notes.
- **Steam Audio via its C API** (not the Unity/FMOD integration). The C API supports
  custom speaker layouts (`IPLSpeakerLayout` with `IPL_SPEAKERLAYOUTTYPE_CUSTOM`,
  unit-direction speakers); the integrations do not expose this. The same dependency
  carries the whole acoustics stack: the binaural HRTF decode (`src/steam_decode.c`),
  occlusion + per-band transmission EQ + directivity (`src/steam_scene.c`), the
  reflection bed with an optional baked mode (`src/steam_reflect.c`), and sound
  pathing (`src/steam_path.c`) — all wired up in `src/engine.c` at `bw_start`.
- **Spatialization: listener-relative DBAP**, recomputed per frame from tracked
  position. Pure ambisonics fails for localized point sources here: its single sweet
  spot does not survive a 3×3 m roam. DBAP is the default; SPCAP and VBAP are
  selectable for fixed-listener installs (`bw_set_panner`), and `bw_set_dual_band`
  adds an optional dual-band mode on top of whichever panner is active. See
  `docs/spatialization.md`.
- **Concurrency: two SPSC rings** (commands down, events up), a voice table owned by
  the audio thread, staging→active promotion on commit, per-voice dirty flags,
  generation-counted handles, and a retire-ack handshake for sound-buffer lifetime.
  See `docs/concurrency.md`.

## Dependencies (minimal by design)

The engine core links four external pieces:

- **ASIO SDK** (GPLv3 option, vendored) — the device backend.
- **Steam Audio (phonon)** — HRTF decode, occlusion, reflections, pathing. Optional:
  auto-detected at `third_party/steam-audio-artifacts/` (`BW_HAVE_STEAMAUDIO`);
  without it the simple-pan monitor is the fallback.
- **dr_libs** (dr_wav/dr_flac/dr_mp3) — WAV/FLAC/MP3 decode in `src/sound.c`.
- **cJSON** — `cave_layout.json` parsing in `src/layout.c`.

dr_libs and cJSON are fetched and pinned by CMake, not vendored. The NatNet
(OptiTrack) consumer is first-party code in `src/natnet.c`, written off-wire — the
proprietary SDK is a wire-format reference only, never linked.

The opt-in tools carry their own stack, and the engine links none of it:
**imgui / implot / implot3d / imgui_test_engine** plus **raylib / rlImGui** for
`bw_playground`, `bw_layout_tool`, and `bw_calib_view`. **Intel Embree** is an
optional runtime acceleration for the ray-traced sims (`BWAUDIO_EMBREE=1`, with a
graceful fallback to the default tracer).

Everything else is first-party. See `docs/build.md`.
