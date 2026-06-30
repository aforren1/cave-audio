# Speaker calibration & room characterization

How the 26-speaker array is surveyed, trimmed, and characterized at install time, and how those
numbers reach the engine. The tool is `bw_calibrate` (`examples/calibrate.cpp`, opt-in
`-DBWAUDIO_BUILD_CALIBRATE=ON`); the measurement DSP is `measure.c`, the solve + JSON writeback is
`calib.c`. All of it is unit-tested off-hardware (`bw_measure_test`, `bw_calib_test`); the ASIO
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
- **`--save-irs prefix`** — dump the per-speaker impulse responses (the deconvolved kernels). One
  capture session therefore serves trims, the room report, AND a future **headphone room simulator**:
  convolving these IRs into the binaural monitor previews the installed sound while developing
  off-site. For the *spatial* version, do a second capture pass with in-ear/dummy-head mics → those
  IRs are BRIRs (direction + room); the omni pass gives timbre/reverb only.

## What feeds the engine

`cave_layout.json` carries per-speaker `position`, `gain_db`, `delay_ms` (consumed by `dbap.c` +
`align.c`). The room itself, if you want simulated reverb, is modeled in Steam Audio as geometry +
material absorption (`steam_reflect.c`) tuned to creative intent — **not** to the measured RT60.
