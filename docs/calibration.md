# Speaker calibration & room characterization

How the 26-speaker array is surveyed, trimmed, and characterized at install time, and how those
numbers reach the engine. The tool is `bw_calibrate` (`examples/calibrate.cpp`, opt-in
`-DBWAUDIO_BUILD_CALIBRATE=ON`); the measurement DSP is `measure.c`, the solve + JSON writeback is
`calib.c`. All of it is unit-tested off-hardware (`test_measure`, `test_calib`); the ASIO
full-duplex capture is the only part that needs the rig.

## The two layout tools

- **`bw_layout_tool`** — *authoring*: place the speakers + identify which channel drives which
  physical box (by ear, via the channel test signal) → writes positions + the channel map.
- **`bw_calibrate`** — *survey + tuning*: measures positions acoustically, and the per-speaker
  delay/gain trims, into the same `cave_layout.json`.

Bring-up order: `layout_tool` (channel map) → `calibrate --localize` (positions) →
`calibrate` (trims) → `calibrate --room` (sanity-check the room). Then the engine loads the layout.

## How it measures (exponential sweep + deconvolution)

Each speaker plays a Farina exponential sine sweep; an omni mic records it; `measure_response`
recovers that speaker's impulse response by regularized deconvolution and reads the **delay**
(direct-path arrival = system latency + time of flight) and **level** (broadband sensitivity). One
sample at 48 kHz = 7 mm, and sub-sample peak interpolation gets well below that, so the limit is the
mic-position accuracy, not the acoustics.

Use an **omnidirectional** measurement mic: it's flat and direction-independent, so each speaker's
delay/level/response comes back uncolored — and measuring *through* the acoustically-transparent
screens is correct, since that's what the listener hears (the screen's slight HF loss is captured in
the trim automatically).

## Modes

- **`--localize positions.txt`** — acoustic self-survey. Capture every speaker at K ≥ 5 known mic
  positions; `calib_trilaterate` solves each speaker's 3D position *and* the unknown constant system
  latency jointly (linear least squares). This **sees speakers optical trackers can't** — the sweep
  passes through the screens. Clever pairing with the OptiTrack you already have for head tracking:
  put a marker on the *mic* (open line-of-sight) so OptiTrack hands you the known mic positions, and
  let acoustics locate the screen-hidden speakers. Spread the mic positions out and make them
  non-coplanar (GDOP) — clustered points amplify error. Cross-check against the install drawings.
- **default** — trims. `calib_solve` turns the per-speaker measurements into `delay_ms` (arrival-align
  every speaker to the farthest) and `gain_db` (equalize sensitivity, with the speaker→mic distance
  divided out so it corrects the *speaker*, not distance; cut-only so nothing clips).
- **`--room`** — RT60 (Schroeder) + early reflections from the captured IRs. **A treatment
  diagnostic, not a model to match.** Matching the engine's reverb to the room would *double-count*
  (the engine renders the virtual room AND the real room adds its own). What it tells you: how live
  the room is — the **floor** on what you can render, since you can't reproduce a space deader than
  your room — and which nearby surfaces throw early reflections that smear localization (treat those).
  You cannot DSP the room away for a moving listener (reverb is non-invertible, position-dependent),
  so the lever is physical treatment. The binaural monitor (headphones, room-free) is the clean
  reference; comparing array-vs-monitor measures how much the room is adding.
