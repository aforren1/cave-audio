# Speaker calibration and room characterization

How the speaker array (24 in the CAVE) is surveyed, trimmed, and characterized at install time, and
how those numbers reach the engine. The tool is `bwa_calibrate` (`examples/calibrate.cpp`, opt-in
`-DBWA_BUILD_CALIBRATE=ON`). The measurement DSP is `measure.c`; the solve + JSON writeback is
`calib.c`. All of it is unit-tested off-hardware (`test_measure`, `test_calib`). The ASIO
full-duplex capture is the only part that needs the rig. Terms used here without definition are in
[glossary.md](./glossary.md).

**Everything here follows the layout's speaker count** (`n`, 4..64; see
[`layout-schema.md`](./layout-schema.md)), not a hard-wired count. The capture opens `n` ASIO outputs
plus the mic input (which rides buffer slot `n`, or, with `--zylia`, the ZM-1's 19 capsule inputs
on slots `n`..`n+18`). It sweeps those `n` speakers and writes `n` records back.
`bwa_calib_view` likewise sizes its plots from each loaded layout. It refuses to Diff two
layouts with different speaker counts rather than mis-compare them.

## The two layout tools

- **`bwa_layout_tool`**, *authoring*: place the speakers + identify which channel drives which
  physical box (by ear, via the channel test signal) → writes positions + the channel map.
- **`bwa_calibrate`**, *survey + tuning*: measures positions acoustically, and the per-speaker
  delay/gain trims, into the same `cave_layout.json`.

Bring-up order: `calibrate --aim-sheet` (before the boxes go up) → `layout_tool` (channel map) →
`calibrate --localize` (positions; or `--zylia` from one ZM-1 placement) → `calibrate` (trims; or
`--zylia --trims`) → `calibrate --verify` from the same placement → `calibrate --room`
(sanity-check the room). Then the engine loads the layout.

## How it measures (exponential sweep + deconvolution)

Each speaker plays a Farina exponential sine sweep; an omni mic records it. `measure_response`
recovers that speaker's impulse response by regularized deconvolution and reads two numbers: the
**delay** (direct-path arrival = system latency + time of flight) and the **level** (broadband
sensitivity). One sample at 48 kHz = 7 mm, and sub-sample peak interpolation gets well below that.
So the limit is the mic-position accuracy and the assumed speed of sound, not the acoustics. Set the
second one: see "Air temperature" below.

Use an **omnidirectional** measurement mic. It's flat and direction-independent, so each speaker's
delay/level/response comes back uncolored. If the ZM-1 is the only mic you have, `--zylia --trims`
turns its 19 capsules into a stand-in for one: see "The ZM-1 as the trim mic" below. Measure
*through* the acoustically-transparent screens. That's what the listener hears, and the trim
captures the screen's slight HF loss automatically.

### What the screens do

An acoustically transparent screen is not transparent. It is close enough in the band the phantom
validation works in (400 to 1200 Hz, where loss is a fraction of a dB and the response is near flat),
and it is worst from about 2 kHz up, which is where pinna elevation cues and interaural level
differences live. So the screens cost you more by ear than on any instrument in this repo.

Three effects, in the order they matter:

- **Per-speaker level and delay.** The first-order term. The trims absorb it, as long as you sweep
  through the screen. That is the whole reason for the instruction above.
- **A cavity comb.** A speaker mounted `d` behind the screen plane gets its own reflection back off
  the screen, off whatever is behind the speaker, and out again, so its response carries notches near
  `c/2d * (n + 1/2)`. At `d` = 0.35 m that is 245, 735 and 1225 Hz, inside the analysis band. Shallow,
  since most of the energy transmits, but different per speaker, and a broadband trim cannot correct a
  notch. Nothing here does: `room_eq` is LF modal cuts and sits below this.
- **Angle-dependent transmission.** Loss rises with incidence, so an obliquely firing speaker is not
  attenuated like a normally firing one, and the difference follows the listener.

The first is common to a given speaker's whole path, so it cancels out of that speaker's measured
direction: a per-bin complex scalar cannot rotate an intensity vector. The other two do **not** cancel
for a phantom, which is a coherent sum over speakers that differ from each other. Screens are a
phantom error term rather than a physical-source one, which is exactly what makes them invisible to
`bwa_validate`'s reference arm.

**Speakers that fire over the screen top are a different population.** In a CAVE the ceiling band
clears the screen edge while every wall speaker fires through fabric. If the over-screen speakers run
even 1 to 2 dB hot in band, phantom images pull upward. The collaborators' 24-speaker array has that
split and measured that signature, a median +12° of upward elevation displacement, which the paper
attributes to speaker density. Both mechanisms push the same way, so their data cannot separate them.
Calibrating through the screens is what keeps the two populations comparable. Check it directly:
split the per-speaker levels by whether the path crosses a screen, and compare.

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

