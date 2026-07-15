# Architecture

## Goal

Render spatialized audio for a CAVE: a tracked observer moving within a ~3×3 m area
inside a **26-speaker array**, fed from a game engine (Unity and/or Unreal). Output
goes to a **Dante Virtual Soundcard (DVS)** over **ASIO**. Latency and timing
precision are first-class requirements.

## Top-level decision: self-hosted core, engines as control clients

The spatializer, mixer, device output, and tracking ingest all live in one native
process behind a single audio callback. Unity/Unreal call a C ABI to trigger sounds
and push positions. **No rendered audio crosses the boundary** — the mix never
routes through the game engine, only control. (The opt-in push-source API feeds
caller-generated PCM *into* the engine over the same control thread; that is a
source feed, not a render path.)

Why:

- **Timing.** Direct ASIO gives you the hardware-anchored clock the design needs:
  `bufferSwitchTimeInfo` delivers an `ASIOTime` with sample position and a
  nanosecond `systemTime`, plus `ASIOGetSamplePosition` on demand. Middleware hides
  these.
- **Engine-agnostic.** One core, two thin clients. The only per-engine code reads
  transforms and tracking.
- **One system to run and author.** The experiment deploys as a single process on
  the machine running DVS. The alternative — an external renderer (Spat, SSR, Max)
  driven over OSC — means a second implementation in a second system: authored
  separately, versioned separately, synchronized at runtime, and maintained by
  whoever still knows that system. In-process control leaves nothing to keep in
  sync.
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

### The full render path

The seam diagram above is the shape; this is the whole plumbing — every signal kind, in
processing order. Side taps (`└→`) leave the chain at that point and land on one of the
named buses; the main chain continues downward. Everything between the two rules runs on
the audio thread inside one `rt_render` block. (The same diagram as a rendered Mermaid
graph, with each stage annotated with the functions that implement and configure it:
[`signal-flow.md`](signal-flow.md) — this ASCII version is canonical for structure.)

```
 control thread (bwa_*)                                off-thread producers
 ──────────────────────                                ────────────────────
 wav/flac/mp3 → decode + resample → mono asset         NatNet → pose seqlock
 AmbiX file ─────────────→ bed asset (4/9/16 ch)       scene sim (30 Hz, phonon):
 FuMa file → reorder + rescale → the same bed asset      occlusion · transmission tilt ·
 disk file → streaming thread → per-stream ring          directivity  (atomic publishes,
 caller PCM → push API (caller is the ring producer)     handle-gated)
                                                       path sim (10 Hz, phonon):
 every bwa_* call → command ring (SPSC)                  per-voice shCoeffs + bending tilt
 bwa_commit promotes pending → active (one snapshot)

═ audio thread — rt_render, one block ═══════════════════════════════════════════════

 drain commands · sample the tracked pose (+ prediction lead) — a move dirties every solve

 MONO VOICE (mix_voice) — per sample                 gain solve — block rate, dirty-gated
 ───────────────────────────────────                 ─────────────────────────────────────
 read: pcm cursor · pitch resample · stream pull     panner DBAP/SPCAP/VBAP at the tracked
 × pause/seek gate                                   listener, energy-meaned over extra
 │ └→ s_raw → bending-loss EQ → × shCoeffs           listeners → spread render (LOBE ·
 │            → PATH ACCUM (ambisonic)               MDAP ring · SPECTRAL 6-band targets;
 transmission EQ (3 biquads — occlusion's tilt)      near-spread + metric-size floors) →
 × occlusion level × directivity                     dual-band low derivation; × user gain
 │ ├→ × wet send (× distance) → AUX (mono)           × group gain × timed fades
 │ └→ ISM: per-voice ring → 6 shoebox mirror
 │       images (frac delay · HF damp · per-
 │       image panner gains) ──────────→ BUS
 air-absorption LP → loudness shelf → Doppler ring
 │ └→ decor split (× √spread) ─────────→ DECOR
 pan: single gains · dual-band (700 Hz) ·
      spectral (6 bands × 6 gain sets) ─→ BUS

 BED VOICE (mix_bed) — per sample
 ────────────────────────────────
 SH frames → rotate (yaw phasor · full 3-axis Ivanic-Ruedenberg matrix; glided)
 ├→ matrix render: × max-rE taper (crossfaded) → bed decode (SAD · AllRAD) ─→ BUS
 └→ parametric render (crossfaded): FOA band split → DirAC direction + diffuseness ψ
      direct  √(1−ψ)·W → listener-relative panner at the array shell ───────→ BUS
      diffuse √ψ·FOA  → bed decode (raw) ───────────────────────────────────→ DECOR

 after the voice loop
 ────────────────────
 DECOR → per-channel sparse velvet-noise filters (mutually incoherent copies) → BUS
 AUX ──→ the ONE reverb tap → BUS:   Steam bed (convolve → ambisonic IR → phonon
         decode)  ·or·  FDN (16 lines · Householder · 2-band decay · direction-
         scaled · lines rendered as plane waves through the bed decode + max-rE pair)
 PATH ACCUM → path tap: phonon's own ambisonics decode ───────────────────────→ BUS

 output stage — everything that reaches a device passes through, in this order
 ─────────────────────────────────────────────────────────────────────────────
 × master gain (ramped)
 align: per-speaker correction FIR · room-EQ biquads (re-aimed at the tracked pose) ·
        gain trim · delay
 + test signal (bwa_set_test_signal — a raw channel, deliberately post-align)
 linked limiter (default −1 dBFS) → per-channel peak meters
      │
      ├ cave      26-ch ASIO ► DVS ► the array
      ├ binaural  each bus channel = a virtual speaker at its room position →
      │           3rd-order SH encode → phonon HRTF decode (simple-pan fallback) → 2 ch
      ├ both      the array sink + the monitor on a second device (double-buffered)
      └ null      no device: keeps rendering in real time, silent (tools' visual mode)
```

