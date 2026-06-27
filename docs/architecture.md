# Architecture

## Goal

Render spatialized audio for a CAVE: a tracked observer moving within a ~3×3 m area
inside a **26-speaker array**, fed from a game engine (Unity and/or Unreal). Output
is a **Dante Virtual Soundcard (DVS)** driven over **ASIO**. Latency and timing
precision are first-class requirements; jitter from middleware or cross-process
control is to be avoided.

## Top-level decision: self-hosted core, engines as control clients

The spatializer, mixer, device output, and tracking ingest all live in one native
process behind a single audio callback. Unity/Unreal call a C ABI to trigger sounds
and push positions; **no audio buffers cross the boundary**, only control.

Rationale:
- **Timing.** Direct ASIO gives the hardware-anchored clock the design needs —
  `bufferSwitchTimeInfo` delivers an `ASIOTime` with sample position and a
  nanosecond `systemTime`, plus `ASIOGetSamplePosition` on demand. Middleware hides
  these.
- **Engine-agnostic.** One core, two thin clients. The library is portable across
  Unity and Unreal by construction; the only per-engine code reads transforms and
  tracking.
- **No middleware jitter.** Control is in-process over a lock-free ring, not OSC/UDP
  to a separate process with its own clock. (An OSC/Max renderer was considered and
  rejected on jitter/complexity grounds.)

Cost: we own voice management, wav playback, and the ASIO host glue. For bounded
research stimuli this is modest and squarely in scope.

## The central seam: a 26-channel in-memory master bus

The panner does not write "to the device." It writes to an in-memory **26-channel
master bus**. *Consumers* read that bus:

- **ASIO device sink (production):** writes the 26-ch bus straight to DVS.
- **Binaural monitor (debug):** treats each of the 26 channels as a virtual speaker
  at that speaker's surveyed room position, HRTFs them to stereo, and writes to an
  ordinary output device (a headphone DAC). Because it sits *after* the panner, it
  auditions the real array render — DBAP behavior and all — not an idealized version.

This seam is why the binaural path was a clean addition rather than a complication:
it is just a second consumer. Keep it that way. The abstraction set is: a **render
target** (the bus), one or more **device sinks** consuming buses, and the binaural
monitor as a **bus→bus transform** (26→2) feeding a stereo sink.

```
            sources
              │  per-voice, listener-relative DBAP (recomputed on move)
              ▼
      ┌───────────────────┐
      │ 26-ch master bus  │  (in memory, owned by audio thread)
      └───────────────────┘
        │                  │
   per-ch gain/delay   26→ambisonics→HRTF
   align               (single binaural decode)
        │                  │
     ASIO ► DVS         stereo device
     (cave)             (binaural debug)
```

## Profiles

Selected at startup (`BwConfig.profile`); usage from the engine is identical across
all three:

| profile    | bus consumers                         | tracking needed | Dante HW |
|------------|---------------------------------------|-----------------|----------|
| `cave`     | ASIO/DVS                              | position only   | yes      |
| `binaural` | binaural monitor → stereo             | full head pose  | no       |
| `both`     | ASIO/DVS + binaural monitor → stereo  | full head pose  | yes      |

`binaural` runs the array render into memory and only the stereo monitor touches a
device, so it needs no Dante hardware — this is the desk-development profile. `both`
gives a live headphone monitor of the CAVE render while standing at the rack; the
two device clocks are independent and that is fine, because the monitor isn't
sample-locked to the array — it just reads the same bus.

## Tracking asymmetry (a correctness point)

The two consumers need different tracking, and the API reflects this:
- **Array render** needs listener **position only**. Head orientation is irrelevant
  — it is real sound from real speakers; the listener's actual ears localize it.
- **Binaural monitor** needs full head **pose** (position + orientation), because it
  simulates the room on headphones and must rotate the virtual speakers with the head.

So the tracking layer always provides full pose; the array renderer ignores the
orientation component.

## Locked decisions

- **Transport: ASIO, not WDM.** DVS's WDM driver caps at 16 channels; ASIO carries
  up to 64. 26 channels makes ASIO mandatory.
- **ASIO SDK used directly** under its GPLv3 option (dual-licensed GPLv3/proprietary
  as of Oct 2025). Windows-only tool, so PortAudio's portability buys nothing and
  direct access to timing hooks is preferred. See `docs/build.md` for copyleft notes.
- **Steam Audio via its C API** (not the Unity/FMOD integration). The C API supports
  custom speaker layouts (`IPLSpeakerLayout` with `IPL_SPEAKERLAYOUTTYPE_CUSTOM`,
  unit-direction speakers); the integrations do not expose this. Used for the
  binaural decode now, and available for occlusion/reflections later with no new
  dependency.
- **Spatialization: listener-relative DBAP**, recomputed per frame from tracked
  position. Pure ambisonics is rejected for localized point sources because a single
  sweet spot fails across a 3×3 m roam. See `docs/spatialization.md`.
- **Concurrency: two SPSC rings** (commands down, events up), a voice table owned by
  the audio thread, staging→active promotion on commit, per-voice dirty flags,
  generation-counted handles, and a retire-ack handshake for sound-buffer lifetime.
  See `docs/concurrency.md`.

## Dependencies (minimal by design)

ASIO SDK (GPLv3 option), Steam Audio (binaural; later occlusion/reflections), a
single-header wav loader (dr_wav), and a NatNet consumer for OptiTrack. Everything
else is first-party. See `docs/build.md`.