**Projectors.** The CAVE's projectors warm the room by about 3 F (1.7 C) over a long session,
which is 1 m/s of `c`, 12 mm of range at 4 m and 35 microseconds of arrival time. That is under
the survey's other error terms and far under the 1 ms scale where alignment reads, so it needs
no second calibration. What the projectors do change is the noise floor: their fans sit under the
direct-sound gate and the RT60 decay fit. So warm the room up with them on, calibrate in that
state with `--temp` set to the air temperature you read then, and leave them on for every pass.
That is the state the listener is in. If the first sweep's IR is dirty, you have a noise problem,
not a temperature one; treat it as one.

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
- **`--zylia`**: says the mic is a ZM-1 (its 19 capsules start at `--input`, `--mic` is the array
  center). The mode comes from the other flags. Alone, `--zylia` is the same self-survey as
  `--localize` from **one** mic placement: direction from arrival-time differences, distance from a
  known latency (`--latency` or `--ref`). See "Zylia ZM-1: full 3D from one placement" below. With
  `--trims` it measures the trims, and with `--verify` it runs the second pass, both through a
  pressure proxy: see "The ZM-1 as the trim mic" below.
- **default** (or **`--trims`**, the same mode spelled out): trims. `calib_solve` turns the per-speaker measurements into `delay_ms`
  (arrival-align every speaker to the farthest) and `gain_db` (equalize sensitivity, with the
  speaker→mic distance divided out so it corrects the *speaker*, not distance; cut-only so nothing
  clips). Those trims align the array at **one** point. The engine can optionally re-reference them
  onto the tracked listener at runtime (`bwa_set_tracked_align`, off by default), the time-alignment
  counterpart of `--room-eq-grid` below. See
  [`spatialization.md`](./spatialization.md#re-aligning-to-the-tracked-listener-bwa_set_tracked_align-off-by-default).
- **`--verify`**: the second pass. Plays every speaker through the trims the layout carries and
  reports what is left over. Nothing is written. See "Verify: the second pass" below.
- **`--aim-sheet out.csv`**: no audio. Writes the installer's aiming sheet and prints a readiness
  summary. See "The aiming sheet" below.
- **`--room`**: RT60 (Schroeder) + early reflections from the captured IRs. It measures **how live
  your room is**. **Do not copy the measured RT60 into the engine's reverb settings.** The room's
  own decay is a **floor**: you cannot render a space deader than the room you're in. Nearby
  surfaces that throw early reflections smear localization. If the room is too live, **treat it
  physically**: you cannot DSP reverb away for a moving listener (it is non-invertible and
  position-dependent). The binaural monitor (headphones, room-free) is the clean reference.
  Comparing array-vs-monitor measures how much the room is adding.
- **`--check-aim`** (with `--localize`, and a layout carrying a `directivity` model): fit each
  speaker's acoustic axis from the captures `--localize` already took. Each mic position sees
  each speaker at a known bearing, and the deconvolved response's high-to-mid band tilt changes
  with that bearing the way the model says, while the speaker's own response cancels out as a
  per-speaker constant. The tilt is the DIRECT sound's: `measure_response` gates the impulse
  response from 1 ms before its peak to just before the first reflection (the `--eq` gate
  policy, 4 ms when no reflection shows). The reverberant part does not follow the axis, and
  left in, it dilutes the bearing dependence so the fit reads smaller errors than are there.
  The tilt bands are 1 to 3 kHz against 3 kHz up, not the 300 Hz and 3 kHz level bands: a gate
  of a few ms smears the spectrum over about 1/gate (250 Hz at 4 ms), and the gate changes with
  the mic position, so a band edge at 300 Hz would carry a position-dependent window error into
  exactly what the fit compares. That costs contrast: on the 4410A the tilt at 45 degrees is
  2.8 dB instead of 3.8. The report fits the whole-response tilt too, in its own column and in
  a summary line beside the gated median, so the dilution is visible on every run.
  `calib_check_aim` grid-searches the direction that explains the tilts
  best and reports it against the layout's `aim`, with the residual before and after. A speaker
  over 15 degrees out with a clearly better fit is flagged: the mount is aimed wrong, or the
  layout's `aim` is. Resolution is set by the bearing spread the positions give it, roughly
  10 degrees over a 3 m working area, so it refuses under 8 degrees of spread rather than
  guess, and it never fits the balloon itself. The run also prints the residual every speaker
  shares as a function of bearing: a slope there is a loss the model does not carry, the screens
  being the obvious one, and not an aim error. Diagnostic only, nothing is written back.
  `--simulate --sim-aim-error 20` rotates every simulated speaker's true axis by 20 degrees, so
  the check has a known error to recover; that is how you know it is measuring anything. Add
  `--sim-room` and the capture has a room to dilute it: on the 26-speaker example with the
  4410A model and 7 positions, the gated fit still reads a median 21.0 degrees while the
  whole-response fit reads 13.6. The `calibrate_sim_room_aim` ctest pins both.
- **`--simulate --sim-room [absorption]`**: the simulator's room. Without it a simulated capture
  is anechoic, so the direct-sound gate and everything built on it (`--check-aim`'s tilt, the
  trims' directivity correction) have nothing to act on. With it, the array sits in a shoebox
  0.5 m larger than the speakers on every side. Each speaker adds image sources of orders 1
  and 2 (24 of them), each leaving at its own departure angle off the speaker's axis and so
  carrying the directivity model's loss at that angle, plus 1/r and `sqrt(1 - absorption)` per
  bounce, generated as the same analytic delayed sweep the direct sound is. Past order 2 a
  deterministic noise tail stands in: Sabine RT60 for the box, the room equation's diffuse
  level for what orders 1 and 2 did not already deliver, shaped by the model's power response,
  starting two mean free paths after the direct sound. Absorption defaults to 0.3 (RT60
  0.36 s on the 26-speaker example). The whole `--localize --check-aim` run above, 182
  captures, takes 18 s instead of 11 s anechoic. It is a test fixture, not a room model: no
  frequency-dependent absorption, no scattering, no air absorption.
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
  interpolation puts the reading at well under 1 mm. With `--zylia` it is live aiming instead:
  one speaker's position against the layout and its off-axis angle, once per sweep. See "Live
  aiming" below.
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
  for a cleaner near-free-field correction than an omni gate; that's a follow-on. Today
  `--zylia --eq` is refused: see "The ZM-1 as the trim mic".

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

## Speaker directivity from the vendor's simulation file

Speaker makers publish measured directivity balloons for room simulators. Genelec's page
for the 4410A ([genelec.com/4410a](https://www.genelec.com/4410a), Downloads) has one in the
Common Loudspeaker Format, `Genelec_Oy-4410A.CF2`: 27 third-octave bands from 40 Hz to
16 kHz, each a full sphere at 5 degree resolution, in dB relative to on-axis. The 4010A's
EASE and GLL files on the same page are proprietary binaries the tooling does not read.

Convert it once and the layout carries the result:

```
uv run tools/directivity/clf_to_json.py Genelec_Oy-4410A.CF2 --into cave_layout.json
```

That writes a top-level `directivity` block (format in
[layout-schema.md](./layout-schema.md)). The converter decodes the binary itself and
checks its decode against the file's own coverage-angle lists before it writes anything.
`examples/genelec_4410a_directivity.json` is the standalone result for the 4410A. The
model is one per layout, so a mixed array takes its dominant model, and it is
axisymmetric about each speaker's axis: the balloon's planes are averaged, because the
layout does not know how a speaker is rolled on its mount. On the 4410A that costs up to
3 dB around 2 to 2.5 kHz for a box rolled 90 degrees, and under 1 dB elsewhere.

Then tell the layout where each speaker points. A speaker record's `aim` is a direction
vector along its acoustic axis. Leave it out and the loader aims the speaker at the
listening point, which is the CAVE's case and the dome's.

**What it changes in `bwa_calibrate`.** The default trim run prints a directivity report:
each speaker's bearing off its axis as seen from the mic and from the listening point, the
model's loss at each, and the correction it applies. The correction re-aims the measured
sensitivity from the mic's bearing to the listening point's, the way the 1/r term is
already divided out, so a mic that could not sit at the listening point still produces the
trims that point hears. With the mic at the listening point every correction is 0 dB by
construction and the trims are exactly what they were. `--ignore-directivity` skips it.

The model describes the direct sound, but the trims are built on the whole response, and in a
live room part of that is reverberant energy that does not follow the axis. Applying the
model's ratio `r = D(ref) / D(mic)` to all of it over-corrects. So the correction acts on the
direct share only. Each capture's `direct_frac` is `f`, the gated share of its energy over the
level band, and the amplitude factor is:

```
corr = sqrt(f * r^2 + (1 - f))
```

An anechoic capture gets `r`, a fully diffuse one gets 1, and a mic at the listening point
still gets exactly 1 whatever `f` is. The report prints `f` per speaker as `direct`, after the
sweeps, and a one-line min, median and max. An anechoic simulated capture reads 1.00 on every
speaker. `--simulate --sim-room --mic 0.6 1.2 0.4` on the 26-speaker example reads 0.29 to
0.74, median 0.42, so there the correction applies well under half of the model's ratio (the
`calibrate_sim_room_trim` ctest pins shares below 1). `calib_directivity_corr` is the one
implementation, so `bwa_calibrate` and `calib_view` write the same trims from the same captures.

**The correction needs the mic's real position.** `bwa_calibrate` applies it only when you pass
`--mic`: the default mic, `(0, 0, 0)`, is the floor origin, and its bearing off every speaker
would put several dB of fictitious correction into the trims. Without `--mic` it says so and
leaves the trims as measured. `calib_view`'s Capture tab follows the same rule. Its mic field
starts at the loaded layout's listening point, the correction stays off until you edit the
field, and a one-line note under the field says which is in effect. "use listening point" puts
the field back and turns the correction off again.

The report is also a first aim check. A speaker whose bearing from the listening point is
over 30 degrees is flagged: either the mount is aimed wrong or the layout's `aim` is, and the
difference is audible treble at the sweet spot. That only catches an `aim` that disagrees with
the geometry, though. Whether the box is physically pointed where the layout says is what
`--check-aim` measures (see "Modes"), from the captures `--localize` already takes.

**What it changes at runtime.** With the model loaded, the engine follows each speaker's
off-axis loss onto the tracked listener (`bwa_set_tracked_directivity`, on by default,
identity at the listening point). The headphone audition of the array (`cave_sim`, and the
`cave_both` monitor) plays each virtual speaker's physical off-axis loss toward the listener at
the model's full resolution, with its distance and arrival time, so a walk on headphones hears
the comp's two-band residual against the loss it is fighting. What that can and cannot buy you:
[spatialization.md](./spatialization.md#compensating-speaker-directivity-for-the-tracked-listener-bwa_set_tracked_directivity-on-by-default).

Not yet: the `--eq` correction FIR is still designed from the mic's bearing. Measured
off-axis, it flattens the off-axis response, which is right for a mic at the listening
point and wrong elsewhere. Dividing the model's off-axis curve out of it is a follow-on.

## Verify: the second pass (`--verify`)

The trim run sweeps the raw outputs and writes trims it never plays. `--verify` plays them. Run it
from the same mic placement, right after the trims are written:

```
bwa_calibrate --layout cave_layout.json --mic 0 1.448 0 --verify
bwa_calibrate --layout cave_layout.json --mic 0 1.448 0 --zylia --input 26 --verify
```

For each speaker it puts the sweep on that speaker's channel, silence on the others, runs it through
the engine's own output stage (`align_create` plus `align_process`, the code the engine renders
with), and plays that channel. It deconvolves the capture against the **raw** sweep, so the measured
response carries the trims. `--simulate` does the same in the other order: it simulates the raw
capture and runs the capture through the stage. The stage is linear and time-invariant, so the
order does not change the result.

**It checks the static stage**: `gain_db`, `delay_ms`, the `eq` FIR and a static `room_eq`. Three
stages follow the listener, and all three are identity at the listening point: `room_eq_grid`
(the engine interpolates it from the tracked position; verify plays it at its flat start), tracked
alignment, and the directivity comp. Verify leaves all three at identity. With the mic at the
listening point that is exactly what a listener there hears. With the mic anywhere else it is what
a listener at the mic hears before the engine starts tracking them, and the tool prints a note
saying so.

Per speaker it prints two residuals, each with the median over the speakers removed:

- **Arrival**, in microseconds: the measured arrival minus the aligned arrival. The trim solve
  makes every arrival equal at the point it measured from, so from that placement the expected
  arrival is one constant: the system latency plus the farthest speaker's flight time. The median
  stands in for it, which also makes the check blind to the latency. `calib_verify_residuals`
  takes the point the trims were aligned at and subtracts `(d_k(mic) - d_k(point)) / c`, but the
  CLI passes the mic itself, so run verify from the trim run's placement.
- **Level**, in dB: the measured level times the speaker-to-mic distance times the directivity
  re-aim factor, the normalization the trim solve used to write `gain_db`. After correct gain
  trims every speaker reads 0 dB.

It flags an arrival beyond ±100 µs and a level beyond ±1 dB:

- **100 µs** is 3.4 cm of path and about 5 samples. A correct pass leaves up to about one sample
  (the trims are whole samples, and the peak interpolation adds a little), and an inter-speaker
  delay moves a phantom image over a range of about 1 ms. So the threshold sits five times above
  the first and at a tenth of the second.
- **1 dB** is about the level just-noticeable difference for broadband noise, and far above what a
  correct pass leaves.

A summary line prints the peak-to-peak spread of both. Exit code 3 if anything is flagged, like
`--check`. Nothing is written back.

What a flag means:

- **ARRIVAL** on one speaker: its `delay_ms` is not what the trim run wrote (a hand edit, a stale
  file), the box moved, or its Dante latency differs from the others.
- **LEVEL**: `gain_db` changed, the box's own volume differs, or an `eq` FIR moves the level band.
  The trim solve does not account for the FIR's own level, so an `--eq` layout leaves some: 0.36 dB
  of spread on the simulated 26-speaker example.
- **DEAD**: no signal at all.

In simulation (the 26-speaker example with the 4410A model, `--sim-room`, mic at `0.6 1.2 0.4`),
a correct pass reads an arrival spread of 18.9 µs and a level spread of 0.01 dB, with the omni and
with the ZM-1. Corrupt one speaker's `delay_ms` by 0.5 ms and another's `gain_db` by 3 dB and
exactly those two flag, at about +490 µs and -3.00 dB. The `calibrate_verify_omni` and
`calibrate_verify_zylia` ctests pin both halves.

Unverified on hardware: the rig path plays a per-speaker signal (`calib_asio_capture_signal`), the
sweep run through the stage, which is longer than the sweep by the longest delay, the FIR and
50 ms. The tool refuses a layout whose delays would eat more than half of the 0.5 s capture tail.

## The aiming sheet (`--aim-sheet`)

Print it before the boxes go up:

```
bwa_calibrate --layout cave_layout.json --aim-sheet aim.csv
```

It opens no audio device. It loads the layout with the engine's own loader, so the sheet uses
exactly the listening point and the default aims the engine uses: the target is `Layout.ref`, which
is `listening_point_m` when the file declares one and the array centroid when it does not, and a
speaker with no `aim` points at it. One CSV row per speaker, one plain header row, no comment lines,
so a spreadsheet opens it as is:

| column | what |
| --- | --- |
| `speaker`, `x_m`, `y_m`, `z_m` | the index and position |
| `target_*_m`, `distance_m` | the listening point and the distance to it |
| `aim_x/y/z`, `bearing_deg`, `down_tilt_deg` | the aim to set: toward the listening point |
| `layout_aim_*`, `layout_aim_source` | the layout's own `aim`, and `explicit` (in the file) or `default` |
| `layout_bearing_deg`, `layout_down_tilt_deg` | the layout aim as angles |
| `layout_aim_off_deg` | the angle between the layout aim and the aim toward the listening point |
| `loss_2k_db`, `loss_16k_db` | the directivity model's loss at that angle (empty with no model) |
| `flag` | `OFF_AIM` when that angle is over 20 degrees |

The angles are in the room frame (`src/core/frame.h`, `bw_audio.h`): +y is up, +z is room-ahead,
and room-right is -x.

- **Bearing** is horizontal, clockwise seen from above: 0 is room-ahead (+z), 90 room-right (-x),
  180 behind (-z), 270 room-left (+x). Lay it out from the room axes with a protractor, not from a
  compass: magnetic north is not room-ahead. A vertical aim has no bearing, and the sheet writes 0.
- **Down-tilt** is the angle below the horizontal: positive points down, negative points up, and
  90 is straight down. Read it with an inclinometer on a cabinet face parallel to the acoustic axis.

It also prints a readiness summary: whether `listening_point_m` is declared, and its height in m
and ft (4.75 ft is `"listening_point_m": [x, 1.448, z]` on a floor-origin layout); whether a
directivity model is present; how many speakers carry an explicit `aim`; and every speaker whose
layout aim is more than 20 degrees off the listening point. Exit code 3 if there is one. At 20
degrees the 4410A's model loses 2.0 dB at 2 kHz and 3.6 dB at 16 kHz toward the listening point;
at 30 degrees, the directivity report's flag, it is 3.7 and 6.9 dB. 20 sits between that flag and
`--check-aim`'s 15. The `calibrate_aim_sheet` ctest aims one speaker 46 degrees off and reads 6.6 dB
at 2 kHz and 12.1 dB at 16 kHz.

A layout with no `aim` fields cannot be off aim: the loader points every speaker at the listening
point, so the sheet's job there is the installer's numbers. Once `aim` fields are in the file, or
the listening point moves, the flag column catches a box the file says points elsewhere.

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

### The ZM-1 as the trim mic (`--zylia --trims`, `--zylia --verify`)

On the rig the ZM-1 is the only measurement mic, so it measures the trims too. Put the array's
center AT the listening point (`--mic` is the center, and here it is the 4.75 ft point): the
delays equalize arrival at the mic, while the engine treats the trims as aligned at the
listening point, and tracked alignment is identity only there. The trim run warns when the mic
is more than 5 cm from it.

```
bwa_calibrate --layout cave_layout.json --zylia --trims --survey s.json --input 26 --mic 0 1.448 0
bwa_calibrate --layout cave_layout.json --zylia --verify --survey s.json --input 26 --mic 0 1.448 0
```

`--zylia` only says which mic is plugged in. Bare `--zylia` is still the position survey above;
`--trims` and `--verify` pick the other two modes. The trims want what an omni at the array center
would measure: one delay, one level, one direct share per speaker. The ZM-1 has no capsule at its
center, and above about 2 kHz no single capsule is a pressure mic: on a 49 mm rigid sphere the
capsule facing the speaker reads up to +6 dB and one in the shadow reads far less. So the tool
sweeps each speaker once, deconvolves all 19 capsules, and pools them (`zylia_pressure_proxy`):

- **Level, bands, direct-sound fields**: the power mean over the capsules, that is the RMS of the
  19 capsule values. The surface-averaged power of a plane wave on a sphere does not depend on
  where the wave comes from, so the mean over 19 points spread over the surface is close to
  direction-independent where any one capsule is not.
- **Direct share**: energy-weighted over the capsules, so it stays a share of the pooled energy.
- **Delay**: the arrival at the array center. That is the capsules' mean arrival, corrected for the
  wavefront's tilt across the sphere along the measured direction (`zylia_center_arrival`, the same
  code `--ref` uses). The correction matters: the capsule centroid sits 2.6 mm above the center, which
  is up to 7.6 µs for a speaker straight overhead.
- **The mic position** is the array center, `--mic`. `--survey` supplies the capsule geometry. Without
  it the built-in table stands in: the power mean does not care about channel order, and the tilt
  correction depends on the orientation by at most about 15 µs.

**What is left of the direction dependence.** The `zylia` ctest models the rigid sphere
analytically and rotates a plane wave over 400 directions:

| frequency | 19-capsule power mean | one capsule |
| --- | --- | --- |
| 500 Hz | 0.04 dB | 0.8 dB |
| 1 kHz | 0.21 dB | 3.6 dB |
| 2 kHz | 0.34 dB | 6.8 dB |
| 4 kHz | 0.55 dB | 11.4 dB |
| 8 kHz | 0.53 dB | 16.7 dB |
| 16 kHz | 0.52 dB | 22.5 dB |
| broadband level (the trims' band) | 0.49 dB | 8.8 dB |

Each figure is the max over min across directions. So a ZM-1 trim carries up to about ±0.25 dB of
error that depends on the speaker's direction, against ±4 dB from one capsule. The leftover is the
sampling error of 19 points that are a dodecahedron minus its nadir, not an even spread. A part that
does NOT depend on direction, the sphere's own mean coloration against the free field, is common to
every speaker and cancels out of relative trims.

**The simulator places each capsule at its own position**: its own flight time, 1/r, directivity
bearing and simulated room. It does not model the rigid sphere, so every simulated capsule is a
free-field omni and the table above is the only place the shadowing shows up. With that caveat, a
simulated ZM-1 trim run lands within 0.10 dB and one sample (20 µs) of the omni run on the
26-speaker example with `--sim-room` and an off-axis mic, and within 0.04 dB and the same samples
anechoic. The `calibrate_zylia_trims` ctest pins 0.25 dB and 25 µs.

**The EQ modes.** `--eq` and `--room-eq` are refused with the ZM-1. Both invert one impulse response
into a filter, and the ZM-1 has none that stands for the pressure at its center: above about 2 kHz
every capsule carries the sphere's shadow, differently for every direction, and the filter would bake
it in. The power mean is a level, not a response. `--room-eq-grid` works: it reads 30 to 200 Hz from
the MEAN of the 19 capsule captures, and at 200 Hz the sphere is acoustically transparent (ka under
0.2). `--room` and `--save-irs` read the same capsule mean. It is the center pressure below about
2 kHz and a direction-dependent beam above it, so treat the RT60, the reflection levels and the
saved IRs above 2 kHz as approximate.

**A dead capsule stops the run.** One whose level is non-finite or more than 20 dB under the
capsules' median would cost the power mean only 0.2 dB, but its "arrival" is the peak of noise, and
one wild arrival in 19 moves the center arrival by milliseconds. So the tool names the capsule and
its input and exits. The sphere's own shadow stays well above that line: a shadowed capsule's
broadband level sits at most about 9 dB under a lit one's.

Unverified on hardware: all of it. A live capsule's gain error enters the power mean at a
nineteenth of its weight (one capsule 1 dB hot moves the mean by 0.06 dB), so run
`bwa_zylia_probe` first and look for a hot channel as well as a dead one.

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

## Optical speaker check (`bwa_speaker_survey`)

The acoustic survey places every speaker. It does not check where a speaker points to better
than about 10 degrees, and it cannot tell you whether Motive's frame is the room frame your
layout is written in. The speakers the cameras can see answer both. On the CAVE that is about
four, which is enough.

### Set up the bodies in Motive

1. Stick 4 markers flat on the speaker's front baffle, around the tweeter. Three work, but any
   3 points lie on a plane, so the flatness check has nothing to check. Keep them at least 5 cm
   apart and not in a line. Center them on the tweeter: the tool reports the marker centroid as
   the speaker's position.
2. Select the markers and create a rigid body. Name it `spk<N>` or `speaker<N>`, where N is the
   speaker's `index` in the layout. `spk07`, `Speaker_7` and `SPK-7` all map to index 7. For any
   other name, pass `--map <name>=<index>`.
3. Stream rigid bodies. The tool asks the server for the model definitions, so it needs the
   server's address, not only the multicast group.

You do not need to know how Motive oriented the body. The aim is the normal of the best-fit
plane through the body's markers, and the tool takes the side that faces the layout's listening
point. A body whose markers cannot sit on the baffle can still give an aim: `--axis x,y,z`
names the body-local axis that points along the speaker's axis. Then the body's orientation
matters, so prefer the baffle.

### Run it

```
bwa_speaker_survey cave_layout.json --server 192.168.1.10
```

It collects 3 seconds of frames (`--seconds`), averages each body and prints the report.
`--local` pins the network interface, `--multicast` the group, and `--natnet M.m` the
bitstream version when the handshake fails. Every other rigid body is listed once as ignored,
so a typo in a name shows.

`--baffle-offset-m x` says the layout's point for a speaker sits x m behind the baffle. The
acoustic survey measures acoustic centers, so set this to compare like with like.

### Read the report

Per speaker:

- `position`: optical against layout, and the distance in mm.
- `aim`: optical against layout, and the angle in degrees. It also says whether the layout's aim
  is explicit or the default, toward the listening point.
- `markers`: the count, the RMS distance off their plane, and the verdict. More than 3 mm off
  the plane, or markers in a line, gives no aim. The position is still reported.
- `spread`: how still the body was. Over 2 mm or 0.5 degrees refuses the speaker.

These numbers are in Motive's frame. The frame block below says whether that is the room's.

Then the frame agreement: one rigid fit, rotation and translation, no scale, from the optical
positions onto the layout's. It needs 3 speakers. With 2 you get only the distance check.

- `rotation` and `translation`: how far Motive's frame is from the room frame. Under 0.5 degrees
  and 10 mm reads as agreement.
- `residual`: what the fit cannot explain. Large means a speaker is not where the layout says,
  and the per-speaker lines under "after the fit" show which.
- `handedness`: `OK`, `MIRRORED` or `UNDETERMINED`. The tool fits a second time with Motive's x
  axis flipped, and a mirrored frame fits that one better. Speakers that all lie on one plane fit
  a mirror exactly as well as the truth, so then only the aims can decide. When they cannot
  either, it says `UNDETERMINED`. Add a speaker off that plane.

If the frame disagrees, fix it in Motive (ground plane, axis convention) and re-run. The engine
takes poses unchanged.

### Write the aims

The tool only reports by default. `--write out.json` copies the measured aims of the matched
speakers into a copy of the layout. Every other field is kept. `--fields position` or
`--fields aim,position` writes positions too. That replaces the acoustic survey's position with
the optical one, so do it only when you trust the optical one more. It refuses a mirrored frame
and warns when the frame disagrees, because the values are in Motive's frame.

### Rehearse without Motive

```
bwa_speaker_survey cave_layout.json --simulate --sim-yaw 2 --sim-offset 0.05,0,0 --sim-aim-error 3
```

This builds Motive's packets from the layout and runs them through the same parsers. The knobs:
`--sim-speakers` (which speakers are visible), `--sim-aim-error` (on the first), `--sim-yaw` and
`--sim-offset` (Motive's frame against the room's), `--sim-mirror`, `--sim-noise-mm` (marker
placement), `--sim-jitter-mm` (per frame) and `--sim-natnet`. It checks its answer against what
it injected and prints `simulate PASS` or `FAIL`.

Unverified against a live Motive: the socket path, and the assumption that Motive sends marker
offsets in the body's own frame. For a body created in place on a fixed speaker the two readings
agree.

## Live aiming (`--live N --zylia`, the Aim tab)

Some boxes you cannot see: they hang behind the acoustically transparent screens, or overhead.
Live aiming turns the ZM-1 into an aiming instrument for them. It sweeps ONE speaker over and
over, captures the 19 capsules, and prints a line per sweep while an installer turns the box and
someone at the calibration PC reads the numbers out.

```
bwa_calibrate --layout cave_layout.json --live 7 --zylia --survey s.json --input 26 --latency 20.6 --aim-ref 3
```

`--mic` is the array center and defaults to the layout's listening point, which is where the
ZM-1 sits on the rig (4.75 ft). `bwa_calib_view` has the same loop in its **Aim** tab, on a
worker thread, for the speakers of layout A: a speaker picker, a big "dB below peak" number
readable from a ladder, a meter with a peak-hold line, the angle estimates, the position, and a
3D view with the layout's position and aim, the ZM-1, and the measured direction and position.

### What one line says

```
  #1   pos   -8.9   -9.6   -1.0 mm (|d|  13.2)  dir 0.36 deg  dist 2.085 m (+0.3 mm) | tilt -1.35 dB  peak -1.35  below 0.00 dB | off-axis ref 25.6 deg [21.5-29.1], file 25.7 deg [21.6-29.3] | layout 0.0 deg | true 25.0 deg
```

That line is simulated, which is what the closing `true` field says; the rig prints no truth.

- **Position**: where the box is against its layout `position`, in mm, plus the direction error
  in degrees and the distance. Direction comes from the capsule arrival differences
  (`zylia_doa`), distance from the arrival at the array center minus the system latency
  (`zylia_live_position`). The distance is exactly as good as the latency: 20 µs of latency
  error is 6.9 mm. Pass `--latency` (a loopback in meters at c) or `--ref <spk> <m>` (one taped
  distance, swept once before the loop). Without either you get the direction only.
- **Tilt, peak, below**: the direct-sound high-to-mid ratio (the gated `band_direct`, 10 kHz up
  against 3 to 10 kHz, pooled over the capsules by `zylia_pressure_proxy`), the highest tilt seen
  since the last reset, and how far this reading sits under it. These are NOT the `--check-aim`
  bands (1 to 3 kHz against 3 kHz up): on the 4410A those fall only 0.25 dB by 15 degrees, under
  what a reading is good to, so a meter on them cannot find the peak. The live pair falls 0.51 dB
  at 10 degrees and 1.00 dB at 15 (`CALIB_LIVE_*_HZ` in `calib.h`).
- **Off-axis**: the estimated angle between the box's axis and the line to the ZM-1, from a
  stored reference (`ref`) and from the vendor file (`file`), each with its bracket.
- **Layout**: the angle the layout's own `aim` makes with the line to the ZM-1, the target.

Keys: `r` stores this reading as the on-axis reference, `p` resets the peak, any other key
stops. `--sweeps N` stops after N readings.

### What the angle is, and what it is not

One mic position sees each speaker from one bearing. So it measures how far the axis is off
the line to the mic, a **magnitude**, and never which way the box points: every axis on a cone
around that line reads the same. The tool cannot tell you "turn left". Nothing here fits an axis;
`--check-aim` does that from several placements.

Two readings, for two jobs:

- **The peak meter** needs no calibration. Treble falls off axis, so the tilt is highest when the
  box points at the ZM-1. Turn until "below" reads 0. Whatever sits on the fixed speaker-to-mic
  path (the screen, the ZM-1's own response, the gate) is the same at every aim, so it shifts the
  tilt but cannot move the peak.
- **The angle** says how far there is to go. It takes the measured tilt minus the tilt the same
  path shows at 0 degrees, and inverts the model's tilt-versus-angle curve (`calib_aim_invert`).

The curve is flattest near 0. On the 4410A, with the live bands, the tilt falls 0.18 dB at 5
degrees, 0.51 dB at 10, 1.00 dB at 15 and 3.2 dB at 30. The estimate therefore comes with a
bracket for ±0.3 dB of tilt (`CALIB_LIVE_TILT_TOL_DB`, about the ZM-1 proxy's leftover direction
dependence), and a reading whose bracket reaches 0 prints **"on axis (under N deg)"** instead of
a number: on the 4410A, under about 7 degrees. The peak is broad for the same reason. To land on
it, find the two sides where "below" reaches 0.5 dB, about 10 degrees each way, and split the
difference.

### Where the 0 degree tilt comes from

- **The file.** The Genelec CF2 carries the absolute on-axis response per band, which
  `clf_to_json.py` exports as `on_axis_db` (see [layout-schema.md](./layout-schema.md)). Its tilt
  over the same bands is the file's 0 degree tilt: +0.04 dB for the 4410A. It knows the speaker
  and nothing else: not the ZM-1, not the gate, not the screen.
- **A reference speaker, better on the rig.** Sweep a speaker you KNOW points at the ZM-1 and
  store its tilt: `--aim-ref <spk>` before the loop, `r` during it, or **Store reference** in the
  tab. The run prints the value, and `--aim-ref-db <dB>` reuses it later. It absorbs the ZM-1's
  own response, the gate and the average effect of the screen, which the file cannot.

The workflow with the optical check: run `bwa_speaker_survey` first. The four or so speakers
the cameras see come back with measured aims; take one it reports within a degree or two of the
layout aim toward the listening point, and use it as the reference. If none is visible, peak any
reachable box by eye and by meter, and store that. Then aim the hidden boxes against it. Take the
reference from a speaker that passes through the same kind of screen as the ones you aim.

### Update rate

The live mode plays its own shorter sweep: 0.5 s instead of 1.5 s, the same 20 Hz to 20 kHz, plus
a 0.25 s tail (`CAL_LIVE_*` in `calib_capture.h`). The tail holds the ZM-1 chain's 60 ms of
latency with 190 ms to spare, and nothing past the direct-sound gate is read. That is 0.75 s of
capture per reading plus the 19 deconvolutions, which run on up to 8 threads. A whole simulated
reading, synthesis included, takes about 0.3 s on the development machine, so expect 1 to 1.2
readings a second on the rig; nobody has timed it there. The shorter sweep costs 4.8 dB of
signal-to-noise against the full one.

### Accuracy, in simulation

`--simulate` synthesizes each reading with the live sweep and a box whose TRUE position and aim
differ from the layout's: `--sim-move dx dy dz` (m) and `--sim-aim-steps a,b,...` (degrees off
the layout aim, one per reading). The simulated box carries the file's on-axis response.
`--sim-screen <dB>` puts a screen's high-frequency loss (a shelf above 4 kHz) on every path out
of every box, which is the case the file cannot see. The numbers below are the 26-speaker example
with the 4410A model, the ZM-1 at (0, 1.448, 0), speaker 7 turned, and speaker 16 as the
reference:

| true angle | below peak | reference | file | file, 3 dB screen |
| --- | --- | --- | --- | --- |
| 0 | 0.00 dB | on axis (under 7) | on axis (under 8) | 12.6 deg |
| 5 | 0.15 dB | on axis (under 9) | on axis (under 10) | 14.2 deg |
| 10 | 0.47 dB | 9.4 deg | 10.1 deg | 16.8 deg |
| 15 | 0.95 dB | 14.4 deg | 14.9 deg | 20.4 deg |
| 25 | 2.31 dB | 24.4 deg | 24.7 deg | 29.4 deg |
| 40 | 4.39 dB | 39.0 deg | 39.3 deg | 45.9 deg |

Behind the screen the reference reads the same as without it; the file does not. A 1 dB shelf
moves the file estimate by only 1 to 3 degrees, because both live bands sit mostly above the
shelf's corner, so a flat screen barely tilts them. A real screen's loss that rises with
frequency tilts them more, which is why the reference is the rig's number. The simulated room
(`--sim-room`, absorption 0.3) moves no entry: its first image arrives after the gate closes, so
the gate removes it. A real box close to a wall gets a shorter gate and a coarser mid band, which
this room does not reproduce. In every case the peak lands on the reading where the box points at
the mic.

Position: over seven speakers the direction reads 0.0 to 0.6 degrees off the truth, which is
up to 20 mm across at 2 m, and the distance within 1 mm. The limit is the sub-sample peak of
each capsule's impulse response, about 1 µs. A 10 cm move reads back as 89 to 109 mm along the
right axis. On exact arrivals (the `zylia` ctest) the same move comes back within 1 mm. So read a
position delta under about 3 cm as "where the layout says".

The `calibrate_live_zylia` ctest pins the 10 cm move, the 25 degree estimate with the reference,
the file's larger error behind the screen, and the peak at 0 degrees. The `aim/sim_live` test in
`calib_view --tests` turns a simulated box away and back through the real UI.

Unverified on hardware: all of it, the short sweep through the ASIO shell included. The
simulator's capsules are free-field omnis on no sphere, so the ZM-1's own scattering, and how
well a reference taken in one direction carries to another, are not in these numbers.

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
button loads the result into Diff for review before you accept it. Its directivity correction
follows the CLI's `--mic` rule (see "Speaker directivity" above): off until you set the mic. The **Zylia tab** (live
clap-DOA, see "Bring-up" above) covers the ZM-1, and the **Aim tab** runs live aiming (see
"Live aiming" above).

`bwa_calibrate` remains the headless CLI over the same code: scriptable, and the only place for
the multi-placement modes (`--localize`, `--zylia`, `--check`, the omni `--live`), for trims measured with
the ZM-1 (the Capture tab reads one omni input), for `--verify` and for `--aim-sheet`.
`bwa_calib_view --tests [filter]` runs its imgui_test_engine suite, wired into ctest as
`calib_view`.

## What feeds the engine

`cave_layout.json` carries per-speaker `position`, `gain_db`, `delay_ms` (consumed by `dbap.c` +
`align.c`), and optionally each speaker's `aim` plus one `directivity` model (consumed by the
tracked directivity compensation in `rt.c` + `align.c`). Model the room itself, if you want simulated reverb, in Steam Audio as geometry +
material absorption (`steam_reflect.c`) tuned to creative intent, **not** to the measured RT60.
