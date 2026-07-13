# Speaker calibration & room characterization

How the speaker array (26 in the CAVE) is surveyed, trimmed, and characterized at install time, and
how those numbers reach the engine. The tool is `bw_calibrate` (`examples/calibrate.cpp`, opt-in
`-DBWAUDIO_BUILD_CALIBRATE=ON`). The measurement DSP is `measure.c`; the solve + JSON writeback is
`calib.c`. All of it is unit-tested off-hardware (`test_measure`, `test_calib`); the ASIO
full-duplex capture is the only part that needs the rig.

**Everything here follows the layout's speaker count** (`n`, 4..26 — see
[`layout-schema.md`](./layout-schema.md)), not a hard-wired 26: the capture opens `n` ASIO outputs
plus the mic input (which rides buffer slot `n`), sweeps those `n` speakers, and writes `n` records
back. `bw_calib_view` likewise sizes its plots from each loaded layout, and refuses to Diff two
layouts with different speaker counts rather than mis-compare them.

## The two layout tools

- **`bw_layout_tool`** — *authoring*: place the speakers + identify which channel drives which
  physical box (by ear, via the channel test signal) → writes positions + the channel map.
- **`bw_calibrate`** — *survey + tuning*: measures positions acoustically, and the per-speaker
  delay/gain trims, into the same `cave_layout.json`.

Bring-up order: `layout_tool` (channel map) → `calibrate --localize` (positions) →
`calibrate` (trims) → `calibrate --room` (sanity-check the room). Then the engine loads the layout.

## How it measures (exponential sweep + deconvolution)

Each speaker plays a Farina exponential sine sweep; an omni mic records it. `measure_response`
recovers that speaker's impulse response by regularized deconvolution and reads two numbers: the
**delay** (direct-path arrival = system latency + time of flight) and the **level** (broadband
sensitivity). One sample at 48 kHz = 7 mm, and sub-sample peak interpolation gets well below that,
so the limit is the mic-position accuracy, not the acoustics.

Use an **omnidirectional** measurement mic. It's flat and direction-independent, so each speaker's
delay/level/response comes back uncolored. Measuring *through* the acoustically-transparent screens
is correct — that's what the listener hears, and the screen's slight HF loss is captured in the trim
automatically.

## Modes

- **`--localize positions.txt`** — acoustic self-survey. Capture every speaker at K ≥ 5 known mic
  positions; `calib_trilaterate` solves each speaker's 3D position *and* the unknown constant system
  latency jointly (linear least squares). This **sees speakers optical trackers can't** — the sweep
  passes through the screens. Pair it with the OptiTrack you already have for head tracking: put a
  marker on the *mic* (open line-of-sight) so OptiTrack hands you the known mic positions, and let
  acoustics locate the screen-hidden speakers. Spread the mic positions out and make them
  non-coplanar (GDOP) — clustered points amplify error. Cross-check against the install drawings.
- **default** — trims. `calib_solve` turns the per-speaker measurements into `delay_ms`
  (arrival-align every speaker to the farthest) and `gain_db` (equalize sensitivity, with the
  speaker→mic distance divided out so it corrects the *speaker*, not distance; cut-only so nothing
  clips).
- **`--room`** — RT60 (Schroeder) + early reflections from the captured IRs. It measures **how live
  your room is**. **Do not copy the measured RT60 into the engine's reverb settings.** The room's
  own decay is a **floor**: you cannot render a space deader than the room you're in, and nearby
  surfaces that throw early reflections smear localization. If the room is too live, **treat it
  physically** — you cannot DSP reverb away for a moving listener (it is non-invertible and
  position-dependent). The binaural monitor (headphones, room-free) is the clean reference;
  comparing array-vs-monitor measures how much the room is adding.
- **`--check`** — drift detector. One fast pass from the mic position; `calib_check_drift` compares
  each speaker's measured distance to its stored position (removing the common latency as the median
  residual, so it's robust to a few moved speakers) and flags anything beyond ~20 mm. Catches a
  bumped speaker in seconds. Radial only — a purely tangential move doesn't change the distance —
  so re-run `--localize` for a full re-survey. Exit code 3 if anything is flagged (scriptable).
