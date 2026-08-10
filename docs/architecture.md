# Architecture

This doc names most of the engine's moving parts. For a one-line definition of any
spatial-audio term it uses, see [glossary.md](./glossary.md).

## Goal

Render spatialized audio for a CAVE. A tracked observer moves within a ~3×3 m area
inside a **26-speaker array**. A game engine (Unity and/or Unreal) drives it.
Output goes to an **RME Digiface Dante** over **ASIO**. Latency and timing
precision are first-class requirements.

## Top-level decision: self-hosted core, engines as control clients

The spatializer, mixer, device output, and tracking ingest all live in one native
process behind a single audio callback. Unity/Unreal call a C ABI to trigger sounds
and push positions. **No rendered audio crosses the boundary**: the mix never
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
  the machine that runs the endpoint. The alternative is an external renderer (Spat,
  SSR, Max) driven over OSC. That means a second implementation in a second system:
  authored separately, versioned separately, synchronized at runtime, and maintained by
  whoever still knows that system. In-process control leaves nothing to keep in
  sync.
- **No middleware jitter.** Control is in-process over a lock-free ring, not OSC/UDP
  to a separate process with its own clock.

The cost: you own voice management, wav playback, and the ASIO host glue.

## The central seam: an in-memory master bus

The panner does not write "to the device". It writes to an in-memory **master bus**,
one channel per speaker. *Consumers* read that bus:

- **ASIO device sink (production):** writes the bus straight to the Digiface.
- **Array-sim monitor (`cave_sim`, and `cave_both`'s tap):** treats each bus channel
  as a virtual speaker at that speaker's surveyed room position, HRTFs them to
  stereo, and writes to an ordinary output device (a headphone DAC). It sits
  *after* the panner, so it auditions the real array render (DBAP behavior and
  all), not an idealized version.

The sim monitor is just a second consumer of the bus. Keep it that way. The
abstraction set is: a **render target** (the bus), one or more **device sinks**
consuming buses, and the monitor as a **bus→bus transform** (N→2) feeding a
stereo sink.

The **direct binaural render** (`BWA_PROFILE_BINAURAL`) is the one deliberate
extension of that picture: point voices bypass the speaker panner and SH-encode at
their true listener-relative directions into a 16-channel **direct field** beside
the bus (with the SDK, each point voice's dry additionally rides its own
`IPLBinauralEffect`). Beds pass SH→SH into the same field, and the pathing
accumulator sums in raw. The bus keeps the synthesized-diffuse taps (the FDN tail,
the reflection bed), and one HRTF decode consumes both. It is still "render
targets + consumers": the direct field and the point taps are profile-gated render
targets, not a parallel engine. Anything synthesized-diffuse belongs on the bus.
Full render description:
[spatialization.md](./spatialization.md#headphone-renders-direct-binaural-and-the-array-sim).

```
            sources
              │  per-voice, listener-relative DBAP (recomputed on move)
              │  (BINAURAL profile: per-voice SH encode at the true direction instead)
              ▼
      ┌───────────────────┐   ┌──────────────────────────┐
      │  N-ch master bus  │   │ 16-ch direct field (SH)  │  (BINAURAL only)
      └───────────────────┘   └──────────────────────────┘
        │                  │            │
   per-ch gain/delay   bus→ambisonics ──┴─→ HRTF
   align               (one binaural decode for both)
        │                  │
     ASIO ► Digiface         stereo device
   (cave, cave_both)    (binaural · cave_sim · cave_both's tap)
```

### The full render path

The seam diagram above is the shape. This is the whole plumbing: every signal kind, in
processing order. Side taps (`└→`) leave the chain at that point and land on one of the
named buses. The main chain continues downward. Everything between the two rules runs on
the audio thread inside one `rt_render` block. ([`signal-flow.md`](signal-flow.md) carries
the same diagram as a rendered Mermaid graph, with each stage annotated with the functions
that implement and configure it. This ASCII version is canonical for structure.)

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

═ audio thread - rt_render, one block ═══════════════════════════════════════════════

 drain commands · sample the tracked pose (+ prediction lead) - a move dirties every solve

 MONO VOICE (mix_voice) - per sample                 gain solve - block rate, dirty-gated
 ───────────────────────────────────                 ─────────────────────────────────────
 read: pcm cursor · pitch resample · stream pull     panner DBAP/SPCAP/VBAP at the tracked
 × pause/seek gate                                   listener, energy-meaned over extra
 │ └→ s_raw → bending-loss EQ → × shCoeffs           listeners → per-source atten override
 │            → PATH ACCUM (ambisonic)               (by ratio) → spread render (LOBE ·
 transmission EQ (3 biquads - occlusion's tilt)      MDAP ring · SPECTRAL 6-band targets;
 × occlusion level × directivity                     w×h extent · near-spread + size floors)
 │ ├→ × wet send (× distance) → AUX (mono)           → dual-band low derivation; × user gain
 │ └→ ISM: per-voice ring → 6 shoebox mirror         × group gain × timed fades
 │       images (frac delay · HF damp · per-         BINAURAL profile: the solve is instead
 │       image panner gains) ──────────→ BUS         16 SH gains at the TRUE direction ×
 air-absorption LP → loudness shelf → Doppler ring   atten × gain (spread = per-degree
 │ └→ decor split (× √spread) ─────────→ DECOR       taper); voice + ISM images then
 pan: single gains · dual-band (700 Hz) ·            accumulate to DIRECT, not BUS (dual-
      spectral (6 bands × 6 gain sets) ─→ BUS        band/decor/spectral gated off). Mode 2
                                                     (SDK): spread power-splits the dry -
                                                     point share √(1−s) onto the voice's OWN
                                                     mono tap (per-voice HRTF), wide √s onto
                                                     the SH field

 BED VOICE (mix_bed) - per sample
 ────────────────────────────────
 SH frames → rotate (yaw phasor · full 3-axis Ivanic-Ruedenberg matrix; glided)
 ├→ matrix render: × max-rE taper (crossfaded; opt. band-split - taper > 700 Hz
 │       only, rV decode below) → bed decode (AllRAD · EPAD) ────────────────→ BUS
 ├→ BINAURAL profile: SH->SH pass - × ambi_canon_to_phonon (one diagonal) ──→ DIRECT
 │       (max-rE + parametric are speaker-decode concerns: gated off)
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
          └→ BINAURAL profile: summed raw instead (already phonon-basis SH) ─→ DIRECT

 output stage - everything that reaches a device passes through, in this order
 ─────────────────────────────────────────────────────────────────────────────
 × master gain (ramped)
 align: per-speaker correction FIR · room-EQ biquads (re-aimed at the tracked pose) ·
        gain trim · delay (re-referenced onto the tracked head: bwa_set_tracked_align,
        slewed + dead-zoned; off = the exact integer tap)
 + test signal (bwa_set_test_signal - a raw channel, deliberately post-align)
 linked limiter (default −1 dBFS) → per-channel peak meters
      │
      ├ cave       26-ch ASIO ► Digiface ► the array
      ├ binaural   DIRECT (SH, unlimited - hard-clamped post-decode) + the bus's
      │            virtual-speaker encode → ONE phonon HRTF decode, then the mode-2
      │            per-voice point taps each through their OWN IPLBinauralEffect,
      │            summed (cardioid+pan fallback, field-only) → 2 ch
      ├ cave_sim   each bus channel = a virtual speaker at its room position →
      │            3rd-order SH encode → phonon HRTF decode (simple-pan fallback) → 2 ch
      ├ cave_both  the array sink + the cave_sim monitor on a second device (double-buffered)
      └ null       no device: keeps rendering in real time, silent (tools' visual mode)
```

The tap ordering is deliberate, not incidental:

- The **reverb send and the ISM images branch off before air absorption, loudness comp,
  and Doppler**: those three model the *direct* path's propagation, and a reflection
  travels its own path (the ISM applies its own delay/damping per image; the reverb bed
  models the room's).
- **Pathing taps `s_raw` before the occlusion EQ**: the indirect route goes *around* the
  occluder, so it must not inherit the direct path's muffling. It takes its own
  bending-loss tilt instead.
- The **decorrelation split leaves right before panning** so the incoherent share carries
  the full per-voice processing, and the velvet filters run once per *channel* (after the
  voice loop), not per voice.
- **max-rE weighting** lives where the engine's own SH→speaker decode renders bed signal
  (the matrix renderer, the FDN's line render): broadband, or band-split (the taper only
  above 700 Hz; the rV-optimal plain decode keeps the low band; bed matrix paths only, the
  FDN stays broadband). The parametric analysis and its re-panned direct stream see the
  raw field, and phonon's decodes (reflection bed, pathing, the HRTF monitor) are its own.
- **Master gain sits before align** so per-speaker trims stay calibrated. The **test
  signal enters after align** so a wiring check is a raw channel, untouched by trims or
  delays. The **limiter is last** so nothing (test signal included) can clip a driver.

### How wide is the bus?

**The layout's speaker count.** `BWA_CHANNELS` (26, `src/sink.h`) is the compile-time
*capacity*. The **active** count is whatever the loaded `cave_layout.json` declares
(any N in 4..26, fixed at `bwa_create`). Everything downstream follows that count.
The CAVE installation is 26 speakers. That is the target deployment, not an engine
limit. Details and the failed-load fence: [api.md](./api.md#channel-count).

## Profiles

Select a profile at startup (`bwa_desc.profile`). Usage from the engine is
identical across all four:

| profile     | what renders                                              | tracking needed | Dante HW |
|-------------|-----------------------------------------------------------|-----------------|----------|
| `cave`      | bus → ASIO/Digiface                                       | position only   | yes      |
| `binaural`  | direct field + diffuse bus → one HRTF decode → stereo     | full head pose  | no       |
| `cave_sim`  | bus → virtual-speaker monitor → stereo                    | full head pose  | no       |
| `cave_both` | bus → ASIO/Digiface + the `cave_sim` monitor → stereo     | full head pose  | yes      |

`binaural` is the first-class headphone render: point sources at their true
directions, no array simulation in the direct path. `cave_sim` auditions the
ARRAY render: same bus, DBAP artifacts included, the desk-verification profile.
Both run the array render into memory. Only the stereo device opens, so neither
needs Dante hardware.

`cave_both` gives you a live headphone monitor of the CAVE render while you stand at
the rack. The two device clocks are independent: the monitor isn't sample-locked to
the array, it just reads the same bus.

## Tracking asymmetry (a correctness point)

The renders need different tracking, and the API reflects this:

- **Array render** needs listener **position only**. Head orientation is irrelevant:
  this is real sound from real speakers, and the listener's actual ears localize it.
- **The headphone renders** need full head **pose** (position + orientation): the
  sim monitor rotates the virtual speakers with the head, and the direct render
  applies orientation at the HRTF decode.

So the tracking layer always provides full pose. The array renderer ignores the
orientation component.

## Locked decisions

- **Transport: ASIO, not WDM.** WDM is a consumer path with its own mixing, resampling and
  channel limits. ASIO is the multichannel low-latency route, and the only one that gives
  you the timing hooks below. The CAVE's 26 channels make it mandatory. The device must
  expose enough outputs for your layout: 26 for the CAVE array.
- **ASIO SDK used directly** under its GPLv3 option (dual-licensed GPLv3/proprietary
  as of Oct 2025), for direct access to the timing hooks. See `docs/build.md` for
  copyleft notes.
- **Steam Audio via its C API** (not the Unity/FMOD integration). The C API supports
  custom speaker layouts (`IPLSpeakerLayout` with `IPL_SPEAKERLAYOUTTYPE_CUSTOM`,
  unit-direction speakers); the integrations do not expose this. The same dependency
  carries the whole acoustics stack: the binaural HRTF decode (`src/steam_decode.c`),
  occlusion + per-band transmission EQ + directivity (`src/steam_scene.c`), the
  reflection bed with an optional baked mode (`src/steam_reflect.c`), and sound
  pathing (`src/steam_path.c`), all wired up in `src/engine.c` at `bwa_start`.
- **Spatialization: listener-relative DBAP**, recomputed per frame from tracked
  position. Pure ambisonics' single sweet spot does not survive a 3×3 m roam. SPCAP
  and VBAP are selectable for fixed-listener installs. The full argument and the
  measured sweet-spot numbers: `docs/spatialization.md`.
- **Concurrency: two SPSC rings** (commands down, events up), a voice table owned by
  the audio thread, staging→active promotion on commit, per-voice dirty flags,
  generation-counted handles, and a retire-ack handshake for sound-buffer lifetime.
  See `docs/concurrency.md`.

## Dependencies (minimal by design)

The engine core links four external pieces:

- **ASIO SDK** (GPLv3 option, vendored): the device backend.
- **Steam Audio (phonon)**: HRTF decode, occlusion, reflections, pathing. Optional:
  auto-detected at `third_party/steam-audio-artifacts/` (`BWA_HAVE_STEAMAUDIO`).
  Without it, the simple-pan monitor is the fallback.
- **dr_libs** (dr_wav/dr_flac/dr_mp3): WAV/FLAC/MP3 decode in `src/sound.c`.
- **cJSON**: `cave_layout.json` parsing in `src/layout.c`.

CMake fetches and pins dr_libs and cJSON. Neither is vendored. The NatNet
(OptiTrack) consumer is first-party code in `src/natnet.c`, written off-wire. The
proprietary SDK is a wire-format reference only, never linked.

The opt-in tools carry their own stack, and the engine links none of it:
**imgui / implot / implot3d / imgui_test_engine** plus **raylib / rlImGui** for
`bwa_playground`, `bwa_layout_tool`, and `bwa_calib_view`. **Intel Embree** is an
optional acceleration for the ray-traced sims (`bwa_desc.embree`, with a
graceful fallback to the default tracer).

Everything else is first-party. See `docs/build.md`.
