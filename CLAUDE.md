# CLAUDE.md

Guidance for working in this repository. Read this first, then the file under
`docs/` relevant to the task. The design is settled and implemented through M6
plus the calibration/tooling work (see "Current state" below) — most work now
extends the engine or verifies it on hardware, against these specs.

## What this is

A self-hosted native (C/C++) spatial audio engine for a CAVE installation. It
drives a **26-speaker array** over **ASIO** into an **RME Digiface Dante** (a hardware
Dante endpoint), with **binaural (HRTF) headphone output** as a second path — a
first-class direct render (`BWA_PROFILE_BINAURAL`) and an array-audition monitor
(`BWA_PROFILE_CAVE_SIM`). Unity and
Unreal are *thin control clients* over a C ABI — no rendered audio crosses that
boundary, only control (sound triggers, source positions, listener pose). The one
inbound exception is the opt-in push-source feed (caller PCM *into* the engine,
control thread) — a source feed, not a render path.

The engine is deliberately *not* built on FMOD/Wwise/middleware. Self-hosting buys
direct access to ASIO timing hooks (`ASIOTime.systemTime`, `ASIOGetSamplePosition`)
and a clean, engine-agnostic core. See `docs/architecture.md` for the why.

## The seam that organizes everything

Sources → per-voice **listener-relative DBAP** panning → an in-memory
**26-channel master bus**. The bus has *consumers*:
- **ASIO device** (production): writes the 26-ch bus straight to the Digiface.
- **Array-sim monitor** (`cave_sim`, and `cave_both`'s tap): treats each bus channel
  as a virtual speaker at its room position, HRTFs to stereo, writes to a normal
  output device.

Adding the sim path did not complicate the core — it is just a second consumer of
the same bus. Protect that property. The one deliberate extension is the
**direct binaural render** (`BWA_PROFILE_BINAURAL`): point voices bypass the
panner — per-voice HRTF convolutions with the SDK (mode 2: rt exposes per-voice
mono taps + directions; spread power-splits toward the field), a 16-ch SH direct
field otherwise — beds pass SH->SH into that field (one diagonal,
ambi_canon_to_phonon) and pathing sums in raw; ONE HRTF decode + the per-voice
convolutions produce stereo. The bus keeps the synthesized-diffuse taps
(FDN/reflection bed). These are profile-gated render targets, not a parallel
engine — anything synthesized-diffuse still belongs on the bus.

## Hard invariants (do not violate)

These are real-time-audio correctness rules. Breaking them causes dropouts/glitches
that are painful to debug, so they are non-negotiable:

1. **No allocation, locks, syscalls, or file I/O on the audio thread.** The audio
   thread is the ASIO `bufferSwitch` callback. wav decode, malloc/free, and logging
   all live on the control thread.
2. **One control thread.** All `bwa_*` calls come from a single thread. The command
   ring is single-producer/single-consumer; a second producer breaks it.
3. **Audio thread owns DSP state** (voice table, bus, panner gains, listener
   active fields). The control thread owns handle allocation and asset memory.
   They communicate only through the two SPSC rings.
4. **Gains ramp, never jump.** Per-voice `gcur -> gtarget` interpolated across the
   block. A discontinuous 26-gain change is audible zipper noise.
5. **Generation counts gate handle reuse.** A stale source handle must be dropped,
   not acted on. Sound *buffers* additionally need the retire-ack handshake before
   the control thread frees them.
6. **`CMD_COMMIT` defines frame coherence.** Position/pose write to *pending* fields;
   only commit promotes them to *active*. The mixer reads only active fields.

See `docs/concurrency.md` for the full model and reference code.

## Repo layout (intended)

```
include/bw_audio.h      Public C ABI (authoritative contract).
src/
  engine.c             public ABI: lifecycle + sink + forwards per-frame calls to rt. [M0/M1/M2]
  rt.h / rt.c          rings, voice table, commit snapshot, generation handles, mixer. [M2]
  sink.h / sink.c      device-sink abstraction + backend dispatch. [M1]
  null_sink.c          offline (no-hardware) sink: threaded silence + timestamps. [M1]
  asio_sink.cpp        ASIO host: driver load, bufferSwitch, sample-pos timestamp. [M1]
  sound.h / sound.c    wav decode to mono float via dr_wav (Sound table lives in rt.c). [M3]
  layout.h / layout.c  speaker geometry load (cave_layout.json via cJSON) + default grid. [M4]
  measure.c/calib.c    bwa_calibrate DSP: sweep+deconvolution, trims, trilateration, room report. [calib]
  zylia.h / zylia.c    Zylia ZM-1: single-position speaker localization (TDOA + GN position) AND the
                       validation-grade estimators — active-intensity DOA, capsule integrity,
                       SRP-PHAT cross-check, comb depth (spectral ripple: what coherent multi-speaker
                       copies cost in timbre, the measurable side of SPCAP focus). [calib]
  valid.h / valid.c    phantom-localization validation: render a source, measure where the array
                       actually put it (feeds/simulate/score + medians, bootstrap, matched-cell
                       contrasts). The PHANTOM arm renders through a REAL ENGINE CORE (a cached
                       RtCore + push voice, limiter off, ramps settled, deterministic timestamp --
                       the same path bwa_render_block/BWA_SINK_MANUAL drives), so every live A/B knob
                       (ValidRender: focus/density, dual-band, CAP, hole spread, tracked align,
                       spread mode/decorrelation/near spread) is sweepable and valid_simulate just
                       propagates those feeds to the 19 capsules. The PHYSICAL REFERENCE arm (drive
                       one speaker alone = a real source, so a phantom miss reads against a floor --
                       also the comb-depth floor) deliberately does NOT: no panner, no knob, no
                       engine state. valid_speaker_feeds_direct is the pre-engine builder, kept as
                       the regression baseline the ctest pins the engine render against. Also
                       stimulus selection (broadband or a tone, analysis band follows). Drives
                       bwa_validate. [validation]
  dbap.h / dbap.c      listener-relative, constant-power DBAP gain solve. [M4]
  cap.h / cap.c        compensated amplitude panning: projects the dual-band LOW band so the rendered
                       ITD matches a real source's for the head's CURRENT orientation. NOT a panner -
                       a modifier on whatever panner is selected, so it reduces to that panner facing
                       the source. The one place head ORIENTATION reaches the speaker path.
                       bwa_set_dual_band_cap. [spatialization]
  hole.h / hole.c      hole-aware spread floor: a source aimed where the array has NO speaker (the
                       barrel's open poles) is floored WIDE instead of split across the hull triangle
                       that closes the hole. Cached per listener like spcap/vbap; bwa_set_hole_spread. [spatialization]
  fdn.h / fdn.c        directional FDN reverb bed (phonon-free; takes the reflection bus tap). [innovations]
  ism.h / ism.c        image-source EARLY reflections: shoebox mirrors, panned as point sources. [innovations]
  align.h / align.c    per-speaker gain trim + delay-line output stage. [M4] Also the tracked-listener
                       re-reference (bwa_set_tracked_align): re-aims the trims from the layout's fixed
                       ref onto the live head, slewed + dead-zoned because every delay change is a
                       resampling event. Off = the exact integer tap, bit-identical. [spatialization]
  binaural.h/binaural.c  head-oriented 26->stereo monitor + the no-SDK cardioid decode of the
                       direct-binaural field (Steam Audio HRTF is the upgrade). [M5]
  hpeq.h / hpeq.c      headphone correction EQ: AutoEq ParametricEQ.txt -> RBJ biquad cascade on
                       the headphone profiles' final stereo (bwa_load_headphone_eq). [binaural]
  ambisonics.h/.c      3rd-order ACN/SN3D encode (+ ambi_encode_phonon, the monitor-basis encode
                       shared by steam_decode and rt's direct mode). [M5]
  steam_decode.h/.c    production ambisonics->stereo HRTF decode via phonon (with-SDK); sums the
                       direct field into the virtual-speaker encode pre-decode. [M5]
  steam_scene.h/.c     materials occlusion: IPLScene+IPLSimulator on a sim thread (with-SDK). [materials]
  steam_reflect.h/.c   reflection bed: IPLSimulator reflections -> ambisonic IR -> SH->26 bus tap (with-SDK). [materials]
  steam_path.h/.c      sound pathing: indirect routing -> per-voice shCoeffs -> SH-encode -> bus tap (with-SDK). [materials]
  natnet.c             OptiTrack pose ingest (off-wire, see docs/build.md). [M6]
test/                  ctest suite; targets are prefixed test_* (test_smoke, test_rt_core, test_rt_feature,
                       test_dsp, ...) so the built tools (bwa_*) and the tests sort apart in the bin dir.
                       The rt test is split: test_rt_core (concurrency/lifecycle spine) + test_rt_feature
                       (spatial-feature DSP toggles), sharing test/rt_test_util.h. xval_data.h is
                       GENERATED (tools/xval) — don't hand-edit.
tools/xval/            gen_reference.py: cross-validation golden generator (scipy SH / l1-LP VBAP /
                       qhull AllRAD / bilinear RBJ / lfilter) -> test/xval_data.h for the xval ctest.
                       Needs numpy+scipy; ctest itself does not (the header is committed).
bindings/
  unity/               P/Invoke + Engine/Emitter (see docs/integration.md).
  godot/               GDExtension (godot-cpp, opt-in -DBWA_BUILD_GODOT=ON): addons/bw_audio/ is the
                       drop-in addon (which CONTAINS playground/, the Godot port of
                       examples/playground.cpp, so the demo ships with every install), demo/ the
                       self-checking test scenes (CI fixtures, never shipped). No 1:1 shim — each call lives on the class owning
                       its handle (BwaEngine / BwaSource → BwaEmitter, BwaPushSource / BwaBed), plus
                       BwaMaterial, Bwa{Acoustic,Dynamic}Geometry, BwaRoomBox, BwaSpeakerView.
  unreal/              module + component — planned, not yet implemented (docs/integration.md has the notes).
docs/                  Specs. Start here.
examples/              cave_layout.json (see docs/layout-schema.md); minimal.c (the client lifecycle),
                       ambisonic.c (beds: AmbiX/FuMa load, rotate/tilt, renderer + max-rE A/B),
                       streaming.c (disk streaming + push sources) — console walkthroughs, built every build.
third_party/           asiosdk/ (GPLv3 option, fetched not committed), steam-audio-source/ (submodule) + steam-audio-artifacts/ (built phonon SDK); dr_wav + cJSON are
                       fetched by CMake (FetchContent, pinned) — see third_party/README.md.
```

## Build

Target: **Windows only** (ASIO is Windows-only; the Digiface is Windows/macOS). CMake.
A future cross-platform move means abstracting the device layer (ASIO is just the
Windows sink) — do not bake ASIO assumptions outside `asio_sink.c`.

```
cmake -S . -B build -A x64      # default generator = newest installed Visual Studio
cmake --build build --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo      # runs the full test suite (test_* targets)
```

**Current state (M6 + occlusion).** The engine builds `bw_audio.dll` and the full ctest
suite — 31 tests with the Steam Audio SDK, 26 without (the 5 SDK-gated ones are `reflect`,
`bake`, `path`, `dynmesh`, `steam_decode`) — a count that INCLUDES the three GUI-tool suites
(`calib_view`, `layout_tool`, `playground`) and the three `validate_*` runs, all under their
build flags. `rt.c` is the concurrency
spine (two SPSC rings, voice + sound tables, commit snapshot, generation handles, retire-ack)
and the whole `bwa_*` API forwards to it. Spatialization (the DBAP/SPCAP/VBAP gain solve,
layout load, per-speaker align), calibration (`bwa_calibrate`, the Zylia capsule survey),
validation (`bwa_validate`), and OptiTrack tracking (`natnet.c` + the seqlock pose handoff)
are all implemented and covered by the off-hardware test suite.

Four subsystems gate on `BWA_HAVE_STEAMAUDIO` (phonon built from the vendored submodule):
`steam_decode.c` (the production HRTF monitor upgrade), `steam_scene.c` (automatic occlusion +
transmission EQ + directivity), `steam_reflect.c` (the reflection bed), and `steam_path.c`
(sound pathing). A no-SDK build is fully viable: the simple-pan binaural monitor, ISM early
reflections + the FDN late tail for reverb, and manual occlusion cover the same ground. What
it loses is automatic occlusion, pathing, and the real HRTF monitor.

Channel count is runtime. `BWA_CHANNELS` (26, `sink.h`) is the CAPACITY; the layout's speaker
count (4..26) is the ACTIVE count, fixed per engine instance and threaded through the rt core,
sinks, monitor, and FDN. `bwa_get_channel_count()` reads it back. A failed explicit layout load
leaves `bwa_create` usable on the 26-grid fallback (reason via `bwa_last_error`), but `bwa_start`
refuses it with `BWA_ERR_LAYOUT` — only `layout_path = NULL` runs the default grid.

The three GUI tools are on the imgui stack — `calib_view` on imgui + implot + implot3d,
`layout_tool` and `playground` on rlImGui (a raylib 3D scene under imgui panels) — and each has
a `--tests` suite that drives the real UI under ctest.

For the feature-level catalog — every `bwa_*` call and what it does — see docs/api.md's
"Feature overview". NOTES.md holds the historical per-feature narration this section used to carry.

Remaining: the by-ear headphone check (HRTF quality), and live Motive verification of the
tracker path (parser + lifecycle are tested off-wire). See docs/hardware-validation.md.

## Traps

Regression-preventing gotchas. Each has bitten before or guards a real invariant:

- **Laterality checks must never drive DC.** A DC-driven `steam_decode` laterality assertion had
  inverted polarity (the default HRTF's per-ear DC gains oppose its audible ILD), mis-diagnosed a
  correct encode, and shipped a left/right mirror that only a by-ear report caught. Drive tones,
  never DC. See NOTES.md.
- **`isfinite()` is not a range check — finite-but-absurd is its own defect class.** `isfinite(3e38)`
  is TRUE, so the whole reject-non-finite guard family passes it, and then `bus * 3e38` OVERFLOWS to
  Inf. Gains are sticky, so every later block overflows too, and the Inf reaches the align delay line
  and the room-EQ biquads, whose IIR state holds it past any later correction. `test_fuzz_api` seed
  12648430 found this on master gain; the whole linear-gain family is capped at `BWA_MAX_GAIN`
  (`rt.h`, +80 dB) now. Any new value that SCALES the bus needs a magnitude cap, not just a finite
  check. Note this is invisible to a reviewer scanning for missing guards — the guard is right there.
- **`powf(base, exp)` with a negative base and a NON-INTEGER exponent is NaN**, and one NaN
  poisons a whole normalized gain vector. `spcap.c`'s lobe `0.5 + 0.5*cos` rounds to ~-5e-8 for an
  antipodal speaker, which was harmless for as long as the focus exponent was the integer 12 and
  silenced *every* default-grid SPCAP solve the moment focus became geometry-derived (12.70). The
  lobe is clamped at 0 now. Any exponent that stops being a literal integer re-opens this class:
  clamp the base, do not trust the caller's range.
- **Publish-then-flag needs the reader to acquire the flag FIRST.** `rt_set_spcap_focus` stores
  focus/density and then release-bumps `RtCore.pan_gen`; `rt_render` must acquire `pan_gen` *before*
  loading the knobs. Read in the other order it is not a memory-model subtlety but a plain
  program-order interleaving (it bites on x86): a block pairs the NEW generation with the OLD value,
  stamps every voice current, and swallows the change until the next bump, so the last set of a
  slider drag silently never takes. Same rule for any future live knob that uses a generation
  counter, and note this is untestable single-threaded, so the ordering comment is the guard.
- **`/experimental:c11atomics`** is required on MSVC for the `stdatomic.h` in `rt.c`/`stream.c`
  (and `steam_reflect.c` with the SDK) — wired per-file in CMake. `pose.h` uses Interlocked
  intrinsics instead, so `natnet.c` and its tests need no flag.
- **Do not link the NatNet SDK.** It is proprietary and conflicts with GPLv3 under distribution.
  `natnet.c` parses the wire format off-wire (reference only, never linked).
- **Proprietary VR-toolkit integrations (MiddleVR, Igloo) live OUTSIDE this repo**, in their own
  package consuming a released `com.brainworks.bw_audio`. Same reasoning as the NatNet rule: the
  Unity package is `GPL-3.0-only`, and an assembly referencing proprietary DLLs shipped inside it
  raises the same distribution conflict. Second, independent reason: CI has no license for either, so
  in-repo code would never be compiled by anything, giving sprawl AND silent drift. The sync cost is
  small because the contact patch is small (pose, registration, listener), and an integration SHOULD
  pin a released ABI rather than chase a moving one. What must track the ABI exactly (the rig-day
  harness) stays in-repo, where CI compiles it.
- **Assemblies point INWARD only.** `BwAudio` (core, no references) <- `BwAudio.RigDay` <-
  `BwAudio.RigDay.Editor`. The core must never reference a tool or an integration, and the asmdefs
  make that a compile error rather than a matter of discipline. The rig-day assemblies are
  `autoReferenced: false` so they stay out of a consumer's default reference set.
- **`tools/upm/gen-meta.ps1` must RECURSE.** An `.asmdef` governs its own folder, so assembly splits
  mean subfolders, and the original flat `Get-ChildItem -File` silently skipped every asset in one
  (folders included, which need a `.meta` as much as files do). A missing `.meta` regenerates that
  GUID per project and breaks every reference into it, which is the exact failure the script exists
  to prevent. Both existing asmdefs also carried `noBwAudioReferences`, a mangled `noEngineReferences`
  that Unity silently ignores.
- **Do not bake ASIO assumptions outside `asio_sink.cpp`.** ASIO is just the Windows sink; a
  future cross-platform move abstracts the device layer behind the sink seam.
- **`test/xval_data.h` is GENERATED** by `tools/xval/gen_reference.py` — don't hand-edit.
  Regenerating needs numpy + scipy; ctest stays hermetic on the committed header.
- **`-DBWA_ASAN=ON`** builds `test_sound` under AddressSanitizer — the control-side
  use-after-free check for the sound retire handshake.
- **A unit belongs in a NAME only when the quantity has two live units.** Time is the only one:
  frames (dsp clock, `play_at`/`stop_at`/`seek`, playheads, output latency) and seconds (fades,
  RT60, IR length, pose lead), so every time-valued name says which — `_frames` on a getter,
  `seconds`/`_s` on a parameter. Distances (meters), frequencies (Hz), angles (radians) and gains
  (linear) have no competitor, so they carry the unit on the VALUE (`radius_m`, `xover_hz`,
  `yaw_rad`) and never on the call. The one hard rule for new calls: a decibel value must say
  `_db`, because linear is the unmarked default everywhere. Full statement in docs/api.md →
  "Coordinates and units".
- **A binding call must not borrow a host-engine name with a different unit or meaning.**
  Godot's `AudioServer.get_output_latency()` is SECONDS and `AudioStreamPlayer3D.seek()` takes
  SECONDS; the binding's frame-valued twins are `get_output_latency_frames/_seconds` and
  `seek_frames/_seconds` for that reason. The 0.4.0 zip shipped the colliding spelling and it hid
  behind the null sink, which returns 0 — and 0 is 0 in any unit.
- **Runtime-printed strings are ASCII.** An en-dash in a `push_warning` becomes mojibake on a
  Windows console codepage. Comments, docs and inspector hint strings keep their punctuation;
  anything that can reach a console does not. `rg '"[^"]*[^\x00-\x7F][^"]*"' bindings/godot/src`
  should stay empty.
- **A shipped artifact must not cite a doc it does not ship.** Both packs run
  `tools/dist/doc-pointers.ps1`, which rewrites repo-doc references in the staged tree to
  permalinks at the packed commit and then fails the pack on any relative `.md` reference the
  stage cannot satisfy. The 0.4.0 Godot zip shipped three dangling ones.

## What NOT to do

- Do not introduce FMOD/Wwise or route audio through the engine's mixer.
- Do not use Unity's built-in audio (8-channel cap) or the device's WDM/DirectSound
  driver (a consumer path: its own mixing, resampling, and no timing hooks).
  26 channels requires ASIO. This is settled.
- Do not pan via pure ambisonics for localized point sources — the listener moves
  across ~3×3 m and a single sweet spot fails. DBAP is recomputed per frame from
  tracked position. See `docs/spatialization.md`.
- Do not assume Steam Audio's Unity/FMOD *integration* limits apply to its C API.
  The C API supports custom speaker layouts; the integrations do not expose them.
- Do not run the Steam reflection bed AND the ISM early reflections together — the
  bed already contains early reflections, so they render twice (engine.c warns once).
- Do not model the CAVE room itself with the ISM. Its shoebox is the *virtual*
  environment; the physical room supplies its own reflections, and modeling it
  double-counts (same trap as matching the measured RT60 — docs/calibration.md).
- Do not let any `bwa_*` per-frame call block or allocate.

## Which acoustics path (the recommendation)

Three implementations now overlap here; they are complementary, not rivals (the full
comparison + rationale is `docs/materials.md` → "Choosing an acoustics path"):

- **Steam scene** (`steam_scene.c`) for occlusion + directivity + pathing — ray tracing
  earns its keep; the manual path needs the game to already know the answer.
- **ISM** (`ism.c`) for early reflections — the Steam bed is listener-CENTRIC (one field
  around one point, 30 Hz) so its reflections have no parallax; the ISM pans each bounce
  as a point source through the listener-relative panner, per block. That is the engine's
  own thesis applied to reflections. Cost: O(N) in sources vs the bed's O(1) — opt in on
  the few that matter.
- **FDN** (`fdn.c`) for the late tail — deterministic, infinite, designable.

That configuration never creates the Steam reflection bed. A **no-SDK build is fully
viable** (ISM + FDN + manual occlusion); what it loses is automatic occlusion, pathing,
and the real HRTF monitor — the last being a *developer-workstation* dependency, since
the production array render never uses HRTF.

## Docs style

**US English spelling, everywhere:** `docs/*.md`, headers, and code comments alike.
`center`/`color`/`behavior`/`optimize`/`meter`/`license`, not `centre`/`colour`/
`behaviour`/`optimise`/`metre`/`licence`. Catches the usual `-ise`/`-isation` family
(`normalize`, not `normalise`) too.

**Prose voice, `docs/*.md` and `README.md` only:** short, blunt prose (the
floooh/sokol voice: direct statements, second person, no throat-clearing), following
the Google developer documentation style guide beyond that. No em dashes, `e.g.`,
`i.e.`, or `&`; spell out `for example`, `that is`, `and` (a hyphen or a rewritten
sentence replaces the em dash). `vs` stays fine in an A/B-style label (`DBAP vs
SPCAP`), just not in flowing prose. Code comments keep their own punctuation and are
exempt from the voice rule.

CLAUDE.md itself is not user-facing (see "Repo layout" note on `docs/api.md` owning
the manual) and is exempt from the voice rule too; it predates it and mixes styles
freely. It still follows the US English rule above.

## Doc index

- `docs/architecture.md` — system overview, the bus seam, the full signal-flow diagram
  (every signal kind, source → device, with tap-order rationale), locked decisions + rationale.
  `docs/signal-flow.md` is the same diagram as a rendered Mermaid graph (ASCII is canonical).
- `docs/concurrency.md` — threading model, SPSC rings, commit snapshot, lifetimes. **Most load-bearing.**
- `docs/api.md` — C ABI reference and per-call threading semantics.
- `docs/spatialization.md` — DBAP, moving observer, binaural decode (3rd-order), speaker alignment.
- `docs/materials.md` — material/geometry model → Steam Audio occlusion + reflections → the bus.
- `docs/integration.md` — Unity + Godot bindings + the per-engine coordinate seams; Unreal notes.
- `docs/build.md` — platform, dependencies, licensing, Dante config.
- `docs/profiling.md` — Tracy instrumentation (`BWA_TRACY`), the headless benches (`profile_bench`/`bench_situations`), real-time scheduling + memory notes.
- `docs/layout-schema.md` — `cave_layout.json` format: speaker geometry, per-speaker gain/delay, DBAP knobs.
- `docs/calibration.md` — `bwa_calibrate`: acoustic position survey, delay/gain trims, room report → `cave_layout.json`.
- `docs/validation.md` — `bwa_validate`: render a phantom, measure where it landed. The Zylia
  intensity/integrity/SRP/comb-depth estimators, the solve-position-versus-mic-position seam, the
  SPCAP focus sweep (and where it has power), simulate versus hardware, and what the measurements
  have said so far.
- `docs/hardware-validation.md` — the rig-day runbook: staged on-hardware checks (device → wiring → calibration → Motive → end-to-end → by-ear) with pass criteria.
- `docs/internal-types.md` — internal structs (`Voice`/`Sound`/`Layout`/`Listener`) + helper signatures. **Not ABI.**
- `docs/glossary.md` — the domain vocabulary across all of the above (panners + knobs, rE/rV metrics,
  ambisonic decoders, spread/decorrelation, acoustics paths, binaural, calibration + validation
  coinages), each entry short, formula-cited to `file:line`, and linked to the doc that owns it.
  Threading vocabulary is deliberately excluded (concurrency.md owns it).