The tap ordering is deliberate, not incidental:

- The **reverb send and the ISM images branch off before air absorption, loudness comp,
  and Doppler** — those three model the *direct* path's propagation, and a reflection
  travels its own path (the ISM applies its own delay/damping per image; the reverb bed
  models the room's).
- **Pathing taps `s_raw` before the occlusion EQ** — the indirect route goes *around* the
  occluder, so it must not inherit the direct path's muffling; it takes its own
  bending-loss tilt instead.
- The **decorrelation split leaves right before panning** so the incoherent share carries
  the full per-voice processing, and the velvet filters run once per *channel* (after the
  voice loop), not per voice.
- **max-rE weighting** lives where the engine's own SH→speaker decode renders bed signal
  (the matrix renderer, the FDN's line render). The parametric analysis and its re-panned
  direct stream see the raw field, and phonon's decodes (reflection bed, pathing, the
  HRTF monitor) are its own.
- **Master gain sits before align** so per-speaker trims stay calibrated; the **test
  signal enters after align** so a wiring check is a raw channel, untouched by trims or
  delays; the **limiter is last** so nothing — test signal included — can clip a driver.

### How wide is the bus?

**The layout's speaker count.** `BWA_CHANNELS` (26, `src/sink.h`) is the compile-time
*capacity*; the **active** count is whatever the loaded `cave_layout.json` declares —
any N in **4..26**, resolved at `bwa_create` and fixed for the engine's lifetime. With
no `layout_path` you get the built-in 26-speaker default grid. `bwa_get_channel_count()`
is the readback, and everything downstream (panners, bed decodes, reverb, the
monitor, the device sink, calibration) is driven from it.

The CAVE installation is 26 speakers — that is the target deployment, not an engine
limit. A collaborator's 12- or 24-speaker rig loads its own layout into the same
binary and the device opens that many channels.

## Profiles

You select a profile at startup (`bwa_desc.profile`); usage from the engine is
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
  pathing (`src/steam_path.c`) — all wired up in `src/engine.c` at `bwa_start`.
- **Spatialization: listener-relative DBAP**, recomputed per frame from tracked
  position. Pure ambisonics fails for localized point sources here: its single sweet
  spot does not survive a 3×3 m roam. DBAP is the default; SPCAP and VBAP are
  selectable for fixed-listener installs (`bwa_set_panner`), and `bwa_set_dual_band`
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
  auto-detected at `third_party/steam-audio-artifacts/` (`BWA_HAVE_STEAMAUDIO`);
  without it the simple-pan monitor is the fallback.
- **dr_libs** (dr_wav/dr_flac/dr_mp3) — WAV/FLAC/MP3 decode in `src/sound.c`.
- **cJSON** — `cave_layout.json` parsing in `src/layout.c`.

dr_libs and cJSON are fetched and pinned by CMake, not vendored. The NatNet
(OptiTrack) consumer is first-party code in `src/natnet.c`, written off-wire — the
proprietary SDK is a wire-format reference only, never linked.

The opt-in tools carry their own stack, and the engine links none of it:
**imgui / implot / implot3d / imgui_test_engine** plus **raylib / rlImGui** for
`bwa_playground`, `bwa_layout_tool`, and `bwa_calib_view`. **Intel Embree** is an
optional acceleration for the ray-traced sims (`bwa_desc.embree`, with a
graceful fallback to the default tracer).

Everything else is first-party. See `docs/build.md`.
