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
`bwa_speaker_survey --require-frame` (Motive's frame) → `calibrate --localize --zylia` (positions) →
`calibrate --capsule-survey` (the ZM-1's capsule table) → `calibrate --live N --zylia` (aim the hidden
boxes) → `calibrate --zylia --trims --room` (trims and the room report) → `calibrate --verify` from
the same placement → `calibrate --room-eq-grid rows.txt`. Then the engine loads the layout. The
rig-day procedure, with pass criteria: [hardware-validation.md](./hardware-validation.md).

**Keep one file.** What the layout tool saves is the plan. The first survey that writes
positions copies the plan into each speaker's `plan_position` and `plan_aim` before it overwrites
`position` and `aim`, and no later survey touches it. So the same `cave_layout.json` carries the
plan and the as-built from then on, the engine renders the as-built, and live aiming aims at the
plan. The rule: [layout-schema.md](./layout-schema.md#plan-versus-as-built). The Session tab is
the one exception: it never writes the plan file, and each step writes a new file in the session
folder instead (see "The rig-day session" below). Its `as_built.json` carries the plan the same way.

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

## Sweep quality: the window, the SNR floor, the re-sweep

A sweep cannot tell its own sound from a cough, a door, a projector fan spinning up, or a person
talking. Each speaker used to be swept once, and the arrival was the strongest tap anywhere in the
IR. One loud event during a sweep could move a level, an EQ band, or, rarely, the arrival, and
nothing said so.

But every sweep is the tool's own. It knows when the sweep plays, and roughly when each speaker's
sound must arrive: the system latency plus the distance over c. Every mode that sweeps uses that.
The thresholds below are **provisional**: nothing here has met the rig. They are named constants in
`src/calib/calib.h` (`CALIB_WIN_*`, `CALIB_SWEEP_*`) and `src/calib/measure.h` (`MEASURE_NOISE_*`).

### The expected-arrival window

The arrival is searched only between

```
lo = latency - early + max(0, d - margin) / c
hi = latency + late  + (d + margin) / c + 2 ms
```

where `d` is the distance from the mic to where the layout says the speaker stands. The 2 ms
(`CALIB_WIN_SPK_S`) is the box's own delay: a crossover's group delay puts the IR peak after the
onset. A capture whose strongest tap lies **outside** the window is flagged: something other than
this speaker was louder (a second speaker on a mis-patched output, a wall reflection stronger than
a direct sound the box fires away from, a door). The run prints where that tap was and by how much
it beat the arrival, and re-sweeps.

The latency, by what the run knows:

| Source | Window |
|---|---|
| simulate | the simulator's own latency, exact |
| `--latency` (from `--localize` or `--capsule-survey`), or live aiming's `--ref` or `--ref-speakers` | the measured value, +/- 1 ms (`CALIB_WIN_LAT_KNOWN_S`) |
| nothing measured | the ASIO driver's reported loop, a hard lower bound, and up to 150 ms later (`CALIB_WIN_LAT_DRIVER_S`): the driver does not report the converters, the analog path or the ZM-1's Dante Via leg, about 60 ms on the rig |
| no driver number either | no window: the whole IR, as before |

The distance margin, by mode:

| Mode | Margin | Why |
|---|---|---|
| trims, `--verify`, `--room-eq-grid` | 0.10 m for a speaker a survey placed, 0.50 m for one at its plan | a survey records `plan_position` when it writes `position`, so a speaker carrying one has been measured; 0.10 m covers the survey plus a 1% error in c at 4 m (`CALIB_WIN_SURVEYED_M`) |
| `--localize`, the `--zylia` position survey, `--capsule-survey`, `--check`, live aiming | 0.50 m (`CALIB_WIN_PLAN_M`) | these modes find or move a position, or start from a taped guess |

The ZM-1 adds 5 cm (`CALIB_WIN_ZM1_M`), its radius, since every capsule is searched with the
center's window. `--verify` moves each speaker's window by its own `delay_ms`, which the staged
sweep carries. `--window-m m` sets the distance margin for every mode.

**A window cannot bias a clean measurement.** It only restricts where the peak is searched. When
the strongest tap lies inside the window, the search returns the same tap the whole-IR search
would, and the parabolic refinement reads the same three samples, so the arrival is the same to the
bit (`test_measure` pins this). The answer changes only on a capture the window also flags, and a
flagged capture is re-swept, never used. That is why the position-finding modes can carry one: a
speaker more than 0.5 m from its plan is flagged with its arrival printed, not mis-measured, and
`--window-m` widens it.

### The noise floor

Each IR's peak is held against its own noise floor (`MeasureResult.snr_db`). Where the floor is
read follows from how `measure.c` deconvolves. The FFT is at least `ncap + nref` long, so the
deconvolution is linear and nothing wraps onto the speaker's response. The IR's positive lags hold
the direct sound and the room; the exponential sweep's harmonic-distortion products land at
**negative** lags, which this deconvolution puts at the end of the buffer, past `ncap`. The floor is
read from two positive-lag regions only:

- **before**: from lag 0 to 2 ms (`MEASURE_NOISE_GUARD_S`) before the window opens. Nothing of
  this speaker can arrive earlier, and the guard keeps the arrival's own band-edge ringing out.
- **after**: from 0.3 s (`MEASURE_NOISE_SKIP_S`) past the window's close to the end of the
  capture. The reflections and the reverberant tail have died there. Past `ncap - nref` only the
  sweep's low end overlaps the capture, so this region is a narrower band, and it is where a
  transient late in a capture shows.

No negative lag is used: a harmonic product of some order can sit at any of them. The floor is the
RMS of the **loudest** 20 ms block (`MEASURE_NOISE_BLOCK_S`) in the two regions, so one cough in an
otherwise quiet capture reads as the cough. Live aiming's 0.25 s tail leaves no "after" region, so
it reads the "before" one, which the ZM-1 chain's 60 ms of latency makes long.

A noise-free simulated capture reads 75 to 78 dB anechoic (the deconvolution's own residue) and 67
to 76 dB with `--sim-room` (the simulated late tail). `CALIB_SWEEP_MIN_SNR_DB` is **40 dB**, 27 dB
under the lowest simulated value. The simulator has no noise floor and a rig does, so a real
capture will read less than these; how much less is the first thing to measure on rig day.

### The re-sweep and the agreement rule

`calib_measure_speaker` (`examples/calib_capture.cpp`) is the one place this lives, so
`bwa_calibrate` and `bwa_calib_view`'s Capture tab cannot measure differently:

1. A sweep outside its window or under the SNR floor is re-swept, with the reason printed.
2. For the trims and `--verify`, a value counts only once **two clean sweeps agree**: arrivals
   within a sample and levels within 0.2 dB (`CALIB_SWEEP_AGREE_SAMPLES`, `CALIB_SWEEP_AGREE_DB`).
   Every clean sweep is kept, so a third that agrees with the first settles it whatever the second
   was. The later sweep of the pair is the result.
3. After `CALIB_SWEEP_MAX_TRIES` (5) sweeps with no result, the run stops and writes nothing.

So a clean trim run costs two sweeps a speaker, and the run's last line says how many it took:

```
trims: 55 sweep(s) for 26 speaker measurement(s), 3 of them re-swept (a rejected or disagreeing sweep)
```

The SNR floor and the agreement rule catch different things, and in simulate the difference is
large. A 250 ms noise burst at the sweep's own level moves that sweep's level by 3.4 dB and leaves
its IR SNR at 47 dB: the level is a mean of `|H|` over every bin, while the IR peak gathers the whole
sweep's energy into one tap, so the floor reads a level error long after it has stopped being
small. The agreement rule catches that burst; the SNR floor catches the gross failures (a dead
channel, a wrong input, a capture swamped by noise). The window catches what neither sees: an
impostor arrival louder than the real one.

`--localize`, `--capsule-survey`, `--check` and the grid rows take one clean sweep a speaker, with
the window and the floor. The `--zylia` position survey re-sweeps on the rig the same way; in
simulate it computes its arrivals analytically and sweeps nothing.

### Rehearse it: `--sim-interferer`

```
bwa_calibrate --simulate --sim-room --mic 0.6 1.2 0.4 --sim-interferer 3,9,30:0.9:0:noise
```

puts an interferer on the 1-based captures listed, counted by the simulator itself (a ZM-1
capture's 19 rows are one capture), so its truth never comes from the code under test. The fields
after the list are when it sounds (seconds into the capture), its RMS at the mic in dB re a
unit-sensitivity speaker's sweep at 1 m, and its kind: `click` (2 ms), `noise` (250 ms) or `sweep`
(the capture's own sweep from another point: a mis-patched second speaker). It stands about 1.5 m
from the mic, off any speaker. The `calibrate_sweep_quality` ctest runs that command and requires
the clean run's trims; a stray sweep louder than the speaker on all five of one speaker's sweeps,
which must stop the run with nothing written; and live aiming with one contaminated reading.

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
  passes through the screens. Pair it with the OptiTrack you already have for head tracking: put
  the mic on a tracked stand and add `--track <body>`, so the tool records each MEASURED mic
  position (see "Placing the ZM-1 with the tracker" below), and let acoustics locate the
  screen-hidden speakers. Spread the mic positions out and make them
  non-coplanar ([GDOP](./glossary.md#gdop)): clustered points amplify error. Cross-check against
  the install drawings. The writeback records the plan the first time (see "Keep one file"
  above) and prints how many speakers it recorded one for. A speaker whose solve fails (the mic
  positions are degenerate for it) keeps the position the file already had, and the run says so.

  With **`--zylia`** the mic is the ZM-1, and each range is the arrival at the array CENTER, pooled
  from the 19 capsules (the pressure proxy's `zylia_center_arrival`, see "The ZM-1 as the trim mic").
  Do not range with one capsule: it sits 49 mm from the center, so its arrival is early or late by up
  to 143 µs depending on where the speaker is. That bias changes from speaker to speaker and row to row,
  so the trilateration cannot fit it out as latency. The center arrival does not change when the array
  turns, so this needs no capsule survey and no orientation: the rows are the array center, tracked or
  typed. The channel order still matters a little, up to 15 µs (5 mm of range), so run `bwa_zylia_probe`
  first. `--check-aim` works here too and reads the pooled direct-sound tilt. In simulation, with the
  stand turned 30° and the array turned 40° inside its mount, 5 tracked rows put 24 of the 26 example
  speakers within 3 mm and the worst 7 mm off; ranging with one capsule instead put every speaker about
  5 cm off. The `calibrate_zylia_localize` ctest pins 12 mm worst and 3 mm RMS.

  The solved latency gets a free sanity check: at open, the capture shell logs the driver's own
  `ASIOGetLatencies` numbers (out + in = the **digital** half of the loop; the Digiface reports its Dante
  buffering there). After the solve the CLI prints solved-vs-driver with the residual. The
  residual is what the driver *can't* see, so it is positive. **Negative is physically impossible**
  (wrong device, sample-rate mismatch), with either mic. How large is fine depends on the mic:
  - An omni: the residual is DAC/ADC conversion and analog, a few ms. Over 20 ms the run warns:
    an unexpected buffer, so check the Dante latency setting.
  - The ZM-1 (`--zylia`): the driver does not report the Dante Via leg either, about 60 ms for the
    whole chain (see "Getting the ZM-1 onto Dante"). Tens of ms is a correct run, and the line says
    so. The run warns only past 150 ms, the allowance the arrival window grants past the driver's
    loop (`CALIB_WIN_LAT_DRIVER_S`, see "Sweep quality"): an arrival that late fell outside every
    sweep's window.

  `calib_latency_check` holds the rule, and both bounds are provisional. The solved value stays
  authoritative: the driver's numbers are nominal, the sweep measured reality.
- **`--zylia`**: says the mic is a ZM-1 (its 19 capsules start at `--input`, `--mic` is the array
  center). The mode comes from the other flags. Alone, `--zylia` is the same self-survey as
  `--localize` from **one** mic placement: direction from arrival-time differences, distance from a
  known latency (`--latency`, `--ref` or `--ref-speakers`), and its writeback keeps the plan the way `--localize`'s
  does. See "Zylia ZM-1: full 3D from one placement" below. With
  `--trims` it measures the trims, and with `--verify` it runs the second pass, both through a
  pressure proxy: see "The ZM-1 as the trim mic" below. With `--room-eq-grid rows.txt` it reads
  the grid, and needs no `--trims`. With `--localize rows.txt` it is the multi-placement survey
  above, ranging with the array center's arrival. With `--capsule-survey out.json` it measures the
  capsule table from the speakers: see "The speaker-sweep capsule survey" below.
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
- **`--simulate --sim-truth f.json`**: the simulated speakers stand where `f.json` says (its
  `position` and `aim` per index), not where `--layout` says. Without it the simulator builds
  every capture from the layout it was given, so a survey can only measure the plan back. With
  it, a rehearsal has an as-built that differs from its plan, and a run that read the wrong file
  lands off the truth. Nothing the run solves or writes reads `f.json`. The room and the
  directivity model stay `--layout`'s. Refused with `--live`, which moves its own box.
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

- **`--room-eq-grid [rows.txt]`**: **tracked room EQ**, the moving-listener answer to `--room-eq`'s modal
  half (Lindfors/Liski/Välimäki, JAES 2022, adapted to the tracked CAVE). One point can't room-EQ a
  roaming listener; a **grid of points can**. Below ~200 Hz the room's mode *frequencies* are fixed
  properties of the room. Only how strongly each mode reads varies with position, and that varies
  smoothly on the half-meter scale of LF wavelengths.

  Workflow: cover the working area at ear height, ~0.5–1 m spacing, up to 16 positions
  (`BWA_RQ_GRID_MAX`). Each position measures its modal cuts (`measure_room_cuts`, same
  30–200 Hz band and 12 dB depth cap as `--room-eq`) and **merges** them into the layout's
  top-level `room_eq_grid` (`calib_room_grid_merge` → `calib_write_room_eq_grid`): per-position fcs
  within ~8% are the same mode, so each speaker gets ONE shared `fc`/`q` ladder with per-position
  depths (0 dB where a position didn't see the mode). A position within 5 cm of an existing entry
  replaces it. See [`layout-schema.md`](./layout-schema.md) for the format. Two ways to run it:

  - **A rows file** (`--room-eq-grid rows.txt`, one `x y z` per line, like `--localize`): one
    run steps through every row. With `--track` the tool waits at each row with `--localize`'s
    loose gate (100 mm, or `--place-tol-mm` if wider), and the row's MEASURED center becomes its
    grid key, because the key is where the engine interpolates from, not where you meant to stand.
    Without a tracker it asks you to place the mic and press Enter, and the row is the key. The
    bump check runs on every capture of every row. The grid is written once, after the last row,
    so a bump or a timeout writes nothing. This mode writes the grid only, no trims. The
    `calibrate_grid_rows` ctest runs it on the simulated tracked stand; it is unverified on
    hardware and against live Motive.
  - **One position per run** (`--room-eq-grid --mic x y z`): `--mic` is the grid key. This form
    also writes trims measured at that mic, which is rarely what you want off the listening point.

  Before any sweep, the rows mode refuses more than 16 rows, rows that would take the grid the
  file already holds past 16 positions, and rows closer than 5 cm. With `--track` the minimum is
  5 cm plus twice the gate tolerance (250 mm at the default), because each measured key may land
  a whole tolerance off its row, and two keys within 5 cm replace each other.

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
bwa_calibrate --layout cave_layout.json --mic 0 1.448 0 --zylia --input 0 --verify
```

In `bwa_calib_view`, pick **verify** on the Capture tab, or press **Verify this result** after a
trim run there. Both mics work, and the residuals land in the tab's table.

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
speaker with no `aim` points at it. When the file carries a plan (`plan_position`, `plan_aim`; see
[layout-schema.md](./layout-schema.md)), the sheet is built from the plan, because that is where you
mount the box to; after a survey, `position` and `aim` are where it already is. The run says how many
speakers it read a plan for. One CSV row per speaker, one plain header row, no comment lines,
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
next section). It sweeps each speaker, deconvolves all 19 capsules (measure.c), times them against
each other by cross-correlation, and hands the arrivals to `zylia_localize`.

**How the capsules are timed.** Each capsule's own impulse-response peak, fitted with a parabola,
is biased by where the true peak falls between samples, and 19 independently biased arrivals cost
the direction about 1 degree. So every ZM-1 sweep path (this survey, `--zylia --trims` and
`--verify`, and live aiming) keeps a short window of each capsule's impulse response and
cross-correlates it with the strongest capsule's (`zylia_ir_tdoa`, the sweep counterpart of the
clap path's `zylia_tdoa`). That compares the same waveform shifted, so the correlation peak is
symmetric, and the peak is refined on a windowed-sinc interpolation rather than a parabola. The
absolute time comes from the reference capsule's own peak on the same interpolation: a constant of
the speaker's impulse shape, which the latency calibration (`--ref` or `--latency`) measures with
the same estimator and cancels. In the `zylia` ctest, over 96 directions, the direction lands within
0.02 degrees of the truth, against 1.0 from each capsule's own peak, and the absolute time varies
by 0.07 µs (0.02 mm) with direction. What simulation cannot show is the rigid sphere: sound
bends around it, so the arrival-time differences are not quite a free-field plane wave's, and that
error lands in the capsule geometry, which the capsule survey measures with this family of
estimator. `--mic x y z` is the array **center**. Positions go back
into `cave_layout.json`. Two flags carry the physics the tool cannot know:

- **`--survey <file>`**: a capsule survey, from `--capsule-survey` (see "The speaker-sweep capsule
  survey") or calib_view → Zylia tab → Capsule survey. Without one the tool trusts the built-in table, and an unpinned channel order or yaw rotates the
  whole recovered layout. The tool warns, but it cannot check. A body-frame survey needs the
  tracker to re-aim it: without `--track` the tool refuses one, and with `--track` this survey
  requires one (see "Placing the ZM-1 with the tracker").
- **`--latency <m>`** or **`--ref <spk> <m>`**: the distance calibration. `--latency` is a
  loopback-measured round trip in meters at c. `--ref` solves it from ONE tape-measured
  center→speaker distance instead (that speaker's mean arrival, wavefront-tilt corrected), so a
  tape measure replaces the loopback rig. The ref speaker's reported `dist` should then read back
  the taped value. Given neither, the run prints directions and **refuses the writeback**: every
  distance would carry the full system latency radially. The tool cross-checks the solved latency
  against the driver's digital loop the way `--localize --zylia` does: below it is physically
  impossible, and above it the capture chain legitimately adds tens of milliseconds the driver never
  reports. A clap loopback on the rig measured about 60 ms, which is about 20 m at c. A solved
  latency several times the array's own extent is the expected reading, not a fault. It warns only
  past 150 ms, the arrival window's allowance.
- **`--ref-speakers <list>`**: the latency from several speakers whose positions you trust, with
  no tape. See the next section. `--ref` and `--ref-speakers` both set the latency: pass one.

#### The latency from measured speakers (`--ref-speakers`)

`--ref` needs a tape. `--ref-speakers 2,6,18-19` takes the distances from the layout instead, for
3 or more speakers:

```
bwa_calibrate --layout surveyed.json --zylia --input <first> --mic 0 1.448 0 --ref-speakers 2,10,18,24
bwa_calibrate --layout surveyed.json --live 7 --zylia --input <first> --ref-speakers 2,10,18,24
```

For each listed speaker, the distance is `|position - the array center|`, the center being the
tracked measured one with `--track`, else `--mic`. Its latency is its center arrival (the pooled
`zylia_center_arrival`, the same reading `--ref` takes) minus that distance over c. The run prints
one line per speaker, then the latency it uses, which is the **median**, and the spread. Here is the
second command, simulated, with speaker 10 set to play 0.5 ms late (`--sim-speaker-latency 10,0.5`):

```
  spk  2: 2.568 m, center arrival 18.1557 ms, latency 10.6676 ms,    -0.1 us from the median
  spk 10: 1.501 m, center arrival 15.5440 ms, latency 11.1682 ms,  +500.4 us from the median   FLAGGED
  spk 18: 2.158 m, center arrival 16.9605 ms, latency 10.6678 ms,    +0.0 us from the median
  spk 24: 2.158 m, center arrival 16.9605 ms, latency 10.6678 ms,    +0.0 us from the median
live: latency 10.6678 ms (3.6590 m at c), the median of 4; spread 500.5 us (max - min), 0.1 us without the flagged; --latency 3.6590 carries it to the next run
```

**A speaker more than 0.1 ms (34 mm at c) off the median is flagged** (`REFSPK_FLAG_S`), with a
warning that names it. That is a box whose Dante latency setting differs from the others, or whose
layout position is wrong along the line to the array. The median stands without it, so the run goes
on, but anything the run measures from that speaker carries the same error. Why 0.1 ms: in simulation
a correct set agrees to 0.5 µs (the center arrival reads about 1 µs long), and on the rig 1 cm of
position error is 29 µs, so a correct set sits well under it. A different Dante receive latency is a
whole setting step, 0.25 ms or more. When the unflagged speakers are not a majority, no latency is
used, nothing is written, and the run exits 6.

**The positions must be MEASURED.** A plan position puts its placement error over c into that
speaker's latency, 29 µs per centimeter. Measure them first, with `--localize --zylia`, or optically
on the camera-visible boxes with `bwa_speaker_survey --write out.json --fields position` (see "Write
the aims"). The run warns about a listed speaker with no `plan_position` (no survey ever wrote its
position), and about one whose `plan_position` equals its `position` (no survey moved it; an aim-only
`--write` records the plan too). The optical position inherits any error in `--baffle-offset-m`, the
shift from the baffle back to the acoustic center: 1 mm of it is about 3 µs of latency.

**The center matters as much.** Every listed distance is measured from it, so a center 1 cm off
moves the latency by up to 29 µs, less where the speakers sit on opposite sides. The `--zylia` survey
requires `--mic` (or `--track`). Live aiming defaults to the listening point, and says so.

Where it works: the `--zylia` position survey and live aiming (`--live N --zylia`). The capsule survey
refuses it: it already trusts every swept speaker's position and solves the latency with the center,
and a latency read at the assumed center would tie the acoustic center to the one it checks. Pass it
`--latency` from `--localize`. `--localize` solves its own. The Session tab does not pass it either:
its localize step pins the latency from moving mic rows, and every later step gets that.

`--sim-speaker-latency spk,ms` (simulate only, repeatable) makes one speaker play that much later
than the rest, so the flag has a known outlier. The `calibrate_live_zylia` ctest pins the latency to
within 5 µs of the simulator's own on 4 and 6 speakers, one speaker 0.5 ms late flagged alone with
the median unmoved, two bad of three refused, and the warnings for unmeasured positions.

`--zylia --simulate` runs the identical solve + writeback off-hardware from synthesized arrivals
and recovers every position exactly. The capture shell itself is rig bring-up code like the rest
([hardware-validation.md](./hardware-validation.md), Stage 2).

### The ZM-1 as the trim mic (`--zylia --trims`, `--zylia --verify`)

On the rig the ZM-1 is the only measurement mic, so it measures the trims too. Put the array's
center AT the listening point (`--mic` is the center, and here it is the 4.75 ft point): the
delays equalize arrival at the mic, while the engine treats the trims as aligned at the
listening point, and tracked alignment is identity only there. The trim run warns when the mic
is more than 5 cm from it. With a tracked stand, `--track <body>` puts the center there for you
and measures where it ended up: see "Placing the ZM-1 with the tracker".

```
bwa_calibrate --layout cave_layout.json --zylia --trims --survey s.json --input 0 --mic 0 1.448 0
bwa_calibrate --layout cave_layout.json --zylia --verify --survey s.json --input 0 --mic 0 1.448 0
```

`bwa_calib_view`'s Capture tab runs both passes with the **ZM-1** mic selected, through the same
code (see "Reviewing the results").

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
that already owns the array's outputs**. One driver, one clock domain, sample-locked:
`calib_capture.cpp` went from "N outs + 1 mic input" to "N outs + 19 inputs", a parameter, not an
architecture (`calib_asio_open_multi`; the omni modes are its `nin = 1` case).
([Danowski's write-up](https://blog.przemekdanowski.com/connecting-zylia-zm-1-to-dante-network/)
is the recipe; Dante Via has a trial, so you can prove the route first.)

What it buys, beyond unblocking the sweep:

- **Absolute latency, so distance becomes real.** `zylia_localize`'s range is only as good as the
  latency you feed it, and the array is far too small to self-calibrate that. Sample-locked
  playback and capture make the round-trip a measurable constant, so the ZM-1 delivers full 3D
  **positions** from one placement, not just directions.
- **Sweeps instead of claps** for the capsule survey (`--capsule-survey`): far better SNR,
  sub-sample arrivals off the deconvolved IR peak, every speaker a known source for free.
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

24 out + 19 in is **43 channels** (55 with 36 speakers), so confirm the Digiface offers that many
at your rate. Dante endpoints commonly halve their channel count at 96 kHz; calibrate at 48 kHz.

**Dante Via presents the ZM-1 as 20 input channels, not 19.** The 20th carries no capsule and
appears to be inactive. Only the first 19 of the block are capsules, in order, so `--input` still
points at the first capsule and the tool still reads 19 consecutive inputs. Budget the routing
for 20 channels and leave the last one unpatched.

**On the rig, the ZM-1 takes the Digiface's first 20 inputs.** The first capsule is ASIO input 0,
so every command takes `--input 0` (`--mic-in 0` for `bwa_validate`), and `bwa_zylia_probe` reads
inputs 0 to 18 only. Patch it anywhere else and the probe shows the wrong channels.

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

**Running it** (calib_view's Zylia tab → *Capsule survey*): tell the tool where the array is, then clap
from ≥ 6 positions it knows.

- **The array center.** Track the ZM-1's stand in the Placement panel (it is at the top of the
  section too) and every clap is taken against the stand's measured center. The stand must read still,
  in position and in orientation, or the clap is refused. A stand that turns more than 0.3° after the
  first clap gets its claps refused too, because a survey holds one orientation. Untracked, tape-measure
  the center and type it.
- **The clap positions.** Typed: set the clap position, arm, then clap (see
  [which transient is the clap](#which-transient-is-the-clap)). If layout A is loaded you can pick
  "clap at speaker *N*" and the position autofills from the surveyed geometry: stand at a speaker, clap,
  move on. Measured: see [the tracked clicker](#clap-positions-from-a-tracked-clicker) below.

Then **Solve** → **Install** → **Save**. Every refused clap is counted, and the last reason shows under
the count. `--tests zylia` drives the whole flow on synthetic claps, typed and tracked.

**Spread is the trap.** Claps in a horizontal ring around the array are *coplanar*: the normal matrix
goes singular in the vertical and the capsules' **heights** are unrecoverable. A solver that shrugged
would hand back a flattened array and a confident wrong answer, so this one refuses. `spread` is 1 for
isotropic directions, 0 for coplanar, and below 0.05 the solver declines and tells you why. **Clap high
and low, not just around.**

**Reading the result.** `residual` is the number that says whether to believe it: what the recovered
geometry *fails* to explain, in microseconds. Sub-microsecond is clean. A few microseconds already means
something: in simulation, claps 12 cm from where the tool thought they were raised it to only 3 µs while
the capsules moved 3 mm. Tens of microseconds means bad claps, a wrong array center, or a clap that
wasn't where you said it was. `radius` should land near
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

#### Clap positions from a tracked clicker

A typed clap position is the weakest number in the survey. A centimeter of it is about 0.2° at 2.5 m,
and the residual can't tell a mistyped position from a bad capsule. So measure it: put an OptiTrack
rigid body on a hand clicker, and the tool takes each clap's position from the clicker's tip.

Set it up:

1. Make the clicker a rigid body in Motive. Note its name or streaming ID.
2. Measure the tip offset: from the body's pivot to the point where the click happens, in the body's
   axes, in meters. Or move the pivot to the tip in Motive and leave the offset at 0.
3. In *Capsule survey*, pick **tracked clicker**, type the body, Motive's IP and the tip offset, and click
   **Connect**. The readout shows the live tip and says STILL when you can click.

Then hold the clicker still for half a second, click, and move to the next spot. Spread the spots high
and low, the same as typed claps.

**Stillness, not a clock match.** A clap is instantaneous but the hand moves: 1 m/s for 10 ms is 1 cm,
the error this exists to remove. Matching the clap to one NatNet frame that closely needs the ASIO
clock, the host clock and Motive's clock agreed to a few milliseconds, and none of that chain is
verified. So the tool asks for stillness instead. Every tip position in the 300 ms window that ends
50 ms before the clap must sit within 4 mm of the window's mean, and that mean is the clap position. A
clap made while the clicker moves is refused, with the reason.

The clock pairing that is left is coarse. Each pose is stamped on the host's monotonic clock (QPC)
when it arrives. The clap's onset is stamped on the same clock where it happens: the capture callback
finds the first sample past the trigger level in its ASIO block and dates it from that block's buffer
switch, less the driver's input latency. The switch time is the driver's own `systemTime` when that
runs on QPC. The ASIO SDK says `systemTime` comes from `timeGetTime`, which ticks at 1 to 15.6 ms and
ran 27 ms off QPC on the development box, so the callback checks which clock the driver's stamps follow.
Unless it is QPC, the callback uses its own QPC read at entry instead. Either way the stamp is late by
the callback's dispatch delay plus any latency the driver does not report, a few milliseconds, and the
50 ms guard covers that. So the window ends before the click however late the tab notices the
snapshot. What is left is the window's spread plus the drift in the 50 ms before the click: about 5 mm
at worst, 0.1° at 2.5 m.

Without a stamp, the tab falls back to the old estimate: the frame that saw the snapshot, minus the
capture's post-roll (3584 samples, 75 ms at 48 kHz). That is late by one ASIO block plus one UI frame on
a good day, and by any UI stall on a bad one. Past about 20 ms of stall the window reaches the click,
and the hand's next move gets a still clap refused. The last-clap line says which onset it used,
**onset stamped** or **onset ESTIMATED**; hover it for the source and for how late the tab saw the
claps. With the ZM-1 open, the trigger line shows the ASIO block, the input latency and which clock
`systemTime` follows.

**Two bodies, one stream.** The stand and the clicker are two rigid bodies on one NatNet stream. The
Placement panel and the clicker each listen on the data port with their own socket. Both set
`SO_REUSEADDR` and join the multicast group, so each gets every frame. That needs multicast: a unicast
stream reaches one socket only.

**What a wrong tip costs.** In simulation, a tip offset left at 0 on a clicker whose tip is 11.6 cm out
put every clap 11.6 cm off, in a different direction each time, because the clicker is held at a
different angle at every spot. The survey still solved: residual 3.2 µs against 0.035 µs, capsules
off by up to 3.2 mm against 0.04 mm. The residual flags it, but weakly, so treat a few microseconds as a
warning.

**Rehearse without Motive.** Tick *simulate* beside the clicker's **Connect**, with *simulate claps*
on. A scripted clicker walks to 10 spots high and low around the array, holds still at each, and
clicks. Once on the way it clicks while waving, and that clap must be refused. *coplanar ring* puts 8
clicks in a ring at the array's height instead. The script keeps its own clock and steps it once per
frame, so a busy machine slows the script down rather than changing what it does. Each scripted clap
carries its onset stamp on that clock, and the tab notices it 80 ms after the capture would have
published it, as a UI stall does, so every simulated run goes through the stamp. `--tests clicker` runs
seven checks: with the stand
tracked, every banked clap is the clicker's true tip against the stand's measured center (not either
typed field) and the survey recovers the capsule table; a tip left at 0 makes the residual and the
geometry measurably worse; the ring is refused; an interferer is refused by the arm window or the
direction check; one that slips past both is flagged by leave-one-out and dropped; with the 80 ms
stall, every still click banks at its true tip when judged at its stamp; and judged at the old
estimate instead, every click that is followed by a move is refused. `--tests onset_stamp_math` feeds
the callback's arithmetic and its clock check the three bases a driver can stamp on.

Nothing here has met live Motive or a real clap. The 4 mm stillness limit is a guess at a held hand: if
real claps get refused, that is the number to revisit.

#### Which transient is the clap?

The capture trips on any transient. Stillness proves the clicker was still. It does not prove the
sound came from it: a clap across the room, a door or a dropped tool while you hold the clicker still
would bank at the clicker's tip. The survey's residual rises for an interferer from far away (23 µs
against 0.04 µs in simulation, with the capsules 11 mm off), but it cannot say which clap, and a clap
a few degrees off barely moves it, because the fit bends the geometry to explain it. So three layers
decide.

**Arm, then accept.** A clap banks only inside an armed window, and only the first transient in it.
One outside the window, or a second one inside it, is refused, and the tab says why.

- Tracked clicker: the window opens when the clicker reads still at a spot and closes when it moves,
  or after 2 s. After a window that took a clap, move the clicker at least 5 cm before the next one
  opens, so a hand that relaxes after its click cannot re-arm the same spot.
- Typed: click **Arm for the next clap**, then clap within 2 s. Set a **lead-in** to get time to walk
  to the spot: transients before the window opens, your footsteps included, are refused.

2 s covers a hold-then-click rhythm with a margin. Every second of it is exposure: a stray transient
that lands in the window first is banked. The window is judged at the clap's onset, not at its
detection, so moving right after you click does not close the window under the click.

**Direction, from the sixth clap.** Six claps is what Solve needs. From there the tab solves a
provisional survey of what is banked, leaving out claps that leave-one-out flags (below), and every
new clap's direction against that table must lie within 10° of the direction of its position from the
array center. Disagreement is refused, and the window stays open, so the real click after an
interferer still banks. 10° is about 5 times the worst error a genuine clap carries: a typed position
5 cm off at 1.5 m is 2°, the clicker's 5 mm is 0.15°, and the DOA's near-field bias stays under 1°. A
wrong tip offset (3° at 2 m) passes, and so does an interferer within 10° of the clicker (35 cm at
2 m). Before the sixth clap, only the arm protects.

**Leave-one-out, on Solve.** `zylia_survey_loo` solves the survey once without each clap and scores
that clap against the geometry the rest give. It flags a clap that misfits the rest by more than 5
times the rest's own residual, and by more than 3 µs (about 2° of direction). The tab offers to drop
it and solves again. Read the limits:

- It finds one clap that disagrees with a consistent rest. An error every clap shares, like a wrong
  tip offset or a wrong center, flags nothing.
- Two bad claps can hide each other. Drop the worst one, then look again.
- A clap that is the only thing lifting the set off a plane cannot be left out: the rest is
  coplanar. The tab lists it as unchecked and never flags it.

The capture holds off for about 375 ms after each trigger (an 85 ms snapshot, then 300 ms). A second
transient sooner than that is never published, so the window cannot refuse it. Two transients inside
one snapshot are timed as one, and the direction check or leave-one-out has to catch the result.

Rehearse it: tick *interferers* beside *coplanar ring* in simulate. Two sounds from 60° away fire
while the clicker holds still: one right after the second click, one timed like the eighth click.
`--tests zylia` checks that both are refused and the geometry lands within 0.2 mm of the capsule
table the claps came from, as a clean run's does. It also checks
one timed like the third click, before any provisional survey: it banks, leave-one-out flags it, and
dropping it restores the geometry. `typed_arm` drives the typed window.

None of this has met a real clap or a real interferer. The 2 s window, the 10° tolerance and the
leave-one-out thresholds are set from the simulation and the error budget above, not from the rig.

### The speaker-sweep capsule survey (`--capsule-survey`)

The clap survey needs a clap position you know, from a tape or a tracked clicker. Once the speakers'
positions are known, the speakers are better sources: a sweep instead of a clap, a position known to a
few millimeters, and every speaker from one placement of the ZM-1.

```
bwa_calibrate --layout cave_layout.json --zylia --capsule-survey zm1.json --input <first> \
              --mic 0 1.448 0 --track zm1 --natnet-server <ip> --mount-offset ring
```

The tool sweeps every speaker in `--speakers` (default all of them) once. It times the 19 capsules by
cross-correlating their impulse responses, the path every ZM-1 sweep takes, and hands `zylia_survey`
those arrivals with each speaker's `position`, relative to the array center, as the known source. It
then solves three times:

1. **The table, at the given center**: the tracked center, or `--mic` untracked.
2. **The array center, acoustically.** With that table installed, each speaker's center arrival
   (`zylia_center_arrival`) is its distance from the center plus the system latency. Trilaterate the
   center from the speaker positions, the way `--localize` trilaterates a speaker from the mic rows
   (`calib_trilaterate`, the latency as a fourth unknown, 5 speakers or more).
3. **The table again**, at the acoustic center.

**The positions are the input, so trust them.** The survey takes each speaker's as-built `position`.
After `--localize --zylia` those are acoustic measurements. A position from the plan, or a guess, makes
the survey wrong in proportion: 1 cm off at 2 m tilts that speaker's direction by 0.3°, and the fitted
table turns with it. The run counts the chosen speakers that carry no `plan_position`, because no survey
ever wrote their position. `--speakers 0,3,5-9` restricts the set to the ones you trust.

**What it refuses**, before any sweep: fewer than 4 speakers (the solver's floor; it asks for 6 or more,
and the acoustic center needs 5), a speaker within 0.2 m of the center, and a set whose directions from
the center have a spread under 0.05. A ring of speakers at the array's height is that last case: the
capsules' heights are then unconstrained.

**The report.**

- `residual`, `radius` and `spread`, read the way the clap survey's are.
- Per speaker, its distance from the acoustic center against its range, in mm, and that residual
  against the median one: the range check, below.
- The acoustic center against the given one, and the system latency, which is the `--latency` value
  for live aiming and the `--zylia` position survey.
- **Pass `--latency` from the localize run.** The survey then solves only the center, three unknowns.
  Without it, it solves the center and the latency together, and a dome around the listening point
  pins the latency badly: every speaker sits at about one distance, all on one side. The linear
  trilateration loses the latency almost completely there (2 m out on 3.5 mm of position error in
  `calib_test`), so the survey follows it with a least-squares refine, which brings it back to about
  the range error times the geometry's **dilution**. The run prints the dilution: 1 when the speakers
  surround the array, and the dome in `calib_test` reads 2.75. Over 3 the run prints the latency with a
  WARNING that it is poorly determined, does not offer it as the `--latency` value, and tells you to
  pass the localize run's. The localize run's latency is well pinned, because its
  mic rows move; the Session tab passes its median.

**Leave-one-out: which speaker does not fit.** One speaker whose layout position is off, or whose
capture was contaminated, bends the whole table, and the residual hides most of it: the fit spends a
little geometry to explain it. So after the solve the run scores each speaker against the table the
OTHER speakers solve to (`zylia_survey_loo`, the clap survey's check), one line per speaker:

```
  spk 14: held out   0.52 us, the rest 0.83 us
  spk 15: held out   4.34 us, the rest 0.18 us   FLAGGED: it does not fit the rest
  spk  3: unchecked: without it the rest have a spread of 0.031, under the 0.05 floor
```

- `held out` is that speaker's misfit against the rest's table. `the rest` is the rest's own residual.
- `FLAGGED` means held out by more than 3 µs and by more than 5 times the rest's residual. 3 µs is
  about 2° of direction on the ZM-1.
- `unchecked` means the rest alone are coplanar, so nothing can check that speaker. It is never
  flagged.
- With 4 speakers, none can be left out, and the run says that no speaker was checked.

**The range check: a distance error.** Leave-one-out sees a position error ACROSS the line of sight,
because it moves the speaker's direction. An error ALONG the line barely moves any direction: in
simulation a speaker 40 mm out radially held out at 0.02 µs, against the 3 µs floor. It does move the
speaker's range, though, and the range feeds the acoustic center's trilateration: unrefused, that
speaker put the center 4.8 mm off. So the run also scores each speaker's range residual from solve 2
(its distance from the acoustic center, plus the latency, minus its center arrival) against the
MEDIAN residual:

```
  spk 14: 2.082 m,  -4.95 mm,  -3.51 from the median
  spk 15: 1.423 m, +33.30 mm, +34.74 from the median   RANGE FLAGGED: its position or its arrival is off along the line
capsule survey: the median range residual is -1.44 mm: an error every speaker shares (the latency), which
                flags nobody
```

- **Against the median, not zero.** An error every speaker shares, a latency that is off (given, or
  poorly pinned), moves the median and flags nobody. With `--latency` 20 mm off, every residual read
  about +20 mm and nothing was flagged; the run warns that `--latency` is off by that much instead.
- **Flagged over 10 mm** (`CSURVEY_RANGE_FLAG_MM`, about 29 µs). A correct run's residuals sit within
  0.2 mm of the median in simulation (the center arrival's plane-wave model reads late by about
  R²/(3dc), 0.3 to 0.6 mm at most). The threshold is not 5 mm because a bad speaker bends the
  least-squares center, and the good speakers' residuals bend with it: beside a 35 mm one they reached
  4.6 mm off the median. 10 mm keeps them clear behind an outlier up to about 75 mm on the 26-speaker
  grid. A 10 mm range error moves the acoustic center about 1.4 mm, past the 1 mm a clean center lands
  within.
- A box whose **Dante latency** differs from the rest lands here too: it delays every capsule alike,
  which no direction shows, and 0.1 ms is 34 mm of range.
- The check needs two speakers more than the unknowns (6 with the latency solved, 5 with `--latency`),
  or it cannot say WHICH range is off. It still holds with the latency poorly pinned: on ten speakers
  of the dome's upper side (dilution 3.3), the clean run's residuals sat within 0.01 mm of the median,
  and a speaker 40 mm out radially read +25.6 mm. Fewer speakers bend more: a good one read 12 mm
  beside it and was flagged too, and cleared once the bad one was dropped.

**A flag of either kind refuses the survey.** The run writes nothing and exits 6. It lists every
flagged speaker with the check that flagged it, names the worst one (the one furthest past its own
threshold, held out over 3 µs or range over 10 mm), and prints the `--speakers` list without it. Check
that speaker's position (re-run `--localize --zylia`) and its Dante latency, or re-run with that list.

**`--drop-outliers`** drops the worst flagged speaker instead, runs all three solves again without it
(so its range is out of the acoustic center's trilateration too), runs both checks again and writes
the survey. The run then names the speaker it left out. It refuses with exit 6 when the rest fall
under 4 speakers or under the spread floor. Two rules:

- **It drops the worst one only, never every flagged one.** A bad speaker bends the table the others
  are scored against, so a good speaker can flag beside it and clear once it is gone.
- **It drops one speaker at most.** A speaker flagged in the second check stops the run with exit 6.
  Both checks assume one bad speaker against a consistent rest. A second one means more than one
  position is off, so the rest is no longer known to be good, and since every position comes from one
  layout, the layout itself is the suspect. Choose the set yourself with `--speakers`.

Read the limits. An error every speaker shares, like a wrong frame or a wrong center, flags nothing.
Two bad speakers can hide each other from leave-one-out: in simulation, a second speaker 80 mm off read
4.4 µs held out but was not flagged while the first was still in the set, because the rest's residual
was 0.9 µs. The range check saw both (about 36 and 38 mm), and the run stopped after the first drop. And a small error passes: the floor is about 2° of direction
from the array, 5 cm across at 1.5 m. One speaker 80 mm off at 1.39 m (2.9° across, 35 mm in
range) read 4.3 µs, and the good speakers beside it under 0.6 µs.

**Tracked, it writes a BODY-FRAME survey.** The table is rotated into the stand's axes at the take's
orientation, `caps_body = Rᵀ caps_room`, the form `--survey` and `bwa_validate --track` read. Its mount
offset is the acoustic one: `offset = Rᵀ (center_acoustic − p)`, with `p` the stand's pivot. The run
prints it beside the offset it was given and how far apart they are, and warns over 5 mm: either the
offset you gave is wrong, or the speaker positions are.

**Why the acoustic center, and not the survey's own.** `zylia_survey` subtracts each speaker's mean
arrival across the capsules, so it cannot see a translation of the whole capsule cloud. It pins the
origin by re-centering on the fitted sphere, and that puts the sphere's center exactly at the point the
source positions were given from. Read as a measured center, it hands back the tracked center to the
bit, and the offset you started with. The center arrivals carry the translation the means removed.

**What the acoustic offset is measured against: the speaker positions.** If they came from
`--localize --zylia --track` with the same mount offset, and the stand kept its orientation through the
rows, an offset error moved every row's center the same way. It moved the solved speakers by the same
amount, and the acoustic center agrees with the tracked one whatever the offset is. So the comparison
checks the offset only as far as the positions do not share it. Turn the stand to a different yaw at
each `--localize` row and an offset error moves each row a different way, so the speakers stop sharing
it in full. For an independent check, re-run `bwa_speaker_survey` against the surveyed file: the
visible speakers' optical positions do not depend on the mount offset at all.

**Untracked, it writes a ROOM-AXES survey**, good for this mounting only, and prints the acoustic center
to pass as `--mic` to the runs that use it. A taped `--mic` only has to be close: in simulation, one
19.7 mm off came back to within 0.1 mm.

**In simulation** the captures come from a simulated PHYSICAL ZM-1, never from a table any solve reads:
the built-in table scaled to 49.5 mm and turned 40° and tilted 3° inside its mount, then turned by the
scripted stand (`calib_sim_zm1_room`, with its own rotation code). On the 26-speaker example in the
simulated room, tracked and told an offset 18.1 mm from the stand's true one (`--track-sim-offset`):
residual 0.04 µs, radius 49.50 mm, every capsule within 0.03 mm of the truth after a reload and a
re-aim, and the acoustic offset within 0.1 mm of the true one. Untracked and anechoic, every capsule
within 0.01 mm. Leave-one-out held every speaker under 0.1 µs on both. The `calibrate_capsule_survey`
ctest pins 0.1 mm and 1 mm, a refusal by name for one speaker moved 80 mm in the layout, the clean
table and center after `--drop-outliers` drops it, the stop on a second one, the refusal of a speaker
40 mm out radially by its range alone, a `--latency` 20 mm off that flags nobody, and the range check on
the dome's upper side with the latency poorly pinned.

**What simulation cannot show: the rigid sphere.** Every simulated capsule is a free-field omni. The
real ZM-1's capsules sit on a rigid sphere, and sound bends around it: a capsule in the shadow hears a
diffracted arrival, later than a free-field one, by an amount that depends on frequency and direction.
At low frequencies the arrival differences across a rigid sphere are about 1.5 times the free-field
ones, at high frequencies close to 1 times. So the survey measures an EFFECTIVE geometry, the capsule
positions that best explain the arrivals the cross-correlation reads. Expect a radius above 49 mm and a
residual of several microseconds rather than a fraction of one, because a shadowed capsule's effective
position depends on where the sound came from, and one table cannot fit every direction. Whether that
table serves the direction modes better than the built-in one is the open question for the rig.
Compare the residual and the radius against a clap survey of the same mounting.

Unverified on hardware: all of it.

### Bootstrapping the ZM-1 with no tape measure

Every direction measurement rests on the capsule table: the `--zylia` position survey, live aiming's
position readout, and `bwa_validate`. The clap survey gets the table from claps at measured positions.
With the ZM-1 on Dante and its stand tracked, you can bootstrap it from the speakers instead, in this
order. No step needs what a later step produces.

1. **Check Motive's frame** with `bwa_speaker_survey` (see "Optical speaker check" below). Every
   tracked center below is a Motive coordinate, so Motive's frame has to BE the layout's room frame.
   Needs: Motive, rigid bodies on the camera-visible speakers, and the plan positions from Stage 1. A
   mirror or a rotated ground plane shows against a taped plan. Needs no audio and no ZM-1.
2. **Locate the speakers**: `bwa_calibrate --localize rows.txt --zylia --track <body> ...`. Needs: the
   tracked center at each row, so the stand's mount offset (`ring` or `x,y,z`), and nothing else. Each
   range is the array center's arrival, which does not change when the array turns, so it needs no
   capsule table and no orientation. Gives: every speaker's as-built position, in Motive's frame.
3. **Survey the capsules**: `bwa_calibrate --capsule-survey zm1.json --zylia --track <body> --mic <x y z>
   ...`. Needs: the positions from step 2, and the tracked center. Gives: the body-frame table, which is
   the channel order and the array's orientation in its mount, the acoustic mount offset, and the system
   latency.
4. **The direction modes**, with `--survey zm1.json`: the `--zylia` position survey, live aiming, and
   `bwa_validate --track`. Needs: the body-frame survey from step 3.

Two cautions. Step 2's center arrival still depends on the channel order, by up to 15 µs (5 mm), so
check the order with `bwa_zylia_probe` before step 2. And step 3's acoustic offset is measured against
step 2's positions, which were solved with the same offset: read "What the acoustic offset is measured
against" above, and turn the stand between the `--localize` rows.

The clap survey in calib_view's Zylia tab stays the alternative. It needs no sweep, so it works before
the ZM-1 is on Dante, and it needs no speaker positions.

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

`--baffle-offset-m x` says the layout's point for a speaker sits x m behind the baffle. Layout
positions are acoustic centers ([layout-schema.md](./layout-schema.md#positions-are-acoustic-centers)),
and the markers sit on the baffle, so the tool moves each optical point back by x along the box's
aim. You do not have to know x: on measured positions the tool reads it (see "Measure the baffle
depth").

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

The exit code says nothing about the frame unless you ask: a report-only run exits 0 whatever the
frame does. `--require-frame` exits 3 unless the fit agrees, that is a right-handed fit of 3 or more
speakers within 0.5 degrees and 10 mm. The summary line then ends `frame check PASS` or
`frame check FAIL` with the reason. The Session tab passes it, because the tab reads only the exit
code.

### Measure the baffle depth

Once `--localize` has written MEASURED acoustic positions, each visible speaker's optical point sits
in front of its acoustic center by the baffle-to-acoustic-center depth, along the box's aim. Run the
tool on that file and it reads the depth:

```
bwa_speaker_survey as_built.json --server 192.168.1.10
```

The `baffle depth` block prints, per speaker, `depth` (the difference along the box's optical aim,
positive when the acoustic center is behind the baffle) and `across the aim` (what is left across
it). Then the median over the speakers, their spread, the worst across-aim residual, and
`suggested --baffle-offset-m <x>`. With `--baffle-offset-m` given, the per-speaker depths are the
RESIDUAL beyond it, the block says so, and the suggested value is the sum. The summary line ends
`baffle depth +60.0 mm over 4` (or `baffle depth residual ...`).

The rules:

- **Measured speakers only.** A speaker with no `plan_position`, or one equal to its `position`, was
  never measured, so its position is the installer's tape, not an acoustic center. The tool skips
  it and says so. A speaker with no optical aim is skipped too.
- **After the frame.** An unaligned frame makes the difference meaningless, so the frame check must
  find a right-handed fit first. Then the tool refits the frame over the measured speakers with
  the depth as a fourth unknown: `layout = R (optical - depth aim) + t`. A plain fit cannot be
  read afterward, because a rigid fit of points all pushed along their aims absorbs much of the
  push into its translation: on four dome subsets a 60 mm depth read back as anything from 14 to
  61 mm, and the lopsided subsets failed the 10 mm frame check on the depth alone. The refit's
  rotation and translation must agree with the room frame (0.5 degrees, 10 mm), or the depth is
  `not measured`. When the plain check failed and the refit agrees, the block says to rerun with
  the suggested value.
- **3 or more measured speakers that face different ways.** The depth is separable from a
  translation only through the spread of the aims. The block prints how far the depth moves per
  millimeter of position error; over 2 mm per mm it warns (`SURVEY_DEPTH_MIN_LEVERAGE`).
- **Agreement.** The boxes are one model, so their depths should match. A speaker more than 20 mm
  off the median depth, or 20 mm across its aim, reads `DISAGREES`: the two surveys disagree
  there (`SURVEY_DEPTH_AGREE_M`, provisional; the acoustic survey's simulated worst is about 12 mm).

The order on the rig:

1. Run the frame check on the plan, with a guessed `--baffle-offset-m` or 0.
2. After `--localize`, run it again on `as_built.json` to read the measured depth.
3. Set `--baffle-offset-m` (and the Session tab's **baffle offset**) to the suggested value, and run
   the frame check again.

Read the depth from `--localize`'s output, never from a file this tool wrote positions into: an
optical `--write --fields position` records a plan, so the tool would read its own optical points
as measured, and read a depth of about 0.

### Write the aims

The tool only reports by default. `--write out.json` copies the measured aims of the matched
speakers into a copy of the layout. Every other field is kept. `--fields position` or
`--fields aim,position` writes positions too. That replaces the acoustic survey's position with
the optical one, so do it only when you trust the optical one more. It refuses a mirrored frame
and warns when the frame disagrees, because the values are in Motive's frame.

Measured optical positions are what `bwa_calibrate --ref-speakers` wants: the system latency from
those boxes, with no tape (see "The latency from measured speakers"). Set `--baffle-offset-m` first:
the latency inherits any error in it, about 3 µs per millimeter.

### Rehearse without Motive

```
bwa_speaker_survey cave_layout.json --simulate --sim-yaw 2 --sim-offset 0.05,0,0 --sim-aim-error 3
```

This builds Motive's packets from the layout and runs them through the same parsers. The knobs:
`--sim-speakers` (which speakers are visible), `--sim-aim-error` (on the first), `--sim-yaw` and
`--sim-offset` (Motive's frame against the room's), `--sim-mirror`, `--sim-noise-mm` (marker
placement), `--sim-jitter-mm` (per frame), `--sim-natnet` and `--sim-depth-m` (the true baffle depth
the bodies are built at; without it, `--baffle-offset-m`). It checks its answer against what it
injected, the baffle depth included, and prints `simulate PASS` or `FAIL`. The
`speaker_survey_depth*` ctests rehearse the depth on `test/survey_depth_layout.json`, an as-built
file with four measured speakers and two that are not: a 60 mm depth comes back within 1.45 mm
through a small frame error, and a 2 degree frame refuses it.

Unverified against a live Motive: the socket path, and the assumption that Motive sends marker
offsets in the body's own frame. For a body created in place on a fixed speaker the two readings
agree.

## Placing the ZM-1 with the tracker (`--track`)

On the rig the ZM-1 stands on a rigid stand with OptiTrack markers, and Motive's frame is the room
frame. Without the tracker, every mic position you give `bwa_calibrate` is a number you typed.
`--track <body>` makes it a measurement. Before each placement's captures the tool shows where the
array's center is against where it should be, waits until it is there and still, and then
measures from where the center actually is. After every capture it checks that the stand did not
move, and in the modes that read room directions, that it did not turn.

```
bwa_calibrate --layout cave_layout.json --zylia --trims --input 0 --track zm1 --natnet-server 10.0.0.5 --mount-offset ring
```

### What each job needs

| job | target | tolerance | why |
| --- | --- | --- | --- |
| trims, `--verify` | `--mic`, else the listening point | 10 mm | 1 cm is up to 29 µs of arrival error, and verify flags at 100 µs. The engine assumes the trims align at the listening point. |
| live aiming | `--mic`, else the listening point | 10 mm | the position readout is relative to the center |
| `--localize` | each row of the file | 100 mm | trilateration needs each position KNOWN, not hit |
| `--room-eq-grid --mic` | `--mic` (required) | 10 mm | the measured position becomes the grid key |
| `--room-eq-grid rows.txt` | each row | 100 mm | the measured position becomes the grid key, so a row only has to be near |
| `--zylia` position survey | `--mic` (required) | 10 mm | the center is where every direction starts |
| `--capsule-survey` | `--mic` (required) | 10 mm | the table is read at the take's orientation, and the center starts the solve |

For `--localize` and the `--room-eq-grid` rows the tolerance is loose, 100 mm (or
`--place-tol-mm` if you set it wider), and the tool records the MEASURED position. That is what the solve needs: a row you missed by 3 cm costs
nothing once it is known. The tolerance is not zero, because the stand is still standing at the
previous row when the next wait starts, and a gate that accepted stillness anywhere would record
that row twice.

`--check`, the omni `--live` and `--aim-sheet` take no placement, and refuse `--track`.

### Set it up

- A rigid stand and a rigid coupling, no shock mount, markers on the stand and not on the sphere
  (the ZM-1 stand prep in [hardware-validation.md](./hardware-validation.md#prepare-the-zm-1-stand)).
- A rigid body for the stand in Motive. Note its streaming ID and its name. Tracking by name needs
  `--natnet-server <ip>`, because the name resolves through Motive's model definitions.
- The mount offset: from the rigid body's pivot to the array's acoustic center, in the body's own
  axes. The tool computes `center = p + R(q) · offset` from every pose. Give it one of:
  - **a body-frame survey** (`--survey`), which carries the probed offset;
  - **`--mount-offset x,y,z`** in meters. With no body-frame survey, this center depends on how
    Motive orients the rigid body. It does not depend on which way the ZM-1 faces;
  - **`--mount-offset ring`**: fit the markers (below);
  - **nothing**: an offset of 0. Move the rigid body's pivot to the array center in Motive first.

The tool prints which source it used.

### What needs a survey

Only a mode that turns capsule arrival DIFFERENCES into a room direction needs the array's
orientation: the `--zylia` position survey and the live-aim position readout. Tracked, those two
need a BODY-FRAME survey, and the tool re-aims its capsule table from each pose. Without one the
position survey is refused, and live aiming runs its tilt meter with the position readout off.
These are the DIRECTION modes, and they are the ones whose gate and bump check also watch the
orientation (below).

Everything else only needs the center: the trims, `--verify`, the live tilt meter, `--localize` and
`--room-eq-grid`. The pressure proxy's power mean does not change when the array rotates, and
neither does `zylia_center_arrival`: its direction and the capsule centroid are both in the
array's own frame, so their dot product is the same at any orientation. These modes take no
survey, a body-frame one, or a room-axes one, which is then installed for its channel order and
geometry only and never re-aimed.

`--capsule-survey` is the one that MAKES the body-frame survey, so it takes none: it needs the offset
alone (`ring` or `x,y,z`) and measures the rest. It reads room directions at the take's orientation, so
it is a direction mode for the gate and the bump check. See "The speaker-sweep capsule survey" above.

### The ring offset (`--mount-offset ring`)

If the markers sit in a ring around the housing's equator, Motive's default pivot is their
CENTROID, and the centroid lies on the ring's axis only when the markers are evenly spaced. You
space them unevenly so the body has a yaw: 0, 90, 180 and 225 degrees on a 60 mm ring puts the
centroid 11.5 mm off the axis. `ring` reads the body's marker offsets from Motive's model
definition, fits a plane through them and a circle in that plane, and takes the circle's center as
the offset (`place_ring_fit`). It needs `--natnet-server`, and at least 3 markers. It refuses markers on
one line, markers more than 3 mm RMS off one plane, and a circle residual over 3 mm RMS. The report
gives the centroid-to-center distance, the radius and both residuals.

It assumes the ring sits at the height of the array center, the capsule sphere's equator. The fit
cannot see a ring that sits higher or lower on the housing, and that vertical error passes
straight into the center.

### The readout and the gate

```
  center (+0.003 +1.450 +0.004)  target (+0.000 +1.448 +0.000)  dx   +3.0 dy   +2.0 dz   +4.0 mm  |d|   5.4 mm  HOLD settling
```

One line, updated in place: the measured center, the target, the delta per axis and in total, and
`HOLD` or `OK`. `HOLD` says why: `moving`, `off target`, `settling`, or no live pose. The gate
(`placement.h`) opens when all of these hold:

- **Still**: every center of the last 0.5 s sits within a spread limit of their mean. The limit is
  a quarter of the tolerance, clamped to 0.5 to 2 mm: Motive's jitter on a rigid body is a few
  tenths of a millimeter, and 2 mm is well under the bump limit.
- **Within tolerance**: the window's mean is within `--place-tol-mm` of the target (default 10).
  The test is on the total distance, not per axis.
- **Held**: both have stayed true for 1 s. A stand reads still the moment the hand pauses; it has
  settled only once it stays put after the hand is gone.

In a direction mode, still also means the orientation: every pose of the window within 0.3° of their
mean. The live line then adds the mount's yaw, and `HOLD turning` says the orientation alone holds the
gate. The 0.3° is Motive's floor, not a choice: a small rigid body's orientation jitters by a few
hundredths of a degree RMS, with single-frame peaks of 0.1° to 0.2°. The center-only modes have no
orientation term.

```
  center (+0.003 +1.498 +0.004)  target (+0.000 +1.500 +0.000)  dx   +3.1 dy   -2.0 dz   +4.0 mm  |d|   5.4 mm  yaw  -30.0  HOLD turning
```

Then the window's mean IS the mic position. The tool prints it with the mount's yaw (bearing of the
body's +z, clockwise from above, 0 = room-ahead) and tilt, re-aims a body-frame table from the
window's mean orientation, and starts.
A pose only counts while NatNet reports the body LIVE: the last published pose stays readable
forever, so an occluded stand would otherwise hand back an old one.

On the rig a key takes the current reading anyway, with a warning. With no pose at all it aborts.
`--place-timeout <s>` (default 300) aborts a wait that never ends.

### The bump check

After every capture the tool reads the center again and compares it with the one the run took. A
move past half the tolerance stops the run with exit 4 and writes nothing. Once the mic moves, the
run's speakers were measured from two points, and a trim set like that is wrong in a way no later
pass can see. The report prints the largest move over the run. Re-place the ZM-1 and run again.

The trims, `--verify`, `--localize`, `--room-eq-grid`, the `--zylia` survey, `--capsule-survey` and
live aiming all check.

**The turn.** A stand turned about the array center moves no center, so the move check cannot see
it. With a marker ring around the ZM-1's equator the center sits close to the body's axis, so that
turn is an easy one to make. The direction modes read it anyway: 1° of turn is 1° of error in every
direction measured after it, and at 2 to 3 m that is 35 to 50 mm of speaker position. So the
`--zylia` survey, `--capsule-survey` and the live-aim position readout also compare each pose's
orientation with the one the run took (the gate window's mean), and stop with exit 4 on a turn past their limit:

```
calibrate: BUMP: the ZM-1 turned 2.03 deg during the position survey, after speaker 4 (limit 0.30 deg), its
           center 0.1 mm from where it was taken. ...
```

The limit is the move limit's own budget, spent as a turn. Half the tolerance is what a placement may
lose to a move; a result at range r loses θ·r to a turn of θ; so the matching limit is
atan(tolerance / 2 / r) (`place_turn_limit_deg`):

| mode | range | budget | limit |
| --- | --- | --- | --- |
| `--zylia` survey | the farthest speaker, 2 to 3 m | 0.10° to 0.14° | 0.3° (the floor) |
| `--capsule-survey` | the farthest swept speaker | about the same | 0.3° (the floor) |
| live position readout | the one speaker | about the same | 0.3° (the floor) |
| `bwa_validate` | the 1.4 m source radius, 20 mm | 0.41° | 0.41° |

Every limit floors at 0.3°, because every check compares ONE pose with a mean, and one Motive frame
can be off by 0.2°. For the survey that floor costs something: 0.3° is 13 mm at 2.5 m, about the
whole 10 mm tolerance rather than half of it. That is what one frame can resolve. The limits are
unverified against live Motive: if a still stand trips one on the rig, the floor is the number to
revisit (`PLACE_TURN_MIN_DEG` in `placement.h`).

The center-only modes keep the center-only check: the trims, `--verify`, the tilt meter, `--localize`
and `--room-eq-grid`. Their pressure proxy does not rotate, and a turn about any point other than the
center moves the center, which the move check already sees. Their report still shows the largest turn,
marked unchecked.

### In `bwa_calib_view`: the Placement panel

The Capture and Aim tabs share a **Placement (tracked ZM-1)** panel: the rigid body, the server
and the multicast group, Connect and Disconnect, an optional survey with an offset field or `ring`
beside it, a tolerance slider, and the target, which follows the tab's layout's listening point
until you edit it. A body-frame survey that carries its own mount offset, which is what
`--capsule-survey --track` writes, overrides the offset field and `ring`. While connected it shows the distance in large type, green once the gate is
open, the move in room words and as an arrow seen from above (screen up is the front, screen right
is room-right), the per-axis delta and the mount's yaw. The words come from the function live
aiming prints (see "The move, in room words" below), with a 1 mm dead band. With a body-frame survey it also shows the
orientation's spread over the gate window, and says when only the orientation holds the Aim tab's
position readout.

A run started while tracking is live waits for the gate, takes the measured center instead of the
typed field, shows the delta it used, and stops on a bump with the move and the capture it
happened after. An Aim run with its position readout on is a direction run: it also waits for the
orientation to hold, shows how far the stand has turned since the take, and stops on a turn past its
limit. A Capture run reads only the center and never stops on a turn. A Capture run with the ZM-1 takes its capsule table from the panel's survey, by
the rules above, and ignores the tab's own survey field. The Aim tab's 3D view marks the tracked center. The poller runs on its own thread,
so the window never waits on NatNet.

### Rehearse without Motive (`--track-sim`)

`--track-sim` (with `--simulate`) replaces Motive with a scripted stand: it walks in from 8.4 cm
off the target over 2 s, settles 5.4 mm off it, yawed 30 degrees and 1.5 degrees off level. The
simulated captures come from its TRUE center, which it computes with its own quaternion code, so a
tool that measured wrong, or solved at the target, is solved at a point the captures did not come
from. `--track-sim-bump N` knocks it 15 mm after the Nth capture. `--track-sim-twist N` turns it 2°
about room vertical through the array center after the Nth capture: no center moves, so only an
orientation check can see it. Its orientation wobbles by 0.03° throughout, so a check that demanded a
perfectly still pose would fail on it. With `--mount-offset ring` it serves a model definition with
the uneven 60 mm ring above. The Placement panel's **simulate**, **bump mid-run** and **twist
mid-run** boxes are the same source.

The `calibrate_track_*` ctests run all of it: a settled trim run solved at the true center and not
the target, the ring, a bump, the refusals, a body-frame survey, and a twist. The twist stops a
`--zylia` survey and the live position readout with exit 4, and the same twist runs a `--zylia
--trims` run with the same body-frame survey, and the bare tilt meter, to the end. `placement`
unit-tests the math, and `calib_view --tests placement` drives the panel, including a twist that stops
an Aim run and does not stop a trim run.

Unverified against live Motive: the whole live path, the socket, the pose timing, and the model
definition the ring reads.

## Live aiming (`--live N --zylia`, the Aim tab)

Some boxes you cannot see: they hang behind the acoustically transparent screens, or overhead.
Live aiming turns the ZM-1 into an aiming instrument for them. It sweeps ONE speaker over and
over, captures the 19 capsules, and prints a line per sweep while an installer turns the box and
someone at the calibration PC reads the numbers out.

```
bwa_calibrate --layout cave_layout.json --live 7 --zylia --survey s.json --input 0 --latency 20.6 --aim-ref 3
```

The target is each box's PLAN: its `plan_position` and `plan_aim` when the file carries them,
else its `position` and `aim`. The header line says which. With a plan it also prints how far the
as-built (the last survey's `position`) sits from the plan, in mm and in room words, and how far
the as-built aim is off the planned one. That is the work the session is there to do.

`--mic` is the array center and defaults to the layout's listening point, which is where the
ZM-1 sits on the rig (4.75 ft). The position readout is relative to that center, so it needs the
center to about 1 cm: with a tracked stand, add `--track <body>` and the tool places and measures
it (see "Placing the ZM-1 with the tracker"). `bwa_calib_view` has the same loop in its **Aim** tab, on a
worker thread, for the speakers of layout A: a speaker picker, a big "dB below peak" number
readable from a ladder, a meter with a peak-hold line, the angle estimates, the position and the
move in room words, and a 3D view with the plan's position and aim (and the as-built ones when the
file has a plan), the ZM-1, and the measured direction and position.

### What one line says

```
live: speaker 7 against its PLAN (plan_position, plan_aim), ZM-1 at (0.000 1.448 0.000), target (1.500 0.000 -0.030), 2.085 m away  [SIMULATE]
live: as-built (the layout's position and aim) is 30.0 mm from the plan (move 30 mm toward the back wall), its aim 0.8 deg
      off the plan aim
  #1   pos   +0.8  +12.2  +30.0 mm (|d|  32.4)  dir 0.86 deg  dist 2.077 m (-8.1 mm) | move 12 mm down, 30 mm toward the back wall | tilt -2.33 dB  peak -2.33  below 0.00 dB | off-axis ref 24.4 deg [22.4-26.4], file 24.7 deg [22.7-26.7] | plan 0.0 deg | true 25.0 deg
```

That run is simulated, which is what the closing `true` field says; the rig prints no truth.

- **Position**: where the box is against its plan (or its layout `position` with no plan), in
  mm per room axis, measured minus target, plus the direction error in degrees and the
  distance. Direction comes from the capsule arrival differences
  (`zylia_doa`), distance from the arrival at the array center minus the system latency
  (`zylia_live_position`). The distance is exactly as good as the latency: 20 µs of latency
  error is 6.9 mm. Pass `--latency` (a loopback in meters at c), `--ref <spk> <m>` (one taped
  distance, swept once before the loop) or `--ref-speakers <list>` (speakers whose positions are
  measured, each swept once before the loop; see "The latency from measured speakers"). Without
  one you get the direction only. `--ref-speakers` refuses the live speaker: its position is the
  one in question.
- **Tilt, peak, below**: the direct-sound high-to-mid ratio (the gated `band_direct`, 10 kHz up
  against 3 to 10 kHz, pooled over the capsules by `zylia_pressure_proxy`), the held peak (see
  "The peak hold" below), and how far this reading sits under it. These are NOT the `--check-aim`
  bands (1 to 3 kHz against 3 kHz up): on the 4410A those fall only 0.25 dB by 15 degrees, under
  what a reading is good to, so a meter on them cannot find the peak. The live pair falls 0.51 dB
  at 10 degrees and 1.00 dB at 15 (`CALIB_LIVE_*_HZ` in `calib.h`).
- **Move**: the same error as an instruction (see "The move, in room words" below).
- **Off-axis**: the estimated angle between the box's axis and the line to the ZM-1, from a
  stored reference (`ref`) and from the vendor file (`file`), each with its bracket.
- **Plan** (or **layout** with no plan): the angle the planned aim makes with the line to the
  ZM-1, the target.

Keys: `r` stores this reading as the on-axis reference, `p` resets the peak, any other key
stops. `--sweeps N` stops after N readings.

### The peak hold

The installer chases the peak, so one contaminated reading must not set it. The peak hold
(`calib_peak_update`, shared by the CLI and the Aim tab) takes a reading only when it is clean (its
arrival inside the expected window and its IR over the SNR floor: see "Sweep quality" above), and
raises the peak only once **two consecutive clean readings agree** within the meter's own tolerance,
`CALIB_LIVE_TILT_TOL_DB` (0.3 dB). The held value is the lower of the pair: a level two readings in
a row reached. A rejected reading breaks the pair.

In practice: turn the box, and **hold it still at the best spot for two readings** (about two
seconds). Until two readings agree the line says `peak --`. A reading that is not held says why:

```
  #3   ... | tilt +0.48 dB  peak -- (two clean readings that agree set it) | NOT HELD: IR peak 24.9 dB over its noise floor, under the 40 dB the sweep needs | ...
```

That one is from the `calibrate_sweep_quality` ctest: an 18 dB click 0.48 s into reading 3 lifts
its tilt by 0.49 dB, the reading is not held, and the peak stays the clean pair's. The window for
live aiming spans both the as-built position and the plan, with the 0.5 m plan margin around each,
since the box is moving from one toward the other.

### The move, in room words

The position is an error, measured minus target. The installer needs the opposite: which way to
push the box. `place_move_words` (`placement.c`) turns the error into that move on the room
frame, in the order right or left, up or down, front or back:

```
move 12 mm toward room-left, 4 mm down, 30 mm toward the front wall
```

Room-right is -x, so a box sitting 12 mm toward -x is told "toward room-left". The front wall is
room-ahead, +z. An axis under the dead band says nothing, so the line stays short: 5 mm for a box
(about what a bracket can be set to, and half the 1 cm the readout is trusted to). With every
axis under it the line reads "no move: every axis within 5 mm". The Placement panel prints the
same function for the ZM-1 stand with a 1 mm band, so the stand and the box are never described
in two conventions. `placement` unit-tests all six directions and the dead band. The
`calibrate_live_zylia` ctest reads the words off a `--sim-move` run on all three axes, and off a
layout whose plan sits 60 mm from its position.

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

`--sim-move` and `--sim-aim-steps` move and turn the AS-BUILT box: the simulated box starts at
the layout's `position` and `aim`, where the last survey put it, and the readout measures it
against the plan.

Position: over seven speakers, with and without the simulated room, the direction reads 0.01 to
0.06 degrees off the truth and the position within 1.6 mm. A 10 cm move reads back as 100.7 mm with
nothing sideways. Before the capsules were cross-correlated, the direction read up to 0.6 degrees
off (20 mm at 2 m) and the same move read 89 mm with 13 mm sideways. The rig will do worse than
the simulation by whatever the sphere's scattering and the capsule survey leave, which nothing here
has measured. So read a delta under about 1 cm as "where the layout says" until the rig shows
otherwise.

The `calibrate_live_zylia` ctest pins the 10 cm move, the 25 degree estimate with the reference,
the file's larger error behind the screen, and the peak at 0 degrees. The `aim/sim_live` test in
`calib_view --tests` turns a simulated box away and back through the real UI.

Unverified on hardware: all of it, the short sweep through the ASIO shell included. The
simulator's capsules are free-field omnis on no sphere, so the ZM-1's own scattering, and how
well a reference taken in one direction carries to another, are not in these numbers.

## The rig-day session (`bwa_calib_view`, Session tab)

The Session tab runs Stage 2 of the runbook ([hardware-validation.md](./hardware-validation.md)) as
one session. You pick a folder, give it the inputs once, and press **Run** on each step in order.
Each step runs the tested tool, writes a new file in the folder, and hands that file to the next
step. The tab records what each step read, what it wrote and whether it passed, keeps one log, and
reopens where you left off.

Unverified on hardware. Every step has run only in simulate.

### The folder

| File | Written by | What it is |
| --- | --- | --- |
| `session.json` | the tab | the inputs and every step's record |
| `session.log` | the tab | every command, every line the tools printed, every result, appended |
| `frame_check.csv` | frame | the optical report, one row per speaker body |
| `as_built.json` | localize | the plan with the measured positions; the plan stays in `plan_position` |
| `capsules.json` | capsules | the capsule table: body frame when tracked, room axes when not |
| `trims.json` | trims | `as_built.json` plus `gain_db` and `delay_ms` |
| `grid.json` | grid | `trims.json` plus `room_eq_grid` |
| `validate.csv` | validate | every measured cell |

The plan is never written. It is an input: the frame check and the localize run read it, and the
localize run writes `as_built.json` beside it with `--out`. The tab refuses a plan that is one of
the session's own outputs. Load `grid.json` (or `trims.json`) into the engine when you are done.

### The steps

| # | Step | Runs | Reads | Writes | Passes when |
| --- | --- | --- | --- | --- | --- |
| 1 | frame | `bwa_speaker_survey <plan> --require-frame --baffle-offset-m <baffle offset> --csv frame_check.csv` | the plan, the baffle offset | `frame_check.csv` | exit 0: Motive's frame agrees within 0.5° and 10 mm |
| 2 | localize | `bwa_calibrate --layout <plan> --out as_built.json --zylia --localize <rows>` | the plan, the localize rows | `as_built.json` | exit 0 |
| 3 | capsules | `bwa_calibrate --layout as_built.json --zylia --capsule-survey capsules.json --mic <listening point>` | `as_built.json` | `capsules.json` | exit 0 |
| 4 | aim | the Aim tab, in this window | `as_built.json`, `capsules.json` | the readings you accept | every listed speaker has an accepted reading |
| 5 | trims | `bwa_calibrate --layout as_built.json --out trims.json --zylia --trims --room --survey capsules.json` | `as_built.json`, `capsules.json` | `trims.json` | exit 0 |
| 6 | verify | `bwa_calibrate --layout trims.json --zylia --verify --survey capsules.json` | `trims.json` | a status | exit 0; exit 3 means flagged speakers |
| 7 | grid | `bwa_calibrate --layout trims.json --out grid.json --zylia --room-eq-grid <rows>` | `trims.json`, the grid rows | `grid.json` | exit 0 |
| 8 | validate | `bwa_validate --layout grid.json --survey capsules.json --out validate.csv` | `grid.json` (`trims.json` when the grid was skipped), `capsules.json` | `validate.csv` | exit 0 |

Every `bwa_calibrate` step also gets the device (`--driver`, `--input`) or `--simulate`, the
tracker (`--track`, `--natnet-server`, `--natnet-multicast`, `--place-timeout`) or `--track-sim`,
and `--temp` when you set one. Localize and the capsule survey take `--mount-offset`; after them
the body-frame survey carries the offset. The full command line is in the log and in the step's
tooltip.

The frame step also takes the **baffle offset** input as `--baffle-offset-m`. It is a value, not a
file, but the record keeps it the same way: change it and the frame step reads `STALE`, which
blocks localize from running again until the frame step passes with the new value. The tab does not
measure it. After localize, run `bwa_speaker_survey as_built.json` by hand and enter its
`suggested --baffle-offset-m` (see "Measure the baffle depth" under "Optical speaker check").

Validate passes on exit 0. It measures; the numbers are yours to judge (Stage 4b).

### The background through the day

Every step that sweeps or captures also reads the room's own level: `bwa_calibrate` from each
capture before its sweep arrives (the median and the loudest, printed once a run as
`background: ... dBFS`), `bwa_validate` from each placement's silent capture. The step's record
keeps the loudest (`background_dbfs` in `session.json`), and the log gets a line after each step
that compares it with the last step that read one:

```
=== background: verify -55.0 dBFS, +15.0 dB against trims (-70.0 dBFS)  <-- RISING: something in the room got louder
```

`RISING` means more than 6 dB louder (`SES_BG_RISE_DB`, provisional). A projector fan that spun up
between the trims and the verify pass shows here before it shows as a flagged speaker. Simulate has
no noise floor, so every step reads `silent`.

### Order, skips, and stale steps

A step **needs** the file of the step that wrote it: trims needs localize's `as_built.json`, so
localize must have passed. A step also comes **after** the steps the runbook puts first: trims
comes after the capsule survey and live aiming. An after-step is satisfied by a pass or by a skip.
A step whose needs or after-steps failed, never ran, or are stale is **blocked**. Its row says
why, and **Run** refuses with that reason and starts nothing.

**Skip** marks a step skipped, and it needs a note: type why in the note field first. For example,
the frame was checked with Motive's own tools. A skipped step writes no file, so a step that needs
its file stays blocked: skip the grid and validate reads `trims.json`; skip localize and nothing
after it can run.

Every record keeps the run number of each file it read and the file's hash. Run a step again and
everything that read its old file reads **STALE**, with the reason: "localize ran again (run 9)
after this step used its as_built.json (run 2)". The record stays, marked. Staleness passes down
the chain: a step that read a stale step's file is stale too. A file edited on disk, or an input
pointed somewhere else, makes its readers stale the same way. A stale step blocks the steps after
it until you run it again. **Reset** puts a step back to pending.

The step's old file is deleted before it runs, so a failed run never leaves the last run's file
looking current.

### Why the steps are subprocesses

The tools are what the ctests pin, so the tab runs them rather than a second copy of their
measurements. It streams their output into the log and takes their exit code as the result: 0
passed, 1 an error, 2 refused before measuring, 3 the tool's own check failed (flagged speakers,
a disagreeing frame), 4 a bump, 5 the track self-check, 6 a capsule-survey speaker that does not fit
the rest (leave-one-out: check its position, or leave it out of the capsule speakers). A step that
exits 0 without writing its file fails.

Live aiming is the one step in this window. It is a person turning a box against a meter, and the
CLI's keys (`r`, `p`, stop) read a console that a subprocess does not have. **Run** on the aim step
loads `as_built.json` as layout A, sets the Aim tab's latency and, when tracked, connects the
Placement panel with `capsules.json`, then opens the Aim tab on the first listed speaker. Start,
turn the box, and press **Accept for the session** when it reads on axis. **Run** again, or the
speaker's **Aim speaker N** button, hands over the next speaker and keeps the readings you
accepted. The step passes when every listed speaker has one.

The latency the tab hands the capsule survey and live aiming is the localize run's: the median of
the per-speaker solves. The capsule survey's own latency is weak on a dome around the listening point
(see "The speaker-sweep capsule survey").

### The audio device

ASIO allows one driver per process, and the driver is usually one client at a time. While a step
runs a tool, the Capture tab's Run, the Aim tab's Start and the Zylia tab's Open ZM-1 are disabled.
A step refuses to start while any of those holds the device, and names the tab. The Capture and Aim
tabs open the device per run and close it when the run ends; the Zylia tab holds it until Close.
NatNet is no conflict: the Placement panel and a tool both listen to the multicast stream
(`SO_REUSEADDR`).

### Prompts

A tool runs with no console, so the tracked steps never wait for a key. The placement gate opens by
itself when the stand is in tolerance and still, and `--place-timeout` (300 s by default) bounds the
wait. Untracked, on the rig, `--localize`, the grid rows and `bwa_validate` stop at each placement
and ask for Enter. The tab feeds the tool's stdin: place the ZM-1, then press **Send Enter**. That
path cannot run in simulate (the tools skip the prompt there), so it is unverified.

### Simulate

**simulate** passes `--simulate` to every tool and `--track-sim` to every tracked one, and puts the
Aim tab and the Placement panel in their simulate modes. The whole session rehearses on one
machine. **simulated truth** (`bwa_calibrate --sim-truth`) is a layout with the speakers where they
really stand, so the rehearsal's as-built differs from its plan, as the rig's will. The settings
that make a rehearsal quick are inputs, not test switches: 3 validate azimuths, one validate
placement, two grid rows, the capsule speakers you list. On a 10-speaker dome the whole session
takes about 40 s.

`calib_view_session` (`bwa_calib_view --tests session`) runs three sessions through the tab:

- **full_sim**: every step in order with the truth 72 to 100 mm off the plan on five speakers.
  `as_built.json` lands within 3.5 mm of the truth. `trims.json` carries `as_built.json`'s positions,
  and its gains match the simulator's sensitivities within 0.013 dB, where a solve against the
  plan's positions would miss by 0.39 dB. Every step read its predecessor's file in its current
  run, and the plan is byte for byte what it was.
- **resume**: the session reopens equal in every field, and running localize again marks every
  step after it stale.
- **failure_blocks**: a 3-speaker capsule survey exits 2, and aim, trims, verify, grid and validate
  refuse to run, each with its reason.

## Reviewing the results (`bwa_calib_view`)

Before trusting a calibration run, LOOK at it. `bwa_calib_view` (opt-in `-DBWA_BUILD_CALIBVIEW=ON`)
loads layouts through the engine's own loader and shows the array in 3D with index labels,
gain/delay trims as bar charts, each speaker's correction-EQ magnitude, the retained IR kernels,
and, the main event, a **layout diff**. `bwa_calib_view before.json after.json` tables
Δposition / Δgain / Δdelay / eq-taps per speaker with outliers highlighted, so a swapped channel,
a bad mic placement, or a bogus `--localize` solve is one glance, not an evening.

It is the **calibration station**: one window for the rig session. The **Capture tab** runs the
trims and the verify pass itself (sweep, measure, solve, write, on a worker thread; simulate
hardware-free, ASIO full-duplex at the rig). Pick the pass and the mic:

- **trims** writes `gain_db` and `delay_ms` to the layout out. One button loads the result into
  Diff for review before you accept it. **Verify this result** selects the verify pass on the
  file just written.
- **verify** plays every speaker through the layout's own output stage and lists each speaker's
  arrival and level residual, flagged beyond ±100 µs and ±1 dB, in the tab's table. It writes
  nothing. See "Verify: the second pass" above.
- **omni** reads one input. **ZM-1** reads 19 consecutive inputs, starting at the first
  capsule's.

Every speaker goes through `calib_measure_speaker` (`calib_capture.cpp`), the capture and
measurement `bwa_calibrate` runs, so the tab and the CLI cannot measure differently. With the
ZM-1 that is the 19 deconvolutions, the cross-correlated arrivals and the pressure proxy (see
"The ZM-1 as the trim mic" above). **simulate** synthesizes the captures, the ZM-1 as 19 capsules
at their own positions, and **room** puts the simulated shoebox around the array
(`--sim-room 0.3`). The tab takes the speed of sound from the layout it measures, like the CLI.

The rules follow the CLI's:

- The directivity correction follows `--mic` (see "Speaker directivity" above): off until you
  set the mic.
- With the ZM-1, **eq** is refused, and the room report and saved IRs read the capsules' mean.
  `--room-eq` and `--room-eq-grid` stay in the CLI.
- The ZM-1's capsule table: the tab's optional survey field takes a ROOM-AXES survey and refuses
  a body-frame one, which needs a pose. While the Placement panel tracks, the panel's survey
  applies instead, by the `--track` rules: a body-frame table is re-aimed by the pose the gate
  accepted, a room-axes one is installed as it is, and no survey means the built-in table. A run
  puts the previously installed table back when it ends, so the Zylia and Aim tabs never inherit
  it.
- A trim run whose mic is more than 5 cm from the listening point says so, as the CLI does.

The **Zylia tab** (live clap-DOA, see "Bring-up" above) covers the ZM-1 bring-up, and the **Aim
tab** runs live aiming (see "Live aiming" above). Capture and Aim share the **Placement** panel for
a tracked stand (see "Placing the ZM-1 with the tracker" above). Capture and Aim never run at the
same time: they share the sweep shell and the simulator's scratch.

The **Session tab** runs the rig day in order through the tools themselves; see "The rig-day
session" above.

`bwa_calibrate` remains the headless CLI over the same code: scriptable, and the only place for
the multi-placement modes (`--localize`, the `--zylia` position survey, the `--room-eq-grid` rows,
`--check`, the omni `--live`), for `--room-eq` and `--room-eq-grid`, and for `--aim-sheet`.
`bwa_calib_view --tests [filter]` runs its imgui_test_engine suite, wired into ctest as
`calib_view`. `--tests capture` drives the ZM-1 path: a simulated trim run held against the
simulator's own truth (every gain within 0.05 dB, every delay within one sample), the same setup
with the room on run through `bwa_calibrate --zylia --trims` as well, where every written trim
must match, and a verify pass that flags exactly the one speaker whose `delay_ms` was moved
0.3 ms, with the ZM-1 and with the omni. `--tests placement` adds a tracked ZM-1 run with a
body-frame survey on the panel.

Unverified on hardware: the tab's ASIO path, the 19-input open included, like the CLI's.

## What feeds the engine

`cave_layout.json` carries per-speaker `position`, `gain_db`, `delay_ms` (consumed by `dbap.c` +
`align.c`), and optionally each speaker's `aim` plus one `directivity` model (consumed by the
tracked directivity compensation in `rt.c` + `align.c`). Model the room itself, if you want simulated reverb, in Steam Audio as geometry +
material absorption (`steam_reflect.c`) tuned to creative intent, **not** to the measured RT60.