- **`--check`** — drift detector. One fast pass from the mic position; `calib_check_drift` compares
  each speaker's measured distance to its stored position (removing the common latency as the median
  residual, so it's robust to a few moved speakers) and flags anything beyond ~20 mm. Catches a bumped
  speaker in seconds. Radial only (a purely tangential move doesn't change the distance) — re-run
  `--localize` for a full re-survey. Exit code 3 if anything is flagged (scriptable).
- **`--live N`** — positioning aid: repeatedly measures speaker N's distance from the mic while you
  move it (`--latency m`, from a prior `--localize`, turns the reading into absolute distance + delta
  from the layout target; press a key to stop). One omni mic gives **distance**, not full 3D — for
  live 3D you'd need ≥4 fixed mics. Sub-sample peak interpolation puts the reading at well under 1 mm.
- **`--save-irs prefix`** — dump the per-speaker impulse responses (the deconvolved kernels). One
  capture session therefore serves trims, the room report, AND a future **headphone room simulator**:
  convolving these IRs into the binaural monitor previews the installed sound while developing
  off-site. For the *spatial* version, do a second capture pass with in-ear/dummy-head mics → those
  IRs are BRIRs (direction + room); the omni pass gives timbre/reverb only.

- **`--eq`** — per-speaker correction filters (the "inverse EQ" a system like Zylia SMS computes),
  written into each speaker's `eq` array. For every speaker it gates the measured IR to the **direct
  sound** (a window ending before the first reflection `--room` finds, or a default ~4 ms) and inverts
  that magnitude into a minimum-phase FIR (`measure_correction` → `calib_eq`), so the filter flattens
  the **speaker's own** response. It deliberately does NOT correct the room: room response is
  position-dependent across the ~3×3 m listening area, so a single-point room EQ over-fits one spot and
  makes the others worse — the same double-counting trap as matching RT60. The inversion is regularized
  (deep nulls aren't fought) and centred on the in-band geometric mean (the scalar `gain_db` trim still
  owns overall level). The engine applies it as a per-speaker FIR stage in `align.c`, before the
  gain+delay. With the Zylia you can gate by *direction* (keep the speaker's DOA, reject off-axis
  reflections) for a cleaner near-free-field correction than an omni gate; that's a follow-on.

## Zylia ZM-1: full 3D from one placement

`--localize` needs the omni mic at ≥5 spots because one omni gives only **distance**. The ZM-1 is 19
capsules on a rigid ~10 cm sphere, so a single placement already records each sweep arriving at 19
slightly-different times — and the arrival-time **differences** across the sphere are a
**direction**. So a speaker's full position falls out of ONE Zylia placement: direction × distance +
the array centre. This is the `--live` note's "for live 3D you'd need ≥4 fixed mics", delivered.

The solve (`zylia.c`, `zylia_doa` / `zylia_localize`, unit-tested off-hardware in the `zylia` test by
synthesizing the 19 arrivals from a known position and recovering it to machine precision):

- **Direction** — least-squares fit of the 19 arrivals to the far-field model `τ_i = A − (R/c)(dir_i·d)`,
  then a Gauss-Newton refine against the exact spherical wavefront. Latency-independent (uses the
  differences), so it's precise — sub-degree given measure.c's sub-sample IR peak. This is the
  headline: *where is every speaker, from one spot*, including the screen-hidden ones.
- **Distance** — `c·(arrival − latency)`. The array is too small for the wavefront curvature across it
  to self-calibrate the latency at metres (sub-mm of differential delay), so feed a loopback-measured
  or `--localize`-recovered latency; then the distance is as good as that latency (~7 mm per sample).
  Fuse with the omni `--localize` when you want both the one-shot directions and a sub-mm distance.

**Spatial room capture** rides the same 19-channel sweep with no new DSP: `--room` already finds each
early reflection's *time* (`er_delay`); window the 19-ch IR around each one and run `zylia_doa` on
those arrivals to get each reflection's *direction*. That turns the room report from "how live" into
"the first slap comes off the **left wall** at 6 ms" — i.e. *which* surface to treat, not just how
much. (A full ambisonic room IR is the same capture encoded to higher order; the directional
early-reflection map is the actionable part.)

Integration is a rig-bound shell (like the ASIO capture): the ZM-1 presents as a 19-ch input, so a
`calibrate --zylia` mode captures the sweep, deconvolves per channel (measure.c), feeds the 19
arrivals to `zylia_localize`, and writes `cave_layout.json`. `--zylia --simulate` synthesizes the 19
arrivals from the layout and recovers every speaker position exactly, so the math + writeback are
validated off-hardware; the two-device ASIO capture is the only unbuilt piece. One caveat baked into
the code: the capsule geometry in `zylia_geometry` is a **placeholder** spread — drop in the ZM-1
datasheet/surveyed capsule directions before trusting on-hardware DOA (the math is geometry-agnostic;
only that table must match the real array).

**Bring-up probe.** Before any of that, `bw_zylia_probe` (built with `-DBWAUDIO_BUILD_CALIBRATE=ON`)
is the "is it talking?" check you can run the moment the ZM-1 is plugged in — input-only, no rig
needed. `bw_zylia_probe --list` enumerates the ASIO drivers + their channel counts (look for the
Zylia driver, or ASIO4ALL over its USB-audio interface, showing `in=19`). It auto-picks a driver whose
name contains "zylia", else `--driver` it. Two views:

- `--console`: a live per-channel RMS meter — confirm all 19 capsules stream, and learn which channel
  is which capsule by tapping each and watching its channel jump. A channel stuck at digital silence
  is dead or unmapped.
- **Live DOA view** (default when built with `-DBWAUDIO_BUILD_PLAYGROUND=ON` too, which supplies
  raylib): the audio callback watches for a transient; CLAP anywhere around the array and a dot
  appears on the capsule sphere where the clap came from (`zylia_tdoa`: onset + windowed
  cross-correlation against the strongest capsule with sub-sample parabolic peaks → `zylia_doa`).
  This verifies the capsule MAPPING and the GEOMETRY table in one gesture — swapped channels or a
  wrong geometry row put the dot somewhere absurd, and you find out in seconds instead of during a
  calibration session. `--simulate` runs the identical snapshot→tdoa→doa→draw pipeline on
  synthesized claps from a walking direction (drawn as a truth ring the dot must land in) — the
  hardware-free check of everything but the ASIO capture; the math itself is unit-tested in the
  `zylia` ctest.

## What feeds the engine

`cave_layout.json` carries per-speaker `position`, `gain_db`, `delay_ms` (consumed by `dbap.c` +
`align.c`). The room itself, if you want simulated reverb, is modeled in Steam Audio as geometry +
material absorption (`steam_reflect.c`) tuned to creative intent — **not** to the measured RT60.