- **`--live N`** — positioning aid: repeatedly measures speaker N's distance from the mic while you
  move it (`--latency m`, from a prior `--localize`, turns the reading into absolute distance + delta
  from the layout target; press a key to stop). One omni mic gives **distance**, not full 3D — for
  live 3D you'd need ≥4 fixed mics. Sub-sample peak interpolation puts the reading at well under 1 mm.
- **`--save-irs prefix`** — dump the per-speaker impulse responses (the deconvolved kernels). One
  capture session therefore serves trims, the room report, AND a future **headphone room simulator**:
  convolving these IRs into the binaural monitor previews the installed sound while developing
  off-site. For the *spatial* version, do a second capture pass with in-ear/dummy-head mics — those
  IRs are BRIRs (direction + room); the omni pass gives timbre/reverb only.

- **`--eq`** — per-speaker correction filters (the "inverse EQ" a system like Zylia SMS computes),
  written into each speaker's `eq` array. For every speaker it gates the measured IR to the **direct
  sound** (a window ending before the first reflection `--room` finds, or a default ~4 ms) and
  inverts that magnitude into a minimum-phase FIR (`measure_correction` → `calib_eq`). The filter
  flattens the **speaker's own** response.

  It does NOT correct the room. Room response is position-dependent across the ~3×3 m listening
  area, so a single-point room EQ over-fits one spot and makes the others worse. The inversion is
  regularized (deep nulls aren't fought) and centred on the in-band geometric mean (the scalar
  `gain_db` trim still owns overall level). The engine applies it as a per-speaker FIR stage in
  `align.c`, before the gain+delay.

  With the Zylia you can gate by *direction* (keep the speaker's DOA, reject off-axis reflections)
  for a cleaner near-free-field correction than an omni gate; that's a follow-on.

- **`--room-eq`** — room correction **at the mic position**, for a **static listener only** (the
  fixed-observer SPCAP/VBAP deployments: one seat, one sweet spot — put the mic there, at ear
  height). A roaming listener keeps plain `--eq`; the one-point objection above applies in full.
  Two halves, split at 200 Hz so nothing is corrected twice:
  - **200 Hz and up** — the `eq` FIR is designed from a **frequency-dependent window**
    (`measure_correction_room`): each frequency's magnitude comes from the IR windowed to ~6 cycles,
    clamped between the direct-sound gate and 400 ms. At HF the window shrinks to the gate (identical
    to `--eq`, and the ~1/6-octave resolution is the broad-stroke smoothing that survives head sway);
    toward LF it grows to include the room. Boosts are capped at **+3 dB** — a seated head still
    sways a few cm, and interference dips move with it, so dips are never fought hard.
  - **30–200 Hz** — discrete **modal cuts** (`measure_room_cuts` → each speaker's `room_eq` array of
    `{fc, gain_db, q}` peaking sections, rendered as biquads in `align.c`). Below the room's Schroeder
    frequency modes are approximately minimum-phase, so a magnitude cut also fixes the ringing, and
    the correction stays valid over meter-scale distances. **Cut-only** by design and by schema
    (`gain_db <= 0` — the loader rejects boosts): peaks are modal energy you can remove; dips are
    position-dependent cancellations you cannot fill.

  What no EQ fixes: **decay**. A ringing room still smears transients once its steady-state
  coloration is flattened. That stays a treatment problem — see `--room` above.

  The wrong-file mistake fails loudly: **`bw_start` refuses** a layout carrying `room_eq` sections
  when the session renders a moving listener (the DBAP panner and/or `track_internal`) — see
  `bw_last_error`. Load the roaming variant, recalibrate with `--room-eq-grid`, or run SPCAP/VBAP
  with a fixed pose.

- **`--room-eq-grid`** — **tracked room EQ**: the moving-listener answer to `--room-eq`'s modal
  half (Lindfors/Liski/Välimäki, JAES 2022, adapted to the tracked CAVE). One point can't room-EQ a
  roaming listener; a **grid of points can**. Below ~200 Hz the room's mode *frequencies* are fixed
  properties of the room — only how strongly each mode reads varies with position, and that varies
  smoothly on the half-metre scale of LF wavelengths.

  Workflow: run once per mic placement — `--mic x y z` **is the grid key** (a rerun within 5 cm
  replaces that entry; up to 16 positions, `BW_RQ_GRID_MAX`). Cover the working area at ear height,
  ~0.5–1 m spacing. Each run measures this position's modal cuts (`measure_room_cuts`, same
  30–200 Hz band and 12 dB depth cap as `--room-eq`) and **merges** them into the layout's
  top-level `room_eq_grid` (`calib_room_grid_merge` → `calib_write_room_eq_grid`): per-position fcs
  within ~8% are the same mode, so each speaker gets ONE shared `fc`/`q` ladder with per-position
  depths (0 dB where a position didn't see the mode). See
  [`layout-schema.md`](./layout-schema.md) for the format.

  At runtime the engine interpolates the depths at the **live listener position** every block
  (inverse-distance weights over the grid points) and the align biquads glide toward them at
  24 dB/s — click-free by construction, fast enough to track a walk. `bw_set_tracked_room_eq` is
  the live kill switch (off glides to flat) for A/B on the rig. Works with every panner; `bw_start`
  has no objection to a grid in a moving session — that's the point.

  **Only** the 30–200 Hz modal band is tracked: the mid/HF room response decorrelates over
  centimetres, far too fast to interpolate between half-metre grid points. The `eq` FIR above stays
  the direct-sound speaker correction for moving installs. `room_eq` and `room_eq_grid` are mutually
  exclusive in one layout file (the loader rejects both together; the grid writeback removes a stale
  static `room_eq` for you).

## Zylia ZM-1: full 3D from one placement

`--localize` needs the omni mic at ≥5 spots because one omni gives only **distance**. The ZM-1 is 19
capsules on a rigid ~10 cm sphere, so a single placement already records each sweep arriving at 19
slightly-different times — and the arrival-time **differences** across the sphere are a
**direction**. A speaker's full position falls out of ONE Zylia placement: direction × distance +
the array centre.

The solve (`zylia.c`, `zylia_doa` / `zylia_localize`, unit-tested off-hardware in the `zylia` test by
synthesizing the 19 arrivals from a known position and recovering it to machine precision):

- **Direction** — least-squares fit of the 19 arrivals to the far-field model `τ_i = A − (R/c)(dir_i·d)`,
  then a Gauss-Newton refine against the exact spherical wavefront. Latency-independent (uses the
  differences), so it's precise — sub-degree given measure.c's sub-sample IR peak, and it finds the
  screen-hidden speakers too.
- **Distance** — `c·(arrival − latency)`. The array is too small for the wavefront curvature across
  it to self-calibrate the latency at metres (sub-mm of differential delay), so feed a
  loopback-measured or `--localize`-recovered latency; then the distance is as good as that latency
  (~7 mm per sample). Fuse with the omni `--localize` when you want both the one-shot directions and
  a sub-mm distance.

**Spatial room capture** *(design — not implemented; nothing consumes `er_delay` directionally yet)*
rides the same 19-channel sweep with no new DSP. `--room` already finds each
early reflection's *time* (`er_delay`); window the 19-ch IR around each one and run `zylia_doa` on
those arrivals to get each reflection's *direction*. That turns the room report from "how live" into
"the first slap comes off the **left wall** at 6 ms" — i.e. *which* surface to treat, not just how
much. (A full ambisonic room IR is the same capture encoded to higher order; the directional
early-reflection map is the actionable part.)

Integration is a rig-bound shell (like the ASIO capture). The ZM-1 presents as a 19-ch input, so a
`calibrate --zylia` mode captures the sweep, deconvolves per channel (measure.c), feeds the 19
arrivals to `zylia_localize`, and writes `cave_layout.json`. `--zylia --simulate` synthesizes the 19
arrivals from the layout and recovers every speaker position exactly, so the math + writeback are
validated off-hardware; the sweep capture is the unbuilt piece — see "Getting the ZM-1 onto Dante".

### Getting the ZM-1 onto Dante

The obstacle to `--zylia` on hardware was never the DSP — it was that the sweep plays out of the **DVS**
device and the ZM-1 is a **different** USB device, while the ASIO SDK has one process-wide current-driver
slot (which is why `asio_session.cpp` exists). Two drivers at once is not a flag, it's a rewrite.

**Put the ZM-1 on the Dante network and the problem dissolves.** Dante Via captures the ZM-1's ASIO
stream and transmits its 19 capsules as Dante channels; DVS on the engine machine receives them as 19
**inputs on the same ASIO device that already owns the 26 outputs**. One driver, one clock domain,
sample-locked — the two-device problem simply stops existing, and `calib_capture.cpp` goes from
"open N outs + 1 mic input" to "open N outs + 19 inputs", which is a parameter, not an architecture.
([Danowski's write-up](https://blog.przemekdanowski.com/connecting-zylia-zm-1-to-dante-network/) is the
recipe; DVS + Via is ~$60 with a 30-day trial.)

What it buys, beyond unblocking the sweep:

- **Absolute latency, so distance becomes real.** Right now `zylia_localize`'s range is only as good as
  the latency you feed it, and the array is far too small to self-calibrate that (below). Sample-locked
  playback and capture make the round-trip latency a measurable constant — so the ZM-1 delivers full 3D
  **positions** from one placement, not just directions.
- **Sweeps instead of claps** for the capsule survey: enormously better SNR, sub-sample arrival times
  straight off `measure.c`'s deconvolved IR peak, and 26 known source directions for free.
- **Spatial room capture** (above) becomes buildable at all — it needs the 19-channel IR.

The cable run is a side benefit worth naming: the ZM-1 wants to sit in the *middle* of the CAVE, and USB
will not reach. 100 m of Cat6 will.

Caveats to check before buying: Via adds a constant 10 ms (irrelevant — it is measured, constant, and
nowhere near the production path), and DVS's channel budget is **64 I/O at 48 kHz**, so 26 out + 19 in =
45 fits comfortably — but DVS typically **halves its channel count at 96 kHz**, where 45 would not fit.
Calibrate at 48 kHz.

Note that none of this is required to *start*. The capsule survey below runs on claps through the capture
shell that already exists, and its solver is the same one the sweep path will use — `zylia_survey` takes
(source position, 19 arrival times) and does not care whether those arrivals came from a clap's
cross-correlation or a sweep's deconvolved IR peak. Dante is the precision upgrade, not the prerequisite.

### The capsule geometry

`zylia_geometry` is the real array: the ZM-1's 19 capsules are the 20 vertices of a **regular
dodecahedron, vertex-up, minus the nadir vertex**. Zylia doesn't put the coordinates in the spec
sheet, but they publish the node table and [ambitools](https://www.sekisushai.net/ambitools/docs/grids.html)
reproduces it. It self-checks: elevation rings come out 1 / 3 / 6 / 6 / 3 (the missing 20th vertex is
the one at −90°), each ring is its opposite ring rotated 180°, and the elevations are exactly
`asin(√5/3) = 48.1897°` and `asin(1/3) = 19.4712°` with azimuths generated by `atan(√(3/5)) =
37.7612°`. We build it from those closed forms, so nothing is rounded. The `zylia` test pins the
structure — ring populations, the unpaired zenith, the sum-to-zenith identity, the 41.81°
dodecahedral edge as the closest pair.

The radius (49 mm) only feeds `zylia_localize`'s near-field solve. `zylia_doa` normalizes the fitted
gradient, so the radius **cancels out of the direction entirely** — 49 vs 50 mm changes nothing.

**Two things the table cannot give you**, and no off-hardware test can catch either, because both
survive every structural check above:

- **Channel order** — node *i* here is not necessarily ASIO input *i*. A permutation still yields a
  confident direction, just the wrong one. `bw_zylia_probe` resolves it: tap a capsule, see which
  channel jumps.
- **Azimuth reference** — nothing published says which capsule faces the device's front, so an unknown
  yaw offset rotates every DOA by a constant. Clap from a known direction in the Zylia tab; the
  discrepancy *is* the offset.

Pin both at the rig — or skip the question entirely and **measure the geometry** (below), which hands
you the order and the orientation as a side effect.

### The capsule self-survey (`zylia_survey`)

Rather than trust a table and then hand-pin the two things it can't tell you, **measure the array**.
The capsules come back indexed by *the ASIO channel that fed them*, expressed in *room axes* — so the
result **is** the channel order and **is** the mounted orientation. Nothing is left to pin.

Sound from a known direction `d_k` reaches capsule `i` at `τ[k][i] = t0_k − (1/c)·m_i·d_k`. The `t0_k` is
unknown and unknowable — system latency, the moment the clap happened, whichever channel `zylia_tdoa`
picked as its reference — and it *does not matter*: it is one constant per observation, so subtracting
each observation's mean across the 19 capsules kills it exactly. What's left is linear in `m_i` and
**separates**: each capsule gets its own 3-unknown least squares, all sharing one 3×3 normal matrix.

Three consequences worth drawing out:

- **No sweep, no sample-sync, no second audio device.** Claps, through the capture shell that already
  exists. This is why the survey is available *today*, ahead of any Dante work.
- **A plane wave is not quite the truth.** A clap 2.5 m out is a sphere, and its curvature across a
  49 mm array is a systematic ~1.4 µs — 2–3 mm of capsule error if ignored. So the solver takes the
  source's *position*, not merely its direction (you know where you clapped — that's how you knew the
  direction), and iterates the exact-minus-plane-wave correction on top of the linear seed. The
  geometry lands well under a millimetre.
- **Where is the origin?** Arrival times fix the capsule cloud's *shape* but not its *position* —
  translating the whole cloud is absorbed by the per-observation constants. So the origin must be
  chosen, and the obvious choice is wrong: the ZM-1's capsule set is a dodecahedron *missing its nadir*,
  so it is not centroid-balanced and its centroid sits **R/19 = 2.6 mm above the sphere centre**. Nobody
  tape-measures to the centroid. The solver therefore fits the sphere the capsules lie on and re-centres
  on that, giving the physical centre of the shell — the point the operator measured the clap positions
  from, and what `zylia_localize`'s `center` means.

**Running it** (calib_view's Zylia tab → *Capsule survey*): tape-measure where the ZM-1 is, set the clap
position, clap. Repeat from ≥ 6 spots. If layout A is loaded you can pick "clap at speaker *N*" and the
position autofills from the surveyed geometry — stand at a speaker, clap, move on. Then **Solve** →
**Install** → **Save**. `--tests zylia` drives the whole flow on synthetic claps.

**Spread is the trap.** Claps in a horizontal ring around the array are *coplanar*: the normal matrix
goes singular in the vertical and the capsules' **heights** are unrecoverable. A solver that shrugged
would hand back a flattened array and a confident wrong answer, so this one refuses — `spread` is 1 for
isotropic directions, 0 for coplanar, and below 0.05 it declines to solve and tells you why. **Clap high
and low, not just around.**

**Reading the result.** `residual` is the number that says whether to believe it: what the recovered
geometry *fails* to explain, in microseconds. Sub-microsecond is clean. Tens of microseconds means bad
claps, a wrong array centre, or a clap that wasn't where you said it was. `radius` should land near
49 mm — if it doesn't, something is badly wrong upstream. Save writes JSON that encodes the geometry,
the channel order *and* the orientation together, so it is specific to **one ZM-1 on one mount**:
re-survey if either changes.

**Bring-up.** Before any of that, run the "is it talking?" checks the moment the ZM-1 is plugged in —
input-only, no rig needed. Both ride the same capture shell (`zylia_capture.cpp`: driver open,
transient trigger, snapshot publish):

- `bw_zylia_probe` (built with `-DBWAUDIO_BUILD_CALIBRATE=ON`): console meter. `--list` enumerates
  the ASIO drivers + channel counts (look for the Zylia driver, or ASIO4ALL over its USB-audio
  interface, showing `in=19`; it auto-picks a name containing "zylia", else `--driver` it). Tap a
  capsule and watch its channel jump; a channel stuck at digital silence is dead or unmapped.
- **Live DOA view** — `bw_calib_view`'s **Zylia tab** (built with `-DBWAUDIO_BUILD_CALIBVIEW=ON`;
  live capture needs the ASIO SDK too): CLAP anywhere around the array and a dot appears on the
  capsule sphere where the clap came from (`zylia_tdoa`: onset + windowed cross-correlation against
  the strongest capsule with sub-sample parabolic peaks → `zylia_doa`). This verifies the capsule
  MAPPING and the GEOMETRY table in one gesture — swapped channels or a wrong geometry row put the
  dot somewhere absurd. Its simulate mode runs the identical snapshot→tdoa→doa→draw pipeline on
  synthesized claps (truth marker drawn; "Clap now" for a deterministic one) — the hardware-free
  check of everything but the ASIO capture. The math is unit-tested in the `zylia` ctest, and the
  whole tab is driven by the `zylia/sim_doa` UI test in `calib_view`.

## Reviewing the results (`bw_calib_view`)

Before trusting a calibration run, LOOK at it. `bw_calib_view` (opt-in `-DBWAUDIO_BUILD_CALIBVIEW=ON`;
Dear ImGui + ImPlot/ImPlot3D on win32+d3d11) loads layouts through the engine's own loader and shows

- the **array in 3D** with per-speaker index labels (the wiring sanity check),
- **gain/delay trims** as bar charts,
- each speaker's **correction-EQ magnitude** (`--eq` filters, 20 Hz–20 kHz),
- the retained **IR kernels** (`--save-irs <prefix>` → `<prefix>_NN.wav`) with the direct-arrival tag,
- and — the main event — a **layout diff**: `bw_calib_view before.json after.json` tables Δposition /
  Δgain / Δdelay / eq-taps per speaker with outliers highlighted, so a swapped channel, a bad mic
  placement, or a bogus `--localize` solve is one glance, not an evening.

It is the **calibration station** — one window for the rig session. The **Capture tab** runs the
calibration itself: sweep every speaker, solve the trims, write the layout — on a worker thread with
live per-speaker progress, through the same capture backends (`calib_capture.cpp`) and measure/solve
DSP as the CLI (simulate hardware-free; ASIO full-duplex at the rig, mic position + input channel +
room/eq/IR options in-window). One button loads the result into Diff (A = input, B = what calibration
wrote) for review before you accept it. The **Zylia tab** (live clap-DOA, see "Bring-up" above)
covers the ZM-1.

`bw_calibrate` remains the headless CLI over the same code — scriptable, and the only place for the
multi-placement modes (`--localize`, `--zylia`, `--check`, `--live`).
`bw_calib_view --tests [filter]` runs its imgui_test_engine suite (fake inputs drive the actual UI
against generated fixture layouts + synthesized claps, screenshots land in `output/captures/`) and
is wired into ctest as `calib_view`.

## What feeds the engine

`cave_layout.json` carries per-speaker `position`, `gain_db`, `delay_ms` (consumed by `dbap.c` +
`align.c`). The room itself, if you want simulated reverb, is modeled in Steam Audio as geometry +
material absorption (`steam_reflect.c`) tuned to creative intent — **not** to the measured RT60.
