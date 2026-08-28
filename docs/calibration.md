# Speaker calibration and room characterization

How the speaker array (26 in the CAVE) is surveyed, trimmed, and characterized at install time, and
how those numbers reach the engine. The tool is `bwa_calibrate` (`examples/calibrate.cpp`, opt-in
`-DBWA_BUILD_CALIBRATE=ON`). The measurement DSP is `measure.c`; the solve + JSON writeback is
`calib.c`. All of it is unit-tested off-hardware (`test_measure`, `test_calib`). The ASIO
full-duplex capture is the only part that needs the rig. Terms used here without definition are in
[glossary.md](./glossary.md).

**Everything here follows the layout's speaker count** (`n`, 4..26; see
[`layout-schema.md`](./layout-schema.md)), not a hard-wired 26. The capture opens `n` ASIO outputs
plus the mic input (which rides buffer slot `n`, or, with `--zylia`, the ZM-1's 19 capsule inputs
on slots `n`..`n+18`). It sweeps those `n` speakers and writes `n` records back.
`bwa_calib_view` likewise sizes its plots from each loaded layout. It refuses to Diff two
layouts with different speaker counts rather than mis-compare them.

## The two layout tools

- **`bwa_layout_tool`**, *authoring*: place the speakers + identify which channel drives which
  physical box (by ear, via the channel test signal) → writes positions + the channel map.
- **`bwa_calibrate`**, *survey + tuning*: measures positions acoustically, and the per-speaker
  delay/gain trims, into the same `cave_layout.json`.

Bring-up order: `layout_tool` (channel map) → `calibrate --localize` (positions; or `--zylia` from
one ZM-1 placement) → `calibrate` (trims) → `calibrate --room` (sanity-check the room). Then the
engine loads the layout.

## How it measures (exponential sweep + deconvolution)

Each speaker plays a Farina exponential sine sweep; an omni mic records it. `measure_response`
recovers that speaker's impulse response by regularized deconvolution and reads two numbers: the
**delay** (direct-path arrival = system latency + time of flight) and the **level** (broadband
sensitivity). One sample at 48 kHz = 7 mm, and sub-sample peak interpolation gets well below that.
So the limit is the mic-position accuracy and the assumed speed of sound, not the acoustics. Set the
second one: see "Air temperature" below.

Use an **omnidirectional** measurement mic. It's flat and direction-independent, so each speaker's
delay/level/response comes back uncolored. Measure *through* the acoustically-transparent screens.
That's what the listener hears, and the trim captures the screen's slight HF loss automatically.

## Air temperature

Every range the survey solves is `c * delay`, and `c` moves about 0.6 m/s per degree C. A room at
15 C runs 340.4 m/s and one at 25 C runs 346.4. Assume the textbook 343.0 in a room that is not at
20 C and you bias every surveyed distance by up to 1%. At a 4 m range that is 4 cm, larger than both
the 7 mm timing resolution above and a tracker-placed mic. It is an order of magnitude above the
2 to 3 mm capsule-geometry term the ZM-1 solve already bothers to correct.

Tell the tool the room temperature:

```
bwa_calibrate --layout cave_layout.json --temp 73F   --localize positions.txt
bwa_calibrate --layout cave_layout.json --temp 22.8  ...   # bare number or a C suffix is Celsius
bwa_calibrate --layout cave_layout.json --c 345.1    ...   # if you measured c directly
```

The tool records the value into the layout as `reference.speed_of_sound_mps`, and every later run on
that file picks it up. So you pass the flag once per rig rather than remembering it every session. An
explicit flag always beats the file. `bwa_layout_tool` reads the same field for its delay
derivation, so the two tools agree on one file instead of quietly disagreeing by a temperature.

A layout with no such field falls back to 343.0. `--simulate` synthesizes its arrival times at
whichever `c` the run is using, not at a fixed 343.0. A simulated survey therefore stays
self-consistent at any temperature and recovers the geometry it started from. That matching matters:
generate at one `c` and solve at another, and every solved position inflates by their ratio,
silently. Then `--localize` writes the result back.

**This does not change what you hear.** 4 cm of speaker position error at 3 m is under half a degree
of direction error, against the 4 to 6 degrees the array carries anyway. Set the temperature so the
survey is honest as a measurement, and so it agrees with the install drawings when you cross-check
it. Do not expect it to be audible.

## Modes

- **`--localize positions.txt`**: acoustic self-survey. Capture every speaker at K ≥ 5 known mic
  positions. `calib_trilaterate` solves each speaker's 3D position *and* the unknown constant system
  latency jointly (linear least squares). This **sees speakers optical trackers can't**: the sweep
  passes through the screens. Pair it with the OptiTrack you already have for head tracking: put a
  marker on the *mic* (open line-of-sight) so OptiTrack hands you the known mic positions, and let
  acoustics locate the screen-hidden speakers. Spread the mic positions out and make them
  non-coplanar ([GDOP](./glossary.md#gdop)): clustered points amplify error. Cross-check against
  the install drawings.

  The solved latency gets a free sanity check: at open, the capture shell logs the driver's own
  `ASIOGetLatencies` numbers (out + in = the **digital** half of the loop; the Digiface reports its Dante
  buffering there). After the solve the CLI prints solved-vs-driver with the residual. The
  residual is what the driver *can't* see (DAC/ADC conversion + analog), so it must be a small
  positive number. **Negative is physically impossible** (wrong device, sample-rate mismatch).
  Tens of ms means an unexpected buffer (check the Dante latency setting). The solved value stays
  authoritative: the driver's numbers are nominal, the sweep measured reality.
- **`--zylia`**: the same self-survey from **one** mic placement, with the ZM-1's 19 capsules
  instead of five omni mic positions. Direction from arrival-time differences, distance from a known
  latency (`--latency` or `--ref`). See "Zylia ZM-1: full 3D from one placement" below.
- **default**: trims. `calib_solve` turns the per-speaker measurements into `delay_ms`
  (arrival-align every speaker to the farthest) and `gain_db` (equalize sensitivity, with the
  speaker→mic distance divided out so it corrects the *speaker*, not distance; cut-only so nothing
  clips). Those trims align the array at **one** point. The engine can optionally re-reference them
  onto the tracked listener at runtime (`bwa_set_tracked_align`, off by default), the time-alignment
  counterpart of `--room-eq-grid` below. See
  [`spatialization.md`](./spatialization.md#re-aligning-to-the-tracked-listener-bwa_set_tracked_align-off-by-default).
- **`--room`**: RT60 (Schroeder) + early reflections from the captured IRs. It measures **how live
  your room is**. **Do not copy the measured RT60 into the engine's reverb settings.** The room's
  own decay is a **floor**: you cannot render a space deader than the room you're in. Nearby
  surfaces that throw early reflections smear localization. If the room is too live, **treat it
  physically**: you cannot DSP reverb away for a moving listener (it is non-invertible and
  position-dependent). The binaural monitor (headphones, room-free) is the clean reference.
  Comparing array-vs-monitor measures how much the room is adding.
- **`--check`**: drift detector. One fast pass from the mic position. `calib_check_drift` compares
  each speaker's measured distance to its stored position (it removes the common latency as the
  median residual, so it's robust to a few moved speakers) and flags anything beyond ~20 mm. Catches a
  bumped speaker in seconds. Radial only (a purely tangential move doesn't change the distance),
  so re-run `--localize` for a full re-survey. Exit code 3 if anything is flagged (scriptable).
- **`--live N`**: positioning aid. Repeatedly measures speaker N's distance from the mic while you
  move it (`--latency m`, from a prior `--localize`, turns the reading into absolute distance + delta
  from the layout target; press a key to stop). With no `--latency` it prints the driver's digital
  loop as a starting value: a hard **lower bound** (the true latency adds DAC/ADC + analog). One
  omni mic gives **distance**, not full 3D; for live 3D you'd need ≥4 fixed mics. Sub-sample peak
  interpolation puts the reading at well under 1 mm.
- **`--save-irs prefix`**: dump the per-speaker impulse responses (the deconvolved kernels). One
  capture session therefore serves trims, the room report, AND a future **headphone room simulator**:
  convolving these IRs into the binaural monitor previews the installed sound while you work
  off-site. For the *spatial* version, do a second capture pass with in-ear/dummy-head mics: those
  IRs are BRIRs (direction + room); the omni pass gives timbre/reverb only.

- **`--eq`**: per-speaker correction filters (the "inverse EQ" a system like Zylia SMS computes),
  written into each speaker's `eq` array. For every speaker it gates the measured IR to the **direct
  sound** (a window ending before the first reflection `--room` finds, or a default ~4 ms) and
  inverts that magnitude into a minimum-phase FIR (`measure_correction` → `calib_eq`). The filter
  flattens the **speaker's own** response.

  It does NOT correct the room. Room response is position-dependent across the ~3×3 m listening
  area, so a single-point room EQ over-fits one spot and makes the others worse. The inversion is
  regularized (deep nulls aren't fought) and centered on the in-band geometric mean (the scalar
  `gain_db` trim still owns overall level). The engine applies it as a per-speaker FIR stage in
  `align.c`, before the gain+delay.

  With the Zylia you can gate by *direction* (keep the speaker's DOA, reject off-axis reflections)
  for a cleaner near-free-field correction than an omni gate; that's a follow-on.

- **`--room-eq`**: room correction **at the mic position**, for a **static listener only** (the
  fixed-observer SPCAP/VBAP deployments: one seat, one sweet spot; put the mic there, at ear
  height). A roaming listener keeps plain `--eq`; the one-point objection above applies in full.
  Two halves, split at 200 Hz so nothing is corrected twice:
  - **200 Hz and up**: `measure_correction_room` designs the `eq` FIR from a
    **frequency-dependent window**: each frequency's magnitude comes from the IR windowed to
    ~6 cycles, clamped between the direct-sound gate and 400 ms. At HF the window shrinks to the
    gate (identical to `--eq`, and the ~1/6-octave resolution is the broad-stroke smoothing that
    survives head sway); toward LF it grows to include the room. Boosts are capped at **+3 dB**: a
    seated head still sways a few cm, and interference dips move with it. So dips are never fought
    hard.
  - **30–200 Hz**: discrete **modal cuts** (`measure_room_cuts` → each speaker's `room_eq` array of
    `{fc, gain_db, q}` peaking sections, rendered as biquads in `align.c`). Below the room's Schroeder
    frequency, modes are approximately minimum-phase, so a magnitude cut also fixes the ringing. The
    correction stays valid over meter-scale distances. **Cut-only** by design and by schema
    (`gain_db <= 0`; the loader rejects boosts), and capped at **12 dB** deep: peaks are modal
    energy you can remove; dips are position-dependent cancellations you cannot fill. The schema
    accepts down to -24 dB, but the solver never designs a cut past -12.

  What no EQ fixes: **decay**. A ringing room still smears transients once its steady-state
  coloration is flattened. That stays a treatment problem; see `--room` above.

  The wrong-file mistake fails loudly: **`bwa_start` refuses** a layout carrying `room_eq` sections
  when the session renders a moving listener (the DBAP panner and/or a connected tracker). See
  `bwa_last_error`. Load the roaming variant, recalibrate with `--room-eq-grid`, or run SPCAP/VBAP
  with a fixed pose.

- **`--room-eq-grid`**: **tracked room EQ**, the moving-listener answer to `--room-eq`'s modal
  half (Lindfors/Liski/Välimäki, JAES 2022, adapted to the tracked CAVE). One point can't room-EQ a
  roaming listener; a **grid of points can**. Below ~200 Hz the room's mode *frequencies* are fixed
  properties of the room. Only how strongly each mode reads varies with position, and that varies
  smoothly on the half-meter scale of LF wavelengths.

  Workflow: run once per mic placement. `--mic x y z` **is the grid key** (a rerun within 5 cm
  replaces that entry; up to 16 positions, `BWA_RQ_GRID_MAX`). Cover the working area at ear height,
  ~0.5–1 m spacing. Each run measures this position's modal cuts (`measure_room_cuts`, same
  30–200 Hz band and 12 dB depth cap as `--room-eq`) and **merges** them into the layout's
  top-level `room_eq_grid` (`calib_room_grid_merge` → `calib_write_room_eq_grid`): per-position fcs
  within ~8% are the same mode, so each speaker gets ONE shared `fc`/`q` ladder with per-position
  depths (0 dB where a position didn't see the mode). See
  [`layout-schema.md`](./layout-schema.md) for the format.

  At runtime the engine interpolates the depths at the **live listener position** every block
  (inverse-distance weights over the grid points). The align biquads glide toward them at
  24 dB/s: click-free by construction, fast enough to track a walk. `bwa_set_tracked_room_eq` is
  the live kill switch (off glides to flat) for A/B on the rig. Works with every panner; `bwa_start`
  has no objection to a grid in a moving session: that's the point.

  **Only** the 30–200 Hz modal band is tracked: the mid/HF room response decorrelates over
  centimeters, far too fast to interpolate between half-meter grid points. The `eq` FIR above stays
  the direct-sound speaker correction for moving installs. `room_eq` and `room_eq_grid` are mutually
  exclusive in one layout file (the loader rejects both together; the grid writeback removes a stale
  static `room_eq` for you).

## Zylia ZM-1: full 3D from one placement

`--localize` needs the omni mic at ≥5 positions because one omni gives only **distance**. The ZM-1
is 19 capsules on a rigid ~10 cm sphere, so a single placement already records each sweep arriving at 19
slightly-different times. The arrival-time **differences** across the sphere are a
**direction**. A speaker's full position falls out of ONE Zylia placement: direction × distance +
the array center.

The solve (`zylia.c`, `zylia_doa` / `zylia_localize`, unit-tested off-hardware in the `zylia` test by
synthesizing the 19 arrivals from a known position and recovering it to machine precision):

- **Direction**: least-squares fit of the 19 arrivals to the far-field model `τ_i = A − (R/c)(dir_i·d)`,
  then a Gauss-Newton refine against the exact spherical wavefront. Latency-independent (uses the
  differences), so it's precise: sub-degree given measure.c's sub-sample IR peak, and it finds the
  screen-hidden speakers too.
- **Distance**: `c·(arrival − latency)`. The array is too small for the wavefront curvature across
  it to self-calibrate the latency at meters (sub-mm of differential delay), so feed a
  loopback-measured or `--localize`-recovered latency. Then the distance is as good as that latency
  (~7 mm per sample). Fuse with the omni `--localize` when you want both the one-shot directions and
  a sub-mm distance.

### The ZM-1's second job: measuring phantoms

Everything above uses the ZM-1 to find **speakers**, from a transient, by arrival times. The same
array also answers a different question: when the array *renders* a source, where does it actually
end up? That needs a different estimator, because a phantom has no arrival time of its own (it is the
summed output of many speakers). So the direction has to come out of continuous content.

`zylia.c` carries both. They share the capsule table, the survey, and the capture rig:

| | `zylia_doa` / `zylia_localize` | `zylia_intensity_doa` |
| --- | --- | --- |
| reads | arrival-time differences | active intensity per frequency bin |
| needs | a transient (sweep, clap) | continuous content |
| answers | where are my speakers | where did the array put this sound |
| band | broadband | 400–1200 Hz (`kr ≈ 1` on a 49 mm sphere) |

Two supporting pieces come with it, both worth knowing about even if you only ever run the survey:

- **`zylia_check_capsules`** flags dead, hot, clipped, and incoherent capsules against the array's own
  robust median. A capsule that goes *hot* is the dangerous case: total array power still looks
  healthy while every spherical-harmonic channel is poisoned, because each one is a weighted sum over
  all capsules. Two estimators agreeing does not clear it. Run this before believing any direction.
- **`zylia_srp_doa`** is an independent steered-power cross-check, reaching `kr ≈ 3` at order 3.
  Coarser than the intensity solve, so use it to check a number rather than to be one.

The measurement workflow built on these is its own tool and its own doc: see
[validation.md](validation.md) and `bwa_validate`.

**Spatial room capture** *(design, not implemented; nothing consumes `er_delay` directionally yet)*
rides the same 19-channel sweep with no new DSP. `--room` already finds each
early reflection's *time* (`er_delay`). Window the 19-ch IR around each one and run `zylia_doa` on
those arrivals to get each reflection's *direction*. That turns the room report from "how live" into
"the first slap comes off the **left wall** at 6 ms", that is, *which* surface to treat, not just how
much. (A full ambisonic room IR is the same capture encoded to higher order; the directional
early-reflection map is the actionable part.)

### Running it (`calibrate --zylia`)

The speaker survey above is wired end to end: the capture shell opens the layout's `n` outputs
plus 19 consecutive inputs starting at `--input`, on ONE ASIO device (the ZM-1 over Dante Via,
next section). It sweeps each speaker, deconvolves all 19 capsules (measure.c), and hands the
sub-sample arrivals to `zylia_localize`. `--mic x y z` is the array **center**. Positions go back
into `cave_layout.json`. Two flags carry the physics the tool cannot know:

- **`--survey <file>`**: a room-axes capsule survey (calib_view → Zylia tab → Capsule survey).
  Without one the tool trusts the built-in table, and an unpinned channel order or yaw rotates the
  whole recovered layout. The tool warns, but it cannot check. (The tool refuses a body-frame
  survey: it has no tracker to re-aim one with. That's `bwa_validate --track`.)
- **`--latency <m>`** or **`--ref <spk> <m>`**: the distance calibration. `--latency` is a
  loopback-measured round trip in meters at c. `--ref` solves it from ONE tape-measured
  center→speaker distance instead (that speaker's mean arrival, wavefront-tilt corrected), so a
  tape measure replaces the loopback rig. The ref speaker's reported `dist` should then read back
  the taped value. Given neither, the run prints directions and **refuses the writeback**: every
  distance would carry the full system latency radially. The tool cross-checks the solved latency
  against the driver's digital loop (below it = physically impossible). No tens-of-ms upper
  warning here: the capture chain legitimately adds tens of milliseconds the driver never reports.
  A clap loopback on the rig measured about 60 ms, which is about 20 m at c. A solved latency
  several times the array's own extent is the expected reading, not a fault.

`--zylia --simulate` runs the identical solve + writeback off-hardware from synthesized arrivals
and recovers every position exactly. The capture shell itself is rig bring-up code like the rest
([hardware-validation.md](./hardware-validation.md), Stage 2).

### Getting the ZM-1 onto Dante

The obstacle to `--zylia` on hardware was never the DSP: the sweep plays out of the **Digiface**
and the ZM-1 is a **different** USB device. The ASIO SDK has one process-wide
current-driver slot (why `asio_session.cpp` exists). Two drivers at once is not a flag, it's a
rewrite.

**Put the ZM-1 on the Dante network and the problem dissolves.** Dante Via transmits the ZM-1's
19 capsules as Dante channels. The Digiface receives them as 19 **inputs on the same ASIO device
that already owns the 26 outputs**. One driver, one clock domain, sample-locked:
`calib_capture.cpp` went from "N outs + 1 mic input" to "N outs + 19 inputs", a parameter, not an
architecture (`calib_asio_open_multi`; the omni modes are its `nin = 1` case).
([Danowski's write-up](https://blog.przemekdanowski.com/connecting-zylia-zm-1-to-dante-network/)
is the recipe; Dante Via has a trial, so you can prove the route first.)

What it buys, beyond unblocking the sweep:

- **Absolute latency, so distance becomes real.** `zylia_localize`'s range is only as good as the
  latency you feed it, and the array is far too small to self-calibrate that. Sample-locked
  playback and capture make the round-trip a measurable constant, so the ZM-1 delivers full 3D
  **positions** from one placement, not just directions.
- **Sweeps instead of claps** for the capsule survey: far better SNR, sub-sample arrivals off the
  deconvolved IR peak, 26 known source directions for free.
- **Spatial room capture** (above) becomes buildable at all: it needs the 19-channel IR.
- The cable run: the ZM-1 wants to sit in the *middle* of the CAVE, and USB will not reach.
  100 m of Cat6 will.

Caveats to check first. **The capture chain is slow: about 60 ms, not the ~10 ms a single Via leg
suggests.** Measured with a clap loopback, that is, ZM-1 in, straight back out to a speaker, both
the clap and its replay recorded on an independent device. The ZM-1's own USB stack, a Via leg in
each direction, and the ASIO buffer all stack up. This costs nothing as long as you remember it:
the number is constant and measurable, so `--latency` absorbs it (60 ms is 20.6 m at c), the 0.5 s
capture tail (`CAL_NTAIL`) covers it many times over, and the production render never runs the ZM-1
at all. What it does rule out is guessing the latency instead of measuring it.

26 out + 19 in is **45 channels**, so confirm the Digiface offers that many
at your rate. Dante endpoints commonly halve their channel count at 96 kHz; calibrate at 48 kHz.

**Dante Via presents the ZM-1 as 20 input channels, not 19.** The 20th carries no capsule and
appears to be inactive. Only the first 19 of the block are capsules, in order, so `--input` still
points at the first capsule and the tool still reads 19 consecutive inputs. Budget the routing
for 20 channels and leave the last one unpatched.

None of this is required to *start*. The capsule survey below runs on claps through the existing
capture shell, and `zylia_survey` does not care whether its arrivals came from a clap's
cross-correlation or a sweep's deconvolved IR peak. Dante is the precision upgrade, not the
prerequisite.

### The capsule geometry

`zylia_geometry` is the real array: the ZM-1's 19 capsules are the 20 vertices of a **regular
dodecahedron, vertex-up, minus the nadir vertex**. Zylia doesn't put the coordinates in the spec
sheet, but they publish the node table and [ambitools](https://www.sekisushai.net/ambitools/docs/grids.html)
reproduces it. It self-checks: elevation rings come out 1 / 3 / 6 / 6 / 3 (the missing 20th vertex is
the one at −90°), each ring is its opposite ring rotated 180°, and the elevations are exactly
`asin(√5/3) = 48.1897°` and `asin(1/3) = 19.4712°` with azimuths generated by `atan(√(3/5)) =
37.7612°`. It's built from those closed forms, so nothing is rounded. The `zylia` test pins the
structure: ring populations, the unpaired zenith, the sum-to-zenith identity, the 41.81°
dodecahedral edge as the closest pair.

The radius (49 mm) only feeds `zylia_localize`'s near-field solve. `zylia_doa` normalizes the fitted
gradient, so the radius **cancels out of the direction entirely**: 49 versus 50 mm changes nothing.

**Three things the table cannot give you**, and no off-hardware test can catch any of them, because
all three survive every structural check above:

- **Channel order**: node *i* here is not necessarily ASIO input *i*. A permutation still yields a
  confident direction, just the wrong one. `bwa_zylia_probe` resolves it: tap a capsule, see which
  channel jumps.
- **Azimuth reference**: nothing published says which capsule faces the device's front, so an unknown
  yaw offset rotates every DOA by a constant. Clap from a known direction in the Zylia tab; the
  discrepancy *is* the offset.
- **Handedness**: a mirrored capsule numbering survives the structural checks too, because a reflected
  dodecahedron is still a dodecahedron with the same rings. It survives the azimuth check as well: a
  clap fits a rotation and cannot see a mirror. The collaborators' AES 161 measurements fit exactly
  this, a fixed azimuth-handedness reflection present on every recording day, and it dominated their
  mount fit.

Pin all three at the rig, or skip the question entirely and **measure the geometry** (below), which
hands you the order and the orientation as a side effect.

### The capsule self-survey (`zylia_survey`)

Rather than trust a table and then hand-pin the two things it can't tell you, **measure the array**.
The capsules come back indexed by *the ASIO channel that fed them*, expressed in *room axes*, so the
result **is** the channel order and **is** the mounted orientation. Nothing is left to pin.

Sound from a known direction `d_k` reaches capsule `i` at `τ[k][i] = t0_k − (1/c)·m_i·d_k`. The `t0_k` is
unknown and unknowable (system latency, the moment the clap happened, whichever channel `zylia_tdoa`
picked as its reference), and it *does not matter*. It is one constant per observation, so
subtracting each observation's mean across the 19 capsules kills it exactly. What's left is linear
in `m_i` and **separates**: each capsule gets its own 3-unknown least squares, all sharing one 3×3 normal matrix.

Three consequences worth drawing out:

- **No sweep, no sample-sync, no second audio device.** Claps, through the capture shell that already
  exists. This is why the survey is available *today*, ahead of any Dante work.
- **A plane wave is not quite the truth.** A clap 2.5 m out is a sphere, and its curvature across a
  49 mm array is a systematic ~1.4 µs: 2–3 mm of capsule error if ignored. So the solver takes the
  source's *position*, not merely its direction (you know where you clapped; that's how you knew the
  direction). It iterates the exact-minus-plane-wave correction on top of the linear seed. The
  geometry lands well under a millimeter.
- **Where is the origin?** Arrival times fix the capsule cloud's *shape* but not its *position*:
  the per-observation constants absorb any translation of the whole cloud. So you must choose the
  origin, and the obvious choice is wrong. The ZM-1's capsule set is a dodecahedron *missing its nadir*,
  so it is not centroid-balanced and its centroid sits **R/19 = 2.6 mm above the sphere center**. Nobody
  tape-measures to the centroid. The solver therefore fits the sphere the capsules lie on and re-centers
  on that. That gives the physical center of the shell: the point the operator measured the clap
  positions from, and what `zylia_localize`'s `center` means.

**Running it** (calib_view's Zylia tab → *Capsule survey*): tape-measure where the ZM-1 is, set the clap
position, clap. Repeat from ≥ 6 clap positions. If layout A is loaded you can pick "clap at
speaker *N*" and the position autofills from the surveyed geometry: stand at a speaker, clap, move on. Then **Solve** →
**Install** → **Save**. `--tests zylia` drives the whole flow on synthetic claps.

**Spread is the trap.** Claps in a horizontal ring around the array are *coplanar*: the normal matrix
goes singular in the vertical and the capsules' **heights** are unrecoverable. A solver that shrugged
would hand back a flattened array and a confident wrong answer, so this one refuses. `spread` is 1 for
isotropic directions, 0 for coplanar, and below 0.05 the solver declines and tells you why. **Clap high
and low, not just around.**

**Reading the result.** `residual` is the number that says whether to believe it: what the recovered
geometry *fails* to explain, in microseconds. Sub-microsecond is clean. Tens of microseconds means bad
claps, a wrong array center, or a clap that wasn't where you said it was. `radius` should land near
49 mm; if it doesn't, something is badly wrong upstream. Save writes JSON that encodes the geometry,
the channel order *and* the orientation together. The result is specific to **one ZM-1 on one
mount**, so re-survey if either changes.

**Bring-up.** Before any of that, run the "is it talking?" checks the moment the ZM-1 is plugged in
(input-only, no rig needed). Both ride the same capture shell (`zylia_capture.cpp`: driver open,
transient trigger, snapshot publish):

- `bwa_zylia_probe` (built with `-DBWA_BUILD_CALIBRATE=ON`): console meter. `--list` enumerates
  the ASIO drivers + channel counts (look for the Zylia driver, or ASIO4ALL over its USB-audio
  interface, showing `in=19`; it auto-picks a name containing "zylia", else `--driver` it). Tap a
  capsule and watch its channel jump; a channel stuck at digital silence is dead or unmapped.
- **Live DOA view**: `bwa_calib_view`'s **Zylia tab** (built with `-DBWA_BUILD_CALIBVIEW=ON`;
  live capture needs the ASIO SDK too): CLAP anywhere around the array and a dot appears on the
  capsule sphere where the clap came from (`zylia_tdoa`: onset + windowed cross-correlation against
  the strongest capsule with sub-sample parabolic peaks → `zylia_doa`). This verifies the capsule
  MAPPING and the GEOMETRY table in one gesture: swapped channels or a wrong geometry row put the
  dot somewhere absurd. Its simulate mode runs the identical snapshot→tdoa→doa→draw pipeline on
  synthesized claps (truth marker drawn; "Clap now" for a deterministic one): the hardware-free
  check of everything but the ASIO capture. The math is unit-tested in the `zylia` ctest, and the
  `zylia/sim_doa` UI test in `calib_view` drives the whole tab.

## Reviewing the results (`bwa_calib_view`)

Before trusting a calibration run, LOOK at it. `bwa_calib_view` (opt-in `-DBWA_BUILD_CALIBVIEW=ON`)
loads layouts through the engine's own loader and shows the array in 3D with index labels,
gain/delay trims as bar charts, each speaker's correction-EQ magnitude, the retained IR kernels,
and, the main event, a **layout diff**. `bwa_calib_view before.json after.json` tables
Δposition / Δgain / Δdelay / eq-taps per speaker with outliers highlighted, so a swapped channel,
a bad mic placement, or a bogus `--localize` solve is one glance, not an evening.

It is the **calibration station**: one window for the rig session. The **Capture tab** runs the
calibration itself (sweep, solve, write, on a worker thread, through the same capture backends
and measure/solve DSP as the CLI; simulate hardware-free, ASIO full-duplex at the rig). One
button loads the result into Diff for review before you accept it. The **Zylia tab** (live
clap-DOA, see "Bring-up" above) covers the ZM-1.

`bwa_calibrate` remains the headless CLI over the same code: scriptable, and the only place for
the multi-placement modes (`--localize`, `--zylia`, `--check`, `--live`).
`bwa_calib_view --tests [filter]` runs its imgui_test_engine suite, wired into ctest as
`calib_view`.

## What feeds the engine

`cave_layout.json` carries per-speaker `position`, `gain_db`, `delay_ms` (consumed by `dbap.c` +
`align.c`). Model the room itself, if you want simulated reverb, in Steam Audio as geometry +
material absorption (`steam_reflect.c`) tuned to creative intent, **not** to the measured RT60.
